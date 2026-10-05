// Prism's viewer: Horizon in a desktop window, driven with the mouse and keyboard.
//
// The window shows one eye of what the compositor puts on the emulator's display (both eyes side
// by side), from the frames the emulator shares (-share-vid, which tools/emulator.py turns on): a
// VideoInfo, then BGRA pixels, in the mapping SHM_videmulator<port>. A thread watches the frame
// number; each new frame's eye is uploaded to a D3D11 texture and drawn scaled by the GPU into a
// flip-model swap chain, drawn only when a back buffer is free (its waitable object), so a frame
// is shown at the next refresh after it arrives.
//
// The right controller points where the mouse is: its ray goes through the pixel under the mouse,
// so hovering and clicking work as they look. The left button pulls its trigger. Drag with the
// right button to look around; W/S move forward and back, A/D sideways, R/F up and down (Shift:
// faster), Space faces ahead again and Home also returns to the origin. Tab presses the Meta
// button (the Universal Menu), E and Q press A and B, G (or the middle button) squeezes the grip.
// Poses and buttons go to Prism's tracking service (native/tracking_prism) as tools/head.py sends
// them, from a thread of their own at 90 Hz.
//
// F1 stats overlay, F2 screenshot, F3 both eyes, F11 fullscreen (Esc leaves it).
//
// Built and started by tools/viewer.py. The window, swap chain, overlay and Android stats follow
// Refract's viewer.
#define _USE_MATH_DEFINES
#include "android_stats.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <mmsystem.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

// --- Where things are -------------------------------------------------------------------------

constexpr float kLook = 0.25f;  // degrees a pixel of right-button drag
constexpr float kSpeed = 1.0f;  // meters a second; Shift: 4x (head.py's SPEED)
constexpr int kTrackingHz = 90;
// Where the right controller is, from the head: just below the eyes, so its ray starts close to
// where the view's does (and the controller itself is too close to be drawn).
constexpr double kHeld[3] = {0.0, -0.06, -0.02};
// Meters ahead: the ray goes through the mouse's pixel this far in front, where VrShell places
// panels; nearer or farther, it misses by a few pixels.
constexpr double kAimDepth = 1.08;
// What each eye's half of the display shows, as tangents of its edges' angles (left, right, up,
// down), from the head's center: the compositor draws the display's halves without the eyes'
// offsets. Fitted to where the pointer's dot lands, within a pixel or two: the middle of what apps
// render (their FOV reaches -1.1504..1.0 across), undistorted.
constexpr double kViews[2][4] = {{-0.7128, 0.6559, 0.7357, -0.8040}, {-0.6559, 0.7128, 0.7357, -0.8040}};
// The runtime's aim pose is the published pose turned this many degrees left (xrprobe: q 0 .0436
// 0 .999) and moved this far (xrprobe: p -.009 0 0); the viewer publishes it turned back.
constexpr double kAimTurn = 5.0;
constexpr double kAimOffset[3] = {-0.009, 0.0, 0.0};
// tracking_prism.c's BUTTON_*: what each key presses (and touches); head.py's KEYS.
constexpr unsigned kTrigger = 0x1 | 0x40;
constexpr unsigned kButtonA = 0x100 | 0x200, kButtonB = 0x400 | 0x800, kGrip = 0x80, kMeta = 0x2;

// The emulator's VideoInfo, ahead of the pixels.
struct VideoInfo {
    uint32_t width, height, fps, frame;
    uint64_t time;
};
static_assert(sizeof(VideoInfo) == 24);

// --- Poses ------------------------------------------------------------------------------------

struct Quat { double x, y, z, w; };
struct Vec { double x, y, z; };

// Turned by yaw about Y (left is positive), then pitched about its own X (up is positive), degrees.
Quat quaternion(double yaw, double pitch)
{
    const double y = yaw * M_PI / 360, p = pitch * M_PI / 360;
    const double cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p);
    return {sp * cy, sy * cp, -sy * sp, cy * cp};
}

Quat multiply(Quat a, Quat b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Vec rotate(Quat q, Vec v)
{
    const Vec t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + q.y * t.z - q.z * t.y, v.y + q.w * t.y + q.z * t.x - q.x * t.z,
            v.z + q.w * t.z + q.x * t.y - q.y * t.x};
}

