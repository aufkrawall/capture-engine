#include "test_capture_pipeline_policy_shared.h"

TEST(CapturePipelinePolicyTest, CfrOverloadPacerProbesRepeatCostWithoutDiscardingFreshLiveness) {
    constexpr double kFrameIntervalMs = 1000.0 / 120.0;
    constexpr uint32_t kFreshSamples = policy::kWgcOverloadRepeatPacerMinSamples;
    policy::CfrOverloadRepeatPacerState state;
    uint32_t probeDecisions = 0;

    for (uint32_t tick = 1; tick <= policy::kCfrOverloadRepeatProbeIntervalTicks * 2; ++tick) {
        const auto decision = policy::UpdateCfrOverloadRepeatPacer(
            state, true, true, true, true, true, 10.0, 0.0, kFrameIntervalMs, kFreshSamples, 0);
        EXPECT_FALSE(decision.active);
        EXPECT_STREQ(decision.reason,
                     decision.repeat ? "measuring_repeat_service" : "warming_repeat_service");
        if (decision.repeat) {
            EXPECT_TRUE(decision.probing);
            ++probeDecisions;
        }
    }

    EXPECT_EQ(probeDecisions, 2u);
    EXPECT_EQ(state.probeRepeats, 2u);
    EXPECT_EQ(state.proactiveRepeats, 2u);
    EXPECT_LE(state.maxConsecutiveProactiveRepeats, 1u);
}

TEST(CapturePipelinePolicyTest, CfrRepeatCatchupRequiresMeasuredHeadroomAndRejectsMuxPressure) {
    constexpr double kFrameIntervalMs = 1000.0 / 120.0;
    constexpr uint32_t kSamples = policy::kWgcOverloadRepeatPacerMinSamples;

    EXPECT_TRUE(policy::ShouldAllowCfrRepeatCatchupUnderFreshPressure(
        true, false, 5.0, kFrameIntervalMs, kSamples));
    EXPECT_FALSE(policy::ShouldAllowCfrRepeatCatchupUnderFreshPressure(
        false, false, 5.0, kFrameIntervalMs, kSamples));
    EXPECT_FALSE(policy::ShouldAllowCfrRepeatCatchupUnderFreshPressure(
        true, true, 5.0, kFrameIntervalMs, kSamples));
    EXPECT_FALSE(policy::ShouldAllowCfrRepeatCatchupUnderFreshPressure(
        true, false, 5.0, kFrameIntervalMs, kSamples - 1u));
    EXPECT_FALSE(policy::ShouldAllowCfrRepeatCatchupUnderFreshPressure(
        true, false, kFrameIntervalMs, kFrameIntervalMs, kSamples));
}

TEST(CapturePipelinePolicyTest, CfrOverloadPacingYieldsToFgSuspensionAndSourceUnderfeed) {
    EXPECT_TRUE(policy::IsCfrSourceHealthyForOverloadPacing(1.0, true, 480.0, 120.0));
    EXPECT_TRUE(policy::IsCfrSourceHealthyForOverloadPacing(1.0, true, 120.0, 120.0));
    EXPECT_FALSE(policy::IsCfrSourceHealthyForOverloadPacing(1.0, true, 115.0, 120.0));
    EXPECT_FALSE(policy::IsCfrSourceHealthyForOverloadPacing(0.89, true, 480.0, 120.0));
    EXPECT_TRUE(policy::IsCfrSourceHealthyForOverloadPacing(1.0, false, 0.0, 120.0));
}

TEST(CapturePipelinePolicyTest, DynamicOverlayRepeatsFreezeOnlyAcrossConfirmedFreshFramePressure) {
    constexpr double kFrameIntervalMs = 1000.0 / 120.0;
    policy::CfrDynamicOverlayRepeatState state;

    auto decision = policy::UpdateCfrDynamicOverlayRepeatState(state, true, 10.0, kFrameIntervalMs);
    EXPECT_FALSE(decision.frozen);
    EXPECT_FALSE(decision.entered);
    decision = policy::UpdateCfrDynamicOverlayRepeatState(state, true, 10.0, kFrameIntervalMs);
    EXPECT_TRUE(decision.frozen);
    EXPECT_TRUE(decision.entered);
    EXPECT_EQ(state.episodes, 1u);

    for (uint32_t frame = 1; frame < policy::kCfrDynamicOverlayRepeatExitConfirmFrames; ++frame) {
        decision = policy::UpdateCfrDynamicOverlayRepeatState(state, true, 5.0, kFrameIntervalMs);
        EXPECT_TRUE(decision.frozen);
        EXPECT_FALSE(decision.exited);
    }
    decision = policy::UpdateCfrDynamicOverlayRepeatState(state, true, 5.0, kFrameIntervalMs);
    EXPECT_FALSE(decision.frozen);
    EXPECT_TRUE(decision.exited);

    state.frozen = true;
    state.frozenRepeats = 17;
    decision = policy::UpdateCfrDynamicOverlayRepeatState(state, false, 10.0, kFrameIntervalMs);
    EXPECT_TRUE(decision.exited);
    EXPECT_FALSE(state.frozen);
    EXPECT_EQ(state.episodes, 1u);
    EXPECT_EQ(state.frozenRepeats, 17u);
}
