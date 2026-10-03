/*
 * Prism's tracking service: the head pose Horizon's compositor and apps read, which a headset's
 * trackingservice computes from its cameras and IMU.
 *
 * Tracking data reaches clients as shared memory regions, handed out by Meta's MemoryBroker
 * (oculus.internal.tracking.IMemoryBrokerService/default). On a headset system_server hosts the
 * broker, loading Meta's arm64 libmemorybroker.so from a JNI library; system_server is x86_64
 * here, so this arm64 daemon (run by the translator) hosts it instead (`prism_tracking broker`).
 * In a second process (`prism_tracking host`) it registers with the broker as the host of the
 * head tracker's region, as trackingservice does, and keeps a headset that sits still at the
 * tracking origin in it.
 *
 * The head tracker's region (libtrackingserviceclients' HeadTrackerSharedMemory, 0x1680 bytes):
 *   0x0000  the latest state: two sequence words, then two copies of HeadState (seqlock)
 *   0x0148  a sequence word and a history of 32 HeadStates, for interpolating past poses
 *   0x1588  two sequence words and two copies of a mode word (seqlock); in mode 1 a reader
 *           returns the latest state as of the time it asks for, unextrapolated
 * HeadState (0xa0 bytes) holds a valid flag, the orientation (a quaternion, x y z w) and
 * position, and its time (ns, CLOCK_MONOTONIC); a second quaternion at 0x80 must be a unit one
 * too for the state to count as valid.
 *
 * The binder thread-pool calls are platform APIs the NDK doesn't declare, so they're looked up.
 */
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TAG "PrismTracking"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define ERR(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define BROKER_LIB "libmemorybroker.so"
#define BROKER_REGISTER "_ZN12memorybroker15registerServiceEv"  // memorybroker::registerService()
#define HOST_LIB "libhzos_trackinghost.meta.so"                  // the broker's C API for hosts

// MemoryBroker_HostConnection_registerSubRegion's argument. ready is called with the region's
// memory whenever the broker (re)allocates it: private memory first, the broker's once connected.
struct region_info {
  uint32_t type;       // the C API's numbering, not SharedMemoryType's
  uint32_t specifier;  // 0: none
  uint32_t size;
  uint32_t reserved;
  void *context;
  void (*ready)(void *context, void *memory);
};
#define HEAD_TRACKER 8  // SharedMemoryType HEAD_TRACKER (11)
#define REGION_SIZE 0x1680

struct head_state {
  uint8_t valid;
  uint8_t reserved0[15];
  float orientation[4];  // x y z w
  float position[3];
  uint8_t reserved1[0x34];
  int64_t time_ns;
  int64_t sample_time_ns;
  uint8_t reserved2[0x10];
  float reference_orientation[4];  // x y z w
  uint8_t reserved3[0x10];
};
_Static_assert(sizeof(struct head_state) == 0xa0, "HeadState is 0xa0 bytes");

struct seqlock {
  _Atomic uint32_t done, begun;  // a reader takes copy[begun & 1] and checks done == begun
};

#define LATEST 0x0000
#define MODE 0x1588
#define MODE_LATEST 1
#define RATE_HZ 100

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;  // the region can move under the writer
static uint8_t *g_region;

static int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Writes the copy readers aren't told to take, then points them at it.
static void publish(uint8_t *at, const void *value, size_t size, size_t stride) {
  struct seqlock *lock = (struct seqlock *)at;
  uint32_t next = atomic_load_explicit(&lock->begun, memory_order_relaxed) + 1;
  memcpy(at + sizeof *lock + (next & 1) * stride, value, size);
  atomic_store_explicit(&lock->begun, next, memory_order_release);
  atomic_store_explicit(&lock->done, next, memory_order_release);
}

static void publish_head(uint8_t *region) {
  struct head_state state = {
      .valid = 1,
      .orientation = {0, 0, 0, 1},
      .reference_orientation = {0, 0, 0, 1},
  };
  state.time_ns = state.sample_time_ns = now_ns();
  publish(region + LATEST, &state, sizeof state, sizeof state);
}

static void region_ready(void *context, void *memory) {
  (void)context;
  uint8_t *region = memory;
  uint32_t mode = MODE_LATEST;
  pthread_mutex_lock(&g_lock);
  memset(region, 0, REGION_SIZE);
  publish(region + MODE, &mode, sizeof mode, sizeof mode);
  publish_head(region);
  g_region = region;
  pthread_mutex_unlock(&g_lock);
  LOG("head tracker region at %p", memory);
}

static void *open_symbol(const char *lib, const char *name) {
  void *handle = dlopen(lib, RTLD_NOW);
  void *symbol = handle ? dlsym(handle, name) : NULL;
  if (!symbol) ERR("%s: %s", lib, dlerror());
  return symbol;
}

static int start_binder(void) {
  int (*set_max_threads)(uint32_t) = open_symbol("libbinder_ndk.so", "ABinderProcess_setThreadPoolMaxThreadCount");
  void (*start_thread_pool)(void) = open_symbol("libbinder_ndk.so", "ABinderProcess_startThreadPool");
  if (!set_max_threads || !start_thread_pool) return 0;
  set_max_threads(4);
  start_thread_pool();
  return 1;
}

// The broker, as system_server hosts it on a headset.
static int run_broker(void) {
  void (*register_broker)(void) = open_symbol(BROKER_LIB, BROKER_REGISTER);
  if (!register_broker || !start_binder()) return 1;
  register_broker();
  LOG("MemoryBroker registered");
  for (;;) pause();
}

// The tracking host, as trackingservice is: in a process of its own, since a host in the broker's
// process registers twice (directly, and again when told the broker exists), which the broker
// refuses.
static int run_host(void) {
  void *(*host_create)(void) = open_symbol(HOST_LIB, "MemoryBroker_HostConnection_create");
  int (*register_region)(void *, const struct region_info *) =
      open_symbol(HOST_LIB, "MemoryBroker_HostConnection_registerSubRegion");
  if (!host_create || !register_region || !start_binder()) return 1;
  struct region_info head = {.type = HEAD_TRACKER, .size = REGION_SIZE, .ready = region_ready};
  void *host = host_create();
  if (!host || !register_region(host, &head)) {
    ERR("can't register the head tracker region");
    return 1;
  }
  for (;;) {
    pthread_mutex_lock(&g_lock);
    if (g_region) publish_head(g_region);
    pthread_mutex_unlock(&g_lock);
    usleep(1000000 / RATE_HZ);
  }
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "broker")) return run_broker();
  if (argc == 2 && !strcmp(argv[1], "host")) return run_host();
  ERR("usage: %s broker|host", argv[0]);
  return 2;
}
