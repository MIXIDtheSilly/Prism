/*
 * Prism's thermal HAL: android.hardware.thermal.IThermal/default (stable AIDL, V1).
 *
 * Horizon's vrdevice refuses to start without the AIDL thermal HAL; the emulator ships only the
 * HIDL thermal@2.0 mock. This serves a fixed, cool reading for each sensor a headset reports, no
 * throttling and no cooling devices, and accepts callbacks it never needs to call: the readings
 * never change.
 *
 * Written against libbinder_ndk directly (the NDK has no AIDL stubs for the interface); the
 * service-manager and process calls are platform APIs the NDK doesn't declare, so they're looked up.
 */
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <android/log.h>
#include <dlfcn.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TAG "PrismThermal"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define ERR(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define DESCRIPTOR "android.hardware.thermal.IThermal"
#define INSTANCE DESCRIPTOR "/default"
#define VERSION 1
#define HASH "76e77ca374a7860f09aeac48e98b2ec61f576767"  // aidl_api/android.hardware.thermal/1/.hash

enum {  // IThermal.aidl, in declaration order
  GET_COOLING_DEVICES = FIRST_CALL_TRANSACTION,
  GET_COOLING_DEVICES_WITH_TYPE,
  GET_TEMPERATURES,
  GET_TEMPERATURES_WITH_TYPE,
  GET_TEMPERATURE_THRESHOLDS,
  GET_TEMPERATURE_THRESHOLDS_WITH_TYPE,
  REGISTER_CALLBACK,
  REGISTER_CALLBACK_WITH_TYPE,
  UNREGISTER_CALLBACK,
  GET_INTERFACE_HASH = 0x00fffffd,
  GET_INTERFACE_VERSION = 0x00fffffe,
};

enum { CPU = 0, GPU = 1, BATTERY = 2, SKIN = 3 };  // TemperatureType
#define SEVERITIES 7                              // ThrottlingSeverity: NONE .. SHUTDOWN

static const struct sensor {
  int type;
  const char *name;
  float value;
  float hot[SEVERITIES];  // throttling thresholds; NaN where a severity has none
} kSensors[] = {
    {CPU, "cpu", 45, {NAN, NAN, NAN, 85, 90, 95, 100}},
    {GPU, "gpu", 45, {NAN, NAN, NAN, 85, 90, 95, 100}},
    {BATTERY, "battery", 30, {NAN, NAN, NAN, 50, 55, 58, 60}},
    {SKIN, "skin", 32, {NAN, 39, 41, 43, 45, 47, 50}},
};
#define SENSORS (sizeof(kSensors) / sizeof(kSensors[0]))

/* Stable parcelables are prefixed with their size, patched in once the fields are written. */
static int32_t begin_parcelable(AParcel *p) {
  AParcel_writeInt32(p, 1);  // non-null, as each element of a parcelable array is marked
  int32_t start = AParcel_getDataPosition(p);
  AParcel_writeInt32(p, 0);
  return start;
}

static void end_parcelable(AParcel *p, int32_t start) {
  int32_t end = AParcel_getDataPosition(p);
  AParcel_setDataPosition(p, start);
  AParcel_writeInt32(p, end - start);
  AParcel_setDataPosition(p, end);
}

static int wanted(const struct sensor *s, int type) { return type < 0 || s->type == type; }

static void write_temperatures(AParcel *out, int type) {
  int32_t n = 0;
  for (size_t i = 0; i < SENSORS; i++) n += wanted(&kSensors[i], type);
  AParcel_writeInt32(out, n);
  for (size_t i = 0; i < SENSORS; i++) {
    const struct sensor *s = &kSensors[i];
    if (!wanted(s, type)) continue;
    int32_t at = begin_parcelable(out);  // Temperature
    AParcel_writeInt32(out, s->type);
    AParcel_writeString(out, s->name, strlen(s->name));
    AParcel_writeFloat(out, s->value);
    AParcel_writeInt32(out, 0);  // throttlingStatus: NONE
    end_parcelable(out, at);
  }
}

static void write_thresholds(AParcel *out, int type) {
  static const float cold[SEVERITIES] = {NAN, NAN, NAN, NAN, NAN, NAN, NAN};
  int32_t n = 0;
  for (size_t i = 0; i < SENSORS; i++) n += wanted(&kSensors[i], type);
  AParcel_writeInt32(out, n);
  for (size_t i = 0; i < SENSORS; i++) {
    const struct sensor *s = &kSensors[i];
    if (!wanted(s, type)) continue;
    int32_t at = begin_parcelable(out);  // TemperatureThreshold
    AParcel_writeInt32(out, s->type);
    AParcel_writeString(out, s->name, strlen(s->name));
    AParcel_writeFloatArray(out, s->hot, SEVERITIES);
    AParcel_writeFloatArray(out, cold, SEVERITIES);
    end_parcelable(out, at);
  }
}

static binder_status_t reply_ok(AParcel *out) {
  AStatus *status = AStatus_newOk();
  binder_status_t ret = AParcel_writeStatusHeader(out, status);
  AStatus_delete(status);
  return ret;
}

static binder_status_t on_transact(AIBinder *binder, transaction_code_t code, const AParcel *in, AParcel *out) {
  (void)binder;
  int32_t type = -1;
  AIBinder *callback = NULL;
  switch (code) {
    case GET_COOLING_DEVICES:
    case GET_COOLING_DEVICES_WITH_TYPE:
      reply_ok(out);
      return AParcel_writeInt32(out, 0);
    case GET_TEMPERATURES_WITH_TYPE:
      AParcel_readInt32(in, &type);
      /* fall through */
    case GET_TEMPERATURES:
      reply_ok(out);
      write_temperatures(out, type);
      return STATUS_OK;
    case GET_TEMPERATURE_THRESHOLDS_WITH_TYPE:
      AParcel_readInt32(in, &type);
      /* fall through */
    case GET_TEMPERATURE_THRESHOLDS:
      reply_ok(out);
      write_thresholds(out, type);
      return STATUS_OK;
    case REGISTER_CALLBACK:
    case REGISTER_CALLBACK_WITH_TYPE:
      /* Kept forever: there is never anything to report, and unregistering is rare. */
      if (AParcel_readStrongBinder(in, &callback) != STATUS_OK || !callback) return STATUS_UNEXPECTED_NULL;
      return reply_ok(out);
    case UNREGISTER_CALLBACK:
      return reply_ok(out);
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
  mark_vintf(service);  // a HAL's stability, which clients built against the stable interface require
  binder_exception_t ex = add_service(service, INSTANCE);
  if (ex != EX_NONE) {
    ERR("registering %s failed: %d", INSTANCE, ex);
    return 1;
  }
  LOG("serving %s", INSTANCE);
  join_pool();
  return 0;
}
