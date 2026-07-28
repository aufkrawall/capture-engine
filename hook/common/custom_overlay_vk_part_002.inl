
    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth stencil state (disabled for overlay)
    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    // Blend state (alpha blending)
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending = {};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // Dynamic state
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Create textured pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    // Set fragment stage for textured pipeline
    VkPipelineShaderStageCreateInfo texturedStages[] = {vertStage, fragStage};
    pipelineInfo.pStages = texturedStages;

    HookLog("VulkanBackend::CreatePipeline - About to create textured pipeline...");
    HookLog(
        "VulkanBackend::CreatePipeline - device=%p, pipelineLayout=%p, "
        "renderPass=%p",
        device, pipelineLayout, renderPass);

    // Validate all pipeline info pointers
    HookLog("VulkanBackend::CreatePipeline - Validating pipeline info...");
    HookLog("  pStages=%p (stageCount=%d)", pipelineInfo.pStages, pipelineInfo.stageCount);
    HookLog("  pVertexInputState=%p", pipelineInfo.pVertexInputState);
    HookLog("  pInputAssemblyState=%p", pipelineInfo.pInputAssemblyState);
    HookLog("  pViewportState=%p", pipelineInfo.pViewportState);
    HookLog("  pRasterizationState=%p", pipelineInfo.pRasterizationState);
    HookLog("  pMultisampleState=%p", pipelineInfo.pMultisampleState);
    HookLog("  pColorBlendState=%p", pipelineInfo.pColorBlendState);
    HookLog("  pDynamicState=%p", pipelineInfo.pDynamicState);
    HookLog("  layout=%p", pipelineInfo.layout);
    HookLog("  renderPass=%p", pipelineInfo.renderPass);

    // Check if shader modules are valid
    VkShaderModule vertMod = pipelineInfo.pStages[0].module;
    VkShaderModule fragMod = pipelineInfo.pStages[1].module;
    HookLog("  Vertex shader module=%p, name=%s", vertMod, pipelineInfo.pStages[0].pName);
    HookLog("  Fragment shader module=%p, name=%s", fragMod, pipelineInfo.pStages[1].pName);

    HookLog("VulkanBackend::CreatePipeline - Calling vkCreateGraphicsPipelines...");
    result = disp->fp_vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    HookLog("VulkanBackend::CreatePipeline - vkCreateGraphicsPipelines returned %d", result);
    if (result != VK_SUCCESS) {
        HookLog("VulkanBackend: Failed to create textured pipeline: %d", result);
        disp->fp_vkDestroyShaderModule(device, vertShaderModule, nullptr);
        disp->fp_vkDestroyShaderModule(device, fragShaderModule, nullptr);
        disp->fp_vkDestroyShaderModule(device, fragShaderSolidModule, nullptr);
        return false;
    }
    HookLog("VulkanBackend::CreatePipeline - Textured pipeline created successfully");

    // Create solid pipeline
    VkPipelineShaderStageCreateInfo solidStages[] = {vertStage, fragSolidStage};
    pipelineInfo.pStages = solidStages;

    result = disp->fp_vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipelineSolid);
    if (result != VK_SUCCESS) {
        HookLog("VulkanBackend: Failed to create solid pipeline: %d", result);
        // Textured pipeline was created successfully, just log the error
    }

    // Cleanup shader modules
    disp->fp_vkDestroyShaderModule(device, vertShaderModule, nullptr);
    disp->fp_vkDestroyShaderModule(device, fragShaderModule, nullptr);
    disp->fp_vkDestroyShaderModule(device, fragShaderSolidModule, nullptr);

    pipelineCreated = true;
    HookLog("VulkanBackend: Pipelines created successfully");
    return true;
}

