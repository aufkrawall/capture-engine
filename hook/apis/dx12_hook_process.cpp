#include "dx12_hook_internal.h"

void ProcessFrame(IDXGISwapChain* pSwapChain, bool processCapture, bool applicationSourcePresent,
                  bool frameGenerationPresentationActive,
                  ce::dx12_process_frame_diagnostics::StageTimings* diagnostics ) {
    // Re-entrancy guard: NVIDIA driver can pump window messages during
    // ExecuteCommandLists (via WaitImpl → DefWindowProc), which can re-enter
    // our overlay code.  Detect and skip the re-entrant call.
    static thread_local bool s_inProcessFrame = false;
    if (s_inProcessFrame) {
        if (diagnostics) {
            diagnostics->reentrantInnerSkipped = true;
        }
        return;
    }
    s_inProcessFrame = true;
    auto reentryGuard = ce::make_scope_guard([&]() { s_inProcessFrame = false; });
    dx12_hook_g_LastProcessFrameTickMs.store(GetTickCount64(), std::memory_order_release);
    CleanupDeferredPostSLQueuesIfSafe("DX12: ProcessFrame deferred PostSL cleanup");

    static bool s_firstFrame = true;
    if (s_firstFrame) {
        s_firstFrame = false;
        HookLog("DX12: ProcessFrame FIRST CALL (swapchain=%p)", (void*)pSwapChain);
        HookLogImportant(
            "DX12 focus-loss sync policy=v13 draw-every-frame + x86 solid-span text + upload-slot per-frame fence "
            "(overlay never hidden on focus; x86 text avoids CE-owned font SRV sampling; upload slot reuse remains "
            "gated on the overlay fence)");
    }

    // Diagnostic: when the D3D12 debug layer is enabled (CE_DX12_DEBUG_LAYER), flush
    // its validation messages each frame so the overlay submit's messages (including
    // any hazard right before an Alt+Tab hang) reach the hook log. No-op when off.
    ce::dx12_dred::DrainDebugLayerMessages(g_Device.load(std::memory_order_acquire), "ProcessFrame");

    // Performance metrics for this frame
    FrameMetrics perfMetrics;
    perfMetrics.qpcUs = PerfLogger::GetQpcUs();
    strncpy(perfMetrics.api, "DX12", sizeof(perfMetrics.api) - 1);
    perfMetrics.api[sizeof(perfMetrics.api) - 1] = '\0';
    static uint64_t s_perfFrameNum = 0;
    perfMetrics.frameNum = ++s_perfFrameNum;
    if (g_pSharedMem) {
        perfMetrics.sourceFrameIndex = DXGIShared::GetLatestSourceFrameIndex();
        perfMetrics.sourceCapturePhase = g_pSharedMem->runtimeState.capturePhase.load(std::memory_order_relaxed);
        perfMetrics.sourceEncoderQueueDepth = g_pSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
        perfMetrics.sourceMuxQueueKb =
            (g_pSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed) + 1023u) / 1024u;
        perfMetrics.sourceOverloadFlags =
            g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
    }
    if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
    perfMetrics.sourceCurrentFpsTimes100 = static_cast<int32_t>(std::lround(perf->GetCurrentFPS() * 100.0f));
    perfMetrics.source1PctLowTimes100 = static_cast<int32_t>(std::lround(perf->Get1PercentLowFPS() * 100.0f));
    perfMetrics.sourcePoint1PctLowTimes100 = static_cast<int32_t>(std::lround(perf->Get01PercentLowFPS() * 100.0f));
    perfMetrics.sourceFrameTimeStdDevUs = static_cast<int32_t>(std::lround(perf->GetWindowStdDev()));
    }
    PresentDebugSample* activeDebugSample = PerfLogger::Get().GetActiveDebugSample();
    const int64_t processFrameStartUs = activeDebugSample ? PerfLogger::GetQpcUs() : 0;

    // Scope guard to log metrics on any exit path
    auto perfGuard = ce::make_scope_guard([&]() {
        if (activeDebugSample) {
            activeDebugSample->processFrameUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - processFrameStartUs);
            activeDebugSample->captureUs = perfMetrics.captureUs;
        }
        if (PerfLogger::Get().IsEnabled()) {
            perfMetrics.totalUs = static_cast<int32_t>((PerfLogger::GetQpcUs() - perfMetrics.qpcUs));
            perfMetrics.fpsLimitWaitUs = static_cast<int32_t>(g_SharedFpsLimiter.GetLastWaitUs());
            PerfLogger::Get().LogFrame(perfMetrics);
        }
    });

    // Skip performance logging if disabled
    if (!PerfLogger::Get().IsEnabled()) {
        perfGuard.dismiss();
    }

    // CRITICAL: Skip all rendering during shutdown to prevent crashes
    if (HookIsShuttingDown()) {
        return;
    }

    // Advanced DX12 focus/mode-switch analysis (config-gated by [Overlay] dx12_focus_analysis; off by
    // default). Runs before the device-removed early-return so the stall present and its residency
    // trajectory are captured. No-op when disabled.
    DX12_UpdateFocusAnalysis(g_IPC ? g_IPC->GetSharedMem() : nullptr);

    // Skip everything when device is removed — avoids reinit spam on a dead
    // device.  DX12_SetCommandQueue clears g_DeviceRemoved when a new device
    // arrives.
    if (dx12_hook_g_DeviceRemoved.load(std::memory_order_relaxed)) {
        return;
    }

    // Actively detect device removal every frame — covers cases where the
    // device gets removed during a suspension/cooldown period and
    // g_DeviceRemoved hasn't been set yet (the render-path check only runs
    // when overlayInit is true).
    {
        ID3D12Device* devCheck = g_Device.load();
        if (devCheck && FAILED(devCheck->GetDeviceRemovedReason())) {
            dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
            DXGIShared::g_SharedState.deviceRemovedFatal.store(true, std::memory_order_release);
            g_RenderWatchdog.SetForceMonitor(true);
            HookLogImportant("DX12: GPU device removed (0x%08X) — stopping overlay",
                             (unsigned)devCheck->GetDeviceRemovedReason());
            if (dx12_hook_g_Dx12FocusAnalysisActive.load(std::memory_order_relaxed)) {
                char reason[96] = {};
                _snprintf_s(reason, sizeof(reason), _TRUNCATE, "device removed 0x%08X",
                            (unsigned)devCheck->GetDeviceRemovedReason());
                DX12_DumpFocusAnalysisRing(reason);
            }
            ce::dx12_dred::DumpOnDeviceRemoved(devCheck, "D3D12 device removed before ProcessFrame setup");
            if (dx12_hook_s_WrappedPresentFocusLossContext.valid) {
                HWND foregroundWindow = nullptr;
                DWORD foregroundPid = 0;
                const bool processHasForeground = ResolveCurrentProcessForeground(&foregroundWindow, &foregroundPid);
                const bool recentFocusTransition =
                    dx12_hook_g_FocusLossRecentTransitionPresentWindow.load(std::memory_order_acquire) > 0 ||
                    dx12_hook_g_FocusLossForegroundReacquirePresentProofRemaining.load(std::memory_order_acquire) > 0;
                if (!processHasForeground || recentFocusTransition) {
                    RequestFocusLossDeviceRemovalDumpOnce("D3D12 focus-loss device removal before ProcessFrame setup",
                                                          devCheck->GetDeviceRemovedReason(),
                                                          dx12_hook_s_WrappedPresentFocusLossContext, foregroundWindow,
                                                          foregroundPid, nullptr, GetCurrentProcessId(), nullptr);
                }
            }
            dx12_hook_g_State.overlayInit = false;
            CleanupRTVs();
            return;
        }
    }

    const bool protectedOfficialFFXStartupOverlayOnly = ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup();

    // Deferred ECL probe: if ProbeRealD3D12ECL was skipped due to the Streamline
    // startup window being active, run it now that the window has expired. Keep
    // probes out of official FSR's pre-configure startup window. The protected
    // path is GPU-quiet until ffxConfigure(enable) or a real FFX present
    // callback arrives; it must not mutate queue hooks or inspect runtime
    // internals in that window.
    if (!protectedOfficialFFXStartupOverlayOnly) {
        DX12_ServiceDeferredECLProbe();
    } else {
        static std::atomic<int> s_protectedOfficialFFXECLProbeSkipLogCount{0};
        const int logCount = s_protectedOfficialFFXECLProbeSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Protected official FFX startup pending - skipping deferred ECL probe while keeping CE "
                "GPU side effects quiesced (log=%d)",
                logCount + 1);
        }
    }

    // Post-FG-OFF frame counter: log every ProcessFrame for first 50 calls after FG
    // transition.  If Present stops being called, this gap will be visible in the log.
    {
        static std::atomic<int> s_postFGOffFrames{-1};
        bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        // Detect FG OFF transition

        static bool s_prevSLFG = false;
        if (s_prevSLFG && !slFGNow) {
            s_postFGOffFrames.store(0, std::memory_order_release);
        }
        s_prevSLFG = slFGNow;

        int pfCount = s_postFGOffFrames.load(std::memory_order_acquire);
        if (pfCount >= 0 && pfCount < 300) {
            s_postFGOffFrames.store(pfCount + 1, std::memory_order_release);
            auto* pfDev = g_Device.load(std::memory_order_acquire);
            HRESULT pfDevHr = pfDev ? pfDev->GetDeviceRemovedReason() : E_FAIL;
            // Log first 50 every frame, then every 10th frame up to 300
            if (pfCount < 50 || pfCount % 10 == 0) {
                HookLogImportant(
                    "DX12: PostFGOff-PF #%d (overlayInit=%d syncInit=%d cooldown=%d "
                    "slFG=%d fgActive=%d devRemoved=0x%08X tid=0x%04X)",
                    pfCount + 1, dx12_hook_g_State.overlayInit ? 1 : 0, dx12_hook_g_State.syncInit ? 1 : 0,
                    dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), slFGNow ? 1 : 0,
                    g_FGCompat.IsFGActive() ? 1 : 0, (unsigned)pfDevHr, GetCurrentThreadId());
            }
            // Immediately abort overlay rendering if device was removed
            if (FAILED(pfDevHr)) {
                HookLogImportant("DX12: PostFGOff-PF #%d DEVICE REMOVED 0x%08X — aborting overlay", pfCount + 1,
                                 (unsigned)pfDevHr);
                dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
                return;
            }
        }
    }

    bool inResize = dx12_hook_g_InSwapchainResizeCleanup.load(std::memory_order_acquire);
    if (!pSwapChain || inResize) {
        HookLog("DX12: ProcessFrame - early return (null=%d, inResize=%d)", !pSwapChain, inResize);
        return;
    }

    DXGI_SWAP_CHAIN_DESC frameDesc = {};
    if (FAILED(pSwapChain->GetDesc(&frameDesc))) {
        HookLog("DX12: ProcessFrame - failed to get swapchain desc (precheck)");
        return;
    }

    {
        DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        bool hasTrackedColorSpace = false;
        const auto encoding = DXGIShared::ResolveSwapChainPresentationEncoding(
            pSwapChain, frameDesc.BufferDesc.Format, &colorSpace, &hasTrackedColorSpace);
        const bool dependsOnPresentationContract = ce::dx12_overlay_policy::IsPresentationContractDependentFormat(
            static_cast<int>(frameDesc.BufferDesc.Format));
        const bool reliableState = encoding != ce::presentation_color::Encoding::Unsupported &&
                                   (!dependsOnPresentationContract || hasTrackedColorSpace);
        if (reliableState) {
            const bool isHdr = ce::presentation_color::IsHDR(encoding);
            const int recordedColorSpace = hasTrackedColorSpace ? static_cast<int>(colorSpace) : -1;
            const bool previousValid = dx12_hook_g_LastKnownSwapchainHDRStateValid.load(std::memory_order_acquire);
            const bool stateChanged = !previousValid ||
                                      dx12_hook_g_LastKnownSwapchainIsHDR.load(std::memory_order_acquire) != isHdr ||
                                      dx12_hook_g_LastKnownSwapchainColorSpace.load(std::memory_order_acquire) !=
                                          recordedColorSpace;
            UpdateLastKnownSwapchainHDRStateCache(frameDesc.BufferDesc.Format, isHdr, recordedColorSpace, true);
            g_OverlayAdapter.SetHDR(isHdr, static_cast<int>(frameDesc.BufferDesc.Format));
            if (g_pSharedMem)
                g_pSharedMem->SetIsHDR(isHdr);
            if (stateChanged) {
                HookLogImportant("DX12: Presentation color state changed format=%d tracked=%d colorSpace=%d "
                                 "encoding=%s hdr=%d",
                                 static_cast<int>(frameDesc.BufferDesc.Format), hasTrackedColorSpace ? 1 : 0,
                                 recordedColorSpace, ce::presentation_color::Describe(encoding), isHdr ? 1 : 0);
            }
        }
    }

    const bool hasOutputWindow = frameDesc.OutputWindow != nullptr;
    const bool outputWindowVisible = hasOutputWindow && IsWindowVisible(frameDesc.OutputWindow) != FALSE;
    if (ce::dx12_overlay_policy::ShouldSkipDX12PresentProcessingForInvisibleWindowSwapchain(hasOutputWindow,
                                                                                            outputWindowVisible)) {
        static std::atomic<int> s_invisibleSwapchainSkipLogCount{0};
        const int logCount = s_invisibleSwapchainSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 256) == 0) {
            HookLogImportant(
                "DX12: Skipping overlay/capture processing for invisible-window swapchain "
                "(sc=%p hwnd=%p size=%ux%u log=%d)",
                pSwapChain, frameDesc.OutputWindow, frameDesc.BufferDesc.Width, frameDesc.BufferDesc.Height,
                logCount + 1);
        }
        return;
    }

    // Track HWND → swapchain mapping for FG-switch recovery
    if (frameDesc.OutputWindow) {
        TrackSwapchainHwnd(pSwapChain, frameDesc.OutputWindow);
    }

    const bool zeroSizedSwapchain = (frameDesc.BufferDesc.Width == 0 || frameDesc.BufferDesc.Height == 0);
    const bool iconicWindow = frameDesc.OutputWindow && IsIconic(frameDesc.OutputWindow);
    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    const DWORD currentProcessId = GetCurrentProcessId();
    bool processHasForeground = true;
    if (frameDesc.OutputWindow) {
        foregroundWindow = GetForegroundWindow();
        processHasForeground = false;
        if (foregroundWindow) {
            GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
            processHasForeground = (foregroundPid == currentProcessId);
        }
    }

    // Transition cooldown: after CreateSwapChainForHwnd, pause overlay D3D12
    // work so we don't interfere with the game's internal state machine (FG
    // switch, native limiter teardown, etc.).
    bool inTransitionCooldown = false;
    {
        int64_t cooldownEnd = dx12_hook_g_OverlayCooldownUntilQpc.load(std::memory_order_acquire);
        if (cooldownEnd > 0) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (now.QuadPart < cooldownEnd) {
                inTransitionCooldown = true;
            } else {
                dx12_hook_g_OverlayCooldownUntilQpc.store(0, std::memory_order_release);
            }
        }
    }

    // Focus tracking: ordinary foreground changes must keep the overlay visible.
    // Invalid swapchain states below still take the heavy suspension path, but
    // Alt+Tab/focus loss alone should not create a blank overlay interval.
    if (frameDesc.OutputWindow) {
        static bool s_focusStateInitialized = false;
        static bool s_gameWasForeground = false;

        bool gameIsForeground = (foregroundWindow == frameDesc.OutputWindow);
        if (!s_focusStateInitialized) {
            s_focusStateInitialized = true;
            s_gameWasForeground = gameIsForeground;
        } else if (gameIsForeground != s_gameWasForeground) {
            s_gameWasForeground = gameIsForeground;
            HookLog("DX12: Foreground changed (gameForeground=%d, hwnd=%p, fg=%p); overlay rendering remains active",
                    gameIsForeground ? 1 : 0, frameDesc.OutputWindow, foregroundWindow);
        }
    }

    // Heavy suspension is reserved for non-drawable swapchain states. A
    // transition cooldown may still gate specific FG routing paths later, but
    // it should not blank a valid game backbuffer during focus/swapchain churn.
    const bool suspendOverlayHeavy =
        ce::dx12_overlay_policy::ShouldHeavySuspendDX12OverlayForSwapchainState(zeroSizedSwapchain, iconicWindow);
    const bool suspendOverlayRender = suspendOverlayHeavy;

    static bool s_swapchainSuspended = false;
    if (suspendOverlayHeavy) {
        if (!s_swapchainSuspended) {
            s_swapchainSuspended = true;
            dx12_hook_g_State.overlayInit = false;
            CleanupRTVs();
            HookLog("DX12: ProcessFrame - suspending overlay (w=%u h=%u iconic=%d cooldown=%d)",
                    frameDesc.BufferDesc.Width, frameDesc.BufferDesc.Height, iconicWindow ? 1 : 0,
                    inTransitionCooldown ? 1 : 0);
        }
    } else if (s_swapchainSuspended) {
        s_swapchainSuspended = false;
        HookLog("DX12: ProcessFrame - resuming overlay after drawable swapchain restored");
    }

    // CPU Prerender Limit - apply only to application source frames. FG
    // runtimes also call Present for generated outputs, and waiting for a
    // queue-depth fence from those workers can form a cycle with the runtime's
    // own Present completion.
    float prerenderLimit = GetActivePrerenderLimit();
    if (prerenderLimit >= 0.0f && applicationSourcePresent) {
        int64_t prerenderStartUs = PerfLogger::GetQpcUs();
        ApplyPrerenderLimitDX12(prerenderLimit, frameGenerationPresentationActive);
        perfMetrics.prerenderWaitUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - prerenderStartUs);
    } else if (prerenderLimit >= 0.0f) {
        static std::atomic<int> s_runtimePrerenderSkipLogs{0};
        const int skipCount = s_runtimePrerenderSkipLogs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (skipCount <= 20 || (skipCount % 600) == 0) {
            HookLogImportant(
                "DX12: Skipping CPU prerender limiter on FG runtime-generated Present #%d "
                "(limit=%.2f currentTid=0x%04X gameTid=0x%04X)",
                skipCount, prerenderLimit, GetCurrentThreadId(), DX12_GetGamePresentThreadId());
        }
    }

    // A post-FSR OFF edge can rotate the top-level Present from the FFX proxy
    // back to an already-existing Streamline proxy without recreating either
    // swapchain. Pointer inequality is therefore not queue-ownership proof.
    // Resolve the exact route before taking the overlay lock or touching any
    // backbuffer: a confirmed Streamline proxy stays on its warm PostSL route;
    // a proven normal swapchain proceeds below; an unknown identity stays
    // GPU-quiet instead of guessing a queue and removing the device.
    bool postFSRNormalRouteExplicitQueueProof = false;
    bool postFSRNormalRouteRememberedSwapchainProof = false;
    bool postFSRNormalRouteOwnershipProven = false;
    bool authoritativeDLSSOffNormalReturnReinitializedThisPresent = false;
    auto routeInactiveDLSSPresentBeforeBackbufferAccess = [&]() -> bool {
        const bool postFSRRecoveryPending = dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire);
        const bool explicitOffKeepAlivePending = dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire);
        if (!postFSRRecoveryPending && !explicitOffKeepAlivePending) {
            return false;
        }

        ID3D12CommandQueue* recoverySwapchainQueue = nullptr;
        ID3D12CommandQueue* recoveryOriginalGameQueue = nullptr;
        ID3D12CommandQueue* recoveryPostSLLastWorkingQueue = nullptr;
        ID3D12CommandQueue* recoveryPostSLLockedQueue = nullptr;
        IDXGISwapChain* queueAssociatedSwapchain = nullptr;
        {
            std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
            recoverySwapchainQueue = dx12_hook_g_SwapchainQueue;
            recoveryOriginalGameQueue = dx12_hook_g_OriginalGameQueue;
            recoveryPostSLLastWorkingQueue = dx12_hook_g_PostSLLastWorkingQueue;
            recoveryPostSLLockedQueue = dx12_hook_g_PostSLLockedQueue;
            queueAssociatedSwapchain = dx12_hook_g_LastSwapchainQueueCaptureSwapchain.load(std::memory_order_acquire);
        }

        IDXGISwapChain* rememberedOriginalSwapchain =
            dx12_hook_g_LastProvenOriginalQueueSwapchain.load(std::memory_order_acquire);
        IDXGISwapChain* lastSuccessfulPostSLSwapchain = dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire);
        postFSRNormalRouteExplicitQueueProof =
            recoverySwapchainQueue != nullptr && recoveryOriginalGameQueue != nullptr &&
            recoverySwapchainQueue == recoveryOriginalGameQueue && queueAssociatedSwapchain == pSwapChain;
        postFSRNormalRouteRememberedSwapchainProof =
            rememberedOriginalSwapchain != nullptr && pSwapChain == rememberedOriginalSwapchain;
        postFSRNormalRouteOwnershipProven = ce::dx12_overlay_policy::IsPostFSRNormalRouteOwnershipProven(
            recoverySwapchainQueue != nullptr, recoveryOriginalGameQueue != nullptr,
            recoverySwapchainQueue != nullptr && recoverySwapchainQueue == recoveryOriginalGameQueue,
            queueAssociatedSwapchain == pSwapChain, postFSRNormalRouteRememberedSwapchainProof);

        const bool actualFGActive = IsActualFrameGenerationActive();
        const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        const bool postSLKeepAliveArmed = explicitOffKeepAlivePending;
        const bool postSLCallbackReady =
            dx12_hook_g_PostSLCallbackExecutionEnabled.load(std::memory_order_acquire) &&
            DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) == &PostSLOverlayRenderGated;
        const bool hasPostSLRenderQueue =
            recoveryPostSLLastWorkingQueue != nullptr || recoveryPostSLLockedQueue != nullptr;
        const auto recoveryRoute = ce::dx12_overlay_policy::DecideInactiveDLSSPresentRoute(
            true, actualFGActive, streamlineFGRunning, postFSRNormalRouteOwnershipProven, postSLKeepAliveArmed,
            postSLCallbackReady, hasPostSLRenderQueue,
            lastSuccessfulPostSLSwapchain != nullptr && pSwapChain == lastSuccessfulPostSLSwapchain);
        if (recoveryRoute == ce::dx12_overlay_policy::InactiveDLSSPresentRoute::kConfirmedPostSLKeepAlive) {
            const bool preRoutingKeepAliveAlreadySubmitted = DXGIShared::WasPostSLOffKeepAlivePrePresentDrawn();
            const bool shouldSubmitKeepAlive =
                ce::dx12_overlay_policy::ShouldSubmitInactiveDLSSExactPostSLKeepAlive(
                    preRoutingKeepAliveAlreadySubmitted);
            const uint64_t successfulSubmitSequenceBefore = dx12_hook_s_PostSLSuccessfulSubmitSequence;
            uint64_t successfulSubmitSequenceAfter = successfulSubmitSequenceBefore;
            bool fallbackKeepAliveDrawSucceeded = false;
            if (shouldSubmitKeepAlive) {
                const bool previousInlineCoverageOwner = dx12_hook_g_PostSLDrawBelongsToEnclosingProcessFramePresent;
                dx12_hook_g_PostSLDrawBelongsToEnclosingProcessFramePresent = true;
                auto inlineCoverageOwnerGuard = ce::make_scope_guard([previousInlineCoverageOwner]() {
                    dx12_hook_g_PostSLDrawBelongsToEnclosingProcessFramePresent = previousInlineCoverageOwner;
                });
                PostSLOverlayRenderGated(pSwapChain);
                successfulSubmitSequenceAfter = dx12_hook_s_PostSLSuccessfulSubmitSequence;
                fallbackKeepAliveDrawSucceeded =
                    successfulSubmitSequenceAfter != successfulSubmitSequenceBefore;
                if (fallbackKeepAliveDrawSucceeded) {
                    DXGIShared::MarkPostSLOffKeepAlivePrePresentDrawn();
                } else {
                    NoteDX12OverlayCoverageGate("postsl-exact-off-keepalive-submit-missed");
                }
            }
            const bool keepAliveCoverageSucceeded =
                preRoutingKeepAliveAlreadySubmitted || fallbackKeepAliveDrawSucceeded;

            static std::atomic<int> s_confirmedPostSLProxyKeepAliveLogCount{0};
            const int logCount = s_confirmedPostSLProxyKeepAliveLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: Inactive-DLSS Present remains on exact confirmed PostSL proxy — keep-alive "
                    "coverage completed=%d preRouting=%d fallbackSubmit=%d sequence=%llu->%llu "
                    "(sc=%p lastWorking=%p locked=%p origGame=%p; no copy/reinit/wait log=%d)",
                    keepAliveCoverageSucceeded ? 1 : 0, preRoutingKeepAliveAlreadySubmitted ? 1 : 0,
                    fallbackKeepAliveDrawSucceeded ? 1 : 0, successfulSubmitSequenceBefore,
                    successfulSubmitSequenceAfter, pSwapChain, recoveryPostSLLastWorkingQueue,
                    recoveryPostSLLockedQueue, recoveryOriginalGameQueue, logCount + 1);
            }
            // Streamline's explicit-OFF pass-through does not reliably issue a
            // later callback. The pre-routing hook normally submitted once on the
            // exact previously successful PostSL route; this ProcessFrame route
            // submits only as a fallback if that attempt missed. If Streamline
            // nests synchronously, dxgi_shared suppresses that same-Present
            // duplicate too.
            return true;
        }
        if (recoveryRoute == ce::dx12_overlay_policy::InactiveDLSSPresentRoute::kAwaitNormalOwnershipProof) {
            NoteDX12OverlayCoverageGate(postFSRRecoveryPending ? "postfsr-normal-ownership-unproven"
                                                               : "postsl-off-normal-ownership-unproven");
            static std::atomic<int> s_unprovenPostFSRNormalOwnershipLogCount{0};
            const int logCount = s_unprovenPostFSRNormalOwnershipLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: Inactive-DLSS Present has no exact queue-ownership proof — keeping normal route GPU quiet "
                    "(sc=%p queueAssociatedSC=%p rememberedOrigSC=%p lastPostSLSC=%p scQueue=%p origGame=%p "
                    "lastWorking=%p "
                    "locked=%p keepAlive=%d callbackReady=%d log=%d)",
                    pSwapChain, queueAssociatedSwapchain, rememberedOriginalSwapchain, lastSuccessfulPostSLSwapchain,
                    recoverySwapchainQueue, recoveryOriginalGameQueue, recoveryPostSLLastWorkingQueue,
                    recoveryPostSLLockedQueue, postSLKeepAliveArmed ? 1 : 0, postSLCallbackReady ? 1 : 0, logCount + 1);
            }
            return true;
        }
        return false;
    };
    if (routeInactiveDLSSPresentBeforeBackbufferAccess()) {
        return;
    }

    // PERFORMANCE FIX: Use try_lock instead of blocking lock_guard
    // This prevents stalling the render thread if another thread holds the lock
    if (!dx12_hook_g_OverlayMutex.try_lock()) {
        // Another thread is processing, skip this frame
        if (activeDebugSample) {
            activeDebugSample->flags |= kPresentSampleFlagMutexBusy;
        }
        HookLog("DX12: ProcessFrame - mutex busy, skipping frame");
        return;
    }
    // RAII unlock when we exit
    std::unique_lock<std::recursive_mutex> lock(dx12_hook_g_OverlayMutex, std::adopt_lock);

    // Close the only transition race left by the non-blocking overlay-lock
    // acquisition: an OFF callback may have armed recovery after the first
    // route snapshot. Re-evaluate outside the overlay lock so the PostSL
    // render->overlay lock order remains intact and no stale cleanup occurs.
    if (dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire) ||
        dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire)) {
        lock.unlock();
        if (routeInactiveDLSSPresentBeforeBackbufferAccess()) {
            return;
        }
        lock.lock();
    }

    // SAFETY: Check device state after acquiring lock
    if (dx12_hook_g_InSwapchainResizeCleanup.load(std::memory_order_acquire)) {
        HookLog("DX12: ProcessFrame - in resize cleanup, returning");
        return;
    }

    UpdateStartupOverlayCompatibilityState();
    bool allowOverlayRender = ApplyOverlayStartupCompatMode(frameDesc.OutputWindow);
    SharedMemoryLayout* observerModeShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const bool observerOnlyMode = IsDX12ObserverOnlyModeActive(observerModeShm);
    const bool observerPolicyOnlyMode = IsDX12ObserverPolicyOnlyModeActive(observerModeShm);
    const bool observerStartupPresentOnlyMode = IsDX12ObserverStartupPresentOnlyModeActive(observerModeShm);
    if (observerOnlyMode) {
        allowOverlayRender = false;
        EnsurePostSLDisabledForObserverOnly(
            "DX12: ProcessFrame observer-only mode",
            ce::streamline_runtime_policy::ShouldPreserveObserverPolicyOnlyStartupTransitionWindow(
                observerOnlyMode, observerPolicyOnlyMode));
    }

    ULONGLONG postResumeSettleRemainingMs = 0;
    const bool startupOverlayCompatibilityActive = IsStartupOverlayCompatibilityActive();
    const ULONGLONG runtimeOwnedSwapchainActiveMs = (dx12_hook_g_FGRuntimeOwnsSwapchain && dx12_hook_g_FGRuntimeOwnsSwapchainSince != 0)
                                                        ? (GetTickCount64() - dx12_hook_g_FGRuntimeOwnsSwapchainSince)
                                                        : dx12_hook_kStartupOverlayPostResumeSettleMs;
    const bool runtimeOwnedSwapchainNeedsExtraResumeSettle =
        ce::dx12_overlay_policy::ShouldDeferStartupOverlayWorkAfterResume(
            startupOverlayCompatibilityActive, dx12_hook_g_FGRuntimeOwnsSwapchain, runtimeOwnedSwapchainActiveMs,
            dx12_hook_kStartupOverlayPostResumeSettleMs, dx12_hook_s_startupOverlayCompatSettled.load(std::memory_order_acquire),
            ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff());
    const bool deferOverlayWorkAfterResume = ShouldDelayOverlayInitAfterStartupResumeCompat(
        allowOverlayRender, frameDesc.OutputWindow, runtimeOwnedSwapchainNeedsExtraResumeSettle,
        &postResumeSettleRemainingMs);
    const bool processNeedsStartupOverlayInitDelay = startupOverlayCompatibilityActive;
    if (processNeedsStartupOverlayInitDelay) {
        if ((!allowOverlayRender || deferOverlayWorkAfterResume) &&
            dx12_hook_s_startupOverlayActivationStage == StartupOverlayActivationStage::kNone) {
            dx12_hook_s_startupOverlayActivationStage = StartupOverlayActivationStage::kDelayRTVInitAfterBackendInit;
        }
    } else {
        ResetStartupOverlayBackendActivationStage();
    }
    DisableDedicatedOverlayQueueForOverlayCompat();
    if (!observerOnlyMode) {
        EnsureDedicatedOverlayQueueForFGCompat();
    }

    // Dedicated overlay queue architecture: when FG is active, overlay commands
    // execute on the overlay queue with CPU-side fence sync to avoid interfering
    // with Streamline.  When FG is not active, commands go on the game queue.

    // FG-SAFE device resolution: avoid COM calls through swapchain when FG is
    // active.  FSR FG wraps the swapchain, and QueryInterface/GetDevice through
    // the wrapper can corrupt FG state (causing a delayed null-deref crash in
    // game code ~2 seconds later when FG activates).
    //
    // Instead, we rely on device/queue already captured by our hook infrastructure:
    //   - g_Device is set from CreateSwapChainForHwnd detour or ECL detour
    //   - g_CommandQueue / g_SwapchainQueue are set from ECL/CreateSwapChain hooks
    //
    // The only thing we need from ProcessFrame is swapchain change tracking.
    // FG-SAFE swapchain change tracking: track pointer value only, no AddRef/Release.
    // FSR FG wraps the swapchain and monitors reference counts. Holding an extra
    // reference prevents FSR FG from properly transitioning, causing a delayed
    // null-deref crash in game code ~2s later when FG activates.

    // Update the FG recency counter BEFORE swapchain change check.
    {
        bool fgNow =
            IsActualFrameGenerationActive() || DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        if (fgNow)
            dx12_hook_g_FramesSinceFGActive = 0;
        else if (dx12_hook_g_FramesSinceFGActive < 9999)
            ++dx12_hook_g_FramesSinceFGActive;

        int slOffSwapchainGrace = dx12_hook_g_SLOffSwapchainReinitGrace.load(std::memory_order_acquire);
        if (slOffSwapchainGrace > 0) {
            dx12_hook_g_SLOffSwapchainReinitGrace.store(slOffSwapchainGrace - 1, std::memory_order_release);
        }
    }

    const bool exactPostDLSSOffNormalReturnSwapchainProof =
        dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.load(std::memory_order_acquire) == pSwapChain;
    const bool exactPrewarmedPostSLHandoffSwapchainProof =
        dx12_hook_g_PrewarmedPostSLHandoffSwapchain.load(std::memory_order_acquire) == pSwapChain;
    const bool processLogicalSwapchainReplacement = ce::dx12_overlay_policy::ShouldProcessLogicalSwapchainReplacement(
        pSwapChain != dx12_hook_g_LastSwapChain,
        exactPostDLSSOffNormalReturnSwapchainProof || exactPrewarmedPostSLHandoffSwapchainProof);
    if (processLogicalSwapchainReplacement) {
        if (pSwapChain == dx12_hook_g_LastSwapChain &&
            (exactPostDLSSOffNormalReturnSwapchainProof || exactPrewarmedPostSLHandoffSwapchainProof)) {
            HookLogImportant(
                "[OVERLAY VISIBILITY] Authoritative %s swapchain creation reused the previous COM pointer "
                "address; processing it as a new lifetime (swapchain=%p)",
                exactPostDLSSOffNormalReturnSwapchainProof ? "native-return" : "prewarmed-PostSL", pSwapChain);
        }
        bool deferredFreshStreamlineNoFGSwapchainCleanup = false;
        bool preserveConfirmedPostSLSwapchainChange = false;
        if (dx12_hook_g_LastSwapChain) {
            const auto runtimeMode = g_FGCompat.GetRuntimeMode();
            const uint32_t streamlineNoFGPresentCount =
                dx12_hook_g_RuntimeOwnedStreamlineNoFGPresentCount.load(std::memory_order_acquire);
            const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
            deferredFreshStreamlineNoFGSwapchainCleanup =
                ce::dx12_overlay_policy::ShouldSuppressFreshRuntimeOwnedStreamlineNoFGSeparateOverlayWork(
                    dx12_hook_g_FGRuntimeOwnsSwapchain, streamlineFGRunning, runtimeMode, streamlineNoFGPresentCount,
                    dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents);
            const bool preserveLiveStreamlineNoFGOverlayResources =
                ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff();
            ID3D12CommandQueue* preserveSwapchainQueue = nullptr;
            ID3D12CommandQueue* preserveOriginalGameQueue = nullptr;
            ID3D12CommandQueue* preserveCommandQueue = nullptr;
            ID3D12CommandQueue* preserveLastWorkingPostSLQueue = nullptr;
            {
                std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                preserveSwapchainQueue = dx12_hook_g_SwapchainQueue;
                preserveOriginalGameQueue = dx12_hook_g_OriginalGameQueue;
                preserveCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
                preserveLastWorkingPostSLQueue = dx12_hook_g_PostSLLastWorkingQueue;
            }
            const bool postSLConfirmedForSwapchainChange = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
            const int postSLStableFramesForSwapchainChange = dx12_hook_g_PostSLStableFrameCount.load(std::memory_order_acquire);
            const bool confirmedPostSLBackendWarmupProtected =
                ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLBackendAsWarmupProtected(
                    postSLConfirmedForSwapchainChange, postSLStableFramesForSwapchainChange);
            preserveConfirmedPostSLSwapchainChange =
                ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
                    streamlineFGRunning, postSLConfirmedForSwapchainChange, confirmedPostSLBackendWarmupProtected,
                    dx12_hook_g_HadFSRFGPhase, dx12_hook_g_FGRuntimeOwnsSwapchain, preserveSwapchainQueue != nullptr,
                    preserveOriginalGameQueue != nullptr,
                    preserveSwapchainQueue != nullptr && preserveOriginalGameQueue != nullptr &&
                        preserveSwapchainQueue != preserveOriginalGameQueue,
                    g_FGCompat.IsFSRFGApiActive(), HookHasRuntimeOwnedNativeFGPresentPath(),
                    pSwapChain != nullptr &&
                        pSwapChain == dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire),
                    preserveLastWorkingPostSLQueue != nullptr,
                    dx12_hook_g_PostSLWarmResumePreservationPending.load(std::memory_order_acquire));
            const bool preserveProtectedOfficialFFXStartupSwapchainChange =
                ce::dx12_overlay_policy::ShouldPreserveOverlayBackendAcrossProtectedOfficialFFXStartupSwapchainChange(
                    dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.load(std::memory_order_acquire),
                    HasResolvedOfficialFFXStartupPath());
            auto* prewarmedHandoffDevice = g_Device.load(std::memory_order_acquire);
            const bool prewarmedHandoffDeviceRemoved =
                prewarmedHandoffDevice != nullptr && FAILED(prewarmedHandoffDevice->GetDeviceRemovedReason());
            const bool preserveExactPrewarmedPostSLHandoffBackend =
                ce::dx12_overlay_policy::ShouldPreserveExactPrewarmedPostSLHandoffBackendOnFirstPresent(
                    exactPrewarmedPostSLHandoffSwapchainProof, dx12_hook_g_State.overlayInit, dx12_hook_g_State.syncInit,
                    dx12_hook_g_State.rtvDescHeap != nullptr, dx12_hook_g_State.cmdList != nullptr, dx12_hook_g_FGRuntimeOwnsSwapchain,
                    preserveSwapchainQueue != nullptr,
                    dx12_hook_g_LastSwapchainQueueCaptureSwapchain.load(std::memory_order_acquire) == pSwapChain,
                    g_FGCompat.IsFSRFGApiActive(), HookHasRuntimeOwnedNativeFGPresentPath(),
                    prewarmedHandoffDeviceRemoved);
            if (exactPrewarmedPostSLHandoffSwapchainProof) {
                IDXGISwapChain* expectedSwapchain = pSwapChain;
                dx12_hook_g_PrewarmedPostSLHandoffSwapchain.compare_exchange_strong(
                    expectedSwapchain, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
            }
            if (preserveExactPrewarmedPostSLHandoffBackend) {
                HookLogImportant(
                    "[OVERLAY VISIBILITY] First exact prewarmed PostSL handoff Present preserved its ready "
                    "overlay backend (oldSC=%p newSC=%p scQueue=%p origGame=%p cmdQ=%p)",
                    dx12_hook_g_LastSwapChain, pSwapChain, preserveSwapchainQueue, preserveOriginalGameQueue,
                    preserveCommandQueue);
            } else if (preserveProtectedOfficialFFXStartupSwapchainChange) {
                // Keep the old backend warm without retargeting it to an unproven nested FFX swapchain.
                // Proxy-backbuffer prework supplies startup visibility until enabled configure resolves
                // the route; the normal backend can then be rebound by the established transition path.
                dx12_hook_g_State.cachedSwapChain = nullptr;
                dx12_hook_g_State.cachedSC3 = nullptr;
                static std::atomic<int> s_preservedProtectedFFXStartupSwapchainCleanupLogCount{0};
                const int logCount =
                    s_preservedProtectedFFXStartupSwapchainCleanupLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 120) == 0) {
                    HookLogImportant(
                        "DX12: Preserving overlay backend across protected official FFX startup swapchain change "
                        "until enabled ffxConfigure/present-callback proof (oldSC=%p newSC=%p scQueue=%p "
                        "origGame=%p cmdQ=%p log=%d)",
                        dx12_hook_g_LastSwapChain, pSwapChain, preserveSwapchainQueue, preserveOriginalGameQueue,
                        preserveCommandQueue, logCount + 1);
                }
            } else if (preserveLiveStreamlineNoFGOverlayResources) {
                dx12_hook_g_State.cachedSwapChain = nullptr;
                dx12_hook_g_State.cachedSC3 = nullptr;
                static std::atomic<int> s_preservedFreshSLNoFGSwapchainCleanupLogCount{0};
                const int logCount =
                    s_preservedFreshSLNoFGSwapchainCleanupLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 120) == 0) {
                    HookLogImportant(
                        "DX12: Preserving live overlay resources during runtime-inactive Streamline no-FG "
                        "swapchain handoff (oldSC=%p newSC=%p scQueue=%p origGame=%p cmdQ=%p log=%d)",
                        dx12_hook_g_LastSwapChain, pSwapChain, dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue,
                        g_CommandQueue.load(std::memory_order_acquire), logCount + 1);
                }
            } else if (preserveConfirmedPostSLSwapchainChange) {
                dx12_hook_g_State.cachedSwapChain = nullptr;
                dx12_hook_g_State.cachedSC3 = nullptr;
                static std::atomic<int> s_preservedConfirmedPostSLSwapchainCleanupLogCount{0};
                const int logCount =
                    s_preservedConfirmedPostSLSwapchainCleanupLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 120) == 0) {
                    HookLogImportant(
                        "DX12: Preserving confirmed PostSL backend during active Streamline FG swapchain change "
                        "(oldSC=%p newSC=%p stableFrames=%d warmupProtected=%d hadFSR=%d fgOwned=%d scQueue=%p "
                        "origGame=%p cmdQ=%p lastWorking=%p exactSuccessful=%d cooldown=%d log=%d)",
                        dx12_hook_g_LastSwapChain, pSwapChain, postSLStableFramesForSwapchainChange,
                        confirmedPostSLBackendWarmupProtected ? 1 : 0, dx12_hook_g_HadFSRFGPhase ? 1 : 0,
                        dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0, preserveSwapchainQueue, preserveOriginalGameQueue,
                        preserveCommandQueue, preserveLastWorkingPostSLQueue,
                        pSwapChain == dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_relaxed) ? 1 : 0,
                        dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), logCount + 1);
                }
            } else if (deferredFreshStreamlineNoFGSwapchainCleanup) {
                static std::atomic<int> s_deferredFreshSLNoFGSwapchainCleanupLogCount{0};
                const int logCount =
                    s_deferredFreshSLNoFGSwapchainCleanupLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 120) == 0) {
                    HookLogImportant(
                        "DX12: Deferring swapchain-change cleanup during fresh runtime-owned Streamline no-FG "
                        "handoff (presentCount=%u settlePresents=%u oldSC=%p newSC=%p scQueue=%p origGame=%p "
                        "cmdQ=%p log=%d)",
                        streamlineNoFGPresentCount, dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents, dx12_hook_g_LastSwapChain,
                        pSwapChain, dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue,
                        g_CommandQueue.load(std::memory_order_acquire), logCount + 1);
                }
            } else {
                CleanupRTVs();
                {
                    std::lock_guard<std::recursive_mutex> capLock(dx12_hook_g_DX12CaptureMutex);
                    dx12_hook_g_SharedCaptureD3D12.Reset();
                }
                dx12_hook_g_State.overlayInit = false;
                ResetStartupOverlayBackendActivationStage();

                // FG TRANSITION PROTECTION: If FG is currently active (or was recently
                // active per the cooldown), the swapchain change is likely caused by an
                // FG mode switch (e.g., FSR FG → DLSS FG).  SL / FSR runtimes need time
                // to finish initializing before we reinit overlay resources on the new
                // swapchain.  Set a transition cooldown so the reinit path (below) defers
                // until the FG runtime is stable.
                bool fgCurrentlyActive = IsActualFrameGenerationActive() ||
                                         DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
                // Also protect if FG was active within the last ~5 seconds (~300 frames).
                // When switching FG modes, the game may disable one FG type many frames
                // before the swapchain actually changes.  Heuristic detection goes inactive
                // immediately, but the swapchain change is delayed.
                constexpr int kFGRecentWindowFrames = 300;
                bool fgRecentlyWasActive = (dx12_hook_g_FramesSinceFGActive < kFGRecentWindowFrames);
                ID3D12CommandQueue* currentSwapchainQueue = nullptr;
                ID3D12CommandQueue* currentOriginalGameQueue = nullptr;
                ID3D12CommandQueue* currentCommandQueue = nullptr;
                ID3D12CommandQueue* currentPrimaryQueue = nullptr;
                IDXGISwapChain* currentQueueAssociatedSwapchain = nullptr;
                {
                    std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                    currentSwapchainQueue = dx12_hook_g_SwapchainQueue;
                    currentOriginalGameQueue = dx12_hook_g_OriginalGameQueue;
                    currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
                    currentPrimaryQueue = dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
                    currentQueueAssociatedSwapchain =
                        dx12_hook_g_LastSwapchainQueueCaptureSwapchain.load(std::memory_order_acquire);
                }
                postFSRNormalRouteExplicitQueueProof =
                    currentSwapchainQueue != nullptr && currentOriginalGameQueue != nullptr &&
                    currentSwapchainQueue == currentOriginalGameQueue && currentQueueAssociatedSwapchain == pSwapChain;
                postFSRNormalRouteRememberedSwapchainProof =
                    dx12_hook_g_LastProvenOriginalQueueSwapchain.load(std::memory_order_acquire) == pSwapChain;
                postFSRNormalRouteOwnershipProven = ce::dx12_overlay_policy::IsPostFSRNormalRouteOwnershipProven(
                    currentSwapchainQueue != nullptr, currentOriginalGameQueue != nullptr,
                    currentSwapchainQueue != nullptr && currentSwapchainQueue == currentOriginalGameQueue,
                    currentQueueAssociatedSwapchain == pSwapChain, postFSRNormalRouteRememberedSwapchainProof);
                int slOffSwapchainGrace = dx12_hook_g_SLOffSwapchainReinitGrace.load(std::memory_order_acquire);
                const bool commandQueueSettledToPrimary =
                    currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue;
                bool guardSwapchainReinit = ce::dx12_overlay_policy::ShouldGuardSwapchainReinitAfterChange(
                    fgCurrentlyActive, fgRecentlyWasActive, dx12_hook_g_FGTransitionCooldown > 0, slOffSwapchainGrace > 0,
                    dx12_hook_g_FGRuntimeOwnsSwapchain, currentSwapchainQueue != nullptr, currentOriginalGameQueue != nullptr,
                    currentSwapchainQueue != nullptr && currentOriginalGameQueue != nullptr &&
                        currentSwapchainQueue != currentOriginalGameQueue);
                const bool immediateReinitAfterNoCallbackFFXTakeover =
                    ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(
                        g_FGCompat.HasDirectFFXApiConfirmation(), g_FGCompat.IsFSRFGApiActive(),
                        dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                        dx12_hook_g_FGRuntimeOwnsSwapchain, currentSwapchainQueue != nullptr,
                        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
                const bool immediateReinitAfterGameSwapchainRecovery =
                    ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyAfterGameSwapchainRecoveryFromNativeFSROff(
                        currentSwapchainQueue != nullptr && dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue.load(
                                                                std::memory_order_acquire) == currentSwapchainQueue,
                        g_FGCompat.IsFSRFGApiActive(),
                        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
                auto* swapchainChangeDevice = g_Device.load(std::memory_order_acquire);
                const bool swapchainChangeDeviceRemoved =
                    swapchainChangeDevice != nullptr && FAILED(swapchainChangeDevice->GetDeviceRemovedReason());
                const bool immediateReinitAfterAuthoritativeDLSSOffNormalReturn =
                    ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyAfterAuthoritativeDLSSOffNormalReturn(
                        exactPostDLSSOffNormalReturnSwapchainProof, postFSRNormalRouteOwnershipProven,
                        fgCurrentlyActive, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                        g_FGCompat.IsFSRFGApiActive(),
                        dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                        dx12_hook_g_FGRuntimeOwnsSwapchain, swapchainChangeDeviceRemoved);
                authoritativeDLSSOffNormalReturnReinitializedThisPresent =
                    immediateReinitAfterAuthoritativeDLSSOffNormalReturn;
                // DLSS-FG SUSPEND (slDLSSGSetOptions(off), proxy stays live): the active-FG
                // preserve path can't fire (streamlineFGRunning already false), so a fresh
                // proxy swapchain pointer on the same live queue used to blank the live overlay
                // for the full 90-frame cooldown. The make-before-break keep-alive latch marks
                // a CONFIRMED PostSL path that is merely suspended (never set during an FSR/
                // native-FG takeover), so reinit the warm backend immediately on its live queue.
                const bool immediateReinitAfterConfirmedPostSLSuspension = ce::dx12_overlay_policy::
                    ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
                        dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire),
                        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                        g_FGCompat.IsFSRFGApiActive(),
                        dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                        dx12_hook_g_FGRuntimeOwnsSwapchain,
                        currentSwapchainQueue != nullptr && currentCommandQueue != nullptr &&
                            currentSwapchainQueue == currentCommandQueue,
                        // The DLSS-G proxy renders the overlay on g_PostSLLastWorkingQueue
                        // (== scQueue), which persists across a suspend even when the live
                        // wrapper cmdQueue differs. Accept it as the confirmed PostSL queue.
                        currentSwapchainQueue != nullptr && dx12_hook_g_PostSLLastWorkingQueue != nullptr &&
                            currentSwapchainQueue == dx12_hook_g_PostSLLastWorkingQueue);
                // DLSS-FG OFF over a runtime-owned (FSR-history) swapchain whose ownership latch is
                // STALE: DLSS-PostSL was the actual presenter (change queue == g_PostSLLastWorkingQueue),
                // but the keep-alive could not arm (blocked by runtimeOwnedNativeFGPresentPath), so the
                // suspension predicate above misses it. FSR is not actually presenting (api inactive,
                // present callback quiet), so reinit the warm backend immediately on the same queue
                // instead of the 90-frame cooldown (session 20260614_023730: 89/90-present blanks).
                const ULONGLONG lastFFXCallbackTickMs = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
                const bool ffxPresentCallbackActiveForDLSSOff =
                    lastFFXCallbackTickMs != 0 && (GetTickCount64() - lastFFXCallbackTickMs) < 1000;
                const bool immediateReinitAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue = ce::dx12_overlay_policy::
                    ShouldReinitOverlayImmediatelyAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue(
                        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                        g_FGCompat.IsFSRFGApiActive(),
                        dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                        ffxPresentCallbackActiveForDLSSOff, dx12_hook_g_FGRuntimeOwnsSwapchain,
                        currentSwapchainQueue != nullptr && dx12_hook_g_PostSLLastWorkingQueue != nullptr &&

                            currentSwapchainQueue == dx12_hook_g_PostSLLastWorkingQueue,
                        swapchainChangeDeviceRemoved);
                if (guardSwapchainReinit &&
                    (immediateReinitAfterNoCallbackFFXTakeover || immediateReinitAfterGameSwapchainRecovery ||
                     immediateReinitAfterAuthoritativeDLSSOffNormalReturn ||
                     immediateReinitAfterConfirmedPostSLSuspension ||
                     immediateReinitAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue)) {
                    // Enable direction: the enabled ffxConfigure already finalized
                    // the official FFX takeover, applied the staged runtime queue,
                    // and drained CE's overlay GPU work; normal overlay rendering on
                    // the runtime-owned swapchain queue is the approved transport
                    // for the no-callback route. Off direction: the game-created
                    // recovery swapchain already ended the runtime-owned teardown
                    // and its queue was captured at creation. Either way, rebuild
                    // the overlay immediately instead of blanking it for the
                    // generic transition cooldown.
                    const int previousCooldown = dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire);
                    dx12_hook_g_FGTransitionCooldown.store(0, std::memory_order_release);
                    dx12_hook_g_PostSLCooldownRemaining.store(0, std::memory_order_release);
                    dx12_hook_g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
                    dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
                    dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
                    // Force sync re-init: old allocators/fence were on the old queue.
                    if (dx12_hook_g_State.syncInit) {
                        dx12_hook_g_State.syncInit = false;
                    }
                    if (immediateReinitAfterAuthoritativeDLSSOffNormalReturn) {
                        IDXGISwapChain* expectedSwapchain = pSwapChain;
                        dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.compare_exchange_strong(
                            expectedSwapchain, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
                    }
                    HookLogImportant(
                        "DX12: Swapchain change is %s — immediate overlay "
                        "reinit on its captured queue instead of FG transition cooldown "
                        "(scQueue=%p origGame=%p cmdQ=%p prevCooldown=%d)",
                        immediateReinitAfterNoCallbackFFXTakeover ? "finalized no-callback official FFX takeover"
                        : immediateReinitAfterAuthoritativeDLSSOffNormalReturn
                            ? "authoritative DLSS-off native swapchain return (exact route, no blank)"
                        : immediateReinitAfterConfirmedPostSLSuspension
                            ? "confirmed-PostSL DLSS-FG suspension (proxy stays live, no blank)"
                        : immediateReinitAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue
                            ? "DLSS-off over confirmed-PostSL runtime-owned queue (FSR latch stale, callback quiet)"
                            : "game-created swapchain recovery after explicit native FSR OFF/destroy",
                        currentSwapchainQueue, currentOriginalGameQueue, currentCommandQueue, previousCooldown);
                } else if (guardSwapchainReinit) {
                    int cooldownFrames = 90;  // ~1.5s at 60fps — longer than normal transition
                    if (ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(
                            commandQueueSettledToPrimary, dx12_hook_g_HadFSRFGPhase, slOffSwapchainGrace > 0)) {
                        // Post-FSR non-FG recovery: Streamline teardown may still be
                        // destabilizing GPU resources.  Use an extended cooldown so the
                        // overlay stays completely idle until the GPU is fully settled.
                        // The first overlay GPU submit after the cooldown can still cause
                        // DEVICE_REMOVED if Streamline teardown isn't complete, so we give
                        // a generous 15-second window.
                        cooldownFrames = ce::dx12_overlay_policy::ResolvePostFSRExtendedCooldownFrames(
                            DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
                    }
                    dx12_hook_g_FGTransitionCooldown = ce::dx12_overlay_policy::ResolveTransitionCooldownFrames(
                        dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), cooldownFrames,
                        ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(
                            commandQueueSettledToPrimary, dx12_hook_g_HadFSRFGPhase, slOffSwapchainGrace > 0));
                    dx12_hook_g_PostSLCooldownRemaining.store(dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire),
                                                    std::memory_order_release);
                    dx12_hook_g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
                    dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
                    dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
                    // Force sync re-init: old allocators/fence were on the old queue.
                    if (dx12_hook_g_State.syncInit) {
                        dx12_hook_g_State.syncInit = false;
                    }
                    HookLogImportant(
                        "DX12: Swapchain change during active FG — cooldown %d frames "
                        "(fgActive=%d, fgRecentFrames=%d, slSignal=%d, prevCooldown=%d, slOffGrace=%d, "
                        "fgOwned=%d, scQueue=%p, origGame=%p cmdQ=%p primaryQ=%p)",
                        cooldownFrames, fgCurrentlyActive ? 1 : 0, dx12_hook_g_FramesSinceFGActive,
                        DXGIShared::g_StreamlineFGRunning.load() ? 1 : 0,
                        dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), slOffSwapchainGrace,
                        dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0, currentSwapchainQueue, currentOriginalGameQueue,
                        currentCommandQueue, currentPrimaryQueue);
                } else {
                    HookLogImportant("DX12: Swapchain change (no FG active) — normal reinit");
                    const bool endingPostFSRNonFGRecovery =
                        dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire);
                    if (endingPostFSRNonFGRecovery && !postFSRNormalRouteOwnershipProven) {
                        NoteDX12OverlayCoverageGate("postfsr-normal-ownership-raced-unproven");
                        HookLogImportant(
                            "DX12: Refusing to end post-FSR recovery on a bare swapchain pointer change "
                            "(sc=%p scQueue=%p origGame=%p rememberedProof=%d explicitQueueProof=%d)",
                            pSwapChain, currentSwapchainQueue, currentOriginalGameQueue,
                            postFSRNormalRouteRememberedSwapchainProof ? 1 : 0,
                            postFSRNormalRouteExplicitQueueProof ? 1 : 0);
                        return;
                    }
                    ID3D12CommandQueue* postSLLockedQueue = nullptr;
                    ID3D12CommandQueue* postSLLastWorkingQueue = nullptr;
                    {
                        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                        postSLLockedQueue = dx12_hook_g_PostSLLockedQueue;
                        postSLLastWorkingQueue = dx12_hook_g_PostSLLastWorkingQueue;
                    }
                    const bool postSLRouteArmed =
                        DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr ||
                        dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire) ||
                        dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire) ||
                        dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire) || postSLLockedQueue != nullptr ||
                        postSLLastWorkingQueue != nullptr;
                    const bool hasDistinctPostSLQueueProof =
                        currentOriginalGameQueue != nullptr &&
                        ((postSLLockedQueue && postSLLockedQueue != currentOriginalGameQueue) ||
                         (postSLLastWorkingQueue && postSLLastWorkingQueue != currentOriginalGameQueue));
                    const bool retirePostSLRoute =
                        ce::dx12_overlay_policy::ShouldRetirePostSLRouteForNormalSwapchainReturn(
                            endingPostFSRNonFGRecovery && postFSRNormalRouteOwnershipProven, postSLRouteArmed,
                            hasDistinctPostSLQueueProof);

                    if (retirePostSLRoute) {
                        PublishPostSLRouteRetirementForNormalSwapchainReturn("DX12: clean non-FG Present return");
                    }
                    if (endingPostFSRNonFGRecovery && postFSRNormalRouteOwnershipProven) {
                        // Publish the authoritative normal-return boundary before
                        // waiting for an already-entered PostSL callback. Concurrent
                        // routing must immediately stop treating its historical queue
                        // as eligible for the replacement swapchain.
                        dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG.store(false, std::memory_order_release);
                    }
                    if (retirePostSLRoute) {
                        // PostSL owns its render mutex before it can enter overlay
                        // initialization (render -> overlay lock order). Release
                        // ProcessFrame's overlay lock while the cancellation epoch
                        // drains an already-entered callback, then reacquire it
                        // before rebuilding/drawing on the normal route below.
                        lock.unlock();
                        const int previousStableFrames =
                            FinishPostSLRouteRetirementForNormalSwapchainReturn("DX12: clean non-FG Present return");
                        lock.lock();
                        HookLogImportant(
                            "DX12: Clean non-FG Present return retired stale PostSL route before normal overlay "
                            "reinit (locked=%p lastWorking=%p origGame=%p stableFrames=%d)",
                            postSLLockedQueue, postSLLastWorkingQueue, currentOriginalGameQueue, previousStableFrames);
                    }
                    if (endingPostFSRNonFGRecovery && postFSRNormalRouteExplicitQueueProof) {
                        HookLogImportant(
                            "DX12: Ended post-FSR non-FG recovery on explicit swapchain-queue proof "
                            "(scQueue=%p matches origGame=%p)",
                            currentSwapchainQueue, currentOriginalGameQueue);
                    } else if (endingPostFSRNonFGRecovery && postFSRNormalRouteRememberedSwapchainProof) {
                        HookLogImportant(
                            "DX12: Ended post-FSR non-FG recovery on remembered exact original-queue swapchain "
                            "identity (sc=%p origGame=%p)",
                            pSwapChain, currentOriginalGameQueue);
                    } else {
                        HookLogImportant("DX12: Ordinary non-FG swapchain change outside post-FSR recovery");
                    }
                    if (ce::dx12_overlay_policy::ShouldResetQueueChangeHeuristicAfterCleanNonFGSwapchainChange(
                            endingPostFSRNonFGRecovery && postFSRNormalRouteOwnershipProven)) {
                        RequestFGDetectionHeuristicReset();
                        if (g_FGCompat.IsHeuristicFSRFGActive()) {
                            g_FGCompat.SetHeuristicFSRFGActive(false);
                        }
                        HookLogImportant(
                            "DX12: Reset queue-change heuristic after clean non-FG swapchain transition ending "
                            "post-FSR "
                            "recovery");
                    }
                }
            }
        }
        if (!deferredFreshStreamlineNoFGSwapchainCleanup) {
            // Store raw pointer for change detection only - no AddRef to avoid
            // interfering with FSR FG's swapchain lifecycle management
            dx12_hook_g_LastSwapChain = pSwapChain;

            if (!g_Device.load()) {
                return;
            }
            HookLog("DX12: ProcessFrame - new swapchain tracked (device=%p)", g_Device.load());
        }
        if (exactPrewarmedPostSLHandoffSwapchainProof) {
            IDXGISwapChain* expectedSwapchain = pSwapChain;
            dx12_hook_g_PrewarmedPostSLHandoffSwapchain.compare_exchange_strong(
                expectedSwapchain, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
        }
    }

    // Prefer the swapchain queue(captured at creation time) so that our
    // RENDER_TARGET -> PRESENT barrier executes on the queue DXGI syncs with.
    // Fall back to the last observed direct queue if it was not captured yet.
    //
    // EXCEPTION: During SL DLSS FG, g_SwapchainQueue may have been overwritten
    // by SL's CreateSwapChainForHwnd (SL creates its own swapchain with its
    // internal queue).  In that case, use g_OriginalGameQueue — the game's
    // real queue captured before any FG ever activated.
    //
    // FSR FG: FSR creates a NEW swapchain with its own queue. Our Present
    // detour sees pSwapChain = FSR's swapchain, so GetBuffer returns FSR's
    // backbuffers.  We MUST submit on the swapchain's associated queue
    // (g_SwapchainQueue = FSR's queue) — submitting on origGame causes
    // cross-queue resource access without synchronization → DEVICE_REMOVED.
    ID3D12CommandQueue* gameQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        bool fsrFGNow = IsFSRFrameGenerationActive();
        ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        ID3D12CommandQueue* currentPrimaryQueue = dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
        const bool recentStreamlineTeardown = dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
        const bool postFSRInactiveRecoveryPending =
            dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire);
        const bool lastWorkingQueueStillActiveDuringRecentTeardown =
            dx12_hook_g_PostSLLastWorkingQueue != nullptr &&
            GetTickCount64() < dx12_hook_g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
        if (protectedOfficialFFXStartupOverlayOnly) {
            static std::atomic<int> s_protectedOfficialFFXStartupGpuQuietLogCount{0};
            const int logCount = s_protectedOfficialFFXStartupGpuQuietLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: Protected official FFX startup keeping nested real-swapchain work tracking-only; "
                    "proxy-backbuffer prework owns overlay visibility until enabled ffxConfigure/present-callback "
                    "proof (sc=%p origGame=%p oldScQueue=%p cmdQ=%p resolved=%d log=%d)",
                    pSwapChain, dx12_hook_g_OriginalGameQueue, dx12_hook_g_SwapchainQueue, currentCommandQueue,
                    HasResolvedOfficialFFXStartupPath() ? 1 : 0, logCount + 1);
            }
            return;
        } else {
            const auto routingDecision = ce::dx12_overlay_policy::DecideSwapchainOverlayRouting(
                dx12_hook_g_FGRuntimeOwnsSwapchain, slFGNow, fsrFGNow, dx12_hook_g_HadFSRFGPhase, dx12_hook_g_SwapchainQueue != nullptr,
                dx12_hook_g_OriginalGameQueue != nullptr, dx12_hook_g_PostSLLastWorkingQueue != nullptr, postFSRInactiveRecoveryPending,
                currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue,
                dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire),
                dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire));

            if (routingDecision ==
                ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kSkipRuntimeOwnedSwapchainWithoutQueue) {
                static int s_fgOwnSkipLog = 0;
                ++s_fgOwnSkipLog;
                if (s_fgOwnSkipLog <= 10 || (s_fgOwnSkipLog % 300) == 0) {
                    HookLogImportant(
                        "DX12: ProcessFrame — FG runtime owns swapchain but scQueue is null, SKIPPING overlay "
                        "(origGame=%p, fsrFGHeur=%d, fgOwnedSince=%llums ago) #%d",
                        dx12_hook_g_OriginalGameQueue, fsrFGNow ? 1 : 0, GetTickCount64() - dx12_hook_g_FGRuntimeOwnsSwapchainSince,
                        s_fgOwnSkipLog);
                }
                return;
            } else if (routingDecision ==
                       ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRStreamlineQueue) {
                // After FSR→DLSS: use scQueue (swapchain creation queue).
                // The swapchain was created on FSR's queue; backbuffers are
                // associated with it.  origGame can't access them (cross-queue).
                // SL's wrapper queue also fails.  scQueue is the ONLY queue
                // with authorized backbuffer access.
                if (dx12_hook_g_SwapchainQueue) {
                    gameQueue = dx12_hook_g_SwapchainQueue;
                    static bool s_loggedPostFSR = false;
                    if (!s_loggedPostFSR) {
                        s_loggedPostFSR = true;
                        HookLogImportant(
                            "DX12: ProcessFrame — post-FSR SL FG, using scQueue %p (swapchain creation queue, "
                            "origGame=%p)",
                            gameQueue, dx12_hook_g_OriginalGameQueue);
                    }
                } else {
                    // Shouldn't happen — scQueue should be kept alive during hadFSR
                    gameQueue = dx12_hook_g_OriginalGameQueue;
                    HookLogImportant("DX12: ProcessFrame — post-FSR SL FG but scQueue is null, fallback to origGame %p",
                                     gameQueue);
                }
            } else if (routingDecision ==
                       ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue) {
                // SL FG (no FSR history): use origGame.
                gameQueue = dx12_hook_g_OriginalGameQueue;
            } else if (routingDecision ==
                       ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue) {
                // During the explicit post-FSR inactive recovery epoch with
                // scQueue intentionally unset, reuse the last queue that already
                // proved it could render the still-live transition swapchain.
                gameQueue = dx12_hook_g_PostSLLastWorkingQueue;
                static std::atomic<int> s_postFSRProcessFrameLastWorkingRouteLogCount{0};
                int logCount = s_postFSRProcessFrameLastWorkingRouteLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: ProcessFrame — post-FSR inactive recovery epoch using preserved PostSL lastWorking "
                        "queue %p (cmdQ=%p origQ=%p primaryQ=%p recentTraffic=%d)",
                        dx12_hook_g_PostSLLastWorkingQueue, currentCommandQueue, dx12_hook_g_OriginalGameQueue, currentPrimaryQueue,
                        lastWorkingQueueStillActiveDuringRecentTeardown ? 1 : 0);
                }
            } else if (routingDecision ==
                       ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue) {
                // After FSR->DLSS->off, or an explicit native-FSR OFF/suspend while
                // the stale FSR swapchain queue latch is still draining, prefer the
                // known original Present queue over the most recent ECL queue. Talos
                // uses separate render/present DIRECT queues; falling back to
                // g_CommandQueue/primary picked the render queue and immediately hit
                // DEVICE_REMOVED on the first recovered non-FG offscreen composite.
                const auto queueSource =
                    ce::dx12_overlay_policy::DecidePostFSRInactiveRecoveryQueueSource(dx12_hook_g_OriginalGameQueue != nullptr);
                if (queueSource == ce::dx12_overlay_policy::PostFSRInactiveRecoveryQueueSource::kOriginalPresentQueue) {
                    gameQueue = dx12_hook_g_OriginalGameQueue;
                    const bool explicitNativeFSROffPending =
                        dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
                    static std::atomic<int> s_postFSRInactiveOrigRouteLogCount{0};
                    int logCount = s_postFSRInactiveOrigRouteLogCount.fetch_add(1, std::memory_order_relaxed);
                    if (logCount < 10 || (logCount % 300) == 0) {
                        HookLogImportant(
                            "DX12: ProcessFrame — post-FSR normal/recovery routing using original present queue %p "
                            "(cmdQ=%p primaryQ=%p recoveryPending=%d explicitNativeOff=%d)",
                            gameQueue, currentCommandQueue, currentPrimaryQueue, postFSRInactiveRecoveryPending ? 1 : 0,
                            explicitNativeFSROffPending ? 1 : 0);
                    }
                } else {
                    gameQueue = currentCommandQueue ? currentCommandQueue : currentPrimaryQueue;
                    static std::atomic<int> s_postFSRInactiveFallbackRouteLogCount{0};
                    int logCount = s_postFSRInactiveFallbackRouteLogCount.fetch_add(1, std::memory_order_relaxed);
                    if (logCount < 10 || (logCount % 300) == 0) {
                        HookLogImportant(
                            "DX12: ProcessFrame — post-FSR inactive recovery missing origGame, falling back to current "
                            "command queue %p (primaryQ=%p)",
                            gameQueue, currentPrimaryQueue);
                    }
                }
            } else if (routingDecision ==
                       ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue) {
                // FSR FG: pSwapChain is FSR's swapchain, backbuffers belong to
                // FSR's queue.  Submit on the swapchain queue to avoid cross-queue
                // resource state conflicts.  We use realECL to bypass FSR's ECL
                // hook on this queue.
                gameQueue = dx12_hook_g_SwapchainQueue;
                if (!dx12_hook_g_HadFSRFGPhase &&
                    ce::dx12_overlay_policy::ShouldLatchFSRFGHistory(g_FGCompat.IsFSRFGApiActive(), true)) {
                    dx12_hook_g_HadFSRFGPhase = true;
                    HookLogImportant(
                        "DX12: ProcessFrame — FSR FG history confirmed, origGame potentially corrupted for future DLSS "
                        "FG");
                }
            } else if (routingDecision ==
                       ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseRuntimeOwnedSwapchainQueue) {
                // Runtime-owned swapchain without FSR evidence. This covers DLSS/
                // Streamline suspend-resume windows where the live swapchain stays on
                // a non-game queue but must NOT be promoted into post-FSR recovery.
                const bool startupCompatCanUseSettledRuntimeOwnedQueue =
                    startupOverlayCompatibilityActive &&
                    ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(
                        true, dx12_hook_g_SwapchainQueue != nullptr, dx12_hook_g_FGRuntimeOwnsSwapchain, runtimeOwnedSwapchainActiveMs,
                        dx12_hook_kStartupOverlayPostResumeSettleMs,
                        dx12_hook_s_startupOverlayCompatSettled.load(std::memory_order_acquire),
                        ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff());
                const bool useOriginalQueueForStartupCompat =
                    startupCompatCanUseSettledRuntimeOwnedQueue && dx12_hook_g_OriginalGameQueue != nullptr;
                gameQueue = useOriginalQueueForStartupCompat ? dx12_hook_g_OriginalGameQueue : dx12_hook_g_SwapchainQueue;
                static int s_runtimeOwnedQueueLogCount = 0;
                ++s_runtimeOwnedQueueLogCount;
                if (s_runtimeOwnedQueueLogCount <= 10 || (s_runtimeOwnedQueueLogCount % 300) == 0) {
                    const bool authoritativeFSR = g_FGCompat.IsFSRFGApiActive();
                    HookLogImportant(
                        "DX12: ProcessFrame — runtime-owned swapchain %s, using %s %p "
                        "(origGame=%p slFG=%d hadFSR=%d apiFSR=%d startupCompatSettled=%d) #%d",
                        authoritativeFSR ? "with authoritative FSR FG state" : "without FSR evidence",
                        useOriginalQueueForStartupCompat ? "origGame" : "scQueue", gameQueue, dx12_hook_g_OriginalGameQueue,
                        slFGNow ? 1 : 0, dx12_hook_g_HadFSRFGPhase ? 1 : 0, authoritativeFSR ? 1 : 0,
                        startupCompatCanUseSettledRuntimeOwnedQueue ? 1 : 0, s_runtimeOwnedQueueLogCount);
                }
            } else if (routingDecision ==
                       ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kSkipFSRWithoutSwapchainQueue) {
                // FSR FG active but g_SwapchainQueue not captured.
                // DO NOT fall back to origGame — FSR FG uses origGame internally
                // and injecting our ECLs on it will corrupt FSR's fence tracking,
                // causing an internal FSR deadlock (ffxQuery spin-wait or WaitForSingleObject).
                // Instead, skip rendering entirely until scQueue is recaptured.
                static int s_fsrSkipLog = 0;
                ++s_fsrSkipLog;
                if (s_fsrSkipLog <= 5 || (s_fsrSkipLog % 300) == 0) {
                    HookLogImportant(
                        "DX12: ProcessFrame — FSR FG active but scQueue=null, SKIPPING overlay (origGame=%p used by "
                        "FSR, "
                        "#%d)",
                        dx12_hook_g_OriginalGameQueue, s_fsrSkipLog);
                }
                return;
            } else {
                gameQueue = dx12_hook_g_SwapchainQueue;
                if (!gameQueue)
                    gameQueue = g_CommandQueue.load();
            }
        }
    }
    if (!gameQueue) {
        HookLog("DX12: ProcessFrame - no game queue, skipping overlay");
        return;
    }
    // Log queue selection decision (rate-limited: first 10, then every 300)
    {
        static int s_queueLogCount = 0;
        ++s_queueLogCount;
        if (s_queueLogCount <= 10 || (s_queueLogCount % 300) == 0) {
            bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
            bool fsrFGActive = IsFSRFrameGenerationActive();
            const char* qPath = "unknown";
            if (slFGNow && dx12_hook_g_OriginalGameQueue && gameQueue == dx12_hook_g_OriginalGameQueue)
                qPath = "origGame(SL-FG)";
            else if (fsrFGActive && gameQueue == dx12_hook_g_SwapchainQueue)
                qPath = "scQueue(FSR-FG)";
            else if (fsrFGActive && gameQueue == dx12_hook_g_OriginalGameQueue)
                qPath = "origGame(FSR-FG-fallback)";
            else if (!slFGNow && !fsrFGActive && dx12_hook_g_HadFSRFGPhase && !dx12_hook_g_SwapchainQueue && dx12_hook_g_PostSLLastWorkingQueue &&
                     gameQueue == dx12_hook_g_PostSLLastWorkingQueue)
                qPath = "lastWorking(post-FSR)";
            else if (!slFGNow && !fsrFGActive && dx12_hook_g_HadFSRFGPhase && !dx12_hook_g_SwapchainQueue && dx12_hook_g_OriginalGameQueue &&
                     gameQueue == dx12_hook_g_OriginalGameQueue)
                qPath = "origGame(post-FSR)";
            else if (gameQueue == dx12_hook_g_SwapchainQueue)
                qPath = "scQueue";
            else if (gameQueue == dx12_hook_g_OriginalGameQueue)
                qPath = "origGame";
            else if (gameQueue == dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire))
                qPath = "primaryQ";
            else if (gameQueue == g_CommandQueue.load(std::memory_order_acquire))
                qPath = "cmdQueue";
            else
                qPath = "otherQ";
            HookLogImportant(
                "DX12: ProcessFrame queue=%p (slFG=%d fsrFG=%d origQ=%p primaryQ=%p scQ=%p cmdQ=%p lastWorkingQ=%p "
                "path=%s) #%d",
                gameQueue, slFGNow ? 1 : 0, fsrFGActive ? 1 : 0, dx12_hook_g_OriginalGameQueue,
                dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire), dx12_hook_g_SwapchainQueue, (void*)g_CommandQueue.load(),
                dx12_hook_g_PostSLLastWorkingQueue, qPath, s_queueLogCount);
        }
    }

    // Track the game's Present thread ID for pre-SL overlay rendering.
    // During SL FG, SL's worker threads also call Present (for generated frames).
    // Pre-SL overlay must ONLY run on the game thread — SL's workers call Present
    // at the wrong timing (during FG frame Present, not game frame Present).
    {
        DWORD currentTid = GetCurrentThreadId();
        bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        if (!slFGNow) {
            // When SL FG is NOT active, the current thread IS the game thread.
            // Update the tracked ID (game might switch render threads).
            dx12_hook_g_GamePresentThreadId.store(currentTid, std::memory_order_release);
            g_RenderWatchdog.SetMonitoredThread(currentTid);
        }
    }

    // Capture the game's original queue ONCE before any FG activation.
    // This queue is guaranteed to be the game's own D3D12 queue (not SL's).
    // During FG transitions, g_SwapchainQueue and g_CommandQueue can both
    // get polluted by SL/FSR internal queues (via CreateSwapChainForHwnd
    // and ECL hooks respectively).
    if (!dx12_hook_g_OriginalGameQueue) {
        dx12_hook_g_OriginalGameQueue = gameQueue;
        dx12_hook_g_LastProvenOriginalQueueSwapchain.store(nullptr, std::memory_order_release);
        gameQueue->AddRef();  // prevent queue from being freed during FG transitions
        HookLogImportant("DX12: Captured original game queue %p (sc=%p cmd=%p)", gameQueue, dx12_hook_g_SwapchainQueue,
                         (void*)g_CommandQueue.load());
    }

    bool currentSwapchainProvenOnOriginalQueue = false;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        currentSwapchainProvenOnOriginalQueue =
            !IsActualFrameGenerationActive() && !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) &&
            !dx12_hook_g_FGRuntimeOwnsSwapchain && dx12_hook_g_OriginalGameQueue != nullptr && dx12_hook_g_SwapchainQueue != nullptr &&
            dx12_hook_g_SwapchainQueue == dx12_hook_g_OriginalGameQueue && gameQueue == dx12_hook_g_OriginalGameQueue &&
            dx12_hook_g_LastSwapchainQueueCaptureSwapchain.load(std::memory_order_acquire) == pSwapChain;
    }
    if (currentSwapchainProvenOnOriginalQueue) {
        RememberOriginalQueueSwapchainIdentity(pSwapChain, "normal Present on captured original queue");
    }

    // Queue-change-based FG detection: FSR FG creates its own command queue
    // and reroutes all ECL calls through it.  Detecting a queue pointer change
    // after the first few frames is a strong signal that FG has activated.
    //
    // IMPORTANT: FSR FG alternates between origGame queue and FSR's internal
    // queue every frame (real frame vs interpolated frame).  We use hysteresis
    // to avoid rapid on/off oscillation:
    //   - Activation: trigger immediately on first queue change
    //   - Deactivation: require N CONSECUTIVE frames on initial queue
    //
    // CRITICAL: Use the RAW command queue (g_CommandQueue from ECL hook), NOT
    // the overridden gameQueue.  When FG is active, gameQueue is forced to
    // g_OriginalGameQueue, which would mask FSR's queue alternation and cause
    // the deactivation counter to fire incorrectly.
    {
        static ID3D12CommandQueue* s_initialQueue = nullptr;
        static ID3D12CommandQueue* s_currentFGQueue = nullptr;
        static int s_queueFrameCount = 0;
        static int s_consecutiveInitialQueueFrames = 0;
        constexpr int kDeactivationThreshold = 120;  // ~2s at 60fps before declaring FG off

        // Decrement SL OFF heuristic grace once per ProcessFrame (not per ECL call).
        int slGrace = dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire);
        if (slGrace > 0) {
            dx12_hook_g_SLOffHeuristicGrace.store(slGrace - 1, std::memory_order_release);
            // Force-clear any lingering heuristic FSR_FG during the grace window.
            // CanUseFSRFGHeuristics blocks new detections, but a stale true from
            // before SL FG activated can persist because no code path overwrites it.
            if (g_FGCompat.IsHeuristicFSRFGActive()) {
                g_FGCompat.SetHeuristicFSRFGActive(false);
            }
            // Also suppress phantom NVIDIA_SM re-detection during the grace window.
            // ClearNvidiaSMState resets the confirm counter and cached multiplier,
            // but DetectPattern can re-detect within 3 frames if the multiplier
            // rebuilds from recent frame history.  Force-clear each frame.
            if (IsNvidiaSmoothMotionActiveRuntime()) {
                g_FGCompat.ClearNvidiaSMState();
                static int s_phantomSMClears = 0;
                if (s_phantomSMClears++ < 5)
                    HookLogImportant("DX12: Cleared phantom NVIDIA_SM during SL grace (remaining=%d)", slGrace - 1);
            }
        }

        // FG transition handler sets this flag to force a full reset.
        // Without this, SL's leftover queue persists in s_initialQueue/
        // s_currentFGQueue and immediately re-triggers false FSR FG detection.
        if (dx12_hook_g_ResetQueueChangeHeuristic.exchange(false, std::memory_order_acquire)) {
            ID3D12CommandQueue* authoritativeBaseline =
                dx12_hook_g_QueueChangeHeuristicAuthoritativeBaseline.load(std::memory_order_acquire);
            // After SL FG OFF, SL may have created a new swapchain on a
            // different queue.  The game continues using SL's swapchain queue
            // even after FG teardown.  Anchoring to origGame would permanently
            // see the new queue as "different" → endless false FSR_FG.
            //
            // Ordinary transitions allow recapture from the next five frames.
            // A proven normal swapchain return instead pins the baseline to its
            // authoritative game queue and waits for that queue to be observed.
            HookLog(
                "DX12: Queue-change heuristic reset (FG transition) — "
                "was initial=%p fgQ=%p frame=%d authoritativeBaseline=%p",
                s_initialQueue, s_currentFGQueue, s_queueFrameCount, authoritativeBaseline);
            s_initialQueue = authoritativeBaseline;
            s_currentFGQueue = nullptr;
            s_queueFrameCount = authoritativeBaseline ? 5 : 0;
            s_consecutiveInitialQueueFrames = 0;
        }

        ID3D12CommandQueue* rawQueue = g_CommandQueue.load(std::memory_order_acquire);
        ID3D12CommandQueue* authoritativeBaseline =
            dx12_hook_g_QueueChangeHeuristicAuthoritativeBaseline.load(std::memory_order_acquire);
        const bool awaitAuthoritativeBaseline = ce::dx12_overlay_policy::ShouldAwaitAuthoritativeQueueChangeBaseline(
            authoritativeBaseline != nullptr, rawQueue == authoritativeBaseline);
        if (awaitAuthoritativeBaseline) {
            static std::atomic<int> s_authoritativeBaselineWaitLogCount{0};
            const int logCount = s_authoritativeBaselineWaitLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 256) == 0) {
                HookLogImportant(
                    "DX12: Ignoring leftover queue traffic while normal swapchain return awaits authoritative "
                    "baseline (raw=%p baseline=%p scQueue=%p origGame=%p count=%d)",
                    rawQueue, authoritativeBaseline, dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue, logCount + 1);
            }
            if (g_FGCompat.IsHeuristicFSRFGActive()) {
                g_FGCompat.SetHeuristicFSRFGActive(false);
            }
        } else {
            bool mayEvaluateQueueChange = true;
            if (authoritativeBaseline) {
                ID3D12CommandQueue* expectedBaseline = authoritativeBaseline;
                if (dx12_hook_g_QueueChangeHeuristicAuthoritativeBaseline.compare_exchange_strong(
                        expectedBaseline, nullptr, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    s_initialQueue = rawQueue;
                    s_currentFGQueue = nullptr;
                    s_queueFrameCount = 5;
                    s_consecutiveInitialQueueFrames = 0;
                    HookLogImportant(
                        "DX12: Established authoritative queue-change baseline after normal swapchain return "
                        "(baseline=%p scQueue=%p origGame=%p)",
                        rawQueue, dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue);
                } else {
                    // A concurrent lifecycle event replaced or consumed this
                    // epoch. Do not evaluate the current queue against stale
                    // function-local state; the winning epoch owns the reset.
                    mayEvaluateQueueChange = false;
                }
            }

            if (mayEvaluateQueueChange)
                ++s_queueFrameCount;
            if (mayEvaluateQueueChange && s_queueFrameCount <= 5) {
                // Capture initial queue during first 5 frames (before FG activates)
                s_initialQueue = rawQueue;
            } else if (mayEvaluateQueueChange && s_initialQueue) {
                const bool recentStreamlineTeardown = dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
                ID3D12CommandQueue* currentSwapchainQueue = nullptr;
                {
                    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
                    currentSwapchainQueue = dx12_hook_g_SwapchainQueue;
                }
                const bool lastWorkingQueueStillActiveDuringRecentTeardown =
                    dx12_hook_g_PostSLLastWorkingQueue != nullptr &&
                    GetTickCount64() < dx12_hook_g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
                const bool postFSRNonFGRecovery = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
                    dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG, IsActualFrameGenerationActive(),
                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                    currentSwapchainQueue != nullptr);
                const bool ignoreQueueChangeDuringRecentTeardown =
                    rawQueue &&
                    ce::dx12_overlay_policy::ShouldIgnoreQueueChangeHeuristicDuringRecentStreamlineTeardown(
                        recentStreamlineTeardown, postFSRNonFGRecovery, lastWorkingQueueStillActiveDuringRecentTeardown,
                        rawQueue == dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire), rawQueue == dx12_hook_g_OriginalGameQueue,
                        rawQueue == currentSwapchainQueue, rawQueue == dx12_hook_g_PostSLLastWorkingQueue);

                if (ignoreQueueChangeDuringRecentTeardown) {
                    static std::atomic<int> s_recentTeardownQueueChangeIgnoreLogCount{0};
                    const int logCount =
                        s_recentTeardownQueueChangeIgnoreLogCount.fetch_add(1, std::memory_order_relaxed);
                    if (logCount < 20 || (logCount % 256) == 0) {
                        HookLogImportant(
                            "DX12: Ignoring queue-change heuristic on teardown/recovery queue %p "
                            "(initial=%p orig=%p primary=%p scQ=%p lastWorking=%p slOffGrace=%d postSLRecent=%d "
                            "postFSR=%d frame=%d)",
                            rawQueue, s_initialQueue, dx12_hook_g_OriginalGameQueue,
                            dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire), currentSwapchainQueue,
                            dx12_hook_g_PostSLLastWorkingQueue, dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire),
                            lastWorkingQueueStillActiveDuringRecentTeardown ? 1 : 0, postFSRNonFGRecovery ? 1 : 0,
                            s_queueFrameCount);
                    }
                    s_consecutiveInitialQueueFrames = 0;
                } else {
                    bool isFGQueue = (rawQueue != s_initialQueue);
                    if (isFGQueue) {
                        // Reset consecutive-initial counter — we just saw the FG queue
                        s_consecutiveInitialQueueFrames = 0;

                        if (!s_currentFGQueue) {
                            if (UpdateHeuristicFSRFGState(true, "queue-change")) {
                                // Queue changed away from initial → FSR FG activated
                                s_currentFGQueue = rawQueue;
                                HookLogImportant(
                                    "DX12: FG detected via queue change "
                                    "(initial=%p, current=%p, gameQ=%p, frame=%d)",
                                    s_initialQueue, rawQueue, gameQueue, s_queueFrameCount);
                            } else {
                                static std::atomic<int> s_ignoredQueueChangeLogCount{0};
                                if (s_ignoredQueueChangeLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                                    HookLog(
                                        "DX12: Ignoring queue change heuristic "
                                        "(initial=%p, current=%p, rawQ=%p, frame=%d)",
                                        s_initialQueue, rawQueue, gameQueue, s_queueFrameCount);
                                }
                            }
                        }
                        // else: FG already active, FG queue seen again — normal FSR FG alternation
                    } else {
                        // Seeing initial queue. During FSR FG this happens every other frame.
                        // Only deactivate after many CONSECUTIVE initial-queue frames.
                        if (s_currentFGQueue) {
                            ++s_consecutiveInitialQueueFrames;
                            if (s_consecutiveInitialQueueFrames >= kDeactivationThreshold) {
                                HookLogImportant(
                                    "DX12: FG deactivated via queue revert after %d consecutive initial-queue "
                                    "frames (initial=%p, fgQueue=%p, frame=%d)",
                                    s_consecutiveInitialQueueFrames, s_initialQueue, s_currentFGQueue,
                                    s_queueFrameCount);
                                s_currentFGQueue = nullptr;

                                s_consecutiveInitialQueueFrames = 0;
                                UpdateHeuristicFSRFGState(false, "queue-change");
                            } else if (s_consecutiveInitialQueueFrames == 1 || s_consecutiveInitialQueueFrames == 30) {
                                HookLog("DX12: Seeing initial queue while FG active (consecutive=%d/%d, frame=%d)",
                                        s_consecutiveInitialQueueFrames, kDeactivationThreshold, s_queueFrameCount);
                            }
                        }
                    }
                }
            }
        }
    }

    // Conditional block: when a startup-blocking overlay (Social Club, EOS) is present
    // and FG is inactive, only allow overlay rendering if we have the correct swapchain
    // queue captured.  GTA5 Enhanced rejects ECL submissions on any queue other than the
    // one the swapchain was created with.  Without g_SwapchainQueue we'd fall back to
    // g_CommandQueue (from ECL hooks) which is often a different queue → ERR_GFX_STATE.
    const bool startupOverlayPresent = startupOverlayCompatibilityActive;
    if (startupOverlayPresent) {
        bool hasSwapchainQueue;
        {
            std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
            hasSwapchainQueue = (dx12_hook_g_SwapchainQueue != nullptr);
        }
        const bool preserveLiveOverlayDuringHandoff =
            ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff();
        if (!ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(
                startupOverlayPresent, hasSwapchainQueue, dx12_hook_g_FGRuntimeOwnsSwapchain, runtimeOwnedSwapchainActiveMs,
                dx12_hook_kStartupOverlayPostResumeSettleMs, dx12_hook_s_startupOverlayCompatSettled.load(std::memory_order_acquire),
                preserveLiveOverlayDuringHandoff)) {
            allowOverlayRender = false;
            dx12_hook_g_PiggybackOverlayActive.store(false, std::memory_order_relaxed);
            static std::atomic<int> s_noQueueBlockLogCount{0};
            if (s_noQueueBlockLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                if (!hasSwapchainQueue) {
                    HookLogImportant(
                        "DX12: Overlay blocked during startup-overlay compatibility - swapchain queue not captured "
                        "(would use wrong queue)");
                } else {
                    HookLogImportant(
                        "DX12: Overlay blocked during startup-overlay compatibility - runtime-owned swapchain queue "
                        "still unstable");
                }
            }
        } else {
            // Startup compatibility remains active, but the observed queue
            // topology is stable enough to allow the barrier-free path.
            static std::atomic<int> s_queueOkLogCount{0};
            if (s_queueOkLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
                HookLogImportant(
                    "DX12: Overlay allowed during startup-overlay compatibility - queue topology stable enough for "
                    "barrier-free mode");
            }
            if (preserveLiveOverlayDuringHandoff) {
                static std::atomic<int> s_preservedStartupOverlayLogCount{0};
                const int preserveLogCount = s_preservedStartupOverlayLogCount.fetch_add(1, std::memory_order_relaxed);
                if (preserveLogCount < 5 || (preserveLogCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: Overlay kept visible during runtime-inactive Streamline startup handoff "
                        "(scQueue=%p origGame=%p runtimeOwnedMs=%llums)",
                        dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue, runtimeOwnedSwapchainActiveMs);
                }
            }
        }
    } else {
        dx12_hook_g_PiggybackOverlayActive.store(false, std::memory_order_relaxed);
    }

    // Remove delay - install overlay immediately(Strange Brigade compatibility)
    if (allowOverlayRender && !suspendOverlayRender && !dx12_hook_g_State.overlayInit) {
        // CRITICAL: Don't reinitialize overlay during FG transition cooldown.
        // During FG mode switches (e.g., FSR FG → DLSS FG), the SL / FSR runtime
        // is mid-initialization.  Creating D3D12 resources (allocators, fences,
        // PSOs) on a potentially wrong queue can corrupt GPU state, causing the
        // FG runtime to crash (observed: sl_dlss_g exception 0x00008000 in Talos).
        if (dx12_hook_g_FGTransitionCooldown > 0) {
            dx12_hook_g_FGTransitionCooldown.fetch_sub(1, std::memory_order_acq_rel);
            const bool streamlineFGRunningDuringReinitCooldown =
                DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
            const bool startupActivationPendingDuringReinitCooldown =
                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
            const bool postSLActiveButUnconfirmedDuringReinitCooldown = HookIsPostSLOverlayActiveButUnconfirmed();
            const bool postSLConfirmedDuringReinitCooldown = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
            const bool postSLSettlingDuringReinitCooldown = HookIsPostSLOverlayConfirmedButStartupSettling();
            const bool preserveSyntheticStartupStateDuringReinitCooldown =
                ce::dx12_overlay_policy::ShouldLetSyntheticPostSLProgressDuringOverlayReinitCooldown(
                    streamlineFGRunningDuringReinitCooldown, startupActivationPendingDuringReinitCooldown,
                    postSLActiveButUnconfirmedDuringReinitCooldown, postSLConfirmedDuringReinitCooldown,
                    postSLSettlingDuringReinitCooldown);
            if (!preserveSyntheticStartupStateDuringReinitCooldown) {
                // Suppress post-SL rendering during cooldown unless the same
                // synthetic startup is already half-armed and still waiting for
                // first confirmation. Otherwise the reinit cooldown path restarts
                // the same pure-DLSS startup into a second reactivation epoch.
                dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
            }
            if (preserveSyntheticStartupStateDuringReinitCooldown) {
                // Keep the reinit/pre-SL path cooled down, but do not re-apply
                // that cooldown to PostSL. PostSL is the safe timing path during
                // active DLSS-G and can rebuild the backend inline on the fresh
                // authoritative Streamline swapchain.
                dx12_hook_g_PostSLCooldownRemaining.store(0, std::memory_order_release);
            } else {
                dx12_hook_g_PostSLCooldownRemaining.store(dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire),
                                                std::memory_order_release);
            }
            static std::atomic<int> s_fgCooldownReinitBlockLogCount{0};
            int logCount = s_fgCooldownReinitBlockLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10 ||
                (preserveSyntheticStartupStateDuringReinitCooldown && (dx12_hook_g_FGTransitionCooldown % 30) == 0) ||
                dx12_hook_g_FGTransitionCooldown == 0) {
                HookLogImportant(
                    "DX12: Deferring overlay reinit during FG transition cooldown (%d frames remaining, "
                    "preserveHalfArmedPostSL=%d postSLCooldown=%d)",
                    dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire),
                    preserveSyntheticStartupStateDuringReinitCooldown ? 1 : 0,
                    dx12_hook_g_PostSLCooldownRemaining.load(std::memory_order_relaxed));
            }
            if (dx12_hook_g_FGTransitionCooldown == 0) {
                s_fgCooldownReinitBlockLogCount.store(0, std::memory_order_relaxed);
                // Re-enable PostSL if SL FG is active NOW.
                // The main cooldown code (inside overlayInit block) won't run because
                // we're about to reinit (overlayInit=false).  Without this, PostSL
                // stays inactive and the pre-SL render runs — which crashes during
                // DLSS FG because pre-SL ECL perturbs SL's FG pipeline.
                bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
                if (slFGNow && DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed)) {
                    const bool preserveSyntheticStartupState =
                        ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(
                            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                                std::memory_order_acquire),
                            HookIsPostSLOverlayActiveButUnconfirmed(),
                            dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                            HookIsPostSLOverlayConfirmedButStartupSettling());
                    const bool keepStartupHandoffPending = ce::dx12_overlay_policy::
                        ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(
                            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                                std::memory_order_acquire),
                            HookIsPostSLOverlayActiveButUnconfirmed(),
                            dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                            HookIsPostSLOverlayConfirmedButStartupSettling());
                    dx12_hook_g_PostSLOverlayActive.store(true, std::memory_order_release);
                    dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
                    DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(!keepStartupHandoffPending,
                                                                                    std::memory_order_release);
                    if (!preserveSyntheticStartupState) {
                        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(
                            false, std::memory_order_release);
                        dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
                        DXGIShared::ResetStreamlineStartupTransitionState();
                        HookLogImportant(
                            "DX12: FG transition cooldown complete — reactivated PostSL (slFG=1, reinit path)");
                    } else {
                        HookLogImportant(
                            "DX12: FG transition cooldown complete — preserving half-armed synthetic PostSL startup "
                            "state until confirmed render (slFG=1, reinit path)");
                    }
                } else {
                    HookLogImportant(
                        "DX12: FG transition cooldown complete — overlay reinit will proceed next frame (slFG=%d)",
                        slFGNow ? 1 : 0);
                }
            }
            // Skip reinit but continue ProcessFrame.
            goto skipOverlayInit;
        }

        // Don't reinit during active SL FG if PostSL callback isn't registered yet.
        // Without PostSL, the overlay would try pre-SL rendering on origGame while
        // backbuffers are on SL's swapchain queue → cross-queue ERR_GFX_STATE.
        // Once PostSL is registered, reinit is safe — PostSL renders on scQueue.
        {
            bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
            if (slFGNow) {
                auto* callback = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
                if (!callback) {
                    static int s_slDeferLogCount = 0;
                    if (s_slDeferLogCount++ < 5) {
                        HookLogImportant("DX12: Deferring overlay reinit — SL FG active, PostSL not registered yet");
                    }
                    goto skipOverlayInit;
                }
            }
        }

        {
            ID3D12CommandQueue* currentSwapchainQueue = nullptr;
            {
                std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
                currentSwapchainQueue = dx12_hook_g_SwapchainQueue;
            }
            ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
            bool actualFGActive = IsActualFrameGenerationActive();
            bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
            const bool recentStreamlineTeardown = dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
            const bool lastWorkingQueueStillActiveDuringRecentTeardown =
                dx12_hook_g_PostSLLastWorkingQueue != nullptr &&
                GetTickCount64() < dx12_hook_g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
            int slOffSwapchainGrace = dx12_hook_g_SLOffSwapchainReinitGrace.load(std::memory_order_acquire);
            // Retained no-callback FSR suspension: AMD keeps the FI swapchain + queue latched while the
            // app renders on origGame, so the queue-settle condition below can never be met — the policy
            // exempts it so the overlay re-inits and draws on the runtime-owned swapchain queue (the
            // suspension-approved backbuffer route; test app session 20260702_142655 was blank the whole
            // suspension without this).
            const bool retainedNoCallbackFSRSuspension =
                dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire) &&
                dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
            if (ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(
                    actualFGActive, streamlineFGRunning, dx12_hook_g_FGRuntimeOwnsSwapchain, currentSwapchainQueue != nullptr,
                    currentCommandQueue != nullptr,
                    currentCommandQueue != nullptr && currentCommandQueue == currentSwapchainQueue,
                    retainedNoCallbackFSRSuspension)) {
                // Attribute these presents as gated so a blank window here shows up as an
                // [OVERLAY COVERAGE] uncovered streak instead of hiding behind coverage inheritance
                // (session 20260702_142655 had ZERO streaks logged while the overlay was invisible).
                NoteDX12OverlayCoverageGate("runtime-owned-init-queue-settle-defer");
                static std::atomic<int> s_runtimeOwnedInactiveInitDeferLogCount{0};
                int logCount = s_runtimeOwnedInactiveInitDeferLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 120) == 0) {
                    HookLogImportant(
                        "DX12: Deferring inactive runtime-owned swapchain overlay init until queue settles "
                        "(slOffGrace=%d scQ=%p cmdQ=%p fgOwned=%d)",
                        slOffSwapchainGrace, currentSwapchainQueue, currentCommandQueue,
                        dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0);
                }
                goto skipOverlayInit;
            }

            const bool commandQueueMatchesPrimaryGameQueue =
                currentCommandQueue != nullptr &&
                currentCommandQueue == dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
            if (ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
                    actualFGActive, streamlineFGRunning, recentStreamlineTeardown, currentSwapchainQueue != nullptr,
                    dx12_hook_g_OriginalGameQueue != nullptr, dx12_hook_g_PostSLLastWorkingQueue != nullptr, currentCommandQueue != nullptr,
                    currentCommandQueue != nullptr && currentCommandQueue == currentSwapchainQueue,
                    currentCommandQueue != nullptr && currentCommandQueue == dx12_hook_g_OriginalGameQueue,
                    commandQueueMatchesPrimaryGameQueue)) {
                // Attribute as gated so any blank window here is a visible [OVERLAY COVERAGE] streak.
                NoteDX12OverlayCoverageGate("sl-teardown-queue-settle-defer");
                static std::atomic<int> s_recentSLTeardownInitDeferLogCount{0};
                int logCount = s_recentSLTeardownInitDeferLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 120) == 0) {
                    HookLogImportant(
                        "DX12: Deferring overlay init until command queue settles after recent Streamline teardown "
                        "(scQ=%p cmdQ=%p origQ=%p primaryQ=%p lastWorkingQ=%p slOffGrace=%d)",
                        currentSwapchainQueue, currentCommandQueue, dx12_hook_g_OriginalGameQueue,
                        dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire), dx12_hook_g_PostSLLastWorkingQueue,
                        dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire));
                }
                goto skipOverlayInit;
            }

            // One-shot diagnostic: when the primary-queue escape hatch allows overlay init
            // despite a missing swapchain queue and cleared lastWorkingQueue, log it so
            // future traces can distinguish "primaryQ safe" from "lastWorkingQ preserved".
            if (recentStreamlineTeardown && currentSwapchainQueue == nullptr && dx12_hook_g_PostSLLastWorkingQueue == nullptr &&
                commandQueueMatchesPrimaryGameQueue) {
                static std::atomic<int> s_primaryQEscapeHatchLogCount{0};
                if (s_primaryQEscapeHatchLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                    HookLogImportant(
                        "DX12: Allowing overlay init on primary game queue despite missing scQueue and lastWorkingQ "
                        "after Streamline teardown (cmdQ=%p primaryQ=%p origQ=%p slOffGrace=%d)",
                        currentCommandQueue, dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire), dx12_hook_g_OriginalGameQueue,
                        dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire));
                }
            }
        }

        ULONGLONG startupInitDelayRemainingMs = 0;
        if (ShouldDeferOverlayInitForStartupCompat(frameDesc.OutputWindow, &startupInitDelayRemainingMs)) {
            static std::atomic<int> s_startupInitDelayLogCount{0};
            if (s_startupInitDelayLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant(
                    "DX12: Delaying overlay init during startup compatibility grace for %s (remaining=%llums)",
                    g_ProcessName, startupInitDelayRemainingMs);
            }
            return;
        }

        if (deferOverlayWorkAfterResume) {
            static std::atomic<int> s_postResumeSettleLogCount{0};
            if (s_postResumeSettleLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                if (runtimeOwnedSwapchainNeedsExtraResumeSettle) {
                    HookLogImportant(
                        "DX12: Keeping overlay work deferred after startup-overlay resume for %s while the "
                        "runtime-owned swapchain queue is still active",
                        g_ProcessName);
                } else {
                    HookLogImportant(
                        "DX12: Keeping overlay work deferred after startup-overlay resume for %s (remaining=%llums)",
                        g_ProcessName, postResumeSettleRemainingMs);
                }
            }
            return;
        }

        if (dx12_hook_s_insideECL) {
            static std::atomic<int> s_initDeferredInEclLogCount{0};
            if (s_initDeferredInEclLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLog("DX12: Deferring overlay init while inside ExecuteCommandLists re-entry");
            }
            return;
        }

        int frames = ++dx12_hook_s_framesBeforeInit;
        if (frames < 1) {
            return;
        } else if (frames == 1) {
            HookLog("DX12: ProcessFrame - Proceeding with overlay init");
        }

        // Rate-limit reinit attempts: after 3 consecutive failures, back off
        // exponentially (wait 60, 120, 240… frames). This prevents the log-spam
        // and driver-stall loop that occurs when the device is removed but the
        // early health check at the top of ProcessFrame somehow misses it.
        static int s_consecutiveInitFails = 0;
        static int s_nextRetryFrame = 0;
        if (s_consecutiveInitFails >= 3 && frames < s_nextRetryFrame) {
            return;
        }

        const char* skipSeparateOverlayGpuReason = nullptr;
        if (ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain(&skipSeparateOverlayGpuReason)) {
            static std::atomic<int> s_runtimeOwnedSeparateWorkSkipLogCount{0};
            int logCount = s_runtimeOwnedSeparateWorkSkipLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                const bool ffxStalled = IsFFXPresentCallbackStalled();
                const bool ffxStallAllows =
                    ShouldAllowNormalOverlayFallbackForCurrentFFXPresentCallbackStall(ffxStalled);
                HookLogImportant(
                    "DX12: Deferring overlay init because %s — decision matrix: "
                    "runtime=%s apiFSR=%d directFFX=%d progressResolved=%d nativeFGPath=%d "
                    "explicitNativeOff=%d ffxStalled=%d ffxStallAllows=%d runtimeOwns=%d "
                    "callbackEver=%d callbackLast=%llu sameQueue=%d stableProof=%d "
                    "cooldown=%d overlayInit=%d syncInit=%d "
                    "scQueue=%p origGame=%p cmdQ=%p",
                    skipSeparateOverlayGpuReason ? skipSeparateOverlayGpuReason : "runtime-owned swapchain",
                    ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()),
                    g_FGCompat.IsFSRFGApiActive() ? 1 : 0, g_FGCompat.HasDirectFFXApiConfirmation() ? 1 : 0,
                    dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire) ? 1 : 0,
                    HookHasRuntimeOwnedNativeFGPresentPath() ? 1 : 0,
                    dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire) ? 1 : 0,
                    ffxStalled ? 1 : 0, ffxStallAllows ? 1 : 0, dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0,
                    dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire) != 0 ? 1 : 0,
                    static_cast<unsigned long long>(dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire)),
                    (dx12_hook_g_SwapchainQueue != nullptr && dx12_hook_g_OriginalGameQueue != nullptr &&
                     dx12_hook_g_SwapchainQueue == dx12_hook_g_OriginalGameQueue)
                        ? 1
                        : 0,
                    EvaluateProgressResolvedOfficialFFXOverlayFallbackProof().proof ? 1 : 0,
                    dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), dx12_hook_g_State.overlayInit ? 1 : 0,
                    dx12_hook_g_State.syncInit ? 1 : 0, dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue,
                    g_CommandQueue.load(std::memory_order_acquire));
            }
            return;
        }

        // CRITICAL FIX: Don't initialize ImGui during FG suspension, FSR
        // stabilization, or native FSR FG This prevents initialization with
        // potentially unstable frame generation state and avoids rebuilding
        // overlay state on a queue topology that is still mid-transition
        // CRITICAL FIX: Clean up any existing overlay context from previous
        // swapchain This happens when FSR FG recreates the swapchain and we
        // deferred cleanup MUST hold mutex to prevent race with DrawOverlay
        if (g_OverlayAdapter.IsInitialized()) {
            std::lock_guard<std::recursive_mutex> cleanupLock(dx12_hook_g_OverlayMutex);
            const bool preserveNativeFSRPresentCallbackBackend =
                ce::dx12_overlay_policy::ShouldPreserveFFXPresentCallbackBackendDuringNormalOverlayCleanup(
                    dx12_hook_g_FFXPresentOverlayAdapter.IsInitialized(), HookHasRuntimeOwnedNativeFGPresentPath());
            HookLog("DX12: ProcessFrame - releasing swapchain/queue-bound overlay state (mutex held)");
            if (preserveNativeFSRPresentCallbackBackend) {
                HookLogImportant(
                    "DX12: ProcessFrame - preserving native FSR present-callback overlay backend during normal "
                    "overlay cleanup because the runtime-owned native FG Present path still owns presentation "
                    "(runtime=%s scQ=%p origGame=%p explicitOff=%d)",
                    ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()), dx12_hook_g_SwapchainQueue,
                    dx12_hook_g_OriginalGameQueue,
                    dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire) ? 1 : 0);
            }
            // Warm-backend: keep the adapter's device-scoped resources (PSOs,
            // font atlas, upload pools) alive across the FG transition; only
            // swapchain/queue-bound state (RTVs, allocators, fence) is released
            // here, after the transition GPU drains already ran. InitImGui
            // below reuses the warm backend when device+format still match and
            // shuts it down for a full rebuild otherwise.
            dx12_hook_g_PreserveOverlayAdapterAcrossResize.store(true, std::memory_order_release);
            CleanupOverlay(preserveNativeFSRPresentCallbackBackend);
            CleanupRTVs();
            HookLog("DX12: ProcessFrame - swapchain-scoped cleanup complete, proceeding with init (warm backend kept)");
        }

        DXGI_SWAP_CHAIN_DESC desc;
        if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            dx12_hook_g_State.cachedWidth = desc.BufferDesc.Width;
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            dx12_hook_g_State.cachedHeight = desc.BufferDesc.Height;

            // Use actual swapchain buffer count for ImGui initialization
            // The separate overlay queue (Change 1) eliminates the need for buffer
            // limiting
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            int imguiBufferCount = desc.BufferCount;

            HookLog("DX12: ProcessFrame - initializing ImGui (%dx%d, buffers=%d)", dx12_hook_g_State.cachedWidth,
                    dx12_hook_g_State.cachedHeight, imguiBufferCount);

            // Validate swapchain buffers are accessible before initializing
            IDXGISwapChain3* sc3 = nullptr;
            if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                int validBuffers = 0;
                for (int i = 0; i < imguiBufferCount; i++) {
                    ID3D12Resource* bb = nullptr;
                    if (SUCCEEDED(sc3->GetBuffer(i, IID_PPV_ARGS(&bb)))) {
                        if (bb) {
                            bb->Release();
                            validBuffers++;
                        }
                    } else {
                        HookLog(
                            "DX12: ProcessFrame - buffer %d not accessible, stopping "
                            "validation",
                            i);
                        break;
                    }
                }

                if (validBuffers < imguiBufferCount) {
                    HookLog(
                        "DX12: ProcessFrame - only %d/%d buffers valid, skipping "
                        "ImGui init this frame",
                        validBuffers, imguiBufferCount);
                    sc3->Release();
                    return;
                }

                if (InitImGui(g_Device.load(), imguiBufferCount, desc.BufferDesc.Format, desc.OutputWindow)) {
                    s_consecutiveInitFails = 0;
                    s_nextRetryFrame = 0;

                    int outputColorSpace = -1;
                    bool presentationContractSupported = false;
                    const bool isActualHDR =
                        ResolveSwapchainOutputHDRState(static_cast<IDXGISwapChain*>(sc3), desc.BufferDesc.Format,
                                                       "DX12: Swapchain color contract", &outputColorSpace,
                                                       &presentationContractSupported);
                    UpdateLastKnownSwapchainHDRStateCache(desc.BufferDesc.Format, isActualHDR, outputColorSpace,
                                                          presentationContractSupported);
                    g_OverlayAdapter.SetHDR(isActualHDR, (int)desc.BufferDesc.Format);

                    // Propagate HDR state to media engine via shared memory
                    if (g_pSharedMem) {
                        g_pSharedMem->SetIsHDR(isActualHDR);
                    }

                    if (dx12_hook_s_startupOverlayActivationStage ==
                        StartupOverlayActivationStage::kDelayRTVInitAfterBackendInit) {
                        dx12_hook_s_startupOverlayActivationStageMs = GetTickCount64();
                        HookLogImportant(
                            "DX12: Startup compat staged activation - backend init complete, delaying RTV init for "
                            "%llums",
                            dx12_hook_kStartupOverlayPostBackendInitSettleMs);
                    } else {
                        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                        int actualBufferCount = desc.BufferCount;
                        if (actualBufferCount > 8) {
                            HookLog("DX12: Swapchain has %d buffers, limiting RTVs to 8", actualBufferCount);
                            actualBufferCount = 8;
                        }
                        CreateRTVs(g_Device.load(), sc3, actualBufferCount);
                        if (!dx12_hook_g_State.rtvDescHeap) {
                            HookLogImportant(
                                "DX12: RTV initialization failed during overlay init, deferring sync init");
                            sc3->Release();
                            return;
                        }
                        InitOverlaySync(g_Device.load(), imguiBufferCount, gameQueue);

                        HookLog(
                            "DX12: ProcessFrame - ImGui initialized with %d RTVs, "
                            "syncInit=%d",
                            actualBufferCount, dx12_hook_g_State.syncInit);
                    }
                } else {
                    s_consecutiveInitFails++;
                    int backoffFrames = 60 * (1 << std::min(s_consecutiveInitFails - 3, 5));
                    s_nextRetryFrame = frames + backoffFrames;
                    if (s_consecutiveInitFails <= 5 || (s_consecutiveInitFails % 100) == 0) {
                        HookLog(
                            "DX12: ProcessFrame - ImGui initialization FAILED (attempt %d, next retry in %d frames)",
                            s_consecutiveInitFails, backoffFrames);
                    }
                }
                // SAFETY: Check sc3 is still valid before releasing
                if (sc3) {
                    sc3->Release();
                }
            } else {
                HookLog("DX12: ProcessFrame - failed to get IDXGISwapChain3 interface");
            }
        } else {
            HookLog("DX12: ProcessFrame - failed to get swapchain desc");
        }
    }
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
            return;
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
                return;
            }
        }

        DXGI_SWAP_CHAIN_DESC desc = {};
        if (FAILED(pSwapChain->GetDesc(&desc))) {
            HookLog("DX12: ProcessFrame - failed to get swapchain desc for staged activation");
            return;
        }

        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
            HookLog("DX12: ProcessFrame - failed to get SwapChain3 for staged activation");
            return;
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

                    return;
                }
            }
            dx12_hook_s_startupOverlayActivationStage = StartupOverlayActivationStage::kDelaySyncInitAfterRTVInit;
            dx12_hook_s_startupOverlayActivationStageMs = GetTickCount64();
            HookLogImportant(
                "DX12: Startup compat staged activation - RTV init complete, delaying sync init for %llums",
                dx12_hook_kStartupOverlayPostRTVInitSettleMs);
            sc3->Release();
            return;
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
                return;
            }
        }

        if (!dx12_hook_g_State.rtvDescHeap) {
            CreateRTVs(g_Device.load(), sc3, actualBufferCount);
            if (!dx12_hook_g_State.rtvDescHeap) {
                HookLogImportant("DX12: RTV initialization failed during staged sync init, keeping overlay deferred");
                sc3->Release();
                return;
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
        return;
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
    const bool focusLossBackgroundUsingDedicatedQueue = dx12_hook_g_State.overlayQueue && ShouldUseDedicatedOverlayQueue();
    const bool focusLossBackgroundRuntimeOwnedPresentation =
        dx12_hook_g_FGRuntimeOwnsSwapchain || HookHasRuntimeOwnedNativeFGPresentPath() || DXGIShared::DoesFGRuntimeOwnSwapchain();
    const bool focusLossBackgroundSteamDeferredSubmit =
        dx12_hook_g_deferOverlaySubmitToSteamECL && !focusLossBackgroundUsingDedicatedQueue;
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
    const bool swapchainOccluded = dx12_hook_g_SwapchainPresentOccluded.load(std::memory_order_acquire);
    // The first predicate arg means "we have a reliable present-result occlusion signal",
    // which is now true for both the wrapped path (context valid) and the vtable DetourPresent
    // path (g_HaveD3D12PresentResultSignal). This lets vtable-hooked apps engage the
    // invisible-safe not-presentable hold during the Alt+Tab mode switch instead of hanging.
    const bool haveReliablePresentResultSignal =
        dx12_hook_s_WrappedPresentFocusLossContext.valid || dx12_hook_g_HaveD3D12PresentResultSignal.load(std::memory_order_acquire);
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
    const int focusTransitionHoldRemaining = dx12_hook_g_FocusTransitionHoldFrames.load(std::memory_order_acquire);
    const bool focusTransitionActive = ce::dx12_overlay_policy::IsD3D12FocusTransitionTelemetryActive(
        frameDesc.Windowed != 0, focusTransitionHoldRemaining, focusLossBackgroundFrameGenerationActive,
        focusLossBackgroundRuntimeOwnedPresentation, focusLossBackgroundUsingDedicatedQueue,
        focusLossBackgroundSteamDeferredSubmit, focusLossBackgroundDeviceLost, gameQueue != nullptr);
    if (focusTransitionActive) {
        // Widen the device-removal dump window so any residual hang at the mode
        // switch is captured with DRED breadcrumbs.
        dx12_hook_g_FocusLossRecentTransitionPresentWindow.store(dx12_hook_kFocusLossRecentTransitionDumpWindowFrames,
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

    if (!observerOnlyMode && !dx12_hook_s_insideECL && dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit) {
        bool outerSLFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        bool previousOuterSLFGRunning = dx12_hook_g_OuterTrackedSLFGRunning.load(std::memory_order_acquire);

        if (outerSLFGRunning != previousOuterSLFGRunning) {
            bool slTurnedOff = previousOuterSLFGRunning && !outerSLFGRunning;
            bool slTurnedOn = !previousOuterSLFGRunning && outerSLFGRunning;
            dx12_hook_g_OuterTrackedSLFGRunning.store(outerSLFGRunning, std::memory_order_release);
            const bool preserveActivePostSLOnLateOuterOn =
                slTurnedOn && ce::dx12_overlay_policy::ShouldPreserveActivePostSLDuringFGCooldown(
                                  outerSLFGRunning, dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                                  HookIsPostSLOverlayActiveButUnconfirmed());
            auto* transitionDevice = g_Device.load(std::memory_order_acquire);
            const HRESULT transitionDeviceHr = transitionDevice ? transitionDevice->GetDeviceRemovedReason() : S_OK;
            const bool bypassPureStreamlineOffCooldown =
                ce::dx12_overlay_policy::ShouldBypassPureStreamlineFGOffOverlayReinitCooldown(
                    slTurnedOff, dx12_hook_g_HadFSRFGPhase, g_FGCompat.IsFSRFGApiActive(),
                    HookHasRuntimeOwnedNativeFGPresentPath(), dx12_hook_g_State.overlayInit, dx12_hook_g_State.syncInit,
                    dx12_hook_g_SwapchainQueue != nullptr, dx12_hook_g_OriginalGameQueue != nullptr, FAILED(transitionDeviceHr));
            // Make-before-break: DX12_OnStreamlineFGStateChanged latched the
            // keep-alive at the explicit OFF edge; the outer teardown must not
            // disable the confirmed PostSL path that is covering the proxy's
            // remaining presents.
            const bool keepConfirmedPostSLAliveAcrossOuterOff =
                slTurnedOff && dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire);
            const auto* lastSuccessfulPostSLSwapchain = dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire);
            const bool preserveConfirmedPostSLProxyResourcesAcrossOuterOff =
                ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLProxyResourcesAcrossOuterOff(
                    slTurnedOff, keepConfirmedPostSLAliveAcrossOuterOff,
                    pSwapChain != nullptr && pSwapChain == lastSuccessfulPostSLSwapchain, dx12_hook_g_State.overlayInit,
                    dx12_hook_g_State.syncInit, FAILED(transitionDeviceHr));
            // A confirmed-PostSL DLSS-FG SUSPEND (proxy stays live) is safe to rebuild
            // immediately even WITH FSR history: PostSL confirmed rendering means the
            // overlay ECL on the runtime-owned SL queue already succeeded this epoch, so
            // the generic 60-frame reinit cooldown only blanks a provably-live overlay
            // (session 20260613_150750: 60-present / 672 ms blank). The stricter cooldown
            // is kept for any current FSR/native-FG ownership or device-removal.
            const bool bypassConfirmedPostSLSuspensionCooldown =
                ce::dx12_overlay_policy::ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(
                    slTurnedOff, dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire),
                    dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire), g_FGCompat.IsFSRFGApiActive(),
                    HookHasRuntimeOwnedNativeFGPresentPath(), dx12_hook_g_State.overlayInit, dx12_hook_g_State.syncInit,
                    dx12_hook_g_SwapchainQueue != nullptr, dx12_hook_g_OriginalGameQueue != nullptr, FAILED(transitionDeviceHr));
            // DLSS-FG -> FSR-FG (no-callback) takeover: the native-FSR takeover path already warm-reinited
            // the overlay on the runtime-owned FSR queue this frame; the [outer] teardown below would
            // force-clear it + 60-frame cooldown (session 20260615_020100: missed=60). Keep it live —
            // strictly the no-callback route (the app-callback bridge keeps the teardown for crash safety).
            const bool keepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover =
                ce::dx12_overlay_policy::ShouldKeepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover(
                    slTurnedOff, g_FGCompat.IsFSRFGApiActive(),
                    dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                    HookHasRuntimeOwnedNativeFGPresentPath(), dx12_hook_g_State.overlayInit, dx12_hook_g_State.syncInit,
                    FAILED(transitionDeviceHr));
            const bool keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn =
                ce::dx12_overlay_policy::ShouldKeepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn(
                    slTurnedOff, authoritativeDLSSOffNormalReturnReinitializedThisPresent,
                    postFSRNormalRouteOwnershipProven, dx12_hook_g_State.overlayInit, dx12_hook_g_State.syncInit,
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
                dx12_hook_g_FGTransitionCooldown.store(0, std::memory_order_release);
                dx12_hook_g_PostSLCooldownRemaining.store(0, std::memory_order_release);
                HookLogImportant(
                    "DX12: [outer] %s — bypassing generic reinit cooldown "
                    "(scQueue=%p origGame=%p devHr=0x%08X)",
                    keepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover
                        ? "DLSS->FSR no-callback takeover (overlay already reinited on FSR queue)"
                    : keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn
                        ? "authoritative DLSS-off native return (overlay already reinited on exact game swapchain)"
                        : (bypassPureStreamlineOffCooldown ? "pure Streamline FG OFF"
                                                           : "confirmed-PostSL DLSS-FG suspension (proxy stays live)"),
                    dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue, (unsigned)transitionDeviceHr);
            } else {
                dx12_hook_g_FGTransitionCooldown = std::max(dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), 60);
                dx12_hook_g_PostSLCooldownRemaining.store(dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire),
                                                std::memory_order_release);
            }

            // Reset PostSL state for fresh start after transition.
            // Keep the callback installed on Streamline FG activation so
            // startup synthetic presents can immediately find it.
            if (!preserveActivePostSLOnLateOuterOn && !keepConfirmedPostSLAliveAcrossOuterOff) {
                dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
            }
            if (!DXGIShared::ShouldKeepPostSLCallbackInstalledDuringTransition(outerSLFGRunning) &&
                !keepConfirmedPostSLAliveAcrossOuterOff) {
                SetPostSLCallbackInstalled(false, "DX12: [outer] SL transition");
            }
            dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
            if (!preserveActivePostSLOnLateOuterOn && !keepConfirmedPostSLAliveAcrossOuterOff) {
                dx12_hook_g_PostSLStableFrameCount.store(0, std::memory_order_release);
                dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
                dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
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
            { dx12_hook_g_OuterSLTransitionEpoch.fetch_add(1, std::memory_order_release); }

            if (slTurnedOff) {
                // Suppress queue-change heuristic for frames after SL OFF.
                // The heuristic runs BEFORE this outer block in ProcessFrame, so
                // on the frame SL turns off, it sees queue switch (SL→origGame)
                // before the reset flag is set → false FSR_FG.
                // Use 600 frames (~4s@150fps) to cover high-fps menus where
                // SL's swapchain queue persists after FG teardown.
                dx12_hook_g_SLOffHeuristicGrace.store(600, std::memory_order_release);
                dx12_hook_g_SLOffSwapchainReinitGrace.store(300, std::memory_order_release);

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
                        dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue);
                }

                // Disable PostSL immediately — SL is tearing down — unless the
                // make-before-break keep-alive is covering the proxy's
                // remaining presents until the normal route confirms.
                if (!keepConfirmedPostSLAliveAcrossOuterOff) {
                    dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
                    SetPostSLCallbackInstalled(false, "DX12: [outer] FG->off");
                } else {
                    HookLogImportant(
                        "DX12: [outer] FG->off — PostSL keep-alive covers proxy presents until normal-route "
                        "recovery");
                }
                InvalidateAllOverlayCachedFrames();

                // Drain in-flight GPU work
                if (dx12_hook_g_State.fence && !preserveConfirmedPostSLProxyResourcesAcrossOuterOff &&
                    !keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn) {
                    UINT64 lastVal = dx12_hook_g_State.currentFenceValue;
                    HANDLE drainEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                    if (drainEvent) {
                        HRESULT drainHr = dx12_hook_g_State.fence->SetEventOnCompletion(lastVal, drainEvent);
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
                        lastSuccessfulPostSLSwapchain, dx12_hook_g_PostSLLastWorkingQueue);
                } else if (keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn) {
                    HookLogImportant(
                        "[OVERLAY VISIBILITY] First authoritative DLSS-off native Present keeps its newly rebuilt "
                        "overlay state (swapchain=%p queue=%p; no second drain/reinit)",
                        pSwapChain, dx12_hook_g_SwapchainQueue);
                }

                // Force overlay reinit — PostSL's RTVs reference SL's swapchain
                // backbuffers, which become invalid after SL tears down FG.  Without
                // reinit, pre-SL rendering uses stale RTVs → DEVICE_HUNG.
                // EXCEPTION (DLSS->FSR no-callback takeover): the native-FSR takeover path
                // already warm-reinited the overlay on the runtime-owned FSR swapchain queue
                // this same frame (its RTVs are valid for FSR's swapchain, not stale SL ones).
                // Tearing it down here is what produced the 60-present blank — keep it live.
                if (dx12_hook_g_State.overlayInit && !keepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover &&
                    !preserveConfirmedPostSLProxyResourcesAcrossOuterOff &&
                    !keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn) {
                    HookLogImportant("DX12: [outer] FG→off — forcing overlay reinit (stale SL backbuffers)");
                    dx12_hook_g_State.overlayInit = false;
                    CleanupRTVs();
                } else if (dx12_hook_g_State.overlayInit && preserveConfirmedPostSLProxyResourcesAcrossOuterOff) {
                    HookLogImportant(
                        "DX12: [outer] FG->off — exact confirmed PostSL proxy remains current; warm backend stays "
                        "drawable for this transition present");
                } else if (dx12_hook_g_State.overlayInit && keepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover) {
                    HookLogImportant(
                        "DX12: [outer] FG→off — DLSS->FSR no-callback takeover already reinited the overlay on the "
                        "runtime-owned FSR queue; keeping it live (no teardown, no cooldown blank)");
                } else if (dx12_hook_g_State.overlayInit && keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn) {
                    HookLogImportant(
                        "DX12: [outer] FG->off — exact native return already rebuilt the overlay this Present; "
                        "keeping it drawable");
                }
                dx12_hook_g_ResetReinitSubmitCounter.store(true, std::memory_order_release);

                // Clear realECL — it was probed from a temporary queue during
                // SL activation and may reference per-instance driver dispatch
                // state that doesn't match the game queue.  After SL teardown,
                // fall back to origECL (saved from the game queue's vtable
                // before our hook was installed).
                {
                    auto* oldRealECL = (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
                    dx12_hook_g_RealD3D12ECL.store(nullptr, std::memory_order_release);
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
                        auto* probed = (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);

                        HookLogImportant("DX12: [outer] SL FG ON — probed realECL=%p (dev=%p)", probed, dev);
                    } else {
                        dx12_hook_g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
                        HookLogImportant("DX12: [outer] SL FG ON — deferred ECL probe (startup window active, dev=%p)",
                                         dev);
                    }
                } else {
                    HookLogImportant("DX12: [outer] SL FG ON — skipped ECL probe (dev=%p, SL=%d)", dev,
                                     IsStreamlineLoaded() ? 1 : 0);
                }
            }
        }

        // Cooldown countdown — must always tick even when overlay blocked
        if (dx12_hook_g_FGTransitionCooldown > 0 && !allowOverlayRender) {
            // Only decrement here when the inner block won't run.
            // The inner block (inside allowOverlayRender gate) has its own
            // countdown logic.  Avoid double-decrementing.
            dx12_hook_g_FGTransitionCooldown.fetch_sub(1, std::memory_order_acq_rel);
            const bool preserveActivePostSLDuringBlockedCooldown =
                ce::dx12_overlay_policy::ShouldPreserveActivePostSLDuringFGCooldown(
                    outerSLFGRunning, dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                    HookIsPostSLOverlayActiveButUnconfirmed());
            if (!preserveActivePostSLDuringBlockedCooldown) {
                dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
            }
            dx12_hook_g_PostSLCooldownRemaining.store(dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire),
                                            std::memory_order_release);
            if (dx12_hook_g_FGTransitionCooldown == 0) {
                HookLogImportant("DX12: [outer] FG transition cooldown complete (slFG=%d)", outerSLFGRunning ? 1 : 0);
                if (outerSLFGRunning) {
                    const bool preserveSyntheticStartupState =
                        ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(
                            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                                std::memory_order_acquire),
                            HookIsPostSLOverlayActiveButUnconfirmed(),
                            dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                            HookIsPostSLOverlayConfirmedButStartupSettling());
                    const bool keepStartupHandoffPending = ce::dx12_overlay_policy::
                        ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(
                            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                                std::memory_order_acquire),
                            HookIsPostSLOverlayActiveButUnconfirmed(),
                            dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                            HookIsPostSLOverlayConfirmedButStartupSettling());
                    dx12_hook_g_PostSLOverlayActive.store(true, std::memory_order_release);
                    dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
                    DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(!keepStartupHandoffPending,
                                                                                    std::memory_order_release);
                    if (!preserveSyntheticStartupState) {
                        dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
                        DXGIShared::ResetStreamlineStartupTransitionState();
                    }
                }
            }
        }

        // PostSL callback management — register when SL FG active, even if overlay blocked
        if (outerSLFGRunning && dx12_hook_g_FGTransitionCooldown == 0) {
            if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) !=
                &PostSLOverlayRenderGated) {
                const bool preserveSyntheticStartupState =
                    ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(
                        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                            std::memory_order_acquire),
                        HookIsPostSLOverlayActiveButUnconfirmed(),
                        dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                        HookIsPostSLOverlayConfirmedButStartupSettling());
                const bool keepStartupHandoffPending =
                    ce::dx12_overlay_policy::ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(
                        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                            std::memory_order_acquire),
                        HookIsPostSLOverlayActiveButUnconfirmed(),
                        dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                        HookIsPostSLOverlayConfirmedButStartupSettling());
                SetPostSLCallbackInstalled(true, "DX12: [outer] Registered PostSL callback");
                dx12_hook_g_PostSLOverlayActive.store(true, std::memory_order_release);
                if (!preserveSyntheticStartupState) {
                    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false,
                                                                                            std::memory_order_release);
                    dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
                }
                dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
                DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(!keepStartupHandoffPending,
                                                                                std::memory_order_release);
                if (!preserveSyntheticStartupState) {
                    DXGIShared::ResetStreamlineStartupTransitionState();
                }
                HookLogImportant("DX12: [outer] Registered PostSL callback (overlay blocked, SL FG active)");
            }
        } else if (!outerSLFGRunning && dx12_hook_g_FGTransitionCooldown == 0 &&
                   !dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire)) {
            // Make-before-break: while the keep-alive latch is set, the
            // installed callback IS the coverage for the proxy's remaining
            // presents; PostSLOverlayRenderGated retires it on normal-route
            // recovery or Streamline unload.
            if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr) {
                SetPostSLCallbackInstalled(false, "DX12: [outer] cooldown complete");
                dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
                dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
                DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
            }
        }
    }

    // [OVERLAY COVERAGE] attribute the responsible gate when this present cannot
    // reach the overlay draw section below (condition mirrors the if that follows).
    if (!(allowOverlayRender && !suspendOverlayRender && !dx12_hook_s_insideECL && dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit &&
          !delayOverlayRenderAfterResume && !delayOverlayRenderAfterSyncInit &&
          !suppressOverlayRenderForLoadedStartupOverlay && !delayOverlayRenderAfterResourcePrime &&
          !delayOverlayRenderAfterFirstDrawProbe)) {
        NoteDX12OverlayCoverageGate(!allowOverlayRender    ? "overlay-render-suppressed"
                                    : suspendOverlayRender ? "swapchain-not-drawable"
                                    : dx12_hook_s_insideECL          ? "inside-ecl-reentry"
                                    : !dx12_hook_g_State.overlayInit ? "overlay-backend-uninitialized"
                                    : !dx12_hook_g_State.syncInit    ? "overlay-sync-uninitialized"
                                                           : "startup-render-delay");
    }

    if (allowOverlayRender && !suspendOverlayRender && !dx12_hook_s_insideECL && dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit &&
        !delayOverlayRenderAfterResume && !delayOverlayRenderAfterSyncInit &&
        !suppressOverlayRenderForLoadedStartupOverlay && !delayOverlayRenderAfterResourcePrime &&
        !delayOverlayRenderAfterFirstDrawProbe) {
        // Single log on first successful overlay render
        static int s_firstOverlayLogged = 0;
        if (s_firstOverlayLogged == 0) {
            s_firstOverlayLogged = 1;
            HookLogImportant(
                "DX12: ProcessFrame - first overlay render attempt (fence=%p, "
                "cmdList=%p, fgActive=%d, fgType=%s)",
                dx12_hook_g_State.fence, dx12_hook_g_State.cmdList, g_FGCompat.IsFGActive() ? 1 : 0,
                g_FGCompat.GetFGTypeName(g_FGCompat.GetActiveFGType()));
        }

        // FG state transition cooldown: skip overlay draws for a brief window
        // after FG mode changes to let Streamline stabilize its internal state.
        // Unlike the old cooldown (which did teardown/reinit and caused resource
        // churn crashes), this only pauses the draw — no resources are destroyed.
        static bool s_lastFGActive = false;
        static ce::fg_runtime::RuntimeMode s_lastRuntimeMode = ce::fg_runtime::RuntimeMode::kOff;
        static bool s_lastSLFGRunning = false;
        static ce::dx12_fg_transition::State s_transitionState;
        // NOTE: FG transition cooldown is now file-scope g_FGTransitionCooldown
        // so swapchain-change detection (earlier in ProcessFrame) can check it.
        bool currentFGActive = g_FGCompat.IsFGActive();
        auto currentRuntimeMode = g_FGCompat.GetRuntimeMode();
        bool currentSLFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);

        // Grace period counter — declared here so epoch sync can reference it.
        static int s_slOffGraceFrames = 0;

        // Epoch sync: when the outer FG state management block has already
        // processed an SL FG transition, bring our tracking variables in sync
        // to avoid redundant transition processing (double cooldowns, duplicate
        // GPU drain, swapchain queue re-clearing).
        static uint32_t s_innerSyncedEpoch = 0;
        uint32_t outerEpoch = dx12_hook_g_OuterSLTransitionEpoch.load(std::memory_order_acquire);
        if (s_innerSyncedEpoch != outerEpoch) {
            bool wasSlOn = s_lastSLFGRunning;
            s_lastFGActive = currentFGActive;
            s_lastRuntimeMode = currentRuntimeMode;
            s_lastSLFGRunning = currentSLFGRunning;
            s_innerSyncedEpoch = outerEpoch;
            // If SL turned off, start grace period (mirroring normal detection)
            if (wasSlOn && !currentSLFGRunning) {
                s_slOffGraceFrames = 300;
            }
            HookLogImportant(
                "DX12: [inner] Synced tracking to outer epoch %u (fgActive=%d runtime=%s slFG=%d grace=%d)", outerEpoch,
                currentFGActive ? 1 : 0, ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode),
                currentSLFGRunning ? 1 : 0, s_slOffGraceFrames);
        }

        // If SL directly signals FG is running, force currentFGActive true.
        // Heuristic detection may lag behind the SL hook's immediate signal,
        // creating a gap where IsFGActive() returns false even though SL FG
        // is already processing frames.
        if (currentSLFGRunning && !currentFGActive) {
            currentFGActive = true;
            if (!ce::fg_runtime::IsRuntimeFGActive(currentRuntimeMode)) {
                currentRuntimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG;
            }
        }

        // When SL signal goes from ON→OFF, the ECL heuristic may briefly
        // false-positive as FSR_FG (elevated ECL count from departing DLSS FG
        // looks like frame generation).  Suppress non-API FG detection for a
        // grace period after SL deactivates.  Also suppresses NVIDIA_SM false
        // positives from the cached 2× multiplier.
        if (s_lastSLFGRunning && !currentSLFGRunning) {
            // SL just turned OFF — start grace period.
            // 300 frames covers the slow ECL ratio decay after DLSS FG shutdown.
            s_slOffGraceFrames = 300;
            HookLogImportant("DX12: SL FG OFF — suppressing heuristic FG for 300 frames");
        }
        if (s_slOffGraceFrames > 0) {
            s_slOffGraceFrames--;
            // During grace period after SL FG OFF, suppress ALL non-API-confirmed
            // FG types.  The cached 2× multiplier from departing DLSS FG falsely
            // activates NVIDIA_SM detection, and elevated ECL counts falsely
            // trigger heuristic FSR_FG.  Only trust explicit API hooks
            // (fsrFGApiActive from ffxCreateContext).  DLSS_FG API requires SL
            // running, which is false during this grace period.
            if (!currentSLFGRunning && currentFGActive) {
                bool fsrApiConfirmed = g_FGCompat.IsFSRFGApiActive();
                if (!fsrApiConfirmed) {
                    currentFGActive = false;
                    currentRuntimeMode = ce::fg_runtime::RuntimeMode::kOff;
                }
            }
        }

        HookUpdatePreferredOverlayFGPublicationState(currentFGActive, currentRuntimeMode, "DX12::ProcessFrame");
        if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
            const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
            // Publish the locally-computed FG state so per-frame suppression
            // (e.g. SL-off grace period) is reflected in the overlay.
            if (ce::dx12_overlay_policy::DoOverlayFGPublishedTypesDiffer(plan.publishFGActive, plan.publishRuntimeMode,
                                                                         currentFGActive, currentRuntimeMode)) {
                HookLogImportant(
                    "DX12::ProcessFrame overlay divergence: plan(active=%d mode=%s) vs local(active=%d mode=%s)",
                    plan.publishFGActive ? 1 : 0, ce::fg_runtime::GetRuntimeModeName(plan.publishRuntimeMode),
                    currentFGActive ? 1 : 0, ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode));
            }
            ce::overlay_metrics::PublicationInput input;
            input.effectiveFGActive = currentFGActive;
            input.runtimeMode = currentRuntimeMode;
            input.outputFPS = g_FGCompat.GetOutputFPS();
            input.baseFPS = g_FGCompat.GetBaseFPS();
            input.multiplier = g_FGCompat.GetFGMultiplier();
            input.publicationSource = "DX12::ProcessFrame";
            ce::overlay_metrics::PublishOverlayFGMetrics(perf, input);
        }

        ID3D12CommandQueue* transitionSwapchainQueue = nullptr;
        {
            std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
            transitionSwapchainQueue = dx12_hook_g_SwapchainQueue;
        }
        const bool transitionRecoveringPostFSRNonFG = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
            dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG, currentFGActive, currentSLFGRunning,
            transitionSwapchainQueue != nullptr);
        const bool transitionStartupBypassActive = startupOverlayCompatibilityActive && !currentFGActive;
        s_transitionState = ce::dx12_fg_transition::Reduce(
            s_transitionState, {
                                   .runtimeMode = currentRuntimeMode,
                                   .effectiveFGActive = currentFGActive,
                                   .streamlineFGRunning = currentSLFGRunning,
                                   .streamlineLoaded = g_FGCompat.HasStreamlineSupport(),
                                   .runtimeOwnsSwapchain = dx12_hook_g_FGRuntimeOwnsSwapchain,
                                   .hadFSRPhase = dx12_hook_g_HadFSRFGPhase,
                                   .recoveringPostFSRNonFG = transitionRecoveringPostFSRNonFG,
                                   .startupBypassActive = transitionStartupBypassActive,
                                   .overlaySuppressed = !allowOverlayRender,
                               });

        // Detect FG on/off changes AND FG type changes (e.g., FSR FG → DLSS FG)
        // Also detect SL FG signal changes (immediate from SL hook)
        bool fgChanged = (currentFGActive != s_lastFGActive);
        bool runtimeModeChanged = (currentRuntimeMode != s_lastRuntimeMode);
        bool slSignalChanged = (currentSLFGRunning != s_lastSLFGRunning);

        if (fgChanged || runtimeModeChanged || slSignalChanged) {
            auto s_lastRuntimeMode_saved = s_lastRuntimeMode;  // save before update for syncInit logic
            const bool previousWasFG = ce::fg_runtime::IsActualGeneratedFrameMode(s_lastRuntimeMode_saved);
            const bool targetIsFGOff = !ce::fg_runtime::IsActualGeneratedFrameMode(currentRuntimeMode);
            ID3D12CommandQueue* transitionOverlayBackendQueue =
                dx12_hook_g_OverlayAdapterBackendQueue.load(std::memory_order_acquire);
            const bool liveNoCallbackNativeFSRSuspensionToggle =
                !slSignalChanged &&
                ce::dx12_overlay_policy::IsLiveNoCallbackNativeFSRSuspensionToggle(
                    s_lastRuntimeMode_saved, currentRuntimeMode, currentSLFGRunning,
                    dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire), dx12_hook_g_FGRuntimeOwnsSwapchain,
                    transitionSwapchainQueue != nullptr, dx12_hook_g_State.overlayInit,
                    transitionSwapchainQueue != nullptr && transitionOverlayBackendQueue == transitionSwapchainQueue);
            ID3D12CommandQueue* postNativeFSROffRecoveryQueue =
                dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue.load(std::memory_order_acquire);
            const bool gameSwapchainRecoveryToggleAfterNativeFSROff =
                !slSignalChanged &&
                ce::dx12_overlay_policy::IsGameSwapchainRecoveryToggleAfterNativeFSROff(
                    s_lastRuntimeMode_saved, currentRuntimeMode, currentSLFGRunning,
                    transitionSwapchainQueue != nullptr && postNativeFSROffRecoveryQueue == transitionSwapchainQueue);
            const bool heuristicOnlyRuntimeModeFlip =
                !slSignalChanged &&
                ce::dx12_overlay_policy::IsHeuristicOnlyRuntimeModeFlip(
                    s_lastSLFGRunning, currentSLFGRunning, dx12_hook_g_FGRuntimeOwnsSwapchain, g_FGCompat.IsFSRFGApiActive(),
                    transitionSwapchainQueue != nullptr, dx12_hook_g_State.overlayInit,
                    transitionSwapchainQueue != nullptr && transitionOverlayBackendQueue == transitionSwapchainQueue);
            const bool shouldStartFGTransitionCooldown =
                ce::dx12_overlay_policy::ShouldStartFrameGenerationTransitionCooldown(
                    s_lastRuntimeMode_saved, currentRuntimeMode, s_lastFGActive, currentFGActive, s_lastSLFGRunning,
                    currentSLFGRunning,
                    liveNoCallbackNativeFSRSuspensionToggle || gameSwapchainRecoveryToggleAfterNativeFSROff ||
                        heuristicOnlyRuntimeModeFlip);
            if (!shouldStartFGTransitionCooldown) {
                HookLogImportant(
                    "DX12: Runtime state changed without generated-frame transition "
                    "(prev_mode=%s next_mode=%s active=%d->%d sl_signal=%d->%d "
                    "liveNoCallbackNativeFSRSuspensionToggle=%d gameSwapchainRecoveryToggle=%d "
                    "heuristicOnlyFlip=%d backendQ=%p "
                    "scQueue=%p recoveryQ=%p) — overlay remains live",
                    ce::fg_runtime::GetRuntimeModeName(s_lastRuntimeMode_saved),
                    ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), s_lastFGActive ? 1 : 0,
                    currentFGActive ? 1 : 0, s_lastSLFGRunning ? 1 : 0, currentSLFGRunning ? 1 : 0,
                    liveNoCallbackNativeFSRSuspensionToggle ? 1 : 0,
                    gameSwapchainRecoveryToggleAfterNativeFSROff ? 1 : 0, heuristicOnlyRuntimeModeFlip ? 1 : 0,
                    transitionOverlayBackendQueue, transitionSwapchainQueue, postNativeFSROffRecoveryQueue);
                LogOverlayCoverageSummary("FG transition edge (overlay remains live)");
                s_lastFGActive = currentFGActive;
                s_lastRuntimeMode = currentRuntimeMode;
                s_lastSLFGRunning = currentSLFGRunning;
            } else {
                const ID3D12CommandQueue* currentPrimaryQueue = dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
                const ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
                const bool commandQueueSettledToPrimary =
                    currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue;
                const int transitionCooldownFrames = ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(
                                                         commandQueueSettledToPrimary, dx12_hook_g_HadFSRFGPhase,
                                                         previousWasFG && targetIsFGOff && !currentSLFGRunning)
                                                         ? 15
                                                         : 60;
                HookLogImportant(
                    "DX12: FG transition prev_mode=%s next_mode=%s phase=%d ownership=%d render_mode=%d callback=%d "
                    "publish_active=%d sl_signal=%d->%d cooldown=%d epoch=%u",
                    ce::fg_runtime::GetRuntimeModeName(s_lastRuntimeMode),
                    ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode),
                    static_cast<int>(s_transitionState.snapshot.phase),
                    static_cast<int>(s_transitionState.snapshot.ownership),
                    static_cast<int>(s_transitionState.snapshot.renderMode),
                    s_transitionState.snapshot.shouldInstallPostSLCallback ? 1 : 0,
                    s_transitionState.snapshot.publishFGActive ? 1 : 0, s_lastSLFGRunning ? 1 : 0,
                    currentSLFGRunning ? 1 : 0, transitionCooldownFrames, s_transitionState.snapshot.epoch);
                LogOverlayCoverageSummary("FG transition edge");
                s_lastFGActive = currentFGActive;
                s_lastRuntimeMode = currentRuntimeMode;
                s_lastSLFGRunning = currentSLFGRunning;
                dx12_hook_g_FGTransitionCooldown = ce::dx12_overlay_policy::ResolveTransitionCooldownFrames(
                    dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), transitionCooldownFrames,
                    ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(
                        commandQueueSettledToPrimary, dx12_hook_g_HadFSRFGPhase,
                        previousWasFG && targetIsFGOff && !currentSLFGRunning));
                dx12_hook_g_PostSLCooldownRemaining.store(dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire),
                                                std::memory_order_release);
                dx12_hook_g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);

                // Immediately disable post-SL rendering during FG transitions.
                // Keep the callback installed when Streamline is still running so
                // synthetic startup presents can route through PostSL safely while
                // the active gate and cooldown still suppress real rendering.
                // Make-before-break: the explicit-OFF keep-alive owns the PostSL
                // path until the normal route recovers — do not disable it here.
                const bool innerKeepConfirmedPostSLAliveAcrossOff =
                    !currentSLFGRunning && dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire);
                if (!innerKeepConfirmedPostSLAliveAcrossOff) {
                    dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
                    if (!DXGIShared::ShouldKeepPostSLCallbackInstalledDuringTransition(currentSLFGRunning)) {
                        SetPostSLCallbackInstalled(false, "DX12: inner FG transition");
                    }
                }
                dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);      // Fresh start after transition
                dx12_hook_g_PostSLStableFrameCount.store(0, std::memory_order_release);  // Reset warmup counter
                dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);

                // When SL FG turns ON or OFF, clear any false heuristic FSR FG state.
                // SL's queue changes trigger the queue-change heuristic, causing
                // false FSR FG detection.  Clear it during ANY SL FG transition:
                //   - ON: SL creates new queues → queue-change heuristic fires falsely
                //   - OFF: SL's queue was "current FG queue" → not cleared until
                //          consecutive initial-queue frames pass the threshold
                if (g_FGCompat.IsHeuristicFSRFGActive()) {
                    g_FGCompat.SetHeuristicFSRFGActive(false);
                    HookLogImportant("DX12: Cleared heuristic FSR FG during SL FG %s transition",
                                     currentSLFGRunning ? "ON" : "OFF");
                }

                // Reset the queue-change heuristic's internal state so it re-captures
                // the "initial queue" after the transition.  SL's leftover queue would
                // otherwise persist as s_initialQueue/s_currentFGQueue and immediately
                // re-trigger false FSR FG detection.
                RequestFGDetectionHeuristicReset();

                // Drain in-flight overlay GPU work on ANY FG transition.
                // When FG activates (especially FSR FG), it may use the same queue
                // our overlay was rendering on.  In-flight overlay ECLs on that queue
                // can cause FSR's internal synchronization to deadlock (spin-wait in
                // ffxQuery).  Drain ensures the queue is clean before FG takes over.
                //
                // Original: only drained on SL OFF.  Extended to all transitions
                // because FSR FG also needs a clean queue at activation.
                if (dx12_hook_g_State.fence && dx12_hook_g_State.currentFenceValue > 0) {
                    UINT64 lastVal = dx12_hook_g_State.currentFenceValue;
                    HANDLE drainEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                    if (drainEvent) {
                        HRESULT drainHr = dx12_hook_g_State.fence->SetEventOnCompletion(lastVal, drainEvent);
                        if (SUCCEEDED(drainHr)) {
                            DWORD waitResult = WaitForSingleObject(drainEvent, 200);
                            HookLogImportant(
                                "DX12: FG transition — drained overlay GPU work (fenceVal=%llu wait=%u "
                                "slSignalChanged=%d "
                                "fgChanged=%d)",
                                (unsigned long long)lastVal, waitResult, slSignalChanged ? 1 : 0, fgChanged ? 1 : 0);
                        } else {
                            HookLogImportant("DX12: FG transition — fence drain failed hr=0x%08X", drainHr);
                        }
                        CloseHandle(drainEvent);
                    }
                }

                ClearPostSLQueues("DX12: FG transition queue reset");
                // Keep the SL wrapper queue alive while Streamline still owns the
                // presentation path, even if FG is temporarily idle.
                {
                    bool targetUsesStreamline = ce::fg_runtime::RuntimeModeUsesStreamline(currentRuntimeMode);
                    if (!targetUsesStreamline) {
                        ID3D12CommandQueue* oldWrapper = dx12_hook_g_SLWrapperQueue.exchange(nullptr, std::memory_order_acq_rel);
                        if (oldWrapper) {
                            oldWrapper->Release();
                            HookLogImportant("DX12: runtime=%s — released SL wrapper queue %p",
                                             ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), oldWrapper);
                        }
                    } else {
                        ID3D12CommandQueue* kept = dx12_hook_g_SLWrapperQueue.load(std::memory_order_acquire);
                        HookLogImportant("DX12: runtime=%s — keeping SL wrapper queue %p alive",
                                         ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), kept);
                    }
                }
                if (!innerKeepConfirmedPostSLAliveAcrossOff) {
                    dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);  // Re-probe needed
                }
                if (!ce::dx12_overlay_policy::ShouldPreservePostSLLastWorkingQueueForPostFSROffRecovery(
                        dx12_hook_g_HadFSRFGPhase, previousWasFG, targetIsFGOff)) {
                    // Old SL queues may be destroyed after most FG mode switches
                    // (e.g., DLSS FG phase 1 -> FSR FG -> DLSS FG phase 2). Keep the
                    // last validated queue only for the immediate post-FSR FG-off
                    // recovery window, where it is the only queue that already proved
                    // safe for the live swapchain.
                    SetPostSLLastWorkingQueue(nullptr);
                } else {
                    HookLogImportant(
                        "DX12: Preserving PostSL lastWorkingQueue %p for immediate post-FSR FG-off recovery",
                        dx12_hook_g_PostSLLastWorkingQueue);
                }

                // Save the current ProcessFrame gameQueue as a pre-FG snapshot.
                // When PostSL activates after the cooldown, g_CommandQueue may have
                // been polluted by SL's internal queues.  gameQueue (resolved at the
                // top of ProcessFrame from scQueue or cmdQueue) is still the game's
                // real queue at this point.
                if (dx12_hook_g_PreFGGameQueue)
                    dx12_hook_g_PreFGGameQueue->Release();
                dx12_hook_g_PreFGGameQueue = gameQueue;
                if (gameQueue)
                    gameQueue->AddRef();

                // Force sync resources re-initialization on next overlay render.
                // After FG type transitions (e.g., FSR→DLSS), the sync resources
                // (allocators, fence, cmdList) were used on a different queue during
                // the previous FG phase.  Re-using them on a new queue after swapchain
                // recreation causes DEVICE_REMOVED.  Fresh resources avoid this.
                //
                // EXCEPTION 1: FG→off transitions do NOT invalidate sync.  There is no
                // swapchain recreation when FG simply turns off, and the allocators/
                // fence are device-level objects that work on any DIRECT queue.  The
                // GPU drain above ensures all in-flight work completes.
                //
                // EXCEPTION 2: off→on transitions (None→FSR_FG or None→DLSS_FG) also
                // do NOT invalidate sync.  The existing resources are device-level and
                // work on any DIRECT queue.  Forcing re-init here is unnecessary and
                // causes the "FG→FG" misclassification for what is really "off→on".
                bool actualFGToFG = previousWasFG && !targetIsFGOff;
                if (dx12_hook_g_State.syncInit && actualFGToFG) {
                    HookLogImportant("DX12: FG transition (FG→FG: %s→%s) — forcing syncInit=false for fresh resources",
                                     ce::fg_runtime::GetRuntimeModeName(s_lastRuntimeMode_saved),
                                     ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode));
                    dx12_hook_g_State.syncInit = false;
                } else if (dx12_hook_g_State.syncInit && targetIsFGOff) {
                    HookLogImportant("DX12: FG→off transition — keeping syncInit=true (reusing existing resources)");
                } else if (dx12_hook_g_State.syncInit && !previousWasFG && !targetIsFGOff) {
                    HookLogImportant(
                        "DX12: FG off→on transition (%s→%s) — keeping syncInit=true (resources work on any queue)",
                        ce::fg_runtime::GetRuntimeModeName(s_lastRuntimeMode_saved),
                        ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode));
                }

                // Clear stale swapchain queue only when transitioning TO SL-based FG
                // if we've never had an FSR FG phase. If FSR FG already ran, SL might
                // reuse FSR's swapchain (no new CreateSwapChainForHwnd), so scQueue is
                // the CORRECT queue for backbuffer access. Keep it alive via AddRef.
                if (runtimeModeChanged) {
                    bool newTypeNeedsScQueue = (currentRuntimeMode == ce::fg_runtime::RuntimeMode::kFSRFG);
                    bool targetIsNone = !ce::fg_runtime::IsActualGeneratedFrameMode(currentRuntimeMode);
                    if (targetIsNone && !dx12_hook_g_HadFSRFGPhase) {
                        // FG→off: keep g_SwapchainQueue as-is.  Same rationale as
                        // the outer slTurnedOff handler: SL's swapchain may persist
                        // after FG teardown, so g_SwapchainQueue (set by the
                        // CreateSwapChainForHwnd hook) already points to the correct
                        // queue.  Restoring to origGame causes queue/swapchain
                        // mismatch → DXGI_ERROR_ACCESS_DENIED.
                        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                        // FG is off — FG runtime no longer owns the queue
                        if (dx12_hook_g_FGRuntimeOwnsSwapchain) {
                            dx12_hook_g_FGRuntimeOwnsSwapchain = false;
                            DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                            dx12_hook_g_FGRuntimeOwnsSwapchainSince = 0;
                            ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
                            ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
                            HookLogImportant("DX12: FG→off — clearing FG runtime ownership of swapchain queue");
                        }
                        HookLogImportant("DX12: FG→off — keeping g_SwapchainQueue %p (origGame=%p)", dx12_hook_g_SwapchainQueue,
                                         dx12_hook_g_OriginalGameQueue);
                        if (!dx12_hook_g_SwapchainQueue && dx12_hook_g_OriginalGameQueue) {
                            // Swapchain queue not captured yet — fall back to origGame
                            dx12_hook_g_OriginalGameQueue->AddRef();
                            dx12_hook_g_SwapchainQueue = dx12_hook_g_OriginalGameQueue;
                            dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
                            HookLogImportant("DX12: FG→off — scQueue was null, falling back to origGame %p",
                                             dx12_hook_g_OriginalGameQueue);
                        }
                    } else if (!newTypeNeedsScQueue && !dx12_hook_g_HadFSRFGPhase) {
                        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                        // Clear FG runtime ownership when transitioning away from FSR FG
                        if (dx12_hook_g_FGRuntimeOwnsSwapchain && targetIsNone) {
                            dx12_hook_g_FGRuntimeOwnsSwapchain = false;
                            DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                            dx12_hook_g_FGRuntimeOwnsSwapchainSince = 0;
                            ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
                            ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
                            HookLogImportant("DX12: FG type change to None — clearing FG runtime ownership");
                        }
                        if (dx12_hook_g_SwapchainQueue) {
                            // Protect recently-captured scQueue from phantom FG detections.
                            // After swapchain recreation, the heuristic may briefly detect
                            // NVIDIA_SM (false positive from Present rate measurement).
                            // Clearing scQueue in that window causes ProcessFrame to fall
                            // back to origGame, which FSR FG uses internally → deadlock.
                            ULONGLONG age = GetTickCount64() - dx12_hook_g_SwapchainQueueCaptureTime;
                            if (age < 5000) {
                                HookLogImportant(
                                    "DX12: FG type change to %s — PRESERVING g_SwapchainQueue %p (captured %llu ms "
                                    "ago, "
                                    "too recent to clear)",
                                    ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), dx12_hook_g_SwapchainQueue, age);
                            } else {
                                HookLogImportant(
                                    "DX12: FG type change to %s — clearing stale g_SwapchainQueue %p (no FSR history, "
                                    "age=%llu ms)",
                                    ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), dx12_hook_g_SwapchainQueue, age);
                                dx12_hook_g_SwapchainQueue->Release();
                                dx12_hook_g_SwapchainQueue = nullptr;
                                dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
                            }
                        }
                    } else if (!newTypeNeedsScQueue && dx12_hook_g_HadFSRFGPhase) {
                        if (targetIsNone) {
                            // FSR→DLSS→Off: wait for live non-FG command traffic to
                            // prove which queue owns the resumed Present path again.
                            // Forcing origGame back into g_SwapchainQueue here caused
                            // Talos to submit the first recovered non-FG overlay ECL
                            // on the wrong queue/backbuffer pairing, immediately
                            // triggering DEVICE_REMOVED.
                            std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                            const bool preserveRuntimeOwnedFSRTeardown =
                                ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(
                                    targetIsNone, dx12_hook_g_HadFSRFGPhase, dx12_hook_g_FGRuntimeOwnsSwapchain,
                                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
                            if (preserveRuntimeOwnedFSRTeardown) {
                                HookLogImportant(
                                    "DX12: FG→off after FSR phase classified while runtime still owns swapchain — "
                                    "preserving FSR queue ownership until a stronger off signal appears "
                                    "(scQ=%p origGame=%p primary=%p cmdQ=%p)",
                                    dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue,
                                    dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire),
                                    g_CommandQueue.load(std::memory_order_acquire));
                                return;
                            }
                            if (dx12_hook_g_FGRuntimeOwnsSwapchain) {
                                dx12_hook_g_FGRuntimeOwnsSwapchain = false;
                                DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false,
                                                                                       std::memory_order_release);
                                dx12_hook_g_FGRuntimeOwnsSwapchainSince = 0;
                                ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
                                ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
                                const bool preserveAuthoritativeFSRDuringTransition =
                                    ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(
                                        g_FGCompat.IsFSRFGApiActive(), targetIsNone,
                                        dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire));
                                if (preserveAuthoritativeFSRDuringTransition) {
                                    HookLogImportant(
                                        "DX12: FG→off after FSR phase detected during active transition cooldown — "
                                        "preserving authoritative FSR state until queue topology settles (cooldown=%d "
                                        "scQ=%p origGame=%p primary=%p cmdQ=%p)",
                                        dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), dx12_hook_g_SwapchainQueue,
                                        dx12_hook_g_OriginalGameQueue, dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire),
                                        g_CommandQueue.load(std::memory_order_acquire));
                                } else if (g_FGCompat.IsFSRFGApiActive()) {
                                    SetNativeFSRStartupConfigureArmingPending(false,
                                                                              "runtime mode transition cleared FSR");
                                    ClearOfficialFFXRuntimeOwnedPresentPathAssumption(
                                        "runtime mode transition cleared FSR");
                                    g_FGCompat.SetFSRFGActive(false);
                                    g_FGCompat.SetFSRFGMultiplier(0);
                                }
                                HookLogImportant(
                                    "DX12: FG→off after FSR phase — clearing FG runtime ownership of swapchain queue");
                            }
                            if (dx12_hook_g_SwapchainQueue && dx12_hook_g_SwapchainQueue != dx12_hook_g_OriginalGameQueue) {
                                HookLogImportant(
                                    "DX12: FG→off after FSR phase — releasing stale g_SwapchainQueue %p and waiting "
                                    "for "
                                    "the live non-FG queue to be recaptured (origGame=%p primary=%p cmdQ=%p)",
                                    dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue,
                                    dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire),
                                    g_CommandQueue.load(std::memory_order_acquire));
                                dx12_hook_g_SwapchainQueue->Release();
                                dx12_hook_g_SwapchainQueue = nullptr;
                                dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
                                dx12_hook_g_SwapchainQueueCaptureTime = 0;
                            }
                            if (!dx12_hook_g_SwapchainQueue) {
                                HookLogImportant(
                                    "DX12: FG→off after FSR phase — keeping g_SwapchainQueue null until non-wrapper "
                                    "command traffic settles (origGame=%p primary=%p cmdQ=%p)",
                                    dx12_hook_g_OriginalGameQueue, dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire),
                                    g_CommandQueue.load(std::memory_order_acquire));
                            }
                        } else {
                            // FSR→DLSS transition: the swapchain was created on FSR's queue
                            // (g_SwapchainQueue), so backbuffers belong to it. Keep it alive.
                            // Render pre-SL on scQueue — the swapchain's own queue has
                            // authorized access to backbuffers without cross-queue issues.
                            HookLogImportant(
                                "DX12: FG type change to %s — KEEPING g_SwapchainQueue %p for backbuffer access (it's "
                                "the "
                                "swapchain creation queue)",
                                ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), dx12_hook_g_SwapchainQueue);
                            dx12_hook_g_NeedGPUDrainBeforeRender = false;
                        }
                    } else {
                        HookLogImportant("DX12: FG type change to %s — keeping g_SwapchainQueue %p (FSR needs it)",
                                         ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), dx12_hook_g_SwapchainQueue);
                    }
                }

                // Probe real D3D12 ECL when SL FG first activates.  Must happen
                // before the first overlay ECL submission so we have the bypass
                // ready.
                if (currentFGActive && IsStreamlineLoaded()) {
                    auto* dev = g_Device.load(std::memory_order_acquire);
                    if (dev)
                        ProbeRealD3D12ECL(dev);

                }

                // Flush any pending deferred signal immediately so GPU work from the
                // previous frame completes before SL reconfigures.
                UINT64 deferredVal = dx12_hook_g_deferredSignalValue.load(std::memory_order_acquire);
                if (deferredVal != 0 && dx12_hook_g_State.fence) {
                    ID3D12CommandQueue* sigQueue = dx12_hook_g_deferredSignalQueue.load(std::memory_order_acquire);
                    if (!sigQueue)
                        sigQueue = gameQueue;
                    if (sigQueue) {
                        HRESULT hr = sigQueue->Signal(dx12_hook_g_State.fence, deferredVal);
                        if (SUCCEEDED(hr)) {
                            int allocIdx = dx12_hook_g_deferredSignalAllocIdx.load(std::memory_order_acquire);
                            dx12_hook_g_State.currentFenceValue = deferredVal;
                            if (allocIdx >= 0 && allocIdx < (int)dx12_hook_g_State.fenceValues.size())
                                dx12_hook_g_State.fenceValues[allocIdx] = deferredVal;
                        }
                    }
                    dx12_hook_g_deferredSignalValue.store(0, std::memory_order_release);
                    dx12_hook_g_deferredSignalAllocIdx.store(-1, std::memory_order_release);
                    dx12_hook_g_deferredSignalQueue.store(nullptr, std::memory_order_release);
                }
            }
        }
        bool skipOverlayDraw = false;
        if (holdFocusLossBackbufferWork) {
            skipOverlayDraw = true;
            NoteDX12OverlayCoverageGate("focus-loss-hold");
        }
        if (dx12_hook_g_FGTransitionCooldown > 0) {
            // PRINCIPLE: never blank a live overlay. When the backend is live and
            // the normal route is its transport (OFF / no-callback FSR), the
            // transition only retargets the submit queue (auto-resolved per
            // frame on device-level sync resources) — the draw cooldown is pure
            // gratuitous suppression, so keep drawing this very frame.
            const bool appCallbackBridgeFSRActive =
                g_FGCompat.IsFSRFGApiActive() &&
                !dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire);
            const bool keepDrawingLiveOverlayThroughCooldown =
                ce::dx12_overlay_policy::ShouldKeepDrawingLiveOverlayThroughFGTransitionCooldown(
                    dx12_hook_g_State.overlayInit, dx12_hook_g_State.syncInit, protectedOfficialFFXStartupOverlayOnly, currentSLFGRunning,
                    appCallbackBridgeFSRActive);
            if (keepDrawingLiveOverlayThroughCooldown) {
                static std::atomic<int> s_keepDrawingLiveOverlayLogCount{0};
                const int logCount = s_keepDrawingLiveOverlayLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: Keeping live overlay drawing through FG transition cooldown — no blank "
                        "(oldCooldown=%d queue=%p scQueue=%p fsrApi=%d noCallback=%d log=%d)",
                        dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), gameQueue, transitionSwapchainQueue,
                        g_FGCompat.IsFSRFGApiActive() ? 1 : 0,
                        dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire) ? 1 : 0, logCount + 1);
                }
                // The transition bookkeeping (GPU drain, queue/heuristic resets)
                // already ran in the arming block; only the multi-frame draw
                // suppression is removed. PostSL is not the transport here, so
                // leave its state untouched.
                dx12_hook_g_FGTransitionCooldown.store(0, std::memory_order_release);
                // A stale scene-transition cooldown (e.g. armed during the prior FSR
                // phase) must never blank the live overlay C1 just decided to keep
                // drawing — clearing it here is the safety net for the phantom-arming
                // path (session 20260613_202646, 14-present FSR->OFF blank).
                if (dx12_hook_g_SceneTransitionCooldown.load(std::memory_order_acquire) > 0) {
                    dx12_hook_g_SceneTransitionCooldown.store(0, std::memory_order_release);
                    HookLogImportant(
                        "DX12: Cleared stale scene-transition cooldown at FG transition keep-drawing edge — "
                        "live overlay is never blanked");
                }
                NoteDX12OverlayCoverageGate("live-overlay-kept-drawing");
            } else {
                dx12_hook_g_FGTransitionCooldown.fetch_sub(1, std::memory_order_acq_rel);
                const bool preserveActivePostSLDuringCooldown =
                    ce::dx12_overlay_policy::ShouldPreserveActivePostSLDuringFGCooldown(
                        currentSLFGRunning, dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                        HookIsPostSLOverlayActiveButUnconfirmed());
                if (preserveActivePostSLDuringCooldown) {
                    // Synthetic startup can already have an active PostSL path before
                    // the game-thread cooldown has fully counted down. Do not let the
                    // slower ProcessFrame cooldown re-disable that same path and force
                    // a second reactivation/warm-up epoch before first confirmation.
                    dx12_hook_g_PostSLOverlayActive.store(true, std::memory_order_release);
                    dx12_hook_g_PostSLCooldownRemaining.store(0, std::memory_order_release);

                    static int s_preserveActivePostSLLog = 0;
                    if (s_preserveActivePostSLLog < 10 || dx12_hook_g_FGTransitionCooldown == 0) {
                        HookLogImportant(
                            "DX12: FG cooldown preserving active PostSL path "
                            "(remaining=%d slSignal=%d confirmed=%d unconfirmed=%d)",
                            dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), currentSLFGRunning ? 1 : 0,
                            dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_relaxed) ? 1 : 0,
                            HookIsPostSLOverlayActiveButUnconfirmed() ? 1 : 0);
                    }
                    s_preserveActivePostSLLog++;
                } else {
                    // During cooldown, suppress BOTH pre-SL and post-SL rendering.
                    dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
                    dx12_hook_g_PostSLCooldownRemaining.store(dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire),
                                                    std::memory_order_release);
                }

                // Periodic device-removed check during cooldown to pinpoint
                // when the device dies (overlay is NOT rendering during this time).
                if ((dx12_hook_g_FGTransitionCooldown % 10) == 0) {
                    auto* cooldownDev = g_Device.load(std::memory_order_acquire);
                    if (cooldownDev) {
                        HRESULT cooldownDevHr = cooldownDev->GetDeviceRemovedReason();
                        if (FAILED(cooldownDevHr)) {
                            HookLogImportant(
                                "DX12: DEVICE REMOVED DURING COOLDOWN (cooldown=%d devRemoved=0x%08X tid=0x%04X)",
                                dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), (unsigned)cooldownDevHr,
                                GetCurrentThreadId());
                        }
                    }
                }

                NoteDX12OverlayCoverageGate("fg-transition-cooldown");
                if (dx12_hook_g_FGTransitionCooldown == 0) {
                    auto fgType = g_FGCompat.GetActiveFGType();
                    bool slFG = currentFGActive && DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
                    HookLogImportant(
                        "DX12: FG transition cooldown complete — resuming overlay (slFG=%d, fgType=%s, slSignal=%d)",
                        slFG ? 1 : 0, g_FGCompat.GetFGTypeName(fgType),
                        DXGIShared::g_StreamlineFGRunning.load() ? 1 : 0);
                    // Re-enable post-SL rendering if SL FG is active
                    if (slFG) {
                        const bool preserveSyntheticStartupState =
                            ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(
                                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                                    std::memory_order_acquire),
                                HookIsPostSLOverlayActiveButUnconfirmed(),
                                dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                                HookIsPostSLOverlayConfirmedButStartupSettling());
                        const bool keepStartupHandoffPending = ce::dx12_overlay_policy::
                            ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(
                                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                                    std::memory_order_acquire),
                                HookIsPostSLOverlayActiveButUnconfirmed(),
                                dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                                HookIsPostSLOverlayConfirmedButStartupSettling());
                        dx12_hook_g_PostSLOverlayActive.store(true, std::memory_order_release);
                        if (!preserveSyntheticStartupState) {
                            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(
                                false, std::memory_order_release);
                            dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
                            DXGIShared::ResetStreamlineStartupTransitionState();
                        }
                        dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
                        DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(!keepStartupHandoffPending,
                                                                                        std::memory_order_release);
                    }
                }
                skipOverlayDraw = true;
            }
        }

        // POST-SL overlay rendering during SL FG:
        //
        // Why post-SL: Pre-SL rendering submits ECLs on the game queue before SL
        // processes Present.  This crashes at ~600-770 frames because the extra ECL
        // perturbs SL's frame generation pipeline (confirmed by binary search: empty
        // ECL=no crash, drawing ECL=crash, regardless of barriers/fences).
        //
        // Post-SL rendering submits the ECL in the re-entrant Present callback —
        // after SL's FG work but before the real Present flip.  Post-SL empty ECL
        // was proven stable (∞ frames).  The overlay draws to the backbuffer that SL
        // is about to present, using implicit state promotion (no explicit barriers).
        // Real D3D12 ECL bypasses all hooks.  No fence signal.
        // SL captures our overlay as part of the scene, FG interpolates it naturally.
        // Overlay appears on all output frames (real + interpolated).
        const bool slFGActive = currentFGActive && DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        // x86 text is emitted as solid glyph spans by the DX12 backend.  That
        // keeps no-FG rendering on the fast native direct path; the independent
        // post-FSR offscreen path below remains only for the existing FG handoff
        // state where backbuffer ownership/state can be indeterminate.
        {
            if (slFGActive) {
                // SL FG active: enable POST-SL overlay rendering.
                // The overlay ECL is submitted in the re-entrant Present callback
                // AFTER SL's FG processing but BEFORE the real Present call.
                // This avoids submitting extra ECLs before SL processes the frame
                // (which caused crashes at ~600-770 frames with pre-SL rendering).
                //
                // EXCEPTION: After FSR→DLSS transition, PostSL rendering causes
                // DEVICE_HUNG because the backbuffer resource state is invalid from
                // any queue we have (FSR created the swapchain, SL resized it).
                // In this case, keep rendering pre-SL: the overlay is rendered
                // BEFORE SL's FG pipeline processes the frame, so origGame's state
                // tracking is still valid (game just finished rendering on it).

                // CRITICAL FIX: PostSL rendering now works for pure DLSS FG
                // (no prior FSR) because IsRecursivePresent() correctly treats
                // SL's cross-thread FG Presents as re-entrant.
                //
                // For post-FSR DLSS FG, we use pre-SL rendering instead.
                // PostSL has irreconcilable cross-queue issues: SL's FG pipeline
                // uses its own internal queue, but the swapchain was created by
                // FSR on scQueue.  All queue options (scQueue, origGame, SL
                // wrapper) cause DEVICE_HUNG from cross-queue backbuffer conflicts.
                // POST-SL overlay for ALL SL FG modes (pure DLSS and post-FSR DLSS).
                //
                // Uses origGame queue: SL routes everything through origGame
                // (via its COM wrapper).  PostSL fires in the re-entrant Present
                // path AFTER SL's FG processing is complete.  The backbuffer is
                // in PRESENT state on origGame — same as pure DLSS.
                //
                // Previously we disabled PostSL for post-FSR and used pre-SL,
                // but SL intercepts the game thread's Present at the COM wrapper
                // level during DLSS FG — only SL's worker threads reach our
                // detour.  PostSL (in the re-entrant path) is the only reliable
                // rendering timing for DLSS FG.
                {
                    // Step 1: Register callback (idempotent)
                    if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) !=
                        &PostSLOverlayRenderGated) {
                        SetPostSLCallbackInstalled(true, "DX12: SL FG active");
                        HookLogImportant("DX12: SL FG active - registered POST-SL overlay callback (hadFSR=%d)",
                                         dx12_hook_g_HadFSRFGPhase ? 1 : 0);
                    }

                    // Step 2: Activate PostSL rendering
                    if (!skipOverlayDraw) {
                        if (!dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire)) {
                            const bool preserveSyntheticStartupState =
                                ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(
                                    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                                        std::memory_order_acquire),
                                    HookIsPostSLOverlayActiveButUnconfirmed(),
                                    dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                                    HookIsPostSLOverlayConfirmedButStartupSettling());
                            const bool keepStartupHandoffPending = ce::dx12_overlay_policy::
                                ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(
                                    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                                        std::memory_order_acquire),
                                    HookIsPostSLOverlayActiveButUnconfirmed(),
                                    dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                                    HookIsPostSLOverlayConfirmedButStartupSettling());
                            dx12_hook_g_PostSLOverlayActive.store(true, std::memory_order_release);
                            if (!preserveSyntheticStartupState) {
                                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(
                                    false, std::memory_order_release);
                                dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
                            }
                            dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
                            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(!keepStartupHandoffPending,
                                                                                            std::memory_order_release);
                            if (!preserveSyntheticStartupState) {
                                DXGIShared::ResetStreamlineStartupTransitionState();
                            }
                            HookLogImportant("DX12: SL FG active - activated POST-SL overlay rendering (hadFSR=%d)",
                                             dx12_hook_g_HadFSRFGPhase ? 1 : 0);
                        }
                    } else {
                        const bool preserveActivePostSL =
                            ce::dx12_overlay_policy::ShouldPreserveActivePostSLWhenPreSLDrawIsSkipped(
                                currentSLFGRunning, dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                                HookIsPostSLOverlayActiveButUnconfirmed());
                        if (preserveActivePostSL) {
                            static int s_preservePostSLOnSkippedPreSLDrawLog = 0;
                            if (s_preservePostSLOnSkippedPreSLDrawLog < 10 ||
                                (s_preservePostSLOnSkippedPreSLDrawLog % 200) == 0) {
                                HookLogImportant(
                                    "DX12: Preserving active PostSL while pre-SL draw is skipped "
                                    "(confirmed=%d unconfirmed=%d skip=%d)",
                                    dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_relaxed) ? 1 : 0,
                                    HookIsPostSLOverlayActiveButUnconfirmed() ? 1 : 0,
                                    s_preservePostSLOnSkippedPreSLDrawLog + 1);
                            }
                            s_preservePostSLOnSkippedPreSLDrawLog++;
                        } else if (dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire)) {
                            dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
                        }
                    }
                    ExecuteCommandListsPtr currentRealECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
                    ExecuteCommandListsPtr selectedQueueOrigECL = GetOriginalExecuteCommandLists(gameQueue);
                    const bool keepPostSLWithoutRealECL =
                        ce::dx12_overlay_policy::ShouldKeepPostSLActiveWhenRealECLUnavailable(
                            currentRealECL != nullptr, selectedQueueOrigECL != nullptr,
                            dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
                            HookIsPostSLOverlayActiveButUnconfirmed());
                    if (!keepPostSLWithoutRealECL) {
                        static bool s_noRealECLLogged = false;
                        if (!s_noRealECLLogged) {
                            s_noRealECLLogged = true;
                            HookLogImportant("DX12: No real D3D12 ECL available - disabling overlay during SL FG");
                        }
                        dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
                    } else if (!currentRealECL) {
                        static std::atomic<int> s_keepPostSLWithoutRealECLLogCount{0};
                        const int logCount = s_keepPostSLWithoutRealECLLogCount.fetch_add(1, std::memory_order_relaxed);
                        if (logCount < 10 || (logCount % 240) == 0) {
                            HookLogImportant(
                                "DX12: Keeping PostSL active without realECL "
                                "(queue=%p origECL=%p confirmed=%d unconfirmed=%d log=%d)",
                                gameQueue, (void*)selectedQueueOrigECL,
                                dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_relaxed) ? 1 : 0,
                                HookIsPostSLOverlayActiveButUnconfirmed() ? 1 : 0, logCount + 1);
                        }
                    }
                    if (dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire)) {
                        // FG "SUSPENSION" STALL DETECTION:
                        //
                        // PostSL was previously confirmed rendering, but it may have
                        // stalled.  This happens when:
                        //   1. DLSS FG is "nominally on" (g_StreamlineFGRunning=true,
                        //      slDLSSGSetOptions was NOT called with mode=0)
                        //   2. But SL stops generating re-entrant Present calls
                        //      (game menu, pause, loading screen)
                        //
                        // In this state, BOTH rendering paths are blocked:
                        //   - Pre-SL: suppressed by skipOverlayDraw (PostSL confirmed)
                        //   - PostSL: never fires (no re-entrant Present from SL)
                        //
                        // FIX: Count consecutive Present calls without PostSL firing.
                        // PostSLOverlayRender resets g_PostSLStallCounter to 0 on
                        // each successful render.  After kPostSLStallThreshold frames
                        // without a reset, allow pre-SL as fallback.
                        //
                        // WARMUP GUARD: The stall fallback is ONLY safe when SL's FG
                        // pipeline is genuinely idle (suspension).  During FG warmup
                        // (just after OFF→ON or re-confirmation), SL's pipeline is
                        // actively processing, and pre-SL ECLs on origGame cause
                        // DEVICE_HUNG.  g_PostSLStableFrameCount tracks consecutive
                        // PostSL frames since the last FG transition.  Fallback is
                        // only enabled after kPostSLWarmupThreshold frames of stable
                        // PostSL rendering, proving the FG pipeline is fully operational.
                        //
                        // TESTED: GTA V Enhanced (menu pauses FG), Talos Reawakened
                        // (continuous FG — stall never triggers during normal play).
                        constexpr int kPostSLStallThreshold = 5;
                        const int kPostSLWarmupThreshold =
                            ce::dx12_overlay_policy::GetConfirmedPostSLWarmupProofFrameThreshold();
                        int stableFrames = dx12_hook_g_PostSLStableFrameCount.load(std::memory_order_acquire);
                        int stallCount = dx12_hook_g_PostSLStallCounter.fetch_add(1, std::memory_order_acq_rel) + 1;

                        if (stableFrames < kPostSLWarmupThreshold) {
                            // FG pipeline still warming up — don't fall back to pre-SL.
                            // Just skip rendering until PostSL stabilizes.
                            skipOverlayDraw = true;
                            if (stableFrames > 0 && stallCount > kPostSLStallThreshold &&
                                (stallCount == kPostSLStallThreshold + 1 || (stallCount % 30) == 0)) {
                                const bool serviced = DX12_TryInvokePostSLStartupActivationCallback(
                                    "DX12::PostSL warmup stall service", false, true);
                                static int s_warmupServiceLog = 0;
                                if (serviced || s_warmupServiceLog < 10 || (s_warmupServiceLog % 100) == 0) {
                                    HookLogImportant(
                                        "DX12: PostSL warmup stall service %s "
                                        "(stableFrames=%d stallCount=%d threshold=%d serviceLog=%d)",
                                        serviced ? "rendered via retained callback" : "could not run", stableFrames,
                                        stallCount, kPostSLWarmupThreshold, s_warmupServiceLog + 1);
                                }
                                ++s_warmupServiceLog;
                            }
                            static int s_warmupSuppressLog = 0;
                            ++s_warmupSuppressLog;
                            if (s_warmupSuppressLog <= 5 || (s_warmupSuppressLog % 200) == 0) {
                                HookLogImportant(
                                    "DX12: PostSL warmup — suppressing stall fallback "
                                    "(stableFrames=%d stallCount=%d threshold=%d) #%d",
                                    stableFrames, stallCount, kPostSLWarmupThreshold, s_warmupSuppressLog);
                            }
                        } else if (stallCount <= kPostSLStallThreshold) {
                            skipOverlayDraw = true;  // PostSL recently active — suppress pre-SL
                        } else {
                            // PostSL has stalled — SL FG is nominally on but not generating
                            // frames.  Allow pre-SL rendering as fallback.
                            static int s_stallFallbackLog = 0;
                            if (s_stallFallbackLog < 10 || (s_stallFallbackLog % 200) == 0) {
                                HookLogImportant(
                                    "DX12: PostSL stalled (%d frames, stableFrames=%d) — falling back to pre-SL "
                                    "rendering #%d",
                                    stallCount, stableFrames, s_stallFallbackLog);
                            }
                            s_stallFallbackLog++;
                            // Don't skip pre-SL draw — it will render the overlay
                        }
                    }
                }
            } else {
                // SL FG not active (FSR FG, no FG, suspension, etc.): render pre-SL.
                // Make-before-break: while a CONFIRMED-PostSL suspension keep-alive
                // is active (DLSS SetOptions(off) churn / menu suspend), keep the
                // callback installed and the confirmed flag set. The normal route
                // still draws this frame (the overlay stays visible during the
                // suspension), and the rapid re-ON then takes the warm-resume path
                // instead of a fresh reactivation epoch — eliminating the 1-present
                // PostSL reactivation/probe gap on every SL-signal churn cycle
                // (session 20260613_041204). PostSLOverlayRenderGated retires the
                // keep-alive on genuine teardown (Streamline unload / swapchain
                // invalidation), and SetPostSLCallbackInstalled(false) on any
                // authoritative disable still clears it.
                if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr &&
                    !dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire)) {
                    SetPostSLCallbackInstalled(false, "DX12: pre-SL fallback");
                    dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
                    // Reset PostSL confirmed flag so pre-SL rendering resumes immediately
                    dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
                    dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
                    dx12_hook_g_PostSLStableFrameCount.store(0, std::memory_order_release);
                    dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
                    HookLogImportant("DX12: Disabled post-SL callback — rendering pre-SL in ProcessFrame (fgType=%s)",
                                     g_FGCompat.GetFGTypeName(g_FGCompat.GetActiveFGType()));
                } else if (dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire)) {
                    static std::atomic<int> s_preSLFallbackKeepAliveLogCount{0};
                    const int logCount = s_preSLFallbackKeepAliveLogCount.fetch_add(1, std::memory_order_relaxed);
                    if (logCount < 10 || (logCount % 300) == 0) {
                        HookLogImportant(
                            "DX12: pre-SL fallback rendering normal route during confirmed-PostSL suspension "
                            "keep-alive — callback stays installed for warm re-ON (log=%d)",
                            logCount + 1);
                    }
                }
            }
        }

        // Periodic routing state diagnostic (every 300 frames)
        {
            static uint64_t s_routingFrameCount = 0;
            ++s_routingFrameCount;
            if ((s_routingFrameCount % 300) == 0) {
                HookLogImportant(
                    "DX12: Routing state: frame=%llu fgActive=%d slFGActive=%d slSignal=%d "
                    "cooldown=%d sceneCool=%d postSLCallback=%d postSLActive=%d skip=%d stallCount=%d stableFrames=%d "
                    "runtime=%s",
                    s_routingFrameCount, currentFGActive ? 1 : 0, slFGActive ? 1 : 0, currentSLFGRunning ? 1 : 0,
                    dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire),
                    dx12_hook_g_SceneTransitionCooldown.load(std::memory_order_relaxed),
                    DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr ? 1 : 0,
                    dx12_hook_g_PostSLOverlayActive.load(std::memory_order_relaxed) ? 1 : 0, skipOverlayDraw ? 1 : 0,
                    dx12_hook_g_PostSLStallCounter.load(std::memory_order_relaxed),
                    dx12_hook_g_PostSLStableFrameCount.load(std::memory_order_relaxed),
                    ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode));
            }
        }

        // Scene transition cooldown: detect large frametime gaps (loading screens,
        // scene changes) and skip overlay rendering briefly.  This runs BEFORE
        // the skipOverlayDraw check so it works for both pre-SL (normal) and
        // post-SL (SL FG) overlay paths.
        {
            static LARGE_INTEGER s_lastProcessFrameTime = {};
            static bool s_lastSceneBlockSuppressedRoute = false;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);

            // While the overlay is presented via a runtime-owned / FSR-callback / PostSL
            // route, this scene block runs at a reduced cadence, so its delta is a
            // measurement artifact, not a real stall. Do not arm on such a route, and
            // discard any gap that SPANS such a route (previous run was suppressed) so the
            // first normal-route frame after the route change can't false-arm either.
            const bool runtimeOwnedOverlayRoute =
                ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownForRuntimeOwnedOverlayRoute(
                    dx12_hook_g_FGRuntimeOwnsSwapchain, g_FGCompat.IsFSRFGApiActive(), HookHasRuntimeOwnedNativeFGPresentPath(),
                    ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup());

            if (s_lastProcessFrameTime.QuadPart != 0 && !runtimeOwnedOverlayRoute && !s_lastSceneBlockSuppressedRoute) {
                LARGE_INTEGER freq;
                QueryPerformanceFrequency(&freq);
                double deltaMs =
                    (double)(now.QuadPart - s_lastProcessFrameTime.QuadPart) * 1000.0 / (double)freq.QuadPart;

                const bool startupActivationPending =
                    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
                const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
                const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
                const bool postSLConfirmedButStartupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
                const bool suppressSceneCooldownForSyntheticStartup =
                    ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownDuringSyntheticPostSLStartup(
                        currentSLFGRunning, startupActivationPending, postSLActiveButUnconfirmed,
                        postSLConfirmedRendering, postSLConfirmedButStartupSettling);
                const bool suppressSceneCooldownForStablePostSLGap =
                    ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownForStablePostSLGap(
                        currentSLFGRunning, postSLConfirmedRendering, dx12_hook_g_PostSLLastWorkingQueue != nullptr,
                        DXGIShared::g_SharedState.swapchainInvalid.load(std::memory_order_acquire),
                        dx12_hook_g_DeviceRemoved.load(std::memory_order_acquire));

                if (deltaMs > 1000.0 && currentFGActive) {
                    if (suppressSceneCooldownForSyntheticStartup) {
                        static std::atomic<int> s_syntheticStartupSceneCooldownSkipLogCount{0};
                        const int logCount =
                            s_syntheticStartupSceneCooldownSkipLogCount.fetch_add(1, std::memory_order_relaxed);
                        if (logCount < 10 || (logCount % 100) == 0) {
                            HookLogImportant(
                                "DX12: Suppressing scene transition cooldown during half-armed synthetic PostSL "
                                "startup "
                                "(gap=%.0fms pending=%d unconfirmed=%d confirmed=%d settling=%d)",
                                deltaMs, startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                                postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0);
                        }
                    } else if (suppressSceneCooldownForStablePostSLGap) {
                        static std::atomic<int> s_stablePostSLSceneCooldownSkipLogCount{0};
                        const int logCount =
                            s_stablePostSLSceneCooldownSkipLogCount.fetch_add(1, std::memory_order_relaxed);
                        if (logCount < 10 || (logCount % 100) == 0) {
                            HookLogImportant(
                                "DX12: Suppressing scene transition cooldown after stable PostSL gap "
                                "(gap=%.0fms lastWorkingQ=%p)",
                                deltaMs, dx12_hook_g_PostSLLastWorkingQueue);
                        }
                    } else {
                        int cooldown = 30;
                        dx12_hook_g_SceneTransitionCooldown.store(cooldown, std::memory_order_release);
                        HookLogImportant(
                            "DX12: Scene transition detected (gap=%.0fms) during FG — overlay cooldown %d frames",
                            deltaMs, cooldown);
                    }
                }
            } else if (runtimeOwnedOverlayRoute && s_lastProcessFrameTime.QuadPart != 0) {
                static std::atomic<int> s_runtimeRouteSceneCooldownSuppressLogCount{0};
                const int logCount =
                    s_runtimeRouteSceneCooldownSuppressLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: Suppressing scene transition cooldown arming on runtime-owned/FSR-callback overlay "
                        "route (cadence artifact, not a stall) — fsrApi=%d runtimeOwns=%d log=%d",
                        g_FGCompat.IsFSRFGApiActive() ? 1 : 0, dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0, logCount + 1);
                }
            }
            s_lastProcessFrameTime = now;
            s_lastSceneBlockSuppressedRoute = runtimeOwnedOverlayRoute;
        }

        if (captureBeforeOverlay) {
            int64_t captureStartUs = PerfLogger::GetQpcUs();
            PublishDX12CapturedFrame(pSwapChain, captureShm, gameQueue, hasCurrentBackBufferIdx, currentBackBufferIdx);
            const int64_t captureUs = PerfLogger::GetQpcUs() - captureStartUs;
            perfMetrics.captureUs = static_cast<int32_t>(captureUs);
            if (diagnostics) {
                diagnostics->captureUs += captureUs;
            }
        }

        if (!skipOverlayDraw) {
            const char* skipSeparateOverlayGpuReason = nullptr;
            if (ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain(&skipSeparateOverlayGpuReason)) {
                static std::atomic<int> s_runtimeOwnedOverlayDrawSkipLogCount{0};
                int logCount = s_runtimeOwnedOverlayDrawSkipLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: Skipping separate overlay GPU draw because %s is active "
                        "(runtime=%s scQueue=%p origGame=%p cmdQ=%p postSL=%d)",
                        skipSeparateOverlayGpuReason ? skipSeparateOverlayGpuReason : "runtime-owned swapchain",
                        ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()), dx12_hook_g_SwapchainQueue,
                        dx12_hook_g_OriginalGameQueue, g_CommandQueue.load(std::memory_order_acquire),
                        dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire) ? 1 : 0);
                }
                goto skip_overlay_draw;
            }

            // PRE-SL RENDERING GATE — controls when pre-SL overlay is suppressed during SL FG.
            //
            // Two suppression points, both with stall fallback:
            //
            // 1. HERE (render site): Suppresses when SL FG is on and PostSL hasn't
            //    confirmed yet.  Gives PostSL ~5 frames to fire before falling back.
            //
            // 2. ABOVE (routing logic, line ~6305): Suppresses via skipOverlayDraw when
            //    PostSL IS confirmed but the stall counter exceeds threshold.
            //
            // Both use kPostSLStallThreshold/kPreSLFallbackThreshold (same value) to
            // detect "FG suspension" (SL nominally on, but not generating frames).
            //
            // PRE-SL RENDERING DURING FG SUSPENSION:
            // When pre-SL fallback activates, the overlay renders BEFORE SL's Present
            // trampoline.  This is safe because SL's FG pipeline is idle (not generating
            // frames).  The game's Present call goes through:
            //   ProcessFrame (overlay renders) → oPresent → SL passes through → real Present
            // Resource state is correct: game transitioned BB to PRESENT before Present,
            // we do PRESENT→RT→PRESENT round-trip, then SL sees PRESENT state.
            //
            // REGRESSION RISK: In Talos Reawakened, SL FG runs continuously (no menu
            // suspension).  The stall counter should never exceed 5 during normal play
            // because PostSL fires multiple times per game Present (real + interpolated).
            // If this regresses, increase kPreSLFallbackThreshold.
            {
                bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
                bool postSLActive = dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire);
                bool postSLConfirmed = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_relaxed);
                auto postSLCallback = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed);
                int stallCount = dx12_hook_g_PostSLStallCounter.load(std::memory_order_relaxed);
                constexpr int kPreSLFallbackThreshold = 5;

                if (slFGNow && !postSLConfirmed) {
                    // SL FG active, PostSL never confirmed yet (FG STARTUP, not suspension).
                    //
                    // CRITICAL: Do NOT fall back to pre-SL rendering here!
                    // During FG startup:
                    //   - SL creates a new swapchain with its own queue
                    //   - Backbuffers belong to SL's swapchain queue
                    //   - Pre-SL renders on origGame queue → cross-queue access → DEVICE_HUNG
                    //   - SL's FG pipeline hasn't started generating re-entrant Presents yet
                    //
                    // Pre-SL fallback is ONLY safe during FG SUSPENSION (PostSL was confirmed
                    // working but stopped firing — the game's backbuffer state is still valid
                    // on the game's queue because SL's FG pipeline is idle).
                    //
                    // During startup, we simply wait for PostSL to confirm. The overlay is
                    // invisible for a few frames during FG initialization. A pre-SL make-before-
                    // break here is NOT possible on the DLSS-startup path: the game's Present is
                    // consumed by Streamline's proxy, so all startup presents are SL re-entrant
                    // (PostSL) and ProcessFrameExternal is dormant — there is no pre-SL frame to
                    // draw. The only overlay route is PostSL, gated by the cold-start warmup
                    // (the documented GTA DLSS-init crash protection). See guardrails.md.
                    // Round 4: RTSS-style exception. When DLSS FG is toggled ON at runtime with NO
                    // separate Streamline queue (the present swapchain queue is still the game's own
                    // original queue), keep the already-live pre-SL overlay drawing instead of
                    // suppressing it, so the frame DLSS-G freezes on during init still carries the
                    // overlay. The overlay ECL lands on the game's own queue (same as the no-FG normal
                    // route) → no cross-queue DEVICE_HUNG, and it is NOT the PostSL re-entrant ECL the
                    // cold-start warmup protects. Opt-in (default OFF) + pure-DLSS + same-queue gated.
                    ID3D12CommandQueue* eagerSwapchainQueue = nullptr;
                    ID3D12CommandQueue* eagerOriginalGameQueue = nullptr;
                    {
                        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                        eagerSwapchainQueue = dx12_hook_g_SwapchainQueue;
                        eagerOriginalGameQueue = dx12_hook_g_OriginalGameQueue;
                    }
                    const bool eagerToggleOnDraw =
                        ce::dx12_overlay_policy::ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn(
                            DXGIShared::IsDlssToggleEagerOverlayEnabled(), dx12_hook_g_HadFSRFGPhase, dx12_hook_g_FGRuntimeOwnsSwapchain,
                            dx12_hook_g_State.overlayInit, dx12_hook_g_State.syncInit,
                            eagerSwapchainQueue != nullptr && eagerSwapchainQueue == eagerOriginalGameQueue);
                    if (eagerToggleOnDraw) {
                        NoteDX12OverlayCoverageGate("dlss-toggle-on-eager-presl-draw");
                        static int s_eagerToggleOnLog = 0;
                        ++s_eagerToggleOnLog;
                        if (s_eagerToggleOnLog <= 10 || (s_eagerToggleOnLog % 300) == 0) {
                            HookLogImportant(
                                "DX12: Keeping pre-SL overlay live during DLSS toggle-on (RTSS-style, "
                                "scQueue==origGame=%p postSLActive=%d stallCount=%d) #%d",
                                (void*)eagerSwapchainQueue, postSLActive ? 1 : 0, stallCount, s_eagerToggleOnLog);
                        }
                        // Fall through to the normal pre-SL draw below (do NOT goto skip_overlay_draw).
                    } else {
                        static int s_preSLSuppressLog = 0;
                        ++s_preSLSuppressLog;
                        if (s_preSLSuppressLog <= 10 || (s_preSLSuppressLog % 300) == 0) {
                            HookLogImportant(
                                "DX12: Suppressing pre-SL draw during SL FG startup — waiting for PostSL "
                                "(postSLCallback=%d postSLActive=%d hadFSR=%d stallCount=%d) #%d",
                                postSLCallback ? 1 : 0, postSLActive ? 1 : 0, dx12_hook_g_HadFSRFGPhase ? 1 : 0, stallCount,
                                s_preSLSuppressLog);
                        }
                        goto skip_overlay_draw;
                    }
                }
            }

            // Check scene transition cooldown for pre-SL path
            {
                int cd = dx12_hook_g_SceneTransitionCooldown.load(std::memory_order_acquire);
                if (cd > 0) {
                    dx12_hook_g_SceneTransitionCooldown.store(cd - 1, std::memory_order_release);
                    if (cd == 1)

                        HookLogImportant("DX12: Scene transition cooldown complete — resuming overlay");
                    NoteDX12OverlayCoverageGate("scene-transition-cooldown");
                    goto skip_overlay_draw;
                }
            }

            // Periodic health log for debugging stability
            static std::atomic<uint64_t> s_overlayFrameCount{0};
            uint64_t frameNum = s_overlayFrameCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (frameNum == 1 || frameNum == 10 || frameNum == 50 || frameNum == 100 || (frameNum % 500) == 0) {
                ID3D12Device* dev = g_Device.load();
                HRESULT devRemovedHr = dev ? dev->GetDeviceRemovedReason() : E_FAIL;
                HookLogImportant(
                    "DX12: Overlay frame #%llu (deviceRemoved=0x%08X, fgActive=%d, "
                    "queue=%p, allocIdx=%d, slFGRunning=%d)",
                    (unsigned long long)frameNum, (unsigned)devRemovedHr, currentFGActive ? 1 : 0, gameQueue,
                    dx12_hook_g_State.allocIndex, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) ? 1 : 0);
            }

            // Check device removed BEFORE rendering.  On first detection, tear
            // down overlay resources and set g_DeviceRemoved so heartbeats stop
            // (letting the freeze watchdog create a dump if we spin forever).
            {
                ID3D12Device* devCheck = g_Device.load();
                if (devCheck && FAILED(devCheck->GetDeviceRemovedReason())) {
                    if (!dx12_hook_g_DeviceRemoved.load(std::memory_order_relaxed)) {
                        dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
                        DXGIShared::g_SharedState.deviceRemovedFatal.store(true, std::memory_order_release);
                        g_RenderWatchdog.SetForceMonitor(true);
                        HookLogImportant("DX12: GPU device removed (0x%08X) — cleaning up overlay",
                                         (unsigned)devCheck->GetDeviceRemovedReason());
                        ce::dx12_dred::DumpOnDeviceRemoved(devCheck, "D3D12 device removed before overlay render");
                        const bool recentFocusTransition =
                            dx12_hook_g_FocusLossRecentTransitionPresentWindow.load(std::memory_order_acquire) > 0 ||
                            dx12_hook_g_FocusLossForegroundReacquirePresentProofRemaining.load(std::memory_order_acquire) > 0;
                        if (dx12_hook_s_WrappedPresentFocusLossContext.valid &&
                            (!processHasForeground || recentFocusTransition)) {
                            RequestFocusLossDeviceRemovalDumpOnce(
                                "D3D12 focus-loss device removal before overlay render",
                                devCheck->GetDeviceRemovedReason(), dx12_hook_s_WrappedPresentFocusLossContext, foregroundWindow,
                                foregroundPid, frameDesc.OutputWindow, currentProcessId, gameQueue);
                        }
                        dx12_hook_g_State.overlayInit = false;
                        CleanupRTVs();
                    }
                    goto overlay_done;
                } else if (dx12_hook_g_DeviceRemoved.load(std::memory_order_relaxed)) {
                    // Device recovered (new device set via DX12_SetCommandQueue)
                    dx12_hook_g_DeviceRemoved.store(false, std::memory_order_release);
                    DXGIShared::g_SharedState.deviceRemovedFatal.store(false, std::memory_order_release);
                    g_RenderWatchdog.SetForceMonitor(false);
                    ce::dx12_dred::ResetDumpEpoch();
                    HookLogImportant("DX12: Device recovered — overlay will reinitialize");
                }
            }

            {
                int allocatorPoolSize = static_cast<int>(dx12_hook_g_State.allocators.size());
                if (allocatorPoolSize <= 0) {
                    goto overlay_done;
                }

                int idx = dx12_hook_g_State.allocIndex % allocatorPoolSize;
                dx12_hook_g_State.allocIndex = (idx + 1) % allocatorPoolSize;

                // With 16 allocators, we never need to wait under normal conditions.
                // However, during Alt+Tab / GPU throttle, the GPU may stall and the
                // fence value for this allocator slot won't advance.  We must check
                // before Reset() to avoid undefined behaviour (driver hang / crash).
                auto* list = dx12_hook_g_State.cmdList;
                auto* alloc = (idx < (int)dx12_hook_g_State.allocators.size()) ? dx12_hook_g_State.allocators[idx] : nullptr;
                if (list && alloc) {
                    // Guard: skip overlay render if this allocator is still in flight.
                    if (dx12_hook_g_State.fence && idx < (int)dx12_hook_g_State.fenceValues.size() && dx12_hook_g_State.fenceValues[idx] > 0) {
                        UINT64 completed = dx12_hook_g_State.fence->GetCompletedValue();
                        if (completed < dx12_hook_g_State.fenceValues[idx]) {
                            if (activeDebugSample) {
                                activeDebugSample->flags |= kPresentSampleFlagAllocatorBusy;
                            }
                            static std::atomic<int> s_allocSkipLogs{0};
                            if (s_allocSkipLogs.fetch_add(1, std::memory_order_relaxed) < 30) {
                                HookLog(
                                    "DX12: Allocator[%d] still in-flight (completed=%llu, needed=%llu), "
                                    "skipping overlay this frame",
                                    idx, completed, dx12_hook_g_State.fenceValues[idx]);
                            }
                            goto overlay_done;
                        }
                    }
                    HRESULT allocResetHr = alloc->Reset();
                    if (SUCCEEDED(allocResetHr)) {
                        HRESULT listResetHr = list->Reset(alloc, nullptr);
                        // Log Reset results during FG for diagnostics
                        if (g_FGCompat.IsFGActive() || slFGActive) {
                            static std::atomic<int> s_fgResetLogs{0};
                            int fgResetLog = s_fgResetLogs.fetch_add(1, std::memory_order_relaxed);
                            if (fgResetLog < 5) {
                                HookLogImportant(
                                    "DX12: FG overlay alloc/list Reset (allocHr=0x%08X listHr=0x%08X idx=%d)",
                                    (unsigned)allocResetHr, (unsigned)listResetHr, idx);
                            }
                        }
                        if (SUCCEEDED(listResetHr)) {
                            // GPU-breadcrumb: stamp the start of CE's overlay command list so a native-FSR
                            // ffxQuery wedge can be attributed to CE's GPU ops vs a fence/CPU deadlock.
                            BeginOverlayGpuBreadcrumbFrame(g_Device.load(std::memory_order_acquire));
                            WriteOverlayGpuBreadcrumb(list, kOverlayBcStart);
                            const bool preserveLiveStartupOverlayDuringInactiveSL =
                                ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff();
                            const bool hasPendingStartupOverlayResources = g_OverlayAdapter.HasPendingDX12Resources();
                            const bool shouldPrimeStartupOverlayResources =
                                dx12_hook_s_startupOverlayResourcePrimeMs == 0 &&
                                ce::dx12_overlay_policy::ShouldPrimeStartupOverlayResources(
                                    startupOverlayCompatibilityActive, hasPendingStartupOverlayResources,
                                    preserveLiveStartupOverlayDuringInactiveSL);
                            if (startupOverlayCompatibilityActive && hasPendingStartupOverlayResources &&
                                preserveLiveStartupOverlayDuringInactiveSL) {
                                static std::atomic<int> s_skipStartupPrimeForLiveOverlayLogCount{0};
                                const int skipPrimeLog =
                                    s_skipStartupPrimeForLiveOverlayLogCount.fetch_add(1, std::memory_order_relaxed);
                                if (skipPrimeLog < 5 || (skipPrimeLog % 300) == 0) {
                                    HookLogImportant(
                                        "DX12: Skipping startup resource priming delay because live overlay is "
                                        "preserved through runtime-inactive Streamline handoff (log=%d)",
                                        skipPrimeLog + 1);
                                }
                            }
                            if (shouldPrimeStartupOverlayResources) {
                                // Check device before priming — after FG teardown the
                                // device may already be removed (async GPU fault).
                                {
                                    auto* primeDev = g_Device.load(std::memory_order_acquire);
                                    HRESULT primeDevHr = primeDev ? primeDev->GetDeviceRemovedReason() : E_FAIL;
                                    if (FAILED(primeDevHr)) {
                                        HookLogImportant("DX12: SKIPPING resource priming — device removed 0x%08X",
                                                         (unsigned)primeDevHr);
                                        dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
                                        goto overlay_done;
                                    }
                                }
                                HookLogImportant("DX12: Priming DX12 overlay resources before first GTA overlay draw");
                                if (!g_OverlayAdapter.PrimeDX12Resources(list)) {
                                    HookLogImportant(
                                        "DX12: DX12 overlay resource priming failed; deferring first overlay draw");
                                    goto overlay_done;
                                }

                                HRESULT closeHr = list->Close();
                                if (FAILED(closeHr)) {
                                    HookLog("DX12: Priming command list close failed hr=0x%08X, forcing reinit",
                                            closeHr);
                                    dx12_hook_g_State.syncInit = false;
                                    goto overlay_done;
                                }

                                if (!SubmitOverlayCommandList(gameQueue, list, idx, "startup resource priming",
                                                              false)) {
                                    HookLogImportant(
                                        "DX12: Startup resource priming submission failed; deferring first overlay "
                                        "draw");
                                    goto overlay_done;
                                }

                                // Check device after priming submit — catch async GPU fault immediately
                                {
                                    auto* postPrimeDev = g_Device.load(std::memory_order_acquire);
                                    HRESULT postPrimeDevHr =
                                        postPrimeDev ? postPrimeDev->GetDeviceRemovedReason() : E_FAIL;
                                    if (FAILED(postPrimeDevHr)) {
                                        HookLogImportant("DX12: Resource priming CAUSED device removal 0x%08X!",
                                                         (unsigned)postPrimeDevHr);
                                        dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
                                        goto overlay_done;
                                    }
                                }

                                dx12_hook_s_startupOverlayResourcePrimeMs = GetTickCount64();
                                HookLogImportant(
                                    "DX12: DX12 overlay resource priming submitted, delaying first overlay draw for "
                                    "%llums",
                                    dx12_hook_kStartupOverlayPostResourcePrimeSettleMs);
                                goto overlay_done;
                            }

                            if (shouldRunStartupOverlayDrawProbe &&
                                dx12_hook_s_startupOverlayFirstDrawProbeStage == StartupOverlayFirstDrawProbeStage::kNone) {
                                // Probe system removed: go straight to rendering.
                                // The 3-stage probe (backbuffer touch → pipeline state → real draw) caused
                                // ERR_GFX_STATE in GTA5 Enhanced because even barrier-only probes on a
                                // dedicated overlay queue conflict with the game's D3D12 state tracking.
                                // With single-queue mode (fix for dedicated queue), we can render directly.
                                dx12_hook_s_startupOverlayFirstDrawProbeStage = StartupOverlayFirstDrawProbeStage::kActualRender;
                            }

                            IDXGISwapChain3* sc3 = dx12_hook_g_State.cachedSC3;
                            if (!sc3) {
                                if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                                    sc3->Release();           // drop QI ref — weak cache is safe
                                    dx12_hook_g_State.cachedSC3 = sc3;  // because swapchain is alive during Present
                                }
                            }
                            LARGE_INTEGER perfQI, perfGetBuf, perfRecord, perfSubmit, perfEnd, perfFreq;
                            QueryPerformanceFrequency(&perfFreq);
                            QueryPerformanceCounter(&perfQI);
                            if (sc3) {
                                UINT swapchainBufferIdx = sc3->GetCurrentBackBufferIndex();
                                currentBackBufferIdx = swapchainBufferIdx;
                                hasCurrentBackBufferIdx = true;
                                // CRITICAL FIX: Use actual swapchain buffer index directly
                                // CreateRTVs now creates RTVs for all swapchain buffers (up to 8)
                                // so no need to wrap the index - this prevents sync issues
                                UINT bufferIdx = swapchainBufferIdx;
                                // Validate buffer index is within our allocated range
                                if (bufferIdx >= (UINT)dx12_hook_g_State.bufferCount) {
                                    HookLog(
                                        "DX12: Buffer index %u exceeds allocated count %d, "
                                        "clamping",
                                        bufferIdx, dx12_hook_g_State.bufferCount);
                                    bufferIdx = dx12_hook_g_State.bufferCount - 1;
                                }
                                // FG-SAFE: Acquire backbuffer per-frame via GetBuffer.
                                // We do NOT cache backbuffer pointers because FSR FG
                                // monitors reference counts and crashes if extra refs
                                // are held persistently.
                                ID3D12Resource* bb = nullptr;
                                bool bbNeedsRelease = false;
                                QueryPerformanceCounter(&perfGetBuf);
                                if (SUCCEEDED(sc3->GetBuffer(swapchainBufferIdx, IID_PPV_ARGS(&bb))) && bb) {
                                    bbNeedsRelease = true;
                                    // Recreate RTV for this buffer index (cheap CPU-side op).
                                    // Ensures RTV matches current buffer even after FSR FG
                                    // swapchain transitions.
                                    D3D12_CPU_DESCRIPTOR_HANDLE rtvRecreate =
                                        dx12_hook_g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                                    rtvRecreate.ptr += (SIZE_T)bufferIdx * dx12_hook_g_State.rtvDescriptorSize;
                                    g_Device.load()->CreateRenderTargetView(bb, nullptr, rtvRecreate);
                                }
                                if (bb) {
                                    bool cmdRecordOk = false;
                                    static std::atomic<int> s_firstBackBufferLogCount{0};
                                    if (s_firstBackBufferLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                                        HookLogImportant(
                                            "DX12: Preparing overlay draw for backbuffer idx=%u resource=%p via %s "
                                            "queue (queue=%p)",
                                            bufferIdx, bb,
                                            dx12_hook_g_State.overlayQueue
                                                ? "dedicated overlay"
                                                : (gameQueue == dx12_hook_g_SwapchainQueue ? "swapchain" : "game"),
                                            gameQueue);
                                    }

                                    // SL FG diagnostic: log every overlay draw during FG
                                    if (slFGActive) {
                                        static std::atomic<int> s_slFGDrawCount{0};
                                        int fgDraw = s_slFGDrawCount.fetch_add(1, std::memory_order_relaxed) + 1;
                                        if (fgDraw <= 20 || (fgDraw % 10) == 0) {
                                            auto* diagDev = g_Device.load(std::memory_order_acquire);
                                            HRESULT devHr = diagDev ? diagDev->GetDeviceRemovedReason() : E_FAIL;
                                            bool dedicated = dx12_hook_g_State.overlayQueue && ShouldUseDedicatedOverlayQueue();
                                            HookLogImportant(
                                                "DX12: SL-FG overlay ENTER #%d (bufIdx=%u bb=%p queue=%p dedQ=%d "
                                                "tid=0x%04X devRemoved=0x%08X)",
                                                fgDraw, bufferIdx, bb, gameQueue, dedicated ? 1 : 0,
                                                GetCurrentThreadId(), (unsigned)devHr);
                                        }
                                    }

                                    // GPU drain: flush all in-flight GPU work before first
                                    // overlay render after FSR→DLSS transition.  This ensures
                                    // SL's FG pipeline has fully completed before we touch
                                    // the backbuffer, preventing GPU-side deadlock/TDR.
                                    if (dx12_hook_g_NeedGPUDrainBeforeRender && gameQueue) {
                                        auto* drainDev = g_Device.load(std::memory_order_acquire);
                                        if (drainDev) {
                                            if (!dx12_hook_g_DrainFence) {
                                                HRESULT hr = drainDev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                                                   IID_PPV_ARGS(&dx12_hook_g_DrainFence));
                                                if (SUCCEEDED(hr)) {
                                                    dx12_hook_g_DrainEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
                                                    dx12_hook_g_DrainFenceValue = 0;
                                                    HookLogImportant("DX12: GPU drain fence created");
                                                } else {
                                                    HookLogImportant("DX12: GPU drain fence creation failed hr=0x%08X",
                                                                     (unsigned)hr);
                                                }
                                            }
                                            if (dx12_hook_g_DrainFence && dx12_hook_g_DrainEvent) {
                                                UINT64 drainVal = ++dx12_hook_g_DrainFenceValue;
                                                HRESULT sigHr = gameQueue->Signal(dx12_hook_g_DrainFence, drainVal);
                                                if (SUCCEEDED(sigHr)) {
                                                    if (dx12_hook_g_DrainFence->GetCompletedValue() < drainVal) {
                                                        dx12_hook_g_DrainFence->SetEventOnCompletion(drainVal, dx12_hook_g_DrainEvent);
                                                        DWORD waitResult = WaitForSingleObject(dx12_hook_g_DrainEvent, 5000);
                                                        HookLogImportant(
                                                            "DX12: GPU drain completed (wait=%s val=%llu queue=%p)",
                                                            waitResult == WAIT_OBJECT_0
                                                                ? "OK"
                                                                : (waitResult == WAIT_TIMEOUT ? "TIMEOUT" : "FAIL"),
                                                            drainVal, gameQueue);
                                                    } else {
                                                        HookLogImportant(
                                                            "DX12: GPU drain — already complete (val=%llu)", drainVal);
                                                    }
                                                } else {
                                                    HookLogImportant("DX12: GPU drain Signal failed hr=0x%08X",
                                                                     (unsigned)sigHr);
                                                }
                                            }
                                        }
                                        dx12_hook_g_NeedGPUDrainBeforeRender = false;
                                    }

                                    // ================================================================
                                    // PRIMARY PATH: Barrier-free offscreen compositing.
                                    // Renders overlay to offscreen RT (no swapchain stall),
                                    // then copies to backbuffer with implicit state promotion.
                                    // DComp disabled — Direct Flip prevents reliable display.
                                    // ================================================================

                                    // ============================================================
                                    // PRIMARY: native DX12 overlay.
                                    // x64 keeps the descriptor-free root-SRV font path that avoids
                                    // SetDescriptorHeaps + OMSetRenderTargets(swapchain).  x86 uses
                                    // the Texture2D/SRV backend because DRED isolated the 32-bit
                                    // hang to the descriptor-free text sampling path.
                                    // ============================================================
                                    bool usedPrimaryOverlayBackend = false;
                                    bool usedDescFree = false;
                                    bool offscreenCompositeRequired = false;
                                    {
                                        auto* dev = g_Device.load();
#if defined(_WIN64)
                                        constexpr bool kIs32BitProcess = false;
#else
                                        constexpr bool kIs32BitProcess = true;
#endif
                                        const bool useTextureDx12Backend =
                                            ce::dx12_overlay_policy::ShouldUseTextureDx12OverlayBackendForProcess(
                                                kIs32BitProcess);
                                        // Pre-DescFree device health check
                                        if (dev) {
                                            HRESULT preDescFreeDevHr = dev->GetDeviceRemovedReason();
                                            if (FAILED(preDescFreeDevHr)) {
                                                HookLogImportant(
                                                    "DX12: DEVICE ALREADY REMOVED before DescFree init "
                                                    "(devRemoved=0x%08X tid=0x%04X)",
                                                    (unsigned)preDescFreeDevHr, GetCurrentThreadId());
                                            }
                                        }
                                        if (useTextureDx12Backend && dx12_hook_g_DescFreeBackend) {
                                            ShutdownDescFreeBackend("x86 Texture2D backend selected");
                                        }
                                        if (dev && useTextureDx12Backend && !dx12_hook_g_D3D11On12Adapter.IsInitialized()) {
                                            ID3D12CommandQueue* backendQueue =
                                                gameQueue ? gameQueue : g_CommandQueue.load(std::memory_order_acquire);
                                            if (backendQueue &&
                                                dx12_hook_g_D3D11On12Adapter.InitDX12(dev, backendQueue, dx12_hook_g_State.format)) {
                                                HookLogImportant(
                                                    "DX12: x86 native Texture2D overlay backend ready "
                                                    "(device=%p queue=%p fmt=%d)",
                                                    dev, backendQueue, (int)dx12_hook_g_State.format);
                                            } else {
                                                HookLogImportant(
                                                    "DX12: x86 native Texture2D overlay backend init failed "
                                                    "(device=%p queue=%p fmt=%d)",
                                                    dev, backendQueue, (int)dx12_hook_g_State.format);
                                            }
                                        } else if (dev && !useTextureDx12Backend) {
                                            // Reuses the warm device-scoped backend when device and
                                            // format still match; rebuilds it otherwise.
                                            EnsureDescFreeBackendForDeviceAndFormat(dev, dx12_hook_g_State.format,
                                                                                    "normal route");
                                        }
                                        const bool primaryOverlayReady =
                                            useTextureDx12Backend
                                                ? dx12_hook_g_D3D11On12Adapter.IsInitialized()
                                                : (dx12_hook_g_DescFreeBackend && dx12_hook_g_D3D11On12Adapter.IsInitialized());
                                        if (primaryOverlayReady) {
                                            bool isRealFrame = g_FGCompat.IsCurrentFrameReal();
                                            dx12_hook_g_D3D11On12Adapter.SetReserveInactiveFGSpace(
                                                ShouldReserveInactiveFGOverlaySpaceNow());

                                            // After FSR→DLSS: ANY direct backbuffer access (barriers,
                                            // RT, ClearRTV) causes DEVICE_HUNG because SL's FG pipeline
                                            // has in-flight work on the backbuffer from another queue.
                                            // Instead, render to offscreen RT, then CopyTextureRegion
                                            // to the backbuffer.  CopyTextureRegion uses COPY_DEST state
                                            // which IS implicitly promotable from COMMON/PRESENT — no
                                            // explicit barrier needed on the backbuffer.
                                            // Two-copy offscreen compositing was designed for PostSL
                                            // rendering where the backbuffer's state is unknown.
                                            // For pre-SL rendering on the game thread, the backbuffer
                                            // is in PRESENT state (game transitioned it before Present).
                                            // Use normal direct rendering with PRESENT→RT→PRESENT barriers.
                                            //
                                            // EXCEPTION: After FSR→DLSS→OFF, the backbuffer state is
                                            // indeterminate (FG pipeline may have left it in any state).
                                            // Use offscreen compositing to avoid explicit barriers on
                                            // the backbuffer entirely.  Cleared on clean swapchain
                                            // transition.
                                            const bool usePostFSROffscreenCopy =
                                                dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG;
                                            bool useOffscreenCopy = usePostFSROffscreenCopy;

                                            if (useOffscreenCopy) {
                                                offscreenCompositeRequired = true;
                                                const char* offscreenReason = "post-FSR DLSS";
                                                if (!bb) {
                                                    HookLogImportant(
                                                        "DX12: Cannot use offscreen compositing for %s overlay because "
                                                        "backbuffer is null; direct backbuffer fallback suppressed",
                                                        offscreenReason);
                                                } else {
                                                    static int s_postFSROffscreenLog = 0;
                                                    if (s_postFSROffscreenLog++ < 10) {
                                                        HookLogImportant(
                                                            "DX12: Using offscreen compositing for post-FSR non-FG "
                                                            "overlay "
                                                            "(bb=%p queue=%p bufIdx=%u #%d)",
                                                            bb, gameQueue, bufferIdx, s_postFSROffscreenLog);
                                                    }
                                                    // Two-copy compositing: avoids ALL explicit barriers on backbuffer.
                                                    // 1. Copy bb→offscreen (bb implicitly promotes COMMON→COPY_SOURCE)
                                                    // 2. Barrier offscreen COPY_DEST→RT
                                                    // 3. Render overlay on top of game frame in offscreen
                                                    // 4. Barrier offscreen RT→COPY_SOURCE
                                                    // 5. Copy offscreen→bb (bb implicitly promotes COMMON→COPY_DEST)
                                                    // After ECL, both resources decay back to COMMON.
                                                    if (EnsureOffscreenRT(dev, dx12_hook_g_State.cachedWidth,
                                                                          dx12_hook_g_State.cachedHeight, dx12_hook_g_State.format)) {
                                                        // Step 1: Copy backbuffer → offscreen RT
                                                        // bb: implicit promotion COMMON→COPY_SOURCE (no explicit
                                                        // barrier!) offscreen: explicit COMMON→COPY_DEST
                                                        {
                                                            D3D12_RESOURCE_BARRIER b = {};
                                                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                            b.Transition.pResource = dx12_hook_g_State.offscreenRT;
                                                            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                                                            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                                                            b.Transition.Subresource =
                                                                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                            list->ResourceBarrier(1, &b);
                                                        }
                                                        {
                                                            D3D12_TEXTURE_COPY_LOCATION src = {};
                                                            src.pResource = bb;
                                                            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                                            src.SubresourceIndex = 0;
                                                            D3D12_TEXTURE_COPY_LOCATION dst = {};
                                                            dst.pResource = dx12_hook_g_State.offscreenRT;
                                                            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                                            dst.SubresourceIndex = 0;
                                                            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                                                        }

                                                        // Step 2: Barrier offscreen COPY_DEST → RENDER_TARGET
                                                        {
                                                            D3D12_RESOURCE_BARRIER b = {};
                                                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                            b.Transition.pResource = dx12_hook_g_State.offscreenRT;
                                                            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                                                            b.Transition.StateAfter =
                                                                D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                            b.Transition.Subresource =
                                                                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                            list->ResourceBarrier(1, &b);
                                                        }

                                                        // Step 3: Render overlay to offscreen RT (on top of game frame)
                                                        D3D12_CPU_DESCRIPTOR_HANDLE offRtv =
                                                            dx12_hook_g_State.offscreenRtvHeap
                                                                ->GetCPUDescriptorHandleForHeapStart();
                                                        if (useTextureDx12Backend) {
                                                            dx12_hook_g_D3D11On12Adapter.SetDX12RenderTarget(list,
                                                                                                   (void*)offRtv.ptr);
                                                            dx12_hook_g_D3D11On12Adapter.SetDX12UploadSlotFence(
                                                                dx12_hook_g_State.fence,
                                                                ce::dx12_overlay_policy::
                                                                    DecideOverlayUploadSlotGuardValue(
                                                                        slFGActive || g_FGCompat.IsFGActive(),
                                                                        dx12_hook_g_State.fence != nullptr,
                                                                        dx12_hook_g_State.currentFenceValue));
                                                        } else {
                                                            dx12_hook_s_descFreeCmdList = list;
                                                            dx12_hook_s_descFreeRtv = offRtv;
                                                            // Publish the per-slot UPLOAD-ring guard.  Non-FG path:
                                                            // this frame's overlay work is signaled on g_State.fence at
                                                            // currentFenceValue+1, so the backend can pace slot reuse
                                                            // to the GPU.  FG paths use a separate completion fence
                                                            // (g_State.fence does not advance to this value) and
                                                            // already synchronize per frame, so disable the guard there
                                                            // to avoid a stale-value wait.
                                                            dx12_hook_s_descFreeSlotFence = dx12_hook_g_State.fence;
                                                            dx12_hook_s_descFreeSlotGuardValue = ce::dx12_overlay_policy::
                                                                DecideOverlayUploadSlotGuardValue(
                                                                    slFGActive || g_FGCompat.IsFGActive(),
                                                                    dx12_hook_g_State.fence != nullptr,
                                                                    dx12_hook_g_State.currentFenceValue);
                                                        }

                                                        dx12_hook_g_D3D11On12Adapter.SetIPCClient(g_IPC);
                                                        const auto metricsBinding =
                                                            ce::dx12_overlay_policy::DecideOverlayMetricsBinding(
                                                                isRealFrame);
                                                        if (metricsBinding.bindMetrics) {
                                                            dx12_hook_g_D3D11On12Adapter.SetMetrics(
                                                                DXGIShared::GetPerformanceMetrics());
                                                        }
                                                        if (metricsBinding.refreshFrameMetadata) {
                                                            const char* api = "DX12";
                                                            dx12_hook_g_D3D11On12Adapter.SetGraphicsAPI(api);
                                                        }
                                                        SyncSecondaryDx12OverlayColorState(dx12_hook_g_State.format);
                                                        dx12_hook_g_D3D11On12Adapter.RenderOverlay(dx12_hook_g_State.cachedWidth,
                                                                                         dx12_hook_g_State.cachedHeight);
                                                        if (!useTextureDx12Backend) {
                                                            dx12_hook_s_descFreeCmdList = nullptr;
                                                        }

                                                        // Step 4: Barrier offscreen RT → COPY_SOURCE
                                                        {
                                                            D3D12_RESOURCE_BARRIER b = {};
                                                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                            b.Transition.pResource = dx12_hook_g_State.offscreenRT;
                                                            b.Transition.StateBefore =
                                                                D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                                                            b.Transition.Subresource =
                                                                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                            list->ResourceBarrier(1, &b);
                                                        }

                                                        // Step 5: Copy offscreen → backbuffer
                                                        // bb: implicit promotion COMMON→COPY_DEST (no explicit
                                                        // barrier!)
                                                        {
                                                            D3D12_TEXTURE_COPY_LOCATION src = {};
                                                            src.pResource = dx12_hook_g_State.offscreenRT;
                                                            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                                            src.SubresourceIndex = 0;
                                                            D3D12_TEXTURE_COPY_LOCATION dst = {};
                                                            dst.pResource = bb;
                                                            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                                            dst.SubresourceIndex = 0;
                                                            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                                                        }

                                                        // After ECL: bb decays COPY_DEST→COMMON, offscreen decays
                                                        // COPY_SOURCE→COMMON

                                                        static int s_offscreenLog = 0;
                                                        if (s_offscreenLog++ < 5) {
                                                            HookLogImportant(
                                                                "DX12: %s overlay via 2-copy compositing (bb=%p "
                                                                "offRT=%p queue=%p)",
                                                                offscreenReason, bb, dx12_hook_g_State.offscreenRT, gameQueue);
                                                        }
                                                        usedPrimaryOverlayBackend = true;
                                                        usedDescFree = !useTextureDx12Backend;
                                                    } else {
                                                        HookLogImportant(
                                                            "DX12: Failed to create offscreen RT for %s overlay; "
                                                            "direct backbuffer fallback suppressed",
                                                            offscreenReason);
                                                    }
                                                }
                                            } else {
                                                // Normal path: render directly to backbuffer
                                                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
                                                    dx12_hook_g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                                                UINT rtvSize = dev->GetDescriptorHandleIncrementSize(
                                                    D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                                                rtvHandle.ptr += (SIZE_T)bufferIdx * rtvSize;

                                                // ALWAYS add barriers in the primary native DX12 path.
                                                // At startup, extOverlay may be false
                                                // (socialclub.dll not loaded yet); after
                                                // FG teardown, Social Club may not render
                                                // in the menu screen.  The backbuffer is
                                                // in PRESENT state in both cases, so the
                                                // PRESENT→RT transition is correct.
                                                bool fgBarriersNeeded = true;
                                                if (fgBarriersNeeded && bb) {
                                                    D3D12_RESOURCE_BARRIER barrier = {};
                                                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                    barrier.Transition.pResource = bb;
                                                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                                                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                    barrier.Transition.Subresource =
                                                        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                    list->ResourceBarrier(1, &barrier);
                                                }

                                                if (useTextureDx12Backend) {
                                                    dx12_hook_g_D3D11On12Adapter.SetDX12RenderTarget(list, (void*)rtvHandle.ptr);
                                                    dx12_hook_g_D3D11On12Adapter.SetDX12UploadSlotFence(
                                                        dx12_hook_g_State.fence,
                                                        ce::dx12_overlay_policy::DecideOverlayUploadSlotGuardValue(
                                                            slFGActive || g_FGCompat.IsFGActive(),
                                                            dx12_hook_g_State.fence != nullptr, dx12_hook_g_State.currentFenceValue));
                                                } else {
                                                    dx12_hook_s_descFreeCmdList = list;
                                                    dx12_hook_s_descFreeRtv = rtvHandle;
                                                    // Publish the per-slot UPLOAD-ring guard (see offscreen path
                                                    // above).
                                                    dx12_hook_s_descFreeSlotFence = dx12_hook_g_State.fence;
                                                    dx12_hook_s_descFreeSlotGuardValue =
                                                        ce::dx12_overlay_policy::DecideOverlayUploadSlotGuardValue(
                                                            slFGActive || g_FGCompat.IsFGActive(),
                                                            dx12_hook_g_State.fence != nullptr, dx12_hook_g_State.currentFenceValue);
                                                }

                                                dx12_hook_g_D3D11On12Adapter.SetIPCClient(g_IPC);
                                                const auto metricsBinding =
                                                    ce::dx12_overlay_policy::DecideOverlayMetricsBinding(isRealFrame);
                                                if (metricsBinding.bindMetrics) {
                                                    dx12_hook_g_D3D11On12Adapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
                                                }
                                                if (metricsBinding.refreshFrameMetadata) {
                                                    static const bool s_isVKD3D = []() {
                                                        return GetModuleHandleA("d3d12core.dll") &&
                                                               (GetModuleHandleA("libvkd3d-1.dll") ||
                                                                GetModuleHandleA("vkd3d.dll"));
                                                    }();
                                                    const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
                                                    dx12_hook_g_D3D11On12Adapter.SetGraphicsAPI(api);
                                                }

                                                SyncSecondaryDx12OverlayColorState(dx12_hook_g_State.format);
                                                dx12_hook_g_D3D11On12Adapter.RenderOverlay(dx12_hook_g_State.cachedWidth,
                                                                                 dx12_hook_g_State.cachedHeight);

                                                // Transition back to PRESENT after overlay draw
                                                if (fgBarriersNeeded && bb) {
                                                    D3D12_RESOURCE_BARRIER barrier = {};
                                                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                    barrier.Transition.pResource = bb;
                                                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                                                    barrier.Transition.Subresource =
                                                        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                    list->ResourceBarrier(1, &barrier);
                                                }

                                                if (!useTextureDx12Backend) {
                                                    dx12_hook_s_descFreeCmdList = nullptr;
                                                }
                                                usedPrimaryOverlayBackend = true;
                                                usedDescFree = !useTextureDx12Backend;
                                            }  // end normal path

                                        }
                                    }

                                    // Fallback: standard DX12 rendering (uses SetDescriptorHeaps —
                                    // may cause 60% GPU on some NVIDIA configs)
                                    if (!usedPrimaryOverlayBackend) {
                                        if (offscreenCompositeRequired) {
                                            static std::atomic<int> s_offscreenRequiredNoFallbackLogCount{0};
                                            const int logCount = s_offscreenRequiredNoFallbackLogCount.fetch_add(
                                                1, std::memory_order_relaxed);
                                            if (logCount < 20 || (logCount % 300) == 0) {
                                                HookLogImportant(
                                                    "DX12: Skipping direct backbuffer fallback because offscreen "
                                                    "composite is required for this frame "
                                                    "(postFSR=%d queue=%p bufIdx=%u log=%d)",
                                                    dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG ? 1 : 0, gameQueue,
                                                    bufferIdx, logCount + 1);
                                            }
                                        } else {
                                            if (!startupOverlayPresent) {
                                                D3D12_RESOURCE_BARRIER barrier = {};
                                                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                barrier.Transition.pResource = bb;
                                                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                                                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                barrier.Transition.Subresource =
                                                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                list->ResourceBarrier(1, &barrier);
                                            }
                                            WriteOverlayGpuBreadcrumb(list, kOverlayBcAfterRTBarrier);

                                            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
                                                dx12_hook_g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                                            UINT rtvSize = g_Device.load()->GetDescriptorHandleIncrementSize(
                                                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                                            rtvHandle.ptr += (SIZE_T)bufferIdx * rtvSize;
                                            list->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

                                            bool isRealFrame = g_FGCompat.IsCurrentFrameReal();
                                            DrawOverlay(list, isRealFrame, bufferIdx);
                                            WriteOverlayGpuBreadcrumb(list, kOverlayBcAfterDraw);

                                            if (!startupOverlayPresent) {
                                                D3D12_RESOURCE_BARRIER barrier = {};
                                                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                barrier.Transition.pResource = bb;
                                                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                                                barrier.Transition.Subresource =
                                                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                list->ResourceBarrier(1, &barrier);
                                            }
                                        }
                                    }

                                    const bool overlayDrawRecorded =
                                        usedPrimaryOverlayBackend || !offscreenCompositeRequired;
                                    QueryPerformanceCounter(&perfRecord);

                                    // GPU-breadcrumb: stamp the END of CE's overlay command list. If the GPU reaches
                                    // this marker but ffxQuery still wedges, CE's GPU work is NOT the stall (look to a
                                    // fence/CPU deadlock or AMD's own work); if it stops earlier, CE's list is the
                                    // stall.
                                    WriteOverlayGpuBreadcrumb(list, kOverlayBcBeforeClose);

                                    HRESULT closeHr = list->Close();
                                    // Log Close result during FG
                                    if (g_FGCompat.IsFGActive() || slFGActive) {
                                        static std::atomic<int> s_fgCloseLogs{0};
                                        if (s_fgCloseLogs.fetch_add(1, std::memory_order_relaxed) < 5) {
                                            HookLogImportant("DX12: FG overlay list->Close hr=0x%08X",
                                                             (unsigned)closeHr);
                                        }
                                    }
                                    // Always log Close result for first N reinit frames
                                    {
                                        static int s_reinitCloseLogCount = 0;
                                        if (dx12_hook_g_ResetReinitSubmitCounter.load(std::memory_order_relaxed))
                                            s_reinitCloseLogCount = 0;
                                        if (s_reinitCloseLogCount < 5) {
                                            s_reinitCloseLogCount++;
                                            auto* closeDev = g_Device.load(std::memory_order_acquire);
                                            HRESULT closeDevHr = closeDev ? closeDev->GetDeviceRemovedReason() : E_FAIL;
                                            HookLogImportant(
                                                "DX12: Reinit Close #%d hr=0x%08X devRemoved=0x%08X primaryOverlay=%d",
                                                s_reinitCloseLogCount, (unsigned)closeHr, (unsigned)closeDevHr,
                                                usedPrimaryOverlayBackend ? 1 : 0);
                                        }
                                    }
                                    if (FAILED(closeHr)) {
                                        HookLog("DX12: list->Close failed hr=0x%08X, forcing reinit", closeHr);
                                        dx12_hook_g_State.syncInit = false;
                                    } else {
                                        // Choose submit queue: dedicated overlay queue when
                                        // available (SL FG active), otherwise game queue.
                                        bool useDedicated = dx12_hook_g_State.overlayQueue && ShouldUseDedicatedOverlayQueue();
                                        ID3D12CommandQueue* eclQueue = useDedicated ? dx12_hook_g_State.overlayQueue : gameQueue;

                                        // One-time diagnostic: check if SL also hooked
                                        // the overlay queue's ECL vtable entry.
                                        if (useDedicated && slFGActive) {
                                            static bool s_eclVtableChecked = false;
                                            if (!s_eclVtableChecked) {
                                                s_eclVtableChecked = true;
                                                void** vtable = *(void***)eclQueue;
                                                void* eclAddr = vtable[10];
                                                HMODULE eclMod = nullptr;
                                                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                                   (LPCSTR)eclAddr, &eclMod);
                                                char modName[MAX_PATH] = {};
                                                if (eclMod)
                                                    GetModuleFileNameA(eclMod, modName, MAX_PATH);
                                                HookLogImportant(
                                                    "DX12: Overlay queue ECL vtable[10]=%p module='%s' (SL hooked=%d)",
                                                    eclAddr, modName, (strstr(modName, "sl.") != nullptr) ? 1 : 0);
                                                // Also log game queue for comparison
                                                void** gvtable = *(void***)gameQueue;
                                                void* geclAddr = gvtable[10];
                                                HMODULE geclMod = nullptr;
                                                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                                   (LPCSTR)geclAddr, &geclMod);
                                                char gModName[MAX_PATH] = {};
                                                if (geclMod)
                                                    GetModuleFileNameA(geclMod, gModName, MAX_PATH);
                                                HookLogImportant("DX12: Game queue ECL vtable[10]=%p module='%s'",
                                                                 geclAddr, gModName);
                                            }
                                        }

                                        // Cross-queue sync: drain game queue before submitting
                                        // on dedicated queue so game rendering completes first.
                                        if (useDedicated && gameQueue) {
                                            WaitForGameQueueBeforeDedicatedOverlaySubmission(gameQueue, "overlay ECL");
                                        }

                                        ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(eclQueue);
                                        ID3D12CommandList* lists[] = {list};

                                        // Pre-ECL device health check — distinguish
                                        // "device already dead" from "our ECL killed it"
                                        {
                                            auto* preEclDev = g_Device.load(std::memory_order_acquire);
                                            if (preEclDev) {
                                                HRESULT preEclDevHr = preEclDev->GetDeviceRemovedReason();
                                                if (FAILED(preEclDevHr)) {
                                                    HookLogImportant(
                                                        "DX12: DEVICE ALREADY REMOVED before overlay ECL "
                                                        "(devRemoved=0x%08X queue=%p realECL=%p origECL=%p tid=0x%04X) "
                                                        "— SKIPPING",
                                                        (unsigned)preEclDevHr, eclQueue, (void*)dx12_hook_g_RealD3D12ECL.load(),
                                                        (void*)origECL, GetCurrentThreadId());
                                                    dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
                                                    goto overlay_done;
                                                }
                                            }
                                        }

                                        // During ANY FG on the game queue, call the real
                                        // D3D12 ECL directly (bypasses our vtable detour
                                        // AND any FG runtime ECL hooks).
                                        // SL FG: avoids incrementing ECL count + SL detour.
                                        // FSR FG: avoids FSR's ECL hook which counts our
                                        //   overlay submission as a game command list,
                                        //   confusing FSR's frame interpolation tracking.
                                        //
                                        // ALWAYS use realECL when available (not just during FG).
                                        // Without this, our overlay ECL goes through the vtable
                                        // → our ECL detour → counted by FG heuristic → false
                                        // FSR FG detection after DLSS FG turns off (2:1 ratio
                                        // from game ECL + overlay ECL looks like frame gen).
                                        //
                                        // EXCEPTION: After FSR→DLSS, eclQueue is SL's wrapper
                                        // (g_CommandQueue). Must use vtable call (origECL) so
                                        // SL's ECL interception handles resource state for
                                        // the FSR-created backbuffers.
                                        ExecuteCommandListsPtr realECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
                                        bool usedRealECL = false;
                                        const bool classifyFocusLossSubmitPath =
                                            dx12_hook_s_WrappedPresentFocusLossContext.valid && !processHasForeground;
                                        bool submitPathIsD3D12Module = false;
                                        bool isSLWrapperECL =
                                            dx12_hook_g_HadFSRFGPhase && slFGActive &&
                                            eclQueue == g_CommandQueue.load(std::memory_order_acquire) &&
                                            eclQueue != dx12_hook_g_OriginalGameQueue;

                                        // Log ECL path decision for first N frames per reinit
                                        {
                                            static int s_eclPathLogCount = 0;
                                            if (dx12_hook_g_ResetReinitSubmitCounter.load(std::memory_order_relaxed))
                                                s_eclPathLogCount = 0;
                                            if (s_eclPathLogCount < 3) {
                                                s_eclPathLogCount++;
                                                const char* path = (!useDedicated && realECL && !isSLWrapperECL)
                                                                       ? "realECL"
                                                                   : origECL ? "origECL"
                                                                             : "vtable";
                                                const bool pathIsD3D12Module =
                                                    (!useDedicated && realECL && !isSLWrapperECL)
                                                        ? true
                                                        : (origECL
                                                               ? IsD3D12ModuleAddress(reinterpret_cast<void*>(origECL))
                                                               : false);
                                                HookLogImportant(
                                                    "DX12: ECL path=%s (eclQ=%p realECL=%p origECL=%p "
                                                    "dedicated=%d slWrapper=%d directD3D12=%d scQ=%p origGame=%p)",
                                                    path, eclQueue, (void*)realECL, (void*)origECL,
                                                    useDedicated ? 1 : 0, isSLWrapperECL ? 1 : 0,
                                                    pathIsD3D12Module ? 1 : 0, dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue);
                                            }
                                        }

                                        // Steam ECL deferred submit: when g_deferOverlaySubmitToSteamECL
                                        // is true (non-SL Steam overlay path), skip the normal overlay ECL
                                        // submission.  The overlay command list is closed and ready, but
                                        // submission is deferred to DetourExecuteCommandLists which fires
                                        // AFTER Steam's overlay handler submits its ECL.  This ensures CE
                                        // overlay renders on top of Steam's cleared backbuffer.
                                        if (dx12_hook_g_deferOverlaySubmitToSteamECL && !useDedicated) {
                                            dx12_hook_g_steamDeferredOverlay.cmdList = list;
                                            dx12_hook_g_steamDeferredOverlay.allocIdx = idx;
                                            dx12_hook_g_steamDeferredOverlay.eclQueue = eclQueue;
                                            dx12_hook_g_steamDeferredOverlay.pending = true;
                                            static std::atomic<int> s_deferredSkipLogCount{0};
                                            int deferredSkipNum =
                                                s_deferredSkipLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                                            if (deferredSkipNum <= 20 || (deferredSkipNum % 200) == 0) {
                                                HookLogImportant(
                                                    "DX12: Deferring overlay ECL submit to Steam ECL hook #%d "
                                                    "(eclQueue=%p, list=%p, allocIdx=%d, bb=%p, bufIdx=%u)",
                                                    deferredSkipNum, eclQueue, list, idx, bb, bufferIdx);
                                            }
                                            // Skip the normal fence signal too - ECL hook will signal it.
                                            goto skip_steam_deferred_fence_signal;
                                        }

                                        {
                                            ScopedCEOverlayECLSubmission ceOverlayECLGuard("normal overlay submit");
                                            if (!useDedicated && realECL && !isSLWrapperECL) {
                                                const bool fsrFGActiveForECL =
                                                    g_FGCompat.GetRuntimeMode() == ce::fg_runtime::RuntimeMode::kFSRFG;
                                                if (fsrFGActiveForECL) {
                                                    // FSR FG active: use origECL (vtable/hook-aware) instead of
                                                    // realECL (raw D3D12 function).  FSR hooks the game queue's
                                                    // ECL vtable entry to track command list submissions.  When
                                                    // we bypass FSR's hook via realECL, FSR detects the raw ECL
                                                    // on its queue as an unexpected submission and removes the
                                                    // D3D12 device (ERR_GFX_STATE).  Using origECL lets FSR's
                                                    // hook see and accept our overlay ECL.
                                                    origECL(eclQueue, 1, lists);
                                                    if (classifyFocusLossSubmitPath) {
                                                        submitPathIsD3D12Module =
                                                            IsD3D12ModuleAddress(reinterpret_cast<void*>(origECL));
                                                    }
                                                } else {
                                                    realECL(eclQueue, 1, lists);
                                                    usedRealECL = true;
                                                    submitPathIsD3D12Module = true;
                                                }
                                            } else if (origECL) {
                                                origECL(eclQueue, 1, lists);
                                                if (classifyFocusLossSubmitPath) {
                                                    submitPathIsD3D12Module =
                                                        IsD3D12ModuleAddress(reinterpret_cast<void*>(origECL));
                                                }
                                            } else {
                                                eclQueue->ExecuteCommandLists(1, lists);
                                                if (classifyFocusLossSubmitPath && eclQueue) {
                                                    void** vtable = *reinterpret_cast<void***>(eclQueue);
                                                    submitPathIsD3D12Module =
                                                        vtable && IsD3D12ModuleAddress(vtable[10]);
                                                }
                                            }
                                        }
                                        if (overlayDrawRecorded) {
                                            NoteDX12OverlayRendered(DX12OverlayRenderRoute::kNormal);
                                        }

                                        // SL/FSR FG diagnostic: log after ECL submission
                                        if (slFGActive || g_FGCompat.IsFGActive()) {
                                            static std::atomic<int> s_fgSubmitCount{0};
                                            int fgSubmit = s_fgSubmitCount.fetch_add(1, std::memory_order_relaxed) + 1;
                                            if (fgSubmit <= 20 || (fgSubmit % 100) == 0) {
                                                auto* diagDev2 = g_Device.load(std::memory_order_acquire);
                                                HRESULT devHr2 = diagDev2 ? diagDev2->GetDeviceRemovedReason() : E_FAIL;
                                                HookLogImportant(
                                                    "DX12: FG overlay SUBMIT #%d (queue=%p descFree=%d realECL=%d "
                                                    "slFG=%d fsrFG=%d gameQ=%d devRemoved=0x%08X tid=0x%04X)",
                                                    fgSubmit, eclQueue, usedDescFree ? 1 : 0, usedRealECL ? 1 : 0,
                                                    slFGActive ? 1 : 0, g_FGCompat.IsFGActive() ? 1 : 0,
                                                    !useDedicated ? 1 : 0, (unsigned)devHr2, GetCurrentThreadId());
                                            }
                                        }

                                        // Unconditional post-submit diagnostic: log first 50
                                        // submits after each overlay reinit.  Catches
                                        // DEVICE_REMOVED even when FG is inactive.
                                        {
                                            static int s_reinitSubmitCount = 0;
                                            if (dx12_hook_g_ResetReinitSubmitCounter.exchange(false, std::memory_order_acquire))
                                                s_reinitSubmitCount = 0;
                                            if (s_reinitSubmitCount < 50) {
                                                s_reinitSubmitCount++;
                                                auto* diagDevR = g_Device.load(std::memory_order_acquire);
                                                HRESULT devHrR = diagDevR ? diagDevR->GetDeviceRemovedReason() : E_FAIL;
                                                HookLogImportant(
                                                    "DX12: Reinit SUBMIT #%d (queue=%p descFree=%d realECL=%d "
                                                    "directD3D12=%d offscreen=%d extOverlay=%d bb=%p bufIdx=%d "
                                                    "devRemoved=0x%08X tid=0x%04X)",
                                                    s_reinitSubmitCount, eclQueue, usedDescFree ? 1 : 0,
                                                    usedRealECL ? 1 : 0, submitPathIsD3D12Module ? 1 : 0,
                                                    offscreenCompositeRequired ? 1 : 0, startupOverlayPresent ? 1 : 0,
                                                    bb, bufferIdx, (unsigned)devHrR, GetCurrentThreadId());
                                                if (FAILED(devHrR)) {
                                                    HookLogImportant("DX12: DEVICE REMOVED after reinit submit #%d!",
                                                                     s_reinitSubmitCount);
                                                    dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
                                                }
                                            }
                                        }

                                        // Post-FG-transition diagnostic: log first 20 frames after any FG change.
                                        // Catches DEVICE_REMOVED right after overlay resumes following FG switches.
                                        {
                                            static int s_postTransitionFrames = 0;
                                            static int s_lastTransitionCooldown = -1;
                                            int curCooldown = dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire);
                                            if (curCooldown > 0 && s_lastTransitionCooldown <= 0)
                                                s_postTransitionFrames = 0;  // new transition started
                                            if (curCooldown <= 0 && s_lastTransitionCooldown > 0)
                                                s_postTransitionFrames = 0;  // transition just ended
                                            s_lastTransitionCooldown = curCooldown;
                                            if (s_postTransitionFrames < 50) {
                                                s_postTransitionFrames++;
                                                auto* diagDev3 = g_Device.load(std::memory_order_acquire);
                                                HRESULT devHr3 = diagDev3 ? diagDev3->GetDeviceRemovedReason() : E_FAIL;
                                                HookLogImportant(
                                                    "DX12: Post-transition SUBMIT #%d (queue=%p origQ=%p cmdQ=%p "
                                                    "fgActive=%d slFG=%d descFree=%d realECL=%d devRemoved=0x%08X "
                                                    "bb=%p bufIdx=%d tid=0x%04X)",
                                                    s_postTransitionFrames, eclQueue, dx12_hook_g_OriginalGameQueue,
                                                    (void*)g_CommandQueue.load(), g_FGCompat.IsFGActive() ? 1 : 0,
                                                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)
                                                        ? 1
                                                        : 0,
                                                    usedDescFree ? 1 : 0, usedRealECL ? 1 : 0, (unsigned)devHr3, bb,
                                                    bufferIdx, GetCurrentThreadId());
                                            }
                                        }

                                        if (dx12_hook_g_State.fence) {
                                            UINT64 next = dx12_hook_g_State.currentFenceValue + 1;
                                            bool anyFGForFence = slFGActive || g_FGCompat.IsFGActive();
                                            const auto presentContext = dx12_hook_s_WrappedPresentFocusLossContext;
                                            const bool runtimeOwnedPresentation =
                                                dx12_hook_g_FGRuntimeOwnsSwapchain || HookHasRuntimeOwnedNativeFGPresentPath() ||
                                                DXGIShared::DoesFGRuntimeOwnSwapchain();
                                            bool deviceLostForFence = false;
                                            {
                                                auto* fenceDev = g_Device.load(std::memory_order_acquire);
                                                if (fenceDev) {
                                                    deviceLostForFence = FAILED(fenceDev->GetDeviceRemovedReason());
                                                }
                                            }
                                            const bool steamDeferredOverlaySubmit =
                                                dx12_hook_g_deferOverlaySubmitToSteamECL && !useDedicated;
                                            const bool focusLossImmediateFence = ce::dx12_overlay_policy::
                                                ShouldSignalD3D12FocusLossOverlayFenceImmediately(
                                                    presentContext.valid, !frameDesc.Windowed, processHasForeground,
                                                    iconicWindow, zeroSizedSwapchain, true, deviceLostForFence,
                                                    anyFGForFence, runtimeOwnedPresentation, useDedicated,
                                                    steamDeferredOverlaySubmit, dx12_hook_g_State.fence != nullptr,
                                                    dx12_hook_g_State.fenceEvent != nullptr, eclQueue != nullptr, next);

                                            if (!focusLossImmediateFence && presentContext.valid &&
                                                !processHasForeground) {
                                                static std::atomic<int> s_focusImmediateSkipLogCount{0};
                                                const int logCount = s_focusImmediateSkipLogCount.fetch_add(
                                                    1, std::memory_order_relaxed);
                                                if (logCount < 40 || (logCount % 300) == 0) {
                                                    HookLog(
                                                        "DX12: Focus-loss immediate overlay fence skipped (%s "
                                                        "present=%s#%d queue=%p fg=%p/%lu game=%p/%lu sync=%u "
                                                        "flags=0x%08X next=%llu event=%p fgActive=%d runtimeOwned=%d "
                                                        "dedicated=%d steamDeferred=%d deviceLost=%d realECL=%d "
                                                        "directD3D12=%d descFree=%d offscreen=%d)",
                                                        DescribeFocusLossImmediateFenceSkip(
                                                            presentContext.valid, !frameDesc.Windowed,
                                                            processHasForeground, iconicWindow, zeroSizedSwapchain,
                                                            true, deviceLostForFence, anyFGForFence,
                                                            runtimeOwnedPresentation, useDedicated,
                                                            steamDeferredOverlaySubmit, dx12_hook_g_State.fence != nullptr,
                                                            dx12_hook_g_State.fenceEvent != nullptr, eclQueue != nullptr, next),
                                                        presentContext.presentName ? presentContext.presentName
                                                                                   : "Present",
                                                        presentContext.callCount, eclQueue, foregroundWindow,
                                                        foregroundPid, frameDesc.OutputWindow, currentProcessId,
                                                        presentContext.syncInterval, presentContext.presentFlags,
                                                        (unsigned long long)next, dx12_hook_g_State.fenceEvent,
                                                        anyFGForFence ? 1 : 0, runtimeOwnedPresentation ? 1 : 0,
                                                        useDedicated ? 1 : 0, steamDeferredOverlaySubmit ? 1 : 0,
                                                        deviceLostForFence ? 1 : 0, usedRealECL ? 1 : 0,
                                                        submitPathIsD3D12Module ? 1 : 0, usedDescFree ? 1 : 0,
                                                        offscreenCompositeRequired ? 1 : 0);
                                                }
                                            }

                                            if (focusLossImmediateFence) {
                                                HRESULT sigHr = eclQueue->Signal(dx12_hook_g_State.fence, next);
                                                UINT64 completedValue = dx12_hook_g_State.fence->GetCompletedValue();
                                                if (SUCCEEDED(sigHr)) {
                                                    dx12_hook_g_State.currentFenceValue = next;
                                                    if (idx >= 0 && idx < (int)dx12_hook_g_State.fenceValues.size())
                                                        dx12_hook_g_State.fenceValues[idx] = next;

                                                    static std::atomic<int> s_focusImmediateSignalLogCount{0};
                                                    const int logCount = s_focusImmediateSignalLogCount.fetch_add(
                                                        1, std::memory_order_relaxed);
                                                    if (logCount < 80 || (logCount % 300) == 0) {
                                                        HookLogImportant(
                                                            "DX12: Focus-loss immediate overlay fence signal "
                                                            "(present=%s#%d queue=%p fence=%llu completed=%llu "
                                                            "fg=%p/%lu game=%p/%lu sync=%u flags=0x%08X realECL=%d "
                                                            "directD3D12=%d descFree=%d offscreen=%d); waiting before "
                                                            "Present to sync same-frame work",
                                                            presentContext.presentName ? presentContext.presentName
                                                                                       : "Present",
                                                            presentContext.callCount, eclQueue,
                                                            (unsigned long long)next,
                                                            (unsigned long long)completedValue, foregroundWindow,
                                                            foregroundPid, frameDesc.OutputWindow, currentProcessId,
                                                            presentContext.syncInterval, presentContext.presentFlags,
                                                            usedRealECL ? 1 : 0, submitPathIsD3D12Module ? 1 : 0,
                                                            usedDescFree ? 1 : 0, offscreenCompositeRequired ? 1 : 0);
                                                    }
                                                    WaitForFocusLossImmediateOverlayFenceBeforePresent(
                                                        focusLossImmediateFence, true, dx12_hook_g_State.fence,
                                                        dx12_hook_g_State.fenceEvent, eclQueue, next, presentContext,
                                                        foregroundWindow, foregroundPid, frameDesc.OutputWindow,
                                                        currentProcessId, usedRealECL, submitPathIsD3D12Module,
                                                        usedDescFree, offscreenCompositeRequired);
                                                } else {
                                                    static std::atomic<int> s_focusImmediateSignalFailLogCount{0};
                                                    const int logCount = s_focusImmediateSignalFailLogCount.fetch_add(
                                                        1, std::memory_order_relaxed);
                                                    if (logCount < 20 || (logCount % 300) == 0) {
                                                        HookLogImportant(
                                                            "DX12: Focus-loss immediate overlay fence Signal failed "
                                                            "hr=0x%08X (present=%s#%d queue=%p fence=%llu fg=%p/%lu "
                                                            "game=%p/%lu sync=%u flags=0x%08X); falling back to "
                                                            "post-Present deferred signal",
                                                            (unsigned)sigHr,
                                                            presentContext.presentName ? presentContext.presentName
                                                                                       : "Present",
                                                            presentContext.callCount, eclQueue,
                                                            (unsigned long long)next, foregroundWindow, foregroundPid,
                                                            frameDesc.OutputWindow, currentProcessId,
                                                            presentContext.syncInterval, presentContext.presentFlags);
                                                    }
                                                    dx12_hook_g_deferredSignalQueue.store(eclQueue, std::memory_order_release);
                                                    dx12_hook_g_deferredSignalValue.store(next, std::memory_order_release);
                                                    dx12_hook_g_deferredSignalAllocIdx.store(idx, std::memory_order_release);
                                                }
                                            } else if (anyFGForFence && !useDedicated) {
                                                // During ANY FG on the game queue, signal a separate
                                                // overlay completion fence via the raw D3D12 Signal
                                                // pointer (bypassing SL/FSR vtable hooks), then wait
                                                // on CPU.  This ensures all overlay GPU work
                                                // (including barriers, font upload, draw commands)
                                                // is complete before the FG runtime reads the
                                                // swapchain backbuffer in its Present hook.
                                                //
                                                // Without this wait, the overlay could still be
                                                // executing PRESENT->RT/RT->PRESENT barriers or
                                                // draw commands on the GPU when the FG runtime
                                                // processes the backbuffer, causing D3D device
                                                // removal (ERR_GFX_STATE).
                                                SignalPtr realSignal =
                                                    dx12_hook_g_RealD3D12Signal.load(std::memory_order_acquire);
                                                ID3D12Fence* completionFence =
                                                    dx12_hook_g_OverlayCompletionFence.load(std::memory_order_acquire);
                                                if (realSignal && completionFence) {
                                                    static std::atomic<UINT64> s_overlayCompletionValue{0};
                                                    UINT64 compVal = ++s_overlayCompletionValue;
                                                    HRESULT compSigHr = realSignal(eclQueue, completionFence, compVal);
                                                    if (SUCCEEDED(compSigHr)) {
                                                        if (completionFence->GetCompletedValue() < compVal) {
                                                            HANDLE compEvent =
                                                                CreateEventW(nullptr, FALSE, FALSE, nullptr);
                                                            if (compEvent) {
                                                                completionFence->SetEventOnCompletion(compVal,
                                                                                                      compEvent);
                                                                WaitForSingleObject(compEvent, 2000);
                                                                CloseHandle(compEvent);
                                                            }
                                                        }
                                                    } else {
                                                        static std::atomic<int> s_compSigFailLog{0};
                                                        if (s_compSigFailLog.fetch_add(1) < 10) {
                                                            HookLogImportant(
                                                                "DX12: Overlay completion fence Signal failed "
                                                                "hr=0x%08X (queue=%p)",
                                                                (unsigned)compSigHr, eclQueue);
                                                        }
                                                    }
                                                }
                                            } else if (useDedicated) {
                                                // Signal immediately on dedicated queue (SL
                                                // doesn't see it).
                                                HRESULT sigHr = eclQueue->Signal(dx12_hook_g_State.fence, next);
                                                if (SUCCEEDED(sigHr)) {
                                                    dx12_hook_g_State.currentFenceValue = next;
                                                    if (idx >= 0 && idx < (int)dx12_hook_g_State.fenceValues.size())
                                                        dx12_hook_g_State.fenceValues[idx] = next;
                                                }
                                            } else {
                                                // Game queue: defer fence Signal to next frame
                                                // (avoids NVIDIA driver stall between Signal and
                                                // Present).
                                                dx12_hook_g_deferredSignalQueue.store(eclQueue, std::memory_order_release);
                                                dx12_hook_g_deferredSignalValue.store(next, std::memory_order_release);
                                                dx12_hook_g_deferredSignalAllocIdx.store(idx, std::memory_order_release);
                                            }
                                        } else if (dx12_hook_s_WrappedPresentFocusLossContext.valid && !processHasForeground) {
                                            static std::atomic<int> s_focusImmediateNoFenceLogCount{0};
                                            const int logCount =
                                                s_focusImmediateNoFenceLogCount.fetch_add(1, std::memory_order_relaxed);
                                            if (logCount < 20 || (logCount % 300) == 0) {
                                                const auto presentContext = dx12_hook_s_WrappedPresentFocusLossContext;
                                                const bool anyFGForFence = slFGActive || g_FGCompat.IsFGActive();
                                                const bool runtimeOwnedPresentation =
                                                    dx12_hook_g_FGRuntimeOwnsSwapchain ||
                                                    HookHasRuntimeOwnedNativeFGPresentPath() ||
                                                    DXGIShared::DoesFGRuntimeOwnSwapchain();
                                                HookLog(
                                                    "DX12: Focus-loss immediate overlay fence skipped (%s "
                                                    "present=%s#%d queue=%p fg=%p/%lu game=%p/%lu sync=%u "
                                                    "flags=0x%08X fgActive=%d runtimeOwned=%d); no same-frame fence "
                                                    "sync is possible",
                                                    DescribeFocusLossImmediateFenceSkip(
                                                        presentContext.valid, !frameDesc.Windowed, processHasForeground,
                                                        iconicWindow, zeroSizedSwapchain, true, false, anyFGForFence,
                                                        runtimeOwnedPresentation, useDedicated,
                                                        dx12_hook_g_deferOverlaySubmitToSteamECL && !useDedicated, false,
                                                        dx12_hook_g_State.fenceEvent != nullptr, eclQueue != nullptr, 0),
                                                    presentContext.presentName ? presentContext.presentName : "Present",
                                                    presentContext.callCount, eclQueue, foregroundWindow, foregroundPid,
                                                    frameDesc.OutputWindow, currentProcessId,
                                                    presentContext.syncInterval, presentContext.presentFlags,
                                                    anyFGForFence ? 1 : 0, runtimeOwnedPresentation ? 1 : 0);
                                            }
                                        }
                                    skip_steam_deferred_fence_signal:
                                        cmdRecordOk = true;
                                        QueryPerformanceCounter(&perfSubmit);
                                    }

                                    QueryPerformanceCounter(&perfEnd);
                                    if (diagnostics && perfFreq.QuadPart > 0) {
                                        const auto toUs = [&](LONGLONG ticks) {
                                            return (ticks * 1000000) / perfFreq.QuadPart;
                                        };
                                        diagnostics->overlayAcquireUs =
                                            toUs(perfGetBuf.QuadPart - perfQI.QuadPart);
                                        diagnostics->overlayRecordUs =
                                            toUs(perfRecord.QuadPart - perfGetBuf.QuadPart);
                                        diagnostics->overlaySubmitUs =
                                            toUs(perfSubmit.QuadPart - perfRecord.QuadPart);
                                        diagnostics->overlayPostSubmitUs =
                                            toUs(perfEnd.QuadPart - perfSubmit.QuadPart);
                                        diagnostics->overlayBreakdownValid = true;
                                    }
                                    // Periodic perf dump every 300 frames
                                    static int s_perfDumpCounter = 0;
                                    if (++s_perfDumpCounter % 300 == 0) {
                                        double toUs = 1000000.0 / (double)perfFreq.QuadPart;
                                        double qiUs = (double)(perfGetBuf.QuadPart - perfQI.QuadPart) * toUs;
                                        double getBufUs = (double)(perfRecord.QuadPart - perfGetBuf.QuadPart) * toUs;
                                        double submitUs = (double)(perfSubmit.QuadPart - perfRecord.QuadPart) * toUs;
                                        double totalUs = (double)(perfEnd.QuadPart - perfQI.QuadPart) * toUs;
                                        HookLogImportant(
                                            "DX12: Overlay perf: QI+idx=%.0fus getBuf+record=%.0fus submit=%.0fus "
                                            "total=%.0fus",
                                            qiUs, getBufUs, submitUs, totalUs);
                                    }

                                    if (cmdRecordOk) {
                                        static int s_firstOverlaySubmitLogged = 0;
                                        if (s_firstOverlaySubmitLogged == 0) {
                                            s_firstOverlaySubmitLogged = 1;
                                            HookLogImportant(
                                                "DX12: ProcessFrame - first overlay render command list submitted "
                                                "successfully");
                                        }

                                        if (!dx12_hook_s_startupOverlayCompatSettled.exchange(true, std::memory_order_acq_rel)) {
                                            if (shouldRunStartupOverlayDrawProbe &&
                                                dx12_hook_s_startupOverlayFirstDrawProbeStage ==
                                                    StartupOverlayFirstDrawProbeStage::kActualRender) {
                                                HookLogImportant(
                                                    "DX12: Startup overlay compat settled - future sync reinit will "
                                                    "keep the full allocator pool");
                                            } else {
                                                HookLogImportant(
                                                    "DX12: Stable DX12 overlay rendering observed - later startup "
                                                    "overlay popups will stay on the normal coexistence path");
                                            }
                                        }

                                        // Clear probe state if we were in a probe sequence
                                        if (shouldRunStartupOverlayDrawProbe &&
                                            dx12_hook_s_startupOverlayFirstDrawProbeStage ==
                                                StartupOverlayFirstDrawProbeStage::kActualRender) {
                                            HookLogImportant("DX12: Startup overlay probe complete - rendering stably");
                                            dx12_hook_s_startupOverlayFirstDrawProbeStage =
                                                StartupOverlayFirstDrawProbeStage::kComplete;
                                            dx12_hook_s_startupOverlayFirstDrawProbeMs = 0;
                                        }
                                    }
                                    // FG-SAFE: Release per-frame backbuffer reference
                                    if (bbNeedsRelease)
                                        bb->Release();
                                } else {
                                    HookLog("DX12: GetBuffer(%u) failed, forcing RTV reinit", swapchainBufferIdx);
                                    CleanupRTVs();
                                    dx12_hook_g_State.overlayInit = false;
                                }
                            } else {
                                HookLog("DX12: ProcessFrame - failed to get SwapChain3 interface");
                            }
                        } else {
                            HookLog("DX12: ProcessFrame - list->Reset failed hr=0x%08X, forcing reinit", listResetHr);
                            dx12_hook_g_State.syncInit = false;
                        }
                    } else {
                        HookLog("DX12: ProcessFrame - alloc->Reset failed hr=0x%08X, forcing reinit", allocResetHr);
                        dx12_hook_g_State.syncInit = false;
                    }
                } else {
                    HookLog("DX12: ProcessFrame - null list or alloc");
                }
            }  // end device-removed-check scope
        }  // end !skipOverlayDraw
    skip_overlay_draw:;
    overlay_done:;
    }

    // Change 6: Remove verbose debug logging - keep only error logging
    if (captureAfterOverlay) {

        int64_t captureStartUs = PerfLogger::GetQpcUs();
        PublishDX12CapturedFrame(pSwapChain, captureShm, gameQueue, hasCurrentBackBufferIdx, currentBackBufferIdx);
        const int64_t captureUs = PerfLogger::GetQpcUs() - captureStartUs;
        perfMetrics.captureUs = static_cast<int32_t>(captureUs);
        if (diagnostics) {
            diagnostics->captureUs += captureUs;
        }
    }
}

