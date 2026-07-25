#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

#include "timeline_constants.h"

// Packet duration clamping, lead trimming, and clock-mismatch drift compensation.

namespace ce::audio {

inline PacketEndClamp ClampPacketDurationToTargetSamples(int64_t packetPtsSamples, int64_t packetDurationSamples,
                                                         int64_t targetSamples) {
    PacketEndClamp result{};
    result.durationSamples = packetDurationSamples;

    if (targetSamples <= 0 || packetPtsSamples < 0) {
        return result;
    }

    if (packetPtsSamples >= targetSamples) {
        result.keep = false;
        result.durationSamples = 0;
        return result;
    }

    if (packetDurationSamples > 0 && packetDurationSamples > targetSamples - packetPtsSamples) {
        result.clamped = true;
        result.durationSamples = std::max<int64_t>(1, targetSamples - packetPtsSamples);
    }

    return result;
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

inline int32_t ComputeTier1CompensationDeltaWithDeadband(int64_t trueDriftSamples, int64_t compensationWindowSamples,
                                                         double maxPitchPercent = 0.5,
                                                         int64_t deadbandSamples = kDefaultAudioPullQuantumSamples) {
    const int64_t boundedDeadband = std::max<int64_t>(0, deadbandSamples);
    if (std::abs(trueDriftSamples) <= boundedDeadband) {
        return 0;
    }

    const int64_t adjustedDrift =
        trueDriftSamples > 0 ? trueDriftSamples - boundedDeadband : trueDriftSamples + boundedDeadband;
    return ComputeTier1CompensationDelta(adjustedDrift, compensationWindowSamples, maxPitchPercent);
}

inline int64_t ResolveAudioSourceClockDriftLagMs(bool isCfrRecording, bool isScreenGrabCfrRecording,
                                                 int64_t screenGrabDriftLagMs, int64_t videoPipelineLagMs,
                                                 int64_t timelineShortfallMs) {
    if (!isCfrRecording) {
        return std::max<int64_t>(0, videoPipelineLagMs) + std::max<int64_t>(0, timelineShortfallMs);
    }

    // Encoder/lookahead latency is downstream of capture and says nothing about
    // the source clock. Screen capture has its own measured content-lag target;
    // inject CFR must therefore keep only the base ingestion reservoir.
    return isScreenGrabCfrRecording ? std::max<int64_t>(0, screenGrabDriftLagMs) : 0;
}

inline int64_t ResolveAudioTargetBufferLagMs(bool isCfrRecording, bool isScreenGrabCfrRecording,
                                              int64_t screenGrabTargetLagMs, int64_t videoPipelineLagMs) {
    if (!isCfrRecording) {
        return std::max<int64_t>(0, videoPipelineLagMs);
    }

    return isScreenGrabCfrRecording ? std::max<int64_t>(0, screenGrabTargetLagMs) : 0;
}

enum class CfrAppAudioBacklogDrainReason : uint8_t {
    Active = 0,
    NotCfr,
    NotAppAudio,
    ForceDrain,
    StartupNotSettled,
    StartupTimelineProtected,
    TimelineRecoveryActive,
    BufferBelowMinimum,
    WithinSlack,
};

struct CfrAppAudioBacklogDrainDecision {
    bool active = false;
    int64_t targetLeadSamples = 0;
    int64_t backlogSamples = 0;
    int64_t excessSamples = 0;
    int32_t compensationDelta = 0;
    CfrAppAudioBacklogDrainReason reason = CfrAppAudioBacklogDrainReason::WithinSlack;
};

inline const char* CfrAppAudioBacklogDrainReasonName(CfrAppAudioBacklogDrainReason reason) {
    switch (reason) {
        case CfrAppAudioBacklogDrainReason::Active:
            return "active";
        case CfrAppAudioBacklogDrainReason::NotCfr:
            return "not_cfr";
        case CfrAppAudioBacklogDrainReason::NotAppAudio:
            return "not_app_audio";
        case CfrAppAudioBacklogDrainReason::ForceDrain:
            return "force_drain";
        case CfrAppAudioBacklogDrainReason::StartupNotSettled:
            return "startup_not_settled";
        case CfrAppAudioBacklogDrainReason::StartupTimelineProtected:
            return "startup_timeline_protected";
        case CfrAppAudioBacklogDrainReason::TimelineRecoveryActive:
            return "timeline_recovery";
        case CfrAppAudioBacklogDrainReason::BufferBelowMinimum:
            return "buffer_below_minimum";
        case CfrAppAudioBacklogDrainReason::WithinSlack:
            return "within_slack";
    }
    return "unknown";
}

inline CfrAppAudioBacklogDrainDecision ComputeCfrAppAudioBacklogDrainDecision(
    bool isCfrRecording, bool isAppAudioSource, bool forceDrain, bool trackStartupSettled,
    bool startupTimelineProtected, bool timelineRecoveryActive, int64_t rbAvailableSamples, int64_t targetLeadSamples,
    int64_t minCompensationBufferSamples, int64_t compensationWindowSamples, double maxPitchPercent,
    int64_t activationSlackSamples, int64_t compensationDeadbandSamples) {
    CfrAppAudioBacklogDrainDecision decision;
    decision.targetLeadSamples = std::max<int64_t>(0, targetLeadSamples);
    decision.backlogSamples = std::max<int64_t>(0, rbAvailableSamples);
    decision.excessSamples = std::max<int64_t>(0, decision.backlogSamples - decision.targetLeadSamples);

    if (!isCfrRecording) {
        decision.reason = CfrAppAudioBacklogDrainReason::NotCfr;
        return decision;
    }
    if (!isAppAudioSource) {
        decision.reason = CfrAppAudioBacklogDrainReason::NotAppAudio;
        return decision;
    }
    if (forceDrain) {
        decision.reason = CfrAppAudioBacklogDrainReason::ForceDrain;
        return decision;
    }
    if (!trackStartupSettled) {
        decision.reason = CfrAppAudioBacklogDrainReason::StartupNotSettled;
        return decision;
    }
    if (startupTimelineProtected) {
        decision.reason = CfrAppAudioBacklogDrainReason::StartupTimelineProtected;
        return decision;
    }
    if (timelineRecoveryActive) {
        // When CFR output is behind wall time, every audio source naturally
        // retains extra live capture. The video scheduler must repay that debt;
        // accelerating only process-loopback audio would move it relative to
        // system/mic tracks and the immutable video-content timeline.
        decision.reason = CfrAppAudioBacklogDrainReason::TimelineRecoveryActive;
        return decision;
    }
    if (decision.backlogSamples < std::max<int64_t>(0, minCompensationBufferSamples)) {
        decision.reason = CfrAppAudioBacklogDrainReason::BufferBelowMinimum;
        return decision;
    }
    if (decision.excessSamples <= std::max<int64_t>(0, activationSlackSamples)) {
        decision.reason = CfrAppAudioBacklogDrainReason::WithinSlack;
        return decision;
    }

    decision.compensationDelta = ComputeTier1CompensationDeltaWithDeadband(
        decision.excessSamples, compensationWindowSamples, maxPitchPercent, compensationDeadbandSamples);
    decision.active = decision.compensationDelta > 0;
    decision.reason =
        decision.active ? CfrAppAudioBacklogDrainReason::Active : CfrAppAudioBacklogDrainReason::WithinSlack;
    return decision;
}

inline bool ShouldActivateTier2Trim(int64_t trueDriftSamples, int sampleRate, int64_t thresholdMs = 20) {
    if (sampleRate <= 0) {
        return false;
    }
    const int64_t thresholdSamples = (static_cast<int64_t>(sampleRate) * thresholdMs) / 1000;
    return trueDriftSamples > thresholdSamples;
}

inline bool ShouldSuppressCfrPositiveDriftCorrectionDuringLiveShortfall(bool isCfrRecording, bool forceDrain,
                                                                        int64_t timelineShortfallMs,
                                                                        bool encoderBottlenecked,
                                                                        int64_t minShortfallMs = 100) {
    if (!isCfrRecording || forceDrain) {
        return false;
    }

    return encoderBottlenecked || timelineShortfallMs >= std::max<int64_t>(0, minShortfallMs);
}

inline bool ShouldSuppressWgcPositiveDriftCorrectionDuringLiveShortfall(bool isWgcCfrRecording, bool forceDrain,
                                                                        int64_t timelineShortfallMs,
                                                                        bool encoderBottlenecked,
                                                                        int64_t minShortfallMs = 100) {
    return ShouldSuppressCfrPositiveDriftCorrectionDuringLiveShortfall(
        isWgcCfrRecording, forceDrain, timelineShortfallMs, encoderBottlenecked, minShortfallMs);
}

inline bool ShouldAllowCfrSourceClockDriftCompensation(bool isCfrRecording, bool forceDrain, bool trackStartupSettled,
                                                       bool startupTimelineProtected, bool encoderBottlenecked,
                                                       int64_t timelineShortfallMs, bool coverageLossActive,
                                                       int64_t minShortfallMs = 100) {
    if (!isCfrRecording || forceDrain || !trackStartupSettled || startupTimelineProtected || coverageLossActive) {
        return false;
    }

    return !ShouldSuppressCfrPositiveDriftCorrectionDuringLiveShortfall(isCfrRecording, forceDrain, timelineShortfallMs,
                                                                        encoderBottlenecked, minShortfallMs);
}

inline int64_t ComputeRuntimeOverflowCapSamples(bool isCfrContinuityProtected, int64_t targetLatencySamples,
                                                int64_t ringCapacitySamples, int64_t defaultOverflowCapSamples,
                                                int64_t emergencyMarginSamples) {
    const int64_t defaultCap = std::max<int64_t>(0, defaultOverflowCapSamples);
    if (!isCfrContinuityProtected || ringCapacitySamples <= 0) {
        return defaultCap;
    }

    const int64_t emergencyCap =
        ringCapacitySamples - std::max<int64_t>(0, targetLatencySamples) - std::max<int64_t>(0, emergencyMarginSamples);
    return std::max<int64_t>(defaultCap, emergencyCap);
}

// A track that has fallen more than maxGapSamples behind the live target is catching up after a
// read-stall (alt-tab, DPC spike, encoder overload) and MUST make forward progress on every pull.
// When this returns true the caller suppresses the buffer-wait defer: otherwise a co-mixed source
// sitting at the live edge (a second app keeping up at ~100ms) never accrues the full catch-up
// chunk, so the defer fires every iteration, the track cursor freezes, and the stalled source's
// ring saturates into permanent silence. Only triggers when already multiple seconds behind, so a
// healthy track keeps the defer/buffer-wait protection. initialTrackCatchup is the legitimate
// startup catch-up (its own path) and is never treated as a stall.
inline bool ShouldSuppressBufferDeferForCatchup(int64_t samplesToEncode, int64_t maxGapSamples,
                                                bool initialTrackCatchup) {
    return samplesToEncode > std::max<int64_t>(0, maxGapSamples) && !initialTrackCatchup;
}

inline int64_t ComputeAudioSamplesAllowedBeforeEnd(int64_t targetSamples, int64_t encodedSamples,
                                                   int64_t queuedSamples) {
    return std::max<int64_t>(0, std::max<int64_t>(0, targetSamples) - std::max<int64_t>(0, encodedSamples) -
                                    std::max<int64_t>(0, queuedSamples));
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

inline bool ShouldRememberPreStartPacketForAppBootstrap(bool isAppAudioSource, bool firstSourcePacket,
                                                        int64_t packetTimestampMs, int64_t recordingStartQpcMs,
                                                        int64_t preStartToleranceMs = 5) {
    if (!isAppAudioSource || !firstSourcePacket || recordingStartQpcMs == 0) {
        return false;
    }

    return packetTimestampMs < (recordingStartQpcMs - std::max<int64_t>(0, preStartToleranceMs));
}

inline bool IsOptionalUnstartedAppAudioSource(bool isAppAudioSource, bool sourceTimelineValid,
                                              bool sawPreStartPackets = false) {
    return isAppAudioSource && !sourceTimelineValid && !sawPreStartPackets;
}

inline bool IsExpectedSourceTimelineSilence(bool isAppAudioSource, bool appCaptureRouteEnded,
                                            bool isSystemLoopbackSource, bool hasAlignedStart) {
    return (isAppAudioSource && appCaptureRouteEnded) || (isSystemLoopbackSource && !hasAlignedStart);
}

inline bool IsSourceStartupPrimed(bool sourceIsPrimed, bool sourceTimelineValid, bool isAppAudioSource,
                                  bool sawPreStartPackets = false) {
    return sourceIsPrimed ||
           IsOptionalUnstartedAppAudioSource(isAppAudioSource, sourceTimelineValid, sawPreStartPackets);
}

inline bool IsSourceBootstrapReady(bool sourceBootstrapComplete, bool sourceTimelineValid, bool sourceIsPrimed,
                                   bool isAppAudioSource, size_t bufferedRealSamples, size_t requiredRealSamples,
                                   bool sawPreStartPackets = false) {
    if (sourceBootstrapComplete) {
        return true;
    }

    if (IsOptionalUnstartedAppAudioSource(isAppAudioSource, sourceTimelineValid, sawPreStartPackets)) {
        return true;
    }

    if (!sourceTimelineValid) {
        return false;
    }

    return sourceIsPrimed || bufferedRealSamples >= requiredRealSamples;
}

inline bool IsSourceBootstrapTimelineReady(bool sourceBootstrapComplete, bool optionalUnstartedSource, bool sourceReady,
                                           bool packetlessSilenceReady, size_t bufferedTimelineSamples,
                                           int64_t targetTimelineSamples) {
    if (!sourceReady && !packetlessSilenceReady) {
        return false;
    }

    // Already-established and genuinely packetless/optional sources have no additional
    // timeline evidence to wait for. A started source with buffered audio must cover the
    // first requested range so bootstrap cannot manufacture a short silence notch.
    if (sourceBootstrapComplete || optionalUnstartedSource || packetlessSilenceReady || targetTimelineSamples <= 0) {
        return true;
    }

    return bufferedTimelineSamples >= static_cast<size_t>(targetTimelineSamples);
}

inline size_t ComputeRequiredBootstrapRealSamples(int64_t targetTimelineSamples, size_t minimumRealSamples) {
    (void)targetTimelineSamples;
    return minimumRealSamples;
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

inline int64_t ResolveSourceTimelineWriteCursor(uint64_t qpcAlignedWrittenSamples, int64_t encodedCursorSamples) {
    return std::max<int64_t>(static_cast<int64_t>(qpcAlignedWrittenSamples),
                             std::max<int64_t>(0, encodedCursorSamples));
}

inline bool IsAudioCaptureEpochTransition(uint64_t previousEpoch, uint64_t packetEpoch) {
    return previousEpoch != 0 && packetEpoch != 0 && packetEpoch != previousEpoch;
}

inline bool IsAppAudioCaptureEpochTransition(bool isAppAudioSource, uint64_t previousEpoch, uint64_t packetEpoch) {
    return isAppAudioSource && IsAudioCaptureEpochTransition(previousEpoch, packetEpoch);
}

inline LateAppSourceJoin ComputeLateAppSourceJoin(bool isAppAudioSource, bool firstTimelinePacket,
                                                  bool sawPreStartPackets, int64_t packetStartSamples,
                                                  int64_t trackCursorSamples, int64_t lateJoinThresholdSamples,
                                                  int64_t preservedCushionSamples) {
    LateAppSourceJoin result{};
    if (!isAppAudioSource || !firstTimelinePacket || sawPreStartPackets) {
        return result;
    }

    const int64_t packetStart = std::max<int64_t>(0, packetStartSamples);
    const int64_t trackCursor = std::max<int64_t>(0, trackCursorSamples);
    const int64_t threshold = std::max<int64_t>(0, lateJoinThresholdSamples);
    if (packetStart <= threshold || trackCursor <= threshold) {
        return result;
    }

    const int64_t preservedCushion = std::clamp<int64_t>(preservedCushionSamples, 0, packetStart);
    const int64_t liveCursor = std::max<int64_t>(trackCursor, packetStart - preservedCushion);
    result.joinLive = liveCursor > 0;
    result.joinCursorSamples = liveCursor;
    result.preservedGapSamples = std::max<int64_t>(0, packetStart - liveCursor);
    result.suppressedGapSamples = std::max<int64_t>(0, packetStart - result.preservedGapSamples);
    return result;
}

inline bool ShouldDrainStoppedCaptureQueuesBeforeFinalAudioPull(bool audioThreadRunning, bool audioOnlyRecording,
                                                                int64_t videoTargetDurationUs) {
    return audioThreadRunning && !audioOnlyRecording && videoTargetDurationUs > 0;
}

inline bool ShouldDeferCfrAudioPullForSourceBuffer(bool isCfrRecording, bool forceDrain, bool optionalUnstartedSource,
                                                   bool sparseStartedSourceMaySilence, int64_t requestedSamples,
                                                   size_t bufferedTimelineSamples) {
    if (!isCfrRecording || forceDrain || optionalUnstartedSource || sparseStartedSourceMaySilence ||
        requestedSamples <= 0) {
        return false;
    }

    return bufferedTimelineSamples < static_cast<size_t>(requestedSamples);
}

inline bool ShouldWaitForFinalCfrSourceCatchup(bool isCfrRecording, bool strictSource, bool optionalUnstartedSource,
                                               bool sparseStartedSourceMaySilence, int64_t requestedSamples,
                                               size_t bufferedTimelineSamples) {
    if (!isCfrRecording || !strictSource || optionalUnstartedSource || sparseStartedSourceMaySilence ||
        requestedSamples <= 0) {
        return false;
    }

    return bufferedTimelineSamples < static_cast<size_t>(requestedSamples);
}

inline bool ShouldTreatSparseStartedSourceAsSilence(bool isCfrRecording, bool sourceTimelineValid,
                                                    bool sourceBootstrapComplete, bool optionalUnstartedSource,
                                                    bool finalStopDrain = false) {
    (void)finalStopDrain;
    // WASAPI loopback and microphone clients may legally produce no packets
    // while silent.  Once their shared timeline is valid and bootstrap has
    // completed, the missing interval is silence for every source type, not a
    // reason to block a co-mixed track indefinitely.
    return isCfrRecording && sourceTimelineValid && sourceBootstrapComplete && !optionalUnstartedSource;
}

}  // namespace ce::audio
