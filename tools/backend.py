"""The backend of Prism's launcher (launcher/, a Tauri window): what the window shows and does, as
requests and events on stdin and stdout, a JSON message a line.

    -> {"id": 1, "method": "state", "args": []}
    <- {"id": 1, "ok": true, "value": {...}}          or {"id": 1, "ok": false, "error": "..."}
    <- {"event": "changed", "payload": state}         the state, whenever it changes
    <- {"event": "output", "payload": [lines]}        the running task's output
    <- {"event": "logcat", "payload": [rows]}         while the window follows logcat

launcher/src-tauri starts it (backend.rs); `python tools/backend.py` takes the same lines from a
terminal. Anything slow that changes things (setup, a start, an install) is a task: a command whose
output goes to work/logs/window-NAME.log and the window's output, one at a time, as prism_ui.pyw
runs prism.py. Everything else (the state, doctor's checks, the apps, a shell command, logcat) it
answers itself, each request on a thread of its own, so a slow one never holds up the rest.
"""
import collections
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LOGS = os.path.join(ROOT, 'work', 'logs')
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

# The protocol has stdout to itself: anything the tools print goes to stderr (backend.rs's log).
OUT = sys.stdout
sys.stdout = sys.stderr


class _WindowlessPopen(subprocess.Popen):
    """The window has no console, so each console program started here (adb, tasklist, every few
    seconds) would get a console window of its own: as prism_ui.pyw, everything starts without one."""

    def __init__(self, *args, creationflags=0, **kwargs):
        super().__init__(*args, creationflags=creationflags | NO_WINDOW, **kwargs)


subprocess.Popen = _WindowlessPopen
sys.path.insert(0, HERE)
sys.path.insert(1, os.path.join(HERE, 'jni'))
import emulator  # noqa: E402
import prism  # noqa: E402

PYTHON = next((p for p in (os.path.join(os.path.dirname(sys.executable), 'python.exe'), sys.executable)
               if os.path.exists(p)), sys.executable)
OUTPUT_LINES = 5000
LOG_BYTES = 2 << 20  # of a log file, the end the window shows
# prism_vr's line once a second, the bridge's status rather than output.
VR_REPORT = re.compile(r'prism_vr: (focused|running|waiting for the session), (tracking|no tracking service), +'
                       r'([\d.]+) frames a second from Horizon, +([\d.]+) submitted, latency (\d+) ms')
VR_LATENCY = re.compile(r'prism_vr: latency (\d+) ms')
LOGCAT = re.compile(r'^(\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\s+(\d+)\s+(\d+)\s+([VDIWEFS])\s+(.*?)\s*: ?(.*)$')
ANSI = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')  # the emulator's log has its colours
PACKAGE = re.compile(r'^[A-Za-z][\w]*(\.[A-Za-z_][\w]*)+$')
SETTING_KEYS = {'ota': (str, type(None)), 'refresh': int, 'latency': (int, float), 'ipd': (int, float),
                'menu_button_is_menu': bool}

_write = threading.Lock()


def send(message):
    with _write:
        OUT.write(json.dumps(message) + '\n')
        OUT.flush()


def event(name, payload):
    send({'event': name, 'payload': payload})


class Failure(Exception):
    """A request's error, as the window shows it."""


def guarded(fn, *args):
    """fn's result; Prism's finders sys.exit when they find nothing (no SDK), which becomes a Failure."""
    try:
        return fn(*args)
    except SystemExit as e:
        raise Failure(str(e.code) if e.code not in (None, 0, 1) else 'a Prism tool stopped') from None


# --- state ----------------------------------------------------------------------------------------

class State:
    def __init__(self):
        self.lock = threading.Lock()
        settings = prism.load_settings()
        emulator.use_avd(settings['avd'])
        self.value = {
            'root': ROOT, 'avd': emulator.AVD, 'serial': emulator.SERIAL, 'settings': settings,
            'horizon': {'stage': 'checking', 'detail': ''}, 'steamvr': False, 'registered': None,
            'bridge': None,  # {'mode', 'external', 'state', 'tracking', 'horizon', 'submitted'}
            'steps': None, 'stepsChecking': False, 'prepared': os.path.exists(prism.REPORT),
            'task': None,    # {'name', 'label', 'running', 'code', 'log'}
            'lastReport': None,
        }

    def get(self, key):
        with self.lock:
            return self.value[key]

    def update(self, **changes):
        with self.lock:
            changed = {k: v for k, v in changes.items() if self.value.get(k) != v}
            if not changed:
                return
            self.value.update(changed)
            snapshot = json.loads(json.dumps(self.value))
        event('changed', snapshot)

    def snapshot(self):
        with self.lock:
            return json.loads(json.dumps(self.value))


