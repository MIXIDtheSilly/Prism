/*
 * xrdemo: the smallest OpenXR app that draws, for seeing an immersive app's frames go through
 * Horizon's runtime and compositor. Each frame it clears a swapchain per eye to a color, with a
 * grid of squares (vkCmdClearAttachments rectangles, no shaders), and submits them as a stereo
 * projection layer in LOCAL space. Holding either trigger turns the scene from blue to orange. It
 * logs (tag XrDemo) its session states, its swapchains and, once a second, the frames it submitted.
 *
 * Vulkan, as most Quest apps (and VrShell) use: the runtime's GLES swapchains import its buffers
 * through GL_EXT_memory_object_fd or EGL dma-buf import, which the emulator's GLES doesn't have.
 * Like xrprobe, it loads Meta's OpenXR loader (libopenxr_loader.so, from the image).
 */
#include <android/log.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define XR_NO_PROTOTYPES  // its functions come from the loader, by name
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define TAG "XrDemo"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define ERR(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static PFN_xrGetInstanceProcAddr get_proc;
static XrInstance instance;

#define XR_FUNCTIONS(F)                                                                                        \
  F(xrCreateInstance) F(xrGetSystem) F(xrCreateSession) F(xrBeginSession) F(xrEndSession) F(xrPollEvent)       \
  F(xrWaitFrame) F(xrBeginFrame) F(xrEndFrame) F(xrCreateReferenceSpace) F(xrStringToPath) F(xrResultToString)  \
  F(xrCreateActionSet) F(xrCreateAction) F(xrSuggestInteractionProfileBindings) F(xrAttachSessionActionSets)    \
  F(xrSyncActions) F(xrGetActionStateFloat) F(xrLocateViews) F(xrEnumerateViewConfigurationViews)              \
  F(xrEnumerateSwapchainFormats) F(xrCreateSwapchain) F(xrEnumerateSwapchainImages) F(xrAcquireSwapchainImage) \
  F(xrWaitSwapchainImage) F(xrReleaseSwapchainImage) F(xrGetVulkanGraphicsRequirements2KHR)                     \
  F(xrCreateVulkanInstanceKHR) F(xrGetVulkanGraphicsDevice2KHR) F(xrCreateVulkanDeviceKHR)

#define DECLARE(name) static PFN_##name name;
XR_FUNCTIONS(DECLARE)

static bool load_all(void) {
  bool ok = true;
#define LOAD(name)                                                                     \
  if (get_proc(instance, #name, (PFN_xrVoidFunction *)&name) != XR_SUCCESS || !name) { \
    ERR("no %s", #name);                                                               \
    ok = false;                                                                        \
  }
  XR_FUNCTIONS(LOAD)
  return ok;
}

static const char *result(XrResult r) {
  static char text[XR_MAX_RESULT_STRING_SIZE];
  if (!instance || !xrResultToString || xrResultToString(instance, r, text) != XR_SUCCESS) snprintf(text, sizeof text, "%d", r);
  return text;
}

#define CHECK(call)                     \
  do {                                  \
    XrResult r_ = (call);               \
    if (XR_FAILED(r_)) {                \
      ERR("%s: %s", #call, result(r_)); \
      return false;                     \
    }                                   \
  } while (0)

#define VK_CHECK(call)                \
  do {                                \
    VkResult v_ = (call);             \
    if (v_ != VK_SUCCESS) {           \
      ERR("%s: VkResult %d", #call, v_); \
      return false;                   \
    }                                 \
  } while (0)

static XrPath path(const char *text) {
  XrPath p = XR_NULL_PATH;
  if (xrStringToPath(instance, text, &p) != XR_SUCCESS) ERR("no path %s", text);
  return p;
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
    LOG("xrInitializeLoaderKHR: %d", initialize((const XrLoaderInitInfoBaseHeaderKHR *)&init));
  }
  return true;
}

static XrSystemId system_id;
static XrSession session;
static XrSessionState state = XR_SESSION_STATE_UNKNOWN;
static bool running;
static XrSpace local;
static XrActionSet set;
static XrAction trigger;
static XrPath hands[2];

static VkInstance vk_instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static uint32_t queue_family;
static VkRenderPass render_pass;
static VkCommandPool pool;
static VkCommandBuffer commands;
static VkFence fence;

#define MAX_IMAGES 8
static struct eye {
  XrSwapchain swapchain;
  int32_t width, height;
  uint32_t images;
  VkImageView views[MAX_IMAGES];
  VkFramebuffer framebuffers[MAX_IMAGES];
} eyes[2];

static bool create_instance(struct android_app *app) {
  const char *enabled[] = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
  XrInstanceCreateInfoAndroidKHR android = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR, NULL, app->activity->vm, app->activity->clazz};
  XrInstanceCreateInfo info = {XR_TYPE_INSTANCE_CREATE_INFO, &android};
  strcpy(info.applicationInfo.applicationName, "xrdemo");
  info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  info.enabledExtensionCount = sizeof enabled / sizeof *enabled;
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

// Vulkan through the runtime (XR_KHR_vulkan_enable2): it adds the instance and device extensions it
// needs, and picks the physical device.
static bool create_vulkan(void) {
  XrGraphicsRequirementsVulkan2KHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
  CHECK(xrGetVulkanGraphicsRequirements2KHR(instance, system_id, &requirements));
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "xrdemo", 1, NULL, 0, VK_API_VERSION_1_1};
  VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
  XrVulkanInstanceCreateInfoKHR xr_instance = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, NULL, system_id, 0,
                                               vkGetInstanceProcAddr, &instance_info, NULL};
  VkResult vk = VK_SUCCESS;
  CHECK(xrCreateVulkanInstanceKHR(instance, &xr_instance, &vk_instance, &vk));
  VK_CHECK(vk);
  XrVulkanGraphicsDeviceGetInfoKHR get = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, NULL, system_id, vk_instance};
  CHECK(xrGetVulkanGraphicsDevice2KHR(instance, &get, &physical));

  uint32_t families = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, NULL);
  VkQueueFamilyProperties props[16];
  if (families > 16) families = 16;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, props);
  for (queue_family = 0; queue_family < families && !(props[queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT); queue_family++) {
  }
  if (queue_family == families) {
    ERR("no graphics queue");
    return false;
  }
  float priority = 1;
  VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, queue_family, 1, &priority};
  VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &queue_info};
  XrVulkanDeviceCreateInfoKHR xr_device = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, NULL, system_id, 0, vkGetInstanceProcAddr,
                                           physical, &device_info, NULL};
  CHECK(xrCreateVulkanDeviceKHR(instance, &xr_device, &device, &vk));
  VK_CHECK(vk);
  vkGetDeviceQueue(device, queue_family, 0, &queue);

  VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                       VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, queue_family};
  VK_CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
  VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VK_CHECK(vkAllocateCommandBuffers(device, &alloc, &commands));
  VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VK_CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(physical, &properties);
  LOG("Vulkan device %s, queue family %u", properties.deviceName, queue_family);
  return true;
}

