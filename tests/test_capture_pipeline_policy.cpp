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

TEST(CapturePipelinePolicyTest, WgcStopDrainRequiresCfrRepeatSourceBeforeAbort) {
    EXPECT_FALSE(policy::CanDrainOutstandingWgcTicks(false, false, false, false));
    EXPECT_TRUE(policy::CanDrainOutstandingWgcTicks(false, false, true, false));
    EXPECT_TRUE(policy::CanDrainOutstandingWgcTicks(false, false, false, true));
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
    EXPECT_EQ(policy::GetMinBufferedInjectFrames(0, false), 0u);
    EXPECT_EQ(policy::GetMinBufferedInjectFrames(3, false), 3u);
    EXPECT_EQ(policy::GetMinBufferedInjectFrames(3, true), 2u);
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
    EXPECT_FALSE(policy::IsWgcFramePastStartupBarrier(1099, 1100));
    EXPECT_TRUE(policy::IsWgcFramePastStartupBarrier(1100, 1100));
    EXPECT_TRUE(policy::IsWgcFramePastStartupBarrier(1200, 1100));
}

TEST(CapturePipelinePolicyTest, WgcCfrOvercaptureCapUsesTwentyFivePercentHeadroom) {
    EXPECT_EQ(policy::GetWgcCfrOvercaptureTargetFps(0), 0u);
    EXPECT_EQ(policy::GetWgcCfrOvercaptureTargetFps(60), 75u);
    EXPECT_EQ(policy::GetWgcCfrOvercaptureTargetFps(120), 150u);
    EXPECT_EQ(policy::GetWgcCfrOvercaptureTargetFps(143), 179u);
}

TEST(CapturePipelinePolicyTest, WgcOvercaptureSwitchesToMaxRateDuringRecovery) {
    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;
    telemetry.recentDeliveredMin250Fps = 120;
    telemetry.recentDeliveredMin500Fps = 120;
    telemetry.recentInputMin250Fps = 122;
    telemetry.recentInputMin500Fps = 122;

    EXPECT_FALSE(policy::ShouldUseWgcMaxRateForRecovery(telemetry, 20, false, false));
    EXPECT_TRUE(policy::ShouldUseWgcMaxRateForRecovery(telemetry, 20, true, false));
    EXPECT_TRUE(policy::ShouldUseWgcMaxRateForRecovery(telemetry, 20, false, true));
    EXPECT_TRUE(policy::ShouldUseWgcMaxRateForRecovery(telemetry, policy::kWgcLowSourceEmptyTickPermille, false, false));

    telemetry.recentInputMin250Fps = 119;
    EXPECT_TRUE(policy::ShouldUseWgcMaxRateForRecovery(telemetry, 20, false, false));
}

TEST(CapturePipelinePolicyTest, WgcOvercaptureRestoresOnlyAfterStableFreshSource) {
    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;
    telemetry.recentDeliveredMin250Fps = 120;
    telemetry.recentDeliveredMin500Fps = 120;
    telemetry.recentInputMin250Fps = 122;
    telemetry.recentInputMin500Fps = 122;

    EXPECT_FALSE(policy::ShouldRestoreWgcOvercaptureCap(telemetry, 20, 1999));
    EXPECT_TRUE(policy::ShouldRestoreWgcOvercaptureCap(telemetry, 20, 2000));

    telemetry.recentInputMin250Fps = 119;
    EXPECT_FALSE(policy::ShouldRestoreWgcOvercaptureCap(telemetry, 20, 2500));

    telemetry.recentInputMin250Fps = 122;
    EXPECT_FALSE(policy::ShouldRestoreWgcOvercaptureCap(telemetry, 80, 2500));
}

TEST(CapturePipelinePolicyTest, AdaptiveEncoderGpuPriorityUsesBudgetHysteresis) {
    EXPECT_FALSE(policy::ShouldRaiseAdaptiveEncoderGpuPriority(5.9, 8.0));
    EXPECT_TRUE(policy::ShouldRaiseAdaptiveEncoderGpuPriority(6.0, 8.0));
    EXPECT_TRUE(policy::ShouldRaiseAdaptiveEncoderGpuPriority(8.0, 8.0));

    EXPECT_TRUE(policy::ShouldRestoreNeutralEncoderGpuPriority(4.0, 8.0));
    EXPECT_FALSE(policy::ShouldRestoreNeutralEncoderGpuPriority(4.1, 8.0));
}

