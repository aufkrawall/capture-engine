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

// --- WASAPI capture QPC sanity guard (192 kHz-loopback bad_alloc regression) ---

TEST(AudioTimeUtilsTest, SanitizeCaptureQpcPassesThroughHealthyPositions) {
    const uint64_t now = 305000000000ULL;  // ~8.5h uptime in 100-ns units
    // Slightly in the past (typical: timestamp of a just-read buffer) -> untouched.
    EXPECT_EQ(ce::audio::SanitizeCaptureQpcPosition(now - 50000, now), now - 50000);
    // Exactly now -> untouched.
    EXPECT_EQ(ce::audio::SanitizeCaptureQpcPosition(now, now), now);
    // Small benign future skew within tolerance -> untouched (raw QPC preserved).
    EXPECT_EQ(ce::audio::SanitizeCaptureQpcPosition(now + 5000000, now), now + 5000000);
}

TEST(AudioTimeUtilsTest, SanitizeCaptureQpcReplacesFarFutureGarbage) {
    const uint64_t now = 305000000000ULL;
    // The observed failure: a first-packet position hundreds of days in the future.
    const uint64_t garbageFuture = now + 429000000000000ULL;  // ~497 days ahead
    EXPECT_EQ(ce::audio::SanitizeCaptureQpcPosition(garbageFuture, now), now);
    // Just past the future tolerance also gets corrected.
    EXPECT_EQ(ce::audio::SanitizeCaptureQpcPosition(
                  now + ce::audio::kDefaultCaptureQpcFutureToleranceUnits + 1, now),
              now);
}

TEST(AudioTimeUtilsTest, SanitizeCaptureQpcReplacesFarPastGarbage) {
    const uint64_t now = 305000000000ULL;
    // Absurdly stale / near-zero positions are also out-of-domain.
    EXPECT_EQ(ce::audio::SanitizeCaptureQpcPosition(0, now), now);
    EXPECT_EQ(ce::audio::SanitizeCaptureQpcPosition(
                  now - ce::audio::kDefaultCaptureQpcPastToleranceUnits - 1, now),
              now);
    // Within the past tolerance -> preserved.
    EXPECT_EQ(ce::audio::SanitizeCaptureQpcPosition(
                  now - ce::audio::kDefaultCaptureQpcPastToleranceUnits + 1, now),
              now - ce::audio::kDefaultCaptureQpcPastToleranceUnits + 1);
}

TEST(AudioTimeUtilsTest, SanitizeCaptureQpcTrustsValueWhenNoReferenceClock) {
    // nowQpc100ns == 0 means QueryPerformanceCounter is unavailable; trust the value.
    const uint64_t reported = 999999999999999ULL;
    EXPECT_EQ(ce::audio::SanitizeCaptureQpcPosition(reported, 0), reported);
}

