#include "test_capture_pipeline_policy_shared.h"

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
    EXPECT_FALSE(softReservePressure.accept);
    EXPECT_TRUE(softReservePressure.decimated);
    EXPECT_TRUE(softReservePressure.softReservePressure);
    EXPECT_STREQ(softReservePressure.reason, "wgc_ingress_decimated_soft_reserve");
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

TEST(CapturePipelinePolicyTest, WgcIngressAdmissionSoftReserveProtectsPoolBeforeLowWaterAndRecovery) {
    auto lowWater = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/5, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/120, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/0.0, /*freeCopySlots=*/6, /*reservedFreeCopySlots=*/6);
    EXPECT_FALSE(lowWater.accept);
    EXPECT_TRUE(lowWater.decimated);
    EXPECT_TRUE(lowWater.softReservePressure);
    EXPECT_STREQ(lowWater.reason, "wgc_ingress_decimated_soft_reserve");

    auto recovery = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/18, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/true,
        /*outputFps=*/120, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/0.0, /*freeCopySlots=*/6, /*reservedFreeCopySlots=*/6);
    EXPECT_FALSE(recovery.accept);
    EXPECT_TRUE(recovery.decimated);
    EXPECT_TRUE(recovery.softReservePressure);
    EXPECT_STREQ(recovery.reason, "wgc_ingress_decimated_soft_reserve");
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

