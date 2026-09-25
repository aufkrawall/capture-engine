#include <gtest/gtest.h>

#include "../hook/common/system_latency_metrics.h"

// GTA session 20260925_233000: after FSR FG switched on, AMD's runtime produced no display for ~500 ms while the
// game presented 25-35 frames. The in-flight count (application presents minus displays / multiplier since the
// FG-on seed) booked all of them as queued, clamped to 8, and the overlay published ~110-150 ms PC latency for
// the whole FG session instead of ~30 ms. A count outside the physically possible range is now abandoned until
// the next known-empty seed, and the documented single-frame hold stands.

namespace {

using ce::system_latency::Tracker;

constexpr int64_t kApplicationIntervalUs = 14'000;
constexpr int64_t kOutputIntervalUs = 7'000;

struct Result {
    float milliseconds = 0.0f;
    uint32_t framesInFlight = 0;
    uint64_t rejected = 0;
};

// framesBeforeFirstDisplay application frames reach the generator before anything is displayed; afterwards every
// frame is shown twice, one frame behind the newest (a one-deep queue in steady state).
Result Measure(int framesBeforeFirstDisplay, bool trustQueueCount = true, int extraDisplaysAtStart = 0) {
    Tracker tracker;
    tracker.SetFrameGeneration(1'000'000.0f / static_cast<float>(kApplicationIntervalUs), 2, /*fgType=*/2);
    if (!trustQueueCount)
        tracker.NoteDisplayStreamGap();
    int64_t lastScreenUs = 0;
    for (int extra = 0; extra < extraDisplaysAtStart; ++extra) {
        lastScreenUs = 30'000'000 - 1'000 * (extraDisplaysAtStart - extra);
        tracker.ObserveDisplay(lastScreenUs, lastScreenUs - 500);
    }
    for (int frame = 0; frame < framesBeforeFirstDisplay + 40; ++frame) {
        tracker.ObserveApplicationPresent(30'000'000 + kApplicationIntervalUs * frame);
        if (frame < framesBeforeFirstDisplay)
            continue;
        for (int output = 0; output < 2; ++output) {
            const int64_t runtimePresentUs =
                30'000'000 + kApplicationIntervalUs * frame + 1'000 + kOutputIntervalUs * output;
            lastScreenUs = runtimePresentUs + 1'500;
            tracker.ObserveDisplay(lastScreenUs, runtimePresentUs);
        }
    }
    const auto diagnostics = tracker.GetDiagnostics();
    Result result;
    result.milliseconds = tracker.GetSnapshot(lastScreenUs).milliseconds;
    result.framesInFlight = diagnostics.applicationFramesInFlight;
    result.rejected = diagnostics.queueDepthCountsRejected;
    return result;
}

TEST(SystemLatencyQueueCountRejection, WarmUpDiscardIsNotReportedAsAQueue) {
    const Result gta = Measure(/*framesBeforeFirstDisplay=*/30);
    EXPECT_EQ(gta.framesInFlight, 0u) << "the old clamp reported 8";
    EXPECT_EQ(gta.rejected, 1u);
    // Falls back to exactly the single-frame hold an untrusted count produces.
    EXPECT_FLOAT_EQ(gta.milliseconds, Measure(30, /*trustQueueCount=*/false).milliseconds);
}

TEST(SystemLatencyQueueCountRejection, RealQueuesUpToTheBoundAreStillMeasured) {
    for (int depth : {1, 2, 4, 8}) {
        const Result queued = Measure(depth);
        EXPECT_EQ(queued.framesInFlight, static_cast<uint32_t>(depth)) << "depth " << depth;
        EXPECT_EQ(queued.rejected, 0u) << "depth " << depth;
    }
}

TEST(SystemLatencyQueueCountRejection, OneFramePastTheBoundIsRejected) {
    // A full 8-deep queue transiently reads 9 while the newest frame is in transit; 9 queued reads 10.
    const Result nine = Measure(9);
    EXPECT_EQ(nine.framesInFlight, 0u);
    EXPECT_EQ(nine.rejected, 1u);
}

TEST(SystemLatencyQueueCountRejection, MoreRetiredThanPresentedIsRejected) {
    // Displays the seed cannot account for (flips of frames issued before it, beyond one frame's worth) break
    // conservation the other way; the count silently read 0 before and now says it is not measurable.
    const Result overRetired = Measure(1, true, /*extraDisplaysAtStart=*/6);
    EXPECT_EQ(overRetired.framesInFlight, 0u);
    EXPECT_EQ(overRetired.rejected, 1u);
    const Result withinSlack = Measure(1, true, /*extraDisplaysAtStart=*/2);
    EXPECT_EQ(withinSlack.rejected, 0u);
}

TEST(SystemLatencyQueueCountRejection, ANewSeedMeasuresAgain) {
    Tracker tracker;
    tracker.SetFrameGeneration(1'000'000.0f / static_cast<float>(kApplicationIntervalUs), 2, 2);
    for (int frame = 0; frame < 20; ++frame)
        tracker.ObserveApplicationPresent(40'000'000 + kApplicationIntervalUs * frame);
    ASSERT_EQ(tracker.GetDiagnostics().queueDepthCountsRejected, 1u);

    // An application-present gap longer than the correlator's interval bound is a known-empty point.
    const int64_t reseedUs = 40'000'000 + kApplicationIntervalUs * 19 + 400'000;
    for (int frame = 0; frame < 30; ++frame) {
        tracker.ObserveApplicationPresent(reseedUs + kApplicationIntervalUs * frame);
        if (frame < 2)
            continue;
        for (int output = 0; output < 2; ++output) {
            const int64_t runtimePresentUs = reseedUs + kApplicationIntervalUs * frame + 1'000 + kOutputIntervalUs * output;
            tracker.ObserveDisplay(runtimePresentUs + 1'500, runtimePresentUs);
        }
    }
    EXPECT_EQ(tracker.GetDiagnostics().applicationFramesInFlight, 2u);
    EXPECT_EQ(tracker.GetDiagnostics().queueDepthCountsRejected, 1u);
}

}  // namespace
