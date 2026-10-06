#include <gtest/gtest.h>

#include "hook/metrics/system_latency_frame_begin.h"
#include "hook/metrics/system_latency_metrics.h"

// Which boundary a frame's input-to-Present span is measured from: the game's
// own frame-ID-paired markers, a low-latency sleep on the presenting thread, a
// span learned from neighbouring frames, or the modelled interval.

namespace {

using ce::system_latency::FrameBeginClock;
using ce::system_latency::FrameBeginKind;
using ce::system_latency::FrameBeginObservation;
using ce::system_latency::Source;
using ce::system_latency::Tracker;

struct ClockReset {
    ClockReset() { FrameBeginClock::Get().Reset(); }
    ~ClockReset() { FrameBeginClock::Get().Reset(); }
};

constexpr uint32_t kGameThread = 101;
constexpr uint32_t kPresentThread = 202;

FrameBeginObservation MarkerFrame(int64_t simulationStartUs, int64_t presentMarkerUs) {
    FrameBeginObservation observation;
    observation.beginUs = simulationStartUs;
    observation.kind = FrameBeginKind::SimulationMarker;
    observation.markerPresentUs = presentMarkerUs;
    return observation;
}

// An engine whose game thread runs `pipelineUs` ahead of the thread that
// presents: frame i starts simulating at simulation_i and is presented
// pipelineUs later on another thread. The runtime PresentStart lands 200 us
// after the hook's timestamp and the frame reaches the screen 5 ms after that.
void FeedPipelinedFrames(Tracker& tracker, int64_t firstSimulationUs, int64_t intervalUs, int64_t pipelineUs,
                         int firstFrame, int frameCount, bool withMarkers) {
    for (int i = firstFrame; i < firstFrame + frameCount; ++i) {
        const int64_t simulationUs = firstSimulationUs + intervalUs * i;
        const int64_t presentUs = simulationUs + pipelineUs;
        FrameBeginObservation frameBegin;
        if (withMarkers)
            frameBegin = MarkerFrame(simulationUs, presentUs - 100);
        tracker.ObservePresent(presentUs, frameBegin, kPresentThread);
        tracker.ObserveDisplay(presentUs + 200 + 5'000, presentUs + 200);
    }
}

}  // namespace

TEST(SystemLatencyAnchorTest, SleepOnAnotherThreadIsNotThePresentedFramesBoundary) {
    // Regression: Unreal sleeps on the game thread and presents on the RHI
    // thread, so the newest sleep belongs to a frame that has not been
    // presented yet. Pairing it shortened the measured span below one frame.
    ClockReset reset;
    ce::system_latency::NoteFrameBegin(1'000'000, FrameBeginKind::LowLatencySleepReturn, kGameThread);

    const auto otherThread = ce::system_latency::ObserveFrameBegin(1'004'000, kPresentThread);
    EXPECT_EQ(otherThread.kind, FrameBeginKind::Modelled);
    EXPECT_EQ(otherThread.beginUs, 0);
    EXPECT_TRUE(otherThread.sleepOnOtherThread);

    const auto sameThread = ce::system_latency::ObserveFrameBegin(1'004'000, kGameThread);
    EXPECT_EQ(sameThread.kind, FrameBeginKind::LowLatencySleepReturn);
    EXPECT_EQ(sameThread.beginUs, 1'000'000);

    // An unknown presenting thread keeps the previous pairing.
    EXPECT_EQ(ce::system_latency::ObserveFrameBegin(1'004'000, 0).kind, FrameBeginKind::LowLatencySleepReturn);
}

TEST(SystemLatencyAnchorTest, MarkerPairTakesPrecedenceAcrossThreads) {
    ClockReset reset;
    ce::system_latency::NoteFrameBegin(1'010'000, FrameBeginKind::LowLatencySleepReturn, kGameThread);
    ce::system_latency::NoteMarkerFrameBegin(985'000, 1'011'000);

    const auto observation = ce::system_latency::ObserveFrameBegin(1'011'200, kPresentThread);
    EXPECT_EQ(observation.kind, FrameBeginKind::SimulationMarker);
    EXPECT_EQ(observation.beginUs, 985'000);
    EXPECT_EQ(observation.markerPresentUs, 1'011'000);

    // A pair whose PresentStart marker is in the future of this Present, or
    // older than the clock's age bound, is not usable.
    EXPECT_EQ(ce::system_latency::ObserveFrameBegin(1'010'500, kGameThread).kind,
              FrameBeginKind::LowLatencySleepReturn);
    EXPECT_EQ(ce::system_latency::ObserveFrameBegin(1'400'000, kPresentThread).kind, FrameBeginKind::Modelled);
}

TEST(SystemLatencyAnchorTest, MarkersMeasureAPipelinedEngineSpanThatTheModelUnderstates) {
    // 2.5 frames from simulation start to Present. The modelled interval says
    // one frame; the frame-ID-paired markers say what the engine actually does.
    Tracker modelled;
    FeedPipelinedFrames(modelled, 1'000'000, 10'000, 25'000, 0, 20, /*withMarkers=*/false);
    Tracker marked;
    FeedPipelinedFrames(marked, 1'000'000, 10'000, 25'000, 0, 20, /*withMarkers=*/true);

    const auto modelledDiagnostics = modelled.GetDiagnostics();
    EXPECT_EQ(modelledDiagnostics.lastFrameBeginKind, FrameBeginKind::Modelled);
    EXPECT_EQ(modelledDiagnostics.lastAnchorToPresentUs, 10'000);

    const auto markedDiagnostics = marked.GetDiagnostics();
    EXPECT_EQ(markedDiagnostics.lastFrameBeginKind, FrameBeginKind::SimulationMarker);
    EXPECT_EQ(markedDiagnostics.lastAnchorToPresentUs, 25'200);
    EXPECT_GT(markedDiagnostics.anchorKindSamples[static_cast<size_t>(FrameBeginKind::SimulationMarker)], 0u);
    EXPECT_EQ(markedDiagnostics.anchorKindSamples[static_cast<size_t>(FrameBeginKind::Modelled)], 0u);

    const int64_t nowUs = 1'000'000 + 10'000 * 20 + 25'000;
    EXPECT_NEAR(marked.GetSnapshot(nowUs).milliseconds - modelled.GetSnapshot(nowUs).milliseconds, 15.2f, 0.1f);
}

TEST(SystemLatencyAnchorTest, MarkerFromAnEarlierFrameIsRejected) {
    // The clock still holds the previous frame's pair when a frame emitted no
    // markers; its PresentStart predates the previous application Present.
    Tracker tracker;
    tracker.ObservePresent(1'000'000, MarkerFrame(980'000, 999'900), kPresentThread);
    tracker.ObservePresent(1'010'000, MarkerFrame(980'000, 999'900), kPresentThread);
    tracker.ObserveDisplay(1'016'000, 1'010'200);

    const auto diagnostics = tracker.GetDiagnostics();
    EXPECT_EQ(diagnostics.markerAnchorsStale, 1u);
    EXPECT_EQ(diagnostics.lastFrameBeginKind, FrameBeginKind::Modelled);
}

TEST(SystemLatencyAnchorTest, SleepOnAnotherThreadIsCountedAndModelled) {
    Tracker tracker;
    FrameBeginObservation crossThread;
    crossThread.sleepOnOtherThread = true;
    for (int i = 0; i < 12; ++i) {
        const int64_t presentUs = 2'000'000 + 10'000 * i;
        tracker.ObservePresent(presentUs, crossThread, kPresentThread);
        tracker.ObserveDisplay(presentUs + 5'200, presentUs + 200);
    }
    const auto diagnostics = tracker.GetDiagnostics();
    EXPECT_EQ(diagnostics.sleepAnchorsOnOtherThread, 12u);
    EXPECT_EQ(diagnostics.lastFrameBeginKind, FrameBeginKind::Modelled);
    EXPECT_EQ(diagnostics.lastAnchorToPresentUs, 10'000);
}

TEST(SystemLatencyAnchorTest, WaitInsideTheApplicationsOwnPresentIsNotCountedTwice) {
    // Regression (Strange Brigade, 90 fps CE limiter cap): the hook is entered
    // 9.3 ms before the runtime PresentStart because CE's limiter waits there.
    // The modelled Present-to-Present interval already contains that wait, so
    // adding it again published 20.4 ms for a frame the game built in 1.8 ms.
    Tracker tracker;
    for (int i = 0; i < 20; ++i) {
        const int64_t hookEntryUs = 3'000'000 + 11'111 * i;
        tracker.ObservePresent(hookEntryUs);
        tracker.ObserveDisplay(hookEntryUs + 9'300 + 400, hookEntryUs + 9'300);
    }
    const auto diagnostics = tracker.GetDiagnostics();
    EXPECT_EQ(diagnostics.lastFrameBeginKind, FrameBeginKind::Modelled);
    EXPECT_EQ(diagnostics.lastAnchorToPresentUs, 11'111);
    EXPECT_EQ(diagnostics.lastPresentToDisplayUs, 400);
    EXPECT_FALSE(diagnostics.generatorHoldApplied);
}

TEST(SystemLatencyAnchorTest, FramesWithoutABoundaryUseTheRecentlyMeasuredSpan) {
    // Markers or input retrievals are not present on every frame. A frame
    // without one, next to frames that measured 25 ms, is not a 10 ms frame.
    Tracker tracker;
    FeedPipelinedFrames(tracker, 1'000'000, 10'000, 25'000, 0, 12, /*withMarkers=*/true);
    FeedPipelinedFrames(tracker, 1'000'000, 10'000, 25'000, 12, 6, /*withMarkers=*/false);

    auto diagnostics = tracker.GetDiagnostics();
    EXPECT_EQ(diagnostics.lastFrameBeginKind, FrameBeginKind::Learned);
    EXPECT_EQ(diagnostics.lastAnchorToPresentUs, 25'200);
    EXPECT_EQ(diagnostics.anchorKindSamples[static_cast<size_t>(FrameBeginKind::Learned)], 6u);

    // Measurements older than the freshness window no longer describe the
    // scene: back to the modelled interval.
    FeedPipelinedFrames(tracker, 1'000'000, 10'000, 25'000, 18 + 250, 6, /*withMarkers=*/false);
    diagnostics = tracker.GetDiagnostics();
    EXPECT_EQ(diagnostics.lastFrameBeginKind, FrameBeginKind::Modelled);
    EXPECT_EQ(diagnostics.lastAnchorToPresentUs, 10'000);
}
