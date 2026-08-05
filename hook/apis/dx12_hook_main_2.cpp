#include "dx12_hook_internal.h"
#include "dx12_hook_main_shared.h"


void DX12_OnStreamlineFGStateChanged(bool active) {
    const auto visibleRuntimeMode = active ? ce::fg_runtime::RuntimeMode::kDLSSFG : g_FGCompat.GetRuntimeMode();
    const bool visibleFGActive = active ? true : g_FGCompat.IsFGActive();
    HookUpdatePreferredOverlayFGPublicationState(visibleFGActive, visibleRuntimeMode,
                                                 "DX12_OnStreamlineFGStateChanged");

    ce::fg_session::EmitFGEvent(active ? ce::fg_session::FGEventKind::kStreamlineSetOptionsRuntimeUpdate
                                       : ce::fg_session::FGEventKind::kTransitionCooldownComplete,
                                "DX12_OnStreamlineFGStateChanged", nullptr, nullptr,
                                active ? ce::fg_runtime::RuntimeMode::kDLSSFG : g_FGCompat.GetRuntimeMode(), active,
                                HookHasExplicitStreamlineSetOptionsActivation());

    const bool observerOnly = HookOverlayObserverOnlyEnabled();
    const bool observerPolicyOnly = HookOverlayObserverPolicyOnlyEnabled();
    const bool observerStartupPresentOnly = HookOverlayObserverStartupPresentOnlyEnabled();
    if (observerOnly) {
        const auto cleanup =
            ce::streamline_runtime_policy::ResolveObserverOnlyHeuristicCleanupForStreamlineSignalTransition(active);
        if (cleanup.clearRecentTeardownGrace) {
            const int previousHeuristicGrace = dx12_hook_g_SLOffHeuristicGrace.exchange(0, std::memory_order_acq_rel);
            const int previousSwapchainGrace = dx12_hook_g_SLOffSwapchainReinitGrace.exchange(0, std::memory_order_acq_rel);
            if (ce::dx12_overlay_policy::ShouldClearRecentStreamlineTeardownGraceOnFreshActivation(
                    true, previousHeuristicGrace > 0, previousSwapchainGrace > 0)) {
                HookLogImportant(
                    "DX12: Observer-only Streamline FG ON - cleared stale teardown grace before fresh activation "
                    "(slOffGrace=%d swapchainGrace=%d)",
                    previousHeuristicGrace, previousSwapchainGrace);
            }
        }
        if (cleanup.seedRecentTeardownGrace) {
            dx12_hook_g_SLOffHeuristicGrace.store(600, std::memory_order_release);
            dx12_hook_g_SLOffSwapchainReinitGrace.store(300, std::memory_order_release);
        }
        if (cleanup.resetQueueChangeHeuristic) {
            RequestFGDetectionHeuristicReset();
        }
        if (cleanup.clearHeuristicFSR && g_FGCompat.IsHeuristicFSRFGActive()) {
            g_FGCompat.SetHeuristicFSRFGActive(false);
            HookLogImportant("DX12: Observer-only cleared heuristic FSR FG during Streamline %s transition",
                             active ? "ON" : "OFF");
        }
        if (cleanup.clearNvidiaSmoothMotion) {
            g_FGCompat.ClearNvidiaSMState();
        }
        if (active) {
            HookLogImportant(
                observerPolicyOnly
                    ? (observerStartupPresentOnly
                           ? "DX12: Streamline FG ON observed in observer-startup-present-only mode - keeping PostSL "
                             "and special Streamline Present routing passive while preserving startup-policy and "
                             "non-Streamline startup-Present probe state"
                           : "DX12: Streamline FG ON observed in observer-policy-only mode - keeping PostSL/startup "
                             "Present passive while preserving Streamline startup-policy state")
                    : "DX12: Streamline FG ON observed in observer-only mode - skipping PostSL startup routing/state "
                      "mutation");
        } else {
            HookLogImportant(
                observerPolicyOnly
                    ? (observerStartupPresentOnly
                           ? "DX12: Streamline FG OFF observed in observer-startup-present-only mode - keeping PostSL "
                             "and special Streamline Present routing passive while preserving startup-policy and "
                             "non-Streamline startup-Present probe state"
                           : "DX12: Streamline FG OFF observed in observer-policy-only mode - keeping PostSL/startup "
                             "Present passive while preserving Streamline startup-policy state")
                    : "DX12: Streamline FG OFF observed in observer-only mode - keeping PostSL disabled and clearing "
                      "startup state");
        }
        EnsurePostSLDisabledForObserverOnly(
            "DX12: observer-only mode",
            ce::streamline_runtime_policy::ShouldPreserveObserverPolicyOnlyStartupTransitionWindow(observerOnly,
                                                                                                   observerPolicyOnly));
        return;
    }

    if (active) {
        dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(nullptr, std::memory_order_release);
        const int previousHeuristicGrace = dx12_hook_g_SLOffHeuristicGrace.exchange(0, std::memory_order_acq_rel);
        const int previousSwapchainGrace = dx12_hook_g_SLOffSwapchainReinitGrace.exchange(0, std::memory_order_acq_rel);
        if (ce::dx12_overlay_policy::ShouldClearRecentStreamlineTeardownGraceOnFreshActivation(
                true, previousHeuristicGrace > 0, previousSwapchainGrace > 0)) {
            HookLogImportant(
                "DX12: Streamline FG ON — cleared stale teardown grace before fresh activation "
                "(slOffGrace=%d swapchainGrace=%d)",
                previousHeuristicGrace, previousSwapchainGrace);
        }

        const bool startupWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
        const ULONGLONG startupWindowRemainingMs =
            startupWindowActive
                ? (DXGIShared::g_SharedState.streamlineStartupTransitionUntilMs.load(std::memory_order_acquire) -
                   GetTickCount64())
                : 0;
        const bool startupTopLevelPresentConsumed =
            DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire);
        const bool wrapperProgressObserved =
            dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount.load(std::memory_order_acquire) > 0;
        HookLogImportant(
            "DX12: Streamline FG ON — GetState transition STARTING "
            "(startupWindowActive=%d startupRemaining=%lldms consumed=%d wrapperProgress=%d)",
            startupWindowActive ? 1 : 0, (long long)startupWindowRemainingMs, startupTopLevelPresentConsumed ? 1 : 0,
            wrapperProgressObserved ? 1 : 0);

        const bool callbackAlreadyInstalled =
            DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr;
        const bool resumeConfirmedPostSLFromKeepAlive =
            ce::dx12_overlay_policy::ShouldResumeConfirmedPostSLFromKeepAliveOnStreamlineOn(
                dx12_hook_g_PostSLExplicitOffKeepAlive.exchange(false, std::memory_order_acq_rel),
                dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire));
        dx12_hook_g_PostSLWarmResumePreservationPending.store(callbackAlreadyInstalled && resumeConfirmedPostSLFromKeepAlive,
                                                    std::memory_order_release);

        if (callbackAlreadyInstalled && resumeConfirmedPostSLFromKeepAlive) {
            // Suspend -> resume cycle bridged by the make-before-break
            // keep-alive: PostSL stayed confirmed-and-renderable the whole
            // time, so this is a RESUME of a continuously-live path, not a
            // cold start. No synthetic-startup pending dance, no countdown
            // re-arm, no lifecycle reset — the first re-entrant present after
            // the resume renders immediately.
            dx12_hook_g_PostSLCallbackExecutionEnabled.store(true, std::memory_order_release);
            dx12_hook_g_PostSLOverlayActive.store(true, std::memory_order_release);
            dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
            // Keep churn protection armed: a quick OFF right after this resume
            // must take the churn path, not a full teardown.
            DXGIShared::ArmStreamlineStartupTransitionWindow();
            HookLogImportant(
                "DX12: Streamline FG ON — warm PostSL resume from make-before-break keep-alive "
                "(confirmed rendering preserved, no countdown/warm-up re-arm)");
        } else if (callbackAlreadyInstalled) {
            dx12_hook_g_PostSLCallbackExecutionEnabled.store(true, std::memory_order_release);
            dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
            dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
            dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
            dx12_hook_g_PostSLStableFrameCount.store(0, std::memory_order_release);
            dx12_hook_g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
            dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(true, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount.store(0, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(true, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
            int cooldownLeft = dx12_hook_g_PostSLCooldownRemaining.load(std::memory_order_acquire);
            while (cooldownLeft < 60 && !dx12_hook_g_PostSLCooldownRemaining.compare_exchange_weak(
                                            cooldownLeft, 60, std::memory_order_acq_rel, std::memory_order_acquire)) {}
            HookLogImportant(
                "DX12: Streamline FG ON — re-enabling dormant PostSL callback for startup routing "
                "(churn re-activation, cooldown=%d)",
                dx12_hook_g_PostSLCooldownRemaining.load(std::memory_order_relaxed));
            // Re-arm the startup transition window: churn re-activation means
            // DLSS FG is still in its initialization dance (game bouncing
            // ON/OFF/ON).  The original window from the first ON may have expired,
            // leaving no protection for OFF signals during this new cycle.
            DXGIShared::ArmStreamlineStartupTransitionWindow();
            HookLogImportant("DX12: Streamline FG ON churn — re-armed startup transition window");
            ResetPostSLLifecycleForTransition("DX12: Streamline FG ON churn re-activation", true);
        } else {
            SetPostSLCallbackInstalled(true, "DX12: Streamline FG ON");
            HookLogImportant("DX12: Streamline FG ON — pre-armed PostSL callback for startup routing");
            int cooldownLeft = dx12_hook_g_PostSLCooldownRemaining.load(std::memory_order_acquire);
            while (cooldownLeft < 60 && !dx12_hook_g_PostSLCooldownRemaining.compare_exchange_weak(
                                            cooldownLeft, 60, std::memory_order_acq_rel, std::memory_order_acquire)) {}
            dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
            dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
            dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
            dx12_hook_g_PostSLStableFrameCount.store(0, std::memory_order_release);
            dx12_hook_g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
            dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(true, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount.store(0, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(true, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
            ResetPostSLLifecycleForTransition("DX12: Streamline FG ON transition", true);
        }
        if (dx12_hook_g_HadFSRFGPhase) {
            ID3D12CommandQueue* staleScQueue = nullptr;
            {
                std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
                const bool streamlineStartupHandoffPending =
                    DXGIShared::g_SharedState.streamlineStartupHandoffPending.load(std::memory_order_acquire);
                const bool explicitSetOptionsActivation = HookHasExplicitStreamlineSetOptionsActivation();
                ClearStaleNativeFGPresentOwnershipForStreamlineComebackLocked(
                    explicitSetOptionsActivation, false, "fresh Streamline active edge");
                if (ce::dx12_overlay_policy::ShouldClearSwapchainQueueAsStaleFSROwnershipOnStreamlineOn(
                        dx12_hook_g_HadFSRFGPhase, dx12_hook_g_SwapchainQueue != nullptr,
                        dx12_hook_g_SwapchainQueue != nullptr && dx12_hook_g_SwapchainQueue != dx12_hook_g_OriginalGameQueue,
                        streamlineStartupHandoffPending, resumeConfirmedPostSLFromKeepAlive)) {
                    staleScQueue = dx12_hook_g_SwapchainQueue;
                    dx12_hook_g_SwapchainQueue = nullptr;
                    dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
                    dx12_hook_g_SwapchainQueueCaptureTime = 0;
                    dx12_hook_g_FGRuntimeOwnsSwapchain = false;
                    DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                    ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
                    if (g_FGCompat.IsFSRFGApiActive()) {
                        SetNativeFSRStartupConfigureArmingPending(false,
                                                                  "Streamline FG comeback cleared FSR ownership");
                        ClearOfficialFFXRuntimeOwnedPresentPathAssumption(
                            "Streamline FG comeback cleared FSR ownership");
                        g_FGCompat.SetFSRFGActive(false);
                        g_FGCompat.SetFSRFGMultiplier(0);
                        ResetAuthoritativeFSRRealFrameOnlyStreak();
                    }
                    HookLogImportant(
                        "DX12: Streamline FG ON after FSR — cleared stale FSR swapchain queue %p (origGame=%p) "
                        "to prevent DEVICE_REMOVED on FSR→DLSS transition",
                        staleScQueue, dx12_hook_g_OriginalGameQueue);
                } else if (dx12_hook_g_SwapchainQueue && dx12_hook_g_SwapchainQueue != dx12_hook_g_OriginalGameQueue &&
                           streamlineStartupHandoffPending) {
                    if (ce::dx12_overlay_policy::ShouldInvalidatePostSLLastWorkingQueueOnFreshPostFSRStreamlineHandoff(
                            dx12_hook_g_HadFSRFGPhase, dx12_hook_g_SwapchainQueue != nullptr,
                            dx12_hook_g_SwapchainQueue != nullptr && dx12_hook_g_SwapchainQueue != dx12_hook_g_OriginalGameQueue,
                            streamlineStartupHandoffPending, dx12_hook_g_PostSLLastWorkingQueue != nullptr,
                            dx12_hook_g_SwapchainQueue != nullptr && dx12_hook_g_SwapchainQueue == dx12_hook_g_PostSLLastWorkingQueue)) {
                        HookLogImportant(
                            "DX12: Streamline FG ON after FSR — cleared stale PostSL lastWorking queue %p because "
                            "fresh Streamline handoff moved to new scQueue %p (origGame=%p)",
                            dx12_hook_g_PostSLLastWorkingQueue, dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue);
                        SetPostSLLastWorkingQueue(nullptr);
                    }
                    HookLogImportant(
                        "DX12: Streamline FG ON after FSR — preserving freshly handed-off Streamline swapchain queue "
                        "%p "
                        "during active startup handoff (origGame=%p)",
                        dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue);
                }
            }
            if (staleScQueue) {
                staleScQueue->Release();
            }
        }

        ID3D12CommandQueue* resumeSwapchainQueue = nullptr;
        ID3D12CommandQueue* resumeLastWorkingQueue = nullptr;
        ID3D12CommandQueue* resumeOriginalGameQueue = nullptr;
        ID3D12CommandQueue* resumeCommandQueue = nullptr;
        {
            std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
            resumeSwapchainQueue = dx12_hook_g_SwapchainQueue;
            resumeLastWorkingQueue = dx12_hook_g_PostSLLastWorkingQueue;
            resumeOriginalGameQueue = dx12_hook_g_OriginalGameQueue;
            resumeCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        }
        if (ce::dx12_overlay_policy::ShouldSeedStreamlineStartupBootstrapAsConsumedForConfirmedPostSLResume(
                dx12_hook_g_HadFSRFGPhase, resumeLastWorkingQueue != nullptr, resumeSwapchainQueue != nullptr,
                resumeSwapchainQueue != nullptr && resumeSwapchainQueue == resumeLastWorkingQueue,
                dx12_hook_g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.load(std::memory_order_acquire),
                resumeOriginalGameQueue != nullptr,
                resumeOriginalGameQueue != nullptr && resumeCommandQueue == resumeOriginalGameQueue)) {
            DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.store(true, std::memory_order_release);
            HookLogImportant(
                "DX12: Streamline FG ON — seeded startup bootstrap as already consumed for confirmed PostSL resume "
                "(scQueue=%p lastWorking=%p clearedStaleNoFG=%d origGame=%p cmdQ=%p)",
                resumeSwapchainQueue, resumeLastWorkingQueue,
                dx12_hook_g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.load(std::memory_order_relaxed) ? 1 : 0,
                resumeOriginalGameQueue, resumeCommandQueue);
        }
        dx12_hook_g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.store(false, std::memory_order_release);
        return;
    }
    if (!active) {
        dx12_hook_g_PrewarmedPostSLHandoffSwapchain.store(nullptr, std::memory_order_release);
    }

    // Make-before-break: a CONFIRMED PostSL path stays armed-and-rendering
    // across the explicit OFF edge — the proxy swapchain keeps presenting
    // after slDLSSGSetOptions(off) (menus/suspension) and tearing PostSL down
    // here is what blanks those presents until an authoritative normal
    // swapchain/queue return. Never while an FSR/native-FG takeover is in play
    // (the quiesce invariant wins).
    dx12_hook_g_PostSLWarmResumePreservationPending.store(false, std::memory_order_release);
    const bool keepConfirmedPostSLAliveAcrossOff =
        ce::dx12_overlay_policy::ShouldKeepConfirmedPostSLAliveAcrossStreamlineOff(
            dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire), g_FGCompat.IsFSRFGApiActive(),
            HookHasRuntimeOwnedNativeFGPresentPath(), ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup());
    if (keepConfirmedPostSLAliveAcrossOff) {
        dx12_hook_g_PostSLExplicitOffKeepAlive.store(true, std::memory_order_release);
        HookLogImportant(
            "DX12: Streamline FG OFF — keeping confirmed PostSL armed-and-rendering until an authoritative "
            "normal swapchain/queue return (make-before-break keep-alive)");
    } else {
        dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
    }

    const bool inStartupChurnWindow = DXGIShared::IsStreamlineStartupTransitionWindowActive();

    if (inStartupChurnWindow) {
        if (!keepConfirmedPostSLAliveAcrossOff) {
            dx12_hook_g_PostSLCallbackExecutionEnabled.store(false, std::memory_order_release);
        }
        HookLogImportant(
            "DX12: Streamline FG OFF during startup transition — keeping PostSL callback %s "
            "(churn suppression, epoch=%u keepAlive=%d)",
            keepConfirmedPostSLAliveAcrossOff ? "armed for keep-alive rendering" : "dormant",
            dx12_hook_g_PostSLLifecycleEpoch.load(std::memory_order_acquire), keepConfirmedPostSLAliveAcrossOff ? 1 : 0);
        // Drop the AddRef'd startup-activation swapchain even on the churn
        // path: pinning it costs nothing on a quick re-ON (every startup-route
        // present re-retains it), but if the game proceeds to a full native
        // teardown instead, CE's reference makes the app's
        // CreateSwapChainForHwnd on the same HWND fail E_ACCESSDENIED through
        // all retries (session 20260613_032326: DLSS->OFF stopped the app's
        // main loop with "no swapchain after OFF request").
        ReleaseStreamlineStartupActivationSwapchain("DX12: Streamline FG OFF (startup churn)");
        dx12_hook_g_SLOffHeuristicGrace.store(600, std::memory_order_release);
        RequestFGDetectionHeuristicReset();
        g_FGCompat.SetHeuristicFSRFGActive(false);
        if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
            const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
            ce::overlay_metrics::PublishOverlayFGMetrics(perf, plan, 0.0f, 0.0f, 1, "DX12_OnStreamlineFGStateChanged");
        }
        return;
    }


    DXGIShared::ResetStreamlineStartupTransitionState();
    if (!keepConfirmedPostSLAliveAcrossOff) {
        SetPostSLCallbackInstalled(false, "DX12: Streamline FG OFF");
        dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
    } else {
        HookLogImportant(
            "DX12: Streamline FG OFF — PostSL callback stays installed for make-before-break keep-alive "
            "(confirmed rendering preserved)");
    }
    dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
    dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
    dx12_hook_g_PostSLStableFrameCount.store(0, std::memory_order_release);
    dx12_hook_g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
    dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount.store(0, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
    ReleaseStreamlineStartupActivationSwapchain("DX12: Streamline FG OFF");
    dx12_hook_g_SLOffHeuristicGrace.store(600, std::memory_order_release);
    dx12_hook_g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.store(false, std::memory_order_release);
    if (dx12_hook_g_PostSLLastWorkingQueue) {
        MarkPostSLRecentTeardownActivity("DX12: Streamline FG OFF seeded recent PostSL teardown activity",
                                         dx12_hook_g_PostSLLastWorkingQueue);
    }
    RequestFGDetectionHeuristicReset();
    g_FGCompat.SetHeuristicFSRFGActive(false);
    g_FGCompat.ClearNvidiaSMState();
    if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
        const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
        ce::overlay_metrics::PublishOverlayFGMetrics(perf, plan, 0.0f, 0.0f, 1, "DX12_OnStreamlineFGStateChanged");
    }
    InvalidateAllOverlayCachedFrames();
    dx12_hook_g_SLOffSwapchainReinitGrace.store(300, std::memory_order_release);
    if (!keepConfirmedPostSLAliveAcrossOff) {
        // A lifecycle reset would force a fresh reactivation epoch/warm-up;
        // keep-alive must keep the continuously-live path untouched.
        ResetPostSLLifecycleForTransition("DX12: Streamline FG OFF transition", true, true);
    }

    if (dx12_hook_g_HadFSRFGPhase) {
        ID3D12CommandQueue* staleScQueue = nullptr;
        const auto runtimeMode = g_FGCompat.GetRuntimeMode();
        // Overlay fallback permission must not be reused as a proof that native
        // FSR ownership is stale.  Preserve ownership while FSR/native Present
        // state is still active and let the explicit native-FSR OFF path clear it.
        const bool preserveRuntimeOwnedFSRTakeover =
            ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
                dx12_hook_g_FGRuntimeOwnsSwapchain, false, runtimeMode, g_FGCompat.IsFSRFGApiActive(),
                HookHasRuntimeOwnedNativeFGPresentPath(), false);
        {
            std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);

            if (preserveRuntimeOwnedFSRTakeover) {
                HookLogImportant(
                    "DX12: Streamline FG OFF overlapped with authoritative/runtime-owned FSR takeover "
                    "(runtime=%s scQueue=%p origGame=%p) — preserving FSR swapchain ownership until native FSR "
                    "emits a stronger off signal",
                    ce::fg_runtime::GetRuntimeModeName(runtimeMode), dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue);
            } else if (dx12_hook_g_FGRuntimeOwnsSwapchain) {
                dx12_hook_g_FGRuntimeOwnsSwapchain = false;
                DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                dx12_hook_g_FGRuntimeOwnsSwapchainSince = 0;
                ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
                ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
                if (g_FGCompat.IsFSRFGApiActive()) {
                    SetNativeFSRStartupConfigureArmingPending(false, "Streamline FG off cleared FSR ownership");
                    ClearOfficialFFXRuntimeOwnedPresentPathAssumption("Streamline FG off cleared FSR ownership");
                    g_FGCompat.SetFSRFGActive(false);
                    g_FGCompat.SetFSRFGMultiplier(0);
                    ResetAuthoritativeFSRRealFrameOnlyStreak();
                }
                HookLogImportant("DX12: Streamline FG OFF after FSR history — clearing lingering FG runtime ownership");
            }

            if (!preserveRuntimeOwnedFSRTakeover && dx12_hook_g_SwapchainQueue && dx12_hook_g_SwapchainQueue != dx12_hook_g_OriginalGameQueue) {
                staleScQueue = dx12_hook_g_SwapchainQueue;
                dx12_hook_g_SwapchainQueue = nullptr;
                dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
                dx12_hook_g_SwapchainQueueCaptureTime = 0;
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — releasing stale swapchain queue %p so top-level "
                    "recovery can recapture the live non-FG queue (origGame=%p)",
                    staleScQueue, dx12_hook_g_OriginalGameQueue);
            }
        }

        if (staleScQueue) {
            staleScQueue->Release();
        }

        if (!preserveRuntimeOwnedFSRTakeover) {
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — leaving swapchain queue uncaptured until a live non-FG "
                "queue is observed again (origGame=%p primary=%p cmdQ=%p)",
                dx12_hook_g_OriginalGameQueue, dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire),
                g_CommandQueue.load(std::memory_order_acquire));
        }

        // A confirmed explicit-OFF keep-alive continues rendering on the exact
        // proxy swapchain/queue that succeeded one Present earlier. Preserve its
        // warm RTV/sync objects until a separately proven normal swapchain takes
        // ownership; tearing them down here defeats make-before-break and adds a
        // transition-time GPU drain/rebuild on the same route.
        const bool preserveConfirmedPostSLProxyResources =
            keepConfirmedPostSLAliveAcrossOff && dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit &&
            dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire) != nullptr;

        // The post-FSR DLSS path rendered through Streamline/PostSL against a
        // different swapchain topology than the resumed non-FG path. Force a
        // swapchain-level reinit only when no exact confirmed-proxy keep-alive
        // remains; the normal-return proof retires the preserved state later.
        if (!preserveRuntimeOwnedFSRTakeover && dx12_hook_g_State.overlayInit && !preserveConfirmedPostSLProxyResources) {
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — forcing overlay swapchain reinit for non-FG recovery");
            dx12_hook_g_State.overlayInit = false;
            CleanupRTVs();
        }

        if (!preserveRuntimeOwnedFSRTakeover && preserveConfirmedPostSLProxyResources) {
            dx12_hook_g_FGTransitionCooldown.store(0, std::memory_order_release);
            dx12_hook_g_PostSLCooldownRemaining.store(0, std::memory_order_release);
            dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG.store(true, std::memory_order_release);
            // The direct state-change callback already owns this OFF edge. Keep
            // the later ProcessFrame outer tracker from replaying a destructive
            // teardown once the native normal route eventually returns.
            dx12_hook_g_OuterTrackedSLFGRunning.store(false, std::memory_order_release);
            dx12_hook_g_OuterSLTransitionEpoch.fetch_add(1, std::memory_order_release);
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — preserved warm confirmed-PostSL proxy resources "
                "for exact-swapchain keep-alive (proxy=%p queue=%p; no reinit/copy/wait)",
                dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_relaxed), dx12_hook_g_PostSLLastWorkingQueue);
        } else if (!preserveRuntimeOwnedFSRTakeover &&
                   ce::dx12_overlay_policy::ShouldDeferOverlayReinitAfterDirectPostFSRStreamlineTeardown(
                       dx12_hook_g_HadFSRFGPhase, dx12_hook_g_State.overlayInit, dx12_hook_g_State.syncInit)) {
            dx12_hook_g_State.syncInit = false;
            dx12_hook_g_ResetReinitSubmitCounter.store(true, std::memory_order_release);
            dx12_hook_g_OuterTrackedSLFGRunning.store(false, std::memory_order_release);
            dx12_hook_g_OuterSLTransitionEpoch.fetch_add(1, std::memory_order_release);
            auto* oldRealECL = (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
            const ID3D12CommandQueue* currentPrimaryQueue = dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
            const ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
            const bool commandQueueSettledToPrimary =
                currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue;
            // DLSS-FG SUSPEND with FSR history (slDLSSGSetOptions(off), proxy stays live):
            // when the make-before-break keep-alive is armed this is a CONFIRMED PostSL
            // suspension, not a teardown. PostSL confirmed rendering means the overlay ECL on
            // the runtime-owned SL queue already succeeded many times this epoch (device
            // demonstrably healthy on that exact path) and the proxy keeps presenting, so
            // there is no Streamline teardown for the 60-frame deferral to wait for — it only
            // blanks a provably-live overlay (session 20260613_202646: 60-present /
            // confirmedDuringStreak=1 blank, gate=overlay-backend-uninitialized). The bypass
            // mirrors ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown but is gated on
            // keepConfirmedPostSLAliveAcrossOff (captured at function entry, BEFORE the teardown
            // above nulled the swapchain queue / cleared overlayInit) plus a device-health guard;
            // a real FSR/native-FG takeover or removed device keeps the strict cooldown.
            auto* suspendDevice = g_Device.load(std::memory_order_acquire);
            const bool suspendDeviceRemoved =
                suspendDevice != nullptr && FAILED(suspendDevice->GetDeviceRemovedReason());
            const bool confirmedPostSLSuspensionImmediateReinit =
                keepConfirmedPostSLAliveAcrossOff && !suspendDeviceRemoved;
            const bool useShortPostFSRCooldown = ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(
                commandQueueSettledToPrimary, dx12_hook_g_HadFSRFGPhase, true);
            const int cooldownFrames =
                confirmedPostSLSuspensionImmediateReinit ? 0 : (useShortPostFSRCooldown ? 15 : 60);
            dx12_hook_g_FGTransitionCooldown = ce::dx12_overlay_policy::ResolveTransitionCooldownFrames(
                dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), cooldownFrames,
                confirmedPostSLSuspensionImmediateReinit || useShortPostFSRCooldown);
            dx12_hook_g_PostSLCooldownRemaining.store(dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire),
                                            std::memory_order_release);
            dx12_hook_g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
            if (confirmedPostSLSuspensionImmediateReinit) {
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — confirmed-PostSL suspension (proxy stays live, "
                    "keep-alive armed), immediate warm overlay reinit instead of 60-frame blank (cooldown=%d)",
                    dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire));
            } else {
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — deferring non-FG overlay reinit for %d frames so "
                    "Talos/Streamline teardown can settle before pre-SL resources are rebuilt",
                    dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire));
            }
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — invalidated sync resources for delayed reinit");
            if (ce::dx12_overlay_policy::ShouldPreserveRealECLForDelayedPostFSRNonFGRecovery(
                    commandQueueSettledToPrimary, dx12_hook_g_HadFSRFGPhase)) {
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — preserving realECL %p for delayed non-FG "
                    "recovery because cmdQ=%p already settled to primary",
                    oldRealECL, currentCommandQueue);
            } else {
                dx12_hook_g_RealD3D12ECL.store(nullptr, std::memory_order_release);
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — cleared realECL %p for delayed non-FG recovery",
                    oldRealECL);
            }
            dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG = true;
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — enabled offscreen overlay compositing for non-FG "
                "recovery (backbuffer state indeterminate after FG teardown)");
        }
    }

    HookLogImportant("DX12: Streamline FG OFF — seeded heuristic reset/grace (slOffGrace=600)");
    HookLogImportant("DX12: Streamline FG OFF — applied PostSL callback/keep-alive state");
}

