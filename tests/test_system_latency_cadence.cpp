#include <gtest/gtest.h>

#include "hook/metrics/system_latency_metrics.h"
#include "hook/metrics/system_latency_windows.h"

// The output cadence the overlay publishes ("Display" fps, the latency chain's
// displayInterval and output ratio) is the period of the display stream. Talos
// with FSR FG at a 120 fps cap (session 20261018_191159) displayed alternating
// 6.5/10.1 ms intervals: the median of those, ~9.8 ms, published 101 displayed
// fps and an output ratio of 1.69 for 2x while 120 frames reached the screen.

namespace {

using ce::system_latency::CadenceIntervalRing;
using ce::system_latency::MedianRing;
using ce::system_latency::Tracker;
using ce::system_latency::ValueRing;

TEST(SystemLatencyCadenceTest, AlternatingCadenceUsesTheMeanNotOneSide) {
    ValueRing<32> intervals;
    for (int i = 0; i < 32; ++i)
        intervals.Push(i % 2 == 0 ? 6'520 : 10'146);
    EXPECT_EQ(MedianRing(intervals), 10'146) << "the median picks one side of the pattern";
    EXPECT_EQ(CadenceIntervalRing(intervals), 8'333);
}

TEST(SystemLatencyCadenceTest, ThreeOutputGroupsStayWhole) {
    ValueRing<32> intervals;
    for (int i = 0; i < 30; ++i)
        intervals.Push(i % 3 == 2 ? 12'000 : 4'000);  // 3x with a long real-frame slot
    EXPECT_EQ(CadenceIntervalRing(intervals), 6'666);
}

TEST(SystemLatencyCadenceTest, HitchesStayOutOfTheCadence) {
    ValueRing<32> intervals;
    for (int i = 0; i < 31; ++i)
        intervals.Push(8'333);
    intervals.Push(200'000);
    EXPECT_EQ(CadenceIntervalRing(intervals), 8'333);
    ValueRing<32> empty;
    EXPECT_EQ(CadenceIntervalRing(empty), 0);
}

// The tracker's published output rate for the Talos pattern: 2x generation,
// base 60 fps, generated frames shown 2.1 ms after their Present and real ones
// 0.5 ms after theirs, presents evenly 8.33 ms apart.
TEST(SystemLatencyCadenceTest, TrackerPublishesTheRealOutputRateForUnevenGeneratedDisplays) {
    Tracker tracker;
    constexpr int64_t applicationIntervalUs = 16'667;
    constexpr int64_t presentSpacingUs = applicationIntervalUs / 2;
    tracker.SetFrameGeneration(60.0f, 2);
    int64_t lastScreenUs = 0;
    for (int frame = 0; frame < 40; ++frame) {
        const int64_t sourceUs = 20'000'000 + applicationIntervalUs * frame;
        tracker.ObserveApplicationPresent(sourceUs, 0, ce::system_latency::FrameBeginKind::Modelled);
        if (frame == 0)
            continue;
        const int64_t generatedPresentUs = sourceUs + 4'200;
        tracker.ObserveDisplay(lastScreenUs = generatedPresentUs + 2'100, generatedPresentUs);
        const int64_t realPresentUs = generatedPresentUs + presentSpacingUs;
        tracker.ObserveDisplay(lastScreenUs = realPresentUs + 500, realPresentUs);
    }
    const auto rates = tracker.GetMeasuredRates(lastScreenUs);
    ASSERT_GT(rates.outputIntervalUs, 0);
    EXPECT_NEAR(1'000'000.0 / static_cast<double>(rates.outputIntervalUs), 120.0, 0.5);
    const auto diagnostics = tracker.GetDiagnostics();
    EXPECT_NEAR(diagnostics.displayIntervalUs, 8'333, 2);
}

}  // namespace
