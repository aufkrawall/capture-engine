#include "streamline_hook_internal.h"

namespace {
constexpr int kSLReflexModeLowLatency = 2;
}

namespace {
constexpr int kSLReflexModeLowLatencyWithBoost = 3;
}

namespace {
constexpr int kSLReflexOptionsModeLowLatency = 1;
}

namespace {
constexpr int kSLReflexOptionsModeLowLatencyWithBoost = 2;
}

bool IsObserverOnlyModeActive() {


    return HookOverlayObserverOnlyEnabled();

}


bool IsObserverPolicyOnlyModeActive() {


    return HookOverlayObserverPolicyOnlyEnabled();

}


bool ShouldKeepPureObserverOnlyStreamlineBehavior() {


    return ce::streamline_runtime_policy::ShouldKeepPureObserverOnlyStreamlineBehavior(
        IsObserverOnlyModeActive(), IsObserverPolicyOnlyModeActive());

}


void LogStreamlineReflexSignalChange(const char* sourceName,  int32_t mode,  uint32_t incomingFrameLimitUs, 
                                     uint32_t forwardedFrameLimitUs,  uint32_t targetIntervalUs) {


    const bool pacingSignalActive =
        ce::streamline_runtime_policy::IsStreamlineReflexPacingSignalActive(mode, incomingFrameLimitUs);
    const bool lowLatencyModeEnabled = ce::streamline_runtime_policy::IsStreamlineReflexLowLatencyModeEnabled(mode);
    const bool frameLimitActive =
        ce::streamline_runtime_policy::IsStreamlineReflexFrameLimitActive(incomingFrameLimitUs);
    const bool forwardedFrameLimitActive =
        ce::streamline_runtime_policy::IsStreamlineReflexFrameLimitActive(forwardedFrameLimitUs);
    const bool frameLimitOverrideApplied = incomingFrameLimitUs != forwardedFrameLimitUs;
    const bool runtimeDLSSFGApiActive = g_FGCompat.IsDLSSFGApiActive();
    const bool runtimeFSRFGApiActive = g_FGCompat.IsFSRFGApiActive();
    const bool streamlineFGSignalActive = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();

    static std::mutex s_reflexSignalLogMutex;
    static ReflexSignalLogState s_optionsState;
    static ReflexSignalLogState s_constantsState;
    const bool isOptionsSource = sourceName && strcmp(sourceName, "slReflexSetOptions") == 0;

    bool shouldLog = false;
    {
        std::lock_guard<std::mutex> lock(s_reflexSignalLogMutex);
        auto& last = isOptionsSource ? s_optionsState : s_constantsState;
        shouldLog = !last.valid || last.mode != mode || last.incomingFrameLimitUs != incomingFrameLimitUs ||
                    last.forwardedFrameLimitUs != forwardedFrameLimitUs || last.targetIntervalUs != targetIntervalUs ||
                    last.frameLimitOverrideApplied != frameLimitOverrideApplied ||
                    last.pacingSignalActive != pacingSignalActive ||
                    last.runtimeDLSSFGApiActive != runtimeDLSSFGApiActive ||
                    last.runtimeFSRFGApiActive != runtimeFSRFGApiActive ||
                    last.streamlineFGSignalActive != streamlineFGSignalActive || last.runtimeMode != runtimeMode;
        if (shouldLog) {
            last.valid = true;
            last.mode = mode;
            last.incomingFrameLimitUs = incomingFrameLimitUs;
            last.forwardedFrameLimitUs = forwardedFrameLimitUs;
            last.targetIntervalUs = targetIntervalUs;
            last.frameLimitOverrideApplied = frameLimitOverrideApplied;
            last.pacingSignalActive = pacingSignalActive;
            last.runtimeDLSSFGApiActive = runtimeDLSSFGApiActive;
            last.runtimeFSRFGApiActive = runtimeFSRFGApiActive;
            last.streamlineFGSignalActive = streamlineFGSignalActive;
            last.runtimeMode = runtimeMode;
        }
    }

    if (shouldLog) {
        HookLogImportant(
            "Streamline Hook: Reflex signal via %s mode=%d lowLatency=%d frameLimitUs=%u frameLimitActive=%d "
            "forwardedFrameLimitUs=%u forwardedFrameLimitActive=%d override=%d pacingActive=%d ceCapActive=%d "
            "ceTargetIntervalUs=%u runtime=%s dlssApi=%d fsrApi=%d slSignal=%d",
            sourceName ? sourceName : "unknown", mode, lowLatencyModeEnabled ? 1 : 0, incomingFrameLimitUs,
            frameLimitActive ? 1 : 0, forwardedFrameLimitUs, forwardedFrameLimitActive ? 1 : 0,
            frameLimitOverrideApplied ? 1 : 0, pacingSignalActive ? 1 : 0, targetIntervalUs > 0 ? 1 : 0,
            targetIntervalUs, ce::fg_runtime::GetRuntimeModeName(runtimeMode), runtimeDLSSFGApiActive ? 1 : 0,
            runtimeFSRFGApiActive ? 1 : 0, streamlineFGSignalActive ? 1 : 0);
    }

}


