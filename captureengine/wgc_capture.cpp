// Windows Graphics Capture (WGC) implementation
// Supports capturing DirectFlip content that Desktop Duplication cannot handle
// Requires Windows 10 1803+

#include "wgc_capture.h"
#include <avrt.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../common/capture_pipeline_policy.h"
#include "../common/frame_timing_utils.h"
#include "../common/logging.h"
#include "../common/rate_window_utils.h"
#include "../common/thread_power_throttling_compat.h"
#include "../mediaengine/video_format_policy.h"
#include "mediaengine_loader.h"

#ifdef _MSC_VER
#pragma comment(lib, "avrt.lib")
#endif

// Global inflight callback counter - outlives WGCCapture::Impl destruction
// to prevent use-after-free when WinRT thread pool callbacks access Impl members
static std::atomic<int32_t> g_WgcInflightCallbacks{0};

// WinRT/C++WinRT headers for WGC
#include <winrt/base.h>

// Initialize apartment for WinRT
#include <roapi.h>

// Check if actual WGC headers are available
#if __has_include(<winrt/Windows.Graphics.Capture.h>)
#define HAS_WGC 1
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

// Manual definition of IDirect3DDxgiInterfaceAccess since SDK header may be
// missing This interface allows extracting the DXGI interface from a WinRT
// Direct3D device/surface {A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1}
struct IDirect3DDxgiInterfaceAccess : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID riid, void** ppvObject) = 0;
};

// Explicit IID for IDirect3DDxgiInterfaceAccess
static const GUID IID_IDirect3DDxgiInterfaceAccess = {
    0xA9B3D012, 0x3DF2, 0x4EE3, {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}};

// Minimal interop interface declaration used to create GraphicsCaptureItem.
// We avoid the generated raw header here because the MinGW WIDL headers are
// not always compatible with the C++/WinRT headers in the same translation unit.
static const GUID IID_IGraphicsCaptureItemInterop = {
    0x3628e81b, 0x3cac, 0x4c60, {0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56}};

struct IGraphicsCaptureItemInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID riid, void** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID riid, void** result) = 0;
};

// CreateDirect3D11DeviceFromDXGIDevice declaration
extern "C" {
HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice* dxgiDevice, IInspectable** graphicsDevice);
}

#else
#define HAS_WGC 0
#endif

#if HAS_WGC
namespace winrt {
using namespace Windows::Foundation;
using namespace Windows::Graphics::Capture;
using namespace Windows::Graphics::DirectX;
using namespace Windows::Graphics::DirectX::Direct3D11;
}  // namespace winrt

namespace {
enum WgcIngressAdmissionReasonCode : uint32_t {
    kWgcIngressReasonUncapped = 0,
    kWgcIngressReasonLowWater = 1,
    kWgcIngressReasonRecovery = 2,
    kWgcIngressReasonSourceBelowTarget = 3,
    kWgcIngressReasonCredit = 4,
    kWgcIngressReasonHealthy = 5,
    kWgcIngressReasonDecimatedSoftReserve = 6,
    kWgcIngressReasonDecimatedHardReserve = 7,
    kWgcIngressReasonDecimatedCredit = 8,
    kWgcIngressReasonUniformPlayoutSoftReserve = 9,
    kWgcIngressReasonUniformPlayoutCredit = 10,
};

uint32_t WgcIngressReasonCodeFromText(const char* reason) {
    if (!reason) {
        return kWgcIngressReasonUncapped;
    }
    if (strcmp(reason, "low_water") == 0) {
        return kWgcIngressReasonLowWater;
    }
    if (strcmp(reason, "recovery") == 0) {
        return kWgcIngressReasonRecovery;
    }
    if (strcmp(reason, "source_below_cfr_target") == 0) {
        return kWgcIngressReasonSourceBelowTarget;
    }
    if (strcmp(reason, "credit") == 0) {
        return kWgcIngressReasonCredit;
    }
    if (strcmp(reason, "healthy") == 0) {
        return kWgcIngressReasonHealthy;
    }
    if (strcmp(reason, "uniform_playout_soft_reserve") == 0) {
        return kWgcIngressReasonUniformPlayoutSoftReserve;
    }
    if (strcmp(reason, "uniform_playout_credit") == 0) {
        return kWgcIngressReasonUniformPlayoutCredit;
    }
    if (strcmp(reason, "wgc_ingress_decimated_soft_reserve") == 0 || strcmp(reason, "wgc_ingress_decimated") == 0) {
        return kWgcIngressReasonDecimatedSoftReserve;
    }
    if (strcmp(reason, "wgc_ingress_decimated_hard_reserve") == 0) {
        return kWgcIngressReasonDecimatedHardReserve;
    }
    if (strcmp(reason, "wgc_ingress_decimated_credit") == 0) {
        return kWgcIngressReasonDecimatedCredit;
    }
    return kWgcIngressReasonUncapped;
}

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

template <typename T>
void UpdateAtomicMax(std::atomic<T>& value, T sample) {
    auto current = value.load(std::memory_order_relaxed);
    while (sample > current &&
           !value.compare_exchange_weak(current, sample, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

inline void UpdateSmoothedAtomicUs(std::atomic<int64_t>& target, int64_t sampleUs) {
    if (sampleUs < 0) {
        return;
    }

    int64_t current = target.load(std::memory_order_relaxed);
    const int64_t next = current == 0 ? sampleUs : ((current * 7) + sampleUs) / 8;
    target.store(next, std::memory_order_relaxed);
}

const char* DxgiFormatName(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return "R8G8B8A8_UNORM";
        default:
            return "UNKNOWN";
    }
}

struct D3D11ContextStateGuard {
    explicit D3D11ContextStateGuard(ID3D11DeviceContext* context) : context_(context) {
        if (!context_) {
            return;
        }
        context_->AddRef();
        context_->OMGetRenderTargets(1, &rtv_, &dsv_);
        viewportCount_ = 1;
        context_->RSGetViewports(&viewportCount_, &viewport_);
        context_->VSGetShader(&vs_, nullptr, nullptr);
        context_->PSGetShader(&ps_, nullptr, nullptr);
        context_->PSGetShaderResources(0, 1, &srv_);
        context_->PSGetSamplers(0, 1, &sampler_);
        context_->PSGetConstantBuffers(0, 1, &constantBuffer_);
        context_->IAGetPrimitiveTopology(&topology_);
        context_->IAGetInputLayout(&inputLayout_);
    }

    ~D3D11ContextStateGuard() {
        if (!context_) {
            return;
        }
        context_->OMSetRenderTargets(1, &rtv_, dsv_);
        if (viewportCount_ > 0) {
            context_->RSSetViewports(viewportCount_, &viewport_);
        }
        context_->VSSetShader(vs_, nullptr, 0);
        context_->PSSetShader(ps_, nullptr, 0);
        context_->PSSetShaderResources(0, 1, &srv_);
        context_->PSSetSamplers(0, 1, &sampler_);
        context_->PSSetConstantBuffers(0, 1, &constantBuffer_);
        context_->IASetPrimitiveTopology(topology_);
        context_->IASetInputLayout(inputLayout_);

        SafeRelease(inputLayout_);
        SafeRelease(constantBuffer_);
        SafeRelease(sampler_);
        SafeRelease(srv_);
        SafeRelease(ps_);
        SafeRelease(vs_);
        SafeRelease(dsv_);
        SafeRelease(rtv_);
        SafeRelease(context_);
    }

    ID3D11DeviceContext* context_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11DepthStencilView* dsv_ = nullptr;
    UINT viewportCount_ = 0;
    D3D11_VIEWPORT viewport_ = {};
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11Buffer* constantBuffer_ = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11InputLayout* inputLayout_ = nullptr;
};

static const char* WGC_POOL_COPY_SHADER_SRC = R"(
Texture2D texIn : register(t0);
SamplerState sam : register(s0);

cbuffer CopyCB : register(b0)
{
    uint colorTransform;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

struct VS_OUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD;
};

VS_OUT VS_Main(uint id : SV_VertexID) {
    VS_OUT o;
    o.uv  = float2((id == 1) ? 2.0f : 0.0f, (id == 2) ? 2.0f : 0.0f);
    o.pos = float4(o.uv.x * 2.0f - 1.0f, 1.0f - o.uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}

float3 LinearToSRGB(float3 c)
{
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
    return float3(c.r < 0.0031308 ? lo.r : hi.r,
                  c.g < 0.0031308 ? lo.g : hi.g,
                  c.b < 0.0031308 ? lo.b : hi.b);
}

float4 PS_Main(VS_OUT input) : SV_TARGET {
    float4 c = texIn.Sample(sam, input.uv);
    if (colorTransform != 0) {
        c.rgb = LinearToSRGB(saturate(c.rgb));
    }
    return c;
}
)";

void DisableCurrentWgcCallbackThreadPowerThrottling() {
    THREAD_POWER_THROTTLING_STATE throttlingState = {};
    throttlingState.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttlingState.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttlingState.StateMask = 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttlingState, sizeof(throttlingState));
}

void EnsureWgcCallbackThreadQoS() {
    thread_local bool configured = false;
    thread_local HANDLE mmcssHandle = nullptr;
    if (configured) {
        return;
    }
    configured = true;

    DisableCurrentWgcCallbackThreadPowerThrottling();
    DWORD taskIndex = 0;
    mmcssHandle = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
    if (mmcssHandle) {
        AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_HIGH);
        LogInfo("[WGC] Callback thread QoS enabled (tid=%lu, task=Capture)", GetCurrentThreadId());
    } else {
        LogWarn("[WGC] Callback thread QoS setup failed (tid=%lu, err=%lu)", GetCurrentThreadId(), GetLastError());
    }
}

int64_t HundredNanosecondsToQpcTicks(int64_t value100ns, int64_t qpcFreq) {
    if (value100ns <= 0 || qpcFreq <= 0) {
        return 0;
    }

    const int64_t wholeSeconds = value100ns / 10000000ll;
    const int64_t remainder100ns = value100ns % 10000000ll;
    return wholeSeconds * qpcFreq + (remainder100ns * qpcFreq) / 10000000ll;
}

bool IsHdrOutputColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) {
    switch (colorSpace) {
        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
        case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
        case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020:
        case DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020:
        case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:
            return true;
        default:
            return false;
    }
}

static const GUID IID_IGraphicsCaptureSession5Abi = {
    0x67C0EA62, 0x1F85, 0x5061, {0x92, 0x5A, 0x23, 0x9B, 0xE0, 0xAC, 0x09, 0xCB}};

struct IGraphicsCaptureSession5Abi : ::IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_MinUpdateInterval(int64_t* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MinUpdateInterval(int64_t value) = 0;
};

static const GUID IID_IDirect3D11CaptureFrame2Abi = {
    0x37869CFA, 0x2B48, 0x5EBF, {0x9A, 0xFB, 0xDF, 0xFD, 0x80, 0x5D, 0xEF, 0xDB}};

struct IDirect3D11CaptureFrame2Abi : ::IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_DirtyRegions(void** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DirtyRegionMode(int32_t* value) = 0;
};

template <typename T>
bool TryQueryComInterface(const T& object, const GUID& iid, void** result) {
    if (!result) {
        return false;
    }

    *result = nullptr;
    auto* unknown = reinterpret_cast<::IUnknown*>(winrt::get_abi(object));
    if (!unknown) {
        return false;
    }

    return SUCCEEDED(unknown->QueryInterface(iid, result));
}

bool GetWindowClientRectInScreen(HWND hwnd, RECT& rect) {
    RECT clientRect = {};
    if (!GetClientRect(hwnd, &clientRect)) {
        return false;
    }

    POINT topLeft = {clientRect.left, clientRect.top};
    POINT bottomRight = {clientRect.right, clientRect.bottom};
    if (!ClientToScreen(hwnd, &topLeft) || !ClientToScreen(hwnd, &bottomRight)) {
        return false;
    }

    rect.left = topLeft.x;
    rect.top = topLeft.y;
    rect.right = bottomRight.x;
    rect.bottom = bottomRight.y;
    return true;
}

bool RectNearlyMatches(const RECT& lhs, const RECT& rhs, LONG tolerance) {
    auto absDiff = [](LONG a, LONG b) -> LONG { return (a >= b) ? (a - b) : (b - a); };

    return absDiff(lhs.left, rhs.left) <= tolerance && absDiff(lhs.top, rhs.top) <= tolerance &&
           absDiff(lhs.right, rhs.right) <= tolerance && absDiff(lhs.bottom, rhs.bottom) <= tolerance;
}

bool IsFullscreenLikeWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor) {
        return false;
    }

    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfo(monitor, &monitorInfo)) {
        return false;
    }

    RECT windowRect = {};
    RECT clientRect = {};
    const bool haveWindowRect = GetWindowRect(hwnd, &windowRect) != FALSE;
    const bool haveClientRect = GetWindowClientRectInScreen(hwnd, clientRect);
    constexpr LONG kFullscreenTolerancePx = 8;

    if (!haveWindowRect && !haveClientRect) {
        return false;
    }

    const bool windowMatchesMonitor =
        haveWindowRect && RectNearlyMatches(windowRect, monitorInfo.rcMonitor, kFullscreenTolerancePx);
    const bool clientMatchesMonitor =
        haveClientRect && RectNearlyMatches(clientRect, monitorInfo.rcMonitor, kFullscreenTolerancePx);
    return windowMatchesMonitor || clientMatchesMonitor;
}

constexpr int kBorderlessAccessUnknown = 0;
constexpr int kBorderlessAccessAllowed = 1;
constexpr int kBorderlessAccessDenied = 2;
constexpr int kBorderlessAccessUnavailable = 3;

std::once_flag g_BorderlessAccessRequestOnce;
std::atomic<int> g_BorderlessAccessRequestState{kBorderlessAccessUnknown};

int GetBorderlessAccessRequestState() {
    return g_BorderlessAccessRequestState.load(std::memory_order_acquire);
}

bool EnsureBorderlessAccessRequested() {
    std::call_once(g_BorderlessAccessRequestOnce, []() {
        int state = kBorderlessAccessUnavailable;
        try {
            const auto status = winrt::Windows::Graphics::Capture::GraphicsCaptureAccess::RequestAccessAsync(
                                    winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind::Borderless)
                                    .get();
            if (status ==
                winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed) {
                state = kBorderlessAccessAllowed;
                LogInfo("[WGC] Borderless access granted by OS");
            } else {
                state = kBorderlessAccessDenied;
                LogInfo("[WGC] Borderless access denied by OS (status=%d)", static_cast<int>(status));
            }
        } catch (winrt::hresult_error const& e) {
            LogInfo("[WGC] Borderless access request unavailable: 0x%08lX", static_cast<unsigned long>(e.code().value));
        } catch (...) {
            LogInfo("[WGC] Borderless access request unavailable");
        }

        g_BorderlessAccessRequestState.store(state, std::memory_order_release);
    });

    return GetBorderlessAccessRequestState() == kBorderlessAccessAllowed;
}
}  // namespace
#endif

class WGCCapture::Impl {
public:
#if HAS_WGC
    ~Impl() {
        if (item_) {
            try {
                item_.Closed(itemClosedToken_);
            } catch (...) {}
        }
        ReleaseTexturePool();
        SafeRelease(latestFrame_);
        SafeRelease(cachedTexture_);
        SafeRelease(d3dContext_);
        if (usingDedicatedCaptureDevice_) {
            SafeRelease(d3dDevice_);
        } else {
            d3dDevice_ = nullptr;
        }
        if (frameArrivedEvent_) {
            CloseHandle(frameArrivedEvent_);
            frameArrivedEvent_ = NULL;
        }
    }

    std::atomic<bool> alive_{true};
    winrt::GraphicsCaptureItem item_{nullptr};
    winrt::Direct3D11CaptureFramePool framePool_{nullptr};
    winrt::GraphicsCaptureSession session_{nullptr};
    winrt::IDirect3DDevice winrtDevice_{nullptr};

    ID3D11Texture2D* latestFrame_ = nullptr;
    std::deque<WGCCapturedFrame> pendingFrames_;
    static constexpr size_t kMaxPendingFrames = 48;

    // Texture pool for zero-copy pipeline: each frame gets its own texture
    // so the encoder can consume frame N while frame N+1 is being copied. The
    // size is budgeted at capture start; VRR/CFR smoothness needs more than the
    // old fixed 12 slots at high refresh, but must stay bounded by VRAM.
    std::vector<ID3D11Texture2D*> texturePool_;         // Encoder-device textures
    std::vector<ID3D11Texture2D*> captureTexturePool_;  // Capture-device views when split
    std::vector<IDXGIKeyedMutex*> captureTextureMutexPool_;
    std::vector<ID3D11RenderTargetView*> poolRenderTargetViews_;
    int32_t poolWidth_ = 0;
    int32_t poolHeight_ = 0;
    std::atomic<uint32_t> poolWriteIndex_{0};

    // Legacy single-texture for GetNextFrame (pull mode)
    ID3D11Texture2D* cachedTexture_ = nullptr;
    int32_t cachedWidth_ = 0;
    int32_t cachedHeight_ = 0;

    std::mutex frameMutex_;
    std::mutex frameProcessingMutex_;
    std::mutex callbackDrainMutex_;
    int64_t lastFrameTime_ = 0;
    bool frameReady_ = false;
    uint32_t frameWidth_ = 0;
    uint32_t frameHeight_ = 0;
    int64_t frameTimestamp_ = 0;

    ID3D11Device* encoderDevice_ = nullptr;  // Non-owning MediaEngine device
    ID3D11Device* d3dDevice_ = nullptr;      // Capture device (dedicated when split succeeds)
    ID3D11DeviceContext* d3dContext_ = nullptr;
    bool usingDedicatedCaptureDevice_ = false;

    winrt::event_token frameArrivedToken_;

    // Event signaled on frame arrival for efficient waiting
    HANDLE frameArrivedEvent_ = NULL;

    // QPC frequency cached for timestamp conversion
    int64_t qpcFreq_ = 0;
    std::atomic<int64_t> lastDeliveredSourceQpc_{0};
    std::atomic<int64_t> lastDeliveredRawSourceQpc_{0};
    std::atomic<int64_t> lastObservedRawSourceQpc_{0};
    std::atomic<int64_t> lastAssignedSourceQpc_{0};

    // Callback function for direct frame processing.
    // Atomic raw function pointer: only static functions (or nullptr) are ever
    // stored, so std::function overhead and its non-atomic nature are avoided.
    // This eliminates the data race between the WinRT callback thread (reader)
    // and the main thread (writer during start/stop recording).
    using DirectFrameCallbackFn = void (*)(ID3D11Texture2D*, uint32_t, uint32_t, int64_t, int64_t, bool, bool, int32_t,
                                           int32_t, WgcPoolSlotLease&&);
    std::atomic<DirectFrameCallbackFn> frameCallback_{nullptr};
    std::atomic<uint32_t> callbackFrameCount_{0};
    std::atomic<uint32_t> inputFrameCount_{0};
    std::atomic<int64_t> lastCallbackStartQpc_{0};
    std::atomic<int64_t> callbackGapAvgUs_{0};
    std::atomic<int64_t> callbackGapMaxUs_{0};
    std::atomic<int64_t> callbackProcessAvgUs_{0};
    std::atomic<int64_t> callbackProcessMaxUs_{0};
    std::atomic<uint32_t> callbackDrainMaxCount_{0};
    std::atomic<int64_t> sourceIntervalAvgUs_{0};
    std::atomic<int64_t> sourceJitterAvgUs_{0};
    std::atomic<int64_t> sourceJitterMaxUs_{0};
    std::atomic<int64_t> sourceToCopyLatencyAvgUs_{0};
    std::atomic<int64_t> sourceToCopyLatencyMaxUs_{0};
    std::atomic<uint32_t> deliveredRatePerSec_{0};
    std::atomic<uint32_t> deliveredMin250Fps_{0};
    std::atomic<uint32_t> deliveredMin500Fps_{0};
    std::atomic<uint32_t> inputMin250Fps_{0};
    std::atomic<uint32_t> inputMin500Fps_{0};

