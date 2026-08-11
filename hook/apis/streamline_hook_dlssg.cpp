#include "streamline_hook_internal.h"


slResult Hooked_slDLSSGGetState(const slViewportHandle& viewport,  slDLSSGState& state,  const slDLSSGOptions* streamline_hook_options) {


    auto originalGetState = GetCallableOriginalDLSSGGetState();
    if (!originalGetState) {
        return streamline_hook_kSlResultErrorInvalidState;
    }
    if (HookIsShuttingDown())
        return originalGetState(viewport, state, streamline_hook_options);

    // Newer integrations can configure DLSS-G by passing options directly to GetState, after
    // slSetTagForFrame has already made the activation input volatile. Keep the latest inactive
    // DX12 UI tag covered before entering GetState so a late OFF->ON observation can adopt it.
    if (!ShouldKeepPureObserverOnlyStreamlineBehavior() && streamline_hook_g_StreamlineUsesD3D12.load(std::memory_order_acquire) &&
        !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        const uint32_t requestedOutputs = streamline_hook_options ? std::clamp(streamline_hook_options->numFramesToGenerate + 1u, 1u, 6u) : 2u;
        ce::dx12_streamline_ui_overlay::BeginPreactivationStandby(requestedOutputs);
    }

    const slResult result = originalGetState(viewport, state, streamline_hook_options);
    RetryResolveReflexFeatureHooksForRuntimeActivity("slDLSSGGetState");
    const uint32_t viewportKey = GetViewportKey(viewport);
    const bool viewportWasActive = WasViewportRuntimeStateActive(viewportKey);
    const bool hasRuntimeFenceEvidence = HasDLSSGRuntimeFenceEvidence(state);
    const bool suppressNewActivation = ShouldSuppressNewGetStateActivation();
    if (result == streamline_hook_kSlResultOk && state.numFramesToGenerateMax > 0) {
        CacheCapabilityMax(viewportKey, state.numFramesToGenerateMax);
    }

    const uint32_t capabilityMax =
        state.numFramesToGenerateMax > 0 ? state.numFramesToGenerateMax : GetCachedCapabilityMax(viewportKey);
    const auto runtimeEvaluation = ce::streamline_runtime_policy::EvaluateViewportRuntimeUpdateFromGetState(
        result == streamline_hook_kSlResultOk, streamline_hook_options != nullptr, viewportWasActive, hasRuntimeFenceEvidence, suppressNewActivation,
        streamline_hook_options ? streamline_hook_options->mode : 0, streamline_hook_options ? streamline_hook_options->numFramesToGenerate : 0u, capabilityMax);
    const bool clearAllViewportStatesForDisable =
        runtimeEvaluation.update.shouldUpdate &&
        ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForGetStateDisable(
            result == streamline_hook_kSlResultOk, streamline_hook_options != nullptr, hasRuntimeFenceEvidence, streamline_hook_options ? streamline_hook_options->mode : 0u,
            capabilityMax);
    if (result == streamline_hook_kSlResultOk && streamline_hook_options != nullptr) {
        static std::atomic<int> s_getStateTraceLogCount{0};
        const int logCount = s_getStateTraceLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 8 || (logCount % 512) == 0) {
            char statusText[160];
            FormatDLSSGStatusFlags(state.status, statusText, sizeof(statusText));
            HookLogImportant(
                "Streamline Hook: slDLSSGGetState observed viewport=%u optionsMode=%s(%u) generated=%u "
                "capabilityMax=%u presented=%u status=0x%X(%s) minWH=%u vsyncOk=%d dynMFG=%d vramMB=%llu "
                "fence=%p fenceValue=%llu viewportWasActive=%d update=%d "
                "updateActive=%d clearAll=%d suppressNew=%d fenceEvidence=%d setOptionsHooked=%d "
                "setOptionsOriginal=%p",
                viewportKey, GetDLSSGModeName(streamline_hook_options->mode), streamline_hook_options->mode, streamline_hook_options->numFramesToGenerate,
                capabilityMax, state.numFramesActuallyPresented, state.status, statusText, state.minWidthOrHeight,
                static_cast<int>(state.bIsVsyncSupportAvailable), static_cast<int>(state.bIsDynamicMFGSupported),
                (unsigned long long)(state.estimatedVRAMUsageInBytes / (1024ull * 1024ull)),
                state.inputsProcessingCompletionFence,
                (unsigned long long)state.lastPresentInputsProcessingCompletionFenceValue, viewportWasActive ? 1 : 0,
                runtimeEvaluation.update.shouldUpdate ? 1 : 0, runtimeEvaluation.update.active ? 1 : 0,
                clearAllViewportStatesForDisable ? 1 : 0, suppressNewActivation ? 1 : 0,
                hasRuntimeFenceEvidence ? 1 : 0, streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
                reinterpret_cast<void*>(streamline_hook_g_Original_slDLSSGSetOptions));
        }
    }

    // [DLSSG HEALTH] — session 20260702_094955: GTA reported DLSSG ON (optionsMode=on, updateActive=1) but
    // presents stayed at base rate all session (numFramesActuallyPresented==1, no fps gain). sl.dlss_g
    // publishes WHY it declines to interpolate in DLSSGState.status; log every status transition, and while
    // the game requests ON without interpolation evidence, emit a deterministic streak warning that pairs
    // NVIDIA's status decode with Reflex call-activity evidence (DLSSG hard-requires Reflex, and GTA's
    // Reflex is historically flaky even without CE).
    if (result == streamline_hook_kSlResultOk) {
        const uint32_t previousStatus = streamline_hook_g_DLSSGLastObservedStatus.exchange(state.status, std::memory_order_relaxed);
        if (previousStatus != state.status) {
            char prevText[160];
            char nowText[160];
            FormatDLSSGStatusFlags(previousStatus, prevText, sizeof(prevText));
            FormatDLSSGStatusFlags(state.status, nowText, sizeof(nowText));
            HookLogImportant(
                "Streamline Hook: [DLSSG HEALTH] status TRANSITION 0x%X(%s) -> 0x%X(%s) (viewport=%u "
                "optionsMode=%s presented=%u minWH=%u vsyncOk=%d dynMFG=%d)",
                previousStatus, prevText, state.status, nowText, viewportKey,
                streamline_hook_options ? GetDLSSGModeName(streamline_hook_options->mode) : "n/a", state.numFramesActuallyPresented,
                state.minWidthOrHeight, static_cast<int>(state.bIsVsyncSupportAvailable),
                static_cast<int>(state.bIsDynamicMFGSupported));
        }
    }
    const bool optionsRequestOn = streamline_hook_options != nullptr && streamline_hook_options->mode != 0;
    if (ce::streamline_runtime_policy::ShouldTrackDLSSGActivationHealthSample(result == streamline_hook_kSlResultOk,
                                                                              optionsRequestOn)) {
        const bool interpolationEvidence =
            ce::streamline_runtime_policy::IsDLSSGInterpolationPresentEvidence(state.numFramesActuallyPresented);
        uint64_t streak = 0;
        if (interpolationEvidence && state.status == 0) {
            streamline_hook_g_DLSSGNotInterpolatingStreak.store(0, std::memory_order_relaxed);
        } else {
            streak = streamline_hook_g_DLSSGNotInterpolatingStreak.fetch_add(1, std::memory_order_relaxed) + 1;
        }
        if (ce::streamline_runtime_policy::ShouldWarnDLSSGActiveButNotInterpolating(streak, streamline_hook_kDLSSGHealthWarnStreak,
                                                                                    streamline_hook_kDLSSGHealthWarnRepeat)) {
            const uint64_t nowMs = GetTickCount64();
            const uint64_t sleepCount = streamline_hook_g_ReflexSleepObservedCount.load(std::memory_order_relaxed);
            const uint64_t sleepCountAtLastLog =
                streamline_hook_g_ReflexSleepCountAtLastHealthLog.exchange(sleepCount, std::memory_order_relaxed);
            const uint64_t sleepLastMs = streamline_hook_g_ReflexSleepLastTickMs.load(std::memory_order_relaxed);
            const uint64_t reflexOptCount = streamline_hook_g_ReflexSetOptionsObservedCount.load(std::memory_order_relaxed);
            const uint64_t reflexOptLastMs = streamline_hook_g_ReflexSetOptionsLastTickMs.load(std::memory_order_relaxed);
            const bool reflexSleepObserved = sleepCount > 0;
            char statusText[160];
            FormatDLSSGStatusFlags(state.status, statusText, sizeof(statusText));
            HookLogImportant(
                "Streamline Hook: [DLSSG HEALTH] ON but NOT interpolating for %llu consecutive GetState samples — "
                "status=0x%X(%s) presented=%u generatedReq=%u capabilityMax=%u minWH=%u vsyncOk=%d dynMFG=%d "
                "vramMB=%llu fence=%p fenceValue=%llu | Reflex evidence: sleepCalls=%llu (+%llu since last warn) "
                "sleepAge=%llums setOptionsCalls=%llu setOptionsAge=%llums lastMode=%d sleepHooked=%d | "
                "REFLEX status bit %s (DLSSG hard-requires Reflex); CE-observed Reflex sleep calls: %llu%s; "
                "status=ok with presented==1 and dynMFG=1 can be hardware flip metering — correlate with the "
                "displayed fps",
                static_cast<unsigned long long>(streak), state.status, statusText, state.numFramesActuallyPresented,
                streamline_hook_options->numFramesToGenerate, capabilityMax, state.minWidthOrHeight,
                static_cast<int>(state.bIsVsyncSupportAvailable), static_cast<int>(state.bIsDynamicMFGSupported),
                (unsigned long long)(state.estimatedVRAMUsageInBytes / (1024ull * 1024ull)),
                state.inputsProcessingCompletionFence,
                (unsigned long long)state.lastPresentInputsProcessingCompletionFenceValue,
                static_cast<unsigned long long>(sleepCount),
                static_cast<unsigned long long>(sleepCount - sleepCountAtLastLog),
                static_cast<unsigned long long>(sleepLastMs ? (nowMs - sleepLastMs) : 0),
                static_cast<unsigned long long>(reflexOptCount),
                static_cast<unsigned long long>(reflexOptLastMs ? (nowMs - reflexOptLastMs) : 0),
                streamline_hook_g_ReflexLastForwardedMode.load(std::memory_order_relaxed),
                streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0,
                reflexSleepObserved ? "absent in Streamline status (runtime-specific)" : "absent",
                static_cast<unsigned long long>(sleepCount),
                reflexSleepObserved ? " (game Reflex pipeline running)" : "");
        }
    } else if (result == streamline_hook_kSlResultOk && streamline_hook_options != nullptr && streamline_hook_options->mode == 0) {
        // Explicit OFF request: end any pending not-interpolating streak so a later re-enable starts a
        // fresh, correctly-attributed streak.
        streamline_hook_g_DLSSGNotInterpolatingStreak.store(0, std::memory_order_relaxed);
    }
    if (runtimeEvaluation.update.shouldUpdate) {
        UpdateViewportRuntimeState(viewportKey, runtimeEvaluation.update.active, runtimeEvaluation.update.multiplier,
                                   runtimeEvaluation.update.generatedFrames, runtimeEvaluation.update.capabilityMax,
                                   "GetState", clearAllViewportStatesForDisable);
    } else if (result == streamline_hook_kSlResultOk && streamline_hook_options != nullptr && runtimeEvaluation.suppressedFreshActivation) {
        static std::atomic<int> s_recentFfxTakeoverSuppressedGetStateLogCount{0};
        const int logCount = s_recentFfxTakeoverSuppressedGetStateLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 128) == 0) {
            const ULONGLONG suppressUntilMs = streamline_hook_g_SuppressNewGetStateActivationUntilMs.load(std::memory_order_acquire);
            const ULONGLONG nowMs = GetTickCount64();
            const ULONGLONG remainingMs = suppressUntilMs > nowMs ? (suppressUntilMs - nowMs) : 0;
            const bool persistentBlock =
                streamline_hook_g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.load(std::memory_order_acquire);
            const ULONGLONG startupTransitionUntilMs =
                DXGIShared::g_SharedState.streamlineStartupTransitionUntilMs.load(std::memory_order_acquire);
            const bool startupWindowActive = startupTransitionUntilMs != 0 && startupTransitionUntilMs > nowMs;
            const ULONGLONG startupRemainingMs = startupWindowActive ? (startupTransitionUntilMs - nowMs) : 0;
            HookLogImportant(
                "Streamline Hook: Suppressing fresh GetState DLSS FG reactivation "
                "(viewport=%u mode=%u generated=%u fence=%p fenceValue=%llu persistentBlock=%d startupWindow=%d "
                "startupRemaining=%llums remaining=%llums)",
                viewportKey, streamline_hook_options->mode, streamline_hook_options->numFramesToGenerate, state.inputsProcessingCompletionFence,
                (unsigned long long)state.lastPresentInputsProcessingCompletionFenceValue, persistentBlock ? 1 : 0,
                startupWindowActive ? 1 : 0, (unsigned long long)startupRemainingMs, (unsigned long long)remainingMs);
        }
    }

    if (!IsObserverOnlyModeActive()) {
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
        if (streamline_hook_g_SuppressedSetOptionsOffDuringStartup && !shouldKeepDeferred) {
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
                    streamline_hook_g_SuppressedOffViewportKey, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                    safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                    postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                    effectivePostSLRuntimeStateStabilizing);
            } else if (auto originalSetOptions = GetCallableOriginalDLSSGSetOptions()) {
                HookLogImportant(
                    "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) via GetState — startup window "
                    "expired (viewport=%u settling=%d stabilizing=%d activeProofPending=%d)",
                    streamline_hook_g_SuppressedOffViewportKey, postSLConfirmedButStartupSettling ? 1 : 0,
                    effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                    postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0);
                const slResult offResult = originalSetOptions(streamline_hook_g_SuppressedOffViewport, streamline_hook_g_SuppressedOffOptions);
                if (offResult != streamline_hook_kSlResultOk) {
                    HookLogImportant("Streamline Hook: Forwarded slDLSSGSetOptions(OFF) via GetState returned %d",
                                     offResult);
                }
            }
            streamline_hook_g_SuppressedSetOptionsOffDuringStartup = false;
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


slResult Hooked_slDLSSGSetOptions(const slViewportHandle& viewport,  const slDLSSGOptions& streamline_hook_options) {


    auto originalSetOptions = GetCallableOriginalDLSSGSetOptions();
    if (!originalSetOptions) {
        return streamline_hook_kSlResultErrorInvalidState;
    }
    if (HookIsShuttingDown())
        return originalSetOptions(viewport, streamline_hook_options);

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
