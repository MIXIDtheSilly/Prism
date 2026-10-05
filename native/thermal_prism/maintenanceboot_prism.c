/*
 * Prism's maintenance boot HAL: vendor.oculus.hardware.maintenanceboot.IMaintenanceBoot/default
 * (stable AIDL, V1).
 *
 * On a headset an odm HAL keeps the maintenance boot settings (whether the user enabled it, whether
 * this boot is one, and the thresholds that schedule one). Horizon's MaintenanceBoot app waits for
 * it at every boot to record that maintenance boot is off, retrying each second forever when it's
 * missing. This keeps the settings in memory: off, with no thresholds, until someone sets them.
 *
 * Written against libbinder_ndk directly, like the thermal HAL (thermal_prism.c).
 */
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>

#define TAG "PrismMaintenanceBoot"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define ERR(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define DESCRIPTOR "vendor.oculus.hardware.maintenanceboot.IMaintenanceBoot"
#define INSTANCE DESCRIPTOR "/default"
#define VERSION 1
#define HASH "6fcd31cf7c81c6f9c5c1a52222730a090f1e7c80"  // Horizon's V1 interface

enum {  // IMaintenanceBoot, in declaration order (Horizon's BpMaintenanceBoot)
  GET_USER_ENABLE_FLAG = FIRST_CALL_TRANSACTION,
  SET_USER_ENABLE_FLAG,
  GET_TIME_SINCE_CHARGING_THRESHOLD,
  SET_TIME_SINCE_CHARGING_THRESHOLD,
  GET_TIME_SINCE_LAST_BOOT_THRESHOLD,
  SET_TIME_SINCE_LAST_BOOT_THRESHOLD,
  GET_BATTERY_THRESHOLD,
  SET_BATTERY_THRESHOLD,
  GET_BOOTED_FOR_MAINTENANCE_FLAG,
  SET_BOOTED_FOR_MAINTENANCE_FLAG,
  GET_INTERFACE_HASH = FIRST_CALL_TRANSACTION + 0x00fffffd,
  GET_INTERFACE_VERSION = FIRST_CALL_TRANSACTION + 0x00fffffe,
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_user_enabled, g_booted_for_maintenance;
static int64_t g_since_charging, g_since_last_boot;
static int32_t g_battery;

static binder_status_t reply_ok(AParcel *out) {
  AStatus *status = AStatus_newOk();
  binder_status_t ret = AParcel_writeStatusHeader(out, status);
  AStatus_delete(status);
  return ret;
}

static binder_status_t get_bool(AParcel *out, const bool *value) {
  reply_ok(out);
  return AParcel_writeBool(out, *value);
}

static binder_status_t set_bool(const AParcel *in, AParcel *out, bool *value) {
  binder_status_t ret = AParcel_readBool(in, value);
  return ret == STATUS_OK ? reply_ok(out) : ret;
}

static binder_status_t get_long(AParcel *out, const int64_t *value) {
  reply_ok(out);
  return AParcel_writeInt64(out, *value);
}

static binder_status_t set_long(const AParcel *in, AParcel *out, int64_t *value) {
  binder_status_t ret = AParcel_readInt64(in, value);
  return ret == STATUS_OK ? reply_ok(out) : ret;
}

static binder_status_t transact(transaction_code_t code, const AParcel *in, AParcel *out) {
  switch (code) {
    case GET_USER_ENABLE_FLAG: return get_bool(out, &g_user_enabled);
    case SET_USER_ENABLE_FLAG: return set_bool(in, out, &g_user_enabled);
    case GET_TIME_SINCE_CHARGING_THRESHOLD: return get_long(out, &g_since_charging);
    case SET_TIME_SINCE_CHARGING_THRESHOLD: return set_long(in, out, &g_since_charging);
    case GET_TIME_SINCE_LAST_BOOT_THRESHOLD: return get_long(out, &g_since_last_boot);
    case SET_TIME_SINCE_LAST_BOOT_THRESHOLD: return set_long(in, out, &g_since_last_boot);
    case GET_BATTERY_THRESHOLD:
      reply_ok(out);
      return AParcel_writeInt32(out, g_battery);
    case SET_BATTERY_THRESHOLD: {
      binder_status_t ret = AParcel_readInt32(in, &g_battery);
      return ret == STATUS_OK ? reply_ok(out) : ret;
    }
    case GET_BOOTED_FOR_MAINTENANCE_FLAG: return get_bool(out, &g_booted_for_maintenance);
    case SET_BOOTED_FOR_MAINTENANCE_FLAG: return set_bool(in, out, &g_booted_for_maintenance);
    case GET_INTERFACE_VERSION:
      reply_ok(out);
      return AParcel_writeInt32(out, VERSION);
    case GET_INTERFACE_HASH:
      reply_ok(out);
      return AParcel_writeString(out, HASH, strlen(HASH));
    default:
      return STATUS_UNKNOWN_TRANSACTION;
  }
}

static binder_status_t on_transact(AIBinder *binder, transaction_code_t code, const AParcel *in, AParcel *out) {
  (void)binder;
  pthread_mutex_lock(&g_lock);
  binder_status_t ret = transact(code, in, out);
  pthread_mutex_unlock(&g_lock);
  return ret;
}

static void *on_create(void *args) { return args; }
static void on_destroy(void *data) { (void)data; }

int main(void) {
  void *ndk = dlopen("libbinder_ndk.so", RTLD_NOW);
  binder_exception_t (*add_service)(AIBinder *, const char *) = ndk ? dlsym(ndk, "AServiceManager_addService") : NULL;
  void (*mark_vintf)(AIBinder *) = ndk ? dlsym(ndk, "AIBinder_markVintfStability") : NULL;
  int (*set_max_threads)(uint32_t) = ndk ? dlsym(ndk, "ABinderProcess_setThreadPoolMaxThreadCount") : NULL;
  void (*join_pool)(void) = ndk ? dlsym(ndk, "ABinderProcess_joinThreadPool") : NULL;
  if (!add_service || !mark_vintf || !set_max_threads || !join_pool) {
    ERR("libbinder_ndk is missing the platform service APIs");
    return 1;
  }
  set_max_threads(0);
  AIBinder_Class *cls = AIBinder_Class_define(DESCRIPTOR, on_create, on_destroy, on_transact);
  AIBinder *service = AIBinder_new(cls, NULL);
  mark_vintf(service);
  binder_exception_t ex = add_service(service, INSTANCE);
  if (ex != EX_NONE) {
    ERR("registering %s failed: %d", INSTANCE, ex);
    return 1;
  }
  LOG("serving %s", INSTANCE);
  join_pool();
  return 0;
}