    // Frame throttle: skip CopyResource if we're ahead of target FPS
    uint32_t targetFps_ = 0;
    int64_t targetIntervalQPC_ = 0;  // Minimum QPC ticks between captured frames (0 = no throttle)
    int64_t lastCapturedQPC_ = 0;    // QPC of last frame we actually copied
    int64_t nextCaptureQPC_ = 0;     // Next QPC deadline that is allowed to perform a GPU copy
    std::atomic<uint32_t> skippedFrameCount_{0};
    std::atomic<uint32_t> pacingSkipCount_{0};
    std::atomic<uint32_t> throttleSkipCount_{0};
    std::atomic<uint32_t> staleSkipCount_{0};
    std::atomic<uint32_t> staleDuplicateTimestampCount_{0};
    std::atomic<uint32_t> staleOutOfOrderTimestampCount_{0};
    std::atomic<uint32_t> normalizedDuplicateTimestampCount_{0};
    std::atomic<uint32_t> duplicateTimestampSkipCount_{0};
    std::atomic<uint32_t> cursorOnlySkipCount_{0};
    std::atomic<uint32_t> poolDropCount_{0};
    std::atomic<uint32_t> keyedMutexAcquireFailCount_{0};
    std::atomic<uint32_t> keyedMutexReleaseFailCount_{0};
    std::atomic<uint32_t> splitDeviceFlushCount_{0};
    std::atomic<uint32_t> splitDeviceFlushSkippedCount_{0};
    std::atomic<uint32_t> poolSlotFastRewriteCount_{0};
    std::atomic<int64_t> lastPoolSlotRewriteUs_{0};
    std::vector<int64_t> poolSlotLastWriteQpc_;
    bool dirtyRegionModeEnabled_ = false;
    HCURSOR lastCursorHandle_ = nullptr;
    POINT lastCursorScreenPos_ = {};
    bool lastCursorScreenPosValid_ = false;
    int32_t lastCursorWidth_ = 64;
    int32_t lastCursorHeight_ = 64;
    int32_t lastCursorHotspotX_ = 0;
    int32_t lastCursorHotspotY_ = 0;

    // External throttle flag (e.g., encoder bottlenecked)
    const std::atomic<bool>* throttleFlag_ = nullptr;

    // Dynamic format detection
    bool formatDetected_ = false;  // True after first frame format is checked

    // Perf tracking
    std::atomic<int64_t> lastCopyUs_{0};
    int64_t lastObservedSourceQpc_ = 0;
    int64_t smoothedSourceIntervalQpc_ = 0;
    uint64_t sourceIntervalSamples_ = 0;
    uint64_t sourceIntervalAccumUs_ = 0;
    uint64_t sourceJitterAccumUs_ = 0;
    uint32_t sourceJitterMaxUsValue_ = 0;
    uint64_t sourceToCopyLatencySamples_ = 0;
    uint64_t sourceToCopyLatencyAccumUs_ = 0;
    uint32_t sourceToCopyLatencyMaxUsValue_ = 0;
    ce::rate_window::SlidingRateWindow<> deliveredRateWindow_;
    ce::rate_window::SlidingRateWindow<> inputRateWindow_;

    DXGI_FORMAT poolSourceFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    DXGI_FORMAT poolFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    DXGI_FORMAT smoothnessSourceFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    DXGI_FORMAT smoothnessCopyFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    uint64_t smoothnessSourceBytesPerSurface_ = 0;
    uint64_t smoothnessCopyBytesPerSurface_ = 0;
    uint64_t smoothnessSourceEstimatedVramBytes_ = 0;
    uint64_t smoothnessCopyEstimatedVramBytes_ = 0;
    bool compactRetainedCopyActive_ = false;
    ID3D11VertexShader* poolCopyVS_ = nullptr;
    ID3D11PixelShader* poolCopyPS_ = nullptr;
    ID3D11SamplerState* poolCopySampler_ = nullptr;
    ID3D11Buffer* poolCopyCB_ = nullptr;
    ID3D11Texture2D* poolCopyStagingTexture_ = nullptr;
    ID3D11ShaderResourceView* poolCopyStagingSrv_ = nullptr;
    uint32_t poolCopyStagingWidth_ = 0;
    uint32_t poolCopyStagingHeight_ = 0;
    DXGI_FORMAT poolCopyStagingFormat_ = DXGI_FORMAT_UNKNOWN;
    std::atomic<int64_t> lastPoolConvertUs_{0};
    winrt::DirectXPixelFormat capturePixelFormat_ = winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
    DXGI_FORMAT captureDxgiFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    HWND targetWindow_ = nullptr;
    HMONITOR targetMonitor_ = nullptr;
    bool useHighPrecisionCapture_ = false;
    bool requireHighPrecisionCapture_ = false;
    bool captureIsHDR_ = false;
    bool borderlessCapture_ = false;
    UINT outputBitsPerColor_ = 8;
    DXGI_COLOR_SPACE_TYPE outputColorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    std::atomic<bool> resetNeeded_{false};
    bool skipSplitDeviceFlush_ = false;
    bool sameDeviceCapture_ = false;
    bool smoothnessBufferEnabled_ = true;
    uint32_t smoothnessOutputFps_ = 0;
    uint32_t smoothnessMaxMs_ = ce::capture_policy::kWgcSmoothnessBufferDefaultMaxMs;
    uint32_t smoothnessVramBudgetMb_ = ce::capture_policy::kWgcSmoothnessBufferDefaultVramBudgetMb;
    uint32_t smoothnessSyncDelayFrames_ = 0;
    uint32_t smoothnessRetainedFrames_ = 0;
    uint32_t smoothnessRetainedFrameCap_ = 0;
    uint32_t sourceFramePoolBufferCount_ = ce::capture_policy::kWgcSmoothnessSourceFramePoolMinBuffers;
    uint32_t smoothnessBudgetSurfaceCount_ = 0;
    uint32_t smoothnessSafetySlots_ = ce::capture_policy::kWgcSmoothnessBufferPoolSafetyFrames;
    uint32_t smoothnessReservedFreeSlots_ = ce::capture_policy::GetWgcSmoothnessReservedFreeCopySlots();
    uint32_t texturePoolSlotCount_ = ce::capture_policy::kWgcSmoothnessBufferMinPoolFrames;
    uint64_t smoothnessEstimatedVramBytes_ = 0;
    bool smoothnessBudgetExhausted_ = false;
    std::shared_ptr<WgcPoolLeaseState> poolLeaseState_;
    uint64_t poolGeneration_ = 0;
    std::atomic<uint32_t> poolSlotOverwritePreventedCount_{0};
    std::atomic<uint32_t> poolSaturatedDropCount_{0};
    std::atomic<uint32_t> ingressAcceptedCount_{0};
    std::atomic<uint32_t> ingressDecimatedCount_{0};
    std::atomic<uint32_t> ingressAcceptedLowWaterCount_{0};
    std::atomic<uint32_t> ingressAcceptedRecoveryCount_{0};
    std::atomic<uint32_t> ingressAcceptedSourceBelowCount_{0};
    std::atomic<uint32_t> ingressAcceptedHealthyCount_{0};
    std::atomic<uint32_t> ingressAcceptedUniformPlayoutSoftReserveCount_{0};
    std::atomic<uint32_t> ingressAcceptedUniformPlayoutCreditCount_{0};
    std::atomic<uint32_t> ingressDecimatedSoftReserveCount_{0};
    std::atomic<uint32_t> ingressDecimatedHardReserveCount_{0};
    std::atomic<uint32_t> ingressDecimatedCreditCount_{0};
    std::atomic<uint32_t> ingressSoftReservePressureCount_{0};
    std::atomic<uint32_t> ingressHardReservePressureCount_{0};
    std::atomic<uint32_t> ingressRetainedFrames_{0};
    std::atomic<uint32_t> ingressRetainedFrameCap_{0};
    std::atomic<uint32_t> ingressLowWaterFrames_{0};
    std::atomic<bool> ingressRecovering_{false};
    std::atomic<bool> ingressUniformPlayoutOwnsSurplus_{false};
    std::atomic<uint32_t> ingressLastReason_{0};
    std::mutex ingressAdmissionMutex_;
    int64_t ingressCreditLastQpc_ = 0;
    double ingressCreditFrames_ = 1.0;
    std::mutex resetReasonMutex_;
    std::string resetReason_;
    winrt::event_token itemClosedToken_{};
    std::atomic<ULONGLONG> lastHDRCheckTick_{0};
    std::atomic<bool> hdrRecheckPending_{false};

    void FlagResetNeeded(const char* reason) {
        resetNeeded_.store(true, std::memory_order_release);
        if (reason && *reason) {
            std::lock_guard<std::mutex> lock(resetReasonMutex_);
            if (resetReason_.empty()) {
                resetReason_ = reason;
            }
        }
    }

    bool NeedsReset() const {
        return resetNeeded_.load(std::memory_order_acquire);
    }

    std::string ConsumeResetReason() {
        resetNeeded_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(resetReasonMutex_);
        std::string reason = resetReason_;
        resetReason_.clear();
        return reason;
    }

    void PerformHDRRecheck() {
        const ULONGLONG now = GetTickCount64();
        lastHDRCheckTick_.store(now, std::memory_order_relaxed);

        if (!targetMonitor_)
            return;

        DXGI_OUTPUT_DESC1 desc1 = {};
        if (QueryOutputDesc1ForMonitor(targetMonitor_, desc1)) {
            bool newHDR = ::IsHdrOutputColorSpace(desc1.ColorSpace);
            if (newHDR != captureIsHDR_) {
                LogInfo("[WGC] HDR state changed mid-capture: %s -> %s", captureIsHDR_ ? "HDR" : "SDR",
                        newHDR ? "HDR" : "SDR");
                captureIsHDR_ = newHDR;
            }
        }
    }

    // Periodically re-check HDR state (handles mid-capture HDR toggle in Windows settings)
    // but keep the DXGI probe off the WinRT callback hot path.
    void RequestHDRRecheckIfDue() {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG lastCheckTick = lastHDRCheckTick_.load(std::memory_order_relaxed);
        if (now - lastCheckTick < 2000) {
            return;
        }

        hdrRecheckPending_.store(true, std::memory_order_relaxed);
    }

    void MaybePerformDeferredHDRRecheck() {
        if (!hdrRecheckPending_.exchange(false, std::memory_order_relaxed)) {
            return;
        }

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG lastCheckTick = lastHDRCheckTick_.load(std::memory_order_relaxed);
        if (now - lastCheckTick < 2000) {
            return;
        }

        PerformHDRRecheck();
    }

    const char* DescribeCaptureFormat() const {
        switch (captureDxgiFormat_) {
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return "R16G16B16A16_FLOAT";
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                return "R10G10B10A2_UNORM";
            case DXGI_FORMAT_B8G8R8A8_UNORM:
                return "B8G8R8A8_UNORM";
            default:
                return "UNKNOWN";
        }
    }

    uint32_t BytesPerPixelForFormat(DXGI_FORMAT format) const {
        switch (format) {
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return 8;
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            default:
                return 4;
        }
    }

    DXGI_FORMAT GetRetainedPoolFormat(DXGI_FORMAT sourceFormat) const {
        if (ce::video_format::ShouldApplySdrLinearToSrgbBeforeRgb10(sourceFormat, captureIsHDR_)) {
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        }
        return sourceFormat;
    }

    bool IsCompactRetainedCopy(DXGI_FORMAT sourceFormat, DXGI_FORMAT retainedFormat) const {
        return retainedFormat != sourceFormat;
    }

    void ReleasePoolConversionResources() {
        for (auto* rtv : poolRenderTargetViews_) {
            SafeRelease(rtv);
        }
        poolRenderTargetViews_.clear();
        SafeRelease(poolCopyStagingSrv_);
        SafeRelease(poolCopyStagingTexture_);
        poolCopyStagingWidth_ = 0;
        poolCopyStagingHeight_ = 0;
        poolCopyStagingFormat_ = DXGI_FORMAT_UNKNOWN;
        SafeRelease(poolCopyCB_);
        SafeRelease(poolCopySampler_);
        SafeRelease(poolCopyPS_);
        SafeRelease(poolCopyVS_);
        lastPoolConvertUs_.store(0, std::memory_order_relaxed);
    }

    bool EnsurePoolCopyShader() {
        if (poolCopyVS_ && poolCopyPS_ && poolCopySampler_ && poolCopyCB_) {
            return true;
        }
        if (!d3dDevice_) {
            return false;
        }

        HMODULE d3dCompiler = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!d3dCompiler) {
            LogError("[WGC] Failed to load d3dcompiler_47.dll for retained-copy conversion");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                                 LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto d3dCompile = reinterpret_cast<PFN_D3DCompile>(GetProcAddress(d3dCompiler, "D3DCompile"));
        if (!d3dCompile) {
            LogError("[WGC] Failed to resolve D3DCompile for retained-copy conversion");
            FreeLibrary(d3dCompiler);
            return false;
        }

        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        ID3DBlob* errBlob = nullptr;
        HRESULT hr = d3dCompile(WGC_POOL_COPY_SHADER_SRC, strlen(WGC_POOL_COPY_SHADER_SRC), nullptr, nullptr, nullptr,
                                "VS_Main", "vs_4_0", 0, 0, &vsBlob, &errBlob);
        if (FAILED(hr)) {
            if (errBlob) {
                LogError("[WGC] Retained-copy VS compile failed: %s", static_cast<const char*>(errBlob->GetBufferPointer()));
                errBlob->Release();
            }
            FreeLibrary(d3dCompiler);
            return false;
        }
        SafeRelease(errBlob);

        hr = d3dCompile(WGC_POOL_COPY_SHADER_SRC, strlen(WGC_POOL_COPY_SHADER_SRC), nullptr, nullptr, nullptr,
                        "PS_Main", "ps_4_0", 0, 0, &psBlob, &errBlob);
        if (FAILED(hr)) {
            if (errBlob) {
                LogError("[WGC] Retained-copy PS compile failed: %s", static_cast<const char*>(errBlob->GetBufferPointer()));
                errBlob->Release();
            }
            SafeRelease(vsBlob);
            FreeLibrary(d3dCompiler);
            return false;
        }
        SafeRelease(errBlob);

        hr = d3dDevice_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &poolCopyVS_);
        SafeRelease(vsBlob);
        if (FAILED(hr)) {
            LogError("[WGC] Retained-copy CreateVertexShader failed: 0x%08lX", (unsigned long)hr);
            SafeRelease(psBlob);
            FreeLibrary(d3dCompiler);
            return false;
        }

