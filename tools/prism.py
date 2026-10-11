"""Prism's launcher: sets Prism up from your OTA, starts it, and checks what it needs.

    python tools/prism.py doctor                   # what Prism needs on this PC, and how to get what's missing
    python tools/prism.py setup [OTA] [--refresh 60|90] [--force STEP]
    python tools/prism.py status                   # each setup step: current or not, and Horizon's stage
    python tools/prism.py vr [--latency MS] [--ipd MM] [--menu-button-is-menu] [--stop-after]
    python tools/prism.py desktop [--stop-after]
    python tools/prism.py stop

setup runs each step that isn't current, in order (STEPS): the workspace from the OTA, the AVD and
its display rate, the emulator's frame sharing, the builds, the PC apps and the deploy. Each step
knows whether it's current (the builds by their stamps, the deploy by the id it leaves on the
guest), so setup run again with nothing changed does nothing, and after a pull does only what the
change needs. vr and desktop bring setup up to date, wait until Home is drawing (emulator.py ready)
and run tools/steamvr.py or tools/viewer.py; the emulator keeps running when they exit, since a boot
takes minutes (--stop-after, or stop, shuts it down).

Settings given once are kept in work/prism.json (the OTA, the refresh rate, the headset's latency and
IPD): later runs, and tools/prism_ui.pyw, use them. Progress lines start with "==" for the window.
"""
import argparse
import importlib
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(1, os.path.join(HERE, 'jni'))
import emulator  # noqa: E402
import stamp  # noqa: E402

SETTINGS = os.path.join(ROOT, 'work', 'prism.json')
DEFAULTS = {'ota': None, 'refresh': 90, 'latency': 45.0, 'ipd': 63.0, 'menu_button_is_menu': False}
PREPARED = os.path.join(ROOT, 'work', 'prepared.json')  # which OTA work/ was prepared from
REPORT = os.path.join(ROOT, 'work', 'inventory', 'report.md')  # prepare's last output
BIONIC = os.path.join(ROOT, 'work', 'device', 'bionic')
BRIDGES = {'vr': 'prism_vr.exe', 'desktop': 'prism_viewer.exe'}
# The builds, in deploy's order: (step, the tool's module, its output directory). The translator
# checks its imports against stock bionic, pulled from the emulator.
BUILDS = [('jni', 'build', 'jni'), ('compat', 'compat', 'compat'), ('translator', 'translator', 'translator'),
          ('vulkan', 'vulkan', 'vulkan'), ('gles', 'gles', 'gles'), ('thermal', 'thermal', 'thermal'),
          ('tracking', 'tracking', 'tracking'), ('hidl', 'hidl', 'hidl'), ('relay', 'relay', 'relay'),
          ('spaces', 'spaces', 'spaces'), ('guest_media', 'guest_media', 'guest_media')]


def say(line):
    print(f'== {line}', flush=True)


# --- settings -------------------------------------------------------------------------------------

def load_settings():
    settings = dict(DEFAULTS)
    try:
        with open(SETTINGS, encoding='utf-8') as f:
            settings.update(json.load(f))
    except (OSError, ValueError):
        pass
    return settings


def save_settings(settings):
    os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
    with open(SETTINGS + '.tmp', 'w', encoding='utf-8') as f:
        json.dump(settings, f, indent=1)
    os.replace(SETTINGS + '.tmp', SETTINGS)


def python(tool, *args):
    """Runs one of Prism's tools, its output going to ours; whether it succeeded."""
    return subprocess.run([sys.executable, '-u', os.path.join(HERE, tool), *map(str, args)], cwd=ROOT).returncode == 0


# --- doctor ---------------------------------------------------------------------------------------

def found(fn):
    """fn's result, or None where it would have exited (Prism's finders sys.exit when they find nothing)."""
    try:
        return fn()
    except SystemExit:
        return None


def steamvr_runtime():
    """SteamVR's install directory, as OpenVR's registry of paths has it; None without one."""
    try:
        with open(os.path.join(os.environ.get('LOCALAPPDATA', ''), 'openvr', 'openvrpaths.vrpath'), encoding='utf-8') as f:
            runtimes = json.load(f).get('runtime') or []
    except (OSError, ValueError):
        return None
    return next((r for r in runtimes if os.path.isdir(r)), None)


