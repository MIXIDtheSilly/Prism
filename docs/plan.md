# Prism plan

## Approach

Emulating a whole Quest isn't realistic: the XR2 SoC, Adreno GPU drivers, DSP-based tracking and
cameras would all need emulating, and full ARM system emulation on x86 is far too slow. Prism
instead runs Horizon's software on top of the x86_64 Android 14 emulator and reimplements only
the parts that need Quest hardware (high-level emulation, "HLE").

| Layer | Comes from | How it runs |
|---|---|---|
| Kernel, HALs, GPU | Android Emulator x86_64 API 34 + Gfxstream | As is |
| Java framework + `system_server` | Horizon's jars | ART recompiles them for x86_64; Prism provides x86_64 versions of Meta's JNI |
| Meta system apps | Horizon APKs, unchanged | arm64 libraries through the native bridge |
| Daemons that don't touch hardware | Horizon arm64 binaries | Translated through binfmt_misc; binder is the same on both architectures |
| Hardware daemons (tracking, calibration, guardian, passthrough, cameras, display) | Prism reimplementations | Fed with poses and input from the PC |
| VR compositor | Meta's `CompositorServer` in `com.oculus.systemdriver`, translated, or a Prism replacement for its AIDL interface | Decided in M3 |

Horizon's Java framework is used instead of stock Android's because Meta's system services live
in `system_server`, and 19 Meta packages share the system UID, which Android only allows when
they're signed with the same key as `framework-res`.

Prism is a standalone project. Pieces it needs that also exist in Refract (runtime, host bridge,
translator, launcher) are ported into Prism, not referenced.

## Milestones

**M0: Extract and take inventory.** *Done.* Pure-Python unpackers for the OTA payload, ext4,
APEX and super images, and an inventory of Horizon against stock Android 14.
See [m0-findings.md](m0-findings.md).

**M1: Boot Horizon's Java layer without a display.**
- Build a system image: stock x86_64 API 34 underneath, Horizon's framework, apps, permissions
  and overlays on top, Quest 3 (`eureka`) properties, SELinux permissive at first.
- Fix native registration: Meta changed two `SurfaceTexture` natives and added about 135
  natives elsewhere. Provide x86_64 implementations (or safe stubs) and keep stock registration
  from aborting zygote.
- Progress measure: binder services registered, against the 338 on a real headset.
- Done when `system_server` stays up with Meta's Java services running and the Horizon packages
  installed.

**M2: Native daemons.**
- Run hardware-free Meta daemons (`vrfocusserver`, `runtimeipcbroker`, `settingsserver`,
  `crcs`, ...) through the stock image's arm64 program runner.
- Reimplement hardware services: tracking (`ITrackingService`, `IControllerTrackingService`,
  `IHandTrackingService`, ...) fed from PC input, plus calibration, a fixed guardian boundary,
  `mrsystemservice` (a virtual room instead of passthrough), sensors and the Meta composer HAL
  (`SurfaceForger`).

**M3: Home in a desktop window.** Start `com.oculus.systemdriver`, VrShell and SystemUX. Either
run Meta's compositor translated over Gfxstream, or replace it behind its AIDL interface.
Mouse and keyboard stand in for controllers. Done when the Home environment and Universal Menu
show up on a monitor and respond to clicks.

**M4: More than one app at a time.** Shell and game together, all layer types, 2D Android apps
as panels, launching games from the Library.

**M5: Headset.** Output through a PC OpenXR runtime (SteamVR), with hands and haptics.

**M6: Launcher and packaging.** Point Prism at your own OTA; the image is built on your PC.

## Out of scope

- Meta account sign-in, the Store and cloud features. Horizon checks it's on a real Quest
  (`DeviceAuthServer`, `Integrity`, key attestation), and Prism won't fake device credentials.
  Prism skips first-time setup and uses a local library.
- Any Horizon files in the repository.

## Risks

1. VrShell may depend on compositor or window-manager behaviour that's hard to reproduce.
2. Translated arm64 code may be too slow for VrShell's heavy native code. The fallback is a
   faster translator (as Refract did with Digitalis), ported into Prism.
3. Meta's compositor may need Adreno-only GPU extensions, which would force a Prism replacement.
