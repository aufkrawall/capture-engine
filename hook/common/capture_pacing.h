#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "../../common/shared_defs.h"
#include "hook_common.h"
#include "perf_logger.h"

// Shared capture cadence gating for all graphics APIs.
//
// Returns true if the current frame should be SKIPPED to maintain target FPS
// cadence.  Uses an atomic deadline with compare-and-swap so that concurrent
// Present threads (e.g. frame-generation paths) are handled lock-free.
//
// Overcapture: the gate runs at 125% of the configured output FPS.  This
// passes ~25% more frames to the downstream encoder, which uses timestamp-
// aware selection to pick the frame closest to each output grid tick.  The
// extra candidates reduce temporal error by ~14% compared to 1:1 gating,
// producing smoother motion in the CFR output.
//
// Half-interval acceptance: within each overcapture interval, the gate accepts
// frames within half an interval *before* the deadline, biasing toward the
// present closest to the ideal output grid tick.
//
// Each binary (hook DLL, Vulkan layer DLL) gets its own set of static atomics,
// which is correct since only one graphics API is active per process.

inline bool ShouldSkipCaptureForTargetCadence(SharedMemoryLayout* shm, const char* apiTag) {
    if (!shm) {
        return false;
    }

    if (!shm->runtimeState.captureRequested.load(std::memory_order_acquire)) {
        return false;
    }

    const int captureFps = shm->fpsLimiter.GetCaptureFps();
    if (captureFps <= 0) {
        return false;
    }

    // Overcapture: capture 25% more frames than the output target so the
    // encoder has selection headroom for timestamp-aware frame picking.
    const int64_t overcaptureFps = static_cast<int64_t>(captureFps) * 5 / 4;
    const int64_t targetIntervalUs = 1000000LL / overcaptureFps;
    const int64_t halfIntervalUs = targetIntervalUs / 2;
    const int64_t resetThresholdUs = targetIntervalUs * 4;
    const int64_t nowUs = PerfLogger::GetQpcUs();

    static std::atomic<int64_t> s_nextCaptureDeadlineUs{0};
    static std::atomic<uint64_t> s_pacedCaptureSkipCount{0};

    int64_t nextCaptureDeadlineUs = s_nextCaptureDeadlineUs.load(std::memory_order_acquire);
    for (;;) {
        // Rebase deadline on first frame, time jump, or large drift.
        if (nextCaptureDeadlineUs == 0 || nowUs + targetIntervalUs < nextCaptureDeadlineUs ||
            nowUs - nextCaptureDeadlineUs > resetThresholdUs) {
            int64_t rebasedDeadlineUs = nowUs + targetIntervalUs;
            if (s_nextCaptureDeadlineUs.compare_exchange_weak(nextCaptureDeadlineUs, rebasedDeadlineUs,
                                                              std::memory_order_acq_rel, std::memory_order_acquire)) {
                return false;  // Capture this frame
            }
            continue;
        }

        // Well before deadline — skip this frame.
        if (nowUs < nextCaptureDeadlineUs - halfIntervalUs) {
            uint64_t skipCount = s_pacedCaptureSkipCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (skipCount <= 10 || (skipCount % 1000) == 0) {
                HookLogImportant("%s: Pacing capture skip #%llu (until=%lldus interval=%lldus captureFps=%d)", apiTag,
                                 static_cast<unsigned long long>(skipCount),
                                 static_cast<long long>(nextCaptureDeadlineUs - nowUs),
                                 static_cast<long long>(targetIntervalUs), captureFps);
            }
            return true;  // Skip
        }

        // Frame is within the half-interval acceptance window OR past deadline.
        // Capture it — it's the closest present to this output grid tick.
        int64_t advancedDeadlineUs;
        if (nowUs < nextCaptureDeadlineUs) {
            // Early-accept: advance by exactly one interval from the grid.
            advancedDeadlineUs = nextCaptureDeadlineUs + targetIntervalUs;
        } else {
            // At or past deadline: advance past current time by whole intervals.
            advancedDeadlineUs = nextCaptureDeadlineUs;
            do {
                advancedDeadlineUs += targetIntervalUs;
            } while (advancedDeadlineUs <= nowUs);
        }

        if (s_nextCaptureDeadlineUs.compare_exchange_weak(nextCaptureDeadlineUs, advancedDeadlineUs,
                                                          std::memory_order_acq_rel, std::memory_order_acquire)) {
            return false;  // Capture this frame
        }
    }
}
