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

// The blank a completion is shown at is normally still in the future at the
// moment the flip completes, and the driver may never report it at all; the
// grid is what lets the clock answer for it either way.
TEST(VerticalBlankClockTest, AnswersForABlankThatHasNotBeenReportedYet) {
    VerticalBlankClock clock;
    const int64_t afterLast = FeedBlanks(clock, 6);
    EXPECT_EQ(clock.Snap(kSource, afterLast - kPeriod + 3920), afterLast);
}

// Measured at 3840x2160 with the panel at its 144 Hz cap: one run reported 4264
// blanks for 4235 flips, the next 657 for 4247. A stream with holes in it is
// still a clock, because the blanks it does report lie on the refresh grid.
TEST(VerticalBlankClockTest, PlacesFramesOnAStreamReportedOnlyInPart) {
    VerticalBlankClock clock;
    for (int step : {0, 1, 2, 5, 6, 9, 13, 14})
        clock.Observe(kSource, kBase + step * kPeriod);
    EXPECT_EQ(clock.PeriodUs(kSource), kPeriod);
    EXPECT_TRUE(clock.CanPlaceFrames(kSource));
    // The blank ahead of this completion was never reported, and is still the
    // blank the frame reaches the screen at.
    EXPECT_EQ(clock.Snap(kSource, kBase + 10 * kPeriod + 3920), kBase + 11 * kPeriod);
}

// The gap sizes here are the ones measured in the Talos run the user reported:
// a panel refreshing when a frame is ready, not on a grid.
TEST(VerticalBlankClockTest, RefusesAVariableRefreshStream) {
    VerticalBlankClock clock;
    int64_t now = kBase;
    for (int64_t gap : {11900, 51100, 9460, 117472, 23000, 9880, 46000}) {
        clock.Observe(kSource, now);
        now += gap;
    }
    clock.Observe(kSource, now);
    EXPECT_EQ(clock.PeriodUs(kSource), 0);
    EXPECT_FALSE(clock.CanPlaceFrames(kSource));
    // Every completion then keeps the driver's own flip timestamp, so the
    // published series stays in one unit instead of mixing two.
    EXPECT_EQ(clock.Snap(kSource, kBase + 5000), 0);
    EXPECT_EQ(clock.Claim(kSource, kBase + 5000), 0);
}

TEST(VerticalBlankClockTest, MeasuresThePeriodAcrossReportingJitter) {
    VerticalBlankClock clock;
    // Interior blanks reported a little early and a little late; the span
    // between the outermost two still covers exactly nine refreshes.
    for (int i = 0; i < 10; ++i) {
        int64_t offset = 0;
        if (i == 3)
            offset = -44;
        else if (i == 7)
            offset = 44;
        clock.Observe(kSource, kBase + i * kPeriod + offset);
    }
    EXPECT_EQ(clock.PeriodUs(kSource), kPeriod);
}

TEST(VerticalBlankClockTest, WillNotExtrapolateAGridBeyondTheWindowItWasMeasuredIn) {
    VerticalBlankClock clock;
    FeedBlanks(clock, 12);
    const int64_t anchor = kBase + 11 * kPeriod;
    EXPECT_EQ(clock.Snap(kSource, anchor + 500 * kPeriod), 0);
    EXPECT_EQ(clock.Snap(kSource, anchor - 500 * kPeriod), 0);
    // Just inside the bound still answers, so the limit is a guard rather than
    // a working constraint.
    EXPECT_EQ(clock.Snap(kSource, anchor + 8 * kPeriod), anchor + 8 * kPeriod);
}

TEST(VerticalBlankClockTest, StopsPlacingWhenTheGridKeepsFailingToAnswer) {
    VerticalBlankClock clock;
    FeedBlanks(clock, 12);
    const int64_t anchor = kBase + 11 * kPeriod;
    EXPECT_TRUE(clock.CanPlaceFrames(kSource));
    for (std::size_t i = 0; i < BlankCadence::kMinimumClaims; ++i)
        EXPECT_EQ(clock.Claim(kSource, anchor + 500 * kPeriod), 0);
    EXPECT_FALSE(clock.CanPlaceFrames(kSource));
    // A grid that has stopped answering does not get to round the completions
    // it still can: half a rounded series is what mixes two quantities.
    EXPECT_EQ(clock.Claim(kSource, anchor + kPeriod + 3920), 0);
}

TEST(VerticalBlankClockTest, StartsPlacingAgainOnceTheGridAnswersAgain) {
    VerticalBlankClock clock;
    FeedBlanks(clock, 12);
    const int64_t anchor = kBase + 11 * kPeriod;
    for (std::size_t i = 0; i < BlankCadence::kMinimumClaims; ++i)
        clock.Claim(kSource, anchor + 500 * kPeriod);
    ASSERT_FALSE(clock.CanPlaceFrames(kSource));
    // Completions the grid can answer for are still attempted while it is
    // refusing, which is what lets the record recover at all.
    for (std::size_t i = 0; i < 2 * BlankCadence::kWindowClaims; ++i)
        clock.Claim(kSource, anchor + 3920);
    EXPECT_TRUE(clock.CanPlaceFrames(kSource));
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

// The reported Talos case: variable refresh below the panel's cap. Rounding
// only the completions a hole-ridden blank stream can answer for published
// screen times and flip-latch times in one series, which measured worse than
// either - so under variable refresh nothing is rounded and the series stays
// in the driver's own units.
TEST(DisplayTimingResolveDeferredScreenTimesTest, LeavesVariableRefreshCompletionsInOneUnit) {
    VerticalBlankClock clock;
    int64_t now = kBase;
    for (int64_t gap : {11900, 51100, 9460, 117472, 23000, 9880, 46000}) {
        clock.Observe(kSource, now);
        now += gap;
    }
    std::vector<PendingTimestamp> pending;
    pending.push_back(SyncEntry(kBase + 3920));
    pending.push_back(SyncEntry(kBase + 14000));
    EXPECT_EQ(ResolveDeferredScreenTimes(pending, clock), 0u);
    EXPECT_EQ(pending[0].timestamp, kBase + 3920);
    EXPECT_EQ(pending[1].timestamp, kBase + 14000);
    EXPECT_FALSE(pending[0].screenTimeResolved);
    EXPECT_FALSE(pending[1].screenTimeResolved);
}

TEST(DisplayTimingResolveDeferredScreenTimesTest, LeavesTheTimestampAloneWithoutABlankStream) {
    VerticalBlankClock clock;
    std::vector<PendingTimestamp> pending;
    pending.push_back(SyncEntry(kBase + 3920));
    EXPECT_EQ(ResolveDeferredScreenTimes(pending, clock), 0u);
    EXPECT_EQ(pending[0].timestamp, kBase + 3920);
    EXPECT_FALSE(pending[0].screenTimeResolved);
}
