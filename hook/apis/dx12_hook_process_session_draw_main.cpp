#include "dx12_hook_internal.h"
#include "dx12_hook_process_session.h"

ProcessFrameFlow FrameProcessSession::DrawCooldownAndRoute() {
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
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawMain() {
    ProcessFrameFlow flow = ProcessFrameFlow::kContinue;
        if (!skipOverlayDraw) {
    flow = DrawSkipAndCounters();
    if (flow != ProcessFrameFlow::kContinue) {
        return flow;
    }
    flow = DrawDeviceScope();
    if (flow != ProcessFrameFlow::kContinue) {
        return flow;
    }
        }  // end !skipOverlayDraw
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawSkipAndCounters() {
skipSeparateOverlayGpuReason = nullptr;
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
    // Native-FSR-FG layering exception for a foreign Present chain: the FFX present callback
    // composites CE's overlay before the runtime presents the output through DXGI, so Steam/RTSS
    // (which patch that present) draw on top of CE. CE's deep body hook runs on the same present
    // after all of them; draw a second, topmost composite onto the presented swapchain backbuffer
    // via the teardown-safe owner-queue renderer. The callback draw stays as the guaranteed
    // baseline, so a refusal here can never hide the overlay.
    if (TryCompositeOverlayBelowForeignChainForRuntimeOwnedFSR()) {
        return ProcessFrameFlow::kOverlayDone;
    }
return ProcessFrameFlow::kSkipOverlayDraw;
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
return ProcessFrameFlow::kSkipOverlayDraw;
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
return ProcessFrameFlow::kSkipOverlayDraw;
    }
}

// Periodic health log for debugging stability
static std::atomic<uint64_t> s_overlayFrameCount{0};
frameNum = s_overlayFrameCount.fetch_add(1, std::memory_order_relaxed) + 1;
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
return ProcessFrameFlow::kOverlayDone;
    } else if (dx12_hook_g_DeviceRemoved.load(std::memory_order_relaxed)) {
        // Device recovered (new device set via DX12_SetCommandQueue)
        dx12_hook_g_DeviceRemoved.store(false, std::memory_order_release);
        DXGIShared::g_SharedState.deviceRemovedFatal.store(false, std::memory_order_release);
        g_RenderWatchdog.SetForceMonitor(false);
        ce::dx12_dred::ResetDumpEpoch();
        HookLogImportant("DX12: Device recovered — overlay will reinitialize");
    }
}
    return ProcessFrameFlow::kContinue;
}

bool FrameProcessSession::TryCompositeOverlayBelowForeignChainForRuntimeOwnedFSR() {
    const bool foreignOverlayLoaded =
        ce::overlay_compat::CountLoadedTrackedOverlayModules(ce::overlay_compat::TrackedOverlaySubset::kOverlay) > 0;
    const bool nativeFSRActive =
        g_FGCompat.IsFSRFGApiActive() || ce::fg_runtime::RuntimeModeUsesFSR(g_FGCompat.GetRuntimeMode());
    const bool submitQueueIsSwapchainQueue = gameQueue != nullptr && gameQueue == dx12_hook_g_SwapchainQueue;
    const auto decision = ce::dx12_overlay_policy::DecideBelowForeignChainFSRDeepDraw(
        DXGIShared::IsPresentInterceptedBelowForeignChain(), foreignOverlayLoaded, nativeFSRActive,
        HookHasRuntimeOwnedNativeFGPresentPath(),
        dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
        dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire) != 0, IsFFXPresentCallbackStalled(),
        dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire),
        ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup(), dx12_hook_g_SwapchainQueue != nullptr,
        submitQueueIsSwapchainQueue, dx12_hook_g_DeviceRemoved.load(std::memory_order_relaxed), HookIsShuttingDown());
    if (decision == ce::dx12_overlay_policy::BelowForeignChainFSRDeepDrawDecision::kUnavailable) {
        return false;
    }
    return DX12_CompositeOverlayBelowForeignChainForRuntimeOwnedFSR(pSwapChain, gameQueue);
}

ProcessFrameFlow FrameProcessSession::DrawDeviceScope() {
    ProcessFrameFlow flow = ProcessFrameFlow::kContinue;
            {
    flow = DrawAllocSetup();
    if (flow != ProcessFrameFlow::kContinue) {
        return flow;
    }
    flow = DrawListAndAlloc();
    if (flow != ProcessFrameFlow::kContinue) {
        return flow;
    }
            }  // end device-removed-check scope
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawAllocSetup() {
    allocatorPoolSize = static_cast<int>(dx12_hook_g_State.allocators.size());
    if (allocatorPoolSize <= 0) {
return ProcessFrameFlow::kOverlayDone;
    }

    idx = dx12_hook_g_State.allocIndex % allocatorPoolSize;
    dx12_hook_g_State.allocIndex = (idx + 1) % allocatorPoolSize;

    // With 16 allocators, we never need to wait under normal conditions.
    // However, during Alt+Tab / GPU throttle, the GPU may stall and the
    // fence value for this allocator slot won't advance.  We must check
    // before Reset() to avoid undefined behaviour (driver hang / crash).
    list = dx12_hook_g_State.cmdList;
    alloc = (idx < (int)dx12_hook_g_State.allocators.size()) ? dx12_hook_g_State.allocators[idx] : nullptr;
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawListAndAlloc() {
    ProcessFrameFlow flow = ProcessFrameFlow::kContinue;
    if (list && alloc) {
    flow = DrawAllocReset();
    if (flow != ProcessFrameFlow::kContinue) {
        return flow;
    }
    flow = DrawReset();
    if (flow != ProcessFrameFlow::kContinue) {
        return flow;
    }
    } else {
        flow = DrawNullList();
        if (flow != ProcessFrameFlow::kContinue) {
            return flow;
        }
    }
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawAllocReset() {
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
return ProcessFrameFlow::kOverlayDone;
            }
        }
        allocResetHr = alloc->Reset();
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawReset() {
    ProcessFrameFlow flow = ProcessFrameFlow::kContinue;
    if (SUCCEEDED(allocResetHr)) {
    flow = DrawResetFront();
    if (flow != ProcessFrameFlow::kContinue) {
        return flow;
    }
    flow = DrawSubmit();
    if (flow != ProcessFrameFlow::kContinue) {
        return flow;
    }
    } else {
        flow = DrawResetElse();
        if (flow != ProcessFrameFlow::kContinue) {
            return flow;
        }
    }
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawResetFront() {
listResetHr = list->Reset(alloc, nullptr);
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
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawSubmit() {
    ProcessFrameFlow flow = ProcessFrameFlow::kContinue;
    if (SUCCEEDED(listResetHr)) {
    flow = DrawSubmitSetup();
    if (flow != ProcessFrameFlow::kContinue) {
        return flow;
    }
    flow = DrawSc3();
    if (flow != ProcessFrameFlow::kContinue) {
        return flow;
    }
    } else {
        flow = DrawSubmitElse();
        if (flow != ProcessFrameFlow::kContinue) {
            return flow;
        }
    }
    return ProcessFrameFlow::kContinue;
}
