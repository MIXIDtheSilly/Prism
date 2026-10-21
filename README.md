![Prism](img/prism_banner.png)

[![Support Prism on Ko-fi](https://img.shields.io/badge/Ko--fi-Support%20Prism-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/mixid_mbb)

Prism is an experimental Horizon OS emulator for Windows PCs. It aims to run Meta's own Horizon
OS system software (the home environment, system UI, panels and apps) on a PC, in a desktop window
first and later in a PC VR headset.

> **Status:** Horizon runs in a desktop window and in a PC VR headset through SteamVR. It boots to
> Meta's home environment, and you can look around, point at and click panels, open the Universal
> Menu and the library, and launch apps: Android apps open as panels, and OpenXR games run as
> immersive apps on Meta's own runtime and compositor. Next: a launcher that sets up and starts
> Prism in one step (M6). See
> [Progress](#progress), [docs/plan.md](docs/plan.md) and
> [docs/m1-progress.md](docs/m1-progress.md).

## Support Prism

I'm a student, and I build Prism in my spare time. Most of that time goes into reverse engineering:
working out what Meta's software expects from a real Quest and then giving it that from a PC.
Getting this far meant things like:

- rebuilding the C API of Meta's space manager from its binaries, so panels know where to go;
- finding the one 16-bit field at offset 0xe38 that tells Meta's runtime a controller is actually
  in your hand, before VrShell would draw it;
- stopping a system bar that asked for a Meta account 40,000 times a second until Android ran out
  of memory and took the emulator down with it.

If you'd like to see Horizon OS running on a PC, a coffee on Ko-fi helps a lot. It keeps the
late-night debugging going and gets the next milestone (a one-step launcher) here sooner.

[![Buy me a coffee on Ko-fi](https://img.shields.io/badge/Ko--fi-Buy%20me%20a%20coffee-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/mixid_mbb)

Not in a position to donate? Starring the repo, trying Prism on your own headset's OTA and
reporting what breaks helps too.

## Progress

| Milestone | State |
|---|---|
| M0: Extract the OTA and take inventory | Done |
| M1: Boot Horizon's Java layer | Done |
| M2: Native daemons and hardware services | Done for what Home and apps need |
| M3: Home in a desktop window | Done |
| M4: More than one app at a time | In progress |
| M5: PC VR headset | Working: Horizon in a headset through SteamVR |
| M6: Launcher and packaging | In progress: the launcher, its window and SteamVR's library |

What works:

- **Boot.** Horizon boots to VrShell, Meta's home shell. Meta's native daemons and apps run as
  arm64 code translated to x86_64 by the Digitalis translator, and system_server runs every Meta
  service.
- **Meta's XR stack.** Meta's OpenXR runtime and compositor run translated, drawing on the PC's
  GPU. The compositor runs at the display's rate: 60 frames a second by default, or 90 (see below).
- **The desktop viewer** (`tools\viewer.py`). The mouse aims the right controller's ray, the left
  button pulls its trigger, Tab presses the Meta button, right-drag looks around, and WASD and R/F
  move.
- **A PC VR headset** (`tools\steamvr.py`). The headset's and controllers' tracking, buttons and
  thumbsticks go to Horizon through SteamVR, Horizon's frames go to the headset, and haptics come
  back to the controllers.
- **Tracking.** The headset and two Touch Plus controllers are tracked, paired and held, all driven
  from the PC. VrShell draws the controllers and their rays.
- **Panels.** Panels draw and respond to clicks: the Universal Menu, the system bar and Quick
  Settings, and the library with Horizon's own apps.
- **Apps.** Android apps installed with adb open as panels. OpenXR apps (Vulkan or OpenGL ES) run
  as Horizon's immersive app at full frame rate. They launch from the library's Unknown Sources,
  and the Universal Menu opens over them and quits them.
- **Offline.** First-time setup is skipped and Horizon runs without a Meta account; the library
  has the apps on the device.

Not yet:

- Panels' frosted-glass backgrounds.
- A steady 90 frames a second in Home: it reaches 89.7 shared frames a second (p99 14.6 ms) once the
  emulator's frame sharing is patched and the display set to 90 Hz, but VrShell's frame loop is
  at the edge of its 11.1 ms budget. See [docs/m1-progress.md](docs/m1-progress.md#running-it).
- Passthrough (black: there are no cameras).
- The Store, Meta accounts and other online features. These are out of scope (see
  [Scope](#scope)).

## How it works

Horizon OS is Android 14 plus a large Meta layer. Prism keeps the bottom of the stack from the
x86_64 Android Emulator (kernel, drivers, GPU forwarding to the PC's graphics card) and puts
Horizon's own software on top of it:

```text
Meta apps (VrShell, SystemUX, ...)       Horizon APKs, arm64 code translated to x86_64
Horizon Java framework + system_server   Horizon jars, recompiled by ART for x86_64
Meta native daemons                      Horizon arm64 binaries, translated
Hardware services (tracking, cameras,    Prism reimplementations, driven from the PC
  calibration, guardian, display)
Kernel, HALs, GPU                        Android Emulator x86_64 (Goldfish + Gfxstream)
```

## Bring your own OTA

This repository contains no Horizon OS files and never will. You supply a full OTA for your own
headset; Prism unpacks and analyses it locally. Everything derived from it goes to `work/`, which
is ignored by git.

### What to install first

| What | How |
|---|---|
| Windows 10 or 11 with virtualization on | Turn on virtualization (VT-x or AMD-V/SVM) in the BIOS, then Windows Hypervisor Platform: as administrator, `dism /online /enable-feature /featurename:HypervisorPlatform /all`, and restart |
| Python 3.10 or later | `winget install Python.Python.3.13` (or python.org, with *Add python.exe to PATH* ticked) |
| Java 17 or later | `winget install EclipseAdoptium.Temurin.21.JDK` |
| Visual Studio's C++ tools | Visual Studio with *Desktop development with C++*, or just the build tools: `winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"` |
| Git | `winget install Git.Git` |
| SteamVR (for a headset) | From Steam; *Desktop* works without it |
| 16 GB of memory and 8 CPU threads | Horizon gets 12 GB and 6 cores. With 12 GB, or 4 to 7 threads, setup gives it less (8 GB, 4 cores) and it runs slower |
| A graphics card with a current driver | Prism is tested on NVIDIA (an RTX 3080); AMD and Intel may work but haven't been tried. On a laptop with two GPUs, *Check this PC* says if the emulator draws on the integrated one |
| About 60 GB free | The workspace and the emulator's disk |

Open a new terminal after installing, so the new programs are on its PATH.

You don't need to install the Android SDK yourself: *Set up* installs the parts Prism needs (the
emulator, platform-tools, NDK 27 and `system-images;android-34;google_apis;x86_64`, the stock
baseline Prism builds on) with Google's `sdkmanager`, and accepts the Android SDK's licenses as it
does. It uses the SDK Android Studio has if there is one, or creates one at
`%LOCALAPPDATA%\Android\Sdk`. Prism builds with NDK 27 even when a newer one is installed, and is
tested with emulators 37.1.11 and 37.2.12.

### Setting up

Open **`Prism.cmd`**: a small window that does the rest. Under *Setup and settings*, pick your
OTA, then *Check this PC* (what's missing and how to get it) and *Set up*. The first setup installs
the SDK's parts, extracts the OTA, creates the emulator, builds Prism's parts and deploys Horizon
onto it, which takes a while; after that, setting up again does only what changed. Then *Start VR* (or *Desktop*): it waits
until Horizon's home is drawing and opens it in the headset, with the latency, recenter and the
Meta and menu buttons in the window.

*Add to SteamVR's library* (under *Setup and settings*) lists Prism in SteamVR as **Horizon OS
(Prism)**, so it starts from the headset too: the window, if it is open, shows that run and its
controls work on it. Quitting it from SteamVR leaves the emulator running, so the next start is
quick.

The same from a terminal:

```powershell
python tools\prism.py doctor
python tools\prism.py setup path\to\horizon-ota.zip
python tools\prism.py vr        # or: desktop; stop shuts Horizon down
python tools\prism.py steamvr register   # Prism in SteamVR's library
```

`setup` extracts the partitions, files and APEX modules, unpacks the stock emulator image and writes
an inventory to `work\inventory\report.md`, then runs the steps in
[docs/m1-progress.md](docs/m1-progress.md#running-it), each only when it isn't already current.

### When it doesn't work

- Run *Check this PC* (`python tools\prism.py doctor`) first: each line that isn't `ok` says how to
  fix it.
- A failed *Set up* names the step that failed. What it printed is in the Log and in
  `work\logs\window-setup.log`; run *Set up* again after fixing it, and it picks up from there.
- If the emulator doesn't start or Horizon doesn't reach Home, `python tools\emulator.py status`
  says how far it got, and `work\logs\emulator.log` has the emulator's own output.
- Paths with characters outside plain ASCII (a user name with accents or another alphabet, for
  instance) can trip up the emulator and the NDK. *Check this PC* warns about them.

To report a problem, [open an issue](../../issues/new?template=setup-problem.md) and attach
the zip *Make a report* (`python tools\prism.py report`) makes right after the failure: it has
what *Check this PC* says and the logs, with your user and computer names replaced.

## Repository layout

```text
tools/sdk.py          The Android SDK's parts Prism needs, installed with sdkmanager
tools/prepare.py      One-command workspace setup from an OTA
tools/ota/            OTA payload, ext4, APEX and super-partition unpackers (pure Python)
tools/inventory/      Horizon vs stock Android analysis (dex, binary XML, ELF readers)
tools/emulator.py     Prism's AVD: create, start, root, status
tools/jni/            Builds Prism's JNI glue for the user's Horizon build
tools/compat.py       Compatibility patches in Horizon's framework jars (via smali)
tools/translator.py   Adapts the Digitalis ARM64 translator to Prism's Android 14 base
tools/vulkan.py       Builds Prism's Vulkan driver (wraps the emulator's)
tools/gles.py         Builds Prism's GLES layer (shared swapchains and multiview for GLES apps)
tools/tracking.py     Builds Prism's tracking service (Meta's MemoryBroker, head and controllers)
tools/hidl.py         Builds Prism's HIDL services (Meta's controller HAL, system suspend)
tools/thermal.py      Builds Prism's thermal and maintenance boot HALs
tools/spaces.py       Builds Prism's space service (Meta's space manager, for system_server)
tools/relay.py        Builds the binder bridge between arm64 apps and the host's Java binder
tools/guest_media.py  Builds the arm64 media functions Meta's libraries expect
tools/head.py         Moves the headset from the PC: mouse and keys drive the head pose
tools/perf.py         Measures shared frame rate, jitter and guest CPU (the benchmark for frame-rate work)
tools/soak.py         Leaves Horizon running, opening and closing apps, and records frames, memory and crashes
tools/stamp.py        Whether a build is current: a stamp of what it was made from
tools/prism.py        The launcher: doctor, setup (each step only when not current), vr, desktop, stop, report
tools/prism_ui.pyw    The launcher's window (Prism.cmd opens it)
tools/viewer.py       Horizon in a desktop window: the mouse points and clicks, keys move
tools/steamvr.py      Horizon in a PC headset through SteamVR (OpenXR): its tracking and frames
tools/xrprobe.py      Builds and runs Prism's OpenXR test apps (xrprobe, xrdemo, xrgame)
tools/smali.py        smali/baksmali, fetched on first use
tools/deploy.py       Puts Horizon's layer onto the emulator
native/prism_jni/     JNI glue between Horizon's Java and stock native code
native/berberis_compat/  Symbols the Android 16 translator build needs from Android 14
native/vulkan_prism/  Vulkan driver: gfxstream plus opaque-fd memory for Meta's compositor
native/gles_prism/    GLES layer: GL_EXT_memory_object_fd and multiview for the emulator's GLES
native/tracking_prism/   Tracking service: the head, the controllers and their tracking
native/hidl_prism/    Meta's controller HAL and the HIDL system suspend
native/thermal_prism/ Thermal HAL and maintenance boot HAL
native/spaces_prism/  Space service over Meta's space manager (its C API, reconstructed)
native/binder_relay/  Binder bridge between an app's arm64 libbinder and the host's Java binder
native/guest_media/   arm64 media functions Meta's libraries expect
native/viewer/        The desktop viewer (Direct3D 11)
native/steamvr/       The SteamVR host (OpenXR, Direct3D 11)
native/xrprobe/       OpenXR test app: logs what Horizon's runtime gives an app
native/xrdemo/        OpenXR test app that draws (xrdemo, xrgame, xrgles)
prebuilts/digitalis/  The Digitalis translator (Apache 2.0; see its NOTICE)
docs/                 Plan, findings and progress
```

## Scope

Prism is for running software from a headset you own, for research and development. It does not
fake Quest device identity or attestation, and Meta account sign-in and the Store are out of
scope. Do not use it to bypass DRM, platform security or license terms.

## License

Prism is source-available, not open source. You may download, build, run and modify it for your
own personal use, and fork it on GitHub to send contributions. You may not sell it or
redistribute it, modified or not, in source or binary form. See [LICENSE](LICENSE) for the full
terms. Third-party components under `prebuilts/` keep their own licenses.
