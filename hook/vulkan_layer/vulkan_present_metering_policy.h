#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

// Frame-generation present metering and the swapchain's display-rate ceiling.
//
// `VK_NV_present_metering` tells NVIDIA's presentation engine how to spread a
// generated batch across its rendered-frame interval. That content-spacing
// signal is essential under variable base frame times; removing it makes the
// generated images arrive as a tight burst followed by a long gap. It does not,
// by itself, impose the display's maximum presentation rate. CE now supplies
// that independent constraint with VK_EXT_present_timing: each image has a
// native relative target no shorter than the display's minimum refresh cycle.
//
// Portal RTX (RTX Remix) session `20260829_022419` is that conflict end to end.
// CE overrode Remix's `VK_PRESENT_MODE_IMMEDIATE_KHR` to FIFO and the driver
// accepted it ("Overriding present mode 0 -> 2 (fifo)", `vkCreateSwapchainKHR
// driver returned: 0 (presentMode=2 ...)"), yet `perf_metrics_29472.csv` shows
// presents climbing to 172/s on a 143 Hz display, arriving as bursts of four
// within ~700 us followed by a ~23 ms gap - a 43 fps base rate times the
// configured 4x multi-frame generation (`rtx.dlfg.maxInterpolatedFrames = 3`).
// CE's final DXGI interception saw the same thing from below: while frame
// generation was off the driver presented the FIFO swapchain with
// `SyncInterval=1`, and the moment the DLFG swapchain went live it presented
// with `SyncInterval=0` plus `DXGI_PRESENT_ALLOW_TEARING` instead. Forcing the
// sync interval back on that final present removed the tearing but could not
// re-introduce the missing wait, which is exactly the reported symptom: no
// tearing, mailbox-like behavior, frame rate above the refresh rate.
//
// Remix documents `rtx.dlfg.enablePresentMetering` as the choice between
// hardware metering and CPU pacing. The runtime has a fallback, but the measured
// fallback did not preserve the generated batch's on-screen spacing.
//
// Session 20260830_234347 closed the remaining ambiguity: with metering disabled,
// 4x MFG still reached vkQueuePresentKHR as three sub-millisecond intervals and
// one 20-50 ms interval. The final SyncInterval=1 backstop capped the rate but
// rendered that burst/gap shape directly, producing the reported judder and
// overlay flicker. Driver-forced VSync remained smooth because it left the
// metering signal intact. CE therefore observes this node for diagnostics but
// never removes it; native relative timing owns only the display ceiling.

namespace ce::vulkan_present_metering_policy {

// VK_NV_present_metering, spec version 1. Mirrored locally so the policy does
// not depend on the Vulkan header revision a given toolchain ships: the Linux
// MSYS2 headers in this tree still predate the extension, while the Windows
// ones define it.
inline constexpr VkStructureType kStructureTypeSetPresentConfigNV = static_cast<VkStructureType>(1000613000);

struct SetPresentConfigNV {
    VkStructureType sType;
    const void* pNext;
    uint32_t numFramesPerBatch;
    uint32_t presentConfigFeedback;
};

#ifdef VK_NV_PRESENT_METERING_SPEC_VERSION
static_assert(static_cast<int>(kStructureTypeSetPresentConfigNV) ==
                  static_cast<int>(VK_STRUCTURE_TYPE_SET_PRESENT_CONFIG_NV),
              "local VK_NV_present_metering sType must match the Vulkan headers");
static_assert(sizeof(SetPresentConfigNV) == sizeof(VkSetPresentConfigNV),
              "local VkSetPresentConfigNV mirror must match the Vulkan headers");
static_assert(offsetof(SetPresentConfigNV, numFramesPerBatch) == offsetof(VkSetPresentConfigNV, numFramesPerBatch),
              "local VkSetPresentConfigNV mirror must match the Vulkan headers");
#endif

// The scan is diagnostic only. It records where the application placed the
// node without modifying the application-owned, const pNext chain.
inline constexpr uint32_t kMaxScannedChainNodes = 32;

struct ChainScan {
    // numFramesPerBatch of the metering request, 0 when the chain carries none.
    uint32_t framesPerBatch = 0;
    bool found = false;
    bool isChainHead = false;
    // Legacy diagnostic: what the chain tail would be for a head node. The
    // presentation path no longer uses this value to unlink metering.
    const void* chainWithoutMetering = nullptr;
    uint32_t nodeCount = 0;
    // The chain was longer than the scan bound, or self-referential.
    bool truncated = false;
};

inline ChainScan ScanPresentChain(const void* pNext) {
    ChainScan scan = {};
    scan.chainWithoutMetering = pNext;

    const auto* node = static_cast<const VkBaseInStructure*>(pNext);
    while (node != nullptr) {
        if (scan.nodeCount >= kMaxScannedChainNodes) {
            scan.truncated = true;
            break;
        }
        ++scan.nodeCount;
        if (!scan.found && node->sType == kStructureTypeSetPresentConfigNV) {
            const auto* config = reinterpret_cast<const SetPresentConfigNV*>(node);
            scan.found = true;
            scan.framesPerBatch = config->numFramesPerBatch;
            scan.isChainHead = (static_cast<const void*>(node) == pNext);
            if (scan.isChainHead)
                scan.chainWithoutMetering = config->pNext;
        }
        node = node->pNext;
    }
    return scan;
}

// `[Graphics] vsync_mode` values that ask for a presented rate bounded by the
// display. These are the same two the layer maps onto FIFO and FIFO_RELAXED at
// swapchain creation; `off`, `mailbox` and `default` deliberately are not.
inline bool RequestsVblankPacedPresentation(std::string_view vsyncMode) {
    return vsyncMode == "fifo" || vsyncMode == "adaptive";
}

inline bool IsVblankPacedPresentMode(VkPresentModeKHR presentMode) {
    return presentMode == VK_PRESENT_MODE_FIFO_KHR || presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
}

struct Input {
    // True when the resolved profile asked CE for vertical-blank-paced
    // presentation (see RequestsVblankPacedPresentation).
    bool vblankPacedPresentationRequested = false;
    // The present mode the swapchain was actually created with, which is the
    // one CE passed to the driver rather than the one the game asked for.
    VkPresentModeKHR swapchainPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    // False when the presented swapchain is not one the layer tracks, in which
    // case its pacing contract is unknown and must not be assumed.
    bool swapchainPresentModeKnown = false;
    // VkPresentInfoKHR::swapchainCount. A multi-swapchain present has no single
    // pacing contract to defend, so the override stays out of it.
    uint32_t swapchainCount = 1;
    uint32_t meteredFramesPerBatch = 0;
};

struct Decision {
    // Keep the driver's generated-frame spacing signal in the present chain.
    // VK_EXT_present_timing supplies the separate minimum display interval.
    bool preserveMetering = false;
};

inline Decision Decide(const Input& input) {
    Decision decision = {};

    if (!input.vblankPacedPresentationRequested)
        return decision;
    if (!input.swapchainPresentModeKnown || !IsVblankPacedPresentMode(input.swapchainPresentMode))
        return decision;
    if (input.swapchainCount != 1)
        return decision;
    // A single-frame batch carries no generated group to diagnose.
    if (input.meteredFramesPerBatch < 2)
        return decision;

    decision.preserveMetering = true;
    return decision;
}

}  // namespace ce::vulkan_present_metering_policy
