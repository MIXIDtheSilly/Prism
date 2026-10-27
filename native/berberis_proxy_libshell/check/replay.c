// replay LIBSHELL RECORDING MAX NAME=GOT...: runs each recorded call (record.c) through the original
// arm64 code, translated, on the memory and registers the lifted code saw, and compares every
// register and every byte it leaves with what the lifted code left. arm64; runs on the emulator
// (tools/lift_check.py). NAME=GOT: the library's GOT slot for each import the code calls (sincosf,
// atan2f, memcpy, abort, ...), which replay fills with its own.
//
// Guest calls the lifted code made (code it doesn't have, which the translator ran) are recorded
// with the registers they had and left; here the arm64 code's call to the same address goes to a
// stub (call.S's guest_stub_entry: each target's first instruction branches to it), which checks
// the arm64 code got there with the same registers, gives it the ones the call left and the memory
// it left as far as the lifted code read it afterwards, and returns. A stubbed function the arm64
// code calls otherwise (a sample's own function) runs, from a copy of its first instruction.
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

struct op { uint8_t kind, size; uint8_t pad[6]; uint64_t addr; uint8_t value[16]; };
struct regs { uint64_t x[31]; uint64_t sp; uint8_t v[32][16]; };
struct header { uint32_t magic, function, ops, pad; uint64_t base, tls, canary; struct regs in, out; };  // canary: the stack guard at tls + 0x28
enum { OP_LOAD, OP_STORE, OP_CALL };
#define CALL_SLOTS (2 * sizeof(struct regs) / sizeof(struct op))
void replay_call(const struct regs *in, struct regs *out, uint64_t fn);
void guest_stub_entry(void);
struct regs g_stub_regs;              // call.S: the registers at a stubbed call
uint8_t g_stub_stack[1 << 16] __attribute__((aligned(16)));  // and the stack guest_stub runs on

static uint8_t *lib;       // libshell.so's file
static uint64_t g_base, g_lo, g_hi;  // its mapping
static uint8_t *g_image;   // a pristine copy of the mapping (stubs in), to restore from

static sigjmp_buf g_jump;
static volatile uint64_t g_fault, g_fault_pc, g_fault_lr;
static void on_fault(int sig, siginfo_t *info, void *context) {
  (void)sig;
  g_fault = (uint64_t)info->si_addr;
  g_fault_pc = ((ucontext_t *)context)->uc_mcontext.pc;
  g_fault_lr = ((ucontext_t *)context)->uc_mcontext.regs[30];
  siglongjmp(g_jump, 1);
}

static void fail(const char *what) { perror(what); exit(1); }
static void my_abort(void) { fprintf(stderr, "abort called\n"); exit(3); }
static void my_stack_chk_fail(void) { fprintf(stderr, "stack check failed\n"); exit(3); }

static char **g_imports;
static int g_nimports;

static uint64_t stand_in(const char *name) {
  static const struct { const char *name; void *fn; } table[] = {
      {"sincosf", (void *)sincosf}, {"atan2f", (void *)atan2f}, {"abort", (void *)my_abort},
      {"__stack_chk_fail", (void *)my_stack_chk_fail}, {"memcpy", (void *)memcpy}, {"memmove", (void *)memmove},
      {"memset", (void *)memset}, {"sinf", (void *)sinf}, {"cosf", (void *)cosf}, {"tanf", (void *)tanf},
      {"asinf", (void *)asinf}, {"acosf", (void *)acosf}, {"atanf", (void *)atanf}, {"expf", (void *)expf},
      {"exp2f", (void *)exp2f}, {"logf", (void *)logf}, {"log2f", (void *)log2f}, {"log10f", (void *)log10f},
      {"sqrtf", (void *)sqrtf}, {"floorf", (void *)floorf}, {"ceilf", (void *)ceilf}, {"roundf", (void *)roundf},
      {"truncf", (void *)truncf}, {"fabsf", (void *)fabsf}, {"sinhf", (void *)sinhf}, {"coshf", (void *)coshf},
      {"tanhf", (void *)tanhf}, {"cbrtf", (void *)cbrtf}, {"powf", (void *)powf}, {"fmodf", (void *)fmodf},
      {"hypotf", (void *)hypotf}, {"fminf", (void *)fminf}, {"fmaxf", (void *)fmaxf},
  };
  for (size_t i = 0; i < sizeof table / sizeof *table; i++)
    if (!strcmp(table[i].name, name)) return (uint64_t)table[i].fn;
  return 0;
}

