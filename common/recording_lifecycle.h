#pragma once

#include "shared_defs.h"

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

}  // namespace ce::recording_lifecycle
