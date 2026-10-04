/*
 * xrprobe: an OpenXR app that reports what Horizon's runtime gives it, for seeing why an app's
 * controllers aren't there. It binds the controllers as VrShell does (grip and aim poses, the
 * trigger's value, for Touch Plus and Touch), runs a session, and logs (tag XrProbe) every event
 * and, once a second: xrSyncActions's result, each hand's current interaction profile, the grip
 * and aim locations' flags in LOCAL space, the trigger, and the headset's own location. The runtime
 * runs in this process, so it also reads the runtime's own state (libvrapiimpl.so's, by address):
 * which way sync went, the controller snapshot it built, the session's selected profiles, each
 * input device's in-hand flag, and what the tracking client reads as in hand.
 *
 * It loads Meta's OpenXR loader (libopenxr_loader.so, from the image), renders nothing and submits
 * no layers.
 */
#include <EGL/egl.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <jni.h>
#include <link.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define XR_NO_PROTOTYPES  // its functions come from the loader, by name
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define TAG "XrProbe"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define ERR(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static PFN_xrGetInstanceProcAddr get_proc;
static XrInstance instance;

#define XR_FUNCTIONS(F)                                                                                     \
  F(xrEnumerateInstanceExtensionProperties) F(xrCreateInstance) F(xrGetSystem) F(xrCreateSession)           \
  F(xrBeginSession) F(xrEndSession) F(xrPollEvent) F(xrWaitFrame) F(xrBeginFrame) F(xrEndFrame)             \
  F(xrCreateReferenceSpace) F(xrCreateActionSpace) F(xrLocateSpace) F(xrStringToPath) F(xrPathToString)     \
  F(xrCreateActionSet) F(xrCreateAction) F(xrSuggestInteractionProfileBindings) F(xrAttachSessionActionSets) \
  F(xrSyncActions) F(xrGetActionStateFloat) F(xrGetActionStatePose) F(xrGetCurrentInteractionProfile)       \
  F(xrResultToString) F(xrGetOpenGLESGraphicsRequirementsKHR)

#define DECLARE(name) static PFN_##name name;
XR_FUNCTIONS(DECLARE)

static bool load(const char *name, PFN_xrVoidFunction *out) {
  XrResult r = get_proc(instance, name, out);
  if (r != XR_SUCCESS || !*out) ERR("no %s (%d)", name, r);
  return r == XR_SUCCESS && *out;
}

static bool load_all(void) {
  bool ok = true;
#define LOAD(name) ok &= load(#name, (PFN_xrVoidFunction *)&name);
  XR_FUNCTIONS(LOAD)
  return ok;
}

static const char *result(XrResult r) {
  static char text[XR_MAX_RESULT_STRING_SIZE];
  if (!instance || !xrResultToString || xrResultToString(instance, r, text) != XR_SUCCESS) snprintf(text, sizeof text, "%d", r);
  return text;
}

#define CHECK(call)                                     \
  do {                                                  \
    XrResult r_ = (call);                               \
    if (XR_FAILED(r_)) {                                \
      ERR("%s: %s", #call, result(r_));                 \
      return false;                                     \
    }                                                   \
  } while (0)

static XrPath path(const char *text) {
  XrPath p = XR_NULL_PATH;
  if (xrStringToPath(instance, text, &p) != XR_SUCCESS) ERR("no path %s", text);
  return p;
}

static const char *path_text(XrPath p) {
  static char text[XR_MAX_PATH_LENGTH];
  uint32_t n = 0;
  if (p == XR_NULL_PATH) return "(none)";
  if (xrPathToString(instance, p, sizeof text, &n, text) != XR_SUCCESS) snprintf(text, sizeof text, "path %llu", (unsigned long long)p);
  return text;
}

static bool loader(struct android_app *app) {
  static const char *const candidates[] = {"libopenxr_loader.so", "/system_ext/lib64/libopenxr_loader.so"};
  void *lib = NULL;
  for (size_t i = 0; !lib && i < sizeof candidates / sizeof *candidates; i++) {
    lib = dlopen(candidates[i], RTLD_NOW);
    LOG("loading %s: %s", candidates[i], lib ? "ok" : dlerror());
  }
  if (!lib || !(get_proc = (PFN_xrGetInstanceProcAddr)dlsym(lib, "xrGetInstanceProcAddr"))) return false;
  PFN_xrInitializeLoaderKHR initialize = NULL;
  get_proc(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&initialize);
  if (initialize) {
    XrLoaderInitInfoAndroidKHR init = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR, NULL, app->activity->vm, app->activity->clazz};
    XrResult r = initialize((const XrLoaderInitInfoBaseHeaderKHR *)&init);
    LOG("xrInitializeLoaderKHR: %d", r);
  }
  return true;
}

// The probe's state.
static XrSystemId system_id;
static XrSession session;
static XrSessionState state = XR_SESSION_STATE_UNKNOWN;
static bool running;
static XrSpace local, view, stage;
static XrActionSet set;
static XrAction grip, aim, trigger;
static XrPath hands[2];
static XrSpace grip_space[2], aim_space[2];

static bool create_instance(struct android_app *app) {
  PFN_xrEnumerateInstanceExtensionProperties enumerate = NULL;
  get_proc(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction *)&enumerate);
  static XrExtensionProperties props[512];
  uint32_t count = 0;
  for (uint32_t i = 0; i < 512; i++) props[i] = (XrExtensionProperties){XR_TYPE_EXTENSION_PROPERTIES};
  if (!enumerate || enumerate(NULL, 512, &count, props) != XR_SUCCESS) ERR("can't enumerate extensions");
  const char *enabled[8];
  uint32_t n = 0;
  enabled[n++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
  enabled[n++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
  for (uint32_t i = 0; i < count; i++) {
    if (strstr(props[i].extensionName, "touch_controller")) LOG("extension %s", props[i].extensionName);
    if (!strcmp(props[i].extensionName, XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME)) enabled[n++] = XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME;
  }
  LOG("%u extensions", count);
  XrInstanceCreateInfoAndroidKHR android = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR, NULL, app->activity->vm, app->activity->clazz};
  XrInstanceCreateInfo info = {XR_TYPE_INSTANCE_CREATE_INFO, &android};
  strcpy(info.applicationInfo.applicationName, "xrprobe");
  info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  info.enabledExtensionCount = n;
  info.enabledExtensionNames = enabled;
  PFN_xrCreateInstance create = NULL;
  get_proc(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&create);
  XrResult r = create ? create(&info, &instance) : XR_ERROR_FUNCTION_UNSUPPORTED;
  if (XR_FAILED(r)) {
    ERR("xrCreateInstance: %d", r);
    return false;
  }
  return load_all();
}

static bool create_session(void) {
  XrSystemGetInfo system = {XR_TYPE_SYSTEM_GET_INFO, NULL, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
  CHECK(xrGetSystem(instance, &system, &system_id));
  XrGraphicsRequirementsOpenGLESKHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
  CHECK(xrGetOpenGLESGraphicsRequirementsKHR(instance, system_id, &requirements));

  EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  eglInitialize(display, NULL, NULL);
  const EGLint config_attribs[] = {EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
  EGLConfig config;
  EGLint configs = 0;
  eglChooseConfig(display, config_attribs, &config, 1, &configs);
  const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
  const EGLint surface_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
  EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attribs);
  if (!configs || context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) {
    ERR("no EGL context (%#x)", eglGetError());
    return false;
  }

  XrGraphicsBindingOpenGLESAndroidKHR binding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, NULL, display, config, context};
  XrSessionCreateInfo info = {XR_TYPE_SESSION_CREATE_INFO, &binding, 0, system_id};
  CHECK(xrCreateSession(instance, &info, &session));

  XrReferenceSpaceCreateInfo space = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, NULL, XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {0, 0, 0}}};
  CHECK(xrCreateReferenceSpace(session, &space, &local));
  space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  CHECK(xrCreateReferenceSpace(session, &space, &view));
  space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
  if (XR_FAILED(xrCreateReferenceSpace(session, &space, &stage))) stage = XR_NULL_HANDLE;
  return true;
}

