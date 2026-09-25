#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../captureengine/display_timing_publication.h"
#include "../captureengine/display_timing_refresh.h"
#include "../captureengine/display_timing_refresh_bound.h"
#include "../captureengine/display_timing_submissions.h"
#include "../captureengine/display_timing_vblank.h"
#include "../common/display_timing_shared.h"

namespace {
constexpr int64_t kPeriod = 6945;  // 144 Hz
constexpr int64_t kStart = 1'000'000;

// Observed blanks on one display, in microseconds.
struct Blanks {
    std::vector<int64_t> times;

    int64_t operator()(int64_t from, int64_t until) const {
        for (const int64_t blank : times) {
            if (blank >= from && blank <= until)
                return blank;
        }
        return 0;
    }
};

Blanks EveryRefresh(int64_t first, int count) {
    Blanks blanks;
    for (int i = 0; i < count; ++i)
        blanks.times.push_back(first + i * kPeriod);
    return blanks;
}

std::vector<int64_t> GraphTimes(RefreshBoundedGraphTime& bound, const std::vector<int64_t>& reports, bool synchronized,
                                int64_t period, const Blanks& blanks) {
    std::vector<int64_t> graph;
    for (const int64_t report : reports)
        graph.push_back(bound.Apply(report, synchronized, period, blanks).graphUs);
    return graph;
}

// The FSR frame generation signature at the vsync ceiling: one flip of every
// pair reported on its blank, the other 3.5 ms after it (session
// 20260925_183820). The panel showed both one refresh apart.
std::vector<int64_t> AlternatingEarlyReports(int pairs) {
    std::vector<int64_t> reports;
    for (int i = 0; i < pairs; ++i) {
        const int64_t onBlank = kStart + 2 * i * kPeriod + 30;
        reports.push_back(onBlank);
        reports.push_back(onBlank + 3530);
    }
    return reports;
}
}  // namespace

TEST(RefreshBoundedGraphTimeTest, FlattensFlipsReportedSoonerThanTheRefreshAllows) {
    RefreshBoundedGraphTime bound;
    const auto reports = AlternatingEarlyReports(40);
    const auto graph = GraphTimes(bound, reports, true, kPeriod, EveryRefresh(kStart, 100));
    for (std::size_t i = 2; i < graph.size(); ++i)
        EXPECT_NEAR(graph[i] - graph[i - 1], kPeriod, 60) << i;
    // Never earlier than the kernel said, and never by more than one refresh.
    for (std::size_t i = 0; i < graph.size(); ++i) {
        EXPECT_GE(graph[i], reports[i]);
        EXPECT_LE(graph[i] - reports[i], kPeriod);
    }
}

TEST(RefreshBoundedGraphTimeTest, KeepsARepeatedFrameAtFullLength) {
    RefreshBoundedGraphTime bound;
    // One frame shown for two refreshes, reported on its blanks.
    const std::vector<int64_t> reports = {kStart, kStart + kPeriod, kStart + 3 * kPeriod, kStart + 4 * kPeriod};
    const auto graph = GraphTimes(bound, reports, true, kPeriod, EveryRefresh(kStart, 10));
    EXPECT_EQ(graph, reports);
}

TEST(RefreshBoundedGraphTimeTest, KeepsAHitchNextToEarlyReportsVisible) {
    RefreshBoundedGraphTime bound;
    auto reports = AlternatingEarlyReports(4);
    // Then a real 3-refresh hitch reported on its blank.
    const int64_t hitch = kStart + 8 * kPeriod + 3 * kPeriod + 30;
    reports.push_back(hitch);
    const auto graph = GraphTimes(bound, reports, true, kPeriod, EveryRefresh(kStart, 30));
    EXPECT_EQ(graph.back(), hitch);
    // The last early flip could not show before its blank (7 refreshes in),
    // so the hitch is drawn as the four refreshes the panel really held it.
    EXPECT_EQ(graph[graph.size() - 2], kStart + 7 * kPeriod);
    EXPECT_EQ(graph.back() - graph[graph.size() - 2], 4 * kPeriod + 30);
}

