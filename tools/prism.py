"""Prism's launcher: sets Prism up from your OTA, starts it, and checks what it needs.

    python tools/prism.py doctor                   # what Prism needs on this PC, and how to get what's missing
    python tools/prism.py setup [OTA] [--refresh 60|90] [--force STEP]
    python tools/prism.py status                   # each setup step: current or not, and Horizon's stage
    python tools/prism.py vr [--latency MS] [--ipd MM] [--menu-button-is-menu] [--stop-after]
    python tools/prism.py desktop [--stop-after]
    python tools/prism.py stop
    python tools/prism.py steamvr register|unregister   # Prism in SteamVR's library, or not

Each command takes --avd NAME: another of Prism's AVDs than prism-api34, as a test from a fresh clone
uses so it leaves the working one alone (kept, as the settings are).

setup runs each step that isn't current, in order (STEPS): the Android SDK's packages Prism needs
(tools/sdk.py installs what's missing), the workspace from the OTA, the AVD and
its display rate, the emulator's frame sharing, the builds, the PC apps, the deploy and the Meta
apps copied from your headset (tools/apps.py pull), installed again after a deploy. Each step
knows whether it's current (the builds by their stamps, the deploy by the id it leaves on the
guest), so setup run again with nothing changed does nothing, and after a pull does only what the
change needs. vr and desktop bring setup up to date, wait until Home is drawing (emulator.py ready)
and run tools/steamvr.py or tools/viewer.py; the emulator keeps running when they exit, since a boot
takes minutes (--stop-after, or stop, shuts it down). While vr runs, the bridge takes commands
(recenter, latency MS, meta|menu down|up) on 127.0.0.1:CONTROL_PORT, a line each: the window's, for a
bridge it didn't start.

steamvr register puts Prism in SteamVR's library (work/steamvr/prism.vrmanifest), which runs
vr --from-steamvr: the bridge starts at once, since SteamVR waits for its app's session, and shows
Horizon once the emulator is up; with no console, the output goes to work/logs/steamvr-run.log.

Settings given once are kept in work/prism.json (the OTA, the AVD, the refresh rate, the headset's
latency and IPD): later runs, and tools/prism_ui.pyw, use them. Progress lines start with "==" for the window.
"""
import argparse
import importlib
import json
import os
import shutil
import socket
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(1, os.path.join(HERE, 'jni'))
import emulator  # noqa: E402
import stamp  # noqa: E402

SETTINGS = os.path.join(ROOT, 'work', 'prism.json')
DEFAULTS = {'ota': None, 'avd': emulator.DEFAULT_AVD, 'refresh': 90, 'latency': 45.0, 'ipd': 63.0, 'menu_button_is_menu': False}
PREPARED = os.path.join(ROOT, 'work', 'prepared.json')  # which OTA work/ was prepared from
REPORT = os.path.join(ROOT, 'work', 'inventory', 'report.md')  # prepare's last output
BIONIC = os.path.join(ROOT, 'work', 'device', 'bionic')
BRIDGES = {'vr': 'prism_vr.exe', 'desktop': 'prism_viewer.exe'}
CONTROL_PORT = 7341  # the VR bridge's commands, from a window that didn't start it (7340 is tracking's)
STEAMVR_LOG = os.path.join(ROOT, 'work', 'logs', 'steamvr-run.log')
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
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


def openvr_path(key):
    """A directory OpenVR's registry of paths has (runtime: SteamVR's install, config: its
    settings); None without SteamVR."""
    try:
        with open(os.path.join(os.environ.get('LOCALAPPDATA', ''), 'openvr', 'openvrpaths.vrpath'), encoding='utf-8') as f:
            paths = json.load(f).get(key) or []
    except (OSError, ValueError):
        return None
    return next((p for p in paths if os.path.isdir(p)), None)


def steamvr_runtime():
    return openvr_path('runtime')


