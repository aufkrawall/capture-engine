#include "dx12_hook_internal.h"
#include "dx12_hook_process_session.h"

void ProcessFrame(IDXGISwapChain* pSwapChain, bool processCapture, bool applicationSourcePresent,
                  bool frameGenerationPresentationActive,
                  ce::dx12_process_frame_diagnostics::StageTimings* diagnostics) {
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
    FrameProcessSession session(pSwapChain, processCapture, applicationSourcePresent,
                               frameGenerationPresentationActive, diagnostics);
    session.Run();
    if (session.metricsGuardArmed) {
        session.LogFrameMetrics();
    }
}

void FrameProcessSession::Run() {
    ProcessFrameFlow flow = ProcessFrameFlow::kContinue;
    flow = Phase1();
    if (flow == ProcessFrameFlow::kReturn) {
        return;
    }
    flow = Phase2();
    if (flow == ProcessFrameFlow::kReturn) {
        return;
    }
    flow = Phase3();
    if (flow == ProcessFrameFlow::kReturn) {
        return;
    }
    flow = Phase4();
    if (flow == ProcessFrameFlow::kReturn) {
        return;
    }
    flow = Phase5();
    if (flow == ProcessFrameFlow::kReturn) {
        return;
    }
    flow = DrawOverlayFrame();
    if (flow == ProcessFrameFlow::kReturn) {
        return;
    }
    flow = Phase6Tail();
    if (flow == ProcessFrameFlow::kReturn) {
        return;
    }
}

