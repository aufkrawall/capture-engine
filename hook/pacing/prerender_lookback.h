#pragma once

// Which earlier present a `cpu_prerender_limit` of 1-6 waits for, for marker streams that count
// presents: the marker signaled after the n-th present (1-based) carries the value n. Before the
// next present the GPU must have finished the present `limit` back, so with `signaled` markers
// out the wait is for value signaled + 1 - limit (none while fewer presents exist). The D3D11
// fence ring (hook/d3d11/dx11_hook_prerender.cpp) uses it; it states the same lookback as the
// event-query, DX12, Vulkan and OpenGL rings.

#include <algorithm>
#include <cstdint>

namespace ce::prerender_lookback {

// The configured depth as a lookback: integer semantics, 1-6 (graphics-overrides-and-frame-pacing.md).
constexpr int ClampLookback(int limit) {
    return std::clamp(limit, 1, 6);
}

// The marker value to wait for before the next present, 0 when there is none yet.
constexpr uint64_t WaitTarget(uint64_t signaled, int limit) {
    const auto lookback = static_cast<uint64_t>(ClampLookback(limit));
    return signaled + 1 > lookback ? signaled + 1 - lookback : 0;
}

}  // namespace ce::prerender_lookback
