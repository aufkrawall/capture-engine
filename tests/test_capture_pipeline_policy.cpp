#include <gtest/gtest.h>
#include "../common/capture_pipeline_policy.h"

namespace policy = ce::capture_policy;

TEST(CapturePipelinePolicyTest, WarmupCommitRequiresPoppedFrame) {
    EXPECT_FALSE(policy::ShouldCommitRecordingWarmup(false, false, false, true, 4, 2, policy::kRecordingWarmupMaxMs));
}

TEST(CapturePipelinePolicyTest, WarmupCommitHonorsMinAndMaxThresholds) {
    EXPECT_FALSE(
        policy::ShouldCommitRecordingWarmup(false, false, true, false, 8, 2, policy::kRecordingWarmupMinMs - 1));
    EXPECT_TRUE(policy::ShouldCommitRecordingWarmup(false, false, true, false, 0, 4, policy::kRecordingWarmupMaxMs));
}

TEST(CapturePipelinePolicyTest, WarmupCommitUsesVfrAndBufferedSourceRules) {
    EXPECT_TRUE(policy::ShouldCommitRecordingWarmup(false, true, true, false, 0, 3, policy::kRecordingWarmupMinMs));
    EXPECT_FALSE(policy::ShouldCommitRecordingWarmup(true, false, true, false, 0, 0, policy::kRecordingWarmupMinMs));
    EXPECT_TRUE(policy::ShouldCommitRecordingWarmup(true, false, true, true, 0, 0, policy::kRecordingWarmupMinMs));
    EXPECT_FALSE(policy::ShouldCommitRecordingWarmup(false, false, true, false, 1, 2, policy::kRecordingWarmupMinMs));
    EXPECT_FALSE(policy::ShouldCommitRecordingWarmup(false, false, true, false, 2, 2, policy::kRecordingWarmupMinMs));
    EXPECT_TRUE(policy::ShouldCommitRecordingWarmup(false, false, true, false, 3, 2, policy::kRecordingWarmupMinMs));
}

TEST(CapturePipelinePolicyTest, InjectReserveFramesScaleWithFenceLeadTime) {
    EXPECT_EQ(policy::GetInjectReserveFrames(false, 0.0, 8.333), 1u);
    EXPECT_EQ(policy::GetInjectReserveFrames(false, 5.0, 8.0), 2u);
    EXPECT_EQ(policy::GetInjectReserveFrames(false, 11.0, 8.0), 3u);
    EXPECT_EQ(policy::GetInjectReserveFrames(false, 19.0, 8.0), 4u);
    EXPECT_EQ(policy::GetInjectReserveFrames(true, 19.0, 8.0), 0u);
}

TEST(CapturePipelinePolicyTest, WgcStopDrainAcceptsAnyRepeatableFrameSource) {
    EXPECT_TRUE(policy::CanDrainOutstandingWgcTicks(false, false, true, false));
    EXPECT_TRUE(policy::CanDrainOutstandingWgcTicks(false, false, false, true));
    EXPECT_TRUE(policy::CanDrainOutstandingWgcTicks(true, false, false, false));
    EXPECT_TRUE(policy::CanDrainOutstandingWgcTicks(false, true, false, false));
    EXPECT_FALSE(policy::CanDrainOutstandingWgcTicks(false, false, false, false));
}