ProcessFrameFlow FrameProcessSession::Phase1() {

    // Diagnostic: when the D3D12 debug layer is enabled (CE_DX12_DEBUG_LAYER), flush
    // its validation messages each frame so the overlay submit's messages (including
    // any hazard right before an Alt+Tab hang) reach the hook log. No-op when off.
    ce::dx12_dred::DrainDebugLayerMessages(g_Device.load(std::memory_order_acquire), "ProcessFrame");

    // Performance metrics for this frame
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
    activeDebugSample = PerfLogger::Get().GetActiveDebugSample();
    processFrameStartUs = activeDebugSample ? PerfLogger::GetQpcUs() : 0;

    // Scope guard to log metrics on any exit path
        metricsGuardArmed = true;

    // Skip performance logging if disabled
    if (!PerfLogger::Get().IsEnabled()) {
        metricsGuardArmed = false;
    }

    // CRITICAL: Skip all rendering during shutdown to prevent crashes
    if (HookIsShuttingDown()) {
            return ProcessFrameFlow::kReturn;
    }

    // Advanced DX12 focus/mode-switch analysis (config-gated by [Overlay] dx12_focus_analysis; off by
    // default). Runs before the device-removed early-return so the stall present and its residency
    // trajectory are captured. No-op when disabled.
    DX12_UpdateFocusAnalysis(g_IPC ? g_IPC->GetSharedMem() : nullptr);

    // Skip everything when device is removed — avoids reinit spam on a dead
    // device.  DX12_SetCommandQueue clears g_DeviceRemoved when a new device
    // arrives.
    if (dx12_hook_g_DeviceRemoved.load(std::memory_order_relaxed)) {
            return ProcessFrameFlow::kReturn;
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
            return ProcessFrameFlow::kReturn;
        }
    }

    protectedOfficialFFXStartupOverlayOnly = ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup();

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
            return ProcessFrameFlow::kReturn;
            }
        }
    }

    inResize = dx12_hook_g_InSwapchainResizeCleanup.load(std::memory_order_acquire);
    if (!pSwapChain || inResize) {
        HookLog("DX12: ProcessFrame - early return (null=%d, inResize=%d)", !pSwapChain, inResize);
            return ProcessFrameFlow::kReturn;
    }

    if (FAILED(pSwapChain->GetDesc(&frameDesc))) {
        HookLog("DX12: ProcessFrame - failed to get swapchain desc (precheck)");
            return ProcessFrameFlow::kReturn;
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

    hasOutputWindow = frameDesc.OutputWindow != nullptr;
    outputWindowVisible = hasOutputWindow && IsWindowVisible(frameDesc.OutputWindow) != FALSE;
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
            return ProcessFrameFlow::kReturn;
    }

    // Track HWND → swapchain mapping for FG-switch recovery
    if (frameDesc.OutputWindow) {
        TrackSwapchainHwnd(pSwapChain, frameDesc.OutputWindow);
    }

    zeroSizedSwapchain = (frameDesc.BufferDesc.Width == 0 || frameDesc.BufferDesc.Height == 0);
    iconicWindow = frameDesc.OutputWindow && IsIconic(frameDesc.OutputWindow);
    foregroundWindow = nullptr;
    foregroundPid = 0;
    currentProcessId = GetCurrentProcessId();
    processHasForeground = true;
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
    inTransitionCooldown = false;
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
    suspendOverlayHeavy =
        ce::dx12_overlay_policy::ShouldHeavySuspendDX12OverlayForSwapchainState(zeroSizedSwapchain, iconicWindow);
    suspendOverlayRender = suspendOverlayHeavy;

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
    prerenderLimit = GetActivePrerenderLimit();
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
    postFSRNormalRouteExplicitQueueProof = false;
    postFSRNormalRouteRememberedSwapchainProof = false;
    postFSRNormalRouteOwnershipProven = false;
    authoritativeDLSSOffNormalReturnReinitializedThisPresent = false;
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
            return ProcessFrameFlow::kReturn;
    }

    // PERFORMANCE FIX: Use try_lock instead of blocking lock_guard
    // This prevents stalling the render thread if another thread holds the lock
    if (!dx12_hook_g_OverlayMutex.try_lock()) {
        // Another thread is processing, skip this frame
        if (activeDebugSample) {
            activeDebugSample->flags |= kPresentSampleFlagMutexBusy;
        }
        HookLog("DX12: ProcessFrame - mutex busy, skipping frame");
            return ProcessFrameFlow::kReturn;
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
            return ProcessFrameFlow::kReturn;
        }
        lock.lock();
    }

    // SAFETY: Check device state after acquiring lock
    if (dx12_hook_g_InSwapchainResizeCleanup.load(std::memory_order_acquire)) {
        HookLog("DX12: ProcessFrame - in resize cleanup, returning");
            return ProcessFrameFlow::kReturn;
    }

    UpdateStartupOverlayCompatibilityState();
    allowOverlayRender = ApplyOverlayStartupCompatMode(frameDesc.OutputWindow);
    observerModeShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    observerOnlyMode = IsDX12ObserverOnlyModeActive(observerModeShm);
    observerPolicyOnlyMode = IsDX12ObserverPolicyOnlyModeActive(observerModeShm);
    observerStartupPresentOnlyMode = IsDX12ObserverStartupPresentOnlyModeActive(observerModeShm);
    if (observerOnlyMode) {
        allowOverlayRender = false;
        EnsurePostSLDisabledForObserverOnly(
            "DX12: ProcessFrame observer-only mode",
            ce::streamline_runtime_policy::ShouldPreserveObserverPolicyOnlyStartupTransitionWindow(
                observerOnlyMode, observerPolicyOnlyMode));
    }

    postResumeSettleRemainingMs = 0;
    startupOverlayCompatibilityActive = IsStartupOverlayCompatibilityActive();
    runtimeOwnedSwapchainActiveMs = (dx12_hook_g_FGRuntimeOwnsSwapchain && dx12_hook_g_FGRuntimeOwnsSwapchainSince != 0)
                                                        ? (GetTickCount64() - dx12_hook_g_FGRuntimeOwnsSwapchainSince)
                                                        : dx12_hook_kStartupOverlayPostResumeSettleMs;
    runtimeOwnedSwapchainNeedsExtraResumeSettle =
        ce::dx12_overlay_policy::ShouldDeferStartupOverlayWorkAfterResume(
            startupOverlayCompatibilityActive, dx12_hook_g_FGRuntimeOwnsSwapchain, runtimeOwnedSwapchainActiveMs,
            dx12_hook_kStartupOverlayPostResumeSettleMs, dx12_hook_s_startupOverlayCompatSettled.load(std::memory_order_acquire),
            ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff());
    deferOverlayWorkAfterResume = ShouldDelayOverlayInitAfterStartupResumeCompat(
        allowOverlayRender, frameDesc.OutputWindow, runtimeOwnedSwapchainNeedsExtraResumeSettle,
        &postResumeSettleRemainingMs);
    processNeedsStartupOverlayInitDelay = startupOverlayCompatibilityActive;
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

    exactPostDLSSOffNormalReturnSwapchainProof =
        dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.load(std::memory_order_acquire) == pSwapChain;
    exactPrewarmedPostSLHandoffSwapchainProof =
        dx12_hook_g_PrewarmedPostSLHandoffSwapchain.load(std::memory_order_acquire) == pSwapChain;
    processLogicalSwapchainReplacement = ce::dx12_overlay_policy::ShouldProcessLogicalSwapchainReplacement(
        pSwapChain != dx12_hook_g_LastSwapChain,
        exactPostDLSSOffNormalReturnSwapchainProof || exactPrewarmedPostSLHandoffSwapchainProof);
    return ProcessFrameFlow::kContinue;
}

void FrameProcessSession::LogFrameMetrics() {
if (activeDebugSample) {
    activeDebugSample->processFrameUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - processFrameStartUs);
    activeDebugSample->captureUs = perfMetrics.captureUs;
}
if (PerfLogger::Get().IsEnabled()) {
    perfMetrics.totalUs = static_cast<int32_t>((PerfLogger::GetQpcUs() - perfMetrics.qpcUs));
    perfMetrics.fpsLimitWaitUs = static_cast<int32_t>(g_SharedFpsLimiter.GetLastWaitUs());
    PerfLogger::Get().LogFrame(perfMetrics);
}
}
