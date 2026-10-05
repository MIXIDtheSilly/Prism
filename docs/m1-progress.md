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
python tools\spaces.py                     # Prism's space service (Meta's space manager, for system_server)
python tools\guest_media.py                # Prism's arm64 media functions (panels' surface sizes)
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
  The first is `SpaceManagerClient` ([hle_spaces.c](../native/prism_jni/hle_spaces.c)), the
  spaces volumetric windows are placed in.
- **Spaces** ([native/spaces_prism](../native/spaces_prism)). Meta's space manager (xrservice's)
  keeps reference, virtual and window spaces, shared between processes by UUID: system_server
  creates a window space per volumetric window and moves it, and VrShell acquires it to place the
  window's panel. Its client library is arm64, which system_server can't load, so Prism's space
  service, an arm64 daemon, calls it (its C API, `Hzu*`, reconstructed in `hzu_spaces.h`) and
  system_server's `SpaceManagerClient` natives call the service over binder (`prism.spaces`).
  Spaces go when the process that made them dies.
- **Java-side compatibility** ([tools/compat.py](../tools/compat.py)). Stock native code looks up
  `InputDevice.<init>` and two `FullBackupDataOutput` members by their stock signatures. Prism
  adds bridge methods with those signatures that call Meta's versions. Meta's
  `Lockdep.registerHandler` needs Meta's modified ART, so its body is replaced with a return.
  Android looks for a bundled app's libraries only under `lib/<first 64-bit ISA>` (`x86_64`);
  services.jar's `getBundledAppAbis` is replaced so Meta's apps, with `lib/arm64`, get
  `arm64-v8a` and load their libraries through the bridge. The volumetric window service's window
  listener skips windows without Meta's `XrWindowInfo` (stock SurfaceFlinger gives none; it threw
  on every change of windows), and sends a panel's injected motion to the panel's mirror display
  (see The system bar). One app is patched too, OCMS (see The library); its APK is repacked with its entries aligned and its APK Signing Block kept, since
  PackageManager reads a system app's signers without verifying its contents.

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
- **Vulkan compositor** (`debug.oculus.compositorGpuApi=vk`, which deploy sets). The GL
  compositor needs `GL_EXT_memory_object_fd` to sample clients' Vulkan swapchains, and its surface
  swapchains abort VrShell (their producers don't cross the binder bridge); the Vulkan one shares
  them through Prism's driver. The x86_64 Vulkan loader
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
- **Panels' surfaces.** VrShell draws each panel window through an Android surface (a mirror of
  the window, made by system_server's `VolumetricWindow`, on a virtual display), which it gets from
  `xrCreateSwapchainAndroidSurfaceKHR` as an `AImageReader`'s window, and sizes it to the panel with
  `AImageReader_setDefaultBufferSize` before binding it; SurfaceFlinger sizes the display from it.
  Meta's libmediandk has that function and Android's doesn't. Prism's arm64
  `libprism_guest_media.so` ([native/guest_media](../native/guest_media)), preloaded globally,
  defines it; arm64 code reaches x86_64 only through the translator's proxies, so it calls the
  proxy's `AImageReader_new` with a format reserved for this, and libprism_jni's replacement of
  that function ([guest_media.c](../native/prism_jni/guest_media.c)) sets the reader's consumer's
  default buffer size, as `AImageReader::init` does.
- **Images the PC's GPU doesn't have.** gfxstream passes images to the PC's Vulkan driver as they
  come, and NVIDIA's crashes on one it doesn't support (a divide by zero in `vkCreateImage`), which
  takes the whole emulator down. Meta's compositor makes its volume indicator R8G8_SRGB, a format
  a Quest's GPU samples and the PC's doesn't. Prism's Vulkan driver makes such images (8-bit
  formats of one to three channels) R8G8B8A8 instead, has their views read the missing channels as
  0 and alpha as 1, and spreads copies from buffers into them to 4 bytes a texel on the GPU (a
  region per texel). It fails any other image the device doesn't support, or the spec forbids.
- **Multiview.** VrShell's renderer draws both eyes in one multiview pass only if the device lists
  `VK_KHR_multiview`; otherwise it draws one view, into the left eye's layer. Multiview is core
  since Vulkan 1.1 and gfxstream doesn't list it, so Prism's driver does (and other extensions
  promoted to core that Meta's code looks for by name, such as `VK_KHR_driver_properties`).
- **Thermal HAL** ([native/thermal_prism](../native/thermal_prism/thermal_prism.c)). vrdevice
  needs the stable-AIDL `android.hardware.thermal.IThermal/default`; the emulator has only the
  HIDL mock. Prism's HAL, written against libbinder_ndk, reports fixed cool readings. Beside it
  goes the maintenance boot HAL a headset's odm has
  ([maintenanceboot_prism.c](../native/thermal_prism/maintenanceboot_prism.c)): Horizon's
  MaintenanceBoot app records at each boot that maintenance boot is off, and without the HAL it
  retried every second, forever. Prism's keeps the settings in memory.
- **Binder objects between arm64 code and Java** ([native/binder_relay](../native/binder_relay)). An
  app's arm64 libbinder is a /dev/binder connection of its own, separate from its Java binder's, so
  Horizon's `AIBinder_toJavaBinder` and `AIBinder_fromJavaBinder` can't hand an object across.
  Prism's arm64 bridge, preloaded after libbinder, takes them over: an object crosses as a
  transaction through Prism's relay (`prism.binder_relay`), put in from one connection for a token
  and taken out from the other.
- **FindClass from native threads.** A thread that attached itself has no Java frames, so ART looks
  classes up with the system class loader, which has none of the app's. Meta's apps take their class
  references on Java threads first; translated, a native thread can get there before (Presence's
  simplejni). When a lookup fails, Prism's JNI table tries the class loaders that registered natives.
- **HIDL services** ([native/hidl_prism](../native/hidl_prism)). Arm64 daemons that derive from the
  interfaces in Horizon's own generated HIDL libraries, built against AOSP's headers
  (`tools/hidl.py`). Meta's controller HAL (`vendor.oculus.hardware.sensors@1.0::IControllerProvider`,
  Meta's sensors HAL on a headset) has two Touch Plus controllers paired and connected; its
  interface was recovered from that library's vtables and stubs, which abort the service if a method
  doesn't call its callback, so each one answers. The HIDL system suspend (`android.system.suspend@1.0`), which Meta's
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
- **The emulator dying.** It writes a minidump for each of these (in
  `%LOCALAPPDATA%\Temp\AndroidEmulator\emu-crash-*.db\reports`; `cdb -z <dump> -c ".ecxr; kn"`).
  Pressing a volume key crashed it in NVIDIA's driver (an image the PC's GPU doesn't have, see
  above), now fixed. A guest reboot (`adb reboot`) can crash qemu itself: restart the emulator
  instead (`tools/emulator.py stop`, then `start`). Earlier deaths also coincided with a
  LiveKernelEvent 141, a GPU engine timeout that Windows resets.
- **Panels show.** VrShell places panel windows in their window spaces and draws their contents,
  mirrored into its panel surfaces: first-time setup's panel (the controller-batteries step) shows
  in the emulator window, in front of passthrough, which is black here (no cameras). This is the
  mirror path; a headset's SurfaceFlinger can also submit panels to the runtime itself, as OpenXR
  layers (`libxrsurfaceflinger.so`, single-pass composition), which Horizon uses when a window
  isn't given a surface.
- **Controllers.** Two Touch Plus controllers, in two places, as on a headset. Their tracking and
  input are regions Prism's tracking service hosts (CONTROLLER and CONTROLLER_TRACKING, left and
  right, listed in the input map), which the runtime and VrShell read: a controller follows the head,
  pointing where it looks, or takes the pose and buttons `tools/head.py` sends (the mouse aims the
  right one, its right button pulls the trigger). Their pairing is the controller HAL's: it lists
  them and writes their states (`PairedControllerInfo`, 0x310 bytes) into the state stream CMSHeadset
  prepares, which then reports them paired and active through OVRRemoteService (`dumpsys
  OVRRemoteService`), once each has a calibration; a flags word in it gives each one's hand. Whether
  they're tracked CMSHeadset learns from the controller tracking service (trackingservice's, on a
  headset), which Prism's tracking host serves: CMSHeadset hands it a socket per controller, and it
  writes "tracked in 6DoF" and "in hand" events into it. Both are then as a Quest 3S's are: Left and
  Right, Active, CONNECTED_ACTIVE, TrackingStatus POSITION (a controller's slot needs its flags to
  name its hand: 0x60 left, 0x50 right). First-time setup moves on to its controller check; VrShell
  doesn't show the controllers yet.