TEST(CapturePipelinePolicyTest, WgcCoverageLossRepeatPolicyRequiresLagMismatch) {
    EXPECT_TRUE(policy::HasWgcUnrecoverableCoverageLoss(6333.0, 23.0));
    EXPECT_FALSE(policy::HasWgcUnrecoverableCoverageLoss(200.0, 0.0));
    EXPECT_FALSE(policy::HasWgcUnrecoverableCoverageLoss(300.0, 220.0));

    EXPECT_DOUBLE_EQ(policy::ComputeWgcCoverageLossRepeatRatio(0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(policy::ComputeWgcCoverageLossRepeatRatio(300.0, 220.0), 0.0);
    EXPECT_NEAR(policy::ComputeWgcCoverageLossRepeatRatio(500.0, 100.0), 0.186666, 0.000001);
    EXPECT_NEAR(policy::ComputeWgcCoverageLossRepeatRatio(2000.0, 0.0), 0.35, 0.000001);
    EXPECT_NEAR(policy::ComputeWgcCoverageLossRepeatRatio(16034.0, 12.0), 0.35, 0.000001);

    EXPECT_EQ(policy::GetWgcCoverageDelayTicks(0, 0.0, 8.333), 0u);
    EXPECT_EQ(policy::GetWgcCoverageDelayTicks(30, 220.0, 8.333), 0u);
    EXPECT_EQ(policy::GetWgcCoverageDelayTicks(31, 0.0, 8.333), 31u);
    EXPECT_EQ(policy::GetWgcCoverageDelayTicks(60, 0.0, 8.333), policy::kWgcCoverageDelayMaxTicks);
    EXPECT_EQ(policy::GetWgcCoverageDelayTicks(31, 100.0, 8.333), 19u);
}

TEST(CapturePipelinePolicyTest, WarmupKeepCountAndMinimumBufferedFramesFollowReserve) {
    EXPECT_EQ(policy::GetWarmupInjectKeepCount(0.0, 8.333), 3u);
    EXPECT_EQ(policy::GetWarmupInjectKeepCount(19.0, 8.0), 5u);
    EXPECT_EQ(policy::GetMinBufferedInjectFrames(0, false), 0u);
    EXPECT_EQ(policy::GetMinBufferedInjectFrames(3, false), 3u);
    EXPECT_EQ(policy::GetMinBufferedInjectFrames(3, true), 2u);
}

TEST(CapturePipelinePolicyTest, StartupHeadroomUsesStartupWindow) {
    EXPECT_TRUE(policy::IsInjectEncoderStartup(false, 1000, 5000));
    EXPECT_TRUE(policy::IsInjectEncoderStartup(true, 1000, 2000));
    EXPECT_FALSE(policy::IsInjectEncoderStartup(true, 1000, 3000));

    EXPECT_EQ(policy::GetInjectBufferedHeadroom(false, 1000, 5000), policy::kStartupInjectBufferedHeadroomFrames);
    EXPECT_EQ(policy::GetInjectBufferedHeadroom(true, 1000, 1200), policy::kStartupInjectBufferedHeadroomFrames);
    EXPECT_EQ(policy::GetInjectBufferedHeadroom(true, 1000, 2600), policy::kMaxInjectBufferedHeadroomFrames);
    EXPECT_EQ(policy::GetMaxBufferedInjectFrames(3, true, 1000, 2600), 3u + policy::kMaxInjectBufferedHeadroomFrames);
}

TEST(CapturePipelinePolicyTest, WarmupStateResetsOnlyWhenCaptureModeChangesBeforeLive) {
    policy::WarmupTransitionState state = {false, 100, 7};

    EXPECT_FALSE(policy::ResetWarmupOnCaptureModeChange(false, false, 250, state));
    EXPECT_EQ(state.startupWarmupStartTick, 100u);
    EXPECT_EQ(state.hiddenStartupFrames, 7u);

    EXPECT_TRUE(policy::ResetWarmupOnCaptureModeChange(false, true, 300, state));
    EXPECT_TRUE(state.warmupWasScreenGrab);
    EXPECT_EQ(state.startupWarmupStartTick, 300u);
    EXPECT_EQ(state.hiddenStartupFrames, 0u);

    state.hiddenStartupFrames = 9;
    EXPECT_FALSE(policy::ResetWarmupOnCaptureModeChange(true, false, 400, state));
    EXPECT_EQ(state.hiddenStartupFrames, 9u);
}

TEST(CapturePipelinePolicyTest, AutoWgcFallbackPolicyUsesSourcePidAwareDelayAndGuards) {
    EXPECT_EQ(policy::GetAutoWgcFallbackDelayMs(0), policy::kAutoWgcFallbackDelayNoPidMs);
    EXPECT_EQ(policy::GetAutoWgcFallbackDelayMs(42), policy::kAutoWgcFallbackDelayWithPidMs);

    EXPECT_FALSE(policy::ShouldTriggerAutoWgcFallback(true, true, true, true, 500, 42));
    EXPECT_FALSE(policy::ShouldTriggerAutoWgcFallback(false, false, true, true, 500, 42));
    EXPECT_FALSE(policy::ShouldTriggerAutoWgcFallback(false, true, false, true, 500, 42));
    EXPECT_FALSE(policy::ShouldTriggerAutoWgcFallback(false, true, true, false, 500, 42));
    EXPECT_FALSE(
        policy::ShouldTriggerAutoWgcFallback(false, true, true, true, policy::kAutoWgcFallbackDelayNoPidMs, 0));
    EXPECT_TRUE(
        policy::ShouldTriggerAutoWgcFallback(false, true, true, true, policy::kAutoWgcFallbackDelayNoPidMs + 1, 0));
    EXPECT_FALSE(
        policy::ShouldTriggerAutoWgcFallback(false, true, true, true, policy::kAutoWgcFallbackDelayWithPidMs, 7));
    EXPECT_TRUE(
        policy::ShouldTriggerAutoWgcFallback(false, true, true, true, policy::kAutoWgcFallbackDelayWithPidMs + 1, 7));
}

TEST(CapturePipelinePolicyTest, WgcLowSourceModePrefersBufferCushionDuringUnderfeed) {
    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;
    telemetry.recentDeliveredFps = 119;
    telemetry.recentDeliveredMin250Fps = 118;
    telemetry.recentDeliveredMin500Fps = 118;
    telemetry.recentInputMin250Fps = 118;
    telemetry.recentInputMin500Fps = 118;
    telemetry.emptyTickPermille = 20;
    EXPECT_TRUE(policy::ShouldUseWgcLowSourceMode(telemetry));

    telemetry.recentDeliveredFps = 125;
    telemetry.recentDeliveredMin250Fps = 124;
    telemetry.recentDeliveredMin500Fps = 124;
    telemetry.recentInputMin250Fps = 124;
    telemetry.recentInputMin500Fps = 124;
    telemetry.emptyTickPermille = 20;
    EXPECT_FALSE(policy::ShouldUseWgcLowSourceMode(telemetry));

    telemetry.emptyTickPermille = 100;
    EXPECT_TRUE(policy::ShouldUseWgcLowSourceMode(telemetry));
    EXPECT_TRUE(policy::ShouldEnterWgcLowSourceMode(telemetry));

    telemetry.recentDeliveredFps = 120;
    telemetry.recentDeliveredMin250Fps = 120;
    telemetry.recentDeliveredMin500Fps = 120;
    telemetry.recentInputMin250Fps = 123;
    telemetry.recentInputMin500Fps = 123;
    telemetry.emptyTickPermille = 30;
    telemetry.bufferedWgcFrames = 1;
    EXPECT_TRUE(policy::ShouldExitWgcLowSourceMode(telemetry));

    telemetry.emptyTickPermille = 60;
    EXPECT_FALSE(policy::ShouldExitWgcLowSourceMode(telemetry));
}

TEST(CapturePipelinePolicyTest, WgcWarmupCommitRequiresStableBufferedSource) {
    EXPECT_FALSE(policy::ShouldCommitWgcWarmup(false, 3, policy::kRecordingWarmupMaxMs, 120.0, 120));
    EXPECT_FALSE(policy::ShouldCommitWgcWarmup(true, 2, policy::kRecordingWarmupMinMs, 120.0, 120));
    EXPECT_TRUE(policy::ShouldCommitWgcWarmup(true, 3, policy::kRecordingWarmupMinMs, 120.0, 120));
    EXPECT_FALSE(policy::ShouldCommitWgcWarmup(true, 3, policy::kRecordingWarmupMinMs, 115.0, 120));
}

TEST(CapturePipelinePolicyTest, WgcLowSourceSelectionClampProtectsFragileQueue) {
    EXPECT_EQ(policy::ClampWgcSelectionIndexForLowSource(3, 4, 2, 116, 120, 130), 0u);
    EXPECT_EQ(policy::ClampWgcSelectionIndexForLowSource(3, 4, 2, 119, 120, 90), 1u);
    EXPECT_EQ(policy::ClampWgcSelectionIndexForLowSource(1, 4, 4, 130, 120, 10), 1u);
}

TEST(CapturePipelinePolicyTest, WgcLowSourceDropPolicyKeepsFrontFrameWhenQueueFragile) {
    EXPECT_TRUE(policy::ShouldDropFrontWgcFrameForSelection(1, 2, true, 140));
    EXPECT_FALSE(policy::ShouldDropFrontWgcFrameForSelection(0, 4, true, 10));
    EXPECT_TRUE(policy::ShouldDropFrontWgcFrameForSelection(1, 4, false, 10));
    EXPECT_TRUE(policy::ShouldDropFrontWgcFrameForSelection(1, 4, true, 10));
}

TEST(CapturePipelinePolicyTest, WgcFreshnessGuardRequiresRecentTimestamp) {
    const int64_t targetIntervalTicks = 100;
    const int64_t minFreshNormal = policy::GetWgcMinimumFreshTimestampQpc(1000, 1500, targetIntervalTicks, false);
    const int64_t minFreshLowSource = policy::GetWgcMinimumFreshTimestampQpc(1000, 1500, targetIntervalTicks, true);

    EXPECT_EQ(minFreshNormal, 1300);
    EXPECT_EQ(minFreshLowSource, 1200);
    EXPECT_TRUE(policy::IsWgcTimestampFreshEnough(1300, minFreshNormal));
    EXPECT_FALSE(policy::IsWgcTimestampFreshEnough(1299, minFreshNormal));
}

TEST(CapturePipelinePolicyTest, WgcStaleUniqueFallbackAllowsOneExtraTickOfLag) {
    const int64_t targetIntervalTicks = 100;
    EXPECT_EQ(policy::GetWgcStaleUniqueFallbackMinTimestampQpc(1000, 1500, targetIntervalTicks, false), 1200);
    EXPECT_EQ(policy::GetWgcStaleUniqueFallbackMinTimestampQpc(1000, 1500, targetIntervalTicks, true), 1100);
}

TEST(CapturePipelinePolicyTest, WgcStaleUniqueFallbackStillRequiresNewSourceTimestamp) {
    const int64_t targetIntervalTicks = 100;
    EXPECT_EQ(policy::GetWgcStaleUniqueFallbackMinTimestampQpc(1499, 1500, targetIntervalTicks, false), 1500);
    EXPECT_EQ(policy::GetWgcStaleUniqueFallbackMinTimestampQpc(1500, 1500, targetIntervalTicks, false), 1501);
}

TEST(CapturePipelinePolicyTest, WgcSelectionTargetDelaysLiveSelectionByOneTick) {
    EXPECT_EQ(policy::GetWgcSelectionTargetQpc(1500, 1400, 100, true), 1400);
    EXPECT_EQ(policy::GetWgcSelectionTargetQpc(1500, 1400, 100, false), 1500);
    EXPECT_EQ(policy::GetWgcSelectionTargetQpc(0, 1400, 100, true), 1300);
    EXPECT_EQ(policy::GetWgcSelectionTargetQpc(50, 40, 100, true), 50);
}

TEST(CapturePipelinePolicyTest, WgcSelectionDelayStopsWhenBehindOrReserveIsGone) {
    EXPECT_TRUE(policy::ShouldApplyWgcSelectionDelay(true, 0, false, true));
    EXPECT_FALSE(policy::ShouldApplyWgcSelectionDelay(true, 1, false, true));
    EXPECT_FALSE(policy::ShouldApplyWgcSelectionDelay(true, 0, true, true));
    EXPECT_FALSE(policy::ShouldApplyWgcSelectionDelay(true, 0, false, false));
    EXPECT_FALSE(policy::ShouldApplyWgcSelectionDelay(false, 0, false, true));
}

TEST(CapturePipelinePolicyTest, WgcReservePressureActivatesOnlyWithSustainedSingleFrameTicks) {
    EXPECT_FALSE(policy::IsWgcReservePressureActive(12, 20, 120));
    EXPECT_FALSE(policy::IsWgcReservePressureActive(20, 20, 120));
    EXPECT_TRUE(policy::IsWgcReservePressureActive(50, 80, 120));
    EXPECT_TRUE(policy::IsWgcReservePressureActive(6, 8, 30));
}

TEST(CapturePipelinePolicyTest, WgcReserveBiasPrefersEarlierFreshFrameWhenDifferenceIsSmall) {
    EXPECT_TRUE(policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(1000, 1040, 1020, 100, false, false));
    EXPECT_FALSE(policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(920, 1040, 1020, 100, false, false));
    EXPECT_FALSE(policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(900, 1040, 1020, 100, true, false));
}

TEST(CapturePipelinePolicyTest, WgcSingleFreshHoldRequiresFragileSourceConditions) {
    EXPECT_TRUE(policy::ShouldAllowSingleFreshWgcHold(true, false, 118, 120, 0.98));
    EXPECT_TRUE(policy::ShouldAllowSingleFreshWgcHold(false, true, 120, 120, 0.99));
    EXPECT_FALSE(policy::ShouldAllowSingleFreshWgcHold(false, false, 118, 120, 0.98));
    EXPECT_FALSE(policy::ShouldAllowSingleFreshWgcHold(true, false, 120, 120, 1.01));
}

TEST(CapturePipelinePolicyTest, WgcSteadyReserveBuildRequiresHealthyNearTargetSource) {
    EXPECT_TRUE(policy::ShouldAllowSteadyStateWgcReserveBuild(120, 120, 1.00));
    EXPECT_TRUE(policy::ShouldAllowSteadyStateWgcReserveBuild(121, 120, 1.02));
    EXPECT_FALSE(policy::ShouldAllowSteadyStateWgcReserveBuild(119, 120, 1.02));
    EXPECT_FALSE(policy::ShouldAllowSteadyStateWgcReserveBuild(120, 120, 0.98));
}

TEST(CapturePipelinePolicyTest, WgcFreshFrameHoldStopsWhenAlreadyBehindOrPressured) {
    EXPECT_TRUE(policy::ShouldHoldSingleFreshWgcFrame(true, false, 118, 120, 0.98, 0, false, false));
    EXPECT_FALSE(policy::ShouldHoldSingleFreshWgcFrame(true, false, 118, 120, 0.98, 1, false, false));
    EXPECT_FALSE(policy::ShouldHoldSingleFreshWgcFrame(true, false, 118, 120, 0.98, 0, true, false));
    EXPECT_FALSE(policy::ShouldHoldSingleFreshWgcFrame(true, false, 118, 120, 0.98, 0, false, true));
    EXPECT_FALSE(policy::ShouldHoldSingleFreshWgcFrame(false, false, 120, 120, 1.00, 0, false, false));
}

TEST(CapturePipelinePolicyTest, WgcExtraCatchupRequiresSurplusAndNoEncoderBottleneck) {
    EXPECT_TRUE(policy::ShouldAllowWgcExtraCatchupTicks(false, 2, 1.0, 0));
    EXPECT_TRUE(policy::ShouldAllowWgcExtraCatchupTicks(false, 4, 2.5, 0));
    EXPECT_FALSE(policy::ShouldAllowWgcExtraCatchupTicks(true, 4, 2.5, 0));
    EXPECT_FALSE(policy::ShouldAllowWgcExtraCatchupTicks(false, 1, 2.5, 0));
    EXPECT_FALSE(
        policy::ShouldAllowWgcExtraCatchupTicks(false, 4, 0.99, policy::kCfrShortfallForceCatchupThresholdTicks - 1));
    EXPECT_TRUE(
        policy::ShouldAllowWgcExtraCatchupTicks(false, 4, 0.0, policy::kCfrShortfallForceCatchupThresholdTicks));
    EXPECT_TRUE(policy::ShouldAllowWgcExtraCatchupTicks(true, 4, 0.0, policy::kCfrShortfallForceCatchupThresholdTicks));
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

TEST(CapturePipelinePolicyTest, TimerRebaseDiscardDropsOnlyOutstandingShortfall) {
    EXPECT_EQ(policy::GetCfrTimerRebaseDiscardTicks(100, 0, 84), 16u);
    EXPECT_EQ(policy::GetCfrTimerRebaseDiscardTicks(221, 16, 203), 2u);
    EXPECT_EQ(policy::GetCfrTimerRebaseDiscardTicks(221, 18, 203), 0u);
}

TEST(CapturePipelinePolicyTest, TimerRebaseDebtDiscardIsDisabledForWgc) {
    EXPECT_FALSE(policy::ShouldDiscardCfrTimerRebaseDebt(true));
    EXPECT_TRUE(policy::ShouldDiscardCfrTimerRebaseDebt(false));
}

TEST(CapturePipelinePolicyTest, CfrCatchupRequiresMeaningfulShortfallOrForceThreshold) {
    EXPECT_FALSE(policy::ShouldCfrCatchUpToWallClock(0, true, true, true));
    EXPECT_FALSE(policy::ShouldCfrCatchUpToWallClock(policy::kCfrShortfallCatchupThresholdTicks - 1, true, true, true));
    EXPECT_TRUE(policy::ShouldCfrCatchUpToWallClock(policy::kCfrShortfallCatchupThresholdTicks, true, true, false));
    EXPECT_TRUE(
        policy::ShouldCfrCatchUpToWallClock(policy::kCfrShortfallForceCatchupThresholdTicks, false, false, false));
    EXPECT_FALSE(policy::ShouldCfrCatchUpToWallClock(policy::kCfrShortfallCatchupThresholdTicks, false, false, true));
}

TEST(CapturePipelinePolicyTest, CfrCatchupTicksGradualBelowForceThreshold) {
    // Generic CFR stays conservative below the force threshold.
    EXPECT_EQ(policy::GetCfrCatchupTicksThisLoop(policy::kCfrShortfallCatchupThresholdTicks), 1u);
    EXPECT_EQ(policy::GetCfrCatchupTicksThisLoop(policy::kCfrShortfallCatchupThresholdTicks + 4), 1u);
    EXPECT_EQ(policy::GetCfrCatchupTicksThisLoop(policy::kCfrShortfallForceCatchupThresholdTicks - 1), 1u);
}

TEST(CapturePipelinePolicyTest, WgcCatchupTicksRecoverModerateShortfallWhenHealthy) {
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, 2, 1.0, policy::kCfrShortfallCatchupThresholdTicks), 2u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, 4, 2.0, policy::kCfrShortfallForceCatchupThresholdTicks - 1),
              2u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, 4, 2.0, policy::kCfrShortfallCatchupThresholdTicks), 1u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, 1, 2.0, policy::kCfrShortfallCatchupThresholdTicks), 1u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, 4, 0.5, policy::kCfrShortfallCatchupThresholdTicks), 1u);
}

TEST(CapturePipelinePolicyTest, CfrCatchupTicksBurstAtForceThreshold) {
    // At and above force threshold: allow larger bursts, capped at 4
    EXPECT_EQ(policy::GetCfrCatchupTicksThisLoop(policy::kCfrShortfallForceCatchupThresholdTicks), 4u);
    EXPECT_EQ(policy::GetCfrCatchupTicksThisLoop(100), 4u);
    // Small force-threshold shortfall still capped to shortfall itself
    // (the function uses min(shortfall, 4))
}

TEST(CapturePipelinePolicyTest, WgcCatchupTicksStillBurstAtForceThreshold) {
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, 2, 1.0, policy::kCfrShortfallForceCatchupThresholdTicks), 4u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, 2, 0.0, policy::kCfrShortfallForceCatchupThresholdTicks), 4u);
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
