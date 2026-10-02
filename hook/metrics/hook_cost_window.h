#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace ce {

struct HookCostSnapshot {
    uint64_t calls = 0;
    uint64_t selfUs = 0;
    uint64_t wrappedUs = 0;
    uint64_t selfMaxUs = 0;
    uint64_t wrappedMaxUs = 0;
    uint64_t over500Us = 0;
    uint64_t over1ms = 0;
};

// Owned by one calling thread. Exact disjoint windows avoid mixing a callback
// with concurrent counter resets, and keep startup maxima from masking later
// sub-millisecond stalls. No contended atomics on the presenter's hot path.
template <uint64_t WindowCalls = 600>
class HookCostWindow {
public:
    std::optional<HookCostSnapshot> Observe(int64_t totalUs, int64_t forwardedUs) {
        const uint64_t self = static_cast<uint64_t>(totalUs > forwardedUs ? totalUs - forwardedUs : 0);
        const uint64_t wrapped = static_cast<uint64_t>(std::max<int64_t>(0, forwardedUs));
        ++window_.calls;
        window_.selfUs += self;
        window_.wrappedUs += wrapped;
        window_.selfMaxUs = std::max(window_.selfMaxUs, self);
        window_.wrappedMaxUs = std::max(window_.wrappedMaxUs, wrapped);
        window_.over500Us += self >= 500 ? 1u : 0u;
        window_.over1ms += self >= 1000 ? 1u : 0u;
        if (window_.calls < WindowCalls)
            return std::nullopt;
        const auto snapshot = window_;
        window_ = {};
        return snapshot;
    }

private:
    HookCostSnapshot window_;
};

}  // namespace ce
