#include "test_capture_pipeline_policy_shared.h"

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
    EXPECT_TRUE(policy::ShouldLiveRecoverySuppressWgcSelectionDelay(/*liveRecoveryActive=*/true,
                                                                    /*uniformCadenceActiveDelay=*/false));
    EXPECT_FALSE(policy::ShouldLiveRecoverySuppressWgcSelectionDelay(/*liveRecoveryActive=*/false,
                                                                     /*uniformCadenceActiveDelay=*/false));
    // Uniform-cadence path: the content delay is maintained continuously, even during live-recovery,
    // so the realized delay cannot collapse/latch. This is the fix for the 0..248ms delay swing.
    EXPECT_FALSE(policy::ShouldLiveRecoverySuppressWgcSelectionDelay(/*liveRecoveryActive=*/true,
                                                                     /*uniformCadenceActiveDelay=*/true));
    EXPECT_FALSE(policy::ShouldLiveRecoverySuppressWgcSelectionDelay(/*liveRecoveryActive=*/false,
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
    auto steady = policy::DecideWgcActiveDelayPace(1.0, /*bufferedFrames=*/6, /*floorFrames=*/4, kNoCap);
    EXPECT_TRUE(steady.advance);
    EXPECT_EQ(steady.dropBeforeAdvance, 0u);
    EXPECT_EQ(steady.capDrops, 0u);
    EXPECT_DOUBLE_EQ(steady.creditConsumed, 1.0);

    // Source below output (credit < 1): HOLD -> an evenly distributed source-limited repeat.
    auto under = policy::DecideWgcActiveDelayPace(0.9, /*bufferedFrames=*/6, /*floorFrames=*/4, kNoCap);
    EXPECT_FALSE(under.advance);
    EXPECT_EQ(under.dropBeforeAdvance, 0u);
    EXPECT_DOUBLE_EQ(under.creditConsumed, 0.0);

    // At the floor: hold even with full credit so the buffer never drains below the delay depth
    // (this is what keeps the realized delay stable / A/V sync preserved).
    auto atFloor = policy::DecideWgcActiveDelayPace(1.0, /*bufferedFrames=*/4, /*floorFrames=*/4, kNoCap);
    EXPECT_FALSE(atFloor.advance);
    EXPECT_EQ(atFloor.dropBeforeAdvance, 0u);

    // Source faster than output (credit >= 2) with headroom above floor+1: decimate one evenly,
    // then advance one. Keeps cadence smooth while holding the floor.
    auto over = policy::DecideWgcActiveDelayPace(2.5, /*bufferedFrames=*/8, /*floorFrames=*/4, kNoCap);
    EXPECT_TRUE(over.advance);
    EXPECT_EQ(over.dropBeforeAdvance, 1u);
    EXPECT_EQ(over.capDrops, 0u);
    EXPECT_DOUBLE_EQ(over.creditConsumed, 2.0);

    // Overcapture but only floor+1 buffered: do not decimate into the floor, just advance one.
    auto overTight = policy::DecideWgcActiveDelayPace(2.5, /*bufferedFrames=*/5, /*floorFrames=*/4, kNoCap);
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
    auto inflated = policy::DecideWgcActiveDelayPace(/*creditAfterIncrement=*/0.9, /*bufferedFrames=*/20, /*floorFrames=*/4, /*maxDepthFrames=*/6);
    EXPECT_EQ(inflated.capDrops, 14u);           // 20 -> 6
    EXPECT_EQ(inflated.dropBeforeAdvance, 14u);  // all from the cap, none credit-driven
    EXPECT_FALSE(inflated.advance);              // credit 0.9 < 1 after the cap, so just trim+hold
    EXPECT_DOUBLE_EQ(inflated.creditConsumed, 0.0);

    // The cap never trims into the delay floor: at exactly the cap there is nothing to trim.
    auto atCap = policy::DecideWgcActiveDelayPace(/*creditAfterIncrement=*/0.9, /*bufferedFrames=*/6, /*floorFrames=*/4, /*maxDepthFrames=*/6);
    EXPECT_EQ(atCap.capDrops, 0u);
    // A pathological maxDepth below floor+1 is clamped so the cap can never starve the reserve.
    auto clampLow = policy::DecideWgcActiveDelayPace(/*creditAfterIncrement=*/1.0, /*bufferedFrames=*/5, /*floorFrames=*/4, /*maxDepthFrames=*/0);
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
    EXPECT_TRUE(policy::ShouldDropWgcFrontForNearerPlayout(/*frontTimestampQpc=*/900, /*nextTimestampQpc=*/980, target, leadTol));
    EXPECT_TRUE(policy::ShouldDropWgcFrontForNearerPlayout(/*frontTimestampQpc=*/900, /*nextTimestampQpc=*/target + leadTol, target, leadTol));
    // The successor may be within the backend's broad lead tolerance yet still be farther from the
    // slot than an exact/closer front. True nearest-neighbour playout must retain the front.
    EXPECT_FALSE(policy::ShouldDropWgcFrontForNearerPlayout(/*frontTimestampQpc=*/target, /*nextTimestampQpc=*/target + 50, target, leadTol));
    // The successor is in the future beyond tolerance -> keep the front (it is the slot frame) and
    // hold the future frame as reserve.
    EXPECT_FALSE(
        policy::ShouldDropWgcFrontForNearerPlayout(/*frontTimestampQpc=*/980, /*nextTimestampQpc=*/target + leadTol + 1, target, leadTol));
    // Non-monotonic / duplicate successor is never advanced past.
    EXPECT_FALSE(policy::ShouldDropWgcFrontForNearerPlayout(/*frontTimestampQpc=*/900, /*nextTimestampQpc=*/900, target, leadTol));
    EXPECT_FALSE(policy::ShouldDropWgcFrontForNearerPlayout(/*frontTimestampQpc=*/900, /*nextTimestampQpc=*/880, target, leadTol));
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
    EXPECT_TRUE(policy::DecideWgcNearestPlayout(/*frontTimestampQpc=*/990, target, leadTol, /*lastEmittedTimestampQpc=*/950).emit);
    EXPECT_TRUE(policy::DecideWgcNearestPlayout(/*frontTimestampQpc=*/target + leadTol, target, leadTol, 950).emit);
    // Frame still in the future beyond tolerance -> hold (slot not aged in yet), keep as reserve.
    auto future = policy::DecideWgcNearestPlayout(/*frontTimestampQpc=*/target + leadTol + 1, target, leadTol, 950);
    EXPECT_FALSE(future.emit);
    EXPECT_TRUE(future.hold);
    // Non-monotonic front -> hold (never emit backwards).
    auto stale = policy::DecideWgcNearestPlayout(/*frontTimestampQpc=*/940, target, leadTol, /*lastEmittedTimestampQpc=*/950);
    EXPECT_FALSE(stale.emit);
    EXPECT_TRUE(stale.hold);
    // A lone frame OLDER than the target but newer than last emit is still emitted (freshest content
    // during a delivery gap) -> a clean monotonic hold/freeze, never a backward jump.
    EXPECT_TRUE(policy::DecideWgcNearestPlayout(/*frontTimestampQpc=*/700, target, leadTol, /*lastEmittedTimestampQpc=*/650).emit);
}

TEST(CapturePipelinePolicyTest, CaptureSyncPhaseLockAcquiresAcrossHalfFrameWrap) {
    policy::CfrCadencePhaseLockState state;
    constexpr int64_t interval = 100;
    int64_t adjustedTarget = 0;

    for (int tick = 0; tick < 80; ++tick) {
        const int64_t jitter = (tick & 1) == 0 ? -2 : 2;
        const int64_t source = 10000 + static_cast<int64_t>(tick) * interval + 49 + jitter;
        const int64_t target = 10000 + static_cast<int64_t>(tick) * interval;
        policy::ObserveCfrCaptureSyncSourceTimestamp(state, source, interval);
        adjustedTarget = policy::ApplyCfrCaptureSyncPhaseLock(state, target, source, interval, true);
    }

    EXPECT_TRUE(state.locked);
    EXPECT_EQ(state.acquisitions, 1u);
    EXPECT_EQ(state.releases, 0u);
    EXPECT_NEAR(state.lockedPhaseQpc, 49, 3);
    EXPECT_NEAR(adjustedTarget - (10000 + 79 * interval), 49, 3);
}

TEST(CapturePipelinePolicyTest, CaptureSyncPhaseLockFallsBackForGenuinelyVaryingCadence) {
    policy::CfrCadencePhaseLockState state;
    constexpr int64_t interval = 100;
    int64_t source = 10000;
    for (int tick = 0; tick < 40; ++tick) {
        source += interval;
        policy::ObserveCfrCaptureSyncSourceTimestamp(state, source, interval);
        policy::ApplyCfrCaptureSyncPhaseLock(state, 10000 + tick * interval, source, interval, true);
    }
    ASSERT_TRUE(state.locked);

    for (int tick = 0; tick < 6; ++tick) {
        source += (tick & 1) == 0 ? 70 : 130;
        policy::ObserveCfrCaptureSyncSourceTimestamp(state, source, interval);
    }
    const int64_t baseTarget = 20000;
    const int64_t adjusted = policy::ApplyCfrCaptureSyncPhaseLock(state, baseTarget, source, interval, true);

    EXPECT_FALSE(state.locked);
    EXPECT_EQ(state.releases, 1u);
    EXPECT_EQ(adjusted, baseTarget);
}

TEST(CapturePipelinePolicyTest, CaptureSyncPhaseLockReleasesWhenCadencePhaseWanders) {
    policy::CfrCadencePhaseLockState state;
    constexpr int64_t interval = 100;
    int64_t source = 10020;
    for (int tick = 0; tick < 40; ++tick) {
        source += interval;
        policy::ObserveCfrCaptureSyncSourceTimestamp(state, source, interval);
        policy::ApplyCfrCaptureSyncPhaseLock(state, 10000 + tick * interval, source, interval, true);
    }
    ASSERT_TRUE(state.locked);

    int64_t adjusted = 0;
    for (int tick = 40; tick < 64; ++tick) {
        // Each delta is close enough to the nominal interval to exercise phase-coherence fallback,
        // but the accumulated phase deliberately wanders instead of settling at a new limiter phase.
        source += 115;
        policy::ObserveCfrCaptureSyncSourceTimestamp(state, source, interval);
        adjusted = policy::ApplyCfrCaptureSyncPhaseLock(
            state, 10000 + static_cast<int64_t>(tick) * interval, source, interval, true);
    }

    EXPECT_FALSE(state.locked);
    EXPECT_EQ(state.releases, 1u);
    EXPECT_EQ(adjusted, 10000 + 63 * interval);
}

TEST(CapturePipelinePolicyTest, CaptureSyncPhaseLockSupportsStableUnderfeedAndMultiplier) {
    policy::CfrCadencePhaseLockState state;
    constexpr int64_t outputInterval = 200;
    const int64_t sourceInterval = policy::GetCfrCaptureSyncSourceIntervalQpc(outputInterval, 2);
    ASSERT_EQ(sourceInterval, 100);

    int64_t source = 10025;
    for (int tick = 0; tick < 60; ++tick) {
        // A stable half-rate source is still phase coherent: every interval spans two source-grid slots.
        source += 2 * sourceInterval;
        policy::ObserveCfrCaptureSyncSourceTimestamp(state, source, sourceInterval);
        policy::ApplyCfrCaptureSyncPhaseLock(state, 10000 + tick * outputInterval, source, sourceInterval, true);
    }

    EXPECT_TRUE(state.locked);
    EXPECT_EQ(state.releases, 0u);
}

TEST(CapturePipelinePolicyTest, CaptureSyncPhaseLockRephasesAfterStableSourcePhaseTransition) {
    policy::CfrCadencePhaseLockState state;
    constexpr int64_t interval = 100;
    int64_t source = 10020;
    for (int tick = 0; tick < 40; ++tick) {
        source += interval;
        policy::ObserveCfrCaptureSyncSourceTimestamp(state, source, interval);
        policy::ApplyCfrCaptureSyncPhaseLock(state, 10000 + tick * interval, source, interval, true);
    }
    ASSERT_TRUE(state.locked);
    const int64_t originalPhase = state.lockedPhaseQpc;

    source += 30;  // one discontinuity, followed by a new stable cadence phase
    for (int tick = 40; tick < 60; ++tick) {
        source += interval;
        policy::ObserveCfrCaptureSyncSourceTimestamp(state, source, interval);
        policy::ApplyCfrCaptureSyncPhaseLock(state, 10000 + tick * interval, source, interval, true);
    }

    EXPECT_TRUE(state.locked);
    EXPECT_GE(state.rephases, 1u);
    EXPECT_GT(policy::GetCfrTimestampDistanceQpc(state.lockedPhaseQpc, originalPhase), 20u);
}

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

TEST(CapturePipelinePolicyTest, WgcUniformPlayoutDeepGridDebtKeepsAudioAlignedTargetAuthoritative) {
    const int64_t interval = 100;
    const int64_t tol = policy::GetWgcActiveDelayResidualToleranceQpc(interval);  // 90
    constexpr int64_t kAudioAlignedTargetQpc = 1000;
    constexpr int64_t kOldestRetainedFrameQpc = 1600;
    constexpr int64_t kWallNowQpc = 5000;
    constexpr int64_t kConfiguredContentDelayQpc = 1000;

    // The removed wall-age heuristic accepted this frame even though it is 600 ticks newer than the
    // audio-aligned target. Grid-relative safety must win regardless of how old the frame is by wall time.
    EXPECT_GE(kWallNowQpc - kOldestRetainedFrameQpc, kConfiguredContentDelayQpc);
    EXPECT_TRUE(policy::IsWgcFrameTooNewForCfrSlot(
        kOldestRetainedFrameQpc, kAudioAlignedTargetQpc, interval));
    const auto decision = policy::DecideWgcNearestPlayout(
        kOldestRetainedFrameQpc, kAudioAlignedTargetQpc, tol, /*lastEmittedTimestampQpc=*/900);
    EXPECT_TRUE(decision.hold);
    EXPECT_FALSE(decision.emit);
    EXPECT_FALSE(policy::IsWgcActiveDelayFinalSelectionWithinHardLimit(
        kOldestRetainedFrameQpc, kOldestRetainedFrameQpc, kAudioAlignedTargetQpc, interval,
        /*qpcTicksPerSecond=*/1000));
}

TEST(CapturePipelinePolicyTest, WgcUniformPlayoutHeldRepeatCatchupRecoversDeepGridDebtWithoutContentLead) {
    const int64_t interval = 100;
    constexpr uint32_t kInitialShortfallTicks = 24;
    constexpr int kReservoirFrames = 8;
    const auto s = RunGridDebtCatchupPlayout(
        /*wallLoops=*/200, interval, /*contentDelay=*/400, kInitialShortfallTicks, kReservoirFrames);

    EXPECT_GT(s.repeatEmits, 0);
    EXPECT_GT(s.freshAfterDebt, 100);
    EXPECT_EQ(s.finalShortfallTicks, 0u);
    EXPECT_EQ(s.backwardFreshEmits, 0);
    EXPECT_LE(s.maxContentLead, policy::GetWgcActiveDelayResidualToleranceQpc(interval));
    EXPECT_LE(s.longestRepeatRun, static_cast<int>(kInitialShortfallTicks) + 2 * kReservoirFrames);
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

TEST(CapturePipelinePolicyTest, InjectTargetPlayoutUsesNormalDepthForPerfectMatchedCadence) {
    auto s = RunInjectTargetPlayout(/*ticks=*/1200, /*outputInterval=*/100, /*initialSourceInterval=*/100,
                                    /*contentDelay=*/400);
    EXPECT_EQ(s.emits, 1200);
    EXPECT_EQ(s.holds, 0);
    EXPECT_EQ(s.staleDrops, 3);  // one-time pre-roll history older than the first delayed target
    EXPECT_EQ(s.dropDupSameTickViolations, 0);
    EXPECT_GE(s.minRealizedDelay, 350);
    EXPECT_LE(s.maxRealizedDelay, 450);
}

TEST(CapturePipelinePolicyTest, CaptureSyncPhaseLockEliminatesHalfFrameBoundaryChurn) {
    const auto unlocked = RunCaptureSyncPhaseBoundaryPlayout(/*ticks=*/1200, /*enablePhaseLock=*/false);
    const auto locked = RunCaptureSyncPhaseBoundaryPlayout(/*ticks=*/1200, /*enablePhaseLock=*/true);

    EXPECT_GT(unlocked.holds, 500);
    EXPECT_GT(unlocked.staleDrops, 500);
    EXPECT_LT(locked.holds, 16);
    EXPECT_LT(locked.staleDrops, 16);
    EXPECT_EQ(locked.longestHoldRun, 1);
}

TEST(CapturePipelinePolicyTest, InjectTargetPlayoutResamplesLowHighAndVaryingSourceRatesWithoutChurn) {
    auto low = RunInjectTargetPlayout(/*ticks=*/1200, /*outputInterval=*/100, /*initialSourceInterval=*/200,
                                      /*contentDelay=*/600);
    EXPECT_GT(low.holds, 0);
    EXPECT_LE(low.longestHoldRun, 2);
    EXPECT_EQ(low.dropDupSameTickViolations, 0);

    auto high = RunInjectTargetPlayout(/*ticks=*/1200, /*outputInterval=*/100, /*initialSourceInterval=*/80,
                                       /*contentDelay=*/600);
    EXPECT_EQ(high.holds, 0);
    EXPECT_GT(high.staleDrops, 0);
    EXPECT_EQ(high.dropDupSameTickViolations, 0);

    auto varying = RunInjectTargetPlayout(/*ticks=*/1800, /*outputInterval=*/100, /*initialSourceInterval=*/133,
                                          /*contentDelay=*/800, /*varySourceRate=*/true);
    EXPECT_GT(varying.emits, 0);
    EXPECT_GT(varying.holds, 0);
    EXPECT_GT(varying.staleDrops, 0);
    EXPECT_LE(varying.longestHoldRun, 2);
    EXPECT_EQ(varying.dropDupSameTickViolations, 0);
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