// What the mouse and keys hold; the window thread writes it, the tracking thread reads it.
struct Input {
    std::mutex mutex;
    double yaw = 0, pitch = 0;
    Vec position{};
    double u = 0.5, v = 0.5;  // where the mouse is, across the eye it's over
    int eye = 0;
    bool keys[256]{};
    bool trigger = false, middle = false;
    std::atomic<bool> connected{false};
};
Input g_input;

// The right controller's place and orientation: its ray through the mouse's pixel.
void aim(const Input& in, Quat head, Vec& at, Quat& orientation)
{
    const double* view = kViews[in.eye];
    const Vec ray{view[0] + in.u * (view[1] - view[0]), view[2] + in.v * (view[3] - view[2]), -1.0};
    const Vec held = rotate(head, {kHeld[0], kHeld[1], kHeld[2]});
    const Vec target = rotate(head, {ray.x * kAimDepth, ray.y * kAimDepth, ray.z * kAimDepth});
    const Vec d{target.x - held.x, target.y - held.y, target.z - held.z};
    const double yaw = std::atan2(-d.x, -d.z) * 180 / M_PI;
    const double pitch = std::atan2(d.y, std::hypot(d.x, d.z)) * 180 / M_PI;
    orientation = multiply(quaternion(yaw, pitch), quaternion(-kAimTurn, 0));
    const Vec offset = rotate(orientation, {kAimOffset[0], kAimOffset[1], kAimOffset[2]});
    at = {in.position.x + held.x - offset.x, in.position.y + held.y - offset.y, in.position.z + held.z - offset.z};
}

// Sends the head, the right controller and its buttons to the tracking service (head.py's lines).
void tracking_thread(int port)
{
    timeBeginPeriod(1);
    SOCKET sock = INVALID_SOCKET;
    std::string sentInput;
    auto last = Clock::now(), retry = Clock::time_point{};
    for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(1000000 / kTrackingHz));
        const auto now = Clock::now();
        const double dt = std::min(0.1, std::chrono::duration<double>(now - last).count());
        last = now;
        char lines[512];
        std::string input;
        {
            std::lock_guard lock(g_input.mutex);
            auto& in = g_input;
            const double step = kSpeed * (in.keys[VK_SHIFT] ? 4 : 1) * dt, yaw = in.yaw * M_PI / 180;
            const struct { int key; double x, y, z; } moves[] = {
                {'W', 0, 0, -1}, {'S', 0, 0, 1}, {'A', -1, 0, 0}, {'D', 1, 0, 0}, {'R', 0, 1, 0}, {'F', 0, -1, 0}};
            for (const auto& m : moves) {
                if (!in.keys[m.key]) continue;
                // Along the floor, in the direction the head faces.
                in.position.x += step * (m.x * std::cos(yaw) + m.z * std::sin(yaw));
                in.position.y += step * m.y;
                in.position.z += step * (-m.x * std::sin(yaw) + m.z * std::cos(yaw));
            }
            const Quat head = quaternion(in.yaw, in.pitch);
            Vec at;
            Quat hand;
            aim(in, head, at, hand);
            std::snprintf(lines, sizeof(lines), "%.6f %.6f %.6f %.6f %.6f %.6f %.6f\nhand r %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",
                          in.position.x, in.position.y, in.position.z, head.x, head.y, head.z, head.w,
                          at.x, at.y, at.z, hand.x, hand.y, hand.z, hand.w);
            const unsigned buttons = (in.trigger ? kTrigger : 0) | (in.keys['E'] ? kButtonA : 0) | (in.keys['Q'] ? kButtonB : 0) |
                                     (in.keys['G'] || in.middle ? kGrip : 0) | (in.keys[VK_TAB] ? kMeta : 0);
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "input r 0x%x %.2f %.2f\n", buttons, in.trigger ? 1.0 : 0.0,
                          in.keys['G'] || in.middle ? 1.0 : 0.0);
            input = buffer;
        }
        if (sock == INVALID_SOCKET) {
            if (now < retry) continue;
            retry = now + std::chrono::seconds(1);
            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<u_short>(port));
            inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
            const BOOL noDelay = TRUE;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
            if (connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
                closesocket(sock);
                sock = INVALID_SOCKET;
                g_input.connected = false;
                continue;
            }
            sentInput.clear();
        }
        std::string message = lines;
        if (input != sentInput) message += input;
        if (send(sock, message.data(), static_cast<int>(message.size()), 0) != static_cast<int>(message.size())) {
            closesocket(sock);
            sock = INVALID_SOCKET;
            g_input.connected = false;
            continue;
        }
        sentInput = input;
        g_input.connected = true;
    }
}

