// Recording build: wraps each guest call into a lifted function, logging registers in and out and
// every memory access, for replay against the original arm64 code (replay.c). In VrShell (the
// proxy's recording build) it starts when debug.prism.lift_record is 1 and takes every 5th call
// until WANT; with FUZZ (fuzz.c) it records every call it's given. A call the lifted code makes to
// guest code (lift_guest_call) is logged as one op, the registers it went in and came out with
// after it; what that code does is the translator's, not recorded (nor lifted calls it makes).
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/system_properties.h>

#undef sincosf
void sincosf(float, float *, float *);
struct op { uint8_t kind, size; uint8_t pad[6]; uint64_t addr; uint8_t value[16]; };
struct regs { uint64_t x[31]; uint64_t sp; uint8_t v[32][16]; };
struct header { uint32_t magic, function, ops, pad; uint64_t base, tls, canary; struct regs in, out; };  // canary: the stack guard at tls + 0x28
enum { OP_LOAD, OP_STORE, OP_CALL };  // a call is followed by CALL_SLOTS ops: its registers in, out
#define CALL_SLOTS (2 * sizeof(struct regs) / sizeof(struct op))

#define MAX_OPS 16384
static struct op t_ops[MAX_OPS];  // the sample being recorded, under g_record
static int t_count, t_overflow;
static __thread int t_active, t_depth;
static pthread_mutex_t g_record = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_fd = -1, g_samples, g_calls;
#define WANT 6000
extern uint64_t BASE;

// Blocks of the lifted code that ran while recording.
static uint32_t g_cover[16384];
static int g_ncover;
void rec_cover(uint64_t a) {
  if (!t_active) return;
  for (int i = 0; i < g_ncover; i++) if (g_cover[i] == (uint32_t)a) return;
  if (g_ncover < 16384) g_cover[g_ncover++] = (uint32_t)a;
}

