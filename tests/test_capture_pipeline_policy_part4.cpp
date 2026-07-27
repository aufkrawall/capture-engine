#include "test_capture_pipeline_policy_shared.h"

#include <limits>

TEST(CapturePipelinePolicyTest, WgcActiveDelayRepeatRescueScoresSafeFramesBeforeRepeat) {
    const uint32_t softLateTargetUs = policy::GetWgcActiveDelaySoftLateTargetUs(8333, 1000000);

    auto score =
        policy::ScoreWgcActiveDelayRepeatRescueCandidate(106000, 105000, 90000, 100000, 8333, 1000000, 1, 0, 0, 0,
                                                         policy::WgcActiveDelayWindowClass::kHealthy, softLateTargetUs);
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kAcceptBetterTarget);
    EXPECT_TRUE(score.Accepted());

    score =
        policy::ScoreWgcActiveDelayRepeatRescueCandidate(106000, 106000, 100000, 100000, 8333, 1000000, 0, 0, 0, 0,
                                                         policy::WgcActiveDelayWindowClass::kHealthy, softLateTargetUs);
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kAcceptSoftRepeatAvoidance);
    EXPECT_TRUE(score.Accepted());
    EXPECT_STREQ(policy::WgcActiveDelayRelaxedDecisionToString(score.decision), "soft_repeat_avoidance");

    score =
        policy::ScoreWgcActiveDelayRepeatRescueCandidate(106000, 111000, 90000, 100000, 8333, 1000000, 1, 0, 0, 0,
                                                         policy::WgcActiveDelayWindowClass::kHealthy, softLateTargetUs);
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kRejectSyncRisk);
    EXPECT_FALSE(score.Accepted());

    score =
        policy::ScoreWgcActiveDelayRepeatRescueCandidate(109000, 109000, 80000, 100000, 8333, 1000000, 1, 0, 0, 7000,
                                                         policy::WgcActiveDelayWindowClass::kHealthy, softLateTargetUs);
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom);
    EXPECT_FALSE(score.Accepted());

    score = policy::ScoreWgcActiveDelayRepeatRescueCandidate(
        106000, 106000, 100000, 100000, 8333, 1000000, 0, 12000, 20000, 10000,
        policy::WgcActiveDelayWindowClass::kPostStallRecovery, softLateTargetUs);
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kAcceptSoftRepeatAvoidance);
    EXPECT_TRUE(score.Accepted());

    score = policy::ScoreWgcActiveDelayRepeatRescueCandidate(
        109000, 109000, 80000, 100000, 8333, 1000000, 1, 12000, 20000, 10000,
        policy::WgcActiveDelayWindowClass::kPostStallRecovery, softLateTargetUs);
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom);
    EXPECT_FALSE(score.Accepted());

    score = policy::ScoreWgcActiveDelayRepeatRescueCandidate(109000, 109000, 108000, 100000, 8333, 1000000, 0, 0, 0, 0,
                                                             policy::WgcActiveDelayWindowClass::kSourceLimited,
                                                             softLateTargetUs);
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kRejectRepeatCost);
    EXPECT_FALSE(score.Accepted());

    score = policy::ScoreWgcActiveDelayRepeatRescueCandidate(106000, 106000, 100000, 100000, 8333, 1000000, 0, 0, 0, 0,
                                                             policy::WgcActiveDelayWindowClass::kSourceLimited,
                                                             softLateTargetUs);
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kRejectRepeatCost);
    EXPECT_FALSE(score.Accepted());

    score = policy::ScoreWgcActiveDelayRepeatRescueCandidate(109000, 109000, 108000, 100000, 8333, 1000000, 2, 0, 0, 0,
                                                             policy::WgcActiveDelayWindowClass::kSourceLimited,
                                                             softLateTargetUs);
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kAcceptRepeatCluster);
    EXPECT_TRUE(score.Accepted());
}

TEST(CapturePipelinePolicyTest, WgcNearestPlayoutRepeatRescueCanUseSyncSafeFutureFrame) {
    const int64_t target = 100000;
    const int64_t interval = 8333;
    const int64_t qpcPerSecond = 1000000;
    const int64_t leadTol = policy::GetWgcActiveDelayResidualToleranceQpc(interval);
    const int64_t candidate = target + leadTol + 5;

    const auto playout = policy::DecideWgcNearestPlayout(candidate, target, leadTol, 90000);
    EXPECT_FALSE(playout.emit);
    EXPECT_TRUE(playout.hold);

    const uint32_t softLateTargetUs = policy::GetWgcActiveDelaySoftLateTargetUs(interval, qpcPerSecond);
    const auto rescueScore = policy::ScoreWgcActiveDelayRepeatRescueCandidate(
        candidate, candidate, target - interval, target, interval, qpcPerSecond, /*repeatClusterTicks=*/1, 0, 0, 0,
        policy::WgcActiveDelayWindowClass::kSourceLimited, softLateTargetUs);
    EXPECT_TRUE(rescueScore.Accepted());
    EXPECT_EQ(rescueScore.decision, policy::WgcActiveDelayRelaxedDecision::kAcceptBetterTarget);

    const auto unsafeRescue = policy::ScoreWgcActiveDelayRepeatRescueCandidate(
        target + 12000, target + 12000, target - interval, target, interval, qpcPerSecond,
        /*repeatClusterTicks=*/1, 0, 0, 0, policy::WgcActiveDelayWindowClass::kHealthy, softLateTargetUs);
    EXPECT_FALSE(unsafeRescue.Accepted());
    EXPECT_EQ(unsafeRescue.decision, policy::WgcActiveDelayRelaxedDecision::kRejectSyncRisk);
}

