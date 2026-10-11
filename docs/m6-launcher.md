# M6: Launcher and packaging (plan)

Goal: Prism runs from a small window. You pick the OTA once and press Set up, and after that you
press Start VR. The same steps are also commands, which the window runs:

```powershell
python tools\prism.py setup path\to\horizon-ota.zip   # once, and again after pulling changes
python tools\prism.py vr                              # or: desktop
```

Done when a fresh clone on a PC with Python, the Android SDK and SteamVR gets from Set up to
Horizon in the headset from the window alone, when setting up again with nothing changed finishes
in seconds, and when Prism can be started from SteamVR's library.

## Where it is now

- A first run is 15 commands ([m1-progress.md](m1-progress.md#running-it)), plus 5 for 90 Hz, in
  an order that matters. The list is also incomplete: deploy takes `work/build/gles`, `hidl` and
  `relay` too (`tools/gles.py`, `hidl.py`, `relay.py`), which it doesn't name.
- Only `viewer.py` and `steamvr.py` know whether their build is current (output newer than
  sources). The guest-side builds rebuild every time or not at all, and nothing says when deploy is
  out of date with them, so after a pull you have to know what changed.
- `emulator.py wait` ends at `sys.boot_completed`, a minute or more before VrShell draws Home, and
  whether a boot lands on the TSC is luck (`ensure_tsc` reboots until it does).
- `vr` means running the emulator and `steamvr.py` yourself, with the emulator started first.

## Design

One entry point, `tools/prism.py`, which calls the existing tools' functions; the tools keep working
on their own. `prism.cmd` at the repository root runs it, so `prism vr` works from that folder.

| Command | What it does |
|---|---|
| `setup OTA [--refresh 60\|90]` | Every step below that isn't current, in order; 90 Hz by default |
| `vr [--latency ms] [--ipd mm]` | Starts Horizon if it isn't running, waits until it's ready, runs `steamvr.py` |
| `desktop` | The same with `viewer.py` |
| `stop` | Stops the bridge and the emulator |
| `doctor` | Checks everything `setup` and `vr` need and says what is missing and how to get it |
| `steamvr register\|unregister` | Prism's entry in SteamVR's library |
| `ui` | The window (below) |

### The window

`tools/prism_ui.pyw`, in tkinter: it comes with Python on Windows, so Prism gains no dependency.
`Prism.cmd` at the repository root opens it with `pythonw` (no console), so it can be
double-clicked or pinned to the taskbar.

```text
┌ Prism ──────────────────────────────────────────┐
│ Horizon   ● Ready (90 fps)        SteamVR ● On  │
│ Setup     ✓ Current               OTA  ✓ Found  │
│                                                 │
│ [ Start VR ]  [ Desktop ]  [ Stop ]             │
│                                                 │
│ Headset   Latency [−] 45 ms [+]   [ Recenter ]  │
│           [ Meta button ]  [ Menu button ]      │
│                                                 │
│ ▸ Setup and settings                            │
│ ▸ Log                                           │
└─────────────────────────────────────────────────┘
```

- **Status:** the emulator (stopped, booting, clock, Horizon starting, ready at N frames a
  second), SteamVR (running or not), and whether setup is current. When it isn't, the line names
  what changed ("tracking rebuilt, needs deploy") and Start offers to bring it up to date first.
- **Start VR, Desktop, Stop** run `prism.py vr`, `desktop` and `stop` as child processes. Their
  output goes to the log, so a crash in the bridge never takes the window down, and Stop ends the
  whole process tree.
- **Headset controls** while VR runs: latency, recenter and the two buttons, as the console's keys
  and F7 to F9 work now. They need a control channel into `prism_vr`, which reads keys from its
  console with `_getch` today: a `--control` option that also reads stdin lines (`latency 50`,
  `recenter`, `meta down`, `meta up`), which the window writes. The values the bridge has are shown
  on its status line, as it prints them now.
- **Setup and settings** (collapsed): the OTA file (a file dialog), Set up and its progress, step
  by step, as `setup` prints it, the refresh rate (60 or 90), IPD, menu-button mapping, and
  Register in SteamVR. These are saved in `work/prism.json`.
- **Log** (collapsed): the running command's output, plus a button that opens `work/logs`.
- The window polls the status every few seconds through the same functions `doctor` and
  `ready()` use, so what it shows is what the commands would find.

### Setup's steps

Each step says whether it's current and is skipped when it is. `setup --force STEP` reruns one.

| Step | Current when |
|---|---|
| prepare (OTA) | `work/` has the images and inventory of this OTA (its size and SHA-256 recorded at the end of prepare) |
| AVD create, config, patch-share-vid | The AVD exists with Prism's `config.ini` values; the qemu binaries carry the patch (`sharer_site`) |
| root | The guest's system partitions remount writable (verity off) |
| jni, compat, translator, vulkan, gles, thermal, tracking, hidl, relay, spaces, guest_media | The build's outputs are newer than its sources (`native/<part>`, its `tools/*.py`, and the OTA for the parts built against it) |
| deploy | The guest's `/data/prism/deploy-id` equals the hash of what deploy would push now (`work/deploy`) |
| viewer, steamvr hosts | As now (mtime), built ahead so `vr` starts at once |

One helper, `stale(outputs, sources)`, shared by the build tools, replaces the separate checks. The
deploy id is written onto the guest by deploy itself, so a wiped or recreated AVD is seen as not
deployed, whatever `work/` says.

`setup` starts the emulator for the steps that need it (root, deploy) and leaves it running.

### Ready, not just booted

`emulator.ready()`: `sys.boot_completed`, the clock on the TSC (rebooting as `ensure_tsc` does),
VrShell's process up, and the shared frames' counter moving at the display's rate (the VideoInfo
`perf.py` reads). `vr` and `desktop` wait for this, with one progress line: booting, clock, Horizon
starting, Home drawing. Past a timeout it prints `emulator.py status` and the last crash.

### Running and stopping

- `vr` starts the emulator headless if it isn't running (the window costs frames at 90 Hz),
  rebuilds and redeploys first only if `setup` would, and then runs `steamvr.py` in the foreground.
- When the bridge exits (closed, Ctrl+C, SteamVR quit), the emulator keeps running, since a boot
  takes minutes. `vr` again reconnects at once. `vr --stop-after` and `prism stop` shut it down.
- One emulator and one bridge at a time: a second `vr` finds the running bridge and says so.

### SteamVR's library

A `.vrmanifest` (app key `prism.horizon`, a name, an image from `img/`), whose binary is Python with
`tools\prism.py vr --from-steamvr` as its arguments, registered with
`IVRApplications::AddApplicationManifest` (openvr_api.dll, which SteamVR ships). Launched this way
there is no console: output goes to `work/logs/steamvr-run.log`, and the window, if it's open,
shows it and takes over the controls; the latency comes from `work/prism.json`.

To check first: SteamVR ties a launched app to the process it started, and the OpenXR session
belongs to `prism_vr.exe`, a child of Python. If SteamVR doesn't count the child as the app (it
would show "loading" and then kill it), the manifest points at `prism_vr.exe` and that starts the
emulator through `prism.py` itself.

