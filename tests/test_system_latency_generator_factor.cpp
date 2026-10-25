#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "hook/metrics/system_latency_metrics.h"

// DLSS-G's dynamic multi-frame generation moves its factor between 2x and 4x on its own, many times a second (Talos,
// session 20260919_223111: published multiplier 2/3/4 against a constant ~140 fps output). Every change used to start
// a new measurement epoch, which emptied the window, so the overlay showed "PC Latency --" for a text refresh at each
// change during ordinary gameplay. A factor change of a generator that keeps running now carries the measurements
// across; switching the generator off, or to another type, still starts over.

namespace {

using ce::system_latency::FrameBeginKind;
using ce::system_latency::Snapshot;
using ce::system_latency::Source;
using ce::system_latency::Tracker;

constexpr int64_t kOutputIntervalUs = 7'000;  // the output cadence a target-rate hold keeps constant
constexpr int kDlssType = 1;
constexpr int kFsrType = 2;

float BaseFps(int factor) {
    return 1'000'000.0f / static_cast<float>(factor * kOutputIntervalUs);
}

// One application frame at a time, each shown `factor` times at the output cadence, with the factor published
// before the frame as the overlay publishes it on the Present path. The overlay reads the latency on that same
// path, before the sensor has delivered the frame's displays: `onPresent` sees what it would show.
class GeneratedStream {
public:
    GeneratedStream(Tracker& tracker, int64_t startUs) : tracker_(tracker), nextApplicationUs_(startUs) {}

