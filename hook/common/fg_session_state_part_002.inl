    valid &=
        ValidateInvariant(current.startupPhase != FGStartupPhase::kStable || !current.postSLStartupActivationPending,
                          "stableStartupPhaseClearsPendingActivation", before, current, false);
    if (previous) {
        const bool runtimeChanged = previous->effectiveRuntimeMode != current.effectiveRuntimeMode;
        valid &= ValidateInvariant(!runtimeChanged || current.postSLStableFrameCount == 0,
                                   "runtimeChangeResetsPostSLStableFrames", before, current, false);
    }
    return valid;
}

void UpdateEpochs(SessionState& state, SnapshotBuildResult* result) {
    if (!result) {
        return;
    }

    FGSessionSnapshot& snapshot = result->snapshot;

    if (!state.initialized) {
        state.sessionEpochCounter = 1;
        state.runtimeEpochCounter = 1;
        state.swapchainEpochCounter = 1;
        state.queueEpochCounter = 1;
        snapshot.sessionEpoch = state.sessionEpochCounter;
        snapshot.runtimeEpoch = state.runtimeEpochCounter;
        snapshot.swapchainEpoch = state.swapchainEpochCounter;
        snapshot.queueEpoch = state.queueEpochCounter;
        state.initialized = true;
        return;
    }

    if (result->sessionChanged) {
        state.sessionEpochCounter++;
    }
    if (result->runtimeChanged) {
        state.runtimeEpochCounter++;
    }
    if (result->swapchainChanged) {
        state.swapchainEpochCounter++;
    }
    if (result->queueChanged) {
        state.queueEpochCounter++;
    }

    snapshot.sessionEpoch = state.sessionEpochCounter;
    snapshot.runtimeEpoch = state.runtimeEpochCounter;
    snapshot.swapchainEpoch = state.swapchainEpochCounter;
    snapshot.queueEpoch = state.queueEpochCounter;

    const uint32_t queueEpoch = snapshot.queueEpoch;
    snapshot.originalGameQueue.epoch = queueEpoch;
    snapshot.primaryGameQueue.epoch = queueEpoch;
    snapshot.swapchainQueue.epoch = queueEpoch;
    snapshot.currentCommandQueue.epoch = queueEpoch;
    snapshot.slWrapperQueue.epoch = queueEpoch;
    snapshot.realQueueBehindWrapper.epoch = queueEpoch;
    snapshot.postSLLockedQueue.epoch = queueEpoch;
    snapshot.postSLLastWorkingQueue.epoch = queueEpoch;
    snapshot.postSLDedicatedQueue.epoch = queueEpoch;
    snapshot.realECL.epoch = queueEpoch;
    snapshot.presentHookAnchor.epoch = snapshot.swapchainEpoch;
}

void CommitState(SessionState& state, const FGSessionSnapshot& snapshot, const FGActionPlan& plan) {
    state.latestSnapshot = snapshot;
    state.latestPlan = plan;
    state.lastRuntimeMode = snapshot.effectiveRuntimeMode;
    state.lastRuntimeOwnsSwapchain = snapshot.runtimeOwnsSwapchain;
    state.lastSwapchainQueuePtr = snapshot.swapchainQueue.ptr;
    state.lastOriginalQueuePtr = snapshot.originalGameQueue.ptr;
    state.lastCurrentQueuePtr = snapshot.currentCommandQueue.ptr;
    state.lastAuthority = snapshot.authority;
    state.lastStartupPhase = snapshot.startupPhase;
}

