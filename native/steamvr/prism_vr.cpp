// Prism in a PC headset: Horizon through SteamVR (or any PC OpenXR runtime with Direct3D 11).
//
// An OpenXR app on the PC, the way Refract's host bridge is one; its loader, session and controller
// actions follow Refract's. It does what tools/viewer.py does with the mouse, with the headset:
//
// - Tracking. Each display frame of the PC runtime, the headset's pose and both controllers' go to
//   Prism's tracking service (native/tracking_prism) as tools/head.py's lines, through `adb
//   forward`: Horizon's compositor and apps then render from where the headset is. Poses are
//   relative to an origin set where the headset is when tracking starts (and again with R), facing
//   ahead, as the viewer's start at the origin. The PC runtime's own recenter (SteamVR's Reset View,
//   a held Oculus button) recenters it too, as does F9 in any window.
// - Frames. What the compositor puts on the emulator's display (both eyes side by side) comes from
//   the frames the emulator shares (-share-vid: a VideoInfo, then BGRA, in SHM_videmulator<port>).
//   Each new one is copied into an OpenXR swapchain and submitted as a projection layer, each half
//   one view, posed where the headset was when Horizon rendered it. The PC runtime then reprojects
//   it to where the headset is now, so the world stays put while Horizon runs at its own rate.
//
// Which pose a frame was rendered with isn't carried with it, so it's taken from the poses sent,
// --latency milliseconds before the emulator shared the frame (its VideoInfo's time, Unix
// microseconds; + and - change it while running). Too short or too long, the world swims as the
// head turns; right, it holds still.
//
// Each half is what an app rendered from one of Horizon's eyes, turned by the compositor to the
// head's latest orientation (a rotation: directions are kept), undistorted: the middle of what apps
// render. tools/viewer.py fitted the halves' edges (kViews) by rays from the head's center that hit
// panels at 1.08 m; seen from the eyes, ±IPD/2 to the sides, those edges move by the parallax at
// that depth, and each view is submitted from its eye with them moved (exact for every depth if the
// halves are the eyes' images turned, as above). --ipd is Horizon's (xrprobe logs its eyes: "eye N:
// ... at x"). The halves show about 2.0 by 2.3 tangent units (deploy's debug.oculus.metersPerTanAngCtr:
// nearly all apps render, 91° by 97°); a headset's lenses are wider, so black is round them.
//
// Controllers: the runtime's aim poses, turned back as the viewer's are (Horizon's runtime turns a
// published pose 5° in to aim). Trigger, grip, A/B and X/Y, the thumbstick (its axes and click; a
// Vive wand's trackpad) and the touches go through; the left menu button presses the Meta button
// (the Universal Menu), since PC runtimes keep the right system button for themselves. On the
// keyboard, in any window (so with the headset on), F7 holds the Meta button and F8 the left menu
// button. Haptics come back: the tracking service sends each controller's vibration as Horizon's
// runtime sets it ("haptic l|r amplitude"), played until it's set to 0 (and renewed while it lasts,
// so a lost stop ends within a second), and pulses ("haptic l|r amplitude seconds": VrShell's UI).
//
// With --control it also takes commands, a line each, on stdin: what tools/prism_ui.pyw sends from
// its buttons. "recenter", "latency MS", "meta down|up" and "menu down|up" (held as F7 and F8 hold
// them), "quit"; stdin's end quits too, so the bridge goes with the window that started it.
//
// Built and started by tools/steamvr.py.
#define _USE_MATH_DEFINES
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#define XR_NO_PROTOTYPES  // every function comes from the loader, by name

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <conio.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <mmsystem.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

// --- Where things are -------------------------------------------------------------------------

// What each eye's half of the display shows, as tangents of its edges' angles (left, right, up,
// down), from the head's center: tools/viewer.py's kViews, fitted to the compositor's output.
constexpr double kViews[2][4] = {{-1.0546, 0.9704, 1.0885, -1.1895}, {-0.9704, 1.0546, 1.0885, -1.1895}};
constexpr double kFitDepth = 1.08;  // meters ahead of the head that kViews were fitted at (the viewer's kAimDepth)
// The runtime's aim pose is the published pose turned this many degrees in (xrprobe, right hand: q 0
// .0436 0 .999) and moved this far (xrprobe: p -.009 0 0); the left hand's is taken as its mirror.
constexpr double kAimTurn = 5.0;
constexpr float kAimOffset = 0.009f;
// tracking_prism.c's BUTTON_*.
constexpr unsigned kTrigger = 0x1, kMeta = 0x2, kMenu = 0x4, kStick = 0x8, kTriggerTouch = 0x40, kGrip = 0x80,
                   kButtonA = 0x100, kButtonATouch = 0x200, kButtonB = 0x400, kButtonBTouch = 0x800;
constexpr float kPressed = 0.5f;  // an analog trigger or grip past this is its button pressed
// Keys that hold buttons down, read whichever window has focus.
constexpr int kMetaKey = VK_F7;  // the Meta button (the Universal Menu)
constexpr int kMenuKey = VK_F8;  // the left controller's menu button
constexpr int kRecenterKey = VK_F9;

// The emulator's VideoInfo, ahead of the pixels.
struct VideoInfo {
    uint32_t width, height, fps, frame;
    uint64_t time;  // when it was shared: Unix time, microseconds
};
static_assert(sizeof(VideoInfo) == 24);

