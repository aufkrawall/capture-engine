#include "dx12_hook_internal.h"
#include "dx12_hook_process_session.h"

ProcessFrameFlow FrameProcessSession::Phase4() {
skipOverlayInit:  // FG cooldown guard jumps here to skip reinit but continue ProcessFrame

    // CRITICAL FIX: Decrement FG transition cooldown when overlayInit=true but syncInit=false.
    // The !overlayInit path (line 4783) decrements when overlay needs full reinit.
    // The overlayInit+syncInit path (line 5334) decrements during normal rendering.
    // But when overlayInit=true and syncInit=false (FG transition invalidated sync
    // resources only), NEITHER path runs — the cooldown stays forever, permanently
    // blocking staged activation and overlay rendering.
    if (dx12_hook_g_State.overlayInit && !dx12_hook_g_State.syncInit && dx12_hook_g_FGTransitionCooldown > 0) {
        dx12_hook_g_FGTransitionCooldown.fetch_sub(1, std::memory_order_acq_rel);
        const bool preserveActivePostSLDuringSynclessCooldown =
            ce::dx12_overlay_policy::ShouldPreserveActivePostSLDuringFGCooldown(
                DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire), HookIsPostSLOverlayActiveButUnconfirmed());
        if (!preserveActivePostSLDuringSynclessCooldown) {
            dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
        }
        dx12_hook_g_PostSLCooldownRemaining.store(dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire),
                                        std::memory_order_release);
        static std::atomic<int> s_synclessCooldownLogCount{0};
        int logCount = s_synclessCooldownLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || dx12_hook_g_FGTransitionCooldown == 0) {
            HookLogImportant("DX12: FG cooldown (sync-invalidated path): %d frames remaining",
                             dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire));
        }
        if (dx12_hook_g_FGTransitionCooldown == 0) {
            s_synclessCooldownLogCount.store(0, std::memory_order_relaxed);
            HookLogImportant("DX12: FG cooldown complete (sync-invalidated) — staged activation can proceed");
            // Device health check after cooldown — if device already dead,
            // sync reinit would be futile and might trigger secondary crashes.
            auto* cooldownDev = g_Device.load(std::memory_order_acquire);
            if (cooldownDev) {
                HRESULT cooldownDevHr = cooldownDev->GetDeviceRemovedReason();
                if (FAILED(cooldownDevHr)) {
                    HookLogImportant("DX12: WARNING — device already dead at cooldown end! hr=0x%08X", cooldownDevHr);
                    // Device is dead — skip all overlay GPU work.  The game is about
                    // to show ERR_GFX_STATE and exit; any GPU submit would just add a
                    // secondary crash on top of an already-fatal device removal.
                    dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
                    dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
                }
            }
            if (dx12_hook_g_DeviceRemoved.load(std::memory_order_relaxed)) {
                allowOverlayRender = false;
            }
            // Device is still alive but FG transition may not be settled yet.
            // If Streamline FG was active during the transition but is now off
            // (or does not match the cooldown-start state), the overlay's
            // offscreen compositing on the original game queue can still cause a
            // GPU hang because the backbuffer state is indeterminate after FG
            // teardown.  Skip reinit GPU work on this frame; the next Present
            // will retry after the transition has had more time to settle.
            if (allowOverlayRender && dx12_hook_g_DeviceRemoved.load(std::memory_order_relaxed) == false) {
                auto* freshDev = g_Device.load(std::memory_order_acquire);
                if (freshDev) {
                    HRESULT freshHr = freshDev->GetDeviceRemovedReason();
                    if (FAILED(freshHr)) {
                        HookLogImportant(
                            "DX12: Cooldown ended but device removed — halting overlays "
                            "(hr=0x%08X)",
                            (unsigned)freshHr);
                        dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
                        dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
                        allowOverlayRender = false;
                    }
                }
            }
        }
    }

    // CRITICAL: Don't run staged sync activation during FG transition cooldown.
    // The cooldown goto above skips normal overlay init but lands HERE — and with
    // syncInit=false (cleared by the transition), this block would run InitOverlaySync
    // while the FG runtime is mid-initialization.  Destroying and recreating sync
    // resources during the transition corrupts GPU state → DEVICE_REMOVED.
    if (allowOverlayRender && !suspendOverlayRender && !dx12_hook_s_insideECL && !deferOverlayWorkAfterResume &&
        dx12_hook_g_State.overlayInit && !dx12_hook_g_State.syncInit && dx12_hook_g_FGTransitionCooldown <= 0) {
        const char* skipSeparateOverlayGpuReason = nullptr;
        if (ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain(&skipSeparateOverlayGpuReason)) {
            static std::atomic<int> s_runtimeOwnedSyncInitSkipLogCount{0};
            int logCount = s_runtimeOwnedSyncInitSkipLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                const bool ffxStalled = IsFFXPresentCallbackStalled();
                const bool ffxStallAllows =
                    ShouldAllowNormalOverlayFallbackForCurrentFFXPresentCallbackStall(ffxStalled);
                HookLogImportant(
                    "DX12: Keeping staged sync init deferred because %s — decision matrix: "
                    "runtime=%s apiFSR=%d directFFX=%d progressResolved=%d nativeFGPath=%d "
                    "explicitNativeOff=%d ffxStalled=%d ffxStallAllows=%d runtimeOwns=%d "
                    "callbackEver=%d sameQueue=%d stableProof=%d "
                    "scQueue=%p origGame=%p cmdQ=%p",
                    skipSeparateOverlayGpuReason ? skipSeparateOverlayGpuReason : "runtime-owned swapchain",
                    ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()),
                    g_FGCompat.IsFSRFGApiActive() ? 1 : 0, g_FGCompat.HasDirectFFXApiConfirmation() ? 1 : 0,
                    dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire) ? 1 : 0,
                    HookHasRuntimeOwnedNativeFGPresentPath() ? 1 : 0,
                    dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire) ? 1 : 0,
                    ffxStalled ? 1 : 0, ffxStallAllows ? 1 : 0, dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0,
                    dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire) != 0 ? 1 : 0,
                    (dx12_hook_g_SwapchainQueue != nullptr && dx12_hook_g_OriginalGameQueue != nullptr &&
                     dx12_hook_g_SwapchainQueue == dx12_hook_g_OriginalGameQueue)
                        ? 1
                        : 0,
                    EvaluateProgressResolvedOfficialFFXOverlayFallbackProof().proof ? 1 : 0, dx12_hook_g_SwapchainQueue,
                    dx12_hook_g_OriginalGameQueue, g_CommandQueue.load(std::memory_order_acquire));
            }
            return ProcessFrameFlow::kReturn;
        }

        if (dx12_hook_s_startupOverlayActivationStage == StartupOverlayActivationStage::kDelayRTVInitAfterBackendInit &&
            dx12_hook_s_startupOverlayActivationStageMs != 0) {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG elapsedSinceBackendInit = now - dx12_hook_s_startupOverlayActivationStageMs;
            if (elapsedSinceBackendInit < dx12_hook_kStartupOverlayPostBackendInitSettleMs) {
                static std::atomic<int> s_postBackendStageLogCount{0};
                if (s_postBackendStageLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                    HookLogImportant(
                        "DX12: Waiting to initialize staged overlay RTVs after backend init for %s (remaining=%llums)",
                        g_ProcessName, dx12_hook_kStartupOverlayPostBackendInitSettleMs - elapsedSinceBackendInit);
                }
            return ProcessFrameFlow::kReturn;
            }
        }

        DXGI_SWAP_CHAIN_DESC desc = {};
        if (FAILED(pSwapChain->GetDesc(&desc))) {
            HookLog("DX12: ProcessFrame - failed to get swapchain desc for staged activation");
            return ProcessFrameFlow::kReturn;
        }

        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
            HookLog("DX12: ProcessFrame - failed to get SwapChain3 for staged activation");
            return ProcessFrameFlow::kReturn;
        }

        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        int actualBufferCount = desc.BufferCount;
        if (actualBufferCount > 8) {
            HookLog("DX12: Swapchain has %d buffers during staged activation, limiting RTVs to 8", actualBufferCount);
            actualBufferCount = 8;
        }

        if (dx12_hook_s_startupOverlayActivationStage == StartupOverlayActivationStage::kDelayRTVInitAfterBackendInit) {
            if (!dx12_hook_g_State.rtvDescHeap) {
                HookLogImportant("DX12: Finalizing staged overlay activation step 1/2 - creating RTVs");
                CreateRTVs(g_Device.load(), sc3, actualBufferCount);
                if (!dx12_hook_g_State.rtvDescHeap) {
                    HookLogImportant("DX12: Staged overlay RTV init failed, keeping sync init deferred");
                    sc3->Release();

            return ProcessFrameFlow::kReturn;
                }
            }
            dx12_hook_s_startupOverlayActivationStage = StartupOverlayActivationStage::kDelaySyncInitAfterRTVInit;
            dx12_hook_s_startupOverlayActivationStageMs = GetTickCount64();
            HookLogImportant(
                "DX12: Startup compat staged activation - RTV init complete, delaying sync init for %llums",
                dx12_hook_kStartupOverlayPostRTVInitSettleMs);
            sc3->Release();
            return ProcessFrameFlow::kReturn;
        }

        if (dx12_hook_s_startupOverlayActivationStage == StartupOverlayActivationStage::kDelaySyncInitAfterRTVInit &&
            dx12_hook_s_startupOverlayActivationStageMs != 0) {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG elapsedSinceRTVInit = now - dx12_hook_s_startupOverlayActivationStageMs;
            if (elapsedSinceRTVInit < dx12_hook_kStartupOverlayPostRTVInitSettleMs) {
                static std::atomic<int> s_postRtvStageLogCount{0};
                if (s_postRtvStageLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                    HookLogImportant(
                        "DX12: Waiting to initialize staged overlay sync after RTV init for %s (remaining=%llums)",
                        g_ProcessName, dx12_hook_kStartupOverlayPostRTVInitSettleMs - elapsedSinceRTVInit);
                }
                sc3->Release();
            return ProcessFrameFlow::kReturn;
            }
        }

        if (!dx12_hook_g_State.rtvDescHeap) {
            CreateRTVs(g_Device.load(), sc3, actualBufferCount);
            if (!dx12_hook_g_State.rtvDescHeap) {
                HookLogImportant("DX12: RTV initialization failed during staged sync init, keeping overlay deferred");
                sc3->Release();
            return ProcessFrameFlow::kReturn;
            }
        }
        HookLogImportant("DX12: Finalizing staged overlay activation step 2/2 - initializing sync");
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        InitOverlaySync(g_Device.load(), desc.BufferCount, gameQueue);
        sc3->Release();

        if (dx12_hook_g_State.syncInit) {
            ResetStartupOverlayBackendActivationStage();
            dx12_hook_s_startupOverlaySyncInitMs = GetTickCount64();
            HookLogImportant(
                "DX12: Startup compat staged activation - sync init complete, delaying overlay rendering for %llums",
                dx12_hook_kStartupOverlayPostSyncInitSettleMs);
            HookLogImportant("DX12: Staged overlay activation completed after backend-only init");
        }
            return ProcessFrameFlow::kReturn;
    }

    // Single log on first frame to verify overlay system is entering
    static int s_firstFrameLogged = 0;
    if (s_firstFrameLogged == 0) {
        s_firstFrameLogged = 1;
        HookLog(
            "DX12: ProcessFrame first call - overlayInit=%d, syncInit=%d, "
            "gameQueue=%p",
            dx12_hook_g_State.overlayInit, dx12_hook_g_State.syncInit, gameQueue);
    }

    currentBackBufferIdx = 0;
    hasCurrentBackBufferIdx = false;
    pendingFocusLossBackbufferWorkHold = ShouldHoldOverlayDrawForPendingFocusLossFence();
    focusLossBackgroundDeviceLost = false;
    {
        auto* focusLossDev = g_Device.load(std::memory_order_acquire);
        if (focusLossDev) {
            focusLossBackgroundDeviceLost = FAILED(focusLossDev->GetDeviceRemovedReason());
        }
    }
    focusLossBackgroundUsingDedicatedQueue = dx12_hook_g_State.overlayQueue && ShouldUseDedicatedOverlayQueue();
    focusLossBackgroundRuntimeOwnedPresentation =
        dx12_hook_g_FGRuntimeOwnsSwapchain || HookHasRuntimeOwnedNativeFGPresentPath() || DXGIShared::DoesFGRuntimeOwnSwapchain();
    focusLossBackgroundSteamDeferredSubmit =
        dx12_hook_g_deferOverlaySubmitToSteamECL && !focusLossBackgroundUsingDedicatedQueue;
    focusLossBackgroundFrameGenerationActive =
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
    swapchainOccluded = dx12_hook_g_SwapchainPresentOccluded.load(std::memory_order_acquire);
    // The first predicate arg means "we have a reliable present-result occlusion signal",
    // which is now true for both the wrapped path (context valid) and the vtable DetourPresent
    // path (g_HaveD3D12PresentResultSignal). This lets vtable-hooked apps engage the
    // invisible-safe not-presentable hold during the Alt+Tab mode switch instead of hanging.
    haveReliablePresentResultSignal =
        dx12_hook_s_WrappedPresentFocusLossContext.valid || dx12_hook_g_HaveD3D12PresentResultSignal.load(std::memory_order_acquire);
    focusLossBackgroundBackbufferHold =
        ce::dx12_overlay_policy::ShouldHoldD3D12OverlayBackbufferWorkForNonPresentableSwapchain(
            haveReliablePresentResultSignal, !frameDesc.Windowed, swapchainOccluded, iconicWindow, zeroSizedSwapchain,
            focusLossBackgroundFrameGenerationActive, focusLossBackgroundRuntimeOwnedPresentation,
            focusLossBackgroundUsingDedicatedQueue, focusLossBackgroundSteamDeferredSubmit,
            focusLossBackgroundDeviceLost, gameQueue != nullptr);
    if (focusLossBackgroundBackbufferHold) {
        // Keep the device-removal dump window open across the not-presentable
        // period and the following presentable transition (the risky DXGI
        // iflip<->composited mode switch).
        dx12_hook_g_FocusLossRecentTransitionPresentWindow.store(dx12_hook_kFocusLossRecentTransitionDumpWindowFrames,
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
    focusTransitionHoldRemaining = dx12_hook_g_FocusTransitionHoldFrames.load(std::memory_order_acquire);
    focusTransitionActive = ce::dx12_overlay_policy::IsD3D12FocusTransitionTelemetryActive(
        frameDesc.Windowed != 0, focusTransitionHoldRemaining, focusLossBackgroundFrameGenerationActive,
        focusLossBackgroundRuntimeOwnedPresentation, focusLossBackgroundUsingDedicatedQueue,
        focusLossBackgroundSteamDeferredSubmit, focusLossBackgroundDeviceLost, gameQueue != nullptr);
    if (focusTransitionActive) {
        // Widen the device-removal dump window so any residual hang at the mode
        // switch is captured with DRED breadcrumbs.
        dx12_hook_g_FocusLossRecentTransitionPresentWindow.store(dx12_hook_kFocusLossRecentTransitionDumpWindowFrames,
                                                       std::memory_order_release);
    }
    holdFocusLossBackbufferWork = pendingFocusLossBackbufferWorkHold || focusLossBackgroundBackbufferHold;
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
                    dx12_hook_s_WrappedPresentFocusLossContext.presentName ? dx12_hook_s_WrappedPresentFocusLossContext.presentName
                                                                 : "Present",
                    dx12_hook_s_WrappedPresentFocusLossContext.callCount, gameQueue, foregroundWindow, foregroundPid,
                    frameDesc.OutputWindow, currentProcessId, processHasForeground ? 1 : 0);
            }
        } else if (s_focusTransitionHoldActive) {
            s_focusTransitionHoldActive = false;
            HookLogImportant(
                "DX12: Focus-change mode switch settled (present=%s#%d queue=%p foreground=%d)",
                dx12_hook_s_WrappedPresentFocusLossContext.presentName ? dx12_hook_s_WrappedPresentFocusLossContext.presentName : "Present",
                dx12_hook_s_WrappedPresentFocusLossContext.callCount, gameQueue, processHasForeground ? 1 : 0);
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
                    dx12_hook_s_WrappedPresentFocusLossContext.presentName ? dx12_hook_s_WrappedPresentFocusLossContext.presentName
                                                                 : "Present",
                    dx12_hook_s_WrappedPresentFocusLossContext.callCount, gameQueue, foregroundWindow, foregroundPid,
                    frameDesc.OutputWindow, currentProcessId, dx12_hook_s_WrappedPresentFocusLossContext.syncInterval,
                    dx12_hook_s_WrappedPresentFocusLossContext.presentFlags, focusLossBackgroundFrameGenerationActive ? 1 : 0,
                    focusLossBackgroundRuntimeOwnedPresentation ? 1 : 0, focusLossBackgroundUsingDedicatedQueue ? 1 : 0,
                    focusLossBackgroundSteamDeferredSubmit ? 1 : 0, focusLossBackgroundDeviceLost ? 1 : 0,
                    logCount + 1);
            }
        } else if (s_focusLossBackbufferHoldActive) {
            s_focusLossBackbufferHoldActive = false;
            HookLogImportant(
                "DX12: Resuming overlay/capture backbuffer work — swapchain presentable again "
                "(present=%s#%d queue=%p fg=%p/%lu game=%p/%lu foreground=%d recentWindow=%d)",
                dx12_hook_s_WrappedPresentFocusLossContext.presentName ? dx12_hook_s_WrappedPresentFocusLossContext.presentName : "Present",
                dx12_hook_s_WrappedPresentFocusLossContext.callCount, gameQueue, foregroundWindow, foregroundPid,
                frameDesc.OutputWindow, currentProcessId, processHasForeground ? 1 : 0,
                dx12_hook_g_FocusLossRecentTransitionPresentWindow.load(std::memory_order_acquire));
        }
    }
    captureShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    captureOverlayCfg = GetActiveDX12OverlayConfig(captureShm);
    captureWantsOverlay = captureOverlayCfg.showOverlay && captureOverlayCfg.captureIncludeOverlay;
    captureUsePostSL = processCapture && g_IPC && g_IPC->IsRecording() && captureWantsOverlay &&
                                  ShouldUseConfirmedPostSLForOverlayIncludedWork(captureOverlayCfg);
    captureAfterOverlay = processCapture && g_IPC && g_IPC->IsRecording() && captureWantsOverlay &&
                                     !captureUsePostSL && !holdFocusLossBackbufferWork;
    captureBeforeOverlay =
        processCapture && g_IPC && g_IPC->IsRecording() && !captureWantsOverlay && !holdFocusLossBackbufferWork;
    delayOverlayRenderAfterSyncInit = false;
    suppressOverlayRenderForLoadedStartupOverlay = false;
    delayOverlayRenderAfterResourcePrime = false;
    delayOverlayRenderAfterFirstDrawProbe = false;
    delayOverlayRenderAfterResume = false;
    shouldRunStartupOverlayDrawProbe = startupOverlayCompatibilityActive;

    if (allowOverlayRender && !suspendOverlayRender && !dx12_hook_s_insideECL && !deferOverlayWorkAfterResume &&
        dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit && dx12_hook_s_startupOverlaySyncInitMs != 0) {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG msSinceSyncInit = now - dx12_hook_s_startupOverlaySyncInitMs;
        const bool processNeedsRenderDelay = startupOverlayCompatibilityActive;
        const bool actualFGActive = IsActualFrameGenerationActive();
        const bool overlayBackendReady = dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit;
        const char* blockingOverlayModule = ce::overlay_compat::GetStartupBlockingOverlayRenderModuleName();
        const ULONGLONG lastBlockingRenderActivityMs =
            dx12_hook_s_lastStartupBlockingRenderModuleActivityMs.load(std::memory_order_acquire);
        const bool hasRecentBlockingRenderActivity = ce::overlay_compat::HasRecentDX12StartupBlockingRenderActivity(
            lastBlockingRenderActivityMs, now, dx12_hook_kStartupOverlayRenderModuleQuietPeriodMs);
        if (ce::overlay_compat::ShouldDelayDX12OverlayRenderAfterSyncInit(
                processNeedsRenderDelay, actualFGActive, msSinceSyncInit, dx12_hook_kStartupOverlayPostSyncInitSettleMs,
                overlayBackendReady)) {
            static std::atomic<int> s_postSyncStageLogCount{0};
            if (s_postSyncStageLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant("DX12: Waiting to render staged overlay after sync init for %s (remaining=%llums)",
                                 g_ProcessName, dx12_hook_kStartupOverlayPostSyncInitSettleMs - msSinceSyncInit);
            }
            delayOverlayRenderAfterSyncInit = true;
        } else if (ce::overlay_compat::ShouldSuppressDX12OverlayRenderForLoadedStartupOverlay(
                       processNeedsRenderDelay, actualFGActive, blockingOverlayModule, msSinceSyncInit,
                       dx12_hook_kStartupOverlayLoadedRenderModuleMaxBlockMs, overlayBackendReady)) {
            static std::atomic<int> s_loadedStartupOverlayRenderSuppressLogCount{0};
            if (s_loadedStartupOverlayRenderSuppressLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant(
                    "DX12: Keeping overlay rendering disabled while startup-blocking render module %s remains loaded "
                    "for %s (remaining=%llums)",
                    blockingOverlayModule, g_ProcessName,
                    dx12_hook_kStartupOverlayLoadedRenderModuleMaxBlockMs - msSinceSyncInit);
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
                    dx12_hook_kStartupOverlayRenderModuleQuietPeriodMs - msSinceLastActivity);
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
            dx12_hook_s_startupOverlaySyncInitMs = 0;
        }
    }

    if (allowOverlayRender && !suspendOverlayRender && !dx12_hook_s_insideECL && !deferOverlayWorkAfterResume &&
        dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit && dx12_hook_s_startupOverlayResourcePrimeMs != 0) {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG msSinceResourcePrime = now - dx12_hook_s_startupOverlayResourcePrimeMs;
        const bool preserveLiveStartupOverlayDuringInactiveSL =
            ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff();
        const bool shouldDelayAfterResourcePrime = ce::dx12_overlay_policy::ShouldDelayAfterStartupOverlayResourcePrime(
            startupOverlayCompatibilityActive, IsActualFrameGenerationActive(), msSinceResourcePrime,
            dx12_hook_kStartupOverlayPostResourcePrimeSettleMs, preserveLiveStartupOverlayDuringInactiveSL);
        if (shouldDelayAfterResourcePrime) {
            static std::atomic<int> s_postResourcePrimeLogCount{0};
            if (s_postResourcePrimeLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant(
                    "DX12: Waiting to draw staged overlay after resource priming for %s (remaining=%llums)",
                    g_ProcessName, dx12_hook_kStartupOverlayPostResourcePrimeSettleMs - msSinceResourcePrime);
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
            dx12_hook_s_startupOverlayResourcePrimeMs = 0;
        }
    }

    if (allowOverlayRender && !suspendOverlayRender && !dx12_hook_s_insideECL && !deferOverlayWorkAfterResume &&
        dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit && dx12_hook_s_startupOverlayFirstDrawProbeMs != 0) {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG msSinceProbe = now - dx12_hook_s_startupOverlayFirstDrawProbeMs;
        if (shouldRunStartupOverlayDrawProbe && msSinceProbe < dx12_hook_kStartupOverlayFirstDrawProbeSettleMs) {
            static std::atomic<int> s_firstDrawProbeWaitLogCount{0};
            if (s_firstDrawProbeWaitLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant("DX12: Waiting to continue GTA overlay probe with %s for %s (remaining=%llums)",
                                 GetStartupOverlayFirstDrawProbeStageName(dx12_hook_s_startupOverlayFirstDrawProbeStage),
                                 g_ProcessName, dx12_hook_kStartupOverlayFirstDrawProbeSettleMs - msSinceProbe);
            }
            delayOverlayRenderAfterFirstDrawProbe = true;
        } else {
            HookLogImportant("DX12: GTA overlay probe settle complete - allowing %s for %s",
                             GetStartupOverlayFirstDrawProbeStageName(dx12_hook_s_startupOverlayFirstDrawProbeStage),
                             g_ProcessName);
            dx12_hook_s_startupOverlayFirstDrawProbeMs = 0;
        }
    }

    if (allowOverlayRender && !suspendOverlayRender && !dx12_hook_s_insideECL && dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit &&
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
        if (slFGNow && dx12_hook_g_FGTransitionCooldown == 0) {
            if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) !=
                &PostSLOverlayRenderGated) {
                SetPostSLCallbackInstalled(true, "DX12: Early PostSL registration");
                HookLogImportant("DX12: Early PostSL registration (overlayInit=%d syncInit=%d)",
                                 dx12_hook_g_State.overlayInit ? 1 : 0, dx12_hook_g_State.syncInit ? 1 : 0);
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
    if (observerOnlyMode && dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit) {
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
    return ProcessFrameFlow::kContinue;
}