extern "C" __declspec(dllexport) bool DX12_FlushDeferredSignalWithInfo(
    ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo* outInfo) {
    if (outInfo) {
        *outInfo = {};
    }

    UINT64 deferredVal = dx12_hook_g_deferredSignalValue.load(std::memory_order_acquire);
    if (outInfo) {
        outInfo->hadDeferredSignal = (deferredVal != 0);
        outInfo->hasFence = (dx12_hook_g_State.fence != nullptr);
        outInfo->hasFenceEvent = (dx12_hook_g_State.fenceEvent != nullptr);
        outInfo->fence = dx12_hook_g_State.fence;
        outInfo->fenceEvent = dx12_hook_g_State.fenceEvent;
        outInfo->fenceValue = deferredVal;
        outInfo->completedValue = dx12_hook_g_State.fence ? dx12_hook_g_State.fence->GetCompletedValue() : 0;
    }
    if (deferredVal == 0 || !dx12_hook_g_State.fence) {
        return false;
    }

    // Use the queue that actually submitted the overlay ECL.  When FG runtimes
    // create swapchains with their own queue, this may differ from g_CommandQueue.
    ID3D12CommandQueue* q = dx12_hook_g_deferredSignalQueue.load(std::memory_order_acquire);
    if (!q)
        q = g_CommandQueue.load(std::memory_order_acquire);
    if (outInfo) {
        outInfo->queue = q;
    }
    if (!q) {
        return false;
    }

    HRESULT hr = q->Signal(dx12_hook_g_State.fence, deferredVal);
    if (outInfo) {
        outInfo->signalHr = hr;
        outInfo->signalSucceeded = SUCCEEDED(hr);
    }
    if (SUCCEEDED(hr)) {
        int allocIdx = dx12_hook_g_deferredSignalAllocIdx.load(std::memory_order_acquire);
        dx12_hook_g_State.currentFenceValue = deferredVal;
        if (allocIdx >= 0 && allocIdx < (int)dx12_hook_g_State.fenceValues.size())
            dx12_hook_g_State.fenceValues[allocIdx] = deferredVal;
        if (outInfo) {
            outInfo->completedValue = dx12_hook_g_State.fence->GetCompletedValue();
        }
    }
    dx12_hook_g_deferredSignalValue.store(0, std::memory_order_release);
    dx12_hook_g_deferredSignalAllocIdx.store(-1, std::memory_order_release);
    dx12_hook_g_deferredSignalQueue.store(nullptr, std::memory_order_release);
    return SUCCEEDED(hr);
}

