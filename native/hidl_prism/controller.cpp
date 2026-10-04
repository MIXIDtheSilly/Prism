/*
 * Prism's controller HAL: vendor.oculus.hardware.sensors@1.0::IControllerProvider/default, which
 * Meta's sensors HAL serves on a headset (talking to the controllers over the headset's radio).
 * CMSHeadset's controller service asks for it as it starts, and stops without it.
 *
 * Here no controllers are paired: the provider lists none, accepts the queues clients prepare for
 * each stream (it would write controller data into them), and its streaming and management clients
 * answer every request with "no", or with an empty answer: Meta's stubs abort the service when a
 * method doesn't call its callback. An arm64 daemon, run by the translator: its interfaces derive
 * from those in Horizon's vendor.oculus.hardware.sensors@1.0.so, whose stubs serve them.
 */
#include <android/log.h>
#include <hidl/HidlTransportSupport.h>

#include <atomic>

#include "sensors_controller.h"

#define TAG "PrismController"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define ERR(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using namespace vendor::oculus::hardware::sensors::V1_0;
using ::android::sp;
using ::android::hardware::hidl_array;
using ::android::hardware::hidl_vec;
using ::android::hardware::MQDescriptorSync;
using ::android::hardware::Return;
using ::android::hardware::Void;

namespace {

// Logs a request the first time it comes.
#define ONCE(what)                     \
  do {                                 \
    static std::atomic_bool logged;    \
    if (!logged.exchange(true)) LOG("%s", what); \
  } while (0)

struct StreamingClient : IControllerStreamingClient {
  Return<void> dispose() override {
    ONCE("streaming client disposed");
    return Void();
  }
  Return<void> getCalibrationData(const ControllerAddr &, CalibrationCachePolicy,
                                  std::function<void(const ControllerCalibrationData &)> cb) override {
    cb({});  // no controller: empty
    return Void();
  }
  Return<void> getAttachmentInfo(const ControllerAddr &, std::function<void(const ControllerAttachmentInfo &)> cb) override {
    cb({});
    return Void();
  }
  Return<void> getAttachmentAuthBufferSizes(const ControllerAddr &, std::function<void(uint32_t, uint32_t, uint32_t)> cb) override {
    cb(0, 0, 0);
    return Void();
  }
  Return<void> authenticateAttachment(const ControllerAddr &, const hidl_vec<uint8_t> &, uint32_t, uint32_t,
                                      std::function<void(bool, const hidl_vec<uint8_t> &, const hidl_vec<uint8_t> &)> cb) override {
    cb(false, {}, {});
    return Void();
  }
  Return<void> enable(const ControllerAddr &) override {
    ONCE("enable");
    return Void();
  }
  Return<void> disable(const ControllerAddr &) override { return Void(); }
  Return<bool> controlInputADCStreaming(const ControllerAddr &, bool) override { return false; }
  Return<bool> setLedOntime(const ControllerAddr &, uint32_t) override { return false; }
  Return<bool> setLedConfig(const ControllerLedConfig &) override { return false; }
  Return<void> getLedConfig(const ControllerAddr &, std::function<void(const ControllerLedConfig &, bool)> cb) override {
    cb({}, false);
    return Void();
  }
  Return<bool> setTransmitPowerBoost(int8_t) override { return false; }
  Return<bool> setSimpleHaptics(const ControllerAddr &, uint8_t) override { return false; }
  Return<bool> setMultiSimpleHaptics(const ControllerAddr &, const hidl_array<SimpleHapticIntensity, 6> &) override {
    return false;
  }
  Return<bool> setBufferedHaptics(const ControllerAddr &, double, uint8_t, bool, const hidl_vec<uint8_t> &) override {
    return false;
  }
  Return<bool> appendBufferedHaptics(const ControllerAddr &, uint8_t, const hidl_vec<uint8_t> &) override { return false; }
  Return<bool> appendPCMHaptics(const ControllerAddr &, const hidl_vec<int8_t> &) override { return false; }
  Return<bool> setForceFeedbackProfile(const ControllerAddr &, const hidl_array<ForceFeedbackProfilePoint, 15> &,
                                       uint8_t) override {
    return false;
  }
  Return<bool> setThumbstickMaxADCRange(const ControllerAddr &, const ThumbstickADCRange &) override { return false; }
  Return<void> getThumbstickMaxADCRange(const ControllerAddr &, std::function<void(const ThumbstickADCRange &, bool)> cb) override {
    cb({}, false);
    return Void();
  }
  Return<bool> resetThumbstickMaxADCRange(const ControllerAddr &) override { return false; }
  Return<bool> setThumbstickDeadzone(const ControllerAddr &, float) override { return false; }
  Return<void> getThumbstickDeadzone(const ControllerAddr &, std::function<void(float, bool)> cb) override {
    cb(0, false);
    return Void();
  }
  Return<bool> resetThumbstickDeadzone(const ControllerAddr &) override { return false; }
  Return<bool> sleepController(const ControllerAddr &) override { return false; }
  Return<bool> wakeController(const ControllerAddr &) override { return false; }
};

struct ManagementClient : IControllerManagementClient {
  Return<void> dispose() override {
    ONCE("management client disposed");
    return Void();
  }
  Return<void> getAdvertisingControllers(std::function<void(const hidl_vec<AdvertisingControllerInfo> &)> cb) override {
    ONCE("advertising controllers: none");
    cb({});
    return Void();
  }
  // No controller to put in or out of its firmware mode, pair or update.
  Return<Result> enterDMM(const ControllerAddr &) override { return Result::OK; }
  Return<Result> exitDMM(const ControllerAddr &) override { return Result::OK; }
  Return<void> pairController(const ControllerAddr &, std::function<void(Result, int32_t)> cb) override {
    ONCE("pair controller: no controller");
    cb(Result::NOT_OK, 0);
    return Void();
  }
  Return<Result> unpairController(const ControllerAddr &) override { return Result::OK; }
  Return<Result> updateFirmware(const ControllerAddr &, ControllerType, const FWUpdateCallbackConfig &) override {
    return Result::OK;
  }
};

#define STREAM(name, T)                                                                                           \
  Return<Result> name(const MQDescriptorSync<T> &, const sp<ISensorClient> &, const FmqConfig &) override { \
    ONCE(#name);                                                                                                  \
    return Result::OK;                                                                                            \
  }

struct Provider : IControllerProvider {
  Return<void> getPairedControllers(std::function<void(const hidl_vec<PairedControllerInfo> &)> cb) override {
    ONCE("paired controllers: none");
    cb({});
    return Void();
  }
  Return<bool> setWirelessFreqBlocklist(const ControllerWirelessFreqBlocklist &) override { return true; }
  // Blocklists are accepted and dropped: there's no radio to keep them from.
  Return<void> getWirelessFreqBlocklist(std::function<void(const ControllerWirelessFreqBlocklist &, bool)> cb) override {
    cb({}, true);
    return Void();
  }
  Return<bool> clearWirelessFreqBlocklist() override { return true; }
  Return<bool> setWirelessFreqBlocklists(const ControllerWirelessFreqBlocklists &) override { return true; }
  Return<void> getWirelessFreqBlocklists(std::function<void(const ControllerWirelessFreqBlocklists &, bool)> cb) override {
    cb({}, true);
    return Void();
  }
  Return<bool> clearWirelessFreqBlocklists() override { return true; }
  Return<bool> setMaxHostTxPower(int8_t) override { return true; }
  Return<void> getHostInfo(std::function<void(const SecureHostInfo &)> cb) override {
    cb({});
    return Void();
  }
  Return<bool> allowDevice(const SecureDeviceInfo &) override { return false; }
  Return<sp<IControllerManagementClient>> getManagementClient() override {
    ONCE("management client");
    return sp<IControllerManagementClient>(new ManagementClient);
  }
  Return<sp<IControllerStreamingClient>> getStreamingClient(const sp<ISensorClient> &) override {
    ONCE("streaming client");
    return sp<IControllerStreamingClient>(new StreamingClient);
  }
  STREAM(prepareStateStream, PairedControllerInfo)
  STREAM(prepareCurlStream, CurlData)
  STREAM(prepareImuStream, ControllerImuData)
  STREAM(prepareInputStream, ButtonData)
  STREAM(preparePrecisionPadStream, PrecisionPadData)
  STREAM(prepareMultiTouchStream, MultiTouchData)
  STREAM(prepareStylusStream, StylusData)
  STREAM(prepareStatsStream, WirelessDeviceStats)
  STREAM(preparePoseStream, PoseInput)
  STREAM(prepareInputADCStream, ControllerInputADCData)
  STREAM(prepareCollisionEventStream, ControllerCollisionEvent)
  STREAM(prepareAlertsStream, ControllerAlerts)
  STREAM(prepareHidInputStream, HidInput)
  STREAM(prepareParsedHidInputStream, ParsedHidInput)
};

}  // namespace

int main() {
  ::android::hardware::configureRpcThreadpool(4, true /* this thread joins */);
  sp<IControllerProvider> provider = new Provider;
  ::android::status_t status = provider->registerAsService("default");
  if (status != ::android::OK) {
    ERR("can't register %s/default: %d", IControllerProvider::descriptor, status);
    return 1;
  }
  LOG("%s/default registered: no controllers paired", IControllerProvider::descriptor);
  ::android::hardware::joinRpcThreadpool();
  return 1;
}
