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
// The newest wait is only the presented frame's own when it returned on the
// thread that presents. An engine whose game thread runs ahead of its render
// thread (Unreal: game thread sleeps, RHI thread presents) has already slept
// for frame N+1 or N+2 when frame N is presented, so pairing across threads
// shortened the measured span by whole frames - below the modelled interval,
// which is itself only a floor for such an engine. The game's own
// SimulationStart/PresentStart markers carry a frame ID and hold across
// threads, so they take precedence whenever the game emits them.
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

// Where the game emitted the PresentStart marker of the pair an observation
// used, relative to the thread that presents. The Reflex contract brackets the
// Present call itself, which only the presenting thread can do; a marker from
// another thread was set before the Present was even queued, so "newest pair
// before this Present" may name a later frame.
enum class MarkerThread : uint8_t {
    Unknown = 0,
    Presenting = 1,
    Other = 2,
};

// What the clock knows about the frame a Present at notAfterUs belongs to.
struct FrameBeginObservation {
    int64_t beginUs = 0;
    FrameBeginKind kind = FrameBeginKind::Modelled;
    // SimulationMarker only: the same frame's PresentStart marker. The tracker
    // rejects it when it predates the previous application Present, which
    // means the marker belongs to an earlier frame.
    int64_t markerPresentUs = 0;
    // A usable sleep existed but returned on another thread than the caller's.
    bool sleepOnOtherThread = false;
    MarkerThread markerThread = MarkerThread::Unknown;
    // The frame generator's own ID for this frame (see
    // present_association::GeneratorFrameToken), when the presenting thread
    // configured one since its previous Present; 0 otherwise.
    uint64_t generatorFrameToken = 0;
};

class FrameBeginClock {
public:
    static FrameBeginClock& Get() {
        static FrameBeginClock instance;
        return instance;
    }

    // threadId 0 means "unknown" and pairs with any presenting thread.
    void Note(int64_t beginUs, FrameBeginKind kind, uint32_t threadId = 0) {
        if (beginUs <= 0 || kind == FrameBeginKind::Modelled)
            return;
        // Publishing the kind and thread first keeps a concurrent reader from
        // attributing a new timestamp to the previous boundary's metadata; the
        // reverse mismatch only costs one frame's anchor.
        kind_.store(static_cast<uint8_t>(kind), std::memory_order_relaxed);
        threadId_.store(threadId, std::memory_order_relaxed);
        beginUs_.store(beginUs, std::memory_order_release);
    }

    // A completed SimulationStart/PresentStart pair of one frame ID. Called
    // from the marker hook once the frame's PresentStart is known.
    void NoteMarkerFrame(int64_t simulationStartUs, int64_t presentMarkerUs, uint32_t markerThreadId = 0) {
        if (simulationStartUs <= 0 || presentMarkerUs < simulationStartUs)
            return;
        WriteMarker(simulationStartUs, presentMarkerUs, markerThreadId);
    }

    // The boundary for a Present entered at notAfterUs on presentingThreadId
    // (0 = unknown). A fresh marker pair wins; otherwise the newest sleep at or
    // before the Present, if it returned on the presenting thread. A boundary in
    // the future belongs to another presenting thread, and one older than a
    // frame-generation-scale interval means the producing hook stopped running.
    FrameBeginObservation Observe(int64_t notAfterUs, uint32_t presentingThreadId) const {
        FrameBeginObservation observation;
        if (notAfterUs <= 0)
            return observation;

        int64_t simulationStartUs = 0;
        int64_t presentMarkerUs = 0;
        uint32_t markerThreadId = 0;
        if (ReadMarker(simulationStartUs, presentMarkerUs, markerThreadId) && presentMarkerUs <= notAfterUs &&
            notAfterUs - presentMarkerUs <= kMaximumAgeUs && presentMarkerUs - simulationStartUs <= kMaximumAgeUs) {
            observation.beginUs = simulationStartUs;
            observation.kind = FrameBeginKind::SimulationMarker;
            observation.markerPresentUs = presentMarkerUs;
            if (presentingThreadId != 0 && markerThreadId != 0) {
                observation.markerThread =
                    markerThreadId == presentingThreadId ? MarkerThread::Presenting : MarkerThread::Other;
            }
        }

        const int64_t beginUs = beginUs_.load(std::memory_order_acquire);
        if (beginUs <= 0 || beginUs > notAfterUs || notAfterUs - beginUs > kMaximumAgeUs)
            return observation;
        const uint32_t sleepThreadId = threadId_.load(std::memory_order_relaxed);
        if (presentingThreadId != 0 && sleepThreadId != 0 && sleepThreadId != presentingThreadId) {
            observation.sleepOnOtherThread = true;
            return observation;
        }
        if (observation.kind != FrameBeginKind::Modelled)
            return observation;
        observation.beginUs = beginUs;
        observation.kind = static_cast<FrameBeginKind>(kind_.load(std::memory_order_relaxed));
        return observation;
    }