void MaybePrepareForStreamlineEnableTransitionFromReflex(const char* sourceName) {


    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool runtimeModeIsFSRFG = runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG;
    const bool runtimeOwnsSwapchain = DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();
    if (ce::streamline_runtime_policy::ShouldRequestStreamlineEnablePreparationOnReflexActivation(
            true, g_FGCompat.IsFSRFGApiActive(), runtimeModeIsFSRFG, runtimeOwnsSwapchain)) {
        HookLogImportant(
            "Streamline Hook: Reflex activation requesting Streamline enable preparation via %s "
            "(runtime=%s apiFSR=%d fgOwned=%d)",
            sourceName ? sourceName : "unknown", ce::fg_runtime::GetRuntimeModeName(runtimeMode),
            g_FGCompat.IsFSRFGApiActive() ? 1 : 0, runtimeOwnsSwapchain ? 1 : 0);
        DX12_PrepareForStreamlineEnableTransition();
    }

}


void HandleStreamlineReflexPacingSignal(const char* sourceName,  int32_t mode,  uint32_t incomingFrameLimitUs, 
                                        uint32_t forwardedFrameLimitUs,  uint32_t targetIntervalUs) {


    const bool lowLatencyModeEnabled = ce::streamline_runtime_policy::IsStreamlineReflexLowLatencyModeEnabled(mode);
    const bool frameLimitActive =
        ce::streamline_runtime_policy::IsStreamlineReflexFrameLimitActive(incomingFrameLimitUs);
    const bool pacingSignalActive =
        ce::streamline_runtime_policy::IsStreamlineReflexPacingSignalActive(mode, incomingFrameLimitUs);

    LogStreamlineReflexSignalChange(sourceName, mode, incomingFrameLimitUs, forwardedFrameLimitUs, targetIntervalUs);

    if (pacingSignalActive) {
        const bool activationEdge = !g_ReflexLimiter.IsGameActivated();
        const bool clearedSuspendIntent =
            streamline_hook_g_ConfirmedDLSSReflexSuspendPending.exchange(false, std::memory_order_acq_rel);
        if (activationEdge) {
            HookLogImportant(
                "Streamline Hook: Game ACTIVATED Reflex pacing via %s (mode=%d lowLatency=%d frameLimitUs=%u "
                "frameLimitActive=%d)",
                sourceName ? sourceName : "unknown", mode, lowLatencyModeEnabled ? 1 : 0, incomingFrameLimitUs,
                frameLimitActive ? 1 : 0);
            if (lowLatencyModeEnabled) {
                MaybePrepareForStreamlineEnableTransitionFromReflex(sourceName);
            }
        }
        if (clearedSuspendIntent) {
            HookLogImportant("Streamline Hook: Cleared confirmed Reflex suspend intent on pacing reactivation via %s",
                             sourceName ? sourceName : "unknown");
        }
        g_ReflexLimiter.SetGameActivated(true);
        g_ReflexLimiter.MarkNativePacingSignal();
    } else {
        const bool deactivationEdge = g_ReflexLimiter.IsGameActivated();
        if (deactivationEdge) {
            HookLogImportant("Streamline Hook: Game DEACTIVATED Reflex pacing via %s (mode=%d frameLimitUs=%u)",
                             sourceName ? sourceName : "unknown", mode, incomingFrameLimitUs);
        }
        if (ce::streamline_runtime_policy::ShouldArmConfirmedDLSSReflexSuspendIntent(
                deactivationEdge, g_FGCompat.IsDLSSFGApiActive(),
                DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                HookIsPostSLOverlayConfirmedRendering(),
                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire),
                HookIsPostSLOverlayActiveButUnconfirmed())) {
            const bool wasPending = streamline_hook_g_ConfirmedDLSSReflexSuspendPending.exchange(true, std::memory_order_acq_rel);
            const bool startupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
            const bool runtimeStabilizing = HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing() ||
                                            HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected();
            ResetStartupProtectedOffChurnActiveProof("confirmed Reflex suspend intent");
            if (!wasPending) {
                HookLogImportant(
                    "Streamline Hook: Confirmed DLSS-G epoch observed game-owned Reflex OFF via %s "
                    "(startupWindow=%d settling=%d stabilizing=%d) — next inactive GetState/SetOptions edge is "
                    "authoritative so DLSS-G cannot remain active without Reflex (manual limiter target remains "
                    "unchanged)",
                    sourceName ? sourceName : "unknown",
                    DXGIShared::IsStreamlineStartupTransitionWindowActive() ? 1 : 0, startupSettling ? 1 : 0,
                    runtimeStabilizing ? 1 : 0);
            }
        }
        g_ReflexLimiter.SetGameActivated(false);
    }

}