        hr = d3dDevice_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &poolCopyPS_);
        SafeRelease(psBlob);
        if (FAILED(hr)) {
            LogError("[WGC] Retained-copy CreatePixelShader failed: 0x%08lX", (unsigned long)hr);
            SafeRelease(poolCopyVS_);
            FreeLibrary(d3dCompiler);
            return false;
        }
        FreeLibrary(d3dCompiler);

        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr = d3dDevice_->CreateSamplerState(&samplerDesc, &poolCopySampler_);
        if (FAILED(hr)) {
            LogError("[WGC] Retained-copy CreateSamplerState failed: 0x%08lX", (unsigned long)hr);
            SafeRelease(poolCopyPS_);
            SafeRelease(poolCopyVS_);
            return false;
        }

        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = 16;
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = d3dDevice_->CreateBuffer(&cbDesc, nullptr, &poolCopyCB_);
        if (FAILED(hr)) {
            LogError("[WGC] Retained-copy CreateBuffer failed: 0x%08lX", (unsigned long)hr);
            SafeRelease(poolCopySampler_);
            SafeRelease(poolCopyPS_);
            SafeRelease(poolCopyVS_);
            return false;
        }

        LogInfo("[WGC] Retained-copy conversion shader created");
        return true;
    }

    bool CreatePoolCopySourceSrv(ID3D11Texture2D* sourceTexture, const D3D11_TEXTURE2D_DESC& sourceDesc,
                                 DXGI_FORMAT inputSrvFormat, ID3D11ShaderResourceView** outSrv,
                                 bool* usedStaging) {
        if (!sourceTexture || !outSrv) {
            return false;
        }
        *outSrv = nullptr;
        if (usedStaging) {
            *usedStaging = false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = inputSrvFormat;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        HRESULT hr = d3dDevice_->CreateShaderResourceView(sourceTexture, &srvDesc, outSrv);
        if (SUCCEEDED(hr) && *outSrv) {
            return true;
        }

        static std::atomic<uint32_t> directSrvFailLogCount{0};
        const uint32_t failLog = directSrvFailLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failLog <= 4) {
            LogInfo(
                "[WGC] Direct WGC source SRV unavailable for retained-copy conversion; using reusable staging "
                "(srcFmt=%s srvFmt=%s bind=0x%X misc=0x%X hr=0x%08lX)",
                DxgiFormatName(sourceDesc.Format), DxgiFormatName(inputSrvFormat), sourceDesc.BindFlags,
                sourceDesc.MiscFlags, (unsigned long)hr);
        }

        if (!poolCopyStagingTexture_ || poolCopyStagingWidth_ != sourceDesc.Width ||
            poolCopyStagingHeight_ != sourceDesc.Height || poolCopyStagingFormat_ != sourceDesc.Format) {
            SafeRelease(poolCopyStagingSrv_);
            SafeRelease(poolCopyStagingTexture_);

            D3D11_TEXTURE2D_DESC stagingDesc = {};
            stagingDesc.Width = sourceDesc.Width;
            stagingDesc.Height = sourceDesc.Height;
            stagingDesc.MipLevels = 1;
            stagingDesc.ArraySize = 1;
            stagingDesc.Format = sourceDesc.Format;
            stagingDesc.SampleDesc.Count = 1;
            stagingDesc.Usage = D3D11_USAGE_DEFAULT;
            stagingDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            hr = d3dDevice_->CreateTexture2D(&stagingDesc, nullptr, &poolCopyStagingTexture_);
            if (FAILED(hr) || !poolCopyStagingTexture_) {
                LogError("[WGC] Failed to create retained-copy source staging texture: 0x%08lX", (unsigned long)hr);
                return false;
            }

            hr = d3dDevice_->CreateShaderResourceView(poolCopyStagingTexture_, &srvDesc, &poolCopyStagingSrv_);
            if (FAILED(hr) || !poolCopyStagingSrv_) {
                LogError("[WGC] Failed to create retained-copy source staging SRV: 0x%08lX", (unsigned long)hr);
                SafeRelease(poolCopyStagingTexture_);
                return false;
            }

            poolCopyStagingWidth_ = sourceDesc.Width;
            poolCopyStagingHeight_ = sourceDesc.Height;
            poolCopyStagingFormat_ = sourceDesc.Format;
            LogInfo("[WGC] Retained-copy source staging created: %ux%u fmt=%s bytes=%lluMB", sourceDesc.Width,
                    sourceDesc.Height, DxgiFormatName(sourceDesc.Format),
                    static_cast<unsigned long long>(
                        (ce::capture_policy::EstimateWgcSurfaceBytes(sourceDesc.Width, sourceDesc.Height,
                                                                     BytesPerPixelForFormat(sourceDesc.Format)) +
                         1024ull * 1024ull - 1ull) /
                        (1024ull * 1024ull)));
        }

        d3dContext_->CopyResource(poolCopyStagingTexture_, sourceTexture);
        poolCopyStagingSrv_->AddRef();
        *outSrv = poolCopyStagingSrv_;
        if (usedStaging) {
            *usedStaging = true;
        }
        return true;
    }

    bool RenderFrameToPoolSlot(ID3D11Texture2D* sourceTexture, const D3D11_TEXTURE2D_DESC& sourceDesc,
                               ID3D11RenderTargetView* targetRtv, bool linearToSrgb, bool* usedStaging) {
        if (!sourceTexture || !targetRtv || !d3dContext_ || !d3dDevice_) {
            return false;
        }
        if (!EnsurePoolCopyShader()) {
            return false;
        }

        const DXGI_FORMAT inputSrvFormat = ce::video_format::GetRgbShaderResourceViewFormat(sourceDesc.Format);
        if (inputSrvFormat == DXGI_FORMAT_UNKNOWN) {
            LogError("[WGC] Unsupported retained-copy source format for shader conversion: %s",
                     DxgiFormatName(sourceDesc.Format));
            return false;
        }

        ID3D11ShaderResourceView* sourceSrv = nullptr;
        bool staging = false;
        if (!CreatePoolCopySourceSrv(sourceTexture, sourceDesc, inputSrvFormat, &sourceSrv, &staging)) {
            return false;
        }
        if (usedStaging) {
            *usedStaging = staging;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = d3dContext_->Map(poolCopyCB_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr) || !mapped.pData) {
            LogError("[WGC] Failed to map retained-copy constant buffer: 0x%08lX", (unsigned long)hr);
            SafeRelease(sourceSrv);
            return false;
        }
        uint32_t* cbData = static_cast<uint32_t*>(mapped.pData);
        cbData[0] = linearToSrgb ? 1u : 0u;
        cbData[1] = 0;
        cbData[2] = 0;
        cbData[3] = 0;
        d3dContext_->Unmap(poolCopyCB_, 0);

        D3D11ContextStateGuard stateGuard(d3dContext_);
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(sourceDesc.Width);
        viewport.Height = static_cast<float>(sourceDesc.Height);
        viewport.MaxDepth = 1.0f;
        d3dContext_->RSSetViewports(1, &viewport);
        d3dContext_->OMSetRenderTargets(1, &targetRtv, nullptr);
        d3dContext_->VSSetShader(poolCopyVS_, nullptr, 0);
        d3dContext_->PSSetShader(poolCopyPS_, nullptr, 0);
        d3dContext_->PSSetShaderResources(0, 1, &sourceSrv);
        d3dContext_->PSSetSamplers(0, 1, &poolCopySampler_);
        d3dContext_->PSSetConstantBuffers(0, 1, &poolCopyCB_);
        d3dContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d3dContext_->IASetInputLayout(nullptr);
        d3dContext_->Draw(3, 0);

        ID3D11RenderTargetView* nullRtv = nullptr;
        ID3D11ShaderResourceView* nullSrv = nullptr;
        d3dContext_->OMSetRenderTargets(1, &nullRtv, nullptr);
        d3dContext_->PSSetShaderResources(0, 1, &nullSrv);
        SafeRelease(sourceSrv);
        return true;
    }

    ce::capture_policy::WgcSmoothnessSurfaceBudget ComputeTexturePoolBudget(uint32_t width, uint32_t height,
                                                                            DXGI_FORMAT format) const {
        const DXGI_FORMAT retainedFormat = GetRetainedPoolFormat(format);
        return ce::capture_policy::ComputeWgcSmoothnessSurfaceBudget(
            smoothnessBufferEnabled_ ? smoothnessOutputFps_ : 0u, smoothnessBufferEnabled_ ? smoothnessMaxMs_ : 0u,
            width, height, BytesPerPixelForFormat(format), BytesPerPixelForFormat(retainedFormat),
            smoothnessVramBudgetMb_, smoothnessSyncDelayFrames_);
    }
    void UpdateSmoothnessBudget(uint32_t width, uint32_t height, DXGI_FORMAT format, bool logBudget) {
        const DXGI_FORMAT retainedFormat = GetRetainedPoolFormat(format);
        const auto budget = ComputeTexturePoolBudget(width, height, format);
        smoothnessRetainedFrames_ = smoothnessBufferEnabled_ ? budget.retainedExtraFrames : 0u;
        smoothnessRetainedFrameCap_ = budget.retainedFrameCap;
        sourceFramePoolBufferCount_ = budget.sourceFramePoolBuffers;
        smoothnessBudgetSurfaceCount_ = budget.budgetSurfaceCount;
        smoothnessSafetySlots_ = budget.safetySlots;
        smoothnessReservedFreeSlots_ = budget.reservedFreeCopySlots;
        smoothnessSourceFormat_ = format;
        smoothnessCopyFormat_ = retainedFormat;
        smoothnessSourceBytesPerSurface_ = budget.sourceBytesPerSurface;
        smoothnessCopyBytesPerSurface_ = budget.copyBytesPerSurface;
        smoothnessSourceEstimatedVramBytes_ = budget.sourceEstimatedBytes;
        smoothnessCopyEstimatedVramBytes_ = budget.copyEstimatedBytes;
        smoothnessEstimatedVramBytes_ = budget.estimatedBytes;
        smoothnessBudgetExhausted_ = budget.budgetExhausted;
        texturePoolSlotCount_ = budget.copyPoolSlots;
        compactRetainedCopyActive_ = IsCompactRetainedCopy(format, retainedFormat);
        ingressRetainedFrameCap_.store(smoothnessRetainedFrameCap_, std::memory_order_relaxed);
        if (logBudget) {
            const uint32_t desiredFrames = smoothnessBufferEnabled_ ? ce::capture_policy::GetWgcSmoothnessDesiredFrames(
                                                                          smoothnessOutputFps_, smoothnessMaxMs_)
                                                                    : 0;
            LogInfo(
                "[WGC] Smoothness buffer budget: enabled=%d targetMs=%u outputFps=%u desiredFrames=%u "
                "retainedFrames=%u sourceFramePoolBuffers=%u copyPoolSlots=%u budgetSurfaces=%u syncFrames=%u "
                "extraFrames=%u retainedCap=%u reservedFreeSlots=%u safetySlots=%u fmt=%d %ux%u bpp=%u budget=%uMB "
                "sourceFmt=%s retainedFmt=%s compactRetained=%d sourceSurfaceMB=%.1f copySurfaceMB=%.1f "
                "sourceBudgetMB=%.1f copyBudgetMB=%.1f estimated=%lluMB capLimited=%d budgetExhausted=%d",
                smoothnessBufferEnabled_ ? 1 : 0, smoothnessMaxMs_, smoothnessOutputFps_, desiredFrames,
                smoothnessRetainedFrames_, sourceFramePoolBufferCount_, texturePoolSlotCount_,
                smoothnessBudgetSurfaceCount_, smoothnessSyncDelayFrames_, smoothnessRetainedFrames_,
                smoothnessRetainedFrameCap_, smoothnessReservedFreeSlots_, smoothnessSafetySlots_, format, width,
                height, BytesPerPixelForFormat(format), smoothnessVramBudgetMb_, DxgiFormatName(format),
                DxgiFormatName(retainedFormat), compactRetainedCopyActive_ ? 1 : 0,
                static_cast<double>(smoothnessSourceBytesPerSurface_) / (1024.0 * 1024.0),
                static_cast<double>(smoothnessCopyBytesPerSurface_) / (1024.0 * 1024.0),
                static_cast<double>(smoothnessSourceEstimatedVramBytes_) / (1024.0 * 1024.0),
                static_cast<double>(smoothnessCopyEstimatedVramBytes_) / (1024.0 * 1024.0),
                static_cast<unsigned long long>((smoothnessEstimatedVramBytes_ + 1024ull * 1024ull - 1ull) /
                                                (1024ull * 1024ull)),
                budget.capLimited ? 1 : 0, budget.budgetExhausted ? 1 : 0);
        }
    }
    void ReleaseTexturePool() {
        ReleasePoolConversionResources();
        for (auto* mutex : captureTextureMutexPool_) {
            SafeRelease(mutex);
        }
        for (auto* texture : captureTexturePool_) {
            SafeRelease(texture);
        }
        for (auto* texture : texturePool_) {
            SafeRelease(texture);
        }
        texturePool_.clear();
        captureTexturePool_.clear();
        captureTextureMutexPool_.clear();
        poolSlotLastWriteQpc_.clear();
        poolLeaseState_.reset();
        poolWidth_ = 0;
        poolHeight_ = 0;
        poolSourceFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
        poolFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
        compactRetainedCopyActive_ = false;
        poolWriteIndex_.store(0, std::memory_order_relaxed);
    }

    void EnableMultithreadProtection(ID3D11Device* device, const char* label) {
        if (!device) {
            return;
        }

        ID3D11Multithread* multithread = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread))) && multithread) {
            multithread->SetMultithreadProtected(TRUE);
            multithread->Release();
            LogInfo("[WGC] D3D11 multithread protection enabled on %s device", label);
        }
    }

    bool InitializeDevices(ID3D11Device* encoderDevice) {
        if (!encoderDevice) {
            return false;
        }

        ReleaseTexturePool();
        SafeRelease(d3dContext_);
        if (usingDedicatedCaptureDevice_) {
            SafeRelease(d3dDevice_);
        } else {
            d3dDevice_ = nullptr;
        }

        encoderDevice_ = encoderDevice;
        usingDedicatedCaptureDevice_ = false;

        if (sameDeviceCapture_) {
            d3dDevice_ = encoderDevice_;
            d3dDevice_->GetImmediateContext(&d3dContext_);
            if (!d3dContext_) {
                LogError("[WGC] Failed to acquire same-device D3D11 immediate context");
                d3dDevice_ = nullptr;
                return false;
            }
            EnableMultithreadProtection(d3dDevice_, "same-device capture");
            LogInfo("[WGC] Same-device capture enabled; reusing encoder D3D11 device");
            return true;
        }

        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        HRESULT hr = encoderDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (SUCCEEDED(hr) && dxgiDevice) {
            hr = dxgiDevice->GetAdapter(&adapter);
        }
        SafeRelease(dxgiDevice);

        if (SUCCEEDED(hr) && adapter) {
            DXGI_ADAPTER_DESC adapterDesc = {};
            adapter->GetDesc(&adapterDesc);

            D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
            hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
                                   0, D3D11_SDK_VERSION, &d3dDevice_, &featureLevel, &d3dContext_);
            SafeRelease(adapter);

            if (SUCCEEDED(hr) && d3dDevice_ && d3dContext_) {
                usingDedicatedCaptureDevice_ = (d3dDevice_ != encoderDevice_);
                EnableMultithreadProtection(d3dDevice_, "capture");
                LogInfo("[WGC] Dedicated capture D3D11 device created (FL=0x%x, adapter=%ls)", featureLevel,
                        adapterDesc.Description);
                return true;
            }

            SafeRelease(d3dContext_);
            SafeRelease(d3dDevice_);
            LogWarn("[WGC] Dedicated capture device creation failed (0x%08lX); falling back to shared device",
                    (unsigned long)hr);
        } else {
            SafeRelease(adapter);
            LogWarn("[WGC] Failed to resolve encoder adapter for dedicated capture device; falling back");
        }

        d3dDevice_ = encoderDevice_;
        d3dDevice_->GetImmediateContext(&d3dContext_);
        if (!d3dContext_) {
            LogError("[WGC] Failed to acquire fallback shared D3D11 immediate context");
            d3dDevice_ = nullptr;
            return false;
        }
        EnableMultithreadProtection(d3dDevice_, "shared capture");
        return true;
    }

    void ReleaseCapturedFrame(WGCCapturedFrame& frame) {
        SafeRelease(frame.texture);
        frame.poolLease.Reset();
        frame.poolSlot = std::numeric_limits<uint32_t>::max();
        frame.poolGeneration = 0;
    }

    void ResetStats() {
        resetNeeded_.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(resetReasonMutex_);
            resetReason_.clear();
        }
        callbackFrameCount_.store(0, std::memory_order_relaxed);
        inputFrameCount_.store(0, std::memory_order_relaxed);
        lastCallbackStartQpc_.store(0, std::memory_order_relaxed);
        callbackGapAvgUs_.store(0, std::memory_order_relaxed);
        callbackGapMaxUs_.store(0, std::memory_order_relaxed);
        callbackProcessAvgUs_.store(0, std::memory_order_relaxed);
        callbackProcessMaxUs_.store(0, std::memory_order_relaxed);
        callbackDrainMaxCount_.store(0, std::memory_order_relaxed);
        sourceIntervalAvgUs_.store(0, std::memory_order_relaxed);
        sourceJitterAvgUs_.store(0, std::memory_order_relaxed);
        sourceJitterMaxUs_.store(0, std::memory_order_relaxed);
        sourceToCopyLatencyAvgUs_.store(0, std::memory_order_relaxed);
        sourceToCopyLatencyMaxUs_.store(0, std::memory_order_relaxed);
        deliveredRatePerSec_.store(0, std::memory_order_relaxed);
        deliveredMin250Fps_.store(0, std::memory_order_relaxed);
        deliveredMin500Fps_.store(0, std::memory_order_relaxed);
        inputMin250Fps_.store(0, std::memory_order_relaxed);
        inputMin500Fps_.store(0, std::memory_order_relaxed);
        skippedFrameCount_.store(0, std::memory_order_relaxed);
        pacingSkipCount_.store(0, std::memory_order_relaxed);
        throttleSkipCount_.store(0, std::memory_order_relaxed);
        staleSkipCount_.store(0, std::memory_order_relaxed);
        staleDuplicateTimestampCount_.store(0, std::memory_order_relaxed);
        staleOutOfOrderTimestampCount_.store(0, std::memory_order_relaxed);
        normalizedDuplicateTimestampCount_.store(0, std::memory_order_relaxed);
        duplicateTimestampSkipCount_.store(0, std::memory_order_relaxed);
        cursorOnlySkipCount_.store(0, std::memory_order_relaxed);
        poolDropCount_.store(0, std::memory_order_relaxed);
        keyedMutexAcquireFailCount_.store(0, std::memory_order_relaxed);
        keyedMutexReleaseFailCount_.store(0, std::memory_order_relaxed);
        splitDeviceFlushCount_.store(0, std::memory_order_relaxed);
        splitDeviceFlushSkippedCount_.store(0, std::memory_order_relaxed);
        poolSlotFastRewriteCount_.store(0, std::memory_order_relaxed);
        lastPoolSlotRewriteUs_.store(0, std::memory_order_relaxed);
        poolSlotOverwritePreventedCount_.store(0, std::memory_order_relaxed);
        poolSaturatedDropCount_.store(0, std::memory_order_relaxed);
        ingressAcceptedCount_.store(0, std::memory_order_relaxed);
        ingressDecimatedCount_.store(0, std::memory_order_relaxed);
        ingressAcceptedLowWaterCount_.store(0, std::memory_order_relaxed);
        ingressAcceptedRecoveryCount_.store(0, std::memory_order_relaxed);
        ingressAcceptedSourceBelowCount_.store(0, std::memory_order_relaxed);
        ingressAcceptedHealthyCount_.store(0, std::memory_order_relaxed);
        ingressAcceptedUniformPlayoutSoftReserveCount_.store(0, std::memory_order_relaxed);
        ingressAcceptedUniformPlayoutCreditCount_.store(0, std::memory_order_relaxed);
        ingressDecimatedSoftReserveCount_.store(0, std::memory_order_relaxed);
        ingressDecimatedHardReserveCount_.store(0, std::memory_order_relaxed);
        ingressDecimatedCreditCount_.store(0, std::memory_order_relaxed);
        ingressSoftReservePressureCount_.store(0, std::memory_order_relaxed);
        ingressHardReservePressureCount_.store(0, std::memory_order_relaxed);
        ingressRetainedFrames_.store(0, std::memory_order_relaxed);
        ingressRetainedFrameCap_.store(smoothnessRetainedFrameCap_, std::memory_order_relaxed);
        ingressLowWaterFrames_.store(0, std::memory_order_relaxed);
        ingressRecovering_.store(false, std::memory_order_relaxed);
        ingressUniformPlayoutOwnsSurplus_.store(false, std::memory_order_relaxed);
        ingressLastReason_.store(kWgcIngressReasonUncapped, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> ingressLock(ingressAdmissionMutex_);
            ingressCreditLastQpc_ = 0;
            ingressCreditFrames_ = 1.0;
        }
        if (poolLeaseState_) {
            const uint32_t current = poolLeaseState_->leasedCurrent.load(std::memory_order_relaxed);
            poolLeaseState_->leasedMax.store(current, std::memory_order_relaxed);
            poolLeaseState_->freeMin.store(
                current >= poolLeaseState_->slotCount ? 0u : poolLeaseState_->slotCount - current,
                std::memory_order_relaxed);
            poolLeaseState_->releaseMismatchCount.store(0, std::memory_order_relaxed);
        }
        std::fill(poolSlotLastWriteQpc_.begin(), poolSlotLastWriteQpc_.end(), 0);
        lastCopyUs_.store(0, std::memory_order_relaxed);
        lastPoolConvertUs_.store(0, std::memory_order_relaxed);
        lastDeliveredSourceQpc_.store(0, std::memory_order_relaxed);
        lastDeliveredRawSourceQpc_.store(0, std::memory_order_relaxed);
        lastObservedRawSourceQpc_.store(0, std::memory_order_relaxed);
        lastAssignedSourceQpc_.store(0, std::memory_order_relaxed);
        lastObservedSourceQpc_ = 0;
        smoothedSourceIntervalQpc_ = 0;
        sourceIntervalSamples_ = 0;
        sourceIntervalAccumUs_ = 0;
        sourceJitterAccumUs_ = 0;
        sourceJitterMaxUsValue_ = 0;
        sourceToCopyLatencySamples_ = 0;
        sourceToCopyLatencyAccumUs_ = 0;
        sourceToCopyLatencyMaxUsValue_ = 0;
        deliveredRateWindow_.Reset();
        inputRateWindow_.Reset();
        lastCapturedQPC_ = 0;
        nextCaptureQPC_ = 0;
        lastCursorScreenPosValid_ = false;
        std::lock_guard<std::mutex> lock(frameMutex_);
        while (!pendingFrames_.empty()) {
            WGCCapturedFrame stale = std::move(pendingFrames_.front());
            pendingFrames_.pop_front();
            ReleaseCapturedFrame(stale);
        }
    }

    void ReleasePendingFramesLocked() {
        while (!pendingFrames_.empty()) {
            WGCCapturedFrame stale = std::move(pendingFrames_.front());
            pendingFrames_.pop_front();
            ReleaseCapturedFrame(stale);
        }
    }

    void EnqueueFrameInternal(WGCCapturedFrame&& frame) {
        if (!frame.texture) {
            return;
        }

        const int64_t frameKey = frame.rawTimestamp > 0 ? frame.rawTimestamp : frame.timestamp;
        if (!pendingFrames_.empty()) {
            WGCCapturedFrame& lastPending = pendingFrames_.back();
            const int64_t lastKey = lastPending.rawTimestamp > 0 ? lastPending.rawTimestamp : lastPending.timestamp;
            if (frameKey > 0 && lastKey > 0 && frameKey == lastKey) {
                ReleaseCapturedFrame(lastPending);
                lastPending = std::move(frame);
                return;
            }
        }

        pendingFrames_.push_back(std::move(frame));
        while (pendingFrames_.size() > kMaxPendingFrames) {
            WGCCapturedFrame stale = std::move(pendingFrames_.front());
            pendingFrames_.pop_front();
            ReleaseCapturedFrame(stale);
        }
    }

    void QueuePendingFrame(WGCCapturedFrame&& frame) {
        std::lock_guard<std::mutex> lock(frameMutex_);
        EnqueueFrameInternal(std::move(frame));
    }

    void RecordInputFrameEvent() {
        const uint64_t nowMs = GetTickCount64();
        inputRateWindow_.AddSample(nowMs);
        inputMin250Fps_.store(inputRateWindow_.MinRatePerSecond(nowMs, 250, 1000), std::memory_order_relaxed);
        inputMin500Fps_.store(inputRateWindow_.MinRatePerSecond(nowMs, 500, 1000), std::memory_order_relaxed);
    }

    void RecordDeliveredFrameEvent() {
        const uint64_t nowMs = GetTickCount64();
        deliveredRateWindow_.AddSample(nowMs);
        deliveredRatePerSec_.store(deliveredRateWindow_.RatePerSecond(nowMs, 1000), std::memory_order_relaxed);
        deliveredMin250Fps_.store(deliveredRateWindow_.MinRatePerSecond(nowMs, 250, 1000), std::memory_order_relaxed);
        deliveredMin500Fps_.store(deliveredRateWindow_.MinRatePerSecond(nowMs, 500, 1000), std::memory_order_relaxed);
    }

    void RecordSourceTimingSample(int64_t sourceFrameQpc) {
        if (sourceFrameQpc <= 0 || qpcFreq_ <= 0) {
            return;
        }

        if (lastObservedSourceQpc_ > 0 && sourceFrameQpc > lastObservedSourceQpc_) {
            const int64_t intervalQpc = sourceFrameQpc - lastObservedSourceQpc_;
            const int64_t intervalUs = (intervalQpc * 1000000) / qpcFreq_;
            if (intervalUs > 0) {
                sourceIntervalAccumUs_ += static_cast<uint64_t>(intervalUs);
                sourceIntervalSamples_++;
                sourceIntervalAvgUs_.store(static_cast<int64_t>(sourceIntervalAccumUs_ / sourceIntervalSamples_),
                                           std::memory_order_relaxed);

                if (smoothedSourceIntervalQpc_ <= 0) {
                    smoothedSourceIntervalQpc_ = intervalQpc;
                } else {
                    smoothedSourceIntervalQpc_ = (smoothedSourceIntervalQpc_ * 7 + intervalQpc) / 8;
                }

                if (smoothedSourceIntervalQpc_ > 0) {
                    const int64_t jitterQpc = intervalQpc >= smoothedSourceIntervalQpc_
                                                  ? (intervalQpc - smoothedSourceIntervalQpc_)
                                                  : (smoothedSourceIntervalQpc_ - intervalQpc);
                    const int64_t jitterUs = (jitterQpc * 1000000) / qpcFreq_;
                    sourceJitterAccumUs_ += static_cast<uint64_t>(jitterUs);
                    sourceJitterAvgUs_.store(static_cast<int64_t>(sourceJitterAccumUs_ / sourceIntervalSamples_),
                                             std::memory_order_relaxed);
                    sourceJitterMaxUsValue_ =
                        std::max<uint32_t>(sourceJitterMaxUsValue_, static_cast<uint32_t>(jitterUs));
                    sourceJitterMaxUs_.store(sourceJitterMaxUsValue_, std::memory_order_relaxed);
                }
            }
        }

        lastObservedSourceQpc_ = sourceFrameQpc;
    }

    void RecordSourceToCopyLatency(int64_t sourceFrameQpc, int64_t copyCompleteQpc) {
        if (sourceFrameQpc <= 0 || copyCompleteQpc <= sourceFrameQpc || qpcFreq_ <= 0) {
            return;
        }

        const int64_t latencyUs = ((copyCompleteQpc - sourceFrameQpc) * 1000000) / qpcFreq_;
        if (latencyUs < 0) {
            return;
        }

        sourceToCopyLatencyAccumUs_ += static_cast<uint64_t>(latencyUs);
        sourceToCopyLatencySamples_++;
        sourceToCopyLatencyAvgUs_.store(static_cast<int64_t>(sourceToCopyLatencyAccumUs_ / sourceToCopyLatencySamples_),
                                        std::memory_order_relaxed);
        sourceToCopyLatencyMaxUsValue_ =
            std::max<uint32_t>(sourceToCopyLatencyMaxUsValue_, static_cast<uint32_t>(latencyUs));
        sourceToCopyLatencyMaxUs_.store(sourceToCopyLatencyMaxUsValue_, std::memory_order_relaxed);
    }

    void ApplyFrameThrottleInterval() {
        if (targetFps_ > 0 && qpcFreq_ > 0) {
            targetIntervalQPC_ = qpcFreq_ / targetFps_;
        } else {
            targetIntervalQPC_ = 0;
        }
        lastCapturedQPC_ = 0;
        nextCaptureQPC_ = 0;
    }

    void ApplyMinUpdateInterval() {
        if (!session_) {
            return;
        }

        try {
            const int64_t interval100ns =
                targetFps_ > 0 ? std::max<int64_t>(1, 10000000ll / static_cast<int64_t>(targetFps_)) : 0;

            IGraphicsCaptureSession5Abi* session5 = nullptr;
            if (!TryQueryComInterface(session_, IID_IGraphicsCaptureSession5Abi, reinterpret_cast<void**>(&session5)) ||
                !session5) {
                LogInfo("[WGC] MinUpdateInterval not available (older WinRT projection/runtime)");
                return;
            }

            const HRESULT hr = session5->put_MinUpdateInterval(interval100ns);
            session5->Release();
            if (FAILED(hr)) {
                LogInfo("[WGC] MinUpdateInterval not available (older WinRT projection/runtime)");
                return;
            }

            if (targetFps_ > 0) {
                LogInfo("[WGC] MinUpdateInterval set to %lld (100ns) for %u fps target", (long long)interval100ns,
                        targetFps_);
            } else {
                LogInfo("[WGC] MinUpdateInterval set to 0 (max rate)");
            }
        } catch (...) {
            LogInfo("[WGC] MinUpdateInterval not available (older WinRT projection/runtime)");
        }
    }

    int64_t GetFrameSourceQpc(const winrt::Direct3D11CaptureFrame& frame) const {
        const auto systemRelativeTime = frame.SystemRelativeTime();
        return HundredNanosecondsToQpcTicks(systemRelativeTime.count(), qpcFreq_);
    }

    bool IsStaleSourceFrameQpc(int64_t sourceFrameQpc) const {
        if (sourceFrameQpc <= 0) {
            return false;
        }

        const int64_t lastDeliveredSourceQpc = lastDeliveredSourceQpc_.load(std::memory_order_relaxed);
        return lastDeliveredSourceQpc > 0 && sourceFrameQpc <= lastDeliveredSourceQpc;
    }

    bool IsOutOfOrderRawSourceFrameQpc(int64_t sourceFrameQpc) const {
        if (sourceFrameQpc <= 0) {
            return false;
        }

        const int64_t lastObservedRawSourceQpc = lastObservedRawSourceQpc_.load(std::memory_order_relaxed);
        return lastObservedRawSourceQpc > 0 && sourceFrameQpc < lastObservedRawSourceQpc;
    }

    int64_t NormalizeSourceFrameQpc(int64_t sourceFrameQpc, bool* duplicateSourceTimestamp = nullptr) {
        if (duplicateSourceTimestamp) {
            *duplicateSourceTimestamp = false;
        }
        if (sourceFrameQpc <= 0) {
            return 0;
        }

        const int64_t rawSourceFrameQpc = sourceFrameQpc;
        const int64_t lastObservedRawSourceQpc = lastObservedRawSourceQpc_.load(std::memory_order_relaxed);
        const int64_t lastAssignedSourceQpc = lastAssignedSourceQpc_.load(std::memory_order_relaxed);
        if (lastAssignedSourceQpc > 0 && rawSourceFrameQpc < lastAssignedSourceQpc) {
            if (duplicateSourceTimestamp) {
                *duplicateSourceTimestamp = true;
            }
            normalizedDuplicateTimestampCount_.fetch_add(1, std::memory_order_relaxed);
            sourceFrameQpc = lastAssignedSourceQpc;
        } else if (lastObservedRawSourceQpc > 0 && rawSourceFrameQpc == lastObservedRawSourceQpc) {
            if (duplicateSourceTimestamp) {
                *duplicateSourceTimestamp = true;
            }
            normalizedDuplicateTimestampCount_.fetch_add(1, std::memory_order_relaxed);
            sourceFrameQpc = lastObservedRawSourceQpc;
        }

        if (rawSourceFrameQpc > lastObservedRawSourceQpc) {
            lastObservedRawSourceQpc_.store(rawSourceFrameQpc, std::memory_order_relaxed);
        }
        if (sourceFrameQpc > lastAssignedSourceQpc) {
            lastAssignedSourceQpc_.store(sourceFrameQpc, std::memory_order_relaxed);
        }
        return sourceFrameQpc;
    }

    bool RefreshCursorMetrics(HCURSOR cursorHandle) {
        if (!cursorHandle) {
            return false;
        }

        if (lastCursorHandle_ == cursorHandle) {
            return true;
        }

        lastCursorHandle_ = cursorHandle;
        lastCursorWidth_ = 64;
        lastCursorHeight_ = 64;
        lastCursorHotspotX_ = 0;
        lastCursorHotspotY_ = 0;

        HICON icon = CopyIcon(cursorHandle);
        if (!icon) {
            return false;
        }

        ICONINFO iconInfo = {};
        if (!GetIconInfo(icon, &iconInfo)) {
            DestroyIcon(icon);
            return false;
        }

        BITMAP bitmap = {};
        if (iconInfo.hbmColor && GetObject(iconInfo.hbmColor, sizeof(bitmap), &bitmap) == sizeof(bitmap)) {
            lastCursorWidth_ = std::max<LONG>(1, bitmap.bmWidth);
            lastCursorHeight_ = std::max<LONG>(1, std::abs(bitmap.bmHeight));
        } else if (iconInfo.hbmMask && GetObject(iconInfo.hbmMask, sizeof(bitmap), &bitmap) == sizeof(bitmap)) {
            lastCursorWidth_ = std::max<LONG>(1, bitmap.bmWidth);
            lastCursorHeight_ = std::max<LONG>(1, std::abs(bitmap.bmHeight) / 2);
        }

        lastCursorHotspotX_ = static_cast<int32_t>(iconInfo.xHotspot);
        lastCursorHotspotY_ = static_cast<int32_t>(iconInfo.yHotspot);

        if (iconInfo.hbmColor) {
            DeleteObject(iconInfo.hbmColor);
        }
        if (iconInfo.hbmMask) {
            DeleteObject(iconInfo.hbmMask);
        }
        DestroyIcon(icon);
        return true;
    }

    static RECT ClampRectToFrame(RECT rect, uint32_t frameWidth, uint32_t frameHeight) {
        rect.left = std::clamp<LONG>(rect.left, 0, static_cast<LONG>(frameWidth));
        rect.top = std::clamp<LONG>(rect.top, 0, static_cast<LONG>(frameHeight));
        rect.right = std::clamp<LONG>(rect.right, 0, static_cast<LONG>(frameWidth));
        rect.bottom = std::clamp<LONG>(rect.bottom, 0, static_cast<LONG>(frameHeight));
        if (rect.right < rect.left) {
            rect.right = rect.left;
        }
        if (rect.bottom < rect.top) {
            rect.bottom = rect.top;
        }
        return rect;
    }

    RECT BuildCursorRectForScreenPoint(const POINT& screenPos, int32_t captureLeft, int32_t captureTop,
                                       uint32_t frameWidth, uint32_t frameHeight, LONG padding) const {
        RECT rect = {};
        rect.left = static_cast<LONG>(screenPos.x - captureLeft - lastCursorHotspotX_ - padding);
        rect.top = static_cast<LONG>(screenPos.y - captureTop - lastCursorHotspotY_ - padding);
        rect.right = rect.left + lastCursorWidth_ + padding * 2;
        rect.bottom = rect.top + lastCursorHeight_ + padding * 2;
        return ClampRectToFrame(rect, frameWidth, frameHeight);
    }

    static bool RectContainsRect(const RECT& outer, const RECT& inner) {
        return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
               inner.bottom <= outer.bottom;
    }

    bool ShouldSkipCursorOnlyDirtyFrame(const winrt::Direct3D11CaptureFrame& frame, uint32_t frameWidth,
                                        uint32_t frameHeight) {
        if (!dirtyRegionModeEnabled_) {
            return false;
        }

        IDirect3D11CaptureFrame2Abi* frame2 = nullptr;
        if (!TryQueryComInterface(frame, IID_IDirect3D11CaptureFrame2Abi, reinterpret_cast<void**>(&frame2)) ||
            !frame2) {
            return false;
        }

        void* dirtyRegionsAbi = nullptr;
        const HRESULT dirtyRegionsHr = frame2->get_DirtyRegions(&dirtyRegionsAbi);
        frame2->Release();
        if (FAILED(dirtyRegionsHr) || !dirtyRegionsAbi) {
            return false;
        }

        winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Graphics::RectInt32> dirtyRegions{
            dirtyRegionsAbi, winrt::take_ownership_from_abi};
        if (!dirtyRegions) {
            return false;
        }

        const uint32_t dirtyRegionCount = dirtyRegions.Size();
        if (dirtyRegionCount == 0 || dirtyRegionCount > 8) {
            return false;
        }

        int32_t captureLeft = 0;
        int32_t captureTop = 0;
        if (!GetCaptureOrigin(captureLeft, captureTop)) {
            return false;
        }

        CURSORINFO cursorInfo = {};
        cursorInfo.cbSize = sizeof(CURSORINFO);
        if (!GetCursorInfo(&cursorInfo) || !cursorInfo.hCursor) {
            lastCursorScreenPosValid_ = false;
            return false;
        }

        RefreshCursorMetrics(cursorInfo.hCursor);

        constexpr LONG kCursorPaddingPx = 48;
        RECT currentCursorRect = BuildCursorRectForScreenPoint(cursorInfo.ptScreenPos, captureLeft, captureTop,
                                                               frameWidth, frameHeight, kCursorPaddingPx);
        RECT previousCursorRect = currentCursorRect;
        if (lastCursorScreenPosValid_) {
            previousCursorRect = BuildCursorRectForScreenPoint(lastCursorScreenPos_, captureLeft, captureTop,
                                                               frameWidth, frameHeight, kCursorPaddingPx);
        }

        const int64_t expandedCursorArea =
            static_cast<int64_t>(std::max<LONG>(1, currentCursorRect.right - currentCursorRect.left)) *
            static_cast<int64_t>(std::max<LONG>(1, currentCursorRect.bottom - currentCursorRect.top));
        const int64_t maxDirtyArea = expandedCursorArea * 3;

        int64_t dirtyArea = 0;
        bool cursorOnly = true;
        for (uint32_t i = 0; i < dirtyRegionCount; ++i) {
            const auto dirtyRegion = dirtyRegions.GetAt(i);
            RECT dirtyRect = {dirtyRegion.X, dirtyRegion.Y, dirtyRegion.X + dirtyRegion.Width,
                              dirtyRegion.Y + dirtyRegion.Height};
            dirtyRect = ClampRectToFrame(dirtyRect, frameWidth, frameHeight);

            const int64_t regionArea = static_cast<int64_t>(std::max<LONG>(0, dirtyRect.right - dirtyRect.left)) *
                                       static_cast<int64_t>(std::max<LONG>(0, dirtyRect.bottom - dirtyRect.top));
            dirtyArea += regionArea;
            if (dirtyArea > maxDirtyArea) {
                cursorOnly = false;
                break;
            }

            if (!RectContainsRect(currentCursorRect, dirtyRect) && !RectContainsRect(previousCursorRect, dirtyRect)) {
                cursorOnly = false;
                break;
            }
        }

        lastCursorScreenPos_ = cursorInfo.ptScreenPos;
        lastCursorScreenPosValid_ = true;
        return cursorOnly;
    }

    HMONITOR ResolveTargetMonitor() const {
        if (targetWindow_) {
            return MonitorFromWindow(targetWindow_, MONITOR_DEFAULTTONEAREST);
        }
        if (targetMonitor_) {
            return targetMonitor_;
        }
        return MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    }

    bool GetCaptureOrigin(int32_t& left, int32_t& top) const {
        left = 0;
        top = 0;

        if (targetWindow_) {
            if (borderlessCapture_) {
                POINT clientTopLeft = {0, 0};
                if (ClientToScreen(targetWindow_, &clientTopLeft)) {
                    left = clientTopLeft.x;
                    top = clientTopLeft.y;
                    return true;
                }
            }

            RECT windowRect = {};
            if (GetWindowRect(targetWindow_, &windowRect)) {
                left = windowRect.left;
                top = windowRect.top;
                return true;
            }
            return false;
        }

        MONITORINFO monitorInfo = {};
        monitorInfo.cbSize = sizeof(monitorInfo);
        HMONITOR monitor = ResolveTargetMonitor();
        if (monitor && GetMonitorInfo(monitor, &monitorInfo)) {
            left = monitorInfo.rcMonitor.left;
            top = monitorInfo.rcMonitor.top;
            return true;
        }

        return false;
    }

    bool QueryOutputDesc1ForMonitor(HMONITOR monitor, DXGI_OUTPUT_DESC1& desc1) {
        if (!monitor) {
            return false;
        }

        IDXGIFactory1* factory = nullptr;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr) || !factory) {
            LogWarn("[WGC] CreateDXGIFactory1 failed while probing output format: 0x%lX", (unsigned long)hr);
            return false;
        }

        bool found = false;
        for (UINT adapterIndex = 0; !found; ++adapterIndex) {
            IDXGIAdapter1* adapter = nullptr;
            hr = factory->EnumAdapters1(adapterIndex, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(hr) || !adapter) {
                continue;
            }

            for (UINT outputIndex = 0; !found; ++outputIndex) {
                IDXGIOutput* output = nullptr;
                hr = adapter->EnumOutputs(outputIndex, &output);
                if (hr == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                if (FAILED(hr) || !output) {
                    continue;
                }

                DXGI_OUTPUT_DESC outputDesc = {};
                if (SUCCEEDED(output->GetDesc(&outputDesc)) && outputDesc.Monitor == monitor) {
                    IDXGIOutput6* output6 = nullptr;
                    hr = output->QueryInterface(IID_PPV_ARGS(&output6));
                    if (SUCCEEDED(hr) && output6) {
                        found = SUCCEEDED(output6->GetDesc1(&desc1));
                        output6->Release();
                    }
                }

                output->Release();
            }

            adapter->Release();
        }

        factory->Release();
        return found;
    }

    void UpdateCaptureFormatSelection() {
        useHighPrecisionCapture_ = false;
        captureIsHDR_ = false;
        outputBitsPerColor_ = 8;
        outputColorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        capturePixelFormat_ = winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
        captureDxgiFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;

        DXGI_OUTPUT_DESC1 desc1 = {};
        const HMONITOR monitor = ResolveTargetMonitor();
        if (!QueryOutputDesc1ForMonitor(monitor, desc1)) {
            if (requireHighPrecisionCapture_) {
                useHighPrecisionCapture_ = true;
                capturePixelFormat_ = winrt::DirectXPixelFormat::R10G10B10A2UIntNormalized;
                captureDxgiFormat_ = DXGI_FORMAT_R10G10B10A2_UNORM;
                LogWarn("[WGC] Output probe unavailable; explicit 10-bit request requires high-precision capture");
            } else {
                LogInfo("[WGC] Output probe unavailable, using BGRA8 capture");
            }
            return;
        }

        outputBitsPerColor_ = desc1.BitsPerColor;
        outputColorSpace_ = desc1.ColorSpace;
        captureIsHDR_ = IsHdrOutputColorSpace(desc1.ColorSpace);
        if (captureIsHDR_) {
            useHighPrecisionCapture_ = true;
            capturePixelFormat_ = winrt::DirectXPixelFormat::R16G16B16A16Float;
            captureDxgiFormat_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
        } else if (desc1.BitsPerColor > 8 || requireHighPrecisionCapture_) {
            useHighPrecisionCapture_ = true;
            capturePixelFormat_ = winrt::DirectXPixelFormat::R10G10B10A2UIntNormalized;
            captureDxgiFormat_ = DXGI_FORMAT_R10G10B10A2_UNORM;
        }

        LogInfo(
            "[WGC] Output probe: bpc=%u colorSpace=%d hdr=%s highPrecision=%s requireHighPrecision=%s "
            "captureFormat=%s",
            outputBitsPerColor_, (int)outputColorSpace_, captureIsHDR_ ? "YES" : "NO",
            useHighPrecisionCapture_ ? "YES" : "NO", requireHighPrecisionCapture_ ? "YES" : "NO",
            DescribeCaptureFormat());
    }

    bool EnsureTexturePool(uint32_t width, uint32_t height,
                           DXGI_FORMAT sourceFormat = DXGI_FORMAT_B8G8R8A8_UNORM) {
        const bool splitDevicePool =
            usingDedicatedCaptureDevice_ && encoderDevice_ && d3dDevice_ && encoderDevice_ != d3dDevice_;
        const DXGI_FORMAT retainedFormat = GetRetainedPoolFormat(sourceFormat);
        const bool compactRetainedCopy = IsCompactRetainedCopy(sourceFormat, retainedFormat);
        UpdateSmoothnessBudget(width, height, sourceFormat, false);
        const uint32_t desiredSlots = std::max<uint32_t>(1u, texturePoolSlotCount_);
        if (poolWidth_ == (int32_t)width && poolHeight_ == (int32_t)height && poolSourceFormat_ == sourceFormat &&
            poolFormat_ == retainedFormat &&
            texturePool_.size() == desiredSlots && !texturePool_.empty() && texturePool_[0] &&
            (!splitDevicePool || (!captureTexturePool_.empty() && captureTexturePool_[0])) &&
            (!compactRetainedCopy || (!poolRenderTargetViews_.empty() && poolRenderTargetViews_[0]))) {
            return true;
        }

        ReleaseTexturePool();
        texturePool_.assign(desiredSlots, nullptr);
        captureTexturePool_.assign(desiredSlots, nullptr);
        captureTextureMutexPool_.assign(desiredSlots, nullptr);
        poolRenderTargetViews_.assign(desiredSlots, nullptr);
        poolSlotLastWriteQpc_.assign(desiredSlots, 0);

        D3D11_TEXTURE2D_DESC copyDesc = {};
        copyDesc.Width = width;
        copyDesc.Height = height;
        copyDesc.MipLevels = 1;
        copyDesc.ArraySize = 1;
        copyDesc.Format = retainedFormat;
        copyDesc.SampleDesc.Count = 1;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        // CRITICAL: VP input view requires RENDER_TARGET bind flag
        // Using only SHADER_RESOURCE causes D3D11 internal stack corruption (0xC0000409)
        copyDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        copyDesc.MiscFlags = splitDevicePool ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : 0;

        for (uint32_t i = 0; i < desiredSlots; i++) {
            HRESULT hr =
                (splitDevicePool ? encoderDevice_ : d3dDevice_)->CreateTexture2D(&copyDesc, nullptr, &texturePool_[i]);
            if (FAILED(hr)) {
                LogError("[WGC] Failed to create texture pool[%d]: 0x%lX", i, (unsigned long)hr);
                ReleaseTexturePool();
                return false;
            }

            if (splitDevicePool) {
                IDXGIResource* dxgiResource = nullptr;
                hr = texturePool_[i]->QueryInterface(IID_PPV_ARGS(&dxgiResource));
                if (FAILED(hr) || !dxgiResource) {
                    LogError("[WGC] Failed to query IDXGIResource for shared texture pool[%d]: 0x%lX", i,
                             (unsigned long)hr);
                    SafeRelease(dxgiResource);
                    ReleaseTexturePool();
                    return false;
                }

                HANDLE sharedHandle = nullptr;
                hr = dxgiResource->GetSharedHandle(&sharedHandle);
                SafeRelease(dxgiResource);
                if (FAILED(hr) || !sharedHandle) {
                    LogError("[WGC] Failed to get shared handle for texture pool[%d]: 0x%lX", i, (unsigned long)hr);
                    ReleaseTexturePool();
                    return false;
                }

                hr = d3dDevice_->OpenSharedResource(sharedHandle, IID_PPV_ARGS(&captureTexturePool_[i]));
                if (FAILED(hr) || !captureTexturePool_[i]) {
                    LogError("[WGC] Failed to open shared capture texture pool[%d]: 0x%lX", i, (unsigned long)hr);
                    ReleaseTexturePool();
                    return false;
                }

                hr = captureTexturePool_[i]->QueryInterface(IID_PPV_ARGS(&captureTextureMutexPool_[i]));
                if (FAILED(hr) || !captureTextureMutexPool_[i]) {
                    LogError("[WGC] Failed to query keyed mutex for capture texture pool[%d]: 0x%lX", i,
                             (unsigned long)hr);
                    ReleaseTexturePool();
                    return false;
                }
            }

            if (compactRetainedCopy) {
                ID3D11Texture2D* renderTarget = captureTexturePool_[i] ? captureTexturePool_[i] : texturePool_[i];
                hr = d3dDevice_->CreateRenderTargetView(renderTarget, nullptr, &poolRenderTargetViews_[i]);
                if (FAILED(hr) || !poolRenderTargetViews_[i]) {
                    LogError("[WGC] Failed to create retained-copy RTV for pool[%u] fmt=%s: 0x%08lX", i,
                             DxgiFormatName(retainedFormat), (unsigned long)hr);
                    ReleaseTexturePool();
                    return false;
                }
            }
        }

        poolWidth_ = width;
        poolHeight_ = height;
        poolSourceFormat_ = sourceFormat;
        poolFormat_ = retainedFormat;
        poolGeneration_++;
        auto leaseState = std::make_shared<WgcPoolLeaseState>();
        leaseState->Init(desiredSlots, poolGeneration_);
        poolLeaseState_ = std::move(leaseState);
        poolWriteIndex_.store(0, std::memory_order_relaxed);

        LogInfo(
            "[WGC] Texture pool created: %dx%d sourceFmt=%s retainedFmt=%s compactRetained=%d "
            "sourceFramePoolBuffers=%u copyPoolSlots=%u budgetSurfaces=%u "
            "syncFrames=%u extraFrames=%u retainedCap=%u reservedFreeSlots=%u safetySlots=%u estimatedSmooth=%lluMB "
            "sourceBudget=%.1fMB copyBudget=%.1fMB convertLast=%lldus "
            "generation=%llu (%s)",
            width, height, DxgiFormatName(sourceFormat), DxgiFormatName(retainedFormat),
            compactRetainedCopy ? 1 : 0, sourceFramePoolBufferCount_, desiredSlots, smoothnessBudgetSurfaceCount_,
            smoothnessSyncDelayFrames_, smoothnessRetainedFrames_, smoothnessRetainedFrameCap_,
            smoothnessReservedFreeSlots_, smoothnessSafetySlots_,
            static_cast<unsigned long long>((smoothnessEstimatedVramBytes_ + 1024ull * 1024ull - 1ull) /
                                            (1024ull * 1024ull)),
            static_cast<double>(smoothnessSourceEstimatedVramBytes_) / (1024.0 * 1024.0),
            static_cast<double>(smoothnessCopyEstimatedVramBytes_) / (1024.0 * 1024.0),
            static_cast<long long>(lastPoolConvertUs_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(poolGeneration_),
            splitDevicePool ? "dedicated capture device" : "shared device");
        return true;
    }

    bool ShouldAdmitFrameToPool(int64_t sourceFrameQpc, int64_t rawSourceFrameQpc, uint32_t poolSize,
                                const std::shared_ptr<WgcPoolLeaseState>& leaseState) {
        const int64_t timingQpc = rawSourceFrameQpc > 0 ? rawSourceFrameQpc : sourceFrameQpc;
        const uint32_t retainedFrames = ingressRetainedFrames_.load(std::memory_order_relaxed);
        uint32_t retainedFrameCap = ingressRetainedFrameCap_.load(std::memory_order_relaxed);
        if (retainedFrameCap == 0) {
            retainedFrameCap = smoothnessRetainedFrameCap_;
        }
        const uint32_t lowWaterFrames = ingressLowWaterFrames_.load(std::memory_order_relaxed);
        const bool recovering = ingressRecovering_.load(std::memory_order_relaxed);
        const uint32_t leasedCurrent = leaseState ? leaseState->leasedCurrent.load(std::memory_order_relaxed) : 0u;
        const uint32_t pressureRetainedFrames = std::max(retainedFrames, leasedCurrent);
        const uint32_t freeCopySlots = poolSize > leasedCurrent ? (poolSize - leasedCurrent) : 0u;
        const uint32_t outputFps = smoothnessOutputFps_;
        const uint32_t inputMin250Fps = inputMin250Fps_.load(std::memory_order_relaxed);
        const uint32_t inputMin500Fps = inputMin500Fps_.load(std::memory_order_relaxed);
        const bool uniformPlayoutOwnsSurplus =
            ingressUniformPlayoutOwnsSurplus_.load(std::memory_order_relaxed);

        ce::capture_policy::WgcIngressAdmissionDecision decision{};
        uint32_t reasonCode = kWgcIngressReasonUncapped;
        {
            std::lock_guard<std::mutex> lock(ingressAdmissionMutex_);
            if (timingQpc > 0 && qpcFreq_ > 0 && outputFps > 0) {
                if (ingressCreditLastQpc_ > 0 && timingQpc > ingressCreditLastQpc_) {
                    const double elapsedFrames =
                        (static_cast<double>(timingQpc - ingressCreditLastQpc_) * static_cast<double>(outputFps)) /
                        static_cast<double>(qpcFreq_);
                    ingressCreditFrames_ = std::min(2.0, ingressCreditFrames_ + std::max(0.0, elapsedFrames));
                } else if (ingressCreditLastQpc_ == 0 || timingQpc < ingressCreditLastQpc_) {
                    ingressCreditFrames_ = std::max(ingressCreditFrames_, 1.0);
                }
                ingressCreditLastQpc_ = timingQpc;
            } else {
                ingressCreditFrames_ = std::max(ingressCreditFrames_, 1.0);
            }

            decision = ce::capture_policy::DecideWgcIngressAdmission(
                pressureRetainedFrames, retainedFrameCap, lowWaterFrames, recovering, outputFps, inputMin250Fps,
                inputMin500Fps, ingressCreditFrames_, freeCopySlots, smoothnessReservedFreeSlots_,
                uniformPlayoutOwnsSurplus);
            reasonCode = WgcIngressReasonCodeFromText(decision.reason);
            if (decision.accept && outputFps > 0 &&
                !ce::capture_policy::IsWgcIngressSourceBelowCfrTarget(outputFps, inputMin250Fps, inputMin500Fps)) {
                ingressCreditFrames_ = std::max(0.0, ingressCreditFrames_ - 1.0);
            }
        }

        ingressLastReason_.store(reasonCode, std::memory_order_relaxed);
        if (decision.softReservePressure) {
            ingressSoftReservePressureCount_.fetch_add(1, std::memory_order_relaxed);
        }
        if (decision.hardReservePressure) {
            ingressHardReservePressureCount_.fetch_add(1, std::memory_order_relaxed);
        }
        if (decision.accept) {
            ingressAcceptedCount_.fetch_add(1, std::memory_order_relaxed);
            switch (reasonCode) {
                case kWgcIngressReasonLowWater:
                    ingressAcceptedLowWaterCount_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case kWgcIngressReasonRecovery:
                    ingressAcceptedRecoveryCount_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case kWgcIngressReasonSourceBelowTarget:
                    ingressAcceptedSourceBelowCount_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case kWgcIngressReasonUniformPlayoutSoftReserve:
                    ingressAcceptedUniformPlayoutSoftReserveCount_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case kWgcIngressReasonUniformPlayoutCredit:
                    ingressAcceptedUniformPlayoutCreditCount_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case kWgcIngressReasonCredit:
                case kWgcIngressReasonHealthy:
                case kWgcIngressReasonUncapped:
                default:
                    ingressAcceptedHealthyCount_.fetch_add(1, std::memory_order_relaxed);
                    break;
            }
            return true;
        }

        skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
        ingressDecimatedCount_.fetch_add(1, std::memory_order_relaxed);
        switch (reasonCode) {
            case kWgcIngressReasonDecimatedHardReserve:
                ingressDecimatedHardReserveCount_.fetch_add(1, std::memory_order_relaxed);
                break;
            case kWgcIngressReasonDecimatedCredit:
                ingressDecimatedCreditCount_.fetch_add(1, std::memory_order_relaxed);
                break;
            case kWgcIngressReasonDecimatedSoftReserve:
            default:
                ingressDecimatedSoftReserveCount_.fetch_add(1, std::memory_order_relaxed);
                break;
        }
        static std::atomic<uint32_t> decimatedLogCount{0};
        const uint32_t logCount = decimatedLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 8 || (logCount % 1000u) == 0u) {
            LogInfo(
                "[WGC] Ingress decimated WGC frame before copy: retained=%u pressure=%u/%u lowWater=%u leased=%u "
                "free=%u reservedFree=%u softReservePressure=%d hardReservePressure=%d "
                "inputMin=%u/%u outputFps=%u frameQpc=%lld reason=%s",
                retainedFrames, pressureRetainedFrames, retainedFrameCap, lowWaterFrames, leasedCurrent, freeCopySlots,
                smoothnessReservedFreeSlots_, decision.softReservePressure ? 1 : 0,
                decision.hardReservePressure ? 1 : 0, inputMin250Fps, inputMin500Fps, outputFps,
                static_cast<long long>(sourceFrameQpc), decision.reason);
        }
        return false;
    }

    bool CopyFrameToPool(ID3D11Texture2D* sourceTexture, const D3D11_TEXTURE2D_DESC& sourceDesc, int64_t sourceFrameQpc,
                         int64_t rawSourceFrameQpc, ID3D11Texture2D** out, int64_t& copyCompleteQpc,
                         WgcPoolSlotLease& outLease, uint32_t& outSlot, uint64_t& outGeneration) {
        if (!sourceTexture || !out) {
            return false;
        }

        *out = nullptr;
        copyCompleteQpc = 0;
        outLease.Reset();
        outSlot = std::numeric_limits<uint32_t>::max();
        outGeneration = 0;

        if (!EnsureTexturePool(sourceDesc.Width, sourceDesc.Height, sourceDesc.Format)) {
            return false;
        }

        const uint32_t poolSize = static_cast<uint32_t>(texturePool_.size());
        if (poolSize == 0) {
            return false;
        }
        const auto leaseState = poolLeaseState_;
        if (!ShouldAdmitFrameToPool(sourceFrameQpc, rawSourceFrameQpc, poolSize, leaseState)) {
            return false;
        }

        const uint32_t startIndex = poolWriteIndex_.fetch_add(1, std::memory_order_relaxed) % poolSize;
        const uint64_t poolGeneration = poolGeneration_;
        for (uint32_t attempt = 0; attempt < poolSize; ++attempt) {
            const uint32_t idx = (startIndex + attempt) % poolSize;
            if (!leaseState || !leaseState->TryAcquire(idx)) {
                poolSlotOverwritePreventedCount_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            WgcPoolSlotLease slotLease(leaseState, idx, poolGeneration);
            ID3D11Texture2D* copyTarget = captureTexturePool_[idx] ? captureTexturePool_[idx] : texturePool_[idx];
            IDXGIKeyedMutex* writeMutex = captureTextureMutexPool_[idx];
            bool mutexAcquired = false;

            if (writeMutex) {
                const HRESULT kmHr = writeMutex->AcquireSync(0, 0);
                if (kmHr != S_OK) {
                    keyedMutexAcquireFailCount_.fetch_add(1, std::memory_order_relaxed);
                    slotLease.Reset();
                    continue;
                }
                mutexAcquired = true;
            }

            LARGE_INTEGER copyStart = {};
            LARGE_INTEGER copyEnd = {};
            QueryPerformanceCounter(&copyStart);

            const bool compactRetainedCopy = IsCompactRetainedCopy(sourceDesc.Format, poolFormat_);
            bool usedConversionStaging = false;
            bool copySucceeded = true;
            if (compactRetainedCopy) {
                const bool linearToSrgb =
                    ce::video_format::ShouldApplySdrLinearToSrgbBeforeRgb10(sourceDesc.Format, captureIsHDR_);
                ID3D11RenderTargetView* targetRtv =
                    idx < poolRenderTargetViews_.size() ? poolRenderTargetViews_[idx] : nullptr;
                copySucceeded =
                    RenderFrameToPoolSlot(sourceTexture, sourceDesc, targetRtv, linearToSrgb, &usedConversionStaging);
            } else {
                d3dContext_->CopyResource(copyTarget, sourceTexture);
            }
            if (mutexAcquired) {
                if (skipSplitDeviceFlush_) {
                    splitDeviceFlushSkippedCount_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    d3dContext_->Flush();
                    splitDeviceFlushCount_.fetch_add(1, std::memory_order_relaxed);
                }
                const HRESULT releaseHr = writeMutex->ReleaseSync(0);
                if (releaseHr != S_OK) {
                    keyedMutexReleaseFailCount_.fetch_add(1, std::memory_order_relaxed);
                    LogWarn("[WGC] Shared texture ReleaseSync failed for slot %u: 0x%08lX", idx,
                            (unsigned long)releaseHr);
                    slotLease.Reset();
                    continue;
                }
            }

            if (!copySucceeded) {
                skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
                poolDropCount_.fetch_add(1, std::memory_order_relaxed);
                slotLease.Reset();
                static std::atomic<uint32_t> conversionFailLogCount{0};
                const uint32_t failLog = conversionFailLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (failLog <= 4) {
                    LogWarn(
                        "[WGC] Retained-copy conversion failed; dropped WGC frame "
                        "(sourceFmt=%s retainedFmt=%s slot=%u generation=%llu frameQpc=%lld)",
                        DxgiFormatName(sourceDesc.Format), DxgiFormatName(poolFormat_), idx,
                        static_cast<unsigned long long>(poolGeneration), static_cast<long long>(sourceFrameQpc));
                }
                return false;
            }

            QueryPerformanceCounter(&copyEnd);

            int64_t copyUs = 0;
            if (qpcFreq_ > 0) {
                copyUs = ((copyEnd.QuadPart - copyStart.QuadPart) * 1000000) / qpcFreq_;
            }
            if (compactRetainedCopy) {
                lastPoolConvertUs_.store(copyUs, std::memory_order_relaxed);
            } else {
                lastPoolConvertUs_.store(0, std::memory_order_relaxed);
            }
            const int64_t previousSlotWriteQpc = poolSlotLastWriteQpc_[idx];
            poolSlotLastWriteQpc_[idx] = copyEnd.QuadPart;
            if (previousSlotWriteQpc > 0 && copyEnd.QuadPart > previousSlotWriteQpc && qpcFreq_ > 0) {
                const int64_t slotRewriteUs = ((copyEnd.QuadPart - previousSlotWriteQpc) * 1000000) / qpcFreq_;
                lastPoolSlotRewriteUs_.store(slotRewriteUs, std::memory_order_relaxed);
                if (slotRewriteUs < 5000) {
                    const uint32_t fastRewrite = poolSlotFastRewriteCount_.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (fastRewrite <= 5) {
                        LogWarn("[WGC] Texture pool slot %u rewritten after %lldus", idx, (long long)slotRewriteUs);
                    }
                }
            }
            lastCopyUs_.store(copyUs, std::memory_order_relaxed);
            const int64_t timingSourceQpc = rawSourceFrameQpc > 0 ? rawSourceFrameQpc : sourceFrameQpc;
            RecordSourceTimingSample(timingSourceQpc);
            RecordSourceToCopyLatency(timingSourceQpc, copyEnd.QuadPart);

            lastCapturedQPC_ = sourceFrameQpc;
            if (targetIntervalQPC_ > 0) {
                if (nextCaptureQPC_ <= 0) {
                    nextCaptureQPC_ = sourceFrameQpc + targetIntervalQPC_;
                } else {
                    do {
                        nextCaptureQPC_ += targetIntervalQPC_;
                    } while (nextCaptureQPC_ <= sourceFrameQpc);
                }
            } else {
                nextCaptureQPC_ = 0;
            }

            texturePool_[idx]->AddRef();
            *out = texturePool_[idx];
            outLease = std::move(slotLease);
            outSlot = idx;
            outGeneration = poolGeneration;
            copyCompleteQpc = copyEnd.QuadPart;
            static std::atomic<uint32_t> copiedLogCount{0};
            const uint32_t copiedLog = copiedLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (copiedLog <= 8 || (copiedLog % 1000u) == 0u) {
                LogInfo(
                    "[WGC] Pool frame copied: slot=%u generation=%llu copyUs=%lld sourceQpc=%lld rawQpc=%lld "
                    "sourceFmt=%s retainedFmt=%s compactRetained=%d staging=%d convertUs=%lld leasedMax=%u freeMin=%u",
                    idx, static_cast<unsigned long long>(poolGeneration), static_cast<long long>(copyUs),
                    static_cast<long long>(sourceFrameQpc), static_cast<long long>(rawSourceFrameQpc),
                    DxgiFormatName(sourceDesc.Format), DxgiFormatName(poolFormat_), compactRetainedCopy ? 1 : 0,
                    usedConversionStaging ? 1 : 0,
                    static_cast<long long>(lastPoolConvertUs_.load(std::memory_order_relaxed)),
                    leaseState ? leaseState->leasedMax.load(std::memory_order_relaxed) : 0u,
                    leaseState ? leaseState->freeMin.load(std::memory_order_relaxed) : 0u);
            }
            return true;
        }

        skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
        poolDropCount_.fetch_add(1, std::memory_order_relaxed);
        poolSaturatedDropCount_.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> contentionLogCount{0};
        if (contentionLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            LogWarn(
                "[WGC] Texture pool saturated: safely dropped WGC frame instead of overwriting a live slot "
                "(copyPoolSlots=%u generation=%llu leasedMax=%u freeMin=%u overwritePrevented=%u frameQpc=%lld)",
                poolSize, static_cast<unsigned long long>(poolGeneration),
                leaseState ? leaseState->leasedMax.load(std::memory_order_relaxed) : 0u,
                leaseState ? leaseState->freeMin.load(std::memory_order_relaxed) : 0u,
                poolSlotOverwritePreventedCount_.load(std::memory_order_relaxed), (long long)sourceFrameQpc);
        }
        return false;
    }

    bool ProcessCapturedFrame(winrt::Direct3D11CaptureFrame winrtFrame, WGCCapturedFrame* outputFrame) {
        if (!winrtFrame) {
            return false;
        }

        if (targetWindow_) {
            if (!IsWindow(targetWindow_)) {
                FlagResetNeeded("target window became invalid");
                winrtFrame.Close();
                return false;
            }
            if (IsIconic(targetWindow_)) {
                FlagResetNeeded("target window minimized");
                winrtFrame.Close();
                return false;
            }
        }

        inputFrameCount_.fetch_add(1, std::memory_order_relaxed);
        RecordInputFrameEvent();

        const int64_t rawSourceFrameQpc = GetFrameSourceQpc(winrtFrame);
        if (IsOutOfOrderRawSourceFrameQpc(rawSourceFrameQpc)) {
            skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
            staleSkipCount_.fetch_add(1, std::memory_order_relaxed);
            staleOutOfOrderTimestampCount_.fetch_add(1, std::memory_order_relaxed);
            winrtFrame.Close();
            return false;
        }

        bool duplicateSourceTimestamp = false;
        const int64_t sourceFrameQpc = NormalizeSourceFrameQpc(rawSourceFrameQpc, &duplicateSourceTimestamp);
        const int64_t lastDeliveredRawSourceQpc = lastDeliveredRawSourceQpc_.load(std::memory_order_relaxed);
        if (ce::capture_policy::ShouldSkipDeliveredDuplicateWgcSourceTimestamp(
                duplicateSourceTimestamp, rawSourceFrameQpc, lastDeliveredRawSourceQpc, targetIntervalQPC_ > 0)) {
            skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
            duplicateTimestampSkipCount_.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<uint32_t> duplicateSkipLogCount{0};
            if (duplicateSkipLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                LogInfo("[WGC] Skipped duplicate source timestamp before copy rawQpc=%lld deliveredRawQpc=%lld",
                        static_cast<long long>(rawSourceFrameQpc), static_cast<long long>(lastDeliveredRawSourceQpc));
            }
            winrtFrame.Close();
            return false;
        }
        if (!duplicateSourceTimestamp && targetIntervalQPC_ > 0 && nextCaptureQPC_ > 0 && sourceFrameQpc > 0 &&
            sourceFrameQpc < nextCaptureQPC_) {
            skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
            pacingSkipCount_.fetch_add(1, std::memory_order_relaxed);
            winrtFrame.Close();
            return false;
        }

        if (throttleFlag_ && throttleFlag_->load(std::memory_order_relaxed)) {
            skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
            throttleSkipCount_.fetch_add(1, std::memory_order_relaxed);
            winrtFrame.Close();
            return false;
        }

        if (!duplicateSourceTimestamp && IsStaleSourceFrameQpc(sourceFrameQpc)) {
            skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
            staleSkipCount_.fetch_add(1, std::memory_order_relaxed);
            const int64_t lastDeliveredSourceQpc = lastDeliveredSourceQpc_.load(std::memory_order_relaxed);
            if (sourceFrameQpc == lastDeliveredSourceQpc) {
                staleDuplicateTimestampCount_.fetch_add(1, std::memory_order_relaxed);
            } else if (sourceFrameQpc < lastDeliveredSourceQpc) {
                staleOutOfOrderTimestampCount_.fetch_add(1, std::memory_order_relaxed);
            }
            winrtFrame.Close();
            return false;
        }

        bool success = false;
        auto surface = winrtFrame.Surface();
        IDirect3DDxgiInterfaceAccess* access = nullptr;

        if (SUCCEEDED(surface.as<IUnknown>()->QueryInterface(IID_IDirect3DDxgiInterfaceAccess, (void**)&access)) &&
            access) {
            ID3D11Texture2D* texture = nullptr;
            if (SUCCEEDED(access->GetInterface(__uuidof(ID3D11Texture2D), (void**)&texture)) && texture) {
                D3D11_TEXTURE2D_DESC desc;
                texture->GetDesc(&desc);

                if (frameWidth_ != 0 && frameHeight_ != 0 &&
                    (desc.Width != frameWidth_ || desc.Height != frameHeight_)) {
                    LogWarn("[WGC] Source size changed from %ux%u to %ux%u", frameWidth_, frameHeight_, desc.Width,
                            desc.Height);
                    FlagResetNeeded("capture size changed");
                    texture->Release();
                    access->Release();
                    winrtFrame.Close();
                    return false;
                }

                if (!formatDetected_) {
                    formatDetected_ = true;
                    LogInfo("[WGC] Source format: fmt=%d %ux%u", desc.Format, desc.Width, desc.Height);
                }

                if (ShouldSkipCursorOnlyDirtyFrame(winrtFrame, desc.Width, desc.Height)) {
                    skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
                    cursorOnlySkipCount_.fetch_add(1, std::memory_order_relaxed);
                    texture->Release();
                    access->Release();
                    winrtFrame.Close();
                    return false;
                }

                ID3D11Texture2D* copiedTexture = nullptr;
                int64_t copyCompleteQpc = 0;
                WgcPoolSlotLease poolLease;
                uint32_t poolSlot = std::numeric_limits<uint32_t>::max();
                uint64_t poolGeneration = 0;
                if (CopyFrameToPool(texture, desc, sourceFrameQpc, rawSourceFrameQpc, &copiedTexture, copyCompleteQpc,
                                    poolLease, poolSlot, poolGeneration)) {
                    const int64_t deliveredTimestamp = sourceFrameQpc > 0 ? sourceFrameQpc : copyCompleteQpc;
                    if (sourceFrameQpc > 0) {
                        lastDeliveredSourceQpc_.store(sourceFrameQpc, std::memory_order_relaxed);
                    }
                    if (rawSourceFrameQpc > 0) {
                        lastDeliveredRawSourceQpc_.store(rawSourceFrameQpc, std::memory_order_relaxed);
                    }
                    RecordDeliveredFrameEvent();
                    RequestHDRRecheckIfDue();

                    int32_t captureLeft = 0;
                    int32_t captureTop = 0;
                    GetCaptureOrigin(captureLeft, captureTop);

                    if (outputFrame) {
                        outputFrame->texture = copiedTexture;
                        outputFrame->width = desc.Width;
                        outputFrame->height = desc.Height;
                        outputFrame->timestamp = deliveredTimestamp;
                        outputFrame->rawTimestamp = rawSourceFrameQpc;
                        outputFrame->isHDR = captureIsHDR_;
                        outputFrame->captureLeft = captureLeft;
                        outputFrame->captureTop = captureTop;
                        outputFrame->duplicateSourceTimestamp = duplicateSourceTimestamp;
                        outputFrame->poolSlot = poolSlot;
                        outputFrame->poolGeneration = poolGeneration;
                        outputFrame->poolLease = std::move(poolLease);
                    } else {
                        auto cb = frameCallback_.load(std::memory_order_acquire);
                        if (cb) {
                            cb(copiedTexture, desc.Width, desc.Height, deliveredTimestamp, rawSourceFrameQpc,
                               captureIsHDR_, duplicateSourceTimestamp, captureLeft, captureTop, std::move(poolLease));
                        } else {
                            SafeRelease(copiedTexture);
                        }
                    }

                    callbackFrameCount_.fetch_add(1, std::memory_order_relaxed);
                    success = true;
                }
                texture->Release();
            }
            access->Release();
        }

        winrtFrame.Close();
        return success;
    }

    void OnFrameArrived(winrt::Direct3D11CaptureFramePool const& sender, winrt::IInspectable const&) {
        // Use global atomic for inflight count - survives Impl destruction
        // to prevent use-after-free on WinRT thread pool callbacks
        g_WgcInflightCallbacks.fetch_add(1, std::memory_order_acq_rel);
        EnsureWgcCallbackThreadQoS();

        LARGE_INTEGER callbackStart = {};
        QueryPerformanceCounter(&callbackStart);
        const int64_t previousCallbackStart =
            lastCallbackStartQpc_.exchange(callbackStart.QuadPart, std::memory_order_relaxed);
        if (previousCallbackStart > 0 && callbackStart.QuadPart > previousCallbackStart && qpcFreq_ > 0) {
            const int64_t gapUs = ((callbackStart.QuadPart - previousCallbackStart) * 1000000) / qpcFreq_;
            UpdateSmoothedAtomicUs(callbackGapAvgUs_, gapUs);
            UpdateAtomicMax(callbackGapMaxUs_, gapUs);
        }

        auto recordCallbackProcess = [&](uint32_t drainedCount) {
            LARGE_INTEGER callbackEnd = {};
            QueryPerformanceCounter(&callbackEnd);
            if (callbackEnd.QuadPart > callbackStart.QuadPart && qpcFreq_ > 0) {
                const int64_t processUs = ((callbackEnd.QuadPart - callbackStart.QuadPart) * 1000000) / qpcFreq_;
                UpdateSmoothedAtomicUs(callbackProcessAvgUs_, processUs);
                UpdateAtomicMax(callbackProcessMaxUs_, processUs);
            }
            UpdateAtomicMax(callbackDrainMaxCount_, drainedCount);
        };

        struct DecrementGuard {
            ~DecrementGuard() {
                g_WgcInflightCallbacks.fetch_sub(1, std::memory_order_acq_rel);
            }
        } decrementGuard;

        // Check if Impl is still alive
        if (!alive_.load(std::memory_order_acquire)) {
            recordCallbackProcess(0);
            return;
        }

        if (NeedsReset()) {
            uint32_t drainedCount = 0;
            while (alive_.load(std::memory_order_acquire)) {
                auto winrtFrame = sender.TryGetNextFrame();
                if (!winrtFrame) {
                    break;
                }
                ++drainedCount;
                winrtFrame.Close();
            }
            recordCallbackProcess(drainedCount);
            return;
        }

        // Pull mode: drain promptly into an internal bounded queue so the
        // encoder thread only performs CFR scheduling. This preserves recent
        // temporal history across callback bursts and avoids coupling source
        // collection to encoder wakeups.
        if (!frameCallback_.load(std::memory_order_acquire)) {
            bool processedFrame = false;
            uint32_t drainedCount = 0;
            std::vector<WGCCapturedFrame> drainedFrames;
            drainedFrames.reserve(4);
            {
                std::lock_guard<std::mutex> processLock(frameProcessingMutex_);
                while (alive_.load(std::memory_order_acquire)) {
                    auto winrtFrame = sender.TryGetNextFrame();
                    if (!winrtFrame) {
                        break;
                    }
                    ++drainedCount;

                    WGCCapturedFrame frame{};
                    if (ProcessCapturedFrame(std::move(winrtFrame), &frame) && frame.texture) {
                        drainedFrames.push_back(std::move(frame));
                        processedFrame = true;
                    }
                }
            }

            if (!drainedFrames.empty()) {
                std::lock_guard<std::mutex> lock(frameMutex_);
                for (auto& frame : drainedFrames) {
                    EnqueueFrameInternal(std::move(frame));
                }
            }

            if (processedFrame && frameArrivedEvent_) {
                SetEvent(frameArrivedEvent_);
            }
            recordCallbackProcess(drainedCount);
            return;
        }

        std::lock_guard<std::mutex> lock(callbackDrainMutex_);
        if (!alive_.load(std::memory_order_acquire)) {
            recordCallbackProcess(0);
            return;
        }

        bool processedFrame = false;
        uint32_t drainedCount = 0;
        {
            std::lock_guard<std::mutex> processLock(frameProcessingMutex_);
            while (alive_.load(std::memory_order_acquire)) {
                auto winrtFrame = sender.TryGetNextFrame();
                if (!winrtFrame) {
                    break;
                }
                ++drainedCount;
                processedFrame = ProcessCapturedFrame(std::move(winrtFrame), nullptr) || processedFrame;
            }
        }

        if (processedFrame && frameArrivedEvent_) {
            SetEvent(frameArrivedEvent_);
        }
        recordCallbackProcess(drainedCount);
    }  // decrementGuard destructor fires here, decrementing inflightCallbacks_

    bool CreateWinRTDevice() {
        // Get DXGI device from D3D11 device
        IDXGIDevice* dxgiDevice = nullptr;
        HRESULT hr = d3dDevice_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
        if (FAILED(hr))
            return false;

        // Create WinRT device interop
        winrt::com_ptr<IInspectable> inspectable;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, inspectable.put());
        dxgiDevice->Release();

        if (FAILED(hr))
            return false;

        winrtDevice_ = inspectable.as<winrt::IDirect3DDevice>();
        return winrtDevice_ != nullptr;
    }

    bool CreateForMonitor(HMONITOR hmon) {
        winrt::com_ptr<IGraphicsCaptureItemInterop> interopFactory;
        winrt::hstring className = L"Windows.Graphics.Capture.GraphicsCaptureItem";
        HRESULT factoryHr = RoGetActivationFactory(reinterpret_cast<HSTRING>(winrt::get_abi(className)),
                                                   IID_IGraphicsCaptureItemInterop, interopFactory.put_void());
        if (FAILED(factoryHr) || !interopFactory) {
            LogError("[WGC] RoGetActivationFactory(CreateForMonitor) failed: 0x%lx", factoryHr);
            return false;
        }

        winrt::GraphicsCaptureItem item{nullptr};
        HRESULT hr =
            interopFactory->CreateForMonitor(hmon, winrt::guid_of<winrt::GraphicsCaptureItem>(), winrt::put_abi(item));

        if (FAILED(hr) || !item) {
            LogError("[WGC] CreateForMonitor failed: 0x%lx", hr);
            return false;
        }

        item_ = item;
        itemClosedToken_ = item_.Closed([this](auto&&, auto&&) {
            LogWarn("[WGC] Capture item closed by OS");
            FlagResetNeeded("capture item closed");
        });
        targetMonitor_ = hmon;
        targetWindow_ = nullptr;
        return true;
    }

    bool CreateForWindow(HWND hwnd) {
        winrt::com_ptr<IGraphicsCaptureItemInterop> interopFactory;
        winrt::hstring className = L"Windows.Graphics.Capture.GraphicsCaptureItem";
        HRESULT factoryHr = RoGetActivationFactory(reinterpret_cast<HSTRING>(winrt::get_abi(className)),
                                                   IID_IGraphicsCaptureItemInterop, interopFactory.put_void());
        if (FAILED(factoryHr) || !interopFactory) {
            LogError("[WGC] RoGetActivationFactory(CreateForWindow) failed: 0x%lx", factoryHr);
            return false;
        }

        winrt::GraphicsCaptureItem item{nullptr};
        HRESULT hr =
            interopFactory->CreateForWindow(hwnd, winrt::guid_of<winrt::GraphicsCaptureItem>(), winrt::put_abi(item));

        if (FAILED(hr) || !item) {
            LogError("[WGC] CreateForWindow failed: 0x%lx", hr);
            return false;
        }

        item_ = item;
        itemClosedToken_ = item_.Closed([this](auto&&, auto&&) {
            LogWarn("[WGC] Window capture item closed by OS");
            FlagResetNeeded("window capture item closed");
        });
        targetWindow_ = hwnd;
        targetMonitor_ = nullptr;
        return true;
    }

    bool StartCapture(uint32_t& width, uint32_t& height, bool captureCursor) {
        if (!item_ || !winrtDevice_)
            return false;

        resetNeeded_.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(resetReasonMutex_);
            resetReason_.clear();
        }

        // Cache QPC frequency for timestamp and throttle calculations
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        qpcFreq_ = freq.QuadPart;
        ApplyFrameThrottleInterval();
        if (targetFps_ > 0 && targetIntervalQPC_ > 0) {
            LogInfo("[WGC] Frame throttle active at %u fps (interval=%lld QPC ticks)", targetFps_,
                    (long long)targetIntervalQPC_);
        }

        auto size = item_.Size();
        width = size.Width;
        height = size.Height;
        frameWidth_ = width;
        frameHeight_ = height;
        UpdateCaptureFormatSelection();
        UpdateSmoothnessBudget(width, height, captureDxgiFormat_, true);
        const DXGI_FORMAT requestedDxgiFormat = captureDxgiFormat_;
        const bool requestedHighPrecision = useHighPrecisionCapture_;
        const bool highPrecisionRequired = requireHighPrecisionCapture_ || captureIsHDR_;
        bool attemptedFp16Fallback = false;
        bool attemptedBgraFallback = false;

        // CRITICAL: Must use CreateFreeThreaded (not Create) because we have no
        // message pump! Create() requires a DispatcherQueue pumping messages for
        // callbacks to fire. CreateFreeThreaded() uses an internal worker thread
        // for callbacks. WGC source buffers are budgeted separately from CE copy
        // slots so delivery bursts and encoder texture lifetime do not fight over
        // one unbounded pool.
        auto tryCreateFramePool = [&](winrt::DirectXPixelFormat format) -> bool {
            try {
                framePool_ = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
                    winrtDevice_, format, static_cast<int32_t>(sourceFramePoolBufferCount_), size);
                return framePool_ != nullptr;
            } catch (const winrt::hresult_error& e) {
                if (format == capturePixelFormat_) {
                    LogWarn("[WGC] Frame pool creation failed for format=%d: 0x%08X", (int)format,
                            (unsigned)e.code().value);
                }
                framePool_ = nullptr;
                return false;
            }
        };

        if (!tryCreateFramePool(capturePixelFormat_)) {
            if (!captureIsHDR_ && capturePixelFormat_ == winrt::DirectXPixelFormat::R10G10B10A2UIntNormalized) {
                // SDR 10-bpc: try FP16 to preserve full 10-bit precision in the
                // captured texture. Explicit 10-bit recording must never fall
                // to BGRA8 silently.
                attemptedFp16Fallback = true;
                capturePixelFormat_ = winrt::DirectXPixelFormat::R16G16B16A16Float;
                captureDxgiFormat_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
                UpdateSmoothnessBudget(width, height, captureDxgiFormat_, true);
                // useHighPrecisionCapture_ stays true
            } else if (highPrecisionRequired) {
                LogError("[WGC] Failed to create required high-precision frame pool for format=%s",
                         DescribeCaptureFormat());
                return false;
            } else {
                attemptedBgraFallback = true;
                capturePixelFormat_ = winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
                captureDxgiFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
                useHighPrecisionCapture_ = false;
                UpdateSmoothnessBudget(width, height, captureDxgiFormat_, true);
            }

            if (!tryCreateFramePool(capturePixelFormat_)) {
                if (capturePixelFormat_ != winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized) {
                    if (!ce::capture_policy::ShouldAllowBgra8WgcFallback(requireHighPrecisionCapture_, captureIsHDR_)) {
                        LogError(
                            "[WGC] Required high-precision frame pool failed after fallback attempts "
                            "(requested=%s finalAttempt=%s)",
                            requestedDxgiFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
                                ? "R16G16B16A16_FLOAT"
                                : (requestedDxgiFormat == DXGI_FORMAT_R10G10B10A2_UNORM ? "R10G10B10A2_UNORM"
                                                                                        : "B8G8R8A8_UNORM"),
                            DescribeCaptureFormat());
                        return false;
                    }
                    attemptedBgraFallback = true;
                    capturePixelFormat_ = winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
                    captureDxgiFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
                    useHighPrecisionCapture_ = false;
                    UpdateSmoothnessBudget(width, height, captureDxgiFormat_, true);
                    if (!tryCreateFramePool(capturePixelFormat_)) {
                        LogError("[WGC] Failed to create frame pool in all capture formats");
                        return false;
                    }
                } else {
                    LogError("[WGC] Failed to create BGRA8 frame pool");
                    return false;
                }
            }
        }
        if (captureDxgiFormat_ != requestedDxgiFormat) {
            const char* requestedFormat =
                requestedDxgiFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
                    ? "R16G16B16A16_FLOAT"
                    : (requestedDxgiFormat == DXGI_FORMAT_R10G10B10A2_UNORM ? "R10G10B10A2_UNORM" : "B8G8R8A8_UNORM");
            LogWarn("[WGC] Frame pool fallback: requested=%s final=%s hdr=%s highPrecision=%s tried(fp16=%d bgra8=%d)",
                    requestedFormat, DescribeCaptureFormat(), captureIsHDR_ ? "YES" : "NO",
                    requestedHighPrecision ? "YES" : "NO", attemptedFp16Fallback ? 1 : 0,
                    attemptedBgraFallback ? 1 : 0);
        }
        LogInfo("[WGC] Frame pool format: %s", DescribeCaptureFormat());

        // Create event for frame arrival signaling (OBS-style immediate wake)
        // Auto-reset event ensures we wake once per signal
        if (!frameArrivedEvent_) {
            frameArrivedEvent_ = CreateEvent(NULL, FALSE, FALSE, NULL);  // Auto-reset
        }

        // Subscribe to FrameArrived - signals event for immediate wake
        // Like OBS, we use callback to trigger processing, but actual work
        // happens on our capture thread to avoid WinRT thread pool issues
        frameArrivedToken_ =
            framePool_.FrameArrived([this](auto&& sender, auto&& args) { OnFrameArrived(sender, args); });

        // Create and start capture session
        session_ = framePool_.CreateCaptureSession(item_);

        // Try to request borderless access and enable border removal (like OBS)
        borderlessCapture_ = false;
        if (session_) {
            if (EnsureBorderlessAccessRequested()) {
                try {
                    session_.IsBorderRequired(false);
                    borderlessCapture_ = true;
                    LogInfo("[WGC] Borderless access granted, border removal enabled");
                } catch (winrt::hresult_error const& e) {
                    borderlessCapture_ = false;
                    LogInfo("[WGC] Borderless access granted but border removal failed: 0x%08lX",
                            static_cast<unsigned long>(e.code().value));
                } catch (...) {
                    borderlessCapture_ = false;
                    LogInfo("[WGC] Borderless access granted but border removal failed");
                }
            } else {
                borderlessCapture_ = false;
                const int accessState = GetBorderlessAccessRequestState();
                if (accessState == kBorderlessAccessDenied) {
                    LogInfo("[WGC] Borderless access denied, keeping default border");
                } else {
                    LogInfo("[WGC] Borderless access unavailable, keeping default border");
                }
            }
        }

        // Configure cursor capture
        try {
            if (session_) {
                session_.IsCursorCaptureEnabled(captureCursor);
                LogInfo("[WGC] Native cursor capture: %s", captureCursor ? "YES" : "NO");
            }
        } catch (...) {
            // Not available on older Windows versions
            LogInfo("[WGC] IsCursorCaptureEnabled not available");
        }

        // Ask WGC to wake no faster than the requested recording cadence so
        // cursor movement cannot drive compositor work far above output FPS.
        ApplyMinUpdateInterval();

        session_.StartCapture();
        LogInfo("[WGC] Capture session started: %dx%d", width, height);
        return true;
    }

    void StopCapture() {
        // Stop WinRT session and frame pool first - prevents new callbacks
        if (session_) {
            session_.Close();
            session_ = nullptr;
        }
        if (framePool_) {
            framePool_.FrameArrived(frameArrivedToken_);
            framePool_.Close();
            framePool_ = nullptr;
        }

        // Wait for any in-flight OnFrameArrived callbacks to finish
        // (WinRT thread pool may still be processing a frame)
        int waitMs = 0;
        while (g_WgcInflightCallbacks.load(std::memory_order_acquire) > 0 && waitMs < 2000) {
            Sleep(1);
            waitMs++;
        }
        if (g_WgcInflightCallbacks.load(std::memory_order_acquire) > 0) {
            LogWarn("[WGC] %d callbacks still in-flight after %dms wait - proceeding with cleanup",
                    g_WgcInflightCallbacks.load(), waitMs);
        }

        // Safe to clear callback now - no more concurrent readers
        frameCallback_.store(nullptr, std::memory_order_release);

        // NOTE: Do NOT null item_ - it's the capture target (monitor) and doesn't
        // change between recordings. StartCapture() needs item_ to exist.

        std::lock_guard<std::mutex> lock(frameMutex_);
        SafeRelease(latestFrame_);
        ReleasePendingFramesLocked();
        ReleaseTexturePool();
        borderlessCapture_ = false;
        frameWidth_ = 0;
        frameHeight_ = 0;

        SafeRelease(cachedTexture_);
        frameReady_ = false;

        // Close the frame arrived event handle
        if (frameArrivedEvent_) {
            CloseHandle(frameArrivedEvent_);
            frameArrivedEvent_ = NULL;
        }

        // Drop idle WGC device state between recordings so desktop capture
        // releases its D3D/WinRT memory footprint instead of keeping standby
        // resources resident until process shutdown.
        winrtDevice_ = nullptr;
        if (d3dContext_) {
            d3dContext_->ClearState();
            d3dContext_->Flush();
        }
        if (d3dDevice_) {
            IDXGIDevice3* dxgiDevice3 = nullptr;
            if (SUCCEEDED(d3dDevice_->QueryInterface(IID_PPV_ARGS(&dxgiDevice3))) && dxgiDevice3) {
                dxgiDevice3->Trim();
                dxgiDevice3->Release();
                LogInfo("[WGC] Trimmed capture-device residency");
            }
        }
        SafeRelease(d3dContext_);
        if (usingDedicatedCaptureDevice_) {
            SafeRelease(d3dDevice_);
        } else {
            d3dDevice_ = nullptr;
        }
    }

    size_t DrainPendingFrames(std::vector<WGCCapturedFrame>& frames, size_t maxFrames) {
        if (!framePool_) {
            return 0;
        }

        MaybePerformDeferredHDRRecheck();
        frames.clear();
        std::lock_guard<std::mutex> lock(frameMutex_);
        while (!pendingFrames_.empty()) {
            frames.push_back(std::move(pendingFrames_.front()));
            pendingFrames_.pop_front();
            if (maxFrames > 0 && frames.size() > maxFrames) {
                WGCCapturedFrame stale = std::move(frames.front());
                frames.erase(frames.begin());
                ReleaseCapturedFrame(stale);
            }
        }

        return frames.size();
    }

    bool GetNextFrame(WGCCapturedFrame& frame) {
        std::vector<WGCCapturedFrame> frames;
        frames.reserve(1);
        if (DrainPendingFrames(frames, 1) == 0) {
            return false;
        }
        frame = std::move(frames.back());
        return frame.texture != nullptr;
    }
