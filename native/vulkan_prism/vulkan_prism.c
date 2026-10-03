// Prism's Vulkan driver (vulkan.prism.so, an x86_64 HAL in /vendor/lib64/hw): the emulator's own
// driver (gfxstream's vulkan.<ro.boot.hardware.vulkan>.so) with what Horizon's compositor needs on
// top.
//
// Meta's CompositorServer requires VK_KHR_external_memory_fd: it hands its clients swapchain memory
// as file descriptors (vkGetMemoryFdKHR). gfxstream implements external memory on Android only as
// AHardwareBuffers. Prism advertises the extension and backs opaque fds with AHardwareBuffers:
//
//   * images, buffers and allocations asking for OPAQUE_FD handles get AHARDWAREBUFFER ones;
//   * vkGetMemoryFdKHR returns one end of a Unix socket with the memory's AHardwareBuffer queued in
//     it (AHardwareBuffer_sendHandleToUnixSocket); the fd crosses processes like any other;
//   * importing such an fd receives the AHardwareBuffer from it and imports that.
//
// Importing transfers an fd's ownership, but a dup of an opaque fd names the same memory, and Meta's
// compositor imports its own swapchain memory and hands a dup of the same fd to its client. So the
// buffer is queued in the socket EXPORT_COPIES times, one for each import; copies nobody reads go
// when the socket's last fd is closed.
// gralloc here makes only single-layer buffers in a few formats, and the host takes only images of
// those for them. An sRGB image is its UNORM twin, created mutable so its views keep their sRGB
// format. Any other image (multiview swapchains: two layers) is backed: each process has its own
// copy, and they share a single-layer mirror of it, which Prism copies into after work that writes
// the image and out of before work that reads it. Prism follows what command buffers do to such
// images (render passes, barriers, transfers, bound descriptor sets) to know when.
//
// gfxstream also leaves out the names of some extensions that later Vulkan versions made core
// (VK_KHR_16bit_storage, ...), which Meta's apps still ask for by name. Prism lists those the
// device's version covers (kPromoted), and drops them again when a device is created.
// Dispatchable handles are the driver's own, so the loader's dispatch works as it does with gfxstream
// alone; Prism only intercepts functions by name.

#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define TAG "PrismVulkan"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// hardware/libhardware's hardware.h and hwvulkan.h (not in the NDK).
typedef struct hw_module_t hw_module_t;
typedef struct hw_device_t hw_device_t;
typedef struct {
  int (*open)(const hw_module_t *module, const char *id, hw_device_t **device);
} hw_module_methods_t;
struct hw_module_t {
  uint32_t tag;
  uint16_t module_api_version;
  uint16_t hal_api_version;
  const char *id;
  const char *name;
  const char *author;
  hw_module_methods_t *methods;
  void *dso;
  uint64_t reserved[32 - 7];
};
struct hw_device_t {
  uint32_t tag;
  uint32_t version;
  hw_module_t *module;
  uint64_t reserved[12];
  int (*close)(hw_device_t *device);
};
typedef struct {
  hw_module_t common;
} hwvulkan_module_t;
typedef struct {
  hw_device_t common;
  PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
  PFN_vkCreateInstance CreateInstance;
  PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} hwvulkan_device_t;
#define HW_TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))
#define HWVULKAN_DEVICE_0 "vk0"

#define OPAQUE_FD VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
#define AHB VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID
static const char kMemoryFd[] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
static const char kAhb[] = VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME;
static const char kForeign[] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;

// The driver underneath. Its entry points are the same for every instance and device, so each is
// recorded when the loader first asks for it.
static struct {
  PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
  PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
  PFN_vkCreateDevice CreateDevice;
  PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
  PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2;
  PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2;
  PFN_vkGetPhysicalDeviceImageFormatProperties2KHR GetPhysicalDeviceImageFormatProperties2KHR;
  PFN_vkGetPhysicalDeviceExternalBufferProperties GetPhysicalDeviceExternalBufferProperties;
  PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR GetPhysicalDeviceExternalBufferPropertiesKHR;
  PFN_vkCreateImage CreateImage;
  PFN_vkDestroyImage DestroyImage;
  PFN_vkFreeMemory FreeMemory;
  PFN_vkCreateBuffer CreateBuffer;
  PFN_vkAllocateMemory AllocateMemory;
  PFN_vkBindImageMemory BindImageMemory;
  PFN_vkBindImageMemory2 BindImageMemory2;
  PFN_vkBindImageMemory2KHR BindImageMemory2KHR;
  PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
  PFN_vkGetMemoryAndroidHardwareBufferANDROID GetMemoryAndroidHardwareBufferANDROID;
  PFN_vkGetAndroidHardwareBufferPropertiesANDROID GetAndroidHardwareBufferPropertiesANDROID;
  // What backed images' copies need: their use is tracked, and Prism records and submits copies.
  PFN_vkCreateImageView CreateImageView;
  PFN_vkDestroyImageView DestroyImageView;
  PFN_vkCreateRenderPass CreateRenderPass;
  PFN_vkCreateRenderPass2 CreateRenderPass2;
  PFN_vkCreateRenderPass2KHR CreateRenderPass2KHR;
  PFN_vkDestroyRenderPass DestroyRenderPass;
  PFN_vkCreateFramebuffer CreateFramebuffer;
  PFN_vkDestroyFramebuffer DestroyFramebuffer;
  PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
  PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
  PFN_vkFreeDescriptorSets FreeDescriptorSets;
  PFN_vkResetDescriptorPool ResetDescriptorPool;
  PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
  PFN_vkGetDeviceQueue GetDeviceQueue;
  PFN_vkGetDeviceQueue2 GetDeviceQueue2;
  PFN_vkBeginCommandBuffer BeginCommandBuffer;
  PFN_vkEndCommandBuffer EndCommandBuffer;
  PFN_vkResetCommandBuffer ResetCommandBuffer;
  PFN_vkFreeCommandBuffers FreeCommandBuffers;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
  PFN_vkCmdPipelineBarrier2 CmdPipelineBarrier2;
  PFN_vkCmdPipelineBarrier2KHR CmdPipelineBarrier2KHR;
  PFN_vkCmdWaitEvents CmdWaitEvents;
  PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
  PFN_vkCmdBeginRenderPass2 CmdBeginRenderPass2;
  PFN_vkCmdBeginRenderPass2KHR CmdBeginRenderPass2KHR;
  PFN_vkCmdClearColorImage CmdClearColorImage;
  PFN_vkCmdCopyImage CmdCopyImage;
  PFN_vkCmdBlitImage CmdBlitImage;
  PFN_vkCmdResolveImage CmdResolveImage;
  PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
  PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
  PFN_vkCmdExecuteCommands CmdExecuteCommands;
  PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
  PFN_vkQueueSubmit QueueSubmit;
  PFN_vkCreateCommandPool CreateCommandPool;
  PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
  PFN_vkCreateFence CreateFence;
  PFN_vkResetFences ResetFences;
  PFN_vkGetFenceStatus GetFenceStatus;
  PFN_vkWaitForFences WaitForFences;
  // debug.prism.vk.dump
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
  PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
  PFN_vkBindBufferMemory BindBufferMemory;
  PFN_vkMapMemory MapMemory;
  PFN_vkDestroyBuffer DestroyBuffer;
  PFN_vkVoidFunction AcquireImageANDROID, QueueSignalReleaseImageANDROID;
  // debug.prism.vk.trace
  PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
  PFN_vkCmdBindPipeline CmdBindPipeline;
  PFN_vkCmdSetViewport CmdSetViewport;
  PFN_vkCmdSetScissor CmdSetScissor;
  PFN_vkCmdDraw CmdDraw;
  PFN_vkCmdDrawIndexed CmdDrawIndexed;
  PFN_vkCmdClearAttachments CmdClearAttachments;
  PFN_vkCmdEndRenderPass CmdEndRenderPass;
  PFN_vkCmdEndRenderPass2 CmdEndRenderPass2;
  PFN_vkCmdEndRenderPass2KHR CmdEndRenderPass2KHR;
} next;

// debug.prism.vk.trace=N: when the value changes, the next N lines of commands are logged: render
// passes, pipelines bound, viewports, scissors, draws, clears, submits and presents. Framebuffers,
// pipelines and swapchain images are logged as they're made (a bounded number of each).
static int g_trace;  // lines left
static int g_trace_images;  // debug.prism.vk.images: log every image and view made (bounded)
#define TRACE(...) \
  do { \
    if (__atomic_load_n(&g_trace, __ATOMIC_RELAXED) > 0 && __atomic_fetch_sub(&g_trace, 1, __ATOMIC_RELAXED) > 0) \
      LOGI("trace: " __VA_ARGS__); \
  } while (0)
// Whether *counter, counting up, is still below limit.
static int within(unsigned *counter, unsigned limit) { return __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED) < limit; }

// VK_ANDROID_native_buffer (vulkan/vk_android_native_buffer.h): what the loader's swapchain calls.
typedef VkResult(VKAPI_PTR *PFN_AcquireImageANDROID)(VkDevice, VkImage, int, VkSemaphore, VkFence);
typedef VkResult(VKAPI_PTR *PFN_QueueSignalReleaseImageANDROID)(VkQueue, uint32_t, const VkSemaphore *, VkImage, int *);
static unsigned g_acquires, g_releases;  // swapchain images acquired and presented

// debug.prism.vk.dump=N: every Nth copy into a mirror, and out of one, also copies the image to
// /data/local/tmp/prism_<into|out>_<pid>.rgba (raw texels, width x height x layers, the layers one
// under another). Read once a second (see trace_submit).
static unsigned g_dump_at;
static unsigned g_submits, g_pulls, g_pushes;  // vkQueueSubmit calls, copies out of mirrors and into them
static VkPhysicalDeviceMemoryProperties g_memory_properties;
static struct {
  VkDevice device;
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkExtent3D extent;
  int push;
} g_dump;

// --- pNext chains ------------------------------------------------------------------------------

static const void *find_in(const void *s, VkStructureType type) {
  for (const VkBaseInStructure *n = ((const VkBaseInStructure *)s)->pNext; n; n = n->pNext)
    if (n->sType == type) return n;
  return NULL;
}

static void *find_out(void *s, VkStructureType type) {
  for (VkBaseOutStructure *n = ((VkBaseOutStructure *)s)->pNext; n; n = n->pNext)
    if (n->sType == type) return n;
  return NULL;
}

// Sizes of the structures that can come before the one Prism replaces, so they can be copied.
static size_t node_size(VkStructureType type) {
  switch (type) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO: return sizeof(VkPhysicalDeviceExternalImageFormatInfo);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT: return sizeof(VkPhysicalDeviceImageDrmFormatModifierInfoEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_IMAGE_FORMAT_INFO_EXT: return sizeof(VkPhysicalDeviceImageViewImageFormatInfoEXT);
    case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: return sizeof(VkImageFormatListCreateInfo);
    case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO: return sizeof(VkImageStencilUsageCreateInfo);
    case VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR: return sizeof(VkImageSwapchainCreateInfoKHR);
    case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO: return sizeof(VkExternalMemoryImageCreateInfo);
    case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO: return sizeof(VkExternalMemoryBufferCreateInfo);
    case VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID: return sizeof(VkExternalFormatANDROID);
    case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO: return sizeof(VkBufferOpaqueCaptureAddressCreateInfo);
    case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO: return sizeof(VkMemoryDedicatedAllocateInfo);
    case VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO: return sizeof(VkMemoryAllocateFlagsInfo);
    case VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT: return sizeof(VkMemoryPriorityAllocateInfoEXT);
    case VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO: return sizeof(VkMemoryOpaqueCaptureAddressAllocateInfo);
    case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO: return sizeof(VkExportMemoryAllocateInfo);
    case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR: return sizeof(VkImportMemoryFdInfoKHR);
    case VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID: return sizeof(VkImportAndroidHardwareBufferInfoANDROID);
    default: return 0;
  }
}

typedef struct {
  _Alignas(16) unsigned char bytes[1024];
  size_t used;
} Scratch;

// In head (a copy of the caller's top-level structure), puts replacement in place of the first
// structure of its type. The caller's structures are never written: those before it are copied
// into scratch. If one of those has a size Prism doesn't know, replacement goes ahead of the
// caller's chain instead; gfxstream reads the first structure of each type.
static void replace_in_chain(void *head, void *replacement, Scratch *scratch) {
  VkBaseOutStructure *h = head, *r = replacement, *prev = h;
  VkBaseOutStructure *original = h->pNext;
  for (const VkBaseInStructure *n = (const VkBaseInStructure *)h->pNext; n; n = n->pNext) {
    if (n->sType == r->sType) {
      r->pNext = (VkBaseOutStructure *)n->pNext;
      prev->pNext = r;
      return;
    }
    size_t size = node_size(n->sType);
    if (!size || scratch->used + size > sizeof scratch->bytes) break;
    VkBaseOutStructure *copy = (VkBaseOutStructure *)(scratch->bytes + scratch->used);
    memcpy(copy, n, size);
    scratch->used += (size + 15) & ~(size_t)15;
    prev->pNext = copy;
    prev = copy;
  }
  r->pNext = original;
  h->pNext = r;
}

static VkExternalMemoryHandleTypeFlags as_ahb(VkExternalMemoryHandleTypeFlags types) {
  return types & OPAQUE_FD ? (types & ~OPAQUE_FD) | AHB : types;
}

// What the driver reports for AHardwareBuffers, reported for opaque fds.
static void as_opaque_fd(VkExternalMemoryProperties *p) {
  if (p->compatibleHandleTypes & AHB) p->compatibleHandleTypes = OPAQUE_FD;
  if (p->exportFromImportedHandleTypes & AHB) p->exportFromImportedHandleTypes = OPAQUE_FD;
}

static int has_extension(const VkExtensionProperties *props, uint32_t count, const char *name) {
  for (uint32_t i = 0; i < count; i++)
    if (!strcmp(props[i].extensionName, name)) return 1;
  return 0;
}

// --- physical device -----------------------------------------------------------------------------

// Extensions promoted to core: listed when the device's version has them (and the feature they
// exist for, when they have one).
static int has_16bit_storage(const VkPhysicalDeviceVulkan11Features *f) { return f->storageBuffer16BitAccess; }
static int has_multiview(const VkPhysicalDeviceVulkan11Features *f) { return f->multiview; }
static const struct {
  const char *name;
  uint32_t spec_version;
  uint32_t core;
  int (*feature)(const VkPhysicalDeviceVulkan11Features *);
} kPromoted[] = {
    {VK_KHR_16BIT_STORAGE_EXTENSION_NAME, VK_KHR_16BIT_STORAGE_SPEC_VERSION, VK_API_VERSION_1_1, has_16bit_storage},
    {VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME, VK_KHR_DEPTH_STENCIL_RESOLVE_SPEC_VERSION, VK_API_VERSION_1_2, NULL},
    {VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_KHR_MULTIVIEW_SPEC_VERSION, VK_API_VERSION_1_1, has_multiview},
    {VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME, VK_KHR_DRIVER_PROPERTIES_SPEC_VERSION, VK_API_VERSION_1_2, NULL},
};
#define PROMOTED_COUNT (sizeof kPromoted / sizeof *kPromoted)

