#include "streamline_hook_internal.h"


slResult Hooked_slDLSSGSetOptions(const slViewportHandle& viewport,  const slDLSSGOptions& streamline_hook_options) {


    auto originalSetOptions = GetCallableOriginalDLSSGSetOptions();
    if (!originalSetOptions) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    slDLSSGOptions adjustedOptions = CloneDLSSGOptions(streamline_hook_options);
    const uint32_t viewportKey = GetViewportKey(viewport);
    const int configuredFactor = NormalizeDLSSFGFactor(GetActiveGraphicsConfig().parsed.dlssFGFactor);
    const uint32_t originalGeneratedFrames = streamline_hook_options.numFramesToGenerate;
    const bool requestedEnabled = ce::streamline_runtime_policy::IsDLSSGModeEnabled(streamline_hook_options.mode);
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
        std::lock_guard<std::mutex> offLock(streamline_hook_g_SuppressedOffMutex);
        if (streamline_hook_g_SuppressedSetOptionsOffDuringStartup) {
            const bool activationPending =
                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
            HookLogImportant(
                "Streamline Hook: Clearing suppressed slDLSSGSetOptions(OFF) due to explicit re-enable request "
                "(viewport=%u) — Streamline never received the OFF, re-enable is consistent "
                "(activationPending=%d)",
                viewportKey, activationPending ? 1 : 0);
            streamline_hook_g_SuppressedSetOptionsOffDuringStartup = false;
        }
    }

    if (!pureObserverOnly) {
        std::lock_guard<std::mutex> offLock(streamline_hook_g_SuppressedOffMutex);
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
            streamline_hook_g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
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
                streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions.load(std::memory_order_acquire));
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
        if (streamline_hook_g_SuppressedSetOptionsOffDuringStartup && !shouldKeepDeferred) {
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
                    streamline_hook_g_SuppressedOffViewportKey, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                    safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                    postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                    effectivePostSLRuntimeStateStabilizing);
            } else if (auto suppressedOriginalSetOptions = GetCallableOriginalDLSSGSetOptions()) {
                HookLogImportant(
                    "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) — startup window expired "
                    "(viewport=%u settling=%d stabilizing=%d activeProofPending=%d)",
                    streamline_hook_g_SuppressedOffViewportKey, postSLConfirmedButStartupSettling ? 1 : 0,
                    effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                    postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0);
                const slResult offResult =
                    suppressedOriginalSetOptions(streamline_hook_g_SuppressedOffViewport, streamline_hook_g_SuppressedOffOptions);
                if (offResult != streamline_hook_kSlResultOk) {
                    HookLogImportant("Streamline Hook: Forwarded slDLSSGSetOptions(OFF) returned %d", offResult);
                } else {
                    streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions.store(false, std::memory_order_release);
                }
            }
            streamline_hook_g_SuppressedSetOptionsOffDuringStartup = false;
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
        streamline_hook_g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
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
            streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions.load(std::memory_order_acquire));
    const bool acceptActivatedUnconfirmedResumeOff =
        ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
            requestedDisabled, startupWindowActive, startupProtectedComebackProof, startupActivationPending,
            postSLActiveButUnconfirmed, postSLStartupActivationEntered, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
    const bool suppressOffCall =
        !pureObserverOnly && requestedDisabled && !explicitSetOptionsDisableIsAuthoritative &&
        !acceptActivatedUnconfirmedResumeOff &&
        !ce::streamline_runtime_policy::ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend(
            streamline_hook_g_ConfirmedDLSSReflexSuspendPending.load(std::memory_order_acquire), requestedDisabled,
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
                viewportKey, streamline_hook_options.mode, startupWindowActive ? 1 : 0, HookHasFSRFGHistory() ? 1 : 0,
                explicitSetOptionsActivationForCurrentComeback ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0,
                startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                postSLConfirmedButStartupSettling ? 1 : 0, effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0,
                static_cast<unsigned long long>(suppressionLogCount));
        }
        MarkStartupProtectedOffChurnObserved("SetOptions", postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                                             effectivePostSLRuntimeStateStabilizing);
        {
            std::lock_guard<std::mutex> offLock(streamline_hook_g_SuppressedOffMutex);
            streamline_hook_g_SuppressedSetOptionsOffDuringStartup = true;
            streamline_hook_g_SuppressedOffViewport = viewport;
            streamline_hook_g_SuppressedOffOptions = adjustedOptions;
            streamline_hook_g_SuppressedOffViewportKey = viewportKey;
        }
        result = streamline_hook_kSlResultOk;
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

    if (!pureObserverOnly && requestedEnabled && result != streamline_hook_kSlResultOk &&
        !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        ce::dx12_streamline_ui_overlay::EndActivation("slDLSSGSetOptions enable failed");
    }

    LogDLSSGSetOptionsTransition(viewportKey, streamline_hook_options, adjustedOptions, originalGeneratedFrames, capabilityMax,
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

    if (result == streamline_hook_kSlResultOk) {
        if (!pureObserverOnly && requestedEnabled) {
            streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions.store(false, std::memory_order_release);
            const ULONGLONG previousSuppressUntilMs =
                streamline_hook_g_SuppressNewGetStateActivationUntilMs.exchange(0, std::memory_order_acq_rel);
            const bool wasBlockingGetStateOnlyReactivation =
                streamline_hook_g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.exchange(false, std::memory_order_acq_rel);
            const bool wasBlockingUnsafePostFSRGetStateOnlyReactivation =
                streamline_hook_g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap.exchange(false, std::memory_order_acq_rel);
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
                       result == streamline_hook_kSlResultOk, setOptionsCallSuppressed)) {
            ResetStartupProtectedOffChurnActiveProof("forwarded explicit SetOptions disable");
            streamline_hook_g_SuppressNewGetStateActivationUntilMs.store(0, std::memory_order_release);
            const bool wasBlockingGetStateOnlyReactivation =
                streamline_hook_g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.exchange(true, std::memory_order_acq_rel);
            if (!wasBlockingGetStateOnlyReactivation) {
                HookLogImportant(
                    "Streamline Hook: Re-armed persistent GetState-only DLSS FG suppression due to explicit "
                    "slDLSSGSetOptions disable request (viewport=%u)",
                    viewportKey);
            }
        }

        if (ce::streamline_runtime_policy::ShouldApplyViewportRuntimeUpdateFromSetOptions(result == streamline_hook_kSlResultOk,
                                                                                          setOptionsCallSuppressed)) {
            const auto runtimeUpdate = ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromRequestedOptions(
                true, true, adjustedOptions.mode, adjustedOptions.numFramesToGenerate, capabilityMax);
            const bool clearAllViewportStatesForDisable =
                ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForSetOptionsDisable(
                    result == streamline_hook_kSlResultOk, setOptionsCallSuppressed, adjustedOptions.mode);
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
                streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions.exchange(false, std::memory_order_acq_rel);
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

slResult SlNullFunctionStub() {


    return streamline_hook_kSlResultOk;

}

void* Hooked_slGetPluginFunction(const char* streamline_hook_functionName) {


    if (StreamlineHook::IsExternalOverlayPresentGuardActive()) {
        static std::atomic<int> s_externalOverlaySuppressedPluginLookupLogCount{0};
        const int logCount = s_externalOverlaySuppressedPluginLookupLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "Streamline Hook: Suppressing re-entrant slGetPluginFunction during guarded external overlay "
                "Present (name=%s depth=%d)",
                streamline_hook_functionName ? streamline_hook_functionName : "null", streamline_hook_g_ExternalOverlayPresentGuardDepth);
        }
        return reinterpret_cast<void*>(SlNullFunctionStub);
    }

    auto originalGetPluginFunction = GetCallableOriginalGetPluginFunction();
    if (!originalGetPluginFunction) {
        return streamline_hook_g_Original_slGetPluginFunction ? reinterpret_cast<void*>(SlNullFunctionStub) : nullptr;
    }

    return originalGetPluginFunction(streamline_hook_functionName);

}

