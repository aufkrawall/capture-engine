#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ce::media {

enum class SubmissionStatus : uint32_t { Rejected, Accepted, Deferred };
enum class SubmissionOutput : uint32_t { None, FreshSource, CachedRepeat };
enum class SourceDisposition : uint32_t { ReleaseEligible, RetainForRetry, NoCandidate };
enum class CommittedTimeline : uint32_t { None, InjectOutput, ScreenGrabScheduled };

// Versioned DLL output. Acceptance is encoder ingestion, not packet emission or GPU completion.
// Leases remain caller-owned; release eligibility does not replace fence-completion checks.
// Timeline values are the committed video value and audio pull target, both in microseconds.
struct alignas(8) FrameSubmissionResultV1 {
    uint32_t size = sizeof(FrameSubmissionResultV1);
    SubmissionStatus status = SubmissionStatus::Rejected;
    SubmissionOutput output = SubmissionOutput::None;
    SourceDisposition source = SourceDisposition::ReleaseEligible;
    uint32_t firstOutputCommitted = 0;
    CommittedTimeline timeline = CommittedTimeline::None;
    int64_t videoTimelineUs = -1;
    int64_t audioTimelineUs = -1;

    bool Accepted() const { return status == SubmissionStatus::Accepted; }
    bool Deferred() const { return status == SubmissionStatus::Deferred; }
};

static_assert(sizeof(FrameSubmissionResultV1) == 40);
static_assert(alignof(FrameSubmissionResultV1) == 8);
static_assert(offsetof(FrameSubmissionResultV1, videoTimelineUs) == 24);
static_assert(offsetof(FrameSubmissionResultV1, audioTimelineUs) == 32);
static_assert(std::is_standard_layout_v<FrameSubmissionResultV1>);
static_assert(std::is_trivially_copyable_v<FrameSubmissionResultV1>);

inline FrameSubmissionResultV1 UnacceptedSubmission(bool deferred, bool candidate = true) {
    FrameSubmissionResultV1 result;
    result.status = deferred ? SubmissionStatus::Deferred : SubmissionStatus::Rejected;
    result.source = candidate ? (deferred ? SourceDisposition::RetainForRetry : SourceDisposition::ReleaseEligible)
                              : SourceDisposition::NoCandidate;
    return result;
}

inline FrameSubmissionResultV1 AcceptedSubmission(SubmissionOutput output, bool first,
                                                   CommittedTimeline timeline, int64_t videoUs, int64_t audioUs) {
    FrameSubmissionResultV1 result;
    result.status = SubmissionStatus::Accepted;
    result.output = output;
    result.source = output == SubmissionOutput::FreshSource ? SourceDisposition::ReleaseEligible
                                                           : SourceDisposition::NoCandidate;
    result.firstOutputCommitted = first ? 1u : 0u;
    result.timeline = timeline;
    result.videoTimelineUs = videoUs;
    result.audioTimelineUs = audioUs;
    return result;
}

}  // namespace ce::media
