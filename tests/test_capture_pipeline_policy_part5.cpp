#include "test_capture_pipeline_policy_shared.h"

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

namespace {
using Completion = policy::InjectFrameCopyCompletion;

template <size_t N>
size_t ExtendTail(const Completion (&states)[N], size_t reserve) {
    return policy::ExtendInjectTailOverPendingCopies(N, reserve, [&](size_t index) { return states[index]; });
}
}  // namespace

// Talos DLSS 3x/4x MFG (logs/20261018_222704): the reserve stayed one frame while the
// newest three copies were in flight (`fence not ready (value=N completed=N-2)`), so
// the selector picked a pending frame, deferred it into a repeat and superseded it on
// the next tick. Oldest frame first, as buffered.
TEST(CapturePipelinePolicyTest, FenceTailCoversEveryNewestInFlightCopy) {
    const Completion states[] = {Completion::kComplete, Completion::kComplete, Completion::kPending,
                                 Completion::kPending, Completion::kPending};
    EXPECT_EQ(ExtendTail(states, 1), 3u);
    EXPECT_EQ(ExtendTail(states, 4), 4u) << "a larger measured reserve is never shrunk";
}

TEST(CapturePipelinePolicyTest, FenceTailStaysAtTheReserveWhenTheNewestCopyFinished) {
    const Completion settled[] = {Completion::kComplete, Completion::kComplete, Completion::kComplete};
    EXPECT_EQ(ExtendTail(settled, 1), 1u);
    EXPECT_EQ(ExtendTail(settled, 0), 0u);
    const Completion newestPending[] = {Completion::kComplete, Completion::kComplete, Completion::kPending};
    EXPECT_EQ(ExtendTail(newestPending, 0), 1u);
    EXPECT_EQ(policy::ExtendInjectTailOverPendingCopies(0, 1, [](size_t) { return Completion::kPending; }), 1u);
}

TEST(CapturePipelinePolicyTest, FenceTailHandsUnknownCompletionToTheEncoderFenceCheck) {
    // Unknown means the encoder has not opened that fence yet; it resolves it without blocking.
    const Completion states[] = {Completion::kPending, Completion::kUnknown, Completion::kPending};
    EXPECT_EQ(ExtendTail(states, 0), 1u);
    const Completion unknownNewest[] = {Completion::kPending, Completion::kUnknown};
    EXPECT_EQ(ExtendTail(unknownNewest, 1), 1u);
}

TEST(CapturePipelinePolicyTest, AllInFlightCopiesStayWithheldWhileCompletedStallFramesStillRelease) {
    const Completion inFlight[] = {Completion::kPending, Completion::kPending};
    const size_t tail = ExtendTail(inFlight, 1);
    EXPECT_EQ(tail, 2u);
    EXPECT_EQ(policy::ReleaseSettledInjectTailFrames(2, tail, [&](size_t index) { return inFlight[index]; }), 2u)
        << "a provably in-flight copy cannot be encoded";
    // A stalled source's finished frame is still handed over (the 2026-09-26 lease deadlock).
    const Completion stalled[] = {Completion::kComplete};
    EXPECT_EQ(policy::ReleaseSettledInjectTailFrames(1, ExtendTail(stalled, 1), [&](size_t index) {
                  return stalled[index];
              }),
              0u);
}

TEST(CapturePipelinePolicyTest, InjectSelectorExtendsTheFenceTailBeforeReleaseAndSelection) {
    const std::string source = ce::test_source::ReadLogicalSource(std::filesystem::current_path() /
                                                                   "captureengine" / "media" / "media_main.cpp");
    ASSERT_FALSE(source.empty());
    const size_t extend = source.find("ExtendInjectTailOverPendingCopies(");
    const size_t release = source.find("ReleaseSettledInjectTailFrames(");
    const size_t select = source.find("SelectFrameClosestToTimestampIf(bufferedInjectFrames");
    ASSERT_NE(extend, std::string::npos);
    ASSERT_NE(release, std::string::npos);
    ASSERT_NE(select, std::string::npos);
    EXPECT_LT(extend, release) << "settled-frame release must see the in-flight run";
    EXPECT_LT(extend, select);
    EXPECT_NE(source.find("return queryInjectFrameCopyCompletion(bufferedInjectFrames[index]);", extend),
              std::string::npos);
}

