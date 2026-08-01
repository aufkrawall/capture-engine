#include "test_audio_sync_utils_shared.h"

TEST(AudioSyncUtilsTest, ComputesVideoPipelineLagOnlyWhenEncodedVideoLagsWallClock) {
    EXPECT_EQ(ce::audio::ComputeVideoPipelineLagMs(4600, 4275), 325);
    EXPECT_EQ(ce::audio::ComputeVideoPipelineLagMs(4275, 4600), 0);
    EXPECT_EQ(ce::audio::ComputeVideoPipelineLagMs(4600, 0), 0);
}

TEST(AudioSyncUtilsTest, BufferedAudioTargetIncludesBoundedVideoPipelineLag) {
    EXPECT_EQ(ce::audio::ComputeBufferedAudioTargetSamples(48000, 960, 325), 2880);
    EXPECT_EQ(ce::audio::ComputeBufferedAudioTargetSamples(48000, 960, 0), 960);
    EXPECT_EQ(ce::audio::ComputeBufferedAudioTargetSamples(48000, 960, 20), 1920);
}

TEST(AudioSyncUtilsTest, BufferedAudioTargetFallsBackToBaseLatencyForInvalidSampleRate) {
    EXPECT_EQ(ce::audio::ComputeBufferedAudioTargetSamples(0, 960, 325), 960);
    EXPECT_EQ(ce::audio::ComputeBufferedAudioTargetSamples(-1, 960, 325), 960);
}

TEST(AudioSyncUtilsTest, InjectCfrSourceClockTargetsIgnoreDownstreamEncoderLatency) {
    EXPECT_EQ(ce::audio::ResolveAudioSourceClockDriftLagMs(true, false, 0, 253, 41), 0);
    EXPECT_EQ(ce::audio::ResolveAudioTargetBufferLagMs(true, false, 0, 253), 0);
}

TEST(AudioSyncUtilsTest, ScreenGrabCfrRetainsMeasuredAudioLagTargets) {
    EXPECT_EQ(ce::audio::ResolveAudioSourceClockDriftLagMs(true, true, 236, 253, 41), 236);
    EXPECT_EQ(ce::audio::ResolveAudioTargetBufferLagMs(true, true, 292, 253), 292);
}

TEST(AudioSyncUtilsTest, NonCfrAudioLagTargetsRetainHistoricalPipelinePolicy) {
    EXPECT_EQ(ce::audio::ResolveAudioSourceClockDriftLagMs(false, false, 0, 253, 41), 294);
    EXPECT_EQ(ce::audio::ResolveAudioTargetBufferLagMs(false, false, 0, 253), 253);
}

TEST(AudioSyncUtilsTest, AudioPullLatencyUsesStartupSlackUntilSourcesPrime) {
    constexpr int64_t kSteadyPullLatencyMs = ce::audio::kDefaultSteadyAudioPullLatencyMs;
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(kSteadyPullLatencyMs, true, 0), kSteadyPullLatencyMs);
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(kSteadyPullLatencyMs, false, 0), kSteadyPullLatencyMs + 30);
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(kSteadyPullLatencyMs, false, 95),
              std::max<int64_t>(kSteadyPullLatencyMs + 30, 115));
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(kSteadyPullLatencyMs, false, 500), 120);
}

TEST(AudioSyncUtilsTest, AudioPullLatencyIsTrackScoped) {
    constexpr int64_t kSteadyPullLatencyMs = ce::audio::kDefaultSteadyAudioPullLatencyMs;
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(kSteadyPullLatencyMs, false, 120), 120);
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(kSteadyPullLatencyMs, true, 120), kSteadyPullLatencyMs);
}

TEST(AudioSyncUtilsTest, SettledCfrAudioPullKeepsBoundedCaptureLookahead) {
    EXPECT_EQ(ce::audio::ComputeSettledCfrAudioPullLatencyMs(60, true, true), 60);
    EXPECT_EQ(ce::audio::ComputeSettledCfrAudioPullLatencyMs(60, false, true), 60);
    EXPECT_EQ(ce::audio::ComputeSettledCfrAudioPullLatencyMs(90, true, false), 90);
    EXPECT_EQ(ce::audio::ComputeSettledCfrAudioPullLatencyMs(90, true, true), 60);
    EXPECT_EQ(ce::audio::ComputeSettledCfrAudioPullLatencyMs(60, true, true, 5), 5);
}

TEST(AudioSyncUtilsTest, AudioPullQuantumDefersOnlySmallSteadyStatePulls) {
    EXPECT_TRUE(ce::audio::ShouldDeferAudioPullUntilQuantum(0, true, false));
    EXPECT_TRUE(
        ce::audio::ShouldDeferAudioPullUntilQuantum(ce::audio::kDefaultAudioPullQuantumSamples - 1, true, false));
    EXPECT_FALSE(ce::audio::ShouldDeferAudioPullUntilQuantum(ce::audio::kDefaultAudioPullQuantumSamples, true, false));
    EXPECT_FALSE(ce::audio::ShouldDeferAudioPullUntilQuantum(240, true, false));
    EXPECT_FALSE(ce::audio::ShouldDeferAudioPullUntilQuantum(120, false, false));
    EXPECT_FALSE(ce::audio::ShouldDeferAudioPullUntilQuantum(120, true, true));
}

TEST(AudioSyncUtilsTest, SettledCfrAudioPullIsSampleExact) {
    EXPECT_TRUE(ce::audio::ShouldUseSampleExactCfrAudioPull(true, true, false));
    EXPECT_FALSE(ce::audio::ShouldUseSampleExactCfrAudioPull(true, false, false));
    EXPECT_FALSE(ce::audio::ShouldUseSampleExactCfrAudioPull(true, true, true));
    EXPECT_FALSE(ce::audio::ShouldUseSampleExactCfrAudioPull(false, true, false));

    EXPECT_EQ(ce::audio::ComputeAudioSamplesToEncode(1, true, true, false, false, false), 1);
    EXPECT_EQ(ce::audio::ComputeAudioSamplesToEncode(239, true, true, false, false, false), 239);
    EXPECT_EQ(ce::audio::ComputeAudioSamplesToEncode(239, false, true, false, false, false), 0);
    EXPECT_EQ(ce::audio::ComputeAudioSamplesToEncode(1000, false, true, false, false, true, 240, 960), 960);
    EXPECT_EQ(ce::audio::ComputeAudioSamplesToEncode(239, false, true, false, true, false), 0);
    EXPECT_EQ(ce::audio::ComputeAudioSamplesToEncode(239, false, false, false, false, false), 239);
}

TEST(AudioSyncUtilsTest, CfrAudioContinuityPolicyFollowsVfrMode) {
    EXPECT_TRUE(ce::audio::ShouldUseCfrAudioContinuityPolicy(false));
    EXPECT_FALSE(ce::audio::ShouldUseCfrAudioContinuityPolicy(true));
}

TEST(AudioSyncUtilsTest, WallClockAudioAnchorIsDisabledForCfr) {
    EXPECT_FALSE(ce::audio::ShouldAllowWallClockAudioAnchor(true, false, 5000));
    EXPECT_FALSE(ce::audio::ShouldAllowWallClockAudioAnchor(false, true, 5000));
    EXPECT_FALSE(ce::audio::ShouldAllowWallClockAudioAnchor(false, false, 500));
    EXPECT_TRUE(ce::audio::ShouldAllowWallClockAudioAnchor(false, false, 501));
}

TEST(AudioSyncUtilsTest, LatencyAdjustedDriftRemovesIntentionalPullOffset) {
    EXPECT_EQ(ce::audio::ComputeLatencyAdjustedAvDriftMs(-20, 20), 0);
    EXPECT_EQ(ce::audio::ComputeLatencyAdjustedAvDriftMs(-50, 20), -30);
    EXPECT_EQ(ce::audio::ComputeLatencyAdjustedAvDriftMs(15, 20), 35);
}

