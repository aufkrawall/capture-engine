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

// Provenance of a published screen timestamp. Kernel sync completions retain
// the ETW timestamp used by PresentMon's MsBetweenDisplayChange. An immediate
// flip may carry a driver-announced schedule; an explicit frame-type payload
// carries its own screen time. None is inferred from interval flatness or a
// guessed refresh grid. These are OS/driver observations, not optical timing.
enum : uint32_t {
    // The producer identified a screen-time event for this transition.
    kDisplayTimingScreenTimeResolved = 1u << 0,
    // graphTimeUs is later than screenTimeUs: the kernel reported this
    // synchronized flip sooner than one refresh after the previous transition,
    // which the panel cannot show (see display_timing_refresh_bound.h).
    kDisplayTimingGraphTimeRefreshBounded = 1u << 1,
};

struct DisplayTimingSample {
    std::atomic<uint64_t> sequence{0};
    // The kernel completion timestamp, as PresentMon reports it. Everything
    // that correlates frames (recording, latency, pacing traces) uses this.
    std::atomic<int64_t> screenTimeUs{0};
    // Runtime PresentStart associated with this displayed transition by the
    // sensor's PresentMon-style ETW reducer. Generated output can be displayed
    // after a newer application marker has already been submitted, so screen
    // time alone is not a causal frame identity.
    std::atomic<int64_t> presentStartTimeUs{0};
    std::atomic<uint32_t> flags{0};
    // The earliest time the panel could have shown this transition. Equal to
    // screenTimeUs unless kDisplayTimingGraphTimeRefreshBounded is set; only
    // the overlay's frame-time graph and its statistics read it.
    std::atomic<int64_t> graphTimeUs{0};
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
            sample.sequence.store(0, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            sample.screenTimeUs.store(0, std::memory_order_relaxed);
            sample.presentStartTimeUs.store(0, std::memory_order_relaxed);
            sample.flags.store(0, std::memory_order_relaxed);
            sample.graphTimeUs.store(0, std::memory_order_relaxed);
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

    // `screenTimeResolved` is what the producer knows about its own timestamp;
    // a publication that does not say otherwise is claiming a screen time.
    // `graphTimeUs` <= screenTimeUs means "not bounded": the graph uses the
    // screen time itself.
    void Publish(int64_t screenTimeUs, int64_t publishQpcUs, int64_t presentStartTimeUs = 0,
                 bool screenTimeResolved = true, int64_t graphTimeUs = 0) {
        const bool refreshBounded = graphTimeUs > screenTimeUs;
        const uint32_t sampleFlags = (screenTimeResolved ? kDisplayTimingScreenTimeResolved : 0u) |
                                     (refreshBounded ? kDisplayTimingGraphTimeRefreshBounded : 0u);
        const uint64_t sequence = writeSequence.load(std::memory_order_relaxed) + 1;
        auto& sample = samples[(sequence - 1) & (DISPLAY_TIMING_RING_SIZE - 1)];
        // Invalidate before overwriting: a lagging reader must not accept new
        // payload fields under this slot's previous sequence. The release /
        // acquire fences also order a reader that sees only part of the new
        // atomic payload before its second sequence check.
        sample.sequence.store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        sample.screenTimeUs.store(screenTimeUs, std::memory_order_relaxed);
        sample.presentStartTimeUs.store(presentStartTimeUs, std::memory_order_relaxed);
        sample.flags.store(sampleFlags, std::memory_order_relaxed);
        sample.graphTimeUs.store(refreshBounded ? graphTimeUs : screenTimeUs, std::memory_order_relaxed);
        sample.sequence.store(sequence, std::memory_order_release);
        lastPublishQpcUs.store(publishQpcUs, std::memory_order_relaxed);
        writeSequence.store(sequence, std::memory_order_release);
        status.store(static_cast<uint32_t>(DisplayTimingStatus::Active), std::memory_order_release);
    }

    bool Read(uint64_t sequence, int64_t& screenTimeUs) const {
        int64_t unusedPresentStartTimeUs = 0;
        return Read(sequence, screenTimeUs, unusedPresentStartTimeUs);
    }

    bool Read(uint64_t sequence, int64_t& screenTimeUs, int64_t& presentStartTimeUs) const {
        bool unusedScreenTimeResolved = false;
        return Read(sequence, screenTimeUs, presentStartTimeUs, unusedScreenTimeResolved);
    }

    bool Read(uint64_t sequence, int64_t& screenTimeUs, int64_t& presentStartTimeUs,
              bool& screenTimeResolved) const {
        int64_t unusedGraphTimeUs = 0;
        return Read(sequence, screenTimeUs, presentStartTimeUs, screenTimeResolved, unusedGraphTimeUs);
    }

    bool Read(uint64_t sequence, int64_t& screenTimeUs, int64_t& presentStartTimeUs,
              bool& screenTimeResolved, int64_t& graphTimeUs) const {
        if (sequence == 0)
            return false;
        const uint64_t generation = publicationGeneration.load(std::memory_order_acquire);
        if ((generation & 1u) != 0)
            return false;
        const auto& sample = samples[(sequence - 1) & (DISPLAY_TIMING_RING_SIZE - 1)];
        if (sample.sequence.load(std::memory_order_acquire) != sequence)
            return false;
        screenTimeUs = sample.screenTimeUs.load(std::memory_order_relaxed);
        presentStartTimeUs = sample.presentStartTimeUs.load(std::memory_order_relaxed);
        screenTimeResolved =
            (sample.flags.load(std::memory_order_relaxed) & kDisplayTimingScreenTimeResolved) != 0;
        graphTimeUs = sample.graphTimeUs.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        return sample.sequence.load(std::memory_order_acquire) == sequence &&
               publicationGeneration.load(std::memory_order_acquire) == generation;
    }
};

#pragma pack(pop)

static_assert((DISPLAY_TIMING_RING_SIZE & (DISPLAY_TIMING_RING_SIZE - 1)) == 0,
              "Display timing ring size must be a power of two");