static int promoted_index(const char *name) {
  for (size_t i = 0; i < PROMOTED_COUNT; i++)
    if (!strcmp(name, kPromoted[i].name)) return (int)i;
  return -1;
}

static int promoted_supported(VkPhysicalDevice pd, size_t i) {
  if (!next.GetPhysicalDeviceProperties) return 0;
  VkPhysicalDeviceProperties props;
  next.GetPhysicalDeviceProperties(pd, &props);
  if (VK_API_VERSION_MAJOR(props.apiVersion) == 1 && props.apiVersion < kPromoted[i].core) return 0;
  if (!kPromoted[i].feature) return 1;
  if (!next.GetPhysicalDeviceFeatures2) return 0;
  VkPhysicalDeviceVulkan11Features v11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11};
  next.GetPhysicalDeviceFeatures2(pd, &features);
  return kPromoted[i].feature(&v11);
}

static VkResult VKAPI_CALL prism_EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer,
                                                                    uint32_t *count, VkExtensionProperties *props) {
  if (layer) return next.EnumerateDeviceExtensionProperties(pd, layer, count, props);
  uint32_t n = 0;
  VkResult result = next.EnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
  if (result != VK_SUCCESS) return result;
  VkExtensionProperties *all = calloc(n + 1 + PROMOTED_COUNT, sizeof *all);
  if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
  result = next.EnumerateDeviceExtensionProperties(pd, NULL, &n, all);
  if (result < 0) {
    free(all);
    return result;
  }
  if (has_extension(all, n, kAhb) && !has_extension(all, n, kMemoryFd)) {
    strcpy(all[n].extensionName, kMemoryFd);
    all[n++].specVersion = VK_KHR_EXTERNAL_MEMORY_FD_SPEC_VERSION;
  }
  for (size_t i = 0; i < PROMOTED_COUNT; i++) {
    if (has_extension(all, n, kPromoted[i].name) || !promoted_supported(pd, i)) continue;
    strcpy(all[n].extensionName, kPromoted[i].name);
    all[n++].specVersion = kPromoted[i].spec_version;
  }
  if (!props) {
    *count = n;
    free(all);
    return VK_SUCCESS;
  }
  uint32_t copied = *count < n ? *count : n;
  memcpy(props, all, copied * sizeof *all);
  *count = copied;
  free(all);
  return copied < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult image_format_properties2(PFN_vkGetPhysicalDeviceImageFormatProperties2 down, VkPhysicalDevice pd,
                                         const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *props) {
  const VkPhysicalDeviceExternalImageFormatInfo *external =
      find_in(info, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
  if (!external || external->handleType != OPAQUE_FD) return down(pd, info, props);
  VkPhysicalDeviceImageFormatInfo2 copy = *info;
  VkPhysicalDeviceExternalImageFormatInfo ahb = *external;
  ahb.handleType = AHB;
  Scratch scratch = {.used = 0};
  replace_in_chain(&copy, &ahb, &scratch);
  VkResult result = down(pd, &copy, props);
  VkExternalImageFormatProperties *out = find_out(props, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
  if (out) as_opaque_fd(&out->externalMemoryProperties);
  return result;
}

static VkResult VKAPI_CALL prism_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd,
                                                                         const VkPhysicalDeviceImageFormatInfo2 *info,
                                                                         VkImageFormatProperties2 *props) {
  return image_format_properties2(next.GetPhysicalDeviceImageFormatProperties2, pd, info, props);
}

static VkResult VKAPI_CALL prism_GetPhysicalDeviceImageFormatProperties2KHR(VkPhysicalDevice pd,
                                                                            const VkPhysicalDeviceImageFormatInfo2 *info,
                                                                            VkImageFormatProperties2 *props) {
  return image_format_properties2(next.GetPhysicalDeviceImageFormatProperties2KHR, pd, info, props);
}

static void external_buffer_properties(PFN_vkGetPhysicalDeviceExternalBufferProperties down, VkPhysicalDevice pd,
                                       const VkPhysicalDeviceExternalBufferInfo *info, VkExternalBufferProperties *props) {
  if (info->handleType != OPAQUE_FD) {
    down(pd, info, props);
    return;
  }
  VkPhysicalDeviceExternalBufferInfo copy = *info;
  copy.handleType = AHB;
  down(pd, &copy, props);
  as_opaque_fd(&props->externalMemoryProperties);
}

static void VKAPI_CALL prism_GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice pd,
                                                                       const VkPhysicalDeviceExternalBufferInfo *info,
                                                                       VkExternalBufferProperties *props) {
  external_buffer_properties(next.GetPhysicalDeviceExternalBufferProperties, pd, info, props);
}

static void VKAPI_CALL prism_GetPhysicalDeviceExternalBufferPropertiesKHR(VkPhysicalDevice pd,
                                                                          const VkPhysicalDeviceExternalBufferInfo *info,
                                                                          VkExternalBufferProperties *props) {
  external_buffer_properties(next.GetPhysicalDeviceExternalBufferPropertiesKHR, pd, info, props);
}

// The opaque-fd extension isn't the driver's: it is dropped, and the AHardwareBuffer extension
// that implements it is enabled instead.
static VkResult VKAPI_CALL prism_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *info,
                                              const VkAllocationCallbacks *allocator, VkDevice *device) {
  const char *names[info->enabledExtensionCount + 2];
  uint32_t n = 0;
  int memory_fd = 0, ahb = 0, foreign = 0;
  for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
    const char *name = info->ppEnabledExtensionNames[i];
    if (!strcmp(name, kMemoryFd)) {
      memory_fd = 1;
      continue;
    }
    if (promoted_index(name) >= 0) continue;  // core in this device's version
    ahb |= !strcmp(name, kAhb);
    foreign |= !strcmp(name, kForeign);
    names[n++] = name;
  }
  if (memory_fd && !ahb) names[n++] = kAhb;
  if (memory_fd && !foreign) names[n++] = kForeign;
  VkDeviceCreateInfo copy = *info;
  copy.enabledExtensionCount = n;
  copy.ppEnabledExtensionNames = names;
  VkResult result = next.CreateDevice(pd, &copy, allocator, device);
  if (result == VK_SUCCESS) {
    PFN_vkGetDeviceProcAddr gdpa = next.GetDeviceProcAddr
        ? next.GetDeviceProcAddr
        : (PFN_vkGetDeviceProcAddr)next.GetInstanceProcAddr(NULL, "vkGetDeviceProcAddr");
    next.GetMemoryAndroidHardwareBufferANDROID = (PFN_vkGetMemoryAndroidHardwareBufferANDROID)gdpa(
        *device, "vkGetMemoryAndroidHardwareBufferANDROID");
    next.GetAndroidHardwareBufferPropertiesANDROID = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)gdpa(
        *device, "vkGetAndroidHardwareBufferPropertiesANDROID");
    // Prism calls these itself, whether or not the loader has asked for them yet.
#define NEED(name) \
  if (!next.name) next.name = (PFN_vk##name)gdpa(*device, "vk" #name)
    NEED(CreateImage);
    NEED(DestroyImage);
    NEED(FreeMemory);
    NEED(AllocateMemory);
    NEED(BindImageMemory);
    NEED(GetImageMemoryRequirements);
    NEED(BeginCommandBuffer);
    NEED(EndCommandBuffer);
    NEED(CmdPipelineBarrier);
    NEED(CmdCopyImage);
    NEED(QueueSubmit);
    NEED(CreateCommandPool);
    NEED(AllocateCommandBuffers);
    NEED(CreateFence);
    NEED(ResetFences);
    NEED(GetFenceStatus);
    NEED(WaitForFences);
    NEED(CreateBuffer);
    NEED(CmdCopyImageToBuffer);
    NEED(GetBufferMemoryRequirements);
    NEED(BindBufferMemory);
    NEED(MapMemory);
    NEED(DestroyBuffer);
    if (next.GetPhysicalDeviceMemoryProperties) next.GetPhysicalDeviceMemoryProperties(pd, &g_memory_properties);
    char images[PROP_VALUE_MAX] = "";
    __system_property_get("debug.prism.vk.images", images);
    g_trace_images = atoi(images);
#undef NEED
    if (memory_fd) LOGI("device %p: %s backed by AHardwareBuffers", (void *)*device, kMemoryFd);
    const VkPhysicalDeviceVulkan11Features *v11 = find_in(info, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    const VkPhysicalDeviceMultiviewFeatures *mv = find_in(info, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES);
    LOGI("device %p: %u extensions, multiview %s", (void *)*device, info->enabledExtensionCount,
         v11 ? (v11->multiview ? "on (1.1 features)" : "off (1.1 features)")
         : mv ? (mv->multiview ? "on" : "off")
              : "not asked");
  }
  return result;
}

// --- backed images -------------------------------------------------------------------------------

// Opaque-fd images no AHardwareBuffer can hold: more than one layer (multiview swapchains; gralloc
// here has no layered buffers), or a format without an AHardwareBuffer one. The host aliases a color
// buffer's memory only for an image of the buffer's own shape, so such an image can't share memory.
// Each process keeps its own copy of it, in memory only it uses, and shares a mirror instead: a
// single-layer buffer as wide as the image and as tall as its layers stacked, what the image's fd
// carries. A batch of work that writes the image ends with a copy of it into the mirror, and one
// that uses it otherwise begins with a copy out of the mirror (see prism_QueueSubmit). So a client's
// eye buffers reach the mirror with the frame it submits, and the compositor's copy is refreshed
// when it next samples them.
#define MAX_BACKED 256
#define MAX_LAYERS 8
#define NO_LAYOUT VK_IMAGE_LAYOUT_MAX_ENUM
typedef struct {
  VkImage image;  // the app's; VK_NULL_HANDLE once destroyed
  VkDevice device;
  VkExtent3D extent;
  uint32_t layers;
  VkFormat format;
  VkFormat mirror_format;  // VK_FORMAT_UNDEFINED: no mirror holds it, its contents aren't shared
  VkImageLayout layout;    // its layout once the work submitted so far is done
  VkDeviceMemory memory;   // the app's memory for it; VK_NULL_HANDLE until allocated and once freed
  VkDeviceMemory own;      // memory it's bound to instead, when the app's is the mirror's (imported)
  AHardwareBuffer *buffer;  // what that memory exports: the mirror's
  VkImage mirror;
  VkDeviceMemory mirror_memory;
  int mirror_general;  // the mirror is in VK_IMAGE_LAYOUT_GENERAL
  void *owner;         // another backed image whose memory this one is bound to: that one's mirror is this one's
  int logged;          // copies of it logged: 1 into the mirror, 2 out of it
} Backed;
static Backed g_backed[MAX_BACKED];
static int g_backed_count;  // read without the lock: with no backed images there's nothing to track
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;  // everything below

static int tracking(void) { return __atomic_load_n(&g_backed_count, __ATOMIC_RELAXED) > 0; }

static Backed *backed_image(VkImage image) {
  for (int i = 0; image != VK_NULL_HANDLE && i < MAX_BACKED; i++)
    if (g_backed[i].image == image) return &g_backed[i];
  return NULL;
}

static Backed *backed_memory(VkDeviceMemory memory) {
  for (int i = 0; memory != VK_NULL_HANDLE && i < MAX_BACKED; i++)
    if (g_backed[i].memory == memory) return &g_backed[i];
  return NULL;
}

// Where b's mirror is: b, or the image whose memory b is bound to.
static Backed *mirror_of(Backed *b) { return b->owner ? (Backed *)b->owner : b; }

// An image with a mirror, whose use is tracked.
static Backed *mirrored(VkImage image) {
  Backed *b = backed_image(image);
  return b && mirror_of(b)->mirror != VK_NULL_HANDLE ? b : NULL;
}

static void release_backed(Backed *b) {
  if (b->image != VK_NULL_HANDLE || b->memory != VK_NULL_HANDLE) return;
  memset(b, 0, sizeof *b);
  __atomic_store_n(&g_backed_count, g_backed_count - 1, __ATOMIC_RELAXED);
}

static int grow(void **items, uint32_t *capacity, size_t size) {
  uint32_t n = *capacity ? *capacity * 2 : 16;
  void *p = realloc(*items, n * size);
  if (!p) return 0;
  *items = p;
  *capacity = n;
  return 1;
}
#define ROOM(items, count, capacity) \
  ((count) < (capacity) || grow((void **)&(items), &(capacity), sizeof *(items)))

// The formats with an AHardwareBuffer equivalent (AHardwareBuffer_Format).
static int ahb_format(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8_UNORM:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_S8_UINT:
      return 1;
    default:
      return 0;
  }
}

// sRGB formats whose UNORM twin has an AHardwareBuffer format (VK_FORMAT_UNDEFINED for others).
static VkFormat unorm_twin(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SRGB: return VK_FORMAT_R8G8B8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
  }
}