TEST(CapturePipelinePolicyTest, WgcWarmupCommitRequiresStableBufferedSource) {
    EXPECT_FALSE(policy::ShouldCommitWgcWarmup(false, 3, policy::kRecordingWarmupMaxMs, 120.0, 120));
    EXPECT_FALSE(policy::ShouldCommitWgcWarmup(true, 2, policy::kRecordingWarmupMinMs, 120.0, 120));
    EXPECT_TRUE(policy::ShouldCommitWgcWarmup(true, 3, policy::kRecordingWarmupMinMs, 120.0, 120));
    EXPECT_FALSE(policy::ShouldCommitWgcWarmup(true, 3, policy::kRecordingWarmupMinMs, 115.0, 120));
}

TEST(CapturePipelinePolicyTest, WgcWarmupTelemetryStartsUnready) {
    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;

    EXPECT_EQ(policy::ClassifyWgcLowSourceState(telemetry), policy::WgcLowSourceState::kInputBelowTarget);
    EXPECT_EQ(policy::ClassifyWgcLiveRecoveryState(telemetry, 4, false), policy::WgcLiveRecoveryState::kSourceStarved);
}

TEST(CapturePipelinePolicyTest, WgcLowSourceSelectionClampProtectsFragileQueue) {
    EXPECT_EQ(policy::ClampWgcSelectionIndexForLowSource(3, 4, 2, 116, 116, 120, 130), 0u);
    EXPECT_EQ(policy::ClampWgcSelectionIndexForLowSource(3, 4, 2, 119, 119, 120, 90), 1u);
    EXPECT_EQ(policy::ClampWgcSelectionIndexForLowSource(1, 4, 4, 130, 130, 120, 10), 1u);
    EXPECT_EQ(policy::ClampWgcSelectionIndexForLowSource(3, 4, 2, 116, 116, 120, 130, true), 3u);
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

    EXPECT_EQ(minFreshNormal, 1400);
    EXPECT_EQ(minFreshLowSource, 1200);
    EXPECT_TRUE(policy::IsWgcTimestampFreshEnough(1400, minFreshNormal));
    EXPECT_FALSE(policy::IsWgcTimestampFreshEnough(1399, minFreshNormal));
}

TEST(CapturePipelinePolicyTest, WgcStaleUniqueFallbackAllowsOneExtraTickOfLag) {
    const int64_t targetIntervalTicks = 100;
    EXPECT_EQ(policy::GetWgcStaleUniqueFallbackMinTimestampQpc(1000, 1500, targetIntervalTicks, false, false), 1300);
    EXPECT_EQ(policy::GetWgcStaleUniqueFallbackMinTimestampQpc(1000, 1500, targetIntervalTicks, true, false), 1100);
    EXPECT_EQ(policy::GetWgcStaleUniqueFallbackMinTimestampQpc(1000, 1500, targetIntervalTicks, true, true), 1001);
    EXPECT_EQ(policy::GetWgcStaleUniqueFallbackMinTimestampQpc(100, 1500, targetIntervalTicks, true, true), 700);
}

TEST(CapturePipelinePolicyTest, WgcStaleUniqueFallbackStillRequiresNewSourceTimestamp) {
    const int64_t targetIntervalTicks = 100;
    EXPECT_EQ(policy::GetWgcStaleUniqueFallbackMinTimestampQpc(1499, 1500, targetIntervalTicks, false, false), 1500);
    EXPECT_EQ(policy::GetWgcStaleUniqueFallbackMinTimestampQpc(1500, 1500, targetIntervalTicks, false, false), 1501);
}

TEST(CapturePipelinePolicyTest, WgcSelectionTargetDelaysLiveSelectionByOneTick) {
    EXPECT_EQ(policy::GetWgcSelectionTargetQpc(1500, 1400, 100, true), 1400);
    EXPECT_EQ(policy::GetWgcSelectionTargetQpc(1500, 1400, 100, false), 1500);
    EXPECT_EQ(policy::GetWgcSelectionTargetQpc(0, 1400, 100, true), 1300);
    EXPECT_EQ(policy::GetWgcSelectionTargetQpc(50, 40, 100, true), 50);
}

