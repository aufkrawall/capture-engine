#pragma once

class WGCCapture;

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
};uint32_t WgcIngressReasonCodeFromText(const char* reason);

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
}void UpdateSmoothedAtomicUs(std::atomic<int64_t>& target, int64_t sampleUs);const char* DxgiFormatName(DXGI_FORMAT format);

enum class WgcItemCreationMethod {
    kNone,
    kWindowId,
    kInteropWindow,
    kInteropMonitor,
    kDxgiDuplication,
};const char* WgcItemCreationMethodName(WgcItemCreationMethod method);

enum class WgcItemSourcePreference {
    kAuto,
    kInteropOnly,
};std::string LowerAscii(std::string value);WgcItemSourcePreference GetWgcItemSourcePreference();bool ShouldKeepWgcBorderByDiagnosticEnv();

using GetWindowIdFromWindowFn = HRESULT(WINAPI*)(HWND hwnd, winrt::Windows::UI::WindowId* id);GetWindowIdFromWindowFn ResolveGetWindowIdFromWindow();bool TryCreateCaptureItemFromWindowId(HWND hwnd, winrt::GraphicsCaptureItem& item, uint64_t& windowIdValue,
                                      HRESULT& helperHr, HRESULT& createHr);

struct D3D11ContextStateGuard {D3D11ContextStateGuard(ID3D11DeviceContext* context);~D3D11ContextStateGuard();

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
)";void DisableCurrentWgcCallbackThreadPowerThrottling();

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
    }~WgcCallbackThreadQoS();

private:
    HANDLE mmcssHandle_ = nullptr;
};void EnsureWgcCallbackThreadQoS();int64_t HundredNanosecondsToQpcTicks(int64_t value100ns, int64_t qpcFreq);bool IsHdrOutputColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace);

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
}bool GetWindowClientRectInScreen(HWND hwnd, RECT& rect);bool RectNearlyMatches(const RECT& lhs, const RECT& rhs, LONG tolerance);LONG RectWidth(const RECT& rect);LONG RectHeight(const RECT& rect);bool SizeNearlyMatchesRect(uint32_t width, uint32_t height, const RECT& rect, LONG tolerance);bool IsFullscreenLikeWindow(HWND hwnd);

constexpr int kBorderlessAccessUnknown = 0;
constexpr int kBorderlessAccessAllowed = 1;
constexpr int kBorderlessAccessDenied = 2;
constexpr int kBorderlessAccessUnavailable = 3;

inline std::once_flag g_BorderlessAccessRequestOnce;
inline std::atomic<int> g_BorderlessAccessRequestState{kBorderlessAccessUnknown};int GetBorderlessAccessRequestState();bool EnsureBorderlessAccessRequested();
#endif

