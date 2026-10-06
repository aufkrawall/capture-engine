#pragma once

#include <cstddef>
#include <cstdint>

#include "common/ipc/display_timing_shared.h"
#include "display_timing_correlation.h"

inline bool ShouldCollectDisplayTiming(bool useScreenGrabTarget, FrameTimeSource configuredSource,
                                       bool injectVideoCaptureNeeded, bool systemLatencyRequested = false) {
    return !useScreenGrabTarget &&
           (configuredSource == FrameTimeSource::DisplayChange || injectVideoCaptureNeeded ||
            systemLatencyRequested);
}

inline bool ShouldStartOverlayDisplayTiming(bool showOverlay, bool showSystemLatency) {
    return showOverlay && showSystemLatency;
}

// The longest a present submission may wait for the flip that shows it. A
// flip queue holds a few frames, so even a frame that sat behind three others
// at 3 fps reaches the screen inside this bound; below that rate the overlay
// reads presentation timing anyway. The bound is what keeps a submission that
// never completes as a flip - a composed or copied present, which the driver
// can switch to mid-session (NVIDIA's Vulkan WSI moving to a DXGI swapchain on
// a resolution change) - from being claimed seconds later by an unrelated
// completion whose submit sequence happens to carry the same number.
inline constexpr int64_t kMaxSubmitToCompletionUs = 1'000'000;
// Input-message retrievals closer together than this are one message-pump
// loop. Well below a frame even at 1000 fps, well above the loop's own pace.
inline constexpr int64_t kInputRetrievalBurstGapUs = 500;

// A completion belongs to a submission only if it follows it and follows it by
// no more than maxAge (in the timestamps' own units; <= 0 disables the bound).
inline bool IsPlausibleSubmitCompletion(int64_t submitTimestamp, int64_t completionTimestamp, int64_t maxAge) {
    if (completionTimestamp < submitTimestamp)
        return false;
    return maxAge <= 0 || completionTimestamp - submitTimestamp <= maxAge;
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
