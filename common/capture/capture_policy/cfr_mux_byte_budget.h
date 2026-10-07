#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "constants.h"

// Byte budget for CFR output when the mux writer, not the encoder, is the limit.
//
// A recording never drops encoded packets: once the mux queue reaches its limit,
// WriteFrame blocks the encoder thread. Every CFR slot that falls due while it is
// blocked becomes timeline debt, and the time-based overload pacer cannot shed it,
// because a cheap cached repeat then waits for the previous fresh frame's bytes to
// drain and measures as slow as a fresh frame. Bytes are the scarce resource, and a
// cached repeat costs almost none of them. This controller measures the writer's
// realized capacity against the enqueue rate and returns the share of output slots
// that may carry fresh pixels so the queue drains instead of blocking. It changes
// only which pixels fill already-scheduled slots: never CFR PTS, the audio
// timeline, or an encoder setting.

namespace ce::capture_policy {

constexpr uint64_t kCfrMuxByteBudgetWindowUs = 250'000;
// A single stalled write credits no busy time until it completes; wait up to this
// long for one to finish before judging the writer stalled.
constexpr uint64_t kCfrMuxByteBudgetMaxWindowUs = 1'000'000;
// The queue absorbs short output stalls. Engage only once a quarter of it holds a
// sustained deficit, aim back below an eighth (the pressure log's own bands).
constexpr uint32_t kCfrMuxByteBudgetEngageFillPermille = 250;
constexpr uint32_t kCfrMuxByteBudgetExitFillPermille = 125;
// Share of the writer's capacity withheld at a full queue, so a backlog drains.
constexpr double kCfrMuxByteBudgetFullQueueDrainShare = 0.5;
// The fresh share may at most double per window: unconstrained demand is not
// observable while capped, so headroom is probed instead of assumed.
constexpr double kCfrMuxByteBudgetMaxGrowthPerWindow = 2.0;
constexpr uint32_t kCfrMuxByteBudgetExitConfirmWindows = 4;

// Cumulative counters; any counter moving backwards (a new recording) rebases.
struct CfrMuxFlowSample {
    uint64_t nowUs = 0;
    uint64_t enqueuedBytes = 0;
    uint64_t writtenBytes = 0;
    uint64_t writerBusyUs = 0;
    uint64_t queuedBytes = 0;
    uint64_t queueLimitBytes = 0;
    uint64_t freshOutputs = 0;
    uint64_t totalOutputs = 0;
};

struct CfrMuxByteBudgetWindow {
    double writerBytesPerSec = 0.0;
    double capacityBytesPerSec = 0.0;
    double enqueueBytesPerSec = 0.0;
    double observedFreshShare = 0.0;
    uint32_t fillPermille = 0;
    uint32_t busyPermille = 0;
    uint64_t windowUs = 0;
};

struct CfrMuxByteBudgetState {
    bool hasBaseline = false;
    CfrMuxFlowSample baseline{};
    bool active = false;
    double freshFractionCap = 1.0;
    double minimumFreshFractionCap = 1.0;
    uint32_t exitConfirmWindows = 0;
    uint64_t episodes = 0;
    uint64_t activeWindows = 0;
    CfrMuxByteBudgetWindow last{};

