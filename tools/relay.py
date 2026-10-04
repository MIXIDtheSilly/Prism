"""Build Prism's binder bridge (native/binder_relay): binder objects cross between an app's arm64
libbinder and the host's Java binder, separate /dev/binder connections, through a relay.

    work/build/relay/prism_binder_relay          the relay (x86_64), installed in /system_ext/bin
    work/build/relay/libprism_binder_bridge.so   its arm64 client, which libprism_jni preloads into
                                                 apps' arm64 code: installed in the guest libraries

    python tools/relay.py [--ndk PATH]
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'jni'))
from build import API, find_ndk  # noqa: E402

NATIVE = os.path.join(ROOT, 'native', 'binder_relay')
RELAY = 'prism_binder_relay'
BRIDGE = 'libprism_binder_bridge.so'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ndk')
    ap.add_argument('--out', default=os.path.join('work', 'build', 'relay'))
    args = ap.parse_args()
    ndk = find_ndk(args.ndk)
    host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
    clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host, 'bin', 'clang' + ('.exe' if os.name == 'nt' else ''))
    os.makedirs(args.out, exist_ok=True)
    common = ['-O2', '-std=c11', '-Wall', '-Wl,--build-id=sha1']
    for name, target, source, flags in (
            (RELAY, 'x86_64', 'binder_relay.c', ['-fPIE', '-pie']),
            # links Horizon's libbinder_ndk at run time: the arm64 one already loaded
            (BRIDGE, 'aarch64', 'guest_bridge.c', ['-fPIC', '-shared', f'-Wl,-soname,{BRIDGE}'])):
        out = os.path.join(args.out, name)
        subprocess.run([clang, f'--target={target}-linux-android{API}', *flags, *common, os.path.join(NATIVE, source),
                        '-o', out, '-lbinder_ndk', '-llog', '-ldl'], check=True)
        print(f'built {out}')


if __name__ == '__main__':
    main()