#else
    // Stub implementation when WGC headers not available
    ID3D11Device* d3dDevice_ = nullptr;
    ID3D11DeviceContext* d3dContext_ = nullptr;

    bool CreateWinRTDevice() {
        return false;
    }
    bool CreateForMonitor(void*) {
        return false;
    }
    bool CreateForWindow(void*) {
        return false;
    }
    bool StartCapture(uint32_t&, uint32_t&, bool) {
        return false;
    }
    void StopCapture() {}
    bool GetNextFrame(WGCCapturedFrame&) {
        return false;
    }
    bool GetCaptureOrigin(int32_t&, int32_t&) const {
        return false;
    }
#endif
};

WGCCapture::WGCCapture() : impl_(std::make_unique<Impl>()) {}

WGCCapture::~WGCCapture() {
    StopCapture();

    // Release WinRT/COM capture objects before apartment teardown.
    impl_.reset();

#if HAS_WGC
    if (roInitialized_) {
        RoUninitialize();
        roInitialized_ = false;
    }
#endif
}

bool WGCCapture::IsSupported() {
#if HAS_WGC
    // Check if GraphicsCaptureSession is supported
    return winrt::GraphicsCaptureSession::IsSupported();
#else
    return false;
#endif
}

bool WGCCapture::IsHdrOutputColorSpace(int colorSpace) {
    return ::IsHdrOutputColorSpace(static_cast<DXGI_COLOR_SPACE_TYPE>(colorSpace));
}

