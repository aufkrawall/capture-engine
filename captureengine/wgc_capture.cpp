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
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../common/callback_epoch.h"
#include "../common/capture_pipeline_policy.h"
#include "../common/frame_timing_utils.h"
#include "../common/logging.h"
#include "../common/rate_window_utils.h"
#include "../common/thread_power_throttling_compat.h"
#include "../mediaengine/video_format_policy.h"
#include "dxgi_dup_capture.h"
#include "mediaengine_loader.h"

#ifdef _MSC_VER
#pragma comment(lib, "avrt.lib")
#endif

// WinRT/C++WinRT headers for WGC
#include <winrt/base.h>

// Initialize apartment for WinRT
#include <roapi.h>

// Check if actual WGC headers are available
#if __has_include(<winrt/Windows.Graphics.Capture.h>)
#define HAS_WGC 1
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.UI.h>

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

enum class WgcItemCreationMethod {
    kNone,
    kWindowId,
    kInteropWindow,
    kInteropMonitor,
    kDxgiDuplication,
};

const char* WgcItemCreationMethodName(WgcItemCreationMethod method) {
    switch (method) {
        case WgcItemCreationMethod::kWindowId:
            return "WindowId";
        case WgcItemCreationMethod::kInteropWindow:
            return "InteropWindow";
        case WgcItemCreationMethod::kInteropMonitor:
            return "InteropMonitor";
        case WgcItemCreationMethod::kDxgiDuplication:
            return "DxgiDuplication";
        default:
            return "None";
    }
}

enum class WgcItemSourcePreference {
    kAuto,
    kInteropOnly,
};

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

WgcItemSourcePreference GetWgcItemSourcePreference() {
    const char* env = std::getenv("CE_WGC_ITEM_SOURCE");
    if (!env || !*env) {
        return WgcItemSourcePreference::kAuto;
    }

    const std::string value = LowerAscii(env);
    if (value == "interop" || value == "legacy") {
        return WgcItemSourcePreference::kInteropOnly;
    }
    return WgcItemSourcePreference::kAuto;
}

bool ShouldKeepWgcBorderByDiagnosticEnv() {
    const char* env = std::getenv("CE_WGC_BORDER_MODE");
    if (!env || !*env) {
        return false;
    }

    const std::string value = LowerAscii(env);
    return value == "keep" || value == "default" || value == "system" || value == "unchanged";
}

using GetWindowIdFromWindowFn = HRESULT(WINAPI*)(HWND hwnd, winrt::Windows::UI::WindowId* id);

GetWindowIdFromWindowFn ResolveGetWindowIdFromWindow() {
    static GetWindowIdFromWindowFn fn = []() -> GetWindowIdFromWindowFn {
        const wchar_t* modules[] = {L"KernelBase.dll", L"Windows.UI.dll", L"user32.dll"};
        for (const wchar_t* moduleName : modules) {
            HMODULE module = GetModuleHandleW(moduleName);
            if (!module) {
                module = LoadLibraryW(moduleName);
            }
            if (!module) {
                continue;
            }

            auto* proc = reinterpret_cast<GetWindowIdFromWindowFn>(GetProcAddress(module, "GetWindowIdFromWindow"));
            if (proc) {
                LogInfo("[WGC] GetWindowIdFromWindow resolved from %ls", moduleName);
                return proc;
            }
        }
        LogInfo("[WGC] GetWindowIdFromWindow not available; using legacy interop item creation");
        return nullptr;
    }();
    return fn;
}

