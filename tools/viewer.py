"""Prism's viewer: Horizon in a desktop window, driven with the mouse and keyboard.

    python tools/viewer.py [--eye left|right] [--scale S]

The window shows one eye of what the compositor puts on the emulator's display (both eyes side by
side), from the emulator's frames in shared memory (`-share-vid`, which tools/emulator.py turns on).
The emulator's own window can stay minimized.

The right controller points where the mouse is: its ray goes through the pixel under the mouse, so
hovering and clicking work as they look. The left button pulls its trigger. Drag with the right
button to look around; W/S move forward and back, A/D sideways, R/F up and down (Shift: faster),
Space faces ahead again and Home also returns to the origin. Tab presses the Meta button (the
Universal Menu), E and Q press A and B, G squeezes the grip. Poses and buttons go to Prism's
tracking service (native/tracking_prism) as tools/head.py sends them.
"""
import argparse
import ctypes
import ctypes.wintypes
import math
import os
import socket
import struct
import subprocess
import sys
import tkinter as tk

from PIL import Image, ImageTk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emulator import PORT as EMULATOR_PORT, SERIAL, adb_path  # noqa: E402
from head import KEYS, MOVES, PORT, SPEED, TRIGGER, quaternion, rotate  # noqa: E402

LOOK = 0.25  # degrees a pixel of right-button drag
HELD = (0.0, -0.06, -0.02)  # where the right controller is, from the head: just below the eyes, so
# its ray starts close to where the view's does (and the controller itself is too close to be drawn)
AIM_DEPTH = 1.08  # meters ahead: the ray goes through the mouse's pixel this far in front, where
# VrShell places panels; nearer or farther, it misses by a few pixels
# What each eye's half of the display shows, as tangents of its edges' angles (left, right, up, down),
# from the head's center: the compositor draws the display's halves without the eyes' offsets. Fitted
# to where the pointer's dot lands, within a pixel or two: the middle of what apps render (their FOV
# reaches -1.1504..1.0 across), undistorted.
VIEWS = {'left': (-0.7128, 0.6559, 0.7357, -0.8040), 'right': (-0.6559, 0.7128, 0.7357, -0.8040)}
AIM_TURN = 5.0  # degrees left the right controller's aim ray is turned from its pose (xrprobe: q 0 .0436 0 .999)
AIM_OFFSET = (-0.009, 0.0, 0.0)  # and where the ray starts, from the pose (xrprobe: p -.009 0 0)
HEADER = struct.Struct('<IIIIQ')  # the emulator's VideoInfo: width, height, fps, frame number, time


def multiply(a, b):
    """The quaternion product a * b (x y z w)."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


class Frames:
    """The emulator's display, as it shares it: a VideoInfo, then RGBA pixels."""

    def __init__(self, port):
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenFileMappingW.restype = ctypes.wintypes.HANDLE
        kernel.OpenFileMappingW.argtypes = [ctypes.wintypes.DWORD, ctypes.wintypes.BOOL, ctypes.wintypes.LPCWSTR]
        kernel.MapViewOfFile.restype = ctypes.c_void_p
        kernel.MapViewOfFile.argtypes = [ctypes.wintypes.HANDLE, ctypes.wintypes.DWORD, ctypes.wintypes.DWORD,
                                         ctypes.wintypes.DWORD, ctypes.c_size_t]
        handle = kernel.OpenFileMappingW(4, False, f'SHM_videmulator{port}')  # FILE_MAP_READ
        if not handle:
            sys.exit(f'no frames from emulator-{port}: start it with tools/emulator.py (which shares them)')
        self.base = kernel.MapViewOfFile(handle, 4, 0, 0, 0)
        if not self.base:
            sys.exit('mapping the emulator\'s frames failed')

    def latest(self):
        """(frame number, an Image of the display) without copying, or None before the first."""
        width, height, _fps, number, _time = HEADER.unpack(ctypes.string_at(self.base, HEADER.size))
        if not width or not height:
            return None
        pixels = (ctypes.c_char * (width * height * 4)).from_address(self.base + HEADER.size)
        return number, Image.frombuffer('RGBA', (width, height), pixels, 'raw', 'RGBA', 0, 1)


