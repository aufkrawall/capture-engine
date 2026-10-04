#include <gtest/gtest.h>

#include <memory>
#include <vector>
#include <string>

#include "captureengine/app/mediaengine_loader.h"
#include "common/capture/frame_queue.h"
#include "captureengine/media/candidate_completion.h"

namespace {
VideoFrameSubmissionDesc lastInject{};
D3D11FrameSubmissionDesc lastScreen{};
ce::media::FrameSubmissionResultV1 nextResult;
bool boundaryValid = true;
uint32_t lastGeneration = 0;
int64_t lastRepeatQpc = 0, lastRepeatUs = 0;
std::vector<std::string> calls;
void BindGeneration(uint32_t generation) { lastGeneration = generation; calls.emplace_back("generation"); }
bool InjectExport(const VideoFrameSubmissionDesc* desc, ce::media::FrameSubmissionResultV1* result) {
    lastInject = *desc; *result = nextResult; calls.emplace_back("inject"); return boundaryValid;
}
bool ScreenExport(const D3D11FrameSubmissionDesc* desc, ce::media::FrameSubmissionResultV1* result) {
    lastScreen = *desc; *result = nextResult; calls.emplace_back("screen"); return boundaryValid;
}
bool RepeatExport(int64_t qpc, int64_t us, const ce::cursor::CaptureState*,
                  ce::media::FrameSubmissionResultV1* result) {
    lastRepeatQpc = qpc; lastRepeatUs = us; *result = nextResult; return boundaryValid;
}
MediaEngine_SetInjectTransportGeneration_t testGeneration = BindGeneration;
MediaEngine_SubmitFrameWithResultV1_t testInject = InjectExport;
MediaEngine_SubmitFrameD3D11WithResultV1_t testScreen = ScreenExport;
MediaEngine_RepeatLastFrameWithResultV1_t testRepeat = RepeatExport;
}  // namespace

// The actual product adapters use controlled DLL exports, with no copied descriptor or transaction code.
#define MediaEngine_SetInjectTransportGeneration testGeneration
#define MediaEngine_SubmitFrameWithResultV1 testInject
#define MediaEngine_SubmitFrameD3D11WithResultV1 testScreen
#define MediaEngine_RepeatLastFrameWithResultV1 testRepeat
// NOLINTNEXTLINE(bugprone-suspicious-include) - actual production adapter orchestration
#include "captureengine/media/frame_submission_inject.cpp"
// NOLINTNEXTLINE(bugprone-suspicious-include) - actual production adapter orchestration
#include "captureengine/media/frame_submission_d3d11.cpp"
#undef MediaEngine_SetInjectTransportGeneration
#undef MediaEngine_SubmitFrameWithResultV1
#undef MediaEngine_SubmitFrameD3D11WithResultV1
#undef MediaEngine_RepeatLastFrameWithResultV1

namespace {
using namespace ce::media;
class FrameSubmissionAdapterTest : public testing::Test {
protected:
    void SetUp() override { nextResult = {}; boundaryValid = true; calls.clear(); lastGeneration = 0; }
};
}  // namespace

TEST_F(FrameSubmissionAdapterTest, InjectBindsGenerationAndPreservesSourceContractWithoutReleasingLease) {
    auto ring = std::make_unique<FrameRingBuffer>();
    ring->writeIndex.store(1); ring->slots[0].valid.store(1);
    auto state = std::make_shared<ce::InjectFrameRingLeaseState>(ring.get());
    QueuedFrame frame;
    frame.injectRingLease = state->Acquire(0);
    frame.sharedHandle = reinterpret_cast<HANDLE>(uint64_t{11});
    frame.fenceHandle = reinterpret_cast<HANDLE>(uint64_t{12});
    frame.fenceValue = 13; frame.timestamp = 14; frame.sourcePid = 15;
    frame.transportGeneration = 16; frame.width = 17; frame.height = 18; frame.format = 19;
    frame.luidLow = -20; frame.luidHigh = 21; frame.isHDR = true;
    nextResult = UnacceptedSubmission(true);
    const auto result = submission::Inject(frame, &frame.cursorState);
    EXPECT_TRUE(result.Deferred());
    EXPECT_EQ(result.source, SourceDisposition::RetainForRetry);
    EXPECT_EQ(calls, (std::vector<std::string>{"generation", "inject"}));
    EXPECT_EQ(lastGeneration, 16u);
    EXPECT_EQ(lastInject.textureHandle, 11u); EXPECT_EQ(lastInject.fenceHandle, 12u);
    EXPECT_EQ(lastInject.fenceValue, 13u); EXPECT_EQ(lastInject.timestamp, 14);
    EXPECT_EQ(lastInject.sourcePid, 15u); EXPECT_EQ(lastInject.width, 17u); EXPECT_EQ(lastInject.height, 18u);
    EXPECT_EQ(lastInject.format, 19u); EXPECT_EQ(lastInject.luidLow, -20); EXPECT_EQ(lastInject.luidHigh, 21);
    EXPECT_TRUE(lastInject.isHDR); EXPECT_EQ(lastInject.cursorState, &frame.cursorState);
    EXPECT_TRUE(frame.injectRingLease); EXPECT_EQ(ring->readIndex.load(), 0u);
    calls.clear(); frame.isShmem = true; frame.shmemSlot = 1;
    nextResult = UnacceptedSubmission(false);
    EXPECT_FALSE(submission::Inject(frame, nullptr).Accepted());
    EXPECT_EQ(calls, (std::vector<std::string>{"inject"}));
    EXPECT_TRUE(lastInject.isShmem); EXPECT_EQ(lastInject.shmemSlot, 1);
    EXPECT_TRUE(frame.injectRingLease);
}

