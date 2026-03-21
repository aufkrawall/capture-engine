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
    EXPECT_TRUE(policy::ShouldCommitRecordingWarmup(false, false, true, false, 2, 2,
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
    EXPECT_EQ(policy::GetWarmupInjectKeepCount(0.0, 8.333), 2u);
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
