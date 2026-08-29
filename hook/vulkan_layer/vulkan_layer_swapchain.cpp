#include "vulkan_layer_internal.h"

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

        // Backbuffer count override. The application's own minImageCount is the
        // floor, never a suggestion: it declares how many images the game keeps
        // acquired, and taking images away below it breaks blocking
        // vkAcquireNextImageKHR (see vulkan_swapchain_image_policy.h).
        ce::vulkan_swapchain_image_policy::Input imagePolicyInput = {};
        imagePolicyInput.configuredBackbufferCount = VulkanLayerState::Get().GetBackbufferCount();
        imagePolicyInput.applicationMinImageCount = pCreateInfo->minImageCount;
        if (imagePolicyInput.configuredBackbufferCount >= 2) {
            VkPhysicalDevice physDev = disp->physicalDevice;
            VkInstance inst = VulkanLayerState::Get().GetInstanceFromPhysicalDevice(physDev);
            InstanceDispatch* instDisp = VulkanLayerState::Get().GetInstanceDispatch(inst);
            if (instDisp && instDisp->fp_vkGetPhysicalDeviceSurfaceCapabilitiesKHR) {
                // Deliberately not value-initialized: VkSurfaceTransformFlagBitsKHR
                // has no zero enumerator, and the driver fills every field CE
                // reads before the success return this is guarded on.
                VkSurfaceCapabilitiesKHR caps;
                if (instDisp->fp_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDev, pCreateInfo->surface, &caps) ==
                    VK_SUCCESS) {
                    imagePolicyInput.surfaceCapabilitiesKnown = true;
                    imagePolicyInput.surfaceMinImageCount = caps.minImageCount;
                    imagePolicyInput.surfaceMaxImageCount = caps.maxImageCount;
                }
            }
        }
        const ce::vulkan_swapchain_image_policy::Decision imagePolicy =
            ce::vulkan_swapchain_image_policy::Decide(imagePolicyInput);
        if (imagePolicy.overrideApplied) {
            modifiedCI.minImageCount = imagePolicy.minImageCount;
            modified = true;
            LayerLog("Vulkan Layer: Overriding minImageCount %u -> %u (configured=%d surfaceMin=%u surfaceMax=%u%s)",
                     pCreateInfo->minImageCount, modifiedCI.minImageCount,
                     imagePolicyInput.configuredBackbufferCount, imagePolicyInput.surfaceMinImageCount,
                     imagePolicyInput.surfaceMaxImageCount,
                     imagePolicy.clampedToSurfaceMaximum ? " clamped-to-surface-maximum" : "");
        } else if (imagePolicy.reductionSkipped) {
            LayerLog(
                "Vulkan Layer: minImageCount override skipped configured=%d game=%u (a reduction would take away the "
                "acquire headroom the game declared; keeping the game's count)",
                imagePolicyInput.configuredBackbufferCount, pCreateInfo->minImageCount);
        } else if (imagePolicy.skippedUnknownCapabilities) {
            LayerLog(
                "Vulkan Layer: minImageCount override skipped configured=%d game=%u - surface capabilities "
                "unavailable, so the request cannot be validated against the surface maximum",
                imagePolicyInput.configuredBackbufferCount, pCreateInfo->minImageCount);
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
    // The sharing mode decides whether CE may write the swapchain image from a
    // queue family other than the presenting one without an ownership transfer.
    // EXCLUSIVE is fine as long as CE uses the family the game rendered on,
    // which is what the reserved overlay queue guarantees; record it so a real
    // run can prove which case a title is in.
    LayerLog(
        "Vulkan Layer: vkCreateSwapchainKHR driver returned: %d (presentMode=%d sharingMode=%s familyCount=%u "
        "imageUsage=0x%x)",
        res, static_cast<int>(pFinalCI->presentMode),
        pCreateInfo->imageSharingMode == VK_SHARING_MODE_CONCURRENT ? "concurrent" : "exclusive",
        pCreateInfo->queueFamilyIndexCount, pCreateInfo->imageUsage);
    if (res == VK_SUCCESS) {
        auto* sd = new SwapchainData();
        sd->swapchain = *pSwapchain;
        sd->device = device;
        sd->format = pCreateInfo->imageFormat;
        sd->colorSpace = pCreateInfo->imageColorSpace;
        sd->extent = pCreateInfo->imageExtent;
        sd->imageUsage = pCreateInfo->imageUsage;
        sd->presentMode = pFinalCI->presentMode;

        uint32_t count = 0;
        disp->fp_vkGetSwapchainImagesKHR(device, *pSwapchain, &count, nullptr);
        sd->images.resize(count);
        disp->fp_vkGetSwapchainImagesKHR(device, *pSwapchain, &count, sd->images.data());
        sd->imageCount = count;
        // Once per swapchain, not per frame: the acquire headroom a game has is
        // `count - surfaceCaps.minImageCount`, so a session that fails inside
        // vkAcquireNextImageKHR has to be able to prove from the log whether CE
        // asked the driver for a different count than the game did.
        LayerLog("Vulkan Layer: Swapchain %p images=%u (game asked minImageCount=%u, CE requested %u)", *pSwapchain,
                 count, pCreateInfo->minImageCount, pFinalCI->minImageCount);

        sd->window = VulkanLayerState::Get().GetSurfaceWindow(pCreateInfo->surface);
        const bool isTinySwapchain = (sd->extent.width < 320 || sd->extent.height < 180);
        const bool activateNow = g_LayerState.whitelisted.load(std::memory_order_acquire);
        if (activateNow && isTinySwapchain) {
            LayerLog(
                "Vulkan Layer: [Info] Skipping overlay/capture init for tiny "
                "swapchain %ux%u",
                sd->extent.width, sd->extent.height);
        } else if (activateNow) {
            LayerLog("Vulkan Layer: Initializing overlay for swapchain %p, images=%d", *pSwapchain, count);
            InitializeOverlay(device, *pSwapchain, sd->format, sd->colorSpace, sd->extent, sd->imageUsage, count,
                              sd->images.data(), sd->window);
            LayerLog(
                "Vulkan Layer: InitializeOverlay returned, registering "
                "swapchain");
            InitializeCapture(device, *pSwapchain, sd->format, sd->colorSpace, sd->extent, count);
        }
        sd->runtimeInitialized.store(activateNow, std::memory_order_release);
        if (activateNow) {
            sd->captureHostGeneration.store(g_LayerHostGeneration.load(std::memory_order_acquire),
                                            std::memory_order_release);
        }

        VulkanLayerState::Get().RegisterSwapchain(*pSwapchain, sd);
        // A new swapchain generation is exactly when a game's present topology
        // can change - DOOM Eternal's "present from compute" toggle recreates
        // the swapchain - so re-arm the one-shot identification for it.
        VulkanLayerState::Get().ArmPresentTopologyLearning();
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
