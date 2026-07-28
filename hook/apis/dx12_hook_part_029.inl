                    return;
                }
            }
            s_startupOverlayActivationStage = StartupOverlayActivationStage::kDelaySyncInitAfterRTVInit;
            s_startupOverlayActivationStageMs = GetTickCount64();
            HookLogImportant(
                "DX12: Startup compat staged activation - RTV init complete, delaying sync init for %llums",
                kStartupOverlayPostRTVInitSettleMs);
            sc3->Release();
            return;
        }

        if (s_startupOverlayActivationStage == StartupOverlayActivationStage::kDelaySyncInitAfterRTVInit &&
            s_startupOverlayActivationStageMs != 0) {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG elapsedSinceRTVInit = now - s_startupOverlayActivationStageMs;
            if (elapsedSinceRTVInit < kStartupOverlayPostRTVInitSettleMs) {
                static std::atomic<int> s_postRtvStageLogCount{0};
                if (s_postRtvStageLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                    HookLogImportant(
                        "DX12: Waiting to initialize staged overlay sync after RTV init for %s (remaining=%llums)",
                        g_ProcessName, kStartupOverlayPostRTVInitSettleMs - elapsedSinceRTVInit);
                }
                sc3->Release();
                return;
            }
        }

        if (!g_State.rtvDescHeap) {
            CreateRTVs(g_Device.load(), sc3, actualBufferCount);
            if (!g_State.rtvDescHeap) {
                HookLogImportant("DX12: RTV initialization failed during staged sync init, keeping overlay deferred");
                sc3->Release();
                return;
            }
        }
        HookLogImportant("DX12: Finalizing staged overlay activation step 2/2 - initializing sync");
        InitOverlaySync(g_Device.load(), desc.BufferCount, gameQueue);
        sc3->Release();

        if (g_State.syncInit) {
            ResetStartupOverlayBackendActivationStage();
            s_startupOverlaySyncInitMs = GetTickCount64();
            HookLogImportant(
                "DX12: Startup compat staged activation - sync init complete, delaying overlay rendering for %llums",
                kStartupOverlayPostSyncInitSettleMs);
            HookLogImportant("DX12: Staged overlay activation completed after backend-only init");
        }
        return;
    }

    // Single log on first frame to verify overlay system is entering
    static int s_firstFrameLogged = 0;
    if (s_firstFrameLogged == 0) {
        s_firstFrameLogged = 1;
        HookLog(
            "DX12: ProcessFrame first call - overlayInit=%d, syncInit=%d, "
            "gameQueue=%p",
            g_State.overlayInit, g_State.syncInit, gameQueue);
    }

    UINT currentBackBufferIdx = 0;
    bool hasCurrentBackBufferIdx = false;
    const bool pendingFocusLossBackbufferWorkHold = ShouldHoldOverlayDrawForPendingFocusLossFence();
    bool focusLossBackgroundDeviceLost = false;
    {
        auto* focusLossDev = g_Device.load(std::memory_order_acquire);
        if (focusLossDev) {
            focusLossBackgroundDeviceLost = FAILED(focusLossDev->GetDeviceRemovedReason());
        }
    }
    const bool focusLossBackgroundUsingDedicatedQueue = g_State.overlayQueue && ShouldUseDedicatedOverlayQueue();
    const bool focusLossBackgroundRuntimeOwnedPresentation =
        g_FGRuntimeOwnsSwapchain || HookHasRuntimeOwnedNativeFGPresentPath() || DXGIShared::DoesFGRuntimeOwnSwapchain();
    const bool focusLossBackgroundSteamDeferredSubmit =
        g_deferOverlaySubmitToSteamECL && !focusLossBackgroundUsingDedicatedQueue;
    const bool focusLossBackgroundFrameGenerationActive =
        g_FGCompat.IsFGActive() || DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    // v8 visibility-gated backbuffer hold.
    //
    // Hold CE backbuffer overlay/capture work ONLY when the swapchain is not
    // presentable (DXGI Present returned OCCLUDED, or the window is minimized /
    // zero-sized). In that state the overlay is not visible to the user anyway,
    // and it is also where the single-monitor Alt+Tab device-hung historically
    // occurred (DXGI tearing down the iflip surfaces). A merely-unfocused but
    // still-visible window (borderless background window, or a window on another
    // monitor) keeps presenting S_OK and MUST keep showing the overlay — that is
    // the behavior a proper inject overlay provides and what the user expects. Focus is no longer a
    // reason to hide the overlay.
    const bool swapchainOccluded = g_SwapchainPresentOccluded.load(std::memory_order_acquire);
    // The first predicate arg means "we have a reliable present-result occlusion signal",
    // which is now true for both the wrapped path (context valid) and the vtable DetourPresent
    // path (g_HaveD3D12PresentResultSignal). This lets vtable-hooked apps engage the
    // invisible-safe not-presentable hold during the Alt+Tab mode switch instead of hanging.
    const bool haveReliablePresentResultSignal =
        s_WrappedPresentFocusLossContext.valid || g_HaveD3D12PresentResultSignal.load(std::memory_order_acquire);
    const bool focusLossBackgroundBackbufferHold =
        ce::dx12_overlay_policy::ShouldHoldD3D12OverlayBackbufferWorkForNonPresentableSwapchain(
            haveReliablePresentResultSignal, !frameDesc.Windowed, swapchainOccluded, iconicWindow, zeroSizedSwapchain,
            focusLossBackgroundFrameGenerationActive, focusLossBackgroundRuntimeOwnedPresentation,
            focusLossBackgroundUsingDedicatedQueue, focusLossBackgroundSteamDeferredSubmit,
            focusLossBackgroundDeviceLost, gameQueue != nullptr);
    if (focusLossBackgroundBackbufferHold) {
        // Keep the device-removal dump window open across the not-presentable
        // period and the following presentable transition (the risky DXGI
        // iflip<->composited mode switch).
        g_FocusLossRecentTransitionPresentWindow.store(kFocusLossRecentTransitionDumpWindowFrames,
                                                       std::memory_order_release);
    }
    // v13: do NOT hold the overlay on focus change — it renders EVERY frame so it
    // never disappears (the user's firm requirement). v10's transition hold both hid
    // the overlay (rejected) and still froze (the hold expired mid-thrash under rapid
    // Alt+Tab). A v11 residency-priority attempt was REVERTED because
    // ID3D12Device1::SetResidencyPriority triggered DXGI_ERROR_INVALID_CALL device
    // removal on init (black screen, logs/20260603_155107). x86 text now uses
    // solid glyph spans so the direct native overlay path no longer samples a
    // CE-owned font resource; focusTransitionActive remains telemetry only and
    // widens the DRED dump window. It does NOT gate the overlay.
    const int focusTransitionHoldRemaining = g_FocusTransitionHoldFrames.load(std::memory_order_acquire);
    const bool focusTransitionActive = ce::dx12_overlay_policy::IsD3D12FocusTransitionTelemetryActive(
        frameDesc.Windowed != 0, focusTransitionHoldRemaining, focusLossBackgroundFrameGenerationActive,
        focusLossBackgroundRuntimeOwnedPresentation, focusLossBackgroundUsingDedicatedQueue,
        focusLossBackgroundSteamDeferredSubmit, focusLossBackgroundDeviceLost, gameQueue != nullptr);
    if (focusTransitionActive) {
        // Widen the device-removal dump window so any residual hang at the mode
        // switch is captured with DRED breadcrumbs.
        g_FocusLossRecentTransitionPresentWindow.store(kFocusLossRecentTransitionDumpWindowFrames,
                                                       std::memory_order_release);
    }
    const bool holdFocusLossBackbufferWork = pendingFocusLossBackbufferWorkHold || focusLossBackgroundBackbufferHold;
    {
        static bool s_focusTransitionHoldActive = false;
        static std::atomic<int> s_focusTransitionHoldLogCount{0};
        if (focusTransitionActive) {
            const int logCount = s_focusTransitionHoldLogCount.fetch_add(1, std::memory_order_relaxed);
            if (!s_focusTransitionHoldActive || logCount < 40 || (logCount % 120) == 0) {
                s_focusTransitionHoldActive = true;
                HookLogImportant(
                    "DX12: Focus-change mode switch active — overlay STILL RENDERING (not held; x86 solid-span text; "
                    "upload-slot fence paces slot reuse) (remaining=%d present=%s#%d queue=%p fg=%p/%lu "
                    "game=%p/%lu foreground=%d)",
                    focusTransitionHoldRemaining,
                    s_WrappedPresentFocusLossContext.presentName ? s_WrappedPresentFocusLossContext.presentName
                                                                 : "Present",
                    s_WrappedPresentFocusLossContext.callCount, gameQueue, foregroundWindow, foregroundPid,
                    frameDesc.OutputWindow, currentProcessId, processHasForeground ? 1 : 0);
            }
        } else if (s_focusTransitionHoldActive) {
            s_focusTransitionHoldActive = false;
            HookLogImportant(
                "DX12: Focus-change mode switch settled (present=%s#%d queue=%p foreground=%d)",
                s_WrappedPresentFocusLossContext.presentName ? s_WrappedPresentFocusLossContext.presentName : "Present",
                s_WrappedPresentFocusLossContext.callCount, gameQueue, processHasForeground ? 1 : 0);
        }
    }
    {
        static bool s_focusLossBackbufferHoldActive = false;
        static std::atomic<int> s_focusLossBackgroundHoldLogCount{0};
        if (focusLossBackgroundBackbufferHold) {
            const int logCount = s_focusLossBackgroundHoldLogCount.fetch_add(1, std::memory_order_relaxed);
            if (!s_focusLossBackbufferHoldActive || logCount < 40 || (logCount % 300) == 0) {
                s_focusLossBackbufferHoldActive = true;
                HookLogImportant(
                    "DX12: Holding overlay/capture backbuffer work while swapchain is NOT presentable "
                    "(occluded=%d iconic=%d zeroSize=%d present=%s#%d queue=%p fg=%p/%lu game=%p/%lu "
                    "sync=%u flags=0x%08X fgActive=%d runtimeOwned=%d dedicated=%d steamDeferred=%d "
                    "deviceLost=%d log=%d); backend/resources preserved, no swapchain backbuffer touch",
                    swapchainOccluded ? 1 : 0, iconicWindow ? 1 : 0, zeroSizedSwapchain ? 1 : 0,
                    s_WrappedPresentFocusLossContext.presentName ? s_WrappedPresentFocusLossContext.presentName
                                                                 : "Present",
                    s_WrappedPresentFocusLossContext.callCount, gameQueue, foregroundWindow, foregroundPid,
                    frameDesc.OutputWindow, currentProcessId, s_WrappedPresentFocusLossContext.syncInterval,
                    s_WrappedPresentFocusLossContext.presentFlags, focusLossBackgroundFrameGenerationActive ? 1 : 0,
                    focusLossBackgroundRuntimeOwnedPresentation ? 1 : 0, focusLossBackgroundUsingDedicatedQueue ? 1 : 0,
                    focusLossBackgroundSteamDeferredSubmit ? 1 : 0, focusLossBackgroundDeviceLost ? 1 : 0,
                    logCount + 1);
            }
        } else if (s_focusLossBackbufferHoldActive) {
            s_focusLossBackbufferHoldActive = false;
            HookLogImportant(
                "DX12: Resuming overlay/capture backbuffer work — swapchain presentable again "
                "(present=%s#%d queue=%p fg=%p/%lu game=%p/%lu foreground=%d recentWindow=%d)",
                s_WrappedPresentFocusLossContext.presentName ? s_WrappedPresentFocusLossContext.presentName : "Present",
                s_WrappedPresentFocusLossContext.callCount, gameQueue, foregroundWindow, foregroundPid,
                frameDesc.OutputWindow, currentProcessId, processHasForeground ? 1 : 0,
                g_FocusLossRecentTransitionPresentWindow.load(std::memory_order_acquire));
        }
    }
    SharedMemoryLayout* captureShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    OverlayConfig captureOverlayCfg = GetActiveDX12OverlayConfig(captureShm);
    const bool captureWantsOverlay = captureOverlayCfg.showOverlay && captureOverlayCfg.captureIncludeOverlay;
    const bool captureUsePostSL = processCapture && g_IPC && g_IPC->IsRecording() && captureWantsOverlay &&
                                  ShouldUseConfirmedPostSLForOverlayIncludedWork(captureOverlayCfg);
    const bool captureAfterOverlay = processCapture && g_IPC && g_IPC->IsRecording() && captureWantsOverlay &&
                                     !captureUsePostSL && !holdFocusLossBackbufferWork;
    const bool captureBeforeOverlay =
        processCapture && g_IPC && g_IPC->IsRecording() && !captureWantsOverlay && !holdFocusLossBackbufferWork;
    bool delayOverlayRenderAfterSyncInit = false;
    bool suppressOverlayRenderForLoadedStartupOverlay = false;
    bool delayOverlayRenderAfterResourcePrime = false;
    bool delayOverlayRenderAfterFirstDrawProbe = false;
    bool delayOverlayRenderAfterResume = false;
    const bool shouldRunStartupOverlayDrawProbe = startupOverlayCompatibilityActive;

    if (allowOverlayRender && !suspendOverlayRender && !s_insideECL && !deferOverlayWorkAfterResume &&
        g_State.overlayInit && g_State.syncInit && s_startupOverlaySyncInitMs != 0) {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG msSinceSyncInit = now - s_startupOverlaySyncInitMs;
        const bool processNeedsRenderDelay = startupOverlayCompatibilityActive;
        const bool actualFGActive = IsActualFrameGenerationActive();
        const bool overlayBackendReady = g_State.overlayInit && g_State.syncInit;
        const char* blockingOverlayModule = ce::overlay_compat::GetStartupBlockingOverlayRenderModuleName();
        const ULONGLONG lastBlockingRenderActivityMs =
            s_lastStartupBlockingRenderModuleActivityMs.load(std::memory_order_acquire);
        const bool hasRecentBlockingRenderActivity = ce::overlay_compat::HasRecentDX12StartupBlockingRenderActivity(
            lastBlockingRenderActivityMs, now, kStartupOverlayRenderModuleQuietPeriodMs);
        if (ce::overlay_compat::ShouldDelayDX12OverlayRenderAfterSyncInit(
                processNeedsRenderDelay, actualFGActive, msSinceSyncInit, kStartupOverlayPostSyncInitSettleMs,
                overlayBackendReady)) {
            static std::atomic<int> s_postSyncStageLogCount{0};
            if (s_postSyncStageLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant("DX12: Waiting to render staged overlay after sync init for %s (remaining=%llums)",
                                 g_ProcessName, kStartupOverlayPostSyncInitSettleMs - msSinceSyncInit);
            }
            delayOverlayRenderAfterSyncInit = true;
        } else if (ce::overlay_compat::ShouldSuppressDX12OverlayRenderForLoadedStartupOverlay(
                       processNeedsRenderDelay, actualFGActive, blockingOverlayModule, msSinceSyncInit,
                       kStartupOverlayLoadedRenderModuleMaxBlockMs, overlayBackendReady)) {
            static std::atomic<int> s_loadedStartupOverlayRenderSuppressLogCount{0};
            if (s_loadedStartupOverlayRenderSuppressLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant(
                    "DX12: Keeping overlay rendering disabled while startup-blocking render module %s remains loaded "
                    "for %s (remaining=%llums)",
                    blockingOverlayModule, g_ProcessName,
                    kStartupOverlayLoadedRenderModuleMaxBlockMs - msSinceSyncInit);
            }
            suppressOverlayRenderForLoadedStartupOverlay = true;
        } else if (ce::overlay_compat::ShouldSuppressDX12OverlayRenderForRecentBlockingRendererActivity(
                       processNeedsRenderDelay, actualFGActive, blockingOverlayModule, hasRecentBlockingRenderActivity,
                       overlayBackendReady)) {
            static std::atomic<int> s_recentBlockingRendererSuppressLogCount{0};
            const ULONGLONG msSinceLastActivity = now - lastBlockingRenderActivityMs;
            if (s_recentBlockingRendererSuppressLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant(
                    "DX12: Keeping overlay rendering disabled while startup-blocking render module %s still shows "
                    "recent D3D12 activity for %s (quietRemaining=%llums)",
                    blockingOverlayModule, g_ProcessName,
                    kStartupOverlayRenderModuleQuietPeriodMs - msSinceLastActivity);
            }
            suppressOverlayRenderForLoadedStartupOverlay = true;
        } else {
            if (blockingOverlayModule && processNeedsRenderDelay && !actualFGActive) {
                if (lastBlockingRenderActivityMs != 0) {
                    HookLogImportant(
                        "DX12: Startup-blocking render module %s has been quiet for %llums; allowing overlay rendering "
                        "for %s",
                        blockingOverlayModule, now - lastBlockingRenderActivityMs, g_ProcessName);
                } else {
                    HookLogImportant(
                        "DX12: Startup-blocking render module %s exceeded the startup safety window with no recent "
                        "activity; allowing overlay rendering for %s",
                        blockingOverlayModule, g_ProcessName);
                }
            } else {
                HookLogImportant("DX12: Startup compat sync settle complete - allowing overlay rendering for %s",
                                 g_ProcessName);
            }
            s_startupOverlaySyncInitMs = 0;
        }
    }

    if (allowOverlayRender && !suspendOverlayRender && !s_insideECL && !deferOverlayWorkAfterResume &&
        g_State.overlayInit && g_State.syncInit && s_startupOverlayResourcePrimeMs != 0) {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG msSinceResourcePrime = now - s_startupOverlayResourcePrimeMs;
        const bool preserveLiveStartupOverlayDuringInactiveSL =
            ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff();
        const bool shouldDelayAfterResourcePrime = ce::dx12_overlay_policy::ShouldDelayAfterStartupOverlayResourcePrime(
            startupOverlayCompatibilityActive, IsActualFrameGenerationActive(), msSinceResourcePrime,
            kStartupOverlayPostResourcePrimeSettleMs, preserveLiveStartupOverlayDuringInactiveSL);
        if (shouldDelayAfterResourcePrime) {
            static std::atomic<int> s_postResourcePrimeLogCount{0};
            if (s_postResourcePrimeLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant(
                    "DX12: Waiting to draw staged overlay after resource priming for %s (remaining=%llums)",
                    g_ProcessName, kStartupOverlayPostResourcePrimeSettleMs - msSinceResourcePrime);
            }
            delayOverlayRenderAfterResourcePrime = true;
        } else {
            if (preserveLiveStartupOverlayDuringInactiveSL) {
                HookLogImportant(
                    "DX12: Skipping startup resource-prime settle delay to keep live overlay visible for %s",
                    g_ProcessName);
            } else {
                HookLogImportant(
                    "DX12: Startup compat resource-prime settle complete - allowing first overlay draw for %s",
                    g_ProcessName);
            }
            s_startupOverlayResourcePrimeMs = 0;
        }
    }

    if (allowOverlayRender && !suspendOverlayRender && !s_insideECL && !deferOverlayWorkAfterResume &&
        g_State.overlayInit && g_State.syncInit && s_startupOverlayFirstDrawProbeMs != 0) {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG msSinceProbe = now - s_startupOverlayFirstDrawProbeMs;
        if (shouldRunStartupOverlayDrawProbe && msSinceProbe < kStartupOverlayFirstDrawProbeSettleMs) {
            static std::atomic<int> s_firstDrawProbeWaitLogCount{0};
            if (s_firstDrawProbeWaitLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant("DX12: Waiting to continue GTA overlay probe with %s for %s (remaining=%llums)",
                                 GetStartupOverlayFirstDrawProbeStageName(s_startupOverlayFirstDrawProbeStage),
                                 g_ProcessName, kStartupOverlayFirstDrawProbeSettleMs - msSinceProbe);
            }
            delayOverlayRenderAfterFirstDrawProbe = true;
        } else {
            HookLogImportant("DX12: GTA overlay probe settle complete - allowing %s for %s",
                             GetStartupOverlayFirstDrawProbeStageName(s_startupOverlayFirstDrawProbeStage),
                             g_ProcessName);
            s_startupOverlayFirstDrawProbeMs = 0;
        }
    }

    if (allowOverlayRender && !suspendOverlayRender && !s_insideECL && g_State.overlayInit && g_State.syncInit &&
        deferOverlayWorkAfterResume) {
        static std::atomic<int> s_postResumeRenderDelayLogCount{0};
        if (s_postResumeRenderDelayLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
            if (runtimeOwnedSwapchainNeedsExtraResumeSettle) {
                HookLogImportant(
                    "DX12: Keeping overlay rendering deferred after startup-overlay resume for %s while the "
                    "runtime-owned swapchain queue is still active",
                    g_ProcessName);
            } else {
                HookLogImportant(
                    "DX12: Keeping overlay rendering deferred after startup-overlay resume for %s (remaining=%llums)",
                    g_ProcessName, postResumeSettleRemainingMs);
            }
        }
        delayOverlayRenderAfterResume = true;
    }

    // =========================================================================
    // FG STATE MANAGEMENT — runs unconditionally when overlay resources exist.
    // =========================================================================
    // Early PostSL registration — break the chicken-and-egg deadlock:
    //   - Reinit guard blocks init during SL FG when PostSL isn't registered
    //   - Outer block registers PostSL but requires overlayInit (which needs init)
    // Fix: register PostSL BEFORE the outer block, regardless of overlayInit.
    // The PostSL callback safely handles !overlayInit (returns early at line 4026).
    // =========================================================================
    if (!observerOnlyMode) {
        bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        if (slFGNow && g_FGTransitionCooldown == 0) {
            if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) !=
                &PostSLOverlayRenderGated) {
                SetPostSLCallbackInstalled(true, "DX12: Early PostSL registration");
                HookLogImportant("DX12: Early PostSL registration (overlayInit=%d syncInit=%d)",
                                 g_State.overlayInit ? 1 : 0, g_State.syncInit ? 1 : 0);
            }
        }
    } else {
        static std::atomic<int> s_observerOnlyEarlyPostSLLogCount{0};
        const int logCount = s_observerOnlyEarlyPostSLLogCount.fetch_add(1, std::memory_order_relaxed);
        if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) &&
            (logCount < 10 || (logCount % 200) == 0)) {
            HookLogImportant(observerStartupPresentOnlyMode
                                 ? "DX12: Observer-startup-present-only mode active - suppressing early PostSL "
                                   "registration while Streamline FG is running"
                                 : "DX12: Observer-only mode active - suppressing early PostSL registration while "
                                   "Streamline FG is running");
        }
    }

    // =========================================================================
    //
    // CRITICAL: This block must run even when overlay RENDERING is blocked
    // (e.g., by the startup overlay blocker setting allowOverlayRender=false).
    // Without it, FG ON↔OFF transitions are missed while the overlay is blocked,
    // causing:
    //   - g_SwapchainQueue stays null → startup blocker keeps blocking forever
    //   - Queue-change heuristic never reset → false FSR FG on next FG cycle
    //   - PostSL warmup counter never reset → stall fallback fires during warmup
    //   - PostSL callback not managed → stale callbacks fire on wrong state
    //
    // The inner block (inside allowOverlayRender gate) also handles transitions
    // but only runs when rendering is allowed.  This outer block is the safety
    // net that ensures state is ALWAYS correct.
    if (observerOnlyMode && g_State.overlayInit && g_State.syncInit) {
        static std::atomic<int> s_observerOnlyDx12StateLogCount{0};
        const int logCount = s_observerOnlyDx12StateLogCount.fetch_add(1, std::memory_order_relaxed);
        if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) &&
            (logCount < 10 || (logCount % 200) == 0)) {
            HookLogImportant(observerStartupPresentOnlyMode
                                 ? "DX12: Observer-startup-present-only mode active - suppressing DX12 overlay/PostSL "
                                   "transition management while Streamline FG is running"
                                 : "DX12: Observer-only mode active - suppressing DX12 overlay/PostSL transition "
                                   "management while Streamline FG is running");
        }
    }

    if (!observerOnlyMode && !s_insideECL && g_State.overlayInit && g_State.syncInit) {
        bool outerSLFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        bool previousOuterSLFGRunning = g_OuterTrackedSLFGRunning.load(std::memory_order_acquire);

        if (outerSLFGRunning != previousOuterSLFGRunning) {
            bool slTurnedOff = previousOuterSLFGRunning && !outerSLFGRunning;
            bool slTurnedOn = !previousOuterSLFGRunning && outerSLFGRunning;
            g_OuterTrackedSLFGRunning.store(outerSLFGRunning, std::memory_order_release);
            const bool preserveActivePostSLOnLateOuterOn =
                slTurnedOn && ce::dx12_overlay_policy::ShouldPreserveActivePostSLDuringFGCooldown(
                                  outerSLFGRunning, g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                                  HookIsPostSLOverlayActiveButUnconfirmed());
            auto* transitionDevice = g_Device.load(std::memory_order_acquire);
            const HRESULT transitionDeviceHr = transitionDevice ? transitionDevice->GetDeviceRemovedReason() : S_OK;
            const bool bypassPureStreamlineOffCooldown =
                ce::dx12_overlay_policy::ShouldBypassPureStreamlineFGOffOverlayReinitCooldown(
                    slTurnedOff, g_HadFSRFGPhase, g_FGCompat.IsFSRFGApiActive(),
                    HookHasRuntimeOwnedNativeFGPresentPath(), g_State.overlayInit, g_State.syncInit,
                    g_SwapchainQueue != nullptr, g_OriginalGameQueue != nullptr, FAILED(transitionDeviceHr));
            // Make-before-break: DX12_OnStreamlineFGStateChanged latched the
            // keep-alive at the explicit OFF edge; the outer teardown must not
            // disable the confirmed PostSL path that is covering the proxy's
            // remaining presents.
            const bool keepConfirmedPostSLAliveAcrossOuterOff =
                slTurnedOff && g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire);
            const auto* lastSuccessfulPostSLSwapchain = g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire);
            const bool preserveConfirmedPostSLProxyResourcesAcrossOuterOff =
                ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLProxyResourcesAcrossOuterOff(
                    slTurnedOff, keepConfirmedPostSLAliveAcrossOuterOff,
                    pSwapChain != nullptr && pSwapChain == lastSuccessfulPostSLSwapchain, g_State.overlayInit,
                    g_State.syncInit, FAILED(transitionDeviceHr));
            // A confirmed-PostSL DLSS-FG SUSPEND (proxy stays live) is safe to rebuild
            // immediately even WITH FSR history: PostSL confirmed rendering means the
            // overlay ECL on the runtime-owned SL queue already succeeded this epoch, so
            // the generic 60-frame reinit cooldown only blanks a provably-live overlay
            // (session 20260613_150750: 60-present / 672 ms blank). The stricter cooldown
            // is kept for any current FSR/native-FG ownership or device-removal.
            const bool bypassConfirmedPostSLSuspensionCooldown =
                ce::dx12_overlay_policy::ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(
                    slTurnedOff, g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire),
                    g_PostSLConfirmedRendering.load(std::memory_order_acquire), g_FGCompat.IsFSRFGApiActive(),
                    HookHasRuntimeOwnedNativeFGPresentPath(), g_State.overlayInit, g_State.syncInit,
                    g_SwapchainQueue != nullptr, g_OriginalGameQueue != nullptr, FAILED(transitionDeviceHr));
            // DLSS-FG -> FSR-FG (no-callback) takeover: the native-FSR takeover path already warm-reinited
            // the overlay on the runtime-owned FSR queue this frame; the [outer] teardown below would
            // force-clear it + 60-frame cooldown (session 20260615_020100: missed=60). Keep it live —
            // strictly the no-callback route (the app-callback bridge keeps the teardown for crash safety).
            const bool keepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover =
                ce::dx12_overlay_policy::ShouldKeepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover(
                    slTurnedOff, g_FGCompat.IsFSRFGApiActive(),
                    g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                    HookHasRuntimeOwnedNativeFGPresentPath(), g_State.overlayInit, g_State.syncInit,
                    FAILED(transitionDeviceHr));
            const bool keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn =
                ce::dx12_overlay_policy::ShouldKeepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn(
                    slTurnedOff, authoritativeDLSSOffNormalReturnReinitializedThisPresent,
                    postFSRNormalRouteOwnershipProven, g_State.overlayInit, g_State.syncInit,
                    FAILED(transitionDeviceHr));
            HookLogImportant("DX12: [outer] SL FG %s (allowOverlayRender=%d keepAlive=%d)", slTurnedOn ? "ON" : "OFF",
                             allowOverlayRender ? 1 : 0, keepConfirmedPostSLAliveAcrossOuterOff ? 1 : 0);

            // Set cooldown — prevents rendering during transition window
            if (preserveActivePostSLOnLateOuterOn) {
                HookLogImportant(
                    "DX12: [outer] SL FG ON after active PostSL — preserving active PostSL path "
                    "instead of re-entering transition cooldown");
            } else if (bypassPureStreamlineOffCooldown || bypassConfirmedPostSLSuspensionCooldown ||
                       keepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover ||
                       keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn) {
                g_FGTransitionCooldown.store(0, std::memory_order_release);
                g_PostSLCooldownRemaining.store(0, std::memory_order_release);
                HookLogImportant(
                    "DX12: [outer] %s — bypassing generic reinit cooldown "
                    "(scQueue=%p origGame=%p devHr=0x%08X)",
                    keepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover
                        ? "DLSS->FSR no-callback takeover (overlay already reinited on FSR queue)"
                    : keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn
                        ? "authoritative DLSS-off native return (overlay already reinited on exact game swapchain)"
                        : (bypassPureStreamlineOffCooldown ? "pure Streamline FG OFF"
                                                           : "confirmed-PostSL DLSS-FG suspension (proxy stays live)"),
                    g_SwapchainQueue, g_OriginalGameQueue, (unsigned)transitionDeviceHr);
            } else {
                g_FGTransitionCooldown = std::max(g_FGTransitionCooldown.load(std::memory_order_acquire), 60);
                g_PostSLCooldownRemaining.store(g_FGTransitionCooldown.load(std::memory_order_acquire),
                                                std::memory_order_release);
            }

            // Reset PostSL state for fresh start after transition.
            // Keep the callback installed on Streamline FG activation so
            // startup synthetic presents can immediately find it.
            if (!preserveActivePostSLOnLateOuterOn && !keepConfirmedPostSLAliveAcrossOuterOff) {
                g_PostSLOverlayActive.store(false, std::memory_order_release);
            }
            if (!DXGIShared::ShouldKeepPostSLCallbackInstalledDuringTransition(outerSLFGRunning) &&
                !keepConfirmedPostSLAliveAcrossOuterOff) {
                SetPostSLCallbackInstalled(false, "DX12: [outer] SL transition");
            }
            g_PostSLStallCounter.store(0, std::memory_order_release);
            if (!preserveActivePostSLOnLateOuterOn && !keepConfirmedPostSLAliveAcrossOuterOff) {
                g_PostSLStableFrameCount.store(0, std::memory_order_release);
                g_PostSLConfirmedRendering.store(false, std::memory_order_release);
                g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
            }

            // Clear false heuristic FSR FG (SL's queues trigger queue-change heuristic)
            if (g_FGCompat.IsHeuristicFSRFGActive()) {
                g_FGCompat.SetHeuristicFSRFGActive(false);
                HookLogImportant("DX12: [outer] Cleared heuristic FSR FG during SL FG %s", slTurnedOn ? "ON" : "OFF");
            }

            // Clear NVIDIA_SM detection state — the cached 2× multiplier from
            // departing DLSS FG would otherwise trigger false NVIDIA_SM detection
            // in DetectPattern() within a few frames.
            g_FGCompat.ClearNvidiaSMState();

            // Reset queue-change heuristic so it re-captures initial queue.
            // If PostSL already confirmed rendering on a late outer ON edge,
            // keep the existing proven queue/routing state instead of forcing a
            // fresh startup-style re-capture that can starve confirmed PostSL.
            if (!preserveActivePostSLOnLateOuterOn) {
                RequestFGDetectionHeuristicReset();
            }

            // Bump epoch so the inner transition handler resyncs its local
            // tracking and skips redundant transition work even when the outer
            // path preserved a proven PostSL state instead of resetting it.
            { g_OuterSLTransitionEpoch.fetch_add(1, std::memory_order_release); }

            if (slTurnedOff) {
                // Suppress queue-change heuristic for frames after SL OFF.
                // The heuristic runs BEFORE this outer block in ProcessFrame, so
                // on the frame SL turns off, it sees queue switch (SL→origGame)
                // before the reset flag is set → false FSR_FG.
                // Use 600 frames (~4s@150fps) to cover high-fps menus where
                // SL's swapchain queue persists after FG teardown.
                g_SLOffHeuristicGrace.store(600, std::memory_order_release);
                g_SLOffSwapchainReinitGrace.store(300, std::memory_order_release);

                // DO NOT restore g_SwapchainQueue to g_OriginalGameQueue here.
                // When SL activates FG, it calls CreateSwapChainForHwnd with its
                // own queue (e.g. F0A0).  After FG teardown, SL's swapchain
                // PERSISTS — the game continues presenting on F0A0, not the
                // original game queue (F620).  Restoring to F620 causes a
                // queue/swapchain mismatch: we'd render to F0A0's backbuffers
                // on F620 → DXGI_ERROR_ACCESS_DENIED → DEVICE_REMOVED.
                //
                // g_SwapchainQueue already holds the correct value from the
                // CreateSwapChainForHwnd hook.  If the game creates a new
                // swapchain later, the hook updates g_SwapchainQueue.
                {
                    std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                    HookLogImportant(
                        "DX12: [outer] FG→off — keeping g_SwapchainQueue=%p "
                        "(origGame=%p) — SL swapchain persists after teardown",
                        g_SwapchainQueue, g_OriginalGameQueue);
                }

                // Disable PostSL immediately — SL is tearing down — unless the
                // make-before-break keep-alive is covering the proxy's
                // remaining presents until the normal route confirms.
                if (!keepConfirmedPostSLAliveAcrossOuterOff) {
                    g_PostSLOverlayActive.store(false, std::memory_order_release);
                    SetPostSLCallbackInstalled(false, "DX12: [outer] FG->off");
                } else {
                    HookLogImportant(
                        "DX12: [outer] FG->off — PostSL keep-alive covers proxy presents until normal-route "
                        "recovery");
                }
                InvalidateAllOverlayCachedFrames();

                // Drain in-flight GPU work
                if (g_State.fence && !preserveConfirmedPostSLProxyResourcesAcrossOuterOff &&
                    !keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn) {
                    UINT64 lastVal = g_State.currentFenceValue;
                    HANDLE drainEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                    if (drainEvent) {
                        HRESULT drainHr = g_State.fence->SetEventOnCompletion(lastVal, drainEvent);
                        if (SUCCEEDED(drainHr)) {
                            DWORD waitResult = WaitForSingleObject(drainEvent, 200);
                            HookLogImportant("DX12: [outer] FG→off GPU drain: fenceVal=%llu wait=%s", lastVal,
                                             waitResult == WAIT_OBJECT_0 ? "OK" : "TIMEOUT");
                        } else {
                            HookLogImportant("DX12: [outer] FG→off GPU drain FAILED: hr=0x%08X", (unsigned)drainHr);
                        }
                        CloseHandle(drainEvent);
                    }
                } else if (preserveConfirmedPostSLProxyResourcesAcrossOuterOff) {
                    HookLogImportant(
                        "DX12: [outer] FG->off — preserving exact confirmed PostSL proxy resources "
                        "(proxy=%p queue=%p; no drain/reinit/copy/wait)",
                        lastSuccessfulPostSLSwapchain, g_PostSLLastWorkingQueue);
                } else if (keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn) {
                    HookLogImportant(
                        "[OVERLAY VISIBILITY] First authoritative DLSS-off native Present keeps its newly rebuilt "
                        "overlay state (swapchain=%p queue=%p; no second drain/reinit)",
                        pSwapChain, g_SwapchainQueue);
                }

                // Force overlay reinit — PostSL's RTVs reference SL's swapchain
                // backbuffers, which become invalid after SL tears down FG.  Without
                // reinit, pre-SL rendering uses stale RTVs → DEVICE_HUNG.
                // EXCEPTION (DLSS->FSR no-callback takeover): the native-FSR takeover path
                // already warm-reinited the overlay on the runtime-owned FSR swapchain queue
                // this same frame (its RTVs are valid for FSR's swapchain, not stale SL ones).
                // Tearing it down here is what produced the 60-present blank — keep it live.
                if (g_State.overlayInit && !keepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover &&
                    !preserveConfirmedPostSLProxyResourcesAcrossOuterOff &&
                    !keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn) {
                    HookLogImportant("DX12: [outer] FG→off — forcing overlay reinit (stale SL backbuffers)");
                    g_State.overlayInit = false;
                    CleanupRTVs();
                } else if (g_State.overlayInit && preserveConfirmedPostSLProxyResourcesAcrossOuterOff) {
                    HookLogImportant(
                        "DX12: [outer] FG->off — exact confirmed PostSL proxy remains current; warm backend stays "
                        "drawable for this transition present");
                } else if (g_State.overlayInit && keepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover) {
                    HookLogImportant(
                        "DX12: [outer] FG→off — DLSS->FSR no-callback takeover already reinited the overlay on the "
                        "runtime-owned FSR queue; keeping it live (no teardown, no cooldown blank)");
                } else if (g_State.overlayInit && keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn) {
                    HookLogImportant(
                        "DX12: [outer] FG->off — exact native return already rebuilt the overlay this Present; "
                        "keeping it drawable");
                }
                g_ResetReinitSubmitCounter.store(true, std::memory_order_release);

                // Clear realECL — it was probed from a temporary queue during
                // SL activation and may reference per-instance driver dispatch
                // state that doesn't match the game queue.  After SL teardown,
                // fall back to origECL (saved from the game queue's vtable
                // before our hook was installed).
                {
                    auto* oldRealECL = (void*)g_RealD3D12ECL.load(std::memory_order_acquire);
                    g_RealD3D12ECL.store(nullptr, std::memory_order_release);
                    HookLogImportant("DX12: [outer] FG→off — cleared realECL %p (will use origECL after reinit)",
                                     oldRealECL);
                }
            }

            if (slTurnedOn) {
                // Probe real D3D12 ECL when SL FG first activates — PostSL needs it
                // to bypass SL's COM wrapper.  The inner transition handler also does
                // this, but the epoch sync skips it for transitions already handled here.
                // Defer if Streamline startup window is active to avoid creating a
                // temporary COMPUTE queue during Streamline's critical initialization.
                auto* dev = g_Device.load(std::memory_order_acquire);
                const bool startupWindowActiveForProbe = DXGIShared::IsStreamlineStartupTransitionWindowActive();
                if (dev && IsStreamlineLoaded()) {
                    if (!startupWindowActiveForProbe) {
                        ProbeRealD3D12ECL(dev);
                        auto* probed = (void*)g_RealD3D12ECL.load(std::memory_order_acquire);