// A mirror's format: one with an AHardwareBuffer format, in the same copy class as the image's
// (texels of the same size), so vkCmdCopyImage moves the bits as they are.
static VkFormat mirror_format(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R32G32_SFLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

#define MAX_VIEW_FORMATS 16

#define NATIVE_BUFFER_ANDROID ((VkStructureType)1000010000)  // VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID
#define MAX_SWAPCHAIN 16
static VkImage g_swapchain[MAX_SWAPCHAIN];  // images the loader's swapchain made
static unsigned g_swapchain_count;

// --- the display ---------------------------------------------------------------------------------

// Horizon's compositor draws for its headset's panel, which on the emulator is larger than the
// swapchain images it renders to: its viewport for a 1920x1080 window is 3840x2160, 1080 rows above
// the image, so most of each eye falls outside it. Prism is the display, so in a pass into a
// swapchain image a viewport larger than the render area is taken for the panel, and the panel is
// mapped onto the render area: that viewport and the scissors after it, scaled and moved alike.
static VkImageView g_swapchain_views[MAX_SWAPCHAIN];
static unsigned g_swapchain_view_count;
static VkFramebuffer g_screens[MAX_SWAPCHAIN];  // framebuffers of swapchain images
static unsigned g_screen_count;

#define MAX_PANELS 32
typedef struct {
  VkCommandBuffer cb;  // in a pass into a swapchain image
  VkRect2D area;       // the pass's render area
  VkViewport panel;    // the panel's viewport, if fit
  int fit;
} Panel;
static Panel g_panels[MAX_PANELS];
static unsigned g_panel_next;

static int is_screen(VkFramebuffer framebuffer) {
  for (unsigned i = 0; framebuffer != VK_NULL_HANDLE && i < MAX_SWAPCHAIN; i++)
    if (g_screens[i] == framebuffer) return 1;
  return 0;
}

// The panel cb draws in, if it's in a pass into a swapchain image. Called with g_lock held.
static Panel *panel_of(VkCommandBuffer cb) {
  for (unsigned i = 0; i < MAX_PANELS; i++)
    if (g_panels[i].cb == cb) return &g_panels[i];
  return NULL;
}

static void panel_begin(VkCommandBuffer cb, const VkRenderPassBeginInfo *begin) {
  if (!__atomic_load_n(&g_screen_count, __ATOMIC_RELAXED)) return;
  pthread_mutex_lock(&g_lock);
  Panel *p = panel_of(cb);
  if (is_screen(begin->framebuffer)) {
    if (!p) p = &g_panels[g_panel_next++ % MAX_PANELS];
    *p = (Panel){cb, begin->renderArea};
  } else if (p) {
    p->cb = VK_NULL_HANDLE;
  }
  pthread_mutex_unlock(&g_lock);
}

static void panel_end(VkCommandBuffer cb) {
  if (!__atomic_load_n(&g_screen_count, __ATOMIC_RELAXED)) return;
  pthread_mutex_lock(&g_lock);
  Panel *p = panel_of(cb);
  if (p) p->cb = VK_NULL_HANDLE;
  pthread_mutex_unlock(&g_lock);
}

// Maps x (horizontal, or vertical) from the panel onto the render area.
static double panel_x(const Panel *p, double x) {
  return p->area.offset.x + (x - p->panel.x) * p->area.extent.width / p->panel.width;
}
static double panel_y(const Panel *p, double y) {
  return p->area.offset.y + (y - p->panel.y) * p->area.extent.height / p->panel.height;
}

// Fits viewports (or, without them, scissors) to the panel into out. Returns whether it did.
static int panel_fit(VkCommandBuffer cb, uint32_t count, const VkViewport *viewports, VkViewport *out,
                     const VkRect2D *scissors, VkRect2D *scissors_out) {
  if (!__atomic_load_n(&g_screen_count, __ATOMIC_RELAXED) || count > MAX_PANELS) return 0;
  pthread_mutex_lock(&g_lock);
  Panel *p = panel_of(cb);
  if (p && viewports) {
    const VkViewport *v = &viewports[0];
    p->fit = v->width > p->area.extent.width || v->height > p->area.extent.height;
    if (p->fit) {
      static unsigned logged;
      if (within(&logged, 1))
        LOGI("display: the compositor's panel, %g,%g %gx%g, fit to %d,%d %ux%u", v->x, v->y, v->width, v->height,
             p->area.offset.x, p->area.offset.y, p->area.extent.width, p->area.extent.height);
      p->panel = *v;
    }
  }
  int fit = p && p->fit;
  for (uint32_t i = 0; fit && i < count; i++) {
    if (viewports) {
      out[i] = viewports[i];
      out[i].x = (float)panel_x(p, viewports[i].x);
      out[i].y = (float)panel_y(p, viewports[i].y);
      out[i].width = (float)(viewports[i].width * p->area.extent.width / p->panel.width);
      out[i].height = (float)(viewports[i].height * p->area.extent.height / p->panel.height);
      continue;
    }
    double left = p->area.offset.x, top = p->area.offset.y;
    double right = left + p->area.extent.width, bottom = top + p->area.extent.height;
    double x0 = panel_x(p, scissors[i].offset.x), y0 = panel_y(p, scissors[i].offset.y);
    double x1 = panel_x(p, (double)scissors[i].offset.x + scissors[i].extent.width);
    double y1 = panel_y(p, (double)scissors[i].offset.y + scissors[i].extent.height);
    x0 = x0 < left ? left : x0 > right ? right : x0;
    y0 = y0 < top ? top : y0 > bottom ? bottom : y0;
    x1 = x1 < x0 ? x0 : x1 > right ? right : x1;
    y1 = y1 < y0 ? y0 : y1 > bottom ? bottom : y1;
    scissors_out[i] = (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)((int32_t)x1 - (int32_t)x0),
                                                              (uint32_t)((int32_t)y1 - (int32_t)y0)}};
  }
  pthread_mutex_unlock(&g_lock);
  return fit;
}

static VkResult VKAPI_CALL prism_CreateImage(VkDevice device, const VkImageCreateInfo *info,
                                             const VkAllocationCallbacks *allocator, VkImage *image) {
  if (find_in(info, NATIVE_BUFFER_ANDROID)) {
    VkResult result = next.CreateImage(device, info, allocator, image);
    unsigned n = __atomic_fetch_add(&g_swapchain_count, 1, __ATOMIC_RELAXED);
    if (result == VK_SUCCESS) g_swapchain[n % MAX_SWAPCHAIN] = *image;
    LOGI("image %p: swapchain image %ux%u, format %d, usage %#x", result == VK_SUCCESS ? (void *)*image : NULL,
         info->extent.width, info->extent.height, info->format, info->usage);
    return result;
  }
  const VkExternalMemoryImageCreateInfo *external = find_in(info, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
  if (!external || !(external->handleTypes & OPAQUE_FD)) {
    VkResult result = next.CreateImage(device, info, allocator, image);
    static unsigned logged;
    if (result == VK_SUCCESS && __atomic_load_n(&g_trace_images, __ATOMIC_RELAXED) && within(&logged, 64))
      LOGI("image %p: %ux%u, %u layers, format %d, %u samples, usage %#x", (void *)*image, info->extent.width,
           info->extent.height, info->arrayLayers, info->format, info->samples, info->usage);
    return result;
  }
  VkImageCreateInfo copy = *info;
  VkExternalMemoryImageCreateInfo ahb = *external;
  Scratch scratch = {.used = 0};
  // gfxstream exports no sRGB image, but shares an sRGB image's UNORM twin, which can have sRGB
  // views if it's created mutable: views keep the format they ask for, so sampling and rendering
  // still convert.
  VkFormat twin = info->arrayLayers == 1 ? unorm_twin(info->format) : VK_FORMAT_UNDEFINED;
  VkFormat formats[MAX_VIEW_FORMATS];
  VkImageFormatListCreateInfo list = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 0, formats};
  if (twin != VK_FORMAT_UNDEFINED) {
    const VkImageFormatListCreateInfo *asked = find_in(info, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO);
    formats[list.viewFormatCount++] = twin;
    formats[list.viewFormatCount++] = info->format;
    for (uint32_t i = 0; asked && i < asked->viewFormatCount && list.viewFormatCount < MAX_VIEW_FORMATS; i++)
      if (asked->pViewFormats[i] != twin && asked->pViewFormats[i] != info->format)
        formats[list.viewFormatCount++] = asked->pViewFormats[i];
    copy.format = twin;
    copy.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    replace_in_chain(&copy, &list, &scratch);
  }
  // The host accepts only single-layer AHardwareBuffer images of AHardwareBuffer formats; any other
  // is a plain image, backed: it shares a mirror (see prism_AllocateMemory).
  int backed = copy.arrayLayers > 1 || !ahb_format(copy.format);
  VkFormat mirror = VK_FORMAT_UNDEFINED;
  if (backed && copy.samples == VK_SAMPLE_COUNT_1_BIT && copy.imageType == VK_IMAGE_TYPE_2D &&
      copy.arrayLayers <= MAX_LAYERS)
    mirror = mirror_format(copy.format);
  if (mirror != VK_FORMAT_UNDEFINED) copy.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ahb.handleTypes = backed ? external->handleTypes & ~(VkExternalMemoryHandleTypeFlags)OPAQUE_FD
                           : as_ahb(external->handleTypes);
  replace_in_chain(&copy, &ahb, &scratch);
  VkResult result = next.CreateImage(device, &copy, allocator, image);
  if (result != VK_SUCCESS || !backed) return result;
  pthread_mutex_lock(&g_lock);
  Backed *b = NULL;
  for (int i = 0; i < MAX_BACKED && !b; i++)
    if (g_backed[i].image == VK_NULL_HANDLE && g_backed[i].memory == VK_NULL_HANDLE) b = &g_backed[i];
  if (b) {
    *b = (Backed){*image, device, info->extent, info->arrayLayers, info->format, mirror, info->initialLayout};
    __atomic_store_n(&g_backed_count, g_backed_count + 1, __ATOMIC_RELAXED);
  }
  pthread_mutex_unlock(&g_lock);
  if (!b) LOGE("image %p: more than %d backed images, its contents aren't shared", (void *)*image, MAX_BACKED);
  else LOGI("image %p: %ux%u, %u layers, format %d: backed, %s", (void *)*image, info->extent.width,
            info->extent.height, info->arrayLayers, info->format,
            mirror != VK_FORMAT_UNDEFINED ? "shared through a mirror" : "its contents aren't shared");
  return result;
}

// --- what command buffers do to backed images ----------------------------------------------------

typedef struct {
  VkImageView view;
  VkImage image;
} View;
static View *g_views;
static uint32_t g_view_count, g_view_capacity;

static VkImage view_image(VkImageView view) {
  for (uint32_t i = 0; view != VK_NULL_HANDLE && i < g_view_count; i++)
    if (g_views[i].view == view) return g_views[i].image;
  return VK_NULL_HANDLE;
}

typedef struct {
  VkRenderPass pass;
  uint32_t count;
  VkImageLayout *final;  // each attachment's layout after the pass
} Pass;
static Pass *g_passes;
static uint32_t g_pass_count, g_pass_capacity;

#define MAX_ATTACHMENTS 8
typedef struct {
  VkFramebuffer framebuffer;
  uint32_t count;
  uint32_t attachment[MAX_ATTACHMENTS];
  VkImage image[MAX_ATTACHMENTS];
} Framebuffer;  // only framebuffers with backed attachments
static Framebuffer *g_framebuffers;
static uint32_t g_framebuffer_count, g_framebuffer_capacity;

typedef struct {
  VkDescriptorSet set;
  uint32_t binding, element;
  VkImage image;
  VkImageLayout layout;  // the layout it's read in
} Descriptor;  // only descriptors of backed images
static Descriptor *g_descriptors;
static uint32_t g_descriptor_count, g_descriptor_capacity;

typedef struct {
  VkDescriptorSet set;
  VkDescriptorPool pool;
} SetPool;  // sets allocated while there are backed images, so their descriptors go with them
static SetPool *g_set_pools;
static uint32_t g_set_pool_count, g_set_pool_capacity;

typedef struct {
  VkImage image;
  VkImageLayout layout;  // NO_LAYOUT: unchanged
  int write;
  VkImageLayout read_in;  // a descriptor's: the layout it's read in, NO_LAYOUT if not known
} Use;
typedef struct {
  VkCommandBuffer cb;
  Use *uses;
  uint32_t count, capacity;
} Record;
static Record *g_records;
static uint32_t g_record_count, g_record_capacity;

static Record *record_of(VkCommandBuffer cb, int create) {
  for (uint32_t i = 0; i < g_record_count; i++)
    if (g_records[i].cb == cb) return &g_records[i];
  if (!create || !ROOM(g_records, g_record_count, g_record_capacity)) return NULL;
  g_records[g_record_count] = (Record){cb, NULL, 0, 0};
  return &g_records[g_record_count++];
}

static void use_as(VkCommandBuffer cb, VkImage image, VkImageLayout layout, int write, VkImageLayout read_in) {
  if (!mirrored(image)) return;
  Record *r = record_of(cb, 1);
  if (r && ROOM(r->uses, r->count, r->capacity)) r->uses[r->count++] = (Use){image, layout, write, read_in};
}

static void use(VkCommandBuffer cb, VkImage image, VkImageLayout layout, int write) {
  use_as(cb, image, layout, write, NO_LAYOUT);
}

static void VKAPI_CALL prism_DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *allocator) {
  if (image != VK_NULL_HANDLE && tracking()) {
    pthread_mutex_lock(&g_lock);
    Backed *b = backed_image(image);
    if (b) {
      b->image = VK_NULL_HANDLE;
      release_backed(b);
      for (uint32_t i = g_view_count; i-- > 0;)
        if (g_views[i].image == image) g_views[i] = g_views[--g_view_count];
      for (uint32_t i = g_descriptor_count; i-- > 0;)
        if (g_descriptors[i].image == image) g_descriptors[i] = g_descriptors[--g_descriptor_count];
    }
    pthread_mutex_unlock(&g_lock);
  }
  next.DestroyImage(device, image, allocator);
}

static VkResult VKAPI_CALL prism_CreateImageView(VkDevice device, const VkImageViewCreateInfo *info,
                                                 const VkAllocationCallbacks *allocator, VkImageView *view) {
  VkResult result = next.CreateImageView(device, info, allocator, view);
  for (unsigned i = 0; result == VK_SUCCESS && i < MAX_SWAPCHAIN; i++) {
    if (g_swapchain[i] == VK_NULL_HANDLE || g_swapchain[i] != info->image) continue;
    unsigned n = __atomic_fetch_add(&g_swapchain_view_count, 1, __ATOMIC_RELAXED);
    g_swapchain_views[n % MAX_SWAPCHAIN] = *view;
    LOGI("view %p: of swapchain image %p, format %d", (void *)*view, (void *)info->image, info->format);
    break;
  }
  static unsigned logged;
  if (result == VK_SUCCESS && __atomic_load_n(&g_trace_images, __ATOMIC_RELAXED) && within(&logged, 96))
    LOGI("view %p: of image %p, type %d, format %d, layers %u+%u", (void *)*view, (void *)info->image, info->viewType,
         info->format, info->subresourceRange.baseArrayLayer, info->subresourceRange.layerCount);
  if (result != VK_SUCCESS || !tracking()) return result;
  pthread_mutex_lock(&g_lock);
  if (backed_image(info->image) && ROOM(g_views, g_view_count, g_view_capacity)) {
    g_views[g_view_count++] = (View){*view, info->image};
    static unsigned logged;
    if (within(&logged, 32))
      LOGI("view %p: of backed image %p, type %d, format %d, layers %u-%u", (void *)*view, (void *)info->image,
           info->viewType, info->format, info->subresourceRange.baseArrayLayer, info->subresourceRange.layerCount);
  }
  pthread_mutex_unlock(&g_lock);
  return result;
}

static void VKAPI_CALL prism_DestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks *allocator) {
  if (__atomic_load_n(&g_view_count, __ATOMIC_RELAXED)) {
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < g_view_count; i++)
      if (g_views[i].view == view) g_views[i--] = g_views[--g_view_count];
    pthread_mutex_unlock(&g_lock);
  }
  next.DestroyImageView(device, view, allocator);
}

static void add_pass(VkRenderPass pass, uint32_t count, const void *attachments, size_t stride, size_t final_offset) {
  pthread_mutex_lock(&g_lock);
  VkImageLayout *final = count ? malloc(count * sizeof *final) : NULL;
  if ((final || !count) && ROOM(g_passes, g_pass_count, g_pass_capacity)) {
    for (uint32_t i = 0; i < count; i++)
      memcpy(&final[i], (const char *)attachments + i * stride + final_offset, sizeof *final);
    g_passes[g_pass_count++] = (Pass){pass, count, final};
  } else {
    free(final);
  }
  pthread_mutex_unlock(&g_lock);
}

