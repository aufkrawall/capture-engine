#include <gtest/gtest.h>

#include "../hook/common/performance_metrics.h"
#include "../hook/common/system_latency_metrics.h"
#include "../hook/common/system_latency_windows.h"

namespace {

using ce::system_latency::FrameBeginKind;
using ce::system_latency::Source;
using ce::system_latency::Tracker;

void FeedPresentationAndDisplay(Tracker& tracker, int64_t firstPresentUs, int64_t intervalUs,
                                int64_t presentToDisplayUs, int count) {
    for (int i = 0; i < count; ++i) {
        const int64_t presentUs = firstPresentUs + intervalUs * i;
        tracker.ObservePresent(presentUs);
        tracker.ObserveDisplay(presentUs + presentToDisplayUs, presentUs + 50);
    }
}

}  // namespace

TEST(SystemLatencyFallbackRecoveryTest, FallbackRecoversAfterPauseOrHitchExceedingMaximumInterval) {
    Tracker tracker;

    // 1. Initial 10 frames at 100 FPS (10ms interval) with 3ms present-to-display.
    FeedPresentationAndDisplay(tracker, 1'000'000, 10'000, 3'000, 10);
    auto snapshot = tracker.GetSnapshot(1'095'000);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::Estimated);
    EXPECT_NEAR(snapshot.milliseconds, 18.0f, 0.5f);

    auto diagnostics = tracker.GetDiagnostics(1'095'000);
    EXPECT_EQ(diagnostics.displaysObserved, 10u);
    // Frame 0 has no cadence yet so baseIntervalUs=0; subsequent frames are accepted.
    EXPECT_LE(diagnostics.samplesRejectedOutOfRange, 1u);
    EXPECT_GE(snapshot.sampleCount, 8u);

    // 2. A 600 ms gap (hitch, loading screen, or menu transition).
    // In the unpatched code, RecordApplicationPresentLocked rejected any interval > 250ms
    // with an early return, freezing applicationPresents_.Back() at the pre-gap frame.
    // Every subsequent present was then compared against the pre-gap frame and also discarded,
    // causing all future displays to match the stale pre-gap frame and fail total latency checks.
    constexpr int64_t kPostGapFirstPresentUs = 1'090'000 + 600'000;
    FeedPresentationAndDisplay(tracker, kPostGapFirstPresentUs, 10'000, 3'000, 20);

    // 3. Fallback estimator must successfully resume and produce valid snapshots post-gap.
    const int64_t postGapEvalUs = kPostGapFirstPresentUs + 20 * 10'000 + 5'000;
    snapshot = tracker.GetSnapshot(postGapEvalUs);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::Estimated);
    EXPECT_NEAR(snapshot.milliseconds, 18.0f, 0.5f);

    diagnostics = tracker.GetDiagnostics(postGapEvalUs);
    EXPECT_EQ(diagnostics.displaysObserved, 30u);
    // Across the gap, steady-state post-gap frames must pass cleanly.
    EXPECT_LE(diagnostics.samplesRejectedOutOfRange, 2u);
    EXPECT_GE(snapshot.sampleCount, 18u);
}

TEST(SystemLatencyFallbackRecoveryTest, HighFramerateUnconstrainedPresentRateWithDisplays) {
    Tracker tracker;

    // Simulate game at 500 FPS (2,000us delta) and display at 144 Hz (~6,944us refresh).
    // Multiple presents occur per display refresh.
    constexpr int64_t kPresentIntervalUs = 2'000;
    constexpr int64_t kDisplayIntervalUs = 6'944;
    constexpr int64_t kPresentToDisplayUs = 13'500;

    int64_t currentPresentUs = 10'000'000;
    int64_t nextDisplayUs = currentPresentUs + kDisplayIntervalUs;

    for (int frame = 0; frame < 150; ++frame) {
        currentPresentUs += kPresentIntervalUs;
        tracker.ObservePresent(currentPresentUs);

        while (nextDisplayUs <= currentPresentUs) {
            const int64_t associatedPresentUs = currentPresentUs - kPresentToDisplayUs;
            if (associatedPresentUs > 10'000'000) {
                tracker.ObserveDisplay(nextDisplayUs, associatedPresentUs);
            }
            nextDisplayUs += kDisplayIntervalUs;
        }
    }

    auto snapshot = tracker.GetSnapshot(currentPresentUs);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::Estimated);
    EXPECT_GT(snapshot.milliseconds, 0.0f);
    EXPECT_LT(snapshot.milliseconds, 50.0f);

    // Now introduce a 500 ms pause/hitch
    currentPresentUs += 500'000;
    nextDisplayUs = currentPresentUs + kDisplayIntervalUs;

    for (int frame = 0; frame < 150; ++frame) {
        currentPresentUs += kPresentIntervalUs;
        tracker.ObservePresent(currentPresentUs);

        while (nextDisplayUs <= currentPresentUs) {
            const int64_t associatedPresentUs = currentPresentUs - kPresentToDisplayUs;
            if (associatedPresentUs > 10'000'000) {
                tracker.ObserveDisplay(nextDisplayUs, associatedPresentUs);
            }
            nextDisplayUs += kDisplayIntervalUs;
        }
    }

    snapshot = tracker.GetSnapshot(currentPresentUs);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::Estimated);
    EXPECT_GT(snapshot.milliseconds, 0.0f);
    EXPECT_LT(snapshot.milliseconds, 50.0f);
}

TEST(SystemLatencyFallbackRecoveryTest, StaleApplicationPresentMatchesDoNotCorruptTotalLatency) {
    Tracker tracker;

    // Record one application present at time 1,000,000
    tracker.ObservePresent(1'000'000);

    // Observe a display 2 seconds later with associated present at 2,000,000
    // (no recent application present recorded).
    tracker.ObserveDisplay(2'010'000, 2'000'000);

    // Stale match check in MatchApplicationPresentLocked ensures the 1-second-old frame
    // is NOT matched. The fallback calculation safely uses baseIntervalUs instead of
    // blowing up into a multi-second anchor delta.
    const auto diagnostics = tracker.GetDiagnostics(2'010'000);
    EXPECT_EQ(diagnostics.samplesRejectedTotalLatency, 0u);
}

TEST(SystemLatencyFallbackRecoveryTest, RejectionBreakdownDiagnosticsTrackSpecificFailureCauses) {
    Tracker tracker;

    // Establish cadence first
    tracker.ObservePresent(1'000'000);
    tracker.ObservePresent(1'010'000);
    tracker.ObserveDisplay(1'013'000, 1'010'000);

    auto diagnostics = tracker.GetDiagnostics(1'013'000);
    EXPECT_EQ(diagnostics.samplesRejectedPresentToDisplay, 0u);

    // Present-to-display exceeds 250ms (kMaximumPresentToDisplayUs)
    tracker.ObservePresent(1'020'000);
    tracker.ObserveDisplay(1'300'000, 1'020'000);

    diagnostics = tracker.GetDiagnostics(1'300'000);
    EXPECT_EQ(diagnostics.samplesRejectedPresentToDisplay, 1u);
    EXPECT_EQ(diagnostics.samplesRejectedBaseInterval, 0u);
    EXPECT_EQ(diagnostics.samplesRejectedTotalLatency, 0u);
    EXPECT_EQ(diagnostics.samplesRejectedOutOfRange, 1u);
}