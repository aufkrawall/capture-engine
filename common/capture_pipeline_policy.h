#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>

namespace ce::capture_policy {

constexpr uint32_t kRecordingWarmupMinMs = 120;
constexpr uint32_t kRecordingWarmupMaxMs = 350;
constexpr size_t kInjectWarmupCommitFloorFrames = 3;
constexpr size_t kMaxInjectBufferedHeadroomFrames = 12;
constexpr size_t kStartupInjectBufferedHeadroomFrames = 48;
constexpr uint32_t kMaxInjectDeferredFrameRetries = 3;
constexpr uint32_t kInjectCfrPublicationHeadroomPermille = 4000;
constexpr int64_t kInjectCfrPublicationEarlySlackMinUs = 250;
constexpr int64_t kInjectCfrPublicationEarlySlackMaxUs = 1500;
constexpr uint32_t kInjectLiveHealthyMaxFrameAgeTicks = 3;
constexpr uint32_t kInjectLivePressureMaxFrameAgeTicks = 12;
constexpr uint64_t kEncoderStartupWindowMs = 1500;
constexpr uint32_t kAutoWgcFallbackDelayNoPidMs = 100;
constexpr uint32_t kAutoWgcFallbackDelayWithPidMs = 200;
constexpr uint32_t kWgcLowSourceEmptyTickPermille = 80;
constexpr uint32_t kWgcLowSourceExitEmptyTickPermille = 40;
constexpr uint32_t kWgcLowSourceEnterMarginFps = 2;
constexpr uint32_t kWgcLowSourceEnterHoldMs = 120;
constexpr uint32_t kWgcLowSourceExitHoldMs = 250;
constexpr uint32_t kWgcMaxSelectionLagTicks = 1;
constexpr uint32_t kWgcLowSourceMaxSelectionLagTicks = 3;
constexpr uint32_t kWgcWarmupBufferedFrames = 3;
constexpr uint32_t kWgcWarmupStableSourceFps = 118;
constexpr size_t kWgcWarmupFreshFrames = 2;
constexpr uint32_t kWgcReservePressurePermille = 600;
constexpr uint32_t kWgcReserveBiasPermille = 250;
constexpr uint32_t kWgcReserveFragileBiasPermille = 375;
constexpr uint32_t kWgcDeepUnderfeedReserveBiasPermille = 750;
constexpr uint32_t kWgcSingleFreshHoldInputPermille = 995;
constexpr uint32_t kWgcSteadyReserveBuildInputPermille = 995;
constexpr uint32_t kWgcSelectionDelayTicks = 1;
constexpr uint32_t kWgcCoverageDelayMaxTicks = 32;
constexpr uint32_t kWgcCfrSelectionMaxLeadTicks = 3;
constexpr uint32_t kWgcActiveDelayResidualTolerancePermille = 600;
constexpr uint32_t kWgcActiveDelayResidualHardLimitUs = 10000;
constexpr uint32_t kWgcActiveDelayResidualMeanTargetUs = 5000;
constexpr uint32_t kWgcActiveDelayResidualP95TargetUs = 10000;
constexpr uint32_t kWgcActiveDelaySoftLateTargetMinUs = 5000;
constexpr uint32_t kWgcActiveDelaySoftLateTargetMaxUs = 7000;
constexpr uint32_t kWgcActiveDelayRepeatClusterPenaltyPermille = 250;
constexpr uint32_t kWgcActiveDelayRepeatClusterPenaltyMaxPermille = 1000;
constexpr uint32_t kWgcActiveDelayPolicyHoldFaultMinCount = 120;
constexpr uint32_t kWgcActiveDelayPolicyHoldFaultPermille = 250;
constexpr uint32_t kWgcActiveDelaySourceRecoveryHoldMs = 1500;
constexpr uint32_t kWgcActiveDelayRecoverableJitterPermille = 1000;
constexpr uint32_t kWgcActiveDelaySourceLimitedJitterPermille = 2000;
constexpr uint32_t kWgcCfrSmoothnessExcessRepeatFaultMinCount = 120;
constexpr uint32_t kWgcCfrSmoothnessExcessRepeatFaultPermille = 50;
constexpr uint32_t kWgcCfrSmoothnessExcessRepeatClusterFaultTicks = 24;
constexpr uint32_t kWgcDelayReservoirTargetExtraFrames = 1;
constexpr uint32_t kWgcMaxLiveVisualDebtMs = 250;
constexpr uint32_t kWgcMaxLiveVisualDebtFrames = 32;
constexpr uint32_t kWgcMaxLiveSchedulerRebaseTicksPerLoop = 1;
constexpr uint32_t kWgcEncoderLimitedLiveVisualDebtMs = 50;
constexpr uint32_t kWgcEncoderLimitedLiveVisualDebtFrames = 3;
constexpr uint32_t kWgcEncoderLimitedSourceBufferFloorFrames = 4;
constexpr uint32_t kWgcEncoderLimitedLiveSchedulerRebaseTicksPerLoop = 4;
constexpr uint32_t kCfrShortfallCatchupThresholdTicks = 2;
constexpr uint32_t kCfrShortfallForceCatchupThresholdTicks = 18;
constexpr double kWgcSevereShortfallDurationMs = 500.0;
constexpr uint32_t kWgcDeepUnderfeedMarginFps = 8;
constexpr uint32_t kWgcDeepUnderfeedEmptyTickPermille = 350;
constexpr uint32_t kWgcDeepUnderfeedStaleFallbackLagTicks = 5;
constexpr uint32_t kWgcRecoverySourceMarginFps = 4;
constexpr uint32_t kWgcRecoveryEmptyTickPermille = 200;
constexpr uint32_t kWgcRecoveryEnterShortfallTicks = 3;
constexpr uint32_t kWgcRecoveryEnterHoldMs = 120;
constexpr uint32_t kWgcRecoveryExitHoldMs = 450;
constexpr double kWgcAudioLeadCatchupThresholdMs = 40.0;
// 0 means "uncapped": WGC CFR needs source candidates on both sides of the
// output grid for zero-latency selection. A modest 1.25x cap leaves too little
// phase margin and can turn normal callback jitter into visible CFR repeats.
constexpr uint32_t kWgcCfrOvercaptureHeadroomPermille = 0;
constexpr uint32_t kWgcCfrOvercaptureStableRestoreMs = 2000;
constexpr double kEncoderGpuPriorityRaiseBudgetRatio = 0.75;
constexpr double kEncoderGpuPriorityRestoreBudgetRatio = 0.50;
constexpr uint32_t kEncoderOverloadFlagEncoder = 1u;
constexpr uint32_t kEncoderOverloadFlagMux = 2u;
constexpr uint32_t kWgcCaptureHealthFlagSourceStarved = 1u;
constexpr uint32_t kWgcCaptureHealthFlagSchedulerLimited = 2u;
constexpr uint32_t kOverlayWarningNone = 0u;
constexpr uint32_t kOverlayWarningEncoderOverload = 1u;

inline uint32_t GetCfrOutputShortfallTicks(uint64_t liveTicksScheduled, uint64_t liveTicksOutput) {
    return liveTicksScheduled > liveTicksOutput
               ? static_cast<uint32_t>(std::min<uint64_t>(liveTicksScheduled - liveTicksOutput, 0xFFFFFFFFull))
               : 0u;
}

inline uint64_t GetAdjustedCfrScheduledTicks(uint64_t elapsedTicks, uint64_t discardedTicks) {
    return elapsedTicks > discardedTicks ? (elapsedTicks - discardedTicks) : 0ull;
}

inline uint64_t GetCfrScheduledTicksForEndpoint(uint64_t elapsedTicks, uint64_t discardedTimerDebtTicks,
                                                uint64_t wgcVisualDebtDiagnosticTicks) {
    (void)wgcVisualDebtDiagnosticTicks;
    // WGC visual debt is handled by source-frame holds/drops. It must not
    // shorten the CFR/audio endpoint, because that turns visual recovery into
    // an audio cut and makes stop timing depend on codec/capture pressure.
    return GetAdjustedCfrScheduledTicks(elapsedTicks, discardedTimerDebtTicks);
}

inline uint32_t GetCfrTimerRebaseDiscardTicks(uint64_t elapsedTicks, uint64_t discardedTicks,
                                              uint64_t liveTicksOutput) {
    return GetCfrOutputShortfallTicks(GetAdjustedCfrScheduledTicks(elapsedTicks, discardedTicks), liveTicksOutput);
}

inline bool ShouldDiscardCfrTimerRebaseDebt(bool useScreenGrab) {
    (void)useScreenGrab;
    // CFR tick debt is part of the recording timeline. Dropping it makes video
    // content jump forward while audio remains continuous, which creates real
    // content-level A/V drift even when packet durations still match.
    return false;
}

inline bool ShouldDrainOutstandingCfrTicksAtStop(bool useScreenGrab, bool useVFR) {
    if (useVFR) {
        return false;
    }

    // WGC has its own live scheduler. If it is behind at stop, draining old
    // slots with a cached frame creates the frozen video tail that users hear
    // as audio continuing after the picture stopped. WGC debt must be handled
    // while live by timestamp rebases and visual drops, not after stop.
    return !useScreenGrab;
}

inline bool ShouldUseInjectCaptureForAutoTarget(bool explicitInjectCapture, bool autoCapture,
                                                bool gameWhitelistMatched) {
    return explicitInjectCapture || (autoCapture && gameWhitelistMatched);
}

inline bool ShouldUseWgcCaptureForAutoTarget(bool explicitWgcCapture, bool autoCapture, bool gameWhitelistMatched) {
    return explicitWgcCapture || (autoCapture && !gameWhitelistMatched);
}

inline bool ShouldAcceptFrameForActiveCapturePath(bool activeScreenGrab, bool frameIsInjectMode) {
    return activeScreenGrab ? !frameIsInjectMode : frameIsInjectMode;
}

inline uint32_t GetInjectCfrSourcePublicationFps(uint32_t outputFps,
                                                 uint32_t headroomPermille = kInjectCfrPublicationHeadroomPermille) {
    if (outputFps == 0 || headroomPermille <= 1000u) {
        return outputFps;
    }

    return static_cast<uint32_t>((static_cast<uint64_t>(outputFps) * headroomPermille + 999ull) / 1000ull);
}

inline int64_t GetInjectCfrSourcePublicationIntervalUs(
    uint32_t outputFps, uint32_t headroomPermille = kInjectCfrPublicationHeadroomPermille) {
    const uint32_t publicationFps = GetInjectCfrSourcePublicationFps(outputFps, headroomPermille);
    return publicationFps > 0 ? 1000000LL / static_cast<int64_t>(publicationFps) : 0;
}

inline int64_t GetInjectCfrSourcePublicationIntervalQpc(
    uint32_t outputFps, int64_t qpcTicksPerSecond, uint32_t headroomPermille = kInjectCfrPublicationHeadroomPermille) {
    if (qpcTicksPerSecond <= 0) {
        return 0;
    }

    const uint32_t publicationFps = GetInjectCfrSourcePublicationFps(outputFps, headroomPermille);
    return publicationFps > 0 ? std::max<int64_t>(1, qpcTicksPerSecond / static_cast<int64_t>(publicationFps)) : 0;
}

inline int64_t GetInjectCfrPublicationEarlySlackUs(int64_t publicationIntervalUs) {
    if (publicationIntervalUs <= 0) {
        return 0;
    }

    const int64_t jitterSlackUs = std::clamp(publicationIntervalUs / 8, kInjectCfrPublicationEarlySlackMinUs,
                                             kInjectCfrPublicationEarlySlackMaxUs);
    return std::min(publicationIntervalUs / 2, jitterSlackUs);
}

inline int64_t GetInjectCfrPublicationMinSpacingUs(int64_t publicationIntervalUs) {
    if (publicationIntervalUs <= 0) {
        return 0;
    }

    return std::max<int64_t>(1, publicationIntervalUs - GetInjectCfrPublicationEarlySlackUs(publicationIntervalUs));
}

inline uint32_t GetCfrTimerRebaseThresholdTicks(bool useScreenGrab, bool useVFR, bool recordingOutputLive) {
    if (!recordingOutputLive || useVFR) {
        return kCfrShortfallCatchupThresholdTicks;
    }

    if (useScreenGrab) {
        return 10000u;
    }

    return kCfrShortfallForceCatchupThresholdTicks;
}

inline bool ShouldCfrCatchUpToWallClock(uint32_t outputShortfallTicks, bool useScreenGrab, bool frameAvailable,
                                        bool hasLastFrame) {
    (void)useScreenGrab;
    if (outputShortfallTicks == 0) {
        return false;
    }

    if (outputShortfallTicks >= kCfrShortfallForceCatchupThresholdTicks) {
        return true;
    }

    if (outputShortfallTicks < kCfrShortfallCatchupThresholdTicks) {
        return false;
    }

    return frameAvailable || hasLastFrame;
}

inline bool CanDrainOutstandingWgcTicks(bool queuedWgcFrameAvailable, bool bufferedWgcFrameAvailable, bool hasLastFrame,
                                        bool mediaEngineCanRepeatLastFrame) {
    (void)queuedWgcFrameAvailable;
    (void)bufferedWgcFrameAvailable;
    (void)hasLastFrame;
    (void)mediaEngineCanRepeatLastFrame;
    // WGC stop drain is intentionally disabled. A held-frame drain hides live
    // scheduler debt by appending synthetic visual time after capture stop.
    return false;
}

inline bool CanDrainOutstandingCfrTicks(bool useScreenGrab, bool queuedFrameAvailable, bool bufferedFrameAvailable,
                                        bool hasLastFrame, bool mediaEngineCanRepeatLastFrame) {
    if (useScreenGrab) {
        return CanDrainOutstandingWgcTicks(queuedFrameAvailable, bufferedFrameAvailable, hasLastFrame,
                                           mediaEngineCanRepeatLastFrame);
    }

    // Inject CFR has no separate source-drain path after capture stops. If the
    // encoder was late, repeat the last captured frame to close only the already
    // scheduled CFR debt so audio does not lag behind content that jumped ahead.
    return queuedFrameAvailable || bufferedFrameAvailable || (hasLastFrame && mediaEngineCanRepeatLastFrame);
}

// Returns the maximum number of output ticks to emit in a single encoder loop
// iteration when catching up.  The value includes the main tick itself, so
// "2" means 1 main + 1 extra repeat.
inline uint32_t GetCfrCatchupTicksThisLoop(uint32_t outputShortfallTicks, bool encoderBottlenecked = false) {
    if (outputShortfallTicks >= kCfrShortfallForceCatchupThresholdTicks) {
        return std::min(outputShortfallTicks, 4u);
    }
    if (outputShortfallTicks >= kCfrShortfallCatchupThresholdTicks) {
        return 2u;
    }
    return 1u;
}

inline bool ShouldApplyWgcSelectionDelay(bool recordingOutputLive, uint32_t outputShortfallTicks,
                                         bool encoderBottlenecked, bool reserveAvailableAtTickStart,
                                         bool contentDelayActive = false) {
    (void)outputShortfallTicks;
    (void)encoderBottlenecked;
    (void)reserveAvailableAtTickStart;
    // A configured A/V content delay (audio_capture_latency_ms) intentionally biases WGC
    // source selection back by the loopback capture latency so video content aligns with
    // inherently-late loopback audio. It must apply continuously (independent of the transient
    // reserve) so the bounded delay buffer builds and holds; the "too new for slot" path keeps
    // newer frames buffered until they age into their slot. With no content delay configured,
    // live CFR keeps its lowest-latency near-live selection (no intentional one-frame delay).
    if (contentDelayActive) {
        return recordingOutputLive;
    }
    return false;
}

inline bool ShouldAllowWgcExtraCatchupTicks(bool encoderBottlenecked, size_t bufferedWgcFrames,
                                            double frameCreditAccumulator, uint32_t outputShortfallTicks) {
    if (encoderBottlenecked && outputShortfallTicks < kCfrShortfallForceCatchupThresholdTicks) {
        return false;
    }

    if (outputShortfallTicks >= kCfrShortfallCatchupThresholdTicks) {
        return true;
    }

    if (bufferedWgcFrames <= 1) {
        return false;
    }

    if (frameCreditAccumulator >= 1.0) {
        return true;
    }

    return outputShortfallTicks >= kCfrShortfallForceCatchupThresholdTicks;
}

inline bool IsWgcSourceHealthyForLiveCatchup(uint32_t outputFps, uint32_t recentDeliveredMin250Fps,
                                             uint32_t recentInputMin250Fps, uint32_t noFreshTickPermille,
                                             bool lowSourceMode) {
    if (lowSourceMode || outputFps == 0) {
        return false;
    }

    const uint32_t deliveredRecoveryFloor = outputFps > 2 ? (outputFps - 2) : outputFps;
    return recentDeliveredMin250Fps >= deliveredRecoveryFloor && recentInputMin250Fps >= outputFps &&
           noFreshTickPermille < kWgcLowSourceEmptyTickPermille;
}

inline bool IsWgcInputBelowTarget(uint32_t outputFps, uint32_t recentInputMin250Fps, uint32_t recentInputMin500Fps,
                                  uint32_t fpsMargin = kWgcRecoverySourceMarginFps) {
    if (outputFps == 0) {
        return false;
    }

    return recentInputMin250Fps + fpsMargin < outputFps || recentInputMin500Fps + fpsMargin < outputFps;
}

inline uint32_t GetWgcFrameIntervalUs(uint32_t outputFps) {
    return outputFps > 0 ? std::max<uint32_t>(1u, 1000000u / outputFps) : 0u;
}

inline bool IsWgcTrueSourceStarvedForRecovery(uint32_t outputFps, uint32_t recentInputMin250Fps,
                                              uint32_t recentInputMin500Fps, uint32_t noFreshTickPermille,
                                              uint32_t bufferedWgcFrames, bool capacityPressure) {
    if (!IsWgcInputBelowTarget(outputFps, recentInputMin250Fps, recentInputMin500Fps)) {
        return false;
    }

    if (capacityPressure && bufferedWgcFrames >= kWgcEncoderLimitedSourceBufferFloorFrames &&
        noFreshTickPermille < kWgcDeepUnderfeedEmptyTickPermille) {
        return false;
    }

    return true;
}

inline bool IsWgcSourceHealthyEnoughForEncoderLimitedSmoothness(uint32_t outputFps, uint32_t recentInputMin250Fps,
                                                                uint32_t recentInputMin500Fps,
                                                                uint32_t noFreshTickPermille,
                                                                uint32_t bufferedWgcFrames) {
    if (outputFps == 0 || (recentInputMin250Fps == 0 && recentInputMin500Fps == 0)) {
        return false;
    }

    if (noFreshTickPermille >= kWgcDeepUnderfeedEmptyTickPermille) {
        return false;
    }

    if (!IsWgcInputBelowTarget(outputFps, recentInputMin250Fps, recentInputMin500Fps)) {
        return true;
    }

    return bufferedWgcFrames >= kWgcEncoderLimitedSourceBufferFloorFrames;
}

inline bool IsWgcSourceHealthyEnoughToSuppressEncoderLimitedCatchup(uint32_t outputFps, uint32_t recentInputMin250Fps,
                                                                    uint32_t noFreshTickPermille, bool lowSourceMode) {
    (void)lowSourceMode;
    return IsWgcSourceHealthyEnoughForEncoderLimitedSmoothness(outputFps, recentInputMin250Fps, recentInputMin250Fps,
                                                               noFreshTickPermille, 0);
}

inline bool IsWgcSourceDegradedForLiveCatchup(uint32_t outputFps, uint32_t recentDeliveredMin250Fps,
                                              uint32_t recentInputMin250Fps, uint32_t noFreshTickPermille) {
    if (outputFps == 0) {
        return false;
    }

    if (noFreshTickPermille >= kWgcDeepUnderfeedEmptyTickPermille) {
        return true;
    }

    // Keep live catchup conservative when the rolling source rate is clearly
    // behind target, but avoid overreacting to small single-digit deficits.
    const uint32_t degradedMarginFps = std::max<uint32_t>(1u, outputFps / 10u);
    const uint32_t degradedFloor = outputFps > degradedMarginFps ? (outputFps - degradedMarginFps) : 0u;
    return recentDeliveredMin250Fps < degradedFloor || recentInputMin250Fps < degradedFloor;
}

inline bool IsWgcDeepUnderfeed(uint32_t outputFps, uint32_t recentDeliveredMin250Fps, uint32_t recentInputMin250Fps,
                               uint32_t emptyTickPermille) {
    if (outputFps == 0) {
        return false;
    }

    const uint32_t severeFloor = outputFps > kWgcDeepUnderfeedMarginFps ? (outputFps - kWgcDeepUnderfeedMarginFps) : 0u;
    return recentDeliveredMin250Fps < severeFloor || recentInputMin250Fps < severeFloor ||
           emptyTickPermille >= kWgcDeepUnderfeedEmptyTickPermille;
}

inline bool IsWgcSevereSourceStallForActiveDelay(uint32_t outputFps, uint32_t recentDeliveredMin250Fps,
                                                 uint32_t recentInputMin250Fps, uint32_t emptyTickPermille,
                                                 uint32_t bufferedFrames) {
    if (outputFps == 0) {
        return false;
    }
    if (emptyTickPermille >= kWgcDeepUnderfeedEmptyTickPermille) {
        return true;
    }
    const uint32_t severeFloor = outputFps > kWgcDeepUnderfeedMarginFps ? (outputFps - kWgcDeepUnderfeedMarginFps) : 0u;
    const bool inputSevere = recentInputMin250Fps > 0 && recentInputMin250Fps < severeFloor;
    const bool deliveredSevere = recentDeliveredMin250Fps > 0 && recentDeliveredMin250Fps < severeFloor;
    return bufferedFrames == 0 && (inputSevere || deliveredSevere);
}

inline bool IsWgcSyncDelayHoldSourceLimited(uint32_t outputFps, uint32_t recentDeliveredMin250Fps,
                                            uint32_t recentInputMin250Fps, uint32_t emptyTickPermille,
                                            bool sourceStarved, bool lowSourceMode, bool deepUnderfeed,
                                            bool activeDelaySourceRecovery = false) {
    if (sourceStarved || lowSourceMode || deepUnderfeed || activeDelaySourceRecovery) {
        return true;
    }
    if (outputFps == 0) {
        return false;
    }
    if (emptyTickPermille >= kWgcRecoveryEmptyTickPermille) {
        return true;
    }
    if (recentInputMin250Fps > 0 && recentInputMin250Fps < outputFps) {
        return true;
    }
    return recentDeliveredMin250Fps > 0 && recentDeliveredMin250Fps < outputFps;
}

inline bool ShouldPrioritizeWgcAudioLeadCatchup(double audioLeadExcessMs,
                                                double minAudioLeadMs = kWgcAudioLeadCatchupThresholdMs) {
    return audioLeadExcessMs >= minAudioLeadMs;
}

inline bool IsWgcSourceRecoveredEnoughForSmoothAudioLeadCatchup(uint32_t outputFps, uint32_t recentDeliveredMin250Fps,
                                                                uint32_t recentInputMin250Fps,
                                                                uint32_t noFreshTickPermille) {
    if (outputFps == 0) {
        return false;
    }

    const uint32_t recoveredFloor =
        outputFps > kWgcRecoverySourceMarginFps ? (outputFps - kWgcRecoverySourceMarginFps) : 0u;
    return recentDeliveredMin250Fps >= recoveredFloor && recentInputMin250Fps >= recoveredFloor &&
           noFreshTickPermille < kWgcRecoveryEmptyTickPermille;
}

inline uint32_t GetWgcFreshCatchupBudgetThisLoop(uint32_t catchupTicksThisLoop) {
    (void)catchupTicksThisLoop;
    // WGC CFR does not spend historical shortfall by encoding extra fresh
    // frames.  Extra debt is represented as visual holds/drops so audio remains
    // continuous and source content never fast-forwards.
    return 0u;
}

inline uint32_t GetWgcCatchupTicksThisLoop(bool encoderBottlenecked, bool encoderActivelyTooSlow,
                                           size_t bufferedWgcFrames, double frameCreditAccumulator,
                                           uint32_t outputShortfallTicks, uint32_t outputFps,
                                           uint32_t recentDeliveredMin250Fps, uint32_t recentInputMin250Fps,
                                           uint32_t noFreshTickPermille, bool lowSourceMode,
                                           double audioLeadExcessMs = 0.0) {
    (void)encoderBottlenecked;
    (void)encoderActivelyTooSlow;
    (void)bufferedWgcFrames;
    (void)frameCreditAccumulator;
    (void)outputShortfallTicks;
    (void)outputFps;
    (void)recentDeliveredMin250Fps;
    (void)recentInputMin250Fps;
    (void)noFreshTickPermille;
    (void)lowSourceMode;
    (void)audioLeadExcessMs;
    // WGC output emits one CFR tick per encoder-loop decision.  Backlog is not
    // drained by burst-encoding old slots, because that creates visible speed
    // changes and encourages audio trimming.  The smoother truthful result is
    // one best-frame/hold/drop decision per tick.
    return 1u;
}

inline double GetCfrShortfallDurationMs(uint32_t outputShortfallTicks, double frameIntervalMs) {
    if (outputShortfallTicks == 0 || frameIntervalMs <= 0.0) {
        return 0.0;
    }

    return static_cast<double>(outputShortfallTicks) * frameIntervalMs;
}

inline bool HasWgcSevereLiveShortfall(double shortfallDurationMs,
                                      double severeShortfallDurationMs = kWgcSevereShortfallDurationMs) {
    return shortfallDurationMs >= std::max(0.0, severeShortfallDurationMs);
}

inline bool ShouldClampWgcCoverageCatchupToSingleTick(bool coverageRepeatActive, bool encoderTooSlowForTarget,
                                                      double shortfallDurationMs) {
    // Never clamp CFR catchup ticks. Sync depends on catching up.
    return false;
}

inline double GetWgcForceCatchupBudgetFrameMultiplier(double shortfallDurationMs) {
    return HasWgcSevereLiveShortfall(shortfallDurationMs) ? 4.0 : 4.0;
}

inline bool HasWgcUnrecoverableCoverageLoss(double shortfallDurationMs, double oldestBufferedFrameAgeMs,
                                            double audioLeadExcessMs = -1.0, double minPipelineLagMs = 250.0,
                                            double minLagMismatchMs = 120.0, double minAudioLeadMs = 40.0) {
    if (shortfallDurationMs < minPipelineLagMs) {
        return false;
    }

    const double clampedBufferedAgeMs = std::max(0.0, oldestBufferedFrameAgeMs);
    const double mismatchMs = shortfallDurationMs - clampedBufferedAgeMs;
    if (mismatchMs <= minLagMismatchMs) {
        return false;
    }

    if (audioLeadExcessMs >= 0.0 && audioLeadExcessMs < minAudioLeadMs) {
        return false;
    }

    return true;
}

inline bool ShouldSuppressWgcCoverageLossForEncoderBottleneck(bool encoderBottlenecked, uint32_t deliveredFps,
                                                              uint32_t targetFps, uint32_t deliveryMarginFps = 2) {
    if (!encoderBottlenecked || deliveredFps == 0 || targetFps == 0) {
        return false;
    }

    const uint32_t healthyDeliveryFloor = targetFps > deliveryMarginFps ? (targetFps - deliveryMarginFps) : targetFps;
    return deliveredFps >= healthyDeliveryFloor;
}

inline double ComputeWgcCoverageLossRepeatRatio(double shortfallDurationMs, double oldestBufferedFrameAgeMs,
                                                double audioLeadExcessMs = -1.0, double fullRepeatMismatchMs = 1500.0,
                                                double minLagMismatchMs = 120.0) {
    if (fullRepeatMismatchMs <= 0.0) {
        return 0.0;
    }

    double mismatchMs = std::max(0.0, shortfallDurationMs - std::max(0.0, oldestBufferedFrameAgeMs));
    if (audioLeadExcessMs >= 0.0) {
        mismatchMs = std::min(mismatchMs, std::max(0.0, audioLeadExcessMs));
    }
    const double effectiveMismatchMs = std::max(0.0, mismatchMs - std::max(0.0, minLagMismatchMs));
    if (effectiveMismatchMs <= 0.0) {
        return 0.0;
    }

    return std::clamp(effectiveMismatchMs / fullRepeatMismatchMs, 0.0, 0.35);
}

inline uint32_t GetWgcCoverageDelayTicks(uint32_t outputShortfallTicks, double oldestBufferedFrameAgeMs,
                                         double frameIntervalMs, uint32_t maxDelayTicks = kWgcCoverageDelayMaxTicks) {
    if (outputShortfallTicks == 0 || frameIntervalMs <= 0.0 || maxDelayTicks == 0) {
        return 0;
    }

    const double shortfallDurationMs = GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs);
    if (!HasWgcUnrecoverableCoverageLoss(shortfallDurationMs, oldestBufferedFrameAgeMs)) {
        return 0;
    }

