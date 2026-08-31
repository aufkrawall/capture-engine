/**
 * VK_LAYER_CE_overlay - device capability forwarding and diagnostics
 *
 * A configured graphics override sometimes has to be applied to what the
 * application is *allowed to ask for*, not to the calls it makes afterwards.
 * Hardware present metering used to be withheld here. Session 20260830_234347
 * proved that doing so destroys generated-frame spacing; the policy now keeps
 * the capability and VK_EXT_present_timing supplies the independent display
 * ceiling. The wrappers remain passthrough-compatible for older deployments.
 */

#include "vulkan_layer_internal.h"

namespace {

// CE advertises no device extensions of its own, so a layer-scoped query that
// names this layer is answered with an empty list rather than the driver's.
bool NamesThisLayer(const char* layerName) {
    if (layerName == nullptr)
        return false;
    return strncmp(layerName, "VK_LAYER_CE_overlay", 19) == 0 ||
           strcmp(layerName, "VK_LAYER_CAPTURE_overlay") == 0;
}

InstanceDispatch* ResolveInstanceDispatchForPhysicalDevice(VkPhysicalDevice physicalDevice) {
    return VulkanLayerState::Get().GetInstanceDispatch(
        VulkanLayerState::Get().GetInstanceFromPhysicalDevice(physicalDevice));
}

}  // namespace

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                           const char* pLayerName,
                                                                           uint32_t* pPropertyCount,
                                                                           VkExtensionProperties* pProperties) {
    if (NamesThisLayer(pLayerName)) {
        if (pPropertyCount)
            *pPropertyCount = 0;
        return VK_SUCCESS;
    }

    InstanceDispatch* disp = ResolveInstanceDispatchForPhysicalDevice(physicalDevice);
    if (!disp || !disp->fp_vkEnumerateDeviceExtensionProperties) {
        LayerLog("Vulkan Layer: [Warn] vkEnumerateDeviceExtensionProperties with no dispatch for physical device %p",
                 (void*)physicalDevice);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Preserve the driver's complete capability contract. In particular,
    // VK_NV_present_metering supplies generated-frame spacing while native
    // relative timing supplies the independent display-rate ceiling.
    return disp->fp_vkEnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
}

namespace {

void GetPhysicalDeviceFeatures2Common(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures,
                                      bool khrAlias) {
    InstanceDispatch* disp = ResolveInstanceDispatchForPhysicalDevice(physicalDevice);
    PFN_vkGetPhysicalDeviceFeatures2 next = disp ? disp->fp_vkGetPhysicalDeviceFeatures2 : nullptr;
    if (!next && disp && disp->fp_vkGetInstanceProcAddr) {
        // The dispatch caches the core spelling only, so a Vulkan 1.0 instance
        // that answers just the KHR alias still resolves here.
        next = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            disp->fp_vkGetInstanceProcAddr(disp->instance, "vkGetPhysicalDeviceFeatures2KHR"));
    }
    if (!next) {
        LayerLog("Vulkan Layer: [Warn] vkGetPhysicalDeviceFeatures2%s with no dispatch for physical device %p",
                 khrAlias ? "KHR" : "", (void*)physicalDevice);
        return;
    }

    next(physicalDevice, pFeatures);
}

}  // namespace

VKAPI_ATTR void VKAPI_CALL Capture_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                                                VkPhysicalDeviceFeatures2* pFeatures) {
    GetPhysicalDeviceFeatures2Common(physicalDevice, pFeatures, false);
}

VKAPI_ATTR void VKAPI_CALL Capture_vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice physicalDevice,
                                                                   VkPhysicalDeviceFeatures2* pFeatures) {
    GetPhysicalDeviceFeatures2Common(physicalDevice, pFeatures, true);
}

// --- presentation-chain diagnostics ---------------------------------------
//
// CE overrides the present mode at swapchain creation and the driver reports it
// back, yet Portal RTX presents that swapchain through DXGI with tearing. The
// only honest way to close that gap is to print the chain CE was actually
// handed at both sites, once per shape.

void LogPresentModeSelectionChain(const char* site, const void* pNext) {
    char text[512] = {};
    const ce::vulkan_present_chain::ChainDescription description =
        ce::vulkan_present_chain::DescribeChain(pNext, text, sizeof(text));
    LayerLog("Vulkan Layer: %s pNext chain: %s (nodes=%u%s%s)", site, text, description.nodeCount,
             description.truncated ? " truncated" : "",
             description.hasPresentModeSelection ? " carries-a-present-mode-selection" : "");
}

void LogPresentChainIfChanged(const char* site, const void* pNext,
                              const ce::vulkan_present_chain::ChainDescription& description,
                              std::atomic<uint32_t>& logState, const char* note) {
    (void)pNext;
    // Signature 0 is a real value for an empty chain, so the gate carries a
    // presence bit of its own.
    const uint32_t state = (description.signature & 0x7FFFFFFFu) | 0x80000000u;
    if (logState.load(std::memory_order_relaxed) == state ||
        logState.exchange(state, std::memory_order_acq_rel) == state) {
        return;
    }
    char text[512] = {};
    ce::vulkan_present_chain::DescribeChain(pNext, text, sizeof(text));
    LayerLog("Vulkan Layer: %s pNext chain: %s (nodes=%u%s) - %s", site, text, description.nodeCount,
             description.truncated ? " truncated" : "", note ? note : "");
}

// Which of the extensions that can move presentation out from under CE's
// creation-time present-mode override the application actually enabled.
// Printing the whole list would be dozens of names; these are the ones a pacing
// investigation needs, and their absence is as informative as their presence.
void LogRequestedPresentationExtensions(const char* const* names, uint32_t count) {
    static const char* const kPresentationExtensions[] = {
        "VK_EXT_swapchain_maintenance1", "VK_KHR_swapchain_maintenance1", "VK_NV_present_metering",
        "VK_KHR_present_id",             "VK_KHR_present_wait",           "VK_NV_low_latency2",
        "VK_GOOGLE_display_timing",      "VK_EXT_present_timing",         "VK_KHR_present_id2",
        "VK_KHR_calibrated_timestamps",
    };
    char enabledText[384] = {};
    size_t written = 0;
    for (const char* candidate : kPresentationExtensions) {
        bool present = false;
        for (uint32_t i = 0; i < count && !present; i++) {
            present = names && names[i] && strcmp(names[i], candidate) == 0;
        }
        if (!present)
            continue;
        const size_t remaining = sizeof(enabledText) - written;
        const int printed = snprintf(enabledText + written, remaining, "%s%s", written ? " " : "", candidate);
        if (printed > 0 && static_cast<size_t>(printed) < remaining)
            written += static_cast<size_t>(printed);
    }
    LayerLog("Vulkan Layer: presentation-relevant device extensions requested: %s", written ? enabledText : "<none>");
}