uint32_t GetCachedCapabilityMax(uint32_t viewportKey) {


    std::lock_guard<std::mutex> lock(streamline_hook_g_StateMutex);
    const auto it = streamline_hook_g_ViewportCapabilityMax.find(viewportKey);
    return it != streamline_hook_g_ViewportCapabilityMax.end() ? it->second : 0u;

}


void CacheCapabilityMax(uint32_t viewportKey,  uint32_t capabilityMax) {


    if (capabilityMax == 0) {
        return;
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_StateMutex);
        auto& cached = streamline_hook_g_ViewportCapabilityMax[viewportKey];
        if (cached != capabilityMax) {
            cached = capabilityMax;
            changed = true;
        }
    }

    if (changed && HookDebugLoggingEnabled()) {
        HookLog("Streamline Hook: Viewport %u reports max generated frames=%u (%dx max)", viewportKey, capabilityMax,
                capabilityMax + 1);
    }

}


void ApplyCombinedDLSSFGState(bool active,  int multiplier) {


    if (active) {
        const int effectiveMultiplier = std::clamp(multiplier, 2, 4);
        g_FGCompat.SetDLSSFGMultiplier(effectiveMultiplier);
        g_FGCompat.SetDLSSFGActive(true);

        if (g_IPC && g_IPC->GetSharedMem()) {
            g_IPC->GetSharedMem()->dlssState.fgActive = true;
            g_IPC->GetSharedMem()->dlssState.mfgMultiplier = effectiveMultiplier;
        }
    } else {
        g_FGCompat.SetDLSSFGActive(false);
        g_FGCompat.SetDLSSFGMultiplier(0);

        if (g_IPC && g_IPC->GetSharedMem()) {
            g_IPC->GetSharedMem()->dlssState.fgActive = false;
            g_IPC->GetSharedMem()->dlssState.mfgMultiplier = 0;
        }
    }

}


