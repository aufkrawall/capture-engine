#include "pseudo_overlay_internal.h"

PseudoOverlay::~PseudoOverlay() {
    Shutdown();
}

int PseudoOverlay::S(int v) const {
    return static_cast<int>(static_cast<float>(v) * scale_);
}

void PseudoOverlay::UpdateScaleForDpi(UINT dpi) {
    const UINT resolvedDpi = dpi == 0 ? 96u : dpi;
    if (resolvedDpi == currentDpi_ && fontWarn_) {
        return;
    }

    currentDpi_ = resolvedDpi;
    scale_ = static_cast<float>(currentDpi_) / 96.0f;
    sizeWarn_ = {0, 0};
    lastWarnMsg_.clear();

    if (bmWarn_) {
        if (hdcWarn_ && oldBmWarn_) {
            SelectObject(hdcWarn_, oldBmWarn_);
        }
        DeleteObject(bmWarn_);
        bmWarn_ = NULL;
    }

    if (fontWarn_) {
        DeleteObject(fontWarn_);
        fontWarn_ = NULL;
    }

    fontWarn_ =
        CreateFontA(-S(40), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, ANTIALIASED_QUALITY, 0, 0, 0, "Segoe UI");
}

void PseudoOverlay::RefreshActiveProfileConfig() {
    HWND foregroundWindow = GetForegroundWindow();
    DWORD foregroundPid = 0;
    if (foregroundWindow)
        GetWindowThreadProcessId(foregroundWindow, &foregroundPid);

    if (foregroundPid != foregroundPid_ || (foregroundPid != 0 && foregroundProcessName_.empty())) {
        foregroundPid_ = foregroundPid;
        foregroundProcessName_ = pseudo_overlay_QueryProcessName(foregroundPid);
        LogDebug("[PseudoOverlay] Foreground process changed: pid=%lu process=%s", foregroundPid,
                 foregroundProcessName_.empty() ? "unknown" : foregroundProcessName_.c_str());
    }

    const PseudoOverlayApplicationConfig* foregroundProfile =
        ce::pseudo_overlay::FindApplicationConfig(profileConfigs_, foregroundProcessName_);
    foregroundIsTarget_ = ce::pseudo_overlay::IsForegroundWarningTarget(
        foregroundProfile, baseConfig_.processList, foregroundProcessName_);

    const bool recordingActive = ce::recording_indicator::IsVisible(recordingIndicatorState_);
    if (!recordingActive) {
        pinnedProfileSection_.clear();
    } else {
        // Once a hook has published the actual source PID, it is stronger evidence
        // than whichever process happened to own foreground at the hotkey edge.
        const PseudoOverlayApplicationConfig* sourceProfile = nullptr;
        if (EnsureSharedMemoryMapping() && pSharedMem_) {
            const uint32_t sourcePid = pSharedMem_->GetSourcePid();
            if (sourcePid != 0) {
                if (sourcePid != sourceProfilePid_ || sourceProcessName_.empty()) {
                    sourceProfilePid_ = sourcePid;
                    sourceProcessName_ = pseudo_overlay_QueryProcessName(sourcePid);
                }
                sourceProfile = ce::pseudo_overlay::FindApplicationConfig(profileConfigs_, sourceProcessName_);
                if (sourceProfile && !sourceProfile->captureUsesInjection)
                    sourceProfile = nullptr;
            }
        }
        if (sourceProfile && (!foregroundProfile || foregroundProfile->captureUsesInjection)) {
            pinnedProfileSection_ = sourceProfile->section;
        } else if (pinnedProfileSection_.empty() && foregroundProfile) {
            // WGC/DXGI routes have no injected source PID. Pin the foreground
            // profile selected at recording start so Alt+Tab does not change the
            // recording indicator's appearance midway through the session.
            pinnedProfileSection_ = foregroundProfile->section;
        }
    }

    const PseudoOverlayApplicationConfig* activeProfile = nullptr;
    if (!pinnedProfileSection_.empty()) {
        const auto pinned = std::find_if(
            profileConfigs_.begin(), profileConfigs_.end(), [&](const PseudoOverlayApplicationConfig& profile) {
                return _stricmp(profile.section.c_str(), pinnedProfileSection_.c_str()) == 0;
            });
        if (pinned != profileConfigs_.end())
            activeProfile = &*pinned;
        else
            pinnedProfileSection_.clear();
    }
    if (!activeProfile && !recordingActive)
        activeProfile = foregroundProfile;

    ApplyEffectiveConfig(activeProfile ? activeProfile->settings : baseConfig_,
                         activeProfile ? activeProfile->section : std::string{});
}