TEST(CapturePipelinePolicyTest, CfrOverloadPacerProbesRepeatCostWithoutDiscardingFreshLiveness) {
    constexpr double kFrameIntervalMs = 1000.0 / 120.0;
    constexpr uint32_t kFreshSamples = policy::kWgcOverloadRepeatPacerMinSamples;
    policy::CfrOverloadRepeatPacerState state;
    uint32_t probeDecisions = 0;

    for (uint32_t tick = 1; tick <= policy::kCfrOverloadRepeatProbeIntervalTicks * 2; ++tick) {
        const auto decision = policy::UpdateCfrOverloadRepeatPacer(
            state, true, true, true, true, true, 10.0, 0.0, kFrameIntervalMs, kFreshSamples, 0);
        EXPECT_FALSE(decision.active);
        EXPECT_STREQ(decision.reason,
                     decision.repeat ? "measuring_repeat_service" : "warming_repeat_service");
        if (decision.repeat) {
            EXPECT_TRUE(decision.probing);
            ++probeDecisions;
        }
    }

    EXPECT_EQ(probeDecisions, 2u);
    EXPECT_EQ(state.probeRepeats, 2u);
    EXPECT_EQ(state.proactiveRepeats, 2u);
    EXPECT_LE(state.maxConsecutiveProactiveRepeats, 1u);
}

TEST(CapturePipelinePolicyTest, CfrRepeatCatchupRequiresMeasuredHeadroomAndRejectsMuxPressure) {
    constexpr double kFrameIntervalMs = 1000.0 / 120.0;
    constexpr uint32_t kSamples = policy::kWgcOverloadRepeatPacerMinSamples;

    EXPECT_TRUE(policy::ShouldAllowCfrRepeatCatchupUnderFreshPressure(
        true, false, 5.0, kFrameIntervalMs, kSamples));
    EXPECT_FALSE(policy::ShouldAllowCfrRepeatCatchupUnderFreshPressure(
        false, false, 5.0, kFrameIntervalMs, kSamples));
    EXPECT_FALSE(policy::ShouldAllowCfrRepeatCatchupUnderFreshPressure(
        true, true, 5.0, kFrameIntervalMs, kSamples));
    EXPECT_FALSE(policy::ShouldAllowCfrRepeatCatchupUnderFreshPressure(
        true, false, 5.0, kFrameIntervalMs, kSamples - 1u));
    EXPECT_FALSE(policy::ShouldAllowCfrRepeatCatchupUnderFreshPressure(
        true, false, kFrameIntervalMs, kFrameIntervalMs, kSamples));
}

TEST(CapturePipelinePolicyTest, CfrOverloadPacingYieldsToFgSuspensionAndSourceUnderfeed) {
    EXPECT_TRUE(policy::IsCfrSourceHealthyForOverloadPacing(1.0, true, 480.0, 120.0));
    EXPECT_TRUE(policy::IsCfrSourceHealthyForOverloadPacing(1.0, true, 120.0, 120.0));
    EXPECT_FALSE(policy::IsCfrSourceHealthyForOverloadPacing(1.0, true, 115.0, 120.0));
    EXPECT_FALSE(policy::IsCfrSourceHealthyForOverloadPacing(0.89, true, 480.0, 120.0));
    EXPECT_TRUE(policy::IsCfrSourceHealthyForOverloadPacing(1.0, false, 0.0, 120.0));
}

