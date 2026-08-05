#include "layer_capture_internal.h"


bool CreateD3D11InteropDevice(IDXGIAdapter* adapter,  ID3D11Device** ppDevice,  ID3D11DeviceContext** ppContext) {


    // Load the NATIVE d3d11.dll from System32 to bypass DXVK's replacement.
    // When DXVK is active, the process-level d3d11.dll is DXVK's Vulkan-backed
    // implementation whose shared handles are incompatible with Vulkan import.
    char systemDir[MAX_PATH];
    GetSystemDirectoryA(systemDir, MAX_PATH);
    char dxgiPath[MAX_PATH];
    snprintf(dxgiPath, MAX_PATH, "%s\\dxgi.dll", systemDir);
    char d3d11Path[MAX_PATH];
    snprintf(d3d11Path, MAX_PATH, "%s\\d3d11.dll", systemDir);

    HMODULE hDXGI = LoadLibraryA(dxgiPath);
    if (!hDXGI) {
        hDXGI = ce::security::LoadSystemLibrary(L"dxgi.dll");
    }

    HMODULE hD3D11 = LoadLibraryA(d3d11Path);
    if (!hD3D11) {
        // Fallback to default search (non-DXVK scenarios)
        hD3D11 = ce::security::LoadSystemLibrary(L"d3d11.dll");
    }
    if (!hD3D11)
        return false;

    if (hDXGI) {
        // Ensure imports inside the loaded d3d11 module resolve against the
        // system dxgi module rather than DXVK's already-loaded dxgi.dll.
        RedirectModuleImports(hD3D11, "dxgi.dll", hDXGI);
    } else {
        LayerLog("Vulkan Layer: [Warn] Failed to load system dxgi.dll for D3D11 interop import redirection");
    }

    char loadedD3D11Path[MAX_PATH] = {};
    char loadedDXGIPath[MAX_PATH] = {};
    GetModuleFileNameA(hD3D11, loadedD3D11Path, MAX_PATH);
    if (hDXGI) {
        GetModuleFileNameA(hDXGI, loadedDXGIPath, MAX_PATH);
    }
    LayerLog("Vulkan Layer: D3D11 interop modules: d3d11=%s dxgi=%s", loadedD3D11Path,
             hDXGI ? loadedDXGIPath : "<unavailable>");

    PFN_D3D11_CREATE_DEVICE createFn = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
    if (!createFn)
        return false;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = createFn(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                          1, D3D11_SDK_VERSION, ppDevice, &featureLevel, ppContext);

    return SUCCEEDED(hr);

}

uint64_t MakeLuidKey(const LUID& luid) {


    return (static_cast<uint64_t>(luid.HighPart) << 32) | static_cast<uint32_t>(luid.LowPart);

}

bool IsSpecificDxvkWrapperLoaded(const char* dllName) {


    if (!IsDllFromProject(dllName, "dxvk")) {
        return false;
    }
    return GetModuleHandleA(dllName) != nullptr;

}

VulkanCaptureInteropMode DetectVulkanInteropMode() {


    const bool dxvkD3D11 = IsSpecificDxvkWrapperLoaded("d3d11.dll");
    const bool dxvkD3D9 = IsSpecificDxvkWrapperLoaded("d3d9.dll");

    if (dxvkD3D11) {
        return VulkanCaptureInteropMode::kDxvkD3D11;
    }
    if (dxvkD3D9) {
        return VulkanCaptureInteropMode::kDxvkD3D9;
    }
    return VulkanCaptureInteropMode::kNative;

}

const char* VulkanInteropModeToString(VulkanCaptureInteropMode mode) {


    switch (mode) {
        case VulkanCaptureInteropMode::kNative:
            return "native";
        case VulkanCaptureInteropMode::kDxvkD3D11:
            return "dxvk-d3d11";
        case VulkanCaptureInteropMode::kDxvkD3D9:
            return "dxvk-d3d9";
    }
    return "unknown";

}

D3D11InteropDevice* GetOrCreateD3D11Device(const LUID& luid) {


    uint64_t key = MakeLuidKey(luid);

    for (auto it = layer_capture_g_D3D11Devices.begin(); it != layer_capture_g_D3D11Devices.end();) {
        if (it->luidKey == key) {
            if (it->valid)
                return &(*it);
            it = layer_capture_g_D3D11Devices.erase(it);
            continue;
        }
        ++it;
    }

    // Load the NATIVE dxgi.dll from System32 to bypass DXVK's replacement.
    // DXVK replaces both d3d11.dll and dxgi.dll; we need the real DXGI factory
    // to enumerate physical adapters and create real D3D11 shared textures.
    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID riid, void** ppFactory);
    PFN_CreateDXGIFactory1 nativeCreateFactory = nullptr;

    char systemDir[MAX_PATH];
    GetSystemDirectoryA(systemDir, MAX_PATH);
    char dxgiPath[MAX_PATH];
    snprintf(dxgiPath, MAX_PATH, "%s\\dxgi.dll", systemDir);

    HMODULE hDxgi = LoadLibraryA(dxgiPath);
    if (hDxgi) {
        nativeCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDxgi, "CreateDXGIFactory1");
    }

    ComPtr<IDXGIFactory1> factory;
    HRESULT factoryHr = E_FAIL;
    if (nativeCreateFactory) {
        factoryHr = nativeCreateFactory(IID_PPV_ARGS(&factory));
    }
    if (FAILED(factoryHr)) {
        // Fallback to default (non-DXVK scenarios)
        factoryHr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    }
    if (FAILED(factoryHr))
        return nullptr;

    ComPtr<IDXGIAdapter> adapter;
    for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);
        if (desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart) {
            LayerLog("Vulkan Layer: Interop adapter: '%ls' VendorId=%x DeviceId=%x LUID=%08x:%08x", desc.Description,
                     desc.VendorId, desc.DeviceId, desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
            ID3D11Device* device = nullptr;
            ID3D11DeviceContext* context = nullptr;
            if (CreateD3D11InteropDevice(adapter.Get(), &device, &context)) {
                D3D11InteropDevice newDev = {};
                newDev.luidKey = key;
                newDev.device = device;
                newDev.context = context;
                newDev.valid = true;
                layer_capture_g_D3D11Devices.push_back(newDev);
                return &layer_capture_g_D3D11Devices.back();
            }
            return nullptr;
        }
    }

    return nullptr;

}

uint32_t VkFormatToDXGI(VkFormat vkFormat) {


    switch (vkFormat) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            // Map SRGB to UNORM: same byte layout, avoids SRGB shared-texture compatibility issues
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            // Map SRGB to UNORM: same byte layout, avoids SRGB shared-texture compatibility issues
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }

}

VkFormat NormalizeVkFormat(VkFormat fmt) {


    switch (fmt) {
        case VK_FORMAT_B8G8R8A8_SRGB:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return VK_FORMAT_R8G8B8A8_UNORM;
        default:
            return fmt;
    }

}

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