class Viewer:
    def __init__(self, root, frames, eye, scale):
        self.root, self.frames, self.eye, self.scale = root, frames, eye, scale
        self.position = [0.0, 0.0, 0.0]
        self.yaw = self.pitch = 0.0
        self.held, self.shift = set(), False
        self.drag = None
        self.mouse = (0.5, 0.5)  # where the mouse is, across the view
        self.trigger = False
        self.sock, self.sent_input = None, None
        self.shown, self.photo = None, None
        self.canvas = tk.Canvas(root, highlightthickness=0, bg='black', cursor='crosshair')
        self.canvas.pack(fill='both', expand=True)
        self.image = self.canvas.create_image(0, 0, anchor='nw')
        self.shadow = self.canvas.create_text(9, 9, anchor='nw', fill='black', font=('Consolas', 9))
        self.status = self.canvas.create_text(8, 8, anchor='nw', fill='#eee', font=('Consolas', 9))
        root.bind('<KeyPress>', self.key_down)
        root.bind('<KeyRelease>', lambda e: self.held.discard(e.keysym.lower()))
        root.bind('<Tab>', lambda e: (self.held.add('tab'), 'break')[1])
        root.bind('<KeyRelease-Tab>', lambda e: (self.held.discard('tab'), 'break')[1])
        self.canvas.bind('<Motion>', self.point)
        self.canvas.bind('<ButtonPress-1>', lambda e: setattr(self, 'trigger', True))
        self.canvas.bind('<ButtonRelease-1>', lambda e: setattr(self, 'trigger', False))
        self.canvas.bind('<ButtonPress-3>', lambda e: setattr(self, 'drag', (e.x, e.y)))
        self.canvas.bind('<ButtonRelease-3>', lambda e: setattr(self, 'drag', None))
        self.canvas.bind('<B3-Motion>', self.look)
        self.canvas.bind('<B1-Motion>', self.point)
        root.bind_all('<Shift_L>', lambda e: setattr(self, 'shift', True))
        root.bind_all('<KeyRelease-Shift_L>', lambda e: setattr(self, 'shift', False))
        self.tick()
        self.draw()

    # --- input -----------------------------------------------------------------------------------

    def key_down(self, event):
        key = event.keysym.lower()
        if key == 'space':
            self.yaw = self.pitch = 0.0
        elif key == 'home':
            self.yaw = self.pitch = 0.0
            self.position = [0.0, 0.0, 0.0]
        else:
            self.held.add(key)

    def look(self, event):
        if self.drag:
            dx, dy = event.x - self.drag[0], event.y - self.drag[1]
            self.yaw = (self.yaw - dx * LOOK + 180) % 360 - 180
            self.pitch = max(-89.0, min(89.0, self.pitch - dy * LOOK))
            self.drag = (event.x, event.y)
        self.point(event)

    def point(self, event):
        w, h = max(self.canvas.winfo_width(), 1), max(self.canvas.winfo_height(), 1)
        self.mouse = (min(max(event.x / w, 0.0), 1.0), min(max(event.y / h, 0.0), 1.0))

    def move(self, dt):
        step = SPEED * (4 if self.shift else 1) * dt
        yaw = math.radians(self.yaw)
        for key in self.held & MOVES.keys():
            x, y, z = MOVES[key]
            self.position[0] += step * (x * math.cos(yaw) + z * math.sin(yaw))
            self.position[1] += step * y
            self.position[2] += step * (-x * math.sin(yaw) + z * math.cos(yaw))

    def aim(self, head):
        """The right controller's place and orientation: its ray through the mouse's pixel."""
        left, right, up, down = VIEWS[self.eye]
        u, v = self.mouse
        ray = (left + u * (right - left), up + v * (down - up), -1.0)  # in the head's axes
        at = rotate(head, HELD)
        target = rotate(head, tuple(c * AIM_DEPTH for c in ray))
        d = [t - a for t, a in zip(target, at)]
        yaw = math.degrees(math.atan2(-d[0], -d[2]))
        pitch = math.degrees(math.atan2(d[1], math.hypot(d[0], d[2])))
        # The runtime's aim pose is the published pose turned by AIM_TURN: publish it turned back.
        orientation = multiply(quaternion(yaw, pitch), quaternion(-AIM_TURN, 0))
        at = [a - o for a, o in zip(at, rotate(orientation, AIM_OFFSET))]
        return [p + a for p, a in zip(self.position, at)], orientation

    def lines(self):
        head = quaternion(self.yaw, self.pitch)
        yield ' '.join(f'{v:.6f}' for v in (*self.position, *head))
        at, orientation = self.aim(head)
        yield 'hand r ' + ' '.join(f'{v:.6f}' for v in (*at, *orientation))
        buttons = (TRIGGER if self.trigger else 0) | sum(bits for key, bits in KEYS.items() if key in self.held)
        state = (buttons, 1.0 if self.trigger else 0.0, 1.0 if 'g' in self.held else 0.0)
        if state != self.sent_input:
            self.sent_input = state
            yield 'input r %#x %.2f %.2f' % state

    def send(self):
        try:
            if not self.sock:
                self.sock = socket.create_connection(('127.0.0.1', PORT), timeout=1)
                self.sent_input = None
            self.sock.sendall(''.join(line + '\n' for line in self.lines()).encode())
            return True
        except OSError:
            if self.sock:
                self.sock.close()
            self.sock = None
            return False

    def tick(self):
        self.move(1 / 60)
        self.connected = self.send()
        self.root.after(16, self.tick)

    # --- the view --------------------------------------------------------------------------------

    def draw(self):
        frame = self.frames.latest()
        if frame and frame[0] != self.shown:
            self.shown, display = frame
            half = display.width // 2
            view = display.crop((0, 0, half, display.height) if self.eye == 'left' else
                                (half, 0, display.width, display.height))
            w, h = self.canvas.winfo_width(), self.canvas.winfo_height()
            if w > 1 and h > 1 and (w, h) != view.size:
                view = view.resize((w, h), Image.BILINEAR)
            self.photo = ImageTk.PhotoImage(view.convert('RGB'))
            self.canvas.itemconfig(self.image, image=self.photo)
        state = 'connected' if self.connected else f'no tracking service on port {PORT}'
        for item in (self.shadow, self.status):
            self.canvas.itemconfig(item, text=f'{state}   yaw {self.yaw:.0f} pitch {self.pitch:.0f}')
            self.canvas.tag_raise(item)
        self.root.after(15, self.draw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--eye', choices=('left', 'right'), default='left')
    ap.add_argument('--scale', type=float, default=0.85, help='the window\'s height, as a part of the screen\'s')
    args = ap.parse_args()
    subprocess.run([adb_path(), '-s', SERIAL, 'forward', f'tcp:{PORT}', f'tcp:{PORT}'], check=True,
                   capture_output=True)
    frames = Frames(EMULATOR_PORT)
    root = tk.Tk()
    root.title('Prism')
    height = int(root.winfo_screenheight() * args.scale)
    root.geometry(f'{height * 960 // 1080}x{height}')
    Viewer(root, frames, args.eye, args.scale)
    root.mainloop()


if __name__ == '__main__':
    main()
