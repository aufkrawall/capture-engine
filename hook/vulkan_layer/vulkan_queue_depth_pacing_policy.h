#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

// `[Graphics] backbuffer_count` on Vulkan as a presentation-queue depth, not an image count.
//
// The image count cannot carry the setting: a game's `minImageCount` declares how many images it
// keeps acquired, and taking images away below it breaks blocking vkAcquireNextImageKHR
// (vulkan_swapchain_image_policy.h, DOOM Eternal). Most games ask for three, so `backbuffer_count=2`
// changed nothing: Strange Brigade Vulkan, logs/20261019_022104, `minImageCount override skipped
// configured=2 game=3`.
//
// The DXGI implementation never shrinks a flip chain either; it waits on the frame-latency waitable
// before each Present so at most `backbuffer_count - 1` presents are queued. VK_KHR_present_wait is
// the Vulkan form of that wait: vkWaitForPresentKHR returns once the present carrying a given
// VkPresentIdKHR has reached the display. Before presenting id k, CE waits for id
// k - (backbuffer_count - 1), keeping every image the game asked for.
//
// Ownership mirrors the DXGI rule that a creator-owned waitable is never waited on by CE: a game that
// enabled VK_KHR_present_id or VK_KHR_present_wait itself manages its own present ids and latency
// (DXVK, for example), so CE leaves it alone. A metered frame generator places its generated images
// itself and a wait inside its batch would break that placement, so its presents are never paced.
//
// A pacing wait is a throttle, never a lock. The ceiling is the DXGI one (1 s, far above the slowest
// healthy wait); a present that misses it - a minimized window whose presents never complete, for
// example - switches that swapchain to non-blocking checks until one of its presents is seen
// complete again, so one stalled presentation cannot turn into a 1 s stall on every later present.
namespace ce::vulkan_queue_depth_pacing {

inline constexpr uint64_t kWaitCeilingNs = 1000ull * 1000ull * 1000ull;

struct DeviceInput {
    int32_t configuredBackbufferCount = -1;
    bool presentIdExtensionAvailable = false;
    bool presentWaitExtensionAvailable = false;
    bool presentIdFeatureSupported = false;
    bool presentWaitFeatureSupported = false;
    bool applicationEnabledPresentId = false;
    bool applicationEnabledPresentWait = false;
    // The application's pNext already carries one of the two feature structures; adding a second
    // structure of the same sType is invalid.
    bool applicationChainHasPresentFeatures = false;
    bool applicationEnabledPresentMetering = false;
    // VK_KHR_present_wait depends on VK_KHR_swapchain; a device that presents nothing needs neither.
    bool applicationEnabledSwapchain = false;
};

enum class DeviceDecision {
    kEnable,
    kNotConfigured,
    kApplicationOwned,
    kMeteredFrameGenerator,
    kUnsupported,
    kFeatureStructureConflict,
    kNoSwapchain,
    // The driver refused the create with the two extensions; CE retried without them.
    kRefusedByDevice,
};

inline DeviceDecision DecideDeviceEnablement(const DeviceInput& in) {
    if (in.configuredBackbufferCount < 2)
        return DeviceDecision::kNotConfigured;
    if (!in.applicationEnabledSwapchain)
        return DeviceDecision::kNoSwapchain;
    if (in.applicationEnabledPresentId || in.applicationEnabledPresentWait)
        return DeviceDecision::kApplicationOwned;
    if (in.applicationEnabledPresentMetering)
        return DeviceDecision::kMeteredFrameGenerator;
    if (!in.presentIdExtensionAvailable || !in.presentWaitExtensionAvailable || !in.presentIdFeatureSupported ||
        !in.presentWaitFeatureSupported)
        return DeviceDecision::kUnsupported;
    if (in.applicationChainHasPresentFeatures)
        return DeviceDecision::kFeatureStructureConflict;
    return DeviceDecision::kEnable;
}

inline const char* DescribeDeviceDecision(DeviceDecision decision) {
    switch (decision) {
        case DeviceDecision::kEnable:
            return "enabled";
        case DeviceDecision::kNotConfigured:
            return "not configured";
        case DeviceDecision::kApplicationOwned:
            return "application owns present ids/waits";
        case DeviceDecision::kMeteredFrameGenerator:
            return "metered frame generator";
        case DeviceDecision::kUnsupported:
            return "VK_KHR_present_id/present_wait unsupported";
        case DeviceDecision::kFeatureStructureConflict:
            return "application chain already carries the feature structures";
        case DeviceDecision::kNoSwapchain:
            return "device without VK_KHR_swapchain";
        case DeviceDecision::kRefusedByDevice:
            return "device creation refused the extensions; created without them";
    }
    return "?";
}

struct PresentInput {
    bool deviceEnabled = false;
    int32_t configuredBackbufferCount = -1;
    uint32_t swapchainImageCount = 0;
    uint32_t presentSwapchainCount = 0;
    // Shared DLSS-G state, a metered batch, or a present-metering node on this present.
    bool frameGenerationOwnsPlacement = false;
    // The application attached its own VkPresentIdKHR to this present.
    bool applicationSuppliedPresentId = false;
};

enum class PresentDecision {
    kPace,
    kNotConfigured,
    kDeviceNotEnabled,
    kImageCountCarriesDepth,
    kFrameGeneration,
    kApplicationPresentId,
    kMultipleSwapchains,
};

inline PresentDecision DecidePresent(const PresentInput& in) {
    if (in.configuredBackbufferCount < 2)
        return PresentDecision::kNotConfigured;
    if (!in.deviceEnabled)
        return PresentDecision::kDeviceNotEnabled;
    if (in.frameGenerationOwnsPlacement)
        return PresentDecision::kFrameGeneration;
    if (in.applicationSuppliedPresentId)
        return PresentDecision::kApplicationPresentId;
    if (in.presentSwapchainCount != 1)
        return PresentDecision::kMultipleSwapchains;
    // With no more images than the configured depth the image count already bounds the queue.
    if (in.swapchainImageCount != 0 && in.swapchainImageCount <= static_cast<uint32_t>(in.configuredBackbufferCount))
        return PresentDecision::kImageCountCarriesDepth;
    return PresentDecision::kPace;
}

inline const char* DescribePresentDecision(PresentDecision decision) {
    switch (decision) {
        case PresentDecision::kPace:
            return "pacing";
        case PresentDecision::kNotConfigured:
            return "not configured";
        case PresentDecision::kDeviceNotEnabled:
            return "device without CE present waits";
        case PresentDecision::kImageCountCarriesDepth:
            return "image count already carries the depth";
        case PresentDecision::kFrameGeneration:
            return "frame generation places these presents";
        case PresentDecision::kApplicationPresentId:
            return "application supplies present ids";
        case PresentDecision::kMultipleSwapchains:
            return "multi-swapchain present";
    }
    return "?";
}

// The present that must have reached the display before `presentId` is queued, so that at most
// `backbuffer_count - 1` presents are outstanding; 0 when nothing needs to be waited for yet.
inline uint64_t WaitTargetFor(uint64_t presentId, int32_t configuredBackbufferCount) {
    if (configuredBackbufferCount < 2)
        return 0;
    const uint64_t outstanding = static_cast<uint64_t>(configuredBackbufferCount - 1);
    return presentId > outstanding ? presentId - outstanding : 0;
}

enum class WaitOutcome {
    kCompleted,
    kTimedOut,
    kSwapchainUnusable,
    kFailed,
};

inline WaitOutcome ClassifyWaitResult(VkResult result) {
    switch (result) {
        case VK_SUCCESS:
        case VK_SUBOPTIMAL_KHR:
            return WaitOutcome::kCompleted;
        case VK_TIMEOUT:
            return WaitOutcome::kTimedOut;
        case VK_ERROR_OUT_OF_DATE_KHR:
        case VK_ERROR_SURFACE_LOST_KHR:
        case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
            return WaitOutcome::kSwapchainUnusable;
        default:
            return WaitOutcome::kFailed;
    }
}

// A degraded swapchain only checks without blocking; a completed present re-arms the blocking wait.
inline uint64_t WaitTimeoutNs(bool degraded) {
    return degraded ? 0ull : kWaitCeilingNs;
}

inline bool NextDegradedState(bool degraded, WaitOutcome outcome) {
    if (outcome == WaitOutcome::kCompleted)
        return false;
    if (outcome == WaitOutcome::kTimedOut)
        return true;
    return degraded;
}

// Ids whose vkQueuePresentKHR was accepted. A present the WSI rejected (out of date during a resize,
// for example) never reaches the display, and waiting for it would always run into the ceiling.
// backbuffer_count is at most 6, so the wait target is never more than 5 presents back.
inline constexpr uint32_t kQueuedIdRingSize = 8;

// Diagnostic window, logged every 10 s while pacing. It belongs to the presentation (the window),
// not to one swapchain: Strange Brigade Vulkan recreates its swapchain every 3-5 s and on every focus
// change or minimize, so a per-swapchain window never reached 10 s and sessions 20261019_124700 and
// _125351 printed no statistics at all. A destroyed swapchain hands its open window to the next
// swapchain on the same window (CarryWindow), and a window already due is logged at the hand-over.
inline constexpr uint64_t kWindowUs = 10ull * 1000ull * 1000ull;

struct WindowStats {
    uint64_t startUs = 0;
    uint32_t presents = 0;
    uint32_t waits = 0;
    uint32_t timeouts = 0;
    uint32_t skippedUnqueued = 0;
    uint64_t waitUs = 0;
    uint32_t maxWaitUs = 0;
    // Swapchains this window has covered, including the current one.
    uint32_t generations = 1;
    uint64_t totalTimeouts = 0;
    uint64_t totalRecoveries = 0;
};

inline bool IsWindowDue(const WindowStats& window, uint64_t nowUs) {
    return window.startUs != 0 && nowUs >= window.startUs && nowUs - window.startUs >= kWindowUs;
}

// Starts the next window after a log; the session totals stay.
inline void ResetWindow(WindowStats& window, uint64_t nowUs) {
    const uint64_t totalTimeouts = window.totalTimeouts;
    const uint64_t totalRecoveries = window.totalRecoveries;
    window = {};
    window.startUs = nowUs;
    window.totalTimeouts = totalTimeouts;
    window.totalRecoveries = totalRecoveries;
}

// A new swapchain on a window continues the window its predecessor left open. Only a window that
// started counts; the new swapchain's own (still empty) window is replaced.
inline bool CarryWindow(WindowStats& successor, const WindowStats& predecessor) {
    if (predecessor.startUs == 0)
        return false;
    successor = predecessor;
    ++successor.generations;
    return true;
}

struct SwapchainState {
    uint64_t nextPresentId = 0;
    uint64_t queuedIds[kQueuedIdRingSize] = {};
    bool degraded = false;
    int32_t loggedDecision = -1;
    WindowStats window;
};

inline void NoteQueuedPresent(SwapchainState& state, uint64_t presentId, bool accepted) {
    if (presentId == 0)
        return;
    state.queuedIds[presentId % kQueuedIdRingSize] = accepted ? presentId : 0;
}

inline bool WasQueued(const SwapchainState& state, uint64_t presentId) {
    return presentId != 0 && state.queuedIds[presentId % kQueuedIdRingSize] == presentId;
}

}  // namespace ce::vulkan_queue_depth_pacing
