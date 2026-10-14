"""Prism's Android Emulator: create the AVD, start/stop it, and run adb against it.

Prism uses its own AVD (prism-api34) on its own port, so it never touches other AVDs. Another of
Prism's AVDs (a test one) is named by PRISM_AVD, which the tools run by prism.py --avd inherit; it
uses the same port, so one of them runs at a time.

    python tools/emulator.py create            # write the AVD (stock API 34 x86_64 image)
    python tools/emulator.py config [--display WxH] [--memory MB] [--cores N]  # change the AVD; next start
    python tools/emulator.py start [--headless|--window] [--wait]  # boot it: writable system, SELinux permissive; windowless above 60 Hz
    python tools/emulator.py wait              # until Android reports boot completed
    python tools/emulator.py ready             # until Horizon's home is drawing (on the TSC, VrShell up, frames shared)
    python tools/emulator.py patch-share-vid [--rate HZ] [--undo]  # -share-vid shares more than 60 frames a second
    python tools/emulator.py clock             # reboot until the guest's clock is the TSC (start --wait does this)
    python tools/emulator.py root              # adb root + make /system, /system_ext, /product writable
    python tools/emulator.py status            # boot state, key processes, services, latest crash
    python tools/emulator.py emulator-flag [hidden|shown]  # ro.kernel.qemu for Meta's runtime; reboots
    python tools/emulator.py model [quest3|eureka]         # Build.MODEL; reboots
    python tools/emulator.py stop
    python tools/emulator.py adb <args...>
"""
import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import time

DEFAULT_AVD = 'prism-api34'
AVD = os.environ.get('PRISM_AVD') or DEFAULT_AVD
PORT = 5590
SERIAL = f'emulator-{PORT}'
IMAGE = os.path.join('system-images', 'android-34', 'google_apis', 'x86_64')
LOG_DIR = os.path.join('work', 'logs')
# The display's refresh rate. The emulator shares at most 60 frames a second (-share-vid, a frame within
# 1/60 s of the last skipped), and deploy runs Meta's compositor at this rate.
DISPLAY_HZ = 60
# The display's size. Each half is an eye, shown over the same angles whatever the size (deploy's
# metersPerTanAngCtr), so more pixels make it sharper: 2560x1440 gives about 630 pixels a tangent
# unit, near what a Quest renders at the middle of its view (1920x1080 gave 474). Keep it 16:9.
DISPLAY = (2560, 1440)
# Android's density buckets; the display takes the one nearest 320 at 1080 rows, so its 2D UI keeps its size.
DENSITIES = (160, 213, 240, 280, 320, 360, 400, 420, 440, 480, 560, 640)


def density(height):
    return min(DENSITIES, key=lambda d: abs(d - 320 * height / 1080))


def sdk_root():
    for candidate in (os.environ.get('ANDROID_SDK_ROOT'), os.environ.get('ANDROID_HOME'),
                      os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk'),
                      os.path.expanduser('~/Android/Sdk')):
        if candidate and os.path.isdir(os.path.join(candidate, 'emulator')):
            return candidate
    sys.exit('Android SDK not found; set ANDROID_SDK_ROOT')


def use_avd(name):
    """Makes `name` the AVD this process, and every tool it runs, uses."""
    global AVD
    AVD = os.environ['PRISM_AVD'] = name or DEFAULT_AVD


def avd_home():
    return os.environ.get('ANDROID_AVD_HOME') or os.path.join(os.path.expanduser('~'), '.android', 'avd')


def adb_path():
    return os.path.join(sdk_root(), 'platform-tools', 'adb.exe' if os.name == 'nt' else 'adb')


def adb(*args, check=True, capture=True, timeout=None, stderr=False):
    """Runs adb against Prism's emulator and returns stdout (text), plus stderr if asked."""
    result = subprocess.run([adb_path(), '-s', SERIAL, *args], capture_output=capture, text=True,
                            encoding='utf-8', errors='replace', timeout=timeout)
    if check and result.returncode != 0:
        raise RuntimeError(f'adb {" ".join(args)} failed: {(result.stderr or result.stdout or "").strip()}')
    out = (result.stdout or '') + (result.stderr or '' if stderr else '')
    return out.replace('\r\n', '\n')


