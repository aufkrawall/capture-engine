#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>

#include "source_state.h"

// Selection targets, live visual debt limits, and scheduler rebasing.

namespace ce::capture_policy {

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

// Single source of truth for the WGC active-delay selection target. It subtracts the configured
// content delay UNLESS live-recovery legitimately suppresses it for this path (legacy reservoir path
// only -- the uniform-cadence path HOLDS the delay through live-recovery, see
// ShouldLiveRecoverySuppressWgcSelectionDelay). The per-tick "is the delay applied this tick" flag
// (ShouldApplyWgcSelectionDelay -> wgcSelectionDelayAppliedThisTick) and this target computation MUST
// agree: if the flag says "delay applied" while the target silently drops the delay, the realized
// content delay collapses to ~0 and -- because a perpetually-below-output VRR source keeps
// live-recovery latched forever -- stays collapsed for the rest of the recording (real regression
// 20260626_050554). Routing both decisions through ShouldLiveRecoverySuppressWgcSelectionDelay keeps
// them from diverging again.
inline int64_t GetWgcActiveDelaySelectionTargetQpc(int64_t scheduledSampleQpc, int64_t fallbackTargetQpc,
                                                   int64_t targetIntervalTicks, bool recordingOutputLive,
                                                   bool applyLiveDelay, bool liveRecoveryActive,
                                                   bool uniformCadenceActiveDelay, int64_t contentDelayQpc) {
    const bool suppress = ShouldLiveRecoverySuppressWgcSelectionDelay(liveRecoveryActive, uniformCadenceActiveDelay);
    return GetWgcSelectionTargetQpc(scheduledSampleQpc, fallbackTargetQpc, targetIntervalTicks,
                                    recordingOutputLive && applyLiveDelay && !suppress, contentDelayQpc);
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

inline int64_t GetWgcLiveVisualDebtLimitQpcForMode(int64_t targetIntervalTicks, int64_t qpcTicksPerSecond,
                                                   bool encoderLimitedSmoothnessMode) {
    return encoderLimitedSmoothnessMode
               ? GetWgcLiveVisualDebtLimitQpc(targetIntervalTicks, qpcTicksPerSecond,
                                              kWgcEncoderLimitedLiveVisualDebtMs,
                                              kWgcEncoderLimitedLiveVisualDebtFrames)
               : GetWgcLiveVisualDebtLimitQpc(targetIntervalTicks, qpcTicksPerSecond);
}

inline uint32_t GetWgcLiveVisualDebtLimitTicksForMode(int64_t targetIntervalTicks, int64_t qpcTicksPerSecond,
                                                      bool encoderLimitedSmoothnessMode) {
    const int64_t debtLimitQpc =
        GetWgcLiveVisualDebtLimitQpcForMode(targetIntervalTicks, qpcTicksPerSecond, encoderLimitedSmoothnessMode);
    if (targetIntervalTicks <= 0 || debtLimitQpc <= 0) {
        return 0;
    }

    return std::max<uint32_t>(1u,
                              static_cast<uint32_t>((debtLimitQpc + targetIntervalTicks - 1) / targetIntervalTicks));
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
    (void)requestedTicks;
    (void)outputShortfallTicks;
    (void)excessTicks;
    (void)encoderLimitedSmoothnessMode;
    // Rebase used to advance the scheduler grid without submitting packets.
    // That made CFR metadata look current while punching real PTS holes. Keep
    // visual source pruning separate, but never consume output ticks here.
    return 0;
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
                                                   int64_t qpcTicksPerSecond, bool encoderLimitedSmoothnessMode,
                                                   int64_t intentionalContentDelayQpc = 0) {
    const int64_t debtLimitQpc =
        GetWgcLiveVisualDebtLimitQpcForMode(targetIntervalTicks, qpcTicksPerSecond, encoderLimitedSmoothnessMode);
    if (liveNowQpc <= 0 || debtLimitQpc <= 0) {
        return 0;
    }

    const int64_t contentDelayQpc = std::max<int64_t>(0, intentionalContentDelayQpc);
    if (contentDelayQpc > INT64_MAX - debtLimitQpc) {
        return 0;
    }
    const int64_t maximumFrameAgeQpc = contentDelayQpc + debtLimitQpc;
    return liveNowQpc > maximumFrameAgeQpc ? (liveNowQpc - maximumFrameAgeQpc) : 0;
}

inline bool ShouldPruneWgcVisualDebtFrameForGrid(int64_t frameQpc, int64_t nextFrameQpc, int64_t wallDebtFloorQpc,
                                                 int64_t immutableSelectionTargetQpc) {
    if (frameQpc <= 0 || wallDebtFloorQpc <= 0 || frameQpc >= wallDebtFloorQpc) {
        return false;
    }
    if (immutableSelectionTargetQpc <= 0) {
        return true;
    }

    // During encoder debt, retain the newest predecessor of the immutable CFR
    // content target. Relabelling newer pixels would break content-level A/V sync.
    return frameQpc < immutableSelectionTargetQpc && nextFrameQpc > 0 &&
           nextFrameQpc <= immutableSelectionTargetQpc;
}

inline bool ShouldProtectWgcStartupSmoothnessHistory(bool recordingOutputLive, bool startupSmoothnessAttempted,
                                                     int64_t smoothnessTargetDelayQpc,
                                                     int64_t liveVisualDebtLimitQpc) {
    // Live visual-debt pruning is intentionally shallower than the optional startup jitter reservoir
    // (250 ms versus a 300 ms default). Before the startup contract locks the smoothness delay, applying
    // that live ceiling makes the requested reservoir mathematically unreachable and forces the
    // partial-span fallback. Preserve the bounded pre-live history until the transactional contract
    // selects its frame; the retained-frame cap still owns memory pressure.
    return !recordingOutputLive && startupSmoothnessAttempted && smoothnessTargetDelayQpc > 0 &&
           liveVisualDebtLimitQpc > 0 && smoothnessTargetDelayQpc > liveVisualDebtLimitQpc;
}

inline int64_t ClampWgcSelectionTargetToLiveQpc(
    int64_t selectionTargetQpc, int64_t liveNowQpc, int64_t targetIntervalTicks, int64_t qpcTicksPerSecond,
    bool lowSourceMode, bool liveRecoveryMode, uint32_t outputShortfallTicks, bool encoderBottlenecked,
    uint32_t severeShortfallThresholdTicks = kCfrShortfallCatchupThresholdTicks,
    bool encoderLimitedSmoothnessMode = false, int64_t intentionalContentDelayQpc = 0) {
    (void)liveNowQpc;
    (void)targetIntervalTicks;
    (void)qpcTicksPerSecond;
    (void)lowSourceMode;
    (void)liveRecoveryMode;
    (void)outputShortfallTicks;
    (void)encoderBottlenecked;
    (void)severeShortfallThresholdTicks;
    (void)encoderLimitedSmoothnessMode;
    (void)intentionalContentDelayQpc;
    // Never move source selection ahead of the immutable CFR output slot.
    // Doing so puts near-live video content into an older PTS while audio
    // remains sample-continuous at that PTS, creating real content-level A/V
    // drift even though packet endpoints still match. Overload debt is repaid
    // with held-frame output slots; source pruning may discard obsolete input,
    // but it must not change the content time requested by the current slot.
    return selectionTargetQpc;
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

}  // namespace ce::capture_policy
