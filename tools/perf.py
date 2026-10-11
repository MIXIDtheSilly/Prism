"""Measures how fast Horizon's frames reach the PC, and what the guest spends making them.

    python tools/perf.py [--seconds 30] [--guest] [--label NAME]

Watches the emulator's shared frames (`-share-vid`: mapping SHM_videmulator<port>, a VideoInfo
{u32 width, height, fps, frame; u64 time (Unix microseconds)}, then BGRA) and prints the shared
frame rate, the interval between frames (mean, p50, p95, p99, max), and how many gaps are longer
than 1.5 frames. With --guest it also reads the compositor's own frame rate from logcat (the VrApi
`FPS=` line) and the busiest guest threads from `top`, so a run says whether the export, the GPU or
the CPU is what's short. Results are appended to work/logs/perf.csv (--label names the run).
"""
import argparse
import csv
import ctypes
import ctypes.wintypes as wt
import os
import re
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from emulator import PORT, adb, shell  # noqa: E402

CSV = os.path.join(ROOT, 'work', 'logs', 'perf.csv')
FILE_MAP_READ = 4
HEADER = struct.Struct('<IIIIQ')


def open_frames(port):
    """Maps the emulator's shared frames read-only; None until the emulator has made them."""
    kernel32 = ctypes.windll.kernel32
    kernel32.OpenFileMappingW.restype = wt.HANDLE
    kernel32.MapViewOfFile.restype = ctypes.c_void_p
    kernel32.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]
    mapping = kernel32.OpenFileMappingW(FILE_MAP_READ, False, f'SHM_videmulator{port}')
    if not mapping:
        return None
    base = kernel32.MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)
    return base or None


def read_header(base):
    return HEADER.unpack(ctypes.string_at(base, HEADER.size))


def frame_rate(seconds=1.0, port=PORT):
    """Frames a second the emulator shared over `seconds`, or None if it isn't sharing any. Unlike
    open_frames, it lets the mapping go again, so it can be asked as often as needed."""
    kernel32 = ctypes.windll.kernel32
    kernel32.OpenFileMappingW.restype = wt.HANDLE
    kernel32.MapViewOfFile.restype = ctypes.c_void_p
    kernel32.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]
    kernel32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.argtypes = [wt.HANDLE]
    mapping = kernel32.OpenFileMappingW(FILE_MAP_READ, False, f'SHM_videmulator{port}')
    if not mapping:
        return None
    try:
        base = kernel32.MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)
        if not base:
            return None
        try:
            start, first = time.perf_counter(), read_header(base)[3]
            time.sleep(seconds)
            frames = (read_header(base)[3] - first) & 0xffffffff
            return frames / (time.perf_counter() - start)
        finally:
            kernel32.UnmapViewOfFile(base)
    finally:
        kernel32.CloseHandle(mapping)


def percentile(sorted_values, p):
    return sorted_values[min(len(sorted_values) - 1, int(len(sorted_values) * p / 100))]


def watch(base, seconds):
    """Frame numbers and their sharing times seen over `seconds`, polled finely."""
    winmm = ctypes.windll.winmm
    winmm.timeBeginPeriod(1)
    try:
        width, height, fps, last, _ = read_header(base)
        seen = []  # (frame number, time in microseconds)
        end = time.perf_counter() + seconds
        while time.perf_counter() < end:
            w, h, f, frame, stamp = read_header(base)
            if frame != last:
                seen.append((frame, stamp))
                last = frame
            time.sleep(0.0003)
        return width, height, fps, seen
    finally:
        winmm.timeEndPeriod(1)


def summarize(seen, seconds):
    if len(seen) < 3:
        return None
    stamps = [t for _, t in seen]
    gaps = sorted((b - a) / 1000.0 for a, b in zip(stamps, stamps[1:]) if b > a)
    span = (stamps[-1] - stamps[0]) / 1e6 or seconds
    rate = (len(seen) - 1) / span
    skipped = sum(b - a - 1 for (a, _), (b, _) in zip(seen, seen[1:]) if b > a)
    mean = sum(gaps) / len(gaps)
    return {'fps': rate, 'mean_ms': mean, 'p50_ms': percentile(gaps, 50), 'p95_ms': percentile(gaps, 95),
            'p99_ms': percentile(gaps, 99), 'max_ms': gaps[-1],
            'long_gaps': sum(g > 1.5 * mean for g in gaps), 'frames': len(seen), 'counter_skips': skipped}


def compositor_fps():
    """The last FPS=x/y the compositor logged, if any."""
    out = shell('logcat -d -t 3000 | grep -E "FPS=[0-9]+" | tail -n 3', check=False)
    found = re.findall(r'FPS=(\d+)/(\d+)', out)
    return f'{found[-1][0]}/{found[-1][1]}' if found else 'n/a'


def busiest_threads(count=10):
    """The busiest guest threads over two seconds (toybox top: -s 9 sorts by CPU), last sample."""
    out = shell(f'top -H -b -n 2 -d 2 -m {count} -s 9', check=False)
    last = out.rfind('Tasks:') if 'Tasks:' in out else out.rfind('Threads:')
    return out[last:].strip() if last >= 0 else out.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--guest', action='store_true', help='also read the compositor FPS and guest CPU over adb')
    parser.add_argument('--label', default='', help='names this run in work/logs/perf.csv')
    parser.add_argument('--port', type=int, default=PORT)
    args = parser.parse_args()

    base = open_frames(args.port)
    if not base:
        sys.exit(f'SHM_videmulator{args.port} is missing: start the emulator with python tools/emulator.py start')
    print(f'watching shared frames for {args.seconds:g} s ...')
    width, height, fps, seen = watch(base, args.seconds)
    result = summarize(seen, args.seconds)
    if not result:
        sys.exit('fewer than three frames were shared: the display may be idle (move the head or a controller)')

    print(f'{width}x{height}, emulator says {fps} fps')
    print(f"shared {result['fps']:.1f} frames/s over {result['frames']} frames "
          f"({result['counter_skips']} frame numbers skipped)")
    print(f"interval ms: mean {result['mean_ms']:.2f}  p50 {result['p50_ms']:.2f}  p95 {result['p95_ms']:.2f}  "
          f"p99 {result['p99_ms']:.2f}  max {result['max_ms']:.2f}  long gaps {result['long_gaps']}")
    comp = 'n/a'
    if args.guest:
        comp = compositor_fps()
        print(f'compositor (logcat FPS=): {comp}')
        print(busiest_threads())

    os.makedirs(os.path.dirname(CSV), exist_ok=True)
    new = not os.path.exists(CSV)
    with open(CSV, 'a', newline='') as f:
        out = csv.writer(f)
        if new:
            out.writerow(['time', 'label', 'size', 'emulator_fps', 'shared_fps', 'mean_ms', 'p50_ms', 'p95_ms',
                          'p99_ms', 'max_ms', 'long_gaps', 'compositor'])
        out.writerow([time.strftime('%Y-%m-%d %H:%M:%S'), args.label, f'{width}x{height}', fps,
                      f"{result['fps']:.1f}", f"{result['mean_ms']:.2f}", f"{result['p50_ms']:.2f}",
                      f"{result['p95_ms']:.2f}", f"{result['p99_ms']:.2f}", f"{result['max_ms']:.2f}",
                      result['long_gaps'], comp])


if __name__ == '__main__':
    main()
