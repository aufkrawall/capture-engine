#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>

#include "cfr_repeat_metrics.h"

// Backend-neutral nearest-target CFR playout and cadence phase locking.

namespace ce::capture_policy {

// ---- Backend-neutral nearest-target CFR playout (fixed-latency jitter-buffer resampling) ----------
//
// The Bresenham source-rate pacer (DecideWgcActiveDelayPace) emits the OLDEST buffered frame and
// bounds the buffer by COUNT (the depth cap). Under bursty WGC delivery -- DWM hands frames to the
// capture frame pool in late batches even when the game itself presents perfectly smoothly (real
// signature: game present max-interval ~10 ms while WGC delivery dips to 24-110 fps with 170-200 ms
// callback gaps) -- that count/oldest model produces two visible defects:
//   1. The count-based cap dumps a whole delivered burst in a single output tick (a cluster of drops)
//      and then the buffer runs dry across the following delivery lull (a cluster of repeats), so a
//      ~115 fps source manufactures ~20 dups AND ~14 drops in the SAME second. That simultaneous
//      drop+dup churn is far harsher than uniform CFR judder.
//   2. Oldest-first emission rubber-bands the realized content delay: the oldest frame's age is ~0 ms
//      right after a burst and hundreds of ms at the end of a lull (observed realizedDelay 0..243 ms
//      against a 30 ms target), which is also a latent A/V-sync defect.
//
// Nearest-target playout treats the buffer as a fixed-latency jitter buffer. Each output tick selects
// the buffered frame nearest the playout target (gridTickQpc - contentDelayQpc): it advances the
// front over frames the audio timeline has already passed (older than the target, when a strictly
// newer not-too-new successor exists), then emits the front if its slot has aged in, otherwise holds.
// Frames newer than the target stay buffered as future reserve. Because emission position is keyed to
// the timestamp grid (not a count), this consumes unique frames at the SOURCE rate by construction --
// repeating evenly when the source is below output, decimating evenly when above -- so it neither
// over-drains like the old grid-rate timestamp-target reservoir (which force-advanced one unique
// frame per OUTPUT tick and clustered "too new" holds) nor clusters, and the realized delay is pinned
// near contentDelayQpc regardless of delivery burstiness. After a true delivery gap it resumes at the
// correct delay by DROPPING the audio-passed backlog (replaying it would put video behind audio)
// rather than replaying it as a rubber-band; the unavoidable in-gap freeze stays a clean freeze.
//
// `leadToleranceQpc` is how far past (newer than) the target a frame may be and still count as the
// slot frame. Backends may use different bounds: inject uses half an output interval for true
// nearest-neighbour resampling, while WGC/DXGI keep their wider compositor-jitter tolerance.

inline int64_t GetCfrNearestPlayoutLeadToleranceQpc(int64_t targetIntervalTicks, uint32_t tolerancePermille) {
    if (targetIntervalTicks <= 0 || tolerancePermille == 0) {
        return 0;
    }
    return (targetIntervalTicks * static_cast<int64_t>(tolerancePermille)) / 1000;
}

inline int64_t GetInjectCfrSelectionLeadToleranceQpc(int64_t targetIntervalTicks) {
    return GetCfrNearestPlayoutLeadToleranceQpc(targetIntervalTicks, kInjectCfrSelectionLeadTolerancePermille);
}

inline uint64_t GetCfrTimestampDistanceQpc(int64_t lhs, int64_t rhs) {
    return lhs >= rhs ? static_cast<uint64_t>(lhs - rhs) : static_cast<uint64_t>(rhs - lhs);
}

inline int64_t GetCfrCaptureSyncSourceIntervalQpc(int64_t outputIntervalQpc, uint32_t captureSyncMultiplier) {
    if (outputIntervalQpc <= 0 || captureSyncMultiplier == 0) {
        return 0;
    }
    return std::max<int64_t>(1, outputIntervalQpc / static_cast<int64_t>(captureSyncMultiplier));
}

inline int64_t NormalizeCfrCadencePhaseQpc(int64_t phaseQpc, int64_t sourceIntervalQpc) {
    if (sourceIntervalQpc <= 0) {
        return 0;
    }
    phaseQpc %= sourceIntervalQpc;
    const int64_t halfInterval = sourceIntervalQpc / 2;
    if (phaseQpc > halfInterval) {
        phaseQpc -= sourceIntervalQpc;
    } else if (phaseQpc < -halfInterval) {
        phaseQpc += sourceIntervalQpc;
    }
    return phaseQpc;
}

struct CfrCadencePhaseLockState {
    int64_t lastSourceTimestampQpc = 0;
    int64_t lastPhaseReferenceQpc = 0;
    int64_t candidatePhaseQpc = 0;
    int64_t lockedPhaseQpc = 0;
    uint32_t stableSourceIntervals = 0;
    uint32_t unstableSourceIntervals = 0;
    uint32_t phaseConfirmations = 0;
    uint32_t phaseMismatchConfirmations = 0;
    uint32_t phaseIncoherentConfirmations = 0;
    bool locked = false;
    uint64_t acquisitions = 0;
    uint64_t releases = 0;
    uint64_t rephases = 0;

