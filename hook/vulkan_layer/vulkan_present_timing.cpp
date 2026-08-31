#include "vulkan_layer_internal.h"

#include "vulkan_present_timing_policy.h"

namespace {

bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [&](const VkExtensionProperties& extension) {
        return strcmp(extension.extensionName, name) == 0;
    });
}

bool HasEnabledExtension(const std::vector<const char*>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(),
                       [&](const char* extension) { return extension && strcmp(extension, name) == 0; });
}

bool AppendExtensionIfMissing(std::vector<const char*>& extensions, const char* name) {
    if (HasEnabledExtension(extensions, name))
        return false;
    extensions.push_back(name);
    return true;
}

struct PresentTimingFeatureSearch {
    const VkPhysicalDevicePresentTimingFeaturesEXT* features = nullptr;
    bool complete = true;
};

PresentTimingFeatureSearch FindRequestedPresentTimingFeatures(const void* pNext) {
    const auto* node = static_cast<const VkBaseInStructure*>(pNext);
    for (uint32_t count = 0; node != nullptr && count < ce::vulkan_present_timing_policy::kMaxChainNodes; ++count) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT)
            return {.features = reinterpret_cast<const VkPhysicalDevicePresentTimingFeaturesEXT*>(node),
                    .complete = true};
        if (node->pNext == node)
            return {.features = nullptr, .complete = false};
        node = node->pNext;
    }
    return {.features = nullptr, .complete = node == nullptr};
}

bool ResolvePresentTimingTimeDomain(DeviceDispatch* dispatch, SwapchainData* swapchainData) {
    if (!dispatch || !swapchainData || !dispatch->fp_vkGetSwapchainTimeDomainPropertiesEXT)
        return false;

    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        VkSwapchainTimeDomainPropertiesEXT properties = {};
        properties.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT;
        uint64_t counter = 0;
        VkResult result = dispatch->fp_vkGetSwapchainTimeDomainPropertiesEXT(
            swapchainData->device, swapchainData->swapchain, &properties, &counter);
        if (result != VK_SUCCESS || properties.timeDomainCount == 0)
            return false;

        const bool domainWasValid =
            swapchainData->presentTimingTimeDomainValid.load(std::memory_order_acquire);
        const uint64_t previousCounter =
            swapchainData->presentTimingTimeDomainsCounter.load(std::memory_order_acquire);
        if (domainWasValid && counter == previousCounter)
            return true;

        std::vector<VkTimeDomainKHR> domains(properties.timeDomainCount);
        std::vector<uint64_t> domainIds(properties.timeDomainCount);
        properties.pTimeDomains = domains.data();
        properties.pTimeDomainIds = domainIds.data();
        result = dispatch->fp_vkGetSwapchainTimeDomainPropertiesEXT(
            swapchainData->device, swapchainData->swapchain, &properties, &counter);
        if (result == VK_INCOMPLETE)
            continue;
        if (result != VK_SUCCESS || properties.timeDomainCount == 0 ||
            properties.timeDomainCount > domains.size()) {
            return false;
        }

        uint32_t selected = 0;
        for (uint32_t index = 0; index < properties.timeDomainCount; ++index) {
            if (domains[index] == VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT) {
                selected = index;
                break;
            }
        }
        swapchainData->presentTimingTimeDomain = domains[selected];
        swapchainData->presentTimingTimeDomainId = domainIds[selected];
        swapchainData->presentTimingTimeDomainsCounter.store(counter, std::memory_order_release);
        swapchainData->presentTimingTimeDomainValid.store(true, std::memory_order_release);
        if (domainWasValid && counter != previousCounter) {
            swapchainData->presentTimingRefreshDurationNs.store(0, std::memory_order_release);
            LayerLog("Vulkan Layer: native relative present timing domain changed for swapchain %p "
                     "(domain=%d id=%llu counter=%llu); refresh properties will be queried again",
                     (void*)swapchainData->swapchain, static_cast<int>(domains[selected]),
                     static_cast<unsigned long long>(domainIds[selected]),
                     static_cast<unsigned long long>(counter));
        }
        return true;
    }
    return false;
}

