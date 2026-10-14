#include <gtest/gtest.h>

#include "captureengine/injection/process_start_etw.h"
#include "common/ipc/elevation_trace_lease.h"

// Exercise the production consumer without a privileged live trace.
namespace {

using namespace ce::process_start;

TEST(ProcessStartEtwLifecycle, LegacyOrUnavailableProviderReleasesLease) {
    for (bool querySucceeds : {false, true}) {
        int acquired = 0, released = 0;
        EXPECT_FALSE(ce::elevation::AcquireProcessTraceLease(
            [&] { ++acquired; return true; },
            [&](ce::elevation::Capabilities&) { return querySucceeds; },
            [&] { ++released; }));
        EXPECT_EQ(acquired, 1);
        EXPECT_EQ(released, 1);
    }
}

TEST(ProcessStartEtwLifecycle, ConfirmedProviderRetainsLeaseAndFailedAcquireDoesNotQuery) {
    int queried = 0, released = 0;
    const auto query = [&](ce::elevation::Capabilities& capabilities) {
        ++queried;
        capabilities.features |= ce::elevation::kProcessStartTraceCapability;
        return true;
    };
    EXPECT_FALSE(ce::elevation::AcquireProcessTraceLease([] { return false; }, query, [&] { ++released; }));
    EXPECT_EQ(queried, 0);
    EXPECT_TRUE(ce::elevation::AcquireProcessTraceLease([] { return true; }, query, [&] { ++released; }));
    EXPECT_EQ(queried, 1);
    EXPECT_EQ(released, 0);
}

struct Session {
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE stopped = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    int acquired = 0, closed = 0, released = 0;
    ULONG result = ERROR_WMI_INSTANCE_NOT_FOUND;
    ~Session() { CloseHandle(entered); CloseHandle(stopped); }
    TraceOperations Operations() {
        return {
            [&] { ++acquired; return true; },
            [](EVENT_TRACE_LOGFILEW*) { return TRACEHANDLE{42}; },
            [&](TRACEHANDLE handle) {
                EXPECT_EQ(handle, 42u);
                SetEvent(entered);
                EXPECT_EQ(WaitForSingleObject(stopped, 10000), WAIT_OBJECT_0);
                return result;
            },
            [&](TRACEHANDLE handle) { EXPECT_EQ(handle, 42u); ++closed; SetEvent(stopped); },
            [&] { ++released; }};
    }
};

TEST(ProcessStartEtwLifecycle, ShutdownJoinsConsumerAndReleasesExactlyOnce) {
    Session session;
    TraceConsumer monitor(session.Operations());
    EVENT_TRACE_LOGFILEW trace{};
    ASSERT_TRUE(monitor.Start(&trace));
    ASSERT_EQ(WaitForSingleObject(session.entered, 10000), WAIT_OBJECT_0);
    monitor.Stop();
    monitor.Stop();
    EXPECT_FALSE(monitor.IsRunning());
    EXPECT_EQ(session.acquired, 1);
    EXPECT_EQ(session.closed, 1);
    EXPECT_EQ(session.released, 1);
}

TEST(ProcessStartEtwLifecycle, FailedConsumerHandsOffOnOwnerThreadAndRescansOnlyOnce) {
    Session session;
    TraceConsumer monitor(session.Operations());
    EVENT_TRACE_LOGFILEW trace{};
    bool selected = monitor.Start(&trace);
    ASSERT_TRUE(selected);
    ASSERT_EQ(WaitForSingleObject(session.entered, 10000), WAIT_OBJECT_0);
    std::vector<std::string> calls;
    const auto service = [&](bool running, bool fallbackWorks) {
        return RecoverStoppedTrace(selected, running,
            [&] { monitor.Stop(); calls.emplace_back("stop"); },
            [&] { calls.emplace_back("fallback"); return fallbackWorks; },
            [&] { calls.emplace_back("rescan"); });
    };
    EXPECT_FALSE(service(true, true));
    EXPECT_TRUE(calls.empty());
    SetEvent(session.stopped);
    // Joining is the deterministic consumer-completion barrier.
    monitor.Stop();
    ASSERT_FALSE(monitor.IsRunning());
    EXPECT_FALSE(service(false, false));
    EXPECT_TRUE(selected);
    EXPECT_TRUE(service(false, true));
    EXPECT_FALSE(service(false, true));
    EXPECT_EQ(calls, (std::vector<std::string>{"stop", "fallback", "stop", "fallback", "rescan"}));
    EXPECT_EQ(session.closed, 1);
    EXPECT_EQ(session.released, 1);
}

}  // namespace