void ApplyCombinedStreamlineRuntimeState(bool active,  int multiplier,  bool explicitSetOptionsEnableSignal, 
                                         const char* source) {


    if (ShouldKeepPureObserverOnlyStreamlineBehavior()) {
        const bool previousSignalObserved =
            DXGIShared::g_StreamlineFGRunning.exchange(active, std::memory_order_acq_rel);
        g_FGCompat.SetStreamlineFGSignal(active);
        ApplyCombinedDLSSFGState(active, active ? std::clamp(multiplier, 2, 4) : 0);
        if (previousSignalObserved != active) {
            DX12_OnStreamlineFGStateChanged(active);
            HookLogImportant("Streamline Hook: FG state transition %s->%s via %s (observer-only pass-through)",
                             previousSignalObserved ? "ON" : "OFF", active ? "ON" : "OFF",
                             source ? source : "runtime-state");
        }
        return;
    }

    const bool startupWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
    const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
    const bool startupActivationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
    const bool postSLConfirmedButStartupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
    const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
    const bool sourceWasSetOptions = source && strcmp(source, "SetOptions") == 0;
    const bool sourceWasGetState = source && strcmp(source, "GetState") == 0;
    const bool postSLConfirmedButRuntimeStateStabilizingBase = HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing();
    const bool postSLConfirmedButStaleOffWarmupProtected =
        !active && HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected();
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
    const bool acceptActivatedUnconfirmedResumeOff =
        ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
            !active, startupWindowActive, startupProtectedComebackProof, startupActivationPending,
            postSLActiveButUnconfirmed, postSLStartupActivationEntered, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling,
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof);
    const bool explicitSetOptionsDisableIsAuthoritative =
        ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
            !active, sourceWasSetOptions, postSLConfirmedRendering, startupActivationPending,
            postSLActiveButUnconfirmed, postSLConfirmedButStartupSettling,
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof,
            streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions.load(std::memory_order_acquire));
    const bool previousSignal = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool confirmedReflexSuspendIsAuthoritative =
        ce::streamline_runtime_policy::ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend(
            streamline_hook_g_ConfirmedDLSSReflexSuspendPending.load(std::memory_order_acquire), !active, previousSignal);
    const bool deferOffSignal =
        !active && !explicitSetOptionsDisableIsAuthoritative && !acceptActivatedUnconfirmedResumeOff &&
        !confirmedReflexSuspendIsAuthoritative &&
        ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
            startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
            safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling,
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof);
    const auto signalUpdate = ce::streamline_runtime_policy::ResolveCombinedRuntimeSignalUpdate(
        active, deferOffSignal, previousSignal, multiplier);
    const bool previousExplicitSetOptionsActivation =
        streamline_hook_g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);

    const bool previousSignalObserved =
        DXGIShared::g_StreamlineFGRunning.exchange(signalUpdate.effectiveActive, std::memory_order_acq_rel);
    if (ce::streamline_runtime_policy::ShouldArmStartupTransitionWindowOnFreshActiveSignal(active, previousSignal)) {
        DXGIShared::ArmStreamlineStartupTransitionWindow();
        streamline_hook_g_StartupWindowOffExtensionPending.store(true, std::memory_order_release);
    }
    const bool explicitSetOptionsActivation = explicitSetOptionsEnableSignal;
    const bool updatedExplicitSetOptionsActivation =
        ce::streamline_runtime_policy::ResolveCurrentComebackExplicitSetOptionsActivation(
            previousExplicitSetOptionsActivation, signalUpdate.effectiveActive, signalUpdate.freshActivationEdge,
            explicitSetOptionsActivation);
    streamline_hook_g_CurrentComebackActivatedViaExplicitSetOptions.store(updatedExplicitSetOptionsActivation,
                                                          std::memory_order_release);
    const bool acceptedRuntimeOffAwaitingSetOptions =
        ce::streamline_runtime_policy::ShouldLatchAcceptedRuntimeOffAwaitingSetOptions(
            previousSignalObserved, signalUpdate.effectiveActive, sourceWasGetState);
    if (acceptedRuntimeOffAwaitingSetOptions) {
        streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions.store(true, std::memory_order_release);
        const bool wasBlockingGetStateOnlyReactivation =
            streamline_hook_g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.exchange(true, std::memory_order_acq_rel);
        HookLogImportant(
            "Streamline Hook: Accepted runtime OFF via %s before matching SetOptions — forwarding the next "
            "SetOptions(OFF) despite stale PostSL startup proof and blocking GetState-only reactivation "
            "(previousBlock=%d)",
            source ? source : "runtime-state", wasBlockingGetStateOnlyReactivation ? 1 : 0);
    }
    g_FGCompat.SetStreamlineFGSignal(signalUpdate.effectiveActive);
    ApplyCombinedDLSSFGState(signalUpdate.effectiveActive, signalUpdate.effectiveMultiplier);
    if (confirmedReflexSuspendIsAuthoritative && !signalUpdate.deferredOffDuringStartupWindow) {
        const bool consumedSuspendIntent =
            streamline_hook_g_ConfirmedDLSSReflexSuspendPending.exchange(false, std::memory_order_acq_rel);
        ResetStartupProtectedOffChurnActiveProof("accepted confirmed Reflex suspend runtime OFF");
        if (consumedSuspendIntent) {
            HookLogImportant(
                "Streamline Hook: Accepted %s OFF as authoritative after confirmed Reflex suspend — "
                "startup churn protection remains armed for future cold starts",
                source ? source : "runtime-state");
        }
    } else if (acceptActivatedUnconfirmedResumeOff) {
        LogAcceptedOffDuringActivatedUnconfirmedResume(
            source, startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
            safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
            postSLStartupActivationEntered, postSLConfirmedRendering, postSLConfirmedButStartupSettling,
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof);
        ResetStartupProtectedOffChurnActiveProof("accepted activated-unconfirmed startup suspend");
    } else if (explicitSetOptionsDisableIsAuthoritative) {
        ResetStartupProtectedOffChurnActiveProof("accepted authoritative SetOptions disable");
    } else if (!active && signalUpdate.deferredOffDuringStartupWindow) {
        MarkStartupProtectedOffChurnObserved(
            source, postSLConfirmedRendering, postSLConfirmedButStartupSettling,
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof);
    } else if (active) {
        MarkStartupProtectedActiveRuntimeProof(source, signalUpdate.effectiveMultiplier);
    } else if (!signalUpdate.effectiveActive) {
        ResetStartupProtectedOffChurnActiveProof("accepted inactive Streamline runtime signal");

    }

    if (!previousExplicitSetOptionsActivation && updatedExplicitSetOptionsActivation && signalUpdate.effectiveActive &&
        explicitSetOptionsActivation && !signalUpdate.freshActivationEdge) {
        DX12_OnStreamlineExplicitSetOptionsActivationConfirmed();
        HookLogImportant(
            "Streamline Hook: Upgraded already-live DLSS comeback provenance to explicit SetOptions enable "
            "(source=%s startupWindow=%d hadFSR=%d safeBootstrap=%d pending=%d unconfirmed=%d settling=%d "
            "stabilizing=%d)",
            source ? source : "runtime-state", startupWindowActive ? 1 : 0, HookHasFSRFGHistory() ? 1 : 0,
            safePostFSRBootstrapPath ? 1 : 0, startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
            postSLConfirmedButStartupSettling ? 1 : 0, postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
    }

    if (previousSignalObserved != signalUpdate.effectiveActive) {
        if (signalUpdate.effectiveActive) {
            ce::dx12_streamline_ui_overlay::BeginActivation(
                static_cast<uint32_t>(std::clamp(signalUpdate.effectiveMultiplier, 1, 6)));
        } else {
            ce::dx12_streamline_ui_overlay::EndActivation("accepted Streamline FG OFF transition");
        }
        DX12_OnStreamlineFGStateChanged(signalUpdate.effectiveActive);
        HookLogImportant("Streamline Hook: FG state transition %s->%s via %s", previousSignalObserved ? "ON" : "OFF",
                         signalUpdate.effectiveActive ? "ON" : "OFF", source ? source : "runtime-state");
    }
    ce::fg_session::EmitFGEvent(sourceWasSetOptions ? ce::fg_session::FGEventKind::kStreamlineSetOptionsRuntimeUpdate
                                                    : ce::fg_session::FGEventKind::kStreamlineGetStateRuntimeUpdate,
                                source ? source : "StreamlineRuntimeState", nullptr, nullptr,
                                signalUpdate.effectiveActive ? ce::fg_runtime::RuntimeMode::kDLSSFG
                                                             : ce::fg_runtime::RuntimeMode::kStreamlineNoFG,
                                signalUpdate.effectiveActive, updatedExplicitSetOptionsActivation);
    if (signalUpdate.deferredOffDuringStartupWindow && !startupWindowActive) {
        if (!active && !postSLConfirmedButStartupSettling && sourceWasGetState &&
            postSLConfirmedButStaleOffWarmupProtected && !postSLConfirmedButRuntimeStateStabilizingBase) {
            static std::atomic<bool> s_loggedGetStateWarmupProofSuppression{false};
            if (!s_loggedGetStateWarmupProofSuppression.exchange(true, std::memory_order_relaxed)) {
                HookLogImportant(
                    "Streamline Hook: Suppressing post-stabilization GetState OFF during PostSL warmup proof "
                    "(hadFSR=%d explicit=%d safeBootstrap=%d stableProtectionWindow=%d-%d)",
                    hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
                    safePostFSRBootstrapPath ? 1 : 0,
                    ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationFirstFrame(),
                    HookGetPostSLStaleOffWarmupProtectionLastFrame());
            }
        } else if (!active && !postSLConfirmedButStartupSettling && sourceWasGetState &&
                   postSLConfirmedButOffChurnAwaitingActiveProof) {
            static std::atomic<bool> s_loggedGetStateActiveProofSuppression{false};
            if (!s_loggedGetStateActiveProofSuppression.exchange(true, std::memory_order_relaxed)) {
                HookLogImportant(
                    "Streamline Hook: Suppressing GetState OFF until startup OFF churn receives active proof "
                    "(hadFSR=%d explicit=%d safeBootstrap=%d activeProof=%u/%u)",
                    hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
                    safePostFSRBootstrapPath ? 1 : 0,
                    streamline_hook_g_StartupProtectedOffChurnActiveProofCount.load(std::memory_order_acquire),
                    ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProofUpdateThreshold());
            }
        } else if (!active && !postSLConfirmedButStartupSettling && postSLConfirmedButRuntimeStateStabilizingBase &&
                   sourceWasGetState) {
            static std::atomic<bool> s_loggedPostSettlingGetStateSuppression{false};
            if (!s_loggedPostSettlingGetStateSuppression.exchange(true, std::memory_order_relaxed)) {
                const int runtimeStateStabilizationLastFrame = HookGetPostSLRuntimeStateStabilizationLastFrame();
                HookLogImportant(
                    "Streamline Hook: Suppressing first post-settling GetState OFF during runtime-state stabilization "
                    "(hadFSR=%d explicit=%d safeBootstrap=%d stableProtectionWindow=%d-%d)",
                    hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
                    safePostFSRBootstrapPath ? 1 : 0,
                    ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationFirstFrame(),
                    runtimeStateStabilizationLastFrame);
            }
        }
        static std::atomic<int> s_halfArmedDeferredOffLogCount{0};
        const int logCount = s_halfArmedDeferredOffLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Keeping OFF churn deferred after startup window expiry because Streamline DLSS "
                "startup is still startup-protected (hadFSR=%d explicit=%d safeBootstrap=%d pending=%d "
                "unconfirmed=%d startupActivationEntered=%d confirmed=%d settling=%d stabilizing=%d "
                "activeProofPending=%d source=%s)",
                hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
                safePostFSRBootstrapPath ? 1 : 0, startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                postSLStartupActivationEntered ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                postSLConfirmedButStartupSettling ? 1 : 0,
                (postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof) ? 1 : 0,
                postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0, source ? source : "runtime-state");
        }
    }
    if (signalUpdate.deferredOffDuringStartupWindow && signalUpdate.shouldExtendStartupTransitionWindow) {
        const bool shouldExtend = streamline_hook_g_StartupWindowOffExtensionPending.exchange(false, std::memory_order_acq_rel);
        if (!shouldExtend) {
            static std::atomic<int> s_startupWindowOffExtensionAlreadyConsumedLogCount{0};
            const int logCount =
                s_startupWindowOffExtensionAlreadyConsumedLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10 || (logCount % 100) == 0) {
                HookLogImportant(
                    "Streamline Hook: Deferring OFF signal during startup transition window "
                    "(g_StreamlineFGRunning stays ON, multiplier=%d source=%s) — extension already consumed for "
                    "current churn burst",
                    signalUpdate.effectiveMultiplier, source ? source : "unknown");
            }
            return;
        }
        DXGIShared::ExtendStreamlineStartupTransitionWindow();
        HookLogImportant(
            "Streamline Hook: Deferring OFF signal during startup transition window "
            "(g_StreamlineFGRunning stays ON, multiplier=%d source=%s) — extended startup window",
            signalUpdate.effectiveMultiplier, source ? source : "unknown");
    } else if (ce::streamline_runtime_policy::ShouldPrimeStartupWindowOffExtensionLatch(
                   signalUpdate.effectiveActive, signalUpdate.freshActivationEdge)) {
        streamline_hook_g_StartupWindowOffExtensionPending.store(true, std::memory_order_release);
    }

}