extern "C" __declspec(dllexport) void DX12_FlushDeferredSignal() {
    DX12_FlushDeferredSignalWithInfo(nullptr);
}

const char* DescribeFocusLossPostPresentFenceSkip(
    const ce::dx12_overlay_policy::D3D12FocusLossOverlayFenceWaitContext& ctx,
    const ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo& info) {
    if (!ctx.isD3D12Swapchain)
        return "non-DX12";
    if (ctx.isFullscreen)
        return "fullscreen";
    if (ctx.processHasForeground)
        return "foreground";
    if (ctx.isIconic)
        return "iconic";
    if (ctx.hasZeroSize)
        return "zero-sized";
    if (!ctx.presentSucceeded)
        return "present-failed";
    if (ctx.presentDeviceLost)
        return "present-device-lost";
    if (ctx.frameGenerationActive)
        return "frame-generation-active";
    if (ctx.runtimeOwnedPresentation)
        return "runtime-owned-presentation";
    if (ctx.usingDedicatedQueue)
        return "dedicated-queue";
    if (!info.hadDeferredSignal)
        return "no-deferred-overlay-signal";
    if (!info.signalSucceeded)
        return "signal-failed";
    if (!info.hasFence)
        return "no-fence";
    if (!info.hasFenceEvent)
        return "no-fence-event";
    if (info.fenceValue == 0)
        return "zero-fence-value";
    return "policy";
}

