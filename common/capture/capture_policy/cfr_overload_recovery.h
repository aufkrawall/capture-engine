#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "cfr_scheduling.h"

// Backend-neutral CFR overload recovery helpers. The immutable output/audio
// timeline remains owned by the caller; these policies change only which
// pixels are submitted for an already scheduled output slot.

namespace ce::capture_policy {

struct CfrOverloadRepeatRuntime {
    double freshServiceMs = 0.0;
    double repeatServiceMs = 0.0;
    uint32_t freshServiceSamples = 0;
    uint32_t repeatServiceSamples = 0;
    CfrOverloadRepeatPacerState pacer{};
};

inline bool IsCfrSourceHealthyForOverloadPacing(double smoothedInputPerTick, bool predictorCalibrated,
                                                 double predictedSourceFps, double targetOutputFps) {
    if (!std::isfinite(smoothedInputPerTick) || !std::isfinite(predictedSourceFps) ||
        !std::isfinite(targetOutputFps) || targetOutputFps <= 0.0) {
        return false;
    }
    const double healthySourceFloorFps =
        std::max(1.0, targetOutputFps - kCfrOverloadPacerSourceMarginFps);
    return smoothedInputPerTick >= kCfrOverloadPacerSourceMinFramesPerTick &&
           (!predictorCalibrated || predictedSourceFps >= healthySourceFloorFps);
}

inline bool ShouldAllowCfrRepeatCatchupUnderFreshPressure(bool pacerActive, bool muxPressure,
                                                           double repeatServiceMs, double frameIntervalMs,
                                                           uint32_t repeatServiceSamples) {
    return pacerActive && !muxPressure &&
           repeatServiceSamples >= kWgcOverloadRepeatPacerMinSamples &&
           std::isfinite(repeatServiceMs) && std::isfinite(frameIntervalMs) &&
           repeatServiceMs > 0.0 && frameIntervalMs > 0.0 &&
           repeatServiceMs < frameIntervalMs * kCfrRepeatCatchupServiceBudgetRatio;
}

struct CfrDynamicOverlayRepeatState {
    bool frozen = false;
    uint32_t overloadConfirmFrames = 0;
    uint32_t recoveryConfirmFrames = 0;
    uint64_t episodes = 0;
    uint64_t frozenRepeats = 0;

    void Reset() {
        *this = {};
    }
};

struct CfrDynamicOverlayRepeatDecision {
    bool frozen = false;
    bool entered = false;
    bool exited = false;
};

inline CfrDynamicOverlayRepeatDecision UpdateCfrDynamicOverlayRepeatState(
    CfrDynamicOverlayRepeatState& state, bool dynamicOverlayActive, double freshServiceMs,
    double frameIntervalMs) {
    CfrDynamicOverlayRepeatDecision decision{};
    if (!dynamicOverlayActive || !std::isfinite(freshServiceMs) || !std::isfinite(frameIntervalMs) ||
        freshServiceMs <= 0.0 || frameIntervalMs <= 0.0) {
        decision.exited = state.frozen;
        const uint64_t episodes = state.episodes;
        const uint64_t frozenRepeats = state.frozenRepeats;
        state.Reset();
        state.episodes = episodes;
        state.frozenRepeats = frozenRepeats;
        return decision;
    }

    if (!state.frozen) {
        state.recoveryConfirmFrames = 0;
        if (freshServiceMs > frameIntervalMs * kCfrDynamicOverlayRepeatEnterBudgetRatio) {
            if (++state.overloadConfirmFrames >= kCfrDynamicOverlayRepeatEnterConfirmFrames) {
                state.frozen = true;
                state.overloadConfirmFrames = 0;
                ++state.episodes;
                decision.entered = true;
            }
        } else {
            state.overloadConfirmFrames = 0;
        }
    } else {
        state.overloadConfirmFrames = 0;
        if (freshServiceMs < frameIntervalMs * kCfrDynamicOverlayRepeatExitBudgetRatio) {
            if (++state.recoveryConfirmFrames >= kCfrDynamicOverlayRepeatExitConfirmFrames) {
                state.frozen = false;
                state.recoveryConfirmFrames = 0;
                decision.exited = true;
            }
        } else {
            state.recoveryConfirmFrames = 0;
        }
    }
    decision.frozen = state.frozen;
    return decision;
}

}  // namespace ce::capture_policy
