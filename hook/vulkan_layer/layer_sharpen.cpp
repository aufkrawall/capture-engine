#include "layer_sharpen_state.h"

#include "../common/sharpen_constants.h"
#include "../common/sharpen_gpu_timeline.h"
#include "vulkan_formatless_storage.h"
#include "vulkan_presentation_color.h"

// Recording half of the Vulkan sharpen pass: what happens once per present.

namespace {

ce::sharpen::TargetEncoding ResolveEncoding(VkFormat format, VkColorSpaceKHR colorSpace) {
    switch (ce::presentation_color::ResolveVulkan(format, colorSpace)) {
        case ce::presentation_color::Encoding::Sdr709:
            // A Vulkan _SRGB format decodes on load and re-encodes on store, so
            // the filter would see linear light; the non-sRGB formats reach the
            // shader as the stored perceptual values.
            return (format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB ||
                    format == VK_FORMAT_A8B8G8R8_SRGB_PACK32)
                       ? ce::sharpen::TargetEncoding::Srgb
                       : ce::sharpen::TargetEncoding::Unorm;
        case ce::presentation_color::Encoding::LinearScRgb:
            return ce::sharpen::TargetEncoding::ScrgbLinear;
        case ce::presentation_color::Encoding::Hdr10Pq:
            return ce::sharpen::TargetEncoding::Pq;
        case ce::presentation_color::Encoding::Unsupported:
        default:
            return ce::sharpen::TargetEncoding::Unknown;
    }
}

// True when the image view applies the sRGB transfer function on every load and
// store, which puts linear light in front of the kernel.
bool FormatViewIsSrgb(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
            return true;
        default:
            return false;
    }
}

VkImageMemoryBarrier MakeImageBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                      VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}

// Whether a shader may write `format` through a formatless storage image. Asked
// only for a compute-only present queue, and once per state lifetime.
bool FormatIsStorageWritable(SharpenState& state, DeviceDispatch* disp, VkFormat format) {
    if (state.storageQueryFormat != format) {
        InstanceDispatch* inst = VulkanLayerState::Get().GetInstanceDispatch(
            VulkanLayerState::Get().GetInstanceFromPhysicalDevice(disp->physicalDevice));
        state.storageWritable = ce::vulkan_formatless_storage::Query(inst, disp->physicalDevice, format,
                                                                     disp->formatFeatureFlags2Available)
                                    .write;
        state.storageQueryFormat = format;
    }
    return state.storageWritable;
}

// A slot whose previous submission has retired, or -1 when all are still in
// flight. Never blocks: a wait here would be a stall inside the game's present.
int AcquireSlot(SharpenState& state, DeviceDispatch* disp) {
    for (uint32_t attempt = 0; attempt < kSharpenSlotCount; ++attempt) {
        const uint32_t slot = (state.nextSlot + attempt) % kSharpenSlotCount;
        if (!state.slotSubmitted[slot]) {
            state.nextSlot = (slot + 1) % kSharpenSlotCount;
            return static_cast<int>(slot);
        }
        if (disp->fp_vkWaitForFences(state.device, 1, &state.fences[slot], VK_TRUE, 0) == VK_SUCCESS) {
            state.nextSlot = (slot + 1) % kSharpenSlotCount;
            return static_cast<int>(slot);
        }
    }
    return -1;
}

// Destroys the retired states on `device` whose submissions have all signalled.
// Caller holds layer_sharpen_g_StateMutex. Never blocks: a state still in
// flight stays retired until a later present finds it done.
void ReapRetiredSharpenStatesLocked(VkDevice device, DeviceDispatch* disp) {
    if (layer_sharpen_g_Registry.RetiredCount(device) == 0)
        return;
    std::vector<SharpenState> ready = layer_sharpen_g_Registry.TakeReadyRetired(
        device, [disp](const SharpenState& retired) { return SharpenStateSubmissionsRetired(retired, disp); });
    for (SharpenState& retired : ready) {
        LayerLog("Vulkan Layer: Released retired sharpen state of swapchain %p (%ux%u route=%s)", retired.swapchain,
                 retired.extent.width, retired.extent.height, ce::vulkan_sharpen_route::RouteName(retired.route));
        DestroySharpenState(retired, disp);
    }
}

