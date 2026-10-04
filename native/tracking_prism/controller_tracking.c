/*
 * oculus.internal.tracking.IControllerTrackingService/default, which a headset's trackingservice
 * serves: how CMSHeadset (OVRRemoteService) learns that a paired controller is tracked.
 *
 * For each controller it has connected, CMSHeadset sends the service the controller's status and
 * one end of a socket (SOCK_SEQPACKET) of that controller's events, and the tracker writes events
 * into it as tracking changes: 24-byte records {u32 type, u32 0, i64 time (ns, CLOCK_MONOTONIC),
 * u64 payload}. Prism's controllers are always tracked and in hand (native/tracking_prism keeps
 * their poses), so each socket gets those two events as soon as it comes:
 *   EVENT_TRACKING_LEVEL, 3: tracked in 6DoF (CMSHeadset's TrackingStatus POSITION)
 *   EVENT_IN_HAND, 1
 *
 * Transactions (oneway unless noted; the interface token is checked by libbinder_ndk):
 *   1  status: client binder, byte slot (0 right, 1 left), int32 presence and, if present, a
 *      ControllerStatus (int32 size, int64 id, four strings: firmware, hardware revision, serial,
 *      IMU; int32 battery, byte connection: 1 active, byte model, bool attached), then the event
 *      socket (a ParcelFileDescriptor). No status clears the slot.
 *   2  map share: an IMapShareHmd binder, unused
 *   0xffffff  interface version (not oneway): 2
 */
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TAG "PrismTracking"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define ERR(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define SERVICE "oculus.internal.tracking.IControllerTrackingService/default"
#define DESCRIPTOR "oculus.internal.tracking.IControllerTrackingService"
#define VERSION 2
enum { STATUS_UPDATE = FIRST_CALL_TRANSACTION, MAP_SHARE, GET_VERSION = 0xffffff };
enum { EVENT_TRACKING_LEVEL = 1, EVENT_IN_HAND = 8 };
#define TRACKED_6DOF 3
#define CONNECTION_ACTIVE 1
#define SLOTS 4

struct event {
  uint32_t type, reserved;
  int64_t time_ns;
  uint64_t payload;
};
_Static_assert(sizeof(struct event) == 24, "CMSHeadset reads 24-byte events");

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_sockets[SLOTS] = {-1, -1, -1, -1};  // each slot's event socket, while active
static AIBinder *g_clients[SLOTS];

static int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void send_event(int fd, uint32_t type, uint64_t payload) {
  struct event e = {.type = type, .time_ns = now_ns(), .payload = payload};
  if (send(fd, &e, sizeof e, MSG_NOSIGNAL | MSG_DONTWAIT) != sizeof e) ERR("controller event %u not sent", type);
}

// Reads a ControllerStatus's id and connection. It's size-delimited, and its last four fields
// (battery, connection, model, attached) take a 32-bit slot each, so the strings between are skipped.
static binder_status_t read_status(const AParcel *in, int64_t *id, int8_t *connection) {
  int32_t start = AParcel_getDataPosition(in), size;
  binder_status_t s = AParcel_readInt32(in, &size);
  if (s != STATUS_OK) return s;
  if (size < 4) return STATUS_BAD_VALUE;
  if (size < 4 + 8 + 16) return STATUS_BAD_VALUE;
  if ((s = AParcel_readInt64(in, id)) != STATUS_OK) return s;
  AParcel_setDataPosition(in, start + size - 16);
  int32_t battery;
  AParcel_readInt32(in, &battery);
  AParcel_readByte(in, connection);
  return AParcel_setDataPosition(in, start + size);
}

static void clear_slot(int slot) {
  if (g_sockets[slot] >= 0) close(g_sockets[slot]);
  g_sockets[slot] = -1;
  if (g_clients[slot]) AIBinder_decStrong(g_clients[slot]);
  g_clients[slot] = NULL;
}

static binder_status_t status_update(const AParcel *in) {
  AIBinder *client = NULL;
  int8_t slot;
  int32_t present;
  int64_t id = 0;
  int8_t connection = 0;
  binder_status_t s;
  if ((s = AParcel_readStrongBinder(in, &client)) != STATUS_OK) return s;
  if ((s = AParcel_readByte(in, &slot)) != STATUS_OK || (s = AParcel_readInt32(in, &present)) != STATUS_OK ||
      (present && (s = read_status(in, &id, &connection)) != STATUS_OK)) {
    if (client) AIBinder_decStrong(client);
    return s;
  }
  int fd = -1;
  AParcel_readParcelFileDescriptor(in, &fd);
  if (slot < 0 || slot >= SLOTS) {
    if (client) AIBinder_decStrong(client);
    if (fd >= 0) close(fd);
    return STATUS_BAD_VALUE;
  }
  pthread_mutex_lock(&g_lock);
  clear_slot(slot);
  if (present && connection == CONNECTION_ACTIVE && fd >= 0) {
    g_sockets[slot] = fd;
    g_clients[slot] = client;  // kept while the slot is
    send_event(fd, EVENT_TRACKING_LEVEL, TRACKED_6DOF);
    send_event(fd, EVENT_IN_HAND, 1);
    LOG("controller %016llx (slot %d): tracked, in hand", (unsigned long long)id, slot);
  } else {
    if (fd >= 0) close(fd);
    if (client) AIBinder_decStrong(client);
    LOG("controller slot %d: %s", slot, present ? "not active" : "cleared");
  }
  pthread_mutex_unlock(&g_lock);
  return STATUS_OK;
}

static binder_status_t on_transact(AIBinder *binder, transaction_code_t code, const AParcel *in, AParcel *out) {
  (void)binder;
  switch (code) {
    case STATUS_UPDATE:
      return status_update(in);
    case MAP_SHARE: {
      AIBinder *share = NULL;
      AParcel_readStrongBinder(in, &share);
      if (share) AIBinder_decStrong(share);
      return STATUS_OK;
    }
    case GET_VERSION: {
      static binder_status_t (*write_status_header)(AParcel *, const AStatus *);
      if (!write_status_header) write_status_header = dlsym(RTLD_DEFAULT, "AParcel_writeStatusHeader");
      AStatus *ok = AStatus_newOk();
      binder_status_t s = write_status_header ? write_status_header(out, ok) : STATUS_UNKNOWN_ERROR;
      AStatus_delete(ok);
      return s == STATUS_OK ? AParcel_writeInt32(out, VERSION) : s;
    }
    default:
      return STATUS_UNKNOWN_TRANSACTION;
  }
}

static void *on_create(void *args) { return args; }
static void on_destroy(void *data) { (void)data; }

// Registers the service; the caller's binder thread pool serves it.
int serve_controller_tracking(void) {
  void *ndk = dlopen("libbinder_ndk.so", RTLD_NOW);
  binder_exception_t (*add_service)(AIBinder *, const char *) = ndk ? dlsym(ndk, "AServiceManager_addService") : NULL;
  AIBinder_Class *cls = AIBinder_Class_define(DESCRIPTOR, on_create, on_destroy, on_transact);
  AIBinder *service = cls ? AIBinder_new(cls, NULL) : NULL;
  if (!add_service || !service || add_service(service, SERVICE) != EX_NONE) {
    ERR("can't register %s", SERVICE);
    return 0;
  }
  LOG("%s registered", SERVICE);
  return 1;
}