def shell(command, check=True, timeout=None):
    return adb('shell', command, check=check, timeout=timeout)


def create(args):
    image = os.path.join(sdk_root(), IMAGE)
    if not os.path.exists(os.path.join(image, 'system.img')):
        sys.exit(f'{image} is missing; install system-images;android-34;google_apis;x86_64')
    content = os.path.join(avd_home(), AVD + '.avd')
    if os.path.exists(content) and not args.force:
        sys.exit(f'{content} already exists (use --force to recreate it)')
    shutil.rmtree(content, ignore_errors=True)
    os.makedirs(content)
    with open(os.path.join(avd_home(), AVD + '.ini'), 'w', newline='\n') as f:
        f.write(f'avd.ini.encoding=UTF-8\npath={content}\npath.rel=avd\\{AVD}.avd\ntarget=android-34\n')
    config = {
        'avd.ini.encoding': 'UTF-8', 'AvdId': AVD, 'avd.ini.displayname': 'Prism (Horizon OS)',
        'image.sysdir.1': IMAGE.replace('/', '\\') + '\\', 'tag.id': 'google_apis', 'tag.display': 'Google APIs',
        'abi.type': 'x86_64', 'hw.cpu.arch': 'x86_64', 'hw.cpu.ncore': str(args.cores),
        'hw.ramSize': str(args.memory), 'vm.heapSize': '512M', 'disk.dataPartition.size': '24G',
        'hw.gpu.enabled': 'yes', 'hw.gpu.mode': 'host', 'hw.gltransport': 'asg',
        # A landscape 16:9 display, the shape a desktop window of Horizon will have: see DISPLAY.
        'hw.lcd.width': str(DISPLAY[0]), 'hw.lcd.height': str(DISPLAY[1]), 'hw.lcd.density': str(density(DISPLAY[1])),
        # At 72 Hz only every other frame (36 a second) reached the viewer: see DISPLAY_HZ.
        'hw.lcd.vsync': str(DISPLAY_HZ),
        'hw.initialOrientation': 'landscape', 'hw.keyboard': 'yes', 'hw.mainKeys': 'no',
        'hw.audioInput': 'yes', 'hw.audioOutput': 'yes', 'hw.sdCard': 'no', 'PlayStore.enabled': 'no',
        'fastboot.forceColdBoot': 'yes', 'showDeviceFrame': 'no', 'hw.useext4': 'yes',
    }
    with open(os.path.join(content, 'config.ini'), 'w', newline='\n') as f:
        f.writelines(f'{k}={v}\n' for k, v in config.items())
    print(f'created {AVD} in {content}')


def listed():
    out = subprocess.run([adb_path(), 'devices'], capture_output=True, text=True).stdout
    return any(line.startswith(SERIAL) for line in out.splitlines())


def running():
    """Whether the emulator is up. An adb server started after the emulator finds emulators only on
    ports up to 5585, so it's told about this one, as the emulator itself does when it starts."""
    if listed():
        return True
    try:
        socket.create_connection(('127.0.0.1', PORT + 1), timeout=1).close()
    except OSError:
        return False
    try:
        with socket.create_connection(('127.0.0.1', 5037), timeout=5) as s:
            request = f'host:emulator:{PORT + 1}'.encode()
            s.sendall(b'%04x' % len(request) + request)
            s.recv(16)
    except OSError:
        return False
    time.sleep(1)
    return listed()


def running_avd():
    """The name of the AVD running on Prism's port (`adb emu avd name`), or None."""
    if not running():
        return None
    try:
        out = adb('emu', 'avd', 'name', check=False, timeout=10)
    except subprocess.TimeoutExpired:
        return None
    return next((line.strip() for line in out.splitlines() if line.strip() and line.strip() != 'OK'), None)


def config_path():
    return os.path.join(avd_home(), AVD + '.avd', 'config.ini')


def avd_config():
    try:
        with open(config_path()) as f:
            return dict(line.rstrip('\n').split('=', 1) for line in f if '=' in line)
    except OSError:
        return {}


def refresh_rate():
    """The AVD's display refresh rate in Hz: what deploy sets the compositor and apps to."""
    try:
        return int(avd_config().get('hw.lcd.vsync', DISPLAY_HZ))
    except ValueError:
        return DISPLAY_HZ


