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

TEST(CapturePipelinePolicyTest, WgcIngressBudgetMeterThinsFastSourceBeforeTheReservoirFills) {
    // 60 fps CFR from a 144 Hz window: the reservoir is sized for 75 source frames/s, so a
    // half-empty reservoir must already be metered instead of admitting every 144 Hz frame.
    const auto exhausted = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/12, /*retainedFrameCap=*/35, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/60, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/2.0, /*freeCopySlots=*/25, /*reservedFreeCopySlots=*/6,
        /*uniformPlayoutOwnsSurplus=*/true, /*budgetCreditFrames=*/0.4);
    EXPECT_FALSE(exhausted.accept);
    EXPECT_TRUE(exhausted.decimated);
    EXPECT_FALSE(exhausted.softReservePressure);
    EXPECT_STREQ(exhausted.reason, "wgc_ingress_decimated_credit");

    const auto funded = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/12, /*retainedFrameCap=*/35, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/60, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/2.0, /*freeCopySlots=*/25, /*reservedFreeCopySlots=*/6,
        /*uniformPlayoutOwnsSurplus=*/true, /*budgetCreditFrames=*/1.0);
    EXPECT_TRUE(funded.accept);
    EXPECT_FALSE(funded.decimated);
    EXPECT_STREQ(funded.reason, "healthy");
}

TEST(CapturePipelinePolicyTest, WgcIngressBudgetMeterNeverBlocksLowWaterRecoveryOrSlowSources) {
    const auto lowWater = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/5, /*retainedFrameCap=*/35, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/60, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/0.0, /*freeCopySlots=*/25, /*reservedFreeCopySlots=*/6,
        /*uniformPlayoutOwnsSurplus=*/true, /*budgetCreditFrames=*/0.0);
    EXPECT_TRUE(lowWater.accept);
    EXPECT_STREQ(lowWater.reason, "low_water");

    const auto recovery = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/20, /*retainedFrameCap=*/35, /*lowWaterFrames=*/8, /*recovering=*/true,
        /*outputFps=*/60, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/0.0, /*freeCopySlots=*/25, /*reservedFreeCopySlots=*/6,
        /*uniformPlayoutOwnsSurplus=*/true, /*budgetCreditFrames=*/0.0);
    EXPECT_TRUE(recovery.accept);
    EXPECT_STREQ(recovery.reason, "recovery");

    const auto belowTarget = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/20, /*retainedFrameCap=*/35, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/60, /*recentInputMin250Fps=*/40, /*recentInputMin500Fps=*/40,
        /*admissionCreditFrames=*/0.0, /*freeCopySlots=*/25, /*reservedFreeCopySlots=*/6,
        /*uniformPlayoutOwnsSurplus=*/false, /*budgetCreditFrames=*/0.0);
    EXPECT_TRUE(belowTarget.accept);
    EXPECT_STREQ(belowTarget.reason, "source_below_cfr_target");
}

namespace {

struct BudgetMeterRun {
    uint32_t accepted = 0;
    uint32_t decimated = 0;
    uint32_t longestRejectRun = 0;
};

BudgetMeterRun RunBudgetMeter(uint32_t outputFps, double sourceFps, double seconds) {
    BudgetMeterRun run{};
    double credit = 1.0;
    uint32_t rejectRun = 0;
    const double intervalSeconds = 1.0 / sourceFps;
    const uint32_t frames = static_cast<uint32_t>(seconds * sourceFps);
    for (uint32_t i = 0; i < frames; ++i) {
        credit = policy::AdvanceWgcIngressBudgetCredit(credit, i == 0 ? 0.0 : intervalSeconds, outputFps);
        const auto decision = policy::DecideWgcIngressAdmission(
            /*retainedFrames=*/12, /*retainedFrameCap=*/35, /*lowWaterFrames=*/8, /*recovering=*/false, outputFps,
            static_cast<uint32_t>(sourceFps), static_cast<uint32_t>(sourceFps),
            /*admissionCreditFrames=*/2.0, /*freeCopySlots=*/25, /*reservedFreeCopySlots=*/6,
            /*uniformPlayoutOwnsSurplus=*/true, credit);
        if (decision.accept) {
            ++run.accepted;
            credit = std::max(0.0, credit - 1.0);
            rejectRun = 0;
        } else {
            ++run.decimated;
            run.longestRejectRun = std::max(run.longestRejectRun, ++rejectRun);
        }
    }
    return run;
}

}  // namespace

