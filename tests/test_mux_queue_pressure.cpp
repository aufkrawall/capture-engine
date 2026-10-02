#include <gtest/gtest.h>

#include "mediaengine/mux/mux_queue_pressure.h"

namespace {

using ce::mux::ComputeMuxQueueFillPermille;
using ce::mux::ComputeMuxWriterWindowRates;
using ce::mux::IsSlowMuxWrite;
using ce::mux::MuxQueuePressureEvent;
using ce::mux::MuxQueuePressureTracker;
using ce::mux::MuxQueuePressureUpdate;
using ce::mux::MuxWriterWindowRates;
using ce::mux::SelectMuxQueuePressureBandPermille;
using ce::mux::ShouldLogSlowMuxWrite;

constexpr size_t kMiB = 1024u * 1024u;
constexpr size_t kLimit = 512u * kMiB;

}  // namespace

TEST(MuxQueuePressureTest, FillPermilleIsClampedAndHandlesAZeroLimit) {
    EXPECT_EQ(ComputeMuxQueueFillPermille(0, kLimit), 0u);
    EXPECT_EQ(ComputeMuxQueueFillPermille(kLimit / 2, kLimit), 500u);
    EXPECT_EQ(ComputeMuxQueueFillPermille(kLimit * 2, kLimit), 1000u);
    EXPECT_EQ(ComputeMuxQueueFillPermille(0, 0), 0u);
    EXPECT_EQ(ComputeMuxQueueFillPermille(1, 0), 1000u);
}

TEST(MuxQueuePressureTest, BandsAreQuarterSteps) {
    EXPECT_EQ(SelectMuxQueuePressureBandPermille(249), 0u);
    EXPECT_EQ(SelectMuxQueuePressureBandPermille(250), 250u);
    EXPECT_EQ(SelectMuxQueuePressureBandPermille(499), 250u);
    EXPECT_EQ(SelectMuxQueuePressureBandPermille(500), 500u);
    EXPECT_EQ(SelectMuxQueuePressureBandPermille(1000), 750u);
}

TEST(MuxQueuePressureTest, SteadyShallowQueueStaysSilent) {
    MuxQueuePressureTracker tracker;
    for (uint64_t ms = 0; ms < 60000; ms += 8) {
        EXPECT_EQ(tracker.Observe(3 * kMiB, kLimit, ms).event, MuxQueuePressureEvent::kNone);
    }
    EXPECT_FALSE(tracker.EpisodeActive());
}

// Replays the growth seen when a network-share output target stalled: the
// queue climbed ~7 MB/s to 421 MB of 512 MB over ~45 s, then drained in ~5 s.
// Only the periodic INFO byte count recorded it; the tracker must raise each
// band once and report the recovery once.
TEST(MuxQueuePressureTest, StalledOutputRaisesEachBandOnceAndRecoversOnce) {
    MuxQueuePressureTracker tracker;
    int raised = 0;
    int recovered = 0;
    uint32_t lastRaisedBand = 0;
    uint64_t nowMs = 0;
    size_t queued = 0;
    for (; queued <= 421 * kMiB; queued += kMiB / 8, nowMs += 17) {
        const MuxQueuePressureUpdate update = tracker.Observe(queued, kLimit, nowMs);
        if (update.event == MuxQueuePressureEvent::kRaised) {
            ++raised;
            EXPECT_GT(update.bandPermille, lastRaisedBand);
            lastRaisedBand = update.bandPermille;
        }
        EXPECT_NE(update.event, MuxQueuePressureEvent::kRecovered);
    }
    EXPECT_EQ(raised, 3);
    EXPECT_EQ(lastRaisedBand, 750u);

    MuxQueuePressureUpdate recovery;
    for (; queued > kMiB; queued -= kMiB, nowMs += 12) {
        const MuxQueuePressureUpdate update = tracker.Observe(queued, kLimit, nowMs);
        EXPECT_NE(update.event, MuxQueuePressureEvent::kRaised);
        if (update.event == MuxQueuePressureEvent::kRecovered) {
            ++recovered;
            recovery = update;
        }
    }
    EXPECT_EQ(recovered, 1);
    EXPECT_EQ(recovery.bandPermille, 750u);
    EXPECT_GE(recovery.episodePeakBytes, 421 * kMiB);
    EXPECT_GT(recovery.episodeDurationMs, 0u);
    EXPECT_LT(recovery.fillPermille, ce::mux::kMuxQueuePressureRecoverPermille);
    EXPECT_FALSE(tracker.EpisodeActive());
}

