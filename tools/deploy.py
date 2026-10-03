"""Put Horizon OS's Java layer onto Prism's emulator (stock Android 14 x86_64 underneath).

What goes where:
  * Horizon's framework, small app directories and configuration replace the stock ones in the
    writable system overlay (adb remount).
  * The large app directories don't fit in that overlay; they go to /data/prism and prism.rc
    bind-mounts them over the stock directories in post-fs-data, before zygote starts.
  * Prism's JNI glue goes to /system/lib64; zygote preloads libprism_jni.so.
  * Horizon's device identity and Meta properties go into /product/etc/build.prop.

Package manager state and compiled code are reset, since the platform signature changes.

    python tools/emulator.py start --wait && python tools/emulator.py root   (once per AVD)
    python tools/jni/build.py && python tools/compat.py
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

# (partition, path in that partition's image, how[, options])
#   replace: wipe the stock directory in the overlay, put Horizon's there
#   merge:   add Horizon's files to the stock directory
#   bind:    Horizon's directory lives in /data/prism and is bind-mounted over the stock one
# options: skip  = top-level entries of Horizon's directory to leave out
#          stock = top-level entries of stock's directory to keep (replace only)
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
    ('system_ext', '/framework', 'replace'),
    ('system_ext', '/priv-app', 'bind'),
    ('system_ext', '/app', 'bind'),
    # Native daemons, SELinux policy and HAL manifests stay stock until Prism provides them.
    ('system_ext', '/etc', 'merge',
     {'skip': {'init', 'selinux', 'vintf', 'build.prop', 'fs_config_dirs', 'fs_config_files'}}),
    ('product', '/priv-app', 'replace'),
    ('product', '/app', 'bind'),
    ('product', '/overlay', 'merge'),
    ('product', '/etc/permissions', 'replace'),
    ('product', '/etc/sysconfig', 'replace'),
    ('product', '/etc/default-permissions', 'replace'),
    ('product', '/etc/VR_UI', 'merge'),
    ('product', '/etc/cameramuxmode', 'merge'),
    ('product', '/etc/perfstream', 'merge'),
]
# Precompiled arm64 code is useless on x86_64; ART recompiles from the dex.
SKIP_DIRS = {'oat', 'arm', 'arm64'}

# Meta properties copied from Horizon's build.prop files (identity props are handled separately).
META_PROPS = re.compile(r'^(config\.disable_|ro\.oculus\.|persist\.ovr\.|ro\.ovr\.|ro\.vros\.|ro\.hzos\.|'
                        r'persist\.oculus\.|debug\.oculus\.|ovr\.|ro\.config\.(event_filtering|max_number|os_app_id))')
IDENTITY = ('brand', 'device', 'manufacturer', 'model', 'name')

ZYGOTE_RC = '/system/etc/init/hw/init.zygote64.rc'
PRELOAD = '/system/lib64/libprism_jni.so'
DATA_ROOT = '/data/prism'

# Meta's native daemons run as arm64 binaries through the stock image's translator
# (binfmt_misc + ndk_translation). Their libraries are Horizon's own arm64 ones, in
# GUEST_DIR, except for the translator's bionic and host-proxy libraries, which stay stock.
GUEST_DIR = '/system/lib64/arm64/prism'
GUEST_STOCK = {
    'libc.so', 'libm.so', 'libdl.so', 'libdl_android.so', 'ld-android.so', 'libnative_bridge_vdso.so',
    'libEGL.so', 'libGLESv1_CM.so', 'libGLESv2.so', 'libGLESv3.so', 'libvulkan.so', 'libOpenMAXAL.so',
    'libOpenSLES.so', 'libaaudio.so', 'libamidi.so', 'libandroid.so', 'libandroid_runtime.so',
    'libcamera2ndk.so', 'libjnigraphics.so', 'libmediandk.so', 'libnativehelper.so', 'libnativewindow.so',
    'libneuralnetworks.so', 'libwebviewchromium_plat_support.so',
    # Not libbinder_ndk: stock's is a proxy to the host's binder, and a daemon also loading
    # Horizon's arm64 libbinder can't map /dev/binder twice. Daemons get Horizon's.
}
GUEST_LDCONFIG = '/system/etc/ld.config.arm64.txt'
# Meta daemons Prism runs, by the Horizon init script that defines them.
DAEMON_RCS = ['preferencesserver.rc']  # settingsserver: PreferencesService
DAEMON_SECLABEL = 'u:r:su:s0'  # stock policy has no domains for Meta's daemons (SELinux is permissive)


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
            if any(p in SKIP_DIRS for p in parts) or (parts and parts[0] in skip):
                continue
            kind = {S_IFDIR: 'd', S_IFLNK: 'l', S_IFREG: 'f'}.get(node.mode & S_IFMT)
            if not kind or (flat and parts and (len(parts) > 1 or kind == 'd')):
                continue
            if kind == 'f' and dest + rel in self.overrides:
                with open(self.overrides[dest + rel], 'rb') as f:
                    data = f.read()
            else:
                data = fs.read(node) if kind in ('f', 'l') else b''
            self.add(dest + rel, kind, node.mode & 0o7777, node.uid, node.gid,
                     data if kind == 'f' else b'', data.decode('utf-8', 'surrogateescape') if kind == 'l' else '')

    def close(self):
        self.tar.close()


def guest_ldconfig(stock_system):
    """Stock's arm64 guest linker config plus a section for Meta's daemons. Apps keep the [system]
    section; executables under /system_ext and /odm get Horizon's libraries first."""
    text = stock_system.read(stock_system.lookup(GUEST_LDCONFIG)).decode()
    dirs = 'dir.prism = /system_ext/bin\ndir.prism = /odm/bin\n'
    first_section = text.index('\n[')
    section = ('\n[prism]\n'
               'namespace.default.isolated = false\n'
               'namespace.default.search.paths  = /system/${LIB}/arm64/bootstrap\n'
               f'namespace.default.search.paths += {GUEST_DIR.replace("lib64", "${LIB}")}\n'
               'namespace.default.search.paths += /system/${LIB}/arm64\n')
    return text[:first_section] + '\n' + dirs + text[first_section:] + section


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
        else:
            overlay.add_tree(fs, path, target, skip)
            if how == 'replace':
                replaced.append(target)
        for name in options.get('stock', []):  # stock's images have the same layout
            overlay.add_tree(stock_images[partition], f'{path}/{name}', f'{target}/{name}')

    # Meta's daemons: binaries, Horizon's arm64 libraries for the translator, linker config, init scripts.
    overlay.add_tree(images['system_ext'], '/bin', '/system_ext/bin')
    for partition, src in (('system', '/system/lib64'), ('system_ext', '/lib64')):
        data.add_tree(images[partition], src, f'{DATA_ROOT}/guest', GUEST_STOCK, flat=True)
    overlay.add(GUEST_DIR, 'd', 0o755)
    binds.append((f'{DATA_ROOT}/guest', GUEST_DIR))
    overlay.add(GUEST_LDCONFIG, 'f', 0o644, data=guest_ldconfig(stock['system']).encode())
    for rc in DAEMON_RCS:
        text = images['system_ext'].read(images['system_ext'].lookup(f'/etc/init/{rc}')).decode()
        text = re.sub(r'(^service [^\n]*\n)', rf'\1    seclabel {DAEMON_SECLABEL}\n', text, flags=re.M)
        overlay.add(f'/system_ext/etc/init/{rc}', 'f', 0o644, data=text.encode())

    # Prism's JNI glue.
    for name in sorted(os.listdir(args.jni)):
        if name.endswith('.so'):
            with open(os.path.join(args.jni, name), 'rb') as f:
                overlay.add(f'/system/lib64/{name}', 'f', 0o644, data=f.read())

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


