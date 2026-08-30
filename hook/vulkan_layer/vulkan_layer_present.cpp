#include "vulkan_layer_internal.h"
#include "vulkan_present_boundary.h"
#include "vulkan_reflex_limiter.h"

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

    bool isFirstHook = !g_InPresentHook;
    g_InPresentHook = true;

    SwapchainData* sd = nullptr;
    if (g_LayerState.whitelisted && pPresentInfo && pPresentInfo->swapchainCount > 0) {
        sd = VulkanLayerState::Get().GetSwapchainData(pPresentInfo->pSwapchains[0]);
    }

    VkDevice queueDevice = VulkanLayerState::Get().GetVkDeviceFromQueue(queue);

    // A compute-only present queue may be fed by a graphics queue owned by a
    // different application thread. Learn that exact semaphore route before
    // applying the configured queue-depth marker.
    LearnPrerenderProducerTopology(sd, queue, pPresentInfo);

    const bool runtimeEligible = sd && sd->extent.width >= 320 && sd->extent.height >= 180;
    if (sd && !sd->runtimeInitialized.exchange(true, std::memory_order_acq_rel)) {
        if (runtimeEligible) {
            InitializeOverlay(sd->device, sd->swapchain, sd->format, sd->colorSpace, sd->extent, sd->imageUsage,
                              sd->imageCount, sd->images.data(), sd->window);
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

    bool asyncPresentDetected = ce::vulkan_present_boundary::ResolvePresentLimiterBoundary(sd, queueDevice);

    if (isFirstHook) {
        // Apply queue-depth control to the render-producing graphics queue.
        // When presentation itself uses a graphics queue that is the producer;
        // otherwise use the semaphore-derived queue cached above. The existing
        // same-thread check protects Vulkan's external queue synchronization.
        float prerenderLimit = VulkanLayerState::Get().GetPrerenderLimit();
        const bool paceOnProducerSubmit =
            sd && sd->prerenderOnProducerSubmit.load(std::memory_order_acquire);
        if (prerenderLimit >= 0.0f && !paceOnProducerSubmit && !asyncPresentDetected &&
            queueDevice != VK_NULL_HANDLE) {
            VkQueue prerenderQueue = VK_NULL_HANDLE;
            if (VulkanLayerState::Get().QueueSupportsGraphics(queue)) {
                prerenderQueue = queue;
            } else if (sd) {
                prerenderQueue = sd->prerenderProducerQueue.load(std::memory_order_acquire);
            }
            if (prerenderQueue != VK_NULL_HANDLE &&
                VulkanLayerState::Get().GetVkDeviceFromQueue(prerenderQueue) == queueDevice &&
                VulkanLayerState::Get().QueueSupportsGraphics(prerenderQueue)) {
                const uint32_t producerThreadId =
                    VulkanLayerState::Get().GetQueueLastSubmitThreadId(prerenderQueue);
                if (producerThreadId == 0 || producerThreadId == GetCurrentThreadId()) {
                    ApplyPrerenderLimitVulkan(queueDevice, prerenderQueue, prerenderLimit);
                }
            }
        }
    }

    // FPS limiter: pace EVERY present, not only the first one entering the hook.
    // Strange Brigade Vulkan presents several swapchain images per frame period
    // (concurrent present streams); gating only the first present let the other
    // images through unpaced, so the displayed rate stayed at 2x the configured
    // target with alternating short/long frame times and bad 1% lows. The
    // limiter's gateEveryPresent mode serializes concurrent presents onto the
    // cadence grid: exactly one present per target interval, evenly spaced.
    // Basic pacing waits before the driver call; native pacing keeps FIFO
    // untouched and hands the interval to the game's pre-input sleep or CE's
    // post-Present driver-signalled boundary.
    // DXVK D3D9 is paced here too: Vulkan owns the final presentation boundary,
    // and actively bootstrapping a parallel DX9 hook is unsafe for translation
    // runtimes with global renderer state.
    if (!asyncPresentDetected) {
        const bool nativeVulkanPresent = !IsDXVKD3D11WrapperLoaded();
        const int64_t fpsLimitStartUs = PerfLogger::GetQpcUs();
        if (shm) {
            const bool sharedDLSSFGActive = shm->dlssState.fgActive.load(std::memory_order_acquire);
            const int sharedDLSSFGMultiplier = shm->dlssState.mfgMultiplier.load(std::memory_order_acquire);
            if (sharedDLSSFGActive) {
                g_FGCompat.SetDLSSFGMultiplier(std::clamp(sharedDLSSFGMultiplier, 2, 4));
                g_FGCompat.SetDLSSFGActive(true);
            } else if (g_FGCompat.IsDLSSFGApiActive()) {
                g_FGCompat.SetDLSSFGActive(false);
                g_FGCompat.SetDLSSFGMultiplier(0);
            }
        }
        g_VulkanReflexLimiter.SetDevice(queueDevice, disp);
        g_SharedFpsLimiter.SetIPCClient(&g_IPCClient);
        g_SharedFpsLimiter.SetNativePacingBackend(GetVulkanNativeFpsPacingBackend());
        ce::vulkan_present_boundary::ReportPresentTimeLimiterBoundary(sd, nativeVulkanPresent);
        g_SharedFpsLimiter.Apply(true, nativeVulkanPresent);
        perfMetrics.fpsLimitWaitUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - fpsLimitStartUs);
    }

    // NOTE: the overlay's own FPS/percentile statistics are sampled *after* the
    // present down-call, not here. They feed the perf CSV and nothing else, and
    // every microsecond spent in this hook before the down-call is a microsecond
    // the present is late by. That is invisible on a swapchain with spare
    // images and fully visible on one without - DOOM Eternal asks for two images
    // with "present from compute" on and three with it off, which is why CE's
    // per-present CPU showed up as a frame-rate difference between the two.

    const VkSemaphore* currentWaitSemaphores =
        (pPresentInfo && pPresentInfo->waitSemaphoreCount > 0) ? pPresentInfo->pWaitSemaphores : nullptr;
    uint32_t currentWaitSemaphoreCount = pPresentInfo ? pPresentInfo->waitSemaphoreCount : 0;
    std::vector<VkSemaphore> chainedWaitSemaphores;
    bool modified = false;

    // Frame-generation present metering displaces the swapchain's FIFO rate
    // contract (see vulkan_present_metering_policy.h). Resolve it before the
    // overlay work so the down-call is not delayed by anything avoidable: on a
    // present that carries no metering request this is a single load and one
    // pointer test.
    const void* meteringFreePresentChain = nullptr;
    bool suppressPresentMetering = false;
    if (pPresentInfo && pPresentInfo->pNext && VulkanLayerState::Get().WantsVblankPacedPresentation()) {
        const ce::vulkan_present_metering_policy::ChainScan scan =
            ce::vulkan_present_metering_policy::ScanPresentChain(pPresentInfo->pNext);
        if (scan.found) {
            ce::vulkan_present_metering_policy::Input meteringInput = {};
            meteringInput.vblankPacedPresentationRequested = true;
            meteringInput.swapchainPresentModeKnown = (sd != nullptr);
            meteringInput.swapchainPresentMode = sd ? sd->presentMode : VK_PRESENT_MODE_IMMEDIATE_KHR;
            meteringInput.swapchainCount = pPresentInfo->swapchainCount;
            meteringInput.meteredFramesPerBatch = scan.framesPerBatch;
            meteringInput.meteringRequestIsChainHead = scan.isChainHead;
            const ce::vulkan_present_metering_policy::Decision decision =
                ce::vulkan_present_metering_policy::Decide(meteringInput);
            if (decision.suppressMetering) {
                meteringFreePresentChain = scan.chainWithoutMetering;
                suppressPresentMetering = true;
                modified = true;
            }
            // One line per state, not per present: activation, a frame
            // generation multiplier change, or a chain shape CE cannot unlink.
            if (sd) {
                const uint32_t logState = (scan.framesPerBatch & 0xFFFFu) | (decision.suppressMetering ? 0x10000u : 0u) |
                                          (decision.blockedByChainPosition ? 0x20000u : 0u) | 0x80000000u;
                if (sd->presentMeteringLogState.load(std::memory_order_relaxed) != logState &&
                    sd->presentMeteringLogState.exchange(logState, std::memory_order_acq_rel) != logState) {
                    LayerLog(
                        "Vulkan Layer: frame-generation present metering numFramesPerBatch=%u "
                        "(swapchain presentMode=%d images=%u chainNodes=%u chainHead=%d) - %s",
                        scan.framesPerBatch, static_cast<int>(sd->presentMode), sd->imageCount, scan.nodeCount,
                        scan.isChainHead ? 1 : 0,
                        decision.suppressMetering
                            ? "suppressed so the swapchain's vertical-blank pacing stays authoritative"
                            : (decision.blockedByChainPosition
                                   ? "NOT suppressed: the request is not the chain head and unlinking it would "
                                     "write application memory"
                                   : "left in place: this present mode has no vertical-blank rate contract to defend"));
                }
            }
        }
    }

    // The present-mode selection is resolved on the chain the metering unlink
    // already produced, so the two head-only substitutions compose instead of
    // fighting over the same node.
    const void* effectivePresentChain = suppressPresentMetering ? meteringFreePresentChain
                                                                : (pPresentInfo ? pPresentInfo->pNext : nullptr);
    ce::vulkan_present_chain::SwapchainPresentModeInfo forcedPresentModeNode = {};
    VkPresentModeKHR forcedPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    bool forcePresentModeSelection = false;
    if (pPresentInfo && effectivePresentChain) {
        const ce::vulkan_present_chain::ChainDescription chain =
            ce::vulkan_present_chain::DescribeChain(effectivePresentChain, nullptr, 0);
        if (sd) {
            LogPresentChainIfChanged("vkQueuePresentKHR", effectivePresentChain, chain, sd->presentChainLogState,
                                     chain.hasPresentModeSelection
                                         ? "the application selects a present mode per present"
                                         : "no per-present mode selection");
        }
        ce::vulkan_present_chain::PresentModeSelectionInput selectionInput = {};
        selectionInput.vblankPacedPresentationRequested = VulkanLayerState::Get().WantsVblankPacedPresentation();
        selectionInput.swapchainPresentModeKnown = (sd != nullptr);
        selectionInput.swapchainPresentMode = sd ? sd->presentMode : VK_PRESENT_MODE_IMMEDIATE_KHR;
        selectionInput.presentSwapchainCount = pPresentInfo->swapchainCount;
        selectionInput.hasPresentModeSelection = chain.hasPresentModeSelection;
        selectionInput.presentModeSelectionIsChainHead = chain.presentModeSelectionIsChainHead;
        selectionInput.presentModeSelectionSwapchainCount = chain.presentModeSelectionSwapchainCount;
        selectionInput.selectedPresentMode = chain.selectedPresentMode;
        const ce::vulkan_present_chain::PresentModeSelectionDecision selectionDecision =
            ce::vulkan_present_chain::DecidePresentModeSelection(selectionInput);
        if (selectionDecision.forceCreatedMode) {
            forcedPresentMode = sd->presentMode;
            forcedPresentModeNode.sType = ce::vulkan_present_chain::kStructureTypeSwapchainPresentModeInfo;
            forcedPresentModeNode.pNext = chain.chainWithoutPresentModeSelection;
            forcedPresentModeNode.swapchainCount = 1;
            forcedPresentModeNode.pPresentModes = &forcedPresentMode;
            forcePresentModeSelection = true;
            modified = true;
        }
        if (selectionDecision.forceCreatedMode || selectionDecision.blockedByChainPosition) {
            static std::atomic<int> s_selectionLogCount{0};
            if (s_selectionLogCount.fetch_add(1, std::memory_order_relaxed) < 4) {
                LayerLog(
                    "Vulkan Layer: per-present mode selection %d conflicts with the swapchain's created mode %d - %s",
                    static_cast<int>(chain.selectedPresentMode), static_cast<int>(selectionInput.swapchainPresentMode),
                    selectionDecision.forceCreatedMode
                        ? "forced back to the created mode"
                        : "NOT forced: the selection is not the chain head and replacing it would write application "
                          "memory");
            }
        }
    }

    if (g_LayerState.whitelisted && pPresentInfo && pPresentInfo->swapchainCount > 0) {

        if (sd) {
            uint32_t idx = pPresentInfo->pImageIndices[0];
            perfMetrics.sourceFrameIndex = idx + 1;

            // OPTIMIZATION: Keep overlay/capture queue work GPU-ordered while letting config choose whether
            // screenshots and capture happen before or after overlay submission.
            VkSemaphore overlayDone = VK_NULL_HANDLE;
            OverlayConfig overlayCfg{};
            overlayCfg.captureIncludeOverlay = true;
            overlayCfg.screenshotIncludeOverlay = true;
            if (shm) {
                overlayCfg = shm->ReadOverlayConfig();
            }
            const bool overlayEnabled = shm && overlayCfg.showOverlay;
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
                int32_t overlayGpuUs = 0;
                int64_t overlayStartUs = PerfLogger::GetQpcUs();
                bool overlayRendered = RenderOverlay(sd->device, queue, idx, currentWaitSemaphores,
                                                     currentWaitSemaphoreCount, &overlayDone, asyncPresentDetected,
                                                     &fenceWaitUs, &overlayGpuUs,
                                                     sd->imageAcquireGeneration.get(), sd->imageCount);
                perfMetrics.overlayGpuUs = overlayGpuUs;
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
        if (suppressPresentMetering) {
            // Unlinking the head node needs no write to the application's own
            // chain: CE's copy simply starts one node later.
            presentInfoCopy.pNext = meteringFreePresentChain;
        }
        if (forcePresentModeSelection) {
            // Same rule, one step further: CE's copy points at its own head
            // node, which carries the created mode and links to the rest of the
            // application's chain unchanged.
            presentInfoCopy.pNext = &forcedPresentModeNode;
        }
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

    // The three-way split of the hook's wall time. CE's work before the
    // down-call delays the present itself; its work after the down-call delays
    // the game's next frame. Both are on the game's thread, so a swapchain with
    // no spare image pays for each of them in frame time, and a session that
    // reports a frame-rate cost has to be able to say which one it is paying.
    const int64_t presentCallStartUs = PerfLogger::GetQpcUs();
    perfMetrics.prePresentUs = static_cast<int32_t>(presentCallStartUs - perfMetrics.qpcUs);

    VkResult res = VK_SUCCESS;
    if (disp && disp->fp_vkQueuePresentKHR) {
        res = disp->fp_vkQueuePresentKHR(queue, (pPresentInfo && modified) ? &presentInfoCopy : pPresentInfo);
    }
    const int64_t presentCallEndUs = PerfLogger::GetQpcUs();
    perfMetrics.presentCallUs = static_cast<int32_t>(presentCallEndUs - presentCallStartUs);
    if (!asyncPresentDetected) {
        if (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR) {
            g_SharedFpsLimiter.ApplyPostPresent();
            perfMetrics.fpsLimitWaitUs += static_cast<int32_t>(g_SharedFpsLimiter.GetLastWaitUs());
        } else {
            g_SharedFpsLimiter.CancelPostPresentPacing();
        }
    }

    if (isFirstHook)
        g_InPresentHook = false;

    if (shm)
        shm->runtimeState.vulkanPresentThreadId.store(0, std::memory_order_release);

    // Log performance metrics. The overlay's percentile statistics are sampled
    // here rather than before the down-call: each of them scans up to five
    // seconds of frame history, and they exist only to fill this CSV row.
    if (PerfLogger::Get().IsEnabled()) {
        if (auto* perf = GetOverlayPerformanceMetrics(queueDevice)) {
            perfMetrics.sourceCurrentFpsTimes100 = static_cast<int32_t>(std::lround(perf->GetCurrentFPS() * 100.0f));
            perfMetrics.source1PctLowTimes100 = static_cast<int32_t>(std::lround(perf->Get1PercentLowFPS() * 100.0f));
            perfMetrics.sourcePoint1PctLowTimes100 =
                static_cast<int32_t>(std::lround(perf->Get01PercentLowFPS() * 100.0f));
            perfMetrics.sourceFrameTimeStdDevUs = static_cast<int32_t>(std::lround(perf->GetWindowStdDev()));
        }
        const int64_t endUs = PerfLogger::GetQpcUs();
        perfMetrics.postPresentUs = static_cast<int32_t>(endUs - presentCallEndUs);
        perfMetrics.totalUs = static_cast<int32_t>(endUs - perfMetrics.qpcUs);
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
    if (sd) {
        sd->lastAcquireThreadId.store(GetCurrentThreadId(), std::memory_order_release);
        sd->lastAcquireTick.store(GetTickCount64(), std::memory_order_release);
        if (sd->asyncPresentDetected.load(std::memory_order_acquire)) {
            g_SharedFpsLimiter.SetIPCClient(&g_IPCClient);
            // Async-present games acquire from a different thread than they
            // present on, so the present-time wait cannot throttle production;
            // gate every acquire on the cadence grid instead.
            const bool groupedAdmission = !IsDXVKD3D11WrapperLoaded();
            ce::vulkan_present_boundary::ReportAcquireTimeLimiterBoundary(sd, groupedAdmission);
            g_SharedFpsLimiter.Apply(false, groupedAdmission);
        }
    }

    const VkResult acquireResult =
        disp->fp_vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
    // A successful acquire is the proof the overlay's submission ring needs
    // that every present of this image - including the one that waited on a
    // slot's binary semaphore - has executed its wait. See
    // IsSubmissionSlotReusable in overlay_submit_queue_policy.h.
    if ((acquireResult == VK_SUCCESS || acquireResult == VK_SUBOPTIMAL_KHR) && sd && pImageIndex &&
        sd->imageAcquireGeneration && *pImageIndex < sd->imageCount) {
        sd->imageAcquireGeneration[*pImageIndex].fetch_add(1, std::memory_order_acq_rel);
    }
    return acquireResult;
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
                const VkBool32 originalEnable = modified.anisotropyEnable;
                const float originalValue = modified.maxAnisotropy;
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
                if ((modified.anisotropyEnable != originalEnable || modified.maxAnisotropy != originalValue) &&
                    (maxAniso <= 1 || disp->samplerAnisotropyEnabled)) {
                    static std::atomic<int> s_anisotropyAppliedLogCount{0};
                    if (s_anisotropyAppliedLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                        LayerLog(
                            "Vulkan sampler: forced AF applied enable=%u/%.1fx -> %u/%.1fx "
                            "(requested=%ux deviceMax=%.1fx)",
                            originalEnable, originalValue, modified.anisotropyEnable, modified.maxAnisotropy,
                            maxAniso, disp->maxSamplerAnisotropy);
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
