"""Leaves Horizon running for a while and watches it for trouble.

    python tools/soak.py [--minutes 60] [--every 60] [--launch [PACKAGE ...]] [--label NAME]

Horizon has to be up (python tools/prism.py desktop, or vr). Every --every seconds it records a row:
the shared frames over 10 s (perf.py's fps, p99 and long gaps), the guest's free memory and swap
(/proc/meminfo), the emulator's memory on the PC (its working set), its windows (NVIDIA's driver
gives each of gfxstream's pbuffers a hidden one, and every frame pays for them) and the PC's free
memory, and what went wrong since the start: crashes (Java's FATAL EXCEPTION and native Fatal
signal lines in logcat's crash buffer, new tombstones), new ANRs, and system_server or VrShell
restarting (a new pid). With --launch, between rows it opens one of the apps in turn (the given packages, or LAUNCH)
and closes it again after LAUNCH_SECONDS, so apps start and stop all through the run.

Rows go to the console and work/logs/soak-TIME[-LABEL].csv; the end says how the frame rate and memory
moved over the run and lists every crash. It exits 1 if anything crashed or restarted, the frame
rate fell below half the display's, or Horizon stopped answering.
"""
import argparse
import csv
import ctypes
import ctypes.wintypes
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import emulator  # noqa: E402
import perf  # noqa: E402

FRAME_SECONDS = 10
LAUNCH_SECONDS = 20
# Apps --launch opens by default: 2D panels from the OTA and the Meta apps tools/apps.py copies,
# whichever the guest has.
LAUNCH = ('com.oculus.browser', 'com.oculus.tv', 'com.oculus.helpcenter', 'com.oculus.vrprivacycheckup',
          'com.oculus.accountscenter', 'com.oculus.igvr')
WATCHED = ('system_server', emulator.SHELL_PROCESS)