def doctor_checks(settings):
    """(what, ok, detail, how to fix it): ok is True, False (Prism can't run) or None (a warning)."""
    import sdk as android_sdk
    from viewer import vcvars
    checks = []
    add = lambda what, ok, detail='', fix='': checks.append((what, ok, detail, fix))  # noqa: E731
    add('Python 3.10 or later', sys.version_info >= (3, 10), sys.version.split()[0],
        'winget install Python.Python.3.13 (or python.org)')
    java = android_sdk.java()
    add('Java 17 or later (smali)', bool(java and java[0] >= 17),
        f'Java {java[0]} at {java[1]}' if java else 'not found',
        'winget install EclipseAdoptium.Temurin.21.JDK, then open a new terminal (or set JAVA_HOME to a JDK)')
    sdk = android_sdk.root()
    todo = android_sdk.missing(sdk)
    add('Android SDK packages', not todo, f'{sdk}: ' + (f'missing {", ".join(todo)}' if todo else 'all there'),
        'Set up installs them (python tools/sdk.py install)')
    exe = os.path.join(sdk, 'emulator', 'emulator.exe' if os.name == 'nt' else 'emulator')
    if os.path.exists(exe):
        version = android_sdk.revision(os.path.dirname(exe))
        accel = subprocess.run([exe, '-accel-check'], capture_output=True, text=True, encoding='utf-8',
                               errors='replace').stdout
        add('Hardware acceleration for the emulator', 'is installed and usable' in accel,
            next((line for line in accel.splitlines() if 'usable' in line or 'not' in line), accel.strip()),
            'turn on virtualization in the BIOS, and Windows Hypervisor Platform: as administrator, '
            'dism /online /enable-feature /featurename:HypervisorPlatform /all, then restart')
        tested = ' and '.join(android_sdk.EMULATOR_TESTED)
        add('Android Emulator version', True if version in android_sdk.EMULATOR_TESTED else None,
            f'{version}, ' + ('one Prism is tested with' if version in android_sdk.EMULATOR_TESTED else
                              f'and Prism is tested with {tested}'),
            "if Horizon doesn't boot, say which emulator you have when you report it")
        if settings['refresh'] > 60:
            sites = [found(lambda n=n: emulator.sharer_site(open(emulator.sharer_path(n), 'rb').read()))
                     for n in emulator.SHARER_BINARIES]
            add(f"Emulator frame sharing at {settings['refresh']} Hz", None if None in sites else True,
                'a build whose frame sharing Prism knows' if None not in sites else
                f'emulator {version}, whose frame sharing patch-share-vid does not know',
                f'install emulator {android_sdk.EMULATOR_TESTED[-1]}, or set the refresh rate to 60')
    vs = found(vcvars)
    add("Visual Studio's C++ tools", bool(vs), vs or 'not found',
        'Visual Studio (Community is fine) with "Desktop development with C++", or just its build tools: '
        'winget install Microsoft.VisualStudio.2022.BuildTools --override '
        '"--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"')
    add('git', bool(shutil.which('git')), shutil.which('git') or 'not found',
        'winget install Git.Git, then open a new terminal')
    gb = emulator.pc_memory_mb() / 1024
    memory, cores = emulator.fitted()
    add('Memory (16 GB or more)', True if gb >= 15 else None if gb >= 11 else False,
        f'{gb:.0f} GB; the AVD gets {memory // 1024} GB',
        f'with less than 16 GB Horizon runs in {emulator.MEMORY_MB[0] // 1024} GB and swaps; close other programs '
        'before starting it' if gb >= 11 else f'Prism needs about {(emulator.MEMORY_MB[0] + 4096) // 1024} GB')
    threads = emulator.performance_threads()
    add('CPU (8 or more performance threads)', True if threads >= 8 else None if threads >= emulator.CORES[0] else False,
        f'{threads} performance threads of {os.cpu_count()}; the AVD gets {cores} cores',
        'with fewer, Horizon runs but its frame rate drops' if threads >= emulator.CORES[0] else
        f'Prism needs {emulator.CORES[0]} or more')
    if os.path.exists(emulator.config_path()):
        over = emulator.oversized()
        add(f'{emulator.AVD} fits this PC', not over, 'yes' if not over else
            'asks for ' + ', '.join(f'{k}={emulator.avd_config().get(k)}' for k in over),
            'Set up lowers it (python tools/emulator.py config --fit)')
    odd = [p for p in (ROOT, emulator.avd_home(), sdk) if not p.isascii()]
    add('Paths in plain ASCII', True if not odd else None, ', '.join(odd) or 'yes',
        "the emulator and the NDK's tools can fail on paths with other characters: move Prism to one "
        'like C:\\Prism, and set ANDROID_AVD_HOME (AVDs) and ANDROID_SDK_ROOT (the SDK) to ones like C:\\Android\\avd')
    runtime = steamvr_runtime()
    add('SteamVR (for vr; desktop works without)', True if runtime else None, runtime or 'not found',
        'install SteamVR from Steam')
    if runtime:
        listed = steamvr_registered()
        add("Prism in SteamVR's library", True if listed else None, 'registered' if listed else 'not registered',
            'python tools/prism.py steamvr register (or start VR from here)')
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