TEST(AudioSyncUtilsTest, DurationUsToSamplesRoundsToNearestSample) {
    EXPECT_EQ(ce::audio::ComputeDurationUsToSamples(8333, 48000), 400);
    EXPECT_EQ(ce::audio::ComputeDurationUsToSamples(16667, 48000), 800);
    EXPECT_EQ(ce::audio::ComputeDurationUsToSamples(20833, 48000), 1000);
    EXPECT_EQ(ce::audio::ComputeDurationUsToSamples(0, 48000), 0);
    EXPECT_EQ(ce::audio::ComputeDurationUsToSamples(-1, 48000), 0);
    EXPECT_EQ(ce::audio::ComputeDurationUsToSamples(1000, 0), 0);
}

TEST(AudioSyncUtilsTest, SamplesToDurationUsRoundsToNearestMicrosecond) {
    EXPECT_EQ(ce::audio::ComputeSamplesToDurationUs(400, 48000), 8333);
    EXPECT_EQ(ce::audio::ComputeSamplesToDurationUs(800, 48000), 16667);
    EXPECT_EQ(ce::audio::ComputeSamplesToDurationUs(-400, 48000), -8333);
    EXPECT_EQ(ce::audio::ComputeSamplesToDurationUs(0, 48000), 0);
    EXPECT_EQ(ce::audio::ComputeSamplesToDurationUs(100, 0), 0);
}

TEST(AudioSyncUtilsTest, SampleExactCfrAudioCursorHasNoLongDurationResidual) {
    constexpr int64_t kSampleRate = 48000;
    constexpr int64_t kFps = 120;
    constexpr int64_t kFrames = 36 * 60 * kFps;
    int64_t cursorSamples = 0;

    for (int64_t frame = 1; frame <= kFrames; ++frame) {
        const int64_t targetUs = ((frame * 1000000ll) + (kFps / 2)) / kFps;
        const int64_t targetSamples = ce::audio::ComputeDurationUsToSamples(targetUs, kSampleRate);
        const int64_t pendingSamples = targetSamples - cursorSamples;
        const int64_t samplesToEncode =
            ce::audio::ComputeAudioSamplesToEncode(pendingSamples, true, true, false, false, false);
        cursorSamples += samplesToEncode;
        ASSERT_EQ(cursorSamples, targetSamples) << "frame=" << frame;
    }
}

TEST(AudioSyncUtilsTest, LeadTrimExcessUsesHysteresisBand) {
    EXPECT_EQ(ce::audio::ComputeLeadTrimExcessSamples(6000, 3000, 2400, 240), 360);
    EXPECT_EQ(ce::audio::ComputeLeadTrimExcessSamples(5640, 3000, 2400, 240), 0);
    EXPECT_EQ(ce::audio::ComputeLeadTrimExcessSamples(5200, 3000, 2400, 240), 0);
    EXPECT_EQ(ce::audio::ComputeLeadTrimExcessSamples(-1, 3000, 2400, 240), 0);
}

TEST(AudioSyncUtilsTest, WgcSteadyStateDriftCompensationRequiresHealthyLargeLead) {
    EXPECT_TRUE(ce::audio::ShouldAllowWgcSteadyStateDriftCompensation(true, 0, 12000, 960, 9600));
    EXPECT_FALSE(ce::audio::ShouldAllowWgcSteadyStateDriftCompensation(false, 0, 12000, 960, 9600));
    EXPECT_TRUE(ce::audio::ShouldAllowWgcSteadyStateDriftCompensation(true, 1, 12000, 960, 9600));
    EXPECT_FALSE(ce::audio::ShouldAllowWgcSteadyStateDriftCompensation(true, 0, 10559, 960, 9600));
}

TEST(AudioSyncUtilsTest, WgcCoverageLossDetectionRequiresTrueLagMismatch) {
    EXPECT_TRUE(ce::audio::HasWgcUnrecoverableCoverageLoss(120, 16034, 12));
    EXPECT_TRUE(ce::audio::HasWgcUnrecoverableCoverageLoss(120, 76837, 0));
    EXPECT_FALSE(ce::audio::HasWgcUnrecoverableCoverageLoss(0, 16034, 12));
    EXPECT_FALSE(ce::audio::HasWgcUnrecoverableCoverageLoss(120, 200, 12));
    EXPECT_FALSE(ce::audio::HasWgcUnrecoverableCoverageLoss(120, 300, 220));
}

TEST(AudioSyncUtilsTest, WgcCoverageLossRatioTracksDeliveredFpsDeficit) {
    EXPECT_DOUBLE_EQ(ce::audio::ComputeWgcCoverageLossRatio(0, 0), 0.0);
    EXPECT_NEAR(ce::audio::ComputeWgcCoverageLossRatio(300, 220), 0.005, 0.000001);
    EXPECT_NEAR(ce::audio::ComputeWgcCoverageLossRatio(2000, 0), 0.125, 0.000001);
    EXPECT_NEAR(ce::audio::ComputeWgcCoverageLossRatio(16034, 12), 0.25, 0.000001);
}

TEST(AudioSyncUtilsTest, WgcCoverageBufferedAudioLagAddsBoundedHistoricalCushion) {
    EXPECT_EQ(ce::audio::ComputeWgcCoverageBufferedAudioLagMs(0, 0), 0);
    EXPECT_EQ(ce::audio::ComputeWgcCoverageBufferedAudioLagMs(300, 220), 0);
    EXPECT_EQ(ce::audio::ComputeWgcCoverageBufferedAudioLagMs(500, 20), 60);
    EXPECT_EQ(ce::audio::ComputeWgcCoverageBufferedAudioLagMs(2000, 0), 300);
}

TEST(AudioSyncUtilsTest, WgcCoverageAudioLagTargetsKeepDriftAndBufferLagSeparate) {
    const auto normal = ce::audio::ComputeWgcAudioLagTargets(325, 12, false);
    EXPECT_EQ(normal.driftLagMs, 12);
    EXPECT_EQ(normal.targetBufferLagMs, 12);

    const auto coverage = ce::audio::ComputeWgcAudioLagTargets(5430, 17, true);
    EXPECT_EQ(coverage.driftLagMs, 17);
    EXPECT_EQ(coverage.targetBufferLagMs, 300);
}

TEST(AudioSyncUtilsTest, WgcCfrLagSelectionPrefersBufferedVideoLagWhenRepeatingVideo) {
    const ce::audio::WgcAudioLagTargets coverageLagTargets{17, 300};
    EXPECT_EQ(ce::audio::ComputeWgcCfrDriftLagMs(coverageLagTargets, true, 4000), 17);
    EXPECT_EQ(ce::audio::ComputeWgcCfrTargetBufferLagMs(coverageLagTargets, 80, true, 4000), 300);

    const ce::audio::WgcAudioLagTargets steadyLagTargets{12, 12};
    EXPECT_EQ(ce::audio::ComputeWgcCfrTargetBufferLagMs(steadyLagTargets, 80, true, 4000), 80);
}

TEST(AudioSyncUtilsTest, WgcCfrLagSelectionCanStillUseEncoderShortfallWhenAudioCutsPreferred) {
    const ce::audio::WgcAudioLagTargets lagTargets{12, 24};
    EXPECT_EQ(ce::audio::ComputeWgcCfrDriftLagMs(lagTargets, false, 4000), 4012);
    EXPECT_EQ(ce::audio::ComputeWgcCfrTargetBufferLagMs(lagTargets, 80, false, 4000), 4000);
}

