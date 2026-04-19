#include "dx12_fg_transition_model.h"

#include "fg_session_state.h"

namespace ce::dx12_fg_transition {
namespace {

ObservedRuntimeMode ToObservedRuntimeModeFromSession(ce::fg_runtime::RuntimeMode mode) {
    switch (mode) {
        case ce::fg_runtime::RuntimeMode::kOff:
            return ObservedRuntimeMode::kOff;
        case ce::fg_runtime::RuntimeMode::kStreamlineNoFG:
            return ObservedRuntimeMode::kStreamlineNoFG;
        case ce::fg_runtime::RuntimeMode::kDLSSFG:
            return ObservedRuntimeMode::kDLSSFG;
        case ce::fg_runtime::RuntimeMode::kFSRFG:
            return ObservedRuntimeMode::kFSRFG;
        case ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion:
            return ObservedRuntimeMode::kNvidiaSM;
        case ce::fg_runtime::RuntimeMode::kUnknown:
        default:
            return ObservedRuntimeMode::kUnknown;
    }
}

OverlayRenderMode ToLegacyRenderMode(ce::fg_session::FGOverlayBackendMode mode) {
    switch (mode) {
        case ce::fg_session::FGOverlayBackendMode::kSuppressed:
            return OverlayRenderMode::kSuppressed;
        case ce::fg_session::FGOverlayBackendMode::kStartupBypass:
            return OverlayRenderMode::kStartupBypass;
        case ce::fg_session::FGOverlayBackendMode::kPostSL:
            return OverlayRenderMode::kPostSL;
        case ce::fg_session::FGOverlayBackendMode::kRuntimeOwnedFSRCallback:
            return OverlayRenderMode::kRuntimeOwnedPreSL;
        case ce::fg_session::FGOverlayBackendMode::kPostFSRRecovery:
            return OverlayRenderMode::kRecoveryPostFSROff;
        case ce::fg_session::FGOverlayBackendMode::kNormalPreSL:
        default:
            return OverlayRenderMode::kNormalPreSL;
    }
}

TransitionPhase ToLegacyPhase(ce::fg_session::FGStartupPhase phase, bool runtimeChanged, bool fgChanged,
                              bool previousWasFG, bool currentIsFG, bool currentStreamlineFGRunning,
                              bool previousStreamlineFGRunning, bool recoveringPostFSRNonFG) {
    if (!currentStreamlineFGRunning && previousStreamlineFGRunning && currentIsFG) {
        return TransitionPhase::kSuspended;
    }

    if (recoveringPostFSRNonFG || phase == ce::fg_session::FGStartupPhase::kHandoffPending ||
        phase == ce::fg_session::FGStartupPhase::kChurnWindow) {
        return TransitionPhase::kRecovering;
    }

    if (runtimeChanged && previousWasFG && currentIsFG) {
        return TransitionPhase::kSwitching;
    }
    if (fgChanged && currentIsFG) {
        return TransitionPhase::kEnabling;
    }
    if (fgChanged && !currentIsFG) {
        return TransitionPhase::kDisabling;
    }
    return TransitionPhase::kStable;
}

OverlayRenderMode ResolveRenderMode(const Input& input) {
    if (input.overlaySuppressed) {
        return OverlayRenderMode::kSuppressed;
    }
    if (input.startupBypassActive) {
        return OverlayRenderMode::kStartupBypass;
    }
    if (input.recoveringPostFSRNonFG) {
        return OverlayRenderMode::kRecoveryPostFSROff;
    }
    if (input.streamlineLoaded && !input.streamlineFGRunning &&
        input.runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG) {
        return OverlayRenderMode::kSuspendedFallback;
    }
    if (input.streamlineFGRunning && input.runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG) {
        return OverlayRenderMode::kPostSL;
    }
    if (input.runtimeOwnsSwapchain) {
        return OverlayRenderMode::kRuntimeOwnedPreSL;
    }
    return OverlayRenderMode::kNormalPreSL;
}

TransitionPhase ResolvePhase(const State& state, const Input& input) {
    if (input.streamlineLoaded && !input.streamlineFGRunning && state.previousStreamlineFGRunning) {
        return TransitionPhase::kSuspended;
    }
    if (input.recoveringPostFSRNonFG) {
        return TransitionPhase::kRecovering;
    }

    const bool runtimeChanged = input.runtimeMode != state.previousRuntimeMode;
    const bool fgChanged = input.effectiveFGActive != state.previousFGActive;
    const bool previousWasFG = ce::fg_runtime::IsRuntimeFGActive(state.previousRuntimeMode);
    const bool currentIsFG = ce::fg_runtime::IsRuntimeFGActive(input.runtimeMode);

    if (runtimeChanged && previousWasFG && currentIsFG && state.previousRuntimeMode != input.runtimeMode) {
        return TransitionPhase::kSwitching;
    }
    if (fgChanged && input.effectiveFGActive) {
        return TransitionPhase::kEnabling;
    }
    if (fgChanged && !input.effectiveFGActive) {
        return TransitionPhase::kDisabling;
    }
    return TransitionPhase::kStable;
}

bool ResolveOverlayAllowed(const Input& input, OverlayRenderMode renderMode) {
    if (input.overlaySuppressed) {
        return false;
    }
    return renderMode != OverlayRenderMode::kSuppressed;
}

bool ResolvePostSLCallbackInstall(const Input& input, OverlayRenderMode renderMode) {
    return renderMode == OverlayRenderMode::kPostSL ||
           (renderMode == OverlayRenderMode::kSuspendedFallback && input.streamlineLoaded);
}

SwapchainOwnershipMode ResolveOwnership(const Input& input) {
    if (input.runtimeOwnsSwapchain) {
        return SwapchainOwnershipMode::kRuntimeOwned;
    }
    return SwapchainOwnershipMode::kGameOwned;
}

}  // namespace

ObservedRuntimeMode ToObservedRuntimeMode(ce::fg_runtime::RuntimeMode mode) {
    switch (mode) {
        case ce::fg_runtime::RuntimeMode::kOff:
            return ObservedRuntimeMode::kOff;
        case ce::fg_runtime::RuntimeMode::kStreamlineNoFG:
            return ObservedRuntimeMode::kStreamlineNoFG;
        case ce::fg_runtime::RuntimeMode::kDLSSFG:
            return ObservedRuntimeMode::kDLSSFG;
        case ce::fg_runtime::RuntimeMode::kFSRFG:
            return ObservedRuntimeMode::kFSRFG;
        case ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion:
            return ObservedRuntimeMode::kNvidiaSM;
        case ce::fg_runtime::RuntimeMode::kUnknown:
        default:
            return ObservedRuntimeMode::kUnknown;
    }
}

State Reduce(const State& state, const Input& input) {
    State next = state;
    Snapshot snapshot = state.snapshot;

    const ce::fg_session::FGSessionSnapshot sessionSnapshot = ce::fg_session::CaptureFGSessionSnapshot();
    const ce::fg_session::FGActionPlan actionPlan = ce::fg_session::BuildFGActionPlan(sessionSnapshot);

    const bool runtimeChanged = input.runtimeMode != state.previousRuntimeMode;
    const bool fgChanged = input.effectiveFGActive != state.previousFGActive;
    const bool previousWasFG = ce::fg_runtime::IsRuntimeFGActive(state.previousRuntimeMode);
    const bool currentIsFG = ce::fg_runtime::IsRuntimeFGActive(input.runtimeMode);

    snapshot.observedMode = ToObservedRuntimeModeFromSession(sessionSnapshot.effectiveRuntimeMode);
    snapshot.ownership = ResolveOwnership(input);
    snapshot.renderMode = ResolveRenderMode(input);
    snapshot.phase = ResolvePhase(state, input);
    snapshot.overlayAllowed = ResolveOverlayAllowed(input, snapshot.renderMode);
    snapshot.shouldInstallPostSLCallback = ResolvePostSLCallbackInstall(input, snapshot.renderMode);
    snapshot.shouldSuppressHeuristics = snapshot.phase == TransitionPhase::kSuspended ||
                                        snapshot.phase == TransitionPhase::kDisabling ||
                                        snapshot.phase == TransitionPhase::kSwitching || input.recoveringPostFSRNonFG;
    snapshot.publishFGActive = actionPlan.publishFGActive;
    snapshot.publishRuntimeMode = actionPlan.publishRuntimeMode;
    snapshot.epoch = sessionSnapshot.sessionEpoch;

    const bool changed = snapshot.observedMode != state.snapshot.observedMode ||
                         snapshot.renderMode != state.snapshot.renderMode ||
                         snapshot.ownership != state.snapshot.ownership || snapshot.phase != state.snapshot.phase ||
                         snapshot.shouldInstallPostSLCallback != state.snapshot.shouldInstallPostSLCallback ||
                         snapshot.publishFGActive != state.snapshot.publishFGActive ||
                         snapshot.publishRuntimeMode != state.snapshot.publishRuntimeMode ||
                         snapshot.overlayAllowed != state.snapshot.overlayAllowed ||
                         snapshot.shouldSuppressHeuristics != state.snapshot.shouldSuppressHeuristics;
    if (changed && snapshot.epoch == 0) {
        snapshot.epoch = state.snapshot.epoch + 1;
    }

    next.snapshot = snapshot;
    next.previousFGActive = input.effectiveFGActive;
    next.previousRuntimeMode = input.runtimeMode;
    next.previousStreamlineFGRunning = input.streamlineFGRunning;
    return next;
}

}  // namespace ce::dx12_fg_transition