def apply(args, replaced, binds):
    adb, shell = emulator.adb, emulator.shell
    if shell('touch /system/.prism && rm /system/.prism && echo ok', check=False).strip() != 'ok':
        sys.exit('/system is not writable; run: python tools/emulator.py root')
    for name in ('overlay.tar', 'data.tar'):
        print(f'pushing {name}...', flush=True)
        adb('push', os.path.join(args.out, name), f'/data/local/tmp/prism-{name}', capture=False)
    script = ['set -e',
              # Undo any earlier bind mounts so the stock directories underneath are what we replace.
              *[f'umount {dst} 2>/dev/null || true' for _src, dst in binds],
              *[f'rm -rf {path}' for path in replaced],
              'tar -xf /data/local/tmp/prism-overlay.tar -C /',
              f'rm -rf {DATA_ROOT}',
              'tar -xf /data/local/tmp/prism-data.tar -C /',
              f'chcon -hR u:object_r:system_file:s0 {DATA_ROOT}',  # -h: Horizon has dangling symlinks
              'restorecon -R /system/framework /system/priv-app /system/app /system/etc /system/lib64 '
              '/system_ext/framework /system_ext/etc /product/priv-app /product/overlay /product/etc',
              'rm -f /data/local/tmp/prism-overlay.tar /data/local/tmp/prism-data.tar',
              # Fresh package manager state and compiled code for the new platform.
              'rm -rf /data/system/packages.xml /data/system/packages.list /data/system/package_cache '
              '/data/system/users/0/package-restrictions.xml /data/dalvik-cache/x86_64/* '
              '/data/misc/apexdata/com.android.art/dalvik-cache/*',
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
    ap.add_argument('--out', default=os.path.join('work', 'deploy'))
    ap.add_argument('--build-only', action='store_true')
    ap.add_argument('--no-reboot', action='store_true')
    args = ap.parse_args()
    replaced, binds = build(args)
    if not args.build_only:
        apply(args, replaced, binds)


if __name__ == '__main__':
    main()
