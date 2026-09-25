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
    // runtime series reveals a difference between submission and displayed cadence;
    // it does not by itself attribute a fault to the collector or the game.
    uint64_t publishedIntervalCount = 0;
    int64_t publishedIntervalMeanUs = 0;
    int64_t publishedIntervalStdDevUs = 0;
    int64_t publishedIntervalJaggednessUs = 0;
    int64_t publishedIntervalP1Us = 0;
    int64_t publishedIntervalP50Us = 0;
    int64_t publishedIntervalP99Us = 0;
    int64_t publishedIntervalMaxUs = 0;
    // What the overlay graph draws: the published series after the refresh
    // bound (display_timing_refresh_bound.h). Equal to the published series
    // unless refreshBound(bounded=...) is non-zero.
    uint64_t graphIntervalCount = 0;
    int64_t graphIntervalMeanUs = 0;
    int64_t graphIntervalStdDevUs = 0;
    int64_t graphIntervalJaggednessUs = 0;
    int64_t graphIntervalP1Us = 0;
    int64_t graphIntervalP50Us = 0;
    int64_t graphIntervalP99Us = 0;
    int64_t graphIntervalMaxUs = 0;
    // Refresh bound over this window on the busiest output: the display's
    // minimum period, transitions it could apply to, transitions it moved later
    // and by how much, and impossible intervals it left alone for want of an
    // observed blank.
    int64_t refreshPeriodUs = 0;
    uint64_t refreshBoundEligible = 0;
    uint64_t refreshBoundApplied = 0;
    uint64_t refreshBoundBlankMissing = 0;
    int64_t refreshBoundShiftMeanUs = 0;
    int64_t refreshBoundShiftMaxUs = 0;
    uint64_t runtimeIntervalCount = 0;
    int64_t runtimeIntervalMeanUs = 0;
    int64_t runtimeIntervalStdDevUs = 0;
    int64_t runtimeIntervalJaggednessUs = 0;
    // Correlated time from the runtime's PresentStart to physical display.
    // A smooth runtime interval next to a jagged duration isolates the fault
    // downstream of Present entry without guessing whether it is queue
    // completion, driver flip scheduling, or scanout.
    uint64_t presentToDisplayCount = 0;
    int64_t presentToDisplayMeanUs = 0;
    int64_t presentToDisplayStdDevUs = 0;
    int64_t presentToDisplayMinUs = 0;
    int64_t presentToDisplayP50Us = 0;
    int64_t presentToDisplayP95Us = 0;
    int64_t presentToDisplayP99Us = 0;
    int64_t presentToDisplayMaxUs = 0;
    // Blank cadence is diagnostic only; no display timestamp is quantized.
    uint64_t blanksObserved = 0;
    int64_t blankIntervalUs = 0;
    bool blankClockPeriodic = false;
    // The shape of the blank stream itself. The clock can only be as good as
    // this: gaps that are not multiples of one period are what the grid refuses.
    uint64_t blankIntervalCount = 0;
    int64_t blankIntervalMeanUs = 0;
    int64_t blankIntervalP50Us = 0;
    int64_t blankIntervalP99Us = 0;
    int64_t blankIntervalMaxUs = 0;
    // The completions as the driver timestamped them, before any rounding. Read
    // against publishedInterval it says what the rounding did: the two are equal
    // while the clock is refusing, and the published one should be the flatter
    // of the two whenever it is not.
    uint64_t latchIntervalCount = 0;
    int64_t latchIntervalMeanUs = 0;
    int64_t latchIntervalStdDevUs = 0;
    int64_t latchIntervalJaggednessUs = 0;
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

inline void SetGraphIntervals(DisplayTimingHealth& health, const DisplayIntervalStats& stats) {
    health.graphIntervalCount = stats.count();
    health.graphIntervalMeanUs = stats.meanUs();
    health.graphIntervalStdDevUs = stats.stdDevUs();
    health.graphIntervalJaggednessUs = stats.jaggednessUs();
    health.graphIntervalP1Us = stats.percentileUs(0.01);
    health.graphIntervalP50Us = stats.percentileUs(0.50);
    health.graphIntervalP99Us = stats.percentileUs(0.99);
    health.graphIntervalMaxUs = stats.maxUs();
}

inline void SetLatchIntervals(DisplayTimingHealth& health, const DisplayIntervalStats& stats) {
    health.latchIntervalCount = stats.count();
    health.latchIntervalMeanUs = stats.meanUs();
    health.latchIntervalStdDevUs = stats.stdDevUs();
    health.latchIntervalJaggednessUs = stats.jaggednessUs();
}

inline void SetBlankIntervals(DisplayTimingHealth& health, const DisplayIntervalStats& stats) {
    health.blankIntervalCount = stats.count();
    health.blankIntervalMeanUs = stats.meanUs();
    health.blankIntervalP50Us = stats.percentileUs(0.50);
    health.blankIntervalP99Us = stats.percentileUs(0.99);
    health.blankIntervalMaxUs = stats.maxUs();
}

inline void SetRuntimeIntervals(DisplayTimingHealth& health, const DisplayIntervalStats& stats) {
    health.runtimeIntervalCount = stats.count();
    health.runtimeIntervalMeanUs = stats.meanUs();
    health.runtimeIntervalStdDevUs = stats.stdDevUs();
    health.runtimeIntervalJaggednessUs = stats.jaggednessUs();
}

