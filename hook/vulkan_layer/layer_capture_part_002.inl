
// Create Vulkan-native images with D3D11_TEXTURE NT export for cross-process sharing.
// Used when DXVK is active: bypasses D3D11 entirely since DXVK's D3D11 produces
// internal handles that native D3D11 in the encoder can't open.
// Uses NT handles (not KMT) because KMT handles from vkGetMemoryWin32HandleKHR are raw
// WDDM allocation handles without D3D11 resource metadata - D3D11's OpenSharedResource
// returns E_INVALIDARG for them. NT handles via D3D11_TEXTURE_BIT carry proper resource
// metadata and are openable by D3D11's OpenSharedResource1 after DuplicateHandle.
static bool CreateVulkanNativeSharedTextures(VkDevice vkDev, DeviceDispatch* disp, VkPhysicalDevice physDev,
                                             const LUID& luid, uint32_t width, uint32_t height, uint32_t vkFormat,
                                             SharedTextureEntry& entry) {
    const uint32_t kTextureCount = SHARED_TEXTURE_SLOT_COUNT;

    entry.vkDevice = vkDev;
    entry.luidKey = MakeLuidKey(luid);
    entry.width = width;
    entry.height = height;
    entry.vkFormat = vkFormat;

    entry.vkImages.assign(kTextureCount, VK_NULL_HANDLE);
    entry.vkMemories.assign(kTextureCount, VK_NULL_HANDLE);
    entry.textureHandles.assign(kTextureCount, nullptr);
    entry.textureHandlesAreNt = true;  // NT handles for cross-process via DuplicateHandle

    if (!disp->fp_vkGetMemoryWin32HandleKHR) {
        LayerLog("Vulkan Layer: [Error] vkGetMemoryWin32HandleKHR not available for Vulkan-native export");
        return false;
    }

    InstanceDispatch* instDisp =
        VulkanLayerState::Get().GetInstanceDispatch(VulkanLayerState::Get().GetInstanceFromPhysicalDevice(physDev));
    if (!instDisp) {
        LayerLog("Vulkan Layer: [Error] No instance dispatch for Vulkan-native export");
        return false;
    }
    VkPhysicalDeviceMemoryProperties memProps;
    instDisp->fp_vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);

    // Security attributes for cross-process NT handle access
    SECURITY_ATTRIBUTES secAttr = {};
    secAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    secAttr.bInheritHandle = FALSE;
    secAttr.lpSecurityDescriptor = nullptr;  // Default security descriptor

    bool failed = false;
    for (uint32_t i = 0; i < kTextureCount; i++) {
        VkExternalMemoryImageCreateInfo extInfo = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

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

        VkResult vkRes = disp->fp_vkCreateImage(vkDev, &imgInfo, nullptr, &entry.vkImages[i]);
        if (vkRes != VK_SUCCESS) {
            LayerLog("Vulkan Layer: [Error] Failed to create exportable image %d (vkResult=%d)", i, vkRes);
            failed = true;
            break;
        }

        VkMemoryRequirements memReq;
        disp->fp_vkGetImageMemoryRequirements(vkDev, entry.vkImages[i], &memReq);

        // NT handle export requires VkExportMemoryWin32HandleInfoKHR with security attributes
        VkExportMemoryWin32HandleInfoKHR exportWin32MemInfo = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        exportWin32MemInfo.pAttributes = &secAttr;
        exportWin32MemInfo.dwAccess = DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE;
        exportWin32MemInfo.name = nullptr;

        VkExportMemoryAllocateInfo exportAllocInfo = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exportAllocInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        exportAllocInfo.pNext = &exportWin32MemInfo;

        VkMemoryDedicatedAllocateInfo dedicatedInfo = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicatedInfo.pNext = &exportAllocInfo;
        dedicatedInfo.image = entry.vkImages[i];

        VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicatedInfo};
        allocInfo.allocationSize = memReq.size;

        uint32_t memType = 0xFFFFFFFF;
        for (uint32_t j = 0; j < memProps.memoryTypeCount; j++) {
            if ((memReq.memoryTypeBits & (1 << j)) &&
                (memProps.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memType = j;
                break;
            }
        }
        if (memType == 0xFFFFFFFF) {
            for (uint32_t j = 0; j < memProps.memoryTypeCount; j++) {
                if (memReq.memoryTypeBits & (1 << j)) {
                    memType = j;
                    break;
                }
            }
        }
        if (memType == 0xFFFFFFFF) {
            LayerLog("Vulkan Layer: [Error] No memory type for exportable image %d (bits=0x%x)", i,
                     memReq.memoryTypeBits);
            failed = true;
            break;
        }
        allocInfo.memoryTypeIndex = memType;

        vkRes = disp->fp_vkAllocateMemory(vkDev, &allocInfo, nullptr, &entry.vkMemories[i]);
        if (vkRes != VK_SUCCESS) {
            LayerLog("Vulkan Layer: [Error] Failed to allocate exportable memory %d (vkResult=%d)", i, vkRes);
            failed = true;
            break;
        }

        vkRes = disp->fp_vkBindImageMemory(vkDev, entry.vkImages[i], entry.vkMemories[i], 0);
        if (vkRes != VK_SUCCESS) {
            LayerLog("Vulkan Layer: [Error] Failed to bind exportable memory %d (vkResult=%d)", i, vkRes);
            failed = true;
            break;
        }

        VkMemoryGetWin32HandleInfoKHR getHandleInfo = {VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
        getHandleInfo.memory = entry.vkMemories[i];
        getHandleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

        vkRes = disp->fp_vkGetMemoryWin32HandleKHR(vkDev, &getHandleInfo, &entry.textureHandles[i]);
        if (vkRes != VK_SUCCESS || !entry.textureHandles[i]) {
            LayerLog("Vulkan Layer: [Error] Failed to export NT handle %d (vkResult=%d)", i, vkRes);
            failed = true;
            break;
        }
    }

    if (failed) {
        for (auto& img : entry.vkImages) {
            if (img != VK_NULL_HANDLE) {
                disp->fp_vkDestroyImage(vkDev, img, nullptr);
                img = VK_NULL_HANDLE;
            }
        }
        for (auto& mem : entry.vkMemories) {
            if (mem != VK_NULL_HANDLE) {
                disp->fp_vkFreeMemory(vkDev, mem, nullptr);
                mem = VK_NULL_HANDLE;
            }
        }
        for (auto& handle : entry.textureHandles) {
            if (handle)
                CloseHandle(handle);
            handle = nullptr;
        }
        return false;
    }

    entry.valid = true;
    LayerLog("Vulkan Layer: Created %d Vulkan-native exportable textures (NT handles, DXVK bypass)", kTextureCount);
    for (uint32_t i = 0; i < kTextureCount; i++) {
        LayerLog("Vulkan Layer: Vulkan-native texture %d NT handle = %p", i, entry.textureHandles[i]);
    }
    return true;
}

