#pragma once

#include <cstdint>

namespace ce::pacing_health {

// Frame-cadence health rings for the frame-generation pacing investigation.
//
// The start-to-start FSR FG pacing degradation shows up as a subset of output
// intervals landing ~1.2-2 ms past the median (bad sessions ~12%, good ~1%),
// which a rolling stddev alone cannot classify. These rings retain the raw
// recent intervals per cadence channel so one rate-limited log line can publish
// median/p95/stddev/late-share directly, and so FG activation can record the
// cadence the runtime is about to pace against.

enum class Channel : uint32_t {
    kPresentation = 0,  // hooked Present entry cadence (presentation series)
    kDisplay = 1,       // displayed-transition cadence (display-change series)
    kCount = 2,
};

// Feed one frame-to-frame interval in microseconds. Each channel has a single
// writer (the presentation and display consumption mutexes); readers may race
// the writer by design and tolerate a mixed-generation sample.
void Observe(Channel channel, int64_t intervalUs);

struct ChannelStats {
    uint32_t samples = 0;
    uint32_t medianUs = 0;
    uint32_t p95Us = 0;
    uint32_t stddevUs = 0;
    uint32_t maxUs = 0;
    uint32_t latePermille = 0;  // share of samples above median + kLateFlipThresholdUs
};

// Intervals past the median by more than this count as late flips. Matches the
// measured bad-start signature (good sessions ~1%, bad ~12%).
constexpr uint32_t kLateFlipThresholdUs = 1200;

// Pure aggregation over a sample span. Order-independent; exposed for tests.
ChannelStats ComputeChannelStats(const uint32_t* samples, uint32_t count);

// Aggregates the whole retained window of a channel.
ChannelStats Snapshot(Channel channel);

// Median/sample count over the most recent `maxSamples` intervals of a channel
// (clamped to the retained window). Used at FG activation.
ChannelStats RecentCadence(Channel channel, uint32_t maxSamples);

}  // namespace ce::pacing_health