bool WasViewportRuntimeStateActive(uint32_t viewportKey) {


    std::lock_guard<std::mutex> lock(streamline_hook_g_StateMutex);
    const auto it = streamline_hook_g_ViewportStates.find(viewportKey);
    return it != streamline_hook_g_ViewportStates.end() && it->second.active;

}


bool ShouldSuppressNewGetStateActivation() {


    if (ShouldKeepPureObserverOnlyStreamlineBehavior()) {
        return false;
    }

    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    if (ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationDuringUnsafePostFSRComeback(
            streamline_hook_g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap.load(std::memory_order_acquire),
            safePostFSRBootstrapPath, runtimeMode)) {
        return true;
    }

    if (ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationWhileRuntimeInactive(
            streamline_hook_g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.load(std::memory_order_acquire),
            DXGIShared::IsStreamlineStartupTransitionWindowActive(), runtimeMode)) {
        return true;
    }

    const ULONGLONG suppressUntilMs = streamline_hook_g_SuppressNewGetStateActivationUntilMs.load(std::memory_order_acquire);
    return suppressUntilMs != 0 && GetTickCount64() < suppressUntilMs;

}


bool HasDLSSGRuntimeFenceEvidence(const slDLSSGState& state) {


    return state.inputsProcessingCompletionFence != nullptr ||
           state.lastPresentInputsProcessingCompletionFenceValue != 0;

}