bool RefreshPresentTimingTimeDomain(DeviceDispatch* dispatch, SwapchainData* swapchainData,
                                    uint32_t presentOccurrence) {
    if (!dispatch || !swapchainData || !swapchainData->relativePresentTimingEnabled)
        return false;
    const bool valid = swapchainData->presentTimingTimeDomainValid.load(std::memory_order_acquire);
    if (!ce::vulkan_present_timing_policy::ShouldRefreshTimeDomains(presentOccurrence, valid))
        return true;
    if (ResolvePresentTimingTimeDomain(dispatch, swapchainData))
        return true;

    swapchainData->presentTimingTimeDomainValid.store(false, std::memory_order_release);
    swapchainData->presentTimingRefreshDurationNs.store(0, std::memory_order_release);
    const uint32_t failure =
        swapchainData->presentTimingDomainQueryFailureCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (failure <= 3 || (failure % 256) == 0) {
        LayerLog("Vulkan Layer: native relative present timing domain unavailable for swapchain %p "
                 "(failure=%u); timing will remain inactive and the query will be retried",
                 (void*)swapchainData->swapchain, failure);
    }
    return false;
}

uint64_t RefreshTimingProperties(DeviceDispatch* dispatch, SwapchainData* swapchainData,
                                 uint32_t presentOccurrence) {
    if (!dispatch || !swapchainData || !swapchainData->relativePresentTimingEnabled ||
        !dispatch->fp_vkGetSwapchainTimingPropertiesEXT) {
        return 0;
    }

    const uint64_t cached = swapchainData->presentTimingRefreshDurationNs.load(std::memory_order_acquire);
    if (!ce::vulkan_present_timing_policy::ShouldRefreshTimingProperties(presentOccurrence, cached))
        return cached;

    VkSwapchainTimingPropertiesEXT properties = {};
    properties.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT;
    uint64_t counter = 0;
    const VkResult result = dispatch->fp_vkGetSwapchainTimingPropertiesEXT(
        swapchainData->device, swapchainData->swapchain, &properties, &counter);
    if (result != VK_SUCCESS || properties.refreshDuration == 0) {
        swapchainData->presentTimingRefreshDurationNs.store(0, std::memory_order_release);
        const uint32_t failure =
            swapchainData->presentTimingQueryFailureCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failure <= 3 || (failure % 256) == 0) {
            LayerLog("Vulkan Layer: native relative present timing properties unavailable for swapchain %p "
                     "(result=%d refreshDuration=%llu failure=%u present=%u); retrying after another present",
                     (void*)swapchainData->swapchain, result,
                     static_cast<unsigned long long>(properties.refreshDuration), failure, presentOccurrence);
        }
        return 0;
    }

    const uint64_t previous =
        swapchainData->presentTimingRefreshDurationNs.exchange(properties.refreshDuration, std::memory_order_acq_rel);
    const uint64_t previousCounter =
        swapchainData->presentTimingPropertiesCounter.exchange(counter, std::memory_order_acq_rel);
    if (previous != properties.refreshDuration || previousCounter != counter) {
        LayerLog("Vulkan Layer: native relative present timing ready for swapchain %p "
                 "(minimum refresh duration=%llu ns, refresh interval=%llu ns, counter=%llu%s)",
                 (void*)swapchainData->swapchain,
                 static_cast<unsigned long long>(properties.refreshDuration),
                 static_cast<unsigned long long>(properties.refreshInterval),
                 static_cast<unsigned long long>(counter),
                 properties.refreshInterval == UINT64_MAX ? ", VRR" : ", fixed/unknown refresh");
    }
    return properties.refreshDuration;
}

}  // namespace