def meminfo():
    """The guest's {MemTotal, MemAvailable, SwapTotal, SwapFree} in MB."""
    out = emulator.shell('cat /proc/meminfo', check=False, timeout=30)
    values = {k: int(v) // 1024 for k, v in re.findall(r'^(\w+):\s+(\d+) kB', out, re.M)}
    return {k: values.get(k, 0) for k in ('MemTotal', 'MemAvailable', 'SwapTotal', 'SwapFree')}


def emulator_working_set_mb():
    """The emulator process's working set on the PC (tasklist's Mem Usage), in MB; 0 if not found."""
    out = subprocess.run(['tasklist', '/fi', 'imagename eq qemu-system-x86_64*', '/fo', 'csv', '/nh'],
                         capture_output=True, text=True, errors='replace').stdout
    sizes = [int(re.sub(r'\D', '', fields[-1]) or 0) for fields in csv.reader(out.splitlines()) if len(fields) >= 5]
    return max(sizes, default=0) // 1024


def emulator_windows():
    """How many windows the emulator process has, hidden ones too; 0 if it isn't found."""
    out = subprocess.run(['tasklist', '/fi', 'imagename eq qemu-system-x86_64*', '/fo', 'csv', '/nh'],
                         capture_output=True, text=True, errors='replace').stdout
    emulators = {int(fields[1]) for fields in csv.reader(out.splitlines()) if len(fields) >= 5}
    user32, count = ctypes.windll.user32, [0]

    def visit(window, _):
        pid = ctypes.wintypes.DWORD()
        user32.GetWindowThreadProcessId(window, ctypes.byref(pid))
        count[0] += pid.value in emulators
        return True
    user32.EnumWindows(ctypes.WINFUNCTYPE(ctypes.wintypes.BOOL, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)(visit), 0)
    return count[0]


def pids():
    out = emulator.shell('; '.join(f'echo "{name}=$(pidof {name})"' for name in WATCHED), check=False, timeout=30)
    return dict(line.split('=', 1) for line in out.splitlines() if '=' in line)


def listing(directory):
    return set(emulator.shell(f'ls {directory}', check=False, timeout=30).split())


def crashes(since):
    """What logcat's crash buffer has had since `since`: a line each ("java: PROCESS: exception" or
    "native: PROCESS: signal")."""
    out = emulator.shell(f"logcat -d -b crash -v brief -T '{since}'", check=False, timeout=60)
    found, process = [], None
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if 'FATAL EXCEPTION' in line:
            process = next((m.group(1) for m in (re.search(r'Process: ([^,\s]+)', x) for x in lines[i + 1:i + 3]) if m), '?')
            reason = next((x.split(': ', 1)[-1].strip() for x in lines[i + 2:i + 5] if 'Exception' in x or 'Error' in x), '')
            found.append(f'java: {process}: {reason}')
        signal = re.search(r'Fatal signal (\d+ \(\w+\)).*?pid \d+ \(([^)]+)\)', line)
        if signal:
            found.append(f'native: {signal.group(2)}: signal {signal.group(1)}')
    return found


def launchable(packages):
    """{package: component} of the packages the guest can launch, in the order given."""
    components = {}
    for category in ('android.intent.category.LAUNCHER', 'com.oculus.intent.category.VR'):
        out = emulator.shell(f'cmd package query-activities --brief -a android.intent.action.MAIN -c {category}',
                             check=False, timeout=30)
        for line in out.splitlines():
            if '/' in line:
                components.setdefault(line.strip().split('/')[0], line.strip())
    return {p: components[p] for p in packages if p in components}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--minutes', type=float, default=60)
    ap.add_argument('--every', type=float, default=60, help='seconds between rows (default 60)')
    ap.add_argument('--launch', nargs='*', metavar='PACKAGE', help=f'open and close apps (default: {", ".join(LAUNCH)})')
    ap.add_argument('--label', default='', help="names the run (in its CSV's name)")
    args = ap.parse_args()

    stage, detail = emulator.stage()
    if stage != 'ready':
        sys.exit(f"Horizon isn't ready ({stage}{': ' + detail if detail else ''}): start it first "
                 '(python tools/prism.py desktop)')
    emulator.adb('root', check=False, timeout=30)
    emulator.adb('wait-for-device', timeout=30)
    base = perf.open_frames(emulator.PORT)
    if not base:
        sys.exit("the emulator isn't sharing frames (it was started without -share-vid)")
    apps = launchable(LAUNCH if args.launch == [] else args.launch or ())
    if args.launch is not None:
        if not apps:
            sys.exit('none of the apps to launch is on the guest: ' + ', '.join(args.launch or LAUNCH))
        print(f'opening and closing in turn: {", ".join(apps)}')
    rate = emulator.refresh_rate()
    since = perf.guest_time()
    first_pids, tombstones, anrs = pids(), listing('/data/tombstones'), listing('/data/anr')
    os.makedirs(os.path.join(ROOT, emulator.LOG_DIR), exist_ok=True)
    name = '-'.join(filter(None, ['soak', time.strftime('%Y%m%d-%H%M%S'), re.sub(r'[^\w.-]', '_', args.label)]))
    out = os.path.join(ROOT, emulator.LOG_DIR, name + '.csv')
    columns = ['minute', 'shared_fps', 'p99_ms', 'long_gaps', 'guest_available_mb', 'guest_swap_used_mb',
               'emulator_ws_mb', 'emulator_windows', 'pc_free_mb', 'crashes', 'new_tombstones', 'new_anrs', 'restarts', 'app']
    print(f'watching for {args.minutes:g} minutes, a row every {args.every:g} s, into {out}')
    print('  '.join(columns))
    rows, problems, restarted, cycle = [], [], set(), 0
    start = time.time()
    with open(out, 'w', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        writer.writerow(columns)
        while time.time() - start < args.minutes * 60:
            began = time.time()
            if not emulator.listed():
                problems.append(f'{(began - start) / 60:.1f} min: the emulator is gone')
                break
            app = ''
            if apps:
                package = list(apps)[cycle % len(apps)]
                cycle += 1
                emulator.shell(f'am start -n {apps[package]}', check=False, timeout=30)
                app = package
            try:
                _, _, _, seen = perf.watch(base, FRAME_SECONDS)
                frames = perf.summarize(seen, FRAME_SECONDS) or {'fps': 0, 'p99_ms': 0, 'long_gaps': 0}
                memory = meminfo()
                now = pids()
            except (RuntimeError, subprocess.TimeoutExpired) as e:
                problems.append(f'{(time.time() - start) / 60:.1f} min: Horizon stopped answering ({e})')
                break
            if apps:
                time.sleep(max(0, LAUNCH_SECONDS - FRAME_SECONDS))
                emulator.shell(f'am force-stop {app}', check=False, timeout=30)
            for name in WATCHED:
                if now.get(name) != first_pids.get(name):
                    restarted.add(f'{name} ({first_pids.get(name) or "none"} -> {now.get(name) or "none"})')
            found = crashes(since)
            row = [f'{(time.time() - start) / 60:.1f}', f"{frames['fps']:.1f}", f"{frames['p99_ms']:.1f}",
                   frames['long_gaps'], memory['MemAvailable'], memory['SwapTotal'] - memory['SwapFree'],
                   emulator_working_set_mb(), emulator_windows(), emulator.pc_memory_mb(available=True), len(found),
                   len(listing('/data/tombstones') - tombstones), len(listing('/data/anr') - anrs), len(restarted), app]
            rows.append(row)
            writer.writerow(row)
            f.flush()
            print('  '.join(str(v) for v in row), flush=True)
            time.sleep(max(0, args.every - (time.time() - began)))

    found = crashes(since)
    print()
    if rows:
        fps = [float(r[1]) for r in rows]
        print(f'shared frames: mean {sum(fps) / len(fps):.1f}, lowest {min(fps):.1f} a second (display {rate} Hz); '
              f'worst p99 {max(float(r[2]) for r in rows):.1f} ms')
        print(f"guest memory available: {rows[0][4]} -> {rows[-1][4]} MB, swap used {rows[0][5]} -> {rows[-1][5]} MB")
        print(f"emulator's working set: {rows[0][6]} -> {rows[-1][6]} MB, windows {rows[0][7]} -> {rows[-1][7]}; "
              f"PC free: {rows[0][8]} -> {rows[-1][8]} MB")
        if min(fps) < rate / 2:
            problems.append(f'the frame rate fell to {min(fps):.1f} a second')
    problems += [f'crashed: {c}' for c in found] + [f'restarted: {r}' for r in sorted(restarted)]
    stones = listing('/data/tombstones') - tombstones
    problems += [f'new tombstone: {s}' for s in sorted(stones) if not s.endswith('.pb')]
    problems += [f'new ANR: {a}' for a in sorted(listing('/data/anr') - anrs)]
    print('\n'.join(problems) if problems else 'nothing crashed or restarted')
    print(f'rows: {out}')
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
