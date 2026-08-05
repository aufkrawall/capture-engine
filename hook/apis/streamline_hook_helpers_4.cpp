#include "streamline_hook_internal.h"


bool ScanLoadedStreamlineModules() {


    HANDLE snapshot = INVALID_HANDLE_VALUE;
    MODULEENTRY32 entry = {};
    DWORD error = ERROR_SUCCESS;
    int attempts = 0;
    bool failedOnFirstEntry = false;
    if (!OpenLoadedModuleSnapshotWithRetry(snapshot, entry, error, attempts, failedOnFirstEntry)) {
        if (!streamline_hook_g_ModuleSnapshotFailureLogged.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant(
                failedOnFirstEntry
                    ? "Streamline Hook: Loaded-module enumeration was empty for feature hooks error=%lu attempts=%d "
                      "retryable=%d"
                    : "Streamline Hook: Failed to enumerate loaded modules for feature hooks error=%lu attempts=%d "
                      "retryable=%d",
                static_cast<unsigned long>(error), attempts,
                ce::streamline_runtime_policy::IsRetryableLoadedModuleSnapshotError(static_cast<uint32_t>(error)) ? 1
                                                                                                                  : 0);
        }
        return false;
    }

    streamline_hook_g_ModuleSnapshotFailureLogged.store(false, std::memory_order_release);

    bool foundModule = false;
    size_t streamlineModuleCount = 0;
    size_t hookedModuleCount = 0;
    do {
        const char* moduleNameOrPath = entry.szExePath[0] != '\0' ? entry.szExePath : entry.szModule;
        if (!ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking(moduleNameOrPath)) {
            continue;
        }

        foundModule = true;
        ++streamlineModuleCount;
        g_FGCompat.SetStreamlineSupportPresent(true);
        if (InstallHooksForModule(entry.hModule, moduleNameOrPath)) {
            ++hookedModuleCount;
        }
    } while (Module32Next(snapshot, &entry));

    const DWORD iterationError = GetLastError();
    CloseHandle(snapshot);

    if (attempts > 1 && !streamline_hook_g_ModuleSnapshotRetrySuccessLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "Streamline Hook: Loaded-module snapshot recovered after transient retry (attempts=%d modules=%zu "
            "hooked=%zu)",
            attempts, streamlineModuleCount, hookedModuleCount);
    }
    if (iterationError != ERROR_SUCCESS && iterationError != ERROR_NO_MORE_FILES &&
        !streamline_hook_g_ModuleSnapshotFailureLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "Streamline Hook: Loaded-module enumeration ended unexpectedly for feature hooks error=%lu "
            "(modules=%zu hooked=%zu)",
            static_cast<unsigned long>(iterationError), streamlineModuleCount, hookedModuleCount);
    }
    return foundModule;

}

bool AreReflexFeatureHooksComplete() {


    return streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) &&
           streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire) &&
           streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire);

}

void RetryResolveReflexFeatureHooksForRuntimeActivity(const char* source) {


    if (AreReflexFeatureHooksComplete()) {
        return;
    }

    constexpr ULONGLONG kRetryIntervalMs = 2500;
    const ULONGLONG nowMs = GetTickCount64();
    ULONGLONG previousMs = streamline_hook_g_ReflexFeatureHookRetryLastMs.load(std::memory_order_acquire);
    if (previousMs != 0 && nowMs >= previousMs && (nowMs - previousMs) < kRetryIntervalMs) {
        return;
    }

    if (!streamline_hook_g_ReflexFeatureHookRetryLastMs.compare_exchange_strong(previousMs, nowMs, std::memory_order_acq_rel,
                                                                std::memory_order_acquire)) {
        return;
    }

    const bool foundModule = ScanLoadedStreamlineModules();
    const bool resolved = TryResolveReflexFeatureHooks();
    static std::atomic<int> s_lateReflexRetryLogCount{0};
    const int logCount = s_lateReflexRetryLogCount.fetch_add(1, std::memory_order_relaxed);
    if (resolved || logCount < 10 || (logCount % 24) == 0) {
        HookLogImportant(
            "Streamline Hook: Late Reflex feature hook retry during DLSSG runtime activity "
            "(source=%s foundModule=%d resolved=%d sleepHooked=%d setOptionsHooked=%d setConstantsHooked=%d "
            "manualLimiter=%d targetIntervalUs=%u)",
            source ? source : "unknown", foundModule ? 1 : 0, resolved ? 1 : 0,
            streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0,
            streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
            streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_ReflexLimiter.IsManualLimiterConfiguredOrActive() ? 1 : 0, g_ReflexLimiter.GetTargetIntervalUs());
    }

}

slResult Hooked_slDLSSGGetState(const slViewportHandle& viewport,  slDLSSGState& state,  const slDLSSGOptions* streamline_hook_options) {


    auto originalGetState = GetCallableOriginalDLSSGGetState();
    if (!originalGetState) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

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
            char statusText[160];
            FormatDLSSGStatusFlags(state.status, statusText, sizeof(statusText));
            HookLogImportant(
                "Streamline Hook: [DLSSG HEALTH] ON but NOT interpolating for %llu consecutive GetState samples — "
                "status=0x%X(%s) presented=%u generatedReq=%u capabilityMax=%u minWH=%u vsyncOk=%d dynMFG=%d "
                "vramMB=%llu fence=%p fenceValue=%llu | Reflex evidence: sleepCalls=%llu (+%llu since last warn) "
                "sleepAge=%llums setOptionsCalls=%llu setOptionsAge=%llums lastMode=%d sleepHooked=%d | "
                "REFLEX-NOT-DETECTED in status = the game's Reflex pipeline is not running (DLSSG requires it); "
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
                streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0);
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
