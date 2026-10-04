#include <gtest/gtest.h>

#include <vector>
#include <string>

#include "mediaengine/engine/submission_transaction.h"
#include "mediaengine/video/video_encoder.h"

struct VideoEncoderTestAccess {
    static void SetPreviousDeferral(VideoEncoder& encoder) {
        encoder.lastFrameDeferred.store(true, std::memory_order_relaxed);
    }
};

namespace {
using namespace ce::media;

struct SubmissionProbe {
    bool accepted = false;
    bool deferred = false;
    bool first = true;
    int anchors = 0;
    int audioPulls = 0;
    int deferredQueries = 0;
    int64_t videoUs = -1;
    std::vector<std::string> events;

    FrameSubmissionResultV1 Submit(bool candidate, CommittedTimeline timeline, int64_t scheduledUs,
                                    int64_t outputUs) {
        return detail::SubmitAndCommit(candidate, [&] {
            events.emplace_back("encode");
            return accepted;
        }, [&] {
            ++deferredQueries;
            return deferred;
        }, [&] {
            events.emplace_back("commit");
            const bool commitsFirst = candidate && first;
            if (commitsFirst) { ++anchors; first = false; }
            videoUs = timeline == CommittedTimeline::ScreenGrabScheduled ? scheduledUs : outputUs;
            ++audioPulls;
            return AcceptedSubmission(candidate ? SubmissionOutput::FreshSource : SubmissionOutput::CachedRepeat,
                                      commitsFirst, timeline, videoUs, outputUs);
        });
    }
};
}  // namespace

TEST(SubmissionTransactionTest, RejectedAndDeferredCandidatesCannotCommitAnchorOrTimeline) {
    SubmissionProbe probe;
    auto rejected = probe.Submit(true, CommittedTimeline::InjectOutput, 50000, 16667);
    EXPECT_EQ(rejected.status, SubmissionStatus::Rejected);
    EXPECT_EQ(rejected.output, SubmissionOutput::None);
    EXPECT_EQ(rejected.source, SourceDisposition::ReleaseEligible);
    EXPECT_EQ(rejected.videoTimelineUs, -1);
    EXPECT_EQ(probe.anchors, 0);
    EXPECT_EQ(probe.audioPulls, 0);
    probe.deferred = true;
    auto deferred = probe.Submit(true, CommittedTimeline::InjectOutput, 50000, 16667);
    EXPECT_EQ(deferred.status, SubmissionStatus::Deferred);
    EXPECT_EQ(deferred.source, SourceDisposition::RetainForRetry);
    EXPECT_EQ(deferred.firstOutputCommitted, 0u);
    EXPECT_EQ(probe.videoUs, -1);
    EXPECT_EQ(probe.anchors, 0);
    EXPECT_EQ(probe.audioPulls, 0);
    probe.accepted = true;
    auto accepted = probe.Submit(true, CommittedTimeline::InjectOutput, 50000, 16667);
    EXPECT_TRUE(accepted.Accepted());
    EXPECT_EQ(accepted.firstOutputCommitted, 1u);
    EXPECT_EQ(accepted.videoTimelineUs, 16667);
    EXPECT_EQ(accepted.audioTimelineUs, 16667);
    EXPECT_EQ(accepted.source, SourceDisposition::ReleaseEligible);
    EXPECT_EQ(probe.anchors, 1);
    EXPECT_EQ(probe.audioPulls, 1);
    EXPECT_EQ(probe.deferredQueries, 2);  // no query after success
    EXPECT_EQ(probe.events, (std::vector<std::string>{"encode", "encode", "encode", "commit"}));
    accepted = probe.Submit(true, CommittedTimeline::InjectOutput, 66667, 33334);
    EXPECT_EQ(accepted.firstOutputCommitted, 0u);
    EXPECT_EQ(probe.anchors, 1);
}

TEST(SubmissionTransactionTest, ScreenGrabScheduledAndAudioOutputTimelinesRemainDistinct) {
    SubmissionProbe probe;
    probe.accepted = true;
    const auto result = probe.Submit(true, CommittedTimeline::ScreenGrabScheduled, 100000, 66667);
    EXPECT_EQ(result.timeline, CommittedTimeline::ScreenGrabScheduled);
    EXPECT_EQ(result.videoTimelineUs, 100000);
    EXPECT_EQ(result.audioTimelineUs, 66667);
}

TEST(SubmissionTransactionTest, RepeatsHaveNoCandidateAndCannotCommitFirstOutput) {
    SubmissionProbe probe;
    probe.deferred = true;
    const auto failure = probe.Submit(false, CommittedTimeline::InjectOutput, 100000, 66667);
    EXPECT_TRUE(failure.Deferred());
    EXPECT_EQ(failure.source, SourceDisposition::NoCandidate);
    EXPECT_EQ(probe.anchors, 0);
    probe.accepted = true;
    const auto result = probe.Submit(false, CommittedTimeline::InjectOutput, 100000, 66667);
    EXPECT_EQ(result.output, SubmissionOutput::CachedRepeat);
    EXPECT_EQ(result.source, SourceDisposition::NoCandidate);
    EXPECT_EQ(result.firstOutputCommitted, 0u);
    EXPECT_EQ(probe.anchors, 0);
}

TEST(SubmissionTransactionTest, InvalidResultSizeCannotInvokeSubmissionOrOverwriteStorage) {
    FrameSubmissionResultV1 result;
    int calls = 0;
    auto submit = [&] { ++calls; return AcceptedSubmission(SubmissionOutput::FreshSource, true,
        CommittedTimeline::InjectOutput, 1, 1); };
    EXPECT_FALSE(detail::PublishSubmissionResult(nullptr, submit));
    for (uint32_t size : {0u, 39u, 41u, 0xffffffffu}) {
        result.size = size;
        result.videoTimelineUs = 123;
        EXPECT_FALSE(detail::PublishSubmissionResult(&result, submit));
        EXPECT_EQ(result.size, size);
        EXPECT_EQ(result.videoTimelineUs, 123);
    }
    EXPECT_EQ(calls, 0);
    result.size = sizeof(result);
    EXPECT_TRUE(detail::PublishSubmissionResult(&result, submit));
    EXPECT_TRUE(result.Accepted());
    EXPECT_EQ(calls, 1);
}

TEST(SubmissionTransactionTest, InactiveProductionEncoderRejectsWithoutReusingPriorDeferral) {
    VideoEncoder encoder;
    VideoEncoderTestAccess::SetPreviousDeferral(encoder);
    EXPECT_FALSE(encoder.EncodeFrame(nullptr, nullptr, 0, 0, 0, 0, 0, 0, false, false, 0));
    EXPECT_FALSE(encoder.WasLastFrameDeferred());
    VideoEncoderTestAccess::SetPreviousDeferral(encoder);
    EXPECT_FALSE(encoder.RepeatLastFrame(0, false));
    EXPECT_FALSE(encoder.WasLastFrameDeferred());
}