// End-to-end regression for the fortistutter bug (session 20260702, build 0.1.4427): a healthy
// 140 fps source into 120 fps CFR produced 22% repeats because the uniform active-delay playout
// (stale sweep + ShouldDropWgcFrontForNearerPlayout + DecideWgcNearestPlayout, media_main's
// useInjectParityDelayPacing loop) was slaved to RAW DWM composition timestamps, which arrive
// quantized under VRR/composed presentation. Recurring >8.3 ms artificial raw gaps starved output
// slots (hold) while the surrounding surplus was dropped. The SAME playout decisions driven by the
// monotonic bounded-deviation smoothed selection timestamps consume the surplus with zero repeats.
TEST(CapturePipelinePolicyTest, WgcUniformPlayoutUsesActualNearestSampleForRawAndSmoothedTimestamps) {
    constexpr int64_t kQpcFreq = 1000000;        // 1 tick == 1 us
    constexpr int64_t kOutputIntervalUs = 8333;  // 120 fps CFR
    constexpr double kTrueIntervalUs = 1000000.0 / 140.0;
    constexpr int64_t kBucketUs = 5000;  // composition clock granularity
    const int64_t leadToleranceUs = policy::GetWgcActiveDelayResidualToleranceQpc(kOutputIntervalUs);

    // True 140 fps cadence quantized UP to the composition clock (the fortistutter timestamp shape).
    std::vector<int64_t> raw;
    raw.reserve(1200);
    for (size_t i = 0; i < 1200; ++i) {
        const double trueTs = 1000000.0 + kTrueIntervalUs * static_cast<double>(i);
        const int64_t quantized = ((static_cast<int64_t>(trueTs) + kBucketUs - 1) / kBucketUs) * kBucketUs;
        raw.push_back(!raw.empty() && quantized <= raw.back() ? raw.back() : quantized);
    }

    InputFrameRatePredictor predictor;
    std::vector<int64_t> smoothed;
    smoothed.reserve(raw.size());
    for (const int64_t ts : raw) {
        predictor.Update(ts, kQpcFreq);
        smoothed.push_back(predictor.SmoothMonotonicTimestamp(ts, kOutputIntervalUs));
    }

    const auto runUniformPlayout = [&](const std::vector<int64_t>& selectionTs) -> int {
        // Skip the EMA warmup span, then replay the exact playout loop shape: the CFR read target
        // starts inside delivered content (the real pipeline holds a ~250 ms reservoir here).
        const size_t warmupFrames = 64;
        std::deque<int64_t> buffer(selectionTs.begin() + warmupFrames, selectionTs.end());
        int64_t target = selectionTs[warmupFrames] + leadToleranceUs;
        int64_t lastEmitted = 0;
        int holds = 0;
        // Stop while future frames remain buffered so end-of-stream cannot fake holds.
        const int64_t lastUsableTarget = selectionTs.back() - 4 * kOutputIntervalUs;
        for (; target <= lastUsableTarget; target += kOutputIntervalUs) {
            while (!buffer.empty() && lastEmitted > 0 && buffer.front() <= lastEmitted) {
                buffer.pop_front();  // stale sweep (already-emitted content)
            }
            while (buffer.size() > 1 &&
                   policy::ShouldDropWgcFrontForNearerPlayout(buffer[0], buffer[1], target, leadToleranceUs)) {
                buffer.pop_front();  // audio-passed surplus drop
            }
            if (buffer.empty()) {
                ++holds;
                continue;
            }
            const auto playout = policy::DecideWgcNearestPlayout(buffer.front(), target, leadToleranceUs, lastEmitted);
            if (playout.emit) {
                lastEmitted = buffer.front();
                buffer.pop_front();
            } else {
                ++holds;
            }
        }
        return holds;
    };

    const int rawHolds = runUniformPlayout(raw);
    const int smoothedHolds = runUniformPlayout(smoothed);

    // Actual nearest-neighbour selection no longer skips an exact/closer raw sample merely because
    // a later successor is inside WGC's broad compositor tolerance. Both domains therefore consume
    // this healthy surplus as pure decimation with no CE-manufactured repeat.
    EXPECT_EQ(rawHolds, 0);
    EXPECT_EQ(smoothedHolds, 0);
}

TEST(CapturePipelinePolicyTest, WgcActiveDelayFinalSelectionChecksPredictedAndRawTimestamps) {
    EXPECT_TRUE(policy::IsWgcActiveDelayFinalSelectionWithinHardLimit(109000, 108000, 100000, 8333, 1000000));
    EXPECT_FALSE(policy::IsWgcActiveDelayFinalSelectionWithinHardLimit(111000, 108000, 100000, 8333, 1000000));
    EXPECT_FALSE(policy::IsWgcActiveDelayFinalSelectionWithinHardLimit(108000, 111000, 100000, 8333, 1000000));
    EXPECT_TRUE(policy::IsWgcActiveDelayFinalSelectionWithinHardLimit(108000, 0, 100000, 8333, 1000000));
    EXPECT_EQ(policy::GetWgcActiveDelayFinalSelectionLateResidualUs(106000, 105000, 100000, 1000000), 6000u);
    EXPECT_TRUE(
        policy::IsWgcActiveDelayFinalSelectionWithinSoftLateTarget(106000, 105000, 100000, 8333, 1000000, 6249));
    EXPECT_FALSE(
        policy::IsWgcActiveDelayFinalSelectionWithinSoftLateTarget(109000, 105000, 100000, 8333, 1000000, 6249));
    EXPECT_FALSE(
        policy::IsWgcActiveDelayFinalSelectionWithinSoftLateTarget(111000, 105000, 100000, 8333, 1000000, 6249));
}

TEST(CapturePipelinePolicyTest, WgcActiveDelayRelaxedCandidateAccountsForRepeatClusterCost) {
    EXPECT_EQ(policy::GetWgcActiveDelayRepeatClusterPenaltyQpc(0, 100), 0);
    EXPECT_EQ(policy::GetWgcActiveDelayRepeatClusterPenaltyQpc(1, 100), 25);
    EXPECT_EQ(policy::GetWgcActiveDelayRepeatClusterPenaltyQpc(20, 100), 100);

    EXPECT_FALSE(policy::IsWgcActiveDelayRelaxedCandidateUseful(1100, 1090, 1000, 100, 1000000, 0));
    EXPECT_TRUE(policy::IsWgcActiveDelayRelaxedCandidateUseful(1100, 1090, 1000, 100, 1000000, 1));
    EXPECT_FALSE(policy::IsWgcActiveDelayRelaxedCandidateUseful(110001, 109000, 100000, 8333, 1000000, 4));
}

TEST(CapturePipelinePolicyTest, WgcActiveDelayMixedPolicyPressureUsesShareNotOnlyCount) {
    EXPECT_FALSE(policy::IsWgcActiveDelayMixedPolicyPressureFault(1291, 385, 1676));
    EXPECT_TRUE(policy::IsWgcActiveDelayMixedPolicyPressureFault(782, 322, 1104));
    EXPECT_TRUE(policy::IsWgcActiveDelayMixedPolicyPressureFault(0, 120, 120));
    EXPECT_FALSE(policy::IsWgcActiveDelayMixedPolicyPressureFault(20, 8, 28));
}

TEST(CapturePipelinePolicyTest, WgcCfrSourceRepeatLowerBoundCountsOnlyUnavoidableRepeats) {
    auto bound = policy::EstimateWgcCfrSourceRepeatLowerBound(120, 90, 35);
    EXPECT_EQ(bound.unavoidableRepeats, 30u);
    EXPECT_EQ(bound.excessRepeats, 5u);
    EXPECT_EQ(bound.excessPermille, 41u);

    bound = policy::EstimateWgcCfrSourceRepeatLowerBound(120, 100, 20);
    EXPECT_EQ(bound.unavoidableRepeats, 20u);
    EXPECT_EQ(bound.excessRepeats, 0u);

    bound = policy::EstimateWgcCfrSourceRepeatLowerBound(120, 110, 14);
    EXPECT_EQ(bound.unavoidableRepeats, 10u);
    EXPECT_EQ(bound.excessRepeats, 4u);

    bound = policy::EstimateWgcCfrSourceRepeatLowerBound(120, 119, 1);
    EXPECT_EQ(bound.unavoidableRepeats, 1u);
    EXPECT_EQ(bound.excessRepeats, 0u);

    bound = policy::EstimateWgcCfrSourceRepeatLowerBound(120, 140, 0);
    EXPECT_EQ(bound.unavoidableRepeats, 0u);
    EXPECT_EQ(bound.excessRepeats, 0u);
}