void RefreshStateLocked(SessionState& state, const char* trigger, bool emitLogs) {
    const bool hadPrevious = state.initialized;
    SnapshotBuildResult buildResult = BuildSnapshotNoLock(state);
    UpdateEpochs(state, &buildResult);
    FGSessionSnapshot& snapshot = buildResult.snapshot;
    snapshot.authority = ResolveAuthorityKind(snapshot);
    snapshot.overlayMode = ResolveOverlayBackendMode(snapshot);
    snapshot.startupPhase = ResolveStartupPhase(snapshot);

    const FGSessionSnapshot previousSnapshot = state.latestSnapshot;
    const FGActionPlan previousPlan = state.latestPlan;

    const FGActionPlan plan = BuildPlanNoLog(snapshot);

    const bool transitionLike = !hadPrevious || buildResult.runtimeChanged ||
                                snapshot.startupPhase != previousSnapshot.startupPhase ||
                                snapshot.authority != previousSnapshot.authority;
    if (transitionLike) {
        state.presentDecisionLogsRemaining = 10;
    }

    if (emitLogs && buildResult.sessionChanged) {
        ValidateSnapshotAgainstPrevious(snapshot, hadPrevious ? &previousSnapshot : nullptr);
        LogSnapshotLine(snapshot);
        LogPlanLine(snapshot, plan);
        if (hadPrevious) {
            LogPlanDiffIfNeeded(previousSnapshot, previousPlan, snapshot, plan);
            LogTransitionIfNeeded(previousSnapshot, previousPlan, snapshot, plan, trigger);
        }
    }
    CommitState(state, snapshot, plan);
    UpdateSessionManifestIfNeeded(state, snapshot);
}

}  // namespace

void RegisterDX12LegacyStateProvider(DX12LegacyStateProvider provider) {
    SessionState& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.dx12Provider = provider;
}

FGSessionSnapshot CaptureFGSessionSnapshot() {
    SessionState& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    RefreshStateLocked(state, "snapshot", false);
    return state.latestSnapshot;
}

FGActionPlan BuildFGActionPlan(const FGSessionSnapshot& snapshot) {
    return BuildPlanNoLog(snapshot);
}

bool ValidateFGSessionSnapshot(const FGSessionSnapshot& current, const FGSessionSnapshot* previous) {
    return ValidateSnapshotAgainstPrevious(current, previous);
}

void EmitFGEvent(const FGEvent& event) {
    SessionState& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);

    const bool isPresentObserved = event.kind == FGEventKind::kPresentObserved;
    RefreshStateLocked(state, event.source ? event.source : GetFGEventKindName(event.kind), !isPresentObserved);

    const FGSessionSnapshot& snapshot = state.latestSnapshot;
    if (isPresentObserved) {
        if (state.presentDecisionLogsRemaining > 0) {
            LogLegacyDecisionLine(snapshot, state.latestPlan);
            state.presentDecisionLogsRemaining--;
        }
        return;
    }

    if (!ShouldLogFGEventLocked(state, event)) {
        return;
    }

    HookLogImportant(
        "FG EVENT kind=%s source=%s ptrA=%p ptrB=%p runtime=%s active=%d explicit=%d ts=%llu sessionEpoch=%u "
        "runtimeEpoch=%u swapchainEpoch=%u queueEpoch=%u",
        GetFGEventKindName(event.kind), SafeString(event.source), event.ptrA, event.ptrB,
        fg_runtime::GetRuntimeModeName(event.hintedRuntimeMode), event.hintedActive ? 1 : 0,
        event.hintedExplicitActivation ? 1 : 0, static_cast<unsigned long long>(event.timestampMs),
        snapshot.sessionEpoch, snapshot.runtimeEpoch, snapshot.swapchainEpoch, snapshot.queueEpoch);
}

void EmitFGEvent(FGEventKind kind, const char* source, void* ptrA, void* ptrB,
                 fg_runtime::RuntimeMode hintedRuntimeMode, bool hintedActive, bool hintedExplicitActivation) {
    const FGSessionSnapshot snapshot = CaptureFGSessionSnapshot();
    FGEvent event;
    event.kind = kind;
    event.source = source;
    event.ptrA = ptrA;
    event.ptrB = ptrB;
    event.hintedRuntimeMode = hintedRuntimeMode;
    event.hintedActive = hintedActive;
    event.hintedExplicitActivation = hintedExplicitActivation;
    event.timestampMs = GetTickCount64();
    event.sessionEpoch = snapshot.sessionEpoch;
    event.runtimeEpoch = snapshot.runtimeEpoch;
    event.swapchainEpoch = snapshot.swapchainEpoch;
    event.queueEpoch = snapshot.queueEpoch;
    EmitFGEvent(event);
}