TEST(CapturePipelinePolicyTest, WgcIngressBudgetMeterHoldsFastSourcesToTheReservoirBudgetRate) {
    // 144 Hz into 60 fps: budget 75/s. The reservoir window (e.g. 331 ms) then holds ~25 frames
    // instead of ~48, which is what the pool was sized for, and the spacing stays regular.
    const BudgetMeterRun into60 = RunBudgetMeter(60, 144.0, 10.0);
    EXPECT_GE(into60.accepted, 700u);
    EXPECT_LE(into60.accepted, 790u);
    EXPECT_LE(into60.longestRejectRun, 2u);

    // 240 Hz into 120 fps: budget 150/s.
    const BudgetMeterRun into120 = RunBudgetMeter(120, 240.0, 10.0);
    EXPECT_GE(into120.accepted, 1450u);
    EXPECT_LE(into120.accepted, 1560u);
    EXPECT_LE(into120.longestRejectRun, 2u);
}

TEST(CapturePipelinePolicyTest, WgcIngressBudgetMeterAdmitsEverySourceFrameAtOrBelowTheBudget) {
    const std::pair<uint32_t, double> cases[] = {{120, 144.0}, {120, 120.0}, {60, 60.0},
                                                 {60, 74.0},   {144, 144.0}, {30, 37.0}};
    for (const auto& [outputFps, sourceFps] : cases) {
        const BudgetMeterRun run = RunBudgetMeter(outputFps, sourceFps, 10.0);
        EXPECT_EQ(run.decimated, 0u) << outputFps << " fps CFR from " << sourceFps << " Hz";
    }
}

TEST(CapturePipelinePolicyTest, WgcIngressBudgetCreditRefillIsCappedAndIgnoresClockGoingBackwards) {
    EXPECT_DOUBLE_EQ(policy::AdvanceWgcIngressBudgetCredit(0.0, 10.0, 60), policy::kWgcIngressBudgetCreditCapFrames);
    EXPECT_DOUBLE_EQ(policy::AdvanceWgcIngressBudgetCredit(0.5, -1.0, 60), 0.5);
    EXPECT_NEAR(policy::AdvanceWgcIngressBudgetCredit(0.0, 0.01, 60), 0.75, 1e-9);
}

TEST(CapturePipelinePolicyTest, WgcSmoothnessRequestedDelayIsTheFloorWhenOnlyAFloorWasRequested) {
    constexpr int64_t kReservoirQpc = 3000000;  // 300 ms at 10 MHz
    constexpr int64_t kFloorQpc = 224840;       // 22.5 ms
    // Video-only floor: the end-of-session deficit is judged against the floor, not the 300 ms headroom.
    EXPECT_EQ(policy::GetWgcSmoothnessRequestedDelayQpc(kReservoirQpc, true, false, kFloorQpc), kFloorQpc);
    // An audio-latency delay takes the whole reservoir target, with or without a floor configured.
    EXPECT_EQ(policy::GetWgcSmoothnessRequestedDelayQpc(kReservoirQpc, true, true, kFloorQpc), kReservoirQpc);
    EXPECT_EQ(policy::GetWgcSmoothnessRequestedDelayQpc(kReservoirQpc, false, false, 0), kReservoirQpc);
    // A floor can never ask for more than the reservoir can hold, and a floor that was never armed asks for none.
    EXPECT_EQ(policy::GetWgcSmoothnessRequestedDelayQpc(kFloorQpc, true, false, kReservoirQpc), kFloorQpc);
    EXPECT_EQ(policy::GetWgcSmoothnessRequestedDelayQpc(kReservoirQpc, true, false, 0), 0);
    EXPECT_EQ(policy::GetWgcSmoothnessRequestedDelayQpc(kReservoirQpc, true, false, -5), 0);
}
