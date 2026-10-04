"""Build Prism's space service (native/spaces_prism): the client of Meta's space manager for
system_server, which can't load Meta's arm64 client library. An arm64 binary, run through the
translator.

    work/build/spaces/prism_spaces   installed in /system_ext/bin

    python tools/spaces.py [--ndk PATH]
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'jni'))
from build import API, find_ndk  # noqa: E402

SOURCE = os.path.join(ROOT, 'native', 'spaces_prism', 'spaces_prism.c')
BINARY = 'prism_spaces'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ndk')
    ap.add_argument('--out', default=os.path.join('work', 'build', 'spaces'))
    args = ap.parse_args()
    ndk = find_ndk(args.ndk)
    host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
    clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host, 'bin', 'clang' + ('.exe' if os.name == 'nt' else ''))
    os.makedirs(args.out, exist_ok=True)
    out = os.path.join(args.out, BINARY)
    # links Horizon's libbinder_ndk at run time; Meta's space library is opened by name
    subprocess.run([clang, f'--target=aarch64-linux-android{API}', '-fPIE', '-pie', '-O2', '-std=c11', '-Wall',
                    SOURCE, '-o', out, '-lbinder_ndk', '-llog', '-ldl', '-Wl,--build-id=sha1'], check=True)
    print(f'built {out}')


if __name__ == '__main__':
    main()
