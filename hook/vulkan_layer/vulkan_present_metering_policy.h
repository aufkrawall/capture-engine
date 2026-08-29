#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

// Frame-generation present metering versus the swapchain's own FIFO pacing.
//
// `VK_PRESENT_MODE_FIFO_KHR` is a rate contract: one presented image per
// vertical blank, so the presented frame rate can never exceed the display's
// refresh rate. `VK_NV_present_metering` is a competing pacing authority for
// the same presents: the application chains `VkSetPresentConfigNV` onto
// `VkPresentInfoKHR` and asks the driver to spread a batch of
// `numFramesPerBatch` images evenly across one *rendered* frame interval, which
// is what multi-frame generation needs to place its generated frames. The batch
// interval is derived from the base frame rate and knows nothing about the
// display, so on a metered present the driver stops applying the swapchain's
// vertical-blank wait: the observed output rate becomes base x
// numFramesPerBatch and runs straight past the refresh rate.
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
// Remix documents both sides of this itself: `rtx.dlfg.enablePresentMetering`
// is described as "Use hardware present metering for DLSS 4.0 frame generation
// instead of CPU pacing", and its V-Sync option carries "When Frame Generation
// is active, V-Sync is automatically disabled". Hardware metering therefore has
// a supported fallback - the runtime's own CPU pacer - and nothing is lost by
// declining it.
//
// So when the profile explicitly asks for vertical-blank-paced presentation,
// CE removes the metering request from the present chain and lets the
// swapchain's FIFO mode be the single pacing authority again. The frames of a
// generated group then land on consecutive vertical blanks, which is both
// evenly paced and rate-limited by construction, and normal present
// backpressure lowers the base rate to refresh / numFramesPerBatch. The
// override is never applied to a present mode that has no rate contract to
// defend (immediate, mailbox) or to a profile that did not ask for one.

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

// A pNext chain is app-owned and const, so a node can only be unlinked without
// writing to application memory when it is the head: the layer then points its
// own VkPresentInfoKHR copy past it. A deeper node would require rewriting the
// predecessor's pNext, which the layer must not do; that case is reported so a
// session can prove it happened instead of silently doing nothing.
inline constexpr uint32_t kMaxScannedChainNodes = 32;

struct ChainScan {
    // numFramesPerBatch of the metering request, 0 when the chain carries none.
    uint32_t framesPerBatch = 0;
    bool found = false;
    bool isChainHead = false;
    // VkPresentInfoKHR::pNext with the metering node removed. Only meaningful
    // when the node is the chain head.
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
    bool meteringRequestIsChainHead = false;
};

struct Decision {
    // Unlink the metering request from the present chain.
    bool suppressMetering = false;
    // The metering request conflicts with the configured pacing but sits deeper
    // in the chain than the layer can unlink without writing app memory.
    bool blockedByChainPosition = false;
};

inline Decision Decide(const Input& input) {
    Decision decision = {};

    if (!input.vblankPacedPresentationRequested)
        return decision;
    if (!input.swapchainPresentModeKnown || !IsVblankPacedPresentMode(input.swapchainPresentMode))
        return decision;
    if (input.swapchainCount != 1)
        return decision;
    // A single-frame batch asks the driver for nothing the swapchain does not
    // already do, so there is no conflict to resolve.
    if (input.meteredFramesPerBatch < 2)
        return decision;

    if (!input.meteringRequestIsChainHead) {
        decision.blockedByChainPosition = true;
        return decision;
    }

    decision.suppressMetering = true;
    return decision;
}

}  // namespace ce::vulkan_present_metering_policy
