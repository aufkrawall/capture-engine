#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

// How the front-loaded release budget treats a stutter spike. The cadence
// itself never needs to recover from one (a late frame re-bases the deadline
// with a whole interval, capture sync skips whole grid slots); the budget did:
// the work ceiling was the maximum of the last 64 frames, so one spike shorter
// than an interval held the release early for 64 frames, and one deadline it
// overran grew the overrun reservation, which decays by an eighth per clean
// 64-frame window - about ten seconds for a 4 ms spike at 120 fps. Both cost
// latency only, never frame pacing, but a spike that does not recur should
// cost nothing once it is over.
namespace ce::fps_limiter_policy {

// The work ceiling ignores the largest kFrameWorkSpikeRank - 1 samples of the
// window: up to two isolated spikes per 64 frames leave the budget untouched,
// a third makes the pattern part of the ceiling.
inline constexpr size_t kFrameWorkSpikeRank = 3;

template <size_t Capacity>
inline int64_t ResolveSpikeTolerantWorkCeilingUs(const std::array<int64_t, Capacity>& samples, size_t count) {
    count = (std::min)(count, Capacity);
    if (count == 0) {
        return 0;
    }
    std::array<int64_t, Capacity> sorted = samples;
    const size_t rank = (std::min)(kFrameWorkSpikeRank, count);
    std::nth_element(sorted.begin(), sorted.begin() + (rank - 1), sorted.begin() + count, std::greater<int64_t>());
    return sorted[rank - 1];
}

// A deadline overrun only grows the reservation when another one happened
// within the last `windowFrames` released frames: an isolated overrun is a
// spike, recurring ones are a budget that is too small - the evidence the
// reservation exists for (Strange Brigade DX12: 0.24% -> 0.63% late frames
// under front-loading, 1% low 86.9 -> 84.9 fps). The reservation then grows to
// the larger of the two. Lateness of a whole interval or more is a hitch and
// never counts. `frame` is a monotonic count of released frames.
struct OverrunRecurrence {
    bool havePrevious = false;
    uint32_t previousFrame = 0;
    int64_t previousLateUs = 0;
};

inline int64_t ResolveRecurringOverrunUs(OverrunRecurrence& state, uint32_t frame, int64_t lateUs,
                                         int64_t intervalUs, uint32_t windowFrames) {
    if (lateUs <= 0 || intervalUs <= 0 || lateUs >= intervalUs) {
        return 0;
    }
    const bool recurring = state.havePrevious && frame - state.previousFrame <= windowFrames;
    const int64_t grownToUs = recurring ? (std::max)(lateUs, state.previousLateUs) : 0;
    state.havePrevious = true;
    state.previousFrame = frame;
    state.previousLateUs = lateUs;
    return grownToUs;
}

}  // namespace ce::fps_limiter_policy