// --- Frames -----------------------------------------------------------------------------------

struct Frames {
    std::atomic<const uint8_t*> base{nullptr};
    HANDLE arrived = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<uint32_t> count{0};  // new frames since the stats last looked
};
Frames g_frames;

// Opens the emulator's frames (waiting for the emulator if need be) and signals each new one.
void frames_thread(int emulatorPort)
{
    timeBeginPeriod(1);
    const std::wstring name = L"SHM_videmulator" + std::to_wstring(emulatorPort);
    uint32_t last = 0;
    for (;;) {
        const uint8_t* base = g_frames.base;
        if (!base) {
            HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
            if (mapping) base = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
            if (!base) {
                if (mapping) CloseHandle(mapping);
                Sleep(500);
                continue;
            }
            g_frames.base = base;  // kept mapped for good: a restarted emulator shares the same mapping
        }
        const auto* info = reinterpret_cast<const volatile VideoInfo*>(base);
        const uint32_t frame = info->frame;
        if (frame != last && info->width && info->height) {
            last = frame;
            ++g_frames.count;
            SetEvent(g_frames.arrived);
        }
        Sleep(1);
    }
}

// --- The window -------------------------------------------------------------------------------

const char kShader[] = R"(
Texture2D image : register(t0);
SamplerState linearSampler : register(s0);
cbuffer Params : register(b0) { float2 uvOffset; float2 uvScale; };
struct VsOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VsOut vs(uint id : SV_VertexID) {
    VsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
float4 ps(VsOut i) : SV_Target {
    return float4(image.Sample(linearSampler, uvOffset + i.uv * uvScale).rgb, 1);
}
)";

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;

struct App {
    HWND window = nullptr;
    ComPtr<IDXGISwapChain1> swapchain;
    HANDLE bufferFree = nullptr;  // the swap chain's frame latency waitable object
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> params;
    ComPtr<ID3D11Texture2D> texture;  // the emulator's display, of which the eye shown is kept current
    ComPtr<ID3D11ShaderResourceView> view;
    UINT textureWidth = 0, textureHeight = 0;
    uint32_t uploaded = 0;  // the frame number in the texture
    bool hasFrame = false, reupload = false;  // reupload: the part shown changed
    UINT width = 0, height = 0;
    bool resized = true, both = false, closed = false, saveRequested = false;
    int eye = 0;  // the eye shown
    D3D11_VIEWPORT viewport{};  // where the image is in the window (both eyes: the whole pair)
    float uploadMs = 0;
    // Stats overlay (Direct2D text over the swap chain).
    bool overlay = true;
    bool log = false;  // --log: the stats line on stdout once a second
    float sourceFps = 0, shownFps = 0;
    std::array<float, 120> intervals{};  // ms between frames shown, a ring
    size_t intervalNext = 0, intervalCount = 0;
    Clock::time_point lastShown{};
    ComPtr<ID2D1DeviceContext> d2d;
    ComPtr<ID2D1Bitmap1> d2dTarget;
    ComPtr<IDWriteTextFormat> font;
    ComPtr<ID2D1SolidColorBrush> brush;
    AndroidStats* android = nullptr;
    POINT drag{};
    bool dragging = false;
    std::filesystem::path shots = "screenshots";
};
App g_app;

bool compile(const char* entry, const char* profile, ComPtr<ID3DBlob>& blob)
{
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(kShader, std::strlen(kShader), "viewer", nullptr, nullptr, entry, profile, 0, 0, &blob, &errors))) {
        std::fprintf(stderr, "viewer: shader %s failed: %s\n", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }
    return true;
}