def same_ota(was, path):
    """Whether work/ was prepared from this OTA. Paths compare without case: a terminal's c:\\ and the
    file dialog's C:\\ are the same file."""
    now = ota_identity(path)
    return was == {**now, 'ota': was.get('ota')} and os.path.normcase(was.get('ota') or '') == os.path.normcase(now['ota'])


def sdk_state(settings):
    import sdk as android_sdk
    todo = android_sdk.missing()
    return not todo, 'has what Prism needs' if not todo else 'to install: ' + ', '.join(todo)


def sdk_run(settings, force):
    return python('sdk.py', 'install')


def workspace_state(settings):
    ota, was = settings.get('ota'), prepared_from()
    if not os.path.exists(REPORT):
        return False, 'not prepared' + ('' if ota else ': choose your OTA')
    if ota and os.path.exists(ota) and was and not same_ota(was, ota):
        return False, f'prepared from another OTA ({was["ota"]})'
    return True, f'prepared from {os.path.basename(was["ota"]) if was else "an OTA"}'


def workspace_run(settings, force):
    ota = settings.get('ota')
    if not ota or not os.path.exists(ota):
        sys.exit("no OTA: python tools/prism.py setup path\\to\\horizon-ota.zip (your own headset's full OTA)")
    was = prepared_from()
    # The same OTA, or a workspace prepared before Prism recorded which: what's there is kept.
    same = not force and (was is None or same_ota(was, ota))
    if not python('prepare.py', ota, *(['--skip-done'] if same else [])):
        return False
    with open(PREPARED, 'w', encoding='utf-8') as f:
        json.dump(ota_identity(ota), f, indent=1)
    return True


def avd_state(settings):
    if not os.path.exists(emulator.config_path()):
        return False, 'not created'
    over = emulator.oversized()
    if over:
        return False, f'{emulator.AVD} asks for more than this PC has ({", ".join(over)})'
    return True, emulator.AVD


def avd_run(settings, force):
    if os.path.exists(emulator.config_path()) and not force:
        return stopped() and python('emulator.py', 'config', '--fit')
    return stopped() and python('emulator.py', 'create', *(['--force'] if force else []))


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


def other_avd():
    """The AVD on Prism's port when it isn't this one (they share the port), else None."""
    running = emulator.running_avd()
    return running if running and running != emulator.AVD else None


def emulator_state(settings):
    up = emulator.running()
    other = up and other_avd()
    if other:
        return False, f'{emulator.SERIAL} runs another AVD ({other})'
    return up, emulator.SERIAL + (' running' if up else ' stopped')


def emulator_run(settings, force):
    return (not other_avd() or python('emulator.py', 'stop')) and python('emulator.py', 'start', '--wait')


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
    if other_avd():
        return None, 'unknown while another AVD runs'
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


