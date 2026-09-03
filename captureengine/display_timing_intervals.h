#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

// Interval statistics over one health window of a timestamp series.
//
// A published-sample count that matches the present rate proves nothing about
// the timestamp *values*: the 2026-08-30 regression published exactly one
// sample per displayed frame, with no drops and no regressions, while every
// value was the flip-programming time instead of the screen time. The shape of
// the series is the missing evidence. A two-phase sawtooth keeps the mean and
// the count exactly right and shows up only as a large jaggedness (the mean
// absolute difference between neighbouring intervals) and a split p1/p99 pair.
class DisplayIntervalStats {
public:
    static constexpr int64_t kBucketWidthUs = 100;
    // 0 .. 51.2 ms; the last bucket saturates, which only understates a
    // percentile that is already far past anything a display can produce.
    static constexpr std::size_t kBucketCount = 512;

    // A duplicate or regressed timestamp must not move the anchor backwards:
    // the next real interval would then be reported longer than it was, which
    // is the one failure mode a jitter statistic can least afford.
    void Observe(int64_t timestampUs) {
        if (timestampUs <= 0)
            return;
        const int64_t previous = lastTimestampUs_;
        if (previous > 0 && timestampUs <= previous)
            return;
        lastTimestampUs_ = timestampUs;
        if (previous > 0)
            AddInterval(timestampUs - previous);
    }

    // Starts a new window without treating the boundary as a gap: the next
    // interval is still measured from the last timestamp of the old window.
    void StartWindow() {
        count_ = 0;
        sumUs_ = 0;
        sumSquaresUs_ = 0.0;
        minUs_ = 0;
        maxUs_ = 0;
        jaggednessSumUs_ = 0;
        jaggednessCount_ = 0;
        hasPreviousInterval_ = false;
        buckets_.fill(0);
    }

    uint64_t count() const { return count_; }
    int64_t minUs() const { return minUs_; }
    int64_t maxUs() const { return maxUs_; }
    int64_t meanUs() const { return count_ != 0 ? sumUs_ / static_cast<int64_t>(count_) : 0; }

    int64_t stdDevUs() const {
        if (count_ < 2)
            return 0;
        const double mean = static_cast<double>(sumUs_) / static_cast<double>(count_);
        const double variance = std::max(0.0, sumSquaresUs_ / static_cast<double>(count_) - mean * mean);
        return static_cast<int64_t>(std::sqrt(variance));
    }

    int64_t jaggednessUs() const {
        return jaggednessCount_ != 0 ? jaggednessSumUs_ / static_cast<int64_t>(jaggednessCount_) : 0;
    }

    // Reports the bucket's lower edge so a percentile never overstates the
    // interval it stands for.
    int64_t percentileUs(double quantile) const {
        if (count_ == 0)
            return 0;
        // Nearest-rank: the smallest value at or above the requested share of
        // the window, so p99 of a short window still names its worst interval.
        const double rank = std::ceil(quantile * static_cast<double>(count_));
        uint64_t target = rank < 1.0 ? 1u : static_cast<uint64_t>(rank);
        target = std::min<uint64_t>(target, count_);
        uint64_t seen = 0;
        for (std::size_t i = 0; i < kBucketCount; ++i) {
            seen += buckets_[i];
            if (seen >= target)
                return static_cast<int64_t>(i) * kBucketWidthUs;
        }
        return static_cast<int64_t>(kBucketCount - 1) * kBucketWidthUs;
    }

private:
    void AddInterval(int64_t interval) {
        ++count_;
        sumUs_ += interval;
        sumSquaresUs_ += static_cast<double>(interval) * static_cast<double>(interval);
        minUs_ = count_ == 1 ? interval : std::min(minUs_, interval);
        maxUs_ = std::max(maxUs_, interval);
        const int64_t bucket =
            std::min<int64_t>(interval / kBucketWidthUs, static_cast<int64_t>(kBucketCount) - 1);
        ++buckets_[static_cast<std::size_t>(bucket)];
        if (hasPreviousInterval_) {
            jaggednessSumUs_ += std::llabs(interval - previousIntervalUs_);
            ++jaggednessCount_;
        }
        previousIntervalUs_ = interval;
        hasPreviousInterval_ = true;
    }

    std::array<uint32_t, kBucketCount> buckets_ = {};
    int64_t lastTimestampUs_ = 0;
    int64_t previousIntervalUs_ = 0;
    int64_t sumUs_ = 0;
    double sumSquaresUs_ = 0.0;
    int64_t minUs_ = 0;
    int64_t maxUs_ = 0;
    int64_t jaggednessSumUs_ = 0;
    uint64_t jaggednessCount_ = 0;
    uint64_t count_ = 0;
    bool hasPreviousInterval_ = false;
};