TEST(AudioSyncUtilsTest, WgcSteadyStateBufferedAudioLagAddsBoundedCushionForDegradedDelivery) {
    EXPECT_EQ(ce::audio::ComputeWgcSteadyStateBufferedAudioLagMs(0, 0, 0, 0, false, 0, 0, 0), 0);
    EXPECT_EQ(ce::audio::ComputeWgcSteadyStateBufferedAudioLagMs(120, 120, 120, 120, false, 0, 2, 0), 0);
    EXPECT_EQ(ce::audio::ComputeWgcSteadyStateBufferedAudioLagMs(120, 100, 100, 100, false, 0, 2, 0), 20);
    EXPECT_EQ(ce::audio::ComputeWgcSteadyStateBufferedAudioLagMs(120, 83, 83, 83, false, 0, 2, 0), 37);
    EXPECT_EQ(ce::audio::ComputeWgcSteadyStateBufferedAudioLagMs(120, 118, 118, 118, true, 0, 2, 0), 20);
    EXPECT_EQ(ce::audio::ComputeWgcSteadyStateBufferedAudioLagMs(120, 0, 0, 0, true, 0, 0, 0), 80);
    EXPECT_EQ(ce::audio::ComputeWgcSteadyStateBufferedAudioLagMs(120, 155, 116, 138, true, 403, 0, 46), 80);
    EXPECT_EQ(ce::audio::ComputeWgcSteadyStateBufferedAudioLagMs(120, 135, 116, 130, false, 180, 1, 18), 40);
}

TEST(AudioSyncUtilsTest, WgcSteadyStateBufferedAudioLagAvoidsLargeStarvationTargetWhenEncoderHealthy) {
    EXPECT_EQ(ce::audio::ComputeWgcSteadyStateBufferedAudioLagMs(120, 143, 124, 132, false, 250, 0, 46), 40);
    EXPECT_EQ(ce::audio::ComputeWgcSteadyStateBufferedAudioLagMs(120, 140, 136, 138, false, 217, 0, 30), 40);
}

TEST(AudioSyncUtilsTest, EncoderOverloadProtectionFollowsDeliveredFpsHealth) {
    EXPECT_TRUE(ce::audio::ShouldProtectWgcAudioContinuityDuringEncoderOverload(true, false, 120, 120));
    EXPECT_TRUE(ce::audio::ShouldProtectWgcAudioContinuityDuringEncoderOverload(true, false, 120, 116));
    EXPECT_FALSE(ce::audio::ShouldProtectWgcAudioContinuityDuringEncoderOverload(true, false, 120, 100));
    EXPECT_FALSE(ce::audio::ShouldProtectWgcAudioContinuityDuringEncoderOverload(true, true, 120, 120));
    EXPECT_FALSE(ce::audio::ShouldProtectWgcAudioContinuityDuringEncoderOverload(false, false, 120, 120));
    EXPECT_FALSE(ce::audio::ShouldProtectWgcAudioContinuityDuringEncoderOverload(true, false, 0, 120));
    EXPECT_FALSE(ce::audio::ShouldProtectWgcAudioContinuityDuringEncoderOverload(true, false, 120, 0));
}

TEST(AudioSyncUtilsTest, WgcPositiveCompensationHysteresisKeepsGuardBandNearTarget) {
    EXPECT_EQ(ce::audio::ComputeWgcPositiveCompensationHysteresisSamples(2880, 9600), 960);
    EXPECT_EQ(ce::audio::ComputeWgcPositiveCompensationHysteresisSamples(960, 9600), 480);
    EXPECT_EQ(ce::audio::ComputeWgcPositiveCompensationHysteresisSamples(0, 9600), 480);
}

TEST(AudioSyncUtilsTest, WgcPositiveCompensationClearsBeforeLeadIsFullySpent) {
    EXPECT_FALSE(ce::audio::ShouldClearWgcPositiveDriftCompensation(true, 3841, 2880, 960));
    EXPECT_TRUE(ce::audio::ShouldClearWgcPositiveDriftCompensation(true, 3840, 2880, 960));
    EXPECT_TRUE(ce::audio::ShouldClearWgcPositiveDriftCompensation(false, 12000, 2880, 960));
}

TEST(AudioSyncUtilsTest, WgcPositiveDriftCorrectionClampsAwayNearTargetSpendDown) {
    EXPECT_EQ(ce::audio::ClampWgcPositiveDriftCorrection(1800, 960), 840);
    EXPECT_EQ(ce::audio::ClampWgcPositiveDriftCorrection(960, 960), 0);
    EXPECT_EQ(ce::audio::ClampWgcPositiveDriftCorrection(600, 960), 0);
    EXPECT_EQ(ce::audio::ClampWgcPositiveDriftCorrection(-240, 960), -240);
}

TEST(AudioSyncUtilsTest, CfrTier1CompensationDeadbandIgnoresSmallBufferWobble) {
    constexpr int64_t kTenSecondWindowSamples = 48000 * 10;
    constexpr double kCfrMaxPitchPercent = 0.05;
    constexpr int64_t kDeadbandSamples = 48000 / 12;

    EXPECT_EQ(ce::audio::ComputeTier1CompensationDeltaWithDeadband(-240, kTenSecondWindowSamples, kCfrMaxPitchPercent,
                                                                   kDeadbandSamples),
              0);
    EXPECT_EQ(ce::audio::ComputeTier1CompensationDeltaWithDeadband(240, kTenSecondWindowSamples, kCfrMaxPitchPercent,
                                                                   kDeadbandSamples),
              0);
    EXPECT_EQ(ce::audio::ComputeTier1CompensationDeltaWithDeadband(-3098, kTenSecondWindowSamples, kCfrMaxPitchPercent,
                                                                   kDeadbandSamples),
              0);
    EXPECT_EQ(ce::audio::ComputeTier1CompensationDeltaWithDeadband(-4800, kTenSecondWindowSamples, kCfrMaxPitchPercent,
                                                                   kDeadbandSamples),
              -240);
    EXPECT_EQ(ce::audio::ComputeTier1CompensationDeltaWithDeadband(4800, kTenSecondWindowSamples, kCfrMaxPitchPercent,
                                                                   kDeadbandSamples),
              240);
}

TEST(AudioSyncUtilsTest, CfrAppAudioBacklogDrainIgnoresSmallExcess) {
    constexpr int64_t kRate = 48000;
    constexpr int64_t kTarget = kRate * 140 / 1000;
    constexpr int64_t kSlack = kRate * 20 / 1000;
    constexpr int64_t kDeadband = kRate * 10 / 1000;
    const auto decision =
        ce::audio::ComputeCfrAppAudioBacklogDrainDecision(true, true, false, true, false, false,
                                                          kTarget + (kRate * 15 / 1000), kTarget, kRate / 64,
                                                          kRate * 10, 0.5, kSlack, kDeadband);

    EXPECT_FALSE(decision.active);
    EXPECT_EQ(decision.reason, ce::audio::CfrAppAudioBacklogDrainReason::WithinSlack);
    EXPECT_EQ(decision.compensationDelta, 0);
}

TEST(AudioSyncUtilsTest, CfrAppAudioBacklogDrainUsesAppPitchCapForElevatedExcess) {
    constexpr int64_t kRate = 48000;
    constexpr int64_t kTarget = kRate * 140 / 1000;
    constexpr int64_t kSlack = kRate * 20 / 1000;
    constexpr int64_t kDeadband = kRate * 10 / 1000;
    constexpr int64_t kTenSecondWindowSamples = kRate * 10;
    const auto decision = ce::audio::ComputeCfrAppAudioBacklogDrainDecision(
        true, true, false, true, false, false, kTarget + (kRate * 85 / 1000), kTarget, kRate / 64,
        kTenSecondWindowSamples, 0.5, kSlack, kDeadband);

    EXPECT_TRUE(decision.active);
    EXPECT_EQ(decision.reason, ce::audio::CfrAppAudioBacklogDrainReason::Active);
    EXPECT_GT(decision.compensationDelta, 0);
    EXPECT_LE(decision.compensationDelta,
              ce::audio::ComputeTier1CompensationDelta(kTenSecondWindowSamples, kTenSecondWindowSamples, 0.5));
}