TEST_F(FrameSubmissionAdapterTest, ScreenGrabSeparatesMediaTimestampAndScheduledTimeline) {
    QueuedFrame frame;
    frame.texture = reinterpret_cast<ID3D11Texture2D*>(uintptr_t{123});
    frame.width = 640; frame.height = 480; frame.timestamp = 999;
    frame.captureLeft = -400; frame.captureTop = 30; frame.isHDR = true;
    nextResult = AcceptedSubmission(SubmissionOutput::FreshSource, true,
                                   CommittedTimeline::ScreenGrabScheduled, 50000, 33333);
    const auto result = submission::ScreenGrab(frame, 77, 50000, &frame.cursorState);
    EXPECT_TRUE(result.Accepted()); EXPECT_EQ(result.videoTimelineUs, 50000); EXPECT_EQ(result.audioTimelineUs, 33333);
    EXPECT_EQ(lastScreen.texture, frame.texture); EXPECT_EQ(lastScreen.timestamp, 77);
    EXPECT_EQ(lastScreen.timelineElapsedUs, 50000); EXPECT_EQ(lastScreen.captureLeft, -400);
    EXPECT_EQ(lastScreen.captureTop, 30); EXPECT_EQ(lastScreen.cursorState, &frame.cursorState);
    EXPECT_TRUE(lastScreen.isHDR);
    EXPECT_TRUE(submission::Black({frame.texture, 640, 480, true}, 88, 60000, nullptr).Accepted());
    EXPECT_EQ(lastScreen.timestamp, 88); EXPECT_EQ(lastScreen.timelineElapsedUs, 60000);
    EXPECT_EQ(lastScreen.captureLeft, 0); EXPECT_EQ(lastScreen.captureTop, 0);
}

TEST_F(FrameSubmissionAdapterTest, RepeatCarriesExplicitTimingAndInvalidBoundaryCannotAdmitOutput) {
    nextResult = AcceptedSubmission(SubmissionOutput::CachedRepeat, false,
                                   CommittedTimeline::ScreenGrabScheduled, 100000, 66667);
    EXPECT_TRUE(submission::Repeat(555, 100000, nullptr).Accepted());
    EXPECT_EQ(lastRepeatQpc, 555); EXPECT_EQ(lastRepeatUs, 100000);
    boundaryValid = false;
    const auto rejected = submission::Repeat(555, -1, nullptr);
    EXPECT_FALSE(rejected.Accepted()); EXPECT_EQ(rejected.output, SubmissionOutput::None);
    EXPECT_EQ(rejected.source, SourceDisposition::NoCandidate);
}

TEST_F(FrameSubmissionAdapterTest, CandidateCompletionRetainsRetryAndCommitsOnlyFreshAcceptedOwnership) {
    auto ring = std::make_unique<FrameRingBuffer>();
    ring->writeIndex.store(1); ring->slots[0].valid.store(1);
    auto state = std::make_shared<ce::InjectFrameRingLeaseState>(ring.get());
    QueuedFrame frame, last;
    frame.frameIndex = 99; last.frameIndex = 7; frame.injectRingLease = state->Acquire(0);
    int promoted = 0, discarded = 0;
    auto promote = [&] { ++promoted; frame.injectRingLease.Reset(); last = std::move(frame); };
    auto discard = [&](bool) { ++discarded; frame.injectRingLease.Reset(); frame = QueuedFrame{}; };
    submission::detail::CompleteCandidate(UnacceptedSubmission(true), false, promote, discard);
    EXPECT_EQ(promoted, 0); EXPECT_EQ(discarded, 0); EXPECT_TRUE(frame.injectRingLease);
    EXPECT_EQ(last.frameIndex, 7u); EXPECT_EQ(ring->readIndex.load(), 0u);
    auto accepted = AcceptedSubmission(SubmissionOutput::FreshSource, true, CommittedTimeline::InjectOutput, 16667, 16667);
    submission::detail::CompleteCandidate(accepted, true, promote, discard);
    EXPECT_EQ(promoted, 1); EXPECT_EQ(discarded, 0); EXPECT_FALSE(frame.injectRingLease);
    EXPECT_EQ(last.frameIndex, 99u); EXPECT_EQ(ring->readIndex.load(), 1u);
}

TEST_F(FrameSubmissionAdapterTest, RejectedAndRecoveredCachedCandidatesNeverPromoteSourceMetadata) {
    for (bool originalAccepted : {false, true}) {
        QueuedFrame last;
        last.frameIndex = 7;
        int promoted = 0, discarded = 0;
        bool replacement = false;
        auto promote = [&] { ++promoted; last.frameIndex = 99; };
        auto discard = [&](bool emitted) { ++discarded; replacement = emitted; };
        auto repeat = AcceptedSubmission(SubmissionOutput::CachedRepeat, false, CommittedTimeline::InjectOutput, 16667, 16667);
        repeat.source = SourceDisposition::ReleaseEligible;
        submission::detail::CompleteCandidate(repeat, originalAccepted, promote, discard);
        EXPECT_EQ(promoted, 0); EXPECT_EQ(discarded, 1); EXPECT_TRUE(replacement); EXPECT_EQ(last.frameIndex, 7u);
        discarded = 0;
        submission::detail::CompleteCandidate(UnacceptedSubmission(false), false, promote, discard);
        EXPECT_EQ(promoted, 0); EXPECT_EQ(discarded, 1); EXPECT_FALSE(replacement); EXPECT_EQ(last.frameIndex, 7u);
    }
}