bool PseudoOverlay::IsForegroundTarget() {
    return foregroundIsTarget_;
}

uint32_t PseudoOverlay::GetForegroundTargetPid() {
    return foregroundPid_;
}

void PseudoOverlay::UpdateForegroundGraceState(bool currentHadTarget, uint32_t currentPid) {
    // Detection: a foreground-acquire transition is either (a) the first time we ever
    // see a whitelisted PID, (b) the PID changed while still whitelisted, or (c) we
    // had no target last tick but have one now. The grace tracking state only
    // advances on these transitions so the grace timer measures from the latest
    // acquire edge, not from init.
    const bool pidChanged = lastForegroundAcquirePid_ != 0 && lastForegroundAcquirePid_ != currentPid;
    const bool firstDetection = lastForegroundAcquireTick_ == 0;
    const bool isTransition = firstDetection || pidChanged || (!hadForegroundTarget_ && currentHadTarget);

    if (currentHadTarget && isTransition) {
        const ULONGLONG now = GetTickCount64();
        lastForegroundAcquireTick_ = now;
        lastForegroundAcquirePid_ = currentPid;
        if (config_.foregroundAcquireGraceMs > 0) {
            foregroundGraceEverStarted_ = true;
            LogInfo("[PseudoOverlay] Foreground grace started pid=%lu grace=%ums (was: hadTarget=%d, prevPid=%lu)",
                    static_cast<unsigned long>(currentPid), config_.foregroundAcquireGraceMs,
                    hadForegroundTarget_ ? 1 : 0,
                    static_cast<unsigned long>(lastForegroundAcquirePid_));
        } else {
            LogDebug("[PseudoOverlay] Foreground grace skipped: grace_ms=0 (pid=%lu)",
                     static_cast<unsigned long>(currentPid));
        }
    } else if (!currentHadTarget) {
        if (hadForegroundTarget_) {
            LogInfo("[PseudoOverlay] Foreground grace aborted: focus_lost (was pid=%lu)",
                    static_cast<unsigned long>(lastForegroundAcquirePid_));
        }
        // Clear the tracking state so the next acquire is treated as a fresh transition.
        lastForegroundAcquireTick_ = 0;
        lastForegroundAcquirePid_ = 0;
    }

    hadForegroundTarget_ = currentHadTarget;
}

ce::pseudo_overlay::FocusGraceDecision PseudoOverlay::EvaluateForegroundGrace(bool currentHadTarget,
                                                                              uint32_t currentPid, ULONGLONG now) {
    const bool recordingChanged = prevRecordingIndicatorState_ != recordingIndicatorState_;
    prevRecordingIndicatorState_ = recordingIndicatorState_;

    auto decision = ce::pseudo_overlay::ComputeFocusGraceDecision(
        now, lastForegroundAcquireTick_, lastForegroundAcquirePid_, currentPid, hadForegroundTarget_, currentHadTarget,
        prevGraceActive_, static_cast<uint32_t>(config_.foregroundAcquireGraceMs), recordingChanged);

    if (decision.justEndedGrace && foregroundGraceEverStarted_) {
        ULONGLONG waited = 0;
        if (lastForegroundAcquireTick_ != 0 && now >= lastForegroundAcquireTick_) {
            waited = now - lastForegroundAcquireTick_;
        }
        LogInfo("[PseudoOverlay] Foreground grace elapsed pid=%lu waited=%lums",
                static_cast<unsigned long>(currentPid),
                static_cast<unsigned long>(waited));
        foregroundGraceEverStarted_ = false;
    }
    if (decision.justStartedGrace && config_.foregroundAcquireGraceMs > 0) {
        // UpdateForegroundGraceState() already logged the start; nothing to do here.
    }

    prevGraceActive_ = decision.graceActive;
    return decision;
}

