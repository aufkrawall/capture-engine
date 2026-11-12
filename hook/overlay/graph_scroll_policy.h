#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

enum class GraphOutputKind { kApplicationDraw, kDistinctOutput };
struct GraphOutputContext {
    GraphOutputKind kind = GraphOutputKind::kApplicationDraw;
    uint64_t outputId = 0;
};
inline GraphOutputContext NextGraphOutput() {
    static std::atomic<uint64_t> next{1};
    return {GraphOutputKind::kDistinctOutput, next.fetch_add(1, std::memory_order_relaxed)};
}

// The time values use the display timing stream's QPC microsecond domain. The measured sample values are never
// modified: this snapshot only associates a horizontal graph position with the output being rendered.
struct GraphCadenceSnapshot {
    uint64_t historyIdentity = 0;
    uint64_t sampleCount = 0;
    int64_t lastSampleUs = 0;
    double sampleIntervalUs = 0.0;
    int64_t lastDisplayUs = 0;
    double displayIntervalUs = 0.0;
    int64_t drawUs = 0;
    GraphOutputKind outputKind = GraphOutputKind::kApplicationDraw;
    uint64_t outputId = 0;
};

// A scrolling graph advances one slot per drawn frame. That is automatic while
// every drawn frame produces exactly one sample, which is what presentation
// timing does. Display-change timing decouples the two: under multi-frame
// generation the runtime issues a whole group of presents within about two
// milliseconds and then idles, while the display consumes them evenly. Measured
// on a 4x MFG session, 64% of overlay draws saw no new sample and 23% saw three
// or four (mean 1.00, stddev 2.18), so a graph advanced by sample arrival
// stepped at the base frame rate while the screen updated at the display rate.
//
// The cursor therefore advances one slot per drawn frame and is pulled gently
// toward the sample stream rather than driven by it.
//
// To keep scrolling across a burst the cursor has to stay far enough behind the
// newest sample to have somewhere to scroll into: a group of N presents drawn
// back to back needs N-1 slots of already-received samples, plus one more for
// the guard sample that keeps the plot covering its panel edge to edge. That
// distance is one base-frame interval by construction and cannot be avoided -
// the samples for those frames do not exist yet when they are drawn. It is
// measured from the stream rather than assumed, so it costs two slots without
// frame generation and grows only as far as the observed group size requires.
class GraphScrollCursor {
public:
    // Floor for the trail: one slot of headroom plus the guard sample.
    static constexpr double kMinTrailSlots = 2.0;
    // Small enough that the residual velocity ripple across a frame-generation
    // group stays within a few percent, large enough to absorb real drift
    // between draw rate and sample rate.
    static constexpr double kCorrectionRate = 0.02;
    // How fast the measured trail relaxes back toward the floor once groups get
    // shorter. Slow enough to survive many groups, so a steady multiplier holds
    // a steady trail instead of sawing between two values.
    static constexpr double kTrailRelaxRate = 0.0005;
    static constexpr double kResyncSlots = 8.0;
    // Bounds how much of a dry streak counts as a group. Frame generation
    // groups are small; anything longer is a stall, which must hold the cursor
    // rather than park the plot far back in history.
    static constexpr uint32_t kMaxDryStreak = 8;

    // Advances by one drawn frame and returns the cursor in absolute sample
    // index units: the integer part is the newest sample to plot, the fraction
    // is the sub-slot offset the plot is shifted by.
    double Advance(uint64_t sampleCount) {
        // The caller reads one sample past the cursor, so two samples must
        // already exist before anything can be plotted.
        if (sampleCount < 2) {
            Reset();
            lastSampleCount_ = sampleCount;
            return 0.0;
        }
        // A stream that restarts - a source switch, a history reset - is a
        // different series, not drift, so re-arm on it instead of scrolling
        // backwards into it. It is a fresh stream, not a draw that missed its
        // sample, so it must not count toward the dry streak either.
        const bool restarted = sampleCount < lastSampleCount_;
        if (restarted) {
            primed_ = false;
            trail_ = kMinTrailSlots;
            dryStreak_ = 0;
        }
        // How many draws in a row have seen no new sample is exactly how far
        // the cursor must be able to scroll without one arriving. The cap keeps
        // a stall - which is not a group - from inflating the trail.
        else if (sampleCount > lastSampleCount_)
            dryStreak_ = 0;
        else if (dryStreak_ < kMaxDryStreak)
            ++dryStreak_;
        lastSampleCount_ = sampleCount;

        const double required = static_cast<double>(dryStreak_) + kMinTrailSlots;
        if (required > trail_)
            trail_ = required;
        else
            trail_ += (kMinTrailSlots - trail_) * kTrailRelaxRate;

        const double newest = static_cast<double>(sampleCount) - 2.0;
        const double target = static_cast<double>(sampleCount) - 1.0 - trail_;
        if (!primed_) {
            primed_ = true;
            position_ = target > 0.0 ? target : 0.0;
            return position_;
        }

        const double error = target - position_;
        if (error > kResyncSlots) {
            // The stream jumped far ahead: catch it in one step rather than
            // crawling for seconds.
            position_ = target;
        } else {
            // Otherwise adjust speed, never direction. A graph that steps
            // backwards reads as a glitch, not as a correction, so the cursor
            // slows to at most a standstill and lets the clamp below hold it.
            double advance = 1.0 + kCorrectionRate * error;
            if (advance < 0.0)
                advance = 0.0;
            position_ += advance;
        }
        if (position_ > newest)
            position_ = newest;
        if (position_ < 0.0)
            position_ = 0.0;
        return position_;
    }

