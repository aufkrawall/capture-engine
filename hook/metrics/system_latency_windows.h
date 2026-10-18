#pragma once

// Bounded history containers shared by the PC-latency tracker. They are kept
// separate from the correlator itself so the tracker file stays focused on the
// causal chain (frame begin -> present -> screen) rather than on storage.

#include "system_latency_types.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace ce::system_latency {

template <size_t Capacity>
class ValueRing {
public:
    void Push(int64_t value) {
        if (count_ < Capacity) {
            values_[(start_ + count_) % Capacity] = value;
            ++count_;
        } else {
            values_[start_] = value;
            start_ = (start_ + 1) % Capacity;
        }
    }

    int64_t At(size_t index) const {
        return values_[(start_ + index) % Capacity];
    }
    int64_t Back() const {
        return At(count_ - 1);
    }
    size_t Size() const {
        return count_;
    }
    bool Empty() const {
        return count_ == 0;
    }
    void Clear() {
        start_ = 0;
        count_ = 0;
    }

private:
    std::array<int64_t, Capacity> values_{};
    size_t start_ = 0;
    size_t count_ = 0;
};

template <size_t Capacity>
inline int64_t MedianRing(const ValueRing<Capacity>& values) {
    if (values.Empty())
        return 0;
    std::array<int64_t, Capacity> sorted{};
    for (size_t i = 0; i < values.Size(); ++i)
        sorted[i] = values.At(i);
    std::sort(sorted.begin(), sorted.begin() + values.Size());
    return sorted[values.Size() / 2];
}

// Hitches more than this many median intervals long are not part of a cadence.
inline constexpr int64_t kCadenceOutlierMedianFactor = 4;

// The mean period of a cadence ring: what a rate is. A median picks one side of
// an alternating cadence - FSR frame generation at a 120 fps cap displayed
// 6.5/10.1 ms pairs (Talos, session 20261018_191159), whose median of ~9.8 ms
// published 101 displayed fps for 120 real ones and an output ratio of 1.69 for
// 2x. The mean is the only unbiased period of a periodic pattern, so only
// intervals far beyond the median (hitches, which a median exists to ignore)
// are left out; ordinary pacing patterns, 3x/4x groups included, stay whole.
template <size_t Capacity>
inline int64_t CadenceIntervalRing(const ValueRing<Capacity>& values) {
    const int64_t median = MedianRing(values);
    if (median <= 0)
        return median;
    const int64_t bound = median * kCadenceOutlierMedianFactor;
    int64_t sum = 0;
    size_t count = 0;
    for (size_t i = 0; i < values.Size(); ++i) {
        const int64_t value = values.At(i);
        if (value <= bound) {
            sum += value;
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<int64_t>(count) : median;
}

template <size_t Capacity>
inline int64_t MaximumRing(const ValueRing<Capacity>& values) {
    int64_t maximum = 0;
    for (size_t i = 0; i < values.Size(); ++i)
        maximum = (std::max)(maximum, values.At(i));
    return maximum;
}

template <size_t Capacity>
inline int64_t MedianRingWithCandidate(const ValueRing<Capacity>& values, int64_t candidate) {
    std::array<int64_t, Capacity + 1> sorted{};
    const size_t first = values.Size() == Capacity ? 1 : 0;
    size_t count = 0;
    for (size_t i = first; i < values.Size(); ++i)
        sorted[count++] = values.At(i);
    sorted[count++] = candidate;
    std::sort(sorted.begin(), sorted.begin() + count);
    return sorted[count / 2];
}

// Rolling window of finished per-frame latency measurements.
//
// A single 250 ms telemetry poll can contribute an entire window's worth of
// frames, so one mis-associated frame must not be able to move the published
// number: the summary is a symmetrically trimmed mean and the distribution is
// retained so diagnostics can show what was actually thrown away.
class SampleWindow {
public:
    static constexpr size_t kCapacity = 32;

    void Add(float milliseconds, int64_t sampleTimeUs) {
        if (lastSampleTimeUs_ > 0 && sampleTimeUs - lastSampleTimeUs_ > kSampleFreshnessUs)
            Clear();
        values_[writeIndex_] = milliseconds;
        writeIndex_ = (writeIndex_ + 1) % values_.size();
        count_ = (std::min)(count_ + 1, values_.size());
        lastSampleTimeUs_ = sampleTimeUs;
    }

    bool IsFresh(int64_t currentQpcUs) const {
        return count_ > 0 && lastSampleTimeUs_ > 0 && currentQpcUs >= lastSampleTimeUs_ &&
               currentQpcUs - lastSampleTimeUs_ <= kSampleFreshnessUs;
    }

    Snapshot MakeSnapshot(Source source) const {
        std::array<float, kCapacity> sorted{};
        const size_t count = SortedValues(sorted);
        if (count == 0)
            return {};

        // Use interquartile trimming once the window carries enough frames:
        // rejects transition hitch spikes and outlier pairing jumps without
        // distorting steady-state latency.
        const size_t trim = count >= 8 ? count / 4 : (count >= 4 ? 1 : 0);
        const size_t retained = count - 2 * trim;
        float sum = 0.0f;
        for (size_t i = trim; i < count - trim; ++i)
            sum += sorted[i];

        Snapshot snapshot{};
        snapshot.milliseconds = sum / static_cast<float>(retained);
        snapshot.source = source;
        snapshot.sampleCount = static_cast<uint32_t>(count);
        snapshot.medianMilliseconds = sorted[count / 2];
        snapshot.minimumMilliseconds = sorted[0];
        snapshot.maximumMilliseconds = sorted[count - 1];
        snapshot.valid = true;
        return snapshot;
    }

    void Clear() {
        values_.fill(0.0f);
        writeIndex_ = 0;
        count_ = 0;
        lastSampleTimeUs_ = 0;
    }

    size_t Size() const {
        return count_;
    }

private:
    size_t SortedValues(std::array<float, kCapacity>& sorted) const {
        for (size_t i = 0; i < count_; ++i)
            sorted[i] = values_[i];
        std::sort(sorted.begin(), sorted.begin() + count_);
        return count_;
    }

    std::array<float, kCapacity> values_{};
    size_t writeIndex_ = 0;
    size_t count_ = 0;
    int64_t lastSampleTimeUs_ = 0;
};

}  // namespace ce::system_latency
