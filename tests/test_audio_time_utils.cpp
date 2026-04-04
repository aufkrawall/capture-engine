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

TEST(AudioTimeUtilsTest, RawQpcToHundredNanosecondsConvertsUsingFrequency) {
    EXPECT_EQ(ce::audio::RawQpcToHundredNanoseconds(30000000, 3000000), 100000000u);
    EXPECT_EQ(ce::audio::RawQpcToHundredNanoseconds(12345678, 10000000), 12345678u);
}

TEST(AudioTimeUtilsTest, RawQpcToHundredNanosecondsHandlesLargeCountersWithoutOverflow) {
    const uint64_t rawQpc = 1234567890123456789ULL;
    EXPECT_EQ(ce::audio::RawQpcToHundredNanoseconds(rawQpc, 10000000ULL), rawQpc);
}
