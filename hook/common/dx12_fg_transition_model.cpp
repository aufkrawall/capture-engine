#include "dx12_fg_transition_model.h"

namespace ce::dx12_fg_transition {
namespace {

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

    snapshot.observedMode = ToObservedRuntimeMode(input.runtimeMode);
    snapshot.ownership = ResolveOwnership(input);
    snapshot.renderMode = ResolveRenderMode(input);
    snapshot.phase = ResolvePhase(state, input);
    snapshot.overlayAllowed = ResolveOverlayAllowed(input, snapshot.renderMode);
    snapshot.shouldInstallPostSLCallback = ResolvePostSLCallbackInstall(input, snapshot.renderMode);
    snapshot.shouldSuppressHeuristics = snapshot.phase == TransitionPhase::kSuspended ||
                                        snapshot.phase == TransitionPhase::kDisabling ||
                                        snapshot.phase == TransitionPhase::kSwitching || input.recoveringPostFSRNonFG;
    snapshot.publishFGActive = input.effectiveFGActive;
    snapshot.publishRuntimeMode = input.effectiveFGActive ? input.runtimeMode : ce::fg_runtime::RuntimeMode::kOff;

    const bool changed = snapshot.observedMode != state.snapshot.observedMode ||
                         snapshot.renderMode != state.snapshot.renderMode ||
                         snapshot.ownership != state.snapshot.ownership || snapshot.phase != state.snapshot.phase ||
                         snapshot.shouldInstallPostSLCallback != state.snapshot.shouldInstallPostSLCallback ||
                         snapshot.publishFGActive != state.snapshot.publishFGActive ||
                         snapshot.publishRuntimeMode != state.snapshot.publishRuntimeMode ||
                         snapshot.overlayAllowed != state.snapshot.overlayAllowed ||
                         snapshot.shouldSuppressHeuristics != state.snapshot.shouldSuppressHeuristics;
    if (changed) {
        snapshot.epoch = state.snapshot.epoch + 1;
    }

    next.snapshot = snapshot;
    next.previousFGActive = input.effectiveFGActive;
    next.previousRuntimeMode = input.runtimeMode;
    next.previousStreamlineFGRunning = input.streamlineFGRunning;
    return next;
}

}  // namespace ce::dx12_fg_transition