STATE = State()
OUTPUT = collections.deque(maxlen=OUTPUT_LINES)
_output = threading.Lock()


def output(lines):
    if not lines:
        return
    with _output:
        OUTPUT.extend(lines)
    event('output', lines)


# --- tasks ----------------------------------------------------------------------------------------

class Task:
    """A command running in the background, its output through a file (work/logs/window-NAME.log) as
    prism_ui.pyw's commands, so one left running when the window closes (a setup) can still print."""

    def __init__(self, name, label, argv):
        self.name, self.label = name, label
        os.makedirs(LOGS, exist_ok=True)
        self.log = os.path.join(LOGS, f'window-{name}.log')
        with open(self.log, 'w', encoding='utf-8') as out:
            self.process = subprocess.Popen(argv, cwd=ROOT, stdin=subprocess.PIPE, stdout=out,
                                            stderr=subprocess.STDOUT, text=True, encoding='utf-8', bufsize=1,
                                            env={**os.environ, 'PYTHONIOENCODING': 'utf-8'})
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        partial = ''
        with open(self.log, encoding='utf-8', errors='replace') as log:
            while True:
                ended = self.process.poll() is not None  # before the read, so nothing written last is missed
                partial += log.read()
                *whole, partial = partial.split('\n')
                lines(whole, self)
                if ended:
                    break
                time.sleep(0.1)
        lines([partial] if partial else [], self)
        finished(self, self.process.returncode)

    def running(self):
        return self.process.poll() is None

    def send(self, line):
        try:
            self.process.stdin.write(line + '\n')
            self.process.stdin.flush()
        except (OSError, ValueError):
            pass

    def close_input(self):
        try:
            self.process.stdin.close()
        except OSError:
            pass

    def kill(self):
        """Ends it and everything it started (a bridge, a build)."""
        subprocess.run(['taskkill', '/pid', str(self.process.pid), '/t', '/f'], capture_output=True)


TASK = None
_task = threading.Lock()


def task_info(task, running=True, code=None):
    return {'name': task.name, 'label': task.label, 'running': running, 'code': code,
            'log': os.path.relpath(task.log, ROOT)}


def start_task(name, label, argv):
    global TASK
    with _task:
        if TASK and TASK.running():
            raise Failure(f'{TASK.label} is still running')
        output([f'> {label}'])
        TASK = Task(name, label, argv)
        STATE.update(task=task_info(TASK))
        if name in ('vr', 'desktop'):
            STATE.update(bridge={'mode': name, 'external': False, 'state': 'setting up'})
    return task_info(TASK)


def prism_task(name, label, *args):
    return start_task(name, label, [PYTHON, '-u', os.path.join(HERE, 'prism.py'), *map(str, args)])


def lines(whole, task):
    shown = []
    for line in whole:
        line = line.rstrip('\r')
        report = VR_REPORT.search(line)
        if report:
            state, tracking, horizon, submitted, latency = report.groups()
            STATE.update(bridge={'mode': 'vr', 'external': task is None, 'state': state, 'tracking': tracking == 'tracking',
                                 'horizon': float(horizon), 'submitted': float(submitted), 'latency': int(latency)})
            continue
        latency = VR_LATENCY.search(line)
        if latency:
            STATE.update(settings={**STATE.get('settings'), 'latency': float(latency.group(1))})
        if task and task.name in ('vr', 'desktop') and line.startswith('== '):
            now = ('running' if line.startswith('== starting ') else 'waiting for Home' if line == '== waiting for Home'
                   else None)
            if now and not (now == 'running' and (STATE.get('bridge') or {}).get('horizon') is not None):
                STATE.update(bridge={'mode': task.name, 'external': False, 'state': now})
        shown.append(line)
    output(shown)


def finished(task, code):
    output([f'< {task.label}: {"done" if code == 0 else f"exit {code}"}'])
    if TASK is not task:  # killed for the task after it (stop's)
        return
    STATE.update(task=task_info(task, running=False, code=code))
    if task.name in ('vr', 'desktop'):
        STATE.update(bridge=None)
    if task.name == 'report' and code == 0:
        import glob
        made = sorted(glob.glob(os.path.join(prism.REPORTS, '*.zip')), key=os.path.getmtime)
        STATE.update(lastReport=made[-1] if made else None)
    if task.name == 'install':
        threading.Thread(target=lambda: ignore(apps_list, False), daemon=True).start()
    CHECK_STEPS.set()


