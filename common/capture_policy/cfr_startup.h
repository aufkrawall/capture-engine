#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>

#include "cfr_scheduling.h"

// Encoder capacity, inject CFR publication, and WGC startup reserve/timeline contracts.

namespace ce::capture_policy {

inline double GetEncoderSustainableOutputFps(double encodeMs) {
    if (encodeMs <= 0.0) {
        return 0.0;
    }

    return 1000.0 / encodeMs;
}

inline double GetInjectCfrServiceMsPerOutputTick(double cycleMs, uint32_t outputTicks) {
    if (cycleMs <= 0.0 || outputTicks == 0) {
        return 0.0;
    }

    return cycleMs / static_cast<double>(outputTicks);
}

inline int64_t GetNextCfrOutputQpc(int64_t liveStartQpc, uint64_t liveTicksOutput, int64_t targetIntervalTicks,
                                  int64_t fallbackQpc) {
    if (liveStartQpc <= 0 || targetIntervalTicks <= 0 ||
        liveTicksOutput > static_cast<uint64_t>((INT64_MAX - liveStartQpc) / targetIntervalTicks)) {
        return fallbackQpc;
    }

    return liveStartQpc + static_cast<int64_t>(liveTicksOutput) * targetIntervalTicks;
}

inline int64_t GetNextInjectCfrOutputQpc(int64_t liveStartQpc, uint64_t liveTicksOutput,
                                         int64_t targetIntervalTicks, int64_t fallbackQpc) {
    return GetNextCfrOutputQpc(liveStartQpc, liveTicksOutput, targetIntervalTicks, fallbackQpc);
}

inline bool ShouldAdvanceWakeDeadlineForCfrCatchupTick(bool useScreenGrab, bool injectRecoveryActive) {
    // An overdue WGC/DXGI held-frame slot and an inject-recovery slot must not
    // postpone the next normal wake. Otherwise two outputs followed by a
    // two-tick wait can never repay wall-clock debt.
    return !useScreenGrab && !injectRecoveryActive;
}

inline bool GetInjectCfrRecoveryActive(bool wasActive, bool recordingOutputLive, bool useVFR,
                                       uint32_t outputShortfallTicks) {
    if (!recordingOutputLive || useVFR) {
        return false;
    }

    if (wasActive) {
        // Do not bounce between 17 and 18 ticks after a one-tick catch-up.
        // Keep the episode armed so later healthy service windows can finish it.
        return outputShortfallTicks > kInjectCfrRecoveryExitShortfallTicks;
    }

    return outputShortfallTicks >= kCfrShortfallForceCatchupThresholdTicks;
}

inline uint32_t GetInjectCfrCatchupTicksThisLoop(uint32_t outputShortfallTicks, bool recoveryActive,
                                                 bool encoderBottlenecked = false) {
    if (!recoveryActive || outputShortfallTicks <= kInjectCfrRecoveryExitShortfallTicks || encoderBottlenecked) {
        return 1u;
    }

    return 2u;
}

inline bool ShouldUseFreshInjectCatchup(bool useVFR, bool encoderBottlenecked, bool encoderActivelyTooSlow,
                                        size_t bufferedInjectFrames, size_t minBufferedInjectFrames,
                                        uint32_t outputShortfallTicks, bool recoveryActive) {
    if (useVFR || encoderBottlenecked || encoderActivelyTooSlow) {
        return false;
    }

    if (!recoveryActive || outputShortfallTicks <= kInjectCfrRecoveryExitShortfallTicks) {
        return false;
    }

    if (bufferedInjectFrames <= minBufferedInjectFrames) {
        return false;
    }

    return true;
}

inline bool IsInjectFrameFreshAfterLastEmission(int64_t frameTimestampQpc, int64_t lastEmittedSourceQpc) {
    return frameTimestampQpc > 0 && (lastEmittedSourceQpc <= 0 || frameTimestampQpc > lastEmittedSourceQpc);
}

inline bool ShouldAllowBgra8WgcFallback(bool explicitTenBitVideo, bool hdrCapture) {
    return !explicitTenBitVideo && !hdrCapture;
}

inline bool ShouldUseWgcCfrStartupSyncBarrier(bool useScreenGrab, bool useVfr, int64_t targetIntervalTicks) {
    return useScreenGrab && !useVfr && targetIntervalTicks > 0;
}

inline int64_t GetWgcCfrStartupPreLiveDelayTicks(int64_t targetIntervalTicks) {
    return targetIntervalTicks > 0 ? (targetIntervalTicks * 24) : 0;
}

inline uint32_t GetWgcFrameCountForDurationMs(uint32_t fps, uint32_t durationMs) {
    if (fps == 0 || durationMs == 0) {
        return 0;
    }
    const uint64_t frames = (static_cast<uint64_t>(fps) * static_cast<uint64_t>(durationMs) + 999ull) / 1000ull;
    return frames > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(frames);
}

inline bool ShouldAttemptWgcStartupSmoothnessBuffer(bool enabled, bool useVfr, bool avContentDelayActive,
                                                    int64_t targetIntervalTicks, uint32_t retainedExtraFrames) {
    return enabled && !useVfr && avContentDelayActive && targetIntervalTicks > 0 && retainedExtraFrames > 0;
}

inline int64_t GetWgcStartupSmoothnessTargetDelayQpc(uint32_t retainedExtraFrames, int64_t targetIntervalTicks,
                                                     uint32_t outputFps = 0, uint32_t maxSmoothnessMs = 0) {
    if (retainedExtraFrames == 0 || targetIntervalTicks <= 0) {
        return 0;
    }
    uint32_t targetFrames = retainedExtraFrames;
    const uint32_t configuredDelayFrames = GetWgcFrameCountForDurationMs(outputFps, maxSmoothnessMs);
    if (configuredDelayFrames > 0) {
        targetFrames = std::min(targetFrames, configuredDelayFrames);
    }
    const uint64_t targetDelay = static_cast<uint64_t>(targetFrames) * static_cast<uint64_t>(targetIntervalTicks);
    return targetDelay > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(targetDelay);
}

inline int64_t GetWgcStartupReserveWaitBudgetQpc(int64_t startupContentDelayTargetQpc, int64_t targetIntervalTicks,
                                                 int64_t smoothnessTargetDelayQpc, bool smoothnessStartupAttempted,
                                                 int64_t qpcTicksPerSecond = 0) {
    if (startupContentDelayTargetQpc <= 0 || targetIntervalTicks <= 0) {
        return 0;
    }

    const auto saturatingAdd = [](int64_t lhs, int64_t rhs) -> int64_t {
        return lhs > INT64_MAX - rhs ? INT64_MAX : lhs + rhs;
    };
    const auto saturatingMul = [](int64_t lhs, int64_t rhs) -> int64_t {
        return rhs > 0 && lhs > INT64_MAX / rhs ? INT64_MAX : lhs * rhs;
    };

    const int64_t baseBudget = saturatingAdd(startupContentDelayTargetQpc, saturatingMul(targetIntervalTicks, 2));
    if (!smoothnessStartupAttempted || smoothnessTargetDelayQpc <= 0) {
        return baseBudget;
    }

    const int64_t smoothnessSlack = saturatingAdd(smoothnessTargetDelayQpc, saturatingMul(targetIntervalTicks, 4));
    const int64_t smoothnessBudget = saturatingAdd(startupContentDelayTargetQpc, smoothnessSlack);
    const int64_t computedBudget = std::max(baseBudget, smoothnessBudget);
    return qpcTicksPerSecond > 0 ? qpcTicksPerSecond : computedBudget;
}

inline int64_t SelectWgcStartupSmoothnessExtraDelayQpc(int64_t actualStartupDelayQpc, int64_t avContentDelayQpc,
                                                       int64_t smoothnessTargetDelayQpc) {
    if (actualStartupDelayQpc <= avContentDelayQpc || smoothnessTargetDelayQpc <= 0) {
        return 0;
    }

    return std::clamp<int64_t>(actualStartupDelayQpc - avContentDelayQpc, 0, smoothnessTargetDelayQpc);
}

// Resolve the FINAL locked WGC smoothness extra read-delay from the startup-barrier candidate.
//
// Root-cause guard for startup-timing-dependent WGC judder: when the startup reserve fill is
// UNDERFED (the buildable smoothness reservoir target is never reached because the source delivers
// at/below the CFR target -- the common case for WGC-capturing a VRR game whose frame delivery is
// bursty around the capture rate), the accidental buffer pile-up at the barrier must NOT become the
// permanent active read delay. The pile-up depth is non-deterministic (how bursty delivery happened
// to be during the sub-second startup wait), and a DEEPER accidental lock permanently starves the
// fresh-frame headroom that the repeat-rescue needs: the read target sits older than the source can
// sustainably keep buffered, so every micro-lull becomes a "too-new" hold and the holds cluster into
// multi-frame freezes for the WHOLE session. Two runs under identical conditions then randomly land
// smooth (shallow lock) or juddery (deep lock).
//
// In the underfed case, pin the extra delay to the measured jitter FLOOR (sized from real observed
// delivery/source jitter, not an accidental snapshot) whenever that is SHALLOWER than the pile-up.
// This is monotonically safe for smoothness: a shallower read delay only ADDS fresh-frame headroom,
// which is exactly what lets the rescue advance instead of clustering. It never INCREASES the delay
// (so it cannot introduce latency the pile-up did not already have), and it is sync-neutral because
// the extra delay is absorbed by the live-start schedule offset while audio stays anchored to the
// true content latency (avContentDelay), so the file's A/V offset is unchanged.
//
// The cap is gated on the source delivering AT/ABOVE the CFR target: that is the regime where a
// shallow read delay is unambiguously correct (the source has enough unique frames; the failure is
// purely lack of fresh-frame headroom for the rescue). When the source runs BELOW the CFR target
// (e.g. a GPU-bound VRR source whose WGC delivery drops into sub-target lulls), the deep reservoir is
// genuinely needed to absorb the lulls, so the pile-up is preserved and the cap does not apply.
//
// When the reservoir target WAS reached (not underfed), the source is below CFR, or no jitter floor is
// available, the validated pile-up behavior is returned unchanged.
inline int64_t ResolveWgcStartupSmoothnessActiveDelayQpc(int64_t pileupExtraDelayQpc, int64_t jitterFloorDelayQpc,
                                                         bool startupUnderfed, bool sourceAtOrAboveCfrTarget) {
    if (!startupUnderfed || !sourceAtOrAboveCfrTarget || jitterFloorDelayQpc <= 0) {
        return pileupExtraDelayQpc;
    }
    // Cap DOWN to the measured jitter floor; never increase a shallow pile-up.
    return std::min(pileupExtraDelayQpc, jitterFloorDelayQpc);
}

inline bool IsWgcStartupCandidateCadenceAtOrAboveCfrTarget(size_t candidateCount, int64_t candidateSpanQpc,
                                                           int64_t targetIntervalQpc) {
    // Startup min-window telemetry includes the pre-live settling gap and can therefore classify an
    // otherwise healthy VRR source differently from run to run. Use the candidates collected inside
    // the bounded reserve wait as a second, local cadence proof. Three frames avoid treating a single
    // coincidental pair as sustained cadence.
    if (candidateCount < 3 || candidateSpanQpc <= 0 || targetIntervalQpc <= 0) {
        return false;
    }

    const int64_t intervalCount = static_cast<int64_t>(candidateCount - 1);
    const int64_t averageIntervalQpc = candidateSpanQpc / intervalCount;
    const int64_t remainder = candidateSpanQpc % intervalCount;
    const int64_t roundedUpAverageIntervalQpc = averageIntervalQpc + (remainder != 0 ? 1 : 0);
    return roundedUpAverageIntervalQpc <= targetIntervalQpc;
}

inline int64_t GetWgcStartupBarrierQpc(int64_t nowQpc, int64_t targetIntervalTicks) {
    if (nowQpc <= 0 || targetIntervalTicks <= 0) {
        return nowQpc;
    }

    return nowQpc + targetIntervalTicks;
}

inline int64_t GetWgcStartupAudioAnchorQpc(int64_t videoFrameQpc, int64_t contentDelayQpc) {
    if (videoFrameQpc <= 0 || contentDelayQpc <= 0) {
        return videoFrameQpc;
    }
    if (videoFrameQpc > INT64_MAX - contentDelayQpc) {
        return videoFrameQpc;
    }
    return videoFrameQpc + contentDelayQpc;
}

struct CfrTimelineStartContract {
    int64_t videoOriginQpc = 0;
    int64_t liveQpc = 0;
    int64_t renderLoopbackLatencyQpc = 0;
    int64_t contentDelayQpc = 0;
    int64_t smoothnessReserveQpc = 0;
    int64_t audioAnchorQpc = 0;
    bool valid = false;
};

inline CfrTimelineStartContract BuildCfrTimelineStartContract(int64_t videoOriginQpc, int64_t liveQpc,
                                                              int64_t renderLoopbackLatencyQpc) {
    CfrTimelineStartContract contract;
    contract.videoOriginQpc = videoOriginQpc;
    contract.liveQpc = liveQpc;
    contract.renderLoopbackLatencyQpc = renderLoopbackLatencyQpc;
    if (videoOriginQpc <= 0 || liveQpc < videoOriginQpc || renderLoopbackLatencyQpc < 0 ||
        videoOriginQpc > INT64_MAX - renderLoopbackLatencyQpc) {
        return contract;
    }

    contract.contentDelayQpc = liveQpc - videoOriginQpc;
    if (contract.contentDelayQpc < renderLoopbackLatencyQpc) {
        return contract;
    }
    contract.smoothnessReserveQpc = contract.contentDelayQpc - renderLoopbackLatencyQpc;
    contract.audioAnchorQpc = videoOriginQpc + renderLoopbackLatencyQpc;
    contract.valid = true;
    return contract;
}

inline CfrTimelineStartContract RebaseCfrTimelineStartContract(const CfrTimelineStartContract& contract,
                                                               int64_t videoOriginQpc) {
    if (!contract.valid || videoOriginQpc <= 0 || contract.contentDelayQpc < 0 ||
        videoOriginQpc > INT64_MAX - contract.contentDelayQpc) {
        return {};
    }
    return BuildCfrTimelineStartContract(videoOriginQpc, videoOriginQpc + contract.contentDelayQpc,
                                         contract.renderLoopbackLatencyQpc);
}

inline size_t SelectNearestMonotonicTimestampIndex(const int64_t* timestamps, size_t count, int64_t targetQpc) {
    if (!timestamps || count == 0) {
        return 0;
    }
    size_t bestIndex = 0;
    uint64_t bestDistance = UINT64_MAX;
    bool found = false;
    const auto signedDistance = [](int64_t left, int64_t right) {
        // Biasing the signed domain into unsigned order makes the distance exact even
        // across INT64_MIN/INT64_MAX, where ordinary signed subtraction would overflow.
        const uint64_t biasedLeft = static_cast<uint64_t>(left) ^ (uint64_t{1} << 63);
        const uint64_t biasedRight = static_cast<uint64_t>(right) ^ (uint64_t{1} << 63);
        return biasedLeft >= biasedRight ? biasedLeft - biasedRight : biasedRight - biasedLeft;
    };
    for (size_t i = 0; i < count; ++i) {
        if (timestamps[i] <= 0) {
            continue;
        }
        const uint64_t distance = signedDistance(timestamps[i], targetQpc);
        if (!found || distance < bestDistance || (distance == bestDistance && timestamps[i] > timestamps[bestIndex])) {
            bestIndex = i;
            bestDistance = distance;
            found = true;
        }
    }
    return bestIndex;
}

struct WgcStartupReserveSelection {
    size_t selectedIndex = 0;
    bool usedDelayReserve = false;
    int64_t targetSelectionQpc = 0;
    int64_t reserveSpanQpc = 0;
    int64_t selectedDelayQpc = 0;
};

inline WgcStartupReserveSelection SelectWgcStartupReserveCandidate(const int64_t* selectionQpcs, size_t count,
                                                                   int64_t contentDelayQpc,
                                                                   int64_t reserveToleranceQpc) {
    WgcStartupReserveSelection result{};
    if (!selectionQpcs || count == 0) {
        return result;
    }

    size_t earliestIndex = 0;
    size_t latestIndex = 0;
    for (size_t i = 1; i < count; ++i) {
        if (selectionQpcs[i] < selectionQpcs[earliestIndex]) {
            earliestIndex = i;
        }
        if (selectionQpcs[i] > selectionQpcs[latestIndex] ||
            (selectionQpcs[i] == selectionQpcs[latestIndex] && i > latestIndex)) {
            latestIndex = i;
        }
    }

    result.selectedIndex = latestIndex;
    const int64_t latestSelectionQpc = selectionQpcs[latestIndex];
    const int64_t earliestSelectionQpc = selectionQpcs[earliestIndex];
    result.reserveSpanQpc = latestSelectionQpc > earliestSelectionQpc ? (latestSelectionQpc - earliestSelectionQpc) : 0;
    if (contentDelayQpc <= 0 || count < 2 || latestSelectionQpc <= 0) {
        return result;
    }

    const int64_t toleranceQpc = std::max<int64_t>(0, reserveToleranceQpc);
    const int64_t requiredSpanQpc = contentDelayQpc > toleranceQpc ? (contentDelayQpc - toleranceQpc) : 0;
    if (result.reserveSpanQpc < requiredSpanQpc) {
        return result;
    }

    const int64_t targetSelectionQpc = latestSelectionQpc - contentDelayQpc;
    if (targetSelectionQpc <= 0) {
        return result;
    }
    result.targetSelectionQpc = targetSelectionQpc;

    size_t bestIndex = latestIndex;
    int64_t bestDistanceQpc = selectionQpcs[latestIndex] > targetSelectionQpc
                                  ? (selectionQpcs[latestIndex] - targetSelectionQpc)
                                  : (targetSelectionQpc - selectionQpcs[latestIndex]);
    for (size_t i = 0; i < count; ++i) {
        if (selectionQpcs[i] <= 0) {
            continue;
        }
        const int64_t distanceQpc = selectionQpcs[i] > targetSelectionQpc ? (selectionQpcs[i] - targetSelectionQpc)
                                                                          : (targetSelectionQpc - selectionQpcs[i]);
        if (distanceQpc < bestDistanceQpc ||
            (distanceQpc == bestDistanceQpc && selectionQpcs[i] > selectionQpcs[bestIndex])) {
            bestDistanceQpc = distanceQpc;
            bestIndex = i;
        }
    }

    const int64_t selectedDelayQpc =
        latestSelectionQpc > selectionQpcs[bestIndex] ? (latestSelectionQpc - selectionQpcs[bestIndex]) : 0;
    if (bestIndex != latestIndex && selectedDelayQpc + toleranceQpc >= contentDelayQpc) {
        result.selectedIndex = bestIndex;
        result.selectedDelayQpc = selectedDelayQpc;
        result.usedDelayReserve = true;
    }
    return result;
}

inline bool IsWgcFramePastStartupBarrier(int64_t frameQpc, int64_t startupBarrierQpc) {
    return startupBarrierQpc <= 0 || (frameQpc > 0 && frameQpc >= startupBarrierQpc);
}

inline uint32_t GetWgcCfrProducerTargetFps(uint32_t /*outputFps*/) {
    // WGC MinUpdateInterval is a minimum gap between accepted compositor
    // updates, not a resampler. With periodic source rate S and finite limit P,
    // the idealized delivery rate is S / ceil(S / P). A 138 fps source limited
    // to 120 therefore becomes about 69 fps; a 160 fps source limited to 150
    // becomes about 80 fps. Since the source cadence is variable and is only
    // observable after this gate, no finite feedback target is universally
    // safe. CFR records at max producer rate and the timestamp-nearest scheduler
    // discards surplus history itself.
    return 0;
}

inline uint32_t EstimateWgcMinUpdateIntervalDeliveryFps(uint32_t sourceFps, uint32_t producerTargetFps) {
    if (sourceFps == 0 || producerTargetFps == 0 || producerTargetFps >= sourceFps) {
        return sourceFps;
    }
    const uint32_t acceptedEvery =
        static_cast<uint32_t>((static_cast<uint64_t>(sourceFps) + static_cast<uint64_t>(producerTargetFps) - 1ull) /
                              static_cast<uint64_t>(producerTargetFps));
    return sourceFps / acceptedEvery;
}

}  // namespace ce::capture_policy
