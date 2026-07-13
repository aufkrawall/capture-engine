#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <vector>

#include "../common/capture_pipeline_policy.h"
#include "../common/frame_timing_utils.h"

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

TEST(CapturePipelinePolicyTest, InjectLiveAgeTrimPreservesReserveAndExpandsUnderPressure) {
    EXPECT_EQ(policy::GetInjectLiveMaxFrameAgeQpc(false, false, false, 100), 0);
    EXPECT_EQ(policy::GetInjectLiveMaxFrameAgeQpc(true, false, false, 100),
              100 * static_cast<int64_t>(policy::kInjectLiveHealthyMaxFrameAgeTicks));
    EXPECT_EQ(policy::GetInjectLiveMaxFrameAgeQpc(true, true, false, 100),
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

    EXPECT_FALSE(policy::BuildCfrTimelineStartContract(100000, 100400, 500).valid);
    EXPECT_FALSE(policy::BuildCfrTimelineStartContract(100000, 99999, 0).valid);
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

TEST(CapturePipelinePolicyTest, WgcCfrSourceCaptureDefaultsToSteadyHeadroomCap) {
    EXPECT_EQ(policy::GetWgcCfrOvercaptureTargetFps(0), 0u);
    EXPECT_EQ(policy::GetWgcCfrOvercaptureTargetFps(60), 75u);
    EXPECT_EQ(policy::GetWgcCfrOvercaptureTargetFps(120), 150u);
    EXPECT_EQ(policy::GetWgcCfrOvercaptureTargetFps(143), 179u);
    EXPECT_EQ(policy::GetWgcCfrOvercaptureTargetFps(120, 0), 0u);
    EXPECT_EQ(policy::GetWgcCfrOvercaptureTargetFps(60, 1250), 75u);
    EXPECT_EQ(policy::GetWgcCfrOvercaptureTargetFps(120, 1250), 150u);
    EXPECT_EQ(policy::GetWgcCfrOvercaptureTargetFps(143, 1250), 179u);
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

TEST(CapturePipelinePolicyTest, WgcOvercaptureSwitchesToMaxRateDuringRecovery) {
    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;
    telemetry.recentDeliveredMin250Fps = 120;
    telemetry.recentDeliveredMin500Fps = 120;
    telemetry.recentInputMin250Fps = 122;
    telemetry.recentInputMin500Fps = 122;

    EXPECT_FALSE(policy::ShouldUseWgcMaxRateForRecovery(telemetry, 20, false, false));

    // When source delivers above the output target on average, per-tick fresh
    // misses and recovery-mode flags are DWM delivery burstiness, not starvation.
    // Max-rate is futile — DWM still controls the frame pipeline.
    EXPECT_FALSE(policy::ShouldUseWgcMaxRateForRecovery(telemetry, 20, true, false));
    EXPECT_FALSE(policy::ShouldUseWgcMaxRateForRecovery(telemetry, 20, false, true));
    EXPECT_FALSE(
        policy::ShouldUseWgcMaxRateForRecovery(telemetry, policy::kWgcLowSourceEmptyTickPermille, false, false));

    telemetry.recentInputMin250Fps = 119;
    EXPECT_TRUE(policy::ShouldUseWgcMaxRateForRecovery(telemetry, 20, false, false));
}

TEST(CapturePipelinePolicyTest, WgcDelayReservoirRecoveryRequiresHealthySource) {
    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;
    telemetry.recentDeliveredMin250Fps = 122;
    telemetry.recentDeliveredMin500Fps = 122;
    telemetry.recentInputMin250Fps = 122;
    telemetry.recentInputMin500Fps = 122;
    telemetry.emptyTickPermille = 20;

    EXPECT_FALSE(policy::ShouldUseWgcMaxRateForDelayReservoirRecovery(telemetry, false, false, false));
    EXPECT_TRUE(policy::ShouldUseWgcMaxRateForDelayReservoirRecovery(telemetry, true, false, false));
    EXPECT_FALSE(policy::ShouldUseWgcMaxRateForDelayReservoirRecovery(telemetry, true, true, false));
    EXPECT_FALSE(policy::ShouldUseWgcMaxRateForDelayReservoirRecovery(telemetry, true, false, true));

    telemetry.recentInputMin250Fps = 90;
    EXPECT_FALSE(policy::ShouldUseWgcMaxRateForDelayReservoirRecovery(telemetry, true, false, false));

    telemetry.recentInputMin250Fps = 122;
    telemetry.recentDeliveredMin250Fps = 90;
    telemetry.emptyTickPermille = policy::kWgcLowSourceExitEmptyTickPermille;
    EXPECT_FALSE(policy::ShouldUseWgcMaxRateForDelayReservoirRecovery(telemetry, true, false, false));
}

TEST(CapturePipelinePolicyTest, WgcCappedActiveDelayUnderfeedUsesMaxRateRecovery) {
    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;
    telemetry.recentDeliveredMin250Fps = 122;
    telemetry.recentInputMin250Fps = 122;
    telemetry.emptyTickPermille = 20;

    EXPECT_FALSE(policy::ShouldUseWgcMaxRateForCappedActiveDelayUnderfeed(
        telemetry, /*delayReservoirBelowLowWater=*/true, /*producerCapped=*/true));

    telemetry.recentInputMin250Fps = 115;
    EXPECT_TRUE(policy::ShouldUseWgcMaxRateForCappedActiveDelayUnderfeed(
        telemetry, /*delayReservoirBelowLowWater=*/true, /*producerCapped=*/true));
    EXPECT_FALSE(policy::ShouldUseWgcMaxRateForCappedActiveDelayUnderfeed(
        telemetry, /*delayReservoirBelowLowWater=*/false, /*producerCapped=*/true));
    EXPECT_FALSE(policy::ShouldUseWgcMaxRateForCappedActiveDelayUnderfeed(
        telemetry, /*delayReservoirBelowLowWater=*/true, /*producerCapped=*/false));

    telemetry.recentInputMin250Fps = 122;
    telemetry.recentDeliveredMin250Fps = 115;
    EXPECT_TRUE(policy::ShouldUseWgcMaxRateForCappedActiveDelayUnderfeed(
        telemetry, /*delayReservoirBelowLowWater=*/true, /*producerCapped=*/true));

    telemetry.recentDeliveredMin250Fps = 122;
    telemetry.emptyTickPermille = policy::kWgcLowSourceEmptyTickPermille;
    // Source delivers above target (122 >= 120) — empty-tick permille is delivery
    // burstiness, not underfeed. Keep the cap.
    EXPECT_FALSE(policy::ShouldUseWgcMaxRateForCappedActiveDelayUnderfeed(
        telemetry, /*delayReservoirBelowLowWater=*/true, /*producerCapped=*/true));
}

TEST(CapturePipelinePolicyTest, WgcStartupReserveWaitCanUseMaxRateRecovery) {
    EXPECT_TRUE(policy::ShouldUseWgcMaxRateForStartupReserveWait(
        /*reserveMissing=*/true, /*waitBudgetRemaining=*/true, /*producerCapped=*/true));
    EXPECT_FALSE(policy::ShouldUseWgcMaxRateForStartupReserveWait(
        /*reserveMissing=*/false, /*waitBudgetRemaining=*/true, /*producerCapped=*/true));
    EXPECT_FALSE(policy::ShouldUseWgcMaxRateForStartupReserveWait(
        /*reserveMissing=*/true, /*waitBudgetRemaining=*/false, /*producerCapped=*/true));
    EXPECT_FALSE(policy::ShouldUseWgcMaxRateForStartupReserveWait(
        /*reserveMissing=*/true, /*waitBudgetRemaining=*/true, /*producerCapped=*/false));
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
    // Source delivers above target — per-tick fresh misses are DWM delivery
    // burstiness, not starvation. Restore the cap.
    EXPECT_TRUE(policy::ShouldRestoreWgcOvercaptureCap(telemetry, 80, 2500));
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

TEST(CapturePipelinePolicyTest, WgcSelectionTargetAppliesConfiguredContentDelay) {
    // audio_capture_latency_ms maps to extraSelectionDelayQpc and IS the selection lag (it
    // replaces the dormant one-tick delay); the PTS schedule is untouched.
    EXPECT_EQ(policy::GetWgcSelectionTargetQpc(2000, 1900, 100, true, 400), 1600);
    // Disabled / non-live: the configured delay is not applied.
    EXPECT_EQ(policy::GetWgcSelectionTargetQpc(2000, 1900, 100, false, 400), 2000);
    // No content delay: only the legacy one-tick delay path remains.
    EXPECT_EQ(policy::GetWgcSelectionTargetQpc(2000, 1900, 100, true, 0), 1900);
    // Negative configured delay is treated as zero (legacy one-tick delay).
    EXPECT_EQ(policy::GetWgcSelectionTargetQpc(2000, 1900, 100, true, -50), 1900);
    // A delay larger than the target leaves the original target rather than going negative.
    EXPECT_EQ(policy::GetWgcSelectionTargetQpc(500, 400, 100, true, 100000), 500);
}

TEST(CapturePipelinePolicyTest, WgcSelectionDelayAppliesOnlyForConfiguredContentDelay) {
    // Without a content delay, live CFR keeps near-live selection (no intentional delay).
    EXPECT_FALSE(policy::ShouldApplyWgcSelectionDelay(true, 0, false, false, false));
    EXPECT_FALSE(policy::ShouldApplyWgcSelectionDelay(true, 0, false, true, false));
    // With a content delay configured, the delay applies continuously for live recording
    // (independent of the transient reserve), so the bounded delay buffer can build/hold.
    EXPECT_TRUE(policy::ShouldApplyWgcSelectionDelay(true, 0, false, false, true));
    EXPECT_TRUE(policy::ShouldApplyWgcSelectionDelay(true, 5, true, false, true));
    // Not live: never apply.
    EXPECT_FALSE(policy::ShouldApplyWgcSelectionDelay(false, 0, false, false, true));
}

TEST(CapturePipelinePolicyTest, LiveRecoverySuppressesDelayOnlyOffUniformCadencePath) {
    // Legacy reservoir-target path (uniform cadence off): live-recovery yields to near-live catch-up.
    EXPECT_TRUE(policy::ShouldLiveRecoverySuppressWgcSelectionDelay(/*liveRecovery=*/true,
                                                                    /*uniformCadenceActiveDelay=*/false));
    EXPECT_FALSE(policy::ShouldLiveRecoverySuppressWgcSelectionDelay(/*liveRecovery=*/false,
                                                                     /*uniformCadenceActiveDelay=*/false));
    // Uniform-cadence path: the content delay is maintained continuously, even during live-recovery,
    // so the realized delay cannot collapse/latch. This is the fix for the 0..248ms delay swing.
    EXPECT_FALSE(policy::ShouldLiveRecoverySuppressWgcSelectionDelay(/*liveRecovery=*/true,
                                                                     /*uniformCadenceActiveDelay=*/true));
    EXPECT_FALSE(policy::ShouldLiveRecoverySuppressWgcSelectionDelay(/*liveRecovery=*/false,
                                                                     /*uniformCadenceActiveDelay=*/true));
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

TEST(CapturePipelinePolicyTest, WgcEncoderPressureOverridesDeliveryLimitedLowSourceWhenInputIsHealthy) {
    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;
    telemetry.recentDeliveredFps = 90;
    telemetry.recentDeliveredMin250Fps = 90;
    telemetry.recentDeliveredMin500Fps = 90;
    telemetry.recentInputMin250Fps = 120;
    telemetry.recentInputMin500Fps = 124;
    telemetry.emptyTickPermille = 20;
    telemetry.bufferedWgcFrames = 24;

    EXPECT_EQ(policy::ClassifyWgcLiveRecoveryState(telemetry, policy::kWgcRecoveryEnterShortfallTicks, true),
              policy::WgcLiveRecoveryState::kEncoderLimited);
    EXPECT_TRUE(policy::IsWgcSourceHealthyEnoughForEncoderLimitedSmoothness(
        telemetry.outputFps, telemetry.recentInputMin250Fps, telemetry.recentInputMin500Fps,
        telemetry.emptyTickPermille, telemetry.bufferedWgcFrames));

    telemetry.recentInputMin250Fps = 90;
    telemetry.recentInputMin500Fps = 92;
    telemetry.emptyTickPermille = 1000;
    telemetry.bufferedWgcFrames = 0;
    EXPECT_EQ(policy::ClassifyWgcLiveRecoveryState(telemetry, policy::kWgcRecoveryEnterShortfallTicks, true),
              policy::WgcLiveRecoveryState::kSourceStarved);
}

TEST(CapturePipelinePolicyTest, WgcBufferedInputBelowTargetCanStillUseEncoderLimitedSmoothnessUnderPressure) {
    EXPECT_FALSE(policy::IsWgcTrueSourceStarvedForRecovery(120, 100, 104, 70, 30, true));
    EXPECT_TRUE(policy::IsWgcSourceHealthyEnoughForEncoderLimitedSmoothness(120, 100, 104, 70, 30));

    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;
    telemetry.recentDeliveredFps = 80;
    telemetry.recentDeliveredMin250Fps = 80;
    telemetry.recentDeliveredMin500Fps = 80;
    telemetry.recentInputMin250Fps = 100;
    telemetry.recentInputMin500Fps = 104;
    telemetry.emptyTickPermille = 70;
    telemetry.bufferedWgcFrames = 30;

    EXPECT_EQ(policy::ClassifyWgcLiveRecoveryState(telemetry, policy::kWgcRecoveryEnterShortfallTicks, true),
              policy::WgcLiveRecoveryState::kEncoderLimited);
    EXPECT_TRUE(policy::IsWgcTrueSourceStarvedForRecovery(120, 100, 104, 70, 2, true));
    EXPECT_TRUE(policy::IsWgcTrueSourceStarvedForRecovery(120, 100, 104, 400, 30, true));
}

TEST(CapturePipelinePolicyTest, WgcSelectionDelayStaysDisabledForZeroLatencyCfr) {
    EXPECT_FALSE(policy::ShouldApplyWgcSelectionDelay(true, 0, false, true));
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
    EXPECT_TRUE(policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(1000, 1040, 1020, 100, false, false, false));
    EXPECT_FALSE(policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(920, 1040, 1020, 100, false, false, false));
    EXPECT_FALSE(policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(900, 1040, 1020, 100, true, false, false));
    EXPECT_FALSE(policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(1000, 1040, 1020, 100, true, true, true));
}

TEST(CapturePipelinePolicyTest, WgcActiveDelayUniformCadenceModeRequiresDelayAndConfigOptIn) {
    // Only active when the selection delay is applied AND the config opts in.
    EXPECT_TRUE(policy::IsWgcActiveDelayUniformCadenceMode(true, true));
    EXPECT_FALSE(policy::IsWgcActiveDelayUniformCadenceMode(true, false));
    EXPECT_FALSE(policy::IsWgcActiveDelayUniformCadenceMode(false, true));
    EXPECT_FALSE(policy::IsWgcActiveDelayUniformCadenceMode(false, false));
}

TEST(CapturePipelinePolicyTest, WgcUniformCadenceSuppressesReserveDefenseOlderFramePerturbation) {
    // Exact scenario that perturbed cadence under the GPU-bound Strange Brigade run: a healthy
    // reserve (not under pressure) where the earlier frame is within bias of the closest-to-target
    // frame, so the legacy reserve defense would drag selection to the older frame.
    EXPECT_TRUE(
        policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(1000, 1040, 1020, 100, false, false, false, false));

    // With reserve defense enabled (uniformCadenceMode = false) the composed policy still prefers
    // the older reserve-building frame, matching the legacy behavior.
    EXPECT_TRUE(policy::ShouldPreferEarlierFreshWgcFrameForReserveDefense(1000, 1040, 1020, 100, false, false, false,
                                                                          false, /*uniformCadenceMode=*/false));

    // In uniform-cadence mode the perturbation is suppressed: the selector keeps the
    // closest-to-target frame and lets the realized delay float (the fix for the abnormal judder).
    EXPECT_FALSE(policy::ShouldPreferEarlierFreshWgcFrameForReserveDefense(1000, 1040, 1020, 100, false, false, false,
                                                                           false, /*uniformCadenceMode=*/true));

    // Uniform-cadence mode never re-introduces an older pick even under reserve pressure / low source.
    EXPECT_FALSE(policy::ShouldPreferEarlierFreshWgcFrameForReserveDefense(1000, 1040, 1020, 100, true, true, true,
                                                                           false, /*uniformCadenceMode=*/true));
}

TEST(CapturePipelinePolicyTest, WgcActiveDelayPaceFloorMatchesReservoirDelayFrames) {
    // 370/100 ticks -> ceil = 4 floor frames (the realized content delay depth).
    EXPECT_EQ(policy::GetWgcActiveDelayPaceFloorFrames(370, 100), 4u);
    // Never returns 0 when a delay is active (would let the buffer fully drain).
    EXPECT_EQ(policy::GetWgcActiveDelayPaceFloorFrames(1, 100), 1u);
}

TEST(CapturePipelinePolicyTest, WgcActiveDelayPaceAdvancesAtSourceRateAndHoldsAtFloor) {
    // A large max-depth makes the setpoint cap inert so these cases exercise the source-rate pacing.
    constexpr size_t kNoCap = 64;
    // Source ~= output (credit 1.0), buffer above the floor: advance one unique frame.
    auto steady = policy::DecideWgcActiveDelayPace(1.0, /*buffered=*/6, /*floor=*/4, kNoCap);
    EXPECT_TRUE(steady.advance);
    EXPECT_EQ(steady.dropBeforeAdvance, 0u);
    EXPECT_EQ(steady.capDrops, 0u);
    EXPECT_DOUBLE_EQ(steady.creditConsumed, 1.0);

    // Source below output (credit < 1): HOLD -> an evenly distributed source-limited repeat.
    auto under = policy::DecideWgcActiveDelayPace(0.9, /*buffered=*/6, /*floor=*/4, kNoCap);
    EXPECT_FALSE(under.advance);
    EXPECT_EQ(under.dropBeforeAdvance, 0u);
    EXPECT_DOUBLE_EQ(under.creditConsumed, 0.0);

    // At the floor: hold even with full credit so the buffer never drains below the delay depth
    // (this is what keeps the realized delay stable / A/V sync preserved).
    auto atFloor = policy::DecideWgcActiveDelayPace(1.0, /*buffered=*/4, /*floor=*/4, kNoCap);
    EXPECT_FALSE(atFloor.advance);
    EXPECT_EQ(atFloor.dropBeforeAdvance, 0u);

    // Source faster than output (credit >= 2) with headroom above floor+1: decimate one evenly,
    // then advance one. Keeps cadence smooth while holding the floor.
    auto over = policy::DecideWgcActiveDelayPace(2.5, /*buffered=*/8, /*floor=*/4, kNoCap);
    EXPECT_TRUE(over.advance);
    EXPECT_EQ(over.dropBeforeAdvance, 1u);
    EXPECT_EQ(over.capDrops, 0u);
    EXPECT_DOUBLE_EQ(over.creditConsumed, 2.0);

    // Overcapture but only floor+1 buffered: do not decimate into the floor, just advance one.
    auto overTight = policy::DecideWgcActiveDelayPace(2.5, /*buffered=*/5, /*floor=*/4, kNoCap);
    EXPECT_TRUE(overTight.advance);
    EXPECT_EQ(overTight.dropBeforeAdvance, 0u);
    EXPECT_DOUBLE_EQ(overTight.creditConsumed, 1.0);
}

TEST(CapturePipelinePolicyTest, WgcActiveDelayPaceMaxDepthBoundsRealizedDelay) {
    // Reservoir target = floor(4) + extra(1) = 5; cap = target + jitter band(1) = 6.
    EXPECT_EQ(policy::GetWgcActiveDelayPaceMaxDepthFrames(370, 100), 6u);
    // Never collapses below floor+1 even for a tiny delay.
    EXPECT_GE(policy::GetWgcActiveDelayPaceMaxDepthFrames(1, 100), 2u);

    // Regression: an inflated reservoir (a VRR source previously delivered above the output rate and
    // the pure source-rate matcher never drained it) must be trimmed back toward the cap EVEN when
    // the current credit is below 1 (source now at/below output). Before the setpoint cap this case
    // produced zero drops and the realized content delay stayed stuck ~20 frames deep.
    auto inflated = policy::DecideWgcActiveDelayPace(/*credit=*/0.9, /*buffered=*/20, /*floor=*/4, /*maxDepth=*/6);
    EXPECT_EQ(inflated.capDrops, 14u);           // 20 -> 6
    EXPECT_EQ(inflated.dropBeforeAdvance, 14u);  // all from the cap, none credit-driven
    EXPECT_FALSE(inflated.advance);              // credit 0.9 < 1 after the cap, so just trim+hold
    EXPECT_DOUBLE_EQ(inflated.creditConsumed, 0.0);

    // The cap never trims into the delay floor: at exactly the cap there is nothing to trim.
    auto atCap = policy::DecideWgcActiveDelayPace(/*credit=*/0.9, /*buffered=*/6, /*floor=*/4, /*maxDepth=*/6);
    EXPECT_EQ(atCap.capDrops, 0u);
    // A pathological maxDepth below floor+1 is clamped so the cap can never starve the reserve.
    auto clampLow = policy::DecideWgcActiveDelayPace(/*credit=*/1.0, /*buffered=*/5, /*floor=*/4, /*maxDepth=*/0);
    EXPECT_EQ(clampLow.capDrops, 0u);  // depthCap = max(0, floor+1) = 5, buffered 5 -> no trim
}

TEST(CapturePipelinePolicyTest, WgcActiveDelayPaceStaysBoundedUnderVrrSourceAboveOutput) {
    // Simulate a VRR / GPU-bound source delivering above the CFR output rate (1.2 unique frames per
    // output tick) for an extended period. Without the setpoint cap the buffer (and therefore the
    // realized content delay) grows without bound (~0.2 frame/tick -> ~120 frames over 600 ticks);
    // with the cap it stays pinned near the reservoir depth.
    const size_t floor = 4;
    const size_t cap = policy::GetWgcActiveDelayPaceMaxDepthFrames(370, 100);  // 6
    const double inPerTick = 1.2;
    size_t buffered = floor;
    double credit = 0.0;
    double arrivalFrac = 0.0;
    size_t maxObservedDepth = buffered;
    for (int tick = 0; tick < 600; ++tick) {
        arrivalFrac += inPerTick;
        const size_t arrived = static_cast<size_t>(arrivalFrac);
        arrivalFrac -= static_cast<double>(arrived);
        buffered += arrived;
        credit = std::min(credit + inPerTick, 5.0);
        const auto pace = policy::DecideWgcActiveDelayPace(credit, buffered, floor, cap);
        const size_t drops = std::min<size_t>(pace.dropBeforeAdvance, buffered);
        buffered -= drops;
        credit -= pace.creditConsumed;
        if (pace.advance && buffered > 0) {
            --buffered;
        }
        maxObservedDepth = std::max(maxObservedDepth, buffered);
    }
    // Bounded near the cap (one tick of arrivals of headroom), never the unbounded ~120 of the bug.
    EXPECT_LE(maxObservedDepth, cap + 2);
    EXPECT_LE(buffered, cap + 1);
}

TEST(CapturePipelinePolicyTest, WgcNearestPlayoutDropsAlreadyPastFramesForCloserSlot) {
    const int64_t target = 1000;
    const int64_t leadTol = policy::GetWgcActiveDelayResidualToleranceQpc(100);  // 60
    // A strictly-newer successor that is still at/before the slot (within lead tolerance) means the
    // older front is already-past history -> drop it.
    EXPECT_TRUE(policy::ShouldDropWgcFrontForNearerPlayout(/*front=*/900, /*next=*/980, target, leadTol));
    EXPECT_TRUE(policy::ShouldDropWgcFrontForNearerPlayout(/*front=*/900, /*next=*/target + leadTol, target, leadTol));
    // The successor is in the future beyond tolerance -> keep the front (it is the slot frame) and
    // hold the future frame as reserve.
    EXPECT_FALSE(
        policy::ShouldDropWgcFrontForNearerPlayout(/*front=*/980, /*next=*/target + leadTol + 1, target, leadTol));
    // Non-monotonic / duplicate successor is never advanced past.
    EXPECT_FALSE(policy::ShouldDropWgcFrontForNearerPlayout(/*front=*/900, /*next=*/900, target, leadTol));
    EXPECT_FALSE(policy::ShouldDropWgcFrontForNearerPlayout(/*front=*/900, /*next=*/880, target, leadTol));
}

TEST(CapturePipelinePolicyTest, WgcDuplicateSourceTimestampSkipRequiresPriorDelivery) {
    EXPECT_FALSE(policy::ShouldSkipDeliveredDuplicateWgcSourceTimestamp(
        /*duplicateSourceTimestamp=*/false, /*rawSourceFrameQpc=*/1000, /*lastDeliveredRawSourceQpc=*/1000,
        /*cfrCaptureActive=*/true));
    EXPECT_FALSE(policy::ShouldSkipDeliveredDuplicateWgcSourceTimestamp(
        /*duplicateSourceTimestamp=*/true, /*rawSourceFrameQpc=*/1000, /*lastDeliveredRawSourceQpc=*/0,
        /*cfrCaptureActive=*/true));
    EXPECT_FALSE(policy::ShouldSkipDeliveredDuplicateWgcSourceTimestamp(
        /*duplicateSourceTimestamp=*/true, /*rawSourceFrameQpc=*/1000, /*lastDeliveredRawSourceQpc=*/999,
        /*cfrCaptureActive=*/true));
    EXPECT_FALSE(policy::ShouldSkipDeliveredDuplicateWgcSourceTimestamp(
        /*duplicateSourceTimestamp=*/true, /*rawSourceFrameQpc=*/1000, /*lastDeliveredRawSourceQpc=*/1000,
        /*cfrCaptureActive=*/false));

    EXPECT_TRUE(policy::ShouldSkipDeliveredDuplicateWgcSourceTimestamp(
        /*duplicateSourceTimestamp=*/true, /*rawSourceFrameQpc=*/1000, /*lastDeliveredRawSourceQpc=*/1000,
        /*cfrCaptureActive=*/true));
}

TEST(CapturePipelinePolicyTest, WgcNearestPlayoutEmitHoldDecision) {
    const int64_t target = 1000;
    const int64_t leadTol = policy::GetWgcActiveDelayResidualToleranceQpc(100);  // 60
    // Frame at the slot (within lead tolerance) and strictly newer than last emit -> emit.
    EXPECT_TRUE(policy::DecideWgcNearestPlayout(/*front=*/990, target, leadTol, /*lastEmitted=*/950).emit);
    EXPECT_TRUE(policy::DecideWgcNearestPlayout(/*front=*/target + leadTol, target, leadTol, 950).emit);
    // Frame still in the future beyond tolerance -> hold (slot not aged in yet), keep as reserve.
    auto future = policy::DecideWgcNearestPlayout(/*front=*/target + leadTol + 1, target, leadTol, 950);
    EXPECT_FALSE(future.emit);
    EXPECT_TRUE(future.hold);
    // Non-monotonic front -> hold (never emit backwards).
    auto stale = policy::DecideWgcNearestPlayout(/*front=*/940, target, leadTol, /*lastEmitted=*/950);
    EXPECT_FALSE(stale.emit);
    EXPECT_TRUE(stale.hold);
    // A lone frame OLDER than the target but newer than last emit is still emitted (freshest content
    // during a delivery gap) -> a clean monotonic hold/freeze, never a backward jump.
    EXPECT_TRUE(policy::DecideWgcNearestPlayout(/*front=*/700, target, leadTol, /*lastEmitted=*/650).emit);
}

namespace {
// Minimal nearest-target playout driver mirroring the media_main integration: per output tick it
// stale-drops already-past frames, then emits the slot frame or holds. Returns realized-delay and
// cadence statistics so tests can assert smoothness + bounded delay without the encoder.
struct PlayoutStats {
    int emits = 0;
    int holds = 0;
    int staleDrops = 0;
    int longestHoldRun = 0;
    int64_t maxRealizedDelay = 0;
    int64_t minRealizedDelay = INT64_MAX;
    int dropDupSameTickViolations = 0;  // a tick must never both stale-drop AND hold (churn signature)
};
PlayoutStats RunNearestPlayout(int ticks, int64_t interval, int64_t contentDelay, int deliveryBatchTicks,
                               int64_t startupFill, bool liveRecoveryLatched = false, bool uniformCadence = true) {
    PlayoutStats s;
    std::deque<int64_t> buffer;  // frame source timestamps, oldest first
    const int64_t leadTol = policy::GetWgcActiveDelayResidualToleranceQpc(interval);
    int64_t lastEmitted = 0;
    int64_t lastDeliveredTs = -interval;  // source timestamp delivered up to (exclusive of next)
    int64_t lastBurstTick = 0;
    int holdRun = 0;
    // Pre-roll: deliver enough startup frames so the first slot is covered.
    for (int64_t i = 0; i < startupFill; ++i) {
        lastDeliveredTs += interval;
        buffer.push_back(lastDeliveredTs);
    }
    for (int tick = 0; tick < ticks; ++tick) {
        const int64_t now = (startupFill + tick) * interval;  // grid time advances one frame/tick
        // Bursty delivery: every deliveryBatchTicks, deliver every source frame produced since the
        // last burst (smooth source, batched delivery). The source presents one frame per interval.
        if (tick - lastBurstTick >= deliveryBatchTicks || tick == 0) {
            while (lastDeliveredTs + interval <= now) {
                lastDeliveredTs += interval;
                buffer.push_back(lastDeliveredTs);
            }
            lastBurstTick = tick;
        }
        // Compute the playout target through the SAME composition the encoder thread uses, so a
        // regression in the live-recovery delay decision (the realized-delay collapse) shows up here
        // as a collapsed realized delay, not only in the isolated helper test. With the delay applied
        // this equals now-contentDelay; if the delay is wrongly suppressed it jumps to ~now (live).
        const int64_t target = policy::GetWgcActiveDelaySelectionTargetQpc(
            /*scheduledSampleQpc=*/now, /*fallbackTargetQpc=*/now - contentDelay,
            /*targetIntervalTicks=*/interval, /*recordingOutputLive=*/true, /*applyLiveDelay=*/true,
            liveRecoveryLatched, uniformCadence, /*contentDelayQpc=*/contentDelay);
        bool staleDroppedThisTick = false;
        // monotonic obsolete-drop (mirrors media_main's pre-pace loop)
        while (!buffer.empty() && buffer.front() <= lastEmitted) {
            buffer.pop_front();
        }
        while (buffer.size() > 1 && policy::ShouldDropWgcFrontForNearerPlayout(buffer[0], buffer[1], target, leadTol)) {
            buffer.pop_front();
            ++s.staleDrops;
            staleDroppedThisTick = true;
        }
        bool held = true;
        if (!buffer.empty()) {
            auto d = policy::DecideWgcNearestPlayout(buffer.front(), target, leadTol, lastEmitted);
            if (d.emit) {
                const int64_t ts = buffer.front();
                buffer.pop_front();
                lastEmitted = ts;
                ++s.emits;
                held = false;
                const int64_t realized = now - ts;
                s.maxRealizedDelay = std::max(s.maxRealizedDelay, realized);
                s.minRealizedDelay = std::min(s.minRealizedDelay, realized);
            }
        }
        if (held) {
            ++s.holds;
            ++holdRun;
            s.longestHoldRun = std::max(s.longestHoldRun, holdRun);
            if (staleDroppedThisTick) {
                ++s.dropDupSameTickViolations;  // never drop fresh content AND repeat in one tick
            }
        } else {
            holdRun = 0;
        }
    }
    return s;
}

PlayoutStats RunNearestPlayoutResample(int ticks, int64_t outputInterval, int64_t sourceInterval, int64_t contentDelay,
                                       int deliveryBatchTicks, int64_t startupFill) {
    PlayoutStats s;
    std::deque<int64_t> buffer;
    const int64_t leadTol = policy::GetWgcActiveDelayResidualToleranceQpc(outputInterval);
    int64_t lastEmitted = 0;
    int64_t lastDeliveredTs = -sourceInterval;
    int64_t lastBurstTick = 0;
    int holdRun = 0;
    for (int64_t i = 0; i < startupFill; ++i) {
        lastDeliveredTs += sourceInterval;
        buffer.push_back(lastDeliveredTs);
    }
    for (int tick = 0; tick < ticks; ++tick) {
        const int64_t now = startupFill * sourceInterval + static_cast<int64_t>(tick) * outputInterval;
        if (tick - lastBurstTick >= deliveryBatchTicks || tick == 0) {
            while (lastDeliveredTs + sourceInterval <= now) {
                lastDeliveredTs += sourceInterval;
                buffer.push_back(lastDeliveredTs);
            }
            lastBurstTick = tick;
        }
        const int64_t target = now - contentDelay;
        bool staleDroppedThisTick = false;
        while (!buffer.empty() && buffer.front() <= lastEmitted) {
            buffer.pop_front();
        }
        while (buffer.size() > 1 && policy::ShouldDropWgcFrontForNearerPlayout(buffer[0], buffer[1], target, leadTol)) {
            buffer.pop_front();
            ++s.staleDrops;
            staleDroppedThisTick = true;
        }
        bool held = true;
        if (!buffer.empty()) {
            auto d = policy::DecideWgcNearestPlayout(buffer.front(), target, leadTol, lastEmitted);
            if (d.emit) {
                const int64_t ts = buffer.front();
                buffer.pop_front();
                lastEmitted = ts;
                ++s.emits;
                held = false;
                const int64_t realized = now - ts;
                s.maxRealizedDelay = std::max(s.maxRealizedDelay, realized);
                s.minRealizedDelay = std::min(s.minRealizedDelay, realized);
            }
        }
        if (held) {
            ++s.holds;
            ++holdRun;
            s.longestHoldRun = std::max(s.longestHoldRun, holdRun);
            if (staleDroppedThisTick) {
                ++s.dropDupSameTickViolations;
            }
        } else {
            holdRun = 0;
        }
    }
    return s;
}

// Encoder-overload grid drift: the CFR encoder grid runs slower than wall-clock (it cannot sustain the
// output rate) so the grid-anchored playout target sits a fixed `gridLag` behind the real-time frame
// timestamps, on top of the content delay. A bounded reservoir keeps only the newest `reservoirFrames`
// frames -- older fresh frames pile up and drop as stale. When gridLag+contentDelay exceeds the reservoir
// span the target falls below the ENTIRE reserve, so without the anti-freeze floor every tick holds and
// the video freezes while fresh frames keep arriving. The reservoir is pre-filled so there is no
// unrealistic negative-time warmup transient. Realized delay is measured against the grid/audio slot
// (gridNow - emittedTs): positive => video is BEHIND the co-timed audio (correct), negative => ahead.
struct GridDriftStats {
    int emits = 0;
    int holds = 0;
    int longestHoldRun = 0;
    int backwardEmits = 0;      // monotonicity violations (must be 0)
    int aheadOfAudioEmits = 0;  // emitted frame newer than the grid/audio slot (must be 0)
    int64_t maxRealizedDelay = 0;
    int64_t minRealizedDelay = INT64_MAX;
};
GridDriftStats RunGridDriftPlayout(int ticks, int64_t interval, int64_t contentDelay, int64_t gridLag,
                                   int reservoirFrames, bool applyAntiFreezeFloor) {
    GridDriftStats s;
    const int64_t leadTol = policy::GetWgcActiveDelayResidualToleranceQpc(interval);
    const int64_t timeBase = 100000;  // keep wall/grid clocks positive throughout
    std::deque<int64_t> buffer;       // source (wall-clock) timestamps, oldest first
    for (int i = reservoirFrames - 1; i >= 0; --i) {
        buffer.push_back(timeBase - static_cast<int64_t>(i) * interval);  // pre-fill a full reserve
    }
    int64_t lastEmitted = timeBase - static_cast<int64_t>(reservoirFrames) * interval;
    int holdRun = 0;
    for (int tick = 1; tick <= ticks; ++tick) {
        const int64_t wallNow = timeBase + static_cast<int64_t>(tick) * interval;  // real time
        const int64_t gridNow = wallNow - gridLag;                                 // grid lags wall-clock
        buffer.push_back(wallNow);                                                 // one fresh frame/tick
        while (static_cast<int>(buffer.size()) > reservoirFrames) {
            buffer.pop_front();  // bounded reservoir: piled-up fresh frames drop as stale
        }
        int64_t target = gridNow - contentDelay;
        if (applyAntiFreezeFloor && !buffer.empty()) {
            target = policy::ApplyWgcUniformPlayoutAntiFreezeFloor(target, buffer.front(), interval);
        }
        while (buffer.size() > 1 && policy::ShouldDropWgcFrontForNearerPlayout(buffer[0], buffer[1], target, leadTol)) {
            buffer.pop_front();
        }
        bool held = true;
        if (!buffer.empty()) {
            auto d = policy::DecideWgcNearestPlayout(buffer.front(), target, leadTol, lastEmitted);
            if (d.emit) {
                const int64_t ts = buffer.front();
                buffer.pop_front();
                if (ts <= lastEmitted) {
                    ++s.backwardEmits;
                }
                lastEmitted = ts;
                ++s.emits;
                held = false;
                const int64_t realized = gridNow - ts;  // >0 => video behind the audio/grid slot
                if (realized < 0) {
                    ++s.aheadOfAudioEmits;
                }
                s.maxRealizedDelay = std::max(s.maxRealizedDelay, realized);
                s.minRealizedDelay = std::min(s.minRealizedDelay, realized);
            }
        }
        if (held) {
            ++s.holds;
            ++holdRun;
            s.longestHoldRun = std::max(s.longestHoldRun, holdRun);
        } else {
            holdRun = 0;
        }
    }
    return s;
}
}  // namespace

TEST(CapturePipelinePolicyTest, WgcNearestPlayoutAbsorbsDeliveryJitterWithinBudget) {
    // Smooth source (1 frame/interval) delivered in late batches whose gap (3 ticks) is WITHIN the
    // 4-frame content-delay budget. The fixed-latency jitter buffer must reconstruct fully smooth
    // output: one emit per tick, zero holds, and the realized delay pinned at the content delay
    // (no rubber-band). The old oldest-first+bulk-trim model churned drops/dups here.
    auto s = RunNearestPlayout(/*ticks=*/600, /*interval=*/100, /*contentDelay=*/400,
                               /*deliveryBatchTicks=*/3, /*startupFill=*/5);
    EXPECT_EQ(s.holds, 0);
    EXPECT_EQ(s.emits, 600);
    EXPECT_EQ(s.dropDupSameTickViolations, 0);
    // Realized delay stays tight around the 400-tick target (no 0..manyX rubber-band).
    EXPECT_GE(s.minRealizedDelay, 400 - 100);
    EXPECT_LE(s.maxRealizedDelay, 400 + 100);
}

TEST(CapturePipelinePolicyTest, WgcNearestPlayoutEvenHoldsAndStableDelayUnderGapBeyondBudget) {
    // Delivery gap (8 ticks) EXCEEDS the 4-frame budget, so some holds are unavoidable. The fix's
    // guarantees under that condition: (1) the realized delay never rubber-bands far past the target
    // even when a stale backlog arrives (audio-passed frames are dropped, not replayed); (2) a tick
    // never both drops fresh content and repeats (no drop+dup churn); (3) holds are bounded by the
    // delivery gap, not amplified into longer clusters.
    auto s = RunNearestPlayout(/*ticks=*/800, /*interval=*/100, /*contentDelay=*/400,
                               /*deliveryBatchTicks=*/8, /*startupFill=*/5);
    EXPECT_EQ(s.dropDupSameTickViolations, 0);
    EXPECT_GT(s.emits, 0);
    // Bounded realized delay: even the worst stale emit stays within a few frames of the target
    // instead of the hundreds-of-ms rubber-band the oldest-first model produced.
    EXPECT_LE(s.maxRealizedDelay, 400 + 4 * 100);
    // Hold clusters cannot exceed the delivery gap (no extra amplification).
    EXPECT_LE(s.longestHoldRun, 8);
}

TEST(CapturePipelinePolicyTest, WgcUniformPlayoutAntiFreezeFloorRaisesOnlyWhenReserveIsTooNew) {
    const int64_t interval = 100;
    const int64_t tol = policy::GetWgcActiveDelayResidualToleranceQpc(interval);  // 90
    // Oldest reserve frame OLDER than the slot -> healthy, no-op.
    EXPECT_EQ(policy::ApplyWgcUniformPlayoutAntiFreezeFloor(/*target=*/1000, /*oldest=*/700, interval), 1000);
    // Oldest within the too-new lead window (<= 3 intervals ahead) -> still aged-in, no-op.
    EXPECT_EQ(policy::ApplyWgcUniformPlayoutAntiFreezeFloor(1000, 1300, interval), 1000);
    // Oldest BEYOND the lead window (whole reserve is too-new = the freeze condition) -> raise the slot
    // target so the oldest lands exactly on the emit boundary (oldest - tolerance), never further.
    EXPECT_EQ(policy::ApplyWgcUniformPlayoutAntiFreezeFloor(1000, 1600, interval), 1600 - tol);
    // The raised target makes DecideWgcNearestPlayout emit the previously-frozen oldest frame.
    const int64_t raised = policy::ApplyWgcUniformPlayoutAntiFreezeFloor(1000, 1600, interval);
    EXPECT_TRUE(policy::DecideWgcNearestPlayout(/*front=*/1600, raised, tol, /*lastEmitted=*/1500).emit);
    // Never lowers the target; guards invalid inputs.
    EXPECT_EQ(policy::ApplyWgcUniformPlayoutAntiFreezeFloor(2000, 1600, interval), 2000);
    EXPECT_EQ(policy::ApplyWgcUniformPlayoutAntiFreezeFloor(0, 1600, interval), 0);
    EXPECT_EQ(policy::ApplyWgcUniformPlayoutAntiFreezeFloor(1000, 0, interval), 1000);
    EXPECT_EQ(policy::ApplyWgcUniformPlayoutAntiFreezeFloor(1000, 1600, 0), 1000);
}

TEST(CapturePipelinePolicyTest, WgcUniformPlayoutAntiFreezeFloorRequiresSyncSafeOldestFrame) {
    const int64_t interval = 100;
    const int64_t contentDelay = 1000;
    const int64_t tol = policy::GetWgcActiveDelayResidualToleranceQpc(interval);

    EXPECT_TRUE(policy::IsWgcUniformPlayoutAntiFreezeFloorSyncSafe(contentDelay - tol, contentDelay, interval));
    EXPECT_TRUE(policy::IsWgcUniformPlayoutAntiFreezeFloorSyncSafe(contentDelay, contentDelay, interval));
    EXPECT_FALSE(policy::IsWgcUniformPlayoutAntiFreezeFloorSyncSafe(contentDelay - tol - 1, contentDelay, interval));
    EXPECT_FALSE(policy::IsWgcUniformPlayoutAntiFreezeFloorSyncSafe(
        /*oldestBufferedAgeQpc=*/0, contentDelay, interval));
    EXPECT_TRUE(policy::IsWgcUniformPlayoutAntiFreezeFloorSyncSafe(
        /*oldestBufferedAgeQpc=*/0, /*contentDelayQpc=*/0, interval));
}

TEST(CapturePipelinePolicyTest, WgcUniformPlayoutFreezesUnderGridDriftWithoutAntiFreezeFloor) {
    // Regression for the 20.9 s hard freeze (build 0.1.4402, 4K120 AV1 NVENC): once the encoder grid had
    // drifted behind wall-clock far enough that the grid-anchored target sat below the ENTIRE reserve,
    // every tick held while fresh frames kept arriving. Pre-fix (no floor) => a freeze spanning nearly
    // the whole run despite a continuously full buffer.
    const int reservoirFrames = 14;
    auto s = RunGridDriftPlayout(/*ticks=*/500, /*interval=*/100, /*contentDelay=*/800, /*gridLag=*/1000,
                                 reservoirFrames, /*applyAntiFreezeFloor=*/false);
    EXPECT_GE(s.longestHoldRun, 480);  // essentially frozen for the whole run
    EXPECT_LE(s.emits, 10);
}

TEST(CapturePipelinePolicyTest, WgcUniformPlayoutAntiFreezeFloorResumesPlayoutBehindAudio) {
    const int reservoirFrames = 14;
    const int64_t interval = 100;
    auto s = RunGridDriftPlayout(/*ticks=*/500, interval, /*contentDelay=*/800, /*gridLag=*/1000, reservoirFrames,
                                 /*applyAntiFreezeFloor=*/true);
    // Freeze released: playout resumes at the source rate (one emit per tick after warmup).
    EXPECT_GE(s.emits, 490);
    EXPECT_LE(s.longestHoldRun, 2);
    // Never backwards, and video content is NEVER newer than the co-timed audio/grid slot: the floor
    // emits the OLDEST (deepest) reserve frame, so video stays behind audio -> A/V offset preserved.
    EXPECT_EQ(s.backwardEmits, 0);
    EXPECT_EQ(s.aheadOfAudioEmits, 0);
    EXPECT_GE(s.minRealizedDelay, 0);
    // Realized delay stays bounded within the reservoir depth (no unbounded lag, no rubber-band).
    EXPECT_LE(s.maxRealizedDelay, reservoirFrames * interval);
}

TEST(CapturePipelinePolicyTest, WgcNearestPlayoutDropsSurplus144HzFor120FpsWithoutRepeats) {
    // Model a fixed-rate source above the CFR target: source interval 100, output interval 120
    // is the same ratio as 144 Hz input to 120 fps output. The playout layer should consume this
    // as planned surplus drops, not CFR repeats.
    auto s = RunNearestPlayoutResample(/*ticks=*/1200, /*outputInterval=*/120, /*sourceInterval=*/100,
                                       /*contentDelay=*/3000, /*deliveryBatchTicks=*/3, /*startupFill=*/35);
    EXPECT_EQ(s.holds, 0);
    EXPECT_EQ(s.emits, 1200);
    EXPECT_GT(s.staleDrops, 0);
    EXPECT_EQ(s.dropDupSameTickViolations, 0);
    EXPECT_GE(s.minRealizedDelay, 3000 - 120);
    EXPECT_LE(s.maxRealizedDelay, 3000 + 120);
}

TEST(CapturePipelinePolicyTest, WgcSmoothnessBufferBudgetCapsRetainedFrames) {
    const uint32_t desired = policy::GetWgcSmoothnessDesiredFrames(/*outputFps=*/120, /*maxSmoothnessMs=*/250);
    EXPECT_EQ(desired, 38u);

    // 4K FP16 is 8 bytes/pixel. The default 2GB budget covers the source WGC
    // frame-pool buffers plus CE copy-pool slots; extra smoothness slots are
    // reduced before sync-delay and safety slots.
    const auto budget = policy::ComputeWgcSmoothnessSurfaceBudget(
        /*outputFps=*/120, /*maxSmoothnessMs=*/250, /*width=*/3840, /*height=*/2160,
        /*bytesPerPixel=*/8, /*budgetMb=*/2048, policy::GetWgcEstimatedSyncDelayFramesForBudget(/*outputFps=*/120));
    EXPECT_EQ(budget.budgetSurfaceCount, 32u);
    EXPECT_EQ(budget.sourceFramePoolBuffers, 8u);
    EXPECT_EQ(budget.copyPoolSlots, 24u);
    EXPECT_EQ(budget.syncDelayFrames, 5u);
    EXPECT_EQ(budget.safetySlots, 4u);
    EXPECT_EQ(budget.inFlightEncodeSlots, 1u);
    EXPECT_EQ(budget.selectedFrameSlackSlots, 1u);
    EXPECT_EQ(budget.reservedFreeCopySlots, 6u);
    EXPECT_EQ(budget.retainedFrameCap, 18u);
    EXPECT_EQ(budget.retainedExtraFrames, 12u);
    EXPECT_LT(budget.retainedExtraFrames, desired);
    EXPECT_TRUE(budget.capLimited);
}

TEST(CapturePipelinePolicyTest, WgcSmoothnessSplitBudgetCompactsSdrFp16RetainedCopies) {
    // With the improved split-budget algorithm, the split that maximizes retained
    // frames is selected. The retained WGC reservoir stores source frames, so the
    // 250 ms target is sized for 125% source-rate headroom: 38 retained frames.
    const auto budget = policy::ComputeWgcSmoothnessSurfaceBudget(
        /*outputFps=*/120, /*maxSmoothnessMs=*/250, /*width=*/3840, /*height=*/2160,
        /*sourceBytesPerPixel=*/8, /*copyBytesPerPixel=*/4, /*budgetMb=*/2048,
        policy::GetWgcEstimatedSyncDelayFramesForBudget(/*outputFps=*/120));
    EXPECT_TRUE(budget.splitByteBudget);
    EXPECT_EQ(budget.sourceFramePoolBuffers, 7u);
    EXPECT_EQ(budget.copyPoolSlots, 50u);
    EXPECT_EQ(budget.retainedFrameCap, 44u);
    EXPECT_EQ(budget.retainedExtraFrames, 38u);
    EXPECT_FALSE(budget.capLimited);
    EXPECT_LE(budget.estimatedBytes, 2048ull * 1024ull * 1024ull);
    EXPECT_EQ(budget.sourceEstimatedBytes,
              policy::EstimateWgcSurfaceBytes(/*width=*/3840, /*height=*/2160, /*bytesPerPixel=*/8) * 7ull);
    EXPECT_EQ(budget.copyEstimatedBytes,
              policy::EstimateWgcSurfaceBytes(/*width=*/3840, /*height=*/2160, /*bytesPerPixel=*/4) * 50ull);
}

TEST(CapturePipelinePolicyTest, WgcSmoothnessDefaultBudgetCoversVrrSourceAboveOutput) {
    EXPECT_EQ(policy::GetWgcSmoothnessBudgetFps(/*outputFps=*/120), 150u);
    EXPECT_EQ(policy::GetWgcSmoothnessDesiredFrames(/*outputFps=*/120, /*maxSmoothnessMs=*/300), 45u);
    EXPECT_EQ(policy::GetWgcEstimatedSyncDelayFramesForBudget(/*outputFps=*/120), 5u);

    const auto budget = policy::ComputeWgcSmoothnessSurfaceBudget(
        /*outputFps=*/120, /*maxSmoothnessMs=*/300, /*width=*/3840, /*height=*/2160,
        /*sourceBytesPerPixel=*/8, /*copyBytesPerPixel=*/4, /*budgetMb=*/3000,
        policy::GetWgcEstimatedSyncDelayFramesForBudget(/*outputFps=*/120));
    EXPECT_TRUE(budget.splitByteBudget);
    // Staging-only source pool (compact retained copy converts+releases in the
    // callback): fps/15 sizing keeps 8 FP16 buffers at 120 fps instead of the
    // retention-era 12, saving ~253MB at 4K while the full 45-frame retained
    // reservoir stays intact.
    EXPECT_EQ(budget.sourceFramePoolBuffers, 8u);
    EXPECT_EQ(budget.copyPoolSlots, 64u);
    EXPECT_EQ(budget.retainedFrameCap, 58u);
    EXPECT_EQ(budget.retainedExtraFrames, 45u);
    EXPECT_FALSE(budget.capLimited);
    EXPECT_LE(budget.estimatedBytes, 3000ull * 1024ull * 1024ull);
}

TEST(CapturePipelinePolicyTest, WgcCompactSourceStagingPoolIsSizedForBurstsNotRetention) {
    // Non-compact (retention-relevant) pools keep the default depth.
    EXPECT_EQ(policy::GetWgcSmoothnessPreferredSourceFramePoolBuffers(120, /*compactCopySurfaces=*/false), 8u);
    EXPECT_EQ(policy::GetWgcSmoothnessPreferredSourceFramePoolBuffers(60, /*compactCopySurfaces=*/true), 8u);
    // Compact-active staging: fps/15 with the 8-buffer floor and 12 cap.
    EXPECT_EQ(policy::GetWgcSmoothnessPreferredSourceFramePoolBuffers(120, /*compactCopySurfaces=*/true), 8u);
    EXPECT_EQ(policy::GetWgcSmoothnessPreferredSourceFramePoolBuffers(144, /*compactCopySurfaces=*/true), 10u);
    EXPECT_EQ(policy::GetWgcSmoothnessPreferredSourceFramePoolBuffers(240, /*compactCopySurfaces=*/true), 12u);
}

TEST(CapturePipelinePolicyTest, WgcSmoothnessHdrFp16RemainsHomogeneousAndBudgetCapped) {
    const auto budget = policy::ComputeWgcSmoothnessSurfaceBudget(
        /*outputFps=*/120, /*maxSmoothnessMs=*/250, /*width=*/3840, /*height=*/2160,
        /*sourceBytesPerPixel=*/8, /*copyBytesPerPixel=*/8, /*budgetMb=*/2048,
        policy::GetWgcEstimatedSyncDelayFramesForBudget(/*outputFps=*/120));
    EXPECT_FALSE(budget.splitByteBudget);
    EXPECT_EQ(budget.sourceFramePoolBuffers, 8u);
    EXPECT_EQ(budget.copyPoolSlots, 24u);
    EXPECT_EQ(budget.retainedExtraFrames, 12u);
    EXPECT_TRUE(budget.capLimited);
    EXPECT_LE(budget.estimatedBytes, 2048ull * 1024ull * 1024ull);
}

TEST(CapturePipelinePolicyTest, WgcSmoothnessSplitBudgetPreservesLowBudgetSafety) {
    const auto budget = policy::ComputeWgcSmoothnessSurfaceBudget(
        /*outputFps=*/120, /*maxSmoothnessMs=*/250, /*width=*/3840, /*height=*/2160,
        /*sourceBytesPerPixel=*/8, /*copyBytesPerPixel=*/4, /*budgetMb=*/512,
        policy::GetWgcEstimatedSyncDelayFramesForBudget(/*outputFps=*/120));
    EXPECT_TRUE(budget.splitByteBudget);
    EXPECT_GE(budget.sourceFramePoolBuffers, policy::kWgcSmoothnessSourceFramePoolMinBuffers);
    EXPECT_GE(budget.copyPoolSlots, policy::kWgcSmoothnessBufferMinPoolFrames);
    EXPECT_TRUE(budget.capLimited);
    EXPECT_LE(budget.estimatedBytes, 512ull * 1024ull * 1024ull);
}

TEST(CapturePipelinePolicyTest, WgcSmoothnessBufferAllowsFullTargetWhenBudgetAllows) {
    const auto budget = policy::ComputeWgcSmoothnessSurfaceBudget(
        /*outputFps=*/120, /*maxSmoothnessMs=*/250, /*width=*/1920, /*height=*/1080,
        /*bytesPerPixel=*/4, /*budgetMb=*/2048, policy::GetWgcEstimatedSyncDelayFramesForBudget(/*outputFps=*/120));
    EXPECT_EQ(budget.retainedExtraFrames, 38u);
    EXPECT_EQ(budget.sourceFramePoolBuffers, 8u);
    EXPECT_EQ(budget.copyPoolSlots, 58u);
    EXPECT_EQ(budget.retainedFrameCap, 52u);
    EXPECT_FALSE(budget.capLimited);
}

TEST(CapturePipelinePolicyTest, WgcSmoothnessBufferInactiveDelayDoesNotReserveExtraSlots) {
    const auto budget = policy::ComputeWgcSmoothnessSurfaceBudget(
        /*outputFps=*/0, /*maxSmoothnessMs=*/0, /*width=*/3840, /*height=*/2160,
        /*bytesPerPixel=*/8, /*budgetMb=*/2048, /*syncDelayFrames=*/0);
    EXPECT_EQ(budget.desiredExtraFrames, 0u);
    EXPECT_EQ(budget.retainedExtraFrames, 0u);
    EXPECT_EQ(budget.sourceFramePoolBuffers, 8u);
    EXPECT_EQ(budget.copyPoolSlots, 8u);
    EXPECT_EQ(budget.retainedFrameCap, 2u);
    EXPECT_FALSE(budget.capLimited);
}

TEST(CapturePipelinePolicyTest, WgcSmoothnessBufferInactiveExtraStillReservesSyncDelay) {
    const auto budget = policy::ComputeWgcSmoothnessSurfaceBudget(
        /*outputFps=*/0, /*maxSmoothnessMs=*/0, /*width=*/3840, /*height=*/2160,
        /*bytesPerPixel=*/8, /*budgetMb=*/2048, /*syncDelayFrames=*/4);
    EXPECT_EQ(budget.desiredExtraFrames, 0u);
    EXPECT_EQ(budget.retainedExtraFrames, 0u);
    EXPECT_EQ(budget.sourceFramePoolBuffers, 8u);
    EXPECT_EQ(budget.copyPoolSlots, 11u);
    EXPECT_EQ(budget.retainedFrameCap, 5u);
    EXPECT_FALSE(budget.capLimited);
}

TEST(CapturePipelinePolicyTest, WgcIngressAdmissionDecimatesOnlyWhenReservoirIsHigh) {
    const auto decision = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/17, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/120, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/0.25, /*freeCopySlots=*/7, /*reservedFreeCopySlots=*/6);
    EXPECT_FALSE(decision.accept);
    EXPECT_TRUE(decision.decimated);
    EXPECT_FALSE(decision.softReservePressure);
    EXPECT_STREQ(decision.reason, "wgc_ingress_decimated_credit");
}

TEST(CapturePipelinePolicyTest, WgcIngressAdmissionAcceptsHighReservoirWhenCreditAllows) {
    const auto decision = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/17, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/120, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/1.0, /*freeCopySlots=*/7, /*reservedFreeCopySlots=*/6);
    EXPECT_TRUE(decision.accept);
    EXPECT_FALSE(decision.decimated);
    EXPECT_STREQ(decision.reason, "credit");
}

TEST(CapturePipelinePolicyTest, WgcIngressAdmissionLetsUniformPlayoutOwnAboveTargetSurplus) {
    auto creditPressure = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/17, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/120, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/0.25, /*freeCopySlots=*/7, /*reservedFreeCopySlots=*/6,
        /*uniformPlayoutOwnsSurplus=*/true);
    EXPECT_TRUE(creditPressure.accept);
    EXPECT_FALSE(creditPressure.decimated);
    EXPECT_FALSE(creditPressure.softReservePressure);
    EXPECT_STREQ(creditPressure.reason, "uniform_playout_credit");

    auto softReservePressure = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/18, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/120, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/2.0, /*freeCopySlots=*/6, /*reservedFreeCopySlots=*/6,
        /*uniformPlayoutOwnsSurplus=*/true);
    EXPECT_TRUE(softReservePressure.accept);
    EXPECT_FALSE(softReservePressure.decimated);
    EXPECT_TRUE(softReservePressure.softReservePressure);
    EXPECT_STREQ(softReservePressure.reason, "uniform_playout_soft_reserve");
}

TEST(CapturePipelinePolicyTest, WgcIngressAdmissionUniformPlayoutStillProtectsHardPoolAndBelowTargetSource) {
    auto hardPressure = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/18, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/120, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/2.0, /*freeCopySlots=*/0, /*reservedFreeCopySlots=*/6,
        /*uniformPlayoutOwnsSurplus=*/true);
    EXPECT_FALSE(hardPressure.accept);
    EXPECT_TRUE(hardPressure.decimated);
    EXPECT_TRUE(hardPressure.hardReservePressure);
    EXPECT_STREQ(hardPressure.reason, "wgc_ingress_decimated_hard_reserve");

    auto belowTarget = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/17, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/120, /*recentInputMin250Fps=*/118, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/0.25, /*freeCopySlots=*/7, /*reservedFreeCopySlots=*/6,
        /*uniformPlayoutOwnsSurplus=*/true);
    EXPECT_FALSE(belowTarget.accept);
    EXPECT_TRUE(belowTarget.decimated);
    EXPECT_STREQ(belowTarget.reason, "wgc_ingress_decimated_credit");
}

