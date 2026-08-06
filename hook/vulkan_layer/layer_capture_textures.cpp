#include "layer_capture_internal.h"


bool CreateSharedTextures(D3D11InteropDevice* interopDev,  VkDevice vkDev,  DeviceDispatch* disp, 
                                 VkPhysicalDevice physDev,  const LUID& luid,  uint32_t width,  uint32_t height, 
                                 uint32_t vkFormat,  SharedTextureEntry& entry) {


    const uint32_t kTextureCount = SHARED_TEXTURE_SLOT_COUNT;

    entry.vkDevice = vkDev;
    entry.luidKey = MakeLuidKey(luid);
    entry.width = width;
    entry.height = height;
    entry.vkFormat = vkFormat;

    entry.textures.assign(kTextureCount, nullptr);
    entry.textureHandles.assign(kTextureCount, nullptr);
    entry.textureHandlesAreNt = false;
    entry.vkImages.assign(kTextureCount, VK_NULL_HANDLE);
    entry.vkMemories.assign(kTextureCount, VK_NULL_HANDLE);

    auto resetAttemptResources = [&]() {
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
            if (entry.textureHandlesAreNt && handle) {
                CloseHandle(handle);
            }
            handle = nullptr;
        }
        for (auto*& tex : entry.textures) {
            if (tex) {
                tex->Release();
                tex = nullptr;
            }
        }
    };

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = (DXGI_FORMAT)VkFormatToDXGI(vkFormat);
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool useNtIpcHandles = (attempt == 0);
        resetAttemptResources();
        entry.textureHandlesAreNt = useNtIpcHandles;

        std::vector<HANDLE> vkImportHandles(kTextureCount, nullptr);
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        if (useNtIpcHandles) {
            texDesc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        }

        bool createFailed = false;
        for (uint32_t i = 0; i < kTextureCount; i++) {
            HRESULT hr = interopDev->device->CreateTexture2D(&texDesc, nullptr, &entry.textures[i]);
            if (FAILED(hr)) {
                LayerLog(
                    "Vulkan Layer: [Error] Failed to create D3D11 texture %d "
                    "(hr=0x%08X, ntIpc=%d)",
                    i, hr, useNtIpcHandles ? 1 : 0);
                createFailed = true;
                break;
            }

            HANDLE ipcHandle = nullptr;
            if (useNtIpcHandles) {
                ComPtr<IDXGIResource1> dxgiRes1;
                if (FAILED(entry.textures[i]->QueryInterface(IID_PPV_ARGS(&dxgiRes1))) || !dxgiRes1) {
                    LayerLog(
                        "Vulkan Layer: [Warn] IDXGIResource1 unavailable for texture "
                        "%d, fallback to KMT-only sharing",
                        i);
                    createFailed = true;
                    break;
                }
                hr = dxgiRes1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                  nullptr, &ipcHandle);
                if (FAILED(hr) || !ipcHandle) {
                    LayerLog(
                        "Vulkan Layer: [Warn] Failed to create NT IPC handle %d "
                        "(hr=0x%08X), fallback to KMT-only sharing",
                        i, hr);
                    createFailed = true;
                    break;
                }
                entry.textureHandles[i] = ipcHandle;

                HANDLE importHandle = nullptr;
                hr = dxgiRes1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                  nullptr, &importHandle);
                if (FAILED(hr) || !importHandle) {
                    LayerLog(
                        "Vulkan Layer: [Warn] Failed to create NT import handle %d "
                        "(hr=0x%08X), fallback to KMT-only sharing",
                        i, hr);
                    createFailed = true;
                    break;
                }
                vkImportHandles[i] = importHandle;
            } else {
                ComPtr<IDXGIResource> dxgiRes;
                HANDLE kmtHandle = nullptr;
                if (FAILED(entry.textures[i]->QueryInterface(IID_PPV_ARGS(&dxgiRes))) || !dxgiRes ||
                    FAILED(dxgiRes->GetSharedHandle(&kmtHandle)) || !kmtHandle) {
                    LayerLog(
                        "Vulkan Layer: [Warn] Failed to get KMT handle for texture "
                        "%d, aborting",
                        i);
                    createFailed = true;
                    break;
                }
                vkImportHandles[i] = kmtHandle;
                entry.textureHandles[i] = kmtHandle;

                // Validate KMT handle by opening it locally on the same device
                ID3D11Texture2D* validateTex = nullptr;
                HRESULT openHr = interopDev->device->OpenSharedResource(kmtHandle, IID_PPV_ARGS(&validateTex));
                if (SUCCEEDED(openHr) && validateTex) {
                    D3D11_TEXTURE2D_DESC openDesc;
                    validateTex->GetDesc(&openDesc);
                    LayerLog("Vulkan Layer: KMT handle %p validated locally (%dx%d fmt=%d)", kmtHandle, openDesc.Width,
                             openDesc.Height, openDesc.Format);
                    validateTex->Release();
                } else {
                    LayerLog("Vulkan Layer: [Warn] KMT handle %p FAILED local validation (hr=0x%08X)", kmtHandle,
                             openHr);
                }
            }
        }

        if (createFailed) {
            for (auto& vkHandle : vkImportHandles) {
                if (useNtIpcHandles && vkHandle) {
                    CloseHandle(vkHandle);
                }
            }
            if (useNtIpcHandles) {
                LayerLog("Vulkan Layer: Falling back to KMT-only texture handles");
                continue;
            }
            resetAttemptResources();
            return false;
        }

        bool importFailed = false;
        VkResult importError = VK_SUCCESS;
        const char* importStage = "none";
        for (uint32_t i = 0; i < kTextureCount; i++) {
            VkExternalMemoryImageCreateInfo extInfo = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
            extInfo.handleTypes = useNtIpcHandles ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT
                                                  : VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

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

            VkResult vkRes = disp->fp_vkCreateImage(vkDev, &imgInfo, nullptr, &entry.vkImages[i]);
            if (vkRes != VK_SUCCESS) {
                importFailed = true;
                importError = vkRes;
                importStage = "vkCreateImage";
                break;
            }

            VkMemoryRequirements memReq;
            disp->fp_vkGetImageMemoryRequirements(vkDev, entry.vkImages[i], &memReq);

            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
            VkImportMemoryWin32HandleInfoKHR importInfo = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
            importInfo.handleType = useNtIpcHandles ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT
                                                    : VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
            importInfo.handle = vkImportHandles[i];
            VkMemoryDedicatedAllocateInfo dedicatedInfo = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
            dedicatedInfo.image = entry.vkImages[i];
            importInfo.pNext = &dedicatedInfo;

            VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &importInfo};
            allocInfo.allocationSize = memReq.size;

            VkPhysicalDeviceMemoryProperties memProps;
            InstanceDispatch* instDisp = VulkanLayerState::Get().GetInstanceDispatch(
                VulkanLayerState::Get().GetInstanceFromPhysicalDevice(physDev));
            instDisp->fp_vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);
            uint32_t memType = 0xFFFFFFFF;
            if (!SelectImportedWin32MemoryType(disp, vkDev, importInfo.handleType, vkImportHandles[i],
                                               memReq.memoryTypeBits, memProps, &memType)) {
                importFailed = true;
                importError = VK_ERROR_INVALID_EXTERNAL_HANDLE;
                importStage = "vkGetMemoryWin32HandlePropertiesKHR";
                break;
            }
            allocInfo.memoryTypeIndex = memType;

            vkRes = disp->fp_vkAllocateMemory(vkDev, &allocInfo, nullptr, &entry.vkMemories[i]);
            if (vkRes != VK_SUCCESS) {
                importFailed = true;
                importError = vkRes;
                importStage = "vkAllocateMemory";
                break;
            }

            if (useNtIpcHandles && vkImportHandles[i]) {
                // Importing a Win32 memory handle does not transfer handle
                // ownership to Vulkan. Close CE's dedicated import duplicate;
                // the separate IPC handle remains live for the media process.
                CloseHandle(vkImportHandles[i]);
                vkImportHandles[i] = nullptr;
            }

            vkRes = disp->fp_vkBindImageMemory(vkDev, entry.vkImages[i], entry.vkMemories[i], 0);
            if (vkRes != VK_SUCCESS) {
                importFailed = true;
                importError = vkRes;
                importStage = "vkBindImageMemory";
                break;
            }
        }

        for (auto& vkHandle : vkImportHandles) {
            if (useNtIpcHandles && vkHandle) {
                CloseHandle(vkHandle);
            }
        }

        if (!importFailed) {
            if (useNtIpcHandles) {
                LayerLog("Vulkan Layer: Using NT IPC handles with NT Vulkan import");
            } else {
                // Fallback path for layer-owned textures: always create encoder relay textures.
                // When encoder-owned textures are not ready yet, publishing these imported KMT handles
                // directly can leave the media process with un-openable handles (seen in Trine 3 DXVK).
                // The relay textures keep the Vulkan import local and expose dedicated encoder handles/fence.
                entry.ipcTextures.assign(kTextureCount, nullptr);
                entry.ipcHandles.assign(kTextureCount, nullptr);
                entry.hasIpcRelay = false;

                bool ipcReady = false;
                for (int ipcAttempt = 0; ipcAttempt < 2 && !ipcReady; ++ipcAttempt) {
                    const bool useNtRelayHandles = (ipcAttempt != 0);
                    entry.ipcHandlesAreNt = useNtRelayHandles;

                    D3D11_TEXTURE2D_DESC ipcDesc = texDesc;
                    // SHARED and SHARED_KEYEDMUTEX are mutually exclusive.
                    // Use plain SHARED for KMT relay handles; use NTHANDLE+KEYEDMUTEX for NT relay handles.
                    ipcDesc.MiscFlags =
                        useNtRelayHandles
                            ? (D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
                            : D3D11_RESOURCE_MISC_SHARED;

                    bool ipcFailed = false;
                    for (uint32_t i = 0; i < kTextureCount; i++) {
                        HRESULT hr = interopDev->device->CreateTexture2D(&ipcDesc, nullptr, &entry.ipcTextures[i]);
                        if (FAILED(hr)) {
                            LayerLog("Vulkan Layer: [Error] Failed to create IPC relay texture %d (hr=0x%08X)", i, hr);
                            ipcFailed = true;
                            break;
                        }

                        HANDLE ipcHandle = nullptr;
                        if (useNtRelayHandles) {
                            ComPtr<IDXGIResource1> dxgiRes1;
                            hr = entry.ipcTextures[i]->QueryInterface(IID_PPV_ARGS(&dxgiRes1));
                            if (FAILED(hr) || !dxgiRes1) {
                                LayerLog("Vulkan Layer: [Warn] IDXGIResource1 unavailable for NT IPC texture %d", i);
                                ipcFailed = true;
                                break;
                            }

                            hr = dxgiRes1->CreateSharedHandle(
                                nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &ipcHandle);
                            if (FAILED(hr) || !ipcHandle) {
                                LayerLog(
                                    "Vulkan Layer: [Warn] Failed to create NT IPC handle for texture %d "
                                    "(hr=0x%08X)",
                                    i, hr);
                                ipcFailed = true;
                                break;
                            }
                        } else {
                            ComPtr<IDXGIResource> dxgiRes;
                            hr = entry.ipcTextures[i]->QueryInterface(IID_PPV_ARGS(&dxgiRes));
                            if (FAILED(hr) || !dxgiRes) {
                                LayerLog("Vulkan Layer: [Error] IDXGIResource unavailable for KMT IPC texture %d", i);
                                ipcFailed = true;
                                break;
                            }

                            hr = dxgiRes->GetSharedHandle(&ipcHandle);
                            if (FAILED(hr) || !ipcHandle) {
                                LayerLog(
                                    "Vulkan Layer: [Error] Failed to get KMT IPC handle for texture %d "
                                    "(hr=0x%08X)",
                                    i, hr);
                                ipcFailed = true;
                                break;
                            }
                        }
                        entry.ipcHandles[i] = ipcHandle;
                    }

                    if (!ipcFailed) {
                        ipcReady = true;
                        break;
                    }

                    // Clean up partially created IPC relay resources before fallback/retry.
                    for (auto& handle : entry.ipcHandles) {
                        if (entry.ipcHandlesAreNt && handle) {
                            CloseHandle(handle);
                        }
                        handle = nullptr;
                    }
                    for (auto*& tex : entry.ipcTextures) {
                        if (tex) {
                            tex->Release();
                            tex = nullptr;
                        }
                    }

                    if (!useNtRelayHandles) {
                        LayerLog("Vulkan Layer: [Warn] KMT IPC relay handles unavailable, falling back to NT relay");
                    }
                }

                if (!ipcReady) {
                    LayerLog("Vulkan Layer: [Warn] IPC relay creation failed, encoder may not receive frames");
                } else {
                    entry.hasIpcRelay = true;
                    LayerLog("Vulkan Layer: Created %d IPC relay textures (%s handles for encoder)", kTextureCount,
                             entry.ipcHandlesAreNt ? "NT" : "KMT");
                }
            }
            entry.valid = true;
            return true;
        }

        LayerLog(
            "Vulkan Layer: [Warn] Vulkan import failed (ntIpc=%d, stage=%s, "
            "vkResult=%d), %s",
            useNtIpcHandles ? 1 : 0, importStage, importError,
            useNtIpcHandles ? "retrying with KMT-only handles" : "aborting");
        if (!useNtIpcHandles) {
            resetAttemptResources();
            return false;
        }
    }

    resetAttemptResources();
    return false;

}


