#include <gtest/gtest.h>
#include <thread>
#include "../hook/common/pacing_trace_boundary.h"
#include "../hook/common/present_heartbeat.h"

namespace {
using namespace ce::pacing_trace;
struct FakeBackend {
    static inline bool enabled = true;
    static inline int64_t time = 100;
    static inline uint64_t id = 0;
    static inline unsigned clockReads = 0;
    static inline std::array<Event, 8> events;
    static inline size_t eventCount = 0;
    static bool Enabled() { return enabled; }
    static uint64_t NextId() { return ++id; }
    static int64_t Now() { ++clockReads; return time; }
    static void Write(Kind kind, uint64_t token, const void* object, uint64_t a, uint64_t b,
                      uint64_t c, uint32_t flags, int64_t timestamp) {
        if (eventCount < events.size())
            events[eventCount++] = {timestamp, 0, token, reinterpret_cast<uintptr_t>(object), a, b, c, 1, flags, kind};
    }
    static void Reset(bool enable = true) {
        enabled = enable; time = 100; id = 0; clockReads = 0; eventCount = 0; events = {};
    }
};
using Scope = BoundaryScope<FakeBackend>;

TEST(PacingPresentBoundaryTest, DisabledTraceReadsNoClockAndEmitsNothing) {
    FakeBackend::Reset(false);
    { Scope scope(PresentStage::Proxy, nullptr, 1, 2); scope.Forward(0, 4); scope.Finish(0); }
    EXPECT_EQ(FakeBackend::eventCount, 0u);
    EXPECT_EQ(FakeBackend::clockReads, 0u);
    EXPECT_EQ(FakeBackend::id, 0u);
}

TEST(PacingPresentBoundaryTest, CapturesInputForwardedIntentAndExactReturnOnce) {
    FakeBackend::Reset();
    {
        Scope scope(PresentStage::Proxy1, nullptr, 1, 2);
        FakeBackend::time = 120;
        scope.Forward(0, 512);
        FakeBackend::time = 180;
        scope.Finish(0x887A0001u);
        FakeBackend::time = 200;
    }
    ASSERT_EQ(FakeBackend::eventCount, 3u);
    const auto& events = FakeBackend::events;
    EXPECT_EQ(events[0].kind, Kind::PresentBegin);
    EXPECT_EQ(events[0].a, 1u);
    EXPECT_EQ(events[0].b, 2u);
    EXPECT_EQ(events[1].kind, Kind::PresentForward);
    EXPECT_EQ(events[1].a, 0u);
    EXPECT_EQ(events[1].b, 512u);
    EXPECT_EQ(events[2].kind, Kind::PresentEnd);
    EXPECT_EQ(events[2].timeUs, 180);
    EXPECT_EQ(events[2].a, 80u);
    EXPECT_EQ(events[2].b, 0x887A0001u);
    EXPECT_EQ(events[2].c, 1u);
    for (size_t i = 0; i < FakeBackend::eventCount; ++i) {
        const auto& event = events[i];
        EXPECT_EQ(event.id, 1u);
        EXPECT_EQ(event.flags, static_cast<uint32_t>(PresentStage::Proxy1));
    }
}

TEST(PacingPresentBoundaryTest, NestedEarlyReturnsHaveDistinctPairedIdsAndUnknownResults) {
    FakeBackend::Reset();
    {
        Scope outer(PresentStage::Detour, nullptr, 0, 0);
        { Scope inner(PresentStage::Forward, nullptr, 0, 0); FakeBackend::time = 140; }
        FakeBackend::time = 160;
    }
    ASSERT_EQ(FakeBackend::eventCount, 4u);
    EXPECT_NE(FakeBackend::events[0].id, FakeBackend::events[1].id);
    EXPECT_EQ(FakeBackend::events[0].id, FakeBackend::events[3].id);
    EXPECT_EQ(FakeBackend::events[1].id, FakeBackend::events[2].id);
    EXPECT_EQ(FakeBackend::events[2].a, 40u);
    EXPECT_EQ(FakeBackend::events[3].a, 60u);
    EXPECT_EQ(FakeBackend::events[2].c, 0u);
    EXPECT_EQ(FakeBackend::events[3].c, 0u);
}

TEST(PresentHeartbeatTest, LateOrDuplicateObserversCannotRegressTheTimestamp) {
    ce::PresentHeartbeat heartbeat;
    EXPECT_EQ(heartbeat.Observe(100).gapUs, 0);
    EXPECT_EQ(heartbeat.Observe(200).gapUs, 100);
    EXPECT_EQ(heartbeat.Observe(150).gapUs, 0);
    EXPECT_EQ(heartbeat.Observe(200).gapUs, 0);
    EXPECT_EQ(heartbeat.Observe(250).gapUs, 50);
    EXPECT_EQ(heartbeat.Observe(500000).gapUs, 499750);
    EXPECT_EQ(heartbeat.Observe(500001).count, 6u);
}

TEST(PresentHeartbeatTest, ConcurrentObserversKeepExactCountAndMonotonicProgress) {
    ce::PresentHeartbeat heartbeat;
    const auto observe = [&]() { for (int i = 1; i <= 10000; ++i) heartbeat.Observe(i); };
    std::thread a(observe), b(observe);
    a.join(); b.join();
    // Final timestamp is 10000: its first publisher either wins, or loses to
    // the other observer publishing that same timestamp. No timing assumptions.
    const auto final = heartbeat.Observe(10001);
    EXPECT_EQ(final.count, 20000u);
    EXPECT_EQ(final.gapUs, 1);
}
}  // namespace
