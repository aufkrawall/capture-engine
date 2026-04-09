#pragma once

#include <cstdint>

#include "../../common/shared_defs.h"

namespace ce::streamline_runtime_policy {

struct ViewportRuntimeUpdate {
    bool shouldUpdate = false;
    bool active = false;
    int multiplier = 0;
    uint32_t generatedFrames = 0;
    uint32_t capabilityMax = 0;
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

inline bool IsLiveFSRRuntimeHandoffSource(bool currentlyAuthoritativeFSRActive, bool currentRuntimeModeIsFSRFG) {
    return currentlyAuthoritativeFSRActive || currentRuntimeModeIsFSRFG;
}

inline bool ShouldPrepareForStreamlineEnableBeforeOriginalCall(bool requestedEnabled, bool currentlyAuthoritativeFSRActive,
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

}  // namespace ce::streamline_runtime_policy
