
    std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);

    const bool windowStillActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
    const bool activationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool callbackInstalled = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr;
    const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
    const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
    const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
    const bool postSLConfirmedButStartupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
    const bool postSLConfirmedButRuntimeStateStabilizing = HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing() ||
                                                           HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected();
    const bool explicitSetOptionsActivationForCurrentComeback =
        g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
    const bool hadFSRFGPhase = HookHasFSRFGHistory();
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    const bool startupProtectedComebackProof =
        explicitSetOptionsActivationForCurrentComeback || safePostFSRBootstrapPath;
    const bool postSLConfirmedButOffChurnAwaitingActiveProof = IsStartupProtectedOffChurnAwaitingActiveProof(
        startupProtectedComebackProof, postSLConfirmedRendering, postSLConfirmedButStartupSettling);
    const bool effectivePostSLRuntimeStateStabilizing =
        postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof;
    const bool acceptActivatedUnconfirmedResumeOff =
        ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
            true, windowStillActive, startupProtectedComebackProof, activationPending, postSLActiveButUnconfirmed,
            postSLStartupActivationEntered, postSLConfirmedRendering, postSLConfirmedButStartupSettling,
            effectivePostSLRuntimeStateStabilizing);
    const bool shouldKeepDeferred =
        !acceptActivatedUnconfirmedResumeOff &&
        ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
            windowStillActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
            activationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering, postSLConfirmedButStartupSettling,
            effectivePostSLRuntimeStateStabilizing);
    if (shouldKeepDeferred) {
        if (ce::dx12_overlay_policy::ShouldServicePostSLStartupActivationWhileOffChurnDeferred(
                shouldKeepDeferred, windowStillActive, activationPending, postSLStartupActivationEntered,
                callbackInstalled)) {
            const bool serviced = TryServicePostSLStartupActivation(
                "StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded deferred OFF churn", true);
            static std::atomic<int> s_deferredOffServiceLogCount{0};
            const int logCount = s_deferredOffServiceLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10 || (logCount % 100) == 0) {
                HookLogImportant(
                    "Streamline Hook: Startup-protected OFF churn serviced PostSL startup activation before "
                    "remaining deferred (serviced=%d pending=%d activeButUnconfirmed=%d "
                    "startupActivationEntered=%d)",
                    serviced ? 1 : 0, activationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                    postSLStartupActivationEntered ? 1 : 0);
            }
        }
        return;
    }

    const bool shouldTriggerDirectCallback =
        ce::streamline_runtime_policy::ShouldTriggerDirectPostSLCallbackAfterStartupWindowExpiry(
            activationPending, postSLStartupActivationEntered);

    auto logSkippedDirectCallbackAfterActivation = [&]() {
        static std::atomic<int> s_skipLogCount{0};
        const int logCount = s_skipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Startup window expired but PostSL startup activation callback already entered — "
                "skipping redundant direct callback until first confirmed render "
                "(activeButUnconfirmed=%d startupActivationEntered=%d)",
                postSLActiveButUnconfirmed ? 1 : 0, postSLStartupActivationEntered ? 1 : 0);
        }
    };

    // Case 1: Suppressed OFF exists — either forward it to Streamline for a real
    // inactive edge, or drop it if a newer post-FSR comeback is already
    // startup-protected and this OFF is now stale churn.
    if (g_SuppressedSetOptionsOffDuringStartup) {
        if (acceptActivatedUnconfirmedResumeOff) {
            LogAcceptedOffDuringActivatedUnconfirmedResume(
                "periodic suppressed-off flush", windowStillActive, hadFSRFGPhase,
                explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath, activationPending,
                postSLActiveButUnconfirmed, postSLStartupActivationEntered, postSLConfirmedRendering,
                postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
        }
        if (!acceptActivatedUnconfirmedResumeOff &&
            ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedStreamlineComeback(
                hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
                DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire), postSLConfirmedButStartupSettling,
                effectivePostSLRuntimeStateStabilizing)) {
            LogDroppedSuppressedOffForStartupProtectedStreamlineComeback(
                g_SuppressedOffViewportKey, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                safePostFSRBootstrapPath, activationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
                postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
            g_SuppressedSetOptionsOffDuringStartup = false;
            ResetStartupProtectedOffChurnActiveProof("dropped stale suppressed OFF after active proof");
        } else {
            auto originalSetOptions = GetCallableOriginalDLSSGSetOptions();
            if (!originalSetOptions) {
                return;
            }
            HookLogImportant(
                "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) via periodic flush — startup window "
                "expired (viewport=%u, activationPending=%d settling=%d stabilizing=%d activeProofPending=%d)",
                g_SuppressedOffViewportKey, activationPending ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
                effectivePostSLRuntimeStateStabilizing ? 1 : 0, postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0);
            const slResult offResult = originalSetOptions(g_SuppressedOffViewport, g_SuppressedOffOptions);
            if (offResult != kSlResultOk) {
                HookLogImportant("Streamline Hook: Forwarded slDLSSGSetOptions(OFF) via periodic flush returned %d",
                                 offResult);
            }
            g_SuppressedSetOptionsOffDuringStartup = false;
            ResetStartupProtectedOffChurnActiveProof("forwarded suppressed OFF after startup expiry");
        }

        // When the startup-handoff Present was promoted to top-level and bypassed
        // the synthetic Present path, the PostSL callback may never fire through
        // DetourPresent/DetourPresent1 — or it may be deferred by the startup
        // transition window guard.  In either case, activation remains pending.
        //
        // If activation is still pending, the suppressed OFF we just forwarded to
        // Streamline will tear down FG without PostSL ever completing.  Trigger the
        // PostSL callback directly so CE can at least attempt to complete activation
        // before Streamline receives the OFF signal and potentially destabilizes its
        // FG pipeline.
        //
        // We intentionally distinguish ProcessFrame pre-arming PostSL from the
        // startup callback actually entering.  Pre-armed-but-unentered still needs
        // the retained activation wake path; once the callback entered, repeated
        // direct callbacks stay blocked until the normal Present route confirms.
        //
        // This is critical for GTA V Enhanced DLSS FG startup, where only one
        // startup-handoff Present arrives via the top-level path (bypassing the
        // synthetic route), and then the game's present thread stalls inside Streamline
        // before any synthetic Presents can drive PostSL activation.
        if (shouldTriggerDirectCallback && callbackInstalled) {
            HookLogImportant(
                "Streamline Hook: Activation still pending after OFF flush — "
                "PostSL callback never entered (deferred or bypassed); trigger direct "
                "callback to attempt activation before Streamline processes OFF");
            const bool serviced = TryServicePostSLStartupActivation(
                "StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded after OFF flush", true);
            HookLogImportant("Streamline Hook: PostSL startup activation service after OFF flush returned %d",
                             serviced ? 1 : 0);
        } else if (activationPending && callbackInstalled && postSLStartupActivationEntered) {
            logSkippedDirectCallbackAfterActivation();
        }
        return;
    }

    // Case 2: No suppressed OFF, but startup window just expired and activation
    // is still pending.  This covers the scenario where ON re-arrived and cleared
    // the suppressed OFF before the window expired — ProcessFrame has stalled,
    // so the deferred PostSL callback in ProcessFrame will never fire.  Trigger
    // it here to complete activation before Streamline times out.
    if (shouldTriggerDirectCallback && callbackInstalled) {
        HookLogImportant(
            "Streamline Hook: Startup window expired with activation pending but no "
            "suppressed OFF — triggering PostSL callback directly to complete "
            "activation before Streamline times out");
        const bool serviced =
            TryServicePostSLStartupActivation("StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded expiry", true);
        HookLogImportant("Streamline Hook: PostSL startup activation service after startup expiry returned %d",
                         serviced ? 1 : 0);
    } else if (activationPending && callbackInstalled && postSLStartupActivationEntered) {
        logSkippedDirectCallbackAfterActivation();
    }
}

}  // namespace StreamlineHook
