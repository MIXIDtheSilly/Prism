"""Checks the lifted hit-testing (tools/lift_arm64.py, native/berberis_proxy_libshell) against
VrShell's own arm64 code, on Prism's emulator.

    python tools/lift_check.py record [--timeout 300]
    python tools/lift_check.py replay [--fuzz 6]

record: builds the proxy with native/berberis_proxy_libshell/check/record.c, installs it over the
deployed one, restarts VrShell and has it record 6000 calls into the lifted code (every register
in and out, every memory access, the basic blocks reached) once you start using it: open app
windows and move the controllers' rays over them, the floor and the sky meanwhile. Then puts the
deployed proxy back and restarts VrShell. The recording goes to work/build/lift-check.

replay: runs each recorded call again through the original arm64 code (translated, on the
emulator) on the same registers and memory and compares what it leaves, byte for byte. With
--fuzz N it also makes N variants of each call (check/fuzz.c: floats and shape types changed), runs
the lifted code on them and replays those too. It then lists the basic blocks no call reached.
Expected differences: a NaN an operation creates from non-NaN operands has the sign bit set
(x86's default NaN), and the blocks after a fatal assert, which neither side can run here.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import emulator  # noqa: E402
import lift_arm64  # noqa: E402
import translator  # noqa: E402

CHECK = os.path.join(ROOT, 'native', 'berberis_proxy_libshell', 'check')
OUT = os.path.join(ROOT, 'work', 'build', 'lift-check')
DEVICE = '/data/local/tmp/prism-lift'
VRSHELL_FILES = '/data/data/com.oculus.vrshell/files'
PROXY = '/system/lib64/' + translator.PROXY_LIBSHELL
WARNINGS = ['-Wall', '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-unused-label',
            '-Wno-misleading-indentation']


def clang():
    host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
    return os.path.join(translator.find_ndk(None), 'toolchains', 'llvm', 'prebuilt', host, 'bin',
                        'clang' + ('.exe' if os.name == 'nt' else ''))


def prepare():
    """libshell.so from the user's VrShell, and its lifted code; returns the library's bytes."""
    os.makedirs(OUT, exist_ok=True)
    with zipfile.ZipFile(translator.VRSHELL_APK) as apk:
        data = apk.read(translator.LIBSHELL)
    with open(os.path.join(OUT, 'libshell.so'), 'wb') as f:
        f.write(data)
    source, blocks = lift_arm64.lift_library(data, translator.LIBSHELL_LIFTED, translator.LIBSHELL_EXTERNS,
                                             f'VrShell.apk:{translator.LIBSHELL}')
    with open(os.path.join(OUT, 'lifted_libshell.c'), 'w', newline='\n') as f:
        f.write(source)
    with open(os.path.join(OUT, 'blocks.json'), 'w') as f:
        json.dump({f'{a:x}': [f'{b:x}' for b in bl] for a, bl in blocks.items()}, f)
    return data


def build(output, sources, target='x86_64', flags=()):
    lib_dir = os.path.join(ROOT, 'work', 'build', 'translator', 'system', 'lib64')
    command = [clang(), f'--target={target}-linux-android34', '-O2', *WARNINGS, *flags, *sources, '-o', output]
    if output.endswith('.so'):
        command += ['-shared', '-fPIC', f'-Wl,-soname,{translator.PROXY_LIBSHELL}', f'-L{lib_dir}',
                    '-Wl,--no-as-needed', '-lberberis_arm64', '-Wl,--allow-shlib-undefined']
    subprocess.run(command + ['-llog', '-lm'], check=True)


def restart_vrshell():
    emulator.shell('kill $(pidof com.oculus.vrshell)', check=False)
    for _ in range(60):
        time.sleep(1)
        if emulator.shell('pidof com.oculus.vrshell', check=False).strip():
            return
    sys.exit('VrShell did not come back')


def record(args):
    prepare()
    lifted = os.path.join(OUT, 'lifted_libshell.c')
    recording_proxy = os.path.join(OUT, translator.PROXY_LIBSHELL)
    build(recording_proxy, [translator.PROXY_LIBSHELL_SOURCE, lifted, os.path.join(CHECK, 'record.c')],
          flags=['-DLIFT_RECORD', '-include', os.path.join(CHECK, 'record.h')])
    emulator.adb('remount')
    emulator.shell(f'mkdir -p {DEVICE}; cp {PROXY} {DEVICE}/deployed.so')
    try:
        emulator.adb('push', recording_proxy, PROXY)
        emulator.shell(f'chmod 644 {PROXY}; restorecon {PROXY}; setprop debug.prism.lift_record 0; '
                       f'rm -f {VRSHELL_FILES}/lift.rec {VRSHELL_FILES}/lift.cov')
        restart_vrshell()
        time.sleep(15)
        emulator.shell('setprop debug.prism.lift_record 1')
        print('recording: open app windows and move the rays over them, the floor and the sky', flush=True)
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            if emulator.shell(f'ls {VRSHELL_FILES}/lift.cov', check=False).strip().endswith('lift.cov'):
                break
            time.sleep(2)
        else:
            sys.exit('VrShell recorded too few calls in time (are the rays moving over something?)')
        for name in ('lift.rec', 'lift.cov'):
            emulator.shell(f'cp {VRSHELL_FILES}/{name} {DEVICE}/{name}; chmod 644 {DEVICE}/{name}')
            emulator.adb('pull', f'{DEVICE}/{name}', os.path.join(OUT, name))
        print(f'recorded -> {os.path.join(OUT, "lift.rec")}')
    finally:
        emulator.shell(f'setprop debug.prism.lift_record 0; cp {DEVICE}/deployed.so {PROXY}; restorecon {PROXY}; '
                       f'rm -f {VRSHELL_FILES}/lift.rec {VRSHELL_FILES}/lift.cov')
        restart_vrshell()


def coverage(paths, blocks):
    reached = set()
    for path in paths:
        if os.path.exists(path):
            with open(path, 'rb') as f:
                data = f.read()
            reached |= set(struct.unpack(f'<{len(data) // 4}I', data))
    total = missing = 0
    for function, bl in blocks.items():
        lost = [b for b in bl if int(b, 16) not in reached]
        total += len(bl)
        missing += len(lost)
        print(f'  {function}: {len(bl) - len(lost)}/{len(bl)} blocks' + (f'; not reached: {" ".join(lost)}' if lost else ''))
    print(f'  {total - missing}/{total} basic blocks reached')


def replay(args):
    data = prepare()
    recording = os.path.join(OUT, 'lift.rec')
    if not os.path.exists(recording):
        sys.exit(f'no {recording}: run python tools/lift_check.py record first')
    lifted = os.path.join(OUT, 'lifted_libshell.c')
    build(os.path.join(OUT, 'replay'), [os.path.join(CHECK, 'replay.c'), os.path.join(CHECK, 'call.S')], target='aarch64')
    build(os.path.join(OUT, 'fuzz'), [os.path.join(CHECK, 'fuzz.c'), lifted, os.path.join(CHECK, 'record.c')],
          flags=['-DFUZZ', '-include', os.path.join(CHECK, 'record.h')])
    imports = lift_arm64.Elf(data).imports()
    got = [f'{name}={imports[name][1]:x}' for name in translator.LIBSHELL_EXTERNS if isinstance(name, str)]
    emulator.shell(f'mkdir -p {DEVICE}')
    for name in ('replay', 'fuzz', 'libshell.so', 'lift.rec'):
        emulator.adb('push', os.path.join(OUT, name), f'{DEVICE}/{name}')
    emulator.shell(f'chmod 755 {DEVICE}/replay {DEVICE}/fuzz')
    run = lambda command: print(emulator.shell(f'cd {DEVICE} && {command} 2>&1', check=False).rstrip())
    print('VrShell\'s calls:')
    run(f'./replay libshell.so lift.rec 0 {" ".join(got)}')
    covers = [os.path.join(OUT, 'lift.cov')]
    if args.fuzz:
        print(f'{args.fuzz} variants of each:')
        run(f'./fuzz libshell.so lift.rec fuzz.rec {args.fuzz} {int(time.time())} fuzz.cov')
        run(f'./replay libshell.so fuzz.rec 0 {" ".join(got)}')
        emulator.adb('pull', f'{DEVICE}/fuzz.cov', os.path.join(OUT, 'fuzz.cov'))
        emulator.shell(f'rm -f {DEVICE}/fuzz.rec')
        covers.append(os.path.join(OUT, 'fuzz.cov'))
    with open(os.path.join(OUT, 'blocks.json')) as f:
        blocks = json.load(f)
    print('coverage:')
    coverage(covers, blocks)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    r = sub.add_parser('record')
    r.add_argument('--timeout', type=int, default=300, help='seconds to wait for 6000 calls')
    p = sub.add_parser('replay')
    p.add_argument('--fuzz', type=int, default=6, help='variants of each recorded call (0: none)')
    args = ap.parse_args()
    {'record': record, 'replay': replay}[args.command](args)


if __name__ == '__main__':
    main()
