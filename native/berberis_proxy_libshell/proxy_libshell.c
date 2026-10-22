/*
 * libberberis_proxy_libshell.so (x86_64): VrShell's ray hit-testing as native code.
 *
 * Moving a controller's ray makes VrShell test it, every frame, against each hit-testable object
 * of each open app window: HitTestableHelpers::ObjectRayTrace, with the shape parameters,
 * transforms and normals it takes, ~130-150 objects a window. Translated that cost ~25 us an
 * object, and it was what made VrShell's frame rate fall while pointing: with a Browser window,
 * the Store and the library open, sweeping both rays took VrShell from 10.7 ms of CPU a frame to
 * 19.7 (49 frames a second). The functions are float math on memory, calling nothing but libm and
 * a fatal assert, so tools/translator.py has tools/lift_arm64.py turn them into C, instruction by
 * instruction (lifted_libshell.c, generated from the user's own VrShell, in the build directory),
 * and builds that with this file. Lifted, the same sweep costs 9.8 ms, as much as standing still.
 *
 * libprism_jni calls prism_lift_attach as VrShell's libshell.so loads, before its code runs. It
 * checks the library is the build the code was lifted from (its GNU build ID, then each function's
 * bytes) and that the CPU has FMA (arm64's fused multiply-adds are fmaf, a slow library call
 * without it), then hands each function's address to the translator
 * (berberis::InterceptGuestSymbol). The translator loads this library as libshell.so's proxy and,
 * when the guest calls one of those addresses, calls the lifted function with the guest's
 * registers (its ThreadState) and returns to the guest's x30. Anything that doesn't check out
 * leaves VrShell as it was: translated.
 *
 * Checking it: native/berberis_proxy_libshell/check records calls in VrShell and replays them
 * through the original arm64 code, comparing every register and byte of memory.
 */
#include <android/log.h>
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "PrismLift"

uint64_t BASE;  // libshell.so's load address, for the lifted code's adrp

// From lifted_libshell.c.
struct prism_lifted {
  uint32_t offset, size;
  uint64_t fnv;
  void (*lifted)(uint8_t *);
  const char *name;
};
extern const struct prism_lifted prism_lifted[];
extern const unsigned prism_lifted_count;
extern const uint8_t prism_lifted_build_id[];
extern const unsigned prism_lifted_build_id_size;

// The lifted code's calls to a fatal assert ("HitTestableHelpers::ObjectRayTrace does not support
// hidden shapes." and the like: file, format, one string), which VrShell reports and aborts on.
void lift_fatal_assert(uint64_t file, uint64_t format, uint64_t arg) {
  char message[256];
  snprintf(message, sizeof message, (const char *)format, (const char *)arg);
  __android_log_print(ANDROID_LOG_FATAL, TAG, "%s: assertion failed (lifted code): %s", (const char *)file, message);
  abort();
}

void lift_fatal(const char *what, uint64_t at) {
  __android_log_print(ANDROID_LOG_FATAL, TAG, "%s at %#llx (lifted code)", what, (unsigned long long)at);
  abort();
}

// berberis's TrampolineFunc: (callee, ThreadState*); the callee is the lifted function.
#ifndef LIFT_RECORD
static void call_lifted(void *lifted, void *thread_state) { ((void (*)(uint8_t *))lifted)(thread_state); }
#else
void record_call(void *lifted, void *thread_state, uint32_t function);  // check/record.c
static void call_lifted(void *lifted, void *thread_state) {
  for (unsigned i = 0; i < prism_lifted_count; i++)
    if ((void *)prism_lifted[i].lifted == lifted) record_call(lifted, thread_state, prism_lifted[i].offset);
}
#endif

// berberis::KnownTrampoline.
struct known_trampoline {
  const char *name;
  void (*marshal)(void *, void *);
  void *thunk;
};
static struct known_trampoline *g_trampolines;

void _ZN8berberis19ProxyLibraryBuilder5BuildEPKcmPKNS_15KnownTrampolineEmPKNS_13KnownVariableE(
    void *builder, const char *library, size_t trampolines, const struct known_trampoline *trampoline,
    size_t variables, const void *variable);

__attribute__((visibility("default"))) void InitProxyLibrary(void *builder) {
  g_trampolines = calloc(prism_lifted_count, sizeof *g_trampolines);
  for (unsigned i = 0; i < prism_lifted_count; i++)
    g_trampolines[i] = (struct known_trampoline){prism_lifted[i].name, call_lifted, (void *)prism_lifted[i].lifted};
  // The builder dlopens the library it's given (for names a trampoline has no thunk for): this one.
  _ZN8berberis19ProxyLibraryBuilder5BuildEPKcmPKNS_15KnownTrampolineEmPKNS_13KnownVariableE(
      builder, "libberberis_proxy_libshell.so", prism_lifted_count, g_trampolines, 0, NULL);
}

static uint64_t fnv1a(const uint8_t *p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  while (n--) h = (h ^ *p++) * 0x100000001b3ull;
  return h;
}

// The GNU build ID of the library mapped at base (its ELF header and notes are in that mapping).
static int has_build_id(uintptr_t base) {
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
  const Elf64_Phdr *ph = (const Elf64_Phdr *)(base + eh->e_phoff);
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type != PT_NOTE) continue;
    const uint8_t *at = (const uint8_t *)base + ph[i].p_vaddr, *end = at + ph[i].p_memsz;
    while (at + sizeof(Elf64_Nhdr) <= end) {
      const Elf64_Nhdr *note = (const Elf64_Nhdr *)at;
      const uint8_t *desc = at + sizeof *note + (note->n_namesz + 3) / 4 * 4;
      if (note->n_type == NT_GNU_BUILD_ID && note->n_descsz == prism_lifted_build_id_size &&
          !memcmp(desc, prism_lifted_build_id, prism_lifted_build_id_size))
        return 1;
      at = desc + (note->n_descsz + 3) / 4 * 4;
    }
  }
  return 0;
}

typedef void (*InterceptFn)(uintptr_t address, const char *library, const char *name, const char *proxy_prefix);

// Called by libprism_jni once libshell.so (mapped at base) has loaded; returns how many functions
// now run lifted.
__attribute__((visibility("default"))) int prism_lift_attach(uintptr_t base, InterceptFn intercept) {
  if (!has_build_id(base)) {
    __android_log_print(ANDROID_LOG_WARN, TAG, "libshell.so: not the build lifted at deploy; runs translated");
    return 0;
  }
  for (unsigned i = 0; i < prism_lifted_count; i++) {
    if (fnv1a((const uint8_t *)base + prism_lifted[i].offset, prism_lifted[i].size) != prism_lifted[i].fnv) {
      __android_log_print(ANDROID_LOG_WARN, TAG, "libshell.so+%#x: not the code lifted; runs translated",
                          prism_lifted[i].offset);
      return 0;
    }
  }
  __builtin_cpu_init();
  if (!__builtin_cpu_supports("fma")) {
    __android_log_print(ANDROID_LOG_WARN, TAG, "no FMA on this CPU; libshell.so runs translated");
    return 0;
  }
  BASE = base;
  for (unsigned i = 0; i < prism_lifted_count; i++)
    intercept(base + prism_lifted[i].offset, "libshell.so", prism_lifted[i].name, "libberberis_proxy_");
  __android_log_print(ANDROID_LOG_INFO, TAG, "libshell.so: %u hit-testing functions run lifted", prism_lifted_count);
  return (int)prism_lifted_count;
}