bool WGCCapture::QueryOutputDesc1ForMonitor(HMONITOR monitor, DXGI_OUTPUT_DESC1& desc1) {
    if (!monitor)
        return false;

    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory)
        return false;

    bool found = false;
    for (UINT adapterIndex = 0; !found; ++adapterIndex) {
        IDXGIAdapter1* adapter = nullptr;
        hr = factory->EnumAdapters1(adapterIndex, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr) || !adapter)
            continue;

        for (UINT outputIndex = 0; !found; ++outputIndex) {
            IDXGIOutput* output = nullptr;
            hr = adapter->EnumOutputs(outputIndex, &output);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(hr) || !output)
                continue;

            DXGI_OUTPUT_DESC outputDesc = {};
            if (SUCCEEDED(output->GetDesc(&outputDesc)) && outputDesc.Monitor == monitor) {
                IDXGIOutput6* output6 = nullptr;
                hr = output->QueryInterface(IID_PPV_ARGS(&output6));
                if (SUCCEEDED(hr) && output6) {
                    found = SUCCEEDED(output6->GetDesc1(&desc1));
                    output6->Release();
                }
            }
            output->Release();
        }
        adapter->Release();
    }

    factory->Release();
    return found;
}

bool WGCCapture::Init(ID3D11Device* device) {
#if HAS_WGC
    if (!device) {
        LogError("[WGC] Init failed: D3D11 device is null");
        return false;
    }
    device_ = device;
    if (!impl_->InitializeDevices(device_)) {
        LogError("[WGC] Failed to initialize capture devices");
        return false;
    }

    // Initialize COM for WinRT
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        LogError("[WGC] RoInitialize failed: 0x%lx", hr);
        return false;
    }
    roInitialized_ = SUCCEEDED(hr);  // Track so we can balance with RoUninitialize

    if (!impl_->CreateWinRTDevice()) {
        LogError("[WGC] Failed to create WinRT device");
        return false;
    }

    // Get primary monitor
    HMONITOR hmon = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    if (!impl_->CreateForMonitor(hmon)) {
        return false;
    }

    initialized_ = true;
    LogInfo("[WGC] Initialized for primary monitor");
    return true;
