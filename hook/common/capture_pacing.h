#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "../../common/capture_pipeline_policy.h"
#include "../../common/shared_defs.h"
#include "hook_common.h"
#include "perf_logger.h"

// Shared capture cadence gating for all graphics APIs.
//
// Returns true if the current frame should be SKIPPED to maintain target FPS
// cadence.  Uses an atomic last-accepted timestamp with compare-and-swap so
// that concurrent Present threads (e.g. frame-generation paths) are handled
// lock-free.
//
// Overcapture: the gate publishes at the shared CFR source-publication rate.
// CFR needs candidates on both sides of an output-grid tick; otherwise a
// source that is merely close to the target FPS can still arrive just after a
// tick and force a repeat.  The encoder thread drops excess candidates before
// encode, so publication headroom buys smoothness without extra encoded frames.
//
// Publication spacing: this gate only limits excessive hook-side publication.
// It does not align to an output grid; the media thread owns CFR selection.
// A small early slack absorbs QPC/source jitter so a 240 Hz source is not
// accidentally reduced to every third frame for a 240 Hz publication cap.
//
// Each binary (hook DLL, Vulkan layer DLL) gets its own default state. A final
// output route can provide a separate state so a base-frame fallback never
// consumes its cadence budget.
using CaptureCadenceGateState = ce::capture_policy::FinalOutputCadenceState;

inline bool ShouldSkipCaptureForTargetCadenceAtUs(
    SharedMemoryLayout* shm, const char* apiTag, int64_t nowUs, CaptureCadenceGateState& gate,
    uint32_t headroomPermille = ce::capture_policy::kInjectCfrPublicationHeadroomPermille) {
    if (!shm) {
        return false;
    }

    if (!shm->runtimeState.IsInjectVideoCaptureRequested()) {
        return false;
    }

    const int captureFps = shm->fpsLimiter.GetCaptureFps();
    if (captureFps <= 0) {
        return false;
    }

    const uint32_t publicationFps = ce::capture_policy::GetInjectCfrSourcePublicationFps(
        static_cast<uint32_t>(captureFps), headroomPermille);
    const int64_t targetIntervalUs = ce::capture_policy::GetInjectCfrSourcePublicationIntervalUs(
        static_cast<uint32_t>(captureFps), headroomPermille);
    if (publicationFps == 0 || targetIntervalUs <= 0) {
        return false;
    }
    const int64_t minSpacingUs = ce::capture_policy::GetInjectCfrPublicationMinSpacingUs(targetIntervalUs);
    const int64_t earlySlackUs = ce::capture_policy::GetInjectCfrPublicationEarlySlackUs(targetIntervalUs);
    const int64_t resetThresholdUs = targetIntervalUs * 8;
    int64_t lastCaptureUs = gate.lastCaptureUs.load(std::memory_order_acquire);
    for (;;) {
        // Rebase on first frame, time jump, or a long source stall.
        if (lastCaptureUs == 0 || nowUs < lastCaptureUs || nowUs - lastCaptureUs > resetThresholdUs) {
            if (gate.lastCaptureUs.compare_exchange_weak(lastCaptureUs, nowUs, std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
                return false;  // Capture this frame
            }
            continue;
        }

        const int64_t elapsedUs = nowUs - lastCaptureUs;
        if (elapsedUs < minSpacingUs) {
            uint64_t skipCount = gate.pacedCaptureSkipCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (skipCount <= 10 || (skipCount % 1000) == 0) {
                HookLogImportant(
                    "%s: Pacing capture skip #%llu (until=%lldus interval=%lldus minSpacing=%lldus slack=%lldus "
                    "captureFps=%d publicationFps=%u)",
                    apiTag, static_cast<unsigned long long>(skipCount),
                    static_cast<long long>(minSpacingUs - elapsedUs), static_cast<long long>(targetIntervalUs),
                    static_cast<long long>(minSpacingUs), static_cast<long long>(earlySlackUs), captureFps,
                    publicationFps);
            }
            return true;  // Skip
        }

        if (gate.lastCaptureUs.compare_exchange_weak(lastCaptureUs, nowUs, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
            return false;  // Capture this frame
        }
    }
}

inline bool ShouldSkipCaptureForTargetCadence(SharedMemoryLayout* shm, const char* apiTag) {
    static CaptureCadenceGateState s_defaultGate;
    return ShouldSkipCaptureForTargetCadenceAtUs(shm, apiTag, PerfLogger::GetQpcUs(), s_defaultGate);
}
