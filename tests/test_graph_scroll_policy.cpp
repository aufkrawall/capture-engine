#include "../hook/common/graph_scroll_policy.h"
#include "../hook/common/performance_metrics.h"
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

// Returns the per-draw advance of the cursor for a given arrival pattern.
// `arrivals[i]` is how many samples landed before draw i.
template <std::size_t N>
std::vector<double> RunPattern(const std::array<int, N>& arrivals, int repeats) {
    GraphScrollCursor cursor;
    uint64_t samples = 0;
    std::vector<double> advances;
    double previous = 0.0;
    bool havePrevious = false;
    for (int r = 0; r < repeats; ++r) {
        for (int arrived : arrivals) {
            samples += static_cast<uint64_t>(arrived);
            const double position = cursor.Advance(samples);
            // Before the cursor arms there is nothing to plot, so those calls
            // are not part of the scroll the viewer sees.
            if (!cursor.primed())
                continue;
            if (havePrevious)
                advances.push_back(position - previous);
            previous = position;
            havePrevious = true;
        }
    }
    return advances;
}

// The measured 4x MFG pattern: the whole group of presents is drawn within
// about two milliseconds, so three draws in four see no new sample at all.
constexpr std::array<int, 4> kBurstPattern = {4, 0, 0, 0};
constexpr std::array<int, 1> kSteadyPattern = {1};

}  // namespace

// Presentation timing produces exactly one sample per drawn frame; the cursor
// must not disturb that case at all.
TEST(GraphScrollCursorTest, OneSamplePerDrawAdvancesExactlyOneSlot) {
    const auto advances = RunPattern(kSteadyPattern, 2000);
    ASSERT_GT(advances.size(), 1500u);
    // The correction settles with a fifty-draw time constant from the initial
    // arming, so steady state is checked past it rather than from draw one.
    for (std::size_t i = advances.size() / 2; i < advances.size(); ++i)
        EXPECT_NEAR(advances[i], 1.0, 1e-6) << "draw " << i;
}

// Even while the initial arming transient is settling the graph must not visibly
// change speed.
TEST(GraphScrollCursorTest, ArmingTransientStaysWithinAFewPercentOfOneSlot) {
    const auto advances = RunPattern(kSteadyPattern, 200);
    ASSERT_FALSE(advances.empty());
    for (std::size_t i = 0; i < advances.size(); ++i) {
        EXPECT_GT(advances[i], 0.95) << "draw " << i;
        EXPECT_LT(advances[i], 1.05) << "draw " << i;
    }
}

// The whole point: a burst arrival pattern must still scroll at a near-constant
// velocity instead of stepping 0, 0, 0, 4.
TEST(GraphScrollCursorTest, BurstArrivalsStillScrollAtNearConstantVelocity) {
    const auto advances = RunPattern(kBurstPattern, 200);
    ASSERT_GT(advances.size(), 400u);
    for (std::size_t i = advances.size() / 2; i < advances.size(); ++i) {
        EXPECT_GT(advances[i], 0.9) << "draw " << i;
        EXPECT_LT(advances[i], 1.1) << "draw " << i;
    }
}

// The trail is measured, not assumed: it must cost only the floor when every
// draw brings a sample, and grow to cover the group when they arrive in bursts.
TEST(GraphScrollCursorTest, TrailStaysAtTheFloorWhenSamplesArriveEveryDraw) {
    GraphScrollCursor cursor;
    for (uint64_t n = 2; n < 2000; ++n)
        cursor.Advance(n);
    EXPECT_NEAR(cursor.trail(), GraphScrollCursor::kMinTrailSlots, 1e-6);
}

TEST(GraphScrollCursorTest, TrailGrowsToCoverTheObservedBurstLength) {
    GraphScrollCursor cursor;
    uint64_t samples = 0;
    for (int r = 0; r < 400; ++r) {
        for (int arrived : kBurstPattern) {
            samples += static_cast<uint64_t>(arrived);
            cursor.Advance(samples);
        }
    }
    // Three dry draws per group of four, plus the floor.
    EXPECT_GT(cursor.trail(), 4.0);
    EXPECT_LT(cursor.trail(), 6.0);
}

TEST(GraphScrollCursorTest, TrailIsBoundedByAPathologicalStall) {
    GraphScrollCursor cursor;
    cursor.Advance(1000);
    for (int i = 0; i < 5000; ++i)
        cursor.Advance(1000);
    EXPECT_LE(cursor.trail(),
              static_cast<double>(GraphScrollCursor::kMaxDryStreak) + GraphScrollCursor::kMinTrailSlots);
}

