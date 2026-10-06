#include <gtest/gtest.h>

#include <windows.h>

#include <thread>

#include "common/ipc/display_timing_shared.h"
#include "hook/metrics/performance_metrics.h"
#include "hook/metrics/system_latency_frame_begin.h"
#include "hook/metrics/system_latency_metrics.h"
#include "hook/present/present_callback_association.h"

// Frame identity through a frame generator (FSR FG's frameID), the display
// scanout term, measured frame-generation rates and where the game emits its
// PresentStart marker.

namespace {

using ce::present_association::GeneratorFrameToken;
using ce::system_latency::FrameBeginClock;
using ce::system_latency::FrameBeginKind;
using ce::system_latency::FrameBeginObservation;
using ce::system_latency::MarkerThread;
using ce::system_latency::Tracker;

constexpr uint32_t kGameThread = 41;
constexpr int64_t kApplicationIntervalUs = 14'000;
constexpr int64_t kOutputIntervalUs = 7'000;
constexpr int64_t kPresentToDisplayUs = 2'000;
constexpr int64_t kSleepLeadUs = 3'000;
// The generator presents frame N's outputs once the game has already handed
// it frames N+1 and N+2: a queue two frames deep behind the interpolation hold.
constexpr int kQueueDepth = 2;

int64_t ApplicationPresentUs(int frame) {
    return 30'000'000 + kApplicationIntervalUs * frame;
}

// One FSR-FG-shaped run. Every tenth frame the generator shows no
// interpolated output (as FSR does when it skips interpolation), which leaves
// a display count one short per skip: a count of displays per application
// frame drifts, the frame identity cannot.
struct GeneratorRun {
    int64_t lastAnchorToPresentUs = 0;
    int generatorQueueDepthById = 0;
    uint64_t matched = 0;
    uint64_t unmatched = 0;
    FrameBeginKind lastKind = FrameBeginKind::Modelled;
};

GeneratorRun RunGenerator(bool reportFrameIds, int frames = 60) {
    Tracker tracker;
    tracker.SetFrameGeneration(1'000'000.0f / kApplicationIntervalUs, 2, /*fgType=*/2);
    for (int frame = 0; frame < frames; ++frame) {
        const int64_t presentUs = ApplicationPresentUs(frame);
        FrameBeginObservation begin;
        begin.beginUs = presentUs - kSleepLeadUs;
        begin.kind = FrameBeginKind::LowLatencySleepReturn;
        begin.generatorFrameToken = reportFrameIds ? GeneratorFrameToken(1000 + frame) : 0;
        tracker.ObserveApplicationPresent(presentUs, begin, kGameThread);

        const int source = frame - kQueueDepth;
        if (source < 0)
            continue;
        const uint64_t token = reportFrameIds ? GeneratorFrameToken(1000 + source) : 0;
        const int64_t generatedUs = presentUs + 1'000;
        if (source % 10 != 9)
            tracker.ObserveDisplay(generatedUs + kPresentToDisplayUs, generatedUs, token);
        const int64_t realUs = generatedUs + kOutputIntervalUs;
        tracker.ObserveDisplay(realUs + kPresentToDisplayUs, realUs, token);
    }
    const auto diagnostics = tracker.GetDiagnostics();
    GeneratorRun run;
    run.lastAnchorToPresentUs = diagnostics.lastAnchorToPresentUs;
    run.generatorQueueDepthById = diagnostics.generatorQueueDepthById;
    run.matched = diagnostics.generatorFramesMatchedById;
    run.unmatched = diagnostics.generatorFramesUnmatchedById;
    run.lastKind = diagnostics.lastFrameBeginKind;
    return run;
}

}  // namespace

TEST(SystemLatencyGeneratorIdentityTest, OutputsAreMatchedToTheirGameFrameByTheGeneratorsFrameId) {
    const GeneratorRun run = RunGenerator(/*reportFrameIds=*/true);
    // Real output of frame N: presented 1 ms + one output interval after the
    // game's Present of frame N+2, anchored 3 ms before the game's Present of N.
    constexpr int64_t kExpectedUs = kQueueDepth * kApplicationIntervalUs + 1'000 + kOutputIntervalUs + kSleepLeadUs;
    EXPECT_EQ(run.lastAnchorToPresentUs, kExpectedUs);
    EXPECT_EQ(run.generatorQueueDepthById, kQueueDepth);
    EXPECT_GT(run.matched, 100u);
    EXPECT_EQ(run.unmatched, 0u);
    EXPECT_EQ(run.lastKind, FrameBeginKind::LowLatencySleepReturn);
}

