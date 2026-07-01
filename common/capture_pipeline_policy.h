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
constexpr uint32_t kWgcActiveDelayResidualTolerancePermille = 900;
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
constexpr uint32_t kWgcCfrSmoothnessPolicyRepeatNoticeMinCount = 24;
constexpr uint32_t kWgcCfrSmoothnessPolicyRepeatNoticePermille = 5;
constexpr uint32_t kWgcCfrSmoothnessExcessRepeatClusterFaultTicks = 24;
constexpr uint32_t kWgcDelayReservoirTargetExtraFrames = 1;
constexpr uint32_t kWgcSmoothnessBufferDefaultMaxMs = 300;
constexpr uint32_t kWgcSmoothnessBufferDefaultVramBudgetMb = 3000;
constexpr uint32_t kWgcSmoothnessBufferPoolSafetyFrames = 4;
constexpr uint32_t kWgcSmoothnessBufferPoolInFlightFrames = 1;
constexpr uint32_t kWgcSmoothnessBufferPoolSelectedSlackFrames = 1;
constexpr uint32_t kWgcSmoothnessSourceFramePoolDefaultBuffers = 8;
constexpr uint32_t kWgcSmoothnessSourceFramePoolMinBuffers = 3;
constexpr uint32_t kWgcSmoothnessSourceFramePoolCompactHighFpsMaxBuffers = 16;
constexpr uint32_t kWgcSmoothnessEstimatedSyncDelayMs = 33;
constexpr uint32_t kWgcSmoothnessBufferMinPoolFrames = 8;
constexpr uint32_t kWgcSmoothnessBufferMaxPoolFrames = 64;
constexpr uint32_t kWgcSmoothnessBufferPoolHeadroomSlots = 8;
// The retained WGC reservoir stores source frames, not output CFR slots. Size the
// millisecond smoothness budget for the normal source-above-output case
// (e.g. 140/144 Hz VRR into 120 fps CFR), otherwise a "300 ms" reservoir is only
// about 250 ms of 144 Hz source history.
constexpr uint32_t kWgcSmoothnessBufferSourceRatePermille = 1250;
// WGC smoothness FLOOR: a baseline jitter-buffer delay that engages the active-delay
// smoothness machinery (nearest-target playout + reservoir) even when there is NO
// audio-latency content delay -- i.e. video-only capture, or a low-confidence loopback
// latency probe, where WGC otherwise runs near-live and is maximally exposed to bursty
// DWM->frame-pool delivery under GPU load. The floor is auto-derived from measured startup
// WGC delivery jitter, then HELD FIXED for the session (never retimed live), so it cannot
// reintroduce the realized-delay-collapse / "ghost image" judder family. It is applied
// symmetrically (older startup video + correspondingly later live-start), so it is
// sync-neutral by construction and NEVER moves the audio anchor (audio stays byte-exact).
// kWgcSmoothnessFloorMinFrames is the minimum depth (frames) the floor engages with so the
// nearest-target playout has room to absorb at least one delivery hiccup.
constexpr uint32_t kWgcSmoothnessFloorMinFrames = 2;
// Jitter headroom (frames) above the active-delay reservoir target before the uniform-cadence pacer
// trims the oldest surplus. Bounds the realized content delay so a VRR / GPU-bound source whose
// present rate transiently rises above the CFR output rate cannot inflate the reservoir without
// bound. Kept small so the realized delay stays close to the target while still absorbing ~1 frame
// of VRR jitter without trimming on every wobble.
constexpr size_t kWgcActiveDelayPaceMaxExcessFrames = 1;
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
// Steady WGC CFR asks the compositor for modest source headroom instead of an
// unlimited producer rate. Max-rate is still used temporarily by the adaptive
// recovery path when source/reservoir telemetry shows that the cap is hurting
// smoothness.
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
        return kCfrShortfallForceCatchupThresholdTicks;
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

