#pragma once

#include <cstdint>
#include <limits>

namespace ce::frame_fence {

struct RemovedTransport {
    uintptr_t fence = 0;
    uint32_t source = 0, generation = 0;
    bool removed = false;
    void Mark(uintptr_t inputFence, uint32_t inputSource, uint32_t inputGeneration) {
        fence = inputFence; source = inputSource; generation = inputGeneration; removed = true;
    }
    bool Matches(uintptr_t inputFence, uint32_t inputSource, uint32_t inputGeneration) const {
        return removed && fence == inputFence && source == inputSource && generation == inputGeneration;
    }
};

enum class Readiness { Pending, Ready, Removed };

inline Readiness Classify(uint64_t completed, uint64_t required) {
    // D3D fences report device removal with UINT64_MAX, not successful completion.
    if (completed == std::numeric_limits<uint64_t>::max()) {
        return Readiness::Removed;
    }
    return completed >= required ? Readiness::Ready : Readiness::Pending;
}

}  // namespace ce::frame_fence