bool TryCreateCaptureItemFromWindowId(HWND hwnd, winrt::GraphicsCaptureItem& item, uint64_t& windowIdValue,
                                      HRESULT& helperHr, HRESULT& createHr) {
    item = nullptr;
    windowIdValue = 0;
    helperHr = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    createHr = E_FAIL;

    GetWindowIdFromWindowFn getWindowId = ResolveGetWindowIdFromWindow();
    if (!getWindowId) {
        return false;
    }

    winrt::Windows::UI::WindowId windowId{};
    helperHr = getWindowId(hwnd, &windowId);
    windowIdValue = windowId.Value;
    if (FAILED(helperHr) || windowId.Value == 0) {
        return false;
    }

    try {
        item = winrt::GraphicsCaptureItem::TryCreateFromWindowId(windowId);
        createHr = item ? S_OK : S_FALSE;
    } catch (winrt::hresult_error const& e) {
        createHr = static_cast<HRESULT>(e.code().value);
        item = nullptr;
    } catch (...) {
        createHr = E_FAIL;
        item = nullptr;
    }
    return item != nullptr;
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

class WgcCallbackThreadQoS final {
public:
    WgcCallbackThreadQoS() {
        DisableCurrentWgcCallbackThreadPowerThrottling();
        DWORD taskIndex = 0;
        mmcssHandle_ = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
        if (mmcssHandle_) {
            AvSetMmThreadPriority(mmcssHandle_, AVRT_PRIORITY_HIGH);
            LogInfo("[WGC] Callback thread QoS enabled (tid=%lu, task=Capture)", GetCurrentThreadId());
        } else {
            LogWarn("[WGC] Callback thread QoS setup failed (tid=%lu, err=%lu)", GetCurrentThreadId(), GetLastError());
        }
    }

    ~WgcCallbackThreadQoS() {
        if (mmcssHandle_) {
            AvRevertMmThreadCharacteristics(mmcssHandle_);
        }
    }

private:
    HANDLE mmcssHandle_ = nullptr;
};

void EnsureWgcCallbackThreadQoS() {
    // The free-threaded WGC pool reuses thread-pool workers. Keep MMCSS active
    // for the lifetime of each worker, then balance it when that worker exits.
    thread_local WgcCallbackThreadQoS qos;
    (void)qos;
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

LONG RectWidth(const RECT& rect) {
    return std::max<LONG>(0, rect.right - rect.left);
}

LONG RectHeight(const RECT& rect) {
    return std::max<LONG>(0, rect.bottom - rect.top);
}

bool SizeNearlyMatchesRect(uint32_t width, uint32_t height, const RECT& rect, LONG tolerance) {
    auto absDiff = [](int64_t a, int64_t b) -> int64_t { return (a >= b) ? (a - b) : (b - a); };
    return absDiff(static_cast<int64_t>(width), static_cast<int64_t>(RectWidth(rect))) <= tolerance &&
           absDiff(static_cast<int64_t>(height), static_cast<int64_t>(RectHeight(rect))) <= tolerance;
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
    Impl()
        : itemCallbackState_(ce::CallbackEpoch<Impl>::Create()),
          frameCallbackState_(ce::CallbackEpoch<Impl>::Create()) {}

    ~Impl() {
        alive_.store(false, std::memory_order_release);
        StopCapture();
        UnsubscribeItemClosed();
        itemCallbackState_->DetachAndDrain();
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
    std::shared_ptr<ce::CallbackEpoch<Impl>> itemCallbackState_;
    std::shared_ptr<ce::CallbackEpoch<Impl>> frameCallbackState_;
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
    std::atomic<uint64_t> sourceEpoch_{0};

    // Callback function for direct frame processing.
    // Atomic raw function pointer: only static functions (or nullptr) are ever
    // stored, so std::function overhead and its non-atomic nature are avoided.
    // This eliminates the data race between the WinRT callback thread (reader)
    // and the main thread (writer during start/stop recording).
    using DirectFrameCallbackFn = void (*)(ID3D11Texture2D*, uint32_t, uint32_t, int64_t, int64_t, bool, bool, bool,
                                           int32_t, int32_t, uint64_t, WgcPoolSlotLease&&);
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
    int64_t targetIntervalQPC_ = 0;   // Minimum QPC ticks between captured frames (0 = no throttle)
    int64_t lastCapturedQPC_ = 0;     // QPC of last frame we actually copied
    int64_t nextCaptureQPC_ = 0;      // Next QPC deadline that is allowed to perform a GPU copy
    uint32_t producerTargetFps_ = 0;  // WGC MinUpdateInterval target (0 = max-rate)
    int64_t producerIntervalQPC_ = 0;
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
    WgcItemCreationMethod itemCreationMethod_ = WgcItemCreationMethod::kNone;
    uint64_t itemCreationIdValue_ = 0;
    // DXGI Desktop Duplication backend: an alternative monitor-scope frame
    // source behind the same pool/ingress/CFR engine. When the preference is
    // set, StartCapture tries duplication first and falls back to a WGC
    // monitor item in place so recording start never fails on a dup-only
    // problem (rotated/cross-adapter output, format policy, API loss).
    std::unique_ptr<DxgiDuplicationSource> dupSource_;
    bool useDuplicationBackend_ = false;
    std::string dupInitFailureReason_;
    bool useHighPrecisionCapture_ = false;
    bool requireHighPrecisionCapture_ = false;
    // Deferred output rechecks run on the consumer thread while frame delivery
    // reads this flag on the WinRT/duplication callback thread.
    std::atomic<bool> captureIsHDR_{false};
    bool borderlessCapture_ = false;
    UINT outputBitsPerColor_ = 8;
    DXGI_COLOR_SPACE_TYPE outputColorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    std::atomic<bool> resetNeeded_{false};
    bool skipSplitDeviceFlush_ = false;
    bool sameDeviceCapture_ = false;
    bool preferCompact10bitPool_ = true;
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

        const HMONITOR monitor = ResolveTargetMonitor();
        if (!monitor)
            return;

        DXGI_OUTPUT_DESC1 desc1 = {};
        if (QueryOutputDesc1ForMonitor(monitor, desc1)) {
            bool newHDR = ::IsHdrOutputColorSpace(desc1.ColorSpace);
            if (newHDR != captureIsHDR_) {
                // The WGC frame-pool pixel format is immutable. Merely changing
                // the metadata flag would mislabel frames and apply the wrong
                // color conversion; recreate the source/pool on the new mode.
                LogInfo("[WGC] HDR state changed mid-capture: %s -> %s; requesting capture recreation",
                        captureIsHDR_ ? "HDR" : "SDR", newHDR ? "HDR" : "SDR");
                FlagResetNeeded("HDR output state changed");
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
        // The duplication backend has no consumer-owned source frame pool (the
        // OS holds the desktop image), so its entire VRAM budget funds retained
        // copy slots for the smoothness reservoir.
        const bool requiresSourceFramePool = !useDuplicationBackend_;
        return ce::capture_policy::ComputeWgcSmoothnessSurfaceBudget(
            smoothnessBufferEnabled_ ? smoothnessOutputFps_ : 0u, smoothnessBufferEnabled_ ? smoothnessMaxMs_ : 0u,
            width, height, BytesPerPixelForFormat(format), BytesPerPixelForFormat(retainedFormat),
            smoothnessVramBudgetMb_, smoothnessSyncDelayFrames_, requiresSourceFramePool);
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
            const uint32_t budgetFps =
                smoothnessBufferEnabled_ ? ce::capture_policy::GetWgcSmoothnessBudgetFps(smoothnessOutputFps_) : 0;
            const uint32_t desiredFrames = smoothnessBufferEnabled_ ? ce::capture_policy::GetWgcSmoothnessDesiredFrames(
                                                                          smoothnessOutputFps_, smoothnessMaxMs_)
                                                                    : 0;
            const uint32_t capShortfall =
                desiredFrames > smoothnessRetainedFrames_ ? desiredFrames - smoothnessRetainedFrames_ : 0;
            const uint64_t capShortfallBytes = capShortfall * smoothnessCopyBytesPerSurface_;
            LogInfo(
                "[WGC] Smoothness buffer budget: enabled=%d targetMs=%u outputFps=%u budgetFps=%u desiredFrames=%u "
                "retainedFrames=%u sourceFramePoolBuffers=%u copyPoolSlots=%u budgetSurfaces=%u syncFrames=%u "
                "extraFrames=%u retainedCap=%u reservedFreeSlots=%u safetySlots=%u fmt=%d %ux%u bpp=%u budget=%uMB "
                "sourceFmt=%s retainedFmt=%s compactRetained=%d sourceSurfaceMB=%.1f copySurfaceMB=%.1f "
                "sourceBudgetMB=%.1f copyBudgetMB=%.1f estimated=%lluMB capLimited=%d capShortfall=%u "
                "capShortfallMB=%.0f budgetExhausted=%d",
                smoothnessBufferEnabled_ ? 1 : 0, smoothnessMaxMs_, smoothnessOutputFps_, budgetFps, desiredFrames,
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
                budget.capLimited ? 1 : 0, capShortfall,
                static_cast<double>(capShortfallBytes) / (1024.0 * 1024.0), budget.budgetExhausted ? 1 : 0);
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
        ApplyProducerInterval();
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

    void ApplyProducerInterval() {
        if (producerTargetFps_ > 0 && qpcFreq_ > 0) {
            producerIntervalQPC_ = std::max<int64_t>(1, qpcFreq_ / static_cast<int64_t>(producerTargetFps_));
        } else {
            producerIntervalQPC_ = 0;
        }
    }

    void ApplyMinUpdateInterval() {
        if (!session_) {
            return;
        }

        try {
            const int64_t interval100ns =
                producerTargetFps_ > 0 ? std::max<int64_t>(1, 10000000ll / static_cast<int64_t>(producerTargetFps_))
                                       : 0;

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

            if (producerTargetFps_ > 0) {
                LogInfo("[WGC] MinUpdateInterval set to %lld (100ns) for %u fps target", (long long)interval100ns,
                        producerTargetFps_);
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
            const char* originMode = ResolveWindowCaptureOrigin(left, top);
            return originMode != nullptr;
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

    const char* ResolveWindowCaptureOrigin(int32_t& left, int32_t& top) const {
        left = 0;
        top = 0;
        if (!targetWindow_ || !IsWindow(targetWindow_)) {
            return nullptr;
        }

        RECT windowRect = {};
        RECT clientRect = {};
        const bool haveWindowRect = GetWindowRect(targetWindow_, &windowRect) != FALSE;
        const bool haveClientRect = GetWindowClientRectInScreen(targetWindow_, clientRect);
        constexpr LONG kOriginSizeTolerancePx = 8;

        if (frameWidth_ > 0 && frameHeight_ > 0) {
            if (haveClientRect && SizeNearlyMatchesRect(frameWidth_, frameHeight_, clientRect, kOriginSizeTolerancePx)) {
                left = clientRect.left;
                top = clientRect.top;
                return "client-size-match";
            }

            if (haveWindowRect && SizeNearlyMatchesRect(frameWidth_, frameHeight_, windowRect, kOriginSizeTolerancePx)) {
                left = windowRect.left;
                top = windowRect.top;
                return "window-size-match";
            }
        }

        if (haveWindowRect) {
            left = windowRect.left;
            top = windowRect.top;
            return "window-fallback";
        }

        if (haveClientRect) {
            left = clientRect.left;
            top = clientRect.top;
            return "client-fallback";
        }

        return nullptr;
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

    // Shared source-agnostic frame admission (timestamp ordering, duplicate
    // normalization, pacing, external throttle, staleness). Used by both the
    // WGC frame-pool path and the DXGI duplication path so every skip counter
    // and cadence policy behaves identically for both backends.
    struct SourceFramePreflight {
        int64_t rawSourceFrameQpc = 0;
        int64_t sourceFrameQpc = 0;
        bool duplicateSourceTimestamp = false;
        bool accepted = false;
    };

    SourceFramePreflight PreflightSourceFrame(int64_t rawSourceFrameQpc) {
        SourceFramePreflight pre;
        pre.rawSourceFrameQpc = rawSourceFrameQpc;

        inputFrameCount_.fetch_add(1, std::memory_order_relaxed);
        RecordInputFrameEvent();

        if (IsOutOfOrderRawSourceFrameQpc(rawSourceFrameQpc)) {
            skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
            staleSkipCount_.fetch_add(1, std::memory_order_relaxed);
            staleOutOfOrderTimestampCount_.fetch_add(1, std::memory_order_relaxed);
            return pre;
        }

        pre.sourceFrameQpc = NormalizeSourceFrameQpc(rawSourceFrameQpc, &pre.duplicateSourceTimestamp);
        const int64_t lastDeliveredRawSourceQpc = lastDeliveredRawSourceQpc_.load(std::memory_order_relaxed);
        if (pre.duplicateSourceTimestamp ||
            ce::capture_policy::ShouldSkipDeliveredDuplicateWgcSourceTimestamp(
                pre.duplicateSourceTimestamp, rawSourceFrameQpc, lastDeliveredRawSourceQpc, targetIntervalQPC_ > 0)) {
            skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
            duplicateTimestampSkipCount_.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<uint32_t> duplicateSkipLogCount{0};
            if (duplicateSkipLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                const int64_t lastDeliveredSourceQpc = lastDeliveredSourceQpc_.load(std::memory_order_relaxed);
                LogInfo(
                    "[WGC] Skipped duplicate/out-of-order source frame before copy: "
                    "rawQpc=%lld dupTs=%d lastDeliveredRawQpc=%lld lastDeliveredNormQpc=%lld",
                    static_cast<long long>(rawSourceFrameQpc), pre.duplicateSourceTimestamp ? 1 : 0,
                    static_cast<long long>(lastDeliveredRawSourceQpc),
                    static_cast<long long>(lastDeliveredSourceQpc));
            }
            return pre;
        }
        if (!pre.duplicateSourceTimestamp && targetIntervalQPC_ > 0 && nextCaptureQPC_ > 0 &&
            pre.sourceFrameQpc > 0 && pre.sourceFrameQpc < nextCaptureQPC_) {
            skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
            pacingSkipCount_.fetch_add(1, std::memory_order_relaxed);
            return pre;
        }

        if (throttleFlag_ && throttleFlag_->load(std::memory_order_relaxed)) {
            skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
            throttleSkipCount_.fetch_add(1, std::memory_order_relaxed);
            return pre;
        }

        if (!pre.duplicateSourceTimestamp && IsStaleSourceFrameQpc(pre.sourceFrameQpc)) {
            skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
            staleSkipCount_.fetch_add(1, std::memory_order_relaxed);
            const int64_t lastDeliveredSourceQpc = lastDeliveredSourceQpc_.load(std::memory_order_relaxed);
            if (pre.sourceFrameQpc == lastDeliveredSourceQpc) {
                staleDuplicateTimestampCount_.fetch_add(1, std::memory_order_relaxed);
            } else if (pre.sourceFrameQpc < lastDeliveredSourceQpc) {
                staleOutOfOrderTimestampCount_.fetch_add(1, std::memory_order_relaxed);
            }
            return pre;
        }

        pre.accepted = true;
        return pre;
    }

    // Shared pool copy + delivery for an admitted source texture. The texture
    // only needs to stay valid for the duration of this call (the GPU copy is
    // submitted synchronously), which is exactly the DXGI duplication
    // Acquire/Release contract as well as the WinRT surface lifetime.
    bool DeliverSourceTexture(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& desc,
                              const SourceFramePreflight& pre, WGCCapturedFrame* outputFrame) {
        // Snapshot identity before any GPU work. If the coordinator advances
        // this already-warmed capture during an inject->WGC commit, an in-flight
        // pre-commit frame must retain the retired epoch.
        const uint64_t sourceEpoch = sourceEpoch_.load(std::memory_order_acquire);
        if (frameWidth_ != 0 && frameHeight_ != 0 && (desc.Width != frameWidth_ || desc.Height != frameHeight_)) {
            LogWarn("[WGC] Source size changed from %ux%u to %ux%u", frameWidth_, frameHeight_, desc.Width,
                    desc.Height);
            FlagResetNeeded("capture size changed");
            return false;
        }

        if (!formatDetected_) {
            formatDetected_ = true;
            LogInfo("[WGC] Source format: fmt=%d %ux%u", desc.Format, desc.Width, desc.Height);
        }

        ID3D11Texture2D* copiedTexture = nullptr;
        int64_t copyCompleteQpc = 0;
        WgcPoolSlotLease poolLease;
        uint32_t poolSlot = std::numeric_limits<uint32_t>::max();
        uint64_t poolGeneration = 0;
        if (!CopyFrameToPool(texture, desc, pre.sourceFrameQpc, pre.rawSourceFrameQpc, &copiedTexture, copyCompleteQpc,
                             poolLease, poolSlot, poolGeneration)) {
            return false;
        }

        const int64_t deliveredTimestamp = pre.sourceFrameQpc > 0 ? pre.sourceFrameQpc : copyCompleteQpc;
        if (pre.sourceFrameQpc > 0) {
            lastDeliveredSourceQpc_.store(pre.sourceFrameQpc, std::memory_order_relaxed);
        }
        if (pre.rawSourceFrameQpc > 0) {
            lastDeliveredRawSourceQpc_.store(pre.rawSourceFrameQpc, std::memory_order_relaxed);
        }
        RecordDeliveredFrameEvent();
        RequestHDRRecheckIfDue();

        int32_t captureLeft = 0;
        int32_t captureTop = 0;
        GetCaptureOrigin(captureLeft, captureTop);
        const bool cursorEmbedded =
            useDuplicationBackend_ && dupSource_ && dupSource_->IsCursorEmbeddedInFrames();

        if (outputFrame) {
            outputFrame->texture = copiedTexture;
            outputFrame->width = desc.Width;
            outputFrame->height = desc.Height;
            outputFrame->timestamp = deliveredTimestamp;
            outputFrame->rawTimestamp = pre.rawSourceFrameQpc;
            outputFrame->isHDR = captureIsHDR_;
            outputFrame->cursorEmbedded = cursorEmbedded;
            outputFrame->captureLeft = captureLeft;
            outputFrame->captureTop = captureTop;
            outputFrame->duplicateSourceTimestamp = pre.duplicateSourceTimestamp;
            outputFrame->sourceEpoch = sourceEpoch;
            outputFrame->poolSlot = poolSlot;
            outputFrame->poolGeneration = poolGeneration;
            outputFrame->poolLease = std::move(poolLease);
        } else {
            auto cb = frameCallback_.load(std::memory_order_acquire);
            if (cb) {
                cb(copiedTexture, desc.Width, desc.Height, deliveredTimestamp, pre.rawSourceFrameQpc, captureIsHDR_,
                   cursorEmbedded, pre.duplicateSourceTimestamp, captureLeft, captureTop, sourceEpoch,
                   std::move(poolLease));
            } else {
                SafeRelease(copiedTexture);
            }
        }

        callbackFrameCount_.fetch_add(1, std::memory_order_relaxed);
        return true;
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

        const SourceFramePreflight pre = PreflightSourceFrame(GetFrameSourceQpc(winrtFrame));
        if (!pre.accepted) {
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
                success = DeliverSourceTexture(texture, desc, pre, outputFrame);
                texture->Release();
            }
            access->Release();
        }

        winrtFrame.Close();
        return success;
    }

    // DXGI duplication sink: mirrors OnFrameArrived's locking, instrumentation,
    // and pull/callback dual-mode dispatch for the duplication capture thread.
    void OnDuplicationFrame(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& desc, int64_t rawSourceQpc) {
        LARGE_INTEGER callbackStart = {};
        QueryPerformanceCounter(&callbackStart);
        const int64_t previousCallbackStart =
            lastCallbackStartQpc_.exchange(callbackStart.QuadPart, std::memory_order_relaxed);
        if (previousCallbackStart > 0 && callbackStart.QuadPart > previousCallbackStart && qpcFreq_ > 0) {
            const int64_t gapUs = ((callbackStart.QuadPart - previousCallbackStart) * 1000000) / qpcFreq_;
            UpdateSmoothedAtomicUs(callbackGapAvgUs_, gapUs);
            UpdateAtomicMax(callbackGapMaxUs_, gapUs);
        }

        auto recordCallbackProcess = [&]() {
            LARGE_INTEGER callbackEnd = {};
            QueryPerformanceCounter(&callbackEnd);
            if (callbackEnd.QuadPart > callbackStart.QuadPart && qpcFreq_ > 0) {
                const int64_t processUs = ((callbackEnd.QuadPart - callbackStart.QuadPart) * 1000000) / qpcFreq_;
                UpdateSmoothedAtomicUs(callbackProcessAvgUs_, processUs);
                UpdateAtomicMax(callbackProcessMaxUs_, processUs);
            }
            UpdateAtomicMax(callbackDrainMaxCount_, 1u);
        };

        if (!alive_.load(std::memory_order_acquire) || NeedsReset()) {
            recordCallbackProcess();
            return;
        }

        bool processed = false;
        if (!frameCallback_.load(std::memory_order_acquire)) {
            // Pull mode: enqueue into the internal bounded queue like WGC.
            WGCCapturedFrame frame{};
            {
                std::lock_guard<std::mutex> processLock(frameProcessingMutex_);
                const SourceFramePreflight pre = PreflightSourceFrame(rawSourceQpc);
                if (pre.accepted) {
                    processed = DeliverSourceTexture(texture, desc, pre, &frame) && frame.texture;
                }
            }
            if (processed) {
                std::lock_guard<std::mutex> lock(frameMutex_);
                EnqueueFrameInternal(std::move(frame));
            }
        } else {
            std::lock_guard<std::mutex> drainLock(callbackDrainMutex_);
            if (!alive_.load(std::memory_order_acquire)) {
                recordCallbackProcess();
                return;
            }
            std::lock_guard<std::mutex> processLock(frameProcessingMutex_);
            const SourceFramePreflight pre = PreflightSourceFrame(rawSourceQpc);
            if (pre.accepted) {
                processed = DeliverSourceTexture(texture, desc, pre, nullptr);
            }
        }

        if (processed && frameArrivedEvent_) {
            SetEvent(frameArrivedEvent_);
        }
        recordCallbackProcess();
    }

    void OnFrameArrived(winrt::Direct3D11CaptureFramePool const& sender, winrt::IInspectable const&) {
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
    }

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

    void UnsubscribeItemClosed() noexcept {
        itemCallbackState_->StopAndDrain();
        if (!item_) {
            itemClosedToken_ = {};
            return;
        }
        try {
            item_.Closed(itemClosedToken_);
        } catch (const winrt::hresult_error& e) {
            LogWarn("[WGC] Failed to unsubscribe capture-item Closed handler: 0x%08lX",
                    static_cast<unsigned long>(e.code().value));
        } catch (...) {
            LogWarn("[WGC] Failed to unsubscribe capture-item Closed handler");
        }
        itemClosedToken_ = {};
    }

    bool SubscribeItemClosed(const char* targetName, const char* resetReason) {
        auto callbackState = itemCallbackState_;
        const uint64_t callbackEpoch = itemCallbackState_->Begin(this);
        try {
            itemClosedToken_ = item_.Closed(
                [callbackState = std::move(callbackState), callbackEpoch, targetName, resetReason](auto&&, auto&&) {
                    auto owner = callbackState->Enter(callbackEpoch);
                    if (!owner) {
                        return;
                    }
                    LogWarn("[WGC] %s capture item closed by OS", targetName);
                    owner->FlagResetNeeded(resetReason);
                });
            return true;
        } catch (const winrt::hresult_error& e) {
            LogError("[WGC] Failed to subscribe %s capture-item Closed handler: 0x%08lX", targetName,
                     static_cast<unsigned long>(e.code().value));
        } catch (...) {
            LogError("[WGC] Failed to subscribe %s capture-item Closed handler", targetName);
        }
        itemCallbackState_->StopAndDrain();
        itemClosedToken_ = {};
        return false;
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

        UnsubscribeItemClosed();
        item_ = item;
        if (!SubscribeItemClosed("Monitor", "capture item closed")) {
            item_ = nullptr;
            return false;
        }
        targetMonitor_ = hmon;
        targetWindow_ = nullptr;
        useDuplicationBackend_ = false;
        itemCreationMethod_ = WgcItemCreationMethod::kInteropMonitor;
        itemCreationIdValue_ = 0;
        LogInfo("[WGC] Capture item created: target=monitor method=%s hmon=0x%p size=%dx%d",
                WgcItemCreationMethodName(itemCreationMethod_), hmon, item_.Size().Width, item_.Size().Height);
        return true;
    }

    bool CreateForWindow(HWND hwnd) {
        if (GetWgcItemSourcePreference() != WgcItemSourcePreference::kInteropOnly) {
            winrt::GraphicsCaptureItem item{nullptr};
            uint64_t windowIdValue = 0;
            HRESULT helperHr = E_FAIL;
            HRESULT createHr = E_FAIL;
            if (TryCreateCaptureItemFromWindowId(hwnd, item, windowIdValue, helperHr, createHr)) {
                UnsubscribeItemClosed();
                item_ = item;
                if (!SubscribeItemClosed("Window", "window capture item closed")) {
                    item_ = nullptr;
                    return false;
                }
                targetWindow_ = hwnd;
                targetMonitor_ = nullptr;
                useDuplicationBackend_ = false;
                itemCreationMethod_ = WgcItemCreationMethod::kWindowId;
                itemCreationIdValue_ = windowIdValue;
                LogInfo("[WGC] Capture item created: target=window method=%s hwnd=0x%p windowId=0x%llx size=%dx%d",
                        WgcItemCreationMethodName(itemCreationMethod_), hwnd,
                        static_cast<unsigned long long>(itemCreationIdValue_), item_.Size().Width, item_.Size().Height);
                return true;
            }

            LogInfo("[WGC] WindowId capture item creation unavailable/failed for hwnd=0x%p "
                    "(helper=0x%08lX create=0x%08lX id=0x%llx); falling back to interop",
                    hwnd, static_cast<unsigned long>(helperHr), static_cast<unsigned long>(createHr),
                    static_cast<unsigned long long>(windowIdValue));
        } else {
            LogInfo("[WGC] WindowId capture item creation skipped by CE_WGC_ITEM_SOURCE=interop");
        }

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

        UnsubscribeItemClosed();
        item_ = item;
        if (!SubscribeItemClosed("Window", "window capture item closed")) {
            item_ = nullptr;
            return false;
        }
        targetWindow_ = hwnd;
        targetMonitor_ = nullptr;
        useDuplicationBackend_ = false;
        itemCreationMethod_ = WgcItemCreationMethod::kInteropWindow;
        itemCreationIdValue_ = 0;
        LogInfo("[WGC] Capture item created: target=window method=%s hwnd=0x%p size=%dx%d",
                WgcItemCreationMethodName(itemCreationMethod_), hwnd, item_.Size().Width, item_.Size().Height);
        return true;
    }

    // Prepare a DXGI Desktop Duplication monitor target. Only light
    // availability validation happens here (adapter/output match, rotation);
    // the duplication object itself is created at StartCapture so an idle
    // primed target does not keep system-wide desktop duplication active
    // (which would suppress MPO/DirectFlip between recordings).
    bool CreateForMonitorDuplication(HMONITOR hmon) {
        if (!d3dDevice_) {
            LogError("[WGC] Duplication target rejected: no capture D3D11 device");
            return false;
        }
        if (!hmon) {
            hmon = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
        }

        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        HRESULT hr = d3dDevice_->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (SUCCEEDED(hr) && dxgiDevice) {
            hr = dxgiDevice->GetAdapter(&adapter);
        }
        SafeRelease(dxgiDevice);
        if (FAILED(hr) || !adapter) {
            LogWarn("[WGC] Duplication target rejected: cannot resolve capture adapter (0x%08lX)",
                    static_cast<unsigned long>(hr));
            return false;
        }

        bool outputFound = false;
        bool rotationOk = true;
        DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
        for (UINT i = 0;; ++i) {
            IDXGIOutput* output = nullptr;
            if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (!output) {
                continue;
            }
            DXGI_OUTPUT_DESC desc = {};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == hmon) {
                outputFound = true;
                rotation = desc.Rotation;
                rotationOk =
                    desc.Rotation == DXGI_MODE_ROTATION_IDENTITY || desc.Rotation == DXGI_MODE_ROTATION_UNSPECIFIED;
                SafeRelease(output);
                break;
            }
            SafeRelease(output);
        }
        SafeRelease(adapter);

        if (!outputFound) {
            LogWarn("[WGC] Duplication target rejected: monitor 0x%p not on capture adapter (cross-adapter output)",
                    hmon);
            return false;
        }
        if (!rotationOk) {
            LogWarn("[WGC] Duplication target rejected: monitor 0x%p output rotation=%d unsupported by duplication",
                    hmon, static_cast<int>(rotation));
            return false;
        }

        UnsubscribeItemClosed();
        item_ = nullptr;
        targetMonitor_ = hmon;
        targetWindow_ = nullptr;
        useDuplicationBackend_ = true;
        dupInitFailureReason_.clear();
        itemCreationMethod_ = WgcItemCreationMethod::kDxgiDuplication;
        itemCreationIdValue_ = 0;
        LogInfo("[WGC] Capture item created: target=monitor method=%s hmon=0x%p (duplication deferred to start)",
                WgcItemCreationMethodName(itemCreationMethod_), hmon);
        return true;
    }

    bool StartDuplicationCapture(uint32_t& width, uint32_t& height) {
        resetNeeded_.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(resetReasonMutex_);
            resetReason_.clear();
        }

        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        qpcFreq_ = freq.QuadPart;
        ApplyFrameThrottleInterval();
        ApplyProducerInterval();
        if (targetFps_ > 0 && targetIntervalQPC_ > 0) {
            LogInfo("[WGC] Frame throttle active at %u fps (interval=%lld QPC ticks)", targetFps_,
                    (long long)targetIntervalQPC_);
        }
        if (producerTargetFps_ > 0) {
            LogInfo(
                "[WGC] Producer cadence target %u fps requested; DXGI duplication has no producer cadence "
                "control (frames arrive at desktop update rate, surplus is decimated by ingress policy)",
                producerTargetFps_);
        }

        // Probe the monitor for HDR/bit-depth so the format policy, retained
        // pool conversion, and encoder 10-bit resolution behave exactly like
        // the WGC backend.
        UpdateCaptureFormatSelection();

        dupSource_ = std::make_unique<DxgiDuplicationSource>();
        std::string failureReason;
        if (!dupSource_->Init(d3dDevice_, targetMonitor_, requireHighPrecisionCapture_, captureIsHDR_,
                              &failureReason)) {
            dupInitFailureReason_ = failureReason;
            dupSource_.reset();
            return false;
        }

        width = dupSource_->GetWidth();
        height = dupSource_->GetHeight();
        frameWidth_ = width;
        frameHeight_ = height;
        targetMonitor_ = dupSource_->GetMonitor();
        // Track the actual desktop surface format for pool sizing/telemetry.
        captureDxgiFormat_ = dupSource_->GetFormat();
        UpdateSmoothnessBudget(width, height, captureDxgiFormat_, true);

        if (!frameArrivedEvent_) {
            frameArrivedEvent_ = CreateEvent(NULL, FALSE, FALSE, NULL);  // Auto-reset
        }

        DxgiDuplicationFrameSink sink;
        sink.onFrame = [this](ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& desc, int64_t rawSourceQpc,
                              uint32_t /*accumulatedFrames*/) { OnDuplicationFrame(texture, desc, rawSourceQpc); };
        sink.onCursorOnlyUpdate = [this]() { cursorOnlySkipCount_.fetch_add(1, std::memory_order_relaxed); };
        sink.onResetNeeded = [this](const char* reason) { FlagResetNeeded(reason); };
        if (!dupSource_->Start(std::move(sink))) {
            dupInitFailureReason_ = "duplication capture thread start failed";
            dupSource_.reset();
            return false;
        }

        LogInfo(
            "[WGC] Capture session diagnostics: target=monitor method=%s hwnd=0x0 hmon=0x%p itemId=0x0 "
            "framePool=DxgiDuplication sourceBuffers=0 borderless=1 nativeCursorRequested=0 "
            "producerTargetFps=%u localThrottleFps=%u format=%s",
            WgcItemCreationMethodName(itemCreationMethod_), targetMonitor_, producerTargetFps_, targetFps_,
            DescribeCaptureFormat());

        int32_t originLeft = 0;
        int32_t originTop = 0;
        const bool originOk = GetCaptureOrigin(originLeft, originTop);
        LogInfo("[WGC] Capture origin diagnostics: target=monitor originMode=%s origin=(%d,%d) itemSize=%ux%u",
                originOk ? "monitor" : "unresolved", originLeft, originTop, frameWidth_, frameHeight_);

        LogInfo("[WGC] Capture session started (DXGI duplication): %dx%d", width, height);
        return true;
    }

    bool StartCapture(uint32_t& width, uint32_t& height, bool captureCursor) {
        if (useDuplicationBackend_) {
            if (StartDuplicationCapture(width, height)) {
                return true;
            }
            LogWarn("[WGC] DXGI duplication backend unavailable (%s); falling back to WGC monitor capture",
                    dupInitFailureReason_.empty() ? "unknown reason" : dupInitFailureReason_.c_str());
            useDuplicationBackend_ = false;
            if (!winrtDevice_ && !CreateWinRTDevice()) {
                LogError("[WGC] Failed to create WinRT device for duplication fallback");
                return false;
            }
            if (!item_ && !CreateForMonitor(ResolveTargetMonitor())) {
                LogError("[WGC] Failed to create WGC monitor item for duplication fallback");
                return false;
            }
        }

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
        ApplyProducerInterval();
        if (targetFps_ > 0 && targetIntervalQPC_ > 0) {
            LogInfo("[WGC] Frame throttle active at %u fps (interval=%lld QPC ticks)", targetFps_,
                    (long long)targetIntervalQPC_);
        }
        if (producerTargetFps_ > 0 && producerIntervalQPC_ > 0) {
            LogInfo("[WGC] Producer cadence target active at %u fps (interval=%lld QPC ticks)", producerTargetFps_,
                    (long long)producerIntervalQPC_);
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
                // R10 frame pools are rejected by the WinRT capture layer itself
                // (E_INVALIDARG regardless of buffer count or driver; WGC only
                // supports the BGRA8/FP16 family). One attempt is kept above for
                // future Windows versions; no buffer-count retry ladder.
                bool resolved = false;
                if (preferCompact10bitPool_) {
                    // BGRA8 as a compact pool format (4bpp instead of FP16's
                    // 8bpp), halving VRAM per source surface. Encoding stays
                    // 10-bit P010 (8-bit source content, transparent upconvert).
                    LogInfo("[WGC] R10 frame pool unsupported by WGC (API-level), "
                            "trying compact BGRA8 pool (wgc_prefer_compact_10bit_pool=true)");
                    const winrt::DirectXPixelFormat bgraFormat = winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
                    const DXGI_FORMAT bgraDxgi = DXGI_FORMAT_B8G8R8A8_UNORM;
                    if (tryCreateFramePool(bgraFormat)) {
                        LogInfo("[WGC] Compact BGRA8 frame pool created successfully. "
                                "Encoding stays 10-bit P010. VRAM saved: ~%.0fMB per source surface vs FP16.",
                                static_cast<double>(BytesPerPixelForFormat(DXGI_FORMAT_R16G16B16A16_FLOAT) -
                                                    BytesPerPixelForFormat(bgraDxgi)) *
                                    static_cast<double>(width) * static_cast<double>(height) / (1024.0 * 1024.0));
                        capturePixelFormat_ = bgraFormat;
                        captureDxgiFormat_ = bgraDxgi;
                        // Keep useHighPrecisionCapture_=true so encoding stays
                        // 10-bit P010.
                        resolved = true;
                    } else {
                        LogInfo("[WGC] Compact BGRA8 pool also failed, falling back to FP16.");
                    }
                }
                if (!resolved) {
                    LogWarn(
                        "[WGC] R10 frame pool unsupported by WGC (API-level), falling back to FP16. "
                        "FP16 at 2x VRAM cost per source surface preserves >8-bit source content losslessly "
                        "via shader conversion to R10 for retained copies.");
                    // SDR 10-bpc: FP16 preserves full >8-bit source content
                    // (e.g. a 10-bit game swapchain re-composed into the pool).
                    // Explicit 10-bit recording must never fall to BGRA8
                    // silently.
                    attemptedFp16Fallback = true;
                    capturePixelFormat_ = winrt::DirectXPixelFormat::R16G16B16A16Float;
                    captureDxgiFormat_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    UpdateSmoothnessBudget(width, height, captureDxgiFormat_, true);
                    if (smoothnessRetainedFrames_ > 0 &&
                        smoothnessRetainedFrames_ < ce::capture_policy::GetWgcSmoothnessDesiredFrames(
                                                         smoothnessOutputFps_, smoothnessMaxMs_)) {
                        const uint32_t desiredFrames = ce::capture_policy::GetWgcSmoothnessDesiredFrames(
                            smoothnessOutputFps_, smoothnessMaxMs_);
                        const uint32_t shortfall = desiredFrames - smoothnessRetainedFrames_;
                        const uint64_t neededBytes = static_cast<uint64_t>(shortfall) * smoothnessCopyBytesPerSurface_;
                        LogWarn(
                            "[WGC] FP16 pool cap-limited: shortfall=%u/%u frames (need ~%.0fMB more "
                            "copy VRAM or reduce source buffers). "
                            "Expected smoothness degraded: reservoir may starve under source dips.",
                            shortfall, desiredFrames, static_cast<double>(neededBytes) / (1024.0 * 1024.0));
                    }
                    // useHighPrecisionCapture_ stays true
                }
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

        // A queued WinRT callback retains only this shared epoch gate. Stop
        // invalidates the epoch before releasing capture resources, so a
        // handler that starts late cannot dereference a destroyed Impl.
        const uint64_t callbackEpoch = frameCallbackState_->Begin(this);
        auto callbackState = frameCallbackState_;
        frameArrivedToken_ = framePool_.FrameArrived(
            [callbackState = std::move(callbackState), callbackEpoch](auto&& sender, auto&& args) {
                auto owner = callbackState->Enter(callbackEpoch);
                if (!owner) {
                    return;
                }
                owner->OnFrameArrived(sender, args);
            });

        // Create and start capture session
        session_ = framePool_.CreateCaptureSession(item_);

        // Try to request borderless access and enable border removal (like OBS)
        borderlessCapture_ = false;
        if (session_) {
            if (ShouldKeepWgcBorderByDiagnosticEnv()) {
                LogInfo("[WGC] Border removal skipped by CE_WGC_BORDER_MODE diagnostic override");
            } else if (EnsureBorderlessAccessRequested()) {
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

        // Configure native WGC cursor capture. WGC recording normally keeps
        // this disabled and composites the cursor in the encoder so the live
        // cursor can remain in the hardware plane.
        try {
            if (session_) {
                session_.IsCursorCaptureEnabled(captureCursor);
                LogInfo("[WGC] Native cursor capture: %s", captureCursor ? "YES" : "NO");
            }
        } catch (...) {
            // Not available on older Windows versions
            LogInfo("[WGC] IsCursorCaptureEnabled not available");
        }

        LogInfo(
            "[WGC] Capture session diagnostics: target=%s method=%s hwnd=0x%p hmon=0x%p itemId=0x%llx "
            "framePool=CreateFreeThreaded sourceBuffers=%u borderless=%d nativeCursorRequested=%d "
            "producerTargetFps=%u localThrottleFps=%u",
            targetWindow_ ? "window" : "monitor", WgcItemCreationMethodName(itemCreationMethod_), targetWindow_,
            targetMonitor_, static_cast<unsigned long long>(itemCreationIdValue_), sourceFramePoolBufferCount_,
            borderlessCapture_ ? 1 : 0, captureCursor ? 1 : 0, producerTargetFps_, targetFps_);

        int32_t originLeft = 0;
        int32_t originTop = 0;
        const char* originMode = nullptr;
        if (targetWindow_) {
            originMode = ResolveWindowCaptureOrigin(originLeft, originTop);
        } else if (GetCaptureOrigin(originLeft, originTop)) {
            originMode = "monitor";
        }
        LogInfo("[WGC] Capture origin diagnostics: target=%s originMode=%s origin=(%d,%d) itemSize=%ux%u",
                targetWindow_ ? "window" : "monitor", originMode ? originMode : "unresolved", originLeft, originTop,
                frameWidth_, frameHeight_);

        // Ask WGC to wake no faster than the requested producer cadence so
        // cursor movement cannot drive compositor work far above useful capture FPS.
        ApplyMinUpdateInterval();

        session_.StartCapture();
        LogInfo("[WGC] Capture session started: %dx%d", width, height);
        return true;
    }

    void StopCapture() {
        // Stop the DXGI duplication source first when active: Stop() joins the
        // duplication capture thread, so no sink callbacks can run past this
        // point (the duplication analogue of the WinRT in-flight wait below).
        if (dupSource_) {
            dupSource_->Stop();
            dupSource_.reset();
        }

        // Stop the producer before closing the callback epoch. Queued handlers
        // still retain the shared gate, but once StopAndDrain invalidates this
        // epoch they can no longer acquire Impl. No timeout/polling is needed:
        // active callback leases notify the gate when they leave.
        if (session_) {
            try {
                session_.Close();
            } catch (const winrt::hresult_error& e) {
                LogWarn("[WGC] Capture session close failed: 0x%08lX", static_cast<unsigned long>(e.code().value));
            } catch (...) {
                LogWarn("[WGC] Capture session close failed");
            }
            session_ = nullptr;
        }

        frameCallbackState_->StopAndDrain();

        if (framePool_) {
            try {
                framePool_.FrameArrived(frameArrivedToken_);
            } catch (const winrt::hresult_error& e) {
                LogWarn("[WGC] FrameArrived unsubscribe failed: 0x%08lX", static_cast<unsigned long>(e.code().value));
            } catch (...) {
                LogWarn("[WGC] FrameArrived unsubscribe failed");
            }
            try {
                framePool_.Close();
            } catch (const winrt::hresult_error& e) {
                LogWarn("[WGC] Frame pool close failed: 0x%08lX", static_cast<unsigned long>(e.code().value));
            } catch (...) {
                LogWarn("[WGC] Frame pool close failed");
            }
            framePool_ = nullptr;
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
        if (!framePool_ && !dupSource_) {
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

bool WGCCapture::InitForMonitorDuplication(ID3D11Device* device, void* hmonitor) {
#if HAS_WGC
    if (!device) {
        LogError("[WGC] InitForMonitorDuplication failed: D3D11 device is null");
        return false;
    }
    device_ = device;
    if (!impl_->InitializeDevices(device_)) {
        LogError("[WGC] Failed to initialize capture devices for duplication capture");
        return false;
    }

    // WinRT stays initialized so StartCapture can fall back to a WGC monitor
    // item in place if the duplication becomes unavailable at start time.
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

    if (!impl_->CreateForMonitorDuplication((HMONITOR)hmonitor)) {
        return false;
    }

    initialized_ = true;
    LogInfo("[WGC] Initialized for monitor 0x%p (DXGI duplication backend)", hmonitor);
    return true;
#else
    (void)device;
    (void)hmonitor;
    LogError("[WGC] Not available - WinRT headers not found");
    return false;
#endif
}

bool WGCCapture::IsUsingDesktopDuplication() const {
#if HAS_WGC
    return impl_ && impl_->useDuplicationBackend_;
#else
    return false;
#endif
}

uint64_t WGCCapture::GetDuplicationAcquireTimeoutCount() const {
#if HAS_WGC
    if (impl_ && impl_->dupSource_) {
        return impl_->dupSource_->GetAcquireTimeoutCount();
    }
#endif
    return 0;
}

uint64_t WGCCapture::GetDuplicationAccumulatedMissedFrameCount() const {
#if HAS_WGC
    if (impl_ && impl_->dupSource_) {
        return impl_->dupSource_->GetAccumulatedMissedFrameCount();
    }
#endif
    return 0;
}

bool WGCCapture::IsDuplicationCursorEmbedded() const {
#if HAS_WGC
    if (impl_ && impl_->dupSource_) {
        return impl_->dupSource_->IsCursorEmbeddedInFrames();
    }
#endif
    return false;
}

bool WGCCapture::IsDuplicationSeparatePointerVisible() const {
#if HAS_WGC
    if (impl_ && impl_->dupSource_) {
        return impl_->dupSource_->IsSeparatePointerVisible();
    }
#endif
    return true;
}

uint64_t WGCCapture::GetDuplicationPointerStateTransitionCount() const {
#if HAS_WGC
    if (impl_ && impl_->dupSource_) {
        return impl_->dupSource_->GetPointerStateTransitionCount();
    }
#endif
    return 0;
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

    bool result = false;
    try {
        result = impl_->StartCapture(width_, height_, captureCursor_);
    } catch (const winrt::hresult_error& e) {
        LogError("[WGC] Capture start failed with WinRT error 0x%08lX: %ls", static_cast<unsigned long>(e.code().value),
                 e.message().c_str());
        impl_->StopCapture();
    } catch (const std::exception& e) {
        LogError("[WGC] Capture start failed with C++ exception: %s", e.what());
        impl_->StopCapture();
    } catch (...) {
        LogError("[WGC] Capture start failed with an unknown exception");
        impl_->StopCapture();
    }
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

void WGCCapture::SetSourceEpoch(uint64_t sourceEpoch) {
#if HAS_WGC
    if (impl_) {
        impl_->sourceEpoch_.store(sourceEpoch, std::memory_order_release);
    }
#else
    (void)sourceEpoch;
#endif
}

uint64_t WGCCapture::GetSourceEpoch() const {
#if HAS_WGC
    return impl_ ? impl_->sourceEpoch_.load(std::memory_order_acquire) : 0;
#else
    return 0;
#endif
}

void WGCCapture::SetDirectFrameCallback(std::function<void(ID3D11Texture2D*, uint32_t, uint32_t, int64_t, int64_t, bool,
                                                           bool, bool, int32_t, int32_t, uint64_t,
                                                           WgcPoolSlotLease&&)>
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

uint32_t WGCCapture::GetPoolSlotFreeCurrentCount() const {
#if HAS_WGC
    if (!impl_ || !impl_->poolLeaseState_) {
        return 0;
    }
    const uint32_t slotCount = impl_->poolLeaseState_->slotCount;
    const uint32_t leased = impl_->poolLeaseState_->leasedCurrent.load(std::memory_order_relaxed);
    return slotCount > leased ? (slotCount - leased) : 0u;
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
        impl_->producerTargetFps_ = fps;
        impl_->ApplyFrameThrottleInterval();
        impl_->ApplyProducerInterval();
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

void WGCCapture::SetProducerTargetFps(uint32_t fps) {
#if HAS_WGC
    if (impl_) {
        impl_->producerTargetFps_ = fps;
        impl_->ApplyProducerInterval();
        impl_->ApplyMinUpdateInterval();

        if (fps > 0) {
            if (impl_->producerIntervalQPC_ > 0) {
                LogInfo("[WGC] Producer cadence target set to %u fps (interval=%lld QPC ticks)", fps,
                        static_cast<long long>(impl_->producerIntervalQPC_));
            } else {
                LogInfo("[WGC] Producer cadence target armed for %u fps (pending capture start)", fps);
            }
        } else {
            LogInfo("[WGC] Producer cadence target disabled (max rate)");
        }
    }
#endif
}

uint32_t WGCCapture::GetProducerTargetFps() const {
#if HAS_WGC
    return impl_ ? impl_->producerTargetFps_ : 0;
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
#if HAS_WGC
    return impl_ ? static_cast<int32_t>(impl_->frameCallbackState_->ActiveCount()) : 0;
#else
    return 0;
#endif
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
    if (impl_) {
        // VFR uses the direct callback path and does not drain the internal
        // frame queue, so service deferred output/HDR probes here as well as
        // from DrainPendingFrames. This remains on the owner/media thread.
        impl_->MaybePerformDeferredHDRRecheck();
        return impl_->NeedsReset();
    }
    return false;
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
        const bool wasDuplicationBackend = impl_->useDuplicationBackend_;
        const bool smoothnessBufferEnabled = impl_->smoothnessBufferEnabled_;
        const uint32_t smoothnessOutputFps = impl_->smoothnessOutputFps_;
        const uint32_t smoothnessMaxMs = impl_->smoothnessMaxMs_;
        const uint32_t smoothnessVramBudgetMb = impl_->smoothnessVramBudgetMb_;
        const uint32_t smoothnessSyncDelayFrames = impl_->smoothnessSyncDelayFrames_;
        const bool skipSplitDeviceFlush = impl_->skipSplitDeviceFlush_;
        const bool sameDeviceCapture = impl_->sameDeviceCapture_;
        const bool preferCompact10bitPool = impl_->preferCompact10bitPool_;
        const bool requireHighPrecisionCapture = impl_->requireHighPrecisionCapture_;
        const uint32_t targetFps = impl_->targetFps_;
        const uint32_t producerTargetFps = impl_->producerTargetFps_;
        const auto* throttleFlag = impl_->throttleFlag_;
        const auto directFrameCallback = impl_->frameCallback_.load(std::memory_order_acquire);
        const uint64_t sourceEpoch = impl_->sourceEpoch_.load(std::memory_order_acquire);

        // Stop all producers and synchronously drain the per-instance callback
        // epoch before destroying Impl. A queued WinRT callback can retain the
        // shared gate, but cannot reacquire this owner after StopCapture.
        impl_->frameCallback_.store(nullptr, std::memory_order_release);
        impl_->alive_.store(false, std::memory_order_release);
        impl_->StopCapture();

        impl_.reset();
        impl_ = std::make_unique<Impl>();
        impl_->smoothnessBufferEnabled_ = smoothnessBufferEnabled;
        impl_->smoothnessOutputFps_ = smoothnessOutputFps;
        impl_->smoothnessMaxMs_ = smoothnessMaxMs;
        impl_->smoothnessVramBudgetMb_ = smoothnessVramBudgetMb;
        impl_->smoothnessSyncDelayFrames_ = smoothnessSyncDelayFrames;
        impl_->skipSplitDeviceFlush_ = skipSplitDeviceFlush;
        impl_->sameDeviceCapture_ = sameDeviceCapture;
        impl_->preferCompact10bitPool_ = preferCompact10bitPool;
        impl_->requireHighPrecisionCapture_ = requireHighPrecisionCapture;
        impl_->targetFps_ = targetFps;
        impl_->producerTargetFps_ = producerTargetFps;
        impl_->throttleFlag_ = throttleFlag;
        impl_->sourceEpoch_.store(sourceEpoch, std::memory_order_release);
        impl_->frameCallback_.store(directFrameCallback, std::memory_order_release);
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
            } else if (wasDuplicationBackend && targetMonitor) {
                if (!impl_->CreateForMonitorDuplication(targetMonitor)) {
                    LogWarn("[WGC] ForceReset failed to recreate duplication target; trying WGC monitor item");
                    if (!impl_->CreateForMonitor(targetMonitor)) {
                        LogWarn("[WGC] ForceReset failed to recreate monitor target");
                    }
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

void WGCCapture::SetPreferCompact10bitPool(bool enabled) {
#if HAS_WGC
    if (impl_) {
        impl_->preferCompact10bitPool_ = enabled;
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
        if (changed && (impl_->framePool_ || impl_->dupSource_)) {
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
