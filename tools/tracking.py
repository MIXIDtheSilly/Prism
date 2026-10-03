"""Build Prism's tracking service (native/tracking_prism): hosts Meta's MemoryBroker and keeps the
head tracker's shared memory, which a headset's trackingservice fills from its cameras and IMU.
An arm64 binary: it loads Meta's arm64 libraries, and runs through the translator.

    work/build/tracking/prism_tracking   installed in /system_ext/bin

    python tools/tracking.py [--ndk PATH]
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'jni'))
from build import API, find_ndk  # noqa: E402

SOURCE = os.path.join(ROOT, 'native', 'tracking_prism', 'tracking_prism.c')
BINARY = 'prism_tracking'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ndk')
    ap.add_argument('--out', default=os.path.join('work', 'build', 'tracking'))
    args = ap.parse_args()
    ndk = find_ndk(args.ndk)
    host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
    clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host, 'bin', 'clang' + ('.exe' if os.name == 'nt' else ''))
    os.makedirs(args.out, exist_ok=True)
    out = os.path.join(args.out, BINARY)
    subprocess.run([clang, f'--target=aarch64-linux-android{API}', '-fPIE', '-pie', '-O2', '-std=c11', '-Wall',
                    SOURCE, '-o', out, '-llog', '-ldl', '-Wl,--build-id=sha1'], check=True)
    print(f'built {out}')


if __name__ == '__main__':
    main()
