# M1 progress: Horizon's Java layer on stock Android 14 x86_64

## Running it

```powershell
python tools\emulator.py create            # once: the prism-api34 AVD
python tools\emulator.py start --wait
python tools\emulator.py root              # once per AVD: writable system partitions
python tools\jni\build.py                  # Prism's JNI glue for this Horizon build
python tools\compat.py                     # bridged framework.jar
python tools\deploy.py                     # push Horizon's layer and reboot
python tools\emulator.py status
```

## How it fits together

- **Layout** ([tools/deploy.py](../tools/deploy.py)). Horizon's framework, config and small app
  directories replace stock's in the writable overlay (`adb remount`, 514 MB). The large app
  directories and Horizon's arm64 libraries go to `/data/prism`, bind-mounted in `post-fs-data`
  by a generated `prism.rc`. Package-manager state is reset on every deploy because the platform
  signature changes. Stock's network-stack apps stay, since they share a UID with apps in stock's
  APEX modules.
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

## Status

- Zygote boots Horizon's framework; `system_server` runs with about 280 binder services
  (338 on a headset) and starts Meta's `oculus.internal.*` services.
- **Blocker:** `LocationUserService` waits for `PreferencesService`, served by Meta's native
  `settingsserver`. It runs through the stock translator (binfmt_misc, registered early by
  `prism.rc`) but dies on ARMv8.1 LSE atomics (`LDADDAL`), which `libndk_translation` 0.2.3
  doesn't support. All of Horizon's arm64 code uses them. Next step: the Digitalis translator,
  which does, with a shim for the newer libc++ symbol its Android 16 build imports.
