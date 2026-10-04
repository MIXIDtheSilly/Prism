"""Build Prism's HIDL services (native/hidl_prism): HIDL interfaces Horizon's software uses that the
stock image doesn't serve.

    work/build/hidl/prism_controller   vendor.oculus.hardware.sensors@1.0::IControllerProvider, Meta's
                                       controller HAL, with two controllers paired
    work/build/hidl/prism_suspend      android.system.suspend@1.0::ISystemSuspend, backed by the AIDL
                                       system suspend service
    both installed in /system_ext/bin/hw

    python tools/hidl.py [--ndk PATH]

Both are arm64 daemons, run through the translator: their interfaces derive from those in Horizon's
generated libraries (vendor.oculus.hardware.sensors@1.0.so, android.system.suspend@1.0.so), whose
stubs serve them. They're C++ against AOSP's HIDL headers (Android 14), fetched into work/aosp the
first time (only the include directories), and link Horizon's own arm64 libraries from work/fs.
Those are built with the platform's libc++ (std::__1), not the NDK's (std::__ndk1), so the build
uses the NDK's headers in the platform's namespace and links Horizon's libc++.so; like the platform,
without RTTI.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'jni'))
from build import API, find_ndk  # noqa: E402

NATIVE = os.path.join(ROOT, 'native', 'hidl_prism')
FS = os.path.join(ROOT, 'work', 'fs')
AOSP = os.path.join(ROOT, 'work', 'aosp')
AOSP_TAG = 'android-14.0.0_r1'
GENERATED = 'arm64/include/generated-headers/system'
# (repository, the directories checked out of it)
AOSP_HEADERS = [
    ('system/libhidl', ['base/include/', 'transport/include/']),
    ('system/libfmq', ['include/', 'base/']),
    ('system/libhwbinder', ['include/']),
    ('system/core', ['libutils/include/', 'libcutils/include/', 'libsystem/include/']),
    ('system/libbase', ['include/']),
    ('system/logging', ['liblog/include/']),
    # hidl-gen's output for the platform's interfaces, as the VNDK snapshot has it
    ('prebuilts/vndk/v33', [f'{GENERATED}/libhidl/transport/base/1.0/', f'{GENERATED}/libhidl/transport/manager/1.0/',
                            f'{GENERATED}/hardware/interfaces/suspend/1.0/']),
]
INCLUDES = [
    'libhidl/base/include', 'libhidl/transport/include', 'libfmq/include', 'libfmq/base', 'libhwbinder/include',
    'core/libutils/include', 'core/libcutils/include', 'core/libsystem/include', 'libbase/include',
    'logging/liblog/include',
    f'vndk33/{GENERATED}/libhidl/transport/base/1.0/android.hidl.base@1.0_genc++_headers/gen',
    f'vndk33/{GENERATED}/libhidl/transport/manager/1.0/android.hidl.manager@1.0_genc++_headers/gen',
    f'vndk33/{GENERATED}/hardware/interfaces/suspend/1.0/android.system.suspend@1.0_genc++_headers/gen',
]
COMMON = ['system/system/lib64/libhidlbase.so', 'system/system/lib64/libutils.so', 'system/system/lib64/libcutils.so',
          'system/system/lib64/liblog.so', 'system/system/lib64/libc++.so']  # Horizon's arm64 builds
BINARIES = {
    'prism_controller': ('controller.cpp', ['odm/lib64/vendor.oculus.hardware.sensors@1.0.so',
                                            'system/system/lib64/libfmq.so']),
    'prism_suspend': ('suspend.cpp', ['system/system/lib64/android.system.suspend@1.0.so',
                                      'system/system/lib64/libbinder_ndk.so']),
}


def fetch_headers():
    for repo, paths in AOSP_HEADERS:
        dest = os.path.join(AOSP, 'vndk33' if repo.startswith('prebuilts/') else repo.split('/')[-1])
        if not os.path.isdir(dest):
            url = f'https://android.googlesource.com/platform/{repo}'
            print(f'fetching {url}')
            subprocess.run(['git', 'clone', '-q', '--depth', '1', '--branch', AOSP_TAG, '--filter=blob:none',
                            '--sparse', url, dest], check=True, stderr=subprocess.DEVNULL)
        subprocess.run(['git', '-C', dest, 'sparse-checkout', 'set', '--no-cone', *paths], check=True)


def config_site(ndk, host, out):
    """The NDK's libc++ configuration in the platform's ABI namespace."""
    src = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host, 'sysroot', 'usr', 'include', 'c++', 'v1', '__config_site')
    with open(src) as f:
        text = f.read().replace('#define _LIBCPP_ABI_NAMESPACE __ndk1', '#define _LIBCPP_ABI_NAMESPACE __1')
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, '__config_site'), 'w') as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ndk')
    ap.add_argument('--out', default=os.path.join('work', 'build', 'hidl'))
    args = ap.parse_args()
    for lib in COMMON + [l for _, libs in BINARIES.values() for l in libs]:
        if not os.path.exists(os.path.join(FS, lib)):
            sys.exit(f'no {lib} in {FS}; run tools/prepare.py first')
    fetch_headers()
    ndk = find_ndk(args.ndk)
    host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
    clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host, 'bin', 'clang++' + ('.exe' if os.name == 'nt' else ''))
    include = os.path.join(args.out, 'include')
    config_site(ndk, host, include)
    flags = [f'--target=aarch64-linux-android{API}', '-std=c++17', '-O2', '-fPIE', '-pie', '-Wall', '-fno-rtti',
             '-nostdlib++', '-I', include]
    for path in INCLUDES:
        flags += ['-isystem', os.path.join(AOSP, path)]
    for binary, (source, libs) in BINARIES.items():
        out = os.path.join(args.out, binary)
        subprocess.run([clang, *flags, os.path.join(NATIVE, source), '-o', out, '-ldl', '-Wl,--build-id=sha1',
                        '-Wl,--allow-shlib-undefined', *[os.path.join(FS, lib) for lib in COMMON + libs]], check=True)
        print(f'built {out}')


if __name__ == '__main__':
    main()