static VkResult VKAPI_CALL prism_CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *info,
                                                  const VkAllocationCallbacks *allocator, VkRenderPass *pass) {
  VkResult result = next.CreateRenderPass(device, info, allocator, pass);
  static unsigned logged;
  const VkRenderPassMultiviewCreateInfo *multiview = find_in(info, VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO);
  if (result == VK_SUCCESS && info->attachmentCount && within(&logged, 64))
    LOGI("pass %p: %u attachments, first: format %d, load %d, store %d, %d to %d; views %#x", (void *)*pass,
         info->attachmentCount, info->pAttachments->format, info->pAttachments->loadOp, info->pAttachments->storeOp,
         info->pAttachments->initialLayout, info->pAttachments->finalLayout,
         multiview && multiview->subpassCount ? multiview->pViewMasks[0] : 0);
  if (result == VK_SUCCESS)
    add_pass(*pass, info->attachmentCount, info->pAttachments, sizeof *info->pAttachments,
             offsetof(VkAttachmentDescription, finalLayout));
  return result;
}

static VkResult render_pass2(PFN_vkCreateRenderPass2 down, VkDevice device, const VkRenderPassCreateInfo2 *info,
                             const VkAllocationCallbacks *allocator, VkRenderPass *pass) {
  VkResult result = down(device, info, allocator, pass);
  static unsigned logged;
  if (result == VK_SUCCESS && info->attachmentCount && within(&logged, 64))
    LOGI("pass %p: %u attachments, first: format %d, load %d, store %d, %d to %d; %u views", (void *)*pass,
         info->attachmentCount, info->pAttachments->format, info->pAttachments->loadOp, info->pAttachments->storeOp,
         info->pAttachments->initialLayout, info->pAttachments->finalLayout,
         info->subpassCount ? info->pSubpasses->viewMask : 0);
  if (result == VK_SUCCESS)
    add_pass(*pass, info->attachmentCount, info->pAttachments, sizeof *info->pAttachments,
             offsetof(VkAttachmentDescription2, finalLayout));
  return result;
}

static VkResult VKAPI_CALL prism_CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2 *info,
                                                   const VkAllocationCallbacks *allocator, VkRenderPass *pass) {
  return render_pass2(next.CreateRenderPass2, device, info, allocator, pass);
}

static VkResult VKAPI_CALL prism_CreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2 *info,
                                                      const VkAllocationCallbacks *allocator, VkRenderPass *pass) {
  return render_pass2(next.CreateRenderPass2KHR, device, info, allocator, pass);
}

static void VKAPI_CALL prism_DestroyRenderPass(VkDevice device, VkRenderPass pass, const VkAllocationCallbacks *allocator) {
  pthread_mutex_lock(&g_lock);
  for (uint32_t i = 0; i < g_pass_count; i++) {
    if (g_passes[i].pass != pass) continue;
    free(g_passes[i].final);
    g_passes[i] = g_passes[--g_pass_count];
    break;
  }
  pthread_mutex_unlock(&g_lock);
  next.DestroyRenderPass(device, pass, allocator);
}

static VkResult VKAPI_CALL prism_CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *info,
                                                   const VkAllocationCallbacks *allocator, VkFramebuffer *framebuffer) {
  VkResult result = next.CreateFramebuffer(device, info, allocator, framebuffer);
  for (uint32_t i = 0; result == VK_SUCCESS && __atomic_load_n(&g_swapchain_view_count, __ATOMIC_RELAXED) &&
                       !(info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && i < info->attachmentCount; i++) {
    int screen = 0;
    for (unsigned k = 0; k < MAX_SWAPCHAIN && !screen; k++)
      screen = g_swapchain_views[k] != VK_NULL_HANDLE && g_swapchain_views[k] == info->pAttachments[i];
    if (!screen) continue;
    pthread_mutex_lock(&g_lock);
    g_screens[g_screen_count % MAX_SWAPCHAIN] = *framebuffer;
    __atomic_store_n(&g_screen_count, g_screen_count + 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_lock);
    break;
  }
  static unsigned logged;
  if (result == VK_SUCCESS && within(&logged, 64))
    LOGI("framebuffer %p: %ux%u, %u layers, pass %p, %u attachments: %p %p %p", (void *)*framebuffer, info->width,
         info->height, info->layers, (void *)info->renderPass, info->attachmentCount,
         info->attachmentCount > 0 && info->pAttachments ? (void *)info->pAttachments[0] : NULL,
         info->attachmentCount > 1 && info->pAttachments ? (void *)info->pAttachments[1] : NULL,
         info->attachmentCount > 2 && info->pAttachments ? (void *)info->pAttachments[2] : NULL);
  if (result != VK_SUCCESS || !__atomic_load_n(&g_view_count, __ATOMIC_RELAXED) ||
      (info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT))
    return result;
  Framebuffer f = {*framebuffer, 0};
  pthread_mutex_lock(&g_lock);
  for (uint32_t i = 0; i < info->attachmentCount && f.count < MAX_ATTACHMENTS; i++) {
    VkImage image = view_image(info->pAttachments[i]);
    if (image == VK_NULL_HANDLE) continue;
    f.attachment[f.count] = i;
    f.image[f.count++] = image;
  }
  if (f.count && ROOM(g_framebuffers, g_framebuffer_count, g_framebuffer_capacity))
    g_framebuffers[g_framebuffer_count++] = f;
  pthread_mutex_unlock(&g_lock);
  return result;
}

static void VKAPI_CALL prism_DestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer,
                                                const VkAllocationCallbacks *allocator) {
  if (__atomic_load_n(&g_framebuffer_count, __ATOMIC_RELAXED)) {
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < g_framebuffer_count; i++)
      if (g_framebuffers[i].framebuffer == framebuffer) g_framebuffers[i--] = g_framebuffers[--g_framebuffer_count];
    pthread_mutex_unlock(&g_lock);
  }
  next.DestroyFramebuffer(device, framebuffer, allocator);
}

static unsigned g_binds, g_bind_hits;  // for the stats

static void forget_set(VkDescriptorSet set) {
  for (uint32_t k = g_descriptor_count; k-- > 0;)
    if (g_descriptors[k].set == set) g_descriptors[k] = g_descriptors[--g_descriptor_count];
}

static VkResult VKAPI_CALL prism_AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *info,
                                                        VkDescriptorSet *sets) {
  VkResult result = next.AllocateDescriptorSets(device, info, sets);
  if (result != VK_SUCCESS || !tracking()) return result;
  pthread_mutex_lock(&g_lock);
  for (uint32_t i = 0; i < info->descriptorSetCount; i++) {
    forget_set(sets[i]);  // a handle used again
    if (ROOM(g_set_pools, g_set_pool_count, g_set_pool_capacity))
      g_set_pools[g_set_pool_count++] = (SetPool){sets[i], info->descriptorPool};
  }
  pthread_mutex_unlock(&g_lock);
  return result;
}

static VkResult VKAPI_CALL prism_FreeDescriptorSets(VkDevice device, VkDescriptorPool pool, uint32_t count,
                                                    const VkDescriptorSet *sets) {
  if (__atomic_load_n(&g_set_pool_count, __ATOMIC_RELAXED)) {
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < count; i++) {
      forget_set(sets[i]);
      for (uint32_t k = g_set_pool_count; k-- > 0;)
        if (g_set_pools[k].set == sets[i]) g_set_pools[k] = g_set_pools[--g_set_pool_count];
    }
    pthread_mutex_unlock(&g_lock);
  }
  return next.FreeDescriptorSets(device, pool, count, sets);
}

static void forget_pool(VkDescriptorPool pool) {
  if (!__atomic_load_n(&g_set_pool_count, __ATOMIC_RELAXED)) return;
  pthread_mutex_lock(&g_lock);
  for (uint32_t k = g_set_pool_count; k-- > 0;) {
    if (g_set_pools[k].pool != pool) continue;
    forget_set(g_set_pools[k].set);
    g_set_pools[k] = g_set_pools[--g_set_pool_count];
  }
  pthread_mutex_unlock(&g_lock);
}

static VkResult VKAPI_CALL prism_ResetDescriptorPool(VkDevice device, VkDescriptorPool pool,
                                                     VkDescriptorPoolResetFlags flags) {
  forget_pool(pool);
  return next.ResetDescriptorPool(device, pool, flags);
}

static void VKAPI_CALL prism_DestroyDescriptorPool(VkDevice device, VkDescriptorPool pool,
                                                   const VkAllocationCallbacks *allocator) {
  forget_pool(pool);
  next.DestroyDescriptorPool(device, pool, allocator);
}

static int image_descriptor(VkDescriptorType type) {
  return type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
         type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}

static void VKAPI_CALL prism_UpdateDescriptorSets(VkDevice device, uint32_t write_count,
                                                  const VkWriteDescriptorSet *writes, uint32_t copy_count,
                                                  const VkCopyDescriptorSet *copies) {
  if (__atomic_load_n(&g_view_count, __ATOMIC_RELAXED) || __atomic_load_n(&g_descriptor_count, __ATOMIC_RELAXED)) {
    pthread_mutex_lock(&g_lock);
    for (uint32_t w = 0; w < write_count; w++) {
      const VkWriteDescriptorSet *wr = &writes[w];
      if (!image_descriptor(wr->descriptorType) || !wr->pImageInfo) continue;
      for (uint32_t i = 0; i < wr->descriptorCount; i++) {
        VkImage image = view_image(wr->pImageInfo[i].imageView);
        uint32_t element = wr->dstArrayElement + i, k = 0;
        while (k < g_descriptor_count && (g_descriptors[k].set != wr->dstSet || g_descriptors[k].binding != wr->dstBinding ||
                                          g_descriptors[k].element != element))
          k++;
        if (k < g_descriptor_count && image == VK_NULL_HANDLE)
          g_descriptors[k] = g_descriptors[--g_descriptor_count];
        else if (k < g_descriptor_count)
          g_descriptors[k].image = image, g_descriptors[k].layout = wr->pImageInfo[i].imageLayout;
        else if (image != VK_NULL_HANDLE && ROOM(g_descriptors, g_descriptor_count, g_descriptor_capacity))
          g_descriptors[g_descriptor_count++] =
              (Descriptor){wr->dstSet, wr->dstBinding, element, image, wr->pImageInfo[i].imageLayout};
      }
    }
    // Copies carry the backed images of the descriptors they copy.
    for (uint32_t c = 0; c < copy_count; c++) {
      const VkCopyDescriptorSet *cp = &copies[c];
      for (uint32_t i = 0; i < cp->descriptorCount; i++) {
        Descriptor from = {VK_NULL_HANDLE};
        for (uint32_t k = 0; k < g_descriptor_count; k++)
          if (g_descriptors[k].set == cp->srcSet && g_descriptors[k].binding == cp->srcBinding &&
              g_descriptors[k].element == cp->srcArrayElement + i)
            from = g_descriptors[k];
        uint32_t element = cp->dstArrayElement + i, k = 0;
        while (k < g_descriptor_count && (g_descriptors[k].set != cp->dstSet || g_descriptors[k].binding != cp->dstBinding ||
                                          g_descriptors[k].element != element))
          k++;
        if (k < g_descriptor_count && from.image == VK_NULL_HANDLE)
          g_descriptors[k] = g_descriptors[--g_descriptor_count];
        else if (k < g_descriptor_count)
          g_descriptors[k].image = from.image, g_descriptors[k].layout = from.layout;
        else if (from.image != VK_NULL_HANDLE && ROOM(g_descriptors, g_descriptor_count, g_descriptor_capacity))
          g_descriptors[g_descriptor_count++] = (Descriptor){cp->dstSet, cp->dstBinding, element, from.image, from.layout};
      }
    }
    pthread_mutex_unlock(&g_lock);
  }
  next.UpdateDescriptorSets(device, write_count, writes, copy_count, copies);
}

static void forget(VkCommandBuffer cb, int free_it) {
  pthread_mutex_lock(&g_lock);
  Record *r = record_of(cb, 0);
  if (r && free_it) {
    free(r->uses);
    *r = g_records[--g_record_count];
  } else if (r) {
    r->count = 0;
  }
  pthread_mutex_unlock(&g_lock);
}

static VkResult VKAPI_CALL prism_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *info) {
  if (__atomic_load_n(&g_record_count, __ATOMIC_RELAXED)) forget(cb, 0);
  return next.BeginCommandBuffer(cb, info);
}

static VkResult VKAPI_CALL prism_ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
  if (__atomic_load_n(&g_record_count, __ATOMIC_RELAXED)) forget(cb, 0);
  return next.ResetCommandBuffer(cb, flags);
}

static void VKAPI_CALL prism_FreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count,
                                                const VkCommandBuffer *cbs) {
  for (uint32_t i = 0; i < count && __atomic_load_n(&g_record_count, __ATOMIC_RELAXED); i++)
    if (cbs[i]) forget(cbs[i], 1);
  next.FreeCommandBuffers(device, pool, count, cbs);
}

static void use_barriers(VkCommandBuffer cb, uint32_t count, const VkImageMemoryBarrier *barriers) {
  if (!count || !tracking()) return;
  pthread_mutex_lock(&g_lock);
  for (uint32_t i = 0; i < count; i++) use(cb, barriers[i].image, barriers[i].newLayout, 0);
  pthread_mutex_unlock(&g_lock);
}

static void VKAPI_CALL prism_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                                VkDependencyFlags flags, uint32_t memory_count,
                                                const VkMemoryBarrier *memory, uint32_t buffer_count,
                                                const VkBufferMemoryBarrier *buffers, uint32_t image_count,
                                                const VkImageMemoryBarrier *images) {
  use_barriers(cb, image_count, images);
  next.CmdPipelineBarrier(cb, src, dst, flags, memory_count, memory, buffer_count, buffers, image_count, images);
}

static void VKAPI_CALL prism_CmdWaitEvents(VkCommandBuffer cb, uint32_t event_count, const VkEvent *events,
                                           VkPipelineStageFlags src, VkPipelineStageFlags dst, uint32_t memory_count,
                                           const VkMemoryBarrier *memory, uint32_t buffer_count,
                                           const VkBufferMemoryBarrier *buffers, uint32_t image_count,
                                           const VkImageMemoryBarrier *images) {
  use_barriers(cb, image_count, images);
  next.CmdWaitEvents(cb, event_count, events, src, dst, memory_count, memory, buffer_count, buffers, image_count,
                     images);
}

static void use_dependency(VkCommandBuffer cb, const VkDependencyInfo *dependency) {
  if (!dependency->imageMemoryBarrierCount || !tracking()) return;
  pthread_mutex_lock(&g_lock);
  for (uint32_t i = 0; i < dependency->imageMemoryBarrierCount; i++)
    use(cb, dependency->pImageMemoryBarriers[i].image, dependency->pImageMemoryBarriers[i].newLayout, 0);
  pthread_mutex_unlock(&g_lock);
}

