/*
 * Prism's controller HAL: vendor.oculus.hardware.sensors@1.0::IControllerProvider/default, which
 * Meta's sensors HAL serves on a headset (talking to the controllers over the headset's radio).
 * CMSHeadset's controller service asks for it as it starts, and stops without it.
 *
 * Here two Touch Plus controllers are paired and connected, the ones Prism's tracking service
 * tracks (native/tracking_prism, the same ids). The provider lists them, and writes their states into
 * the state stream CMSHeadset prepares (one PairedControllerInfo each), waking it through the event
 * flag its FmqConfig shares, as a headset's sensors HAL does; CMSHeadset's ControllerGlue then
 * reports them paired and active (OVRRemoteService, which first-time setup asks). It reads each one's
 * calibration, and blocks a controller whose calibration is empty. The other streams' queues are
 * accepted and left unwritten (Prism's tracking regions carry the controllers' input and poses),
 * and the streaming and management clients answer everything else with "no", or with an empty
 * answer: Meta's stubs abort the service when a method doesn't call its callback. An arm64 daemon,
 * run by the translator: its interfaces derive from those in Horizon's
 * vendor.oculus.hardware.sensors@1.0.so, whose stubs serve them.
 */
#include <android/log.h>
#include <fmq/EventFlag.h>
#include <fmq/MessageQueue.h>
#include <hidl/HidlTransportSupport.h>
#include <sys/mman.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <time.h>
#include <vector>

#include "sensors_controller.h"

#define TAG "PrismController"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define ERR(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using namespace vendor::oculus::hardware::sensors::V1_0;
using ::android::sp;
using ::android::hardware::hidl_array;
using ::android::hardware::hidl_vec;
using ::android::hardware::EventFlag;
using ::android::hardware::kSynchronizedReadWrite;
using ::android::hardware::MessageQueue;
using ::android::hardware::MQDescriptorSync;
using ::android::hardware::Return;
using ::android::hardware::Void;

namespace {

// The controllers: two Touch Plus (Quest 3's), paired and connected, as a headset's HAL reports
// them. Their ids are the ones Prism's tracking service gives them (native/tracking_prism).
constexpr uint32_t RUBYPRQ = 6;  // ControllerType: Touch Plus
constexpr uint64_t LEFT_ID = 1, RIGHT_ID = 2;

PairedControllerInfo controller(uint64_t id, bool right, const char *serial, bool connected) {
  PairedControllerInfo c{};
  c.type = RUBYPRQ;
  c.flags = HANDED | (right ? RIGHT_HAND : LEFT_HAND);
  c.addr = id;
  c.connected = connected;
  c.battery = 100;
  strncpy(c.serial, serial, sizeof c.serial - 1);
  strcpy(c.firmware, "206.3.0");
  strcpy(c.model, "ICM42686");  // its IMU
  strcpy(c.hardware_rev, "0x9a");
  return c;
}

// Paired, and connected a moment after a client prepares its state stream, as a headset's
// controllers connect after it starts: CMSHeadset connects a controller as its state says so,
// reading its calibration through the streaming client it gets after listing the paired ones.
std::atomic<int64_t> g_stream_prepared;  // CLOCK_MONOTONIC seconds
int64_t now_s() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}
std::vector<PairedControllerInfo> controllers() {
  bool connected = now_s() - g_stream_prepared >= 3;
  return {controller(RIGHT_ID, true, "PRISM0RIGHT", connected), controller(LEFT_ID, false, "PRISM0LEFT", connected)};
}

// A client's queue of controllers' states: each state change is an entry, and the client is woken
// through the event flag its FmqConfig shares.
struct StateStream {
  std::unique_ptr<MessageQueue<PairedControllerInfo, kSynchronizedReadWrite>> queue;
  EventFlag *flag = nullptr;
  uint32_t bits = 0;
};
std::mutex g_streams_lock;
std::vector<std::unique_ptr<StateStream>> g_streams;

void send_states(StateStream &stream) {
  for (const PairedControllerInfo &c : controllers())
    if (!stream.queue->write(&c)) ERR("state stream full");
  if (stream.flag) stream.flag->wake(stream.bits);
}

// The controllers connect a few seconds in, and their states go again now and then, as a headset's
// HAL repeats them.
void repeat_states() {
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      std::lock_guard<std::mutex> lock(g_streams_lock);
      for (auto &stream : g_streams) send_states(*stream);
    }
  }).detach();
}

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
  Return<void> getCalibrationData(const ControllerAddr &addr, CalibrationCachePolicy,
                                  std::function<void(const ControllerCalibrationData &)> cb) override {
    uint64_t id;
    memcpy(&id, &addr, sizeof id);
    LOG("calibration of %016llx", (unsigned long long)id);
    // CMSHeadset blocks a controller whose calibration is empty. A headset's trackingservice reads
    // it (the controller's LEDs and IMU); Prism's tracking is its own, so this one is nominal.
    ControllerCalibrationData data{};
    data.data = "{}";
    cb(data);
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
    ONCE("paired controllers: two");
    hidl_vec<PairedControllerInfo> paired(controllers());
    cb(paired);
    return Void();
  }
  Return<Result> prepareStateStream(const MQDescriptorSync<PairedControllerInfo> &desc, const sp<ISensorClient> &,
                                    const FmqConfig &config) override {
    g_stream_prepared = now_s();
    auto stream = std::make_unique<StateStream>();
    stream->queue.reset(new MessageQueue<PairedControllerInfo, kSynchronizedReadWrite>(desc, false));
    const native_handle_t *handle = config.flag.getNativeHandle();
    LOG("state stream: queue %s, quantum %zu, %zu bytes; flag handle %d fds %d ints; bits %#x %#x",
        stream->queue->isValid() ? "valid" : "invalid", desc.getQuantum(), desc.getSize(),
        handle ? handle->numFds : -1, handle ? handle->numInts : -1, config.bits[0], config.bits[1]);
    if (!stream->queue->isValid()) return Result::NOT_OK;
    if (handle && handle->numFds > 0) {
      void *word = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, handle->data[0], 0);
      if (word == MAP_FAILED || EventFlag::createEventFlag(static_cast<std::atomic<uint32_t> *>(word), &stream->flag) != ::android::OK)
        ERR("state stream: no event flag");
    } else if (stream->queue->getEventFlagWord()) {
      EventFlag::createEventFlag(stream->queue->getEventFlagWord(), &stream->flag);
    }
    stream->bits = config.bits[0] | config.bits[1];
    send_states(*stream);
    std::lock_guard<std::mutex> lock(g_streams_lock);
    g_streams.push_back(std::move(stream));
    return Result::OK;
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
  LOG("%s/default registered: two controllers paired", IControllerProvider::descriptor);
  repeat_states();
  ::android::hardware::joinRpcThreadpool();
  return 1;
}
