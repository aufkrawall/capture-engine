#include <gtest/gtest.h>

#include "../mediaengine/matroska_timing.h"
#include "../mediaengine/mux_invariants.h"

namespace {

using ce::mux::ChoosePostMuxStreamStartUs;
using ce::mux::ComputeAudioMuxRoundingToleranceUs;
using ce::mux::ComputeAudioPaddingDurationUs;
using ce::mux::ComputeCfrAudioLatticeExtensionFrames;
using ce::mux::ComputeCfrAudioLatticeFrameQuantum;
using ce::mux::ComputeDecodedAudioDurationUs;
using ce::mux::ComputeDurationDeltaUs;
using ce::mux::ComputePacketEndUs;
using ce::mux::HeaderValidationIssue;
using ce::mux::HeaderValidationIssueToString;
using ce::mux::IsDurationWithinToleranceUs;
using ce::mux::ObservePacketTimeline;
using ce::mux::PacketTimelineExceedsTarget;
using ce::mux::PacketTimelineStats;
using ce::mux::ValidateStreamForHeader;
using ce::mux::SelectVideoOutputDisposition;
using ce::mux::VideoOutputDisposition;

}  // namespace

TEST(MuxInvariantTest, BundledMatroskaMuxerRequiresAndReportsMicrosecondPrecision) {
    AVFormatContext* formatContext = nullptr;
    ASSERT_GE(avformat_alloc_output_context2(&formatContext, nullptr, "matroska", nullptr), 0);
    ASSERT_NE(formatContext, nullptr);
    EXPECT_TRUE(ce::media::IsMatroskaMuxer(formatContext));
    EXPECT_TRUE(ce::media::RequireMicrosecondMatroskaTimestampPrecision(formatContext));
    avformat_free_context(formatContext);
}

TEST(MuxInvariantTest, VideoOutputPublishesOnlyCommittedVideo) {
    EXPECT_EQ(SelectVideoOutputDisposition(false, 0, 0, 16667, 1), VideoOutputDisposition::kPublish);
    EXPECT_EQ(SelectVideoOutputDisposition(true, 0, 0, 16667, 1),
              VideoOutputDisposition::kPublishAfterCancel);
    EXPECT_EQ(SelectVideoOutputDisposition(false, 0, 0, 0, 1), VideoOutputDisposition::kPublishAfterFinalizeFailure);
    EXPECT_EQ(SelectVideoOutputDisposition(false, 0, 0, 16667, 0), VideoOutputDisposition::kDiscardNoVideo);
    EXPECT_TRUE(ce::mux::ShouldPublishVideoOutput(VideoOutputDisposition::kPublish));
    EXPECT_FALSE(ce::mux::ShouldPublishVideoOutput(VideoOutputDisposition::kDiscardCancelled));
    EXPECT_FALSE(ce::mux::ShouldPublishVideoOutput(VideoOutputDisposition::kDiscardNoVideo));
}

// Regression: a cancellation after live output (and the one-packet HDR metadata
// failure that used to mark the whole session cancelled) deleted recordings
// with hours of committed packets. Committed video survives every exit path;
// only a recording with exactly zero committed video packets is deleted.
TEST(MuxInvariantTest, CommittedVideoSurvivesEveryNonEmptyExitPath) {
    constexpr int64_t kTwoHoursUs = 2LL * 60 * 60 * 1000000;
    EXPECT_EQ(SelectVideoOutputDisposition(true, 0, 0, kTwoHoursUs, 432000),
              VideoOutputDisposition::kPublishAfterCancel);
    EXPECT_EQ(SelectVideoOutputDisposition(true, -1, -1, kTwoHoursUs, 432000),
              VideoOutputDisposition::kPublishAfterCancel);
    EXPECT_EQ(SelectVideoOutputDisposition(false, 0, 0, 0, 432000),
              VideoOutputDisposition::kPublishAfterFinalizeFailure);
    EXPECT_TRUE(ce::mux::ShouldPublishVideoOutput(VideoOutputDisposition::kPublishAfterCancel));
    EXPECT_STREQ(ce::mux::VideoOutputDispositionToString(VideoOutputDisposition::kPublishAfterCancel),
                 "publish-after-cancel");
}