static void VKAPI_CALL prism_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *dependency) {
  use_dependency(cb, dependency);
  next.CmdPipelineBarrier2(cb, dependency);
}

static void VKAPI_CALL prism_CmdPipelineBarrier2KHR(VkCommandBuffer cb, const VkDependencyInfo *dependency) {
  use_dependency(cb, dependency);
  next.CmdPipelineBarrier2KHR(cb, dependency);
}

// A render pass writes its attachments and leaves them in their final layouts.
static void use_pass(VkCommandBuffer cb, const VkRenderPassBeginInfo *begin) {
  TRACE("cb %p: begin pass %p, framebuffer %p, area %d,%d %ux%u, %u clears", (void *)cb, (void *)begin->renderPass,
        (void *)begin->framebuffer, begin->renderArea.offset.x, begin->renderArea.offset.y,
        begin->renderArea.extent.width, begin->renderArea.extent.height, begin->clearValueCount);
  panel_begin(cb, begin);
  if (!tracking()) return;
  pthread_mutex_lock(&g_lock);
  const Pass *pass = NULL;
  for (uint32_t i = 0; i < g_pass_count && !pass; i++)
    if (g_passes[i].pass == begin->renderPass) pass = &g_passes[i];
  const VkRenderPassAttachmentBeginInfo *views = find_in(begin, VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO);
  for (uint32_t i = 0; views && i < views->attachmentCount; i++)
    use(cb, view_image(views->pAttachments[i]), pass && i < pass->count ? pass->final[i] : NO_LAYOUT, 1);
  for (uint32_t i = 0; !views && i < g_framebuffer_count; i++) {
    const Framebuffer *f = &g_framebuffers[i];
    if (f->framebuffer != begin->framebuffer) continue;
    for (uint32_t k = 0; k < f->count; k++)
      use(cb, f->image[k], pass && f->attachment[k] < pass->count ? pass->final[f->attachment[k]] : NO_LAYOUT, 1);
    break;
  }
  pthread_mutex_unlock(&g_lock);
}

static void VKAPI_CALL prism_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *begin,
                                                VkSubpassContents contents) {
  use_pass(cb, begin);
  next.CmdBeginRenderPass(cb, begin, contents);
}

static void VKAPI_CALL prism_CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo *begin,
                                                 const VkSubpassBeginInfo *subpass) {
  use_pass(cb, begin);
  next.CmdBeginRenderPass2(cb, begin, subpass);
}

static void VKAPI_CALL prism_CmdBeginRenderPass2KHR(VkCommandBuffer cb, const VkRenderPassBeginInfo *begin,
                                                    const VkSubpassBeginInfo *subpass) {
  use_pass(cb, begin);
  next.CmdBeginRenderPass2KHR(cb, begin, subpass);
}

// A transfer reads src and writes dst (either may be VK_NULL_HANDLE).
static void use_transfer(VkCommandBuffer cb, VkImage src, VkImage dst) {
  if (!tracking()) return;
  pthread_mutex_lock(&g_lock);
  if (src != VK_NULL_HANDLE) use(cb, src, NO_LAYOUT, 0);
  if (dst != VK_NULL_HANDLE) use(cb, dst, NO_LAYOUT, 1);
  pthread_mutex_unlock(&g_lock);
}

static void VKAPI_CALL prism_CmdClearColorImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout,
                                                const VkClearColorValue *color, uint32_t count,
                                                const VkImageSubresourceRange *ranges) {
  use_transfer(cb, VK_NULL_HANDLE, image);
  next.CmdClearColorImage(cb, image, layout, color, count, ranges);
}

static void VKAPI_CALL prism_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout src_layout, VkImage dst,
                                          VkImageLayout dst_layout, uint32_t count, const VkImageCopy *regions) {
  use_transfer(cb, src, dst);
  next.CmdCopyImage(cb, src, src_layout, dst, dst_layout, count, regions);
}

static void VKAPI_CALL prism_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout src_layout, VkImage dst,
                                          VkImageLayout dst_layout, uint32_t count, const VkImageBlit *regions,
                                          VkFilter filter) {
  use_transfer(cb, src, dst);
  next.CmdBlitImage(cb, src, src_layout, dst, dst_layout, count, regions, filter);
}

static void VKAPI_CALL prism_CmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout src_layout, VkImage dst,
                                             VkImageLayout dst_layout, uint32_t count, const VkImageResolve *regions) {
  use_transfer(cb, src, dst);
  next.CmdResolveImage(cb, src, src_layout, dst, dst_layout, count, regions);
}

static void VKAPI_CALL prism_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout layout,
                                                  uint32_t count, const VkBufferImageCopy *regions) {
  use_transfer(cb, VK_NULL_HANDLE, dst);
  next.CmdCopyBufferToImage(cb, src, dst, layout, count, regions);
}

static void VKAPI_CALL prism_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout layout, VkBuffer dst,
                                                  uint32_t count, const VkBufferImageCopy *regions) {
  use_transfer(cb, src, VK_NULL_HANDLE);
  next.CmdCopyImageToBuffer(cb, src, layout, dst, count, regions);
}

// Sampling reads every backed image the bound sets hold.
static void VKAPI_CALL prism_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint point,
                                                   VkPipelineLayout layout, uint32_t first, uint32_t count,
                                                   const VkDescriptorSet *sets, uint32_t dynamic_count,
                                                   const uint32_t *dynamic_offsets) {
  if (__atomic_load_n(&g_descriptor_count, __ATOMIC_RELAXED)) {
    pthread_mutex_lock(&g_lock);
    g_binds++;
    for (uint32_t i = 0; i < count; i++) {
      for (uint32_t k = 0; k < g_descriptor_count; k++)
        if (g_descriptors[k].set == sets[i])
          use_as(cb, g_descriptors[k].image, NO_LAYOUT, 0, g_descriptors[k].layout), g_bind_hits++;
    }
    pthread_mutex_unlock(&g_lock);
  }
  next.CmdBindDescriptorSets(cb, point, layout, first, count, sets, dynamic_count, dynamic_offsets);
}

static void VKAPI_CALL prism_CmdExecuteCommands(VkCommandBuffer cb, uint32_t count, const VkCommandBuffer *secondaries) {
  if (__atomic_load_n(&g_record_count, __ATOMIC_RELAXED)) {
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < count; i++) {
      Record *s = record_of(secondaries[i], 0);
      for (uint32_t k = 0; s && k < s->count; k++) {
        Use u = s->uses[k];
        use_as(cb, u.image, u.layout, u.write, u.read_in);
        s = record_of(secondaries[i], 0);  // use() may have moved the records
      }
    }
    pthread_mutex_unlock(&g_lock);
  }
  next.CmdExecuteCommands(cb, count, secondaries);
}

// --- copies to and from mirrors ------------------------------------------------------------------

typedef struct {
  VkQueue queue;
  VkDevice device;
  uint32_t family;
} Queue;
static Queue *g_queues;
static uint32_t g_queue_count, g_queue_capacity;

static void add_queue(VkDevice device, uint32_t family, VkQueue queue) {
  pthread_mutex_lock(&g_lock);
  uint32_t i = 0;
  while (i < g_queue_count && g_queues[i].queue != queue) i++;
  if (i < g_queue_count || ROOM(g_queues, g_queue_count, g_queue_capacity)) {
    g_queues[i] = (Queue){queue, device, family};
    if (i == g_queue_count) g_queue_count++;
  }
  pthread_mutex_unlock(&g_lock);
}

static void VKAPI_CALL prism_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *queue) {
  next.GetDeviceQueue(device, family, index, queue);
  if (*queue) add_queue(device, family, *queue);
}

static void VKAPI_CALL prism_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue) {
  next.GetDeviceQueue2(device, info, queue);
  if (*queue) add_queue(device, info->queueFamilyIndex, *queue);
}

// Prism's command buffers, per device and queue family: a ring of slots, each a fence and the
// command buffers one vkQueueSubmit adds, free again once its fence is signaled.
#define RING 8
#define SLOT_CBS 8
typedef struct {
  VkFence fence;
  VkCommandBuffer cb[SLOT_CBS];
  uint32_t used;
} Slot;
typedef struct {
  VkDevice device;
  uint32_t family;
  VkCommandPool pool;
  Slot slot[RING];
  uint32_t next;
} Ring;
static Ring *g_rings;
static uint32_t g_ring_count, g_ring_capacity;

static Ring *ring_of(VkDevice device, uint32_t family) {
  for (uint32_t i = 0; i < g_ring_count; i++)
    if (g_rings[i].device == device && g_rings[i].family == family) return &g_rings[i];
  VkCommandPoolCreateInfo info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                  VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
  VkCommandPool pool;
  if (!ROOM(g_rings, g_ring_count, g_ring_capacity) || next.CreateCommandPool(device, &info, NULL, &pool) != VK_SUCCESS)
    return NULL;
  Ring *r = &g_rings[g_ring_count++];
  memset(r, 0, sizeof *r);
  r->device = device, r->family = family, r->pool = pool;
  return r;
}

static Slot *take_slot(Ring *r) {
  Slot *s = NULL;
  for (uint32_t k = 0; k < RING && !s; k++) {
    Slot *candidate = &r->slot[(r->next + k) % RING];
    if (candidate->fence == VK_NULL_HANDLE) {
      VkFenceCreateInfo info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      if (next.CreateFence(r->device, &info, NULL, &candidate->fence) != VK_SUCCESS) return NULL;
      s = candidate;
    } else if (next.GetFenceStatus(r->device, candidate->fence) == VK_SUCCESS) {
      s = candidate;
    }
  }
  if (!s) {  // all in flight: wait for the oldest
    s = &r->slot[r->next];
    if (next.WaitForFences(r->device, 1, &s->fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) return NULL;
  }
  if (next.ResetFences(r->device, 1, &s->fence) != VK_SUCCESS) return NULL;
  r->next = (uint32_t)(s - r->slot + 1) % RING;
  s->used = 0;
  return s;
}

static VkCommandBuffer slot_cb(Ring *r, Slot *s) {
  if (s->used == SLOT_CBS) return VK_NULL_HANDLE;
  VkCommandBuffer *cb = &s->cb[s->used];
  if (!*cb) {
    VkCommandBufferAllocateInfo info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, r->pool,
                                        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (next.AllocateCommandBuffers(r->device, &info, cb) != VK_SUCCESS) return *cb = VK_NULL_HANDLE;
  }
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  if (next.BeginCommandBuffer(*cb, &begin) != VK_SUCCESS) return VK_NULL_HANDLE;
  s->used++;
  return *cb;
}

// The first memory type of bits with flags want; memoryTypeCount if there's none.
static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
  uint32_t type = 0;
  while (type < g_memory_properties.memoryTypeCount &&
         (!(bits & (1u << type)) || (g_memory_properties.memoryTypes[type].propertyFlags & want) != want))
    type++;
  return type;
}

// Records a copy of b's image (in layout) to a host-visible buffer, for prism_QueueSubmit to write to
// a file. Returns the image's layout afterwards.
static VkImageLayout record_dump(VkCommandBuffer cb, Backed *b, int push, VkImageLayout layout) {
  VkDeviceSize layer_size = (VkDeviceSize)b->extent.width * b->extent.height * 4;
  VkDeviceSize size = layer_size * b->layers;
  VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_SHARING_MODE_EXCLUSIVE};
  VkBuffer buffer;
  if (next.CreateBuffer(b->device, &info, NULL, &buffer) != VK_SUCCESS) return layout;
  VkMemoryRequirements req;
  next.GetBufferMemoryRequirements(b->device, buffer, &req);
  uint32_t type =
      memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
  VkDeviceMemory memory;
  if (type == g_memory_properties.memoryTypeCount || next.AllocateMemory(b->device, &allocate, NULL, &memory) != VK_SUCCESS) {
    next.DestroyBuffer(b->device, buffer, NULL);
    return layout;
  }
  next.BindBufferMemory(b->device, buffer, memory, 0);
  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                                  VK_ACCESS_TRANSFER_READ_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, b->image,
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
  next.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                          &barrier);
  VkBufferImageCopy regions[MAX_LAYERS];
  for (uint32_t i = 0; i < b->layers; i++)
    regions[i] = (VkBufferImageCopy){i * layer_size, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, i, 1}, {0, 0, 0},
                                     {b->extent.width, b->extent.height, 1}};
  next.CmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, b->layers, regions);
  g_dump.device = b->device, g_dump.buffer = buffer, g_dump.memory = memory, g_dump.extent = b->extent;
  g_dump.extent.height *= b->layers;
  g_dump.push = push;
  return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}

static void write_dump(VkFence fence) {
  if (next.WaitForFences(g_dump.device, 1, &fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) return;
  void *data;
  if (next.MapMemory(g_dump.device, g_dump.memory, 0, VK_WHOLE_SIZE, 0, &data) != VK_SUCCESS) return;
  char path[96];
  snprintf(path, sizeof path, "/data/local/tmp/prism_%s_%d.rgba", g_dump.push ? "into" : "out", getpid());
  FILE *f = fopen(path, "wb");
  if (f) {
    fwrite(data, 4, (size_t)g_dump.extent.width * g_dump.extent.height, f);
    fclose(f);
  }
  LOGI("dump: %s, %ux%u (%s)", path, g_dump.extent.width, g_dump.extent.height, f ? "written" : strerror(errno));
}

// Records a copy of b's image into its mirror (push) or out of it, the image starting in b->layout.
// Returns the image's layout afterwards: the same, or if that isn't known, read_in (a descriptor's,
// for an image the app never transitions because the writer leaves it in that layout) or GENERAL.
static VkImageLayout record_copy(VkCommandBuffer cb, Backed *b, int push, VkImageLayout read_in) {
  Backed *m = mirror_of(b);
  VkImageLayout layout = b->layout;
  VkImageLayout after = layout != VK_IMAGE_LAYOUT_UNDEFINED && layout != VK_IMAGE_LAYOUT_PREINITIALIZED ? layout
                        : read_in != NO_LAYOUT                                                         ? read_in
                                                                                                       : VK_IMAGE_LAYOUT_GENERAL;
  VkImageLayout transfer = push ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  VkAccessFlags image_access = push ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
  VkAccessFlags mirror_access = push ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT;
  const VkAccessFlags any = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  VkImageSubresourceRange image_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                         VK_REMAINING_ARRAY_LAYERS};
  VkImageSubresourceRange mirror_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageMemoryBarrier before[2] = {
      {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, any, image_access, layout, transfer, VK_QUEUE_FAMILY_IGNORED,
       VK_QUEUE_FAMILY_IGNORED, b->image, image_range},
      {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, any, mirror_access,
       m->mirror_general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
       VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, m->mirror, mirror_range},
  };
  next.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                          2, before);
  VkImageCopy regions[MAX_LAYERS];
  for (uint32_t l = 0; l < b->layers; l++) {
    VkImageSubresourceLayers layer = {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1}, mirror = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    VkOffset3D origin = {0, 0, 0}, stacked = {0, (int32_t)(l * b->extent.height), 0};
    regions[l] = push ? (VkImageCopy){layer, origin, mirror, stacked, {b->extent.width, b->extent.height, 1}}
                      : (VkImageCopy){mirror, stacked, layer, origin, {b->extent.width, b->extent.height, 1}};
  }
  if (push)
    next.CmdCopyImage(cb, b->image, transfer, m->mirror, VK_IMAGE_LAYOUT_GENERAL, b->layers, regions);
  else
    next.CmdCopyImage(cb, m->mirror, VK_IMAGE_LAYOUT_GENERAL, b->image, transfer, b->layers, regions);
  if (g_dump_at && !g_dump.buffer && ((push ? g_pushes : g_pulls) + 1) % g_dump_at == 0) transfer = record_dump(cb, b, push, transfer);
  VkImageMemoryBarrier done[2] = {
      {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, image_access, any, transfer, after, VK_QUEUE_FAMILY_IGNORED,
       VK_QUEUE_FAMILY_IGNORED, b->image, image_range},
      {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, mirror_access, any, VK_IMAGE_LAYOUT_GENERAL,
       VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, m->mirror, mirror_range},
  };
  next.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
                          2, done);
  m->mirror_general = 1;
  int bit = push ? 1 : 2;
  if (!(b->logged & bit)) {
    b->logged |= bit;
    LOGI("image %p: copied %s its mirror", (void *)b->image, push ? "into" : "out of");
  }
  return after;
}