bool create_device()
{
    // The emulator renders on the PC's discrete GPU; prefer it (Nvidia, then any hardware adapter).
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> chosen, adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (!chosen && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) chosen = adapter;
        if (desc.VendorId == 0x10de) { chosen = adapter; break; }
        adapter.Reset();
    }
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    return SUCCEEDED(D3D11CreateDevice(chosen.Get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &g_device, nullptr, &g_context));
}

bool create_pipeline()
{
    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!compile("vs", "vs_5_0", vsBlob) || !compile("ps", "ps_5_0", psBlob)) return false;
    if (FAILED(g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_app.vs)) ||
        FAILED(g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_app.ps))) return false;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_device->CreateSamplerState(&sampler, &g_app.sampler))) return false;
    D3D11_BUFFER_DESC buffer{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    return SUCCEEDED(g_device->CreateBuffer(&buffer, nullptr, &g_app.params));
}

bool create_swapchain()
{
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(g_device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // Draw only when a back buffer is free (see the main loop), so a frame never waits behind one
    // the desktop compositor still holds.
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (FAILED(factory->CreateSwapChainForHwnd(g_device.Get(), g_app.window, &desc, nullptr, nullptr, &g_app.swapchain))) return false;
    ComPtr<IDXGISwapChain2> waitable;
    if (FAILED(g_app.swapchain.As(&waitable)) || FAILED(waitable->SetMaximumFrameLatency(1))) return false;
    g_app.bufferFree = waitable->GetFrameLatencyWaitableObject();
    factory->MakeWindowAssociation(g_app.window, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

bool create_overlay()
{
    ComPtr<ID2D1Factory1> factory;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<ID2D1Device> device;
    ComPtr<IDWriteFactory> dwrite;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) ||
        FAILED(g_device.As(&dxgiDevice)) || FAILED(factory->CreateDevice(dxgiDevice.Get(), &device)) ||
        FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_app.d2d))) return false;
    g_app.d2d->SetDpi(96.0f, 96.0f);  // draw in pixels
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()))) ||
        FAILED(dwrite->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &g_app.font))) return false;
    return SUCCEEDED(g_app.d2d->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g_app.brush));
}

void resize_targets()
{
    RECT rect{};
    GetClientRect(g_app.window, &rect);
    const UINT w = std::max<LONG>(1, rect.right), h = std::max<LONG>(1, rect.bottom);
    g_app.target.Reset();
    if (g_app.d2d) g_app.d2d->SetTarget(nullptr);
    g_app.d2dTarget.Reset();
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    g_app.swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    ComPtr<ID3D11Texture2D> back;
    g_app.swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back.Get(), nullptr, &g_app.target);
    ComPtr<IDXGISurface> surface;
    if (g_app.d2d && SUCCEEDED(back.As(&surface))) {
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (SUCCEEDED(g_app.d2d->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &g_app.d2dTarget)))
            g_app.d2d->SetTarget(g_app.d2dTarget.Get());
    }
    g_app.width = w;
    g_app.height = h;
    g_app.resized = false;
}

// Copies the newest frame's visible part (one eye, or both) into the texture; false if there was
// nothing new.
bool upload()
{
    const uint8_t* base = g_frames.base;
    if (!base) return false;
    const auto start = Clock::now();
    const auto* info = reinterpret_cast<const volatile VideoInfo*>(base);
    const UINT width = info->width, height = info->height;
    const uint32_t number = info->frame;
    if (!width || !height || (g_app.hasFrame && number == g_app.uploaded && !g_app.reupload)) return false;
    if (!g_app.texture || g_app.textureWidth != width || g_app.textureHeight != height) {
        g_app.texture.Reset();
        g_app.view.Reset();
        const D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_app.texture)) ||
            FAILED(g_device->CreateShaderResourceView(g_app.texture.Get(), nullptr, &g_app.view))) return false;
        g_app.textureWidth = width;
        g_app.textureHeight = height;
    }
    const UINT half = width / 2, left = g_app.both ? 0 : g_app.eye * half, right = g_app.both ? width : left + half;
    const D3D11_BOX box{left, 0, 0, right, height, 1};
    g_context->UpdateSubresource(g_app.texture.Get(), 0, &box, base + sizeof(VideoInfo) + size_t(left) * 4, width * 4, 0);
    g_app.uploadMs = std::chrono::duration<float, std::milli>(Clock::now() - start).count();
    const bool fresh = !g_app.hasFrame || number != g_app.uploaded;
    g_app.uploaded = number;
    g_app.hasFrame = true;
    g_app.reupload = false;
    return fresh;
}

