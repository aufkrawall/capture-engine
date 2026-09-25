#include <gtest/gtest.h>

#include <cmath>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

#include "../mediaengine/audio_sync_utils.h"
#include "source_fragment_reader.h"

namespace {

// Applies the track ramp to a constant signal in CFR-sized pulls and returns the
// concatenated per-sample gain.
std::vector<float> RampGainsInPulls(int64_t fadeSamples, size_t pullFrames, size_t totalFrames, int channels) {
    std::vector<float> gains;
    int64_t remaining = fadeSamples;
    for (size_t done = 0; done < totalFrames; done += pullFrames) {
        std::vector<float> pull(pullFrames * static_cast<size_t>(channels), 1.0f);
        ce::audio::ApplyTrackFadeIn(pull.data(), pullFrames, channels, remaining, fadeSamples);
        for (size_t s = 0; s < pullFrames; ++s) {
            for (int ch = 1; ch < channels; ++ch) {
                EXPECT_EQ(pull[s * channels + ch], pull[s * channels]);
            }
            gains.push_back(pull[s * static_cast<size_t>(channels)]);
        }
    }
    return gains;
}

}  // namespace

// Regression: the resume fade was applied to the first pull only. One settled CFR
// pull at 120 fps is 400 samples, so the 2400-sample ramp stopped at 0.17 and the
// gain jumped straight to 1.0 at the next pull boundary - an audible step.
TEST(AudioFadePolicyTest, TrackFadeRampIsContinuousAcrossCfrPulls) {
    constexpr int64_t kFade = 2400;
    const std::vector<float> gains = RampGainsInPulls(kFade, 400, 4000, 2);
    EXPECT_EQ(gains.front(), 0.0f);
    for (size_t s = 1; s < gains.size(); ++s) {
        const float step = gains[s] - gains[s - 1];
        EXPECT_GE(step, 0.0f) << s;
        EXPECT_LE(step, 1.0f / static_cast<float>(kFade) + 1e-6f) << "gain step at sample " << s;
    }
    EXPECT_FLOAT_EQ(gains[static_cast<size_t>(kFade) - 1], static_cast<float>(kFade - 1) / kFade);
    for (size_t s = static_cast<size_t>(kFade); s < gains.size(); ++s) {
        EXPECT_EQ(gains[s], 1.0f);
    }
}

TEST(AudioFadePolicyTest, TrackFadeMatchesSingleChunkRampAndIgnoresIdleState) {
    constexpr int64_t kFade = 1200;
    const std::vector<float> chunked = RampGainsInPulls(kFade, 240, 1440, 1);
    const std::vector<float> whole = RampGainsInPulls(kFade, 1440, 1440, 1);
    ASSERT_EQ(chunked.size(), whole.size());
    for (size_t s = 0; s < whole.size(); ++s) {
        EXPECT_EQ(chunked[s], whole[s]) << s;
    }
    std::vector<float> untouched(8, 0.7f);
    int64_t remaining = 0;
    ce::audio::ApplyTrackFadeIn(untouched.data(), 4, 2, remaining, kFade);
    for (float sample : untouched) {
        EXPECT_EQ(sample, 0.7f);
    }
    remaining = -5;
    ce::audio::ApplyTrackFadeIn(untouched.data(), 4, 2, remaining, kFade);
    EXPECT_EQ(remaining, 0);
}

// Regression (VFR backlog cut): the seam after a post-resample trim sits at the
// FRONT of the retained backlog. It must start at the last emitted frame and
// reach the retained signal after the fade, instead of fading appended samples.
TEST(AudioFadePolicyTest, AnchoredCrossfadeStartsAtLastEmittedFrame) {
    std::deque<float> backlog;
    for (int s = 0; s < 20; ++s) {
        backlog.push_back(-0.8f);
        backlog.push_back(0.4f);
    }
    const std::vector<float> anchor = {0.6f, -0.2f};
    ce::audio::ApplyAnchoredCrossfadeIn(backlog, anchor, 2, 10);
    EXPECT_NEAR(backlog[0], 0.6f + (-0.8f - 0.6f) * 0.1f, 1e-6f);
    EXPECT_NEAR(backlog[1], -0.2f + (0.4f + 0.2f) * 0.1f, 1e-6f);
    float previous = backlog[0];
    for (size_t s = 1; s < 10; ++s) {
        EXPECT_LT(backlog[s * 2], previous) << s;
        previous = backlog[s * 2];
    }
    EXPECT_FLOAT_EQ(backlog[9 * 2], -0.8f);
    for (size_t s = 10; s < 20; ++s) {
        EXPECT_EQ(backlog[s * 2], -0.8f);
        EXPECT_EQ(backlog[s * 2 + 1], 0.4f);
    }
    std::deque<float> fromSilence(4, 1.0f);
    ce::audio::ApplyAnchoredCrossfadeIn(fromSilence, {}, 2, 4);
    EXPECT_FLOAT_EQ(fromSilence[0], 0.25f);
    EXPECT_FLOAT_EQ(fromSilence[2], 0.5f);
}

TEST(AudioFadePolicyTest, SteadyPlacementStatsReportNetDeviceDriftInPpm) {
    ce::audio::SteadyPlacementCorrectionStats stats;
    ce::audio::ObserveSteadyPlacementCorrection(stats, 49, 0, false);  // startup window: ignored
    EXPECT_EQ(stats.gapEvents, 0u);
    for (int i = 0; i < 10; ++i) {
        ce::audio::ObserveSteadyPlacementCorrection(stats, 49, 0, true);
    }
    ce::audio::ObserveSteadyPlacementCorrection(stats, 0, 0, true);
    EXPECT_EQ(stats.gapEvents, 10u);
    EXPECT_EQ(stats.gapSamples, 490u);
    // 490 samples inserted over 100 s at 48 kHz: device ~102 ppm slower than QPC.
    EXPECT_NEAR(ce::audio::ComputePlacementClockMismatchPpm(stats, 4800000), 102.08, 0.01);
    ce::audio::ObserveSteadyPlacementCorrection(stats, 0, 980, true);
    EXPECT_EQ(stats.overlapEvents, 1u);
    EXPECT_LT(ce::audio::ComputePlacementClockMismatchPpm(stats, 4800000), 0.0);
    EXPECT_EQ(ce::audio::ComputePlacementClockMismatchPpm(stats, 0), 0.0);
}

TEST(AudioFadePolicyTest, PullPathCarriesTrackFadesAcrossPulls) {
    const std::string source = ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "mediaengine" /
                                                                   "mediaengine_audio_pull_sync.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("ce::audio::ApplyTrackFadeIn(mixBuffer.data()"), std::string::npos);
    EXPECT_NE(source.find("trackFadeInSamplesRemaining[track]"), std::string::npos);
    EXPECT_NE(source.find("fadeInAppliedThisPull"), std::string::npos);
    EXPECT_EQ(source.find("fadeStart = applyTransitionFade ? 0 : trackPos"), std::string::npos);
}