def ignore(fn, *args):
    try:
        fn(*args)
    except Exception:  # noqa: BLE001 (a poll's failure is the next poll's to report)
        pass


# --- pollers --------------------------------------------------------------------------------------

def processes():
    """The names of the PC's running programs, lower case."""
    out = subprocess.run(['tasklist', '/fo', 'csv', '/nh'], capture_output=True, text=True, errors='replace').stdout
    return {line.split('","')[0].strip('"').lower() for line in out.splitlines() if line}


def poll_horizon():
    while True:
        try:
            stage, detail = emulator.stage()
        except (Exception, SystemExit) as e:  # noqa: BLE001 (no SDK, or adb misbehaving: say so, keep polling)
            stage, detail = 'stopped', str(e)
        running = processes()
        STATE.update(horizon={'stage': stage, 'detail': detail}, steamvr='vrserver.exe' in running)
        ours = TASK and TASK.running() and TASK.name in ('vr', 'desktop')
        external = not ours and prism.BRIDGES['vr'] in running
        bridge = STATE.get('bridge')
        if external and not (bridge and bridge.get('external')):
            STATE.update(bridge={'mode': 'vr', 'external': True, 'state': 'started from SteamVR'})
        elif not external and bridge and bridge.get('external'):
            STATE.update(bridge=None)
        time.sleep(3)


CHECK_STEPS = threading.Event()


def step_states():
    settings = STATE.get('settings')
    steps = []
    for name, state, _ in prism.STEPS:
        try:
            current, detail = state(settings)
        except (Exception, SystemExit) as e:  # noqa: BLE001
            current, detail = None, f"couldn't tell: {e}"
        steps.append({'name': name, 'current': current, 'detail': detail})
    return steps


def poll_steps():
    """Setup's steps: when asked (after a task, from the window) and every 30 s, but not while a task
    runs (setup reads them itself, and a build makes them churn)."""
    last = 0
    while True:
        CHECK_STEPS.wait(1)
        if TASK and TASK.running():
            time.sleep(1)
            continue
        if not (CHECK_STEPS.is_set() or time.time() - last > 30):
            continue
        CHECK_STEPS.clear()
        STATE.update(stepsChecking=True)
        steps = step_states()
        STATE.update(steps=steps, stepsChecking=False, prepared=os.path.exists(prism.REPORT),
                     registered=ignore_errors(prism.steamvr_registered))
        last = time.time()


def ignore_errors(fn):
    try:
        return fn()
    except (Exception, SystemExit):  # noqa: BLE001
        return None


def follow_steamvr():
    """While a bridge SteamVR started runs, its log's new lines are the output (its status line the
    bridge's), as prism_ui.pyw follows it."""
    log, offset, partial = None, 0, ''
    while True:
        bridge = STATE.get('bridge')
        if not (bridge and bridge.get('external')):
            if log:
                log.close()
                log = None
            time.sleep(0.5)
            continue
        if log is None:
            try:
                log, offset, partial = open(prism.STEAMVR_LOG, encoding='utf-8', errors='replace'), 0, ''
                output(['> started from SteamVR (work/logs/steamvr-run.log)'])
            except OSError:
                time.sleep(0.5)
                continue
        try:
            if os.path.getsize(prism.STEAMVR_LOG) < offset:  # rewritten by a new start
                log.seek(0)
                partial = ''
        except OSError:
            pass
        partial += log.read()
        offset = log.tell()
        *whole, partial = partial.split('\n')
        lines(whole, None)
        time.sleep(0.25)


# --- the emulator ---------------------------------------------------------------------------------

def need_emulator():
    if not guarded(emulator.running):
        raise Failure("The emulator isn't running: start Horizon first")


def adb_shell(command, timeout=30):
    need_emulator()
    try:
        result = subprocess.run([guarded(emulator.adb_path), '-s', emulator.SERIAL, 'shell', command],
                                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=timeout)
    except subprocess.TimeoutExpired:
        raise Failure(f'no answer within {timeout} s') from None
    return result.returncode, (result.stdout + result.stderr).replace('\r\n', '\n')


