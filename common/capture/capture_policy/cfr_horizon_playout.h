#pragma once

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <limits>

// Backend-neutral CFR playout that avoids repeat+skip pairs by looking a few output slots ahead.

namespace ce::capture_policy {

// Nearest-timestamp selection (cfr_nearest_playout.h) decides each output slot on its own. When the
// source runs near the output rate, small timestamp jitter moves frames back and forth across the
// half-slot boundary: one slot repeats because its nearest frame is a little too new, and a later
// slot skips the frame that slot left behind. Each such pair is a visible 0-step/2-step stutter
// although the source supplied a frame per slot. Talos DLSS 3x at an uncapped 114-125 fps
// (logs/20261019_010437) recorded about 22 such pairs above the per-second rate-conversion floor in
// 28.6 s.
//
// The buffered frames already carry the timestamps of the next few slots, so the selector can plan
// those slots together: every repeat and every skipped frame costs one output slot, and every shown
// frame costs its distance to the slot's content target. The cheapest plan's first decision is
// applied and the plan is recomputed on the next slot. A plan may show a frame up to one slot before
// its target (`lagBoundQpc`) but never later than the normal lead tolerance, so content timing stays
// within one frame of the audio. Where the source is clearly above or below the output rate the
// required skips/repeats are the same for every plan, the timing term decides, and the choice equals
// nearest-timestamp selection; a tie also keeps the nearest choice. Replay of the Talos session and
// synthetic 60-240 fps sources: about two thirds fewer excess pairs at the session's cadence and
// nearly all at jittered 110-121 fps, p95 timing error +0.3 ms, worst case 11 -> 8 ms, and no change
// at steady 60/144/240 fps (llm-wiki/cfr-capture-sync.md, 2026-10-19).

constexpr uint32_t kCfrHorizonPlayoutSlots = 4;
constexpr size_t kCfrHorizonPlayoutMaxCandidates = 32;

struct CfrHorizonPlayoutInput {
    const int64_t* timestampsQpc = nullptr;  // fresh candidates, oldest first, all newer than the shown frame
    const bool* readyNow = nullptr;          // candidate may be shown on this slot (pixels available)
    size_t count = 0;
    int64_t shownTimestampQpc = 0;  // frame a repeat would show; 0 when nothing has been shown yet
    int64_t targetQpc = 0;          // content target of this slot
    int64_t slotQpc = 0;            // output interval
    int64_t leadToleranceQpc = 0;   // a shown frame may be at most this much newer than its target
    int64_t lagBoundQpc = 0;        // ... and at most this much older
    uint32_t horizonSlots = kCfrHorizonPlayoutSlots;
};

struct CfrHorizonPlayoutDecision {
    bool valid = false;  // false: no candidate fits this slot's window; keep the nearest decision
    bool emit = false;
    size_t index = 0;    // candidate to show when emit
    int64_t costQpc = 0;
};

namespace detail {

constexpr int64_t kCfrHorizonInfiniteCost = std::numeric_limits<int64_t>::max() / 4;

inline int64_t CfrHorizonDistance(int64_t a, int64_t b) {
    return a >= b ? a - b : b - a;
}

// Cheapest cost of slots [firstSlot, horizon) starting with `shownIndex` shown (-1: the prior frame).
inline int64_t CfrHorizonPlanCost(const CfrHorizonPlayoutInput& in, size_t count, int shownIndex,
                                  uint32_t firstSlot, uint32_t horizon) {
    int64_t cost[kCfrHorizonPlayoutMaxCandidates + 1];
    int64_t next[kCfrHorizonPlayoutMaxCandidates + 1];
    // State s holds the plan whose last shown frame is candidate s - 1 (s == 0: the prior frame).
    for (size_t s = 0; s <= count; ++s)
        cost[s] = kCfrHorizonInfiniteCost;
    cost[static_cast<size_t>(shownIndex + 1)] = 0;
    for (uint32_t slot = firstSlot; slot < horizon; ++slot) {
        const int64_t slotTarget = in.targetQpc + static_cast<int64_t>(slot) * in.slotQpc;
        for (size_t s = 0; s <= count; ++s)
            next[s] = kCfrHorizonInfiniteCost;
        for (size_t s = 0; s <= count; ++s) {
            if (cost[s] >= kCfrHorizonInfiniteCost)
                continue;
            const int64_t shownTs = s == 0 ? in.shownTimestampQpc : in.timestampsQpc[s - 1];
            const int64_t repeatCost =
                cost[s] + in.slotQpc + (shownTs > 0 ? CfrHorizonDistance(shownTs, slotTarget) : 0);
            next[s] = std::min(next[s], repeatCost);
            for (size_t i = s; i < count; ++i) {
                const int64_t offset = in.timestampsQpc[i] - slotTarget;
                if (offset > in.leadToleranceQpc)
                    break;
                if (offset < -in.lagBoundQpc)
                    continue;
                const int64_t skipped = static_cast<int64_t>(i - s);
                const int64_t showCost = cost[s] + skipped * in.slotQpc + CfrHorizonDistance(offset, 0);
                next[i + 1] = std::min(next[i + 1], showCost);
            }
        }
        std::copy(next, next + count + 1, cost);
    }
    int64_t best = kCfrHorizonInfiniteCost;
    for (size_t s = 0; s <= count; ++s)
        best = std::min(best, cost[s]);
    return best;
}

}  // namespace detail

// `nearestEmit`/`nearestIndex` describe what nearest-timestamp selection would do on this slot; it
// wins every tie, so the horizon only changes a decision that measurably avoids a repeat or skip.
inline CfrHorizonPlayoutDecision DecideCfrHorizonPlayout(const CfrHorizonPlayoutInput& in, bool nearestEmit,
                                                         size_t nearestIndex) {
    CfrHorizonPlayoutDecision decision;
    if (!in.timestampsQpc || !in.readyNow || in.count == 0 || in.slotQpc <= 0 || in.targetQpc <= 0 ||
        in.leadToleranceQpc < 0 || in.lagBoundQpc < 0 || in.horizonSlots == 0) {
        return decision;
    }
    const uint32_t horizon = std::min<uint32_t>(in.horizonSlots, 8);
    // Frames newer than the last planned slot can take no part in this plan.
    const int64_t newestUsableQpc =
        in.targetQpc + static_cast<int64_t>(horizon - 1) * in.slotQpc + in.leadToleranceQpc;
    size_t count = std::min(in.count, kCfrHorizonPlayoutMaxCandidates);
    while (count > 0 && in.timestampsQpc[count - 1] > newestUsableQpc)
        --count;
    if (count == 0)
        return decision;

    bool anyInWindow = false;
    for (size_t i = 0; i < count; ++i) {
        const int64_t offset = in.timestampsQpc[i] - in.targetQpc;
        if (offset > in.leadToleranceQpc)
            break;
        if (in.readyNow[i] && offset >= -in.lagBoundQpc) {
            anyInWindow = true;
            break;
        }
    }
    // Only stale frames (or none) are ready: a lone late frame after a stall is still shown by the
    // nearest rule, which keeps an in-gap freeze a clean monotonic hold.
    if (!anyInWindow)
        return decision;

    auto firstCost = [&](bool emit, size_t index) -> int64_t {
        if (!emit) {
            const int64_t hold = in.slotQpc + (in.shownTimestampQpc > 0
                                                   ? detail::CfrHorizonDistance(in.shownTimestampQpc, in.targetQpc)
                                                   : 0);
            const int64_t rest = detail::CfrHorizonPlanCost(in, count, -1, 1, horizon);
            return rest >= detail::kCfrHorizonInfiniteCost ? rest : hold + rest;
        }
        const int64_t offset = in.timestampsQpc[index] - in.targetQpc;
        if (!in.readyNow[index] || offset > in.leadToleranceQpc || offset < -in.lagBoundQpc)
            return detail::kCfrHorizonInfiniteCost;
        const int64_t show = static_cast<int64_t>(index) * in.slotQpc + detail::CfrHorizonDistance(offset, 0);
        const int64_t rest = detail::CfrHorizonPlanCost(in, count, static_cast<int>(index), 1, horizon);
        return rest >= detail::kCfrHorizonInfiniteCost ? rest : show + rest;
    };

    int64_t bestCost = firstCost(false, 0);
    bool bestEmit = false;
    size_t bestIndex = 0;
    for (size_t i = 0; i < count; ++i) {
        if (in.timestampsQpc[i] - in.targetQpc > in.leadToleranceQpc)
            break;
        const int64_t cost = firstCost(true, i);
        if (cost < bestCost) {
            bestCost = cost;
            bestEmit = true;
            bestIndex = i;
        }
    }
    const bool nearestComparable = !nearestEmit || nearestIndex < count;
    if (nearestComparable) {
        const int64_t nearestCost = firstCost(nearestEmit, nearestEmit ? nearestIndex : 0);
        if (nearestCost <= bestCost) {
            bestCost = nearestCost;
            bestEmit = nearestEmit;
            bestIndex = nearestEmit ? nearestIndex : 0;
        }
    }
    if (bestCost >= detail::kCfrHorizonInfiniteCost)
        return decision;
    decision.valid = true;
    decision.emit = bestEmit;
    decision.index = bestIndex;
    decision.costQpc = bestCost;
    return decision;
}

}  // namespace ce::capture_policy
