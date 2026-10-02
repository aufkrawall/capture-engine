#include "captureengine/display_timing/display_timing_submissions.h"
#include "common/ipc/display_timing_shared.h"
#include "hook/metrics/system_latency_metrics.h"

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

TEST(DisplayTimingSubmissionsTest, CompletionPlausibilityRequiresCausalOrderAndTheBound) {
    EXPECT_TRUE(IsPlausibleSubmitCompletion(1'000, 1'000, 500));
    EXPECT_TRUE(IsPlausibleSubmitCompletion(1'000, 1'500, 500));
    EXPECT_FALSE(IsPlausibleSubmitCompletion(1'000, 1'501, 500));
    EXPECT_FALSE(IsPlausibleSubmitCompletion(1'000, 999, 500));
    // Zero keeps only the causal order.
    EXPECT_TRUE(IsPlausibleSubmitCompletion(1'000, 1'000'000'000, 0));
    EXPECT_FALSE(IsPlausibleSubmitCompletion(1'000, 999, 0));
    EXPECT_EQ(kMaxSubmitToCompletionUs, 1'000'000);
}

TEST(DisplayTimingSubmissionsTest, CompletionWithinBoundClaimsTheSubmission) {
    DisplaySubmissionTracker tracker;
    tracker.SetMaxCompletionAge(kMaxSubmitToCompletionUs);

    EXPECT_TRUE(tracker.Associate(54820, 1, 700, 5'000'000));
    const SubmitAssociation* association = tracker.FindForCompletion(700, 5'007'000);
    ASSERT_NE(association, nullptr);
    EXPECT_EQ(association->timestamp, 5'000'000);
    EXPECT_EQ(tracker.matchedCompletions(), 1u);
    EXPECT_EQ(tracker.rejectedStaleCompletions(), 0u);
    tracker.Erase(700);
    EXPECT_EQ(tracker.FindForCompletion(700, 5'007'000), nullptr);
}

// DOOM Eternal session 20260928_042343: dropping from native 4K to a 1440p
// mode moved NVIDIA's Vulkan WSI onto a DXGI swapchain whose frames the DWM
// composes, so the game's present packets stopped completing as flips. With
// ten seconds of unclaimed submissions retained, unrelated VSync completions
// whose submit sequence carried the same number claimed them: every published
// sample had presentToDisplay between 8.8 s and 12 s and the overlay drew ~29 fps
// for a game running at 140. Before the bound, Find() returned the stale entry.
TEST(DisplayTimingSubmissionsTest, StaleUnflippedSubmissionIsNotClaimedByAnUnrelatedCompletion) {
    DisplaySubmissionTracker tracker;
    tracker.SetMaxCompletionAge(kMaxSubmitToCompletionUs);

    constexpr int64_t kSubmitUs = 30'000'000;
    EXPECT_TRUE(tracker.Associate(54820, 1, 4242, kSubmitUs));
    ASSERT_NE(tracker.Find(4242), nullptr);

    EXPECT_EQ(tracker.FindForCompletion(4242, kSubmitUs + 10'000'000), nullptr);
    EXPECT_EQ(tracker.rejectedStaleCompletions(), 1u);
    EXPECT_EQ(tracker.matchedCompletions(), 0u);
    // The stale entry is gone rather than left for the next collision.
    EXPECT_EQ(tracker.Find(4242), nullptr);
}

TEST(DisplayTimingSubmissionsTest, StaleEntryIsSkippedForAFreshSubmissionReusingTheNumber) {
    DisplaySubmissionTracker tracker;
    tracker.SetMaxCompletionAge(kMaxSubmitToCompletionUs);

    EXPECT_TRUE(tracker.Associate(54820, 1, 900, 1'000'000));
    EXPECT_TRUE(tracker.Associate(54820, 1, 900, 20'000'000));
    const SubmitAssociation* association = tracker.FindForCompletion(900, 20'006'000);
    ASSERT_NE(association, nullptr);
    EXPECT_EQ(association->timestamp, 20'000'000);
    EXPECT_EQ(tracker.rejectedStaleCompletions(), 1u);
    EXPECT_EQ(tracker.matchedCompletions(), 1u);
}

TEST(DisplayTimingSubmissionsTest, SubmissionNewerThanTheCompletionWaitsForItsOwn) {
    DisplaySubmissionTracker tracker;
    tracker.SetMaxCompletionAge(kMaxSubmitToCompletionUs);

    EXPECT_TRUE(tracker.Associate(54820, 1, 77, 8'000'000));
    // A completion that precedes the submission cannot be its flip.
    EXPECT_EQ(tracker.FindForCompletion(77, 7'990'000), nullptr);
    EXPECT_EQ(tracker.rejectedStaleCompletions(), 0u);
    ASSERT_NE(tracker.FindForCompletion(77, 8'004'000), nullptr);
}

TEST(DisplayTimingSubmissionsTest, UnboundedTrackerKeepsCausalOrderOnly) {
    DisplaySubmissionTracker tracker;

    EXPECT_TRUE(tracker.Associate(1, 1, 5, 100));
    EXPECT_EQ(tracker.FindForCompletion(5, 99), nullptr);
    EXPECT_NE(tracker.FindForCompletion(5, 100'000'000), nullptr);
}

TEST(DisplayTimingSubmissionsTest, PruneReportsExpiredSubmissionsAndTheirProcess) {
    DisplaySubmissionTracker tracker;

    tracker.ObserveRuntimePresent(300, 1, 50);
    EXPECT_TRUE(tracker.Associate(400, 1, 10, 100));
    EXPECT_TRUE(tracker.Associate(400, 1, 11, 200));
    EXPECT_TRUE(tracker.Associate(400, 1, 12, 300));

    EXPECT_EQ(tracker.PruneBefore(250), 2u);
    EXPECT_EQ(tracker.expiredAssociations(), 2u);
    EXPECT_EQ(tracker.lastExpiredProcessId(), 400u);
    EXPECT_EQ(tracker.PruneBefore(250), 0u);
    EXPECT_EQ(tracker.expiredAssociations(), 2u);
    ASSERT_NE(tracker.Find(12), nullptr);
}

TEST(DisplayTimingSubmissionsTest, ExpiryMonitorReportsOnlyTheNoFlipState) {
    using Transition = DisplaySubmissionExpiryMonitor::Transition;
    DisplaySubmissionExpiryMonitor monitor;
    uint64_t matchedTotal = 0;

    // Flip-model discards: some presents expire, the rest still flip.
    matchedTotal += 20;
    EXPECT_EQ(monitor.Observe(15, matchedTotal, 1'000), Transition::None);
    EXPECT_FALSE(monitor.expiring());
    EXPECT_EQ(monitor.lastCompleted(), 20u);

    // Composed presentation: everything expires, nothing completes.
    EXPECT_EQ(monitor.Observe(35, matchedTotal, 1'250), Transition::Started);
    EXPECT_TRUE(monitor.expiring());
    EXPECT_EQ(monitor.Observe(35, matchedTotal, 1'500), Transition::None);

    // Flips resume, but inside the minimum interval the state holds.
    matchedTotal += 30;
    EXPECT_EQ(monitor.Observe(0, matchedTotal, 5'000), Transition::None);
    EXPECT_TRUE(monitor.expiring());
    matchedTotal += 30;
    EXPECT_EQ(monitor.Observe(0, matchedTotal, 1'250 + DisplaySubmissionExpiryMonitor::kMinTransitionIntervalMs),
              Transition::Stopped);
    EXPECT_FALSE(monitor.expiring());
    EXPECT_EQ(monitor.lastCompleted(), 30u);
}

TEST(DisplayTimingSubmissionsTest, ExpiryMonitorIgnoresAnIdleProcess) {
    DisplaySubmissionExpiryMonitor monitor;
    EXPECT_EQ(monitor.Observe(0, 0, 1'000), DisplaySubmissionExpiryMonitor::Transition::None);
    EXPECT_EQ(monitor.Observe(0, 0, 60'000), DisplaySubmissionExpiryMonitor::Transition::None);
    EXPECT_FALSE(monitor.expiring());
}
