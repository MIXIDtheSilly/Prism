"""Meta's apps from your own headset: the full versions of what the OTA has only placeholders of.

An OTA ships some of Meta's apps (TV, Media Player, Remote Desktop, ...) as placeholders of a few KB
with no activities; a headset downloads the real ones from Meta after setup, as it does Meta's
Store-only apps (Explore, Horizon Worlds, ...). This copies those from a headset connected with adb
(developer mode) into work/apps, and installs them on Prism's emulator as updates of the
placeholders (they're signed alike) or as new apps. Apps the OTA has in full stay as deployed, and
other developers' apps are left out: only Meta's apps with a launcher activity are copied.

    python tools/apps.py list [--serial S]      # what pull would copy from the headset
    python tools/apps.py pull [--serial S]      # copy them into work/apps
    python tools/apps.py install                # install what work/apps has on the emulator
    python tools/apps.py status                 # each copied app: installed on the emulator or not

Nothing from the headset goes into the repository: work/ is ignored by git.
"""
import argparse
import csv
import json
import os
import shutil
import subprocess
import sys

import emulator

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APPS = os.path.join(ROOT, 'work', 'apps')
RECORD = os.path.join(APPS, 'apps.json')  # package -> {'version': versionCode, 'files': [apk names]}
PACKAGES = os.path.join(ROOT, 'work', 'inventory', 'packages.tsv')
FS = os.path.join(ROOT, 'work', 'fs')
META = ('com.oculus.', 'com.meta.', 'com.facebook.', 'com.whatsapp', 'com.instagram.')
# The categories OCMS takes an app with a launcher activity by (LocalAppDataRepository).
LAUNCHER_CATEGORIES = ('android.intent.category.LAUNCHER', 'com.oculus.intent.category.VR',
                       'android.intent.category.LEANBACK_LAUNCHER')
PLACEHOLDER_SIZE = 100_000  # an OTA's placeholder APKs are 4-25 KB


def adb(serial, *args, check=True, stderr=False):
    result = subprocess.run([emulator.adb_path(), '-s', serial, *args], capture_output=True, text=True,
                            encoding='utf-8', errors='replace')
    if check and result.returncode != 0:
        raise RuntimeError(f'adb {" ".join(args)} failed: {(result.stderr or result.stdout).strip()}')
    return (result.stdout + (result.stderr if stderr else '')).replace('\r\n', '\n')


def headset(serial):
    """The connected headset's serial: the one asked for, or the only adb device that isn't an emulator."""
    out = subprocess.run([emulator.adb_path(), 'devices'], capture_output=True, text=True, errors='replace').stdout
    devices = [line.split()[0] for line in out.splitlines()[1:]
               if line.strip().endswith('device') and not line.startswith('emulator-')]
    if serial:
        if serial not in devices:
            sys.exit(f'{serial} is not connected (adb devices: {", ".join(devices) or "none"})')
        return serial
    if not devices:
        sys.exit('no headset found: connect it with USB, with developer mode on, and allow this PC')
    if len(devices) > 1:
        sys.exit(f'more than one device ({", ".join(devices)}); pick one with --serial')
    return devices[0]


def versions(serial):
    """Installed packages and their version codes."""
    found = {}
    for line in adb(serial, 'shell', 'pm list packages --show-versioncode').splitlines():
        if line.startswith('package:'):
            name, _, version = line[len('package:'):].partition(' versionCode:')
            found[name] = int(version)
    return found


def launchable(serial):
    packages = set()
    for category in LAUNCHER_CATEGORIES:
        out = adb(serial, 'shell', f'cmd package query-activities --brief -a android.intent.action.MAIN -c {category}')
        packages.update(line.strip().split('/')[0] for line in out.splitlines() if '/' in line)
    return packages


def ota_apps():
    """The OTA's packages, each with whether its APK is a placeholder."""
    apps = {}
    with open(PACKAGES, encoding='utf-8') as f:
        for row in csv.DictReader(f, delimiter='\t'):
            for base in (FS, os.path.join(FS, 'system')):
                path = base + row['path'].replace('/', os.sep)
                if os.path.isfile(path):
                    apps[row['package']] = os.path.getsize(path) < PLACEHOLDER_SIZE
                    break
    return apps