FGSessionSnapshot GetLatestFGSessionSnapshot() {
    return CaptureFGSessionSnapshot();
}

FGActionPlan GetLatestFGActionPlan() {
    SessionState& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    RefreshStateLocked(state, "latest-plan", false);
    return state.latestPlan;
}

bool IsFGShadowStateEnabled() {
    return true;
}

uint32_t GetFGStateSchemaVersion() {
    return kFGStateSchemaVersion;
}

void ResetFGSessionStateForTests() {
    SessionState& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.dx12Provider = nullptr;
    state.latestSnapshot = FGSessionSnapshot{};
    state.latestPlan = FGActionPlan{};
    state.initialized = false;
    state.sessionEpochCounter = 0;
    state.runtimeEpochCounter = 0;
    state.swapchainEpochCounter = 0;
    state.queueEpochCounter = 0;
    state.lastRuntimeMode = fg_runtime::RuntimeMode::kOff;
    state.lastRuntimeOwnsSwapchain = false;
    state.lastSwapchainQueuePtr = nullptr;
    state.lastOriginalQueuePtr = nullptr;
    state.lastCurrentQueuePtr = nullptr;
    state.lastAuthority = FGAuthorityKind::kNone;
    state.lastStartupPhase = FGStartupPhase::kNone;
    state.lastManifestSteamOverlayLoaded = false;
    state.lastManifestStreamlineLoaded = false;
    state.lastManifestFFXLoaded = false;
    state.lastManifestShadowEnabled = false;
    state.lastManifestSchemaVersion = 0;
    state.manifestInitialized = false;
    state.presentDecisionLogsRemaining = 0;
    state.runtimeUpdateEventLogValid = false;
    state.lastRuntimeUpdateEventRuntime = fg_runtime::RuntimeMode::kUnknown;
    state.lastRuntimeUpdateEventActive = false;
    state.lastRuntimeUpdateEventExplicit = false;
    state.runtimeUpdateEventLogCount = 0;
}

const char* GetFGAuthorityKindName(FGAuthorityKind kind) {
    switch (kind) {
        case FGAuthorityKind::kNone:
            return "none";
        case FGAuthorityKind::kStreamlineGetStateProvisional:
            return "sl-getstate";
        case FGAuthorityKind::kStreamlineSetOptionsAuthoritative:
            return "sl-setoptions";
        case FGAuthorityKind::kNativeFSRConfigureAuthoritative:
            return "native-fsr-configure";
        case FGAuthorityKind::kNativeFSRContextOnly:
            return "native-fsr-context";
        case FGAuthorityKind::kHeuristic:
            return "heuristic";
        default:
            return "unknown";
    }
}

const char* GetFGStartupPhaseName(FGStartupPhase phase) {
    switch (phase) {
        case FGStartupPhase::kNone:
            return "none";
        case FGStartupPhase::kHandoffPending:
            return "handoffPending";
        case FGStartupPhase::kChurnWindow:
            return "churnWindow";
        case FGStartupPhase::kActivationPending:
            return "activationPending";
        case FGStartupPhase::kActiveUnconfirmed:
            return "activeUnconfirmed";
        case FGStartupPhase::kSettling:
            return "settling";
        case FGStartupPhase::kStable:
            return "stable";
        default:
            return "unknown";
    }
}