    // Predict the display slot the image can reach. Multiple application draws for that slot get the same
    // position; distinct runtime outputs reserve successive slots even if submitted in one CPU burst.
    double Advance(const GraphCadenceSnapshot& sample) {
        const bool restarted = !cadencePrimed_ || sample.historyIdentity != cadenceHistoryIdentity_ ||
                               sample.sampleCount < lastSampleCount_;
        if (restarted) {
            Reset();
            cadencePrimed_ = true;
            cadenceHistoryIdentity_ = sample.historyIdentity;
        }
        if (sample.sampleCount < 2) {
            lastSampleCount_ = sample.sampleCount;
            return 0.0;
        }
        if (sample.lastSampleUs <= 0 || sample.lastDisplayUs <= 0 || sample.drawUs <= 0 ||
            sample.sampleIntervalUs <= 0.0 || sample.displayIntervalUs <= 0.0) {
            // Without a cadence there is no evidence that a draw represents a new display. Hold within history.
            lastSampleCount_ = sample.sampleCount;
            return position_;
        }
        if (sample.outputKind == GraphOutputKind::kDistinctOutput && sample.outputId != 0 &&
            lastOutputKind_ == GraphOutputKind::kDistinctOutput && sample.outputId == lastOutputId_) {
            return position_;
        }
        // QPC times are published in integral microseconds. Select the first slot strictly after the draw,
        // comparing in that same domain so rounding a fractional refresh period cannot repeat a slot.
        double displaySteps = (std::max)(1.0, std::floor((static_cast<double>(sample.drawUs) - sample.lastDisplayUs) /
                                                        sample.displayIntervalUs));
        double displayTime = static_cast<double>(sample.lastDisplayUs) + displaySteps * sample.displayIntervalUs;
        if (std::llround(displayTime) <= sample.drawUs)
            displayTime += sample.displayIntervalUs;
        if (sample.outputKind == GraphOutputKind::kDistinctOutput &&
            lastOutputKind_ == GraphOutputKind::kDistinctOutput && lastOutputId_ != 0) {
            displayTime = (std::max)(displayTime, lastScheduledDisplayUs_ + sample.displayIntervalUs);
        }
        // A missing display stream must not build an unbounded queue of invented output times.
        const double latestPrediction = (std::max)(static_cast<double>(sample.drawUs),
                                                    static_cast<double>(sample.lastDisplayUs)) +
                                        kMaxDryStreak * sample.displayIntervalUs;
        displayTime = (std::min)(displayTime, latestPrediction);
        const double ahead = (std::max)(0.0, (displayTime - sample.lastSampleUs) / sample.sampleIntervalUs);
        const double boundedLead = (std::min)(ahead, static_cast<double>(kMaxDryStreak));
        trail_ = (std::max)(trail_, kMinTrailSlots + std::ceil(boundedLead));
        const double newest = static_cast<double>(sample.sampleCount) - 2.0;
        double target = static_cast<double>(sample.sampleCount) - 1.0 + ahead - trail_;
        target = (std::max)(0.0, (std::min)(target, newest));
        if (!primed_) {
            position_ = target;
            primed_ = true;
        } else {
            position_ = (std::max)(position_, target);
            position_ = (std::min)(position_, newest);
        }
        lastSampleCount_ = sample.sampleCount;
        lastOutputKind_ = sample.outputKind;
        lastOutputId_ = sample.outputId;
        lastScheduledDisplayUs_ = displayTime;
        return position_;
    }

    void Reset() {
        cadencePrimed_ = false;
        cadenceHistoryIdentity_ = 0;
        lastOutputKind_ = GraphOutputKind::kApplicationDraw;
        lastOutputId_ = 0;
        lastScheduledDisplayUs_ = 0.0;
        primed_ = false;
        position_ = 0.0;
        trail_ = kMinTrailSlots;
        dryStreak_ = 0;
        lastSampleCount_ = 0;
    }

    bool primed() const noexcept { return primed_; }
    double trail() const noexcept { return trail_; }

private:
    bool cadencePrimed_ = false;
    uint64_t cadenceHistoryIdentity_ = 0;
    GraphOutputKind lastOutputKind_ = GraphOutputKind::kApplicationDraw;
    uint64_t lastOutputId_ = 0;
    double lastScheduledDisplayUs_ = 0.0;
    double position_ = 0.0;
    double trail_ = kMinTrailSlots;
    uint64_t lastSampleCount_ = 0;
    uint32_t dryStreak_ = 0;
    bool primed_ = false;
};
