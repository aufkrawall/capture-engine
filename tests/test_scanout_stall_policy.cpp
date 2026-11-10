#include <gtest/gtest.h>

#include <cstring>

#include "hook/present/scanout_stall_policy.h"

namespace {

using namespace ce::scanout_stall;

DisplayObservation Display(uint64_t sequence, int64_t publishUs, uint64_t generation = 2) {
    DisplayObservation observation;
    observation.active = true;
    observation.generation = generation;
    observation.sequence = sequence;
    observation.publishUs = publishUs;
    return observation;
}

constexpr int64_t kPresentIntervalUs = 7'000;

// Presents `count` times with an unchanged display, returns the last step and the first non-empty event.
struct PresentRun {
    StallStep last;
    StallStep firstEvent;
    int64_t nowUs = 0;
};

PresentRun PresentTimes(StallTracker& tracker, PresentRun run, uint32_t count, const DisplayObservation& observation) {
    for (uint32_t i = 0; i < count; ++i) {
        run.nowUs += kPresentIntervalUs;
        run.last = tracker.OnPresent(run.nowUs, observation);
        if (run.firstEvent.event == StallEvent::kNone && run.last.event != StallEvent::kNone) {
            run.firstEvent = run.last;
        }
    }
    return run;
}

TEST(ScanoutStallTracker, ASteadilyChangingDisplayNeverStalls) {
    StallTracker tracker;
    int64_t now = 1'000'000;
    for (uint64_t sequence = 1; sequence <= 200; ++sequence) {
        now += kPresentIntervalUs;
        const auto step = tracker.OnPresent(now, Display(sequence, now));
        EXPECT_EQ(step.event, StallEvent::kNone);
    }
    EXPECT_FALSE(tracker.InEpisode());
    EXPECT_EQ(tracker.Episodes(), 0u);
}

TEST(ScanoutStallTracker, AnEpisodeStartsAfterTheAgeAndThePresentCountAreBothReached) {
    StallTracker tracker;
    PresentRun run;
    run.nowUs = 1'000'000;
    ASSERT_EQ(tracker.OnPresent(run.nowUs, Display(10, run.nowUs)).event, StallEvent::kNone);
    // 40 presents (280 ms) without a change: both thresholds are met around the 36th (252 ms).
    run = PresentTimes(tracker, run, 40, Display(10, 1'000'000));
    ASSERT_EQ(run.firstEvent.event, StallEvent::kStart);
    EXPECT_GE(run.firstEvent.ageUs, kStallAgeUs);
    EXPECT_GE(run.firstEvent.presentsSinceChange, kStallMinPresents);
    EXPECT_EQ(run.firstEvent.episode, 1u);
    EXPECT_TRUE(tracker.InEpisode());
}

TEST(ScanoutStallTracker, FewPresentsOrALowAgeDoNotStartAnEpisode) {
    {
        // A long gap with fewer than kStallMinPresents presents: the game itself stopped presenting.
        StallTracker tracker;
        ASSERT_EQ(tracker.OnPresent(1'000'000, Display(10, 1'000'000)).event, StallEvent::kNone);
        StallStep step;
        for (uint32_t i = 0; i < kStallMinPresents - 1; ++i) {
            step = tracker.OnPresent(1'000'000 + 100'000 * (i + 1), Display(10, 1'000'000));
            EXPECT_EQ(step.event, StallEvent::kNone);
        }
        EXPECT_FALSE(tracker.InEpisode());
    }
    {
        // Many presents inside the age: an ordinary frame rate on a display that is about to change.
        StallTracker tracker;
        ASSERT_EQ(tracker.OnPresent(1'000'000, Display(10, 1'000'000)).event, StallEvent::kNone);
        for (uint32_t i = 0; i < 100; ++i) {
            const auto step = tracker.OnPresent(1'000'000 + 2'000 * (i + 1), Display(10, 1'000'000));
            EXPECT_EQ(step.event, StallEvent::kNone) << i;
        }
    }
}

TEST(ScanoutStallTracker, ARunningEpisodeRepeatsAtMostEveryTwoSeconds) {
    StallTracker tracker;
    PresentRun run;
    run.nowUs = 1'000'000;
    ASSERT_EQ(tracker.OnPresent(run.nowUs, Display(10, run.nowUs)).event, StallEvent::kNone);
    run = PresentTimes(tracker, run, 40, Display(10, 1'000'000));
    ASSERT_TRUE(tracker.InEpisode());
    int diffs = 0;
    int64_t firstDiffUs = 0;
    int64_t lastDiffUs = 0;
    for (int i = 0; i < 1'500; ++i) {  // 10.5 s at 142 presents/s
        run.nowUs += kPresentIntervalUs;
        const auto step = tracker.OnPresent(run.nowUs, Display(10, 1'000'000));
        if (step.event == StallEvent::kDiff) {
            ++diffs;
            if (diffs == 1) {
                firstDiffUs = run.nowUs;
            } else {
                EXPECT_GE(run.nowUs - lastDiffUs, kStallDiffIntervalUs);
            }
            lastDiffUs = run.nowUs;
        }
        EXPECT_NE(step.event, StallEvent::kStart);
    }
    EXPECT_GE(diffs, 4);
    EXPECT_LE(diffs, 6);
    EXPECT_GT(firstDiffUs, 0);
}

TEST(ScanoutStallTracker, TheEpisodeEndsWithItsDurationAndWhatChangedFirst) {
    StallTracker tracker;
    PresentRun run;
    run.nowUs = 1'000'000;
    ASSERT_EQ(tracker.OnPresent(run.nowUs, Display(10, run.nowUs)).event, StallEvent::kNone);
    run = PresentTimes(tracker, run, 300, Display(10, 1'000'000));
    ASSERT_TRUE(tracker.InEpisode());
    // Three transitions arrive at once, as the compositor's first composed frames did.
    run.nowUs += kPresentIntervalUs;
    const auto end = tracker.OnPresent(run.nowUs, Display(13, run.nowUs));
    EXPECT_EQ(end.event, StallEvent::kEnd);
    EXPECT_EQ(end.episode, 1u);
    EXPECT_EQ(end.sequenceAdvance, 3u);
    EXPECT_FALSE(end.generationChanged);
    EXPECT_EQ(end.episodeUs, run.nowUs - 1'000'000);
    EXPECT_EQ(end.episodePresents, 301u);
    EXPECT_FALSE(tracker.InEpisode());
    // The next stall is a new episode.
    run.firstEvent = StallStep{};
    run = PresentTimes(tracker, run, 60, Display(13, run.nowUs));
    ASSERT_EQ(run.firstEvent.event, StallEvent::kStart);
    EXPECT_EQ(run.firstEvent.episode, 2u);
}

TEST(ScanoutStallTracker, AStreamResetEndsTheEpisodeAndIsSaidSo) {
    StallTracker tracker;
    PresentRun run;
    run.nowUs = 1'000'000;
    ASSERT_EQ(tracker.OnPresent(run.nowUs, Display(10, run.nowUs, 2)).event, StallEvent::kNone);
    run = PresentTimes(tracker, run, 100, Display(10, 1'000'000, 2));
    ASSERT_TRUE(tracker.InEpisode());
    run.nowUs += kPresentIntervalUs;
    const auto end = tracker.OnPresent(run.nowUs, Display(1, run.nowUs, 4));
    EXPECT_EQ(end.event, StallEvent::kEnd);
    EXPECT_TRUE(end.generationChanged);
    EXPECT_EQ(end.sequenceAdvance, 0u);
}

TEST(ScanoutStallTracker, AnInactiveStreamSaysNothingAboutTheScreen) {
    StallTracker tracker;
    PresentRun run;
    run.nowUs = 1'000'000;
    ASSERT_EQ(tracker.OnPresent(run.nowUs, Display(10, run.nowUs)).event, StallEvent::kNone);
    DisplayObservation inactive;  // Unavailable / failed: not active
    run = PresentTimes(tracker, run, 500, inactive);
    EXPECT_EQ(run.firstEvent.event, StallEvent::kNone);
    EXPECT_FALSE(tracker.InEpisode());
    // Publishing resumes: counting starts over, so the first present cannot be an old stall's end.
    run.nowUs += kPresentIntervalUs;
    EXPECT_EQ(tracker.OnPresent(run.nowUs, Display(20, run.nowUs)).event, StallEvent::kNone);
}

TEST(ScanoutStallTracker, TheAgeFollowsThePublicationStampNotTheFirstPresentWeSaw) {
    // Attached long after the last transition: the stream's own stamp is already 600 ms old at the first present.
    StallTracker tracker;
    ASSERT_EQ(tracker.OnPresent(2'000'000, Display(10, 1'400'000)).event, StallEvent::kNone);
    StallStep step;
    for (uint32_t i = 0; i < kStallMinPresents; ++i) {
        step = tracker.OnPresent(2'000'000 + 1'000 * (i + 1), Display(10, 1'400'000));
    }
    EXPECT_EQ(step.event, StallEvent::kStart);
    EXPECT_GE(step.ageUs, 600'000);
}

TEST(ScanoutFirstChangeWatch, ReportsTheFirstDisplayChangeAfterTheHandoffWithItsPresents) {
    FirstChangeWatch watch;
    constexpr uint64_t kChain = 0x1000;
    constexpr uint64_t kOtherChain = 0x2000;
    watch.Arm(3, kChain, 5'000'000, Display(100, 4'990'000));
    ASSERT_TRUE(watch.Armed());
    for (int i = 0; i < 40; ++i) {
        EXPECT_FALSE(watch.OnPresent(kChain, 5'000'000 + 7'000 * (i + 1), Display(100, 4'990'000)).reached);
    }
    // Another chain's presents are not this chain's.
    EXPECT_FALSE(watch.OnPresent(kOtherChain, 5'400'000, Display(100, 4'990'000)).reached);
    const auto change = watch.OnPresent(kChain, 5'500'000, Display(101, 5'499'000));
    ASSERT_TRUE(change.reached);
    EXPECT_EQ(change.handoff, 3u);
    EXPECT_EQ(change.sinceHandoffUs, 500'000);
    EXPECT_EQ(change.presentsBefore, 40u);
    EXPECT_FALSE(change.generationChanged);
    EXPECT_FALSE(watch.Armed());
    // Reported once.
    EXPECT_FALSE(watch.OnPresent(kChain, 5'600'000, Display(102, 5'599'000)).reached);
}

TEST(ScanoutFirstChangeWatch, AStreamResetCountsAsTheChangeAndAnInactiveStreamDoesNot) {
    FirstChangeWatch watch;
    watch.Arm(1, 0x1000, 1'000'000, Display(50, 990'000, 2));
    DisplayObservation inactive;
    EXPECT_FALSE(watch.OnPresent(0x1000, 1'100'000, inactive).reached);
    const auto change = watch.OnPresent(0x1000, 1'200'000, Display(1, 1'199'000, 4));
    ASSERT_TRUE(change.reached);
    EXPECT_TRUE(change.generationChanged);
}

TEST(ScanoutFirstChangeWatch, NothingIsReportedWhileUnarmed) {
    FirstChangeWatch watch;
    EXPECT_FALSE(watch.OnPresent(0x1000, 1'000'000, Display(1, 1'000'000)).reached);
}

TEST(ScanoutPresentResultTally, CountsDistinctResultsMostFrequentFirst) {
    PresentResultTally tally;
    char text[160] = {};
    EXPECT_GT(tally.Describe(text, sizeof(text)), 0u);
    EXPECT_STREQ(text, "none");
    for (int i = 0; i < 3; ++i) {
        tally.Note(0);
    }
    tally.Note(static_cast<int32_t>(0x087A0001));  // DXGI_STATUS_OCCLUDED
    for (int i = 0; i < 5; ++i) {
        tally.Note(static_cast<int32_t>(0x887A0005));  // DXGI_ERROR_DEVICE_REMOVED
    }
    EXPECT_EQ(tally.total(), 9u);
    tally.Describe(text, sizeof(text));
    EXPECT_STREQ(text, "0x887A0005=5 0x00000000=3 0x087A0001=1");
}

TEST(ScanoutPresentResultTally, ResultsBeyondTheSlotsAreCountedTogetherAndResetClears) {
    PresentResultTally tally;
    for (int32_t value = 1; value <= static_cast<int32_t>(PresentResultTally::kSlots) + 3; ++value) {
        tally.Note(value);
    }
    EXPECT_EQ(tally.other(), 3u);
    char text[256] = {};
    tally.Describe(text, sizeof(text));
    EXPECT_NE(std::strstr(text, "other=3"), nullptr) << text;
    tally.Reset();
    EXPECT_EQ(tally.total(), 0u);
    tally.Describe(text, sizeof(text));
    EXPECT_STREQ(text, "none");
}

TEST(ScanoutPresentResultTally, ATinyBufferStaysTerminated) {
    PresentResultTally tally;
    tally.Note(0);
    tally.Note(static_cast<int32_t>(0x087A0001));
    char text[12];
    std::memset(text, 'x', sizeof(text));
    const size_t written = tally.Describe(text, sizeof(text));
    EXPECT_LT(written, sizeof(text));
    EXPECT_EQ(text[written], '\0');
    EXPECT_EQ(tally.Describe(nullptr, 8), 0u);
    char one[1] = {'x'};
    EXPECT_EQ(tally.Describe(one, 1), 0u);
    EXPECT_EQ(one[0], '\0');
}

}  // namespace
