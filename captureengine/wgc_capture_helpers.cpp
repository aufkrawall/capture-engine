#include "wgc_capture_internal.h"


#if HAS_WGC

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

#endif

#if HAS_WGC

void UpdateSmoothedAtomicUs(std::atomic<int64_t>& target,  int64_t sampleUs) {


    if (sampleUs < 0) {
        return;
    }

    int64_t current = target.load(std::memory_order_relaxed);
    const int64_t next = current == 0 ? sampleUs : ((current * 7) + sampleUs) / 8;
    target.store(next, std::memory_order_relaxed);

}

#endif

#if HAS_WGC

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

#endif

#if HAS_WGC

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

#endif

#if HAS_WGC

std::string LowerAscii(std::string value) {


    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;

}

#endif

#if HAS_WGC

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

#endif

#if HAS_WGC

bool ShouldKeepWgcBorderByDiagnosticEnv() {


    const char* env = std::getenv("CE_WGC_BORDER_MODE");
    if (!env || !*env) {
        return false;
    }

    const std::string value = LowerAscii(env);
    return value == "keep" || value == "default" || value == "system" || value == "unchanged";

}

#endif

#if HAS_WGC

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

#endif

#if HAS_WGC

bool TryCreateCaptureItemFromWindowId(HWND hwnd,  winrt::GraphicsCaptureItem& item,  uint64_t& windowIdValue, 
                                      HRESULT& helperHr,  HRESULT& createHr) {


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

#endif

#if HAS_WGC

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

#endif

#if HAS_WGC

void EnsureWgcCallbackThreadQoS() {


    // The free-threaded WGC pool reuses thread-pool workers. Keep MMCSS active
    // for the lifetime of each worker, then balance it when that worker exits.
    thread_local WgcCallbackThreadQoS qos;
    (void)qos;

}

#endif

#if HAS_WGC

int64_t HundredNanosecondsToQpcTicks(int64_t value100ns,  int64_t qpcFreq) {


    if (value100ns <= 0 || qpcFreq <= 0) {
        return 0;
    }

    const int64_t wholeSeconds = value100ns / 10000000ll;
    const int64_t remainder100ns = value100ns % 10000000ll;
    return wholeSeconds * qpcFreq + (remainder100ns * qpcFreq) / 10000000ll;

}

#endif

#if HAS_WGC

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

#endif

#if HAS_WGC

bool GetWindowClientRectInScreen(HWND hwnd,  RECT& rect) {


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

#endif

#if HAS_WGC

bool RectNearlyMatches(const RECT& lhs,  const RECT& rhs,  LONG tolerance) {


    auto absDiff = [](LONG a, LONG b) -> LONG { return (a >= b) ? (a - b) : (b - a); };

    return absDiff(lhs.left, rhs.left) <= tolerance && absDiff(lhs.top, rhs.top) <= tolerance &&
           absDiff(lhs.right, rhs.right) <= tolerance && absDiff(lhs.bottom, rhs.bottom) <= tolerance;

}

#endif

#if HAS_WGC

LONG RectWidth(const RECT& rect) {


    return std::max<LONG>(0, rect.right - rect.left);

}

#endif

#if HAS_WGC

LONG RectHeight(const RECT& rect) {


    return std::max<LONG>(0, rect.bottom - rect.top);

}

#endif

#if HAS_WGC

bool SizeNearlyMatchesRect(uint32_t width,  uint32_t height,  const RECT& rect,  LONG tolerance) {


    auto absDiff = [](int64_t a, int64_t b) -> int64_t { return (a >= b) ? (a - b) : (b - a); };
    return absDiff(static_cast<int64_t>(width), static_cast<int64_t>(RectWidth(rect))) <= tolerance &&
           absDiff(static_cast<int64_t>(height), static_cast<int64_t>(RectHeight(rect))) <= tolerance;

}

#endif

#if HAS_WGC

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

#endif

#if HAS_WGC

int GetBorderlessAccessRequestState() {


    return g_BorderlessAccessRequestState.load(std::memory_order_acquire);

}

#endif

#if HAS_WGC

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

#endif