TEST(CapturePipelinePolicyTest, WgcCfrSmoothnessFaultUsesExcessRepeatsAndClusters) {
    EXPECT_FALSE(policy::IsWgcCfrSmoothnessNotMaximal(1200, 40, 20, 4, 0));
    EXPECT_TRUE(policy::IsWgcCfrSmoothnessNotMaximal(1200, 121, 0, 4, 0));
    EXPECT_TRUE(policy::IsWgcCfrSmoothnessNotMaximal(1200, 40, 120, 4, 0));
    EXPECT_TRUE(policy::IsWgcCfrSmoothnessNotMaximal(7000, 40, 82, 4, 0));
    EXPECT_TRUE(policy::IsWgcCfrSmoothnessNotMaximal(1200, 40, 0, 24, 0));
    EXPECT_TRUE(policy::IsWgcCfrSmoothnessNotMaximal(1200, 0, 0, 0, 1));
}

TEST(CapturePipelinePolicyTest, WgcDelayReservoirFramesFollowMeasuredDelay) {
    EXPECT_EQ(policy::GetWgcDelayReservoirDelayFrames(0, 100), 0u);
    EXPECT_EQ(policy::GetWgcDelayReservoirDelayFrames(301, 100), 4u);
    EXPECT_EQ(policy::GetWgcDelayReservoirLowWaterFrames(301, 100), 4u);
    EXPECT_EQ(policy::GetWgcDelayReservoirTargetFrames(301, 100), 5u);

    EXPECT_TRUE(policy::IsWgcDelayReservoirBelowLowWater(3, 301, 100));
    EXPECT_FALSE(policy::IsWgcDelayReservoirBelowLowWater(4, 301, 100));
    EXPECT_FALSE(policy::IsWgcDelayReservoirRecovered(4, 301, 100));
    EXPECT_TRUE(policy::IsWgcDelayReservoirRecovered(5, 301, 100));

    EXPECT_FALSE(policy::ShouldPreserveWgcStartupPartialReserve(1, 0, true, true));
    EXPECT_FALSE(policy::ShouldPreserveWgcStartupPartialReserve(4, 180, false, true));
    EXPECT_FALSE(policy::ShouldPreserveWgcStartupPartialReserve(4, 180, true, false));
    EXPECT_TRUE(policy::ShouldPreserveWgcStartupPartialReserve(4, 180, true, true));
}

TEST(CapturePipelinePolicyTest, WgcFreshCatchupRequiresGridMatchedMonotonicHistoricalFrame) {
    EXPECT_TRUE(policy::ShouldUseFreshWgcCatchupFrame(1000, 995, 995, 1000, 900, 900, 100));
    EXPECT_TRUE(policy::ShouldUseFreshWgcCatchupFrame(1050, 1100, 1090, 1000, 900, 900, 100));
    EXPECT_FALSE(policy::ShouldUseFreshWgcCatchupFrame(1051, 1000, 1000, 1000, 900, 900, 100));
    EXPECT_FALSE(policy::ShouldUseFreshWgcCatchupFrame(1000, 1101, 1000, 1000, 900, 900, 100));
    EXPECT_FALSE(policy::ShouldUseFreshWgcCatchupFrame(1000, 995, 900, 1000, 900, 900, 100));
    EXPECT_FALSE(policy::ShouldUseFreshWgcCatchupFrame(900, 895, 1000, 1000, 900, 900, 100));
    EXPECT_FALSE(policy::ShouldUseFreshWgcCatchupFrame(1000, 995, 1000, 0, 900, 900, 100));

    EXPECT_TRUE(policy::IsWgcFrameWithinLiveVisualDebtWindow(1350, 1600, 100, 1000));
    EXPECT_FALSE(policy::IsWgcFrameWithinLiveVisualDebtWindow(1349, 1600, 100, 1000));
}

TEST(CapturePipelinePolicyTest, WgcFreshCatchupNeverRelabelsLiveContentIntoAnOldCfrSlot) {
    constexpr int64_t kHistoricalSelectionTargetQpc = 1200;
    EXPECT_TRUE(
        policy::ShouldUseFreshWgcCatchupFrame(1200, 1195, 1195, kHistoricalSelectionTargetQpc, 1100, 1100, 100));
    EXPECT_FALSE(
        policy::ShouldUseFreshWgcCatchupFrame(1550, 1545, 1545, kHistoricalSelectionTargetQpc, 1100, 1100, 100));
}

TEST(CapturePipelinePolicyTest, WgcStopDrainKeepsOnlyFramesCapturedAtOrBeforeStop) {
    EXPECT_TRUE(policy::ShouldKeepWgcFrameForStopDrain(999, 1000));
    EXPECT_TRUE(policy::ShouldKeepWgcFrameForStopDrain(1000, 1000));
    EXPECT_FALSE(policy::ShouldKeepWgcFrameForStopDrain(1001, 1000));
    EXPECT_TRUE(policy::ShouldKeepWgcFrameForStopDrain(0, 1000));
    EXPECT_TRUE(policy::ShouldKeepWgcFrameForStopDrain(1001, 0));
}

TEST(CapturePipelinePolicyTest, WgcStopDrainUsesCapturedOrCachedStateForScheduledTicks) {
    EXPECT_FALSE(policy::CanDrainOutstandingWgcTicks(false, false, false, false));
    EXPECT_FALSE(policy::CanDrainOutstandingWgcTicks(false, false, true, false));
    EXPECT_TRUE(policy::CanDrainOutstandingWgcTicks(true, false, false, false));
    EXPECT_TRUE(policy::CanDrainOutstandingWgcTicks(false, true, false, false));
    EXPECT_TRUE(policy::CanDrainOutstandingWgcTicks(false, false, true, true));
}

TEST(CapturePipelinePolicyTest, CfrOutputShortfallTicksIsClampedToPositiveDelta) {
    EXPECT_EQ(policy::GetCfrOutputShortfallTicks(0, 0), 0u);
    EXPECT_EQ(policy::GetCfrOutputShortfallTicks(10, 10), 0u);
    EXPECT_EQ(policy::GetCfrOutputShortfallTicks(25, 20), 5u);
}

TEST(CapturePipelinePolicyTest, AdjustedScheduledTicksSubtractDiscardedTimerDebt) {
    EXPECT_EQ(policy::GetAdjustedCfrScheduledTicks(100, 16), 84u);
    EXPECT_EQ(policy::GetAdjustedCfrScheduledTicks(12, 16), 0u);
}