bool PseudoOverlay::EnsureSharedMemoryMapping() {
    if (!hDiscoveryMap_) {
        hDiscoveryMap_ = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
        if (!hDiscoveryMap_) {
            return false;
        }
    }

    if (!pDiscovery_) {
        pDiscovery_ = (DiscoveryInfo*)MapViewOfFile(hDiscoveryMap_, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
        if (!pDiscovery_) {
            CloseHandle(hDiscoveryMap_);
            hDiscoveryMap_ = NULL;
            return false;
        }
    }

    if (!ValidateDiscoveryInfo(pDiscovery_)) {
        return false;
    }

    const uint32_t injectPid = pDiscovery_->GetInjectPid();
    if (injectPid == 0) {
        if (pSharedMem_) {
            UnmapViewOfFile(pSharedMem_);
            pSharedMem_ = nullptr;
        }
        if (hSharedMemMap_) {
            CloseHandle(hSharedMemMap_);
            hSharedMemMap_ = NULL;
        }
        mappedInjectPid_ = 0;
        return false;
    }

    if (pSharedMem_ && mappedInjectPid_ == injectPid) {
        return true;
    }

    if (pSharedMem_) {
        UnmapViewOfFile(pSharedMem_);
        pSharedMem_ = nullptr;
    }
    if (hSharedMemMap_) {
        CloseHandle(hSharedMemMap_);
        hSharedMemMap_ = NULL;
    }

    wchar_t sharedMemName[64];
    GenerateSharedMemName(sharedMemName, 64, injectPid);
    hSharedMemMap_ = OpenFileMappingW(FILE_MAP_READ, FALSE, sharedMemName);
    if (!hSharedMemMap_) {
        mappedInjectPid_ = 0;
        return false;
    }

    pSharedMem_ = (SharedMemoryLayout*)MapViewOfFile(hSharedMemMap_, FILE_MAP_READ, 0, 0, sizeof(SharedMemoryLayout));
    if (!pSharedMem_) {
        CloseHandle(hSharedMemMap_);
        hSharedMemMap_ = NULL;
        mappedInjectPid_ = 0;
        return false;
    }
    if (!ValidateSharedMemory(pSharedMem_)) {
        LogError("[PseudoOverlay] Rejected incompatible inject shared memory ABI (version=%u size=%u abi=0x%08X)",
                 pSharedMem_->GetVersion(), pSharedMem_->structSize.load(std::memory_order_acquire),
                 pSharedMem_->abiSignature.load(std::memory_order_acquire));
        UnmapViewOfFile(pSharedMem_);
        pSharedMem_ = nullptr;
        CloseHandle(hSharedMemMap_);
        hSharedMemMap_ = NULL;
        mappedInjectPid_ = 0;
        return false;
    }

    mappedInjectPid_ = injectPid;
    return true;
}

bool PseudoOverlay::IsInjectOverlayActive() {
    return EnsureSharedMemoryMapping() &&
           pSharedMem_->runtimeState.HasRuntimeFlag(kCaptureRuntimeFlagInjectOverlayActive);
}

bool PseudoOverlay::IsInjectOverlayPending() {
    return EnsureSharedMemoryMapping() &&
           pSharedMem_->runtimeState.HasRuntimeFlag(kCaptureRuntimeFlagInjectOverlayPending);
}

bool PseudoOverlay::RefreshRecordingState() {
    bool recordingActive = false;
    bool recordingAudioOnly = false;
    bool statusDarkForCapture = false;
    RecordingStartIntent sharedIntent = RecordingStartIntent::Idle;
    const bool haveSharedState = EnsureSharedMemoryMapping() && pSharedMem_;
    if (haveSharedState) {
        recordingActive = pSharedMem_->runtimeState.isRecording.load(std::memory_order_acquire);
        recordingAudioOnly = pSharedMem_->runtimeState.audioOnly.load(std::memory_order_acquire);
        sharedIntent = pSharedMem_->runtimeState.GetRecordingStartIntent();
        statusDarkForCapture =
            pSharedMem_->runtimeState.HasRuntimeFlag(kCaptureRuntimeFlagStatusOverlayDarkForCapture);
    }
#ifdef CE_UNIT_TESTS
    statusDarkForCapture = statusDarkForCapture || forcedStatusDarkForCapture_.load(std::memory_order_acquire);
#endif
    const bool statusDarkChanged = statusDarkForCapture != statusDarkForCapture_;
    if (statusDarkChanged) {
        LogInfo("[PseudoOverlay] Capture-dark request %s (media armed screen-grab capture)",
                statusDarkForCapture ? "engaged" : "released");
        statusDarkForCapture_ = statusDarkForCapture;
    }

    RecordingStartIntent startIntent = sharedIntent;
    if (startIntent == RecordingStartIntent::Idle) {
        startIntent = requestedStartIntent_.load(std::memory_order_acquire);
    }
    const auto nextState =
        ce::recording_indicator::SelectState(recordingActive, recordingAudioOnly, startIntent);
    if (recordingActive) {
        requestedStartIntent_.store(RecordingStartIntent::Idle, std::memory_order_release);
    }

    if (nextState == recordingIndicatorState_) {
        return statusDarkChanged;
    }

    LogInfo("[PseudoOverlay] Recording indicator state %u -> %u",
            static_cast<unsigned>(recordingIndicatorState_), static_cast<unsigned>(nextState));
    recordingIndicatorState_ = nextState;
    publishedRecordingIndicatorState_.store(nextState, std::memory_order_release);
    isRecording_.store(ce::recording_indicator::IsRecording(nextState), std::memory_order_release);
    if (nextState != ce::recording_indicator::State::Idle) {
        warnActive_ = false;
        warnVisible_ = false;
    }
    return true;
}

PseudoOverlay::AnchorInfo PseudoOverlay::ResolveAnchorInfo() {
    AnchorInfo anchor;

    auto isUsableAnchorWindow = [](HWND hwnd) -> bool {
        return hwnd && IsWindow(hwnd) && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == 0;
    };

    HWND sourceWindow = NULL;
    if (EnsureSharedMemoryMapping() && pSharedMem_) {
        sourceWindow = pseudo_overlay_GetMainWindowForProcess(pSharedMem_->GetSourcePid());
        if (!isUsableAnchorWindow(sourceWindow)) {
            sourceWindow = NULL;
        }
    }

    HWND anchorWindow = sourceWindow;
    if (!anchorWindow && isUsableAnchorWindow(this->stickyAnchorWindow_)) {
        anchorWindow = this->stickyAnchorWindow_;
    }

    HMONITOR anchorMonitor = NULL;
    if (anchorWindow) {
        anchorMonitor = MonitorFromWindow(anchorWindow, MONITOR_DEFAULTTONEAREST);
    }

    if (!anchorMonitor && this->stickyAnchorMonitor_) {
        RECT stickyRect = {};
        if (pseudo_overlay_GetMonitorRectForMonitor(this->stickyAnchorMonitor_, &stickyRect)) {
            anchorMonitor = this->stickyAnchorMonitor_;
        }
    }

    if (!anchorMonitor) {
        const HWND foreground = GetForegroundWindow();
        if (isUsableAnchorWindow(foreground)) {
            anchorWindow = foreground;
            anchorMonitor = MonitorFromWindow(anchorWindow, MONITOR_DEFAULTTONEAREST);
        }
    }

    if (!anchorMonitor) {
        anchorMonitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }

    anchor.window = anchorWindow;
    anchor.monitor = anchorMonitor;
    if (!pseudo_overlay_GetMonitorRectForMonitor(anchor.monitor, &anchor.monitorRect)) {
        anchor.monitorRect.left = 0;
        anchor.monitorRect.top = 0;
        anchor.monitorRect.right = GetSystemMetrics(SM_CXSCREEN);
        anchor.monitorRect.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    anchor.dpi = anchorWindow ? pseudo_overlay_GetResolvedWindowDpi(anchorWindow)
                              : (this->stickyAnchorDpi_ ? this->stickyAnchorDpi_ : GetDpiForSystem());
    anchor.fullscreenLike = IsWindowFullscreenLike(anchorWindow);

    if (anchorWindow) {
        if (this->stickyAnchorWindow_ != anchorWindow || this->stickyAnchorMonitor_ != anchor.monitor ||
            this->stickyAnchorDpi_ != anchor.dpi) {
            LogInfo("[PseudoOverlay] Sticky anchor updated: window=%p monitor=%p dpi=%u fullscreenLike=%d",
                    anchorWindow, anchor.monitor, anchor.dpi, anchor.fullscreenLike ? 1 : 0);
        }
        this->stickyAnchorWindow_ = anchorWindow;
        this->stickyAnchorMonitor_ = anchor.monitor;
        this->stickyAnchorDpi_ = anchor.dpi;
    } else if (this->stickyAnchorMonitor_ != anchor.monitor || this->stickyAnchorDpi_ != anchor.dpi) {
        LogInfo("[PseudoOverlay] Sticky anchor monitor fallback: monitor=%p dpi=%u", anchor.monitor, anchor.dpi);
        this->stickyAnchorMonitor_ = anchor.monitor;
        this->stickyAnchorDpi_ = anchor.dpi;
    }

    return anchor;
}
