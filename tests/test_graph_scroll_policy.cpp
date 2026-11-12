#include "hook/overlay/graph_scroll_policy.h"
#include "hook/metrics/performance_metrics.h"
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

// Display and draw clocks are independent. The compositor chooses the latest submitted frame at each 144 Hz
// scanout while the application submits at 160 Hz. A display step must move the visible graph one slot, even when
// two application frames were submitted since the preceding scanout. Measured bounds: 0.75-1.25 slots per displayed
// step after warmup; aggregate drift must remain below two slots over the measured window.
TEST(GraphScrollCursorTest, DisplayedSequenceDoesNotAccelerateOnDiscardedApplicationPresents) {
    GraphScrollCursor cursor;
    uint64_t sampleCount = 100;
    int draw = 0;
    constexpr int64_t kOriginUs = 1'000'000;
    constexpr double kDisplayPeriodUs = 1'000'000.0 / 144.0;
    int64_t lastPublishedUs = kOriginUs;
    double latest = 0.0;
    double previous = 0.0;
    double firstMeasured = 0.0;
    constexpr int kWarmupDisplays = 120;
    constexpr int kDisplays = 600;
    for (int display = 1; display <= kDisplays; ++display) {
        const int64_t displayUs = kOriginUs + static_cast<int64_t>(display) * 1'000'000 / 144;
        while (kOriginUs + static_cast<int64_t>(draw) * 1'000'000 / 160 < displayUs) {
            const int64_t drawUs = kOriginUs + static_cast<int64_t>(draw) * 1'000'000 / 160;
            latest = cursor.Advance({1, sampleCount, lastPublishedUs, kDisplayPeriodUs, lastPublishedUs,
                                     kDisplayPeriodUs, drawUs});
            ++draw;
        }
        if (display == kWarmupDisplays) {
            firstMeasured = latest;
        } else if (display > kWarmupDisplays) {
            EXPECT_GE(latest - previous, 0.75) << "display " << display;
            EXPECT_LE(latest - previous, 1.25) << "display " << display;
        }
        previous = latest;
        if (display % 4 == 0) {
            sampleCount += 4;  // delayed, batched publication after the selected frame reaches the display
            lastPublishedUs = displayUs;
        }
    }
    EXPECT_NEAR(latest - firstMeasured, kDisplays - kWarmupDisplays, 2.0);
}

TEST(GraphScrollCursorTest, DistinctGeneratedOutputsAdvanceInBurstsAndDuplicatesDoNot) {
    constexpr int64_t kPeriodUs = 10'000;
    constexpr int64_t kOriginUs = 1'000'000;
    for (uint64_t multiplier : {2u, 3u, 4u}) {
        SCOPED_TRACE(multiplier);
        GraphScrollCursor cursor;
        double previous = 0.0;
        double first = 0.0;
        uint64_t outputId = 1;
        for (uint64_t group = 0; group < 200; ++group) {
            const uint64_t samples = 100 + group * multiplier;
            const int64_t lastDisplay = kOriginUs + static_cast<int64_t>(group * multiplier) * kPeriodUs;
            for (uint64_t output = 0; output < multiplier; ++output) {
                GraphCadenceSnapshot snapshot{1, samples, lastDisplay, static_cast<double>(kPeriodUs), lastDisplay,
                                              static_cast<double>(kPeriodUs), lastDisplay + 1,
                                              GraphOutputKind::kDistinctOutput, outputId++};
                const double position = cursor.Advance(snapshot);
                EXPECT_DOUBLE_EQ(cursor.Advance(snapshot), position) << "duplicate output must not scroll twice";
                EXPECT_LE(position, static_cast<double>(samples) - 2.0);
                if (group == 20 && output == 0)
                    first = position;
                else if (group > 20 || (group == 20 && output > 0)) {
                    EXPECT_NEAR(position - previous, 1.0, 0.001);
                }
                previous = position;
            }
        }
        EXPECT_NEAR(previous - first, static_cast<double>((200 - 20) * multiplier - 1), 0.01);
    }
}