def cores():
    try:
        return int(avd_config().get('hw.cpu.ncore', '1'))
    except ValueError:
        return 1


def display():
    """The AVD's display size (width, height)."""
    config = avd_config()
    try:
        return int(config['hw.lcd.width']), int(config['hw.lcd.height'])
    except (KeyError, ValueError):
        return DISPLAY


def configure(args):
    """Changes the existing AVD's display, memory or CPUs from its next start; with none given,
    prints them. The emulator's frame mapping keeps the size it was made with while anything has it
    open, so tools/viewer.py and tools/steamvr.py are closed before it restarts at another size."""
    config = avd_config()
    if not config:
        sys.exit(f'{config_path()} is missing; run create first')
    changes = {}
    if args.display:
        try:
            width, height = (int(n) for n in args.display.lower().split('x'))
        except ValueError:
            sys.exit(f'--display takes WIDTHxHEIGHT, not {args.display!r}')
        if width * 9 != height * 16:
            print(f'warning: {width}x{height} is not 16:9, which the fitted view edges (kViews) assume')
        changes.update({'hw.lcd.width': str(width), 'hw.lcd.height': str(height),
                        'hw.lcd.density': str(density(height))})
    if args.refresh:
        changes['hw.lcd.vsync'] = str(args.refresh)
    if args.memory:
        changes['hw.ramSize'] = str(args.memory)
    if args.cores:
        changes['hw.cpu.ncore'] = str(args.cores)
    if not changes:
        print(f'display {config.get("hw.lcd.width")}x{config.get("hw.lcd.height")} at {refresh_rate()} Hz '
              f'(density {config.get("hw.lcd.density")}), memory {config.get("hw.ramSize")} MB, '
              f'{config.get("hw.cpu.ncore")} cores')
        return
    config.update(changes)
    with open(config_path(), 'w', newline='\n') as f:
        f.writelines(f'{k}={v}\n' for k, v in config.items())
    print('set ' + ', '.join(f'{k}={v}' for k, v in changes.items()))
    if args.refresh:
        print('the compositor and apps take the rate from deploy: run python tools/deploy.py after the restart')
    if running():
        if args.display:
            # Deploy's frontbufferHeight follows the display; set it now so the restart needs no deploy.
            root(argparse.Namespace(timeout=300))
            shell(f"sed -i 's/^{FRONTBUFFER}=.*/{FRONTBUFFER}={height}/' {PRODUCT_PROPS}", check=False)
        print('from the next start: close the viewer and prism_vr, then '
              'python tools/emulator.py stop, python tools/emulator.py start --wait')


# The emulator's frame sharing (-share-vid, VideoFrameSharer) skips a frame that comes within 1/fps of
# the last, and its constructor starts fps at 60: `mov dword ptr [rcx + 8], 60`, the field VideoInfo
# also reports. At 72 Hz only every other frame (36 a second) got through. Patching that immediate is
# the whole change, made only where the instructions around it match (emulator 37.1.11), in both
# builds; the SDK's originals are kept beside them as <name>.orig. Nothing of the emulator is in the repo.
SHARER_BINARIES = ('qemu-system-x86_64.exe', 'qemu-system-x86_64-headless.exe')
SHARER_BEFORE = bytes.fromhex('44894104c74108')  # mov [rcx + 4], r8d; mov dword ptr [rcx + 8], imm32
SHARER_AFTER = bytes.fromhex('0f57c0')  # xorps xmm0, xmm0


def sharer_path(name):
    return os.path.join(sdk_root(), 'emulator', 'qemu', 'windows-x86_64', name)


def sharer_site(data):
    """The offset of the rate's immediate in a binary's image, or None unless exactly one place fits."""
    pattern = re.escape(SHARER_BEFORE) + rb'[\x01-\xff]\x00\x00\x00' + re.escape(SHARER_AFTER)
    sites = [m.start() + len(SHARER_BEFORE) for m in re.finditer(pattern, bytes(data), re.DOTALL)]
    return sites[0] if len(sites) == 1 else None


