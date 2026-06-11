#include <gtest/gtest.h>

#include "../mediaengine/mux_invariants.h"

namespace {

using ce::mux::ComputeDurationDeltaUs;
using ce::mux::ComputeAudioMuxRoundingToleranceUs;
using ce::mux::ComputePacketEndUs;
using ce::mux::HeaderValidationIssue;
using ce::mux::HeaderValidationIssueToString;
using ce::mux::IsDurationWithinToleranceUs;
using ce::mux::ObservePacketTimeline;
using ce::mux::PacketTimelineExceedsTarget;
using ce::mux::PacketTimelineStats;
using ce::mux::ValidateStreamForHeader;

}  // namespace

TEST(MuxInvariantTest, HeaderValidationAcceptsStreamsWithCodecParamsAndTimeBase) {
    EXPECT_EQ(ValidateStreamForHeader(true, true, 1, 1000), HeaderValidationIssue::kNone);
}

TEST(MuxInvariantTest, HeaderValidationRejectsMissingStream) {
    EXPECT_EQ(ValidateStreamForHeader(false, false, 0, 0), HeaderValidationIssue::kMissingStream);
    EXPECT_STREQ(HeaderValidationIssueToString(HeaderValidationIssue::kMissingStream), "missing stream");
}

TEST(MuxInvariantTest, HeaderValidationRejectsMissingCodecParameters) {
    EXPECT_EQ(ValidateStreamForHeader(true, false, 1, 1000), HeaderValidationIssue::kMissingCodecParams);
    EXPECT_STREQ(HeaderValidationIssueToString(HeaderValidationIssue::kMissingCodecParams), "missing codec parameters");
}

TEST(MuxInvariantTest, HeaderValidationRejectsInvalidTimeBase) {
    EXPECT_EQ(ValidateStreamForHeader(true, true, 0, 1000), HeaderValidationIssue::kInvalidTimeBase);
    EXPECT_EQ(ValidateStreamForHeader(true, true, 1, 0), HeaderValidationIssue::kInvalidTimeBase);
    EXPECT_STREQ(HeaderValidationIssueToString(HeaderValidationIssue::kInvalidTimeBase), "invalid time base");
}

TEST(MuxInvariantTest, DurationDeltaAndToleranceAreAbsolute) {
    EXPECT_EQ(ComputeDurationDeltaUs(1000, 900), 100);
    EXPECT_EQ(ComputeDurationDeltaUs(900, 1000), 100);
    EXPECT_TRUE(IsDurationWithinToleranceUs(1000, 1001, 1));
    EXPECT_FALSE(IsDurationWithinToleranceUs(1000, 1002, 1));
    EXPECT_TRUE(IsDurationWithinToleranceUs(1000, 1000, -1));
    EXPECT_FALSE(IsDurationWithinToleranceUs(1000, 1002, -1));
}

TEST(MuxInvariantTest, AudioMuxRoundingToleranceCoversOneSampleOrTimebaseTick) {
    EXPECT_EQ(ComputeAudioMuxRoundingToleranceUs(48000, 1, 1000000), 21);
    EXPECT_EQ(ComputeAudioMuxRoundingToleranceUs(0, 1, 1000000), 1);
    EXPECT_EQ(ComputeAudioMuxRoundingToleranceUs(48000, 1, 1000), 1000);
    EXPECT_EQ(ComputeAudioMuxRoundingToleranceUs(0, 0, 0), 1);
}

TEST(MuxInvariantTest, PacketTimelineTracksActualPacketEnd) {
    PacketTimelineStats stats;
    ObservePacketTimeline(stats, 0, 8333);
    ObservePacketTimeline(stats, 8333, 8334);

    EXPECT_TRUE(stats.seen);
    EXPECT_EQ(stats.packetCount, 2u);
    EXPECT_EQ(stats.firstStartUs, 0);
    EXPECT_EQ(stats.lastStartUs, 8333);
    EXPECT_EQ(stats.lastEndUs, 16667);
}

TEST(MuxInvariantTest, PacketTimelineDetectsAudioPastMetadataTarget) {
    PacketTimelineStats stats;
    ObservePacketTimeline(stats, 102485333, 85333);

    EXPECT_EQ(ComputePacketEndUs(102485333, 85333), 102570666);
    EXPECT_TRUE(PacketTimelineExceedsTarget(stats, 102183333, 1000));
    EXPECT_FALSE(PacketTimelineExceedsTarget(stats, 102570000, 1000));
}
