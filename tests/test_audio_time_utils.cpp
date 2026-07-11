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
    EXPECT_EQ(ce::audio::SanitizeCaptureQpcPosition(now + ce::audio::kDefaultCaptureQpcFutureToleranceUnits + 1, now),
              now);
}

TEST(AudioTimeUtilsTest, SanitizeCaptureQpcReplacesFarPastGarbage) {
    const uint64_t now = 305000000000ULL;
    // Absurdly stale / near-zero positions are also out-of-domain.
    EXPECT_EQ(ce::audio::SanitizeCaptureQpcPosition(0, now), now);
    EXPECT_EQ(ce::audio::SanitizeCaptureQpcPosition(now - ce::audio::kDefaultCaptureQpcPastToleranceUnits - 1, now),
              now);
    // Within the past tolerance -> preserved.
    EXPECT_EQ(ce::audio::SanitizeCaptureQpcPosition(now - ce::audio::kDefaultCaptureQpcPastToleranceUnits + 1, now),
              now - ce::audio::kDefaultCaptureQpcPastToleranceUnits + 1);
}

TEST(AudioTimeUtilsTest, SanitizeCaptureQpcTrustsValueWhenNoReferenceClock) {
    // nowQpc100ns == 0 means QueryPerformanceCounter is unavailable; trust the value.
    const uint64_t reported = 999999999999999ULL;
    EXPECT_EQ(ce::audio::SanitizeCaptureQpcPosition(reported, 0), reported);
}

TEST(AudioTimeUtilsTest, SanitizeCaptureQpcHandlesCounterValuesNearUint64Max) {
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    const uint64_t now = max - 100;
    // A small future delta must survive even though now + tolerance would wrap.
    EXPECT_EQ(ce::audio::SanitizeCaptureQpcPosition(max - 50, now, 100, 100), max - 50);
    // A delta outside the explicit tolerance is replaced without overflowing.
    EXPECT_EQ(ce::audio::SanitizeCaptureQpcPosition(max, now, 50, 100), now);
}

TEST(AudioTimeUtilsTest, WasapiPacketFrameCountRequiresExactBoundedPacket) {
    EXPECT_TRUE(ce::audio::IsWasapiCapturePacketFrameCountValid(480, 480, 960, 48000));
    EXPECT_TRUE(ce::audio::IsWasapiCapturePacketFrameCountValid(480, 480, 0, 48000));
    EXPECT_FALSE(ce::audio::IsWasapiCapturePacketFrameCountValid(480, 479, 960, 48000));
    EXPECT_FALSE(ce::audio::IsWasapiCapturePacketFrameCountValid(0, 0, 960, 48000));
    EXPECT_FALSE(ce::audio::IsWasapiCapturePacketFrameCountValid(961, 961, 960, 48000));
    EXPECT_FALSE(ce::audio::IsWasapiCapturePacketFrameCountValid(480, 480, 960, 0));
}

TEST(AudioTimeUtilsTest, WasapiPacketFrameCountHasAbsoluteLiveCaptureBoundWithoutTelemetry) {
    constexpr uint32_t sampleRate = 48000;
    const uint32_t maxFrames = ce::audio::MaxWasapiCapturePacketFrames(sampleRate);
    ASSERT_EQ(maxFrames, sampleRate * ce::audio::kMaxWasapiCapturePacketDurationSeconds);
    EXPECT_TRUE(ce::audio::IsWasapiCapturePacketFrameCountValid(maxFrames, maxFrames, 0, sampleRate));
    EXPECT_FALSE(ce::audio::IsWasapiCapturePacketFrameCountValid(maxFrames + 1, maxFrames + 1, 0, sampleRate));
}

TEST(AudioTimeUtilsTest, AudioPacketByteSizeIsChecked) {
    size_t bytes = 0;
    EXPECT_TRUE(ce::audio::TryComputeAudioPacketByteSize(480, 8, &bytes));
    EXPECT_EQ(bytes, 3840u);
    EXPECT_FALSE(ce::audio::TryComputeAudioPacketByteSize(0, 8, &bytes));
    EXPECT_FALSE(ce::audio::TryComputeAudioPacketByteSize(480, 0, &bytes));
    EXPECT_FALSE(ce::audio::TryComputeAudioPacketByteSize(480, 8, nullptr));
}

TEST(AudioTimeUtilsTest, ParseSampleRateAcceptsDefaultSentinelAndEmpty) {
    EXPECT_EQ(ce::audio::ParseSampleRateOr("default", 48000), 48000);
    EXPECT_EQ(ce::audio::ParseSampleRateOr("", 48000), 48000);
    // The fallback is honored, not hardcoded.
    EXPECT_EQ(ce::audio::ParseSampleRateOr("default", 44100), 44100);
}

TEST(AudioTimeUtilsTest, ParseSampleRateParsesValidPositiveIntegers) {
    EXPECT_EQ(ce::audio::ParseSampleRateOr("48000", 48000), 48000);
    EXPECT_EQ(ce::audio::ParseSampleRateOr("44100", 48000), 44100);
    EXPECT_EQ(ce::audio::ParseSampleRateOr("96000", 48000), 96000);
    EXPECT_EQ(ce::audio::ParseSampleRateOr("192000", 48000), 192000);
}

TEST(AudioTimeUtilsTest, ParseSampleRateFallsBackOnMalformedInputWithoutThrowing) {
    // These all threw std::invalid_argument / std::out_of_range under the old
    // std::stoi path and crashed encoder init. They must now fall back safely.
    EXPECT_EQ(ce::audio::ParseSampleRateOr("48kHz", 48000), 48000);
    EXPECT_EQ(ce::audio::ParseSampleRateOr("abc", 48000), 48000);
    EXPECT_EQ(ce::audio::ParseSampleRateOr("-44100", 48000), 48000);
    EXPECT_EQ(ce::audio::ParseSampleRateOr("0", 48000), 48000);
    EXPECT_EQ(ce::audio::ParseSampleRateOr("   ", 48000), 48000);
    EXPECT_EQ(ce::audio::ParseSampleRateOr("99999999999999999999", 48000), 48000);  // out of range
}

TEST(AudioTimeUtilsTest, ParseSampleRateRejectsPartialNumbersStrictly) {
    // A distinct fallback proves the value was rejected outright, not parsed as a
    // bogus leading integer (the old std::stoi would have yielded 48 / 44100).
    EXPECT_EQ(ce::audio::ParseSampleRateOr("48000abc", 44100), 44100);
    EXPECT_EQ(ce::audio::ParseSampleRateOr("44100x", 96000), 96000);
}
