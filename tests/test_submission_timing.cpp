#include <gtest/gtest.h>

#include <type_traits>

#include "common/capture/time_grid.h"
#include "mediaengine/engine/submission_timing.h"
#include "mediaengine/engine/submission_transaction.h"

namespace {
using namespace ce::time;
using namespace ce::media;
using namespace ce::media::timing;
static_assert(!std::is_convertible_v<QpcTicks, Microseconds>);
static_assert(!std::is_convertible_v<Microseconds, AudioHundredNanoseconds>);
static_assert(!std::is_convertible_v<FrameIndex, QpcTicks>);
static_assert(!std::is_convertible_v<int64_t, Microseconds>);
static_assert(sizeof(QpcTicks) == sizeof(int64_t));
}  // namespace

TEST(SubmissionTimingTest, UnitsConvertAtTheAudioAndScheduledOutputBoundaries) {
    EXPECT_EQ(AudioAnchor(QpcTicks{12345678}, QpcFrequency{1000000}).count(), 123456780);
    EXPECT_EQ(DiagnosticTimestamp(QpcTicks{12345678}, QpcFrequency{1000000}).count(), 12345);
    EXPECT_EQ(AudioAnchor(QpcTicks{-1}, QpcFrequency{1000000}).count(), 0);
    EXPECT_EQ(AudioAnchor(QpcTicks{123}, QpcFrequency{0}).count(), 0);
    EXPECT_EQ(DiagnosticTimestamp(QpcTicks{123}, QpcFrequency{0}).count(), 123);  // legacy diagnostics only
    EXPECT_EQ(ce::time::ScheduledElapsed(QpcTicks{100}, QpcTicks{350}, QpcFrequency{1000}).count(), 250000);
    EXPECT_EQ(ce::time::ScheduledElapsed(QpcTicks{100}, QpcTicks{99}, QpcFrequency{1000}).count(), -1);
    EXPECT_EQ(ce::time::ScheduledElapsed(QpcTicks{100}, QpcTicks{350}, QpcFrequency{0}).count(), -1);
    const auto slot = ce::time::SelectionSlot(QpcTicks{1000}, GridTick{121}, QpcFrequency{10000000}, 120);
    EXPECT_EQ(ce::time::NextOutputFrame(FrameIndex{119}).count(), 120);
    EXPECT_EQ(ce::time::NextOutputFrame(FrameIndex{-1}).count(), 0);
    EXPECT_EQ(ce::time::SelectionSlot(QpcTicks{1000}, GridTick{1}, QpcFrequency{10000000}, 120).count(), 1000);
    EXPECT_EQ(slot.count(), 10001000);  // selection tick 1 is frame zero; tick 121 is one second at 120 Hz
}

TEST(SubmissionTimingTest, FailedAndDeferredCandidatesCannotCommitFirstAnchorOrOutputTime) {
    SubmissionTiming timing;
    bool accepted = false, deferred = false;
    int anchors = 0;
    const FirstOutputAnchor candidate{Milliseconds{10}, Milliseconds{15}, AudioHundredNanoseconds{150000}};
    auto submit = [&] {
        return ce::media::detail::SubmitAndCommit(true, [&] { return accepted; }, [&] { return deferred; }, [&] {
            const bool first = timing.CommitFirst(candidate, [&](const auto& anchor) {
                ++anchors;
                EXPECT_TRUE(timing.HasFirstOutput());  // publication precedes the audio callback
                EXPECT_EQ(anchor.audioQpc.count(), 150000);
            });
            timing.CommitElapsed(SourceClock::Inject, Microseconds{16667});
            return AcceptedSubmission(SubmissionOutput::FreshSource, first, CommittedTimeline::InjectOutput, 16667, 16667);
        });
    };
    EXPECT_FALSE(submit().Accepted());
    deferred = true;
    EXPECT_TRUE(submit().Deferred());
    EXPECT_FALSE(timing.HasFirstOutput());
    EXPECT_EQ(timing.CommittedElapsed(SourceClock::Inject).count(), 0);
    EXPECT_EQ(anchors, 0);
    accepted = true;
    EXPECT_EQ(submit().firstOutputCommitted, 1u);
    EXPECT_EQ(anchors, 1);
    EXPECT_EQ(timing.FirstAnchor().source.count(), 10);
    EXPECT_EQ(timing.FirstAnchor().audio.count(), 15);
    EXPECT_EQ(submit().firstOutputCommitted, 0u);
    EXPECT_EQ(anchors, 1);
}

