#pragma once

#include <cstdint>

// When a display episode deserves its raw trace (common/platform/display_flight_recorder.h).
//
// The submission-expiry monitor (display_timing_submissions.h) starts an episode when a tracked process's presents
// stop completing as flips of their own and ends it when they do again. Most episodes are the compositor showing the
// process (windowed, an overlay above it): its frames still reach the screen. The ones worth a trace are those where
// the screen stood still while the game presented (GTA V Enhanced, 20261110_222057: 37 s, 4925 successful presents,
// 3 composed frames at the very end).
//
// The silence is measured from the first present that expired unshown until the compositor shows one of the process's
// frames again (or its own flips resume). The monitor notices an episode only once that present is older than the
// completion bound (~1 s), so its own episode clock misses most of a short stall: 20261111_013758 stood still for
// 1.71 s, the monitor counted 531 ms and no trace was sealed. The recovery frames end the silence; they do not
// count against it.
namespace display_stall_trace {

inline constexpr uint64_t kMinimumSilenceMs = 1'500;
inline constexpr uint64_t kOngoingSealMs = 10'000;
inline constexpr uint32_t kMaximumSealsPerSession = 6;

enum class SealReason : uint8_t { kNone, kOngoing, kEnded };

inline const char* SealReasonName(SealReason reason) {
    switch (reason) {
        case SealReason::kOngoing:
            return "ongoing";
        case SealReason::kEnded:
            return "ended";
        case SealReason::kNone:
        default:
            return "none";
    }
}

struct SealRequest {
    SealReason reason = SealReason::kNone;
    uint32_t episode = 0;
    uint32_t processId = 0;
    uint64_t silenceMs = 0;
    int64_t silenceStartUs = 0;  // QPC microseconds of the first present that never reached the screen
    uint64_t shown = 0;          // the compositor's frames of the process during the episode
    explicit operator bool() const noexcept { return reason != SealReason::kNone; }
};

// One prune of the submission tracker, as the display timing service sees it.
struct Prune {
    bool started = false;   // the expiry monitor started an episode in this prune
    bool stopped = false;   // ... or ended it
    bool expiring = false;  // an episode is running after this prune
    int64_t nowUs = 0;      // QPC microseconds of the prune
    // On a start: QPC microseconds of the oldest present that expired in this prune; 0 if unknown.
    int64_t firstUnshownUs = 0;
    uint64_t shownTotal = 0;  // cumulative composed frames of the followed process
    uint32_t processId = 0;
};

class Policy {
public:
    SealRequest Observe(const Prune& prune) {
        if (prune.started) {
            ++episode_;
            processId_ = prune.processId;
            shownAtStart_ = prune.shownTotal;
            silenceStartUs_ = prune.firstUnshownUs > 0 && prune.firstUnshownUs <= prune.nowUs ? prune.firstUnshownUs
                                                                                                 : prune.nowUs;
            silenceEndUs_ = 0;
            ongoingSealed_ = false;
            inEpisode_ = true;
        }
        if (!inEpisode_ || (!prune.expiring && !prune.stopped))
            return {};
        inEpisode_ = !prune.stopped;
        const uint64_t shown = prune.shownTotal - shownAtStart_;
        if (silenceEndUs_ == 0 && (shown != 0 || prune.stopped))
            silenceEndUs_ = prune.nowUs;
        const int64_t endUs = silenceEndUs_ != 0 ? silenceEndUs_ : prune.nowUs;
        const uint64_t silenceMs = endUs > silenceStartUs_ ? static_cast<uint64_t>(endUs - silenceStartUs_) / 1000 : 0;
        if (prune.stopped)
            return silenceMs >= kMinimumSilenceMs ? Take(SealReason::kEnded, silenceMs, shown) : SealRequest{};
        if (!ongoingSealed_ && silenceEndUs_ == 0 && silenceMs >= kOngoingSealMs) {
            ongoingSealed_ = true;
            return Take(SealReason::kOngoing, silenceMs, shown);
        }
        return {};
    }

    uint32_t seals() const noexcept { return seals_; }
    uint32_t refusedForLimit() const noexcept { return refusedForLimit_; }

private:
    SealRequest Take(SealReason reason, uint64_t silenceMs, uint64_t shown) {
        if (seals_ >= kMaximumSealsPerSession) {
            ++refusedForLimit_;
            return {};
        }
        ++seals_;
        return {reason, episode_, processId_, silenceMs, silenceStartUs_, shown};
    }

    uint32_t episode_ = 0;
    uint32_t processId_ = 0;
    uint64_t shownAtStart_ = 0;
    int64_t silenceStartUs_ = 0;
    int64_t silenceEndUs_ = 0;
    bool inEpisode_ = false;
    bool ongoingSealed_ = false;
    uint32_t seals_ = 0;
    uint32_t refusedForLimit_ = 0;
};

}  // namespace display_stall_trace
