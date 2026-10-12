#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ce::audio {
// One timeline stage owns each instance; timeline resets may concurrently Arm(0).
// Progress survives short packets/pulls, and ingest corrections never consume the output fade.
class PacketBoundaryFade {
public:
    PacketBoundaryFade() = default;
    PacketBoundaryFade(const PacketBoundaryFade& other) noexcept
        : state_(other.state_.load(std::memory_order_relaxed)) {}
    PacketBoundaryFade& operator=(const PacketBoundaryFade& other) noexcept {
        if (this != &other) {
            state_.store(other.state_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    void Arm(int samples) {
        const uint32_t clamped = static_cast<uint32_t>(std::max(0, samples));
        state_.store((static_cast<uint64_t>(clamped) << 32) | clamped, std::memory_order_release);
    }

    int Remaining() const {
        return static_cast<int>(state_.load(std::memory_order_acquire) & 0xFFFFFFFFu);
    }

    size_t Apply(float* data, size_t frames, size_t channels) {
        if (!data || !channels) return 0;
        uint64_t current = state_.load(std::memory_order_acquire);
        const int total = static_cast<int>(current >> 32);
        const int remaining = static_cast<int>(current & 0xFFFFFFFFu);
        if (remaining <= 0 || total <= 0) return 0;
        const size_t count = std::min(frames, static_cast<size_t>(remaining));
        const int done = total - remaining;
        for (size_t frame = 0; frame < count; ++frame) {
            const float phase = static_cast<float>(done + frame + 1) / static_cast<float>(total);
            const float gain = 0.5f * (1.0f - std::cos(3.14159265358979323846f * phase));
            for (size_t channel = 0; channel < channels; ++channel)
                data[frame * channels + channel] *= gain;
        }
        const uint32_t nextRemaining = static_cast<uint32_t>(remaining - static_cast<int>(count));
        const uint64_t next =
            nextRemaining > 0 ? ((static_cast<uint64_t>(static_cast<uint32_t>(total)) << 32) | nextRemaining) : 0;
        state_.compare_exchange_strong(current, next, std::memory_order_acq_rel, std::memory_order_relaxed);
        return count;
    }

private:
    std::atomic<uint64_t> state_{0};
};
}  // namespace ce::audio
