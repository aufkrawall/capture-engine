#include <gtest/gtest.h>

#include "hook/d3d9/dx9_capture_cost.h"

using ce::dx9_capture_cost::Accumulator;

TEST(DX9CaptureCostTest, DeferredEndSceneCaptureIsReported) {
    Accumulator cost;
    cost.AddRingFrame(3, 12, true);
    cost.AddDeferredCapture(19);
    const auto snapshot = cost.Consume();
    EXPECT_EQ(snapshot.deferredCaptureUs, 19);
    EXPECT_EQ(snapshot.ringDrainUs, 3);
    EXPECT_EQ(snapshot.ringSubmitUs, 12);
    EXPECT_EQ(snapshot.ringSubmits, 1u);
}

TEST(DX9CaptureCostTest, ConsumeResetsSoSkippedFramesReportZero) {
    Accumulator cost;
    cost.AddRingFrame(4, 10, true);
    cost.AddDeferredCapture(15);
    (void)cost.Consume();
    const auto skipped = cost.Consume();
    EXPECT_EQ(skipped.deferredCaptureUs, 0);
    EXPECT_EQ(skipped.ringDrainUs, 0);
    EXPECT_EQ(skipped.ringSubmitUs, 0);
    EXPECT_EQ(skipped.ringSubmits, 0u);
}

TEST(DX9CaptureCostTest, AccumulatesSeveralCapturesInOnePresent) {
    Accumulator cost;
    cost.AddRingFrame(1, 5, true);
    cost.AddRingFrame(2, 0, false);  // drained but no free slot: no submit
    const auto snapshot = cost.Consume();
    EXPECT_EQ(snapshot.ringDrainUs, 3);
    EXPECT_EQ(snapshot.ringSubmitUs, 5);
    EXPECT_EQ(snapshot.ringSubmits, 1u);
}

TEST(DX9CaptureCostTest, NegativeAndHugeDurationsAreBounded) {
    Accumulator cost;
    cost.AddDeferredCapture(-7);
    cost.AddRingFrame(-1, INT64_C(5000000000), true);
    const auto snapshot = cost.Consume();
    EXPECT_EQ(snapshot.deferredCaptureUs, 0);
    EXPECT_EQ(snapshot.ringDrainUs, 0);
    EXPECT_EQ(snapshot.ringSubmitUs, INT32_MAX);
}
