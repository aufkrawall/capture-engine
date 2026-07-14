#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <array>
#include <limits>

namespace ce::rate_window {

constexpr uint64_t kDefaultBucketMs = 50;
constexpr size_t kDefaultBucketCount = 20;

inline uint64_t DivideRoundUp(uint64_t numerator, uint64_t denominator) {
    return denominator == 0 ? 0 : ((numerator + denominator - 1) / denominator);
}

inline uint32_t ScaleCountToRate(uint32_t count, uint64_t windowMs) {
    if (windowMs == 0) {
        return 0;
    }

    return static_cast<uint32_t>((static_cast<uint64_t>(count) * 1000ull + (windowMs / 2ull)) / windowMs);
}

inline uint32_t AgeCachedRate(uint32_t cachedRate, uint64_t lastSampleMs, uint64_t nowMs,
                              uint64_t sampleWindowMs) {
    if (cachedRate == 0 || sampleWindowMs == 0 || lastSampleMs == 0 || nowMs < lastSampleMs) {
        return 0;
    }
    return nowMs - lastSampleMs < sampleWindowMs ? cachedRate : 0;
}

template <uint64_t BucketMs = kDefaultBucketMs, size_t BucketCount = kDefaultBucketCount>
class SlidingRateWindow {
public:
    static_assert(BucketMs > 0, "BucketMs must be > 0");
    static_assert(BucketCount > 0, "BucketCount must be > 0");

    void Reset() {
        bucketEpochs_.fill(0);
        bucketCounts_.fill(0);
        hasSamples_ = false;
        firstSampleBucket_ = 0;
    }

    void AddSample(uint64_t nowMs, uint32_t count = 1) {
        const uint64_t bucketId = nowMs / BucketMs;
        const size_t slot = static_cast<size_t>(bucketId % BucketCount);
        if (!hasSamples_) {
            hasSamples_ = true;
            firstSampleBucket_ = bucketId;
        }
        if (bucketEpochs_[slot] != bucketId) {
            bucketEpochs_[slot] = bucketId;
            bucketCounts_[slot] = 0;
        }
        bucketCounts_[slot] += count;
    }

    uint32_t RatePerSecond(uint64_t nowMs, uint64_t windowMs = BucketMs * BucketCount) const {
        return ScaleCountToRate(SumWindow(nowMs, windowMs), windowMs);
    }

    uint32_t SumWindow(uint64_t nowMs, uint64_t windowMs) const {
        if (windowMs == 0) {
            return 0;
        }

        const uint64_t currentBucket = nowMs / BucketMs;
        const uint64_t bucketSpan = std::max<uint64_t>(1, DivideRoundUp(windowMs, BucketMs));
        uint32_t sum = 0;
        for (uint64_t i = 0; i < bucketSpan; ++i) {
            if (currentBucket < i) {
                break;
            }
            sum += BucketCountForEpoch(currentBucket - i);
        }
        return sum;
    }

    uint32_t MinRatePerSecond(uint64_t nowMs, uint64_t sampleWindowMs, uint64_t lookbackMs) const {
        if (!hasSamples_ || sampleWindowMs == 0 || lookbackMs == 0) {
            return 0;
        }

        const uint64_t currentBucket = nowMs / BucketMs;
        const uint64_t sampleBucketSpan = std::max<uint64_t>(1, DivideRoundUp(sampleWindowMs, BucketMs));
        const uint64_t lookbackBucketSpan = std::max<uint64_t>(sampleBucketSpan, DivideRoundUp(lookbackMs, BucketMs));
        const uint64_t historyFloor = currentBucket >= (BucketCount - 1) ? currentBucket - (BucketCount - 1) : 0;
        const uint64_t observedFloor = std::max(firstSampleBucket_, historyFloor);
        const uint64_t searchStart = observedFloor + sampleBucketSpan - 1;
        const uint64_t searchFloor =
            currentBucket >= (lookbackBucketSpan - 1) ? currentBucket - (lookbackBucketSpan - 1) : 0;
        const uint64_t firstEndBucket = std::max(searchStart, searchFloor + sampleBucketSpan - 1);
        if (firstEndBucket > currentBucket) {
            return RatePerSecond(nowMs, sampleWindowMs);
        }

        uint32_t minCount = UINT32_MAX;
        for (uint64_t endBucket = firstEndBucket; endBucket <= currentBucket; ++endBucket) {
            uint32_t windowCount = 0;
            for (uint64_t i = 0; i < sampleBucketSpan; ++i) {
                windowCount += BucketCountForEpoch(endBucket - i);
            }
            minCount = std::min(minCount, windowCount);
        }

        return minCount == UINT32_MAX ? 0 : ScaleCountToRate(minCount, sampleWindowMs);
    }

private:
    uint32_t BucketCountForEpoch(uint64_t epoch) const {
        const size_t slot = static_cast<size_t>(epoch % BucketCount);
        return bucketEpochs_[slot] == epoch ? bucketCounts_[slot] : 0u;
    }

    std::array<uint64_t, BucketCount> bucketEpochs_{};
    std::array<uint32_t, BucketCount> bucketCounts_{};
    bool hasSamples_ = false;
    uint64_t firstSampleBucket_ = 0;
};

}  // namespace ce::rate_window
