#pragma once

#include <cstdint>

namespace ce::dxgi_video_memory_log_policy {

inline constexpr uint64_t kFirstVideoMemoryQueryLogs = 8;
inline constexpr uint64_t kSuccessPeriodicCallInterval = 4096;
inline constexpr uint64_t kFailurePeriodicCallInterval = 256;
inline constexpr uint64_t kPeriodicLogIntervalMs = 1000;
inline constexpr uint64_t kMeaningfulBudgetDeltaBytes = 16ull * 1024ull * 1024ull;

inline bool UnsignedDeltaAtLeast(uint64_t a, uint64_t b, uint64_t threshold) {
    return a >= b ? (a - b) >= threshold : (b - a) >= threshold;
}

inline bool HasMeaningfulVideoMemoryDelta(uint64_t previousBudget, uint64_t previousUsage,
                                          uint64_t previousAvailableForReservation, uint64_t budget, uint64_t usage,
                                          uint64_t availableForReservation) {
    return UnsignedDeltaAtLeast(previousBudget, budget, kMeaningfulBudgetDeltaBytes) ||
           UnsignedDeltaAtLeast(previousUsage, usage, kMeaningfulBudgetDeltaBytes) ||
           UnsignedDeltaAtLeast(previousAvailableForReservation, availableForReservation, kMeaningfulBudgetDeltaBytes);
}

inline bool ShouldLogVideoMemoryQuery(uint64_t callCount, uint64_t nowMs, uint64_t lastLogMs, bool valueChanged,
                                      bool failed) {
    if (callCount <= kFirstVideoMemoryQueryLogs) {
        return true;
    }
    if (valueChanged) {
        return true;
    }
    if (lastLogMs == 0 || nowMs < lastLogMs || nowMs - lastLogMs >= kPeriodicLogIntervalMs) {
        return true;
    }

    const uint64_t periodicCallInterval = failed ? kFailurePeriodicCallInterval : kSuccessPeriodicCallInterval;
    return periodicCallInterval != 0 && (callCount % periodicCallInterval) == 0;
}

}  // namespace ce::dxgi_video_memory_log_policy
