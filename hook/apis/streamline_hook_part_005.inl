                state.inputsProcessingCompletionFence,
                (unsigned long long)state.lastPresentInputsProcessingCompletionFenceValue,
                static_cast<unsigned long long>(sleepCount),
                static_cast<unsigned long long>(sleepCount - sleepCountAtLastLog),
                static_cast<unsigned long long>(sleepLastMs ? (nowMs - sleepLastMs) : 0),
                static_cast<unsigned long long>(reflexOptCount),
                static_cast<unsigned long long>(reflexOptLastMs ? (nowMs - reflexOptLastMs) : 0),
                g_ReflexLastForwardedMode.load(std::memory_order_relaxed),
                g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0);
        }
    } else if (result == kSlResultOk && options != nullptr && options->mode == 0) {
        // Explicit OFF request: end any pending not-interpolating streak so a later re-enable starts a
        // fresh, correctly-attributed streak.
        g_DLSSGNotInterpolatingStreak.store(0, std::memory_order_relaxed);
    }
    if (runtimeEvaluation.update.shouldUpdate) {
        UpdateViewportRuntimeState(viewportKey, runtimeEvaluation.update.active, runtimeEvaluation.update.multiplier,
                                   runtimeEvaluation.update.generatedFrames, runtimeEvaluation.update.capabilityMax,
                                   "GetState", clearAllViewportStatesForDisable);
    } else if (result == kSlResultOk && options != nullptr && runtimeEvaluation.suppressedFreshActivation) {
        static std::atomic<int> s_recentFfxTakeoverSuppressedGetStateLogCount{0};
        const int logCount = s_recentFfxTakeoverSuppressedGetStateLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 128) == 0) {
            const ULONGLONG suppressUntilMs = g_SuppressNewGetStateActivationUntilMs.load(std::memory_order_acquire);
            const ULONGLONG nowMs = GetTickCount64();
            const ULONGLONG remainingMs = suppressUntilMs > nowMs ? (suppressUntilMs - nowMs) : 0;
            const bool persistentBlock =
                g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.load(std::memory_order_acquire);
            const ULONGLONG startupTransitionUntilMs =
                DXGIShared::g_SharedState.streamlineStartupTransitionUntilMs.load(std::memory_order_acquire);
            const bool startupWindowActive = startupTransitionUntilMs != 0 && startupTransitionUntilMs > nowMs;
            const ULONGLONG startupRemainingMs = startupWindowActive ? (startupTransitionUntilMs - nowMs) : 0;
            HookLogImportant(
                "Streamline Hook: Suppressing fresh GetState DLSS FG reactivation "
                "(viewport=%u mode=%u generated=%u fence=%p fenceValue=%llu persistentBlock=%d startupWindow=%d "
                "startupRemaining=%llums remaining=%llums)",
                viewportKey, options->mode, options->numFramesToGenerate, state.inputsProcessingCompletionFence,
                (unsigned long long)state.lastPresentInputsProcessingCompletionFenceValue, persistentBlock ? 1 : 0,
                startupWindowActive ? 1 : 0, (unsigned long long)startupRemainingMs, (unsigned long long)remainingMs);
        }
    }

    if (!IsObserverOnlyModeActive()) {
        std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);
        const bool startupWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
        const bool startupActivationPending =
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
        const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
        const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
        const bool postSLConfirmedButStartupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
        const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
        const bool postSLConfirmedButRuntimeStateStabilizing =
            HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing() ||
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
        const bool currentGetStateReportsInactive =
            runtimeEvaluation.update.shouldUpdate && !runtimeEvaluation.update.active;
        const bool acceptActivatedUnconfirmedResumeOff =
            ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
                currentGetStateReportsInactive, startupWindowActive, startupProtectedComebackProof,
                startupActivationPending, postSLActiveButUnconfirmed, postSLStartupActivationEntered,
                postSLConfirmedRendering, postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
        const bool shouldKeepDeferred =
            !acceptActivatedUnconfirmedResumeOff &&
            ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
                startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                postSLConfirmedRendering, postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
        if (g_SuppressedSetOptionsOffDuringStartup && !shouldKeepDeferred) {
            if (acceptActivatedUnconfirmedResumeOff) {
                LogAcceptedOffDuringActivatedUnconfirmedResume(
                    "GetState/suppressed-off-flush", startupWindowActive, hadFSRFGPhase,
                    explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath, startupActivationPending,
                    postSLActiveButUnconfirmed, postSLStartupActivationEntered, postSLConfirmedRendering,
                    postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
            }
            if (!acceptActivatedUnconfirmedResumeOff &&
                ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedStreamlineComeback(
                    hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                    postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing)) {
                LogDroppedSuppressedOffForStartupProtectedStreamlineComeback(
                    g_SuppressedOffViewportKey, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                    safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                    postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                    effectivePostSLRuntimeStateStabilizing);
            } else if (auto originalSetOptions = GetCallableOriginalDLSSGSetOptions()) {
                HookLogImportant(
                    "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) via GetState — startup window "
                    "expired (viewport=%u settling=%d stabilizing=%d activeProofPending=%d)",
                    g_SuppressedOffViewportKey, postSLConfirmedButStartupSettling ? 1 : 0,
                    effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                    postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0);
                const slResult offResult = originalSetOptions(g_SuppressedOffViewport, g_SuppressedOffOptions);
                if (offResult != kSlResultOk) {
                    HookLogImportant("Streamline Hook: Forwarded slDLSSGSetOptions(OFF) via GetState returned %d",
                                     offResult);
                }
            }
            g_SuppressedSetOptionsOffDuringStartup = false;
        }
    }

    // SL may overwrite our Present vtable hook asynchronously during FG
    // activation (not necessarily inside slDLSSGSetOptions).  This check
    // runs every frame the game polls FG state and will re-patch if needed.
    // Skip vtable repair while PostSL has not yet confirmed rendering, to
    // avoid calling through Steam's overlay hook chain during SL's DllMain
    // (which can crash gameoverlayrenderer64 with a null pointer).
    if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) && HookIsPostSLOverlayConfirmedRendering()) {
        DXGIShared::RepairVTableHooksIfNeeded();
    }

    return result;
}