// Regression: av_write_trailer returns the AVIOContext's sticky error, so one
// transient write failure anywhere in a long recording, or a full disk while the
// final index is written, used to delete the whole recording.
TEST(MuxInvariantTest, FinalizeFailureKeepsCommittedVideo) {
    constexpr int64_t kTwoHoursUs = 2LL * 60 * 60 * 1000000;
    EXPECT_EQ(SelectVideoOutputDisposition(false, -28, 0, kTwoHoursUs, 432000),
              VideoOutputDisposition::kPublishAfterFinalizeFailure);
    EXPECT_EQ(SelectVideoOutputDisposition(false, 0, -5, 16667, 1),
              VideoOutputDisposition::kPublishAfterFinalizeFailure);
    EXPECT_TRUE(ce::mux::ShouldPublishVideoOutput(VideoOutputDisposition::kPublishAfterFinalizeFailure));
    EXPECT_STREQ(ce::mux::VideoOutputDispositionToString(VideoOutputDisposition::kPublishAfterFinalizeFailure),
                 "publish-after-finalize-failure");

    // Without committed video there is nothing to save, finalize failure or not;
    // with committed video a cancellation keeps the file (CommittedVideoSurvives
    // EveryNonEmptyExitPath pins the full matrix).
    EXPECT_EQ(SelectVideoOutputDisposition(false, -1, -1, 16667, 0), VideoOutputDisposition::kDiscardNoVideo);
    EXPECT_EQ(SelectVideoOutputDisposition(true, -1, -1, kTwoHoursUs, 432000),
              VideoOutputDisposition::kPublishAfterCancel);
}

TEST(MuxInvariantTest, AudioOnlyFinalizeFailureKeepsCommittedPackets) {
    EXPECT_TRUE(ce::mux::ShouldPublishAudioOnlyOutput(true, true, 0));
    EXPECT_TRUE(ce::mux::ShouldPublishAudioOnlyOutput(false, true, 1200));
    EXPECT_TRUE(ce::mux::ShouldPublishAudioOnlyOutput(true, false, 1200));
    EXPECT_FALSE(ce::mux::ShouldPublishAudioOnlyOutput(false, true, 0));
    EXPECT_FALSE(ce::mux::ShouldPublishAudioOnlyOutput(true, false, 0));
}

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

TEST(MuxInvariantTest, AacPacketCoverageExcludesCodecPrimingAndTerminalPadding) {
    EXPECT_EQ(ComputeAudioPaddingDurationUs(1024, 48000), 21333);
    EXPECT_EQ(ComputeDecodedAudioDurationUs(16938666, 48000, 1024, 32), 16916666);
    EXPECT_EQ(ComputeDecodedAudioDurationUs(33770666, 48000, 1024, 768), 33733333);
    EXPECT_EQ(ComputeDecodedAudioDurationUs(1000, 0, 1024, 32), 1000);
}

TEST(MuxInvariantTest, CfrEndpointExtendsOnlyToTheMinimumCommonAudioLattice) {
    EXPECT_EQ(ComputeCfrAudioLatticeFrameQuantum(120, {48000}), 1);
    EXPECT_EQ(ComputeCfrAudioLatticeFrameQuantum(144, {48000}), 3);
    EXPECT_EQ(ComputeCfrAudioLatticeFrameQuantum(144, {44100, 48000}), 12);
    EXPECT_EQ(ComputeCfrAudioLatticeExtensionFrames(100, 3), 2);
    EXPECT_EQ(ComputeCfrAudioLatticeExtensionFrames(102, 3), 0);
    EXPECT_EQ(ComputeCfrAudioLatticeExtensionFrames(109, 12), 11);
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

TEST(MuxInvariantTest, PacketTimelineExcludesTerminalDiscardFromDecodedEndpoint) {
    PacketTimelineStats stats;
    ObservePacketTimeline(stats, 16896000, 21333, 667);

    EXPECT_EQ(stats.lastEndUs, 16917333);
    EXPECT_EQ(stats.lastDecodedEndUs, 16916666);
}

TEST(MuxInvariantTest, OutputIoDeadlineExpiresOnlyWhileArmed) {
    EXPECT_FALSE(ce::mux::IsOutputIoDeadlineExpired(0, 0));
    EXPECT_FALSE(ce::mux::IsOutputIoDeadlineExpired(0, 123456));
    EXPECT_FALSE(ce::mux::IsOutputIoDeadlineExpired(5000, 4999));
    EXPECT_TRUE(ce::mux::IsOutputIoDeadlineExpired(5000, 5000));
    EXPECT_TRUE(ce::mux::IsOutputIoDeadlineExpired(5000, 90000));
}

// A finalize Stop() stopped waiting for has an unknown outcome: it must never
// be reported as a clean save.
TEST(MuxInvariantTest, UnfinishedFinalizeIsReportedDegraded) {
    EXPECT_FALSE(ce::mux::IsFinalizedOutputDegraded(0, 0, false, false));
    EXPECT_TRUE(ce::mux::IsFinalizedOutputDegraded(0, 0, false, true));
    EXPECT_TRUE(ce::mux::IsFinalizedOutputDegraded(1, 0, false, false));
    EXPECT_TRUE(ce::mux::IsFinalizedOutputDegraded(0, 1, false, false));
    EXPECT_TRUE(ce::mux::IsFinalizedOutputDegraded(0, 0, true, false));
}
