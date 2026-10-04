// Meta's controller HAL interfaces (vendor.oculus.hardware.sensors@1.0), declared as hidl-gen would
// generate them, so Prism's implementation derives from the interface classes in Horizon's own
// vendor.oculus.hardware.sensors@1.0.so: the stubs, proxies and parcel code are Meta's.
//
// Recovered from that library: each interface's methods in vtable order (the BpHw* vtables), their
// parameter types (mangled names), their return types (what each BnHw* stub writes to the reply
// after calling the implementation: a bool, a uint32 Result, a binder, or nothing for methods that
// answer through a callback) and the interface hierarchy (the construction vtables).
//
// Structures Prism only receives are declared but not defined; their layouts aren't needed to take
// them by reference. Those Prism answers with are defined as far as their stubs show (the size each
// writes, and where it writes embedded buffers): Prism answers with them zeroed. Meta's stubs abort
// the service when a method returns without calling its callback, so every callback is called.
#pragma once

#include <android/hidl/base/1.0/IBase.h>
#include <hidl/HidlSupport.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>

namespace vendor::oculus::hardware::sensors::V1_0 {

using ::android::sp;
using ::android::hardware::hidl_array;
using ::android::hardware::hidl_vec;
using ::android::hardware::MQDescriptorSync;
using ::android::hardware::Return;

enum class Result : uint32_t { OK = 0, NOT_OK = 1 };  // the library keeps no names; any nonzero fails
enum class ControllerType : uint32_t {};
enum class CalibrationCachePolicy : uint32_t {};

struct ControllerAddr;
// A stream's notifications: the client's event flag word (a handle to its memory), and two bits
// (FmqConfig is read as a handle and 8 more bytes).
struct FmqConfig {
  ::android::hardware::hidl_handle flag;
  uint32_t bits[2];
};
static_assert(sizeof(FmqConfig) == 0x18, "as Meta's stub reads it");
// A paired controller, as libsensorscommon's ControllerProviderEndpoint copies it into the HIDL
// struct and CMSHeadset's ControllerGlue prints it ([PairedControllerInfo <type> <addr> connected
// detached ... sn= fwver= model= rev= expectedfwver= mcnt=]).
struct PairedControllerInfo {
  uint32_t type;  // ControllerType
  uint32_t reserved0;
  uint64_t addr;  // the controller's id
  bool connected, attached, update_required, asleep;
  float battery;  // percent
  char serial[16];
  char firmware[64];
  char model[64];
  char hardware_rev[64];
  char expected_firmware[64];
  uint32_t flags;  // HANDED, RIGHT_HAND (libremotedeviceutils' getDeviceHandedness)
  uint8_t extra[5][0x60];  // not known; zero
  uint32_t mcnt;
};
static_assert(sizeof(PairedControllerInfo) == 0x310, "as Meta's stubs copy it");
enum : uint32_t { RIGHT_HAND = 0x10, HANDED = 0x40 };
struct AdvertisingControllerInfo { uint8_t opaque; };  // likewise
struct ControllerWirelessFreqBlocklist { uint8_t opaque[4]; };
struct ControllerWirelessFreqBlocklists { hidl_vec<uint32_t> blocklists; };  // 4-byte elements
struct alignas(4) SecureHostInfo { uint8_t opaque[0x14]; };
struct SecureDeviceInfo;
struct ControllerCalibrationData {
  uint64_t head;
  ::android::hardware::hidl_string data;  // at 8, written as a string
};
static_assert(sizeof(ControllerCalibrationData) == 0x18, "as Meta's stub writes it");
struct alignas(8) ControllerAttachmentInfo { uint8_t opaque[0x18]; };
struct alignas(4) ControllerLedConfig { uint8_t opaque[8]; };
struct SimpleHapticIntensity;
struct ForceFeedbackProfilePoint;
struct alignas(4) ThumbstickADCRange { uint8_t opaque[8]; };
// The streams' element types.
struct CurlData;
struct ControllerImuData;
struct ButtonData;
struct PrecisionPadData;
struct MultiTouchData;
struct StylusData;
struct WirelessDeviceStats;
struct PoseInput;
struct ControllerInputADCData;
struct ControllerCollisionEvent;
struct ControllerAlerts;
struct HidInput;
struct ParsedHidInput;

struct ISensorClient;  // the client's callbacks; Prism only receives them

struct IDisposable : public ::android::hidl::base::V1_0::IBase {
  static const char *descriptor;
  virtual bool isRemote() const override { return false; }
  virtual Return<void> dispose() = 0;