TEST(CapturePipelinePolicyTest, WgcLiveRecoveryModeTracksSourceSchedulerAndEncoderStress) {
    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;
    telemetry.recentDeliveredFps = 82;
    telemetry.recentDeliveredMin250Fps = 80;
    telemetry.recentDeliveredMin500Fps = 80;
    telemetry.recentInputMin250Fps = 78;
    telemetry.recentInputMin500Fps = 78;
    telemetry.emptyTickPermille = 40;

    EXPECT_TRUE(policy::IsWgcSourceStarved(telemetry));
    EXPECT_FALSE(policy::IsWgcSchedulerDeliveryLimited(telemetry));
    EXPECT_TRUE(policy::ShouldEnterWgcLiveRecoveryMode(telemetry, 4, false));

    telemetry.recentInputMin250Fps = 122;
    telemetry.recentInputMin500Fps = 122;
    EXPECT_FALSE(policy::IsWgcSourceStarved(telemetry));
    EXPECT_TRUE(policy::IsWgcSchedulerDeliveryLimited(telemetry));
    EXPECT_TRUE(policy::ShouldEnterWgcLiveRecoveryMode(telemetry, 4, false));

    telemetry.recentDeliveredFps = 121;
    telemetry.recentDeliveredMin250Fps = 120;
    telemetry.recentDeliveredMin500Fps = 120;
    telemetry.emptyTickPermille = 20;
    EXPECT_FALSE(policy::IsWgcSchedulerDeliveryLimited(telemetry));
    EXPECT_TRUE(policy::ShouldEnterWgcLiveRecoveryMode(telemetry, policy::kWgcRecoveryEnterShortfallTicks, true));

    telemetry.bufferedWgcFrames = 2;
    EXPECT_FALSE(policy::ShouldExitWgcLiveRecoveryMode(telemetry, 2, false));
    EXPECT_TRUE(policy::ShouldExitWgcLiveRecoveryMode(telemetry, 1, false));
}

TEST(CapturePipelinePolicyTest, WgcLiveRecoveryStateClassificationIsExplicit) {
    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;
    telemetry.recentDeliveredFps = 120;
    telemetry.recentDeliveredMin250Fps = 120;
    telemetry.recentDeliveredMin500Fps = 120;
    telemetry.recentInputMin250Fps = 120;
    telemetry.recentInputMin500Fps = 120;
    telemetry.emptyTickPermille = 0;

    EXPECT_EQ(policy::ClassifyWgcLiveRecoveryState(telemetry, 0, false), policy::WgcLiveRecoveryState::kHealthy);
    EXPECT_STREQ(policy::WgcLiveRecoveryStateToString(policy::WgcLiveRecoveryState::kHealthy), "healthy");

    telemetry.recentInputMin250Fps = 100;
    telemetry.recentInputMin500Fps = 100;
    EXPECT_EQ(policy::ClassifyWgcLiveRecoveryState(telemetry, 4, false), policy::WgcLiveRecoveryState::kSourceStarved);
    EXPECT_STREQ(policy::WgcLiveRecoveryStateToString(policy::WgcLiveRecoveryState::kSourceStarved), "source-starved");

    telemetry.recentInputMin250Fps = 120;
    telemetry.recentInputMin500Fps = 120;
    telemetry.recentDeliveredMin250Fps = 100;
    telemetry.recentDeliveredMin500Fps = 100;
    EXPECT_EQ(policy::ClassifyWgcLiveRecoveryState(telemetry, 4, false),
              policy::WgcLiveRecoveryState::kSchedulerLimited);
    EXPECT_STREQ(policy::WgcLiveRecoveryStateToString(policy::WgcLiveRecoveryState::kSchedulerLimited),
                 "scheduler-limited");

    telemetry.recentDeliveredMin250Fps = 120;
    telemetry.recentDeliveredMin500Fps = 120;
    EXPECT_EQ(policy::ClassifyWgcLiveRecoveryState(telemetry, policy::kWgcRecoveryEnterShortfallTicks, true),
              policy::WgcLiveRecoveryState::kEncoderLimited);
    EXPECT_STREQ(policy::WgcLiveRecoveryStateToString(policy::WgcLiveRecoveryState::kEncoderLimited),
                 "encoder-limited");
}

