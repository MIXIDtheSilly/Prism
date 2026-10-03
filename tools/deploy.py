"""Put Horizon OS's Java layer onto Prism's emulator (stock Android 14 x86_64 underneath).

What goes where:
  * Horizon's framework, small app directories and configuration replace the stock ones in the
    writable system overlay (adb remount).
  * The large app directories don't fit in that overlay; they go to /data/prism and prism.rc
    bind-mounts them over the stock directories in post-fs-data, before zygote starts.
  * Prism's JNI glue goes to /system/lib64; zygote preloads libprism_jni.so.
  * Prism's Vulkan driver goes to /vendor/lib64/hw (ro.hardware.vulkan=prism).
  * Prism's thermal HAL goes to /vendor/bin/hw, with its init script and VINTF declaration.
  * Prism's tracking service goes to /system_ext/bin, with its init script; it hosts Meta's
    MemoryBroker, whose VINTF declaration goes with it.
  * Horizon's device identity and Meta properties go into /product/etc/build.prop.

Package manager state and compiled code are reset, since the platform signature changes.

    python tools/emulator.py start --wait && python tools/emulator.py root   (once per AVD)
    python tools/jni/build.py && python tools/compat.py && python tools/translator.py && python tools/vulkan.py
    python tools/thermal.py && python tools/tracking.py
    python tools/deploy.py [--no-reboot]
"""
import argparse
import io
import os
import re
import sys
import tarfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'ota'))
sys.path.insert(0, HERE)
import emulator  # noqa: E402
from ext4 import S_IFDIR, S_IFLNK, S_IFMT, S_IFREG, Ext4  # noqa: E402

# Stock's mainline modules (APEXes) stay underneath Horizon, and some of their apps share a UID
# with apps in /system. Those partners must be signed with the same key as the modules, so they
# come from stock too.
STOCK_NETWORKSTACK = {'skip': {'NetworkStack', 'NetworkStackNext'}, 'stock': ['NetworkStackGoogle']}
# The module metadata package describes those modules; the stock product overlays name the Google
# one (config_defaultModuleMetadataProvider), as they name the modules' other Google packages.
STOCK_MODULEMETADATA = {'stock': ['ModuleMetadataGoogle']}
# Except these: Horizon's framework uses APIs that only Horizon's builds of them have (see
# tools/inventory/module_api.py), and they hold no native code, so Horizon's APEX runs as is.
# Horizon's APEX file -> the stock one it replaces.
HORIZON_APEXES = {
    'com.android.configinfrastructure.apex': 'com.google.android.configinfrastructure.apex',  # DeviceConfig
    # PermissionController defines the roles; Meta's (extra_roles.xml) grant role permissions such
    # as horizonos.permission.READ_UI_MODE, which VrShell needs.
    'com.android.permission.apex': 'com.google.android.permission.apex',
}
# Meta's own APEXes: (partition, path). They go in /system_ext/apex, which stock's apexd scans (it
# has no /odm): com.meta.xr holds VrDriver (the XR runtime) and SystemActivities, com.meta.quest
# PresenceService, com.meta.hzos native libraries.
META_APEXES = [
    ('system_ext', '/apex/com.meta.hzos.apex'),
    ('product', '/apex/com.meta.quest.apex'),
    ('odm', '/apex/com.meta.xr.apex'),
]

# Files in Horizon's /odm/etc that belong to the partition, not to Meta's software.
ODM_STOCK = {'build.prop', 'fs_config_dirs', 'fs_config_files', 'group', 'passwd', 'NOTICE.xml.gz'}

