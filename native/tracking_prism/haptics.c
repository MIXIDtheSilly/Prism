/*
 * oculus.internal.tracking.IHapticsService/default, which a headset's trackingservice serves: how
 * Horizon's runtime (its HapticsServer, through libtrackingserviceclients) drives the controllers'
 * haptics.
 *
 * Transactions (the interface token is checked by libbinder_ndk):
 *   1         the haptic socket: a status, then a ParcelFileDescriptor the client writes into
 *   0xffffff  interface version: 2
 *
 * Each write into the socket is one message: a header {u32 type, u32 length} and length bytes:
 *   1  amplitude, 9 bytes: {u64 device (the controller's id), u8 amplitude (0-255)}: an app's
 *      xrApplyHapticFeedback (the runtime sets it back to 0 when its duration is over)
 *   3  buffered, 0x22 bytes: {u64 device, u8 count, u8 samples[25] (amplitudes, 0-255)} at the
 *      sample rate tracking_prism.c gives each haptic channel (320 Hz): VrShell's parametric
 *      effects (its UI's ticks), rendered by the runtime's haptics plugin
 *   others (5: 0x50 bytes of PCM, ...) are only logged
 * Each goes to the PC client (tracking_prism.c's haptic()), which plays it on its controllers: an
 * amplitude until the next, a buffer as a pulse of its peak amplitude for as long as it isn't silent.
 */
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TAG "PrismTracking"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define ERR(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define SERVICE "oculus.internal.tracking.IHapticsService/default"
#define DESCRIPTOR "oculus.internal.tracking.IHapticsService"
#define VERSION 2
enum { GET_SOCKET = FIRST_CALL_TRANSACTION, GET_VERSION = 0xffffff };
enum { MESSAGE_AMPLITUDE = 1, MESSAGE_BUFFERED = 3 };
#define BUFFERED_RATE 320.0f  // tracking_prism.c's haptic channels
#define BUFFERED_SAMPLES 25
#define MESSAGE_MAX 0x400
#define LOGGED_MESSAGES 64  // the first of each socket

void haptic(uint64_t device, float amplitude, float seconds);  // tracking_prism.c

struct reader {
  int fd;    // ours
  int peer;  // the client's end, kept until the reply carrying it has gone
};

static void log_message(const uint8_t *m, ssize_t size) {
  char hex[3 * 64 + 1] = "";
  for (ssize_t i = 0; i < size && i < 64; i++) snprintf(hex + 3 * i, 4, "%02x ", m[i]);
  LOG("haptics: %zd bytes: %s%s", size, hex, size > 64 ? "..." : "");
}

static void *read_socket(void *arg) {
  struct reader r = *(struct reader *)arg;
  free(arg);
  uint8_t m[MESSAGE_MAX];
  int logged = 0;
  ssize_t size;
  while ((size = recv(r.fd, m, sizeof m, 0)) > 0) {
    if (r.peer >= 0) {
      close(r.peer);
      r.peer = -1;
    }
    uint32_t type, length;
    if (size < 8) continue;
    memcpy(&type, m, 4);
    memcpy(&length, m + 4, 4);
    if (logged < LOGGED_MESSAGES) {
      log_message(m, size);
      logged++;
    }
    uint64_t device;
    if (type == MESSAGE_AMPLITUDE && length >= 9 && size >= 17) {
      memcpy(&device, m + 8, 8);
      haptic(device, m[16] / 255.0f, -1);
    } else if (type == MESSAGE_BUFFERED && length >= 9 + BUFFERED_SAMPLES && size >= 17 + BUFFERED_SAMPLES) {
      memcpy(&device, m + 8, 8);
      int count = m[16] < BUFFERED_SAMPLES ? m[16] : BUFFERED_SAMPLES, last = -1, peak = 0;
      for (int i = 0; i < count; i++) {
        if (m[17 + i]) last = i;
        if (m[17 + i] > peak) peak = m[17 + i];
      }
      if (last >= 0) haptic(device, peak / 255.0f, (last + 1) / BUFFERED_RATE);
    }
  }
  LOG("haptics: socket closed");
  if (r.peer >= 0) close(r.peer);
  close(r.fd);
  return NULL;
}

static binder_status_t write_status_ok(AParcel *out) {
  static binder_status_t (*write_status_header)(AParcel *, const AStatus *);
  if (!write_status_header) write_status_header = dlsym(RTLD_DEFAULT, "AParcel_writeStatusHeader");
  AStatus *ok = AStatus_newOk();
  binder_status_t s = write_status_header ? write_status_header(out, ok) : STATUS_UNKNOWN_ERROR;
  AStatus_delete(ok);
  return s;
}

// A socket pair: ours is read by a thread of its own, the client's goes in the reply. The parcel
// only names the client's end, so it stays open until the client has written into it (or its end
// closes).
static binder_status_t get_socket(AParcel *out) {
  int ends[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ends)) {
    ERR("haptics: socketpair failed");
    return STATUS_NO_MEMORY;
  }
  struct reader *r = malloc(sizeof *r);
  pthread_t thread;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (r) *r = (struct reader){.fd = ends[0], .peer = ends[1]};
  if (!r || pthread_create(&thread, &attr, read_socket, r)) {
    free(r);
    close(ends[0]);
    close(ends[1]);
    pthread_attr_destroy(&attr);
    return STATUS_NO_MEMORY;
  }
  pthread_attr_destroy(&attr);
  binder_status_t s = write_status_ok(out);
  if (s == STATUS_OK) s = AParcel_writeParcelFileDescriptor(out, ends[1]);
  LOG("haptics: socket handed out");
  return s;
}

static binder_status_t on_transact(AIBinder *binder, transaction_code_t code, const AParcel *in, AParcel *out) {
  (void)binder;
  (void)in;
  switch (code) {
    case GET_SOCKET:
      return get_socket(out);
    case GET_VERSION: {
      binder_status_t s = write_status_ok(out);
      return s == STATUS_OK ? AParcel_writeInt32(out, VERSION) : s;
    }
    default:
      return STATUS_UNKNOWN_TRANSACTION;
  }
}

static void *on_create(void *args) { return args; }
static void on_destroy(void *data) { (void)data; }

// Registers the service; the caller's binder thread pool serves it.
int serve_haptics(void) {
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
