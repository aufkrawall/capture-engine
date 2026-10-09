#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "mediaengine/audio/audio_sync_utils.h"

namespace {

constexpr int64_t kRate = 48000;
constexpr int64_t kPacket = 480;  // 10 ms
constexpr int64_t kSteadySlop = kRate / 1000;
constexpr int64_t kStartupWindow = (kRate * 150) / 1000;
constexpr int64_t kStartupSlop = kRate / 250;
constexpr int64_t kOverlapThreshold = kRate / 200;

ce::audio::PacketTimelineAdjustment Adjust(int64_t start, int64_t written, bool firstPacket) {
    return ce::audio::ComputeStartupAwarePacketTimelineAdjustment(start, written, kSteadySlop, kStartupWindow,
                                                                  kStartupSlop, kOverlapThreshold, firstPacket);
}

struct GapInsertion {
    int64_t packetIndex = 0;
    int64_t outputPosition = 0;  // timeline sample where the silence lands
    int64_t samples = 0;
};

// Replays AudioLoopCommitSource's placement for a source whose packets arrive with perfect cadence and whose
// first packet starts `firstPacketOffset` samples after the recording start.
std::vector<GapInsertion> ReplayPlacement(int64_t firstPacketOffset, bool placeFirstPacketGap, int packets) {
    std::vector<GapInsertion> gaps;
    int64_t written = 0;
    for (int k = 0; k < packets; ++k) {
        const int64_t start = firstPacketOffset + static_cast<int64_t>(k) * kPacket;
        const auto adjustment = Adjust(start, written, placeFirstPacketGap && k == 0);
        if (adjustment.gapSamples > 0) {
            gaps.push_back({k, written, adjustment.gapSamples});
            written += adjustment.gapSamples;
        }
        written += kPacket;
    }
    return gaps;
}

}  // namespace

TEST(StartupFirstPacketGapTest, GapInsideTheStartupSlopIsPlacedWithTheFirstPacket) {
    EXPECT_EQ(Adjust(136, 0, true).gapSamples, 136);
    EXPECT_EQ(Adjust(192, 0, true).gapSamples, 192);
    // Without the first-packet flag the startup slop leaves it unplaced (the original behavior).
    EXPECT_EQ(Adjust(136, 0, false).gapSamples, 0);
}

TEST(StartupFirstPacketGapTest, GapWithinTheSteadySlopStaysUnplaced) {
    EXPECT_EQ(Adjust(0, 0, true).gapSamples, 0);
    EXPECT_EQ(Adjust(kSteadySlop, 0, true).gapSamples, 0);
    EXPECT_EQ(Adjust(kSteadySlop + 1, 0, true).gapSamples, kSteadySlop + 1);
}

TEST(StartupFirstPacketGapTest, LargeFirstPacketGapsKeepTheirExistingHandling) {
    EXPECT_EQ(Adjust(1664, 0, true).gapSamples, 1664);
    EXPECT_EQ(Adjust(1664, 0, false).gapSamples, 1664);
}

TEST(StartupFirstPacketGapTest, OverlapHandlingIsUnchangedByTheFlag) {
    for (bool first : {false, true}) {
        EXPECT_EQ(Adjust(100, 300, first).overlapSamples, 0);    // below the 5 ms startup trim threshold
        EXPECT_EQ(Adjust(100, 600, first).overlapSamples, 500);  // above it
        EXPECT_EQ(Adjust(100, 300, first).gapSamples, 0);
    }
}

TEST(StartupFirstPacketGapTest, FlagHasNoEffectOnceTheStartupWindowIsOver) {
    EXPECT_EQ(Adjust(kStartupWindow + 136, kStartupWindow, true).gapSamples,
              Adjust(kStartupWindow + 136, kStartupWindow, false).gapSamples);
}

TEST(StartupFirstPacketGapTest, ReplayReproducesTheDeferredHardGapAtExactlyOneHundredFiftyMilliseconds) {
    // Logs 20261009_190736 / _181916: first packet placed at +136 / +144 samples, gapTotal 0 until the
    // steady slop applied; the decoded Track 1 dropout began at sample 7200 and lasted ~141 samples.
    const auto legacy = ReplayPlacement(136, /*placeFirstPacketGap=*/false, 40);
    ASSERT_EQ(legacy.size(), 1u);
    EXPECT_EQ(legacy[0].outputPosition, kStartupWindow);  // 150.000 ms into the signal
    EXPECT_EQ(legacy[0].samples, 136);
    EXPECT_GT(legacy[0].packetIndex, 0);
}

TEST(StartupFirstPacketGapTest, ReplayWithTheFixPlacesTheGapBeforeTheSignalAndNothingLater) {
    for (int64_t offset : {int64_t{49}, int64_t{136}, int64_t{144}, int64_t{192}}) {
        const auto gaps = ReplayPlacement(offset, /*placeFirstPacketGap=*/true, 400);
        ASSERT_EQ(gaps.size(), 1u) << "offset=" << offset;
        EXPECT_EQ(gaps[0].packetIndex, 0);
        EXPECT_EQ(gaps[0].outputPosition, 0);  // leading silence, before any signal exists
        EXPECT_EQ(gaps[0].samples, offset);
    }
}

TEST(StartupFirstPacketGapTest, SourcesStartingOnTheGridNeedNoGapAnywhere) {
    EXPECT_TRUE(ReplayPlacement(0, true, 400).empty());
    EXPECT_TRUE(ReplayPlacement(kSteadySlop, true, 400).empty());
}
