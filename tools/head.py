"""Move the headset and the right controller from the PC: a small window whose mouse and keys drive
the poses and buttons Prism's tracking service (native/tracking_prism) gives Horizon.

    python tools/head.py

Drag with the left mouse button to look around. W/S move forward and back, A/D sideways, R/F up and
down (hold Shift to go faster). Space faces ahead again, Home also returns to the origin.

The right controller is held ahead of the head and points at the mouse: the window is the view,
its middle straight ahead. The right mouse button pulls its trigger; E presses A, Q presses B, G
squeezes the grip, and Tab presses the Meta button. The poses and buttons reach the emulator
through `adb forward` to the tracking service's port.
"""
import math
import socket
import subprocess
import sys
import tkinter as tk

sys.path.insert(0, __import__('os').path.dirname(__file__))
from emulator import SERIAL, adb_path  # noqa: E402

PORT = 7340  # tracking_prism.c's POSE_PORT
RATE_HZ = 60
LOOK = 0.25  # degrees a pixel
SPEED = 1.0  # meters a second; Shift: 4x
MOVES = {'w': (0, 0, -1), 's': (0, 0, 1), 'a': (-1, 0, 0), 'd': (1, 0, 0), 'r': (0, 1, 0), 'f': (0, -1, 0)}
AIM = 90.0  # degrees across the window the controller points over
HELD = (0.15, -0.25, -0.35)  # where the right controller is, from the head (meters, the head's axes)
# tracking_prism.c's BUTTON_*: what each key presses (and touches).
TRIGGER = 0x1 | 0x40
KEYS = {'e': 0x100 | 0x200, 'q': 0x400 | 0x800, 'g': 0x80, 'tab': 0x2}


def quaternion(yaw, pitch):
    """Turned by yaw about Y (left is positive), then pitched by pitch about its own X (up is
    positive), in degrees: x y z w."""
    y, p = math.radians(yaw) / 2, math.radians(pitch) / 2
    cy, sy, cp, sp = math.cos(y), math.sin(y), math.cos(p), math.sin(p)
    return sp * cy, sy * cp, -sy * sp, cy * cp


def rotate(q, v):
    """v rotated by the unit quaternion q (x y z w)."""
    x, y, z, w = q
    t = (2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0]))
    return (v[0] + w * t[0] + y * t[2] - z * t[1],
            v[1] + w * t[1] + z * t[0] - x * t[2],
            v[2] + w * t[2] + x * t[1] - y * t[0])


class Head:
    def __init__(self, root):
        self.root = root
        self.position = [0.0, 0.0, 0.0]
        self.yaw = self.pitch = 0.0
        self.held = set()
        self.drag = None
        self.sock = None
        self.aim = (0.5, 0.5)  # where the mouse is, across the window
        self.trigger = False
        self.sent_input = None
        self.label = tk.Label(root, font=('Consolas', 10), justify='left', padx=10, pady=10)
        self.label.pack(fill='both', expand=True)
        root.bind('<KeyPress>', self.key_down)
        root.bind('<KeyRelease>', lambda e: self.held.discard(e.keysym.lower()))
        root.bind('<Tab>', lambda e: (self.held.add('tab'), 'break')[1])
        root.bind('<ButtonPress-1>', lambda e: setattr(self, 'drag', (e.x, e.y)))
        root.bind('<ButtonRelease-1>', lambda e: setattr(self, 'drag', None))
        root.bind('<B1-Motion>', self.look)
        root.bind('<Motion>', self.point)
        root.bind('<ButtonPress-3>', lambda e: setattr(self, 'trigger', True))
        root.bind('<ButtonRelease-3>', lambda e: setattr(self, 'trigger', False))
        self.shift = False
        root.bind_all('<Shift_L>', lambda e: setattr(self, 'shift', True))
        root.bind_all('<KeyRelease-Shift_L>', lambda e: setattr(self, 'shift', False))
        self.tick()

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
        w, h = max(self.root.winfo_width(), 1), max(self.root.winfo_height(), 1)
        self.aim = (min(max(event.x / w, 0.0), 1.0), min(max(event.y / h, 0.0), 1.0))

    def move(self, dt):
        step = SPEED * (4 if self.shift else 1) * dt
        yaw = math.radians(self.yaw)
        for key in self.held & MOVES.keys():
            x, y, z = MOVES[key]
            # Along the floor, in the direction the head faces.
            self.position[0] += step * (x * math.cos(yaw) + z * math.sin(yaw))
            self.position[1] += step * y
            self.position[2] += step * (-x * math.sin(yaw) + z * math.cos(yaw))

    def lines(self):
        head = quaternion(self.yaw, self.pitch)
        yield ' '.join(f'{v:.6f}' for v in (*self.position, *head))
        w, h = max(self.root.winfo_width(), 1), max(self.root.winfo_height(), 1)
        aim = quaternion(self.yaw - (self.aim[0] - 0.5) * AIM, self.pitch - (self.aim[1] - 0.5) * AIM * h / w)
        at = [p + o for p, o in zip(self.position, rotate(head, HELD))]
        yield 'hand r ' + ' '.join(f'{v:.6f}' for v in (*at, *aim))
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
        self.move(1 / RATE_HZ)
        connected = self.send()
        x, y, z = self.position
        self.label.config(text=(f'{"connected" if connected else "no tracking service on port %d" % PORT}\n\n'
                                f'yaw {self.yaw:7.1f}   pitch {self.pitch:6.1f}\n'
                                f'x {x:6.2f}   y {y:6.2f}   z {z:6.2f}\n'
                                f'trigger {"down" if self.trigger else "up"}\n\n'
                                'drag: look   WASD: move   R/F: up/down\n'
                                'Shift: faster   Space: face ahead   Home: origin\n'
                                'mouse: point   right button: trigger\n'
                                'E: A   Q: B   G: grip   Tab: Meta button'))
        self.root.after(int(1000 / RATE_HZ), self.tick)


def main():
    subprocess.run([adb_path(), '-s', SERIAL, 'forward', f'tcp:{PORT}', f'tcp:{PORT}'], check=True,
                   capture_output=True)
    root = tk.Tk()
    root.title('Prism head')
    root.geometry('480x300')
    Head(root)
    root.mainloop()


if __name__ == '__main__':
    main()