static void log_op(int kind, uint64_t a, const void *v, int size) {
  if (!t_active) return;
  if (t_count >= MAX_OPS) { t_overflow = 1; return; }
  struct op *o = &t_ops[t_count++];
  o->kind = kind; o->size = size; o->addr = a;
  memset(o->value, 0, 16);
  memcpy(o->value, v, size);
}
uint8_t rec_ld8(uint64_t a) { uint8_t v = *(uint8_t *)a; log_op(OP_LOAD, a, &v, 1); return v; }
uint16_t rec_ld16(uint64_t a) { uint16_t v; memcpy(&v, (void *)a, 2); log_op(OP_LOAD, a, &v, 2); return v; }
uint32_t rec_ld32(uint64_t a) { uint32_t v; memcpy(&v, (void *)a, 4); log_op(OP_LOAD, a, &v, 4); return v; }
uint64_t rec_ld64(uint64_t a) { uint64_t v; memcpy(&v, (void *)a, 8); log_op(OP_LOAD, a, &v, 8); return v; }
void rec_ldq(void *r, uint64_t a) { memcpy(r, (void *)a, 16); log_op(OP_LOAD, a, r, 16); }
void rec_st8(uint64_t a, uint8_t v) { *(uint8_t *)a = v; log_op(OP_STORE, a, &v, 1); }
void rec_st16(uint64_t a, uint16_t v) { memcpy((void *)a, &v, 2); log_op(OP_STORE, a, &v, 2); }
void rec_st32(uint64_t a, uint32_t v) { memcpy((void *)a, &v, 4); log_op(OP_STORE, a, &v, 4); }
void rec_st64(uint64_t a, uint64_t v) { memcpy((void *)a, &v, 8); log_op(OP_STORE, a, &v, 8); }
void rec_stq(uint64_t a, const void *r) { memcpy((void *)a, r, 16); log_op(OP_STORE, a, r, 16); }
int rec_cas(uint64_t a, void *expected, uint64_t v, int size) {
  int ok = 0;
  switch (size) {
    case 1: ok = __atomic_compare_exchange_n((uint8_t *)a, (uint8_t *)expected, (uint8_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); break;
    case 2: ok = __atomic_compare_exchange_n((uint16_t *)a, (uint16_t *)expected, (uint16_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); break;
    case 4: ok = __atomic_compare_exchange_n((uint32_t *)a, (uint32_t *)expected, (uint32_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); break;
    default: ok = __atomic_compare_exchange_n((uint64_t *)a, (uint64_t *)expected, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); break;
  }
  if (ok) {  // it read what it expected, then wrote
    log_op(OP_LOAD, a, expected, size);
    log_op(OP_STORE, a, &v, size);
  } else {
    log_op(OP_LOAD, a, expected, size);
  }
  return ok;
}
__attribute__((target("cx16"))) unsigned __int128 rec_ld128(uint64_t a) {
  unsigned __int128 v = 0;
  __atomic_compare_exchange_n((unsigned __int128 *)a, &v, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  log_op(OP_LOAD, a, &v, 16);
  return v;
}
__attribute__((target("cx16"))) int rec_cas128(uint64_t a, unsigned __int128 e, unsigned __int128 v) {
  int ok = __atomic_compare_exchange_n((unsigned __int128 *)a, &e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  log_op(OP_LOAD, a, &e, 16);
  if (ok) log_op(OP_STORE, a, &v, 16);
  return ok;
}
// memmove and memset as the loads and stores they are (in order: a move reads all, then writes).
void rec_copy(uint64_t d, uint64_t s, uint64_t n) {
  for (uint64_t k = 0; k < n; k += 16) log_op(OP_LOAD, s + k, (void *)(s + k), n - k < 16 ? (int)(n - k) : 16);
  memmove((void *)d, (void *)s, n);
  for (uint64_t k = 0; k < n; k += 16) log_op(OP_STORE, d + k, (void *)(d + k), n - k < 16 ? (int)(n - k) : 16);
}
void rec_set(uint64_t d, uint8_t c, uint64_t n) {
  memset((void *)d, c, n);
  for (uint64_t k = 0; k < n; k += 16) log_op(OP_STORE, d + k, (void *)(d + k), n - k < 16 ? (int)(n - k) : 16);
}
void rec_sincosf(float x, float *s, float *c) {
  sincosf(x, s, c);
  log_op(OP_STORE, (uint64_t)s, s, 4);
  log_op(OP_STORE, (uint64_t)c, c, 4);
}

static void snapshot(struct regs *r, const uint8_t *ts) {
  memcpy(r->x, ts, 8 * 31);
  memcpy(&r->sp, ts + 0x108, 8);
  memcpy(r->v, ts + 0x110, 512);
}

static void log_call(uint64_t target, const struct regs *in, const struct regs *out) {
  if (t_count + 1 + (int)CALL_SLOTS > MAX_OPS) { t_overflow = 1; return; }
  struct op *o = &t_ops[t_count];
  memset(o, 0, sizeof *o);
  o->kind = OP_CALL; o->size = CALL_SLOTS; o->addr = target;
  memcpy(o + 1, in, sizeof *in);
  memcpy((uint8_t *)(o + 1) + sizeof *in, out, sizeof *out);
  t_count += 1 + CALL_SLOTS;
}

static void write_sample(struct header *h) {
  h->ops = t_count;
  if (write(g_fd, h, sizeof *h) != sizeof *h ||
      write(g_fd, t_ops, sizeof(struct op) * t_count) != (ssize_t)(sizeof(struct op) * t_count))
    __android_log_print(ANDROID_LOG_WARN, "PrismLift", "recording: short write");
}

#ifdef FUZZ
// fuzz.c runs no guest code: a variant that would call some is dropped.
extern sigjmp_buf g_jump;
static int g_guest_calls;
void lift_guest_call(uint8_t *ts, uint64_t target) {
  (void)ts, (void)target;
  g_guest_calls++;
  siglongjmp(g_jump, 3);
}
void lift_guest_call_stop(uint8_t *ts, uint64_t target) { lift_guest_call(ts, target); }
struct prism_lifted { uint32_t offset, nranges; const uint32_t *ranges; uint64_t fnv; void (*lifted)(uint8_t *);
                      const char *name; uint32_t nstops; const uint32_t *stops; };
extern const struct prism_lifted prism_lifted[];
extern const unsigned prism_lifted_count;
void (*lift_find(uint64_t address))(uint8_t *) {
  for (unsigned i = 0; i < prism_lifted_count; i++)
    if (BASE + prism_lifted[i].offset == address) return prism_lifted[i].lifted;
  return NULL;
}
int record_guest_calls(void) { return g_guest_calls; }
void record_open(const char *path) { g_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); }
void record_dump_cover(const char *path) {
  int cf = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (cf >= 0) { write(cf, g_cover, 4 * g_ncover); close(cf); }
}
// A variant that faulted or hit a fatal path: drop it.
void record_abandon(void) { t_active = 0; }
void record_call(void *lifted, void *thread_state, uint32_t function) {
  uint8_t *ts = thread_state;
  struct header h = {0x4345524c, function, 0, 0, BASE, 0};
  memcpy(&h.tls, ts + 0x358, 8);
  memcpy(&h.canary, (void *)(h.tls + 0x28), 8);
  snapshot(&h.in, ts);
  t_count = 0; t_overflow = 0; t_active = 1;
  ((void (*)(uint8_t *))lifted)(ts);
  t_active = 0;
  snapshot(&h.out, ts);
  if (!t_overflow) write_sample(&h);
}
#else
void prism_guest_call(uint8_t *ts, uint64_t target);  // proxy_libshell.c
void prism_guest_call_stop(uint8_t *ts, uint64_t target);
static void guest_call(uint8_t *ts, uint64_t target, void (*call)(uint8_t *, uint64_t)) {
  if (!t_active) { call(ts, target); return; }
  struct regs in, out;
  snapshot(&in, ts);
  t_active = 0;  // what the guest code does isn't this sample's
  call(ts, target);
  t_active = 1;
  snapshot(&out, ts);
  log_call(target, &in, &out);
}
void lift_guest_call(uint8_t *ts, uint64_t target) { guest_call(ts, target, prism_guest_call); }
void lift_guest_call_stop(uint8_t *ts, uint64_t target) { guest_call(ts, target, prism_guest_call_stop); }

static int recording_wanted(void) {
  static int calls, on;
  if (on) return 1;
  if (calls++ % 2000) return 0;
  char v[PROP_VALUE_MAX] = "";
  __system_property_get("debug.prism.lift_record", v);
  return on = v[0] == '1';
}

void record_call(void *lifted, void *thread_state, uint32_t function) {
  uint8_t *ts = thread_state;
  int take = 0;
  if (t_depth) { ((void (*)(uint8_t *))lifted)(ts); return; }  // inside a recorded call's guest call
  pthread_mutex_lock(&g_lock);
  if (g_samples < WANT && recording_wanted() && (g_calls++ % 5) == 0) take = 1;
  pthread_mutex_unlock(&g_lock);
  // One sample at a time; another thread's is skipped, not waited for (its guest calls run guest
  // code, which might wait for this thread).
  if (!take || pthread_mutex_trylock(&g_record)) { ((void (*)(uint8_t *))lifted)(ts); return; }
  struct header h = {0x4345524c, function, 0, 0, BASE, 0};
  memcpy(&h.tls, ts + 0x358, 8);
  memcpy(&h.canary, (void *)(h.tls + 0x28), 8);
  snapshot(&h.in, ts);
  t_count = 0; t_overflow = 0; t_active = 1; t_depth = 1;
  ((void (*)(uint8_t *))lifted)(ts);
  t_active = 0; t_depth = 0;
  snapshot(&h.out, ts);
  if (!t_overflow) {
    pthread_mutex_lock(&g_lock);
    if (g_fd < 0) g_fd = open("/data/data/com.oculus.vrshell/files/lift.rec", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (g_fd >= 0 && g_samples < WANT) {
      write_sample(&h);
      if (++g_samples == WANT) {
        close(g_fd); g_fd = -1;
        int cf = open("/data/data/com.oculus.vrshell/files/lift.cov", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (cf >= 0) { write(cf, g_cover, 4 * g_ncover); close(cf); }
        __android_log_print(ANDROID_LOG_INFO, "PrismLift", "recorded %d samples", WANT);
      }
    }
    pthread_mutex_unlock(&g_lock);
  }
  pthread_mutex_unlock(&g_record);
}
#endif
