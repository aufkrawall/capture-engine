// Included by vulkan_fg_switch_test.cpp; owner-typed swapchain creation, replacement, and WSI calls.

namespace testapp::vkfg {
namespace {

VkPresentModeKHR ToVkPresentMode(PresentMode mode) {
    switch (mode) {
        case PresentMode::Immediate:
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        case PresentMode::Mailbox:
            return VK_PRESENT_MODE_MAILBOX_KHR;
        case PresentMode::FifoRelaxed:
            return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        case PresentMode::Fifo:
        default:
            return VK_PRESENT_MODE_FIFO_KHR;
    }
}

PresentMode FromVkPresentMode(VkPresentModeKHR mode) {
    switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:
            return PresentMode::Immediate;
        case VK_PRESENT_MODE_MAILBOX_KHR:
            return PresentMode::Mailbox;
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
            return PresentMode::FifoRelaxed;
        case VK_PRESENT_MODE_FIFO_KHR:
        default:
            return PresentMode::Fifo;
    }
}

VulkanWsiDispatch NativeWsi() {
    VulkanWsiDispatch wsi{};
    wsi.route = VulkanWsiRoute::Loader;
    wsi.createSwapchain = vkCreateSwapchainKHR;
    wsi.destroySwapchain = vkDestroySwapchainKHR;
    wsi.getSwapchainImages = vkGetSwapchainImagesKHR;
    wsi.acquireNextImage = vkAcquireNextImageKHR;
    wsi.queuePresent = vkQueuePresentKHR;
    wsi.deviceWaitIdle = vkDeviceWaitIdle;
    return wsi;
}

VulkanWsiDispatch DispatchForOwner(SwapchainOwner owner) {
    if (owner == SwapchainOwner::Streamline) {
        return g_App.sl.wsi;
    }
    if (owner == SwapchainOwner::FidelityFX) {
        VulkanWsiDispatch wsi{};
        wsi.route = VulkanWsiRoute::FidelityFXReplacement;
        wsi.createSwapchainFfx = g_App.ffx.replacement.pOutCreateSwapchainFFXAPI;
        wsi.destroySwapchainFfx = g_App.ffx.replacement.pOutDestroySwapchainFFXAPI;
        wsi.getSwapchainImages = g_App.ffx.replacement.pOutGetSwapchainImagesKHR;
        wsi.acquireNextImage = g_App.ffx.replacement.pOutAcquireNextImageKHR;
        wsi.queuePresent = g_App.ffx.replacement.pOutQueuePresentKHR;
        wsi.deviceWaitIdle = vkDeviceWaitIdle;
        wsi.getLastPresentCountFfx = g_App.ffx.replacement.pOutGetLastPresentCountFFXAPI;
        wsi.context = g_App.ffx.swapchainContext;
        return wsi;
    }
    return NativeWsi();
}

VkResult WsiCreate(const VulkanWsiDispatch& wsi, const VkSwapchainCreateInfoKHR* createInfo,
                   VkSwapchainKHR* swapchain) {
    if (wsi.route == VulkanWsiRoute::FidelityFXReplacement) {
        return wsi.createSwapchainFfx
                   ? wsi.createSwapchainFfx(g_App.vk.device, createInfo, nullptr, swapchain, wsi.context)
                   : VK_ERROR_INITIALIZATION_FAILED;
    }
    return wsi.createSwapchain ? wsi.createSwapchain(g_App.vk.device, createInfo, nullptr, swapchain)
                               : VK_ERROR_INITIALIZATION_FAILED;
}

void WsiDestroy(const VulkanWsiDispatch& wsi, VkSwapchainKHR swapchain) {
    if (swapchain == VK_NULL_HANDLE) {
        return;
    }
    if (wsi.route == VulkanWsiRoute::FidelityFXReplacement) {
        if (wsi.destroySwapchainFfx) {
            wsi.destroySwapchainFfx(g_App.vk.device, swapchain, nullptr, wsi.context);
        }
    } else if (wsi.destroySwapchain) {
        wsi.destroySwapchain(g_App.vk.device, swapchain, nullptr);
    }
}

void DestroySwapchainViews(SwapchainState* state) {
    if (!state || g_App.vk.device == VK_NULL_HANDLE) {
        return;
    }
    for (VkFramebuffer framebuffer : state->framebuffers) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(g_App.vk.device, framebuffer, nullptr);
        }
    }
    state->framebuffers.clear();
    for (VkImageView view : state->views) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(g_App.vk.device, view, nullptr);
        }
    }
    state->views.clear();
    state->images.clear();
    state->layouts.clear();
    state->imageFences.clear();
}

