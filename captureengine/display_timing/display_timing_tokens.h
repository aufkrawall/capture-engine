#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// How the compositor ended the presents of a followed process: Win32k's composition-token states.
//
// GTA V Enhanced, session 20261110_182518: the new DLSS-G swapchain presented for 19.5 s while the sensor saw its
// presents expire without a flip completion (`composed ready=2340 published=0`). Whether DWM took those presents and
// retired them, or discarded them without composing, is exactly what the compositor's own trace tells and what the
// flip stream cannot: a token of a windowed present is created when the process presents (TokenCompositionSurfaceObject)
// and ends Retired (the frame was composed and its surface released) or Discarded (replaced or dropped before it was
// shown). Event ids, keywords, layouts and state values: PresentMon's Microsoft_Windows_Win32k.h
// (external/SpecialK-main/include/SpecialK/render/present_mon/ETW); the decoding is common/platform/display_timing_etw.h.
//
// A token is matched on (CompositionSurfaceLuid, PresentCount, BindId) between its creation, which the service sees
// only for tracked processes, and its state changes, which are emitted for every process on the machine: a state
// change nobody created is another process's and is not counted anywhere but `unmatched`.
//
// Pure: no ETW calls; unit-tested.

namespace display_timing_tokens {

// NewState values of TokenStateChanged (PresentMon's TokenState).
inline constexpr uint32_t kStateCompleted = 2;
inline constexpr uint32_t kStateInFrame = 3;
inline constexpr uint32_t kStateConfirmed = 4;
inline constexpr uint32_t kStateRetired = 5;
inline constexpr uint32_t kStateDiscarded = 6;

struct TokenCounts {
    uint64_t confirmed = 0;
    uint64_t retired = 0;
    uint64_t discarded = 0;

    bool operator==(const TokenCounts& other) const noexcept {
        return confirmed == other.confirmed && retired == other.retired && discarded == other.discarded;
    }
};

class CompositionTokenTally {
public:
    // Only bounds a runaway producer; a token ends within a few frames.
    static constexpr std::size_t kMaxTracked = 256;

    // A tracked process created a token. PresentCount is 64 bits here and 32 bits in the state event: the low half
    // identifies the present.
    void ObserveCreated(uint64_t surfaceLuid, uint64_t presentCount, uint64_t bindId) {
        if (tokens_.size() >= kMaxTracked) {
            tokens_.erase(tokens_.begin());
            ++evicted_;
        }
        tokens_.push_back({surfaceLuid, static_cast<uint32_t>(presentCount), bindId});
        ++created_;
    }

    enum class Match : uint8_t {
        kUnmatched,  // no tracked token: another process's
        kCounted,    // an end state (or a confirmation) of a tracked token
        kIgnored,    // an intermediate state of a tracked token
    };

    Match ObserveState(uint64_t surfaceLuid, uint32_t presentCount, uint64_t bindId, uint32_t newState) {
        std::size_t found = tokens_.size();
        for (std::size_t i = tokens_.size(); i-- > 0;) {
            if (tokens_[i].surfaceLuid == surfaceLuid && tokens_[i].presentCount == presentCount &&
                tokens_[i].bindId == bindId) {
                found = i;
                break;
            }
        }
        if (found == tokens_.size()) {
            ++unmatched_;
            return Match::kUnmatched;
        }
        switch (newState) {
            case kStateConfirmed:
                ++total_.confirmed;
                ++episode_.confirmed;
                return Match::kCounted;
            case kStateRetired:
                ++total_.retired;
                ++episode_.retired;
                tokens_.erase(tokens_.begin() + static_cast<std::ptrdiff_t>(found));
                return Match::kCounted;
            case kStateDiscarded:
                ++total_.discarded;
                ++episode_.discarded;
                tokens_.erase(tokens_.begin() + static_cast<std::ptrdiff_t>(found));
                return Match::kCounted;
            default:
                return Match::kIgnored;
        }
    }

    // A composed episode starts: its counts begin again; the totals do not.
    void BeginEpisode() { episode_ = {}; }

    bool HasTrackedTokens() const noexcept { return !tokens_.empty(); }
    std::size_t tracked() const noexcept { return tokens_.size(); }
    const TokenCounts& total() const noexcept { return total_; }
    const TokenCounts& episode() const noexcept { return episode_; }
    uint64_t created() const noexcept { return created_; }
    uint64_t unmatched() const noexcept { return unmatched_; }
    uint64_t evicted() const noexcept { return evicted_; }

private:
    struct Token {
        uint64_t surfaceLuid = 0;
        uint32_t presentCount = 0;
        uint64_t bindId = 0;
    };

    std::vector<Token> tokens_;
    TokenCounts total_;
    TokenCounts episode_;
    uint64_t created_ = 0;
    uint64_t unmatched_ = 0;
    uint64_t evicted_ = 0;
};

}  // namespace display_timing_tokens
