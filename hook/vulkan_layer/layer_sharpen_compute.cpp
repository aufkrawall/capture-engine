#include "layer_sharpen_state.h"

#include "../common/sharpen_shader_spirv.h"

// Compute route of the Vulkan sharpen pass, used when the application presents
// from a queue family without graphics support. See vulkan_sharpen_route_policy.h.
//
// Same copy-then-filter shape as the render-pass route: the frame is copied to
// the source image, and the kernel reads the copy and writes every pixel of the
// presentable image. The difference is only how the image is written - as a
// formatless storage image from a dispatch - so the work stays on the queue the
// game presents from.

namespace {

// Matches local_size_x/y in sharpen_cas.comp and sharpen_rcas.comp.
constexpr uint32_t kSharpenComputeGroupSize = 8;

bool CreateComputeDescriptors(SharpenState& state, DeviceDispatch* disp) {
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (disp->fp_vkCreateDescriptorSetLayout(state.device, &layoutInfo, nullptr, &state.setLayout) != VK_SUCCESS)
        return false;

    const uint32_t imageCount = static_cast<uint32_t>(state.imageViews.size());
    VkDescriptorPoolSize poolSizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageCount},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, imageCount}};
    VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = imageCount;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (disp->fp_vkCreateDescriptorPool(state.device, &poolInfo, nullptr, &state.descriptorPool) != VK_SUCCESS)
        return false;

    std::vector<VkDescriptorSetLayout> layouts(imageCount, state.setLayout);
    state.imageDescriptorSets.assign(imageCount, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo setInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = state.descriptorPool;
    setInfo.descriptorSetCount = imageCount;
    setInfo.pSetLayouts = layouts.data();
    if (disp->fp_vkAllocateDescriptorSets(state.device, &setInfo, state.imageDescriptorSets.data()) != VK_SUCCESS)
        return false;

    for (uint32_t image = 0; image < imageCount; ++image) {
        VkDescriptorImageInfo source = {};
        source.sampler = state.sampler;
        source.imageView = state.sourceView;
        source.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo target = {};
        target.imageView = state.imageViews[image];
        target.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = state.imageDescriptorSets[image];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &source;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = state.imageDescriptorSets[image];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &target;
        disp->fp_vkUpdateDescriptorSets(state.device, 2, writes, 0, nullptr);
    }
    return true;
}

bool CreateComputePipeline(SharpenState& state, DeviceDispatch* disp, const uint32_t* code, size_t byteSize,
                           VkPipeline* pipeline) {
    VkShaderModuleCreateInfo moduleInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = byteSize;
    moduleInfo.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (disp->fp_vkCreateShaderModule(state.device, &moduleInfo, nullptr, &module) != VK_SUCCESS)
        return false;
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - Vulkan structs require zero initialization before their enum fields are assigned
    VkComputePipelineCreateInfo pipelineInfo = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = state.pipelineLayout;
    const VkResult result =
        disp->fp_vkCreateComputePipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, pipeline);
    disp->fp_vkDestroyShaderModule(state.device, module, nullptr);
    return result == VK_SUCCESS;
}

VkImageMemoryBarrier MakeBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                 VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
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

}  // namespace

bool CreateSharpenComputeObjects(SharpenState& state, DeviceDispatch* disp) {
    if (!disp->fp_vkCreateComputePipelines || !disp->fp_vkCmdDispatch || state.imageViews.empty() ||
        state.sourceView == VK_NULL_HANDLE || state.sampler == VK_NULL_HANDLE) {
        return false;
    }
    if (!CreateComputeDescriptors(state, disp))
        return false;

    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.size = sizeof(ce::sharpen::ShaderConstants);
    VkPipelineLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &state.setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (disp->fp_vkCreatePipelineLayout(state.device, &layoutInfo, nullptr, &state.pipelineLayout) != VK_SUCCESS)
        return false;

    return CreateComputePipeline(state, disp, g_SharpenCasComputeShaderSpv, sizeof(g_SharpenCasComputeShaderSpv),
                                 &state.casPipeline) &&
           CreateComputePipeline(state, disp, g_SharpenRcasComputeShaderSpv, sizeof(g_SharpenRcasComputeShaderSpv),
                                 &state.rcasPipeline);
}

void RecordSharpenCompute(SharpenState& state, DeviceDispatch* disp, VkCommandBuffer cmd, VkImage image,
                          uint32_t imageIndex, VkPipeline pipeline, const ce::sharpen::ShaderConstants& constants) {
    // As in the render-pass route: the frame becomes a transfer source, and the
    // copy target waits for the previous frame's shader read of the same image.
    const VkImageMemoryBarrier preCopy[2] = {
        MakeBarrier(image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
        MakeBarrier(state.sourceImage,
                    state.sourceInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                  nullptr, 0, nullptr, 2, preCopy);

    VkImageCopy region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {state.extent.width, state.extent.height, 1};
    disp->fp_vkCmdCopyImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state.sourceImage,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // The kernel overwrites every pixel, so the frame's own contents are not
    // needed in GENERAL beyond what the copy already read.
    const VkImageMemoryBarrier preDispatch[2] = {
        MakeBarrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT),
        MakeBarrier(state.sourceImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
    };
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                  nullptr, 0, nullptr, 2, preDispatch);

    disp->fp_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    disp->fp_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipelineLayout, 0, 1,
                                     &state.imageDescriptorSets[imageIndex], 0, nullptr);
    disp->fp_vkCmdPushConstants(cmd, state.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                                &constants);
    disp->fp_vkCmdDispatch(cmd, (state.extent.width + kSharpenComputeGroupSize - 1) / kSharpenComputeGroupSize,
                           (state.extent.height + kSharpenComputeGroupSize - 1) / kSharpenComputeGroupSize, 1);

    // Hand the image back for presentation. Whatever runs next on this queue -
    // the overlay composite, capture, the present - is ordered by the signal
    // semaphore, which covers every stage.
    const VkImageMemoryBarrier postDispatch = MakeBarrier(image, VK_IMAGE_LAYOUT_GENERAL,
                                                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                                          VK_ACCESS_SHADER_WRITE_BIT, 0);
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                                  0, nullptr, 0, nullptr, 1, &postDispatch);
}
