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
constexpr uint32_t kWgcCfrOvercaptureHeadroomPermille = 1250;
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
    (void)hasLastFrame;
    (void)mediaEngineCanRepeatLastFrame;
    // Stop drain may finish real WGC content that was already captured but not
    // encoded yet. It must not extend the tail using only cached last-frame
    // repeats, because that creates frozen video with continuing audio.
    return queuedWgcFrameAvailable || bufferedWgcFrameAvailable;
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
                                         bool encoderBottlenecked, bool reserveAvailableAtTickStart) {
    if (!recordingOutputLive || kWgcSelectionDelayTicks == 0) {
        return false;
    }

    return true;
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

inline bool IsWgcSourceHealthyEnoughToSuppressEncoderLimitedCatchup(uint32_t outputFps, uint32_t recentInputMin250Fps,
                                                                    uint32_t noFreshTickPermille, bool lowSourceMode) {
    if (lowSourceMode || outputFps == 0) {
        return false;
    }

    return recentInputMin250Fps >= outputFps && noFreshTickPermille < kWgcLowSourceEmptyTickPermille;
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
    if (catchupTicksThisLoop <= 1u) {
        return 0u;
    }

    return catchupTicksThisLoop - 1u;
}

inline uint32_t GetWgcCatchupTicksThisLoop(bool encoderBottlenecked, bool encoderActivelyTooSlow,
                                           size_t bufferedWgcFrames, double frameCreditAccumulator,
                                           uint32_t outputShortfallTicks, uint32_t outputFps,
                                           uint32_t recentDeliveredMin250Fps, uint32_t recentInputMin250Fps,
                                           uint32_t noFreshTickPermille, bool lowSourceMode,
                                           double audioLeadExcessMs = 0.0) {
    if (outputShortfallTicks < kCfrShortfallCatchupThresholdTicks) {
        return 1u;
    }

    if (ShouldPrioritizeWgcAudioLeadCatchup(audioLeadExcessMs)) {
        // Audio lead should restore live catchup immediately, but once the
        // encoder/source recover enough to support smooth pacing again, do not
        // keep escalating historical debt into repeat-heavy force bursts.
        if (outputShortfallTicks >= kCfrShortfallForceCatchupThresholdTicks && !encoderActivelyTooSlow &&
            IsWgcSourceRecoveredEnoughForSmoothAudioLeadCatchup(outputFps, recentDeliveredMin250Fps,
                                                                recentInputMin250Fps, noFreshTickPermille)) {
            return 2u;
        }

        return std::max<uint32_t>(2u, GetCfrCatchupTicksThisLoop(outputShortfallTicks, encoderBottlenecked));
    }

    if (IsWgcSourceHealthyForLiveCatchup(outputFps, recentDeliveredMin250Fps, recentInputMin250Fps, noFreshTickPermille,
                                         lowSourceMode)) {
        // When WGC is already delivering near-target fresh content, draining
        // historical shortfall via live duplicate bursts is visually worse than
        // carrying that debt and letting stop-drain close the exact CFR count.
        return 1u;
    }

    if (encoderBottlenecked && IsWgcSourceHealthyEnoughToSuppressEncoderLimitedCatchup(
                                   outputFps, recentInputMin250Fps, noFreshTickPermille, lowSourceMode)) {
        // When the encoder is the limiter but WGC input is still healthy, live
        // catch-up drains audio buffers and produces visibly uneven repeat
        // bursts without improving source coverage. Carry the debt instead.
        return 1u;
    }

    if (outputShortfallTicks < kCfrShortfallForceCatchupThresholdTicks &&
        IsWgcSourceDegradedForLiveCatchup(outputFps, recentDeliveredMin250Fps, recentInputMin250Fps,
                                          noFreshTickPermille)) {
        return 1u;
    }

    const uint32_t baseCatchupTicks = GetCfrCatchupTicksThisLoop(outputShortfallTicks, encoderBottlenecked);
    if (baseCatchupTicks > 1u) {
        return baseCatchupTicks;
    }

    return ShouldAllowWgcExtraCatchupTicks(encoderBottlenecked, bufferedWgcFrames, frameCreditAccumulator,
                                           outputShortfallTicks)
               ? 2u
               : 1u;
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

inline bool IsWgcFramePastStartupBarrier(int64_t frameQpc, int64_t startupBarrierQpc) {
    return startupBarrierQpc <= 0 || (frameQpc > 0 && frameQpc >= startupBarrierQpc);
}

inline uint32_t GetWgcCfrOvercaptureTargetFps(uint32_t outputFps,
                                              uint32_t headroomPermille = kWgcCfrOvercaptureHeadroomPermille) {
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
        return injectReserveFrames - 1;
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

    return telemetry.recentInputMin250Fps + fpsMargin < telemetry.outputFps ||
           telemetry.recentInputMin500Fps + fpsMargin < telemetry.outputFps;
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

    if (IsWgcSourceStarved(telemetry)) {
        return WgcLiveRecoveryState::kSourceStarved;
    }

    if (IsWgcSchedulerDeliveryLimited(telemetry)) {
        return WgcLiveRecoveryState::kSchedulerLimited;
    }

    if (encoderBottlenecked && outputShortfallTicks >= kWgcRecoveryEnterShortfallTicks) {
        return WgcLiveRecoveryState::kEncoderLimited;
    }

    return WgcLiveRecoveryState::kHealthy;
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

inline int64_t GetWgcSelectionTargetQpc(int64_t scheduledSampleQpc, int64_t fallbackTargetQpc,
                                        int64_t targetIntervalTicks, bool recordingOutputLive) {
    int64_t selectionTargetQpc = scheduledSampleQpc > 0 ? scheduledSampleQpc : fallbackTargetQpc;
    if (!recordingOutputLive || selectionTargetQpc <= 0 || targetIntervalTicks <= 0 || kWgcSelectionDelayTicks == 0) {
        return selectionTargetQpc;
    }

    const int64_t delayedSelectionTargetQpc =
        selectionTargetQpc - (targetIntervalTicks * static_cast<int64_t>(kWgcSelectionDelayTicks));
    return delayedSelectionTargetQpc > 0 ? delayedSelectionTargetQpc : selectionTargetQpc;
}

inline int64_t ClampWgcSelectionTargetToLiveQpc(
    int64_t selectionTargetQpc, int64_t liveNowQpc, int64_t targetIntervalTicks, bool lowSourceMode,
    bool liveRecoveryMode, uint32_t outputShortfallTicks, bool encoderBottlenecked,
    uint32_t severeShortfallThresholdTicks = kCfrShortfallCatchupThresholdTicks) {
    if (selectionTargetQpc <= 0 || liveNowQpc <= 0 || targetIntervalTicks <= 0) {
        return selectionTargetQpc;
    }

    int64_t maxLiveLagQpc = GetWgcMaxSelectionLagQpc(targetIntervalTicks, lowSourceMode);
    if (maxLiveLagQpc <= 0) {
        return selectionTargetQpc;
    }

    if (liveRecoveryMode) {
        maxLiveLagQpc = std::min<int64_t>(maxLiveLagQpc, targetIntervalTicks);
    } else if (encoderBottlenecked || outputShortfallTicks >= severeShortfallThresholdTicks) {
        maxLiveLagQpc = std::min<int64_t>(maxLiveLagQpc, targetIntervalTicks);
    } else {
        maxLiveLagQpc = std::min<int64_t>(maxLiveLagQpc, targetIntervalTicks * 2);
    }

    const int64_t liveSelectionFloorQpc = liveNowQpc - maxLiveLagQpc;
    return std::max(selectionTargetQpc, liveSelectionFloorQpc);
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