TEST(CapturePipelinePolicyTest, DynamicOverlayRepeatsFreezeOnlyAcrossConfirmedFreshFramePressure) {
    constexpr double kFrameIntervalMs = 1000.0 / 120.0;
    policy::CfrDynamicOverlayRepeatState state;

    auto decision = policy::UpdateCfrDynamicOverlayRepeatState(state, true, 10.0, kFrameIntervalMs);
    EXPECT_FALSE(decision.frozen);
    EXPECT_FALSE(decision.entered);
    decision = policy::UpdateCfrDynamicOverlayRepeatState(state, true, 10.0, kFrameIntervalMs);
    EXPECT_TRUE(decision.frozen);
    EXPECT_TRUE(decision.entered);
    EXPECT_EQ(state.episodes, 1u);

    for (uint32_t frame = 1; frame < policy::kCfrDynamicOverlayRepeatExitConfirmFrames; ++frame) {
        decision = policy::UpdateCfrDynamicOverlayRepeatState(state, true, 5.0, kFrameIntervalMs);
        EXPECT_TRUE(decision.frozen);
        EXPECT_FALSE(decision.exited);
    }
    decision = policy::UpdateCfrDynamicOverlayRepeatState(state, true, 5.0, kFrameIntervalMs);
    EXPECT_FALSE(decision.frozen);
    EXPECT_TRUE(decision.exited);

    state.frozen = true;
    state.frozenRepeats = 17;
    decision = policy::UpdateCfrDynamicOverlayRepeatState(state, false, 10.0, kFrameIntervalMs);
    EXPECT_TRUE(decision.exited);
    EXPECT_FALSE(state.frozen);
    EXPECT_EQ(state.episodes, 1u);
    EXPECT_EQ(state.frozenRepeats, 17u);
}

TEST(CapturePipelinePolicyTest, WgcIngressBudgetMeterThinsFastSourceBeforeTheReservoirFills) {
    // 60 fps CFR from a 144 Hz window: the reservoir is sized for 75 source frames/s, so a
    // half-empty reservoir must already be metered instead of admitting every 144 Hz frame.
    const auto exhausted = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/12, /*retainedFrameCap=*/35, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/60, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/2.0, /*freeCopySlots=*/25, /*reservedFreeCopySlots=*/6,
        /*uniformPlayoutOwnsSurplus=*/true, /*budgetCreditFrames=*/0.4);
    EXPECT_FALSE(exhausted.accept);
    EXPECT_TRUE(exhausted.decimated);
    EXPECT_FALSE(exhausted.softReservePressure);
    EXPECT_STREQ(exhausted.reason, "wgc_ingress_decimated_credit");

    const auto funded = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/12, /*retainedFrameCap=*/35, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/60, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/2.0, /*freeCopySlots=*/25, /*reservedFreeCopySlots=*/6,
        /*uniformPlayoutOwnsSurplus=*/true, /*budgetCreditFrames=*/1.0);
    EXPECT_TRUE(funded.accept);
    EXPECT_FALSE(funded.decimated);
    EXPECT_STREQ(funded.reason, "healthy");
}

TEST(CapturePipelinePolicyTest, WgcIngressBudgetMeterNeverBlocksLowWaterRecoveryOrSlowSources) {
    const auto lowWater = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/5, /*retainedFrameCap=*/35, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/60, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/0.0, /*freeCopySlots=*/25, /*reservedFreeCopySlots=*/6,
        /*uniformPlayoutOwnsSurplus=*/true, /*budgetCreditFrames=*/0.0);
    EXPECT_TRUE(lowWater.accept);
    EXPECT_STREQ(lowWater.reason, "low_water");

    const auto recovery = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/20, /*retainedFrameCap=*/35, /*lowWaterFrames=*/8, /*recovering=*/true,
        /*outputFps=*/60, /*recentInputMin250Fps=*/144, /*recentInputMin500Fps=*/144,
        /*admissionCreditFrames=*/0.0, /*freeCopySlots=*/25, /*reservedFreeCopySlots=*/6,
        /*uniformPlayoutOwnsSurplus=*/true, /*budgetCreditFrames=*/0.0);
    EXPECT_TRUE(recovery.accept);
    EXPECT_STREQ(recovery.reason, "recovery");

    const auto belowTarget = policy::DecideWgcIngressAdmission(
        /*retainedFrames=*/20, /*retainedFrameCap=*/35, /*lowWaterFrames=*/8, /*recovering=*/false,
        /*outputFps=*/60, /*recentInputMin250Fps=*/40, /*recentInputMin500Fps=*/40,
        /*admissionCreditFrames=*/0.0, /*freeCopySlots=*/25, /*reservedFreeCopySlots=*/6,
        /*uniformPlayoutOwnsSurplus=*/false, /*budgetCreditFrames=*/0.0);
    EXPECT_TRUE(belowTarget.accept);
    EXPECT_STREQ(belowTarget.reason, "source_below_cfr_target");
}