# (partition, path in that partition's image, how[, options])
#   replace: wipe the stock directory in the overlay, put Horizon's there
#   merge:   add Horizon's files to the stock directory
#   bind:    Horizon's directory lives in /data/prism and is bind-mounted over the stock one
# options: skip  = top-level entries of Horizon's directory to leave out
#          stock = top-level entries of stock's directory to keep (replace and bind)
LAYOUT = [
    ('system', '/system/framework', 'replace'),
    ('system', '/system/priv-app', 'replace', STOCK_NETWORKSTACK),
    ('system', '/system/app', 'replace'),
    ('system', '/system/etc/permissions', 'replace'),
    ('system', '/system/etc/sysconfig', 'replace'),
    ('system', '/system/etc/classpaths', 'replace'),
    ('system', '/system/etc/compatconfig', 'replace'),
    ('system', '/system/etc/preloaded-classes', 'replace'),
    ('system', '/system/etc/dirty-image-objects', 'replace'),
    ('system', '/system/etc/boot-image.prof', 'replace'),
    ('system', '/system/etc/boot-image.bprof', 'replace'),
    ('system', '/system/etc/aconfig_flags.pb', 'replace'),
    ('system', '/system/etc/build_flags.json', 'replace'),
    ('system', '/system/etc/mrsystemservice.cfg', 'merge'),
    ('system', '/system/etc/xrs-hmdconfig.capnp.bin', 'merge'),
    ('system', '/system/etc/xrs_version_code', 'merge'),
    ('system', '/system/etc/display.conf', 'merge'),  # vrdevice: display timing
    ('system', '/system/etc/device_props.json', 'merge'),  # vrdevice: chipset, refresh rate
    ('system_ext', '/framework', 'replace'),
    # DeviceAuthServer proves the device's identity with the headset's certificate hardware, which
    # Prism doesn't fake (see README, Scope); without that hardware it crashes in a loop.
    ('system_ext', '/priv-app', 'bind', {'skip': {'DeviceAuthServer'}}),
    ('system_ext', '/app', 'bind'),
    # Native daemons, SELinux policy and HAL manifests stay stock until Prism provides them.
    ('system_ext', '/etc', 'merge',
     {'skip': {'init', 'selinux', 'vintf', 'build.prop', 'fs_config_dirs', 'fs_config_files'}}),
    ('product', '/priv-app', 'replace'),
    ('product', '/app', 'bind', STOCK_MODULEMETADATA),
    # Merged: the stock RROs point the framework at stock's Google-named mainline packages.
    ('product', '/overlay', 'merge'),
    ('product', '/etc/permissions', 'replace'),
    ('product', '/etc/sysconfig', 'replace'),
    ('product', '/etc/default-permissions', 'replace'),
    ('product', '/etc/VR_UI', 'merge'),
    ('product', '/etc/cameramuxmode', 'merge'),
    ('product', '/etc/perfstream', 'merge'),
]
# Precompiled arm64 code is useless on x86_64; ART recompiles from the dex. (An app's lib/arm64,
# its JNI libraries, is kept.)
SKIP_DIRS = {'oat', 'arm', 'arm64'}
# Apps' lib/arm64 entries link to Horizon's arm64 libraries, which Prism keeps in GUEST_DIR.
HORIZON_LIB64 = ('/system/lib64/', '/system_ext/lib64/')

# Meta properties copied from Horizon's build.prop files (identity props are handled separately).
META_PROPS = re.compile(r'^(config\.disable_|ro\.oculus\.|persist\.ovr\.|ro\.ovr\.|ro\.vros\.|ro\.hzos\.|'
                        r'persist\.oculus\.|debug\.oculus\.|ovr\.|ro\.config\.(event_filtering|max_number|os_app_id))')
IDENTITY = ('brand', 'device', 'manufacturer', 'model', 'name')

ZYGOTE_RC = '/system/etc/init/hw/init.zygote64.rc'
PRELOAD = '/system/lib64/libprism_jni.so'
DATA_ROOT = '/data/prism'

