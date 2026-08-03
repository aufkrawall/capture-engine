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

    SwapchainData* sd = VulkanLayerState::Get().GetSwapchainData(swapchain);
    const bool preferDX9Path = IsDXVKD3D9WrapperLoaded() && !IsDXVKD3D11WrapperLoaded();
    if (sd) {
        sd->lastAcquireThreadId.store(GetCurrentThreadId(), std::memory_order_release);
        sd->lastAcquireTick.store(GetTickCount64(), std::memory_order_release);
        if (sd->asyncPresentDetected.load(std::memory_order_acquire) && !preferDX9Path) {
            g_SharedFpsLimiter.SetIPCClient(&g_IPCClient);
            g_SharedFpsLimiter.Apply();
        }
    }

    return disp->fp_vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator, VkSampler* pSampler) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp || !disp->fp_vkCreateSampler || !pCreateInfo)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkSamplerCreateInfo modified = *pCreateInfo;
    if (g_LayerState.whitelisted) {
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
