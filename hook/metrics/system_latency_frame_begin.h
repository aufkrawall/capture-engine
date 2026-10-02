#pragma once

// Process-wide record of when the game's current CPU frame was allowed to
// start.
//
// Without game-owned latency markers the only honest way to know how much of a
// frame's latency is simulation and render work is to observe the boundary the
// game itself waits on. The Reflex/Streamline/Vulkan low-latency sleep hooks
// see the closest available point: the application normally samples input
// after the wait returns. The hooks do not hold the per-API
// PerformanceMetrics instance that consumes the value, hence the process-wide
// slot.
//
// The wait stream is NOT treated as an application-frame counter. Real games
// can emit markers/waits at output cadence or from more than one integration
// layer. Application-source Present classification supplies that count; the
// newest usable wait is only paired with the classified source frame.
//
// A Present returning is deliberately NOT a boundary, though it looks like one:
//
//   * the present wrapper is entered more than once per displayed frame in
//     several configurations - 2.3x on a 144 Hz Talos session, against 178-201
//     runtime presents and ~100 displayed transitions per second - which
//     shredded the measured application cadence into sub-frame fragments;
//   * under frame generation the Present that returns is the generator's
//     pacing thread, not the application's frame, so it says nothing about
//     when the application last sampled input.
//
// A game with no low-latency integration therefore has no boundary and falls
// back to modelling one interval of CPU work, which is honest rather than
// confidently wrong.

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