TEST(CapturePipelinePolicyTest, WgcVisualDebtDiagnosticsDoNotShortenEndpointSchedule) {
    EXPECT_EQ(policy::GetCfrScheduledTicksForEndpoint(100, 0, 24), 100u);
    EXPECT_EQ(policy::GetCfrScheduledTicksForEndpoint(100, 16, 24), 84u);
}

TEST(CapturePipelinePolicyTest, TimerRebaseDiscardDropsOnlyOutstandingShortfall) {
    EXPECT_EQ(policy::GetCfrTimerRebaseDiscardTicks(100, 0, 84), 16u);
    EXPECT_EQ(policy::GetCfrTimerRebaseDiscardTicks(221, 16, 203), 2u);
    EXPECT_EQ(policy::GetCfrTimerRebaseDiscardTicks(221, 18, 203), 0u);
}

TEST(CapturePipelinePolicyTest, TimerRebaseDebtIsPreservedForAllCfrPaths) {
    EXPECT_FALSE(policy::ShouldDiscardCfrTimerRebaseDebt(false));
    EXPECT_FALSE(policy::ShouldDiscardCfrTimerRebaseDebt(true));
}

TEST(CapturePipelinePolicyTest, TimerRebaseThresholdKeepsInjectCfrFromSmallCatchupBursts) {
    EXPECT_EQ(policy::GetCfrTimerRebaseThresholdTicks(false, false, true),
              policy::kCfrShortfallForceCatchupThresholdTicks);
    EXPECT_EQ(policy::GetCfrTimerRebaseThresholdTicks(true, false, true),
              policy::kCfrShortfallForceCatchupThresholdTicks);
    EXPECT_EQ(policy::GetCfrTimerRebaseThresholdTicks(false, true, true), policy::kCfrShortfallCatchupThresholdTicks);
    EXPECT_EQ(policy::GetCfrTimerRebaseThresholdTicks(false, false, false), policy::kCfrShortfallCatchupThresholdTicks);
}

TEST(CapturePipelinePolicyTest, CfrCatchupRequiresMeaningfulShortfallOrForceThreshold) {
    EXPECT_FALSE(policy::ShouldCfrCatchUpToWallClock(0, true, true, true));
    EXPECT_FALSE(policy::ShouldCfrCatchUpToWallClock(policy::kCfrShortfallCatchupThresholdTicks - 1, true, true, true));
    EXPECT_TRUE(policy::ShouldCfrCatchUpToWallClock(policy::kCfrShortfallCatchupThresholdTicks, true, true, false));
    EXPECT_TRUE(
        policy::ShouldCfrCatchUpToWallClock(policy::kCfrShortfallForceCatchupThresholdTicks, false, false, false));
    EXPECT_TRUE(policy::ShouldCfrCatchUpToWallClock(policy::kCfrShortfallCatchupThresholdTicks, false, false, true));
}

TEST(CapturePipelinePolicyTest, CfrCatchupTicksGradualBelowForceThreshold) {
    // Generic CFR stays conservative below the force threshold.
    EXPECT_EQ(policy::GetCfrCatchupTicksThisLoop(policy::kCfrShortfallCatchupThresholdTicks), 2u);
    EXPECT_EQ(policy::GetCfrCatchupTicksThisLoop(policy::kCfrShortfallCatchupThresholdTicks + 4), 2u);
    EXPECT_EQ(policy::GetCfrCatchupTicksThisLoop(policy::kCfrShortfallForceCatchupThresholdTicks - 1), 2u);
}

TEST(CapturePipelinePolicyTest, InjectCfrRecoveryUsesHysteresisAndPausesWhileEncoderIsSlow) {
    EXPECT_FALSE(
        policy::GetInjectCfrRecoveryActive(false, true, false, policy::kCfrShortfallForceCatchupThresholdTicks - 1));
    EXPECT_TRUE(
        policy::GetInjectCfrRecoveryActive(false, true, false, policy::kCfrShortfallForceCatchupThresholdTicks));
    EXPECT_TRUE(
        policy::GetInjectCfrRecoveryActive(true, true, false, policy::kInjectCfrRecoveryExitShortfallTicks + 1));
    EXPECT_FALSE(policy::GetInjectCfrRecoveryActive(true, true, false, policy::kInjectCfrRecoveryExitShortfallTicks));
    EXPECT_FALSE(policy::GetInjectCfrRecoveryActive(true, false, false, 100));
    EXPECT_FALSE(policy::GetInjectCfrRecoveryActive(true, true, true, 100));

    EXPECT_EQ(policy::GetInjectCfrCatchupTicksThisLoop(100, false), 1u);
    EXPECT_EQ(policy::GetInjectCfrCatchupTicksThisLoop(100, true), 2u);
    EXPECT_EQ(policy::GetInjectCfrCatchupTicksThisLoop(100, true, true), 1u);
}

TEST(CapturePipelinePolicyTest, CfrRecoveryRepaysDebtWithoutSelfThrottlingItsWakeCadence) {
    constexpr double kFrameIntervalMs = 1000.0 / 120.0;
    EXPECT_DOUBLE_EQ(policy::GetInjectCfrServiceMsPerOutputTick(12.0, 2), 6.0);
    EXPECT_DOUBLE_EQ(policy::GetInjectCfrServiceMsPerOutputTick(6.0, 1), 6.0);
    EXPECT_DOUBLE_EQ(policy::GetInjectCfrServiceMsPerOutputTick(12.0, 0), 0.0);
    EXPECT_FALSE(policy::IsEncoderTooSlowForTargetFps(policy::GetInjectCfrServiceMsPerOutputTick(12.0, 2),
                                                       kFrameIntervalMs, 120));
    EXPECT_TRUE(policy::IsEncoderTooSlowForTargetFps(12.0, kFrameIntervalMs, 120));

    EXPECT_FALSE(policy::ShouldAdvanceWakeDeadlineForCfrCatchupTick(false, true));
    EXPECT_FALSE(policy::ShouldAdvanceWakeDeadlineForCfrCatchupTick(true, true));
    EXPECT_FALSE(policy::ShouldAdvanceWakeDeadlineForCfrCatchupTick(true, false));
    EXPECT_TRUE(policy::ShouldAdvanceWakeDeadlineForCfrCatchupTick(false, false));

    uint32_t scheduledTicks = policy::kCfrShortfallForceCatchupThresholdTicks;
    uint32_t outputTicks = 0;
    bool recoveryActive = false;
    for (uint32_t loop = 0; loop < policy::kCfrShortfallForceCatchupThresholdTicks; ++loop) {
        const uint32_t shortfall = policy::GetCfrOutputShortfallTicks(scheduledTicks, outputTicks);
        recoveryActive = policy::GetInjectCfrRecoveryActive(recoveryActive, true, false, shortfall);
        const bool serviceTooSlow = policy::IsEncoderTooSlowForTargetFps(
            policy::GetInjectCfrServiceMsPerOutputTick(12.0, 2), kFrameIntervalMs, 120);
        outputTicks += policy::GetInjectCfrCatchupTicksThisLoop(shortfall, recoveryActive, serviceTooSlow);
        ++scheduledTicks;  // The normal 120 Hz wake cadence continues while one extra slot is serviced.
    }

    const uint32_t finalShortfall = policy::GetCfrOutputShortfallTicks(scheduledTicks, outputTicks);
    EXPECT_LE(finalShortfall, policy::kInjectCfrRecoveryExitShortfallTicks);
    EXPECT_FALSE(policy::GetInjectCfrRecoveryActive(recoveryActive, true, false, finalShortfall));

    scheduledTicks = policy::kCfrShortfallForceCatchupThresholdTicks;
    outputTicks = 0;
    for (uint32_t loop = 0; loop < policy::kCfrShortfallForceCatchupThresholdTicks; ++loop) {
        const uint32_t shortfall = policy::GetCfrOutputShortfallTicks(scheduledTicks, outputTicks);
        outputTicks +=
            policy::GetWgcCatchupTicksThisLoop(true, true, 4, 2.0, shortfall, 120, 118, 124, 0, false, 200.0);
        ++scheduledTicks;
    }
    EXPECT_LT(policy::GetCfrOutputShortfallTicks(scheduledTicks, outputTicks),
              policy::kCfrShortfallCatchupThresholdTicks);
}

