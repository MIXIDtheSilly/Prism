"""The Android SDK's parts Prism needs, installed with Google's sdkmanager.

    python tools/sdk.py              # what the SDK has and what's missing
    python tools/sdk.py install      # install what's missing (setup's first step)

PACKAGES are what Prism builds on and runs: adb, the emulator, the stock Android 14 image and NDK 27,
the NDK Prism's guest builds are tested with (hidl.py doesn't build with NDK 28's libc++ headers).
With no SDK, install creates one at %LOCALAPPDATA%\\Android\\Sdk, where Android Studio would, starting
with Google's command-line tools (CMDLINE_TOOLS). From those it installs with Google's Android CLI
(android.exe, which brings its own Java; Google's metrics off), or with an older SDK's sdkmanager,
which needs Java 17 or later (as smali does). Installing accepts the Android SDK's licenses.
"""
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import smali  # noqa: E402

NDK = '27.3.13750724'
NDK_MAJOR = NDK.split('.')[0]
IMAGE = 'system-images;android-34;google_apis;x86_64'
PACKAGES = ['platform-tools', 'emulator', IMAGE, f'ndk;{NDK}']
# The emulators Prism is tested with: 37.2.12 (sdkmanager's since 2026-10) reached Home at 89 frames a
# second (p99 13.9 ms) from a new AVD, as 37.1.11 does.
EMULATOR_TESTED = ('37.1.11', '37.2.12')
# sdkmanager, from dl.google.com/android/repository/repository2-3.xml's cmdline-tools;latest (23.0)
CMDLINE_TOOLS = ('https://dl.google.com/android/repository/commandlinetools-win-16111833_latest.zip',
                 '57d04f2d75eb8e8fffc5000a987e5de4b5a63e9d')


