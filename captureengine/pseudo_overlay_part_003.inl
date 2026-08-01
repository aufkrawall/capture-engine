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

// ---- State updates ----

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

void PseudoOverlay::ApplyPendingConfig() {
    const uint64_t generation = pendingConfigGeneration_.load(std::memory_order_acquire);
    if (generation == appliedConfigGeneration_) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pendingConfigMutex_);
        baseConfig_ = pendingConfig_;
        profileConfigs_ = pendingProfileConfigs_;
    }
    appliedConfigGeneration_ = generation;
}

void PseudoOverlay::ApplyEffectiveConfig(const PseudoOverlayConfig& cfg, const std::string& profileSection) {
    if (PseudoOverlayConfigsEqual(config_, cfg) && activeProfileSection_ == profileSection)
        return;

    bool wasEnabled = config_.enabled;
    const uint32_t prevGraceMs = static_cast<uint32_t>(config_.foregroundAcquireGraceMs);
    const std::string previousProfile = activeProfileSection_;
    config_ = cfg;
    activeProfileSection_ = profileSection;
    lastWarnMsg_.clear();
    sizeWarn_ = {0, 0};

    if (_stricmp(previousProfile.c_str(), activeProfileSection_.c_str()) != 0) {
        LogInfo("[PseudoOverlay] Active DesktopOverlay settings: %s",
                activeProfileSection_.empty() ? "global" : activeProfileSection_.c_str());
    }

    if (wasEnabled && !cfg.enabled) {
        // Disable overlay: hide windows
        DestroyOverlayWindows();
    }

    // If the grace length changed, drop any in-flight grace so the new value is used
    // for the next foreground acquire. Otherwise the helper would happily keep
    // suppressing with the old length.
    if (prevGraceMs != static_cast<uint32_t>(cfg.foregroundAcquireGraceMs)) {
        lastForegroundAcquireTick_ = 0;
        lastForegroundAcquirePid_ = 0;
        hadForegroundTarget_ = false;
        prevGraceActive_ = false;
        foregroundGraceEverStarted_ = false;
        LogInfo("[PseudoOverlay] Foreground grace reset: grace_ms changed %u -> %u", prevGraceMs,
                static_cast<uint32_t>(cfg.foregroundAcquireGraceMs));
    }
}

void PseudoOverlay::SetRecordingStartIntent(RecordingStartIntent intent) {
    requestedStartIntent_.store(intent, std::memory_order_release);
    if (intent != RecordingStartIntent::Idle) {
        recordingNotifyUntil_.store(0, std::memory_order_relaxed);
        recordingNotification_.store(ce::pseudo_overlay::RecordingNotificationKind::None,
                                     std::memory_order_relaxed);
        overloadWarnUntil_.store(0, std::memory_order_relaxed);
        overloadWarnKind_.store(ce::capture_policy::kOverlayWarningNone, std::memory_order_relaxed);
    }
    PostRefresh();
}

void PseudoOverlay::RequestRefresh() {
    PostRefresh();
}

#ifdef CE_UNIT_TESTS
bool PseudoOverlay::WaitForUiIdleForTesting(DWORD timeoutMs) {
    const DWORD threadId = uiThreadId_.load(std::memory_order_acquire);
    if (threadId == 0) {
        return false;
    }
    HANDLE barrier = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!barrier) {
        return false;
    }
    const bool posted = PostThreadMessageW(threadId, kMsgTestBarrier, reinterpret_cast<WPARAM>(barrier), 0) != FALSE;
    const bool signaled = posted && WaitForSingleObject(barrier, timeoutMs) == WAIT_OBJECT_0;
    CloseHandle(barrier);
    return signaled;
}
#endif

void PseudoOverlay::PostRefresh() {
    const DWORD threadId = uiThreadId_.load(std::memory_order_acquire);
    if (threadId != 0) {
        PostThreadMessageW(threadId, kMsgRefresh, 0, 0);
    }
}

void PseudoOverlay::TriggerRecordingHealthWarning(uint32_t warningKind, uint32_t sustainFpsX100) {
    overloadWarnKind_.store(warningKind, std::memory_order_relaxed);
    overloadWarnSustainFpsX100_.store(sustainFpsX100, std::memory_order_relaxed);
    overloadWarnUntil_.store(GetTickCount64() + 5000ULL);
    PostRefresh();
}

void PseudoOverlay::ShowScreenshotNotification(bool succeeded) {
    screenshotNotificationSucceeded_.store(succeeded, std::memory_order_relaxed);
    screenshotNotifyUntil_.store(GetTickCount64() + 2000ULL);
    PostRefresh();
}

void PseudoOverlay::BeginScreenshotCapture() {
    screenshotInProgress_.store(true, std::memory_order_release);
    if (hOv_)
        ShowWindow(hOv_, SW_HIDE);
    if (hWarn_)
        ShowWindow(hWarn_, SW_HIDE);
    LogDebug("[PseudoOverlay] Overlay hidden for screenshot capture");
}

void PseudoOverlay::EndScreenshotCapture() {
    screenshotInProgress_.store(false, std::memory_order_release);
    PostRefresh();
    LogDebug("[PseudoOverlay] Screenshot capture ended, overlay restore requested");
}

void PseudoOverlay::ShowRecordingFinalizingNotification() {
    recordingNotification_.store(ce::pseudo_overlay::RecordingNotificationKind::Finalizing,
                                 std::memory_order_relaxed);
    recordingNotifyUntil_.store(GetTickCount64() + 60000ULL);
    PostRefresh();
}

bool PseudoOverlay::EnsureOverlayWindows() {
    if (hOv_ && hWarn_) {
        return true;
    }

    if (!hInstance_) {
        LogError("[PseudoOverlay] Missing HINSTANCE for overlay window creation");
        return false;
    }

    if (!hOv_) {
        hOv_ = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               kIndicatorClass, "", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInstance_, 0);
        if (!hOv_) {
            LogError("[PseudoOverlay] Failed to create indicator overlay window");
            return false;
        }
    }

    if (!hWarn_) {
        hWarn_ =
            CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            kWarningClass, "", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInstance_, 0);
        if (!hWarn_) {
            LogError("[PseudoOverlay] Failed to create warning overlay window");
            DestroyWindow(hOv_);
            hOv_ = NULL;
            return false;
        }
    }

    EnsureDwmApi();
    if (g_DwmSetWindowAttribute) {
        BOOL peekExclude = TRUE;
        g_DwmSetWindowAttribute(hOv_, DWMWA_EXCLUDED_FROM_PEEK, &peekExclude, sizeof(peekExclude));
        g_DwmSetWindowAttribute(hWarn_, DWMWA_EXCLUDED_FROM_PEEK, &peekExclude, sizeof(peekExclude));
    }

    return true;
}

void PseudoOverlay::DestroyOverlayWindows() {
    if (hWarn_) {
        DestroyWindow(hWarn_);
        hWarn_ = NULL;
    }
    if (hOv_) {
        DestroyWindow(hOv_);
        hOv_ = NULL;
    }
}