void save_png()
{
    if (!g_app.texture) return;
    D3D11_TEXTURE2D_DESC desc{};
    g_app.texture->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &staging))) return;
    g_context->CopyResource(staging.Get(), g_app.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
    const UINT half = desc.Width / 2, left = g_app.both ? 0 : g_app.eye * half, width = g_app.both ? desc.Width : half;
    std::vector<uint8_t> pixels(size_t(width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch + size_t(left) * 4;
        auto* out = pixels.data() + size_t(y) * width * 4;
        for (UINT x = 0; x < width; ++x) {  // BGRA, as WIC wants it; opaque
            out[x * 4 + 0] = row[x * 4 + 0];
            out[x * 4 + 1] = row[x * 4 + 1];
            out[x * 4 + 2] = row[x * 4 + 2];
            out[x * 4 + 3] = 255;
        }
    }
    g_context->Unmap(staging.Get(), 0);

    std::filesystem::create_directories(g_app.shots);
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    const auto path = g_app.shots / (std::string("prism-") + stamp + ".png");
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(width, desc.Height)) || FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WritePixels(desc.Height, width * 4, static_cast<UINT>(pixels.size()), pixels.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        std::fprintf(stderr, "viewer: screenshot failed\n");
        return;
    }
    std::printf("saved %s\n", path.string().c_str());
    std::fflush(stdout);
}

// Letterboxed rectangle for an image of the given aspect inside the window.
D3D11_VIEWPORT fit(float w, float h, float aspect)
{
    float vw = w, vh = w / aspect;
    if (vh > h) { vh = h; vw = h * aspect; }
    return {(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1};
}

// Frame-time bar color: within a 60 fps budget green, within 30 fps yellow, else red.
D2D1_COLOR_F frame_color(float ms)
{
    return ms <= 17.5f ? D2D1::ColorF(0.3f, 0.85f, 0.4f) : ms <= 33.4f ? D2D1::ColorF(0.95f, 0.8f, 0.2f) : D2D1::ColorF(0.95f, 0.3f, 0.25f);
}

void draw_overlay()
{
    if (!g_app.overlay || !g_app.d2dTarget) return;
    std::vector<float> frames;  // oldest first
    for (size_t i = 0; i < g_app.intervalCount; ++i)
        frames.push_back(g_app.intervals[(g_app.intervalNext + g_app.intervals.size() - g_app.intervalCount + i) % g_app.intervals.size()]);
    float average = 0, worst = 0;
    for (float f : frames) { average += f; worst = std::max(worst, f); }
    if (!frames.empty()) average /= frames.size();
    const auto android = g_app.android ? g_app.android->snapshot() : AndroidSnapshot{};
    std::wstring text;
    wchar_t line[256];
    swprintf_s(line, L"Horizon  compositor %hs fps   Android CPU %3.0f%% of %d cores\n",
        android.compositor.empty() ? "?" : android.compositor.c_str(), android.cpuPercent, android.cores);
    text += line;
    swprintf_s(line, L"Viewer   %3.0f fps from the emulator, %3.0f shown   %5.1f ms avg  %5.1f worst   upload %.2f ms\n",
        g_app.sourceFps, g_app.shownFps, average, worst, g_app.uploadMs);
    text += line;
    text += L"Busiest ";
    for (size_t i = 0; i < std::min<size_t>(4, android.threads.size()); ++i) {
        swprintf_s(line, L" %hs %.0f%%", android.threads[i].name.c_str(), android.threads[i].percent);
        text += line;
    }
    text += android.valid ? L"\n" : L" (no stats from adb yet)\n";
    double yaw, pitch;
    {
        std::lock_guard lock(g_input.mutex);
        yaw = g_input.yaw;
        pitch = g_input.pitch;
    }
    swprintf_s(line, L"Tracking %ls   yaw %.0f pitch %.0f\n", g_input.connected ? L"connected" : L"no tracking service", yaw, pitch);
    text += line;
    text += L"F1 hide  F2 screenshot  F3 both eyes  F11 fullscreen";

    constexpr float x = 10, y = 10, width = 760, textHeight = 96, graphHeight = 50;
    auto& d2d = *g_app.d2d.Get();
    d2d.BeginDraw();
    g_app.brush->SetColor(D2D1::ColorF(0, 0, 0, 0.6f));
    d2d.FillRectangle(D2D1::RectF(x, y, x + width, y + textHeight + graphHeight + 20), g_app.brush.Get());
    g_app.brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
    d2d.DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), g_app.font.Get(),
        D2D1::RectF(x + 10, y + 8, x + width - 10, y + textHeight), g_app.brush.Get());
    // Frame-time graph, newest on the right; 0-50 ms tall, lines at 60 and 30 fps.
    const float graphTop = y + textHeight + 10, graphBottom = graphTop + graphHeight, scale = graphHeight / 50.0f;
    const float barWidth = (width - 20) / float(g_app.intervals.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        const float left = x + 10 + (g_app.intervals.size() - frames.size() + i) * barWidth;
        g_app.brush->SetColor(frame_color(frames[i]));
        d2d.FillRectangle(D2D1::RectF(left, std::max(graphTop, graphBottom - frames[i] * scale), left + barWidth, graphBottom), g_app.brush.Get());
    }
    g_app.brush->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
    for (float ms : {16.7f, 33.3f})
        d2d.DrawLine(D2D1::Point2F(x + 10, graphBottom - ms * scale), D2D1::Point2F(x + width - 10, graphBottom - ms * scale), g_app.brush.Get());
    d2d.EndDraw();
}