class WGCCapture::Impl {
public:

#if HAS_WGC
    Impl()
        : itemCallbackState_(ce::CallbackEpoch<Impl>::Create()),
          frameCallbackState_(ce::CallbackEpoch<Impl>::Create()) {}~Impl();

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
                                           const ce::cursor::SourcePointerObservation&, int32_t, int32_t, uint64_t,
                                           WgcPoolSlotLease&&);
    std::atomic<DirectFrameCallbackFn> frameCallback_{nullptr};
    using DirectCursorCallbackFn = void (*)(const ce::cursor::SourcePointerObservation&, int32_t, int32_t, uint32_t,
                                            uint32_t, uint64_t);
    std::atomic<DirectCursorCallbackFn> cursorCallback_{nullptr};
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
    std::atomic<uint64_t> lastDeliveredRateSampleTickMs_{0};
    std::atomic<uint64_t> lastInputRateSampleTickMs_{0};

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
    std::atomic<uint32_t> keyedMutexAbandonedReclaimCount_{0};
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
    ID3D11Query* gpuTimingDisjoint_ = nullptr;
    ID3D11Query* gpuTimingStart_ = nullptr;
    ID3D11Query* gpuTimingEnd_ = nullptr;
    bool gpuTimingPending_ = false;
    bool gpuTimingActive_ = false;
    ULONGLONG lastGpuTimingSampleTick_ = 0;
    int64_t gpuTimingSubmitQpc_ = 0;
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
    // set, StartCapture tries duplication first. Auto/8-bit operation may
    // fall back to WGC; explicit DXGI + 10-bit operation is strict.
    std::unique_ptr<DxgiDuplicationSource> dupSource_;
    bool useDuplicationBackend_ = false;
    std::string dupInitFailureReason_;
    bool useHighPrecisionCapture_ = false;
    bool requireHighPrecisionCapture_ = false;
    bool allowDuplicationFallback_ = true;
    // Deferred output rechecks run on the consumer thread while frame delivery
    // reads this flag on the WinRT/duplication callback thread.
    std::atomic<bool> captureIsHDR_{false};
    bool borderlessCapture_ = false;
    UINT outputBitsPerColor_ = 8;
    DXGI_COLOR_SPACE_TYPE outputColorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    std::atomic<bool> resetNeeded_{false};
    bool skipSplitDeviceFlush_ = false;
    bool sameDeviceCapture_ = false;
    bool allowLossyBgra8Pool_ = false;
    bool smoothnessBufferEnabled_ = true;
    uint32_t smoothnessOutputFps_ = 0;
    uint32_t smoothnessMaxMs_ = ce::capture_policy::kWgcSmoothnessBufferDefaultMaxMs;
    uint32_t smoothnessVramBudgetMb_ = ce::capture_policy::kWgcSmoothnessBufferDefaultVramBudgetMb;
    std::atomic<int> desiredGpuPriority_{0};
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
    uint32_t allocationLimitedPoolSlots_ = 0;
    uint32_t allocationLimitedSourceBuffers_ = 0;
    uint32_t allocationLimitWidth_ = 0;
    uint32_t allocationLimitHeight_ = 0;
    DXGI_FORMAT allocationLimitSourceFormat_ = DXGI_FORMAT_UNKNOWN;
    std::atomic<ULONGLONG> lastVideoMemoryLogTick_{0};
    std::atomic<uint32_t> videoMemoryOverBudgetEpisodes_{0};
    enum class VideoMemoryReservationMode : uint8_t { kOff, kMandatory, kFull };
    VideoMemoryReservationMode videoMemoryReservationMode_ = VideoMemoryReservationMode::kOff;
    uint64_t activeVideoMemoryReservationBytes_ = 0;
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
    std::atomic<bool> hdrRecheckPending_{false};void FlagResetNeeded(const char* reason);bool NeedsReset() const;std::string ConsumeResetReason();void PerformHDRRecheck();

    // Periodically re-check HDR state (handles mid-capture HDR toggle in Windows settings)
    // but keep the DXGI probe off the WinRT callback hot path.