TEST(GraphScrollCursorTest, UnevenDrawIntervalsStillTrackRegularDisplayedFrames) {
    GraphScrollCursor cursor;
    constexpr int64_t kPeriodUs = 10'000;
    constexpr int64_t kOriginUs = 1'000'000;
    constexpr std::array<int64_t, 4> kLeadUs = {1000, 6000, 3000, 8000};
    uint64_t samples = 100;
    int64_t lastPublished = kOriginUs;
    double previous = 0.0;
    for (int display = 1; display <= 400; ++display) {
        const int64_t displayUs = kOriginUs + display * kPeriodUs;
        const int64_t drawUs = displayUs - kLeadUs[static_cast<size_t>(display) % kLeadUs.size()];
        const double position = cursor.Advance({2, samples, lastPublished, static_cast<double>(kPeriodUs),
                                                lastPublished, static_cast<double>(kPeriodUs), drawUs});
        if (display > 50)
            EXPECT_NEAR(position - previous, 1.0, 0.001) << "display " << display;
        previous = position;
        if (display % 4 == 0) {
            samples += 4;
            lastPublished = displayUs;
        }
    }
}

TEST(GraphScrollCursorTest, CadenceHoldsWithinHistoryOnStallAndRearmsOnNewHistory) {
    GraphScrollCursor cursor;
    GraphCadenceSnapshot sample{3, 100, 1'000'000, 10'000, 1'000'000, 10'000, 1'000'001};
    double position = cursor.Advance(sample);
    for (int i = 0; i < 100; ++i) {
        sample.drawUs += 10'000;
        const double next = cursor.Advance(sample);
        EXPECT_GE(next, position);
        EXPECT_LE(next, 98.0);
        position = next;
    }
    sample.historyIdentity = 4;
    sample.sampleCount = 20;
    sample.lastSampleUs = sample.lastDisplayUs = sample.drawUs;
    const double reset = cursor.Advance(sample);
    EXPECT_GE(reset, 0.0);
    EXPECT_LE(reset, 18.0);
}

TEST(GraphScrollCursorTest, GeneratedAndSuspendedRoutesShareOneHistoryThroughRepeatedHandoffs) {
    GraphScrollCursor cursor;
    constexpr int64_t kPeriod = 10'000;
    int64_t displayTime = 1'000'000;
    int64_t publishedTime = displayTime;
    uint64_t samples = 100;
    uint64_t outputId = 1;
    double previous = -1.0;
    int shown = 0;
    auto check = [&](double position) {
        if (shown > 80) {
            EXPECT_GE(position - previous, 0.75) << "display " << shown;
            EXPECT_LE(position - previous, 1.25) << "display " << shown;
        }
        previous = position;
        ++shown;
    };
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (int group = 0; group < 50; ++group) {
            for (int output = 0; output < 4; ++output) {
                GraphCadenceSnapshot snapshot{5, samples, publishedTime, kPeriod, publishedTime, kPeriod,
                                              displayTime + 1, GraphOutputKind::kDistinctOutput, outputId++};
                check(cursor.Advance(snapshot));
            }
            displayTime += 4 * kPeriod;
            publishedTime = displayTime;
            samples += 4;
        }
        // Two app/proxy draws reach each output; only the latter frame is displayed. Route changes do not
        // change the stream identity and therefore may not reset the graph history.
        for (int frame = 0; frame < 200; ++frame) {
            GraphCadenceSnapshot snapshot{5, samples, publishedTime, kPeriod, publishedTime, kPeriod,
                                          displayTime + 1000};
            cursor.Advance(snapshot);
            snapshot.drawUs = displayTime + 6000;
            check(cursor.Advance(snapshot));
            displayTime += kPeriod;
            if (frame % 4 == 3) {
                publishedTime = displayTime;
                samples += 4;
            }
        }
    }
}

TEST(GraphScrollCursorTest, DistinctOutputsAfterAStallRecoverWithoutUnboundedPrediction) {
    GraphScrollCursor cursor;
    GraphCadenceSnapshot sample{6, 100, 1'000'000, 10'000, 1'000'000, 10'000, 1'000'001,
                                GraphOutputKind::kDistinctOutput, 1};
    for (int i = 0; i < 200; ++i) {
        sample.drawUs += 10'000;
        ++sample.outputId;
        EXPECT_LE(cursor.Advance(sample), 98.0);
    }
    double previous = cursor.Advance(sample);
    for (int i = 0; i < 40; ++i) {
        sample.drawUs += 10'000;
        sample.lastSampleUs = sample.lastDisplayUs = sample.drawUs - 1;
        ++sample.sampleCount;
        ++sample.outputId;
        const double position = cursor.Advance(sample);
        if (i > 10)
            EXPECT_NEAR(position - previous, 1.0, 0.01);
        previous = position;
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
