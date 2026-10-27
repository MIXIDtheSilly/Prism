// insn_test: runs single instructions on generated register, flag and memory states and writes a
// hash of what each leaves, the same program for both sides (tools/lift_check.py insns): built
// for arm64 it runs the instructions themselves (translated by the emulator's translator,
// insn_arm64.S), built for x86_64 their lifted C (insn_x86.c, generated). Same states on both: the
// generator is seeded by the test and input numbers.
//
//     insn_test OUT.BIN INPUTS          two hashes per test and input (exact, and NaNs as one)
//     insn_test --dump TEST INPUT OUT   the state and memory window one leaves
//
// insn_meta.h (generated): per test, the memory operand's base register (-1 none, 31 sp), its
// index register (-1 none) and the immediate offset, so the access lands in a window of BUF this
// fills and hashes.
#define _GNU_SOURCE
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct tstate { uint64_t x[31]; uint64_t sp; uint64_t nzcv; uint8_t v[32][16]; };
struct meta { int base, index, offset; };
#include "insn_meta.h"  // static const struct meta g_meta[]; enum { NTESTS = ... };

#define BUF 0x40000000ull
#define BUF_SIZE 0x40000ull
#define CENTER (BUF + 0x20000)
#define WINDOW 1024
int run_test(int k, const struct tstate *in, struct tstate *out);  // 0, or 1 if it faulted

static uint64_t rng;
static uint64_t next(void) {  // xorshift64*
  rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
  return rng * 0x2545F4914F6CDD1Dull;
}
static uint64_t gen_x(void) {
  uint64_t r = next();
  switch (r % 13) {
    case 0: return 0;
    case 1: return next() % 16;
    case 2: return (uint64_t)-(int64_t)(1 + next() % 16);
    case 3: return next() & 0xffffffffull;
    case 4: return 1ull << (next() % 64);
    case 5: return 0x7fffffffull + (next() % 3) - 1;
    case 6: return 0xffffffff80000000ull + (next() % 3) - 1;
    case 7: return (next() & 1) ? 0x8000000000000000ull : 0x7fffffffffffffffull;
    case 8: return next() & 0xffff;
    case 9: return (1ull << (next() % 64)) - 1;
    case 10: return 0xffffffffull;
    case 11: return next() & 0xff;
    default: return next();
  }
}
static uint32_t gen_f(void) {
  static const uint32_t special[] = {0, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u,
                                     0x7fa12345u, 0x00012345u, 0x80000001u, 0x3f800000u, 0xbf800000u, 0x3f000000u,
                                     0x7f7fffffu, 0x4f000000u, 0xcf000000u, 0x5f000000u, 0x4f800000u, 0x40200000u,
                                     0xc0600000u, 0x3fc00000u, 0x00800000u};
  uint64_t r = next() % 10;
  float f;
  if (r < 3) { uint32_t s = special[next() % (sizeof special / sizeof *special)]; return s; }
  if (r < 6) {  // (one next() a statement: C leaves the order of two in an expression open)
    int64_t num = (int64_t)(next() % 2001) - 1000;
    f = (float)num / (float)(1 + next() % 8);
  }
  else if (r < 8) f = ((float)(next() % 1000000) / 250000.0f - 2.0f);
  else if (r < 9) f = (float)((int64_t)(next() % 200) - 100) + 0.5f;
  else { uint32_t b = (uint32_t)next(); return b; }
  uint32_t b;
  memcpy(&b, &f, 4);
  return b;
}
static uint64_t gen_d(void) {
  static const uint64_t special[] = {0, 0x8000000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
                                     0x7ff8000000000000ull, 0x3ff0000000000000ull, 0x41e0000000000000ull,
                                     0xc1e0000000000000ull, 0x43e0000000000000ull, 0x0000000000012345ull};
  uint64_t r = next() % 8;
  if (r < 2) return special[next() % (sizeof special / sizeof *special)];
  double d;
  if (r < 6) {
    int64_t num = (int64_t)(next() % 2000001) - 1000000;
    d = (double)num / (double)(1 + next() % 64);
  } else {
    d = (double)(int64_t)next() / 1e6;
  }
  uint64_t b;
  memcpy(&b, &d, 8);
  return r == 7 ? next() : b;
}