TEST(AudioSyncUtilsTest, SharedCaptureSiblingBacklogMustNotDriveHealthyRouteCompensation) {
    constexpr int64_t kRate = 48000;
    constexpr int64_t kTarget = kRate * 100 / 1000;
    constexpr int64_t kHealthyRouteBuffer = kRate * 105 / 1000;
    constexpr int64_t kDelayedSiblingBuffer = kRate * 1600 / 1000;
    constexpr int64_t kSlack = kRate * 20 / 1000;
    constexpr int64_t kDeadband = kRate * 10 / 1000;

    const auto healthyRoute = ce::audio::ComputeCfrAppAudioBacklogDrainDecision(
        true, true, false, true, false, false, kHealthyRouteBuffer, kTarget, kRate / 64, kRate * 10, 0.5, kSlack,
        kDeadband);
    const auto delayedSibling = ce::audio::ComputeCfrAppAudioBacklogDrainDecision(
        true, true, false, true, false, false, kDelayedSiblingBuffer, kTarget, kRate / 64, kRate * 10, 0.5, kSlack,
        kDeadband);

    EXPECT_FALSE(healthyRoute.active);
    EXPECT_EQ(healthyRoute.compensationDelta, 0);
    EXPECT_TRUE(delayedSibling.active);
    EXPECT_GT(delayedSibling.compensationDelta, 0);
}

TEST(AudioSyncUtilsTest, CfrAppAudioBacklogDrainRequiresEligibleAppCfrState) {
    constexpr int64_t kRate = 48000;
    constexpr int64_t kTarget = kRate * 140 / 1000;
    constexpr int64_t kBacklog = kTarget + (kRate * 85 / 1000);
    constexpr int64_t kSlack = kRate * 20 / 1000;
    constexpr int64_t kDeadband = kRate * 10 / 1000;

    EXPECT_EQ(ce::audio::ComputeCfrAppAudioBacklogDrainDecision(false, true, false, true, false, false, kBacklog,
                                                                kTarget, kRate / 64, kRate * 10, 0.5, kSlack,
                                                                kDeadband)
                  .reason,
              ce::audio::CfrAppAudioBacklogDrainReason::NotCfr);
    EXPECT_EQ(ce::audio::ComputeCfrAppAudioBacklogDrainDecision(true, false, false, true, false, false, kBacklog,
                                                                kTarget, kRate / 64, kRate * 10, 0.5, kSlack,
                                                                kDeadband)
                  .reason,
              ce::audio::CfrAppAudioBacklogDrainReason::NotAppAudio);
    EXPECT_EQ(ce::audio::ComputeCfrAppAudioBacklogDrainDecision(true, true, true, true, false, false, kBacklog, kTarget,
                                                                kRate / 64, kRate * 10, 0.5, kSlack, kDeadband)
                  .reason,
              ce::audio::CfrAppAudioBacklogDrainReason::ForceDrain);
    EXPECT_EQ(ce::audio::ComputeCfrAppAudioBacklogDrainDecision(true, true, false, false, false, false, kBacklog,
                                                                kTarget, kRate / 64, kRate * 10, 0.5, kSlack,
                                                                kDeadband)
                  .reason,
              ce::audio::CfrAppAudioBacklogDrainReason::StartupNotSettled);
    EXPECT_EQ(ce::audio::ComputeCfrAppAudioBacklogDrainDecision(true, true, false, true, true, false, kBacklog, kTarget,
                                                                kRate / 64, kRate * 10, 0.5, kSlack, kDeadband)
                  .reason,
              ce::audio::CfrAppAudioBacklogDrainReason::StartupTimelineProtected);
}

TEST(AudioSyncUtilsTest, CfrAppAudioBacklogDrainPausesDuringTimelineRecovery) {
    constexpr int64_t kRate = 48000;
    constexpr int64_t kTarget = kRate * 140 / 1000;
    constexpr int64_t kBacklog = kTarget + (kRate * 2);
    constexpr int64_t kSlack = kRate * 20 / 1000;
    constexpr int64_t kDeadband = kRate * 10 / 1000;

    const auto decision = ce::audio::ComputeCfrAppAudioBacklogDrainDecision(
        true, true, false, true, false, true, kBacklog, kTarget, kRate / 64, kRate * 10, 0.5, kSlack, kDeadband);

    EXPECT_FALSE(decision.active);
    EXPECT_EQ(decision.compensationDelta, 0);
    EXPECT_EQ(decision.reason, ce::audio::CfrAppAudioBacklogDrainReason::TimelineRecoveryActive);
}

TEST(AudioSyncUtilsTest, Tier2TrimOnlyActivatesForPositiveLead) {
    EXPECT_TRUE(ce::audio::ShouldActivateTier2Trim(1200, 48000, 20));
    EXPECT_FALSE(ce::audio::ShouldActivateTier2Trim(-1200, 48000, 20));
    EXPECT_FALSE(ce::audio::ShouldActivateTier2Trim(959, 48000, 20));
}

TEST(AudioSyncUtilsTest, WgcLiveShortfallSuppressesPositiveDriftCorrection) {
    EXPECT_TRUE(ce::audio::ShouldSuppressWgcPositiveDriftCorrectionDuringLiveShortfall(true, false, 100, false));
    EXPECT_TRUE(ce::audio::ShouldSuppressWgcPositiveDriftCorrectionDuringLiveShortfall(true, false, 0, true));
    EXPECT_FALSE(ce::audio::ShouldSuppressWgcPositiveDriftCorrectionDuringLiveShortfall(true, true, 1000, true));
    EXPECT_FALSE(ce::audio::ShouldSuppressWgcPositiveDriftCorrectionDuringLiveShortfall(false, false, 1000, true));
    EXPECT_FALSE(ce::audio::ShouldSuppressWgcPositiveDriftCorrectionDuringLiveShortfall(true, false, 99, false));
}

TEST(AudioSyncUtilsTest, CfrLiveShortfallSuppressesPositiveDriftCorrection) {
    EXPECT_TRUE(ce::audio::ShouldSuppressCfrPositiveDriftCorrectionDuringLiveShortfall(true, false, 100, false));
    EXPECT_TRUE(ce::audio::ShouldSuppressCfrPositiveDriftCorrectionDuringLiveShortfall(true, false, 0, true));
    EXPECT_FALSE(ce::audio::ShouldSuppressCfrPositiveDriftCorrectionDuringLiveShortfall(true, true, 1000, true));
    EXPECT_FALSE(ce::audio::ShouldSuppressCfrPositiveDriftCorrectionDuringLiveShortfall(false, false, 1000, true));
}

TEST(AudioSyncUtilsTest, CfrSourceClockCompensationRequiresSettledHealthyTimeline) {
    EXPECT_TRUE(ce::audio::ShouldAllowCfrSourceClockDriftCompensation(true, false, true, false, false, 0, false));
    EXPECT_FALSE(ce::audio::ShouldAllowCfrSourceClockDriftCompensation(false, false, true, false, false, 0, false));
    EXPECT_FALSE(ce::audio::ShouldAllowCfrSourceClockDriftCompensation(true, true, true, false, false, 0, false));
    EXPECT_FALSE(ce::audio::ShouldAllowCfrSourceClockDriftCompensation(true, false, false, false, false, 0, false));
    EXPECT_FALSE(ce::audio::ShouldAllowCfrSourceClockDriftCompensation(true, false, true, true, false, 0, false));
    EXPECT_FALSE(ce::audio::ShouldAllowCfrSourceClockDriftCompensation(true, false, true, false, true, 0, false));
    EXPECT_FALSE(ce::audio::ShouldAllowCfrSourceClockDriftCompensation(true, false, true, false, false, 100, false));
    EXPECT_FALSE(ce::audio::ShouldAllowCfrSourceClockDriftCompensation(true, false, true, false, false, 0, true));
}