- **Pointing at panels.** VrShell sends the panel window you point at touches (hover, and taps),
  through Android's input on the default display, where the panels' windows are. Prism's layer
  for the compositor's output covers that display, and input drops touches to a window another
  app's layer covers, so Prism makes it a trusted overlay. With no controllers, the pointer is your
  head (`tools/head.py`); first-time setup's hover reaches it.
- **The headset is tracked.** A HeadState carries tracking flags (at 4, as a controller's pose
  sample does) besides its valid byte; without them the runtime located the headset as valid but
  not tracked, and Guardian, which wants both tracked bits of the view's location, put up
  "tracking lost" (`GUARDIAN_TRACKING_LOST`, "Continue without tracking") as soon as setup ended.
  With them an app locates its view and stage at 0xf.
- **Past first-time setup.** NuxOta's own skip (`am startservice -n
  com.oculus.nux.ota/.NuxOtaIntentService -a nux.ota.SKIP_NUX --ez REBOOT false`, as root) marks
  setup, health and safety and Guardian setup done; VrShell then leaves setup for its home (an
  environment's floor and the Store's window bar show). Until it's done, VrShell holds any other
  immersive app back for Guardian setup (its "setup active" comes from Guardian's state, not from
  Android's provisioning settings).
- **xrdemo** (`python tools/xrprobe.py xrdemo --install`, then `--start`, `adb logcat -s XrDemo`):
  the smallest OpenXR app that draws, a moving grid in a stereo projection layer through Vulkan,
  blue, or orange while a trigger is held. Built and installed like xrprobe. `xrgame` is the same
  app as a store game is: an ordinary app (`adb install`) bundling its own loader, Khronos's, built
  from the OpenXR SDK's source into `work/build/openxr-sdk`; `xrgles` is xrgame drawing with
  OpenGL ES.
- **xrprobe** (`python tools/xrprobe.py --install`, then `--start`, `adb logcat -s XrProbe`): an
  OpenXR app, bound to the controllers as VrShell is, that logs its session states, each hand's
  interaction profile and the controllers' and headset's locations. It's a system app (Meta's
  loader is a private system library), declares itself boundaryless and asks to see the runtime's
  package. With the headset tracked its session focuses and syncs, the runtime has both controllers
  (`REMOTE: Hand=1/2`), yet neither hand gets an interaction profile, so their poses don't locate:
  that's why VrShell doesn't show them. It also reads the runtime's own state from inside the app
  (the session object, its controller snapshot, the selected profiles, each device's in-hand flag).
- **Controllers in hand.** A controller the runtime service doesn't take as held is a detached
  controller, which only apps enabling `XR_META_detached_controllers` see; xrSyncActions drops it
  for everyone else before picking interaction profiles. With hand tracking off
  (`hand_tracking_opt_in` false) the service takes in-hand from the tracking client, which reads
  it from the CONTROLLER_TRACKING region at 0xe38 (a snapshot of a u16: in hand, and known).
  Prism publishes the controllers held, so each hand gets `meta/touch_controller_plus`, grip and
  aim locate, and VrShell draws both Touch Plus with their pointer rays. (The 0x250 field in
  CONTROLLER that looked like a candidate is the thumbstick.)
- **A signed-in user.** Without a Meta account, the Library panel's system bar busy-waits for a
  user id (`ActiveImmersiveRepositoryImpl`): about 40,000 `getDefaultAccount` calls a second, until
  system_server runs out of memory and dies, and qemu with it (5-15 minutes after setup). An
  offline login stops it: Horizon's `shared_prefs/authentication.xml` with a `uid` and an
  `access_token` (placeholders; Meta's servers reject the token, and Horizon doesn't log out for
  that), then Horizon adds the `com.oculus` account itself. With an account but no token, VrShell
  holds every app back behind "You've been logged out" instead.
- **Clicking panels.** A controller's ray hovers a panel's controls and its trigger presses them:
  the right trigger closes the Store's window from its bar's ✕. Behind that window is the home
  environment's sky and floor; the solid green that filled the view was the Store window's body,
  which wasn't drawn then (see Panels' bodies). Prism publishes a controller's aim pose (the runtime pitches the grip from it).
- **The Universal Menu.** A headset's trackingservice also reports the Meta button as a key
  (KEY_FORWARD, from its own input device), and VrShell's SystemButtonHandler takes it only from
  Meta's controller input source; released within 500 ms it's VrShell's system button press. Stock
  input has no such source, so Prism's tracking service hands VrShell that press through the intent
  VrShell also takes it from (`QUIT_TO_HOME` to `AndroidIntentsRelayActivity`): the Navigator opens
  (`InvokeNavigator showReason: hardware-button-click`); its panels draw (see Panels' bodies).
- **The emulator freezing** when a VR process dies (system_server, the runtime service): qemu's
  main loop and first vCPU stop, and its hang detector kills it 15 s later (minidump: "detected a
  hanging thread 'QEMU2 main loop'"). A gfxstream render thread was in NVIDIA's driver, waiting
  without end on a sync object for a guest Vulkan wait (`vkWaitSemaphores`, by its decoder case)
  whose signaller had died, and ending that process's connection waits for that thread. Prism's
  Vulkan driver now never has the PC wait (fences and semaphores): it asks with no timeout and
  sleeps in the guest between asks, 0.1 ms doubling to 0.5 ms; killing the runtime service no
  longer stops the emulator. Asking that way also stops a guest spin: while the PC waits, the
  waiting thread spins on gfxstream's ring buffer (`AddressSpaceStream::speculativeRead`), and the
  runtime service's frame waits kept one of its binder threads at 50-97% of a vCPU.
- **Chrome's app zygote spinning** (`com.oculus.browser_zygote` at ~90% of a vCPU, and no
  renderer ever started): Prism started the arm64 libbinder's thread pool at the first namespace,
  which an app zygote makes while preloading, and a zygote forks only once it's down to one thread
  (`ZygoteHooks.preFork` lists /proc/self/task until then). libprism_jni now leaves a zygote's
  pool alone (by process name: `zygote`, `zygote64`, `*_zygote`) and each child starts its own at
  its next namespace or arm64 library.
- **The guest pausing.** When the whole guest stalls for seconds (the PC busy), gfxstream's guest
  health monitor reports every stream as hung, with the encoder's current packet as hex
  (`VkEncoder::getPacketContents`); between packets that packet's pointer is null, and the report
  killed the runtime service with SIGSEGV after almost six hours up. Prism's Vulkan driver makes
  the function return an empty string (checking its prologue first).
- **Translator aborts** (scudo: "invalid chunk state when deallocating", in
  `berberis::InstallTranslated`): the bundle's block chaining keeps per-thread state in a
  `thread_local` vector, which bionic destroys before running pthread key destructors; an arm64
  key destructor reaching untranslated code then reused it. VrShell and the runtime service died
  of it every so often. `tools/translator.py` makes the vector's destructor return at once
  (keep_chain_sites): two places register it, the accessor and its copy inlined in
  InstallTranslated.
- **adb losing the emulator.** An adb server started after the emulator scans for emulators only up
  to port 5585, and Prism's is 5590; `tools/emulator.py` tells the server about it again
  (`host:emulator:5591`), as the emulator does when it starts.
- **CMSHeadset runs** (`com.oculus.os.cm`): with Prism's controller HAL and HIDL system suspend it
  starts all its roles and stays up, with no controllers paired.
- **Presence runs** (`com.oculus.presence`): its native ID anonymizer reaches Java through the
  binder relay, and its native threads find its classes. No process crash-loops after boot.
- **The viewer** (`python toolsiewer.py`): Horizon in a desktop window, one eye's half of the
  display from the frames the emulator shares (`-share-vid`; mapping `SHM_videmulator5590`, a
  VideoInfo then BGRA), so its own window can stay minimized. The right controller points where the
  mouse is: its ray goes through the pixel under the mouse, within a pixel or two on panels at
  VrShell's distance (1.08 m). The left button pulls the trigger, Tab presses the Meta button,
  right-drag looks around, WASD and R/F move. The compositor draws each half from the head's
  center (not the eye's), the middle of the FOV apps render, undistorted; the runtime's aim pose is
  the published pose turned 5° in and moved 9 mm (xrprobe), which the viewer undoes.
  The compositor runs at 60 Hz, the display's rate: its vsync otherwise starts at the device's
  default 72 whatever the display reports, and the emulator shares at most 60 frames a second, so
  one compositor frame in six was dropped, unevenly. `debug.oculus.refreshRate` ("<rate>", or
  "<rate>:2" for half) forces the compositor's rate; deploy sets 60. Apps take theirs from
  vrdevice's `device_default_refresh_rate` (/system/etc/device_props.json, read once by
  vrdevicemanagerserver at boot), which deploy sets to 60 too: with only the property, VrShell
  restarted on its own timed its frames for 72 against the 60 Hz compositor (47 shared frames a
  second, 10 ms of jitter, against 60 and 1.4 ms with both).
- **Panels' bodies.** VrShell gave the Store's and the Universal Menu's volumetric windows
  single-pass composition (`dumpsys volumetric_window`: XR_SINGLE_PASS), where a headset's
  SurfaceFlinger hands the windows' buffers to the runtime as OpenXR layers
  (`libxrsurfaceflinger.so`); stock SurfaceFlinger doesn't, and their bodies stayed solid green.
  With single-pass off (`persist.debug.vw.spc_disable all`, which deploy sets) VrShell draws them
  into surfaces the compositor makes (`xrCreateSwapchainAndroidSurfaceKHR`): the runtime's are
  ImageReader windows, x86_64 objects, which Meta's arm64 RuntimeIpcManager took for arm64 Surfaces
  to hand their producer to the app (and died), and the app's arm64 Surface would have gone to the
  host's `ANativeWindow_toSurface` as it was. The binder bridge now moves the producer binder in
  both directions (Java Surface parcels on the host side, the relay in between), and the Store's
  body draws ("Token fetch rejected by server": no Meta account), as does the Universal Menu:
  its system bar, clock and the library's tiles.
- **Anchors.** Meta's spatial persistence client (`libplugin.so`) names its caller by
  `readlink("/proc/self/exe")`: `/system/bin/app_process64` means a Java app, whose package name it
  sends; any other path under `/system/bin` is a native executable, sent as the path. The
  translator answers arm64 code with the guest app_process it loaded
  (`/system/bin/arm64/app_process64`), so the spatial persistence service refused the shell's and
  Guardian's anchor queries (`ERROR_RPC_PACKAGE_NAME_MISMATCH`, 10 a second, half of all logging).
  libprism_jni now records the host's app_process as the translator's main executable
  (`berberis::SetMainExecutableRealPath`) once an app's guest side is up; both clients are
  validated by package and VrShell is trusted.
- **The library.** OCMS (Horizon's content service) publishes the library once the Store channel's
  content is resolved, and skipped resolving a fetch that found nothing (no Meta account), so the
  library never got ready: OCMS refetched every 15 s and VrShell polled it every 500 ms. Prism
  patches OCMS to resolve an empty fetch like any other (an empty Store library plus the device's
  own apps): it reaches LIBRARY_READY at boot. The Universal Menu's library then shows Horizon's
  own apps (Chats, Files, Settings, Camera, Dogfood Hub), with icons and names, once the guest has
  more than one CPU (see CPUs).
- **CPUs.** Under the Windows Hypervisor Platform the emulator gives the guest one vCPU whatever
  the AVD asks for ("Not all modern X86 virtualization features supported"). On one CPU the
  Navigator's coroutine dispatcher has two workers, and both blocked for good on account-only
  GraphQL requests (profile photo and avatar), so the library's flows never ran and its grid
  stayed a loading shimmer. `tools/emulator.py` passes qemu its own `-smp` (the AVD's 6 cores),
  which wins; boot to the first panel went from 2 min 18 s to 1 min 21 s, and the compositor from
  about 17 frames a second to 40-46 (of 72).
- **The system bar.** Every volumetric window's Android windows sit at the origin of display 0,
  one over another, and the volumetric window service injects a panel's motion there tagged with
  its volumetric window token (`setVwToken`). Meta's InputDispatcher delivers by that token; stock
  InputDispatcher drops it and hit-tests, so the open library or a 2D app took the clicks meant for
  the system bar under them, and Quick Settings never opened. Each volumetric window also mirrors
  its windows onto a display of its own (`MirrorRoot for <token>`), where they're the only ones:
  Prism patches the service to send a panel's motion to that display. The system bar's Quick
  Settings opens (Quick Controls: volume, brightness, Wi-Fi, Bluetooth, boundary, Link).
- **An immersive app.** xrdemo, an arm64 OpenXR app running through the translator, starts as
  Horizon's immersive app (VrShell's immersive transition, the UI mode's immersive package), its
  Vulkan session reaches FOCUSED and it submits 72 frames a second, which Meta's compositor shows
  in place of Home; its trigger action follows the controller. The Meta button opens the
  Universal Menu over it while it keeps rendering (VISIBLE), and the menu's Quit ends its session.
  xrgame runs the same way: Horizon's runtime brokers are disabled stubs (stock: they return no
  cursor), so its loader takes `/product/etc/openxr/1/active_runtime.aarch64.json` and loads the
  runtime from VrDriver's APK, as a game's loader does on a headset.
  The running-app card says "App name unavailable": the library service has no record of a
  system app it didn't install.
- **OpenGL ES apps.** The runtime's swapchain images are the compositor's, shared with the app as
  file descriptors, which it imports into GL through `GL_EXT_memory_object_fd` or EGL dma-buf
  import; the emulator's GLES has neither, and a GLES app's `xrCreateSwapchain` failed. Prism's
  GLES layer (`native/gles_prism`, `tools/gles.py`; Android's EGL loader takes it from
  `/data/local/debug/gles`, named by `debug.gles.layers`) adds the extension: an fd from Prism's
  Vulkan driver holds an AHardwareBuffer (the image, or for two layers a mirror with them
  stacked), the texture gets ordinary storage of its format, and before the app's next fence a
  draw in a context of the layer's own copies what was written into the buffer, re-encoding sRGB
  (the emulator has no raw copy). xrgles (xrgame drawing with GLES) runs at 72 frames a second,
  upright and in the same colors as xrgame, with an image per eye or one image of two layers
  (`debug.prism.xrdemo.layers=2`). Meta's switch `persist.oculus.useGraphicBuffers` (swapchains as
  AHardwareBuffers) is no way around it: gralloc here makes no layered buffers, and VrShell's
  multiview swapchains fail. The emulator's GLES advertises the PC's `GL_OVR_multiview2` but its
  encoder has none of its functions; Guardian, which renders multiview without asking, aborted on
  an incomplete framebuffer whenever it showed (an app's exit, say), and with the extension hidden
  jumped to a null function instead. The layer emulates multiview: a multiview attachment is its
  base view's layer, a draw or clear into it is made once a view with that view's layer attached,
  and a shader's `gl_ViewID_OVR` becomes a uniform the layer sets. Guardian's framebuffers are
  complete and its frames reach the compositor. (A multiview GLES game is untried.)
- **2D apps.** An ordinary Android app installed with adb opens as a Horizon panel, with its title
  and window bar.
- **Launching from the library.** The library's menu (its side bar) has All, Unknown Sources and
  Downloads. Unknown Sources lists the apps installed with adb (the library service adds an
  install as it happens, without logging it), under Meta's warning; clicking xrgame there launches
  it through VrShell (`com.oculus.vrshell.intent.action.LAUNCH`) as Horizon's immersive app. The
  library is a cylinder layer around the viewer, its tiles bowed and tilted toward its edges as
  they should be; its glass background isn't drawn (the dark card behind it is the Store's
  window), and the first tile of All is blank (an app without an icon).
- **Meta's emulator mode.** Meta's runtime has an emulator of its own in mind, and two things put
  Prism in it:
  - `ro.kernel.qemu=1`, from the emulator's `/vendor/build.prop`, which nothing of the emulator's
    own reads (its HALs use `ro.boot.qemu`). With it, the runtime skips DeviceConfig for its
    gatekeepers. Deploy overrides it to 0 from `/product/etc/build.prop`, which loads last
    (`--emulator-flag shown` keeps it; `tools/emulator.py emulator-flag [hidden|shown]` switches a
    running emulator, with a reboot).
  - The device type. The runtime looks `Build.MODEL` up in a table of codenames; "Quest 3" isn't
    one, and on ranchu hardware an unknown model is its emulator (type 270). `--model eureka`
    (`tools/emulator.py model [quest3|eureka]`) makes the model Quest 3's codename, so the runtime
    takes it for a Quest 3 (type 272). It's off by default: every app then sees "Eureka" as the
    model, and the compositor takes the headset's paths, which pre-warp each eye's image for the
    lenses (Quest 1's, as its lens profile is looked up elsewhere) in the window.
- **Meta's config parameters.** Gatekeepers and other parameters come from Meta's servers. Each
  app's config client caches the ones it uses (`files/deviceconfig_cache_v2.txt` in its data:
  name, logging id, timestamp, source, propagation, value), loads them at start and writes them
  back. Horizon's own local override (the `DC_OVERRIDE` debug broadcast to
  `com.oculus.deviceconfigservice.DeviceConfigDebugReceiver`) stores an override but doesn't reach
  a parameter its service has no schema for, which is all of them without a server sync. Deploy's
  `META_PARAMS` are set in those caches at post-fs-data, before apps start; the runtime's native
  gatekeepers (`gkcache.dat`) follow its client's values from its next start.
- **Frosted glass.** Panels' glass backgrounds are the compositor's: it blurs what's behind a glass
  layer (`XR_METAX1_composition_layer_glass`). It sets glass up only for the headsets it knows and
  when `oculus_xrruntime:oculus_frosted_glass` is on. Prism sets the parameter (`META_PARAMS`), and
  libprism_jni patches the device check (one instruction of `libvrruntimeservice.so`, written as
  the translator loads it, after checking the bytes:
  [guest_patch.c](../native/prism_jni/guest_patch.c)); the compositor logs "Glass is enabled".
- **Next:** panels' glass backgrounds; system_server's deaths by SIGPIPE (twice,
  soon after an app's window was placed; none since).