TEST(CapturePipelinePolicyTest, WgcSelectionDelayIsUnconditional) {
    EXPECT_TRUE(policy::ShouldApplyWgcSelectionDelay(true, 0, false, true));
    EXPECT_TRUE(policy::ShouldApplyWgcSelectionDelay(true, 1, false, true));
    EXPECT_TRUE(policy::ShouldApplyWgcSelectionDelay(true, 0, true, true));
    EXPECT_TRUE(policy::ShouldApplyWgcSelectionDelay(true, 0, false, false));
    EXPECT_FALSE(policy::ShouldApplyWgcSelectionDelay(false, 0, false, true));
}

TEST(CapturePipelinePolicyTest, WgcReservePressureActivatesOnlyWithSustainedSingleFrameTicks) {
    EXPECT_FALSE(policy::IsWgcReservePressureActive(12, 20, 120));
    EXPECT_FALSE(policy::IsWgcReservePressureActive(20, 20, 120));
    EXPECT_TRUE(policy::IsWgcReservePressureActive(50, 80, 120));
    EXPECT_TRUE(policy::IsWgcReservePressureActive(6, 8, 30));
}

TEST(CapturePipelinePolicyTest, WgcReserveBiasPrefersEarlierFreshFrameWhenDifferenceIsSmall) {
    EXPECT_TRUE(policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(1000, 1040, 1020, 100, false, false, false));
    EXPECT_FALSE(policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(920, 1040, 1020, 100, false, false, false));
    EXPECT_FALSE(policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(900, 1040, 1020, 100, true, false, false));
    EXPECT_FALSE(policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(1000, 1040, 1020, 100, true, true, true));
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

TEST(CapturePipelinePolicyTest, WgcLowSourceClampLeavesHealthyQueueSelectionAlone) {
    EXPECT_EQ(policy::ClampWgcSelectionIndexForLowSource(2, 4, 4, 120, 120, 120, 20), 2u);
    EXPECT_EQ(policy::ClampWgcSelectionIndexForLowSource(3, 5, 5, 122, 122, 120, 10), 3u);
}

TEST(CapturePipelinePolicyTest, WgcDeepUnderfeedKeepsSparseBurstReserveWhenCandidateIsClose) {
    EXPECT_TRUE(policy::IsWgcDeepUnderfeed(120, 80, 82, 420));
    EXPECT_FALSE(policy::IsWgcDeepUnderfeed(120, 118, 120, 40));
    EXPECT_EQ(policy::ClampWgcSelectionIndexForLowSource(3, 4, 2, 40, 44, 120, 420), 3u);
    EXPECT_FALSE(policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(930, 1040, 1020, 100, false, false, false));
    EXPECT_TRUE(policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(930, 1040, 1020, 100, false, false, true));
}

TEST(CapturePipelinePolicyTest, WgcFreshFrameHoldStopsWhenAlreadyBehindOrPressured) {
    EXPECT_TRUE(policy::ShouldHoldSingleFreshWgcFrame(true, false, 118, 120, 0.98, 0, false, false, false));
    EXPECT_FALSE(policy::ShouldHoldSingleFreshWgcFrame(true, false, 118, 120, 0.98, 1, false, false, false));
    EXPECT_FALSE(policy::ShouldHoldSingleFreshWgcFrame(true, false, 118, 120, 0.98, 0, true, false, false));
    EXPECT_FALSE(policy::ShouldHoldSingleFreshWgcFrame(true, false, 118, 120, 0.98, 0, false, true, false));
    EXPECT_FALSE(policy::ShouldHoldSingleFreshWgcFrame(true, true, 80, 120, 0.65, 0, false, false, true));
    EXPECT_FALSE(policy::ShouldHoldSingleFreshWgcFrame(false, false, 120, 120, 1.00, 0, false, false, false));
}

TEST(CapturePipelinePolicyTest, WgcExtraCatchupRequiresSurplusAndNoEncoderBottleneck) {
    EXPECT_TRUE(policy::ShouldAllowWgcExtraCatchupTicks(false, 2, 1.0, 0));
    EXPECT_TRUE(policy::ShouldAllowWgcExtraCatchupTicks(false, 4, 2.5, 0));
    EXPECT_FALSE(policy::ShouldAllowWgcExtraCatchupTicks(true, 4, 2.5, 0));
    EXPECT_FALSE(policy::ShouldAllowWgcExtraCatchupTicks(false, 1, 2.5, 0));
    EXPECT_TRUE(
        policy::ShouldAllowWgcExtraCatchupTicks(false, 4, 0.99, policy::kCfrShortfallForceCatchupThresholdTicks - 1));
    EXPECT_TRUE(
        policy::ShouldAllowWgcExtraCatchupTicks(false, 4, 0.0, policy::kCfrShortfallForceCatchupThresholdTicks));
    EXPECT_TRUE(policy::ShouldAllowWgcExtraCatchupTicks(true, 4, 0.0, policy::kCfrShortfallForceCatchupThresholdTicks));
}

TEST(CapturePipelinePolicyTest, WgcCatchupStaysSingleTickWhenSourceIsDegraded) {
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, false, 4, 2.0, 8, 120, 80, 82, 0, true), 1u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, false, 4, 2.0, 8, 120, 84, 84, 420, false), 1u);
}

