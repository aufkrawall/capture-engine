#include "layer_capture_internal.h"

bool CaptureFrame(VkDevice device, VkSwapchainKHR swapchain, VkQueue queue, VkImage srcImage,
                  uint32_t swapchainImageIndex, const VkSemaphore* waitSemaphores,
                  uint32_t waitSemaphoreCount, VkSemaphore* signaledSemaphore,
                  bool* captureStateMissing, const FrameCaptureMetadata* metadata) {
    if (signaledSemaphore)
        *signaledSemaphore = VK_NULL_HANDLE;
    if (captureStateMissing)
        *captureStateMissing = false;
    std::unique_lock<std::mutex> lock(layer_capture_g_CaptureMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        if (auto* mem = g_IPCClient.GetSharedMem())
            mem->runtimeState.injectProducerCaptureLockDrops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    auto it = layer_capture_g_CaptureStates.find(device);
    if (it == layer_capture_g_CaptureStates.end() || !it->second.initialized || it->second.swapchain != swapchain) {
        if (captureStateMissing)
            *captureStateMissing = true;
        return false;
    }

    // Check if we should throttle capture (encoder is falling behind)
    if (g_IPCClient.GetSharedMem()) {
        if (g_IPCClient.GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
            return false;
        }
    }

    VulkanCaptureState& state = it->second;
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp)
        return false;
    if (swapchainImageIndex >= state.signalSemaphores.size() ||
        swapchainImageIndex >= state.presentedImages.size()) {
        return false;
    }
    const VkSemaphore signalSemaphore = state.signalSemaphores[swapchainImageIndex];
    if (signalSemaphore == VK_NULL_HANDLE || srcImage == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
        return false;
    }

    const bool imageWasPresented = state.presentedImages[swapchainImageIndex];
    state.presentedImages[swapchainImageIndex] = true;
    if (imageWasPresented) {
        // Reacquiring the image proves its preceding present wait was consumed,
        // so retired capture semaphores can be reclaimed under this same
        // non-blocking state lock instead of taking two more hot-path locks.
        for (auto retired = layer_capture_g_RetiredCaptureStates.begin();
             retired != layer_capture_g_RetiredCaptureStates.end();) {
            if (retired->device == device && CaptureStateCopiesComplete(*retired, disp)) {
                DestroyCaptureStateResources(*retired, disp);
                retired = layer_capture_g_RetiredCaptureStates.erase(retired);
            } else {
                ++retired;
            }
        }
    }

    const uint32_t queueFamilyIndex = VulkanLayerState::Get().GetQueueFamilyIndex(queue);
    if (queueFamilyIndex == VK_QUEUE_FAMILY_IGNORED) {
        static std::atomic<int> s_unknownQueueLogCount{0};
        if (s_unknownQueueLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            LayerLog("Vulkan Layer: Capture skipped because present queue family is unknown");
        }
        return false;
    }

    if (!VulkanLayerState::Get().QueueSupportsTransfer(queue)) {
        static std::atomic<int> s_nonTransferQueueLogCount{0};
        if (s_nonTransferQueueLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            LayerLog("Vulkan Layer: Capture skipped on queue family %u without transfer support", queueFamilyIndex);
        }
        return false;
    }

    VulkanCaptureState::CommandResources* commandResources =
        EnsureCaptureCommandResources(state, disp, device, queueFamilyIndex);
    if (!commandResources) {
        return false;
    }

    auto* mem = g_IPCClient.GetSharedMem();
    const bool allowDxvkEncoderTextures = (state.interopMode == VulkanCaptureInteropMode::kDxvkD3D11);
    const bool encoderAdoptionRequested = allowDxvkEncoderTextures && mem &&
                                          !mem->useEncoderTextures.load(std::memory_order_acquire) &&
                                          mem->encoderTextures.kmtReady.load(std::memory_order_acquire) &&
                                          state.captureFrameCounter >= state.nextEncoderImportRetryFrame;
    if (encoderAdoptionRequested && mem->frameRing.readIndex.load(std::memory_order_acquire) !=
                                        mem->frameRing.writeIndex.load(std::memory_order_acquire)) {
        // Handles/fence are generation-global in shared memory. Stop producing
        // briefly so every old-generation lease drains before replacing them.
        static std::atomic<int> s_encoderAdoptionDrainLogCount{0};
        if (s_encoderAdoptionDrainLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
            LayerLog("Vulkan Layer: Deferring encoder-texture adoption until old capture leases drain");
        }
        return false;
    }
    if (encoderAdoptionRequested && !CaptureStateCopiesComplete(state, disp))
        return false;
    if (encoderAdoptionRequested) {
        SharedTextureEntry encoderEntry;
        HANDLE kmtHandles[ENCODER_TEXTURE_SLOT_COUNT] = {};
        if (ImportEncoderKmtTextures(device, disp, state.luidKey, state.captureWidth, state.captureHeight,
                                     state.captureFormat, mem, &encoderEntry, kmtHandles)) {
            {
                std::unique_lock<std::mutex> texLock(layer_capture_g_InteropMutex, std::try_to_lock);
                if (!texLock.owns_lock()) {
                    DestroySharedTextureEntryResources(encoderEntry, disp);
                    state.nextEncoderImportRetryFrame = state.captureFrameCounter + 60;
                    return false;
                }
                for (auto textureIt = layer_capture_g_TextureCache.begin(); textureIt != layer_capture_g_TextureCache.end();) {
                    if (textureIt->vkDevice == device && textureIt->luidKey == state.luidKey &&
                        textureIt->width == state.captureWidth && textureIt->height == state.captureHeight &&
                        textureIt->vkFormat == state.captureFormat) {
                        DestroySharedTextureEntryResources(*textureIt, disp);
                        textureIt = layer_capture_g_TextureCache.erase(textureIt);
                    } else {
                        ++textureIt;
                    }
                }
                layer_capture_g_TextureCache.push_back(std::move(encoderEntry));
            }

            LayerIPC_SetTextures(kmtHandles, ENCODER_TEXTURE_SLOT_COUNT, state.captureWidth, state.captureHeight,
                                 VkFormatToDXGI((VkFormat)state.captureFormat));
            mem->useEncoderTextures.store(true, std::memory_order_release);
            state.sharedImageInitialized.fill(false);
            state.relayCompletionValues.fill(0);
            LayerLog("Vulkan Layer: DXVK d3d11 zero-copy: adopted encoder KMT textures after media startup");
        } else {
            state.nextEncoderImportRetryFrame = state.captureFrameCounter + 60;
        }
    }

    // Get shared textures from cache
    SharedTextureEntry* sharedTextures = nullptr;
    for (auto& entry : layer_capture_g_TextureCache) {
        if (entry.vkDevice == device && entry.luidKey == state.luidKey && entry.width == state.captureWidth &&
            entry.height == state.captureHeight && entry.vkFormat == state.captureFormat && entry.valid) {
            sharedTextures = &entry;
            break;
        }
    }
    if (!sharedTextures || !sharedTextures->valid)
        return false;
    if (state.copyFences.empty() || commandResources->buffers.empty() || state.signalSemaphores.empty() ||
        sharedTextures->vkImages.empty()) {
        LayerLog("Vulkan Layer: Capture skipped because synchronization/image resources are incomplete");
        return false;
    }

    // Use a monotonic counter independent of swapchain image patterns, but
    // rotate over the actual shared texture count rather than assuming four.
    const uint32_t sharedTextureCount =
        static_cast<uint32_t>(std::min<size_t>(sharedTextures->vkImages.size(), SHARED_TEXTURE_SLOT_COUNT));
    if (sharedTextureCount == 0)
        return false;
    const bool doRelay = sharedTextures->hasIpcRelay && state.d3d11Fence && state.d3d11Context4;
    ID3D11Fence* relayCompletionFence = state.d3d11IpcFence ? state.d3d11IpcFence : state.d3d11Fence;
    const uint32_t firstSlot = static_cast<uint32_t>(state.captureFrameCounter++ % sharedTextureCount);
    int32_t availableSlot = -1;
    uint32_t cpuBusySlots = 0;
    uint32_t gpuBusySlots = 0;
    for (uint32_t offset = 0; offset < sharedTextureCount; ++offset) {
        const uint32_t candidate = (firstSlot + offset) % sharedTextureCount;
        if (IsCaptureTextureSlotOutstanding(mem, static_cast<int32_t>(candidate))) {
            ++cpuBusySlots;
            continue;
        }
        if (doRelay && state.relayCompletionValues[candidate] != 0) {
            const uint64_t completedRelayValue = relayCompletionFence->GetCompletedValue();
            if (completedRelayValue == UINT64_MAX) {
                LayerLog("Vulkan Layer: IPC relay fence reported device removal; disabling capture state");
                state.initialized = false;
                return false;
            }
            if (completedRelayValue < state.relayCompletionValues[candidate]) {
                ++gpuBusySlots;
                continue;
            }
        }
        const uint32_t candidateFenceIndex = candidate % state.copyFences.size();
        const VkFence candidateFence = state.copyFences[candidateFenceIndex];
        const VkResult fenceStatus = disp->fp_vkWaitForFences(device, 1, &candidateFence, VK_TRUE, 0);
        if (fenceStatus != VK_SUCCESS) {
            if (fenceStatus == VK_ERROR_DEVICE_LOST) {
                state.initialized = false;
                return false;
            }
            ++gpuBusySlots;
            continue;
        }
        availableSlot = static_cast<int32_t>(candidate);
        break;

    }
    if (availableSlot < 0) {
        if (mem) {
            if (cpuBusySlots != 0)
                mem->runtimeState.injectProducerCpuLeaseBusyDrops.fetch_add(1, std::memory_order_relaxed);
            if (gpuBusySlots != 0)
                mem->runtimeState.injectProducerGpuBusyDrops.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }
    const uint32_t slotIndex = static_cast<uint32_t>(availableSlot);
    LARGE_INTEGER sourceQpc = {};
    if (metadata && metadata->timestampQpc > 0) {
        sourceQpc.QuadPart = metadata->timestampQpc;
    } else {
        QueryPerformanceCounter(&sourceQpc);
    }

    // Fence/command-buffer ownership follows the destination texture slot.
    // Swapchains may have more images than the capture ring; indexing these by
    // image would not prove that an independently rotated destination is idle.
    uint32_t fenceIndex = slotIndex % state.copyFences.size();
    VkFence fence = state.copyFences[fenceIndex];

    // The scan above proved both the Vulkan submission and any D3D11 relay
    // operation for this exact slot complete without waiting on Present.

    uint32_t cmdIndex = slotIndex % commandResources->buffers.size();
    VkCommandBuffer cmd = commandResources->buffers[cmdIndex];
    const VkResult commandResetResult = disp->fp_vkResetCommandBuffer(cmd, 0);
    if (commandResetResult != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Capture command-buffer reset failed (index=%u family=%u result=%d)", cmdIndex,
                 queueFamilyIndex, commandResetResult);
        return false;
    }
    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                          VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    const VkResult beginResult = disp->fp_vkBeginCommandBuffer(cmd, &beginInfo);
    if (beginResult != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Capture command-buffer begin failed (index=%u family=%u result=%d)", cmdIndex,
                 queueFamilyIndex, beginResult);
        return false;
    }

    const uint32_t externalQueueFamily = VK_QUEUE_FAMILY_EXTERNAL;

    // Transition and copy
    VkImageMemoryBarrier srcBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    srcBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;  // Paranoid: Wait for everything
    srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;  // Assume presentable layout from game
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier.image = srcImage;
    srcBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier dstBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    dstBarrier.srcAccessMask = 0;
    dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier.oldLayout =
        state.sharedImageInitialized[slotIndex] ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.srcQueueFamilyIndex = externalQueueFamily;
    dstBarrier.dstQueueFamilyIndex = queueFamilyIndex;
    dstBarrier.image = sharedTextures->vkImages[slotIndex];
    dstBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier barriers[] = {srcBarrier, dstBarrier};
    // Use ALL_COMMANDS to ensure we catch any previous usage (compute, graphics,
    // etc.)
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                  nullptr, 0, nullptr, 2, barriers);

    VkImageCopy region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffset = {0, 0, 0};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffset = {0, 0, 0};
    region.extent = {(uint32_t)sharedTextures->width, (uint32_t)sharedTextures->height, 1};

    disp->fp_vkCmdCopyImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sharedTextures->vkImages[slotIndex],
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier srcBarrier2 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    srcBarrier2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier2.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    srcBarrier2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    srcBarrier2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier2.image = srcImage;
    srcBarrier2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier dstBarrier2 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    dstBarrier2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier2.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    dstBarrier2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dstBarrier2.srcQueueFamilyIndex = queueFamilyIndex;
    dstBarrier2.dstQueueFamilyIndex = externalQueueFamily;
    dstBarrier2.image = sharedTextures->vkImages[slotIndex];
    dstBarrier2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier postBarriers[] = {srcBarrier2, dstBarrier2};
    // Transition back for Present, enabling all subsequent stages
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                  nullptr, 0, nullptr, 2, postBarriers);

    if (disp->fp_vkEndCommandBuffer(cmd) != VK_SUCCESS)
        return false;

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr};

    // We signal TWO semaphores:
    // 1. The binary semaphore for Present to wait on (passed as arg)
    // 2. The timeline semaphore for the Encoder to wait on (in state struct)
    // Use stack arrays to avoid heap allocation in the capture hot path (max 2 each)
    VkSemaphore signalSems[2];
    uint64_t signalValues[2] = {0, 0};
    uint32_t signalCount = 0;

    if (signalSemaphore != VK_NULL_HANDLE) {
        signalSems[signalCount] = signalSemaphore;
        signalValues[signalCount] = 0;  // Binary semaphore ignores value
        signalCount++;
    }

    // When IPC relay is active, we use two fence values per frame:
    //   vulkanSignalValue: Vulkan signals after copy to KMT texture
    //   encoderFenceValue: D3D11 signals after relay copy to NT IPC texture
    // The encoder waits on encoderFenceValue.
    uint64_t vulkanSignalValue, encoderFenceValue;
    if (doRelay) {
        state.currentFenceValue += 2;
        vulkanSignalValue = state.currentFenceValue - 1;
        encoderFenceValue = state.currentFenceValue;
    } else {
        vulkanSignalValue = ++state.currentFenceValue;
        encoderFenceValue = vulkanSignalValue;
    }

    if (state.timelineSemaphore != VK_NULL_HANDLE) {
        signalSems[signalCount] = state.timelineSemaphore;
        signalValues[signalCount] = vulkanSignalValue;
        signalCount++;
    }

    if (signalCount > 0) {
        submit.signalSemaphoreCount = signalCount;
        submit.pSignalSemaphores = signalSems;
    }

    // Wait Semaphores
    std::vector<uint64_t> waitValues;
    std::vector<VkPipelineStageFlags> waitStages;
    uint32_t waitCount = 0;

    if (waitSemaphores && waitSemaphoreCount > 0) {
        waitStages.assign(waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
        submit.waitSemaphoreCount = waitSemaphoreCount;
        submit.pWaitSemaphores = waitSemaphores;
        submit.pWaitDstStageMask = waitStages.data();
        waitValues.assign(waitSemaphoreCount, 0);
        waitCount = waitSemaphoreCount;
    }

    // Prepare Timeline Info
    VkTimelineSemaphoreSubmitInfo timelineSubmit = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineSubmit.waitSemaphoreValueCount = waitCount;
    timelineSubmit.pWaitSemaphoreValues = waitCount > 0 ? waitValues.data() : nullptr;
    timelineSubmit.signalSemaphoreValueCount = signalCount;
    timelineSubmit.pSignalSemaphoreValues = signalValues;

    submit.pNext = &timelineSubmit;

    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;

    const VkResult resetResult = disp->fp_vkResetFences(device, 1, &fence);
    if (resetResult != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Capture fence reset failed (index=%u result=%d)", fenceIndex, resetResult);
        return false;
    }

    ScopedBorrowedQueueSubmission captureSubmissionGuard(queue);
    const VkResult submitResult = disp->fp_vkQueueSubmit(queue, 1, &submit, fence);
    if (submitResult != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Capture queue submit failed (index=%u result=%d); Present wait chain unchanged",
                 fenceIndex, submitResult);
        if (submitResult == VK_ERROR_DEVICE_LOST) {
            state.initialized = false;
            return false;
        }

        // Queue submission did not take ownership of the command buffer or
        // signal this fence. Replace the now-unsignaled fence so a transient
        // submit failure cannot permanently suppress all later captures.
        disp->fp_vkDestroyFence(device, fence, nullptr);
        VkFenceCreateInfo recoveryFenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr,
                                               VK_FENCE_CREATE_SIGNALED_BIT};
        VkFence recoveryFence = VK_NULL_HANDLE;
        if (disp->fp_vkCreateFence(device, &recoveryFenceInfo, nullptr, &recoveryFence) == VK_SUCCESS) {
            state.copyFences[fenceIndex] = recoveryFence;
        } else {
            state.copyFences[fenceIndex] = VK_NULL_HANDLE;
            state.initialized = false;
            LayerLog("Vulkan Layer: Capture fence recovery failed; disabling this capture state");
        }
        return false;
    }
    state.sharedImageInitialized[slotIndex] = true;
    if (signaledSemaphore)
        *signaledSemaphore = signalSemaphore;

    // IPC relay: D3D11 Wait/CopyResource/Signal to copy from KMT texture to NT IPC texture
    if (doRelay) {
        // GPU waits for Vulkan copy to complete (shared fence)
        const HRESULT waitHr = state.d3d11Context4->Wait(state.d3d11Fence, vulkanSignalValue);
        if (FAILED(waitHr)) {
            LayerLog("Vulkan Layer: IPC relay Wait failed (value=%llu hr=0x%08X); frame not published",
                     static_cast<unsigned long long>(vulkanSignalValue), static_cast<unsigned>(waitHr));
            return true;  // Vulkan submit consumed Present waits and will signal signalSemaphore.
        }
        // GPU copies from KMT-imported D3D11 texture to NT-shared IPC texture
        state.d3d11Context4->CopyResource(sharedTextures->ipcTextures[slotIndex], sharedTextures->textures[slotIndex]);
        // GPU signals completion for encoder to consume (use cross-process IPC fence if available)
        ID3D11Fence* signalFence = state.d3d11IpcFence ? state.d3d11IpcFence : state.d3d11Fence;
        const HRESULT signalHr = state.d3d11Context4->Signal(signalFence, encoderFenceValue);
        if (FAILED(signalHr)) {
            LayerLog("Vulkan Layer: IPC relay Signal failed (value=%llu hr=0x%08X); frame not published",
                     static_cast<unsigned long long>(encoderFenceValue), static_cast<unsigned>(signalHr));
            // CopyResource was already queued. Without a completion value this
            // slot can never be proven safe for Vulkan to overwrite again.
            state.relayCompletionUnknown = true;
            state.initialized = false;
            return true;  // Keep Present chained to the already-submitted Vulkan capture.
        }
        state.relayCompletionValues[slotIndex] = encoderFenceValue;
        // Flush to submit the D3D11 GPU work immediately
        state.d3d11Context4->Flush();
    }

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    LayerIPC_SignalFrameReady(slotIndex, encoderFenceValue, sourceQpc.QuadPart, metadata);
    return true;
}

