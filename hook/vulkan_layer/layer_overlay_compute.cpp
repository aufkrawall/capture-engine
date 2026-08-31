/**
 * VK_LAYER_CE_overlay - compute-present compositor
 *
 * A graphics render pass after a compute-only present producer creates a
 * compute -> graphics -> present round trip. Render the overlay concurrently
 * into a transparent image instead, then alpha-composite its occupied rectangle
 * on the original compute/present queue.
 */

#include "layer_overlay_internal.h"

#include <algorithm>
#include <vector>

#include "../common/overlay_shader_spirv.h"

namespace {

constexpr uint64_t kDiagnosticWindowFrames = 2048;
constexpr uint64_t kDiagnosticSampleInterval = 128;

uint32_t FindDeviceLocalMemoryType(const OverlayState& state, uint32_t typeBits) {
    InstanceDispatch* inst = VulkanLayerState::Get().GetInstanceDispatch(state.instance);
    if (!inst || !inst->fp_vkGetPhysicalDeviceMemoryProperties)
        return UINT32_MAX;
    VkPhysicalDeviceMemoryProperties properties = {};
    inst->fp_vkGetPhysicalDeviceMemoryProperties(state.physicalDevice, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0 &&
            (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool CreateOffscreenRenderPass(OverlayState& state, DeviceDispatch* disp) {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - Vulkan structs require zero initialization before their enum fields are assigned
    VkAttachmentDescription attachment = {};
    attachment.format = state.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;

    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    return disp->fp_vkCreateRenderPass(state.device, &info, nullptr, &state.offscreenRenderPass) == VK_SUCCESS;
}

// One slot's offscreen target, appended at the end of the per-slot vectors so
// the slot index stays the vector index for both initial creation and growth.
bool AppendOffscreenTarget(OverlayState& state, DeviceDispatch* disp, uint32_t graphicsFamily,
                           uint32_t computeFamily) {
    const uint32_t families[2] = {graphicsFamily, computeFamily};
    state.offscreenImages.push_back(VK_NULL_HANDLE);
    state.offscreenMemory.push_back(VK_NULL_HANDLE);
    state.offscreenImageViews.push_back(VK_NULL_HANDLE);
    state.offscreenFramebuffers.push_back(VK_NULL_HANDLE);
    state.offscreenReadySemaphores.push_back(VK_NULL_HANDLE);
    const size_t i = state.offscreenImages.size() - 1;
    {
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - Vulkan structs require zero initialization before their enum fields are assigned
        VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = state.format;
        imageInfo.extent = {state.extent.width, state.extent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = graphicsFamily == computeFamily ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
        imageInfo.queueFamilyIndexCount = graphicsFamily == computeFamily ? 0u : 2u;
        imageInfo.pQueueFamilyIndices = graphicsFamily == computeFamily ? nullptr : families;
        if (disp->fp_vkCreateImage(state.device, &imageInfo, nullptr, &state.offscreenImages[i]) != VK_SUCCESS)
            return false;

        VkMemoryRequirements requirements = {};
        disp->fp_vkGetImageMemoryRequirements(state.device, state.offscreenImages[i], &requirements);
        const uint32_t memoryType = FindDeviceLocalMemoryType(state, requirements.memoryTypeBits);
        if (memoryType == UINT32_MAX)
            return false;
        VkMemoryAllocateInfo allocation = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        if (disp->fp_vkAllocateMemory(state.device, &allocation, nullptr, &state.offscreenMemory[i]) != VK_SUCCESS ||
            disp->fp_vkBindImageMemory(state.device, state.offscreenImages[i], state.offscreenMemory[i], 0) !=
                VK_SUCCESS) {
            return false;
        }
        state.offscreenMemoryBytes += requirements.size;
        state.offscreenSlotBytes = requirements.size;

        VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = state.offscreenImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = state.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (disp->fp_vkCreateImageView(state.device, &viewInfo, nullptr, &state.offscreenImageViews[i]) != VK_SUCCESS)
            return false;

        VkFramebufferCreateInfo framebufferInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = state.offscreenRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &state.offscreenImageViews[i];
        framebufferInfo.width = state.extent.width;
        framebufferInfo.height = state.extent.height;
        framebufferInfo.layers = 1;
        if (disp->fp_vkCreateFramebuffer(state.device, &framebufferInfo, nullptr,
                                         &state.offscreenFramebuffers[i]) != VK_SUCCESS) {
            return false;
        }

        VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (disp->fp_vkCreateSemaphore(state.device, &semaphoreInfo, nullptr,
                                       &state.offscreenReadySemaphores[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

// The last appended slot's offscreen target, released so a failed growth leaves
// the compute route exactly as complete as it was.
void PopOffscreenTarget(OverlayState& state, DeviceDispatch* disp) {
    if (state.offscreenImages.empty())
        return;
    const size_t i = state.offscreenImages.size() - 1;
    if (state.offscreenReadySemaphores[i] != VK_NULL_HANDLE)
        disp->fp_vkDestroySemaphore(state.device, state.offscreenReadySemaphores[i], nullptr);
    if (state.offscreenFramebuffers[i] != VK_NULL_HANDLE)
        disp->fp_vkDestroyFramebuffer(state.device, state.offscreenFramebuffers[i], nullptr);
    if (state.offscreenImageViews[i] != VK_NULL_HANDLE)
        disp->fp_vkDestroyImageView(state.device, state.offscreenImageViews[i], nullptr);
    if (state.offscreenImages[i] != VK_NULL_HANDLE)
        disp->fp_vkDestroyImage(state.device, state.offscreenImages[i], nullptr);
    if (state.offscreenMemory[i] != VK_NULL_HANDLE) {
        disp->fp_vkFreeMemory(state.device, state.offscreenMemory[i], nullptr);
        state.offscreenMemoryBytes -= std::min(state.offscreenMemoryBytes, state.offscreenSlotBytes);
    }
    state.offscreenReadySemaphores.pop_back();
    state.offscreenFramebuffers.pop_back();
    state.offscreenImageViews.pop_back();
    state.offscreenMemory.pop_back();
    state.offscreenImages.pop_back();
}

bool CreateComputePipeline(OverlayState& state, DeviceDispatch* disp) {
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo descriptorLayout = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptorLayout.bindingCount = 2;
    descriptorLayout.pBindings = bindings;
    if (disp->fp_vkCreateDescriptorSetLayout(state.device, &descriptorLayout, nullptr,
                                              &state.computeDescriptorSetLayout) != VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.size = sizeof(int32_t) * 4;
    VkPipelineLayoutCreateInfo pipelineLayout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayout.setLayoutCount = 1;
    pipelineLayout.pSetLayouts = &state.computeDescriptorSetLayout;
    pipelineLayout.pushConstantRangeCount = 1;
    pipelineLayout.pPushConstantRanges = &pushRange;
    if (disp->fp_vkCreatePipelineLayout(state.device, &pipelineLayout, nullptr, &state.computePipelineLayout) !=
        VK_SUCCESS) {
        return false;
    }

    VkShaderModuleCreateInfo shaderInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = sizeof(g_ComputeCompositeShaderSpv);
    shaderInfo.pCode = g_ComputeCompositeShaderSpv;
    VkShaderModule shader = VK_NULL_HANDLE;
    if (disp->fp_vkCreateShaderModule(state.device, &shaderInfo, nullptr, &shader) != VK_SUCCESS)
        return false;
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - Vulkan structs require zero initialization before their enum fields are assigned
    VkPipelineShaderStageCreateInfo stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - Vulkan structs require zero initialization before their enum fields are assigned
    VkComputePipelineCreateInfo pipelineInfo = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = stage;
    pipelineInfo.layout = state.computePipelineLayout;
    const VkResult result = disp->fp_vkCreateComputePipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                                              &state.computePipeline);
    disp->fp_vkDestroyShaderModule(state.device, shader, nullptr);
    return result == VK_SUCCESS;
}

bool CreateComputeSampler(OverlayState& state, DeviceDispatch* disp) {
    VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    return disp->fp_vkCreateSampler(state.device, &samplerInfo, nullptr, &state.computeSampler) == VK_SUCCESS;
}

// Descriptor/command state is cached for every (submission slot, target image)
// pair, which keeps the full-resolution offscreen allocation at one image per
// slot while avoiding descriptor updates and command recording when MFG makes
// image identity advance independently from slot identity. The pair index is
// slot-major (ComputeCompositeResourceIndex), so one slot's worth appended at
// the end keeps every existing index valid.
bool AppendComputeDescriptorSetsForSlot(OverlayState& state, DeviceDispatch* disp, uint32_t slot) {
    const uint32_t imageCount = static_cast<uint32_t>(state.swapchainImages.size());
    if (imageCount == 0 || state.imageViews.size() < imageCount || state.offscreenImageViews.size() <= slot ||
        state.computeDescriptorSetLayout == VK_NULL_HANDLE) {
        return false;
    }
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, imageCount},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageCount}};
    VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = imageCount;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (disp->fp_vkCreateDescriptorPool(state.device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
        return false;

    std::vector<VkDescriptorSetLayout> layouts(imageCount, state.computeDescriptorSetLayout);
    std::vector<VkDescriptorSet> sets(imageCount, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo allocation = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = pool;
    allocation.descriptorSetCount = imageCount;
    allocation.pSetLayouts = layouts.data();
    if (disp->fp_vkAllocateDescriptorSets(state.device, &allocation, sets.data()) != VK_SUCCESS) {
        disp->fp_vkDestroyDescriptorPool(state.device, pool, nullptr);
        return false;
    }

    state.computeDescriptorPools.push_back(pool);
    state.computeDescriptorSets.insert(state.computeDescriptorSets.end(), sets.begin(), sets.end());
    for (uint32_t image = 0; image < imageCount; ++image) {
        const uint32_t resourceIndex =
            ce::overlay_submit_queue_policy::ComputeCompositeResourceIndex(slot, image, imageCount);
        VkDescriptorImageInfo target = {};
        target.imageView = state.imageViews[image];
        target.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo overlay = {};
        overlay.sampler = state.computeSampler;
        overlay.imageView = state.offscreenImageViews[slot];
        overlay.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = state.computeDescriptorSets[resourceIndex];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &target;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = state.computeDescriptorSets[resourceIndex];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &overlay;
        disp->fp_vkUpdateDescriptorSets(state.device, 2, writes, 0, nullptr);
    }
    return true;
}

bool AppendComputeCommandBuffersForSlot(OverlayState& state, DeviceDispatch* disp) {
    const uint32_t imageCount = static_cast<uint32_t>(state.swapchainImages.size());
    if (imageCount == 0 || state.computeCommandPool == VK_NULL_HANDLE)
        return false;
    std::vector<VkCommandBuffer> commands(imageCount, VK_NULL_HANDLE);
    VkCommandBufferAllocateInfo commandInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = state.computeCommandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = imageCount;
    if (disp->fp_vkAllocateCommandBuffers(state.device, &commandInfo, commands.data()) != VK_SUCCESS)
        return false;
    state.computeCommandBuffers.insert(state.computeCommandBuffers.end(), commands.begin(), commands.end());
    state.computeCommandBounds.resize(state.computeCommandBuffers.size());
    state.computeCommandRecorded.resize(state.computeCommandBuffers.size(), 0);
    return true;
}

bool CreateComputeCommandPool(OverlayState& state, DeviceDispatch* disp, uint32_t computeFamily) {
    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = computeFamily;
    return disp->fp_vkCreateCommandPool(state.device, &poolInfo, nullptr, &state.computeCommandPool) == VK_SUCCESS;
}

// Transactional: on failure nothing of this slot is left behind, so the caller
// may keep using the route at its current depth.
bool AppendComputePresentSlotLocked(OverlayState& state, DeviceDispatch* disp) {
    const uint32_t slot = static_cast<uint32_t>(state.offscreenImages.size());
    if (!AppendOffscreenTarget(state, disp, state.computeGraphicsQueueFamilyIndex,
                               state.computeQueueFamilyIndex)) {
        PopOffscreenTarget(state, disp);
        return false;
    }
    if (!AppendComputeDescriptorSetsForSlot(state, disp, slot)) {
        PopOffscreenTarget(state, disp);
        return false;
    }
    if (!AppendComputeCommandBuffersForSlot(state, disp)) {
        // The descriptor pool owns its sets; destroying it frees them all.
        disp->fp_vkDestroyDescriptorPool(state.device, state.computeDescriptorPools.back(), nullptr);
        state.computeDescriptorPools.pop_back();
        state.computeDescriptorSets.resize(static_cast<size_t>(slot) * state.swapchainImages.size());
        PopOffscreenTarget(state, disp);
        return false;
    }
    return true;
}

bool InitializeComputePresentOverlay(OverlayState& state, DeviceDispatch* disp, uint32_t graphicsFamily,
                                     uint32_t computeFamily) {
    if (state.computePresentInitialized)
        return state.computeQueueFamilyIndex == computeFamily;
    if (state.computePresentUnavailable)
        return false;
    if (!disp->fp_vkCreateComputePipelines || !disp->fp_vkCmdDispatch || !disp->fp_vkCreateImage ||
        !disp->fp_vkBindImageMemory || !disp->fp_vkCreateDescriptorSetLayout) {
        state.computePresentUnavailable = true;
        return false;
    }

    state.computeQueueFamilyIndex = computeFamily;
    state.computeGraphicsQueueFamilyIndex = graphicsFamily;
    const size_t slotCount = state.fences.size();
    const size_t imageCount = state.swapchainImages.size();
    bool created = ce::overlay_submit_queue_policy::RequiredComputeCompositeResourceCount(
                       static_cast<uint32_t>(slotCount), static_cast<uint32_t>(imageCount)) != 0 &&
                   slotCount <= UINT32_MAX / std::max<size_t>(imageCount, 1) &&
                   CreateOffscreenRenderPass(state, disp) && CreateComputePipeline(state, disp) &&
                   CreateComputeSampler(state, disp) && CreateComputeCommandPool(state, disp, computeFamily);
    for (size_t slot = 0; created && slot < slotCount; ++slot) {
        created = AppendComputePresentSlotLocked(state, disp);
    }
    if (!created) {
        CleanupComputePresentOverlay(state, disp);
        state.computePresentUnavailable = true;
        LayerLog("Vulkan Layer: Compute-present overlay resource creation failed; retaining graphics fallback");
        return false;
    }
    state.computeWaitSemaphores.reserve(2);
    state.computeWaitStages.reserve(2);

    state.computePresentInitialized = true;
    LayerLog(
        "Vulkan Layer: Compute-present overlay ready (graphicsFamily=%u computePresentFamily=%u images=%u "
        "submissionSlots=%u cachedComposites=%u format=%d)",
        graphicsFamily, computeFamily, static_cast<uint32_t>(imageCount), static_cast<uint32_t>(slotCount),
        static_cast<uint32_t>(state.computeCommandBuffers.size()), state.format);
    return true;
}

void RecordSwapchainLayoutBarrier(DeviceDispatch* disp, VkCommandBuffer command, VkImage image, VkImageLayout oldLayout,
                                  VkImageLayout newLayout, VkPipelineStageFlags sourceStage,
                                  VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess,
                                  VkAccessFlags destinationAccess) {
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    disp->fp_vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void CommitComputePresentDiagnostics(OverlayState& state, bool commandReused, bool phaseSampled,
                                     uint64_t graphicsRecordUs, uint64_t graphicsSubmitUs, uint64_t computeSubmitUs,
                                     uint64_t computeRecordMissUs) {
    ComputePresentDiagnostics& stats = state.computePresentDiagnostics;
    ++stats.frames;
    if (commandReused) {
        ++stats.commandCacheHits;
    } else {
        ++stats.commandRecordMisses;
        stats.computeRecordMissUs += computeRecordMissUs;
    }
    if (phaseSampled) {
        ++stats.phaseSamples;
        stats.graphicsRecordUs += graphicsRecordUs;
        stats.graphicsSubmitUs += graphicsSubmitUs;
        stats.computeSubmitUs += computeSubmitUs;
    }
    if (stats.frames < kDiagnosticWindowFrames)
        return;

    const uint64_t phaseSamples = std::max<uint64_t>(stats.phaseSamples, 1);
    const uint64_t recordMisses = std::max<uint64_t>(stats.commandRecordMisses, 1);
    LayerLog(
        "Vulkan Layer: Compute-present CPU summary frames=%llu commandCacheHits=%llu/%llu "
        "graphicsRecordAvg=%lluus graphicsSubmitAvg=%lluus computeSubmitAvg=%lluus "
        "computeRecordMissAvg=%lluus",
        static_cast<unsigned long long>(stats.frames), static_cast<unsigned long long>(stats.commandCacheHits),
        static_cast<unsigned long long>(stats.frames),
        static_cast<unsigned long long>(stats.graphicsRecordUs / phaseSamples),
        static_cast<unsigned long long>(stats.graphicsSubmitUs / phaseSamples),
        static_cast<unsigned long long>(stats.computeSubmitUs / phaseSamples),
        static_cast<unsigned long long>(stats.computeRecordMissUs / recordMisses));
    stats = {};
}

}  // namespace

void CleanupComputePresentOverlay(OverlayState& state, DeviceDispatch* disp) {
    if (!disp)
        return;
    if (state.computePipeline != VK_NULL_HANDLE)
        disp->fp_vkDestroyPipeline(state.device, state.computePipeline, nullptr);
    if (state.computePipelineLayout != VK_NULL_HANDLE)
        disp->fp_vkDestroyPipelineLayout(state.device, state.computePipelineLayout, nullptr);
    for (VkDescriptorPool pool : state.computeDescriptorPools) {
        if (pool != VK_NULL_HANDLE)
            disp->fp_vkDestroyDescriptorPool(state.device, pool, nullptr);
    }
    if (state.computeDescriptorSetLayout != VK_NULL_HANDLE)
        disp->fp_vkDestroyDescriptorSetLayout(state.device, state.computeDescriptorSetLayout, nullptr);
    if (state.computeSampler != VK_NULL_HANDLE)
        disp->fp_vkDestroySampler(state.device, state.computeSampler, nullptr);
    if (state.computeCommandPool != VK_NULL_HANDLE)
        disp->fp_vkDestroyCommandPool(state.device, state.computeCommandPool, nullptr);
    for (VkFramebuffer framebuffer : state.offscreenFramebuffers) {
        if (framebuffer != VK_NULL_HANDLE)
            disp->fp_vkDestroyFramebuffer(state.device, framebuffer, nullptr);
    }
    for (VkImageView view : state.offscreenImageViews) {
        if (view != VK_NULL_HANDLE)
            disp->fp_vkDestroyImageView(state.device, view, nullptr);
    }
    for (VkImage image : state.offscreenImages) {
        if (image != VK_NULL_HANDLE)
            disp->fp_vkDestroyImage(state.device, image, nullptr);
    }
    for (VkDeviceMemory memory : state.offscreenMemory) {
        if (memory != VK_NULL_HANDLE)
            disp->fp_vkFreeMemory(state.device, memory, nullptr);
    }
    for (VkSemaphore semaphore : state.offscreenReadySemaphores) {
        if (semaphore != VK_NULL_HANDLE)
            disp->fp_vkDestroySemaphore(state.device, semaphore, nullptr);
    }
    if (state.offscreenRenderPass != VK_NULL_HANDLE)
        disp->fp_vkDestroyRenderPass(state.device, state.offscreenRenderPass, nullptr);
    state.computePipeline = VK_NULL_HANDLE;
    state.computePipelineLayout = VK_NULL_HANDLE;
    state.computeDescriptorPools.clear();
    state.computeDescriptorSetLayout = VK_NULL_HANDLE;
    state.computeSampler = VK_NULL_HANDLE;
    state.computeCommandPool = VK_NULL_HANDLE;
    state.offscreenRenderPass = VK_NULL_HANDLE;
    state.computeCommandBuffers.clear();
    state.computeCommandBounds.clear();
    state.computeCommandRecorded.clear();
    state.computeWaitSemaphores.clear();
    state.computeWaitStages.clear();
    state.computeDescriptorSets.clear();
    state.offscreenFramebuffers.clear();
    state.offscreenImageViews.clear();
    state.offscreenImages.clear();
    state.offscreenMemory.clear();
    state.offscreenReadySemaphores.clear();
    state.offscreenMemoryBytes = 0;
    state.computeGraphicsQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    state.computePresentInitialized = false;
    state.computePresentDiagnostics = {};
}

bool AppendComputePresentSlot(OverlayState& state, DeviceDispatch* disp) {
    if (!disp || !state.computePresentInitialized)
        return false;
    if (!AppendComputePresentSlotLocked(state, disp))
        return false;
    LayerLog(
        "Vulkan Layer: compute-composite route extended to %zu slots so every present keeps one composite route "
        "(%llu MB of offscreen overlay targets)",
        state.offscreenImages.size(), static_cast<unsigned long long>(state.offscreenMemoryBytes / (1024 * 1024)));
    return true;
}

bool RenderComputePresentOverlay(OverlayState& state, DeviceDispatch* disp, const OverlaySubmitTarget& graphicsTarget,
                                 VkQueue presentQueue, uint32_t submissionSlot, uint32_t imageIndex,
                                 const VkSemaphore* waitSemaphores,
                                 uint32_t waitSemaphoreCount, VkSemaphore signalSemaphore, bool* routeAttempted) {
    if (routeAttempted)
        *routeAttempted = false;
    const uint32_t presentFamily = VulkanLayerState::Get().GetQueueFamilyIndex(presentQueue);
    if (!InitializeComputePresentOverlay(state, disp, graphicsTarget.queueFamilyIndex, presentFamily))
        return false;
    const uint32_t imageCount = static_cast<uint32_t>(state.swapchainImages.size());
    const uint32_t resourceIndex = ce::overlay_submit_queue_policy::ComputeCompositeResourceIndex(
        submissionSlot, imageIndex, imageCount);
    if (submissionSlot >= state.commandBuffers.size() || submissionSlot >= state.offscreenFramebuffers.size() ||
        submissionSlot >= state.offscreenReadySemaphores.size() || imageIndex >= imageCount ||
        resourceIndex >= state.computeCommandBuffers.size() || resourceIndex >= state.computeCommandBounds.size() ||
        resourceIndex >= state.computeCommandRecorded.size() || resourceIndex >= state.computeDescriptorSets.size() ||
        !state.overlayAdapter) {
        return false;
    }

    const bool phaseSampled =
        (state.computePresentDiagnostics.frames + 1) % kDiagnosticSampleInterval == 0;
    const int64_t graphicsRecordStartUs = phaseSampled ? PerfLogger::GetQpcUs() : 0;

    VkCommandBuffer graphicsCommand = state.commandBuffers[submissionSlot];
    disp->fp_vkResetCommandBuffer(graphicsCommand, 0);
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (disp->fp_vkBeginCommandBuffer(graphicsCommand, &begin) != VK_SUCCESS)
        return false;
    // The query pool was sized for the slots that existed at initialization.
    // A slot the ring added later has no queries reserved, so it reports the
    // last measured GPU time instead of writing outside the pool.
    const bool writeTimestamps = state.timestampPool != VK_NULL_HANDLE && disp->fp_vkCmdResetQueryPool &&
                                 disp->fp_vkCmdWriteTimestamp && submissionSlot < state.timestampSlotCapacity &&
                                 submissionSlot < state.timestampWritten.size();
    if (writeTimestamps) {
        disp->fp_vkCmdResetQueryPool(graphicsCommand, state.timestampPool, submissionSlot * 2, 2);
        disp->fp_vkCmdWriteTimestamp(graphicsCommand, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, state.timestampPool,
                                     submissionSlot * 2);
    }

    VkRenderPassBeginInfo renderPass = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    renderPass.renderPass = state.offscreenRenderPass;
    renderPass.framebuffer = state.offscreenFramebuffers[submissionSlot];
    renderPass.renderArea.extent = state.extent;
    disp->fp_vkCmdBeginRenderPass(graphicsCommand, &renderPass, VK_SUBPASS_CONTENTS_INLINE);
    auto* backend = state.overlayAdapter->GetBackendType() == OverlayBackendType::Vulkan
                        ? static_cast<CustomOverlay::VulkanBackend*>(state.overlayAdapter->GetBackend())
                        : nullptr;
    if (!backend) {
        disp->fp_vkCmdEndRenderPass(graphicsCommand);
        disp->fp_vkEndCommandBuffer(graphicsCommand);
        return false;
    }
    backend->SetRenderContext(graphicsCommand, state.offscreenRenderPass,
                              state.offscreenFramebuffers[submissionSlot], state.extent, true);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - Vulkan extents are bounded by the implementation and OverlayAdapter intentionally accepts int dimensions
    state.overlayAdapter->RenderOverlay(state.extent.width, state.extent.height);
    const VkRect2D bounds = backend->GetLastRenderBounds();
    disp->fp_vkCmdEndRenderPass(graphicsCommand);
    if (writeTimestamps) {
        disp->fp_vkCmdWriteTimestamp(graphicsCommand, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, state.timestampPool,
                                     submissionSlot * 2 + 1);
    }
    if (disp->fp_vkEndCommandBuffer(graphicsCommand) != VK_SUCCESS)
        return false;
    const uint64_t graphicsRecordUs =
        phaseSampled ? static_cast<uint64_t>(PerfLogger::GetQpcUs() - graphicsRecordStartUs) : 0;

    VkCommandBuffer computeCommand = state.computeCommandBuffers[resourceIndex];
    const ce::overlay_submit_queue_policy::ComputeCompositeBounds currentBounds = {
        bounds.offset.x, bounds.offset.y, bounds.extent.width, bounds.extent.height};
    const bool commandReused = ce::overlay_submit_queue_policy::CanReuseComputeCompositeCommand(
        state.computeCommandRecorded[resourceIndex] != 0, state.computeCommandBounds[resourceIndex], currentBounds);
    uint64_t computeRecordMissUs = 0;
    if (!commandReused) {
        const int64_t computeRecordStartUs = PerfLogger::GetQpcUs();
        state.computeCommandRecorded[resourceIndex] = 0;
        if (disp->fp_vkResetCommandBuffer(computeCommand, 0) != VK_SUCCESS)
            return false;
        VkCommandBufferBeginInfo computeBegin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        if (disp->fp_vkBeginCommandBuffer(computeCommand, &computeBegin) != VK_SUCCESS)
            return false;
        // The acquire transition has to be ordered *after* the semaphore wait,
        // and a semaphore wait only orders the stages named in its
        // dstStageMask. This submit waits at COMPUTE_SHADER, so a barrier whose
        // srcStageMask is TOP_OF_PIPE - the shape this used to have - is free
        // to retile the image while the game's own composite into it is still
        // running. That is a per-frame race whose visible form is an overlay
        // that appears on some presented frames and not others.
        RecordSwapchainLayoutBarrier(disp, computeCommand, state.swapchainImages[imageIndex],
                                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_GENERAL,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        if (bounds.extent.width > 0 && bounds.extent.height > 0) {
            const int32_t rect[4] = {bounds.offset.x, bounds.offset.y, static_cast<int32_t>(bounds.extent.width),
                                     static_cast<int32_t>(bounds.extent.height)};
            disp->fp_vkCmdBindPipeline(computeCommand, VK_PIPELINE_BIND_POINT_COMPUTE, state.computePipeline);
            disp->fp_vkCmdBindDescriptorSets(computeCommand, VK_PIPELINE_BIND_POINT_COMPUTE,
                                             state.computePipelineLayout, 0, 1,
                                             &state.computeDescriptorSets[resourceIndex], 0, nullptr);
            disp->fp_vkCmdPushConstants(computeCommand, state.computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                        sizeof(rect), rect);
            disp->fp_vkCmdDispatch(computeCommand, (bounds.extent.width + 15) / 16,
                                   (bounds.extent.height + 15) / 16, 1);
        }
        RecordSwapchainLayoutBarrier(disp, computeCommand, state.swapchainImages[imageIndex],
                                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                     VK_ACCESS_SHADER_WRITE_BIT, 0);
        if (disp->fp_vkEndCommandBuffer(computeCommand) != VK_SUCCESS)
            return false;
        state.computeCommandBounds[resourceIndex] = currentBounds;
        state.computeCommandRecorded[resourceIndex] = 1;
        computeRecordMissUs = static_cast<uint64_t>(PerfLogger::GetQpcUs() - computeRecordStartUs);
    }

    VkSubmitInfo graphicsSubmit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    graphicsSubmit.commandBufferCount = 1;
    graphicsSubmit.pCommandBuffers = &graphicsCommand;
    graphicsSubmit.signalSemaphoreCount = 1;
    graphicsSubmit.pSignalSemaphores = &state.offscreenReadySemaphores[submissionSlot];
    const int64_t graphicsSubmitStartUs = phaseSampled ? PerfLogger::GetQpcUs() : 0;
    VkFence fence = state.fences[submissionSlot];
    {
        ScopedBorrowedQueueSubmission guard(graphicsTarget.queue);
        if (disp->fp_vkQueueSubmit(graphicsTarget.queue, 1, &graphicsSubmit, VK_NULL_HANDLE) != VK_SUCCESS) {
            LayerLog("Vulkan Layer: compute-present offscreen submit FAILED (slot %u, image %u)", submissionSlot,
                     imageIndex);
            return false;
        }
    }
    // Only now, immediately before the submit that carries it. Resetting a slot
    // fence and then failing to submit leaves it unsignalled for the lifetime of
    // the swapchain, which silently shrinks the ring one slot at a time.
    if (disp->fp_vkResetFences(state.device, 1, &fence) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: compute-present fence reset FAILED (slot %u)", submissionSlot);
        return false;
    }
    const uint64_t graphicsSubmitUs =
        phaseSampled ? static_cast<uint64_t>(PerfLogger::GetQpcUs() - graphicsSubmitStartUs) : 0;
    if (routeAttempted)
        *routeAttempted = true;

    std::vector<VkSemaphore>& waits = state.computeWaitSemaphores;
    std::vector<VkPipelineStageFlags>& waitStages = state.computeWaitStages;
    waits.resize(waitSemaphoreCount + 1);
    waitStages.resize(waitSemaphoreCount + 1);
    for (uint32_t i = 0; i < waitSemaphoreCount; ++i) {
        waits[i] = waitSemaphores[i];
        waitStages[i] = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
    waits[waitSemaphoreCount] = state.offscreenReadySemaphores[submissionSlot];
    waitStages[waitSemaphoreCount] = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkSubmitInfo computeSubmit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    computeSubmit.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
    computeSubmit.pWaitSemaphores = waits.data();
    computeSubmit.pWaitDstStageMask = waitStages.data();
    computeSubmit.commandBufferCount = 1;
    computeSubmit.pCommandBuffers = &computeCommand;
    computeSubmit.signalSemaphoreCount = 1;
    computeSubmit.pSignalSemaphores = &signalSemaphore;
    const int64_t computeSubmitStartUs = phaseSampled ? PerfLogger::GetQpcUs() : 0;
    if (disp->fp_vkQueueSubmit(presentQueue, 1, &computeSubmit, fence) != VK_SUCCESS) {
        // The fence is disarmed and nothing will signal it. Re-arm it with a
        // fence-only submit rather than retiring this slot permanently.
        LayerLog("Vulkan Layer: compute-present composite submit FAILED (slot %u, image %u); re-arming its fence",
                 submissionSlot, imageIndex);
        if (disp->fp_vkQueueSubmit(presentQueue, 0, nullptr, fence) != VK_SUCCESS) {
            LayerLog("Vulkan Layer: [Error] overlay submission slot %u is stranded; the ring is one slot shallower",
                     submissionSlot);
        }
        return false;
    }
    const uint64_t computeSubmitUs =
        phaseSampled ? static_cast<uint64_t>(PerfLogger::GetQpcUs() - computeSubmitStartUs) : 0;
    CommitComputePresentDiagnostics(state, commandReused, phaseSampled, graphicsRecordUs, graphicsSubmitUs,
                                    computeSubmitUs, computeRecordMissUs);
    if (writeTimestamps)
        state.timestampWritten[submissionSlot] = true;
    return true;
}