  virtual Return<void> interfaceChain(interfaceChain_cb _hidl_cb) override;
  virtual Return<void> debug(const ::android::hardware::hidl_handle &fd,
                             const hidl_vec<::android::hardware::hidl_string> &options) override;
  virtual Return<void> interfaceDescriptor(interfaceDescriptor_cb _hidl_cb) override;
  virtual Return<void> getHashChain(getHashChain_cb _hidl_cb) override;
  virtual Return<void> setHALInstrumentation() override;
  virtual Return<bool> linkToDeath(const sp<::android::hardware::hidl_death_recipient> &recipient,
                                   uint64_t cookie) override;
  virtual Return<void> ping() override;
  virtual Return<void> getDebugInfo(getDebugInfo_cb _hidl_cb) override;
  virtual Return<void> notifySyspropsChanged() override;
  virtual Return<bool> unlinkToDeath(const sp<::android::hardware::hidl_death_recipient> &recipient) override;
};

// The IBase methods each interface overrides, defined in Meta's library.
#define PRISM_IBASE_OVERRIDES                                                                                 \
  virtual bool isRemote() const override { return false; }                                                    \
  virtual Return<void> interfaceChain(interfaceChain_cb _hidl_cb) override;                                   \
  virtual Return<void> debug(const ::android::hardware::hidl_handle &fd,                                      \
                             const hidl_vec<::android::hardware::hidl_string> &options) override;             \
  virtual Return<void> interfaceDescriptor(interfaceDescriptor_cb _hidl_cb) override;                         \
  virtual Return<void> getHashChain(getHashChain_cb _hidl_cb) override;                                       \
  virtual Return<void> setHALInstrumentation() override;                                                      \
  virtual Return<bool> linkToDeath(const sp<::android::hardware::hidl_death_recipient> &recipient,            \
                                   uint64_t cookie) override;                                                 \
  virtual Return<void> ping() override;                                                                       \
  virtual Return<void> getDebugInfo(getDebugInfo_cb _hidl_cb) override;                                       \
  virtual Return<void> notifySyspropsChanged() override;                                                      \
  virtual Return<bool> unlinkToDeath(const sp<::android::hardware::hidl_death_recipient> &recipient) override;

struct IControllerStreamingClient : public IDisposable {
  static const char *descriptor;
  PRISM_IBASE_OVERRIDES
  virtual Return<void> getCalibrationData(const ControllerAddr &addr, CalibrationCachePolicy policy,
                                          std::function<void(const ControllerCalibrationData &)> cb) = 0;
  virtual Return<void> getAttachmentInfo(const ControllerAddr &addr,
                                         std::function<void(const ControllerAttachmentInfo &)> cb) = 0;
  virtual Return<void> getAttachmentAuthBufferSizes(const ControllerAddr &addr,
                                                    std::function<void(uint32_t, uint32_t, uint32_t)> cb) = 0;
  virtual Return<void> authenticateAttachment(
      const ControllerAddr &addr, const hidl_vec<uint8_t> &challenge, uint32_t a, uint32_t b,
      std::function<void(bool, const hidl_vec<uint8_t> &, const hidl_vec<uint8_t> &)> cb) = 0;
  virtual Return<void> enable(const ControllerAddr &addr) = 0;
  virtual Return<void> disable(const ControllerAddr &addr) = 0;
  virtual Return<bool> controlInputADCStreaming(const ControllerAddr &addr, bool on) = 0;
  virtual Return<bool> setLedOntime(const ControllerAddr &addr, uint32_t ontime) = 0;
  virtual Return<bool> setLedConfig(const ControllerLedConfig &config) = 0;
  virtual Return<void> getLedConfig(const ControllerAddr &addr,
                                    std::function<void(const ControllerLedConfig &, bool)> cb) = 0;
  virtual Return<bool> setTransmitPowerBoost(int8_t boost) = 0;
  virtual Return<bool> setSimpleHaptics(const ControllerAddr &addr, uint8_t intensity) = 0;
  virtual Return<bool> setMultiSimpleHaptics(const ControllerAddr &addr,
                                             const hidl_array<SimpleHapticIntensity, 6> &intensities) = 0;
  virtual Return<bool> setBufferedHaptics(const ControllerAddr &addr, double rate, uint8_t a, bool b,
                                          const hidl_vec<uint8_t> &samples) = 0;
  virtual Return<bool> appendBufferedHaptics(const ControllerAddr &addr, uint8_t a, const hidl_vec<uint8_t> &samples) = 0;
  virtual Return<bool> appendPCMHaptics(const ControllerAddr &addr, const hidl_vec<int8_t> &samples) = 0;
  virtual Return<bool> setForceFeedbackProfile(const ControllerAddr &addr,
                                               const hidl_array<ForceFeedbackProfilePoint, 15> &points, uint8_t a) = 0;
  virtual Return<bool> setThumbstickMaxADCRange(const ControllerAddr &addr, const ThumbstickADCRange &range) = 0;
  virtual Return<void> getThumbstickMaxADCRange(const ControllerAddr &addr,
                                                std::function<void(const ThumbstickADCRange &, bool)> cb) = 0;
  virtual Return<bool> resetThumbstickMaxADCRange(const ControllerAddr &addr) = 0;
  virtual Return<bool> setThumbstickDeadzone(const ControllerAddr &addr, float deadzone) = 0;
  virtual Return<void> getThumbstickDeadzone(const ControllerAddr &addr, std::function<void(float, bool)> cb) = 0;
  virtual Return<bool> resetThumbstickDeadzone(const ControllerAddr &addr) = 0;
  virtual Return<bool> sleepController(const ControllerAddr &addr) = 0;
  virtual Return<bool> wakeController(const ControllerAddr &addr) = 0;
};

struct IControllerManagementClient : public IDisposable {
  struct FWUpdateCallbackConfig;
  static const char *descriptor;
  PRISM_IBASE_OVERRIDES
  virtual Return<void> getAdvertisingControllers(std::function<void(const hidl_vec<AdvertisingControllerInfo> &)> cb) = 0;
  virtual Return<Result> enterDMM(const ControllerAddr &addr) = 0;
  virtual Return<Result> exitDMM(const ControllerAddr &addr) = 0;
  virtual Return<void> pairController(const ControllerAddr &addr, std::function<void(Result, int32_t)> cb) = 0;
  virtual Return<Result> unpairController(const ControllerAddr &addr) = 0;
  virtual Return<Result> updateFirmware(const ControllerAddr &addr, ControllerType type,
                                        const FWUpdateCallbackConfig &config) = 0;
};

#define PRISM_STREAM(name, T) \
  virtual Return<Result> name(const MQDescriptorSync<T> &queue, const sp<ISensorClient> &client, const FmqConfig &config) = 0;

struct IControllerProvider : public ::android::hidl::base::V1_0::IBase {
  static const char *descriptor;
  PRISM_IBASE_OVERRIDES
  virtual Return<void> getPairedControllers(std::function<void(const hidl_vec<PairedControllerInfo> &)> cb) = 0;
  virtual Return<bool> setWirelessFreqBlocklist(const ControllerWirelessFreqBlocklist &list) = 0;
  virtual Return<void> getWirelessFreqBlocklist(std::function<void(const ControllerWirelessFreqBlocklist &, bool)> cb) = 0;
  virtual Return<bool> clearWirelessFreqBlocklist() = 0;
  virtual Return<bool> setWirelessFreqBlocklists(const ControllerWirelessFreqBlocklists &lists) = 0;
  virtual Return<void> getWirelessFreqBlocklists(std::function<void(const ControllerWirelessFreqBlocklists &, bool)> cb) = 0;
  virtual Return<bool> clearWirelessFreqBlocklists() = 0;
  virtual Return<bool> setMaxHostTxPower(int8_t power) = 0;
  virtual Return<void> getHostInfo(std::function<void(const SecureHostInfo &)> cb) = 0;
  virtual Return<bool> allowDevice(const SecureDeviceInfo &device) = 0;
  virtual Return<sp<IControllerManagementClient>> getManagementClient() = 0;
  virtual Return<sp<IControllerStreamingClient>> getStreamingClient(const sp<ISensorClient> &client) = 0;
  PRISM_STREAM(prepareStateStream, PairedControllerInfo)
  PRISM_STREAM(prepareCurlStream, CurlData)
  PRISM_STREAM(prepareImuStream, ControllerImuData)
  PRISM_STREAM(prepareInputStream, ButtonData)
  PRISM_STREAM(preparePrecisionPadStream, PrecisionPadData)
  PRISM_STREAM(prepareMultiTouchStream, MultiTouchData)
  PRISM_STREAM(prepareStylusStream, StylusData)
  PRISM_STREAM(prepareStatsStream, WirelessDeviceStats)
  PRISM_STREAM(preparePoseStream, PoseInput)
  PRISM_STREAM(prepareInputADCStream, ControllerInputADCData)
  PRISM_STREAM(prepareCollisionEventStream, ControllerCollisionEvent)
  PRISM_STREAM(prepareAlertsStream, ControllerAlerts)
  PRISM_STREAM(prepareHidInputStream, HidInput)
  PRISM_STREAM(prepareParsedHidInputStream, ParsedHidInput)

  // Registers with hwservicemanager (Meta's library makes the BnHw stub).
  ::android::status_t registerAsService(const std::string &serviceName = "default");
};

#undef PRISM_STREAM
#undef PRISM_IBASE_OVERRIDES

}  // namespace vendor::oculus::hardware::sensors::V1_0
