# M1 progress: Horizon's Java layer on stock Android 14 x86_64

## Running it

```powershell
python tools\emulator.py create            # once: the prism-api34 AVD
python tools\emulator.py start --wait
python tools\emulator.py root              # once per AVD: writable system partitions
python tools\jni\build.py                  # Prism's JNI glue for this Horizon build
python tools\compat.py                     # bridged framework.jar
python tools\translator.py                 # Digitalis ARM64 translator, adapted to Android 14
python tools\deploy.py                     # reset to stock, push Horizon's layer, reboot
python tools\emulator.py status
```

## How it fits together

- **Layout** ([tools/deploy.py](../tools/deploy.py)). Horizon's framework, config and small app
  directories replace stock's in the writable overlay (`adb remount`, 514 MB). The large app
  directories and Horizon's arm64 libraries go to `/data/prism`, bind-mounted in `post-fs-data`
  by a generated `prism.rc`. Each deploy first empties the overlay's upper layer and reboots into
  stock, because deletions made through overlayfs persist as whiteouts. Package-manager state is
  reset too, since the platform signature changes. Stock's mainline APEX modules stay underneath,
  so their partners stay stock as well: the network-stack apps (shared UID), the Google module
  metadata app, and the stock product RROs that name those Google packages.
- **Meta's native daemons** run as arm64 binaries through binfmt_misc and the Digitalis
  translator ([tools/translator.py](../tools/translator.py)), which handles the ARMv8.1 LSE atomics
  all of Horizon's arm64 code uses. Digitalis is an Android 16 build; `libpcx.so`
  ([native/berberis_compat](../native/berberis_compat/berberis_compat.c)) supplies what Android
  14's libraries lack: libc++'s verbose abort and three `std::filesystem` functions, plus Vulkan
  1.4 and camera stubs. Daemons link Horizon's own arm64 libraries (from `/system`, `/system_ext`
  and its APEXes, such as ICU) through a `[prism]` section in the guest linker config; only bionic
  and the host-proxy libraries are the translator's. `tools/deploy.py`'s `DAEMON_RCS` lists the
  init scripts Prism runs.
- **Native registration** ([native/prism_jni](../native/prism_jni/prism_jni.c)). Zygote preloads
  `libprism_jni.so`. `jniRegisterNativeMethods` is inlined into every library since Android 12,
  so it can't be interposed. Instead Prism wraps libutils' `androidSetCreateThreadFunc` (the
  first cross-library call in `startReg`, right after the VM exists) and installs a JNI function
  table through ART's `JNIEnvExt::SetTableOverride`, found by reading libart's dynamic symbols in
  memory. Its `RegisterNatives` drops stock natives Horizon's classes lack, adapts Meta's changed
  `SurfaceTexture` signatures to the stock functions, and adds a logging stub for each of the 113
  natives Meta added. Eleven stand-in libraries replace the arm64 JNI libraries Meta's jars load.
- **Java-side compatibility** ([tools/compat.py](../tools/compat.py)). Stock native code looks up
  `InputDevice.<init>` and two `FullBackupDataOutput` members by their stock signatures. Prism
  adds bridge methods with those signatures that call Meta's versions. Meta's
  `Lockdep.registerHandler` needs Meta's modified ART, so its body is replaced with a return.

- **Mainline modules.** Stock's APEXes stay, except pure-Java ones whose newer APIs Horizon's
  framework needs (`HORIZON_APEXES` in deploy.py: configinfrastructure for now); those are
  Horizon's own. `tools/inventory/module_api.py` lists every use of a module member stock lacks.
  Meta's additions to ART (`RLIMIT_RTPRIO`, `clampGrowthLimit(long)`, lock-order checking) are
  rewritten by `tools/compat.py`.

## Status

- **Horizon's framework reaches `sys.boot_completed=1`** on stock Android 14 x86_64. Meta's
  native `settingsserver`, `vrfocusserver` and `xrservice` run as arm64 under Digitalis;
  system_server starts every Meta service, and Meta apps begin launching.
- **Blocker:** right after boot, `VolumetricWindowManagerService` creates a window and calls
  `SpaceManagerClient.nativeInit()`. Prism's `spacemanager_jni` stand-in only logs and returns 0,
  so system_server dies ("Is xrservice running?"). Next step: a real HLE of the 21
  `SpaceManagerClient` natives (spaces with poses, bounds and alpha), either in-process or
  talking to the running `xrservice`.
- Meta apps whose JNI libraries are arm64 (`com.oculus.os.cm` and others) need their native
  bridge namespace to reach Horizon's arm64 libraries; deploy now keeps their `lib/arm64` links.
