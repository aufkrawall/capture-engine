#pragma once

// Process-wide record of when the game's current CPU frame was allowed to
// start.
//
// Without game-owned latency markers the only honest way to know how much of a
// frame's latency is simulation and render work is to observe the boundary the
// game itself waits on. Two hooks see it and neither of them holds the per-API
// PerformanceMetrics instance that consumes the value:
//
//   * the present wrappers know when the previous Present returned, which is
//     the earliest a game that is not throttled can start its next frame;
//   * the Reflex/Streamline/Vulkan low-latency sleep hooks know when the
//     per-frame wait ended, which is where a throttled game starts instead.
//
// Both are recorded and the most recent one wins, which resolves the two cases
// without any per-runtime special casing: with a low-latency runtime active the
// sleep returns after the previous Present did, so it is naturally preferred;
// with the runtime off no sleep is ever observed and the Present return stands.
// That is exactly the difference a low-latency mode makes to input latency, so
// the estimate becomes sensitive to it instead of modelling a fixed frame of
// CPU work in both configurations.

#include "system_latency_types.h"

#include <atomic>
#include <cstdint>

namespace ce::system_latency {

class FrameBeginClock {
public:
    static FrameBeginClock& Get() {
        static FrameBeginClock instance;
        return instance;
    }

    void Note(int64_t beginUs, FrameBeginKind kind) {
        if (beginUs <= 0 || kind == FrameBeginKind::Modelled)
            return;
        // Publishing the kind first keeps a concurrent reader from attributing
        // a new timestamp to the previous boundary's kind. The kind is only
        // ever used for diagnostics, so the reverse mismatch is harmless.
        kind_.store(static_cast<uint8_t>(kind), std::memory_order_relaxed);
        beginUs_.store(beginUs, std::memory_order_release);
    }

    // Returns the most recent boundary at or before notAfterUs, or zero when
    // none is usable. A boundary in the future belongs to another presenting
    // thread, and one older than a frame-generation-scale interval means the
    // producing hook stopped running.
    int64_t Latest(int64_t notAfterUs, FrameBeginKind& kind) const {
        kind = FrameBeginKind::Modelled;
        const int64_t beginUs = beginUs_.load(std::memory_order_acquire);
        if (beginUs <= 0 || notAfterUs <= 0 || beginUs > notAfterUs || notAfterUs - beginUs > kMaximumAgeUs)
            return 0;
        kind = static_cast<FrameBeginKind>(kind_.load(std::memory_order_relaxed));
        return beginUs;
    }

    void Reset() {
        beginUs_.store(0, std::memory_order_relaxed);
        kind_.store(static_cast<uint8_t>(FrameBeginKind::Modelled), std::memory_order_release);
    }

private:
    static constexpr int64_t kMaximumAgeUs = 250'000;

    std::atomic<int64_t> beginUs_{0};
    std::atomic<uint8_t> kind_{static_cast<uint8_t>(FrameBeginKind::Modelled)};
};

inline void NoteFrameBegin(int64_t beginUs, FrameBeginKind kind) {
    FrameBeginClock::Get().Note(beginUs, kind);
}

inline int64_t LatestFrameBegin(int64_t notAfterUs, FrameBeginKind& kind) {
    return FrameBeginClock::Get().Latest(notAfterUs, kind);
}

}  // namespace ce::system_latency