slResult Hooked_slDLSSGSetOptions(const slViewportHandle& viewport, const slDLSSGOptions& options) {
    auto originalSetOptions = GetCallableOriginalDLSSGSetOptions();
    if (!originalSetOptions) {
        return kSlResultErrorInvalidState;
    }

    slDLSSGOptions adjustedOptions = CloneDLSSGOptions(options);
    const uint32_t viewportKey = GetViewportKey(viewport);
    const int configuredFactor = NormalizeDLSSFGFactor(GetActiveGraphicsConfig().parsed.dlssFGFactor);
    const uint32_t originalGeneratedFrames = options.numFramesToGenerate;
    const bool requestedEnabled = ce::streamline_runtime_policy::IsDLSSGModeEnabled(options.mode);
    const bool requestedDisabled = !requestedEnabled;

    uint32_t capabilityMax = GetCachedCapabilityMax(viewportKey);
    bool overrideApplied = false;
    bool overrideClamped = false;

    if (configuredFactor > 0 && requestedEnabled) {
        const uint32_t desiredGeneratedFrames = DLSSFGMultiplierToGeneratedFrames(configuredFactor);
        if (capabilityMax == 0 && desiredGeneratedFrames > 1) {
            capabilityMax = QueryCapabilityMax(viewport, &adjustedOptions);
        }

        uint32_t finalGeneratedFrames = desiredGeneratedFrames;
        if (capabilityMax > 0 && finalGeneratedFrames > capabilityMax) {
            finalGeneratedFrames = capabilityMax;
            overrideClamped = true;
        }

        if (finalGeneratedFrames > 0 && finalGeneratedFrames != adjustedOptions.numFramesToGenerate) {
            adjustedOptions.numFramesToGenerate = finalGeneratedFrames;
            overrideApplied = true;
        }
    }

    const bool pureObserverOnly = ShouldKeepPureObserverOnlyStreamlineBehavior();

    if (!pureObserverOnly && requestedEnabled) {
        std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);
        if (g_SuppressedSetOptionsOffDuringStartup) {
            const bool activationPending =
                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
            HookLogImportant(
                "Streamline Hook: Clearing suppressed slDLSSGSetOptions(OFF) due to explicit re-enable request "
                "(viewport=%u) — Streamline never received the OFF, re-enable is consistent "
                "(activationPending=%d)",
                viewportKey, activationPending ? 1 : 0);
            g_SuppressedSetOptionsOffDuringStartup = false;
        }
    }

    if (!pureObserverOnly) {
        std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);
        const bool startupWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
        const bool startupActivationPending =
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
        const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
        const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
        const bool postSLConfirmedButStartupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
        const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
        const bool postSLConfirmedButRuntimeStateStabilizing =
            HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing() ||
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
        const bool explicitSetOptionsDisableIsAuthoritative =
            ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
                requestedDisabled, true, postSLConfirmedRendering, startupActivationPending, postSLActiveButUnconfirmed,
                postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing,
                g_AcceptedRuntimeOffAwaitingSetOptions.load(std::memory_order_acquire));
        const bool acceptActivatedUnconfirmedResumeOff =
            ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
                requestedDisabled, startupWindowActive, startupProtectedComebackProof, startupActivationPending,
                postSLActiveButUnconfirmed, postSLStartupActivationEntered, postSLConfirmedRendering,
                postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
        const bool shouldKeepDeferred =
            !explicitSetOptionsDisableIsAuthoritative && !acceptActivatedUnconfirmedResumeOff &&
            ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
                startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                postSLConfirmedRendering, postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
        if (g_SuppressedSetOptionsOffDuringStartup && !shouldKeepDeferred) {
            if (acceptActivatedUnconfirmedResumeOff) {
                LogAcceptedOffDuringActivatedUnconfirmedResume(
                    "SetOptions/suppressed-off-flush", startupWindowActive, hadFSRFGPhase,
                    explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath, startupActivationPending,
                    postSLActiveButUnconfirmed, postSLStartupActivationEntered, postSLConfirmedRendering,
                    postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
            }
            if (!acceptActivatedUnconfirmedResumeOff &&
                ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedStreamlineComeback(
                    hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                    postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing)) {
                LogDroppedSuppressedOffForStartupProtectedStreamlineComeback(
                    g_SuppressedOffViewportKey, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                    safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                    postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                    effectivePostSLRuntimeStateStabilizing);
            } else if (auto suppressedOriginalSetOptions = GetCallableOriginalDLSSGSetOptions()) {
                HookLogImportant(
                    "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) — startup window expired "
                    "(viewport=%u settling=%d stabilizing=%d activeProofPending=%d)",
                    g_SuppressedOffViewportKey, postSLConfirmedButStartupSettling ? 1 : 0,
                    effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                    postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0);
                const slResult offResult =
                    suppressedOriginalSetOptions(g_SuppressedOffViewport, g_SuppressedOffOptions);
                if (offResult != kSlResultOk) {
                    HookLogImportant("Streamline Hook: Forwarded slDLSSGSetOptions(OFF) returned %d", offResult);
                } else {
                    g_AcceptedRuntimeOffAwaitingSetOptions.store(false, std::memory_order_release);
                }
            }
            g_SuppressedSetOptionsOffDuringStartup = false;
        }
    }

    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool runtimeModeIsFSRFG = runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG;
    if (!pureObserverOnly && ce::streamline_runtime_policy::ShouldPrepareForStreamlineEnableBeforeOriginalCall(
                                 requestedEnabled, g_FGCompat.IsFSRFGApiActive(), runtimeModeIsFSRFG,
                                 DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration())) {
        DX12_PrepareForStreamlineEnableTransition();
    }

    const bool startupWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
    const bool startupActivationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
    const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
    const bool postSLConfirmedButStartupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
    const bool postSLConfirmedButRuntimeStateStabilizingBase = HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing();
    const bool postSLConfirmedButStaleOffWarmupProtected =
        requestedDisabled && HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected();
    const bool postSLConfirmedButRuntimeStateStabilizing =
        postSLConfirmedButRuntimeStateStabilizingBase || postSLConfirmedButStaleOffWarmupProtected;
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
    const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
    const bool explicitSetOptionsDisableIsAuthoritative =
        ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
            requestedDisabled, true, postSLConfirmedRendering, startupActivationPending, postSLActiveButUnconfirmed,
            postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing,
            g_AcceptedRuntimeOffAwaitingSetOptions.load(std::memory_order_acquire));
    const bool acceptActivatedUnconfirmedResumeOff =
        ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
            requestedDisabled, startupWindowActive, startupProtectedComebackProof, startupActivationPending,
            postSLActiveButUnconfirmed, postSLStartupActivationEntered, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
    const bool suppressOffCall =
        !pureObserverOnly && requestedDisabled && !explicitSetOptionsDisableIsAuthoritative &&
        !acceptActivatedUnconfirmedResumeOff &&
        !ce::streamline_runtime_policy::ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend(
            g_ConfirmedDLSSReflexSuspendPending.load(std::memory_order_acquire), requestedDisabled,
            DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) &&
        ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
            startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
            safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);

    // Arm before forwarding the first OFF->ON call. Streamline is allowed to do synchronous
    // setup inside SetOptions; if it asks the app to tag the activation frame re-entrantly, the
    // official UIColorAndAlpha path must already be ready. BeginActivation is idempotent until
    // the corresponding accepted OFF transition.
    if (!pureObserverOnly && requestedEnabled && !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        ce::dx12_streamline_ui_overlay::BeginActivation(std::clamp(adjustedOptions.numFramesToGenerate + 1u, 1u, 6u));
    }

    const bool setOptionsCallSuppressed = suppressOffCall;
    slResult result;
    if (suppressOffCall) {
        if (postSLConfirmedButStaleOffWarmupProtected && !postSLConfirmedButRuntimeStateStabilizingBase) {
            static std::atomic<bool> s_loggedSetOptionsWarmupProofSuppression{false};
            if (!s_loggedSetOptionsWarmupProofSuppression.exchange(true, std::memory_order_relaxed)) {
                HookLogImportant(
                    "Streamline Hook: Suppressing slDLSSGSetOptions(OFF) during PostSL warmup proof "
                    "(hadFSR=%d explicit=%d safeBootstrap=%d stableProtectionWindow=%d-%d) — treating it as "
                    "startup stale-OFF churn until PostSL proves stable",
                    hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
                    safePostFSRBootstrapPath ? 1 : 0,
                    ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationFirstFrame(),
                    HookGetPostSLStaleOffWarmupProtectionLastFrame());
            }
        }
        static std::atomic<uint64_t> s_startupProtectedSetOptionsOffSuppressionLogCount{0};
        const uint64_t suppressionLogCount =
            s_startupProtectedSetOptionsOffSuppressionLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (suppressionLogCount <= 20 || (suppressionLogCount % 200) == 0) {
            HookLogImportant(
                "Streamline Hook: Suppressing slDLSSGSetOptions(OFF) while DLSS comeback remains startup-protected "
                "(viewport=%u mode=%u startupWindow=%d hadFSR=%d explicitComeback=%d safeBootstrap=%d pending=%d "
                "unconfirmed=%d confirmed=%d settling=%d stabilizing=%d activeProofPending=%d suppressCount=%llu) — "
                "preventing Streamline FG de-initialization before recovery proves stable",
                viewportKey, options.mode, startupWindowActive ? 1 : 0, HookHasFSRFGHistory() ? 1 : 0,
                explicitSetOptionsActivationForCurrentComeback ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0,
                startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                postSLConfirmedButStartupSettling ? 1 : 0, effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0,
                static_cast<unsigned long long>(suppressionLogCount));
        }
        MarkStartupProtectedOffChurnObserved("SetOptions", postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                                             effectivePostSLRuntimeStateStabilizing);
        {
            std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);
            g_SuppressedSetOptionsOffDuringStartup = true;
            g_SuppressedOffViewport = viewport;
            g_SuppressedOffOptions = adjustedOptions;
            g_SuppressedOffViewportKey = viewportKey;
        }
        result = kSlResultOk;
    } else {
        if (acceptActivatedUnconfirmedResumeOff) {
            LogAcceptedOffDuringActivatedUnconfirmedResume(
                "SetOptions", startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                postSLStartupActivationEntered, postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                effectivePostSLRuntimeStateStabilizing);
            ResetStartupProtectedOffChurnActiveProof("forwarded activated-unconfirmed SetOptions disable");
        } else if (explicitSetOptionsDisableIsAuthoritative) {
            static std::atomic<uint64_t> s_authoritativeSetOptionsOffLogCount{0};
            const uint64_t logCount = s_authoritativeSetOptionsOffLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "Streamline Hook: Accepting explicit slDLSSGSetOptions(OFF) as authoritative after confirmed "
                    "PostSL rendering (viewport=%u startupWindow=%d hadFSR=%d safeBootstrap=%d pending=%d "
                    "unconfirmed=%d settling=%d stabilizing=%d activeProofPending=%d log=%llu)",
                    viewportKey, startupWindowActive ? 1 : 0, hadFSRFGPhase ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0,
                    startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                    postSLConfirmedButStartupSettling ? 1 : 0, effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                    postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0, static_cast<unsigned long long>(logCount));
            }
            ResetStartupProtectedOffChurnActiveProof("forwarded authoritative SetOptions disable");
        }
        if (!pureObserverOnly && requestedEnabled) {
            DX12_BeginStreamlineEnableCall();
        }
        result = originalSetOptions(viewport, adjustedOptions);
        if (!pureObserverOnly && requestedEnabled) {
            DX12_EndStreamlineEnableCall();
        }
    }

    if (!pureObserverOnly && requestedEnabled && result != kSlResultOk &&
        !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        ce::dx12_streamline_ui_overlay::EndActivation("slDLSSGSetOptions enable failed");
    }

    LogDLSSGSetOptionsTransition(viewportKey, options, adjustedOptions, originalGeneratedFrames, capabilityMax,
                                 requestedEnabled, setOptionsCallSuppressed, overrideApplied, overrideClamped, result,
                                 pureObserverOnly, startupWindowActive, hadFSRFGPhase,
                                 explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
                                 startupActivationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
                                 postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
    RetryResolveReflexFeatureHooksForRuntimeActivity("slDLSSGSetOptions");

    // SL may overwrite our vtable hooks during slDLSSGSetOptions (especially
    // when re-activating FG).  Verify and repair immediately.
    if (requestedEnabled) {
        DXGIShared::RepairVTableHooksIfNeeded();

        // Detect Present bypass: if DetourPresent hasn't fired in the last N
        // slDLSSGSetOptions calls despite DLSS FG being active, it means SL's
        // wrapper is bypassing our vtable hook entirely.
        static uint64_t s_lastPresentCount = 0;
        static uint32_t s_stallFrames = 0;
        uint64_t currentPresentCount = DXGIShared::g_PresentCallCounter.load(std::memory_order_relaxed);
        if (currentPresentCount == s_lastPresentCount) {
            s_stallFrames++;
            if (s_stallFrames == 10 || s_stallFrames == 30 || s_stallFrames == 100 || (s_stallFrames % 500) == 0) {
                HookLogImportant(
                    "Streamline Hook: Present STALLED for %u frames (counter=%llu) — "
                    "vtable hook bypassed?",
                    s_stallFrames, (unsigned long long)currentPresentCount);
            }
            if (s_stallFrames == 30) {
                g_RenderWatchdog.RequestImmediateDump("Streamline Present stalled for 30 frames",
                                                      DX12_GetGamePresentThreadId());
            }
        } else {
            if (s_stallFrames > 0) {
                HookLogImportant("Streamline Hook: Present resumed after %u stalled frames (counter=%llu)",
                                 s_stallFrames, (unsigned long long)currentPresentCount);
            }
            s_stallFrames = 0;
        }
        s_lastPresentCount = currentPresentCount;
    }

    if (result == kSlResultOk) {
        if (!pureObserverOnly && requestedEnabled) {
            g_AcceptedRuntimeOffAwaitingSetOptions.store(false, std::memory_order_release);
            const ULONGLONG previousSuppressUntilMs =
                g_SuppressNewGetStateActivationUntilMs.exchange(0, std::memory_order_acq_rel);
            const bool wasBlockingGetStateOnlyReactivation =
                g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.exchange(false, std::memory_order_acq_rel);
            const bool wasBlockingUnsafePostFSRGetStateOnlyReactivation =
                g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap.exchange(false, std::memory_order_acq_rel);
            if (previousSuppressUntilMs != 0) {
                const ULONGLONG nowMs = GetTickCount64();
                if (previousSuppressUntilMs > nowMs) {
                    HookLogImportant(
                        "Streamline Hook: Cleared recent-authoritative-FFX GetState suppression due to explicit "
                        "slDLSSGSetOptions enable request (viewport=%u remaining=%llums)",
                        viewportKey, (unsigned long long)(previousSuppressUntilMs - nowMs));
                }
            }
            if (wasBlockingGetStateOnlyReactivation) {
                HookLogImportant(
                    "Streamline Hook: Cleared persistent GetState-only DLSS FG suppression due to explicit "
                    "slDLSSGSetOptions enable request (viewport=%u)",
                    viewportKey);
            }
            if (wasBlockingUnsafePostFSRGetStateOnlyReactivation) {
                HookLogImportant(
                    "Streamline Hook: Cleared unsafe post-FSR GetState-only DLSS FG suppression due to explicit "
                    "slDLSSGSetOptions enable request (viewport=%u)",
                    viewportKey);
            }
        } else if (!pureObserverOnly && requestedDisabled &&
                   ce::streamline_runtime_policy::ShouldApplyViewportRuntimeUpdateFromSetOptions(
                       result == kSlResultOk, setOptionsCallSuppressed)) {
            ResetStartupProtectedOffChurnActiveProof("forwarded explicit SetOptions disable");
            g_SuppressNewGetStateActivationUntilMs.store(0, std::memory_order_release);
            const bool wasBlockingGetStateOnlyReactivation =
                g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.exchange(true, std::memory_order_acq_rel);
            if (!wasBlockingGetStateOnlyReactivation) {
                HookLogImportant(
                    "Streamline Hook: Re-armed persistent GetState-only DLSS FG suppression due to explicit "
                    "slDLSSGSetOptions disable request (viewport=%u)",
                    viewportKey);
            }
        }

        if (ce::streamline_runtime_policy::ShouldApplyViewportRuntimeUpdateFromSetOptions(result == kSlResultOk,
                                                                                          setOptionsCallSuppressed)) {
            const auto runtimeUpdate = ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromRequestedOptions(
                true, true, adjustedOptions.mode, adjustedOptions.numFramesToGenerate, capabilityMax);
            const bool clearAllViewportStatesForDisable =
                ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForSetOptionsDisable(
                    result == kSlResultOk, setOptionsCallSuppressed, adjustedOptions.mode);
            UpdateViewportRuntimeState(viewportKey, runtimeUpdate.active, runtimeUpdate.multiplier,
                                       runtimeUpdate.generatedFrames, runtimeUpdate.capabilityMax, "SetOptions",
                                       clearAllViewportStatesForDisable);
        } else if (setOptionsCallSuppressed) {
            static std::atomic<int> s_suppressedSetOptionsRuntimeSkipLogCount{0};
            const int logCount = s_suppressedSetOptionsRuntimeSkipLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 200) == 0) {
                HookLogImportant(
                    "Streamline Hook: Skipping local runtime-state reduction for suppressed slDLSSGSetOptions(OFF) "
                    "because Streamline never observed that disable edge (viewport=%u startupWindow=%d pending=%d "
                    "unconfirmed=%d confirmed=%d settling=%d stabilizing=%d activeProofPending=%d)",
                    viewportKey, startupWindowActive ? 1 : 0, startupActivationPending ? 1 : 0,
                    postSLActiveButUnconfirmed ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                    postSLConfirmedButStartupSettling ? 1 : 0, effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                    postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0);
            }
        }

        if (!pureObserverOnly && requestedDisabled && !setOptionsCallSuppressed) {
            const bool clearedAcceptedRuntimeOff =
                g_AcceptedRuntimeOffAwaitingSetOptions.exchange(false, std::memory_order_acq_rel);
            if (clearedAcceptedRuntimeOff) {
                HookLogImportant(
                    "Streamline Hook: Matching slDLSSGSetOptions(OFF) reached Streamline successfully — cleared "
                    "accepted-runtime-OFF latch (viewport=%u)",
                    viewportKey);
            }
        }

        const int effectiveMultiplier = GetEffectiveMultiplier(adjustedOptions);

        if (overrideApplied || overrideClamped) {
            if (capabilityMax > 0) {
                HookLog("Streamline Hook: Overrode DLSS-G viewport=%u mode=%s generatedFrames=%u->%u (%dx, max=%u)",
                        viewportKey, GetDLSSGModeName(adjustedOptions.mode), originalGeneratedFrames,
                        adjustedOptions.numFramesToGenerate, effectiveMultiplier, capabilityMax);
            } else {
                HookLog("Streamline Hook: Overrode DLSS-G viewport=%u mode=%s generatedFrames=%u->%u (%dx)",
                        viewportKey, GetDLSSGModeName(adjustedOptions.mode), originalGeneratedFrames,
                        adjustedOptions.numFramesToGenerate, effectiveMultiplier);
            }
        }
    } else if (overrideApplied || overrideClamped) {
        HookLogImportant("Streamline Hook: DLSS-G override failed viewport=%u mode=%s generatedFrames=%u->%u result=%d",
                         viewportKey, GetDLSSGModeName(adjustedOptions.mode), originalGeneratedFrames,
                         adjustedOptions.numFramesToGenerate, result);
    }

    return result;
}

