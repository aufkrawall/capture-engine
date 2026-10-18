#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "common/capture/capture_pipeline_policy.h"
#include "source_fragment_reader.h"

namespace policy = ce::capture_policy;

namespace {

constexpr int64_t kSlot = 83'333;            // 120 fps at a 10 MHz QPC
constexpr int64_t kLead = kSlot / 2;          // inject lead tolerance
constexpr int64_t kTarget = 1'000'000'000;

policy::CfrHorizonPlayoutDecision Decide(const std::vector<int64_t>& timestamps, const std::vector<bool>& ready,
                                         int64_t shown, bool nearestEmit, size_t nearestIndex) {
    std::vector<int64_t> ts = timestamps;
    bool readyFlags[policy::kCfrHorizonPlayoutMaxCandidates] = {};
    for (size_t i = 0; i < ready.size() && i < policy::kCfrHorizonPlayoutMaxCandidates; ++i)
        readyFlags[i] = ready[i];
    policy::CfrHorizonPlayoutInput input;
    input.timestampsQpc = ts.data();
    input.readyNow = readyFlags;
    input.count = ts.size();
    input.shownTimestampQpc = shown;
    input.targetQpc = kTarget;
    input.slotQpc = kSlot;
    input.leadToleranceQpc = kLead;
    input.lagBoundQpc = kSlot;
    return policy::DecideCfrHorizonPlayout(input, nearestEmit, nearestIndex);
}

int64_t At(double slots) {
    return kTarget + static_cast<int64_t>(std::llround(slots * static_cast<double>(kSlot)));
}

// --- Deterministic replay of the live inject selector (nearest, optionally with the horizon) ---

struct SourceFrame {
    int64_t arrivalQpc;
    int64_t timestampQpc;
};

struct PlayoutStats {
    uint32_t repeats = 0;
    uint32_t skips = 0;
    uint32_t floorRepeats = 0;
    uint32_t floorSkips = 0;
    uint32_t deviations = 0;
    double meanOffsetUs = 0.0;
    double p95AbsOffsetUs = 0.0;
    double maxAbsOffsetUs = 0.0;
    std::vector<int64_t> shownTimestamps;
    uint32_t ExcessPairs() const {
        const uint32_t extraRepeats = repeats > floorRepeats ? repeats - floorRepeats : 0;
        const uint32_t extraSkips = skips > floorSkips ? skips - floorSkips : 0;
        return std::min(extraRepeats, extraSkips);
    }
};

// Portable deterministic jitter: an LCG with a Box-Muller pair.
class Jitter {
public:
    explicit Jitter(uint64_t seed) : state_(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    double Gaussian() {
        const double u1 = (static_cast<double>(Next() >> 11) + 1.0) / 9007199254740993.0;
        const double u2 = static_cast<double>(Next() >> 11) / 9007199254740992.0;
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }

private:
    uint64_t Next() {
        state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
        return state_;
    }
    uint64_t state_;
};

std::vector<SourceFrame> MakeSource(double fps, double jitterUs, uint64_t seed, double wander = 0.02) {
    Jitter jitter(seed);
    std::vector<SourceFrame> frames;
    const double interval = 10'000'000.0 / fps;
    double t = 10'000'000.0;
    const int count = static_cast<int>(fps * 30.0);
    for (int i = 0; i < count; ++i) {
        t += interval * (1.0 + wander * std::sin(static_cast<double>(i) / (fps * 1.7)));  // slow rate wander
        const int64_t ts = static_cast<int64_t>(std::llround(t + jitter.Gaussian() * jitterUs * 10.0));
        frames.push_back({static_cast<int64_t>(std::llround(t)) - 120'000, ts});
    }
    std::sort(frames.begin(), frames.end(),
              [](const SourceFrame& a, const SourceFrame& b) { return a.timestampQpc < b.timestampQpc; });
    for (size_t i = 1; i < frames.size(); ++i)
        frames[i].timestampQpc = std::max(frames[i].timestampQpc, frames[i - 1].timestampQpc + 1);
    return frames;
}

PlayoutStats ReplayLiveSelector(const std::vector<SourceFrame>& source, bool horizon, int64_t gridOffset) {
    constexpr int64_t kContentDelay = 305'390;
    PlayoutStats stats;
    std::vector<int64_t> offsets;
    const int64_t start = 25'000'000 + gridOffset;
    const int64_t end = source.back().arrivalQpc - kSlot;
    size_t head = 0;
    size_t arrived = 0;
    int64_t lastShown = 0;
    bool shownAny = false;
    for (int64_t wall = start; wall < end; wall += kSlot) {
        const int64_t target = wall - kContentDelay;
        while (arrived < source.size() && source[arrived].arrivalQpc <= wall)
            ++arrived;
        // Live tail: the newest arrived frame stays protected (fence reserve of one).
        const size_t buffered = arrived > head ? arrived - head : 0;
        const size_t available = buffered > 1 ? buffered - 1 : 0;
        bool emit = false;
        size_t bestIdx = 0;
        if (available > 0) {
            int64_t bestDistance = 0;
            for (size_t i = 0; i < available; ++i) {
                const int64_t ts = source[head + i].timestampQpc;
                const int64_t distance = ts >= target ? ts - target : target - ts;
                if (i == 0 || distance < bestDistance) {
                    bestDistance = distance;
                    bestIdx = i;
                }
            }
            auto decision = policy::DecideCfrNearestPlayout(source[head + bestIdx].timestampQpc, target, kLead,
                                                            lastShown);
            if (horizon) {
                const size_t count = std::min(buffered, policy::kCfrHorizonPlayoutMaxCandidates);
                int64_t ts[policy::kCfrHorizonPlayoutMaxCandidates];
                bool ready[policy::kCfrHorizonPlayoutMaxCandidates];
                for (size_t i = 0; i < count; ++i) {
                    ts[i] = source[head + i].timestampQpc;
                    ready[i] = i < available;
                }
                policy::CfrHorizonPlayoutInput input;
                input.timestampsQpc = ts;
                input.readyNow = ready;
                input.count = count;
                input.shownTimestampQpc = lastShown;
                input.targetQpc = target;
                input.slotQpc = kSlot;
                input.leadToleranceQpc = kLead;
                input.lagBoundQpc = kSlot;
                const auto plan = policy::DecideCfrHorizonPlayout(input, decision.emit, bestIdx);
                if (plan.valid && (plan.emit != decision.emit || (plan.emit && plan.index != bestIdx))) {
                    ++stats.deviations;
                    decision.emit = plan.emit;
                    if (plan.emit)
                        bestIdx = plan.index;
                }
            }
            emit = decision.emit;
        }
        if (emit) {
            if (shownAny)
                stats.skips += static_cast<uint32_t>(bestIdx);
            head += bestIdx;
            lastShown = source[head].timestampQpc;
            ++head;
            shownAny = true;
        } else if (shownAny) {
            ++stats.repeats;
        }
        if (shownAny) {
            offsets.push_back(lastShown - target);
            stats.shownTimestamps.push_back(lastShown);
        }
    }
    // Per-second rate-conversion floor on the content timeline.
    for (int64_t window = start; window + 120 * kSlot <= end; window += 120 * kSlot) {
        const int64_t from = window - kContentDelay;
        const int64_t to = from + 120 * kSlot;
        const auto count = std::count_if(source.begin(), source.end(), [&](const SourceFrame& frame) {
            return frame.timestampQpc >= from && frame.timestampQpc < to;
        });
        stats.floorRepeats += count < 120 ? static_cast<uint32_t>(120 - count) : 0;
        stats.floorSkips += count > 120 ? static_cast<uint32_t>(count - 120) : 0;
    }
    double sum = 0.0;
    std::vector<double> absolute;
    for (const int64_t offset : offsets) {
        sum += static_cast<double>(offset) / 10.0;
        absolute.push_back(std::abs(static_cast<double>(offset)) / 10.0);
    }
    std::sort(absolute.begin(), absolute.end());
    stats.meanOffsetUs = offsets.empty() ? 0.0 : sum / static_cast<double>(offsets.size());
    stats.p95AbsOffsetUs = absolute.empty() ? 0.0 : absolute[absolute.size() * 95 / 100];
    stats.maxAbsOffsetUs = absolute.empty() ? 0.0 : absolute.back();
    return stats;
}

}  // namespace

// Nearest holds because its nearest frame is slightly too new, then skips the older frame the
// next slot: a repeat+skip pair. The older frame still fits this slot within one slot of lag.
TEST(CfrHorizonPlayoutTest, ShowsTheOlderFrameInsteadOfARepeatFollowedByItsSkip) {
    const std::vector<int64_t> ts = {At(-0.55), At(0.52), At(1.52)};
    const auto decision = Decide(ts, {true, true, true}, At(-1.0), false, 1);
    ASSERT_TRUE(decision.valid);
    EXPECT_TRUE(decision.emit);
    EXPECT_EQ(decision.index, 0u);
}

// Nearest shows the newer frame and skips the older one, and the slot after it has nothing new.
TEST(CfrHorizonPlayoutTest, KeepsTheOlderFrameWhenTheNextSlotWouldOtherwiseRepeat) {
    const std::vector<int64_t> ts = {At(-0.45), At(0.4), At(2.4)};
    const auto decision = Decide(ts, {true, true, true}, At(-1.0), true, 1);
    ASSERT_TRUE(decision.valid);
    EXPECT_TRUE(decision.emit);
    EXPECT_EQ(decision.index, 0u);
}

TEST(CfrHorizonPlayoutTest, MatchedCadenceKeepsTheNearestDecision) {
    const std::vector<int64_t> ts = {At(0.0), At(1.0), At(2.0), At(3.0)};
    const auto decision = Decide(ts, {true, true, true, false}, At(-1.0), true, 0);
    ASSERT_TRUE(decision.valid);
    EXPECT_TRUE(decision.emit);
    EXPECT_EQ(decision.index, 0u);
}

TEST(CfrHorizonPlayoutTest, LoneStaleFrameAfterAStallIsLeftToTheNearestRule) {
    // The nearest rule shows a lone late frame (a clean monotonic hold after a stall);
    // the horizon has no in-window candidate and must not turn it into a freeze.
    const auto decision = Decide({At(-3.0)}, {true}, At(-6.0), true, 0);
    EXPECT_FALSE(decision.valid);
    EXPECT_FALSE(Decide({}, {}, At(-1.0), false, 0).valid);
}

TEST(CfrHorizonPlayoutTest, NeverShowsAFrameWhosePixelsAreNotReadyYet) {
    // The older frame is still in flight; its timestamp may plan later slots only.
    const std::vector<int64_t> ts = {At(-0.55), At(0.52), At(1.52)};
    const auto decision = Decide(ts, {false, true, true}, At(-1.0), false, 1);
    if (decision.valid && decision.emit) {
        EXPECT_NE(decision.index, 0u);
    }
    const auto noneReady = Decide(ts, {false, false, false}, At(-1.0), false, 0);
    EXPECT_FALSE(noneReady.valid);
}

TEST(CfrHorizonPlayoutTest, NeverShowsAFrameOutsideTheTimingWindow) {
    const std::vector<int64_t> ts = {At(-1.2), At(0.6), At(1.6)};
    const auto decision = Decide(ts, {true, true, true}, At(-2.0), false, 1);
    if (decision.valid && decision.emit) {
        const int64_t offset = ts[decision.index] - kTarget;
        EXPECT_LE(offset, kLead);
        EXPECT_GE(offset, -kSlot);
    }
}

TEST(CfrHorizonPlayoutTest, SteadySourcesAtAndAwayFromTheRecordingRateAreUntouched) {
    for (const double fps : {60.0, 120.0, 144.0, 240.0}) {
        const auto source = MakeSource(fps, 0.0, 1, 0.0);
        for (int64_t phase = 0; phase < kSlot; phase += kSlot / 4) {
            const PlayoutStats nearest = ReplayLiveSelector(source, false, phase);
            const PlayoutStats horizon = ReplayLiveSelector(source, true, phase);
            EXPECT_EQ(horizon.repeats, nearest.repeats) << fps << " fps phase " << phase;
            EXPECT_EQ(horizon.skips, nearest.skips) << fps << " fps phase " << phase;
            EXPECT_NEAR(horizon.meanOffsetUs, nearest.meanOffsetUs, 50.0) << fps << " fps phase " << phase;
            EXPECT_NEAR(horizon.p95AbsOffsetUs, nearest.p95AbsOffsetUs, 100.0) << fps << " fps phase " << phase;
        }
    }
}

// The session cadence (Talos DLSS 3x, uncapped near 120 fps) in synthetic form: the nearest rule
// records repeat+skip pairs well above the rate-conversion floor; the horizon removes most of them
// and keeps content timing within one slot.
TEST(CfrHorizonPlayoutTest, JitteredSourceNearTheRecordingRateLosesMostExcessPairs) {
    for (const double fps : {118.0, 121.0}) {
        uint32_t nearestPairs = 0;
        uint32_t horizonPairs = 0;
        for (uint64_t seed = 1; seed <= 4; ++seed) {
            const auto source = MakeSource(fps, 300.0, seed + static_cast<uint64_t>(fps));
            const int64_t phase = static_cast<int64_t>(seed) * kSlot / 5;
            const PlayoutStats nearest = ReplayLiveSelector(source, false, phase);
            const PlayoutStats horizon = ReplayLiveSelector(source, true, phase);
            nearestPairs += nearest.ExcessPairs();
            horizonPairs += horizon.ExcessPairs();
            EXPECT_LE(horizon.maxAbsOffsetUs, static_cast<double>(kSlot) / 10.0 + 1.0) << fps << " fps";
            EXPECT_LE(horizon.p95AbsOffsetUs, nearest.p95AbsOffsetUs + 700.0) << fps << " fps";
            EXPECT_NEAR(horizon.meanOffsetUs, nearest.meanOffsetUs, 700.0) << fps << " fps";
            EXPECT_LE(horizon.repeats, nearest.repeats) << fps << " fps";
        }
        EXPECT_GE(nearestPairs, 40u) << fps << " fps: the scenario must reproduce the defect";
        EXPECT_LE(horizonPairs * 4, nearestPairs) << fps << " fps";
    }
}

TEST(CfrHorizonPlayoutTest, JitteredFastSourceGainsNoTimingBias) {
    for (const double fps : {144.0, 240.0}) {
        const auto source = MakeSource(fps, 1000.0, 9);
        const PlayoutStats nearest = ReplayLiveSelector(source, false, kSlot / 3);
        const PlayoutStats horizon = ReplayLiveSelector(source, true, kSlot / 3);
        EXPECT_LE(horizon.ExcessPairs(), nearest.ExcessPairs()) << fps << " fps";
        EXPECT_NEAR(horizon.meanOffsetUs, nearest.meanOffsetUs, 300.0) << fps << " fps";
        EXPECT_LE(horizon.p95AbsOffsetUs, nearest.p95AbsOffsetUs + 500.0) << fps << " fps";
    }
}

TEST(CfrHorizonPlayoutTest, ShownContentNeverMovesBackwards) {
    const auto source = MakeSource(119.0, 1000.0, 3);
    const PlayoutStats horizon = ReplayLiveSelector(source, true, kSlot / 2);
    ASSERT_GT(horizon.shownTimestamps.size(), 1000u);
    for (size_t i = 1; i < horizon.shownTimestamps.size(); ++i)
        ASSERT_GE(horizon.shownTimestamps[i], horizon.shownTimestamps[i - 1]) << "slot " << i;
}

TEST(CfrHorizonPlayoutTest, InjectSelectorAppliesTheHorizonBeforeDroppingFrames) {
    const std::string source = ce::test_source::ReadLogicalSource(std::filesystem::current_path() /
                                                                   "captureengine" / "media" / "media_main.cpp");
    ASSERT_FALSE(source.empty());
    const size_t nearest = source.find("auto decision = ce::capture_policy::DecideCfrNearestPlayout(");
    const size_t apply = source.find("applyInjectHorizonPlayout(availableCount, playoutTargetQpc", nearest);
    const size_t selected = source.find("const int64_t selectedTimestamp = bufferedInjectFrames[bestIdx].timestamp;",
                                        nearest);
    const size_t supersede = source.find("recordInjectTargetDrop(superseded);", nearest);
    ASSERT_NE(nearest, std::string::npos);
    ASSERT_NE(apply, std::string::npos);
    ASSERT_NE(selected, std::string::npos);
    ASSERT_NE(supersede, std::string::npos);
    EXPECT_LT(apply, selected) << "residual/logging must describe the frame actually shown";
    EXPECT_LT(apply, supersede);
    EXPECT_NE(source.find("input.lagBoundQpc = targetIntervalTicks;"), std::string::npos);
    EXPECT_NE(source.find("DecideCfrHorizonPlayout(input, decision.emit, bestIdx)"), std::string::npos);
}