# ARM64 code runs through Digitalis (tools/translator.py), which replaces the stock image's
# libndk_translation: Horizon is built with ARMv8.1 atomics that the stock translator lacks.
# Meta's native daemons run as arm64 binaries (binfmt_misc). Their libraries are Horizon's own
# arm64 ones, in GUEST_DIR, except for the translator's bionic and host-proxy libraries. They come
# from /system, then /system_ext, then Horizon's APEXes (ICU for one), then /odm, flattened.
GUEST_DIR = '/system/lib64/arm64/prism'  # also in tools/compat.py
GUEST_SKIP_APEXES = {
    'com.android.runtime',  # bionic: the translator's
    'com.android.art',  # ART internals; daemons use none
    'com.android.vndk.v34',  # vendor variants of system libraries
}
GUEST_TRANSLATOR = {
    'libc.so', 'libm.so', 'libdl.so', 'libdl_android.so', 'ld-android.so', 'libnative_bridge_vdso.so',
    'libEGL.so', 'libGLESv1_CM.so', 'libGLESv2.so', 'libGLESv3.so', 'libvulkan.so', 'libOpenMAXAL.so',
    'libOpenSLES.so', 'libaaudio.so', 'libamidi.so', 'libandroid.so', 'libandroid_runtime.so',
    'libcamera2ndk.so', 'libjnigraphics.so', 'libmediandk.so', 'libnativehelper.so', 'libnativewindow.so',
    'libneuralnetworks.so', 'libwebviewchromium_plat_support.so',
    # Not libbinder_ndk: the translator's is a proxy to the host's binder, and a daemon also
    # loading Horizon's arm64 libbinder can't map /dev/binder twice. Daemons get Horizon's.
}
GUEST_LDCONFIG = '/system/etc/ld.config.arm64.txt'
NATIVE_BRIDGE = 'libberberis_arm64.so'
# Prism's Vulkan driver (tools/vulkan.py) wraps the emulator's; the loader picks vulkan.<this>.so.
VULKAN_DRIVER = 'prism'
# Meta daemons Prism runs: Horizon's init script for each, and the VINTF manifest fragments that
# declare its stable-AIDL services (servicemanager refuses undeclared ones). Only these fragments
# are installed: a declared service nobody serves makes clients wait for it forever.
DAEMON_RCS = {
    'preferencesserver.rc': [],  # settingsserver: PreferencesService
    'vrdevicemanagerserver.rc': [],  # vrdevice: the headset's device manager (display, thermal)
    'runtimeipcbroker.rc': ['runtimeipc_manifest.xml'],  # RuntimeIPC, which the XR runtime's clients use
    'vrfocusserver.rc': [],  # vrfocus: which app has VR focus
    'xrservice.rc': ['xrservice-permission.xml', 'xrservice-spaces.xml'],  # SpaceManager, for volumetric windows
}
# VINTF fragments for stable-AIDL services system_server hosts (Meta's Java services): the service
# refuses to start if servicemanager won't register it.
SYSTEM_SERVER_VINTF = [
    'vrpowermanager.xml',  # VrPowerManagerService: display power, and whether the headset is worn
]
# Prism's thermal HAL (tools/thermal.py): vrdevice needs the stable-AIDL IThermal.
THERMAL_HAL = 'android.hardware.thermal-service.prism'
THERMAL_RC = f'''service vendor.thermal-prism /vendor/bin/hw/{THERMAL_HAL}
    class hal
    user system
    group system
    seclabel u:r:su:s0
'''
THERMAL_VINTF = '''<manifest version="1.0" type="device">
    <hal format="aidl">
        <name>android.hardware.thermal</name>
        <version>1</version>
        <fqname>IThermal/default</fqname>
    </hal>
</manifest>
'''
DAEMON_SECLABEL = 'u:r:su:s0'  # stock policy has no domains for Meta's daemons (SELinux is permissive)
# Prism's tracking service (tools/tracking.py): Meta's MemoryBroker, which system_server hosts on a
# headset, and the head tracker's shared memory, which trackingservice fills there. Two processes:
# the broker refuses a host in its own process.
TRACKING = 'prism_tracking'
TRACKING_RC = f'''service prism_memorybroker /system_ext/bin/{TRACKING} broker
    class main
    user system
    group system
    seclabel {DAEMON_SECLABEL}

service prism_tracking /system_ext/bin/{TRACKING} host
    class main
    user system
    group system
    seclabel {DAEMON_SECLABEL}
'''
# Horizon's declaration, without the native instance (trackingservice's own, which nothing serves
# here: a declared service nobody serves makes clients wait for it).
MEMORYBROKER_VINTF = '''<manifest version="1.0" type="framework">
    <hal format="aidl" optional="true">
        <name>oculus.internal.tracking</name>
        <version>2</version>
        <fqname>IMemoryBrokerService/default</fqname>
    </hal>
</manifest>
'''


def device_path(partition, path):
    return path if partition == 'system' else f'/{partition}{path}'


class Archive:
    def __init__(self, path, overrides=None):
        self.tar = tarfile.open(path, 'w', format=tarfile.GNU_FORMAT)
        self.overrides = overrides or {}  # device path -> host file built by Prism (tools/compat.py)
        self.count = 0

    def add(self, name, kind, mode, uid=0, gid=0, data=b'', target=''):
        info = tarfile.TarInfo(name.lstrip('/'))
        info.mode, info.uid, info.gid, info.mtime = mode, uid, gid, int(time.time())
        if kind == 'd':
            info.type = tarfile.DIRTYPE
        elif kind == 'l':
            info.type, info.linkname = tarfile.SYMTYPE, target
        else:
            info.size = len(data)
        self.tar.addfile(info, io.BytesIO(data) if kind == 'f' else None)
        self.count += 1

    def add_tree(self, fs, src, dest, skip=(), flat=False):
        """Copies an image subtree (file or directory) to dest, keeping modes, owners and symlinks.
        flat: only the directory's own files, none of its subdirectories."""
        ino = fs.lookup(src)
        for path, node in fs.walk(ino, src.rstrip('/')) if ino.mode & S_IFMT == S_IFDIR else [(src, ino)]:
            rel = path[len(src.rstrip('/')):]
            parts = rel.strip('/').split('/') if rel.strip('/') else []
            if (any(p in SKIP_DIRS and (i == 0 or parts[i - 1] != 'lib') for i, p in enumerate(parts))
                    or (parts and parts[0] in skip)):
                continue
            kind = {S_IFDIR: 'd', S_IFLNK: 'l', S_IFREG: 'f'}.get(node.mode & S_IFMT)
            if not kind or (flat and parts and (len(parts) > 1 or kind == 'd')):
                continue
            if kind == 'f' and dest + rel in self.overrides:
                with open(self.overrides[dest + rel], 'rb') as f:
                    data = f.read()
            else:
                data = fs.read(node) if kind in ('f', 'l') else b''
            target = data.decode('utf-8', 'surrogateescape') if kind == 'l' else ''
            if kind == 'l' and '/lib/arm64/' in rel and target.startswith(HORIZON_LIB64):
                target = f'{GUEST_DIR}/{target.rsplit("/", 1)[1]}'
            self.add(dest + rel, kind, node.mode & 0o7777, node.uid, node.gid, data if kind == 'f' else b'', target)

    def close(self):
        self.tar.close()


