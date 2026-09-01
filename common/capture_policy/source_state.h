#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <limits>

#include "encoder_priority_and_routing.h"

// Held mode, low-source/live-recovery classification, warmup, and inject reserves.

namespace ce::capture_policy {

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
    kHardSourceStall,
    kPostStallRecovery,
};

inline const char* WgcActiveDelayWindowClassToString(WgcActiveDelayWindowClass state) {
    switch (state) {
        case WgcActiveDelayWindowClass::kHealthy:
            return "healthy";
        case WgcActiveDelayWindowClass::kRecoverableUnderfill:
            return "recoverable_underfill";
        case WgcActiveDelayWindowClass::kSourceLimited:
            return "source_limited";
        case WgcActiveDelayWindowClass::kHardSourceStall:
            return "hard_source_stall";
        case WgcActiveDelayWindowClass::kPostStallRecovery:
            return "post_stall_recovery";
    }
    return "unknown";
}

inline bool IsWgcActiveDelaySourceLimitedClass(WgcActiveDelayWindowClass state) {
    return state == WgcActiveDelayWindowClass::kSourceLimited || state == WgcActiveDelayWindowClass::kHardSourceStall;
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

// Inject CFR startup needs enough source history to cover the fixed A/V content-delay target while
// still retaining the physical GPU/fence safety tail. This count is a startup readiness condition
// only; contentDelayFrames must never be subtracted from the live timestamp selector's candidate set.
inline size_t GetInjectCfrStartupReadyFrames(size_t injectReserveFrames, size_t contentDelayFrames) {
    const size_t required = injectReserveFrames + contentDelayFrames + 1;
    return std::max(required, kInjectWarmupCommitFloorFrames);
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

inline bool ShouldWarnEncoderApproachingCapacity(double smoothedEncodeMs, double frameIntervalMs,
                                                  bool startupWindowActive) {
    return !startupWindowActive && smoothedEncodeMs > 0.0 && frameIntervalMs > 0.0 &&
           smoothedEncodeMs > frameIntervalMs * kEncoderCapacityWarningRatio;
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

// Display-correlated inject timestamps can legitimately move ahead when the
// presentation queue deepens (for example, a DLSS-G cutscene transition).
// Retain enough source samples to span from the newest adjusted timestamp back
// to the current CFR playout target; that direct span includes virtual-clock
// lead, display phase, and fixed A/V content delay. Otherwise the count cap can
// delete the oldest future sample every tick, preventing it from ever aging
// into the target and turning one transient into a permanent repeat loop.
inline size_t GetInjectTimestampRetentionLimit(size_t baselineLimit, size_t injectReserveFrames,
                                               int64_t requiredTimestampSpanQpc,
                                               int64_t sourceIntervalQpc,
                                               size_t maximumAdaptiveLimit) {
    if (sourceIntervalQpc <= 0 || maximumAdaptiveLimit <= baselineLimit) {
        return baselineLimit;
    }

    const uint64_t retentionQpc =
        static_cast<uint64_t>(std::max<int64_t>(0, requiredTimestampSpanQpc));
    const uint64_t intervalQpc = static_cast<uint64_t>(sourceIntervalQpc);
    uint64_t timestampFrames = retentionQpc / intervalQpc;
    if (retentionQpc % intervalQpc != 0) {
        ++timestampFrames;
    }

    const uint64_t fixedFrames = static_cast<uint64_t>(injectReserveFrames) + 2ull;
    const uint64_t requiredFrames =
        timestampFrames > std::numeric_limits<uint64_t>::max() - fixedFrames
            ? std::numeric_limits<uint64_t>::max()
            : timestampFrames + fixedFrames;
    const size_t boundedRequired = static_cast<size_t>(
        std::min<uint64_t>(requiredFrames, static_cast<uint64_t>(maximumAdaptiveLimit)));
    return std::max(baselineLimit, boundedRequired);
}

// The metadata ring can describe more frames than the producer has reusable
// textures. Bound startup headroom and adaptive growth to both physical
// resources so a display-phase event cannot lease every producer texture and
// stop the arrivals needed to recover. The minimum required limit represents
// the configured A/V delay and remains authoritative if it needs more than the
// ordinary safety-reserved capacity.
inline size_t GetInjectRetentionCeiling(size_t minimumRequiredLimit,
                                        size_t metadataSlotCount,
                                        size_t textureSlotCount) {
    const size_t metadataLimit =
        metadataSlotCount > kInjectFrameRingSafetySlots
            ? metadataSlotCount - kInjectFrameRingSafetySlots
            : metadataSlotCount;
    const size_t textureLimit =
        textureSlotCount > kInjectTextureLeaseSafetySlots
            ? textureSlotCount - kInjectTextureLeaseSafetySlots
            : textureSlotCount;
    return std::max(minimumRequiredLimit, std::min(metadataLimit, textureLimit));
}

// Retained jitter-buffer history is intentional source state, not ingress
// pressure. Feeding it into throttleCapture creates a closed loop: capture is
// stopped precisely while fresh arrivals are needed to advance selection.
inline bool ShouldThrottleInjectProducer(size_t ingressQueueDepth,
                                         size_t ingressQueueCapacity,
                                         int64_t latestFenceWaitUs) {
    const size_t queuePressureThreshold =
        std::max<size_t>(8, ingressQueueCapacity / 2);
    return ingressQueueDepth >= queuePressureThreshold || latestFenceWaitUs > 16'000;
}

inline bool ShouldPreserveInjectFrontAtBufferCap(int64_t frontTimestampQpc,
                                                 int64_t playoutTargetQpc,
                                                 int64_t leadToleranceQpc) {
    if (frontTimestampQpc <= 0 || playoutTargetQpc <= 0 || leadToleranceQpc < 0) {
        return false;
    }
    if (playoutTargetQpc < std::numeric_limits<int64_t>::min() + leadToleranceQpc) {
        return true;
    }
    // Cap enforcement precedes selection. Keep both a future frame that still
    // needs to age in and a near-target frame that is ready on this exact tick;
    // only a front genuinely behind the target is disposable backlog.
    return frontTimestampQpc >= playoutTargetQpc - leadToleranceQpc;
}

inline int64_t GetInjectLiveMaxFrameAgeQpc(bool recordingOutputLive, bool encoderBottlenecked,
                                           bool encoderActivelyTooSlow, bool recoveryActive,
                                           int64_t targetIntervalTicks) {
    if (!recordingOutputLive || targetIntervalTicks <= 0) {
        return 0;
    }

    const uint32_t maxAgeTicks = (encoderBottlenecked || encoderActivelyTooSlow || recoveryActive)
                                     ? kInjectLivePressureMaxFrameAgeTicks
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

inline WgcActiveDelayWindowClass ClassifyWgcActiveDelayWindow(const WgcAdaptiveTelemetry& telemetry, bool lowSourceMode,
                                                              bool liveRecoveryMode, bool sourceStarved,
                                                              bool deepUnderfeed, bool activeDelaySourceRecovery,
                                                              bool hardSafeCandidateAvailable) {
    if (telemetry.outputFps == 0) {
        return WgcActiveDelayWindowClass::kHealthy;
    }

    const bool telemetrySourceLimited =
        sourceStarved || deepUnderfeed ||
        IsWgcInputBelowTarget(telemetry.outputFps, telemetry.recentInputMin250Fps, telemetry.recentInputMin500Fps) ||
        telemetry.emptyTickPermille >= kWgcDeepUnderfeedEmptyTickPermille ||
        IsWgcActiveDelaySourceLimitedJitter(telemetry);

    if (!hardSafeCandidateAvailable) {
        return WgcActiveDelayWindowClass::kHardSourceStall;
    }
    if (activeDelaySourceRecovery) {
        return WgcActiveDelayWindowClass::kPostStallRecovery;
    }
    if (telemetrySourceLimited) {
        return WgcActiveDelayWindowClass::kSourceLimited;
    }
    if (lowSourceMode || liveRecoveryMode || IsWgcSchedulerDeliveryLimited(telemetry) ||
        telemetry.emptyTickPermille >= kWgcLowSourceEmptyTickPermille || IsWgcActiveDelayRecoverableJitter(telemetry)) {
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
                                          bool encoderBottlenecked, bool stableUnderfeedExit = false) {
    if (telemetry.outputFps == 0) {
        return true;
    }

    if (encoderBottlenecked || outputShortfallTicks > 1) {
        return false;
    }

    // When low-source mode exits via stable underfeed (source consistently below target
    // but delivering steadily), also exit live-recovery to let the overcapture cap
    // restore at an adaptive rate matching actual source capability.
    if (stableUnderfeedExit) {
        return true;
    }

    return !ShouldEnterWgcLowSourceMode(telemetry) && telemetry.bufferedWgcFrames <= 4;
}

inline bool ShouldExitWgcLowSourceMode(const WgcAdaptiveTelemetry& telemetry, bool encoderTooSlowForTarget = false,
                                       bool bufferedReserveRecovered = false, uint64_t durationInModeMs = 0) {
    if (telemetry.outputFps == 0) {
        return true;
    }

    if (!encoderTooSlowForTarget && bufferedReserveRecovered) {
        return true;
    }

    // Stable underfeed exit: if the source has been delivering below target for a long
    // period without starvation, treat this as the steady state and exit low-source mode
    // so the overcapture cap can be restored to an adaptive rate matching actual capability.
    constexpr uint64_t kStableUnderfeedExitMs = 5000;
    if (durationInModeMs >= kStableUnderfeedExitMs && telemetry.recentDeliveredMin250Fps > 0 &&
        telemetry.recentDeliveredMin500Fps > 0 && telemetry.recentInputMin250Fps > 0 &&
        telemetry.recentInputMin500Fps > 0 && telemetry.emptyTickPermille <= kWgcLowSourceExitEmptyTickPermille &&
        telemetry.bufferedWgcFrames <= 4) {
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

}  // namespace ce::capture_policy