bool CreateVulkanNativeSharedTextures(VkDevice vkDev,  DeviceDispatch* disp,  VkPhysicalDevice physDev, 
                                             const LUID& luid,  uint32_t width,  uint32_t height,  uint32_t vkFormat, 
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

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
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


SharedTextureEntry* GetOrCreateSharedTextures(VkDevice vkDev,  DeviceDispatch* disp,  VkPhysicalDevice physDev, 
                                                     const LUID& luid,  uint32_t width,  uint32_t height, 
                                                     uint32_t vkFormat) {


    std::lock_guard<std::mutex> lock(layer_capture_g_InteropMutex);
    uint64_t luidKey = MakeLuidKey(luid);

    // Check existing cache
    for (auto it = layer_capture_g_TextureCache.begin(); it != layer_capture_g_TextureCache.end();) {
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
            layer_capture_g_TextureCache.push_back(std::move(newEntry));
            return &layer_capture_g_TextureCache.back();
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
            layer_capture_g_TextureCache.push_back(std::move(nativeEntry));
            return &layer_capture_g_TextureCache.back();
        }
        LayerLog("Vulkan Layer: [Error] Vulkan-native fallback also failed");
    }

    return nullptr;

}


void DestroySharedTextureEntryResources(SharedTextureEntry& entry,  DeviceDispatch* disp) {


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