TEST(CapturePipelinePolicyTest, WgcIngressAdmissionHonorsReservedFreeSlotsBeforeCredit) {
    const auto decision = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/1, /*retainedFrameCap=*/2, /*lowWaterFrames=*/0, /*recovering=*/false,
        /*outputFps=*/60, /*recentInputMin250Fps=*/120, /*recentInputMin500Fps=*/120,
        /*admissionCreditFrames=*/2.0, /*freeCopySlots=*/6, /*reservedFreeCopySlots=*/6);
    EXPECT_FALSE(decision.accept);
    EXPECT_TRUE(decision.decimated);
    EXPECT_TRUE(decision.softReservePressure);
    EXPECT_STREQ(decision.reason, "wgc_ingress_decimated_soft_reserve");
}

TEST(CapturePipelinePolicyTest, WgcIngressAdmissionZeroLowWaterDoesNotBypassReservedSlots) {
    const auto decision = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/0, /*retainedFrameCap=*/2, /*lowWaterFrames=*/0, /*recovering=*/false,
        /*outputFps=*/120, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/2.0, /*freeCopySlots=*/6, /*reservedFreeCopySlots=*/6);
    EXPECT_FALSE(decision.accept);
    EXPECT_TRUE(decision.decimated);
    EXPECT_TRUE(decision.softReservePressure);
    EXPECT_STREQ(decision.reason, "wgc_ingress_decimated_soft_reserve");
}