    const uint32_t bufferedAgeTicks = static_cast<uint32_t>(
        std::clamp(oldestBufferedFrameAgeMs / frameIntervalMs, 0.0, static_cast<double>(outputShortfallTicks)));
    const uint32_t desiredDelayTicks =
        outputShortfallTicks > bufferedAgeTicks ? (outputShortfallTicks - bufferedAgeTicks) : 0u;
    return std::min(desiredDelayTicks, maxDelayTicks);
}

inline double GetEncoderSustainableOutputFps(double encodeMs) {
    if (encodeMs <= 0.0) {
        return 0.0;
    }

    return 1000.0 / encodeMs;
}

inline uint32_t GetInjectCfrCatchupTicksThisLoop(uint32_t outputShortfallTicks, bool encoderBottlenecked = false) {
    if (outputShortfallTicks < kCfrShortfallForceCatchupThresholdTicks || encoderBottlenecked) {
        return 1u;
    }

    return 2u;
}

inline bool ShouldUseFreshInjectCatchup(bool useVFR, bool encoderBottlenecked, bool encoderActivelyTooSlow,
                                        size_t bufferedInjectFrames, size_t minBufferedInjectFrames,
                                        double frameCreditAccumulator, uint32_t outputShortfallTicks) {
    if (useVFR || encoderBottlenecked || encoderActivelyTooSlow) {
        return false;
    }

    if (outputShortfallTicks < kCfrShortfallForceCatchupThresholdTicks) {
        return false;
    }

    if (bufferedInjectFrames <= minBufferedInjectFrames || frameCreditAccumulator < 1.0) {
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

inline uint32_t GetWgcCfrOvercaptureTargetFps(uint32_t outputFps,
                                              uint32_t headroomPermille = kWgcCfrOvercaptureHeadroomPermille) {
    if (headroomPermille == 0u) {
        return 0u;
    }

    if (outputFps == 0 || headroomPermille <= 1000u) {
        return outputFps;
    }

    return static_cast<uint32_t>((static_cast<uint64_t>(outputFps) * headroomPermille + 999ull) / 1000ull);
}

inline bool ShouldRaiseAdaptiveEncoderGpuPriority(double encodeMs, double frameIntervalMs,
                                                  double raiseBudgetRatio = kEncoderGpuPriorityRaiseBudgetRatio) {
    return encodeMs > 0.0 && frameIntervalMs > 0.0 && encodeMs >= frameIntervalMs * raiseBudgetRatio;
}

inline bool ShouldRestoreNeutralEncoderGpuPriority(double encodeMs, double frameIntervalMs,
                                                   double restoreBudgetRatio = kEncoderGpuPriorityRestoreBudgetRatio) {
    return frameIntervalMs > 0.0 && encodeMs >= 0.0 && encodeMs <= frameIntervalMs * restoreBudgetRatio;
}

inline bool IsAdaptiveEncoderGpuPriorityPressureActive(double encodeMs, double frameIntervalMs,
                                                       bool encoderPressureActive) {
    return encoderPressureActive || ShouldRaiseAdaptiveEncoderGpuPriority(encodeMs, frameIntervalMs);
}

inline bool ShouldResetAdaptiveEncoderGpuPriorityPressure(double encodeMs, double frameIntervalMs,
                                                          bool encoderPressureActive = false) {
    return !encoderPressureActive && ShouldRestoreNeutralEncoderGpuPriority(encodeMs, frameIntervalMs);
}

inline bool IsWgcCaptureLimitedForOverlay(uint32_t captureHealthFlags) {
    const uint32_t captureLimitedFlags = kWgcCaptureHealthFlagSourceStarved | kWgcCaptureHealthFlagSchedulerLimited;
    return (captureHealthFlags & captureLimitedFlags) != 0;
}

inline uint32_t SelectWgcOverlayWarningKind(uint32_t overloadFlags, uint32_t captureHealthFlags) {
    if (IsWgcCaptureLimitedForOverlay(captureHealthFlags)) {
        return kOverlayWarningNone;
    }

    if ((overloadFlags & kEncoderOverloadFlagEncoder) != 0) {
        return kOverlayWarningEncoderOverload;
    }

    return kOverlayWarningNone;
}

inline uint32_t GetEncoderBudgetUtilizationPermille(double encodeMs, double frameIntervalMs) {
    if (encodeMs <= 0.0 || frameIntervalMs <= 0.0) {
        return 0u;
    }

    const double utilizationPermille = (encodeMs * 1000.0) / frameIntervalMs;
    const double clampedPermille = std::clamp(utilizationPermille, 0.0, 1000000.0);
    return static_cast<uint32_t>(clampedPermille + 0.5);
}

inline bool IsEncoderTooSlowForTargetFps(double encodeMs, double frameIntervalMs, uint32_t targetFps,
                                         double toleranceFps = 0.5) {
    if (frameIntervalMs <= 0.0 || targetFps == 0u) {
        return false;
    }

    const double sustainableFps = GetEncoderSustainableOutputFps(encodeMs);
    return sustainableFps > 0.0 && sustainableFps + toleranceFps < static_cast<double>(targetFps);
}

struct WgcAdaptiveTelemetry {
    uint32_t outputFps = 0;
    uint32_t recentDeliveredFps = 0;
    uint32_t recentDeliveredMin250Fps = 0;
    uint32_t recentDeliveredMin500Fps = 0;
    uint32_t recentInputMin250Fps = 0;
    uint32_t recentInputMin500Fps = 0;
    uint32_t averageJitterUs = 0;
    uint32_t emptyTickPermille = 0;
    uint32_t bufferedWgcFrames = 0;
    uint32_t encoderQueueDepth = 0;
    double duplicateRatio = 0.0;
};

inline bool IsWgcActiveDelayRecoverableJitter(const WgcAdaptiveTelemetry& telemetry) {
    const uint32_t frameIntervalUs = GetWgcFrameIntervalUs(telemetry.outputFps);
    if (frameIntervalUs == 0 || telemetry.averageJitterUs == 0) {
        return false;
    }

    return static_cast<uint64_t>(telemetry.averageJitterUs) * 1000ull >=
           static_cast<uint64_t>(frameIntervalUs) * kWgcActiveDelayRecoverableJitterPermille;
}

inline bool IsWgcActiveDelaySourceLimitedJitter(const WgcAdaptiveTelemetry& telemetry) {
    const uint32_t frameIntervalUs = GetWgcFrameIntervalUs(telemetry.outputFps);
    if (frameIntervalUs == 0 || telemetry.averageJitterUs == 0) {
        return false;
    }

    if (static_cast<uint64_t>(telemetry.averageJitterUs) * 1000ull <
        static_cast<uint64_t>(frameIntervalUs) * kWgcActiveDelaySourceLimitedJitterPermille) {
        return false;
    }

    return telemetry.emptyTickPermille >= kWgcLowSourceExitEmptyTickPermille ||
           telemetry.recentDeliveredMin250Fps < telemetry.outputFps ||
           telemetry.recentInputMin250Fps < telemetry.outputFps;
}

inline bool ShouldUseWgcMaxRateForRecovery(const WgcAdaptiveTelemetry& telemetry, uint32_t noFreshTickPermille,
                                           bool lowSourceModeActive, bool liveRecoveryModeActive) {
    if (telemetry.outputFps == 0) {
        return false;
    }

    if (lowSourceModeActive || liveRecoveryModeActive) {
        return true;
    }

    if (noFreshTickPermille >= kWgcLowSourceEmptyTickPermille) {
        return true;
    }

    return telemetry.recentInputMin250Fps > 0 && telemetry.recentInputMin250Fps < telemetry.outputFps;
}

inline bool ShouldUseWgcMaxRateForDelayReservoirRecovery(const WgcAdaptiveTelemetry& telemetry,
                                                         bool delayReservoirBelowLowWater,
                                                         bool lowSourceModeActive,
                                                         bool liveRecoveryModeActive) {
    if (!delayReservoirBelowLowWater || telemetry.outputFps == 0) {
        return false;
    }
    if (lowSourceModeActive || liveRecoveryModeActive) {
        return false;
    }
    if (telemetry.emptyTickPermille >= kWgcDeepUnderfeedEmptyTickPermille) {
        return false;
    }
    if (telemetry.recentInputMin250Fps > 0 &&
        telemetry.recentInputMin250Fps + kWgcRecoverySourceMarginFps < telemetry.outputFps) {
        return false;
    }
    if (telemetry.recentDeliveredMin250Fps > 0 &&
        telemetry.recentDeliveredMin250Fps + kWgcRecoverySourceMarginFps < telemetry.outputFps &&
        telemetry.emptyTickPermille >= kWgcLowSourceExitEmptyTickPermille) {
        return false;
    }
    return true;
}

inline bool ShouldRestoreWgcOvercaptureCap(const WgcAdaptiveTelemetry& telemetry, uint32_t noFreshTickPermille,
                                           uint64_t stableDurationMs,
                                           uint64_t requiredStableMs = kWgcCfrOvercaptureStableRestoreMs) {
    if (telemetry.outputFps == 0 || stableDurationMs < requiredStableMs) {
        return false;
    }

    return telemetry.recentDeliveredMin250Fps >= telemetry.outputFps &&
           telemetry.recentDeliveredMin500Fps >= telemetry.outputFps &&
           telemetry.recentInputMin250Fps >= telemetry.outputFps &&
           telemetry.recentInputMin500Fps >= telemetry.outputFps &&
           noFreshTickPermille <= kWgcLowSourceExitEmptyTickPermille;
}

enum class HeldModeTransition : uint8_t {
    kNone = 0,
    kEntered,
    kExited,
};

struct HeldModeUpdate {
    bool active = false;
    uint64_t stateChangeTick = 0;
    HeldModeTransition transition = HeldModeTransition::kNone;
    bool immediate = false;
};

inline HeldModeUpdate UpdateHeldMode(bool active, uint64_t stateChangeTick, uint64_t nowTick, bool shouldEnter,
                                     bool shouldExit, bool shouldExitImmediately, uint32_t enterHoldMs,
                                     uint32_t exitHoldMs) {
    HeldModeUpdate update;
    update.active = active;

    if (!active) {
        if (!shouldEnter) {
            return update;
        }

        if (stateChangeTick == 0 || nowTick < stateChangeTick) {
            if (enterHoldMs == 0) {
                update.active = true;
                update.transition = HeldModeTransition::kEntered;
                return update;
            }

            update.stateChangeTick = nowTick;
            return update;
        }

        if ((nowTick - stateChangeTick) >= enterHoldMs) {
            update.active = true;
            update.transition = HeldModeTransition::kEntered;
            return update;
        }

        update.stateChangeTick = stateChangeTick;
        return update;
    }

    if (shouldExitImmediately) {
        update.active = false;
        update.transition = HeldModeTransition::kExited;
        update.immediate = true;
        return update;
    }

    if (!shouldExit) {
        return update;
    }

    if (stateChangeTick == 0 || nowTick < stateChangeTick) {
        if (exitHoldMs == 0) {
            update.active = false;
            update.transition = HeldModeTransition::kExited;
            return update;
        }

        update.stateChangeTick = nowTick;
        return update;
    }

    if ((nowTick - stateChangeTick) >= exitHoldMs) {
        update.active = false;
        update.transition = HeldModeTransition::kExited;
        return update;
    }

    update.stateChangeTick = stateChangeTick;
    return update;
}

enum class WgcLowSourceState : uint8_t {
    kHealthy = 0,
    kInputBelowTarget,
    kDeliveryBelowTarget,
    kQueueEmptyPressure,
};

inline const char* WgcLowSourceStateToString(WgcLowSourceState state) {
    switch (state) {
        case WgcLowSourceState::kHealthy:
            return "healthy";
        case WgcLowSourceState::kInputBelowTarget:
            return "input-below-target";
        case WgcLowSourceState::kDeliveryBelowTarget:
            return "delivery-below-target";
        case WgcLowSourceState::kQueueEmptyPressure:
            return "queue-empty-pressure";
    }
    return "unknown";
}

enum class WgcLiveRecoveryState : uint8_t {
    kHealthy = 0,
    kSourceStarved,
    kSchedulerLimited,
    kEncoderLimited,
};

enum class WgcActiveDelayWindowClass : uint8_t {
    kHealthy = 0,
    kRecoverableUnderfill,
    kSourceLimited,
};

inline const char* WgcActiveDelayWindowClassToString(WgcActiveDelayWindowClass state) {
    switch (state) {
        case WgcActiveDelayWindowClass::kHealthy:
            return "healthy";
        case WgcActiveDelayWindowClass::kRecoverableUnderfill:
            return "recoverable_underfill";
        case WgcActiveDelayWindowClass::kSourceLimited:
            return "source_limited";
    }
    return "unknown";
}

inline const char* WgcLiveRecoveryStateToString(WgcLiveRecoveryState state) {
    switch (state) {
        case WgcLiveRecoveryState::kHealthy:
            return "healthy";
        case WgcLiveRecoveryState::kSourceStarved:
            return "source-starved";
        case WgcLiveRecoveryState::kSchedulerLimited:
            return "scheduler-limited";
        case WgcLiveRecoveryState::kEncoderLimited:
            return "encoder-limited";
    }
    return "unknown";
}

struct WarmupTransitionState {
    bool warmupWasScreenGrab = false;
    uint64_t startupWarmupStartTick = 0;
    uint32_t hiddenStartupFrames = 0;
};

inline bool ShouldCommitRecordingWarmup(bool useScreenGrab, bool useVFR, bool poppedFrame, bool hasBufferedWgcFrame,
                                        size_t bufferedInjectFrames, size_t injectReserveFrames,
                                        uint32_t warmupElapsedMs) {
    if (!poppedFrame) {
        return false;
    }

    if (warmupElapsedMs >= kRecordingWarmupMaxMs) {
        return true;
    }

    if (warmupElapsedMs < kRecordingWarmupMinMs) {
        return false;
    }

    if (useVFR) {
        return true;
    }

    if (useScreenGrab) {
        return hasBufferedWgcFrame;
    }

    const size_t minInjectFrames = std::max(injectReserveFrames, kInjectWarmupCommitFloorFrames);
    return bufferedInjectFrames >= minInjectFrames;
}

inline size_t GetInjectReserveFrames(bool useVFR, double smoothedInjectFenceMs, double frameIntervalMs) {
    if (useVFR || frameIntervalMs <= 0.0) {
        return 0;
    }

    const double reserveFramesNeeded = smoothedInjectFenceMs / frameIntervalMs;
    size_t reserveFrames = 1;
    if (reserveFramesNeeded > 0.5) {
        reserveFrames = 2;
    }
    if (reserveFramesNeeded > 1.25) {
        reserveFrames = 3;
    }
    if (reserveFramesNeeded > 2.25) {
        reserveFrames = 4;
    }
    return reserveFrames;
}

inline size_t GetWarmupInjectKeepCount(double smoothedInjectFenceMs, double frameIntervalMs) {
    return std::max(GetInjectReserveFrames(false, smoothedInjectFenceMs, frameIntervalMs) + 1,
                    kInjectWarmupCommitFloorFrames);
}

inline size_t GetMinBufferedInjectFrames(size_t injectReserveFrames, bool recordingOutputLive) {
    if (recordingOutputLive && injectReserveFrames > 0) {
        return std::max<size_t>(1, injectReserveFrames - 1);
    }
    return injectReserveFrames;
}

inline bool IsEncoderStartupWindow(bool recordingOutputLive, uint64_t recordingLiveTick, uint64_t nowTick) {
    if (!recordingOutputLive || nowTick < recordingLiveTick) {
        return true;
    }
    return (nowTick - recordingLiveTick) < kEncoderStartupWindowMs;
}

inline bool IsInjectEncoderStartup(bool recordingOutputLive, uint64_t recordingLiveTick, uint64_t nowTick) {
    return IsEncoderStartupWindow(recordingOutputLive, recordingLiveTick, nowTick);
}

inline size_t GetInjectBufferedHeadroom(bool recordingOutputLive, uint64_t recordingLiveTick, uint64_t nowTick) {
    return IsEncoderStartupWindow(recordingOutputLive, recordingLiveTick, nowTick)
               ? kStartupInjectBufferedHeadroomFrames
               : kMaxInjectBufferedHeadroomFrames;
}

inline size_t GetMaxBufferedInjectFrames(size_t injectReserveFrames, bool recordingOutputLive,
                                         uint64_t recordingLiveTick, uint64_t nowTick) {
    return injectReserveFrames + GetInjectBufferedHeadroom(recordingOutputLive, recordingLiveTick, nowTick);
}

inline int64_t GetInjectLiveMaxFrameAgeQpc(bool recordingOutputLive, bool encoderBottlenecked,
                                           bool encoderActivelyTooSlow, int64_t targetIntervalTicks) {
    if (!recordingOutputLive || targetIntervalTicks <= 0) {
        return 0;
    }

    const uint32_t maxAgeTicks = (encoderBottlenecked || encoderActivelyTooSlow) ? kInjectLivePressureMaxFrameAgeTicks
                                                                                 : kInjectLiveHealthyMaxFrameAgeTicks;
    return targetIntervalTicks * static_cast<int64_t>(maxAgeTicks);
}

inline bool ShouldTrimStaleInjectLiveFrame(int64_t frameTimestampQpc, int64_t liveNowQpc, int64_t maxFrameAgeQpc,
                                           size_t bufferedInjectFrames, size_t minBufferedInjectFrames) {
    if (frameTimestampQpc <= 0 || liveNowQpc <= frameTimestampQpc || maxFrameAgeQpc <= 0) {
        return false;
    }

    if (bufferedInjectFrames <= minBufferedInjectFrames + 1) {
        return false;
    }

    return (liveNowQpc - frameTimestampQpc) > maxFrameAgeQpc;
}

inline bool ResetWarmupOnCaptureModeChange(bool recordingOutputLive, bool useScreenGrab, uint64_t nowTick,
                                           WarmupTransitionState& state) {
    if (recordingOutputLive || useScreenGrab == state.warmupWasScreenGrab) {
        return false;
    }

    state.warmupWasScreenGrab = useScreenGrab;
    state.startupWarmupStartTick = nowTick;
    state.hiddenStartupFrames = 0;
    return true;
}

inline uint32_t GetAutoWgcFallbackDelayMs(uint32_t activeSourcePid) {
    return activeSourcePid == 0 ? kAutoWgcFallbackDelayNoPidMs : kAutoWgcFallbackDelayWithPidMs;
}

inline bool ShouldTriggerAutoWgcFallback(bool receivedFirstFrame, bool autoCaptureMode, bool autoFallbackArmed,
                                         bool hasWgcCapture, uint32_t elapsedMs, uint32_t activeSourcePid) {
    if (receivedFirstFrame || !autoCaptureMode || !autoFallbackArmed || !hasWgcCapture) {
        return false;
    }

    return elapsedMs > GetAutoWgcFallbackDelayMs(activeSourcePid);
}

inline WgcLowSourceState ClassifyWgcLowSourceState(const WgcAdaptiveTelemetry& telemetry) {
    if (telemetry.outputFps == 0) {
        return WgcLowSourceState::kHealthy;
    }

    const uint32_t enterMarginFps = kWgcLowSourceEnterMarginFps;
    const uint32_t inputEnterFloor = telemetry.outputFps > enterMarginFps ? (telemetry.outputFps - enterMarginFps) : 0u;

    if (telemetry.recentInputMin500Fps < telemetry.outputFps || telemetry.recentInputMin250Fps < inputEnterFloor) {
        return WgcLowSourceState::kInputBelowTarget;
    }

    if (telemetry.recentDeliveredFps < inputEnterFloor || telemetry.recentDeliveredMin250Fps < inputEnterFloor ||
        telemetry.recentDeliveredMin500Fps < telemetry.outputFps) {
        return WgcLowSourceState::kDeliveryBelowTarget;
    }

    if (telemetry.emptyTickPermille >= kWgcLowSourceEmptyTickPermille) {
        return WgcLowSourceState::kQueueEmptyPressure;
    }

    return WgcLowSourceState::kHealthy;
}

inline bool ShouldEnterWgcLowSourceMode(const WgcAdaptiveTelemetry& telemetry) {
    return ClassifyWgcLowSourceState(telemetry) != WgcLowSourceState::kHealthy;
}

inline bool IsWgcSourceStarved(const WgcAdaptiveTelemetry& telemetry,
                               uint32_t fpsMargin = kWgcRecoverySourceMarginFps) {
    if (telemetry.outputFps == 0) {
        return false;
    }

    return IsWgcInputBelowTarget(telemetry.outputFps, telemetry.recentInputMin250Fps, telemetry.recentInputMin500Fps,
                                 fpsMargin);
}

inline bool IsWgcSchedulerDeliveryLimited(const WgcAdaptiveTelemetry& telemetry,
                                          uint32_t fpsMargin = kWgcRecoverySourceMarginFps,
                                          uint32_t emptyTickThresholdPermille = kWgcRecoveryEmptyTickPermille) {
    if (telemetry.outputFps == 0 || IsWgcSourceStarved(telemetry, fpsMargin)) {
        return false;
    }

    return telemetry.recentDeliveredMin250Fps + fpsMargin < telemetry.outputFps ||
           telemetry.recentDeliveredMin500Fps + fpsMargin < telemetry.outputFps ||
           telemetry.emptyTickPermille >= emptyTickThresholdPermille;
}

inline WgcLiveRecoveryState ClassifyWgcLiveRecoveryState(const WgcAdaptiveTelemetry& telemetry,
                                                         uint32_t outputShortfallTicks, bool encoderBottlenecked) {
    if (telemetry.outputFps == 0 || outputShortfallTicks == 0) {
        return WgcLiveRecoveryState::kHealthy;
    }

    if (IsWgcTrueSourceStarvedForRecovery(telemetry.outputFps, telemetry.recentInputMin250Fps,
                                          telemetry.recentInputMin500Fps, telemetry.emptyTickPermille,
                                          telemetry.bufferedWgcFrames, encoderBottlenecked)) {
        return WgcLiveRecoveryState::kSourceStarved;
    }

    if (encoderBottlenecked && outputShortfallTicks >= kWgcRecoveryEnterShortfallTicks &&
        IsWgcSourceHealthyEnoughForEncoderLimitedSmoothness(telemetry.outputFps, telemetry.recentInputMin250Fps,
                                                            telemetry.recentInputMin500Fps, telemetry.emptyTickPermille,
                                                            telemetry.bufferedWgcFrames)) {
        return WgcLiveRecoveryState::kEncoderLimited;
    }

    if (IsWgcSchedulerDeliveryLimited(telemetry)) {
        return WgcLiveRecoveryState::kSchedulerLimited;
    }

    if (encoderBottlenecked && outputShortfallTicks >= kWgcRecoveryEnterShortfallTicks) {
        return WgcLiveRecoveryState::kEncoderLimited;
    }

    return WgcLiveRecoveryState::kHealthy;
}

inline WgcActiveDelayWindowClass ClassifyWgcActiveDelayWindow(const WgcAdaptiveTelemetry& telemetry,
                                                             bool lowSourceMode, bool liveRecoveryMode,
                                                             bool sourceStarved, bool deepUnderfeed,
                                                             bool activeDelaySourceRecovery,
                                                             bool hardSafeCandidateAvailable) {
    if (telemetry.outputFps == 0) {
        return WgcActiveDelayWindowClass::kHealthy;
    }
    if (!hardSafeCandidateAvailable || sourceStarved || deepUnderfeed || activeDelaySourceRecovery) {
        return WgcActiveDelayWindowClass::kSourceLimited;
    }
    if (IsWgcInputBelowTarget(telemetry.outputFps, telemetry.recentInputMin250Fps,
                              telemetry.recentInputMin500Fps) ||
        telemetry.emptyTickPermille >= kWgcDeepUnderfeedEmptyTickPermille ||
        IsWgcActiveDelaySourceLimitedJitter(telemetry)) {
        return WgcActiveDelayWindowClass::kSourceLimited;
    }
    if (lowSourceMode || liveRecoveryMode || IsWgcSchedulerDeliveryLimited(telemetry) ||
        telemetry.emptyTickPermille >= kWgcLowSourceEmptyTickPermille ||
        IsWgcActiveDelayRecoverableJitter(telemetry)) {
        return WgcActiveDelayWindowClass::kRecoverableUnderfill;
    }
    return WgcActiveDelayWindowClass::kHealthy;
}

inline bool ShouldEnterWgcLiveRecoveryMode(const WgcAdaptiveTelemetry& telemetry, uint32_t outputShortfallTicks,
                                           bool encoderBottlenecked) {
    return ClassifyWgcLiveRecoveryState(telemetry, outputShortfallTicks, encoderBottlenecked) !=
           WgcLiveRecoveryState::kHealthy;
}

inline bool ShouldExitWgcLiveRecoveryMode(const WgcAdaptiveTelemetry& telemetry, uint32_t outputShortfallTicks,
                                          bool encoderBottlenecked) {
    if (telemetry.outputFps == 0) {
        return true;
    }

    if (encoderBottlenecked || outputShortfallTicks > 1) {
        return false;
    }

    return !ShouldEnterWgcLowSourceMode(telemetry) && telemetry.bufferedWgcFrames <= 4;
}

inline bool ShouldExitWgcLowSourceMode(const WgcAdaptiveTelemetry& telemetry, bool encoderTooSlowForTarget = false,
                                       bool bufferedReserveRecovered = false) {
    if (telemetry.outputFps == 0) {
        return true;
    }

    if (!encoderTooSlowForTarget && bufferedReserveRecovered) {
        return true;
    }

    const uint32_t recoveredInputThresholdFps = telemetry.outputFps + 2u;
    return telemetry.recentDeliveredFps >= telemetry.outputFps &&
           telemetry.recentDeliveredMin250Fps >= telemetry.outputFps &&
           telemetry.recentDeliveredMin500Fps >= telemetry.outputFps &&
           telemetry.recentInputMin250Fps >= recoveredInputThresholdFps &&
           telemetry.recentInputMin500Fps >= recoveredInputThresholdFps &&
           telemetry.emptyTickPermille <= kWgcLowSourceExitEmptyTickPermille && telemetry.bufferedWgcFrames <= 4;
}

inline bool ShouldAllowWgcAdaptiveHeadroom(const WgcAdaptiveTelemetry& telemetry, uint32_t noFreshTickPermille,
                                           bool lowSourceModeActive, bool liveRecoveryModeActive,
                                           double maxDuplicateRatio = 0.18) {
    if (telemetry.outputFps == 0 || lowSourceModeActive || liveRecoveryModeActive) {
        return false;
    }

    if (ShouldEnterWgcLowSourceMode(telemetry) || noFreshTickPermille >= kWgcReservePressurePermille) {
        return false;
    }

    return telemetry.duplicateRatio <= maxDuplicateRatio;
}

inline bool ShouldUseWgcLowSourceMode(const WgcAdaptiveTelemetry& telemetry) {
    return ClassifyWgcLowSourceState(telemetry) != WgcLowSourceState::kHealthy;
}

inline bool IsWgcReservePressureActive(uint32_t noReserveTickCount, uint32_t queueTickSampleCount, uint32_t outputFps) {
    const uint32_t minSamples = std::max<uint32_t>(outputFps / 4u, 8u);
    if (queueTickSampleCount < minSamples || queueTickSampleCount == 0) {
        return false;
    }

    return static_cast<uint64_t>(noReserveTickCount) * 1000ull >=
           static_cast<uint64_t>(queueTickSampleCount) * static_cast<uint64_t>(kWgcReservePressurePermille);
}

inline bool ShouldCommitWgcWarmup(bool poppedFrame, size_t bufferedWgcFrames, uint32_t warmupElapsedMs,
                                  double measuredInputFps, uint32_t outputFps) {
    if (!poppedFrame) {
        return false;
    }

    if (warmupElapsedMs >= kRecordingWarmupMaxMs) {
        return true;
    }

    if (warmupElapsedMs < kRecordingWarmupMinMs) {
        return false;
    }

    const double stableSourceFps = std::max<double>(kWgcWarmupStableSourceFps, static_cast<double>(outputFps) - 2.0);
    return bufferedWgcFrames >= kWgcWarmupBufferedFrames && measuredInputFps >= stableSourceFps;
}

inline int64_t GetWgcMaxSelectionLagQpc(int64_t targetIntervalTicks, bool lowSourceMode) {
    if (targetIntervalTicks <= 0) {
        return 0;
    }

    const int64_t maxLagTicks = lowSourceMode ? static_cast<int64_t>(kWgcLowSourceMaxSelectionLagTicks)
                                              : static_cast<int64_t>(kWgcMaxSelectionLagTicks);
    return targetIntervalTicks * maxLagTicks;
}

// Selects which buffered source-frame content time a CFR tick should target. This is
// intentionally separate from the PTS schedule (scheduledSampleQpc): biasing the selection
// target backwards picks slightly older video content without changing the output PTS grid,
// so track length, start/end, and cadence are unaffected. `extraSelectionDelayQpc` is the
// configured A/V content delay in QPC ticks (audio_capture_latency_ms): it delays the video
// content to match inherently-late loopback audio, leaving audio byte-exact. It is applied
// only for live recording and clamped so the target stays positive.
inline int64_t GetWgcSelectionTargetQpc(int64_t scheduledSampleQpc, int64_t fallbackTargetQpc,
                                        int64_t targetIntervalTicks, bool recordingOutputLive,
                                        int64_t extraSelectionDelayQpc = 0) {
    int64_t selectionTargetQpc = scheduledSampleQpc > 0 ? scheduledSampleQpc : fallbackTargetQpc;
    if (!recordingOutputLive || selectionTargetQpc <= 0 || targetIntervalTicks <= 0) {
        return selectionTargetQpc;
    }
    if (extraSelectionDelayQpc < 0) {
        extraSelectionDelayQpc = 0;
    }
    // When a configured A/V content delay is present it IS the selection lag (it equals the
    // measured loopback capture latency); the legacy one-tick delay only applies on its own
    // when no content delay is configured. This keeps the video-content delay exactly L.
    const int64_t totalDelayQpc = extraSelectionDelayQpc > 0
                                      ? extraSelectionDelayQpc
                                      : (targetIntervalTicks * static_cast<int64_t>(kWgcSelectionDelayTicks));
    if (totalDelayQpc <= 0) {
        return selectionTargetQpc;
    }
    const int64_t delayedSelectionTargetQpc = selectionTargetQpc - totalDelayQpc;
    return delayedSelectionTargetQpc > 0 ? delayedSelectionTargetQpc : selectionTargetQpc;
}

inline int64_t GetWgcLiveVisualDebtLimitQpc(int64_t targetIntervalTicks, int64_t qpcTicksPerSecond,
                                            uint32_t maxDebtMs = kWgcMaxLiveVisualDebtMs,
                                            uint32_t maxDebtFrames = kWgcMaxLiveVisualDebtFrames) {
    if (targetIntervalTicks <= 0) {
        return 0;
    }

    const int64_t frameLimitQpc = targetIntervalTicks * static_cast<int64_t>(std::max<uint32_t>(1u, maxDebtFrames));
    if (qpcTicksPerSecond <= 0 || maxDebtMs == 0) {
        return frameLimitQpc;
    }

    const int64_t timeLimitQpc = (qpcTicksPerSecond * static_cast<int64_t>(std::max<uint32_t>(1u, maxDebtMs))) / 1000;
    return std::max<int64_t>(1, std::min(frameLimitQpc, timeLimitQpc));
}

inline uint32_t GetWgcLiveVisualDebtLimitTicks(int64_t targetIntervalTicks, int64_t qpcTicksPerSecond,
                                               uint32_t maxDebtMs = kWgcMaxLiveVisualDebtMs,
                                               uint32_t maxDebtFrames = kWgcMaxLiveVisualDebtFrames) {
    const int64_t debtLimitQpc =
        GetWgcLiveVisualDebtLimitQpc(targetIntervalTicks, qpcTicksPerSecond, maxDebtMs, maxDebtFrames);
    if (targetIntervalTicks <= 0 || debtLimitQpc <= 0) {
        return 0;
    }

    return std::max<uint32_t>(1u,
                              static_cast<uint32_t>((debtLimitQpc + targetIntervalTicks - 1) / targetIntervalTicks));
}

inline bool IsWgcEncoderLimitedSmoothnessMode(bool encoderBottlenecked, bool encoderActivelyTooSlow,
                                              uint32_t overloadFlags) {
    return encoderBottlenecked || encoderActivelyTooSlow ||
           (overloadFlags & (kEncoderOverloadFlagEncoder | kEncoderOverloadFlagMux)) != 0;
}

inline uint32_t GetWgcLiveVisualDebtLimitTicksForMode(int64_t targetIntervalTicks, int64_t qpcTicksPerSecond,
                                                      bool encoderLimitedSmoothnessMode) {
    if (!encoderLimitedSmoothnessMode) {
        return GetWgcLiveVisualDebtLimitTicks(targetIntervalTicks, qpcTicksPerSecond);
    }

    return GetWgcLiveVisualDebtLimitTicks(targetIntervalTicks, qpcTicksPerSecond, kWgcEncoderLimitedLiveVisualDebtMs,
                                          kWgcEncoderLimitedLiveVisualDebtFrames);
}

inline uint32_t GetWgcLiveVisualDebtExcessTicks(uint32_t outputShortfallTicks, int64_t targetIntervalTicks,
                                                int64_t qpcTicksPerSecond, uint32_t maxDebtMs = kWgcMaxLiveVisualDebtMs,
                                                uint32_t maxDebtFrames = kWgcMaxLiveVisualDebtFrames) {
    const uint32_t debtLimitTicks =
        GetWgcLiveVisualDebtLimitTicks(targetIntervalTicks, qpcTicksPerSecond, maxDebtMs, maxDebtFrames);
    if (debtLimitTicks == 0 || outputShortfallTicks <= debtLimitTicks) {
        return 0;
    }

    return outputShortfallTicks - debtLimitTicks;
}

inline uint32_t GetWgcLiveVisualDebtExcessTicksForMode(uint32_t outputShortfallTicks, int64_t targetIntervalTicks,
                                                       int64_t qpcTicksPerSecond, bool encoderLimitedSmoothnessMode) {
    if (!encoderLimitedSmoothnessMode) {
        return GetWgcLiveVisualDebtExcessTicks(outputShortfallTicks, targetIntervalTicks, qpcTicksPerSecond);
    }

    return GetWgcLiveVisualDebtExcessTicks(outputShortfallTicks, targetIntervalTicks, qpcTicksPerSecond,
                                           kWgcEncoderLimitedLiveVisualDebtMs, kWgcEncoderLimitedLiveVisualDebtFrames);
}

inline uint32_t GetWgcLiveSchedulerRebaseTicksThisLoop(
    uint32_t requestedTicks, uint32_t outputShortfallTicks, uint32_t excessTicks,
    uint32_t maxTicksPerLoop = kWgcMaxLiveSchedulerRebaseTicksPerLoop) {
    if (requestedTicks == 0 || outputShortfallTicks == 0 || excessTicks == 0 || maxTicksPerLoop == 0) {
        return 0;
    }

    return std::min(std::min(requestedTicks, outputShortfallTicks), std::min(excessTicks, maxTicksPerLoop));
}

inline uint32_t GetWgcLiveSchedulerRebaseTicksThisLoopForMode(uint32_t requestedTicks, uint32_t outputShortfallTicks,
                                                              uint32_t excessTicks, bool encoderLimitedSmoothnessMode) {
    return GetWgcLiveSchedulerRebaseTicksThisLoop(requestedTicks, outputShortfallTicks, excessTicks,
                                                  encoderLimitedSmoothnessMode
                                                      ? kWgcEncoderLimitedLiveSchedulerRebaseTicksPerLoop
                                                      : kWgcMaxLiveSchedulerRebaseTicksPerLoop);
}

inline int64_t GetWgcLiveVisualDebtFloorQpc(int64_t liveNowQpc, int64_t targetIntervalTicks,
                                            int64_t qpcTicksPerSecond) {
    const int64_t debtLimitQpc = GetWgcLiveVisualDebtLimitQpc(targetIntervalTicks, qpcTicksPerSecond);
    if (liveNowQpc <= 0 || debtLimitQpc <= 0) {
        return 0;
    }

    return liveNowQpc > debtLimitQpc ? (liveNowQpc - debtLimitQpc) : 0;
}

inline int64_t GetWgcLiveVisualDebtFloorQpcForMode(int64_t liveNowQpc, int64_t targetIntervalTicks,
                                                   int64_t qpcTicksPerSecond, bool encoderLimitedSmoothnessMode) {
    const int64_t debtLimitQpc =
        encoderLimitedSmoothnessMode
            ? GetWgcLiveVisualDebtLimitQpc(targetIntervalTicks, qpcTicksPerSecond, kWgcEncoderLimitedLiveVisualDebtMs,
                                           kWgcEncoderLimitedLiveVisualDebtFrames)
            : GetWgcLiveVisualDebtLimitQpc(targetIntervalTicks, qpcTicksPerSecond);
    if (liveNowQpc <= 0 || debtLimitQpc <= 0) {
        return 0;
    }

    return liveNowQpc > debtLimitQpc ? (liveNowQpc - debtLimitQpc) : 0;
}

inline int64_t ClampWgcSelectionTargetToLiveQpc(
    int64_t selectionTargetQpc, int64_t liveNowQpc, int64_t targetIntervalTicks, int64_t qpcTicksPerSecond,
    bool lowSourceMode, bool liveRecoveryMode, uint32_t outputShortfallTicks, bool encoderBottlenecked,
    uint32_t severeShortfallThresholdTicks = kCfrShortfallCatchupThresholdTicks,
    bool encoderLimitedSmoothnessMode = false) {
    (void)lowSourceMode;
    (void)liveRecoveryMode;
    (void)encoderBottlenecked;
    if (selectionTargetQpc <= 0) {
        return selectionTargetQpc;
    }
    if (liveNowQpc <= 0 || targetIntervalTicks <= 0 || outputShortfallTicks < severeShortfallThresholdTicks) {
        return selectionTargetQpc;
    }

    if (GetWgcLiveVisualDebtExcessTicksForMode(outputShortfallTicks, targetIntervalTicks, qpcTicksPerSecond,
                                               encoderLimitedSmoothnessMode) == 0) {
        return selectionTargetQpc;
    }

    const int64_t visualDebtFloorQpc = GetWgcLiveVisualDebtFloorQpcForMode(
        liveNowQpc, targetIntervalTicks, qpcTicksPerSecond, encoderLimitedSmoothnessMode);
    if (visualDebtFloorQpc <= 0 || selectionTargetQpc >= visualDebtFloorQpc) {
        return selectionTargetQpc;
    }

    // The CFR PTS/audio endpoint remain sequential and authoritative.  Once
    // the encoder loop has fallen outside the bounded live window, however,
    // keeping source selection pinned to the stale slot causes "too-new"
    // repeats while good near-live frames pile up.  Clamp only the source-frame
    // selection target to the live-window floor so overload is absorbed as
    // visual debt drops, not audio cuts or fast-forwarded backlog.
    return visualDebtFloorQpc;
}

inline bool IsWgcFrameTooNewForCfrSlot(int64_t frameSelectionQpc, int64_t selectionTargetQpc,
                                       int64_t targetIntervalTicks,
                                       uint32_t maxLeadTicks = kWgcCfrSelectionMaxLeadTicks) {
    if (frameSelectionQpc <= 0 || selectionTargetQpc <= 0 || targetIntervalTicks <= 0) {
        return false;
    }

    const int64_t maxLeadQpc = targetIntervalTicks * static_cast<int64_t>(std::max<uint32_t>(1u, maxLeadTicks));
    return frameSelectionQpc > selectionTargetQpc + maxLeadQpc;
}

inline uint32_t GetWgcDelayReservoirDelayFrames(int64_t contentDelayQpc, int64_t targetIntervalTicks) {
    if (contentDelayQpc <= 0 || targetIntervalTicks <= 0) {
        return 0;
    }

    return static_cast<uint32_t>((contentDelayQpc + targetIntervalTicks - 1) / targetIntervalTicks);
}

inline uint32_t GetWgcDelayReservoirLowWaterFrames(int64_t contentDelayQpc, int64_t targetIntervalTicks) {
    return GetWgcDelayReservoirDelayFrames(contentDelayQpc, targetIntervalTicks);
}

inline uint32_t GetWgcDelayReservoirTargetFrames(int64_t contentDelayQpc, int64_t targetIntervalTicks,
                                                 uint32_t extraFrames = kWgcDelayReservoirTargetExtraFrames) {
    const uint32_t delayFrames = GetWgcDelayReservoirDelayFrames(contentDelayQpc, targetIntervalTicks);
    if (delayFrames == 0) {
        return 0;
    }

    return delayFrames + extraFrames;
}

inline int64_t GetWgcActiveDelayResidualToleranceQpc(int64_t targetIntervalTicks) {
    if (targetIntervalTicks <= 0) {
        return 0;
    }

    return std::max<int64_t>(
        1, (targetIntervalTicks * static_cast<int64_t>(kWgcActiveDelayResidualTolerancePermille)) / 1000);
}

inline int64_t GetWgcActiveDelayResidualHardLimitQpc(int64_t targetIntervalTicks, int64_t qpcTicksPerSecond) {
    const int64_t strictToleranceQpc = GetWgcActiveDelayResidualToleranceQpc(targetIntervalTicks);
    if (qpcTicksPerSecond <= 0) {
        return strictToleranceQpc;
    }

    const int64_t hardLimitQpc =
        (qpcTicksPerSecond * static_cast<int64_t>(kWgcActiveDelayResidualHardLimitUs)) / 1000000;
    return std::max<int64_t>(strictToleranceQpc, hardLimitQpc);
}

inline uint32_t GetWgcActiveDelaySoftLateTargetUs(int64_t targetIntervalTicks, int64_t qpcTicksPerSecond) {
    if (targetIntervalTicks <= 0 || qpcTicksPerSecond <= 0) {
        return kWgcActiveDelaySoftLateTargetMinUs;
    }
    const uint64_t frameUs =
        (static_cast<uint64_t>(targetIntervalTicks) * 1000000ull) / static_cast<uint64_t>(qpcTicksPerSecond);
    const uint32_t softTargetUs = static_cast<uint32_t>((frameUs * 3ull) / 4ull);
    return std::clamp(softTargetUs, kWgcActiveDelaySoftLateTargetMinUs, kWgcActiveDelaySoftLateTargetMaxUs);
}

inline bool IsWgcFrameTooNewForActiveDelaySlot(int64_t frameSelectionQpc, int64_t selectionTargetQpc,
                                               int64_t targetIntervalTicks) {
    if (frameSelectionQpc <= 0 || selectionTargetQpc <= 0 || targetIntervalTicks <= 0) {
        return false;
    }

    return frameSelectionQpc > selectionTargetQpc + GetWgcActiveDelayResidualToleranceQpc(targetIntervalTicks);
}

inline bool IsWgcFrameTooNewForActiveDelayHardLimit(int64_t frameSelectionQpc, int64_t selectionTargetQpc,
                                                    int64_t targetIntervalTicks, int64_t qpcTicksPerSecond) {
    if (frameSelectionQpc <= 0 || selectionTargetQpc <= 0 || targetIntervalTicks <= 0) {
        return false;
    }

    return frameSelectionQpc >
           selectionTargetQpc + GetWgcActiveDelayResidualHardLimitQpc(targetIntervalTicks, qpcTicksPerSecond);
}

inline bool IsWgcActiveDelayFinalSelectionWithinHardLimit(int64_t predictedSelectionQpc, int64_t rawSelectionQpc,
                                                          int64_t selectionTargetQpc, int64_t targetIntervalTicks,
                                                          int64_t qpcTicksPerSecond) {
    if (selectionTargetQpc <= 0 || targetIntervalTicks <= 0) {
        return true;
    }
    if (IsWgcFrameTooNewForActiveDelayHardLimit(predictedSelectionQpc, selectionTargetQpc, targetIntervalTicks,
                                                qpcTicksPerSecond)) {
        return false;
    }
    if (rawSelectionQpc > 0 &&
        IsWgcFrameTooNewForActiveDelayHardLimit(rawSelectionQpc, selectionTargetQpc, targetIntervalTicks,
                                                qpcTicksPerSecond)) {
        return false;
    }
    return true;
}

inline int64_t GetWgcActiveDelayRepeatClusterPenaltyQpc(uint32_t repeatClusterTicks, int64_t targetIntervalTicks) {
    if (repeatClusterTicks == 0 || targetIntervalTicks <= 0) {
        return 0;
    }

    const uint32_t penaltyPermille = std::min<uint32_t>(
        repeatClusterTicks * kWgcActiveDelayRepeatClusterPenaltyPermille,
        kWgcActiveDelayRepeatClusterPenaltyMaxPermille);
    return std::max<int64_t>(1, (targetIntervalTicks * static_cast<int64_t>(penaltyPermille)) / 1000);
}

enum class WgcActiveDelayRelaxedDecision : uint8_t {
    kRejectInvalid = 0,
    kRejectNotRelaxed = 1,
    kRejectSyncRisk = 2,
    kRejectResidualHeadroom = 3,
    kRejectRepeatCost = 4,
    kAcceptBetterTarget = 5,
    kAcceptRepeatCluster = 6,
    kAcceptSoftRepeatAvoidance = 7,
};

inline const char* WgcActiveDelayRelaxedDecisionToString(WgcActiveDelayRelaxedDecision decision) {
    switch (decision) {
        case WgcActiveDelayRelaxedDecision::kRejectInvalid:
            return "invalid";
        case WgcActiveDelayRelaxedDecision::kRejectNotRelaxed:
            return "not_relaxed";
        case WgcActiveDelayRelaxedDecision::kRejectSyncRisk:
            return "sync_risk";
        case WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom:
            return "residual_headroom";
        case WgcActiveDelayRelaxedDecision::kRejectRepeatCost:
            return "repeat_cost";
        case WgcActiveDelayRelaxedDecision::kAcceptBetterTarget:
            return "better_target";
        case WgcActiveDelayRelaxedDecision::kAcceptRepeatCluster:
            return "repeat_cluster";
        case WgcActiveDelayRelaxedDecision::kAcceptSoftRepeatAvoidance:
            return "soft_repeat_avoidance";
    }
    return "unknown";
}

struct WgcActiveDelayRelaxedCandidateScore {
    WgcActiveDelayRelaxedDecision decision = WgcActiveDelayRelaxedDecision::kRejectInvalid;
    int64_t candidateDamageQpc = 0;
    int64_t repeatDamageQpc = 0;
    int64_t repeatClusterPenaltyQpc = 0;
    int64_t repeatBudgetQpc = 0;
    uint32_t candidateLateResidualUs = 0;

    bool Accepted() const {
        return decision == WgcActiveDelayRelaxedDecision::kAcceptBetterTarget ||
               decision == WgcActiveDelayRelaxedDecision::kAcceptRepeatCluster ||
               decision == WgcActiveDelayRelaxedDecision::kAcceptSoftRepeatAvoidance;
    }
};

inline bool HasWgcActiveDelayResidualHeadroom(
    uint32_t candidateLateResidualUs, uint32_t residualAvgAbsUs, uint32_t residualP95Us, uint32_t residualLateMaxUs,
    WgcActiveDelayWindowClass windowClass = WgcActiveDelayWindowClass::kSourceLimited,
    uint32_t softLateTargetUs = kWgcActiveDelaySoftLateTargetMaxUs) {
    if (candidateLateResidualUs > kWgcActiveDelayResidualHardLimitUs) {
        return false;
    }
    if (windowClass != WgcActiveDelayWindowClass::kSourceLimited && candidateLateResidualUs > softLateTargetUs &&
        residualLateMaxUs >= softLateTargetUs) {
        return false;
    }
    if (residualAvgAbsUs > kWgcActiveDelayResidualMeanTargetUs) {
        return false;
    }
    if (residualP95Us > kWgcActiveDelayResidualP95TargetUs) {
        return false;
    }
    return residualLateMaxUs <= kWgcActiveDelayResidualHardLimitUs;
}

inline WgcActiveDelayRelaxedCandidateScore ScoreWgcActiveDelayRelaxedCandidate(
    int64_t candidateSelectionQpc, int64_t repeatSelectionQpc, int64_t selectionTargetQpc,
    int64_t targetIntervalTicks, int64_t qpcTicksPerSecond, uint32_t repeatClusterTicks = 0,
    uint32_t residualAvgAbsUs = 0, uint32_t residualP95Us = 0, uint32_t residualLateMaxUs = 0,
    WgcActiveDelayWindowClass windowClass = WgcActiveDelayWindowClass::kSourceLimited,
    uint32_t softLateTargetUs = kWgcActiveDelaySoftLateTargetMaxUs) {
    WgcActiveDelayRelaxedCandidateScore result{};
    if (candidateSelectionQpc <= 0 || repeatSelectionQpc <= 0 || selectionTargetQpc <= 0 ||
        targetIntervalTicks <= 0) {
        result.decision = WgcActiveDelayRelaxedDecision::kRejectInvalid;
        return result;
    }
    if (!IsWgcFrameTooNewForActiveDelaySlot(candidateSelectionQpc, selectionTargetQpc, targetIntervalTicks)) {
        result.decision = WgcActiveDelayRelaxedDecision::kRejectNotRelaxed;
        return result;
    }
    if (IsWgcFrameTooNewForActiveDelayHardLimit(candidateSelectionQpc, selectionTargetQpc, targetIntervalTicks,
                                                qpcTicksPerSecond)) {
        result.decision = WgcActiveDelayRelaxedDecision::kRejectSyncRisk;
        return result;
    }

    const int64_t candidateLateQpc =
        candidateSelectionQpc > selectionTargetQpc ? (candidateSelectionQpc - selectionTargetQpc) : 0;
    if (candidateLateQpc > 0 && qpcTicksPerSecond > 0) {
        const uint64_t candidateLateUs =
            (static_cast<uint64_t>(candidateLateQpc) * 1000000ull) / static_cast<uint64_t>(qpcTicksPerSecond);
        result.candidateLateResidualUs = candidateLateUs > UINT32_MAX ? UINT32_MAX
                                                                      : static_cast<uint32_t>(candidateLateUs);
    }
    if (!HasWgcActiveDelayResidualHeadroom(result.candidateLateResidualUs, residualAvgAbsUs, residualP95Us,
                                           residualLateMaxUs, windowClass, softLateTargetUs)) {
        result.decision = WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom;
        return result;
    }

    result.repeatDamageQpc = repeatSelectionQpc >= selectionTargetQpc ? (repeatSelectionQpc - selectionTargetQpc)
                                                                      : (selectionTargetQpc - repeatSelectionQpc);
    result.candidateDamageQpc = candidateSelectionQpc >= selectionTargetQpc
                                    ? (candidateSelectionQpc - selectionTargetQpc)
                                    : (selectionTargetQpc - candidateSelectionQpc);
    result.repeatClusterPenaltyQpc =
        GetWgcActiveDelayRepeatClusterPenaltyQpc(repeatClusterTicks, targetIntervalTicks);
    result.repeatBudgetQpc =
        INT64_MAX - result.repeatDamageQpc < result.repeatClusterPenaltyQpc
            ? INT64_MAX
            : result.repeatDamageQpc + result.repeatClusterPenaltyQpc;

    if (result.candidateDamageQpc < result.repeatDamageQpc) {
        result.decision = WgcActiveDelayRelaxedDecision::kAcceptBetterTarget;
        return result;
    }
    if (windowClass != WgcActiveDelayWindowClass::kSourceLimited &&
        result.candidateLateResidualUs <= softLateTargetUs && qpcTicksPerSecond > 0) {
        const uint64_t softBudgetQpc =
            (static_cast<uint64_t>(softLateTargetUs) * static_cast<uint64_t>(qpcTicksPerSecond)) / 1000000ull;
        const int64_t softRepeatBudgetQpc =
            INT64_MAX - result.repeatDamageQpc < static_cast<int64_t>(softBudgetQpc)
                ? INT64_MAX
                : result.repeatDamageQpc + static_cast<int64_t>(softBudgetQpc);
        if (result.candidateDamageQpc <= softRepeatBudgetQpc) {
            result.decision = WgcActiveDelayRelaxedDecision::kAcceptSoftRepeatAvoidance;
            return result;
        }
    }
    if (result.repeatClusterPenaltyQpc > 0 && result.candidateDamageQpc <= result.repeatBudgetQpc) {
        result.decision = WgcActiveDelayRelaxedDecision::kAcceptRepeatCluster;
        return result;
    }

    result.decision = WgcActiveDelayRelaxedDecision::kRejectRepeatCost;
    return result;
}

inline bool IsWgcActiveDelayRelaxedCandidateUseful(int64_t candidateSelectionQpc, int64_t repeatSelectionQpc,
                                                   int64_t selectionTargetQpc, int64_t targetIntervalTicks,
                                                   int64_t qpcTicksPerSecond,
                                                   uint32_t repeatClusterTicks = 0, uint32_t residualAvgAbsUs = 0,
                                                   uint32_t residualP95Us = 0, uint32_t residualLateMaxUs = 0,
                                                   WgcActiveDelayWindowClass windowClass =
                                                       WgcActiveDelayWindowClass::kSourceLimited,
                                                   uint32_t softLateTargetUs =
                                                       kWgcActiveDelaySoftLateTargetMaxUs) {
    return ScoreWgcActiveDelayRelaxedCandidate(candidateSelectionQpc, repeatSelectionQpc, selectionTargetQpc,
                                               targetIntervalTicks, qpcTicksPerSecond, repeatClusterTicks,
                                               residualAvgAbsUs, residualP95Us, residualLateMaxUs, windowClass,
                                               softLateTargetUs)
        .Accepted();
}

inline WgcActiveDelayRelaxedCandidateScore ScoreWgcActiveDelayRepeatRescueCandidate(
    int64_t candidateSelectionQpc, int64_t candidateRawSelectionQpc, int64_t repeatSelectionQpc,
    int64_t selectionTargetQpc, int64_t targetIntervalTicks, int64_t qpcTicksPerSecond,
    uint32_t repeatClusterTicks = 0, uint32_t residualAvgAbsUs = 0, uint32_t residualP95Us = 0,
    uint32_t residualLateMaxUs = 0, WgcActiveDelayWindowClass windowClass = WgcActiveDelayWindowClass::kSourceLimited,
    uint32_t softLateTargetUs = kWgcActiveDelaySoftLateTargetMaxUs) {
    WgcActiveDelayRelaxedCandidateScore result{};
    if (candidateSelectionQpc <= 0 || repeatSelectionQpc <= 0 || selectionTargetQpc <= 0 ||
        targetIntervalTicks <= 0) {
        result.decision = WgcActiveDelayRelaxedDecision::kRejectInvalid;
        return result;
    }
    if (!IsWgcActiveDelayFinalSelectionWithinHardLimit(candidateSelectionQpc, candidateRawSelectionQpc,
                                                       selectionTargetQpc, targetIntervalTicks, qpcTicksPerSecond)) {
        result.decision = WgcActiveDelayRelaxedDecision::kRejectSyncRisk;
        return result;
    }

    const int64_t predictedLateQpc =
        candidateSelectionQpc > selectionTargetQpc ? (candidateSelectionQpc - selectionTargetQpc) : 0;
    const int64_t rawLateQpc =
        candidateRawSelectionQpc > selectionTargetQpc ? (candidateRawSelectionQpc - selectionTargetQpc) : 0;
    const int64_t lateQpc = std::max(predictedLateQpc, rawLateQpc);
    if (lateQpc > 0 && qpcTicksPerSecond > 0) {
        const uint64_t candidateLateUs =
            (static_cast<uint64_t>(lateQpc) * 1000000ull) / static_cast<uint64_t>(qpcTicksPerSecond);
        result.candidateLateResidualUs = candidateLateUs > UINT32_MAX ? UINT32_MAX
                                                                      : static_cast<uint32_t>(candidateLateUs);
    }
    if (!HasWgcActiveDelayResidualHeadroom(result.candidateLateResidualUs, residualAvgAbsUs, residualP95Us,
                                           residualLateMaxUs, windowClass, softLateTargetUs)) {
        result.decision = WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom;
        return result;
    }

    result.repeatDamageQpc = repeatSelectionQpc >= selectionTargetQpc ? (repeatSelectionQpc - selectionTargetQpc)
                                                                      : (selectionTargetQpc - repeatSelectionQpc);
    result.candidateDamageQpc = candidateSelectionQpc >= selectionTargetQpc
                                    ? (candidateSelectionQpc - selectionTargetQpc)
                                    : (selectionTargetQpc - candidateSelectionQpc);
    result.repeatClusterPenaltyQpc =
        GetWgcActiveDelayRepeatClusterPenaltyQpc(repeatClusterTicks, targetIntervalTicks);
    result.repeatBudgetQpc =
        INT64_MAX - result.repeatDamageQpc < result.repeatClusterPenaltyQpc
            ? INT64_MAX
            : result.repeatDamageQpc + result.repeatClusterPenaltyQpc;

    if (result.candidateDamageQpc < result.repeatDamageQpc) {
        result.decision = WgcActiveDelayRelaxedDecision::kAcceptBetterTarget;
        return result;
    }
    if (windowClass != WgcActiveDelayWindowClass::kSourceLimited &&
        result.candidateLateResidualUs <= softLateTargetUs && qpcTicksPerSecond > 0) {
        const uint64_t softBudgetQpc =
            (static_cast<uint64_t>(softLateTargetUs) * static_cast<uint64_t>(qpcTicksPerSecond)) / 1000000ull;
        const int64_t softRepeatBudgetQpc =
            INT64_MAX - result.repeatDamageQpc < static_cast<int64_t>(softBudgetQpc)
                ? INT64_MAX
                : result.repeatDamageQpc + static_cast<int64_t>(softBudgetQpc);
        if (result.candidateDamageQpc <= softRepeatBudgetQpc) {
            result.decision = WgcActiveDelayRelaxedDecision::kAcceptSoftRepeatAvoidance;
            return result;
        }
    }
    if (result.repeatClusterPenaltyQpc > 0 && result.candidateDamageQpc <= result.repeatBudgetQpc) {
        result.decision = WgcActiveDelayRelaxedDecision::kAcceptRepeatCluster;
        return result;
    }

    result.decision = WgcActiveDelayRelaxedDecision::kRejectRepeatCost;
    return result;
}

inline bool IsWgcActiveDelayMixedPolicyPressureFault(
    uint32_t sourceLimitedHolds, uint32_t policyHolds, uint32_t totalHolds,
    uint32_t minPolicyHolds = kWgcActiveDelayPolicyHoldFaultMinCount,
    uint32_t minPolicyPermille = kWgcActiveDelayPolicyHoldFaultPermille) {
    if (policyHolds < minPolicyHolds || totalHolds == 0) {
        return false;
    }
    const uint64_t policyPermille = (static_cast<uint64_t>(policyHolds) * 1000ull) / static_cast<uint64_t>(totalHolds);
    return policyPermille >= minPolicyPermille || policyHolds > sourceLimitedHolds;
}

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

inline bool IsWgcCfrSmoothnessNotMaximal(uint32_t totalOutputTicks, uint32_t excessRepeats,
                                         uint32_t policyAddedRepeats, uint32_t excessRepeatClusterMaxTicks,
                                         uint32_t postSelectionRejectedSync) {
    if (postSelectionRejectedSync > 0) {
        return true;
    }
    if (excessRepeatClusterMaxTicks >= kWgcCfrSmoothnessExcessRepeatClusterFaultTicks) {
        return true;
    }
    if (policyAddedRepeats >= kWgcCfrSmoothnessExcessRepeatFaultMinCount) {
        return true;
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
                                                 bool encoderLimitedSmoothnessMode = false) {
    if (frameSelectionQpc <= 0) {
        return true;
    }

    const int64_t visualDebtFloorQpc = GetWgcLiveVisualDebtFloorQpcForMode(
        liveNowQpc, targetIntervalTicks, qpcTicksPerSecond, encoderLimitedSmoothnessMode);
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