def patch_share_vid(args):
    """Sets the most frames a second the emulator shares (the AVD's refresh rate, or --rate); --undo
    restores the installed builds. The emulator has to be stopped: Windows locks a running executable."""
    if running():
        sys.exit('stop the emulator first (python tools/emulator.py stop)')
    rate = args.rate or refresh_rate()
    if not 1 <= rate <= 255:
        sys.exit('the rate has to fit in one byte (1-255)')
    for name in SHARER_BINARIES:
        path = sharer_path(name)
        if args.undo:
            if os.path.exists(path + '.orig'):
                shutil.copyfile(path + '.orig', path)
                print(f'{name}: restored')
            else:
                print(f'{name}: nothing to restore')
            continue
        with open(path, 'rb') as f:
            data = bytearray(f.read())
        site = sharer_site(data)
        if site is None:
            sys.exit(f'{name}: not a build whose frame sharing this knows (emulator 37.1.11 is); nothing changed')
        if data[site] == rate:
            print(f'{name}: already shares up to {rate} frames a second')
            continue
        if not os.path.exists(path + '.orig'):
            shutil.copyfile(path, path + '.orig')
        was, data[site] = data[site], rate
        with open(path, 'wb') as f:
            f.write(data)
        print(f'{name}: shares up to {rate} frames a second (was {was})')


def start(args):
    if running():
        other = running_avd()
        if other and other != AVD:
            sys.exit(f'{SERIAL} is running another AVD ({other}); stop it first (python tools/emulator.py stop)')
        print(f'{SERIAL} is already running')
        return
    os.makedirs(LOG_DIR, exist_ok=True)
    log = open(os.path.join(LOG_DIR, 'emulator.log'), 'w')
    command = [os.path.join(sdk_root(), 'emulator', 'emulator.exe' if os.name == 'nt' else 'emulator'),
               '-avd', AVD, '-port', str(PORT), '-writable-system', '-selinux', 'permissive', '-no-snapshot',
               '-no-boot-anim', '-gpu', 'host', '-crash-report-mode', 'never',
               '-share-vid',  # the display's frames in shared memory, for tools/viewer.py
               # The composer HAL waits out each frame's composition on the PC (rcCompose) by spinning
               # on its ring, 8-9 ms of a 16.7 ms frame already; with panels redrawing it ran over and
               # the display (and the viewer) lost frames while the compositor kept 60. With this the
               # HAL composes asynchronously (rcComposeAsync).
               '-feature', 'AsyncComposeSupport']
    # The emulator's own window composites every frame on the host; at 90 Hz that cost the guest half
    # its frames (the viewer reads the shared frames, so nothing needs the window).
    if args.headless or (refresh_rate() > 60 and not args.window):
        command.append('-no-window')
    if args.wipe:
        command.append('-wipe-data')
    # Under the Windows Hypervisor Platform the emulator overrides hw.cpu.ncore with one vCPU ("Not
    # all modern X86 virtualization features supported"), and on one CPU Horizon starves: a Navigator
    # panel's two coroutine workers block on account-only requests and its library never loads.
    # qemu's own -smp comes after the emulator's and wins. Must be last: -qemu takes the rest; its
    # -append is added to the kernel command line the emulator makes.
    qemu = ['-smp', str(cores())] if cores() > 1 else []
    qemu += ['-append', kernel_args(args)]
    command += ['-qemu', *qemu]
    if os.name == 'nt' and not args.any_core:
        pin_to_performance_cores()
    flags = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS if os.name == 'nt' else 0
    try:
        # Out of any job we're in too: a launcher (SteamVR, for prism.py vr --from-steamvr) may end
        # its app's whole job when the app quits, and the emulator outlives the app.
        subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                         creationflags=flags | getattr(subprocess, 'CREATE_BREAKAWAY_FROM_JOB', 0))
    except PermissionError:  # a job that allows no breakaway
        subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
    print(f'starting {AVD} as {SERIAL} (log: {log.name})')
    if args.wait and wait(args):
        ensure_tsc(args)