void RequestHDRRecheckIfDue();void MaybePerformDeferredHDRRecheck();const char* DescribeCaptureFormat() const;uint32_t BytesPerPixelForFormat(DXGI_FORMAT format) const;DXGI_FORMAT GetRetainedPoolFormat(DXGI_FORMAT sourceFormat) const;bool IsCompactRetainedCopy(DXGI_FORMAT sourceFormat, DXGI_FORMAT retainedFormat) const;void ReleasePoolConversionResources();void ReleaseGpuTimingResources();bool EnsurePoolCopyShader();bool CreatePoolCopySourceSrv(ID3D11Texture2D* sourceTexture, const D3D11_TEXTURE2D_DESC& sourceDesc,
                                 DXGI_FORMAT inputSrvFormat, ID3D11ShaderResourceView** outSrv, bool* usedStaging);bool RenderFrameToPoolSlot(ID3D11Texture2D* sourceTexture, const D3D11_TEXTURE2D_DESC& sourceDesc,
                               ID3D11RenderTargetView* targetRtv, bool linearToSrgb, bool* usedStaging);ce::capture_policy::WgcSmoothnessSurfaceBudget ComputeTexturePoolBudget(uint32_t width, uint32_t height,
                                                                            DXGI_FORMAT format) const;void UpdateSmoothnessBudget(uint32_t width, uint32_t height, DXGI_FORMAT format, bool logBudget);void ReleaseTexturePool();bool EnsureGpuTimingQueries();void PollGpuTimingSample();void BeginGpuTimingSample();void EndGpuTimingSample();void LogVideoMemoryInfo(const char* stage, bool force = false);bool IsAllocationExhaustion(HRESULT hr);void SetVideoMemoryReservationBytes(uint64_t requestedBytes, const char* stage);void ApplyConfiguredVideoMemoryReservation();void ResetVideoMemoryReservation();void EnableMultithreadProtection(ID3D11Device* device, const char* label);void ApplyConfiguredGpuPriority(const char* role);bool InitializeDevices(ID3D11Device* encoderDevice);void ReleaseCapturedFrame(WGCCapturedFrame& frame);void ResetStats();void ReleasePendingFramesLocked();void EnqueueFrameInternal(WGCCapturedFrame&& frame);void QueuePendingFrame(WGCCapturedFrame&& frame);void RecordInputFrameEvent();void RecordDeliveredFrameEvent();void RecordSourceTimingSample(int64_t sourceFrameQpc);void RecordSourceToCopyLatency(int64_t sourceFrameQpc, int64_t copyCompleteQpc);void ApplyFrameThrottleInterval();void ApplyProducerInterval();void ApplyMinUpdateInterval();int64_t GetFrameSourceQpc(const winrt::Direct3D11CaptureFrame& frame) const;bool IsStaleSourceFrameQpc(int64_t sourceFrameQpc) const;bool IsOutOfOrderRawSourceFrameQpc(int64_t sourceFrameQpc) const;int64_t NormalizeSourceFrameQpc(int64_t sourceFrameQpc, bool* duplicateSourceTimestamp = nullptr);HMONITOR ResolveTargetMonitor() const;bool GetCaptureOrigin(int32_t& left, int32_t& top) const;const char* ResolveWindowCaptureOrigin(int32_t& left, int32_t& top) const;bool QueryOutputDesc1ForMonitor(HMONITOR monitor, DXGI_OUTPUT_DESC1& desc1);void UpdateCaptureFormatSelection();bool EnsureTexturePool(uint32_t width, uint32_t height, DXGI_FORMAT sourceFormat = DXGI_FORMAT_B8G8R8A8_UNORM);bool ShouldAdmitFrameToPool(int64_t sourceFrameQpc, int64_t rawSourceFrameQpc, uint32_t poolSize,
                                const std::shared_ptr<WgcPoolLeaseState>& leaseState);bool CopyFrameToPool(ID3D11Texture2D* sourceTexture, const D3D11_TEXTURE2D_DESC& sourceDesc, int64_t sourceFrameQpc,
                         int64_t rawSourceFrameQpc, ID3D11Texture2D** out, int64_t& copyCompleteQpc,
                         WgcPoolSlotLease& outLease, uint32_t& outSlot, uint64_t& outGeneration);

    // Shared source-agnostic frame admission (timestamp ordering, duplicate
    // normalization, pacing, external throttle, staleness). Used by both the
    // WGC frame-pool path and the DXGI duplication path so every skip counter
    // and cadence policy behaves identically for both backends.
    struct SourceFramePreflight {
        int64_t rawSourceFrameQpc = 0;
        int64_t sourceFrameQpc = 0;
        bool duplicateSourceTimestamp = false;
        bool accepted = false;
    };SourceFramePreflight PreflightSourceFrame(int64_t rawSourceFrameQpc);

    // Shared pool copy + delivery for an admitted source texture. The texture
    // only needs to stay valid for the duration of this call (the GPU copy is
    // submitted synchronously), which is exactly the DXGI duplication
    // Acquire/Release contract as well as the WinRT surface lifetime.
bool DeliverSourceTexture(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& desc,
                              const SourceFramePreflight& pre, WGCCapturedFrame* outputFrame);bool ProcessCapturedFrame(const winrt::Direct3D11CaptureFrame& winrtFrame, WGCCapturedFrame* outputFrame);

    // DXGI duplication sink: mirrors OnFrameArrived's locking, instrumentation,
    // and pull/callback dual-mode dispatch for the duplication capture thread.
void OnDuplicationFrame(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& desc, int64_t rawSourceQpc);void OnFrameArrived(winrt::Direct3D11CaptureFramePool const& sender, winrt::IInspectable const&);bool CreateWinRTDevice();void UnsubscribeItemClosed() noexcept;bool SubscribeItemClosed(const char* targetName, const char* resetReason);bool CreateForMonitor(HMONITOR hmon);bool CreateForWindow(HWND hwnd);

    // Prepare a DXGI Desktop Duplication monitor target. Only light
    // availability validation happens here (adapter/output match, rotation);
    // the duplication object itself is created at StartCapture so an idle
    // primed target does not keep system-wide desktop duplication active
    // (which would suppress MPO/DirectFlip between recordings).
bool CreateForMonitorDuplication(HMONITOR hmon);bool StartDuplicationCapture(uint32_t& width, uint32_t& height);bool StartCapture(uint32_t& width, uint32_t& height, bool captureCursor);void StopCapture();size_t DrainPendingFrames(std::vector<WGCCapturedFrame>& frames, size_t maxFrames);bool GetNextFrame(WGCCapturedFrame& frame);

#endif
#if !HAS_WGC
    // Stub implementation when WGC headers not available
    ID3D11Device* d3dDevice_ = nullptr;
    ID3D11DeviceContext* d3dContext_ = nullptr;bool CreateWinRTDevice();bool CreateForMonitor(void*);bool CreateForWindow(void*);bool StartCapture(uint32_t&, uint32_t&, bool);void StopCapture();bool GetNextFrame(WGCCapturedFrame&);bool GetCaptureOrigin(int32_t&, int32_t&) const;

#endif
};