TEST(RefreshBoundedGraphTimeTest, LeavesVariableRefreshBelowTheCeilingUntouched) {
    RefreshBoundedGraphTime bound;
    std::vector<int64_t> reports;
    Blanks blanks;
    int64_t time = kStart;
    const int64_t gaps[] = {8100, 11900, 7600, 9800, 14200, 7000, 10400};
    for (int i = 0; i < 70; ++i) {
        time += gaps[i % 7];
        reports.push_back(time);
        blanks.times.push_back(time);
    }
    EXPECT_EQ(GraphTimes(bound, reports, true, kPeriod, blanks), reports);
}

TEST(RefreshBoundedGraphTimeTest, LeavesTearingPresentsUntouched) {
    RefreshBoundedGraphTime bound;
    const auto reports = AlternatingEarlyReports(10);
    EXPECT_EQ(GraphTimes(bound, reports, false, kPeriod, EveryRefresh(kStart, 40)), reports);
}

TEST(RefreshBoundedGraphTimeTest, LeavesReportsAloneWithoutAPeriodOrAnObservedBlank) {
    RefreshBoundedGraphTime unknownPeriod;
    const auto reports = AlternatingEarlyReports(10);
    EXPECT_EQ(GraphTimes(unknownPeriod, reports, true, 0, EveryRefresh(kStart, 40)), reports);

    RefreshBoundedGraphTime noBlanks;
    std::vector<RefreshBoundDecision> decisions;
    for (const int64_t report : reports)
        decisions.push_back(noBlanks.Apply(report, true, kPeriod, Blanks{}));
    for (std::size_t i = 0; i < reports.size(); ++i) {
        EXPECT_EQ(decisions[i].graphUs, reports[i]);
        EXPECT_FALSE(decisions[i].bounded);
    }
    EXPECT_TRUE(decisions[1].blankMissing);
}

// A completion reported late (a delayed DPC) must not push every later frame:
// the next report sits on a real blank, which ends the correction there.
TEST(RefreshBoundedGraphTimeTest, CarriesNoDelayForwardFromALateReport) {
    RefreshBoundedGraphTime bound;
    const std::vector<int64_t> reports = {kStart,           kStart + 12000,       kStart + 2 * kPeriod,
                                          kStart + 3 * kPeriod, kStart + 4 * kPeriod, kStart + 5 * kPeriod};
    const auto graph = GraphTimes(bound, reports, true, kPeriod, EveryRefresh(kStart, 10));
    EXPECT_EQ(graph, reports);
}

TEST(RefreshBoundedGraphTimeTest, TwoReportsInOneRefreshTakeConsecutiveBlanks) {
    RefreshBoundedGraphTime bound;
    const std::vector<int64_t> reports = {kStart, kStart + 3000, kStart + 5000};
    const auto graph = GraphTimes(bound, reports, true, kPeriod, EveryRefresh(kStart, 10));
    EXPECT_EQ(graph[1], kStart + kPeriod);
    EXPECT_EQ(graph[2], kStart + 2 * kPeriod);
}