TEST(CapturePipelinePolicyTest, WgcIngressAdmissionAcceptsLowWaterAndRecovery) {
    auto lowWater = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/5, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/120, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/0.0, /*freeCopySlots=*/7, /*reservedFreeCopySlots=*/6);
    EXPECT_TRUE(lowWater.accept);
    EXPECT_FALSE(lowWater.decimated);
    EXPECT_STREQ(lowWater.reason, "low_water");

    auto recovery = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/18, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/true,
        /*outputFps=*/120, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/0.0, /*freeCopySlots=*/7, /*reservedFreeCopySlots=*/6);
    EXPECT_TRUE(recovery.accept);
    EXPECT_FALSE(recovery.decimated);
    EXPECT_STREQ(recovery.reason, "recovery");
}

TEST(CapturePipelinePolicyTest, WgcPoolPressureTrimKeepsDelayTargetButProtectsFreeSlots) {
    EXPECT_EQ(policy::GetWgcPoolPressureRetainedTrimTarget(/*currentFreeCopySlots=*/0,
                                                           /*reservedFreeCopySlots=*/6,
                                                           /*delayReservoirTargetFrames=*/30,
                                                           /*retainedFrameCap=*/34),
              30u);
    EXPECT_EQ(policy::GetWgcPoolPressureRetainedTrimTarget(/*currentFreeCopySlots=*/6,
                                                           /*reservedFreeCopySlots=*/6,
                                                           /*delayReservoirTargetFrames=*/30,
                                                           /*retainedFrameCap=*/34),
              30u);
    EXPECT_EQ(policy::GetWgcPoolPressureRetainedTrimTarget(/*currentFreeCopySlots=*/7,
                                                           /*reservedFreeCopySlots=*/6,
                                                           /*delayReservoirTargetFrames=*/30,
                                                           /*retainedFrameCap=*/34),
              34u);
    EXPECT_EQ(policy::GetWgcPoolPressureRetainedTrimTarget(/*currentFreeCopySlots=*/0,
                                                           /*reservedFreeCopySlots=*/6,
                                                           /*delayReservoirTargetFrames=*/0,
                                                           /*retainedFrameCap=*/34),
              34u);
}