// What one batch does to the mirrored images it uses.
#define MAX_PLANS 32
typedef struct {
  Backed *b;
  int write, read;
  VkImageLayout layout;   // the last layout its commands leave it in (NO_LAYOUT: unchanged)
  VkImageLayout read_in;  // a layout a descriptor reads it in (NO_LAYOUT: none)
} Plan;

static uint32_t plan_batch(const VkSubmitInfo *batch, Plan *plans) {
  uint32_t n = 0;
  for (uint32_t c = 0; c < batch->commandBufferCount; c++) {
    const Record *r = record_of(batch->pCommandBuffers[c], 0);
    for (uint32_t k = 0; r && k < r->count; k++) {
      Backed *b = mirrored(r->uses[k].image);
      if (!b) continue;
      uint32_t p = 0;
      while (p < n && plans[p].b != b) p++;
      if (p == n) {
        if (n == MAX_PLANS) continue;
        plans[n++] = (Plan){b, 0, 0, NO_LAYOUT, NO_LAYOUT};
      }
      if (r->uses[k].write) plans[p].write = 1;
      else plans[p].read = 1;
      if (r->uses[k].layout != NO_LAYOUT) plans[p].layout = r->uses[k].layout;
      if (r->uses[k].read_in != NO_LAYOUT && plans[p].read_in == NO_LAYOUT) plans[p].read_in = r->uses[k].read_in;
    }
  }
  return n;
}

// Copies that bring other processes' writes in and send this one's out. A batch that writes a
// mirrored image gets a copy into the mirror after its own command buffers, so the semaphores and
// fences it signals cover the copy. One that only reads it gets a copy out of the mirror first,
// after the semaphores it waits for (their wait stages are widened to transfers). The image's
// layouts are followed through each batch, in submission order.
static time_t g_stats_time;

// Arms the trace when debug.prism.vk.trace changes and reads debug.prism.vk.dump (once a second), and
// traces the batches.
static void trace_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits) {
  static time_t checked;
  static char seen[PROP_VALUE_MAX];
  time_t now = time(NULL);
  if (now != __atomic_load_n(&checked, __ATOMIC_RELAXED)) {
    __atomic_store_n(&checked, now, __ATOMIC_RELAXED);
    char value[PROP_VALUE_MAX] = "";
    __system_property_get("debug.prism.vk.dump", value);
    __atomic_store_n(&g_dump_at, (unsigned)atoi(value), __ATOMIC_RELAXED);
    value[0] = 0;
    __system_property_get("debug.prism.vk.trace", value);
    pthread_mutex_lock(&g_lock);
    if (strcmp(value, seen)) {
      strcpy(seen, value);
      __atomic_store_n(&g_trace, atoi(value), __ATOMIC_RELAXED);
      if (atoi(value) > 0) LOGI("trace: the next %d lines", atoi(value));
    }
    pthread_mutex_unlock(&g_lock);
  }
  for (uint32_t i = 0; i < count && __atomic_load_n(&g_trace, __ATOMIC_RELAXED) > 0; i++) {
    const VkSubmitInfo *s = &submits[i];
    TRACE("queue %p: batch %u of %u, %u waits, %u command buffers (%p %p %p %p), %u signals", (void *)queue, i, count,
          s->waitSemaphoreCount, s->commandBufferCount, s->commandBufferCount > 0 ? (void *)s->pCommandBuffers[0] : NULL,
          s->commandBufferCount > 1 ? (void *)s->pCommandBuffers[1] : NULL,
          s->commandBufferCount > 2 ? (void *)s->pCommandBuffers[2] : NULL,
          s->commandBufferCount > 3 ? (void *)s->pCommandBuffers[3] : NULL, s->signalSemaphoreCount);
  }
}

static VkResult VKAPI_CALL prism_QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
  trace_submit(queue, count, submits);
  if (tracking()) {
    pthread_mutex_lock(&g_lock);
    g_submits++;
    time_t now = time(NULL);
    if (now - g_stats_time >= 10) {
      g_stats_time = now;
      LOGI("stats: %u submits, %u copies out of mirrors, %u into; %d backed, %u views, %u descriptors, %u framebuffers, "
           "%u records, %u sets; %u binds, %u hits; %u acquires, %u presents", g_submits, g_pulls, g_pushes,
           g_backed_count, g_view_count, g_descriptor_count, g_framebuffer_count, g_record_count, g_set_pool_count, g_binds,
           g_bind_hits, g_acquires, g_releases);
    }
    pthread_mutex_unlock(&g_lock);
  }
  if (!count || !tracking() || !__atomic_load_n(&g_record_count, __ATOMIC_RELAXED))
    return next.QueueSubmit(queue, count, submits, fence);
  pthread_mutex_lock(&g_lock);
  const Queue *q = NULL;
  for (uint32_t i = 0; i < g_queue_count && !q; i++)
    if (g_queues[i].queue == queue) q = &g_queues[i];
  Ring *ring = NULL;
  Slot *slot = NULL;
  VkSubmitInfo *batches = NULL;
  VkCommandBuffer *cbs = NULL;
  VkPipelineStageFlags *stages = NULL;
  size_t cb_used = 0, stage_used = 0;
  int prepared = 0;
  for (uint32_t i = 0; i < count; i++) {
    Plan plans[MAX_PLANS];
    uint32_t n = plan_batch(&submits[i], plans), pulls = 0, pushes = 0;
    for (uint32_t p = 0; p < n; p++) {
      pulls += plans[p].read && !plans[p].write;
      pushes += plans[p].write;
    }
    if ((pulls || pushes) && q && !prepared) {
      prepared = 1;
      ring = ring_of(q->device, q->family);
      slot = ring ? take_slot(ring) : NULL;
      size_t total_cbs = 0, total_stages = 0;
      for (uint32_t k = 0; k < count; k++) {
        total_cbs += submits[k].commandBufferCount + 2;
        total_stages += submits[k].waitSemaphoreCount;
      }
      batches = malloc(count * sizeof *batches);
      cbs = malloc(total_cbs * sizeof *cbs);
      stages = malloc((total_stages + 1) * sizeof *stages);
      if (batches) memcpy(batches, submits, count * sizeof *batches);
      if (!slot || !batches || !cbs || !stages) LOGE("vkQueueSubmit: no room for mirror copies");
    }
    int copying = slot && batches && cbs && stages && !find_in(&submits[i], VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO);
    VkCommandBuffer pull = VK_NULL_HANDLE, push = VK_NULL_HANDLE;
    if (copying && pulls && (pull = slot_cb(ring, slot)))
      for (uint32_t p = 0; p < n; p++)
        if (plans[p].read && !plans[p].write) plans[p].b->layout = record_copy(pull, plans[p].b, 0, plans[p].read_in), g_pulls++;
    for (uint32_t p = 0; p < n; p++)
      if (plans[p].layout != NO_LAYOUT) plans[p].b->layout = plans[p].layout;
    if (copying && pushes && (push = slot_cb(ring, slot)))
      for (uint32_t p = 0; p < n; p++) {
        if (!plans[p].write) continue;
        if (plans[p].b->layout == VK_IMAGE_LAYOUT_UNDEFINED)
          LOGE("image %p: written, but its layout isn't known; not copied", (void *)plans[p].b->image);
        else
          plans[p].b->layout = record_copy(push, plans[p].b, 1, NO_LAYOUT), g_pushes++;
      }
    if (pull) next.EndCommandBuffer(pull);
    if (push) next.EndCommandBuffer(push);
    if (!pull && !push) continue;
    VkSubmitInfo *batch = &batches[i];
    VkCommandBuffer *list = cbs + cb_used;
    uint32_t k = 0;
    if (pull) list[k++] = pull;
    memcpy(list + k, submits[i].pCommandBuffers, submits[i].commandBufferCount * sizeof *list);
    k += submits[i].commandBufferCount;
    if (push) list[k++] = push;
    batch->commandBufferCount = k;
    batch->pCommandBuffers = list;
    cb_used += k;
    if (pull && submits[i].waitSemaphoreCount) {
      VkPipelineStageFlags *wait = stages + stage_used;
      for (uint32_t w = 0; w < submits[i].waitSemaphoreCount; w++)
        wait[w] = submits[i].pWaitDstStageMask[w] | VK_PIPELINE_STAGE_TRANSFER_BIT;
      batch->pWaitDstStageMask = wait;
      stage_used += submits[i].waitSemaphoreCount;
    }
  }
  VkResult result = next.QueueSubmit(queue, count, batches ? batches : submits, fence);
  if (slot && next.QueueSubmit(queue, 0, NULL, slot->fence) != VK_SUCCESS)
    LOGE("vkQueueSubmit: can't fence the mirror copies");
  if (slot && g_dump.buffer) {
    write_dump(slot->fence);
    next.DestroyBuffer(g_dump.device, g_dump.buffer, NULL);
    next.FreeMemory(g_dump.device, g_dump.memory, NULL);
    memset(&g_dump, 0, sizeof g_dump);
  }
  pthread_mutex_unlock(&g_lock);
  free(batches);
  free(cbs);
  free(stages);
  return result;
}

// --- memory --------------------------------------------------------------------------------------

static VkResult VKAPI_CALL prism_CreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
                                              const VkAllocationCallbacks *allocator, VkBuffer *buffer) {
  const VkExternalMemoryBufferCreateInfo *external = find_in(info, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO);
  if (!external || !(external->handleTypes & OPAQUE_FD)) return next.CreateBuffer(device, info, allocator, buffer);
  VkBufferCreateInfo copy = *info;
  VkExternalMemoryBufferCreateInfo ahb = *external;
  ahb.handleTypes = as_ahb(external->handleTypes);
  Scratch scratch = {.used = 0};
  replace_in_chain(&copy, &ahb, &scratch);
  return next.CreateBuffer(device, &copy, allocator, buffer);
}

// The buffer a backed image's fd carries: its mirror, a single-layer color buffer (what gralloc
// here can share) as wide as the image and as tall as its layers stacked. An image no mirror can
// hold gets a buffer of its size, which nothing reads; its fd imports all the same.
static AHardwareBuffer *mirror_buffer(const Backed *b, VkDeviceSize size) {
  AHardwareBuffer_Desc desc = {
      b->extent.width, b->extent.height * b->layers, 1,
      b->mirror_format == VK_FORMAT_R16G16B16A16_SFLOAT ? AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT
                                                        : AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
      AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT, 0, 0, 0};
  if (b->mirror_format == VK_FORMAT_UNDEFINED) {
    const uint32_t width = 1024;  // RGBA8: 4 KiB a row
    desc.width = width;
    desc.height = (uint32_t)((size + width * 4 - 1) / (width * 4));
  }
  AHardwareBuffer *buffer = NULL;
  if (AHardwareBuffer_allocate(&desc, &buffer)) {
    LOGE("no %u x %u buffer for image %p", desc.width, desc.height, (void *)b->image);
    return NULL;
  }
  return buffer;
}

// Imports buffer into device as the memory of a new image of its shape, bound to it.
static VkResult import_mirror(VkDevice device, AHardwareBuffer *buffer, VkFormat format, VkImage *image,
                              VkDeviceMemory *memory) {
  AHardwareBuffer_Desc desc;
  AHardwareBuffer_describe(buffer, &desc);
  VkExternalMemoryImageCreateInfo external = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, AHB};
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external, 0, VK_IMAGE_TYPE_2D, format,
                            {desc.width, desc.height, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
  VkResult result = next.CreateImage(device, &info, NULL, image);
  if (result != VK_SUCCESS) return result;
  VkAndroidHardwareBufferPropertiesANDROID props = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
  result = next.GetAndroidHardwareBufferPropertiesANDROID
               ? next.GetAndroidHardwareBufferPropertiesANDROID(device, buffer, &props)
               : VK_ERROR_INVALID_EXTERNAL_HANDLE;
  if (result == VK_SUCCESS && !props.memoryTypeBits) result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
  if (result == VK_SUCCESS) {
    VkMemoryDedicatedAllocateInfo dedicated = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, *image};
    VkImportAndroidHardwareBufferInfoANDROID import = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
                                                       &dedicated, buffer};
    VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import, props.allocationSize,
                                     (uint32_t)__builtin_ctz(props.memoryTypeBits)};
    result = next.AllocateMemory(device, &allocate, NULL, memory);
  }
  if (result == VK_SUCCESS && (result = next.BindImageMemory(device, *image, *memory, 0)) != VK_SUCCESS)
    next.FreeMemory(device, *memory, NULL);
  if (result != VK_SUCCESS) next.DestroyImage(device, *image, NULL);
  return result;
}