static bool create_actions(void) {
  hands[0] = path("/user/hand/left");
  hands[1] = path("/user/hand/right");
  XrActionSetCreateInfo set_info = {XR_TYPE_ACTION_SET_CREATE_INFO};
  strcpy(set_info.actionSetName, "probe");
  strcpy(set_info.localizedActionSetName, "Probe");
  CHECK(xrCreateActionSet(instance, &set_info, &set));
  struct {
    XrAction *action;
    XrActionType type;
    const char *name;
  } actions[] = {{&grip, XR_ACTION_TYPE_POSE_INPUT, "grip"},
                 {&aim, XR_ACTION_TYPE_POSE_INPUT, "aim"},
                 {&trigger, XR_ACTION_TYPE_FLOAT_INPUT, "trigger"}};
  for (size_t i = 0; i < sizeof actions / sizeof *actions; i++) {
    XrActionCreateInfo info = {XR_TYPE_ACTION_CREATE_INFO, NULL, "", actions[i].type, 2, hands};
    strcpy(info.actionName, actions[i].name);
    strcpy(info.localizedActionName, actions[i].name);
    CHECK(xrCreateAction(set, &info, actions[i].action));
  }
  static const char *const profiles[] = {"/interaction_profiles/meta/touch_controller_plus", "/interaction_profiles/oculus/touch_controller"};
  for (size_t p = 0; p < 2; p++) {
    XrActionSuggestedBinding bindings[] = {
        {grip, path("/user/hand/left/input/grip/pose")},      {grip, path("/user/hand/right/input/grip/pose")},
        {aim, path("/user/hand/left/input/aim/pose")},        {aim, path("/user/hand/right/input/aim/pose")},
        {trigger, path("/user/hand/left/input/trigger/value")}, {trigger, path("/user/hand/right/input/trigger/value")},
    };
    XrInteractionProfileSuggestedBinding suggested = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, NULL, path(profiles[p]),
                                                      sizeof bindings / sizeof *bindings, bindings};
    XrResult r = xrSuggestInteractionProfileBindings(instance, &suggested);
    LOG("suggested %s: %s", profiles[p], result(r));
  }
  XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, NULL, 1, &set};
  CHECK(xrAttachSessionActionSets(session, &attach));
  for (int h = 0; h < 2; h++) {
    XrActionSpaceCreateInfo info = {XR_TYPE_ACTION_SPACE_CREATE_INFO, NULL, grip, hands[h], {{0, 0, 0, 1}, {0, 0, 0}}};
    CHECK(xrCreateActionSpace(session, &info, &grip_space[h]));
    info.action = aim;
    CHECK(xrCreateActionSpace(session, &info, &aim_space[h]));
  }
  return true;
}

