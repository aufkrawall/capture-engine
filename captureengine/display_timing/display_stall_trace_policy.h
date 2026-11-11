#pragma once

#include <cstdint>

// When a display episode deserves its raw trace (common/platform/display_flight_recorder.h).
//
// The submission-expiry monitor (display_timing_submissions.h) starts an episode when a tracked process's presents
// stop completing as flips of their own and ends it when they do again. Most episodes are the compositor showing the
// process (windowed, an overlay above it): its frames still reach the screen. The ones worth a trace are those where
// the compositor showed almost none of them either - the screen stood still while the game presented (GTA V Enhanced,
// 20261110_222057: 37 s, 4925 successful presents, 3 composed frames at the very end). Such an episode is sealed when
// it ends, and once while it lasts so a long one keeps its start in the circular recording.
namespace display_stall_trace {

inline constexpr uint64_t kMinimumEpisodeMs = 1'500;
inline constexpr uint64_t kOngoingSealMs = 10'000;
// Composed frames per second of the episode that still count as "nothing reached the screen".
inline constexpr uint64_t kMaximumShownPerSecond = 2;
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
    uint64_t episodeMs = 0;
    uint64_t shown = 0;  // the compositor's frames of the process during the episode
    explicit operator bool() const noexcept { return reason != SealReason::kNone; }
};

inline bool NothingReachedTheScreen(uint64_t episodeMs, uint64_t shown) {
    return shown * 1000 <= kMaximumShownPerSecond * episodeMs;
}

class Policy {
public:
    // Every prune: the monitor's transition (started/stopped), whether it is in an episode, the episode's length so far,
    // the cumulative count of composed frames the compositor showed, and the process the episode is about.
    SealRequest Observe(bool started, bool stopped, bool expiring, uint64_t episodeMs, uint64_t shownTotal,
                        uint32_t processId) {
        if (started) {
            ++episode_;
            shownAtStart_ = shownTotal;
            processId_ = processId;
            ongoingSealed_ = false;
        }
        const uint64_t shown = shownTotal - shownAtStart_;
        const bool silent = NothingReachedTheScreen(episodeMs, shown);
        if (stopped) {
            const bool seal = episode_ != 0 && episodeMs >= kMinimumEpisodeMs && silent;
            return seal ? Take(SealReason::kEnded, episodeMs, shown) : SealRequest{};
        }
        if (expiring && !ongoingSealed_ && episode_ != 0 && episodeMs >= kOngoingSealMs && silent) {
            ongoingSealed_ = true;
            return Take(SealReason::kOngoing, episodeMs, shown);
        }
        return {};
    }

    uint32_t seals() const noexcept { return seals_; }
    uint32_t refusedForLimit() const noexcept { return refusedForLimit_; }

private:
    SealRequest Take(SealReason reason, uint64_t episodeMs, uint64_t shown) {
        if (seals_ >= kMaximumSealsPerSession) {
            ++refusedForLimit_;
            return {};
        }
        ++seals_;
        return {reason, episode_, processId_, episodeMs, shown};
    }

    uint32_t episode_ = 0;
    uint32_t processId_ = 0;
    uint64_t shownAtStart_ = 0;
    bool ongoingSealed_ = false;
    uint32_t seals_ = 0;
    uint32_t refusedForLimit_ = 0;
};

}  // namespace display_stall_trace
