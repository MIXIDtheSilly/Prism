// replay LIBSHELL RECORDING MAX NAME=GOT...: runs each recorded call (record.c) through the original
// arm64 code, translated, on the memory and registers the lifted code saw, and compares every
// register and every byte it leaves with what the lifted code left. arm64; runs on the emulator
// (tools/lift_check.py). NAME=GOT: the library's GOT slot for each import the code calls (sincosf,
// atan2f, abort, __stack_chk_fail), which replay fills with its own.
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
struct header { uint32_t magic, function, ops, pad; uint64_t base, tls; struct regs in, out; };
void replay_call(const struct regs *in, struct regs *out, uint64_t fn);

static uint8_t *lib;       // libshell.so's file
static uint64_t g_base, g_lo, g_hi;  // its mapping
static uint8_t *g_image;   // a pristine copy of the mapping, to restore from

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
  // The imports the lifted functions call: their GOT slots point at this process's own.
  for (int i = 0; i < g_nimports; i++) {
    char name[64];
    unsigned long long slot;
    if (sscanf(g_imports[i], "%63[^=]=%llx", name, &slot) != 2) { fprintf(stderr, "%s: not NAME=GOT\n", g_imports[i]); exit(2); }
    uint64_t fn = !strcmp(name, "sincosf") ? (uint64_t)sincosf : !strcmp(name, "atan2f") ? (uint64_t)atan2f
                : !strcmp(name, "abort") ? (uint64_t)my_abort : !strcmp(name, "__stack_chk_fail") ? (uint64_t)my_stack_chk_fail : 0;
    if (!fn) { fprintf(stderr, "%s: no stand-in for it\n", name); exit(2); }
    *(uint64_t *)(base + slot) = fn;
  }
  g_base = base; g_lo = base; g_hi = base + hi;
  g_image = malloc(hi);
  memcpy(g_image, (void *)base, hi);
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

// Bytes the call touched: their address, the expected final value, and whether known.
#define MAX_BYTES (1 << 20)
static uint64_t baddr[MAX_BYTES];
static uint8_t bfinal[MAX_BYTES], bwritten[MAX_BYTES];
static int nbytes;
static int byte_index(uint64_t a) {
  for (int i = nbytes - 1; i >= 0 && i >= nbytes - 4096; i--) if (baddr[i] == a) return i;
  for (int i = 0; i < nbytes; i++) if (baddr[i] == a) return i;
  return -1;
}

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
  uint64_t my_tls = (uint64_t)__builtin_thread_pointer();
  uint64_t my_canary = *(uint64_t *)(my_tls + 0x28);
  size_t at = 0;
  int samples = 0, same = 0, nans = 0, skipped = 0, differ = 0, faulted = 0;
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
    if (!lib) map_library(argv[1], h->base);
    uint64_t canary_at = h->tls + 0x28, rec_canary = 0;
    int has_canary = 0;
    // Pages: every one an access touches, outside the library.
    npages = 0;
    int ok = 1;
    for (uint32_t i = 0; i < h->ops && ok; i++) {
      if (ops[i].addr == canary_at) { memcpy(&rec_canary, ops[i].value, 8); has_canary = 1; continue; }
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
    // Initial memory: what each load read from a byte this call hadn't written yet. Expected
    // final memory: that, then each store in order (the return address and stack guard as this
    // process's own).
    nbytes = 0;
    uint64_t label = 0;  // filled after the call: replay's return address is in out.x30
    for (uint32_t i = 0; i < h->ops; i++) {
      if (ops[i].addr == canary_at) continue;
      for (int k = 0; k < ops[i].size; k++) {
        uint64_t a = ops[i].addr + k;
        int j = byte_index(a);
        if (j < 0) {
          if (nbytes >= MAX_BYTES) { fprintf(stderr, "too many bytes\n"); return 1; }
          j = nbytes++;
          baddr[j] = a; bwritten[j] = 0;
          if (ops[i].kind == 0) { *(uint8_t *)a = ops[i].value[k]; bfinal[j] = ops[i].value[k]; }
          else bfinal[j] = *(uint8_t *)a;  // written before any read: its old value doesn't matter
        }
        if (ops[i].kind == 1) { bfinal[j] = ops[i].value[k]; bwritten[j] = 1; }
      }
    }
    struct regs in = h->in, out;
    if (sigsetjmp(g_jump, 1)) {
      // The arm64 code touched memory the lifted code didn't: it went another way.
      if (faulted++ < 20)
        printf("sample %d (libshell+%#x): arm64 faulted at %#llx (pc libshell+%#llx, lr +%#llx)\n", samples - 1, h->function, (unsigned long long)g_fault,
               (unsigned long long)(g_fault_pc - g_lo), (unsigned long long)(g_fault_lr - g_lo));
      for (int i = 0; i < npages; i++) munmap((void *)pages[i], 0x1000);
      for (int j = 0; j < nbytes; j++)
        if (in_lib(baddr[j])) *(uint8_t *)baddr[j] = g_image[baddr[j] - g_lo];
      continue;
    }
    replay_call(&in, &out, h->base + h->function);
    label = out.x[30];
    // Substitute the stack guard and return address in stores of them.
    int bad = 0;
    for (uint32_t i = 0; i < h->ops; i++) {
      if (ops[i].kind != 1 || ops[i].size != 8 || ops[i].addr == canary_at) continue;
      uint64_t v;
      memcpy(&v, ops[i].value, 8);
      uint64_t want = has_canary && v == rec_canary ? my_canary : v == h->in.x[30] ? label : v;
      if (want != v) for (int k = 0; k < 8; k++) bfinal[byte_index(ops[i].addr + k)] = (uint8_t)(want >> (8 * k));
    }
    char why[256] = "";
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
      else if (has_canary && want == rec_canary) want = my_canary;
      else if (want == h->tls) want = my_tls;
      if (out.x[r] != want) {
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
  printf("%d samples: %d the same, %d the same but for a made NaN's sign, %d different, %d faulted, %d skipped "
         "(address taken here)\n", samples, same, nans, differ, faulted, skipped);
  return differ != 0 || faulted != 0;
}