static void poll_events(void) {
  XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(instance, &event) == XR_SUCCESS) {
    switch (event.type) {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
        state = ((XrEventDataSessionStateChanged *)&event)->state;
        LOG("session state %d", state);
        if (state == XR_SESSION_STATE_READY) {
          XrSessionBeginInfo begin = {XR_TYPE_SESSION_BEGIN_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
          XrResult r = xrBeginSession(session, &begin);
          LOG("xrBeginSession: %s", result(r));
          running = XR_SUCCEEDED(r);
        } else if (state == XR_SESSION_STATE_STOPPING) {
          xrEndSession(session);
          running = false;
        }
        break;
      }
      case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
        LOG("interaction profile changed");
        break;
      default:
        LOG("event %d", event.type);
    }
    event = (XrEventDataBuffer){XR_TYPE_EVENT_DATA_BUFFER};
  }
}

static void locate(const char *name, XrSpace space, XrSpace base, XrTime time) {
  XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
  XrResult r = xrLocateSpace(space, base, time, &location);
  const XrVector3f *p = &location.pose.position;
  const XrQuaternionf *q = &location.pose.orientation;
  LOG("  %-10s %s flags %#llx  p %.2f %.2f %.2f  q %.2f %.2f %.2f %.2f", name, result(r), (unsigned long long)location.locationFlags,
      p->x, p->y, p->z, q->x, q->y, q->z, q->w);
}

struct lookup {
  const char *name;
  void *found;
};

static uintptr_t runtime;  // libvrapiimpl.so's load address