def apps_state(settings):
    apps = importlib.import_module('apps')
    if not apps.load_record():
        return True, 'none copied from a headset (python tools/apps.py pull)'
    if not emulator.running() or other_avd():
        return None, 'unknown while the emulator is stopped'
    try:
        todo = apps.missing()
    except RuntimeError:
        return None, 'the emulator is not answering yet'
    if todo:
        return False, f'to install: {", ".join(sorted(todo))}'
    return True, 'the emulator has the apps copied from your headset (python tools/apps.py status)'


def apps_run(settings, force):
    return python('apps.py', 'install')


STEPS = [('sdk', sdk_state, sdk_run),
         ('workspace', workspace_state, workspace_run),
         ('avd', avd_state, avd_run),
         ('refresh', refresh_state, refresh_run),
         ('frame sharing', sharing_state, sharing_run),
         ('emulator', emulator_state, emulator_run),
         ('bionic', bionic_state, bionic_run),
         *[(name, *build_step(module, out)) for name, module, out in BUILDS],
         ('pc apps', pc_apps_state, pc_apps_run),
         ('deploy', deploy_state, deploy_run),
         ('apps', apps_state, apps_run)]
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
                         capture_output=True, text=True, errors='replace').stdout
    return name.lower() in out.lower()


def stop_bridges():
    for name in BRIDGES.values():
        if bridge_running(name):
            subprocess.run(['taskkill', '/im', name, '/f'], capture_output=True)
            print(f'stopped {name}')


class Bridge:
    """prism_vr, its commands (stdin, --control) ours to send: from our own stdin when the window
    started us (its end quits the bridge, as the window's closing should), and from CONTROL_PORT."""

    def __init__(self, settings, from_window):
        import steamvr
        self.process = subprocess.Popen(steamvr.command(settings['latency'], settings['ipd'],
                                                        settings['menu_button_is_menu'], control=True),
                                        stdin=subprocess.PIPE, text=True, bufsize=1)
        self.lock = threading.Lock()
        if from_window:
            threading.Thread(target=self.relay_stdin, daemon=True).start()
        threading.Thread(target=self.serve, daemon=True).start()

    def send(self, line):
        with self.lock:
            try:
                self.process.stdin.write(line.strip() + '\n')
                self.process.stdin.flush()
            except (OSError, ValueError):
                pass

    def relay_stdin(self):
        for line in sys.stdin:
            self.send(line)
        self.send('quit')

    def serve(self):
        try:
            server = socket.create_server(('127.0.0.1', CONTROL_PORT))
        except OSError as e:
            print(f'no control port: {e}', flush=True)
            return
        while True:
            connection, _ = server.accept()
            threading.Thread(target=self.relay, args=(connection,), daemon=True).start()

    def relay(self, connection):
        with connection, connection.makefile('r', encoding='utf-8', errors='replace') as lines:
            for line in lines:
                self.send(line)

    def wait(self):
        while True:
            try:
                return self.process.wait()
            except KeyboardInterrupt:  # Ctrl+C reaches the bridge too, which then quits
                pass

    def quit(self):
        self.send('quit')
        try:
            self.process.wait(10)
        except subprocess.TimeoutExpired:
            self.process.kill()


def logged():
    """vr --from-steamvr under pythonw (SteamVR's launch): no console, so this runs again under
    python.exe with its output, its children's included, in STEAMVR_LOG (as the window runs
    commands). The emulator breaks away from SteamVR's job when it starts."""
    os.makedirs(os.path.dirname(STEAMVR_LOG), exist_ok=True)
    console = os.path.join(os.path.dirname(sys.executable), 'python.exe')
    with open(STEAMVR_LOG, 'w', encoding='utf-8') as log:
        return subprocess.run([console, '-u', os.path.abspath(__file__), *sys.argv[1:]], cwd=ROOT,
                              stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                              creationflags=NO_WINDOW).returncode


