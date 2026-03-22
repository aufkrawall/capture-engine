#include <gtest/gtest.h>

#include "../mediaengine/audio_sync_utils.h"

TEST(AudioSyncUtilsTest, ComputesVideoPipelineLagOnlyWhenEncodedVideoLagsWallClock) {
    EXPECT_EQ(ce::audio::ComputeVideoPipelineLagMs(4600, 4275), 325);
    EXPECT_EQ(ce::audio::ComputeVideoPipelineLagMs(4275, 4600), 0);
    EXPECT_EQ(ce::audio::ComputeVideoPipelineLagMs(4600, 0), 0);
}

TEST(AudioSyncUtilsTest, BufferedAudioTargetIncludesVideoPipelineLag) {
    EXPECT_EQ(ce::audio::ComputeBufferedAudioTargetSamples(48000, 960, 325), 16560);
    EXPECT_EQ(ce::audio::ComputeBufferedAudioTargetSamples(48000, 960, 0), 960);
}

TEST(AudioSyncUtilsTest, BufferedAudioTargetFallsBackToBaseLatencyForInvalidSampleRate) {
    EXPECT_EQ(ce::audio::ComputeBufferedAudioTargetSamples(0, 960, 325), 960);
    EXPECT_EQ(ce::audio::ComputeBufferedAudioTargetSamples(-1, 960, 325), 960);
}

TEST(AudioSyncUtilsTest, AudioPullLatencyUsesStartupSlackUntilSourcesPrime) {
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(50, true, 0), 50);
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(50, false, 0), 80);
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(50, false, 95), 115);
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(50, false, 500), 120);
}

TEST(AudioSyncUtilsTest, AudioPullLatencyIsTrackScoped) {
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(50, false, 120), 120);
    EXPECT_EQ(ce::audio::ComputeAudioPullLatencyMs(50, true, 120), 50);
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
