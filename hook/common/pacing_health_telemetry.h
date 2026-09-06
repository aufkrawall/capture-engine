#pragma once

#include <cstdint>

namespace ce::pacing_health {

// Frame-cadence health rings for the frame-generation pacing investigation.
//
// The start-to-start FSR FG pacing degradation shows up as a wide physical-
// completion distribution (reproductions have ranged from ~12% to 38% late),
// which a rolling average alone cannot classify. These rings retain the raw
// recent intervals per cadence channel so one rate-limited log line can publish
// median/p95/stddev/late-share directly, and so FG activation can record the
// cadence the runtime is about to pace against.

enum class Channel : uint32_t {
    kPresentation = 0,  // hooked Present entry cadence (presentation series)
    kDisplay = 1,       // displayed-transition cadence (display-change series)
    kPresentToDisplay = 2,  // correlated PresentStart-to-screen duration
    kCount = 3,
};

// Feed one interval/value in microseconds. `observationTimeUs` makes periodic
// snapshots exact wall-clock windows instead of whatever happens to remain in
// the ring. A non-zero tag marks a sample as belonging to an active FSR segment;
// zero remains useful to activation-cadence snapshots but is excluded from the
// FSR health line. Each channel has a single writer.
void Observe(Channel channel, int64_t valueUs, int64_t observationTimeUs = 0, uint64_t tag = 0);

struct ChannelStats {
    uint32_t samples = 0;
    uint32_t medianUs = 0;
    uint32_t p95Us = 0;
    uint32_t meanUs = 0;
    uint32_t stddevUs = 0;
    uint32_t minUs = 0;
    uint32_t maxUs = 0;
    uint32_t latePermille = 0;  // share of samples above median + kLateFlipThresholdUs
    uint32_t segments = 0;      // distinct non-zero tags in a time-window snapshot
};

enum class Signature : uint32_t {
    kInsufficient = 0,
    kHealthy,
    kDownstreamJitter,
    kMixedJitter,
};

// Intervals past the median by more than this count as late flips. Matches the
// measured bad-start signature (good-session tails stay below the classifier's
// independently calibrated degraded threshold).
constexpr uint32_t kLateFlipThresholdUs = 1200;

// Pure aggregation over a sample span. Order-independent; exposed for tests.
ChannelStats ComputeChannelStats(const uint32_t* samples, uint32_t count);

// Diagnostic classifier for the measured Talos failure shape. It never changes
// rendering or pacing policy: "downstream" means Present entry was smooth while
// physical display transitions were not, not that a particular driver stage has
// already been proven responsible.
Signature Classify(const ChannelStats& display, const ChannelStats& presentation);
const char* SignatureName(Signature signature);

// Aggregates the whole retained window of a channel.
ChannelStats Snapshot(Channel channel);

// Aggregates tagged samples observed in (startTimeUs, endTimeUs]. This is the
// periodic FSR health primitive: stale startup samples and non-FSR intervals
// are excluded even when FSR briefly toggles off/on inside one log window.
ChannelStats SnapshotTaggedWindow(Channel channel, int64_t startTimeUs, int64_t endTimeUs);

// Median/sample count over the most recent `maxSamples` intervals of a channel
// (clamped to the retained window). Used at FG activation.
ChannelStats RecentCadence(Channel channel, uint32_t maxSamples);

}  // namespace ce::pacing_health
