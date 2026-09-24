#include "layer_sharpen_state.h"

#include "../common/sharpen_shader_spirv.h"
#include "overlay_swapchain_lifetime_policy.h"

// Lifecycle half of the Vulkan sharpen pass: everything that is built once per
// swapchain generation and destroyed with it.

std::mutex layer_sharpen_g_StateMutex;
ce::vulkan_sharpen_registry::Registry<SharpenState, VkDevice> layer_sharpen_g_Registry;
std::vector<DeferredSharpenSemaphores> layer_sharpen_g_DeferredSemaphores;

namespace {

uint32_t FindDeviceLocalMemoryType(const SharpenState& state, uint32_t typeBits) {
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

VkShaderModule CreateModule(DeviceDispatch* disp, VkDevice device, const uint32_t* code, size_t byteSize) {
    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = byteSize;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (disp->fp_vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module;
}

bool CreateRenderPass(SharpenState& state, DeviceDispatch* disp) {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - Vulkan structs require zero initialization before their enum fields are assigned
    VkAttachmentDescription attachment = {};
    attachment.format = state.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    // The pass writes every pixel of the frame, so nothing has to be loaded.
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // The recording half transitions the image into this layout before the pass
    // begins, and the pass hands it back ready to present.
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - Vulkan structs require zero initialization before their enum fields are assigned
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    // The copy that produced the source has to be complete before the fragment
    // shader samples it, and the colour writes have to be complete before
    // anything after the pass reads the frame.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - Vulkan structs require zero initialization before their enum fields are assigned
    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    return disp->fp_vkCreateRenderPass(state.device, &info, nullptr, &state.renderPass) == VK_SUCCESS;
}

bool CreateSourceImage(SharpenState& state, DeviceDispatch* disp) {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - Vulkan structs require zero initialization before their enum fields are assigned
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = state.format;
    imageInfo.extent = {state.extent.width, state.extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (disp->fp_vkCreateImage(state.device, &imageInfo, nullptr, &state.sourceImage) != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements = {};
    disp->fp_vkGetImageMemoryRequirements(state.device, state.sourceImage, &requirements);
    const uint32_t memoryType = FindDeviceLocalMemoryType(state, requirements.memoryTypeBits);
    if (memoryType == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo allocation = {};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (disp->fp_vkAllocateMemory(state.device, &allocation, nullptr, &state.sourceMemory) != VK_SUCCESS)
        return false;
    if (disp->fp_vkBindImageMemory(state.device, state.sourceImage, state.sourceMemory, 0) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = state.sourceImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = state.format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (disp->fp_vkCreateImageView(state.device, &viewInfo, nullptr, &state.sourceView) != VK_SUCCESS)
        return false;

    state.sourceInitialized = false;
    return true;
}

bool CreateSampler(SharpenState& state, DeviceDispatch* disp) {
    // The kernel only ever uses texelFetch, so the sampler's filtering and
    // addressing never come into play; it exists because the binding is a
    // combined image sampler.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - Vulkan structs require zero initialization before their enum fields are assigned
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    return disp->fp_vkCreateSampler(state.device, &samplerInfo, nullptr, &state.sampler) == VK_SUCCESS;
}

bool CreateDescriptorObjects(SharpenState& state, DeviceDispatch* disp) {
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (disp->fp_vkCreateDescriptorSetLayout(state.device, &layoutInfo, nullptr, &state.setLayout) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (disp->fp_vkCreateDescriptorPool(state.device, &poolInfo, nullptr, &state.descriptorPool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo setInfo = {};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = state.descriptorPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &state.setLayout;
    if (disp->fp_vkAllocateDescriptorSets(state.device, &setInfo, &state.descriptorSet) != VK_SUCCESS)
        return false;

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.sampler = state.sampler;
    imageInfo.imageView = state.sourceView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = state.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    disp->fp_vkUpdateDescriptorSets(state.device, 1, &write, 0, nullptr);
    return true;
}

bool CreatePipelines(SharpenState& state, DeviceDispatch* disp) {
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(ce::sharpen::ShaderConstants);
    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &state.setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (disp->fp_vkCreatePipelineLayout(state.device, &layoutInfo, nullptr, &state.pipelineLayout) != VK_SUCCESS)
        return false;

    VkShaderModule vertexModule =
        CreateModule(disp, state.device, g_SharpenVertexShaderSpv, sizeof(g_SharpenVertexShaderSpv));
    VkShaderModule casModule =
        CreateModule(disp, state.device, g_SharpenCasFragmentShaderSpv, sizeof(g_SharpenCasFragmentShaderSpv));
    VkShaderModule rcasModule =
        CreateModule(disp, state.device, g_SharpenRcasFragmentShaderSpv, sizeof(g_SharpenRcasFragmentShaderSpv));
    const bool modulesReady = vertexModule && casModule && rcasModule;

    bool created = false;
    if (modulesReady) {
        // The triangle is generated from gl_VertexIndex: no bindings, no
        // attributes, nothing per-swapchain to keep alive.
        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - Vulkan structs require zero initialization before their enum fields are assigned
        VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport viewport = {0.0f, 0.0f, static_cast<float>(state.extent.width),
                               static_cast<float>(state.extent.height), 0.0f, 1.0f};
        VkRect2D scissor = {{0, 0}, state.extent};
        VkPipelineViewportStateCreateInfo viewportState = {};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - Vulkan structs require zero initialization before their enum fields are assigned
        VkPipelineRasterizationStateCreateInfo rasterization = {};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - Vulkan structs require zero initialization before their enum fields are assigned
        VkPipelineMultisampleStateCreateInfo multisample = {};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // The filter replaces the frame; it never blends with what is there.
        VkPipelineColorBlendAttachmentState blendAttachment = {};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - Vulkan structs require zero initialization before their enum fields are assigned
        VkPipelineColorBlendStateCreateInfo blend = {};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - Vulkan structs require zero initialization before their enum fields are assigned
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertexModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].pName = "main";

        VkGraphicsPipelineCreateInfo pipelineInfo = {};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.layout = state.pipelineLayout;
        pipelineInfo.renderPass = state.renderPass;
        pipelineInfo.subpass = 0;

        stages[1].module = casModule;
        const bool casOk = disp->fp_vkCreateGraphicsPipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                                              &state.casPipeline) == VK_SUCCESS;
        stages[1].module = rcasModule;
        const bool rcasOk = disp->fp_vkCreateGraphicsPipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                                               &state.rcasPipeline) == VK_SUCCESS;
        created = casOk && rcasOk;
    }

    if (vertexModule)
        disp->fp_vkDestroyShaderModule(state.device, vertexModule, nullptr);
    if (casModule)
        disp->fp_vkDestroyShaderModule(state.device, casModule, nullptr);
    if (rcasModule)
        disp->fp_vkDestroyShaderModule(state.device, rcasModule, nullptr);
    return created;
}

bool CreateImageSemaphore(SharpenState& state, DeviceDispatch* disp, uint32_t image) {
    VkSemaphoreCreateInfo semaphoreInfo = {};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    return disp->fp_vkCreateSemaphore(state.device, &semaphoreInfo, nullptr, &state.imageSemaphores[image]) ==
           VK_SUCCESS;
}

bool CreatePerImageObjects(SharpenState& state, DeviceDispatch* disp, uint32_t imageCount, const VkImage* images) {
    state.imageViews.assign(imageCount, VK_NULL_HANDLE);
    state.framebuffers.assign(imageCount, VK_NULL_HANDLE);
    state.imageSemaphores.assign(imageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = state.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (disp->fp_vkCreateImageView(state.device, &viewInfo, nullptr, &state.imageViews[i]) != VK_SUCCESS)
            return false;

        // The compute route writes the image as a storage image and has no
        // render pass to build a framebuffer against.
        if (state.renderPass == VK_NULL_HANDLE) {
            if (!CreateImageSemaphore(state, disp, i))
                return false;
            continue;
        }

        VkFramebufferCreateInfo framebufferInfo = {};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = state.renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &state.imageViews[i];
        framebufferInfo.width = state.extent.width;
        framebufferInfo.height = state.extent.height;
        framebufferInfo.layers = 1;
        if (disp->fp_vkCreateFramebuffer(state.device, &framebufferInfo, nullptr, &state.framebuffers[i]) !=
            VK_SUCCESS) {
            return false;
        }

        if (!CreateImageSemaphore(state, disp, i))
            return false;
    }
    return true;
}

bool CreateCommandObjects(SharpenState& state, DeviceDispatch* disp) {
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = state.queueFamily;
    if (disp->fp_vkCreateCommandPool(state.device, &poolInfo, nullptr, &state.commandPool) != VK_SUCCESS)
        return false;

    VkCommandBufferAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = state.commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = kSharpenSlotCount;
    if (disp->fp_vkAllocateCommandBuffers(state.device, &allocateInfo, state.commandBuffers) != VK_SUCCESS)
        return false;

    for (uint32_t slot = 0; slot < kSharpenSlotCount; ++slot) {
        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (disp->fp_vkCreateFence(state.device, &fenceInfo, nullptr, &state.fences[slot]) != VK_SUCCESS)
            return false;
        state.slotSubmitted[slot] = false;
    }
    return true;
}

}  // namespace

bool InitializeSharpenState(SharpenState& state, DeviceDispatch* disp, VkDevice device, VkSwapchainKHR swapchain,
                            VkFormat format, VkExtent2D extent, uint32_t queueFamily,
                            ce::vulkan_sharpen_route::Route route, uint32_t imageCount, const VkImage* images) {
    if (!disp || imageCount == 0 || !images || extent.width == 0 || extent.height == 0 ||
        route == ce::vulkan_sharpen_route::Route::kNone) {
        return false;
    }

    state.device = device;
    state.swapchain = swapchain;
    state.format = format;
    state.extent = extent;
    state.queueFamily = queueFamily;
    state.route = route;
    state.physicalDevice = disp->physicalDevice;
    state.instance = VulkanLayerState::Get().GetInstanceFromPhysicalDevice(disp->physicalDevice);

    const bool ready =
        route == ce::vulkan_sharpen_route::Route::kGraphics
            ? CreateRenderPass(state, disp) && CreateSourceImage(state, disp) && CreateSampler(state, disp) &&
                  CreateDescriptorObjects(state, disp) && CreatePipelines(state, disp) &&
                  CreatePerImageObjects(state, disp, imageCount, images) && CreateCommandObjects(state, disp)
            : CreateSourceImage(state, disp) && CreateSampler(state, disp) &&
                  CreatePerImageObjects(state, disp, imageCount, images) && CreateSharpenComputeObjects(state, disp) &&
                  CreateCommandObjects(state, disp);
    if (!ready) {
        LayerLog("Vulkan Layer: Sharpen initialization failed for swapchain %p (%ux%u fmt=%d route=%s family=%u)",
                 swapchain, extent.width, extent.height, static_cast<int>(format),
                 ce::vulkan_sharpen_route::RouteName(route), queueFamily);
        DestroySharpenState(state, disp);
        return false;
    }

    state.initialized = true;
    LayerLog("Vulkan Layer: Sharpen ready for swapchain %p (%ux%u fmt=%d images=%u family=%u route=%s)", swapchain,
             extent.width, extent.height, static_cast<int>(format), imageCount, queueFamily,
             ce::vulkan_sharpen_route::RouteName(route));
    return true;
}

ce::vulkan_sharpen_route::Identity SharpenStateIdentity(const SharpenState& state) {
    ce::vulkan_sharpen_route::Identity identity;
    identity.swapchain = SharpenSwapchainKey(state.swapchain);
    identity.format = static_cast<int32_t>(state.format);
    identity.width = state.extent.width;
    identity.height = state.extent.height;
    identity.imageCount = static_cast<uint32_t>(state.imageViews.size());
    identity.queueFamily = state.queueFamily;
    identity.route = state.route;
    return identity;
}

bool SharpenStateSubmissionsRetired(const SharpenState& state, DeviceDispatch* disp) {
    if (!disp || state.device == VK_NULL_HANDLE) {
        return true;
    }
    // Only VK_TIMEOUT means "still in flight": after a lost device nothing will
    // ever signal, and the teardown's own waits return at once.
    for (uint32_t slot = 0; slot < kSharpenSlotCount; ++slot) {
        if (state.fences[slot] != VK_NULL_HANDLE && state.slotSubmitted[slot] &&
            disp->fp_vkWaitForFences(state.device, 1, &state.fences[slot], VK_TRUE, 0) == VK_TIMEOUT) {
            return false;
        }
    }
    return true;
}

void DestroySharpenState(SharpenState& state, DeviceDispatch* disp) {
    if (!disp || state.device == VK_NULL_HANDLE) {
        state = SharpenState();
        return;
    }

    // Nothing here may be destroyed while the GPU is still reading it, and the
    // only fences CE owns for this pass are its own submissions'.
    for (uint32_t slot = 0; slot < kSharpenSlotCount; ++slot) {
        if (state.fences[slot] != VK_NULL_HANDLE && state.slotSubmitted[slot]) {
            disp->fp_vkWaitForFences(state.device, 1, &state.fences[slot], VK_TRUE, 1000ull * 1000ull * 1000ull);
        }
    }

    for (VkFramebuffer framebuffer : state.framebuffers) {
        if (framebuffer != VK_NULL_HANDLE)
            disp->fp_vkDestroyFramebuffer(state.device, framebuffer, nullptr);
    }
    for (VkImageView view : state.imageViews) {
        if (view != VK_NULL_HANDLE)
            disp->fp_vkDestroyImageView(state.device, view, nullptr);
    }
    // The present of an image may still be waiting on its semaphore, and CE's
    // own fences say nothing about that wait. Only the swapchain's destruction
    // does, so the semaphores wait for it in the deferred store.
    if (!state.imageSemaphores.empty()) {
        DeferredSharpenSemaphores batch;
        batch.device = state.device;
        batch.swapchain = state.swapchain;
        batch.semaphores = std::move(state.imageSemaphores);
        layer_sharpen_g_DeferredSemaphores.push_back(std::move(batch));
    }
    for (uint32_t slot = 0; slot < kSharpenSlotCount; ++slot) {
        if (state.fences[slot] != VK_NULL_HANDLE)
            disp->fp_vkDestroyFence(state.device, state.fences[slot], nullptr);
    }
    if (state.commandPool != VK_NULL_HANDLE)
        disp->fp_vkDestroyCommandPool(state.device, state.commandPool, nullptr);
    if (state.casPipeline != VK_NULL_HANDLE)
        disp->fp_vkDestroyPipeline(state.device, state.casPipeline, nullptr);
    if (state.rcasPipeline != VK_NULL_HANDLE)
        disp->fp_vkDestroyPipeline(state.device, state.rcasPipeline, nullptr);
    if (state.pipelineLayout != VK_NULL_HANDLE)
        disp->fp_vkDestroyPipelineLayout(state.device, state.pipelineLayout, nullptr);
    if (state.descriptorPool != VK_NULL_HANDLE)
        disp->fp_vkDestroyDescriptorPool(state.device, state.descriptorPool, nullptr);
    if (state.setLayout != VK_NULL_HANDLE)
        disp->fp_vkDestroyDescriptorSetLayout(state.device, state.setLayout, nullptr);
    if (state.sampler != VK_NULL_HANDLE)
        disp->fp_vkDestroySampler(state.device, state.sampler, nullptr);
    if (state.sourceView != VK_NULL_HANDLE)
        disp->fp_vkDestroyImageView(state.device, state.sourceView, nullptr);
    if (state.sourceImage != VK_NULL_HANDLE)
        disp->fp_vkDestroyImage(state.device, state.sourceImage, nullptr);
    if (state.sourceMemory != VK_NULL_HANDLE)
        disp->fp_vkFreeMemory(state.device, state.sourceMemory, nullptr);
    if (state.renderPass != VK_NULL_HANDLE)
        disp->fp_vkDestroyRenderPass(state.device, state.renderPass, nullptr);

    state = SharpenState();
}

void DrainDeferredSharpenSemaphoresLocked(VkDevice device, VkSwapchainKHR destroyedSwapchain, bool deviceTeardown) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    size_t destroyed = 0;
    for (auto batch = layer_sharpen_g_DeferredSemaphores.begin();
         batch != layer_sharpen_g_DeferredSemaphores.end();) {
        ce::overlay_present_semaphore_lifetime::Input input = {};
        input.deferredSwapchain = SharpenSwapchainKey(batch->swapchain);
        input.destroyedSwapchain = SharpenSwapchainKey(destroyedSwapchain);
        input.deviceTeardown = deviceTeardown;
        if (batch->device != device || !ce::overlay_present_semaphore_lifetime::MayDestroy(input)) {
            ++batch;
            continue;
        }
        if (disp && disp->fp_vkDestroySemaphore) {
            for (VkSemaphore semaphore : batch->semaphores) {
                if (semaphore != VK_NULL_HANDLE) {
                    disp->fp_vkDestroySemaphore(device, semaphore, nullptr);
                    ++destroyed;
                }
            }
        }
        batch = layer_sharpen_g_DeferredSemaphores.erase(batch);
    }
    if (destroyed > 0) {
        LayerLog("Vulkan Layer: destroyed %zu sharpen present semaphores held past swapchain %p%s", destroyed,
                 destroyedSwapchain, deviceTeardown ? " (device teardown)" : "");
    }
}
