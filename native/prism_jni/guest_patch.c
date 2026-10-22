// Patches to Horizon's arm64 libraries, made as they load (libprism_jni only).
//
// The translator loads an app's arm64 library from its own copy (berberis_extract/<name>.so), mapped
// from offset 0, and translates its code as it first runs. Right after the library loads, before
// any of its code has, Prism writes the code below into that mapping. Each patch names the bytes it
// expects there; a library is patched only when all of its patches find them (or are already in
// place), and is otherwise left alone, with a warning: another Horizon build.
//
// * Frosted glass. Glass backgrounds are the compositor's (libvrruntimeservice.so): each frame it
//   blurs a background layer (passthrough) into a texture, and a layer that asks for glass shows
//   that texture where it's transparent. The compositor sets glass up only when two checks pass,
//   which on Prism they don't:
//   - The headsets it knows (device types 271-301). Here the device type is 270, its emulator's:
//     the runtime looks Build.MODEL ("Quest 3") up in a table of codenames, finds nothing, and the
//     hardware is ranchu. Claiming a real headset instead would change much more than glass (GPU
//     and format fallbacks, DRM, the HMD's configuration), so the patch makes only the glass
//     check's device test pass (sub w8, w8, #0x10f becomes mov w8, wzr: the first known type).
//   - The oculus_xrruntime/oculus_frosted_glass gatekeeper is read from the runtime's own cache
//     (gkcache.dat), which it rebuilds from its Java config client's values. That client rewrites
//     its cache at every boot with the value it has (false: nothing synced without a Meta account),
//     over anything set before it starts. The patch takes the gatekeeper's answer as on
//     (ldrb w8, [sp, #0x50], the value the gatekeeper call wrote, becomes mov w8, #1).
//   Which layers are glass is left to Horizon, as on a headset. A Quest 3S on the same build
//   (VrShell's and the compositor's code the same) gives the library, the system bar and other
//   panels glassMode=0 (SurfaceFlinger's list of the layers it sends the compositor): no glass, no
//   panel shading, the tiles over whatever is behind, and a translucent gray only where a panel
//   draws one itself (the running app's card). Glass forced onto VrShell's layers instead showed
//   the blurred environment (no passthrough to blur) or, without it, the runtime's solid fallback.
//
// * VrShell's ray hit-testing, run as native code: as libshell.so loads, libberberis_proxy_libshell
//   (native/berberis_proxy_libshell) checks it and has the translator call its lifted functions
//   in place of the guest's (lift_libshell below). The guest code itself is left as it is.

#ifndef PRISM_STUB_LIB

#include "prism_guest.h"

#include <android/log.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TAG "PrismJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

#define BYTES(s) (const uint8_t *)(s), sizeof(s) - 1
static const struct {
  const char *library;
  uint32_t offset;  // in the library's file, which is its address from its base
  const uint8_t *original;
  size_t original_size;
  const uint8_t *patched;
  size_t patched_size;
  const char *what;
} kPatches[] = {
    {"libvrruntimeservice.so", 0x6becfc, BYTES("\x08\x3d\x04\x51"), BYTES("\xe8\x03\x1f\x2a"),
     "frosted glass on the emulator's device type"},
    {"libvrruntimeservice.so", 0x6bed50, BYTES("\xe8\x43\x41\x39"), BYTES("\x28\x00\x80\x52"),
     "frosted glass without its gatekeeper"},
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

// VrShell's ray hit-testing runs as native code (libberberis_proxy_libshell.so, from
// tools/lift_arm64.py): that library checks the build and asks the translator to call its code for
// the guest functions' addresses (berberis::InterceptGuestSymbol), before any of them has run.
static void lift_libshell(const char *name) {
  uintptr_t base = library_base(name);
  void *lift = dlopen("libberberis_proxy_libshell.so", RTLD_NOW);
  void *translator = dlopen("libberberis_arm64.so", RTLD_NOW | RTLD_NOLOAD);
  int (*attach)(uintptr_t, void *) = lift ? (int (*)(uintptr_t, void *))dlsym(lift, "prism_lift_attach") : NULL;
  void *intercept = translator ? dlsym(translator, "_ZN8berberis20InterceptGuestSymbolEmPKcS1_S1_") : NULL;
  if (!base || !attach || !intercept) {
    LOGW("arm64 %s: hit-testing not lifted (%s)", name,
         !base ? "library not found" : !attach ? "no libberberis_proxy_libshell.so" : "no InterceptGuestSymbol");
    return;
  }
  attach(base, intercept);
}

void prism_patch_guest_library(const char *path) {
  const char *slash = path ? strrchr(path, '/') : NULL;
  const char *name = slash ? slash + 1 : path;
  if (!name) return;
  uintptr_t base = 0;
  for (size_t i = 0; i < sizeof kPatches / sizeof *kPatches; i++) {
    if (strcmp(name, kPatches[i].library)) continue;
    if (!base && !(base = library_base(name))) {
      LOGW("arm64 %s: not found in memory; not patched", name);
      return;
    }
    const uint8_t *at = (const uint8_t *)(base + kPatches[i].offset);
    if (memcmp(at, kPatches[i].original, kPatches[i].original_size) &&
        memcmp(at, kPatches[i].patched, kPatches[i].patched_size)) {
      LOGW("arm64 %s+%#x: not the expected code (another build?); %s not patched", name, kPatches[i].offset, name);
      return;
    }
  }
  for (size_t i = 0; base && i < sizeof kPatches / sizeof *kPatches; i++) {
    if (strcmp(name, kPatches[i].library)) continue;
    uintptr_t at = base + kPatches[i].offset;
    if (!memcmp((const void *)at, kPatches[i].patched, kPatches[i].patched_size)) continue;
    if (write_code(at, kPatches[i].patched, kPatches[i].patched_size))
      LOGI("arm64 %s+%#x patched: %s", name, kPatches[i].offset, kPatches[i].what);
    else
      LOGW("arm64 %s+%#x: can't write; %s not patched", name, kPatches[i].offset, kPatches[i].what);
  }
  if (!strcmp(name, "libshell.so")) lift_libshell(name);
}

#endif  // PRISM_STUB_LIB