bool CreateSwapchainFramebuffers(SwapchainState* state) {
    if (!state || g_App.renderer.swapchainRenderPass == VK_NULL_HANDLE) {
        return true;
    }
    state->framebuffers.resize(state->views.size(), VK_NULL_HANDLE);
    for (size_t index = 0; index < state->views.size(); ++index) {
        VkFramebufferCreateInfo framebufferInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = g_App.renderer.swapchainRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &state->views[index];
        framebufferInfo.width = state->extent.width;
        framebufferInfo.height = state->extent.height;
        framebufferInfo.layers = 1;
        const VkResult result =
            vkCreateFramebuffer(g_App.vk.device, &framebufferInfo, nullptr, &state->framebuffers[index]);
        if (result != VK_SUCCESS) {
            testapp::Log("[FG-DIAG] vkCreateFramebuffer(swapchain index=%zu) result=%s(%d)\n", index,
                         VkResultName(result), static_cast<int>(result));
            return false;
        }
    }
    return true;
}

bool PopulateSwapchainImages(SwapchainState* state) {
    uint32_t imageCount = 0;
    VkResult result = state->wsi.getSwapchainImages(g_App.vk.device, state->handle, &imageCount, nullptr);
    if (result != VK_SUCCESS || imageCount == 0) {
        testapp::Log("[FG-DIAG] getSwapchainImages(count) route=%s result=%s(%d) count=%u\n",
                     WsiRouteName(state->wsi.route), VkResultName(result), static_cast<int>(result), imageCount);
        return false;
    }
    state->images.resize(imageCount);
    result = state->wsi.getSwapchainImages(g_App.vk.device, state->handle, &imageCount, state->images.data());
    state->images.resize(imageCount);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        testapp::Log("[FG-DIAG] getSwapchainImages(data) route=%s result=%s(%d)\n",
                     WsiRouteName(state->wsi.route), VkResultName(result), static_cast<int>(result));
        return false;
    }
    state->views.resize(imageCount, VK_NULL_HANDLE);
    state->layouts.resize(imageCount, VK_IMAGE_LAYOUT_UNDEFINED);
    state->imageFences.resize(imageCount, VK_NULL_HANDLE);
    for (uint32_t index = 0; index < imageCount; ++index) {
        VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = state->images[index];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = state->format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        result = vkCreateImageView(g_App.vk.device, &viewInfo, nullptr, &state->views[index]);
        if (result != VK_SUCCESS) {
            testapp::Log("[FG-DIAG] vkCreateImageView(swapchain index=%u) result=%s(%d)\n", index,
                         VkResultName(result), static_cast<int>(result));
            return false;
        }
    }
    return CreateSwapchainFramebuffers(state);
}

