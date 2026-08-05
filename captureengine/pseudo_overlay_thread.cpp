#include "pseudo_overlay_internal.h"

LRESULT CALLBACK PseudoOverlay::WarningWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    if (m == WM_ACTIVATE || m == WM_ACTIVATEAPP) {
        return 0;
    }
    if (m == WM_SETFOCUS) {
        SetFocus(NULL);
        return 0;
    }
    if (m == WM_NCACTIVATE) {
        return FALSE;
    }
    if (m == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    return DefWindowProc(h, m, w, l);
}

VOID CALLBACK PseudoOverlay::TimerProc(HWND, UINT, UINT_PTR timerId, DWORD) {
    if (timerId != 0 && instance_ && instance_->initialized_.load(std::memory_order_acquire)) {
        instance_->OnTimerTick();
    }
}

bool PseudoOverlay::Init(HINSTANCE hInstance) {
    if (initialized_.load(std::memory_order_acquire))
        return true;

    if (uiThread_.joinable()) {
        return false;
    }

    hInstance_ = hInstance;
    uiReadyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!uiReadyEvent_) {
        LogError("[PseudoOverlay] Failed to create UI-thread readiness event");
        return false;
    }

    uiInitSucceeded_.store(false, std::memory_order_release);
    uiThread_ = std::thread([this]() { ThreadMain(); });
    const DWORD readyResult = WaitForSingleObject(uiReadyEvent_, 5000);
    const bool ready = readyResult == WAIT_OBJECT_0 && uiInitSucceeded_.load(std::memory_order_acquire);
    if (!ready) {
        LogError("[PseudoOverlay] UI thread failed to initialize (wait=%lu)", readyResult);
        const DWORD threadId = uiThreadId_.load(std::memory_order_acquire);
        if (threadId != 0) {
            PostThreadMessageW(threadId, kMsgShutdown, 0, 0);
        }
        if (uiThread_.joinable()) {
            uiThread_.join();
        }
        CloseHandle(uiReadyEvent_);
        uiReadyEvent_ = NULL;
        uiThreadId_.store(0, std::memory_order_release);
        hInstance_ = NULL;
        return false;
    }

    CloseHandle(uiReadyEvent_);
    uiReadyEvent_ = NULL;
    StartStatusSyncWatcher();
    return true;
}

void PseudoOverlay::ThreadMain() {
    uiThreadId_.store(GetCurrentThreadId(), std::memory_order_release);
    MSG msg = {};
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    const bool initialized = InitializeOnUiThread();
    uiInitSucceeded_.store(initialized, std::memory_order_release);
    if (uiReadyEvent_) {
        SetEvent(uiReadyEvent_);
    }
    if (!initialized) {
        instance_ = nullptr;
        uiThreadId_.store(0, std::memory_order_release);
        return;

    }

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kMsgShutdown) {
            break;
        }
        if (msg.message == kMsgRefresh) {
            ApplyPendingConfig();
            RefreshRecordingState();
            RefreshActiveProfileConfig();
            UpdateOverlay();
            continue;
        }
        if (msg.message == kMsgStatusSync) {
            HandleStatusSyncOnUiThread();
            continue;
        }
#ifdef CE_UNIT_TESTS
        if (msg.message == kMsgTestBarrier) {
            SetEvent(reinterpret_cast<HANDLE>(msg.wParam));
            continue;
        }
#endif
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ShutdownOnUiThread();
    uiThreadId_.store(0, std::memory_order_release);
}

bool PseudoOverlay::InitializeOnUiThread() {
    instance_ = this;

    currentDpi_ = 96;
    scale_ = 1.0f;
    UpdateScaleForDpi(GetDpiForSystem());

    InitGDI();

    // Register indicator window class
    WNDCLASSA wcInd = {};
    wcInd.lpfnWndProc = IndicatorWndProc;
    wcInd.hInstance = hInstance_;
    wcInd.lpszClassName = kIndicatorClass;
    if (!RegisterClassA(&wcInd) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogError("[PseudoOverlay] Failed to register indicator window class");
        CleanupGDI();
        return false;
    }

    // Register warning window class
    WNDCLASSA wcWarn = {};
    wcWarn.lpfnWndProc = WarningWndProc;
    wcWarn.hInstance = hInstance_;
    wcWarn.lpszClassName = kWarningClass;
    if (!RegisterClassA(&wcWarn) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogError("[PseudoOverlay] Failed to register warning window class");
        CleanupGDI();
        return false;
    }

    timerHandle_ = SetTimer(NULL, kTimerId, kTimerInterval, TimerProc);
    if (timerHandle_ == 0) {
        LogError("[PseudoOverlay] Failed to start timer");
        CleanupGDI();
        return false;
    }

    const AnchorInfo anchor = ResolveAnchorInfo();
    initialized_.store(true, std::memory_order_release);
    ApplyPendingConfig();
    RefreshRecordingState();
    RefreshActiveProfileConfig();
    LogInfo("[PseudoOverlay] Initialized (scale=%.2f)", scale_);
    LogInfo("[PseudoOverlay] Initial anchor: monitor=%p window=%p dpi=%u fullscreenLike=%d", anchor.monitor,
            anchor.window, currentDpi_, anchor.fullscreenLike ? 1 : 0);

    // Initial render
    UpdateOverlay();

    return true;
}