slResult Hooked_slGetFeatureFunction(uint32_t feature,  const char* streamline_hook_functionName,  void*& streamline_hook_function) {


    if (StreamlineHook::IsExternalOverlayPresentGuardActive()) {
        static std::atomic<int> s_externalOverlaySuppressedLookupLogCount{0};
        const int logCount = s_externalOverlaySuppressedLookupLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "Streamline Hook: Suppressing re-entrant slGetFeatureFunction during guarded external overlay "
                "Present (feature=%u name=%s depth=%d)",
                feature, streamline_hook_functionName ? streamline_hook_functionName : "null", streamline_hook_g_ExternalOverlayPresentGuardDepth);
        }
        streamline_hook_function = reinterpret_cast<void*>(SlNullFunctionStub);
        return streamline_hook_kSlResultErrorInvalidState;
    }

    auto originalGetFeatureFunction = GetCallableOriginalGetFeatureFunction();
    if (!originalGetFeatureFunction) {
        streamline_hook_function = reinterpret_cast<void*>(SlNullFunctionStub);
        return streamline_hook_kSlResultErrorInvalidState;
    }

    const slResult result = originalGetFeatureFunction(feature, streamline_hook_functionName, streamline_hook_function);
    // Safety: if the original returned success but gave us NULL, the caller
    // would call through NULL → RIP=0 crash.  This can happen when third-party
    // overlays (e.g., Steam's OverlayHookD3D3) call slGetFeatureFunction
    // re-entrantly from within Streamline's own code during FG processing.
    // Substitute a safe no-op stub so the caller doesn't crash even if it
    // ignores the error return and uses the function pointer directly.
    if (result == streamline_hook_kSlResultOk && !streamline_hook_function) {
        static std::atomic<int> s_nullFunctionLogCount{0};
        if (s_nullFunctionLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            HookLogImportant(
                "Streamline Hook: slGetFeatureFunction returned OK with NULL function "
                "(feature=%u name=%s) — substituting safe no-op stub to prevent null call crash",
                feature, streamline_hook_functionName ? streamline_hook_functionName : "null");
        }
        streamline_hook_function = reinterpret_cast<void*>(SlNullFunctionStub);
        return streamline_hook_kSlResultOk;
    }
    if (result != streamline_hook_kSlResultOk || !streamline_hook_functionName || !streamline_hook_function) {
        return result;
    }

    // DLSS Frame Generation feature hooks
    if (feature == streamline_hook_kSLFeatureDLSSG) {
        if (strcmp(streamline_hook_functionName, "slDLSSGSetOptions") == 0) {
            void* originalFunction = streamline_hook_function;
            const bool hookReady = MaybeHookDLSSGSetOptions(streamline_hook_function, true);
            LogFeatureLookupOutcomeOnce(streamline_hook_g_DLSSGSetOptionsLookupLogged, "slDLSSGSetOptions", originalFunction, streamline_hook_function,
                                        hookReady);
        } else if (strcmp(streamline_hook_functionName, "slDLSSGGetState") == 0) {
            void* originalFunction = streamline_hook_function;
            const bool hookReady = MaybeHookDLSSGGetState(streamline_hook_function, true);
            LogFeatureLookupOutcomeOnce(streamline_hook_g_DLSSGGetStateLookupLogged, "slDLSSGGetState", originalFunction, streamline_hook_function,
                                        hookReady);
            // Talos resolves GetState shortly before it starts tagging the activation inputs, but
            // never resolves/calls SetOptions. Arm standby at pointer delivery, before those tags.
            if (!ShouldKeepPureObserverOnlyStreamlineBehavior() &&
                streamline_hook_g_StreamlineUsesD3D12.load(std::memory_order_acquire)) {
                ce::dx12_streamline_ui_overlay::BeginPreactivationStandby(2);
            }
        }
    }
    // Reflex feature hook — detect game activation of native Reflex
    else if (feature == streamline_hook_kSLFeatureReflex) {
        if (strcmp(streamline_hook_functionName, "slReflexSleep") == 0) {
            void* originalFunction = streamline_hook_function;
            const bool hookReady = MaybeHookReflexSleep(streamline_hook_function, true);
            LogFeatureLookupOutcomeOnce(streamline_hook_g_ReflexSleepLookupLogged, "slReflexSleep", originalFunction, streamline_hook_function,
                                        hookReady);
        } else if (strcmp(streamline_hook_functionName, "slReflexSetOptions") == 0) {
            void* originalFunction = streamline_hook_function;
            const bool hookReady = MaybeHookReflexSetOptions(streamline_hook_function, true);
            LogFeatureLookupOutcomeOnce(streamline_hook_g_ReflexSetOptionsLookupLogged, "slReflexSetOptions", originalFunction,
                                        streamline_hook_function, hookReady);
        } else if (strcmp(streamline_hook_functionName, "slReflexSetConstants") == 0) {
            void* originalFunction = streamline_hook_function;
            const bool hookReady = MaybeHookReflexSetConstants(streamline_hook_function, true);
            LogFeatureLookupOutcomeOnce(streamline_hook_g_ReflexSetConstantsLookupLogged, "slReflexSetConstants", originalFunction,
                                        streamline_hook_function, hookReady);
        }
    }

    return result;

}