TEST(CapturePipelinePolicyTest, RecoveryOutputQpcStaysOnTheImmutableCfrGrid) {
    EXPECT_EQ(policy::GetNextCfrOutputQpc(1000, 0, 100, 77), 1000);
    EXPECT_EQ(policy::GetNextCfrOutputQpc(1000, 3, 100, 77), 1300);
    EXPECT_EQ(policy::GetNextCfrOutputQpc(0, 3, 100, 77), 77);
    EXPECT_EQ(policy::GetNextCfrOutputQpc(1000, 3, 0, 77), 77);
    EXPECT_EQ(policy::GetNextCfrOutputQpc(INT64_MAX - 10, 2, 10, 77), 77);
    EXPECT_EQ(policy::GetNextInjectCfrOutputQpc(1000, 0, 100, 77), 1000);
    EXPECT_EQ(policy::GetNextInjectCfrOutputQpc(1000, 3, 100, 77), 1300);
    EXPECT_EQ(policy::GetNextInjectCfrOutputQpc(0, 3, 100, 77), 77);
    EXPECT_EQ(policy::GetNextInjectCfrOutputQpc(1000, 3, 0, 77), 77);
    EXPECT_EQ(policy::GetNextInjectCfrOutputQpc(INT64_MAX - 10, 2, 10, 77), 77);
}

TEST(CapturePipelinePolicyTest, InjectFreshCatchupRequiresHealthyEncoderAndTargetCandidate) {
    EXPECT_TRUE(policy::ShouldUseFreshInjectCatchup(false, false, false, 4, 2,
                                                    policy::kCfrShortfallForceCatchupThresholdTicks, true));
    EXPECT_FALSE(policy::ShouldUseFreshInjectCatchup(true, false, false, 4, 2,
                                                     policy::kCfrShortfallForceCatchupThresholdTicks, true));
    EXPECT_FALSE(policy::ShouldUseFreshInjectCatchup(false, true, false, 4, 2,
                                                     policy::kCfrShortfallForceCatchupThresholdTicks, true));
    EXPECT_FALSE(policy::ShouldUseFreshInjectCatchup(false, false, true, 4, 2,
                                                     policy::kCfrShortfallForceCatchupThresholdTicks, true));
    EXPECT_FALSE(policy::ShouldUseFreshInjectCatchup(false, false, false, 2, 2,
                                                     policy::kCfrShortfallForceCatchupThresholdTicks, true));
    EXPECT_FALSE(policy::ShouldUseFreshInjectCatchup(false, false, false, 4, 2,
                                                     policy::kInjectCfrRecoveryExitShortfallTicks, true));
    EXPECT_FALSE(policy::ShouldUseFreshInjectCatchup(false, false, false, 4, 2,
                                                     policy::kCfrShortfallForceCatchupThresholdTicks, false));
}

TEST(CapturePipelinePolicyTest, WgcCatchupTicksRecoverModerateShortfallWhenHealthy) {
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, false, 2, 1.0, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 118, 120, 0, false),
              2u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(
                  false, false, 4, 2.0, policy::kCfrShortfallForceCatchupThresholdTicks - 1, 120, 124, 130, 0, false),
              2u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, true, 4, 2.0, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 112, 124, 0, false),
              2u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, true, 4, 2.0, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 110, 112, 0, true),
              2u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, false, 1, 2.0, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 110, 112, 0, true),
              2u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, false, 4, 0.5, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 110, 112, 120, false),
              2u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, false, 4, 2.0,
                                                 policy::kCfrShortfallCatchupThresholdTicks - 1, 120, 118, 120, 0,
                                                 false),
              1u);
}

TEST(CapturePipelinePolicyTest, WgcCatchupDoesNotWaitForDelayedAudioLeadTelemetry) {
    EXPECT_FALSE(policy::ShouldPrioritizeWgcAudioLeadCatchup(39.9));
    EXPECT_TRUE(policy::ShouldPrioritizeWgcAudioLeadCatchup(policy::kWgcAudioLeadCatchupThresholdMs));

    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, true, 4, 2.0, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 118, 124, 0, false, policy::kWgcAudioLeadCatchupThresholdMs - 0.1),
              2u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, true, 4, 2.0, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 118, 124, 0, false, policy::kWgcAudioLeadCatchupThresholdMs),
              2u);
}

TEST(CapturePipelinePolicyTest, WgcFreshCatchupSpendsOnlyReservoirSurplusWithEncoderHeadroom) {
    constexpr double kFrameIntervalMs = 1000.0 / 120.0;
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(4, false, false, 5.0, kFrameIntervalMs, 9, 4), 3u);
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(2, false, false, 5.0, kFrameIntervalMs, 9, 4), 1u);
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(4, false, false, 5.0, kFrameIntervalMs, 5, 4), 1u);
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(4, true, false, 5.0, kFrameIntervalMs, 9, 4), 0u);
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(4, false, true, 5.0, kFrameIntervalMs, 9, 4), 0u);
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(4, false, false, 6.25, kFrameIntervalMs, 9, 4), 0u);
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(4, false, false, 6.3, kFrameIntervalMs, 9, 4), 0u);
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(4, false, false, 0.0, kFrameIntervalMs, 9, 4), 0u);
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(
                  4, false, false, std::numeric_limits<double>::infinity(), kFrameIntervalMs, 9, 4),
              0u);
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(
                  4, false, false, std::numeric_limits<double>::quiet_NaN(), kFrameIntervalMs, 9, 4),
              0u);
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(4, false, false, 5.0, 0.0, 9, 4), 0u);
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(1, false, false, 5.0, kFrameIntervalMs, 9, 4), 0u);
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(4, false, false, 5.0, kFrameIntervalMs, 4, 4), 0u);

    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, true, 4, 2.0, policy::kCfrShortfallForceCatchupThresholdTicks,
                                                 120, 118, 124, 0, false, policy::kWgcAudioLeadCatchupThresholdMs),
              4u);
}

