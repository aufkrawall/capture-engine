#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <random>
#include <vector>

#include "mediaengine/video/encoder_rate_compensation.h"

// NVENC spends bitRate / fps per frame it receives. With skipped slots the
// controller scales the rate so the real-time output stays at the target: the
// budget model below spends exactly that per submitted frame and checks the
// output against the target in every one-second window.

namespace {

using ce::media::SubmittedRateCompensation;

constexpr int kFps = 120;
constexpr int64_t kBitRate = 125'000'000;
constexpr int64_t kMaxRate = 200'000'000;
// The live cap, plus what kLiveMinFactor can leave above it right after a
// burst from a very high factor (paying it back fully would need frames below
// half the base rate).
constexpr double kLivePeakBound = SubmittedRateCompensation::kPeakAllowance + 0.05;

struct Budget {
    std::deque<double> lastSecond;
    double total = 0.0;
    double peakSecond = 0.0;
    void Slot(double bits) {
        lastSecond.push_back(bits);
        total += bits;
        if (lastSecond.size() > static_cast<size_t>(kFps))
            lastSecond.pop_front();
        if (lastSecond.size() == static_cast<size_t>(kFps)) {
            double sum = 0.0;
            for (double value : lastSecond)
                sum += value;
            peakSecond = std::max(peakSecond, sum);
        }
    }
};

// Runs `submitted` per slot; returns the bits a per-frame budget encoder spends.
Budget RunSlots(SubmittedRateCompensation& rc, const std::vector<bool>& submitted) {
    Budget budget;
    for (bool slot : submitted) {
        // As VideoEncoder does it: observe the slot, then send with Current().
        rc.ObserveSlot(slot);
        rc.TakeChange();
        budget.Slot(slot ? static_cast<double>(rc.Current().bitRate) / kFps : 0.0);
    }
    return budget;
}

std::vector<bool> Pattern(int slots, int uniqueEvery) {
    std::vector<bool> out;
    out.reserve(static_cast<size_t>(std::max(slots, 0)));
    for (int slot = 0; slot < slots; ++slot)
        out.push_back(slot % uniqueEvery == 0);
    return out;
}

TEST(SubmittedRateCompensationTest, ConstantQpHasNothingToScale) {
    SubmittedRateCompensation rc;
    rc.Reset(kFps, 0, 0, false);
    EXPECT_FALSE(rc.Active());
    for (bool slot : Pattern(600, 3))
        rc.ObserveSlot(slot);
    EXPECT_FALSE(rc.TakeChange());
    EXPECT_EQ(rc.Current().factor, 1.0);
}

TEST(SubmittedRateCompensationTest, FullRateNeverReconfigures) {
    SubmittedRateCompensation rc;
    rc.Reset(kFps, kBitRate, kMaxRate, false);
    for (bool slot : Pattern(1200, 1))
        rc.ObserveSlot(slot);
    EXPECT_FALSE(rc.TakeChange());
    EXPECT_EQ(rc.Changes(), 0u);
}

TEST(SubmittedRateCompensationTest, SteadyRepeatsSettleOnSlotsPerSubmittedFrame) {
    for (int uniqueEvery : {2, 4, 10}) {
        SCOPED_TRACE(uniqueEvery);
        SubmittedRateCompensation rc;
        rc.Reset(kFps, kBitRate, kMaxRate, false);
        const Budget budget = RunSlots(rc, Pattern(kFps * 10, uniqueEvery));
        EXPECT_NEAR(rc.Current().factor, uniqueEvery, uniqueEvery * 0.1);
        EXPECT_NEAR(static_cast<double>(rc.Current().maxRate) / kMaxRate, rc.Current().factor, 1e-6);
        // Steps only while the window fills; a steady cadence never toggles.
        const uint64_t settled = rc.Changes();
        RunSlots(rc, Pattern(kFps * 10, uniqueEvery));
        EXPECT_EQ(rc.Changes(), settled);
        EXPECT_LE(budget.peakSecond, kBitRate * 1.05);
        EXPECT_GE(budget.total, kBitRate * 10 * 0.85);
    }
}

TEST(SubmittedRateCompensationTest, ReturningToFullRateDropsTheFactorWithinTheShortWindow) {
    SubmittedRateCompensation rc;
    rc.Reset(kFps, kBitRate, kMaxRate, false);
    RunSlots(rc, Pattern(kFps * 3, 4));
    ASSERT_GT(rc.Current().factor, 3.5);
    int slotsUntilFull = 0;
    while (rc.Current().factor > 1.0 && slotsUntilFull < kFps) {
        rc.ObserveSlot(true);
        ++slotsUntilFull;
    }
    EXPECT_EQ(rc.Current().factor, 1.0);
    EXPECT_LE(slotsUntilFull, static_cast<int>(rc.ShortWindow()));
}

TEST(SubmittedRateCompensationTest, RealTimeRateStaysAtTheTargetThroughTransitions) {
    // NOLINTNEXTLINE(bugprone-random-generator-seed) - a fixed seed keeps the test deterministic
    std::mt19937 random(7);
    std::vector<bool> slots;
    for (int segment = 0; segment < 40; ++segment) {
        const int uniqueEvery = 1 + static_cast<int>(random() % 6);
        const int length = kFps / 4 + static_cast<int>(random() % (kFps * 2));
        for (int slot = 0; slot < length; ++slot)
            slots.push_back(slot % uniqueEvery == 0);
    }
    const double seconds = static_cast<double>(slots.size()) / kFps;
    for (bool live : {false, true}) {
        SCOPED_TRACE(live);
        SubmittedRateCompensation rc;
        rc.Reset(kFps, kBitRate, kMaxRate, live);
        const Budget budget = RunSlots(rc, slots);
        // A file only overshoots while the short window catches a speed-up; a
        // live stream never leaves the hard cap. Both stay near the target.
        EXPECT_LE(budget.peakSecond, kBitRate * (live ? kLivePeakBound : 1.5));
        EXPECT_GE(budget.total, kBitRate * seconds * 0.8);
        EXPECT_LE(budget.total, kBitRate * seconds * 1.05);
    }
}

// A live stream must not burst the uplink: a source that jumps from every 6th slot to
// every slot would otherwise spend the 6x budget until the estimate caught up.
TEST(SubmittedRateCompensationTest, ASourceThatSpeedsUpCannotBurstTheLastSecond) {
    std::vector<bool> slots = Pattern(kFps * 3, 6);
    const std::vector<bool> fresh = Pattern(kFps * 2, 1);
    slots.insert(slots.end(), fresh.begin(), fresh.end());
    SubmittedRateCompensation rc;
    rc.Reset(kFps, kBitRate, kMaxRate, true);
    const Budget budget = RunSlots(rc, slots);
    EXPECT_LE(budget.peakSecond, kBitRate * kLivePeakBound);
    EXPECT_GT(rc.PeakCapped(), 0u);
    EXPECT_EQ(rc.Current().factor, 1.0);
}

TEST(SubmittedRateCompensationTest, EncoderRateLimitAndRejectionsCapTheFactor) {
    SubmittedRateCompensation rc;
    rc.Reset(kFps, 1'500'000'000, 2'000'000'000, false);
    RunSlots(rc, Pattern(kFps * 4, 10));
    EXPECT_LE(rc.Current().maxRate, int64_t{4'294'967'295});
    EXPECT_GT(rc.Current().factor, 1.5);

    SubmittedRateCompensation rejected;
    rejected.Reset(kFps, kBitRate, kMaxRate, false);
    RunSlots(rejected, Pattern(kFps * 4, 8));
    const double refused = rejected.Current().factor;
    rejected.OnRejected();
    EXPECT_TRUE(rejected.TakeChange());
    EXPECT_LT(rejected.Current().factor, refused);
    RunSlots(rejected, Pattern(kFps * 4, 10));
    EXPECT_LT(rejected.Current().factor, refused);
    EXPECT_EQ(rejected.Rejections(), 1u);
}

}  // namespace