// ---- Vulkan Screenshot ----
// Reads pixels from the swapchain image using a staging buffer.
// Uses the Vulkan dispatch table and creates a one-shot command buffer.
bool TakeVulkanScreenshot(DeviceDispatch* disp, VkDevice device, VkQueue queue, VkImage srcImage, uint32_t width,
                          uint32_t height, VkFormat format, VkColorSpaceKHR colorSpace,
                          const VkSemaphore* waitSemaphores,
                          uint32_t waitSemaphoreCount, SharedMemoryLayout* sharedMemory, uint64_t requestId) {
    if (!disp || !device || !srcImage || !sharedMemory || requestId == 0 || width == 0 || height == 0 ||
        width > 16384 || height > 16384) {
        return false;
    }

    uint32_t bytesPerPixel = 0;
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    ScreenshotPixelFormat pixelFormat{};
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    ScreenshotColorEncoding colorEncoding{};
    bool swapPackedRedBlue = false;
    const auto presentationEncoding = ce::presentation_color::ResolveVulkan(format, colorSpace);
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            bytesPerPixel = 4;
            pixelFormat = ScreenshotPixelFormat::BGRA8;
            colorEncoding = ScreenshotColorEncoding::SRGB;
            break;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            bytesPerPixel = 4;
            pixelFormat = ScreenshotPixelFormat::RGBA8;
            colorEncoding = ScreenshotColorEncoding::SRGB;
            break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            bytesPerPixel = 4;
            pixelFormat = ScreenshotPixelFormat::R10G10B10A2;
            colorEncoding = presentationEncoding == ce::presentation_color::Encoding::Hdr10Pq
                                ? ScreenshotColorEncoding::BT2020_PQ
                                : ScreenshotColorEncoding::SRGB;
            break;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            bytesPerPixel = 4;
            pixelFormat = ScreenshotPixelFormat::R10G10B10A2;
            colorEncoding = presentationEncoding == ce::presentation_color::Encoding::Hdr10Pq
                                ? ScreenshotColorEncoding::BT2020_PQ
                                : ScreenshotColorEncoding::SRGB;
            swapPackedRedBlue = true;
            break;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            bytesPerPixel = 8;
            pixelFormat = ScreenshotPixelFormat::RGBA16F;
            colorEncoding = ScreenshotColorEncoding::LinearScRGB;
            break;
        default:
            HookLog("[Screenshot] Vulkan: Unsupported format %u", static_cast<unsigned>(format));
            return false;
    }
    if (presentationEncoding == ce::presentation_color::Encoding::Unsupported) {
        HookLog("[Screenshot] Vulkan: Unsupported presentation contract format=%u colorSpace=%u",
                static_cast<unsigned>(format), static_cast<unsigned>(colorSpace));
        return false;
    }

    const uint32_t queueFamilyIndex = VulkanLayerState::Get().GetQueueFamilyIndex(queue);
    if (queueFamilyIndex == VK_QUEUE_FAMILY_IGNORED) {
        HookLog("[Screenshot] Vulkan: Queue family unknown, skipping screenshot");
        return false;
    }
    if (!VulkanLayerState::Get().QueueSupportsTransfer(queue)) {
        HookLog("[Screenshot] Vulkan: Queue family %u lacks transfer support, skipping screenshot", queueFamilyIndex);
        return false;
    }

    // Calculate row pitch (4 bytes per pixel for BGRA/RGBA)
    uint32_t rowPitch = width * bytesPerPixel;
    VkDeviceSize bufferSize = static_cast<VkDeviceSize>(rowPitch) * height;

    // Find HOST_VISIBLE | HOST_COHERENT memory type
    VkPhysicalDeviceMemoryProperties memProps;
    VkPhysicalDevice physDev = disp->physicalDevice;
    if (physDev == VK_NULL_HANDLE)
        return false;

    auto* instDisp =
        VulkanLayerState::Get().GetInstanceDispatch(VulkanLayerState::Get().GetInstanceFromPhysicalDevice(physDev));
    if (!instDisp || !instDisp->fp_vkGetPhysicalDeviceMemoryProperties)
        return false;
    instDisp->fp_vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);

    // Create staging buffer
    VkBufferCreateInfo bufInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = bufferSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    if (disp->fp_vkCreateBuffer(device, &bufInfo, nullptr, &stagingBuffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReqs;
    disp->fp_vkGetBufferMemoryRequirements(device, stagingBuffer, &memReqs);

    uint32_t memTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1u << i)) != 0 &&
            (memProps.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memTypeIndex = i;
            break;
        }
    }
    if (memTypeIndex == UINT32_MAX) {
        HookLog("[Screenshot] Vulkan: No compatible HOST_VISIBLE memory type found");
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = memTypeIndex;

    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (disp->fp_vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory) != VK_SUCCESS) {
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }
    if (disp->fp_vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0) != VK_SUCCESS) {
        disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    // Create command pool
    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    if (disp->fp_vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool) != VK_SUCCESS) {
        disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    // Allocate command buffer
    VkCommandBufferAllocateInfo cmdAllocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdAllocInfo.commandPool = cmdPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    if (disp->fp_vkAllocateCommandBuffers(device, &cmdAllocInfo, &cmdBuf) != VK_SUCCESS) {
        disp->fp_vkDestroyCommandPool(device, cmdPool, nullptr);
        disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    // Record commands
    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (disp->fp_vkBeginCommandBuffer(cmdBuf, &beginInfo) != VK_SUCCESS) {
        disp->fp_vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
        disp->fp_vkDestroyCommandPool(device, cmdPool, nullptr);
        disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    // Transition image: PRESENT_SRC_KHR -> TRANSFER_SRC_OPTIMAL
    VkImageMemoryBarrier imgBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    imgBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    imgBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    imgBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imgBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imgBarrier.image = srcImage;
    imgBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    disp->fp_vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                  nullptr, 0, nullptr, 1, &imgBarrier);

    // Copy image to buffer
    VkBufferImageCopy copyRegion = {};
    copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.imageExtent = {width, height, 1};
    disp->fp_vkCmdCopyImageToBuffer(cmdBuf, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1,
                                    &copyRegion);

    // Transition image back: TRANSFER_SRC_OPTIMAL -> PRESENT_SRC_KHR
    imgBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    imgBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    imgBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imgBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    disp->fp_vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                  nullptr, 0, nullptr, 1, &imgBarrier);

    if (disp->fp_vkEndCommandBuffer(cmdBuf) != VK_SUCCESS) {
        disp->fp_vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
        disp->fp_vkDestroyCommandPool(device, cmdPool, nullptr);
        disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    // Create fence and submit
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (disp->fp_vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        disp->fp_vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
        disp->fp_vkDestroyCommandPool(device, cmdPool, nullptr);
        disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuf;
    std::vector<VkPipelineStageFlags> waitStages;
    if (waitSemaphores && waitSemaphoreCount > 0) {
        waitStages.assign(waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
        submitInfo.waitSemaphoreCount = waitSemaphoreCount;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages.data();
    }
    ScopedBorrowedQueueSubmission screenshotSubmissionGuard(queue);
    const VkResult submitResult = disp->fp_vkQueueSubmit(queue, 1, &submitInfo, fence);
    if (submitResult != VK_SUCCESS) {
        disp->fp_vkDestroyFence(device, fence, nullptr);
        disp->fp_vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
        disp->fp_vkDestroyCommandPool(device, cmdPool, nullptr);
        disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    // This is a one-shot readback. Waiting for its fence both makes mapped data
    // valid and proves that any Present wait semaphores consumed above are done.
    const VkResult waitResult = disp->fp_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    bool queued = false;
    if (waitResult == VK_SUCCESS) {
        void* mappedData = nullptr;
        if (disp->fp_vkMapMemory(device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mappedData) == VK_SUCCESS && mappedData) {
            if (swapPackedRedBlue) {
                std::vector<uint32_t> converted(static_cast<size_t>(width) * height);
                const auto* sourcePixels = static_cast<const uint32_t*>(mappedData);
                for (size_t i = 0; i < converted.size(); ++i) {
                    const uint32_t value = sourcePixels[i];
                    converted[i] = (value & 0xC00FFC00u) | ((value >> 20) & 0x3FFu) | ((value & 0x3FFu) << 20);
                }
                queued =
                    QueueScreenshotPixels(sharedMemory, requestId, reinterpret_cast<const uint8_t*>(converted.data()),
                                          width, height, rowPitch, pixelFormat, colorEncoding);
            } else {
                queued = QueueScreenshotPixels(sharedMemory, requestId, static_cast<const uint8_t*>(mappedData), width,
                                               height, rowPitch, pixelFormat, colorEncoding);
            }
            disp->fp_vkUnmapMemory(device, stagingMemory);
        }
    } else {
        HookLog("[Screenshot] Vulkan fence wait failed: %d", waitResult);
    }
    if (!queued) {
        CompleteScreenshotRequest(sharedMemory, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);
    }

    // Cleanup
    disp->fp_vkDestroyFence(device, fence, nullptr);
    disp->fp_vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
    disp->fp_vkDestroyCommandPool(device, cmdPool, nullptr);
    disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
    disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
    return true;
}