namespace {

struct BudgetMeterRun {
    uint32_t accepted = 0;
    uint32_t decimated = 0;
    uint32_t longestRejectRun = 0;
};

BudgetMeterRun RunBudgetMeter(uint32_t outputFps, double sourceFps, double seconds) {
    BudgetMeterRun run{};
    double credit = 1.0;
    uint32_t rejectRun = 0;
    const double intervalSeconds = 1.0 / sourceFps;
    const uint32_t frames = static_cast<uint32_t>(seconds * sourceFps);
    for (uint32_t i = 0; i < frames; ++i) {
        credit = policy::AdvanceWgcIngressBudgetCredit(credit, i == 0 ? 0.0 : intervalSeconds, outputFps);
        const auto decision = policy::DecideWgcIngressAdmission(
            /*retainedFrames=*/12, /*retainedFrameCap=*/35, /*lowWaterFrames=*/8, /*recovering=*/false, outputFps,
            static_cast<uint32_t>(sourceFps), static_cast<uint32_t>(sourceFps),
            /*admissionCreditFrames=*/2.0, /*freeCopySlots=*/25, /*reservedFreeCopySlots=*/6,
            /*uniformPlayoutOwnsSurplus=*/true, credit);
        if (decision.accept) {
            ++run.accepted;
            credit = std::max(0.0, credit - 1.0);
            rejectRun = 0;
        } else {
            ++run.decimated;
            run.longestRejectRun = std::max(run.longestRejectRun, ++rejectRun);
        }
    }
    return run;
}

}  // namespace

TEST(CapturePipelinePolicyTest, WgcIngressBudgetMeterHoldsFastSourcesToTheReservoirBudgetRate) {
    // 144 Hz into 60 fps: budget 75/s. The reservoir window (e.g. 331 ms) then holds ~25 frames
    // instead of ~48, which is what the pool was sized for, and the spacing stays regular.
    const BudgetMeterRun into60 = RunBudgetMeter(60, 144.0, 10.0);
    EXPECT_GE(into60.accepted, 700u);
    EXPECT_LE(into60.accepted, 790u);
    EXPECT_LE(into60.longestRejectRun, 2u);

    // 240 Hz into 120 fps: budget 150/s.
    const BudgetMeterRun into120 = RunBudgetMeter(120, 240.0, 10.0);
    EXPECT_GE(into120.accepted, 1450u);
    EXPECT_LE(into120.accepted, 1560u);
    EXPECT_LE(into120.longestRejectRun, 2u);
}

TEST(CapturePipelinePolicyTest, WgcIngressBudgetMeterAdmitsEverySourceFrameAtOrBelowTheBudget) {
    const std::pair<uint32_t, double> cases[] = {{120, 144.0}, {120, 120.0}, {60, 60.0},
                                                 {60, 74.0},   {144, 144.0}, {30, 37.0}};
    for (const auto& [outputFps, sourceFps] : cases) {
        const BudgetMeterRun run = RunBudgetMeter(outputFps, sourceFps, 10.0);
        EXPECT_EQ(run.decimated, 0u) << outputFps << " fps CFR from " << sourceFps << " Hz";
    }
}

