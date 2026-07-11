#include <gtest/gtest.h>

#include "../mediaengine/mux_invariants.h"

namespace {

using ce::mux::ComputeDurationDeltaUs;
using ce::mux::ComputeAudioCodecPrimingToleranceUs;
using ce::mux::ComputeAudioMuxRoundingToleranceUs;
using ce::mux::ComputePacketEndUs;
using ce::mux::ChoosePostMuxStreamStartUs;
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

TEST(MuxInvariantTest, AudioCodecPrimingToleranceCoversCodecDelayPlusRounding) {
    EXPECT_EQ(ComputeAudioCodecPrimingToleranceUs(1024, 48000, 21), 21355);
    EXPECT_EQ(ComputeAudioCodecPrimingToleranceUs(312, 48000, 21), 6521);
    EXPECT_EQ(ComputeAudioCodecPrimingToleranceUs(0, 48000, 21), 0);
    EXPECT_EQ(ComputeAudioCodecPrimingToleranceUs(312, 0, 21), 0);
    EXPECT_EQ(ComputeAudioCodecPrimingToleranceUs(312, 48000, -1), 6500);
}

TEST(MuxInvariantTest, PostMuxStartUsesPrimingStartBeforeFirstReadablePacket) {
    EXPECT_EQ(ChoosePostMuxStreamStartUs(-6500, true, 13500, true), -6500);
    EXPECT_EQ(ChoosePostMuxStreamStartUs(0, true, 33333, true), 0);
    EXPECT_EQ(ChoosePostMuxStreamStartUs(0, false, 33333, true), 33333);
    EXPECT_EQ(ChoosePostMuxStreamStartUs(0, false, 0, false), 0);
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
    EXPECT_EQ(stats.maxForwardStartGapUs, 8333);
}

TEST(MuxInvariantTest, PacketTimelineTracksLargestForwardPtsGap) {
    PacketTimelineStats stats;
    ObservePacketTimeline(stats, 0, 8333);
    ObservePacketTimeline(stats, 8333, 8333);
    ObservePacketTimeline(stats, 33333, 8333);
    ObservePacketTimeline(stats, 25000, 8333);  // decode-order backtrack must not inflate the gap

    EXPECT_EQ(stats.maxForwardStartGapUs, 25000);
    EXPECT_EQ(stats.lastStartUs, 33333);
}

TEST(MuxInvariantTest, PacketTimelineDetectsAudioPastMetadataTarget) {
    PacketTimelineStats stats;
    ObservePacketTimeline(stats, 102485333, 85333);

    EXPECT_EQ(ComputePacketEndUs(102485333, 85333), 102570666);
    EXPECT_TRUE(PacketTimelineExceedsTarget(stats, 102183333, 1000));
    EXPECT_FALSE(PacketTimelineExceedsTarget(stats, 102570000, 1000));
}