// A stale period from before a mode switch (60 Hz believed, 144 Hz driven)
// must not stretch real 144 Hz frames: every report is on an observed blank.
TEST(RefreshBoundedGraphTimeTest, AStalePeriodCannotStretchFramesPastObservedBlanks) {
    RefreshBoundedGraphTime bound;
    std::vector<int64_t> reports;
    for (int i = 0; i < 20; ++i)
        reports.push_back(kStart + i * kPeriod + 20);
    const auto graph = GraphTimes(bound, reports, true, 16'667, EveryRefresh(kStart, 30));
    for (std::size_t i = 1; i < graph.size(); ++i)
        EXPECT_NEAR(graph[i] - graph[i - 1], kPeriod, 60) << i;
}

TEST(DisplayRefreshPeriodsTest, ConvertsModeRatesAndRejectsAmbiguousSources) {
    const auto periods = DisplayRefreshPeriods::FromPaths({
        {1, 0, 144000, 1000},
        {1, 1, 60000, 1001},
        {2, 1, 60000, 1001},   // same rate on a second adapter: still usable
        {1, 2, 120, 1},
        {2, 2, 60, 1},         // same id, different rate: unusable
        {1, 3, 0, 0},          // unreported rate
    });
    EXPECT_EQ(periods.PeriodUs(0), 6944);
    EXPECT_EQ(periods.PeriodUs(1), 16683);
    EXPECT_EQ(periods.PeriodUs(2), 0);
    EXPECT_EQ(periods.PeriodUs(3), 0);
    EXPECT_EQ(periods.PeriodUs(9), 0);
}

TEST(VerticalBlankClockTest, FindsOnlyObservedBlanksInRange) {
    VerticalBlankClock clock;
    for (int i = 0; i < 10; ++i)
        clock.Observe(1, kStart + i * kPeriod);
    EXPECT_EQ(clock.FirstBlankInRange(1, kStart + 1, kStart + 2 * kPeriod), kStart + kPeriod);
    EXPECT_EQ(clock.FirstBlankInRange(1, kStart + 1, kStart + kPeriod - 1), 0);
    EXPECT_EQ(clock.FirstBlankInRange(0, kStart, kStart + kPeriod), 0);
    EXPECT_EQ(clock.FirstBlankInRange(1, kStart + 20 * kPeriod, kStart + 30 * kPeriod), 0);
}

TEST(DisplayTimingSubmissionsTest, CarriesThePresentSyncIntervalToItsSubmission) {
    DisplaySubmissionTracker tracker;
    tracker.ObserveRuntimePresent(100, 10, 1'000'000, 1);
    ASSERT_TRUE(tracker.Associate(100, 10, 7, 1'000'100));
    ASSERT_NE(tracker.Find(7), nullptr);
    EXPECT_EQ(tracker.Find(7)->syncInterval, 1);

    // A submission without a runtime present has no known sync interval.
    ASSERT_TRUE(tracker.Associate(200, 20, 8, 1'000'200));
    ASSERT_NE(tracker.Find(8), nullptr);
    EXPECT_EQ(tracker.Find(8)->syncInterval, kUnknownSyncInterval);
}

TEST(DisplayTimingOutputsTest, PublishesKernelTimeAndBoundedGraphTimeSideBySide) {
    constexpr int64_t kFrequency = 1'000'000;  // QPC ticks are microseconds here
    SharedDisplayTiming timing;
    timing.Reset(42, 0, DisplayTimingStatus::Active);
    DisplayTimingOutputs outputs;
    outputs.SetQpcFrequency(kFrequency);
    outputs.Track(&timing);
    const std::vector<DisplayTimingTarget> targets = {{42, 0, &timing}};
    const Blanks blanks = EveryRefresh(kStart, 100);
    const auto periodUs = [](uint32_t source) { return source == 3 ? kPeriod : 0; };
    const auto firstBlank = [&](uint32_t source, int64_t from, int64_t until) {
        return source == 3 ? blanks(from, until) : 0;
    };

    const auto reports = AlternatingEarlyReports(20);
    for (const int64_t report : reports) {
        DisplayTimingPublication sample;
        sample.processId = 42;
        sample.timestampQpc = report;
        sample.presentStartQpc = report - 13'000;
        sample.synchronizedFlip = true;
        sample.displaySource = 3;
        outputs.Publish(targets, sample, report + 30'000, periodUs, firstBlank);
    }

    int64_t previousGraph = 0;
    for (uint64_t sequence = 1; sequence <= reports.size(); ++sequence) {
        int64_t screenTimeUs = 0;
        int64_t presentStartUs = 0;
        bool resolved = false;
        int64_t graphTimeUs = 0;
        ASSERT_TRUE(timing.Read(sequence, screenTimeUs, presentStartUs, resolved, graphTimeUs));
        EXPECT_EQ(screenTimeUs, reports[sequence - 1]);
        EXPECT_GE(graphTimeUs, screenTimeUs);
        if (sequence > 2) {
            EXPECT_NEAR(graphTimeUs - previousGraph, kPeriod, 60) << sequence;
        }
        previousGraph = graphTimeUs;
    }

    DisplayTimingHealth health;
    outputs.Snapshot(health);
    EXPECT_EQ(health.refreshPeriodUs, kPeriod);
    EXPECT_EQ(health.refreshBoundApplied, reports.size() / 2);
    EXPECT_NEAR(health.refreshBoundShiftMeanUs, kPeriod - 3530, 60);
    EXPECT_GT(health.publishedIntervalJaggednessUs, 6000);
    EXPECT_LT(health.graphIntervalJaggednessUs, 100);
}

TEST(DisplayTimingOutputsTest, LeavesUnsynchronizedFlipsAsReported) {
    SharedDisplayTiming timing;
    timing.Reset(42, 0, DisplayTimingStatus::Active);
    DisplayTimingOutputs outputs;
    outputs.SetQpcFrequency(1'000'000);
    outputs.Track(&timing);
    const std::vector<DisplayTimingTarget> targets = {{42, 0, &timing}};
    const Blanks blanks = EveryRefresh(kStart, 100);
    const auto reports = AlternatingEarlyReports(5);
    for (const int64_t report : reports) {
        DisplayTimingPublication sample;
        sample.processId = 42;
        sample.timestampQpc = report;
        sample.synchronizedFlip = false;
        outputs.Publish(
            targets, sample, report + 30'000, [](uint32_t) { return kPeriod; },
            [&](uint32_t, int64_t from, int64_t until) { return blanks(from, until); });
    }
    for (uint64_t sequence = 1; sequence <= reports.size(); ++sequence) {
        int64_t screenTimeUs = 0;
        int64_t presentStartUs = 0;
        bool resolved = false;
        int64_t graphTimeUs = 0;
        ASSERT_TRUE(timing.Read(sequence, screenTimeUs, presentStartUs, resolved, graphTimeUs));
        EXPECT_EQ(graphTimeUs, screenTimeUs);
    }
}

TEST(SharedDisplayTimingTest, GraphTimeDefaultsToTheScreenTime) {
    SharedDisplayTiming timing;
    timing.Reset(1, 0, DisplayTimingStatus::Active);
    timing.Publish(5000, 9000);
    timing.Publish(8000, 9001, 0, true, 12000);
    timing.Publish(20000, 9002, 0, true, 15000);  // not later: not a bound

    int64_t screenTimeUs = 0;
    int64_t presentStartUs = 0;
    bool resolved = false;
    int64_t graphTimeUs = 0;
    ASSERT_TRUE(timing.Read(1, screenTimeUs, presentStartUs, resolved, graphTimeUs));
    EXPECT_EQ(graphTimeUs, 5000);
    ASSERT_TRUE(timing.Read(2, screenTimeUs, presentStartUs, resolved, graphTimeUs));
    EXPECT_EQ(screenTimeUs, 8000);
    EXPECT_EQ(graphTimeUs, 12000);
    EXPECT_NE(timing.samples[1].flags.load() & kDisplayTimingGraphTimeRefreshBounded, 0u);
    ASSERT_TRUE(timing.Read(3, screenTimeUs, presentStartUs, resolved, graphTimeUs));
    EXPECT_EQ(graphTimeUs, 20000);
    EXPECT_EQ(timing.samples[2].flags.load() & kDisplayTimingGraphTimeRefreshBounded, 0u);
}