bool BuildSwapchainDescription(VkSwapchainKHR oldSwapchain, VkSwapchainCreateInfoKHR* createInfo,
                               VkFormat* selectedFormat, VkColorSpaceKHR* selectedColorSpace,
                               VkPresentModeKHR* selectedPresentMode, VkExtent2D* selectedExtent) {
    VkSurfaceCapabilitiesKHR capabilities{};
    VkResult result =
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_App.vk.physicalDevice, g_App.vk.surface, &capabilities);
    if (result != VK_SUCCESS) {
        testapp::Log("[FG-DIAG] vkGetPhysicalDeviceSurfaceCapabilitiesKHR result=%s(%d)\n",
                     VkResultName(result), static_cast<int>(result));
        return false;
    }
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_App.vk.physicalDevice, g_App.vk.surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    if (formatCount) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(g_App.vk.physicalDevice, g_App.vk.surface, &formatCount,
                                             formats.data());
    }
    if (formats.empty()) {
        return false;
    }
    VkSurfaceFormatKHR surfaceFormat = formats.front();
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = format;
            break;
        }
        if (format.format == VK_FORMAT_R8G8B8A8_UNORM) {
            surfaceFormat = format;
        }
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_App.vk.physicalDevice, g_App.vk.surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> vkPresentModes(presentModeCount);
    if (presentModeCount) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(g_App.vk.physicalDevice, g_App.vk.surface, &presentModeCount,
                                                  vkPresentModes.data());
    }
    std::vector<PresentMode> policyPresentModes;
    for (VkPresentModeKHR mode : vkPresentModes) {
        policyPresentModes.push_back(FromVkPresentMode(mode));
        testapp::Log("[FG-DIAG] Present mode available=%d\n", static_cast<int>(mode));
    }
    const VkPresentModeKHR presentMode =
        ToVkPresentMode(SelectPresentMode(g_App.config.vsync != 0, policyPresentModes));

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        RECT clientRect{};
        GetClientRect(g_App.hwnd, &clientRect);
        const uint32_t width = static_cast<uint32_t>(std::max<LONG>(clientRect.right - clientRect.left, 1));
        const uint32_t height = static_cast<uint32_t>(std::max<LONG>(clientRect.bottom - clientRect.top, 1));
        extent.width = std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    uint32_t imageCount = std::max(kRequestedSwapchainImages, capabilities.minImageCount);
    if (capabilities.maxImageCount != 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }
    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((capabilities.supportedCompositeAlpha & compositeAlpha) == 0) {
        const VkCompositeAlphaFlagBitsKHR candidates[] = {
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        };
        for (VkCompositeAlphaFlagBitsKHR candidate : candidates) {
            if (capabilities.supportedCompositeAlpha & candidate) {
                compositeAlpha = candidate;
                break;
            }
        }
    }
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    usage &= capabilities.supportedUsageFlags;
    if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
        testapp::Log("[FG-DIAG] ERROR surface lacks color-attachment swapchain usage\n");
        return false;
    }

    *createInfo = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    createInfo->surface = g_App.vk.surface;
    createInfo->minImageCount = imageCount;
    createInfo->imageFormat = surfaceFormat.format;
    createInfo->imageColorSpace = surfaceFormat.colorSpace;
    createInfo->imageExtent = extent;
    createInfo->imageArrayLayers = 1;
    createInfo->imageUsage = usage;
    createInfo->imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo->preTransform = capabilities.currentTransform;
    createInfo->compositeAlpha = compositeAlpha;
    createInfo->presentMode = presentMode;
    createInfo->clipped = VK_TRUE;
    createInfo->oldSwapchain = oldSwapchain;
    *selectedFormat = surfaceFormat.format;
    *selectedColorSpace = surfaceFormat.colorSpace;
    *selectedPresentMode = presentMode;
    *selectedExtent = extent;
    testapp::Log(
        "[FG-DIAG] Swapchain description extent=%ux%u format=%d colorspace=%d presentMode=%d images=%u "
        "usage=0x%x old=%p\n",
        extent.width, extent.height, static_cast<int>(surfaceFormat.format),
        static_cast<int>(surfaceFormat.colorSpace), static_cast<int>(presentMode), imageCount,
        static_cast<unsigned>(usage), reinterpret_cast<void*>(oldSwapchain));
    return true;
}

