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
        source->periodUs = MeasurePeriod(*source);
    }

    // The blank this completion's frame reaches the screen at, or 0 when the
    // clock cannot answer - the caller then keeps the uncorrected timestamp
    // rather than inventing one. The answer is 0 for as long as the blank is
    // still in the future, which is the normal case at the moment a flip
    // completes; the publication reorder window is what makes waiting free.
    int64_t Snap(uint32_t displaySource, int64_t timestamp) const {
        const Source* source = Find(displaySource);
        if (!source || source->periodUs <= 0 || timestamp <= 0)
            return 0;
        const int64_t tolerance = (source->periodUs * kAtBlankNumerator) / kAtBlankDenominator;
        int64_t previous = 0;
        int64_t next = 0;
        for (std::size_t i = 0; i < source->count; ++i) {
            const int64_t blank = source->blanks[i];
            if (blank <= timestamp)
                previous = std::max(previous, blank);
            else if (next == 0 || blank < next)
                next = blank;
        }
        if (previous != 0 && timestamp - previous <= tolerance)
            return previous;
        return next;
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
    // the clock cannot answer yet, leaving the completion unresolved.
    int64_t Claim(uint32_t displaySource, int64_t timestamp) {
        const int64_t natural = Snap(displaySource, timestamp);
        Source* source = FindOrCreate(displaySource);
        if (natural == 0 || !source)
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
            forced = FirstBlankAfter(*source, forced);
            if (forced == 0)
                return 0;  // The blank it would move to has not happened yet.
            if (forced > source->lastClaimed) {
                source->lastClaimed = forced;
                return forced;
            }
        }
        return natural;
    }

    int64_t PeriodUs(uint32_t displaySource) const {
        const Source* source = Find(displaySource);
        return source ? source->periodUs : 0;
    }

    uint64_t observedBlanks(uint32_t displaySource) const {
        const Source* source = Find(displaySource);
        return source ? source->observed : 0;
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
        int64_t periodUs = 0;
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

    static int64_t FirstBlankAfter(const Source& source, int64_t timestamp) {
        int64_t found = 0;
        for (std::size_t i = 0; i < source.count; ++i) {
            const int64_t blank = source.blanks[i];
            if (blank > timestamp && (found == 0 || blank < found))
                found = blank;
        }
        return found;
    }

    // The median gap between retained blanks. A mean would be dragged by any
    // idle stretch or mode change still inside the window, and the tolerance
    // this feeds has to describe the display as it is refreshing now.
    static int64_t MeasurePeriod(const Source& source) {
        if (source.count < 2)
            return 0;
        std::array<int64_t, kBlanksPerSource> gaps = {};
        const std::size_t gapCount = source.count - 1;
        for (std::size_t i = 0; i < gapCount; ++i)
            gaps[i] = source.At(i + 1) - source.At(i);
        const auto middle = gaps.begin() + static_cast<std::ptrdiff_t>(gapCount / 2);
        std::nth_element(gaps.begin(), middle, gaps.begin() + static_cast<std::ptrdiff_t>(gapCount));
        return *middle;
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
