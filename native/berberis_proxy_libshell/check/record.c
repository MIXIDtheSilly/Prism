// Recording build: wraps each guest call into a lifted function, logging registers in and out and
// every memory access, for replay against the original arm64 code (replay.c). In VrShell (the
// proxy's recording build) it starts when debug.prism.lift_record is 1 and takes every 5th call
// until WANT; with FUZZ (fuzz.c) it records every call it's given.
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/system_properties.h>

#undef sincosf
void sincosf(float, float *, float *);
struct op { uint8_t kind, size; uint8_t pad[6]; uint64_t addr; uint8_t value[16]; };
struct regs { uint64_t x[31]; uint64_t sp; uint8_t v[32][16]; };
struct header { uint32_t magic, function, ops, pad; uint64_t base, tls; struct regs in, out; };

#define MAX_OPS 8192
static struct op t_ops[MAX_OPS];  // the sample being recorded, under g_record
static int t_count, t_overflow;
static __thread int t_active;
static pthread_mutex_t g_record = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_fd = -1, g_samples, g_calls;
#define WANT 6000
extern uint64_t BASE;

// Blocks of the lifted code that ran while recording.
static uint32_t g_cover[8192];
static int g_ncover;
void rec_cover(uint64_t a) {
  if (!t_active) return;
  for (int i = 0; i < g_ncover; i++) if (g_cover[i] == (uint32_t)a) return;
  if (g_ncover < 8192) g_cover[g_ncover++] = (uint32_t)a;
}

static void log_op(int kind, uint64_t a, const void *v, int size) {
  if (!t_active) return;
  if (t_count >= MAX_OPS) { t_overflow = 1; return; }
  struct op *o = &t_ops[t_count++];
  o->kind = kind; o->size = size; o->addr = a;
  memset(o->value, 0, 16);
  memcpy(o->value, v, size);
}
uint32_t rec_ld32(uint64_t a) { uint32_t v; memcpy(&v, (void *)a, 4); log_op(0, a, &v, 4); return v; }
uint64_t rec_ld64(uint64_t a) { uint64_t v; memcpy(&v, (void *)a, 8); log_op(0, a, &v, 8); return v; }
float rec_ldf(uint64_t a) { float v; memcpy(&v, (void *)a, 4); log_op(0, a, &v, 4); return v; }
void rec_ldq(void *r, uint64_t a) { memcpy(r, (void *)a, 16); log_op(0, a, r, 16); }
void rec_st32(uint64_t a, uint32_t v) { memcpy((void *)a, &v, 4); log_op(1, a, &v, 4); }
void rec_st64(uint64_t a, uint64_t v) { memcpy((void *)a, &v, 8); log_op(1, a, &v, 8); }
void rec_stq(uint64_t a, const void *r) { memcpy((void *)a, r, 16); log_op(1, a, r, 16); }
void rec_sincosf(float x, float *s, float *c) {
  sincosf(x, s, c);
  log_op(1, (uint64_t)s, s, 4);
  log_op(1, (uint64_t)c, c, 4);
}

static void snapshot(struct regs *r, const uint8_t *ts) {
  memcpy(r->x, ts, 8 * 31);
  memcpy(&r->sp, ts + 0x108, 8);
  memcpy(r->v, ts + 0x110, 512);
}

static void write_sample(struct header *h) {
  h->ops = t_count;
  if (write(g_fd, h, sizeof *h) != sizeof *h ||
      write(g_fd, t_ops, sizeof(struct op) * t_count) != (ssize_t)(sizeof(struct op) * t_count))
    __android_log_print(ANDROID_LOG_WARN, "PrismLift", "recording: short write");
}

#ifdef FUZZ
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
  snapshot(&h.in, ts);
  t_count = 0; t_overflow = 0; t_active = 1;
  ((void (*)(uint8_t *))lifted)(ts);
  t_active = 0;
  snapshot(&h.out, ts);
  if (!t_overflow) write_sample(&h);
}
#else
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
  pthread_mutex_lock(&g_lock);
  if (g_samples < WANT && recording_wanted() && (g_calls++ % 5) == 0) take = 1;
  pthread_mutex_unlock(&g_lock);
  if (!take) { ((void (*)(uint8_t *))lifted)(ts); return; }
  struct header h = {0x4345524c, function, 0, 0, BASE, 0};
  memcpy(&h.tls, ts + 0x358, 8);
  snapshot(&h.in, ts);
  pthread_mutex_lock(&g_record);
  t_count = 0; t_overflow = 0; t_active = 1;
  ((void (*)(uint8_t *))lifted)(ts);
  t_active = 0;
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