TEST(MuxQueuePressureTest, HoveringAroundTheFirstBandDoesNotFlap) {
    MuxQueuePressureTracker tracker;
    int events = 0;
    for (int i = 0; i < 1000; ++i) {
        const size_t queued = (i % 2 == 0) ? kLimit / 4 : kLimit / 5;  // 25% / 20%
        if (tracker.Observe(queued, kLimit, static_cast<uint64_t>(i)).event != MuxQueuePressureEvent::kNone) {
            ++events;
        }
    }
    EXPECT_EQ(events, 1);
    EXPECT_TRUE(tracker.EpisodeActive());
}

TEST(MuxQueuePressureTest, NewEpisodeAfterRecoveryStartsFromTheFirstBand) {
    MuxQueuePressureTracker tracker;
    EXPECT_EQ(tracker.Observe(kLimit * 6 / 10, kLimit, 100).bandPermille, 500u);
    EXPECT_EQ(tracker.Observe(kLimit / 10, kLimit, 200).event, MuxQueuePressureEvent::kRecovered);
    const MuxQueuePressureUpdate again = tracker.Observe(kLimit * 3 / 10, kLimit, 300);
    EXPECT_EQ(again.event, MuxQueuePressureEvent::kRaised);
    EXPECT_EQ(again.bandPermille, 250u);
    EXPECT_EQ(again.episodeDurationMs, 0u);
}

TEST(MuxQueuePressureTest, ResetEndsAnActiveEpisodeSilently) {
    MuxQueuePressureTracker tracker;
    tracker.Observe(kLimit, kLimit, 10);
    tracker.Reset();
    EXPECT_FALSE(tracker.EpisodeActive());
    EXPECT_EQ(tracker.Observe(kMiB, kLimit, 20).event, MuxQueuePressureEvent::kNone);
}

TEST(MuxQueuePressureTest, WindowRatesSeparateAStalledWriterFromAnIdleOne) {
    // 5 s window: encoder produced 8.4 MB/s, writer managed 1.4 MB/s while
    // blocked in its write calls for 4.9 s of it.
    const MuxWriterWindowRates stalled = ComputeMuxWriterWindowRates(7 * kMiB, 42 * kMiB, 4900000, 5000000);
    EXPECT_EQ(stalled.writerBytesPerSecond, 7 * kMiB / 5);
    EXPECT_EQ(stalled.encoderBytesPerSecond, 42 * kMiB / 5);
    EXPECT_EQ(stalled.busyPermille, 980u);

    const MuxWriterWindowRates idle = ComputeMuxWriterWindowRates(kMiB, 42 * kMiB, 50000, 5000000);
    EXPECT_EQ(idle.busyPermille, 10u);

    const MuxWriterWindowRates empty = ComputeMuxWriterWindowRates(kMiB, kMiB, 10, 0);
    EXPECT_EQ(empty.writerBytesPerSecond, 0u);
    EXPECT_EQ(empty.busyPermille, 0u);

    EXPECT_EQ(ComputeMuxWriterWindowRates(0, 0, 9000000, 5000000).busyPermille, 1000u);
}

TEST(MuxQueuePressureTest, SlowWriteThresholdAndRateLimit) {
    EXPECT_FALSE(IsSlowMuxWrite(249999));
    EXPECT_TRUE(IsSlowMuxWrite(250000));

    EXPECT_TRUE(ShouldLogSlowMuxWrite(1000, 0));
    EXPECT_FALSE(ShouldLogSlowMuxWrite(5999, 1000));
    EXPECT_TRUE(ShouldLogSlowMuxWrite(6000, 1000));
    EXPECT_TRUE(ShouldLogSlowMuxWrite(500, 1000));  // clock went backwards
}