std::atomic<bool> g_quit{false};
// --control: the buttons held from stdin, and the other commands, waiting for the main loop.
std::atomic<bool> g_metaHeld{false}, g_menuHeld{false};
std::mutex g_commandsLock;
std::deque<std::string> g_commands;

// --- OpenXR functions, from the loader ---------------------------------------------------------

#define XR_FUNCTIONS(X)                                                                                         \
    X(xrCreateInstance) X(xrDestroyInstance) X(xrGetSystem)                                                     \
    X(xrGetD3D11GraphicsRequirementsKHR) X(xrCreateSession) X(xrDestroySession) X(xrBeginSession)               \
    X(xrEndSession) X(xrRequestExitSession) X(xrPollEvent) X(xrCreateReferenceSpace) X(xrDestroySpace)          \
    X(xrLocateSpace) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain)                       \
    X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage)      \
    X(xrReleaseSwapchainImage) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrStringToPath)                   \
    X(xrCreateActionSet) X(xrCreateAction) X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets)  \
    X(xrCreateActionSpace) X(xrSyncActions) X(xrGetActionStatePose) X(xrGetActionStateFloat)                    \
    X(xrGetActionStateBoolean) X(xrGetActionStateVector2f) X(xrApplyHapticFeedback) X(xrStopHapticFeedback)

#define DECLARE(name) PFN_##name name = nullptr;
XR_FUNCTIONS(DECLARE)
#undef DECLARE
PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;

// Loads the loader: PRISM_OPENXR_LOADER, then one on the path, then SteamVR's (as Refract does).
bool load_loader()
{
    HMODULE library = nullptr;
    char path[1024]{};
    const DWORD size = GetEnvironmentVariableA("PRISM_OPENXR_LOADER", path, sizeof(path));
    if (size > 0 && size < sizeof(path)) library = LoadLibraryA(path);
    if (!library) library = LoadLibraryA("openxr_loader.dll");
    if (!library) library = LoadLibraryA("C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openxr_loader.dll");
    if (!library) {
        std::fprintf(stderr, "prism_vr: no OpenXR loader (openxr_loader.dll; PRISM_OPENXR_LOADER names one)\n");
        return false;
    }
    xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(library, "xrGetInstanceProcAddr"));
    if (!xrGetInstanceProcAddr) return false;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance));
    return xrCreateInstance != nullptr;
}

bool load_functions(XrInstance instance)
{
    bool ok = true;
#define LOAD(name)                                                                                     \
    if (!name && XR_FAILED(xrGetInstanceProcAddr(instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&name)))) { \
        std::fprintf(stderr, "prism_vr: the runtime has no %s\n", #name);                              \
        ok = false;                                                                                    \
    }
    XR_FUNCTIONS(LOAD)
#undef LOAD
    return ok;
}

bool check(XrResult result, const char* what)
{
    if (XR_SUCCEEDED(result)) return true;
    std::fprintf(stderr, "prism_vr: %s failed: %d\n", what, static_cast<int>(result));
    return false;
}

// --- Poses ------------------------------------------------------------------------------------

XrQuaternionf multiply(XrQuaternionf a, XrQuaternionf b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

XrVector3f rotate(XrQuaternionf q, XrVector3f v)
{
    const XrVector3f t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + q.y * t.z - q.z * t.y, v.y + q.w * t.y + q.z * t.x - q.x * t.z,
            v.z + q.w * t.z + q.x * t.y - q.y * t.x};
}

// Turned by yaw degrees about Y, left positive.
XrQuaternionf yaw_rotation(double degrees)
{
    const double half = degrees * M_PI / 360;
    return {0, static_cast<float>(std::sin(half)), 0, static_cast<float>(std::cos(half))};
}

// The pose's heading alone, at its position: where the origin goes when recentering.
XrPosef heading(const XrPosef& pose)
{
    const XrVector3f ahead = rotate(pose.orientation, {0, 0, -1});
    return {yaw_rotation(std::atan2(-ahead.x, -ahead.z) * 180 / M_PI), pose.position};
}

