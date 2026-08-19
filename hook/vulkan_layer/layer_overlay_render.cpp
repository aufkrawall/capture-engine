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
                   uint32_t waitSemaphoreCount, VkSemaphore signalSemaphore, bool gameSubmitsConcurrently,
                   int32_t* fenceWaitUs, int32_t* overlayGpuUs) {
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
    const OverlaySubmitTarget submitTarget =
        ResolveOverlaySubmitTarget(device, disp, queue, queueFamilyIndex, gameSubmitsConcurrently);
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

    // The fence above already proves the previous submission for this image
    // index retired, so the timestamp pair it wrote is readable without ever
    // blocking. Report it as this frame's overlay GPU cost: it is the same
    // command buffer doing the same work, one trip round the swapchain ago.
    if (state.timestampPool != VK_NULL_HANDLE && imageIndex < state.timestampWritten.size() &&
        state.timestampWritten[imageIndex] && disp->fp_vkGetQueryPoolResults) {
        uint64_t stamps[2] = {0, 0};
        const VkResult queryResult =
            disp->fp_vkGetQueryPoolResults(device, state.timestampPool, imageIndex * 2, 2, sizeof(stamps), stamps,
                                           sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (queryResult == VK_SUCCESS && stamps[1] > stamps[0]) {
            const double elapsedNs = static_cast<double>(stamps[1] - stamps[0]) * state.timestampPeriodNs;
            state.lastOverlayGpuUs = static_cast<int32_t>(elapsedNs / 1000.0);
        }
    }
    if (overlayGpuUs) {
        *overlayGpuUs = state.lastOverlayGpuUs;
    }

    // Record command buffer
    VkCommandBuffer cmd = state.commandBuffers[imageIndex];
    disp->fp_vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (disp->fp_vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
        return false;

    const bool writeTimestamps = state.timestampPool != VK_NULL_HANDLE && disp->fp_vkCmdResetQueryPool &&
                                 disp->fp_vkCmdWriteTimestamp && imageIndex < state.timestampWritten.size();
    if (writeTimestamps) {
        disp->fp_vkCmdResetQueryPool(cmd, state.timestampPool, imageIndex * 2, 2);
        disp->fp_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, state.timestampPool, imageIndex * 2);
    }

    // No explicit layout barriers around the render pass. It declares
    // PRESENT_SRC_KHR as both the initial and the final layout and moves the
    // image to COLOR_ATTACHMENT_OPTIMAL for its subpass, so it performs exactly
    // one transition each way. The pair of barriers that used to sit here
    // performed a third transition and a fourth that was invalid outright: it
    // named COLOR_ATTACHMENT_OPTIMAL as the old layout for an image the render
    // pass had already handed back in PRESENT_SRC_KHR.

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

    // End render pass. Its finalLayout already hands the image back in
    // PRESENT_SRC_KHR and its outgoing subpass dependency already makes the
    // overlay's writes available, so nothing follows it here.
    disp->fp_vkCmdEndRenderPass(cmd);

    if (writeTimestamps) {
        disp->fp_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, state.timestampPool,
                                     imageIndex * 2 + 1);
    }

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
    if (writeTimestamps) {
        state.timestampWritten[imageIndex] = true;
    }
    return true;
}
