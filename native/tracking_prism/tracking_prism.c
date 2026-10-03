/*
 * Prism's tracking service: the head pose Horizon's compositor and apps read, which a headset's
 * trackingservice computes from its cameras and IMU.
 *
 * Tracking data reaches clients as shared memory regions, handed out by Meta's MemoryBroker
 * (oculus.internal.tracking.IMemoryBrokerService/default). On a headset system_server hosts the
 * broker, loading Meta's arm64 libmemorybroker.so from a JNI library; system_server is x86_64
 * here, so this arm64 daemon (run by the translator) hosts it instead (`prism_tracking broker`).
 * In a second process (`prism_tracking host`) it registers with the broker as the host of the
 * head tracker's region, as trackingservice does, and keeps the headset's pose in it: still at the
 * tracking origin, until a client on the PC moves it (tools/head.py, through `adb forward` to
 * POSE_PORT: lines of seven numbers, the position in meters and the orientation quaternion,
 * px py pz qx qy qz qw, in OpenXR's axes, Y up and -Z ahead).
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
 * It hosts the regions trackingservice keeps for hands and input devices too, in the state a
 * headset's are in before any sample: no hands tracked, no input devices. Without a host, their
 * clients (the compositor, VrShell) ask the broker again and again.
 *   HAND_TRACKER, left and right (0x13e8 bytes each): two hand snapshots (0x7c8 apart, at 8) and
 *     two hand configurations (0x68 apart, at 0x1308), whose times, -1, mean no sample yet
 *   INPUT_TYPE_MAP (0xc8 bytes): the input devices' ids and the regions they're in, none
 *
 * The binder thread-pool calls are platform APIs the NDK doesn't declare, so they're looked up.
 */
#include <android/log.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
#define HAND_TRACKER 7  // SharedMemoryType HAND_TRACKER (10)
#define HAND_SIZE 0x13e8
#define INPUT_TYPE_MAP 13  // SharedMemoryType INPUT_TYPE_MAP (13)
#define INPUT_MAP_SIZE 0xc8
#define LEFT 1   // the C API's specifiers
#define RIGHT 2

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
#define POSE_PORT 7340  // on the guest's loopback

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;  // the region can move under the writer
static uint8_t *g_region;
static float g_position[3], g_orientation[4] = {0, 0, 0, 1};  // the head's pose

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
      .reference_orientation = {0, 0, 0, 1},
  };
  memcpy(state.orientation, g_orientation, sizeof g_orientation);
  memcpy(state.position, g_position, sizeof g_position);
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

// Reads poses from one client at a time; the last one stays when it goes.
static void *serve_poses(void *unused) {
  (void)unused;
  int server = socket(AF_INET, SOCK_STREAM, 0);
  int on = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  struct sockaddr_in at = {.sin_family = AF_INET, .sin_port = htons(POSE_PORT), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (server < 0 || bind(server, (struct sockaddr *)&at, sizeof at) || listen(server, 1)) {
    ERR("no pose port %d", POSE_PORT);
    return NULL;
  }
  for (;;) {
    int client = accept(server, NULL, NULL);
    FILE *in = client >= 0 ? fdopen(client, "r") : NULL;
    if (!in) continue;
    LOG("pose client connected");
    char line[256];
    while (fgets(line, sizeof line, in)) {
      float p[3], q[4];
      if (sscanf(line, "%f %f %f %f %f %f %f", &p[0], &p[1], &p[2], &q[0], &q[1], &q[2], &q[3]) != 7) continue;
      float norm = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
      if (!(norm > 0.5f && norm < 2)) continue;
      pthread_mutex_lock(&g_lock);
      memcpy(g_position, p, sizeof p);
      for (int i = 0; i < 4; i++) g_orientation[i] = q[i] / norm;
      pthread_mutex_unlock(&g_lock);
    }
    fclose(in);
    LOG("pose client gone");
  }
}

static void put_i64(uint8_t *at, int64_t v) { memcpy(at, &v, sizeof v); }
static void put_f32(uint8_t *at, float v) { memcpy(at, &v, sizeof v); }

// A hand region as trackingservice first allocates it: no sample yet.
static void hand_ready(void *context, void *memory) {
  uint8_t *region = memory;
  memset(region, 0, HAND_SIZE);
  for (size_t at = 8; at < 0xf98; at += 0x7c8) {  // the two snapshots
    put_i64(region + at + 0x008, -1);
    put_i64(region + at + 0x010, -1);  // the sample's time
    for (int i = 0; i < 6; i++) put_i64(region + at + 0x470 + i * 0x80, -1);  // history
    uint16_t status = 0x01ff;
    memcpy(region + at + 0x740, &status, sizeof status);
    put_i64(region + at + 0x790, -1);
    put_i64(region + at + 0x798, -1);
    put_i64(region + at + 0x7a0, -1);
    put_f32(region + at + 0x7b4, 1.0f);  // an identity quaternion, w first
  }
  for (size_t at = 0x1308; at <= 0x1370; at += 0x68) {  // the two configurations
    put_f32(region + at + 4, 1.0f);
    put_i64(region + at + 8, -1);
  }
  LOG("%s hand region at %p", context ? "right" : "left", memory);
}

// No input devices: both copies of the map empty.
static void input_map_ready(void *context, void *memory) {
  (void)context;
  memset(memory, 0, INPUT_MAP_SIZE);
  LOG("input map region at %p", memory);
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
  // One host connection holds several regions, each a type and specifier of its own.
  const struct region_info others[] = {
      {.type = HAND_TRACKER, .specifier = LEFT, .size = HAND_SIZE, .context = NULL, .ready = hand_ready},
      {.type = HAND_TRACKER, .specifier = RIGHT, .size = HAND_SIZE, .context = (void *)1, .ready = hand_ready},
      {.type = INPUT_TYPE_MAP, .size = INPUT_MAP_SIZE, .ready = input_map_ready},
  };
  for (size_t i = 0; i < sizeof others / sizeof *others; i++)
    if (!register_region(host, &others[i])) ERR("can't register region type %u/%u", others[i].type, others[i].specifier);
  pthread_t poses;
  pthread_create(&poses, NULL, serve_poses, NULL);
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