slResult Hooked_slSetD3DDevice(void* streamline_hook_d3dDevice) {


    auto originalSetD3DDevice = GetCallableOriginalSetD3DDevice();
    if (!originalSetD3DDevice) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    ID3D12Device* acceptedD3D12Device = nullptr;
    if (streamline_hook_d3dDevice) {
        static_cast<IUnknown*>(streamline_hook_d3dDevice)->QueryInterface(IID_PPV_ARGS(&acceptedD3D12Device));
    }
    const bool isD3D12 = acceptedD3D12Device != nullptr;

    const slResult result = originalSetD3DDevice(streamline_hook_d3dDevice);
    if (result == streamline_hook_kSlResultOk) {
        ID3D12Device* previousAcceptedDevice = nullptr;
        {
            std::lock_guard<std::mutex> lock(streamline_hook_g_AcceptedD3D12DeviceMutex);
            previousAcceptedDevice = streamline_hook_g_AcceptedD3D12Device;
            streamline_hook_g_AcceptedD3D12Device = acceptedD3D12Device;
            acceptedD3D12Device = nullptr;
        }
        if (previousAcceptedDevice) {
            previousAcceptedDevice->Release();
        }
        streamline_hook_g_StreamlineUsesD3D12.store(isD3D12, std::memory_order_release);
        if (isD3D12 && !ShouldKeepPureObserverOnlyStreamlineBehavior()) {
            // Resource tags are legal immediately after Streamline accepts the device. Some
            // integrations (Talos) publish their reusable UI tag before resolving any DLSS-G
            // feature function, so GetState-pointer delivery is too late to cover that tag.
            ce::dx12_streamline_ui_overlay::BeginPreactivationStandby(2);
            HookLogImportant(
                "Streamline Hook: D3D12 device accepted — official UI preactivation standby ready before tags "
                "(device=%p)",
                streamline_hook_d3dDevice);
        } else if (!isD3D12) {
            ce::dx12_streamline_ui_overlay::EndPreactivationStandby("Streamline device is not D3D12");
        }
        TryResolveDLSSGFeatureHooks();
        TryResolveReflexFeatureHooks();
    }
    if (acceptedD3D12Device) {
        acceptedD3D12Device->Release();
    }
    return result;

}