#else
    LogError("[WGC] Not available - WinRT headers not found");
    return false;
#endif
}

bool WGCCapture::InitForWindow(ID3D11Device* device, void* hwnd) {
#if HAS_WGC
    if (!device) {
        LogError("[WGC] InitForWindow failed: D3D11 device is null");
        return false;
    }
    if (!hwnd) {
        LogError("[WGC] InitForWindow failed: window handle is null");
        return false;
    }
    device_ = device;
    if (!impl_->InitializeDevices(device_)) {
        LogError("[WGC] Failed to initialize capture devices for window capture");
        return false;
    }

    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        LogError("[WGC] RoInitialize failed: 0x%lx", hr);
        return false;
    }
    roInitialized_ = SUCCEEDED(hr);

    if (!impl_->CreateWinRTDevice()) {
        LogError("[WGC] Failed to create WinRT device");
        return false;
    }

    if (!impl_->CreateForWindow((HWND)hwnd)) {
        return false;
    }

    initialized_ = true;
    LogInfo("[WGC] Initialized for window 0x%p", hwnd);
    return true;
#else
    LogError("[WGC] Not available - WinRT headers not found");
    return false;
#endif
}

void WGCCapture::SetCaptureCursor(bool enabled) {
    captureCursor_ = enabled;
}

