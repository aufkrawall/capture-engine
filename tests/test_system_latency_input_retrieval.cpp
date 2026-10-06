#include <gtest/gtest.h>

#include <windows.h>

#include <vector>

#include "captureengine/display_timing/display_timing_input.h"
#include "common/ipc/display_timing_shared.h"
#include "hook/metrics/performance_metrics.h"
#include "hook/metrics/system_latency_metrics.h"

// The presenting thread's input-message retrieval as the measured start of a
// frame without latency markers (Win32k RetrieveInputMessage via the sensor).

namespace {

using ce::system_latency::FrameBeginKind;
using ce::system_latency::FrameBeginObservation;
using ce::system_latency::Tracker;

constexpr uint32_t kLoopThread = 77;

// A single-threaded loop paced by a limiter that waits before the frame is
// built: input is read at the top of each iteration, the frame is built in
// 1.8 ms and presented; the runtime PresentStart follows 200 us later.
void FeedFrontLoadedLoop(Tracker& tracker, uint32_t inputThreadId, int frameCount) {
    const int64_t intervalUs = 11'111;
    for (int i = 0; i < frameCount; ++i) {
        const int64_t loopStartUs = 5'000'000 + intervalUs * i;
        tracker.ObserveInputRetrieval(loopStartUs, inputThreadId);
        const int64_t presentUs = loopStartUs + 1'800;
        tracker.ObservePresent(presentUs, FrameBeginObservation{}, kLoopThread);
        tracker.ObserveDisplay(presentUs + 200 + 400, presentUs + 200);
    }
}

}  // namespace

TEST(DisplayInputRetrievalBurstsTest, OneTimestampPerPumpLoopAndThread) {
    DisplayInputRetrievalBursts bursts;
    bursts.SetMaximumGap(500);
    std::vector<DisplayInputRetrievalBursts::Burst> emitted;
    auto collect = [&](const DisplayInputRetrievalBursts::Burst& burst) { emitted.push_back(burst); };

    // Thread 1 pumps three messages, thread 2 one, interleaved.
    bursts.Observe(10, 1, 1'000, collect);
    bursts.Observe(10, 2, 1'050, collect);
    bursts.Observe(10, 1, 1'100, collect);
    bursts.Observe(10, 1, 1'300, collect);
    EXPECT_TRUE(emitted.empty());

    // The stream moving past the gap ends both bursts at their last retrieval.
    bursts.FlushBefore(1'700, collect);
    ASSERT_EQ(emitted.size(), 1u);
    EXPECT_EQ(emitted[0].threadId, 2u);
    EXPECT_EQ(emitted[0].endTimestamp, 1'050);
    bursts.FlushBefore(1'900, collect);
    ASSERT_EQ(emitted.size(), 2u);
    EXPECT_EQ(emitted[1].threadId, 1u);
    EXPECT_EQ(emitted[1].endTimestamp, 1'300);
    EXPECT_EQ(emitted[1].retrievals, 3u);

    // The next loop on the same thread starts a new burst and delivers the
    // previous one at once.
    bursts.Observe(10, 3, 5'000, collect);
    bursts.Observe(10, 3, 9'000, collect);
    ASSERT_EQ(emitted.size(), 3u);
    EXPECT_EQ(emitted[2].endTimestamp, 5'000);
    EXPECT_EQ(bursts.observed(), 6u);
    EXPECT_EQ(bursts.emitted(), 3u);
}

TEST(DisplayInputRetrievalBurstsTest, SharedRingDeliversRetrievalsAndResetsWithTheGeneration) {
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);
    timing.PublishInputRetrieval(2'000'000, 42);
    timing.PublishInputRetrieval(2'010'000, 43);

    int64_t timeUs = 0;
    uint32_t threadId = 0;
    ASSERT_TRUE(timing.ReadInputRetrieval(2, timeUs, threadId));
    EXPECT_EQ(timeUs, 2'010'000);
    EXPECT_EQ(threadId, 43u);
    EXPECT_FALSE(timing.ReadInputRetrieval(3, timeUs, threadId));

    timing.Reset(1234, 0, DisplayTimingStatus::Starting);
    EXPECT_EQ(timing.inputWriteSequence.load(), 0u);
    EXPECT_FALSE(timing.ReadInputRetrieval(1, timeUs, threadId));
}

TEST(SystemLatencyInputRetrievalTest, PresentingThreadsRetrievalMeasuresTheFrame) {
    // The modelled interval says 11.1 ms of CPU work; the loop actually read
    // its input 2 ms before the runtime Present.
    Tracker modelled;
    FeedFrontLoadedLoop(modelled, /*inputThreadId=*/0, 20);
    Tracker measured;
    FeedFrontLoadedLoop(measured, kLoopThread, 20);

    EXPECT_EQ(modelled.GetDiagnostics().lastAnchorToPresentUs, 11'111);
    const auto diagnostics = measured.GetDiagnostics();
    EXPECT_EQ(diagnostics.lastFrameBeginKind, FrameBeginKind::InputRetrieval);
    EXPECT_EQ(diagnostics.lastAnchorToPresentUs, 2'000);
    EXPECT_EQ(diagnostics.inputRetrievalsObserved, 20u);
    EXPECT_GT(diagnostics.anchorKindSamples[static_cast<size_t>(FrameBeginKind::InputRetrieval)], 0u);
}

TEST(SystemLatencyInputRetrievalTest, RetrievalOnAnotherThreadIsNotThePresentedFrame) {
    // A game thread reads input for a later frame than the render thread is
    // presenting; pairing it would understate the span by whole frames.
    Tracker tracker;
    FeedFrontLoadedLoop(tracker, /*inputThreadId=*/kLoopThread + 1, 20);

    const auto diagnostics = tracker.GetDiagnostics();
    EXPECT_EQ(diagnostics.lastFrameBeginKind, FrameBeginKind::Modelled);
    EXPECT_EQ(diagnostics.lastAnchorToPresentUs, 11'111);
    EXPECT_GT(diagnostics.inputRetrievalFramesOnOtherThread, 0u);
}

TEST(SystemLatencyInputRetrievalTest, PerformanceMetricsConsumesRetrievalsBeforeDisplays) {
    PerformanceMetrics metrics;
    SharedDisplayTiming timing;
    timing.Reset(4321, 0, DisplayTimingStatus::Starting);
    metrics.ConsumeDisplayTiming(timing, 7'900'000);

    const uint32_t thisThread = GetCurrentThreadId();
    for (int i = 0; i < 16; ++i) {
        const int64_t loopStartUs = 8'000'000 + 10'000 * i;
        timing.PublishInputRetrieval(loopStartUs, thisThread);
        metrics.Update(loopStartUs + 3'000);
        timing.Publish(loopStartUs + 3'000 + 200 + 1'000, loopStartUs + 4'500, loopStartUs + 3'000 + 200);
    }
    metrics.ConsumeDisplayTiming(timing, 8'170'000);

    const auto diagnostics = metrics.GetSystemLatencyDiagnostics(8'170'000);
    EXPECT_EQ(diagnostics.lastFrameBeginKind, FrameBeginKind::InputRetrieval);
    EXPECT_EQ(diagnostics.lastAnchorToPresentUs, 3'200);
    EXPECT_EQ(diagnostics.inputRetrievalsObserved, 16u);
}