    int64_t Frame(int factor, Snapshot* onPresent = nullptr) {
        tracker_.SetFrameGeneration(BaseFps(factor), factor, kDlssType);
        const int64_t applicationUs = nextApplicationUs_;
        tracker_.ObserveApplicationPresent(applicationUs, applicationUs - 2'000, FrameBeginKind::LowLatencySleepReturn);
        if (onPresent)
            *onPresent = tracker_.GetSnapshot(applicationUs);
        for (int output = 0; output < factor; ++output) {
            const int64_t runtimePresentUs = applicationUs + 1'000 + kOutputIntervalUs * output;
            lastScreenUs_ = runtimePresentUs + 1'500;
            tracker_.ObserveDisplay(lastScreenUs_, runtimePresentUs);
        }
        nextApplicationUs_ += factor * kOutputIntervalUs;
        return lastScreenUs_;
    }

private:
    Tracker& tracker_;
    int64_t nextApplicationUs_;
    int64_t lastScreenUs_ = 0;
};

TEST(SystemLatencyGeneratorFactorTest, DynamicFactorChangesNeverBlankTheReading) {
    Tracker tracker;
    GeneratedStream stream(tracker, 50'000'000);
    for (int frame = 0; frame < 16; ++frame)
        stream.Frame(2);
    const auto before = tracker.GetDiagnostics();

    constexpr int kFactors[] = {2, 2, 3, 3, 4, 4, 4, 3, 2, 3, 4, 2};
    constexpr size_t kFactorCount = sizeof(kFactors) / sizeof(kFactors[0]);
    int previousFactor = 2;
    uint64_t expectedChanges = 0;
    int blankFrames = 0;
    float lowest = 0.0f;
    float highest = 0.0f;
    for (size_t frame = 0; frame < 10 * kFactorCount; ++frame) {
        const int factor = kFactors[frame % kFactorCount];
        expectedChanges += factor != previousFactor ? 1 : 0;
        previousFactor = factor;
        Snapshot snapshot;
        stream.Frame(factor, &snapshot);
        if (!snapshot.valid) {
            ++blankFrames;
            continue;
        }
        EXPECT_EQ(snapshot.source, Source::Estimated);
        lowest = lowest == 0.0f || snapshot.milliseconds < lowest ? snapshot.milliseconds : lowest;
        highest = snapshot.milliseconds > highest ? snapshot.milliseconds : highest;
    }

    EXPECT_EQ(blankFrames, 0) << "frames whose reading was blank after a factor change";
    const auto after = tracker.GetDiagnostics();
    EXPECT_EQ(after.measurementEpochResets, before.measurementEpochResets);
    EXPECT_EQ(after.generatorFactorChanges - before.generatorFactorChanges, expectedChanges);
    // Still a measurement of this chain: between one output interval and the 4x frame plus its hold.
    EXPECT_GT(lowest, static_cast<float>(kOutputIntervalUs) / 1000.0f);
    EXPECT_LT(highest, static_cast<float>(3 * 4 * kOutputIntervalUs) / 1000.0f);
}

TEST(SystemLatencyGeneratorFactorTest, AFactorChangeKeepsThePublishedReading) {
    Tracker tracker;
    GeneratedStream stream(tracker, 51'000'000);
    int64_t screenUs = 0;
    for (int frame = 0; frame < 24; ++frame)
        screenUs = stream.Frame(3);
    const auto reading = tracker.GetSnapshot(screenUs);
    ASSERT_TRUE(reading.valid);

    tracker.SetFrameGeneration(BaseFps(4), 4, kDlssType);

    const auto afterChange = tracker.GetSnapshot(screenUs);
    EXPECT_TRUE(afterChange.valid);
    EXPECT_FLOAT_EQ(afterChange.milliseconds, reading.milliseconds);
    EXPECT_TRUE(afterChange.frameGenerationConfigured);
    EXPECT_EQ(tracker.GetDiagnostics().generatorFactorChanges, 1u);
}

TEST(SystemLatencyGeneratorFactorTest, SwitchingTheGeneratorOffOrToAnotherTypeStillStartsOver) {
    for (const bool switchOff : {true, false}) {
        Tracker tracker;
        GeneratedStream stream(tracker, 52'000'000);
        int64_t screenUs = 0;
        for (int frame = 0; frame < 24; ++frame)
            screenUs = stream.Frame(2);
        ASSERT_TRUE(tracker.GetSnapshot(screenUs).valid);
        const uint64_t resetsBefore = tracker.GetDiagnostics().measurementEpochResets;

        if (switchOff)
            tracker.SetFrameGeneration(0.0f, 1, 0);
        else
            tracker.SetFrameGeneration(BaseFps(2), 2, kFsrType);

        EXPECT_FALSE(tracker.GetSnapshot(screenUs).valid) << (switchOff ? "off" : "type change");
        EXPECT_EQ(tracker.GetDiagnostics().measurementEpochResets, resetsBefore + 1);
        EXPECT_EQ(tracker.GetDiagnostics().generatorFactorChanges, 0u);
    }
}

// Conservation (application frames minus displays / factor since a known-empty seed) assumed the factor it was
// seeded under; after a change that count no longer means anything, so the single-frame hold stands.
TEST(SystemLatencyGeneratorFactorTest, AFactorChangeAbandonsAQueueCountSeededUnderTheOldFactor) {
    constexpr int64_t kApplicationIntervalUs = 2 * kOutputIntervalUs;
    Tracker tracker;
    tracker.SetFrameGeneration(BaseFps(2), 2, kDlssType);
    int64_t lastScreenUs = 0;
    for (int frame = 0; frame < 42; ++frame) {
        tracker.ObserveApplicationPresent(60'000'000 + kApplicationIntervalUs * frame);
        if (frame < 2)
            continue;
        for (int output = 0; output < 2; ++output) {
            const int64_t runtimePresentUs =
                60'000'000 + kApplicationIntervalUs * frame + 1'000 + kOutputIntervalUs * output;
            lastScreenUs = runtimePresentUs + 1'500;
            tracker.ObserveDisplay(lastScreenUs, runtimePresentUs);
        }
    }
    ASSERT_EQ(tracker.GetDiagnostics().applicationFramesInFlight, 2u);

    tracker.SetFrameGeneration(BaseFps(3), 3, kDlssType);

    const auto diagnostics = tracker.GetDiagnostics();
    EXPECT_EQ(diagnostics.applicationFramesInFlight, 0u);
    EXPECT_EQ(diagnostics.queueDepthCountsRejected, 0u) << "not an impossible count, a superseded one";
    EXPECT_EQ(diagnostics.generatorFactorChanges, 1u);
    EXPECT_TRUE(tracker.GetSnapshot(lastScreenUs).valid);
}

}  // namespace
