// fuzz LIBSHELL IN.rec OUT.rec PER_SAMPLE SEED [COVER]: x86_64. For each recorded call, makes
// variants (floats scaled, negated, zeroed, replaced; small integers such as shape types changed,
// in memory and in argument registers), runs the lifted code on each with recording on, and writes
// them for replay (replay.c checks them against arm64): calls VrShell didn't make while recording,
// down paths it didn't take. Variants that fault, reach a fatal assert or a guest call (code the
// translator would run, not here) are dropped. COVER: where to write the basic blocks reached.
#define _GNU_SOURCE
#include <elf.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct op { uint8_t kind, size; uint8_t pad[6]; uint64_t addr; uint8_t value[16]; };
struct regs { uint64_t x[31]; uint64_t sp; uint8_t v[32][16]; };
struct header { uint32_t magic, function, ops, pad; uint64_t base, tls, canary; struct regs in, out; };  // canary: the stack guard at tls + 0x28
enum { OP_LOAD, OP_STORE, OP_CALL };
#define CALL_SLOTS (2 * sizeof(struct regs) / sizeof(struct op))
static uint32_t next_op(const struct op *ops, uint32_t i) { return ops[i].kind == OP_CALL ? i + 1 + CALL_SLOTS : i + 1; }

uint64_t BASE;
void record_abandon(void);
void record_call(void *lifted, void *thread_state, uint32_t function);
void record_open(const char *path);
void record_dump_cover(const char *path);
struct prism_lifted { uint32_t offset, nranges; const uint32_t *ranges; uint64_t fnv; void (*lifted)(uint8_t *); const char *name;
                      uint32_t nstops; const uint32_t *stops; };
int record_guest_calls(void);
extern const struct prism_lifted prism_lifted[];
extern const unsigned prism_lifted_count;
static void *lifted_for(uint32_t f) {
  for (unsigned i = 0; i < prism_lifted_count; i++)
    if (prism_lifted[i].offset == f) return (void *)prism_lifted[i].lifted;
  return NULL;
}

static uint64_t g_lo, g_hi;
static uint8_t *g_image;
static void map_library(const char *path, uint64_t base) {
  int fd = open(path, O_RDONLY);
  struct stat st;
  fstat(fd, &st);
  uint8_t *lib = malloc(st.st_size);
  if (read(fd, lib, st.st_size) != st.st_size) { perror("read"); exit(1); }
  Elf64_Ehdr *eh = (Elf64_Ehdr *)lib;
  Elf64_Phdr *ph = (Elf64_Phdr *)(lib + eh->e_phoff);
  uint64_t hi = 0;
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
  hi = (hi + 0xfff) & ~0xfffull;
  if (mmap((void *)base, hi, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != (void *)base) {
    perror("map libshell"); exit(1);
  }
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD) memcpy((uint8_t *)base + ph[i].p_vaddr, lib + ph[i].p_offset, ph[i].p_filesz);
  g_lo = base; g_hi = base + hi;
  g_image = malloc(hi);
  memcpy(g_image, (void *)base, hi);
  BASE = base;
}

sigjmp_buf g_jump;
static void on_fault(int sig) { (void)sig; siglongjmp(g_jump, 1); }
static int g_fatal;
void lift_fatal_assert(uint64_t file, uint64_t format, uint64_t arg) { (void)file, (void)format, (void)arg; g_fatal++; siglongjmp(g_jump, 2); }
void lift_fatal(const char *what, uint64_t at) { (void)what, (void)at; g_fatal++; siglongjmp(g_jump, 2); }

static uint64_t rng = 88172645463325252ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static float frand(void) { return (float)((next() >> 11) * (1.0 / 9007199254740992.0)); }

