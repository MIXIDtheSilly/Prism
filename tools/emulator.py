"""Prism's Android Emulator: create the AVD, start/stop it, and run adb against it.

Prism uses its own AVD (prism-api34) on its own port, so it never touches other AVDs.

    python tools/emulator.py create            # write the AVD (stock API 34 x86_64 image)
    python tools/emulator.py start [--headless]  # boot it in a window: writable system, SELinux permissive
    python tools/emulator.py wait              # until Android reports boot completed
    python tools/emulator.py root              # adb root + make /system, /system_ext, /product writable
    python tools/emulator.py status            # boot state, key processes, services, latest crash
    python tools/emulator.py emulator-flag [hidden|shown]  # ro.kernel.qemu for Meta's runtime; reboots
    python tools/emulator.py model [quest3|eureka]         # Build.MODEL; reboots
    python tools/emulator.py stop
    python tools/emulator.py adb <args...>
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import time

AVD = 'prism-api34'
PORT = 5590
SERIAL = f'emulator-{PORT}'
IMAGE = os.path.join('system-images', 'android-34', 'google_apis', 'x86_64')
LOG_DIR = os.path.join('work', 'logs')


def sdk_root():
    for candidate in (os.environ.get('ANDROID_SDK_ROOT'), os.environ.get('ANDROID_HOME'),
                      os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk'),
                      os.path.expanduser('~/Android/Sdk')):
        if candidate and os.path.isdir(os.path.join(candidate, 'emulator')):
            return candidate
    sys.exit('Android SDK not found; set ANDROID_SDK_ROOT')


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
        # A landscape 1080p display, the shape a desktop window of Horizon will have.
        'hw.lcd.width': '1920', 'hw.lcd.height': '1080', 'hw.lcd.density': '320',
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


def cores():
    try:
        with open(os.path.join(avd_home(), AVD + '.avd', 'config.ini')) as f:
            config = dict(line.rstrip('\n').split('=', 1) for line in f if '=' in line)
        return int(config.get('hw.cpu.ncore', '1'))
    except (OSError, ValueError):
        return 1


def start(args):
    if running():
        print(f'{SERIAL} is already running')
        return
    os.makedirs(LOG_DIR, exist_ok=True)
    log = open(os.path.join(LOG_DIR, 'emulator.log'), 'w')
    command = [os.path.join(sdk_root(), 'emulator', 'emulator.exe' if os.name == 'nt' else 'emulator'),
               '-avd', AVD, '-port', str(PORT), '-writable-system', '-selinux', 'permissive', '-no-snapshot',
               '-no-boot-anim', '-gpu', 'host', '-crash-report-mode', 'never',
               '-share-vid']  # the display's frames in shared memory, for tools/viewer.py
    if args.headless:
        command.append('-no-window')
    if args.wipe:
        command.append('-wipe-data')
    # Under the Windows Hypervisor Platform the emulator overrides hw.cpu.ncore with one vCPU ("Not
    # all modern X86 virtualization features supported"), and on one CPU Horizon starves: a Navigator
    # panel's two coroutine workers block on account-only requests and its library never loads.
    # qemu's own -smp comes after the emulator's and wins. Must be last: -qemu takes the rest.
    if cores() > 1:
        command += ['-qemu', '-smp', str(cores())]
    flags = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS if os.name == 'nt' else 0
    subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
    print(f'starting {AVD} as {SERIAL} (log: {log.name})')
    if args.wait:
        wait(args)


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


def root(args):
    adb('root')
    adb('wait-for-device')
    out = adb('remount', check=False, stderr=True)
    if 'reboot' in out.lower():  # first remount only disables verity; the next boot is writable
        print('verity disabled; rebooting once so the system partitions become writable')
        adb('reboot')
        adb('wait-for-device')
        args.timeout = max(args.timeout, 300)
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
              'echo "services=$(service list 2>/dev/null | grep -c "^[0-9]")"; '
              'logcat -d -b crash -t 200 | grep -E "FATAL EXCEPTION|Process:|Exception|Error|^.{33}\\s+at " | tail -%d'
              % args.lines)
    print(shell(script, check=False).strip())


# ro.kernel.qemu=1 (the emulator's /vendor/build.prop) is how Meta's runtime knows its emulator; deploy
# hides it by overriding it from /product/etc/build.prop, which loads last. A read-only property
# changes only with a reboot.
FLAG = 'ro.kernel.qemu'
PRODUCT_PROPS = '/product/etc/build.prop'


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
    if running():
        adb('shell', 'sync', check=False, timeout=30)  # a kill drops what's still in the guest's page cache
        adb('emu', 'kill', check=False)
        print('stopped')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    c = sub.add_parser('create')
    c.add_argument('--force', action='store_true')
    c.add_argument('--cores', type=int, default=6)
    c.add_argument('--memory', type=int, default=8192)
    s = sub.add_parser('start')
    s.add_argument('--headless', action='store_true', help='no emulator window')
    s.add_argument('--wipe', action='store_true', help='reset /data on this boot')
    s.add_argument('--wait', action='store_true')
    for p in (s, sub.add_parser('wait'), sub.add_parser('root')):
        p.add_argument('--timeout', type=int, default=300)
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
    {'create': create, 'start': start, 'wait': wait, 'root': root, 'status': status,
     'stop': stop, 'emulator-flag': emulator_flag, 'model': model}[args.command](args)


if __name__ == '__main__':
    main()