def candidates():
    return [os.environ.get('ANDROID_SDK_ROOT'), os.environ.get('ANDROID_HOME'),
            os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk'), os.path.expanduser('~/Android/Sdk')]


def root():
    """The SDK: ANDROID_SDK_ROOT's or ANDROID_HOME's if set, else the first of the usual places that
    has the emulator (as emulator.py looks), or anything, else where install creates it."""
    explicit = os.environ.get('ANDROID_SDK_ROOT') or os.environ.get('ANDROID_HOME')
    if explicit:
        return explicit
    usual = candidates()[2:]
    there = [c for c in usual if os.path.isdir(c)]
    return next((c for c in there if os.path.isdir(os.path.join(c, 'emulator'))), there[0] if there else usual[0])


def revision(path):
    """A package's version (Pkg.Revision in its source.properties), or None."""
    try:
        with open(os.path.join(path, 'source.properties'), encoding='utf-8', errors='replace') as f:
            return next((m.group(1) for m in re.finditer(r'^Pkg\.Revision\s*=\s*(\S+)', f.read(), re.M)), None)
    except OSError:
        return None


def ndks(sdk):
    """The SDK's NDKs, newest first."""
    found = [p for p in glob.glob(os.path.join(sdk, 'ndk', '*')) if os.path.isdir(os.path.join(p, 'toolchains', 'llvm'))]
    return sorted(found, key=lambda p: [int(n) for n in re.findall(r'\d+', revision(p) or os.path.basename(p))],
                  reverse=True)


def installed(sdk, package):
    if package.startswith('ndk;'):
        return any((revision(p) or '').split('.')[0] == NDK_MAJOR for p in ndks(sdk))
    path = os.path.join(sdk, *package.split(';'))
    if package == IMAGE:
        return os.path.exists(os.path.join(path, 'system.img'))
    return os.path.exists(os.path.join(path, 'package.xml')) or revision(path) is not None


def missing(sdk=None):
    sdk = sdk or root()
    return [p for p in PACKAGES if not installed(sdk, p)]


def java():
    """(Java's major version, its home) for the Java smali and sdkmanager use, or None."""
    try:
        exe = smali.java()
    except SystemExit:
        return None
    try:
        out = subprocess.run([exe, '-XshowSettings:properties', '-version'], capture_output=True, text=True,
                             encoding='utf-8', errors='replace', timeout=30).stderr
    except (OSError, subprocess.TimeoutExpired):
        return None
    version = re.search(r'java\.specification\.version = (?:1\.)?(\d+)', out)
    home = re.search(r'java\.home = (.+)', out)
    return (int(version.group(1)), home.group(1).strip() if home else None) if version else None


def tool(sdk, name):
    """One of the SDK's command-line tools (bin/NAME), from the latest that has it; or None."""
    for path in [os.path.join(sdk, 'cmdline-tools', 'latest'), *sorted(glob.glob(os.path.join(sdk, 'cmdline-tools', '*')), reverse=True)]:
        exe = os.path.join(path, 'bin', name)
        if os.path.exists(exe):
            return exe
    return None


def fetch_cmdline_tools(sdk):
    """Google's command-line tools into SDK/cmdline-tools/latest."""
    url, sha1 = CMDLINE_TOOLS
    tools = os.path.join(sdk, 'cmdline-tools')
    os.makedirs(tools, exist_ok=True)
    download = os.path.join(tools, 'download.zip')
    print(f'downloading {url}', flush=True)
    digest, done, shown = hashlib.sha1(), 0, -1
    with urllib.request.urlopen(url, timeout=60) as response, open(download, 'wb') as f:
        size = int(response.headers.get('Content-Length') or 0)
        while chunk := response.read(1 << 20):
            f.write(chunk)
            digest.update(chunk)
            done += len(chunk)
            if size and done * 10 // size != shown:
                shown = done * 10 // size
                print(f'  {done * 100 // size}%', flush=True)
    if digest.hexdigest() != sha1:
        os.remove(download)
        sys.exit(f'the download of {url} is not the file it should be (SHA-1 {digest.hexdigest()}); try again')
    unpacked = os.path.join(tools, 'unpacked')
    shutil.rmtree(unpacked, ignore_errors=True)
    with zipfile.ZipFile(download) as z:
        z.extractall(unpacked)
    shutil.rmtree(os.path.join(tools, 'latest'), ignore_errors=True)
    os.replace(os.path.join(unpacked, 'cmdline-tools'), os.path.join(tools, 'latest'))
    shutil.rmtree(unpacked, ignore_errors=True)
    os.remove(download)


def run_sdkmanager(manager, sdk, args, home, **kwargs):
    """sdkmanager.bat with args, each quoted: cmd splits an unquoted package's name at its ';'. Its own
    check of Java's version is skipped (install has checked it): it reads a Java that says "27" as
    older than 17.0."""
    command = f'"{manager}" "--sdk_root={sdk}" ' + ' '.join(f'"{a}"' for a in args)
    env = dict(os.environ, SKIP_JDK_VERSION_CHECK='1', **({'JAVA_HOME': home} if home else {}))
    return subprocess.run(command, env=env, **kwargs).returncode == 0


def install():
    """Installs the packages the SDK is missing; whether it has them all now."""
    sdk = root()
    todo = missing(sdk)
    if not todo:
        print('the Android SDK has everything Prism needs')
        return True
    print(f'installing into {sdk}: {", ".join(todo)}', flush=True)
    if not tool(sdk, 'android.exe') and not tool(sdk, 'sdkmanager.bat'):
        fetch_cmdline_tools(sdk)
    print("accepting the Android SDK's licenses (https://developer.android.com/studio/terms)", flush=True)
    cli = tool(sdk, 'android.exe')
    if cli:
        # Google's Android CLI (command-line tools 23 on), which brings its own Java; the licenses
        # need no step of their own.
        ok = subprocess.run([cli, '--no-metrics', f'--sdk={sdk}', 'sdk', 'install', *[p.replace(';', '/') for p in todo]],
                            stdin=subprocess.DEVNULL).returncode == 0
    else:
        # An older SDK's sdkmanager, which runs on the PC's Java.
        found = java()
        if not found or found[0] < 17:
            sys.exit("the SDK's sdkmanager needs Java 17 or later: winget install EclipseAdoptium.Temurin.21.JDK, "
                     'or set JAVA_HOME to a JDK')
        manager = tool(sdk, 'sdkmanager.bat')
        run_sdkmanager(manager, sdk, ['--licenses'], found[1], input='y\n' * 50, text=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ok = run_sdkmanager(manager, sdk, todo, found[1], stdin=subprocess.DEVNULL)
    if not ok:
        print('installing failed; what the SDK printed above says why')
        return False
    still = missing(sdk)
    if still:
        print(f'still missing after sdkmanager: {", ".join(still)}')
    return not still


def main():
    if sys.argv[1:] == ['install']:
        sys.exit(0 if install() else 1)
    if sys.argv[1:]:
        sys.exit(__doc__)
    sdk = root()
    print(f'Android SDK: {sdk}{"" if os.path.isdir(sdk) else " (not there yet)"}')
    for package in PACKAGES:
        print(f'{"ok  " if installed(sdk, package) else "todo"}  {package}')


if __name__ == '__main__':
    main()
