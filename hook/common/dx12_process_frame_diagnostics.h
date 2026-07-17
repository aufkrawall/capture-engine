#pragma once

#include <algorithm>
#include <cstdint>

namespace ce::dx12_process_frame_diagnostics {

struct ConcurrentActivitySnapshot {
    uint64_t generation = 0;
    uint32_t activeCalls = 0;
};

inline bool DidActivityOverlap(const ConcurrentActivitySnapshot& before,
                               const ConcurrentActivitySnapshot& after) {
    return before.activeCalls != 0 || after.activeCalls != 0 || before.generation != after.generation;
}

struct StageTimings {
    int64_t totalUs = 0;
    int64_t innerUs = 0;
    int64_t captureUs = 0;
    int64_t screenshotUs = 0;
    int64_t commandQueueLockWaitUs = 0;
    int64_t overlayAcquireUs = 0;
    int64_t overlayRecordUs = 0;
    int64_t overlaySubmitUs = 0;
    int64_t overlayPostSubmitUs = 0;
    bool innerCalled = false;
    bool overlayBreakdownValid = false;
    bool reentrantInnerSkipped = false;
};

struct Breakdown {
    int64_t externalUs = 0;
    int64_t innerOtherUs = 0;
    int64_t overlayUs = 0;
};

inline Breakdown ComputeBreakdown(const StageTimings& timings) {
    Breakdown result;
    result.overlayUs = std::max<int64_t>(0, timings.overlayAcquireUs) +
                       std::max<int64_t>(0, timings.overlayRecordUs) +
                       std::max<int64_t>(0, timings.overlaySubmitUs) +
                       std::max<int64_t>(0, timings.overlayPostSubmitUs);
    result.innerOtherUs = std::max<int64_t>(0, timings.innerUs - timings.captureUs - result.overlayUs);
    result.externalUs = std::max<int64_t>(0, timings.totalUs - timings.innerUs - timings.screenshotUs);
    return result;
}

}  // namespace ce::dx12_process_frame_diagnostics
