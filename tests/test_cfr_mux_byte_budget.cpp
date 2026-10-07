#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>

#include "common/capture/capture_pipeline_policy.h"
#include "source_fragment_reader.h"

namespace policy = ce::capture_policy;

namespace {

constexpr double kMiB = 1024.0 * 1024.0;
constexpr uint64_t kQueueLimitBytes = 512ull * 1024ull * 1024ull;
constexpr double kFrameIntervalMs = 1000.0 / 120.0;
constexpr uint32_t kSamples = policy::kWgcOverloadRepeatPacerMinSamples;

// Session 20261007_120811 r0005: AV1 4K120 VBR into an SMB share. Under background CPU
// load the share accepted ~9 MB/s while fresh frames produced ~23.5 MB/s; cached
// repeats and two AAC tracks are small.
struct MuxPlant {
    double writerBytesPerSec = 9.0 * kMiB;
    double freshFrameBytes = 23.5 * kMiB / 120.0;
    double repeatFrameBytes = 2.0 * 1024.0;
    double audioBytesPerSec = 48.0 * 1024.0;
    double queuedBytes = 0.0;
    policy::CfrMuxFlowSample counters{0, 0, 0, 0, 0, kQueueLimitBytes, 0, 0};
    bool blockedEncoder = false;

    // Advances one budget window in which `freshShare` of the 120 fps slots were fresh.
    // A full queue blocks the encoder (WriteFrame backpressure): no slot is emitted.
    void Advance(double freshShare, uint64_t windowUs) {
        const double seconds = static_cast<double>(windowUs) / 1e6;
        const double slots = 120.0 * seconds;
        const double produced =
            slots * (freshShare * freshFrameBytes + (1.0 - freshShare) * repeatFrameBytes) +
            audioBytesPerSec * seconds;
        const double drained = std::min(queuedBytes + produced, writerBytesPerSec * seconds);
        const double room = static_cast<double>(kQueueLimitBytes) - queuedBytes + drained;
        blockedEncoder = produced > room;
        const double enqueued = blockedEncoder ? room : produced;
        const double emittedSlots = blockedEncoder ? slots * room / produced : slots;
        queuedBytes = queuedBytes + enqueued - drained;
        counters.nowUs += windowUs;
        counters.enqueuedBytes += static_cast<uint64_t>(enqueued);
        counters.writtenBytes += static_cast<uint64_t>(drained);
        counters.writerBusyUs += drained > 0.0 ? windowUs : 0;
        counters.queuedBytes = static_cast<uint64_t>(queuedBytes);
        counters.queueLimitBytes = kQueueLimitBytes;
        counters.freshOutputs += static_cast<uint64_t>(std::llround(emittedSlots * freshShare));
        counters.totalOutputs += static_cast<uint64_t>(std::llround(emittedSlots));
    }
};

}  // namespace

TEST(CfrMuxByteBudgetTest, ShortOutputStallIsAbsorbedByTheQueueWithoutPacing) {
    MuxPlant plant;
    policy::CfrMuxByteBudgetState state;
    policy::UpdateCfrMuxByteBudget(state, plant.counters, true);
    // 7.5 s of deficit fill under a quarter of the queue: no reaction.
    for (int window = 0; window < 30; ++window) {
        plant.Advance(1.0, policy::kCfrMuxByteBudgetWindowUs);
        const auto decision = policy::UpdateCfrMuxByteBudget(state, plant.counters, true);
        ASSERT_TRUE(decision.sampled);
        EXPECT_FALSE(decision.active);
        EXPECT_DOUBLE_EQ(decision.freshFractionCap, 1.0);
    }
    EXPECT_LT(state.last.fillPermille, policy::kCfrMuxByteBudgetEngageFillPermille);
}