TEST(CapturePipelinePolicyTest, WgcHealthySourcePrefersSmoothnessOverLiveCatchup) {
    EXPECT_TRUE(policy::IsWgcSourceHealthyForLiveCatchup(120, 118, 120, 0, false));
    EXPECT_FALSE(policy::IsWgcSourceHealthyForLiveCatchup(120, 116, 120, 0, false));
    EXPECT_FALSE(policy::IsWgcSourceHealthyForLiveCatchup(120, 118, 119, 0, false));
    EXPECT_FALSE(policy::IsWgcSourceHealthyForLiveCatchup(120, 118, 120, 100, false));
    EXPECT_FALSE(policy::IsWgcSourceHealthyForLiveCatchup(120, 118, 120, 0, true));

    EXPECT_TRUE(policy::IsWgcSourceHealthyEnoughToSuppressEncoderLimitedCatchup(120, 120, 0, false));
    EXPECT_TRUE(policy::IsWgcSourceHealthyEnoughToSuppressEncoderLimitedCatchup(120, 124, 40, false));
    EXPECT_FALSE(policy::IsWgcSourceHealthyEnoughToSuppressEncoderLimitedCatchup(120, 119, 0, false));
    EXPECT_FALSE(policy::IsWgcSourceHealthyEnoughToSuppressEncoderLimitedCatchup(120, 124, 100, false));
    EXPECT_FALSE(policy::IsWgcSourceHealthyEnoughToSuppressEncoderLimitedCatchup(120, 124, 0, true));
}

TEST(CapturePipelinePolicyTest, WgcCoverageCatchupClampRelaxesAtSevereShortfall) {
    EXPECT_FALSE(policy::HasWgcSevereLiveShortfall(499.9));
    EXPECT_TRUE(policy::HasWgcSevereLiveShortfall(500.0));
    EXPECT_FALSE(policy::ShouldClampWgcCoverageCatchupToSingleTick(true, true, 300.0));
    EXPECT_FALSE(policy::ShouldClampWgcCoverageCatchupToSingleTick(true, true, 500.0));
    EXPECT_FALSE(policy::ShouldClampWgcCoverageCatchupToSingleTick(true, false, 300.0));
    EXPECT_FALSE(policy::ShouldClampWgcCoverageCatchupToSingleTick(false, true, 300.0));
    EXPECT_DOUBLE_EQ(policy::GetWgcForceCatchupBudgetFrameMultiplier(250.0), 4.0);
    EXPECT_DOUBLE_EQ(policy::GetWgcForceCatchupBudgetFrameMultiplier(500.0), 4.0);
}