// Memory for a backed image: its own, which only this process uses, and its mirror, made here
// (exported) or received from fd (imported).
static VkResult backed_memory_allocate(VkDevice device, const VkMemoryAllocateInfo *info,
                                       const VkAllocationCallbacks *allocator, VkDeviceMemory *memory,
                                       const Backed *want, int fd) {
  AHardwareBuffer *buffer = NULL;
  if (fd >= 0 && (AHardwareBuffer_recvHandleFromUnixSocket(fd, &buffer) != 0 || !buffer)) {
    LOGE("fd %d holds no AHardwareBuffer; only memory exported by Prism's driver can be imported", fd);
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  }
  if (fd < 0 && !(buffer = mirror_buffer(want, info->allocationSize))) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  VkMemoryDedicatedAllocateInfo dedicated = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, want->image};
  VkMemoryAllocateInfo own = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated, info->allocationSize,
                              info->memoryTypeIndex};
  VkResult result = next.AllocateMemory(device, &own, allocator, memory);
  if (result != VK_SUCCESS) {
    AHardwareBuffer_release(buffer);
    return result;
  }
  VkImage mirror = VK_NULL_HANDLE;
  VkDeviceMemory mirror_memory = VK_NULL_HANDLE;
  if (want->mirror_format != VK_FORMAT_UNDEFINED) {
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(buffer, &desc);
    VkResult imported = VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (desc.width == want->extent.width && desc.height == want->extent.height * want->layers)
      imported = import_mirror(device, buffer, want->mirror_format, &mirror, &mirror_memory);
    if (imported != VK_SUCCESS) {
      mirror = VK_NULL_HANDLE, mirror_memory = VK_NULL_HANDLE;
      LOGE("image %p: no mirror from a %u x %u buffer (%d); its contents aren't shared", (void *)want->image,
           desc.width, desc.height, imported);
    }
  }
  pthread_mutex_lock(&g_lock);
  Backed *b = backed_image(want->image);
  if (b) {
    b->memory = *memory, b->buffer = buffer;
    b->mirror = mirror, b->mirror_memory = mirror_memory, b->mirror_general = 0;
  }
  pthread_mutex_unlock(&g_lock);
  if (!b) {  // destroyed meanwhile
    if (mirror != VK_NULL_HANDLE) next.DestroyImage(device, mirror, NULL);
    if (mirror_memory != VK_NULL_HANDLE) next.FreeMemory(device, mirror_memory, NULL);
    AHardwareBuffer_release(buffer);
  }
  if (fd >= 0) close(fd);  // a successful import owns the fd
  return VK_SUCCESS;
}

// Memory imported from an fd without naming a backed image: its buffer, in case a backed image is
// bound to it (the compositor imports the memory of each swapchain image it exports, and binds a
// second image to it).
#define MAX_IMPORTS 256
static struct {
  VkDeviceMemory memory;
  AHardwareBuffer *buffer;
} g_imports[MAX_IMPORTS];

static AHardwareBuffer *take_import(VkDeviceMemory memory) {
  for (int i = 0; i < MAX_IMPORTS; i++) {
    if (g_imports[i].memory != memory || !g_imports[i].buffer) continue;
    AHardwareBuffer *buffer = g_imports[i].buffer;
    g_imports[i].memory = VK_NULL_HANDLE, g_imports[i].buffer = NULL;
    return buffer;
  }
  return NULL;
}

static VkResult VKAPI_CALL prism_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
                                                const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
  const VkExportMemoryAllocateInfo *export = find_in(info, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
  const VkImportMemoryFdInfoKHR *import = find_in(info, VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
  int exporting = export && (export->handleTypes & OPAQUE_FD);
  int importing = import && import->handleType == OPAQUE_FD;
  if (!exporting && !importing) return next.AllocateMemory(device, info, allocator, memory);

  const VkMemoryDedicatedAllocateInfo *dedicated = find_in(info, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
  Backed want = {VK_NULL_HANDLE};
  if (dedicated && dedicated->image != VK_NULL_HANDLE && tracking()) {
    pthread_mutex_lock(&g_lock);
    Backed *b = backed_image(dedicated->image);
    if (b) want = *b;
    pthread_mutex_unlock(&g_lock);
  }
  if (want.image != VK_NULL_HANDLE)
    return backed_memory_allocate(device, info, allocator, memory, &want, importing ? import->fd : -1);

  VkMemoryAllocateInfo copy = *info;
  Scratch scratch = {.used = 0};
  VkExportMemoryAllocateInfo export_ahb;
  if (exporting) {
    export_ahb = *export;
    export_ahb.handleTypes = as_ahb(export->handleTypes);
    replace_in_chain(&copy, &export_ahb, &scratch);
  }
  AHardwareBuffer *buffer = NULL;
  if (importing && (AHardwareBuffer_recvHandleFromUnixSocket(import->fd, &buffer) != 0 || !buffer)) {
    LOGE("fd %d holds no AHardwareBuffer; only memory exported by Prism's driver can be imported", import->fd);
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  }
  VkImportAndroidHardwareBufferInfoANDROID import_ahb = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
  if (buffer) {
    VkAndroidHardwareBufferPropertiesANDROID props = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    VkResult result = next.GetAndroidHardwareBufferPropertiesANDROID
                          ? next.GetAndroidHardwareBufferPropertiesANDROID(device, buffer, &props)
                          : VK_ERROR_INVALID_EXTERNAL_HANDLE;
    if (result != VK_SUCCESS || !props.memoryTypeBits) {
      AHardwareBuffer_release(buffer);
      return result != VK_SUCCESS ? result : VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    // gfxstream may not know the size of a buffer it didn't allocate (0); the caller's is the image's.
    if (props.allocationSize) copy.allocationSize = props.allocationSize;
    if (!(props.memoryTypeBits & (1u << copy.memoryTypeIndex))) copy.memoryTypeIndex = __builtin_ctz(props.memoryTypeBits);
    import_ahb.buffer = buffer;
    replace_in_chain(&copy, &import_ahb, &scratch);
  }
  VkResult result = next.AllocateMemory(device, &copy, allocator, memory);
  if (result == VK_SUCCESS && importing) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_IMPORTS; i++) {
      if (g_imports[i].buffer) continue;
      AHardwareBuffer_acquire(buffer);
      g_imports[i].memory = *memory, g_imports[i].buffer = buffer;
      break;
    }
    pthread_mutex_unlock(&g_lock);
  }
  if (buffer) AHardwareBuffer_release(buffer);  // the memory holds its own reference
  if (importing && result == VK_SUCCESS) close(import->fd);  // a successful import owns the fd
  return result;
}

static void VKAPI_CALL prism_FreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *allocator) {
  Backed gone = {VK_NULL_HANDLE};
  if (memory != VK_NULL_HANDLE) {
    pthread_mutex_lock(&g_lock);
    gone.buffer = take_import(memory);
    Backed *b = backed_memory(memory);
    if (b) {
      if (gone.buffer) AHardwareBuffer_release(gone.buffer);
      gone = *b;
      b->memory = VK_NULL_HANDLE, b->buffer = NULL, b->mirror = VK_NULL_HANDLE, b->mirror_memory = VK_NULL_HANDLE;
      for (int i = 0; i < MAX_BACKED; i++)
        if (g_backed[i].owner == b) g_backed[i].owner = NULL;
      release_backed(b);
    }
    pthread_mutex_unlock(&g_lock);
  }
  if (gone.mirror != VK_NULL_HANDLE) next.DestroyImage(device, gone.mirror, NULL);
  if (gone.mirror_memory != VK_NULL_HANDLE) next.FreeMemory(device, gone.mirror_memory, NULL);
  if (gone.own != VK_NULL_HANDLE) next.FreeMemory(device, gone.own, NULL);
  if (gone.buffer) AHardwareBuffer_release(gone.buffer);
  next.FreeMemory(device, memory, allocator);
}

// Memory imported from a mirror's buffer is the mirror's memory on the host, whatever image is bound
// to it, and the mirror is an image of another shape: a layered image bound there would share its
// first layer with the mirror's top rows and scramble the rest with every copy between them. So a
// backed image bound to such memory (the compositor binds a second image to the memory of each
// swapchain image it exports) is bound to memory of its own instead, returned in own; the import
// only holds the mirror.
static VkDeviceMemory memory_to_bind(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceMemory *own) {
  *own = VK_NULL_HANDLE;
  if (!tracking() || !next.GetImageMemoryRequirements) return memory;
  pthread_mutex_lock(&g_lock);
  Backed *b = backed_image(image);
  int imported = 0;
  for (int i = 0; b && b->mirror_format != VK_FORMAT_UNDEFINED && b->memory == VK_NULL_HANDLE && i < MAX_IMPORTS; i++)
    imported |= g_imports[i].memory == memory && g_imports[i].buffer;
  pthread_mutex_unlock(&g_lock);
  if (!imported) return memory;
  VkMemoryRequirements req;
  next.GetImageMemoryRequirements(device, image, &req);
  uint32_t type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type == g_memory_properties.memoryTypeCount) type = memory_type(req.memoryTypeBits, 0);
  VkMemoryDedicatedAllocateInfo dedicated = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, image};
  VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated, req.size, type};
  if (type == g_memory_properties.memoryTypeCount || next.AllocateMemory(device, &allocate, NULL, own) != VK_SUCCESS) {
    LOGE("image %p: no memory of its own; bound to its mirror's, its layers won't survive copies", (void *)image);
    *own = VK_NULL_HANDLE;
    return memory;
  }
  return *own;
}

// A backed image bound to another's memory (the compositor binds a second image to the memory of
// each swapchain image it exports) aliases that image here, and shares its mirror.
static void bound(VkImage image, VkDeviceMemory memory, VkDeviceMemory own) {
  if (!tracking()) return;
  pthread_mutex_lock(&g_lock);
  Backed *b = backed_image(image), *owner = backed_memory(memory);
  if (b && owner && owner != b && b->memory == VK_NULL_HANDLE) {
    b->owner = owner;
    LOGI("image %p: bound to the memory of image %p, shares its mirror", (void *)image, (void *)owner->image);
  } else if (b && !owner && b->memory == VK_NULL_HANDLE) {
    // Imported memory: its buffer is the exporter's mirror, which this image gets too. The image
    // itself, this process's copy, is in own (see memory_to_bind).
    AHardwareBuffer *buffer = take_import(memory);
    VkImage mirror = VK_NULL_HANDLE;
    VkDeviceMemory mirror_memory = VK_NULL_HANDLE;
    VkResult result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    if (buffer && b->mirror_format != VK_FORMAT_UNDEFINED) {
      AHardwareBuffer_Desc desc;
      AHardwareBuffer_describe(buffer, &desc);
      if (desc.width == b->extent.width && desc.height == b->extent.height * b->layers)
        result = import_mirror(b->device, buffer, b->mirror_format, &mirror, &mirror_memory);
    }
    if (buffer) {
      b->memory = memory, b->buffer = buffer, b->own = own;  // freed with the memory
      if (result == VK_SUCCESS) b->mirror = mirror, b->mirror_memory = mirror_memory, b->mirror_general = 0;
    }
    if (result == VK_SUCCESS)
      LOGI("image %p: bound to imported memory, shares its mirror", (void *)image);
    else
      LOGE("image %p: bound to memory with no mirror for it (%d); its contents aren't shared", (void *)image, result);
  }
  pthread_mutex_unlock(&g_lock);
}

static VkResult VKAPI_CALL prism_BindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
                                                 VkDeviceSize offset) {
  VkDeviceMemory own;
  VkDeviceMemory to = memory_to_bind(device, image, memory, &own);
  VkResult result = next.BindImageMemory(device, image, to, own != VK_NULL_HANDLE ? 0 : offset);
  if (result == VK_SUCCESS)
    bound(image, memory, own);
  else if (own != VK_NULL_HANDLE)
    next.FreeMemory(device, own, NULL);
  return result;
}

static VkResult bind_image_memory2(PFN_vkBindImageMemory2 down, VkDevice device, uint32_t count,
                                   const VkBindImageMemoryInfo *infos) {
  if (!count) return down(device, count, infos);
  VkBindImageMemoryInfo copy[count];
  VkDeviceMemory own[count];
  for (uint32_t i = 0; i < count; i++) {
    copy[i] = infos[i];
    copy[i].memory = memory_to_bind(device, infos[i].image, infos[i].memory, &own[i]);
    if (own[i] != VK_NULL_HANDLE) copy[i].memoryOffset = 0;
  }
  VkResult result = down(device, count, copy);
  for (uint32_t i = 0; i < count; i++) {
    if (result == VK_SUCCESS)
      bound(infos[i].image, infos[i].memory, own[i]);
    else if (own[i] != VK_NULL_HANDLE)
      next.FreeMemory(device, own[i], NULL);
  }
  return result;
}

static VkResult VKAPI_CALL prism_BindImageMemory2(VkDevice device, uint32_t count, const VkBindImageMemoryInfo *infos) {
  return bind_image_memory2(next.BindImageMemory2, device, count, infos);
}

static VkResult VKAPI_CALL prism_BindImageMemory2KHR(VkDevice device, uint32_t count,
                                                     const VkBindImageMemoryInfo *infos) {
  return bind_image_memory2(next.BindImageMemory2KHR, device, count, infos);
}

static VkResult VKAPI_CALL prism_AcquireImageANDROID(VkDevice device, VkImage image, int fence_fd, VkSemaphore semaphore,
                                                     VkFence fence) {
  __atomic_add_fetch(&g_acquires, 1, __ATOMIC_RELAXED);
  TRACE("acquire %p", (void *)image);
  return ((PFN_AcquireImageANDROID)next.AcquireImageANDROID)(device, image, fence_fd, semaphore, fence);
}

static VkResult VKAPI_CALL prism_QueueSignalReleaseImageANDROID(VkQueue queue, uint32_t count, const VkSemaphore *waits,
                                                                VkImage image, int *fence_fd) {
  __atomic_add_fetch(&g_releases, 1, __ATOMIC_RELAXED);
  TRACE("present %p after %u waits", (void *)image, count);
  return ((PFN_QueueSignalReleaseImageANDROID)next.QueueSignalReleaseImageANDROID)(queue, count, waits, image, fence_fd);
}

static VkResult VKAPI_CALL prism_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                        const VkGraphicsPipelineCreateInfo *infos,
                                                        const VkAllocationCallbacks *allocator, VkPipeline *pipelines) {
  VkResult result = next.CreateGraphicsPipelines(device, cache, count, infos, allocator, pipelines);
  static unsigned logged;
  for (uint32_t i = 0; i < count && within(&logged, 96); i++) {
    const VkGraphicsPipelineCreateInfo *p = &infos[i];
    const VkPipelineRasterizationStateCreateInfo *r = p->pRasterizationState;
    const VkPipelineColorBlendStateCreateInfo *c = p->pColorBlendState;
    const VkPipelineViewportStateCreateInfo *v = p->pViewportState;
    const VkPipelineDepthStencilStateCreateInfo *d = p->pDepthStencilState;
    const VkPipelineColorBlendAttachmentState *a = c && c->attachmentCount ? c->pAttachments : NULL;
    void *made = result == VK_SUCCESS ? (void *)pipelines[i] : NULL;
    unsigned dynamic = 0;  // bit 0: viewport, 1: scissor
    for (uint32_t k = 0; p->pDynamicState && k < p->pDynamicState->dynamicStateCount; k++) {
      VkDynamicState state = p->pDynamicState->pDynamicStates[k];
      if (state == VK_DYNAMIC_STATE_VIEWPORT || state == VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT) dynamic |= 1;
      if (state == VK_DYNAMIC_STATE_SCISSOR || state == VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT) dynamic |= 2;
    }
    LOGI("pipeline %p: result %d, pass %p/%u, %u stages, %u viewports (dynamic %u), discard %d, cull %#x, front %d, "
         "depth test %d write %d op %d, %u blends: enable %d, write mask %#x, color %d %d %d, alpha %d %d %d",
         made, result, (void *)p->renderPass, p->subpass, p->stageCount, v ? v->viewportCount : 0, dynamic,
         r ? r->rasterizerDiscardEnable : -1, r ? r->cullMode : 0, r ? r->frontFace : -1,
         d ? d->depthTestEnable : -1, d ? d->depthWriteEnable : -1, d ? d->depthCompareOp : -1,
         c ? c->attachmentCount : 0, a ? a->blendEnable : -1, a ? a->colorWriteMask : 0,
         a ? a->srcColorBlendFactor : -1, a ? a->dstColorBlendFactor : -1, a ? a->colorBlendOp : -1,
         a ? a->srcAlphaBlendFactor : -1, a ? a->dstAlphaBlendFactor : -1, a ? a->alphaBlendOp : -1);
    if (v && v->pViewports && !(dynamic & 1))
      LOGI("pipeline %p: viewport %g,%g %gx%g", made, v->pViewports[0].x, v->pViewports[0].y, v->pViewports[0].width,
           v->pViewports[0].height);
  }
  return result;
}

