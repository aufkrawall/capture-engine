#include "test_capture_pipeline_policy_shared.h"

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

TEST(CapturePipelinePolicyTest, InjectCfrSourcePublicationUsesFourTimesOutputCadence) {
    EXPECT_EQ(policy::GetInjectCfrSourcePublicationFps(0), 0u);
    EXPECT_EQ(policy::GetInjectCfrSourcePublicationFps(60), 240u);
    EXPECT_EQ(policy::GetInjectCfrSourcePublicationFps(120), 480u);
    EXPECT_EQ(policy::GetInjectCfrSourcePublicationIntervalUs(60), 4166);
    EXPECT_EQ(policy::GetInjectCfrSourcePublicationIntervalUs(120), 2083);
    EXPECT_EQ(policy::GetInjectCfrSourcePublicationIntervalQpc(60, 10000000), 41666);
    EXPECT_EQ(policy::GetInjectCfrSourcePublicationIntervalQpc(120, 10000000), 20833);
    EXPECT_EQ(policy::GetInjectCfrSourcePublicationIntervalQpc(60, 0), 0);
}

TEST(CapturePipelinePolicyTest, InjectPublicationSpacingAllowsSourceJitterWithoutGridStarvation) {
    EXPECT_EQ(policy::GetInjectCfrPublicationEarlySlackUs(4166), 520);
    EXPECT_EQ(policy::GetInjectCfrPublicationMinSpacingUs(4166), 3646);
    EXPECT_EQ(policy::GetInjectCfrPublicationEarlySlackUs(2083), 260);
    EXPECT_EQ(policy::GetInjectCfrPublicationMinSpacingUs(2083), 1823);
    EXPECT_EQ(policy::GetInjectCfrPublicationEarlySlackUs(8333), 1041);
    EXPECT_EQ(policy::GetInjectCfrPublicationMinSpacingUs(8333), 7292);
    EXPECT_EQ(policy::GetInjectCfrPublicationMinSpacingUs(0), 0);
}

TEST(CapturePipelinePolicyTest, InjectFrameFreshnessRequiresMonotonicSourceTime) {
    EXPECT_TRUE(policy::IsInjectFrameFreshAfterLastEmission(1000, 0));
    EXPECT_TRUE(policy::IsInjectFrameFreshAfterLastEmission(1001, 1000));
    EXPECT_FALSE(policy::IsInjectFrameFreshAfterLastEmission(1000, 1000));
    EXPECT_FALSE(policy::IsInjectFrameFreshAfterLastEmission(999, 1000));
    EXPECT_FALSE(policy::IsInjectFrameFreshAfterLastEmission(0, 1000));
}

TEST(CapturePipelinePolicyTest, WgcStopDrainClosesOnlyWhenAFrameOrRepeatExists) {
    EXPECT_TRUE(policy::ShouldDrainOutstandingCfrTicksAtStop(true, false));
    EXPECT_FALSE(policy::CanDrainOutstandingWgcTicks(false, false, true, false));
    EXPECT_FALSE(policy::CanDrainOutstandingWgcTicks(false, false, false, true));
    EXPECT_TRUE(policy::CanDrainOutstandingWgcTicks(false, false, true, true));
    EXPECT_TRUE(policy::CanDrainOutstandingWgcTicks(true, false, false, false));
    EXPECT_TRUE(policy::CanDrainOutstandingWgcTicks(false, true, false, false));
    EXPECT_FALSE(policy::CanDrainOutstandingWgcTicks(false, false, false, false));
}

TEST(CapturePipelinePolicyTest, WgcStopDrainHeldFrameRequiresMediaEngineRepeatCache) {
    EXPECT_FALSE(policy::CanDrainOutstandingWgcTicks(false, false, false, false));
    EXPECT_FALSE(policy::CanDrainOutstandingWgcTicks(false, false, true, false));
    EXPECT_FALSE(policy::CanDrainOutstandingWgcTicks(false, false, false, true));
    EXPECT_TRUE(policy::CanDrainOutstandingWgcTicks(false, false, true, true));
}

TEST(CapturePipelinePolicyTest, ScheduledFreshEncodeFailureUsesPriorFrameWithoutTimelineHole) {
    EXPECT_TRUE(policy::ShouldRepeatAfterScheduledFreshEncodeFailure(true, false, false, true, true));
    EXPECT_FALSE(policy::ShouldRepeatAfterScheduledFreshEncodeFailure(false, false, false, true, true));
    EXPECT_FALSE(policy::ShouldRepeatAfterScheduledFreshEncodeFailure(true, true, false, true, true));
    EXPECT_FALSE(policy::ShouldRepeatAfterScheduledFreshEncodeFailure(true, false, true, true, true));
    EXPECT_FALSE(policy::ShouldRepeatAfterScheduledFreshEncodeFailure(true, false, false, false, true));
    EXPECT_FALSE(policy::ShouldRepeatAfterScheduledFreshEncodeFailure(true, false, false, true, false));
}

TEST(CapturePipelinePolicyTest, WgcLiveSchedulerRebaseIsBoundedToSingleTick) {
    EXPECT_EQ(policy::GetWgcLiveSchedulerRebaseTicksThisLoop(0, 32, 1), 0u);
    EXPECT_EQ(policy::GetWgcLiveSchedulerRebaseTicksThisLoop(20, 0, 1), 0u);
    EXPECT_EQ(policy::GetWgcLiveSchedulerRebaseTicksThisLoop(20, 32, 0), 0u);
    EXPECT_EQ(policy::GetWgcLiveSchedulerRebaseTicksThisLoop(20, 32, 1), 1u);
    EXPECT_EQ(policy::GetWgcLiveSchedulerRebaseTicksThisLoop(20, 32, 8), 1u);
    EXPECT_EQ(policy::GetWgcLiveSchedulerRebaseTicksThisLoop(20, 32, 8, 2), 2u);
    EXPECT_EQ(policy::GetWgcLiveSchedulerRebaseTicksThisLoop(1, 32, 8, 2), 1u);
}

