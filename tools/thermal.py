"""Build Prism's thermal HAL (native/thermal_prism): the stable-AIDL IThermal Horizon's vrdevice
requires, which the emulator lacks (it has only the HIDL thermal@2.0 mock).

    work/build/thermal/android.hardware.thermal-service.prism   installed in /vendor/bin/hw

    python tools/thermal.py [--ndk PATH]
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'jni'))
from build import API, find_ndk  # noqa: E402

SOURCE = os.path.join(ROOT, 'native', 'thermal_prism', 'thermal_prism.c')
BINARY = 'android.hardware.thermal-service.prism'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ndk')
    ap.add_argument('--out', default=os.path.join('work', 'build', 'thermal'))
    args = ap.parse_args()
    ndk = find_ndk(args.ndk)
    host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
    clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host, 'bin', 'clang' + ('.exe' if os.name == 'nt' else ''))
    os.makedirs(args.out, exist_ok=True)
    out = os.path.join(args.out, BINARY)
    subprocess.run([clang, f'--target=x86_64-linux-android{API}', '-fPIE', '-pie', '-O2', '-std=c11', '-Wall',
                    SOURCE, '-o', out, '-lbinder_ndk', '-llog', '-ldl', '-Wl,--build-id=sha1'], check=True)
    print(f'built {out}')


if __name__ == '__main__':
    main()