TEST(CfrMuxByteBudgetTest, IncidentWithoutBudgetBlocksTheEncoderAndWithBudgetNeverDoes) {
    // Without the budget every slot stays fresh and the queue reaches its limit
    // within a minute: from then on WriteFrame blocks the encoder thread and the
    // slots it misses become CFR timeline debt (the 150 s debt of r0005).
    MuxPlant unpaced;
    bool unpacedBlocked = false;
    for (int window = 0; window < 4 * 120; ++window) {
        unpaced.Advance(1.0, policy::kCfrMuxByteBudgetWindowUs);
        unpacedBlocked = unpacedBlocked || unpaced.blockedEncoder;
    }
    EXPECT_TRUE(unpacedBlocked);

    MuxPlant plant;
    policy::CfrMuxByteBudgetState state;
    policy::UpdateCfrMuxByteBudget(state, plant.counters, true);
    double freshShare = 1.0;
    double peakFill = 0.0;
    bool entered = false;
    for (int window = 0; window < 4 * 600; ++window) {
        plant.Advance(freshShare, policy::kCfrMuxByteBudgetWindowUs);
        ASSERT_FALSE(plant.blockedEncoder) << "window " << window;
        const auto decision = policy::UpdateCfrMuxByteBudget(state, plant.counters, true);
        entered = entered || decision.entered;
        freshShare = decision.freshFractionCap;
        peakFill = std::max(peakFill, plant.queuedBytes / static_cast<double>(kQueueLimitBytes));
    }
    EXPECT_TRUE(entered);
    EXPECT_TRUE(state.active);
    EXPECT_EQ(state.episodes, 1u);
    EXPECT_LT(peakFill, 0.30);
    // The sustainable mix: (writer - audio - repeats) / (fresh - repeat) bytes per slot.
    const double sustainable =
        (plant.writerBytesPerSec - plant.audioBytesPerSec - 120.0 * plant.repeatFrameBytes) /
        (120.0 * (plant.freshFrameBytes - plant.repeatFrameBytes));
    EXPECT_NEAR(freshShare, sustainable, 0.05);
    EXPECT_GT(freshShare, 0.30);  // ~45 unique fps of 120, evenly spread, not a frozen video
}

TEST(CfrMuxByteBudgetTest, EngagementCapMatchesTheWriterShareOfTheEnqueueRate) {
    policy::CfrMuxByteBudgetState state;
    policy::CfrMuxFlowSample sample{};
    sample.queueLimitBytes = kQueueLimitBytes;
    policy::UpdateCfrMuxByteBudget(state, sample, true);
    sample.nowUs += 1'000'000;
    sample.enqueuedBytes += static_cast<uint64_t>(23.5 * kMiB);
    sample.writtenBytes += static_cast<uint64_t>(9.0 * kMiB);
    sample.writerBusyUs += 1'000'000;
    sample.queuedBytes = kQueueLimitBytes / 4;
    sample.freshOutputs += 120;
    sample.totalOutputs += 120;
    const auto decision = policy::UpdateCfrMuxByteBudget(state, sample, true);
    ASSERT_TRUE(decision.entered);
    // fill 25% -> excess (250-125)/875 -> target 9 MB/s * (1 - 0.5 * 0.1429).
    EXPECT_NEAR(decision.freshFractionCap, 9.0 * (1.0 - 0.5 * 125.0 / 875.0) / 23.5, 1e-3);
    EXPECT_STREQ(decision.reason, "pacing");
}

TEST(CfrMuxByteBudgetTest, RecoveredWriterRampsBackAndExitsOnlyOnceTheQueueDrained) {
    MuxPlant plant;
    policy::CfrMuxByteBudgetState state;
    policy::UpdateCfrMuxByteBudget(state, plant.counters, true);
    double freshShare = 1.0;
    for (int window = 0; window < 4 * 120; ++window) {
        plant.Advance(freshShare, policy::kCfrMuxByteBudgetWindowUs);
        freshShare = policy::UpdateCfrMuxByteBudget(state, plant.counters, true).freshFractionCap;
    }
    ASSERT_TRUE(state.active);

    plant.writerBytesPerSec = 60.0 * kMiB;  // the load ended; the share is fast again
    bool exited = false;
    int exitWindow = -1;
    for (int window = 0; window < 4 * 60 && !exited; ++window) {
        const double before = freshShare;
        plant.Advance(freshShare, policy::kCfrMuxByteBudgetWindowUs);
        const auto decision = policy::UpdateCfrMuxByteBudget(state, plant.counters, true);
        freshShare = decision.freshFractionCap;
        if (!decision.exited) {
            EXPECT_LE(freshShare, std::min(1.0, before * policy::kCfrMuxByteBudgetMaxGrowthPerWindow) + 1e-9);
        }
        exited = decision.exited;
        exitWindow = window;
    }
    EXPECT_TRUE(exited);
    EXPECT_STREQ(policy::UpdateCfrMuxByteBudget(state, plant.counters, true).reason, "collecting");
    EXPECT_FALSE(state.active);
    EXPECT_DOUBLE_EQ(freshShare, 1.0);
    EXPECT_LE(state.last.fillPermille, policy::kCfrMuxByteBudgetExitFillPermille);
    EXPECT_LT(exitWindow, 4 * 30);
}