TEST(CapturePipelinePolicyTest, WgcIngressAdmissionSoftReserveDoesNotBeatLowWaterAndRecovery) {
    auto lowWater = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/5, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/120, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/0.0, /*freeCopySlots=*/6, /*reservedFreeCopySlots=*/6);
    EXPECT_TRUE(lowWater.accept);
    EXPECT_FALSE(lowWater.decimated);
    EXPECT_TRUE(lowWater.softReservePressure);
    EXPECT_STREQ(lowWater.reason, "low_water");

    auto recovery = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/18, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/true,
        /*outputFps=*/120, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/0.0, /*freeCopySlots=*/6, /*reservedFreeCopySlots=*/6);
    EXPECT_TRUE(recovery.accept);
    EXPECT_FALSE(recovery.decimated);
    EXPECT_TRUE(recovery.softReservePressure);
    EXPECT_STREQ(recovery.reason, "recovery");
}

TEST(CapturePipelinePolicyTest, WgcIngressAdmissionDoesNotDecimateBelowTargetSourceWithHeadroom) {
    const auto decision = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/18, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/120, /*recentInputMin250Fps=*/108, /*recentInputMin500Fps=*/109,
        /*admissionCreditFrames=*/0.0, /*freeCopySlots=*/7, /*reservedFreeCopySlots=*/6);
    EXPECT_TRUE(decision.accept);
    EXPECT_FALSE(decision.decimated);
    EXPECT_STREQ(decision.reason, "source_below_cfr_target");
}