// Safe no-op stub for SL function pointers that SL returned as NULL during
// re-entrant calls.  Steam's OverlayHookD3D3 may call slGetFeatureFunction
// from within SL's execution context (during DllMain or FG processing).
// If SL returns NULL, Steam calls through the NULL pointer → RIP=0 crash.
// Instead of returning an error (which Steam may ignore while still using the
// NULL pointer), substitute a safe stub that returns success and does nothing.
// This allows Steam to continue overlay rendering without crashing.
static slResult SlNullFunctionStub() {
    return kSlResultOk;
}

void* Hooked_slGetPluginFunction(const char* functionName) {
    if (StreamlineHook::IsExternalOverlayPresentGuardActive()) {
        static std::atomic<int> s_externalOverlaySuppressedPluginLookupLogCount{0};
        const int logCount = s_externalOverlaySuppressedPluginLookupLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "Streamline Hook: Suppressing re-entrant slGetPluginFunction during guarded external overlay "
                "Present (name=%s depth=%d)",
                functionName ? functionName : "null", g_ExternalOverlayPresentGuardDepth);
        }
        return reinterpret_cast<void*>(SlNullFunctionStub);
    }

    auto originalGetPluginFunction = GetCallableOriginalGetPluginFunction();
    if (!originalGetPluginFunction) {
        return g_Original_slGetPluginFunction ? reinterpret_cast<void*>(SlNullFunctionStub) : nullptr;
    }

    return originalGetPluginFunction(functionName);
}