TEST(AudioSyncUtilsTest, WgcRuntimeOverflowCapUsesRingCapacityGuard) {
    EXPECT_EQ(ce::audio::ComputeRuntimeOverflowCapSamples(false, 6720, 1440000, 24000, 48000), 24000);
    EXPECT_EQ(ce::audio::ComputeRuntimeOverflowCapSamples(true, 6720, 1440000, 24000, 48000), 1385280);
    EXPECT_EQ(ce::audio::ComputeRuntimeOverflowCapSamples(true, 6720, 40000, 24000, 48000), 24000);
}

TEST(AudioSyncUtilsTest, AudioEndBoundaryAccountsForEncodedAndQueuedSamples) {
    EXPECT_EQ(ce::audio::ComputeAudioSamplesAllowedBeforeEnd(48000, 47000, 500), 500);
    EXPECT_EQ(ce::audio::ComputeAudioSamplesAllowedBeforeEnd(48000, 48000, 0), 0);
    EXPECT_EQ(ce::audio::ComputeAudioSamplesAllowedBeforeEnd(48000, 47000, 2000), 0);
    EXPECT_EQ(ce::audio::ComputeAudioSamplesAllowedBeforeEnd(-1, -1, -1), 0);
}

TEST(AudioSyncUtilsTest, PackedSilenceBufferIncludesEveryChannel) {
    EXPECT_EQ(ce::audio::ComputeAudioSampleBufferBytes(720, 4, 2), 5760u);
    EXPECT_EQ(ce::audio::ComputeAudioSampleBufferBytes(720, 4, 6), 17280u);
    EXPECT_EQ(ce::audio::ComputeAudioPlaneOffsetBytes(0, 720, 4, false), 0u);
    EXPECT_EQ(ce::audio::ComputeAudioPlaneOffsetBytes(1, 720, 4, false), 0u);
    EXPECT_EQ(ce::audio::ComputeAudioPlaneOffsetBytes(1, 720, 4, true), 2880u);
    EXPECT_EQ(ce::audio::ComputeAudioSampleBufferBytes(0, 4, 2), 0u);
}

TEST(AudioSyncUtilsTest, FinalPacketDurationClampsToVideoSampleTarget) {
    auto clamp = ce::audio::ClampPacketDurationToTargetSamples(523264, 1024, 524000);
    EXPECT_TRUE(clamp.keep);
    EXPECT_TRUE(clamp.clamped);
    EXPECT_EQ(clamp.durationSamples, 736);

    auto exact = ce::audio::ClampPacketDurationToTargetSamples(522976, 1024, 524000);
    EXPECT_TRUE(exact.keep);
    EXPECT_FALSE(exact.clamped);
    EXPECT_EQ(exact.durationSamples, 1024);

    auto past = ce::audio::ClampPacketDurationToTargetSamples(524000, 1024, 524000);
    EXPECT_FALSE(past.keep);
    EXPECT_EQ(past.durationSamples, 0);
}

TEST(AudioSyncUtilsTest, WgcCoverageLossTrimSamplesUsesFractionalAccumulatorAndCap) {
    double accumulator = 0.0;
    EXPECT_EQ(ce::audio::ComputeWgcCoverageLossTrimSamples(240, 0.05, accumulator, 48), 12);
    EXPECT_NEAR(accumulator, 0.0, 0.000001);

    accumulator = 0.0;
    EXPECT_EQ(ce::audio::ComputeWgcCoverageLossTrimSamples(240, 0.021, accumulator, 48), 5);
    EXPECT_NEAR(accumulator, 0.04, 0.000001);
    EXPECT_EQ(ce::audio::ComputeWgcCoverageLossTrimSamples(240, 0.021, accumulator, 48), 5);
    EXPECT_NEAR(accumulator, 0.08, 0.000001);

    accumulator = 0.0;
    EXPECT_EQ(ce::audio::ComputeWgcCoverageLossTrimSamples(240, 0.50, accumulator, 24), 24);
    EXPECT_NEAR(accumulator, 96.0, 0.000001);
    EXPECT_EQ(ce::audio::ComputeWgcCoverageLossTrimSamples(240, 0.0, accumulator, 24), 0);
    EXPECT_NEAR(accumulator, 96.0, 0.000001);

    accumulator = 0.0;
    EXPECT_EQ(ce::audio::ComputeWgcCoverageLossTrimSamples(960, 0.25, accumulator, 240), 240);
    EXPECT_NEAR(accumulator, 0.0, 0.000001);
}

TEST(AudioSyncUtilsTest, WgcBufferedVideoContentLagUsesSharedTelemetryAge) {
    EXPECT_EQ(ce::audio::ComputeWgcBufferedVideoContentLagMs(0), 0);
    EXPECT_EQ(ce::audio::ComputeWgcBufferedVideoContentLagMs(12345), 12);
}

TEST(AudioSyncUtilsTest, WgcRecordingCadenceFavorsOutputFpsOverAdaptiveCaptureTarget) {
    EXPECT_EQ(ce::audio::GetWgcRecordingCadenceFps(120, 130), 120u);
    EXPECT_EQ(ce::audio::GetWgcRecordingCadenceFps(0, 130), 130u);
    EXPECT_EQ(ce::audio::GetWgcRecordingCadenceFps(0, 0), 0u);
}

TEST(AudioSyncUtilsTest, TrackStartupSettledUsesBootstrapOrPrimedSources) {
    EXPECT_FALSE(ce::audio::IsTrackAudioStartupSettled(false, false));
    EXPECT_TRUE(ce::audio::IsTrackAudioStartupSettled(true, false));
    EXPECT_TRUE(ce::audio::IsTrackAudioStartupSettled(false, true));
}

TEST(AudioSyncUtilsTest, OptionalUnstartedAppAudioDoesNotBlockStartupPriming) {
    EXPECT_TRUE(ce::audio::IsOptionalUnstartedAppAudioSource(true, false));
    EXPECT_FALSE(ce::audio::IsOptionalUnstartedAppAudioSource(true, false, true));
    EXPECT_FALSE(ce::audio::IsOptionalUnstartedAppAudioSource(true, true));
    EXPECT_FALSE(ce::audio::IsOptionalUnstartedAppAudioSource(false, false));

    EXPECT_TRUE(ce::audio::IsSourceStartupPrimed(false, false, true));
    EXPECT_FALSE(ce::audio::IsSourceStartupPrimed(false, false, true, true));
    EXPECT_FALSE(ce::audio::IsSourceStartupPrimed(false, true, true));
    EXPECT_FALSE(ce::audio::IsSourceStartupPrimed(false, false, false));
    EXPECT_TRUE(ce::audio::IsSourceStartupPrimed(true, true, false));
}

TEST(AudioSyncUtilsTest, IdleSystemLoopbackSilenceIsNotAnUnderrun) {
    EXPECT_TRUE(ce::audio::IsExpectedSourceTimelineSilence(false, false, true, false));
    EXPECT_FALSE(ce::audio::IsExpectedSourceTimelineSilence(false, false, true, true));
    EXPECT_FALSE(ce::audio::IsExpectedSourceTimelineSilence(false, false, false, false));
    EXPECT_TRUE(ce::audio::IsExpectedSourceTimelineSilence(true, true, false, true));
}

