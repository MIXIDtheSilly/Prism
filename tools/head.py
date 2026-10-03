"""Move the headset from the PC: a small window whose mouse and keys drive the head pose that
Prism's tracking service (native/tracking_prism) gives Horizon.

    python tools/head.py

Drag with the left mouse button to look around. W/S move forward and back, A/D sideways, R/F up and
down (hold Shift to go faster). Space faces ahead again, Home also returns to the origin. The pose
reaches the emulator through `adb forward` to the tracking service's port.
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


def quaternion(yaw, pitch):
    """Turned by yaw about Y (left is positive), then pitched by pitch about its own X (up is
    positive), in degrees: x y z w."""
    y, p = math.radians(yaw) / 2, math.radians(pitch) / 2
    cy, sy, cp, sp = math.cos(y), math.sin(y), math.cos(p), math.sin(p)
    return sp * cy, sy * cp, -sy * sp, cy * cp


class Head:
    def __init__(self, root):
        self.root = root
        self.position = [0.0, 0.0, 0.0]
        self.yaw = self.pitch = 0.0
        self.held = set()
        self.drag = None
        self.sock = None
        self.label = tk.Label(root, font=('Consolas', 10), justify='left', padx=10, pady=10)
        self.label.pack(fill='both', expand=True)
        root.bind('<KeyPress>', self.key_down)
        root.bind('<KeyRelease>', lambda e: self.held.discard(e.keysym.lower()))
        root.bind('<ButtonPress-1>', lambda e: setattr(self, 'drag', (e.x, e.y)))
        root.bind('<ButtonRelease-1>', lambda e: setattr(self, 'drag', None))
        root.bind('<B1-Motion>', self.look)
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

    def move(self, dt):
        step = SPEED * (4 if self.shift else 1) * dt
        yaw = math.radians(self.yaw)
        for key in self.held & MOVES.keys():
            x, y, z = MOVES[key]
            # Along the floor, in the direction the head faces.
            self.position[0] += step * (x * math.cos(yaw) + z * math.sin(yaw))
            self.position[1] += step * y
            self.position[2] += step * (-x * math.sin(yaw) + z * math.cos(yaw))

    def send(self):
        q = quaternion(self.yaw, self.pitch)
        line = ' '.join(f'{v:.6f}' for v in (*self.position, *q)) + '\n'
        try:
            if not self.sock:
                self.sock = socket.create_connection(('127.0.0.1', PORT), timeout=1)
            self.sock.sendall(line.encode())
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
                                f'x {x:6.2f}   y {y:6.2f}   z {z:6.2f}\n\n'
                                'drag: look   WASD: move   R/F: up/down\n'
                                'Shift: faster   Space: face ahead   Home: origin'))
        self.root.after(int(1000 / RATE_HZ), self.tick)


def main():
    subprocess.run([adb_path(), '-s', SERIAL, 'forward', f'tcp:{PORT}', f'tcp:{PORT}'], check=True,
                   capture_output=True)
    root = tk.Tk()
    root.title('Prism head')
    root.geometry('360x200')
    Head(root)
    root.mainloop()


if __name__ == '__main__':
    main()
