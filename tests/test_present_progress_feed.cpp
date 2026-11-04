#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

#include "captureengine/display_timing/present_progress_feed.h"
#include "captureengine/elevation/elevation_client.h"
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

struct FakeEtw {
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE stopped = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool serviceConfigured = true;
    bool serviceAcquire = true;
    int serviceAcquired = 0;
    int serviceReleased = 0;

    ULONG ownOpenStatus = ERROR_SUCCESS;
    int ownOpened = 0;
    int ownStopped = 0;
    std::wstring lastOpenedSessionName;
    std::wstring lastStoppedSessionName;

    bool openSucceeds = true;
    std::wstring lastLoggerName;
    ULONG consumeResult = ERROR_SUCCESS;
    int consumed = 0;
    int closed = 0;

    std::vector<std::string> sessionActionLog;

    ~FakeEtw() {
        CloseHandle(entered);
        CloseHandle(stopped);
    }

    void ResetEvents() {
        ResetEvent(entered);
        ResetEvent(stopped);
    }

    progress::TraceEnvironment Environment() {
        return {
            [this] { return serviceConfigured; },
            [this] {
                if (serviceAcquire) {
                    ++serviceAcquired;
                    return true;
                }
                return false;
            },
            [this] { ++serviceReleased; },
            [this](TRACEHANDLE* session, const wchar_t* name) -> ULONG {
                ++ownOpened;
                sessionActionLog.push_back("open");
                if (name)
                    lastOpenedSessionName = name;
                if (ownOpenStatus == ERROR_SUCCESS) {
                    *session = TRACEHANDLE{1001};
                    return ERROR_SUCCESS;
                }
                *session = 0;
                return ownOpenStatus;
            },
            [this](TRACEHANDLE session, const wchar_t* name) {
                EXPECT_EQ(session, TRACEHANDLE{1001});
                sessionActionLog.push_back("stop");
                if (name)
                    lastStoppedSessionName = name;
                ++ownStopped;
            },
            [this](EVENT_TRACE_LOGFILEW* trace) {
                if (trace && trace->LoggerName) {
                    lastLoggerName = trace->LoggerName;
                }
                if (!openSucceeds) {
                    SetLastError(ERROR_FILE_NOT_FOUND);
                    return INVALID_PROCESSTRACE_HANDLE;
                }
                return TRACEHANDLE{42};
            },
            [this](TRACEHANDLE handle) {
                EXPECT_EQ(handle, TRACEHANDLE{42});
                ++consumed;
                SetEvent(entered);
                EXPECT_EQ(WaitForSingleObject(stopped, 10000), WAIT_OBJECT_0);
                return consumeResult;
            },
            [this](TRACEHANDLE handle) {
                EXPECT_EQ(handle, TRACEHANDLE{42});
                ++closed;
                SetEvent(stopped);
            }};
    }
};

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

TEST(PresentProgressFeedTest, LeaseGrantedUsesServiceRoute) {
    FakeEtw fake;
    progress::TraceFeed feed(fake.Environment());
    progress::Tracker tracker(1'000'000);
    ASSERT_TRUE(feed.Start(tracker));
    ASSERT_EQ(WaitForSingleObject(fake.entered, 10000), WAIT_OBJECT_0);
    EXPECT_EQ(fake.lastLoggerName, ce::elevation::kTraceName);
    EXPECT_EQ(fake.ownOpened, 0);
    EXPECT_TRUE(feed.Running());
    feed.Stop();
    EXPECT_EQ(fake.serviceReleased, 1);
    EXPECT_EQ(fake.ownStopped, 0);
    EXPECT_FALSE(feed.Running());
}

