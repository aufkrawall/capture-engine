#pragma once

#include <algorithm>
#include <stddef.h>
#include <stdint.h>

namespace ce::capture_policy {

constexpr uint32_t kRecordingWarmupMinMs = 120;
constexpr uint32_t kRecordingWarmupMaxMs = 350;
constexpr size_t kInjectWarmupCommitFloorFrames = 3;
constexpr size_t kMaxInjectBufferedHeadroomFrames = 12;
constexpr size_t kStartupInjectBufferedHeadroomFrames = 48;
constexpr uint64_t kEncoderStartupWindowMs = 1500;
constexpr uint32_t kAutoWgcFallbackDelayNoPidMs = 100;
constexpr uint32_t kAutoWgcFallbackDelayWithPidMs = 200;
constexpr uint32_t kWgcHeadroomEnableMarginFps = 10;
constexpr uint32_t kWgcHeadroomDisableMarginFps = 2;
constexpr uint32_t kWgcHeadroomEnableMaxJitterUs = 900;
constexpr uint32_t kWgcHeadroomDisableMaxJitterUs = 1100;
constexpr uint32_t kWgcHeadroomEnableMaxEmptyTickPermille = 50;
constexpr uint32_t kWgcHeadroomDisableEmptyTickPermille = 125;
constexpr uint32_t kWgcLowSourceEmptyTickPermille = 80;
constexpr uint32_t kWgcWarmupBufferedFrames = 3;
constexpr uint32_t kWgcWarmupStableSourceFps = 118;

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

inline bool ShouldCommitRecordingWarmup(bool useScreenGrab, bool useVFR, bool poppedFrame,
                                        bool hasBufferedWgcFrame, size_t bufferedInjectFrames,
                                        size_t injectReserveFrames, uint32_t warmupElapsedMs) {
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

inline uint32_t GetWgcHeadroomEnableThresholdFps(uint32_t outputFps) {
    return outputFps + std::max<uint32_t>(kWgcHeadroomEnableMarginFps, outputFps / 12u);
}

inline uint32_t GetWgcHeadroomDisableThresholdFps(uint32_t outputFps) {
    return outputFps + std::max<uint32_t>(kWgcHeadroomDisableMarginFps, outputFps / 60u);
}

inline uint32_t GetWgcHeadroomTargetFps(uint32_t outputFps) {
    const uint32_t minimumHeadroomFps = outputFps + kWgcHeadroomEnableMarginFps;
    const uint32_t scaledHeadroomFps = outputFps + std::max<uint32_t>(1u, (outputFps * 8u + 99u) / 100u);
    return std::max(minimumHeadroomFps, scaledHeadroomFps);
}

inline bool ShouldEnterWgcHeadroomMode(const WgcAdaptiveTelemetry& telemetry) {
    if (telemetry.outputFps == 0) {
        return false;
    }

    const uint32_t enableThresholdFps = GetWgcHeadroomEnableThresholdFps(telemetry.outputFps);
    const uint32_t safeInputThresholdFps = telemetry.outputFps + 6u;
    const bool stableDelivered = telemetry.recentDeliveredFps >= enableThresholdFps &&
                                 telemetry.recentDeliveredMin250Fps >= enableThresholdFps &&
                                 telemetry.recentDeliveredMin500Fps >= enableThresholdFps;
    const bool stableInput = telemetry.recentInputMin250Fps >= safeInputThresholdFps &&
                             telemetry.recentInputMin500Fps >= safeInputThresholdFps;
    const bool stableQueue = telemetry.emptyTickPermille <= kWgcHeadroomEnableMaxEmptyTickPermille &&
                             telemetry.bufferedWgcFrames <= 2 && telemetry.encoderQueueDepth <= 1;
    const bool lowDuplicatePressure = telemetry.duplicateRatio <= 0.01;
    const bool lowJitter = telemetry.averageJitterUs > 0 && telemetry.averageJitterUs <= kWgcHeadroomEnableMaxJitterUs;
    return stableDelivered && stableInput && stableQueue && lowDuplicatePressure && lowJitter;
}

inline bool ShouldExitWgcHeadroomMode(const WgcAdaptiveTelemetry& telemetry) {
    if (telemetry.outputFps == 0) {
        return true;
    }

    const uint32_t disableThresholdFps = GetWgcHeadroomDisableThresholdFps(telemetry.outputFps);
    if (telemetry.recentDeliveredFps <= disableThresholdFps || telemetry.recentDeliveredMin250Fps <= disableThresholdFps ||
        telemetry.recentDeliveredMin500Fps <= disableThresholdFps || telemetry.recentInputMin250Fps <= disableThresholdFps ||
        telemetry.recentInputMin500Fps <= disableThresholdFps) {
        return true;
    }

    return telemetry.duplicateRatio > 0.015 || telemetry.averageJitterUs >= kWgcHeadroomDisableMaxJitterUs ||
           telemetry.emptyTickPermille >= kWgcHeadroomDisableEmptyTickPermille || telemetry.bufferedWgcFrames > 3 ||
           telemetry.encoderQueueDepth > 2;
}

inline bool ShouldUseWgcLowSourceMode(const WgcAdaptiveTelemetry& telemetry) {
    if (telemetry.outputFps == 0) {
        return false;
    }

    return telemetry.recentDeliveredFps < telemetry.outputFps || telemetry.recentDeliveredMin250Fps < telemetry.outputFps ||
           telemetry.recentDeliveredMin500Fps < telemetry.outputFps || telemetry.recentInputMin250Fps < telemetry.outputFps ||
           telemetry.recentInputMin500Fps < telemetry.outputFps ||
           telemetry.emptyTickPermille >= kWgcLowSourceEmptyTickPermille;
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
    if (dropIndex == 0) {
        return false;
    }

    if (!lowSourceMode) {
        return true;
    }

    const bool fragileQueue = bufferedWgcFrames <= 2 || emptyTickPermille >= 120;
    return !fragileQueue;
}

}  // namespace ce::capture_policy
