# M1 progress: Horizon's Java layer on stock Android 14 x86_64

## Running it

```powershell
python tools\emulator.py create            # once: the prism-api34 AVD
python tools\emulator.py start --wait
python tools\emulator.py root              # once per AVD: writable system partitions
python tools\jni\build.py                  # Prism's JNI glue for this Horizon build
python tools\compat.py                     # bridged framework.jar
python tools\translator.py                 # Digitalis ARM64 translator, adapted to Android 14
python tools\vulkan.py                     # Prism's Vulkan driver (opaque-fd memory for the compositor)
python tools\thermal.py                    # Prism's thermal HAL (stable-AIDL IThermal, for vrdevice)
python tools\tracking.py                   # Prism's tracking service (MemoryBroker, the head pose)
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

- **Vulkan** ([native/vulkan_prism](../native/vulkan_prism/vulkan_prism.c)). Meta's compositor
  requires `VK_KHR_external_memory_fd`: it hands swapchain memory to its clients as file
  descriptors. The emulator's gfxstream driver shares memory on Android only as AHardwareBuffers.
  Prism's driver (`vulkan.prism.so`, selected by `ro.hardware.vulkan`) wraps the emulator's and
  backs opaque fds with them: OPAQUE_FD images, buffers and allocations become AHardwareBuffer
  ones, `vkGetMemoryFdKHR` returns a Unix socket with the memory's AHardwareBuffer queued in it,
  and importing that fd receives the buffer and imports it. Fences and semaphores already export
  as sync fds. Two of Meta's switches fit the compositor to the emulator: GL contexts get pbuffers
  (`persist.oculus.forceGLESContextBuffer`; the emulator's EGL has no surfaceless contexts), and
  its output surface comes from SurfaceFlinger rather than the headset's display path
  (`persist.oculus.strata.disable`).
- **arm64 windows** ([guest_window.c](../native/prism_jni/guest_window.c)). The compositor creates
  that surface with Horizon's arm64 libgui and hands it to x86_64 EGL, which would call its arm64
  hooks directly. The translator binds the x86_64 functions arm64 code calls with `dlsym`;
  `libprism_jni` wraps that import in the translator and swaps in its own `eglCreateWindowSurface`
  and `ANativeWindow_*`. They give EGL an x86_64 stand-in for an arm64 window: a layer of the same
  size created through Java's `SurfaceControl`, shown on top.

- **Front buffer.** The compositor asks EGL for a single-buffered (front-buffer) window surface, which
  a headset's display scans out as it is drawn; here nothing reaches SurfaceFlinger unless a buffer
  is queued. Prism refuses that surface with a real EGL error, and the compositor falls back to a
  back buffer it swaps each frame.
- **Vulkan compositor** (`setprop debug.oculus.compositorGpuApi vk`, not yet the default). The
  emulator's GLES lacks `GL_EXT_memory_object_fd`, so the GL compositor can't sample clients'
  Vulkan swapchains; the Vulkan one shares them through Prism's driver. The x86_64 Vulkan loader
  drives the window too, so the translator's Vulkan proxy is patched (its table of loader
  functions) to give `vkCreateAndroidSurfaceKHR` the same stand-in window as EGL.
- **arm64 libraries by path.** Meta's code dlopens some platform libraries by absolute path
  (`/system/lib64/heapprofd_client_api.so`), which here are x86_64. The guest linker is translated
  code whose opens reach the host through the translator's `open`, `openat` and `syscall` imports;
  `libprism_jni` wraps those and hands it Horizon's arm64 copy from `GUEST_DIR`.
- **Layered and sRGB images.** gralloc here makes single-layer buffers in a few formats only, and
  gfxstream can't export an sRGB image at all. A shared sRGB image is its UNORM twin, created
  mutable so its views stay sRGB. Multiview swapchains (two layers, such as VrShell's eye buffers,
  1440x1584) can't share memory directly: memory imported from a color buffer is that buffer's
  memory on the host, laid out for the buffer's own shape, so a layered image bound there shares
  its first layer and scrambles the rest. Each process keeps its own copy of such an image, in
  memory of its own, and they share a mirror: a single-layer buffer as wide as the image and as
  tall as its layers stacked, which is what the image's fd carries. Prism follows what command buffers do to these images (render
  passes, barriers, transfers, bound descriptor sets, layouts) and, at `vkQueueSubmit`, ends a batch
  that writes one with a copy into its mirror, and begins a batch that only reads one with a copy
  out of it, after the semaphores the batch waits for. The compositor imports the memory of each
  swapchain image it exports and binds a second image to it, which it samples without barriers in
  the layout its descriptors name; that image is bound to memory of its own instead and gets the
  mirror the imported buffer holds. `setprop debug.prism.vk.dump N` (read live) writes every Nth
  copied image, its layers one under another, to `/data/local/tmp/prism_*.rgba`.
- **Multiview.** VrShell's renderer draws both eyes in one multiview pass only if the device lists
  `VK_KHR_multiview`; otherwise it draws one view, into the left eye's layer. Multiview is core
  since Vulkan 1.1 and gfxstream doesn't list it, so Prism's driver does (and other extensions
  promoted to core that Meta's code looks for by name, such as `VK_KHR_driver_properties`).
- **Thermal HAL** ([native/thermal_prism](../native/thermal_prism/thermal_prism.c)). vrdevice
  needs the stable-AIDL `android.hardware.thermal.IThermal/default`; the emulator has only the
  HIDL mock. Prism's HAL, written against libbinder_ndk, reports fixed cool readings.
- **HIDL services** ([native/hidl_prism](../native/hidl_prism)). Arm64 daemons that derive from the
  interfaces in Horizon's own generated HIDL libraries, built against AOSP's headers
  (`tools/hidl.py`). Meta's controller HAL (`vendor.oculus.hardware.sensors@1.0::IControllerProvider`,
  Meta's sensors HAL on a headset) has no controllers paired; its interface was recovered from that
  library's vtables and stubs, which abort the service if a method doesn't call its callback, so
  each one answers. The HIDL system suspend (`android.system.suspend@1.0`), which Meta's
  libnativewakelock uses, passes wakelocks to the AIDL service. Stock's manifest declares it only up
  to FCM level 6, below the emulator's, so deploy drops that limit; a second declaration would
  conflict, and a framework manifest that doesn't assemble stops every HAL lookup, and the boot.
- **Tracking** ([native/tracking_prism](../native/tracking_prism/tracking_prism.c)). Tracking data
  reaches Meta's software as shared memory regions handed out by Meta's MemoryBroker, which
  system_server hosts on a headset from an arm64 JNI library; a headset's trackingservice fills
  them from its cameras and IMU. Prism's tracking service, an arm64 daemon, hosts the broker
  (`memorybroker::registerService()`) and, in a second process, registers as the head tracker's
  host and keeps the headset's pose in that region (a mode in which readers take the latest pose
  as of the time they ask): still at the origin, or moved from the PC by `tools/head.py`, a window
  whose mouse and keys stream poses to the service through `adb forward`. The compositor and
  VrShell read it with Meta's own client code. The same host keeps the hand regions (left and
  right) and the input-type map, as a headset's are before any sample: no hands, no input devices.
  Anchors aren't hosted: the broker leaves them out on an emulator.
- **Wearing the headset.** VrPowerManagerService (declared in VINTF so servicemanager registers it)
  puts Horizon to sleep 15 s after boot unless the headset is worn; `prism.rc` sets its virtual
  proximity sensor to "close" once boot completes.
- **Crash reports** ([fault_report.c](../native/prism_jni/fault_report.c)). App processes that
  crash in host code under the translator die without a tombstone. `libprism_jni` logs those
  (`PrismFault`) and, for a null call in arm64 code, the guest's call chain.
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
- **The XR runtime starts.** The OpenXR loader finds Meta's runtime through
  `/product/etc/openxr/1` (Horizon has these links on `/odm`), `runtimeipcbroker` runs, and
  VrDriver's `vrruntimeservice` registers its RuntimeIPC servers; VrShell's client state
  initializes. Meta's libraries that are dlopened by absolute path (`/system_ext/lib64/...`) are
  linked there to their `GUEST_DIR` copies.
- **The compositor initializes.** Meta's `CompositorServer` runs translated with Vulkan and GL on
  the PC's GPU (an RTX 3080 through the emulator), builds its distortion meshes and timing, and
  waits for its first client, drawing to a Prism layer on the emulator's display. It has no
  display state provider (vsync comes from its fallback timing).
- **VrShell runs in VR.** With the headset worn, VrShell's OpenXR session reaches FOCUSED with
  rendering enabled: it creates and imports its swapchains through Prism's Vulkan driver. Horizon's
  first-time setup (`FirstTimeNuxActivity`, the controller-batteries step) shows in the emulator
  window as a 2D panel.
- **Home in the window.** The compositor composites VrShell's frames: its home environment shows
  in the emulator window in stereo, both eyes side by side, and `python tools\head.py` looks and
  moves around it with the mouse and keyboard.
- **Known issue: the emulator sometimes dies silently** (no crash report), most often during a
  reboot. At those times Windows logs a LiveKernelEvent 141, a GPU engine timeout that it resets:
  host Vulkan work from the guest hung the GPU. Killing VrShell mid-frame alone doesn't do it.
- **Panels don't show yet.** VrShell places panels (first-time setup, for one) but their content
  never arrives. On a headset it comes from SurfaceFlinger itself: Horizon's SurfaceFlinger
  (`libxrsurfaceflinger.so`) runs an OpenXR session against Meta's runtime and submits each panel
  window's buffers as composition layers (`XR_METAI_buffer_composition`: graphic buffers, no
  swapchains), placed at its volumetric window's aperture (`XR_METAX1_aperture`, by window token).
  The emulator's SurfaceFlinger is stock, so Prism needs that OpenXR client of its own, fed with
  the panel windows' contents (captures or mirrors of their layers).
- **CMSHeadset runs** (`com.oculus.os.cm`): with Prism's controller HAL and HIDL system suspend it
  starts all its roles and stays up, with no controllers paired.
- **Next:** VrShell's panels and the Universal Menu, controller input, and
  `com.oculus.presence`, still crash-looping: its `onBind` asserts that the native ID anonymizer
  isn't null (the `ClassNotFoundException` for `com.facebook.simplejni.CoreFunctions` follows, from
  a native thread reporting it).