def guest_ldconfig(translator):
    """The translator's arm64 guest linker config, with Horizon's libraries (GUEST_DIR) searched
    before the translator's: Meta's code needs Horizon's builds of liblog, libgui, fmt and the like,
    and the translator's own libraries resolve against them too. The [system] section serves app
    processes (their JNI libraries) and, through the extra dir lines, Meta's daemons."""
    with open(os.path.join(translator, *GUEST_LDCONFIG.strip('/').split('/')), encoding='utf-8') as f:
        text = f.read()
    guest = GUEST_DIR.replace('lib64', '${LIB}')
    search = 'namespace.default.search.paths += /system/${LIB}/arm64\n'
    permitted = 'namespace.default.permitted.paths += /system/${LIB}/arm64/bootstrap\n'
    for line in (search, permitted):
        assert text.count(line) == 1, line
    text = text.replace(search, f'namespace.default.search.paths += {guest}\n{search}')
    text = text.replace(permitted, f'{permitted}namespace.default.permitted.paths += {guest}\n')
    dirs = 'dir.system = /system_ext/bin\ndir.system = /odm/bin\n'
    first_section = text.index('\n[')
    return text[:first_section] + '\n' + dirs + text[first_section:]


def read_prop_file(fs, path):
    try:
        text = fs.read(fs.lookup(path)).decode('utf-8', 'replace')
    except FileNotFoundError:
        return []
    return [line for line in text.splitlines() if '=' in line and not line.lstrip().startswith('#')]


def prism_props(images):
    props = {}
    for partition, path in (('system', '/system/build.prop'), ('system_ext', '/etc/build.prop'),
                            ('product', '/etc/build.prop')):
        for line in read_prop_file(images[partition], path):
            key, _, value = line.partition('=')
            if META_PROPS.match(key.strip()):
                props[key.strip()] = value.strip()
            m = re.fullmatch(rf'ro\.product\.{partition}\.({"|".join(IDENTITY)})', key.strip())
            if m:  # the product partition's identity props win, so set Horizon's there
                props[f'ro.product.product.{m.group(1)}'] = value.strip()
    props['ro.control_privapp_permissions'] = 'log'  # report missing allowlist entries, don't crash
    props['ro.dalvik.vm.native.bridge'] = NATIVE_BRIDGE
    props['ro.hardware.vulkan'] = VULKAN_DRIVER  # overrides /vendor/build.prop's: product loads last
    # The compositor makes its GL contexts current without a surface, which the emulator's EGL
    # refuses (no EGL_KHR_surfaceless_context). Meta's switch gives each context a pbuffer instead.
    props['persist.oculus.forceGLESContextBuffer'] = 'true'
    # Strata is the headset's display path (Meta's composer HAL). Without it the compositor's
    # output surface comes from SurfaceFlinger, which is what the emulator's window shows.
    props['persist.oculus.strata.disable'] = 'true'
    # The headset scans its panel out in slices, each rendered in its own pass that clears the whole
    # image first; on one screen the last slice's clear erased the others. One slice, the height of
    # the window, renders the frame in one pass.
    props['ro.ovr.sliceCountY'] = '1'
    props['debug.oculus.frontbufferHeight'] = '1080'
    # VrShell gives its environment 15 s to load, times the device's timeout multiplier when Meta's
    # switch allows it. Under translation the load takes 15 s to well over a minute, so it failed on
    # most boots and the home stayed empty; AOSP's multiplier is meant for slow (emulated) hardware.
    props['ro.hw_timeout_multiplier'] = '8'
    props['persist.oculus.shell_hw_mult.enable'] = '1'  # read with atoi: 'true' is off
    return props


