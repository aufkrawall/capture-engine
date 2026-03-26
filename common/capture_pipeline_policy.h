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
constexpr uint32_t kWgcLowSourceEnterHoldMs = 120;
constexpr uint32_t kWgcLowSourceExitHoldMs = 250;
constexpr uint32_t kWgcMaxSelectionLagTicks = 2;
constexpr uint32_t kWgcLowSourceMaxSelectionLagTicks = 3;
constexpr uint32_t kWgcWarmupBufferedFrames = 3;
constexpr uint32_t kWgcWarmupStableSourceFps = 118;
constexpr size_t kWgcWarmupFreshFrames = 2;
constexpr uint32_t kWgcReservePressurePermille = 600;
constexpr uint32_t kWgcReserveBiasPermille = 250;
constexpr uint32_t kWgcReserveFragileBiasPermille = 375;
constexpr uint32_t kWgcSingleFreshHoldInputPermille = 995;
constexpr uint32_t kWgcSteadyReserveBuildInputPermille = 995;
constexpr uint32_t kWgcSelectionDelayTicks = 2;
constexpr uint32_t kWgcCoverageDelayMaxTicks = 32;
constexpr uint32_t kCfrShortfallCatchupThresholdTicks = 2;
constexpr uint32_t kCfrShortfallForceCatchupThresholdTicks = 18;
constexpr double kWgcSevereShortfallDurationMs = 500.0;

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
    // Both WGC and Inject must NOT discard timer debt in CFR mode. If timer debt is discarded,
    // the total output frames will fall short of the CFR target over time, causing the
    // video track length to be shorter than real time. To maintain exact A/V sync and
    // track length matching, we must preserve this debt and drain it at the end of capture.
    return false;
}

inline bool ShouldCfrCatchUpToWallClock(uint32_t outputShortfallTicks, bool useScreenGrab, bool frameAvailable,
                                        bool hasLastFrame) {
    if (outputShortfallTicks == 0) {
        return false;
    }

    if (outputShortfallTicks >= kCfrShortfallForceCatchupThresholdTicks) {
        return true;
    }

    if (outputShortfallTicks < kCfrShortfallCatchupThresholdTicks) {
        return false;
    }

    return useScreenGrab ? (frameAvailable || hasLastFrame) : frameAvailable;
}