TEST(CapturePipelinePolicyTest, WgcEncoderLimitedModeUsesNearLiveDebtWindow) {
    EXPECT_TRUE(policy::IsWgcEncoderLimitedSmoothnessMode(true, false, 0));
    EXPECT_TRUE(policy::IsWgcEncoderLimitedSmoothnessMode(false, true, 0));
    EXPECT_TRUE(policy::IsWgcEncoderLimitedSmoothnessMode(false, false, policy::kEncoderOverloadFlagEncoder));
    EXPECT_FALSE(policy::IsWgcEncoderLimitedSmoothnessMode(false, false, 0));

    EXPECT_EQ(policy::GetWgcLiveVisualDebtLimitTicksForMode(100, 1000, false), 3u);
    EXPECT_EQ(policy::GetWgcLiveVisualDebtLimitTicksForMode(100, 1000, true), 1u);
    EXPECT_EQ(policy::GetWgcLiveVisualDebtExcessTicksForMode(2, 100, 1000, true), 1u);
    EXPECT_EQ(policy::GetWgcLiveVisualDebtFloorQpcForMode(1600, 100, 1000, true), 1550);

    EXPECT_EQ(policy::GetWgcLiveSchedulerRebaseTicksThisLoopForMode(20, 32, 8, false), 0u);
    EXPECT_EQ(policy::GetWgcLiveSchedulerRebaseTicksThisLoopForMode(20, 32, 8, true), 0u);
}

TEST(CapturePipelinePolicyTest, InjectStopDrainMayUseCachedRepeatToCloseCfrDebt) {
    EXPECT_FALSE(policy::CanDrainOutstandingCfrTicks(false, false, false, false, false));
    EXPECT_TRUE(policy::CanDrainOutstandingCfrTicks(false, true, false, false, false));
    EXPECT_TRUE(policy::CanDrainOutstandingCfrTicks(false, false, true, false, false));
    EXPECT_FALSE(policy::CanDrainOutstandingCfrTicks(false, false, false, true, false));
    EXPECT_TRUE(policy::CanDrainOutstandingCfrTicks(false, false, false, true, true));
}

TEST(CapturePipelinePolicyTest, EveryCfrBackendDrainsItsContiguousPrefixAtStop) {
    EXPECT_TRUE(policy::ShouldDrainOutstandingCfrTicksAtStop(true, false));
    EXPECT_TRUE(policy::ShouldDrainOutstandingCfrTicksAtStop(false, false));
    EXPECT_FALSE(policy::ShouldDrainOutstandingCfrTicksAtStop(false, true));
    EXPECT_TRUE(policy::CanDrainOutstandingCfrTicks(true, false, false, true, true));
    EXPECT_FALSE(policy::CanDrainOutstandingCfrTicks(true, false, false, true, false));
    EXPECT_TRUE(policy::CanDrainOutstandingCfrTicks(true, true, false, false, false));
    EXPECT_TRUE(policy::CanDrainOutstandingCfrTicks(true, false, true, false, false));
}

TEST(CapturePipelinePolicyTest, StopBeforeFirstLiveFrameCannotKeepCfrDrainArmed) {
    EXPECT_TRUE(policy::ShouldAbortCfrStopDrainBeforeOutputIsLive(false, false, true));
    EXPECT_FALSE(policy::ShouldAbortCfrStopDrainBeforeOutputIsLive(true, false, true));
    EXPECT_FALSE(policy::ShouldAbortCfrStopDrainBeforeOutputIsLive(false, true, true));
    EXPECT_FALSE(policy::ShouldAbortCfrStopDrainBeforeOutputIsLive(false, false, false));
}

