"""Build, install and run Prism's OpenXR test apps, arm64 NativeActivities like a Quest app's, which
run through the translator:

- xrprobe (native/xrprobe): logs what Horizon's runtime gives an app (its session states, each
  hand's interaction profile, the controllers' locations). Renders nothing.
- xrdemo (native/xrdemo): draws, a moving grid in a stereo projection layer, for seeing an
  immersive app's frames reach Horizon's compositor.
- xrgame: xrdemo as a store app is: an ordinary app (installed with adb install) that bundles its
  own OpenXR loader, Khronos's, which finds Horizon's runtime as a game's would.

    work/build/<app>/<app>.apk   xrprobe and xrdemo installed as /system_ext/app/<Dir>/<Dir>.apk

    python tools/xrprobe.py [xrdemo|xrgame] [--ndk PATH] [--install] [--start]
    then: adb logcat -s XrProbe (XrDemo)

xrprobe and xrdemo load Meta's OpenXR loader, a system library an app can't load, so they're system
apps: --install puts them there (the emulator then needs a restart, for the package manager to find
them). --start starts the app. The OpenXR headers are downloaded once, from Khronos, into
work/build/<app>/include, and Khronos's loader is built once from the OpenXR SDK's source in
work/build/openxr-sdk; the signing key is made once with openssl.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'jni'))
from build import find_ndk  # noqa: E402
from emulator import SERIAL, adb_path  # noqa: E402

APPS = {  # name: (package, system app directory or None, source, log tag, graphics libraries)
    'xrprobe': ('com.prism.xrprobe', 'PrismXrProbe', 'xrprobe', 'XrProbe', ['-lEGL']),
    'xrdemo': ('com.prism.xrdemo', 'PrismXrDemo', 'xrdemo', 'XrDemo', ['-lvulkan']),
    'xrgame': ('com.prism.xrgame', None, 'xrdemo', 'XrDemo', ['-lvulkan']),  # bundles Khronos's loader
}
LOADER = 'release-1.1.63'  # Khronos's OpenXR SDK, for xrgame's loader
HEADERS = 'https://raw.githubusercontent.com/KhronosGroup/OpenXR-SDK/main/include/openxr/'
API = 29
MANIFEST = '''<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{package}"
    android:versionCode="1" android:versionName="1">
  <uses-feature android:name="android.hardware.vr.headtracking" android:required="true" android:version="1" />
  <uses-feature android:name="com.oculus.feature.BOUNDARYLESS_APP" android:required="true" />  <!-- no boundary setup first -->
  <uses-permission android:name="android.permission.QUERY_ALL_PACKAGES" />
  <uses-permission android:name="org.khronos.openxr.permission.OPENXR" />
  <uses-permission android:name="org.khronos.openxr.permission.OPENXR_SYSTEM" />
  <queries>
    <package android:name="com.oculus.systemdriver" />  <!-- the runtime, which the loader looks up -->
    <provider android:authorities="org.khronos.openxr.runtime_broker;org.khronos.openxr.system_runtime_broker" />
    <intent><action android:name="org.khronos.openxr.OpenXRRuntimeService" /></intent>
  </queries>
  <application android:label="{name}" android:hasCode="false" android:extractNativeLibs="false">
    <meta-data android:name="com.oculus.supportedDevices" android:value="quest3|quest3s|quest2|questpro" />
    <activity android:name="android.app.NativeActivity" android:exported="true" android:launchMode="singleTask"
        android:screenOrientation="landscape" android:configChanges="density|keyboard|keyboardHidden|navigation|orientation|screenLayout|screenSize|uiMode">
      <meta-data android:name="android.app.lib_name" android:value="{source}" />
      <intent-filter>
        <action android:name="android.intent.action.MAIN" />
        <category android:name="android.intent.category.LAUNCHER" />
        <category android:name="com.oculus.intent.category.VR" />
      </intent-filter>
    </activity>
  </application>
</manifest>
'''


def build_tools():
    sdk = os.path.dirname(os.path.dirname(adb_path()))
    tools = sorted(glob.glob(os.path.join(sdk, 'build-tools', '*')))
    platforms = sorted(glob.glob(os.path.join(sdk, 'platforms', 'android-*', 'android.jar')))
    if not tools or not platforms:
        sys.exit('Android SDK build-tools and a platform are needed')
    return tools[-1], platforms[-1]


def headers(out):
    folder = os.path.join(out, 'include', 'openxr')
    os.makedirs(folder, exist_ok=True)
    for name in ('openxr.h', 'openxr_platform.h', 'openxr_platform_defines.h'):
        path = os.path.join(folder, name)
        if not os.path.exists(path):
            urllib.request.urlretrieve(HEADERS + name, path)
    return os.path.join(out, 'include')


def signing_key(out):
    key, cert = os.path.join(out, 'key.pk8'), os.path.join(out, 'cert.pem')
    if not os.path.exists(key):
        openssl = shutil.which('openssl') or r'C:\Program Files\Git\mingw64\bin\openssl.exe'
        pem = os.path.join(out, 'key.pem')
        subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', pem, '-out', cert,
                        '-days', '10000', '-subj', '/CN=xrprobe'], check=True, capture_output=True)
        subprocess.run([openssl, 'pkcs8', '-topk8', '-inform', 'PEM', '-outform', 'DER', '-in', pem, '-out', key, '-nocrypt'],
                       check=True)
    return key, cert


def khronos_loader(ndk):
    """Khronos's OpenXR loader for arm64 Android, built from the SDK's source (Apache 2.0)."""
    top = os.path.join(ROOT, 'work', 'build', 'openxr-sdk')
    lib = os.path.join(top, 'build-arm64', 'src', 'loader', 'libopenxr_loader.so')
    if os.path.exists(lib):
        return lib
    source = os.path.join(top, f'OpenXR-SDK-{LOADER}')
    if not os.path.isdir(source):
        os.makedirs(top, exist_ok=True)
        archive = os.path.join(top, 'sdk.tar.gz')
        urllib.request.urlretrieve(f'https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/{LOADER}.tar.gz', archive)
        import tarfile
        with tarfile.open(archive) as t:
            t.extractall(top)
    sdk = os.path.dirname(os.path.dirname(adb_path()))
    cmake_bin = os.path.join(sorted(glob.glob(os.path.join(sdk, 'cmake', '*')))[-1], 'bin')
    exe = '.exe' if os.name == 'nt' else ''
    cmake = os.path.join(cmake_bin, 'cmake' + exe)
    build = os.path.join(top, 'build-arm64')
    subprocess.run([cmake, '-S', source, '-B', build, '-G', 'Ninja', f'-DCMAKE_MAKE_PROGRAM={os.path.join(cmake_bin, "ninja" + exe)}',
                    f'-DCMAKE_TOOLCHAIN_FILE={os.path.join(ndk, "build", "cmake", "android.toolchain.cmake")}',
                    '-DANDROID_ABI=arm64-v8a', f'-DANDROID_PLATFORM=android-{API}', '-DCMAKE_BUILD_TYPE=Release',
                    '-DBUILD_TESTS=OFF', '-DBUILD_API_LAYERS=OFF', '-DDYNAMIC_LOADER=ON'], check=True)
    subprocess.run([cmake, '--build', build, '--target', 'openxr_loader'], check=True)
    return lib


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('app', nargs='?', choices=sorted(APPS), default='xrprobe')
    ap.add_argument('--ndk')
    ap.add_argument('--out', help='default: work/build/<app>')
    ap.add_argument('--install', action='store_true', help='install it as a system app')
    ap.add_argument('--start', action='store_true', help='start it')
    args = ap.parse_args()
    name = args.app
    package, folder, source, tag, graphics = APPS[name]
    system_dir = f'/system_ext/app/{folder}'
    out = os.path.abspath(args.out or os.path.join('work', 'build', name))
    os.makedirs(out, exist_ok=True)

    ndk = find_ndk(args.ndk)
    host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
    clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host, 'bin', 'clang' + ('.exe' if os.name == 'nt' else ''))
    glue = os.path.join(ndk, 'sources', 'android', 'native_app_glue')
    lib = os.path.join(out, f'lib{source}.so')
    subprocess.run([clang, f'--target=aarch64-linux-android{API}', '-shared', '-fPIC', '-O2', '-std=c11', '-Wall',
                    '-I', headers(out), '-I', glue, os.path.join(ROOT, 'native', source, f'{source}.c'), os.path.join(glue, 'android_native_app_glue.c'),
                    '-u', 'ANativeActivity_onCreate', '-o', lib, '-landroid', '-llog', *graphics, '-ldl'], check=True)

    tools, android_jar = build_tools()
    exe = '.exe' if os.name == 'nt' else ''
    manifest = os.path.join(out, 'AndroidManifest.xml')
    with open(manifest, 'w') as f:
        f.write(MANIFEST.format(package=package, name=name, source=source))
    unsigned, aligned, apk = (os.path.join(out, name) for name in ('unsigned.apk', 'aligned.apk', f'{name}.apk'))
    subprocess.run([os.path.join(tools, 'aapt2' + exe), 'link', '-o', unsigned, '--manifest', manifest, '-I', android_jar,
                    '--min-sdk-version', str(API), '--target-sdk-version', '32'], check=True)
    with zipfile.ZipFile(unsigned, 'a', zipfile.ZIP_STORED) as z:  # loaded from the APK, page-aligned
        z.write(lib, f'lib/arm64-v8a/lib{source}.so')
        if not folder:
            z.write(khronos_loader(ndk), 'lib/arm64-v8a/libopenxr_loader.so')
    subprocess.run([os.path.join(tools, 'zipalign' + exe), '-f', '-p', '4', unsigned, aligned], check=True)
    key, cert = signing_key(out)
    apksigner = os.path.join(tools, 'apksigner' + ('.bat' if os.name == 'nt' else ''))
    subprocess.run([apksigner, 'sign', '--key', key, '--cert', cert, '--out', apk, aligned], check=True)
    print(f'built {apk}')

    adb = [adb_path(), '-s', SERIAL]
    if args.install and not folder:
        subprocess.run(adb + ['install', '-r', apk], check=True)
        print('installed')
    elif args.install:
        subprocess.run(adb + ['root'], check=True, capture_output=True)
        subprocess.run(adb + ['wait-for-device'], check=True)
        installed = subprocess.run(adb + ['shell', 'pm', 'path', package], capture_output=True, text=True).stdout
        if '/data/' in installed:  # an app install would shadow it; the system app itself is left alone
            subprocess.run(adb + ['uninstall', package], capture_output=True)
        subprocess.run(adb + ['shell', 'mkdir', '-p', system_dir], check=True)
        # Renamed into place: a push rewrites the file in place, under processes that have it mapped.
        subprocess.run(adb + ['push', apk, f'{system_dir}/{folder}.apk.new'], check=True)
        subprocess.run(adb + ['shell', 'mv', f'{system_dir}/{folder}.apk.new', f'{system_dir}/{folder}.apk'], check=True)
        print('installed; restart the emulator (python tools/emulator.py stop, then start)')
    if args.start:
        subprocess.run(adb + ['shell', 'am', 'start', '-n', f'{package}/android.app.NativeActivity'], check=True)
        print(f'started; adb logcat -s {tag}')


if __name__ == '__main__':
    main()
