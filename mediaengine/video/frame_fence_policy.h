#pragma once

#include <cstdint>
#include <limits>

namespace ce::frame_fence {

enum class Readiness { Pending, Ready, Removed };

inline Readiness Classify(uint64_t completed, uint64_t required) {
    // D3D fences report device removal with UINT64_MAX, not successful completion.
    if (completed == std::numeric_limits<uint64_t>::max()) {
        return Readiness::Removed;
    }
    return completed >= required ? Readiness::Ready : Readiness::Pending;
}

}  // namespace ce::frame_fence