TEST(AudioSyncUtilsTest, BootstrapReadinessRequiresPrimedOrBufferedRealAudio) {
    EXPECT_TRUE(ce::audio::IsSourceBootstrapReady(false, false, false, true, 0, 240));
    EXPECT_FALSE(ce::audio::IsSourceBootstrapReady(false, true, false, true, 0, 240));
    EXPECT_FALSE(ce::audio::IsSourceBootstrapReady(false, true, false, false, 0, 240));
    EXPECT_FALSE(ce::audio::IsSourceBootstrapReady(false, true, false, false, 239, 240));
    EXPECT_TRUE(ce::audio::IsSourceBootstrapReady(false, true, false, false, 240, 240));
    EXPECT_TRUE(ce::audio::IsSourceBootstrapReady(false, true, true, false, 0, 240));
    EXPECT_FALSE(ce::audio::IsSourceBootstrapReady(false, false, false, false, 0, 240));
    EXPECT_TRUE(ce::audio::IsSourceBootstrapReady(true, false, false, false, 0, 240));
}

TEST(AudioSyncUtilsTest, BootstrapStartedSourceMustCoverFirstRequestedTimelineRange) {
    EXPECT_FALSE(ce::audio::IsSourceBootstrapTimelineReady(
        /*sourceBootstrapComplete*/ false, /*optionalUnstartedSource*/ false, /*sourceReady*/ true,
        /*packetlessSilenceReady*/ false, /*bufferedTimelineSamples*/ 1920, /*targetTimelineSamples*/ 2000));
    EXPECT_TRUE(ce::audio::IsSourceBootstrapTimelineReady(false, false, true, false, /*bufferedTimelineSamples*/ 2000,
                                                          /*targetTimelineSamples*/ 2000));
    EXPECT_TRUE(ce::audio::IsSourceBootstrapTimelineReady(false, false, false, true, /*bufferedTimelineSamples*/ 0,
                                                          /*targetTimelineSamples*/ 2000));
    EXPECT_TRUE(ce::audio::IsSourceBootstrapTimelineReady(false, true, true, false, /*bufferedTimelineSamples*/ 0,
                                                          /*targetTimelineSamples*/ 2000));
}

TEST(AudioSyncUtilsTest, SilentStartedSystemSourceIsTimelineReadyAtSampleZero) {
    EXPECT_FALSE(ce::audio::IsSourceBootstrapReady(false, true, false, false, 0, 1200));
    EXPECT_FALSE(ce::audio::IsSourceStartupPrimed(false, true, false));
}

TEST(AudioSyncUtilsTest, AppAudioRemainsOptionalUntilTimelineValid) {
    EXPECT_TRUE(ce::audio::IsOptionalUnstartedAppAudioSource(true, false));
    EXPECT_TRUE(ce::audio::IsSourceBootstrapReady(false, false, false, true, 0, 1200));
    EXPECT_FALSE(ce::audio::IsOptionalUnstartedAppAudioSource(true, false, true));
    EXPECT_FALSE(ce::audio::IsSourceBootstrapReady(false, false, false, true, 0, 1200, true));
    EXPECT_FALSE(ce::audio::IsOptionalUnstartedAppAudioSource(true, true));
    EXPECT_FALSE(ce::audio::IsSourceBootstrapReady(false, true, false, true, 0, 1200));
    EXPECT_FALSE(ce::audio::IsSourceBootstrapReady(false, true, false, true, 0, 1200, true));
    EXPECT_TRUE(ce::audio::IsSourceBootstrapReady(false, true, false, true, 1200, 1200));
}

TEST(AudioSyncUtilsTest, BootstrapRealAudioRequirementKeepsBoundedStartupCushion) {
    EXPECT_EQ(ce::audio::ComputeRequiredBootstrapRealSamples(320, 1200), 1200u);
    EXPECT_EQ(ce::audio::ComputeRequiredBootstrapRealSamples(1920, 1200), 1200u);
    EXPECT_EQ(ce::audio::ComputeRequiredBootstrapRealSamples(96000, 1200), 1200u);
    EXPECT_EQ(ce::audio::ComputeRequiredBootstrapRealSamples(-1, 1200), 1200u);
}

TEST(AudioSyncUtilsTest, BufferedRealAudioExcludesSyntheticStartupSamples) {
    EXPECT_EQ(ce::audio::ComputeBufferedRealAudioSamples(960, 0), 960u);
    EXPECT_EQ(ce::audio::ComputeBufferedRealAudioSamples(960, 240), 720u);
    EXPECT_EQ(ce::audio::ComputeBufferedRealAudioSamples(960, 960), 0u);
    EXPECT_EQ(ce::audio::ComputeBufferedRealAudioSamples(960, 1200), 0u);
}

TEST(AudioSyncUtilsTest, ConsumingSyntheticSamplesTracksOldestBufferedPortion) {
    uint64_t syntheticSamples = 480;
    EXPECT_EQ(ce::audio::ConsumeSyntheticBufferedSamples(syntheticSamples, 120), 120u);
    EXPECT_EQ(syntheticSamples, 360u);
    EXPECT_EQ(ce::audio::ConsumeSyntheticBufferedSamples(syntheticSamples, 600), 360u);
    EXPECT_EQ(syntheticSamples, 0u);
}

TEST(AudioSyncUtilsTest, SourceTimelineWriteCursorNeverTrailsEncodedTrackCursor) {
    EXPECT_EQ(ce::audio::ResolveSourceTimelineWriteCursor(0, 24000), 24000);
    EXPECT_EQ(ce::audio::ResolveSourceTimelineWriteCursor(32000, 24000), 32000);
    EXPECT_EQ(ce::audio::ResolveSourceTimelineWriteCursor(32000, -1), 32000);
}

TEST(AudioSyncUtilsTest, AppAudioCaptureEpochTransitionDistinguishesRestartFromOrdinaryGap) {
    EXPECT_FALSE(ce::audio::IsAppAudioCaptureEpochTransition(true, 0, 1));
    EXPECT_FALSE(ce::audio::IsAppAudioCaptureEpochTransition(true, 1, 1));
    EXPECT_TRUE(ce::audio::IsAppAudioCaptureEpochTransition(true, 1, 2));
    EXPECT_TRUE(ce::audio::IsAppAudioCaptureEpochTransition(true, 9, 3));
    EXPECT_FALSE(ce::audio::IsAppAudioCaptureEpochTransition(true, 1, 0));
    EXPECT_FALSE(ce::audio::IsAppAudioCaptureEpochTransition(false, 1, 2));
}

TEST(AudioSyncUtilsTest, EverySuccessfulWasapiReactivationStartsANewEpoch) {
    EXPECT_FALSE(ce::audio::IsAudioCaptureEpochTransition(0, 1));
    EXPECT_FALSE(ce::audio::IsAudioCaptureEpochTransition(4, 4));
    EXPECT_TRUE(ce::audio::IsAudioCaptureEpochTransition(4, 5));
    EXPECT_FALSE(ce::audio::IsAudioCaptureEpochTransition(4, 0));
}

TEST(AudioSyncUtilsTest, RestartedAppAudioEpochReusesLateJoinInsteadOfMaterializingLongAbsence) {
    constexpr int64_t kRate = 48000;
    const bool firstTimelinePacketAfterEpochReset = ce::audio::IsAppAudioCaptureEpochTransition(true, 1, 2);
    const auto join = ce::audio::ComputeLateAppSourceJoin(true, firstTimelinePacketAfterEpochReset, false, kRate * 1430,
                                                          kRate * 1430, kRate / 2, kRate / 100);

    ASSERT_TRUE(join.joinLive);
    EXPECT_EQ(join.joinCursorSamples, kRate * 1430);
    EXPECT_EQ(join.preservedGapSamples, 0);
    EXPECT_EQ(join.suppressedGapSamples, kRate * 1430);
}

