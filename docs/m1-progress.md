# M1 progress: Horizon's Java layer on stock Android 14 x86_64

## Running it

```powershell
python tools\emulator.py create            # once: the prism-api34 AVD
python tools\emulator.py start --wait
python tools\emulator.py root              # once per AVD: writable system partitions
python tools\emulator.py adb pull /apex/com.android.runtime/lib64/bionic work\device\  # once: stock bionic, for translator.py's check
python tools\jni\build.py                  # Prism's JNI glue for this Horizon build
python tools\compat.py                     # bridged framework.jar
python tools\translator.py                 # Digitalis ARM64 translator, adapted to Android 14
python tools\vulkan.py                     # Prism's Vulkan driver (opaque-fd memory for the compositor)
python tools\gles.py                       # Prism's GLES layer (GL_EXT_memory_object_fd, for GLES apps)
python tools\thermal.py                    # Prism's thermal HAL (stable-AIDL IThermal, for vrdevice)
python tools\tracking.py                   # Prism's tracking service (MemoryBroker, the head pose)
python tools\hidl.py                       # Prism's HIDL services (controller HAL, system suspend)
python tools\relay.py                      # Prism's binder relay (arm64 apps' binder objects)
python tools\spaces.py                     # Prism's space service (Meta's space manager, for system_server)
python tools\guest_media.py                # Prism's arm64 media functions (panels' surface sizes)
python tools\deploy.py                     # reset to stock, push Horizon's layer, reboot (onto the TSC)
python tools\emulator.py ready             # until Home is drawing: on the TSC, VrShell up, frames shared
python tools\viewer.py                     # Horizon in a desktop window
python tools\steamvr.py                    # or in a PC headset, through SteamVR
```

Each build leaves a stamp of what it was made from (`tools/stamp.py`: its sources' and inputs'
sizes and times) and does nothing while they are unchanged, so after a pull the whole list can be
run again and only what changed is built (`--force` builds anyway; compat alone takes minutes).
Deploy writes the fingerprint of everything it pushed to the guest (`/data/prism/deploy-id`), so
`deploy.deployed_id() == deploy.deploy_id(args)` says whether the running emulator has the current
build.

For 90 frames a second instead of 60 (the PC's monitor can be any rate; the frames go to the viewer
through shared memory):