TEST(GraphScrollCursorTest, CursorNeverMovesBackwards) {
    GraphScrollCursor cursor;
    uint64_t samples = 0;
    double previous = -1.0;
    for (int r = 0; r < 200; ++r) {
        for (int arrived : kBurstPattern) {
            samples += static_cast<uint64_t>(arrived);
            const double position = cursor.Advance(samples);
            EXPECT_GE(position, previous);
            previous = position;
        }
    }
}

// The caller plots one sample past the cursor, so that sample has to exist.
TEST(GraphScrollCursorTest, CursorStaysWithinTheWrittenSamples) {
    GraphScrollCursor cursor;
    uint64_t samples = 0;
    for (int r = 0; r < 200; ++r) {
        for (int arrived : kBurstPattern) {
            samples += static_cast<uint64_t>(arrived);
            const double position = cursor.Advance(samples);
            EXPECT_LE(std::floor(position) + 1.0, static_cast<double>(samples) - 1.0);
        }
    }
}

// Over a long run the cursor must not drift away from the stream it plots.
TEST(GraphScrollCursorTest, CursorTracksTheSampleStreamWithoutDrift) {
    GraphScrollCursor cursor;
    uint64_t samples = 0;
    double position = 0.0;
    for (int r = 0; r < 5000; ++r) {
        for (int arrived : kBurstPattern) {
            samples += static_cast<uint64_t>(arrived);
            position = cursor.Advance(samples);
        }
    }
    const double lag = static_cast<double>(samples) - position;
    EXPECT_GT(lag, 0.0);
    EXPECT_LT(lag, GraphScrollCursor::kResyncSlots);
}

// A source switch or history reset is a different stream, not drift.
// A stream that restarts far behind is a different series; the cursor re-arms
// on it rather than scrolling backwards into it over many frames.
TEST(GraphScrollCursorTest, RestartedStreamRearmsImmediately) {
    GraphScrollCursor cursor;
    for (uint64_t n = 2; n < 500; ++n)
        cursor.Advance(n);
    const double afterRestart = cursor.Advance(40);
    EXPECT_NEAR(afterRestart, 40.0 - 1.0 - GraphScrollCursor::kMinTrailSlots, 1e-6);
}

