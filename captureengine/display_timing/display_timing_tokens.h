#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
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
// How a token ends, as seen in the 2026-11-11 GTA traces (display-stall-trace.md):
// - composed and shown: InFrame -> Confirmed -> Retired, then Discarded once its buffer is released (`retired`; the
//   trailing Discarded is not a second end);
// - shown by independent flip: InFrame -> Discarded with IndependentFlip=1 (`flipped`);
// - not shown: Discarded without either (`discarded`). Presents replaced by a newer one before the compositor's next
//   frame end here too; a black screen is presents ending here with nothing retired or flipped - the cold DLSS-G loads
//   took ~150 of them through Confirmed -> Discarded before DWM bound the new surface.
//
// Surface onsets: the first present of a (surface, binding) pair the process has not used recently starts an onset,
// which ends at that surface's first shown present (or when the process starts yet another surface before any was
// shown). Its span and the presents discarded before it are what a black screen after a swapchain switch looks like
// from the compositor's side, with no raw trace needed.
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
    uint64_t confirmed = 0;  // taken into a compositor frame (an intermediate state, counted once per token)
    uint64_t retired = 0;    // shown by composition
    uint64_t flipped = 0;    // shown by independent flip
    uint64_t discarded = 0;  // ended without being shown

    bool operator==(const TokenCounts& other) const noexcept {
        return confirmed == other.confirmed && retired == other.retired && flipped == other.flipped &&
               discarded == other.discarded;
    }
};

struct SurfaceOnset {
    uint32_t processId = 0;
    uint64_t surfaceLuid = 0;
    uint64_t bindId = 0;
    int64_t firstPresentUs = 0;    // the surface's first token
    int64_t shownUs = 0;           // its first shown present; 0 when the process moved on before any was shown
    bool shownByIndependentFlip = false;
    uint64_t presents = 0;         // the surface's presents created until it was first shown (all, if never)
    uint64_t discardedBefore = 0;  // of those, the ones that ended unshown before it
};

class CompositionTokenTally {
public:
    // Only bounds a runaway producer; a token ends within a few frames.
    static constexpr std::size_t kMaxTracked = 256;
    // Surfaces a process switches between without that being a new onset (DXGI rebinds a resized chain).
    static constexpr std::size_t kRememberedSurfaces = 8;
    // Onsets in flight and finished ones not yet taken; one per tracked process in practice.
    static constexpr std::size_t kMaxOnsets = 4;

    // A tracked process created a token. PresentCount is 64 bits here and 32 bits in the state event: the low half
    // identifies the present.
    void ObserveCreated(uint32_t processId, uint64_t surfaceLuid, uint64_t presentCount, uint64_t bindId,
                        int64_t timeUs) {
        if (tokens_.size() >= kMaxTracked) {
            tokens_.erase(tokens_.begin());
            ++evicted_;
        }
        tokens_.push_back({surfaceLuid, static_cast<uint32_t>(presentCount), bindId, false});
        ++created_;
        NoteOnsetPresent(processId, surfaceLuid, bindId, timeUs);
    }

    enum class Match : uint8_t {
        kUnmatched,  // no tracked token: another process's
        kCounted,    // an end state (or a confirmation) of a tracked token
        kIgnored,    // an intermediate state of a tracked token, or the release after its retirement
    };

    Match ObserveState(uint64_t surfaceLuid, uint32_t presentCount, uint64_t bindId, uint32_t newState,
                       bool independentFlip, int64_t timeUs) {
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
        Token& token = tokens_[found];
        switch (newState) {
            case kStateConfirmed:
                Count(&TokenCounts::confirmed);
                return Match::kCounted;
            case kStateRetired:
                // Stays tracked until its release, which must not count again.
                token.retired = true;
                Count(&TokenCounts::retired);
                NoteOnsetShown(surfaceLuid, bindId, timeUs, false);
                return Match::kCounted;
            case kStateDiscarded: {
                const bool wasRetired = token.retired;
                tokens_.erase(tokens_.begin() + static_cast<std::ptrdiff_t>(found));
                if (wasRetired)
                    return Match::kIgnored;
                if (independentFlip) {
                    Count(&TokenCounts::flipped);
                    NoteOnsetShown(surfaceLuid, bindId, timeUs, true);
                } else {
                    Count(&TokenCounts::discarded);
                    NoteOnsetDiscarded(surfaceLuid, bindId);
                }
                return Match::kCounted;
            }
            default:
                return Match::kIgnored;
        }
    }