static bool create_session(void) {
  XrSystemGetInfo system = {XR_TYPE_SYSTEM_GET_INFO, NULL, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
  CHECK(xrGetSystem(instance, &system, &system_id));
  if (!create_vulkan()) return false;
  XrGraphicsBindingVulkan2KHR binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, NULL, vk_instance, physical, device, queue_family, 0};
  XrSessionCreateInfo info = {XR_TYPE_SESSION_CREATE_INFO, &binding, 0, system_id};
  CHECK(xrCreateSession(instance, &info, &session));
  XrReferenceSpaceCreateInfo space = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, NULL, XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {0, 0, 0}}};
  CHECK(xrCreateReferenceSpace(session, &space, &local));
  return true;
}

static bool create_swapchains(void) {
  XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
  uint32_t count = 0;
  CHECK(xrEnumerateViewConfigurationViews(instance, system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views));
  static int64_t formats[256];
  uint32_t format_count = 0;
  CHECK(xrEnumerateSwapchainFormats(session, 0, &format_count, NULL));
  if (format_count > 256) format_count = 256;
  CHECK(xrEnumerateSwapchainFormats(session, format_count, &format_count, formats));
  int64_t format = 0;
  for (uint32_t i = 0; i < format_count && !format; i++)
    if (formats[i] == VK_FORMAT_R8G8B8A8_SRGB || formats[i] == VK_FORMAT_R8G8B8A8_UNORM) format = formats[i];
  if (!format) format = formats[0];

  VkAttachmentDescription attachment = {0, (VkFormat)format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                        VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                        VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &reference};
  VkRenderPassCreateInfo pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &attachment, 1, &subpass};
  if (vkCreateRenderPass(device, &pass_info, NULL, &render_pass) != VK_SUCCESS) {
    ERR("vkCreateRenderPass failed");
    return false;
  }

  for (uint32_t e = 0; e < 2; e++) {
    struct eye *eye = &eyes[e];
    eye->width = (int32_t)views[e].recommendedImageRectWidth;
    eye->height = (int32_t)views[e].recommendedImageRectHeight;
    XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO, NULL, 0,
                                  XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT, format, 1,
                                  (uint32_t)eye->width, (uint32_t)eye->height, 1, 1, 1};
    CHECK(xrCreateSwapchain(session, &info, &eye->swapchain));
    XrSwapchainImageVulkan2KHR images[MAX_IMAGES];
    for (int i = 0; i < MAX_IMAGES; i++) images[i] = (XrSwapchainImageVulkan2KHR){XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR};
    CHECK(xrEnumerateSwapchainImages(eye->swapchain, MAX_IMAGES, &eye->images, (XrSwapchainImageBaseHeader *)images));
    for (uint32_t i = 0; i < eye->images; i++) {
      VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, images[i].image,
                                         VK_IMAGE_VIEW_TYPE_2D, (VkFormat)format, {0},
                                         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      VK_CHECK(vkCreateImageView(device, &view_info, NULL, &eye->views[i]));
      VkFramebufferCreateInfo fb_info = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, render_pass, 1, &eye->views[i],
                                         (uint32_t)eye->width, (uint32_t)eye->height, 1};
      VK_CHECK(vkCreateFramebuffer(device, &fb_info, NULL, &eye->framebuffers[i]));
    }
    LOG("eye %u: %dx%d, format %lld, %u images", e, eye->width, eye->height, (long long)format, eye->images);
  }
  return true;
}