// Whether WGC live-recovery should suppress the active content-delay selection for this tick.
// Live-recovery (max-rate catch-up after a shortfall) legitimately runs the legacy reservoir-target
// path near-live. But on the uniform-cadence path a below-target source is the STEADY STATE absorbed
// by even holds at the delay floor, and live-recovery only exits once the source outruns the output
// target -- so suppressing the delay there collapses the realized content delay to ~0 and latches it
// for tens of seconds (the collapse half of the realized-delay rubber-band). Keep the delay applied
// on the uniform-cadence path; live-recovery still drives capture-rate refill.
inline bool ShouldLiveRecoverySuppressWgcSelectionDelay(bool liveRecoveryActive, bool uniformCadenceActiveDelay) {
    return liveRecoveryActive && !uniformCadenceActiveDelay;
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

inline uint32_t GetWgcFrameCountForDurationMs(uint32_t fps, uint32_t durationMs) {
    if (fps == 0 || durationMs == 0) {
        return 0;
    }
    const uint64_t frames =
        (static_cast<uint64_t>(fps) * static_cast<uint64_t>(durationMs) + 999ull) / 1000ull;
    return frames > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(frames);
}

inline bool ShouldAttemptWgcStartupSmoothnessBuffer(bool enabled, bool useVfr, bool avContentDelayActive,
                                                    int64_t targetIntervalTicks, uint32_t retainedExtraFrames) {
    return enabled && !useVfr && avContentDelayActive && targetIntervalTicks > 0 && retainedExtraFrames > 0;
}

inline int64_t GetWgcStartupSmoothnessTargetDelayQpc(uint32_t retainedExtraFrames, int64_t targetIntervalTicks,
                                                     uint32_t outputFps = 0, uint32_t maxSmoothnessMs = 0) {
    if (retainedExtraFrames == 0 || targetIntervalTicks <= 0) {
        return 0;
    }
    uint32_t targetFrames = retainedExtraFrames;
    const uint32_t configuredDelayFrames = GetWgcFrameCountForDurationMs(outputFps, maxSmoothnessMs);
    if (configuredDelayFrames > 0) {
        targetFrames = std::min(targetFrames, configuredDelayFrames);
    }
    const uint64_t targetDelay = static_cast<uint64_t>(targetFrames) * static_cast<uint64_t>(targetIntervalTicks);
    return targetDelay > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(targetDelay);
}

inline int64_t GetWgcStartupReserveWaitBudgetQpc(int64_t startupContentDelayTargetQpc, int64_t targetIntervalTicks,
                                                 int64_t smoothnessTargetDelayQpc,
                                                 bool smoothnessStartupAttempted) {
    if (startupContentDelayTargetQpc <= 0 || targetIntervalTicks <= 0) {
        return 0;
    }

    const auto saturatingAdd = [](int64_t lhs, int64_t rhs) -> int64_t {
        return lhs > INT64_MAX - rhs ? INT64_MAX : lhs + rhs;
    };
    const auto saturatingMul = [](int64_t lhs, int64_t rhs) -> int64_t {
        return rhs > 0 && lhs > INT64_MAX / rhs ? INT64_MAX : lhs * rhs;
    };

    const int64_t baseBudget = saturatingAdd(startupContentDelayTargetQpc, saturatingMul(targetIntervalTicks, 2));
    if (!smoothnessStartupAttempted || smoothnessTargetDelayQpc <= 0) {
        return baseBudget;
    }

    const int64_t smoothnessSlack =
        saturatingAdd(smoothnessTargetDelayQpc, saturatingMul(targetIntervalTicks, 4));
    const int64_t smoothnessBudget = saturatingAdd(startupContentDelayTargetQpc, smoothnessSlack);
    return std::max(baseBudget, smoothnessBudget);
}

inline int64_t SelectWgcStartupSmoothnessExtraDelayQpc(int64_t actualStartupDelayQpc, int64_t avContentDelayQpc,
                                                       int64_t smoothnessTargetDelayQpc) {
    if (actualStartupDelayQpc <= avContentDelayQpc || smoothnessTargetDelayQpc <= 0) {
        return 0;
    }

    return std::clamp<int64_t>(actualStartupDelayQpc - avContentDelayQpc, 0, smoothnessTargetDelayQpc);
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
                                                         bool delayReservoirBelowLowWater, bool lowSourceModeActive,
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

inline bool ShouldUseWgcMaxRateForCappedActiveDelayUnderfeed(const WgcAdaptiveTelemetry& telemetry,
                                                             bool delayReservoirBelowLowWater,
                                                             bool producerCapped) {
    if (!producerCapped || !delayReservoirBelowLowWater || telemetry.outputFps == 0) {
        return false;
    }

    const bool inputBelowOutput =
        telemetry.recentInputMin250Fps > 0 &&
        telemetry.recentInputMin250Fps + kWgcRecoverySourceMarginFps < telemetry.outputFps;
    const bool deliveredBelowOutput =
        telemetry.recentDeliveredMin250Fps > 0 &&
        telemetry.recentDeliveredMin250Fps + kWgcRecoverySourceMarginFps < telemetry.outputFps;
    return inputBelowOutput || deliveredBelowOutput ||
           telemetry.emptyTickPermille >= kWgcLowSourceEmptyTickPermille;
}

inline bool ShouldUseWgcMaxRateForStartupReserveWait(bool reserveMissing, bool waitBudgetRemaining,
                                                     bool producerCapped) {
    return reserveMissing && waitBudgetRemaining && producerCapped;
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

inline bool ShouldUseNativeWgcCursorCapture(bool /*recordingCursorRequested*/) {
    // Native WGC cursor capture can force the live cursor out of the hardware
    // plane and perturb flip-model/VRR promotion. Keep the user's cursor in the
    // recording by compositing it in the encoder instead.
    return false;
}

inline bool ShouldPreferForegroundFullscreenWindowForAutoWgc(bool autoCaptureConfig, bool explicitInjectConfig,
                                                             bool injectWhitelisted, bool hasSourcePid,
                                                             bool hasMatchedConfiguredWgcWindow, bool foregroundUsable,
                                                             bool foregroundFullscreenLike) {
    return autoCaptureConfig && !explicitInjectConfig && !injectWhitelisted && !hasSourcePid &&
           !hasMatchedConfiguredWgcWindow && foregroundUsable && foregroundFullscreenLike;
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

inline uint32_t GetWgcSmoothnessBudgetFps(
    uint32_t outputFps, uint32_t sourceRatePermille = kWgcSmoothnessBufferSourceRatePermille) {
    if (outputFps == 0) {
        return 0;
    }
    if (sourceRatePermille <= 1000u) {
        return outputFps;
    }
    const uint64_t scaled =
        (static_cast<uint64_t>(outputFps) * static_cast<uint64_t>(sourceRatePermille) + 999ull) / 1000ull;
    return scaled > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(scaled);
}

inline uint32_t GetWgcSmoothnessDesiredFrames(uint32_t outputFps, uint32_t maxSmoothnessMs) {
    if (outputFps == 0 || maxSmoothnessMs == 0) {
        return 0;
    }
    const uint32_t budgetFps = GetWgcSmoothnessBudgetFps(outputFps);
    return GetWgcFrameCountForDurationMs(budgetFps, maxSmoothnessMs);
}

inline bool ShouldUseWgcSmoothnessBuffer(bool enabled, bool useVfr, bool avContentDelayActive,
                                         int64_t targetIntervalTicks) {
    return enabled && !useVfr && avContentDelayActive && targetIntervalTicks > 0;
}

// Measured startup WGC delivery jitter used to auto-derive a smoothness floor. All fields are
// microseconds. Delivery-gap fields describe the wall-clock spacing of FrameArrived callbacks
// (DWM -> capture frame pool burstiness); source-jitter fields describe the spacing of the
// game's own presents. Avg fields are for logging only; the derivation uses the max fields.
struct WgcSmoothnessFloorJitter {
    uint32_t deliveryGapAvgUs = 0;
    uint32_t deliveryGapMaxUs = 0;
    uint32_t sourceJitterAvgUs = 0;
    uint32_t sourceJitterMaxUs = 0;
};

// True when a smoothness reservoir/delay should be armed: either an audio-latency content delay
// is active, or a smoothness floor is configured (auto, or an explicit value > 0). This lets the
// existing smoothness gates arm the buffer for video-only / low-confidence-probe captures, where
// avContentDelayActive is false but a baseline jitter buffer is still wanted.
inline bool WgcSmoothnessDelayDesired(bool avContentDelayActive, bool smoothnessFloorConfigured) {
    return avContentDelayActive || smoothnessFloorConfigured;
}

// Upper bound (QPC) for a smoothness floor delay: the smaller of the configured max smoothness
// window and the buildable retained reservoir (so the floor can never target a delay the pool
// cannot hold). Returns 0 when no reservoir capacity is available.
inline int64_t GetWgcSmoothnessFloorCapQpc(int64_t targetIntervalTicks, int64_t qpcTicksPerSecond,
                                           uint32_t maxSmoothnessMs, uint32_t maxReservoirFrames) {
    if (targetIntervalTicks <= 0 || maxReservoirFrames == 0) {
        return 0;
    }
    int64_t capQpc = targetIntervalTicks * static_cast<int64_t>(maxReservoirFrames);
    if (maxSmoothnessMs > 0 && qpcTicksPerSecond > 0) {
        const int64_t maxMsQpc = (qpcTicksPerSecond * static_cast<int64_t>(maxSmoothnessMs)) / 1000;
        if (maxMsQpc > 0) {
            capQpc = std::min(capQpc, maxMsQpc);
        }
    }
    return capQpc > 0 ? capQpc : 0;
}

// Clamp an arbitrary requested floor delay (QPC) to [minFloorFrames, cap]. Returns 0 when no
// reservoir capacity exists (cap == 0), i.e. the floor cannot be realized.
inline int64_t ClampWgcSmoothnessFloorDelayQpc(int64_t requestedFloorQpc, int64_t targetIntervalTicks,
                                               int64_t qpcTicksPerSecond, uint32_t maxSmoothnessMs,
                                               uint32_t maxReservoirFrames,
                                               uint32_t minFloorFrames = kWgcSmoothnessFloorMinFrames) {
    const int64_t capQpc =
        GetWgcSmoothnessFloorCapQpc(targetIntervalTicks, qpcTicksPerSecond, maxSmoothnessMs, maxReservoirFrames);
    if (capQpc <= 0 || targetIntervalTicks <= 0) {
        return 0;
    }
    const int64_t minFloorQpc =
        std::min(capQpc, targetIntervalTicks * static_cast<int64_t>(std::max<uint32_t>(1u, minFloorFrames)));
    return std::clamp(requestedFloorQpc, minFloorQpc, capQpc);
}

// Auto-derive a smoothness floor delay (QPC) from measured startup WGC delivery jitter. It sizes
// the floor to absorb the worst observed delivery burst beyond one frame interval (and the worst
// source-present jitter), with a structural minimum of kWgcSmoothnessFloorMinFrames, clamped to
// the configured max smoothness window and the buildable reservoir. There is NO device-specific
// hardcoded value: the magnitude is measured, then only clamped by config/budget. When no jitter
// has been observed yet the floor falls back to the structural minimum so the active-delay
// machinery still engages with a small buffer.
inline int64_t DeriveWgcSmoothnessFloorDelayQpc(const WgcSmoothnessFloorJitter& jitter, int64_t targetIntervalTicks,
                                                int64_t qpcTicksPerSecond, uint32_t maxSmoothnessMs,
                                                uint32_t maxReservoirFrames,
                                                uint32_t minFloorFrames = kWgcSmoothnessFloorMinFrames) {
    if (targetIntervalTicks <= 0 || qpcTicksPerSecond <= 0) {
        return 0;
    }
    const int64_t frameIntervalUs = (targetIntervalTicks * 1000000) / qpcTicksPerSecond;
    const int64_t deliveryExcessUs = static_cast<int64_t>(jitter.deliveryGapMaxUs) > frameIntervalUs
                                         ? (static_cast<int64_t>(jitter.deliveryGapMaxUs) - frameIntervalUs)
                                         : 0;
    const int64_t jitterUs = std::max<int64_t>(deliveryExcessUs, static_cast<int64_t>(jitter.sourceJitterMaxUs));
    const int64_t requestedFloorQpc = jitterUs > 0 ? (qpcTicksPerSecond * jitterUs) / 1000000 : 0;
    return ClampWgcSmoothnessFloorDelayQpc(requestedFloorQpc, targetIntervalTicks, qpcTicksPerSecond, maxSmoothnessMs,
                                           maxReservoirFrames, minFloorFrames);
}

inline bool ShouldArmWgcSmoothnessBufferForSourceRate(uint32_t outputFps, uint32_t recentInputMin250Fps,
                                                      uint32_t recentInputMin500Fps,
                                                      uint32_t fpsMargin = kWgcRecoverySourceMarginFps) {
    if (outputFps == 0) {
        return false;
    }

    if (recentInputMin250Fps == 0 || recentInputMin500Fps == 0) {
        return false;
    }

    const bool sustainedBelowTarget =
        recentInputMin250Fps + fpsMargin < outputFps && recentInputMin500Fps + fpsMargin < outputFps;
    return !sustainedBelowTarget;
}

inline uint64_t EstimateWgcSurfaceBytes(uint32_t width, uint32_t height, uint32_t bytesPerPixel) {
    if (width == 0 || height == 0 || bytesPerPixel == 0) {
        return 0;
    }
    return static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * static_cast<uint64_t>(bytesPerPixel);
}

inline uint64_t EstimateWgcSmoothnessBytesPerRetainedFrame(uint32_t width, uint32_t height, uint32_t bytesPerPixel,
                                                           uint32_t bufferedCopies = 2) {
    return EstimateWgcSurfaceBytes(width, height, bytesPerPixel) * static_cast<uint64_t>(bufferedCopies);
}

inline uint32_t GetWgcEstimatedSyncDelayFramesForBudget(uint32_t outputFps,
                                                        uint32_t syncDelayMs = kWgcSmoothnessEstimatedSyncDelayMs) {
    return GetWgcSmoothnessDesiredFrames(outputFps, syncDelayMs);
}

inline uint32_t GetWgcSmoothnessBudgetedSurfaceCount(uint32_t width, uint32_t height, uint32_t bytesPerPixel,
                                                     uint32_t budgetMb) {
    const uint64_t bytesPerSurface = EstimateWgcSurfaceBytes(width, height, bytesPerPixel);
    if (budgetMb == 0 || bytesPerSurface == 0) {
        return 0;
    }
    const uint64_t budgetBytes = static_cast<uint64_t>(budgetMb) * 1024ull * 1024ull;
    const uint64_t surfaces = budgetBytes / bytesPerSurface;
    return surfaces > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(surfaces);
}

struct WgcSmoothnessSurfaceBudget {
    uint32_t desiredExtraFrames = 0;
    uint32_t retainedExtraFrames = 0;
    uint32_t retainedFrameCap = 0;
    uint32_t sourceFramePoolBuffers = 0;
    uint32_t copyPoolSlots = 0;
    uint32_t syncDelayFrames = 0;
    uint32_t safetySlots = kWgcSmoothnessBufferPoolSafetyFrames;
    uint32_t inFlightEncodeSlots = kWgcSmoothnessBufferPoolInFlightFrames;
    uint32_t selectedFrameSlackSlots = kWgcSmoothnessBufferPoolSelectedSlackFrames;
    uint32_t reservedFreeCopySlots = kWgcSmoothnessBufferPoolSafetyFrames + kWgcSmoothnessBufferPoolInFlightFrames +
                                     kWgcSmoothnessBufferPoolSelectedSlackFrames;
    uint32_t budgetSurfaceCount = 0;
    uint32_t budgetCopySurfaceCount = 0;
    uint64_t sourceBytesPerSurface = 0;
    uint64_t copyBytesPerSurface = 0;
    uint64_t sourceEstimatedBytes = 0;
    uint64_t copyEstimatedBytes = 0;
    uint64_t estimatedBytes = 0;
    bool splitByteBudget = false;
    bool capLimited = false;
    bool budgetExhausted = false;
};

inline uint32_t GetWgcSmoothnessReservedFreeCopySlots(
    uint32_t safetySlots = kWgcSmoothnessBufferPoolSafetyFrames,
    uint32_t inFlightEncodeSlots = kWgcSmoothnessBufferPoolInFlightFrames,
    uint32_t selectedFrameSlackSlots = kWgcSmoothnessBufferPoolSelectedSlackFrames) {
    return safetySlots + inFlightEncodeSlots + selectedFrameSlackSlots;
}

inline uint32_t GetWgcSmoothnessRetainedFrameCap(uint32_t copyPoolSlots, uint32_t reservedFreeCopySlots) {
    return copyPoolSlots > reservedFreeCopySlots ? (copyPoolSlots - reservedFreeCopySlots) : 0u;
}

inline uint32_t GetWgcSmoothnessExtraFramesForRetainedCap(uint32_t retainedFrameCap, uint32_t syncDelayFrames) {
    const uint32_t requiredDelayFrames = syncDelayFrames + kWgcDelayReservoirTargetExtraFrames;
    return retainedFrameCap > requiredDelayFrames ? (retainedFrameCap - requiredDelayFrames) : 0u;
}

inline uint32_t GetWgcSmoothnessPreferredSourceFramePoolBuffers(uint32_t outputFps, bool compactCopySurfaces) {
    if (!compactCopySurfaces || outputFps < 100) {
        return kWgcSmoothnessSourceFramePoolDefaultBuffers;
    }

    const uint32_t fpsScaled = std::max<uint32_t>(kWgcSmoothnessSourceFramePoolDefaultBuffers, (outputFps + 9u) / 10u);
    return std::min<uint32_t>(fpsScaled, kWgcSmoothnessSourceFramePoolCompactHighFpsMaxBuffers);
}

inline WgcSmoothnessSurfaceBudget ComputeWgcSmoothnessSurfaceBudget(uint32_t outputFps, uint32_t maxSmoothnessMs,
                                                                    uint32_t width, uint32_t height,
                                                                    uint32_t sourceBytesPerPixel,
                                                                    uint32_t copyBytesPerPixel, uint32_t budgetMb,
                                                                    uint32_t syncDelayFrames) {
    WgcSmoothnessSurfaceBudget result{};
    result.desiredExtraFrames = GetWgcSmoothnessDesiredFrames(outputFps, maxSmoothnessMs);
    result.syncDelayFrames = syncDelayFrames;
    result.reservedFreeCopySlots = GetWgcSmoothnessReservedFreeCopySlots(result.safetySlots, result.inFlightEncodeSlots,
                                                                         result.selectedFrameSlackSlots);
    result.splitByteBudget = sourceBytesPerPixel != copyBytesPerPixel;

    const uint64_t sourceBytesPerSurface = EstimateWgcSurfaceBytes(width, height, sourceBytesPerPixel);
    const uint64_t copyBytesPerSurface = EstimateWgcSurfaceBytes(width, height, copyBytesPerPixel);
    result.sourceBytesPerSurface = sourceBytesPerSurface;
    result.copyBytesPerSurface = copyBytesPerSurface;
    result.budgetSurfaceCount = GetWgcSmoothnessBudgetedSurfaceCount(width, height, sourceBytesPerPixel, budgetMb);
    result.budgetCopySurfaceCount = GetWgcSmoothnessBudgetedSurfaceCount(width, height, copyBytesPerPixel, budgetMb);
    if (sourceBytesPerSurface == 0 || copyBytesPerSurface == 0 || result.budgetSurfaceCount == 0 ||
        result.budgetCopySurfaceCount == 0) {
        result.sourceFramePoolBuffers = kWgcSmoothnessSourceFramePoolMinBuffers;
        result.copyPoolSlots = kWgcSmoothnessBufferMinPoolFrames;
        result.retainedFrameCap = GetWgcSmoothnessRetainedFrameCap(result.copyPoolSlots, result.reservedFreeCopySlots);
        result.budgetExhausted = true;
        result.capLimited = result.desiredExtraFrames > 0;
        result.sourceEstimatedBytes = sourceBytesPerSurface * static_cast<uint64_t>(result.sourceFramePoolBuffers);
        result.copyEstimatedBytes = copyBytesPerSurface * static_cast<uint64_t>(result.copyPoolSlots);
        result.estimatedBytes = result.sourceEstimatedBytes + result.copyEstimatedBytes;
        return result;
    }

    uint32_t maxBudgetedCopySlots = 0;
    if (!result.splitByteBudget) {
        uint32_t remainingSurfaces = result.budgetSurfaceCount;
        if (remainingSurfaces >= kWgcSmoothnessSourceFramePoolDefaultBuffers + kWgcSmoothnessBufferMinPoolFrames) {
            result.sourceFramePoolBuffers = kWgcSmoothnessSourceFramePoolDefaultBuffers;
        } else if (remainingSurfaces > kWgcSmoothnessBufferMinPoolFrames) {
            result.sourceFramePoolBuffers = std::max<uint32_t>(kWgcSmoothnessSourceFramePoolMinBuffers,
                                                               remainingSurfaces - kWgcSmoothnessBufferMinPoolFrames);
        } else {
            result.sourceFramePoolBuffers = std::max<uint32_t>(
                1u, std::min<uint32_t>(kWgcSmoothnessSourceFramePoolMinBuffers, remainingSurfaces / 2u));
        }
        result.sourceFramePoolBuffers = std::min(result.sourceFramePoolBuffers, remainingSurfaces);
        remainingSurfaces -= result.sourceFramePoolBuffers;
        maxBudgetedCopySlots = std::min<uint32_t>(remainingSurfaces, kWgcSmoothnessBufferMaxPoolFrames);
    } else {
        const uint64_t budgetBytes = static_cast<uint64_t>(budgetMb) * 1024ull * 1024ull;
        const uint32_t preferredSourceBuffers =
            GetWgcSmoothnessPreferredSourceFramePoolBuffers(outputFps, copyBytesPerPixel < sourceBytesPerPixel);
        const auto copySlotsForSourceBuffers = [&](uint32_t sourceBuffers) -> uint32_t {
            const uint64_t sourceBytes = sourceBytesPerSurface * static_cast<uint64_t>(sourceBuffers);
            if (sourceBytes >= budgetBytes) {
                return 0;
            }
            const uint64_t copySlots = (budgetBytes - sourceBytes) / copyBytesPerSurface;
            return copySlots > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(copySlots);
        };

        uint32_t selectedSourceBuffers = 0;
        uint32_t firstValidSourceBuffers = 0;
        uint32_t bestRetainedFrames = 0;
        for (uint32_t candidate = preferredSourceBuffers; candidate >= kWgcSmoothnessSourceFramePoolMinBuffers;
             --candidate) {
            const uint32_t rawCopySlots = copySlotsForSourceBuffers(candidate);
            if (rawCopySlots >= kWgcSmoothnessBufferMinPoolFrames) {
                if (firstValidSourceBuffers == 0) {
                    firstValidSourceBuffers = candidate;
                }
                const uint32_t cappedCopySlots =
                    std::min<uint32_t>(rawCopySlots, kWgcSmoothnessBufferMaxPoolFrames);
                const uint32_t retainedCap =
                    GetWgcSmoothnessRetainedFrameCap(cappedCopySlots, result.reservedFreeCopySlots);
                const uint32_t extraCapacity =
                    GetWgcSmoothnessExtraFramesForRetainedCap(retainedCap, syncDelayFrames);
                const uint32_t retainedForCandidate =
                    std::min(result.desiredExtraFrames, extraCapacity);
                if (retainedForCandidate > bestRetainedFrames) {
                    bestRetainedFrames = retainedForCandidate;
                    selectedSourceBuffers = candidate;
                }
            }
            if (candidate == kWgcSmoothnessSourceFramePoolMinBuffers) {
                break;
            }
        }
        if (selectedSourceBuffers == 0) {
            selectedSourceBuffers = firstValidSourceBuffers;
        }
        if (selectedSourceBuffers == 0) {
            const uint32_t maxSourceByBudget = static_cast<uint32_t>(
                std::min<uint64_t>(preferredSourceBuffers, budgetBytes / sourceBytesPerSurface));
            selectedSourceBuffers = std::max<uint32_t>(1u, std::min<uint32_t>(maxSourceByBudget, preferredSourceBuffers));
        }

        result.sourceFramePoolBuffers = selectedSourceBuffers;
        maxBudgetedCopySlots =
            std::min<uint32_t>(copySlotsForSourceBuffers(result.sourceFramePoolBuffers),
                               kWgcSmoothnessBufferMaxPoolFrames);
    }

    uint32_t copySlots = maxBudgetedCopySlots;
    if (result.desiredExtraFrames > 0) {
        result.retainedFrameCap = GetWgcSmoothnessRetainedFrameCap(copySlots, result.reservedFreeCopySlots);
        const uint32_t extraFrameCapacity =
            GetWgcSmoothnessExtraFramesForRetainedCap(result.retainedFrameCap, result.syncDelayFrames);
        result.retainedExtraFrames = std::min<uint32_t>(result.desiredExtraFrames, extraFrameCapacity);
        const uint32_t desiredCopySlots = std::max<uint32_t>(
            kWgcSmoothnessBufferMinPoolFrames, result.reservedFreeCopySlots + result.syncDelayFrames +
                                                   kWgcDelayReservoirTargetExtraFrames + result.retainedExtraFrames);
        copySlots = std::min<uint32_t>(copySlots, desiredCopySlots + kWgcSmoothnessBufferPoolHeadroomSlots);
        result.retainedFrameCap = GetWgcSmoothnessRetainedFrameCap(copySlots, result.reservedFreeCopySlots);
    } else {
        result.retainedExtraFrames = 0;
        const uint32_t requiredDelayCopySlots =
            result.reservedFreeCopySlots + result.syncDelayFrames + kWgcDelayReservoirTargetExtraFrames;
        copySlots = std::min<uint32_t>(copySlots,
                                       std::max<uint32_t>(kWgcSmoothnessBufferMinPoolFrames, requiredDelayCopySlots));
        result.retainedFrameCap = GetWgcSmoothnessRetainedFrameCap(copySlots, result.reservedFreeCopySlots);
    }
    result.copyPoolSlots = std::max<uint32_t>(1u, copySlots);
    result.capLimited = result.retainedExtraFrames < result.desiredExtraFrames;
    const uint32_t minimumProtectedCopySlots =
        result.reservedFreeCopySlots + result.syncDelayFrames + kWgcDelayReservoirTargetExtraFrames;
    result.budgetExhausted = result.desiredExtraFrames > 0 && result.copyPoolSlots < minimumProtectedCopySlots;
    result.sourceEstimatedBytes = sourceBytesPerSurface * static_cast<uint64_t>(result.sourceFramePoolBuffers);
    result.copyEstimatedBytes = copyBytesPerSurface * static_cast<uint64_t>(result.copyPoolSlots);
    result.estimatedBytes = result.sourceEstimatedBytes + result.copyEstimatedBytes;
    return result;
}

inline WgcSmoothnessSurfaceBudget ComputeWgcSmoothnessSurfaceBudget(uint32_t outputFps, uint32_t maxSmoothnessMs,
                                                                    uint32_t width, uint32_t height,
                                                                    uint32_t bytesPerPixel, uint32_t budgetMb,
                                                                    uint32_t syncDelayFrames) {
    return ComputeWgcSmoothnessSurfaceBudget(outputFps, maxSmoothnessMs, width, height, bytesPerPixel, bytesPerPixel,
                                             budgetMb, syncDelayFrames);
}

inline uint32_t GetWgcSmoothnessRetainedFrames(uint32_t outputFps, uint32_t maxSmoothnessMs, uint32_t width,
                                               uint32_t height, uint32_t bytesPerPixel, uint32_t budgetMb,
                                               uint32_t syncDelayFrames = 0) {
    const uint32_t desiredFrames = GetWgcSmoothnessDesiredFrames(outputFps, maxSmoothnessMs);
    if (desiredFrames == 0) {
        return 0;
    }
    return ComputeWgcSmoothnessSurfaceBudget(outputFps, maxSmoothnessMs, width, height, bytesPerPixel, budgetMb,
                                             syncDelayFrames)
        .retainedExtraFrames;
}

inline uint32_t GetWgcSmoothnessPoolFrameCount(uint32_t retainedFrames) {
    const uint32_t desiredPool = retainedFrames + kWgcSmoothnessBufferPoolSafetyFrames;
    return std::clamp(desiredPool, kWgcSmoothnessBufferMinPoolFrames, kWgcSmoothnessBufferMaxPoolFrames);
}

inline uint32_t GetWgcPoolPressureRetainedTrimTarget(uint32_t currentFreeCopySlots, uint32_t reservedFreeCopySlots,
                                                     uint32_t delayReservoirTargetFrames,
                                                     uint32_t retainedFrameCap) {
    if (reservedFreeCopySlots == 0 || currentFreeCopySlots > reservedFreeCopySlots || retainedFrameCap == 0 ||
        delayReservoirTargetFrames == 0) {
        return retainedFrameCap;
    }

    return std::min(retainedFrameCap, std::max<uint32_t>(1u, delayReservoirTargetFrames));
}

struct WgcIngressAdmissionDecision {
    bool accept = true;
    bool decimated = false;
    bool softReservePressure = false;
    bool hardReservePressure = false;
    const char* reason = "uncapped";
};

inline bool IsWgcIngressSourceBelowCfrTarget(uint32_t outputFps, uint32_t recentInputMin250Fps,
                                             uint32_t recentInputMin500Fps,
                                             uint32_t fpsMargin = kWgcRecoverySourceMarginFps) {
    if (outputFps == 0 || recentInputMin250Fps == 0 || recentInputMin500Fps == 0) {
        return false;
    }

    return recentInputMin250Fps + fpsMargin < outputFps && recentInputMin500Fps + fpsMargin < outputFps;
}

inline bool IsWgcIngressSourceAtOrAboveCfrTarget(uint32_t outputFps, uint32_t recentInputMin250Fps,
                                                 uint32_t recentInputMin500Fps) {
    if (outputFps == 0) {
        return false;
    }

    const bool has250 = recentInputMin250Fps > 0;
    const bool has500 = recentInputMin500Fps > 0;
    if (!has250 && !has500) {
        return false;
    }
    if (has250 && recentInputMin250Fps < outputFps) {
        return false;
    }
    if (has500 && recentInputMin500Fps < outputFps) {
        return false;
    }
    return true;
}

inline WgcIngressAdmissionDecision DecideWgcIngressAdmission(uint32_t retainedFrames, uint32_t retainedFrameCap,
                                                             uint32_t lowWaterFrames, bool recovering,
                                                             uint32_t outputFps, uint32_t recentInputMin250Fps,
                                                             uint32_t recentInputMin500Fps,
                                                             double admissionCreditFrames, uint32_t freeCopySlots,
                                                             uint32_t reservedFreeCopySlots,
                                                             bool uniformPlayoutOwnsSurplus = false) {
    WgcIngressAdmissionDecision decision{};
    if (retainedFrameCap == 0) {
        decision.reason = "uncapped";
        return decision;
    }
    decision.hardReservePressure = freeCopySlots == 0;
    decision.softReservePressure = reservedFreeCopySlots > 0 && freeCopySlots <= reservedFreeCopySlots;

    if (decision.hardReservePressure) {
        decision.accept = false;
        decision.decimated = true;
        decision.reason = "wgc_ingress_decimated_hard_reserve";
        return decision;
    }

    if (lowWaterFrames > 0 && retainedFrames <= lowWaterFrames) {
        decision.reason = "low_water";
        return decision;
    }
    if (recovering) {
        decision.reason = "recovery";
        return decision;
    }
    if (IsWgcIngressSourceBelowCfrTarget(outputFps, recentInputMin250Fps, recentInputMin500Fps)) {
        decision.reason = "source_below_cfr_target";
        return decision;
    }

    const bool playoutShouldOwnSurplus =
        uniformPlayoutOwnsSurplus &&
        IsWgcIngressSourceAtOrAboveCfrTarget(outputFps, recentInputMin250Fps, recentInputMin500Fps);
    if (decision.softReservePressure) {
        if (playoutShouldOwnSurplus) {
            decision.reason = "uniform_playout_soft_reserve";
            return decision;
        }
        decision.accept = false;
        decision.decimated = true;
        decision.reason =
            decision.hardReservePressure ? "wgc_ingress_decimated_hard_reserve" : "wgc_ingress_decimated_soft_reserve";
        return decision;
    }

    const bool retainedHigh = retainedFrames >= (retainedFrameCap - 1u);
    if (retainedHigh && admissionCreditFrames < 1.0) {
        if (playoutShouldOwnSurplus) {
            decision.reason = "uniform_playout_credit";
            return decision;
        }
        decision.accept = false;
        decision.decimated = true;
        decision.reason = "wgc_ingress_decimated_credit";
        return decision;
    }

    decision.reason = retainedHigh ? "credit" : "healthy";
    return decision;
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
    if (rawSelectionQpc > 0 && IsWgcFrameTooNewForActiveDelayHardLimit(rawSelectionQpc, selectionTargetQpc,
                                                                       targetIntervalTicks, qpcTicksPerSecond)) {
        return false;
    }
    return true;
}

inline uint32_t GetWgcActiveDelayFinalSelectionLateResidualUs(int64_t predictedSelectionQpc, int64_t rawSelectionQpc,
                                                              int64_t selectionTargetQpc, int64_t qpcTicksPerSecond) {
    if (selectionTargetQpc <= 0 || qpcTicksPerSecond <= 0) {
        return 0;
    }
    const int64_t predictedLateQpc =
        predictedSelectionQpc > selectionTargetQpc ? (predictedSelectionQpc - selectionTargetQpc) : 0;
    const int64_t rawLateQpc = rawSelectionQpc > selectionTargetQpc ? (rawSelectionQpc - selectionTargetQpc) : 0;
    const int64_t lateQpc = std::max(predictedLateQpc, rawLateQpc);
    if (lateQpc <= 0) {
        return 0;
    }
    const uint64_t lateUs = (static_cast<uint64_t>(lateQpc) * 1000000ull) / static_cast<uint64_t>(qpcTicksPerSecond);
    return lateUs > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(lateUs);
}

inline bool IsWgcActiveDelayFinalSelectionWithinSoftLateTarget(int64_t predictedSelectionQpc, int64_t rawSelectionQpc,
                                                               int64_t selectionTargetQpc, int64_t targetIntervalTicks,
                                                               int64_t qpcTicksPerSecond, uint32_t softLateTargetUs) {
    if (!IsWgcActiveDelayFinalSelectionWithinHardLimit(predictedSelectionQpc, rawSelectionQpc, selectionTargetQpc,
                                                       targetIntervalTicks, qpcTicksPerSecond)) {
        return false;
    }
    return GetWgcActiveDelayFinalSelectionLateResidualUs(predictedSelectionQpc, rawSelectionQpc, selectionTargetQpc,
                                                         qpcTicksPerSecond) <= softLateTargetUs;
}

inline int64_t GetWgcActiveDelayRepeatClusterPenaltyQpc(uint32_t repeatClusterTicks, int64_t targetIntervalTicks) {
    if (repeatClusterTicks == 0 || targetIntervalTicks <= 0) {
        return 0;
    }

    const uint32_t penaltyPermille =
        std::min<uint32_t>(repeatClusterTicks * kWgcActiveDelayRepeatClusterPenaltyPermille,
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
    if (windowClass == WgcActiveDelayWindowClass::kPostStallRecovery && candidateLateResidualUs <= softLateTargetUs) {
        return true;
    }
    if (!IsWgcActiveDelaySourceLimitedClass(windowClass) && candidateLateResidualUs > softLateTargetUs) {
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
    int64_t candidateSelectionQpc, int64_t repeatSelectionQpc, int64_t selectionTargetQpc, int64_t targetIntervalTicks,
    int64_t qpcTicksPerSecond, uint32_t repeatClusterTicks = 0, uint32_t residualAvgAbsUs = 0,
    uint32_t residualP95Us = 0, uint32_t residualLateMaxUs = 0,
    WgcActiveDelayWindowClass windowClass = WgcActiveDelayWindowClass::kSourceLimited,
    uint32_t softLateTargetUs = kWgcActiveDelaySoftLateTargetMaxUs) {
    WgcActiveDelayRelaxedCandidateScore result{};
    if (candidateSelectionQpc <= 0 || repeatSelectionQpc <= 0 || selectionTargetQpc <= 0 || targetIntervalTicks <= 0) {
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
        result.candidateLateResidualUs =
            candidateLateUs > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(candidateLateUs);
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
    result.repeatClusterPenaltyQpc = GetWgcActiveDelayRepeatClusterPenaltyQpc(repeatClusterTicks, targetIntervalTicks);
    result.repeatBudgetQpc = INT64_MAX - result.repeatDamageQpc < result.repeatClusterPenaltyQpc
                                 ? INT64_MAX
                                 : result.repeatDamageQpc + result.repeatClusterPenaltyQpc;

    if (result.candidateDamageQpc < result.repeatDamageQpc) {
        result.decision = WgcActiveDelayRelaxedDecision::kAcceptBetterTarget;
        return result;
    }
    if (!IsWgcActiveDelaySourceLimitedClass(windowClass) && result.candidateLateResidualUs <= softLateTargetUs &&
        qpcTicksPerSecond > 0) {
        const uint64_t softBudgetQpc =
            (static_cast<uint64_t>(softLateTargetUs) * static_cast<uint64_t>(qpcTicksPerSecond)) / 1000000ull;
        const int64_t softRepeatBudgetQpc = INT64_MAX - result.repeatDamageQpc < static_cast<int64_t>(softBudgetQpc)
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

inline bool IsWgcActiveDelayRelaxedCandidateUseful(
    int64_t candidateSelectionQpc, int64_t repeatSelectionQpc, int64_t selectionTargetQpc, int64_t targetIntervalTicks,
    int64_t qpcTicksPerSecond, uint32_t repeatClusterTicks = 0, uint32_t residualAvgAbsUs = 0,
    uint32_t residualP95Us = 0, uint32_t residualLateMaxUs = 0,
    WgcActiveDelayWindowClass windowClass = WgcActiveDelayWindowClass::kSourceLimited,
    uint32_t softLateTargetUs = kWgcActiveDelaySoftLateTargetMaxUs) {
    return ScoreWgcActiveDelayRelaxedCandidate(
               candidateSelectionQpc, repeatSelectionQpc, selectionTargetQpc, targetIntervalTicks, qpcTicksPerSecond,
               repeatClusterTicks, residualAvgAbsUs, residualP95Us, residualLateMaxUs, windowClass, softLateTargetUs)
        .Accepted();
}

inline WgcActiveDelayRelaxedCandidateScore ScoreWgcActiveDelayRepeatRescueCandidate(
    int64_t candidateSelectionQpc, int64_t candidateRawSelectionQpc, int64_t repeatSelectionQpc,
    int64_t selectionTargetQpc, int64_t targetIntervalTicks, int64_t qpcTicksPerSecond, uint32_t repeatClusterTicks = 0,
    uint32_t residualAvgAbsUs = 0, uint32_t residualP95Us = 0, uint32_t residualLateMaxUs = 0,
    WgcActiveDelayWindowClass windowClass = WgcActiveDelayWindowClass::kSourceLimited,
    uint32_t softLateTargetUs = kWgcActiveDelaySoftLateTargetMaxUs) {
    WgcActiveDelayRelaxedCandidateScore result{};
    if (candidateSelectionQpc <= 0 || repeatSelectionQpc <= 0 || selectionTargetQpc <= 0 || targetIntervalTicks <= 0) {
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
        result.candidateLateResidualUs =
            candidateLateUs > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(candidateLateUs);
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
    result.repeatClusterPenaltyQpc = GetWgcActiveDelayRepeatClusterPenaltyQpc(repeatClusterTicks, targetIntervalTicks);
    result.repeatBudgetQpc = INT64_MAX - result.repeatDamageQpc < result.repeatClusterPenaltyQpc
                                 ? INT64_MAX
                                 : result.repeatDamageQpc + result.repeatClusterPenaltyQpc;

    if (result.candidateDamageQpc < result.repeatDamageQpc) {
        result.decision = WgcActiveDelayRelaxedDecision::kAcceptBetterTarget;
        return result;
    }
    if (!IsWgcActiveDelaySourceLimitedClass(windowClass) && result.candidateLateResidualUs <= softLateTargetUs &&
        qpcTicksPerSecond > 0) {
        const uint64_t softBudgetQpc =
            (static_cast<uint64_t>(softLateTargetUs) * static_cast<uint64_t>(qpcTicksPerSecond)) / 1000000ull;
        const int64_t softRepeatBudgetQpc = INT64_MAX - result.repeatDamageQpc < static_cast<int64_t>(softBudgetQpc)
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

inline bool IsWgcCfrSmoothnessNotMaximal(uint32_t totalOutputTicks, uint32_t excessRepeats, uint32_t policyAddedRepeats,
                                         uint32_t excessRepeatClusterMaxTicks, uint32_t postSelectionRejectedSync) {
    if (postSelectionRejectedSync > 0) {
        return true;
    }
    if (excessRepeatClusterMaxTicks >= kWgcCfrSmoothnessExcessRepeatClusterFaultTicks) {
        return true;
    }
    if (policyAddedRepeats >= kWgcCfrSmoothnessExcessRepeatFaultMinCount) {
        return true;
    }
    if (totalOutputTicks > 0 && policyAddedRepeats >= kWgcCfrSmoothnessPolicyRepeatNoticeMinCount) {
        const uint64_t policyPermille =
            (static_cast<uint64_t>(policyAddedRepeats) * 1000ull) / static_cast<uint64_t>(totalOutputTicks);
        if (policyPermille >= kWgcCfrSmoothnessPolicyRepeatNoticePermille) {
            return true;
        }
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

// Uniform-cadence mode is active when an A/V content delay is being applied to the WGC selection
// (selectionDelayApplied) AND the config opts in. In this mode WGC paces the active-delay output
// like the inject path: it keeps a frame-count delay floor and advances unique frames at the
// SOURCE input rate (Bresenham), so a VRR/under-delivering source (present rate dipping below the
// output target) keeps the buffer fed and the ~delay-old frame available, with the unavoidable
// source-limited repeats distributed evenly instead of clustered into delay-slot holds. This
// preserves A/V sync (the realized delay stays ~floor frames deep) while staying smooth.
inline bool IsWgcActiveDelayUniformCadenceMode(bool selectionDelayApplied, bool uniformCadenceConfigEnabled) {
    return selectionDelayApplied && uniformCadenceConfigEnabled;
}

// Result of the inject-parity input-rate pacing decision for the WGC active-delay floor model.
struct WgcActiveDelayPaceResult {
    bool advance = false;            // pop one unique frame for this output tick
    uint32_t dropBeforeAdvance = 0;  // even-decimation drops when the source outran the output
    uint32_t capDrops = 0;           // subset of dropBeforeAdvance forced by the reservoir depth cap
    double creditConsumed = 0.0;     // amount to subtract from the running credit accumulator
};

// Maximum buffered depth the uniform-cadence active-delay pacer allows before trimming the oldest
// surplus. The realized content delay is ~(depth-1) source-intervals (the emitted/oldest frame is
// behind the newest by depth-1 frames), so the reservoir target depth (floor+extra) realizes the
// floor-deep delay; the cap is that target plus a small jitter band. Bounding the depth bounds the
// realized content delay, which is the fix for the unbounded inflation observed when a VRR source
// transiently delivers above the output rate (realized delay drifting 31ms -> 248ms).
inline size_t GetWgcActiveDelayPaceMaxDepthFrames(int64_t contentDelayQpc, int64_t targetIntervalTicks) {
    const uint32_t targetFrames = GetWgcDelayReservoirTargetFrames(contentDelayQpc, targetIntervalTicks);
    const size_t target = targetFrames > 0 ? static_cast<size_t>(targetFrames) : 1u;
    return target + static_cast<size_t>(kWgcActiveDelayPaceMaxExcessFrames);
}

// Decide whether to advance/drop/hold for one WGC active-delay output tick. Mirrors the inject
// Bresenham pacer: with the credit already incremented by the source unique-frames-per-tick rate,
// decimate evenly while the source is ahead (credit >= 2, keep floor+1), advance one unique frame
// when credit >= 1 and the buffer is above the delay floor, otherwise hold (an evenly distributed
// source-limited repeat). The delay floor (frames kept) is what realizes the content delay, so the
// emitted frame is always ~floor source-frames old; it can never be "too new" for the slot, which
// is why this path needs no per-tick reserve defense.
//
// Setpoint restoring drain (maxDepthFrames): the pure source-rate matcher has NO restoring force
// toward the floor, so any transient where the source outran the output (a VRR / GPU-bound present
// rate briefly above the CFR target) inflates the buffer and the inflation never drains back -- the
// realized content delay then drifts upward without bound until a starvation event empties it. Trim
// the oldest excess down to maxDepthFrames every tick so the realized delay is pinned at ~floor.
// Because the surplus arrives gradually (a fraction of a frame per tick), this trims evenly and
// stays smooth; it never trims into floor+1, so it cannot starve the delay reserve.
inline WgcActiveDelayPaceResult DecideWgcActiveDelayPace(double creditAfterIncrement, size_t bufferedFrames,
                                                         size_t floorFrames, size_t maxDepthFrames) {
    WgcActiveDelayPaceResult result;
    size_t available = bufferedFrames;
    const size_t depthCap = std::max(maxDepthFrames, floorFrames + 1);
    while (available > depthCap) {
        ++result.dropBeforeAdvance;
        ++result.capDrops;
        --available;
    }
    while ((creditAfterIncrement - result.creditConsumed) >= 2.0 && available > floorFrames + 1) {
        ++result.dropBeforeAdvance;
        --available;
        result.creditConsumed += 1.0;
    }
    if ((creditAfterIncrement - result.creditConsumed) >= 1.0 && available > floorFrames) {
        result.advance = true;
        result.creditConsumed += 1.0;
    }
    return result;
}

// Delay floor (frames retained ahead of the emitted frame) for the inject-parity WGC pacer. This
// is the WGC analogue of injectContentDelayFrames; the realized content delay is ~floor source
// frames. Reuses the existing reservoir delay-frame derivation so WGC and the timestamp-target
// path agree on the nominal depth.
inline size_t GetWgcActiveDelayPaceFloorFrames(int64_t contentDelayQpc, int64_t targetIntervalTicks) {
    const uint32_t delayFrames = GetWgcDelayReservoirDelayFrames(contentDelayQpc, targetIntervalTicks);
    return delayFrames > 0 ? static_cast<size_t>(delayFrames) : 1u;
}

// Final reserve-defense decision used by the WGC active-delay selector. The older (reserve-
// building) frame is preferred over the closest-to-target frame only when reserve defense is in
// effect; uniform-cadence mode disables it so an under-delivering source does not get its cadence
// perturbed by per-tick older-frame selection.
inline bool ShouldPreferEarlierFreshWgcFrameForReserveDefense(int64_t earlierFrameTimestampQpc,
                                                              int64_t selectedFrameTimestampQpc,
                                                              int64_t selectionTargetQpc, int64_t targetIntervalTicks,
                                                              bool reservePressureActive, bool lowSourceMode,
                                                              bool deepUnderfeed, bool liveRecoveryMode,
                                                              bool uniformCadenceMode) {
    if (uniformCadenceMode) {
        return false;
    }
    return ShouldPreferEarlierFreshWgcFrameToPreserveReserve(
        earlierFrameTimestampQpc, selectedFrameTimestampQpc, selectionTargetQpc, targetIntervalTicks,
        reservePressureActive, lowSourceMode, deepUnderfeed, liveRecoveryMode);
}

// ---- WGC active-delay nearest-target playout (fixed-latency jitter-buffer resampling) -------------
//
// The Bresenham source-rate pacer (DecideWgcActiveDelayPace) emits the OLDEST buffered frame and
// bounds the buffer by COUNT (the depth cap). Under bursty WGC delivery -- DWM hands frames to the
// capture frame pool in late batches even when the game itself presents perfectly smoothly (real
// signature: game present max-interval ~10 ms while WGC delivery dips to 24-110 fps with 170-200 ms
// callback gaps) -- that count/oldest model produces two visible defects:
//   1. The count-based cap dumps a whole delivered burst in a single output tick (a cluster of drops)
//      and then the buffer runs dry across the following delivery lull (a cluster of repeats), so a
//      ~115 fps source manufactures ~20 dups AND ~14 drops in the SAME second. That simultaneous
//      drop+dup churn is far harsher than uniform CFR judder.
//   2. Oldest-first emission rubber-bands the realized content delay: the oldest frame's age is ~0 ms
//      right after a burst and hundreds of ms at the end of a lull (observed realizedDelay 0..243 ms
//      against a 30 ms target), which is also a latent A/V-sync defect.
//
// Nearest-target playout treats the buffer as a fixed-latency jitter buffer. Each output tick selects
// the buffered frame nearest the playout target (gridTickQpc - contentDelayQpc): it advances the
// front over frames the audio timeline has already passed (older than the target, when a strictly
// newer not-too-new successor exists), then emits the front if its slot has aged in, otherwise holds.
// Frames newer than the target stay buffered as future reserve. Because emission position is keyed to
// the timestamp grid (not a count), this consumes unique frames at the SOURCE rate by construction --
// repeating evenly when the source is below output, decimating evenly when above -- so it neither
// over-drains like the old grid-rate timestamp-target reservoir (which force-advanced one unique
// frame per OUTPUT tick and clustered "too new" holds) nor clusters, and the realized delay is pinned
// near contentDelayQpc regardless of delivery burstiness. After a true delivery gap it resumes at the
// correct delay by DROPPING the audio-passed backlog (replaying it would put video behind audio)
// rather than replaying it as a rubber-band; the unavoidable in-gap freeze stays a clean freeze.
//
// `leadToleranceQpc` is how far past (newer than) the target a frame may be and still count as the
// slot frame -- reuse GetWgcActiveDelayResidualToleranceQpc so the playout boundary matches the
// active-delay "too new for slot" boundary used elsewhere.

// Should the current front be dropped in favour of `nextTimestampQpc`? True when the successor is
// strictly newer (monotonic safety) and is still at-or-before the playout slot within the lead
// tolerance, i.e. it is a closer representative of the target than the older front, so the front is
// already-past surplus history. Stops naturally at the newest not-too-new frame, leaving any future
// frames as reserve.
inline bool ShouldDropWgcFrontForNearerPlayout(int64_t frontTimestampQpc, int64_t nextTimestampQpc,
                                               int64_t playoutTargetQpc, int64_t leadToleranceQpc) {
    if (frontTimestampQpc <= 0 || nextTimestampQpc <= 0 || playoutTargetQpc <= 0) {
        return false;
    }
    if (nextTimestampQpc <= frontTimestampQpc) {
        return false;  // not strictly newer -> never advance past (duplicate/non-monotonic safety)
    }
    return nextTimestampQpc <= playoutTargetQpc + leadToleranceQpc;
}

inline bool ShouldSkipDeliveredDuplicateWgcSourceTimestamp(bool duplicateSourceTimestamp, int64_t rawSourceFrameQpc,
                                                           int64_t lastDeliveredRawSourceQpc,
                                                           bool cfrCaptureActive) {
    if (!cfrCaptureActive || !duplicateSourceTimestamp || rawSourceFrameQpc <= 0 || lastDeliveredRawSourceQpc <= 0) {
        return false;
    }
    return rawSourceFrameQpc == lastDeliveredRawSourceQpc;
}

struct WgcNearestPlayoutDecision {
    bool emit = false;  // pop and emit the (post-stale-drop) front frame for this slot
    bool hold = false;  // repeat the previous frame: the slot frame has not been delivered yet
};

// After stale-dropping, decide what to do with the front frame for this output tick. Emit when it has
// aged into the slot (timestamp <= target + leadTolerance) and is strictly newer than the last
// emitted frame (monotonic). Otherwise hold -- the slot frame is still in the future, so leave the
// newer buffered frames as reserve and repeat the previous frame (an evenly distributed
// source-limited / delivery-gap repeat). A lone frame older than the target is still emitted (it is
// the freshest available content and strictly newer than the last emit), which makes an in-gap freeze
// a clean monotonic hold instead of a backward rubber-band.
inline WgcNearestPlayoutDecision DecideWgcNearestPlayout(int64_t frontTimestampQpc, int64_t playoutTargetQpc,
                                                         int64_t leadToleranceQpc, int64_t lastEmittedTimestampQpc) {
    WgcNearestPlayoutDecision decision;
    if (frontTimestampQpc <= 0 || playoutTargetQpc <= 0) {
        return decision;  // no usable timing -> caller falls back / holds
    }
    if (frontTimestampQpc <= lastEmittedTimestampQpc) {
        decision.hold = true;  // would be non-monotonic -> repeat
        return decision;
    }
    if (frontTimestampQpc <= playoutTargetQpc + leadToleranceQpc) {
        decision.emit = true;
    } else {
        decision.hold = true;  // front still in the future beyond tolerance -> slot not aged in
    }
    return decision;
}

// Anti-freeze floor for the uniform active-delay playout slot target.
//
// The uniform playout target is grid-anchored (gridSlot - contentDelay) and intentionally UNCLAMPED so
// the realized content delay is held through low-source / live-recovery. But when the CFR encoder grid
// falls behind wall-clock and cannot repay the deficit (sustained encoder overload: the grid->wall
// resync is gated off while a shortfall exists, and the per-loop rebase is capped), the grid-anchored
// target drifts arbitrarily far behind the real-time WGC frame timestamps. Once it has drifted behind
// the ENTIRE reserve -- i.e. even the OLDEST buffered frame is "too new for the slot"
// (IsWgcFrameTooNewForCfrSlot) -- DecideWgcNearestPlayout holds every tick: the last frame repeats
// forever while fresh frames pile up and drop as stale. Observed as a multi-second hard video freeze
// (build 0.1.4402: a 20.9 s / 2510-tick contiguous duplicate run under 4K120 AV1 NVENC overload) with
// only the composited cursor moving.
//
// When (and only when) even the oldest reserve frame is too-new for the slot, raise the slot target
// just enough to age that oldest frame in, so playout resumes at the SOURCE rate against the deepest
// reserve frame. The caller must first verify that the oldest frame is still old enough to preserve the
// active content delay; otherwise a source-underfilled reservoir would be mistaken for encoder-grid
// drift and the realized A/V delay would collapse toward live. This helper is a strict no-op in healthy
// cadence: the oldest reserve frame is normally OLDER than the slot (it never trips the too-new
// boundary), so the grid target stays authoritative and the realized delay is unchanged. The too-new
// boundary (kWgcCfrSelectionMaxLeadTicks) sits well beyond source delivery jitter, so normal bursty
// delivery cannot toggle the floor -- it engages only on a genuine sustained grid drift.
inline int64_t ApplyWgcUniformPlayoutAntiFreezeFloor(int64_t playoutTargetQpc, int64_t oldestBufferedSlotQpc,
                                                     int64_t targetIntervalTicks,
                                                     uint32_t maxLeadTicks = kWgcCfrSelectionMaxLeadTicks) {
    if (playoutTargetQpc <= 0 || oldestBufferedSlotQpc <= 0 || targetIntervalTicks <= 0) {
        return playoutTargetQpc;
    }
    if (!IsWgcFrameTooNewForCfrSlot(oldestBufferedSlotQpc, playoutTargetQpc, targetIntervalTicks, maxLeadTicks)) {
        return playoutTargetQpc;  // oldest reserve frame still ages into the slot -> healthy, no-op
    }
    // Land the oldest frame exactly on the emit boundary (target + tolerance): DecideWgcNearestPlayout
    // and ShouldDropWgcFrontForNearerPlayout both use the SAME tolerance and a `<=` comparison, so the
    // oldest frame is emitted and no newer reserve frame is dropped (max delay retained). Never lower
    // the target (std::max) so a mis-ordered caller cannot collapse the delay.
    const int64_t agedInTargetQpc = oldestBufferedSlotQpc - GetWgcActiveDelayResidualToleranceQpc(targetIntervalTicks);
    return std::max(playoutTargetQpc, agedInTargetQpc);
}

inline bool IsWgcUniformPlayoutAntiFreezeFloorSyncSafe(int64_t oldestBufferedAgeQpc, int64_t contentDelayQpc,
                                                       int64_t targetIntervalTicks) {
    if (contentDelayQpc <= 0) {
        return true;
    }
    if (oldestBufferedAgeQpc <= 0 || targetIntervalTicks <= 0) {
        return false;
    }
    const int64_t residualToleranceQpc = GetWgcActiveDelayResidualToleranceQpc(targetIntervalTicks);
    return oldestBufferedAgeQpc + residualToleranceQpc >= contentDelayQpc;
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