bool tracked(XrSpaceLocationFlags flags)
{
    return (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
}

// The head poses sent to Prism, with when they were sent, for finding the one a frame was rendered with.
class PoseHistory {
public:
    void add(Clock::time_point at, const XrPosef& pose)
    {
        entries_[next_++ % entries_.size()] = {at, pose};
    }
    // The newest pose sent no later than at (the oldest kept, if all are newer).
    bool at(Clock::time_point at, XrPosef* pose) const
    {
        const size_t count = std::min(next_, entries_.size());
        for (size_t i = 1; i <= count; ++i) {
            const auto& entry = entries_[(next_ - i) % entries_.size()];
            *pose = entry.pose;
            if (entry.at <= at) return true;
        }
        return count > 0;
    }
    void clear() { next_ = 0; }

private:
    struct Entry {
        Clock::time_point at;
        XrPosef pose;
    };
    std::array<Entry, 512> entries_{};  // about 5 s at 90 Hz
    size_t next_ = 0;
};

// --- Prism's tracking service -----------------------------------------------------------------

// The connection to the tracking service, through adb forward; reconnects once a second. Lines come
// back on it too (the controllers' haptics).
class Tracking {
public:
    explicit Tracking(int port) : port_(port) {}

    void send(const std::string& lines)
    {
        if (sock_ == INVALID_SOCKET) {
            const auto now = Clock::now();
            if (now < retry_) return;
            retry_ = now + std::chrono::seconds(1);
            sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<u_short>(port_));
            inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
            const BOOL noDelay = TRUE;
            setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
            if (connect(sock_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
                close();
                return;
            }
            sentInput_[0].clear();
            sentInput_[1].clear();
        }
        if (::send(sock_, lines.data(), static_cast<int>(lines.size()), 0) != static_cast<int>(lines.size())) close();
    }

    // An input line only when it changed (head.py's way): a new connection gets each once.
    void add_input(std::string& lines, int hand, const std::string& input)
    {
        if (input == sentInput_[hand]) return;
        lines += input;
        sentInput_[hand] = input;
    }

    bool connected() const { return sock_ != INVALID_SOCKET; }

    // The next whole line the service sent, without its newline; false if there's none yet.
    bool receive(std::string* line)
    {
        u_long waiting = 0;
        if (sock_ != INVALID_SOCKET && ioctlsocket(sock_, FIONREAD, &waiting) == 0 && waiting > 0) {
            std::string data(waiting, '\0');
            const int got = recv(sock_, data.data(), static_cast<int>(waiting), 0);
            if (got > 0) received_.append(data, 0, got);
            else close();
        }
        const size_t end = received_.find('\n');
        if (end == std::string::npos) return false;
        *line = received_.substr(0, end);
        received_.erase(0, end + 1);
        return true;
    }

private:
    void close()
    {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        received_.clear();
    }
    int port_;
    SOCKET sock_ = INVALID_SOCKET;
    Clock::time_point retry_{};
    std::string sentInput_[2];
    std::string received_;
};

std::string pose_line(const char* prefix, const XrPosef& p)
{
    char line[160];
    std::snprintf(line, sizeof(line), "%s%.6f %.6f %.6f %.6f %.6f %.6f %.6f\n", prefix, p.position.x, p.position.y,
                  p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
    return line;
}

// --- The emulator's frames --------------------------------------------------------------------

class Frames {
public:
    explicit Frames(int emulatorPort) : name_(L"SHM_videmulator" + std::to_wstring(emulatorPort)) {}

    // The mapping, once the emulator shares it (looked for twice a second); kept for good, since a
    // restarted emulator shares the same one.
    const VideoInfo* info()
    {
        if (!base_ && Clock::now() >= retry_) {
            retry_ = Clock::now() + std::chrono::milliseconds(500);
            if (HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name_.c_str())) {
                base_ = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
                if (!base_) CloseHandle(mapping);
            }
        }
        return reinterpret_cast<const VideoInfo*>(base_);
    }
    const uint8_t* pixels() const { return base_ + sizeof(VideoInfo); }

private:
    std::wstring name_;
    const uint8_t* base_ = nullptr;
    Clock::time_point retry_{};
};

// --- Direct3D ---------------------------------------------------------------------------------

// The frame as it came (BGRA, sRGB-encoded) is sampled through an sRGB view and drawn into the
// swapchain's image, whose format may be sRGB (encoded again: the same bytes) or not (linear, as
// the runtime takes a non-sRGB image).
const char kShader[] = R"(
Texture2D image : register(t0);
SamplerState pointSampler : register(s0);
struct VsOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VsOut vs(uint id : SV_VertexID) {
    VsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
float4 ps(VsOut i) : SV_Target { return float4(image.Sample(pointSampler, i.uv).rgb, 1); }
)";

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Texture2D> frame;  // the emulator's latest frame
    ComPtr<ID3D11ShaderResourceView> frameView;
    UINT width = 0, height = 0;

    // On the adapter the runtime asks for.
    bool create(LUID adapterLuid)
    {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == adapterLuid.LowPart && desc.AdapterLuid.HighPart == adapterLuid.HighPart) break;
            adapter.Reset();
        }
        if (!adapter) {
            std::fprintf(stderr, "prism_vr: the runtime's adapter isn't there\n");
            return false;
        }
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                     2, D3D11_SDK_VERSION, &device, nullptr, &context))) return false;
        ComPtr<ID3DBlob> vsBlob, psBlob, errors;
        if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "prism_vr", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsBlob, &errors)) ||
            FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "prism_vr", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psBlob, &errors))) {
            std::fprintf(stderr, "prism_vr: shader: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            return false;
        }
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;  // drawn 1:1
        samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        return SUCCEEDED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs)) &&
               SUCCEEDED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps)) &&
               SUCCEEDED(device->CreateSamplerState(&samplerDesc, &sampler));
    }

    bool upload(const VideoInfo& info, const uint8_t* pixels)
    {
        if (!frame || width != info.width || height != info.height) {
            frame.Reset();
            frameView.Reset();
            const D3D11_TEXTURE2D_DESC desc{info.width, info.height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, {1, 0},
                                            D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &frame)) ||
                FAILED(device->CreateShaderResourceView(frame.Get(), nullptr, &frameView))) return false;
            width = info.width;
            height = info.height;
        }
        context->UpdateSubresource(frame.Get(), 0, nullptr, pixels, width * 4, 0);
        return true;
    }

    void draw(ID3D11RenderTargetView* target)
    {
        const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        context->OMSetRenderTargets(1, &target, nullptr);
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(nullptr);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, frameView.GetAddressOf());
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        context->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(0, 1, &none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }
};

