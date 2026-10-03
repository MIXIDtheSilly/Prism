// libpcx.so: what the Android 16 build of Digitalis imports that Android 14's x86_64 libraries
// lack. tools/translator.py points each Digitalis host binary's DT_NEEDED "libc++.so" here; this
// library in turn needs the real libc++.so, so everything else still resolves as before.

#include <android/log.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>

#define TAG "PrismBerberisCompat"

// libc++ 16+: called on a failed hardening assertion. Android 14's libc++ predates it.
__attribute__((noreturn, visibility("default"))) void prism_libcpp_verbose_abort(const char *format, ...)
    __asm__("_ZNSt3__122__libcpp_verbose_abortEPKcz");

void prism_libcpp_verbose_abort(const char *format, ...) {
  va_list args;
  va_start(args, format);
  __android_log_vprint(ANDROID_LOG_FATAL, TAG, format, args);
  va_end(args);
  abort();
}

// std::filesystem. Android 14 links it statically into its users (libc++fs) rather than exporting
// it from libc++.so; Berberis uses three entry points to resolve paths. POSIX semantics only: no
// root names. Layouts are libc++'s stable ABI: a path is one std::string (short form: size << 1
// in byte 0, chars from byte 1; long form: bit 0 set, size at +8, data at +16).
struct string_view {
  const char *data;
  size_t size;
};

static struct string_view path_string(const void *path) {
  const unsigned char *s = path;
  if (s[0] & 1) return (struct string_view){*(const char *const *)(s + 16), *(const size_t *)(s + 8)};
  return (struct string_view){(const char *)s + 1, s[0] >> 1};
}

// path::__root_directory() const: the leading separator, if any.
__attribute__((visibility("default"))) struct string_view prism_path_root_directory(const void *path)
    __asm__("_ZNKSt3__14__fs10filesystem4path16__root_directoryEv");

struct string_view prism_path_root_directory(const void *path) {
  struct string_view p = path_string(path);
  if (p.size && p.data[0] == '/') return (struct string_view){p.data, 1};
  return (struct string_view){NULL, 0};
}

// path::__filename() const: the last element; empty for a root-only path, "" after a trailing
// separator.
__attribute__((visibility("default"))) struct string_view prism_path_filename(const void *path)
    __asm__("_ZNKSt3__14__fs10filesystem4path10__filenameEv");

struct string_view prism_path_filename(const void *path) {
  struct string_view p = path_string(path);
  size_t end = p.size, start;
  if (!end) return (struct string_view){NULL, 0};
  if (p.data[end - 1] == '/') {
    while (end && p.data[end - 1] == '/') end--;
    if (!end) return (struct string_view){NULL, 0};
    return (struct string_view){"", 0};
  }
  for (start = end; start && p.data[start - 1] != '/'; start--) {
  }
  return (struct string_view){p.data + start, end - start};
}

// __status(const path&, error_code*) -> file_status. file_status has a user-provided destructor,
// so it comes back through a hidden pointer: { int8 file_type; uint32 perms at +4 }.
struct file_status {
  int8_t type;
  uint32_t perms;
};
struct error_code {
  int value;
  const void *category;
};
enum { FT_NONE = 0, FT_NOT_FOUND = -1, FT_REGULAR = 1, FT_DIRECTORY = 2, FT_SYMLINK = 3, FT_BLOCK = 4,
       FT_CHARACTER = 5, FT_FIFO = 6, FT_SOCKET = 7, FT_UNKNOWN = 8 };
#define PERMS_UNKNOWN 0xFFFF

const void *prism_system_category(void) __asm__("_ZNSt3__115system_categoryEv");
const void *prism_generic_category(void) __asm__("_ZNSt3__116generic_categoryEv");

__attribute__((visibility("default"))) struct file_status *prism_fs_status(struct file_status *out, const void *path,
                                                                           struct error_code *ec)
    __asm__("_ZNSt3__14__fs10filesystem8__statusERKNS1_4pathEPNS_10error_codeE");

struct file_status *prism_fs_status(struct file_status *out, const void *path, struct error_code *ec) {
  struct stat st;
  // libc++ keeps paths NUL-terminated.
  if (stat(path_string(path).data, &st) == -1) {
    int error = errno;
    if (ec) *ec = (struct error_code){error, prism_generic_category()};
    *out = (struct file_status){error == ENOENT || error == ENOTDIR ? FT_NOT_FOUND : FT_NONE, PERMS_UNKNOWN};
    return out;
  }
  if (ec) *ec = (struct error_code){0, prism_system_category()};
  int8_t type = S_ISREG(st.st_mode)    ? FT_REGULAR
                : S_ISDIR(st.st_mode)  ? FT_DIRECTORY
                : S_ISLNK(st.st_mode)  ? FT_SYMLINK
                : S_ISBLK(st.st_mode)  ? FT_BLOCK
                : S_ISCHR(st.st_mode)  ? FT_CHARACTER
                : S_ISFIFO(st.st_mode) ? FT_FIFO
                : S_ISSOCK(st.st_mode) ? FT_SOCKET
                                       : FT_UNKNOWN;
  *out = (struct file_status){type, st.st_mode & 07777};
  return out;
}

// Vulkan 1.4 core commands, which the guest Vulkan proxy links directly. Android 14's loader is
// Vulkan 1.3; apps check the API version before using these, so they're never meant to run.
#define VK_ERROR_FEATURE_NOT_PRESENT (-8)

static void not_available(const char *name) {
  __android_log_print(ANDROID_LOG_ERROR, TAG, "%s called, but this Vulkan is 1.3", name);
}

#define VOID_COMMAND(name) \
  __attribute__((visibility("default"))) void name(void) { not_available(#name); }
#define RESULT_COMMAND(name)                                    \
  __attribute__((visibility("default"))) int32_t name(void) { \
    not_available(#name);                                       \
    return VK_ERROR_FEATURE_NOT_PRESENT;                        \
  }

VOID_COMMAND(vkCmdBindDescriptorSets2)
VOID_COMMAND(vkCmdPushConstants2)
VOID_COMMAND(vkCmdPushDescriptorSet)
VOID_COMMAND(vkCmdPushDescriptorSet2)
VOID_COMMAND(vkCmdPushDescriptorSetWithTemplate2)
VOID_COMMAND(vkCmdSetRenderingAttachmentLocations)
VOID_COMMAND(vkCmdSetRenderingInputAttachmentIndices)
VOID_COMMAND(vkGetDeviceImageSubresourceLayout)
VOID_COMMAND(vkGetImageSubresourceLayout2)
VOID_COMMAND(vkGetRenderingAreaGranularity)
RESULT_COMMAND(vkCopyImageToImage)
RESULT_COMMAND(vkCopyImageToMemory)
RESULT_COMMAND(vkCopyMemoryToImage)
RESULT_COMMAND(vkMapMemory2)
RESULT_COMMAND(vkTransitionImageLayout)
RESULT_COMMAND(vkUnmapMemory2)

// Camera NDK, API 36.
#define ACAMERA_ERROR_UNSUPPORTED_OPERATION (-10014)

__attribute__((visibility("default"))) int32_t ACameraManager_openSharedCamera(void) {
  not_available("ACameraManager_openSharedCamera");
  return ACAMERA_ERROR_UNSUPPORTED_OPERATION;
}