TEST(CapturePipelinePolicyTest, WgcIngressAdmissionSoftReserveDoesNotBeatBelowTargetSource) {
    const auto decision = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/18, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/120, /*recentInputMin250Fps=*/108, /*recentInputMin500Fps=*/109,
        /*admissionCreditFrames=*/0.0, /*freeCopySlots=*/6, /*reservedFreeCopySlots=*/6);
    EXPECT_TRUE(decision.accept);
    EXPECT_FALSE(decision.decimated);
    EXPECT_TRUE(decision.softReservePressure);
    EXPECT_STREQ(decision.reason, "source_below_cfr_target");
}

TEST(CapturePipelinePolicyTest, WgcIngressAdmissionHardReserveIsFaultEvidenceForImportantFrames) {
    const auto decision = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/18, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/120, /*recentInputMin250Fps=*/108, /*recentInputMin500Fps=*/109,
        /*admissionCreditFrames=*/0.0, /*freeCopySlots=*/0, /*reservedFreeCopySlots=*/6);
    EXPECT_FALSE(decision.accept);
    EXPECT_TRUE(decision.decimated);
    EXPECT_TRUE(decision.hardReservePressure);
    EXPECT_TRUE(decision.softReservePressure);
    EXPECT_STREQ(decision.reason, "wgc_ingress_decimated_hard_reserve");
}