static void generate(int k, int j, struct tstate *s) {
  rng = 0x9E3779B97F4A7C15ull * (uint64_t)(k + 1) ^ 0xD1B54A32D192ED03ull * (uint64_t)(j + 1);
  for (int i = 0; i < 4; i++) next();
  for (int i = 0; i < 31; i++) s->x[i] = gen_x();
  s->sp = CENTER + 16 * (next() % 64) - 512;
  s->nzcv = (next() & 0xf) << 28;
  for (int i = 0; i < 32; i++) {
    uint64_t r = next() % 10;
    if (r < 2) {
      uint64_t a = gen_d(), b = gen_d();
      memcpy(s->v[i], &a, 8); memcpy(s->v[i] + 8, &b, 8);
    } else if (r < 3) {
      uint64_t a = next(), b = next();
      memcpy(s->v[i], &a, 8); memcpy(s->v[i] + 8, &b, 8);
    } else {
      for (int l = 0; l < 4; l++) { uint32_t f = gen_f(); memcpy(s->v[i] + 4 * l, &f, 4); }
    }
  }
  const struct meta *m = &g_meta[k];
  uint8_t *window = (uint8_t *)(CENTER - WINDOW / 2);
  memset(window, 0, WINDOW);
  if (m->base >= 0) {
    uint64_t base = CENTER - m->offset + 16 * (next() % 16) - 128;
    if (m->base == 31) s->sp = base & ~15ull; else s->x[m->base] = base;
    if (m->index >= 0) s->x[m->index] = next() % 16;  // any extend and scale stays in the buffer (a fault the translator can't always recover)
    for (int i = 0; i < WINDOW; i += 4) {
      uint32_t w = (next() % 3) ? gen_f() : (uint32_t)gen_x();
      memcpy(window + i, &w, 4);
    }
  }
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
  const uint8_t *b = p;
  while (n--) h = (h ^ *b++) * 0x100000001b3ull;
  return h;
}
// nan: every vector register word that is a float NaN as one NaN (the second hash: a NaN's sign
// and payload, which the two sides' float units choose differently, aside).
static uint64_t hash(const struct tstate *s, int memory, int nan) {
  struct tstate c = *s;
  c.x[16] = c.x[18] = 0;  // the arm64 side's test address and way back
  c.nzcv &= 0xf0000000ull;
  if (nan)
    for (int r = 0; r < 32; r++)
      for (int l = 0; l < 4; l++) {
        uint32_t w;
        memcpy(&w, c.v[r] + 4 * l, 4);
        if ((w & 0x7f800000u) == 0x7f800000u && (w & 0x7fffffu)) w = 0x7fc00000u;
        memcpy(c.v[r] + 4 * l, &w, 4);
      }
  uint64_t h = fnv(0xcbf29ce484222325ull, &c, sizeof c);
  if (memory) h = fnv(h, (void *)(CENTER - WINDOW / 2), WINDOW);
  return h;
}

static sigjmp_buf g_jump;
static void on_fault(int sig) { (void)sig; siglongjmp(g_jump, 1); }
static int guarded(int k, const struct tstate *in, struct tstate *out) {
  if (sigsetjmp(g_jump, 1)) return 1;
  return run_test(k, in, out);
}

int main(int argc, char **argv) {
  if (mmap((void *)BUF, BUF_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != (void *)BUF) {
    perror("map the buffer");
    return 1;
  }
  struct sigaction sa = {0};
  sa.sa_handler = on_fault;
  sa.sa_flags = SA_NODEFER;
  sigaction(SIGSEGV, &sa, 0);
  sigaction(SIGBUS, &sa, 0);
  sigaction(SIGILL, &sa, 0);
  struct tstate in, out;
  if (argc == 5 && !strcmp(argv[1], "--dump")) {
    int k = atoi(argv[2]), j = atoi(argv[3]);
    generate(k, j, &in);
    memset(&out, 0, sizeof out);
    int faulted = guarded(k, &in, &out);
    FILE *f = fopen(argv[4], "wb");
    fwrite(&faulted, 4, 1, f);
    fwrite(&out, sizeof out, 1, f);
    fwrite((void *)(CENTER - WINDOW / 2), WINDOW, 1, f);
    generate(k, j, &in);
    fwrite(&in, sizeof in, 1, f);
    fwrite((void *)(CENTER - WINDOW / 2), WINDOW, 1, f);
    fclose(f);
    return 0;
  }
  if (argc != 3) { fprintf(stderr, "insn_test OUT.BIN INPUTS | --dump TEST INPUT OUT\n"); return 2; }
  int inputs = atoi(argv[2]);
  FILE *f = fopen(argv[1], "wb");
  if (!f) { perror(argv[1]); return 1; }
  int trace = getenv("INSN_TRACE") != NULL;  // which test was running when it crashed
  for (int k = 0; k < NTESTS; k++)
    for (int j = 0; j < inputs; j++) {
      if (trace) { fprintf(stderr, "%d %d\n", k, j); fflush(stderr); }
      generate(k, j, &in);
      memset(&out, 0, sizeof out);
      uint64_t h[2] = {0xfa017edull, 0xfa017edull};
      if (!guarded(k, &in, &out)) {
        h[0] = hash(&out, g_meta[k].base >= 0, 0);
        h[1] = hash(&out, g_meta[k].base >= 0, 1);
      }
      fwrite(h, 8, 2, f);
    }
  fclose(f);
  return 0;
}
