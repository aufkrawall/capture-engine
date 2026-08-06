#include "media_main_internal.h"

std::string MediaProcessSession::refreshActiveConfig(bool forceReload, HWND targetWindow , uint32_t confirmedPid ,
                                   const std::string& confirmedProcessName) {
    uint32_t sourcePid = 0;
    std::string processName;
    if (media_main_g_pSharedMem) {
        sourcePid = media_main_g_pSharedMem->GetSourcePid();
        if (sourcePid != 0) {
            processName = GetProcessNameFromPID(sourcePid);
            if (processName.empty() && sourcePid == activeConfigSourcePid)
                processName = activeConfigProcessName;
        }
    }
    if (sourcePid == 0) {
        HWND resolvedWindow = targetWindow;
        if (!resolvedWindow && currentCapturedWindow && IsWindow(currentCapturedWindow))
            resolvedWindow = currentCapturedWindow;
        if (resolvedWindow) {
            DWORD resolvedPid = 0;
            GetWindowThreadProcessId(resolvedWindow, &resolvedPid);
            sourcePid = resolvedPid;
        }
        if (sourcePid != 0) {
            if (sourcePid == confirmedPid && !confirmedProcessName.empty())
                processName = confirmedProcessName;
            else
                processName = GetProcessNameFromPID(sourcePid);
            if (processName.empty() && sourcePid == activeConfigSourcePid)
                processName = activeConfigProcessName;
        }
    }
    if (sourcePid == 0 && media_main_g_Recording && activeConfigSourcePid != 0) {
        sourcePid = activeConfigSourcePid;
        processName = activeConfigProcessName;
    }

    if (!forceReload && sourcePid == activeConfigSourcePid && processName == activeConfigProcessName) {
        return processName;
    }

    AppConfig resolvedConfig;
    LoadConfig(configPath, resolvedConfig);
    if (!processName.empty()) {
        LoadConfig(configPath, resolvedConfig, processName);
    }
    // Normalize runtime-only sync state before comparing with the active config. Otherwise a
    // config.ini reload with no real media changes would compare file defaults (usually 0 ms)
    // against the active auto-detected render latency and reload unnecessarily.
    ApplyAutoDetectedRenderLatencyToConfig(resolvedConfig);

    const bool mediaConfigChanged = !MediaEngineConfigEquals(config, resolvedConfig);

    config = std::move(resolvedConfig);
    Log_SetLevel(config.logLevel);
    activeConfigSourcePid = sourcePid;
    activeConfigProcessName = processName;

    if (media_main_g_Recording && IsActiveScreenGrab())
        PublishMediaScreenGrabTarget(activeConfigSourcePid, d3dDevice, true, "active profile refresh");

    ApplyMediaPrioritySettings(config);
    if (d3dDevice) {
        ApplyMediaGpuSchedulingPriorityForDevice(config, d3dDevice);
    } else {
        ApplyMediaGpuSchedulingPriorityForSharedAdapter(config);
    }
    if (auto capture = media_main_g_WgcCap.Read()) {
        applyWgcOptions(capture.get());
        capture->SetCaptureCursor(ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
    }
    if (mediaEngineReady) {
        MediaEngine_SetLogCallback(IsDebugLoggingEnabled(config.logLevel) ? MediaLogCallback : nullptr);
        if (forceReload || mediaConfigChanged) {
            MediaEngine_ReloadConfig(&config);
        }
        if (media_main_g_pSharedMem || media_main_g_pShmem) {
            MediaEngine_SetSharedMem(media_main_g_pSharedMem, media_main_g_pShmem);
        }
    }
    return processName;
}


bool MediaProcessSession::markInjectPreferredTarget(HWND targetWindow, uint32_t sourcePid, const char* reason) {
    if (isExplicitScreenGrabConfig()) {
        return false;
    }

    if (!isInjectCaptureTargetForSource(sourcePid)) {
        return false;
    }

    if (!ShouldPreferInjectCaptureForFullscreenWindow(targetWindow, sourcePid)) {
        return false;
    }

    if (currentTargetPrefersInject && currentCapturedWindow == targetWindow) {
        SetPreferredScreenGrab(false);
        return true;
    }

    currentCapturedWindow = targetWindow;
    currentCapturedMonitorStableId.clear();
    currentTargetPrefersInject = true;
    SetPreferredScreenGrab(false);
    LogInfo("[Media] %s: hooked fullscreen-like window 0x%p will use inject capture instead of WGC", reason,
            targetWindow);
    return true;
}


bool MediaProcessSession::primeWgcMonitorTarget(HMONITOR targetMonitor) {
    if (isExplicitInjectConfig() || !targetMonitor) {
        return false;
    }

    // Monitor-scope backend priority: DXGI Desktop Duplication is the
    // preferred pure desktop/monitor capture path (explicit dxgi_dup, or
    // auto mode where no inject/window target exists). Explicit wgc keeps
    // the WGC monitor item; duplication failures always fall back to WGC.
    const bool preferDuplication = ce::capture_policy::ShouldPreferDxgiDuplicationForMonitorCapture(
        isExplicitDxgiDupConfig(), isExplicitWgcConfig(), isAutoCaptureConfig());

    {
        const auto existingCapture = media_main_g_WgcCap.Read();
        HWND existingWindow = NULL;
        HMONITOR existingMonitor = NULL;
        if (existingCapture)
            existingCapture->GetTargetIdentity(&existingWindow, &existingMonitor);
        if (targetMonitor == existingMonitor && existingWindow == NULL && currentCapturedWindow == NULL &&
            !currentTargetPrefersInject && existingCapture &&
            existingCapture->IsUsingDesktopDuplication() == preferDuplication) {
            applyWgcOptions(existingCapture.get());
            existingCapture->SetCaptureCursor(
                ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
            existingCapture->SetThrottleFlag(nullptr);
            SetPreferredScreenGrab(true);
            return true;
        }
    }

    if (!ensureWgcDevice()) {
        return false;
    }

    auto capture = std::make_shared<WGCCapture>();
    applyWgcOptions(capture.get());
    bool initOk = false;
    if (preferDuplication) {
        initOk = capture->InitForMonitorDuplication(d3dDevice, targetMonitor);
        if (initOk) {
            LogInfo("[Media] Monitor capture backend selected: DXGI duplication (%s)",
                    isExplicitDxgiDupConfig() ? "explicit capture_method=dxgi_dup" : "auto desktop fallback");
        } else {
            LogWarn(
                "[Media] DXGI duplication monitor target unavailable (monitor=0x%p); "
                "falling back to WGC monitor capture",
                targetMonitor);
        }
    }
    if (!initOk && targetMonitor) {
        initOk = capture->InitForMonitor(d3dDevice, targetMonitor);
        if (!initOk) {
            LogWarn("[Media] Failed to init WGC for selected monitor 0x%p; refusing cross-monitor fallback",
                    targetMonitor);
        }
    }
    if (!initOk) {
        return false;
    }

    capture->SetCaptureCursor(ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
    capture->SetThrottleFlag(nullptr);
    PublishWgcCapture(std::move(capture), "monitor retarget");
    SetPreferredScreenGrab(true);
    currentCapturedWindow = NULL;
    currentTargetPrefersInject = false;
    return true;
}


bool MediaProcessSession::primeConfiguredMonitorTarget(HWND targetWindow, HMONITOR targetHint, const std::string& selectorText,
                                            const char* context) {
    ce::monitor_selection::Selector selector;
    if (!ce::monitor_selection::TryParseSelector(selectorText, selector)) {
        LogError("[CaptureTarget] invalid normalized monitor selector '%s' (%s)", selectorText.c_str(), context);
        return false;
    }

    HWND foregroundWindow = NULL;
    if (selector.kind == ce::monitor_selection::SelectorKind::kAuto) {
        const ForegroundWgcWindowCandidate foreground = GetForegroundWgcWindowCandidate();
        if (foreground.usable)
            foregroundWindow = foreground.hwnd;
    }

    ce::monitor_selection::ResolveRequest request;
    request.selector = selector;
    request.targetWindow = targetWindow;
    request.hint = targetHint;
    request.foregroundWindow = foregroundWindow;
    const ce::monitor_selection::ResolveResult resolved = ce::monitor_selection::Resolve(request);
    if (!resolved) {
        LogError(
            "[CaptureTarget] monitor resolution failed: selector=%s context=%s error=%s; refusing fallback to "
            "another display",
            selector.canonical.c_str(), context, resolved.error.c_str());
        return false;
    }

    const ce::monitor_selection::MonitorDescriptor& monitor = resolved.descriptor;
    LogInfo(
        "[CaptureTarget] resolved selector=%s reason=%s context=%s monitor=0x%p id=%s device=%s name=%s "
        "bounds=(%ld,%ld)-(%ld,%ld) primary=%d adapter=%08X:%08X source=%u target=%u",
        selector.canonical.c_str(), resolved.reason.c_str(), context, resolved.monitor, monitor.stableId.c_str(),
        monitor.deviceName.c_str(), monitor.friendlyName.c_str(), monitor.desktopRect.left, monitor.desktopRect.top,
        monitor.desktopRect.right, monitor.desktopRect.bottom, monitor.primary ? 1 : 0,
        static_cast<uint32_t>(monitor.adapterLuid.HighPart), monitor.adapterLuid.LowPart, monitor.sourceId,
        monitor.targetId);
    if (!primeWgcMonitorTarget(resolved.monitor))
        return false;
    currentCapturedMonitorStableId = monitor.stableId;
    return true;
}


bool MediaProcessSession::primePinnedMonitorTarget(HMONITOR previousMonitor, const char* context) {
    if (!currentCapturedMonitorStableId.empty()) {
        return primeConfiguredMonitorTarget(NULL, NULL, "id:" + currentCapturedMonitorStableId, context);
    }
    return primeConfiguredMonitorTarget(NULL, previousMonitor, "auto", context);
}


bool MediaProcessSession::monitorSelectorIsExplicit(const std::string& selectorText) {
    ce::monitor_selection::Selector selector;
    return ce::monitor_selection::TryParseSelector(selectorText, selector) &&
           ce::monitor_selection::IsExplicitSelector(selector);
}


bool MediaProcessSession::primeDxgiDupForWindowMonitor(HWND targetWindow, const std::string& selectorText,
                                            const char* reason) {
    if (isExplicitInjectConfig() || !targetWindow) {
        return false;
    }
    if (!primeConfiguredMonitorTarget(targetWindow, NULL, selectorText, reason))
        return false;

    const auto capture = media_main_g_WgcCap.Read();
    const bool usingDuplication = capture && capture->IsUsingDesktopDuplication();
    LogInfo(
        "[Media] Fullscreen game target captured from its selected monitor via %s (%s hwnd=0x%p)",
        usingDuplication ? "DXGI duplication" : "same-monitor WGC fallback", reason, targetWindow);
    return true;
}


bool MediaProcessSession::primeWgcWindowTarget(HWND targetWindow, bool logPrimed, bool allowMonitorFallback) {
    if (isExplicitInjectConfig()) {
        return false;
    }

    if (!targetWindow) {
        return false;
    }

    {
        const auto existingCapture = media_main_g_WgcCap.Read();
        if (currentCapturedWindow == targetWindow && existingCapture && !currentTargetPrefersInject) {
            applyWgcOptions(existingCapture.get());
            existingCapture->SetCaptureCursor(
                ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
            existingCapture->SetThrottleFlag(nullptr);
            SetPreferredScreenGrab(true);
            return true;
        }
    }

    if (!ensureWgcDevice()) {
        setWgcPreferenceAfterFailure();
        return false;
    }

    auto capture = std::make_shared<WGCCapture>();
    applyWgcOptions(capture.get());
    if (capture->InitForWindow(d3dDevice, targetWindow)) {
        capture->SetCaptureCursor(ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));

        capture->SetThrottleFlag(nullptr);
        PublishWgcCapture(std::move(capture), "window retarget");
        SetPreferredScreenGrab(true);
        currentCapturedWindow = targetWindow;
        currentCapturedMonitorStableId.clear();
        currentTargetPrefersInject = false;
        if (logPrimed) {
            LogInfo("[Media] WGC target primed for window 0x%p", targetWindow);
        }
        return true;
    }

    LogError("[Media] Failed to init WGC for found window 0x%p.", targetWindow);
    currentCapturedWindow = NULL;
    currentTargetPrefersInject = false;
    if (!allowMonitorFallback) {
        return false;
    }

    LogWarn("[Media] Falling back to WGC monitor capture after window init failure");
    if (!primeConfiguredMonitorTarget(targetWindow, NULL, config.captureMonitor, "WGC window fallback")) {
        setWgcPreferenceAfterFailure();
        discardCurrentWgcTarget("WGC window monitor fallback unavailable");
        return false;
    }
    return true;
}


bool MediaProcessSession::applyPendingWgcRetarget() {
    if (!pendingWgcRetarget.active) {
        return false;
    }

    WgcRetargetRequest request = pendingWgcRetarget;
    pendingWgcRetarget = {};

    const bool restartActiveCapture = media_main_g_Recording && IsActiveScreenGrab();
    const auto previousCapture = media_main_g_WgcCap.Load();
    const HWND previousCapturedWindow = currentCapturedWindow;
    const std::string previousCapturedMonitorStableId = currentCapturedMonitorStableId;
    const bool previousTargetPrefersInject = currentTargetPrefersInject;
    const bool previousPreferredScreenGrab = IsPreferredScreenGrab();
    if (restartActiveCapture) {
        StopWgcCapturePipeline();
    }

    auto restorePreviousCapture = [&](const char* failureReason) -> bool {
        if (!previousCapture) {
            LogError("[Media] WGC retarget rollback unavailable (%s): no previous capture", failureReason);
            return false;
        }
        const auto publishedCapture = media_main_g_WgcCap.Load();
        if (publishedCapture.get() != previousCapture.get()) {
            PublishWgcCapture(previousCapture, "retarget rollback");
        }
        currentCapturedWindow = previousCapturedWindow;
        currentCapturedMonitorStableId = previousCapturedMonitorStableId;
        currentTargetPrefersInject = previousTargetPrefersInject;
        SetPreferredScreenGrab(previousPreferredScreenGrab);
        if (restartActiveCapture && !StartWgcRecordingCapture(config)) {
            LogError("[Media] WGC retarget rollback failed to restart previous source (%s)", failureReason);
            return false;
        }
        LogWarn("[Media] WGC retarget rolled back to previous source (%s)", failureReason);
        return true;
    };

    if (!request.preferMonitor && request.window && !IsWindow(request.window)) {
        request.window = NULL;
        request.preferMonitor = true;
    }

    bool primed = false;
    if (!request.preferMonitor && request.window) {
        primed = primeWgcWindowTarget(request.window, true);
    }
    if (!primed) {
        primed = primePinnedMonitorTarget(request.monitor, "runtime monitor retarget");
    }
    if (!primed) {
        LogWarn("[Media] Failed to initialize queued WGC retarget; restoring previous source");
        restorePreviousCapture("replacement initialization failed");
        return false;
    }

    if (restartActiveCapture) {
        if (!StartWgcRecordingCapture(config)) {
            LogError("[Media] Failed to start replacement WGC capture; restoring previous source");
            restorePreviousCapture("replacement start failed");
            return false;
        }
        LogInfo("[Media] WGC capture restarted after retarget");
    }
    return true;
}


void MediaProcessSession::clearCurrentWgcTarget() {
    currentCapturedWindow = NULL;
    currentCapturedMonitorStableId.clear();
    currentTargetPrefersInject = false;
    pendingWgcRetarget = {};
}


void MediaProcessSession::discardCurrentWgcTarget(const char* reason) {
    PublishWgcCapture(nullptr, reason);
    clearCurrentWgcTarget();
}


void MediaProcessSession::queueWgcRetarget(HWND targetWindow, HMONITOR targetMonitor, bool preferMonitor, const char* reason) {
    pendingWgcRetarget.window = targetWindow;
    pendingWgcRetarget.monitor = targetMonitor;
    pendingWgcRetarget.preferMonitor = preferMonitor || targetWindow == NULL;
    pendingWgcRetarget.active = true;
    LogWarn("[Media] Queued WGC retarget: %s (window=0x%p monitor=0x%p monitorOnly=%d)", reason, targetWindow,
            targetMonitor, pendingWgcRetarget.preferMonitor ? 1 : 0);
}


void MediaProcessSession::prepareCaptureForRecordingStart() {
    std::string processName = refreshActiveConfig(false);
    uint32_t sourcePid = media_main_g_pSharedMem ? media_main_g_pSharedMem->GetSourcePid() : 0;
    bool injectWhitelisted = isInjectCaptureTarget(processName);

    autoWgcHandoff.Reset();
    autoWgcHandoffBaselineFrames = 0;
    autoWgcHandoffDeadlineTick = 0;
    media_main_g_AutoWgcFallbackArmed.store(false, std::memory_order_release);

    int profileWgcScore = std::numeric_limits<int>::min();
    int profileDxgiScore = std::numeric_limits<int>::min();
    uint32_t profileWgcPid = 0;
    uint32_t profileDxgiPid = 0;
    std::string profileWgcProcessName;
    std::string profileDxgiProcessName;
    WhitelistEntry profileWgcTarget;
    WhitelistEntry profileDxgiTarget;
    HWND profileWgcWindow = FindMatchingWgcWindow(config.profileWgcTargets, &profileWgcScore, true,
                                                  &profileWgcPid, &profileWgcProcessName, &profileWgcTarget);
    HWND profileDxgiWindow = FindMatchingWgcWindow(config.profileDxgiDupTargets, &profileDxgiScore, true,
                                                   &profileDxgiPid, &profileDxgiProcessName, &profileDxgiTarget);
    const bool useProfileDxgi = profileDxgiWindow && (!profileWgcWindow || profileDxgiScore > profileWgcScore);
    HWND profileWindow = useProfileDxgi ? profileDxgiWindow : profileWgcWindow;
    if (profileWindow) {
        const uint32_t profilePid = useProfileDxgi ? profileDxgiPid : profileWgcPid;
        const std::string& profileProcessName =
            useProfileDxgi ? profileDxgiProcessName : profileWgcProcessName;
        if (sourcePid == 0) {
            sourcePid = profilePid;
        }
        processName = refreshActiveConfig(false, profileWindow, profilePid, profileProcessName);
        injectWhitelisted = isInjectCaptureTarget(processName);
        if (useProfileDxgi) {
            std::string profileMonitorSelector = config.captureMonitor;
            if (const ApplicationProfile* profile = FindApplicationProfileForTarget(config, profileDxgiTarget)) {
                profileMonitorSelector = profile->captureMonitor;
            }
            ce::monitor_selection::Selector parsedProfileSelector;
            const bool explicitProfileMonitor =
                ce::monitor_selection::TryParseSelector(profileMonitorSelector, parsedProfileSelector) &&
                ce::monitor_selection::IsExplicitSelector(parsedProfileSelector);
            if (primeDxgiDupForWindowMonitor(profileWindow, profileMonitorSelector, "application profile")) {
                const auto profileCapture = media_main_g_WgcCap.Read();
                LogInfo("[Media] Application profile selected monitor capture via %s (process=%s hwnd=0x%p)",
                        profileCapture && profileCapture->IsUsingDesktopDuplication() ? "DXGI duplication"
                                                                                      : "same-monitor WGC",
                        processName.empty() ? "unknown" : processName.c_str(), profileWindow);
                return;
            }
            if (explicitProfileMonitor) {
                LogError(
                    "[Media] Application profile's explicit monitor target is unavailable; refusing window or "
                    "primary fallback");
                SetPreferredScreenGrab(true);
                discardCurrentWgcTarget("explicit profile monitor unavailable");
                return;
            }
            if (primeWgcWindowTarget(profileWindow, false, false)) {
                LogWarn("[Media] Application profile DXGI target unavailable; using WGC window capture");
                return;
            }
            LogWarn("[Media] Application profile DXGI and WGC targets failed; continuing fallback selection");
        } else {
            if (primeWgcWindowTarget(profileWindow, false, false)) {
                LogInfo("[Media] Application profile selected WGC window capture (process=%s hwnd=0x%p)",
                        processName.empty() ? "unknown" : processName.c_str(), profileWindow);
                return;
            }
            LogWarn("[Media] Application profile WGC target failed to initialize; continuing fallback selection");
        }
    }

    if (isExplicitInjectConfig()) {
        SetPreferredScreenGrab(false);
        clearCurrentWgcTarget();
        return;
    }

    if (isAutoCaptureConfig() && injectWhitelisted) {
        // Auto mode prefers inject for known-compatible games, but a hook
        // connection is not proof that frames will arrive. Keep a fully
        // initialized screen-grab target for the SAME game/window/monitor
        // as a startup fallback. The generic primary-monitor standby is not
        // sufficient on multi-monitor systems.
        bool fallbackReady = false;
        HWND fallbackWindow = sourcePid != 0 ? GetMainWindowForProcess(sourcePid) : NULL;
        if (!fallbackWindow) {
            const ForegroundWgcWindowCandidate candidate = GetForegroundWgcWindowCandidate();
            // A foreground window is a valid fallback only when it belongs
            // to the requested source process (or no source PID is known).
            // Otherwise a transiently missing game window could silently
            // redirect an auto recording to an unrelated foreground app.
            if (candidate.usable && (sourcePid == 0 || candidate.pid == sourcePid)) {
                fallbackWindow = candidate.hwnd;
            }
        }
        HMONITOR fallbackMonitor =
            fallbackWindow ? MonitorFromWindow(fallbackWindow, MONITOR_DEFAULTTONEAREST) : NULL;
        if (fallbackWindow && IsWindowFullscreenLike(fallbackWindow) && config.autoFullscreenPrefersDxgiDup) {
            fallbackReady =
                primeDxgiDupForWindowMonitor(fallbackWindow, config.captureMonitor, "inject startup fallback");
        }
        const bool explicitMonitorFallbackFailed =
            !fallbackReady && fallbackWindow && IsWindowFullscreenLike(fallbackWindow) &&
            config.autoFullscreenPrefersDxgiDup && monitorSelectorIsExplicit(config.captureMonitor);
        if (explicitMonitorFallbackFailed) {
            LogError(
                "[CaptureTarget] explicit monitor startup fallback is unavailable; leaving inject fallback "
                "unarmed instead of selecting another display or window");
        }
        if (!fallbackReady && !explicitMonitorFallbackFailed && fallbackWindow) {
            fallbackReady = primeWgcWindowTarget(fallbackWindow, false, false);
        }
        if (!fallbackReady && !explicitMonitorFallbackFailed && fallbackMonitor && WGCCapture::IsSupported()) {
            fallbackReady = primeConfiguredMonitorTarget(fallbackWindow, fallbackMonitor, config.captureMonitor,
                                                           "inject startup monitor fallback");
        }
        if (!fallbackReady && !fallbackWindow) {
            LogWarn(
                "[Media] Auto inject fallback could not resolve a window/monitor for source PID %lu; "
                "leaving fallback unarmed instead of capturing an unrelated primary monitor",
                static_cast<unsigned long>(sourcePid));
        }
        media_main_g_AutoWgcFallbackArmed.store(fallbackReady, std::memory_order_release);
        SetPreferredScreenGrab(false);
        LogInfo(
            "[Media] Injection whitelist matched %s; auto mode will use inject capture "
            "(WGC fallback=%s hwnd=0x%p hmon=0x%p)",
            processName.c_str(), fallbackReady ? "armed" : "unavailable", fallbackWindow, fallbackMonitor);
        return;
    }

    if (!config.wgcWindowTitles.empty()) {
        if (isExplicitDxgiDupConfig()) {
            LogInfo("[Media] wgc_window_detection ignored: capture_method=dxgi_dup is monitor-scope only");
        } else {
            HWND matchedWindow = FindMatchingWgcWindow(config.wgcWindowTitles);
            if (matchedWindow) {
                if (markInjectPreferredTarget(matchedWindow, sourcePid, "WGC title match")) {
                    return;
                }
                if (primeWgcWindowTarget(matchedWindow, false, false)) {
                    LogInfo(
                        "[Media] WGC window detection matched configured target; WGC window capture selected "
                        "(hwnd=0x%p)",
                        matchedWindow);
                    return;
                }
                LogWarn(
                    "[Media] WGC configured window target 0x%p failed to initialize; continuing fallback selection",
                    matchedWindow);
            }
        }
    }

    if (sourcePid != 0 && MatchesProcessEntries(config.overlayWhitelist, processName)) {
        if (!ensureWgcDevice()) {
            LogWarn("[Media] Overlay whitelist requested WGC but D3D11 device unavailable");
            setWgcPreferenceAfterFailure();
            discardCurrentWgcTarget("overlay capture device unavailable");
            return;
        }

        SetPreferredScreenGrab(true);
        HWND hGameWindow = GetMainWindowForProcess(sourcePid);
        if (isExplicitDxgiDupConfig()) {
            if (!primeConfiguredMonitorTarget(hGameWindow, NULL, config.captureMonitor,
                                              "overlay target DXGI monitor capture")) {
                setWgcPreferenceAfterFailure();
                discardCurrentWgcTarget("overlay DXGI monitor unavailable");
            }
            return;
        }
        if (hGameWindow) {
            LogInfo("[Media] Overlay-only hook target %s; WGC capture selected", processName.c_str());
            if (!primeWgcWindowTarget(hGameWindow, false, false)) {
                LogWarn("[Media] Overlay-only WGC window target 0x%p failed to initialize; falling back to monitor",
                        hGameWindow);
                if (!primeConfiguredMonitorTarget(hGameWindow, NULL, config.captureMonitor,
                                                  "overlay target monitor fallback")) {
                    setWgcPreferenceAfterFailure();
                    discardCurrentWgcTarget("overlay target monitor fallback unavailable");
                }
            }
        } else if (!primeConfiguredMonitorTarget(NULL, NULL, config.captureMonitor,
                                                 "overlay target without a window")) {
            setWgcPreferenceAfterFailure();
            discardCurrentWgcTarget("overlay monitor unavailable");
        }
        return;
    }

    if (isExplicitScreenGrabConfig()) {
        HWND sourceWindow = sourcePid != 0 ? GetMainWindowForProcess(sourcePid) : NULL;
        if (!primeConfiguredMonitorTarget(sourceWindow, NULL, config.captureMonitor,
                                          "explicit monitor-scope capture")) {
            setWgcPreferenceAfterFailure();
            discardCurrentWgcTarget("explicit monitor unavailable");
        }
        return;
    }

    if (currentCapturedWindow != NULL && !IsWindow(currentCapturedWindow)) {
        clearCurrentWgcTarget();
    }

    if (isAutoCaptureConfig()) {
        if (sourcePid != 0) {
            HWND hGameWindow = GetMainWindowForProcess(sourcePid);
            const bool monitorRouteRequested =
                hGameWindow && ce::capture_policy::ShouldPreferDxgiDuplicationForFullscreenAutoTarget(
                                   isAutoCaptureConfig(), isExplicitInjectConfig(), injectWhitelisted,
                                   IsWindowFullscreenLike(hGameWindow), config.autoFullscreenPrefersDxgiDup);
            if (monitorRouteRequested) {
                if (primeDxgiDupForWindowMonitor(hGameWindow, config.captureMonitor, "unhooked source window"))
                    return;
                if (monitorSelectorIsExplicit(config.captureMonitor)) {
                    LogError("[CaptureTarget] explicit auto-mode monitor target failed; refusing window fallback");
                    SetPreferredScreenGrab(true);
                    discardCurrentWgcTarget("explicit auto monitor unavailable");
                    return;
                }
            }
            if (hGameWindow && primeWgcWindowTarget(hGameWindow, false, false)) {
                LogInfo("[Media] Auto mode: %s is not on the inject whitelist; WGC window capture selected",
                        processName.empty() ? "target" : processName.c_str());
                return;
            }
        }

        ForegroundWgcWindowCandidate foregroundCandidate = GetForegroundWgcWindowCandidate();
        const bool matchedConfiguredWgcWindow = false;
        if (ce::capture_policy::ShouldPreferForegroundFullscreenWindowForAutoWgc(
                isAutoCaptureConfig(), isExplicitInjectConfig(), injectWhitelisted, sourcePid != 0,
                matchedConfiguredWgcWindow, foregroundCandidate.usable, foregroundCandidate.fullscreenLike)) {
            const bool monitorRouteRequested =
                ce::capture_policy::ShouldPreferDxgiDuplicationForFullscreenAutoTarget(
                    isAutoCaptureConfig(), isExplicitInjectConfig(), injectWhitelisted,
                    foregroundCandidate.fullscreenLike, config.autoFullscreenPrefersDxgiDup);
            if (monitorRouteRequested) {
                if (primeDxgiDupForWindowMonitor(foregroundCandidate.hwnd, config.captureMonitor,
                                                 "foreground fullscreen window"))
                    return;
                if (monitorSelectorIsExplicit(config.captureMonitor)) {
                    LogError(
                        "[CaptureTarget] explicit foreground monitor target failed; refusing window fallback");
                    SetPreferredScreenGrab(true);
                    discardCurrentWgcTarget("explicit foreground monitor unavailable");
                    return;
                }
            }
            if (primeWgcWindowTarget(foregroundCandidate.hwnd, false, false)) {
                LogInfo(
                    "[Media] Auto mode: no source PID; foreground fullscreen WGC window capture selected "
                    "(pid=%lu process=%s hwnd=0x%p)",
                    static_cast<unsigned long>(foregroundCandidate.pid), foregroundCandidate.processName.c_str(),
                    foregroundCandidate.hwnd);
                return;
            }
            LogWarn(
                "[Media] Auto mode: foreground fullscreen WGC window init failed "
                "(pid=%lu process=%s hwnd=0x%p); falling back to monitor capture",
                static_cast<unsigned long>(foregroundCandidate.pid), foregroundCandidate.processName.c_str(),
                foregroundCandidate.hwnd);
        } else if (sourcePid == 0) {
            LogInfo(
                "[Media] Auto mode: foreground WGC window candidate not used "
                "(usable=%d fullscreenLike=%d matchedConfiguredWindow=%d configuredEntries=%zu)",
                foregroundCandidate.usable ? 1 : 0, foregroundCandidate.fullscreenLike ? 1 : 0,
                matchedConfiguredWgcWindow ? 1 : 0, config.wgcWindowTitles.size());
        }

        if (primeConfiguredMonitorTarget(foregroundCandidate.usable ? foregroundCandidate.hwnd : NULL, NULL,
                                         config.captureMonitor, "auto monitor fallback")) {
            LogInfo("[Media] Auto mode: no inject whitelist match; WGC monitor capture selected");
        } else {
            setWgcPreferenceAfterFailure();
            discardCurrentWgcTarget("auto monitor target unavailable");
            LogWarn("[Media] Auto mode: WGC target unavailable and inject capture is not allowed for this target");
        }
        return;
    }

    SetPreferredScreenGrab(false);
}

