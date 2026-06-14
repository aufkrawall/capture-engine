#include <gtest/gtest.h>

#include "../mediaengine/audio_time_utils.h"

TEST(AudioTimeUtilsTest, HundredNanosecondsToMillisecondsMatchesWasapiUnits) {
    EXPECT_EQ(ce::audio::HundredNanosecondsToMilliseconds(0), 0u);
    EXPECT_EQ(ce::audio::HundredNanosecondsToMilliseconds(200000), 20u);
    EXPECT_EQ(ce::audio::HundredNanosecondsToMilliseconds(123456789), 12345u);
}

TEST(AudioTimeUtilsTest, HundredNanosecondsToSamplesRoundsToNearestSample) {
    EXPECT_EQ(ce::audio::HundredNanosecondsToSamples(0, 48000), 0u);
    EXPECT_EQ(ce::audio::HundredNanosecondsToSamples(100000, 48000), 480u);
    EXPECT_EQ(ce::audio::HundredNanosecondsToSamples(1830000, 48000), 8784u);
    EXPECT_EQ(ce::audio::HundredNanosecondsToSamples(1835000, 48000), 8808u);
    EXPECT_EQ(ce::audio::HundredNanosecondsToSamples(100000, 0), 0u);
}

TEST(AudioTimeUtilsTest, AudioFramesToHundredNanosecondsRoundsToNearestTick) {
    EXPECT_EQ(ce::audio::AudioFramesToHundredNanoseconds(0, 48000), 0u);
    EXPECT_EQ(ce::audio::AudioFramesToHundredNanoseconds(480, 48000), 100000u);
    EXPECT_EQ(ce::audio::AudioFramesToHundredNanoseconds(960, 48000), 200000u);
    EXPECT_EQ(ce::audio::AudioFramesToHundredNanoseconds(480, 0), 0u);
}

TEST(AudioTimeUtilsTest, RawQpcToHundredNanosecondsConvertsUsingFrequency) {
    EXPECT_EQ(ce::audio::RawQpcToHundredNanoseconds(30000000, 3000000), 100000000u);
    EXPECT_EQ(ce::audio::RawQpcToHundredNanoseconds(12345678, 10000000), 12345678u);
}

TEST(AudioTimeUtilsTest, RawQpcToHundredNanosecondsHandlesLargeCountersWithoutOverflow) {
    const uint64_t rawQpc = 1234567890123456789ULL;
    EXPECT_EQ(ce::audio::RawQpcToHundredNanoseconds(rawQpc, 10000000ULL), rawQpc);
}

TEST(AudioTimeUtilsTest, CaptureLatencyCompensationSubtractsWhenEnabled) {
    EXPECT_EQ(ce::audio::ApplyCaptureLatencyCompensation(1230000, 300000, true), 930000u);
    EXPECT_EQ(ce::audio::ApplyCaptureLatencyCompensation(1230000, 300000, false), 1230000u);
    EXPECT_EQ(ce::audio::ApplyCaptureLatencyCompensation(1230000, 0, true), 1230000u);
}

TEST(AudioTimeUtilsTest, CaptureLatencyCompensationClampsUnderflow) {
    EXPECT_EQ(ce::audio::ApplyCaptureLatencyCompensation(100000, 300000, true), 0u);
}

TEST(AudioTimeUtilsTest, ProcessLoopbackPacketTimestampCompensationUsesPacketCenterBias) {
    EXPECT_EQ(ce::audio::ApplyProcessLoopbackPacketTimestampCompensation(1230000, 480, 48000), 1180000u);
    EXPECT_EQ(ce::audio::ApplyProcessLoopbackPacketTimestampCompensation(1230000, 960, 48000), 1130000u);
    EXPECT_EQ(ce::audio::ApplyProcessLoopbackPacketTimestampCompensation(100000, 960, 48000), 0u);
}