bool StructTypesEqual(const slStructType& lhs,  const slStructType& rhs) {


    return lhs.data1 == rhs.data1 && lhs.data2 == rhs.data2 && lhs.data3 == rhs.data3 &&
           std::memcmp(lhs.data4, rhs.data4, sizeof(lhs.data4)) == 0;

}

bool TryRecordOfficialUiResourceTag(const void* frameToken,  const slResourceTag& tag,  void* streamline_hook_commandBuffer) {


    if (ShouldKeepPureObserverOnlyStreamlineBehavior() || !streamline_hook_g_StreamlineUsesD3D12.load(std::memory_order_acquire) ||
        !streamline_hook_commandBuffer || tag.type != streamline_hook_kSLBufferTypeUIColorAndAlpha || !tag.resource || !tag.resource->native ||
        tag.resource->type != slResourceType::kTexture2D || tag.extent.top != 0 || tag.extent.left != 0) {
        return false;
    }

    auto* uiResource = static_cast<ID3D12Resource*>(tag.resource->native);
    const D3D12_RESOURCE_DESC desc = uiResource->GetDesc();
    const uint32_t width = tag.extent.width != 0
                               ? tag.extent.width
                               : (tag.resource->width != 0 ? tag.resource->width : static_cast<uint32_t>(desc.Width));
    const uint32_t height =
        tag.extent.height != 0 ? tag.extent.height : (tag.resource->height != 0 ? tag.resource->height : desc.Height);
    const DXGI_FORMAT format =
        tag.resource->nativeFormat != 0 ? static_cast<DXGI_FORMAT>(tag.resource->nativeFormat) : desc.Format;
    const bool hdr = DX12_ResolveRuntimeOwnedOverlayTargetHDRState(format);
    ID3D12CommandQueue* initializationQueue = DX12_AcquireOriginalGameQueueForOverlay();
    if (!initializationQueue) {
        return false;
    }

    ce::dx12_streamline_ui_overlay::RecordRequest request;
    request.commandList = static_cast<ID3D12GraphicsCommandList*>(streamline_hook_commandBuffer);
    request.uiResource = uiResource;
    request.initializationQueue = initializationQueue;
    request.resourceState = static_cast<D3D12_RESOURCE_STATES>(tag.resource->state);
    request.format = format;
    request.width = width;
    request.height = height;
    request.hdr = hdr;
    request.frameToken = frameToken;
    const bool recorded = ce::dx12_streamline_ui_overlay::TryRecordBootstrap(request);
    initializationQueue->Release();
    return recorded;

}

