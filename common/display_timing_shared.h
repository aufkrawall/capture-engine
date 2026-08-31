#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

enum class FrameTimeSource : uint32_t {
    DisplayChange = 0,
    Presentation = 1,
};

enum class DisplayTimingStatus : uint32_t {
    Unavailable = 0,
    Starting = 1,
    Active = 2,
    AccessDenied = 3,
    Failed = 4,
};

static constexpr std::size_t DISPLAY_TIMING_RING_SIZE = 512;

// Producer and consumer live in different processes and must agree exactly on
// what a published timestamp means. The naive `ticks * 1'000'000 / frequency`
// overflows int64 after roughly ten days of uptime at a 10 MHz counter, which
// would silently turn every published timestamp stale on a machine that has
// simply been running a while. Divide first, then carry the remainder.
inline int64_t DisplayTimingQpcToUs(int64_t ticks, int64_t frequency) {
    if (ticks <= 0 || frequency <= 0)
        return 0;
    return (ticks / frequency) * 1'000'000 + ((ticks % frequency) * 1'000'000) / frequency;
}

inline int64_t DisplayTimingUsToQpc(int64_t microseconds, int64_t frequency) {
    if (microseconds <= 0 || frequency <= 0)
        return 0;
    return (microseconds / 1'000'000) * frequency +
           ((microseconds % 1'000'000) * frequency) / 1'000'000;
}

#pragma pack(push, 8)

struct DisplayTimingSample {
    std::atomic<uint64_t> sequence{0};
    std::atomic<int64_t> screenTimeUs{0};
};

// Sensor -> overlay single-producer/multi-consumer timestamp ring. Each reader
// owns its cursor, so DXGI and Vulkan overlays can consume the same stream
// without acknowledging or blocking one another.
struct SharedDisplayTiming {
    DisplayTimingSample samples[DISPLAY_TIMING_RING_SIZE]{};

    alignas(64) std::atomic<uint64_t> writeSequence{0};
    alignas(64) std::atomic<uint64_t> publicationGeneration{0};
    std::atomic<int64_t> lastPublishQpcUs{0};
    std::atomic<uint32_t> sourcePid{0};
    std::atomic<uint32_t> rendererPid{0};
    std::atomic<uint32_t> status{static_cast<uint32_t>(DisplayTimingStatus::Unavailable)};
    std::atomic<uint32_t> droppedTimestampCount{0};

    void Reset(uint32_t source, uint32_t renderer, DisplayTimingStatus newStatus) {
        publicationGeneration.fetch_add(1, std::memory_order_acq_rel);
        writeSequence.store(0, std::memory_order_relaxed);
        lastPublishQpcUs.store(0, std::memory_order_relaxed);
        sourcePid.store(source, std::memory_order_relaxed);
        rendererPid.store(renderer, std::memory_order_relaxed);
        droppedTimestampCount.store(0, std::memory_order_relaxed);
        for (auto& sample : samples) {
            sample.screenTimeUs.store(0, std::memory_order_relaxed);
            sample.sequence.store(0, std::memory_order_relaxed);
        }
        status.store(static_cast<uint32_t>(newStatus), std::memory_order_relaxed);
        publicationGeneration.fetch_add(1, std::memory_order_release);
    }

    void SetStatus(DisplayTimingStatus newStatus) {
        status.store(static_cast<uint32_t>(newStatus), std::memory_order_release);
    }

    DisplayTimingStatus GetStatus() const {
        return static_cast<DisplayTimingStatus>(status.load(std::memory_order_acquire));
    }

    void Publish(int64_t screenTimeUs, int64_t publishQpcUs) {
        const uint64_t sequence = writeSequence.load(std::memory_order_relaxed) + 1;
        auto& sample = samples[(sequence - 1) & (DISPLAY_TIMING_RING_SIZE - 1)];
        sample.screenTimeUs.store(screenTimeUs, std::memory_order_relaxed);
        sample.sequence.store(sequence, std::memory_order_release);
        lastPublishQpcUs.store(publishQpcUs, std::memory_order_relaxed);
        writeSequence.store(sequence, std::memory_order_release);
        status.store(static_cast<uint32_t>(DisplayTimingStatus::Active), std::memory_order_release);
    }

    bool Read(uint64_t sequence, int64_t& screenTimeUs) const {
        if (sequence == 0)
            return false;
        const auto& sample = samples[(sequence - 1) & (DISPLAY_TIMING_RING_SIZE - 1)];
        if (sample.sequence.load(std::memory_order_acquire) != sequence)
            return false;
        screenTimeUs = sample.screenTimeUs.load(std::memory_order_relaxed);
        return sample.sequence.load(std::memory_order_acquire) == sequence;
    }
};

#pragma pack(pop)

static_assert((DISPLAY_TIMING_RING_SIZE & (DISPLAY_TIMING_RING_SIZE - 1)) == 0,
              "Display timing ring size must be a power of two");