    void Reset() {
        *this = {};
    }
};

inline void ObserveCfrCaptureSyncSourceTimestamp(CfrCadencePhaseLockState& state, int64_t sourceTimestampQpc,
                                                 int64_t sourceIntervalQpc) {
    if (sourceTimestampQpc <= 0 || sourceIntervalQpc <= 0) {
        return;
    }
    if (state.lastSourceTimestampQpc <= 0) {
        state.lastSourceTimestampQpc = sourceTimestampQpc;
        return;
    }
    if (sourceTimestampQpc == state.lastSourceTimestampQpc) {
        return;
    }

    bool cadenceStable = false;
    if (sourceTimestampQpc > state.lastSourceTimestampQpc) {
        const uint64_t delta = static_cast<uint64_t>(sourceTimestampQpc - state.lastSourceTimestampQpc);
        const uint64_t interval = static_cast<uint64_t>(sourceIntervalQpc);
        const uint64_t nearestSteps = std::max<uint64_t>(1, (delta + interval / 2) / interval);
        if (nearestSteps <= 64 && nearestSteps <= static_cast<uint64_t>(INT64_MAX / sourceIntervalQpc)) {
            const uint64_t expected = nearestSteps * interval;
            const uint64_t error = delta >= expected ? delta - expected : expected - delta;
            const uint64_t tolerance =
                (interval * static_cast<uint64_t>(kCfrPhaseLockCadenceTolerancePermille)) / 1000ull;
            cadenceStable = error <= tolerance;
        }
    }
    state.lastSourceTimestampQpc = sourceTimestampQpc;

    if (cadenceStable) {
        state.stableSourceIntervals = std::min<uint32_t>(state.stableSourceIntervals + 1, 1000000u);
        state.unstableSourceIntervals = 0;
        return;
    }

    state.unstableSourceIntervals = std::min<uint32_t>(state.unstableSourceIntervals + 1, 1000000u);
    state.stableSourceIntervals = state.stableSourceIntervals > 2 ? state.stableSourceIntervals - 2 : 0;
    if (state.unstableSourceIntervals >= kCfrPhaseLockReleaseIntervals) {
        if (state.locked) {
            ++state.releases;
        }
        state.locked = false;
        state.phaseConfirmations = 0;
        state.phaseMismatchConfirmations = 0;
        state.phaseIncoherentConfirmations = 0;
        state.lastPhaseReferenceQpc = 0;
    }
}

inline int64_t ApplyCfrCaptureSyncPhaseLock(CfrCadencePhaseLockState& state, int64_t baseTargetQpc,
                                            int64_t sourceReferenceQpc, int64_t sourceIntervalQpc, bool enabled) {
    if (!enabled || baseTargetQpc <= 0 || sourceIntervalQpc <= 0) {
        if (state.locked) {
            ++state.releases;
        }
        state.locked = false;
        state.phaseConfirmations = 0;
        state.phaseMismatchConfirmations = 0;
        state.phaseIncoherentConfirmations = 0;
        return baseTargetQpc;
    }

    if (sourceReferenceQpc > 0 && sourceReferenceQpc != state.lastPhaseReferenceQpc &&
        state.stableSourceIntervals >= kCfrPhaseLockMinStableIntervals) {
        state.lastPhaseReferenceQpc = sourceReferenceQpc;
        const int64_t observedPhase =
            NormalizeCfrCadencePhaseQpc(sourceReferenceQpc - baseTargetQpc, sourceIntervalQpc);
        const int64_t phaseTolerance = std::max<int64_t>(
            1, (sourceIntervalQpc * static_cast<int64_t>(kCfrPhaseLockPhaseTolerancePermille)) / 1000);
        const int64_t rephaseTolerance = std::max<int64_t>(
            1, (sourceIntervalQpc * static_cast<int64_t>(kCfrPhaseLockRephaseTolerancePermille)) / 1000);

        if (!state.locked) {
            if (state.phaseConfirmations == 0) {
                state.candidatePhaseQpc = observedPhase;
                state.phaseConfirmations = 1;
            } else {
                const int64_t error = NormalizeCfrCadencePhaseQpc(
                    observedPhase - state.candidatePhaseQpc, sourceIntervalQpc);
                if (GetCfrTimestampDistanceQpc(error, 0) <= static_cast<uint64_t>(phaseTolerance)) {
                    state.candidatePhaseQpc = NormalizeCfrCadencePhaseQpc(
                        state.candidatePhaseQpc + error / 4, sourceIntervalQpc);
                    ++state.phaseConfirmations;
                } else {
                    state.candidatePhaseQpc = observedPhase;
                    state.phaseConfirmations = 1;
                }
            }
            if (state.phaseConfirmations >= kCfrPhaseLockConfirmations) {
                state.lockedPhaseQpc = state.candidatePhaseQpc;
                state.locked = true;
                state.phaseMismatchConfirmations = 0;
                state.phaseIncoherentConfirmations = 0;
                ++state.acquisitions;
            }
        } else {
            const int64_t error =
                NormalizeCfrCadencePhaseQpc(observedPhase - state.lockedPhaseQpc, sourceIntervalQpc);
            if (GetCfrTimestampDistanceQpc(error, 0) <= static_cast<uint64_t>(phaseTolerance)) {
                state.lockedPhaseQpc =
                    NormalizeCfrCadencePhaseQpc(state.lockedPhaseQpc + error / 16, sourceIntervalQpc);
                state.phaseMismatchConfirmations = 0;
                state.phaseIncoherentConfirmations = 0;
            } else if (state.phaseMismatchConfirmations == 0) {
                state.candidatePhaseQpc = observedPhase;
                state.phaseMismatchConfirmations = 1;
            } else {
                const int64_t candidateError = NormalizeCfrCadencePhaseQpc(
                    observedPhase - state.candidatePhaseQpc, sourceIntervalQpc);
                if (GetCfrTimestampDistanceQpc(candidateError, 0) <= static_cast<uint64_t>(rephaseTolerance)) {
                    state.candidatePhaseQpc = NormalizeCfrCadencePhaseQpc(
                        state.candidatePhaseQpc + candidateError / 4, sourceIntervalQpc);
                    state.phaseIncoherentConfirmations = 0;
                    if (++state.phaseMismatchConfirmations >= kCfrPhaseLockRephaseConfirmations) {
                        state.lockedPhaseQpc = state.candidatePhaseQpc;
                        state.phaseMismatchConfirmations = 0;
                        ++state.rephases;
                    }
                } else {
                    // A real limiter phase transition converges on one new phase. Wandering source
                    // phases are varying cadence, so release instead of repeatedly moving the CFR
                    // selection boundary and making variable-FPS resampling less predictable.
                    state.candidatePhaseQpc = observedPhase;
                    state.phaseMismatchConfirmations = 1;
                    if (++state.phaseIncoherentConfirmations >= kCfrPhaseLockIncoherentReleaseIntervals) {
                        state.locked = false;
                        state.phaseConfirmations = 0;
                        state.phaseMismatchConfirmations = 0;
                        state.phaseIncoherentConfirmations = 0;
                        state.lastPhaseReferenceQpc = 0;
                        ++state.releases;
                    }
                }
            }
        }
    }

    if (!state.locked || state.lockedPhaseQpc == 0) {
        return baseTargetQpc;
    }
    if ((state.lockedPhaseQpc > 0 && baseTargetQpc > INT64_MAX - state.lockedPhaseQpc) ||
        (state.lockedPhaseQpc < 0 && baseTargetQpc < INT64_MIN - state.lockedPhaseQpc)) {
        return baseTargetQpc;
    }
    return baseTargetQpc + state.lockedPhaseQpc;
}

// Should the current front be dropped in favour of `nextTimestampQpc`? True when the successor is
// strictly newer (monotonic safety) and is still at-or-before the playout slot within the lead
// tolerance, i.e. it is a closer representative of the target than the older front, so the front is
// already-past surplus history. Stops naturally at the newest not-too-new frame, leaving any future
// frames as reserve.
inline bool ShouldDropCfrFrontForNearerPlayout(int64_t frontTimestampQpc, int64_t nextTimestampQpc,
                                               int64_t playoutTargetQpc, int64_t leadToleranceQpc) {
    if (frontTimestampQpc <= 0 || nextTimestampQpc <= 0 || playoutTargetQpc <= 0) {
        return false;
    }
    if (nextTimestampQpc <= frontTimestampQpc) {
        return false;  // not strictly newer -> never advance past (duplicate/non-monotonic safety)
    }
    if (nextTimestampQpc > playoutTargetQpc + leadToleranceQpc) {
        return false;
    }
    // Choose the actual nearest source sample. Ties go to the newer frame so a surplus source is
    // deterministically decimated without retaining already-superseded history.
    return GetCfrTimestampDistanceQpc(nextTimestampQpc, playoutTargetQpc) <=
           GetCfrTimestampDistanceQpc(frontTimestampQpc, playoutTargetQpc);
}

inline bool ShouldDropWgcFrontForNearerPlayout(int64_t frontTimestampQpc, int64_t nextTimestampQpc,
                                               int64_t playoutTargetQpc, int64_t leadToleranceQpc) {
    return ShouldDropCfrFrontForNearerPlayout(frontTimestampQpc, nextTimestampQpc, playoutTargetQpc,
                                              leadToleranceQpc);
}

inline bool ShouldSkipDeliveredDuplicateWgcSourceTimestamp(bool duplicateSourceTimestamp, int64_t rawSourceFrameQpc,
                                                           int64_t lastDeliveredRawSourceQpc, bool cfrCaptureActive) {
    if (!cfrCaptureActive || !duplicateSourceTimestamp || rawSourceFrameQpc <= 0 || lastDeliveredRawSourceQpc <= 0) {
        return false;
    }
    return rawSourceFrameQpc == lastDeliveredRawSourceQpc;
}

struct CfrNearestPlayoutDecision {
    bool emit = false;  // pop and emit the (post-stale-drop) front frame for this slot
    bool hold = false;  // repeat the previous frame: the slot frame has not been delivered yet
};

// After stale-dropping, decide what to do with the front frame for this output tick. Emit when it has
// aged into the slot (timestamp <= target + leadTolerance) and is strictly newer than the last
// emitted frame (monotonic). Otherwise hold -- the slot frame is still in the future, so leave the
// newer buffered frames as reserve and repeat the previous frame (an evenly distributed
// source-limited / delivery-gap repeat). A lone frame older than the target is still emitted (it is
// the freshest available content and strictly newer than the last emit), which makes an in-gap freeze
// a clean monotonic hold instead of a backward rubber-band.
inline CfrNearestPlayoutDecision DecideCfrNearestPlayout(int64_t frontTimestampQpc, int64_t playoutTargetQpc,
                                                         int64_t leadToleranceQpc, int64_t lastEmittedTimestampQpc) {
    CfrNearestPlayoutDecision decision;
    if (frontTimestampQpc <= 0 || playoutTargetQpc <= 0) {
        return decision;  // no usable timing -> caller falls back / holds
    }
    if (frontTimestampQpc <= lastEmittedTimestampQpc) {
        decision.hold = true;  // would be non-monotonic -> repeat
        return decision;
    }
    if (frontTimestampQpc <= playoutTargetQpc + leadToleranceQpc) {
        decision.emit = true;
    } else {
        decision.hold = true;  // front still in the future beyond tolerance -> slot not aged in
    }
    return decision;
}

using WgcNearestPlayoutDecision = CfrNearestPlayoutDecision;

inline WgcNearestPlayoutDecision DecideWgcNearestPlayout(int64_t frontTimestampQpc, int64_t playoutTargetQpc,
                                                         int64_t leadToleranceQpc, int64_t lastEmittedTimestampQpc) {
    return DecideCfrNearestPlayout(frontTimestampQpc, playoutTargetQpc, leadToleranceQpc, lastEmittedTimestampQpc);
}

inline bool ShouldAllowSingleFreshWgcHold(bool reservePressureActive, bool lowSourceMode, uint32_t recentInputMin250Fps,
                                          uint32_t outputFps, double smoothedInputPerTick) {
    if (!(reservePressureActive || lowSourceMode) || outputFps == 0) {
        return false;
    }

    if (recentInputMin250Fps < outputFps) {
        return true;
    }

    const double holdInputThreshold = static_cast<double>(kWgcSingleFreshHoldInputPermille) / 1000.0;
    return smoothedInputPerTick < holdInputThreshold;
}

inline bool ShouldAllowSteadyStateWgcReserveBuild(uint32_t recentInputMin250Fps, uint32_t outputFps,
                                                  double smoothedInputPerTick) {
    if (outputFps == 0 || recentInputMin250Fps < outputFps) {
        return false;
    }

    const double reserveBuildThreshold = static_cast<double>(kWgcSteadyReserveBuildInputPermille) / 1000.0;
    return smoothedInputPerTick >= reserveBuildThreshold;
}

inline bool HasRecordingEncoderOrMuxPressure(uint32_t encoderOverloadFlags, uint32_t muxBackpressureCount,
                                             uint64_t encoderLimitedDropCount) {
    return encoderOverloadFlags != 0 || muxBackpressureCount > 0 || encoderLimitedDropCount > 0;
}

inline bool ShouldHoldSingleFreshWgcFrame(bool reservePressureActive, bool lowSourceMode, uint32_t recentInputMin250Fps,
                                          uint32_t outputFps, double smoothedInputPerTick,
                                          uint32_t outputShortfallTicks, bool encoderBottlenecked,
                                          bool reserveAvailableAtTickStart, bool deepUnderfeed) {
    if (outputShortfallTicks > 0 || encoderBottlenecked || reserveAvailableAtTickStart || deepUnderfeed) {
        return false;
    }

    return ShouldAllowSingleFreshWgcHold(reservePressureActive, lowSourceMode, recentInputMin250Fps, outputFps,
                                         smoothedInputPerTick);
}

inline size_t ClampWgcSelectionIndexForLowSource(size_t bestIdx, size_t availableCount, size_t bufferedWgcFrames,
                                                 uint32_t recentDeliveredFps, uint32_t recentInputMin250Fps,
                                                 uint32_t outputFps, uint32_t emptyTickPermille,
                                                 bool liveRecoveryMode = false) {
    if (availableCount <= 1) {
        return 0;
    }

    size_t clampedIdx = std::min(bestIdx, availableCount - 1);
    if (liveRecoveryMode) {
        return clampedIdx;
    }

    if (IsWgcDeepUnderfeed(outputFps, recentDeliveredFps, recentInputMin250Fps, emptyTickPermille)) {
        return clampedIdx;
    }

    const bool severeUnderfeed = recentDeliveredFps + 2u < outputFps;
    const bool fragileQueue = bufferedWgcFrames <= 2 || emptyTickPermille >= 120;
    if (severeUnderfeed && fragileQueue) {
        return 0;
    }

    if (fragileQueue && clampedIdx > 1) {
        clampedIdx = 1;
    }
    return clampedIdx;
}

inline bool ShouldDropFrontWgcFrameForSelection(size_t dropIndex, size_t bufferedWgcFrames, bool lowSourceMode,
                                                uint32_t emptyTickPermille) {
    (void)bufferedWgcFrames;
    (void)lowSourceMode;
    (void)emptyTickPermille;
    return dropIndex > 0;
}

}  // namespace ce::capture_policy
