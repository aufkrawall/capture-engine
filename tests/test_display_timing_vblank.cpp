#include "../captureengine/display_timing_policy.h"
#include "../captureengine/display_timing_vblank.h"
#include <gtest/gtest.h>

#include <vector>

namespace {

constexpr uint32_t kSource = 0;
constexpr uint32_t kOtherSource = 1;
constexpr int64_t kPeriod = 6944;  // 144 Hz, in the same units as the timestamps
constexpr int64_t kBase = 1'000'000;

// Fills the clock with a steady blank cadence and returns the next blank time.
int64_t FeedBlanks(VerticalBlankClock& clock, std::size_t count, int64_t period = kPeriod,
                   int64_t start = kBase, uint32_t source = kSource) {
    int64_t now = start;
    for (std::size_t i = 0; i < count; ++i) {
        clock.Observe(source, now);
        now += period;
    }
    return now;
}

PendingTimestamp SyncEntry(int64_t timestamp, uint32_t source = kSource) {
    PendingTimestamp entry;
    entry.processId = 7;
    entry.timestamp = timestamp;
    entry.completionKind = DisplayCompletionKind::Sync;
    entry.displaySource = source;
    return entry;
}

}  // namespace

TEST(VerticalBlankClockTest, CannotAnswerBeforeItHasSeenTheDisplay) {
    VerticalBlankClock clock;
    EXPECT_EQ(clock.Snap(kSource, kBase), 0);
    clock.Observe(kSource, kBase);
    EXPECT_EQ(clock.Snap(kSource, kBase + 100), 0);  // one blank is not a cadence
    EXPECT_EQ(clock.PeriodUs(kSource), 0);
}

TEST(VerticalBlankClockTest, MeasuresTheRefreshPeriod) {
    VerticalBlankClock clock;
    FeedBlanks(clock, 10);
    EXPECT_EQ(clock.PeriodUs(kSource), kPeriod);
    EXPECT_EQ(clock.observedBlanks(kSource), 10u);
}

// The measured FSR frame-generation case: completions land at two alternating
// phases inside the interval and must both resolve onto the blank ahead of them.
TEST(VerticalBlankClockTest, RoundsAlternatingCompletionPhasesOntoTheirBlanks) {
    VerticalBlankClock clock;
    FeedBlanks(clock, 12);
    const int64_t firstBlank = kBase;
    EXPECT_EQ(clock.Snap(kSource, firstBlank + 3920), firstBlank + kPeriod);
    EXPECT_EQ(clock.Snap(kSource, firstBlank + 6140), firstBlank + kPeriod);
    EXPECT_EQ(clock.Snap(kSource, firstBlank + kPeriod + 3920), firstBlank + 2 * kPeriod);
}

TEST(VerticalBlankClockTest, ACompletionAlreadyAtABlankStaysOnThatBlank) {
    VerticalBlankClock clock;
    FeedBlanks(clock, 12);
    // A DPC that ran a little late still describes the blank it fired for, and
    // must not be pushed a whole refresh into the future.
    EXPECT_EQ(clock.Snap(kSource, kBase + 3 * kPeriod), kBase + 3 * kPeriod);
    EXPECT_EQ(clock.Snap(kSource, kBase + 3 * kPeriod + 200), kBase + 3 * kPeriod);
}

TEST(VerticalBlankClockTest, DoesNotAnswerForABlankThatHasNotHappenedYet) {
    VerticalBlankClock clock;
    const int64_t afterLast = FeedBlanks(clock, 6);
    EXPECT_EQ(clock.Snap(kSource, afterLast - kPeriod + 3920), 0);
}

TEST(VerticalBlankClockTest, KeepsDisplaysApart) {
    VerticalBlankClock clock;
    FeedBlanks(clock, 8, kPeriod, kBase, kSource);
    FeedBlanks(clock, 8, 8333, kBase + 3000, kOtherSource);
    EXPECT_EQ(clock.PeriodUs(kSource), kPeriod);
    EXPECT_EQ(clock.PeriodUs(kOtherSource), 8333);
    EXPECT_EQ(clock.Snap(kOtherSource, kBase + 3000 + 100), kBase + 3000);
    EXPECT_EQ(clock.busiestSource(), kSource);
}

// The sub-cap case: a lead spread wider than one refresh puts two completions
// inside the same blank interval, and the second frame did reach the screen.
TEST(VerticalBlankClockTest, TwoCompletionsInOneIntervalTakeConsecutiveBlanks) {
    VerticalBlankClock clock;
    FeedBlanks(clock, 12);
    EXPECT_EQ(clock.Claim(kSource, kBase + 3920), kBase + kPeriod);
    EXPECT_EQ(clock.Claim(kSource, kBase + 6140), kBase + 2 * kPeriod);
}

