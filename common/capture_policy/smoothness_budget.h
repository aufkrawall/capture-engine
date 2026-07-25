#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>

#include "live_debt.h"

// Smoothness buffering: delay floors, surface budgets, and pool sizing.

namespace ce::capture_policy {

inline uint32_t GetWgcSmoothnessBudgetFps(uint32_t outputFps,
                                          uint32_t sourceRatePermille = kWgcSmoothnessBufferSourceRatePermille) {
    if (outputFps == 0) {
        return 0;
    }
    if (sourceRatePermille <= 1000u) {
        return outputFps;
    }
    const uint64_t scaled =
        (static_cast<uint64_t>(outputFps) * static_cast<uint64_t>(sourceRatePermille) + 999ull) / 1000ull;
    return scaled > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(scaled);
}

inline uint32_t GetWgcSmoothnessDesiredFrames(uint32_t outputFps, uint32_t maxSmoothnessMs) {
    if (outputFps == 0 || maxSmoothnessMs == 0) {
        return 0;
    }
    const uint32_t budgetFps = GetWgcSmoothnessBudgetFps(outputFps);
    return GetWgcFrameCountForDurationMs(budgetFps, maxSmoothnessMs);
}

inline bool ShouldUseWgcSmoothnessBuffer(bool enabled, bool useVfr, bool avContentDelayActive,
                                         int64_t targetIntervalTicks) {
    return enabled && !useVfr && avContentDelayActive && targetIntervalTicks > 0;
}

// Measured startup WGC delivery jitter used to auto-derive a smoothness floor. All fields are
// microseconds. Delivery-gap fields describe the wall-clock spacing of FrameArrived callbacks
// (DWM -> capture frame pool burstiness); source-jitter fields describe the spacing of the
// game's own presents. Avg fields are for logging only; the derivation uses the max fields.
struct WgcSmoothnessFloorJitter {
    uint32_t deliveryGapAvgUs = 0;
    uint32_t deliveryGapMaxUs = 0;
    uint32_t sourceJitterAvgUs = 0;
    uint32_t sourceJitterMaxUs = 0;
};

// True when a smoothness reservoir/delay should be armed: either an audio-latency content delay
// is active, or a smoothness floor is configured (auto, or an explicit value > 0). This lets the
// existing smoothness gates arm the buffer for video-only / low-confidence-probe captures, where
// avContentDelayActive is false but a baseline jitter buffer is still wanted.
inline bool WgcSmoothnessDelayDesired(bool avContentDelayActive, bool smoothnessFloorConfigured) {
    return avContentDelayActive || smoothnessFloorConfigured;
}

// Upper bound (QPC) for a smoothness floor delay: the smaller of the configured max smoothness
// window and the buildable retained reservoir (so the floor can never target a delay the pool
// cannot hold). Returns 0 when no reservoir capacity is available.
inline int64_t GetWgcSmoothnessFloorCapQpc(int64_t targetIntervalTicks, int64_t qpcTicksPerSecond,
                                           uint32_t maxSmoothnessMs, uint32_t maxReservoirFrames) {
    if (targetIntervalTicks <= 0 || maxReservoirFrames == 0) {
        return 0;
    }
    int64_t capQpc = targetIntervalTicks * static_cast<int64_t>(maxReservoirFrames);
    if (maxSmoothnessMs > 0 && qpcTicksPerSecond > 0) {
        const int64_t maxMsQpc = (qpcTicksPerSecond * static_cast<int64_t>(maxSmoothnessMs)) / 1000;
        if (maxMsQpc > 0) {
            capQpc = std::min(capQpc, maxMsQpc);
        }
    }
    return capQpc > 0 ? capQpc : 0;
}

// Clamp an arbitrary requested floor delay (QPC) to [minFloorFrames, cap]. Returns 0 when no
// reservoir capacity exists (cap == 0), i.e. the floor cannot be realized.
inline int64_t ClampWgcSmoothnessFloorDelayQpc(int64_t requestedFloorQpc, int64_t targetIntervalTicks,
                                               int64_t qpcTicksPerSecond, uint32_t maxSmoothnessMs,
                                               uint32_t maxReservoirFrames,
                                               uint32_t minFloorFrames = kWgcSmoothnessFloorMinFrames) {
    const int64_t capQpc =
        GetWgcSmoothnessFloorCapQpc(targetIntervalTicks, qpcTicksPerSecond, maxSmoothnessMs, maxReservoirFrames);
    if (capQpc <= 0 || targetIntervalTicks <= 0) {
        return 0;
    }
    const int64_t minFloorQpc =
        std::min(capQpc, targetIntervalTicks * static_cast<int64_t>(std::max<uint32_t>(1u, minFloorFrames)));
    return std::clamp(requestedFloorQpc, minFloorQpc, capQpc);
}

// Auto-derive a smoothness floor delay (QPC) from measured startup WGC delivery jitter. It sizes
// the floor to absorb the worst observed delivery burst beyond one frame interval (and the worst
// source-present jitter), with a structural minimum of kWgcSmoothnessFloorMinFrames, clamped to
// the configured max smoothness window and the buildable reservoir. There is NO device-specific
// hardcoded value: the magnitude is measured, then only clamped by config/budget. When no jitter
// has been observed yet the floor falls back to the structural minimum so the active-delay
// machinery still engages with a small buffer.
inline int64_t DeriveWgcSmoothnessFloorDelayQpc(const WgcSmoothnessFloorJitter& jitter, int64_t targetIntervalTicks,
                                                int64_t qpcTicksPerSecond, uint32_t maxSmoothnessMs,
                                                uint32_t maxReservoirFrames,
                                                uint32_t minFloorFrames = kWgcSmoothnessFloorMinFrames) {
    if (targetIntervalTicks <= 0 || qpcTicksPerSecond <= 0) {
        return 0;
    }
    const int64_t frameIntervalUs = (targetIntervalTicks * 1000000) / qpcTicksPerSecond;
    const int64_t deliveryExcessUs = static_cast<int64_t>(jitter.deliveryGapMaxUs) > frameIntervalUs
                                         ? (static_cast<int64_t>(jitter.deliveryGapMaxUs) - frameIntervalUs)
                                         : 0;
    const int64_t jitterUs = std::max<int64_t>(deliveryExcessUs, static_cast<int64_t>(jitter.sourceJitterMaxUs));
    const int64_t requestedFloorQpc = jitterUs > 0 ? (qpcTicksPerSecond * jitterUs) / 1000000 : 0;
    return ClampWgcSmoothnessFloorDelayQpc(requestedFloorQpc, targetIntervalTicks, qpcTicksPerSecond, maxSmoothnessMs,
                                           maxReservoirFrames, minFloorFrames);
}

inline bool ShouldArmWgcSmoothnessBufferForSourceRate(uint32_t outputFps, uint32_t recentInputMin250Fps,
                                                      uint32_t recentInputMin500Fps,
                                                      uint32_t fpsMargin = kWgcRecoverySourceMarginFps) {
    if (outputFps == 0) {
        return false;
    }

    if (recentInputMin250Fps == 0 || recentInputMin500Fps == 0) {
        return false;
    }

    const bool sustainedBelowTarget =
        recentInputMin250Fps + fpsMargin < outputFps && recentInputMin500Fps + fpsMargin < outputFps;
    return !sustainedBelowTarget;
}

inline uint64_t EstimateWgcSurfaceBytes(uint32_t width, uint32_t height, uint32_t bytesPerPixel) {
    if (width == 0 || height == 0 || bytesPerPixel == 0) {
        return 0;
    }
    return static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * static_cast<uint64_t>(bytesPerPixel);
}

inline uint64_t EstimateWgcSmoothnessBytesPerRetainedFrame(uint32_t width, uint32_t height, uint32_t bytesPerPixel,
                                                           uint32_t bufferedCopies = 2) {
    return EstimateWgcSurfaceBytes(width, height, bytesPerPixel) * static_cast<uint64_t>(bufferedCopies);
}

inline uint32_t GetWgcEstimatedSyncDelayFramesForBudget(uint32_t outputFps,
                                                        uint32_t syncDelayMs = kWgcSmoothnessEstimatedSyncDelayMs) {
    return GetWgcSmoothnessDesiredFrames(outputFps, syncDelayMs);
}

inline uint32_t GetWgcSmoothnessBudgetedSurfaceCount(uint32_t width, uint32_t height, uint32_t bytesPerPixel,
                                                     uint32_t budgetMb) {
    const uint64_t bytesPerSurface = EstimateWgcSurfaceBytes(width, height, bytesPerPixel);
    if (budgetMb == 0 || bytesPerSurface == 0) {
        return 0;
    }
    const uint64_t budgetBytes = static_cast<uint64_t>(budgetMb) * 1024ull * 1024ull;
    const uint64_t surfaces = budgetBytes / bytesPerSurface;
    return surfaces > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(surfaces);
}

struct WgcSmoothnessSurfaceBudget {
    uint32_t desiredExtraFrames = 0;
    uint32_t retainedExtraFrames = 0;
    uint32_t retainedFrameCap = 0;
    uint32_t sourceFramePoolBuffers = 0;
    uint32_t copyPoolSlots = 0;
    uint32_t syncDelayFrames = 0;
    uint32_t safetySlots = kWgcSmoothnessBufferPoolSafetyFrames;
    uint32_t inFlightEncodeSlots = kWgcSmoothnessBufferPoolInFlightFrames;
    uint32_t selectedFrameSlackSlots = kWgcSmoothnessBufferPoolSelectedSlackFrames;
    uint32_t reservedFreeCopySlots = kWgcSmoothnessBufferPoolSafetyFrames + kWgcSmoothnessBufferPoolInFlightFrames +
                                     kWgcSmoothnessBufferPoolSelectedSlackFrames;
    uint32_t budgetSurfaceCount = 0;
    uint32_t budgetCopySurfaceCount = 0;
    uint64_t sourceBytesPerSurface = 0;
    uint64_t copyBytesPerSurface = 0;
    uint64_t sourceEstimatedBytes = 0;
    uint64_t copyEstimatedBytes = 0;
    uint64_t estimatedBytes = 0;
    bool splitByteBudget = false;
    bool capLimited = false;
    bool budgetExhausted = false;
};

inline uint32_t GetWgcSmoothnessReservedFreeCopySlots(
    uint32_t safetySlots = kWgcSmoothnessBufferPoolSafetyFrames,
    uint32_t inFlightEncodeSlots = kWgcSmoothnessBufferPoolInFlightFrames,
    uint32_t selectedFrameSlackSlots = kWgcSmoothnessBufferPoolSelectedSlackFrames) {
    return safetySlots + inFlightEncodeSlots + selectedFrameSlackSlots;
}

inline uint32_t GetWgcSmoothnessRetainedFrameCap(uint32_t copyPoolSlots, uint32_t reservedFreeCopySlots) {
    return copyPoolSlots > reservedFreeCopySlots ? (copyPoolSlots - reservedFreeCopySlots) : 0u;
}

inline uint32_t GetWgcSmoothnessExtraFramesForRetainedCap(uint32_t retainedFrameCap, uint32_t syncDelayFrames) {
    const uint32_t requiredDelayFrames = syncDelayFrames + kWgcDelayReservoirTargetExtraFrames;
    return retainedFrameCap > requiredDelayFrames ? (retainedFrameCap - requiredDelayFrames) : 0u;
}

inline uint32_t GetWgcSmoothnessPreferredSourceFramePoolBuffers(uint32_t outputFps, bool compactCopySurfaces) {
    if (!compactCopySurfaces || outputFps < 100) {
        return kWgcSmoothnessSourceFramePoolDefaultBuffers;
    }

    // The source frame pool is STAGING only when the compact retained copy is
    // active: each delivered frame is converted into the (cheaper) retained
    // copy pool synchronously in the WGC callback and released, so the pool
    // depth only needs to cover callback scheduling latency and delivery
    // bursts, not retention. The previous fps/10 sizing (12 FP16 buffers at
    // 120 fps = 759 MB staging at 4K) was retention-era; fps/15 keeps ~2
    // output frames of burst headroom above the historical 8-buffer default
    // (120 fps -> 8, 144 -> 10, 240 -> 12 capped).
    const uint32_t fpsScaled = std::max<uint32_t>(kWgcSmoothnessSourceFramePoolDefaultBuffers, (outputFps + 14u) / 15u);
    return std::min<uint32_t>(fpsScaled, kWgcSmoothnessSourceFramePoolCompactHighFpsMaxBuffers);
}

// requiresSourceFramePool=false is the DXGI Desktop Duplication shape: the OS
// owns the single desktop image (no consumer-owned WGC source frame pool), so
// the entire VRAM budget funds retained copy slots instead of splitting it.
inline WgcSmoothnessSurfaceBudget ComputeWgcSmoothnessSurfaceBudget(
    uint32_t outputFps, uint32_t maxSmoothnessMs, uint32_t width, uint32_t height, uint32_t sourceBytesPerPixel,
    uint32_t copyBytesPerPixel, uint32_t budgetMb, uint32_t syncDelayFrames, bool requiresSourceFramePool = true) {
    WgcSmoothnessSurfaceBudget result{};
    result.desiredExtraFrames = GetWgcSmoothnessDesiredFrames(outputFps, maxSmoothnessMs);
    result.syncDelayFrames = syncDelayFrames;
    result.reservedFreeCopySlots = GetWgcSmoothnessReservedFreeCopySlots(result.safetySlots, result.inFlightEncodeSlots,
                                                                         result.selectedFrameSlackSlots);
    result.splitByteBudget = sourceBytesPerPixel != copyBytesPerPixel;

    const uint64_t sourceBytesPerSurface = EstimateWgcSurfaceBytes(width, height, sourceBytesPerPixel);
    const uint64_t copyBytesPerSurface = EstimateWgcSurfaceBytes(width, height, copyBytesPerPixel);
    result.sourceBytesPerSurface = sourceBytesPerSurface;
    result.copyBytesPerSurface = copyBytesPerSurface;
    result.budgetSurfaceCount = GetWgcSmoothnessBudgetedSurfaceCount(width, height, sourceBytesPerPixel, budgetMb);
    result.budgetCopySurfaceCount = GetWgcSmoothnessBudgetedSurfaceCount(width, height, copyBytesPerPixel, budgetMb);
    if (sourceBytesPerSurface == 0 || copyBytesPerSurface == 0 || result.budgetSurfaceCount == 0 ||
        result.budgetCopySurfaceCount == 0) {
        result.sourceFramePoolBuffers = requiresSourceFramePool ? kWgcSmoothnessSourceFramePoolMinBuffers : 0u;
        result.copyPoolSlots = kWgcSmoothnessBufferMinPoolFrames;
        result.retainedFrameCap = GetWgcSmoothnessRetainedFrameCap(result.copyPoolSlots, result.reservedFreeCopySlots);
        result.budgetExhausted = true;
        result.capLimited = result.desiredExtraFrames > 0;
        result.sourceEstimatedBytes = sourceBytesPerSurface * static_cast<uint64_t>(result.sourceFramePoolBuffers);
        result.copyEstimatedBytes = copyBytesPerSurface * static_cast<uint64_t>(result.copyPoolSlots);
        result.estimatedBytes = result.sourceEstimatedBytes + result.copyEstimatedBytes;
        return result;
    }

    uint32_t maxBudgetedCopySlots = 0;
    if (!requiresSourceFramePool) {
        result.sourceFramePoolBuffers = 0;
        const uint64_t budgetBytes = static_cast<uint64_t>(budgetMb) * 1024ull * 1024ull;
        const uint64_t rawCopySlots = budgetBytes / copyBytesPerSurface;
        maxBudgetedCopySlots = static_cast<uint32_t>(
            std::min<uint64_t>(rawCopySlots, static_cast<uint64_t>(kWgcSmoothnessBufferMaxPoolFrames)));
    } else if (!result.splitByteBudget) {
        uint32_t remainingSurfaces = result.budgetSurfaceCount;
        if (remainingSurfaces >= kWgcSmoothnessSourceFramePoolDefaultBuffers + kWgcSmoothnessBufferMinPoolFrames) {
            result.sourceFramePoolBuffers = kWgcSmoothnessSourceFramePoolDefaultBuffers;
        } else if (remainingSurfaces > kWgcSmoothnessBufferMinPoolFrames) {
            result.sourceFramePoolBuffers = std::max<uint32_t>(kWgcSmoothnessSourceFramePoolMinBuffers,
                                                               remainingSurfaces - kWgcSmoothnessBufferMinPoolFrames);
        } else {
            result.sourceFramePoolBuffers = std::max<uint32_t>(
                1u, std::min<uint32_t>(kWgcSmoothnessSourceFramePoolMinBuffers, remainingSurfaces / 2u));
        }
        result.sourceFramePoolBuffers = std::min(result.sourceFramePoolBuffers, remainingSurfaces);
        remainingSurfaces -= result.sourceFramePoolBuffers;
        maxBudgetedCopySlots = std::min<uint32_t>(remainingSurfaces, kWgcSmoothnessBufferMaxPoolFrames);
    } else {
        const uint64_t budgetBytes = static_cast<uint64_t>(budgetMb) * 1024ull * 1024ull;
        const uint32_t preferredSourceBuffers =
            GetWgcSmoothnessPreferredSourceFramePoolBuffers(outputFps, copyBytesPerPixel < sourceBytesPerPixel);
        const auto copySlotsForSourceBuffers = [&](uint32_t sourceBuffers) -> uint32_t {
            const uint64_t sourceBytes = sourceBytesPerSurface * static_cast<uint64_t>(sourceBuffers);
            if (sourceBytes >= budgetBytes) {
                return 0;
            }
            const uint64_t copySlots = (budgetBytes - sourceBytes) / copyBytesPerSurface;
            return copySlots > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(copySlots);
        };

        uint32_t selectedSourceBuffers = 0;
        uint32_t firstValidSourceBuffers = 0;
        uint32_t bestRetainedFrames = 0;
        for (uint32_t candidate = preferredSourceBuffers; candidate >= kWgcSmoothnessSourceFramePoolMinBuffers;
             --candidate) {
            const uint32_t rawCopySlots = copySlotsForSourceBuffers(candidate);
            if (rawCopySlots >= kWgcSmoothnessBufferMinPoolFrames) {
                if (firstValidSourceBuffers == 0) {
                    firstValidSourceBuffers = candidate;
                }
                const uint32_t cappedCopySlots = std::min<uint32_t>(rawCopySlots, kWgcSmoothnessBufferMaxPoolFrames);
                const uint32_t retainedCap =
                    GetWgcSmoothnessRetainedFrameCap(cappedCopySlots, result.reservedFreeCopySlots);
                const uint32_t extraCapacity = GetWgcSmoothnessExtraFramesForRetainedCap(retainedCap, syncDelayFrames);
                const uint32_t retainedForCandidate = std::min(result.desiredExtraFrames, extraCapacity);
                if (retainedForCandidate > bestRetainedFrames) {
                    bestRetainedFrames = retainedForCandidate;
                    selectedSourceBuffers = candidate;
                }
            }
            if (candidate == kWgcSmoothnessSourceFramePoolMinBuffers) {
                break;
            }
        }
        if (selectedSourceBuffers == 0) {
            selectedSourceBuffers = firstValidSourceBuffers;
        }
        if (selectedSourceBuffers == 0) {
            const uint32_t maxSourceByBudget =
                static_cast<uint32_t>(std::min<uint64_t>(preferredSourceBuffers, budgetBytes / sourceBytesPerSurface));
            selectedSourceBuffers =
                std::max<uint32_t>(1u, std::min<uint32_t>(maxSourceByBudget, preferredSourceBuffers));
        }

        result.sourceFramePoolBuffers = selectedSourceBuffers;
        maxBudgetedCopySlots = std::min<uint32_t>(copySlotsForSourceBuffers(result.sourceFramePoolBuffers),
                                                  kWgcSmoothnessBufferMaxPoolFrames);
    }

    uint32_t copySlots = maxBudgetedCopySlots;
    if (result.desiredExtraFrames > 0) {
        result.retainedFrameCap = GetWgcSmoothnessRetainedFrameCap(copySlots, result.reservedFreeCopySlots);
        const uint32_t extraFrameCapacity =
            GetWgcSmoothnessExtraFramesForRetainedCap(result.retainedFrameCap, result.syncDelayFrames);
        result.retainedExtraFrames = std::min<uint32_t>(result.desiredExtraFrames, extraFrameCapacity);
        const uint32_t desiredCopySlots = std::max<uint32_t>(
            kWgcSmoothnessBufferMinPoolFrames, result.reservedFreeCopySlots + result.syncDelayFrames +
                                                   kWgcDelayReservoirTargetExtraFrames + result.retainedExtraFrames);
        copySlots = std::min<uint32_t>(copySlots, desiredCopySlots + kWgcSmoothnessBufferPoolHeadroomSlots);
        result.retainedFrameCap = GetWgcSmoothnessRetainedFrameCap(copySlots, result.reservedFreeCopySlots);
    } else {
        result.retainedExtraFrames = 0;
        const uint32_t requiredDelayCopySlots =
            result.reservedFreeCopySlots + result.syncDelayFrames + kWgcDelayReservoirTargetExtraFrames;
        copySlots = std::min<uint32_t>(copySlots,
                                       std::max<uint32_t>(kWgcSmoothnessBufferMinPoolFrames, requiredDelayCopySlots));
        result.retainedFrameCap = GetWgcSmoothnessRetainedFrameCap(copySlots, result.reservedFreeCopySlots);
    }
    result.copyPoolSlots = std::max<uint32_t>(1u, copySlots);
    result.capLimited = result.retainedExtraFrames < result.desiredExtraFrames;
    const uint32_t minimumProtectedCopySlots =
        result.reservedFreeCopySlots + result.syncDelayFrames + kWgcDelayReservoirTargetExtraFrames;
    result.budgetExhausted = result.desiredExtraFrames > 0 && result.copyPoolSlots < minimumProtectedCopySlots;
    result.sourceEstimatedBytes = sourceBytesPerSurface * static_cast<uint64_t>(result.sourceFramePoolBuffers);
    result.copyEstimatedBytes = copyBytesPerSurface * static_cast<uint64_t>(result.copyPoolSlots);
    result.estimatedBytes = result.sourceEstimatedBytes + result.copyEstimatedBytes;
    return result;
}

inline WgcSmoothnessSurfaceBudget ComputeWgcSmoothnessSurfaceBudget(uint32_t outputFps, uint32_t maxSmoothnessMs,
                                                                    uint32_t width, uint32_t height,
                                                                    uint32_t bytesPerPixel, uint32_t budgetMb,
                                                                    uint32_t syncDelayFrames) {
    return ComputeWgcSmoothnessSurfaceBudget(outputFps, maxSmoothnessMs, width, height, bytesPerPixel, bytesPerPixel,
                                             budgetMb, syncDelayFrames);
}

inline uint32_t GetWgcSmoothnessRetainedFrames(uint32_t outputFps, uint32_t maxSmoothnessMs, uint32_t width,
                                               uint32_t height, uint32_t bytesPerPixel, uint32_t budgetMb,
                                               uint32_t syncDelayFrames = 0) {
    const uint32_t desiredFrames = GetWgcSmoothnessDesiredFrames(outputFps, maxSmoothnessMs);
    if (desiredFrames == 0) {
        return 0;
    }
    return ComputeWgcSmoothnessSurfaceBudget(outputFps, maxSmoothnessMs, width, height, bytesPerPixel, budgetMb,
                                             syncDelayFrames)
        .retainedExtraFrames;
}

inline uint32_t GetWgcSmoothnessPoolFrameCount(uint32_t retainedFrames) {
    const uint32_t desiredPool = retainedFrames + kWgcSmoothnessBufferPoolSafetyFrames;
    return std::clamp(desiredPool, kWgcSmoothnessBufferMinPoolFrames, kWgcSmoothnessBufferMaxPoolFrames);
}

inline uint32_t GetWgcPoolPressureRetainedTrimTarget(uint32_t currentFreeCopySlots, uint32_t reservedFreeCopySlots,
                                                     uint32_t delayReservoirTargetFrames, uint32_t retainedFrameCap) {
    if (reservedFreeCopySlots == 0 || currentFreeCopySlots > reservedFreeCopySlots || retainedFrameCap == 0 ||
        delayReservoirTargetFrames == 0) {
        return retainedFrameCap;
    }

    return std::min(retainedFrameCap, std::max<uint32_t>(1u, delayReservoirTargetFrames));
}

}  // namespace ce::capture_policy
