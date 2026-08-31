#pragma once

#include <cstddef>
#include <cstdint>

#include "../common/display_timing_shared.h"
#include "display_timing_correlation.h"

inline bool ShouldCollectDisplayTiming(bool useScreenGrabTarget, FrameTimeSource configuredSource,
                                       bool injectVideoCaptureNeeded) {
    return !useScreenGrabTarget &&
           (configuredSource == FrameTimeSource::DisplayChange || injectVideoCaptureNeeded);
}

// No outstanding runtime present of that process is waiting for a submission.
inline constexpr std::size_t kNoPendingDisplayPresent = static_cast<std::size_t>(-1);

// A runtime present and the kernel present submission that carries it are not
// guaranteed to share a thread: D3D11 and D3D12 hand the packet to a runtime
// worker thread of the same process, so an exact-thread-only rule associates
// nothing at all for those APIs. Prefer the exact thread when it does match so
// interleaved presents from several render threads keep their own order, and
// otherwise take that process's oldest outstanding present, which is the one
// the runtime is submitting.
inline std::size_t SelectDisplaySubmissionPresent(const uint32_t* pendingThreadIds, std::size_t pendingCount,
                                                  uint32_t submittingThreadId) {
    if (pendingCount == 0 || !pendingThreadIds)
        return kNoPendingDisplayPresent;
    for (std::size_t i = 0; i < pendingCount; ++i) {
        if (pendingThreadIds[i] == submittingThreadId)
            return i;
    }
    return 0;
}
