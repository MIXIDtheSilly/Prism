---
name: Setup or start problem
about: Prism doesn't set up, start, or reach Home on your PC
labels: setup
---

**What happened**

Which button or command (Set up, Start VR, Desktop, or `python tools\prism.py ...`), and what you
saw.

**Check this PC**

Paste what *Check this PC* (`python tools\prism.py doctor`) prints. It names your graphics card and
driver, which matter: Prism has only been tested on NVIDIA. Run it after the failure, so it also
says which GPU the emulator drew with.

```text

```

**The report**

Right after it fails, click *Make a report* under the Log (or run `python tools\prism.py report`)
and attach the zip it makes in `work\reports`. It has what *Check this PC* and *status* say, the
window's and the emulator's logs and, if the emulator is still running, Horizon's crash logs. Your
Windows user and computer names are replaced in it; look through it first if you're unsure.

If the report itself fails, paste the end of the log of what failed instead:
`work\logs\window-setup.log` for Set up, `window-vr.log` or `window-desktop.log` for Start,
`steamvr-run.log` from SteamVR's library, and `emulator.log` if the emulator didn't start (they
have your Windows user name in their paths).

```text

```

**Your headset's OTA**

The headset and the Horizon OS version (the OTA file's name is enough). Don't attach the OTA or
anything else from `work\`.