TEST(AudioSyncUtilsTest, PreStartAppAudioEvidenceBlocksOptionalStartupUntilPrimed) {
    EXPECT_TRUE(ce::audio::ShouldRememberPreStartPacketForAppBootstrap(
        /*isAppAudioSource*/ true, /*firstSourcePacket*/ true, /*packetTimestampMs*/ 990,
        /*recordingStartQpcMs*/ 1000));
    EXPECT_FALSE(ce::audio::ShouldRememberPreStartPacketForAppBootstrap(
        /*isAppAudioSource*/ false, /*firstSourcePacket*/ true, /*packetTimestampMs*/ 990,
        /*recordingStartQpcMs*/ 1000));
    EXPECT_FALSE(ce::audio::ShouldRememberPreStartPacketForAppBootstrap(
        /*isAppAudioSource*/ true, /*firstSourcePacket*/ false, /*packetTimestampMs*/ 990,
        /*recordingStartQpcMs*/ 1000));
    EXPECT_FALSE(ce::audio::ShouldRememberPreStartPacketForAppBootstrap(
        /*isAppAudioSource*/ true, /*firstSourcePacket*/ true, /*packetTimestampMs*/ 996,
        /*recordingStartQpcMs*/ 1000));

    EXPECT_FALSE(ce::audio::IsOptionalUnstartedAppAudioSource(
        /*isAppAudioSource*/ true, /*sourceTimelineValid*/ false, /*sawPreStartPackets*/ true));
    EXPECT_FALSE(ce::audio::IsSourceStartupPrimed(
        /*sourceIsPrimed*/ false, /*sourceTimelineValid*/ false, /*isAppAudioSource*/ true,
        /*sawPreStartPackets*/ true));
    EXPECT_FALSE(ce::audio::IsSourceBootstrapReady(
        /*sourceBootstrapComplete*/ false, /*sourceTimelineValid*/ false, /*sourceIsPrimed*/ false,
        /*isAppAudioSource*/ true, /*bufferedRealSamples*/ 0, /*requiredRealSamples*/ 1200,
        /*sawPreStartPackets*/ true));
    EXPECT_TRUE(ce::audio::IsSourceBootstrapReady(
        /*sourceBootstrapComplete*/ false, /*sourceTimelineValid*/ true, /*sourceIsPrimed*/ true,
        /*isAppAudioSource*/ true, /*bufferedRealSamples*/ 0, /*requiredRealSamples*/ 1200,
        /*sawPreStartPackets*/ true));
}

TEST(AudioSyncUtilsTest, LateAppSourceFirstPacketJoinsCurrentTrackCursor) {
    const auto join =
        ce::audio::ComputeLateAppSourceJoin(true, true, false, 48000 * 7, 48000 * 7 + 960, 48000 / 2, 480);

    EXPECT_TRUE(join.joinLive);
    EXPECT_EQ(join.joinCursorSamples, 48000 * 7 + 960);
    EXPECT_EQ(join.preservedGapSamples, 0);
    EXPECT_EQ(join.suppressedGapSamples, 48000 * 7);
}

TEST(AudioSyncUtilsTest, LateAppSourceCanPreserveSmallFadeCushion) {
    const auto join = ce::audio::ComputeLateAppSourceJoin(true, true, false, 48000 * 7, 48000 * 6, 48000 / 2, 480);

    EXPECT_TRUE(join.joinLive);
    EXPECT_EQ(join.joinCursorSamples, 48000 * 7 - 480);
    EXPECT_EQ(join.preservedGapSamples, 480);
    EXPECT_EQ(join.suppressedGapSamples, 48000 * 7 - 480);
}

TEST(AudioSyncUtilsTest, LateAppSourceJoinLeavesStartupAndNonAppSourcesUnchanged) {
    EXPECT_FALSE(
        ce::audio::ComputeLateAppSourceJoin(false, true, false, 48000 * 7, 48000 * 7, 48000 / 2, 480).joinLive);
    EXPECT_FALSE(
        ce::audio::ComputeLateAppSourceJoin(true, false, false, 48000 * 7, 48000 * 7, 48000 / 2, 480).joinLive);
    EXPECT_FALSE(ce::audio::ComputeLateAppSourceJoin(true, true, true, 48000 * 7, 48000 * 7, 48000 / 2, 480).joinLive);
    EXPECT_FALSE(ce::audio::ComputeLateAppSourceJoin(true, true, false, 1200, 1200, 48000 / 2, 480).joinLive);
}

TEST(AudioSyncUtilsTest, StopDrainRunsOnlyForVideoAudioSessions) {
    EXPECT_TRUE(ce::audio::ShouldDrainStoppedCaptureQueuesBeforeFinalAudioPull(true, false, 1));
    EXPECT_FALSE(ce::audio::ShouldDrainStoppedCaptureQueuesBeforeFinalAudioPull(false, false, 1));
    EXPECT_FALSE(ce::audio::ShouldDrainStoppedCaptureQueuesBeforeFinalAudioPull(true, true, 1));
    EXPECT_FALSE(ce::audio::ShouldDrainStoppedCaptureQueuesBeforeFinalAudioPull(true, false, 0));
}

TEST(AudioSyncUtilsTest, CfrAudioPullDefersUntilActiveSourceHasRequestedSamples) {
    EXPECT_TRUE(ce::audio::ShouldDeferCfrAudioPullForSourceBuffer(true, false, false, false, 480, 479));
    EXPECT_FALSE(ce::audio::ShouldDeferCfrAudioPullForSourceBuffer(true, false, false, false, 480, 480));
    EXPECT_FALSE(ce::audio::ShouldDeferCfrAudioPullForSourceBuffer(false, false, false, false, 480, 0));
    EXPECT_FALSE(ce::audio::ShouldDeferCfrAudioPullForSourceBuffer(true, true, false, false, 480, 0));
    EXPECT_FALSE(ce::audio::ShouldDeferCfrAudioPullForSourceBuffer(true, false, true, false, 480, 0));
    EXPECT_FALSE(ce::audio::ShouldDeferCfrAudioPullForSourceBuffer(true, false, false, true, 480, 0));
}

TEST(AudioSyncUtilsTest, FinalCfrSourceCatchupWaitsOnlyForStrictStartedSources) {
    EXPECT_TRUE(ce::audio::ShouldWaitForFinalCfrSourceCatchup(true, true, false, false, 480, 479));
    EXPECT_FALSE(ce::audio::ShouldWaitForFinalCfrSourceCatchup(true, true, false, false, 480, 480));
    EXPECT_FALSE(ce::audio::ShouldWaitForFinalCfrSourceCatchup(false, true, false, false, 480, 0));
    EXPECT_FALSE(ce::audio::ShouldWaitForFinalCfrSourceCatchup(true, false, false, false, 480, 0));
    EXPECT_FALSE(ce::audio::ShouldWaitForFinalCfrSourceCatchup(true, true, true, false, 480, 0));
    EXPECT_FALSE(ce::audio::ShouldWaitForFinalCfrSourceCatchup(true, true, false, true, 480, 0));
    EXPECT_FALSE(ce::audio::ShouldWaitForFinalCfrSourceCatchup(true, true, false, false, 0, 0));
}

TEST(AudioSyncUtilsTest, StartedSparseTimelineSourcesMayContributeSilence) {
    EXPECT_TRUE(ce::audio::ShouldTreatSparseStartedSourceAsSilence(true, true, true, false));
    EXPECT_FALSE(ce::audio::ShouldTreatSparseStartedSourceAsSilence(false, true, true, false));
    EXPECT_FALSE(ce::audio::ShouldTreatSparseStartedSourceAsSilence(true, false, true, false));
    EXPECT_FALSE(ce::audio::ShouldTreatSparseStartedSourceAsSilence(true, true, false, false));
    EXPECT_FALSE(ce::audio::ShouldTreatSparseStartedSourceAsSilence(true, true, true, true));
    EXPECT_TRUE(ce::audio::ShouldTreatSparseStartedSourceAsSilence(true, true, true, false, true));

    EXPECT_TRUE(ce::audio::ShouldTreatStartedTimelineSourceShortfallAsSilence(true, 0));
    EXPECT_FALSE(ce::audio::ShouldTreatStartedTimelineSourceShortfallAsSilence(true, 1));
    EXPECT_FALSE(ce::audio::ShouldTreatStartedTimelineSourceShortfallAsSilence(false, 0));
}

