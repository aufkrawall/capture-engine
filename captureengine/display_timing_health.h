#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../common/logging.h"
#include "display_timing_intervals.h"

// Which kernel event carried a frame's screen time. Reported per window because
// only the immediate flip path can take NVIDIA's scheduled-flip announcement,
// so the split is what says whether that correction reaches the published
// series at all on a given machine and present mode. VSync and HSync multiplane
// DPCs are counted apart: the HSync variant is the hardware flip queue, which
// is what a variable-refresh path uses, and the two do not necessarily carry a
// screen time of the same quality.
enum class DisplayCompletionSource : std::size_t {
    VSyncDpc = 0,
    VSyncDpcMultiPlane = 1,
    HSyncDpcMultiPlane = 2,
    ImmediateFlip = 3,
    ImmediateMultiPlaneFlip = 4,
    Count = 5,
};

// One window's counters, snapshotted under the lock and formatted outside it.
struct DisplayTimingHealth {
    uint64_t presents = 0;
    uint64_t associations = 0;
    uint64_t queued = 0;
    uint64_t published = 0;
    uint64_t suppressed = 0;
    uint64_t regressed = 0;
    uint64_t payloadReceived = 0;
    uint64_t payloadValid = 0;
    uint64_t payloadCorrelated = 0;
    uint64_t payloadPending = 0;
    uint64_t payloadPendingObserved = 0;
    uint64_t authoritative = 0;
    uint64_t payloadDuplicate = 0;
    uint64_t payloadLate = 0;
    uint64_t fallbackPublished = 0;
    uint64_t fallbackSuppressed = 0;
    uint64_t nvReceived = 0;
    uint64_t nvUndecodable = 0;
    uint64_t nvApplied = 0;
    int64_t nvAverageDelayUs = 0;
    int64_t nvMaxDelayUs = 0;
    // Payload offset the announcement was located at, or -1 while unlocated.
    int32_t nvFieldOffset = -1;
    bool nvFieldAbandoned = false;
    std::array<uint64_t, static_cast<std::size_t>(DisplayCompletionSource::Count)> completions = {};
    // Shape of the two series over this window alone, in microseconds. The
    // published one is what the overlay draws; the runtime one is the same
    // frames measured at Present, so a jagged published series next to a flat
    // runtime series localizes the fault in this service rather than the game.
    uint64_t publishedIntervalCount = 0;
    int64_t publishedIntervalMeanUs = 0;
    int64_t publishedIntervalStdDevUs = 0;
    int64_t publishedIntervalJaggednessUs = 0;
    int64_t publishedIntervalP1Us = 0;
    int64_t publishedIntervalP50Us = 0;
    int64_t publishedIntervalP99Us = 0;
    int64_t publishedIntervalMaxUs = 0;
    uint64_t runtimeIntervalCount = 0;
    int64_t runtimeIntervalMeanUs = 0;
    int64_t runtimeIntervalStdDevUs = 0;
    int64_t runtimeIntervalJaggednessUs = 0;
    // The screen's own clock and how often a deferred completion had to be
    // rounded onto it. `unresolved` counts completions the clock could not
    // answer for, which keep the uncorrected timestamp.
    uint64_t blanksObserved = 0;
    int64_t blankIntervalUs = 0;
    uint64_t blankAdjusted = 0;
    uint64_t blankUnresolved = 0;
};

inline void SetPublishedIntervals(DisplayTimingHealth& health, const DisplayIntervalStats& stats) {
    health.publishedIntervalCount = stats.count();
    health.publishedIntervalMeanUs = stats.meanUs();
    health.publishedIntervalStdDevUs = stats.stdDevUs();
    health.publishedIntervalJaggednessUs = stats.jaggednessUs();
    health.publishedIntervalP1Us = stats.percentileUs(0.01);
    health.publishedIntervalP50Us = stats.percentileUs(0.50);
    health.publishedIntervalP99Us = stats.percentileUs(0.99);
    health.publishedIntervalMaxUs = stats.maxUs();
}

inline void SetRuntimeIntervals(DisplayTimingHealth& health, const DisplayIntervalStats& stats) {
    health.runtimeIntervalCount = stats.count();
    health.runtimeIntervalMeanUs = stats.meanUs();
    health.runtimeIntervalStdDevUs = stats.stdDevUs();
    health.runtimeIntervalJaggednessUs = stats.jaggednessUs();
}

// Formatting lives here so the service translation unit stays inside the source
// size ceiling; the health snapshot is the only input.
inline void LogDisplayTimingHealth(const DisplayTimingHealth& health) {
    // A stalled window and a healthy one report the same fields so the two stay
    // directly comparable; only the level and the prefix differ.
    const bool stalled = health.published == 0;
    if (!stalled && !Log_IsEnabled(LogLevel::Debug))
        return;
    Log(stalled ? LogLevel::Warn : LogLevel::Debug,
        "[DisplayTiming]%s runtimePresents=%llu submitAssociations=%llu queued=%llu published=%llu "
        "suppressed=%llu regressed=%llu frameType(received=%llu valid=%llu matched=%llu pendingCurrent=%llu "
        "pendingObserved=%llu authoritativeQueued=%llu duplicate=%llu late=%llu) "
        "fallback(committed=%llu suppressed=%llu) "
        "completion(vsyncDpc=%llu vsyncDpcMpo=%llu hsyncDpcMpo=%llu immediateFlip=%llu immediateMpoFlip=%llu) "
        "nvFlipSchedule(received=%llu undecodable=%llu applied=%llu avgDelayUs=%lld maxDelayUs=%lld "
        "fieldOffset=%d abandoned=%d) "
        "vblank(observed=%llu intervalUs=%lld adjusted=%llu unresolved=%llu) "
        "publishedInterval(n=%llu meanUs=%lld stddevUs=%lld jaggednessUs=%lld p1Us=%lld p50Us=%lld p99Us=%lld "
        "maxUs=%lld) runtimeInterval(n=%llu meanUs=%lld stddevUs=%lld jaggednessUs=%lld)",
        stalled ? " no screen-change timestamp published yet:" : "", health.presents, health.associations,
        health.queued, health.published, health.suppressed, health.regressed, health.payloadReceived,
        health.payloadValid, health.payloadCorrelated, health.payloadPending, health.payloadPendingObserved,
        health.authoritative, health.payloadDuplicate, health.payloadLate, health.fallbackPublished,
        health.fallbackSuppressed, health.completions[0], health.completions[1], health.completions[2],
        health.completions[3], health.completions[4], health.nvReceived, health.nvUndecodable, health.nvApplied,
        static_cast<long long>(health.nvAverageDelayUs), static_cast<long long>(health.nvMaxDelayUs),
        health.nvFieldOffset, health.nvFieldAbandoned ? 1 : 0, health.blanksObserved,
        static_cast<long long>(health.blankIntervalUs), health.blankAdjusted, health.blankUnresolved,
        health.publishedIntervalCount,
        static_cast<long long>(health.publishedIntervalMeanUs),
        static_cast<long long>(health.publishedIntervalStdDevUs),
        static_cast<long long>(health.publishedIntervalJaggednessUs),
        static_cast<long long>(health.publishedIntervalP1Us),
        static_cast<long long>(health.publishedIntervalP50Us),
        static_cast<long long>(health.publishedIntervalP99Us),
        static_cast<long long>(health.publishedIntervalMaxUs), health.runtimeIntervalCount,
        static_cast<long long>(health.runtimeIntervalMeanUs),
        static_cast<long long>(health.runtimeIntervalStdDevUs),
        static_cast<long long>(health.runtimeIntervalJaggednessUs));
}
