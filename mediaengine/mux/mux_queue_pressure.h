#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

// Early warning and attribution for the encoded-packet queue between the
// encoder and the async mux writer.
//
// A recording never drops encoded packets: at the queue limit the encoder is
// held back (backpressure), which shows up as capture stutter. Before that
// point the queue only grows silently, so a slow output target (a network
// share that stalls, a saturated disk) was visible only as a periodic INFO
// byte count. The tracker reports each higher fill band once per episode and
// the recovery once, so a long stall costs at most a handful of lines.

namespace ce::mux {

inline constexpr uint32_t kMuxQueuePressureBandsPermille[] = {250, 500, 750};
// Below this fill the episode is over; half the lowest band, so a queue
// hovering around 25% does not flap between raised and recovered.
inline constexpr uint32_t kMuxQueuePressureRecoverPermille = 125;

// How often the rate window restarts while no pressure episode is active.
inline constexpr uint64_t kMuxPressureBaselineRefreshMs = 5000;

enum class MuxQueuePressureEvent : uint8_t {
    kNone = 0,
    kRaised,
    kRecovered,
};

struct MuxQueuePressureUpdate {
    MuxQueuePressureEvent event = MuxQueuePressureEvent::kNone;
    uint32_t fillPermille = 0;
    // Highest band reached in the episode (0 when no episode is active).
    uint32_t bandPermille = 0;
    uint64_t episodeDurationMs = 0;
    size_t episodePeakBytes = 0;
};

inline uint32_t ComputeMuxQueueFillPermille(size_t queuedBytes, size_t limitBytes) {
    if (limitBytes == 0) {
        return queuedBytes > 0 ? 1000u : 0u;
    }
    const uint64_t permille = static_cast<uint64_t>(queuedBytes) * 1000u / static_cast<uint64_t>(limitBytes);
    return static_cast<uint32_t>(std::min<uint64_t>(permille, 1000u));
}

inline uint32_t SelectMuxQueuePressureBandPermille(uint32_t fillPermille) {
    uint32_t band = 0;
    for (const uint32_t threshold : kMuxQueuePressureBandsPermille) {
        if (fillPermille >= threshold) {
            band = threshold;
        }
    }
    return band;
}

class MuxQueuePressureTracker {
public:
    MuxQueuePressureUpdate Observe(size_t queuedBytes, size_t limitBytes, uint64_t nowMs) {
        MuxQueuePressureUpdate update;
        update.fillPermille = ComputeMuxQueueFillPermille(queuedBytes, limitBytes);
        const uint32_t band = SelectMuxQueuePressureBandPermille(update.fillPermille);

        if (episodeBandPermille_ == 0) {
            if (band == 0) {
                return update;
            }
            episodeStartMs_ = nowMs;
            episodePeakBytes_ = 0;
        }
        episodePeakBytes_ = std::max(episodePeakBytes_, queuedBytes);
        update.episodeDurationMs = nowMs >= episodeStartMs_ ? nowMs - episodeStartMs_ : 0;
        update.episodePeakBytes = episodePeakBytes_;

        if (band > episodeBandPermille_) {
            episodeBandPermille_ = band;
            update.event = MuxQueuePressureEvent::kRaised;
        } else if (update.fillPermille < kMuxQueuePressureRecoverPermille) {
            update.event = MuxQueuePressureEvent::kRecovered;
            update.bandPermille = episodeBandPermille_;
            episodeBandPermille_ = 0;
            return update;
        }
        update.bandPermille = episodeBandPermille_;
        return update;
    }

    void Reset() {
        *this = MuxQueuePressureTracker{};
    }

    bool EpisodeActive() const {
        return episodeBandPermille_ != 0;
    }

private:
    uint32_t episodeBandPermille_ = 0;
    uint64_t episodeStartMs_ = 0;
    size_t episodePeakBytes_ = 0;
};

// Writer-side rates over one reporting window. `writtenBytes` and `busyUs`
// cover the time the writer spent inside the muxer's write call. A writer that
// is busy almost the whole window while its rate stays below the encoder's is
// waiting on the output target; a mostly idle writer points elsewhere (the
// writer thread not being scheduled, or a lock).
struct MuxWriterWindowRates {
    uint64_t writerBytesPerSecond = 0;
    uint64_t encoderBytesPerSecond = 0;
    uint32_t busyPermille = 0;
};

inline MuxWriterWindowRates ComputeMuxWriterWindowRates(uint64_t writtenBytes, uint64_t enqueuedBytes, uint64_t busyUs,
                                                        uint64_t windowUs) {
    MuxWriterWindowRates rates;
    if (windowUs == 0) {
        return rates;
    }
    rates.writerBytesPerSecond =
        static_cast<uint64_t>(static_cast<double>(writtenBytes) * 1000000.0 / static_cast<double>(windowUs));
    rates.encoderBytesPerSecond =
        static_cast<uint64_t>(static_cast<double>(enqueuedBytes) * 1000000.0 / static_cast<double>(windowUs));
    rates.busyPermille = static_cast<uint32_t>(std::min<uint64_t>(busyUs * 1000u / windowUs, 1000u));
    return rates;
}

// One mux write that blocks this long already queues a noticeable amount of
// encoded output; report it, rate-limited, with the count of suppressed ones.
inline constexpr uint64_t kSlowMuxWriteThresholdUs = 250000;
inline constexpr uint64_t kSlowMuxWriteLogIntervalMs = 5000;

inline bool IsSlowMuxWrite(uint64_t writeUs) {
    return writeUs >= kSlowMuxWriteThresholdUs;
}

inline bool ShouldLogSlowMuxWrite(uint64_t nowMs, uint64_t lastLogMs) {
    return lastLogMs == 0 || nowMs < lastLogMs || nowMs - lastLogMs >= kSlowMuxWriteLogIntervalMs;
}

}  // namespace ce::mux