void UpdateViewportRuntimeState(uint32_t viewportKey,  bool active,  int multiplier,  uint32_t generatedFrames, 
                                uint32_t capabilityMax,  const char* source, 
                                bool clearAllViewportStatesForDisable) {


    ViewportFGState previousState{};
    bool hadPreviousState = false;
    bool stateChanged = false;
    bool anyActive = false;
    int combinedMultiplier = 0;
    size_t clearedActiveViewportCount = 0;
    const int configuredMultiplier =
        NormalizeDLSSFGFactor(GetActiveGraphicsConfig().parsed.dlssFGFactor);
    const int publishedMultiplier =
        ce::streamline_runtime_policy::ResolvePublishedDLSSFGMultiplier(
            active, multiplier, configuredMultiplier, capabilityMax);

    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_StateMutex);
        const auto existing = streamline_hook_g_ViewportStates.find(viewportKey);
        if (existing != streamline_hook_g_ViewportStates.end()) {
            previousState = existing->second;
            hadPreviousState = true;
        }

        if (active) {
            streamline_hook_g_ViewportStates[viewportKey] = {
                true, publishedMultiplier, generatedFrames, capabilityMax};
        } else if (clearAllViewportStatesForDisable) {
            clearedActiveViewportCount = streamline_hook_g_ViewportStates.size();
            streamline_hook_g_ViewportStates.clear();
        } else {
            streamline_hook_g_ViewportStates.erase(viewportKey);
        }

        const auto current = streamline_hook_g_ViewportStates.find(viewportKey);
        const ViewportFGState currentState =
            current != streamline_hook_g_ViewportStates.end() ? current->second : ViewportFGState{false, 0, 0, capabilityMax};

        stateChanged = !hadPreviousState || previousState.active != currentState.active ||
                       previousState.multiplier != currentState.multiplier ||
                       previousState.generatedFrames != currentState.generatedFrames ||
                       previousState.capabilityMax != currentState.capabilityMax;

        for (const auto& [_, state] : streamline_hook_g_ViewportStates) {
            if (!state.active) {
                continue;
            }
            anyActive = true;
            combinedMultiplier = std::max(combinedMultiplier, state.multiplier);
        }
    }

    const bool explicitSetOptionsEnableSignal = source && strcmp(source, "SetOptions") == 0 && active;
    ApplyCombinedStreamlineRuntimeState(anyActive, combinedMultiplier, explicitSetOptionsEnableSignal, source);

    if (clearedActiveViewportCount > 0) {
        HookLogImportant(
            "Streamline Hook: Cleared %zu cached DLSSG viewport runtime state(s) after %s disable "
            "(triggerViewport=%u generatedFrames=%u capabilityMax=%u)",
            clearedActiveViewportCount, source ? source : "runtime", viewportKey, generatedFrames, capabilityMax);
    }

    if (stateChanged) {
        HookLog(
            "Streamline Hook: Viewport %u state active=%d multiplier=%dx runtimeMultiplier=%dx "
            "configuredMultiplier=%dx generatedFrames=%u capabilityMax=%u source=%s clearAll=%d",
            viewportKey, active ? 1 : 0, active ? publishedMultiplier : 0,
            active ? multiplier : 0, configuredMultiplier, generatedFrames, capabilityMax,
            source ? source : "runtime", clearAllViewportStatesForDisable ? 1 : 0);
    }

}
