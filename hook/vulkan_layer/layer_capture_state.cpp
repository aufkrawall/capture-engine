#include "layer_capture_internal.h"


bool CaptureStateCopiesComplete(const VulkanCaptureState& state,  DeviceDispatch* disp) {


    if (!disp)
        return false;
    if (state.relayCompletionUnknown)
        return false;
    for (VkFence fence : state.copyFences) {
        if (fence != VK_NULL_HANDLE && disp->fp_vkWaitForFences(state.device, 1, &fence, VK_TRUE, 0) != VK_SUCCESS)
            return false;
    }

    uint64_t latestRelayValue = 0;
    for (uint64_t value : state.relayCompletionValues)
        latestRelayValue = (std::max)(latestRelayValue, value);
    if (latestRelayValue != 0) {
        ID3D11Fence* relayFence = state.d3d11IpcFence ? state.d3d11IpcFence : state.d3d11Fence;
        if (!relayFence || relayFence->GetCompletedValue() < latestRelayValue)
            return false;
    }
    return true;

}
void DestroyCaptureStateResources(VulkanCaptureState& state,  DeviceDispatch* disp) {


    if (!disp || state.device == VK_NULL_HANDLE)
        return;

    for (VkFence& fence : state.copyFences) {
        if (fence != VK_NULL_HANDLE) {
            disp->fp_vkDestroyFence(state.device, fence, nullptr);
            fence = VK_NULL_HANDLE;
        }
    }
    for (VkSemaphore& semaphore : state.signalSemaphores) {
        if (semaphore != VK_NULL_HANDLE) {
            disp->fp_vkDestroySemaphore(state.device, semaphore, nullptr);
            semaphore = VK_NULL_HANDLE;
        }
    }
    if (state.timelineSemaphore != VK_NULL_HANDLE) {
        disp->fp_vkDestroySemaphore(state.device, state.timelineSemaphore, nullptr);
        state.timelineSemaphore = VK_NULL_HANDLE;
    }
    for (auto& [queueFamilyIndex, resources] : state.commandResourcesByQueueFamily) {
        (void)queueFamilyIndex;
        if (resources.pool != VK_NULL_HANDLE) {
            disp->fp_vkDestroyCommandPool(state.device, resources.pool, nullptr);
            resources.pool = VK_NULL_HANDLE;
        }
        resources.buffers.clear();
    }
    state.commandResourcesByQueueFamily.clear();
    if (state.sharedFenceHandle) {
        CloseHandle(state.sharedFenceHandle);
        state.sharedFenceHandle = nullptr;
    }
    if (state.ipcFenceHandle) {
        CloseHandle(state.ipcFenceHandle);
        state.ipcFenceHandle = nullptr;
    }
    if (state.d3d11IpcFence) {
        state.d3d11IpcFence->Release();
        state.d3d11IpcFence = nullptr;
    }
    if (state.d3d11Fence) {
        state.d3d11Fence->Release();
        state.d3d11Fence = nullptr;
    }
    if (state.d3d11Context4) {
        state.d3d11Context4->Release();
        state.d3d11Context4 = nullptr;
    }
    state.initialized = false;

}
bool SelectImportedWin32MemoryType(DeviceDispatch* disp,  VkDevice device, 
                                          VkExternalMemoryHandleTypeFlagBits handleType,  HANDLE handle, 
                                          uint32_t imageMemoryTypeBits, 
                                          const VkPhysicalDeviceMemoryProperties& memoryProperties, 
                                          uint32_t* layer_capture_memoryTypeIndex) {


    if (!disp || !disp->fp_vkGetMemoryWin32HandlePropertiesKHR || !handle || !layer_capture_memoryTypeIndex)
        return false;

    VkMemoryWin32HandlePropertiesKHR handleProperties = {VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    const VkResult propertiesResult =
        disp->fp_vkGetMemoryWin32HandlePropertiesKHR(device, handleType, handle, &handleProperties);
    if (propertiesResult != VK_SUCCESS) {
        LayerLog("Vulkan Layer: vkGetMemoryWin32HandlePropertiesKHR failed (type=0x%x result=%d)", handleType,
                 propertiesResult);
        return false;
    }

    const uint32_t compatibleTypes = imageMemoryTypeBits & handleProperties.memoryTypeBits;
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((compatibleTypes & (1u << i)) != 0 &&
            (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            *layer_capture_memoryTypeIndex = i;
            return true;
        }
    }
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((compatibleTypes & (1u << i)) != 0) {
            *layer_capture_memoryTypeIndex = i;
            return true;
        }
    }
    LayerLog("Vulkan Layer: No compatible memory type for imported Win32 handle (imageBits=0x%x handleBits=0x%x)",
             imageMemoryTypeBits, handleProperties.memoryTypeBits);
    return false;

}
VulkanCaptureState::CommandResources* EnsureCaptureCommandResources(VulkanCaptureState& state, 
                                                                           DeviceDispatch* disp,  VkDevice device, 
                                                                           uint32_t queueFamilyIndex) {


    if (!disp || queueFamilyIndex == VK_QUEUE_FAMILY_IGNORED || state.copyFences.empty())
        return nullptr;

    auto existing = state.commandResourcesByQueueFamily.find(queueFamilyIndex);
    if (existing != state.commandResourcesByQueueFamily.end())
        return &existing->second;

    VulkanCaptureState::CommandResources resources;

    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, queueFamilyIndex};
    if (disp->fp_vkCreateCommandPool(device, &poolInfo, nullptr, &resources.pool) != VK_SUCCESS ||
        resources.pool == VK_NULL_HANDLE) {
        LayerLog("Vulkan Layer: Failed to create capture command pool for queue family %u", queueFamilyIndex);
        return nullptr;
    }

    resources.buffers.resize(state.copyFences.size());
    VkCommandBufferAllocateInfo cbInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, resources.pool,
                                          VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          static_cast<uint32_t>(resources.buffers.size())};
    if (disp->fp_vkAllocateCommandBuffers(device, &cbInfo, resources.buffers.data()) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Failed to allocate capture command buffers for queue family %u", queueFamilyIndex);
        disp->fp_vkDestroyCommandPool(device, resources.pool, nullptr);
        return nullptr;
    }

    auto [inserted, wasInserted] = state.commandResourcesByQueueFamily.emplace(queueFamilyIndex, std::move(resources));
    return wasInserted ? &inserted->second : nullptr;

}
bool GetLUIDFromPhysicalDevice(VkPhysicalDevice physDev,  LUID* outLuid) {


    InstanceDispatch* instDisp =
        VulkanLayerState::Get().GetInstanceDispatch(VulkanLayerState::Get().GetInstanceFromPhysicalDevice(physDev));
    if (!instDisp || !instDisp->fp_vkGetPhysicalDeviceProperties2)
        return false;

    VkPhysicalDeviceIDProperties idProps = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &idProps;

    instDisp->fp_vkGetPhysicalDeviceProperties2(physDev, &props2);

    if (idProps.deviceLUIDValid) {
        memcpy(outLuid, idProps.deviceLUID, sizeof(LUID));
        return true;
    }
    return false;

}
bool ImportEncoderKmtTextures(VkDevice device,  DeviceDispatch* disp,  uint64_t luidKey,  uint32_t width, 
                                     uint32_t height,  uint32_t vkFormat,  SharedMemoryLayout* mem, 
                                     SharedTextureEntry* outEntry, 
                                     HANDLE outKmtHandles[ENCODER_TEXTURE_SLOT_COUNT]) {


    if (!disp || !mem || !outEntry || !outKmtHandles)
        return false;

    if (!mem->encoderTextures.kmtReady.load(std::memory_order_acquire))
        return false;

    const uint32_t expectedDxgiFormat = VkFormatToDXGI((VkFormat)vkFormat);
    const uint32_t encoderWidth = mem->encoderTextures.GetWidth();
    const uint32_t encoderHeight = mem->encoderTextures.GetHeight();
    const uint32_t encoderFormat = mem->encoderTextures.GetFormat();

    if (encoderWidth != width || encoderHeight != height) {
        LayerLog("Vulkan Layer: Encoder KMT size mismatch (%ux%u vs %ux%u)", encoderWidth, encoderHeight, width,
                 height);
        return false;
    }
    if (encoderFormat != 0 && encoderFormat != expectedDxgiFormat) {
        LayerLog("Vulkan Layer: Encoder KMT format mismatch (existing=%u, need=%u)", encoderFormat, expectedDxgiFormat);
        return false;
    }

    bool allValid = true;
    for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; i++) {
        outKmtHandles[i] = (HANDLE)mem->encoderTextures.GetKmtTextureHandle(i);
        if (!outKmtHandles[i]) {
            allValid = false;
            break;
        }
    }
    if (!allValid)
        return false;

    LayerLog("Vulkan Layer: Encoder KMT handles received, importing into Vulkan");
    for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; i++) {
        LayerLog("Vulkan Layer: Encoder KMT handle %d = %p", i, outKmtHandles[i]);
    }

    SharedTextureEntry newEntry;
    newEntry.vkDevice = device;
    newEntry.luidKey = luidKey;
    newEntry.width = width;
    newEntry.height = height;
    newEntry.vkFormat = vkFormat;
    newEntry.vkImages.assign(ENCODER_TEXTURE_SLOT_COUNT, VK_NULL_HANDLE);
    newEntry.vkMemories.assign(ENCODER_TEXTURE_SLOT_COUNT, VK_NULL_HANDLE);
    newEntry.textureHandles.assign(ENCODER_TEXTURE_SLOT_COUNT, nullptr);
    newEntry.textureHandlesAreNt = false;
    newEntry.hasIpcRelay = false;

    auto cleanupImportedEntry = [&]() {
        for (auto& img : newEntry.vkImages) {
            if (img != VK_NULL_HANDLE) {
                disp->fp_vkDestroyImage(device, img, nullptr);
            }
        }
        for (auto& mem2 : newEntry.vkMemories) {
            if (mem2 != VK_NULL_HANDLE) {
                disp->fp_vkFreeMemory(device, mem2, nullptr);
            }
        }
    };

    for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; i++) {
        VkExternalMemoryImageCreateInfo extInfo = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &extInfo};
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = (VkFormat)vkFormat;
        imgInfo.extent = {width, height, 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkResult vkRes = disp->fp_vkCreateImage(device, &imgInfo, nullptr, &newEntry.vkImages[i]);
        if (vkRes != VK_SUCCESS) {
            LayerLog("Vulkan Layer: [Error] vkCreateImage failed for encoder KMT %d: %d", i, vkRes);
            cleanupImportedEntry();
            return false;
        }

        VkMemoryRequirements memReq;
        disp->fp_vkGetImageMemoryRequirements(device, newEntry.vkImages[i], &memReq);

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        VkImportMemoryWin32HandleInfoKHR importInfo = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
        importInfo.handle = outKmtHandles[i];

        VkMemoryDedicatedAllocateInfo dedicatedInfo = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicatedInfo.image = newEntry.vkImages[i];
        importInfo.pNext = &dedicatedInfo;

        VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &importInfo};
        allocInfo.allocationSize = memReq.size;

        VkPhysicalDeviceMemoryProperties memProps;
        InstanceDispatch* instDisp = VulkanLayerState::Get().GetInstanceDispatch(
            VulkanLayerState::Get().GetInstanceFromPhysicalDevice(disp->physicalDevice));
        instDisp->fp_vkGetPhysicalDeviceMemoryProperties(disp->physicalDevice, &memProps);

        uint32_t memType = 0xFFFFFFFF;
        if (!SelectImportedWin32MemoryType(disp, device, importInfo.handleType, outKmtHandles[i], memReq.memoryTypeBits,
                                           memProps, &memType)) {
            cleanupImportedEntry();
            return false;
        }
        allocInfo.memoryTypeIndex = memType;

        vkRes = disp->fp_vkAllocateMemory(device, &allocInfo, nullptr, &newEntry.vkMemories[i]);
        if (vkRes != VK_SUCCESS) {
            LayerLog("Vulkan Layer: [Error] vkAllocateMemory failed for encoder KMT %d: %d", i, vkRes);
            cleanupImportedEntry();
            return false;
        }

        vkRes = disp->fp_vkBindImageMemory(device, newEntry.vkImages[i], newEntry.vkMemories[i], 0);
        if (vkRes != VK_SUCCESS) {
            LayerLog("Vulkan Layer: [Error] vkBindImageMemory failed for encoder KMT %d: %d", i, vkRes);
            cleanupImportedEntry();
            return false;
        }

        newEntry.textureHandles[i] = outKmtHandles[i];
    }

    newEntry.valid = true;
    *outEntry = std::move(newEntry);
    return true;

}
