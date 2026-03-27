#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ce::audio {

constexpr int64_t kDefaultSteadyAudioPullLatencyMs = 60;
constexpr int64_t kDefaultAudioPullQuantumSamples = 240;

struct PacketTimelineAdjustment {
    int64_t gapSamples = 0;
    int64_t overlapSamples = 0;
};

struct WgcAudioLagTargets {
    int64_t driftLagMs = 0;
    int64_t targetBufferLagMs = 0;
};

inline bool ShouldDeferAudioPullUntilQuantum(int64_t pendingSamples, bool trackStartupSettled, bool forceDrain,
                                             int64_t quantumSamples = kDefaultAudioPullQuantumSamples) {
    if (forceDrain || !trackStartupSettled) {
        return false;
    }

    if (pendingSamples <= 0) {
        return true;
    }

    return pendingSamples < std::max<int64_t>(1, quantumSamples);
}

inline int64_t ComputeVideoPipelineLagMs(int64_t wallVideoMs, int64_t encodedVideoMs) {
    if (wallVideoMs <= 0 || encodedVideoMs <= 0 || wallVideoMs <= encodedVideoMs) {
        return 0;
    }
    return wallVideoMs - encodedVideoMs;
}

inline int64_t ComputeWgcBufferedVideoContentLagMs(uint32_t oldestBufferedFrameAgeUs) {
    return oldestBufferedFrameAgeUs == 0 ? 0 : static_cast<int64_t>(oldestBufferedFrameAgeUs / 1000u);
}

inline bool HasWgcUnrecoverableCoverageLoss(uint32_t targetFps, int64_t videoPipelineLagMs,
                                            int64_t bufferedVideoContentLagMs, bool encoderBottlenecked = false,
                                            uint32_t wgcDeliveredFps = 0, int64_t minPipelineLagMs = 250,
                                            int64_t minLagMismatchMs = 120) {
    if (targetFps == 0) {
        return false;
    }

    if (videoPipelineLagMs < minPipelineLagMs) {
        return false;
    }

    // When the encoder is bottlenecked but WGC is delivering frames at or near
    // the target rate, the pipeline lag is caused by encoder slowness rather
    // than actual frame loss.  Suppress coverage loss detection to prevent
    // false-positive audio trimming.
    if (encoderBottlenecked && wgcDeliveredFps > 0 && wgcDeliveredFps + 2 >= targetFps) {
        return false;
    }

    const int64_t clampedBufferedLagMs = std::max<int64_t>(0, bufferedVideoContentLagMs);
    return videoPipelineLagMs > clampedBufferedLagMs + minLagMismatchMs;
}

inline double ComputeWgcCoverageLossRatio(int64_t videoPipelineLagMs, int64_t bufferedVideoContentLagMs,
                                          int64_t fullTrimMismatchMs = 16000) {
    if (fullTrimMismatchMs <= 0) {
        return 0.0;
    }

    const int64_t mismatchMs = std::max<int64_t>(0, videoPipelineLagMs - std::max<int64_t>(0, bufferedVideoContentLagMs));
    if (mismatchMs <= 0) {
        return 0.0;
    }

    const double lossRatio = static_cast<double>(mismatchMs) / static_cast<double>(fullTrimMismatchMs);
    return std::clamp(lossRatio, 0.0, 0.25);
}

inline int64_t ComputeWgcCoverageBufferedAudioLagMs(int64_t videoPipelineLagMs, int64_t bufferedVideoContentLagMs,
                                                    int64_t minLagMismatchMs = 120,
                                                    int64_t maxBufferedLagMs = 300,
                                                    int64_t lagDivisor = 6) {
    if (videoPipelineLagMs <= 0 || maxBufferedLagMs <= 0 || lagDivisor <= 0) {
        return 0;
    }

    const int64_t mismatchMs = std::max<int64_t>(0, videoPipelineLagMs - std::max<int64_t>(0, bufferedVideoContentLagMs));
    const int64_t effectiveMismatchMs = mismatchMs - std::max<int64_t>(0, minLagMismatchMs);
    if (effectiveMismatchMs <= 0) {
        return 0;
    }

    return std::clamp<int64_t>(effectiveMismatchMs / lagDivisor, 0, maxBufferedLagMs);
}

inline WgcAudioLagTargets ComputeWgcAudioLagTargets(int64_t videoPipelineLagMs, int64_t bufferedVideoContentLagMs,
                                                    bool coverageLossActive, int64_t maxBufferedLagMs = 300) {
    WgcAudioLagTargets targets{};
    if (!coverageLossActive) {
        // In steady-state CFR recording the video encoder can temporarily fall behind
        // wall clock while still preserving the exact slot timeline. Audio should not
        // absorb that encoder backlog as permanent extra latency; only real buffered
        // content lag should influence the target buffer in the non-coverage-loss case.
        const int64_t lagMs = std::max<int64_t>(0, bufferedVideoContentLagMs);
        targets.driftLagMs = lagMs;
        targets.targetBufferLagMs = lagMs;
        return targets;
    }

    targets.driftLagMs = std::max<int64_t>(0, bufferedVideoContentLagMs);
    targets.targetBufferLagMs =
        std::max<int64_t>(targets.driftLagMs,
                          ComputeWgcCoverageBufferedAudioLagMs(videoPipelineLagMs, bufferedVideoContentLagMs, 120,
                                                               maxBufferedLagMs));
    return targets;
}