inline void SetPresentToDisplay(DisplayTimingHealth& health, const DisplayDurationStats& stats) {
    health.presentToDisplayCount = stats.count();
    health.presentToDisplayMeanUs = stats.meanUs();
    health.presentToDisplayStdDevUs = stats.stdDevUs();
    health.presentToDisplayMinUs = stats.minUs();
    health.presentToDisplayP50Us = stats.percentileUs(0.50);
    health.presentToDisplayP95Us = stats.percentileUs(0.95);
    health.presentToDisplayP99Us = stats.percentileUs(0.99);
    health.presentToDisplayMaxUs = stats.maxUs();
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
        "vblank(observed=%llu periodUs=%lld periodic=%d timestampPolicy=event "
        "gaps(n=%llu meanUs=%lld p50Us=%lld p99Us=%lld maxUs=%lld)) "
        "latchInterval(n=%llu meanUs=%lld stddevUs=%lld jaggednessUs=%lld) "
        "publishedInterval(n=%llu meanUs=%lld stddevUs=%lld jaggednessUs=%lld p1Us=%lld p50Us=%lld p99Us=%lld "
        "maxUs=%lld) graphInterval(n=%llu meanUs=%lld stddevUs=%lld jaggednessUs=%lld p1Us=%lld p50Us=%lld "
        "p99Us=%lld maxUs=%lld) refreshBound(periodUs=%lld eligible=%llu bounded=%llu noBlank=%llu "
        "meanShiftUs=%lld maxShiftUs=%lld) runtimeInterval(n=%llu meanUs=%lld stddevUs=%lld jaggednessUs=%lld) "
        "presentToDisplay(n=%llu meanUs=%lld stddevUs=%lld minUs=%lld p50Us=%lld p95Us=%lld p99Us=%lld "
        "maxUs=%lld)",
        stalled ? " no screen-change timestamp published yet:" : "", health.presents, health.associations,
        health.queued, health.published, health.suppressed, health.regressed, health.payloadReceived,
        health.payloadValid, health.payloadCorrelated, health.payloadPending, health.payloadPendingObserved,
        health.authoritative, health.payloadDuplicate, health.payloadLate, health.fallbackPublished,
        health.fallbackSuppressed, health.completions[0], health.completions[1], health.completions[2],
        health.completions[3], health.completions[4], health.nvReceived, health.nvUndecodable, health.nvApplied,
        static_cast<long long>(health.nvAverageDelayUs), static_cast<long long>(health.nvMaxDelayUs),
        health.nvFieldOffset, health.nvFieldAbandoned ? 1 : 0, health.blanksObserved,
        static_cast<long long>(health.blankIntervalUs), health.blankClockPeriodic ? 1 : 0, health.blankIntervalCount, static_cast<long long>(health.blankIntervalMeanUs),
        static_cast<long long>(health.blankIntervalP50Us), static_cast<long long>(health.blankIntervalP99Us),
        static_cast<long long>(health.blankIntervalMaxUs), health.latchIntervalCount,
        static_cast<long long>(health.latchIntervalMeanUs), static_cast<long long>(health.latchIntervalStdDevUs),
        static_cast<long long>(health.latchIntervalJaggednessUs), health.publishedIntervalCount,
        static_cast<long long>(health.publishedIntervalMeanUs),
        static_cast<long long>(health.publishedIntervalStdDevUs),
        static_cast<long long>(health.publishedIntervalJaggednessUs),
        static_cast<long long>(health.publishedIntervalP1Us),
        static_cast<long long>(health.publishedIntervalP50Us),
        static_cast<long long>(health.publishedIntervalP99Us),
        static_cast<long long>(health.publishedIntervalMaxUs), health.graphIntervalCount,
        static_cast<long long>(health.graphIntervalMeanUs), static_cast<long long>(health.graphIntervalStdDevUs),
        static_cast<long long>(health.graphIntervalJaggednessUs), static_cast<long long>(health.graphIntervalP1Us),
        static_cast<long long>(health.graphIntervalP50Us), static_cast<long long>(health.graphIntervalP99Us),
        static_cast<long long>(health.graphIntervalMaxUs), static_cast<long long>(health.refreshPeriodUs),
        health.refreshBoundEligible, health.refreshBoundApplied, health.refreshBoundBlankMissing,
        static_cast<long long>(health.refreshBoundShiftMeanUs), static_cast<long long>(health.refreshBoundShiftMaxUs),
        health.runtimeIntervalCount,
        static_cast<long long>(health.runtimeIntervalMeanUs),
        static_cast<long long>(health.runtimeIntervalStdDevUs),
        static_cast<long long>(health.runtimeIntervalJaggednessUs), health.presentToDisplayCount,
        static_cast<long long>(health.presentToDisplayMeanUs),
        static_cast<long long>(health.presentToDisplayStdDevUs),
        static_cast<long long>(health.presentToDisplayMinUs),
        static_cast<long long>(health.presentToDisplayP50Us),
        static_cast<long long>(health.presentToDisplayP95Us),
        static_cast<long long>(health.presentToDisplayP99Us),
        static_cast<long long>(health.presentToDisplayMaxUs));
}
