"""Prism's window: sets Prism up, starts and stops it, and drives the headset's controls.

    Prism.cmd (in the repository's root), or: pythonw tools/prism_ui.pyw [--vr]

Everything it does is a tools/prism.py command run in the background (setup, vr, desktop, stop,
doctor), whose output goes to the Log (and work/logs/window-COMMAND.log); so the window shows what
those commands would print, and a crash in one never takes the window down. Its status lines poll the same functions the commands
use: emulator.stage() for Horizon, and setup's steps for whether setup is current. While VR runs,
the Headset controls go to the bridge (tools/steamvr.py --control): latency, recenter, and the Meta
and menu buttons held while pressed. A VR bridge it didn't start (SteamVR's library starts prism.py vr
--from-steamvr) it follows too: its log (work/logs/steamvr-run.log) goes to the Log, and the controls
to prism.py's control port. --vr starts VR as the window opens.
"""
import os
import queue
import re
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LOGS = os.path.join(ROOT, 'work', 'logs')


def fail(message):
    """A message box, as pythonw has no console to say why it stopped; it needs no tkinter."""
    import ctypes
    ctypes.windll.user32.MessageBoxW(None, message, 'Prism', 0x10)  # MB_ICONERROR
    sys.exit(1)


if sys.version_info < (3, 10):
    fail(f'Prism needs Python 3.10 or later, and this is {sys.version.split()[0]} ({sys.executable}).\n\n'
         'Install a newer one: winget install Python.Python.3.13, or python.org.')
try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    from tkinter.scrolledtext import ScrolledText
except ImportError:
    fail(f"Prism's window needs tkinter, which this Python ({sys.executable}) was installed without.\n\n"
         'Run its installer again, choose Modify, and tick "tcl/tk and IDLE".')
sys.path.insert(0, HERE)
import emulator  # noqa: E402
import prism  # noqa: E402

NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


class _WindowlessPopen(subprocess.Popen):
    """Under pythonw the window has no console, so each console program it starts (adb, tasklist:
    the pollers run them every few seconds) would get a console window of its own. Everything this
    process starts, through Prism's tools too, starts without one."""

    def __init__(self, *args, creationflags=0, **kwargs):
        super().__init__(*args, creationflags=creationflags | NO_WINDOW, **kwargs)


subprocess.Popen = _WindowlessPopen  # subprocess.run and the tools' calls look it up here
# python.exe for the commands, also when the window runs under pythonw.exe.
PYTHON = next((p for p in (os.path.join(os.path.dirname(sys.executable), 'python.exe'), sys.executable)
               if os.path.exists(p)), sys.executable)
LOG_LINES = 3000
GREY, AMBER, GREEN, RED = '#8a8a8a', '#e0a020', '#2fb350', '#d64040'
STAGES = {'stopped': ('Stopped', GREY), 'booting': ('Booting Android', AMBER), 'clock': ('Fixing the clock', AMBER),
          'starting': ('Starting Horizon', AMBER), 'ready': ('Ready', GREEN)}
# prism_vr's line once a second, shown as the bridge's status rather than logged.
VR_REPORT = re.compile(r'prism_vr: (focused|running|waiting for the session), (tracking|no tracking service), +'
                       r'([\d.]+) frames a second from Horizon, +([\d.]+) submitted, latency (\d+) ms')
VR_LATENCY = re.compile(r'prism_vr: latency (\d+) ms')


class Command:
    """A tools/prism.py command running in the background: its output lines go to `lines`, then
    (self, exit code) when it ends. The output goes through a file (work/logs/window-COMMAND.log),
    not a pipe, so a command left running when the window closes (a setup) can still print."""

    def __init__(self, args, lines):
        self.args, self.lines = args, lines
        os.makedirs(LOGS, exist_ok=True)
        self.log = os.path.join(LOGS, f'window-{args[0]}.log')
        with open(self.log, 'w', encoding='utf-8') as out:
            self.process = subprocess.Popen([PYTHON, '-u', os.path.join(HERE, 'prism.py'), *args], cwd=ROOT,
                                            stdin=subprocess.PIPE, stdout=out, stderr=subprocess.STDOUT,
                                            text=True, encoding='utf-8', bufsize=1, creationflags=NO_WINDOW,
                                            env={**os.environ, 'PYTHONIOENCODING': 'utf-8'})
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        partial = ''
        with open(self.log, encoding='utf-8', errors='replace') as log:
            while True:
                ended = self.process.poll() is not None  # before the read, so nothing written last is missed
                partial += log.read()
                *whole, partial = partial.split('\n')
                for line in whole:
                    self.lines.put(line.rstrip('\r'))
                if ended:
                    break
                time.sleep(0.1)
        if partial:
            self.lines.put(partial.rstrip('\r'))
        self.lines.put((self, self.process.returncode))

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
        subprocess.run(['taskkill', '/pid', str(self.process.pid), '/t', '/f'], capture_output=True,
                       creationflags=NO_WINDOW)


