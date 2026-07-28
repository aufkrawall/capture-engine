    if (!chain_info) {
        LayerLog(
            "Vulkan Layer: [Error] VK_LAYER_LINK_INFO not found in pNext chain "
            "after %u iterations",
            chainDepth);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    LayerLog("Vulkan Layer: Found VK_LAYER_LINK_INFO at depth %u", chainDepth);
    gipa = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    LayerLog("Vulkan Layer: pfnNextGetInstanceProcAddr=%p", (void*)gipa);
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    PFN_vkCreateInstance create_fn = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");

    VkResult res = VK_SUCCESS;

    if (!g_LayerState.whitelisted) {
        // Passthrough: call next layer directly without modification
        res = create_fn(pCreateInfo, pAllocator, pInstance);
    } else {
        // Inject required extensions
        std::vector<const char*> extensions;
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
            extensions.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
        }

        bool hasProps2 = false;
        bool hasExtMemCaps = false;
        for (const char* ext : extensions) {
            if (strcmp(ext, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0)
                hasProps2 = true;
            if (strcmp(ext, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME) == 0)
                hasExtMemCaps = true;
        }

        if (!hasProps2)
            extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        if (!hasExtMemCaps)
            extensions.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);

        VkInstanceCreateInfo modifiedCreateInfo = *pCreateInfo;
        modifiedCreateInfo.enabledExtensionCount = (uint32_t)extensions.size();
        modifiedCreateInfo.ppEnabledExtensionNames = extensions.data();

        // Enable validation layers in debug builds
#ifdef _DEBUG
        const char* validationLayerName = "VK_LAYER_KHRONOS_validation";
        bool hasValidationLayer = false;

        // Check if already requested
        for (uint32_t i = 0; i < modifiedCreateInfo.enabledLayerCount; i++) {
            if (strcmp(modifiedCreateInfo.ppEnabledLayerNames[i], validationLayerName) == 0) {
                hasValidationLayer = true;
                break;
            }
        }

        if (!hasValidationLayer) {
            // Query available layers
            uint32_t layerCount = 0;
            PFN_vkEnumerateInstanceLayerProperties enumerateLayers =
                (PFN_vkEnumerateInstanceLayerProperties)gipa(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
            if (enumerateLayers) {
                enumerateLayers(&layerCount, nullptr);
                std::vector<VkLayerProperties> availableLayers(layerCount);
                enumerateLayers(&layerCount, availableLayers.data());

                // Check if validation layer is available
                for (const auto& layer : availableLayers) {
                    if (strcmp(layer.layerName, validationLayerName) == 0) {
                        static const char* validationLayers[] = {validationLayerName};
                        modifiedCreateInfo.enabledLayerCount = 1;
                        modifiedCreateInfo.ppEnabledLayerNames = validationLayers;
                        LayerLog("Vulkan Layer: Enabling validation layer: %s", validationLayerName);
                        break;
                    }
                }
            }
        }
#endif

        LayerLog("Vulkan Layer: Calling next vkCreateInstance...");
        res = create_fn(&modifiedCreateInfo, pAllocator, pInstance);
    }

    LayerLog("Vulkan Layer: next vkCreateInstance returned %d", res);
    if (res != VK_SUCCESS)
        return res;

    auto* dispatch = new InstanceDispatch();
    PopulateInstanceDispatch(dispatch, *pInstance, gipa);
    VulkanLayerState::Get().RegisterInstance(*pInstance, dispatch);

    LayerLog("Vulkan Layer: Capture_vkCreateInstance END - success, instance=%p", (void*)*pInstance);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    InstanceDispatch* disp = VulkanLayerState::Get().GetInstanceDispatch(instance);
    if (disp && disp->fp_vkDestroyInstance)
        disp->fp_vkDestroyInstance(instance, pAllocator);
    VulkanLayerState::Get().UnregisterInstance(instance);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount,
                                                                  VkPhysicalDevice* pPhysicalDevices) {
    InstanceDispatch* disp = VulkanLayerState::Get().GetInstanceDispatch(instance);
    if (!disp || !disp->fp_vkEnumeratePhysicalDevices)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = disp->fp_vkEnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);

    if (res >= VK_SUCCESS && pPhysicalDevices && pPhysicalDeviceCount && *pPhysicalDeviceCount > 0) {
        for (uint32_t i = 0; i < *pPhysicalDeviceCount; i++) {
            VulkanLayerState::Get().TrackPhysicalDevice(pPhysicalDevices[i], instance);
        }
    }

    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                      const VkDeviceCreateInfo* pCreateInfo,
                                                      const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    LayerLog("Vulkan Layer: Capture_vkCreateDevice BEGIN");

    if (!pCreateInfo) {
        LayerLog("Vulkan Layer: [Error] Capture_vkCreateDevice called with NULL pCreateInfo");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (!pDevice) {
        LayerLog("Vulkan Layer: [Error] Capture_vkCreateDevice called with NULL pDevice");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    LayerLog(
        "Vulkan Layer: Device create info - queueCreateInfoCount=%u, "
        "enabledExtensionCount=%u, enabledLayerCount=%u",
        pCreateInfo->queueCreateInfoCount, pCreateInfo->enabledExtensionCount, pCreateInfo->enabledLayerCount);

    VkInstance instance = VulkanLayerState::Get().GetInstanceFromPhysicalDevice(physicalDevice);
    if (instance == VK_NULL_HANDLE) {
        LayerLog("Vulkan Layer: [Error] Could not find instance for physical device %p", physicalDevice);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    LayerLog("Vulkan Layer: Found instance %p for physical device %p", (void*)instance, (void*)physicalDevice);

    VkLayerDeviceCreateInfo* chain_info = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    LayerLog("Vulkan Layer: Searching for VK_LAYER_LINK_INFO in device pNext chain...");
    uint32_t chainDepth = 0;
    while (chain_info && !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                           chain_info->function == VK_LAYER_LINK_INFO)) {
        LayerLog("Vulkan Layer:   chain[%u] sType=%u, function=%u", chainDepth, chain_info->sType,
                 chain_info->function);
        chain_info = (VkLayerDeviceCreateInfo*)chain_info->pNext;
        chainDepth++;
    }
    if (!chain_info) {
        LayerLog(
            "Vulkan Layer: [Error] VK_LAYER_LINK_INFO not found in device pNext chain "
            "after %u iterations",
            chainDepth);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    LayerLog("Vulkan Layer: Found VK_LAYER_LINK_INFO at depth %u", chainDepth);

    PFN_vkGetDeviceProcAddr gdpa = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkGetInstanceProcAddr gipa = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    PFN_vkCreateDevice create_fn = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
    if (!create_fn) {
        LayerLog(
            "Vulkan Layer: [Error] Failed to get next vkCreateDevice from "
            "instance %p",
            instance);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result = VK_SUCCESS;
    bool captureInteropEnabled = false;
    bool samplerAnisotropyEnabled = false;
    float maxSamplerAnisotropy = 1.0f;
    float maxSamplerLodBias = 0.0f;

    if (!g_LayerState.whitelisted) {
        // Passthrough: call next layer directly without modification
        result = create_fn(physicalDevice, pCreateInfo, pAllocator, pDevice);
    } else {
        InstanceDispatch* instanceDispatch = VulkanLayerState::Get().GetInstanceDispatch(instance);
        std::vector<VkExtensionProperties> availableExtensions;
        if (instanceDispatch && instanceDispatch->fp_vkEnumerateDeviceExtensionProperties) {
            uint32_t extensionCount = 0;
            if (instanceDispatch->fp_vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount,
                                                                          nullptr) == VK_SUCCESS &&
                extensionCount > 0) {
                availableExtensions.resize(extensionCount);
                if (instanceDispatch->fp_vkEnumerateDeviceExtensionProperties(
                        physicalDevice, nullptr, &extensionCount, availableExtensions.data()) != VK_SUCCESS) {
                    availableExtensions.clear();
                }
            }
        }
        auto extensionAvailable = [&](const char* name) {
            return std::any_of(availableExtensions.begin(), availableExtensions.end(),
                               [&](const VkExtensionProperties& ext) { return strcmp(ext.extensionName, name) == 0; });
        };

        VkPhysicalDeviceProperties physicalProperties = {};
        if (instanceDispatch && instanceDispatch->fp_vkGetPhysicalDeviceProperties)
            instanceDispatch->fp_vkGetPhysicalDeviceProperties(physicalDevice, &physicalProperties);
        maxSamplerAnisotropy = physicalProperties.limits.maxSamplerAnisotropy;
        maxSamplerLodBias = physicalProperties.limits.maxSamplerLodBias;
        const bool externalMemoryCore = physicalProperties.apiVersion >= VK_API_VERSION_1_1;
        const bool externalSemaphoreCore = physicalProperties.apiVersion >= VK_API_VERSION_1_1;
        const bool timelineCore = physicalProperties.apiVersion >= VK_API_VERSION_1_2;

        PFN_vkGetPhysicalDeviceFeatures2 getFeatures2 =
            instanceDispatch ? instanceDispatch->fp_vkGetPhysicalDeviceFeatures2 : nullptr;
        if (!getFeatures2) {
            getFeatures2 =
                reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(instance, "vkGetPhysicalDeviceFeatures2KHR"));
        }
        VkPhysicalDeviceTimelineSemaphoreFeatures supportedTimeline = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        if (getFeatures2) {
            VkPhysicalDeviceFeatures2 supportedFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            supportedFeatures.pNext = &supportedTimeline;
            getFeatures2(physicalDevice, &supportedFeatures);
        }

        const bool requiredExtensionsAvailable =
            (externalMemoryCore || extensionAvailable(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME)) &&
            extensionAvailable(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) &&
            (externalSemaphoreCore || extensionAvailable(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME)) &&
            extensionAvailable(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME) &&
            (timelineCore || extensionAvailable(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME));

        const VkPhysicalDeviceTimelineSemaphoreFeatures* requestedTimeline = nullptr;
        const VkPhysicalDeviceVulkan12Features* requestedVulkan12 = nullptr;
        for (const VkBaseInStructure* node = reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext); node;
             node = node->pNext) {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
                requestedTimeline = reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(node);
            } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
                requestedVulkan12 = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(node);
            }
        }
        const bool canEnableTimeline = supportedTimeline.timelineSemaphore == VK_TRUE;
        const bool appSpecifiedTimeline = requestedTimeline || requestedVulkan12;
        const bool appAlreadyEnabledTimeline = (requestedTimeline && requestedTimeline->timelineSemaphore == VK_TRUE) ||
                                               (requestedVulkan12 && requestedVulkan12->timelineSemaphore == VK_TRUE);
        captureInteropEnabled =
            requiredExtensionsAvailable && canEnableTimeline && (!appSpecifiedTimeline || appAlreadyEnabledTimeline);

        // Inject capture extensions only when the physical device actually
        // advertises the complete Win32 external-memory/fence contract. Never
        // make the game's vkCreateDevice fail merely because capture is absent.
        std::vector<const char*> extensions;
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
            extensions.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
        }

        bool hasExtMem = false;
        bool hasExtMemWin32 = false;
        bool hasExtSem = false;
        bool hasExtSemWin32 = false;
        bool hasTimeline = false;
        for (const char* ext : extensions) {
            if (strcmp(ext, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) == 0)
                hasExtMem = true;
            if (strcmp(ext, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) == 0)
                hasExtMemWin32 = true;
            if (strcmp(ext, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME) == 0)
                hasExtSem = true;
            if (strcmp(ext, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME) == 0)
                hasExtSemWin32 = true;
            if (strcmp(ext, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0)
                hasTimeline = true;
        }

        if (captureInteropEnabled && !hasExtMem && !externalMemoryCore)
            extensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
        if (captureInteropEnabled && !hasExtMemWin32)
            extensions.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
        if (captureInteropEnabled && !hasExtSem && !externalSemaphoreCore)
            extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
        if (captureInteropEnabled && !hasExtSemWin32)
            extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
        if (captureInteropEnabled && !hasTimeline && !timelineCore)
            extensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);

        VkDeviceCreateInfo modifiedCreateInfo = *pCreateInfo;
        modifiedCreateInfo.enabledExtensionCount = (uint32_t)extensions.size();
        modifiedCreateInfo.ppEnabledExtensionNames = extensions.data();

        VkPhysicalDeviceFeatures enabledFeatures = {};
        if (pCreateInfo->pEnabledFeatures) {
            enabledFeatures = *pCreateInfo->pEnabledFeatures;
            samplerAnisotropyEnabled = enabledFeatures.samplerAnisotropy == VK_TRUE;
            modifiedCreateInfo.pEnabledFeatures = &enabledFeatures;
        } else {
            for (const VkBaseInStructure* node = reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext); node;
                 node = node->pNext) {
                if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                    const auto* features2 = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node);
                    samplerAnisotropyEnabled = features2->features.samplerAnisotropy == VK_TRUE;
                    break;
                }
            }
        }

        // Enable timeline semaphores only when the app did not already include
        // the feature structure. Duplicating an sType in pNext is invalid.
        VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        timelineFeatures.timelineSemaphore = VK_TRUE;
        if (captureInteropEnabled && !appSpecifiedTimeline) {
            timelineFeatures.pNext = (void*)modifiedCreateInfo.pNext;
            modifiedCreateInfo.pNext = &timelineFeatures;
        }

        if (!captureInteropEnabled) {
            LayerLog(
                "Vulkan Layer: Win32 external capture unavailable; creating device without capture-only "
                "extensions (extensions=%d timelineSupported=%d timelineRequested=%d)",
                requiredExtensionsAvailable ? 1 : 0, canEnableTimeline ? 1 : 0,
                appSpecifiedTimeline ? (appAlreadyEnabledTimeline ? 1 : 0) : -1);
        }

        LayerLog("Vulkan Layer: Calling next vkCreateDevice...");
        result = create_fn(physicalDevice, &modifiedCreateInfo, pAllocator, pDevice);
    }

    LayerLog("Vulkan Layer: next vkCreateDevice returned %d", result);
    if (result != VK_SUCCESS)
        return result;

    auto* dispatch = new DeviceDispatch();
    dispatch->physicalDevice = physicalDevice;
    dispatch->captureInteropEnabled = captureInteropEnabled;
    dispatch->samplerAnisotropyEnabled = samplerAnisotropyEnabled;
    dispatch->maxSamplerAnisotropy = maxSamplerAnisotropy;
    dispatch->maxSamplerLodBias = maxSamplerLodBias;
    PopulateDeviceDispatch(dispatch, *pDevice, gdpa);
    VulkanLayerState::Get().RegisterDevice(*pDevice, dispatch);

    LayerLog("Vulkan Layer: Capture_vkCreateDevice END - success, device=%p", (void*)*pDevice);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    CleanupOverlay(device);
    CleanupCapture(device);
    CleanupPrerenderFences(device);
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp && disp->fp_vkDestroyDevice)
        disp->fp_vkDestroyDevice(device, pAllocator);
    VulkanLayerState::Get().UnregisterDevice(device);
}