    // Returns the most recent sleep boundary at or before notAfterUs from any
    // thread, or zero when none is usable.
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
        threadId_.store(0, std::memory_order_relaxed);
        kind_.store(static_cast<uint8_t>(FrameBeginKind::Modelled), std::memory_order_release);
        WriteMarker(0, 0, 0);
    }

private:
    static constexpr int64_t kMaximumAgeUs = 250'000;

    bool ReadMarker(int64_t& simulationStartUs, int64_t& presentMarkerUs, uint32_t& markerThreadId) const {
        const uint64_t before = markerVersion_.load(std::memory_order_acquire);
        if ((before & 1u) != 0)
            return false;
        simulationStartUs = markerSimulationStartUs_.load(std::memory_order_relaxed);
        presentMarkerUs = markerPresentUs_.load(std::memory_order_relaxed);
        markerThreadId = markerThreadId_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        return markerVersion_.load(std::memory_order_relaxed) == before && simulationStartUs > 0 &&
               presentMarkerUs >= simulationStartUs;
    }

    // Seqlock: odd while writing, so a reader never pairs one frame's
    // simulation start with another frame's present marker. A second writer
    // racing the first drops its pair; the next frame supplies a fresh one.
    void WriteMarker(int64_t simulationStartUs, int64_t presentMarkerUs, uint32_t markerThreadId) {
        uint64_t version = markerVersion_.load(std::memory_order_relaxed);
        if ((version & 1u) != 0 ||
            !markerVersion_.compare_exchange_strong(version, version + 1, std::memory_order_relaxed)) {
            return;
        }
        std::atomic_thread_fence(std::memory_order_release);
        markerSimulationStartUs_.store(simulationStartUs, std::memory_order_relaxed);
        markerPresentUs_.store(presentMarkerUs, std::memory_order_relaxed);
        markerThreadId_.store(markerThreadId, std::memory_order_relaxed);
        markerVersion_.store(version + 2, std::memory_order_release);
    }

    std::atomic<int64_t> beginUs_{0};
    std::atomic<uint32_t> threadId_{0};
    std::atomic<uint8_t> kind_{static_cast<uint8_t>(FrameBeginKind::Modelled)};
    std::atomic<uint64_t> markerVersion_{0};
    std::atomic<int64_t> markerSimulationStartUs_{0};
    std::atomic<int64_t> markerPresentUs_{0};
    std::atomic<uint32_t> markerThreadId_{0};
};

inline void NoteFrameBegin(int64_t beginUs, FrameBeginKind kind, uint32_t threadId = 0) {
    FrameBeginClock::Get().Note(beginUs, kind, threadId);
}

inline void NoteMarkerFrameBegin(int64_t simulationStartUs, int64_t presentMarkerUs, uint32_t markerThreadId = 0) {
    FrameBeginClock::Get().NoteMarkerFrame(simulationStartUs, presentMarkerUs, markerThreadId);
}

namespace detail {
inline thread_local uint64_t t_configuredGeneratorFrameToken = 0;
}  // namespace detail

// A frame generator was configured for the frame this thread is building (FSR
// 3.1: ffxConfigure with the frame's frameID). The application Present that
// follows on the same thread is that frame; consuming the token there names it
// with the ID the generator's present callback reports for every output derived
// from it. Thread-local on purpose: a configure on another thread cannot be
// paired with this thread's Present without guessing their order.
inline void NoteGeneratorFrameConfigured(uint64_t generatorFrameToken) {
    detail::t_configuredGeneratorFrameToken = generatorFrameToken;
}

inline uint64_t ConsumeGeneratorFrameConfigured() {
    const uint64_t token = detail::t_configuredGeneratorFrameToken;
    detail::t_configuredGeneratorFrameToken = 0;
    return token;
}

inline int64_t LatestFrameBegin(int64_t notAfterUs, FrameBeginKind& kind) {
    return FrameBeginClock::Get().Latest(notAfterUs, kind);
}

inline FrameBeginObservation ObserveFrameBegin(int64_t notAfterUs, uint32_t presentingThreadId) {
    return FrameBeginClock::Get().Observe(notAfterUs, presentingThreadId);
}

}  // namespace ce::system_latency
