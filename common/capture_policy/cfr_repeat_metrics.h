#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>

#include "ingress_and_active_delay.h"

// CFR repeat lower bounds, smoothness faults, and fresh-frame catch-up selection.

namespace ce::capture_policy {

struct WgcCfrRepeatLowerBound {
    uint32_t unavoidableRepeats = 0;
    uint32_t excessRepeats = 0;
    uint32_t excessPermille = 0;
};

inline WgcCfrRepeatLowerBound EstimateWgcCfrSourceRepeatLowerBound(uint32_t outputTicks, uint32_t uniqueSourceFrames,
                                                                   uint32_t actualRepeats) {
    WgcCfrRepeatLowerBound result{};
    if (outputTicks == 0) {
        return result;
    }

    const uint32_t usableSourceFrames = std::min(outputTicks, uniqueSourceFrames);
    result.unavoidableRepeats = outputTicks - usableSourceFrames;
    result.excessRepeats = actualRepeats > result.unavoidableRepeats ? (actualRepeats - result.unavoidableRepeats) : 0;
    result.excessPermille =
        static_cast<uint32_t>((static_cast<uint64_t>(result.excessRepeats) * 1000ull) / outputTicks);
    return result;
}

inline bool IsWgcCfrSmoothnessNotMaximal(uint32_t totalOutputTicks, uint32_t excessRepeats, uint32_t policyAddedRepeats,
                                         uint32_t excessRepeatClusterMaxTicks, uint32_t postSelectionRejectedSync) {
    if (postSelectionRejectedSync > 0) {
        return true;
    }
    if (excessRepeatClusterMaxTicks >= kWgcCfrSmoothnessExcessRepeatClusterFaultTicks) {
        return true;
    }
    if (policyAddedRepeats >= kWgcCfrSmoothnessExcessRepeatFaultMinCount) {
        return true;
    }
    if (totalOutputTicks > 0 && policyAddedRepeats >= kWgcCfrSmoothnessPolicyRepeatNoticeMinCount) {
        const uint64_t policyPermille =
            (static_cast<uint64_t>(policyAddedRepeats) * 1000ull) / static_cast<uint64_t>(totalOutputTicks);
        if (policyPermille >= kWgcCfrSmoothnessPolicyRepeatNoticePermille) {
            return true;
        }
    }
    if (totalOutputTicks == 0 || excessRepeats < kWgcCfrSmoothnessExcessRepeatFaultMinCount) {
        return false;
    }
    const uint64_t excessPermille =
        (static_cast<uint64_t>(excessRepeats) * 1000ull) / static_cast<uint64_t>(totalOutputTicks);
    return excessPermille >= kWgcCfrSmoothnessExcessRepeatFaultPermille;
}

inline bool IsWgcDelayReservoirBelowLowWater(size_t bufferedFrames, int64_t contentDelayQpc,
                                             int64_t targetIntervalTicks) {
    const uint32_t lowWaterFrames = GetWgcDelayReservoirLowWaterFrames(contentDelayQpc, targetIntervalTicks);
    return lowWaterFrames > 0 && bufferedFrames < lowWaterFrames;
}

inline bool IsWgcDelayReservoirRecovered(size_t bufferedFrames, int64_t contentDelayQpc, int64_t targetIntervalTicks) {
    const uint32_t targetFrames = GetWgcDelayReservoirTargetFrames(contentDelayQpc, targetIntervalTicks);
    return targetFrames == 0 || bufferedFrames >= targetFrames;
}

inline bool ShouldPreserveWgcStartupPartialReserve(size_t candidateCount, int64_t reserveSpanQpc,
                                                   bool contentDelayActive, bool waitTimedOut) {
    return contentDelayActive && waitTimedOut && candidateCount > 1 && reserveSpanQpc > 0;
}

inline bool IsWgcFrameWithinLiveVisualDebtWindow(int64_t frameSelectionQpc, int64_t liveNowQpc,
                                                 int64_t targetIntervalTicks, int64_t qpcTicksPerSecond,
                                                 bool encoderLimitedSmoothnessMode = false,
                                                 int64_t intentionalContentDelayQpc = 0) {
    if (frameSelectionQpc <= 0) {
        return true;
    }

    const int64_t visualDebtFloorQpc = GetWgcLiveVisualDebtFloorQpcForMode(
        liveNowQpc, targetIntervalTicks, qpcTicksPerSecond, encoderLimitedSmoothnessMode, intentionalContentDelayQpc);
    return visualDebtFloorQpc <= 0 || frameSelectionQpc >= visualDebtFloorQpc;
}

inline bool ShouldUseFreshWgcCatchupFrame(int64_t frameSelectionQpc, int64_t liveNowQpc, int64_t targetIntervalTicks,
                                          int64_t qpcTicksPerSecond, uint32_t outputShortfallTicks) {
    (void)frameSelectionQpc;
    (void)liveNowQpc;
    (void)targetIntervalTicks;
    (void)qpcTicksPerSecond;
    if (outputShortfallTicks == 0) {
        return true;
    }

    // Extra WGC catch-up ticks represent old CFR debt.  Encoding a fresh frame
    // for those slots is a fast-forward/content-shift bug, even when final mux
    // durations remain equal.  Old WGC debt must be absorbed by holds/drops.
    return false;
}

inline bool ShouldKeepWgcFrameForStopDrain(int64_t sourceFrameQpc, int64_t stopQpc) {
    if (sourceFrameQpc <= 0 || stopQpc <= 0) {
        return true;
    }

    return sourceFrameQpc <= stopQpc;
}

inline int64_t GetWgcMinimumFreshTimestampQpc(int64_t lastEmittedSourceQpc, int64_t scheduledSampleQpc,
                                              int64_t targetIntervalTicks, bool lowSourceMode) {
    int64_t minFreshTimestampQpc = lastEmittedSourceQpc > 0 ? (lastEmittedSourceQpc + 1) : 0;
    const int64_t maxSelectionLagQpc = GetWgcMaxSelectionLagQpc(targetIntervalTicks, lowSourceMode);
    if (scheduledSampleQpc > 0 && maxSelectionLagQpc > 0) {
        minFreshTimestampQpc = std::max(minFreshTimestampQpc, scheduledSampleQpc - maxSelectionLagQpc);
    }
    return minFreshTimestampQpc;
}

inline int64_t GetWgcStaleUniqueFallbackMinTimestampQpc(int64_t lastEmittedSourceQpc, int64_t selectionTargetQpc,
                                                        int64_t targetIntervalTicks, bool lowSourceMode,
                                                        bool deepUnderfeed) {
    int64_t minTimestampQpc = lastEmittedSourceQpc > 0 ? (lastEmittedSourceQpc + 1) : 0;
    const int64_t maxSelectionLagQpc = GetWgcMaxSelectionLagQpc(targetIntervalTicks, lowSourceMode);
    if (selectionTargetQpc > 0 && maxSelectionLagQpc > 0 && targetIntervalTicks > 0) {
        const int64_t extraLagTicks = deepUnderfeed ? static_cast<int64_t>(kWgcDeepUnderfeedStaleFallbackLagTicks) : 1;
        minTimestampQpc =
            std::max(minTimestampQpc, selectionTargetQpc - maxSelectionLagQpc - (targetIntervalTicks * extraLagTicks));
    }
    return minTimestampQpc;
}

inline bool IsWgcTimestampFreshEnough(int64_t frameTimestampQpc, int64_t minFreshTimestampQpc) {
    return frameTimestampQpc > 0 && (minFreshTimestampQpc <= 0 || frameTimestampQpc >= minFreshTimestampQpc);
}

inline bool ShouldPreferEarlierFreshWgcFrameToPreserveReserve(int64_t earlierFrameTimestampQpc,
                                                              int64_t selectedFrameTimestampQpc,
                                                              int64_t selectionTargetQpc, int64_t targetIntervalTicks,
                                                              bool reservePressureActive, bool lowSourceMode,
                                                              bool deepUnderfeed, bool liveRecoveryMode = false) {
    if (earlierFrameTimestampQpc <= 0 || selectedFrameTimestampQpc <= 0 || selectionTargetQpc <= 0 ||
        targetIntervalTicks <= 0) {
        return false;
    }

    if (liveRecoveryMode) {
        return false;
    }

    if (reservePressureActive || lowSourceMode) {
        return false;
    }

    const int64_t earlierDistance = earlierFrameTimestampQpc >= selectionTargetQpc
                                        ? (earlierFrameTimestampQpc - selectionTargetQpc)
                                        : (selectionTargetQpc - earlierFrameTimestampQpc);
    const int64_t selectedDistance = selectedFrameTimestampQpc >= selectionTargetQpc
                                         ? (selectedFrameTimestampQpc - selectionTargetQpc)
                                         : (selectionTargetQpc - selectedFrameTimestampQpc);
    const uint32_t biasPermille = deepUnderfeed                              ? kWgcDeepUnderfeedReserveBiasPermille
                                  : (reservePressureActive || lowSourceMode) ? kWgcReserveFragileBiasPermille
                                                                             : kWgcReserveBiasPermille;
    const int64_t reserveBiasQpc =
        std::max<int64_t>((targetIntervalTicks * static_cast<int64_t>(biasPermille)) / 1000, 1);
    return earlierDistance <= (selectedDistance + reserveBiasQpc);
}

// Uniform-cadence mode is active when an A/V content delay is being applied to the WGC selection
// (selectionDelayApplied) AND the config opts in. In this mode WGC paces the active-delay output
// like the inject path: it keeps a frame-count delay floor and advances unique frames at the
// SOURCE input rate (Bresenham), so a VRR/under-delivering source (present rate dipping below the
// output target) keeps the buffer fed and the ~delay-old frame available, with the unavoidable
// source-limited repeats distributed evenly instead of clustered into delay-slot holds. This
// preserves A/V sync (the realized delay stays ~floor frames deep) while staying smooth.
inline bool IsWgcActiveDelayUniformCadenceMode(bool selectionDelayApplied, bool uniformCadenceConfigEnabled) {
    return selectionDelayApplied && uniformCadenceConfigEnabled;
}

// Result of the inject-parity input-rate pacing decision for the WGC active-delay floor model.
struct WgcActiveDelayPaceResult {
    bool advance = false;            // pop one unique frame for this output tick
    uint32_t dropBeforeAdvance = 0;  // even-decimation drops when the source outran the output
    uint32_t capDrops = 0;           // subset of dropBeforeAdvance forced by the reservoir depth cap
    double creditConsumed = 0.0;     // amount to subtract from the running credit accumulator
};

// Maximum buffered depth the uniform-cadence active-delay pacer allows before trimming the oldest
// surplus. The realized content delay is ~(depth-1) source-intervals (the emitted/oldest frame is
// behind the newest by depth-1 frames), so the reservoir target depth (floor+extra) realizes the
// floor-deep delay; the cap is that target plus a small jitter band. Bounding the depth bounds the
// realized content delay, which is the fix for the unbounded inflation observed when a VRR source
// transiently delivers above the output rate (realized delay drifting 31ms -> 248ms).
inline size_t GetWgcActiveDelayPaceMaxDepthFrames(int64_t contentDelayQpc, int64_t targetIntervalTicks) {
    const uint32_t targetFrames = GetWgcDelayReservoirTargetFrames(contentDelayQpc, targetIntervalTicks);
    const size_t target = targetFrames > 0 ? static_cast<size_t>(targetFrames) : 1u;
    return target + static_cast<size_t>(kWgcActiveDelayPaceMaxExcessFrames);
}

// Decide whether to advance/drop/hold for the legacy WGC active-delay source-rate matcher. With
// credit already incremented by the source unique-frames-per-tick rate, decimate evenly while the
// source is ahead (credit >= 2, keep floor+1), advance one unique frame when credit >= 1 and the
// buffer is above the delay floor, otherwise hold (an evenly distributed source-limited repeat).
// The delay floor (frames kept) realizes the content delay, so the emitted frame is always ~floor
// source-frames old; it can never be "too new" for the slot, which is why this policy needs no
// per-tick reserve defense. The live timestamp-target paths below do not use this count model.
//
// Setpoint restoring drain (maxDepthFrames): the pure source-rate matcher has NO restoring force
// toward the floor, so any transient where the source outran the output (a VRR / GPU-bound present
// rate briefly above the CFR target) inflates the buffer and the inflation never drains back -- the
// realized content delay then drifts upward without bound until a starvation event empties it. Trim
// the oldest excess down to maxDepthFrames every tick so the realized delay is pinned at ~floor.
// Because the surplus arrives gradually (a fraction of a frame per tick), this trims evenly and
// stays smooth; it never trims into floor+1, so it cannot starve the delay reserve.
inline WgcActiveDelayPaceResult DecideWgcActiveDelayPace(double creditAfterIncrement, size_t bufferedFrames,
                                                         size_t floorFrames, size_t maxDepthFrames) {
    WgcActiveDelayPaceResult result;
    size_t available = bufferedFrames;
    const size_t depthCap = std::max(maxDepthFrames, floorFrames + 1);
    while (available > depthCap) {
        ++result.dropBeforeAdvance;
        ++result.capDrops;
        --available;
    }
    while ((creditAfterIncrement - result.creditConsumed) >= 2.0 && available > floorFrames + 1) {
        ++result.dropBeforeAdvance;
        --available;
        result.creditConsumed += 1.0;
    }
    if ((creditAfterIncrement - result.creditConsumed) >= 1.0 && available > floorFrames) {
        result.advance = true;
        result.creditConsumed += 1.0;
    }
    return result;
}

// Delay floor (frames retained ahead of the emitted frame) for the inject-parity WGC pacer. This
// is the WGC analogue of injectContentDelayFrames; the realized content delay is ~floor source
// frames. Reuses the existing reservoir delay-frame derivation so WGC and the timestamp-target
// path agree on the nominal depth.
inline size_t GetWgcActiveDelayPaceFloorFrames(int64_t contentDelayQpc, int64_t targetIntervalTicks) {
    const uint32_t delayFrames = GetWgcDelayReservoirDelayFrames(contentDelayQpc, targetIntervalTicks);
    return delayFrames > 0 ? static_cast<size_t>(delayFrames) : 1u;
}

// Final reserve-defense decision used by the WGC active-delay selector. The older (reserve-
// building) frame is preferred over the closest-to-target frame only when reserve defense is in
// effect; uniform-cadence mode disables it so an under-delivering source does not get its cadence
// perturbed by per-tick older-frame selection.
inline bool ShouldPreferEarlierFreshWgcFrameForReserveDefense(int64_t earlierFrameTimestampQpc,
                                                              int64_t selectedFrameTimestampQpc,
                                                              int64_t selectionTargetQpc, int64_t targetIntervalTicks,
                                                              bool reservePressureActive, bool lowSourceMode,
                                                              bool deepUnderfeed, bool liveRecoveryMode,
                                                              bool uniformCadenceMode) {
    if (uniformCadenceMode) {
        return false;
    }
    return ShouldPreferEarlierFreshWgcFrameToPreserveReserve(
        earlierFrameTimestampQpc, selectedFrameTimestampQpc, selectionTargetQpc, targetIntervalTicks,
        reservePressureActive, lowSourceMode, deepUnderfeed, liveRecoveryMode);
}

}  // namespace ce::capture_policy
