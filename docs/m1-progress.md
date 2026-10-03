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
  by a generated `prism.rc` (onto mount points the overlay creates; stock has no
  `/system_ext/app`). Each deploy first empties the overlay's upper layer and reboots into
  stock, because deletions made through overlayfs persist as whiteouts. Package-manager state is
  reset too, since the platform signature changes. Meta's software turns adb off (as a headset
  does outside developer mode); `prism.rc` restarts adbd whenever init stops it, and the reset
  restores the persisted USB config. Stock's mainline APEX modules stay underneath,
  so their partners stay stock as well: the network-stack apps (shared UID), the Google module
  metadata app, and the stock product RROs that name those Google packages.
- **Meta's native daemons** run as arm64 binaries through binfmt_misc and the Digitalis
  translator ([tools/translator.py](../tools/translator.py)), which handles the ARMv8.1 LSE atomics
  all of Horizon's arm64 code uses. Digitalis is an Android 16 build; `libpcx.so`
  ([native/berberis_compat](../native/berberis_compat/berberis_compat.c)) supplies what Android
  14's libraries lack: libc++'s verbose abort and three `std::filesystem` functions, plus Vulkan
  1.4 and camera stubs. Daemons and apps' JNI libraries link Horizon's own arm64 libraries (from
  `/system`, `/system_ext` and its APEXes, such as ICU), searched before the translator's in the
  guest linker config; only bionic and the host-proxy libraries are the translator's. Meta's
  libraries need Horizon's builds of `liblog`, `libgui` and `fmt`, and the translator's resolve
  against Horizon's too. `tools/deploy.py`'s `DAEMON_RCS` lists the init scripts Prism runs.
- **Native registration** ([native/prism_jni](../native/prism_jni/prism_jni.c)). Zygote preloads
  `libprism_jni.so`. `jniRegisterNativeMethods` is inlined into every library since Android 12,
  so it can't be interposed. Instead Prism wraps libutils' `androidSetCreateThreadFunc` (the
  first cross-library call in `startReg`, right after the VM exists) and installs a JNI function
  table through ART's `JNIEnvExt::SetTableOverride`, found by reading libart's dynamic symbols in
  memory. Its `RegisterNatives` drops stock natives Horizon's classes lack, adapts Meta's changed
  `SurfaceTexture` signatures to the stock functions, and adds a logging stub for each of the 113
  natives Meta added. Eleven stand-in libraries replace the arm64 JNI libraries Meta's jars load.
  It also wraps arm64 functions that apps register: since Android 15, ART asks the native bridge
  for a trampoline to each guest function passed to `RegisterNatives`, and Digitalis (an Android 16
  build) relies on that. Android 14's ART doesn't, so Prism asks instead (`NativeBridgeItf`'s
  `getTrampolineForFunctionPointer`).
- **HLE** (high-level emulation): natives Prism implements rather than stubs, declared with
  `PRISM_HLE` ([prism_hle.h](../native/prism_jni/prism_hle.h)) in `native/prism_jni/hle_*.c`.
  The first is `SpaceManagerClient` ([hle_spaces.c](../native/prism_jni/hle_spaces.c)): a tree of
  reference, virtual and window spaces with poses, bounds, alpha and update tokens, kept in
  system_server's process.
- **Java-side compatibility** ([tools/compat.py](../tools/compat.py)). Stock native code looks up
  `InputDevice.<init>` and two `FullBackupDataOutput` members by their stock signatures. Prism
  adds bridge methods with those signatures that call Meta's versions. Meta's
  `Lockdep.registerHandler` needs Meta's modified ART, so its body is replaced with a return.
  Android looks for a bundled app's libraries only under `lib/<first 64-bit ISA>` (`x86_64`);
  services.jar's `getBundledAppAbis` is replaced so Meta's apps, with `lib/arm64`, get
  `arm64-v8a` and load their libraries through the bridge.

- **Mainline modules.** Stock's APEXes stay, except pure-Java ones whose newer APIs Horizon's
  framework needs (`HORIZON_APEXES` in deploy.py); those are Horizon's own: configinfrastructure
  (DeviceConfig) and permission (PermissionController holds Meta's roles, which grant role-only
  permissions such as `horizonos.permission.READ_UI_MODE`). `tools/inventory/module_api.py` lists every use of a module member stock lacks.
  Meta's additions to ART (`RLIMIT_RTPRIO`, `clampGrowthLimit(long)`, lock-order checking) are
  rewritten by `tools/compat.py`. Meta's own APEXes (`com.meta.xr` with VrDriver and
  SystemActivities, `com.meta.quest`, `com.meta.hzos`) go in `/system_ext/apex` (`META_APEXES`).

## Status

- **Horizon boots to its home shell** on stock Android 14 x86_64: `sys.boot_completed=1`, with
  Meta's VrShell (`com.oculus.vrshell/.HomeActivity`) as the resumed activity. Meta's native
  `settingsserver`, `vrfocusserver` and `xrservice` run as arm64 under Digitalis, system_server
  runs every Meta service with the space manager emulated, and Meta's arm64 apps load and run
  their native code.
- **Next: the XR runtime.** VrShell's 3D side (Clay) fails `xrCreateInstance` with
  `XR_ERROR_RUNTIME_UNAVAILABLE` and its process restarts: Meta's OpenXR runtime has no compositor
  or tracking behind it. Other apps wait on services that are the headset's hardware layers
  (`vrdevice`, `OVRRemoteService`, the maintenance-boot HAL); `com.oculus.os.cm` aborts on a
  missing service, PresenceService on a missing ID anonymizer.