void DX12_ResetImGuiFrameCounter() {
    dx12_hook_s_framesBeforeInit = 0;
    // Also reset the post-init frame counter
    dx12_hook_s_framesSinceInit = 0;
    HookLog("DX12: Reset ImGui frame counter");
}

void DX12_ResetOverlayFrameDelay() {
    dx12_hook_s_framesSinceInit = 0;
    dx12_hook_s_initDelayComplete = false;
    HookLog("DX12: Reset overlay frame delay counter");
}

// Minimal-overhead ProcessFrame for no-callback FSR FG: skips the ~400 lines of policy/lock/heuristic
// work in DX12_ProcessFrameExternal (two g_CommandQueueMutex acquisitions, FSR heuristic checks, stale
// cleanup with AddRef/Release, PostSL processing) that take ~27.5ms and desync AMD's QPC-timed pacing.
// Does only: frame-count reset, RecordFrame, sc3 acquire, capture decision, inner ProcessFrame (which
// has the overlay-skip gate — no overlay draw during no-callback FSR FG). Capture still works.
void DX12_ProcessFrameMinimal(IDXGISwapChain* pSwapChain, bool applicationSourcePresent,
                              bool frameGenerationPresentationActive) {
    if (HookIsShuttingDown() || !pSwapChain) {
        return;
    }
    if (!dx12_hook_g_DeviceRemoved.load(std::memory_order_acquire)) {
        g_RenderWatchdog.HeartbeatFromHelperThread();
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
        return;
    }
    const int count = dx12_hook_g_CommandListsExecutedThisFrame.exchange(0);
    ++dx12_hook_g_FGDebugFrameCount;
    g_FGCompat.RecordFrame(count);
    const bool isInterpolatedFrame = (count == 0);
    bool processCapture = !isInterpolatedFrame;
    if (processCapture && ShouldSkipCaptureForTargetCadence()) {
        processCapture = false;
    }
    SharedMemoryLayout* screenshotShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    OverlayConfig screenshotOverlayCfg = GetActiveDX12OverlayConfig(screenshotShm);
    const uint64_t screenshotRequestId = GetPendingScreenshotRequestId(screenshotShm);
    const bool screenshotRequested = screenshotRequestId != 0;
    const bool screenshotWantsOverlay =
        screenshotRequested && screenshotOverlayCfg.showOverlay && screenshotOverlayCfg.screenshotIncludeOverlay;
    if (screenshotRequested && !screenshotWantsOverlay) {
        CaptureRequestedDX12Screenshot(sc3, screenshotShm, screenshotRequestId);
    }
    ProcessFrame(sc3, processCapture, applicationSourcePresent, frameGenerationPresentationActive);
    if (screenshotWantsOverlay && !ShouldUseConfirmedPostSLForOverlayIncludedWork(screenshotOverlayCfg)) {
        CaptureRequestedDX12Screenshot(sc3, screenshotShm, screenshotRequestId);
    }
    sc3->Release();
}