class Window:
    def __init__(self, root, start_vr=False):
        self.root = root
        self.settings = prism.load_settings()
        emulator.use_avd(self.settings['avd'])
        self.lines = queue.Queue()     # from commands: str, or (command, exit code)
        self.updates = queue.Queue()   # from the pollers: (what, value)
        self.command = None
        self.stopping = False
        self.external = False  # a VR bridge this window didn't start: SteamVR's
        self.control = None    # its control port, connected
        self.registered = None
        root.title('Prism')
        root.resizable(False, False)
        # Buttons take no keyboard focus: a key meant for another window (focus moves when the emulator
        # or a bridge opens) never starts or stops anything. Everything here is clicked.
        root.option_add('*TButton.takeFocus', 0)
        root.protocol('WM_DELETE_WINDOW', self.close)
        self.build()
        threading.Thread(target=self.poll_horizon, daemon=True).start()
        threading.Thread(target=self.poll_setup, daemon=True).start()
        threading.Thread(target=self.follow_steamvr, daemon=True).start()
        self.root.after(100, self.drain)
        if start_vr:
            self.root.after(500, lambda: self.start('vr'))

    # --- layout ---------------------------------------------------------------------------------

    def build(self):
        frame = ttk.Frame(self.root, padding=12)
        frame.grid(sticky='nsew')
        frame.columnconfigure(1, weight=1)

        ttk.Label(frame, text='Horizon', width=9).grid(row=0, column=0, sticky='w')
        horizon = ttk.Frame(frame)
        horizon.grid(row=0, column=1, sticky='w')
        self.dot = tk.Canvas(horizon, width=12, height=12, highlightthickness=0)
        self.dot_id = self.dot.create_oval(1, 1, 11, 11, fill=GREY, outline='')
        self.dot.pack(side='left', padx=(0, 6))
        self.horizon = ttk.Label(horizon, text='Checking…')
        self.horizon.pack(side='left')
        self.steamvr = ttk.Label(frame, text='', foreground=GREY)
        self.steamvr.grid(row=0, column=2, sticky='e')

        ttk.Label(frame, text='Bridge').grid(row=1, column=0, sticky='w')
        self.bridge = ttk.Label(frame, text='Not running', foreground=GREY)
        self.bridge.grid(row=1, column=1, columnspan=2, sticky='w')
        ttk.Label(frame, text='Setup').grid(row=2, column=0, sticky='w')
        self.setup_status = ttk.Label(frame, text='Checking…', foreground=GREY, wraplength=330)
        self.setup_status.grid(row=2, column=1, columnspan=2, sticky='w')

        buttons = ttk.Frame(frame)
        buttons.grid(row=3, column=0, columnspan=3, sticky='w', pady=(10, 6))
        self.vr_button = ttk.Button(buttons, text='Start VR', command=lambda: self.start('vr'))
        self.desktop_button = ttk.Button(buttons, text='Desktop', command=lambda: self.start('desktop'))
        self.stop_button = ttk.Button(buttons, text='Stop', command=self.stop)
        for b in (self.vr_button, self.desktop_button, self.stop_button):
            b.pack(side='left', padx=(0, 6))

        headset = ttk.LabelFrame(frame, text='Headset', padding=(8, 4))
        headset.grid(row=4, column=0, columnspan=3, sticky='ew', pady=(4, 6))
        ttk.Label(headset, text='Latency').grid(row=0, column=0, sticky='w')
        ttk.Button(headset, text='−', width=3, command=lambda: self.latency(-5)).grid(row=0, column=1)
        self.latency_label = ttk.Label(headset, width=7, anchor='center')
        self.latency_label.grid(row=0, column=2)
        ttk.Button(headset, text='+', width=3, command=lambda: self.latency(5)).grid(row=0, column=3)
        self.headset_buttons = [ttk.Button(headset, text='Recenter', command=lambda: self.send('recenter'))]
        self.headset_buttons[0].grid(row=0, column=4, padx=(12, 0))
        for column, (text, name) in enumerate((('Meta button', 'meta'), ('Menu button', 'menu'))):
            b = ttk.Button(headset, text=text)
            b.bind('<ButtonPress-1>', lambda e, n=name: self.send(f'{n} down'))
            b.bind('<ButtonRelease-1>', lambda e, n=name: self.send(f'{n} up'))
            b.grid(row=1, column=column * 2, columnspan=2, sticky='w', pady=(4, 0))
            self.headset_buttons.append(b)
        ttk.Label(headset, text='Hold to press', foreground=GREY).grid(row=1, column=4, sticky='e', pady=(4, 0))
        self.show_latency()

        self.settings_frame = self.expander(frame, 5, 'Setup and settings', self.build_settings)
        self.log_frame = self.expander(frame, 7, 'Log', self.build_log)
        self.update_buttons()

    def expander(self, parent, row, title, fill):
        """A section that opens and closes under a ▸ title."""
        body = ttk.Frame(parent)
        toggle = ttk.Label(parent, text=f'▸ {title}', cursor='hand2')
        toggle.grid(row=row, column=0, columnspan=3, sticky='w', pady=(4, 0))

        def flip(_event=None):
            if body.winfo_ismapped():
                body.grid_remove()
                toggle.configure(text=f'▸ {title}')
            else:
                body.grid(row=row + 1, column=0, columnspan=3, sticky='ew', pady=(4, 0))
                toggle.configure(text=f'▾ {title}')
        toggle.bind('<Button-1>', flip)
        fill(body)
        body.flip = flip
        return body

    def build_settings(self, body):
        ttk.Label(body, text='OTA').grid(row=0, column=0, sticky='w')
        self.ota = tk.StringVar(value=self.settings.get('ota') or '')
        ttk.Entry(body, textvariable=self.ota, width=34).grid(row=0, column=1, sticky='ew', padx=4)
        ttk.Button(body, text='Browse…', command=self.browse).grid(row=0, column=2)
        ttk.Label(body, text='Refresh').grid(row=1, column=0, sticky='w', pady=(4, 0))
        rates = ttk.Frame(body)
        rates.grid(row=1, column=1, sticky='w', pady=(4, 0))
        self.refresh = tk.IntVar(value=self.settings['refresh'])
        for rate in (60, 90):
            ttk.Radiobutton(rates, text=f'{rate} Hz', value=rate, variable=self.refresh,
                            command=self.save).pack(side='left', padx=(0, 8))
        ttk.Label(body, text='IPD (mm)').grid(row=2, column=0, sticky='w', pady=(4, 0))
        self.ipd = tk.DoubleVar(value=self.settings['ipd'])
        ttk.Spinbox(body, from_=50, to=80, increment=0.5, textvariable=self.ipd, width=6,
                    command=self.save).grid(row=2, column=1, sticky='w', padx=4, pady=(4, 0))
        self.menu_is_menu = tk.BooleanVar(value=self.settings['menu_button_is_menu'])
        ttk.Checkbutton(body, text="Left menu button is Horizon's menu button (not Meta)", variable=self.menu_is_menu,
                        command=self.save).grid(row=3, column=0, columnspan=3, sticky='w', pady=(4, 0))
        actions = ttk.Frame(body)
        actions.grid(row=4, column=0, columnspan=3, sticky='w', pady=(8, 0))
        self.setup_button = ttk.Button(actions, text='Set up', command=lambda: self.start('setup'))
        self.setup_button.pack(side='left', padx=(0, 6))
        self.doctor_button = ttk.Button(actions, text='Check this PC', command=lambda: self.start('doctor'))
        self.doctor_button.pack(side='left', padx=(0, 6))
        self.library_button = ttk.Button(actions, text="Add to SteamVR's library", command=self.library)
        self.library_button.pack(side='left')
        body.columnconfigure(1, weight=1)

    def build_log(self, body):
        self.log = ScrolledText(body, width=62, height=14, font=('Consolas', 9), state='disabled', wrap='none')
        self.log.grid(row=0, column=0, sticky='ew')
        ttk.Button(body, text='Open logs folder',
                   command=lambda: os.startfile(LOGS)).grid(row=1, column=0, sticky='w', pady=(4, 0))

    # --- settings -------------------------------------------------------------------------------

    def save(self):
        try:
            ipd = float(self.ipd.get())
        except (tk.TclError, ValueError):
            ipd = self.settings['ipd']
        self.settings.update({'ota': self.ota.get().strip() or None, 'refresh': self.refresh.get(), 'ipd': ipd,
                              'menu_button_is_menu': self.menu_is_menu.get()})
        prism.save_settings(self.settings)

    def browse(self):
        path = filedialog.askopenfilename(title="Your headset's full Horizon OS OTA",
                                          filetypes=[('OTA', '*.zip *.bin'), ('All files', '*.*')])
        if path:
            self.ota.set(os.path.normpath(path))
            self.save()

    def show_latency(self):
        self.latency_label.configure(text=f"{self.settings['latency']:.0f} ms")

    def latency(self, delta):
        self.settings['latency'] = max(0.0, self.settings['latency'] + delta)
        prism.save_settings(self.settings)
        self.show_latency()
        self.send(f"latency {self.settings['latency']:.0f}")

    # --- commands -------------------------------------------------------------------------------

    def vr_running(self):
        return self.command is not None and self.command.running() and self.command.args[0] == 'vr'

    def send(self, line):
        if self.vr_running():
            self.command.send(line)
        elif self.external:
            for _ in range(2):  # once more on a new connection, if the last one went with its bridge
                try:
                    if self.control is None:
                        self.control = socket.create_connection(('127.0.0.1', prism.CONTROL_PORT), timeout=1)
                    self.control.sendall((line + '\n').encode())
                    return
                except OSError:
                    self.disconnect()

    def disconnect(self):
        if self.control:
            self.control.close()
            self.control = None

    def library(self):
        self.start('steamvr', 'unregister' if self.registered else 'register')

    def start(self, what, *more):
        if self.command and self.command.running():
            return
        self.save()
        args = [what, *more]
        if what == 'vr':
            args.append('--control')
        if what == 'setup' and self.settings.get('ota'):
            args.append(self.settings['ota'])
        self.write(f'> prism.py {" ".join(args)}')
        if what == 'setup' and not self.log_frame.winfo_ismapped():
            self.log_frame.flip()
        self.command = Command(args, self.lines)
        self.update_buttons()

    def stop(self):
        """Ends what's running and stops Horizon (prism.py stop: the bridges and the emulator)."""
        if self.command and self.command.running():
            if self.vr_running():
                self.command.send('quit')
            self.command.kill()
        self.stopping = True
        self.write('> prism.py stop')
        self.command = Command(['stop'], self.lines)
        self.update_buttons()

    def update_buttons(self):
        busy = bool(self.command and self.command.running())
        for b in (self.setup_button, self.doctor_button, self.library_button):
            b.state(['disabled'] if busy else ['!disabled'])
        for b in (self.vr_button, self.desktop_button):
            b.state(['disabled'] if busy or self.external else ['!disabled'])
        for b in self.headset_buttons:
            b.state(['!disabled'] if self.vr_running() or self.external else ['disabled'])
        if self.external and not busy:
            if self.bridge.cget('text') == 'Not running':
                self.bridge.configure(text='VR: started from SteamVR', foreground=AMBER)
        elif not busy:
            self.bridge.configure(text='Not running', foreground=GREY)

    def write(self, line):
        self.log.configure(state='normal')
        self.log.insert('end', line + '\n')
        excess = int(self.log.index('end-1c').split('.')[0]) - LOG_LINES
        if excess > 0:
            self.log.delete('1.0', f'{excess + 1}.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def on_line(self, line):
        report = VR_REPORT.search(line)
        if report:
            state, tracking, horizon, submitted, _ms = report.groups()
            ok = state == 'focused' and tracking == 'tracking'
            self.bridge.configure(text=f'VR: {state}, {tracking} · {float(horizon):.0f} fps from Horizon, '
                                       f'{float(submitted):.0f} to the headset', foreground=GREEN if ok else AMBER)
            return
        latency = VR_LATENCY.search(line)
        if latency:
            self.settings['latency'] = float(latency.group(1))
            self.show_latency()
        if line.startswith('== '):
            self.setup_status.configure(text=line[3:], foreground=AMBER)
            if line.startswith('== starting '):
                mode = {'vr': 'VR', 'desktop': 'Desktop'}.get(line[12:], line[12:])
                self.bridge.configure(text=f'{mode}: starting', foreground=AMBER)
        self.write(line)

    def on_exit(self, command, code):
        name = command.args[0]
        self.write(f'< prism.py {name}: {"done" if code == 0 else f"exit {code}"}')
        if code != 0 and name in ('setup', 'vr', 'desktop', 'doctor') and not self.stopping:
            if not self.log_frame.winfo_ismapped():
                self.log_frame.flip()
        if name == 'stop':
            self.stopping = False
        self.update_buttons()
        self.refresh_setup = True

    # --- polling --------------------------------------------------------------------------------

    def poll_horizon(self):
        while True:
            try:
                stage = emulator.stage()
            except (Exception, SystemExit) as e:  # no SDK, or adb misbehaving: say so, keep polling
                stage = ('stopped', str(e))
            vr = prism.bridge_running('vrserver.exe')
            self.updates.put(('horizon', stage))
            self.updates.put(('steamvr', vr))
            self.updates.put(('bridge', prism.bridge_running(prism.BRIDGES['vr'])))
            time.sleep(3)

    refresh_setup = True

    def poll_setup(self):
        last = 0
        while True:
            if self.refresh_setup or time.time() - last > 30:
                self.refresh_setup, last = False, time.time()
                todo, unknown = [], []
                for name, state, _ in prism.STEPS:
                    try:
                        current = state(self.settings)[0]
                    except (Exception, SystemExit):
                        current = None
                    (todo if current is False else unknown if current is None else []).append(name)
                self.updates.put(('setup', (todo, unknown)))
                self.updates.put(('library', prism.steamvr_registered()))
            time.sleep(1)

    def follow_steamvr(self):
        """While a bridge SteamVR started runs, its log's new lines go where a command's would."""
        log, offset, partial = None, 0, ''
        while True:
            if not self.external:
                if log:
                    log.close()
                    log = None
                time.sleep(0.5)
                continue
            if log is None:
                try:
                    log, offset, partial = open(prism.STEAMVR_LOG, encoding='utf-8', errors='replace'), 0, ''
                    self.lines.put('> started from SteamVR (work/logs/steamvr-run.log)')
                except OSError:
                    time.sleep(0.5)
                    continue
            try:
                rewritten = os.path.getsize(prism.STEAMVR_LOG) < offset  # by a new start
            except OSError:
                rewritten = False
            if rewritten:
                log.seek(0)
                partial = ''
            partial += log.read()
            offset = log.tell()
            *whole, partial = partial.split('\n')
            for line in whole:
                self.lines.put(line.rstrip('\r'))
            time.sleep(0.25)

    def drain(self):
        while True:
            try:
                item = self.lines.get_nowait()
            except queue.Empty:
                break
            if isinstance(item, tuple):
                self.on_exit(*item)
            else:
                self.on_line(item)
        while True:
            try:
                what, value = self.updates.get_nowait()
            except queue.Empty:
                break
            if what == 'horizon':
                stage, detail = value
                text, color = STAGES.get(stage, (stage, GREY))
                self.dot.itemconfigure(self.dot_id, fill=color)
                self.horizon.configure(text=f'{text}{" · " + detail if detail else ""}')
            elif what == 'bridge':
                external = value and not self.vr_running()
                if external != self.external:
                    self.external = external
                    if not external:
                        self.disconnect()
                    self.update_buttons()
            elif what == 'library':
                self.registered = value
                self.library_button.configure(text="Remove from SteamVR's library" if value else "Add to SteamVR's library")
            elif what == 'steamvr':
                self.steamvr.configure(text='SteamVR on' if value else 'SteamVR off', foreground=GREEN if value else GREY)
            elif what == 'setup' and not (self.command and self.command.running() and self.command.args[0] == 'setup'):
                todo, unknown = value
                if todo:
                    text, color = f'To do: {", ".join(todo)} (Start runs it first)', AMBER
                elif unknown:
                    text, color = f'Current; {", ".join(unknown)} checked once Horizon runs', GREEN
                else:
                    text, color = 'Current', GREEN
                if not os.path.exists(prism.REPORT) and not self.settings.get('ota'):
                    text, color = 'Choose your OTA under Setup and settings, then Set up', RED
                self.setup_status.configure(text=text, foreground=color)
        self.root.after(100, self.drain)

    def close(self):
        busy = self.command and self.command.running()
        if busy or emulator.running():
            answer = messagebox.askyesnocancel('Prism', 'Stop Horizon too?\n\nNo leaves the emulator running, '
                                               'so the next start is quick.')
            if answer is None:
                return
            if answer:
                if busy:
                    self.command.send('quit')
                    self.command.kill()
                subprocess.Popen([PYTHON, os.path.join(HERE, 'prism.py'), 'stop'], cwd=ROOT, creationflags=NO_WINDOW)
            elif busy:
                self.command.close_input()  # the VR bridge quits with its input; a build or setup runs on, into its log
        self.root.destroy()


def main():
    root = tk.Tk()
    Window(root, start_vr='--vr' in sys.argv[1:])
    root.mainloop()


if __name__ == '__main__':
    try:
        main()
    except Exception:
        import traceback
        os.makedirs(LOGS, exist_ok=True)
        with open(os.path.join(LOGS, 'window-crash.log'), 'w', encoding='utf-8') as f:
            traceback.print_exc(file=f)
        fail(f"Prism's window stopped: {sys.exc_info()[1]}\n\nThe details are in work\\logs\\window-crash.log.")
