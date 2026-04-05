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

inline uint32_t GetWgcRecordingCadenceFps(uint32_t outputFps, uint32_t captureTargetFps) {
    return outputFps > 0 ? outputFps : captureTargetFps;
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

    const int64_t mismatchMs =
        std::max<int64_t>(0, videoPipelineLagMs - std::max<int64_t>(0, bufferedVideoContentLagMs));
    if (mismatchMs <= 0) {
        return 0.0;
    }

    const double lossRatio = static_cast<double>(mismatchMs) / static_cast<double>(fullTrimMismatchMs);
    return std::clamp(lossRatio, 0.0, 0.25);
}

inline int64_t ComputeWgcCoverageBufferedAudioLagMs(int64_t videoPipelineLagMs, int64_t bufferedVideoContentLagMs,
                                                    int64_t minLagMismatchMs = 120, int64_t maxBufferedLagMs = 300,
                                                    int64_t lagDivisor = 6) {
    if (videoPipelineLagMs <= 0 || maxBufferedLagMs <= 0 || lagDivisor <= 0) {
        return 0;
    }

    const int64_t mismatchMs =
        std::max<int64_t>(0, videoPipelineLagMs - std::max<int64_t>(0, bufferedVideoContentLagMs));
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
    targets.targetBufferLagMs = std::max<int64_t>(
        targets.driftLagMs,
        ComputeWgcCoverageBufferedAudioLagMs(videoPipelineLagMs, bufferedVideoContentLagMs, 120, maxBufferedLagMs));
    return targets;
}

inline int64_t ComputeWgcCfrDriftLagMs(const WgcAudioLagTargets& lagTargets, bool preferVideoRepeatsOverAudioCuts,
                                       int64_t encoderShortfallBufferedLagMs = 0) {
    const int64_t baseDriftLagMs = std::max<int64_t>(0, lagTargets.driftLagMs);
    if (preferVideoRepeatsOverAudioCuts) {
        return baseDriftLagMs;
    }

    return baseDriftLagMs + std::max<int64_t>(0, encoderShortfallBufferedLagMs);
}

inline int64_t ComputeWgcCfrTargetBufferLagMs(const WgcAudioLagTargets& lagTargets,
                                              int64_t steadyStateBufferedAudioLagMs,
                                              bool preferVideoRepeatsOverAudioCuts,
                                              int64_t encoderShortfallBufferedLagMs = 0) {
    int64_t targetBufferLagMs = std::max<int64_t>(std::max<int64_t>(0, lagTargets.targetBufferLagMs),
                                                  std::max<int64_t>(0, steadyStateBufferedAudioLagMs));
    if (!preferVideoRepeatsOverAudioCuts) {
        targetBufferLagMs = std::max<int64_t>(targetBufferLagMs, std::max<int64_t>(0, encoderShortfallBufferedLagMs));
    }

    return targetBufferLagMs;
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

    // WGC source starvation should bias us toward preserving audio continuity, but
    // not by inflating the steady-state audio backlog target so far that the audio
    // path starts performing audible trim/correction. Keep only a modest cushion in
    // degraded queue health and cap severe starvation to the same conservative band
    // unless the encoder itself is the bottleneck.
    if (degradedQueueHealth) {
        lagMs = std::max<int64_t>(lagMs, degradedLagMs);
    }
    if (severeQueueHealth) {
        lagMs = std::max<int64_t>(lagMs, encoderBottlenecked ? severeLagMs : degradedLagMs);
    }

    return std::clamp<int64_t>(lagMs, 0, maxLagMs);
}

