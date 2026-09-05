#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <thread>
#include <type_traits>
#include <vector>

#include "../captureengine/display_timing_correlation.h"
#include "../hook/common/performance_metrics.h"
#include "source_fragment_reader.h"

namespace {

TEST(DisplayPacingIntegrityTest, HistoryReadersUseLockFreeAtomicValues) {
    static_assert(std::atomic<float>::is_always_lock_free);
    PerformanceMetrics metrics;
    static_assert(std::is_same_v<decltype(metrics.GetHistoryArray()), const std::atomic<float>*>);
    metrics.Update(1'000'000);
    metrics.Update(1'016'000);
    EXPECT_FLOAT_EQ(metrics.GetHistoryArray()[0].load(std::memory_order_relaxed), 16.0f);
}

TEST(DisplayPacingIntegrityTest, ConcurrentPublicationAfterFrameEntryCannotTriggerPresentationFallback) {
    SharedDisplayTiming timing;
    PerformanceMetrics metrics;
    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    timing.Publish(1'000'000, 2'000'000);
    timing.Publish(1'008'000, 2'000'001);
    // QPC was captured before the sensor published, or before the consumer lock.
    metrics.ConsumeDisplayTiming(timing, 1'999'990);
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    EXPECT_FLOAT_EQ(metrics.GetLastDisplayFrameTimeMs(), 8.0f);
    metrics.ConsumeDisplayTiming(timing, 4'000'002);
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::Presentation);
}

TEST(DisplayPacingIntegrityTest, DistinctVeryShortDisplayIntervalsAreNeverMerged) {
    SharedDisplayTiming timing;
    PerformanceMetrics metrics;
    timing.Publish(1'000'000, 2'000'000);
    timing.Publish(1'000'050, 2'000'001);
    timing.Publish(1'010'000, 2'000'002);
    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.ConsumeDisplayTiming(timing, 2'000'010);
    ASSERT_EQ(metrics.GetSampleCount(), 2u);
    std::array<float, 2> history{};
    metrics.GetLastHistory(history.data(), static_cast<int>(history.size()));
    EXPECT_FLOAT_EQ(history[0], 0.05f);
    EXPECT_FLOAT_EQ(history[1], 9.95f);
}