// A stream that jumps far ahead must be caught in one step, not crawled toward.
TEST(GraphScrollCursorTest, ForwardJumpSnapsToTheNewStream) {
    GraphScrollCursor cursor;
    for (uint64_t n = 2; n < 100; ++n)
        cursor.Advance(n);
    const double jumped = cursor.Advance(100'000);
    EXPECT_NEAR(jumped, 100'000.0 - 1.0 - GraphScrollCursor::kMinTrailSlots, 1e-6);
}

// Nothing can be plotted before two samples exist, and the cursor must arm
// itself cleanly once they do.
TEST(GraphScrollCursorTest, EmptyAndSingleSampleStreamsYieldNothing) {
    GraphScrollCursor cursor;
    EXPECT_EQ(cursor.Advance(0), 0.0);
    EXPECT_FALSE(cursor.primed());
    EXPECT_EQ(cursor.Advance(1), 0.0);
    EXPECT_FALSE(cursor.primed());
    cursor.Advance(8);
    EXPECT_TRUE(cursor.primed());
}

TEST(GraphScrollCursorTest, ResetRearmsAtTheCurrentStreamPosition) {
    GraphScrollCursor cursor;
    for (uint64_t n = 2; n < 300; ++n)
        cursor.Advance(n);
    cursor.Reset();
    EXPECT_FALSE(cursor.primed());
    const double primed = cursor.Advance(300);
    EXPECT_NEAR(primed, 300.0 - 1.0 - GraphScrollCursor::kMinTrailSlots, 1e-6);
}

// A stalled stream must hold the cursor where it is: it may not run past the
// samples that exist, and it may never rewind.
TEST(GraphScrollCursorTest, StalledStreamHoldsTheCursorWithoutRewinding) {
    GraphScrollCursor cursor;
    uint64_t samples = 0;
    for (int i = 0; i < 200; ++i)
        cursor.Advance(++samples);
    double previous = cursor.Advance(samples);
    const double ceiling = static_cast<double>(samples) - 2.0;
    for (int i = 0; i < 500; ++i) {
        const double position = cursor.Advance(samples);
        EXPECT_GE(position, previous);
        EXPECT_LE(position, ceiling);
        previous = position;
    }
}

// Recovering from a stall must not freeze the graph either.
TEST(GraphScrollCursorTest, StreamResumingAfterAStallKeepsScrolling) {
    GraphScrollCursor cursor;
    uint64_t samples = 0;
    for (int i = 0; i < 200; ++i)
        cursor.Advance(++samples);
    for (int i = 0; i < 400; ++i)
        cursor.Advance(samples);
    double previous = cursor.Advance(++samples);
    for (int i = 0; i < 400; ++i) {
        const double position = cursor.Advance(++samples);
        EXPECT_GT(position - previous, 0.5) << "draw " << i;
        previous = position;
    }
}

// The cursor must never step backwards under any arrival pattern.
TEST(GraphScrollCursorTest, CursorNeverRewindsAcrossMixedArrivalPatterns) {
    GraphScrollCursor cursor;
    uint64_t samples = 4;
    double previous = cursor.Advance(samples);
    const std::array<int, 17> mixed = {4, 0, 0, 0, 1, 1, 2, 0, 0, 3, 0, 0, 0, 0, 0, 6, 1};
    for (int r = 0; r < 300; ++r) {
        for (int arrived : mixed) {
            samples += static_cast<uint64_t>(arrived);
            const double position = cursor.Advance(samples);
            EXPECT_GE(position, previous) << "repeat " << r;
            previous = position;
        }
    }
}

// --- Absolute-index history access the scrolling graph reads through ---

namespace {

// Appends `count` samples one millisecond apart, values 1.0, 2.0, ...
void FillPresentationHistory(PerformanceMetrics& metrics, int count) {
    int64_t qpcUs = 1'000'000;
    for (int i = 0; i < count; ++i) {
        qpcUs += 1000 * (i + 1);
        metrics.Update(qpcUs);
    }
}

}  // namespace

TEST(PerformanceMetricsHistoryTest, SampleCountMatchesAppendedSamples) {
    PerformanceMetrics metrics;
    EXPECT_EQ(metrics.GetSampleCount(), 0u);
    FillPresentationHistory(metrics, 10);
    // The first Update only seeds the previous timestamp, so it appends nothing.
    EXPECT_EQ(metrics.GetSampleCount(), 9u);
}

TEST(PerformanceMetricsHistoryTest, WindowEndingAtNewestMatchesTheTailRead) {
    PerformanceMetrics metrics;
    FillPresentationHistory(metrics, 40);
    const uint64_t written = metrics.GetSampleCount();
    ASSERT_GE(written, 20u);

    float tail[16] = {};
    metrics.GetLastHistory(tail, 16);
    float window[16] = {};
    metrics.GetHistoryEndingAt(written - 1, window, 16);
    for (int i = 0; i < 16; ++i)
        EXPECT_FLOAT_EQ(window[i], tail[i]) << "slot " << i;
}

// Stepping the end index back by one must shift the window by exactly one slot;
// this is what makes the scroll cursor's motion a pure translation.
TEST(PerformanceMetricsHistoryTest, SteppingTheEndIndexShiftsTheWindowByOneSlot) {
    PerformanceMetrics metrics;
    FillPresentationHistory(metrics, 60);
    const uint64_t written = metrics.GetSampleCount();

    float newer[12] = {};
    float older[12] = {};
    metrics.GetHistoryEndingAt(written - 1, newer, 12);
    metrics.GetHistoryEndingAt(written - 2, older, 12);
    for (int i = 0; i < 11; ++i)
        EXPECT_FLOAT_EQ(older[i + 1], newer[i]) << "slot " << i;
}

TEST(PerformanceMetricsHistoryTest, IndicesBeyondTheStreamReadBackAsZero) {
    PerformanceMetrics metrics;
    FillPresentationHistory(metrics, 8);
    const uint64_t written = metrics.GetSampleCount();

    float window[8] = {};
    metrics.GetHistoryEndingAt(written + 4, window, 8);
    // The four slots past the newest sample are unwritten and must not alias
    // stale ring contents.
    for (int i = 4; i < 8; ++i)
        EXPECT_FLOAT_EQ(window[i], 0.0f) << "slot " << i;
}

TEST(PerformanceMetricsHistoryTest, WindowBeforeTheStreamStartReadsBackAsZero) {
    PerformanceMetrics metrics;
    FillPresentationHistory(metrics, 6);
    float window[8] = {};
    metrics.GetHistoryEndingAt(1, window, 8);
    for (int i = 0; i < 6; ++i)
        EXPECT_FLOAT_EQ(window[i], 0.0f) << "slot " << i;
    EXPECT_GT(window[7], 0.0f);
}

TEST(PerformanceMetricsHistoryTest, ZeroOrNegativeCountIsRejectedSafely) {
    PerformanceMetrics metrics;
    FillPresentationHistory(metrics, 8);
    float window[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    metrics.GetHistoryEndingAt(4, window, 0);
    metrics.GetHistoryEndingAt(4, nullptr, 4);
    EXPECT_FLOAT_EQ(window[0], 1.0f);
}