def kernel_args(args):
    """The guest kernel's extra parameters.

    tsc=nowatchdog: under WHPX slow HPET reads look like TSC drift to the clocksource watchdog,
    which then drops the TSC for HPET mid-session; every HPET read is an exit to qemu (about 9 us,
    against 17 ns for the TSC). tsc=reliable would also skip the boot-time correction of the vCPUs'
    TSC offsets, and the guest hangs. idle=poll: an idle vCPU spins instead of halting, so waking a
    thread on it doesn't need an IPI through qemu's userspace APIC (some 300 us under WHPX), at the
    cost of a host thread busy per vCPU. Opt-in (--idle-poll): Horizon's home holds 71-72 fps of 72
    without it."""
    params = ['tsc=nowatchdog']
    if args.idle_poll:
        params.append('idle=poll')
    return ' '.join(params)


def pin_to_performance_cores():
    """Restricts this process, and so the emulator and qemu it starts, to the CPU's performance
    cores (on a hybrid CPU, those of the highest efficiency class; all cores otherwise), so Windows
    doesn't move vCPU threads to efficiency cores, as it does while the window is minimized."""
    import ctypes
    from ctypes import wintypes
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    length = wintypes.DWORD(0)
    kernel32.GetLogicalProcessorInformationEx(0, None, ctypes.byref(length))  # 0: RelationProcessorCore
    buffer = ctypes.create_string_buffer(length.value)
    if not kernel32.GetLogicalProcessorInformationEx(0, buffer, ctypes.byref(length)):
        return
    mask, best, offset = 0, -1, 0
    while offset < length.value:
        # SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX: Size at 4; PROCESSOR_RELATIONSHIP's
        # EfficiencyClass at 9 and its first GROUP_AFFINITY's mask at 32.
        size = int.from_bytes(buffer.raw[offset + 4:offset + 8], 'little')
        efficiency = buffer.raw[offset + 9]
        core = int.from_bytes(buffer.raw[offset + 32:offset + 40], 'little')
        if efficiency > best:
            mask, best = 0, efficiency
        if efficiency == best:
            mask |= core
        offset += size
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    kernel32.SetProcessAffinityMask.argtypes = (wintypes.HANDLE, ctypes.c_size_t)
    if mask and kernel32.SetProcessAffinityMask(kernel32.GetCurrentProcess(), mask):
        print(f'emulator pinned to performance cores (mask {mask:#x})')


CLOCKSOURCE = '/sys/devices/system/clocksource/clocksource0/current_clocksource'


def clocksource():
    adb('root', check=False)
    adb('wait-for-device')
    return shell(f'cat {CLOCKSOURCE}', check=False).strip()


def ensure_tsc(args, attempts=6):
    """Reboots Android until its clock is the TSC. At boot the kernel measures and corrects the
    TSC offsets between vCPUs; if jitter remains it gives up on the TSC for the whole boot and uses
    HPET, which costs every time read an exit to qemu. Whether a boot passes is luck."""
    for attempt in range(attempts + 1):
        clock = clocksource()
        if clock == 'tsc':
            print(f'clocksource is tsc{f" after {attempt} reboot(s)" if attempt else ""}')
            return True
        if attempt == attempts:
            break
        print(f'clocksource is {clock} (the TSC failed the boot check); rebooting ({attempt + 1}/{attempts})')
        adb('reboot', check=False)
        time.sleep(5)
        adb('wait-for-device')
        args.timeout = max(args.timeout, 300)
        if not wait(args):
            return False
    print(f'clocksource is still {clock}; time reads exit to qemu and everything runs slower')
    return False


def wait(args):
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        if running():
            try:
                if shell('getprop sys.boot_completed', check=False, timeout=10).strip() == '1':
                    print('boot completed')
                    return True
            except subprocess.TimeoutExpired:
                pass
        time.sleep(3)
    print(f'boot did not complete within {args.timeout}s')
    return False


SHELL_PROCESS = 'com.oculus.vrshell'  # VrShell, which draws Horizon's home