inline int64_t ComputeWgcSteadyStateBufferedAudioLagMs(uint32_t targetFps, uint32_t deliveredFps,
                                                       uint32_t deliveredMin250Fps, uint32_t deliveredMin500Fps,
                                                       bool encoderBottlenecked, uint32_t queueEmptyTickPermille,
                                                       uint32_t bufferedAtTickMin, uint32_t singleFrameTickCount,
                                                       int64_t minLagMs = 20, int64_t degradedLagMs = 40,
                                                       int64_t severeLagMs = 80, int64_t maxLagMs = 80,
                                                       int64_t lagDivisor = 8) {
    if (targetFps == 0 || maxLagMs <= 0 || lagDivisor <= 0) {
        return 0;
    }

    uint32_t effectiveDeliveredFps = deliveredFps;
    if (deliveredMin250Fps > 0) {
        effectiveDeliveredFps = std::min(effectiveDeliveredFps, deliveredMin250Fps);
    }
    if (deliveredMin500Fps > 0) {
        effectiveDeliveredFps = std::min(effectiveDeliveredFps, deliveredMin500Fps);
    }

    const int64_t fpsDeficit =
        std::max<int64_t>(0, static_cast<int64_t>(targetFps) - static_cast<int64_t>(effectiveDeliveredFps));
    const int64_t frameIntervalMs = std::max<int64_t>(1, 1000 / static_cast<int64_t>(targetFps));
    int64_t lagMs = 0;
    if (fpsDeficit > 0) {
        lagMs = (fpsDeficit * frameIntervalMs) / lagDivisor;
    }

    if (encoderBottlenecked) {
        lagMs = std::max<int64_t>(lagMs, minLagMs);
    }

    const bool degradedQueueHealth =
        queueEmptyTickPermille >= 125 || bufferedAtTickMin <= 1 || singleFrameTickCount >= targetFps / 4;
    const bool severeQueueHealth =
        queueEmptyTickPermille >= 250 || bufferedAtTickMin == 0 || singleFrameTickCount >= targetFps / 2;
    if (degradedQueueHealth) {
        lagMs = std::max<int64_t>(lagMs, degradedLagMs);
    }
    if (severeQueueHealth &&
        (encoderBottlenecked || effectiveDeliveredFps + 2 < targetFps)) {
        lagMs = std::max<int64_t>(lagMs, severeLagMs);
    }

    return std::clamp<int64_t>(lagMs, 0, maxLagMs);
}

inline int64_t ComputeWgcCoverageLossTrimSamples(int64_t samplesToEncode, double coverageLossRatio,
                                                 double& trimAccumulator, int64_t maxDropPerCall) {
    if (samplesToEncode <= 0 || coverageLossRatio <= 0.0 || maxDropPerCall <= 0) {
        trimAccumulator = std::max(0.0, trimAccumulator);
        return 0;
    }

    trimAccumulator += static_cast<double>(samplesToEncode) * coverageLossRatio;
    const int64_t desiredDropSamples = static_cast<int64_t>(trimAccumulator);
    if (desiredDropSamples <= 0) {
        return 0;
    }

    const int64_t appliedDropSamples = std::clamp<int64_t>(desiredDropSamples, 0, maxDropPerCall);
    trimAccumulator -= static_cast<double>(appliedDropSamples);
    return appliedDropSamples;
}

inline int64_t ComputeBufferedAudioTargetSamples(int sampleRate, int64_t baseLatencySamples, int64_t videoPipelineLagMs,
                                                 int64_t maxLagContributionMs = 40) {
    if (sampleRate <= 0) {
        return std::max<int64_t>(0, baseLatencySamples);
    }

    int64_t lagSamples = 0;
    if (videoPipelineLagMs > 0) {
        const int64_t boundedLagMs =
            std::clamp<int64_t>(videoPipelineLagMs, 0, std::max<int64_t>(0, maxLagContributionMs));
        lagSamples = (boundedLagMs * sampleRate) / 1000;
    }

    return std::max<int64_t>(0, baseLatencySamples + lagSamples);
}

inline int64_t ComputeAudioPullLatencyMs(int64_t steadyLatencyMs, bool allSourcesPrimed,
                                         int64_t maxObservedLateStartMs) {
    const int64_t baseLatencyMs = std::max<int64_t>(0, steadyLatencyMs);
    if (allSourcesPrimed) {
        return baseLatencyMs;
    }

    const int64_t startupLatencyMs = std::max<int64_t>(baseLatencyMs + 30, maxObservedLateStartMs + 20);
    return std::clamp<int64_t>(startupLatencyMs, baseLatencyMs, 120);
}

