/*
 * Prism's tracking service: the head and controller poses Horizon's compositor and apps read, which
 * a headset's trackingservice computes from its cameras and IMUs, and the controllers' input.
 *
 * Tracking data reaches clients as shared memory regions, handed out by Meta's MemoryBroker
 * (oculus.internal.tracking.IMemoryBrokerService/default). On a headset system_server hosts the
 * broker, loading Meta's arm64 libmemorybroker.so from a JNI library; system_server is x86_64
 * here, so this arm64 daemon (run by the translator) hosts it instead (`prism_tracking broker`).
 * In a second process (`prism_tracking host`) it registers with the broker as the host of the
 * tracking regions, as trackingservice does, and keeps them up to date.
 *
 * Poses and input come from a client on the PC (tools/head.py, through `adb forward` to POSE_PORT),
 * as lines of text, in OpenXR's axes (Y up, -Z ahead), meters and quaternions (x y z w):
 *   px py pz qx qy qz qw                the headset's pose
 *   hand l|r px py pz qx qy qz qw       a controller's pose; until one comes it follows the head,
 *                                       held ahead of it and pointing where it looks
 *   follow l|r                          the controller follows the head again
 *   input l|r buttons trigger grip      its buttons (a mask of BUTTON_*) and its analog triggers (0-1)
 * The last of each stays when the client goes.
 *
 * Most regions hold snapshots as two copies behind two counters, started (+0) and completed (+4):
 * a writer counts the write started, writes the copy readers aren't taking and counts it done, and
 * a reader takes copy[completed & 1] and checks started hasn't moved (publish()).
 *
 * The head tracker's region (libtrackingserviceclients' HeadTrackerSharedMemory, 0x1680 bytes):
 *   0x0000  the latest HeadState (two copies)
 *   0x0148  a sequence word and a history of 32 HeadStates, for interpolating past poses
 *   0x1588  a mode word (two copies); in mode 1 a reader returns the latest state as of the time
 *           it asks for, unextrapolated
 * HeadState (0xa0 bytes) holds a valid flag, the orientation (a quaternion, x y z w) and
 * position, and its time (ns, CLOCK_MONOTONIC); a second quaternion at 0x80 must be a unit one
 * too for the state to count as valid.
 *
 * Controllers (two, Touch Plus: left and right). Each is in three regions, which InputHub (in
 * libtrackingserviceclients, which the runtime reads them with) finds through the input map:
 *   INPUT_TYPE_MAP (0xc8 bytes): the input devices, as (device id, specifier) pairs sorted by id,
 *     their count and a generation (two copies at 8, 0x60 each)
 *   CONTROLLER, left and right (0x7f8 bytes): its capabilities (0x000), live input (0x040),
 *     thumbstick (0x250), descriptor (0x3d0: device id, model, firmware, handedness, strings) and
 *     battery (0x4a0). Buttons are counters of transitions, odd while down.
 *   CONTROLLER_TRACKING, left and right (0xe48 bytes): the latest pose sample (two copies of 0x78
 *     at 8), and a history of 32 (a sequence word at 0xf8, odd while a slot is written; slots at
 *     0x100). A sample is valid with its flag set, a nonzero quaternion and a time, and readers
 *     extrapolate it at most 0.2 s.
 * Hands, as a headset's are before any sample: HAND_TRACKER, left and right (0x13e8 bytes each):
 *   two hand snapshots (0x7c8 apart, at 8) and two hand configurations (0x68 apart, at 0x1308),
 *   whose times, -1, mean no sample yet. Without a host, their clients ask the broker again and again.
 *
 * The host also serves CMSHeadset's controller tracking service (controller_tracking.c): the
 * controllers are tracked and in hand.
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

int serve_controller_tracking(void);  // controller_tracking.c

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
#define CONTROLLER 12  // SharedMemoryType CONTROLLER (5)
#define CONTROLLER_SIZE 0x7f8
#define CONTROLLER_TRACKING 4  // SharedMemoryType CONTROLLER_TRACKING (6)
#define CONTROLLER_TRACKING_SIZE 0xe48
#define LEFT 1  // the C API's specifiers
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
  _Atomic uint32_t started, completed;
};

#define LATEST 0x0000
#define MODE 0x1588
#define MODE_LATEST 1
#define RATE_HZ 100
#define POSE_PORT 7340  // on the guest's loopback

// --- controllers ---------------------------------------------------------------------------------

// A controller's buttons: the bits of its live input, each a transition counter there.
enum {
  BUTTON_TRIGGER = 1 << 0,  // index trigger pressed
  BUTTON_SYSTEM = 1 << 1,   // Meta button
  BUTTON_MENU = 1 << 2,
  BUTTON_STICK = 1 << 3,  // thumbstick clicked
  BUTTON_TRIGGER_TOUCH = 1 << 6,
  BUTTON_GRIP = 1 << 7,
  BUTTON_A = 1 << 8,  // X on the left
  BUTTON_A_TOUCH = 1 << 9,
  BUTTON_B = 1 << 10,  // Y on the left
  BUTTON_B_TOUCH = 1 << 11,
};
#define BUTTONS 20
#define ALL_BUTTONS ((1u << BUTTONS) - 1)

#define DEVICE_MODEL 8000  // Meta Quest Touch Plus (the runtime's render models: 8000-8999)
#define CAPABILITIES 0x000
#define INPUT 0x040
#define INPUT_STRIDE 0xc8
#define INPUT_SIZE 0xc4
#define STICK 0x250
#define DESCRIPTOR 0x3d0
#define DESCRIPTOR_SIZE 0x64
#define BATTERY 0x4a0

struct pose_sample {
  uint8_t valid;
  uint8_t reserved0[3];
  uint32_t flags;        // POSE_TRACKED
  float orientation[4];  // x y z w
  float position[3];
  float angular_velocity[3];
  float linear_velocity[3];
  float linear_acceleration[3];
  float angular_acceleration[3];
  uint32_t reserved1;
  int64_t time_ns;       // CLOCK_MONOTONIC
  int64_t predicted_ns;  // -1
};
_Static_assert(sizeof(struct pose_sample) == 0x68, "a pose sample is 0x68 bytes");
#define POSE_TRACKED 0x8f  // orientation and position valid and tracked, as trackingservice writes them
struct latest_pose {
  struct pose_sample sample;
  uint32_t tag;  // 0: a pose
  uint32_t reserved;
  uint8_t present;
  uint8_t padding[7];
};
_Static_assert(sizeof(struct latest_pose) == 0x78, "the latest pose's copies are 0x78 apart");
#define HISTORY_SEQUENCE 0xf8
#define HISTORY 0x100
#define HISTORY_SLOTS 32
#define TRACKING_EXTRA 0xe00  // a snapshot of {int64, double, double}: {-1, 0, 0} when there's none

struct controller {
  int hand;  // LEFT or RIGHT
  int follow;
  float position[3], orientation[4];
  uint32_t buttons;
  float trigger, grip;
  uint64_t counters[BUTTONS];
  int input_changed;
  uint8_t *input, *tracking;  // their regions, once allocated
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;  // everything below; regions can move under the writer
static uint8_t *g_region;
static float g_position[3], g_orientation[4] = {0, 0, 0, 1};  // the head's pose
static struct controller g_controllers[2] = {{.hand = LEFT, .follow = 1}, {.hand = RIGHT, .follow = 1}};
static uint8_t *g_input_map;
static int g_map_published;  // the map in g_input_map lists the controllers

static int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Writes the copy readers aren't taking, between counting the write started and done.
static void publish(uint8_t *at, const void *value, size_t size, size_t stride) {
  struct seqlock *lock = (struct seqlock *)at;
  uint32_t old = atomic_fetch_add_explicit(&lock->started, 1, memory_order_relaxed);
  atomic_thread_fence(memory_order_seq_cst);
  memcpy(at + sizeof *lock + ((old + 1) & 1) * stride, value, size);
  atomic_fetch_add_explicit(&lock->completed, 1, memory_order_release);
}

// Both copies, with nothing written yet: for a region just allocated.
static void seed(uint8_t *at, const void *value, size_t size, size_t stride) {
  memset(at, 0, sizeof(struct seqlock));
  memcpy(at + sizeof(struct seqlock), value, size);
  memcpy(at + sizeof(struct seqlock) + stride, value, size);
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

// v rotated by the unit quaternion q (x y z w).
static void rotate(const float q[4], const float v[3], float out[3]) {
  float t[3] = {2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0])};
  out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
  out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
  out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

// Where a controller following the head is: held ahead of it, a little low, pointing where it looks.
static void follow_head(struct controller *c) {
  const float held[3] = {c->hand == LEFT ? -0.15f : 0.15f, -0.25f, -0.35f};
  float offset[3];
  rotate(g_orientation, held, offset);
  for (int i = 0; i < 3; i++) c->position[i] = g_position[i] + offset[i];
  memcpy(c->orientation, g_orientation, sizeof c->orientation);
}

static struct pose_sample controller_sample(const struct controller *c, int64_t time_ns) {
  struct pose_sample s = {.valid = 1, .flags = POSE_TRACKED, .time_ns = time_ns, .predicted_ns = -1};
  memcpy(s.orientation, c->orientation, sizeof s.orientation);
  memcpy(s.position, c->position, sizeof s.position);
  return s;
}

static void append_history(uint8_t *region, const struct pose_sample *s) {
  _Atomic uint32_t *sequence = (_Atomic uint32_t *)(region + HISTORY_SEQUENCE);
  uint32_t old = atomic_fetch_add_explicit(sequence, 1, memory_order_relaxed);  // odd: writing
  atomic_thread_fence(memory_order_seq_cst);
  memcpy(region + HISTORY + (((old >> 1) + 1) % HISTORY_SLOTS) * sizeof *s, s, sizeof *s);
  atomic_fetch_add_explicit(sequence, 1, memory_order_release);
}

static void publish_pose(struct controller *c) {
  if (c->follow) follow_head(c);
  struct latest_pose latest = {.sample = controller_sample(c, now_ns()), .present = 1};
  publish(c->tracking, &latest, sizeof latest, sizeof latest);
  append_history(c->tracking, &latest.sample);
}

// Counts each button's transitions since the last input: odd counts are buttons down.
static void publish_input(struct controller *c) {
  uint8_t input[INPUT_SIZE] = {0};
  for (int b = 0; b < BUTTONS; b++) {
    uint64_t down = (c->buttons >> b) & 1;
    if ((c->counters[b] & 1) != down) c->counters[b]++;
    size_t at = b < 18 ? b * 8 : 0x98 + (b - 18) * 8;  // the triggers' values are at 0x90
    memcpy(input + at, &c->counters[b], sizeof c->counters[b]);
  }
  memcpy(input + 0x90, &c->trigger, sizeof c->trigger);
  memcpy(input + 0x94, &c->grip, sizeof c->grip);
  publish(c->input + INPUT, input, sizeof input, INPUT_STRIDE);
  c->input_changed = 0;
}

static uint64_t device_id(const struct controller *c) { return (uint64_t)c->hand; }

static void controller_ready(void *context, void *memory) {
  struct controller *c = context;
  uint8_t *region = memory;
  memset(region, 0, CONTROLLER_SIZE);
  // Thumbstick range and size, the buttons it has, and two fields not known.
  const uint32_t capabilities[7] = {65535, 65535, 1, 1, ALL_BUTTONS, 0, 0};
  seed(region + CAPABILITIES, capabilities, sizeof capabilities, sizeof capabilities);
  const uint8_t stick[7] = {0, 0, 0xff, 0x7f, 0xff, 0x7f, 1};  // not touched, centered, valid
  seed(region + STICK, stick, sizeof stick, 8);
  uint8_t descriptor[DESCRIPTOR_SIZE] = {0};
  uint64_t id = device_id(c);
  uint32_t model = DEVICE_MODEL, firmware = 0, hand = c->hand;
  memcpy(descriptor + 0x00, &id, sizeof id);
  memcpy(descriptor + 0x08, &model, sizeof model);
  memcpy(descriptor + 0x0c, &firmware, sizeof firmware);
  memcpy(descriptor + 0x10, &hand, sizeof hand);
  strcpy((char *)descriptor + 0x14, c->hand == LEFT ? "PRISM-L" : "PRISM-R");  // 20 bytes each, 4 strings
  seed(region + DESCRIPTOR, descriptor, sizeof descriptor, sizeof descriptor);
  const uint8_t battery[2] = {1, 100};  // valid, percent
  seed(region + BATTERY, battery, sizeof battery, sizeof battery);
  pthread_mutex_lock(&g_lock);
  c->input = region;
  publish_input(c);
  pthread_mutex_unlock(&g_lock);
  LOG("%s controller region at %p", c->hand == LEFT ? "left" : "right", memory);
}

static void controller_tracking_ready(void *context, void *memory) {
  struct controller *c = context;
  uint8_t *region = memory;
  memset(region, 0, CONTROLLER_TRACKING_SIZE);
  pthread_mutex_lock(&g_lock);
  if (c->follow) follow_head(c);
  // A history of samples 10 ms apart up to now, the newest in the last slot.
  int64_t now = now_ns();
  for (int i = 0; i < HISTORY_SLOTS; i++) {
    struct pose_sample s = controller_sample(c, now - (int64_t)(HISTORY_SLOTS - 1 - i) * 10000000);
    memcpy(region + HISTORY + i * sizeof s, &s, sizeof s);
  }
  uint32_t newest = 2 * (HISTORY_SLOTS - 1);  // (sequence >> 1) % 32 is the newest slot
  memcpy(region + HISTORY_SEQUENCE, &newest, sizeof newest);
  struct latest_pose latest = {.sample = controller_sample(c, now), .present = 1};
  seed(region, &latest, sizeof latest, sizeof latest);
  struct {
    int64_t time;
    double a, b;
  } extra = {-1, 0, 0};
  seed(region + TRACKING_EXTRA, &extra, sizeof extra, sizeof extra);
  c->tracking = region;
  pthread_mutex_unlock(&g_lock);
  LOG("%s controller tracking region at %p", c->hand == LEFT ? "left" : "right", memory);
}

// The input map's snapshot: the controllers once their regions are all there, else nothing.
struct input_map {
  struct {
    uint64_t id;
    uint32_t specifier;
    uint32_t padding;
  } entries[5];  // sorted by id
  uint64_t count;
  uint64_t generation;
};
_Static_assert(sizeof(struct input_map) == 0x60, "the input map's copies are 0x60 bytes");

static int controllers_ready(void) {
  for (int i = 0; i < 2; i++)
    if (!g_controllers[i].input || !g_controllers[i].tracking) return 0;
  return 1;
}

static struct input_map controller_map(void) {
  struct input_map map = {.count = 2, .generation = 1};
  for (int i = 0; i < 2; i++) {
    map.entries[i].id = device_id(&g_controllers[i]);
    map.entries[i].specifier = g_controllers[i].hand;
  }
  return map;
}

static void input_map_ready(void *context, void *memory) {
  (void)context;
  struct input_map empty = {0};
  pthread_mutex_lock(&g_lock);
  seed(memory, &empty, sizeof empty, sizeof empty);
  g_input_map = memory;
  g_map_published = 0;
  pthread_mutex_unlock(&g_lock);
  LOG("input map region at %p", memory);
}

// --- poses from the PC ---------------------------------------------------------------------------

static struct controller *controller_named(const char *name) {
  if (!strcmp(name, "l")) return &g_controllers[0];
  if (!strcmp(name, "r")) return &g_controllers[1];
  return NULL;
}

static int unit(float q[4]) {
  float norm = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (!(norm > 0.5f && norm < 2)) return 0;
  for (int i = 0; i < 4; i++) q[i] /= norm;
  return 1;
}

static void take_line(const char *line) {
  char name[8];
  float p[3], q[4], trigger, grip;
  unsigned buttons;
  struct controller *c;
  if (sscanf(line, "hand %7s %f %f %f %f %f %f %f", name, &p[0], &p[1], &p[2], &q[0], &q[1], &q[2], &q[3]) == 8) {
    if (!(c = controller_named(name)) || !unit(q)) return;
    c->follow = 0;
    memcpy(c->position, p, sizeof p);
    memcpy(c->orientation, q, sizeof q);
  } else if (sscanf(line, "follow %7s", name) == 1) {
    if ((c = controller_named(name))) c->follow = 1;
  } else if (sscanf(line, "input %7s %i %f %f", name, (int *)&buttons, &trigger, &grip) == 4) {
    if (!(c = controller_named(name))) return;
    c->buttons = buttons & ALL_BUTTONS;
    c->trigger = trigger;
    c->grip = grip;
    c->input_changed = 1;
  } else if (sscanf(line, "%f %f %f %f %f %f %f", &p[0], &p[1], &p[2], &q[0], &q[1], &q[2], &q[3]) == 7) {
    if (!unit(q)) return;
    memcpy(g_position, p, sizeof p);
    memcpy(g_orientation, q, sizeof q);
  }
}

// Reads from one client at a time.
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
      pthread_mutex_lock(&g_lock);
      take_line(line);
      pthread_mutex_unlock(&g_lock);
    }
    fclose(in);
    LOG("pose client gone");
  }
}

// --- hands ---------------------------------------------------------------------------------------

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

// --- processes -----------------------------------------------------------------------------------

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
  struct controller *left = &g_controllers[0], *right = &g_controllers[1];
  const struct region_info others[] = {
      {.type = HAND_TRACKER, .specifier = LEFT, .size = HAND_SIZE, .context = NULL, .ready = hand_ready},
      {.type = HAND_TRACKER, .specifier = RIGHT, .size = HAND_SIZE, .context = (void *)1, .ready = hand_ready},
      {.type = CONTROLLER, .specifier = LEFT, .size = CONTROLLER_SIZE, .context = left, .ready = controller_ready},
      {.type = CONTROLLER, .specifier = RIGHT, .size = CONTROLLER_SIZE, .context = right, .ready = controller_ready},
      {.type = CONTROLLER_TRACKING, .specifier = LEFT, .size = CONTROLLER_TRACKING_SIZE, .context = left,
       .ready = controller_tracking_ready},
      {.type = CONTROLLER_TRACKING, .specifier = RIGHT, .size = CONTROLLER_TRACKING_SIZE, .context = right,
       .ready = controller_tracking_ready},
      {.type = INPUT_TYPE_MAP, .size = INPUT_MAP_SIZE, .ready = input_map_ready},
  };
  for (size_t i = 0; i < sizeof others / sizeof *others; i++)
    if (!register_region(host, &others[i])) ERR("can't register region type %u/%u", others[i].type, others[i].specifier);
  serve_controller_tracking();  // CMSHeadset's: whether the controllers are tracked
  pthread_t poses;
  pthread_create(&poses, NULL, serve_poses, NULL);
  for (;;) {
    pthread_mutex_lock(&g_lock);
    if (g_region) publish_head(g_region);
    for (int i = 0; i < 2; i++) {
      struct controller *c = &g_controllers[i];
      if (c->tracking) publish_pose(c);
      if (c->input && c->input_changed) publish_input(c);
    }
    // The map lists the controllers once their regions hold them.
    if (g_input_map && !g_map_published && controllers_ready()) {
      struct input_map map = controller_map();
      publish(g_input_map, &map, sizeof map, sizeof map);
      g_map_published = 1;
      LOG("input map: two controllers");
    }
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
