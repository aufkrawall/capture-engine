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
namespace {

// One more command buffer, fence and binary semaphore. Everything else the ring
// tracks is plain per-slot bookkeeping, so a failed allocation simply leaves the
// ring at its current size and the caller falls back on backpressure.
bool GrowSubmissionRing(OverlayState& state, DeviceDispatch* disp) {
    if (state.commandPool == VK_NULL_HANDLE || !disp->fp_vkAllocateCommandBuffers || !disp->fp_vkCreateFence ||
        !disp->fp_vkCreateSemaphore) {
        return false;
    }

    VkCommandBufferAllocateInfo cbInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbInfo.commandPool = state.commandPool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (disp->fp_vkAllocateCommandBuffers(state.device, &cbInfo, &command) != VK_SUCCESS)
        return false;

    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence = VK_NULL_HANDLE;
    if (disp->fp_vkCreateFence(state.device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        disp->fp_vkFreeCommandBuffers(state.device, state.commandPool, 1, &command);
        return false;
    }

    VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (disp->fp_vkCreateSemaphore(state.device, &semInfo, nullptr, &semaphore) != VK_SUCCESS) {
        disp->fp_vkDestroyFence(state.device, fence, nullptr);
        disp->fp_vkFreeCommandBuffers(state.device, state.commandPool, 1, &command);
        return false;
    }

    state.commandBuffers.push_back(command);
    state.fences.push_back(fence);
    state.semaphores.push_back(semaphore);
    state.slotImageIndex.push_back(0);
    state.slotAcquireGeneration.push_back(0);
    state.slotEverUsed.push_back(0);
    state.timestampWritten.push_back(false);

    const uint64_t growth = ++state.submissionRingGrowths;
    if (growth <= 8 || (growth & (growth - 1)) == 0) {
        LayerLog(
            "Vulkan Layer: overlay submission ring extended to %zu slots (growth #%llu) so CE never blocks the "
            "game present thread for its own resource recycling",
            state.fences.size(), static_cast<unsigned long long>(growth));
    }
    return true;
}

}  // namespace

bool RenderOverlay(VkDevice device, VkQueue queue, uint32_t imageIndex, const VkSemaphore* waitSemaphores,
                   uint32_t waitSemaphoreCount, VkSemaphore* signalSemaphoreOut, bool gameSubmitsConcurrently,
                   int32_t* fenceWaitUs, int32_t* overlayGpuUs,
                   const std::atomic<uint64_t>* imageAcquireGeneration, uint32_t imageAcquireGenerationCount) {
    if (signalSemaphoreOut)
        *signalSemaphoreOut = VK_NULL_HANDLE;
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

    ce::overlay_submit_queue_policy::ComputePresentAvailability computeAvailability;
    computeAvailability.presentQueueSupportsGraphics = VulkanLayerState::Get().QueueSupportsGraphics(queue);
    computeAvailability.presentQueueSupportsCompute = VulkanLayerState::Get().QueueSupportsCompute(queue);
    computeAvailability.swapchainSupportsStorage = (state.imageUsage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    computeAvailability.storageReadWithoutFormatAvailable =
        disp->storageImageReadWithoutFormatAvailable && state.storageFormatReadWithoutFormatSupported;
    computeAvailability.storageWriteWithoutFormatAvailable =
        disp->storageImageWriteWithoutFormatAvailable && state.storageFormatWriteWithoutFormatSupported;
    const bool useComputePresent =
        ce::overlay_submit_queue_policy::ShouldUseComputePresent(computeAvailability);
    if (!computeAvailability.presentQueueSupportsGraphics && !useComputePresent) {
        static std::atomic<int> s_computeFallbackLogCount{0};
        if (s_computeFallbackLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            LayerLog(
                "Vulkan Layer: Compute-present overlay unavailable - computeQueue=%d storageUsage=%d "
                "readWithoutFormat=%d writeWithoutFormat=%d; retaining graphics fallback",
                computeAvailability.presentQueueSupportsCompute ? 1 : 0,
                computeAvailability.swapchainSupportsStorage ? 1 : 0,
                computeAvailability.storageReadWithoutFormatAvailable ? 1 : 0,
                computeAvailability.storageWriteWithoutFormatAvailable ? 1 : 0);
        }
    }

    // The overlay is normally a render pass. A game is free to present from a
    // queue family without VK_QUEUE_GRAPHICS_BIT, so resolve the graphics work
    // before touching command resources. Compute-present uses an independent
    // offscreen target and therefore prefers CE's reserved queue; the direct
    // fallback still joins the game's graphics timeline when that is safe.
    const OverlaySubmitTarget submitTarget = ResolveOverlaySubmitTarget(
        device, disp, queue, queueFamilyIndex, gameSubmitsConcurrently, useComputePresent);
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
    const uint32_t slotCount = static_cast<uint32_t>(state.fences.size());
    if (!signalSemaphoreOut || imageIndex >= state.framebuffers.size() || imageIndex >= state.swapchainImages.size() ||
        slotCount == 0 || state.commandBuffers.size() != slotCount || state.semaphores.size() != slotCount) {
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
        state.metrics->Update(PerfLogger::GetQpcUs());
        ce::overlay_metrics::PublishDetectedOverlayFGMetrics(state.metrics, "Vulkan::RenderOverlay");
    }

    // Submission resources are a global ring, not properties of a swapchain
    // image. MFG runtimes can present one image repeatedly in a generated-output
    // burst; binding the ring to imageIndex then waits on the same in-flight
    // slot while every other slot is idle. Probe from the round-robin cursor and
    // block only when every slot is truly in flight.
    int64_t fenceStartUs = PerfLogger::GetQpcUs();
    VkResult probeFailure = VK_SUCCESS;
    const auto currentAcquireGeneration = [&](uint32_t slot) -> uint64_t {
        if (!imageAcquireGeneration || slot >= state.slotImageIndex.size())
            return 0;
        const uint32_t slotImage = state.slotImageIndex[slot];
        if (slotImage >= imageAcquireGenerationCount)
            return 0;
        return imageAcquireGeneration[slotImage].load(std::memory_order_acquire);
    };
    // The compute-composite route owns a full-resolution offscreen target per
    // slot, so an appended slot gets none - and needs none: it fails that
    // route's own resource bounds check before either queue sees work, and
    // falls through to the direct graphics path below for that present. Growth
    // therefore costs one command buffer, one fence and one semaphore on both
    // routes, and never a second full-resolution image.
    static_assert(static_cast<int>(ce::overlay_submit_queue_policy::kMaxSubmissionSlots) <=
                      CustomOverlay::VulkanBackend::kFramePoolSize,
                  "the submission ring may not outgrow the overlay backend's per-frame buffer pool");
    const bool ringMayGrow = state.submissionRingMayGrow;
    auto slotChoice = ce::overlay_submit_queue_policy::ChooseSubmissionSlot(
        slotCount, state.nextSubmissionSlot,
        [&](uint32_t candidate) {
            const VkFence candidateFence = state.fences[candidate];
            const VkResult result = disp->fp_vkWaitForFences(device, 1, &candidateFence, VK_TRUE, 0);
            if (result != VK_SUCCESS && result != VK_TIMEOUT)
                probeFailure = result;
            if (result != VK_SUCCESS)
                return false;
            // The fence only clears the command buffer. Without a present fence
            // the acquire generation is the one sound proof that the present
            // which waited on this slot's binary semaphore has executed.
            return ce::overlay_submit_queue_policy::IsSubmissionSlotReusable(
                true, candidate < state.slotEverUsed.size() && state.slotEverUsed[candidate] != 0,
                candidate < state.slotAcquireGeneration.size() ? state.slotAcquireGeneration[candidate] : 0,
                currentAcquireGeneration(candidate));
        },
        ringMayGrow);
    if (!slotChoice.valid || probeFailure != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Submission-slot fence probe FAILED with result %d", probeFailure);
        return false;
    }
    if (slotChoice.growRing) {
        if (GrowSubmissionRing(state, disp)) {
            slotChoice.index = static_cast<uint32_t>(state.fences.size()) - 1;
        } else {
            // Extending failed, so fall back on the old behaviour rather than
            // dropping the overlay for this frame.
            slotChoice.index = state.nextSubmissionSlot % static_cast<uint32_t>(state.fences.size());
            slotChoice.waitForCompletion = true;
        }
        slotChoice.growRing = false;
    }
    const uint32_t submissionSlot = slotChoice.index;
    VkFence fence = state.fences[submissionSlot];
    VkResult fenceResult = VK_SUCCESS;
    if (slotChoice.waitForCompletion) {
        const uint64_t waitNumber = ++state.submissionBackpressureWaits;
        if (waitNumber <= 8 || (waitNumber & (waitNumber - 1)) == 0) {
            // Reaching this now means the ring could not be extended - the
            // memory-safety bound, or a failed allocation - so CE is back to
            // being a pacing authority and the session should say so.
            LayerLog(
                "Vulkan Layer: overlay submission ring could not be extended past %zu slots; blocking the game's "
                "present thread on slot %u (present image=%u wait=%llu, growths=%llu)",
                state.fences.size(), submissionSlot, imageIndex, static_cast<unsigned long long>(waitNumber),
                static_cast<unsigned long long>(state.submissionRingGrowths));
        }
        fenceResult = disp->fp_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    int64_t fenceEndUs = PerfLogger::GetQpcUs();
    if (fenceWaitUs) {
        *fenceWaitUs = static_cast<int32_t>(fenceEndUs - fenceStartUs);
    }

    if (fenceResult != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Submission-slot fence wait FAILED with result %d (slot %u, image %u)", fenceResult,
                 submissionSlot, imageIndex);
        return false;
    }
    const uint32_t currentSlotCount = static_cast<uint32_t>(state.fences.size());
    state.nextSubmissionSlot = (submissionSlot + 1) % currentSlotCount;
    if (submissionSlot < state.slotImageIndex.size()) {
        state.slotImageIndex[submissionSlot] = imageIndex;
        state.slotAcquireGeneration[submissionSlot] =
            (imageAcquireGeneration && imageIndex < imageAcquireGenerationCount)
                ? imageAcquireGeneration[imageIndex].load(std::memory_order_acquire)
                : 0;
        state.slotEverUsed[submissionSlot] = 1;
    }
    const VkSemaphore signalSemaphore = state.semaphores[submissionSlot];

    // The fence above proves this slot's previous submission retired, so its
    // timestamp pair is readable without another wait.
    if (state.timestampPool != VK_NULL_HANDLE && submissionSlot < state.timestampSlotCapacity &&
        state.timestampWritten[submissionSlot] && disp->fp_vkGetQueryPoolResults) {
        uint64_t stamps[2] = {0, 0};
        const VkResult queryResult =
            disp->fp_vkGetQueryPoolResults(device, state.timestampPool, submissionSlot * 2, 2, sizeof(stamps), stamps,
                                           sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (queryResult == VK_SUCCESS && stamps[1] > stamps[0]) {
            const double elapsedNs = static_cast<double>(stamps[1] - stamps[0]) * state.timestampPeriodNs;
            state.lastOverlayGpuUs = static_cast<int32_t>(elapsedNs / 1000.0);
        }
    }
    if (overlayGpuUs) {
        *overlayGpuUs = state.lastOverlayGpuUs;
    }

    if (useComputePresent) {
        bool routeAttempted = false;
        const bool rendered =
            RenderComputePresentOverlay(state, disp, submitTarget, queue, submissionSlot, imageIndex, waitSemaphores,
                                        waitSemaphoreCount, signalSemaphore, &routeAttempted);
        if (routeAttempted) {
            if (rendered)
                *signalSemaphoreOut = signalSemaphore;
            return rendered;
        }
        // Resource creation/recording failed before either queue saw work. The
        // direct graphics path below remains a safe per-frame fallback.
    }

    // Record command buffer
    VkCommandBuffer cmd = state.commandBuffers[submissionSlot];
    disp->fp_vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (disp->fp_vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
        return false;

    const bool writeTimestamps = state.timestampPool != VK_NULL_HANDLE && disp->fp_vkCmdResetQueryPool &&
                                 disp->fp_vkCmdWriteTimestamp && submissionSlot < state.timestampSlotCapacity;
    if (writeTimestamps) {
        disp->fp_vkCmdResetQueryPool(cmd, state.timestampPool, submissionSlot * 2, 2);
        disp->fp_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, state.timestampPool,
                                     submissionSlot * 2);
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
                                     submissionSlot * 2 + 1);
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
    if (disp->fp_vkResetFences(device, 1, &fence) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Failed to reset overlay submission fence (slot %u, image %u)", submissionSlot,
                 imageIndex);
        return false;
    }
    VkResult submitResult = VK_SUCCESS;
    {
        ScopedBorrowedQueueSubmission submissionGuard(submitQueue);
        submitResult = disp->fp_vkQueueSubmit(submitQueue, 1, &submitInfo, fence);
    }
    if (submitResult != VK_SUCCESS) {
        LayerLog("Vulkan Layer: QueueSubmit FAILED with result %d (slot %u, image %u)", submitResult,
                 submissionSlot, imageIndex);
        return false;
    }
    if (writeTimestamps) {
        state.timestampWritten[submissionSlot] = true;
    }
    *signalSemaphoreOut = signalSemaphore;
    return true;
}
