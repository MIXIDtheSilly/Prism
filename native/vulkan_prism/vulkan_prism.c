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
// An exported fd is good for one import, as Vulkan's are (importing transfers its ownership).
// Dispatchable handles are the driver's own, so the loader's dispatch works as it does with gfxstream
// alone; Prism only intercepts functions by name.

#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
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
  PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2;
  PFN_vkGetPhysicalDeviceImageFormatProperties2KHR GetPhysicalDeviceImageFormatProperties2KHR;
  PFN_vkGetPhysicalDeviceExternalBufferProperties GetPhysicalDeviceExternalBufferProperties;
  PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR GetPhysicalDeviceExternalBufferPropertiesKHR;
  PFN_vkCreateImage CreateImage;
  PFN_vkCreateBuffer CreateBuffer;
  PFN_vkAllocateMemory AllocateMemory;
  PFN_vkGetMemoryAndroidHardwareBufferANDROID GetMemoryAndroidHardwareBufferANDROID;
  PFN_vkGetAndroidHardwareBufferPropertiesANDROID GetAndroidHardwareBufferPropertiesANDROID;
} next;

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

static VkResult VKAPI_CALL prism_EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer,
                                                                    uint32_t *count, VkExtensionProperties *props) {
  if (layer) return next.EnumerateDeviceExtensionProperties(pd, layer, count, props);
  uint32_t n = 0;
  VkResult result = next.EnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
  if (result != VK_SUCCESS) return result;
  VkExtensionProperties *all = calloc(n + 1, sizeof *all);
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
    if (memory_fd) LOGI("device %p: %s backed by AHardwareBuffers", (void *)*device, kMemoryFd);
  }
  return result;
}

// --- device --------------------------------------------------------------------------------------

static VkResult VKAPI_CALL prism_CreateImage(VkDevice device, const VkImageCreateInfo *info,
                                             const VkAllocationCallbacks *allocator, VkImage *image) {
  const VkExternalMemoryImageCreateInfo *external = find_in(info, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
  if (!external || !(external->handleTypes & OPAQUE_FD)) return next.CreateImage(device, info, allocator, image);
  VkImageCreateInfo copy = *info;
  VkExternalMemoryImageCreateInfo ahb = *external;
  ahb.handleTypes = as_ahb(external->handleTypes);
  Scratch scratch = {.used = 0};
  replace_in_chain(&copy, &ahb, &scratch);
  return next.CreateImage(device, &copy, allocator, image);
}

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

static VkResult VKAPI_CALL prism_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
                                                const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
  const VkExportMemoryAllocateInfo *export = find_in(info, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
  const VkImportMemoryFdInfoKHR *import = find_in(info, VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
  int exporting = export && (export->handleTypes & OPAQUE_FD);
  int importing = import && import->handleType == OPAQUE_FD;
  if (!exporting && !importing) return next.AllocateMemory(device, info, allocator, memory);

  VkMemoryAllocateInfo copy = *info;
  Scratch scratch = {.used = 0};
  VkExportMemoryAllocateInfo export_ahb;
  if (exporting) {
    export_ahb = *export;
    export_ahb.handleTypes = as_ahb(export->handleTypes);
    replace_in_chain(&copy, &export_ahb, &scratch);
  }
  AHardwareBuffer *buffer = NULL;
  VkImportAndroidHardwareBufferInfoANDROID import_ahb = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
  if (importing) {
    if (!next.GetAndroidHardwareBufferPropertiesANDROID ||
        AHardwareBuffer_recvHandleFromUnixSocket(import->fd, &buffer) != 0 || !buffer) {
      LOGE("fd %d holds no AHardwareBuffer; only memory exported by Prism's driver can be imported", import->fd);
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    VkAndroidHardwareBufferPropertiesANDROID props = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    VkResult result = next.GetAndroidHardwareBufferPropertiesANDROID(device, buffer, &props);
    if (result != VK_SUCCESS || !props.memoryTypeBits) {
      AHardwareBuffer_release(buffer);
      return result != VK_SUCCESS ? result : VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    copy.allocationSize = props.allocationSize;
    if (!(props.memoryTypeBits & (1u << copy.memoryTypeIndex))) copy.memoryTypeIndex = __builtin_ctz(props.memoryTypeBits);
    import_ahb.buffer = buffer;
    replace_in_chain(&copy, &import_ahb, &scratch);
  }
  VkResult result = next.AllocateMemory(device, &copy, allocator, memory);
  if (buffer) {
    AHardwareBuffer_release(buffer);  // the imported memory holds its own reference
    if (result == VK_SUCCESS) close(import->fd);  // a successful import owns the fd
  }
  return result;
}

static VkResult VKAPI_CALL prism_GetMemoryFdKHR(VkDevice device, const VkMemoryGetFdInfoKHR *info, int *fd) {
  if (info->handleType != OPAQUE_FD || !next.GetMemoryAndroidHardwareBufferANDROID)
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  VkMemoryGetAndroidHardwareBufferInfoANDROID get = {VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
                                                     NULL, info->memory};
  AHardwareBuffer *buffer = NULL;
  VkResult result = next.GetMemoryAndroidHardwareBufferANDROID(device, &get, &buffer);
  if (result != VK_SUCCESS) {
    LOGE("vkGetMemoryFdKHR: memory has no AHardwareBuffer (%d)", result);
    return result;
  }
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair)) {
    AHardwareBuffer_release(buffer);
    return VK_ERROR_TOO_MANY_OBJECTS;
  }
  int sent = AHardwareBuffer_sendHandleToUnixSocket(buffer, pair[0]);
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
    HOOK(CreateBuffer),
    HOOK(AllocateMemory),
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
  g_device.CreateInstance = down->CreateInstance;
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