TEST(CapturePipelinePolicyTest, WgcIngressAdmissionHardReserveSkipsBeforeLowWaterOrRecovery) {
    auto lowWater = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/2, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/120, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/1.0, /*freeCopySlots=*/0, /*reservedFreeCopySlots=*/6);
    EXPECT_FALSE(lowWater.accept);
    EXPECT_TRUE(lowWater.decimated);
    EXPECT_TRUE(lowWater.hardReservePressure);
    EXPECT_STREQ(lowWater.reason, "wgc_ingress_decimated_hard_reserve");

    auto recovery = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/18, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/true,
        /*outputFps=*/120, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/1.0, /*freeCopySlots=*/0, /*reservedFreeCopySlots=*/6);
    EXPECT_FALSE(recovery.accept);
    EXPECT_TRUE(recovery.decimated);
    EXPECT_TRUE(recovery.hardReservePressure);
    EXPECT_STREQ(recovery.reason, "wgc_ingress_decimated_hard_reserve");
}

TEST(CapturePipelinePolicyTest, WgcSmoothnessBufferRequiresActiveSyncDelay) {
    EXPECT_TRUE(policy::ShouldUseWgcSmoothnessBuffer(/*enabled=*/true, /*useVfr=*/false,
                                                     /*avContentDelayActive=*/true, /*targetIntervalTicks=*/100));
    EXPECT_FALSE(policy::ShouldUseWgcSmoothnessBuffer(/*enabled=*/true, /*useVfr=*/false,
                                                      /*avContentDelayActive=*/false, /*targetIntervalTicks=*/100));
    EXPECT_FALSE(policy::ShouldUseWgcSmoothnessBuffer(/*enabled=*/false, /*useVfr=*/false,
                                                      /*avContentDelayActive=*/true, /*targetIntervalTicks=*/100));
    EXPECT_FALSE(policy::ShouldUseWgcSmoothnessBuffer(/*enabled=*/true, /*useVfr=*/true,
                                                      /*avContentDelayActive=*/true, /*targetIntervalTicks=*/100));
    EXPECT_FALSE(policy::ShouldUseWgcSmoothnessBuffer(/*enabled=*/true, /*useVfr=*/false,
                                                      /*avContentDelayActive=*/true, /*targetIntervalTicks=*/0));
}

TEST(CapturePipelinePolicyTest, WgcSmoothnessDelayDesiredArmsForFloorWithoutAudioLatency) {
    // Floor is the whole point: arm the smoothness machinery even when there is no audio-latency
    // content delay (video-only / low-confidence probe). With the floor configured, the existing
    // gates (fed WgcSmoothnessDelayDesired as their avContentDelayActive argument) must arm.
    EXPECT_TRUE(policy::WgcSmoothnessDelayDesired(/*avContentDelayActive=*/false,
                                                  /*smoothnessFloorConfigured=*/true));
    EXPECT_TRUE(policy::WgcSmoothnessDelayDesired(/*avContentDelayActive=*/true,
                                                  /*smoothnessFloorConfigured=*/false));
    // Escape hatch: floor off AND no audio latency -> not desired (exact current behavior).
    EXPECT_FALSE(policy::WgcSmoothnessDelayDesired(/*avContentDelayActive=*/false,
                                                   /*smoothnessFloorConfigured=*/false));

    const bool floorDesired = policy::WgcSmoothnessDelayDesired(false, true);
    EXPECT_TRUE(policy::ShouldUseWgcSmoothnessBuffer(/*enabled=*/true, /*useVfr=*/false,
                                                     /*avContentDelayActive=*/floorDesired,
                                                     /*targetIntervalTicks=*/100));
    EXPECT_TRUE(policy::ShouldAttemptWgcStartupSmoothnessBuffer(/*enabled=*/true, /*useVfr=*/false,
                                                                /*avContentDelayActive=*/floorDesired,
                                                                /*targetIntervalTicks=*/100,
                                                                /*retainedExtraFrames=*/1));
    // Floor off + no audio latency -> gates stay closed (regression escape hatch).
    const bool floorOff = policy::WgcSmoothnessDelayDesired(false, false);
    EXPECT_FALSE(policy::ShouldUseWgcSmoothnessBuffer(true, false, floorOff, 100));
}

TEST(CapturePipelinePolicyTest, WgcSmoothnessFloorDerivationIsClampedAndMonotonic) {
    constexpr int64_t kQpcFreq = 10000000;   // 10 MHz QPC
    constexpr int64_t kInterval120 = 83333;  // ~8.333 ms at 120 fps
    constexpr uint32_t kMaxMs = 300;
    constexpr uint32_t kReservoirFrames = 30;  // ~250 ms buildable reservoir

    // No measured jitter -> falls back to the structural minimum (kWgcSmoothnessFloorMinFrames).
    policy::WgcSmoothnessFloorJitter none{};
    const int64_t floorNone =
        policy::DeriveWgcSmoothnessFloorDelayQpc(none, kInterval120, kQpcFreq, kMaxMs, kReservoirFrames);
    EXPECT_EQ(floorNone, kInterval120 * policy::kWgcSmoothnessFloorMinFrames);

    // Moderate delivery burst (~50 ms gaps) -> floor absorbs the excess beyond one frame interval.
    policy::WgcSmoothnessFloorJitter moderate{};
    moderate.deliveryGapMaxUs = 50000;  // 50 ms
    const int64_t floorModerate =
        policy::DeriveWgcSmoothnessFloorDelayQpc(moderate, kInterval120, kQpcFreq, kMaxMs, kReservoirFrames);
    EXPECT_GT(floorModerate, floorNone);

    // Larger burst -> larger floor (monotonic) until clamped.
    policy::WgcSmoothnessFloorJitter heavy{};
    heavy.deliveryGapMaxUs = 170000;  // 170 ms
    const int64_t floorHeavy =
        policy::DeriveWgcSmoothnessFloorDelayQpc(heavy, kInterval120, kQpcFreq, kMaxMs, kReservoirFrames);
    EXPECT_GT(floorHeavy, floorModerate);

    // Extreme jitter must clamp to the smaller of maxMs and the buildable reservoir.
    policy::WgcSmoothnessFloorJitter extreme{};
    extreme.deliveryGapMaxUs = 5000000;  // 5 s (absurd outlier)
    const int64_t floorExtreme =
        policy::DeriveWgcSmoothnessFloorDelayQpc(extreme, kInterval120, kQpcFreq, kMaxMs, kReservoirFrames);
    const int64_t capQpc = policy::GetWgcSmoothnessFloorCapQpc(kInterval120, kQpcFreq, kMaxMs, kReservoirFrames);
    EXPECT_EQ(floorExtreme, capQpc);
    EXPECT_LE(capQpc, kInterval120 * kReservoirFrames);
    EXPECT_LE(capQpc, (kQpcFreq * kMaxMs) / 1000);

    // No reservoir capacity -> floor cannot be realized (0), regardless of measured jitter.
    EXPECT_EQ(policy::DeriveWgcSmoothnessFloorDelayQpc(heavy, kInterval120, kQpcFreq, kMaxMs, /*reservoir=*/0), 0);
    EXPECT_EQ(policy::GetWgcSmoothnessFloorCapQpc(kInterval120, kQpcFreq, kMaxMs, /*reservoir=*/0), 0);
}

TEST(CapturePipelinePolicyTest, WgcSmoothnessFloorIsSyncNeutralAcrossDelays) {
    // Invariant #1: the smoothness extra delay S (whether from the audio-latency reservoir or the
    // new floor) is sync-neutral by construction. The audio anchor delay tracks ONLY the true audio
    // latency L (GetWgcStartupAudioAnchorQpc(videoQpc, L)); S is realized purely as the startup video
    // frame being older with a correspondingly later live-start. For every PTS tick the selected
    // video content time must equal the audio real-sound time, INDEPENDENT of S. This is what keeps
    // a video-only floor (L=0, S>0) from drifting audio and from reintroducing ghost-image judder.
    const int64_t startupVideoQpc = 1000000;
    const int64_t interval = 83333;              // ~120 fps at 10 MHz QPC
    for (int64_t L : {0, 350000, 720000}) {      // 0 / 35 ms / 72 ms audio latency
        for (int64_t S : {0, 166666, 500000}) {  // 0 / 2 frames / ~60 ms smoothness extra
            if (L + S <= 0) {
                continue;  // no delay desired -> active-delay path not engaged (legacy near-live)
            }
            // The audio anchor must depend on L only, never on S.
            const int64_t audioAnchor = policy::GetWgcStartupAudioAnchorQpc(startupVideoQpc, L);
            EXPECT_EQ(audioAnchor, L > 0 ? startupVideoQpc + L : startupVideoQpc);

            // The startup frame is (L+S) old at live-start (symmetric application of the delay).
            const int64_t liveStart = startupVideoQpc + L + S;
            for (int64_t p = 0; p < 5; ++p) {
                const int64_t gridTick = liveStart + p * interval;
                const int64_t selectionTarget =
                    policy::GetWgcSelectionTargetQpc(gridTick, /*fallback=*/0, interval, /*recordingOutputLive=*/true,
                                                     /*extraSelectionDelayQpc=*/L + S);
                const int64_t audioRealSoundTime = audioAnchor + p * interval - L;
                EXPECT_EQ(selectionTarget, audioRealSoundTime) << "L=" << L << " S=" << S << " p=" << p;
            }
        }
    }
}