inline int64_t ComputeLatencyAdjustedAvDriftMs(int64_t rawAvDriftMs, int64_t intentionalPullLatencyMs) {
    return rawAvDriftMs + std::max<int64_t>(intentionalPullLatencyMs, 0);
}

inline bool ShouldAllowWgcSteadyStateDriftCompensation(bool trackStartupSettled, int64_t videoPipelineLagMs,
                                                        int64_t bufferedSamples, int64_t targetLatencySamples,
                                                        int64_t leadWarningSamples) {
    if (!trackStartupSettled || bufferedSamples <= 0) {
        return false;
    }

    const int64_t boundedTargetSamples = std::max<int64_t>(0, targetLatencySamples);
    const int64_t leadSamples = bufferedSamples - boundedTargetSamples;
    return leadSamples >= std::max<int64_t>(1, leadWarningSamples);
}

inline int64_t ComputeWgcPositiveCompensationHysteresisSamples(int64_t targetLatencySamples,
                                                               int64_t leadWarningSamples,
                                                               int64_t quantumSamples = kDefaultAudioPullQuantumSamples) {
    const int64_t boundedQuantumSamples = std::max<int64_t>(1, quantumSamples);
    const int64_t baseHysteresisSamples =
        std::max<int64_t>(boundedQuantumSamples * 2, std::max<int64_t>(0, targetLatencySamples) / 3);
    const int64_t maxHysteresisSamples = std::max<int64_t>(baseHysteresisSamples, leadWarningSamples / 4);
    return std::clamp<int64_t>(baseHysteresisSamples, boundedQuantumSamples, maxHysteresisSamples);
}

inline bool ShouldClearWgcPositiveDriftCompensation(bool allowSteadyStateDriftCompensation,
                                                    int64_t bufferedSamples, int64_t expectedLeadSamples,
                                                    int64_t hysteresisSamples) {
    if (!allowSteadyStateDriftCompensation) {
        return true;
    }

    return bufferedSamples <= expectedLeadSamples + std::max<int64_t>(0, hysteresisSamples);
}

inline int64_t ClampWgcPositiveDriftCorrection(int64_t targetCorrection, int64_t hysteresisSamples) {
    if (targetCorrection <= 0) {
        return targetCorrection;
    }

    return std::max<int64_t>(0, targetCorrection - std::max<int64_t>(0, hysteresisSamples));
}

inline bool IsTrackAudioStartupSettled(bool trackBootstrapComplete, bool allSourcesPrimed) {
    return trackBootstrapComplete || allSourcesPrimed;
}

inline bool IsOptionalUnstartedAppAudioSource(bool isAppAudioSource, bool hasAlignedStart) {
    return isAppAudioSource && !hasAlignedStart;
}

inline bool IsSourceStartupPrimed(bool sourceIsPrimed, bool hasAlignedStart, bool isAppAudioSource) {
    return sourceIsPrimed || IsOptionalUnstartedAppAudioSource(isAppAudioSource, hasAlignedStart);
}

inline bool IsSourceBootstrapReady(bool sourceBootstrapComplete, bool hasAlignedStart, bool sourceIsPrimed,
                                   bool isAppAudioSource, size_t bufferedTimelineSamples,
                                   size_t requiredTimelineSamples) {
    if (sourceBootstrapComplete || IsOptionalUnstartedAppAudioSource(isAppAudioSource, hasAlignedStart)) {
        return true;
    }

    return hasAlignedStart && sourceIsPrimed && bufferedTimelineSamples >= requiredTimelineSamples;
}

inline size_t ComputeBufferedRealAudioSamples(size_t bufferedSamples, uint64_t syntheticBufferedSamples) {
    if (syntheticBufferedSamples >= bufferedSamples) {
        return 0;
    }
    return bufferedSamples - static_cast<size_t>(syntheticBufferedSamples);
}

inline uint64_t ConsumeSyntheticBufferedSamples(uint64_t& syntheticBufferedSamples, uint64_t consumedSamples) {
    const uint64_t syntheticConsumed = std::min<uint64_t>(syntheticBufferedSamples, consumedSamples);
    syntheticBufferedSamples -= syntheticConsumed;
    return syntheticConsumed;
}

inline PacketTimelineAdjustment ComputePacketTimelineAdjustment(int64_t packetStartSamples, int64_t writtenTimelineSamples,
                                                               int64_t slopSamples = 0) {
    PacketTimelineAdjustment adjustment;
    const int64_t clampedPacketStartSamples = std::max<int64_t>(0, packetStartSamples);
    const int64_t clampedWrittenTimelineSamples = std::max<int64_t>(0, writtenTimelineSamples);
    const int64_t deltaSamples = clampedPacketStartSamples - clampedWrittenTimelineSamples;
    if (std::abs(deltaSamples) <= std::max<int64_t>(0, slopSamples)) {
        return adjustment;
    }

    if (deltaSamples > 0) {
        adjustment.gapSamples = deltaSamples;
    } else {
        adjustment.overlapSamples = -deltaSamples;
    }
    return adjustment;
}

}  // namespace ce::audio