TEST(CapturePipelinePolicyTest, WgcIngressBudgetCreditRefillIsCappedAndIgnoresClockGoingBackwards) {
    EXPECT_DOUBLE_EQ(policy::AdvanceWgcIngressBudgetCredit(0.0, 10.0, 60), policy::kWgcIngressBudgetCreditCapFrames);
    EXPECT_DOUBLE_EQ(policy::AdvanceWgcIngressBudgetCredit(0.5, -1.0, 60), 0.5);
    EXPECT_NEAR(policy::AdvanceWgcIngressBudgetCredit(0.0, 0.01, 60), 0.75, 1e-9);
}

TEST(CapturePipelinePolicyTest, WgcSmoothnessRequestedDelayIsTheFloorWhenOnlyAFloorWasRequested) {
    constexpr int64_t kReservoirQpc = 3000000;  // 300 ms at 10 MHz
    constexpr int64_t kFloorQpc = 224840;       // 22.5 ms
    // Video-only floor: the end-of-session deficit is judged against the floor, not the 300 ms headroom.
    EXPECT_EQ(policy::GetWgcSmoothnessRequestedDelayQpc(kReservoirQpc, true, false, kFloorQpc), kFloorQpc);
    // An audio-latency delay takes the whole reservoir target, with or without a floor configured.
    EXPECT_EQ(policy::GetWgcSmoothnessRequestedDelayQpc(kReservoirQpc, true, true, kFloorQpc), kReservoirQpc);
    EXPECT_EQ(policy::GetWgcSmoothnessRequestedDelayQpc(kReservoirQpc, false, false, 0), kReservoirQpc);
    // A floor can never ask for more than the reservoir can hold, and a floor that was never armed asks for none.
    EXPECT_EQ(policy::GetWgcSmoothnessRequestedDelayQpc(kFloorQpc, true, false, kReservoirQpc), kFloorQpc);
    EXPECT_EQ(policy::GetWgcSmoothnessRequestedDelayQpc(kReservoirQpc, true, false, 0), 0);
    EXPECT_EQ(policy::GetWgcSmoothnessRequestedDelayQpc(kReservoirQpc, true, false, -5), 0);
}

