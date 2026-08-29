#pragma once

#include <cstddef>
#include <cstdint>

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

enum class DisplayCompletionKind : uint8_t {
    Unconditional,
    Sync,
    Immediate,
};

struct DisplayFrameTypeState {
    int64_t timestamp = 0;
    bool explicitFrameSeen = false;
    bool generatedFrameSeen = false;
    bool nonGeneratedFrameSeen = false;
};

inline bool IsGeneratedDisplayFrameType(uint8_t frameType) {
    return frameType == 50 || frameType == 100;
}

inline void ObserveDisplayFrameType(DisplayFrameTypeState& state, int64_t timestamp, uint8_t frameType) {
    state.timestamp = timestamp;
    state.explicitFrameSeen = true;
    if (IsGeneratedDisplayFrameType(frameType))
        state.generatedFrameSeen = true;
    else
        state.nonGeneratedFrameSeen = true;
}

inline bool ShouldPublishDisplayCompletion(DisplayCompletionKind completionKind,
                                           const DisplayFrameTypeState* state) {
    if (completionKind == DisplayCompletionKind::Unconditional || !state || !state->explicitFrameSeen)
        return true;
    if (completionKind == DisplayCompletionKind::Immediate)
        return false;
    return state->generatedFrameSeen && !state->nonGeneratedFrameSeen;
}