static void DX12_ProcessFrameExternalForPresent(IDXGISwapChain* pSwapChain, bool applicationSourcePresent,
                                                bool frameGenerationPresentationActive) {
    ce::dx12_process_frame_diagnostics::StageTimings timings;
    const auto wrapperActivityBefore = GetWrapperHookActivitySnapshot();
    DX12_ProcessFrameExternal(pSwapChain, applicationSourcePresent, frameGenerationPresentationActive, &timings);
    if (timings.totalUs >= 5000) {
        const auto wrapperActivityAfter = GetWrapperHookActivitySnapshot();
        static std::atomic<int> s_slowProcessFrameLogCount{0};
        const int logCount = s_slowProcessFrameLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 200 || (logCount % 50) == 0) {
            const auto breakdown = ce::dx12_process_frame_diagnostics::ComputeBreakdown(timings);
            const bool wrapperActivityOverlap = ce::dx12_process_frame_diagnostics::DidActivityOverlap(
                wrapperActivityBefore, wrapperActivityAfter);
            HookLogImportant(
                "DX12 DIAG: ProcessFrame (overlay) SLOW %.1fms breakdown external=%.3fms inner=%.3fms "
                "innerOther=%.3fms capture=%.3fms overlay=%.3fms "
                "[valid=%d acquire=%.3fms record=%.3fms submit=%.3fms post=%.3fms] screenshot=%.3fms "
                "queueLockWait=%.3fms wrapperInitOverlap=%d wrapperActivity=%llu/%u->%llu/%u "
                "innerCalled=%d reentrantSkip=%d tid=0x%04X",
                static_cast<double>(timings.totalUs) / 1000.0,
                static_cast<double>(breakdown.externalUs) / 1000.0,
                static_cast<double>(timings.innerUs) / 1000.0,
                static_cast<double>(breakdown.innerOtherUs) / 1000.0,
                static_cast<double>(timings.captureUs) / 1000.0,
                static_cast<double>(breakdown.overlayUs) / 1000.0, timings.overlayBreakdownValid ? 1 : 0,
                static_cast<double>(timings.overlayAcquireUs) / 1000.0,
                static_cast<double>(timings.overlayRecordUs) / 1000.0,
                static_cast<double>(timings.overlaySubmitUs) / 1000.0,
                static_cast<double>(timings.overlayPostSubmitUs) / 1000.0,
                static_cast<double>(timings.screenshotUs) / 1000.0,
                static_cast<double>(timings.commandQueueLockWaitUs) / 1000.0, wrapperActivityOverlap ? 1 : 0,
                static_cast<unsigned long long>(wrapperActivityBefore.generation), wrapperActivityBefore.activeCalls,
                static_cast<unsigned long long>(wrapperActivityAfter.generation), wrapperActivityAfter.activeCalls,
                timings.innerCalled ? 1 : 0, timings.reentrantInnerSkipped ? 1 : 0, GetCurrentThreadId());
        }
    }
}