def stage():
    """How far Horizon has got, as (stage, detail): 'stopped'; 'booting' (Android); 'clock' (booted
    on HPET, not the TSC: everything runs slower); 'starting' (VrShell not up yet, or the compositor
    not yet sharing frames at half the display's rate); 'ready'. One adb call and a second's look
    at the shared frames, so it can be polled."""
    if not running():
        return 'stopped', ''
    try:
        out = shell(f'echo "$(getprop sys.boot_completed)|$(cat {CLOCKSOURCE})|$(pidof {SHELL_PROCESS})"',
                    check=False, timeout=10).strip()
    except subprocess.TimeoutExpired:
        return 'booting', 'adb is not answering yet'
    booted, clock, shell_pid = (out.split('|') + ['', '', ''])[:3]
    if booted != '1':
        return 'booting', ''
    if clock != 'tsc':
        return 'clock', f'the clock is {clock or "unknown"}'
    if not shell_pid:
        return 'starting', 'VrShell is not up yet'
    from perf import frame_rate  # perf imports this module
    fps = frame_rate(1.0)
    if fps is None:
        return 'starting', 'no shared frames (the emulator runs without -share-vid?)'
    if fps < refresh_rate() / 2:
        return 'starting', f'{fps:.0f} frames a second'
    return 'ready', f'{fps:.0f} frames a second'


def ready(args):
    """Waits until Horizon's home is drawing (stage() 'ready'), rebooting onto the TSC as ensure_tsc
    does, and printing each stage as it's reached. False after args.timeout seconds."""
    deadline = time.time() + args.timeout
    shown = None
    while time.time() < deadline:
        now, detail = stage()
        line = f'{now}: {detail}' if detail else now
        # A frame rate that is still climbing is not news; anything else is.
        climbing = shown and shown.startswith(now + ':') and 'frames a second' in shown and 'frames a second' in detail
        if line != shown and not climbing:
            print(line, flush=True)
            shown = line
        if now == 'ready':
            return True
        if now == 'clock' and not ensure_tsc(args):
            return False
        time.sleep(2)
    print(f'Horizon was not ready within {args.timeout}s ({shown})')
    return False


def root(args):
    adb('root')
    adb('wait-for-device')
    out = adb('remount', check=False, stderr=True)
    if 'reboot' in out.lower():  # first remount only disables verity; the next boot is writable
        print('verity disabled; rebooting once so the system partitions become writable')
        adb('reboot')
        adb('wait-for-device')
        # Deploy passes its own arguments, which have no timeout; only a new AVD gets here.
        args.timeout = max(getattr(args, 'timeout', 0), 300)
        wait(args)
        adb('root')
        adb('wait-for-device')
        out = adb('remount', stderr=True)
    print(out.strip().splitlines()[-1] if out.strip() else 'remounted')


def status(args):
    """One look at how far Horizon got: boot state, key processes, services, the latest crash."""
    adb('root', check=False)
    adb('wait-for-device')
    script = ('echo "boot_completed=$(getprop sys.boot_completed) zygote=$(pidof zygote64) '
              'system_server=$(pidof system_server)"; '
              'echo "services=$(service list 2>/dev/null | grep -c "^[0-9]") '
              f'clocksource=$(cat {CLOCKSOURCE})"; '
              'logcat -d -b crash -t 200 | grep -E "FATAL EXCEPTION|Process:|Exception|Error|^.{33}\\s+at " | tail -%d'
              % args.lines)
    print(shell(script, check=False).strip())


# ro.kernel.qemu=1 (the emulator's /vendor/build.prop) is how Meta's runtime knows its emulator; deploy
# hides it by overriding it from /product/etc/build.prop, which loads last. A read-only property
# changes only with a reboot.
FLAG = 'ro.kernel.qemu'
PRODUCT_PROPS = '/product/etc/build.prop'
FRONTBUFFER = 'debug.oculus.frontbufferHeight'  # deploy sets it to the display's height


def emulator_flag(args):
    if args.state is None:
        value = shell(f'getprop {FLAG}', check=False).strip()
        print(f'{FLAG}={value!r}: {"shown" if value == "1" else "hidden"}')
        return
    root(argparse.Namespace(timeout=300))
    line = f'{FLAG}=0'
    script = f"sed -i '/^{FLAG}=/d' {PRODUCT_PROPS}"
    if args.state == 'hidden':
        script += f" && echo '{line}' >> {PRODUCT_PROPS}"
    shell(script)
    print(f'{FLAG} {args.state} from the next boot; rebooting')
    adb('reboot')