TEST(CapturePipelinePolicyTest, WgcOverloadRepeatPacerTurnsDeepHoldIntoEvenCapacityMix) {
    constexpr double kFrameIntervalMs = 1000.0 / 120.0;
    constexpr double kFreshServiceMs = 9.21;
    constexpr double kRepeatServiceMs = 5.48;
    constexpr uint32_t kOutputTicks = 7744;
    policy::WgcOverloadRepeatPacerState state;
    uint32_t freshGrants = 0;
    uint32_t proactiveRepeats = 0;

    for (uint32_t tick = 0; tick < kOutputTicks; ++tick) {
        const auto decision = policy::UpdateWgcOverloadRepeatPacer(
            state, true, true, true, true, true, kFreshServiceMs, kRepeatServiceMs, kFrameIntervalMs,
            policy::kWgcOverloadRepeatPacerMinSamples, policy::kWgcOverloadRepeatPacerMinSamples);
        if (decision.repeat) {
            ++proactiveRepeats;
        } else {
            ++freshGrants;
        }
    }

    EXPECT_TRUE(state.active);
    EXPECT_EQ(state.episodes, 1u);
    EXPECT_EQ(state.proactiveRepeats, proactiveRepeats);
    EXPECT_EQ(state.freshGrants, freshGrants);
    EXPECT_GT(freshGrants, 5000u);
    EXPECT_LT(freshGrants, 5100u);
    EXPECT_LE(state.maxConsecutiveProactiveRepeats, 1u);
    const double weightedServiceMs =
        (static_cast<double>(freshGrants) * kFreshServiceMs +
         static_cast<double>(proactiveRepeats) * kRepeatServiceMs) /
        kOutputTicks;
    EXPECT_LE(weightedServiceMs, kFrameIntervalMs * policy::kWgcOverloadRepeatPacerBudgetRatio + 0.01);
}

TEST(CapturePipelinePolicyTest, WgcOverloadRepeatPacerRequiresMeasuredUsefulRepeatCapacity) {
    constexpr double kFrameIntervalMs = 1000.0 / 120.0;
    constexpr uint32_t kSamples = policy::kWgcOverloadRepeatPacerMinSamples;
    policy::WgcOverloadRepeatPacerState state;

    auto decision = policy::UpdateWgcOverloadRepeatPacer(
        state, true, true, true, true, true, 9.21, 5.48, kFrameIntervalMs, kSamples - 1u, kSamples);
    EXPECT_FALSE(decision.active);
    EXPECT_STREQ(decision.reason, "warming_service_samples");

    decision = policy::UpdateWgcOverloadRepeatPacer(
        state, true, false, true, true, true, 9.21, 5.48, kFrameIntervalMs, kSamples, kSamples);
    EXPECT_FALSE(decision.active);
    EXPECT_STREQ(decision.reason, "source_not_healthy");

    decision = policy::UpdateWgcOverloadRepeatPacer(
        state, true, true, false, true, true, 9.21, 5.48, kFrameIntervalMs, kSamples, kSamples);
    EXPECT_FALSE(decision.active);
    EXPECT_STREQ(decision.reason, "capacity_not_confirmed");

    decision = policy::UpdateWgcOverloadRepeatPacer(
        state, true, true, true, true, false, 9.21, 5.48, kFrameIntervalMs, kSamples, kSamples);
    EXPECT_FALSE(decision.active);
    EXPECT_STREQ(decision.reason, "repeat_unavailable");

    decision = policy::UpdateWgcOverloadRepeatPacer(
        state, true, true, true, true, true, 9.21, 8.0, kFrameIntervalMs, kSamples, kSamples);
    EXPECT_TRUE(decision.active);
    EXPECT_STREQ(decision.reason, "pacing");
    EXPECT_GT(decision.serviceBudgetMs, kFrameIntervalMs * policy::kWgcOverloadRepeatPacerBudgetRatio);

    state.ResetActivePacing();
    decision = policy::UpdateWgcOverloadRepeatPacer(
        state, true, true, true, true, true, 8.15, 7.7, kFrameIntervalMs, kSamples, kSamples);
    EXPECT_FALSE(decision.active);
    EXPECT_STREQ(decision.reason, "fresh_within_budget");

    decision = policy::UpdateWgcOverloadRepeatPacer(
        state, true, true, true, true, true, 9.21, 8.9, kFrameIntervalMs, kSamples, kSamples);
    EXPECT_FALSE(decision.active);
    EXPECT_STREQ(decision.reason, "repeat_not_cheaper");
}

TEST(CapturePipelinePolicyTest, WgcOverloadRepeatPacerCreditsNaturalHoldsAndRecoversCleanly) {
    constexpr double kFrameIntervalMs = 1000.0 / 120.0;
    constexpr uint32_t kSamples = policy::kWgcOverloadRepeatPacerMinSamples;
    policy::WgcOverloadRepeatPacerState state;

    auto decision = policy::UpdateWgcOverloadRepeatPacer(
        state, true, true, true, true, true, 9.21, 5.48, kFrameIntervalMs, kSamples, kSamples);
    EXPECT_TRUE(decision.entered);
    EXPECT_FALSE(decision.repeat);

    decision = policy::UpdateWgcOverloadRepeatPacer(
        state, true, true, false, false, true, 9.21, 5.48, kFrameIntervalMs, kSamples, kSamples);
    EXPECT_TRUE(decision.active);
    EXPECT_FALSE(decision.repeat);

    decision = policy::UpdateWgcOverloadRepeatPacer(
        state, true, true, false, true, true, 9.21, 5.48, kFrameIntervalMs, kSamples, kSamples);
    EXPECT_TRUE(decision.active);
    EXPECT_FALSE(decision.repeat);

    decision = policy::UpdateWgcOverloadRepeatPacer(
        state, true, true, false, true, true, 7.0, 5.48, kFrameIntervalMs, kSamples, kSamples);
    EXPECT_FALSE(decision.exited);
    EXPECT_TRUE(decision.active);
    EXPECT_FALSE(decision.repeat);
    EXPECT_STREQ(decision.reason, "recovery_hysteresis");
    for (uint32_t tick = 1; tick < policy::kWgcOverloadRepeatPacerRecoveryConfirmTicks; ++tick) {
        decision = policy::UpdateWgcOverloadRepeatPacer(
            state, true, true, false, true, true, 7.0, 5.48, kFrameIntervalMs, kSamples, kSamples);
    }
    EXPECT_TRUE(decision.exited);
    EXPECT_FALSE(decision.active);
    EXPECT_STREQ(decision.reason, "service_recovered");
}