static SharedTextureEntry* GetOrCreateSharedTextures(VkDevice vkDev, DeviceDispatch* disp, VkPhysicalDevice physDev,
                                                     const LUID& luid, uint32_t width, uint32_t height,
                                                     uint32_t vkFormat) {
    std::lock_guard<std::mutex> lock(g_InteropMutex);
    uint64_t luidKey = MakeLuidKey(luid);

    // Check existing cache
    for (auto it = g_TextureCache.begin(); it != g_TextureCache.end();) {
        if (it->vkDevice == vkDev && it->luidKey == luidKey && it->width == width && it->height == height &&
            it->vkFormat == vkFormat) {
            if (it->valid)
                return &(*it);
            // Invalid entries may still be referenced by an in-flight capture
            // submission. Keep them retired until device teardown instead of
            // releasing Vulkan/D3D resources on the Present thread.
        }
        ++it;
    }

    // Under DXVK, prefer encoder-owned KMT textures for the true zero-copy path.
    // If those are unavailable, fall back to D3D11 interop textures and expose
    // dedicated relay handles/fences for the encoder instead of publishing the
    // imported Vulkan-side KMT handles directly.
    const VulkanCaptureInteropMode interopMode = DetectVulkanInteropMode();
    const bool allowDxvkEncoderTextures = (interopMode == VulkanCaptureInteropMode::kDxvkD3D11);

    // Get D3D11 device
    D3D11InteropDevice* interopDev = GetOrCreateD3D11Device(luid);
    if (interopDev && interopDev->valid) {
        // Create new textures
        SharedTextureEntry newEntry;
        if (CreateSharedTextures(interopDev, vkDev, disp, physDev, luid, width, height, vkFormat, newEntry)) {
            g_TextureCache.push_back(std::move(newEntry));
            return &g_TextureCache.back();
        }

        LayerLog("Vulkan Layer: [Warn] D3D11 interop texture setup failed%s",
                 allowDxvkEncoderTextures ? ", trying Vulkan-native fallback" : "");
    } else if (!allowDxvkEncoderTextures) {
        return nullptr;
    }

    // Vulkan-native fallback: only useful for non-DXVK or when D3D11 interop fails completely
    if (allowDxvkEncoderTextures) {
        LayerLog("Vulkan Layer: DXVK d3d11 interop mode active - trying Vulkan-native shared textures as fallback");
        SharedTextureEntry nativeEntry;
        if (CreateVulkanNativeSharedTextures(vkDev, disp, physDev, luid, width, height, vkFormat, nativeEntry)) {
            LayerLog("Vulkan Layer: Vulkan-native fallback succeeded (exported NT handles)");
            g_TextureCache.push_back(std::move(nativeEntry));
            return &g_TextureCache.back();
        }
        LayerLog("Vulkan Layer: [Error] Vulkan-native fallback also failed");
    }

    return nullptr;
}

