#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ce {

enum class HookThreadStage : std::size_t {
    kConfig = 0,
    kDeferredRelease,
    kHookScan,
    kPresentHooks,
    kRetirement,
    kUE5,
    kIpcAndLifecycle,
    kCount,
};

struct HookThreadStageCost {
    uint64_t calls = 0;
    uint64_t totalUs = 0;
    uint64_t maxUs = 0;
    uint64_t over1ms = 0;
};

struct HookThreadStageSnapshot {
    uint64_t passes = 0;
    uint64_t totalUs = 0;
    uint64_t maxUs = 0;
    uint64_t over1ms = 0;
    std::array<HookThreadStageCost, static_cast<std::size_t>(HookThreadStage::kCount)> stages = {};
};

// Single-hook-thread accumulator. Optional stages count only invocations, so a
// once-per-second 100 ms module scan is not diluted by nine zero-cost passes.
template <uint64_t WindowPasses = 100>
class HookThreadStageCostWindow {
public:
    void Observe(HookThreadStage stage, int64_t durationUs) {
        if (stage >= HookThreadStage::kCount)
            return;
        auto& cost = window_.stages[static_cast<std::size_t>(stage)];
        const uint64_t duration = static_cast<uint64_t>(std::max<int64_t>(0, durationUs));
        ++cost.calls;
        cost.totalUs += duration;
        cost.maxUs = std::max(cost.maxUs, duration);
        cost.over1ms += duration >= 1000 ? 1u : 0u;
    }

    std::optional<HookThreadStageSnapshot> FinishPass(int64_t durationUs) {
        const uint64_t duration = static_cast<uint64_t>(std::max<int64_t>(0, durationUs));
        ++window_.passes;
        window_.totalUs += duration;
        window_.maxUs = std::max(window_.maxUs, duration);
        window_.over1ms += duration >= 1000 ? 1u : 0u;
        if (window_.passes < WindowPasses)
            return std::nullopt;
        const auto snapshot = window_;
        window_ = {};
        return snapshot;
    }

private:
    HookThreadStageSnapshot window_;
};

inline uint64_t AverageHookThreadStageUs(const HookThreadStageCost& cost) {
    return cost.calls != 0 ? cost.totalUs / cost.calls : 0;
}

}  // namespace ce