def build(args):
    images = {p: Ext4(os.path.join(args.images, p + '.img')) for p in ('system', 'system_ext', 'product')}
    stock = stock_images = {p: Ext4(os.path.join(args.baseline, p + '.img'))
                            for p in ('system', 'system_ext', 'product')}
    os.makedirs(args.out, exist_ok=True)
    overrides = {}
    for base, _dirs, files in os.walk(args.compat):
        for name in files:
            host = os.path.join(base, name)
            overrides['/' + os.path.relpath(host, args.compat).replace(os.sep, '/')] = host
    if not overrides:
        sys.exit(f'no compatibility jars in {args.compat}; run: python tools/compat.py')
    if not os.path.exists(os.path.join(args.translator, 'system', 'lib64', NATIVE_BRIDGE)):
        sys.exit(f'no translator in {args.translator}; run: python tools/translator.py')
    vulkan = os.path.join(args.vulkan, f'vulkan.{VULKAN_DRIVER}.so')
    if not os.path.exists(vulkan):
        sys.exit(f'no Vulkan driver in {args.vulkan}; run: python tools/vulkan.py')
    thermal = os.path.join(args.thermal, THERMAL_HAL)
    if not os.path.exists(thermal):
        sys.exit(f'no thermal HAL in {args.thermal}; run: python tools/thermal.py')
    tracking = os.path.join(args.tracking, TRACKING)
    if not os.path.exists(tracking):
        sys.exit(f'no tracking service in {args.tracking}; run: python tools/tracking.py')
    print(f'using {len(overrides)} Prism-patched files: {", ".join(sorted(overrides))}')
    overlay = Archive(os.path.join(args.out, 'overlay.tar'), overrides)
    data = Archive(os.path.join(args.out, 'data.tar'))
    replaced, binds = [], []
    for partition, path, how, *rest in LAYOUT:
        options = rest[0] if rest else {}
        fs = images[partition]
        target = device_path(partition, path)
        try:
            fs.lookup(path)
        except FileNotFoundError:
            print(f'  (Horizon has no {target}; skipped)')
            continue
        skip = options.get('skip', ())
        if how == 'bind':
            data.add_tree(fs, path, f'{DATA_ROOT}/{partition}{path}', skip)
            binds.append((f'{DATA_ROOT}/{partition}{path}', target))
            overlay.add(target, 'd', 0o755)  # the mount point; stock has no /system_ext/app
        else:
            overlay.add_tree(fs, path, target, skip)
            if how == 'replace':
                replaced.append(target)
        for name in options.get('stock', []):  # stock's images have the same layout
            if how == 'bind':
                data.add_tree(stock_images[partition], f'{path}/{name}', f'{DATA_ROOT}/{partition}{path}/{name}')
            else:
                overlay.add_tree(stock_images[partition], f'{path}/{name}', f'{target}/{name}')

    for horizon_apex, stock_apex in HORIZON_APEXES.items():
        stock_images['system'].lookup(f'/system/apex/{stock_apex}')  # raises if stock has no such file
        replaced.append(f'/system/apex/{stock_apex}')
        overlay.add_tree(images['system'], f'/system/apex/{horizon_apex}', f'/system/apex/{horizon_apex}')
    overlay.add('/system_ext/apex', 'd', 0o755)
    odm = Ext4(os.path.join(args.images, 'odm.img'))
    for partition, path in META_APEXES:
        fs = odm if partition == 'odm' else images[partition]
        overlay.add_tree(fs, path, f'/system_ext/apex/{path.rsplit("/", 1)[1]}')
    # The OpenXR loader's fallback when no runtime broker answers: <partition>/etc/openxr/1, links
    # into com.meta.xr naming VrDriver's runtime. Horizon has them on /odm; the loader also looks
    # in /product.
    overlay.add('/product/etc/openxr', 'd', 0o755)
    overlay.add_tree(odm, '/etc/openxr/1', '/product/etc/openxr/1')
    # Meta's configuration files in /odm/etc (thread priorities, tracking and sensor services, ...).
    # The emulator's /odm/etc links to /vendor/odm/etc. Only the files: the directories hold
    # tracking models (over 200 MB), and the partition's own files stay stock.
    overlay.add_tree(odm, '/etc', '/vendor/odm/etc', ODM_STOCK, flat=True)

    # The Digitalis translator: its guest directories replace stock's, host files are added.
    for guest in ('/system/lib64/arm64', '/system/bin/arm64'):
        replaced.append(guest)
    for base, _dirs, files in os.walk(os.path.join(args.translator, 'system')):
        for name in files:
            device = '/' + os.path.relpath(os.path.join(base, name), args.translator).replace(os.sep, '/')
            if device == GUEST_LDCONFIG or device.endswith('/berberis.rc'):
                continue  # generated below; binfmt_misc is mounted by stock and registered by prism.rc
            executable = '/bin/' in device
            with open(os.path.join(base, name), 'rb') as f:
                overlay.add(device, 'f', 0o755 if executable else 0o644, gid=2000 if executable else 0,
                            data=f.read())

    # Meta's daemons: binaries, Horizon's arm64 libraries for the translator, linker config, init scripts.
    overlay.add_tree(images['system_ext'], '/bin', '/system_ext/bin')
    sources = [(images['system'], '/system/lib64'), (images['system_ext'], '/lib64')]
    apex_dir = os.path.join(args.images, 'apex')
    for name in sorted(os.listdir(apex_dir)):
        if name.endswith('.img') and name[:-4] not in GUEST_SKIP_APEXES:
            fs = Ext4(os.path.join(apex_dir, name))
            try:
                fs.lookup('/lib64')
            except FileNotFoundError:
                continue
            sources.append((fs, '/lib64'))
    sources.append((odm, '/lib64'))  # Meta's tracking and sensor libraries
    taken = set(GUEST_TRANSLATOR)
    for fs, src in sources:  # the first source with a library wins
        data.add_tree(fs, src, f'{DATA_ROOT}/guest', taken, flat=True)
        taken |= {name for name, _ in fs.listdir(fs.lookup(src))}
    overlay.add(GUEST_DIR, 'd', 0o755)
    binds.append((f'{DATA_ROOT}/guest', GUEST_DIR))
    # Meta's code also dlopens its libraries by absolute path (/system_ext/lib64/libosndk...). Each
    # Horizon library whose name stock's directory doesn't use gets a link there to its GUEST_DIR
    # copy; host processes never ask for those names.
    for partition, src, dest in (('system', '/system/lib64', '/system/lib64'),
                                 ('system_ext', '/lib64', '/system_ext/lib64')):
        stock_fs = stock_images[partition]
        try:
            stock_names = {name for name, _ in stock_fs.listdir(stock_fs.lookup(src))}
        except FileNotFoundError:
            stock_names = set()
            overlay.add(dest, 'd', 0o755)
        fs = images[partition]
        for name, _ in fs.listdir(fs.lookup(src)):
            if name.endswith('.so') and name not in stock_names and name not in GUEST_TRANSLATOR:
                overlay.add(f'{dest}/{name}', 'l', 0o777, target=f'{GUEST_DIR}/{name}')
    overlay.add(GUEST_LDCONFIG, 'f', 0o644, data=guest_ldconfig(args.translator).encode())
    overlay.add('/system_ext/etc/vintf/manifest', 'd', 0o755)  # stock has only manifest.xml
    for rc, fragments in DAEMON_RCS.items():
        text = images['system_ext'].read(images['system_ext'].lookup(f'/etc/init/{rc}')).decode()
        text = re.sub(r'(^[ \t]*service [^\n]*\n)', rf'\1    seclabel {DAEMON_SECLABEL}\n', text, flags=re.M)
        overlay.add(f'/system_ext/etc/init/{rc}', 'f', 0o644, data=text.encode())
        for name in fragments:
            overlay.add_tree(images['system_ext'], f'/etc/vintf/manifest/{name}',
                             f'/system_ext/etc/vintf/manifest/{name}')
    for name in SYSTEM_SERVER_VINTF:
        overlay.add_tree(images['system_ext'], f'/etc/vintf/manifest/{name}', f'/system_ext/etc/vintf/manifest/{name}')

    # Prism's JNI glue.
    for name in sorted(os.listdir(args.jni)):
        if name.endswith('.so'):
            with open(os.path.join(args.jni, name), 'rb') as f:
                overlay.add(f'/system/lib64/{name}', 'f', 0o644, data=f.read())

    # Prism's Vulkan driver, next to the emulator's.
    with open(vulkan, 'rb') as f:
        overlay.add(f'/vendor/lib64/hw/vulkan.{VULKAN_DRIVER}.so', 'f', 0o644, data=f.read())

    # Prism's thermal HAL, beside the emulator's HIDL one (which the framework stops using).
    with open(thermal, 'rb') as f:
        overlay.add(f'/vendor/bin/hw/{THERMAL_HAL}', 'f', 0o755, gid=2000, data=f.read())
    overlay.add(f'/vendor/etc/init/{THERMAL_HAL}.rc', 'f', 0o644, data=THERMAL_RC.encode())
    overlay.add(f'/vendor/etc/vintf/manifest/{THERMAL_HAL}.xml', 'f', 0o644, data=THERMAL_VINTF.encode())

    # Prism's tracking service.
    with open(tracking, 'rb') as f:
        overlay.add(f'/system_ext/bin/{TRACKING}', 'f', 0o755, gid=2000, data=f.read())
    overlay.add(f'/system_ext/etc/init/{TRACKING}.rc', 'f', 0o644, data=TRACKING_RC.encode())
    overlay.add('/system_ext/etc/vintf/manifest/memorybroker_manifest.xml', 'f', 0o644,
                data=MEMORYBROKER_VINTF.encode())

    # zygote preloads libprism_jni.so.
    rc = stock['system'].read(stock['system'].lookup(ZYGOTE_RC)).decode()
    rc = re.sub(r'(service zygote [^\n]*\n)', rf'\1    setenv LD_PRELOAD {PRELOAD}\n', rc, count=1)
    overlay.add(ZYGOTE_RC, 'f', 0o644, data=rc.encode())

    # prism.rc: bind mounts for what lives on /data, and arm64 executables runnable from early on.
    lines = ['# Generated by tools/deploy.py.',
             '# Stock registers the arm64 binfmt handlers on a property trigger, after post-fs-data, which',
             '# is too late for Meta daemons started there. Register them as soon as binfmt_misc is mounted',
             '# (ndk_translation.rc, parsed before this file); stock\'s later attempt just fails harmlessly.',
             'on early-init && property:ro.enable.native.bridge.exec=1',
             '    copy /system/etc/binfmt_misc/arm64_exe /proc/sys/fs/binfmt_misc/register',
             '    copy /system/etc/binfmt_misc/arm64_dyn /proc/sys/fs/binfmt_misc/register',
             '',
             '# Horizon directories kept on /data (system partitions are too small).',
             'on post-fs-data']
    lines += [f'    mount none {src} {dst} bind' for src, dst in binds]
    lines += ['',
              '# adb stays up whatever Horizon decides: Meta\'s software turns adb off (as a headset does',
              '# outside developer mode), and init then stops adbd. The emulator\'s adbd talks over a qemu',
              '# pipe, not USB, so the USB functions don\'t matter to it.',
              'on property:init.svc.adbd=stopped',
              '    start adbd',
              '',
              '# Apps render only while the headset is worn. There is no proximity sensor, so Prism tells',
              '# VrPowerManagerService the headset is on (its virtual proximity, a developer setting).',
              'on property:sys.boot_completed=1',
              f'    exec_background {DAEMON_SECLABEL} system system -- /system/bin/am broadcast -a com.oculus.vrpowermanager.prox_close']
    overlay.add('/system/etc/init/prism.rc', 'f', 0o644, data=('\n'.join(lines) + '\n').encode())

    # Identity and Meta properties, appended to the stock product build.prop.
    base = stock['product'].read(stock['product'].lookup('/etc/build.prop')).decode()
    props = prism_props(images)
    section = '\n# Prism: Horizon OS identity and Meta properties\n' + ''.join(f'{k}={v}\n' for k, v in props.items())
    overlay.add('/product/etc/build.prop', 'f', 0o644, data=(base + section).encode())

    overlay.close()
    data.close()
    print(f'overlay.tar: {overlay.count} entries; data.tar: {data.count} entries; {len(props)} properties')
    return replaced, binds


