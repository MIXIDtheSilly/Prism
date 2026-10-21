// Patches to Horizon's arm64 libraries, made as they load (libprism_jni only).
//
// The translator loads an app's arm64 library from its own copy (berberis_extract/<name>.so), mapped
// from offset 0, and translates its code as it first runs. Right after the library loads, before
// any of its code has, Prism writes the code below into that mapping. Each patch names the bytes it
// expects there; a library is patched only when all of its patches find them (or are already in
// place), and is otherwise left alone, with a warning: another Horizon build.
//
// * Frosted glass. Panels' glass backgrounds are the compositor's (libvrruntimeservice.so): each
//   frame it blurs a background layer into a texture, and a glass layer shows that texture where
//   it's transparent. Four things stand in its way on Prism:
//   - It sets glass up only on the headsets it knows (device types 271-301). Here the device type
//     is 270, its emulator's: the runtime looks Build.MODEL ("Quest 3") up in a table of codenames,
//     finds nothing, and the hardware is ranchu. Claiming a real headset instead would change much
//     more than glass (GPU and format fallbacks, DRM, the HMD's configuration), so the patch makes
//     only the glass check's device test pass (sub w8, w8, #0x10f becomes mov w8, wzr: the first
//     known type).
//   - The oculus_xrruntime/oculus_frosted_glass gatekeeper is read from the runtime's own cache
//     (gkcache.dat), which it rebuilds from its Java config client's values. That client rewrites
//     its cache at every boot with the value it has (false: nothing synced without a Meta account),
//     over anything set before it starts. The patch takes the gatekeeper's answer as on
//     (ldrb w8, [sp, #0x50], the value the gatekeeper call wrote, becomes mov w8, #1).
//   - The background is the layer the compositor nominates each frame: a passthrough layer
//     (projection-like types 1 and 8 with subtypes 1, 3 or 5), else, with
//     debug.oculus.frostedGlass.enableVR, a 420 x 440 one. Prism has neither, so the background
//     was empty and glass showed the runtime's solid fallback, RGB(18,24,30). The patch nominates
//     the first (lowest) projection layer instead: Home's environment, or the immersive app under
//     the Universal Menu. In the tagging loop (0x6d2db4), the subtype test's b.eq and the
//     mov w19, w20 that records the candidate become cmn w19, #1; csel w19, w20, w19, eq.
//   - Which layers are glass is VrShell's (libshell.so) to say, per layer, in the
//     XR_METAX1_composition_layer_glass struct its layer builder chains on (0xb0c208, when the
//     descriptor's flag 0x20 is set). On Prism it's set for almost none: VrShell's own choice
//     depends on the OCUI theme resources and on the SurfaceFlinger side of panel windows, neither
//     of which Prism has. The patch has every layer the builder makes, other than projections
//     (VrShell's scene: the background), ask for glass: the panels, the library, the system bar.
//     Its routine goes in the library's .note.android.ident (read by the linker as the library
//     loads, not after), and the builder's tbnz w9, #5 jumps to it.
//   The compositor holds 90 frames a second with the library's glass on.

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
    {"libvrruntimeservice.so", 0x6d2df0, BYTES("\x60\x00\x00\x54\xf3\x03\x14\x2a"),
     BYTES("\x7f\x06\x00\x31\x93\x02\x93\x1a"), "frosted glass blurs the lowest projection layer"},
    // ldr w10, [x0]; cmp w10, #0x23 (a projection); b.eq 1f; b glass; 1: tbz w9, #5, 2f; b glass;
    // 2: b no_glass. glass is the builder's own block (0xb0c5a0), no_glass the instruction after
    // the tbnz (0xb0c434).
    {"libshell.so", 0x270,
     BYTES("\x08\x00\x00\x00\x84\x00\x00\x00\x01\x00\x00\x00\x41\x6e\x64\x72\x6f\x69\x64\x00\x22\x00\x00\x00"
           "\x72\x32\x36\x62"),
     BYTES("\x0a\x00\x40\xb9\x5f\x8d\x00\x71\x40\x00\x00\x54\xc9\x30\x2c\x14\x49\x00\x28\x36\xc7\x30\x2c\x14"
           "\x6b\x30\x2c\x14"),
     "frosted glass on VrShell's layers (its routine)"},
    {"libshell.so", 0xb0c430, BYTES("\x89\x0b\x28\x37"), BYTES("\x90\xcf\xd3\x17"),
     "frosted glass on VrShell's layers"},
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
}

#endif  // PRISM_STUB_LIB
