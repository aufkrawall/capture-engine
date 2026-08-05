#include "media_main_internal.h"

void MediaProcessSession::Loop() {
    while (media_main_g_Running) {
        // WGC window detection must run BEFORE resolution polling/texture creation.
        // CreateSharedCaptureTextures sets the encoder's LUID device, which conflicts
        // with WGC's shared device. By scanning first, the preferred capture mode is set correctly
        // and we skip the LUID-based texture creation for WGC games.
        if (mediaEngineReady && media_main_g_pSharedMem && isAutoCaptureConfig() && !config.wgcWindowTitles.empty() &&
            !IsPreferredScreenGrab() && !media_main_g_Recording) {
            DWORD now = GetTickCount();
            if (now - lastEarlyWgcScan > 500) {
                lastEarlyWgcScan = now;
                HWND matchedWindow = FindMatchingWgcWindow(config.wgcWindowTitles);
                if (matchedWindow) {
                    uint32_t sourcePid = media_main_g_pSharedMem->GetSourcePid();
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
        if (media_main_g_Recording && media_main_g_pSharedMem && !IsPreferredScreenGrab() &&
            !media_main_g_pSharedMem->encoderTextures.kmtReady.load(std::memory_order_acquire)) {
            uint32_t w = media_main_g_pSharedMem->GetWidth();
            uint32_t h = media_main_g_pSharedMem->GetHeight();
            uint32_t f = media_main_g_pSharedMem->GetFormat();
            if (w > 0 && h > 0) {
                LogInfo("[Media] Resolution available (%dx%d fmt=%d), creating encoder textures", w, h, f);
                if (MediaEngine_CreateSharedCaptureTextures(w, h, f, media_main_g_pSharedMem)) {
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
                    media_main_g_Running = false;
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
                case ProcessCommand::StartRecording: {
                    media_main_g_AudioOnly = (strcmp(cmdPayload, "audio_only") == 0);
                    if (!ensureMediaEngineReady()) {
                        LogError("[Media] Failed to reinitialize MediaEngine for recording start");
                        PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed,
                                                     "MediaEngine reinitialization");
                        media_main_g_AudioOnly = false;
                        ipc.SendResponse(ProcessResponse::Error, "recording_start_failed");
                        break;
                    }
                    prepareCaptureForRecordingStart();
                    PublishMediaScreenGrabTarget(activeConfigSourcePid, d3dDevice,
                                                 !media_main_g_AudioOnly && IsPreferredScreenGrab(), "recording start");
                    const bool started = StartRecording(config);
                    if (!started)
                        PublishMediaScreenGrabTarget(0, nullptr, false, "recording start failure");
                    media_main_g_AudioOnly = false;  // Reset after StartRecording consumed it
                    ipc.SendResponse(started ? ProcessResponse::RecordingStarted : ProcessResponse::Error,
                                     started ? nullptr : "recording_start_failed");
                    break;
                }
                case ProcessCommand::StopRecording:
                    // A direct stop owns all hook-facing state. Do this even if
                    // startup had not yet reached g_Recording, and consume any
                    // older shared-memory command before this process exits.
                    if (media_main_g_pSharedMem) {
                        StoreRelease(media_main_g_pSharedMem->runtimeState.cmdStartRecording, false);
                        StoreRelease(media_main_g_pSharedMem->runtimeState.cmdStopRecording, false);
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
                    media_main_g_Running = false;
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
            if (media_main_g_Recording)
                StopRecording();
            media_main_g_Running = false;
            break;
        }

        if (media_main_g_PrivacyFailClosedStopRequested.exchange(false, std::memory_order_acq_rel) && media_main_g_Recording) {
            LogError("[PrivacyBlackout] Owner thread is stopping the recording after a fail-closed video error");
            StopRecording();
        }

        if (media_main_g_pSharedMem) {
            if (media_main_g_WgcCap && media_main_g_Recording && IsActiveScreenGrab() && media_main_g_WgcCap->NeedsReset()) {
                HWND currentWindow = NULL;
                HMONITOR currentMonitor = NULL;
                media_main_g_WgcCap->GetTargetIdentity(&currentWindow, &currentMonitor);
                const std::string resetReason = media_main_g_WgcCap->ConsumeResetReason();
                queueWgcRetarget(currentWindow, currentMonitor, currentWindow == NULL,
                                 resetReason.empty() ? "runtime reset requested" : resetReason.c_str());
            }

            if (media_main_g_WgcCap && media_main_g_Recording && IsActiveScreenGrab() && media_main_g_WgcCap->IsWindowTarget() &&
                !media_main_g_WgcCap->IsTargetWindowValid()) {
                HWND currentWindow = NULL;
                HMONITOR currentMonitor = NULL;
                media_main_g_WgcCap->GetTargetIdentity(&currentWindow, &currentMonitor);
                queueWgcRetarget(NULL, currentMonitor, true, "target window invalid during recording");
            }

            if (pendingWgcRetarget.active && media_main_g_Recording && IsActiveScreenGrab()) {
                applyPendingWgcRetarget();
            }

            if (LoadAcquire(media_main_g_pSharedMem->runtimeState.cmdStartRecording)) {
                StoreRelease(media_main_g_pSharedMem->runtimeState.cmdStartRecording, false);
                if (!media_main_g_Recording) {
                    // Check audioOnly flag from shared memory (set by controller for audio-only via inject)
                    media_main_g_AudioOnly = LoadAcquire(media_main_g_pSharedMem->runtimeState.audioOnly);
                    // Always clear after consuming to prevent stale carry-over
                    StoreRelease(media_main_g_pSharedMem->runtimeState.audioOnly, false);
                    if (!ensureMediaEngineReady()) {
                        LogError("[Media] Failed to reinitialize MediaEngine for shared-memory recording start");
                        PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed,
                                                     "shared-memory MediaEngine reinitialization");
                    } else {
                        prepareCaptureForRecordingStart();
                        PublishMediaScreenGrabTarget(activeConfigSourcePid, d3dDevice,
                                                     !media_main_g_AudioOnly && IsPreferredScreenGrab(),
                                                     "shared-memory recording start");
                        const bool started = StartRecording(config);
                        if (!started)
                            PublishMediaScreenGrabTarget(0, nullptr, false, "shared-memory start failure");
                        media_main_g_AudioOnly = false;
                        if (started) {
                            media_main_g_pSharedMem->runtimeState.ackRecordingStarted.store(true, std::memory_order_release);
                        }
                    }
                }
            }
            if (LoadAcquire(media_main_g_pSharedMem->runtimeState.cmdStopRecording)) {
                StoreRelease(media_main_g_pSharedMem->runtimeState.cmdStopRecording, false);
                if (media_main_g_Recording) {
                    PublishMediaScreenGrabTarget(0, nullptr, false, "shared-memory stop request");
                    StopRecording();
                    releaseIdleWgcResources();
                    media_main_g_pSharedMem->runtimeState.ackRecordingStopped.store(true, std::memory_order_release);
                }
                // Exit after recording stops to free GPU VRAM.
                LogInfo("[Media] Recording finished (shmem), exiting to release GPU resources");
                media_main_g_Running = false;
            }

            DWORD now = GetTickCount();
            if (!media_main_g_Recording && !isExplicitInjectConfig() && !isExplicitDxgiDupConfig() &&
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
                    if (markInjectPreferredTarget(foundWindow, media_main_g_pSharedMem->GetSourcePid(), "WGC trigger")) {
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

            uint32_t currentSourcePid = media_main_g_pSharedMem->GetSourcePid();

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
                    if (media_main_g_Recording && !IsActiveScreenGrab()) {
                        LogInfo(
                            "[Media] Current recording stays on inject; WGC remains armed "
                            "only as a startup fallback");
                    }
                }

                if (media_main_g_Recording && !IsActiveScreenGrab() && injectWhitelisted &&
                    media_main_g_InjectDeliveredFirstFrame.load(std::memory_order_acquire) &&
                    media_main_g_AutoWgcFallbackArmed.exchange(false, std::memory_order_acq_rel)) {
                    LogInfo("[Media] Inject delivery confirmed for %s; disarming WGC startup fallback",
                            procName.c_str());
                }

                if (!media_main_g_Recording && forceWGC) {
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

                    } else {
                        SetPreferredScreenGrab(true);

                        HWND hGameWindow = GetMainWindowForProcess(currentSourcePid);
                        if (isExplicitDxgiDupConfig()) {
                            if (!primeConfiguredMonitorTarget(hGameWindow, NULL, config.captureMonitor,
                                                              "connected overlay DXGI target")) {
                                LogError("[Media] Failed to initialize selected DXGI monitor target");
                                setWgcPreferenceAfterFailure();
                                discardCurrentWgcTarget("connected overlay DXGI monitor unavailable");
                            }
                            continue;
                        }
                        if (hGameWindow) {
                            LogInfo(
                                "[Media] Overlay-only target: found main window 0x%p. "
                                "Switching WGC to window mode.",
                                hGameWindow);

                            if (primeWgcWindowTarget(hGameWindow, false, false)) {
                                LogInfo("[Media] WGC window target primed for PID %u", currentSourcePid);
                            } else if (primeConfiguredMonitorTarget(hGameWindow, NULL, config.captureMonitor,
                                                                    "connected overlay monitor fallback")) {
                                LogWarn("[Media] Failed to init WGC for window - falling back to monitor capture");
                            } else {
                                LogError("[Media] Failed to init WGC for window or monitor");
                                setWgcPreferenceAfterFailure();
                            }
                        } else {
                            LogInfo(
                                "[Media] Whitelist: No main window found for PID %u. Using "
                                "Monitor Capture.",
                                currentSourcePid);
                            if (!primeConfiguredMonitorTarget(NULL, NULL, config.captureMonitor,
                                                              "connected overlay target without window")) {
                                setWgcPreferenceAfterFailure();
                            }
                        }
                    }

                } else if (!media_main_g_Recording && injectWhitelisted) {
                    SetPreferredScreenGrab(false);
                    clearCurrentWgcTarget();
                    LogInfo("[Media] Injection whitelist matched %s; using inject capture", procName.c_str());
                } else if (!media_main_g_Recording && isAutoCaptureConfig()) {
                    HWND hGameWindow = GetMainWindowForProcess(currentSourcePid);
                    const bool monitorRouteRequested =
                        hGameWindow && ce::capture_policy::ShouldPreferDxgiDuplicationForFullscreenAutoTarget(
                                           isAutoCaptureConfig(), isExplicitInjectConfig(), injectWhitelisted,
                                           IsWindowFullscreenLike(hGameWindow), config.autoFullscreenPrefersDxgiDup);
                    bool monitorRouteSelected = false;
                    if (monitorRouteRequested) {
                        monitorRouteSelected = primeDxgiDupForWindowMonitor(
                            hGameWindow, config.captureMonitor, "unhooked connected source window");
                        if (!monitorRouteSelected && monitorSelectorIsExplicit(config.captureMonitor)) {
                            LogError(
                                "[CaptureTarget] explicit connected-source monitor target failed; refusing window "
                                "fallback");
                            SetPreferredScreenGrab(true);
                            discardCurrentWgcTarget("explicit connected monitor unavailable");
                            continue;
                        }
                    }
                    if (monitorRouteSelected) {
                        // Selected monitor-scope capture for the game's display.
                    } else if (hGameWindow && primeWgcWindowTarget(hGameWindow, false, false)) {
                        LogInfo("[Media] Auto mode: %s is not on the inject whitelist; WGC window capture selected",
                                procName.c_str());
                    } else if (primeConfiguredMonitorTarget(hGameWindow, NULL, config.captureMonitor,
                                                            "connected auto monitor fallback")) {
                        LogInfo("[Media] Auto mode: %s is not on the inject whitelist; WGC monitor capture selected",
                                procName.c_str());
                    } else {
                        setWgcPreferenceAfterFailure();
                        discardCurrentWgcTarget("connected auto monitor unavailable");
                        LogWarn("[Media] Auto mode: WGC target unavailable for %s; no inject capture fallback",
                                procName.c_str());
                    }
                } else if (!media_main_g_Recording && !isExplicitScreenGrabConfig()) {
                    SetPreferredScreenGrab(false);
                    LogInfo("[Media] Using Inject Mode (explicit/default)");
                }
            }
        }

        // Release preserved encoder textures once the hook signals it no longer uses them.
        // The Vulkan layer clears useEncoderTextures in CleanupCapture (vkDestroyDevice).
        if (!media_main_g_Recording && media_main_g_pSharedMem && MediaEngine_ReleaseEncoderTextures) {
            static bool lastUseEncoderTextures = false;
            bool curUseEncoderTextures = media_main_g_pSharedMem->useEncoderTextures.load(std::memory_order_acquire);
            if (lastUseEncoderTextures && !curUseEncoderTextures) {
                LogInfo("[Media] Game released encoder textures - freeing preserved D3D11/VRAM resources");
                MediaEngine_ReleaseEncoderTextures();
            }
            lastUseEncoderTextures = curUseEncoderTextures;
        }

        bool hasPendingInputs = false;

        if (IsActiveScreenGrab() && media_main_g_Recording) {
            if (recordingStartTime == 0) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                recordingStartTime = now.QuadPart;
            }

            if (media_main_g_pSharedMem) {
                media_main_g_pSharedMem->runtimeState.hostDroppedFrames.store(
                    static_cast<uint32_t>(media_main_g_FrameQueue.GetDroppedCount()), std::memory_order_relaxed);
            }
        } else if (!IsActiveScreenGrab() && media_main_g_Recording && media_main_g_pSharedMem) {
            FrameRingBuffer& ring = media_main_g_pSharedMem->frameRing;
            uint32_t wIdx = ring.writeIndex.load(std::memory_order_acquire);
            static uint32_t localReadIdx = 0;
            static bool receivedFirstFrame = false;
            static DWORD injectModeStartTime = 0;
            static bool sessionInitialized = false;

            static bool sharedTexturesCreated = false;

            // Reset session state when a new recording starts
            if (media_main_g_InjectSessionReset.exchange(false, std::memory_order_acq_rel)) {
                sessionInitialized = false;
                localReadIdx = 0;
                receivedFirstFrame = false;
                injectModeStartTime = 0;
                sharedTexturesCreated = false;
            }

            if (!sessionInitialized) {
                localReadIdx = wIdx;
                receivedFirstFrame = false;
                injectModeStartTime = 0;
                sessionInitialized = true;
                LogInfo("[Media] Inject mode session initialized, localReadIdx=%u, wIdx=%u", localReadIdx, wIdx);
            }

            static DWORD lastPollLog = 0;
            if (GetTickCount() - lastPollLog > 1000) {
                LogInfo("[Media] Polling: localReadIdx=%u, wIdx=%u", localReadIdx, wIdx);
                lastPollLog = GetTickCount();
            }

            if (injectModeStartTime == 0) {
                injectModeStartTime = GetTickCount();
                receivedFirstFrame = false;
            }

            if (!receivedFirstFrame && media_main_g_InjectDeliveredFirstFrame.load(std::memory_order_acquire)) {
                receivedFirstFrame = true;
                LogInfo("[Media] Inject delivery confirmed before monitor observed ring activity");
            }

            if (autoWgcHandoff.GetPhase() == ce::capture_handoff::Phase::kStarting) {
                if (receivedFirstFrame) {
                    const auto transition = autoWgcHandoff.OnInjectFrame();
                    if (transition.action == ce::capture_handoff::Action::kStopWgcKeepInject) {
                        StopWgcCapturePipeline();
                        media_main_g_RejectInjectFrames.store(false, std::memory_order_release);
                        LogInfo(
                            "[Media] Inject delivered while WGC fallback was warming; WGC standby stopped and "
                            "inject remains authoritative");
                    }
                } else {
                    bool wgcReady = false;
                    bool wgcFailed = false;
                    uint32_t observedFrames = 0;
                    if (auto capture = media_main_g_WgcCap.Read()) {
                        observedFrames = capture->GetCallbackFrameCount();
                        wgcFailed = !capture->IsCapturing() || capture->NeedsReset();
                        wgcReady = !wgcFailed && (config.video.useVFR ? HasStandbyWgcHandoffFrame()
                                                                      : observedFrames > autoWgcHandoffBaselineFrames);
                    } else {
                        wgcFailed = true;
                    }

                    if (wgcReady) {
                        const auto transition = autoWgcHandoff.OnWgcFirstFrame();
                        if (transition.action == ce::capture_handoff::Action::kCommitWgcStopInject) {
                            // Invalidate every cached/queued frame from the old
                            // inject/WGC lineage before the encoder observes the
                            // new active path. The CFR/audio clock itself is not
                            // restarted.
                            // Keep the standby capture's publication epoch. Its
                            // proven first frame already carries that identity;
                            // advancing here would discard the proof, clear the
                            // inject repeat cache, and create a handoff hole
                            // before any replacement-epoch frame exists.
                            media_main_g_RejectInjectFrames.store(true, std::memory_order_release);
                            media_main_g_RetainStandbyWgcFrameForHandoff.store(false, std::memory_order_release);
                            QueuedFrame retainedVfrFrame;
                            const bool hasRetainedVfrFrame =
                                config.video.useVFR && TakeStandbyWgcHandoffFrame(retainedVfrFrame);
                            SetActiveScreenGrab(true);
                            SetInjectVideoCaptureRequestedState(false, "auto inject-to-WGC handoff committed");
                            if (hasRetainedVfrFrame) {
                                SubmitWgcQueuedFrame(std::move(retainedVfrFrame));
                            }
                            StopInjectCapturePipeline();
                            LogInfo(
                                "[Media] Active recording path switched to WGC after first-frame proof "
                                "(inputFrames=%u); inject stopped only after the replacement was delivering",
                                observedFrames);
                        }
                    } else if (wgcFailed) {
                        const auto transition = autoWgcHandoff.OnWgcFailure();
                        if (transition.action == ce::capture_handoff::Action::kStopWgcKeepInject) {
                            StopWgcCapturePipeline();
                            media_main_g_RejectInjectFrames.store(false, std::memory_order_release);
                            LogWarn(
                                "[Media] WGC fallback stopped before first-frame proof; inject capture remains "
                                "active");
                        }
                    } else if (GetTickCount64() >= autoWgcHandoffDeadlineTick) {
                        const auto transition = autoWgcHandoff.OnWgcReadinessTimeout();
                        if (transition.action == ce::capture_handoff::Action::kStopWgcKeepInject) {
                            StopWgcCapturePipeline();
                            media_main_g_RejectInjectFrames.store(false, std::memory_order_release);
                            LogWarn(
                                "[Media] WGC fallback produced no frame within %llums; inject capture remains "
                                "active",
                                static_cast<unsigned long long>(kAutoWgcHandoffReadyTimeoutMs));
                        }
                    }
                }
            }

            if (!receivedFirstFrame && autoWgcHandoff.GetPhase() == ce::capture_handoff::Phase::kIdle &&
                isAutoCaptureConfig() && media_main_g_AutoWgcFallbackArmed.load(std::memory_order_acquire) && media_main_g_WgcCap) {
                DWORD elapsed = GetTickCount() - injectModeStartTime;
                const uint32_t activeSourcePid = media_main_g_pSharedMem ? media_main_g_pSharedMem->GetSourcePid() : 0;
                if (ce::capture_policy::ShouldTriggerAutoWgcFallback(
                        receivedFirstFrame, isAutoCaptureConfig(),
                        media_main_g_AutoWgcFallbackArmed.load(std::memory_order_acquire), media_main_g_WgcCap != nullptr, elapsed,
                        activeSourcePid)) {
                    LogInfo("[Media] No frames from inject mode after %lums - starting WGC standby handoff", elapsed);

                    const auto transition = autoWgcHandoff.Begin();
                    media_main_g_AutoWgcFallbackArmed.store(false, std::memory_order_release);
                    ClearStandbyWgcHandoffFrame();
                    media_main_g_RetainStandbyWgcFrameForHandoff.store(config.video.useVFR, std::memory_order_release);
                    if (transition.action == ce::capture_handoff::Action::kStartWgcKeepInject &&
                        StartWgcRecordingCapture(config)) {
                        autoWgcHandoffBaselineFrames = 0;
                        autoWgcHandoffDeadlineTick = GetTickCount64() + kAutoWgcHandoffReadyTimeoutMs;
                        LogInfo(
                            "[Media] WGC fallback session started; inject remains active pending first-frame "
                            "proof (timeout=%llums)",
                            static_cast<unsigned long long>(kAutoWgcHandoffReadyTimeoutMs));
                    } else {
                        media_main_g_RetainStandbyWgcFrameForHandoff.store(false, std::memory_order_release);
                        ClearStandbyWgcHandoffFrame();
                        autoWgcHandoff.OnWgcFailure();
                        media_main_g_RejectInjectFrames.store(false, std::memory_order_release);
                        LogWarn("[Media] WGC fallback failed to start; inject capture remains active");
                    }

                    injectModeStartTime = 0;
                }
            }

            if (!sharedTexturesCreated && media_main_g_pSharedMem->GetWidth() > 0 && media_main_g_pSharedMem->GetHeight() > 0) {
                if (media_main_g_pSharedMem->encoderTextures.ready.load(std::memory_order_acquire)) {
                    sharedTexturesCreated = true;
                }
            }

            if (wIdx > localReadIdx + FRAME_RING_SIZE) {
                uint32_t newReadIdx = wIdx - FRAME_RING_SIZE;
                localReadIdx = newReadIdx;
            }

            if (localReadIdx < wIdx) {
                if (!receivedFirstFrame) {
                    receivedFirstFrame = true;
                    LogInfo("[Media] First frame detected (monitoring)");
                }
                localReadIdx = wIdx;
            }

            hasPendingInputs = false;
        } else {
            recordingStartTime = 0;
        }

        if (media_main_g_Recording && (media_main_g_FrameQueue.Size() > 0 || hasPendingInputs)) {
            Sleep(1);
        } else {
            Sleep(5);
        }
    }

    PublishMediaScreenGrabTarget(0, nullptr, false, "media process exit");
    StopRecording();

}

bool MediaProcessSession::ensureWgcDevice() {
    if (!ensureMediaEngineReady()) {
        return false;
    }
    if (d3dDevice) {
        return true;
    }
    d3dDevice = MediaEngine_GetD3D11Device();
    if (!d3dDevice) {
        return false;
    }
    ApplyMediaGpuSchedulingPriorityForDevice(config, d3dDevice);
    if (!d3dContext) {
        d3dDevice->GetImmediateContext(&d3dContext);
    }
    return true;
}


void MediaProcessSession::releaseIdleWgcResources() {
    StopWgcCapturePipeline();
    PublishWgcCapture(nullptr, "idle WGC resource release");
    if (d3dContext) {
        d3dContext->ClearState();
        d3dContext->Flush();
        d3dContext->Release();
        d3dContext = nullptr;
    }
    d3dDevice = nullptr;
    if (MediaEngine_ReleaseSharedD3D11Device) {
        MediaEngine_ReleaseSharedD3D11Device();
    }
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    LogInfo("[Media] Released idle WGC D3D11 resources");
}


