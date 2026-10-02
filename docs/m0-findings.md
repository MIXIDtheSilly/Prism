# M0 findings

Analysed: Quest 3 (`eureka`) full OTA, Horizon build 52433670048800520, Android 14
(`UP1A.231005.007.A1`, SDK 34), against the Android Emulator's stock API 34 x86_64 image
(`UE1A.230829.050`). Reproduce with `python tools\prepare.py <ota.zip>`; the raw tables are in
`work\inventory\`. The service list comes from a Quest 2 running an older Horizon build, so treat
service counts as approximate.

## Verdict

Moving Horizon's Java layer onto the x86_64 emulator is viable. Prism goes ahead with the approach
in [plan.md](plan.md).

## Images

- All Android partitions are ext4 (no EROFS); 34 APEX modules, all with ext4 payloads.
- Three Meta APEXes:
  - `com.meta.xr` (odm): `VrDriver.apk` (`com.oculus.systemdriver`), the OpenXR runtime manifests
    and a `trackingservice`.
  - `com.meta.quest` (product): `PresenceService.apk`.
  - `com.meta.hzos` (system_ext): `libosutils.so`.
- All framework jars and APKs still contain dex, so ART can recompile them for x86_64.

## Java framework

- Meta adds `com.oculus.os.platform.jar`, `hzos-framework.jar` and `ocui.jar` to the boot and
  system_server classpaths, and `horizonos-services.jar` and `oculus-system-services.jar` to
  system_server.
- Native methods to provide on x86_64:

  | jar | added vs stock | main classes |
  |---|---|---|
  | framework.jar | 55 | `SurfaceControlExtInternal` (14), `SurfaceTexture` (8), `AudioTrack` (5), `SurfaceControl`, `Zygote`, `SurfaceForger` |
  | services.jar | 6 | `D2DConnectionContextV1`, `NativeInputManagerService` |
  | com.oculus.os.platform.jar | 13 | `NativeInputSettings`, `TrackingInjection` |
  | hzos-framework.jar | 32 | `SpaceManagerClient` (21), `PrescriptionLensInsertManager` |
  | oculus-system-services.jar | 28 | `SpaceManagerClient` (21), `VrPowerManagerService`, `TrackingProxyServer` |
  | meta-supplement-staging.jar | 1 | `TelemetryManager` |

- **Blocker to handle first in M1:** Meta changed the signatures of `SurfaceTexture.nativeInit`
  and `nativeUpdateTexImage`. Stock `libandroid_runtime` registers the old signatures, which
  fails against Horizon's `framework.jar` and aborts zygote.

## Packages

- 174 APKs, 142 not in stock Android. 41 carry arm64 native code and 101 are Java only.
- 19 Meta packages run as the system UID (`horizonos.platform`, `oculus.platform`,
  `com.oculus.os.vrlockscreen`, ...). They need Horizon's `framework-res` signature.

## Native daemons

- 195 init services, 127 with binaries not in stock Android.
- Hardware-bound, to reimplement: `/odm/bin/trackingservice` (24 vendor libraries) and the
  `com.meta.xr` one, the sensors and flicker HALs, `sensorlock`, `hotplugd` (`/dev/syncboss0`),
  calibration, attestation/devicecert/keybox (out of scope), and the composer HAL that hosts
  `SurfaceForger`.
- Hardware-free, likely to run translated: `vrfocusserver` (window focus, `IOculusWindowManager`),
  `runtimeipcbroker`, `settingsserver`, `crcs`, `orchestrator`, `xrservice`, `sensorproxy`, the
  telemetry daemons (which can probably just stay off).
- The stock API 34 image already runs arm64 executables: `ndk_translation_program_runner_binfmt_misc_arm64`
  with `ro.enable.native.bridge.exec=1`.

## VR runtime and compositor

- The compositor is `CompositorServer` in `libvrruntimeservice.so`, inside the persistent
  `com.oculus.systemdriver` app, not in a native daemon. Apps reach it through AIDL
  (`VrRuntime_Server_CompositorServer_compositor_aidl`).
- The runtime exposes 391 OpenXR extension names, including Meta-internal families (`METAI`, `FBI`,
  `METAX1`-`METAX8`). VrShell uses 111, Guardian setup 90, Guardian 46, all provided by Meta's
  runtime.
- So Prism should keep Meta's client runtime and reproduce what sits below it: tracking, display
  and GPU. Reimplementing Meta's internal extensions would be far more work.

## Binder services

- 338 services on the reference headset, 139 non-AOSP. 8 aren't named in any file of this build
  (renamed or removed since the reference build).