def apps_list(everything):
    """The emulator's apps: those installed from outside the system image (or all of them), with
    their versions and whether they run."""
    flag = '' if everything else '-3'
    code, out = adb_shell(f'pm list packages {flag} -f --show-versioncode; echo ===; ps -A -o NAME')
    listing, _, running = out.partition('===')
    running = {line.strip() for line in running.splitlines()}
    apps = []
    for line in listing.splitlines():
        line = line.strip()
        if not line.startswith('package:'):
            continue
        line, _, version = line[8:].rpartition(' versionCode:')
        path, _, package = line.rpartition('=')
        if not package:
            continue
        apps.append({'package': package, 'version': version, 'path': path,
                     'system': not path.startswith('/data/'),
                     'running': package in running or any(r.startswith(package + ':') for r in running)})
    apps.sort(key=lambda a: (a['system'], a['package']))
    return apps


def checked_package(package):
    if not isinstance(package, str) or not PACKAGE.match(package):
        raise Failure(f'not a package name: {package!r}')
    return package


def launch(package):
    for category in ('android.intent.category.LAUNCHER', 'com.oculus.intent.category.VR',
                     'android.intent.category.LEANBACK_LAUNCHER'):
        _, out = adb_shell(f'cmd package resolve-activity --brief -a android.intent.action.MAIN -c {category} {package}')
        component = out.strip().splitlines()[-1].strip() if out.strip() else ''
        if '/' in component:
            _, out = adb_shell(f'am start -n {component}')
            if 'Error' in out:
                raise Failure(out.strip())
            return component
    raise Failure(f'{package} has no activity to launch')


def app_action(package, action):
    package = checked_package(package)
    if action == 'launch':
        return launch(package)
    if action == 'stop':
        # Close the app's windows as Horizon's close does first: a force-stop alone skips their
        # onDestroy, and the Browser then reopens every window it had at its next start (crash recovery)
        _, out = adb_shell('am stack list')
        tasks = re.findall(rf'taskId=(\d+): {re.escape(package)}/', out)
        for task in tasks:
            adb_shell(f'am stack remove {task}')
        if tasks:  # the system lets go of the windows before the app has run their onDestroy
            deadline = time.time() + 5
            while time.time() < deadline and 'ActivityRecord' in adb_shell(f'dumpsys activity processes {package}')[1]:
                time.sleep(0.2)
            time.sleep(1)
    commands = {'stop': f'am force-stop {package}', 'uninstall': f'pm uninstall {package}',
                'clear': f'pm clear {package}'}
    if action not in commands:
        raise Failure(f'unknown action {action}')
    code, out = adb_shell(commands[action], timeout=120)
    if code != 0 or 'Failure' in out:
        raise Failure(out.strip() or f'{action} failed')
    return out.strip()


def install(paths):
    need_emulator()
    apks = [p for p in paths if isinstance(p, str) and p.lower().endswith('.apk') and os.path.isfile(p)]
    if not apks:
        raise Failure('choose one or more .apk files')
    label = f'install {os.path.basename(apks[0])}' + (f' and {len(apks) - 1} more' if len(apks) > 1 else '')
    return start_task('install', label, [PYTHON, '-u', os.path.abspath(__file__), 'install', *apks])


def install_command(apks):
    """`backend.py install APK...`, the install task: an app split in several APKs (split_*.apk,
    config.*.apk beside its base) installs together, separate apps one at a time."""
    adb = emulator.adb_path()
    split = len(apks) > 1 and any(re.match(r'(split_|config\.)', os.path.basename(p), re.I) for p in apks)
    commands = ([['install-multiple', '-r', '-g', *apks]] if split else [['install', '-r', '-g', p] for p in apks])
    for command in commands:
        print(f'adb {" ".join(os.path.basename(a) if a in apks else a for a in command)}', flush=True)
        # Its output handed over explicitly: started without a console window (as everything here is),
        # adb would not inherit the task's log as its output.
        code = subprocess.run([adb, '-s', emulator.SERIAL, *command], stdin=subprocess.DEVNULL,
                              stdout=sys.__stdout__, stderr=subprocess.STDOUT).returncode
        if code:
            return code
    return 0


def shell(command):
    if not isinstance(command, str) or not command.strip():
        raise Failure('no command')
    began = time.time()
    code, out = adb_shell(command, timeout=60)
    return {'command': command, 'code': code, 'output': out, 'ms': round((time.time() - began) * 1000)}


