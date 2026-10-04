"""Build Prism's arm64 media functions (native/guest_media): functions Meta's libmediandk has and
Android's doesn't, for arm64 code. libprism_jni preloads the library into apps, globally.

    work/build/guest_media/libprism_guest_media.so   installed with the guest libraries

    python tools/guest_media.py [--ndk PATH]
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'jni'))
from build import API, find_ndk  # noqa: E402

SOURCE = os.path.join(ROOT, 'native', 'guest_media', 'guest_media.c')
LIBRARY = 'libprism_guest_media.so'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ndk')
    ap.add_argument('--out', default=os.path.join('work', 'build', 'guest_media'))
    args = ap.parse_args()
    ndk = find_ndk(args.ndk)
    host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
    clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host, 'bin', 'clang' + ('.exe' if os.name == 'nt' else ''))
    os.makedirs(args.out, exist_ok=True)
    out = os.path.join(args.out, LIBRARY)
    # -z global (DF_1_GLOBAL): the linker shares it into the namespaces made after it, where dlsym's
    # RTLD_DEFAULT finds it; RTLD_GLOBAL alone keeps it to its own. No DT_NEEDED on libmediandk:
    # preloaded into every app, it opens that only when called.
    subprocess.run([clang, f'--target=aarch64-linux-android{API}', '-fPIC', '-shared', '-O2', '-std=c11', '-Wall',
                    f'-Wl,-soname,{LIBRARY}', '-Wl,-z,global', SOURCE, '-o', out, '-ldl', '-Wl,--build-id=sha1'],
                   check=True)
    print(f'built {out}')


if __name__ == '__main__':
    main()