bool VulkanBackend::CreatePipelineForRenderPass(VkRenderPass renderPass) {
    HookLog("VulkanBackend::CreatePipelineForRenderPass - ENTRY, renderPass=%p", renderPass);

    if (!initialized) {
        HookLog("VulkanBackend::CreatePipelineForRenderPass - ERROR: not initialized");
        return false;
    }
    if (pipelineCreated && currentRenderPass == renderPass) {
        HookLog(
            "VulkanBackend::CreatePipelineForRenderPass - Pipeline already "
            "created for this render pass");
        return true;  // Already created for this render pass
    }

    // Destroy existing pipelines if any
    HookLog(
        "VulkanBackend::CreatePipelineForRenderPass - Destroying existing "
        "pipelines...");
    DeviceDispatch* disp = static_cast<DeviceDispatch*>(deviceDispatch);
    if (pipeline != VK_NULL_HANDLE && disp && disp->fp_vkDestroyPipeline) {
        disp->fp_vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (pipelineSolid != VK_NULL_HANDLE && disp && disp->fp_vkDestroyPipeline) {
        disp->fp_vkDestroyPipeline(device, pipelineSolid, nullptr);
        pipelineSolid = VK_NULL_HANDLE;
    }

    currentRenderPass = renderPass;
    HookLog("VulkanBackend::CreatePipelineForRenderPass - Calling CreatePipeline...");
    bool result = CreatePipeline(renderPass);
    HookLog("VulkanBackend::CreatePipelineForRenderPass - CreatePipeline returned %d", result);
    return result;
}

bool VulkanBackend::CreateBuffers() {
    DeviceDispatch* disp = static_cast<DeviceDispatch*>(deviceDispatch);
    if (!disp || !disp->fp_vkCreateBuffer || !disp->fp_vkAllocateMemory || !disp->fp_vkMapMemory)
        return false;

    for (int i = 0; i < kFramePoolSize; i++) {
        // Vertex buffer (1MB)
        vertexBufferSize[i] = 1024 * 1024;
        VkBufferCreateInfo vbInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        vbInfo.size = vertexBufferSize[i];
        vbInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        vbInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult result = disp->fp_vkCreateBuffer(device, &vbInfo, nullptr, &vertexBuffer[i]);
        if (result != VK_SUCCESS) {
            HookLog("VulkanBackend: Failed to create vertex buffer[%d]: %d", i, result);
            return false;
        }

        VkMemoryRequirements vbMemReqs;
        disp->fp_vkGetBufferMemoryRequirements(device, vertexBuffer[i], &vbMemReqs);

        uint32_t vbMemType = FindMemoryType(vbMemReqs.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vbMemType == UINT32_MAX) {
            HookLog("VulkanBackend: Failed to find memory type for vertex buffer");
            return false;
        }

        VkMemoryAllocateInfo vbAllocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        vbAllocInfo.allocationSize = vbMemReqs.size;
        vbAllocInfo.memoryTypeIndex = vbMemType;

        result = disp->fp_vkAllocateMemory(device, &vbAllocInfo, nullptr, &vertexMemory[i]);
        if (result != VK_SUCCESS) {
            HookLog("VulkanBackend: Failed to allocate vertex buffer memory[%d]: %d", i, result);
            return false;
        }

        disp->fp_vkBindBufferMemory(device, vertexBuffer[i], vertexMemory[i], 0);
        disp->fp_vkMapMemory(device, vertexMemory[i], 0, vertexBufferSize[i], 0, &vertexBufferPtr[i]);

        // Index buffer (256KB)
        indexBufferSize[i] = 256 * 1024;
        VkBufferCreateInfo ibInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ibInfo.size = indexBufferSize[i];
        ibInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        ibInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        result = disp->fp_vkCreateBuffer(device, &ibInfo, nullptr, &indexBuffer[i]);
        if (result != VK_SUCCESS) {
            HookLog("VulkanBackend: Failed to create index buffer[%d]: %d", i, result);
            return false;
        }

        VkMemoryRequirements ibMemReqs;
        disp->fp_vkGetBufferMemoryRequirements(device, indexBuffer[i], &ibMemReqs);

        uint32_t ibMemType = FindMemoryType(ibMemReqs.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ibMemType == UINT32_MAX) {
            HookLog("VulkanBackend: Failed to find memory type for index buffer");
            return false;
        }

        VkMemoryAllocateInfo ibAllocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ibAllocInfo.allocationSize = ibMemReqs.size;
        ibAllocInfo.memoryTypeIndex = ibMemType;

        result = disp->fp_vkAllocateMemory(device, &ibAllocInfo, nullptr, &indexMemory[i]);
        if (result != VK_SUCCESS) {
            HookLog("VulkanBackend: Failed to allocate index buffer memory[%d]: %d", i, result);
            return false;
        }

        disp->fp_vkBindBufferMemory(device, indexBuffer[i], indexMemory[i], 0);
        disp->fp_vkMapMemory(device, indexMemory[i], 0, indexBufferSize[i], 0, &indexBufferPtr[i]);
    }

    return true;
}

bool VulkanBackend::CreateFontTexture(int width, int height, const uint8_t* data) {
    DeviceDispatch* disp = static_cast<DeviceDispatch*>(deviceDispatch);
    InstanceDispatch* instDisp = static_cast<InstanceDispatch*>(instanceDispatch);
    if (!disp || !instDisp)
        return false;

    // Create image
    VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VkResult result = disp->fp_vkCreateImage(device, &imageInfo, nullptr, &fontImage);
    if (result != VK_SUCCESS) {
        HookLog("VulkanBackend: Failed to create font image: %d", result);
        return false;
    }

    // Allocate memory
    VkMemoryRequirements memReqs;
    disp->fp_vkGetImageMemoryRequirements(device, fontImage, &memReqs);

    uint32_t memType = FindMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memType == UINT32_MAX) {
        HookLog("VulkanBackend: Failed to find memory type for font image");
        return false;
    }

    VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = memType;

    result = disp->fp_vkAllocateMemory(device, &allocInfo, nullptr, &fontMemory);
    if (result != VK_SUCCESS) {
        HookLog("VulkanBackend: Failed to allocate font image memory: %d", result);
        return false;
    }

    disp->fp_vkBindImageMemory(device, fontImage, fontMemory, 0);

    // Create staging buffer
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize imageSize = width * height;

    VkBufferCreateInfo stagingInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    stagingInfo.size = imageSize;
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    result = disp->fp_vkCreateBuffer(device, &stagingInfo, nullptr, &stagingBuffer);
    if (result != VK_SUCCESS) {
        HookLog("VulkanBackend: Failed to create staging buffer: %d", result);
        return false;
    }

    VkMemoryRequirements stagingMemReqs;
    disp->fp_vkGetBufferMemoryRequirements(device, stagingBuffer, &stagingMemReqs);

    uint32_t stagingMemType = FindMemoryType(
        stagingMemReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo stagingAllocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    stagingAllocInfo.allocationSize = stagingMemReqs.size;
    stagingAllocInfo.memoryTypeIndex = stagingMemType;

    result = disp->fp_vkAllocateMemory(device, &stagingAllocInfo, nullptr, &stagingMemory);
    if (result != VK_SUCCESS) {
        HookLog("VulkanBackend: Failed to allocate staging memory: %d", result);
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    disp->fp_vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

    // Copy data to staging buffer (extract alpha channel from RGBA)
    void* stagingPtr = nullptr;
    disp->fp_vkMapMemory(device, stagingMemory, 0, imageSize, 0, &stagingPtr);
    if (stagingPtr && data) {
        // data is RGBA (4 bytes per pixel), extract only alpha (4th byte)
        const uint8_t* src = data;
        uint8_t* dst = (uint8_t*)stagingPtr;
        for (uint32_t y = 0; y < (uint32_t)height; y++) {
            for (uint32_t x = 0; x < (uint32_t)width; x++) {
                dst[y * width + x] = src[(y * width + x) * 4 + 3];  // Alpha channel
            }
        }
    }
    disp->fp_vkUnmapMemory(device, stagingMemory);

    // Create command buffer for upload
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

    result = disp->fp_vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool);
    if (result != VK_SUCCESS) {
        HookLog("VulkanBackend: Failed to create command pool: %d", result);
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
        return false;
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cmdAllocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdAllocInfo.commandPool = cmdPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;

    disp->fp_vkAllocateCommandBuffers(device, &cmdAllocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    disp->fp_vkBeginCommandBuffer(cmd, &beginInfo);

    // Transition image to transfer dst
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = fontImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                  0, nullptr, 1, &barrier);

    // Copy buffer to image
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};

    disp->fp_vkCmdCopyBufferToImage(cmd, stagingBuffer, fontImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition image to shader read only
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                  nullptr, 0, nullptr, 1, &barrier);

    disp->fp_vkEndCommandBuffer(cmd);

    // Submit and wait
    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    disp->fp_vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    disp->fp_vkQueueWaitIdle(queue);

    // Cleanup staging resources
    disp->fp_vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
    disp->fp_vkDestroyCommandPool(device, cmdPool, nullptr);
    disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
    disp->fp_vkFreeMemory(device, stagingMemory, nullptr);

    // Create image view
    VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = fontImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    result = disp->fp_vkCreateImageView(device, &viewInfo, nullptr, &fontImageView);
    if (result != VK_SUCCESS) {
        HookLog("VulkanBackend: Failed to create font image view: %d", result);
        return false;
    }

    // Create sampler
    VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;

    result = disp->fp_vkCreateSampler(device, &samplerInfo, nullptr, &sampler);
    if (result != VK_SUCCESS) {
        HookLog("VulkanBackend: Failed to create sampler: %d", result);
        return false;
    }

    // Create descriptor pool
    VkDescriptorPoolSize descPoolSize = {};
    descPoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descPoolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo descPoolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descPoolInfo.poolSizeCount = 1;
    descPoolInfo.pPoolSizes = &descPoolSize;
    descPoolInfo.maxSets = 1;

    result = disp->fp_vkCreateDescriptorPool(device, &descPoolInfo, nullptr, &descriptorPool);
    if (result != VK_SUCCESS) {
        HookLog("VulkanBackend: Failed to create descriptor pool: %d", result);
        return false;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo setAllocInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setAllocInfo.descriptorPool = descriptorPool;
    setAllocInfo.descriptorSetCount = 1;
    setAllocInfo.pSetLayouts = &descriptorSetLayout;

    result = disp->fp_vkAllocateDescriptorSets(device, &setAllocInfo, &descriptorSet);
    if (result != VK_SUCCESS) {
        HookLog("VulkanBackend: Failed to allocate descriptor set: %d", result);
        return false;
    }

    // Update descriptor set
    VkDescriptorImageInfo descImageInfo = {};
    descImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descImageInfo.imageView = fontImageView;
    descImageInfo.sampler = sampler;

    VkWriteDescriptorSet descriptorWrite = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    descriptorWrite.dstSet = descriptorSet;
    descriptorWrite.dstBinding = 1;  // Matches layout binding=1
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &descImageInfo;

    disp->fp_vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);

    HookLog("VulkanBackend: Font texture created (%dx%d)", width, height);
    return true;
}

void VulkanBackend::SetRenderContext(VkCommandBuffer cmdBuffer, VkRenderPass renderPass, VkFramebuffer framebuffer,
                                     VkExtent2D extent) {
    currentCmdBuffer = cmdBuffer;
    currentRenderPass = renderPass;
    currentFramebuffer = framebuffer;
    currentExtent = extent;
}

void VulkanBackend::Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                           const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) {
    // Fast-path validation (no logging in hot path)
    if (!initialized || !deviceDispatch || currentCmdBuffer == VK_NULL_HANDLE || currentRenderPass == VK_NULL_HANDLE)
        return;

    DeviceDispatch* disp = static_cast<DeviceDispatch*>(deviceDispatch);
    if (!disp || vertices.empty() || indices.empty() || commands.empty())
        return;

    // Ensure pipeline is created for current render pass
    if (!pipelineCreated) {
        if (!CreatePipelineForRenderPass(currentRenderPass))
            return;
    }

    if (pipeline == VK_NULL_HANDLE)
        return;

    // Advance to the next pool slot for this frame
    int slot = frameIdx.fetch_add(1, std::memory_order_relaxed) % kFramePoolSize;

    // Check buffer sizes
    size_t vertexDataSize = vertices.size() * sizeof(DrawVertex);
    size_t indexDataSize = indices.size() * sizeof(uint16_t);
    if (vertexDataSize > vertexBufferSize[slot] || indexDataSize > indexBufferSize[slot])
        return;

    // Update vertex buffer
    if (vertexBufferPtr[slot]) {
        memcpy(vertexBufferPtr[slot], vertices.data(), vertexDataSize);
    }

    // Update index buffer
    if (indexBufferPtr[slot]) {
        memcpy(indexBufferPtr[slot], indices.data(), indexDataSize);
    }

    // Set viewport (standard setup - shader handles NDC conversion)
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)viewportWidth;
    viewport.height = (float)viewportHeight;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    disp->fp_vkCmdSetViewport(currentCmdBuffer, 0, 1, &viewport);

    // Set scissor
    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {(uint32_t)viewportWidth, (uint32_t)viewportHeight};
    disp->fp_vkCmdSetScissor(currentCmdBuffer, 0, 1, &scissor);

    // Bind vertex buffer
    VkDeviceSize vertexOffset = 0;
    disp->fp_vkCmdBindVertexBuffers(currentCmdBuffer, 0, 1, &vertexBuffer[slot], &vertexOffset);

    // Bind index buffer
    disp->fp_vkCmdBindIndexBuffer(currentCmdBuffer, indexBuffer[slot], 0, VK_INDEX_TYPE_UINT16);

    // Push constants: viewport size + HDR params
    float pushConstants[4] = {(float)viewportWidth, (float)viewportHeight, (float)hdrMode, paperWhiteNits};
    disp->fp_vkCmdPushConstants(currentCmdBuffer, pipelineLayout,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushConstants),
                                pushConstants);

    if (pipelineLayout == VK_NULL_HANDLE || descriptorSet == VK_NULL_HANDLE)
        return;

    // Bind descriptor set (font texture)
    disp->fp_vkCmdBindDescriptorSets(currentCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1,
                                     &descriptorSet, 0, nullptr);

    // Draw commands with pipeline binding cache
    VkPipeline lastBoundPipeline = VK_NULL_HANDLE;
    for (const auto& cmd : commands) {
        VkPipeline pipelineToUse = cmd.useTexture ? pipeline : pipelineSolid;
        if (pipelineToUse == VK_NULL_HANDLE)
            pipelineToUse = pipeline;
        if (pipelineToUse == VK_NULL_HANDLE)
            continue;

        if (pipelineToUse != lastBoundPipeline) {
            disp->fp_vkCmdBindPipeline(currentCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineToUse);
            lastBoundPipeline = pipelineToUse;
        }

        disp->fp_vkCmdDrawIndexed(currentCmdBuffer, cmd.indexCount, 1, cmd.indexOffset, 0, 0);
    }
}

uint32_t VulkanBackend::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    InstanceDispatch* instDisp = static_cast<InstanceDispatch*>(instanceDispatch);
    if (!instDisp || !instDisp->fp_vkGetPhysicalDeviceMemoryProperties)
        return UINT32_MAX;

    VkPhysicalDeviceMemoryProperties memProperties;
    instDisp->fp_vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    return UINT32_MAX;
}

}  // namespace CustomOverlay