void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain) {
    const bool frameGenerationPresentationActive =
        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) ||
        DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration() ||
        HookHasRuntimeOwnedNativeFGPresentPath();
    const bool applicationSourcePresent = ce::dx12_overlay_policy::ShouldApplyDX12PrerenderLimitOnPresent(
        frameGenerationPresentationActive, DX12_GetGamePresentThreadId(), GetCurrentThreadId());
    DX12_ProcessFrameExternalForPresent(pSwapChain, applicationSourcePresent, frameGenerationPresentationActive);
}

namespace DXGIShared {
void HandleDX12ProcessFrame(IDXGISwapChain* pSwapChain, bool applicationSourcePresent,
                            bool frameGenerationPresentationActive) {
    DX12_ProcessFrameExternalForPresent(pSwapChain, applicationSourcePresent, frameGenerationPresentationActive);
}
}

namespace DXGIShared {
void HandleDX12ResizeBegin() {
    HookLog("DX12: HandleDX12ResizeBegin CALLED from DetourResizeBuffers");
    DX12_OnSwapchainResizeBegin();
}
}

namespace DXGIShared {
void HandleDX12ResizeEnd() {
    HookLog("DX12: HandleDX12ResizeEnd CALLED");
    DX12_OnSwapchainResizeEnd();
}
}