uint32_t LogOfficialUiTagOpportunity(const char* tagApi,  const void* frameToken,  uint32_t viewportKey, 
                                     const slResourceTag* tags,  uint32_t numTags,  void* streamline_hook_commandBuffer, 
                                     uint32_t feature,  uint32_t numInputs) {


    static std::atomic<uint32_t> s_uiTagOpportunityLogCount{0};
    const uint32_t opportunity = s_uiTagOpportunityLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (opportunity > 12 && (opportunity % 300) != 0) {
        return 0;
    }

    HookLogImportant(
        "Streamline Hook: Official UI tag record opportunity #%u (api=%s feature=%u frame=%p viewport=%u "
        "tags=%p numTags=%u inputs=%u commandBuffer=%p d3d12=%d)",
        opportunity, tagApi ? tagApi : "unknown", feature, frameToken, viewportKey, tags, numTags, numInputs,
        streamline_hook_commandBuffer, streamline_hook_g_StreamlineUsesD3D12.load(std::memory_order_relaxed) ? 1 : 0);
    const uint32_t loggedTags = tags ? std::min(numTags, 12u) : 0u;
    for (uint32_t i = 0; i < loggedTags; ++i) {
        const slResourceTag& tag = tags[i];
        HookLogImportant(
            "Streamline Hook: UI tag opportunity #%u tag[%u] type=%u lifecycle=%d resource=%p "
            "extent=(%u,%u %ux%u)",
            opportunity, i, tag.type, tag.lifecycle, tag.resource, tag.extent.left, tag.extent.top, tag.extent.width,
            tag.extent.height);
    }
    return opportunity;

}

void TryRecordOfficialUiTag(const char* tagApi,  const void* frameToken,  const slViewportHandle& viewport, 
                            const slResourceTag* tags,  uint32_t numTags,  void* streamline_hook_commandBuffer) {


    const bool wantsUiBootstrapRecord = ce::dx12_streamline_ui_overlay::OnFrameTag(frameToken);

    if (wantsUiBootstrapRecord) {
        LogOfficialUiTagOpportunity(tagApi, frameToken, GetViewportKey(viewport), tags, numTags, streamline_hook_commandBuffer);
    }

    // DLSS-G consumes UIColorAndAlpha before its first generated output exists, while PostSL can
    // only run after that output has been produced. Record CE's rolling/one-shot overlay into the
    // official UI layer on the app-provided command list. Source frames keep replacing the
    // eValidUntilPresent record until PostSL consumes the bounded output handoff. This introduces
    // no copy, extra submission, queue, or wait and naturally follows Streamline's synchronization.
    if (wantsUiBootstrapRecord && tags) {
        for (uint32_t i = 0; i < numTags; ++i) {
            if (TryRecordOfficialUiResourceTag(frameToken, tags[i], streamline_hook_commandBuffer)) {
                break;
            }
        }
    }

}

slResult Hooked_slSetTag(const slViewportHandle& viewport,  const slResourceTag* tags,  uint32_t numTags, 
                         void* streamline_hook_commandBuffer) {


    auto originalSetTag = GetCallableOriginalSetTag();
    if (!originalSetTag) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    // Legacy/global resource tagging has no frame token. A monotonically unique opaque identity
    // lets the standby state roll across calls without dereferencing or fabricating an SL object.
    static std::atomic<uintptr_t> s_legacyTagToken{1};
    const uintptr_t tokenValue = s_legacyTagToken.fetch_add(1, std::memory_order_relaxed);
    const void* frameToken = reinterpret_cast<const void*>((tokenValue << 1u) | 1u);
    TryRecordOfficialUiTag("slSetTag", frameToken, viewport, tags, numTags, streamline_hook_commandBuffer);

    return originalSetTag(viewport, tags, numTags, streamline_hook_commandBuffer);

}
