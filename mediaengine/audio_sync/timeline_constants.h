#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

// Latency/drift tunables and the shared audio timeline accounting.

namespace ce::audio {

constexpr int64_t kDefaultSteadyAudioPullLatencyMs = 60;
// This is capture/encode scheduling lookahead, not a content or PTS offset. Keeping the
// CFR consumer behind the live capture edge absorbs ordinary WASAPI/process-loopback
// delivery jitter; the final force-drain still closes every track at the exact video end.
constexpr int64_t kSettledCfrAudioPullLatencyMs = kDefaultSteadyAudioPullLatencyMs;
constexpr int64_t kDefaultAudioPullQuantumSamples = 240;

// ---------------------------------------------------------------------------
// Adaptive CFR audio ingestion reservoir
// ---------------------------------------------------------------------------
// The fixed steady reservoir above is the ONLY margin between the CFR consumer
// and the live capture edge. Its realized headroom is
// `reservoirMs - captureDeliveryLatencyMs`, typically only 40-70 ms.
//
// If a single transient (encoder overload, DPC storm, scheduler hiccup) lets the
// consumer overrun that margin, the track exports silence for the missing range
// and the per-source write cursor is pinned forward to the exported cursor. Every
// later packet then places BEHIND that cursor, is trimmed as timeline overlap, and
// is destroyed. Because the exported cursor and the capture edge both advance at
// exactly wall rate, the resulting deficit can never repay itself: the source goes
// permanently silent while packet delivery stays perfectly healthy, and the file
// still has sample-exact track lengths. Session `logs/audiodeath` lost ~3.5 minutes
// of every audio track this way after one ~67 ms overrun.
//
// Deepening the reservoir is SYNC-NEUTRAL BY CONSTRUCTION: it changes only WHEN
// samples are pulled (the pull target), never WHERE they land on the timeline
// (the track cursor). Freezing the pull target lets the producer overtake the
// consumer again with zero content loss, zero PTS change, and zero pitch change.
constexpr int64_t kAudioIngestMinHeadroomMs = 25;
constexpr int64_t kAudioIngestMaxExtraReservoirMs = 500;
constexpr int64_t kAudioIngestReservoirMarginMs = 20;
constexpr int64_t kAudioIngestReservoirDecayIntervalMs = 2000;
constexpr int64_t kAudioIngestReservoirDecayStepMs = 5;
constexpr int64_t kAudioIngestReservoirDecaySlackMs = 25;
// A source that has not delivered a real packet for this long is genuinely idle
// (silent app, stopped endpoint) rather than late; its missing range stays
// timeline silence and must never block a co-mixed track.
constexpr int64_t kAudioIngestLiveSourceWindowMs = 400;

struct AudioIngestReservoirState {
    int64_t extraMs = 0;
    int64_t healthyElapsedMs = 0;
};

struct AudioIngestReservoirDecision {
    int64_t extraMs = 0;
    int64_t healthyElapsedMs = 0;
    bool raised = false;
    bool decayed = false;
    bool atCap = false;
};

// Instant response path: a live source that is short by `shortfallMs` needs at
// least that much extra scheduling lookahead plus a margin. Ratchets up only.
inline int64_t RaiseAudioIngestReservoirForShortfall(int64_t currentExtraMs, int64_t shortfallMs,
                                                     int64_t marginMs = kAudioIngestReservoirMarginMs,
                                                     int64_t maxExtraMs = kAudioIngestMaxExtraReservoirMs) {
    const int64_t boundedMaxMs = std::max<int64_t>(0, maxExtraMs);
    const int64_t boundedCurrentMs = std::clamp<int64_t>(currentExtraMs, 0, boundedMaxMs);
    if (shortfallMs <= 0) {
        return boundedCurrentMs;
    }

    return std::min<int64_t>(boundedMaxMs, boundedCurrentMs + shortfallMs + std::max<int64_t>(0, marginMs));
}

// Steady-state controller: `observedHeadroomMs` is the worst (minimum) distance
// between a freshly ingested packet's timeline placement and the already-exported
// cursor since the previous evaluation. Negative means the consumer already
// overran the producer and content is being destroyed.
inline AudioIngestReservoirDecision ComputeAudioIngestReservoir(
    const AudioIngestReservoirState& state, bool active, bool headroomObserved, int64_t observedHeadroomMs,
    int64_t elapsedMs, int64_t minHeadroomMs = kAudioIngestMinHeadroomMs,
    int64_t maxExtraMs = kAudioIngestMaxExtraReservoirMs, int64_t marginMs = kAudioIngestReservoirMarginMs,
    int64_t decayIntervalMs = kAudioIngestReservoirDecayIntervalMs,
    int64_t decayStepMs = kAudioIngestReservoirDecayStepMs, int64_t decaySlackMs = kAudioIngestReservoirDecaySlackMs) {
    AudioIngestReservoirDecision decision;
    const int64_t boundedMaxMs = std::max<int64_t>(0, maxExtraMs);
    decision.extraMs = std::clamp<int64_t>(state.extraMs, 0, boundedMaxMs);
    decision.healthyElapsedMs = std::max<int64_t>(0, state.healthyElapsedMs);
    if (!active) {
        decision.extraMs = 0;
        decision.healthyElapsedMs = 0;
        return decision;
    }
    if (!headroomObserved) {
        // No live placement evidence this window (every source idle/silent).
        // Hold the current reservoir; do not decay on missing evidence.
        decision.atCap = decision.extraMs >= boundedMaxMs;
        return decision;
    }

    const int64_t boundedMinHeadroomMs = std::max<int64_t>(0, minHeadroomMs);
    if (observedHeadroomMs < boundedMinHeadroomMs) {
        const int64_t raisedMs = RaiseAudioIngestReservoirForShortfall(
            decision.extraMs, boundedMinHeadroomMs - observedHeadroomMs, marginMs, boundedMaxMs);
        decision.raised = raisedMs > decision.extraMs;
        decision.extraMs = raisedMs;
        decision.healthyElapsedMs = 0;
        decision.atCap = decision.extraMs >= boundedMaxMs;
        return decision;
    }

    if (decision.extraMs <= 0 || observedHeadroomMs < boundedMinHeadroomMs + std::max<int64_t>(0, decaySlackMs)) {
        decision.healthyElapsedMs = 0;
        return decision;
    }

    decision.healthyElapsedMs += std::max<int64_t>(0, elapsedMs);
    const int64_t boundedIntervalMs = std::max<int64_t>(1, decayIntervalMs);
    if (decision.healthyElapsedMs >= boundedIntervalMs) {
        const int64_t steps = decision.healthyElapsedMs / boundedIntervalMs;
        decision.healthyElapsedMs -= steps * boundedIntervalMs;
        const int64_t decayMs = steps * std::max<int64_t>(1, decayStepMs);
        const int64_t decayedMs = std::max<int64_t>(0, decision.extraMs - decayMs);
        decision.decayed = decayedMs < decision.extraMs;
        decision.extraMs = decayedMs;
    }
    return decision;
}

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
    // The startup cap must never fall below the steady base: an adaptive ingestion reservoir can
    // push the base past 120 ms, and std::clamp with lo > hi is undefined behavior.
    return std::clamp<int64_t>(startupLatencyMs, baseLatencyMs, std::max<int64_t>(baseLatencyMs, 120));
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

}  // namespace ce::audio