class Logcat:
    """`adb logcat` while the window shows it: its lines, parsed, as logcat events a few times a
    second. Started again when the emulator comes back, or the window follows it again, from the
    last line it had (logcat -T TIME), so nothing comes twice but that line."""

    def __init__(self):
        self.on = False
        self.since = None
        self.process = None
        self.lock = threading.Lock()
        self.rows = []
        threading.Thread(target=self.flush, daemon=True).start()

    def follow(self, on):
        with self.lock:
            if on and not self.on:
                self.on = True
                threading.Thread(target=self.run, daemon=True).start()
            elif not on and self.on:
                self.on = False
                if self.process:
                    self.process.kill()
        return self.on

    def run(self):
        while self.on:
            if not ignore_errors(emulator.running):
                time.sleep(2)
                continue
            args = [emulator.adb_path(), '-s', emulator.SERIAL, 'logcat', '-v', 'threadtime', '-T', self.since or '2000']
            self.process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            stdin=subprocess.DEVNULL, text=True, encoding='utf-8', errors='replace')
            for line in self.process.stdout:
                if not self.on:
                    break
                line = line.rstrip('\r\n')
                m = LOGCAT.match(line)
                row = list(m.groups()) if m else ['', '', '', '-', '', line]
                if m:
                    self.since = m.group(1)
                with self.lock:
                    self.rows.append(row)
            self.process.kill()
            if self.on:
                with self.lock:
                    self.rows.append(['', '', '', '-', '', '--- logcat ended (the emulator stopped?); waiting for it'])
                time.sleep(2)

    def flush(self):
        while True:
            time.sleep(0.2)
            with self.lock:
                rows, self.rows = self.rows, []
            if rows:
                event('logcat', rows)


LOGCAT_FOLLOWER = Logcat()


# --- Prism's own logs -----------------------------------------------------------------------------

def logs():
    try:
        names = os.listdir(LOGS)
    except OSError:
        return []
    found = []
    for name in names:
        path = os.path.join(LOGS, name)
        if os.path.isfile(path):
            st = os.stat(path)
            found.append({'name': name, 'size': st.st_size, 'modified': st.st_mtime})
    found.sort(key=lambda f: -f['modified'])
    return found


def read_log(name):
    if not isinstance(name, str) or os.path.basename(name) != name or name in ('', '.', '..'):
        raise Failure('not a log name')
    path = os.path.join(LOGS, name)
    if not os.path.isfile(path):
        raise Failure(f'{name} is gone')
    with open(path, 'rb') as f:
        end = f.seek(0, 2)
        f.seek(max(0, end - LOG_BYTES))
        data = f.read()
    text = ANSI.sub('', data.decode('utf-8', 'replace')).replace('\r\n', '\n')
    if end > LOG_BYTES:
        text = text.partition('\n')[2]  # from the first whole line
    return {'name': name, 'size': end, 'cut': end > LOG_BYTES, 'text': text}


# --- requests -------------------------------------------------------------------------------------

def save_settings(patch):
    if not isinstance(patch, dict):
        raise Failure('settings must be an object')
    settings = dict(STATE.get('settings'))
    for key, value in patch.items():
        kind = SETTING_KEYS.get(key)
        if kind is None or not isinstance(value, kind) or (isinstance(value, bool) and kind is not bool):
            raise Failure(f'bad setting {key}: {value!r}')
        if key == 'refresh' and value not in (60, 90):
            raise Failure('the refresh rate is 60 or 90')
        if key == 'ota' and value:
            value = os.path.abspath(value)
        settings[key] = value
    prism.save_settings(settings)
    STATE.update(settings=settings)
    CHECK_STEPS.set()
    return settings


def doctor():
    settings = STATE.get('settings')
    return [{'what': what, 'ok': ok, 'detail': detail, 'fix': fix}
            for what, ok, detail, fix in guarded(prism.doctor_checks, settings)]


def run(name, options=None):
    options = options or {}
    settings = STATE.get('settings')
    if name == 'setup':
        force = options.get('force')
        if force is not None and force not in prism.STEP_NAMES:
            raise Failure(f'no step {force}')
        args = ['setup', *([settings['ota']] if settings.get('ota') else []), *(['--force', force] if force else [])]
        return prism_task('setup', 'set up' + (f' ({force} again)' if force else ''), *args)
    if name == 'vr':
        return prism_task('vr', 'start VR', 'vr', '--control')
    if name == 'desktop':
        return prism_task('desktop', 'start desktop', 'desktop')
    if name == 'report':
        return prism_task('report', 'make a report', 'report')
    if name == 'library':
        action = 'unregister' if STATE.get('registered') else 'register'
        return prism_task('steamvr', f'steamvr {action}', 'steamvr', action)
    if name == 'pull':
        return start_task('pull', "copy Meta's apps from the headset",
                          [PYTHON, '-u', os.path.join(HERE, 'apps.py'), 'pull'])
    raise Failure(f'no task {name}')