static void map_library(const char *path, uint64_t base) {
  int fd = open(path, O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st)) fail(path);
  lib = malloc(st.st_size);
  if (read(fd, lib, st.st_size) != st.st_size) fail("read");
  Elf64_Ehdr *eh = (Elf64_Ehdr *)lib;
  Elf64_Phdr *ph = (Elf64_Phdr *)(lib + eh->e_phoff);
  uint64_t hi = 0;
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
  hi = (hi + 0xfff) & ~0xfffull;
  void *m = mmap((void *)base, hi, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (m != (void *)base) fail("map libshell at VrShell's base");
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD) memcpy((uint8_t *)base + ph[i].p_vaddr, lib + ph[i].p_offset, ph[i].p_filesz);
  // The imports the code calls: their GOT slots point at this process's own.
  for (int i = 0; i < g_nimports; i++) {
    char name[64];
    unsigned long long slot;
    if (sscanf(g_imports[i], "%63[^=]=%llx", name, &slot) != 2) { fprintf(stderr, "%s: not NAME=GOT\n", g_imports[i]); exit(2); }
    uint64_t fn = stand_in(name);
    if (!fn) { fprintf(stderr, "%s: no stand-in for it\n", name); exit(2); }
    *(uint64_t *)(base + slot) = fn;
  }
  g_base = base; g_lo = base; g_hi = base + hi;
}

// ---- stubs for the guest calls' targets
#define MAX_TARGETS 4096
struct target { uint64_t addr, resume; };  // resume: where its own code continues (0: none here)
static struct target g_targets[MAX_TARGETS];
static int g_ntargets;
static uint8_t *g_stub_page;   // after the library: the targets' stubs
#define STUB_SIZE 48
static int pc_relative(uint32_t w) {
  return (w & 0x1f000000) == 0x10000000 ||  // adr, adrp
         (w & 0x7c000000) == 0x14000000 ||  // b, bl
         (w & 0x3b000000) == 0x18000000 ||  // ldr (literal)
         (w & 0x7e000000) == 0x34000000 ||  // cbz, cbnz
         (w & 0x7e000000) == 0x36000000 ||  // tbz, tbnz
         (w & 0xff000010) == 0x54000000;    // b.cond
}
static uint32_t branch_to(uint64_t from, uint64_t to) { return 0x14000000u | (uint32_t)(((int64_t)(to - from) >> 2) & 0x3ffffff); }
static void write_stub(uint32_t *s, uint64_t target) {
  s[0] = 0x58000090;  // ldr x16, +16
  s[1] = 0x580000b1;  // ldr x17, +20
  s[2] = 0xd61f0200;  // br x16
  s[3] = 0xd503201f;  // nop
  uint64_t entry = (uint64_t)guest_stub_entry;
  memcpy(s + 4, &entry, 8);
  memcpy(s + 6, &target, 8);
}
static int add_target(uint64_t t) {
  for (int i = 0; i < g_ntargets; i++) if (g_targets[i].addr == t) return 1;
  if (g_ntargets == MAX_TARGETS) return 0;
  struct target *tg = &g_targets[g_ntargets];
  tg->addr = t;
  tg->resume = 0;
  if (t >= g_lo && t < g_hi) {
    uint32_t *s = (uint32_t *)(g_stub_page + STUB_SIZE * g_ntargets);
    write_stub(s, t);
    uint32_t first = *(uint32_t *)t;
    if (!pc_relative(first)) {  // its own code continues from a copy of its first instruction
      s[8] = first;
      s[9] = branch_to((uint64_t)(s + 9), t + 4);
      tg->resume = (uint64_t)(s + 8);
    }
    *(uint32_t *)t = branch_to(t, (uint64_t)s);
  } else {  // elsewhere (another library): a page of its own, the stub at the address itself
    uint64_t p = t & ~0xfffull;
    void *m = mmap((void *)p, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != (void *)p && errno != EEXIST) return 0;
    write_stub((uint32_t *)t, t);
  }
  g_ntargets++;
  return 1;
}
static uint64_t resume_of(uint64_t t) {
  for (int i = 0; i < g_ntargets; i++) if (g_targets[i].addr == t) return g_targets[i].resume;
  return 0;
}