### Settings

`work/prism.json` keeps what is set once rather than each run: the refresh rate, `--latency`,
`--ipd`, `--menu-button-is-menu`. Command-line flags override it for one run.

## Phases

1. **Plumbing.** *Done (2026-10-11).* `tools/stamp.py` in the eleven build tools (`--force` to
   build anyway): rerun with nothing changed, all eleven take 2 s together, against minutes for a
   full build (compat alone 8.5); a touched source rebuilds only its part. Deploy writes its id to
   `/data/prism/deploy-id` (the next deploy is the first to). `emulator.stage()` (one adb call and
   a second of frames) and `emulator.py ready` reach "ready: 89 frames a second" on a running Home.
2. **`setup` and `doctor`.** *Done (2026-10-11)*, but for the run from a fresh clone and new AVD. Done when a fresh clone with a new AVD (an `--avd` option, since
   `emulator.py` has the one name, `prism-api34`, today) goes from
   the OTA to Horizon running, and a second `setup` finishes in seconds. `doctor` names a missing
   SDK, system image, SteamVR, OTA or patch, with the command that fixes each one (the system image
   through `sdkmanager`).
3. **`vr`, `desktop`, `stop`.** *Built (2026-10-11).* To check in the headset. Done when `vr` from a stopped emulator reaches the headset with no
   other command; closing the bridge and running `vr` again reconnects without a boot; and a
   source changed, then `vr`, rebuilds and redeploys only that part.
4. **The window.** *Built (2026-10-11)*, the headset controls included; it also follows a VR
   bridge SteamVR started (2026-10-12). To check in the headset. The status, Start VR, Desktop and Stop first, then Set up with its OTA picker and
   progress, then the headset controls (with `prism_vr`'s `--control`). Done when everything from
   a fresh clone to Horizon in the headset, the controls included, works from the window without a
   console.
5. **SteamVR's library.** *Built (2026-10-12).* `prism.py steamvr register` writes
   `work/steamvr/prism.vrmanifest` (pythonw running `prism.py vr --from-steamvr`) and adds it through
   `IVRApplications_007` while SteamVR runs, or to `appconfig.json` while it doesn't; the window has
   the button, and `doctor` says whether it's there. Launched from it, the bridge starts before setup
   and the boot, so SteamVR gets its session at once, and its controls come over `CONTROL_PORT`
   (7341), which the window uses for a bridge it didn't start. The emulator breaks away from the
   launcher's job. Tested without a headset: the launch as SteamVR makes it (pythonw, the log, setup,
   ready at 88 frames a second, the bridge started) and the window driving a stand-in bridge over the
   port. Still to check in the headset: whether SteamVR counts `prism_vr.exe`, a grandchild, as the
   app. Done when Prism starts from the library in the headset, and quitting it
   from SteamVR's dashboard leaves the emulator running for the next start.
6. **Docs.** *Done (2026-10-12).* README's quick start becomes: open `Prism.cmd`, pick the OTA, Set up, Start VR. The
   commands are listed under it, and [m1-progress.md](m1-progress.md)'s commands stay as what
   `setup` does underneath.
7. **Experiment: boot from a snapshot.** A quick-boot snapshot taken once Home is drawn would skip
   the boot and the TSC check. The emulator's snapshots may not keep gfxstream's Vulkan state, which
   Meta's compositor depends on, so this is a test before a feature: Home drawing after a restore,
   at 90 frames a second (`perf.py --guest`).

## Not in M6

- A larger app: the window starts, stops and sets up Prism; Horizon itself is used in the headset
  or the viewer, not from it.
- A single installer or `.exe` (Python, the SDK and the OTA stay the user's to provide), and
  anything that ships Horizon's files ([Scope](../README.md#scope)).