TEST(CapturePipelinePolicyTest, WgcOverloadRepeatPacerBoundsLivenessWhenEvenRepeatsMissInterval) {
    constexpr double kFrameIntervalMs = 1000.0 / 120.0;
    constexpr uint32_t kSamples = policy::kWgcOverloadRepeatPacerMinSamples;
    policy::WgcOverloadRepeatPacerState state;
    for (uint32_t tick = 0; tick < 400; ++tick) {
        const auto decision = policy::UpdateWgcOverloadRepeatPacer(
            state, true, true, true, true, true, 10.0, 8.5, kFrameIntervalMs, kSamples, kSamples);
        EXPECT_TRUE(decision.active);
        EXPECT_STREQ(decision.reason, "degraded_repeat_over_interval");
    }
    EXPECT_GT(state.freshGrants, 15u);
    EXPECT_LE(state.maxConsecutiveProactiveRepeats, 20u);
}

TEST(CapturePipelinePolicyTest, WgcOverloadRepeatPacerServiceEmaUsesConservativeValidSamples) {
    double serviceMs = 0.0;
    uint32_t samples = 0;

    policy::UpdateWgcServiceTimeEma(4.0, 6.0, 0.25, serviceMs, samples);
    EXPECT_DOUBLE_EQ(serviceMs, 6.0);
    EXPECT_EQ(samples, 1u);

    policy::UpdateWgcServiceTimeEma(10.0, 8.0, 0.25, serviceMs, samples);
    EXPECT_DOUBLE_EQ(serviceMs, 7.0);
    EXPECT_EQ(samples, 2u);

    policy::UpdateWgcServiceTimeEma(std::numeric_limits<double>::quiet_NaN(), 8.0, 0.25, serviceMs, samples);
    policy::UpdateWgcServiceTimeEma(8.0, 8.0, 0.0, serviceMs, samples);
    EXPECT_DOUBLE_EQ(serviceMs, 7.0);
    EXPECT_EQ(samples, 2u);
}

TEST(CapturePipelinePolicyTest, WgcForceShortfallRepeatCatchupIsIndependentOfSourceRecovery) {
    EXPECT_TRUE(policy::IsWgcSourceRecoveredEnoughForSmoothAudioLeadCatchup(120, 116, 116, 40));
    EXPECT_FALSE(policy::IsWgcSourceRecoveredEnoughForSmoothAudioLeadCatchup(120, 115, 116, 40));
    EXPECT_FALSE(policy::IsWgcSourceRecoveredEnoughForSmoothAudioLeadCatchup(120, 116, 116, 200));

    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, false, 4, 2.0, policy::kCfrShortfallForceCatchupThresholdTicks,
                                                 120, 116, 116, 40, false, policy::kWgcAudioLeadCatchupThresholdMs),
              4u);
}

TEST(CapturePipelinePolicyTest, CfrCatchupTicksBurstAtForceThreshold) {
    // At and above force threshold: allow larger bursts, capped at 4
    EXPECT_EQ(policy::GetCfrCatchupTicksThisLoop(policy::kCfrShortfallForceCatchupThresholdTicks), 4u);
    EXPECT_EQ(policy::GetCfrCatchupTicksThisLoop(100), 4u);
    // Small force-threshold shortfall still capped to shortfall itself
    // (the function uses min(shortfall, 4))
}

TEST(CapturePipelinePolicyTest, WgcCatchupCapsSevereHeldRepeatBurstAtFourTicks) {
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, false, 2, 1.0, policy::kCfrShortfallForceCatchupThresholdTicks,
                                                 120, 118, 120, 0, false),
              4u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, true, 4, 2.0, policy::kCfrShortfallForceCatchupThresholdTicks,
                                                 120, 112, 124, 0, false),
              4u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, true, 2, 0.0, policy::kCfrShortfallForceCatchupThresholdTicks,
                                                 120, 84, 84, 200, true),
              4u);
}

TEST(CapturePipelinePolicyTest, EncoderCapacityDiagnosticsQuantifyBudgetAndShortfall) {
    EXPECT_NEAR(policy::GetCfrShortfallDurationMs(3, 8.333333), 25.0, 0.01);
    EXPECT_DOUBLE_EQ(policy::GetCfrShortfallDurationMs(0, 8.333333), 0.0);

    EXPECT_NEAR(policy::GetEncoderSustainableOutputFps(8.333333), 120.0, 0.05);
    EXPECT_DOUBLE_EQ(policy::GetEncoderSustainableOutputFps(0.0), 0.0);

    EXPECT_EQ(policy::GetEncoderBudgetUtilizationPermille(8.333333, 8.333333), 1000u);
    EXPECT_EQ(policy::GetEncoderBudgetUtilizationPermille(4.166666, 8.333333), 500u);
    EXPECT_EQ(policy::GetEncoderBudgetUtilizationPermille(0.0, 8.333333), 0u);

    EXPECT_TRUE(policy::IsEncoderTooSlowForTargetFps(9.50, 8.333333, 120));
    EXPECT_FALSE(policy::IsEncoderTooSlowForTargetFps(8.20, 8.333333, 120));
    EXPECT_FALSE(policy::IsEncoderTooSlowForTargetFps(0.0, 8.333333, 120));
}

TEST(CapturePipelinePolicyTest, DxgiDuplicationPreferredForMonitorScopeDesktopFallback) {
    // Explicit capture_method=dxgi_dup always prefers duplication.
    EXPECT_TRUE(policy::ShouldPreferDxgiDuplicationForMonitorCapture(
        /*explicitDxgiDupConfig=*/true, /*explicitWgcConfig=*/false, /*autoCaptureConfig=*/false));

    // Explicit capture_method=wgc keeps the WGC monitor item.
    EXPECT_FALSE(policy::ShouldPreferDxgiDuplicationForMonitorCapture(
        /*explicitDxgiDupConfig=*/false, /*explicitWgcConfig=*/true, /*autoCaptureConfig=*/false));

    // Auto mode: any monitor-scope (pure desktop) fallback prefers duplication.
    // Inject and WGC window capture take priority upstream of this decision.
    EXPECT_TRUE(policy::ShouldPreferDxgiDuplicationForMonitorCapture(
        /*explicitDxgiDupConfig=*/false, /*explicitWgcConfig=*/false, /*autoCaptureConfig=*/true));

    // Explicit inject (neither flag, not auto) never reaches monitor priming,
    // but the policy must still answer false.
    EXPECT_FALSE(policy::ShouldPreferDxgiDuplicationForMonitorCapture(
        /*explicitDxgiDupConfig=*/false, /*explicitWgcConfig=*/false, /*autoCaptureConfig=*/false));
}

