#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

// What else can decide how a swapchain is presented, besides the present mode
// CE overrides at `vkCreateSwapchainKHR`.
//
// Portal RTX sessions `20260830_175147` and `20260830_182939` both show the
// same contradiction: `vulkan_layer.log` reports `Overriding present mode 0 ->
// 2 (fifo)` and `vkCreateSwapchainKHR driver returned: 0 (presentMode=2 ...)`,
// while `hook_debug.log` reports the driver presenting that very swapchain
// through DXGI with `SyncInterval=0` plus `DXGI_PRESENT_ALLOW_TEARING`. In the
// same process, on a swapchain generation the application itself created as
// FIFO, the driver used `SyncInterval=1`. So the WSI does honour FIFO - the
// effective mode of the overridden swapchain simply is not FIFO.
//
// `VK_EXT_swapchain_maintenance1` (core-promoted as
// `VK_KHR_swapchain_maintenance1`) is how that happens without CE seeing it:
// the application lists compatible modes in
// `VkSwapchainPresentModesCreateInfoEXT` at creation and then selects one per
// present with `VkSwapchainPresentModeInfoEXT`, no recreation involved. RTX
// Remix documents that it turns V-Sync off while frame generation is active,
// and DXVK - which Remix is built on - uses this extension for exactly that
// kind of switch.
//
// This header is deliberately two things at once: the decision that forces a
// per-present selection back to the mode the swapchain was created with, and a
// plain description of the chain for the log. The second exists because the
// first is a hypothesis until a session prints the chain it was handed - the
// previous round of this investigation cost a game session by fixing a
// mechanism that turned out not to be the one in play.