bool RestoreSwapchainAfterFailedFfxTakeover(SwapchainState* state, bool oldHandleConsumed,
                                            const char* reason) {
    if (!state) {
        return false;
    }
    if (oldHandleConsumed) {
        state->handle = VK_NULL_HANDLE;
    } else if (state->handle != VK_NULL_HANDLE && PopulateSwapchainImages(state)) {
        const bool framebuffersReady = !g_App.renderer.initialized || CreateSwapchainFramebuffers(state);
        testapp::Log(
            "[FG-TRANSITION] FFX takeover rollback retained old handle=%p owner=%s route=%s "
            "framebuffers=%d reason=%s\n",
            reinterpret_cast<void*>(state->handle), OwnerName(state->owner),
            WsiRouteName(state->wsi.route), framebuffersReady ? 1 : 0,
            reason ? reason : "unknown");
        return framebuffersReady;
    }

    // FidelityFX consumes createInfo.oldSwapchain when its context is created. If a later setup
    // step fails, rebuild the old owner through its original immutable dispatch table so rollback
    // still leaves a usable visible surface.
    state->handle = VK_NULL_HANDLE;
    state->createInfo.oldSwapchain = VK_NULL_HANDLE;
    const VkResult createResult = WsiCreate(state->wsi, &state->createInfo, &state->handle);
    bool restored = createResult == VK_SUCCESS && state->handle != VK_NULL_HANDLE &&
                    PopulateSwapchainImages(state);
    if (restored && g_App.renderer.initialized) {
        restored = CreateSwapchainFramebuffers(state);
    }
    if (restored && state->owner == SwapchainOwner::FidelityFX) {
        g_App.ffx.swapchainHandleStorage = state->handle;
    }
    testapp::Log(
        "[FG-TRANSITION] FFX takeover rollback recreated old owner=%s route=%s handle=%p "
        "create=%s(%d) restored=%d reason=%s\n",
        OwnerName(state->owner), WsiRouteName(state->wsi.route),
        reinterpret_cast<void*>(state->handle), VkResultName(createResult),
        static_cast<int>(createResult), restored ? 1 : 0, reason ? reason : "unknown");
    testapp::LogFlush();
    return restored;
}

void DestroyFailedFfxTakeover(SwapchainState* replacement, const char* reason) {
    if (!replacement) {
        return;
    }
    // The FFX configure/shutdown contract must receive the proxy swapchain which belongs to its
    // context. During takeover g_App.swapchain still contains moved-from scalar fields from the
    // old owner, whose handle may already have been consumed by FFX. Exposing that stale handle to
    // ffxConfigure raises STATUS_INVALID_HANDLE inside the signed runtime.
    SwapchainState transactionScratch = std::move(g_App.swapchain);
    g_App.swapchain = *replacement;
    testapp::Log(
        "[FG-TRANSITION] cleaning failed FFX takeover with owning proxy handle=%p route=%s "
        "context=%p reason=%s\n",
        reinterpret_cast<void*>(g_App.swapchain.handle),
        WsiRouteName(g_App.swapchain.wsi.route), g_App.ffx.swapchainContext,
        reason ? reason : "unknown");
    DestroyFidelityFxContexts(g_App.config.fsrReloadRuntimeOnSwitch, reason);
    replacement->handle = VK_NULL_HANDLE;
    g_App.swapchain = std::move(transactionScratch);
}

}  // namespace

bool DrainSwapchainBoundWork(const char* reason) {
    if (g_App.vk.device == VK_NULL_HANDLE) {
        return true;
    }
    if (g_App.swapchain.owner == SwapchainOwner::FidelityFX && !WaitForFsrPresents(reason)) {
        return false;
    }
    PFN_vkDeviceWaitIdle waitIdle = g_App.swapchain.wsi.deviceWaitIdle;
    if (!waitIdle) {
        waitIdle = vkDeviceWaitIdle;
    }
    const VkResult result = waitIdle(g_App.vk.device);
    testapp::Log("[FG-DIAG] Drain swapchain work reason=%s owner=%s route=%s result=%s(%d)\n",
                 reason ? reason : "unknown", OwnerName(g_App.swapchain.owner),
                 WsiRouteName(g_App.swapchain.wsi.route), VkResultName(result), static_cast<int>(result));
    if (result == VK_ERROR_DEVICE_LOST) {
        g_App.vk.deviceLost = true;
        MarkDeviceLost(&g_App.transition);
        LogDeviceFault("drain swapchain work");
    }
    testapp::LogFlush();
    return result == VK_SUCCESS;
}

void DestroySwapchainState(bool destroyHandle) {
    DestroySwapchainViews(&g_App.swapchain);
    if (destroyHandle && g_App.swapchain.handle != VK_NULL_HANDLE) {
        WsiDestroy(g_App.swapchain.wsi, g_App.swapchain.handle);
        g_App.swapchain.handle = VK_NULL_HANDLE;
    }
}

