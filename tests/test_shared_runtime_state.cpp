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