TEST(CapturePipelinePolicyTest, WgcSmoothnessBufferDoesNotGrowWithoutBudgetOrSourceFrames) {
    EXPECT_EQ(policy::GetWgcSmoothnessRetainedFrames(/*outputFps=*/120, /*maxSmoothnessMs=*/250,
                                                     /*width=*/3840, /*height=*/2160, /*bytesPerPixel=*/8,
                                                     /*budgetMb=*/0),
              0u);

    EXPECT_FALSE(policy::ShouldArmWgcSmoothnessBufferForSourceRate(
        /*outputFps=*/120, /*recentInputMin250Fps=*/110, /*recentInputMin500Fps=*/111));
    EXPECT_TRUE(policy::ShouldArmWgcSmoothnessBufferForSourceRate(
        /*outputFps=*/120, /*recentInputMin250Fps=*/118, /*recentInputMin500Fps=*/118));
    EXPECT_FALSE(policy::ShouldArmWgcSmoothnessBufferForSourceRate(
        /*outputFps=*/120, /*recentInputMin250Fps=*/0, /*recentInputMin500Fps=*/0));

    auto underTarget = RunNearestPlayout(/*ticks=*/240, /*interval=*/100, /*contentDelay=*/400,
                                         /*deliveryBatchTicks=*/12, /*startupFill=*/5);
    EXPECT_GT(underTarget.holds, 0);
    EXPECT_LE(underTarget.maxRealizedDelay, 400 + 12 * 100);
}

TEST(CapturePipelinePolicyTest, WgcActiveDelaySelectionTargetHoldsDelayThroughLiveRecoveryOnUniformPath) {
    // Regression for 20260626_050554: a perpetually-below-output VRR source latched WGC live-recovery
    // for the rest of the recording, and the selection target dropped the content delay during
    // live-recovery -> realized delay collapsed to ~0 and stayed there (video ran ~31.5 ms ahead of
    // the loopback audio it should align with). The flag (ShouldApplyWgcSelectionDelay) said "delay
    // applied" while the target silently went near-live. GetWgcActiveDelaySelectionTargetQpc must keep
    // the two in agreement.
    const int64_t scheduled = 2000, fallback = 1900, interval = 100, contentDelay = 400;
    // Uniform-cadence path: the content delay is HELD whether or not live-recovery is active.
    EXPECT_EQ(policy::GetWgcActiveDelaySelectionTargetQpc(scheduled, fallback, interval, /*live=*/true,
                                                          /*applyLiveDelay=*/true, /*liveRecovery=*/true,
                                                          /*uniformCadence=*/true, contentDelay),
              scheduled - contentDelay);  // 1600 -- would have been 2000 (collapsed) before the fix
    EXPECT_EQ(
        policy::GetWgcActiveDelaySelectionTargetQpc(scheduled, fallback, interval, true, true,
                                                    /*liveRecovery=*/false, /*uniformCadence=*/true, contentDelay),
        scheduled - contentDelay);
    // Legacy reservoir-target path: live-recovery still legitimately yields to near-live catch-up.
    EXPECT_EQ(
        policy::GetWgcActiveDelaySelectionTargetQpc(scheduled, fallback, interval, true, true,
                                                    /*liveRecovery=*/true, /*uniformCadence=*/false, contentDelay),
        scheduled);
    EXPECT_EQ(
        policy::GetWgcActiveDelaySelectionTargetQpc(scheduled, fallback, interval, true, true,
                                                    /*liveRecovery=*/false, /*uniformCadence=*/false, contentDelay),
        scheduled - contentDelay);
    // applyLiveDelay false, or not live, never applies the delay regardless of path.
    EXPECT_EQ(policy::GetWgcActiveDelaySelectionTargetQpc(scheduled, fallback, interval, true,
                                                          /*applyLiveDelay=*/false, true, true, contentDelay),
              scheduled);
    EXPECT_EQ(policy::GetWgcActiveDelaySelectionTargetQpc(scheduled, fallback, interval, /*live=*/false, true, true,
                                                          true, contentDelay),
              scheduled);
}

TEST(CapturePipelinePolicyTest, WgcNearestPlayoutPinsDelayThroughLatchedLiveRecovery) {
    // End-to-end: a below-output VRR source keeps live-recovery LATCHED for the whole run (the real
    // steady state). On the uniform-cadence path the realized content delay must stay pinned near the
    // target -- it must NOT collapse toward zero. Before the fix the target lost its delay while
    // live-recovery was active, so the playout fast-forwarded to live content and the realized delay
    // dropped to ~0 (minRealizedDelay near 0).
    auto fixed = RunNearestPlayout(/*ticks=*/600, /*interval=*/100, /*contentDelay=*/400,
                                   /*deliveryBatchTicks=*/2, /*startupFill=*/8,
                                   /*liveRecoveryLatched=*/true, /*uniformCadence=*/true);
    EXPECT_GE(fixed.minRealizedDelay, 400 - 2 * 100);  // pinned: never collapses to ~0
    EXPECT_LE(fixed.maxRealizedDelay, 400 + 2 * 100);

    // The legacy path intentionally yields to live-recovery: the delay collapses toward live. This
    // asserts the simulation actually discriminates the two paths (so the pin above is meaningful).
    auto legacy = RunNearestPlayout(/*ticks=*/600, /*interval=*/100, /*contentDelay=*/400,
                                    /*deliveryBatchTicks=*/2, /*startupFill=*/8,
                                    /*liveRecoveryLatched=*/true, /*uniformCadence=*/false);
    EXPECT_LT(legacy.minRealizedDelay, fixed.minRealizedDelay);
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
    EXPECT_TRUE(policy::IsWgcSourceHealthyEnoughToSuppressEncoderLimitedCatchup(120, 119, 0, false));
    EXPECT_TRUE(policy::IsWgcSourceHealthyEnoughToSuppressEncoderLimitedCatchup(120, 124, 100, false));
    EXPECT_TRUE(policy::IsWgcSourceHealthyEnoughToSuppressEncoderLimitedCatchup(120, 124, 0, true));
    EXPECT_FALSE(policy::IsWgcSourceHealthyEnoughToSuppressEncoderLimitedCatchup(120, 115, 0, false));
    EXPECT_FALSE(policy::IsWgcSourceHealthyEnoughToSuppressEncoderLimitedCatchup(120, 124, 400, false));
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

TEST(CapturePipelinePolicyTest, WgcLiveSelectionTargetClampsOnlyBeyondVisualDebtWindow) {
    EXPECT_EQ(policy::GetWgcLiveVisualDebtLimitQpc(100, 1000), 250);
    EXPECT_EQ(policy::GetWgcLiveVisualDebtLimitQpc(100, 0), 3200);
    EXPECT_EQ(policy::GetWgcLiveVisualDebtLimitTicks(100, 1000), 3u);
    EXPECT_EQ(policy::GetWgcLiveVisualDebtExcessTicks(3, 100, 1000), 0u);
    EXPECT_EQ(policy::GetWgcLiveVisualDebtExcessTicks(4, 100, 1000), 1u);
    EXPECT_EQ(policy::GetWgcLiveVisualDebtFloorQpc(1600, 100, 1000), 1350);

    EXPECT_EQ(policy::ClampWgcSelectionTargetToLiveQpc(1400, 1600, 100, 1000, false, false, 0, false), 1400);
    EXPECT_EQ(policy::ClampWgcSelectionTargetToLiveQpc(1000, 1600, 100, 1000, false, false, 3, false), 1000);
    EXPECT_EQ(policy::ClampWgcSelectionTargetToLiveQpc(1000, 1600, 100, 1000, false, false, 4, false), 1350);
    EXPECT_EQ(policy::ClampWgcSelectionTargetToLiveQpc(1000, 1600, 100, 1000, false, true, 20, true), 1350);
    EXPECT_EQ(policy::ClampWgcSelectionTargetToLiveQpc(1000, 1600, 100, 1000, false, true, 20, true,
                                                       policy::kCfrShortfallCatchupThresholdTicks, true),
              1550);
    EXPECT_EQ(policy::ClampWgcSelectionTargetToLiveQpc(1360, 1600, 100, 1000, true, true, 20, true), 1360);
    EXPECT_FALSE(
        policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(1000, 1040, 1020, 100, true, true, true, true));
}

TEST(CapturePipelinePolicyTest, WgcCfrRejectsFramesTooNewForSlot) {
    EXPECT_FALSE(policy::IsWgcFrameTooNewForCfrSlot(1299, 1000, 100));
    EXPECT_FALSE(policy::IsWgcFrameTooNewForCfrSlot(1300, 1000, 100));
    EXPECT_TRUE(policy::IsWgcFrameTooNewForCfrSlot(1301, 1000, 100));
    EXPECT_FALSE(policy::IsWgcFrameTooNewForCfrSlot(1250, 1000, 100, 3));
    EXPECT_FALSE(policy::IsWgcFrameTooNewForCfrSlot(0, 1000, 100));
    EXPECT_FALSE(policy::IsWgcFrameTooNewForCfrSlot(1200, 0, 100));
    EXPECT_FALSE(policy::IsWgcFrameTooNewForCfrSlot(1200, 1000, 0));
}

TEST(CapturePipelinePolicyTest, WgcActiveDelayUsesStrictResidualTolerance) {
    EXPECT_EQ(policy::GetWgcActiveDelayResidualToleranceQpc(100), 90);
    EXPECT_FALSE(policy::IsWgcFrameTooNewForActiveDelaySlot(1090, 1000, 100));
    EXPECT_TRUE(policy::IsWgcFrameTooNewForActiveDelaySlot(1091, 1000, 100));

    EXPECT_EQ(policy::GetWgcActiveDelayResidualHardLimitQpc(8333, 1000000), 10000);
    EXPECT_FALSE(policy::IsWgcFrameTooNewForActiveDelayHardLimit(110000, 100000, 8333, 1000000));
    EXPECT_TRUE(policy::IsWgcFrameTooNewForActiveDelayHardLimit(110001, 100000, 8333, 1000000));
    EXPECT_EQ(policy::GetWgcActiveDelaySoftLateTargetUs(4167, 1000000), 5000u);
    EXPECT_EQ(policy::GetWgcActiveDelaySoftLateTargetUs(8333, 1000000), 6249u);
    EXPECT_EQ(policy::GetWgcActiveDelaySoftLateTargetUs(16667, 1000000), 7000u);

    EXPECT_FALSE(policy::IsWgcFrameTooNewForCfrSlot(1250, 1000, 100));
    EXPECT_TRUE(policy::IsWgcFrameTooNewForActiveDelaySlot(1250, 1000, 100));
}

TEST(CapturePipelinePolicyTest, WgcActiveDelayClassifiesUnstableFpsWindows) {
    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;
    telemetry.recentDeliveredFps = 140;
    telemetry.recentDeliveredMin250Fps = 140;
    telemetry.recentDeliveredMin500Fps = 140;
    telemetry.recentInputMin250Fps = 140;
    telemetry.recentInputMin500Fps = 140;
    telemetry.bufferedWgcFrames = 5;

    EXPECT_EQ(policy::ClassifyWgcActiveDelayWindow(telemetry, false, false, false, false, false, true),
              policy::WgcActiveDelayWindowClass::kHealthy);

    telemetry.recentDeliveredMin250Fps = 110;
    EXPECT_EQ(policy::ClassifyWgcActiveDelayWindow(telemetry, false, false, false, false, false, true),
              policy::WgcActiveDelayWindowClass::kRecoverableUnderfill);

    telemetry.recentDeliveredMin250Fps = 140;
    telemetry.emptyTickPermille = policy::kWgcLowSourceEmptyTickPermille;
    EXPECT_EQ(policy::ClassifyWgcActiveDelayWindow(telemetry, false, false, false, false, false, true),
              policy::WgcActiveDelayWindowClass::kRecoverableUnderfill);

    telemetry.emptyTickPermille = 0;
    telemetry.recentInputMin250Fps = 90;
    EXPECT_EQ(policy::ClassifyWgcActiveDelayWindow(telemetry, false, false, false, false, false, true),
              policy::WgcActiveDelayWindowClass::kSourceLimited);

    telemetry.recentInputMin250Fps = 140;
    EXPECT_EQ(policy::ClassifyWgcActiveDelayWindow(telemetry, false, false, false, false, false, false),
              policy::WgcActiveDelayWindowClass::kHardSourceStall);
    EXPECT_TRUE(policy::IsWgcActiveDelaySourceLimitedClass(policy::WgcActiveDelayWindowClass::kHardSourceStall));

    telemetry.averageJitterUs = policy::GetWgcFrameIntervalUs(telemetry.outputFps) + 500;
    EXPECT_EQ(policy::ClassifyWgcActiveDelayWindow(telemetry, false, false, false, false, false, true),
              policy::WgcActiveDelayWindowClass::kRecoverableUnderfill);

    telemetry.emptyTickPermille = policy::kWgcLowSourceExitEmptyTickPermille;
    telemetry.averageJitterUs = (policy::GetWgcFrameIntervalUs(telemetry.outputFps) * 2) + 500;
    EXPECT_EQ(policy::ClassifyWgcActiveDelayWindow(telemetry, false, false, false, false, false, true),
              policy::WgcActiveDelayWindowClass::kSourceLimited);

    telemetry.emptyTickPermille = 0;
    telemetry.averageJitterUs = 0;
    telemetry.recentDeliveredMin250Fps = 140;
    telemetry.recentInputMin250Fps = 140;
    telemetry.recentInputMin500Fps = 140;
    EXPECT_EQ(policy::ClassifyWgcActiveDelayWindow(telemetry, false, false, false, false, true, true),
              policy::WgcActiveDelayWindowClass::kPostStallRecovery);
    EXPECT_FALSE(policy::IsWgcActiveDelaySourceLimitedClass(policy::WgcActiveDelayWindowClass::kPostStallRecovery));
    EXPECT_EQ(policy::ClassifyWgcActiveDelayWindow(telemetry, false, false, false, false, true, false),
              policy::WgcActiveDelayWindowClass::kHardSourceStall);

    telemetry.recentInputMin250Fps = 90;
    EXPECT_EQ(policy::ClassifyWgcActiveDelayWindow(telemetry, false, false, false, false, true, true),
              policy::WgcActiveDelayWindowClass::kPostStallRecovery);
}

TEST(CapturePipelinePolicyTest, WgcActiveDelayRelaxedCandidateMustBeatRepeatWithinHardLimit) {
    EXPECT_FALSE(policy::IsWgcActiveDelayRelaxedCandidateUseful(1090, 900, 1000, 100, 1000000));
    EXPECT_TRUE(policy::IsWgcActiveDelayRelaxedCandidateUseful(1095, 900, 1000, 100, 1000000));
    EXPECT_FALSE(policy::IsWgcActiveDelayRelaxedCandidateUseful(1095, 1040, 1000, 100, 1000000));
    EXPECT_FALSE(policy::IsWgcActiveDelayRelaxedCandidateUseful(110001, 80000, 100000, 8333, 1000000));
}

TEST(CapturePipelinePolicyTest, WgcActiveDelayScoresRelaxedCandidateReasons) {
    auto score = policy::ScoreWgcActiveDelayRelaxedCandidate(1095, 900, 1000, 100, 1000000);
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kAcceptBetterTarget);
    EXPECT_TRUE(score.Accepted());
    EXPECT_STREQ(policy::WgcActiveDelayRelaxedDecisionToString(score.decision), "better_target");

    score = policy::ScoreWgcActiveDelayRelaxedCandidate(1100, 1090, 1000, 100, 1000000, 1, 4000, 9000, 9000);
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kAcceptRepeatCluster);
    EXPECT_TRUE(score.Accepted());

    score = policy::ScoreWgcActiveDelayRelaxedCandidate(1100, 1090, 1000, 100, 1000000, 0);
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kRejectRepeatCost);
    EXPECT_FALSE(score.Accepted());

    score = policy::ScoreWgcActiveDelayRelaxedCandidate(1100, 1090, 1000, 100, 1000000, 1, 6000, 9000, 9000);
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom);
    EXPECT_FALSE(score.Accepted());

    score = policy::ScoreWgcActiveDelayRelaxedCandidate(110001, 109000, 100000, 8333, 1000000, 4);
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kRejectSyncRisk);
    EXPECT_FALSE(score.Accepted());
}

