// Windows Graphics Capture (WGC) implementation
// Supports capturing DirectFlip content that Desktop Duplication cannot handle
// Requires Windows 10 1803+

#include "wgc_capture.h"
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include "../common/frame_timing_utils.h"
#include "../common/logging.h"
#include "../common/rate_window_utils.h"
#include "mediaengine_loader.h"

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
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
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
template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
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
    // so the encoder can consume frame N while frame N+1 is being copied.
    static constexpr int TEXTURE_POOL_SIZE = 12;                   // Enough for 120fps + short encoder/callback spikes
    ID3D11Texture2D* texturePool_[TEXTURE_POOL_SIZE] = {};         // Encoder-device textures
    ID3D11Texture2D* captureTexturePool_[TEXTURE_POOL_SIZE] = {};  // Capture-device views when split
    IDXGIKeyedMutex* captureTextureMutexPool_[TEXTURE_POOL_SIZE] = {};
    int32_t poolWidth_ = 0;
    int32_t poolHeight_ = 0;
    std::atomic<uint32_t> poolWriteIndex_{0};

    // Legacy single-texture for GetNextFrame (pull mode)
    ID3D11Texture2D* cachedTexture_ = nullptr;
    int32_t cachedWidth_ = 0;
    int32_t cachedHeight_ = 0;

    std::mutex frameMutex_;
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
    std::atomic<int64_t> lastObservedRawSourceQpc_{0};
    std::atomic<int64_t> lastAssignedSourceQpc_{0};

    // Callback function for direct frame processing.
    // Atomic raw function pointer: only static functions (or nullptr) are ever
    // stored, so std::function overhead and its non-atomic nature are avoided.
    // This eliminates the data race between the WinRT callback thread (reader)
    // and the main thread (writer during start/stop recording).
    using DirectFrameCallbackFn = void (*)(ID3D11Texture2D*, uint32_t, uint32_t, int64_t, int64_t, bool, bool, int32_t,
                                           int32_t);
    std::atomic<DirectFrameCallbackFn> frameCallback_{nullptr};
    std::atomic<uint32_t> callbackFrameCount_{0};
    std::atomic<uint32_t> inputFrameCount_{0};
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
    std::atomic<uint32_t> cursorOnlySkipCount_{0};
    std::atomic<uint32_t> poolDropCount_{0};
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

    DXGI_FORMAT poolFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    winrt::DirectXPixelFormat capturePixelFormat_ = winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
    DXGI_FORMAT captureDxgiFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    HWND targetWindow_ = nullptr;
    HMONITOR targetMonitor_ = nullptr;
    bool useHighPrecisionCapture_ = false;
    bool captureIsHDR_ = false;
    bool borderlessCapture_ = false;
    UINT outputBitsPerColor_ = 8;
    DXGI_COLOR_SPACE_TYPE outputColorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    std::atomic<bool> resetNeeded_{false};
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

    void ReleaseTexturePool() {
        for (int i = 0; i < TEXTURE_POOL_SIZE; ++i) {
            SafeRelease(captureTextureMutexPool_[i]);
            SafeRelease(captureTexturePool_[i]);
            SafeRelease(texturePool_[i]);
        }
        poolWidth_ = 0;
        poolHeight_ = 0;
        poolFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
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

    void ResetStats() {
        resetNeeded_.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(resetReasonMutex_);
            resetReason_.clear();
        }
        callbackFrameCount_.store(0, std::memory_order_relaxed);
        inputFrameCount_.store(0, std::memory_order_relaxed);
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
        cursorOnlySkipCount_.store(0, std::memory_order_relaxed);
        poolDropCount_.store(0, std::memory_order_relaxed);
        lastCopyUs_.store(0, std::memory_order_relaxed);
        lastDeliveredSourceQpc_.store(0, std::memory_order_relaxed);
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
            SafeRelease(stale.texture);
        }
    }

    void ReleasePendingFramesLocked() {
        while (!pendingFrames_.empty()) {
            WGCCapturedFrame stale = std::move(pendingFrames_.front());
            pendingFrames_.pop_front();
            SafeRelease(stale.texture);
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
                SafeRelease(lastPending.texture);
                lastPending = std::move(frame);
                return;
            }
        }

        pendingFrames_.push_back(std::move(frame));
        while (pendingFrames_.size() > kMaxPendingFrames) {
            WGCCapturedFrame stale = std::move(pendingFrames_.front());
            pendingFrames_.pop_front();
            SafeRelease(stale.texture);
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
            if (targetFps_ > 0) {
                int64_t interval100ns = 10000000ll / targetFps_;
                if (interval100ns <= 0) {
                    interval100ns = 1;
                }
                session_.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{interval100ns});
                LogInfo("[WGC] MinUpdateInterval set to %lld (100ns) for %u fps target", (long long)interval100ns,
                        targetFps_);
            } else {
                session_.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{0});
                LogInfo("[WGC] MinUpdateInterval set to 0 (max rate)");
            }
        } catch (...) {
            LogInfo("[WGC] MinUpdateInterval not available (older Windows)");
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

        auto frame2 = frame.try_as<winrt::Windows::Graphics::Capture::IDirect3D11CaptureFrame2>();
        if (!frame2) {
            return false;
        }

        auto dirtyRegions = frame2.DirtyRegions();
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
            LogInfo("[WGC] Output probe unavailable, using BGRA8 capture");
            return;
        }

        outputBitsPerColor_ = desc1.BitsPerColor;
        outputColorSpace_ = desc1.ColorSpace;
        captureIsHDR_ = IsHdrOutputColorSpace(desc1.ColorSpace);
        if (captureIsHDR_) {
            useHighPrecisionCapture_ = true;
            capturePixelFormat_ = winrt::DirectXPixelFormat::R16G16B16A16Float;
            captureDxgiFormat_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
        } else if (desc1.BitsPerColor > 8) {
            useHighPrecisionCapture_ = true;
            capturePixelFormat_ = winrt::DirectXPixelFormat::R10G10B10A2UIntNormalized;
            captureDxgiFormat_ = DXGI_FORMAT_R10G10B10A2_UNORM;
        }

        LogInfo("[WGC] Output probe: bpc=%u colorSpace=%d hdr=%s highPrecision=%s captureFormat=%s",
                outputBitsPerColor_, (int)outputColorSpace_, captureIsHDR_ ? "YES" : "NO",
                useHighPrecisionCapture_ ? "YES" : "NO", DescribeCaptureFormat());
    }

    bool EnsureTexturePool(uint32_t width, uint32_t height, DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM) {
        const bool splitDevicePool =
            usingDedicatedCaptureDevice_ && encoderDevice_ && d3dDevice_ && encoderDevice_ != d3dDevice_;
        if (poolWidth_ == (int32_t)width && poolHeight_ == (int32_t)height && poolFormat_ == format &&
            texturePool_[0] && (!splitDevicePool || captureTexturePool_[0])) {
            return true;
        }

        ReleaseTexturePool();

        D3D11_TEXTURE2D_DESC copyDesc = {};
        copyDesc.Width = width;
        copyDesc.Height = height;
        copyDesc.MipLevels = 1;
        copyDesc.ArraySize = 1;
        copyDesc.Format = format;  // Match WGC source format
        copyDesc.SampleDesc.Count = 1;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        // CRITICAL: VP input view requires RENDER_TARGET bind flag
        // Using only SHADER_RESOURCE causes D3D11 internal stack corruption (0xC0000409)
        copyDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        copyDesc.MiscFlags = splitDevicePool ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : 0;

        for (int i = 0; i < TEXTURE_POOL_SIZE; i++) {
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
        }

        poolWidth_ = width;
        poolHeight_ = height;
        poolFormat_ = format;
        LogInfo("[WGC] Texture pool created: %dx%d fmt=%d x%d (%s)", width, height, format, TEXTURE_POOL_SIZE,
                splitDevicePool ? "dedicated capture device" : "shared device");
        return true;
    }

    bool CopyFrameToPool(ID3D11Texture2D* sourceTexture, const D3D11_TEXTURE2D_DESC& sourceDesc, int64_t sourceFrameQpc,
                         int64_t rawSourceFrameQpc, ID3D11Texture2D** out, int64_t& copyCompleteQpc) {
        if (!sourceTexture || !out) {
            return false;
        }

        *out = nullptr;
        copyCompleteQpc = 0;

        if (!EnsureTexturePool(sourceDesc.Width, sourceDesc.Height, sourceDesc.Format)) {
            return false;
        }

        const uint32_t startIndex = poolWriteIndex_.fetch_add(1, std::memory_order_relaxed) % TEXTURE_POOL_SIZE;
        for (int attempt = 0; attempt < TEXTURE_POOL_SIZE; ++attempt) {
            const uint32_t idx = (startIndex + attempt) % TEXTURE_POOL_SIZE;
            ID3D11Texture2D* copyTarget = captureTexturePool_[idx] ? captureTexturePool_[idx] : texturePool_[idx];
            IDXGIKeyedMutex* writeMutex = captureTextureMutexPool_[idx];
            bool mutexAcquired = false;

            if (writeMutex) {
                const HRESULT kmHr = writeMutex->AcquireSync(0, 0);
                if (kmHr != S_OK) {
                    continue;
                }
                mutexAcquired = true;
            }

            LARGE_INTEGER copyStart = {};
            LARGE_INTEGER copyEnd = {};
            QueryPerformanceCounter(&copyStart);

            d3dContext_->CopyResource(copyTarget, sourceTexture);
            if (mutexAcquired) {
                d3dContext_->Flush();
                const HRESULT releaseHr = writeMutex->ReleaseSync(0);
                if (releaseHr != S_OK) {
                    LogWarn("[WGC] Shared texture ReleaseSync failed for slot %u: 0x%08lX", idx,
                            (unsigned long)releaseHr);
                    continue;
                }
            }

            QueryPerformanceCounter(&copyEnd);

            int64_t copyUs = 0;
            if (qpcFreq_ > 0) {
                copyUs = ((copyEnd.QuadPart - copyStart.QuadPart) * 1000000) / qpcFreq_;
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
            copyCompleteQpc = copyEnd.QuadPart;
            return true;
        }

        skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
        poolDropCount_.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> contentionLogCount{0};
        if (contentionLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            LogWarn("[WGC] Shared texture pool saturated; dropping frame");
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
                if (CopyFrameToPool(texture, desc, sourceFrameQpc, rawSourceFrameQpc, &copiedTexture,
                                    copyCompleteQpc)) {
                    const int64_t deliveredTimestamp = sourceFrameQpc > 0 ? sourceFrameQpc : copyCompleteQpc;
                    if (sourceFrameQpc > 0 && !duplicateSourceTimestamp) {
                        lastDeliveredSourceQpc_.store(sourceFrameQpc, std::memory_order_relaxed);
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
                    } else {
                        auto cb = frameCallback_.load(std::memory_order_acquire);
                        if (cb) {
                            cb(copiedTexture, desc.Width, desc.Height, deliveredTimestamp, rawSourceFrameQpc,
                               captureIsHDR_, duplicateSourceTimestamp, captureLeft, captureTop);
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

        struct DecrementGuard {
            ~DecrementGuard() {
                g_WgcInflightCallbacks.fetch_sub(1, std::memory_order_acq_rel);
            }
        } decrementGuard;

        // Check if Impl is still alive
        if (!alive_.load(std::memory_order_acquire)) {
            return;
        }

        if (NeedsReset()) {
            while (alive_.load(std::memory_order_acquire)) {
                auto winrtFrame = sender.TryGetNextFrame();
                if (!winrtFrame) {
                    break;
                }
                winrtFrame.Close();
            }
            return;
        }

        // Pull mode: drain promptly into an internal bounded queue so the
        // encoder thread only performs CFR scheduling. This preserves recent
        // temporal history across callback bursts and avoids coupling source
        // collection to encoder wakeups.
        if (!frameCallback_.load(std::memory_order_acquire)) {
            bool processedFrame = false;
            {
                std::lock_guard<std::mutex> lock(frameMutex_);
                while (alive_.load(std::memory_order_acquire)) {
                    auto winrtFrame = sender.TryGetNextFrame();
                    if (!winrtFrame) {
                        break;
                    }

                    WGCCapturedFrame frame{};
                    if (ProcessCapturedFrame(std::move(winrtFrame), &frame) && frame.texture) {
                        EnqueueFrameInternal(std::move(frame));
                        processedFrame = true;
                    }
                }
            }

            if (processedFrame && frameArrivedEvent_) {
                SetEvent(frameArrivedEvent_);
            }
            return;
        }

        std::lock_guard<std::mutex> lock(callbackDrainMutex_);
        if (!alive_.load(std::memory_order_acquire)) {
            return;
        }

        bool processedFrame = false;
        while (alive_.load(std::memory_order_acquire)) {
            auto winrtFrame = sender.TryGetNextFrame();
            if (!winrtFrame) {
                break;
            }
            processedFrame = ProcessCapturedFrame(std::move(winrtFrame), nullptr) || processedFrame;
        }

        if (processedFrame && frameArrivedEvent_) {
            SetEvent(frameArrivedEvent_);
        }
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
                                                   __uuidof(IGraphicsCaptureItemInterop), interopFactory.put_void());
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
                                                   __uuidof(IGraphicsCaptureItemInterop), interopFactory.put_void());
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

        // CRITICAL: Must use CreateFreeThreaded (not Create) because we have no
        // message pump! Create() requires a DispatcherQueue pumping messages for
        // callbacks to fire. CreateFreeThreaded() uses an internal worker thread
        // for callbacks. Use 12 buffers to give the CFR selector more headroom
        // during high-Hz bursts and brief encoder catchup pressure.
        auto tryCreateFramePool = [&](winrt::DirectXPixelFormat format) -> bool {
            try {
                framePool_ = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(winrtDevice_, format, 12, size);
                return framePool_ != nullptr;
            } catch (const winrt::hresult_error& e) {
                LogWarn("[WGC] Frame pool creation failed for format=%d: 0x%08X", (int)format,
                        (unsigned)e.code().value);
                framePool_ = nullptr;
                return false;
            }
        };

        if (!tryCreateFramePool(capturePixelFormat_)) {
            if (!captureIsHDR_ && capturePixelFormat_ == winrt::DirectXPixelFormat::R10G10B10A2UIntNormalized) {
                // SDR 10-bpc: try FP16 to preserve full 10-bit precision in the
                // captured texture.  Only fall to BGRA8 if FP16 also fails.
                LogWarn("[WGC] R10G10B10A2 frame pool unavailable for SDR 10-bit, trying FP16");
                capturePixelFormat_ = winrt::DirectXPixelFormat::R16G16B16A16Float;
                captureDxgiFormat_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
                // useHighPrecisionCapture_ stays true
            } else {
                capturePixelFormat_ = winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
                captureDxgiFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
                useHighPrecisionCapture_ = false;
            }

            if (!tryCreateFramePool(capturePixelFormat_)) {
                if (capturePixelFormat_ != winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized) {
                    LogWarn("[WGC] High-precision frame pool unavailable, falling back to BGRA8 capture");
                    capturePixelFormat_ = winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
                    captureDxgiFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
                    useHighPrecisionCapture_ = false;
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
                SafeRelease(stale.texture);
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

void WGCCapture::SetDirectFrameCallback(
    std::function<void(ID3D11Texture2D*, uint32_t, uint32_t, int64_t, int64_t, bool, bool, int32_t, int32_t)>
        callback) {
#if HAS_WGC
    if (impl_) {
        // Extract the raw function pointer from std::function.
        // Only static/free functions are ever passed (QueueWgcFrame or nullptr).
        Impl::DirectFrameCallbackFn rawPtr = nullptr;
        if (callback) {
            rawPtr = *callback.target<Impl::DirectFrameCallbackFn>();
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
