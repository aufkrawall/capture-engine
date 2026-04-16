#pragma once

#include <cstdint>

#include "../../common/shared_defs.h"
#include "fg_runtime_state.h"

namespace ce::streamline_runtime_policy {

struct ViewportRuntimeUpdate {
    bool shouldUpdate = false;
    bool active = false;
    int multiplier = 0;
    uint32_t generatedFrames = 0;
    uint32_t capabilityMax = 0;
};

struct GetStateRuntimeEvaluation {
    ViewportRuntimeUpdate update;
    bool suppressedFreshActivation = false;
};

struct CombinedRuntimeSignalUpdate {
    bool effectiveActive = false;
    int effectiveMultiplier = 0;
    bool deferredOffDuringStartupWindow = false;
    bool freshActivationEdge = false;
    bool shouldExtendStartupTransitionWindow = false;
};

struct ObserverOnlyHeuristicCleanup {
    bool clearRecentTeardownGrace = false;
    bool seedRecentTeardownGrace = false;
    bool resetQueueChangeHeuristic = false;
    bool clearHeuristicFSR = false;
    bool clearNvidiaSmoothMotion = false;
};

inline bool IsDLSSGModeEnabled(uint32_t mode) {
    return mode != 0;
}

inline int ResolveDLSSFGMultiplier(bool active, uint32_t requestedGeneratedFrames) {
    if (!active) {
        return 0;
    }

    const int multiplier = StreamlineGeneratedFramesToDLSSFGMultiplier(requestedGeneratedFrames);
    return multiplier > 0 ? multiplier : 2;
}

inline uint32_t ResolveDLSSFGGeneratedFrames(bool active, uint32_t requestedGeneratedFrames, int multiplier) {
    if (!active) {
        return 0;
    }

    return requestedGeneratedFrames > 0 ? requestedGeneratedFrames : DLSSFGMultiplierToGeneratedFrames(multiplier);
}

inline ViewportRuntimeUpdate BuildViewportRuntimeUpdateFromRequestedOptions(bool callSucceeded, bool hasOptions,
                                                                            uint32_t mode,
                                                                            uint32_t requestedGeneratedFrames,
                                                                            uint32_t capabilityMax) {
    ViewportRuntimeUpdate update;
    update.capabilityMax = capabilityMax;
    if (!callSucceeded || !hasOptions) {
        return update;
    }

    update.shouldUpdate = true;
    update.active = IsDLSSGModeEnabled(mode);
    update.multiplier = ResolveDLSSFGMultiplier(update.active, requestedGeneratedFrames);
    update.generatedFrames = ResolveDLSSFGGeneratedFrames(update.active, requestedGeneratedFrames, update.multiplier);
    return update;
}

inline ViewportRuntimeUpdate BuildViewportRuntimeUpdateFromGetState(
    bool callSucceeded, bool hasOptions, bool viewportWasActive, bool hasRuntimeFenceEvidence,
    bool suppressNewActivation, uint32_t mode, uint32_t requestedGeneratedFrames, uint32_t capabilityMax) {
    ViewportRuntimeUpdate update;
    update.capabilityMax = capabilityMax;
    if (!callSucceeded || !hasOptions) {
        return update;
    }

    const bool active = IsDLSSGModeEnabled(mode);
    if (active && !viewportWasActive && (suppressNewActivation || !hasRuntimeFenceEvidence)) {
        return update;
    }

    update.shouldUpdate = true;
    update.active = active;
    update.multiplier = ResolveDLSSFGMultiplier(active, requestedGeneratedFrames);
    update.generatedFrames = ResolveDLSSFGGeneratedFrames(active, requestedGeneratedFrames, update.multiplier);
    return update;
}

inline GetStateRuntimeEvaluation EvaluateViewportRuntimeUpdateFromGetState(
    bool callSucceeded, bool hasOptions, bool viewportWasActive, bool hasRuntimeFenceEvidence,
    bool suppressNewActivation, uint32_t mode, uint32_t requestedGeneratedFrames, uint32_t capabilityMax) {
    GetStateRuntimeEvaluation evaluation;
    evaluation.update =
        BuildViewportRuntimeUpdateFromGetState(callSucceeded, hasOptions, viewportWasActive, hasRuntimeFenceEvidence,
                                               suppressNewActivation, mode, requestedGeneratedFrames, capabilityMax);

    const bool attemptedFreshActivation = callSucceeded && hasOptions && IsDLSSGModeEnabled(mode) && !viewportWasActive;
    evaluation.suppressedFreshActivation =
        attemptedFreshActivation && suppressNewActivation && !evaluation.update.shouldUpdate;
    return evaluation;
}

inline bool ShouldSuppressFreshGetStateActivationWhileRuntimeInactive(bool persistentSetOptionsBlock,
                                                                      bool startupTransitionWindowActive,
                                                                      ce::fg_runtime::RuntimeMode runtimeMode) {
    const bool runtimeStillInactive =
        runtimeMode == ce::fg_runtime::RuntimeMode::kStreamlineNoFG || runtimeMode == ce::fg_runtime::RuntimeMode::kOff;
    return runtimeStillInactive && (persistentSetOptionsBlock || startupTransitionWindowActive);
}

inline bool ShouldSuppressFreshGetStateActivationDuringUnsafePostFSRComeback(bool blockUntilSafePostFSRBootstrapPath,
                                                                             bool safePostFSRBootstrapPath,
                                                                             ce::fg_runtime::RuntimeMode runtimeMode) {
    const bool runtimeStillInactive =
        runtimeMode == ce::fg_runtime::RuntimeMode::kStreamlineNoFG || runtimeMode == ce::fg_runtime::RuntimeMode::kOff;
    return runtimeStillInactive && blockUntilSafePostFSRBootstrapPath && !safePostFSRBootstrapPath;
}

inline bool ShouldArmStartupTransitionWindowOnFreshActiveSignal(bool active, bool previousSignal) {
    // The startup transition window is only for fresh activation churn around a
    // real handoff/enable edge. Keeping it refreshed by every later active
    // GetState/SetOptions poll causes startup-only OFF deferral to leak into
    // normal steady-state DLSS FG runtime operation.
    return active && !previousSignal;
}

inline CombinedRuntimeSignalUpdate ResolveCombinedRuntimeSignalUpdate(bool requestedActive,
                                                                      bool startupTransitionWindowActive,
                                                                      bool previousSignal, int requestedMultiplier) {
    CombinedRuntimeSignalUpdate update;
    update.deferredOffDuringStartupWindow = !requestedActive && startupTransitionWindowActive;
    update.effectiveActive = update.deferredOffDuringStartupWindow ? previousSignal : requestedActive;
    update.effectiveMultiplier = update.effectiveActive ? requestedMultiplier : 0;
    update.freshActivationEdge = requestedActive && !previousSignal;
    update.shouldExtendStartupTransitionWindow = update.deferredOffDuringStartupWindow && previousSignal;
    return update;
}

inline bool IsLiveFSRRuntimeHandoffSource(bool currentlyAuthoritativeFSRActive, bool currentRuntimeModeIsFSRFG) {
    return currentlyAuthoritativeFSRActive || currentRuntimeModeIsFSRFG;
}

inline bool ShouldPrepareForStreamlineEnableBeforeOriginalCall(bool requestedEnabled,
                                                               bool currentlyAuthoritativeFSRActive,
                                                               bool currentRuntimeModeIsFSRFG,
                                                               bool runtimeOwnsSwapchain) {
    return requestedEnabled && runtimeOwnsSwapchain &&
           IsLiveFSRRuntimeHandoffSource(currentlyAuthoritativeFSRActive, currentRuntimeModeIsFSRFG);
}

inline bool ShouldRequestStreamlineEnablePreparationOnReflexActivation(bool reflexActivating,
                                                                       bool currentlyAuthoritativeFSRActive,
                                                                       bool currentRuntimeModeIsFSRFG,
                                                                       bool runtimeOwnsSwapchain) {
    return reflexActivating && runtimeOwnsSwapchain &&
           IsLiveFSRRuntimeHandoffSource(currentlyAuthoritativeFSRActive, currentRuntimeModeIsFSRFG);
}

inline bool ShouldSuppressSetOptionsOffDuringStartupTransitionWindow(bool requestedDisabled,
                                                                     bool startupTransitionWindowActive) {
    return requestedDisabled && startupTransitionWindowActive;
}

inline bool ShouldTriggerDirectPostSLCallbackAfterStartupWindowExpiry(bool activationPending,
                                                                      bool postSLActiveButUnconfirmed) {
    return activationPending && !postSLActiveButUnconfirmed;
}

inline ObserverOnlyHeuristicCleanup ResolveObserverOnlyHeuristicCleanupForStreamlineSignalTransition(bool active) {
    ObserverOnlyHeuristicCleanup cleanup;
    cleanup.clearRecentTeardownGrace = active;
    cleanup.seedRecentTeardownGrace = !active;
    cleanup.resetQueueChangeHeuristic = true;
    cleanup.clearHeuristicFSR = true;
    cleanup.clearNvidiaSmoothMotion = true;
    return cleanup;
}

inline bool ShouldKeepPureObserverOnlyStreamlineBehavior(bool observerOnlyEnabled, bool observerPolicyOnlyEnabled) {
    return observerOnlyEnabled && !observerPolicyOnlyEnabled;
}

inline bool ShouldPreserveObserverPolicyOnlyStartupTransitionWindow(bool observerOnlyEnabled,
                                                                    bool observerPolicyOnlyEnabled) {
    return observerOnlyEnabled && observerPolicyOnlyEnabled;
}

}  // namespace ce::streamline_runtime_policy
