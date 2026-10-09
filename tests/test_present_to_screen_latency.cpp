#include <gtest/gtest.h>

#include <memory>

#include "common/capture/capture_policy/present_to_screen_latency.h"

namespace {

namespace policy = ce::capture_policy;

std::unique_ptr<SharedDisplayTiming> MakeTiming() {
    auto timing = std::make_unique<SharedDisplayTiming>();
    timing->Reset(1234, 1234, DisplayTimingStatus::Starting);
    return timing;
}

void PublishSteady(SharedDisplayTiming& timing, int count, int64_t gapUs, int64_t intervalUs = 6944) {
    int64_t presentUs = 1'000'000;
    for (int i = 0; i < count; ++i) {
        timing.Publish(presentUs + gapUs, presentUs + gapUs, presentUs, true);
        presentUs += intervalUs;
    }
}

}  // namespace

TEST(PresentToScreenLatencyTest, MedianOfSteadyFlipQueueLatency) {
    auto timing = MakeTiming();
    PublishSteady(*timing, 200, 27800);
    const auto estimate = policy::EstimatePresentToScreenLatency(*timing);
    ASSERT_TRUE(estimate.valid);
    EXPECT_EQ(estimate.medianUs, 27800);
    EXPECT_EQ(estimate.minUs, 27800);
    EXPECT_EQ(estimate.maxUs, 27800);
    EXPECT_EQ(estimate.samples, 200u);
}

TEST(PresentToScreenLatencyTest, OutliersDoNotMoveTheMedian) {
    auto timing = MakeTiming();
    int64_t presentUs = 1'000'000;
    for (int i = 0; i < 120; ++i) {
        // Every tenth present is a stall of 80 ms; the steady latency is 21 ms.
        const int64_t gapUs = (i % 10 == 9) ? 80000 : 21000;
        timing->Publish(presentUs + gapUs, presentUs + gapUs, presentUs, true);
        presentUs += 6944;
    }
    const auto estimate = policy::EstimatePresentToScreenLatency(*timing);
    ASSERT_TRUE(estimate.valid);
    EXPECT_EQ(estimate.medianUs, 21000);
    EXPECT_EQ(estimate.maxUs, 80000);
}

TEST(PresentToScreenLatencyTest, RejectsUnresolvedMissingAndImplausiblePairs) {
    auto timing = MakeTiming();
    int64_t presentUs = 1'000'000;
    for (int i = 0; i < 100; ++i) {
        timing->Publish(presentUs + 9000, presentUs + 9000, presentUs, /*screenTimeResolved=*/false);
        timing->Publish(presentUs + 9000, presentUs + 9000, /*presentStartTimeUs=*/0, true);
        timing->Publish(presentUs + 900000, presentUs + 900000, presentUs, true);  // beyond the plausible bound
        timing->Publish(presentUs - 500, presentUs, presentUs, true);                // screen before Present
        presentUs += 6944;
    }
    EXPECT_FALSE(policy::EstimatePresentToScreenLatency(*timing).valid);
}

TEST(PresentToScreenLatencyTest, NeedsEnoughSamplesAndAnActiveRing) {
    auto timing = MakeTiming();
    EXPECT_FALSE(policy::EstimatePresentToScreenLatency(*timing).valid);  // nothing published
    PublishSteady(*timing, static_cast<int>(policy::kPresentToScreenMinSamples) - 1, 15000);
    EXPECT_FALSE(policy::EstimatePresentToScreenLatency(*timing).valid);
    PublishSteady(*timing, 1, 15000);
    EXPECT_TRUE(policy::EstimatePresentToScreenLatency(*timing).valid);

    timing->SetStatus(DisplayTimingStatus::Failed);
    EXPECT_FALSE(policy::EstimatePresentToScreenLatency(*timing).valid);
}

TEST(PresentToScreenLatencyTest, UsesOnlyTheNewestWindowWhenTheRingHasWrapped) {
    auto timing = MakeTiming();
    PublishSteady(*timing, 700, 30000);  // older queue state, ring (512) has wrapped
    PublishSteady(*timing, 240, 4000);   // the queue drained (app now frame-capped)
    const auto estimate = policy::EstimatePresentToScreenLatency(*timing);
    ASSERT_TRUE(estimate.valid);
    EXPECT_EQ(estimate.medianUs, 4000);
    EXPECT_EQ(estimate.maxUs, 4000);
}

TEST(PresentToScreenLatencyTest, ScreenGrabContentDelayTakesTheMeasuredQueueOffTheProbeLatency) {
    policy::PresentToScreenLatency measured;
    measured.valid = true;
    measured.samples = 240;

    measured.medianUs = 20000;  // uncapped, three frames queued at 144 Hz
    EXPECT_NEAR(policy::ComputeScreenGrabContentDelayMs(31.2, measured), 11.2, 1e-9);
    EXPECT_NEAR(policy::ComputeScreenGrabLatencyReductionMs(31.2, measured), 20.0, 1e-9);
    measured.medianUs = 6800;  // windowed
    EXPECT_NEAR(policy::ComputeScreenGrabContentDelayMs(31.2, measured), 24.4, 1e-9);
    measured.medianUs = 350;  // frame-capped, empty queue: the probe latency stands
    EXPECT_NEAR(policy::ComputeScreenGrabContentDelayMs(31.2, measured), 30.85, 1e-9);
    measured.medianUs = 45000;  // a deeper queue than the probe latency never turns into a negative delay
    EXPECT_DOUBLE_EQ(policy::ComputeScreenGrabContentDelayMs(31.2, measured), 0.0);
    EXPECT_NEAR(policy::ComputeScreenGrabLatencyReductionMs(31.2, measured), 31.2, 1e-9);
}

TEST(PresentToScreenLatencyTest, ScreenGrabContentDelayKeepsTheProbeLatencyWithoutAMeasurement) {
    const policy::PresentToScreenLatency none;  // no hook or display timing not active yet
    EXPECT_DOUBLE_EQ(policy::ComputeScreenGrabContentDelayMs(31.2, none), 31.2);
    EXPECT_DOUBLE_EQ(policy::ComputeScreenGrabLatencyReductionMs(31.2, none), 0.0);
    EXPECT_DOUBLE_EQ(policy::ComputeScreenGrabContentDelayMs(0.0, none), 0.0);
    policy::PresentToScreenLatency measured;
    measured.valid = true;
    measured.medianUs = 5000;
    EXPECT_DOUBLE_EQ(policy::ComputeScreenGrabContentDelayMs(0.0, measured), 0.0);
    EXPECT_DOUBLE_EQ(policy::ComputeScreenGrabLatencyReductionMs(0.0, measured), 0.0);
}
