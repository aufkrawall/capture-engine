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

// Releases the slot GrowSubmissionRing just appended, so a growth that could
// not be completed leaves the ring exactly as it was.
void PopSubmissionRingSlot(OverlayState& state, DeviceDispatch* disp) {
    if (state.fences.empty())
        return;
    disp->fp_vkDestroySemaphore(state.device, state.semaphores.back(), nullptr);
    disp->fp_vkDestroyFence(state.device, state.fences.back(), nullptr);
    disp->fp_vkFreeCommandBuffers(state.device, state.commandPool, 1, &state.commandBuffers.back());
    state.timestampWritten.pop_back();
    state.slotEverUsed.pop_back();
    state.slotAcquireGeneration.pop_back();
    state.slotImageIndex.pop_back();
    state.semaphores.pop_back();
    state.fences.pop_back();
    state.commandBuffers.pop_back();
}

// One more command buffer, fence and binary semaphore - plus, while the
// compute-composite route is live, that route's own per-slot resources. A slot
// without them would silently fall back to the direct render-pass route for its
// presents, so the two routes would alternate from present to present on a
// compute-only present queue. Portal RTX session 20260831_054801 is that state
// measured: 6 images, 10 slots created, the ring extended to 12 under 4x
// multi-frame generation, and the compute route's own diagnostics counting only
// 83-90% of the presents the perf CSV recorded. Growth is therefore
// all-or-nothing; a failed allocation leaves the ring at its current size and
// the caller falls back on bounded backpressure.
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

    if (state.computePresentInitialized && !AppendComputePresentSlot(state, disp)) {
        LayerLog(
            "Vulkan Layer: overlay submission ring could not be extended past %zu slots because the "
            "compute-composite route has no resources left for another one; keeping every present on one route",
            state.fences.size() - 1);
        PopSubmissionRingSlot(state, disp);
        return false;
    }

    const uint64_t growth = ++state.submissionRingGrowths;
    if (growth <= 8 || (growth & (growth - 1)) == 0) {
        LayerLog(
            "Vulkan Layer: overlay submission ring extended to %zu slots (growth #%llu) so CE never blocks the "
            "game present thread for its own resource recycling",
            state.fences.size(), static_cast<unsigned long long>(growth));
    }
    return true;
}

void RecordOverlayComposite(OverlayState& state, uint32_t imageIndex, uint64_t acquireGeneration) {
    if (imageIndex < state.imageCompositeAcquireGeneration.size())
        state.imageCompositeAcquireGeneration[imageIndex] = acquireGeneration;
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
    if (state.deviceLost)
        return false;
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

    // Compositing the overlay is a blend, so doing it twice into one image is
    // not idempotent: the panel's own alpha is blended onto itself and that
    // present shows a more opaque overlay than its neighbours. A
    // generated-output runtime may present one swapchain image several times
    // without the application re-acquiring it, and an application may not alter
    // a presented image before it re-acquires it - so an acquire generation
    // unchanged since CE's last composite into this image proves both that the
    // image content is the same and that CE's overlay is still in it.
    // Zero means CE has seen no acquire for this image at all (the counter is
    // maintained by both acquire entry points), and the guard stays out of the
    // way rather than guessing.
    const uint64_t acquireGeneration = (imageAcquireGeneration && imageIndex < imageAcquireGenerationCount)
                                           ? imageAcquireGeneration[imageIndex].load(std::memory_order_acquire)
                                           : 0;
    if (imageIndex < state.imageCompositeAcquireGeneration.size() &&
        ce::overlay_submit_queue_policy::ShouldSkipRepeatPresentComposite(
            acquireGeneration, state.imageCompositeAcquireGeneration[imageIndex])) {
        const uint64_t repeats = ++state.repeatPresentComposites;
        if (repeats <= 8 || (repeats & (repeats - 1)) == 0) {
            LayerLog(
                "Vulkan Layer: image %u presented again at acquire generation %llu; the overlay CE already "
                "composited into it is still there and a second blend would darken it (repeat #%llu)",
                imageIndex, static_cast<unsigned long long>(acquireGeneration),
                static_cast<unsigned long long>(repeats));
        }
        return false;
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
    // slot, so growth extends that route too and costs one of those images on
    // top of the command buffer, fence and semaphore. It has to: a slot without
    // one would fall through to the direct graphics path for its presents, and
    // two composite routes alternating on a compute-only present queue is a
    // per-present difference on screen. When the extension cannot be made,
    // GrowSubmissionRing declines the growth and the ring falls back on its
    // bounded backpressure instead.
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
        if (probeFailure == VK_ERROR_DEVICE_LOST) {
            state.deviceLost = true;
            LayerLog("Vulkan Layer: device loss latched from submission-slot fence probe; "
                     "all further overlay GPU work is disabled for this device");
        } else {
            LayerLog("Vulkan Layer: Submission-slot fence probe FAILED with result %d", probeFailure);
        }
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
        if (fenceResult == VK_ERROR_DEVICE_LOST)
            state.deviceLost = true;
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
            if (rendered) {
                *signalSemaphoreOut = signalSemaphore;
                RecordOverlayComposite(state, imageIndex, acquireGeneration);
            }
            return rendered;
        }
        static std::atomic<uint64_t> s_directFallbacks{0};
        const uint64_t fallbacks = s_directFallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fallbacks <= 8 || (fallbacks & (fallbacks - 1)) == 0) {
            // Every ring slot owns a complete compute route, so this is a
            // resource failure rather than the routine shape it used to be.
            LayerLog(
                "Vulkan Layer: compute-composite route unavailable for slot %u image %u; this present falls back "
                "to the direct render pass (occurrence #%llu)",
                submissionSlot, imageIndex, static_cast<unsigned long long>(fallbacks));
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
    const VkResult resetResult = disp->fp_vkResetFences(device, 1, &fence);
    if (resetResult != VK_SUCCESS) {
        if (resetResult == VK_ERROR_DEVICE_LOST)
            state.deviceLost = true;
        LayerLog("Vulkan Layer: Failed to reset overlay submission fence (slot %u, image %u, result=%d)",
                 submissionSlot, imageIndex, resetResult);
        return false;
    }
    VkResult submitResult = VK_SUCCESS;
    {
        ScopedBorrowedQueueSubmission submissionGuard(submitQueue);
        submitResult = disp->fp_vkQueueSubmit(submitQueue, 1, &submitInfo, fence);
    }
    if (submitResult != VK_SUCCESS) {
        if (submitResult == VK_ERROR_DEVICE_LOST)
            state.deviceLost = true;
        LayerLog("Vulkan Layer: QueueSubmit FAILED with result %d (slot %u, image %u)", submitResult,
                 submissionSlot, imageIndex);
        return false;
    }
    if (writeTimestamps) {
        state.timestampWritten[submissionSlot] = true;
    }
    *signalSemaphoreOut = signalSemaphore;
    RecordOverlayComposite(state, imageIndex, acquireGeneration);
    return true;
}