TEST(CapturePipelinePolicyTest, WgcActiveDelaySoftLateTargetProtectsHealthyWindows) {
    auto score = policy::ScoreWgcActiveDelayRelaxedCandidate(109000, 80000, 100000, 8333, 1000000, 1, 0, 0, 7000,
                                                             policy::WgcActiveDelayWindowClass::kHealthy,
                                                             policy::GetWgcActiveDelaySoftLateTargetUs(8333, 1000000));
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom);
    EXPECT_FALSE(score.Accepted());

    score = policy::ScoreWgcActiveDelayRelaxedCandidate(108000, 80000, 100000, 8333, 1000000, 1, 0, 0, 7000,
                                                        policy::WgcActiveDelayWindowClass::kSourceLimited,
                                                        policy::GetWgcActiveDelaySoftLateTargetUs(8333, 1000000));
    EXPECT_TRUE(score.Accepted());

    score = policy::ScoreWgcActiveDelayRelaxedCandidate(109000, 80000, 100000, 8333, 1000000, 1, 0, 0, 0,
                                                        policy::WgcActiveDelayWindowClass::kRecoverableUnderfill,
                                                        policy::GetWgcActiveDelaySoftLateTargetUs(8333, 1000000));
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom);
    EXPECT_FALSE(score.Accepted());

    score = policy::ScoreWgcActiveDelayRelaxedCandidate(109000, 80000, 100000, 8333, 1000000, 1, 0, 0, 0,
                                                        policy::WgcActiveDelayWindowClass::kPostStallRecovery,
                                                        policy::GetWgcActiveDelaySoftLateTargetUs(8333, 1000000));
    EXPECT_EQ(score.decision, policy::WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom);
    EXPECT_FALSE(score.Accepted());

    score = policy::ScoreWgcActiveDelayRelaxedCandidate(109000, 80000, 100000, 8333, 1000000, 1, 0, 0, 7000,
                                                        policy::WgcActiveDelayWindowClass::kSourceLimited,
                                                        policy::GetWgcActiveDelaySoftLateTargetUs(8333, 1000000));
    EXPECT_TRUE(score.Accepted());
}

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
TEST(CapturePipelinePolicyTest, WgcUniformPlayoutQuantizedSurplusSourceHoldsOnlyWithRawTimestamps) {
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

    // Before the fix: constant visible stutter manufactured from a healthy surplus source.
    EXPECT_GT(rawHolds, 10);
    // After the fix: pure surplus decimation, zero repeats.
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

TEST(CapturePipelinePolicyTest, WgcFreshCatchupRejectsFramesOutsideLiveVisualDebtWindow) {
    EXPECT_FALSE(policy::ShouldUseFreshWgcCatchupFrame(1360, 1600, 100, 1000, 20));
    EXPECT_TRUE(policy::ShouldUseFreshWgcCatchupFrame(1000, 1600, 100, 1000, 0));
    EXPECT_FALSE(policy::ShouldUseFreshWgcCatchupFrame(1349, 1600, 100, 1000, 20));
    EXPECT_TRUE(policy::IsWgcFrameWithinLiveVisualDebtWindow(1350, 1600, 100, 1000));
    EXPECT_FALSE(policy::IsWgcFrameWithinLiveVisualDebtWindow(1349, 1600, 100, 1000));
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

TEST(CapturePipelinePolicyTest, InjectCfrCatchupAvoidsDuplicateBurstsBelowForceThreshold) {
    EXPECT_EQ(policy::GetInjectCfrCatchupTicksThisLoop(policy::kCfrShortfallCatchupThresholdTicks), 1u);
    EXPECT_EQ(policy::GetInjectCfrCatchupTicksThisLoop(policy::kCfrShortfallForceCatchupThresholdTicks - 1), 1u);
    EXPECT_EQ(policy::GetInjectCfrCatchupTicksThisLoop(policy::kCfrShortfallForceCatchupThresholdTicks), 2u);
    EXPECT_EQ(policy::GetInjectCfrCatchupTicksThisLoop(100), 2u);
    EXPECT_EQ(policy::GetInjectCfrCatchupTicksThisLoop(100, true), 1u);
}

TEST(CapturePipelinePolicyTest, InjectFreshCatchupRequiresHealthyEncoderAndQueuedCredit) {
    EXPECT_TRUE(policy::ShouldUseFreshInjectCatchup(false, false, false, 4, 2, 1.0,
                                                    policy::kCfrShortfallForceCatchupThresholdTicks));
    EXPECT_FALSE(policy::ShouldUseFreshInjectCatchup(true, false, false, 4, 2, 1.0,
                                                     policy::kCfrShortfallForceCatchupThresholdTicks));
    EXPECT_FALSE(policy::ShouldUseFreshInjectCatchup(false, true, false, 4, 2, 1.0,
                                                     policy::kCfrShortfallForceCatchupThresholdTicks));
    EXPECT_FALSE(policy::ShouldUseFreshInjectCatchup(false, false, true, 4, 2, 1.0,
                                                     policy::kCfrShortfallForceCatchupThresholdTicks));
    EXPECT_FALSE(policy::ShouldUseFreshInjectCatchup(false, false, false, 2, 2, 1.0,
                                                     policy::kCfrShortfallForceCatchupThresholdTicks));
    EXPECT_FALSE(policy::ShouldUseFreshInjectCatchup(false, false, false, 4, 2, 0.99,
                                                     policy::kCfrShortfallForceCatchupThresholdTicks));
    EXPECT_FALSE(policy::ShouldUseFreshInjectCatchup(false, false, false, 4, 2, 1.0,
                                                     policy::kCfrShortfallForceCatchupThresholdTicks - 1));
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
              1u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, false, 1, 2.0, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 110, 112, 0, true),
              1u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, false, 4, 0.5, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 110, 112, 120, false),
              1u);
}

TEST(CapturePipelinePolicyTest, WgcAudioLeadPressureRestoresGentleCatchupUnderEncoderLoad) {
    EXPECT_FALSE(policy::ShouldPrioritizeWgcAudioLeadCatchup(39.9));
    EXPECT_TRUE(policy::ShouldPrioritizeWgcAudioLeadCatchup(policy::kWgcAudioLeadCatchupThresholdMs));

    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, true, 4, 2.0, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 118, 124, 0, false, policy::kWgcAudioLeadCatchupThresholdMs - 0.1),
              1u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, true, 4, 2.0, policy::kCfrShortfallCatchupThresholdTicks, 120,
                                                 118, 124, 0, false, policy::kWgcAudioLeadCatchupThresholdMs),
              1u);
}

TEST(CapturePipelinePolicyTest, WgcAudioLeadPressureEscalatesAtForceThreshold) {
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(4), 0u);
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(2), 0u);
    EXPECT_EQ(policy::GetWgcFreshCatchupBudgetThisLoop(1), 0u);

    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, true, 4, 2.0, policy::kCfrShortfallForceCatchupThresholdTicks,
                                                 120, 118, 124, 0, false, policy::kWgcAudioLeadCatchupThresholdMs),
              1u);
}

TEST(CapturePipelinePolicyTest, WgcAudioLeadForceCatchupStaysGentleAfterEncoderRecovery) {
    EXPECT_TRUE(policy::IsWgcSourceRecoveredEnoughForSmoothAudioLeadCatchup(120, 116, 116, 40));
    EXPECT_FALSE(policy::IsWgcSourceRecoveredEnoughForSmoothAudioLeadCatchup(120, 115, 116, 40));
    EXPECT_FALSE(policy::IsWgcSourceRecoveredEnoughForSmoothAudioLeadCatchup(120, 116, 116, 200));

    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(true, false, 4, 2.0, policy::kCfrShortfallForceCatchupThresholdTicks,
                                                 120, 116, 116, 40, false, policy::kWgcAudioLeadCatchupThresholdMs),
              1u);
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
              1u);
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
