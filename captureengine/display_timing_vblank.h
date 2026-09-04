#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// The vertical-blank clock, and the rule that a vsync-deferred flip's screen
// time is a vertical blank rather than the timestamp of the event that reported
// the flip.
//
// Measured on this hardware under FSR frame generation (3840x2160 at 144 Hz,
// hardware flip queue): `DxgKrnl` `VSyncDPC` arrives every 6.946 ms without
// deviation, while the `HSyncDPCMultiPlane` completions that carry the flips
// arrive at two alternating phases inside that interval - 3.92 ms and 6.14 ms
// after the preceding blank - because the two frames of a generated pair become
// ready at different times and the hardware latches each one a variable time
// ahead of the scanout that shows it. Publishing those completion timestamps
// produced a 4.7 ms / 9.2 ms sawtooth at an exactly correct 144 fps mean, and
// 4.7 ms is shorter than the panel's own minimum frame interval, so it could
// not have been a screen cadence. Rounding each completion onto the blank it
// belongs to restores the cadence the screen actually shows.

// Whether the blank clock is currently able to place frames.
//
// Measured over a decaying window of completions rather than assumed, so a
// display that changes regime - a variable-refresh panel reaching or leaving its
// cap, a mode change, a monitor swap - is followed rather than remembered. The
// attempt is made on every completion whatever this says; only publishing the
// result is gated, so the record can never latch itself into a corner.
class BlankCadence {
public:
    // The clock must be able to answer for at least this share of recent
    // completions. The slack absorbs the odd blank lost to an event-buffer drop;
    // it is far looser than the shortfall an aperiodic blank stream produces.
    static constexpr uint64_t kRequiredPlacementPercent = 90;
    // Enough completions for the ratio to mean something - about a third of a
    // second of frames - before it may overrule the initial assumption.
    static constexpr uint64_t kMinimumClaims = 32;
    // The window halves at this many claims, so the judgement follows the display
    // instead of averaging over everything it has ever done.
    static constexpr uint64_t kWindowClaims = 256;

    void ObserveClaim(bool placed) {
        ++claims_;
        if (placed)
            ++placements_;
        if (claims_ >= kWindowClaims) {
            claims_ /= 2;
            placements_ /= 2;
        }
    }

    // True while the clock is placing frames reliably. Assumed true until there
    // is enough evidence to say otherwise, so a display whose blanks do lie on a
    // grid never spends its first frames unrounded.
    bool CanPlaceFrames() const {
        if (claims_ < kMinimumClaims)
            return true;
        return placements_ * 100 >= claims_ * kRequiredPlacementPercent;
    }

    uint64_t claims() const { return claims_; }
    uint64_t placements() const { return placements_; }

private:
    uint64_t claims_ = 0;
    uint64_t placements_ = 0;
};

// The refresh grid a display's reported blanks lie on.
//
// The driver does not report every vertical blank. Measured on this hardware at
// 3840x2160 with the panel at its 144 Hz cap, one run reported 4264 blanks for
// 4235 flips and the next reported 657 for 4247 - same application, same
// settings. A clock that can only answer for blanks it happened to observe is
// therefore unusable a good share of the time, and rounding only the completions
// it can answer for publishes screen times and flip-latch times in one series,
// which is worse than not rounding at all: measured under variable refresh, the
// published intervals went from 2.0 ms of standard deviation (the driver's raw
// latch timestamps) to 4.4 ms, with intervals as short as 0.7 ms and as long as
// 40 ms on a screen that was changing every 9.5 ms.
//
// A blank stream with holes in it is still a clock as long as the blanks it does
// report fall on a regular grid: the gaps are then whole multiples of one refresh
// period, and the blanks nothing reported are exactly the grid points in between.
//
// Variable refresh below the cap is where that is genuinely untrue. The panel
// refreshes when a frame is ready, the gaps measured 11 ms and 51 ms and 119 ms,
// and no period divides them. There is then no grid, and the honest answer is to
// leave the completions carrying their own timestamps.
struct BlankGrid {
    int64_t periodUs = 0;  // 0 when the observed gaps do not lie on one grid.
    int64_t anchor = 0;    // The newest observed blank; the grid runs through it.

    bool valid() const { return periodUs > 0 && anchor > 0; }
};

