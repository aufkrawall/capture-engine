#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ce::ngx_lifecycle {

enum class RecordResult { kInvalid, kInserted, kUpdated, kFull };

struct EvaluationResult {
    bool found = false;
    bool firstEvaluation = false;
    int feature = -1;
};

struct RemovalResult {
    bool found = false;
    bool wasEvaluated = false;
    int feature = -1;
};

constexpr bool IsSuccessfulResult(uint32_t result) noexcept {
    return (result & 0xFFF00000u) != 0xBAD00000u;
}

// Resolve the DLSS frame-generation output multiplier (2x/3x/4x) for an NVNGX
// CreateFeature call. Feature IDs 9/0xB (legacy FG) and 18 (MFG) all carry the
// configured multiplier in the FrameGenerationMultiplier parameter; the legacy
// FG branch must not hardcode 2x. Late injection depends on this because the
// Streamline GetState path that keeps the multiplier fresh at startup is not
// hooked when sl.dlssg was already loaded before hook installation (session
// 20260811_222500: Talos configured for 4x MFG but the overlay showed DLSS
// 2x). Precedence mirrors the MFG branch: a configured override wins over the
// 2x default. An explicit config is an override and therefore wins over a
// valid parameter value (2/3/4); without a config, the observed value wins.
// paramMultiplier must be 0 when the parameter is absent or unreadable.
inline int ResolveNVNGXFrameGenerationMultiplier(int configuredMultiplier, int paramMultiplier) noexcept {
    if (configuredMultiplier >= 2 && configuredMultiplier <= 4)
        return configuredMultiplier;
    if (paramMultiplier >= 2 && paramMultiplier <= 4)
        return paramMultiplier;
    return 2;  // Standard FG default is 2x
}

// Modern DLSS-G runtimes expose the number of generated frames between real
// frames (1/2/3) while older runtimes expose the output multiplier (2/3/4).
// Prefer the modern value when both are present: an injected/stale legacy key
// must not mask what the active runtime actually consumes.
inline int ResolveNVNGXObservedFrameGenerationMultiplier(int modernGeneratedFrames,
                                                         int legacyMultiplier) noexcept {
    if (modernGeneratedFrames >= 1 && modernGeneratedFrames <= 3) {
        return modernGeneratedFrames + 1;
    }
    if (legacyMultiplier >= 2 && legacyMultiplier <= 4) {
        return legacyMultiplier;
    }
    return 0;
}

template <std::size_t Capacity>
class FeatureHandleRegistry {
 public:
    RecordResult RecordCreated(void* handle, int feature) noexcept {
        if (!handle || handle == ClaimedHandle())
            return RecordResult::kInvalid;

        for (Slot& slot : slots_) {
            void* current = slot.handle.load(std::memory_order_acquire);
            if (current == handle) {
                slot.feature.store(feature, std::memory_order_relaxed);
                slot.evaluated.store(false, std::memory_order_release);
                return RecordResult::kUpdated;
            }
            if (current != nullptr)
                continue;

            void* expected = nullptr;
            if (!slot.handle.compare_exchange_strong(expected, ClaimedHandle(), std::memory_order_acq_rel)) {
                if (expected == handle) {
                    slot.feature.store(feature, std::memory_order_relaxed);
                    slot.evaluated.store(false, std::memory_order_release);
                    return RecordResult::kUpdated;
                }
                continue;
            }
            slot.feature.store(feature, std::memory_order_relaxed);
            slot.evaluated.store(false, std::memory_order_relaxed);
            slot.handle.store(handle, std::memory_order_release);
            return RecordResult::kInserted;
        }
        return RecordResult::kFull;
    }

    int FindFeature(void* handle) const noexcept {
        if (!handle)
            return -1;
        for (const Slot& slot : slots_) {
            if (slot.handle.load(std::memory_order_acquire) != handle)
                continue;
            const int feature = slot.feature.load(std::memory_order_relaxed);
            if (slot.handle.load(std::memory_order_acquire) == handle)
                return feature;
        }
        return -1;
    }

    EvaluationResult MarkEvaluated(void* handle) noexcept {
        if (!handle)
            return {};
        for (Slot& slot : slots_) {
            if (slot.handle.load(std::memory_order_acquire) != handle)
                continue;
            const int feature = slot.feature.load(std::memory_order_relaxed);
            const bool alreadyEvaluated = slot.evaluated.exchange(true, std::memory_order_acq_rel);
            if (slot.handle.load(std::memory_order_acquire) == handle)
                return {true, !alreadyEvaluated, feature};
        }
        return {};
    }

    RemovalResult Remove(void* handle) noexcept {
        if (!handle)
            return {};
        for (Slot& slot : slots_) {
            void* expected = handle;
            if (!slot.handle.compare_exchange_strong(expected, ClaimedHandle(), std::memory_order_acq_rel))
                continue;
            const int feature = slot.feature.load(std::memory_order_relaxed);
            const bool evaluated = slot.evaluated.load(std::memory_order_acquire);
            slot.feature.store(-1, std::memory_order_relaxed);
            slot.evaluated.store(false, std::memory_order_relaxed);
            slot.handle.store(nullptr, std::memory_order_release);
            return {true, evaluated, feature};
        }
        return {};
    }

    bool HasEvaluatedFeature(int feature) const noexcept {
        for (const Slot& slot : slots_) {
            void* handle = slot.handle.load(std::memory_order_acquire);
            if (!handle || handle == ClaimedHandle())
                continue;
            if (slot.feature.load(std::memory_order_relaxed) == feature &&
                slot.evaluated.load(std::memory_order_acquire) &&
                slot.handle.load(std::memory_order_acquire) == handle) {
                return true;
            }
        }
        return false;
    }

 private:
    static void* ClaimedHandle() noexcept { return reinterpret_cast<void*>(static_cast<uintptr_t>(1)); }

    struct Slot {
        std::atomic<void*> handle{nullptr};
        std::atomic<int> feature{-1};
        std::atomic<bool> evaluated{false};
    };

    std::array<Slot, Capacity> slots_{};
};

}  // namespace ce::ngx_lifecycle
