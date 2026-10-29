#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "common/platform/display_timing_etw.h"
#include "common/platform/present_progress.h"
#include "tests/source_fragment_reader.h"

namespace {
namespace progress = ce::present_progress;

constexpr uint64_t kThresholdUs = 2'000'000;
constexpr DWORD kGame = 100;
constexpr DWORD kOther = 200;

// A tracker following `processes`, on a 1 MHz clock.
struct Tracked : progress::Tracker {
    explicit Tracked(std::initializer_list<DWORD> processes = {kGame}) : progress::Tracker(1'000'000) {
        Track(processes);
    }
};

TEST(PresentProgressTest, AnUnansweredPresentBeyondTheThresholdStallsUntilItReturns) {
    Tracked tracker;
    tracker.ObserveStart(kGame, 7, 1'000, false);
    tracker.ObserveReturn(kGame, 7, 1'010);
    tracker.ObserveStart(kGame, 7, 34'000, true);
    EXPECT_FALSE(tracker.Inspect(kGame, 34'000 + kThresholdUs - 1, kThresholdUs).stalled);
    const progress::Stall stall = tracker.Inspect(kGame, 34'000 + kThresholdUs, kThresholdUs);
    ASSERT_TRUE(stall.stalled);
    EXPECT_EQ(stall.threadId, 7u);
    EXPECT_EQ(stall.openUs, kThresholdUs);
    EXPECT_EQ(stall.lastIntervalUs, 33'000u);
    EXPECT_TRUE(stall.multiplane);
    EXPECT_EQ(tracker.Returns(kGame), 1u);
    tracker.ObserveReturn(kGame, 7, 9'000'000);
    EXPECT_FALSE(tracker.Inspect(kGame, 20'000'000, kThresholdUs).stalled);
    EXPECT_EQ(tracker.Returns(kGame), 2u);
}

TEST(PresentProgressTest, APausedOrThrottledGameIsNeverStalled) {
    Tracked tracker;
    for (int64_t frame = 0; frame < 30; ++frame) {
        tracker.ObserveStart(kGame, 7, frame * 33'333, false);
        tracker.ObserveReturn(kGame, 7, frame * 33'333 + 500);
    }
    // Background pause: no Present call at all for a minute.
    EXPECT_FALSE(tracker.Inspect(kGame, 60'000'000, kThresholdUs).stalled);
}

TEST(PresentProgressTest, AServiceThatDeliversNoReturnsCannotProduceAStall) {
    Tracked tracker;
    tracker.ObserveStart(kGame, 7, 0, false);
    EXPECT_FALSE(tracker.Inspect(kGame, 60'000'000, kThresholdUs).stalled);
    // Any process's return proves the provider set carries returns.
    tracker.ObserveReturn(kOther, 9, 10);
    EXPECT_TRUE(tracker.Inspect(kGame, 60'000'000, kThresholdUs).stalled);
    EXPECT_EQ(tracker.Returns(kOther), 0u);
    EXPECT_EQ(tracker.Snapshot().returns, 0u);
}

TEST(PresentProgressTest, UntrackedProcessesAndDroppedProcessesLeaveNoState) {
    Tracked tracker({kGame, kOther});
    tracker.ObserveReturn(kOther, 9, 0);
    tracker.ObserveStart(kOther, 9, 1, false);
    tracker.ObserveStart(300, 9, 1, false);
    tracker.Track({kGame});
    EXPECT_FALSE(tracker.Inspect(kOther, 60'000'000, kThresholdUs).stalled);
    EXPECT_FALSE(tracker.Inspect(300, 60'000'000, kThresholdUs).stalled);
    tracker.Track({kGame, kOther});
    EXPECT_FALSE(tracker.Inspect(kOther, 60'000'000, kThresholdUs).stalled);
    EXPECT_EQ(tracker.Returns(kOther), 0u);
}

TEST(PresentProgressTest, LostEventsForgetOpenCallsInsteadOfInventingAStall) {
    Tracked tracker;
    tracker.ObserveReturn(kGame, 7, 0);
    tracker.ObserveStart(kGame, 7, 10, false);
    tracker.ObserveLoss();
    EXPECT_FALSE(tracker.Inspect(kGame, 60'000'000, kThresholdUs).stalled);
    EXPECT_EQ(tracker.Snapshot().losses, 1u);
    tracker.ObserveStart(kGame, 7, 100, false);
    EXPECT_TRUE(tracker.Inspect(kGame, 60'000'000, kThresholdUs).stalled);
}

TEST(PresentProgressTest, TheLongestOpenThreadOfTheProcessIsReported) {
    Tracked tracker;
    tracker.ObserveReturn(kGame, 1, 0);
    tracker.ObserveStart(kGame, 1, 3'000'000, false);
    tracker.ObserveStart(kGame, 2, 1'000'000, true);
    const progress::Stall stall = tracker.Inspect(kGame, 10'000'000, kThresholdUs);
    ASSERT_TRUE(stall.stalled);
    EXPECT_EQ(stall.threadId, 2u);
    EXPECT_EQ(stall.openUs, 9'000'000u);
}

TEST(PresentProgressTest, ThreadCapEvictsQuietReturnedThreadsBeforeOpenOnes) {
    Tracked tracker;
    tracker.ObserveReturn(kGame, 1, 0);
    tracker.ObserveStart(kGame, 2, 5, false);  // the stalled render thread
    for (DWORD thread = 10; thread < 10 + progress::Tracker::kMaxThreadsPerProcess * 2; ++thread) {
        tracker.ObserveStart(kGame, thread, 100 + thread, false);
        tracker.ObserveReturn(kGame, thread, 101 + thread);
    }
    const progress::Stall stall = tracker.Inspect(kGame, 10'000'000, kThresholdUs);
    ASSERT_TRUE(stall.stalled);
    EXPECT_EQ(stall.threadId, 2u);
}

TEST(PresentProgressTest, ResetForgetsOpenCallsButKeepsReturnCountsMonotonic) {
    Tracked tracker;
    tracker.ObserveReturn(kGame, 7, 0);
    tracker.ObserveStart(kGame, 7, 10, false);
    tracker.Reset();
    EXPECT_FALSE(tracker.Inspect(kGame, 60'000'000, kThresholdUs).stalled);
    EXPECT_EQ(tracker.Returns(kGame), 1u);
    EXPECT_FALSE(tracker.Snapshot().returnsDelivered);
}

TEST(PresentProgressTest, SharedProviderSetDeliversPresentReturns) {
    EXPECT_EQ(display_timing_etw::kRuntimePresentStop, 0x2b);
    EXPECT_EQ(display_timing_etw::kRuntimeMpoPresentStop, 0x38);
    const std::string etw = ce::test_source::ReadFile(std::filesystem::current_path() / "common" / "platform" /
                                                      "display_timing_etw.h");
    const size_t providerSet = etw.find("inline DisplayTimingProviderStatus EnableDisplayTimingProviders(");
    ASSERT_NE(providerSet, std::string::npos);
    const size_t runtime = etw.find("kRuntimeProvider, kRuntimeKeyword", providerSet);
    ASSERT_NE(runtime, std::string::npos);
    const size_t end = etw.find(");", runtime);
    const std::string filter = etw.substr(runtime, end - runtime);
    EXPECT_NE(filter.find("kRuntimePresentStop"), std::string::npos);
    EXPECT_NE(filter.find("kRuntimeMpoPresentStop"), std::string::npos);
}

}  // namespace