TEST(CapturePipelinePolicyTest, WgcCoverageLossRepeatPolicyRequiresLagMismatch) {
    EXPECT_TRUE(policy::HasWgcUnrecoverableCoverageLoss(6333.0, 23.0));
    EXPECT_TRUE(policy::HasWgcUnrecoverableCoverageLoss(6333.0, 23.0, 80.0));
    EXPECT_FALSE(policy::HasWgcUnrecoverableCoverageLoss(6333.0, 23.0, 20.0));
    EXPECT_FALSE(policy::HasWgcUnrecoverableCoverageLoss(200.0, 0.0));
    EXPECT_FALSE(policy::HasWgcUnrecoverableCoverageLoss(300.0, 220.0));

    EXPECT_DOUBLE_EQ(policy::ComputeWgcCoverageLossRepeatRatio(0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(policy::ComputeWgcCoverageLossRepeatRatio(300.0, 220.0), 0.0);
    EXPECT_NEAR(policy::ComputeWgcCoverageLossRepeatRatio(500.0, 100.0), 0.186666, 0.000001);
    EXPECT_NEAR(policy::ComputeWgcCoverageLossRepeatRatio(2000.0, 0.0), 0.35, 0.000001);
    EXPECT_NEAR(policy::ComputeWgcCoverageLossRepeatRatio(2000.0, 0.0, 250.0), 0.086666, 0.000001);
    EXPECT_NEAR(policy::ComputeWgcCoverageLossRepeatRatio(16034.0, 12.0), 0.35, 0.000001);

    EXPECT_EQ(policy::GetWgcCoverageDelayTicks(0, 0.0, 8.333), 0u);
    EXPECT_EQ(policy::GetWgcCoverageDelayTicks(30, 220.0, 8.333), 0u);
    EXPECT_EQ(policy::GetWgcCoverageDelayTicks(31, 0.0, 8.333), 31u);
    EXPECT_EQ(policy::GetWgcCoverageDelayTicks(60, 0.0, 8.333), policy::kWgcCoverageDelayMaxTicks);
    EXPECT_EQ(policy::GetWgcCoverageDelayTicks(31, 100.0, 8.333), 19u);
}

TEST(CapturePipelinePolicyTest, WgcCoverageLossSuppressionHonorsEncoderOnlyShortfall) {
    EXPECT_TRUE(policy::ShouldSuppressWgcCoverageLossForEncoderBottleneck(true, 120, 120));
    EXPECT_TRUE(policy::ShouldSuppressWgcCoverageLossForEncoderBottleneck(true, 118, 120));
    EXPECT_FALSE(policy::ShouldSuppressWgcCoverageLossForEncoderBottleneck(true, 100, 120));
    EXPECT_FALSE(policy::ShouldSuppressWgcCoverageLossForEncoderBottleneck(false, 120, 120));
    EXPECT_FALSE(policy::ShouldSuppressWgcCoverageLossForEncoderBottleneck(true, 0, 120));
    EXPECT_FALSE(policy::ShouldSuppressWgcCoverageLossForEncoderBottleneck(true, 120, 0));
}

TEST(CapturePipelinePolicyTest, WarmupKeepCountAndMinimumBufferedFramesFollowReserve) {
    EXPECT_EQ(policy::GetWarmupInjectKeepCount(0.0, 8.333), 3u);
    EXPECT_EQ(policy::GetWarmupInjectKeepCount(19.0, 8.0), 5u);
    EXPECT_EQ(policy::GetInjectCfrStartupReadyFrames(/*reserve=*/1, /*contentDelay=*/4), 6u);
    EXPECT_EQ(policy::GetInjectCfrStartupReadyFrames(/*reserve=*/0, /*contentDelay=*/0), 3u);
    EXPECT_EQ(policy::GetMinBufferedInjectFrames(0, false), 0u);
    EXPECT_EQ(policy::GetMinBufferedInjectFrames(1, true), 1u);
    EXPECT_EQ(policy::GetMinBufferedInjectFrames(3, false), 3u);
    EXPECT_EQ(policy::GetMinBufferedInjectFrames(3, true), 2u);
}

TEST(CapturePipelinePolicyTest, AutoCaptureUsesInjectOnlyForGameWhitelistMatches) {
    EXPECT_TRUE(policy::ShouldUseInjectCaptureForAutoTarget(true, false, false));
    EXPECT_FALSE(policy::ShouldUseInjectCaptureForAutoTarget(false, true, false));
    EXPECT_TRUE(policy::ShouldUseInjectCaptureForAutoTarget(false, true, true));
    EXPECT_FALSE(policy::ShouldUseInjectCaptureForAutoTarget(false, false, true));

    EXPECT_TRUE(policy::ShouldUseWgcCaptureForAutoTarget(true, false, true));
    EXPECT_TRUE(policy::ShouldUseWgcCaptureForAutoTarget(false, true, false));
    EXPECT_FALSE(policy::ShouldUseWgcCaptureForAutoTarget(false, true, true));
}

TEST(CapturePipelinePolicyTest, ActiveCapturePathRejectsMismatchedFrameKinds) {
    EXPECT_TRUE(policy::ShouldAcceptFrameForActiveCapturePath(false, true));
    EXPECT_FALSE(policy::ShouldAcceptFrameForActiveCapturePath(false, false));
    EXPECT_TRUE(policy::ShouldAcceptFrameForActiveCapturePath(true, false));
    EXPECT_FALSE(policy::ShouldAcceptFrameForActiveCapturePath(true, true));
}

TEST(CapturePipelinePolicyTest, StartupHeadroomUsesStartupWindow) {
    EXPECT_TRUE(policy::IsEncoderStartupWindow(false, 1000, 5000));
    EXPECT_TRUE(policy::IsEncoderStartupWindow(true, 1000, 2000));
    EXPECT_FALSE(policy::IsEncoderStartupWindow(true, 1000, 3000));

    EXPECT_TRUE(policy::IsInjectEncoderStartup(false, 1000, 5000));
    EXPECT_TRUE(policy::IsInjectEncoderStartup(true, 1000, 2000));
    EXPECT_FALSE(policy::IsInjectEncoderStartup(true, 1000, 3000));

    EXPECT_EQ(policy::GetInjectBufferedHeadroom(false, 1000, 5000), policy::kStartupInjectBufferedHeadroomFrames);
    EXPECT_EQ(policy::GetInjectBufferedHeadroom(true, 1000, 1200), policy::kStartupInjectBufferedHeadroomFrames);
    EXPECT_EQ(policy::GetInjectBufferedHeadroom(true, 1000, 2600), policy::kMaxInjectBufferedHeadroomFrames);
    EXPECT_EQ(policy::GetMaxBufferedInjectFrames(3, true, 1000, 2600), 3u + policy::kMaxInjectBufferedHeadroomFrames);
}

TEST(CapturePipelinePolicyTest, EncoderCapacityWarningIgnoresStartupPriming) {
    constexpr double frameIntervalMs = 1000.0 / 120.0;

    EXPECT_FALSE(policy::ShouldWarnEncoderApproachingCapacity(20.19, frameIntervalMs, true));
    EXPECT_FALSE(policy::ShouldWarnEncoderApproachingCapacity(7.0, frameIntervalMs, false));
    EXPECT_TRUE(policy::ShouldWarnEncoderApproachingCapacity(7.2, frameIntervalMs, false));
    EXPECT_FALSE(policy::ShouldWarnEncoderApproachingCapacity(7.2, 0.0, false));
}

TEST(CapturePipelinePolicyTest, InjectLiveAgeTrimPreservesReserveAndExpandsUnderPressure) {
    EXPECT_EQ(policy::GetInjectLiveMaxFrameAgeQpc(false, false, false, false, 100), 0);
    EXPECT_EQ(policy::GetInjectLiveMaxFrameAgeQpc(true, false, false, false, 100),
              100 * static_cast<int64_t>(policy::kInjectLiveHealthyMaxFrameAgeTicks));
    EXPECT_EQ(policy::GetInjectLiveMaxFrameAgeQpc(true, true, false, false, 100),
              100 * static_cast<int64_t>(policy::kInjectLivePressureMaxFrameAgeTicks));
    EXPECT_EQ(policy::GetInjectLiveMaxFrameAgeQpc(true, false, false, true, 100),
              100 * static_cast<int64_t>(policy::kInjectLivePressureMaxFrameAgeTicks));

    EXPECT_FALSE(policy::ShouldTrimStaleInjectLiveFrame(1000, 1400, 300, 3, 2));
    EXPECT_TRUE(policy::ShouldTrimStaleInjectLiveFrame(1000, 1401, 300, 4, 2));
    EXPECT_FALSE(policy::ShouldTrimStaleInjectLiveFrame(1000, 1250, 300, 4, 2));
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

TEST(CapturePipelinePolicyTest, WgcLowSourceModeIgnoresBorderlineInstantaneousDeficits) {
    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;
    telemetry.recentDeliveredFps = 119;
    telemetry.recentDeliveredMin250Fps = 119;
    telemetry.recentDeliveredMin500Fps = 120;
    telemetry.recentInputMin250Fps = 119;
    telemetry.recentInputMin500Fps = 120;
    telemetry.emptyTickPermille = 20;

    EXPECT_FALSE(policy::ShouldUseWgcLowSourceMode(telemetry));

    telemetry.recentDeliveredFps = 117;
    EXPECT_TRUE(policy::ShouldUseWgcLowSourceMode(telemetry));

    telemetry.recentDeliveredFps = 120;
    telemetry.recentDeliveredMin250Fps = 120;
    telemetry.recentInputMin250Fps = 117;
    EXPECT_TRUE(policy::ShouldUseWgcLowSourceMode(telemetry));

    telemetry.recentInputMin250Fps = 119;
    telemetry.recentInputMin500Fps = 119;
    EXPECT_TRUE(policy::ShouldUseWgcLowSourceMode(telemetry));
}

TEST(CapturePipelinePolicyTest, WgcLowSourceStateClassificationIsExplicit) {
    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;
    telemetry.recentDeliveredFps = 120;
    telemetry.recentDeliveredMin250Fps = 120;
    telemetry.recentDeliveredMin500Fps = 120;
    telemetry.recentInputMin250Fps = 120;
    telemetry.recentInputMin500Fps = 120;
    telemetry.emptyTickPermille = 0;

    EXPECT_EQ(policy::ClassifyWgcLowSourceState(telemetry), policy::WgcLowSourceState::kHealthy);
    EXPECT_STREQ(policy::WgcLowSourceStateToString(policy::WgcLowSourceState::kHealthy), "healthy");

    telemetry.recentInputMin250Fps = 119;
    telemetry.recentInputMin500Fps = 118;
    EXPECT_EQ(policy::ClassifyWgcLowSourceState(telemetry), policy::WgcLowSourceState::kInputBelowTarget);
    EXPECT_STREQ(policy::WgcLowSourceStateToString(policy::WgcLowSourceState::kInputBelowTarget), "input-below-target");

    telemetry.recentInputMin250Fps = 120;
    telemetry.recentInputMin500Fps = 120;
    telemetry.recentDeliveredFps = 119;
    EXPECT_EQ(policy::ClassifyWgcLowSourceState(telemetry), policy::WgcLowSourceState::kHealthy);

    telemetry.recentDeliveredFps = 117;
    EXPECT_EQ(policy::ClassifyWgcLowSourceState(telemetry), policy::WgcLowSourceState::kDeliveryBelowTarget);
    EXPECT_STREQ(policy::WgcLowSourceStateToString(policy::WgcLowSourceState::kDeliveryBelowTarget),
                 "delivery-below-target");

    telemetry.recentDeliveredFps = 120;
    telemetry.emptyTickPermille = policy::kWgcLowSourceEmptyTickPermille;
    EXPECT_EQ(policy::ClassifyWgcLowSourceState(telemetry), policy::WgcLowSourceState::kQueueEmptyPressure);
    EXPECT_STREQ(policy::WgcLowSourceStateToString(policy::WgcLowSourceState::kQueueEmptyPressure),
                 "queue-empty-pressure");
}

TEST(CapturePipelinePolicyTest, HeldModeUpdateAppliesEnterExitHoldsAndImmediateExit) {
    auto inactivePending = policy::UpdateHeldMode(false, 0, 100, true, false, false, 120, 250);
    EXPECT_FALSE(inactivePending.active);
    EXPECT_EQ(inactivePending.stateChangeTick, 100u);
    EXPECT_EQ(inactivePending.transition, policy::HeldModeTransition::kNone);

    auto inactiveStillPending = policy::UpdateHeldMode(false, 100, 219, true, false, false, 120, 250);
    EXPECT_FALSE(inactiveStillPending.active);
    EXPECT_EQ(inactiveStillPending.stateChangeTick, 100u);
    EXPECT_EQ(inactiveStillPending.transition, policy::HeldModeTransition::kNone);

    auto entered = policy::UpdateHeldMode(false, 100, 220, true, false, false, 120, 250);
    EXPECT_TRUE(entered.active);
    EXPECT_EQ(entered.stateChangeTick, 0u);
    EXPECT_EQ(entered.transition, policy::HeldModeTransition::kEntered);

    auto activePendingExit = policy::UpdateHeldMode(true, 0, 300, false, true, false, 120, 250);
    EXPECT_TRUE(activePendingExit.active);
    EXPECT_EQ(activePendingExit.stateChangeTick, 300u);
    EXPECT_EQ(activePendingExit.transition, policy::HeldModeTransition::kNone);

    auto activeStillPendingExit = policy::UpdateHeldMode(true, 300, 549, false, true, false, 120, 250);
    EXPECT_TRUE(activeStillPendingExit.active);
    EXPECT_EQ(activeStillPendingExit.stateChangeTick, 300u);
    EXPECT_EQ(activeStillPendingExit.transition, policy::HeldModeTransition::kNone);

    auto exited = policy::UpdateHeldMode(true, 300, 550, false, true, false, 120, 250);
    EXPECT_FALSE(exited.active);
    EXPECT_EQ(exited.stateChangeTick, 0u);
    EXPECT_EQ(exited.transition, policy::HeldModeTransition::kExited);
    EXPECT_FALSE(exited.immediate);

    auto immediateExit = policy::UpdateHeldMode(true, 400, 401, false, false, true, 120, 250);
    EXPECT_FALSE(immediateExit.active);
    EXPECT_EQ(immediateExit.stateChangeTick, 0u);
    EXPECT_EQ(immediateExit.transition, policy::HeldModeTransition::kExited);
    EXPECT_TRUE(immediateExit.immediate);
}

TEST(CapturePipelinePolicyTest, WgcAdaptiveHeadroomRequiresHealthySourceReserve) {
    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;
    telemetry.recentDeliveredFps = 120;
    telemetry.recentDeliveredMin250Fps = 120;
    telemetry.recentDeliveredMin500Fps = 120;
    telemetry.recentInputMin250Fps = 123;
    telemetry.recentInputMin500Fps = 123;
    telemetry.emptyTickPermille = 20;
    telemetry.duplicateRatio = 0.05;

    EXPECT_TRUE(policy::ShouldAllowWgcAdaptiveHeadroom(telemetry, 20, false, false));
    EXPECT_FALSE(policy::ShouldAllowWgcAdaptiveHeadroom(telemetry, policy::kWgcReservePressurePermille, false, false));
    EXPECT_FALSE(policy::ShouldAllowWgcAdaptiveHeadroom(telemetry, 20, true, false));
    EXPECT_FALSE(policy::ShouldAllowWgcAdaptiveHeadroom(telemetry, 20, false, true));

    telemetry.duplicateRatio = 0.25;
    EXPECT_FALSE(policy::ShouldAllowWgcAdaptiveHeadroom(telemetry, 20, false, false));

    telemetry.duplicateRatio = 0.05;
    telemetry.recentDeliveredMin250Fps = 117;
    EXPECT_FALSE(policy::ShouldAllowWgcAdaptiveHeadroom(telemetry, 20, false, false));
}

TEST(CapturePipelinePolicyTest, WgcExplicitTenBitDisallowsBgraFallback) {
    EXPECT_FALSE(policy::ShouldAllowBgra8WgcFallback(true, false));
    EXPECT_FALSE(policy::ShouldAllowBgra8WgcFallback(true, true));
    EXPECT_FALSE(policy::ShouldAllowBgra8WgcFallback(false, true));
    EXPECT_TRUE(policy::ShouldAllowBgra8WgcFallback(false, false));
}

TEST(CapturePipelinePolicyTest, WgcStartupBarrierDelaysUntilFutureFreshFrame) {
    EXPECT_TRUE(policy::ShouldUseWgcCfrStartupSyncBarrier(true, false, 100));
    EXPECT_FALSE(policy::ShouldUseWgcCfrStartupSyncBarrier(true, true, 100));
    EXPECT_FALSE(policy::ShouldUseWgcCfrStartupSyncBarrier(false, false, 100));
    EXPECT_FALSE(policy::ShouldUseWgcCfrStartupSyncBarrier(true, false, 0));
    EXPECT_EQ(policy::GetWgcCfrStartupPreLiveDelayTicks(100), 2400);
    EXPECT_EQ(policy::GetWgcCfrStartupPreLiveDelayTicks(0), 0);
    EXPECT_EQ(policy::GetWgcCfrStartupPreLiveDelayTicks(-100), 0);
    EXPECT_EQ(policy::GetWgcStartupBarrierQpc(1000, 100), 1100);
    EXPECT_EQ(policy::GetWgcStartupBarrierQpc(1000, 0), 1000);
    EXPECT_EQ(policy::GetWgcStartupAudioAnchorQpc(1000, 35), 1035);
    EXPECT_EQ(policy::GetWgcStartupAudioAnchorQpc(1000, 35 + 133), 1168);
    EXPECT_EQ(policy::GetWgcStartupAudioAnchorQpc(1000, 0), 1000);
    EXPECT_EQ(policy::GetWgcStartupAudioAnchorQpc(0, 35), 0);
    EXPECT_FALSE(policy::IsWgcFramePastStartupBarrier(1099, 1100));
    EXPECT_TRUE(policy::IsWgcFramePastStartupBarrier(1100, 1100));
    EXPECT_TRUE(policy::IsWgcFramePastStartupBarrier(1200, 1100));
}

TEST(CapturePipelinePolicyTest, WgcStartupSmoothnessAttemptUsesBudgetNotEarlySourceRate) {
    EXPECT_FALSE(policy::ShouldArmWgcSmoothnessBufferForSourceRate(
        /*outputFps=*/120, /*recentInputMin250Fps=*/4, /*recentInputMin500Fps=*/4));
    EXPECT_TRUE(policy::ShouldAttemptWgcStartupSmoothnessBuffer(
        /*enabled=*/true, /*useVfr=*/false, /*avContentDelayActive=*/true,
        /*targetIntervalTicks=*/100, /*retainedExtraFrames=*/13));
}

TEST(CapturePipelinePolicyTest, WgcStartupSmoothnessAttemptRequiresCfrAvDelayAndBudget) {
    EXPECT_TRUE(policy::ShouldAttemptWgcStartupSmoothnessBuffer(true, false, true, 100, 1));
    EXPECT_FALSE(policy::ShouldAttemptWgcStartupSmoothnessBuffer(false, false, true, 100, 1));
    EXPECT_FALSE(policy::ShouldAttemptWgcStartupSmoothnessBuffer(true, true, true, 100, 1));
    EXPECT_FALSE(policy::ShouldAttemptWgcStartupSmoothnessBuffer(true, false, false, 100, 1));
    EXPECT_FALSE(policy::ShouldAttemptWgcStartupSmoothnessBuffer(true, false, true, 0, 1));
    EXPECT_FALSE(policy::ShouldAttemptWgcStartupSmoothnessBuffer(true, false, true, 100, 0));
}

TEST(CapturePipelinePolicyTest, WgcStartupSmoothnessDelayUsesActualReserveAndBudget) {
    EXPECT_EQ(policy::GetWgcStartupSmoothnessTargetDelayQpc(/*retainedExtraFrames=*/13,
                                                            /*targetIntervalTicks=*/100),
              1300);
    EXPECT_EQ(policy::GetWgcFrameCountForDurationMs(/*fps=*/120, /*durationMs=*/300), 36u);
    EXPECT_EQ(policy::GetWgcStartupSmoothnessTargetDelayQpc(/*retainedExtraFrames=*/45,
                                                            /*targetIntervalTicks=*/100, /*outputFps=*/120,
                                                            /*maxSmoothnessMs=*/300),
              3600);
    EXPECT_EQ(policy::GetWgcStartupSmoothnessTargetDelayQpc(/*retainedExtraFrames=*/0,
                                                            /*targetIntervalTicks=*/100),
              0);
    EXPECT_EQ(policy::GetWgcStartupSmoothnessTargetDelayQpc(/*retainedExtraFrames=*/13,
                                                            /*targetIntervalTicks=*/0),
              0);

    EXPECT_EQ(policy::SelectWgcStartupSmoothnessExtraDelayQpc(/*actualStartupDelayQpc=*/900,
                                                              /*avContentDelayQpc=*/400,
                                                              /*smoothnessTargetDelayQpc=*/1300),
              500);
    EXPECT_EQ(policy::SelectWgcStartupSmoothnessExtraDelayQpc(/*actualStartupDelayQpc=*/2400,
                                                              /*avContentDelayQpc=*/400,
                                                              /*smoothnessTargetDelayQpc=*/1300),
              1300);
    EXPECT_EQ(policy::SelectWgcStartupSmoothnessExtraDelayQpc(/*actualStartupDelayQpc=*/300,
                                                              /*avContentDelayQpc=*/400,
                                                              /*smoothnessTargetDelayQpc=*/1300),
              0);
    EXPECT_EQ(policy::SelectWgcStartupSmoothnessExtraDelayQpc(/*actualStartupDelayQpc=*/900,
                                                              /*avContentDelayQpc=*/400,
                                                              /*smoothnessTargetDelayQpc=*/0),
              0);
}

TEST(CapturePipelinePolicyTest, WgcStartupActiveDelayCapsUnderfedPileupToJitterFloor) {
    // Underfed startup + source at/above CFR + a deep accidental pile-up: cap DOWN to the measured
    // jitter floor so the read delay keeps fresh-frame headroom (prevents startup-timing-dependent
    // deep-lock repeat clustering). Mirrors the Fortnite regression (pile-up ~222ms, floor ~16ms).
    EXPECT_EQ(policy::ResolveWgcStartupSmoothnessActiveDelayQpc(/*pileupExtraDelayQpc=*/2220000,
                                                                /*jitterFloorDelayQpc=*/160000,
                                                                /*startupUnderfed=*/true,
                                                                /*sourceAtOrAboveCfrTarget=*/true),
              160000);

    // Never INCREASE a shallow pile-up: floor deeper than pile-up -> keep pile-up.
    EXPECT_EQ(policy::ResolveWgcStartupSmoothnessActiveDelayQpc(/*pileupExtraDelayQpc=*/100000,
                                                                /*jitterFloorDelayQpc=*/160000,
                                                                /*startupUnderfed=*/true,
                                                                /*sourceAtOrAboveCfrTarget=*/true),
              100000);

    // Source BELOW the CFR target: preserve the deep reservoir (iter-6 lull absorption), no cap.
    EXPECT_EQ(policy::ResolveWgcStartupSmoothnessActiveDelayQpc(/*pileupExtraDelayQpc=*/2220000,
                                                                /*jitterFloorDelayQpc=*/160000,
                                                                /*startupUnderfed=*/true,
                                                                /*sourceAtOrAboveCfrTarget=*/false),
              2220000);

    // Reservoir target reached (not underfed): validated pile-up behavior unchanged.
    EXPECT_EQ(policy::ResolveWgcStartupSmoothnessActiveDelayQpc(/*pileupExtraDelayQpc=*/2220000,
                                                                /*jitterFloorDelayQpc=*/160000,
                                                                /*startupUnderfed=*/false,
                                                                /*sourceAtOrAboveCfrTarget=*/true),
              2220000);

    // No measured floor available: no cap.
    EXPECT_EQ(policy::ResolveWgcStartupSmoothnessActiveDelayQpc(/*pileupExtraDelayQpc=*/2220000,
                                                                /*jitterFloorDelayQpc=*/0,
                                                                /*startupUnderfed=*/true,
                                                                /*sourceAtOrAboveCfrTarget=*/true),
              2220000);
}

TEST(CapturePipelinePolicyTest, WgcStartupCandidateCadenceIgnoresPreLiveMinWindowPollution) {
    constexpr int64_t targetIntervalQpc = 1000;

    EXPECT_TRUE(policy::IsWgcStartupCandidateCadenceAtOrAboveCfrTarget(
        /*candidateCount=*/30, /*candidateSpanQpc=*/29 * targetIntervalQpc, targetIntervalQpc));
    EXPECT_TRUE(policy::IsWgcStartupCandidateCadenceAtOrAboveCfrTarget(
        /*candidateCount=*/30, /*candidateSpanQpc=*/24 * targetIntervalQpc, targetIntervalQpc));
    EXPECT_FALSE(policy::IsWgcStartupCandidateCadenceAtOrAboveCfrTarget(
        /*candidateCount=*/30, /*candidateSpanQpc=*/30 * targetIntervalQpc, targetIntervalQpc));

    // A single pair is too fragile to override the longer min-window evidence.
    EXPECT_FALSE(policy::IsWgcStartupCandidateCadenceAtOrAboveCfrTarget(2, targetIntervalQpc, targetIntervalQpc));
    EXPECT_FALSE(policy::IsWgcStartupCandidateCadenceAtOrAboveCfrTarget(30, 0, targetIntervalQpc));
    EXPECT_FALSE(policy::IsWgcStartupCandidateCadenceAtOrAboveCfrTarget(30, 29 * targetIntervalQpc, 0));
}

TEST(CapturePipelinePolicyTest, WgcStartupReserveWaitBudgetExtendsForSmoothnessReservoir) {
    EXPECT_EQ(policy::GetWgcStartupReserveWaitBudgetQpc(/*startupContentDelayTargetQpc=*/2700,
                                                        /*targetIntervalTicks=*/100,
                                                        /*smoothnessTargetDelayQpc=*/2400,
                                                        /*smoothnessStartupAttempted=*/true,
                                                        /*qpcTicksPerSecond=*/10000),
              10000);
    EXPECT_EQ(policy::GetWgcStartupReserveWaitBudgetQpc(/*startupContentDelayTargetQpc=*/15000,
                                                        /*targetIntervalTicks=*/100,
                                                        /*smoothnessTargetDelayQpc=*/5000,
                                                        /*smoothnessStartupAttempted=*/true,
                                                        /*qpcTicksPerSecond=*/10000),
              10000);
    EXPECT_EQ(policy::GetWgcStartupReserveWaitBudgetQpc(/*startupContentDelayTargetQpc=*/2700,
                                                        /*targetIntervalTicks=*/100,
                                                        /*smoothnessTargetDelayQpc=*/0,
                                                        /*smoothnessStartupAttempted=*/false),
              2900);
    EXPECT_EQ(policy::GetWgcStartupReserveWaitBudgetQpc(/*startupContentDelayTargetQpc=*/0,
                                                        /*targetIntervalTicks=*/100,
                                                        /*smoothnessTargetDelayQpc=*/2400,
                                                        /*smoothnessStartupAttempted=*/true),
              0);
}

TEST(CapturePipelinePolicyTest, CfrTimelineStartContractKeepsVideoReserveAndAudioAnchorAtomic) {
    const auto contract = policy::BuildCfrTimelineStartContract(
        /*videoOriginQpc=*/100000, /*liveQpc=*/103300, /*renderLoopbackLatencyQpc=*/500);

    ASSERT_TRUE(contract.valid);
    EXPECT_EQ(contract.contentDelayQpc, 3300);
    EXPECT_EQ(contract.smoothnessReserveQpc, 2800);
    EXPECT_EQ(contract.audioAnchorQpc, 100500);
    EXPECT_EQ(contract.liveQpc - contract.audioAnchorQpc, contract.smoothnessReserveQpc);

    const auto rebased = policy::RebaseCfrTimelineStartContract(contract, 200000);
    ASSERT_TRUE(rebased.valid);
    EXPECT_EQ(rebased.videoOriginQpc, 200000);
    EXPECT_EQ(rebased.liveQpc, 203300);
    EXPECT_EQ(rebased.audioAnchorQpc, 200500);
    EXPECT_EQ(rebased.contentDelayQpc, contract.contentDelayQpc);
    EXPECT_EQ(rebased.smoothnessReserveQpc, contract.smoothnessReserveQpc);

    EXPECT_FALSE(policy::BuildCfrTimelineStartContract(100000, 100400, 500).valid);
    EXPECT_FALSE(policy::BuildCfrTimelineStartContract(100000, 99999, 0).valid);
    EXPECT_FALSE(policy::RebaseCfrTimelineStartContract({}, 200000).valid);
}

TEST(CapturePipelinePolicyTest, PartialStartupReservoirReselectsNearestMatchingContractFrame) {
    const int64_t candidates[] = {1000, 1100, 1180, 1240};
    EXPECT_EQ(policy::SelectNearestMonotonicTimestampIndex(candidates, 4, /*targetQpc=*/1140), 2u);
    EXPECT_EQ(policy::SelectNearestMonotonicTimestampIndex(candidates, 4, /*targetQpc=*/1050), 1u);
}

TEST(CapturePipelinePolicyTest, WgcPreLiveStartupDelayExcludesSmoothnessReservoir) {
    const int64_t targetIntervalTicks = 100;
    const int64_t preLiveDelayTicks = policy::GetWgcCfrStartupPreLiveDelayTicks(targetIntervalTicks);
    const int64_t smoothnessTargetDelayTicks =
        policy::GetWgcStartupSmoothnessTargetDelayQpc(/*retainedExtraFrames=*/13, targetIntervalTicks);

    EXPECT_EQ(preLiveDelayTicks, 2400);
    EXPECT_EQ(smoothnessTargetDelayTicks, 1300);
    EXPECT_EQ(policy::GetWgcCfrStartupPreLiveDelayTicks(targetIntervalTicks), preLiveDelayTicks);
}

TEST(CapturePipelinePolicyTest, WgcStartupReserveSelectionUsesDelayedCandidateWhenSpanExists) {
    const int64_t candidates[] = {1000, 1100, 1200, 1300, 1400};
    const auto selection = policy::SelectWgcStartupReserveCandidate(candidates, 5, 300, 50);

    EXPECT_TRUE(selection.usedDelayReserve);
    EXPECT_EQ(selection.selectedIndex, 1u);
    EXPECT_EQ(selection.targetSelectionQpc, 1100);
    EXPECT_EQ(selection.reserveSpanQpc, 400);
    EXPECT_EQ(selection.selectedDelayQpc, 300);
}

TEST(CapturePipelinePolicyTest, WgcStartupReserveSelectionFallsBackWhenSpanIsInsufficient) {
    const int64_t candidates[] = {1000, 1100, 1180};
    const auto selection = policy::SelectWgcStartupReserveCandidate(candidates, 3, 300, 40);

    EXPECT_FALSE(selection.usedDelayReserve);
    EXPECT_EQ(selection.selectedIndex, 2u);
    EXPECT_EQ(selection.reserveSpanQpc, 180);
    EXPECT_EQ(selection.selectedDelayQpc, 0);
}

TEST(CapturePipelinePolicyTest, WgcSyncDelayHoldAttributionSeparatesSourceLimitedPressure) {
    EXPECT_TRUE(policy::IsWgcSyncDelayHoldSourceLimited(120, 120, 116, 20, false, false, false));
    EXPECT_TRUE(policy::IsWgcSyncDelayHoldSourceLimited(120, 120, 120, policy::kWgcRecoveryEmptyTickPermille, false,
                                                        false, false));
    EXPECT_TRUE(policy::IsWgcSyncDelayHoldSourceLimited(120, 120, 120, 0, true, false, false));
    EXPECT_TRUE(policy::IsWgcSyncDelayHoldSourceLimited(120, 120, 120, 0, false, false, false, true));
    EXPECT_FALSE(policy::IsWgcSyncDelayHoldSourceLimited(120, 120, 120, 0, false, false, false));
}

TEST(CapturePipelinePolicyTest, WgcActiveDelaySourceRecoveryClassifiesSevereStalls) {
    EXPECT_TRUE(
        policy::IsWgcSevereSourceStallForActiveDelay(120, 120, 120, policy::kWgcDeepUnderfeedEmptyTickPermille, 8));
    EXPECT_TRUE(policy::IsWgcSevereSourceStallForActiveDelay(120, 120, 8, 20, 0));
    EXPECT_TRUE(policy::IsWgcSevereSourceStallForActiveDelay(120, 8, 120, 20, 0));
    EXPECT_FALSE(policy::IsWgcSevereSourceStallForActiveDelay(120, 120, 8, 20, 4));
    EXPECT_FALSE(policy::IsWgcSevereSourceStallForActiveDelay(120, 120, 120, 20, 0));
    EXPECT_FALSE(policy::IsWgcSevereSourceStallForActiveDelay(0, 0, 0, 1000, 0));
}

TEST(CapturePipelinePolicyTest, WgcCfrSourceCaptureAlwaysUsesMaxProducerRate) {
    EXPECT_EQ(policy::GetWgcCfrProducerTargetFps(0), 0u);
    EXPECT_EQ(policy::GetWgcCfrProducerTargetFps(60), 0u);
    EXPECT_EQ(policy::GetWgcCfrProducerTargetFps(120), 0u);
    EXPECT_EQ(policy::GetWgcCfrProducerTargetFps(1000), 0u);
}

TEST(CapturePipelinePolicyTest, WgcFiniteProducerIntervalsCanAliasToHalfRate) {
    EXPECT_EQ(policy::EstimateWgcMinUpdateIntervalDeliveryFps(138, 0), 138u);
    EXPECT_EQ(policy::EstimateWgcMinUpdateIntervalDeliveryFps(138, 120), 69u);
    EXPECT_EQ(policy::EstimateWgcMinUpdateIntervalDeliveryFps(138, 150), 138u);
    EXPECT_EQ(policy::EstimateWgcMinUpdateIntervalDeliveryFps(160, 150), 80u);
    EXPECT_EQ(policy::EstimateWgcMinUpdateIntervalDeliveryFps(144, 120), 72u);
    EXPECT_EQ(policy::EstimateWgcMinUpdateIntervalDeliveryFps(120, 120), 120u);
}

TEST(CapturePipelinePolicyTest, WgcRecordingAvoidsNativeCursorCapture) {
    EXPECT_FALSE(policy::ShouldUseNativeWgcCursorCapture(false));
    EXPECT_FALSE(policy::ShouldUseNativeWgcCursorCapture(true));
}

TEST(CapturePipelinePolicyTest, AutoWgcUsesForegroundFullscreenWindowOnlyWhenNoSourcePidAndNoMatchedWindow) {
    EXPECT_TRUE(policy::ShouldPreferForegroundFullscreenWindowForAutoWgc(
        /*autoCaptureConfig=*/true, /*explicitInjectConfig=*/false, /*injectWhitelisted=*/false,
        /*hasSourcePid=*/false, /*hasMatchedConfiguredWgcWindow=*/false, /*foregroundUsable=*/true,
        /*foregroundFullscreenLike=*/true));

    EXPECT_FALSE(policy::ShouldPreferForegroundFullscreenWindowForAutoWgc(
        /*autoCaptureConfig=*/false, /*explicitInjectConfig=*/false, /*injectWhitelisted=*/false,
        /*hasSourcePid=*/false, /*hasMatchedConfiguredWgcWindow=*/false, /*foregroundUsable=*/true,
        /*foregroundFullscreenLike=*/true));
    EXPECT_FALSE(policy::ShouldPreferForegroundFullscreenWindowForAutoWgc(
        /*autoCaptureConfig=*/true, /*explicitInjectConfig=*/true, /*injectWhitelisted=*/false,
        /*hasSourcePid=*/false, /*hasMatchedConfiguredWgcWindow=*/false, /*foregroundUsable=*/true,
        /*foregroundFullscreenLike=*/true));
    EXPECT_FALSE(policy::ShouldPreferForegroundFullscreenWindowForAutoWgc(
        /*autoCaptureConfig=*/true, /*explicitInjectConfig=*/false, /*injectWhitelisted=*/true,
        /*hasSourcePid=*/false, /*hasMatchedConfiguredWgcWindow=*/false, /*foregroundUsable=*/true,
        /*foregroundFullscreenLike=*/true));
    EXPECT_FALSE(policy::ShouldPreferForegroundFullscreenWindowForAutoWgc(
        /*autoCaptureConfig=*/true, /*explicitInjectConfig=*/false, /*injectWhitelisted=*/false,
        /*hasSourcePid=*/true, /*hasMatchedConfiguredWgcWindow=*/false, /*foregroundUsable=*/true,
        /*foregroundFullscreenLike=*/true));
    EXPECT_FALSE(policy::ShouldPreferForegroundFullscreenWindowForAutoWgc(
        /*autoCaptureConfig=*/true, /*explicitInjectConfig=*/false, /*injectWhitelisted=*/false,
        /*hasSourcePid=*/false, /*hasMatchedConfiguredWgcWindow=*/true, /*foregroundUsable=*/true,
        /*foregroundFullscreenLike=*/true));
    EXPECT_FALSE(policy::ShouldPreferForegroundFullscreenWindowForAutoWgc(
        /*autoCaptureConfig=*/true, /*explicitInjectConfig=*/false, /*injectWhitelisted=*/false,
        /*hasSourcePid=*/false, /*hasMatchedConfiguredWgcWindow=*/false, /*foregroundUsable=*/true,
        /*foregroundFullscreenLike=*/false));
}

TEST(CapturePipelinePolicyTest, AdaptiveEncoderGpuPriorityUsesBudgetHysteresis) {
    EXPECT_FALSE(policy::ShouldRaiseAdaptiveEncoderGpuPriority(5.9, 8.0));
    EXPECT_TRUE(policy::ShouldRaiseAdaptiveEncoderGpuPriority(6.0, 8.0));
    EXPECT_TRUE(policy::ShouldRaiseAdaptiveEncoderGpuPriority(8.0, 8.0));

    EXPECT_TRUE(policy::ShouldRestoreNeutralEncoderGpuPriority(4.0, 8.0));
    EXPECT_FALSE(policy::ShouldRestoreNeutralEncoderGpuPriority(4.1, 8.0));

    EXPECT_TRUE(policy::ShouldResetAdaptiveEncoderGpuPriorityPressure(4.0, 8.0));
    EXPECT_FALSE(policy::ShouldResetAdaptiveEncoderGpuPriorityPressure(5.9, 8.0));
    EXPECT_FALSE(policy::ShouldResetAdaptiveEncoderGpuPriorityPressure(6.0, 8.0));

    EXPECT_TRUE(policy::IsAdaptiveEncoderGpuPriorityPressureActive(1.0, 8.0, true));
    EXPECT_TRUE(policy::IsAdaptiveEncoderGpuPriorityPressureActive(6.0, 8.0, false));
    EXPECT_FALSE(policy::IsAdaptiveEncoderGpuPriorityPressureActive(5.9, 8.0, false));
    EXPECT_FALSE(policy::ShouldResetAdaptiveEncoderGpuPriorityPressure(4.0, 8.0, true));
}

TEST(CapturePipelinePolicyTest, WgcOverlayWarningSuppressesEncoderPressureWhenSourceLimited) {
    EXPECT_FALSE(policy::IsWgcCaptureLimitedForOverlay(0));
    EXPECT_TRUE(policy::IsWgcCaptureLimitedForOverlay(policy::kWgcCaptureHealthFlagSourceStarved));
    EXPECT_TRUE(policy::IsWgcCaptureLimitedForOverlay(policy::kWgcCaptureHealthFlagSchedulerLimited));

    EXPECT_EQ(policy::SelectWgcOverlayWarningKind(0, 0), policy::kOverlayWarningNone);
    EXPECT_EQ(policy::SelectWgcOverlayWarningKind(policy::kEncoderOverloadFlagEncoder, 0),
              policy::kOverlayWarningEncoderOverload);
    EXPECT_EQ(policy::SelectWgcOverlayWarningKind(policy::kEncoderOverloadFlagMux, 0), policy::kOverlayWarningNone);
    EXPECT_EQ(policy::SelectWgcOverlayWarningKind(policy::kEncoderOverloadFlagEncoder,
                                                  policy::kWgcCaptureHealthFlagSourceStarved),
              policy::kOverlayWarningNone);
    EXPECT_EQ(policy::SelectWgcOverlayWarningKind(policy::kEncoderOverloadFlagEncoder,
                                                  policy::kWgcCaptureHealthFlagSchedulerLimited),
              policy::kOverlayWarningNone);
    EXPECT_EQ(policy::SelectWgcOverlayWarningKind(0, policy::kWgcCaptureHealthFlagSourceStarved),
              policy::kOverlayWarningNone);
}