TEST(PresentProgressFeedTest, ServiceNotConfiguredOpensOwnSession) {
    FakeEtw fake;
    fake.serviceConfigured = false;
    progress::TraceFeed feed(fake.Environment());
    progress::Tracker tracker(1'000'000);
    ASSERT_TRUE(feed.Start(tracker));
    ASSERT_EQ(WaitForSingleObject(fake.entered, 10000), WAIT_OBJECT_0);
    wchar_t expectedName[64];
    swprintf_s(expectedName, L"CE_PresentProgress_%08X", static_cast<unsigned int>(GetCurrentProcessId()));
    EXPECT_EQ(fake.serviceAcquired, 0);
    EXPECT_EQ(fake.ownOpened, 1);
    EXPECT_EQ(fake.lastOpenedSessionName, expectedName);
    EXPECT_EQ(fake.lastLoggerName, expectedName);
    EXPECT_TRUE(feed.Running());
    feed.Stop();
    EXPECT_EQ(fake.closed, 1);
    EXPECT_EQ(fake.ownStopped, 1);
    EXPECT_EQ(fake.lastStoppedSessionName, expectedName);
    EXPECT_FALSE(feed.Running());
}

TEST(PresentProgressFeedTest, LeaseRefusedFallsThroughToOwnSession) {
    FakeEtw fake;
    fake.serviceConfigured = true;
    fake.serviceAcquire = false;
    progress::TraceFeed feed(fake.Environment());
    progress::Tracker tracker(1'000'000);
    ASSERT_TRUE(feed.Start(tracker));
    ASSERT_EQ(WaitForSingleObject(fake.entered, 10000), WAIT_OBJECT_0);
    EXPECT_EQ(fake.serviceAcquired, 0);
    EXPECT_EQ(fake.serviceReleased, 1);
    EXPECT_EQ(fake.ownOpened, 1);
    EXPECT_TRUE(feed.Running());
    feed.Stop();
    EXPECT_EQ(fake.ownStopped, 1);
    EXPECT_FALSE(feed.Running());
}

TEST(PresentProgressFeedTest, OpenFailsOnServiceRouteFallsThroughToOwnSession) {
    FakeEtw fake;
    fake.serviceConfigured = true;
    fake.serviceAcquire = true;
    progress::TraceEnvironment env = fake.Environment();
    env.open = [&fake, openCount = 0](EVENT_TRACE_LOGFILEW* trace) mutable {
        ++openCount;
        if (openCount == 1) {
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_PROCESSTRACE_HANDLE;
        }
        if (trace && trace->LoggerName) {
            fake.lastLoggerName = trace->LoggerName;
        }
        return TRACEHANDLE{42};
    };
    progress::TraceFeed feed(std::move(env));
    progress::Tracker tracker(1'000'000);
    ASSERT_TRUE(feed.Start(tracker));
    ASSERT_EQ(WaitForSingleObject(fake.entered, 10000), WAIT_OBJECT_0);
    EXPECT_EQ(fake.serviceReleased, 1);
    EXPECT_EQ(fake.ownOpened, 1);
    EXPECT_TRUE(feed.Running());
    feed.Stop();
    EXPECT_EQ(fake.ownStopped, 1);
    EXPECT_FALSE(feed.Running());
}

TEST(PresentProgressFeedTest, OwnSessionAccessDeniedNotRetriedOnSubsequentStart) {
    FakeEtw fake;
    fake.serviceConfigured = false;
    fake.ownOpenStatus = ERROR_ACCESS_DENIED;
    progress::TraceFeed feed(fake.Environment());
    progress::Tracker tracker(1'000'000);
    EXPECT_FALSE(feed.Start(tracker));
    EXPECT_EQ(fake.ownOpened, 1);
    EXPECT_EQ(fake.ownStopped, 0);
    EXPECT_FALSE(feed.Running());

    fake.serviceConfigured = true;
    fake.serviceAcquire = false;
    EXPECT_FALSE(feed.Start(tracker));
    EXPECT_EQ(fake.serviceReleased, 1);
    EXPECT_EQ(fake.ownOpened, 1);
}

TEST(PresentProgressFeedTest, OwnSession1450RetriedOnNextStart) {
    FakeEtw fake;
    fake.serviceConfigured = false;
    fake.ownOpenStatus = 1450;
    progress::TraceFeed feed(fake.Environment());
    progress::Tracker tracker(1'000'000);
    EXPECT_FALSE(feed.Start(tracker));
    EXPECT_EQ(fake.ownOpened, 1);
    EXPECT_FALSE(feed.Running());

    EXPECT_FALSE(feed.Start(tracker));
    EXPECT_EQ(fake.ownOpened, 2);
}