```powershell
python tools\emulator.py stop
python tools\emulator.py config --refresh 90      # the AVD's display rate
python tools\emulator.py patch-share-vid          # the emulator's frame sharing allows 90 (--undo restores)
python tools\emulator.py start --wait  # no emulator window at 90 Hz (it costs frames); --window keeps it
python tools\deploy.py                            # the compositor, apps and Android's minimum rate follow
python tools\perf.py --guest                      # shared frame rate, jitter, busiest guest threads
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
  ones, `vkGetMemoryFdKHR` returns one end of a Unix socket, and a thread in the exporting process
  sends the memory's AHardwareBuffer down the other each time an import asks for it, so an fd and
  its dups import any number of times (four queued copies were a fixed budget before, and a failed
  import used one up). Fences and semaphores already export
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
  that), then Horizon adds the `com.oculus` account itself. Horizon removed that account again
  (2026-10-05), and the busy-wait came back as a dispatcher thread at 55% of a vCPU plus about as
  much in system_server's binder threads. It is the loop in the flow's coroutine (R8's `X/28c`)
  that asks again at once while the id is empty; `tools/compat.py` patches LibraryPanelApp.apk to
  wait a second between asks, account or not (system_server went from 67% of a vCPU to 8%). With
  an account but no token, VrShell
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
- **The emulator freezing when an app is killed mid-upload** (2026-10-21; twice that night while
  opening and force-stopping WhatsApp in a loop): the same hang detector, after qemu's main loop and
  the vCPUs stopped for 15 s. One vCPU was closing a GL context (qemu's
  `AddressSpaceGraphicsContext` destructor, run by the guest's write as the dead app's connection
  closed) and waited, holding qemu's lock, for the context's render thread to finish; that thread
  was spinning in gfxstream's ring reader (`RingStream::readRaw`), which waits out a large transfer
  it has begun (ASG's type 3, used for big uploads: `transfer_size` still nonzero) and looks at
  nothing else, for bytes the killed app was never going to write. A guest program uploading a
  64 MB texture over and over, killed every 0.5-3 s, froze the emulator at the 12th kill.
  `python tools/emulator.py patch-transfers` (a setup step, checked by doctor) has the destructor
  also set the ring's `in_error` (the guest abandons a transfer when it's set, and gfxstream sets it
  for a broken one), from padding it calls in both qemu builds, and has the reader stop waiting for
  a transfer when it's set: the reader then takes the Exit the destructor queued and the thread
  ends. A live transfer is waited out as before. Patched: 60 kills in a row, the guest answered
  after each and no render thread was left spinning; Home at 90 frames a second.
- **VrShell's memory growing while idle** (2026-10-21): its native heap grew ~4.7 MB a minute with
  nothing happening (183 MB fresh, 445 MB after two hours). Android's heap profiler put nearly all
  of it under one call, Meta's arm64 code allocating through libc, which it can't unwind past; the
  newest blocks were vectors of `PersistableBundle`, the type of Meta's
  `ITelemetryService::reportEvents`. Every app's telemetry client (libossdk) queues its events for
  TelemetryService, Horizon's telemetry daemon, which uploads them to Meta; Prism doesn't run it, so
  the queue was never sent and never dropped. Deploy sets the client's own switch,
  `persist.telemetry.drop_all_client_events`: set on a running system, VrShell's heap stopped
  growing at once (445.9 -> 445.2 MB over four minutes). Whether it's what made VrShell slow down
  after long use isn't established: a VrShell restarted fresh (88-89 VrApi FPS, still or moving)
  held 84-90 for half an hour and through 21 app opens with the leak still running, while the
  VrShell from boot had dropped to 73 still and 65 moving after the same opens.
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
  second, 10 ms of jitter, against 60 and 1.4 ms with both). The emulator runs with
  `-feature AsyncComposeSupport`: the composer HAL otherwise waits out each composition on the PC
  (rcCompose, 8-9 ms a frame), and with panels redrawing under the controller's ray the display
  lost frames while the compositor kept 60 (52.8 shared frames a second, against 60 with it).
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
  The running-app card says "App name unavailable", for xrdemo and for xrgame (installed with adb,
  listed under Unknown Sources) alike. The navigator (LibraryPanelApp's
  `ActiveImmersiveRepositoryImpl`) names the running app from its task's label, which a
  NativeActivity that never calls `setTaskDescription` doesn't have, and failing that from the
  display name in OCMS's package record, through the library's package repository; neither gives
  one here. Which step of that lookup misses isn't established (the library's own repository
  leaves out apps of no store channel, as xrgame is).
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
  Downloads. Unknown Sources lists the apps installed with adb, under Meta's warning. The library
  service (OCMS) adds an install as it happens; the ones already installed it adds at start only
  once it has fetched a signed-in user's library, so with no Meta account they were gone after
  every boot, and with them Unknown Sources, until an app was installed again. `tools/compat.py`
  has OCMS add them as soon as it has loaded the device's apps (2026-10-21: after a cold boot,
  Unknown Sources lists the 11 apps `tools/apps.py` copied, and WhatsApp opens from it). On a
  headset Meta's apps are the Store's and sit under All; here they have no channel, so OCMS counts
  a Meta package (`tools/apps.py`'s prefixes) with none as the Store's, in the library's origin
  filter and in the origin it reports (its channel stays NONE, and no entitlement is made): All
  lists Facebook, Instagram, WhatsApp, Theater Elsewhere and the rest, WhatsApp opens from it, and
  Unknown Sources is left for other developers' apps. Clicking xrgame there launches
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
  gatekeepers (`gkcache.dat`) follow its client's values from its next start. A client that
  rewrites its cache at start with what it synced undoes them, as systemdriver's does (the list is
  empty for now).
- **Frosted glass.** Panels' glass backgrounds are the compositor's: it blurs what's behind a glass
  layer (`XR_METAX1_composition_layer_glass`). It sets glass up only for the headsets it knows and
  when `oculus_xrruntime:oculus_frosted_glass` is on. libprism_jni patches both checks, one
  instruction of `libvrruntimeservice.so` each, written as the translator loads it, after checking
  the bytes ([guest_patch.c](../native/prism_jni/guest_patch.c)): the device type, and the
  gatekeeper's answer, since systemdriver's config client rewrites its cache to its synced value
  (false without a Meta account) at every boot, over what deploy had set. The compositor logs
  "Glass is enabled" at every boot, with no change in frame rate, but panels still show none:
  VrShell asks for glass on few of its layers (OCUI theme and per-panel checks), and where glass
  was forced on every layer, its background was the compositor's solid fallback colour, as no
  layer is chosen as the source to blur (no passthrough here).
- **SteamVR** (M5; tried with a headset on 2026-10-11: focused and tracking, 90 frames a second
  submitted, and performance good enough to use). `python tools\steamvr.py` starts
  `native/steamvr`, an OpenXR app on the PC (Direct3D 11; its loader, session and controller actions
  follow Refract's host bridge). Each frame of the PC runtime it sends the headset's pose, both
  controllers' aim poses (turned back as the viewer's are) and their buttons to the tracking service,
  as the viewer does. Each new frame the emulator shares goes to the headset as a projection layer,
  a view per half, posed where the headset was when Horizon rendered it, so the PC runtime reprojects
  Horizon's 60 frames a second to the headset's rate. That pose is the one sent `--latency` ms
  (default 45) before the frame arrived; + and - tune it in the console until the world holds still
  as the head turns. Each view is submitted from Horizon's eye (`--ipd`, which xrprobe logs), with
  the viewer's fitted edges moved by the parallax at the depth they were fitted at. The origin is
  where the headset is when tracking starts (R again, F9 in any window, or the PC runtime's own
  recenter: LOCAL's change). The left menu button is the Meta button (PC runtimes keep the right
  system button); F7 and F8 on the keyboard hold the Meta and left menu buttons, in any window, and
  also while the PC runtime's dashboard has the controllers. The thumbsticks go through: the input
  line takes x and y after the triggers, and tracking_prism publishes them in CONTROLLER's thumbstick
  snapshot at 0x250, which the tracking client reads as 7 bytes (touched, a byte not known, x and y
  as u16s over 0-65535, centered at 0x7fff, up the smaller y, valid), as xrprobe showed: an app sees
  the stick pushed right as x 1 and up as y 1. Haptics come back: tracking_prism serves
  `IHapticsService/default` ([haptics.c](../native/tracking_prism/haptics.c)), whose one call hands
  the runtime a socket; the runtime writes {u32 type, u32 length} messages into it, and for an app's
  xrApplyHapticFeedback sets the amplitude (type 1: u64 device id, u8 0-255) and sets it back to 0
  when the duration is over (xrprobe vibrates on a trigger press: 0xff, then 0 200 ms later).
  VrShell's UI ticks are parametric effects (XR_EXT_haptic_parametric), which the runtime's haptics
  plugin (`libtrex_haptics_server_plugin.so`, in VrDriver) plays only at a location the controller
  describes: CONTROLLER's three haptic channels from 0x4b0 (snapshots of 0x85 bytes, 0x118 apart:
  the location, four actuator entries {present, type, sample rate, samples, ...}, valid), which
  were empty ("Parametric haptics is not supported for the location"). tracking_prism describes
  each channel as buffered (type 2) at 320 Hz, 25 samples a submission, as Touch's were; the plugin
  then renders them into buffered messages (type 3: u64 device, u8 count, 25 amplitudes), a hover
  being 17 samples, three at 50/255. Each amplitude goes back up the pose connection ("haptic l|r
  amplitude"), each buffer as a pulse of its peak for as long as it isn't silent ("haptic l|r
  amplitude seconds"), and prism_vr plays them on that controller. PCM messages are only logged.
  Each frame's pose is
  found from when the emulator shared it (VideoInfo's time: Unix microseconds) rather than when
  prism_vr saw it, a PC frame later at most. The compositor's window per eye was 1.37 by 1.54
  tangent units (69° by 75°) against 2.15 across in what apps render, since the window is a
  physical screen (the compositor's default Quest 1 headset) seen at `metersPerTanAngCtr` meters a
  unit. Deploy sets `debug.oculus.headsetOverride=1` and `debug.oculus.metersPerTanAngCtr=0.0264`
  (from about 0.039), which the runtime reads at its start ("Custom Headset Overrides Enabled" in its
  log): each half now shows 2.03 by 2.28 (91° by 97°), nearly all apps render, 1.48 times the
  default's, and the pointer's dot lands within a few pixels of the fitted edges (kViews, scaled
  by that). The `UPDATE_HMDMESH` broadcast, which should reload them, never reached the runtime's
  receiver; killing the runtime service reloads them but took the emulator down the second time.
  The view stays narrower than a headset's lenses, and a frame arrives ~45 ms after its pose, so
  turning the head showed black past its edges. Since 2026-10-21 the bridge keeps the world as last
  seen behind it: each frame is painted into a cube map per eye, and a projection layer under
  Horizon's draws that cube from where the headset is now (an offscreen check with synthetic
  frames: within 3 levels of the world on 99.7% of pixels wherever a frame had been, black only
  where none had; untried in a headset). The display is 2560x1440
  (`tools/emulator.py config --display`), about 630 pixels a tangent unit against 474 at 1920x1080,
  over the same angles: the compositor draws its panel at 3840x2160 whatever the display, and
  Prism's Vulkan driver fits it to the output, so only the output's size changes. That output is
  Prism's stand-in for the compositor's arm64 window, which reports no size; it was made 1920x1080
  and Horizon filled only that corner of a larger display. It now takes the display's size
  (`Display.getRealSize`). Deploy sets `frontbufferHeight` (1080, 1440 or 2160) to the display's
  height. Horizon's home holds 59.7 shared frames a second at 2560x1440 (59.2 at 1080p). The
  guest has 12 GB (8 had the home 3 GB into swap).
- **Performance** (`python tools/perf.py --guest`: the shared frames' rate and interval percentiles, the
  compositor's `FPS=`, the busiest guest threads; each run is a line of `work/logs/perf.csv`).
  - *60 Hz, settled (2026-10-09):* 59.9 shared frames a second, p99 19 ms. Prism's Vulkan driver is not
    the cost: its stats line (now with time in waits and in vkQueueSubmit) shows one copy into and one
    out of a mirror a frame and vkQueueSubmit at 0.08 ms (compositor) to 0.3 ms (VrShell) a submit.
    VrShell's `ShellFrameLoop` is at 69% of a vCPU, all user time in translated code;
    `berberis.gear_switch_threshold` (default 1000) at 100 changed nothing, so the translator's
    tiering is not the lever.
  - *The 60 cap* is the emulator's frame sharing (`-share-vid`, VideoFrameSharer): its constructor
    starts fps at 60 and a frame within 1/fps of the last is skipped (72 Hz gave 36).
    `python tools/emulator.py patch-share-vid` sets that one immediate (checked against the
    instructions around it, in both qemu builds; the originals stay as `.orig`, `--undo` restores
    them), and `emulator.py config --refresh 90` + deploy gives the display, the compositor and apps
    the rate (`SupportedDisplayRefreshRates: [ 45 90 ]`).
  - *90 Hz, patched (2026-10-09):* the emulator reported 90 fps and the compositor 89 of 90, yet only
    45 frames a second were shared, with no frame numbers skipped: not the sharing cap, the display.
    SurfaceFlinger's scheduler (`dumpsys SurfaceFlinger`: the time spent at each rate) sat at a 45 Hz
    *render rate*, half of the display's one 90 Hz mode, for 8 minutes of every 8; with nothing asking
    for more, it composes every other vsync, and the compositor's layer got 45 buffers a second
    (`dumpsys SurfaceFlinger --latency <layer>`: frameReady 22 ms apart). Android's minimum and peak
    refresh rate Settings pin it (`settings put system min_refresh_rate 90.0`, peak the same; deploy's
    prism.rc does it at boot, as the shell user: from system, uid 1000 with no package, `settings
    put` throws in getCallingPackage, which went unseen until a new AVD stayed at 45 (2026-10-12);
    the Settings set by hand had stayed in /data): then 89.7 shared frames a second, mean 11.2 ms,
    p99 14.6 ms, the compositor 83-89 of 90.
    From a fresh deploy and boot: both Settings 90.0,
    SurfaceFlinger only ever at 90 Hz, 89.4 shared frames a second, but p99 18.5 ms, 39 gaps over
    1.5 frames in 30 s and the compositor at 72 of 90: the average is 90, the jitter is not yet
    (VrShell's frame loop below).
  - *The emulator's own window costs a lot at 90 Hz:* with it open (the default), the compositor
    reached 49 of 90 and the guest's render threads were ~90% system time (a synchronous call to
    gfxstream's render thread under WHPX counts as the caller's kernel time; vkQueueSubmit cost 0.5-2
    ms a submit, seven times 60 Hz's). `emulator.py start --headless` (now the default above 60 Hz; `--window` keeps the window) took that away (compositor 89
    of 90, system time 34% of the guest's CPU instead of 203%); the viewer reads the shared frames, so
    it doesn't need the window. The RTX 3080 never leaves P3/P5 (675-1185 MHz of 2100) under this
    bursty load; NVIDIA's "Prefer maximum performance" for qemu-system-x86_64[-headless].exe is the
    thing to try for the submit latency.
  - *What is left:* VrShell's `ShellFrameLoop` thread is at ~93% of a vCPU at 90 Hz (69% at 60): about
    10.3 ms of translated-code CPU a frame in an 11.1 ms budget, so p99 (14.6 ms) and the 22 gaps
    over 1.5 frames in 30 s are its jitter. That is the next target, with the translator's hot paths.
    `log.tag.berberis=I` (deploy) stops Digitalis logging every region it translates (311,000 lines
    in one hour).
  - *Where ShellFrameLoop's time goes (2026-10-09):* `simpleperf record -e cpu-clock -t <tid>` in the
    guest puts 94% of its samples in translated code ("unknown"), 2.5% in the kernel, 1.9% in
    bionic, 0.2% in libberberis: not the translator's runtime, lookups or helpers. Digitalis's
    `berberis.interp_stats=1` (logs as `berberis_interp`) shows almost nothing interpreted: 27
    entries a second in VrShell, and the one hot spot is a `mrs cntvct_el0` in libvrapiimpl
    (~3,000 trapping reads a second in one thread, 24,000 instructions a second). Pinning the
    emulator to one thread per physical core (0x5555), or to High priority, changed nothing
    measurable. The runs vary with what else the PC does: with another process using a core
    (100%) the same build gave 88 frames a second, p99 21 ms and 110+ gaps in 60 s, against 89.6,
    p99 14.5 ms and 15 gaps with the PC quiet, so measure with it quiet. The jitter is VrShell's
    ~10.4 ms of translated code a frame in an 11.1 ms budget; shrinking it needs the translator's
    code quality, or less work in the shell, not a driver or emulator change.
  - *Slower the longer it runs (2026-10-17):* after hours of opening apps the shared frames fell from
    85 to ~42 a second and stayed there until the emulator restarted. The host's render thread spent
    ~58% of a core in the kernel's `NtUserWindowFromDC`, which NVIDIA's `DrvSetContext` calls on every
    `wglMakeCurrent` and which walks the session's own-DC windows, newest first; the emulator had
    1057 hidden `NVOpenGLPbuffer` windows, one for each WGL pbuffer (holding 500, 1000 or 2000 windows
    in another process took a fresh emulator from 90 to 84, 71 and 38). gfxstream gives every guest
    EGL surface a 1x1 pbuffer from a pool (`libgfxstream_backend`'s WGL display; the pool grows by as
    many as are in use when it runs dry) and puts it back on release only if `wglReleasePbufferDCARB`
    succeeds. On NVIDIA it mostly doesn't: making a context current rewrites the DC the driver keeps
    for a list of drawables, so the DC gfxstream got for a pbuffer no longer matches it
    (`0xC0070591`), and gfxstream gives up on that pbuffer, which stays counted in use. Of 272 in use
    one evening, 251 were such leftovers. `python tools/emulator.py patch-pbuffers` (a setup step)
    patches out the jump on that failure, so the pbuffer goes back to the pool; gfxstream binds a
    pbuffer with the DC it gets when handing it out, so the driver's record isn't needed. Patched:
    160 guest surfaces made and destroyed, and 24 apps opened and closed, left the count in use where
    it was (unpatched, each 20 surfaces kept 19), with no failed or retried `wglMakeCurrent`. A
    30-minute soak from a fresh boot opening an app every 30 s (60 opens, `soak.py`'s new
    `emulator_windows` column) held 103-107 windows throughout, 88.5 shared frames a second on
    average, nothing crashed; unpatched, as many opens had taken the count from 49 into the hundreds.
- **Next:** panels' glass backgrounds; system_server's deaths by SIGPIPE (twice,
  soon after an app's window was placed; none since).