bool SubmitSharpenPass(SharpenState& state, DeviceDispatch* disp, VkQueue queue, VkImage image, uint32_t imageIndex,
                       VkExtent2D extent, const ce::sharpen::Request& request, const ce::sharpen::Decision& decision,
                       const VkSemaphore* waitSemaphores, uint32_t waitSemaphoreCount,
                       VkSemaphore* signaledSemaphore);

}  // namespace

bool SharpenPresentedFrame(VkDevice device, VkSwapchainKHR swapchain, VkQueue queue, VkImage image,
                           uint32_t imageIndex, VkFormat format, VkColorSpaceKHR colorSpace, VkExtent2D extent,
                           uint32_t imageCount, const VkImage* images, const VkSemaphore* waitSemaphores,
                           uint32_t waitSemaphoreCount, VkSemaphore* signaledSemaphore) {
    if (signaledSemaphore)
        *signaledSemaphore = VK_NULL_HANDLE;
    if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE || image == VK_NULL_HANDLE)
        return false;

    // The host rewrites graphicsConfig in place on a live reload and the D3D
    // hooks read it on every call; this pass has to follow the same live copy.
    if (const SharedMemoryLayout* shared = g_IPCClient.GetSharedMem()) {
        const auto& cfg = shared->graphicsConfig;
        VulkanLayerState::Get().StoreSharpenSettings(cfg.sharpenMode, cfg.sharpenColorSpace, cfg.sharpenStrength,
                                                     cfg.sharpenIntensity);
    }
    const ce::sharpen::Request request = VulkanLayerState::Get().GetSharpenRequest();

    std::unique_lock<std::mutex> lock(layer_sharpen_g_StateMutex, std::try_to_lock);
    if (!lock.owns_lock())
        return false;

    // The dispatch table first: Live() would otherwise insert a state entry
    // for a device this function is about to refuse, and nothing ever erases it -
    // CleanupSharpen for that device has already run by the time its dispatch
    // table is gone.
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp)
        return false;

    // States retired earlier (filter switched off, or rebuilt) are destroyed
    // once their own submissions have signalled - never waited for here.
    ReapRetiredSharpenStatesLocked(device, disp);

    if (request.mode == ce::sharpen::Mode::Off) {
        // Switching the feature off hands its resources back rather than
        // leaving a full-frame image and a swapchain's worth of views resident.
        // Retired, not destroyed: the last frames' submissions may still read
        // them, and waiting for those here would stall the game's present.
        const size_t live = layer_sharpen_g_Registry.LiveCount(device);
        if (live > 0) {
            layer_sharpen_g_Registry.RetireAll(device);
            LayerLog("Vulkan Layer: Sharpen disabled - %zu state(s) retired, released once their submissions finish",
                     live);
        }
        return false;
    }

    // Each swapchain owns its own state: a second live swapchain on the same
    // device is filtered too, and its presents never touch the first one's.
    SharpenState& state = layer_sharpen_g_Registry.Live(device, SharpenSwapchainKey(swapchain));

    ce::sharpen::Target target;
    // The Vulkan layer only ever sees the application's own swapchain; a present
    // interposer's private chain is a DXGI concept and never reaches here.
    target.route = ce::sharpen::Route::NormalBackbuffer;
    target.encoding = ResolveEncoding(format, colorSpace);
    target.width = extent.width;
    target.height = extent.height;
    target.viewAppliesSrgbConversion = FormatViewIsSrgb(format);
    // The copy is only legal when the swapchain was created with TRANSFER_SRC,
    // which vulkan_swapchain_usage_policy.h negotiates at creation.
    const SwapchainData* swapchainData = VulkanLayerState::Get().GetSwapchainData(swapchain);
    target.readable =
        swapchainData != nullptr && (swapchainData->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    const uint32_t imageUsage = swapchainData != nullptr ? swapchainData->imageUsage : 0u;

    // The pass runs on the present queue, which need not support graphics.
    const uint32_t queueFamily = VulkanLayerState::Get().GetQueueFamilyIndex(queue);
    ce::vulkan_sharpen_route::Input routeInput;
    routeInput.queueFamilyKnown = queueFamily != VK_QUEUE_FAMILY_IGNORED;
    routeInput.queueSupportsGraphics =
        routeInput.queueFamilyKnown && VulkanLayerState::Get().QueueSupportsGraphics(queue);
    routeInput.queueSupportsCompute =
        routeInput.queueFamilyKnown && VulkanLayerState::Get().QueueSupportsCompute(queue);
    routeInput.swapchainHasStorageUsage = (imageUsage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    if (!routeInput.queueSupportsGraphics && routeInput.queueSupportsCompute && routeInput.swapchainHasStorageUsage) {
        routeInput.storageWriteWithoutFormat =
            disp->storageImageWriteWithoutFormatAvailable && FormatIsStorageWritable(state, disp, format);
    }
    const ce::vulkan_sharpen_route::Route route = ce::vulkan_sharpen_route::Choose(routeInput);
    // The compute route's storage requirement is already part of the route
    // decision; the render-pass route writes through a colour attachment.
    target.writable = route == ce::vulkan_sharpen_route::Route::kGraphics
                          ? (imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0
                          : true;

    const ce::sharpen::Decision decision = ce::sharpen::Decide(request, target);
    // The overlay's runtime-eligibility floor, not just Decide's 32 px
    // minimum: a tiny auxiliary swapchain must not build the full pipeline.
    const bool belowFloor =
        decision.run && !ce::vulkan_sharpen_route::MeetsMinimumTargetSize(target.width, target.height);
    const bool routeRefused = decision.run && !belowFloor && route == ce::vulkan_sharpen_route::Route::kNone;
    const bool run = decision.run && !routeRefused && !belowFloor;
    const char* reason = belowFloor ? "target_below_320x180_floor"
                         : routeRefused ? ce::vulkan_sharpen_route::RefusalReason(routeInput)
                                        : decision.reason;
    if (state.logGate.ShouldLog(run, reason)) {
        LayerLog("Vulkan Layer: Sharpen %s reason=%s %ux%u fmt=%d srgbView=%d usage=0x%x param=%.3f route=%s "
                 "family=%u",
                 run ? "running" : "idle", reason, target.width, target.height, static_cast<int>(format),
                 target.viewAppliesSrgbConversion ? 1 : 0, imageUsage, static_cast<double>(decision.effectParameter),
                 ce::vulkan_sharpen_route::RouteName(route), queueFamily);
    }
    if (!run)
        return false;

    // A swapchain generation change invalidates every view, framebuffer and
    // semaphore built over the old presentable images, and a present-queue
    // family change invalidates the command pool and possibly the route.
    ce::vulkan_sharpen_route::Identity current;
    current.swapchain = SharpenSwapchainKey(swapchain);
    current.format = static_cast<int32_t>(format);
    current.width = extent.width;
    current.height = extent.height;
    current.imageCount = imageCount;
    current.queueFamily = queueFamily;
    current.route = route;
    SharpenState* active = &state;
    if (state.initialized && ce::vulkan_sharpen_route::MustRebuild(SharpenStateIdentity(state), current)) {
        LayerLog("Vulkan Layer: Sharpen rebuilding (swapchain %p family %u -> %u route %s -> %s) - the old state "
                 "is retired until its submissions finish",
                 swapchain, state.queueFamily, queueFamily, ce::vulkan_sharpen_route::RouteName(state.route),
                 ce::vulkan_sharpen_route::RouteName(route));
        // `state` is moved out by the retirement; continue on the fresh one.
        layer_sharpen_g_Registry.Retire(device, SharpenSwapchainKey(swapchain));
        active = &layer_sharpen_g_Registry.Live(device, SharpenSwapchainKey(swapchain));
    }
    if (!active->initialized) {
        if (!InitializeSharpenState(*active, disp, device, swapchain, format, extent, queueFamily, route, imageCount,
                                    images)) {
            return false;
        }
    }
    return SubmitSharpenPass(*active, disp, queue, image, imageIndex, extent, request, decision, waitSemaphores,
                             waitSemaphoreCount, signaledSemaphore);
}

namespace {

bool SubmitSharpenPass(SharpenState& state, DeviceDispatch* disp, VkQueue queue, VkImage image, uint32_t imageIndex,
                       VkExtent2D extent, const ce::sharpen::Request& request, const ce::sharpen::Decision& decision,
                       const VkSemaphore* waitSemaphores, uint32_t waitSemaphoreCount,
                       VkSemaphore* signaledSemaphore) {
    if (imageIndex >= state.imageViews.size() || imageIndex >= state.imageSemaphores.size())
        return false;

    const int slot = AcquireSlot(state, disp);
    if (slot < 0) {
        static std::atomic<int> s_busyLogCount{0};
        const int logCount = s_busyLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 600) == 0) {
            LayerLog("Vulkan Layer: Sharpen skipped a frame - every command buffer is still in flight (#%d)",
                     logCount + 1);
        }
        return false;
    }

    VkCommandBuffer cmd = state.commandBuffers[static_cast<uint32_t>(slot)];
    if (disp->fp_vkResetCommandBuffer(cmd, 0) != VK_SUCCESS)
        return false;
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (disp->fp_vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
        return false;

    const ce::sharpen::ShaderConstants constants =
        ce::sharpen::BuildShaderConstants(request.mode, decision, extent.width, extent.height);
    const VkPipeline pipeline = request.mode == ce::sharpen::Mode::Cas ? state.casPipeline : state.rcasPipeline;
    if (state.route == ce::vulkan_sharpen_route::Route::kCompute) {
        RecordSharpenCompute(state, disp, cmd, image, imageIndex, pipeline, constants);
    } else {
        // The frame becomes a transfer source, and the copy target waits for the
        // previous frame's fragment-shader read of the same image. Both barriers
        // are submission-ordered against everything already on this queue, which is
        // what makes one source image enough for the whole ring.
        const VkImageMemoryBarrier preCopy[2] = {
            MakeImageBarrier(image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
            MakeImageBarrier(state.sourceImage,
                             state.sourceInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                     : VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT),
        };
        disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                      nullptr, 0, nullptr, 2, preCopy);

        VkImageCopy region = {};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {extent.width, extent.height, 1};
        disp->fp_vkCmdCopyImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state.sourceImage,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        // `sourceInitialized` says the image is really in SHADER_READ_ONLY_OPTIMAL,
        // which only the executed command buffer can make true. Recording is not
        // executing: it is set after the submit succeeds, further down.

        const VkImageMemoryBarrier preDraw[2] = {
            MakeImageBarrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT),
            MakeImageBarrier(state.sourceImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_ACCESS_SHADER_READ_BIT),
        };
        disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      0, 0, nullptr, 0, nullptr, 2, preDraw);

        VkRenderPassBeginInfo passInfo = {};
        passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        passInfo.renderPass = state.renderPass;
        passInfo.framebuffer = state.framebuffers[imageIndex];
        passInfo.renderArea = {{0, 0}, extent};
        disp->fp_vkCmdBeginRenderPass(cmd, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
        disp->fp_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        disp->fp_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipelineLayout, 0, 1,
                                         &state.descriptorSet, 0, nullptr);
        disp->fp_vkCmdPushConstants(cmd, state.pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants),
                                    &constants);
        disp->fp_vkCmdDraw(cmd, 3, 1, 0, 0);
        disp->fp_vkCmdEndRenderPass(cmd);
    }

    if (disp->fp_vkEndCommandBuffer(cmd) != VK_SUCCESS)
        return false;

    // Every wait stage is ALL_COMMANDS because the incoming semaphores are the
    // game's own rendering completion, whose stages CE does not know.
    std::vector<VkPipelineStageFlags> waitStages(waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    const VkSemaphore signalSemaphore = state.imageSemaphores[imageIndex];
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = waitSemaphoreCount;
    submit.pWaitSemaphores = waitSemaphoreCount > 0 ? waitSemaphores : nullptr;
    submit.pWaitDstStageMask = waitSemaphoreCount > 0 ? waitStages.data() : nullptr;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &signalSemaphore;

    const uint32_t slotIndex = static_cast<uint32_t>(slot);
    // Everything from here is paired: resetting the fence is what makes the slot
    // look busy, so any exit after it must say what really happened to the
    // submission. `SubmissionOutcome::kNotQueued` is the case that used to be
    // missing - a slot left marked in flight against a fence nothing will ever
    // signal is retired for good, and enough of those retire the whole ring, at
    // which point sharpening stops for the rest of the session with nothing but
    // "every command buffer is still in flight" to show for it.
    if (disp->fp_vkResetFences(state.device, 1, &state.fences[slotIndex]) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Sharpen fence reset failed (slot=%d image=%u)", slot, imageIndex);
        state.slotSubmitted[slotIndex] = ce::sharpen::SlotIsBusyAfter(ce::sharpen::SubmissionOutcome::kNotQueued);
        return false;
    }
    // VkQueue is externally synchronized and this may be the game's own queue:
    // take the same lock the layer's submit wrappers take for it.
    ScopedBorrowedQueueSubmission sharpenSubmissionGuard(queue);
    const VkResult submitResult = disp->fp_vkQueueSubmit(queue, 1, &submit, state.fences[slotIndex]);
    if (submitResult != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Sharpen submit failed (result=%d slot=%d image=%u)", submitResult, slot, imageIndex);
        state.slotSubmitted[slotIndex] = ce::sharpen::SlotIsBusyAfter(ce::sharpen::SubmissionOutcome::kNotQueued);
        return false;
    }
    state.slotSubmitted[slotIndex] = ce::sharpen::SlotIsBusyAfter(ce::sharpen::SubmissionOutcome::kSubmitted);
    // The copy is now queued ahead of everything that follows on this queue, so
    // the next frame may legitimately claim SHADER_READ_ONLY_OPTIMAL for it.
    state.sourceInitialized = true;

    if (signaledSemaphore)
        *signaledSemaphore = signalSemaphore;
    return true;
}

}  // namespace