bool WGCCapture::InitForMonitor(ID3D11Device* device, void* hmonitor) {
#if HAS_WGC
    if (!device) {
        LogError("[WGC] InitForMonitor failed: D3D11 device is null");
        return false;
    }
    if (!hmonitor) {
        LogError("[WGC] InitForMonitor failed: monitor handle is null");
        return false;
    }
    device_ = device;
    if (!impl_->InitializeDevices(device_)) {
        LogError("[WGC] Failed to initialize capture devices for monitor capture");
        return false;
    }

    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        LogError("[WGC] RoInitialize failed: 0x%lx", hr);
        return false;
    }
    roInitialized_ = SUCCEEDED(hr);

    if (!impl_->CreateWinRTDevice()) {
        LogError("[WGC] Failed to create WinRT device");
        return false;
    }

    if (!impl_->CreateForMonitor((HMONITOR)hmonitor)) {
        return false;
    }

    initialized_ = true;
    LogInfo("[WGC] Initialized for monitor 0x%p", hmonitor);
    return true;
#else
    LogError("[WGC] Not available - WinRT headers not found");
    return false;
#endif
}

bool WGCCapture::StartCapture() {
    if (!initialized_)
        return false;

#if HAS_WGC
    if ((!impl_->d3dDevice_ || !impl_->d3dContext_) && !impl_->InitializeDevices(device_)) {
        LogError("[WGC] Failed to rebuild capture devices for restart");
        return false;
    }
    if (!impl_->winrtDevice_ && !impl_->CreateWinRTDevice()) {
        LogError("[WGC] Failed to rebuild WinRT capture device for restart");
        return false;
    }
#endif

    bool result = impl_->StartCapture(width_, height_, captureCursor_);
    if (result) {
        capturing_ = true;
        LogInfo("[WGC] Capture started");
    }
    return result;
}

void WGCCapture::StopCapture() {
    capturing_ = false;
    impl_->StopCapture();
    LogInfo("[WGC] Capture stopped");
}

bool WGCCapture::GetNextFrame(WGCCapturedFrame& frame) {
    if (!capturing_)
        return false;
    return impl_->GetNextFrame(frame);
}

size_t WGCCapture::DrainPendingFrames(std::vector<WGCCapturedFrame>& frames, size_t maxFrames) {
#if HAS_WGC
    if (!capturing_ || !impl_) {
        frames.clear();
        return 0;
    }
    return impl_->DrainPendingFrames(frames, maxFrames);
#else
    (void)maxFrames;
    frames.clear();
    return 0;
#endif
}

bool WGCCapture::GetCaptureOrigin(int32_t& left, int32_t& top) const {
#if HAS_WGC
    if (impl_) {
        return impl_->GetCaptureOrigin(left, top);
    }
#endif
    left = 0;
    top = 0;
    return false;
}

HANDLE WGCCapture::GetFrameArrivedEvent() const {
#if HAS_WGC
    return impl_ ? impl_->frameArrivedEvent_ : NULL;
#else
    return NULL;
#endif
}

