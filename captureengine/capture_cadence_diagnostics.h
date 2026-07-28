#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

struct InjectFrameLineage {
    uint32_t frameIndex = 0;
    int32_t textureIndex = -1;
    uint64_t fenceValue = 0;
    uint32_t ringIndex = 0;
    int64_t timestamp = 0;
    int64_t enqueueQpc = 0;
    uint32_t deferCount = 0;

    bool IsValid() const {
        return frameIndex != 0 || fenceValue != 0 || timestamp != 0;
    }
};

struct CadenceHealthCounters {
    uint64_t frameAgeAccumUs = 0;
    uint32_t frameAgeSamples = 0;
    uint32_t frameAgeMaxUs = 0;
    uint64_t selectionErrorAccumUs = 0;
    int64_t selectionErrorSignedAccumUs = 0;
    uint32_t selectionErrorSamples = 0;
    uint32_t selectionErrorMaxUs = 0;
    uint32_t selectionEarlyMaxUs = 0;
    uint32_t selectionLateMaxUs = 0;
    uint32_t consecutiveDeferredFrames = 0;
    uint32_t maxConsecutiveDeferredFrames = 0;
    uint32_t consecutiveDuplicateFrames = 0;
    uint32_t maxConsecutiveDuplicateFrames = 0;
    uint32_t liveTickEmitCount = 0;
    uint32_t liveTickUniqueCount = 0;
    uint32_t liveTickDuplicateCount = 0;
    uint32_t liveTickMissCount = 0;
    uint64_t outputScheduleErrorAccumUs = 0;
    int64_t outputScheduleErrorSignedAccumUs = 0;
    uint32_t outputScheduleErrorSamples = 0;
    uint32_t outputScheduleErrorMaxUs = 0;
    uint32_t outputScheduleEarlyMaxUs = 0;
    uint32_t outputScheduleLateMaxUs = 0;

    // Hold-time histogram: how many output ticks each unique source frame was
    // shown for. holdHist[0]=1 tick through holdHist[5]=6+ ticks.
    static constexpr uint32_t kHoldHistBuckets = 6;
    uint32_t holdHist[kHoldHistBuckets] = {};
    uint32_t holdTicksRunning = 0;

    void CommitHoldRun() {
        if (holdTicksRunning > 0) {
            const uint32_t bucket = std::min(holdTicksRunning, kHoldHistBuckets) - 1;
            holdHist[bucket]++;
            holdTicksRunning = 0;
        }
    }

    void RecordSelectionError(int64_t signedErrorUs) {
        RecordSignedError(signedErrorUs, selectionErrorAccumUs, selectionErrorSignedAccumUs, selectionErrorSamples,
                          selectionErrorMaxUs, selectionEarlyMaxUs, selectionLateMaxUs);
    }

    void RecordOutputScheduleError(int64_t signedErrorUs) {
        RecordSignedError(signedErrorUs, outputScheduleErrorAccumUs, outputScheduleErrorSignedAccumUs,
                          outputScheduleErrorSamples, outputScheduleErrorMaxUs, outputScheduleEarlyMaxUs,
                          outputScheduleLateMaxUs);
    }

    uint32_t srcFpsX100 = 0;
    uint32_t srcJitterUs = 0;
    uint32_t encCycleAvgUs = 0;
    uint32_t encCycleMaxUs = 0;
    uint32_t dupTimestampCount = 0;

    void Reset() {
        *this = {};
    }

private:
    static uint32_t SaturateToUint32(uint64_t value) {
        return value > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                            : static_cast<uint32_t>(value);
    }

    static void RecordSignedError(int64_t signedErrorUs, uint64_t& absoluteAccumUs, int64_t& signedAccumUs,
                                  uint32_t& samples, uint32_t& maxUs, uint32_t& earlyMaxUs, uint32_t& lateMaxUs) {
        const uint64_t absoluteUs = static_cast<uint64_t>(signedErrorUs >= 0 ? signedErrorUs : -signedErrorUs);
        absoluteAccumUs += absoluteUs;
        signedAccumUs += signedErrorUs;
        ++samples;
        maxUs = std::max(maxUs, SaturateToUint32(absoluteUs));
        uint32_t& directionalMaxUs = signedErrorUs < 0 ? earlyMaxUs : lateMaxUs;
        directionalMaxUs = std::max(directionalMaxUs, SaturateToUint32(absoluteUs));
    }
};