def doctor_checks(settings):
    """(what, ok, detail, how to fix it): ok is True, False (Prism can't run) or None (a warning)."""
    from build import find_ndk
    from viewer import vcvars
    checks = []
    add = lambda what, ok, detail='', fix='': checks.append((what, ok, detail, fix))  # noqa: E731
    add('Python 3.10 or later', sys.version_info >= (3, 10), sys.version.split()[0], 'install Python from python.org')
    sdk = found(emulator.sdk_root)
    add('Android SDK', bool(sdk), sdk or 'not found',
        'install Android Studio (or its command-line tools) and set ANDROID_SDK_ROOT')
    if sdk:
        image = os.path.join(sdk, emulator.IMAGE, 'system.img')
        add('Stock Android 14 x86_64 system image', os.path.exists(image), os.path.dirname(image),
            'sdkmanager "system-images;android-34;google_apis;x86_64"')
        add('adb', os.path.exists(emulator.adb_path()), emulator.adb_path(), 'sdkmanager "platform-tools"')
        exe = os.path.join(sdk, 'emulator', 'emulator.exe' if os.name == 'nt' else 'emulator')
        accel = subprocess.run([exe, '-accel-check'], capture_output=True, text=True).stdout if os.path.exists(exe) else ''
        add('Android Emulator with hardware acceleration', 'is installed and usable' in accel,
            next((line for line in accel.splitlines() if 'usable' in line or 'not' in line), 'emulator not found'),
            'sdkmanager "emulator", and turn on Windows Hypervisor Platform in Windows Features')
        if settings['refresh'] > 60:
            sites = [found(lambda n=n: emulator.sharer_site(open(emulator.sharer_path(n), 'rb').read()))
                     for n in emulator.SHARER_BINARIES]
            add(f"Emulator frame sharing at {settings['refresh']} Hz", None if None in sites else True,
                'a build whose frame sharing Prism knows' if None not in sites else
                'the emulator build is not 37.1.11, whose frame sharing patch-share-vid knows',
                'install emulator 37.1.11, or set the refresh rate to 60')
    ndk = found(lambda: find_ndk(None))
    add('Android NDK', bool(ndk), ndk or 'not found', 'sdkmanager "ndk;27.3.13750724"')
    vs = found(vcvars)
    add("Visual Studio's C++ tools", bool(vs), vs or 'not found',
        'install Visual Studio (Community is fine) with "Desktop development with C++"')
    add('git', bool(shutil.which('git')), shutil.which('git') or 'not found', 'install Git for Windows')
    runtime = steamvr_runtime()
    add('SteamVR (for vr; desktop works without)', True if runtime else None, runtime or 'not found',
        'install SteamVR from Steam')
    prepared = os.path.exists(REPORT)
    ota = settings.get('ota')
    add('Horizon OS OTA', prepared or bool(ota and os.path.exists(ota)),
        'workspace prepared' if prepared else (ota or 'none chosen'),
        'python tools/prism.py setup path\\to\\horizon-ota.zip (your own headset\'s full OTA)')
    free = shutil.disk_usage(ROOT).free / 2 ** 30
    need = 10 if prepared else 60
    add(f'Free disk space ({need} GB)', True if free >= need else None, f'{free:.0f} GB free on {os.path.splitdrive(ROOT)[0] or "/"}',
        'free some space: the workspace and the AVD take about 60 GB')
    return checks


def doctor(args, settings):
    checks = doctor_checks(settings)
    for what, ok, detail, fix in checks:
        mark = {True: 'ok  ', False: 'FAIL', None: 'warn'}[ok]
        print(f'{mark}  {what}: {detail}')
        if ok is not True:
            print(f'        fix: {fix}')
    failed = [c for c in checks if c[1] is False]
    print('everything Prism needs is here' if not failed else f'{len(failed)} thing(s) missing')
    return not failed


# --- setup's steps --------------------------------------------------------------------------------
# Each step is (name, state, run): state(settings) gives (current, detail), current being True,
# False or None (can't tell: it needs the emulator, which is stopped); run(settings, force) does it.