slResult Hooked_slGetFeatureFunction(uint32_t feature, const char* functionName, void*& function) {
    if (StreamlineHook::IsExternalOverlayPresentGuardActive()) {
        static std::atomic<int> s_externalOverlaySuppressedLookupLogCount{0};
        const int logCount = s_externalOverlaySuppressedLookupLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "Streamline Hook: Suppressing re-entrant slGetFeatureFunction during guarded external overlay "
                "Present (feature=%u name=%s depth=%d)",
                feature, functionName ? functionName : "null", g_ExternalOverlayPresentGuardDepth);
        }
        function = reinterpret_cast<void*>(SlNullFunctionStub);
        return kSlResultErrorInvalidState;
    }

    auto originalGetFeatureFunction = GetCallableOriginalGetFeatureFunction();
    if (!originalGetFeatureFunction) {
        function = reinterpret_cast<void*>(SlNullFunctionStub);
        return kSlResultErrorInvalidState;
    }

    const slResult result = originalGetFeatureFunction(feature, functionName, function);
    // Safety: if the original returned success but gave us NULL, the caller
    // would call through NULL → RIP=0 crash.  This can happen when third-party
    // overlays (e.g., Steam's OverlayHookD3D3) call slGetFeatureFunction
    // re-entrantly from within Streamline's own code during FG processing.
    // Substitute a safe no-op stub so the caller doesn't crash even if it
    // ignores the error return and uses the function pointer directly.
    if (result == kSlResultOk && !function) {
        static std::atomic<int> s_nullFunctionLogCount{0};
        if (s_nullFunctionLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            HookLogImportant(
                "Streamline Hook: slGetFeatureFunction returned OK with NULL function "
                "(feature=%u name=%s) — substituting safe no-op stub to prevent null call crash",
                feature, functionName ? functionName : "null");
        }
        function = reinterpret_cast<void*>(SlNullFunctionStub);
        return kSlResultOk;
    }
    if (result != kSlResultOk || !functionName || !function) {
        return result;
    }

    // DLSS Frame Generation feature hooks
    if (feature == kSLFeatureDLSSG) {
        if (strcmp(functionName, "slDLSSGSetOptions") == 0) {
            void* originalFunction = function;
            const bool hookReady = MaybeHookDLSSGSetOptions(function, true);
            LogFeatureLookupOutcomeOnce(g_DLSSGSetOptionsLookupLogged, "slDLSSGSetOptions", originalFunction, function,
                                        hookReady);
        } else if (strcmp(functionName, "slDLSSGGetState") == 0) {
            void* originalFunction = function;
            const bool hookReady = MaybeHookDLSSGGetState(function, true);
            LogFeatureLookupOutcomeOnce(g_DLSSGGetStateLookupLogged, "slDLSSGGetState", originalFunction, function,
                                        hookReady);
            // Talos resolves GetState shortly before it starts tagging the activation inputs, but
            // never resolves/calls SetOptions. Arm standby at pointer delivery, before those tags.
            if (!ShouldKeepPureObserverOnlyStreamlineBehavior() &&
                g_StreamlineUsesD3D12.load(std::memory_order_acquire)) {
                ce::dx12_streamline_ui_overlay::BeginPreactivationStandby(2);
            }
        }
    }
    // Reflex feature hook — detect game activation of native Reflex
    else if (feature == kSLFeatureReflex) {
        if (strcmp(functionName, "slReflexSleep") == 0) {
            void* originalFunction = function;
            const bool hookReady = MaybeHookReflexSleep(function, true);
            LogFeatureLookupOutcomeOnce(g_ReflexSleepLookupLogged, "slReflexSleep", originalFunction, function,
                                        hookReady);
        } else if (strcmp(functionName, "slReflexSetOptions") == 0) {
            void* originalFunction = function;
            const bool hookReady = MaybeHookReflexSetOptions(function, true);
            LogFeatureLookupOutcomeOnce(g_ReflexSetOptionsLookupLogged, "slReflexSetOptions", originalFunction,
                                        function, hookReady);
        } else if (strcmp(functionName, "slReflexSetConstants") == 0) {
            void* originalFunction = function;
            const bool hookReady = MaybeHookReflexSetConstants(function, true);
            LogFeatureLookupOutcomeOnce(g_ReflexSetConstantsLookupLogged, "slReflexSetConstants", originalFunction,
                                        function, hookReady);
        }
    }

    return result;
}
