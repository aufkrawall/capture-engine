#pragma once

#include "common/ipc/shared_defs.h"

#include <atomic>
#include <cstdint>

namespace ce::recording_lifecycle {

inline bool IsTerminalTransition(CapturePipelinePhase phase) {
    return phase == CapturePipelinePhase::kStopping || phase == CapturePipelinePhase::kCancelling;
}

inline CapturePipelinePhase SelectStopTransition(CapturePipelinePhase phase, uint32_t liveFramesEncoded) {
    if (IsTerminalTransition(phase)) {
        return phase;
    }
    if (liveFramesEncoded == 0 &&
        (phase == CapturePipelinePhase::kIdle || phase == CapturePipelinePhase::kWarmup)) {
        return CapturePipelinePhase::kCancelling;
    }
    return CapturePipelinePhase::kStopping;
}

inline bool TryArmWarmup(std::atomic<uint32_t>& phase, const std::atomic<bool>& recordingRequested) {
    if (!recordingRequested.load(std::memory_order_acquire)) {
        return false;
    }
    uint32_t expected = static_cast<uint32_t>(CapturePipelinePhase::kIdle);
    return phase.compare_exchange_strong(expected, static_cast<uint32_t>(CapturePipelinePhase::kWarmup),
                                         std::memory_order_acq_rel, std::memory_order_acquire);
}

inline bool TryCommitLive(std::atomic<uint32_t>& phase, const std::atomic<bool>& recordingRequested) {
    if (!recordingRequested.load(std::memory_order_acquire)) {
        return false;
    }
    uint32_t expected = static_cast<uint32_t>(CapturePipelinePhase::kWarmup);
    return phase.compare_exchange_strong(expected, static_cast<uint32_t>(CapturePipelinePhase::kLive),
                                         std::memory_order_acq_rel, std::memory_order_acquire);
}

inline CapturePipelinePhase BeginStop(std::atomic<uint32_t>& phase, uint32_t liveFramesEncoded) {
    uint32_t current = phase.load(std::memory_order_acquire);
    while (true) {
        const auto currentPhase = static_cast<CapturePipelinePhase>(current);
        const CapturePipelinePhase target = SelectStopTransition(currentPhase, liveFramesEncoded);
        if (target == currentPhase ||
            phase.compare_exchange_weak(current, static_cast<uint32_t>(target), std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            return target;
        }
    }
}

// Startup latency for the controller's "Recording is live" line. The controller only notices
// liveness in its once-per-second health poll, so the observation alone overstated startup by up
// to a second (20260927_195021: media live +5453 ms, logged +6375 ms). Media stamps
// runtimeState.recordingStartTime with GetTickCount64() at the exact live transition, the same
// system-wide clock as the controller's request tick, so that stamp is used whenever it is
// plausible: at or after the request and not after the observation. A missing or stale stamp
// falls back to the observation time with exact=false instead of reporting a wrong value.
struct RecordingStartupTiming {
    uint64_t startupMs = 0;   // start request -> media live (observation time when !exact)
    uint64_t observedMs = 0;  // start request -> controller observation
    bool exact = false;       // startupMs comes from the media live stamp
};

inline RecordingStartupTiming ResolveRecordingStartupTiming(uint64_t requestTick, int64_t liveStampTick,
                                                            uint64_t observedTick) {
    RecordingStartupTiming timing;
    if (requestTick == 0 || observedTick < requestTick) {
        return timing;
    }
    timing.observedMs = observedTick - requestTick;
    timing.startupMs = timing.observedMs;
    if (liveStampTick > 0) {
        const uint64_t liveTick = static_cast<uint64_t>(liveStampTick);
        if (liveTick >= requestTick && liveTick <= observedTick) {
            timing.startupMs = liveTick - requestTick;
            timing.exact = true;
        }
    }
    return timing;
}

}  // namespace ce::recording_lifecycle