TEST(CapturePipelinePolicyTest, WgcIngressAdmissionSoftReserveProtectsPoolForBelowTargetSource) {
    const auto decision = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/18, /*retainedFrameCap=*/18, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/120, /*recentInputMin250Fps=*/108, /*recentInputMin500Fps=*/109,
        /*admissionCreditFrames=*/0.0, /*freeCopySlots=*/6, /*reservedFreeCopySlots=*/6);
    EXPECT_FALSE(decision.accept);
    EXPECT_TRUE(decision.decimated);
    EXPECT_TRUE(decision.softReservePressure);
    EXPECT_STREQ(decision.reason, "wgc_ingress_decimated_soft_reserve");
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
    EXPECT_EQ(policy::DeriveWgcSmoothnessFloorDelayQpc(heavy, kInterval120, kQpcFreq, kMaxMs, /*maxReservoirFrames=*/0), 0);
    EXPECT_EQ(policy::GetWgcSmoothnessFloorCapQpc(kInterval120, kQpcFreq, kMaxMs, /*maxReservoirFrames=*/0), 0);
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
                    policy::GetWgcSelectionTargetQpc(gridTick, /*fallbackTargetQpc=*/0, interval, /*recordingOutputLive=*/true,
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
    EXPECT_EQ(policy::GetWgcActiveDelaySelectionTargetQpc(scheduled, fallback, interval, /*recordingOutputLive=*/true,
                                                          /*applyLiveDelay=*/true, /*liveRecoveryActive=*/true,
                                                          /*uniformCadenceActiveDelay=*/true, contentDelay),
              scheduled - contentDelay);  // 1600 -- would have been 2000 (collapsed) before the fix
    EXPECT_EQ(
        policy::GetWgcActiveDelaySelectionTargetQpc(scheduled, fallback, interval, true, true,
                                                    /*liveRecoveryActive=*/false, /*uniformCadenceActiveDelay=*/true, contentDelay),
        scheduled - contentDelay);
    // Legacy reservoir-target path: live-recovery still legitimately yields to near-live catch-up.
    EXPECT_EQ(
        policy::GetWgcActiveDelaySelectionTargetQpc(scheduled, fallback, interval, true, true,
                                                    /*liveRecoveryActive=*/true, /*uniformCadenceActiveDelay=*/false, contentDelay),
        scheduled);
    EXPECT_EQ(
        policy::GetWgcActiveDelaySelectionTargetQpc(scheduled, fallback, interval, true, true,
                                                    /*liveRecoveryActive=*/false, /*uniformCadenceActiveDelay=*/false, contentDelay),
        scheduled - contentDelay);
    // applyLiveDelay false, or not live, never applies the delay regardless of path.
    EXPECT_EQ(policy::GetWgcActiveDelaySelectionTargetQpc(scheduled, fallback, interval, true,
                                                          /*applyLiveDelay=*/false, true, true, contentDelay),
              scheduled);
    EXPECT_EQ(policy::GetWgcActiveDelaySelectionTargetQpc(scheduled, fallback, interval, /*recordingOutputLive=*/false, true, true,
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

TEST(CapturePipelinePolicyTest, WgcCatchupUsesHeldRepeatEvenWhenSourceIsDegraded) {
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, false, 4, 2.0, 8, 120, 80, 82, 0, true), 2u);
    EXPECT_EQ(policy::GetWgcCatchupTicksThisLoop(false, false, 4, 2.0, 8, 120, 84, 84, 420, false), 2u);
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

TEST(CapturePipelinePolicyTest, WgcLiveSelectionTargetNeverFastForwardsPastCfrGrid) {
    EXPECT_EQ(policy::GetWgcLiveVisualDebtLimitQpc(100, 1000), 250);
    EXPECT_EQ(policy::GetWgcLiveVisualDebtLimitQpc(100, 0), 3200);
    EXPECT_EQ(policy::GetWgcLiveVisualDebtLimitTicks(100, 1000), 3u);
    EXPECT_EQ(policy::GetWgcLiveVisualDebtExcessTicks(3, 100, 1000), 0u);
    EXPECT_EQ(policy::GetWgcLiveVisualDebtExcessTicks(4, 100, 1000), 1u);
    EXPECT_EQ(policy::GetWgcLiveVisualDebtFloorQpc(1600, 100, 1000), 1350);

    EXPECT_EQ(policy::ClampWgcSelectionTargetToLiveQpc(1400, 1600, 100, 1000, false, false, 0, false), 1400);
    EXPECT_EQ(policy::ClampWgcSelectionTargetToLiveQpc(1000, 1600, 100, 1000, false, false, 3, false), 1000);
    EXPECT_EQ(policy::ClampWgcSelectionTargetToLiveQpc(1000, 1600, 100, 1000, false, false, 4, false), 1000);
    EXPECT_EQ(policy::ClampWgcSelectionTargetToLiveQpc(1000, 1600, 100, 1000, false, true, 20, true), 1000);
    EXPECT_EQ(policy::ClampWgcSelectionTargetToLiveQpc(1000, 1600, 100, 1000, false, true, 20, true,
                                                       policy::kCfrShortfallCatchupThresholdTicks, true),
              1000);
    EXPECT_EQ(policy::ClampWgcSelectionTargetToLiveQpc(1360, 1600, 100, 1000, true, true, 20, true), 1360);
    // A deliberate 400-tick content delay is the normal playout position.
    // Neither intentional delay nor overload debt may move the requested
    // source-content time ahead of its immutable output slot.
    EXPECT_EQ(policy::GetWgcLiveVisualDebtFloorQpcForMode(1600, 100, 1000, false, 400), 950);
    EXPECT_EQ(policy::GetWgcLiveVisualDebtFloorQpcForMode(1600, 100, 1000, true, 400), 1150);
    EXPECT_TRUE(policy::ShouldPruneWgcVisualDebtFrameForGrid(1000, 1050, 1200, 0));
    EXPECT_TRUE(policy::ShouldPruneWgcVisualDebtFrameForGrid(1000, 1090, 1200, 1100));
    EXPECT_FALSE(policy::ShouldPruneWgcVisualDebtFrameForGrid(1090, 1110, 1200, 1100));
    EXPECT_FALSE(policy::ShouldPruneWgcVisualDebtFrameForGrid(1110, 1150, 1200, 1100));
    EXPECT_EQ(policy::ClampWgcSelectionTargetToLiveQpc(1000, 1600, 100, 1000, false, true, 20, true,
                                                       policy::kCfrShortfallCatchupThresholdTicks, false, 400),
              1000);
    EXPECT_FALSE(
        policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(1000, 1040, 1020, 100, true, true, true, true));
}

TEST(CapturePipelinePolicyTest, WgcStartupSmoothnessHistoryOutlivesShallowerLiveDebtWindow) {
    constexpr int64_t kQpcPerSecond = 1000000;
    constexpr int64_t kOutputIntervalQpc = 8333;
    constexpr int64_t kRenderDelayQpc = 28000;
    constexpr int64_t kSmoothnessTargetQpc = 300000;
    constexpr int64_t kStartupContentDelayQpc = kRenderDelayQpc + kSmoothnessTargetQpc;
    constexpr int64_t kLatestQpc = 1399000;
    constexpr int64_t kLiveNowQpc = 1400000;

    const int64_t liveDebtLimitQpc =
        policy::GetWgcLiveVisualDebtLimitQpcForMode(kOutputIntervalQpc, kQpcPerSecond, false);
    ASSERT_EQ(liveDebtLimitQpc, 250000);
    EXPECT_TRUE(policy::ShouldProtectWgcStartupSmoothnessHistory(
        /*recordingOutputLive=*/false, /*startupSmoothnessAttempted=*/true, kSmoothnessTargetQpc,
        liveDebtLimitQpc));
    EXPECT_FALSE(policy::ShouldProtectWgcStartupSmoothnessHistory(
        /*recordingOutputLive=*/true, /*startupSmoothnessAttempted=*/true, kSmoothnessTargetQpc,
        liveDebtLimitQpc));
    EXPECT_FALSE(policy::ShouldProtectWgcStartupSmoothnessHistory(
        /*recordingOutputLive=*/false, /*startupSmoothnessAttempted=*/false, kSmoothnessTargetQpc,
        liveDebtLimitQpc));
    EXPECT_FALSE(policy::ShouldProtectWgcStartupSmoothnessHistory(
        /*recordingOutputLive=*/false, /*startupSmoothnessAttempted=*/true, liveDebtLimitQpc,
        liveDebtLimitQpc));

    std::vector<int64_t> fullHistory;
    for (int64_t timestampQpc = 1000000; timestampQpc <= kLatestQpc; timestampQpc += 7000) {
        fullHistory.push_back(timestampQpc);
    }
    const auto fullSelection = policy::SelectWgcStartupReserveCandidate(
        fullHistory.data(), fullHistory.size(), kStartupContentDelayQpc, kOutputIntervalQpc / 2);
    EXPECT_TRUE(fullSelection.usedDelayReserve);

    const int64_t oldLiveDebtFloorQpc = policy::GetWgcLiveVisualDebtFloorQpcForMode(
        kLiveNowQpc, kOutputIntervalQpc, kQpcPerSecond, false, kRenderDelayQpc);
    std::vector<int64_t> prematurelyPrunedHistory;
    for (const int64_t timestampQpc : fullHistory) {
        if (timestampQpc >= oldLiveDebtFloorQpc) {
            prematurelyPrunedHistory.push_back(timestampQpc);
        }
    }
    const auto prunedSelection = policy::SelectWgcStartupReserveCandidate(
        prematurelyPrunedHistory.data(), prematurelyPrunedHistory.size(), kStartupContentDelayQpc,
        kOutputIntervalQpc / 2);
    EXPECT_FALSE(prunedSelection.usedDelayReserve);
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
