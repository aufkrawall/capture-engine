#include <gtest/gtest.h>

#include "../common/shared_defs.h"

TEST(CaptureStateTest, RuntimeFlagsRoundTrip) {
    CaptureState state;

    EXPECT_FALSE(state.HasRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive));

    state.SetRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive, true);
    EXPECT_TRUE(state.HasRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive));

    state.SetRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive, false);
    EXPECT_FALSE(state.HasRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive));
}

TEST(CaptureStateTest, CaptureRequestAndRecordingVisibilityAreIndependent) {
    CaptureState state;

    EXPECT_FALSE(state.captureRequested.load(std::memory_order_relaxed));
    EXPECT_FALSE(state.isRecording.load(std::memory_order_relaxed));

    state.captureRequested.store(true, std::memory_order_relaxed);
    EXPECT_TRUE(state.captureRequested.load(std::memory_order_relaxed));
    EXPECT_FALSE(state.isRecording.load(std::memory_order_relaxed));

    state.isRecording.store(true, std::memory_order_relaxed);
    EXPECT_TRUE(state.captureRequested.load(std::memory_order_relaxed));
    EXPECT_TRUE(state.isRecording.load(std::memory_order_relaxed));

    state.captureRequested.store(false, std::memory_order_relaxed);
    EXPECT_FALSE(state.captureRequested.load(std::memory_order_relaxed));
    EXPECT_TRUE(state.isRecording.load(std::memory_order_relaxed));
}

TEST(CaptureStateTest, WgcDiagnosticsFieldsDefaultToZero) {
    CaptureState state;

    EXPECT_EQ(state.wgcDeliveredFramesPerSec.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcDeliveredMin250Fps.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcDeliveredMin500Fps.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcInputMin250Fps.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcInputMin500Fps.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcQueueEmptyTickPermille.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcBufferedAtTickAvgPermille.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcBufferedAtTickMin.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcStarvedTickCount.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(state.wgcSingleFrameTickCount.load(std::memory_order_relaxed), 0u);
}

TEST(CaptureStateTest, WgcTelemetryFieldsRepresentFreshnessAndReserveCounters) {
    CaptureState state;

    state.wgcQueueEmptyTickPermille.store(375, std::memory_order_relaxed);
    state.wgcStarvedTickCount.store(9, std::memory_order_relaxed);
    state.wgcSingleFrameTickCount.store(15, std::memory_order_relaxed);

    EXPECT_EQ(state.wgcQueueEmptyTickPermille.load(std::memory_order_relaxed), 375u);
    EXPECT_EQ(state.wgcStarvedTickCount.load(std::memory_order_relaxed), 9u);
    EXPECT_EQ(state.wgcSingleFrameTickCount.load(std::memory_order_relaxed), 15u);
}