TEST(SubmissionTimingTest, FirstAnchorReentryAndResetHaveExactlyOnceCommitment) {
    SubmissionTiming timing;
    int anchors = 0;
    const FirstOutputAnchor initial{Milliseconds{1}, Milliseconds{2}, AudioHundredNanoseconds{20000}};
    const FirstOutputAnchor replacement{Milliseconds{3}, Milliseconds{4}, AudioHundredNanoseconds{40000}};
    EXPECT_TRUE(timing.CommitFirst(initial, [&](const auto&) {
        ++anchors;
        EXPECT_FALSE(timing.CommitFirst(replacement, [&](const auto&) { ++anchors; }));
    }));
    EXPECT_EQ(anchors, 1);
    EXPECT_EQ(timing.FirstAnchor().audio.count(), 2);
    timing.CommitElapsed(SourceClock::ScreenGrab, Microseconds{100000});
    timing.Reset();
    EXPECT_FALSE(timing.HasFirstOutput());
    EXPECT_EQ(timing.FirstAnchor().audioQpc.count(), 0);
    EXPECT_EQ(timing.SamplingElapsed(SourceClock::ScreenGrab).count(), 0);
    EXPECT_EQ(timing.CommittedElapsed(SourceClock::ScreenGrab).count(), 0);
    EXPECT_TRUE(timing.CommitFirst(replacement, [&](const auto&) { ++anchors; }));
    EXPECT_EQ(anchors, 2);
}

TEST(SubmissionTimingTest, SourceSamplingCursorsAndAcceptedTimelinesRemainIndependent) {
    SubmissionTiming timing;
    const QpcFrequency frequency{1000};
    EXPECT_EQ(timing.ResolveCandidate(SourceClock::Inject, true, frequency, QpcTicks{1000},
                                      Microseconds{0}, Microseconds{-1}).count(), 0);
    EXPECT_EQ(timing.ResolveCandidate(SourceClock::Inject, true, frequency, QpcTicks{2000},
                                      Microseconds{10}, Microseconds{-1}).count(), 1000000);
    // Legacy VFR sampling may advance after a rejected candidate; output/audio commitment does not.
    EXPECT_EQ(timing.SamplingElapsed(SourceClock::Inject).count(), 1000000);
    EXPECT_EQ(timing.CommittedElapsed(SourceClock::Inject).count(), 0);
    EXPECT_FALSE(timing.HasFirstOutput());
    timing.CommitElapsed(SourceClock::Inject, Microseconds{16667});
    EXPECT_EQ(timing.CommittedElapsed(SourceClock::Inject).count(), 16667);
    EXPECT_EQ(timing.SamplingElapsed(SourceClock::Inject).count(), 1000000);
    EXPECT_EQ(timing.ResolveCandidate(SourceClock::ScreenGrab, false, frequency, QpcTicks{3000},
                                      Microseconds{999999}, Microseconds{50000}).count(), 50000);
    timing.CommitElapsed(SourceClock::ScreenGrab, Microseconds{50000});
    EXPECT_EQ(timing.CommittedElapsed(SourceClock::ScreenGrab).count(), 50000);
    EXPECT_EQ(timing.CommittedElapsed(SourceClock::Inject).count(), 16667);
    // Explicit WGC schedules remain authoritative; scheduling, not this owner, orders them.
    EXPECT_EQ(timing.ResolveCandidate(SourceClock::ScreenGrab, false, frequency, QpcTicks{4000},
                                      Microseconds{100000}, Microseconds{30000}).count(), 30000);
    timing.CommitElapsed(SourceClock::ScreenGrab, Microseconds{-1});
    EXPECT_EQ(timing.CommittedElapsed(SourceClock::ScreenGrab).count(), 50000);
}