bool DX12Hook::IsRealFrame() const {
    return g_FGCompat.IsCurrentFrameReal();
}

void DX12Hook::ClassifyFrame(int commandListCount) {
    g_FGCompat.RecordFrame(commandListCount);
}

// FIXED: Clean up the global hook instance if allocated
// Service the deferred ECL probe: if ProbeRealD3D12ECL was skipped because
// the Streamline startup window was active, try to probe now that the window
// has expired.  Safe to call from any thread at any time.
void DX12_ServiceDeferredECLProbe() {
    if (!dx12_hook_g_ProbeRealD3D12ECLDeferred.load(std::memory_order_acquire)) {
        return;
    }
    if (DXGIShared::IsStreamlineStartupTransitionWindowActive()) {
        return;
    }
    auto* srvDev = g_Device.load(std::memory_order_acquire);
    if (srvDev && IsStreamlineLoaded()) {
        ProbeRealD3D12ECL(srvDev);
        auto* srvProbed = (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
        dx12_hook_g_ProbeRealD3D12ECLDeferred.store(false, std::memory_order_release);
        HookLogImportant("DX12: ServiceDeferredECLProbe — realECL=%p", srvProbed);
    }
}

DWORD WINAPI UnloadThread(LPVOID lpParam) {
    Sleep(200);
    if (g_dx12HookInstance) {
        g_dx12HookInstance->Shutdown();
        delete g_dx12HookInstance;
        g_dx12HookInstance = nullptr;
    }
    return 0;
}