// ---- one sample's calls, as the stub sees them
#define MAX_CALLS 1024
struct call { uint64_t target; const struct regs *in, *out; int patch_lo, patch_hi; };
static struct call g_calls[MAX_CALLS];
static int g_ncalls, g_next_call;
#define MAX_PATCH (1 << 18)
static uint64_t g_patch_addr[MAX_PATCH];
static uint8_t g_patch_val[MAX_PATCH];
static int g_npatch;
static uint64_t g_rec_tls, g_my_tls, g_my_canary;
static char g_why[256];

// A recorded value as this process has it: the TLS pointer is its own (the stack guard is
// VrShell's while a call replays). A value that only looks like it stays as it was too: as_recorded().
static uint64_t here(uint64_t v) { return v == g_rec_tls ? g_my_tls : v; }
static int as_recorded(uint64_t got, uint64_t recorded) { return got == recorded || got == here(recorded); }

uint64_t guest_stub(uint64_t target) {
  struct regs *r = &g_stub_regs;
  if (g_next_call < g_ncalls && g_calls[g_next_call].target == target &&
      r->x[30] == g_calls[g_next_call].in->x[30]) {
    struct call *c = &g_calls[g_next_call++];
    for (int k = 0; k < 31; k++) {
      if (k >= 16 && k <= 18) continue;
      if (!as_recorded(r->x[k], c->in->x[k])) {
        snprintf(g_why, sizeof g_why, "call %d to libshell+%#llx: x%d %#llx, lifted %#llx", g_next_call - 1,
                 (unsigned long long)(target - g_lo), k, (unsigned long long)r->x[k], (unsigned long long)c->in->x[k]);
        siglongjmp(g_jump, 3);
      }
    }
    if (r->sp != c->in->sp) {
      snprintf(g_why, sizeof g_why, "call %d: sp", g_next_call - 1);
      siglongjmp(g_jump, 3);
    }
    for (int k = 0; k < 32; k++)
      if (memcmp(r->v[k], c->in->v[k], 16)) {
        uint32_t a[4], b[4];
        memcpy(a, r->v[k], 16); memcpy(b, c->in->v[k], 16);
        snprintf(g_why, sizeof g_why, "call %d to libshell+%#llx: v%d %08x %08x %08x %08x, lifted %08x %08x %08x %08x",
                 g_next_call - 1, (unsigned long long)(target - g_lo), k, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
        siglongjmp(g_jump, 3);
      }
    // What it left: registers (one it didn't change as the arm64 code has it), and memory the
    // lifted code read after it.
    for (int k = 0; k < 31; k++)
      if (k != 30 && (k < 16 || k > 18) && c->out->x[k] != c->in->x[k]) r->x[k] = here(c->out->x[k]);
    r->sp = c->out->sp;
    memcpy(r->v, c->out->v, sizeof r->v);
    for (int p = c->patch_lo; p < c->patch_hi; p++) *(uint8_t *)g_patch_addr[p] = g_patch_val[p];
    return 0;
  }
  uint64_t resume = resume_of(target);
  if (resume) return resume;
  snprintf(g_why, sizeof g_why, "a call to %#llx (lr libshell+%#llx) the lifted code didn't make",
           (unsigned long long)target, (unsigned long long)(r->x[30] - g_lo));
  siglongjmp(g_jump, 3);
}

#define MAX_PAGES 4096
static uint64_t pages[MAX_PAGES];
static int npages;
// Both NaNs, differing in the sign bit only: a NaN made from non-NaN operands (0 * inf), arm64's
// default NaN 0x7fc00000 where the lifted code has x86's, 0xffc00000.
static int made_nan(uint32_t a, uint32_t b) {
  return (a ^ b) == 0x80000000u && (a & 0x7f800000u) == 0x7f800000u && (a & 0x7fffffu);
}
static int in_lib(uint64_t a) { return a >= g_lo && a < g_hi; }
static int have_page(uint64_t p) {
  for (int i = 0; i < npages; i++) if (pages[i] == p) return 1;
  return 0;
}

// Bytes the call touched: their address, the expected final value, and the latest value known
// while walking the ops.
#define MAX_BYTES (1 << 20)
static uint64_t baddr[MAX_BYTES];
static uint8_t bfinal[MAX_BYTES], bcur[MAX_BYTES];
static int bcall[MAX_BYTES];  // the number of calls made when the byte was last seen
static int nbytes;
static int byte_index(uint64_t a) {
  for (int i = nbytes - 1; i >= 0 && i >= nbytes - 4096; i--) if (baddr[i] == a) return i;
  for (int i = 0; i < nbytes; i++) if (baddr[i] == a) return i;
  return -1;
}

static int next_op(const struct op *ops, uint32_t i) { return ops[i].kind == OP_CALL ? i + 1 + CALL_SLOTS : i + 1; }

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "replay LIBSHELL RECORDING MAX NAME=GOT...\n"); return 2; }
  g_imports = argv + 4;
  g_nimports = argc - 4;
  int fd = open(argv[2], O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st)) fail(argv[2]);
  uint8_t *rec = malloc(st.st_size);
  if (read(fd, rec, st.st_size) != st.st_size) fail("read recording");
  int limit = atoi(argv[3]) > 0 ? atoi(argv[3]) : 1 << 30;
  g_my_tls = (uint64_t)__builtin_thread_pointer();
  g_my_canary = *(uint64_t *)(g_my_tls + 0x28);
  // The library, and a stub for every guest call's target, before any code runs (the translator
  // would keep code it had already translated).
  if (st.st_size >= (off_t)sizeof(struct header)) {
    map_library(argv[1], ((struct header *)rec)->base);
    g_stub_page = mmap((void *)g_hi, STUB_SIZE * MAX_TARGETS, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (g_stub_page != (void *)g_hi) fail("map the stubs after libshell");
    for (size_t at = 0; at < (size_t)st.st_size;) {
      struct header *h = (struct header *)(rec + at);
      struct op *ops = (struct op *)(h + 1);
      at += sizeof *h + sizeof(struct op) * h->ops;
      for (uint32_t i = 0; i < h->ops; i = next_op(ops, i))
        if (ops[i].kind == OP_CALL && !add_target(ops[i].addr)) {
          fprintf(stderr, "can't stub %#llx\n", (unsigned long long)ops[i].addr);
          return 1;
        }
    }
    g_image = malloc(g_hi - g_lo);
    memcpy(g_image, (void *)g_lo, g_hi - g_lo);
  }
  size_t at = 0;
  int samples = 0, same = 0, nans = 0, skipped = 0, differ = 0, faulted = 0, racy = 0, with_calls = 0;
  struct sigaction sa = {0};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGSEGV, &sa, 0);
  sigaction(SIGBUS, &sa, 0);
  while (at < (size_t)st.st_size && samples < limit) {
    struct header *h = (struct header *)(rec + at);
    struct op *ops = (struct op *)(h + 1);
    at += sizeof *h + sizeof(struct op) * h->ops;
    samples++;
    uint64_t canary_at = h->tls + 0x28;
    g_rec_tls = h->tls;
    // Pages: every one an access touches, outside the library.
    npages = 0;
    int ok = 1;
    for (uint32_t i = 0; i < h->ops && ok; i = next_op(ops, i)) {
      if (ops[i].kind == OP_CALL) continue;
      if (ops[i].addr == canary_at) continue;
      for (uint64_t p = ops[i].addr & ~0xfffull; p < ops[i].addr + ops[i].size; p += 0x1000) {
        if (in_lib(p) || have_page(p)) continue;
        if (mmap((void *)p, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != (void *)p) { ok = 0; break; }
        pages[npages++] = p;
      }
    }
    if (!ok) {
      skipped++;
      for (int i = 0; i < npages; i++) munmap((void *)pages[i], 0x1000);
      continue;
    }
    // Initial memory: what each load read from a byte not seen yet. A byte read again with
    // another value after a guest call: that call left it so (the stub writes it). Expected final
    // memory: that, then each store in order (the return address and stack guard as this
    // process's own).
    nbytes = 0;
    g_ncalls = 0;
    g_next_call = 0;
    g_npatch = 0;
    int consistent = 1;
    for (uint32_t i = 0; i < h->ops; i = next_op(ops, i)) {
      if (ops[i].kind == OP_CALL) {
        if (g_ncalls == MAX_CALLS) { consistent = 0; break; }
        struct call *c = &g_calls[g_ncalls++];
        c->target = ops[i].addr;
        c->in = (const struct regs *)(ops + i + 1);
        c->out = c->in + 1;
        c->patch_lo = c->patch_hi = g_npatch;
        continue;
      }
      if (ops[i].addr == canary_at) continue;
      for (int k = 0; k < ops[i].size; k++) {
        uint64_t a = ops[i].addr + k;
        int j = byte_index(a);
        uint8_t v = ops[i].value[k];
        if (j < 0) {
          if (nbytes >= MAX_BYTES) { fprintf(stderr, "too many bytes\n"); return 1; }
          j = nbytes++;
          baddr[j] = a;
          if (ops[i].kind == OP_LOAD) *(uint8_t *)a = v;
          bfinal[j] = ops[i].kind == OP_LOAD ? v : *(uint8_t *)a;  // a store's old value doesn't matter
        } else if (ops[i].kind == OP_LOAD && bcur[j] != v) {
          if (bcall[j] == g_ncalls) { consistent = 0; continue; }  // changed by another thread
          if (g_npatch == MAX_PATCH) { consistent = 0; continue; }
          g_patch_addr[g_npatch] = a;
          g_patch_val[g_npatch++] = v;
          g_calls[g_ncalls - 1].patch_hi = g_npatch;
        }
        bcur[j] = v;
        bcall[j] = g_ncalls;
        bfinal[j] = v;
      }
    }
    if (!consistent) {
      racy++;
      for (int i = 0; i < npages; i++) munmap((void *)pages[i], 0x1000);
      for (int j = 0; j < nbytes; j++) if (in_lib(baddr[j])) *(uint8_t *)baddr[j] = g_image[baddr[j] - g_lo];
      continue;
    }
    if (g_ncalls) with_calls++;
    struct regs in = h->in, out;
    g_why[0] = 0;
    int jumped = sigsetjmp(g_jump, 1);
    *(uint64_t *)(g_my_tls + 0x28) = g_my_canary;  // this process's own again, after a call
    if (jumped == 1) {
      // The arm64 code touched memory the lifted code didn't: it went another way.
      if (faulted++ < 20)
        printf("sample %d (libshell+%#x): arm64 faulted at %#llx (pc libshell+%#llx, lr +%#llx)\n", samples - 1, h->function, (unsigned long long)g_fault,
               (unsigned long long)(g_fault_pc - g_lo), (unsigned long long)(g_fault_lr - g_lo));
    } else if (jumped == 3) {
      if (differ++ < 20) printf("sample %d (libshell+%#x, %u accesses): %s\n", samples - 1, h->function, h->ops, g_why);
    }
    if (jumped) {
      for (int i = 0; i < npages; i++) munmap((void *)pages[i], 0x1000);
      for (int j = 0; j < nbytes; j++) if (in_lib(baddr[j])) *(uint8_t *)baddr[j] = g_image[baddr[j] - g_lo];
      continue;
    }
    // VrShell's stack guard while the call runs (what it loads, stores and compares): none of this
    // process's frames returns meanwhile but those made inside it.
    *(uint64_t *)(g_my_tls + 0x28) = h->canary;
    replay_call(&in, &out, h->base + h->function);
    *(uint64_t *)(g_my_tls + 0x28) = g_my_canary;
    uint64_t label = out.x[30];  // replay's return address
    // Substitute the return address in stores of it. A stored TLS pointer may be either process's
    // (from mrs, this one's; from a register the call came in with, VrShell's): tls_words.
    int bad = 0, ntls = 0;
    static uint64_t tls_words[4096];
    char why[256] = "";
    if (g_next_call != g_ncalls) {
      bad = 1;
      snprintf(why, sizeof why, "made %d of the lifted code's %d guest calls", g_next_call, g_ncalls);
    }
    for (uint32_t i = 0; i < h->ops; i = next_op(ops, i)) {
      if (ops[i].kind != OP_STORE || ops[i].size != 8 || ops[i].addr == canary_at) continue;
      uint64_t v;
      memcpy(&v, ops[i].value, 8);
      if (v == g_rec_tls && ntls < 4096) tls_words[ntls++] = ops[i].addr;
      uint64_t want = v == h->in.x[30] ? label : v;
      if (want != v) for (int k = 0; k < 8; k++) bfinal[byte_index(ops[i].addr + k)] = (uint8_t)(want >> (8 * k));
    }
    int nan_sign = 0;  // a NaN an operation made from numbers: arm64's is 0x7fc00000, x86's 0xffc00000
    for (int j = 0; j < nbytes && !bad; j++)
      if (*(uint8_t *)baddr[j] != bfinal[j]) {
        uint64_t w = baddr[j] & ~3ull;
        uint32_t got, want = 0;
        memcpy(&got, (void *)w, 4);
        for (int k = 0; k < 4; k++) { int q = byte_index(w + k); want |= (uint32_t)(q >= 0 ? bfinal[q] : 0) << (8 * k); }
        if (made_nan(got, want)) {
          nan_sign = 1;
          for (int k = 0; k < 4; k++) { int q = byte_index(w + k); if (q >= 0) bfinal[q] = ((uint8_t *)&got)[k]; }
          continue;
        }
        int tls = 0;
        for (int t = 0; t < ntls && !tls; t++)
          if (baddr[j] >= tls_words[t] && baddr[j] < tls_words[t] + 8 && !memcmp((void *)tls_words[t], &g_my_tls, 8)) {
            tls = 1;
            for (int k = 0; k < 8; k++) { int q = byte_index(tls_words[t] + k); if (q >= 0) bfinal[q] = ((uint8_t *)&g_my_tls)[k]; }
          }
        if (tls) continue;
        bad = 1;
        float gf, wf; memcpy(&gf, &got, 4); memcpy(&wf, &want, 4);
        snprintf(why, sizeof why, "memory %#llx: %08x (%.9g), lifted %08x (%.9g)", (unsigned long long)w, got, gf, want, wf);
      }
    // Any byte of a mapped page the lifted code didn't touch must still be 0.
    for (int i = 0; i < npages && !bad; i++)
      for (int k = 0; k < 0x1000 && !bad; k++) {
        uint8_t b = ((uint8_t *)pages[i])[k];
        if (b && byte_index(pages[i] + k) < 0) {
          bad = 1;
          snprintf(why, sizeof why, "memory %#llx written only by arm64", (unsigned long long)(pages[i] + k));
        }
      }
    for (int r = 0; r < 31 && !bad; r++) {
      if (r >= 16 && r <= 18) continue;
      uint64_t want = h->out.x[r];
      if (r == 30) want = h->out.x[30] == h->in.x[30] ? label : ~0ull;
      else if (want == h->in.x[r]) want = in.x[r];
      if (out.x[r] != want && !(r != 30 && as_recorded(out.x[r], h->out.x[r]))) {
        bad = 1;
        snprintf(why, sizeof why, "x%d: %#llx, lifted %#llx", r, (unsigned long long)out.x[r], (unsigned long long)h->out.x[r]);
      }
    }
    if (!bad && out.sp != h->out.sp) { bad = 1; snprintf(why, sizeof why, "sp"); }
    for (int r = 0; r < 32 && !bad; r++) {
      uint32_t a[4], b[4];
      memcpy(a, out.v[r], 16); memcpy(b, h->out.v[r], 16);
      for (int k = 0; k < 4 && !bad; k++) {
        if (a[k] == b[k]) continue;
        if (made_nan(a[k], b[k])) { nan_sign = 1; continue; }
        bad = 1;
        snprintf(why, sizeof why, "v%d: %08x %08x %08x %08x, lifted %08x %08x %08x %08x", r, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
      }
    }
    if (bad) {
      if (differ++ < 20) printf("sample %d (libshell+%#x, %u accesses): %s\n", samples - 1, h->function, h->ops, why);
    } else if (nan_sign) {
      nans++;
    } else {
      same++;
    }
    for (int i = 0; i < npages; i++) munmap((void *)pages[i], 0x1000);
    for (int j = 0; j < nbytes; j++)
      if (in_lib(baddr[j])) *(uint8_t *)baddr[j] = g_image[baddr[j] - g_lo];
  }
  printf("%d samples (%d making guest calls): %d the same, %d the same but for a made NaN's sign, %d different, "
         "%d faulted, %d skipped (address taken here), %d racy (memory changed by another thread)\n",
         samples, with_calls, same, nans, differ, faulted, skipped, racy);
  return differ != 0 || faulted != 0;
}