static void DestroySharedTextureEntryResources(SharedTextureEntry& entry, DeviceDispatch* disp) {
    if (!disp || entry.vkDevice == VK_NULL_HANDLE)
        return;

    for (VkImage& image : entry.vkImages) {
        if (image != VK_NULL_HANDLE) {
            disp->fp_vkDestroyImage(entry.vkDevice, image, nullptr);
            image = VK_NULL_HANDLE;
        }
    }
    for (VkDeviceMemory& memory : entry.vkMemories) {
        if (memory != VK_NULL_HANDLE) {
            disp->fp_vkFreeMemory(entry.vkDevice, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
    }
    for (HANDLE& handle : entry.textureHandles) {
        if (entry.textureHandlesAreNt && handle)
            CloseHandle(handle);
        handle = nullptr;
    }
    for (ID3D11Texture2D*& texture : entry.textures) {
        if (texture)
            texture->Release();
        texture = nullptr;
    }
    for (HANDLE& handle : entry.ipcHandles) {
        if (entry.ipcHandlesAreNt && handle)
            CloseHandle(handle);
        handle = nullptr;
    }
    for (ID3D11Texture2D*& texture : entry.ipcTextures) {
        if (texture)
            texture->Release();
        texture = nullptr;
    }
    entry.valid = false;
}

// ============================================================================
// Per-Device Capture State
// ============================================================================

struct VulkanCaptureState {
    bool initialized = false;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint64_t luidKey = 0;
    uint32_t captureWidth = 0;
    uint32_t captureHeight = 0;
    uint32_t captureFormat = 0;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

    struct CommandResources {
        VkCommandPool pool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> buffers;
    };
    std::unordered_map<uint32_t, CommandResources> commandResourcesByQueueFamily;
    std::vector<VkFence> copyFences;
    std::vector<VkSemaphore> signalSemaphores;
    std::vector<bool> presentedImages;
    std::array<uint64_t, SHARED_TEXTURE_SLOT_COUNT> relayCompletionValues{};
    std::array<bool, SHARED_TEXTURE_SLOT_COUNT> sharedImageInitialized{};
    bool relayCompletionUnknown = false;

    // Cross-API Synchronization
    VkSemaphore timelineSemaphore = VK_NULL_HANDLE;
    uint64_t currentFenceValue = 0;
    uint64_t captureFrameCounter = 0;  // Monotonic counter for slot rotation
    HANDLE sharedFenceHandle = NULL;

    // D3D11 relay: fence and context for IPC relay copy (KMT→NT)
    ID3D11Fence* d3d11Fence = nullptr;
    ID3D11DeviceContext4* d3d11Context4 = nullptr;

    // D3D11-native shared fence for cross-process IPC with encoder
    // The Vulkan opaque fence (d3d11Fence) works in-process but can't be opened cross-process.
    // This separate D3D11 fence has a standard shared handle the encoder can open.
    ID3D11Fence* d3d11IpcFence = nullptr;
    HANDLE ipcFenceHandle = nullptr;

    uint64_t nextEncoderImportRetryFrame = 0;
    VulkanCaptureInteropMode interopMode = VulkanCaptureInteropMode::kNative;
};

static std::mutex g_CaptureMutex;
static std::unordered_map<VkDevice, VulkanCaptureState> g_CaptureStates;
static std::vector<VulkanCaptureState> g_RetiredCaptureStates;

static bool CaptureStateCopiesComplete(const VulkanCaptureState& state, DeviceDispatch* disp) {
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

static void DestroyCaptureStateResources(VulkanCaptureState& state, DeviceDispatch* disp) {
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

static bool SelectImportedWin32MemoryType(DeviceDispatch* disp, VkDevice device,
                                          VkExternalMemoryHandleTypeFlagBits handleType, HANDLE handle,
                                          uint32_t imageMemoryTypeBits,
                                          const VkPhysicalDeviceMemoryProperties& memoryProperties,
                                          uint32_t* memoryTypeIndex) {
    if (!disp || !disp->fp_vkGetMemoryWin32HandlePropertiesKHR || !handle || !memoryTypeIndex)
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
            *memoryTypeIndex = i;
            return true;
        }
    }
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((compatibleTypes & (1u << i)) != 0) {
            *memoryTypeIndex = i;
            return true;
        }
    }
    LayerLog("Vulkan Layer: No compatible memory type for imported Win32 handle (imageBits=0x%x handleBits=0x%x)",
             imageMemoryTypeBits, handleProperties.memoryTypeBits);
    return false;
}

static VulkanCaptureState::CommandResources* EnsureCaptureCommandResources(VulkanCaptureState& state,
                                                                           DeviceDispatch* disp, VkDevice device,
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

// Helper to get LUID from Vulkan Physical Device
static bool GetLUIDFromPhysicalDevice(VkPhysicalDevice physDev, LUID* outLuid) {
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

static bool ImportEncoderKmtTextures(VkDevice device, DeviceDispatch* disp, uint64_t luidKey, uint32_t width,
                                     uint32_t height, uint32_t vkFormat, SharedMemoryLayout* mem,
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

void InitializeCapture(VkDevice device, VkSwapchainKHR swapchain, VkFormat format, VkColorSpaceKHR colorSpace,
                       VkExtent2D extent,
                       uint32_t imageCount) {
    LayerLog(
        "Vulkan Layer: InitializeCapture(device=%p, images=%d, size=%dx%d, "
        "vkFormat=%d)",
        device, imageCount, extent.width, extent.height, format);

    std::lock_guard<std::mutex> lock(g_CaptureMutex);

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp) {
        LayerLog("Vulkan Layer: [Error] No dispatch for device %p", device);
        return;
    }
    if (!disp->captureInteropEnabled) {
        static std::atomic<int> s_captureCapabilityLogCount{0};
        if (s_captureCapabilityLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            LayerLog(
                "Vulkan Layer: Capture disabled for device %p because required Win32 external-memory/fence "
                "features were unavailable at device creation",
                device);
        }
        return;
    }
    if (swapchain == VK_NULL_HANDLE || imageCount == 0 || extent.width == 0 || extent.height == 0) {
        LayerLog("Vulkan Layer: [Error] Invalid capture initialization inputs (swapchain=%p images=%u size=%ux%u)",
                 swapchain, imageCount, extent.width, extent.height);
        return;
    }
    if (VkFormatToDXGI(format) == DXGI_FORMAT_UNKNOWN) {
        LayerLog("Vulkan Layer: [Error] Capture format %d has no byte-compatible DXGI shared format", format);
        return;
    }
    auto* mem = g_IPCClient.GetSharedMem();
    const auto presentationEncoding = ce::presentation_color::ResolveVulkan(format, colorSpace);
