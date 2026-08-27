#pragma once

// Pacing-boundary selection and diagnostics for the Vulkan limiter call sites.
// Header-only sibling of vulkan_layer_present.cpp so that file stays under the
// source-size ceiling; the boundary decision is the semantic unit.

#include <atomic>
#include <cstdint>

#include "layer_main.h"  // LayerLog
#include "vulkan_layer.h"

#include "../common/fps_limiter.h"

namespace ce::vulkan_present_boundary {

// The route by which the layer proved that the application does not present
// from the thread currently inside the present hook. Both routes move the
// limiter boundary to vkAcquireNextImageKHR: pacing inside the present of a
// split production/presentation topology (Portal with RTX Remix renders on a
// graphics producer thread and presents from a non-graphics queue thread)
// cannot throttle production, while the acquire boundary can.
enum class AsyncRoute { kNone, kAcquireThreadMismatch, kSubmitThreadMismatch };

inline const char* AsyncRouteName(AsyncRoute route) {
    switch (route) {
        case AsyncRoute::kAcquireThreadMismatch:
            return "acquire-thread mismatch";
        case AsyncRoute::kSubmitThreadMismatch:
            return "submit-thread mismatch";
        default:
            return "none";
    }
}

inline AsyncRoute DetectAcquireThreadMismatch(uint32_t acquireThreadId, uint64_t lastAcquireTickMs,
                                              uint32_t currentThreadId, uint64_t nowTickMs) {
    if (acquireThreadId != 0 && acquireThreadId != currentThreadId && lastAcquireTickMs != 0 &&
        (nowTickMs - lastAcquireTickMs) < 2000ULL) {
        return AsyncRoute::kAcquireThreadMismatch;
    }
    return AsyncRoute::kNone;
}

// Resolves async-present detection for the present hook and emits the
// one-time, edge-triggered state-transition diagnostics for BOTH detection
// routes (the submit-thread route previously switched to acquire pacing
// silently, which made acquire-time pacing invisible in session logs).
// Returns whether presentation is asynchronous for this call. A detection edge
// also resets the limiter's output-group admission so the first callback after
// the pacing boundary moved owns a clean cadence slot.
inline bool ResolvePresentLimiterBoundary(SwapchainData* sd, VkDevice queueDevice) {
    bool asyncPresentDetected = false;
    const uint32_t currentThreadId = GetCurrentThreadId();
    if (sd) {
        const uint32_t acquireThreadId = sd->lastAcquireThreadId.load(std::memory_order_acquire);
        const uint64_t lastAcquireTick = sd->lastAcquireTick.load(std::memory_order_acquire);
        if (DetectAcquireThreadMismatch(acquireThreadId, lastAcquireTick, currentThreadId, GetTickCount64()) ==
            AsyncRoute::kAcquireThreadMismatch) {
            if (!sd->asyncPresentDetected.exchange(true, std::memory_order_acq_rel)) {
                LayerLog(
                    "Vulkan Layer: Async present detected for swapchain %p (acquire-thread mismatch "
                    "acquire=%u present=%u); limiter boundary moved to vkAcquireNextImageKHR",
                    reinterpret_cast<void*>(sd->swapchain), acquireThreadId, currentThreadId);
                g_SharedFpsLimiter.ResetOutputGroupAdmission();
            }
        }
        asyncPresentDetected = sd->asyncPresentDetected.load(std::memory_order_acquire);
    }
    if (!asyncPresentDetected && queueDevice != VK_NULL_HANDLE) {
        const uint32_t lastSubmitThreadId = VulkanLayerState::Get().GetLastSubmitThreadId(queueDevice);
        if (lastSubmitThreadId != 0 && lastSubmitThreadId != currentThreadId) {
            asyncPresentDetected = true;
            if (sd) {
                if (!sd->asyncPresentDetected.exchange(true, std::memory_order_acq_rel)) {
                    LayerLog(
                        "Vulkan Layer: Async present detected for swapchain %p (submit-thread mismatch "
                        "submit=%u present=%u); limiter boundary moved to vkAcquireNextImageKHR",
                        reinterpret_cast<void*>(sd->swapchain), lastSubmitThreadId, currentThreadId);
                    g_SharedFpsLimiter.ResetOutputGroupAdmission();
                }
            } else {
                // No swapchain state to latch on; a process-global latch keeps
                // the transition diagnostics and the admission reset one-shot.
                static std::atomic<bool> s_swapchainlessSubmitTransitionLogged{false};
                if (!s_swapchainlessSubmitTransitionLogged.exchange(true, std::memory_order_acq_rel)) {
                    LayerLog(
                        "Vulkan Layer: Async present detected without swapchain state (submit-thread mismatch "
                        "submit=%u present=%u); limiter boundary moved to vkAcquireNextImageKHR",
                        lastSubmitThreadId, currentThreadId);
                    g_SharedFpsLimiter.ResetOutputGroupAdmission();
                }
            }
        }
    }
    return asyncPresentDetected;
}

// One-shot boundary-identity log latch: fires when the encoded boundary
// (call site kind + FG multiplier + grouped admission) changes, never per
// present.
inline bool ShouldLogBoundaryChange(std::atomic<uint32_t>& latch, uint32_t encoded) {
    uint32_t prev = latch.load(std::memory_order_acquire);
    while (prev != encoded) {
        if (latch.compare_exchange_weak(prev, encoded, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

constexpr uint32_t kEncodeMask = 0xFFu;

inline uint32_t EncodeLimiterBoundary(uint32_t boundaryKind, int fgMultiplier, bool groupedAdmission) {
    return (boundaryKind & kEncodeMask) | (static_cast<uint32_t>(fgMultiplier & 0xFF) << 8) |
           (groupedAdmission ? (1u << 16) : 0u);
}

inline int ResolveBoundaryFGMultiplier() {
    return g_FGCompat.IsFGActive() ? g_FGCompat.GetFGMultiplier() : 1;
}

inline void ReportPresentTimeLimiterBoundary(const SwapchainData* sd, bool groupedAdmission) {
    static std::atomic<uint32_t> s_lastLoggedBoundary{0};
    const int fgMultiplier = ResolveBoundaryFGMultiplier();
    if (ShouldLogBoundaryChange(s_lastLoggedBoundary, EncodeLimiterBoundary(1, fgMultiplier, groupedAdmission))) {
        LayerLog("Vulkan Layer: FPS limiter boundary = vkQueuePresentKHR swapchain=%p fg=%dx grouped=%d",
                 sd ? reinterpret_cast<void*>(sd->swapchain) : nullptr, fgMultiplier, groupedAdmission ? 1 : 0);
    }
}

inline void ReportAcquireTimeLimiterBoundary(const SwapchainData* sd, bool groupedAdmission) {
    static std::atomic<uint32_t> s_lastLoggedBoundary{0};
    const int fgMultiplier = ResolveBoundaryFGMultiplier();
    if (ShouldLogBoundaryChange(s_lastLoggedBoundary, EncodeLimiterBoundary(2, fgMultiplier, groupedAdmission))) {
        LayerLog("Vulkan Layer: FPS limiter boundary = vkAcquireNextImageKHR swapchain=%p fg=%dx grouped=%d",
                 sd ? reinterpret_cast<void*>(sd->swapchain) : nullptr, fgMultiplier, groupedAdmission ? 1 : 0);
    }
}

}  // namespace ce::vulkan_present_boundary