static int find_in(struct dl_phdr_info *info, size_t size, void *data) {
  (void)size;
  struct lookup *lookup = data;
  if (!info->dlpi_name || !strstr(info->dlpi_name, "libvrapiimpl.so")) return 0;
  runtime = info->dlpi_addr;
  for (int i = 0; i < info->dlpi_phnum; i++) {
    if (info->dlpi_phdr[i].p_type != PT_DYNAMIC) continue;
    const ElfW(Dyn) *dyn = (const ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    const ElfW(Sym) *symbols = NULL;
    const char *strings = NULL;
    for (; dyn->d_tag != DT_NULL; dyn++) {
      if (dyn->d_tag == DT_SYMTAB) symbols = (const ElfW(Sym) *)(info->dlpi_addr + dyn->d_un.d_ptr);
      if (dyn->d_tag == DT_STRTAB) strings = (const char *)(info->dlpi_addr + dyn->d_un.d_ptr);
    }
    if (!symbols || !strings || (const char *)symbols >= strings) return 1;
    for (const ElfW(Sym) *s = symbols; (const char *)(s + 1) <= strings; s++)
      if (s->st_value && !strcmp(strings + s->st_name, lookup->name)) {
        lookup->found = (void *)(info->dlpi_addr + s->st_value);
        break;
      }
    return 1;
  }
  return 0;
}

// The runtime's own state for this session, read from inside it (it runs in this process): which
// way xrSyncActions goes, and the controller snapshot it builds for its interaction profiles. The
// addresses are libvrapiimpl.so's (Horizon v76): xrSyncActions (0x7b80f4) finds the session object
// through the client state's handle manager, takes a fast path (0x7b82dc) when a gatekeeper and
// the session's byte +0x46c8 allow, and otherwise converts the input devices (0x7c4c00) into the
// snapshot at +0x1e28.
static uintptr_t runtime_base(void) {
  struct lookup lookup = {"vrapi_EnumerateInputDevices", NULL};
  if (!dl_iterate_phdr(find_in, &lookup)) return 0;
  return runtime;
}

static void hex(const char *name, const uint8_t *p, size_t n) {
  char text[3 * 64 + 1];
  size_t i;
  for (i = 0; i < n && i < 64; i++) snprintf(text + 3 * i, 4, "%02x ", p[i]);
  text[3 * i] = 0;
  LOG("  %-12s %s", name, text);
}

static uint8_t *session_object(void) {
  static uint8_t *object;
  if (object) return object;
  uintptr_t base = runtime_base();
  if (!base) return NULL;
  void *client = *(void **)(base + 0x949960);
  void **table = *(void ***)(base + 0x949968);
  if (!client || !table) {
    ERR("no client state");
    return NULL;
  }
  void *manager = ((void *(*)(void *))table[0])(client);
  XrSession handle = session;
  object = ((void *(*)(void *, void *, int))(*(void ***)manager)[0x30 / 8])(manager, &handle, 2);
  if (!object) ERR("no session object");
  return object;
}

// The snapshot's four device variants (left: +0x1f8, +0x398; right: +0x538, +0x6d8; each with its
// variant index at +0x190 and engaged flag at +0x198) and the two hands' input slots (+0xab8,
// +0xb70; their first word is the input type, 4, which a successful input read writes).
static const uint32_t device_offsets[4] = {0x1f8, 0x398, 0x538, 0x6d8};
static const uint32_t slot_offsets[2] = {0xab8, 0xb70};
static int frames, engaged[4], read_ok[2];

static void before_sync(void) {
  uint8_t *o = session_object();
  if (!o) return;
  for (int h = 0; h < 2; h++) *(uint32_t *)(o + 0x1e28 + slot_offsets[h]) = 0xeeeeeeee;
}

static void after_sync(void) {
  uint8_t *o = session_object();
  if (!o) return;
  const uint8_t *snapshot = o + 0x1e28;
  frames++;
  for (int i = 0; i < 4; i++) engaged[i] += snapshot[device_offsets[i] + 0x198] == 1;
  for (int h = 0; h < 2; h++) read_ok[h] += *(const uint32_t *)(snapshot + slot_offsets[h]) != 0xeeeeeeee;
}

// Calls fn(a0, a1) for a result returned through x8 (a C++ object returned by value).
static void call_x8(void *fn, void *a0, uint64_t a1, void *result) {
  register void *x0 __asm__("x0") = a0;
  register uint64_t x1 __asm__("x1") = a1;
  register void *x8 __asm__("x8") = result;
  register void *x16 __asm__("x16") = fn;
  __asm__ volatile("blr x16"
                   : "+r"(x0), "+r"(x1), "+r"(x8), "+r"(x16)
                   :
                   : "x2", "x3", "x4", "x5", "x6", "x7", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "x18", "x30",
                     "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
                     "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "memory", "cc");
}

// The session's action state, which xrGetCurrentInteractionProfile reads (0x559214 -> 0x58ba1c): in
// the action system (the client state's function table, slot 4), its map at +0x90 by session handle
// (0x5629f8). Its byte +0xc8 is set once action sets are attached; +0x130 heads the list of selected
// profiles (+0x138 counts them), each node's +0x18 a profile object: +0x10 the selected profile path,
// +0x18 the top-level path, +0x20 the device type, +0x28 the device id.
static uint8_t *action_state(void) {
  static uint8_t *state;
  if (state) return state;
  uintptr_t base = runtime_base();
  void *client = *(void **)(base + 0x949960);
  void **table = *(void ***)(base + 0x949968);
  uint8_t *actions = ((void *(*)(void *))table[4])(client);
  void *found[2] = {0};  // a shared pointer, kept (one reference leaked)
  call_x8((void *)(base + 0x5629f8), actions + 0x90, (uint64_t)session, found);
  state = found[0];
  if (!state) ERR("no action state");
  return state;
}

static void dump_profiles(void) {
  uint8_t *state = action_state();
  if (!state) return;
  LOG(" action state %p: attached %d, profiles %llu", state, state[0xc8], (unsigned long long)*(uint64_t *)(state + 0x138));
  for (uint8_t **node = *(uint8_t ***)(state + 0x130); node; node = (uint8_t **)node[0]) {
    uint8_t *profile = node[3];
    if (!profile) continue;
    LOG("  profile %s for %s, device type %u, id %#llx", path_text(*(XrPath *)(profile + 0x10)), path_text(*(XrPath *)(profile + 0x18)),
        *(uint32_t *)(profile + 0x20), (unsigned long long)*(uint64_t *)(profile + 0x28));
  }
}

// The runtime's input manager, as the sync's controller conversion (0x7c4c00) gets it from its
// context {vtable 0x91ec48, session}; for each device (0x4f9588: {type, id}), whether the runtime
// takes a controller (type 4) as held in a hand (0x4fa284), which decides whether an app without
// XR_META_detached_controllers sees it.
static void dump_in_hand(void) {
  uint8_t *o = session_object();
  if (!o) return;
  void *context[2] = {(void *)(runtime + 0x91ec48), o};
  void *manager = ((void *(*)(void *))(*(void ***)context)[2])(context);
  for (uint32_t i = 0; i < 8; i++) {
    uint32_t header[2] = {0};
    if (((int (*)(void *, uint32_t, void *))(runtime + 0x4f9588))(manager, i, header)) break;
    uint8_t in_hand = 0xee;
    int r = header[0] == 4 ? ((int (*)(void *, uint32_t, void *))(runtime + 0x4fa284))(manager, header[1], &in_hand) : -1;
    LOG("  input device %u: type %u, id %#x, in hand %d (%d)", i, header[0], header[1], in_hand, r);
  }
}

// What Horizon's tracking client says about each controller's in-hand flag: the controller input
// interface the runtime service queries (v8, 0x3f0), its method +0x10 for a device id: {u8 value, u8
// has a value, ...} packed in a u64.
static void dump_client_in_hand(void) {
  static void *input;
  if (!input) {
    void *lib = dlopen("libtrackingserviceclients.so", RTLD_NOW);
    void *(*create)(long) = lib ? (void *(*)(long))dlsym(lib, "createControllerInputFbs") : NULL;
    input = create ? create(0x3f0) : NULL;
    if (!input) {
      ERR("no controller input: %s", lib ? "create failed" : dlerror());
      return;
    }
  }
  uint64_t (*in_hand)(void *, uint64_t) = (uint64_t (*)(void *, uint64_t))(*(void ***)input)[2];
  static const uint64_t ids[] = {0, 1, 2, 3, 0x20000002, 0x20000003};
  for (size_t i = 0; i < sizeof ids / sizeof *ids; i++)
    LOG("  client in hand(%#llx): %#llx", (unsigned long long)ids[i], (unsigned long long)in_hand(input, ids[i]));
}

static void dump_session(void) {
  uint8_t *o = session_object();
  if (!o) return;
  void *system = ((void *(*)(void *))(*(void ***)o)[0x208 / 8])(o);
  void *features = ((void *(*)(void *))(*(void ***)system)[0x38 / 8])(system);
  bool (*feature)(void *, int, int) = (bool (*)(void *, int, int))(*(void ***)features)[0x10 / 8];
  bool fast = feature(features, 0x100, 1);
  LOG(" features object %p (vtable +%#lx, query +%#lx)", features, (unsigned long)(*(uintptr_t *)features - runtime),
      (unsigned long)((uintptr_t)feature - runtime));
  for (int i = 0; i < 0x140; i += 64) hex("levels", (const uint8_t *)features + 8 + i, 64);
  LOG(" features: 0x7b %d, 0xac %d, 0x63 %d, 0x100 %d", feature(features, 0x7b, 1), feature(features, 0xac, 1), feature(features, 0x63, 1),
      fast);
  LOG(" session %p: fast path %d/%d; of %d frames, input read L %d R %d; engaged %d %d | %d %d", o, fast, o[0x46c8], frames,
      read_ok[0], read_ok[1], engaged[0], engaged[1], engaged[2], engaged[3]);
  const uint8_t *snapshot = o + 0x1e28;
  for (int i = 0; i < 4; i++) {
    LOG("  device +%#x: engaged %d, index %d", device_offsets[i], snapshot[device_offsets[i] + 0x198],
        *(const int32_t *)(snapshot + device_offsets[i] + 0x190));
    hex("", snapshot + device_offsets[i], 32);
  }
  dump_profiles();
  dump_in_hand();
  dump_client_in_hand();
  frames = engaged[0] = engaged[1] = engaged[2] = engaged[3] = read_ok[0] = read_ok[1] = 0;
}

static void report(XrTime time) {
  XrActiveActionSet active = {set, XR_NULL_PATH};
  XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO, NULL, 1, &active};
  before_sync();
  XrResult synced = xrSyncActions(session, &sync);
  after_sync();
  static int64_t last;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  int64_t now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  if (now - last < 1000) return;
  last = now;
  LOG("state %d, xrSyncActions: %s", state, result(synced));
  dump_session();
  locate("view", view, local, time);
  if (stage) locate("stage", stage, local, time);
  for (int h = 0; h < 2; h++) {
    XrInteractionProfileState profile = {XR_TYPE_INTERACTION_PROFILE_STATE};
    XrResult r = xrGetCurrentInteractionProfile(session, hands[h], &profile);
    LOG(" %s: profile %s (%s)", h ? "right" : "left", path_text(profile.interactionProfile), result(r));
    XrActionStateGetInfo get = {XR_TYPE_ACTION_STATE_GET_INFO, NULL, grip, hands[h]};
    XrActionStatePose pose = {XR_TYPE_ACTION_STATE_POSE};
    xrGetActionStatePose(session, &get, &pose);
    XrActionStatePose aim_pose = {XR_TYPE_ACTION_STATE_POSE};
    get.action = aim;
    xrGetActionStatePose(session, &get, &aim_pose);
    XrActionStateFloat value = {XR_TYPE_ACTION_STATE_FLOAT};
    get.action = trigger;
    xrGetActionStateFloat(session, &get, &value);
    LOG("  grip active %d, aim active %d, trigger active %d value %.2f", pose.isActive, aim_pose.isActive, value.isActive, value.currentState);
    locate("grip", grip_space[h], local, time);
    locate("aim", aim_space[h], local, time);
  }
}

static void frame(void) {
  XrFrameState frame_state = {XR_TYPE_FRAME_STATE};
  XrFrameWaitInfo wait = {XR_TYPE_FRAME_WAIT_INFO};
  if (XR_FAILED(xrWaitFrame(session, &wait, &frame_state))) return;
  XrFrameBeginInfo begin = {XR_TYPE_FRAME_BEGIN_INFO};
  xrBeginFrame(session, &begin);
  report(frame_state.predictedDisplayTime);
  XrFrameEndInfo end = {XR_TYPE_FRAME_END_INFO, NULL, frame_state.predictedDisplayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0, NULL};
  XrResult r = xrEndFrame(session, &end);
  if (XR_FAILED(r)) ERR("xrEndFrame: %s", result(r));
}

void android_main(struct android_app *app) {
  LOG("starting");
  if (!loader(app) || !create_instance(app) || !create_session() || !create_actions()) {
    ERR("setup failed");
    ANativeActivity_finish(app->activity);
  }
  while (!app->destroyRequested) {
    int events;
    struct android_poll_source *source;
    while (ALooper_pollOnce(running ? 0 : 100, NULL, &events, (void **)&source) >= 0)
      if (source) source->process(app, source);
    if (!session) continue;
    poll_events();
    if (running) frame();
  }
  LOG("done");
}