def prepared_from():
    try:
        with open(PREPARED, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def ota_identity(path):
    st = os.stat(path)
    return {'ota': os.path.abspath(path), 'size': st.st_size, 'mtime_ns': st.st_mtime_ns}


def workspace_state(settings):
    ota, was = settings.get('ota'), prepared_from()
    if not os.path.exists(REPORT):
        return False, 'not prepared' + ('' if ota else ': choose your OTA')
    if ota and os.path.exists(ota) and was and was != ota_identity(ota):
        return False, f'prepared from another OTA ({was["ota"]})'
    return True, f'prepared from {os.path.basename(was["ota"]) if was else "an OTA"}'


def workspace_run(settings, force):
    ota = settings.get('ota')
    if not ota or not os.path.exists(ota):
        sys.exit("no OTA: python tools/prism.py setup path\\to\\horizon-ota.zip (your own headset's full OTA)")
    was = prepared_from()
    # The same OTA, or a workspace prepared before Prism recorded which: what's there is kept.
    same = not force and (was is None or was == ota_identity(ota))
    if not python('prepare.py', ota, *(['--skip-done'] if same else [])):
        return False
    with open(PREPARED, 'w', encoding='utf-8') as f:
        json.dump(ota_identity(ota), f, indent=1)
    return True


def avd_state(settings):
    exists = os.path.exists(emulator.config_path())
    return exists, emulator.AVD if exists else 'not created'


def avd_run(settings, force):
    return python('emulator.py', 'create', *(['--force'] if force else []))


def refresh_state(settings):
    have, want = emulator.refresh_rate(), settings['refresh']
    return have == want, f'{have} Hz' + ('' if have == want else f', want {want}')


def stopped():
    """Stops the emulator if it's running: the AVD's settings and the emulator's binaries change
    only while it's stopped."""
    return not emulator.running() or python('emulator.py', 'stop')


def refresh_run(settings, force):
    return stopped() and python('emulator.py', 'config', '--refresh', settings['refresh'])


def sharing_state(settings):
    rate = settings['refresh']
    if rate <= 60:
        return True, 'the emulator shares 60 frames a second as installed'
    rates = []
    for name in emulator.SHARER_BINARIES:
        try:
            with open(emulator.sharer_path(name), 'rb') as f:
                data = f.read()
        except OSError:
            return False, f'{name} not found'
        site = emulator.sharer_site(data)
        if site is None:
            return False, f'{name}: an emulator build patch-share-vid does not know'
        rates.append(data[site])
    return all(r == rate for r in rates), f'shares up to {min(rates)} frames a second' + (
        '' if all(r == rate for r in rates) else f', want {rate}')


def sharing_run(settings, force):
    return stopped() and python('emulator.py', 'patch-share-vid', '--rate', settings['refresh'])


def emulator_state(settings):
    up = emulator.running()
    return up, emulator.SERIAL + (' running' if up else ' stopped')


def emulator_run(settings, force):
    return python('emulator.py', 'start', '--wait')


def bionic_state(settings):
    there = os.path.exists(os.path.join(BIONIC, 'libc.so'))
    return there, 'pulled' if there else 'not pulled'


def bionic_run(settings, force):
    os.makedirs(os.path.dirname(BIONIC), exist_ok=True)
    return python('emulator.py', 'adb', 'pull', '/apex/com.android.runtime/lib64/bionic', os.path.dirname(BIONIC))


def build_step(module, out):
    out = os.path.join(ROOT, 'work', 'build', out)

    def state(settings):
        current = stamp.current(out, importlib.import_module(module).inputs())
        return current, 'current' if current else ('changed' if stamp.recorded(out) else 'not built')

    def run(settings, force):
        tool = os.path.join('jni', 'build.py') if module == 'build' else module + '.py'
        return python(tool, *(['--force'] if force else []))
    return state, run


def pc_apps_state(settings):
    import steamvr
    import viewer
    stale = [os.path.basename(m.EXE) for m in (viewer, steamvr) if not os.path.exists(m.EXE) or
             os.path.getmtime(m.EXE) < max(map(os.path.getmtime, stamp.files(m.SOURCE_DIR)))]
    return not stale, 'current' if not stale else 'to build: ' + ', '.join(stale)


def pc_apps_run(settings, force):
    import steamvr
    import viewer
    viewer.build(force)
    steamvr.build(force)
    return True


def deploy_args():
    import deploy
    return deploy, deploy.parser().parse_args([])


def deploy_state(settings):
    if not emulator.running():
        return None, 'unknown while the emulator is stopped'
    deploy, args = deploy_args()
    try:
        on_guest = deploy.deployed_id()
    except (RuntimeError, subprocess.TimeoutExpired):
        return None, 'the emulator is not answering yet'
    if on_guest == deploy.deploy_id(args):
        return True, 'the emulator has the current build'
    return False, 'not deployed' if on_guest is None else 'the emulator has an older build'


def deploy_run(settings, force):
    return python('deploy.py')


STEPS = [('workspace', workspace_state, workspace_run),
         ('avd', avd_state, avd_run),
         ('refresh', refresh_state, refresh_run),
         ('frame sharing', sharing_state, sharing_run),
         ('emulator', emulator_state, emulator_run),
         ('bionic', bionic_state, bionic_run),
         *[(name, *build_step(module, out)) for name, module, out in BUILDS],
         ('pc apps', pc_apps_state, pc_apps_run),
         ('deploy', deploy_state, deploy_run)]
STEP_NAMES = [name for name, _, _ in STEPS]


def setup(args, settings):
    """Runs each step that isn't current; whether all of them are now."""
    force = set(args.force or [])
    for i, (name, state, run) in enumerate(STEPS, 1):
        current, detail = state(settings)
        if current and name not in force:
            say(f'[{i}/{len(STEPS)}] {name}: {detail}')
            continue
        say(f'[{i}/{len(STEPS)}] {name}: {detail}; {"forced" if current else "running"}')
        if not run(settings, name in force):
            say(f'{name} failed; fix what it printed above and run setup again')
            return False
        current, detail = state(settings)
        if current is False:
            say(f'{name} ran but is still not current ({detail})')
            return False
    say('setup is current')
    return True


def status(args, settings):
    for name, state, _ in STEPS:
        current, detail = state(settings)
        print(f'{ {True: "ok  ", False: "todo", None: "?   "}[current]}  {name}: {detail}')
    now, detail = emulator.stage()
    print(f'Horizon: {now}{": " + detail if detail else ""}')


# --- running --------------------------------------------------------------------------------------

def bridge_running(name):
    """Whether a bridge (prism_vr.exe, prism_viewer.exe) is running, anyone's."""
    out = subprocess.run(['tasklist', '/fi', f'imagename eq {name}', '/fo', 'csv', '/nh'],
                         capture_output=True, text=True).stdout
    return name.lower() in out.lower()


def stop_bridges():
    for name in BRIDGES.values():
        if bridge_running(name):
            subprocess.run(['taskkill', '/im', name, '/f'], capture_output=True)
            print(f'stopped {name}')


def launch(args, settings, mode):
    exe = BRIDGES[mode]
    if bridge_running(exe):
        sys.exit(f'{exe} is already running; close it first (or python tools/prism.py stop)')
    if not args.no_setup and not setup(args, settings):
        sys.exit(1)
    say('waiting for Home')
    if not python('emulator.py', 'ready'):
        sys.exit('Horizon did not get ready; python tools/emulator.py status says what it got to')
    say(f'starting {mode}')
    if mode == 'vr':
        tool = ['steamvr.py', '--latency', settings['latency'], '--ipd', settings['ipd'],
                *(['--menu-button-is-menu'] if settings['menu_button_is_menu'] else []),
                *(['--control'] if args.control else [])]
    else:
        tool = ['viewer.py']
    try:
        python(*tool)
    except KeyboardInterrupt:
        pass
    if args.stop_after:
        python('emulator.py', 'stop')
    else:
        say('the emulator keeps running; python tools/prism.py stop shuts it down')


def stop(args, settings):
    stop_bridges()
    python('emulator.py', 'stop')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    sub.add_parser('doctor')
    s = sub.add_parser('setup')
    s.add_argument('ota', nargs='?', help="your headset's full OTA (kept for later runs)")
    s.add_argument('--refresh', type=int, choices=(60, 90), help='the display rate (kept; default 90)')
    for p in (s, *[sub.add_parser(m) for m in ('vr', 'desktop')]):
        p.add_argument('--force', action='append', choices=STEP_NAMES, metavar='STEP',
                       help='run this step even if current (repeatable)')
    sub.add_parser('status')
    sub.add_parser('stop')
    for mode in ('vr', 'desktop'):
        p = sub.choices[mode]
        p.add_argument('--no-setup', action='store_true', help="start as things are, without bringing setup up to date")
        p.add_argument('--stop-after', action='store_true', help='stop the emulator when the bridge exits')
    vr = sub.choices['vr']
    vr.add_argument('--latency', type=float, help='ms from the pose a frame is rendered with to the PC (kept)')
    vr.add_argument('--ipd', type=float, help="Horizon's IPD in mm (kept)")
    vr.add_argument('--menu-button-is-menu', action='store_true', default=None,
                    help="the left menu button is Horizon's menu button, not the Meta button (kept)")
    vr.add_argument('--control', action='store_true',
                    help="the bridge also takes commands on stdin (tools/steamvr.py's --control): the window's")
    args = ap.parse_args()
    os.chdir(ROOT)

    settings = load_settings()
    given = {k: getattr(args, k, None) for k in ('ota', 'refresh', 'latency', 'ipd', 'menu_button_is_menu')}
    if given['ota']:
        given['ota'] = os.path.abspath(given['ota'])
    changed = {k: v for k, v in given.items() if v is not None and settings.get(k) != v}
    if changed:
        settings.update(changed)
        save_settings(settings)

    if args.command == 'doctor':
        sys.exit(0 if doctor(args, settings) else 1)
    if args.command == 'setup':
        sys.exit(0 if setup(args, settings) else 1)
    if args.command in BRIDGES:
        launch(args, settings, args.command)
        return
    {'status': status, 'stop': stop}[args.command](args, settings)


if __name__ == '__main__':
    main()
