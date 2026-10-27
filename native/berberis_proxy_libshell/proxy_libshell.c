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
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "PrismLift"

uint64_t BASE;  // libshell.so's load address, for the lifted code's adrp

// From lifted_libshell.c.
struct prism_lifted {
  uint32_t offset;
  uint32_t nranges;  // its code: offset, size pairs
  const uint32_t *ranges;
  uint64_t fnv;
  void (*lifted)(uint8_t *);
  const char *name;
  uint32_t nstops;  // its guest calls' return addresses (offsets)
  const uint32_t *stops;
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

// A call the lifted code makes to code it doesn't have (a virtual function, an import, one not
// lifted): the translator runs it on the guest's registers, from its first instruction until it
// returns, as it does a host call into guest code (berberis::RunGuestCall), but on the guest's own
// stack as it is (stack arguments stay where the callee expects them). It returns to x30, the
// call's own return address in the guest function, which prism_lift_attach made one of the
// translator's stops: the guest stack is then arm64's, so an exception thrown below unwinds
// through the lifted function's frame (the lifted code keeps it as the guest code does), and
// should it land outside, the translator's own longjmp handling unwinds the host frames. A tail
// call (or a call returning where other code branches to) returns to the translator's common stop
// address instead (_stop).
#define CPU_X30 0xf0
#define CPU_PC 0x310  // CPUState::insn_addr
void _ZN8berberis16ExecuteGuestCallEPNS_11ThreadStateE(void *thread_state);
void _ZN8berberis38InitVirtualGuestCallFrameReturnAddressEm(uint64_t address);
void _ZN8berberis20InvalidateGuestRangeEmm(uint64_t start, uint64_t end);
extern uint64_t _ZN8berberis27ScopedVirtualGuestCallFrame17g_return_address_E;
#define CPU_SP 0x108
//
// Mostly the call doesn't go through ExecuteGuestCall, though: its dispatcher looks the target up
// in the translation table, and the callee's return looks its stop up twice more (generated code's
// dispatch, then the dispatcher's), each a cache and TLB miss in a table of tens of megabytes at
// thousands of different addresses; that was most of what a call cost. prism_run enters the
// target's code as berberis_RunGeneratedCode does, then makes the call as generated code makes its
// own (Digitalis's EmitCallDispatch): a host call leaving a host return pair with the call's
// return address, which the callee's return takes (EmitReturnDispatch), with a host ret the CPU
// predicts, to exit generated code without a lookup. Any other exit (an untranslated target, a
// pending signal, a stop elsewhere) finishes the call in the dispatcher, from where it got to.
#define TS_PENDING_SIGNALS 0x360  // ThreadState::pending_signals_status; 2 is kPendingSignalsPresent
uint8_t *_ZN8berberis16TranslationCache11GetInstanceEv(void);
extern uint32_t _ZN8berberis10kEntryStopE, _ZN8berberis19kEntryNotTranslatedE, _ZN8berberis17kEntryTranslatingE,
    _ZN8berberis18kEntryInvalidatingE, _ZN8berberis14kEntryWrappingE, _ZN8berberis15kEntryInterpretE,
    _ZN8berberis12kEntryNoExecE;
void berberis_RunGeneratedCode(void *thread_state, uint64_t code);
void berberis_entry_ExitGeneratedCode(void);
static uint64_t *const *volatile g_table;  // TranslationCache's main table, once prism_run is checked
__attribute__((naked)) static void prism_run(uint8_t *ts, uint64_t code, uint64_t ret) {
  // berberis_RunGeneratedCode's own instructions (prism_lift_attach compares them)...
  __asm__(
      "push %rbp\n push %rbx\n push %r12\n push %r13\n push %r14\n push %r15\n"
      "push 0x378(%rdi)\n push 0x380(%rdi)\n"  // the host return stack's base and limit
      "sub $0x28, %rsp\n"
      "mov %rsp, 0x378(%rdi)\n"
      "mov %rsp, %rcx\n sub $0x4000, %rcx\n mov %rcx, 0x380(%rdi)\n"
      "mov berberis_entry_ExitGeneratedCode@GOTPCREL(%rip), %rcx\n mov %rcx, 0x10(%rsp)\n"
      "movq $0, 0x18(%rsp)\n"
      "mov %rdi, %rbp\n mov 0x310(%rbp), %rax\n movb $1, 0x361(%rbp)\n"
      // ...but for a call instead of the jump: the pair is the guest return address, then the
      // host one, then 16 bytes for the callee.
      "push %rdx\n"
      "call 1f\n"
      "add $8, %rsp\n"
      "jmp *berberis_entry_ExitGeneratedCode@GOTPCREL(%rip)\n"
      "1: sub $16, %rsp\n"
      "jmp *%rsi\n");
}
static uint32_t lookup(uint64_t pc) {
  return ((const uint32_t *)g_table[(pc >> 24) & 0xffffff])[pc & 0xffffff];
}
// Even one lookup a call was most of what was left (the table entry, 2026-10-27: 3% of VrShell's
// frame thread), so the targets in libshell.so (it's never unloaded or written: its code stays as
// translated) keep their code here, looked up again every 64 calls for a newer translation (an old
// one still runs: the translator never frees code). An entry is one word, the target's offset and
// its code, so threads see one or the other whole.
#define CODES 4096
static uint64_t g_codes[CODES];
static uint8_t g_code_uses[CODES];
static uint64_t g_text_size;  // libshell.so's code (from BASE), set by prism_lift_attach
static int special(uint32_t code) {
  return code == _ZN8berberis10kEntryStopE || code == _ZN8berberis19kEntryNotTranslatedE ||
         code == _ZN8berberis17kEntryTranslatingE || code == _ZN8berberis18kEntryInvalidatingE ||
         code == _ZN8berberis14kEntryWrappingE || code == _ZN8berberis15kEntryInterpretE ||
         code == _ZN8berberis12kEntryNoExecE;
}
static uint32_t host_code(uint64_t target) {
  uint64_t offset = target - BASE;
  if (offset >= g_text_size) return lookup(target);
  unsigned k = (unsigned)((offset >> 2) * 0x9E3779B1u) >> 20;
  uint64_t e = __atomic_load_n(&g_codes[k], __ATOMIC_RELAXED);
  if ((uint32_t)e == (uint32_t)(offset | 1) && g_code_uses[k]--) return (uint32_t)(e >> 32);
  uint32_t code = lookup(target);
  if (!special(code)) {
    __atomic_store_n(&g_codes[k], (uint64_t)code << 32 | (offset | 1), __ATOMIC_RELAXED);
    g_code_uses[k] = 63;
  }
  return code;
}
// The rest is ExecuteGuestCall's: the call is one of the thread's guest call executions (the
// translator's longjmp handling finds them by their sp), and the dispatcher (ExecuteGuest) takes
// any exit that isn't the return, until a stop.
void _ZN8berberis12ExecuteGuestEPNS_11ThreadStateE(void *thread_state);
#define TS_THREAD 0x350                    // ThreadState::thread
#define THREAD_GUEST_CALL_EXECUTION 0x70  // GuestThread::guest_call_execution_
#define TS_RETURN_STACK 0x378              // ThreadState::host_return_stack: base, limit
struct guest_call_execution {
  struct guest_call_execution *parent;
  uint64_t sp;
  sigjmp_buf buf;
};
static void fast_guest_call(uint8_t *ts, uint64_t target, uint64_t ret, uint64_t sp) {
  struct guest_call_execution **current =
      (struct guest_call_execution **)(*(uint8_t **)(ts + TS_THREAD) + THREAD_GUEST_CALL_EXECUTION);
  struct guest_call_execution call = {.parent = *current, .sp = sp};
  uint64_t base = *(uint64_t *)(ts + TS_RETURN_STACK), limit = *(uint64_t *)(ts + TS_RETURN_STACK + 8);
  if (!sigsetjmp(call.buf, 0)) {
    *current = &call;
    prism_run(ts, host_code(target), ret);
    if (*(uint64_t *)(ts + CPU_PC) != ret || *(volatile uint8_t *)(ts + TS_PENDING_SIGNALS) == 2)
      _ZN8berberis12ExecuteGuestEPNS_11ThreadStateE(ts);
  } else {  // a guest longjmp below came back to this frame
    *current = &call;
    _ZN8berberis12ExecuteGuestEPNS_11ThreadStateE(ts);
  }
  *(uint64_t *)(ts + TS_RETURN_STACK) = base;
  *(uint64_t *)(ts + TS_RETURN_STACK + 8) = limit;
  *current = call.parent;
  uint64_t now = *(uint64_t *)(ts + CPU_SP);
  if (now != sp)
    for (struct guest_call_execution *e = call.parent; e; e = e->parent)
      if (e->sp == now) siglongjmp(e->buf, 1);
}
void prism_guest_call(uint8_t *ts, uint64_t target) {
  uint64_t pc = *(uint64_t *)(ts + CPU_PC);
  uint64_t ret = *(uint64_t *)(ts + CPU_X30), sp = *(uint64_t *)(ts + CPU_SP);
  *(uint64_t *)(ts + CPU_PC) = target;
  if (g_table)
    fast_guest_call(ts, target, ret, sp);
  else
    _ZN8berberis16ExecuteGuestCallEPNS_11ThreadStateE(ts);
  // The translator stops where the call returns, with the stack as it was; anything else means
  // guest code ran on past this function's frame (a stop it didn't stop at), and what it left
  // isn't this call's.
  uint64_t at = *(uint64_t *)(ts + CPU_PC), now = *(uint64_t *)(ts + CPU_SP);
  if (at != ret || now != sp) {
    __android_log_print(ANDROID_LOG_FATAL, TAG, "guest call to libshell.so+%#llx returned at %#llx (sp %#llx), not at %#llx (sp %#llx)",
                        (unsigned long long)(target - BASE), (unsigned long long)(at - BASE), (unsigned long long)now,
                        (unsigned long long)(ret - BASE), (unsigned long long)sp);
    abort();
  }
  *(uint64_t *)(ts + CPU_PC) = pc;
}
void prism_guest_call_stop(uint8_t *ts, uint64_t target) {
  *(uint64_t *)(ts + CPU_X30) = _ZN8berberis27ScopedVirtualGuestCallFrame17g_return_address_E;
  prism_guest_call(ts, target);
}
#ifndef LIFT_RECORD
void lift_guest_call(uint8_t *ts, uint64_t target) __attribute__((alias("prism_guest_call")));
void lift_guest_call_stop(uint8_t *ts, uint64_t target) __attribute__((alias("prism_guest_call_stop")));
#endif  // check/record.c's log the call, then make it

// The lifted functions by guest address, for the lifted code's indirect calls (a virtual call to a
// lifted function is a call here, not a round trip through the translator): open addressing, set
// up by prism_lift_attach.
// Most of those calls are to functions that aren't, so a bit for each slot answers them first, from
// 1 KB that stays in the cache rather than the table's 64.
#define FIND_SIZE 4096  // a power of two, over twice the functions
static struct { uint64_t address; void (*lifted)(uint8_t *); } g_find[FIND_SIZE];
static uint32_t g_find_bits[8192 / 32];
static unsigned find_hash(uint64_t address) { return (unsigned)((address >> 2) * 0x9E3779B1u) >> 19; }  // 13 bits
static unsigned find_slot(uint64_t address) { return find_hash(address) & (FIND_SIZE - 1); }
void (*lift_find(uint64_t address))(uint8_t *) {
  unsigned h = find_hash(address);
  if (!(g_find_bits[h / 32] >> (h % 32) & 1)) return NULL;
  for (unsigned i = h & (FIND_SIZE - 1);; i = (i + 1) & (FIND_SIZE - 1)) {
    if (g_find[i].address == address) return g_find[i].lifted;
    if (!g_find[i].address) return NULL;
  }
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

static uint64_t fnv1a(uint64_t h, const uint8_t *p, size_t n) {
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

// The end of the library's code (its executable segments), from base.
static uint64_t text_end(uintptr_t base) {
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
  const Elf64_Phdr *ph = (const Elf64_Phdr *)(base + eh->e_phoff);
  uint64_t end = 0;
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X) && ph[i].p_vaddr + ph[i].p_memsz > end)
      end = ph[i].p_vaddr + ph[i].p_memsz;
  return end;
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
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned r = 0; r < prism_lifted[i].nranges; r++)
      h = fnv1a(h, (const uint8_t *)base + prism_lifted[i].ranges[2 * r], prism_lifted[i].ranges[2 * r + 1]);
    if (h != prism_lifted[i].fnv) {
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
  if (!_ZN8berberis27ScopedVirtualGuestCallFrame17g_return_address_E) {
    __android_log_print(ANDROID_LOG_WARN, TAG, "the translator has no guest call return address; libshell.so runs translated");
    return 0;
  }
  BASE = base;
  // The guest calls' return addresses become the translator's stops, as its own return address for
  // host calls is (marking one sets that address too: it's put back after). The library's
  // constructors may have run some of this code translated already: that goes first.
  for (unsigned i = 0; i < prism_lifted_count; i++)
    for (unsigned r = 0; r < prism_lifted[i].nranges; r++) {
      uint64_t at = base + prism_lifted[i].ranges[2 * r];
      _ZN8berberis20InvalidateGuestRangeEmm(at, at + prism_lifted[i].ranges[2 * r + 1]);
    }
  uint64_t own = _ZN8berberis27ScopedVirtualGuestCallFrame17g_return_address_E;
  unsigned stops = 0;
  for (unsigned i = 0; i < prism_lifted_count; i++)
    for (unsigned k = 0; k < prism_lifted[i].nstops; k++, stops++)
      _ZN8berberis38InitVirtualGuestCallFrameReturnAddressEm(base + prism_lifted[i].stops[k]);
  _ZN8berberis38InitVirtualGuestCallFrameReturnAddressEm(own);
  // The fast guest call copies the translator's entry and reads its table: only if this is the
  // translator it was written for (the entry's bytes), and its table is where it's read (the common
  // stop's entry is a stop).
  static const uint8_t run_generated_code[] = {
      0x55, 0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xff, 0xb7, 0x78, 0x03, 0x00, 0x00,
      0xff, 0xb7, 0x80, 0x03, 0x00, 0x00, 0x48, 0x83, 0xec, 0x28, 0x48, 0x89, 0xa7, 0x78, 0x03, 0x00,
      0x00, 0x48, 0x89, 0xe1, 0x48, 0x81, 0xe9, 0x00, 0x40, 0x00, 0x00, 0x48, 0x89, 0x8f, 0x80, 0x03,
      0x00, 0x00};  // then the load of berberis_entry_ExitGeneratedCode's address (rip-relative), then
  static const uint8_t run_generated_code_end[] = {
      0x48, 0x89, 0x4c, 0x24, 0x10, 0x48, 0xc7, 0x44, 0x24, 0x18, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0xfd,
      0x48, 0x8b, 0x85, 0x10, 0x03, 0x00, 0x00, 0xc6, 0x85, 0x61, 0x03, 0x00, 0x00, 0x01, 0xff, 0xe6};
  // And ExecuteGuestCall's execution: the thread's (0x350), its current one (0x70), sp (0x108) and
  // the parent at -0x98 below the jmp_buf at -0x88.
  static const uint8_t execute_guest_call[] = {
      0x4c, 0x8b, 0xaf, 0x50, 0x03, 0x00, 0x00, 0x4c, 0x8d, 0xb5, 0x78, 0xff, 0xff, 0xff, 0x0f, 0x57, 0xc0,
      0x0f, 0x11, 0x45, 0xb8, 0x0f, 0x11, 0x45, 0xa8, 0x0f, 0x11, 0x45, 0x98, 0x0f, 0x11, 0x45, 0x88, 0x0f,
      0x11, 0x85, 0x78, 0xff, 0xff, 0xff, 0x48, 0xc7, 0x45, 0xc8, 0x00, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x45,
      0x70, 0x48, 0x89, 0x85, 0x68, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x87, 0x08, 0x01, 0x00, 0x00, 0x48, 0x89,
      0x85, 0x70, 0xff, 0xff, 0xff};
  const uint8_t *entry = (const uint8_t *)berberis_RunGeneratedCode;
  int fast = !memcmp(entry, run_generated_code, sizeof run_generated_code) &&
             !memcmp(entry + sizeof run_generated_code + 7, run_generated_code_end, sizeof run_generated_code_end) &&
             !memcmp((const uint8_t *)_ZN8berberis16ExecuteGuestCallEPNS_11ThreadStateE + 0x2a, execute_guest_call,
                     sizeof execute_guest_call);
  if (fast) {
    g_text_size = text_end(base);
    g_table = *(uint64_t *const **)(_ZN8berberis16TranslationCache11GetInstanceEv() + 0xa8);
    fast = lookup(own) == _ZN8berberis10kEntryStopE;
    if (!fast) g_table = NULL;
  }
  if (2 * prism_lifted_count > FIND_SIZE) {
    __android_log_print(ANDROID_LOG_WARN, TAG, "%u lifted functions: more than the lookup holds; libshell.so runs translated",
                        prism_lifted_count);
    return 0;
  }
  for (unsigned i = 0; i < prism_lifted_count; i++) {
    unsigned k = find_slot(base + prism_lifted[i].offset);
    while (g_find[k].address) k = (k + 1) & (FIND_SIZE - 1);
    g_find[k].address = base + prism_lifted[i].offset;
    g_find[k].lifted = prism_lifted[i].lifted;
    unsigned h = find_hash(base + prism_lifted[i].offset);
    g_find_bits[h / 32] |= 1u << (h % 32);
  }
  for (unsigned i = 0; i < prism_lifted_count; i++)
    intercept(base + prism_lifted[i].offset, "libshell.so", prism_lifted[i].name, "libberberis_proxy_");
  __android_log_print(ANDROID_LOG_INFO, TAG, "libshell.so: %u functions run lifted (%u guest call returns, %s)",
                      prism_lifted_count, stops, fast ? "fast guest calls" : "guest calls through ExecuteGuestCall");
  return (int)prism_lifted_count;
}
