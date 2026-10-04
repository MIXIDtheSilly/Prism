/*
 * Prism's binder relay: prism.IBinderRelay, registered as prism.binder_relay.
 *
 * An app's arm64 libbinder and the host's Java binder use separate /dev/binder connections, even in
 * the same process. A binder object crosses between them through this service (guest_bridge.c is
 * its client): PUT keeps a strong reference under a random token, and TAKE hands it out once, to
 * the other connection. References not taken within 60 seconds are dropped; a full table drops the
 * oldest.
 *
 * libbinder_ndk checks the interface token. Replies hold only the payload, with no status header.
 */
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/log.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/random.h>
#include <time.h>

#include <dlfcn.h>
#include <stdbool.h>

#define TAG "PrismRelay"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define WARN(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define ERR(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define DESCRIPTOR "prism.IBinderRelay"
#define INSTANCE "prism.binder_relay"
#define CAPACITY 1024
#define EXPIRY_SECONDS 60

enum { PUT = FIRST_CALL_TRANSACTION, TAKE };

static struct entry {
  int64_t token;
  AIBinder *binder;
  struct timespec created;
} entries[CAPACITY];
static size_t count;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static int expired(const struct entry *e, const struct timespec *now) {
  time_t seconds = now->tv_sec - e->created.tv_sec;
  return seconds > EXPIRY_SECONDS || (seconds == EXPIRY_SECONDS && now->tv_nsec >= e->created.tv_nsec);
}

// Removing an entry hands its strong reference to the caller.
static AIBinder *remove_entry(size_t i) {
  AIBinder *binder = entries[i].binder;
  entries[i] = entries[--count];
  return binder;
}

static void expire_entries(const struct timespec *now) {
  for (size_t i = 0; i < count;) {
    if (expired(&entries[i], now)) AIBinder_decStrong(remove_entry(i));
    else i++;
  }
}

static binder_status_t fresh_token(int64_t *token) {
  for (;;) {
    size_t done = 0;
    while (done < sizeof(*token)) {
      ssize_t n = getrandom((char *)token + done, sizeof(*token) - done, 0);
      if (n < 0) {
        if (errno == EINTR) continue;
        return STATUS_UNKNOWN_ERROR;
      }
      if (!n) return STATUS_UNKNOWN_ERROR;
      done += (size_t)n;
    }
    if (!*token) continue;
    size_t i = 0;
    while (i < count && entries[i].token != *token) i++;
    if (i == count) return STATUS_OK;
  }
}

static binder_status_t on_transact(AIBinder *service, transaction_code_t code, const AParcel *in, AParcel *out) {
  (void)service;
  if (code != PUT && code != TAKE) return STATUS_UNKNOWN_TRANSACTION;
  AIBinder *binder = NULL;
  int64_t token = 0;
  binder_status_t ret = code == PUT ? AParcel_readStrongBinder(in, &binder) : AParcel_readInt64(in, &token);
  if (ret != STATUS_OK) {
    if (binder) AIBinder_decStrong(binder);
    if (code == TAKE) WARN("TAKE could not read token: %d", ret);
    return ret;
  }

  pthread_mutex_lock(&mutex);
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    pthread_mutex_unlock(&mutex);
    if (binder) AIBinder_decStrong(binder);
    if (code == TAKE) WARN("TAKE could not check expiry for token %" PRId64, token);
    return STATUS_UNKNOWN_ERROR;
  }
  expire_entries(&now);
  if (code == PUT) {
    if (binder) ret = fresh_token(&token);
    if (ret == STATUS_OK) ret = AParcel_writeInt64(out, token);
    if (ret == STATUS_OK && binder) {
      if (count == CAPACITY) {
        size_t oldest = 0;
        for (size_t i = 1; i < count; i++) {
          const struct timespec *a = &entries[i].created, *b = &entries[oldest].created;
          if (a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec)) oldest = i;
        }
        AIBinder_decStrong(remove_entry(oldest));
      }
      entries[count++] = (struct entry){token, binder, now};  // keep the reference read from the parcel
      binder = NULL;
    }
    pthread_mutex_unlock(&mutex);
    if (binder) AIBinder_decStrong(binder);
    return ret;
  }

  for (size_t i = 0; i < count; i++) {
    if (entries[i].token == token) {
      binder = remove_entry(i);
      break;
    }
  }
  pthread_mutex_unlock(&mutex);
  if (!binder) WARN("TAKE unknown token %" PRId64, token);
  ret = AParcel_writeStrongBinder(out, binder);
  if (binder) AIBinder_decStrong(binder);  // the reply now owns its reference
  if (ret != STATUS_OK) WARN("TAKE could not write binder for token %" PRId64 ": %d", token, ret);
  return ret;
}

static void *on_create(void *args) { return args; }
static void on_destroy(void *data) { (void)data; }

int main(void) {
  // Not in the NDK's headers: the platform's service manager and thread pool functions.
  void *ndk = dlopen("libbinder_ndk.so", RTLD_NOW);
  binder_exception_t (*add_service)(AIBinder *, const char *) = ndk ? dlsym(ndk, "AServiceManager_addService") : NULL;
  bool (*set_max_threads)(uint32_t) = ndk ? dlsym(ndk, "ABinderProcess_setThreadPoolMaxThreadCount") : NULL;
  void (*start_pool)(void) = ndk ? dlsym(ndk, "ABinderProcess_startThreadPool") : NULL;
  void (*join_pool)(void) = ndk ? dlsym(ndk, "ABinderProcess_joinThreadPool") : NULL;
  if (!add_service || !set_max_threads || !start_pool || !join_pool) {
    ERR("registering %s failed: libbinder_ndk is missing the platform service APIs", INSTANCE);
    return 1;
  }
  if (!set_max_threads(4)) {
    ERR("registering %s failed: could not set thread pool size", INSTANCE);
    return 1;
  }
  AIBinder_Class *cls = AIBinder_Class_define(DESCRIPTOR, on_create, on_destroy, on_transact);
  AIBinder *service = cls ? AIBinder_new(cls, NULL) : NULL;
  if (!service) {
    ERR("registering %s failed: could not create binder", INSTANCE);
    return 1;
  }
  binder_exception_t ex = add_service(service, INSTANCE);
  if (ex != EX_NONE) {
    ERR("registering %s failed: %d", INSTANCE, ex);
    AIBinder_decStrong(service);
    return 1;
  }
  LOG("serving %s", INSTANCE);
  start_pool();
  join_pool();
  AIBinder_decStrong(service);
  return 0;
}
