#include <gtest/gtest.h>

#include <array>
#include <cmath>

#include "../testapp/av_sync_stimulus.h"

namespace avs = testapp::avsync;

TEST(AvSyncStimulusTest, EventScheduleMapsTimesToPaletteAndFrequency) {
    EXPECT_EQ(avs::EventIndexAt(-0.001), -1);
    EXPECT_EQ(avs::EventIndexAt(0.0), 0);
    EXPECT_EQ(avs::EventIndexAt(0.999), 0);
    EXPECT_EQ(avs::EventIndexAt(1.0), 1);
    EXPECT_EQ(avs::EventIndexAt(16.0), 16);

    const auto first = avs::StateAt(0.25);
    EXPECT_EQ(first.eventIndex, 0);
    EXPECT_EQ(first.paletteIndex, 0);
    EXPECT_DOUBLE_EQ(first.eventStartSeconds, 0.0);
    EXPECT_DOUBLE_EQ(first.eventEndSeconds, 1.0);
    EXPECT_DOUBLE_EQ(first.frequencyHz, avs::kFrequenciesHz[0]);
    EXPECT_EQ(first.color.r, avs::kPalette[0].r);

    const auto wrapped = avs::StateAt(16.25);
    EXPECT_EQ(wrapped.eventIndex, 16);
    EXPECT_EQ(wrapped.paletteIndex, 0);
    EXPECT_DOUBLE_EQ(wrapped.frequencyHz, avs::kFrequenciesHz[0]);
}

TEST(AvSyncStimulusTest, FrameMarkerRoundTripsLowSixteenBits) {
    for (uint64_t frameId : {0ull, 1ull, 0x1234ull, 0xffffull, 0x10000ull, 0x12345ull}) {
        const uint16_t marker = avs::EncodeFrameMarker(frameId);
        std::array<bool, avs::kFrameMarkerBits> bits = {};
        for (int bit = 0; bit < avs::kFrameMarkerBits; ++bit) {
            bits[bit] = avs::FrameMarkerBit(marker, bit);
        }
        EXPECT_EQ(avs::DecodeFrameMarkerBits(bits.data(), static_cast<int>(bits.size())), marker);
    }
}

TEST(AvSyncStimulusTest, FrameMarkerRedundancyDetectsChecksumAndParity) {
    const uint16_t marker = avs::EncodeFrameMarker(0x1234);
    const uint8_t checksum = avs::FrameMarkerChecksum(marker, 5);
    unsigned parityValue = static_cast<unsigned>(marker ^ checksum);
    bool expectedParity = false;
    while (parityValue) {
        expectedParity = !expectedParity;
        parityValue &= parityValue - 1u;
    }
    EXPECT_EQ(checksum, static_cast<uint8_t>(0x12 ^ 0x34 ^ 5));
    EXPECT_EQ(avs::FrameMarkerParity(marker, 5), expectedParity);
    EXPECT_NE(avs::FrameMarkerChecksum(marker, 5), avs::FrameMarkerChecksum(marker ^ 0x0001u, 5));
}

TEST(AvSyncStimulusTest, SourceStallSpecParsesStartAndDuration) {
    avs::SourceStallSpec spec;
    EXPECT_TRUE(avs::ParseSourceStallSpec("8.0:300", &spec));
    EXPECT_TRUE(spec.valid);
    EXPECT_DOUBLE_EQ(spec.startSeconds, 8.0);
    EXPECT_DOUBLE_EQ(spec.durationSeconds, 0.3);
    EXPECT_DOUBLE_EQ(spec.EndSeconds(), 8.3);

    EXPECT_FALSE(avs::ParseSourceStallSpec("8.0", &spec));
    EXPECT_FALSE(avs::ParseSourceStallSpec("-1:300", &spec));
    EXPECT_FALSE(avs::ParseSourceStallSpec("1:0", &spec));
}

TEST(AvSyncStimulusTest, AudioBufferClampKeepsDeterministicLowLatencyBounds) {
    EXPECT_EQ(avs::ClampAudioBufferMs(1), avs::kMinAudioBufferMs);
    EXPECT_EQ(avs::ClampAudioBufferMs(avs::kDefaultAudioBufferMs), avs::kDefaultAudioBufferMs);
    EXPECT_EQ(avs::ClampAudioBufferMs(1000), avs::kMaxAudioBufferMs);
}