namespace ce::vulkan_present_chain {

// Mirrored locally for the same reason the metering structures are: the Linux
// MSYS2 headers in this tree predate parts of this, the Windows ones do not.
inline constexpr VkStructureType kStructureTypeSwapchainPresentModesCreateInfo =
    static_cast<VkStructureType>(1000275002);
inline constexpr VkStructureType kStructureTypeSwapchainPresentModeInfo = static_cast<VkStructureType>(1000275003);

struct SwapchainPresentModeInfo {
    VkStructureType sType;
    const void* pNext;
    uint32_t swapchainCount;
    const VkPresentModeKHR* pPresentModes;
};

struct SwapchainPresentModesCreateInfo {
    VkStructureType sType;
    const void* pNext;
    uint32_t presentModeCount;
    const VkPresentModeKHR* pPresentModes;
};

#ifdef VK_KHR_swapchain_maintenance1
static_assert(static_cast<int>(kStructureTypeSwapchainPresentModeInfo) ==
                  static_cast<int>(VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR),
              "local swapchain-maintenance1 sType must match the Vulkan headers");
static_assert(sizeof(SwapchainPresentModeInfo) == sizeof(VkSwapchainPresentModeInfoKHR),
              "local VkSwapchainPresentModeInfoKHR mirror must match the Vulkan headers");
static_assert(offsetof(SwapchainPresentModeInfo, pPresentModes) ==
                  offsetof(VkSwapchainPresentModeInfoKHR, pPresentModes),
              "local VkSwapchainPresentModeInfoKHR mirror must match the Vulkan headers");
#endif

inline constexpr uint32_t kMaxScannedChainNodes = 32;

// Names only the structures that can change presentation behaviour; everything
// else is reported by number, which is enough to spot an unexpected node.
inline const char* NodeName(VkStructureType sType) {
    switch (static_cast<int>(sType)) {
    case 1000275001:
        return "SwapchainPresentFenceInfo";
    case 1000275002:
        return "SwapchainPresentModesCreateInfo";
    case 1000275003:
        return "SwapchainPresentModeInfo";
    case 1000275004:
        return "SwapchainPresentScalingCreateInfo";
    case 1000294000:
        return "PresentId";
    case 1000084000:
        return "PresentRegions";
    case 1000092000:
        return "PresentTimesInfoGOOGLE";
    case 1000505005:
        return "LatencySubmissionPresentIdNV";
    case 1000613000:
        return "SetPresentConfigNV";
    default:
        return nullptr;
    }
}

struct ChainDescription {
    uint32_t nodeCount = 0;
    bool truncated = false;
    // Distinct enough to gate "log only when the shape changed" without keeping
    // the string around.
    uint32_t signature = 0;
    // The per-present mode selection, when the chain carries one.
    bool hasPresentModeSelection = false;
    bool presentModeSelectionIsChainHead = false;
    uint32_t presentModeSelectionSwapchainCount = 0;
    VkPresentModeKHR selectedPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    // VkPresentInfoKHR::pNext with the selection node removed; only meaningful
    // when that node is the chain head.
    const void* chainWithoutPresentModeSelection = nullptr;
};

// `out` receives a human-readable list such as
// "SwapchainPresentModeInfo(immediate) -> PresentId". It is always
// NUL-terminated when outSize is non-zero.
inline ChainDescription DescribeChain(const void* pNext, char* out, size_t outSize) {
    ChainDescription description = {};
    description.chainWithoutPresentModeSelection = pNext;
    size_t written = 0;
    if (out && outSize > 0)
        out[0] = '\0';

    const auto append = [&](const char* text) {
        if (!out || outSize == 0)
            return;
        const size_t remaining = outSize - written;
        if (remaining <= 1)
            return;
        const int printed = std::snprintf(out + written, remaining, "%s", text);
        if (printed > 0)
            written += static_cast<size_t>(printed) < remaining ? static_cast<size_t>(printed) : remaining - 1;
    };

    const auto* node = static_cast<const VkBaseInStructure*>(pNext);
    while (node != nullptr) {
        if (description.nodeCount >= kMaxScannedChainNodes) {
            description.truncated = true;
            break;
        }
        if (description.nodeCount > 0)
            append(" -> ");
        ++description.nodeCount;
        description.signature = description.signature * 31u + static_cast<uint32_t>(node->sType);

        const char* name = NodeName(node->sType);
        char numeric[32] = {};
        if (!name) {
            std::snprintf(numeric, sizeof(numeric), "sType=%d", static_cast<int>(node->sType));
            name = numeric;
        }
        append(name);

        if (node->sType == kStructureTypeSwapchainPresentModeInfo) {
            const auto* selection = reinterpret_cast<const SwapchainPresentModeInfo*>(node);
            description.hasPresentModeSelection = true;
            description.presentModeSelectionSwapchainCount = selection->swapchainCount;
            description.presentModeSelectionIsChainHead = (static_cast<const void*>(node) == pNext);
            if (description.presentModeSelectionIsChainHead)
                description.chainWithoutPresentModeSelection = selection->pNext;
            if (selection->pPresentModes && selection->swapchainCount > 0) {
                description.selectedPresentMode = selection->pPresentModes[0];
                char modeText[24] = {};
                std::snprintf(modeText, sizeof(modeText), "(mode=%d)",
                              static_cast<int>(description.selectedPresentMode));
                append(modeText);
            }
        }
        node = node->pNext;
    }
    if (description.nodeCount == 0)
        append("<empty>");
    return description;
}

struct PresentModeSelectionInput {
    // The resolved profile asked CE for vertical-blank-paced presentation.
    bool vblankPacedPresentationRequested = false;
    // The mode CE actually passed to vkCreateSwapchainKHR for this swapchain.
    VkPresentModeKHR swapchainPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    bool swapchainPresentModeKnown = false;
    uint32_t presentSwapchainCount = 1;
    bool hasPresentModeSelection = false;
    bool presentModeSelectionIsChainHead = false;
    uint32_t presentModeSelectionSwapchainCount = 0;
    VkPresentModeKHR selectedPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
};

struct PresentModeSelectionDecision {
    // Substitute CE's own selection node forcing swapchainPresentMode.
    bool forceCreatedMode = false;
    // The selection conflicts with the configured pacing but sits deeper in the
    // chain than the layer can replace without writing application memory.
    bool blockedByChainPosition = false;
};

// The forced value is always the mode the swapchain was *created* with, never a
// mode of CE's choosing: Vulkan requires a per-present selection to name one of
// the modes the swapchain declared compatible, and the creation mode is the one
// value guaranteed to be in that set.
inline PresentModeSelectionDecision DecidePresentModeSelection(const PresentModeSelectionInput& input) {
    PresentModeSelectionDecision decision = {};
    if (!input.vblankPacedPresentationRequested || !input.hasPresentModeSelection)
        return decision;
    if (!input.swapchainPresentModeKnown)
        return decision;
    if (input.swapchainPresentMode != VK_PRESENT_MODE_FIFO_KHR &&
        input.swapchainPresentMode != VK_PRESENT_MODE_FIFO_RELAXED_KHR) {
        return decision;
    }
    // A multi-swapchain present has no single contract to defend, and a
    // selection array with a different length than CE would write is not one it
    // can safely replace.
    if (input.presentSwapchainCount != 1 || input.presentModeSelectionSwapchainCount != 1)
        return decision;
    if (input.selectedPresentMode == input.swapchainPresentMode)
        return decision;

    if (!input.presentModeSelectionIsChainHead) {
        decision.blockedByChainPosition = true;
        return decision;
    }
    decision.forceCreatedMode = true;
    return decision;
}

}  // namespace ce::vulkan_present_chain