def launch(args, settings, mode):
    exe = BRIDGES[mode]
    if bridge_running(exe):
        sys.exit(f'{exe} is already running; close it first (or python tools/prism.py stop)')
    # From SteamVR the bridge starts first: SteamVR waits for its app's session (a boot takes
    # minutes), and the bridge waits for the frames and the tracking service by itself.
    bridge = None
    if mode == 'vr' and args.from_steamvr:
        import steamvr
        say('starting vr')
        steamvr.build()
        bridge = Bridge(settings, from_window=False)
    if not args.no_setup and not setup(args, settings):
        if bridge:
            bridge.quit()
        sys.exit(1)
    say('waiting for Home')
    if not python('emulator.py', 'ready'):
        if bridge:
            bridge.quit()
        sys.exit('Horizon did not get ready; python tools/emulator.py status says what it got to')
    if mode == 'vr':
        import steamvr
        steamvr.forward()
        if not bridge:
            say('starting vr')
            steamvr.build()  # current after setup; with --no-setup, as steamvr.py would
            bridge = Bridge(settings, from_window=args.control)
        bridge.wait()
    else:
        say('starting desktop')
        try:
            python('viewer.py')
        except KeyboardInterrupt:
            pass
    if args.stop_after:
        python('emulator.py', 'stop')
    else:
        say('the emulator keeps running; python tools/prism.py stop shuts it down')


def stop(args, settings):
    stop_bridges()
    python('emulator.py', 'stop')


# --- SteamVR's library ----------------------------------------------------------------------------

APP_KEY = 'prism.horizon'
MANIFEST = os.path.join(ROOT, 'work', 'steamvr', 'prism.vrmanifest')


def write_manifest():
    pythonw = os.path.join(os.path.dirname(sys.executable), 'pythonw.exe')
    app = {'app_key': APP_KEY, 'launch_type': 'binary',
           'binary_path_windows': pythonw if os.path.exists(pythonw) else sys.executable,
           'arguments': f'"{os.path.join(HERE, "prism.py")}" vr --from-steamvr',
           'working_directory': ROOT, 'image_path': os.path.join(ROOT, 'img', 'prism_banner.png'),
           'strings': {'en_us': {'name': 'Horizon OS (Prism)',
                                 'description': 'Horizon OS in the emulator, through Prism'}}}
    os.makedirs(os.path.dirname(MANIFEST), exist_ok=True)
    with open(MANIFEST + '.tmp', 'w', encoding='utf-8') as f:
        json.dump({'source': 'Prism', 'applications': [app]}, f, indent=1)
    os.replace(MANIFEST + '.tmp', MANIFEST)


def appconfig_path():
    config = openvr_path('config')
    return config and os.path.join(config, 'appconfig.json')


def steamvr_registered():
    """Whether SteamVR's list of app manifests (appconfig.json) has Prism's."""
    try:
        with open(appconfig_path(), encoding='utf-8') as f:
            paths = json.load(f).get('manifest_paths') or []
    except (OSError, TypeError, ValueError):
        return False
    return os.path.normcase(MANIFEST) in map(os.path.normcase, paths)


def openvr_call(add):
    """Adds Prism's manifest (or removes it) through SteamVR's IVRApplications, which also saves it
    in appconfig.json; None if SteamVR isn't running (as a background app this never starts it),
    else SteamVR's EVRApplicationError (0: done)."""
    import ctypes
    runtime = steamvr_runtime()
    if not runtime or not bridge_running('vrserver.exe'):
        return None
    api = ctypes.CDLL(os.path.join(runtime, 'bin', 'win64', 'openvr_api.dll'))
    api.VR_InitInternal2.restype = ctypes.c_uint32
    api.VR_InitInternal2.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_int, ctypes.c_char_p]
    api.VR_GetGenericInterface.restype = ctypes.c_void_p
    api.VR_GetGenericInterface.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]
    error = ctypes.c_int()
    api.VR_InitInternal2(ctypes.byref(error), 3, None)  # VRApplication_Background
    if error.value:
        return None
    try:
        table = api.VR_GetGenericInterface(b'FnTable:IVRApplications_007', ctypes.byref(error))
        if error.value or not table:
            return None
        # The table's first two: AddApplicationManifest(path, temporary), RemoveApplicationManifest(path).
        functions = ctypes.cast(table, ctypes.POINTER(ctypes.c_void_p * 2)).contents
        path = MANIFEST.encode('utf-8')
        if add:
            return ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_bool)(functions[0])(path, False)
        return ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_char_p)(functions[1])(path)
    finally:
        api.VR_ShutdownInternal()