TEST(DisplayPacingIntegrityTest, LongDisplayedFramesReduceCurrentFPSAndRemainInGraph) {
    SharedDisplayTiming timing;
    PerformanceMetrics metrics;
    int64_t time = 1'000'000;
    timing.Publish(time, time);
    for (int i = 0; i < 59; ++i) {
        time += 10'000;
        timing.Publish(time, time);
    }
    time += 250'000;
    timing.Publish(time, time);
    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.ConsumeDisplayTiming(timing, time);
    EXPECT_NEAR(metrics.GetCurrentFPS(), 60'000.0f / 840.0f, 0.001f);
    EXPECT_FLOAT_EQ(metrics.GetLastDisplayFrameTimeMs(), 250.0f);
    EXPECT_FLOAT_EQ(metrics.GetMaxFrameTime(5.0f), 250.0f);
}

TEST(DisplayPacingIntegrityTest, WorstPercentileDoesNotDiluteOneHitchWithAnExtraFastFrame) {
    PerformanceMetrics metrics;
    int64_t time = 1'000'000;
    metrics.Update(time);
    for (int i = 0; i < 99; ++i) {
        time += 10'000;
        metrics.Update(time);
    }
    metrics.Update(time + 100'000);
    // Exactly one of the 100 frames is in the worst 1%.
    EXPECT_FLOAT_EQ(metrics.Get1PercentLowFPS(), 10.0f);
    EXPECT_FLOAT_EQ(metrics.Get01PercentLowFPS(), 10.0f);
}

TEST(DisplayPacingIntegrityTest, RingOverrunDoesNotTurnMissingTelemetryIntoOneLongFrame) {
    SharedDisplayTiming timing;
    PerformanceMetrics metrics;
    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    int64_t time = 1'000'000;
    timing.Publish(time, time);
    time += 10'000;
    timing.Publish(time, time);
    metrics.ConsumeDisplayTiming(timing, time);
    ASSERT_EQ(metrics.GetSampleCount(), 1u);
    for (std::size_t i = 0; i < DISPLAY_TIMING_RING_SIZE + 20; ++i) {
        time += 10'000;
        timing.Publish(time, time);
    }
    metrics.ConsumeDisplayTiming(timing, time);
    EXPECT_EQ(metrics.GetSampleCount(), DISPLAY_TIMING_RING_SIZE);
    EXPECT_FLOAT_EQ(metrics.GetMaxFrameTime(20.0f), 10.0f);
    EXPECT_DOUBLE_EQ(metrics.GetWindowStdDev(), 0.0);
}

TEST(DisplayPacingIntegrityTest, KernelCompletionsRetainMeasuredIntervalsAndProvenance) {
    DisplayTimingCorrelation correlation;
    std::vector<PendingTimestamp> pending;
    uint64_t order = 0;
    constexpr std::array<int64_t, 6> times = {
        1'000'000, 1'004'700, 1'013'900, 1'038'700, 1'038'750, 1'250'000};
    for (std::size_t i = 0; i < times.size(); ++i)
        correlation.QueueFallback(42, i + 1, times[i], DisplayCompletionKind::Sync,
                                   pending, order, times[i] - 1000, 3);
    SharedDisplayTiming timing;
    for (auto& completion : pending) {
        ASSERT_TRUE(correlation.ShouldPublish(completion));
        EXPECT_TRUE(completion.screenTimeResolved);
        timing.Publish(completion.timestamp, 2'000'000, completion.presentStartTimestamp,
                        completion.screenTimeResolved);
        correlation.CommitFallback(completion);
    }
    PerformanceMetrics metrics;
    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.ConsumeDisplayTiming(timing, 2'000'001);
    std::array<float, times.size() - 1> history{};
    metrics.GetLastHistory(history.data(), static_cast<int>(history.size()));
    for (std::size_t i = 0; i < history.size(); ++i)
        EXPECT_FLOAT_EQ(history[i], static_cast<float>(times[i + 1] - times[i]) / 1000.0f);
    // Neither service drain may reintroduce the old grid rewrite after reduction.
    const auto service = ce::test_source::ReadLogicalSource(
        std::filesystem::current_path() / "captureengine/display_timing_service.cpp");
    EXPECT_EQ(service.find("ResolveDeferredScreenTimes"), std::string::npos);
    EXPECT_EQ(service.find(".Claim("), std::string::npos);
    EXPECT_EQ(service.find(".Snap("), std::string::npos);
}

TEST(DisplayPacingIntegrityTest, SharedRingWrapNeverAcceptsMixedPayloadsUnderAnOldSequence) {
    SharedDisplayTiming timing;
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (uint64_t sequence = 1; sequence <= 100'000; ++sequence) {
            const auto timestamp = static_cast<int64_t>(sequence) * 100;
            timing.Publish(timestamp, timestamp, timestamp - 37, (sequence & 1u) != 0);
        }
        done.store(true, std::memory_order_release);
    });
    bool coherent = true;
    start.store(true, std::memory_order_release);
    do {
        const uint64_t newest = timing.writeSequence.load(std::memory_order_acquire);
        const uint64_t oldest = newest >= DISPLAY_TIMING_RING_SIZE ? newest - DISPLAY_TIMING_RING_SIZE + 1 : 1;
        for (uint64_t sequence = oldest; sequence <= newest; ++sequence) {
            int64_t screen = 0, present = 0;
            bool resolved = false;
            if (timing.Read(sequence, screen, present, resolved))
                coherent &= screen == static_cast<int64_t>(sequence) * 100 && present == screen - 37 &&
                            resolved == ((sequence & 1u) != 0);
        }
    } while (!done.load(std::memory_order_acquire));
    producer.join();
    EXPECT_TRUE(coherent);
}

TEST(DisplayPacingIntegrityTest, SharedReadRejectsResetInProgressEvenBeforeItsSlotIsCleared) {
    SharedDisplayTiming timing;
    timing.Publish(1'000'000, 1'010'000);
    timing.publicationGeneration.fetch_add(1, std::memory_order_acq_rel);
    int64_t screen = 0;
    EXPECT_FALSE(timing.Read(1, screen));
    timing.publicationGeneration.fetch_add(1, std::memory_order_release);
    timing.Reset(7, 0, DisplayTimingStatus::Starting);
    timing.Publish(2'000'000, 2'010'000);
    ASSERT_TRUE(timing.Read(1, screen));
    EXPECT_EQ(screen, 2'000'000);
}

}  // namespace