void PseudoOverlay::Shutdown() {
    StopStatusSyncWatcher();
    if (!uiThread_.joinable())
        return;

    const DWORD threadId = uiThreadId_.load(std::memory_order_acquire);
    if (threadId != 0) {
        PostThreadMessageW(threadId, kMsgShutdown, 0, 0);
    }
    uiThread_.join();
    uiThreadId_.store(0, std::memory_order_release);
    hInstance_ = NULL;
}

void PseudoOverlay::ShutdownOnUiThread() {
    if (!initialized_.load(std::memory_order_acquire))
        return;

    if (timerHandle_ != 0) {
        KillTimer(NULL, timerHandle_);
        timerHandle_ = 0;
    }

    DestroyOverlayWindows();

    CleanupGDI();

    // Release shared memory handles
    if (pSharedMem_) {
        UnmapViewOfFile(pSharedMem_);
        pSharedMem_ = nullptr;
    }
    if (pDiscovery_) {
        UnmapViewOfFile(pDiscovery_);
        pDiscovery_ = nullptr;
    }
    if (hSharedMemMap_) {
        CloseHandle(hSharedMemMap_);
        hSharedMemMap_ = NULL;
    }
    if (hDiscoveryMap_) {
        CloseHandle(hDiscoveryMap_);
        hDiscoveryMap_ = NULL;
    }

    // Reset tracking state
    lastOv_ = {};
    lastCol_ = 0;
    lastWarnVis_ = false;
    recordingNotifyUntil_.store(0, std::memory_order_relaxed);
    recordingNotification_.store(ce::pseudo_overlay::RecordingNotificationKind::None, std::memory_order_relaxed);
    lastWarnMsg_.clear();
    warnActive_ = false;
    warnVisible_ = false;
    warnCycleStart_ = 0;
    lastTimerTickMs_ = 0;
    statusDarkForCapture_ = false;
    mappedInjectPid_ = 0;
    lastEncoderOverloadFlags_ = 0;
    lastCaptureHealthFlags_ = 0;
    overloadWarnSustainFpsX100_.store(0, std::memory_order_relaxed);
    overloadWarnKind_.store(ce::capture_policy::kOverlayWarningNone, std::memory_order_relaxed);
    lastOverlaySuppressed_ = false;
    lastFullscreenSuppressed_ = false;
    stickyAnchorWindow_ = NULL;
    stickyAnchorMonitor_ = NULL;
    stickyAnchorDpi_ = 96;
    lastForegroundAcquireTick_ = 0;
    lastForegroundAcquirePid_ = 0;
    hadForegroundTarget_ = false;
    activeProfileSection_.clear();
    pinnedProfileSection_.clear();
    foregroundProcessName_.clear();
    foregroundPid_ = 0;
    sourceProcessName_.clear();
    sourceProfilePid_ = 0;
    foregroundIsTarget_ = false;
    prevRecordingIndicatorState_ = ce::recording_indicator::State::Idle;
    prevGraceActive_ = false;
    foregroundGraceEverStarted_ = false;
    recordingIndicatorState_ = ce::recording_indicator::State::Idle;
    publishedRecordingIndicatorState_.store(ce::recording_indicator::State::Idle, std::memory_order_release);
    isRecording_.store(false, std::memory_order_release);
    initialized_.store(false, std::memory_order_release);
    instance_ = nullptr;

    LogInfo("[PseudoOverlay] Shutdown complete");
}

void PseudoOverlay::UpdateConfig(const PseudoOverlayConfig& cfg,
                                 const std::vector<PseudoOverlayApplicationConfig>& profiles) {
    {
        std::lock_guard<std::mutex> lock(pendingConfigMutex_);
        pendingConfig_ = cfg;
        pendingProfileConfigs_ = profiles;
        pendingConfigGeneration_.fetch_add(1, std::memory_order_release);
    }
    PostRefresh();
}