#define MAX_PAGES 512
static uint64_t pages[MAX_PAGES];
static int npages;
static int map_page(uint64_t p) {
  if (p >= g_lo && p < g_hi) return 1;
  for (int i = 0; i < npages; i++) if (pages[i] == p) return 1;
  if (npages == MAX_PAGES) return 0;
  if (mmap((void *)p, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != (void *)p) return 0;
  pages[npages++] = p;
  return 1;
}

static void mutate_float(uint8_t *p) {
  float f;
  memcpy(&f, p, 4);
  switch (next() % 8) {
    case 0: f = -f; break;
    case 1: f = 0; break;
    case 2: f *= frand() * 4 - 2; break;
    case 3: f += frand() * 2 - 1; break;
    case 4: f = frand() * 10 - 5; break;
    case 5: f *= 1000; break;
    case 6: f = 1; break;
    default: f = (frand() - 0.5f) * 1e-3f; break;
  }
  memcpy(p, &f, 4);
}
static int floatish(uint32_t w) {
  uint32_t e = (w >> 23) & 0xff;
  return w == 0 || w == 0x80000000u || (e > 90 && e < 160);
}

int main(int argc, char **argv) {
  if (argc < 6) { fprintf(stderr, "fuzz LIBSHELL IN.rec OUT.rec PER_SAMPLE SEED [COVER]\n"); return 2; }
  int fd = open(argv[2], O_RDONLY);
  struct stat st;
  fstat(fd, &st);
  uint8_t *rec = malloc(st.st_size);
  if (read(fd, rec, st.st_size) != st.st_size) { perror("read"); return 1; }
  int per = atoi(argv[4]);
  rng ^= strtoull(argv[5], 0, 0) * 0x9E3779B97F4A7C15ull;
  record_open(argv[3]);
  struct sigaction sa = {0};
  sa.sa_handler = on_fault;
  sa.sa_flags = SA_NODEFER;
  sigaction(SIGSEGV, &sa, 0);
  sigaction(SIGBUS, &sa, 0);
  uint8_t *tls = calloc(1, 0x100);
  memset(tls + 0x28, 0x5a, 8);
  size_t at = 0;
  int made = 0, faulted = 0, unmappable = 0;
  while (at < (size_t)st.st_size) {
    struct header *h = (struct header *)(rec + at);
    struct op *ops = (struct op *)(h + 1);
    at += sizeof *h + sizeof(struct op) * h->ops;
    if (!g_image) map_library(argv[1], h->base);
    void *fn = lifted_for(h->function);
    if (!fn) continue;
    for (int m = 0; m < per; m++) {
      npages = 0;
      int ok = 1;
      for (uint32_t i = 0; i < h->ops && ok; i = next_op(ops, i)) {
        if (ops[i].kind == OP_CALL || ops[i].addr == h->tls + 0x28) continue;
        for (uint64_t p = ops[i].addr & ~0xfffull; p < ops[i].addr + ops[i].size && ok; p += 0x1000) ok = map_page(p);
      }
      for (uint64_t p = (h->in.sp & ~0xfffull) - 0x2000; p <= (h->in.sp & ~0xfffull) && ok; p += 0x1000) ok = map_page(p);
      if (!ok) { unmappable++; goto cleanup; }
      // Initial memory, as in replay.c; remember the words loaded before any store.
      static uint64_t cand[4096];
      int ncand = 0;
      static uint64_t written[65536];
      int nwritten = 0;
      for (uint32_t i = 0; i < h->ops; i = next_op(ops, i)) {
        if (ops[i].kind == OP_CALL) break;  // a variant goes no further than its first guest call
        if (ops[i].addr == h->tls + 0x28) continue;
        for (int k = 0; k < ops[i].size; k++) {
          uint64_t a = ops[i].addr + k;
          int seen = 0;
          for (int j = 0; j < nwritten && !seen; j++) seen = written[j] == a;
          if (seen) continue;
          if (ops[i].kind == 0) *(uint8_t *)a = ops[i].value[k];
          if (nwritten < 65536) written[nwritten++] = a;
        }
        if (ops[i].kind == 0 && ops[i].size >= 4)
          for (int k = 0; k + 4 <= ops[i].size && ncand < 4096; k += 4) cand[ncand++] = ops[i].addr + k;
      }
      uint8_t ts[0x400] = {0};
      memcpy(ts, h->in.x, 8 * 31);
      memcpy(ts + 0x108, &h->in.sp, 8);
      memcpy(ts + 0x110, h->in.v, 512);
      uint64_t t = (uint64_t)tls;
      memcpy(ts + 0x358, &t, 8);
      if (m > 0) {
        int changes = 1 + (int)(next() % 4);
        for (int c = 0; c < changes; c++) {
          int r = (int)(next() % (ncand + 16));
          if (r >= ncand + 8) {  // a small integer argument (a shape type)
            uint64_t *x = (uint64_t *)ts + (r - ncand - 8);
            if (*x < 8) *x = next() % 7;
            continue;
          }
          if (r >= ncand) {  // a float argument register
            mutate_float(ts + 0x110 + 16 * (r - ncand));
            continue;
          }
          uint32_t w;
          memcpy(&w, (void *)cand[r], 4);
          if (w < 8) { w = (uint32_t)(next() % 6); memcpy((void *)cand[r], &w, 4); }
          else if (floatish(w)) mutate_float((uint8_t *)cand[r]);
        }
      }
      if (sigsetjmp(g_jump, 1) == 0) {
        record_call(fn, ts, h->function);
        made++;
      } else {
        record_abandon();
        faulted++;
      }
    cleanup:
      for (int i = 0; i < npages; i++) munmap((void *)pages[i], 0x1000);
      for (uint32_t i = 0; i < h->ops; i = next_op(ops, i))
        if (ops[i].kind != OP_CALL && ops[i].addr >= g_lo && ops[i].addr < g_hi) {
          uint64_t p = ops[i].addr & ~0xfffull;
          memcpy((void *)p, g_image + (p - g_lo), 0x1000);
        }
    }
  }
  if (argc > 6) record_dump_cover(argv[6]);
  fprintf(stderr, "%d variants recorded; dropped: %d reaching a fatal assert, %d a guest call, %d touching memory "
          "the recording didn't have, %d with pages this process can't map\n", made, g_fatal, record_guest_calls(),
          faulted - g_fatal - record_guest_calls(), unmappable);
  return 0;
}