class VerticalBlankClock {
public:
    // Enough blanks to cover the publication reorder window several times over
    // at any refresh rate a display timing consumer cares about.
    static constexpr std::size_t kBlanksPerSource = 32;
    static constexpr std::size_t kMaxSources = 8;
    // A completion this close after a blank already is that blank's screen
    // time, reported by a DPC that ran a little late; it is rounded back onto
    // the blank instead of forward to the next one. Expressed as a share of the
    // measured refresh period so it holds at any refresh rate.
    static constexpr int64_t kAtBlankNumerator = 1;
    static constexpr int64_t kAtBlankDenominator = 5;
    // How far an ordered claim may be pushed past the blank a completion landed
    // on before it gives up and lets the frame be recorded as dropped.
    static constexpr std::size_t kMaxForcedBlanks = 4;
    // How far the grid may be extrapolated past the newest observed blank, so a
    // grid left over from before an idle stretch cannot answer for a time far
    // outside the window it was measured in.
    static constexpr int64_t kMaxExtrapolatedPeriods = 64;
    // Two blanks are one gap, which fits any period; three is the least that can
    // disagree about one.
    static constexpr std::size_t kMinimumGridBlanks = 3;

    void Observe(uint32_t displaySource, int64_t timestamp) {
        if (timestamp <= 0)
            return;
        Source* source = FindOrCreate(displaySource);
        if (!source || (source->count != 0 && timestamp <= source->Newest()))
            return;
        source->blanks[source->next] = timestamp;
        source->next = (source->next + 1) % kBlanksPerSource;
        if (source->count < kBlanksPerSource)
            ++source->count;
        ++source->observed;
        source->grid = MeasureGrid(*source);
    }

    // The blank this completion's frame reaches the screen at, or 0 when the
    // clock cannot answer - the caller then keeps the uncorrected timestamp
    // rather than inventing one. Answered from the grid rather than from the
    // blanks that were observed, so a stream the driver reported only part of
    // still places frames, and a blank still in the future - the normal case at
    // the moment a flip completes - is answered rather than waited for.
    int64_t Snap(uint32_t displaySource, int64_t timestamp) const {
        const Source* source = Find(displaySource);
        if (!source || !source->grid.valid() || timestamp <= 0)
            return 0;
        return GridBlankFor(source->grid, timestamp);
    }

    // A display shows at most one new frame per blank and shows them in order,
    // so consecutive completions take consecutive blanks. The lead between
    // latching a flip and the scanout that shows it is not constant - under
    // frame generation the two frames of a pair differ by milliseconds - and
    // once that spread exceeds a refresh period, Snap alone maps two flips onto
    // one blank and the later one would be published as a dropped frame that
    // the screen did in fact show. Claiming blanks in completion order is what
    // keeps the published series at the rate the display is running at.
    //
    // Must be called once per completion, in completion order. Returns 0 when
    // the clock cannot answer, leaving the completion unresolved.
    int64_t Claim(uint32_t displaySource, int64_t timestamp) {
        const int64_t natural = Snap(displaySource, timestamp);
        Source* source = FindOrCreate(displaySource);
        if (!source)
            return 0;
        // Whether the grid could answer is recorded whatever the gate then
        // decides, so refusing can never be the reason the record keeps
        // refusing. The ordering walk below is deliberately not part of this:
        // giving up there means the frame did not get a blank of its own, which
        // is a true statement about a game outrunning the display rather than a
        // fault in the clock.
        source->cadence.ObserveClaim(natural != 0);
        // A clock that is not placing frames reliably cannot place this one
        // either. Leaving it unresolved keeps the published series in one unit
        // instead of mixing screen times with flip-latch times.
        if (natural == 0 || !source->cadence.CanPlaceFrames())
            return 0;
        if (source->lastClaimed == 0 || natural > source->lastClaimed) {
            source->lastClaimed = natural;
            return natural;
        }
        // Walk forward from the blank this completion landed on to the first
        // one nothing has claimed. A frame rate above the refresh rate would
        // march that forward without end, so the walk is bounded by how far the
        // frame may be carried past its own blank; beyond that it keeps the
        // natural blank, where the publisher's monotonic guard records an
        // honest dropped frame - which is what the display did with it.
        int64_t forced = natural;
        for (std::size_t step = 0; step < kMaxForcedBlanks; ++step) {
            forced += source->grid.periodUs;
            if (forced > source->lastClaimed) {
                source->lastClaimed = forced;
                return forced;
            }
        }
        return natural;
    }

    // The refresh period the clock is running on, or 0 while it has no grid.
    int64_t PeriodUs(uint32_t displaySource) const {
        const Source* source = Find(displaySource);
        return source ? source->grid.periodUs : 0;
    }

    uint64_t observedBlanks(uint32_t displaySource) const {
        const Source* source = Find(displaySource);
        return source ? source->observed : 0;
    }

    // Whether this source's blanks are currently placing frames. False is the
    // statement that the completions on this display are being published with
    // the driver's own flip timestamps, which is what a health report needs to
    // say to explain the shape of the series.
    bool CanPlaceFrames(uint32_t displaySource) const {
        const Source* source = Find(displaySource);
        return source ? source->grid.valid() && source->cadence.CanPlaceFrames() : false;
    }