RESET_PARTITIONS = ('system', 'system_ext', 'product', 'vendor')
# Per-package state each boot rebuilds. Stock and Horizon write it differently (Horizon has more
# app ops, and its own permission module), and a deploy boots one, then the other. The .reservecopy
# backups go too: the package manager restores from them when the file is missing.
FRESH_PACKAGE_STATE = ('rm -rf /data/system/packages.xml* /data/system/packages-backup.xml /data/system/packages.list '
                       '/data/system/package_cache /data/system/users/0/package-restrictions.xml* '
                       '/data/dalvik-cache/x86_64/* '
                       '/data/misc/apexdata/com.android.art/dalvik-cache/* '
                       '/data/system/appops.xml /data/system/appops_accesses.xml /data/system/appops '
                       '/data/misc_de/0/apexdata/com.android.permission/*')
# Once Horizon turns adb off, the setting and the persisted USB config outlive the deploy; stock's
# boot then has no adb either.
BOOT_ID = 'cat /proc/sys/kernel/random/boot_id'
ADB_ON = 'setprop persist.sys.usb.config adb && (settings put global adb_enabled 1 || true)'


def reset(args):
    """Empties the writable layer (adb remount's overlayfs) of the partitions Prism changes and
    reboots into plain stock. Changing those layers goes through overlayfs, so what an earlier
    deploy deleted stays deleted (whiteouts) unless each deploy starts from stock."""
    emulator.root(args)  # adb root doesn't survive a reboot, and every deploy ends with one
    shell = emulator.shell
    mounts = shell('cat /proc/mounts')
    uppers = []
    for partition in RESET_PARTITIONS:
        m = re.search(rf'^overlay /{partition} overlay \S*upperdir=([^,\s]+)', mounts, re.M)
        if not m:
            sys.exit(f'/{partition} has no overlay; run: python tools/emulator.py root')
        uppers.append(m.group(1))
    print('resetting to stock...', flush=True)
    out = shell('(' + ' && '.join([f'find {u} -mindepth 1 -maxdepth 1 -exec rm -rf {{}} +' for u in uppers] +
                                  [FRESH_PACKAGE_STATE, 'sync', 'echo reset']) + ') 2>&1', check=False)
    if 'reset' not in out:
        sys.exit(f'resetting failed:\n{out}')
    boot = shell(BOOT_ID, check=False).strip()
    shell(ADB_ON, check=False)  # if this changes the USB config, init restarts adbd and drops the shell
    # The upper layers were emptied under the live overlay, which now has stale entries: the reboot
    # must happen. A restarting adbd can swallow it, so it's repeated until the boot id changes.
    for _ in range(10):
        emulator.adb('wait-for-device')
        emulator.adb('reboot', check=False)
        time.sleep(5)
        emulator.adb('wait-for-device')
        if shell(BOOT_ID, check=False).strip() not in ('', boot):
            break
    else:
        sys.exit('the emulator did not reboot after the reset')
    args.timeout = 300
    if not emulator.wait(args):
        sys.exit('stock did not finish booting after the reset')
    emulator.root(args)