bool CreateOrReplaceSwapchain(SwapchainOwner owner, const char* reason) {
    if (!IsOwnerDispatchPairValid(owner, ExpectedWsiRoute(owner))) {
        return false;
    }
    SwapchainState old = std::move(g_App.swapchain);
    const VkSwapchainKHR oldHandleForLog = old.handle;
    const bool crossOwner = old.handle != VK_NULL_HANDLE && old.owner != owner;
    const bool leavingFfx = old.owner == SwapchainOwner::FidelityFX && owner != SwapchainOwner::FidelityFX;

    VkSwapchainCreateInfoKHR createInfo{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent{};
    // FFX wrapper handles never cross WSI ownership boundaries. Native/Streamline replacement can
    // use the standard oldSwapchain path directly. FidelityFX takeover instead gets a loader-owned
    // bridge below, which lets the old proxy observe its own destroy hook before FFX consumes a
    // valid raw handle. FFX same-owner recreation cannot forward its wrapper because the context
    // accepts a new proxy only after the previous proxy has been destroyed.
    const bool forwardOldSwapchain = old.handle != VK_NULL_HANDLE &&
        ShouldForwardOldSwapchain(old.owner, owner);
    const VkSwapchainKHR oldForCreate = forwardOldSwapchain ? old.handle : VK_NULL_HANDLE;
    if (!BuildSwapchainDescription(oldForCreate, &createInfo, &format, &colorSpace, &presentMode, &extent)) {
        g_App.swapchain = std::move(old);
        return false;
    }

    SwapchainState replacement{};
    replacement.owner = owner;
    replacement.format = format;
    replacement.colorSpace = colorSpace;
    replacement.presentMode = presentMode;
    replacement.extent = extent;
    replacement.createInfo = createInfo;
    replacement.wsi = DispatchForOwner(owner);
    VkResult createResult = VK_ERROR_INITIALIZATION_FAILED;
    bool newFfxContext = false;
    bool oldViewsReleasedForFfx = false;
    bool oldSwapchainConsumedByFfx = false;
    bool oldFfxProxyRetiredBeforeCreate = false;
    if (owner == SwapchainOwner::FidelityFX && !g_App.ffx.swapchainContext) {
        // The signed provider unconditionally destroys its incoming raw swapchain. Supplying null
        // is legal Vulkan but crashes buggy implicit layers such as RTSS; supplying a Streamline
        // proxy bypasses DLSS-G's destroy bookkeeping. Create a loader-owned bridge first, retire
        // the old surface through its immutable route, and then let FFX consume the valid bridge.
        VulkanWsiDispatch bridgeWsi = NativeWsi();
        VkSwapchainCreateInfoKHR bridgeInfo = createInfo;
        bridgeInfo.oldSwapchain = old.owner == SwapchainOwner::FidelityFX
                                      ? VK_NULL_HANDLE
                                      : old.handle;
        VkSwapchainKHR bridgeSwapchain = VK_NULL_HANDLE;
        const VkResult bridgeResult =
            WsiCreate(bridgeWsi, &bridgeInfo, &bridgeSwapchain);
        testapp::Log(
            "[FG-TRANSITION] FidelityFX loader bridge create result=%s(%d) oldOwner=%s "
            "old=%p bridge=%p\n",
            VkResultName(bridgeResult), static_cast<int>(bridgeResult), OwnerName(old.owner),
            reinterpret_cast<void*>(old.handle), reinterpret_cast<void*>(bridgeSwapchain));

        if (old.handle != VK_NULL_HANDLE && bridgeInfo.oldSwapchain != VK_NULL_HANDLE) {
            // oldSwapchain is retired by the bridge call even when creation fails. Destroy it via
            // the owner route now so Streamline sees its hook and rollback can recreate cleanly.
            DestroySwapchainViews(&old);
            oldViewsReleasedForFfx = true;
            WsiDestroy(old.wsi, old.handle);
            old.handle = VK_NULL_HANDLE;
            oldSwapchainConsumedByFfx = true;
        }

        if (bridgeResult == VK_SUCCESS && bridgeSwapchain != VK_NULL_HANDLE) {
            VkSwapchainCreateInfoKHR ffxCreateInfo = createInfo;
            ffxCreateInfo.oldSwapchain = bridgeSwapchain;
            bool bridgeConsumedByFfx = false;
            if (CreateFidelityFxSwapchain(bridgeSwapchain, ffxCreateInfo,
                                          &replacement.handle, &bridgeConsumedByFfx)) {
                createResult = VK_SUCCESS;
                newFfxContext = true;
                // The replacement table does not exist until ffxQuery succeeds inside
                // CreateFidelityFxSwapchain. Snapshot it only now so this swapchain permanently
                // owns the exact WSI route which created it.
                replacement.wsi = DispatchForOwner(owner);
            }
            if (!bridgeConsumedByFfx) {
                WsiDestroy(bridgeWsi, bridgeSwapchain);
            }
        } else {
            createResult = bridgeResult;
            if (bridgeSwapchain != VK_NULL_HANDLE) {
                WsiDestroy(bridgeWsi, bridgeSwapchain);
            }
        }
    } else {
        if (leavingFfx && old.handle != VK_NULL_HANDLE) {
            // VK_KHR_win32_surface permits only one swapchain for this HWND and the FFX wrapper
            // does not expose its internal real VkSwapchainKHR as oldSwapchain. The transition has
            // already presented a passthrough frame and drained all work. Retire only the FFX
            // proxy now while retaining its API context, so failed target creation can recreate
            // the old owner transactionally through the same replacement table.
            DestroySwapchainViews(&old);
            oldViewsReleasedForFfx = true;
            WsiDestroy(old.wsi, old.handle);
            old.handle = VK_NULL_HANDLE;
            oldSwapchainConsumedByFfx = true;
            oldFfxProxyRetiredBeforeCreate = true;
            testapp::Log(
                "[FG-TRANSITION] FidelityFX proxy retired after drained passthrough for owner "
                "handoff; context retained=%d target=%s\n",
                g_App.ffx.swapchainContext ? 1 : 0, OwnerName(owner));
        }
        if (owner == SwapchainOwner::FidelityFX && old.owner == SwapchainOwner::FidelityFX &&
            old.handle != VK_NULL_HANDLE) {
            // The queried FFX create function deliberately rejects a second live proxy in the
            // same context. Drain has already completed, so retire the old proxy through its own
            // table and let the context create its replacement with oldSwapchain == null.
            DestroySwapchainViews(&old);
            oldViewsReleasedForFfx = true;
            WsiDestroy(old.wsi, old.handle);
            old.handle = VK_NULL_HANDLE;
            oldSwapchainConsumedByFfx = true;
            testapp::Log(
                "[FG-TRANSITION] FidelityFX same-owner proxy retired before recreation; "
                "context retained=%d\n",
                g_App.ffx.swapchainContext ? 1 : 0);
        }
        createResult = WsiCreate(replacement.wsi, &createInfo, &replacement.handle);
    }
    if (createResult == VK_SUCCESS && replacement.handle != VK_NULL_HANDLE &&
        owner == SwapchainOwner::FidelityFX) {
        g_App.ffx.swapchainHandleStorage = replacement.handle;
    }
    testapp::Log(
        "[FG-TRANSITION] swapchain-create reason=%s owner=%s route=%s result=%s(%d) old=%p new=%p "
        "crossOwner=%d forwardedOld=%d newFfxContext=%d\n",
        reason ? reason : "unknown", OwnerName(owner), WsiRouteName(replacement.wsi.route),
        VkResultName(createResult), static_cast<int>(createResult), reinterpret_cast<void*>(oldHandleForLog),
        reinterpret_cast<void*>(replacement.handle), crossOwner ? 1 : 0,
        forwardOldSwapchain ? 1 : 0, newFfxContext ? 1 : 0);
    if (createResult != VK_SUCCESS || replacement.handle == VK_NULL_HANDLE ||
        !IsOwnerDispatchPairValid(owner, replacement.wsi.route) || !replacement.wsi.getSwapchainImages ||
        !replacement.wsi.acquireNextImage || !replacement.wsi.queuePresent) {
        ++g_App.transitionFailures;
        if (newFfxContext) {
            DestroyFailedFfxTakeover(&replacement,
                                     "rollback failed FidelityFX swapchain creation");
        } else if (replacement.handle != VK_NULL_HANDLE) {
            WsiDestroy(replacement.wsi, replacement.handle);
        }
        if (oldViewsReleasedForFfx &&
            !RestoreSwapchainAfterFailedFfxTakeover(&old, oldSwapchainConsumedByFfx,
                                                    "FFX swapchain creation failure")) {
            g_App.running = false;
            testapp::Log("[FG-DIAG] FATAL unable to restore old swapchain after FFX takeover failure\n");
        }
        g_App.swapchain = std::move(old);
        testapp::LogFlush();
        return false;
    }

    if (!PopulateSwapchainImages(&replacement)) {
        ++g_App.transitionFailures;
        DestroySwapchainViews(&replacement);
        if (newFfxContext) {
            DestroyFailedFfxTakeover(&replacement,
                                     "rollback FidelityFX image enumeration failure");
        } else {
            WsiDestroy(replacement.wsi, replacement.handle);
        }
        if (oldViewsReleasedForFfx &&
            !RestoreSwapchainAfterFailedFfxTakeover(&old, oldSwapchainConsumedByFfx,
                                                    "FFX image enumeration failure")) {
            g_App.running = false;
            testapp::Log("[FG-DIAG] FATAL unable to restore old swapchain after FFX image failure\n");
        }
        g_App.swapchain = std::move(old);
        return false;
    }

    DestroySwapchainViews(&old);
    if (leavingFfx) {
        if (!oldFfxProxyRetiredBeforeCreate) {
            SetFsrFrameGeneration(false, "leave FidelityFX swapchain", true);
            WaitForFsrPresents("leave FidelityFX swapchain");
        }
        DestroyFidelityFxContexts(g_App.config.fsrReloadRuntimeOnSwitch,
                                  "replace FidelityFX owner",
                                  oldFfxProxyRetiredBeforeCreate);
        old.handle = VK_NULL_HANDLE;
    } else {
        if (old.handle != VK_NULL_HANDLE) {
            WsiDestroy(old.wsi, old.handle);
            old.handle = VK_NULL_HANDLE;
        }
    }
    if ((old.owner == SwapchainOwner::Streamline && owner != SwapchainOwner::Streamline) ||
        (owner == SwapchainOwner::FidelityFX && g_App.sl.featuresLoaded)) {
        if (!RetireStreamlinePresentation(owner, reason)) {
            testapp::Log(
                "[FG-TRANSITION] WARN Streamline retirement incomplete after replacement commit; "
                "new owner remains FG-off\n");
        }
    }
    replacement.createInfo.oldSwapchain = VK_NULL_HANDLE;
    g_App.swapchain = std::move(replacement);
    g_App.transition.owner = owner;
    g_App.resetTemporalHistory = true;
    g_App.ffx.lastSwapchainPresentCount = 0;
    if (g_App.renderer.initialized) {
        if (owner == SwapchainOwner::FidelityFX) {
            ReleaseFidelityFxEffectsForExtent(
                "swapchain committed; release old renderer image references");
        }
        if (!RecreateRendererForExtent()) {
            testapp::Log(
                "[FG-DIAG] FATAL renderer recreation failed after swapchain commit; "
                "the old surface is already retired and cannot be rolled back safely\n");
            testapp::LogFlush();
            return true;
        }
        if (owner == SwapchainOwner::FidelityFX && !RecreateFidelityFxEffectsForExtent()) {
            testapp::Log(
                "[FG-DIAG] FidelityFX effect recreation failed after swapchain commit; "
                "new surface remains usable with FG disabled\n");
        }
        if (owner == SwapchainOwner::Streamline) {
            ConfigureDlssSuperResolution(true);
        }
    }
    testapp::Log(
        "[FG-TRANSITION] swapchain-commit owner=%s route=%s handle=%p images=%zu extent=%ux%u format=%d "
        "presentMode=%d\n",
        OwnerName(g_App.swapchain.owner), WsiRouteName(g_App.swapchain.wsi.route),
        reinterpret_cast<void*>(g_App.swapchain.handle), g_App.swapchain.images.size(),
        g_App.swapchain.extent.width, g_App.swapchain.extent.height, static_cast<int>(g_App.swapchain.format),
        static_cast<int>(g_App.swapchain.presentMode));
    testapp::LogFlush();
    return true;
}

bool RecreateCurrentSwapchain(const char* reason) {
    if (!DrainSwapchainBoundWork(reason)) {
        return false;
    }
    const FgMode mode = g_App.transition.currentMode;
    const bool wasActive = mode != FgMode::Off && !g_App.transition.suspended;
    if (wasActive) {
        SetModeFeatureState(mode, false, "swapchain recreation");
    }
    const bool result = CreateOrReplaceSwapchain(g_App.swapchain.owner, reason);
    if (result && mode == FgMode::Fsr) {
        RegisterFsrUiResource(g_App.renderer.resources[g_App.frameSlot]);
    }
    if (result && wasActive) {
        if (!MarkSameOwnerReplacementCommitted(&g_App.transition)) {
            testapp::Log(
                "[FG-TRANSITION] ERROR same-owner recreation committed but transition state "
                "could not enter replacement-present stage mode=%s owner=%s stage=%s\n",
                ModeName(mode), OwnerName(g_App.swapchain.owner),
                TransitionStageName(g_App.transition.stage));
            testapp::LogFlush();
        }
    }
    return result;
}

VkResult AcquireSwapchainImage(FrameContext& frame, uint32_t* imageIndex) {
    const VkResult result = g_App.swapchain.wsi.acquireNextImage(
        g_App.vk.device, g_App.swapchain.handle, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        g_App.swapchainRecreatePending = true;
    } else if (result == VK_ERROR_DEVICE_LOST) {
        g_App.vk.deviceLost = true;
        MarkDeviceLost(&g_App.transition);
        LogDeviceFault("vkAcquireNextImageKHR");
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        testapp::Log("[FG-DIAG] acquireNextImage owner=%s route=%s result=%s(%d) frameSlot=%u\n",
                     OwnerName(g_App.swapchain.owner), WsiRouteName(g_App.swapchain.wsi.route),
                     VkResultName(result), static_cast<int>(result), g_App.frameSlot);
    }
    return result;
}

VkResult PresentSwapchainImage(FrameContext& frame, uint32_t imageIndex) {
    VkPresentInfoKHR presentInfo = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &frame.renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &g_App.swapchain.handle;
    presentInfo.pImageIndices = &imageIndex;
    const VkResult result = g_App.swapchain.wsi.queuePresent(g_App.vk.gameQueue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        g_App.swapchainRecreatePending = true;
    } else if (result == VK_ERROR_DEVICE_LOST) {
        g_App.vk.deviceLost = true;
        MarkDeviceLost(&g_App.transition);
        LogDeviceFault("vkQueuePresentKHR");
    }
    if (result != VK_SUCCESS || g_App.frameId < 5 || (g_App.frameId % 240) == 0) {
        testapp::Log(
            "[FG-DIAG] queuePresent frameID=%llu image=%u owner=%s route=%s result=%s(%d) "
            "requestedFG=%s effective(sl=%d,ffx=%d)\n",
            static_cast<unsigned long long>(g_App.frameId), imageIndex, OwnerName(g_App.swapchain.owner),
            WsiRouteName(g_App.swapchain.wsi.route), VkResultName(result), static_cast<int>(result),
            ModeName(g_App.transition.currentMode), g_App.sl.dlssFgConfigured ? 1 : 0,
            g_App.ffx.frameGenerationConfigured ? 1 : 0);
    }
    if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) &&
        g_App.swapchain.wsi.getLastPresentCountFfx) {
        const uint64_t count = g_App.swapchain.wsi.getLastPresentCountFfx(g_App.swapchain.handle);
        if (g_App.ffx.lastSwapchainPresentCount != 0 && count > g_App.ffx.lastSwapchainPresentCount + 1) {
            const uint64_t generated = count - g_App.ffx.lastSwapchainPresentCount - 1;
            g_App.generatedFrames += generated;
            g_App.ffx.generatedPresentCount += generated;
        }
        g_App.ffx.lastSwapchainPresentCount = count;
    }
    return result;
}

}  // namespace testapp::vkfg