static void VKAPI_CALL prism_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline) {
  TRACE("cb %p: bind pipeline %p (point %d)", (void *)cb, (void *)pipeline, point);
  next.CmdBindPipeline(cb, point, pipeline);
}

static void VKAPI_CALL prism_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                           const VkViewport *viewports) {
  for (uint32_t i = 0; i < count; i++)
    TRACE("cb %p: viewport %u: %g,%g %gx%g, depth %g-%g", (void *)cb, first + i, viewports[i].x, viewports[i].y,
          viewports[i].width, viewports[i].height, viewports[i].minDepth, viewports[i].maxDepth);
  VkViewport fitted[MAX_PANELS];
  next.CmdSetViewport(cb, first, count, first == 0 && panel_fit(cb, count, viewports, fitted, NULL, NULL) ? fitted : viewports);
}

static void VKAPI_CALL prism_CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkRect2D *scissors) {
  for (uint32_t i = 0; i < count; i++)
    TRACE("cb %p: scissor %u: %d,%d %ux%u", (void *)cb, first + i, scissors[i].offset.x, scissors[i].offset.y,
          scissors[i].extent.width, scissors[i].extent.height);
  VkRect2D fitted[MAX_PANELS];
  next.CmdSetScissor(cb, first, count, panel_fit(cb, count, NULL, NULL, scissors, fitted) ? fitted : scissors);
}

static void VKAPI_CALL prism_CmdDraw(VkCommandBuffer cb, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                                    uint32_t first_instance) {
  TRACE("cb %p: draw %u vertices, %u instances", (void *)cb, vertices, instances);
  next.CmdDraw(cb, vertices, instances, first_vertex, first_instance);
}

static void VKAPI_CALL prism_CmdDrawIndexed(VkCommandBuffer cb, uint32_t indices, uint32_t instances, uint32_t first_index,
                                           int32_t vertex_offset, uint32_t first_instance) {
  TRACE("cb %p: draw %u indices, %u instances", (void *)cb, indices, instances);
  next.CmdDrawIndexed(cb, indices, instances, first_index, vertex_offset, first_instance);
}

static void VKAPI_CALL prism_CmdClearAttachments(VkCommandBuffer cb, uint32_t count, const VkClearAttachment *attachments,
                                                uint32_t rects, const VkClearRect *rect) {
  TRACE("cb %p: clear %u attachments in %u rects (first %d,%d %ux%u)", (void *)cb, count, rects,
        rects ? rect->rect.offset.x : 0, rects ? rect->rect.offset.y : 0, rects ? rect->rect.extent.width : 0,
        rects ? rect->rect.extent.height : 0);
  next.CmdClearAttachments(cb, count, attachments, rects, rect);
}

static void VKAPI_CALL prism_CmdEndRenderPass(VkCommandBuffer cb) {
  TRACE("cb %p: end pass", (void *)cb);
  panel_end(cb);
  next.CmdEndRenderPass(cb);
}

static void VKAPI_CALL prism_CmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo *end) {
  TRACE("cb %p: end pass", (void *)cb);
  panel_end(cb);
  next.CmdEndRenderPass2(cb, end);
}

static void VKAPI_CALL prism_CmdEndRenderPass2KHR(VkCommandBuffer cb, const VkSubpassEndInfo *end) {
  TRACE("cb %p: end pass", (void *)cb);
  panel_end(cb);
  next.CmdEndRenderPass2KHR(cb, end);
}

#define EXPORT_COPIES 4

static VkResult VKAPI_CALL prism_GetMemoryFdKHR(VkDevice device, const VkMemoryGetFdInfoKHR *info, int *fd) {
  if (info->handleType != OPAQUE_FD || !next.GetMemoryAndroidHardwareBufferANDROID)
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  VkMemoryGetAndroidHardwareBufferInfoANDROID get = {VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
                                                     NULL, info->memory};
  AHardwareBuffer *buffer = NULL;
  if (tracking()) {
    pthread_mutex_lock(&g_lock);
    Backed *b = backed_memory(info->memory);
    if (b && (buffer = b->buffer)) AHardwareBuffer_acquire(buffer);
    pthread_mutex_unlock(&g_lock);
  }
  VkResult result = buffer ? VK_SUCCESS : next.GetMemoryAndroidHardwareBufferANDROID(device, &get, &buffer);
  if (result != VK_SUCCESS) {
    LOGE("vkGetMemoryFdKHR: memory has no AHardwareBuffer (%d)", result);
    return result;
  }
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair)) {
    AHardwareBuffer_release(buffer);
    return VK_ERROR_TOO_MANY_OBJECTS;
  }
  int sent = 0;
  for (int i = 0; i < EXPORT_COPIES && !sent; i++) sent = AHardwareBuffer_sendHandleToUnixSocket(buffer, pair[0]);
  close(pair[0]);  // what was sent stays readable at the other end
  AHardwareBuffer_release(buffer);
  if (sent) {
    close(pair[1]);
    LOGE("vkGetMemoryFdKHR: sending the AHardwareBuffer failed (%d)", sent);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  *fd = pair[1];
  return VK_SUCCESS;
}

static VkResult VKAPI_CALL prism_GetMemoryFdPropertiesKHR(VkDevice device, VkExternalMemoryHandleTypeFlagBits type,
                                                          int fd, VkMemoryFdPropertiesKHR *props) {
  (void)device, (void)type, (void)fd, (void)props;
  return VK_ERROR_INVALID_EXTERNAL_HANDLE;  // only for dma-bufs, which Prism doesn't offer
}

// --- dispatch ------------------------------------------------------------------------------------

typedef struct {
  const char *name;
  PFN_vkVoidFunction fn;
  PFN_vkVoidFunction *down;  // NULL: Prism's alone, offered whatever the driver has
} Hook;

static PFN_vkVoidFunction VKAPI_CALL prism_GetInstanceProcAddr(VkInstance instance, const char *name);
static PFN_vkVoidFunction VKAPI_CALL prism_GetDeviceProcAddr(VkDevice device, const char *name);

#define HOOK(name) {"vk" #name, (PFN_vkVoidFunction)prism_##name, (PFN_vkVoidFunction *)&next.name}
#define OWN(name) {"vk" #name, (PFN_vkVoidFunction)prism_##name, NULL}
static const Hook kHooks[] = {
    HOOK(GetInstanceProcAddr),
    HOOK(GetDeviceProcAddr),
    HOOK(EnumerateDeviceExtensionProperties),
    HOOK(CreateDevice),
    HOOK(GetPhysicalDeviceImageFormatProperties2),
    HOOK(GetPhysicalDeviceImageFormatProperties2KHR),
    HOOK(GetPhysicalDeviceExternalBufferProperties),
    HOOK(GetPhysicalDeviceExternalBufferPropertiesKHR),
    HOOK(CreateImage),
    HOOK(DestroyImage),
    HOOK(FreeMemory),
    HOOK(CreateBuffer),
    HOOK(AllocateMemory),
    HOOK(BindImageMemory),
    HOOK(BindImageMemory2),
    HOOK(BindImageMemory2KHR),
    HOOK(CreateImageView),
    HOOK(DestroyImageView),
    HOOK(CreateRenderPass),
    HOOK(CreateRenderPass2),
    HOOK(CreateRenderPass2KHR),
    HOOK(DestroyRenderPass),
    HOOK(CreateFramebuffer),
    HOOK(DestroyFramebuffer),
    HOOK(UpdateDescriptorSets),
    HOOK(AllocateDescriptorSets),
    HOOK(FreeDescriptorSets),
    HOOK(ResetDescriptorPool),
    HOOK(DestroyDescriptorPool),
    HOOK(GetDeviceQueue),
    HOOK(GetDeviceQueue2),
    HOOK(BeginCommandBuffer),
    HOOK(ResetCommandBuffer),
    HOOK(FreeCommandBuffers),
    HOOK(CmdPipelineBarrier),
    HOOK(CmdPipelineBarrier2),
    HOOK(CmdPipelineBarrier2KHR),
    HOOK(CmdWaitEvents),
    HOOK(CmdBeginRenderPass),
    HOOK(CmdBeginRenderPass2),
    HOOK(CmdBeginRenderPass2KHR),
    HOOK(CmdClearColorImage),
    HOOK(CmdCopyImage),
    HOOK(CmdBlitImage),
    HOOK(CmdResolveImage),
    HOOK(CmdCopyBufferToImage),
    HOOK(CmdCopyImageToBuffer),
    HOOK(CmdExecuteCommands),
    HOOK(CmdBindDescriptorSets),
    HOOK(QueueSubmit),
    HOOK(AcquireImageANDROID),
    HOOK(CreateGraphicsPipelines),
    HOOK(CmdBindPipeline),
    HOOK(CmdSetViewport),
    HOOK(CmdSetScissor),
    HOOK(CmdDraw),
    HOOK(CmdDrawIndexed),
    HOOK(CmdClearAttachments),
    HOOK(CmdEndRenderPass),
    HOOK(CmdEndRenderPass2),
    HOOK(CmdEndRenderPass2KHR),
    HOOK(QueueSignalReleaseImageANDROID),
    OWN(GetMemoryFdKHR),
    OWN(GetMemoryFdPropertiesKHR),
};

static PFN_vkVoidFunction hook(const char *name, PFN_vkVoidFunction down) {
  if (!name) return down;
  for (size_t i = 0; i < sizeof kHooks / sizeof *kHooks; i++) {
    const Hook *h = &kHooks[i];
    if (strcmp(name, h->name)) continue;
    if (!h->down) return h->fn;
    if (!down) return NULL;
    if (h->down != (PFN_vkVoidFunction *)&next.GetInstanceProcAddr)
      __atomic_store_n(h->down, down, __ATOMIC_RELEASE);
    return h->fn;
  }
  return down;
}

static PFN_vkVoidFunction VKAPI_CALL prism_GetInstanceProcAddr(VkInstance instance, const char *name) {
  return hook(name, next.GetInstanceProcAddr(instance, name));
}

static PFN_vkVoidFunction VKAPI_CALL prism_GetDeviceProcAddr(VkDevice device, const char *name) {
  return hook(name, next.GetDeviceProcAddr(device, name));
}

// --- HAL module ----------------------------------------------------------------------------------

static hwvulkan_device_t g_device;
static PFN_vkCreateInstance g_create_instance;

// The driver's instance, and the physical-device queries Prism makes itself.
static VkResult VKAPI_CALL prism_CreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
                                                VkInstance *instance) {
  VkResult result = g_create_instance(info, allocator, instance);
  if (result == VK_SUCCESS)
    LOGI("instance %p: API %#x", (void *)*instance, info->pApplicationInfo ? info->pApplicationInfo->apiVersion : 0);
  if (result == VK_SUCCESS) {
    next.GetPhysicalDeviceProperties =
        (PFN_vkGetPhysicalDeviceProperties)next.GetInstanceProcAddr(*instance, "vkGetPhysicalDeviceProperties");
    next.GetPhysicalDeviceFeatures2 =
        (PFN_vkGetPhysicalDeviceFeatures2)next.GetInstanceProcAddr(*instance, "vkGetPhysicalDeviceFeatures2");
    next.GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)next.GetInstanceProcAddr(
        *instance, "vkGetPhysicalDeviceMemoryProperties");
  }
  return result;
}
static int g_open_result = -ENOENT;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static hw_module_t *g_module;

static int prism_close(hw_device_t *device) {
  (void)device;
  return 0;
}

static void open_driver(void) {
  char driver[PROP_VALUE_MAX] = "";
  __system_property_get("ro.boot.hardware.vulkan", driver);  // the emulator's choice
  if (!driver[0] || !strcmp(driver, "prism")) strcpy(driver, "ranchu");
  char path[PROP_VALUE_MAX + 32];
  snprintf(path, sizeof path, "/vendor/lib64/hw/vulkan.%s.so", driver);
  void *so = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  hwvulkan_module_t *module = so ? dlsym(so, "HMI") : NULL;
  if (!module) {
    LOGE("can't load %s: %s", path, dlerror());
    return;
  }
  hw_device_t *opened = NULL;
  g_open_result = module->common.methods->open(&module->common, HWVULKAN_DEVICE_0, &opened);
  if (g_open_result) {
    LOGE("%s: open failed (%d)", path, g_open_result);
    return;
  }
  const hwvulkan_device_t *down = (const hwvulkan_device_t *)opened;
  next.GetInstanceProcAddr = down->GetInstanceProcAddr;
  g_device.common = down->common;
  g_device.common.module = g_module;
  g_device.common.close = prism_close;
  g_device.EnumerateInstanceExtensionProperties = down->EnumerateInstanceExtensionProperties;
  g_create_instance = down->CreateInstance;
  g_device.CreateInstance = prism_CreateInstance;
  g_device.GetInstanceProcAddr = prism_GetInstanceProcAddr;
  LOGI("wrapping %s", path);
}

static int prism_open(const hw_module_t *module, const char *id, hw_device_t **device) {
  if (strcmp(id, HWVULKAN_DEVICE_0)) return -ENOENT;
  g_module = (hw_module_t *)module;
  pthread_once(&g_once, open_driver);
  if (g_open_result) return g_open_result;
  *device = &g_device.common;
  return 0;
}

static hw_module_methods_t g_methods = {.open = prism_open};

__attribute__((visibility("default"))) hwvulkan_module_t HMI = {
    .common = {
        .tag = HW_TAG('H', 'W', 'M', 'T'),
        .module_api_version = 0x0001,  // HWVULKAN_MODULE_API_VERSION_0_1
        .hal_api_version = 0x0100,     // HARDWARE_HAL_API_VERSION
        .id = "vulkan",
        .name = "Prism Vulkan (gfxstream with opaque-fd memory)",
        .author = "Prism",
        .methods = &g_methods,
    },
};
