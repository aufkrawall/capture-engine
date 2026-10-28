#include <gtest/gtest.h>

#include <algorithm>

#include "captureengine/display_timing/display_timing_submissions.h"

TEST(DisplayTimingRuntimePresentTest, StatusProbesCannotInventAnOldDisplayQueue) {
    DisplaySubmissionTracker tracker;
    uint64_t admittedProbes = 0;
    int64_t largestPresentToDisplayUs = 0;
    constexpr int64_t kIntervalUs = 7'142;  // ~140 fps, as in the Metro session
    constexpr int64_t kActualPresentToDisplayUs = 1'000;
    constexpr uint32_t kFrames = 100;
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        const int64_t presentUs = 1'000'000 + frame * kIntervalUs;
        admittedProbes += tracker.ObserveRuntimePresent(100, 10, presentUs - 100, 0, DXGI_PRESENT_TEST) ? 1 : 0;
        ASSERT_TRUE(tracker.ObserveRuntimePresent(100, 10, presentUs, 0, 0));
        ASSERT_TRUE(tracker.Associate(100, 10, frame, presentUs + 50));
        const auto* association = tracker.Find(frame);
        ASSERT_NE(association, nullptr);
        const int64_t screenUs = presentUs + kActualPresentToDisplayUs;
        largestPresentToDisplayUs =
            std::max(largestPresentToDisplayUs, screenUs - association->presentStartTimestamp);
        tracker.Erase(frame);
    }
    // Counting both calls fills the bounded runtime queue with probes that never submit,
    // eventually associating the current flip with a Present about seven frames old.
    EXPECT_EQ(admittedProbes, 0u);
    EXPECT_EQ(tracker.observedPresents(), kFrames);
    EXPECT_EQ(tracker.ignoredTestPresents(), kFrames);
    EXPECT_EQ(tracker.observedAssociations(), kFrames);
    EXPECT_EQ(largestPresentToDisplayUs, kActualPresentToDisplayUs);
}

TEST(DisplayTimingRuntimePresentTest, TestFlagIsIgnoredAlongsideOtherPresentFlags) {
    DisplaySubmissionTracker tracker;
    EXPECT_FALSE(tracker.ObserveRuntimePresent(100, 10, 100, 0,
                                              DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT));
    EXPECT_TRUE(tracker.ObserveRuntimePresent(100, 10, 200, 0, DXGI_PRESENT_DO_NOT_WAIT));
    // The real Present may be submitted by a runtime worker, on a different thread.
    ASSERT_TRUE(tracker.Associate(100, 20, 1, 250));
    const auto* association = tracker.Find(1);
    ASSERT_NE(association, nullptr);
    EXPECT_EQ(association->presentStartTimestamp, 200);
    EXPECT_EQ(tracker.observedPresents(), 1u);
    EXPECT_EQ(tracker.ignoredTestPresents(), 1u);
}
