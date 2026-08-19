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
    // The sharing mode decides whether CE may write the swapchain image from a
    // queue family other than the presenting one without an ownership transfer.
    // EXCLUSIVE is fine as long as CE uses the family the game rendered on,
    // which is what the reserved overlay queue guarantees; record it so a real
    // run can prove which case a title is in.
    LayerLog("Vulkan Layer: vkCreateSwapchainKHR driver returned: %d (sharingMode=%s familyCount=%u)", res,
             pCreateInfo->imageSharingMode == VK_SHARING_MODE_CONCURRENT ? "concurrent" : "exclusive",
             pCreateInfo->queueFamilyIndexCount);
    if (res == VK_SUCCESS) {
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

        sd->window = VulkanLayerState::Get().GetSurfaceWindow(pCreateInfo->surface);
        const bool isTinySwapchain = (sd->extent.width < 320 || sd->extent.height < 180);
        const bool preferDX9Path = IsDXVKD3D9WrapperLoaded() && !IsDXVKD3D11WrapperLoaded();
        const bool activateNow = g_LayerState.whitelisted.load(std::memory_order_acquire);
        if (activateNow && isTinySwapchain) {
            LayerLog(
                "Vulkan Layer: [Info] Skipping overlay/capture init for tiny "
                "swapchain %ux%u",
                sd->extent.width, sd->extent.height);
        } else if (activateNow) {
            LayerLog("Vulkan Layer: Initializing overlay for swapchain %p, images=%d", *pSwapchain, count);
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
                                  sd->images.data(), sd->window);
                LayerLog(
                    "Vulkan Layer: InitializeOverlay returned, registering "
                    "swapchain");
                InitializeCapture(device, *pSwapchain, sd->format, sd->colorSpace, sd->extent, count);
            }
        }
        sd->runtimeInitialized.store(activateNow, std::memory_order_release);
        if (activateNow) {
            sd->captureHostGeneration.store(g_LayerHostGeneration.load(std::memory_order_acquire),
                                            std::memory_order_release);
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
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceFromQueue(queue);
    if (!g_LayerState.whitelisted.load(std::memory_order_acquire)) {
        return (disp && disp->fp_vkQueuePresentKHR) ? disp->fp_vkQueuePresentKHR(queue, pPresentInfo)
                                                   : VK_ERROR_INITIALIZATION_FAILED;
    }

    g_FGCompat.RecordPresentForNvidiaSmoothMotion();

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

    VkDevice queueDevice = VulkanLayerState::Get().GetVkDeviceFromQueue(queue);

    const bool runtimeEligible = sd && sd->extent.width >= 320 && sd->extent.height >= 180;
    if (sd && !sd->runtimeInitialized.exchange(true, std::memory_order_acq_rel)) {
        if (runtimeEligible) {
            if (!preferDX9Path) {
                InitializeOverlay(sd->device, sd->swapchain, sd->format, sd->colorSpace, sd->extent, sd->imageCount,
                                  sd->images.data(), sd->window);
            }
            LayerLog("[InjectLifecycle] Late-initialized Vulkan swapchain %p", sd->swapchain);
        }
    }

    if (runtimeEligible) {
        const uint64_t hostGeneration = g_LayerHostGeneration.load(std::memory_order_acquire);
        const uint64_t captureGeneration = sd->captureHostGeneration.load(std::memory_order_acquire);
        if (hostGeneration != 0 && captureGeneration != hostGeneration) {
            if (!RepublishCaptureTransportForHost(sd->device, sd->swapchain)) {
                InitializeCapture(sd->device, sd->swapchain, sd->format, sd->colorSpace, sd->extent, sd->imageCount);
            }
            sd->captureHostGeneration.store(hostGeneration, std::memory_order_release);
        }
    }

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
        // Apply CPU prerender limit - only if we have valid device and queue
        // tracking
        float prerenderLimit = VulkanLayerState::Get().GetPrerenderLimit();
        if (prerenderLimit >= 0.0f && !asyncPresentDetected && VulkanLayerState::Get().QueueSupportsGraphics(queue)) {
            if (queueDevice != VK_NULL_HANDLE && queue != VK_NULL_HANDLE) {
                ApplyPrerenderLimitVulkan(queueDevice, queue, prerenderLimit);
            }
        }
    }

    // FPS limiter: pace EVERY present, not only the first one entering the hook.
    // Strange Brigade Vulkan presents several swapchain images per frame period
    // (concurrent present streams); gating only the first present let the other
    // images through unpaced, so the displayed rate stayed at 2x the configured
    // target with alternating short/long frame times and bad 1% lows. The
    // limiter's gateEveryPresent mode serializes concurrent presents onto the
    // cadence grid: exactly one present per target interval, evenly spaced,
    // regardless of vsync (FIFO stays untouched and the limiter waits before
    // the driver call, so vsync on/off paces identically).
    // DXVK keeps the legacy first-present gating + dedup: its CS thread presents
    // once per frame while the DX9 hook paces the game thread for d3d9, and the
    // game-thread DXGI hook + layer both pace for d3d11 — the layer's second
    // call is then a duplicate of the same frame and must not wait again.
    if (!preferDX9Path && !asyncPresentDetected) {
        const bool nativeVulkanPresent = !IsDXVKD3D11WrapperLoaded();
        const int64_t fpsLimitStartUs = PerfLogger::GetQpcUs();
        g_SharedFpsLimiter.SetIPCClient(&g_IPCClient);
        g_SharedFpsLimiter.Apply(false, nativeVulkanPresent);
        perfMetrics.fpsLimitWaitUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - fpsLimitStartUs);
    }

    if (auto* perf = GetOverlayPerformanceMetrics(queueDevice)) {
    perfMetrics.sourceCurrentFpsTimes100 = static_cast<int32_t>(std::lround(perf->GetCurrentFPS() * 100.0f));
    perfMetrics.source1PctLowTimes100 = static_cast<int32_t>(std::lround(perf->Get1PercentLowFPS() * 100.0f));
    perfMetrics.sourcePoint1PctLowTimes100 = static_cast<int32_t>(std::lround(perf->Get01PercentLowFPS() * 100.0f));
    perfMetrics.sourceFrameTimeStdDevUs = static_cast<int32_t>(std::lround(perf->GetWindowStdDev()));
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

        if (sd) {
            uint32_t idx = pPresentInfo->pImageIndices[0];
            perfMetrics.sourceFrameIndex = idx + 1;

            // OPTIMIZATION: Keep overlay/capture queue work GPU-ordered while letting config choose whether
            // screenshots and capture happen before or after overlay submission.
            VkSemaphore overlayDone = GetOverlaySemaphore(sd->device, idx);
            OverlayConfig overlayCfg{};
            overlayCfg.captureIncludeOverlay = true;
            overlayCfg.screenshotIncludeOverlay = true;
            if (shm) {
                overlayCfg = shm->ReadOverlayConfig();
            }
            const bool overlayEnabled = !preferDX9Path && shm && overlayCfg.showOverlay;
            const bool captureRequested = shm && shm->runtimeState.IsInjectVideoCaptureRequested();
            const uint64_t screenshotRequestId = GetPendingScreenshotRequestId(shm);
            const bool screenshotRequested = screenshotRequestId != 0;
            const bool captureAfterOverlay = captureRequested && overlayEnabled && overlayCfg.captureIncludeOverlay;
            const bool captureBeforeOverlay = captureRequested && !captureAfterOverlay;
            const bool screenshotAfterOverlay =
                screenshotRequested && overlayEnabled && overlayCfg.screenshotIncludeOverlay;
            const bool screenshotBeforeOverlay = screenshotRequested && !screenshotAfterOverlay;

            auto doOverlay = [&]() {
                if (!overlayEnabled)
                    return;

                // Measure ONLY the actual CPU overhead of overlay work.
                // Fence wait is tracked separately (it's GPU sync, not our overhead).
                int32_t fenceWaitUs = 0;
                int64_t overlayStartUs = PerfLogger::GetQpcUs();
                bool overlayRendered = RenderOverlay(sd->device, queue, idx, currentWaitSemaphores,
                                                     currentWaitSemaphoreCount, overlayDone, &fenceWaitUs);
                perfMetrics.overlayUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - overlayStartUs);
                perfMetrics.fenceWaitUs = fenceWaitUs;
                if (fenceWaitUs > 0 && perfMetrics.overlayUs > fenceWaitUs) {
                    perfMetrics.overlayUs -= fenceWaitUs;
                }
                if (overlayRendered) {
                    chainedWaitSemaphores.assign(1, overlayDone);
                    currentWaitSemaphores = chainedWaitSemaphores.data();
                    currentWaitSemaphoreCount = 1;
                    modified = true;
                }
            };

            auto doCapture = [&]() {
                if (!captureRequested || !shm)
                    return;
                if (shm->throttleCapture.load(std::memory_order_acquire)) {
                    return;
                }
                if (ShouldSkipCaptureForTargetCadence(shm, "Vulkan")) {
                    return;
                }

                int64_t captureStartUs = PerfLogger::GetQpcUs();
                VkSemaphore captureDone = GetCaptureSemaphore(sd->device, sd->swapchain, idx);
                if (captureDone == VK_NULL_HANDLE) {
                    // Initialization may have been deferred while the previous
                    // swapchain generation's media leases drained. Retry without
                    // blocking the present path.
                    InitializeCapture(sd->device, sd->swapchain, sd->format, sd->colorSpace, sd->extent,
                                      sd->imageCount);
                    captureDone = GetCaptureSemaphore(sd->device, sd->swapchain, idx);
                }
                NoteCaptureSwapchainImagePresented(sd->device, sd->swapchain, idx);
                const bool captureSubmitted =
                    CaptureFrame(sd->device, sd->swapchain, queue, sd->images[idx], currentWaitSemaphores,
                                 currentWaitSemaphoreCount, captureDone);
                perfMetrics.captureUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - captureStartUs);
                // Capture may intentionally skip before queue submission (for
                // example while its non-blocking fence is busy). Preserve the
                // original Present wait chain unless captureDone was really signaled.
                if (captureSubmitted && captureDone != VK_NULL_HANDLE) {
                    chainedWaitSemaphores.assign(1, captureDone);
                    currentWaitSemaphores = chainedWaitSemaphores.data();
                    currentWaitSemaphoreCount = 1;
                    modified = true;
                }
            };

            auto doScreenshot = [&]() {
                if (!screenshotRequested || !shm)
                    return;
                bool waitsConsumed = false;
                DeviceDispatch* vkDisp = VulkanLayerState::Get().GetDeviceDispatch(sd->device);
                if (vkDisp && idx < sd->images.size()) {
                    waitsConsumed = TakeVulkanScreenshot(vkDisp, sd->device, queue, sd->images[idx], sd->extent.width,
                                                         sd->extent.height, sd->format, sd->colorSpace,
                                                         currentWaitSemaphores,
                                                         currentWaitSemaphoreCount, shm, screenshotRequestId);
                }
                if (waitsConsumed) {
                    currentWaitSemaphores = nullptr;
                    currentWaitSemaphoreCount = 0;
                    modified = true;
                } else {
                    CompleteScreenshotRequest(shm, screenshotRequestId, ScreenshotRequestStatus::Failed,
                                              ERROR_READ_FAULT);
                }
            };

            if (captureBeforeOverlay) {
                doCapture();
            }
            if (screenshotBeforeOverlay) {
                doScreenshot();
            }
            doOverlay();
            if (captureAfterOverlay) {
                doCapture();
            }
            if (screenshotAfterOverlay) {
                doScreenshot();
            }
        }
    }

    // Create modified PresentInfo with chained semaphore
    VkPresentInfoKHR presentInfoCopy;
    if (pPresentInfo && modified) {
        presentInfoCopy = *pPresentInfo;
        if (currentWaitSemaphores && currentWaitSemaphoreCount > 0) {
            presentInfoCopy.waitSemaphoreCount = currentWaitSemaphoreCount;
            presentInfoCopy.pWaitSemaphores = currentWaitSemaphores;
        } else {
            // If we have no wait semaphore (e.g. game didn't provide one and we
            // didn't add one), we must ensure we don't pass garbage.
            presentInfoCopy.waitSemaphoreCount = 0;
            presentInfoCopy.pWaitSemaphores = nullptr;
        }
    }

    VkResult res = VK_SUCCESS;
    if (disp && disp->fp_vkQueuePresentKHR) {
        res = disp->fp_vkQueuePresentKHR(queue, (pPresentInfo && modified) ? &presentInfoCopy : pPresentInfo);
    }

    if (isFirstHook)
        g_InPresentHook = false;

    if (shm)
        shm->runtimeState.vulkanPresentThreadId.store(0, std::memory_order_release);

    // Log performance metrics
    if (PerfLogger::Get().IsEnabled()) {
        perfMetrics.totalUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - perfMetrics.qpcUs);
        PerfLogger::Get().LogFrame(perfMetrics);
    }

    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                             uint64_t timeout, VkSemaphore semaphore, VkFence fence,
                                                             uint32_t* pImageIndex) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp || !disp->fp_vkAcquireNextImageKHR)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!g_LayerState.whitelisted.load(std::memory_order_acquire))
        return disp->fp_vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);

    SwapchainData* sd = VulkanLayerState::Get().GetSwapchainData(swapchain);
    const bool preferDX9Path = IsDXVKD3D9WrapperLoaded() && !IsDXVKD3D11WrapperLoaded();
    if (sd) {
        sd->lastAcquireThreadId.store(GetCurrentThreadId(), std::memory_order_release);
        sd->lastAcquireTick.store(GetTickCount64(), std::memory_order_release);
        if (sd->asyncPresentDetected.load(std::memory_order_acquire) && !preferDX9Path) {
            g_SharedFpsLimiter.SetIPCClient(&g_IPCClient);
            // Async-present games acquire from a different thread than they
            // present on, so the present-time wait cannot throttle production;
            // gate every acquire on the cadence grid instead. DXVK keeps the
            // legacy behavior for the same reason as in vkQueuePresentKHR.
            g_SharedFpsLimiter.Apply(false, !IsDXVKD3D11WrapperLoaded());
        }
    }

    return disp->fp_vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator, VkSampler* pSampler) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp || !disp->fp_vkCreateSampler || !pCreateInfo)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!g_LayerState.whitelisted.load(std::memory_order_acquire))
        return disp->fp_vkCreateSampler(device, pCreateInfo, pAllocator, pSampler);
    VkSamplerCreateInfo modified = *pCreateInfo;
    {
        auto& state = VulkanLayerState::Get();

        bool specialReduction = false;
        for (const VkBaseInStructure* node = reinterpret_cast<const VkBaseInStructure*>(modified.pNext); node;
             node = node->pNext) {
            if (node->sType == VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO) {
                const auto* reduction = reinterpret_cast<const VkSamplerReductionModeCreateInfo*>(node);
                specialReduction = reduction->reductionMode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;
                break;
            }
        }
        const auto isMaterialAddress = [](VkSamplerAddressMode mode) {
            return mode == VK_SAMPLER_ADDRESS_MODE_REPEAT || mode == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        };
        const bool materialAddress = isMaterialAddress(modified.addressModeU) &&
                                     isMaterialAddress(modified.addressModeV) &&
                                     isMaterialAddress(modified.addressModeW);
        const bool borderAddress = modified.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                                   modified.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                                   modified.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        const bool mipmapped = modified.maxLod > 0.0f && modified.minLod < modified.maxLod;
        const bool linearMinMag = modified.minFilter == VK_FILTER_LINEAR && modified.magFilter == VK_FILTER_LINEAR;
        const bool standardMinMag =
            (modified.minFilter == VK_FILTER_NEAREST || modified.minFilter == VK_FILTER_LINEAR) &&
            (modified.magFilter == VK_FILTER_NEAREST || modified.magFilter == VK_FILTER_LINEAR);
        ce::vulkan_sampler_policy::Input policyInput = {};
        policyInput.mipmapped = mipmapped;
        policyInput.comparison = modified.compareEnable != VK_FALSE;
        policyInput.specialReduction = specialReduction;
        policyInput.borderAddress = borderAddress;
        policyInput.unnormalizedCoordinates = modified.unnormalizedCoordinates != VK_FALSE;
        policyInput.standardMinMag = standardMinMag;
        policyInput.linearMinMag = linearMinMag;
        policyInput.materialAddress = materialAddress;
        policyInput.aggressive = state.IsAggressiveSamplerOverride();
        const ce::vulkan_sampler_policy::Decision policy = ce::vulkan_sampler_policy::Classify(policyInput);

        if (policy.allowAnisotropy) {
            // Anisotropic filtering override
            if (state.IsAnisotropyOverrideActive()) {
                uint32_t maxAniso = state.GetMaxAnisotropy();
                if (maxAniso <= 1) {
                    // "off" - disable anisotropic filtering
                    modified.anisotropyEnable = VK_FALSE;
                    modified.maxAnisotropy = 1.0f;
                } else if (disp->samplerAnisotropyEnabled) {
                    modified.anisotropyEnable = VK_TRUE;
                    modified.maxAnisotropy =
                        std::min(static_cast<float>(maxAniso), std::max(1.0f, disp->maxSamplerAnisotropy));
                } else {
                    static std::atomic<int> s_anisotropyFeatureLogCount{0};
                    if (s_anisotropyFeatureLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                        LayerLog(
                            "Vulkan sampler: forced AF skipped because samplerAnisotropy was not enabled at "
                            "vkCreateDevice");
                    }
                }
            }

            // Mip bias override with mode support
            if (state.IsForceMipBiasClampEnabled()) {
                modified.mipLodBias = 0.0f;
            } else if (state.IsMipBiasOverrideActive()) {
                float userBias = state.GetMipLodBias();
                const char* mode = state.GetMipBiasMode();
                float originalBias = pCreateInfo->mipLodBias;

                if (strcmp(mode, "offset") == 0) {
                    modified.mipLodBias = originalBias + userBias;
                } else if (strcmp(mode, "base") == 0) {
                    if (originalBias >= 0.0f)
                        modified.mipLodBias = originalBias;
                    else
                        modified.mipLodBias = originalBias + userBias;
                } else {
                    // "strict" - absolute override
                    modified.mipLodBias = userBias;
                }

                const float maxBias = disp->maxSamplerLodBias > 0.0f ? disp->maxSamplerLodBias : 16.0f;
                modified.mipLodBias = std::clamp(modified.mipLodBias, -maxBias, maxBias);
            }
        }

        const ce::mip_mapping::Mode mipMode = ce::mip_mapping::ParseMode(state.GetMipMapping());
        if (ce::mip_mapping::IsExplicit(mipMode)) {
            static std::atomic<int> s_mipAppliedLogCount{0};
            static std::atomic<int> s_mipSkippedLogCount{0};
            if (policy.allowMipMapping) {
                if (mipMode == ce::mip_mapping::Mode::Nearest) {
                    modified.minFilter = VK_FILTER_NEAREST;
                    modified.magFilter = VK_FILTER_NEAREST;
                    modified.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                } else {
                    modified.minFilter = VK_FILTER_LINEAR;
                    modified.magFilter = VK_FILTER_LINEAR;
                    modified.mipmapMode = mipMode == ce::mip_mapping::Mode::Trilinear
                                              ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                              : VK_SAMPLER_MIPMAP_MODE_NEAREST;
                }
                const int logIndex = s_mipAppliedLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logIndex < 24) {
                    LayerLog("Vulkan sampler: mip override mode=%s min=%d mag=%d mip=%d (#%d)",
                             state.GetMipMapping(), static_cast<int>(modified.minFilter),
                             static_cast<int>(modified.magFilter), static_cast<int>(modified.mipmapMode), logIndex + 1);
                }
            } else {
                const int logIndex = s_mipSkippedLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logIndex < 24) {
                    LayerLog("Vulkan sampler: mip override mode=%s skipped reason=%s (#%d)", state.GetMipMapping(),
                             ce::vulkan_sampler_policy::ReasonName(policy.mipMappingReason), logIndex + 1);
                }
            }
        }
    }
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison) - modified is a byte copy of pCreateInfo, so padding cannot differ
    const bool changed = std::memcmp(&modified, pCreateInfo, sizeof(modified)) != 0;
    VkResult result = disp->fp_vkCreateSampler(device, &modified, pAllocator, pSampler);
    if (result != VK_SUCCESS && changed) {
        static std::atomic<int> s_fallbackLogCount{0};
        if (s_fallbackLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            LayerLog("Vulkan sampler: overridden descriptor failed result=%d; retrying original transactionally",
                     static_cast<int>(result));
        }
        result = disp->fp_vkCreateSampler(device, pCreateInfo, pAllocator, pSampler);
    }
    return result;
}

#ifdef VK_USE_PLATFORM_WIN32_KHR
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateWin32SurfaceKHR(VkInstance instance,
                                                               const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
                                                               const VkAllocationCallbacks* pAllocator,
                                                               VkSurfaceKHR* pSurface) {
    InstanceDispatch* disp = VulkanLayerState::Get().GetInstanceDispatch(instance);
    if (!disp || !disp->fp_vkCreateWin32SurfaceKHR)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = disp->fp_vkCreateWin32SurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    if (res == VK_SUCCESS) {
        VulkanLayerState::Get().RegisterSurface(*pSurface, pCreateInfo->hwnd);
    }
    return res;
}
#endif