void WGCCapture::SetDirectFrameCallback(std::function<void(ID3D11Texture2D*, uint32_t, uint32_t, int64_t, int64_t, bool,
                                                           bool, int32_t, int32_t, WgcPoolSlotLease&&)>
                                            callback) {
#if HAS_WGC
    if (impl_) {
        // Extract the raw function pointer from std::function.
        // Only static/free functions are ever passed (QueueWgcFrame or nullptr).
        Impl::DirectFrameCallbackFn rawPtr = nullptr;
        if (callback) {
            if (auto target = callback.target<Impl::DirectFrameCallbackFn>()) {
                rawPtr = *target;
            }
        }
        impl_->frameCallback_.store(rawPtr, std::memory_order_release);
    }
#endif
}

uint32_t WGCCapture::GetCallbackFrameCount() const {
#if HAS_WGC
    return impl_ ? impl_->callbackFrameCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetInputFrameCount() const {
#if HAS_WGC
    return impl_ ? impl_->inputFrameCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetLastCopyTimeUs() const {
#if HAS_WGC
    return impl_ ? impl_->lastCopyUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetSourceIntervalAvgUs() const {
#if HAS_WGC
    return impl_ ? impl_->sourceIntervalAvgUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetSourceJitterAvgUs() const {
#if HAS_WGC
    return impl_ ? impl_->sourceJitterAvgUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetSourceJitterMaxUs() const {
#if HAS_WGC
    return impl_ ? impl_->sourceJitterMaxUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetSourceToCopyLatencyAvgUs() const {
#if HAS_WGC
    return impl_ ? impl_->sourceToCopyLatencyAvgUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetSourceToCopyLatencyMaxUs() const {
#if HAS_WGC
    return impl_ ? impl_->sourceToCopyLatencyMaxUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetDeliveredRatePerSec() const {
#if HAS_WGC
    return impl_ ? impl_->deliveredRatePerSec_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetDeliveredMin250Fps() const {
#if HAS_WGC
    return impl_ ? impl_->deliveredMin250Fps_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetDeliveredMin500Fps() const {
#if HAS_WGC
    return impl_ ? impl_->deliveredMin500Fps_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetInputMin250Fps() const {
#if HAS_WGC
    return impl_ ? impl_->inputMin250Fps_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetInputMin500Fps() const {
#if HAS_WGC
    return impl_ ? impl_->inputMin500Fps_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPacingSkipCount() const {
#if HAS_WGC
    return impl_ ? impl_->pacingSkipCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetThrottleSkipCount() const {
#if HAS_WGC
    return impl_ ? impl_->throttleSkipCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetStaleSkipCount() const {
#if HAS_WGC
    return impl_ ? impl_->staleSkipCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetStaleDuplicateTimestampCount() const {
#if HAS_WGC
    return impl_ ? impl_->staleDuplicateTimestampCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetStaleOutOfOrderTimestampCount() const {
#if HAS_WGC
    return impl_ ? impl_->staleOutOfOrderTimestampCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetCursorOnlySkipCount() const {
#if HAS_WGC
    return impl_ ? impl_->cursorOnlySkipCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPoolDropCount() const {
#if HAS_WGC
    return impl_ ? impl_->poolDropCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetNormalizedDuplicateTimestampCount() const {
#if HAS_WGC
    return impl_ ? impl_->normalizedDuplicateTimestampCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetDuplicateTimestampSkipCount() const {
#if HAS_WGC
    return impl_ ? impl_->duplicateTimestampSkipCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetKeyedMutexAcquireFailCount() const {
#if HAS_WGC
    return impl_ ? impl_->keyedMutexAcquireFailCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetKeyedMutexReleaseFailCount() const {
#if HAS_WGC
    return impl_ ? impl_->keyedMutexReleaseFailCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSplitDeviceFlushCount() const {
#if HAS_WGC
    return impl_ ? impl_->splitDeviceFlushCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSplitDeviceFlushSkippedCount() const {
#if HAS_WGC
    return impl_ ? impl_->splitDeviceFlushSkippedCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPoolSlotFastRewriteCount() const {
#if HAS_WGC
    return impl_ ? impl_->poolSlotFastRewriteCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetLastPoolSlotRewriteUs() const {
#if HAS_WGC
    return impl_ ? impl_->lastPoolSlotRewriteUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPoolSlotLeasedMaxCount() const {
#if HAS_WGC
    return (impl_ && impl_->poolLeaseState_) ? impl_->poolLeaseState_->leasedMax.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPoolSlotFreeMinCount() const {
#if HAS_WGC
    return (impl_ && impl_->poolLeaseState_) ? impl_->poolLeaseState_->freeMin.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPoolSlotOverwritePreventedCount() const {
#if HAS_WGC
    return impl_ ? impl_->poolSlotOverwritePreventedCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPoolSaturatedDropCount() const {
#if HAS_WGC
    return impl_ ? impl_->poolSaturatedDropCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPoolLeaseMismatchCount() const {
#if HAS_WGC
    return (impl_ && impl_->poolLeaseState_)
               ? impl_->poolLeaseState_->releaseMismatchCount.load(std::memory_order_relaxed)
               : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetCallbackGapAvgUs() const {
#if HAS_WGC
    return impl_ ? impl_->callbackGapAvgUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetCallbackGapMaxUs() const {
#if HAS_WGC
    return impl_ ? impl_->callbackGapMaxUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetCallbackProcessAvgUs() const {
#if HAS_WGC
    return impl_ ? impl_->callbackProcessAvgUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetCallbackProcessMaxUs() const {
#if HAS_WGC
    return impl_ ? impl_->callbackProcessMaxUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetCallbackDrainMaxCount() const {
#if HAS_WGC
    return impl_ ? impl_->callbackDrainMaxCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

bool WGCCapture::IsUsingDedicatedCaptureDevice() const {
#if HAS_WGC
    return impl_ ? impl_->usingDedicatedCaptureDevice_ : false;
#else
    return false;
#endif
}

uint32_t WGCCapture::GetTexturePoolSlotCount() const {
#if HAS_WGC
    return impl_ ? impl_->texturePoolSlotCount_ : ce::capture_policy::kWgcSmoothnessBufferMinPoolFrames;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSourceFramePoolBufferCount() const {
#if HAS_WGC
    return impl_ ? impl_->sourceFramePoolBufferCount_ : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSmoothnessBudgetSurfaceCount() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessBudgetSurfaceCount_ : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSmoothnessSyncFrameCount() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessSyncDelayFrames_ : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSmoothnessSafetySlotCount() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessSafetySlots_ : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSmoothnessRetainedFrameCount() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessRetainedFrames_ : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSmoothnessRetainedFrameCap() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessRetainedFrameCap_ : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSmoothnessReservedFreeSlotCount() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessReservedFreeSlots_ : 0;
#else
    return 0;
#endif
}

uint64_t WGCCapture::GetSmoothnessEstimatedVramBytes() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessEstimatedVramBytes_ : 0;
#else
    return 0;
#endif
}

uint64_t WGCCapture::GetSmoothnessSourceEstimatedVramBytes() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessSourceEstimatedVramBytes_ : 0;
#else
    return 0;
#endif
}

uint64_t WGCCapture::GetSmoothnessCopyEstimatedVramBytes() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessCopyEstimatedVramBytes_ : 0;
#else
    return 0;
#endif
}

uint64_t WGCCapture::GetSmoothnessSourceBytesPerSurface() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessSourceBytesPerSurface_ : 0;
#else
    return 0;
#endif
}

uint64_t WGCCapture::GetSmoothnessCopyBytesPerSurface() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessCopyBytesPerSurface_ : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSmoothnessSourceDxgiFormat() const {
#if HAS_WGC
    return impl_ ? static_cast<uint32_t>(impl_->smoothnessSourceFormat_) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSmoothnessCopyDxgiFormat() const {
#if HAS_WGC
    return impl_ ? static_cast<uint32_t>(impl_->smoothnessCopyFormat_) : 0;
#else
    return 0;
#endif
}

bool WGCCapture::IsCompactRetainedCopyActive() const {
#if HAS_WGC
    return impl_ ? impl_->compactRetainedCopyActive_ : false;
#else
    return false;
#endif
}

int64_t WGCCapture::GetLastPoolConvertTimeUs() const {
#if HAS_WGC
    return impl_ ? impl_->lastPoolConvertUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressAcceptedCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressAcceptedCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressDecimatedCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressDecimatedCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressAcceptedLowWaterCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressAcceptedLowWaterCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressAcceptedRecoveryCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressAcceptedRecoveryCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressAcceptedSourceBelowCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressAcceptedSourceBelowCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressAcceptedHealthyCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressAcceptedHealthyCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressAcceptedUniformPlayoutSoftReserveCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressAcceptedUniformPlayoutSoftReserveCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressAcceptedUniformPlayoutCreditCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressAcceptedUniformPlayoutCreditCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressDecimatedSoftReserveCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressDecimatedSoftReserveCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressDecimatedHardReserveCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressDecimatedHardReserveCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressDecimatedCreditCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressDecimatedCreditCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressSoftReservePressureCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressSoftReservePressureCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressHardReservePressureCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressHardReservePressureCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressRetainedFrameCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressRetainedFrames_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressRetainedFrameCap() const {
#if HAS_WGC
    return impl_ ? impl_->ingressRetainedFrameCap_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressLowWaterFrameCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressLowWaterFrames_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressAdmissionReasonCode() const {
#if HAS_WGC
    return impl_ ? impl_->ingressLastReason_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

void WGCCapture::SetGpuPriority(int priority) {
#if HAS_WGC
    if (!impl_ || !impl_->d3dDevice_) {
        LogWarn("[WGC] SetGpuPriority: no capture device available");
        return;
    }
    priority = std::clamp(priority, -7, 7);
    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT hr = impl_->d3dDevice_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (SUCCEEDED(hr) && dxgiDevice) {
        hr = dxgiDevice->SetGPUThreadPriority(priority);
        if (SUCCEEDED(hr)) {
            LogInfo("[WGC] Set GPU thread priority to %d (dedicated=%d)", priority,
                    impl_->usingDedicatedCaptureDevice_ ? 1 : 0);
        } else {
            LogWarn("[WGC] SetGPUThreadPriority(%d) failed: HR=0x%08lX", priority, (unsigned long)hr);
        }
        dxgiDevice->Release();
    } else {
        LogWarn("[WGC] SetGpuPriority: failed to query IDXGIDevice (HR=0x%08lX)", (unsigned long)hr);
    }
#else
    (void)priority;
#endif
}

void WGCCapture::ResetStats() {
    droppedFrames.store(0, std::memory_order_relaxed);
#if HAS_WGC
    if (impl_) {
        impl_->ResetStats();
    }
#endif
}

void WGCCapture::SetTargetFps(uint32_t fps) {
#if HAS_WGC
    if (impl_) {
        impl_->targetFps_ = fps;
        impl_->ApplyFrameThrottleInterval();
        impl_->ApplyMinUpdateInterval();

        if (fps > 0) {
            if (impl_->targetIntervalQPC_ > 0) {
                LogInfo("[WGC] Frame throttle set to %u fps (interval=%lld QPC ticks)", fps,
                        (long long)impl_->targetIntervalQPC_);
            } else {
                LogInfo("[WGC] Frame throttle armed for %u fps (pending capture start)", fps);
            }
        } else {
            LogInfo("[WGC] Frame throttle disabled");
        }
    }
#endif
}

uint32_t WGCCapture::GetTargetFps() const {
#if HAS_WGC
    return impl_ ? impl_->targetFps_ : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSkippedFrameCount() const {
#if HAS_WGC
    return impl_ ? impl_->skippedFrameCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int32_t WGCCapture::GetInflightCallbackCount() const {
    return g_WgcInflightCallbacks.load(std::memory_order_acquire);
}

bool WGCCapture::IsHighPrecisionSource() const {
#if HAS_WGC
    if (impl_) {
        return impl_->useHighPrecisionCapture_;
    }
#endif
    return false;
}

bool WGCCapture::IsWindowTarget() const {
#if HAS_WGC
    return impl_ && impl_->targetWindow_ != nullptr;
#else
    return false;
#endif
}

bool WGCCapture::IsTargetWindowValid() const {
#if HAS_WGC
    return impl_ && (!impl_->targetWindow_ || IsWindow(impl_->targetWindow_));
#else
    return false;
#endif
}

void WGCCapture::GetTargetIdentity(HWND* hwnd, HMONITOR* hmonitor) const {
#if HAS_WGC
    if (hwnd) {
        *hwnd = impl_ ? impl_->targetWindow_ : nullptr;
    }
    if (hmonitor) {
        *hmonitor = impl_ ? impl_->targetMonitor_ : nullptr;
    }
#else
    if (hwnd) {
        *hwnd = nullptr;
    }
    if (hmonitor) {
        *hmonitor = nullptr;
    }
#endif
}

bool WGCCapture::NeedsReset() const {
#if HAS_WGC
    return impl_ && impl_->NeedsReset();
#else
    return false;
#endif
}

std::string WGCCapture::ConsumeResetReason() {
#if HAS_WGC
    return impl_ ? impl_->ConsumeResetReason() : std::string();
#else
    return {};
#endif
}

void WGCCapture::ForceReset() {
#if HAS_WGC
    capturing_ = false;

    if (impl_) {
        HWND targetWindow = impl_->targetWindow_;
        HMONITOR targetMonitor = impl_->targetMonitor_;
        const bool wasWindowCapture = targetWindow != nullptr;
        const bool wasMonitorCapture = targetMonitor != nullptr && targetWindow == nullptr;
        const bool smoothnessBufferEnabled = impl_->smoothnessBufferEnabled_;
        const uint32_t smoothnessOutputFps = impl_->smoothnessOutputFps_;
        const uint32_t smoothnessMaxMs = impl_->smoothnessMaxMs_;
        const uint32_t smoothnessVramBudgetMb = impl_->smoothnessVramBudgetMb_;

        // Clear the active direct-frame callback before tearing down
        impl_->frameCallback_.store(nullptr, std::memory_order_release);

        // Mark Impl as dead BEFORE destroying - prevents callbacks from accessing freed memory
        impl_->alive_.store(false, std::memory_order_release);

        // Close session first - this stops new callbacks
        if (impl_->session_) {
            impl_->session_.Close();
            impl_->session_ = nullptr;
        }
        if (impl_->framePool_) {
            impl_->framePool_.FrameArrived(impl_->frameArrivedToken_);
            impl_->framePool_.Close();
            impl_->framePool_ = nullptr;
        }

        // Wait for in-flight callbacks to finish BEFORE destroying resources
        int waitMs = 0;
        while (g_WgcInflightCallbacks.load(std::memory_order_acquire) > 0 && waitMs < 5000) {
            Sleep(1);
            waitMs++;
        }

        // Now safe to release textures
        impl_->ReleaseTexturePool();
        SafeRelease(impl_->latestFrame_);

        impl_.reset();
        impl_ = std::make_unique<Impl>();
        impl_->smoothnessBufferEnabled_ = smoothnessBufferEnabled;
        impl_->smoothnessOutputFps_ = smoothnessOutputFps;
        impl_->smoothnessMaxMs_ = smoothnessMaxMs;
        impl_->smoothnessVramBudgetMb_ = smoothnessVramBudgetMb;
        if (!impl_->InitializeDevices(device_)) {
            LogError("[WGC] ForceReset failed to reinitialize capture devices");
            return;
        }

        if (device_) {
            if (!impl_->CreateWinRTDevice()) {
                LogError("[WGC] ForceReset failed to rebuild WinRT device");
                return;
            }
            if (wasWindowCapture && targetWindow) {
                if (!impl_->CreateForWindow(targetWindow)) {
                    LogWarn("[WGC] ForceReset failed to recreate window target");
                }
            } else if (wasMonitorCapture && targetMonitor) {
                if (!impl_->CreateForMonitor(targetMonitor)) {
                    LogWarn("[WGC] ForceReset failed to recreate monitor target");
                }
            }
        }

        LogWarn("[WGC] ForceReset complete - WGC session recreated");
    }
#endif
}

void WGCCapture::SetThrottleFlag(const std::atomic<bool>* flag) {
#if HAS_WGC
    if (impl_) {
        impl_->throttleFlag_ = flag;
    }
#endif
}

void WGCCapture::SetSkipSplitDeviceFlush(bool enabled) {
#if HAS_WGC
    if (impl_) {
        impl_->skipSplitDeviceFlush_ = enabled;
    }
#else
    (void)enabled;
#endif
}

void WGCCapture::SetSameDeviceCapture(bool enabled) {
#if HAS_WGC
    if (impl_) {
        const bool changed = impl_->sameDeviceCapture_ != enabled;
        impl_->sameDeviceCapture_ = enabled;
        if (changed && impl_->d3dDevice_) {
            impl_->FlagResetNeeded("same-device capture option changed");
        }
    }
#else
    (void)enabled;
#endif
}

void WGCCapture::SetRequireHighPrecisionCapture(bool enabled) {
#if HAS_WGC
    if (impl_) {
        const bool changed = impl_->requireHighPrecisionCapture_ != enabled;
        impl_->requireHighPrecisionCapture_ = enabled;
        if (changed && impl_->framePool_) {
            impl_->FlagResetNeeded("high-precision capture requirement changed");
        }
    }
#else
    (void)enabled;
#endif
}

void WGCCapture::SetSmoothnessBufferBudget(bool enabled, uint32_t outputFps, uint32_t maxMs, uint32_t vramBudgetMb,
                                           uint32_t syncDelayFrames) {
#if HAS_WGC
    if (impl_) {
        impl_->smoothnessBufferEnabled_ = enabled;
        impl_->smoothnessOutputFps_ = outputFps;
        impl_->smoothnessMaxMs_ = maxMs;
        impl_->smoothnessVramBudgetMb_ = vramBudgetMb;
        impl_->smoothnessSyncDelayFrames_ = (enabled && syncDelayFrames == 0)
                                                ? ce::capture_policy::GetWgcEstimatedSyncDelayFramesForBudget(outputFps)
                                                : syncDelayFrames;
        LogInfo("[WGC] Smoothness buffer config: enabled=%d outputFps=%u maxMs=%u budget=%uMB syncFrames=%u",
                enabled ? 1 : 0, outputFps, maxMs, vramBudgetMb, impl_->smoothnessSyncDelayFrames_);
    }
#else
    (void)enabled;
    (void)outputFps;
    (void)maxMs;
    (void)vramBudgetMb;
    (void)syncDelayFrames;
#endif
}

void WGCCapture::SetRetainedFramePressure(uint32_t retainedFrames, uint32_t retainedFrameCap, uint32_t lowWaterFrames,
                                          bool recovering, bool uniformPlayoutOwnsSurplus) {
#if HAS_WGC
    if (impl_) {
        const uint32_t effectiveCap = retainedFrameCap > 0 ? retainedFrameCap : impl_->smoothnessRetainedFrameCap_;
        impl_->ingressRetainedFrames_.store(retainedFrames, std::memory_order_relaxed);
        impl_->ingressRetainedFrameCap_.store(effectiveCap, std::memory_order_relaxed);
        impl_->ingressLowWaterFrames_.store(lowWaterFrames, std::memory_order_relaxed);
        impl_->ingressRecovering_.store(recovering, std::memory_order_relaxed);
        impl_->ingressUniformPlayoutOwnsSurplus_.store(uniformPlayoutOwnsSurplus, std::memory_order_relaxed);
    }
#else
    (void)retainedFrames;
    (void)retainedFrameCap;
    (void)lowWaterFrames;
    (void)recovering;
    (void)uniformPlayoutOwnsSurplus;
#endif
}