inline bool CanDrainOutstandingWgcTicks(bool queuedWgcFrameAvailable, bool bufferedWgcFrameAvailable, bool hasLastFrame,
                                        bool mediaEngineCanRepeatLastFrame) {
    return queuedWgcFrameAvailable || bufferedWgcFrameAvailable || hasLastFrame || mediaEngineCanRepeatLastFrame;
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

inline uint32_t GetWgcCatchupTicksThisLoop(bool encoderBottlenecked, size_t bufferedWgcFrames,
                                           double frameCreditAccumulator, uint32_t outputShortfallTicks) {
    const uint32_t baseCatchupTicks = GetCfrCatchupTicksThisLoop(outputShortfallTicks);
    if (baseCatchupTicks > 1u) {
        return baseCatchupTicks;
    }

    if (outputShortfallTicks < kCfrShortfallCatchupThresholdTicks) {
        return baseCatchupTicks;
    }

    return ShouldAllowWgcExtraCatchupTicks(encoderBottlenecked, bufferedWgcFrames, frameCreditAccumulator,
                                           outputShortfallTicks)
               ? 2u
               : baseCatchupTicks;
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
    return HasWgcSevereLiveShortfall(shortfallDurationMs) ? 4.0 : 3.0;
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
                                         double frameIntervalMs,
                                         uint32_t maxDelayTicks = kWgcCoverageDelayMaxTicks) {
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

inline bool IsInjectEncoderStartup(bool recordingOutputLive, uint64_t recordingLiveTick, uint64_t nowTick) {
    if (!recordingOutputLive || nowTick < recordingLiveTick) {
        return true;
    }
    return (nowTick - recordingLiveTick) < kEncoderStartupWindowMs;
}

inline size_t GetInjectBufferedHeadroom(bool recordingOutputLive, uint64_t recordingLiveTick, uint64_t nowTick) {
    return IsInjectEncoderStartup(recordingOutputLive, recordingLiveTick, nowTick)
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

inline bool ShouldEnterWgcLowSourceMode(const WgcAdaptiveTelemetry& telemetry) {
    if (telemetry.outputFps == 0) {
        return false;
    }

    return telemetry.recentDeliveredFps < telemetry.outputFps ||
           telemetry.recentDeliveredMin250Fps < telemetry.outputFps ||
           telemetry.recentDeliveredMin500Fps < telemetry.outputFps ||
           telemetry.recentInputMin250Fps < telemetry.outputFps ||
           telemetry.recentInputMin500Fps < telemetry.outputFps ||
           telemetry.emptyTickPermille >= kWgcLowSourceEmptyTickPermille;
}

inline bool ShouldExitWgcLowSourceMode(const WgcAdaptiveTelemetry& telemetry) {
    if (telemetry.outputFps == 0) {
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

inline bool ShouldUseWgcLowSourceMode(const WgcAdaptiveTelemetry& telemetry) {
    return ShouldEnterWgcLowSourceMode(telemetry);
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
                                                        int64_t targetIntervalTicks, bool lowSourceMode) {
    int64_t minTimestampQpc = lastEmittedSourceQpc > 0 ? (lastEmittedSourceQpc + 1) : 0;
    const int64_t maxSelectionLagQpc = GetWgcMaxSelectionLagQpc(targetIntervalTicks, lowSourceMode);
    if (selectionTargetQpc > 0 && maxSelectionLagQpc > 0 && targetIntervalTicks > 0) {
        // When the queue is underfed, allow one extra output tick of lag before we
        // prefer repeating the previous frame over consuming unique source content.
        minTimestampQpc = std::max(minTimestampQpc, selectionTargetQpc - maxSelectionLagQpc - targetIntervalTicks);
    }
    return minTimestampQpc;
}

inline bool IsWgcTimestampFreshEnough(int64_t frameTimestampQpc, int64_t minFreshTimestampQpc) {
    return frameTimestampQpc > 0 && (minFreshTimestampQpc <= 0 || frameTimestampQpc >= minFreshTimestampQpc);
}

inline bool ShouldPreferEarlierFreshWgcFrameToPreserveReserve(int64_t earlierFrameTimestampQpc,
                                                              int64_t selectedFrameTimestampQpc,
                                                              int64_t selectionTargetQpc, int64_t targetIntervalTicks,
                                                              bool reservePressureActive, bool lowSourceMode) {
    if (earlierFrameTimestampQpc <= 0 || selectedFrameTimestampQpc <= 0 || selectionTargetQpc <= 0 ||
        targetIntervalTicks <= 0) {
        return false;
    }

    const int64_t earlierDistance = earlierFrameTimestampQpc >= selectionTargetQpc
                                        ? (earlierFrameTimestampQpc - selectionTargetQpc)
                                        : (selectionTargetQpc - earlierFrameTimestampQpc);
    const int64_t selectedDistance = selectedFrameTimestampQpc >= selectionTargetQpc
                                         ? (selectedFrameTimestampQpc - selectionTargetQpc)
                                         : (selectionTargetQpc - selectedFrameTimestampQpc);
    const uint32_t biasPermille =
        (reservePressureActive || lowSourceMode) ? kWgcReserveFragileBiasPermille : kWgcReserveBiasPermille;
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
                                          bool reserveAvailableAtTickStart) {
    if (outputShortfallTicks > 0 || encoderBottlenecked || reserveAvailableAtTickStart) {
        return false;
    }

    return ShouldAllowSingleFreshWgcHold(reservePressureActive, lowSourceMode, recentInputMin250Fps, outputFps,
                                         smoothedInputPerTick);
}

inline size_t ClampWgcSelectionIndexForLowSource(size_t bestIdx, size_t availableCount, size_t bufferedWgcFrames,
                                                 uint32_t recentDeliveredFps, uint32_t outputFps,
                                                 uint32_t emptyTickPermille) {
    if (availableCount <= 1) {
        return 0;
    }

    size_t clampedIdx = std::min(bestIdx, availableCount - 1);
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
