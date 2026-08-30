/**
 * VK_LAYER_CE_overlay - device capabilities the layer withholds
 *
 * A configured graphics override sometimes has to be applied to what the
 * application is *allowed to ask for*, not to the calls it makes afterwards.
 * Hardware present metering is the case that forced this unit into existence:
 * it replaces the swapchain's vertical-blank rate contract for the whole
 * lifetime of the device, so `vsync_mode=fifo` cannot be honored by editing
 * present calls once the capability is enabled. See
 * `vulkan_present_metering_policy.h` for the session evidence and the reasoning.
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

bool WantsPresentMeteringWithheld() {
    return g_LayerState.whitelisted.load(std::memory_order_acquire) &&
           VulkanLayerState::Get().WantsVblankPacedPresentation();
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

    const bool withhold = pLayerName == nullptr && WantsPresentMeteringWithheld();
    if (!withhold)
        return disp->fp_vkEnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);

    if (!pPropertyCount)
        return VK_ERROR_INITIALIZATION_FAILED;

    // The count the application is told has to be the filtered one, and a
    // buffer sized from that count has to come back VK_SUCCESS. Filtering the
    // driver's answer in place cannot do both, so the complete list is always
    // fetched first and copied from.
    uint32_t driverCount = 0;
    const VkResult countResult =
        disp->fp_vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &driverCount, nullptr);
    if (countResult != VK_SUCCESS)
        return countResult;

    std::vector<VkExtensionProperties> driverExtensions(driverCount);
    if (driverCount > 0) {
        const VkResult listResult = disp->fp_vkEnumerateDeviceExtensionProperties(
            physicalDevice, nullptr, &driverCount, driverExtensions.data());
        if (listResult != VK_SUCCESS && listResult != VK_INCOMPLETE)
            return listResult;
        driverExtensions.resize(driverCount);
    }

    const ce::vulkan_present_metering_policy::ExtensionFilterResult filtered =
        ce::vulkan_present_metering_policy::CopyWithoutPresentMetering(
            driverExtensions.data(), static_cast<uint32_t>(driverExtensions.size()), pProperties,
            pProperties ? *pPropertyCount : 0);

    if (filtered.removedCount > 0) {
        static std::atomic<int> s_withheldLogCount{0};
        if (s_withheldLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            LayerLog(
                "Vulkan Layer: withholding %s from vkEnumerateDeviceExtensionProperties (%u of %u device "
                "extensions) because vsync_mode=%s asks for vertical-blank-paced presentation; hardware present "
                "metering would replace that rate contract for the life of the device",
                ce::vulkan_present_metering_policy::kPresentMeteringExtensionName, filtered.removedCount, driverCount,
                VulkanLayerState::Get().GetVsyncMode());
        }
    }

    if (!pProperties) {
        *pPropertyCount = filtered.filteredTotal;
        return VK_SUCCESS;
    }
    *pPropertyCount = filtered.writtenCount;
    return filtered.truncated ? VK_INCOMPLETE : VK_SUCCESS;
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

    // A runtime that gates on VkPhysicalDevicePresentMeteringFeaturesNV instead
    // of the extension name has to reach the same answer the enumeration above
    // gave it. pFeatures is application-owned *output*, so clearing the flag
    // there is the documented contract rather than a write into its own state.
    if (!pFeatures || !WantsPresentMeteringWithheld())
        return;
    const uint32_t cleared = ce::vulkan_present_metering_policy::ClearPresentMeteringFeature(pFeatures->pNext);
    if (cleared > 0) {
        static std::atomic<int> s_clearedLogCount{0};
        if (s_clearedLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            LayerLog(
                "Vulkan Layer: reported presentMetering=VK_FALSE from vkGetPhysicalDeviceFeatures2%s to match the "
                "withheld %s (vsync_mode=%s)",
                khrAlias ? "KHR" : "", ce::vulkan_present_metering_policy::kPresentMeteringExtensionName,
                VulkanLayerState::Get().GetVsyncMode());
        }
    }
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

// The enabled-extension list is the second half of the same decision: an
// application that queried before CE was configured, or one that enables the
// extension without querying at all, still must not reach a device where the
// driver owns presentation pacing. Returns the filtered list; `removed` reports
// whether anything was taken out so the caller can retry transactionally.
std::vector<const char*> FilterWithheldDeviceExtensions(const char* const* names, uint32_t count, bool* removed) {
    std::vector<const char*> filtered;
    filtered.reserve(count);
    bool anyRemoved = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (WantsPresentMeteringWithheld() &&
            ce::vulkan_present_metering_policy::IsPresentMeteringExtensionName(names[i])) {
            anyRemoved = true;
            continue;
        }
        filtered.push_back(names[i]);
    }
    if (removed)
        *removed = anyRemoved;
    if (anyRemoved) {
        LayerLog(
            "Vulkan Layer: removing %s from vkCreateDevice because vsync_mode=%s asks for vertical-blank-paced "
            "presentation; the frame-generation runtime keeps its own CPU pacer",
            ce::vulkan_present_metering_policy::kPresentMeteringExtensionName,
            VulkanLayerState::Get().GetVsyncMode());
    }
    return filtered;
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
        "VK_GOOGLE_display_timing",
    };
    char enabledText[256] = {};
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