    void Reset() {
        const uint64_t keptEpisodes = episodes;
        const uint64_t keptActiveWindows = activeWindows;
        const double keptMinimum = minimumFreshFractionCap;
        *this = {};
        episodes = keptEpisodes;
        activeWindows = keptActiveWindows;
        minimumFreshFractionCap = keptMinimum;
    }
};

struct CfrMuxByteBudgetDecision {
    bool sampled = false;
    bool entered = false;
    bool exited = false;
    bool active = false;
    double freshFractionCap = 1.0;
    const char* reason = "idle";
};

inline bool IsCfrMuxFlowRegression(const CfrMuxFlowSample& base, const CfrMuxFlowSample& now) {
    return now.nowUs < base.nowUs || now.enqueuedBytes < base.enqueuedBytes ||
           now.writtenBytes < base.writtenBytes || now.writerBusyUs < base.writerBusyUs ||
           now.freshOutputs < base.freshOutputs || now.totalOutputs < base.totalOutputs;
}

inline bool ShouldSampleCfrMuxFlow(const CfrMuxByteBudgetState& state, uint64_t nowUs) {
    return !state.hasBaseline || nowUs < state.baseline.nowUs ||
           nowUs - state.baseline.nowUs >= kCfrMuxByteBudgetWindowUs;
}

// The fresh-slot share whose enqueue rate fits the target rate, assuming fresh
// frames carry the bytes (repeats and audio are small). A window without any
// fresh frame proves nothing about what one costs, so the cap is kept.
inline double ComputeCfrMuxFreshFractionCap(double currentCap, double observedFreshShare,
                                             double enqueueBytesPerSec, double targetBytesPerSec) {
    const double floor = kWgcOverloadRepeatPacerDegradedFreshFraction;
    if (!std::isfinite(observedFreshShare) || !std::isfinite(enqueueBytesPerSec) ||
        !std::isfinite(targetBytesPerSec) || observedFreshShare <= 0.0 || enqueueBytesPerSec <= 0.0) {
        return std::clamp(currentCap, floor, 1.0);
    }
    const double proposed = observedFreshShare * std::max(targetBytesPerSec, 0.0) / enqueueBytesPerSec;
    const double ceiling = std::min(1.0, std::max(currentCap, floor) * kCfrMuxByteBudgetMaxGrowthPerWindow);
    return std::clamp(proposed, floor, ceiling);
}

inline CfrMuxByteBudgetDecision UpdateCfrMuxByteBudget(CfrMuxByteBudgetState& state,
                                                       const CfrMuxFlowSample& sample, bool liveCfr) {
    CfrMuxByteBudgetDecision decision{};
    const auto finish = [&](const char* reason) {
        decision.active = state.active;
        decision.freshFractionCap = state.active ? state.freshFractionCap : 1.0;
        decision.reason = reason;
        return decision;
    };
    if (!liveCfr || sample.queueLimitBytes == 0) {
        decision.exited = state.active;
        state.Reset();
        return finish(liveCfr ? "no_queue_limit" : "not_live_cfr");
    }
    if (!state.hasBaseline || IsCfrMuxFlowRegression(state.baseline, sample)) {
        decision.exited = state.active;
        state.Reset();
        state.hasBaseline = true;
        state.baseline = sample;
        return finish("baseline");
    }
    const CfrMuxFlowSample& base = state.baseline;
    const uint64_t windowUs = sample.nowUs - base.nowUs;
    const uint64_t busyUs = sample.writerBusyUs - base.writerBusyUs;
    const bool backlogHeld = base.queuedBytes > 0 && sample.queuedBytes > 0;
    if (windowUs < kCfrMuxByteBudgetWindowUs ||
        (busyUs == 0 && backlogHeld && windowUs < kCfrMuxByteBudgetMaxWindowUs)) {
        return finish("collecting");
    }

    const double seconds = static_cast<double>(windowUs) / 1e6;
    const uint64_t freshOutputs = sample.freshOutputs - base.freshOutputs;
    const uint64_t totalOutputs = sample.totalOutputs - base.totalOutputs;
    CfrMuxByteBudgetWindow window{};
    window.windowUs = windowUs;
    window.writerBytesPerSec = static_cast<double>(sample.writtenBytes - base.writtenBytes) / seconds;
    window.enqueueBytesPerSec = static_cast<double>(sample.enqueuedBytes - base.enqueuedBytes) / seconds;
    window.observedFreshShare =
        totalOutputs > 0 ? static_cast<double>(freshOutputs) / static_cast<double>(totalOutputs) : 0.0;
    window.fillPermille = static_cast<uint32_t>(
        std::min<uint64_t>(1000u, sample.queuedBytes * 1000u / sample.queueLimitBytes));
    window.busyPermille = static_cast<uint32_t>(std::min<uint64_t>(1000u, busyUs * 1000u / windowUs));
    // With a backlog the realized drain rate is the capacity, whatever slowed it
    // (output target or a starved writer thread), and a writer that completed no
    // write under a backlog has none. Without a backlog the writer idled, and its
    // bytes per busy second show the headroom; with no work at all nothing limits.
    const bool backlog = backlogHeld && window.fillPermille >= kCfrMuxByteBudgetExitFillPermille;
    if (backlog || (backlogHeld && busyUs == 0)) {
        window.capacityBytesPerSec = window.writerBytesPerSec;
    } else if (busyUs > 0) {
        window.capacityBytesPerSec =
            static_cast<double>(sample.writtenBytes - base.writtenBytes) / (static_cast<double>(busyUs) / 1e6);
    } else {
        window.capacityBytesPerSec = window.enqueueBytesPerSec;
    }
    state.last = window;
    state.baseline = sample;
    decision.sampled = true;

    const double excessFill =
        std::clamp((static_cast<double>(window.fillPermille) - kCfrMuxByteBudgetExitFillPermille) /
                       (1000.0 - kCfrMuxByteBudgetExitFillPermille),
                   0.0, 1.0);
    const double targetBytesPerSec =
        window.capacityBytesPerSec * (1.0 - kCfrMuxByteBudgetFullQueueDrainShare * excessFill);

    if (!state.active) {
        const bool deficit = window.enqueueBytesPerSec > window.capacityBytesPerSec;
        if (window.fillPermille < kCfrMuxByteBudgetEngageFillPermille || !deficit) {
            return finish("writer_keeping_up");
        }
        state.active = true;
        state.freshFractionCap = 1.0;
        state.exitConfirmWindows = 0;
        ++state.episodes;
        decision.entered = true;
    }
    ++state.activeWindows;
    const double cap = ComputeCfrMuxFreshFractionCap(state.freshFractionCap, window.observedFreshShare,
                                                     window.enqueueBytesPerSec, targetBytesPerSec);
    state.freshFractionCap = cap;
    state.minimumFreshFractionCap = std::min(state.minimumFreshFractionCap, cap);
    if (cap >= 1.0 && window.fillPermille <= kCfrMuxByteBudgetExitFillPermille) {
        if (++state.exitConfirmWindows >= kCfrMuxByteBudgetExitConfirmWindows) {
            state.active = false;
            state.freshFractionCap = 1.0;
            state.exitConfirmWindows = 0;
            decision.exited = true;
            return finish("writer_recovered");
        }
    } else {
        state.exitConfirmWindows = 0;
    }
    return finish(totalOutputs == 0 ? "no_video_output" : "pacing");
}

}  // namespace ce::capture_policy