const char* GetFGOverlayBackendModeName(FGOverlayBackendMode mode) {
    switch (mode) {
        case FGOverlayBackendMode::kSuppressed:
            return "suppressed";
        case FGOverlayBackendMode::kNormalPreSL:
            return "normalPreSL";
        case FGOverlayBackendMode::kStartupBypass:
            return "startupBypass";
        case FGOverlayBackendMode::kPostSL:
            return "postSL";
        case FGOverlayBackendMode::kRuntimeOwnedFSRCallback:
            return "runtimeOwnedFSRCallback";
        case FGOverlayBackendMode::kPostFSRRecovery:
            return "postFSRRecovery";
        default:
            return "unknown";
    }
}

const char* GetFGPresentRouteName(FGPresentRoute route) {
    switch (route) {
        case FGPresentRoute::kTopLevel:
            return "topLevel";
        case FGPresentRoute::kSyntheticReentrant:
            return "syntheticReentrant";
        case FGPresentRoute::kStartupHandoffNormalRoute:
            return "startupHandoffNormalRoute";
        case FGPresentRoute::kConfirmedStandaloneNormalRoute:
            return "confirmedStandaloneNormalRoute";
        case FGPresentRoute::kPassiveBypass:
            return "passiveBypass";
        default:
            return "unknown";
    }
}

const char* GetFGPresentTransportName(FGPresentTransport transport) {
    switch (transport) {
        case FGPresentTransport::kNormalChain:
            return "normalChain";
        case FGPresentTransport::kTrampoline:
            return "trampoline";
        case FGPresentTransport::kDirectBypass:
            return "directBypass";
        default:
            return "unknown";
    }
}

const char* GetFGQueueRoleName(FGQueueRole role) {
    switch (role) {
        case FGQueueRole::kNone:
            return "none";
        case FGQueueRole::kOriginalGame:
            return "originalGame";
        case FGQueueRole::kSwapchain:
            return "swapchain";
        case FGQueueRole::kWrapperBootstrap:
            return "wrapperBootstrap";
        case FGQueueRole::kRealBehindWrapper:
            return "realBehindWrapper";
        case FGQueueRole::kDedicatedOverlayQueue:
            return "dedicatedOverlayQueue";
        case FGQueueRole::kPostSLLastWorking:
            return "postSLLastWorking";
        case FGQueueRole::kFFXCallbackQueue:
            return "ffxCallbackQueue";
        default:
            return "unknown";
    }
}

const char* GetFGEventKindName(FGEventKind kind) {
    switch (kind) {
        case FGEventKind::kUnknown:
            return "unknown";
        case FGEventKind::kStreamlineGetStateRuntimeUpdate:
            return "streamline-getstate-runtime-update";
        case FGEventKind::kStreamlineSetOptionsRuntimeUpdate:
            return "streamline-setoptions-runtime-update";
        case FGEventKind::kAuthoritativeStreamlineStartupHandoff:
            return "authoritative-streamline-startup-handoff";
        case FGEventKind::kAuthoritativeFFXTakeover:
            return "authoritative-ffx-takeover";
        case FGEventKind::kNativeFSRConfigureOn:
            return "native-fsr-configure-on";
        case FGEventKind::kNativeFSRConfigureOff:
            return "native-fsr-configure-off";
        case FGEventKind::kFFXContextDestroy:
            return "ffx-context-destroy";
        case FGEventKind::kSwapchainInvalidation:
            return "swapchain-invalidation";
        case FGEventKind::kPresentObserved:
            return "present-observed";
        case FGEventKind::kPostSLCallbackInstalled:
            return "postsl-callback-installed";
        case FGEventKind::kPostSLCallbackRemoved:
            return "postsl-callback-removed";
        case FGEventKind::kPostSLActivationComplete:
            return "postsl-activation-complete";
        case FGEventKind::kPostSLFirstConfirmedRender:
            return "postsl-first-confirmed-render";
        case FGEventKind::kStartupWindowExpired:
            return "startup-window-expired";
        case FGEventKind::kStaleOwnershipCleanupComplete:
            return "stale-ownership-cleanup-complete";
        case FGEventKind::kTransitionCooldownComplete:
            return "transition-cooldown-complete";
        default:
            return "unknown";
    }
}

}  // namespace ce::fg_session