TEST(AudioSyncUtilsTest, InactiveStartedAppCaptureBecomesNonBlockingTimelineSilence) {
    EXPECT_TRUE(ce::audio::ShouldTreatInactiveStartedAppCaptureAsSilence(
        /*isCfrRecording*/ true, /*isAppAudioSource*/ true, /*sourceHasStarted*/ true,
        /*captureActive*/ false));
    EXPECT_FALSE(ce::audio::ShouldTreatInactiveStartedAppCaptureAsSilence(true, true, true, true));
    EXPECT_FALSE(ce::audio::ShouldTreatInactiveStartedAppCaptureAsSilence(true, true, false, false));
    EXPECT_FALSE(ce::audio::ShouldTreatInactiveStartedAppCaptureAsSilence(false, true, true, false));
    EXPECT_FALSE(ce::audio::ShouldTreatInactiveStartedAppCaptureAsSilence(true, false, true, false));

    const bool inactiveSourceMaySilence =
        ce::audio::ShouldTreatInactiveStartedAppCaptureAsSilence(true, true, true, false);
    EXPECT_FALSE(ce::audio::ShouldDeferCfrAudioPullForSourceBuffer(true, false, false, inactiveSourceMaySilence,
                                                                   /*requestedSamples*/ 48000 * 2,
                                                                   /*bufferedTimelineSamples*/ 0));
    EXPECT_FALSE(ce::audio::ShouldWaitForFinalCfrSourceCatchup(true, true, false, inactiveSourceMaySilence,
                                                               /*requestedSamples*/ 48000 * 2,
                                                               /*bufferedTimelineSamples*/ 0));
}

TEST(AudioSyncUtilsTest, PacketlessTimelineSourceCanBootstrapAsSilence) {
    EXPECT_TRUE(ce::audio::ShouldBootstrapPacketlessSourceAsSilence(true, true, 0, 1200, 1200));
    EXPECT_FALSE(ce::audio::ShouldBootstrapPacketlessSourceAsSilence(false, true, 0, 1200, 1200));
    EXPECT_FALSE(ce::audio::ShouldBootstrapPacketlessSourceAsSilence(true, false, 0, 1200, 1200));
    EXPECT_FALSE(ce::audio::ShouldBootstrapPacketlessSourceAsSilence(true, true, 1, 1200, 1200));
    EXPECT_FALSE(ce::audio::ShouldBootstrapPacketlessSourceAsSilence(true, true, 0, 1199, 1200));
}

TEST(AudioSyncUtilsTest, StartedSparseTimelinePartialShortfallSilencesOnlyLargeLivePulls) {
    constexpr int64_t thresholdSamples = ce::audio::kDefaultAudioPullQuantumSamples * 4;

    EXPECT_TRUE(ce::audio::ShouldTreatStartedTimelineSourceShortfallAsSilence(
        /*sparseStartedSourceMaySilence*/ true, /*bufferedTimelineSamples*/ 0,
        /*requestedSamples*/ ce::audio::kDefaultAudioPullQuantumSamples, thresholdSamples));
    EXPECT_FALSE(ce::audio::ShouldTreatStartedTimelineSourceShortfallAsSilence(
        /*sparseStartedSourceMaySilence*/ true, /*bufferedTimelineSamples*/ 320, /*requestedSamples*/ 400,
        thresholdSamples));
    EXPECT_TRUE(ce::audio::ShouldTreatStartedTimelineSourceShortfallAsSilence(
        /*sparseStartedSourceMaySilence*/ true, /*bufferedTimelineSamples*/ 320, /*requestedSamples*/ 13600,
        thresholdSamples));

    const bool sparsePartialLargePullMaySilence = ce::audio::ShouldTreatStartedTimelineSourceShortfallAsSilence(
        /*sparseStartedSourceMaySilence*/ true, /*bufferedTimelineSamples*/ 320, /*requestedSamples*/ 13600,
        thresholdSamples);
    EXPECT_FALSE(ce::audio::ShouldDeferCfrAudioPullForSourceBuffer(
        /*isCfrRecording*/ true, /*forceDrain*/ false, /*optionalUnstartedSource*/ false,
        sparsePartialLargePullMaySilence, /*requestedSamples*/ 13600, /*bufferedTimelineSamples*/ 320));
}

TEST(AudioSyncUtilsTest, LatePacketAfterCommittedSilenceOverlapsAlreadyEncodedRange) {
    const int64_t writeCursor = ce::audio::ResolveSourceTimelineWriteCursor(0, 24000);
    const auto adjustment = ce::audio::ComputePacketTimelineAdjustment(18000, writeCursor, 48);
    EXPECT_EQ(adjustment.gapSamples, 0);
    EXPECT_EQ(adjustment.overlapSamples, 6000);
}

TEST(AudioSyncUtilsTest, PacketAfterRealSilenceAddsOnlyUnencodedGapRemainder) {
    const int64_t writeCursor = ce::audio::ResolveSourceTimelineWriteCursor(24000, 45120);
    const auto adjustment = ce::audio::ComputePacketTimelineAdjustment(48000, writeCursor, 48);
    EXPECT_EQ(adjustment.gapSamples, 2880);
    EXPECT_EQ(adjustment.overlapSamples, 0);
}

TEST(AudioSyncUtilsTest, PacketTimelineAdjustmentIgnoresSmallClockSkew) {
    const auto adjustment = ce::audio::ComputePacketTimelineAdjustment(1008, 960, 48);
    EXPECT_EQ(adjustment.gapSamples, 0);
    EXPECT_EQ(adjustment.overlapSamples, 0);
}

TEST(AudioSyncUtilsTest, PacketTimelineAdjustmentComputesTimelineGap) {
    const auto adjustment = ce::audio::ComputePacketTimelineAdjustment(1320, 960, 48);
    EXPECT_EQ(adjustment.gapSamples, 360);
    EXPECT_EQ(adjustment.overlapSamples, 0);
}

TEST(AudioSyncUtilsTest, PacketTimelineAdjustmentComputesPacketOverlap) {
    const auto adjustment = ce::audio::ComputePacketTimelineAdjustment(720, 960, 48);
    EXPECT_EQ(adjustment.gapSamples, 0);
    EXPECT_EQ(adjustment.overlapSamples, 240);
}

// Defense-in-depth bound that prevents an out-of-domain packet timestamp from
// sizing a pathological leading-silence allocation (192 kHz-loopback bad_alloc).
TEST(AudioSyncUtilsTest, TimelineGapClampLeavesInRangeGapsUntouched) {
    // 30 s stereo ring at 48 kHz -> 1.44M samples of capacity per channel-frame.
    const int64_t ringCapacitySamples = 48000 * 30;
    EXPECT_EQ(ce::audio::ClampTimelineGapSamplesToCapacity(480, ringCapacitySamples), 480);
    EXPECT_EQ(ce::audio::ClampTimelineGapSamplesToCapacity(ringCapacitySamples, ringCapacitySamples),
              ringCapacitySamples);
}

TEST(AudioSyncUtilsTest, TimelineGapClampBoundsOversizedGapToRingCapacity) {
    const int64_t ringCapacitySamples = 48000 * 30;
    // The crash value: ~2 trillion samples (~497 days) -> clamped to ring capacity.
    EXPECT_EQ(ce::audio::ClampTimelineGapSamplesToCapacity(2061584303294LL, ringCapacitySamples), ringCapacitySamples);
}