namespace {

// One recording start as the startup reserve wait sees it (logs/20261025_120518):
// 10 MHz QPC, 120 fps output, a 328.9 ms start delay (28.9 ms audio latency plus the
// 300 ms smoothness reserve) and a 58-frame retained cap (64 slots, 6 reserved).
constexpr int64_t kStartQpcPerSecond = 10000000;
constexpr int64_t kStartOutputIntervalQpc = 83333;
constexpr int64_t kStartDelayQpc = 3288890;
constexpr int64_t kStartToleranceQpc = 41666;  // min(half the output interval, 5 ms)
constexpr uint32_t kStartRetainedCap = 58;
constexpr int64_t kEightyFourFpsQpc = 119048;

struct StartupWaitRun {
    bool done = false;
    int64_t doneQpc = 0;
    int64_t startTargetQpc = 0;  // latest - delay when the wait ended
    int64_t selectedDelayQpc = 0;
    size_t newer = 0;
    size_t maxLeased = 0;  // what ingress sees before a pass trims
};

// Frames arrive every sourceIntervalQpc from phaseQpc (none inside [holeBegin, holeEnd));
// every output interval the wait evaluates the reserve and trims to its retained limit,
// as MediaEncoderSession::CommitWarmupSync does, until the 1 s budget runs out.
StartupWaitRun RunStartupWait(int64_t sourceIntervalQpc, int64_t phaseQpc, int64_t holeBeginQpc = 0,
                              int64_t holeEndQpc = 0, uint32_t retainedCap = kStartRetainedCap) {
    StartupWaitRun run;
    std::deque<int64_t> buffered;
    int64_t nextArrivalQpc = phaseQpc;
    const uint32_t outputLowWater = policy::GetWgcDelayReservoirLowWaterFrames(kStartDelayQpc, kStartOutputIntervalQpc);
    const uint32_t limit = policy::GetWgcStartupWaitRetainedLimit(retainedCap);
    for (int64_t now = kStartOutputIntervalQpc; now <= kStartQpcPerSecond; now += kStartOutputIntervalQpc) {
        for (; nextArrivalQpc <= now; nextArrivalQpc += sourceIntervalQpc) {
            if (nextArrivalQpc < holeBeginQpc || nextArrivalQpc >= holeEndQpc)
                buffered.push_back(nextArrivalQpc);
        }
        run.maxLeased = std::max(run.maxLeased, buffered.size());
        const std::vector<int64_t> qpcs(buffered.begin(), buffered.end());
        auto selection =
            policy::SelectWgcStartupReserveCandidate(qpcs.data(), qpcs.size(), kStartDelayQpc, kStartToleranceQpc);
        const bool atOrAboveCfr = policy::IsWgcStartupCandidateCadenceAtOrAboveCfrTarget(
            qpcs.size(), selection.reserveSpanQpc, kStartOutputIntervalQpc);
        const uint32_t lowWater = policy::GetWgcStartupReserveLowWaterFrames(qpcs.data(), qpcs.size(), kStartDelayQpc,
                                                                             outputLowWater, atOrAboveCfr);
        const auto countQualified = policy::SelectWgcStartupReserveCandidate(
            qpcs.data(), qpcs.size(), kStartDelayQpc, kStartToleranceQpc, lowWater, kStartOutputIntervalQpc);
        if (countQualified.usedDelayReserve)
            selection = countQualified;
        run.newer = qpcs.empty() ? 0 : qpcs.size() - selection.selectedIndex - 1;
        if (selection.usedDelayReserve && run.newer >= lowWater) {
            run.done = true;
            run.doneQpc = now;
            run.startTargetQpc = qpcs.back() - kStartDelayQpc;
            run.selectedDelayQpc = selection.selectedDelayQpc;
            return run;
        }
        while (buffered.size() > limit)
            buffered.pop_front();
    }
    return run;
}

}  // namespace

// logs/20261025_120518 (Talos 4K at ~84 fps into 120 fps CFR): the wait asked for 40
// frames newer than the start, the game delivers ~28 in 328.9 ms, so it ran its whole
// 1 s budget, the pool filled at 58 and ingress dropped the 40 newest frames; the
// video began with ~0.3 s of one held frame. At any alignment of the source to the
// wait's passes, the wait must end once one delay of frames is in, below the cap. A
// 65 fps source puts the frame nearest the target 5.8 ms too young (more than the
// tolerance), which the selection rejected on every pass before.
TEST(CapturePipelinePolicyTest, WgcStartupWaitBelowTheOutputRateEndsAfterOneDelayBelowThePoolCap) {
    for (const int64_t interval :
         {kStartQpcPerSecond / 60, kStartQpcPerSecond / 65, kEightyFourFpsQpc, kStartQpcPerSecond / 119}) {
        for (int64_t phase = 0; phase < interval; phase += interval / 16) {
            const StartupWaitRun run = RunStartupWait(interval, phase);
            ASSERT_TRUE(run.done) << "interval " << interval << " phase " << phase << ": newer=" << run.newer;
            EXPECT_LE(run.doneQpc, phase + kStartDelayQpc + 2 * interval + kStartOutputIntervalQpc)
                << "interval " << interval << " phase " << phase;
            EXPECT_GE(run.selectedDelayQpc, kStartDelayQpc - kStartToleranceQpc) << "interval " << interval;
            EXPECT_GE(run.newer, static_cast<size_t>(kStartDelayQpc / interval - 1)) << "interval " << interval;
            EXPECT_LT(run.maxLeased, kStartRetainedCap)
                << "interval " << interval << " phase " << phase << ": ingress would drop the newest frames";
        }
    }
}

