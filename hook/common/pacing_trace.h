#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <vector>
#include "pacing_health_telemetry.h"

namespace ce::pacing_trace {
enum class Kind : uint32_t { Epoch, DisplayPair, CallbackBegin, CallbackEnd, Work, Submit, Fence, Marker, Frame, MarkerObserved, FenceSignal,
                             PresentBegin, PresentForward, PresentEnd };
struct Event {
    int64_t timeUs = 0;
    uint64_t epoch = 0, id = 0, object = 0, a = 0, b = 0, c = 0;
    uint32_t thread = 0, flags = 0;
    Kind kind = Kind::Epoch;
};

// No allocation, spin, wait or pointer dereference on the producer path. Readers
// and producers try a slot once; unavailable/overwritten observations stay gaps.
template <size_t Capacity> class Ring {
    struct Slot { std::atomic_flag busy = ATOMIC_FLAG_INIT; uint64_t sequence = UINT64_MAX; Event event; };
    std::array<Slot, Capacity> slots_{};
    std::atomic<uint64_t> total_{0}, dropped_{0};
public:
    void Push(const Event& event) {
        const auto sequence = total_.fetch_add(1, std::memory_order_relaxed);
        auto& slot = slots_[sequence % Capacity];
        if (slot.busy.test_and_set(std::memory_order_acquire)) { ++dropped_; return; }
        if (slot.sequence == UINT64_MAX || sequence > slot.sequence) {
            slot.event = event;
            slot.sequence = sequence;
        } else { ++dropped_; }
        slot.busy.clear(std::memory_order_release);
    }
    uint64_t Total() const { return total_.load(std::memory_order_acquire); }
    uint64_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }
    std::vector<Event> Snapshot(uint64_t minimum = 0, int64_t afterTimeUs = INT64_MIN) {
        const auto end = Total();
        const auto first = end > Capacity ? end - Capacity : 0;
        std::vector<Event> events;
        events.reserve(static_cast<size_t>(end - first));
        for (auto i = first > minimum ? first : minimum; i < end; ++i) {
            auto& slot = slots_[i % Capacity];
            if (slot.busy.test_and_set(std::memory_order_acquire)) continue;
            if (slot.sequence == i && slot.event.timeUs > afterTimeUs) events.push_back(slot.event);
            slot.busy.clear(std::memory_order_release);
        }
        return events;
    }
};

struct EpisodeDetector {
    unsigned bad = 0, healthy = 0;
    bool latched = false;
    uint64_t epoch = 0;
    uint32_t previousMedian = 0;
    bool Observe(uint64_t tag, bool stable, const pacing_health::ChannelStats& display,
                 const pacing_health::ChannelStats& present) {
        if (tag != epoch) { epoch = tag; bad = healthy = 0; previousMedian = 0; }
        // A transition does not re-arm an existing episode. Only measured recovery does.
        if (!tag || !stable || display.samples < 80 || present.samples < 80 ||
            present.maxUs > 100'000 || display.maxUs > 100'000) {
            bad = healthy = 0; return false;
        }
        const bool cadenceChanged = previousMedian &&
            (present.medianUs > previousMedian * 5ULL / 4 || present.medianUs < previousMedian * 3ULL / 4);
        previousMedian = present.medianUs;
        if (cadenceChanged) { bad = healthy = 0; return false; }
        const bool suspect = display.stddevUs >= 1500 && display.latePermille >= 150 &&
            present.stddevUs < 1200 && display.stddevUs >= 2 * present.stddevUs;
        if (suspect) {
            healthy = 0;
            if (++bad >= 3 && !latched) { latched = true; return true; }
        } else {
            bad = 0;
            if (display.stddevUs < 1200 && display.latePermille < 150) {
                if (++healthy >= 3) latched = false;
            } else { healthy = 0; }
        }
        return false;
    }
};

#ifdef VK_LAYER_CE_OVERLAY
// The standalone Vulkan layer has no DX12 callbacks or hook-service loop.
inline void Initialize(const char*) {}
inline bool Enabled() { return false; }
inline void Service() {}
inline void Epoch(uint64_t, int64_t) {}
inline void Invalidate(int64_t) {}
inline void Record(Kind, uint64_t = 0, const void* = nullptr, uint64_t = 0,
                   uint64_t = 0, uint64_t = 0, uint32_t = 0, int64_t = 0) {}
#else
void Initialize(const char* perfPath);
bool Enabled();
void Service(); // existing hook-service thread only; all aggregation/file I/O lives here
void Epoch(uint64_t tag, int64_t timeUs);
void Invalidate(int64_t timeUs);
void Record(Kind kind, uint64_t id = 0, const void* object = nullptr, uint64_t a = 0,
            uint64_t b = 0, uint64_t c = 0, uint32_t flags = 0, int64_t timeUs = 0);
#endif
}  // namespace ce::pacing_trace