VKAPI_ATTR void VKAPI_CALL Capture_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex,
                                                    VkQueue* pQueue) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp && disp->fp_vkGetDeviceQueue) {
        disp->fp_vkGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
        if (pQueue && *pQueue != VK_NULL_HANDLE) {
            VulkanLayerState::Get().RegisterQueue(*pQueue, device, queueFamilyIndex);
        }
    }
}

VKAPI_ATTR void VKAPI_CALL Capture_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo,
                                                     VkQueue* pQueue) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp || !disp->fp_vkGetDeviceQueue2 || !pQueueInfo) {
        return;
    }

    disp->fp_vkGetDeviceQueue2(device, pQueueInfo, pQueue);
    if (pQueue && *pQueue != VK_NULL_HANDLE) {
        VulkanLayerState::Get().RegisterQueue(*pQueue, device, pQueueInfo->queueFamilyIndex);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSwapchainKHR(VkDevice device,
                                                            const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                            const VkAllocationCallbacks* pAllocator,
                                                            VkSwapchainKHR* pSwapchain) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp || !disp->fp_vkCreateSwapchainKHR)
        return VK_ERROR_INITIALIZATION_FAILED;

    // Apply config overrides
    VkSwapchainCreateInfoKHR modifiedCI = *pCreateInfo;
    bool modified = false;

    if (g_LayerState.whitelisted) {
        // VSync / Present mode override
        const char* vsyncMode = VulkanLayerState::Get().GetVsyncMode();
        if (vsyncMode && strcmp(vsyncMode, "default") != 0) {
            VkPresentModeKHR desiredMode = pCreateInfo->presentMode;
            if (strcmp(vsyncMode, "off") == 0)
                desiredMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else if (strcmp(vsyncMode, "fifo") == 0)
                desiredMode = VK_PRESENT_MODE_FIFO_KHR;
            else if (strcmp(vsyncMode, "mailbox") == 0)
                desiredMode = VK_PRESENT_MODE_MAILBOX_KHR;
            else if (strcmp(vsyncMode, "adaptive") == 0)
                desiredMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;

            if (desiredMode != pCreateInfo->presentMode) {
                // Validate that the desired present mode is supported
                bool modeSupported = false;
                VkPhysicalDevice physDev = disp->physicalDevice;
                VkInstance inst = VulkanLayerState::Get().GetInstanceFromPhysicalDevice(physDev);
                InstanceDispatch* instDisp = VulkanLayerState::Get().GetInstanceDispatch(inst);
                if (instDisp && instDisp->fp_vkGetPhysicalDeviceSurfacePresentModesKHR) {
                    uint32_t modeCount = 0;
                    instDisp->fp_vkGetPhysicalDeviceSurfacePresentModesKHR(physDev, pCreateInfo->surface, &modeCount,
                                                                           nullptr);
                    if (modeCount > 0) {
                        std::vector<VkPresentModeKHR> modes(modeCount);
                        instDisp->fp_vkGetPhysicalDeviceSurfacePresentModesKHR(physDev, pCreateInfo->surface,
                                                                               &modeCount, modes.data());
                        for (uint32_t i = 0; i < modeCount; i++) {
                            if (modes[i] == desiredMode) {
                                modeSupported = true;
                                break;
                            }
                        }
                    }
                } else {
                    // Can't validate, assume FIFO is always supported per Vulkan spec
                    modeSupported = (desiredMode == VK_PRESENT_MODE_FIFO_KHR);
                }

                if (modeSupported) {
                    modifiedCI.presentMode = desiredMode;
                    modified = true;
                    LayerLog("Vulkan Layer: Overriding present mode %d -> %d (%s)", pCreateInfo->presentMode,
                             desiredMode, vsyncMode);
                } else {
                    LayerLog(
                        "Vulkan Layer: Present mode %d (%s) not supported, keeping "
                        "original %d",
                        desiredMode, vsyncMode, pCreateInfo->presentMode);
                }
            }
        }

        // Backbuffer count override
        int32_t bbCount = VulkanLayerState::Get().GetBackbufferCount();
        if (bbCount >= 2 && bbCount != (int32_t)pCreateInfo->minImageCount) {
            modifiedCI.minImageCount = (uint32_t)bbCount;
            modified = true;
            LayerLog("Vulkan Layer: Overriding minImageCount %u -> %u", pCreateInfo->minImageCount,
                     modifiedCI.minImageCount);
        }
    }

    const VkSwapchainCreateInfoKHR* pFinalCI = modified ? &modifiedCI : pCreateInfo;

    // CRITICAL: Clean up old swapchain resources before creating new one
    // This prevents fence/semaphore conflicts when the game recreates the swapchain
    if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE) {
        LayerLog("Vulkan Layer: Cleaning up old swapchain %p before recreation", pCreateInfo->oldSwapchain);
        SwapchainData* oldSd = VulkanLayerState::Get().GetSwapchainData(pCreateInfo->oldSwapchain);
        if (oldSd) {
            CleanupOverlay(oldSd->device);
        }
        VulkanLayerState::Get().UnregisterSwapchain(pCreateInfo->oldSwapchain);
    }

    VkResult res = disp->fp_vkCreateSwapchainKHR(device, pFinalCI, pAllocator, pSwapchain);
    LayerLog("Vulkan Layer: vkCreateSwapchainKHR driver returned: %d", res);
    if (res == VK_SUCCESS && g_LayerState.whitelisted) {
        auto* sd = new SwapchainData();
        sd->swapchain = *pSwapchain;
        sd->device = device;
        sd->format = pCreateInfo->imageFormat;
        sd->colorSpace = pCreateInfo->imageColorSpace;
        sd->extent = pCreateInfo->imageExtent;

        uint32_t count = 0;
        disp->fp_vkGetSwapchainImagesKHR(device, *pSwapchain, &count, nullptr);
        sd->images.resize(count);
        disp->fp_vkGetSwapchainImagesKHR(device, *pSwapchain, &count, sd->images.data());
        sd->imageCount = count;

        HWND window = VulkanLayerState::Get().GetSurfaceWindow(pCreateInfo->surface);
        LayerLog("Vulkan Layer: Initializing overlay for swapchain %p, images=%d", *pSwapchain, count);
        const bool isTinySwapchain = (sd->extent.width < 320 || sd->extent.height < 180);
        const bool preferDX9Path = IsDXVKD3D9WrapperLoaded() && !IsDXVKD3D11WrapperLoaded();
        if (isTinySwapchain) {
            LayerLog(
                "Vulkan Layer: [Info] Skipping overlay/capture init for tiny "
                "swapchain %ux%u",
                sd->extent.width, sd->extent.height);
        } else {
            if (preferDX9Path) {
                // DXVK d3d9: skip overlay (DX9 hook handles it) but still init capture for zero-copy
                LayerLog("Vulkan Layer: DXVK d3d9 - skipping overlay, initializing Vulkan capture (%ux%u)",
                         sd->extent.width, sd->extent.height);
                InitializeCapture(device, *pSwapchain, sd->format, sd->colorSpace, sd->extent, count);
                // Ensure vulkanLayerActive is set: may have been missed at vkCreateInstance if IPC
                // wasn't ready yet. DX9 hook checks this flag to decide whether to use Vulkan
                // capture vs its own staging path. Setting it here (before any Present) guarantees
                // the Vulkan layer capture path is used for all frames.
                auto* shmPtr = g_IPCClient.GetSharedMem();
                if (shmPtr && !shmPtr->runtimeState.vulkanLayerActive.load(std::memory_order_acquire)) {
                    LayerLog(
                        "Vulkan Layer: DXVK d3d9 swapchain - setting vulkanLayerActive=true (deferred from "
                        "vkCreateInstance)");
                    shmPtr->runtimeState.vulkanLayerActive.store(true, std::memory_order_release);
                }
            } else {
                InitializeOverlay(device, *pSwapchain, sd->format, sd->colorSpace, sd->extent, count,
                                  sd->images.data(), window);
                LayerLog(
                    "Vulkan Layer: InitializeOverlay returned, registering "
                    "swapchain");
                InitializeCapture(device, *pSwapchain, sd->format, sd->colorSpace, sd->extent, count);
            }
        }

        VulkanLayerState::Get().RegisterSwapchain(*pSwapchain, sd);
        LayerLog("Vulkan Layer: Swapchain registration complete");
    }
    LayerLog("Vulkan Layer: vkCreateSwapchainKHR returning: %d", res);
    return res;
}

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                         const VkAllocationCallbacks* pAllocator) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    RetireCaptureSwapchain(device, swapchain);
    if (disp && disp->fp_vkDestroySwapchainKHR)
        disp->fp_vkDestroySwapchainKHR(device, swapchain, pAllocator);
    VulkanLayerState::Get().UnregisterSwapchain(swapchain);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                               uint32_t* pSwapchainImageCount,
                                                               VkImage* pSwapchainImages) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp || !disp->fp_vkGetSwapchainImagesKHR)
        return VK_ERROR_INITIALIZATION_FAILED;
    return disp->fp_vkGetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount, pSwapchainImages);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    // Performance metrics for this frame
    FrameMetrics perfMetrics;
    perfMetrics.qpcUs = PerfLogger::GetQpcUs();
    strcpy(perfMetrics.api, "Vulkan");
    static uint64_t s_perfFrameNum = 0;
    perfMetrics.frameNum = ++s_perfFrameNum;

    // Set flag to inform DXGI hooks that Vulkan is presenting on this thread.
    // This prevents double-drawing and incorrect API labeling when Vulkan
    // presents via DXGI.
    auto* shm = g_IPCClient.GetSharedMem();
    if (shm) {
        perfMetrics.sourceCapturePhase = shm->runtimeState.capturePhase.load(std::memory_order_relaxed);
        perfMetrics.sourceEncoderQueueDepth = shm->encoderQueueDepth.load(std::memory_order_relaxed);
        perfMetrics.sourceMuxQueueKb =
            (shm->runtimeState.muxQueueBytes.load(std::memory_order_relaxed) + 1023u) / 1024u;
        perfMetrics.sourceOverloadFlags = shm->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
    }
    if (shm) {
        shm->runtimeState.vulkanPresentThreadId.store(GetCurrentThreadId(), std::memory_order_release);
        shm->runtimeState.vulkanPresentTick.store(GetTickCount64(), std::memory_order_release);
    }

    const bool preferDX9Path = IsDXVKD3D9WrapperLoaded() && !IsDXVKD3D11WrapperLoaded();
    bool isFirstHook = !g_InPresentHook;
    g_InPresentHook = true;

    SwapchainData* sd = nullptr;
    if (g_LayerState.whitelisted && pPresentInfo && pPresentInfo->swapchainCount > 0) {
        sd = VulkanLayerState::Get().GetSwapchainData(pPresentInfo->pSwapchains[0]);
    }

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceFromQueue(queue);
    VkDevice queueDevice = VulkanLayerState::Get().GetVkDeviceFromQueue(queue);

    bool asyncPresentDetected = false;
    if (sd) {
        const uint32_t acquireThreadId = sd->lastAcquireThreadId.load(std::memory_order_acquire);
        const uint64_t lastAcquireTick = sd->lastAcquireTick.load(std::memory_order_acquire);
        if (acquireThreadId != 0 && acquireThreadId != GetCurrentThreadId() && lastAcquireTick != 0 &&
            (GetTickCount64() - lastAcquireTick) < 2000ULL) {
            if (!sd->asyncPresentDetected.exchange(true, std::memory_order_acq_rel)) {
                LayerLog("Vulkan Layer: Async present detected for swapchain %p; moving limiter off present thread",
                         sd->swapchain);
            }
        }
        asyncPresentDetected = sd->asyncPresentDetected.load(std::memory_order_acquire);
    }
    if (!asyncPresentDetected && queueDevice != VK_NULL_HANDLE) {
        const uint32_t lastSubmitThreadId = VulkanLayerState::Get().GetLastSubmitThreadId(queueDevice);
        if (lastSubmitThreadId != 0 && lastSubmitThreadId != GetCurrentThreadId()) {
            asyncPresentDetected = true;
            if (sd) {
                sd->asyncPresentDetected.store(true, std::memory_order_release);
            }
        }
    }

    if (isFirstHook) {
        // For DXVK: FPS limiter runs in DX9 hook (game thread) instead.
        // This vkQueuePresent runs on DXVK's CS thread — blocking here
        // doesn't limit the game's actual render rate.
        if (!preferDX9Path && !asyncPresentDetected) {
            g_SharedFpsLimiter.SetIPCClient(&g_IPCClient);
            g_SharedFpsLimiter.Apply();
        }

        // Apply CPU prerender limit - only if we have valid device and queue
        // tracking
        float prerenderLimit = VulkanLayerState::Get().GetPrerenderLimit();
        if (prerenderLimit >= 0.0f && !asyncPresentDetected && VulkanLayerState::Get().QueueSupportsGraphics(queue)) {
            if (queueDevice != VK_NULL_HANDLE && queue != VK_NULL_HANDLE) {
                ApplyPrerenderLimitVulkan(queueDevice, queue, prerenderLimit);
            }
        }
    }
    if (auto* perf = GetOverlayPerformanceMetrics(queueDevice)) {
        perfMetrics.sourceCurrentFpsTimes100 = static_cast<int32_t>(perf->GetCurrentFPS() * 100.0f + 0.5f);
        perfMetrics.source1PctLowTimes100 = static_cast<int32_t>(perf->Get1PercentLowFPS() * 100.0f + 0.5f);
        perfMetrics.sourcePoint1PctLowTimes100 = static_cast<int32_t>(perf->Get01PercentLowFPS() * 100.0f + 0.5f);
        perfMetrics.sourceFrameTimeStdDevUs = static_cast<int32_t>(perf->GetWindowStdDev() + 0.5);
    }

    const VkSemaphore* currentWaitSemaphores =
        (pPresentInfo && pPresentInfo->waitSemaphoreCount > 0) ? pPresentInfo->pWaitSemaphores : nullptr;
    uint32_t currentWaitSemaphoreCount = pPresentInfo ? pPresentInfo->waitSemaphoreCount : 0;
    std::vector<VkSemaphore> chainedWaitSemaphores;
    bool modified = false;
    if (preferDX9Path) {
        static int dxvkPresentSkipLogCount = 0;
        if (dxvkPresentSkipLogCount < 6) {
            LayerLog("Vulkan Layer: DXVK d3d9 wrapper detected, skipping Vulkan present-time overlay only");
            dxvkPresentSkipLogCount++;
        }
    }

    if (g_LayerState.whitelisted && pPresentInfo && pPresentInfo->swapchainCount > 0) {