def candidates(serial):
    """The headset's apps to copy: package -> version code."""
    if not os.path.exists(PACKAGES):
        sys.exit(f'{PACKAGES} is missing; run setup first')
    ota = ota_apps()
    installed = versions(serial)
    on_data = {line[len('package:'):].rpartition('=')[2]
               for line in adb(serial, 'shell', 'pm list packages -f').splitlines()
               if line.startswith('package:/data/app/')}
    wanted = {}
    for name in sorted(on_data & launchable(serial)):
        if (ota[name] if name in ota else name.startswith(META)):
            wanted[name] = installed[name]
    return wanted


def load_record():
    try:
        with open(RECORD, encoding='utf-8') as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


def save_record(record):
    os.makedirs(APPS, exist_ok=True)
    with open(RECORD + '.tmp', 'w', encoding='utf-8') as f:
        json.dump(record, f, indent=1, sort_keys=True)
    os.replace(RECORD + '.tmp', RECORD)


def list_(args):
    serial = headset(args.serial)
    record = load_record()
    for name, version in candidates(serial).items():
        have = record.get(name, {}).get('version')
        print(f'{name} {version}' + ('  (copied)' if have == version else ''))


def pull(args):
    serial = headset(args.serial)
    record = load_record()
    for name, version in candidates(serial).items():
        if record.get(name, {}).get('version') == version:
            print(f'{name}: copied already ({version})')
            continue
        paths = [line[len('package:'):] for line in adb(serial, 'shell', f'pm path {name}').splitlines()
                 if line.startswith('package:')]
        folder = os.path.join(APPS, name)
        shutil.rmtree(folder, ignore_errors=True)
        os.makedirs(folder)
        files = []
        for path in paths:
            files.append(os.path.basename(path))
            adb(serial, 'pull', path, os.path.join(folder, files[-1]))
        size = sum(os.path.getsize(os.path.join(folder, f)) for f in files)
        print(f'{name}: {version}, {len(files)} APK{"s" if len(files) > 1 else ""}, {size / 1e6:.0f} MB')
        record[name] = {'version': version, 'files': files}
        save_record(record)


def missing():
    """The copied apps the emulator doesn't have at the copied version, but for those it refused."""
    record = load_record()
    on_emulator = versions(emulator.SERIAL) if record else {}
    return {name: app for name, app in record.items()
            if on_emulator.get(name) != app['version'] and 'refused' not in app}


def install(args):
    """Installs what's missing. An app the package manager refuses (Media Player needs a Qualcomm
    library, libhexagon.so, the emulator doesn't have) is recorded as refused, with the reason, and
    left out until another version is copied."""
    todo = missing()
    record = load_record()
    for name, app in todo.items():
        files = [os.path.join(APPS, name, f) for f in app['files']]
        out = adb(emulator.SERIAL, 'install-multiple', '-r', *files, check=False, stderr=True).strip()
        result = out.splitlines()[-1] if out else 'no answer'
        print(f'{name}: {result}')
        if result.startswith('Failure [INSTALL_FAILED_MISSING_SHARED_LIBRARY'):
            record[name]['refused'] = result
            save_record(record)
    if not todo:
        print('the emulator has every copied app')
    return not missing()


def status(args):
    record = load_record()
    if not record:
        print('nothing copied; python tools/apps.py pull, with your headset connected')
    todo = missing()
    for name, app in sorted(record.items()):
        state = 'todo' if name in todo else 'no  ' if 'refused' in app else 'ok  '
        print(f'{state}  {name} {app["version"]}' + (f'  ({app["refused"]})' if 'refused' in app else ''))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    for name in ('list', 'pull'):
        sub.add_parser(name).add_argument('--serial', help='the headset, when more than one device is connected')
    sub.add_parser('install')
    sub.add_parser('status')
    args = ap.parse_args()
    result = {'list': list_, 'pull': pull, 'install': install, 'status': status}[args.command](args)
    sys.exit(0 if result in (None, True) else 1)


if __name__ == '__main__':
    main()