def stop_all():
    """Ends what runs and stops Horizon (prism.py stop: the bridges and the emulator)."""
    with _task:
        task = TASK
    if task and task.running():
        if task.name == 'vr':
            task.send('quit')
        task.kill()
        for _ in range(50):
            if not task.running():
                break
            time.sleep(0.1)
    return prism_task('stop', 'stop Horizon', 'stop')


def cancel():
    with _task:
        task = TASK
    if not task or not task.running():
        return False
    if task.name == 'vr':
        task.send('quit')
    task.kill()
    return True


CONTROL = {'socket': None}


def bridge_send(line):
    """A command for the VR bridge: through our task's input, or SteamVR's bridge's control port."""
    if not isinstance(line, str) or not re.fullmatch(r'recenter|latency \d{1,3}|(meta|menu) (down|up)', line):
        raise Failure(f'not a bridge command: {line!r}')
    if line.startswith('latency '):
        STATE.update(settings={**STATE.get('settings'), 'latency': float(line[8:])})
        prism.save_settings(STATE.get('settings'))
    with _task:
        task = TASK
    if task and task.running() and task.name == 'vr':
        task.send(line)
        return True
    for _ in range(2):  # once more on a new connection, if the last one went with its bridge
        try:
            if CONTROL['socket'] is None:
                CONTROL['socket'] = socket.create_connection(('127.0.0.1', prism.CONTROL_PORT), timeout=1)
            CONTROL['socket'].sendall((line + '\n').encode())
            return True
        except OSError:
            if CONTROL['socket']:
                CONTROL['socket'].close()
            CONTROL['socket'] = None
    raise Failure("VR isn't running")


def open_target(what):
    """A folder or file the window asks the system to show (backend.rs opens it)."""
    paths = {'logs': LOGS, 'reports': prism.REPORTS, 'root': ROOT}
    if what == 'report':
        report = STATE.get('lastReport')
        if not report or not os.path.exists(report):
            raise Failure('no report yet')
        return {'revealPath': report}
    if what not in paths:
        raise Failure(f'nothing to open called {what}')
    os.makedirs(paths[what], exist_ok=True)
    return {'openPath': paths[what]}


METHODS = {
    'state': lambda: STATE.snapshot(),
    'output': lambda: list(OUTPUT),
    'settings': save_settings,
    'doctor': doctor,
    'checkSteps': lambda: CHECK_STEPS.set() or True,
    'run': run,
    'stop': stop_all,
    'cancel': cancel,
    'bridge': bridge_send,
    'apps': lambda everything=False: apps_list(bool(everything)),
    'app': app_action,
    'install': install,
    'shell': shell,
    'logcat': lambda on: LOGCAT_FOLLOWER.follow(bool(on)),
    'logs': logs,
    'readLog': read_log,
    'open': open_target,
}


def handle(message):
    request = message.get('id')
    try:
        method = METHODS.get(message.get('method'))
        if method is None:
            raise Failure(f'no method {message.get("method")}')
        value = method(*(message.get('args') or []))
        send({'id': request, 'ok': True, 'value': value})
    except Failure as e:
        send({'id': request, 'ok': False, 'error': str(e)})
    except Exception as e:  # noqa: BLE001 (a bug here is the window's error message, not the end of the backend)
        import traceback
        traceback.print_exc()
        send({'id': request, 'ok': False, 'error': f'{type(e).__name__}: {e}'})


def main():
    os.chdir(ROOT)
    for poller in (poll_horizon, poll_steps, follow_steamvr):
        threading.Thread(target=poller, daemon=True).start()
    CHECK_STEPS.set()
    for raw in sys.stdin.buffer:
        try:
            message = json.loads(raw.decode('utf-8'))
        except ValueError:
            continue
        threading.Thread(target=handle, args=(message,), daemon=True).start()
    # The window closed. A VR bridge quits with its input; a setup or a build runs on, into its log.
    LOGCAT_FOLLOWER.follow(False)
    if TASK and TASK.running():
        TASK.close_input()


if __name__ == '__main__':
    if sys.argv[1:2] == ['install']:
        sys.exit(install_command(sys.argv[2:]))
    main()
