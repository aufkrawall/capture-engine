/**
 * VK_LAYER_CE_overlay - per-present overlay recording and submission
 *
 * Split out of layer_overlay.cpp to keep both units under the source-size
 * ceiling. Initialization, teardown and the shared overlay state live there;
 * this unit owns the work done inside vkQueuePresentKHR.
 */

#include "layer_overlay_internal.h"

// Render overlay using OverlayAdapter
// fenceWaitUs returns the time spent waiting for fence (previous frame sync)
bool RenderOverlay(VkDevice device, VkQueue queue, uint32_t imageIndex, const VkSemaphore* waitSemaphores,
                   uint32_t waitSemaphoreCount, VkSemaphore signalSemaphore, int32_t* fenceWaitUs) {
    // Early out if overlay is disabled (use seqlock for consistent read)
    if (g_IPCClient.GetSharedMem() && !g_IPCClient.GetSharedMem()->ReadOverlayConfig().showOverlay) {
        return false;
    }

    // PERFORMANCE: Use try_lock to avoid blocking the render thread
    if (!g_OverlayMutex.try_lock()) {
        static std::atomic<int> s_skipCount{0};
        int skips = ++s_skipCount;
        if (skips <= 5) {
            LayerLog("Vulkan Layer: Overlay mutex busy, skipping frame (imageIndex=%u)", imageIndex);
        }
        return false;
    }
    // RAII unlock when we exit
    std::lock_guard<std::mutex> lock(g_OverlayMutex, std::adopt_lock);
    auto it = g_OverlayStates.find(device);
    if (it == g_OverlayStates.end() || !it->second.initialized)
        return false;

    OverlayState& state = it->second;
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp)
        return false;

    const uint32_t queueFamilyIndex = VulkanLayerState::Get().GetQueueFamilyIndex(queue);
    if (queueFamilyIndex == VK_QUEUE_FAMILY_IGNORED) {
        static std::atomic<int> s_unknownQueueLogCount{0};
        if (s_unknownQueueLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            LayerLog("Vulkan Layer: Overlay skipped because present queue family is unknown");
        }
        return false;
    }

    // The overlay is a render pass. A game is free to present from a queue
    // family without VK_QUEUE_GRAPHICS_BIT - DOOM Eternal presents from a
    // compute-only family once its real render loop starts - so resolve which
    // graphics queue this submit belongs on before touching any resources.
    const OverlaySubmitTarget submitTarget = ResolveOverlaySubmitTarget(device, disp, queue, queueFamilyIndex);
    if (!submitTarget.valid) {
        return false;
    }
    const VkQueue submitQueue = submitTarget.queue;
    const uint32_t submitQueueFamily = submitTarget.queueFamilyIndex;
    const bool borrowedSubmitQueue = submitTarget.borrowed;

    if ((state.commandPool == VK_NULL_HANDLE || state.queueFamilyIndex != submitQueueFamily) &&
        !RecreateOverlayCommandResources(state, disp, submitQueueFamily)) {
        return false;
    }

    // Deferred window hook
    if (state.needsWindowHook) {
        HWND hwnd = GetForegroundWindow();
        if (hwnd) {
            DWORD foregroundPid = 0;
            GetWindowThreadProcessId(hwnd, &foregroundPid);
            if (foregroundPid == GetCurrentProcessId()) {
                InputManager::Get().HookWindow(hwnd);
                state.needsWindowHook = false;
                LayerLog("Vulkan Layer: Deferred window hook successful (hwnd=%p)", hwnd);
            }
        }
    }

    // Update metrics
    if (state.metrics) {
        state.metrics->Update(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
        ce::overlay_metrics::PublishDetectedOverlayFGMetrics(state.metrics, "Vulkan::RenderOverlay");
    }

    // FENCE WAIT: Wait for GPU to be ready for this buffer
    // Unlike DX12's value-based fences, Vulkan binary semaphores cannot be reused
    // until consumed. We MUST wait for the previous overlay work to complete
    // before we can safely signal the semaphore again.
    //
    // This is the only reliable way to prevent flickering with binary semaphores.
    // The wait time is typically minimal since we use triple buffering.
    VkFence fence = state.fences[imageIndex];
    int64_t fenceStartUs = PerfLogger::GetQpcUs();

    // Wait for GPU with a reasonable timeout (16ms = ~1 frame at 60fps)
    // This ensures overlay appears every frame (no flickering)
    constexpr uint64_t FENCE_TIMEOUT_NS = 16000000;  // 16ms
    VkResult fenceResult = disp->fp_vkWaitForFences(device, 1, &fence, VK_TRUE, FENCE_TIMEOUT_NS);

    int64_t fenceEndUs = PerfLogger::GetQpcUs();
    if (fenceWaitUs) {
        *fenceWaitUs = static_cast<int32_t>(fenceEndUs - fenceStartUs);
    }

    if (fenceResult == VK_TIMEOUT) {
        // Skip this overlay frame instead of force-resetting/reusing an in-flight
        // command buffer, which can cause transient glyph artifacts.
        static std::atomic<int> s_timeoutCount{0};
        int timeouts = ++s_timeoutCount;
        if (timeouts <= 5) {
            LayerLog("Vulkan Layer: Fence wait timeout after %d us (buffer %u, timeout=%d)",
                     static_cast<int>(fenceEndUs - fenceStartUs), imageIndex, timeouts);
        }
        return false;
    } else if (fenceResult != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Fence wait FAILED with result %d (buffer %u)", fenceResult, imageIndex);
        return false;
    } else {
        // Success - reset fence for this frame's work
        disp->fp_vkResetFences(device, 1, &fence);
    }

    // Record command buffer
    VkCommandBuffer cmd = state.commandBuffers[imageIndex];
    disp->fp_vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (disp->fp_vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
        return false;

    // CRITICAL: Transition image preserving existing content
    // The game has already rendered to this image, so we must preserve it.
    // Use PRESENT_SRC_KHR as oldLayout since that's what Present expects.
    // The render pass will load existing content (VK_ATTACHMENT_LOAD_OP_LOAD).
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;  // Previous read for Present
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = state.swapchainImages[imageIndex];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                  &barrier);

    // Begin render pass
    VkRenderPassBeginInfo rpBeginInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpBeginInfo.renderPass = state.renderPass;
    rpBeginInfo.framebuffer = state.framebuffers[imageIndex];
    rpBeginInfo.renderArea.extent = state.extent;
    disp->fp_vkCmdBeginRenderPass(cmd, &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Set render context for VulkanBackend and render via OverlayAdapter
    if (state.overlayAdapter && state.overlayAdapter->IsInitialized()) {
        // Verify backend type before casting
        if (state.overlayAdapter->GetBackendType() != OverlayBackendType::Vulkan) {
            LayerLog("Vulkan Layer: [Error] Backend type mismatch in RenderOverlay");
        } else {
            auto* vkBackend = static_cast<CustomOverlay::VulkanBackend*>(state.overlayAdapter->GetBackend());
            if (vkBackend) {
                vkBackend->SetRenderContext(cmd, state.renderPass, state.framebuffers[imageIndex], state.extent);
                // RenderOverlay will check if pipeline is ready
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                state.overlayAdapter->RenderOverlay(state.extent.width, state.extent.height);
            } else {
                LayerLog("Vulkan Layer: [Error] Vulkan backend is null");
            }
        }
    } else {
        LayerLog("Vulkan Layer: [Warning] OverlayAdapter not ready, skipping render");
    }

    // End render pass
    disp->fp_vkCmdEndRenderPass(cmd);

    // Transition to present
    VkImageMemoryBarrier presentBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    presentBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    presentBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    presentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    presentBarrier.image = state.swapchainImages[imageIndex];
    presentBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &presentBarrier);

    disp->fp_vkEndCommandBuffer(cmd);

    // Submit
    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    std::vector<VkPipelineStageFlags> waitStages;
    if (waitSemaphores && waitSemaphoreCount > 0) {
        waitStages.assign(waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        submitInfo.waitSemaphoreCount = waitSemaphoreCount;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages.data();
    }

    if (signalSemaphore != VK_NULL_HANDLE) {
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signalSemaphore;
    }

    // A borrowed queue is the game's, and VkQueue is externally synchronized:
    // hold the same lock the layer's own vkQueueSubmit wrappers take for it.
    (void)borrowedSubmitQueue;
    VkResult submitResult = VK_SUCCESS;
    {
        ScopedBorrowedQueueSubmission submissionGuard(submitQueue);
        submitResult = disp->fp_vkQueueSubmit(submitQueue, 1, &submitInfo, fence);
    }
    if (submitResult != VK_SUCCESS) {
        LayerLog("Vulkan Layer: QueueSubmit FAILED with result %d (buffer %u)", submitResult, imageIndex);
        return false;
    }
    return true;
}
