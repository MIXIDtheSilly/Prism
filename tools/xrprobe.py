"""Build, install and run xrprobe (native/xrprobe): an OpenXR app that logs what Horizon's runtime
gives an app (its session states, each hand's interaction profile, the controllers' locations).
An arm64 NativeActivity, like a Quest app; it runs through the translator.

    work/build/xrprobe/xrprobe.apk   installed as /system_ext/app/PrismXrProbe/PrismXrProbe.apk

    python tools/xrprobe.py [--ndk PATH] [--install] [--start]   then: adb logcat -s XrProbe

It loads Meta's OpenXR loader, a system library an app can't load, so it's a system app: --install
puts it there (the emulator then needs a restart, for the package manager to find it) and --start
starts it. The OpenXR headers are downloaded once, from Khronos, into
work/build/xrprobe/include; the signing key is made once with openssl.
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

PACKAGE = 'com.prism.xrprobe'
SYSTEM_DIR = '/system_ext/app/PrismXrProbe'
SOURCE = os.path.join(ROOT, 'native', 'xrprobe', 'xrprobe.c')
HEADERS = 'https://raw.githubusercontent.com/KhronosGroup/OpenXR-SDK/main/include/openxr/'
API = 29
MANIFEST = f'''<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{PACKAGE}"
    android:versionCode="1" android:versionName="1">
  <uses-feature android:name="android.hardware.vr.headtracking" android:required="true" android:version="1" />
  <uses-feature android:name="com.oculus.feature.BOUNDARYLESS_APP" android:required="true" />  <!-- no boundary setup first -->
  <uses-permission android:name="android.permission.QUERY_ALL_PACKAGES" />
  <queries>
    <package android:name="com.oculus.systemdriver" />  <!-- the runtime, which the loader looks up -->
  </queries>
  <application android:label="xrprobe" android:hasCode="false" android:extractNativeLibs="false">
    <meta-data android:name="com.oculus.supportedDevices" android:value="quest3|quest3s|quest2|questpro" />
    <activity android:name="android.app.NativeActivity" android:exported="true" android:launchMode="singleTask"
        android:screenOrientation="landscape" android:configChanges="density|keyboard|keyboardHidden|navigation|orientation|screenLayout|screenSize|uiMode">
      <meta-data android:name="android.app.lib_name" android:value="xrprobe" />
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ndk')
    ap.add_argument('--out', default=os.path.join('work', 'build', 'xrprobe'))
    ap.add_argument('--install', action='store_true', help='install it as a system app')
    ap.add_argument('--start', action='store_true', help='start it')
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)

    ndk = find_ndk(args.ndk)
    host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
    clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host, 'bin', 'clang' + ('.exe' if os.name == 'nt' else ''))
    glue = os.path.join(ndk, 'sources', 'android', 'native_app_glue')
    lib = os.path.join(out, 'libxrprobe.so')
    subprocess.run([clang, f'--target=aarch64-linux-android{API}', '-shared', '-fPIC', '-O2', '-std=c11', '-Wall',
                    '-I', headers(out), '-I', glue, SOURCE, os.path.join(glue, 'android_native_app_glue.c'),
                    '-u', 'ANativeActivity_onCreate', '-o', lib, '-landroid', '-llog', '-lEGL', '-ldl'], check=True)

    tools, android_jar = build_tools()
    exe = '.exe' if os.name == 'nt' else ''
    manifest = os.path.join(out, 'AndroidManifest.xml')
    with open(manifest, 'w') as f:
        f.write(MANIFEST)
    unsigned, aligned, apk = (os.path.join(out, name) for name in ('unsigned.apk', 'aligned.apk', 'xrprobe.apk'))
    subprocess.run([os.path.join(tools, 'aapt2' + exe), 'link', '-o', unsigned, '--manifest', manifest, '-I', android_jar,
                    '--min-sdk-version', str(API), '--target-sdk-version', '32'], check=True)
    with zipfile.ZipFile(unsigned, 'a', zipfile.ZIP_STORED) as z:  # loaded from the APK, page-aligned
        z.write(lib, 'lib/arm64-v8a/libxrprobe.so')
    subprocess.run([os.path.join(tools, 'zipalign' + exe), '-f', '-p', '4', unsigned, aligned], check=True)
    key, cert = signing_key(out)
    apksigner = os.path.join(tools, 'apksigner' + ('.bat' if os.name == 'nt' else ''))
    subprocess.run([apksigner, 'sign', '--key', key, '--cert', cert, '--out', apk, aligned], check=True)
    print(f'built {apk}')

    adb = [adb_path(), '-s', SERIAL]
    if args.install:
        subprocess.run(adb + ['root'], check=True, capture_output=True)
        subprocess.run(adb + ['wait-for-device'], check=True)
        installed = subprocess.run(adb + ['shell', 'pm', 'path', PACKAGE], capture_output=True, text=True).stdout
        if '/data/' in installed:  # an app install would shadow it; the system app itself is left alone
            subprocess.run(adb + ['uninstall', PACKAGE], capture_output=True)
        subprocess.run(adb + ['shell', 'mkdir', '-p', SYSTEM_DIR], check=True)
        # Renamed into place: a push rewrites the file in place, under processes that have it mapped.
        subprocess.run(adb + ['push', apk, f'{SYSTEM_DIR}/PrismXrProbe.apk.new'], check=True)
        subprocess.run(adb + ['shell', 'mv', f'{SYSTEM_DIR}/PrismXrProbe.apk.new', f'{SYSTEM_DIR}/PrismXrProbe.apk'], check=True)
        print('installed; restart the emulator (python tools/emulator.py stop, then start)')
    if args.start:
        subprocess.run(adb + ['shell', 'am', 'start', '-n', f'{PACKAGE}/android.app.NativeActivity'], check=True)
        print('started; adb logcat -s XrProbe')


if __name__ == '__main__':
    main()
