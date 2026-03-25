#include <gtest/gtest.h>

#include "../mediaengine/audio_sync_utils.h"

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

TEST(AudioSyncUtilsTest, AudioPullLatencyUsesStartupSlackUntilSourcesPrime) {
    constexpr int64_t kSteadyPullLatencyMs = ce::audio::kDefaultSteadyAudioPullLatencyMs;
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(kSteadyPullLatencyMs, true, 0), kSteadyPullLatencyMs);
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(kSteadyPullLatencyMs, false, 0), 30);
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(kSteadyPullLatencyMs, false, 95), 115);
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(kSteadyPullLatencyMs, false, 500), 120);
}

TEST(AudioSyncUtilsTest, AudioPullLatencyIsTrackScoped) {
    constexpr int64_t kSteadyPullLatencyMs = ce::audio::kDefaultSteadyAudioPullLatencyMs;
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(kSteadyPullLatencyMs, false, 120), 120);
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(kSteadyPullLatencyMs, true, 120), kSteadyPullLatencyMs);
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

TEST(AudioSyncUtilsTest, LatencyAdjustedDriftRemovesIntentionalPullOffset) {
    EXPECT_EQ(ce::audio::ComputeLatencyAdjustedAvDriftMs(-20, 20), 0);
    EXPECT_EQ(ce::audio::ComputeLatencyAdjustedAvDriftMs(-50, 20), -30);
    EXPECT_EQ(ce::audio::ComputeLatencyAdjustedAvDriftMs(15, 20), 35);
}

TEST(AudioSyncUtilsTest, WgcSteadyStateDriftCompensationRequiresHealthyLargeLead) {
    EXPECT_TRUE(ce::audio::ShouldAllowWgcSteadyStateDriftCompensation(true, 0, 12000, 960, 9600));
    EXPECT_FALSE(ce::audio::ShouldAllowWgcSteadyStateDriftCompensation(false, 0, 12000, 960, 9600));
    EXPECT_FALSE(ce::audio::ShouldAllowWgcSteadyStateDriftCompensation(true, 1, 12000, 960, 9600));
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
    EXPECT_EQ(normal.driftLagMs, 325);
    EXPECT_EQ(normal.targetBufferLagMs, 325);

    const auto coverage = ce::audio::ComputeWgcAudioLagTargets(5430, 17, true);
    EXPECT_EQ(coverage.driftLagMs, 17);
    EXPECT_EQ(coverage.targetBufferLagMs, 300);
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

TEST(AudioSyncUtilsTest, TrackStartupSettledUsesBootstrapOrPrimedSources) {
    EXPECT_FALSE(ce::audio::IsTrackAudioStartupSettled(false, false));
    EXPECT_TRUE(ce::audio::IsTrackAudioStartupSettled(true, false));
    EXPECT_TRUE(ce::audio::IsTrackAudioStartupSettled(false, true));
}

TEST(AudioSyncUtilsTest, OptionalUnstartedAppAudioDoesNotBlockStartupPriming) {
    EXPECT_TRUE(ce::audio::IsOptionalUnstartedAppAudioSource(true, false));
    EXPECT_FALSE(ce::audio::IsOptionalUnstartedAppAudioSource(true, true));
    EXPECT_FALSE(ce::audio::IsOptionalUnstartedAppAudioSource(false, false));

    EXPECT_TRUE(ce::audio::IsSourceStartupPrimed(false, false, true));
    EXPECT_FALSE(ce::audio::IsSourceStartupPrimed(false, true, true));
    EXPECT_FALSE(ce::audio::IsSourceStartupPrimed(false, false, false));
    EXPECT_TRUE(ce::audio::IsSourceStartupPrimed(true, true, false));
}

TEST(AudioSyncUtilsTest, BootstrapReadinessIgnoresOnlyUnstartedAppAudio) {
    EXPECT_TRUE(ce::audio::IsSourceBootstrapReady(false, false, false, true, 0, 240));
    EXPECT_FALSE(ce::audio::IsSourceBootstrapReady(false, true, false, true, 240, 240));
    EXPECT_FALSE(ce::audio::IsSourceBootstrapReady(false, false, false, false, 0, 240));
    EXPECT_TRUE(ce::audio::IsSourceBootstrapReady(false, true, true, false, 240, 240));
    EXPECT_TRUE(ce::audio::IsSourceBootstrapReady(true, false, false, false, 0, 240));
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

TEST(AudioSyncUtilsTest, PacketTimelineAdjustmentClampsNegativePacketStarts) {
    const auto adjustment = ce::audio::ComputePacketTimelineAdjustment(-200, 0, 48);
    EXPECT_EQ(adjustment.gapSamples, 0);
    EXPECT_EQ(adjustment.overlapSamples, 0);
}