TEST(VerticalBlankClockTest, CompletionsInSeparateIntervalsAreNotPushedAround) {
    VerticalBlankClock clock;
    FeedBlanks(clock, 12);
    // The measured 144 Hz frame-generation pattern: alternating phases, but one
    // completion per interval, so ordering never has to move anything.
    EXPECT_EQ(clock.Claim(kSource, kBase + 3920), kBase + kPeriod);
    EXPECT_EQ(clock.Claim(kSource, kBase + kPeriod + 6140), kBase + 2 * kPeriod);
    EXPECT_EQ(clock.Claim(kSource, kBase + 2 * kPeriod + 3920), kBase + 3 * kPeriod);
}

TEST(VerticalBlankClockTest, AnUnclaimedBlankIsStillTakenDirectly) {
    VerticalBlankClock clock;
    FeedBlanks(clock, 12);
    EXPECT_EQ(clock.Claim(kSource, kBase + 3920), kBase + kPeriod);
    // A later completion skips the blanks nothing claimed rather than crawling.
    EXPECT_EQ(clock.Claim(kSource, kBase + 5 * kPeriod + 3920), kBase + 6 * kPeriod);
}

TEST(VerticalBlankClockTest, StopsForcingRatherThanRunningAwayFromTheDisplay) {
    VerticalBlankClock clock;
    FeedBlanks(clock, 20);
    int64_t last = 0;
    for (std::size_t i = 0; i < 12; ++i)
        last = clock.Claim(kSource, kBase + 3920);
    // Frames arriving faster than the display refreshes cannot all be shown, so
    // the walk gives up and hands back the blank the completion landed on; the
    // publisher then records the frame as dropped instead of inventing a screen
    // time far past it.
    EXPECT_EQ(last, kBase + kPeriod);
}

TEST(VerticalBlankClockTest, ClearForgetsEverything) {
    VerticalBlankClock clock;
    FeedBlanks(clock, 12);
    clock.Clear();
    EXPECT_EQ(clock.PeriodUs(kSource), 0);
    EXPECT_EQ(clock.observedBlanks(kSource), 0u);
    EXPECT_EQ(clock.Snap(kSource, kBase + 3920), 0);
}

TEST(DisplayTimingResolveDeferredScreenTimesTest, RoundsSyncCompletionsAndLeavesTheRestAlone) {
    VerticalBlankClock clock;
    FeedBlanks(clock, 12);
    std::vector<PendingTimestamp> pending;
    pending.push_back(SyncEntry(kBase + 3920));
    PendingTimestamp immediate = SyncEntry(kBase + 3920);
    immediate.completionKind = DisplayCompletionKind::Immediate;
    pending.push_back(immediate);
    PendingTimestamp authoritative = SyncEntry(kBase + 3920);
    authoritative.completionKind = DisplayCompletionKind::Unconditional;
    pending.push_back(authoritative);

    EXPECT_EQ(ResolveDeferredScreenTimes(pending, clock), 1u);
    EXPECT_EQ(pending[0].timestamp, kBase + kPeriod);
    EXPECT_TRUE(pending[0].screenTimeResolved);
    // A tearing flip really did change the screen mid-scanout, and a driver's
    // own generated-frame timestamp is authoritative; neither is rounded.
    EXPECT_EQ(pending[1].timestamp, kBase + 3920);
    EXPECT_EQ(pending[2].timestamp, kBase + 3920);
}

TEST(DisplayTimingResolveDeferredScreenTimesTest, IsIdempotentAndClaimsEachBlankOnce) {
    VerticalBlankClock clock;
    FeedBlanks(clock, 12);
    std::vector<PendingTimestamp> pending;
    pending.push_back(SyncEntry(kBase + 3920));
    pending.push_back(SyncEntry(kBase + 6140));

    EXPECT_EQ(ResolveDeferredScreenTimes(pending, clock), 2u);
    EXPECT_EQ(pending[0].timestamp, kBase + kPeriod);
    EXPECT_EQ(pending[1].timestamp, kBase + 2 * kPeriod);
    // A second pass over the same queue must not move anything again.
    EXPECT_EQ(ResolveDeferredScreenTimes(pending, clock), 0u);
    EXPECT_EQ(pending[0].timestamp, kBase + kPeriod);
    EXPECT_EQ(pending[1].timestamp, kBase + 2 * kPeriod);
}

TEST(DisplayTimingResolveDeferredScreenTimesTest, LeavesTheTimestampAloneWithoutABlankStream) {
    VerticalBlankClock clock;
    std::vector<PendingTimestamp> pending;
    pending.push_back(SyncEntry(kBase + 3920));
    EXPECT_EQ(ResolveDeferredScreenTimes(pending, clock), 0u);
    EXPECT_EQ(pending[0].timestamp, kBase + 3920);
    EXPECT_FALSE(pending[0].screenTimeResolved);
}