void render()
{
    if (g_app.resized) resize_targets();
    const float background[4] = {0, 0, 0, 1};
    g_context->OMSetRenderTargets(1, g_app.target.GetAddressOf(), nullptr);
    g_context->ClearRenderTargetView(g_app.target.Get(), background);
    if (g_app.hasFrame) {
        if (g_app.saveRequested) save_png();
        const float aspect = float(g_app.both ? g_app.textureWidth : g_app.textureWidth / 2) / float(g_app.textureHeight);
        g_app.viewport = fit(float(g_app.width), float(g_app.height), aspect);
        const float params[4] = {g_app.both ? 0.0f : 0.5f * g_app.eye, 0, g_app.both ? 1.0f : 0.5f, 1};
        g_context->UpdateSubresource(g_app.params.Get(), 0, nullptr, params, 0, 0);
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->IASetInputLayout(nullptr);
        g_context->VSSetShader(g_app.vs.Get(), nullptr, 0);
        g_context->PSSetShader(g_app.ps.Get(), nullptr, 0);
        g_context->PSSetShaderResources(0, 1, g_app.view.GetAddressOf());
        g_context->PSSetSamplers(0, 1, g_app.sampler.GetAddressOf());
        g_context->PSSetConstantBuffers(0, 1, g_app.params.GetAddressOf());
        g_context->RSSetViewports(1, &g_app.viewport);
        g_context->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        g_context->PSSetShaderResources(0, 1, &none);
    }
    g_app.saveRequested = false;
    draw_overlay();
    g_app.swapchain->Present(0, 0);
}

// Where the mouse is, as the eye it's over and the place across that eye's view.
void point(int x, int y)
{
    const auto& vp = g_app.viewport;
    if (vp.Width < 1 || vp.Height < 1) return;
    double u = std::clamp((x - vp.TopLeftX) / vp.Width, 0.0f, 1.0f), v = std::clamp((y - vp.TopLeftY) / vp.Height, 0.0f, 1.0f);
    int eye = g_app.eye;
    if (g_app.both) {
        eye = u >= 0.5 ? 1 : 0;
        u = std::clamp(u * 2 - eye, 0.0, 1.0);
    }
    std::lock_guard lock(g_input.mutex);
    g_input.u = u;
    g_input.v = v;
    g_input.eye = eye;
}

