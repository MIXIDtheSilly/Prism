// Patches to Horizon's arm64 libraries, made as they load (libprism_jni only).
//
// The translator loads an app's arm64 library from its own copy (berberis_extract/<name>.so), mapped
// from offset 0, and translates its code as it first runs. Right after the library loads, before
// any of its code has, Prism writes the instructions below into that mapping. Each patch names the
// bytes it expects there and is skipped, with a warning, when they differ: another Horizon build.
//
// * Frosted glass (libvrruntimeservice.so, Meta's compositor). Panels' glass backgrounds are the
//   compositor's: it blurs what's behind a glass layer. It sets glass up only on the headsets it
//   knows (device types 271-301) and when the oculus_xrruntime/oculus_frosted_glass gatekeeper is
//   on. Here the device type is 270, its emulator's: the runtime looks Build.MODEL ("Quest 3") up
//   in a table of codenames, finds nothing, and the hardware is ranchu. Claiming a real headset
//   instead would change much more than glass (GPU and format fallbacks, DRM, the HMD's
//   configuration), so the patch makes only the glass check's device test pass (sub w8, w8, #0x10f
//   becomes mov w8, wzr: the first known type); the gatekeeper still decides.

#ifndef PRISM_STUB_LIB

#include "prism_guest.h"

#include <android/log.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TAG "PrismJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

static const struct {
  const char *library;
  uint32_t offset;  // in the library's file, which is its address from its base
  uint8_t original[4], patched[4];
  const char *what;
} kPatches[] = {
    {"libvrruntimeservice.so", 0x6becfc, {0x08, 0x3d, 0x04, 0x51}, {0xe8, 0x03, 0x1f, 0x2a},
     "frosted glass on the emulator's device type"},
};

// The library's base: its mapping from offset 0 (the lowest, which holds its ELF header).
static uintptr_t library_base(const char *name) {
  FILE *maps = fopen("/proc/self/maps", "re");
  if (!maps) return 0;
  char line[640];
  uintptr_t base = 0;
  size_t n = strlen(name);
  while (!base && fgets(line, sizeof line, maps)) {
    unsigned long start, end, offset;
    char path[512] = "";
    if (sscanf(line, "%lx-%lx %*4s %lx %*s %*s %511s", &start, &end, &offset, path) < 4 || offset) continue;
    size_t length = strlen(path);
    if (length > n && path[length - n - 1] == '/' && !strcmp(path + length - n, name) &&
        !memcmp((const void *)start, "\177ELF", 4))
      base = start;
  }
  fclose(maps);
  return base;
}

static int write_code(uintptr_t address, const uint8_t *bytes, size_t size) {
  size_t page_size = (size_t)getpagesize();
  uintptr_t page = address & ~(uintptr_t)(page_size - 1);
  size_t span = ((address + size - page + page_size - 1) / page_size) * page_size;
  // Read-only to the host: the translator reads it, it never runs as x86_64.
  if (mprotect((void *)page, span, PROT_READ | PROT_WRITE)) return 0;
  memcpy((void *)address, bytes, size);
  mprotect((void *)page, span, PROT_READ);
  return 1;
}

void prism_patch_guest_library(const char *path) {
  const char *slash = path ? strrchr(path, '/') : NULL;
  const char *name = slash ? slash + 1 : path;
  if (!name) return;
  for (size_t i = 0; i < sizeof kPatches / sizeof *kPatches; i++) {
    if (strcmp(name, kPatches[i].library)) continue;
    uintptr_t base = library_base(name);
    if (!base) {
      LOGW("arm64 %s: not found in memory; %s not patched", name, kPatches[i].what);
      continue;
    }
    uint8_t *at = (uint8_t *)(base + kPatches[i].offset);
    if (!memcmp(at, kPatches[i].patched, 4)) continue;
    if (memcmp(at, kPatches[i].original, 4)) {
      LOGW("arm64 %s+%#x: %02x %02x %02x %02x, not the expected instruction (another build?); %s not patched", name,
           kPatches[i].offset, at[0], at[1], at[2], at[3], kPatches[i].what);
      continue;
    }
    if (write_code((uintptr_t)at, kPatches[i].patched, 4))
      LOGI("arm64 %s+%#x patched: %s", name, kPatches[i].offset, kPatches[i].what);
    else
      LOGW("arm64 %s+%#x: can't write; %s not patched", name, kPatches[i].offset, kPatches[i].what);
  }
}

#endif  // PRISM_STUB_LIB
