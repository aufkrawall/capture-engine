#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <cmath>

#include "constants.h"

// CFR tick accounting, catch-up budgets, and WGC selection delay.

namespace ce::capture_policy {

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
    (void)useScreenGrab;
    if (useVFR) {
        return false;
    }

    // Every accrued CFR tick is part of the immutable output timeline.  A
    // backend-specific stop path must not silently discard late ticks; it may
    // repeat the newest captured state to close the contiguous prefix.
    return true;
}

inline bool ShouldAbortCfrStopDrainBeforeOutputIsLive(bool recording, bool recordingOutputLive,
                                                      bool drainOutstandingCfrTicks) {
    // A recording that never committed its first video frame has no CFR output
    // prefix or accrued output debt to close.  Keeping the drain armed in this
    // state also prevents the encoder worker from observing shutdown.
    return !recording && !recordingOutputLive && drainOutstandingCfrTicks;
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
    return queuedWgcFrameAvailable || bufferedWgcFrameAvailable || (hasLastFrame && mediaEngineCanRepeatLastFrame);
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

// A non-deferred fresh-frame failure must not punch a hole in an already
// scheduled CFR timeline. If the encoder still owns a cache from the previous
// successfully emitted frame, the caller can emit that content at the exact
// scheduled timestamp and account it as a duplicate. Deferred inject frames
// remain on their retry path and must not be mistaken for hard failures.
inline bool ShouldRepeatAfterScheduledFreshEncodeFailure(bool scheduledCfrTick, bool freshEncodeSucceeded,
                                                         bool freshEncodeDeferred, bool repeatPathAvailable,
                                                         bool repeatCacheAvailable) {
    return scheduledCfrTick && !freshEncodeSucceeded && !freshEncodeDeferred && repeatPathAvailable &&
           repeatCacheAvailable;
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

inline uint32_t GetWgcFreshCatchupBudgetThisLoop(uint32_t catchupTicksThisLoop, bool encoderBottlenecked,
                                                 bool encoderActivelyTooSlow, double freshServiceMs,
                                                 double frameIntervalMs, size_t bufferedWgcFrames,
                                                 size_t reserveFrames) {
    if (catchupTicksThisLoop <= 1u || encoderBottlenecked || encoderActivelyTooSlow ||
        !std::isfinite(freshServiceMs) || !std::isfinite(frameIntervalMs) || freshServiceMs <= 0.0 ||
        frameIntervalMs <= 0.0 ||
        freshServiceMs >= frameIntervalMs * kWgcFreshCatchupServiceBudgetRatio ||
        bufferedWgcFrames <= reserveFrames) {
        return 0u;
    }

    // Only reservoir surplus can replace held recovery slots. The caller still
    // validates each candidate against that slot's immutable content-time
    // target, so this changes pixels only: CFR PTS and audio remain untouched.
    const size_t surplusFrames = bufferedWgcFrames - reserveFrames;
    return static_cast<uint32_t>(
        std::min<size_t>(static_cast<size_t>(catchupTicksThisLoop - 1u), surplusFrames));
}

struct WgcOverloadRepeatPacerState {
    bool active = false;
    double freshCredit = 0.0;
    double freshFraction = 1.0;
    double minimumFreshFraction = 1.0;
    uint32_t recoveryConfirmTicks = 0;
    uint32_t consecutiveProactiveRepeats = 0;
    uint32_t maxConsecutiveProactiveRepeats = 0;
    uint64_t episodes = 0;
    uint64_t proactiveRepeats = 0;
    uint64_t emittedRepeats = 0;
    uint64_t freshGrants = 0;

    void ResetActivePacing() {
        active = false;
        freshCredit = 0.0;
        freshFraction = 1.0;
        recoveryConfirmTicks = 0;
        consecutiveProactiveRepeats = 0;
    }
};

struct WgcOverloadRepeatPacerDecision {
    bool active = false;
    bool repeat = false;
    bool entered = false;
    bool exited = false;
    double freshFraction = 1.0;
    double serviceBudgetMs = 0.0;
    const char* reason = "inactive";
};

inline WgcOverloadRepeatPacerDecision UpdateWgcOverloadRepeatPacer(
    WgcOverloadRepeatPacerState& state, bool liveCfr, bool sourceHealthy, bool capacityPressure,
    bool freshCandidateAvailable, bool repeatAvailable, double freshServiceMs, double repeatServiceMs,
    double frameIntervalMs, uint32_t freshServiceSamples, uint32_t repeatServiceSamples) {
    WgcOverloadRepeatPacerDecision decision{};
    decision.serviceBudgetMs =
        std::isfinite(frameIntervalMs) && frameIntervalMs > 0.0
            ? frameIntervalMs * kWgcOverloadRepeatPacerBudgetRatio
            : 0.0;

    const auto deactivate = [&](const char* reason) {
        decision.exited = state.active;
        decision.freshFraction = state.freshFraction;
        decision.reason = reason;
        state.ResetActivePacing();
    };
    if (!liveCfr) {
        deactivate("not_live_cfr");
        return decision;
    }
    if (!sourceHealthy) {
        deactivate("source_not_healthy");
        return decision;
    }
    if (!repeatAvailable) {
        deactivate("repeat_unavailable");
        return decision;
    }
    if (freshServiceSamples < kWgcOverloadRepeatPacerMinSamples ||
        repeatServiceSamples < kWgcOverloadRepeatPacerMinSamples) {
        deactivate("warming_service_samples");
        return decision;
    }
    if (!std::isfinite(freshServiceMs) || !std::isfinite(repeatServiceMs) ||
        decision.serviceBudgetMs <= 0.0 || freshServiceMs <= 0.0 || repeatServiceMs <= 0.0) {
        deactivate("invalid_service_samples");
        return decision;
    }

    if (repeatServiceMs < frameIntervalMs) {
        const double marginalBudgetMs =
            repeatServiceMs +
            (frameIntervalMs - repeatServiceMs) * kWgcOverloadRepeatPacerMarginalHeadroomUseRatio;
        decision.serviceBudgetMs = std::max(decision.serviceBudgetMs, marginalBudgetMs);
    }
    const double minimumAdvantageMs = frameIntervalMs * kWgcOverloadRepeatPacerMinAdvantageRatio;
    const bool repeatHasUsefulAdvantage = freshServiceMs - repeatServiceMs >= minimumAdvantageMs;
    if (!repeatHasUsefulAdvantage) {
        deactivate("repeat_not_cheaper");
        return decision;
    }
    const bool freshNeedsPacing = freshServiceMs > decision.serviceBudgetMs;
    if (state.active && !freshNeedsPacing) {
        if (++state.recoveryConfirmTicks >= kWgcOverloadRepeatPacerRecoveryConfirmTicks) {
            deactivate("service_recovered");
            return decision;
        }
    } else {
        state.recoveryConfirmTicks = 0;
    }
    if (!state.active && !capacityPressure) {
        decision.reason = "capacity_not_confirmed";
        return decision;
    }
    if (!state.active && !freshNeedsPacing) {
        decision.reason = "fresh_within_budget";
        return decision;
    }

    double freshFraction =
        std::clamp((decision.serviceBudgetMs - repeatServiceMs) / (freshServiceMs - repeatServiceMs), 0.0, 1.0);
    if (repeatServiceMs >= frameIntervalMs) {
        // No real-time mix exists. Repeats are still the least expensive work,
        // but preserve bounded visual liveness so an intermittent episode can
        // recover without an intentionally unbounded static run.
        freshFraction = std::max(freshFraction, kWgcOverloadRepeatPacerDegradedFreshFraction);
        decision.reason = "degraded_repeat_over_interval";
    }
    if (!state.active) {
        state.active = true;
        state.freshCredit = 1.0 - freshFraction;
        ++state.episodes;
        decision.entered = true;
    }
    state.freshFraction = freshFraction;
    state.minimumFreshFraction = std::min(state.minimumFreshFraction, freshFraction);
    decision.active = true;
    decision.freshFraction = freshFraction;
    if (repeatServiceMs < frameIntervalMs) {
        decision.reason = freshNeedsPacing ? "pacing" : "recovery_hysteresis";
    }

    if (!freshCandidateAvailable) {
        // A natural source hold already paid for a cheap slot. Preserve at most
        // one fresh-frame credit so the next covered slot is not needlessly held.
        state.freshCredit = std::min(1.0, state.freshCredit + freshFraction);
        state.consecutiveProactiveRepeats = 0;
        return decision;
    }

    state.freshCredit += freshFraction;
    if (state.freshCredit + 1e-12 >= 1.0) {
        state.freshCredit -= 1.0;
        state.consecutiveProactiveRepeats = 0;
        ++state.freshGrants;
        return decision;
    }

    decision.repeat = true;
    ++state.proactiveRepeats;
    ++state.consecutiveProactiveRepeats;
    state.maxConsecutiveProactiveRepeats =
        std::max(state.maxConsecutiveProactiveRepeats, state.consecutiveProactiveRepeats);
    return decision;
}

inline void UpdateWgcServiceTimeEma(double wallServiceMs, double pureServiceMs, double smoothingAlpha,
                                    double& smoothedServiceMs, uint32_t& serviceSamples) {
    const double serviceMs = std::max(wallServiceMs, pureServiceMs);
    if (!std::isfinite(serviceMs) || serviceMs <= 0.0 || !std::isfinite(smoothingAlpha) ||
        smoothingAlpha <= 0.0 || smoothingAlpha > 1.0) {
        return;
    }
    smoothedServiceMs = smoothedServiceMs == 0.0
                            ? serviceMs
                            : smoothedServiceMs * (1.0 - smoothingAlpha) + serviceMs * smoothingAlpha;
    serviceSamples = serviceSamples < UINT32_MAX ? serviceSamples + 1u : UINT32_MAX;
}

inline uint32_t GetWgcCatchupTicksThisLoop(bool encoderBottlenecked, bool encoderActivelyTooSlow,
                                           size_t bufferedWgcFrames, double frameCreditAccumulator,
                                           uint32_t outputShortfallTicks, uint32_t outputFps,
                                           uint32_t recentDeliveredMin250Fps, uint32_t recentInputMin250Fps,
                                           uint32_t noFreshTickPermille, bool lowSourceMode,
                                           double audioLeadExcessMs = 0.0) {
    (void)encoderActivelyTooSlow;
    (void)bufferedWgcFrames;
    (void)frameCreditAccumulator;
    (void)outputFps;
    (void)recentDeliveredMin250Fps;
    (void)recentInputMin250Fps;
    (void)noFreshTickPermille;
    (void)lowSourceMode;
    (void)audioLeadExcessMs;
    // A WGC/DXGI worker wake and a CFR output slot are different clocks. Once
    // the worker owes at least two output slots, service one additional recovery
    // slot without moving the wake deadline. At severe debt the existing
    // four-slot bound permits up to three recovery slots. Grid-matched buffered
    // history may replace a hold only under the separate service-headroom and
    // reservoir policy; all other debt remains the smallest bounded visual
    // hold, without fast-forwarding video or trimming/resampling audio.
    return GetCfrCatchupTicksThisLoop(outputShortfallTicks, encoderBottlenecked);
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

inline double GetWgcForceCatchupBudgetFrameMultiplier(double /*shortfallDurationMs*/) {
    return 4.0;
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

}  // namespace ce::capture_policy
