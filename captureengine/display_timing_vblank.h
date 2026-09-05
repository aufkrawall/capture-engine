#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// Diagnostic summary of reported vertical blanks. A periodic blank stream
// does not identify which blank displayed a particular flip. Never snap,
// extrapolate, or assign display-change timestamps from this estimate.
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

    // The refresh period the clock is running on, or 0 while it has no grid.
    int64_t PeriodUs(uint32_t displaySource) const {
        const Source* source = Find(displaySource);
        return source ? source->grid.periodUs : 0;
    }

    uint64_t observedBlanks(uint32_t displaySource) const {
        const Source* source = Find(displaySource);
        return source ? source->observed : 0;
    }

    bool HasPeriodicCadence(uint32_t displaySource) const {
        const Source* source = Find(displaySource);
        return source && source->grid.valid();
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
        BlankGrid grid;
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
        const int64_t tolerance = candidate / 5;
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
        // it, which averages reporting jitter in this diagnostic estimate.
        // This estimate never changes a frame timestamp.
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