TEST(PresentProgressFeedTest, OpenFailsOnOwnSessionStopsSessionOnce) {
    FakeEtw fake;
    fake.serviceConfigured = false;
    fake.openSucceeds = false;
    progress::TraceFeed feed(fake.Environment());
    progress::Tracker tracker(1'000'000);
    EXPECT_FALSE(feed.Start(tracker));
    EXPECT_EQ(fake.ownOpened, 1);
    EXPECT_EQ(fake.ownStopped, 1);
    EXPECT_FALSE(feed.Running());
}

TEST(PresentProgressFeedTest, ConsumerEndsOnItsOwnNextStartStopsOldSessionBeforeOpeningNew) {
    FakeEtw fake;
    fake.serviceConfigured = false;
    HANDLE allowConsumeReturn = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE consumeReturned = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    progress::TraceEnvironment env = fake.Environment();
    env.consume = [&fake, allowConsumeReturn, consumeReturned](TRACEHANDLE handle) {
        EXPECT_EQ(handle, TRACEHANDLE{42});
        ++fake.consumed;
        SetEvent(fake.entered);
        EXPECT_EQ(WaitForSingleObject(allowConsumeReturn, 10000), WAIT_OBJECT_0);
        SetEvent(consumeReturned);
        return ERROR_SUCCESS;
    };
    progress::TraceFeed feed(std::move(env));
    progress::Tracker tracker(1'000'000);
    ASSERT_TRUE(feed.Start(tracker));
    ASSERT_EQ(WaitForSingleObject(fake.entered, 10000), WAIT_OBJECT_0);
    EXPECT_TRUE(feed.Running());

    SetEvent(allowConsumeReturn);
    ASSERT_EQ(WaitForSingleObject(consumeReturned, 10000), WAIT_OBJECT_0);

    while (feed.Running()) {
        std::this_thread::yield();
    }
    EXPECT_FALSE(feed.Running());
    EXPECT_EQ(fake.ownStopped, 0);

    fake.sessionActionLog.clear();
    fake.ResetEvents();
    ASSERT_TRUE(feed.Start(tracker));
    ASSERT_EQ(WaitForSingleObject(fake.entered, 10000), WAIT_OBJECT_0);
    EXPECT_EQ(fake.sessionActionLog, (std::vector<std::string>{"stop", "open"}));
    feed.Stop();

    CloseHandle(allowConsumeReturn);
    CloseHandle(consumeReturned);
}

TEST(PresentProgressFeedTest, OwnSessionRunStopServiceNowAvailableUsesService) {
    FakeEtw fake;
    fake.serviceConfigured = false;
    progress::TraceFeed feed(fake.Environment());
    progress::Tracker tracker(1'000'000);
    ASSERT_TRUE(feed.Start(tracker));
    ASSERT_EQ(WaitForSingleObject(fake.entered, 10000), WAIT_OBJECT_0);
    EXPECT_EQ(fake.ownOpened, 1);
    EXPECT_TRUE(feed.Running());
    feed.Stop();
    EXPECT_EQ(fake.ownStopped, 1);
    EXPECT_FALSE(feed.Running());

    fake.ResetEvents();
    fake.serviceConfigured = true;
    fake.serviceAcquire = true;
    ASSERT_TRUE(feed.Start(tracker));
    ASSERT_EQ(WaitForSingleObject(fake.entered, 10000), WAIT_OBJECT_0);
    EXPECT_EQ(fake.lastLoggerName, ce::elevation::kTraceName);
    EXPECT_EQ(fake.ownOpened, 1);
    EXPECT_TRUE(feed.Running());
    feed.Stop();
    EXPECT_EQ(fake.serviceReleased, 1);
    EXPECT_FALSE(feed.Running());
}

}  // namespace