    // The oldest finished onset, once.
    std::optional<SurfaceOnset> TakeOnset() {
        if (finished_.empty())
            return std::nullopt;
        SurfaceOnset onset = finished_.front();
        finished_.pop_front();
        return onset;
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
    uint64_t onsetsDropped() const noexcept { return onsetsDropped_; }

private:
    struct Token {
        uint64_t surfaceLuid = 0;
        uint32_t presentCount = 0;
        uint64_t bindId = 0;
        bool retired = false;
    };

    struct SurfaceKey {
        uint32_t processId = 0;
        uint64_t surfaceLuid = 0;
        uint64_t bindId = 0;
    };

    void Count(uint64_t TokenCounts::* field) {
        ++(total_.*field);
        ++(episode_.*field);
    }

    void NoteOnsetPresent(uint32_t processId, uint64_t surfaceLuid, uint64_t bindId, int64_t timeUs) {
        for (SurfaceOnset& onset : pending_) {
            if (onset.surfaceLuid == surfaceLuid && onset.bindId == bindId) {
                ++onset.presents;
                return;
            }
        }
        for (const SurfaceKey& known : known_) {
            if (known.processId == processId && known.surfaceLuid == surfaceLuid && known.bindId == bindId)
                return;
        }
        // The process moved on: an onset of its that never showed a frame ends unshown.
        for (std::size_t i = 0; i < pending_.size();) {
            if (pending_[i].processId == processId) {
                Finish(i);
            } else {
                ++i;
            }
        }
        if (pending_.size() >= kMaxOnsets)
            Finish(0);
        if (known_.size() >= kRememberedSurfaces)
            known_.erase(known_.begin());
        known_.push_back({processId, surfaceLuid, bindId});
        SurfaceOnset onset;
        onset.processId = processId;
        onset.surfaceLuid = surfaceLuid;
        onset.bindId = bindId;
        onset.firstPresentUs = timeUs;
        onset.presents = 1;
        pending_.push_back(onset);
    }

    void NoteOnsetShown(uint64_t surfaceLuid, uint64_t bindId, int64_t timeUs, bool byIndependentFlip) {
        for (std::size_t i = 0; i < pending_.size(); ++i) {
            if (pending_[i].surfaceLuid == surfaceLuid && pending_[i].bindId == bindId) {
                pending_[i].shownUs = timeUs;
                pending_[i].shownByIndependentFlip = byIndependentFlip;
                Finish(i);
                return;
            }
        }
    }

    void NoteOnsetDiscarded(uint64_t surfaceLuid, uint64_t bindId) {
        for (SurfaceOnset& onset : pending_) {
            if (onset.surfaceLuid == surfaceLuid && onset.bindId == bindId) {
                ++onset.discardedBefore;
                return;
            }
        }
    }

    void Finish(std::size_t index) {
        if (finished_.size() >= kMaxOnsets) {
            finished_.pop_front();
            ++onsetsDropped_;
        }
        finished_.push_back(pending_[index]);
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
    }

    std::vector<Token> tokens_;
    std::vector<SurfaceKey> known_;
    std::vector<SurfaceOnset> pending_;
    std::deque<SurfaceOnset> finished_;
    TokenCounts total_;
    TokenCounts episode_;
    uint64_t created_ = 0;
    uint64_t unmatched_ = 0;
    uint64_t evicted_ = 0;
    uint64_t onsetsDropped_ = 0;
};

}  // namespace display_timing_tokens
