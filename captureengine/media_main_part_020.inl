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
    };

    auto applyPendingWgcRetarget = [&]() -> bool {
        if (!pendingWgcRetarget.active) {
            return false;
        }

        WgcRetargetRequest request = pendingWgcRetarget;
        pendingWgcRetarget = {};

        const bool restartActiveCapture = g_Recording && IsActiveScreenGrab();
        const auto previousCapture = g_WgcCap.Load();
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
            const auto publishedCapture = g_WgcCap.Load();
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
    };

    auto prepareCaptureForRecordingStart = [&]() {
        std::string processName = refreshActiveConfig(false);
        uint32_t sourcePid = g_pSharedMem ? g_pSharedMem->GetSourcePid() : 0;
        bool injectWhitelisted = isInjectCaptureTarget(processName);

        autoWgcHandoff.Reset();
        autoWgcHandoffBaselineFrames = 0;
        autoWgcHandoffDeadlineTick = 0;
        g_AutoWgcFallbackArmed.store(false, std::memory_order_release);

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
                    const auto profileCapture = g_WgcCap.Read();
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
            g_AutoWgcFallbackArmed.store(fallbackReady, std::memory_order_release);
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
    };

    while (g_Running) {
        // WGC window detection must run BEFORE resolution polling/texture creation.
        // CreateSharedCaptureTextures sets the encoder's LUID device, which conflicts
        // with WGC's shared device. By scanning first, the preferred capture mode is set correctly
        // and we skip the LUID-based texture creation for WGC games.
        if (mediaEngineReady && g_pSharedMem && isAutoCaptureConfig() && !config.wgcWindowTitles.empty() &&
            !IsPreferredScreenGrab() && !g_Recording) {
            DWORD now = GetTickCount();
            if (now - lastEarlyWgcScan > 500) {
                lastEarlyWgcScan = now;
                HWND matchedWindow = FindMatchingWgcWindow(config.wgcWindowTitles);
                if (matchedWindow) {
                    uint32_t sourcePid = g_pSharedMem->GetSourcePid();
                    if (!markInjectPreferredTarget(matchedWindow, sourcePid, "Early WGC scan")) {
                        if (!primeWgcWindowTarget(matchedWindow, false, false)) {
                            LogWarn("[Media] Early WGC window scan matched 0x%p but window init failed", matchedWindow);
                        }
                    }
                }
            }
        }

        // Poll for resolution availability and create encoder textures early.
        // The Vulkan layer sets resolution when it creates the swapchain, then waits
        // for encoder KMT textures. We must create them ASAP to avoid timeout.
        // Skip when using screengrab (WGC) - the encoder should use the shared device.
        if (g_Recording && g_pSharedMem && !IsPreferredScreenGrab() &&
            !g_pSharedMem->encoderTextures.kmtReady.load(std::memory_order_acquire)) {
            uint32_t w = g_pSharedMem->GetWidth();
            uint32_t h = g_pSharedMem->GetHeight();
            uint32_t f = g_pSharedMem->GetFormat();
            if (w > 0 && h > 0) {
                LogInfo("[Media] Resolution available (%dx%d fmt=%d), creating encoder textures", w, h, f);
                if (MediaEngine_CreateSharedCaptureTextures(w, h, f, g_pSharedMem)) {
                    LogInfo("[Media] Encoder KMT textures created (main loop)");
                }
            }
        }

        ProcessCommand cmd;
        char cmdPayload[256] = {};
        if (ipc.PollCommand(cmd, cmdPayload, sizeof(cmdPayload))) {
            switch (cmd) {
                case ProcessCommand::Shutdown:
                    LogInfo("[Media] Shutdown command received");
                    g_Running = false;
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
                case ProcessCommand::StartRecording: {
                    g_AudioOnly = (strcmp(cmdPayload, "audio_only") == 0);
                    if (!ensureMediaEngineReady()) {
                        LogError("[Media] Failed to reinitialize MediaEngine for recording start");
                        PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed,
                                                     "MediaEngine reinitialization");
                        g_AudioOnly = false;
                        ipc.SendResponse(ProcessResponse::Error, "recording_start_failed");
                        break;
                    }
                    prepareCaptureForRecordingStart();
                    PublishMediaScreenGrabTarget(activeConfigSourcePid, d3dDevice,
                                                 !g_AudioOnly && IsPreferredScreenGrab(), "recording start");
                    const bool started = StartRecording(config);
                    if (!started)
                        PublishMediaScreenGrabTarget(0, nullptr, false, "recording start failure");
                    g_AudioOnly = false;  // Reset after StartRecording consumed it
                    ipc.SendResponse(started ? ProcessResponse::RecordingStarted : ProcessResponse::Error,
                                     started ? nullptr : "recording_start_failed");
                    break;
                }
                case ProcessCommand::StopRecording:
                    // A direct stop owns all hook-facing state. Do this even if
                    // startup had not yet reached g_Recording, and consume any
                    // older shared-memory command before this process exits.
                    if (g_pSharedMem) {
                        StoreRelease(g_pSharedMem->runtimeState.cmdStartRecording, false);
                        StoreRelease(g_pSharedMem->runtimeState.cmdStopRecording, false);
                    }
                    SetInjectVideoCaptureRequestedState(false, "authenticated stop request");
                    SetCaptureRequestedState(false);
                    SetRecordingVisibleState(false);
                    PublishMediaScreenGrabTarget(0, nullptr, false, "authenticated stop request");
                    // Accept the authenticated request before potentially lengthy
                    // encoder/mux finalization. The controller may then release its
                    // endpoint while this disposable media process finishes and exits.
                    if (!ipc.SendResponse(ProcessResponse::Ack))
                        LogWarn("[Media] Failed to acknowledge graceful recording stop; finalizing anyway");
                    StopRecording();
                    releaseIdleWgcResources();
                    // Exit after recording stops to free GPU VRAM.
                    // Controller respawns on next recording via EnsureMediaProcessReady.
                    LogInfo("[Media] Recording finished, exiting to release GPU resources");
                    g_Running = false;
                    break;
                case ProcessCommand::Ping:
                    ipc.SendResponse(ProcessResponse::Pong);
                    break;
                case ProcessCommand::ReloadConfig: {
                    refreshActiveConfig(true);
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
                }
                default:
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
            }
        }
        if (ipc.HasFatalDisconnect()) {
            LogWarn("[Media] Controller IPC disconnected; stopping for a clean respawn");
            if (g_Recording)
                StopRecording();
            g_Running = false;
            break;
        }

        if (g_PrivacyFailClosedStopRequested.exchange(false, std::memory_order_acq_rel) && g_Recording) {
            LogError("[PrivacyBlackout] Owner thread is stopping the recording after a fail-closed video error");
            StopRecording();
        }

        if (g_pSharedMem) {
            if (g_WgcCap && g_Recording && IsActiveScreenGrab() && g_WgcCap->NeedsReset()) {
                HWND currentWindow = NULL;
                HMONITOR currentMonitor = NULL;
                g_WgcCap->GetTargetIdentity(&currentWindow, &currentMonitor);
                const std::string resetReason = g_WgcCap->ConsumeResetReason();
                queueWgcRetarget(currentWindow, currentMonitor, currentWindow == NULL,
                                 resetReason.empty() ? "runtime reset requested" : resetReason.c_str());
            }

            if (g_WgcCap && g_Recording && IsActiveScreenGrab() && g_WgcCap->IsWindowTarget() &&
                !g_WgcCap->IsTargetWindowValid()) {
                HWND currentWindow = NULL;
                HMONITOR currentMonitor = NULL;
                g_WgcCap->GetTargetIdentity(&currentWindow, &currentMonitor);
                queueWgcRetarget(NULL, currentMonitor, true, "target window invalid during recording");
            }

            if (pendingWgcRetarget.active && g_Recording && IsActiveScreenGrab()) {
                applyPendingWgcRetarget();
            }

            if (LoadAcquire(g_pSharedMem->runtimeState.cmdStartRecording)) {
                StoreRelease(g_pSharedMem->runtimeState.cmdStartRecording, false);
                if (!g_Recording) {
                    // Check audioOnly flag from shared memory (set by controller for audio-only via inject)
                    g_AudioOnly = LoadAcquire(g_pSharedMem->runtimeState.audioOnly);
                    // Always clear after consuming to prevent stale carry-over
                    StoreRelease(g_pSharedMem->runtimeState.audioOnly, false);
                    if (!ensureMediaEngineReady()) {
                        LogError("[Media] Failed to reinitialize MediaEngine for shared-memory recording start");
                        PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed,
                                                     "shared-memory MediaEngine reinitialization");
                    } else {
                        prepareCaptureForRecordingStart();
                        PublishMediaScreenGrabTarget(activeConfigSourcePid, d3dDevice,
                                                     !g_AudioOnly && IsPreferredScreenGrab(),
                                                     "shared-memory recording start");
                        const bool started = StartRecording(config);
                        if (!started)
                            PublishMediaScreenGrabTarget(0, nullptr, false, "shared-memory start failure");
                        g_AudioOnly = false;
                        if (started) {
                            g_pSharedMem->runtimeState.ackRecordingStarted.store(true, std::memory_order_release);
                        }
                    }
                }
            }
            if (LoadAcquire(g_pSharedMem->runtimeState.cmdStopRecording)) {
                StoreRelease(g_pSharedMem->runtimeState.cmdStopRecording, false);
                if (g_Recording) {
                    PublishMediaScreenGrabTarget(0, nullptr, false, "shared-memory stop request");
                    StopRecording();
                    releaseIdleWgcResources();
                    g_pSharedMem->runtimeState.ackRecordingStopped.store(true, std::memory_order_release);
                }
                // Exit after recording stops to free GPU VRAM.
                LogInfo("[Media] Recording finished (shmem), exiting to release GPU resources");
                g_Running = false;
            }

            DWORD now = GetTickCount();
            if (!g_Recording && !isExplicitInjectConfig() && !isExplicitDxgiDupConfig() &&
                !config.wgcWindowTitles.empty() && (now - lastWindowScanTime > 1000)) {
                lastWindowScanTime = now;
                HWND foundWindow = FindMatchingWgcWindow(config.wgcWindowTitles);

                if (foundWindow) {
                    if (foundWindow != currentCapturedWindow) {
                        LogInfo(
                            "[Media] WGC Trigger: Found window (0x%p) matching config. "
                            "Switching capture...",
                            foundWindow);
                    }
                    if (markInjectPreferredTarget(foundWindow, g_pSharedMem->GetSourcePid(), "WGC trigger")) {
                        continue;
                    }
                    if (!primeWgcWindowTarget(foundWindow, foundWindow != currentCapturedWindow, false)) {
                        LogWarn("[Media] WGC trigger window 0x%p failed to initialize; falling back to monitor target",
                                foundWindow);
                        if (!primeConfiguredMonitorTarget(foundWindow, NULL, config.captureMonitor,
                                                          "configured WGC trigger fallback")) {
                            LogWarn("[Media] WGC trigger ignored: D3D11 device unavailable");
                        }
                    }
                } else if (!foundWindow && currentCapturedWindow != NULL) {
                    if (!IsWindow(currentCapturedWindow)) {
                        LogInfo(
                            "[Media] Captured window 0x%p no longer valid. Reverting "
                            "to monitor/inject.",
                            currentCapturedWindow);
                        clearCurrentWgcTarget();
                        if (!primeConfiguredMonitorTarget(NULL, NULL, config.captureMonitor,
                                                          "invalid WGC window fallback")) {
                            setWgcPreferenceAfterFailure();
                        }
                    }
                }
            }

            uint32_t currentSourcePid = g_pSharedMem->GetSourcePid();

            if (currentSourcePid != 0 && currentSourcePid != lastSourcePid) {
                lastSourcePid = currentSourcePid;
                std::string procName = refreshActiveConfig(false);
                if (procName.empty()) {
                    procName = GetProcessNameFromPID(currentSourcePid);
                }
                LogInfo("[Media] Hook connected: %s (PID: %u)", procName.c_str(), currentSourcePid);

                const bool injectWhitelisted = isInjectCaptureTarget(procName);
                const bool forceWGC = !isExplicitInjectConfig() && !injectWhitelisted &&
                                      MatchesProcessEntries(config.overlayWhitelist, procName);
                if (forceWGC) {
                    LogInfo("[Media] Overlay-only hook target %s connected; WGC capture remains selected",
                            procName.c_str());
                    if (g_Recording && !IsActiveScreenGrab()) {
                        LogInfo(
                            "[Media] Current recording stays on inject; WGC remains armed "
                            "only as a startup fallback");
                    }
                }

                if (g_Recording && !IsActiveScreenGrab() && injectWhitelisted &&
                    g_InjectDeliveredFirstFrame.load(std::memory_order_acquire) &&
                    g_AutoWgcFallbackArmed.exchange(false, std::memory_order_acq_rel)) {
                    LogInfo("[Media] Inject delivery confirmed for %s; disarming WGC startup fallback",
                            procName.c_str());
                }

                if (!g_Recording && forceWGC) {
                    HWND matchedWindow = (config.wgcWindowTitles.empty() || isExplicitDxgiDupConfig())
                                             ? NULL
                                             : FindMatchingWgcWindow(config.wgcWindowTitles);
                    if (matchedWindow) {
                        if (primeWgcWindowTarget(matchedWindow, false, false)) {
                            continue;
                        }
                        LogWarn(
                            "[Media] WGC configured window target 0x%p failed during pre-record scan; "
                            "continuing fallback selection",
                            matchedWindow);
                    }

                    if (!ensureWgcDevice()) {
                        LogWarn("[Media] Overlay whitelist requested WGC but D3D11 device unavailable");
                        setWgcPreferenceAfterFailure();
                        discardCurrentWgcTarget("connected overlay capture device unavailable");