void CleanupSharpen(VkDevice device) {
    std::lock_guard<std::mutex> lock(layer_sharpen_g_StateMutex);
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    for (SharpenState& state : layer_sharpen_g_Registry.TakeForDevice(device))
        DestroySharpenState(state, disp);
    // Every swapchain on the device is gone before the device is, so no
    // present can still be waiting on any deferred semaphore.
    DrainDeferredSharpenSemaphoresLocked(device, VK_NULL_HANDLE, true);
}

void ReleaseSharpenForSwapchain(VkDevice device, VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lock(layer_sharpen_g_StateMutex);
    // Every state built over this swapchain goes - the live one and any retired
    // ones still waiting for their fences, since all of them hold views of its
    // images. A driver may hand the next swapchain the same handle (DOOM
    // Eternal `20260922_235937` did), so a state kept past the destroy would be
    // found by the new swapchain's presents and draw through views of freed
    // images. Waiting is correct here: the images die with the swapchain.
    std::vector<SharpenState> states = layer_sharpen_g_Registry.TakeForSwapchain(device, SharpenSwapchainKey(swapchain));
    size_t built = 0;
    for (const SharpenState& state : states)
        built += state.initialized ? 1 : 0;
    if (built > 0) {
        LayerLog("Vulkan Layer: Releasing %zu sharpen state(s) built over swapchain %p before the driver destroys it",
                 built, swapchain);
    }
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    for (SharpenState& state : states)
        DestroySharpenState(state, disp);
}

void DestroyDeferredSharpenSemaphores(VkDevice device, VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lock(layer_sharpen_g_StateMutex);
    DrainDeferredSharpenSemaphoresLocked(device, swapchain, false);
}
