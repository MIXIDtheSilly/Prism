"""Build Prism's Vulkan driver (native/vulkan_prism): the emulator's gfxstream driver with
VK_KHR_external_memory_fd on top, which Horizon's compositor requires.

    work/build/vulkan/vulkan.prism.so   installed in /vendor/lib64/hw; ro.hardware.vulkan=prism

    python tools/vulkan.py [--ndk PATH]
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'jni'))
from build import API, find_ndk  # noqa: E402

SOURCE = os.path.join(ROOT, 'native', 'vulkan_prism', 'vulkan_prism.c')
LIBRARY = 'vulkan.prism.so'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ndk')
    ap.add_argument('--out', default=os.path.join('work', 'build', 'vulkan'))
    args = ap.parse_args()
    ndk = find_ndk(args.ndk)
    host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
    clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host, 'bin', 'clang' + ('.exe' if os.name == 'nt' else ''))
    os.makedirs(args.out, exist_ok=True)
    out = os.path.join(args.out, LIBRARY)
    subprocess.run([clang, f'--target=x86_64-linux-android{API}', '-shared', '-fPIC', '-O2', '-std=c11', '-Wall',
                    '-fvisibility=hidden', SOURCE, '-o', out, f'-Wl,-soname,{LIBRARY}',
                    '-lnativewindow', '-llog', '-ldl', '-Wl,--build-id=sha1'], check=True)
    print(f'built {out}')


if __name__ == '__main__':
    main()