# Build.MODEL. Deploy writes Horizon's ("Quest 3"); Meta's runtime knows Quest 3 by its codename,
# Eureka, and without it takes the emulator's device type. A later line of build.prop wins.
MODEL = 'ro.product.product.model'
CODENAME = 'Eureka'


def model(args):
    if args.state is None:
        value = shell('getprop ro.product.model', check=False).strip()
        print(f'ro.product.model={value!r}: {"eureka" if value == CODENAME else "quest3"}')
        return
    root(argparse.Namespace(timeout=300))
    script = f"sed -i '/^{MODEL}={CODENAME}$/d' {PRODUCT_PROPS}"
    if args.state == 'eureka':
        script += f" && echo '{MODEL}={CODENAME}' >> {PRODUCT_PROPS}"
    shell(script)
    print(f'model {args.state} from the next boot; rebooting')
    adb('reboot')


def stop(_args):
    """Stops the emulator and returns once it's gone: `emu kill` returns while it's still shutting
    down, and what comes next (another start, a config change, patching its locked binaries) needs
    it gone."""
    if running():
        adb('shell', 'sync', check=False, timeout=30)  # a kill drops what's still in the guest's page cache
        adb('emu', 'kill', check=False)
        deadline = time.time() + 60
        while running() and time.time() < deadline:
            time.sleep(1)
        print('stopped' if not running() else f'{SERIAL} is still shutting down after 60 s')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    c = sub.add_parser('create')
    c.add_argument('--force', action='store_true')
    c.add_argument('--cores', type=int, default=6)
    c.add_argument('--memory', type=int, default=12288)  # with 8192, Horizon's home had 3 GB in swap
    cf = sub.add_parser('config')
    cf.add_argument('--display', metavar='WxH', help=f'e.g. {DISPLAY[0]}x{DISPLAY[1]} (16:9)')
    cf.add_argument('--refresh', type=int, metavar='HZ', help='the display refresh rate; deploy again after')
    cf.add_argument('--memory', type=int, metavar='MB')
    cf.add_argument('--cores', type=int)
    ps = sub.add_parser('patch-share-vid')
    ps.add_argument('--rate', type=int, help="frames a second (default: the AVD's refresh rate)")
    ps.add_argument('--undo', action='store_true', help='restore the emulator builds as installed')
    s = sub.add_parser('start')
    s.add_argument('--headless', action='store_true', help='no emulator window (the default above 60 Hz)')
    s.add_argument('--window', action='store_true', help='keep the emulator window above 60 Hz too')
    s.add_argument('--wipe', action='store_true', help='reset /data on this boot')
    s.add_argument('--wait', action='store_true', help='until boot completes, then until the clock is the TSC')
    s.add_argument('--idle-poll', action='store_true', help='idle vCPUs spin instead of halting (faster wake-ups, a busy host thread per vCPU)')
    s.add_argument('--any-core', action='store_true', help="don't pin the emulator to the performance cores")
    for p in (s, sub.add_parser('wait'), sub.add_parser('root'), sub.add_parser('clock')):
        p.add_argument('--timeout', type=int, default=300)
    sub.add_parser('ready').add_argument('--timeout', type=int, default=600)
    st = sub.add_parser('status')
    st.add_argument('--lines', type=int, default=25)
    sub.add_parser('stop')
    f = sub.add_parser('emulator-flag')
    f.add_argument('state', nargs='?', choices=('hidden', 'shown'), help='omit to print the current state')
    m = sub.add_parser('model')
    m.add_argument('state', nargs='?', choices=('quest3', 'eureka'), help='omit to print the current state')
    a = sub.add_parser('adb')
    a.add_argument('rest', nargs=argparse.REMAINDER)
    args = ap.parse_args()
    if args.command == 'adb':
        subprocess.run([adb_path(), '-s', SERIAL, *args.rest])
        return
    if args.command == 'ready':
        sys.exit(0 if ready(args) else 1)
    {'create': create, 'config': configure, 'start': start, 'wait': wait, 'root': root, 'status': status, 'clock': ensure_tsc,
     'stop': stop, 'patch-share-vid': patch_share_vid, 'emulator-flag': emulator_flag, 'model': model}[args.command](args)


if __name__ == '__main__':
    main()
