#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ce::audio {

constexpr int64_t kDefaultSteadyAudioPullLatencyMs = 60;
constexpr int64_t kSettledCfrAudioPullLatencyMs = 0;
constexpr int64_t kDefaultAudioPullQuantumSamples = 240;

struct PacketTimelineAdjustment {
    int64_t gapSamples = 0;
    int64_t overlapSamples = 0;
};

struct LateAppSourceJoin {
    bool joinLive = false;
    int64_t joinCursorSamples = 0;
    int64_t preservedGapSamples = 0;
    int64_t suppressedGapSamples = 0;
};

struct PacketEndClamp {
    bool keep = true;
    bool clamped = false;
    int64_t durationSamples = 0;
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

inline bool ShouldUseSampleExactCfrAudioPull(bool isCfrRecording, bool trackStartupSettled, bool forceDrain) {
    return isCfrRecording && trackStartupSettled && !forceDrain;
}

inline int64_t ComputeAudioSamplesToEncode(int64_t pendingSamples, bool isCfrRecording, bool trackStartupSettled,
                                           bool forceDrain, bool initialTrackCatchup, bool overloadPullQuantum,
                                           int64_t defaultQuantumSamples = kDefaultAudioPullQuantumSamples,
                                           int64_t overloadQuantumSamples = kDefaultAudioPullQuantumSamples) {
    if (pendingSamples <= 0) {
        return 0;
    }

    if (ShouldUseSampleExactCfrAudioPull(isCfrRecording, trackStartupSettled, forceDrain)) {
        return pendingSamples;
    }

    const int64_t quantumSamples = overloadPullQuantum ? overloadQuantumSamples : defaultQuantumSamples;
    if (ShouldDeferAudioPullUntilQuantum(pendingSamples, trackStartupSettled, forceDrain, quantumSamples)) {
        return 0;
    }

    if (trackStartupSettled && !forceDrain && !initialTrackCatchup) {
        const int64_t boundedQuantum = std::max<int64_t>(1, quantumSamples);
        return (pendingSamples / boundedQuantum) * boundedQuantum;
    }

    return pendingSamples;
}

inline bool ShouldUseCfrAudioContinuityPolicy(bool useVfr) {
    return !useVfr;
}

inline bool ShouldAllowWallClockAudioAnchor(bool isCfrRecording, bool forceDrain, int64_t wallVideoLagMs,
                                            int64_t minLagMs = 500) {
    return !forceDrain && !isCfrRecording && wallVideoLagMs > std::max<int64_t>(0, minLagMs);
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

// Effective delivered FPS for WGC audio-continuity decisions: the worst (minimum)
// of the instantaneous delivered rate and the windowed minimums, ignoring windows
// that have not measured yet (reported as 0). A transient delivery dip in any
// window must not be masked by a healthy instantaneous rate when deciding whether
// to protect audio continuity during encoder overload.
inline uint32_t ComputeEffectiveDeliveredFpsForAudioContinuity(uint32_t deliveredFps, uint32_t deliveredMin250Fps,
                                                               uint32_t deliveredMin500Fps) {
    uint32_t effective = deliveredFps;
    if (deliveredMin250Fps > 0) {
        effective = std::min(effective, deliveredMin250Fps);
    }
    if (deliveredMin500Fps > 0) {
        effective = std::min(effective, deliveredMin500Fps);
    }
    return effective;
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

    const uint32_t effectiveDeliveredFps =
        ComputeEffectiveDeliveredFpsForAudioContinuity(deliveredFps, deliveredMin250Fps, deliveredMin500Fps);

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

// WGC live source-selection bias (signed us) split into how far the selected
// content runs AHEAD of (lead) and BEHIND (lag) the scheduled timeline, in ms.
// A positive bias means the scheduler is picking content that leads the timeline.
inline int64_t ComputeWgcSelectedContentLeadMs(int64_t selectionBiasUs) {
    return std::max<int64_t>(0, selectionBiasUs / 1000);
}
inline int64_t ComputeWgcSelectedContentLagMs(int64_t selectionBiasUs) {
    return std::max<int64_t>(0, (-selectionBiasUs) / 1000);
}

// Visual content lag the audio path should target: the timeline shortfall plus how
// far the selected content trails, minus how far it leads, clamped to [0, max].
// Captures how far behind real visual content the encoded video actually is so
// audio can be held to match it without over-buffering.
inline int64_t ComputeWgcVisualContentLagMs(int64_t timelineShortfallMs, int64_t selectedContentLeadMs,
                                            int64_t selectedContentLagMs, int64_t maxBufferedLagMs) {
    return std::clamp<int64_t>(timelineShortfallMs + selectedContentLagMs - selectedContentLeadMs, 0, maxBufferedLagMs);
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

inline int64_t ComputeSettledCfrAudioPullLatencyMs(int64_t currentLatencyMs, bool trackStartupSettled,
                                                   bool allSourcesPrimed,
                                                   int64_t settledLatencyMs = kSettledCfrAudioPullLatencyMs) {
    if (!trackStartupSettled || !allSourcesPrimed) {
        return std::max<int64_t>(0, currentLatencyMs);
    }

    return std::min<int64_t>(std::max<int64_t>(0, currentLatencyMs), std::max<int64_t>(0, settledLatencyMs));
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

inline int64_t ComputeSamplesToDurationUs(int64_t samples, int sampleRate) {
    if (sampleRate <= 0 || samples == 0) {
        return 0;
    }

    constexpr int64_t kMicrosecondsPerSecond = 1000000;
    const bool negative = samples < 0;
    const int64_t absSamples = negative ? -samples : samples;
    const int64_t durationUs = ((absSamples * kMicrosecondsPerSecond) + (static_cast<int64_t>(sampleRate) / 2)) /
                               static_cast<int64_t>(sampleRate);
    return negative ? -durationUs : durationUs;
}

inline size_t ComputeAudioSampleBufferBytes(int64_t samples, int bytesPerSample, int channels) {
    if (samples <= 0 || bytesPerSample <= 0 || channels <= 0) {
        return 0;
    }

    return static_cast<size_t>(samples) * static_cast<size_t>(bytesPerSample) * static_cast<size_t>(channels);
}

inline size_t ComputeAudioPlaneOffsetBytes(int planeIndex, int64_t samplesPerPlane, int bytesPerSample, bool planar) {
    if (!planar || planeIndex <= 0 || samplesPerPlane <= 0 || bytesPerSample <= 0) {
        return 0;
    }

    return static_cast<size_t>(planeIndex) * static_cast<size_t>(samplesPerPlane) * static_cast<size_t>(bytesPerSample);
}

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

enum class CfrAppAudioBacklogDrainReason : uint8_t {
    Active = 0,
    NotCfr,
    NotAppAudio,
    ForceDrain,
    StartupNotSettled,
    StartupTimelineProtected,
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
    case CfrAppAudioBacklogDrainReason::BufferBelowMinimum:
        return "buffer_below_minimum";
    case CfrAppAudioBacklogDrainReason::WithinSlack:
        return "within_slack";
    }
    return "unknown";
}

inline CfrAppAudioBacklogDrainDecision ComputeCfrAppAudioBacklogDrainDecision(
    bool isCfrRecording, bool isAppAudioSource, bool forceDrain, bool trackStartupSettled,
    bool startupTimelineProtected, int64_t rbAvailableSamples, int64_t targetLeadSamples,
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
    decision.reason = decision.active ? CfrAppAudioBacklogDrainReason::Active
                                      : CfrAppAudioBacklogDrainReason::WithinSlack;
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

// Catastrophic backlog resync for a CFR app-audio source.
//
// Normal CFR policy never trims app-audio backlog: sub-second drift is absorbed by
// repeating video frames, never by cutting audio (which would be an audible artifact
// for no benefit). But a SUSTAINED read-stall - the consume/encode side frozen by an
// alt-tab, a DPC latency spike, or encoder overload while live process-loopback capture
// keeps writing - makes the per-source ring backlog grow without bound. Left unchecked it
// saturates the (multi-second) ring, after which WriteRetainNew discards the very samples
// the reader still needs and the source can never recover: the track goes permanently
// silent. Those stalls are unavoidable in the field, so the pipeline must survive them.
//
// Above a CATASTROPHIC threshold (seconds of backlog - far beyond any legitimate jitter,
// so the healthy steady state is never touched) the only options are unbounded latency or
// a one-time resync. Resync wins: drop the stale backlog down to a small live cushion and
// keep the newest audio, so the source resumes contributing live audio immediately (the
// caller fades the seam). Returns the number of oldest samples to drop, or 0 to keep the
// no-trim policy. Scoped to CFR app-audio; everything else keeps the existing behavior.
inline int64_t ComputeCatastrophicBacklogResyncTrim(bool isCfrRecording, bool isAppAudioSource,
                                                    int64_t rbAvailableSamples, int64_t targetLatencySamples,
                                                    int64_t catastrophicThresholdSamples, int64_t minKeepSamples) {
    if (!isCfrRecording || !isAppAudioSource) {
        return 0;
    }
    if (rbAvailableSamples <= std::max<int64_t>(0, catastrophicThresholdSamples)) {
        return 0;
    }
    const int64_t keepSamples =
        std::max<int64_t>(std::max<int64_t>(0, minKeepSamples), std::max<int64_t>(0, targetLatencySamples));
    const int64_t trimSamples = rbAvailableSamples - keepSamples;
    return trimSamples > 0 ? trimSamples : 0;
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

inline bool ShouldAdvancePacketTimelineToEncodedCursor(bool isAppAudioSource) {
    return !isAppAudioSource;
}

inline bool IsAppAudioCaptureEpochTransition(bool isAppAudioSource, uint64_t previousEpoch, uint64_t packetEpoch) {
    return isAppAudioSource && previousEpoch != 0 && packetEpoch != 0 && packetEpoch != previousEpoch;
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

inline bool ShouldBootstrapPacketlessSourceAsSilence(bool isCfrRecording, bool sourceTimelineValid,
                                                      size_t bufferedRealSamples, int64_t targetTimelineSamples,
                                                      size_t requiredRealSamples) {
    return isCfrRecording && sourceTimelineValid && bufferedRealSamples == 0 && targetTimelineSamples > 0 &&
           static_cast<uint64_t>(targetTimelineSamples) >= static_cast<uint64_t>(requiredRealSamples);
}

inline bool ShouldTreatStartedAppSourceShortfallAsSilence(bool sparseStartedSourceMaySilence,
                                                          size_t bufferedTimelineSamples,
                                                          int64_t requestedSamples = 0,
                                                          int64_t partialSilenceThresholdSamples = 0) {
    if (!sparseStartedSourceMaySilence) {
        return false;
    }
    if (bufferedTimelineSamples == 0) {
        return true;
    }
    if (requestedSamples <= 0 || partialSilenceThresholdSamples <= 0 ||
        requestedSamples <= partialSilenceThresholdSamples) {
        return false;
    }
    return bufferedTimelineSamples < static_cast<size_t>(requestedSamples);
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

// A single packet's leading-silence timeline gap can never legitimately exceed
// what the destination ring buffer can retain: WriteRetainNew drops the oldest
// samples to make room, so any silence beyond the capacity is discarded the moment
// it is written. Bounding the gap to the ring capacity keeps a corrupt or
// out-of-domain packet timestamp from sizing a pathological silence allocation
// (the 192 kHz-loopback bad_alloc crash) while leaving every in-range gap
// untouched. ringCapacitySamples <= 0 disables the bound.
inline int64_t ClampTimelineGapSamplesToCapacity(int64_t gapSamples, int64_t ringCapacitySamples) {
    if (gapSamples <= 0) {
        return 0;
    }
    if (ringCapacitySamples <= 0) {
        return gapSamples;
    }
    return std::min<int64_t>(gapSamples, ringCapacitySamples);
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

inline int64_t ComputeSharedStartupFirstPacketRebaseOffset(int64_t earliestPacketStartSamples,
                                                           int64_t cappedStartupGapSamples,
                                                           int64_t rebaseThresholdSamples) {
    return ComputeStartupFirstPacketRebaseOffset(earliestPacketStartSamples, true, cappedStartupGapSamples,
                                                 rebaseThresholdSamples);
}

inline int64_t ApplyStartupPacketTimelineRebaseOffset(int64_t packetStartSamples, int64_t startupRebasedGapSamples) {
    const int64_t clampedPacketStartSamples = std::max<int64_t>(0, packetStartSamples);
    const int64_t clampedRebasedGapSamples = std::max<int64_t>(0, startupRebasedGapSamples);
    if (clampedRebasedGapSamples <= 0) {
        return clampedPacketStartSamples;
    }

    return std::max<int64_t>(0, clampedPacketStartSamples - clampedRebasedGapSamples);
}

inline bool ShouldPreservePendingAudioPacketsForStartupSync(bool isWgcCfrRecording, int64_t wgcStartupExtraDelayQpc) {
    return isWgcCfrRecording && wgcStartupExtraDelayQpc > 0;
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

// Smooth soft-knee limiter for the final per-track mix. Samples with magnitude
// at or below `knee` pass through bit-exact; above the knee the excess is mapped
// through x/(1+x) so the output asymptotically approaches (but never reaches)
// +-1.0. This guarantees no hard clip / no clipped click at the encoder input
// while keeping in-range audio fully transparent (important for lossless codecs).
//
// NOTE: deliberately contains NO additional global waveshaper (e.g. tanh). An
// unconditional tanh squashes and harmonically distorts every sample even when
// far below full scale, which audibly thins the audio. The soft-knee alone is
// sufficient to bound the signal.
// Stable identity for an app-audio capture targeting a specific track. Two
// app-audio sources that resolve to the same process and feed the same track
// would capture identical audio twice; summing those independently buffered,
// phase-offset streams into one track causes comb-filter ("metallic") artifacts.
// Process name is matched case-insensitively; an explicit PID is used when no
// name is set.
inline std::string AppAudioTrackIdentity(const std::string& processName, unsigned long processId, int track) {
    std::string id;
    if (!processName.empty()) {
        id = processName;
        std::transform(id.begin(), id.end(), id.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    } else {
        id = "pid:" + std::to_string(processId);
    }
    return id + "#" + std::to_string(track);
}

inline void ApplySoftKneeLimiter(float* buffer, size_t count, float knee = 0.9f) {
    if (!buffer || count == 0) {
        return;
    }
    const float range = 1.0f - knee;
    if (range <= 0.0f) {
        return;
    }
    const float scale = 1.0f / range;
    for (size_t i = 0; i < count; ++i) {
        float s = buffer[i];
        if (s > knee) {
            const float excess = (s - knee) * scale;
            buffer[i] = knee + range * (excess / (1.0f + excess));
        } else if (s < -knee) {
            const float excess = (-s - knee) * scale;
            buffer[i] = -(knee + range * (excess / (1.0f + excess)));
        }
        // else: |s| <= knee -> untouched (bit-exact passthrough)
    }
}

}  // namespace ce::audio