// --- The OpenXR side --------------------------------------------------------------------------

enum Input { Trigger, Squeeze, Primary, Secondary, Menu, StickClick, PrimaryTouch, SecondaryTouch, TriggerTouch, Inputs };

class Vr {
public:
    Vr(Gpu& gpu, Tracking& tracking, Frames& frames, double latencyMs, double ipd, bool metaFromMenu)
        : gpu_(gpu), tracking_(tracking), frames_(frames), latencyMs_(latencyMs), metaFromMenu_(metaFromMenu)
    {
        // Each view from Horizon's eye, its edges moved by the parallax at the depth they were fitted at.
        for (int i = 0; i < 2; ++i) {
            const double eye = (i ? 0.5 : -0.5) * ipd, shift = -eye / kFitDepth;
            eyes_[i] = {static_cast<float>(eye), 0, 0};
            fov_[i] = {static_cast<float>(std::atan(kViews[i][0] + shift)), static_cast<float>(std::atan(kViews[i][1] + shift)),
                       static_cast<float>(std::atan(kViews[i][2])), static_cast<float>(std::atan(kViews[i][3]))};
        }
    }

    ~Vr()
    {
        if (swapchain_) xrDestroySwapchain(swapchain_);
        if (session_) xrDestroySession(session_);  // its spaces and the action set's go with it
        if (instance_) xrDestroyInstance(instance_);
    }

    bool create()
    {
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strncpy(info.applicationInfo.applicationName, "Prism", XR_MAX_APPLICATION_NAME_SIZE - 1);
        std::strncpy(info.applicationInfo.engineName, "Prism", XR_MAX_ENGINE_NAME_SIZE - 1);
        info.applicationInfo.applicationVersion = info.applicationInfo.engineVersion = 1;
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        info.enabledExtensionCount = 1;
        info.enabledExtensionNames = extensions;
        if (!check(xrCreateInstance(&info, &instance_), "xrCreateInstance (is SteamVR the OpenXR runtime?)") ||
            !load_functions(instance_)) return false;

        // No headset yet: wait for one.
        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        for (bool told = false; !g_quit; told = true) {
            const XrResult result = xrGetSystem(instance_, &systemInfo, &system_);
            if (result == XR_SUCCESS) break;
            if (result != XR_ERROR_FORM_FACTOR_UNAVAILABLE) return check(result, "xrGetSystem");
            if (!told) std::printf("prism_vr: waiting for a headset\n");
            Sleep(1000);
        }
        if (g_quit) return false;

        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        if (!check(xrGetD3D11GraphicsRequirementsKHR(instance_, system_, &requirements), "xrGetD3D11GraphicsRequirementsKHR") ||
            !gpu_.create(requirements.adapterLuid)) return false;
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = gpu_.device.Get();
        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
        sessionInfo.next = &binding;
        sessionInfo.systemId = system_;
        if (!check(xrCreateSession(instance_, &sessionInfo, &session_), "xrCreateSession")) return false;

        if (!reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {0, 0, 0}}, &local_) ||
            !reference_space(XR_REFERENCE_SPACE_TYPE_VIEW, {{0, 0, 0, 1}, {0, 0, 0}}, &view_)) return false;
        base_ = local_;  // until recentered on the first tracked pose
        create_actions();
        std::printf("prism_vr: OpenXR session created\n");
        return true;
    }

    // One pass: events, and a frame while the session runs. False once it's over.
    bool tick()
    {
        poll_events();
        if (exiting_) return false;
        if (g_quit && running_ && !exitRequested_) {
            xrRequestExitSession(session_);
            exitRequested_ = true;
        }
        if (!running_) {
            Sleep(50);
            return !g_quit || exitRequested_;
        }
        frame();
        return true;
    }

    void recenter() { recenterRequested_ = true; }
    void change_latency(double ms) { set_latency(latencyMs_ + ms); }
    void set_latency(double ms)
    {
        latencyMs_ = std::max(0.0, ms);
        std::printf("prism_vr: latency %.0f ms\n", latencyMs_);
    }

    // Once a second, on the console.
    void report(double seconds)
    {
        std::printf("prism_vr: %s, %s, %4.1f frames a second from Horizon, %4.1f submitted, latency %.0f ms\n",
                    running_ ? (focused_ ? "focused" : "running") : "waiting for the session",
                    tracking_.connected() ? "tracking" : "no tracking service", emulatorFrames_ / seconds,
                    submitted_ / seconds, latencyMs_);
        emulatorFrames_ = submitted_ = 0;
    }