TEST(CapturePipelinePolicyTest, WgcLiveRecoveryClampHugsLiveTimeAndDisablesReserveBias) {
    EXPECT_EQ(policy::ClampWgcSelectionTargetToLiveQpc(1000, 1600, 100, false, false, 0, false), 1500);
    EXPECT_EQ(policy::ClampWgcSelectionTargetToLiveQpc(1000, 1600, 100, false, true, 20, true), 1500);
    EXPECT_FALSE(
        policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(1000, 1040, 1020, 100, true, true, true, true));
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

TEST(CapturePipelinePolicyTest, TimerRebaseDebtDiscardIsEnabledForAllCfrModes) {
    EXPECT_TRUE(policy::ShouldDiscardCfrTimerRebaseDebt(false));
    EXPECT_TRUE(policy::ShouldDiscardCfrTimerRebaseDebt(true));
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
    EXPECT_EQ(policy::GetCfrCatchupTicksThisLoop(policy::kCfrShortfallCatchupThresholdTicks), 2u);
    EXPECT_EQ(policy::GetCfrCatchupTicksThisLoop(policy::kCfrShortfallCatchupThresholdTicks + 4), 2u);
    EXPECT_EQ(policy::GetCfrCatchupTicksThisLoop(policy::kCfrShortfallForceCatchupThresholdTicks - 1), 2u);
}

TEST(CapturePipelinePolicyTest, WgcCatchupTicksRecoverModerateShortfallWhenHealthy) {
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, false, 2, 1.0, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 118, 120, 0, false),
              1u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(
                  false, false, 4, 2.0, policy::kCfrShortfallForceCatchupThresholdTicks - 1, 120, 124, 130, 0, false),
              1u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, true, 4, 2.0, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 112, 124, 0, false),
              1u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, true, 4, 2.0, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 110, 112, 0, true),
              2u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, false, 1, 2.0, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 110, 112, 0, true),
              2u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, false, 4, 0.5, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 110, 112, 120, false),
              2u);
}

TEST(CapturePipelinePolicyTest, WgcAudioLeadPressureRestoresGentleCatchupUnderEncoderLoad) {
    EXPECT_FALSE(policy::ShouldPrioritizeWgcAudioLeadCatchup(39.9));
    EXPECT_TRUE(policy::ShouldPrioritizeWgcAudioLeadCatchup(policy::kWgcAudioLeadCatchupThresholdMs));

    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, true, 4, 2.0, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 118, 124, 0, false, policy::kWgcAudioLeadCatchupThresholdMs - 0.1),
              1u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, true, 4, 2.0, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 118, 124, 0, false, policy::kWgcAudioLeadCatchupThresholdMs),
              2u);
}

TEST(CapturePipelinePolicyTest, WgcAudioLeadPressureEscalatesAtForceThreshold) {
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(4), 3u);
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(2), 1u);
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(1), 0u);

    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, true, 4, 2.0, policy::kCfrShortfallForceCatchupThresholdTicks,
                                                 120, 118, 124, 0, false, policy::kWgcAudioLeadCatchupThresholdMs),
              4u);
}

TEST(CapturePipelinePolicyTest, WgcAudioLeadForceCatchupStaysGentleAfterEncoderRecovery) {
    EXPECT_TRUE(policy::IsWgcSourceRecoveredEnoughForSmoothAudioLeadCatchup(120, 116, 116, 40));
    EXPECT_FALSE(policy::IsWgcSourceRecoveredEnoughForSmoothAudioLeadCatchup(120, 115, 116, 40));
    EXPECT_FALSE(policy::IsWgcSourceRecoveredEnoughForSmoothAudioLeadCatchup(120, 116, 116, 200));

    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, false, 4, 2.0, policy::kCfrShortfallForceCatchupThresholdTicks,
                                                 120, 116, 116, 40, false, policy::kWgcAudioLeadCatchupThresholdMs),
              2u);
}

TEST(CapturePipelinePolicyTest, CfrCatchupTicksBurstAtForceThreshold) {
    // At and above force threshold: allow larger bursts, capped at 4
    EXPECT_EQ(policy::GetCfrCatchupTicksThisLoop(policy::kCfrShortfallForceCatchupThresholdTicks), 4u);
    EXPECT_EQ(policy::GetCfrCatchupTicksThisLoop(100), 4u);
    // Small force-threshold shortfall still capped to shortfall itself
    // (the function uses min(shortfall, 4))
}

TEST(CapturePipelinePolicyTest, WgcCatchupTicksStillBurstAtForceThreshold) {
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, false, 2, 1.0, policy::kCfrShortfallForceCatchupThresholdTicks,
                                                 120, 118, 120, 0, false),
              1u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, true, 4, 2.0, policy::kCfrShortfallForceCatchupThresholdTicks,
                                                 120, 112, 124, 0, false),
              1u);
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
