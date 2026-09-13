#include "../captureengine/display_timing_submissions.h"
#include "../common/display_timing_shared.h"
#include "../hook/common/system_latency_metrics.h"

#include <gtest/gtest.h>

TEST(DisplayTimingSubmissionsTest, AssociateWithPendingRuntimePresentsUsesRuntimePresentTimestamp) {
    DisplaySubmissionTracker tracker;
    bool isFallback = true;

    tracker.ObserveRuntimePresent(100, 10, 1'000'000);
    EXPECT_EQ(tracker.observedPresents(), 1u);

    const bool associated = tracker.Associate(100, 10, 42, 1'000'050, &isFallback);
    EXPECT_TRUE(associated);
    EXPECT_FALSE(isFallback);
    EXPECT_EQ(tracker.observedAssociations(), 1u);
    EXPECT_EQ(tracker.observedFallbackAssociations(), 0u);

    const SubmitAssociation* association = tracker.Find(42);
    ASSERT_NE(association, nullptr);
    EXPECT_EQ(association->processId, 100u);
    EXPECT_EQ(association->timestamp, 1'000'050);
    EXPECT_EQ(association->presentStartTimestamp, 1'000'000);

    tracker.Erase(42);
    EXPECT_EQ(tracker.Find(42), nullptr);
}

TEST(DisplayTimingSubmissionsTest, AssociateWithoutRuntimePresentsUsesSubmissionTimestamp) {
    DisplaySubmissionTracker tracker;
    bool isFallback = false;

    // In Vulkan, no runtime present is observed. Associate must still succeed
    // using the submission timestamp as the present start.
    const bool associated = tracker.Associate(200, 20, 99, 2'000'000, &isFallback);
    EXPECT_TRUE(associated);
    EXPECT_TRUE(isFallback);
    EXPECT_EQ(tracker.observedPresents(), 0u);
    EXPECT_EQ(tracker.observedAssociations(), 1u);
    EXPECT_EQ(tracker.observedFallbackAssociations(), 1u);

    const SubmitAssociation* association = tracker.Find(99);
    ASSERT_NE(association, nullptr);
    EXPECT_EQ(association->processId, 200u);
    EXPECT_EQ(association->timestamp, 2'000'000);
    EXPECT_EQ(association->presentStartTimestamp, 2'000'000);

    tracker.Erase(99);
    EXPECT_EQ(tracker.Find(99), nullptr);
}

TEST(DisplayTimingSubmissionsTest, MultipleVulkanSubmissionsAssociateInOrder) {
    DisplaySubmissionTracker tracker;

    for (uint32_t i = 1; i <= 5; ++i) {
        bool isFallback = false;
        const int64_t submitTime = 3'000'000 + i * 16'666;
        EXPECT_TRUE(tracker.Associate(7232, 1, 100 + i, submitTime, &isFallback));
        EXPECT_TRUE(isFallback);
    }
    EXPECT_EQ(tracker.observedAssociations(), 5u);
    EXPECT_EQ(tracker.observedFallbackAssociations(), 5u);

    uint64_t previousAssociationId = 0;
    for (uint32_t i = 1; i <= 5; ++i) {
        const SubmitAssociation* association = tracker.Find(100 + i);
        ASSERT_NE(association, nullptr);
        EXPECT_EQ(association->processId, 7232u);
        EXPECT_GT(association->associationId, previousAssociationId);
        previousAssociationId = association->associationId;
        EXPECT_EQ(association->presentStartTimestamp, 3'000'000 + i * 16'666);
        tracker.Erase(100 + i);
    }
}

TEST(DisplayTimingSubmissionsTest, PruneBeforeRemovesUncompletedFallbackAssociations) {
    DisplaySubmissionTracker tracker;

    EXPECT_TRUE(tracker.Associate(500, 1, 10, 100));
    EXPECT_TRUE(tracker.Associate(500, 1, 20, 200));
    EXPECT_TRUE(tracker.Associate(500, 1, 30, 300));

    tracker.PruneBefore(250);
    EXPECT_EQ(tracker.Find(10), nullptr);
    EXPECT_EQ(tracker.Find(20), nullptr);
    ASSERT_NE(tracker.Find(30), nullptr);
    EXPECT_EQ(tracker.Find(30)->timestamp, 300);
}

TEST(DisplayTimingSubmissionsTest, VulkanFallbackYieldsEstimatedSystemLatencyInOverlay) {
    // End-to-end regression test: verifies that Vulkan present submissions
    // associated via the fallback path publish into SharedDisplayTiming and feed
    // SystemLatencyMetrics to produce Source::Estimated ("Latency est. XX.X ms").
    DisplaySubmissionTracker submissionTracker;
    SharedDisplayTiming sharedTiming;
    sharedTiming.Reset(7232, 0, DisplayTimingStatus::Active);

    ce::system_latency::Tracker latencyTracker;

    constexpr int64_t frameIntervalUs = 16'666;
    constexpr int64_t submitLeadUs = 50;
    constexpr int64_t presentToDisplayUs = 20'000;

    for (int frame = 0; frame < 12; ++frame) {
        const int64_t presentTimeUs = 1'000'000 + frame * frameIntervalUs;
        const int64_t submitTimeUs = presentTimeUs + submitLeadUs;
        const int64_t screenTimeUs = presentTimeUs + presentToDisplayUs;
        const uint32_t submitSeq = 1000 + frame;

        // 1. Vulkan layer records present
        latencyTracker.ObservePresent(presentTimeUs);

        // 2. Kernel queue packet associates without prior runtime present
        bool isFallback = false;
        EXPECT_TRUE(submissionTracker.Associate(7232, 1, submitSeq, submitTimeUs, &isFallback));
        EXPECT_TRUE(isFallback);

        // 3. Flip completes and publishes to shared memory
        const SubmitAssociation* assoc = submissionTracker.Find(submitSeq);
        ASSERT_NE(assoc, nullptr);
        sharedTiming.Publish(screenTimeUs, screenTimeUs + 100, assoc->presentStartTimestamp, true);
        submissionTracker.Erase(submitSeq);

        // 4. Overlay consumes shared display timing
        int64_t readScreenTimeUs = 0;
        int64_t readPresentStartUs = 0;
        bool screenTimeResolved = false;
        ASSERT_TRUE(sharedTiming.Read(frame + 1, readScreenTimeUs, readPresentStartUs, screenTimeResolved));
        latencyTracker.ObserveDisplay(readScreenTimeUs, readPresentStartUs);
    }

    // 5. Query system latency snapshot as overlay does (after last displayed frame)
    const auto snapshot = latencyTracker.GetSnapshot(1'000'000 + 11 * frameIntervalUs + presentToDisplayUs + 1'000);
    EXPECT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, ce::system_latency::Source::Estimated);
    EXPECT_STREQ(ce::system_latency::SourceOverlayLabel(snapshot.source), "Latency est.");
    EXPECT_GT(snapshot.milliseconds, 0.0f);
    EXPECT_GE(snapshot.sampleCount, 6u);
}