static bool create_actions(void) {
  hands[0] = path("/user/hand/left");
  hands[1] = path("/user/hand/right");
  XrActionSetCreateInfo set_info = {XR_TYPE_ACTION_SET_CREATE_INFO};
  strcpy(set_info.actionSetName, "demo");
  strcpy(set_info.localizedActionSetName, "Demo");
  CHECK(xrCreateActionSet(instance, &set_info, &set));
  XrActionCreateInfo info = {XR_TYPE_ACTION_CREATE_INFO, NULL, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, 2, hands, "Trigger"};
  CHECK(xrCreateAction(set, &info, &trigger));
  static const char *const profiles[] = {"/interaction_profiles/meta/touch_controller_plus", "/interaction_profiles/oculus/touch_controller"};
  for (size_t p = 0; p < 2; p++) {
    XrActionSuggestedBinding bindings[] = {{trigger, path("/user/hand/left/input/trigger/value")},
                                           {trigger, path("/user/hand/right/input/trigger/value")}};
    XrInteractionProfileSuggestedBinding suggested = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, NULL, path(profiles[p]), 2, bindings};
    LOG("suggested %s: %s", profiles[p], result(xrSuggestInteractionProfileBindings(instance, &suggested)));
  }
  XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, NULL, 1, &set};
  CHECK(xrAttachSessionActionSets(session, &attach));
  return true;
}

static void poll_events(void) {
  XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(instance, &event) == XR_SUCCESS) {
    if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
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
    } else {
      LOG("event %d", event.type);
    }
    event = (XrEventDataBuffer){XR_TYPE_EVENT_DATA_BUFFER};
  }
}

static float trigger_value(void) {
  XrActiveActionSet active = {set, XR_NULL_PATH};
  XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO, NULL, 1, &active};
  if (state != XR_SESSION_STATE_FOCUSED || XR_FAILED(xrSyncActions(session, &sync))) return 0;
  float value = 0;
  for (int h = 0; h < 2; h++) {
    XrActionStateGetInfo get = {XR_TYPE_ACTION_STATE_GET_INFO, NULL, trigger, hands[h]};
    XrActionStateFloat s = {XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_SUCCEEDED(xrGetActionStateFloat(session, &get, &s)) && s.isActive && s.currentState > value) value = s.currentState;
  }
  return value;
}