def apply(args, replaced):
    adb, shell = emulator.adb, emulator.shell
    reset(args)
    if shell('touch /system/.prism && rm /system/.prism && echo ok', check=False).strip() != 'ok':
        sys.exit('/system is not writable; run: python tools/emulator.py root')
    for name in ('overlay.tar', 'data.tar'):
        print(f'pushing {name}...', flush=True)
        adb('push', os.path.join(args.out, name), f'/data/local/tmp/prism-{name}', capture=False)
    script = ['set -e',
              *[f'rm -rf {path}' for path in replaced],
              'tar -xf /data/local/tmp/prism-overlay.tar -C /',
              f'rm -rf {DATA_ROOT}',
              'tar -xf /data/local/tmp/prism-data.tar -C /',
              f'chcon -hR u:object_r:system_file:s0 {DATA_ROOT}',  # -h: Horizon has dangling symlinks
              'restorecon -R /system/framework /system/priv-app /system/app /system/etc /system/lib64 /system/bin '
              '/system_ext/framework /system_ext/etc /product/priv-app /product/overlay /product/etc /vendor/lib64/hw '
              '/vendor/odm/etc /vendor/bin/hw /vendor/etc/init /vendor/etc/vintf',
              'rm -f /data/local/tmp/prism-overlay.tar /data/local/tmp/prism-data.tar',
              # Fresh package manager state and compiled code for the new platform.
              FRESH_PACKAGE_STATE,
              'sync', 'echo applied']
    out = adb('shell', '\n'.join(script), check=False, timeout=900, stderr=True)
    print(out.strip().splitlines()[-1] if out.strip() else '')
    if 'applied' not in out:
        sys.exit(f'applying failed:\n{out}')
    if not args.no_reboot:
        adb('reboot')
        print('rebooting into Horizon\'s framework')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--images', default=os.path.join('work', 'images'))
    ap.add_argument('--baseline', default=os.path.join('work', 'baseline', 'images'))
    ap.add_argument('--jni', default=os.path.join('work', 'build', 'jni'))
    ap.add_argument('--compat', default=os.path.join('work', 'build', 'compat'))
    ap.add_argument('--translator', default=os.path.join('work', 'build', 'translator'))
    ap.add_argument('--vulkan', default=os.path.join('work', 'build', 'vulkan'))
    ap.add_argument('--thermal', default=os.path.join('work', 'build', 'thermal'))
    ap.add_argument('--tracking', default=os.path.join('work', 'build', 'tracking'))
    ap.add_argument('--out', default=os.path.join('work', 'deploy'))
    ap.add_argument('--build-only', action='store_true')
    ap.add_argument('--no-reboot', action='store_true')
    args = ap.parse_args()
    replaced = build(args)[0]
    if not args.build_only:
        apply(args, replaced)


if __name__ == '__main__':
    main()