TEST(CapturePipelinePolicyTest, DxgiDuplicationContentBitsClassifyDeliveredFormats) {
    constexpr uint32_t kFp16 = 10;   // DXGI_FORMAT_R16G16B16A16_FLOAT
    constexpr uint32_t kR10 = 24;    // DXGI_FORMAT_R10G10B10A2_UNORM
    constexpr uint32_t kBgra8 = 87;  // DXGI_FORMAT_B8G8R8A8_UNORM
    constexpr uint32_t kNv12 = 103;  // DXGI_FORMAT_NV12 (never a desktop surface)

    EXPECT_EQ(policy::GetDxgiDuplicationSourceContentBits(kFp16), 16u);
    EXPECT_EQ(policy::GetDxgiDuplicationSourceContentBits(kR10), 10u);
    EXPECT_EQ(policy::GetDxgiDuplicationSourceContentBits(kBgra8), 8u);
    EXPECT_EQ(policy::GetDxgiDuplicationSourceContentBits(kNv12), 0u);

    EXPECT_TRUE(policy::IsAcceptableDxgiDuplicationFrameFormat(kR10, true, false));
    EXPECT_TRUE(policy::IsAcceptableDxgiDuplicationFrameFormat(kFp16, true, false));
    EXPECT_FALSE(policy::IsAcceptableDxgiDuplicationFrameFormat(kBgra8, true, false));
    EXPECT_TRUE(policy::IsAcceptableDxgiDuplicationFrameFormat(kBgra8, false, false));
    EXPECT_TRUE(policy::IsAcceptableDxgiDuplicationFrameFormat(kFp16, true, true));
    EXPECT_FALSE(policy::IsAcceptableDxgiDuplicationFrameFormat(kR10, true, true));
    EXPECT_FALSE(policy::IsAcceptableDxgiDuplicationFrameFormat(kNv12, false, false));
}

TEST(CapturePipelinePolicyTest, ExplicitTenBitDxgiDoesNotSilentlyFallbackToWgc) {
    EXPECT_FALSE(policy::ShouldAllowWgcFallbackAfterDxgiFailure(true, true));
    EXPECT_TRUE(policy::ShouldAllowWgcFallbackAfterDxgiFailure(true, false));
    EXPECT_TRUE(policy::ShouldAllowWgcFallbackAfterDxgiFailure(false, true));
    EXPECT_TRUE(policy::ShouldAllowWgcFallbackAfterDxgiFailure(false, false));
}

TEST(CapturePipelinePolicyTest, FullscreenAutoTargetPrefersDuplicationForHardwareCursor) {
    // Unhooked fullscreen game in auto mode with the preference enabled: use
    // monitor-scope duplication (WGC sessions demote the hardware cursor).
    EXPECT_TRUE(policy::ShouldPreferDxgiDuplicationForFullscreenAutoTarget(
        /*autoCaptureConfig=*/true, /*explicitInjectConfig=*/false, /*injectWhitelisted=*/false,
        /*targetFullscreenLike=*/true, /*fullscreenPrefersDuplication=*/true));

    // Windowed targets keep WGC window capture (scoped content is the point).
    EXPECT_FALSE(policy::ShouldPreferDxgiDuplicationForFullscreenAutoTarget(true, false, false,
                                                                            /*targetFullscreenLike=*/false, true));

    // Config can force the WGC window path for fullscreen games.
    EXPECT_FALSE(policy::ShouldPreferDxgiDuplicationForFullscreenAutoTarget(true, false, false, true,
                                                                            /*fullscreenPrefersDuplication=*/false));

    // Inject-whitelisted games and explicit inject never reach screen grab.
    EXPECT_FALSE(policy::ShouldPreferDxgiDuplicationForFullscreenAutoTarget(true, false,
                                                                            /*injectWhitelisted=*/true, true, true));
    EXPECT_FALSE(policy::ShouldPreferDxgiDuplicationForFullscreenAutoTarget(true, /*explicitInjectConfig=*/true, false,
                                                                            true, true));

    // Non-auto configs make their own explicit backend choice.
    EXPECT_FALSE(policy::ShouldPreferDxgiDuplicationForFullscreenAutoTarget(/*autoCaptureConfig=*/false, false, false,
                                                                            true, true));
}

TEST(CapturePipelinePolicyTest, DuplicationBudgetSpendsSourcePoolShareOnCopySlots) {
    // WGC shape at 4K FP16 source + R10 retained copy, 3000MB, 120fps/300ms.
    const auto wgcBudget = policy::ComputeWgcSmoothnessSurfaceBudget(
        /*outputFps=*/120, /*maxSmoothnessMs=*/300, /*width=*/3840, /*height=*/2160,
        /*sourceBytesPerPixel=*/8, /*copyBytesPerPixel=*/4, /*budgetMb=*/3000, /*syncDelayFrames=*/4,
        /*requiresSourceFramePool=*/true);
    EXPECT_GT(wgcBudget.sourceFramePoolBuffers, 0u);

    // Duplication shape: no consumer-owned source frame pool; the whole budget
    // funds retained copy slots, so the copy pool must be at least as deep and
    // the retained reservoir must not shrink versus the WGC split.
    const auto dupBudget = policy::ComputeWgcSmoothnessSurfaceBudget(
        /*outputFps=*/120, /*maxSmoothnessMs=*/300, /*width=*/3840, /*height=*/2160,
        /*sourceBytesPerPixel=*/8, /*copyBytesPerPixel=*/4, /*budgetMb=*/3000, /*syncDelayFrames=*/4,
        /*requiresSourceFramePool=*/false);
    EXPECT_EQ(dupBudget.sourceFramePoolBuffers, 0u);
    EXPECT_GE(dupBudget.copyPoolSlots, wgcBudget.copyPoolSlots);
    EXPECT_GE(dupBudget.retainedExtraFrames, wgcBudget.retainedExtraFrames);
    EXPECT_EQ(dupBudget.sourceEstimatedBytes, 0ull);
    EXPECT_FALSE(dupBudget.budgetExhausted);

    // A tight budget that cap-limits the WGC split must recover reservoir depth
    // in duplication mode because the source-pool share is reclaimed.
    const auto tightWgc = policy::ComputeWgcSmoothnessSurfaceBudget(120, 300, 3840, 2160, 8, 4, 1024, 4, true);
    const auto tightDup = policy::ComputeWgcSmoothnessSurfaceBudget(120, 300, 3840, 2160, 8, 4, 1024, 4, false);
    EXPECT_GT(tightDup.retainedExtraFrames, tightWgc.retainedExtraFrames);
}

TEST(CapturePipelinePolicyTest, RecordingPressureRequiresAnAuthoritativeRuntimeEvent) {
    EXPECT_FALSE(policy::HasRecordingEncoderOrMuxPressure(0, 0, 0));
    EXPECT_TRUE(policy::HasRecordingEncoderOrMuxPressure(1, 0, 0));
    EXPECT_TRUE(policy::HasRecordingEncoderOrMuxPressure(0, 1, 0));
    EXPECT_TRUE(policy::HasRecordingEncoderOrMuxPressure(0, 0, 1));
}
