# Prism

Prism is an experimental Horizon OS emulator for Windows PCs. It aims to run Meta's own Horizon
OS system software (the home environment, system UI, panels and apps) on a PC, in a desktop window
first and later in a PC VR headset.

> **Status:** M1. Horizon's framework reaches boot completed on the emulator, with Meta's first
> native daemons running as translated arm64; reimplementing Meta's hardware-facing layers is
> next. See [docs/plan.md](docs/plan.md) and
> [docs/m1-progress.md](docs/m1-progress.md).

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

Requirements: Python 3.10+, and the Android SDK with
`system-images;android-34;google_apis;x86_64` (the stock baseline Prism compares against and builds
on).

```powershell
python tools\prepare.py path\to\horizon-ota.zip
```

This extracts the partitions, files and APEX modules, unpacks the stock emulator image, and writes
an inventory to `work\inventory\report.md`.

## Repository layout

```text
tools/prepare.py      One-command workspace setup from an OTA
tools/ota/            OTA payload, ext4, APEX and super-partition unpackers (pure Python)
tools/inventory/      Horizon vs stock Android analysis (dex, binary XML, ELF readers)
tools/emulator.py     Prism's AVD: create, start, root, status
tools/jni/            Builds Prism's JNI glue for the user's Horizon build
tools/compat.py       Compatibility patches in Horizon's framework jars (via smali)
tools/translator.py   Adapts the Digitalis ARM64 translator to Prism's Android 14 base
tools/deploy.py       Puts Horizon's layer onto the emulator
native/prism_jni/     JNI glue between Horizon's Java and stock native code
native/berberis_compat/  Symbols the Android 16 translator build needs from Android 14
prebuilts/digitalis/  The Digitalis translator (Apache 2.0; see its NOTICE)
docs/                 Plan, findings and progress
```

## Scope

Prism is for running software from a headset you own, for research and development. It does not
fake Quest device identity or attestation, and Meta account sign-in and the Store are out of
scope. Do not use it to bypass DRM, platform security or license terms.
