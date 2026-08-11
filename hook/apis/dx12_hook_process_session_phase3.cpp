#include "dx12_hook_internal.h"
#include "dx12_hook_process_session.h"

ProcessFrameFlow FrameProcessSession::Phase3() {
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
startupOverlayPresent = startupOverlayCompatibilityActive;
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
        return ProcessFrameFlow::kSkipOverlayInit;
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
        return ProcessFrameFlow::kSkipOverlayInit;
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
        return ProcessFrameFlow::kSkipOverlayInit;
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
        return ProcessFrameFlow::kSkipOverlayInit;
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
        return ProcessFrameFlow::kReturn;
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
        return ProcessFrameFlow::kReturn;
    }

    if (dx12_hook_s_insideECL) {
        static std::atomic<int> s_initDeferredInEclLogCount{0};
        if (s_initDeferredInEclLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("DX12: Deferring overlay init while inside ExecuteCommandLists re-entry");
        }
        return ProcessFrameFlow::kReturn;
    }

    int frames = ++dx12_hook_s_framesBeforeInit;
    if (frames < 1) {
        return ProcessFrameFlow::kReturn;
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
        return ProcessFrameFlow::kReturn;
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
        return ProcessFrameFlow::kReturn;
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
        return ProcessFrameFlow::kReturn;
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
                if (SharedMemoryLayout* sharedMemory = GetHookSharedMemory()) {
                    sharedMemory->SetIsHDR(isActualHDR);
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
        return ProcessFrameFlow::kReturn;
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
    return ProcessFrameFlow::kContinue;
}
