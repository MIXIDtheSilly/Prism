"""Prism's viewer: Horizon in a desktop window, driven with the mouse and keyboard.

    python tools/viewer.py [--eye left|right] [--no-stats] [--build]

Builds and starts native/viewer (Visual Studio's C++ tools; rebuilt when its sources change):
one eye of what the compositor puts on the emulator's display, from the emulator's frames in
shared memory (`-share-vid`, which tools/emulator.py turns on), drawn with Direct3D 11 as each
frame arrives. The emulator's own window can stay minimized.

The right controller points where the mouse is: its ray goes through the pixel under the mouse, so
hovering and clicking work as they look. The left button pulls its trigger. Drag with the right
button to look around; W/S move forward and back, A/D sideways, R/F up and down (Shift: faster),
Space faces ahead again and Home also returns to the origin. Tab presses the Meta button (the
Universal Menu), E and Q press A and B, G or the middle button squeezes the grip. F1 shows the
stats (the compositor's frame rate, Android's CPU, the viewer's frames), F2 saves a screenshot to
work/screenshots, F3 shows both eyes, F11 is fullscreen. Poses and buttons go to Prism's tracking
service (native/tracking_prism) as tools/head.py sends them.
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

SOURCE_DIR = os.path.join(ROOT, 'native', 'viewer')
OUT = os.path.join(ROOT, 'work', 'build', 'viewer')
EXE = os.path.join(OUT, 'prism_viewer.exe')
LIBS = ['d3d11', 'dxgi', 'd2d1', 'dwrite', 'd3dcompiler', 'ws2_32', 'windowscodecs', 'ole32', 'user32', 'winmm']


def vcvars():
    vswhere = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                           'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    if os.path.exists(vswhere):
        out = subprocess.run([vswhere, '-latest', '-products', '*', '-requires',
                              'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                             capture_output=True, text=True).stdout.strip()
        candidate = os.path.join(out, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat') if out else ''
        if os.path.exists(candidate):
            return candidate
    sys.exit("Visual Studio's C++ tools not found (the viewer needs them: install the "
             '"Desktop development with C++" workload)')


def build(force=False):
    sources = glob.glob(os.path.join(SOURCE_DIR, '*'))
    if not force and os.path.exists(EXE) and os.path.getmtime(EXE) >= max(map(os.path.getmtime, sources)):
        return
    os.makedirs(OUT, exist_ok=True)
    compile_ = ('cl /nologo /O2 /MT /EHsc /std:c++20 /W3 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX '
                f'"{os.path.join(SOURCE_DIR, "viewer.cpp")}" /Fe"{EXE}" /Fo"{OUT}\\\\" '
                f'/link {" ".join(lib + ".lib" for lib in LIBS)}')
    result = subprocess.run(f'"{vcvars()}" >nul && {compile_}', shell=True, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f'building the viewer failed:\n{result.stdout}{result.stderr}')
    print(f'built {os.path.relpath(EXE, ROOT)}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--eye', choices=('left', 'right'), default='left')
    ap.add_argument('--no-stats', action='store_true', help='start with the F1 stats hidden')
    ap.add_argument('--build', action='store_true', help='rebuild the viewer even if it looks current')
    args = ap.parse_args()
    if os.name != 'nt':
        sys.exit('the viewer runs on Windows')
    build(args.build)
    subprocess.run([adb_path(), '-s', SERIAL, 'forward', f'tcp:{PORT}', f'tcp:{PORT}'], check=True,
                   capture_output=True)
    subprocess.run([EXE, '--emulator-port', str(EMULATOR_PORT), '--tracking-port', str(PORT), '--eye', args.eye,
                    '--adb', adb_path(), '--serial', SERIAL, '--stats', '0' if args.no_stats else '1',
                    '--shots', os.path.join(ROOT, 'work', 'screenshots')])


if __name__ == '__main__':
    main()