private:
    bool reference_space(XrReferenceSpaceType type, XrPosef pose, XrSpace* space)
    {
        XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        info.referenceSpaceType = type;
        info.poseInReferenceSpace = pose;
        return check(xrCreateReferenceSpace(session_, &info, space), "xrCreateReferenceSpace");
    }

    XrPath path(const char* text)
    {
        XrPath path = XR_NULL_PATH;
        check(xrStringToPath(instance_, text, &path), text);
        return path;
    }

    XrAction action(XrActionType type, const char* name)
    {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        info.actionType = type;
        std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(info.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        info.countSubactionPaths = 2;
        info.subactionPaths = hands_;
        XrAction action = XR_NULL_HANDLE;
        check(xrCreateAction(actionSet_, &info, &action), name);
        return action;
    }

    // Bindings for a profile, from its thumbstick and components per input (empty: the profile hasn't
    // it), both hands.
    void suggest(const char* profile, const char* stick, const std::array<const char*, Inputs>& left,
                 const std::array<const char*, Inputs>& right)
    {
        std::vector<XrActionSuggestedBinding> bindings;
        for (int hand = 0; hand < 2; ++hand) {
            const std::string prefix = hand ? "/user/hand/right/input/" : "/user/hand/left/input/";
            bindings.push_back({aim_, path((prefix + "aim/pose").c_str())});
            bindings.push_back({stick_, path((prefix + stick).c_str())});
            bindings.push_back({haptic_, path(hand ? "/user/hand/right/output/haptic" : "/user/hand/left/output/haptic")});
            const auto& components = hand ? right : left;
            for (int i = 0; i < Inputs; ++i)
                if (*components[i]) bindings.push_back({inputs_[i], path((prefix + components[i]).c_str())});
        }
        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = path(profile);
        suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();
        check(xrSuggestInteractionProfileBindings(instance_, &suggested), profile);
    }

    void create_actions()
    {
        hands_[0] = path("/user/hand/left");
        hands_[1] = path("/user/hand/right");
        XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strncpy(setInfo.actionSetName, "horizon", XR_MAX_ACTION_SET_NAME_SIZE - 1);
        std::strncpy(setInfo.localizedActionSetName, "Horizon", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
        if (!check(xrCreateActionSet(instance_, &setInfo, &actionSet_), "xrCreateActionSet")) return;
        aim_ = action(XR_ACTION_TYPE_POSE_INPUT, "aim");
        stick_ = action(XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick");
        haptic_ = action(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic");
        const char* names[Inputs] = {"trigger", "squeeze", "primary", "secondary", "menu", "stick_click",
                                     "primary_touch", "secondary_touch", "trigger_touch"};
        for (int i = 0; i < Inputs; ++i)
            inputs_[i] = action(i <= Squeeze ? XR_ACTION_TYPE_FLOAT_INPUT : XR_ACTION_TYPE_BOOLEAN_INPUT, names[i]);

        suggest("/interaction_profiles/oculus/touch_controller", "thumbstick",
                {"trigger/value", "squeeze/value", "x/click", "y/click", "menu/click", "thumbstick/click", "x/touch", "y/touch", "trigger/touch"},
                {"trigger/value", "squeeze/value", "a/click", "b/click", "", "thumbstick/click", "a/touch", "b/touch", "trigger/touch"});
        suggest("/interaction_profiles/valve/index_controller", "thumbstick",
                {"trigger/value", "squeeze/value", "a/click", "b/click", "", "thumbstick/click", "a/touch", "b/touch", "trigger/touch"},
                {"trigger/value", "squeeze/value", "a/click", "b/click", "", "thumbstick/click", "a/touch", "b/touch", "trigger/touch"});
        suggest("/interaction_profiles/htc/vive_controller", "trackpad",
                {"trigger/value", "squeeze/click", "trackpad/click", "", "menu/click", "", "", "", ""},
                {"trigger/value", "squeeze/click", "trackpad/click", "", "menu/click", "", "", "", ""});

        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &actionSet_;
        if (!check(xrAttachSessionActionSets(session_, &attach), "xrAttachSessionActionSets")) return;
        for (int hand = 0; hand < 2; ++hand) {
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.action = aim_;
            info.subactionPath = hands_[hand];
            info.poseInActionSpace.orientation.w = 1;
            check(xrCreateActionSpace(session_, &info, &aimSpaces_[hand]), "xrCreateActionSpace");
        }
        actionsReady_ = aimSpaces_[0] && aimSpaces_[1];
    }

    void poll_events()
    {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto state = reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state;
                focused_ = state == XR_SESSION_STATE_FOCUSED;
                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    running_ = check(xrBeginSession(session_, &begin), "xrBeginSession");
                    if (running_) std::printf("prism_vr: session running\n");
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session_);
                    running_ = false;
                } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                    running_ = false;
                    exiting_ = true;
                }
            } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                exiting_ = true;
            } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
                // The runtime recentered: LOCAL moved, and the origin made in it with it.
                const auto& change = reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(event);
                if (change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) recenterRequested_ = true;
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    // The origin where the headset is at time, facing ahead (its heading alone), made in LOCAL; poses
    // are sent relative to it.
    bool set_origin(XrTime time)
    {
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        XrSpace origin = XR_NULL_HANDLE;
        if (!XR_SUCCEEDED(xrLocateSpace(view_, local_, time, &head)) || !tracked(head.locationFlags) ||
            !reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL, heading(head.pose), &origin)) return false;
        if (base_ != local_) xrDestroySpace(base_);
        base_ = origin;
        history_.clear();
        haveFrame_ = false;  // its pose was in the old origin's terms
        std::printf("prism_vr: recentered\n");
        return true;
    }

    // The buttons, analog values and thumbstick of a hand, as tracking_prism's input line: the
    // controller's when its actions are synced, with the Meta and menu buttons held from elsewhere
    // (the keyboard).
    std::string input_line(int hand, bool synced, bool metaHeld, bool menuHeld)
    {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.subactionPath = hands_[hand];
        float values[2] = {0, 0};
        XrVector2f stick{0, 0};
        bool down[Inputs]{};
        if (synced) {
            info.action = stick_;
            XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
            if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &info, &state)) && state.isActive) stick = state.currentState;
        }
        for (int i = 0; synced && i < Inputs; ++i) {
            info.action = inputs_[i];
            if (i <= Squeeze) {
                XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
                if (XR_SUCCEEDED(xrGetActionStateFloat(session_, &info, &state)) && state.isActive) values[i] = state.currentState;
            } else {
                XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
                down[i] = XR_SUCCEEDED(xrGetActionStateBoolean(session_, &info, &state)) && state.isActive && state.currentState;
            }
        }
        unsigned buttons = (values[Trigger] > kPressed ? kTrigger : 0) | (values[Squeeze] > kPressed ? kGrip : 0) |
                           (down[Primary] ? kButtonA : 0) | (down[Secondary] ? kButtonB : 0) | (down[StickClick] ? kStick : 0) |
                           (down[PrimaryTouch] ? kButtonATouch : 0) | (down[SecondaryTouch] ? kButtonBTouch : 0) |
                           (down[TriggerTouch] || values[Trigger] > 0.05f ? kTriggerTouch : 0);
        if ((down[Menu] && !metaFromMenu_) || (hand == 0 && menuHeld)) buttons |= kMenu;
        if (hand == 1 && metaHeld) buttons |= kMeta;
        char line[96];
        std::snprintf(line, sizeof(line), "input %c 0x%x %.2f %.2f %.3f %.3f\n", hand ? 'r' : 'l', buttons, values[Trigger],
                      values[Squeeze], stick.x, stick.y);
        return line;
    }

    bool menu_held()
    {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = inputs_[Menu];
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        for (XrPath hand : hands_) {
            info.subactionPath = hand;
            if (XR_SUCCEEDED(xrGetActionStateBoolean(session_, &info, &state)) && state.isActive && state.currentState) return true;
        }
        return false;
    }

    // The controller's aim pose turned back to the pose Horizon's runtime aims from (see kAimTurn).
    static XrPosef published(int hand, const XrPosef& aim)
    {
        const XrQuaternionf orientation = multiply(aim.orientation, yaw_rotation(hand ? -kAimTurn : kAimTurn));
        const XrVector3f offset = rotate(orientation, {hand ? -kAimOffset : kAimOffset, 0, 0});
        return {orientation, {aim.position.x - offset.x, aim.position.y - offset.y, aim.position.z - offset.z}};
    }

    // Sends the headset's and controllers' poses (at the frame's display time) and buttons to Prism.
    void send_tracking(XrTime time)
    {
        const bool recenterKey = GetAsyncKeyState(kRecenterKey) & 0x8000;
        if (recenterKey && !recenterKeyHeld_) recenterRequested_ = true;
        recenterKeyHeld_ = recenterKey;
        if (!originSet_ || recenterRequested_) {
            if (!set_origin(time)) return;
            originSet_ = true;
            recenterRequested_ = false;
        }
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        if (!XR_SUCCEEDED(xrLocateSpace(view_, base_, time, &head)) || !tracked(head.locationFlags)) return;
        std::string lines = pose_line("", head.pose);
        bool synced = false;
        if (actionsReady_) {
            XrActiveActionSet active{actionSet_, XR_NULL_PATH};
            XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
            sync.countActiveActionSets = 1;
            sync.activeActionSets = &active;
            synced = xrSyncActions(session_, &sync) == XR_SUCCESS;  // not while the PC runtime's dashboard has focus
        }
        // The keyboard's buttons, whichever window has focus: they work with the headset on.
        const bool metaKey = (GetAsyncKeyState(kMetaKey) & 0x8000) || g_metaHeld;
        const bool menuKey = (GetAsyncKeyState(kMenuKey) & 0x8000) || g_menuHeld;
        const bool meta = metaKey || (synced && metaFromMenu_ && menu_held());
        for (int hand = 0; hand < 2; ++hand) {
            XrSpaceLocation aim{XR_TYPE_SPACE_LOCATION};
            if (synced && XR_SUCCEEDED(xrLocateSpace(aimSpaces_[hand], base_, time, &aim)) && tracked(aim.locationFlags))
                lines += pose_line(hand ? "hand r " : "hand l ", published(hand, aim.pose));
            tracking_.add_input(lines, hand, input_line(hand, synced, meta, menuKey));
        }
        tracking_.send(lines);
        history_.add(Clock::now(), head.pose);
        haptics();
    }

    // The controllers' vibration, as the tracking service sends it: each amplitude plays until the
    // next, renewed every half second for a second more; a pulse plays once, for its length.
    void haptics()
    {
        for (std::string line; tracking_.receive(&line);) {
            char hand = 0;
            float amplitude = 0, seconds = 0;
            const int fields = std::sscanf(line.c_str(), "haptic %c %f %f", &hand, &amplitude, &seconds);
            if (fields < 2 || (hand != 'l' && hand != 'r')) continue;
            const int h = hand == 'r';
            amplitude = std::clamp(amplitude, 0.0f, 1.0f);
            if (fields == 2) {
                vibration_[h] = amplitude;
            } else if (actionsReady_ && amplitude > 0) {
                XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
                info.action = haptic_;
                info.subactionPath = hands_[h];
                XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
                vibration.duration = static_cast<XrDuration>(std::max(seconds, 0.01f) * 1e9f);
                vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
                vibration.amplitude = amplitude;
                xrApplyHapticFeedback(session_, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
                vibration_[h] = playing_[h] = 0;  // it replaced what played
            }
        }
        if (!actionsReady_) return;
        const auto now = Clock::now();
        for (int hand = 0; hand < 2; ++hand) {
            const float amplitude = tracking_.connected() ? vibration_[hand] : 0;
            if (amplitude == playing_[hand] && (amplitude == 0 || now - played_[hand] < std::chrono::milliseconds(500))) continue;
            XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
            info.action = haptic_;
            info.subactionPath = hands_[hand];
            if (amplitude > 0) {
                XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
                vibration.duration = 1000000000;  // 1 s
                vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
                vibration.amplitude = amplitude;
                xrApplyHapticFeedback(session_, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
            } else {
                xrStopHapticFeedback(session_, &info);
            }
            playing_[hand] = amplitude;
            played_[hand] = now;
        }
    }

    bool create_swapchain(uint32_t width, uint32_t height)
    {
        if (swapchain_) xrDestroySwapchain(swapchain_);
        swapchain_ = XR_NULL_HANDLE;
        targets_.clear();
        uint32_t count = 0;
        if (!check(xrEnumerateSwapchainFormats(session_, 0, &count, nullptr), "xrEnumerateSwapchainFormats") || !count) return false;
        std::vector<int64_t> formats(count);
        xrEnumerateSwapchainFormats(session_, count, &count, formats.data());
        // sRGB first: the frame's bytes are sRGB-encoded.
        int64_t format = 0;
        for (int64_t preferred : {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
                                  DXGI_FORMAT_R8G8B8A8_UNORM}) {
            if (std::find(formats.begin(), formats.end(), preferred) != formats.end()) {
                format = preferred;
                break;
            }
        }
        if (!format) {
            std::fprintf(stderr, "prism_vr: the runtime offers no 8-bit RGBA swapchain\n");
            return false;
        }
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = format;
        info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
        info.width = width;
        info.height = height;
        if (!check(xrCreateSwapchain(session_, &info, &swapchain_), "xrCreateSwapchain")) return false;
        xrEnumerateSwapchainImages(swapchain_, 0, &count, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(swapchain_, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        for (const auto& image : images) {
            D3D11_RENDER_TARGET_VIEW_DESC desc{static_cast<DXGI_FORMAT>(format), D3D11_RTV_DIMENSION_TEXTURE2D};
            ComPtr<ID3D11RenderTargetView> target;
            if (FAILED(gpu_.device->CreateRenderTargetView(image.texture, &desc, &target))) return false;
            targets_.push_back(target);
        }
        swapchainWidth_ = width;
        swapchainHeight_ = height;
        std::printf("prism_vr: swapchain %ux%u, format %lld, %u images\n", width, height, static_cast<long long>(format), count);
        return true;
    }

    // When the emulator shared a frame, from its time: on Clock, now if the time looks wrong.
    static Clock::time_point shared(uint64_t unixMicroseconds)
    {
        const auto now = Clock::now();
        const int64_t unixNow = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count();
        const int64_t age = unixNow - static_cast<int64_t>(unixMicroseconds);
        return age >= 0 && age < 1000000 ? now - std::chrono::microseconds(age) : now;
    }

    // A new frame from the emulator into the swapchain, with the pose it was rendered from.
    void take_frame()
    {
        const VideoInfo* info = frames_.info();
        if (!info) return;
        const auto* live = const_cast<const volatile VideoInfo*>(info);  // the emulator writes it as we read
        const VideoInfo current{live->width, live->height, live->fps, live->frame, live->time};
        if (!current.width || !current.height || (haveFrame_ && current.frame == frameNumber_)) return;
        if (current.frame != frameNumber_) ++emulatorFrames_;
        frameNumber_ = current.frame;
        XrPosef pose;
        const auto rendered = shared(current.time) - std::chrono::microseconds(static_cast<int64_t>(latencyMs_ * 1000));
        if (!history_.at(rendered, &pose)) return;
        if ((current.width != swapchainWidth_ || current.height != swapchainHeight_) && !create_swapchain(current.width, current.height))
            return;
        if (!gpu_.upload(current, frames_.pixels())) return;
        uint32_t index = 0;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        if (!check(xrAcquireSwapchainImage(swapchain_, nullptr, &index), "xrAcquireSwapchainImage")) return;
        if (check(xrWaitSwapchainImage(swapchain_, &wait), "xrWaitSwapchainImage")) gpu_.draw(targets_[index].Get());
        xrReleaseSwapchainImage(swapchain_, nullptr);
        framePose_ = pose;
        haveFrame_ = true;
    }

    void frame()
    {
        XrFrameState state{XR_TYPE_FRAME_STATE};
        if (!check(xrWaitFrame(session_, nullptr, &state), "xrWaitFrame") || !check(xrBeginFrame(session_, nullptr), "xrBeginFrame"))
            return;
        send_tracking(state.predictedDisplayTime);
        take_frame();

        // The frame as a projection layer: each half one view, from Horizon's eye as it rendered it.
        XrCompositionLayerProjectionView views[2]{};
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
        const bool show = state.shouldRender && haveFrame_;
        if (show) {
            const int32_t half = static_cast<int32_t>(swapchainWidth_ / 2);
            for (int i = 0; i < 2; ++i) {
                const XrVector3f eye = rotate(framePose_.orientation, eyes_[i]);
                views[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                views[i].pose = {framePose_.orientation,
                                 {framePose_.position.x + eye.x, framePose_.position.y + eye.y, framePose_.position.z + eye.z}};
                views[i].fov = fov_[i];
                views[i].subImage.swapchain = swapchain_;
                views[i].subImage.imageRect = {{i * half, 0}, {half, static_cast<int32_t>(swapchainHeight_)}};
            }
            layer.space = base_;
            layer.viewCount = 2;
            layer.views = views;
        }
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = state.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = show ? 1 : 0;
        end.layers = layers;
        if (check(xrEndFrame(session_, &end), "xrEndFrame") && show) ++submitted_;
    }

    Gpu& gpu_;
    Tracking& tracking_;
    Frames& frames_;
    double latencyMs_;
    bool metaFromMenu_;
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace local_ = XR_NULL_HANDLE, view_ = XR_NULL_HANDLE, base_ = XR_NULL_HANDLE;
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction aim_ = XR_NULL_HANDLE, stick_ = XR_NULL_HANDLE, haptic_ = XR_NULL_HANDLE;
    float vibration_[2]{}, playing_[2]{};  // each hand's amplitude, as sent and as played
    Clock::time_point played_[2]{};
    XrAction inputs_[Inputs]{};
    XrPath hands_[2]{};
    XrSpace aimSpaces_[2]{};
    bool actionsReady_ = false;
    bool running_ = false, focused_ = false, exiting_ = false, exitRequested_ = false;
    bool originSet_ = false, recenterRequested_ = false, recenterKeyHeld_ = false;
    XrVector3f eyes_[2]{};  // Horizon's eyes, from the head's center
    XrFovf fov_[2]{};
    PoseHistory history_;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    std::vector<ComPtr<ID3D11RenderTargetView>> targets_;
    uint32_t swapchainWidth_ = 0, swapchainHeight_ = 0;
    uint32_t frameNumber_ = 0;
    bool haveFrame_ = false;
    XrPosef framePose_{{0, 0, 0, 1}, {0, 0, 0}};
    uint32_t emulatorFrames_ = 0, submitted_ = 0;
};

BOOL WINAPI on_console(DWORD)
{
    g_quit = true;
    return TRUE;
}

// --control's reader: stdin's lines, queued for the main loop (the buttons are set at once).
void read_commands()
{
    for (std::string line; std::getline(std::cin, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line == "meta down" || line == "meta up") { g_metaHeld = line == "meta down"; continue; }
        if (line == "menu down" || line == "menu up") { g_menuHeld = line == "menu down"; continue; }
        std::lock_guard<std::mutex> lock(g_commandsLock);
        g_commands.push_back(line);
    }
    g_quit = true;  // the window that started it is gone
}

// The commands read since the last look: recenter, latency MS, quit.
void run_commands(Vr& vr)
{
    std::deque<std::string> commands;
    {
        std::lock_guard<std::mutex> lock(g_commandsLock);
        commands.swap(g_commands);
    }
    for (const std::string& command : commands) {
        double ms = 0;
        if (command == "recenter") vr.recenter();
        else if (command == "quit") g_quit = true;
        else if (std::sscanf(command.c_str(), "latency %lf", &ms) == 1) vr.set_latency(ms);
        else if (!command.empty()) std::printf("prism_vr: unknown command \"%s\"\n", command.c_str());
    }
}

}  // namespace

int main(int argc, char** argv)
{
    int emulatorPort = 5590, trackingPort = 7340;
    double latencyMs = 45, ipd = 0.063;
    bool metaFromMenu = true, control = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--menu-button-is-menu") { metaFromMenu = false; continue; }
        if (arg == "--control") { control = true; continue; }
        if (i + 1 == argc) break;  // the rest take a value
        if (arg == "--emulator-port") emulatorPort = std::atoi(argv[++i]);
        else if (arg == "--tracking-port") trackingPort = std::atoi(argv[++i]);
        else if (arg == "--latency") latencyMs = std::atof(argv[++i]);
        else if (arg == "--ipd") ipd = std::atof(argv[++i]) / 1000;  // mm
    }
    // Each line as it's printed, also into a pipe (the window's log), not when a buffer fills.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (control) std::thread(read_commands).detach();  // from now on: closing stdin quits while waiting for a headset too
    SetConsoleCtrlHandler(on_console, TRUE);
    timeBeginPeriod(1);
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    if (!load_loader()) return 1;

    Gpu gpu;
    Tracking tracking(trackingPort);
    Frames frames(emulatorPort);
    Vr vr(gpu, tracking, frames, latencyMs, ipd, metaFromMenu);
    if (!vr.create()) return 1;
    std::printf("prism_vr: R recenters, + and - change the latency, Q quits; in any window, F7 is the Meta button "
                "(Universal Menu), F8 the menu button, F9 recenters\n");

    auto reported = Clock::now();
    while (vr.tick()) {
        run_commands(vr);
        while (_kbhit()) {
            switch (_getch()) {
            case 'r': case 'R': vr.recenter(); break;
            case '+': case '=': vr.change_latency(5); break;
            case '-': case '_': vr.change_latency(-5); break;
            case 'q': case 'Q': case 27: g_quit = true; break;
            }
        }
        const auto now = Clock::now();
        if (const double seconds = std::chrono::duration<double>(now - reported).count(); seconds >= 1) {
            vr.report(seconds);
            reported = now;
        }
    }
    return 0;
}
