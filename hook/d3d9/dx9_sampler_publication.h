#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "dx9_state_block_sampler_policy.h"

// The D3D9 sampler shadow's per-draw answers, without the device lock.
//
// Engines re-set every sampler state for every draw. Borderlands 2 with AF
// forced made about 1.7 million SetSamplerState calls a second, 97% of them
// re-setting the value the sampler already held (20261103_183225:
// interceptedSets=16134840 redundantSetsSkipped=15644518 in its first ~9 s).
// Each one took the device mutex - two locked instructions and two calls into
// the C++ runtime - for an answer that only changes when the shadow does.
//
// So the shadow publishes, per sampler, the application's values and its bound
// texture while that sampler is reconciled, and a redundant set or a re-bind of
// the same texture is answered from the publication with plain loads.
//
// Every change to the shadow happens under the device mutex, and the holder
// withdraws the samplers it may change BEFORE changing them, then republishes
// the ones that end clean. A set bit therefore always means the published
// values are the shadow's. A reader that saw the bit and then a value from a
// later republish saw that later clean state instead; either is a state the
// sampler held while the reader's call ran.

namespace ce::dx9_sampler_state {

class SamplerPublication {
public:
    static constexpr uint32_t kAllSamplers = (1u << kSamplerCount) - 1u;
    static constexpr uint32_t Bit(size_t sampler) {
        return 1u << sampler;
    }

    // Writers: the device mutex is held.
    void Withdraw(uint32_t samplers) {
        clean_.store(clean_.load(std::memory_order_relaxed) & ~samplers, std::memory_order_relaxed);
    }
    void Publish(size_t sampler, const SamplerStateValues& logical, const void* texture) {
        Entry& entry = entries_[sampler];
        for (size_t i = 0; i < kStateCount; ++i)
            entry.logical[i].store(logical[i], std::memory_order_relaxed);
        entry.texture.store(texture, std::memory_order_relaxed);
        clean_.store(clean_.load(std::memory_order_relaxed) | Bit(sampler), std::memory_order_release);
    }

    // Readers: no lock.
    bool IsRedundantSet(size_t sampler, size_t stateIndex, DWORD value) const {
        return (clean_.load(std::memory_order_acquire) & Bit(sampler)) != 0 &&
               entries_[sampler].logical[stateIndex].load(std::memory_order_relaxed) == value;
    }
    bool IsSameTexture(size_t sampler, const void* texture) const {
        return texture && (clean_.load(std::memory_order_acquire) & Bit(sampler)) != 0 &&
               entries_[sampler].texture.load(std::memory_order_relaxed) == texture;
    }
    uint32_t Clean() const {
        return clean_.load(std::memory_order_acquire);
    }

private:
    struct Entry {
        std::array<std::atomic<DWORD>, kStateCount> logical{};
        std::atomic<const void*> texture{nullptr};
    };

    std::atomic<uint32_t> clean_{0};
    std::array<Entry, kSamplerCount> entries_{};
};

// The end of a change: of the samplers it withdrew, those that are reconciled
// go back into the publication; the rest stay out until a change cleans them.
// Nothing is published while the device as a whole may not answer from it
// (recording, no override, a configuration not yet applied).
inline void RepublishClean(SamplerPublication& publication, const SamplerShadow& shadow, uint32_t withdrawn,
                           bool devicePublishable) {
    if (!devicePublishable)
        return;
    for (uint32_t pending = withdrawn & SamplerPublication::kAllSamplers; pending != 0; pending &= pending - 1) {
        const size_t sampler = static_cast<size_t>(std::countr_zero(pending));
        const SamplerState& state = shadow[sampler];
        if (state.initialized && state.reconciled)
            publication.Publish(sampler, state.logical, state.boundTexture);
    }
}

}  // namespace ce::dx9_sampler_state
