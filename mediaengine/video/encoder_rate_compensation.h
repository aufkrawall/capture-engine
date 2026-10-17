#pragma once

// Keeps the output bitrate of a skipping encoder at the user's target.
//
// NVENC rate control budgets averageBitRate / fps per frame it receives and
// never sees timestamps. Output slots served by AV1 show-existing units reach
// no encoder, so without help the file would drop to the submitted share of
// the target, and every unique frame would lose the bits a cheap re-encoded
// repeat used to leave over (measured: -1.4 dB at half the slots, -8.5 dB at a
// tenth; llm-wiki/nvenc-encoding.md). Scaling the configured rates by
// slots/submitted restores the real-time rate: each submitted frame gets the
// budget of all the slots it covers.
//
// Raising follows the long window only (a short run of repeats must not spend
// ahead); once the short window clearly sees more encoder frames, lowering
// follows it at once. A live stream adds a hard cap: the budgets handed out in
// any one second never exceed kPeakAllowance times the configured rate, so a
// source that speeds up cannot burst the uplink while the estimate catches
// up. Holding that cap means paying a burst back with frames below the base
// rate (down to kLiveMinFactor), a brief dip a file recording does not need.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ce::media {

class SubmittedRateCompensation {
public:
    // Live: budget handed out in any one-second window, relative to the
    // target, and the lowest factor used to pay a burst back.
    static constexpr double kPeakAllowance = 1.10;
    static constexpr double kLiveMinFactor = 0.5;

    struct Rates {
        int64_t bitRate = 0;
        int64_t maxRate = 0;
        double factor = 1.0;
    };

    // `baseBitRate`/`baseMaxRate` are the configured rates (0 = not set).
    void Reset(int fps, int64_t baseBitRate, int64_t baseMaxRate, bool liveCap);
    bool Active() const {
        return active_;
    }

    // One output slot; `submitted` when it reaches the encoder. Call before
    // the frame is sent: Current() then holds the rate for that frame.
    void ObserveSlot(bool submitted);
    // True when the encoder should switch to Current() before the next frame.
    bool TakeChange();
    Rates Current() const {
        return current_;
    }

    // The encoder rejected Current(): stay below it for the rest of the
    // session (FFmpeg would otherwise retry the rejected rates every frame).
    void OnRejected();

    double MaxAppliedFactor() const {
        return maxAppliedFactor_;
    }
    uint64_t Changes() const {
        return changes_;
    }
    uint64_t PeakCapped() const {
        return peakCapped_;
    }
    uint64_t Rejections() const {
        return rejections_;
    }
    size_t ShortWindow() const {
        return shortWindow_.size();
    }

private:
    class Window {
    public:
        void Reset(size_t slots, double fill);
        // Adds `value` for the newest slot and returns the one that dropped out.
        double Push(double value);
        double Sum() const {
            return sum_;
        }
        double Fraction() const {
            return sum_ / static_cast<double>(values_.size());
        }
        double Oldest() const {
            return values_[next_];
        }
        size_t size() const {
            return values_.size();
        }

    private:
        std::vector<double> values_;
        size_t next_ = 0;
        double sum_ = 0.0;
    };

    void UpdateDesired();
    void Apply(double factor);
    Rates RatesFor(double factor) const;

    bool active_ = false;
    bool liveCap_ = false;
    int64_t baseBitRate_ = 0;
    int64_t baseMaxRate_ = 0;
    double factorCap_ = 1.0;
    Window longWindow_;   // encoder frames per slot, estimate
    Window shortWindow_;  // encoder frames per slot, speed-up detection
    Window spend_;        // factor handed to each slot over the last second
    double desired_ = 1.0;
    Rates current_;
    double maxAppliedFactor_ = 1.0;
    size_t slotsSinceChange_ = 0;
    bool pending_ = false;
    uint64_t changes_ = 0;
    uint64_t peakCapped_ = 0;
    uint64_t rejections_ = 0;
};

}  // namespace ce::media
