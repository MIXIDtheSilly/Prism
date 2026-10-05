"""Build Prism's GLES layer (native/gles_prism): GL_EXT_memory_object_fd for the emulator's GLES, so
OpenGL ES apps can use Horizon's runtime's swapchains.

    work/build/gles/libgles_prism.so   installed in /system_ext/lib64, copied at boot to
                                       /data/local/debug/gles (where Android's EGL loader takes
                                       layers from); debug.gles.layers=libgles_prism.so

    python tools/gles.py [--ndk PATH]
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'jni'))
from build import API, find_ndk  # noqa: E402

SOURCE = os.path.join(ROOT, 'native', 'gles_prism', 'gles_prism.c')
LIBRARY = 'libgles_prism.so'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ndk')
    ap.add_argument('--out', default=os.path.join('work', 'build', 'gles'))
    args = ap.parse_args()
    ndk = find_ndk(args.ndk)
    host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
    clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host, 'bin', 'clang' + ('.exe' if os.name == 'nt' else ''))
    os.makedirs(args.out, exist_ok=True)
    out = os.path.join(args.out, LIBRARY)
    subprocess.run([clang, f'--target=x86_64-linux-android{API}', '-shared', '-fPIC', '-O2', '-std=c11', '-Wall',
                    '-fvisibility=hidden', SOURCE, '-o', out, f'-Wl,-soname,{LIBRARY}',
                    '-lEGL', '-lGLESv3', '-lnativewindow', '-llog', '-Wl,--build-id=sha1'], check=True)
    print(f'built {out}')


if __name__ == '__main__':
    main()