TEST(CfrMuxByteBudgetTest, StalledWriteWaitsForACompletionThenCutsFreshToTheLivenessFloor) {
    policy::CfrMuxByteBudgetState state;
    policy::CfrMuxFlowSample sample{};
    sample.queueLimitBytes = kQueueLimitBytes;
    sample.queuedBytes = kQueueLimitBytes / 2;
    policy::UpdateCfrMuxByteBudget(state, sample, true);
    state.active = true;  // already pacing when the share stops completing writes
    state.freshFractionCap = 0.4;

    sample.nowUs += policy::kCfrMuxByteBudgetWindowUs;
    sample.enqueuedBytes += 2 * 1024 * 1024;
    sample.freshOutputs += 12;
    sample.totalOutputs += 30;
    auto decision = policy::UpdateCfrMuxByteBudget(state, sample, true);
    EXPECT_FALSE(decision.sampled);
    EXPECT_STREQ(decision.reason, "collecting");
    EXPECT_DOUBLE_EQ(decision.freshFractionCap, 0.4);

    sample.nowUs += policy::kCfrMuxByteBudgetMaxWindowUs;
    decision = policy::UpdateCfrMuxByteBudget(state, sample, true);
    ASSERT_TRUE(decision.sampled);
    EXPECT_DOUBLE_EQ(state.last.capacityBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(decision.freshFractionCap, policy::kWgcOverloadRepeatPacerDegradedFreshFraction);
}

TEST(CfrMuxByteBudgetTest, WindowWithoutFreshFramesKeepsTheCapAndNewRecordingRebases) {
    EXPECT_DOUBLE_EQ(policy::ComputeCfrMuxFreshFractionCap(0.37, 0.0, 9.0 * kMiB, 8.0 * kMiB), 0.37);
    EXPECT_DOUBLE_EQ(policy::ComputeCfrMuxFreshFractionCap(0.37, 0.4, 0.0, 8.0 * kMiB), 0.37);
    EXPECT_DOUBLE_EQ(policy::ComputeCfrMuxFreshFractionCap(0.37, 0.4, 1.0, 8.0 * kMiB),
                     0.37 * policy::kCfrMuxByteBudgetMaxGrowthPerWindow);

    policy::CfrMuxByteBudgetState state;
    policy::CfrMuxFlowSample sample{};
    sample.nowUs = 10'000'000;
    sample.queueLimitBytes = kQueueLimitBytes;
    sample.enqueuedBytes = sample.writtenBytes = 900 * 1024 * 1024;
    policy::UpdateCfrMuxByteBudget(state, sample, true);
    state.active = true;
    state.freshFractionCap = 0.3;
    sample.nowUs += policy::kCfrMuxByteBudgetWindowUs;
    sample.enqueuedBytes = sample.writtenBytes = 0;  // the next recording reset the counters
    const auto decision = policy::UpdateCfrMuxByteBudget(state, sample, true);
    EXPECT_TRUE(decision.exited);
    EXPECT_FALSE(decision.active);
    EXPECT_STREQ(decision.reason, "baseline");
    EXPECT_DOUBLE_EQ(decision.freshFractionCap, 1.0);
    EXPECT_FALSE(policy::UpdateCfrMuxByteBudget(state, sample, false).active);
}

TEST(CfrMuxByteBudgetTest, PacerSpendsTheByteCapEvenWhereServiceTimeCannotSeeTheLimit) {
    // Under mux backpressure a repeat waits for the previous fresh frame's bytes, so both
    // measure far over the interval and the time-based pacer starves the video to its 5%
    // liveness floor (r0005: fresh=23.07ms repeat=22.33ms), although the writer could
    // take ~38% fresh slots; once service times recover it returns to 100% and blocks again.
    policy::WgcOverloadRepeatPacerState timeOnly;
    const auto blind = policy::UpdateWgcOverloadRepeatPacer(
        timeOnly, true, true, true, true, true, 23.07, 22.33, kFrameIntervalMs, kSamples, kSamples);
    EXPECT_STREQ(blind.reason, "degraded_repeat_over_interval");
    EXPECT_DOUBLE_EQ(blind.freshFraction, policy::kWgcOverloadRepeatPacerDegradedFreshFraction);

    // The byte cap paces regardless of unhealthy source or unmeasured service time.
    policy::WgcOverloadRepeatPacerState state;
    uint32_t fresh = 0;
    for (int tick = 0; tick < 1200; ++tick) {
        const auto decision = policy::UpdateWgcOverloadRepeatPacer(
            state, true, false, false, true, true, 0.0, 0.0, kFrameIntervalMs, 0, 0, 0.375);
        ASSERT_TRUE(decision.active);
        EXPECT_STREQ(decision.reason, "mux_byte_budget");
        fresh += decision.repeat ? 0u : 1u;
    }
    EXPECT_EQ(state.episodes, 1u);
    EXPECT_NEAR(static_cast<double>(fresh), 0.375 * 1200.0, 1.0);
    EXPECT_LE(state.maxConsecutiveProactiveRepeats, 2u);  // evenly spread, no frozen runs

    // A tighter measured service limit still wins.
    policy::WgcOverloadRepeatPacerState both;
    const auto tighter = policy::UpdateWgcOverloadRepeatPacer(
        both, true, true, true, true, true, 30.0, 2.0, kFrameIntervalMs, kSamples, kSamples, 0.9);
    EXPECT_STREQ(tighter.reason, "mux_byte_budget_and_service");
    EXPECT_LT(tighter.freshFraction, 0.9);

    // Without a repeat cache nothing can replace a fresh slot.
    policy::WgcOverloadRepeatPacerState noRepeat;
    EXPECT_FALSE(policy::UpdateWgcOverloadRepeatPacer(noRepeat, true, true, true, true, false, 0.0, 0.0,
                                                      kFrameIntervalMs, 0, 0, 0.3)
                     .active);
}

TEST(CfrMuxByteBudgetTest, PacerHandsBackToServiceTimePolicyWhenTheByteCapLifts) {
    policy::WgcOverloadRepeatPacerState state;
    for (int tick = 0; tick < 16; ++tick) {
        policy::UpdateWgcOverloadRepeatPacer(state, true, true, true, true, true, 4.0, 2.0, kFrameIntervalMs,
                                             kSamples, kSamples, 0.5);
    }
    ASSERT_TRUE(state.active);
    policy::WgcOverloadRepeatPacerDecision decision{};
    for (uint32_t tick = 0; tick < policy::kWgcOverloadRepeatPacerRecoveryConfirmTicks; ++tick) {
        decision = policy::UpdateWgcOverloadRepeatPacer(state, true, true, true, true, true, 4.0, 2.0,
                                                        kFrameIntervalMs, kSamples, kSamples);
    }
    EXPECT_TRUE(decision.exited);
    EXPECT_STREQ(decision.reason, "service_recovered");
    EXPECT_FALSE(state.active);
}

TEST(CfrMuxByteBudgetTest, EncoderLoopFeedsTheByteCapIntoBothBackendPacers) {
    const std::string source = ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "captureengine" /
                                                                  "media" / "media_main.cpp");
    ASSERT_FALSE(source.empty());
    // Sampled once per loop before selection, from the DLL's lock-free mux counters and the
    // submission funnel's accepted-output counts.
    EXPECT_NE(source.find("dropWgcVisualTimelineDebtToLiveWindow(media_main_g_Recording.load(std::memory_order_acquire) "
                          "? \"live\" : \"drain\");\n        }\n        updateCfrMuxByteBudget();"),
              std::string::npos);
    EXPECT_NE(source.find("MediaEngine_GetMuxFlowSnapshotV1(&snapshot)"), std::string::npos);
    EXPECT_NE(source.find("ce::media::submission::GetAcceptedOutputCounts()"), std::string::npos);
    // Both the WGC/DXGI and the inject pacer spend it.
    EXPECT_NE(source.find("wgcFreshServiceSamples, wgcRepeatServiceSamples,\n        cfrMuxFreshFractionCap());"),
              std::string::npos);
    EXPECT_NE(source.find("runtime.freshServiceSamples, runtime.repeatServiceSamples, cfrMuxFreshFractionCap());"),
              std::string::npos);
    // An active budget is mux pressure for recording health, and the stop summary reports it.
    EXPECT_NE(source.find("kEncoderOverloadFlagMux) != 0 || cfrMuxByteBudget.active;"), std::string::npos);
    EXPECT_NE(source.find("logCfrMuxByteBudgetSummary();"), std::string::npos);
}