TEST(AvSyncStimulusTest, AudioLeadClampKeepsStimulusCompensationBounded) {
    EXPECT_DOUBLE_EQ(avs::ClampAudioLeadMs(-1000.0), avs::kMinAudioLeadMs);
    EXPECT_DOUBLE_EQ(avs::ClampAudioLeadMs(avs::kDefaultAudioLeadMs), avs::kDefaultAudioLeadMs);
    EXPECT_DOUBLE_EQ(avs::ClampAudioLeadMs(1000.0), avs::kMaxAudioLeadMs);
}

TEST(AvSyncStimulusTest, AnalysisStartClampStaysInsideRunDuration) {
    EXPECT_DOUBLE_EQ(avs::ClampAnalysisStartSeconds(-1.0, 10.0), 0.0);
    EXPECT_DOUBLE_EQ(avs::ClampAnalysisStartSeconds(2.0, 10.0), 2.0);
    EXPECT_DOUBLE_EQ(avs::ClampAnalysisStartSeconds(30.0, 10.0), 10.0);
}

TEST(AvSyncStimulusTest, DxgiTearingRequiresExplicitRequestSupportAndNoVsync) {
    EXPECT_FALSE(avs::ShouldUseDxgiTearing(false, true, false));
    EXPECT_FALSE(avs::ShouldUseDxgiTearing(true, false, false));
    EXPECT_FALSE(avs::ShouldUseDxgiTearing(true, true, true));
    EXPECT_TRUE(avs::ShouldUseDxgiTearing(true, true, false));
}

TEST(AvSyncStimulusTest, PaletteNearestRecoversNoisyMarkerColors) {
    for (size_t index = 0; index < avs::kPalette.size(); ++index) {
        const auto color = avs::kPalette[index];
        const uint8_t r = static_cast<uint8_t>(std::min<int>(255, color.r + 3));
        const uint8_t g = static_cast<uint8_t>(std::max<int>(0, color.g - 2));
        const uint8_t b = static_cast<uint8_t>(std::min<int>(255, color.b + 1));
        EXPECT_EQ(avs::NearestPaletteIndex(r, g, b), static_cast<int>(index));
    }
}

TEST(AvSyncStimulusTest, AudioTransitionsAreAtZeroCrossings) {
    for (int eventIndex = 0; eventIndex < 8; ++eventIndex) {
        const double boundarySeconds = static_cast<double>(eventIndex);
        EXPECT_NEAR(avs::AudioSampleAt(boundarySeconds, 0, 2), 0.0f, 0.0001f);
        EXPECT_NEAR(avs::AudioSampleAt(boundarySeconds, 1, 2), 0.0f, 0.0001f);
    }
}

TEST(AvSyncStimulusTest, SmoothLanePositionWrapsDeterministically) {
    EXPECT_DOUBLE_EQ(avs::SmoothLanePosition(-0.1), 0.0);
    EXPECT_NEAR(avs::SmoothLanePosition(0.0), 0.0, 0.000001);
    EXPECT_NEAR(avs::SmoothLanePosition(1.0), 0.25, 0.000001);
    EXPECT_NEAR(avs::SmoothLanePosition(4.0), 0.0, 0.000001);
    EXPECT_NEAR(avs::SmoothLanePosition(5.0), 0.25, 0.000001);
    EXPECT_NEAR(avs::ExpectedMotionPosition(5.0), avs::SmoothLanePosition(5.0), 0.000001);
}

TEST(AvSyncStimulusTest, FastLanePositionWrapsEverySecond) {
    EXPECT_DOUBLE_EQ(avs::FastLanePosition(-0.1), 0.0);
    EXPECT_NEAR(avs::FastLanePosition(0.0), 0.0, 0.000001);
    EXPECT_NEAR(avs::FastLanePosition(0.25), 0.25, 0.000001);
    EXPECT_NEAR(avs::FastLanePosition(0.75), 0.75, 0.000001);
    EXPECT_NEAR(avs::FastLanePosition(1.0), 0.0, 0.000001);
    EXPECT_NEAR(avs::FastLanePosition(1.25), 0.25, 0.000001);
}
