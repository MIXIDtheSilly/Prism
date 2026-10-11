"""Prism in a PC headset: Horizon through SteamVR (or any PC OpenXR runtime with Direct3D 11).

    python tools/steamvr.py [--latency MS] [--ipd MM] [--menu-button-is-menu] [--build]

Builds and starts native/steamvr (Visual Studio's C++ tools; rebuilt when its sources change), an
OpenXR app on the PC. The headset's and controllers' poses and buttons go to Prism's tracking
service (native/tracking_prism) as tools/viewer.py's do, and each frame the compositor puts on the
emulator's display (both eyes side by side, from `-share-vid`) goes to the headset as a projection
layer, posed where the headset was when Horizon rendered it, so the PC runtime can reproject it.

The origin is where the headset is when tracking starts, facing ahead; R in the console, F9 in any
window or the PC runtime's own recenter (SteamVR's Reset View) sets it again. --latency is how long a frame takes from the pose Horizon renders it with to the PC (45 ms by
default); + and - in the console change it by 5 ms: the right value is the one where the world
holds still as you turn your head. --ipd is Horizon's, the eyes apps render from (63 mm unless set;
xrprobe logs the real one, "eye N: ... at x"): each view is submitted from that eye. The thumbsticks
go through (a Vive wand's trackpad stands in), and Horizon's vibration comes back to the
controllers. The left menu button presses the Meta button (the
Universal Menu), or with --menu-button-is-menu, the menu button. On the keyboard, in any window (so
with the headset on), F7 holds the Meta button and F8 the left menu button.

The OpenXR headers come from the OpenXR SDK (tools/xrprobe.py downloads it once); the loader is
SteamVR's, or the openxr_loader.dll PRISM_OPENXR_LOADER names.
"""
import argparse
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from emulator import PORT as EMULATOR_PORT, SERIAL, adb_path  # noqa: E402
from head import PORT  # noqa: E402
from viewer import vcvars  # noqa: E402
from xrprobe import openxr_sdk  # noqa: E402

SOURCE_DIR = os.path.join(ROOT, 'native', 'steamvr')
OUT = os.path.join(ROOT, 'work', 'build', 'steamvr')
EXE = os.path.join(OUT, 'prism_vr.exe')
LIBS = ['d3d11', 'dxgi', 'd3dcompiler', 'ws2_32', 'user32', 'winmm']


def build(force=False):
    sources = glob.glob(os.path.join(SOURCE_DIR, '*'))
    if not force and os.path.exists(EXE) and os.path.getmtime(EXE) >= max(map(os.path.getmtime, sources)):
        return
    include = os.path.join(openxr_sdk(), 'include')
    os.makedirs(OUT, exist_ok=True)
    compile_ = ('cl /nologo /O2 /MT /EHsc /std:c++20 /W3 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS '
                f'/I"{include}" "{os.path.join(SOURCE_DIR, "prism_vr.cpp")}" /Fe"{EXE}" /Fo"{OUT}\\\\" '
                f'/link {" ".join(lib + ".lib" for lib in LIBS)}')
    result = subprocess.run(f'"{vcvars()}" >nul && {compile_}', shell=True, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f'building prism_vr failed:\n{result.stdout}{result.stderr}')
    print(f'built {os.path.relpath(EXE, ROOT)}')


def forward():
    """The tracking service's port on the PC, through adb (gone when the emulator restarts)."""
    subprocess.run([adb_path(), '-s', SERIAL, 'forward', f'tcp:{PORT}', f'tcp:{PORT}'], check=True,
                   capture_output=True)


def command(latency, ipd, menu_button_is_menu=False, control=False):
    """prism_vr's command line. It waits for the emulator's frames and the tracking service by
    itself, so it can start before either is there."""
    return ([EXE, '--emulator-port', str(EMULATOR_PORT), '--tracking-port', str(PORT),
             '--latency', str(latency), '--ipd', str(ipd)] + (['--menu-button-is-menu'] if menu_button_is_menu else []) +
            (['--control'] if control else []))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--latency', type=float, default=45, help='ms from the pose a frame is rendered with to the PC')
    ap.add_argument('--ipd', type=float, default=63,
                    help="Horizon's IPD in mm, which apps render with (xrprobe logs its eyes' positions)")
    ap.add_argument('--menu-button-is-menu', action='store_true',
                    help="the left menu button is Horizon's menu button, not the Meta button")
    ap.add_argument('--build', action='store_true', help='rebuild even if it looks current')
    ap.add_argument('--control', action='store_true',
                    help="also take commands on stdin (recenter, latency MS, meta|menu down|up, quit): the window's")
    args = ap.parse_args()
    if os.name != 'nt':
        sys.exit('prism_vr runs on Windows')
    build(args.build)
    forward()
    subprocess.run(command(args.latency, args.ipd, args.menu_button_is_menu, args.control))


if __name__ == '__main__':
    main()