    // The display the most blanks were seen on, which is the one a single-line
    // health report should describe.
    uint32_t busiestSource() const {
        const Source* busiest = nullptr;
        for (std::size_t i = 0; i < sourceCount_; ++i) {
            if (sources_[i].used && (!busiest || sources_[i].observed > busiest->observed))
                busiest = &sources_[i];
        }
        return busiest ? busiest->id : 0;
    }

    void Clear() {
        sources_ = {};
        sourceCount_ = 0;
    }

private:
    struct Source {
        std::array<int64_t, kBlanksPerSource> blanks = {};
        uint64_t observed = 0;
        BlankCadence cadence;
        BlankGrid grid;
        int64_t lastClaimed = 0;
        std::size_t next = 0;
        std::size_t count = 0;
        uint32_t id = 0;
        bool used = false;

        int64_t Newest() const { return blanks[(next + kBlanksPerSource - 1) % kBlanksPerSource]; }

        // The i-th oldest retained blank. Observe rejects anything that does not
        // advance, so insertion order is time order.
        int64_t At(std::size_t i) const {
            const std::size_t oldest = count < kBlanksPerSource ? 0 : next;
            return blanks[(oldest + i) % kBlanksPerSource];
        }
    };

    // The grid point this timestamp belongs to: the first one at or after it,
    // except that a completion within tolerance after a grid point already is
    // that point's screen time and rounds back onto it.
    static int64_t GridBlankFor(const BlankGrid& grid, int64_t timestamp) {
        const int64_t tolerance = (grid.periodUs * kAtBlankNumerator) / kAtBlankDenominator;
        const int64_t delta = timestamp - tolerance - grid.anchor;
        const int64_t steps =
            delta >= 0 ? (delta + grid.periodUs - 1) / grid.periodUs : -((-delta) / grid.periodUs);
        if (steps > kMaxExtrapolatedPeriods || steps < -kMaxExtrapolatedPeriods)
            return 0;
        return grid.anchor + steps * grid.periodUs;
    }

    // The grid the retained blanks lie on: the smallest gap is one refresh - it
    // is one whenever any two consecutive refreshes were both reported, which
    // bursts in the stream provide - and every other gap has to be a whole
    // number of them. A stream that does not satisfy that is refreshing at a
    // rate that keeps changing, and has no grid to offer.
    static BlankGrid MeasureGrid(const Source& source) {
        BlankGrid grid;
        if (source.count < kMinimumGridBlanks)
            return grid;
        const std::size_t gapCount = source.count - 1;
        int64_t candidate = 0;
        for (std::size_t i = 0; i < gapCount; ++i) {
            const int64_t gap = source.At(i + 1) - source.At(i);
            if (gap > 0 && (candidate == 0 || gap < candidate))
                candidate = gap;
        }
        if (candidate <= 0)
            return grid;
        const int64_t tolerance = (candidate * kAtBlankNumerator) / kAtBlankDenominator;
        int64_t steps = 0;
        for (std::size_t i = 0; i < gapCount; ++i) {
            const int64_t gap = source.At(i + 1) - source.At(i);
            const int64_t multiples = (gap + candidate / 2) / candidate;
            if (multiples <= 0)
                return grid;
            const int64_t residual = gap - multiples * candidate;
            if (residual > tolerance || residual < -tolerance)
                return grid;  // Not one grid: the refresh rate is varying.
            steps += multiples;
        }
        // The smallest gap is one sample and carries that sample's jitter. Now
        // that the step count is known, the period is the whole span divided by
        // it, which averages the jitter away and keeps extrapolation accurate
        // over the distance the grid is allowed to reach.
        grid.periodUs = (source.At(source.count - 1) - source.At(0)) / steps;
        if (grid.periodUs <= 0)
            return BlankGrid{};
        grid.anchor = source.Newest();
        return grid;
    }

    const Source* Find(uint32_t displaySource) const {
        for (std::size_t i = 0; i < sourceCount_; ++i) {
            if (sources_[i].used && sources_[i].id == displaySource)
                return &sources_[i];
        }
        return nullptr;
    }

    Source* FindOrCreate(uint32_t displaySource) {
        for (std::size_t i = 0; i < sourceCount_; ++i) {
            if (sources_[i].used && sources_[i].id == displaySource)
                return &sources_[i];
        }
        if (sourceCount_ >= kMaxSources)
            return nullptr;
        Source& source = sources_[sourceCount_++];
        source.id = displaySource;
        source.used = true;
        return &source;
    }

    std::array<Source, kMaxSources> sources_ = {};
    std::size_t sourceCount_ = 0;
};