// Borderless fullscreen on the window's monitor; a second call restores the window.
void toggle_fullscreen()
{
    static WINDOWPLACEMENT saved{sizeof(WINDOWPLACEMENT)};
    const LONG style = GetWindowLongW(g_app.window, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetWindowPlacement(g_app.window, &saved) ||
            !GetMonitorInfoW(MonitorFromWindow(g_app.window, MONITOR_DEFAULTTOPRIMARY), &monitor)) return;
        SetWindowLongW(g_app.window, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        const RECT& r = monitor.rcMonitor;
        SetWindowPos(g_app.window, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(g_app.window, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_app.window, &saved);
        SetWindowPos(g_app.window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    switch (message) {
    case WM_SIZE: g_app.resized = true; return 0;
    case WM_CLOSE: g_app.closed = true; DestroyWindow(window); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_KILLFOCUS: {  // keys released elsewhere never come back up here
        std::lock_guard lock(g_input.mutex);
        std::fill(std::begin(g_input.keys), std::end(g_input.keys), false);
        g_input.trigger = g_input.middle = false;
        g_app.dragging = false;
        return 0;
    }
    case WM_RBUTTONDOWN:
        g_app.dragging = true;
        g_app.drag = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        SetCapture(window);
        return 0;
    case WM_RBUTTONUP: g_app.dragging = false; ReleaseCapture(); return 0;
    case WM_MOUSEMOVE: {
        const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (g_app.dragging) {
            const double dx = p.x - g_app.drag.x, dy = p.y - g_app.drag.y;
            g_app.drag = p;
            std::lock_guard lock(g_input.mutex);
            g_input.yaw = std::fmod(g_input.yaw - dx * kLook + 540.0, 360.0) - 180.0;
            g_input.pitch = std::clamp(g_input.pitch - dy * kLook, -89.0, 89.0);
        }
        point(p.x, p.y);
        return 0;
    }
    case WM_LBUTTONDOWN: { std::lock_guard lock(g_input.mutex); g_input.trigger = true; return 0; }
    case WM_LBUTTONUP: { std::lock_guard lock(g_input.mutex); g_input.trigger = false; return 0; }
    case WM_MBUTTONDOWN: { std::lock_guard lock(g_input.mutex); g_input.middle = true; return 0; }
    case WM_MBUTTONUP: { std::lock_guard lock(g_input.mutex); g_input.middle = false; return 0; }
    case WM_KEYDOWN:
    case WM_KEYUP: {
        const bool down = message == WM_KEYDOWN;
        if (down) {
            if (w == VK_F1) {
                g_app.overlay = !g_app.overlay;
                if (g_app.android) g_app.android->enabled = g_app.overlay;
            }
            if (w == VK_F2) g_app.saveRequested = true;
            if (w == VK_F3) g_app.both = !g_app.both, g_app.reupload = true;
            if (w == VK_F11 || (w == VK_ESCAPE && !(GetWindowLongW(window, GWL_STYLE) & WS_OVERLAPPEDWINDOW))) toggle_fullscreen();
            SetEvent(g_frames.arrived);  // redraw with the new setting
        }
        std::lock_guard lock(g_input.mutex);
        if (down && (w == VK_SPACE || w == VK_HOME)) {
            g_input.yaw = g_input.pitch = 0;
            if (w == VK_HOME) g_input.position = {};
        }
        if (w < 256) g_input.keys[w] = down;
        return 0;
    }
    }
    return DefWindowProcW(window, message, w, l);
}

}  // namespace

int main(int argc, char** argv)
{
    int emulatorPort = 5590, trackingPort = 7340;
    std::string adb = "adb", serial = "emulator-5590";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--log") { g_app.log = true; continue; }
        if (i + 1 == argc) break;  // the rest take a value
        if (arg == "--emulator-port") emulatorPort = std::atoi(argv[++i]);
        else if (arg == "--tracking-port") trackingPort = std::atoi(argv[++i]);
        else if (arg == "--eye") g_app.eye = std::string(argv[++i]) == "right" ? 1 : 0;
        else if (arg == "--shots") g_app.shots = argv[++i];
        else if (arg == "--adb") adb = argv[++i];
        else if (arg == "--serial") serial = argv[++i];
        else if (arg == "--stats") g_app.overlay = std::atoi(argv[++i]) != 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    if (!create_device() || !create_pipeline()) { std::fprintf(stderr, "viewer: D3D11 setup failed\n"); return 1; }

    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PrismViewer";
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    RegisterClassW(&wc);
    // An eye's shape (960x1080), 85% of the work area's height.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int height = (work.bottom - work.top) * 85 / 100;
    RECT frame{0, 0, height * 960 / 1080, height};
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    g_app.window = CreateWindowExW(0, wc.lpszClassName, L"Prism", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_app.window || !create_swapchain()) { std::fprintf(stderr, "viewer: window setup failed\n"); return 1; }
    if (!create_overlay()) std::fprintf(stderr, "viewer: overlay setup failed; no stats\n");
    AndroidStats android(adb, serial);
    android.enabled = g_app.overlay;
    android.start();
    g_app.android = &android;
    ShowWindow(g_app.window, SW_SHOWNORMAL);

    std::thread(frames_thread, emulatorPort).detach();
    std::thread(tracking_thread, trackingPort).detach();

    auto statsAt = Clock::now();
    uint32_t presented = 0;
    bool bufferFree = false, pending = true;  // pending: something new to show
    while (!g_app.closed) {
        const HANDLE handles[] = {g_frames.arrived, g_app.bufferFree};
        const DWORD woke = MsgWaitForMultipleObjects(bufferFree ? 1 : 2, handles, FALSE, 100, QS_ALLINPUT);
        if (woke == WAIT_OBJECT_0) pending = true;
        if (woke == WAIT_OBJECT_0 + 1 || (!bufferFree && WaitForSingleObject(g_app.bufferFree, 0) == WAIT_OBJECT_0))
            bufferFree = true;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_app.closed = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_app.closed) break;

        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - statsAt).count();
        if (elapsed >= 1.0) {
            g_app.sourceFps = float(g_frames.count.exchange(0) / elapsed);
            g_app.shownFps = float(presented / elapsed);
            wchar_t title[256];
            swprintf_s(title, L"Prism  |  %.0f fps  |  %ls  |  left: trigger, right-drag: look, WASD R/F: move, Tab: Meta  |  F1 stats",
                g_app.shownFps, !g_frames.base ? L"waiting for the emulator" : g_input.connected ? L"tracking" : L"no tracking service");
            SetWindowTextW(g_app.window, title);
            if (g_app.log) {
                float average = 0, worst = 0;
                for (size_t i = 0; i < g_app.intervalCount; ++i) {
                    const float ms = g_app.intervals[(g_app.intervalNext + g_app.intervals.size() - 1 - i) % g_app.intervals.size()];
                    if (i < size_t(presented)) average += ms, worst = std::max(worst, ms);
                }
                const size_t n = std::min<size_t>(presented, g_app.intervalCount);
                std::printf("viewer: %4.1f fps from the emulator, %4.1f shown, %5.1f ms avg %5.1f worst, upload %.2f ms\n",
                            g_app.sourceFps, g_app.shownFps, n ? average / n : 0.f, worst, g_app.uploadMs);
                std::fflush(stdout);
            }
            presented = 0;
            statsAt = now;
            pending = true;  // the overlay's numbers changed
        }
        if (bufferFree && (pending || g_app.resized || g_app.saveRequested)) {
            const bool fresh = upload();
            render();
            bufferFree = pending = false;
            if (fresh) {
                ++presented;
                if (g_app.lastShown != Clock::time_point{}) {
                    g_app.intervals[g_app.intervalNext] = std::chrono::duration<float, std::milli>(now - g_app.lastShown).count();
                    g_app.intervalNext = (g_app.intervalNext + 1) % g_app.intervals.size();
                    g_app.intervalCount = std::min(g_app.intervalCount + 1, g_app.intervals.size());
                }
                g_app.lastShown = now;
            }
        }
    }
    return 0;
}
