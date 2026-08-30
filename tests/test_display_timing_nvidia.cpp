#include "../captureengine/display_timing_nvidia.h"
#include <vector>
#include <gtest/gtest.h>

namespace {
constexpr uint32_t kThread = 0x1234;
constexpr uint32_t kOtherThread = 0x5678;
constexpr uint32_t kHead = 0;
}  // namespace

// The whole point of the tracker: the flip event fires when the driver programs
// the flip, the announcement says when it will actually scan out.
TEST(DisplayTimingNvidiaTest, AnnouncedScreenTimeBecomesTheDelayOverTheFlipEvent) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveFlipRequest(kThread, kHead, 0, 1000, 1700, 1);
    const NvidiaFlipDelay delay = tracker.TakeFlipDelay(kThread);
    EXPECT_TRUE(delay.matched);
    EXPECT_EQ(delay.delay, 700);
    EXPECT_EQ(delay.token, 1u);
}

// Frame generation paces several flips out of one render: the driver programs
// them close together and they scan out evenly. Without the announcement the
// published series follows the burst, which is the sawtooth this fixes.
TEST(DisplayTimingNvidiaTest, PacedFlipsProgrammedInABurstResolveToAnEvenScreenSeries) {
    NvidiaFlipDelayTracker tracker;
    const int64_t programmedAt[] = {1000, 1100, 1150, 1200};
    const int64_t announced[] = {2000, 2700, 3400, 4100};
    std::vector<int64_t> screenTimes;
    for (int i = 0; i < 4; ++i) {
        tracker.ObserveFlipRequest(kThread, kHead, 0, programmedAt[i], announced[i],
                                   static_cast<uint32_t>(i + 1));
        screenTimes.push_back(programmedAt[i] + tracker.TakeFlipDelay(kThread).delay);
    }
    ASSERT_EQ(screenTimes.size(), 4u);
    for (std::size_t i = 1; i < screenTimes.size(); ++i)
        EXPECT_EQ(screenTimes[i] - screenTimes[i - 1], 700) << "interval " << i;
}

TEST(DisplayTimingNvidiaTest, RepeatedTokenDoesNotReplaceTheOutstandingAnnouncement) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveFlipRequest(kThread, kHead, 0, 1000, 1500, 7);
    tracker.ObserveFlipRequest(kThread, kHead, 0, 1000, 9999, 7);
    EXPECT_EQ(tracker.TakeFlipDelay(kThread).delay, 500);
}

// A request that carries no announcement must not invent a delay, and it clears
// the head's baseline so a later announcement is taken at face value.
TEST(DisplayTimingNvidiaTest, RequestWithAllocationCarriesNoDelayAndResetsTheHead) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveFlipRequest(kThread, kHead, 0, 1000, 5000, 1);
    EXPECT_EQ(tracker.TakeFlipDelay(kThread).delay, 4000);
    tracker.ObserveFlipRequest(kThread, kHead, 0x8000, 2000, 6000, 2);
    EXPECT_EQ(tracker.TakeFlipDelay(kThread).delay, 0);
    tracker.ObserveFlipRequest(kThread, kHead, 0, 3000, 3200, 3);
    EXPECT_EQ(tracker.TakeFlipDelay(kThread).delay, 200);
}

TEST(DisplayTimingNvidiaTest, AnnouncementAlreadyInThePastYieldsNoDelay) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveFlipRequest(kThread, kHead, 0, 5000, 4000, 1);
    EXPECT_EQ(tracker.TakeFlipDelay(kThread).delay, 0);
}

// A head never scans out backwards: an announcement that predates the previous
// one on the same head is deferred to it instead of regressing screen time.
TEST(DisplayTimingNvidiaTest, AnnouncementBehindThePreviousOneIsDeferredToIt) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveFlipRequest(kThread, kHead, 0, 1000, 5000, 1);
    EXPECT_EQ(tracker.TakeFlipDelay(kThread).delay, 4000);
    tracker.ObserveFlipRequest(kThread, kHead, 0, 2000, 4500, 2);
    // Deferred to 5000, so the flip event at 2000 still resolves to 5000.
    EXPECT_EQ(tracker.TakeFlipDelay(kThread).delay, 500);
}

TEST(DisplayTimingNvidiaTest, HeadsKeepIndependentBaselines) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveFlipRequest(kThread, 0, 0, 1000, 9000, 1);
    EXPECT_EQ(tracker.TakeFlipDelay(kThread).delay, 8000);
    tracker.ObserveFlipRequest(kThread, 1, 0, 1000, 2000, 2);
    // The other head's later announcement must not defer this one.
    EXPECT_EQ(tracker.TakeFlipDelay(kThread).delay, 1000);
}

// Every outstanding announcement describes a flip that has already been
// programmed, so a consumed flip drops all of them rather than leaving one to
// be applied to an unrelated later flip.
TEST(DisplayTimingNvidiaTest, ConsumingOneFlipDropsEveryOutstandingAnnouncement) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveFlipRequest(kThread, kHead, 0, 1000, 1600, 1);
    tracker.ObserveFlipRequest(kOtherThread, kHead, 0, 1000, 2200, 2);
    EXPECT_EQ(tracker.TakeFlipDelay(kThread).delay, 600);
    EXPECT_EQ(tracker.pendingRequestCount(), 0u);
    EXPECT_FALSE(tracker.TakeFlipDelay(kOtherThread).matched);
}

TEST(DisplayTimingNvidiaTest, UnmatchedThreadKeepsOutstandingAnnouncementsForTheirOwnFlip) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveFlipRequest(kThread, kHead, 0, 1000, 1600, 1);
    const NvidiaFlipDelay miss = tracker.TakeFlipDelay(kOtherThread);
    EXPECT_FALSE(miss.matched);
    EXPECT_EQ(miss.delay, 0);
    EXPECT_EQ(tracker.TakeFlipDelay(kThread).delay, 600);
}

// Non-NVIDIA adapters never emit an announcement; the flip event timestamp must
// pass through completely unchanged.
TEST(DisplayTimingNvidiaTest, NoAnnouncementLeavesTheFlipTimestampUntouched) {
    NvidiaFlipDelayTracker tracker;
    const NvidiaFlipDelay delay = tracker.TakeFlipDelay(kThread);
    EXPECT_FALSE(delay.matched);
    EXPECT_EQ(delay.delay, 0);
}

TEST(DisplayTimingNvidiaTest, PruneDropsAnnouncementsOlderThanTheCutoff) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveFlipRequest(kThread, kHead, 0, 1000, 1600, 1);
    tracker.ObserveFlipRequest(kOtherThread, kHead, 0, 9000, 9600, 2);
    tracker.PruneBefore(5000);
    EXPECT_EQ(tracker.pendingRequestCount(), 1u);
    EXPECT_FALSE(tracker.TakeFlipDelay(kThread).matched);
    EXPECT_TRUE(tracker.TakeFlipDelay(kOtherThread).matched);
}

TEST(DisplayTimingNvidiaTest, ClearResetsBaselineAndTokenDeduplication) {
    NvidiaFlipDelayTracker tracker;
    tracker.ObserveFlipRequest(kThread, kHead, 0, 1000, 5000, 1);
    tracker.Clear();
    EXPECT_EQ(tracker.pendingRequestCount(), 0u);
    // The same token is accepted again, and the cleared baseline no longer
    // defers an earlier announcement.
    tracker.ObserveFlipRequest(kThread, kHead, 0, 1000, 1300, 1);
    EXPECT_EQ(tracker.TakeFlipDelay(kThread).delay, 300);
}
