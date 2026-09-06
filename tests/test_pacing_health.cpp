#include <gtest/gtest.h>

#include <vector>

#include "../hook/common/pacing_health_telemetry.h"

// Regression coverage for the start-to-start FSR FG frame pacing investigation
// (logs/talosfullfsrfgbaddlssfggoodfsrfgbadrestartfsrfggood, build 0.1.6491).
//
// The bad-start signature is a share of output intervals landing past the
// median by 1.2-2 ms (measured: 11.8% of flips in a bad window against 1.0% in
// a good one) while the medians differ by under 5%. A rolling stddev alone
// could not classify that, which is why the classification had to be
// reconstructed offline from perf CSVs. The pacing-health ring must reproduce
// the measured statistics so future sessions can be classified from one log
// line.

namespace {

using ce::pacing_health::Channel;
using ce::pacing_health::ComputeChannelStats;
using ce::pacing_health::kLateFlipThresholdUs;
using ce::pacing_health::Observe;
using ce::pacing_health::RecentCadence;
using ce::pacing_health::Signature;
using ce::pacing_health::Snapshot;
using ce::pacing_health::SnapshotTaggedWindow;

std::vector<uint32_t> GoodWindowSamples() {
    // ~90 Hz output cadence with the good session's tail: 1% of flips ~1.4 ms late.
    std::vector<uint32_t> samples;
    samples.reserve(1000);
    for (int i = 0; i < 990; i++) {
        samples.push_back(10778 + (i % 7) * 30 - 90);
    }
    for (int i = 0; i < 10; i++) {
        samples.push_back(12300);
    }
    return samples;
}

std::vector<uint32_t> BadWindowSamples() {
    // The reproduced 20260906_160321 bad window: median ~11 ms with both early
    // and late physical completions (p1 ~7.1 ms, p99 ~17 ms, stddev ~2.5 ms).
    std::vector<uint32_t> samples;
    samples.reserve(1000);
    for (int i = 0; i < 700; i++) {
        samples.push_back(11228 + (i % 5) * 40 - 80);
    }
    for (int i = 0; i < 150; i++) {
        samples.push_back(7100);
    }
    for (int i = 0; i < 150; i++) {
        samples.push_back(17000);
    }
    return samples;
}

// The rings are process-global and other fixtures feed them through
// PerformanceMetrics; flushing one full capacity makes every later Snapshot
// assertion order-independent.
void FlushRing(Channel channel, uint32_t intervalUs) {
    for (uint32_t i = 0; i < 4096; i++) {
        Observe(channel, intervalUs);
    }
}

}  // namespace

TEST(PacingHealthStats, UniformCadenceHasZeroLateShare) {
    std::vector<uint32_t> samples(120, 11122);
    const auto stats = ComputeChannelStats(samples.data(), static_cast<uint32_t>(samples.size()));
    EXPECT_EQ(stats.samples, 120u);
    EXPECT_EQ(stats.medianUs, 11122u);
    EXPECT_EQ(stats.latePermille, 0u);
    EXPECT_EQ(stats.stddevUs, 0u);
}

TEST(PacingHealthStats, ReproducedBadWindowHasWidePhysicalCompletionDistribution) {
    const auto samples = BadWindowSamples();
    const auto stats = ComputeChannelStats(samples.data(), static_cast<uint32_t>(samples.size()));
    EXPECT_EQ(stats.samples, 1000u);
    EXPECT_NEAR(stats.medianUs, 11228u, 80u);
    EXPECT_NEAR(stats.latePermille, 150u, 6u);
    EXPECT_GT(stats.stddevUs, 2500u);
    EXPECT_EQ(stats.p95Us, 17000u);
    EXPECT_EQ(stats.maxUs, 17000u);
}

TEST(PacingHealthStats, GoodWindowClassifiesOnePercentLate) {
    const auto samples = GoodWindowSamples();
    const auto stats = ComputeChannelStats(samples.data(), static_cast<uint32_t>(samples.size()));
    EXPECT_EQ(stats.samples, 1000u);
    EXPECT_LE(stats.latePermille, 15u);
    EXPECT_LT(stats.stddevUs, 300u);
}

TEST(PacingHealthStats, NearThresholdIntervalsDoNotCountLate) {
    // Exactly median + threshold is not late; the measured bad-start excess was
    // 1.2-2 ms, the good-session noise stayed below.
    std::vector<uint32_t> atThreshold{10000, 10000, 10000, 10000 + kLateFlipThresholdUs};
    const auto stats = ComputeChannelStats(atThreshold.data(), static_cast<uint32_t>(atThreshold.size()));
    EXPECT_EQ(stats.medianUs, 10000u);
    EXPECT_EQ(stats.latePermille, 0u);

    std::vector<uint32_t> justAbove{10000, 10000, 10000, 10000 + kLateFlipThresholdUs + 1};
    const auto above = ComputeChannelStats(justAbove.data(), static_cast<uint32_t>(justAbove.size()));
    EXPECT_EQ(above.latePermille, 250u);
}