TEST(SystemLatencyGeneratorIdentityTest, CountingTowardTheFrameDriftsWhereIdentityDoesNot) {
    // The same run without frame IDs falls back to conservation, which the
    // skipped interpolations push off the true frame.
    const GeneratorRun counted = RunGenerator(/*reportFrameIds=*/false);
    const GeneratorRun identified = RunGenerator(/*reportFrameIds=*/true);
    EXPECT_NE(counted.lastAnchorToPresentUs, identified.lastAnchorToPresentUs);
    EXPECT_EQ(counted.matched, 0u);
    EXPECT_EQ(counted.unmatched, 0u);
}

TEST(SystemLatencyGeneratorIdentityTest, AnIdNoApplicationPresentCarriedFallsBackAndIsCounted) {
    Tracker tracker;
    tracker.SetFrameGeneration(1'000'000.0f / kApplicationIntervalUs, 2, /*fgType=*/2);
    for (int frame = 0; frame < 20; ++frame) {
        const int64_t presentUs = ApplicationPresentUs(frame);
        // Configured on another thread: the game's Presents carry no ID.
        tracker.ObserveApplicationPresent(presentUs, FrameBeginObservation{}, kGameThread);
        if (frame == 0)
            continue;
        const int64_t runtimeUs = presentUs + 1'000;
        tracker.ObserveDisplay(runtimeUs + kPresentToDisplayUs, runtimeUs, GeneratorFrameToken(frame - 1));
    }
    const auto diagnostics = tracker.GetDiagnostics();
    EXPECT_EQ(diagnostics.generatorFramesMatchedById, 0u);
    EXPECT_GT(diagnostics.generatorFramesUnmatchedById, 10u);
    EXPECT_GT(tracker.GetSnapshot(ApplicationPresentUs(20)).sampleCount, 0u);
}

TEST(SystemLatencyGeneratorIdentityTest, APassthroughProxyWithoutFrameGenerationOffersNoIdentity) {
    // Talos keeps FSR's proxy after FG is switched off; its callback then
    // reports frame ID 0 for every output while the game's Presents carry
    // none. That is not a failed match and must not read as one.
    Tracker tracker;
    tracker.SetFrameGeneration(0.0f, 1, /*fgType=*/0);
    for (int frame = 0; frame < 20; ++frame) {
        const int64_t presentUs = ApplicationPresentUs(frame);
        tracker.ObserveApplicationPresent(presentUs, FrameBeginObservation{}, kGameThread);
        const int64_t runtimeUs = presentUs + 1'000;
        tracker.ObserveDisplay(runtimeUs + kPresentToDisplayUs, runtimeUs, GeneratorFrameToken(0));
    }
    const auto diagnostics = tracker.GetDiagnostics();
    EXPECT_EQ(diagnostics.generatorFramesMatchedById, 0u);
    EXPECT_EQ(diagnostics.generatorFramesUnmatchedById, 0u);
    EXPECT_GT(tracker.GetSnapshot(ApplicationPresentUs(20)).sampleCount, 0u);
}