TEST(CapturePipelinePolicyTest, WgcStartupWaitAtOrAboveTheOutputRateKeepsTheOutputLowWater) {
    for (const int64_t interval : {kStartOutputIntervalQpc, kStartQpcPerSecond / 144}) {
        for (int64_t phase = 0; phase < interval; phase += interval / 16) {
            const StartupWaitRun run = RunStartupWait(interval, phase);
            ASSERT_TRUE(run.done) << "interval " << interval << " phase " << phase;
            EXPECT_GE(run.newer, policy::GetWgcDelayReservoirLowWaterFrames(kStartDelayQpc, kStartOutputIntervalQpc));
            EXPECT_LE(run.doneQpc, phase + kStartDelayQpc + 2 * interval + kStartOutputIntervalQpc);
            EXPECT_LT(run.maxLeased, kStartRetainedCap);
        }
    }
}

// A 250 ms hitch at the start is one long interval: the median ignores it, so the wait
// still asks for a hole-free window and ends only once the start target is within a few
// frames of the hitch's end (a mean interval would start the video inside the hitch).
TEST(CapturePipelinePolicyTest, WgcStartupWaitBelowTheOutputRateWaitsOutAHitch) {
    const int64_t holeBegin = 2000000;
    const int64_t holeEnd = 4500000;
    const StartupWaitRun run = RunStartupWait(kEightyFourFpsQpc, 0, holeBegin, holeEnd);
    ASSERT_TRUE(run.done) << "newer=" << run.newer;
    EXPECT_GE(run.startTargetQpc, holeEnd - 3 * kEightyFourFpsQpc)
        << "the start would hold a frame over most of the hitch";
    EXPECT_LT(run.maxLeased, kStartRetainedCap);
}

TEST(CapturePipelinePolicyTest, WgcStartupReserveLowWaterCountsWhatTheSourceDelivers) {
    std::vector<int64_t> eightyFour;
    for (int i = 0; i < 30; ++i)
        eightyFour.push_back(1000000 + i * kEightyFourFpsQpc);
    // floor(328.9 ms / 11.9 ms) - 1 of phase slack, instead of 40 output frames.
    const size_t count = eightyFour.size();
    EXPECT_EQ(policy::GetWgcStartupReserveLowWaterFrames(eightyFour.data(), count, kStartDelayQpc, 40, false), 26u);
    // At or above the output rate (by either proof) the validated output count stays.
    EXPECT_EQ(policy::GetWgcStartupReserveLowWaterFrames(eightyFour.data(), count, kStartDelayQpc, 40, true), 40u);
    // A fast source never asks for more than the output count.
    std::vector<int64_t> fast;
    for (int i = 0; i < 50; ++i)
        fast.push_back(1000000 + i * (kStartQpcPerSecond / 135));
    EXPECT_EQ(policy::GetWgcStartupReserveLowWaterFrames(fast.data(), fast.size(), kStartDelayQpc, 40, false), 40u);
    // Too few intervals to measure, or nothing to wait for: the output count.
    EXPECT_EQ(policy::GetWgcStartupReserveLowWaterFrames(eightyFour.data(), 2, kStartDelayQpc, 40, false), 40u);
    EXPECT_EQ(policy::GetWgcStartupReserveLowWaterFrames(nullptr, 30, kStartDelayQpc, 40, false), 40u);
    EXPECT_EQ(policy::GetWgcStartupReserveLowWaterFrames(eightyFour.data(), eightyFour.size(), 0, 0, false), 0u);
    // A source slower than the delay still needs one newer frame.
    const int64_t slow[] = {1000000, 6000000, 11000000};
    EXPECT_EQ(policy::GetWgcStartupReserveLowWaterFrames(slow, 3, kStartDelayQpc, 40, false), 1u);
}