void PopulatePresentTimingInstanceDispatch(InstanceDispatch* dispatch, VkInstance instance,
                                           PFN_vkGetInstanceProcAddr gipa) {
    if (!dispatch || !gipa)
        return;
    dispatch->fp_vkGetPhysicalDeviceSurfaceCapabilities2KHR =
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
            gipa(instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
}

void PopulatePresentTimingDeviceDispatch(DeviceDispatch* dispatch, VkDevice device,
                                         PFN_vkGetDeviceProcAddr gdpa) {
    if (!dispatch || !gdpa)
        return;
    dispatch->fp_vkGetSwapchainTimingPropertiesEXT =
        reinterpret_cast<PFN_vkGetSwapchainTimingPropertiesEXT>(
            gdpa(device, "vkGetSwapchainTimingPropertiesEXT"));
    dispatch->fp_vkGetSwapchainTimeDomainPropertiesEXT =
        reinterpret_cast<PFN_vkGetSwapchainTimeDomainPropertiesEXT>(
            gdpa(device, "vkGetSwapchainTimeDomainPropertiesEXT"));
}

bool EnablePresentTimingSurfaceQueries(PFN_vkGetInstanceProcAddr gipa, bool requested,
                                       std::vector<const char*>& extensions, bool* extensionAdded) {
    if (extensionAdded)
        *extensionAdded = false;
    if (!requested)
        return false;
    // VK_KHR_get_surface_capabilities2 depends on VK_KHR_surface. A compute-only
    // Vulkan client can still inherit a FIFO profile, but CE must not make that
    // otherwise-valid instance request fail by adding an extension whose base
    // dependency the application never enabled.
    if (!HasEnabledExtension(extensions, VK_KHR_SURFACE_EXTENSION_NAME)) {
        LayerLog("Vulkan Layer: native relative present timing unavailable: instance extension %s is not enabled",
                 VK_KHR_SURFACE_EXTENSION_NAME);
        return false;
    }
    if (HasEnabledExtension(extensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME))
        return true;
    if (!gipa)
        return false;

    auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    if (!enumerate)
        return false;
    uint32_t count = 0;
    if (enumerate(nullptr, &count, nullptr) != VK_SUCCESS || count == 0)
        return false;
    std::vector<VkExtensionProperties> available(count);
    if (enumerate(nullptr, &count, available.data()) != VK_SUCCESS)
        return false;
    available.resize(count);
    if (!HasExtension(available, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
        LayerLog("Vulkan Layer: native relative present timing unavailable: instance extension %s is absent",
                 VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        return false;
    }

    extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    if (extensionAdded)
        *extensionAdded = true;
    LayerLog("Vulkan Layer: enabled instance extension %s for native FIFO/VRR timing capability queries",
             VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    return true;
}

void PreparePresentTimingDevice(InstanceDispatch* instanceDispatch, VkPhysicalDevice physicalDevice,
                                const VkDeviceCreateInfo& applicationCreateInfo,
                                const std::vector<VkExtensionProperties>& availableExtensions,
                                std::vector<const char*>& enabledExtensions, VkDeviceCreateInfo& modifiedCreateInfo,
                                PresentTimingDeviceEnablement& enablement) {
    ce::vulkan_present_timing_policy::DeviceInput input = {};
    input.vblankPacingRequested = VulkanLayerState::Get().WantsVblankPacedPresentation();
    input.surfaceCapabilities2Enabled =
        instanceDispatch && instanceDispatch->presentTimingSurfaceQueriesEnabled &&
        instanceDispatch->fp_vkGetPhysicalDeviceSurfaceCapabilities2KHR;
    input.swapchainExtensionEnabled = HasEnabledExtension(enabledExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    input.presentTimingExtensionAvailable = HasExtension(availableExtensions, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
    input.presentId2ExtensionAvailable = HasExtension(availableExtensions, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
    input.calibratedTimestampsExtensionAvailable =
        HasExtension(availableExtensions, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);

    PFN_vkGetPhysicalDeviceFeatures2 getFeatures2 =
        instanceDispatch ? instanceDispatch->fp_vkGetPhysicalDeviceFeatures2 : nullptr;
    if (!getFeatures2 && instanceDispatch && instanceDispatch->fp_vkGetInstanceProcAddr) {
        getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            instanceDispatch->fp_vkGetInstanceProcAddr(instanceDispatch->instance,
                                                       "vkGetPhysicalDeviceFeatures2KHR"));
    }
    input.featureQueryAvailable = getFeatures2 != nullptr;
    if (getFeatures2) {
        VkPhysicalDevicePresentTimingFeaturesEXT supportedTiming = {};
        supportedTiming.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT;
        VkPhysicalDeviceFeatures2 supportedFeatures = {};
        supportedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        supportedFeatures.pNext = &supportedTiming;
        getFeatures2(physicalDevice, &supportedFeatures);
        input.presentTimingFeatureSupported = supportedTiming.presentTiming == VK_TRUE;
        input.relativeTimeFeatureSupported = supportedTiming.presentAtRelativeTime == VK_TRUE;
    }

    const PresentTimingFeatureSearch requested =
        FindRequestedPresentTimingFeatures(applicationCreateInfo.pNext);
    input.applicationFeatureChainInspectable = requested.complete || requested.features != nullptr;
    input.applicationSpecifiedFeature = requested.features != nullptr;
    input.applicationEnabledPresentTiming =
        requested.features && requested.features->presentTiming == VK_TRUE;
    input.applicationEnabledRelativeTime =
        requested.features && requested.features->presentAtRelativeTime == VK_TRUE;

    const auto decision = ce::vulkan_present_timing_policy::DecideDevice(input);
    if (!decision.enable) {
        if (input.vblankPacingRequested) {
            LayerLog("Vulkan Layer: native relative present timing unavailable for this device "
                     "(surfaceCaps2=%d swapchain=%d timingExt=%d presentId2=%d calibrated=%d "
                     "featureQuery=%d timingFeature=%d relativeFeature=%d appChain=%d appFeature=%d/%d/%d)",
                     input.surfaceCapabilities2Enabled ? 1 : 0, input.swapchainExtensionEnabled ? 1 : 0,
                     input.presentTimingExtensionAvailable ? 1 : 0, input.presentId2ExtensionAvailable ? 1 : 0,
                     input.calibratedTimestampsExtensionAvailable ? 1 : 0, input.featureQueryAvailable ? 1 : 0,
                     input.presentTimingFeatureSupported ? 1 : 0, input.relativeTimeFeatureSupported ? 1 : 0,
                     input.applicationFeatureChainInspectable ? 1 : 0,
                     input.applicationSpecifiedFeature ? 1 : 0, input.applicationEnabledPresentTiming ? 1 : 0,
                     input.applicationEnabledRelativeTime ? 1 : 0);
        }
        return;
    }

    enablement.extensionCountBefore = enabledExtensions.size();
    enablement.extensionsAdded |=
        AppendExtensionIfMissing(enabledExtensions, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
    enablement.extensionsAdded |=
        AppendExtensionIfMissing(enabledExtensions, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    enablement.extensionsAdded |=
        AppendExtensionIfMissing(enabledExtensions, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
    modifiedCreateInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    modifiedCreateInfo.ppEnabledExtensionNames = enabledExtensions.data();

    if (decision.addFeatureNode) {
        enablement.injectedFeatures.presentTiming = VK_TRUE;
        enablement.injectedFeatures.presentAtRelativeTime = VK_TRUE;
        enablement.injectedFeatures.presentAtAbsoluteTime = VK_FALSE;
        enablement.previousPNext = modifiedCreateInfo.pNext;
        enablement.injectedFeatures.pNext = const_cast<void*>(modifiedCreateInfo.pNext);
        modifiedCreateInfo.pNext = &enablement.injectedFeatures;
        enablement.featureNodeAdded = true;
    }
    enablement.enabled = true;
    LayerLog("Vulkan Layer: native relative present timing enabled for FIFO/VRR ceiling scheduling "
             "(extensionsAdded=%d featureNodeAdded=%d)",
             enablement.extensionsAdded ? 1 : 0, enablement.featureNodeAdded ? 1 : 0);
}

namespace {

void RollBackPresentTimingDevice(std::vector<const char*>& enabledExtensions,
                                 VkDeviceCreateInfo& modifiedCreateInfo,
                                 PresentTimingDeviceEnablement& enablement) {
    if (enablement.extensionsAdded)
        enabledExtensions.resize(enablement.extensionCountBefore);
    if (enablement.featureNodeAdded)
        modifiedCreateInfo.pNext = enablement.previousPNext;
    modifiedCreateInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    modifiedCreateInfo.ppEnabledExtensionNames = enabledExtensions.data();
    enablement.enabled = false;
}

void RestorePresentTimingDevice(std::vector<const char*>& enabledExtensions,
                                VkDeviceCreateInfo& modifiedCreateInfo,
                                PresentTimingDeviceEnablement& enablement) {
    if (enablement.extensionsAdded) {
        AppendExtensionIfMissing(enabledExtensions, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
        AppendExtensionIfMissing(enabledExtensions, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        AppendExtensionIfMissing(enabledExtensions, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
    }
    if (enablement.featureNodeAdded) {
        enablement.injectedFeatures.pNext = const_cast<void*>(enablement.previousPNext);
        modifiedCreateInfo.pNext = &enablement.injectedFeatures;
    }
    modifiedCreateInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    modifiedCreateInfo.ppEnabledExtensionNames = enabledExtensions.data();
    enablement.enabled = true;
}

}  // namespace

VkResult CreateDeviceWithPresentTimingFallback(PFN_vkCreateDevice createFunction,
                                               VkPhysicalDevice physicalDevice,
                                               std::vector<const char*>& enabledExtensions,
                                               VkDeviceCreateInfo& modifiedCreateInfo,
                                               PresentTimingDeviceEnablement& enablement,
                                               const VkAllocationCallbacks* allocator,
                                               VkDevice* device) {
    VkResult result = createFunction(physicalDevice, &modifiedCreateInfo, allocator, device);
    if (result == VK_SUCCESS || (!enablement.extensionsAdded && !enablement.featureNodeAdded))
        return result;

    LayerLog("Vulkan Layer: vkCreateDevice failed with native relative present timing enabled "
             "(result=%d); retrying without CE's optional timing additions", result);
    RollBackPresentTimingDevice(enabledExtensions, modifiedCreateInfo, enablement);
    result = createFunction(physicalDevice, &modifiedCreateInfo, allocator, device);
    if (result == VK_SUCCESS)
        return result;

    RestorePresentTimingDevice(enabledExtensions, modifiedCreateInfo, enablement);
    LayerLog("Vulkan Layer: vkCreateDevice also failed without native relative present timing "
             "(result=%d); restoring timing before unrelated recovery paths", result);
    return result;
}

PresentTimingSwapchainEnablement PreparePresentTimingSwapchain(
    DeviceDispatch* deviceDispatch, const VkSwapchainCreateInfoKHR& applicationCreateInfo,
    VkSwapchainCreateInfoKHR& modifiedCreateInfo) {
    PresentTimingSwapchainEnablement enablement = {};
    ce::vulkan_present_timing_policy::SwapchainInput input = {};
    input.vblankPacingRequested = VulkanLayerState::Get().WantsVblankPacedPresentation();
    input.deviceEnabled = deviceDispatch && deviceDispatch->relativePresentTimingEnabled;
    input.presentMode = modifiedCreateInfo.presentMode;

    VkPresentTimingSurfaceCapabilitiesEXT timingCapabilities = {};
    timingCapabilities.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT;
    VkInstance instance = VK_NULL_HANDLE;
    InstanceDispatch* instanceDispatch = nullptr;
    if (input.deviceEnabled) {
        instance = VulkanLayerState::Get().GetInstanceFromPhysicalDevice(deviceDispatch->physicalDevice);
        instanceDispatch = VulkanLayerState::Get().GetInstanceDispatch(instance);
    }
    input.surfaceQueryAvailable = input.deviceEnabled &&
                                  deviceDispatch->fp_vkGetSwapchainTimingPropertiesEXT &&
                                  deviceDispatch->fp_vkGetSwapchainTimeDomainPropertiesEXT && instanceDispatch &&
                                  instanceDispatch->fp_vkGetPhysicalDeviceSurfaceCapabilities2KHR;
    if (input.surfaceQueryAvailable) {
        VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo = {};
        surfaceInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
        surfaceInfo.surface = applicationCreateInfo.surface;
        VkSurfaceCapabilities2KHR surfaceCapabilities = {
            .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
            .pNext = &timingCapabilities,
            .surfaceCapabilities = {
                .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
            },
        };
        const VkResult result = instanceDispatch->fp_vkGetPhysicalDeviceSurfaceCapabilities2KHR(
            deviceDispatch->physicalDevice, &surfaceInfo, &surfaceCapabilities);
        input.surfaceQuerySucceeded = result == VK_SUCCESS;
        input.presentTimingSupportedForSurface =
            result == VK_SUCCESS && timingCapabilities.presentTimingSupported == VK_TRUE;
        input.relativeTimeSupportedForSurface =
            result == VK_SUCCESS && timingCapabilities.presentAtRelativeTimeSupported == VK_TRUE;
        if (result != VK_SUCCESS) {
            LayerLog("Vulkan Layer: native relative present timing surface query failed (result=%d)", result);
        }
    }

    enablement.enabled = ce::vulkan_present_timing_policy::ShouldEnableSwapchain(input);
    if (!enablement.enabled) {
        if (input.deviceEnabled && ce::vulkan_present_timing_policy::IsFifoPresentMode(input.presentMode)) {
            LayerLog("Vulkan Layer: native relative present timing unavailable for surface %p "
                     "(query=%d success=%d presentTiming=%d relative=%d)",
                     (void*)applicationCreateInfo.surface, input.surfaceQueryAvailable ? 1 : 0,
                     input.surfaceQuerySucceeded ? 1 : 0, input.presentTimingSupportedForSurface ? 1 : 0,
                     input.relativeTimeSupportedForSurface ? 1 : 0);
        }
        return enablement;
    }

    enablement.flagAdded =
        (applicationCreateInfo.flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT) == 0;
    modifiedCreateInfo.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
    LayerLog("Vulkan Layer: enabling native relative present timing for FIFO swapchain on surface %p "
             "(presentTiming=%d relative=%d flagAdded=%d)",
             (void*)applicationCreateInfo.surface, timingCapabilities.presentTimingSupported ? 1 : 0,
             timingCapabilities.presentAtRelativeTimeSupported ? 1 : 0, enablement.flagAdded ? 1 : 0);
    return enablement;
}

void InitializePresentTimingSwapchain(DeviceDispatch* deviceDispatch, SwapchainData* swapchainData) {
    if (!deviceDispatch || !swapchainData || !swapchainData->relativePresentTimingEnabled)
        return;
    const bool timeDomainReady = RefreshPresentTimingTimeDomain(deviceDispatch, swapchainData, 0);
    // Some implementations expose these properties immediately and some only
    // after one image has been presented. Query once here so the first image is
    // scheduled when possible; a native VK_NOT_READY simply leaves the cache at
    // zero and the first/next present retries without inventing a refresh rate.
    RefreshTimingProperties(deviceDispatch, swapchainData, 0);
    if (!timeDomainReady)
        return;
    LayerLog("Vulkan Layer: native relative present timing domain selected for swapchain %p "
             "(domain=%d id=%llu)",
             (void*)swapchainData->swapchain, static_cast<int>(swapchainData->presentTimingTimeDomain),
             static_cast<unsigned long long>(swapchainData->presentTimingTimeDomainId));
}

bool BuildRelativePresentTiming(DeviceDispatch* deviceDispatch, SwapchainData* swapchainData,
                                const VkPresentInfoKHR& applicationPresentInfo, bool fifoPresentModeActive,
                                const void* nextChain,
                                VkPresentTimingInfoEXT& timingInfo, VkPresentTimingsInfoEXT& timingsInfo) {
    ce::vulkan_present_timing_policy::PresentInput input = {};
    input.vblankPacingRequested = VulkanLayerState::Get().WantsVblankPacedPresentation();
    input.swapchainEnabled = swapchainData && swapchainData->relativePresentTimingEnabled;
    input.fifoPresentModeActive = fifoPresentModeActive;
    input.swapchainCount = applicationPresentInfo.swapchainCount;
    const auto chainScan = ce::vulkan_present_timing_policy::ScanChainForStructureType(
        nextChain, VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT);
    input.applicationChainInspectable = chainScan.complete || chainScan.found;
    input.applicationAlreadySpecifiedTiming = chainScan.found;
    if (!input.vblankPacingRequested || !input.swapchainEnabled || !input.fifoPresentModeActive ||
        input.swapchainCount != 1 ||
        !input.applicationChainInspectable || input.applicationAlreadySpecifiedTiming) {
        return false;
    }

    const uint32_t occurrence =
        swapchainData->presentTimingOccurrence.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!RefreshPresentTimingTimeDomain(deviceDispatch, swapchainData, occurrence))
        return false;
    const uint64_t refreshDuration = RefreshTimingProperties(deviceDispatch, swapchainData, occurrence);
    input.refreshDurationNs = refreshDuration;
    if (!ce::vulkan_present_timing_policy::ShouldInjectRelativeTiming(input))
        return false;

    timingInfo = {};
    timingInfo.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT;
    timingInfo.flags = VK_PRESENT_TIMING_INFO_PRESENT_AT_RELATIVE_TIME_BIT_EXT;
    timingInfo.targetTime = refreshDuration;
    timingInfo.timeDomainId = swapchainData->presentTimingTimeDomainId;
    timingInfo.presentStageQueries = 0;
    timingInfo.targetTimeDomainPresentStage =
        swapchainData->presentTimingTimeDomain == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT
            ? VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT
            : static_cast<VkPresentStageFlagsEXT>(0);

    timingsInfo = {};
    timingsInfo.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT;
    timingsInfo.pNext = nextChain;
    timingsInfo.swapchainCount = 1;
    timingsInfo.pTimingInfos = &timingInfo;

    if (!swapchainData->presentTimingActiveLogged.exchange(true, std::memory_order_acq_rel)) {
        LayerLog("Vulkan Layer: native relative FIFO timing active for swapchain %p: each image is scheduled "
                 "no earlier than %llu ns after the previous visible image (no timer or synthetic FPS limiter)",
                 (void*)swapchainData->swapchain, static_cast<unsigned long long>(refreshDuration));
    }
    return true;
}
