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

namespace {

struct PreStartReplay {
    std::vector<GapInsertion> gaps;
    std::vector<GapInsertion> overlaps;
};

// Replays a source whose first packet starts `lead` samples BEFORE the recording start and whose later packets
// keep a perfect cadence. `equalizationDelay` is the A/V equalization delay the source is placed with.
// Legacy: the first packet bypassed stitching and was written raw. Fixed: its head is trimmed and the rest is
// placed as a first packet that starts at the origin.
PreStartReplay ReplayPreStartFirstPacket(int64_t lead, int64_t equalizationDelay, bool trimHead, int packets) {
    PreStartReplay replay;
    int64_t written = 0;
    if (trimHead) {
        const auto first = Adjust(equalizationDelay, 0, true);
        if (first.gapSamples > 0) {
            replay.gaps.push_back({0, 0, first.gapSamples});
            written += first.gapSamples;
        }
        written += kPacket - lead;
    } else {
        written = kPacket;
    }
    for (int k = 1; k < packets; ++k) {
        const int64_t start = static_cast<int64_t>(k) * kPacket - lead + equalizationDelay;
        const auto adjustment = Adjust(start, written, false);
        if (adjustment.gapSamples > 0) {
            replay.gaps.push_back({k, written, adjustment.gapSamples});
            written += adjustment.gapSamples;
        }
        if (adjustment.overlapSamples > 0) {
            replay.overlaps.push_back({k, written, adjustment.overlapSamples});
            written -= adjustment.overlapSamples;
        }
        written += kPacket;
    }
    return replay;
}

}  // namespace

TEST(PreStartHeadTrimTest, LeadIsConvertedAtTheTargetRate) {
    constexpr int64_t kStart = 5'000'000;
    // 49583 x 100 ns = 4.958 ms = 238 samples at 48 kHz (session 20261009_201855, Fortnite).
    const auto trim = ce::audio::ComputePreStartHeadTrim(kStart - 49583, kStart, 48000, kPacket);
    EXPECT_EQ(trim.trimSamples, 238);
    EXPECT_FALSE(trim.wholePacket);
}

TEST(PreStartHeadTrimTest, PacketsAtOrAfterTheStartAreUntouched) {
    constexpr int64_t kStart = 5'000'000;
    EXPECT_EQ(ce::audio::ComputePreStartHeadTrim(kStart, kStart, 48000, kPacket).trimSamples, 0);
    EXPECT_EQ(ce::audio::ComputePreStartHeadTrim(kStart + 1, kStart, 48000, kPacket).trimSamples, 0);
    EXPECT_EQ(ce::audio::ComputePreStartHeadTrim(kStart + 40000, kStart, 48000, kPacket).trimSamples, 0);
}

TEST(PreStartHeadTrimTest, UnusableInputsTrimNothing) {
    EXPECT_EQ(ce::audio::ComputePreStartHeadTrim(100, 0, 48000, kPacket).trimSamples, 0);
    EXPECT_EQ(ce::audio::ComputePreStartHeadTrim(100, -5, 48000, kPacket).trimSamples, 0);
    EXPECT_EQ(ce::audio::ComputePreStartHeadTrim(100, 5'000'000, 0, kPacket).trimSamples, 0);
    EXPECT_EQ(ce::audio::ComputePreStartHeadTrim(100, 5'000'000, 48000, 0).trimSamples, 0);
}

TEST(PreStartHeadTrimTest, LeadReachingTheWholePacketDropsItAndKeepsTheTimelineUnstarted) {
    constexpr int64_t kStart = 5'000'000;
    // A 2.5 ms packet that started 4.958 ms before the start lies entirely before it.
    const auto whole = ce::audio::ComputePreStartHeadTrim(kStart - 49583, kStart, 48000, 120);
    EXPECT_EQ(whole.trimSamples, 120);
    EXPECT_TRUE(whole.wholePacket);
    // Exactly one packet length of lead is also entirely before the start.
    const auto exact = ce::audio::ComputePreStartHeadTrim(kStart - 100000, kStart, 48000, 480);
    EXPECT_EQ(exact.trimSamples, 480);
    EXPECT_TRUE(exact.wholePacket);
}

TEST(PreStartFirstPacketReplayTest, LegacyRawWriteDeletedTheLeadAsOneSpliceAtExactlyOneHundredFiftyMilliseconds) {
    // Session 20261009_201855: Fortnite's first packet placed at +242 with the write cursor at 480 (overlap 238
    // samples, below the 5 ms startup trim threshold); overlapTotal 238 appeared once and the decoded Track 1 / 2
    // alignment stepped by 238 samples at sample 7200 with the strongest click of the first 1.5 s.
    const auto legacy = ReplayPreStartFirstPacket(238, 0, /*trimHead=*/false, 400);
    EXPECT_TRUE(legacy.gaps.empty());
    ASSERT_EQ(legacy.overlaps.size(), 1u);
    EXPECT_EQ(legacy.overlaps[0].outputPosition, kStartupWindow);
    EXPECT_EQ(legacy.overlaps[0].samples, 238);
    EXPECT_EQ(legacy.overlaps[0].packetIndex, 15);
}

TEST(PreStartFirstPacketReplayTest, TrimmingTheHeadLeavesNothingToDeleteLater) {
    for (int64_t lead : {int64_t{49}, int64_t{109}, int64_t{238}, int64_t{288}}) {
        const auto fixed = ReplayPreStartFirstPacket(lead, 0, /*trimHead=*/true, 400);
        EXPECT_TRUE(fixed.gaps.empty()) << "lead=" << lead;
        EXPECT_TRUE(fixed.overlaps.empty()) << "lead=" << lead;
    }
}

TEST(PreStartFirstPacketReplayTest, SmallLeadsWithinTheSteadySlopNeverProducedAnEvent) {
    const auto legacy = ReplayPreStartFirstPacket(40, 0, /*trimHead=*/false, 400);
    EXPECT_TRUE(legacy.gaps.empty());
    EXPECT_TRUE(legacy.overlaps.empty());
}

TEST(PreStartFirstPacketReplayTest, EqualizedMicrophoneGetsItsDelayAsLeadingSilenceNotInsideTheSignal) {
    // The same log: microphone src=1, equalization delay 1500 samples, first packet 109 samples before the start.
    // The raw first packet left a 1391-sample silence between its 480 samples and the second packet
    // ("Packet timeline adjust src=1 gap=1391 written=1871").
    const auto legacy = ReplayPreStartFirstPacket(109, 1500, /*trimHead=*/false, 400);
    ASSERT_EQ(legacy.gaps.size(), 1u);
    EXPECT_EQ(legacy.gaps[0].samples, 1391);
    EXPECT_EQ(legacy.gaps[0].outputPosition, kPacket);
    EXPECT_TRUE(legacy.overlaps.empty());

    const auto fixed = ReplayPreStartFirstPacket(109, 1500, /*trimHead=*/true, 400);
    ASSERT_EQ(fixed.gaps.size(), 1u);
    EXPECT_EQ(fixed.gaps[0].packetIndex, 0);
    EXPECT_EQ(fixed.gaps[0].outputPosition, 0);
    EXPECT_EQ(fixed.gaps[0].samples, 1500);
    EXPECT_TRUE(fixed.overlaps.empty());
}