def edit_appconfig(add):
    """The same in appconfig.json while SteamVR is stopped (it reads the file when it starts)."""
    path = appconfig_path()
    if not path:
        sys.exit('SteamVR not found: install it from Steam, and run it once')
    try:
        with open(path, encoding='utf-8') as f:
            config = json.load(f)
    except FileNotFoundError:
        config = {}
    paths = [p for p in config.get('manifest_paths') or [] if os.path.normcase(p) != os.path.normcase(MANIFEST)]
    config['manifest_paths'] = paths + ([MANIFEST] if add else [])
    with open(path + '.tmp', 'w', encoding='utf-8') as f:
        json.dump(config, f, indent=3)
    os.replace(path + '.tmp', path)


def steamvr_library(args, settings):
    add = args.action == 'register'
    if add:
        write_manifest()
    elif not os.path.exists(MANIFEST) and not steamvr_registered():
        print("Prism isn't in SteamVR's library")
        return True
    error = openvr_call(add)
    if error is None:
        edit_appconfig(add)
    elif error:
        print(f'SteamVR refused it (EVRApplicationError {error})')
        return False
    print("Prism is in SteamVR's library as Horizon OS (Prism)" if add else "Prism is out of SteamVR's library")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument('--avd', help=f'the AVD to use (kept; default {emulator.DEFAULT_AVD})')
    sub = ap.add_subparsers(dest='command', required=True)

    def command(name, **kw):
        return sub.add_parser(name, parents=[common], **kw)
    command('doctor')
    s = command('setup')
    s.add_argument('ota', nargs='?', help="your headset's full OTA (kept for later runs)")
    s.add_argument('--refresh', type=int, choices=(60, 90), help='the display rate (kept; default 90)')
    for p in (s, *[command(m) for m in ('vr', 'desktop')]):
        p.add_argument('--force', action='append', choices=STEP_NAMES, metavar='STEP',
                       help='run this step even if current (repeatable)')
    command('status')
    command('stop')
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
                    help="commands for the bridge on stdin, its end quitting it (tools/steamvr.py's --control): the window's")
    vr.add_argument('--from-steamvr', action='store_true',
                    help="as SteamVR's library starts it: the bridge first, and with no console, the output in "
                         "work/logs/steamvr-run.log")
    library = command('steamvr', help="Prism in SteamVR's library")
    library.add_argument('action', choices=('register', 'unregister'))
    args = ap.parse_args()
    os.chdir(ROOT)
    if getattr(args, 'from_steamvr', False) and os.path.basename(sys.executable).lower() == 'pythonw.exe':
        sys.exit(logged())

    settings = load_settings()
    given = {k: getattr(args, k, None) for k in ('ota', 'avd', 'refresh', 'latency', 'ipd', 'menu_button_is_menu')}
    if given['ota']:
        given['ota'] = os.path.abspath(given['ota'])
    changed = {k: v for k, v in given.items() if v is not None and settings.get(k) != v}
    if changed:
        settings.update(changed)
        save_settings(settings)
    emulator.use_avd(settings['avd'])

    if args.command == 'doctor':
        sys.exit(0 if doctor(args, settings) else 1)
    if args.command == 'setup':
        sys.exit(0 if setup(args, settings) else 1)
    if args.command == 'steamvr':
        sys.exit(0 if steamvr_library(args, settings) else 1)
    if args.command in BRIDGES:
        launch(args, settings, args.command)
        return
    {'status': status, 'stop': stop}[args.command](args, settings)


if __name__ == '__main__':
    main()