// A steady source keeps one phase against the target; when the nearest frame is too
// young to leave delay - tolerance of reserve, the next older one is the start
// (llm-wiki/log/archive-2026-W39h.md, 2026-09-27: it was rejected on every pass).
TEST(CapturePipelinePolicyTest, WgcStartupReserveSelectionTakesTheNextOlderFrameWhenTheNearestIsTooYoung) {
    const int64_t delay = 50000 + 27 * kEightyFourFpsQpc;  // the target sits 50000 before frame 1
    std::vector<int64_t> frames;
    for (int i = 0; i <= 28; ++i)
        frames.push_back(1000000 + i * kEightyFourFpsQpc);
    const auto selection =
        policy::SelectWgcStartupReserveCandidate(frames.data(), frames.size(), delay, kStartToleranceQpc);
    ASSERT_TRUE(selection.usedDelayReserve);
    EXPECT_EQ(selection.selectedIndex, 0u);
    EXPECT_EQ(selection.selectedDelayQpc, 28 * kEightyFourFpsQpc);
    EXPECT_EQ(selection.targetSelectionQpc, frames[1] - 50000);
}

TEST(CapturePipelinePolicyTest, WgcStartupReserveSelectionHonorsTheRequiredNewerFrameCount) {
    std::vector<int64_t> frames;
    for (int i = 0; i <= 40; ++i)
        frames.push_back(1000000 + i * kStartOutputIntervalQpc);
    const auto nearest =
        policy::SelectWgcStartupReserveCandidate(frames.data(), frames.size(), kStartDelayQpc, kStartToleranceQpc);
    ASSERT_TRUE(nearest.usedDelayReserve);
    EXPECT_EQ(nearest.selectedIndex, 1u);  // within time tolerance, but only 39 newer frames
    const auto qualified = policy::SelectWgcStartupReserveCandidate(
        frames.data(), frames.size(), kStartDelayQpc, kStartToleranceQpc, 40, kStartOutputIntervalQpc);
    ASSERT_TRUE(qualified.usedDelayReserve);
    EXPECT_EQ(qualified.selectedIndex, 0u);
    EXPECT_GE(qualified.selectedDelayQpc, kStartDelayQpc - kStartToleranceQpc);
    const auto underfilled = policy::SelectWgcStartupReserveCandidate(
        frames.data(), frames.size() - 1, kStartDelayQpc, kStartToleranceQpc, 40, kStartOutputIntervalQpc);
    EXPECT_FALSE(underfilled.usedDelayReserve);
    frames[0] -= 500000;  // 50 ms hole: never reach behind it just to meet the count
    const auto beforeHole = policy::SelectWgcStartupReserveCandidate(
        frames.data(), frames.size(), kStartDelayQpc, kStartToleranceQpc, 40, kStartOutputIntervalQpc);
    EXPECT_FALSE(beforeHole.usedDelayReserve);
}

// A pool too small for one delay of frames runs the wait to its budget; the trim must
// still keep it below the cap, or ingress drops the newest frames for the oldest.
TEST(CapturePipelinePolicyTest, WgcStartupWaitWithASmallPoolNeverReachesTheCap) {
    const uint32_t smallCap = 30;
    const StartupWaitRun run = RunStartupWait(kEightyFourFpsQpc, 0, 0, 0, smallCap);
    EXPECT_FALSE(run.done);
    EXPECT_LT(run.maxLeased, smallCap);
}

TEST(CapturePipelinePolicyTest, WgcStartupWaitRetainedLimitLeavesIngressHeadroomBelowTheCap) {
    EXPECT_EQ(policy::GetWgcStartupWaitRetainedLimit(58), 53u);
    EXPECT_EQ(policy::GetWgcStartupWaitRetainedLimit(8), 6u);  // a small pool keeps most slots
    EXPECT_EQ(policy::GetWgcStartupWaitRetainedLimit(3), 3u);
    EXPECT_EQ(policy::GetWgcStartupWaitRetainedLimit(0), 0u);
}
