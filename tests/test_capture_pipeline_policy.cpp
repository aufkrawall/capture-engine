#include <gtest/gtest.h>
#include "../common/capture_pipeline_policy.h"

namespace policy = ce::capture_policy;

TEST(CapturePipelinePolicyTest, WarmupCommitRequiresPoppedFrame) {
    EXPECT_FALSE(policy::ShouldCommitRecordingWarmup(false, false, false, true, 4, 2,
                                                     policy::kRecordingWarmupMaxMs));
}

TEST(CapturePipelinePolicyTest, WarmupCommitHonorsMinAndMaxThresholds) {
    EXPECT_FALSE(policy::ShouldCommitRecordingWarmup(false, false, true, false, 8, 2,
                                                     policy::kRecordingWarmupMinMs - 1));
    EXPECT_TRUE(policy::ShouldCommitRecordingWarmup(false, false, true, false, 0, 4,
                                                    policy::kRecordingWarmupMaxMs));
}

TEST(CapturePipelinePolicyTest, WarmupCommitUsesVfrAndBufferedSourceRules) {
    EXPECT_TRUE(policy::ShouldCommitRecordingWarmup(false, true, true, false, 0, 3,
                                                    policy::kRecordingWarmupMinMs));
    EXPECT_FALSE(policy::ShouldCommitRecordingWarmup(true, false, true, false, 0, 0,
                                                     policy::kRecordingWarmupMinMs));
    EXPECT_TRUE(policy::ShouldCommitRecordingWarmup(true, false, true, true, 0, 0,
                                                    policy::kRecordingWarmupMinMs));
    EXPECT_FALSE(policy::ShouldCommitRecordingWarmup(false, false, true, false, 1, 2,
                                                     policy::kRecordingWarmupMinMs));
    EXPECT_FALSE(policy::ShouldCommitRecordingWarmup(false, false, true, false, 2, 2,
                                                      policy::kRecordingWarmupMinMs));
    EXPECT_TRUE(policy::ShouldCommitRecordingWarmup(false, false, true, false, 3, 2,
                                                    policy::kRecordingWarmupMinMs));
}

TEST(CapturePipelinePolicyTest, InjectReserveFramesScaleWithFenceLeadTime) {
    EXPECT_EQ(policy::GetInjectReserveFrames(false, 0.0, 8.333), 1u);
    EXPECT_EQ(policy::GetInjectReserveFrames(false, 5.0, 8.0), 2u);
    EXPECT_EQ(policy::GetInjectReserveFrames(false, 11.0, 8.0), 3u);
    EXPECT_EQ(policy::GetInjectReserveFrames(false, 19.0, 8.0), 4u);
    EXPECT_EQ(policy::GetInjectReserveFrames(true, 19.0, 8.0), 0u);
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
    EXPECT_FALSE(policy::ShouldTriggerAutoWgcFallback(false, true, true, true,
                                                      policy::kAutoWgcFallbackDelayNoPidMs, 0));
    EXPECT_TRUE(policy::ShouldTriggerAutoWgcFallback(false, true, true, true,
                                                     policy::kAutoWgcFallbackDelayNoPidMs + 1, 0));
    EXPECT_FALSE(policy::ShouldTriggerAutoWgcFallback(false, true, true, true,
                                                      policy::kAutoWgcFallbackDelayWithPidMs, 7));
    EXPECT_TRUE(policy::ShouldTriggerAutoWgcFallback(false, true, true, true,
                                                     policy::kAutoWgcFallbackDelayWithPidMs + 1, 7));
}

TEST(CapturePipelinePolicyTest, WgcHeadroomEntryRequiresSustainedStrongSource) {
    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;
    telemetry.recentDeliveredFps = 132;
    telemetry.recentDeliveredMin250Fps = 131;
    telemetry.recentDeliveredMin500Fps = 130;
    telemetry.recentInputMin250Fps = 127;
    telemetry.recentInputMin500Fps = 126;
    telemetry.averageJitterUs = 700;
    telemetry.emptyTickPermille = 20;
    telemetry.bufferedWgcFrames = 1;
    telemetry.encoderQueueDepth = 0;
    telemetry.duplicateRatio = 0.005;

    EXPECT_TRUE(policy::ShouldEnterWgcHeadroomMode(telemetry));

    telemetry.recentDeliveredMin250Fps = 121;
    EXPECT_FALSE(policy::ShouldEnterWgcHeadroomMode(telemetry));

    telemetry.recentDeliveredMin250Fps = 131;
    telemetry.recentInputMin250Fps = 120;
    EXPECT_FALSE(policy::ShouldEnterWgcHeadroomMode(telemetry));
}

TEST(CapturePipelinePolicyTest, WgcHeadroomExitIsImmediateOnMarginalConditions) {
    policy::WgcAdaptiveTelemetry telemetry{};
    telemetry.outputFps = 120;
    telemetry.recentDeliveredFps = 128;
    telemetry.recentDeliveredMin250Fps = 127;
    telemetry.recentDeliveredMin500Fps = 127;
    telemetry.recentInputMin250Fps = 127;
    telemetry.recentInputMin500Fps = 127;
    telemetry.averageJitterUs = 700;
    telemetry.emptyTickPermille = 40;
    telemetry.bufferedWgcFrames = 1;
    telemetry.encoderQueueDepth = 0;
    telemetry.duplicateRatio = 0.005;

    EXPECT_FALSE(policy::ShouldExitWgcHeadroomMode(telemetry));

    telemetry.recentDeliveredMin250Fps = 122;
    EXPECT_TRUE(policy::ShouldExitWgcHeadroomMode(telemetry));

    telemetry.recentDeliveredMin250Fps = 127;
    telemetry.emptyTickPermille = 130;
    EXPECT_TRUE(policy::ShouldExitWgcHeadroomMode(telemetry));
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
    EXPECT_FALSE(policy::ShouldDropFrontWgcFrameForSelection(1, 2, true, 140));
    EXPECT_FALSE(policy::ShouldDropFrontWgcFrameForSelection(0, 4, true, 10));
    EXPECT_TRUE(policy::ShouldDropFrontWgcFrameForSelection(1, 4, false, 10));
    EXPECT_TRUE(policy::ShouldDropFrontWgcFrameForSelection(1, 4, true, 10));
}
