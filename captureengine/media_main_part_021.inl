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

                } else if (!g_Recording && injectWhitelisted) {
                    SetPreferredScreenGrab(false);
                    clearCurrentWgcTarget();
                    LogInfo("[Media] Injection whitelist matched %s; using inject capture", procName.c_str());
                } else if (!g_Recording && isAutoCaptureConfig()) {
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
                } else if (!g_Recording && !isExplicitScreenGrabConfig()) {
                    SetPreferredScreenGrab(false);
                    LogInfo("[Media] Using Inject Mode (explicit/default)");
                }
            }
        }

        // Release preserved encoder textures once the hook signals it no longer uses them.
        // The Vulkan layer clears useEncoderTextures in CleanupCapture (vkDestroyDevice).
        if (!g_Recording && g_pSharedMem && MediaEngine_ReleaseEncoderTextures) {
            static bool lastUseEncoderTextures = false;
            bool curUseEncoderTextures = g_pSharedMem->useEncoderTextures.load(std::memory_order_acquire);
            if (lastUseEncoderTextures && !curUseEncoderTextures) {
                LogInfo("[Media] Game released encoder textures - freeing preserved D3D11/VRAM resources");
                MediaEngine_ReleaseEncoderTextures();
            }
            lastUseEncoderTextures = curUseEncoderTextures;
        }

        bool hasPendingInputs = false;

        if (IsActiveScreenGrab() && g_Recording) {
            if (recordingStartTime == 0) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                recordingStartTime = now.QuadPart;
            }

            if (g_pSharedMem) {
                g_pSharedMem->runtimeState.hostDroppedFrames.store(
                    static_cast<uint32_t>(g_FrameQueue.GetDroppedCount()), std::memory_order_relaxed);
            }
        } else if (!IsActiveScreenGrab() && g_Recording && g_pSharedMem) {
            FrameRingBuffer& ring = g_pSharedMem->frameRing;
            uint32_t wIdx = ring.writeIndex.load(std::memory_order_acquire);
            static uint32_t localReadIdx = 0;
            static bool receivedFirstFrame = false;
            static DWORD injectModeStartTime = 0;
            static bool sessionInitialized = false;

            static bool sharedTexturesCreated = false;

            // Reset session state when a new recording starts
            if (g_InjectSessionReset.exchange(false, std::memory_order_acq_rel)) {
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

            if (!receivedFirstFrame && g_InjectDeliveredFirstFrame.load(std::memory_order_acquire)) {
                receivedFirstFrame = true;
                LogInfo("[Media] Inject delivery confirmed before monitor observed ring activity");
            }

            if (autoWgcHandoff.GetPhase() == ce::capture_handoff::Phase::kStarting) {
                if (receivedFirstFrame) {
                    const auto transition = autoWgcHandoff.OnInjectFrame();
                    if (transition.action == ce::capture_handoff::Action::kStopWgcKeepInject) {
                        StopWgcCapturePipeline();
                        g_RejectInjectFrames.store(false, std::memory_order_release);
                        LogInfo(
                            "[Media] Inject delivered while WGC fallback was warming; WGC standby stopped and "
                            "inject remains authoritative");
                    }
                } else {
                    bool wgcReady = false;
                    bool wgcFailed = false;
                    uint32_t observedFrames = 0;
                    if (auto capture = g_WgcCap.Read()) {
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
                            g_RejectInjectFrames.store(true, std::memory_order_release);
                            g_RetainStandbyWgcFrameForHandoff.store(false, std::memory_order_release);
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
                            g_RejectInjectFrames.store(false, std::memory_order_release);
                            LogWarn(
                                "[Media] WGC fallback stopped before first-frame proof; inject capture remains "
                                "active");
                        }
                    } else if (GetTickCount64() >= autoWgcHandoffDeadlineTick) {
                        const auto transition = autoWgcHandoff.OnWgcReadinessTimeout();
                        if (transition.action == ce::capture_handoff::Action::kStopWgcKeepInject) {
                            StopWgcCapturePipeline();
                            g_RejectInjectFrames.store(false, std::memory_order_release);
                            LogWarn(
                                "[Media] WGC fallback produced no frame within %llums; inject capture remains "
                                "active",
                                static_cast<unsigned long long>(kAutoWgcHandoffReadyTimeoutMs));
                        }
                    }
                }
            }

            if (!receivedFirstFrame && autoWgcHandoff.GetPhase() == ce::capture_handoff::Phase::kIdle &&
                isAutoCaptureConfig() && g_AutoWgcFallbackArmed.load(std::memory_order_acquire) && g_WgcCap) {
                DWORD elapsed = GetTickCount() - injectModeStartTime;
                const uint32_t activeSourcePid = g_pSharedMem ? g_pSharedMem->GetSourcePid() : 0;
                if (ce::capture_policy::ShouldTriggerAutoWgcFallback(
                        receivedFirstFrame, isAutoCaptureConfig(),
                        g_AutoWgcFallbackArmed.load(std::memory_order_acquire), g_WgcCap != nullptr, elapsed,
                        activeSourcePid)) {
                    LogInfo("[Media] No frames from inject mode after %lums - starting WGC standby handoff", elapsed);

                    const auto transition = autoWgcHandoff.Begin();
                    g_AutoWgcFallbackArmed.store(false, std::memory_order_release);
                    ClearStandbyWgcHandoffFrame();
                    g_RetainStandbyWgcFrameForHandoff.store(config.video.useVFR, std::memory_order_release);
                    if (transition.action == ce::capture_handoff::Action::kStartWgcKeepInject &&
                        StartWgcRecordingCapture(config)) {
                        autoWgcHandoffBaselineFrames = 0;
                        autoWgcHandoffDeadlineTick = GetTickCount64() + kAutoWgcHandoffReadyTimeoutMs;
                        LogInfo(
                            "[Media] WGC fallback session started; inject remains active pending first-frame "
                            "proof (timeout=%llums)",
                            static_cast<unsigned long long>(kAutoWgcHandoffReadyTimeoutMs));
                    } else {
                        g_RetainStandbyWgcFrameForHandoff.store(false, std::memory_order_release);
                        ClearStandbyWgcHandoffFrame();
                        autoWgcHandoff.OnWgcFailure();
                        g_RejectInjectFrames.store(false, std::memory_order_release);
                        LogWarn("[Media] WGC fallback failed to start; inject capture remains active");
                    }

                    injectModeStartTime = 0;
                }
            }

            if (!sharedTexturesCreated && g_pSharedMem->GetWidth() > 0 && g_pSharedMem->GetHeight() > 0) {
                if (g_pSharedMem->encoderTextures.ready.load(std::memory_order_acquire)) {
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

        if (g_Recording && (g_FrameQueue.Size() > 0 || hasPendingInputs)) {
            Sleep(1);
        } else {
            Sleep(5);
        }
    }

    PublishMediaScreenGrabTarget(0, nullptr, false, "media process exit");
    StopRecording();

    if (auto capture = g_WgcCap.LockExclusive()) {
        capture->StopCapture();
    }
    if (d3dContext)
        d3dContext->Release();
    d3dDevice = nullptr;

    // Shutdown media engine BEFORE unmapping shared memory to avoid use-after-free
    // (VideoEncoder::CleanupResources accesses pSharedMem during destruction)
    if (mediaEngineReady && MediaEngine_Shutdown) {
        MediaEngine_Shutdown();
        mediaEngineReady = false;
    }
    MediaEngine_Unload();

    // Release every remaining metadata/resource lease while the cross-process
    // mapping is still valid. Normal recording stop already does this, but the
    // process-exit path also covers partial startup failures and shutdowns that
    // occur before a recording becomes active.
    g_FrameQueue.Clear();
    ClearStandbyWgcHandoffFrame();
    ResetLastQueuedFrameCache();

    if (g_InjectFrameReadyEvent) {
        CloseHandle(g_InjectFrameReadyEvent);
        g_InjectFrameReadyEvent = NULL;
    }
    if (g_InjectCaptureShutdownEvent) {
        CloseHandle(g_InjectCaptureShutdownEvent);
        g_InjectCaptureShutdownEvent = NULL;
    }

    if (g_pShmem)
        UnmapViewOfFile(g_pShmem);
    if (g_hMapShmem)
        CloseHandle(g_hMapShmem);

    if (g_pSharedMem)
        UnmapViewOfFile(g_pSharedMem);
    if (g_hMapFile)
        CloseHandle(g_hMapFile);

    LogInfo("[Media] Process exiting");
    return 0;
}