TEST(PacingHealthStats, EmptyAndSingleSampleSpans) {
    const auto empty = ComputeChannelStats(nullptr, 0);
    EXPECT_EQ(empty.samples, 0u);
    EXPECT_EQ(empty.medianUs, 0u);
    const uint32_t single = 7000;
    const auto one = ComputeChannelStats(&single, 1);
    EXPECT_EQ(one.samples, 1u);
    EXPECT_EQ(one.medianUs, 7000u);
    EXPECT_EQ(one.latePermille, 0u);
}

TEST(PacingHealthRing, ObserveAndSnapshotRoundTrip) {
    FlushRing(Channel::kDisplay, 11000);
    for (int i = 0; i < 300; i++) {
        Observe(Channel::kDisplay, 11000 + (i % 3) * 100);
    }
    const auto stats = Snapshot(Channel::kDisplay);
    EXPECT_EQ(stats.samples, 4096u);
    EXPECT_EQ(stats.medianUs, 11000u);
    EXPECT_EQ(stats.latePermille, 0u);
}

TEST(PacingHealthRing, RecentCadenceLimitsToRequestedWindow) {
    FlushRing(Channel::kPresentation, 9000);
    for (int i = 0; i < 200; i++) {
        Observe(Channel::kPresentation, 10780);
    }
    const auto recent = RecentCadence(Channel::kPresentation, 120);
    EXPECT_EQ(recent.samples, 120u);
    EXPECT_EQ(recent.medianUs, 10780u);
    const auto none = RecentCadence(Channel::kPresentation, 0);
    EXPECT_EQ(none.samples, 0u);
}

TEST(PacingHealthRing, OutlierIntervalsAreDropped) {
    // A retained sub-second gap must not drag the median up even though it
    // shows in max; multi-second stalls and invalid samples are dropped.
    FlushRing(Channel::kDisplay, 6900);
    Observe(Channel::kDisplay, 130'000);
    Observe(Channel::kDisplay, 1'300'000);
    Observe(Channel::kDisplay, 6'000'000);
    Observe(Channel::kDisplay, -5);
    Observe(Channel::kDisplay, 0);
    const auto stats = Snapshot(Channel::kDisplay);
    EXPECT_EQ(stats.samples, 4096u);
    EXPECT_EQ(stats.medianUs, 6900u);
    EXPECT_EQ(stats.maxUs, 1'300'000u);
    // The two retained gap samples count late; ~1 per mille of the window.
    EXPECT_LE(stats.latePermille, 2u);
}

TEST(PacingHealthRing, TaggedWindowExcludesStartupOtherModesAndBoundaries) {
    FlushRing(Channel::kDisplay, 50000);
    Observe(Channel::kDisplay, 30000, 9'999'999, 7);  // before window
    Observe(Channel::kDisplay, 40000, 10'000'000, 8);  // disjoint-window boundary
    Observe(Channel::kDisplay, 11000, 10'100'000, 8);
    Observe(Channel::kDisplay, 13000, 11'000'000, 8);
    Observe(Channel::kDisplay, 12000, 12'000'000, 9);
    Observe(Channel::kDisplay, 45000, 12'000'001, 9);  // after window

    const auto stats = SnapshotTaggedWindow(Channel::kDisplay, 10'000'000, 12'000'000);
    EXPECT_EQ(stats.samples, 3u);
    EXPECT_EQ(stats.minUs, 11000u);
    EXPECT_EQ(stats.maxUs, 13000u);
    EXPECT_EQ(stats.segments, 2u);
}

TEST(PacingHealthClassification, IdentifiesMeasuredDownstreamFailureShape) {
    auto display = ComputeChannelStats(BadWindowSamples().data(), 1000);
    auto presentation = ComputeChannelStats(GoodWindowSamples().data(), 1000);
    EXPECT_EQ(ce::pacing_health::Classify(display, presentation), Signature::kDownstreamJitter);
    EXPECT_STREQ(ce::pacing_health::SignatureName(Signature::kDownstreamJitter), "downstream-jitter");

    EXPECT_EQ(ce::pacing_health::Classify(presentation, presentation), Signature::kHealthy);
    display.samples = 119;
    EXPECT_EQ(ce::pacing_health::Classify(display, presentation), Signature::kInsufficient);
}
