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
#include <cctype>
#include <chrono>
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
#include "../common/secure_dll_loading.h"
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
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.h>
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
                module = ce::security::LoadSystemLibrary(moduleName);
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
    if (!SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttlingState, sizeof(throttlingState))) {
        LogWarn("[WGC] Failed to disable callback execution-speed throttling (tid=%lu, err=%lu)", GetCurrentThreadId(),
                GetLastError());
    }
}

class WgcCallbackThreadQoS final {
public:
    WgcCallbackThreadQoS() {
        DisableCurrentWgcCallbackThreadPowerThrottling();
        DWORD taskIndex = 0;
        mmcssHandle_ = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
        if (mmcssHandle_) {
            if (AvSetMmThreadPriority(mmcssHandle_, AVRT_PRIORITY_HIGH)) {
                LogInfo("[WGC] Callback thread QoS enabled (tid=%lu, task=Capture)", GetCurrentThreadId());
            } else {
                LogWarn("[WGC] Callback MMCSS priority elevation failed (tid=%lu, err=%lu)", GetCurrentThreadId(),
                        GetLastError());
            }
        } else {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            LogWarn("[WGC] Callback thread QoS setup failed (tid=%lu, err=%lu)", GetCurrentThreadId(), GetLastError());
        }
    }

    ~WgcCallbackThreadQoS() {
        if (mmcssHandle_) {
            if (!AvRevertMmThreadCharacteristics(mmcssHandle_)) {
                LogWarn("[WGC] Failed to revert callback MMCSS registration (tid=%lu, err=%lu)", GetCurrentThreadId(),
                        GetLastError());
            }
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