// One eye's image: a background, and a grid of squares in two colors whose phase follows the frame
// count, so a running app is visibly moving.
#define MAX_RECTS 512
static void record(const struct eye *eye, VkFramebuffer framebuffer, bool pressed, uint64_t frame) {
  VkClearValue background = {.color = {.float32 = {pressed ? 0.55f : 0.05f, pressed ? 0.25f : 0.12f, pressed ? 0.05f : 0.35f, 1}}};
  VkRenderPassBeginInfo begin = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, render_pass, framebuffer,
                                 {{0, 0}, {(uint32_t)eye->width, (uint32_t)eye->height}}, 1, &background};
  vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
  static VkClearRect rects[2][MAX_RECTS];
  uint32_t counts[2] = {0, 0};
  int cell = eye->width / 12, size = cell / 2, shift = (int)(frame % (uint64_t)cell);
  for (int y = 0; y + cell <= eye->height; y += cell)
    for (int x = 0; x + cell <= eye->width; x += cell) {
      int odd = (x / cell + y / cell) & 1;
      int left = x + (cell - size) / 2 + shift - cell / 2;
      if (left < 0 || left + size > eye->width || counts[odd] == MAX_RECTS) continue;
      rects[odd][counts[odd]++] = (VkClearRect){{{left, y + (cell - size) / 2}, {(uint32_t)size, (uint32_t)size}}, 0, 1};
    }
  VkClearAttachment colors[2] = {
      {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {.float32 = {pressed ? 1.0f : 0.3f, pressed ? 0.7f : 0.8f, pressed ? 0.2f : 1.0f, 1}}}},
      {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {.float32 = {0.9f, 0.9f, 0.9f, 1}}}}};
  for (int c = 0; c < 2; c++)
    if (counts[c]) vkCmdClearAttachments(commands, 1, &colors[c], counts[c], rects[c]);
  vkCmdEndRenderPass(commands);
}

static bool draw(const struct eye *eye, uint32_t index, bool pressed, uint64_t frame) {
  VK_CHECK(vkResetCommandBuffer(commands, 0));
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  VK_CHECK(vkBeginCommandBuffer(commands, &begin));
  record(eye, eye->framebuffers[index], pressed, frame);
  VK_CHECK(vkEndCommandBuffer(commands));
  VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &commands};
  VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
  VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
  VK_CHECK(vkResetFences(device, 1, &fence));
  return true;
}

static uint64_t frames, failures;

static void frame(void) {
  XrFrameState frame_state = {XR_TYPE_FRAME_STATE};
  XrFrameWaitInfo wait = {XR_TYPE_FRAME_WAIT_INFO};
  if (XR_FAILED(xrWaitFrame(session, &wait, &frame_state))) return;
  XrFrameBeginInfo begin = {XR_TYPE_FRAME_BEGIN_INFO};
  xrBeginFrame(session, &begin);
  bool pressed = trigger_value() > 0.5f;

  XrViewLocateInfo locate = {XR_TYPE_VIEW_LOCATE_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                             frame_state.predictedDisplayTime, local};
  XrViewState view_state = {XR_TYPE_VIEW_STATE};
  XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  uint32_t view_count = 0;
  XrCompositionLayerProjectionView projection_views[2];
  XrCompositionLayerProjection projection = {XR_TYPE_COMPOSITION_LAYER_PROJECTION, NULL, 0, local, 2, projection_views};
  const XrCompositionLayerBaseHeader *layers[] = {(const XrCompositionLayerBaseHeader *)&projection};
  bool render = frame_state.shouldRender &&
                XR_SUCCEEDED(xrLocateViews(session, &locate, &view_state, 2, &view_count, views)) && view_count == 2;
  for (uint32_t e = 0; render && e < 2; e++) {
    struct eye *eye = &eyes[e];
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo image_wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, NULL, XR_INFINITE_DURATION};
    if (XR_FAILED(xrAcquireSwapchainImage(eye->swapchain, &acquire, &index)) ||
        XR_FAILED(xrWaitSwapchainImage(eye->swapchain, &image_wait))) {
      render = false;
      break;
    }
    bool drawn = draw(eye, index, pressed, frames);
    XrSwapchainImageReleaseInfo release = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(eye->swapchain, &release);
    render &= drawn;
    projection_views[e] = (XrCompositionLayerProjectionView){
        XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, NULL, views[e].pose, views[e].fov,
        {eye->swapchain, {{0, 0}, {eye->width, eye->height}}, 0}};
  }
  XrFrameEndInfo end = {XR_TYPE_FRAME_END_INFO, NULL, frame_state.predictedDisplayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                        render ? 1u : 0u, layers};
  XrResult r = xrEndFrame(session, &end);
  frames += render;
  if (XR_FAILED(r) && failures++ < 5) ERR("xrEndFrame: %s", result(r));

  static int64_t last;
  static uint64_t last_frames;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  int64_t now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  if (now - last >= 1000) {
    LOG("state %d: %llu frames this second, %llu in all, trigger %s", state, (unsigned long long)(frames - last_frames),
        (unsigned long long)frames, pressed ? "held" : "up");
    last = now;
    last_frames = frames;
  }
}

void android_main(struct android_app *app) {
  LOG("starting");
  if (!loader(app) || !create_instance(app) || !create_session() || !create_swapchains() || !create_actions()) {
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
