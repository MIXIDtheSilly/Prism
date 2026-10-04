/*
 * Prism's android.system.suspend@1.0::ISystemSuspend/default: the HIDL face of the system suspend
 * service, which Horizon's still serves beside the AIDL one and stock Android 14's no longer does.
 * Meta's libnativewakelock (CMSHeadset's, for one) takes its wakelocks through it, and aborts when
 * it isn't there.
 *
 * Each wakelock is the AIDL service's (android.system.suspend.ISystemSuspend/default): acquired
 * there under the same name, and released there when this one is released or dropped. An arm64
 * daemon, run by the translator; the HIDL stubs are those in Horizon's android.system.suspend@1.0.so.
 */
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <android/log.h>
#include <android/system/suspend/1.0/ISystemSuspend.h>
#include <android/system/suspend/1.0/IWakeLock.h>
#include <dlfcn.h>
#include <hidl/HidlTransportSupport.h>

#include <mutex>

#define TAG "PrismSuspend"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define ERR(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using ::android::sp;
using ::android::hardware::hidl_string;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::system::suspend::V1_0::ISystemSuspend;
using ::android::system::suspend::V1_0::IWakeLock;
using ::android::system::suspend::V1_0::WakeLockType;

namespace {

const char kAidlService[] = "android.system.suspend.ISystemSuspend/default";

// The AIDL interfaces, as far as Prism calls them: transactions carry the descriptor as a token.
AIBinder_Class *aidl_class(const char *descriptor) {
  return AIBinder_Class_define(
      descriptor, [](void *) -> void * { return nullptr; }, [](void *) {},
      [](AIBinder *, transaction_code_t, const AParcel *, AParcel *) -> binder_status_t { return STATUS_UNKNOWN_TRANSACTION; });
}

AIBinder *g_service;  // the AIDL ISystemSuspend
AIBinder_Class *g_wake_lock_class;

// One of the AIDL service's wakelocks, released with this one.
struct WakeLock : IWakeLock {
  explicit WakeLock(AIBinder *aidl) : aidl_(aidl) {}
  ~WakeLock() { release(); }
  Return<void> release() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!aidl_) return Void();
    AParcel *in = nullptr, *out = nullptr;
    if (AIBinder_prepareTransaction(aidl_, &in) == STATUS_OK)
      AIBinder_transact(aidl_, FIRST_CALL_TRANSACTION, &in, &out, FLAG_ONEWAY);  // release() (oneway)
    if (out) AParcel_delete(out);
    AIBinder_decStrong(aidl_);
    aidl_ = nullptr;
    return Void();
  }

 private:
  std::mutex mutex_;
  AIBinder *aidl_;
};

struct SystemSuspend : ISystemSuspend {
  Return<sp<IWakeLock>> acquireWakeLock(WakeLockType type, const hidl_string &name) override {
    AParcel *in = nullptr, *out = nullptr;
    AIBinder *lock = nullptr;
    binder_status_t status = AIBinder_prepareTransaction(g_service, &in);
    if (status == STATUS_OK) status = AParcel_writeInt32(in, static_cast<int32_t>(type));
    if (status == STATUS_OK) status = AParcel_writeString(in, name.c_str(), name.size());
    if (status == STATUS_OK) status = AIBinder_transact(g_service, FIRST_CALL_TRANSACTION, &in, &out, 0);  // acquireWakeLock
    AStatus *result = nullptr;
    if (status == STATUS_OK) status = AParcel_readStatusHeader(out, &result);
    if (status == STATUS_OK && AStatus_isOk(result)) status = AParcel_readStrongBinder(out, &lock);
    if (result) AStatus_delete(result);
    if (out) AParcel_delete(out);
    if (!lock) {
      ERR("wakelock %s: not acquired (%d)", name.c_str(), status);
      return sp<IWakeLock>();
    }
    AIBinder_associateClass(lock, g_wake_lock_class);
    return sp<IWakeLock>(new WakeLock(lock));
  }
};

}  // namespace

int main() {
  void *ndk = dlopen("libbinder_ndk.so", RTLD_NOW);
  auto wait_for_service = ndk ? reinterpret_cast<AIBinder *(*)(const char *)>(dlsym(ndk, "AServiceManager_waitForService")) : nullptr;
  if (!wait_for_service || !(g_service = wait_for_service(kAidlService))) {
    ERR("no %s", kAidlService);
    return 1;
  }
  AIBinder_associateClass(g_service, aidl_class("android.system.suspend.ISystemSuspend"));
  g_wake_lock_class = aidl_class("android.system.suspend.IWakeLock");

  ::android::hardware::configureRpcThreadpool(2, true /* this thread joins */);
  sp<ISystemSuspend> service = new SystemSuspend;
  ::android::status_t status = service->registerAsService("default");
  if (status != ::android::OK) {
    ERR("can't register %s/default: %d", ISystemSuspend::descriptor, status);
    return 1;
  }
  LOG("%s/default registered, backed by %s", ISystemSuspend::descriptor, kAidlService);
  ::android::hardware::joinRpcThreadpool();
  return 1;
}