TEST(SystemLatencyGeneratorIdentityTest, HalfTheScanoutIsAddedToBothPaths) {
    auto run = [](Tracker& tracker, int64_t refreshPeriodUs, bool markers) {
        tracker.SetDisplayScanoutPeriod(refreshPeriodUs);
        ce::system_latency::NativeReport report{};
        for (int frame = 0; frame < 40; ++frame) {
            const int64_t presentUs = 40'000'000 + 10'000 * frame;
            tracker.ObservePresent(presentUs);
            tracker.ObserveDisplay(presentUs + 3'000, presentUs);
            if (frame >= 40 - static_cast<int>(report.frames.size()) && report.count < report.frames.size()) {
                auto& marker = report.frames[report.count++];
                marker.frameId = static_cast<uint64_t>(frame);
                marker.simulationStartTimeUs = static_cast<uint64_t>(presentUs - 4'000);
                marker.presentStartTimeUs = static_cast<uint64_t>(presentUs - 100);
            }
        }
        if (markers)
            tracker.SubmitNativeReport(report);
    };
    const int64_t nowUs = 40'000'000 + 10'000 * 40;
    for (const bool markers : {false, true}) {
        Tracker without;
        run(without, 0, markers);
        Tracker with;
        run(with, 6'944, markers);
        const auto plain = without.GetSnapshot(nowUs);
        const auto scanned = with.GetSnapshot(nowUs);
        ASSERT_GT(plain.sampleCount, 0u);
        EXPECT_EQ(plain.source, scanned.source);
        EXPECT_NEAR(scanned.milliseconds - plain.milliseconds, 3.472f, 0.002f) << "markers=" << markers;
        EXPECT_EQ(with.GetDiagnostics().scanoutToCenterUs, 3'472);
    }
    // A period no display has is not a scanout.
    Tracker implausible;
    implausible.SetDisplayScanoutPeriod(250'000);
    EXPECT_EQ(implausible.GetDiagnostics().scanoutToCenterUs, 0);
}

TEST(SystemLatencyGeneratorIdentityTest, MeasuredRatesFollowTheScreenNotTheRuntimesFigure) {
    PerformanceMetrics metrics;
    SharedDisplayTiming timing;
    timing.Reset(4321, 0, DisplayTimingStatus::Starting);
    metrics.ConsumeDisplayTiming(timing, 49'000'000);
    // The runtime-reported figures as Talos froze them under DLSS-G.
    metrics.SetFGMetrics(133.2f, 66.6f, 2, /*fgType=*/1);
    constexpr int64_t kScreenIntervalUs = 19'200;
    int64_t nowUs = 0;
    for (int frame = 0; frame < 40; ++frame) {
        const int64_t applicationUs = 50'000'000 + 2 * kScreenIntervalUs * frame;
        metrics.ObserveApplicationPresent(applicationUs);
        for (int output = 0; output < 2; ++output) {
            const int64_t presentUs = applicationUs + 1'000 + kScreenIntervalUs * output;
            nowUs = presentUs + 3'000;
            timing.Publish(nowUs, nowUs + 100, presentUs);
        }
        metrics.ConsumeDisplayTiming(timing, nowUs + 200);
    }
    EXPECT_NEAR(metrics.GetFGOutputFPS(), 1'000'000.0f / kScreenIntervalUs, 0.1f);
    EXPECT_NEAR(metrics.GetFGBaseFPS(), 1'000'000.0f / (2 * kScreenIntervalUs), 0.1f);
    EXPECT_FLOAT_EQ(metrics.GetReportedFGOutputFPS(), 133.2f);
    EXPECT_FLOAT_EQ(metrics.GetReportedFGBaseFPS(), 66.6f);

    // Long after the stream stopped the reported figures stand in again.
    metrics.ConsumeDisplayTiming(timing, nowUs + 5'000'000);
    EXPECT_FLOAT_EQ(metrics.GetFGOutputFPS(), 133.2f);
}

TEST(SystemLatencyGeneratorIdentityTest, AnIdleGeneratorShowsEqualBaseAndOutputRates) {
    // DLSS-G configured for 4x but not interpolating (a menu): every
    // application frame reaches the screen once, and the readout says so.
    PerformanceMetrics metrics;
    SharedDisplayTiming timing;
    timing.Reset(4321, 0, DisplayTimingStatus::Starting);
    metrics.ConsumeDisplayTiming(timing, 59'000'000);
    metrics.SetFGMetrics(133.2f, 33.3f, 4, /*fgType=*/1);
    int64_t nowUs = 0;
    for (int frame = 0; frame < 40; ++frame) {
        const int64_t presentUs = 60'000'000 + 7'226 * frame;
        metrics.ObserveApplicationPresent(presentUs);
        nowUs = presentUs + 500;
        timing.Publish(nowUs, nowUs + 100, presentUs + 100);
        metrics.ConsumeDisplayTiming(timing, nowUs + 200);
    }
    EXPECT_NEAR(metrics.GetFGOutputFPS(), 138.4f, 0.2f);
    EXPECT_NEAR(metrics.GetFGBaseFPS(), 138.4f, 0.2f);
}

TEST(SystemLatencyGeneratorIdentityTest, FrameIdTravelsFromConfigureAndCallbackIntoTheTracker) {
    ce::present_association::Reset();
    FrameBeginClock::Get().Reset();
    PerformanceMetrics metrics;
    SharedDisplayTiming timing;
    timing.Reset(4321, 0, DisplayTimingStatus::Starting);
    metrics.ConsumeDisplayTiming(timing, 69'000'000);
    metrics.SetFGMetrics(140.0f, 70.0f, 2, /*fgType=*/2);
    for (int frame = 0; frame < 30; ++frame) {
        const int64_t applicationUs = 70'000'000 + kApplicationIntervalUs * frame;
        // ffxConfigure then the proxy Present, on the game thread.
        ce::system_latency::NoteGeneratorFrameConfigured(GeneratorFrameToken(500 + frame));
        metrics.ObserveApplicationPresent(applicationUs);
        if (frame < 1)
            continue;
        // The generator's presenter: callback, then its own Present.
        for (int output = 0; output < 2; ++output) {
            const int64_t callbackEndUs = applicationUs + 900 + kOutputIntervalUs * output;
            const int64_t presentUs = callbackEndUs + 100;
            ce::present_association::NoteCallbackEnd(callbackEndUs, output == 0, GeneratorFrameToken(500 + frame - 1));
            ce::present_association::NotePresentEntry(presentUs);
            timing.Publish(presentUs + kPresentToDisplayUs, presentUs + kPresentToDisplayUs + 100, presentUs);
        }
        metrics.ConsumeDisplayTiming(timing, applicationUs + kApplicationIntervalUs);
    }
    const auto diagnostics = metrics.GetSystemLatencyDiagnostics(70'000'000 + kApplicationIntervalUs * 30);
    EXPECT_GT(diagnostics.generatorFramesMatchedById, 40u);
    EXPECT_EQ(diagnostics.generatorFramesUnmatchedById, 0u);
    EXPECT_EQ(diagnostics.generatorQueueDepthById, 1);
    ce::present_association::Reset();
}

TEST(SystemLatencyGeneratorIdentityTest, ConfiguredFrameIdBelongsToTheConfiguringThreadOnly) {
    ce::system_latency::NoteGeneratorFrameConfigured(GeneratorFrameToken(7));
    uint64_t seenElsewhere = 1;
    std::thread other([&] { seenElsewhere = ce::system_latency::ConsumeGeneratorFrameConfigured(); });
    other.join();
    EXPECT_EQ(seenElsewhere, 0u);
    EXPECT_EQ(ce::system_latency::ConsumeGeneratorFrameConfigured(), GeneratorFrameToken(7));
    // Consumed once: the next Present without a configure carries no ID.
    EXPECT_EQ(ce::system_latency::ConsumeGeneratorFrameConfigured(), 0u);
}

TEST(SystemLatencyGeneratorIdentityTest, PresentStartMarkerThreadIsAttributedAndCounted) {
    FrameBeginClock& clock = FrameBeginClock::Get();
    clock.Reset();
    clock.NoteMarkerFrame(1'000'000, 1'004'000, kGameThread);
    EXPECT_EQ(clock.Observe(1'004'050, kGameThread).markerThread, MarkerThread::Presenting);
    EXPECT_EQ(clock.Observe(1'004'050, kGameThread + 1).markerThread, MarkerThread::Other);
    EXPECT_EQ(clock.Observe(1'004'050, 0).markerThread, MarkerThread::Unknown);

    Tracker tracker;
    for (int frame = 0; frame < 10; ++frame) {
        const int64_t presentUs = 2'000'000 + 10'000 * frame;
        FrameBeginObservation begin;
        begin.beginUs = presentUs - 2'000;
        begin.kind = FrameBeginKind::SimulationMarker;
        begin.markerPresentUs = presentUs - 40;
        begin.markerThread = frame % 2 == 0 ? MarkerThread::Presenting : MarkerThread::Other;
        tracker.ObservePresent(presentUs, begin, kGameThread);
    }
    const auto diagnostics = tracker.GetDiagnostics();
    EXPECT_EQ(diagnostics.markerOnPresentingThread, 5u);
    EXPECT_EQ(diagnostics.markerOnOtherThread, 5u);
    EXPECT_EQ(diagnostics.markerToPresentUs, 40);
    clock.Reset();
}
