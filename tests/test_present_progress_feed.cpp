#include <gtest/gtest.h>

#include "captureengine/display_timing/present_progress_feed.h"
#include "common/platform/display_timing_etw.h"

namespace {
namespace progress = ce::present_progress;
namespace dte = display_timing_etw;

constexpr uint64_t kThresholdUs = 2'000'000;

EVENT_RECORD Event(const GUID& provider, USHORT id, DWORD processId, DWORD threadId, int64_t qpc) {
    EVENT_RECORD event = {};
    event.EventHeader.ProviderId = provider;
    event.EventHeader.EventDescriptor.Id = id;
    event.EventHeader.ProcessId = processId;
    event.EventHeader.ThreadId = threadId;
    event.EventHeader.TimeStamp.QuadPart = qpc;
    return event;
}

TEST(PresentProgressFeedTest, RuntimePresentStartsAndReturnsReachTheTracker) {
    progress::Tracker tracker(1'000'000);
    tracker.Track({100});
    progress::TraceFeed::Route(Event(dte::kRuntimeProvider, dte::kRuntimePresentStart, 100, 7, 0), tracker);
    progress::TraceFeed::Route(Event(dte::kRuntimeProvider, dte::kRuntimePresentStop, 100, 7, 10), tracker);
    progress::TraceFeed::Route(Event(dte::kRuntimeProvider, dte::kRuntimeMpoPresentStart, 100, 7, 20), tracker);
    const progress::Stall stall = tracker.Inspect(100, 20 + kThresholdUs, kThresholdUs);
    ASSERT_TRUE(stall.stalled);
    EXPECT_TRUE(stall.multiplane);
    progress::TraceFeed::Route(Event(dte::kRuntimeProvider, dte::kRuntimeMpoPresentStop, 100, 7, 30), tracker);
    EXPECT_FALSE(tracker.Inspect(100, 60'000'000, kThresholdUs).stalled);
    EXPECT_EQ(tracker.Returns(100), 2u);
}

TEST(PresentProgressFeedTest, KernelAndOtherProviderEventsAreIgnored) {
    progress::Tracker tracker(1'000'000);
    tracker.Track({100});
    progress::TraceFeed::Route(Event(dte::kRuntimeProvider, dte::kRuntimePresentStop, 999, 1, 0), tracker);
    // Same event ids from the graphics kernel and an unrelated runtime event change nothing.
    progress::TraceFeed::Route(Event(dte::kGraphicsKernelProvider, dte::kRuntimePresentStart, 100, 7, 0), tracker);
    progress::TraceFeed::Route(Event(dte::kWin32kProvider, dte::kRuntimePresentStart, 100, 7, 0), tracker);
    progress::TraceFeed::Route(Event(dte::kRuntimeProvider, 0x2c, 100, 7, 0), tracker);
    EXPECT_FALSE(tracker.Inspect(100, 60'000'000, kThresholdUs).stalled);
    EXPECT_EQ(tracker.Snapshot().starts, 0u);
}

}  // namespace
