#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../common/display_timing_shared.h"
#include "display_timing_correlation.h"
#include "display_timing_vblank.h"

inline bool ShouldCollectDisplayTiming(bool useScreenGrabTarget, FrameTimeSource configuredSource,
                                       bool injectVideoCaptureNeeded, bool systemLatencyRequested = false) {
    return !useScreenGrabTarget &&
           (configuredSource == FrameTimeSource::DisplayChange || injectVideoCaptureNeeded ||
            systemLatencyRequested);
}

inline bool ShouldStartOverlayDisplayTiming(bool showOverlay, bool showSystemLatency) {
    return showOverlay && showSystemLatency;
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

// Replaces each vsync-deferred completion's timestamp with the vertical blank
// its frame actually reaches the screen at. Entries the clock cannot answer for
// keep the uncorrected timestamp and stay unresolved, so a machine or present
// mode with no blank stream behaves exactly as it did before. The queue must
// already be ordered by timestamp: blanks are claimed in display order.
// Returns how many timestamps the rounding moved.
inline uint64_t ResolveDeferredScreenTimes(std::vector<PendingTimestamp>& pending, VerticalBlankClock& blanks) {
    uint64_t adjusted = 0;
    for (auto& entry : pending) {
        if (entry.completionKind != DisplayCompletionKind::Sync || entry.screenTimeResolved)
            continue;
        const int64_t blank = blanks.Claim(entry.displaySource, entry.timestamp);
        if (blank == 0)
            continue;  // The blank has not happened yet, or there is no clock.
        if (blank != entry.timestamp)
            ++adjusted;
        entry.timestamp = blank;
        entry.screenTimeResolved = true;
    }
    return adjusted;
}