inline bool ShouldProtectWgcAudioContinuityDuringEncoderOverload(bool encoderBottlenecked, bool coverageLossActive,
                                                                 uint32_t targetFps, uint32_t deliveredFps,
                                                                 uint32_t deliveryMarginFps = 4) {
    if (!encoderBottlenecked || coverageLossActive || targetFps == 0 || deliveredFps == 0) {
        return false;
    }

    const uint32_t healthyDeliveryFloor = targetFps > deliveryMarginFps ? (targetFps - deliveryMarginFps) : targetFps;
    return deliveredFps >= healthyDeliveryFloor;
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

inline int64_t ComputeDurationUsToSamples(int64_t durationUs, int sampleRate) {
    if (sampleRate <= 0 || durationUs <= 0) {
        return 0;
    }

    constexpr int64_t kMicrosecondsPerSecond = 1000000;
    return ((durationUs * static_cast<int64_t>(sampleRate)) + (kMicrosecondsPerSecond / 2)) / kMicrosecondsPerSecond;
}

inline int64_t ClampStartupAnchorQpc(int64_t sourceAnchorQpc, int64_t observedNowQpc, int64_t qpcFrequency,
                                     uint32_t nominalFps, int64_t fallbackFrameMs = 16) {
    if (sourceAnchorQpc <= 0 || observedNowQpc <= 0 || qpcFrequency <= 0 || observedNowQpc <= sourceAnchorQpc) {
        return sourceAnchorQpc;
    }

    int64_t frameDurationQpc = 0;
    if (nominalFps > 0) {
        frameDurationQpc = (qpcFrequency + static_cast<int64_t>(nominalFps / 2)) / static_cast<int64_t>(nominalFps);
    }
    if (frameDurationQpc <= 0) {
        frameDurationQpc = ((qpcFrequency * std::max<int64_t>(1, fallbackFrameMs)) + 500) / 1000;
    }

    return std::min<int64_t>(observedNowQpc, sourceAnchorQpc + std::max<int64_t>(1, frameDurationQpc));
}

inline int64_t ComputeLeadTrimExcessSamples(int64_t bufferedSamples, int64_t targetLatencySamples,
                                            int64_t allowedLeadSamples,
                                            int64_t hysteresisSamples = kDefaultAudioPullQuantumSamples) {
    const int64_t thresholdSamples = std::max<int64_t>(0, targetLatencySamples) +
                                     std::max<int64_t>(0, allowedLeadSamples) + std::max<int64_t>(0, hysteresisSamples);
    return std::max<int64_t>(0, bufferedSamples - thresholdSamples);
}

inline double ComputeSamplesPerMinute(uint64_t sampleCount, int64_t durationUs) {
    if (durationUs <= 0) {
        return 0.0;
    }

    const double durationMinutes = static_cast<double>(durationUs) / 60000000.0;
    if (durationMinutes <= 0.0) {
        return 0.0;
    }

    return static_cast<double>(sampleCount) / durationMinutes;
}

inline double ComputeClockMismatchPpm(int32_t compensationDelta, int sampleRate, int compensationWindowSeconds = 10) {
    if (sampleRate <= 0 || compensationWindowSeconds <= 0) {
        return 0.0;
    }

    const double baseSamples = static_cast<double>(sampleRate) * static_cast<double>(compensationWindowSeconds);
    if (baseSamples <= 0.0) {
        return 0.0;
    }

    return static_cast<double>(compensationDelta) * 1000000.0 / baseSamples;
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

inline int64_t ComputeWgcPositiveCompensationHysteresisSamples(
    int64_t targetLatencySamples, int64_t leadWarningSamples,
    int64_t quantumSamples = kDefaultAudioPullQuantumSamples) {
    const int64_t boundedQuantumSamples = std::max<int64_t>(1, quantumSamples);
    const int64_t baseHysteresisSamples =
        std::max<int64_t>(boundedQuantumSamples * 2, std::max<int64_t>(0, targetLatencySamples) / 3);
    const int64_t maxHysteresisSamples = std::max<int64_t>(baseHysteresisSamples, leadWarningSamples / 4);
    return std::clamp<int64_t>(baseHysteresisSamples, boundedQuantumSamples, maxHysteresisSamples);
}

inline bool ShouldClearWgcPositiveDriftCompensation(bool allowSteadyStateDriftCompensation, int64_t bufferedSamples,
                                                    int64_t expectedLeadSamples, int64_t hysteresisSamples) {
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

inline int32_t ComputeTier1CompensationDelta(int64_t trueDriftSamples, int64_t compensationWindowSamples,
                                             double maxPitchPercent = 0.5) {  // Match WGC CFR default
    const int32_t maxDelta = static_cast<int32_t>(compensationWindowSamples * maxPitchPercent / 100.0 + 0.5);
    if (maxDelta <= 0) {
        return 0;
    }
    return static_cast<int32_t>(
        std::clamp(trueDriftSamples, static_cast<int64_t>(-maxDelta), static_cast<int64_t>(maxDelta)));
}

inline bool ShouldActivateTier2Trim(int64_t trueDriftSamples, int sampleRate, int64_t thresholdMs = 20) {
    if (sampleRate <= 0) {
        return false;
    }
    const int64_t thresholdSamples = (static_cast<int64_t>(sampleRate) * thresholdMs) / 1000;
    return trueDriftSamples > thresholdSamples;
}

inline int64_t ComputeTier2TrimBudget(int64_t trueDriftSamples, int sampleRate, int64_t baseQuantumSamples,
                                      int64_t largeDriftThresholdMs = 100, int64_t maxTrimMs = 20) {
    if (sampleRate <= 0)
        return baseQuantumSamples;
    const int64_t absDriftSamples = std::abs(trueDriftSamples);
    const int64_t largeThresholdSamples = (sampleRate * largeDriftThresholdMs) / 1000;
    if (absDriftSamples <= largeThresholdSamples) {
        return baseQuantumSamples;
    }

    // Scale tiered trim budget with drift magnitude for faster recovery:
    //   >100ms drift: trim = |drift| / 10, capped at maxTrimMs (20ms by default)
    //   >2s drift:    trim = |drift| / 5, capped at 50ms (2400 samples at 48kHz)
    //   >10s drift:   trim = |drift| / 2, capped at 100ms (4800 samples at 48kHz)
    // This prevents spending 500+ pull calls to recover from severe encoder stalls.
    const int64_t severeThresholdSamples = static_cast<int64_t>(sampleRate) * 2;    // 2 seconds
    const int64_t extremeThresholdSamples = static_cast<int64_t>(sampleRate) * 10;  // 10 seconds

    int64_t proportionalTrim;
    int64_t budgetCap;

    if (absDriftSamples > extremeThresholdSamples) {
        proportionalTrim = absDriftSamples / 2;
        budgetCap = (sampleRate * 100) / 1000;  // 100ms
    } else if (absDriftSamples > severeThresholdSamples) {
        proportionalTrim = absDriftSamples / 5;
        budgetCap = (sampleRate * 50) / 1000;  // 50ms
    } else {
        proportionalTrim = absDriftSamples / 10;
        budgetCap = (sampleRate * maxTrimMs) / 1000;  // default max
    }

    return std::clamp<int64_t>(proportionalTrim, baseQuantumSamples, budgetCap);
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
                                   bool isAppAudioSource, size_t bufferedRealSamples, size_t requiredRealSamples) {
    if (sourceBootstrapComplete || IsOptionalUnstartedAppAudioSource(isAppAudioSource, hasAlignedStart)) {
        return true;
    }

    return hasAlignedStart && sourceIsPrimed && bufferedRealSamples >= requiredRealSamples;
}

inline size_t ComputeRequiredBootstrapRealSamples(int64_t targetTimelineSamples, size_t minimumRealSamples) {
    return static_cast<size_t>(
        std::max<int64_t>(std::max<int64_t>(0, targetTimelineSamples), static_cast<int64_t>(minimumRealSamples)));
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

inline PacketTimelineAdjustment ComputePacketTimelineAdjustment(int64_t packetStartSamples,
                                                                int64_t writtenTimelineSamples,
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

inline int64_t ComputeStartupFirstPacketRebaseOffset(int64_t packetStartSamples, bool sawSyncPendingPackets,
                                                     int64_t cappedStartupGapSamples, int64_t rebaseThresholdSamples) {
    if (!sawSyncPendingPackets) {
        return 0;
    }

    const int64_t clampedPacketStartSamples = std::max<int64_t>(0, packetStartSamples);
    const int64_t clampedCappedGapSamples = std::max<int64_t>(0, cappedStartupGapSamples);
    const int64_t clampedThresholdSamples = std::max<int64_t>(clampedCappedGapSamples, rebaseThresholdSamples);
    if (clampedPacketStartSamples < clampedThresholdSamples || clampedPacketStartSamples <= clampedCappedGapSamples) {
        return 0;
    }

    return clampedPacketStartSamples - clampedCappedGapSamples;
}

inline int64_t ApplyStartupPacketTimelineRebaseOffset(int64_t packetStartSamples, int64_t startupRebasedGapSamples) {
    const int64_t clampedPacketStartSamples = std::max<int64_t>(0, packetStartSamples);
    const int64_t clampedRebasedGapSamples = std::max<int64_t>(0, startupRebasedGapSamples);
    if (clampedRebasedGapSamples <= 0) {
        return clampedPacketStartSamples;
    }

    return std::max<int64_t>(0, clampedPacketStartSamples - clampedRebasedGapSamples);
}

inline PacketTimelineAdjustment ComputeStartupAwarePacketTimelineAdjustment(
    int64_t packetStartSamples, int64_t writtenTimelineSamples, int64_t steadyStateSlopSamples,
    int64_t startupWindowSamples, int64_t startupSlopSamples, int64_t startupOverlapTrimThresholdSamples) {
    const int64_t startupBoundarySamples =
        std::max(std::max<int64_t>(0, packetStartSamples), std::max<int64_t>(0, writtenTimelineSamples));
    const bool startupSettling = startupBoundarySamples < std::max<int64_t>(0, startupWindowSamples);

    PacketTimelineAdjustment adjustment = ComputePacketTimelineAdjustment(
        packetStartSamples, writtenTimelineSamples, startupSettling ? startupSlopSamples : steadyStateSlopSamples);
    if (startupSettling && adjustment.overlapSamples > 0 &&
        adjustment.overlapSamples < std::max<int64_t>(0, startupOverlapTrimThresholdSamples)) {
        adjustment.overlapSamples = 0;
    }

    return adjustment;
}

}  // namespace ce::audio
