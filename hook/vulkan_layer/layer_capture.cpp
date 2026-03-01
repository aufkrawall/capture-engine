/**
 * VK_LAYER_CE_overlay - Zero-Copy Capture via D3D11 Interop
 *
 * Architecture:
 * - Global D3D11 interop device per GPU (keyed by LUID)
 * - Global shared texture cache per (LUID, resolution)
 * - Vulkan devices map to D3D11 devices via LUID matching
 * - No recreation during runtime, no resource leaks
 */

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vector>
#include "../../common/shared_defs.h"
#include "../common/hook_common.h"
#include "layer_main.h"
#include "vulkan_layer.h"
using Microsoft::WRL::ComPtr;

// ============================================================================
// Global D3D11 Interop Device Cache (per GPU)
// ============================================================================

struct D3D11InteropDevice {
    uint64_t luidKey = 0;  // Combined LUID as single key
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    bool valid = false;
};

struct SharedTextureEntry {
    uint64_t luidKey = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t vkFormat = 0;

    std::vector<ID3D11Texture2D*> textures;  // 4 textures for double/triple buffering
    std::vector<HANDLE> textureHandles;      // Vulkan import handles (KMT or NT)
    bool textureHandlesAreNt = false;
    std::vector<VkImage> vkImages;           // Vulkan imported images
    std::vector<VkDeviceMemory> vkMemories;  // Vulkan imported memories

    // IPC relay: separate shared textures for encoder (not Vulkan-imported)
    // When KMT handles are used for Vulkan import, they become un-openable cross-process.
    // These relay textures use independent KMT handles that remain openable by the encoder.
    std::vector<ID3D11Texture2D*> ipcTextures;
    std::vector<HANDLE> ipcHandles;
    bool ipcHandlesAreNt = false;
    bool hasIpcRelay = false;

    bool valid = false;
};

// Global caches
static std::mutex g_InteropMutex;
static std::vector<D3D11InteropDevice> g_D3D11Devices;
static std::vector<SharedTextureEntry> g_TextureCache;

// ============================================================================
// D3D11 Device Creation
// ============================================================================

static bool CreateD3D11InteropDevice(IDXGIAdapter* adapter, ID3D11Device** ppDevice, ID3D11DeviceContext** ppContext) {
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
        hDXGI = LoadLibraryA("dxgi.dll");
    }

    HMODULE hD3D11 = LoadLibraryA(d3d11Path);
    if (!hD3D11) {
        // Fallback to default search (non-DXVK scenarios)
        hD3D11 = LoadLibraryA("d3d11.dll");
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

static uint64_t MakeLuidKey(const LUID& luid) {
    return (static_cast<uint64_t>(luid.HighPart) << 32) | static_cast<uint32_t>(luid.LowPart);
}

// Detect DXVK by checking if d3d11.dll is NOT from System32.
// DXVK replaces d3d11.dll in the game directory, and LoadLibraryA returns
// DXVK's module due to Windows DLL name deduplication.
static bool IsDXVKD3D11Active() {
    HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
    if (!hD3D11)
        return false;
    char loadedPath[MAX_PATH] = {};
    GetModuleFileNameA(hD3D11, loadedPath, MAX_PATH);
    char systemDir[MAX_PATH] = {};
    GetSystemDirectoryA(systemDir, MAX_PATH);
    size_t sysLen = strlen(systemDir);
    if (_strnicmp(loadedPath, systemDir, sysLen) == 0 &&
        (loadedPath[sysLen] == '\\' || loadedPath[sysLen] == '/')) {
        return false;  // Real d3d11 from System32
    }
    LayerLog("Vulkan Layer: DXVK d3d11.dll detected at: %s", loadedPath);
    return true;
}

// Unified DXVK detection: checks dxgi.dll, d3d11.dll, and d3d9.dll.
// Returns true if ANY of them are loaded from outside System32.
// This catches both DXVK d3d11 and DXVK d3d9 (e.g. Trine 3 via DXVK d3d9) variants.
static bool IsDXVKActive() {
    char systemDir[MAX_PATH] = {};
    GetSystemDirectoryA(systemDir, MAX_PATH);
    size_t sysLen = strlen(systemDir);

    const char* dllNames[] = {"dxgi.dll", "d3d11.dll", "d3d9.dll"};
    for (const char* dllName : dllNames) {
        HMODULE hMod = GetModuleHandleA(dllName);
        if (!hMod)
            continue;
        char loadedPath[MAX_PATH] = {};
        GetModuleFileNameA(hMod, loadedPath, MAX_PATH);
        if (_strnicmp(loadedPath, systemDir, sysLen) != 0 ||
            (loadedPath[sysLen] != '\\' && loadedPath[sysLen] != '/')) {
            LayerLog("Vulkan Layer: DXVK detected - %s loaded from: %s (not System32)", dllName, loadedPath);
            return true;
        }
    }
    return false;
}

static D3D11InteropDevice* GetOrCreateD3D11Device(const LUID& luid) {
    uint64_t key = MakeLuidKey(luid);

    for (auto it = g_D3D11Devices.begin(); it != g_D3D11Devices.end();) {
        if (it->luidKey == key) {
            if (it->valid)
                return &(*it);
            it = g_D3D11Devices.erase(it);
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
            LayerLog("Vulkan Layer: Interop adapter: '%ls' VendorId=%x DeviceId=%x LUID=%08x:%08x",
                     desc.Description, desc.VendorId, desc.DeviceId,
                     desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
            ID3D11Device* device = nullptr;
            ID3D11DeviceContext* context = nullptr;
            if (CreateD3D11InteropDevice(adapter.Get(), &device, &context)) {
                D3D11InteropDevice newDev = {};
                newDev.luidKey = key;
                newDev.device = device;
                newDev.context = context;
                newDev.valid = true;
                g_D3D11Devices.push_back(newDev);
                return &g_D3D11Devices.back();
            }
            return nullptr;
        }
    }

    return nullptr;
}

// ============================================================================
// Vulkan <-> D3D11 Interop via KMT Handles
// ============================================================================

static uint32_t VkFormatToDXGI(VkFormat vkFormat) {
    switch (vkFormat) {
        case VK_FORMAT_B8G8R8A8_UNORM:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

static bool CreateSharedTextures(D3D11InteropDevice* interopDev, VkDevice vkDev, DeviceDispatch* disp,
                                 VkPhysicalDevice physDev, const LUID& luid, uint32_t width, uint32_t height,
                                 uint32_t vkFormat, SharedTextureEntry& entry) {
    const uint32_t kTextureCount = 4;

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
        for (auto& mem : entry.vkMemories) {
            if (mem != VK_NULL_HANDLE) {
                disp->fp_vkFreeMemory(vkDev, mem, nullptr);
                mem = VK_NULL_HANDLE;
            }
        }
        for (auto& img : entry.vkImages) {
            if (img != VK_NULL_HANDLE) {
                disp->fp_vkDestroyImage(vkDev, img, nullptr);
                img = VK_NULL_HANDLE;
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
                HRESULT openHr =
                    interopDev->device->OpenSharedResource(kmtHandle, IID_PPV_ARGS(&validateTex));
                if (SUCCEEDED(openHr) && validateTex) {
                    D3D11_TEXTURE2D_DESC openDesc;
                    validateTex->GetDesc(&openDesc);
                    LayerLog("Vulkan Layer: KMT handle %p validated locally (%dx%d fmt=%d)",
                             kmtHandle, openDesc.Width, openDesc.Height, openDesc.Format);
                    validateTex->Release();
                } else {
                    LayerLog("Vulkan Layer: [Warn] KMT handle %p FAILED local validation (hr=0x%08X)",
                             kmtHandle, openHr);
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
            allocInfo.memoryTypeIndex = memType;

            vkRes = disp->fp_vkAllocateMemory(vkDev, &allocInfo, nullptr, &entry.vkMemories[i]);
            if (vkRes != VK_SUCCESS) {
                importFailed = true;
                importError = vkRes;
                importStage = "vkAllocateMemory";
                break;
            }

            vkRes = disp->fp_vkBindImageMemory(vkDev, entry.vkImages[i], entry.vkMemories[i], 0);
            if (vkRes != VK_SUCCESS) {
                importFailed = true;
                importError = vkRes;
                importStage = "vkBindImageMemory";
                break;
            }

            if (useNtIpcHandles) {
                // NT handles are consumed by Vulkan import on successful allocation.
                vkImportHandles[i] = nullptr;
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
                // KMT import path succeeded. Under DXVK, skip IPC relay entirely:
                // KMT handles are global WDDM handles that the encoder can open directly
                // via OpenSharedResource. This avoids the relay copy for true zero-copy.
                bool dxvkActive = IsDXVKActive();
                if (dxvkActive) {
                    LayerLog("Vulkan Layer: DXVK active - publishing KMT handles directly (no IPC relay, zero-copy)");
                } else {
                // Create separate IPC relay textures for encoder access.
                // Prefer NT relay handles first for reliable cross-process/cross-bitness transport,
                // then fallback to KMT relay handles if NT relay creation fails.
                entry.ipcTextures.assign(kTextureCount, nullptr);
                entry.ipcHandles.assign(kTextureCount, nullptr);
                entry.hasIpcRelay = false;

                bool ipcReady = false;
                for (int ipcAttempt = 0; ipcAttempt < 2 && !ipcReady; ++ipcAttempt) {
                    const bool useNtRelayHandles = (ipcAttempt == 0);
                    entry.ipcHandlesAreNt = useNtRelayHandles;

                    D3D11_TEXTURE2D_DESC ipcDesc = texDesc;
                    // SHARED and SHARED_KEYEDMUTEX are mutually exclusive.
                    // Use plain SHARED for KMT relay handles; use NTHANDLE+KEYEDMUTEX for NT relay handles.
                    ipcDesc.MiscFlags = useNtRelayHandles
                                            ? (D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                                               D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
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

                            hr = dxgiRes1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                              nullptr, &ipcHandle);
                            if (FAILED(hr) || !ipcHandle) {
                                LayerLog("Vulkan Layer: [Warn] Failed to create NT IPC handle for texture %d (hr=0x%08X)",
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
                                LayerLog("Vulkan Layer: [Error] Failed to get KMT IPC handle for texture %d (hr=0x%08X)",
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

                    if (useNtRelayHandles) {
                        LayerLog("Vulkan Layer: [Warn] NT IPC relay handles unavailable, falling back to KMT relay");
                    }
                }

                if (!ipcReady) {
                    LayerLog("Vulkan Layer: [Warn] IPC relay creation failed, encoder may not receive frames");
                } else {
                    entry.hasIpcRelay = true;
                    LayerLog("Vulkan Layer: Created %d IPC relay textures (%s handles for encoder)", kTextureCount,
                             entry.ipcHandlesAreNt ? "NT" : "KMT");
                }
                }  // end else (!dxvkActive)
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
    const uint32_t kTextureCount = 4;

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

    InstanceDispatch* instDisp = VulkanLayerState::Get().GetInstanceDispatch(
        VulkanLayerState::Get().GetInstanceFromPhysicalDevice(physDev));
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
        VkExportMemoryWin32HandleInfoKHR exportWin32MemInfo = {
            VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
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
        for (auto& mem : entry.vkMemories) {
            if (mem != VK_NULL_HANDLE) {
                disp->fp_vkFreeMemory(vkDev, mem, nullptr);
                mem = VK_NULL_HANDLE;
            }
        }
        for (auto& img : entry.vkImages) {
            if (img != VK_NULL_HANDLE) {
                disp->fp_vkDestroyImage(vkDev, img, nullptr);
                img = VK_NULL_HANDLE;
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
        if (it->luidKey == luidKey && it->width == width && it->height == height && it->vkFormat == vkFormat) {
            if (it->valid)
                return &(*it);
            // Remove invalid entry
            it = g_TextureCache.erase(it);
            continue;
        }
        ++it;
    }

    // Under DXVK, use D3D11 interop with KMT handles (no IPC relay).
    // - Vulkan-native D3D11_TEXTURE_BIT export handles are NOT openable by D3D11
    //   OpenSharedResource1 (E_INVALIDARG) despite successful vkGetMemoryWin32HandleKHR.
    // - D3D11 interop NT import into Vulkan fails (VK_ERROR_OUT_OF_DEVICE_MEMORY).
    // - D3D11 interop KMT import into Vulkan SUCCEEDS.
    // - KMT handles are global WDDM handles the encoder can OpenSharedResource directly.
    // - No IPC relay copy needed = true zero-copy path.
    bool dxvkActive = IsDXVKActive();

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
                 dxvkActive ? ", trying Vulkan-native fallback" : "");
    } else if (!dxvkActive) {
        return nullptr;
    }

    // Vulkan-native fallback: only useful for non-DXVK or when D3D11 interop fails completely
    if (dxvkActive) {
        LayerLog("Vulkan Layer: DXVK active - trying Vulkan-native shared textures as fallback");
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

// ============================================================================
// Per-Device Capture State
// ============================================================================

struct VulkanCaptureState {
    bool initialized = false;
    VkDevice device = VK_NULL_HANDLE;
    uint64_t luidKey = 0;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkFence> copyFences;
    std::vector<VkSemaphore> signalSemaphores;

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
};

static std::mutex g_CaptureMutex;
static std::unordered_map<VkDevice, VulkanCaptureState> g_CaptureStates;

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

void InitializeCapture(VkDevice device, VkSwapchainKHR swapchain, VkFormat format, VkExtent2D extent,
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

    // Get LUID for GPU identification
    LUID luid = {};
    if (!GetLUIDFromPhysicalDevice(disp->physicalDevice, &luid)) {
        LayerLog("Vulkan Layer: [Error] Failed to get LUID from physical device");
        return;
    }
    LayerIPC_SetLUID(static_cast<int32_t>(luid.LowPart), static_cast<int32_t>(luid.HighPart));

    uint64_t luidKey = MakeLuidKey(luid);

    // Check for existing state
    auto it = g_CaptureStates.find(device);
    if (it != g_CaptureStates.end()) {
        if (it->second.initialized && it->second.luidKey == luidKey) {
            LayerLog("Vulkan Layer: Reusing existing capture state for device");
            return;
        }
        g_CaptureStates.erase(it);
    }

    // Get shared textures (creates if needed)
    // For DXVK, we'll try encoder-created KMT textures first (imported into Vulkan)
    SharedTextureEntry* sharedTextures = nullptr;
    auto* mem = g_IPCClient.GetSharedMem();
    bool usingEncoderTextures = false;
    bool dxvkActive = IsDXVKActive();

    if (dxvkActive && mem) {
        // DXVK zero-copy path: import encoder's KMT handles into Vulkan.
        // The D3D11 interop device inside DXVK processes creates textures with
        // invalid shared handles, so we use textures from the encoder process instead.

        // Publish resolution FIRST so the encoder can create textures
        mem->SetWidth(extent.width);
        mem->SetHeight(extent.height);
        mem->SetFormat(VkFormatToDXGI(format));
        LayerLog("Vulkan Layer: DXVK active - published resolution %dx%d fmt=%d, waiting for encoder KMT textures",
                 extent.width, extent.height, VkFormatToDXGI(format));

        const int maxWaitMs = 5000;
        const int checkIntervalMs = 50;
        int waitedMs = 0;

        while (waitedMs < maxWaitMs) {
            if (mem->encoderTextures.kmtReady.load(std::memory_order_acquire)) {
                break;
            }
            Sleep(checkIntervalMs);
            waitedMs += checkIntervalMs;
        }

        if (mem->encoderTextures.kmtReady.load(std::memory_order_acquire)) {
            HANDLE kmtHandles[4];
            bool allValid = true;
            for (int i = 0; i < 4; i++) {
                kmtHandles[i] = (HANDLE)mem->encoderTextures.GetKmtTextureHandle(i);
                if (!kmtHandles[i]) {
                    allValid = false;
                    break;
                }
            }

            if (allValid) {
                LayerLog("Vulkan Layer: Encoder KMT handles received, importing into Vulkan");
                for (int i = 0; i < 4; i++) {
                    LayerLog("Vulkan Layer: Encoder KMT handle %d = %p", i, kmtHandles[i]);
                }

                // Create a SharedTextureEntry by importing encoder KMT handles
                SharedTextureEntry newEntry;
                newEntry.luidKey = MakeLuidKey(luid);
                newEntry.width = extent.width;
                newEntry.height = extent.height;
                newEntry.vkFormat = format;
                newEntry.vkImages.assign(4, VK_NULL_HANDLE);
                newEntry.vkMemories.assign(4, VK_NULL_HANDLE);
                newEntry.textureHandles.assign(4, nullptr);
                newEntry.textureHandlesAreNt = false;
                newEntry.hasIpcRelay = false;

                bool importOk = true;
                for (uint32_t i = 0; i < 4; i++) {
                    VkExternalMemoryImageCreateInfo extInfo = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
                    extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

                    VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &extInfo};
                    imgInfo.imageType = VK_IMAGE_TYPE_2D;
                    imgInfo.format = (VkFormat)format;
                    imgInfo.extent = {extent.width, extent.height, 1};
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
                        importOk = false;
                        break;
                    }

                    VkMemoryRequirements memReq;
                    disp->fp_vkGetImageMemoryRequirements(device, newEntry.vkImages[i], &memReq);

                    VkImportMemoryWin32HandleInfoKHR importInfo = {
                        VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
                    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
                    importInfo.handle = kmtHandles[i];

                    VkMemoryDedicatedAllocateInfo dedicatedInfo = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
                    dedicatedInfo.image = newEntry.vkImages[i];
                    importInfo.pNext = &dedicatedInfo;

                    VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &importInfo};
                    allocInfo.allocationSize = memReq.size;

                    // Find device-local memory type
                    VkPhysicalDeviceMemoryProperties memProps;
                    InstanceDispatch* instDisp = VulkanLayerState::Get().GetInstanceDispatch(
                        VulkanLayerState::Get().GetInstanceFromPhysicalDevice(disp->physicalDevice));
                    instDisp->fp_vkGetPhysicalDeviceMemoryProperties(disp->physicalDevice, &memProps);
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
                    allocInfo.memoryTypeIndex = memType;

                    vkRes = disp->fp_vkAllocateMemory(device, &allocInfo, nullptr, &newEntry.vkMemories[i]);
                    if (vkRes != VK_SUCCESS) {
                        LayerLog("Vulkan Layer: [Error] vkAllocateMemory failed for encoder KMT %d: %d", i, vkRes);
                        importOk = false;
                        break;
                    }

                    vkRes = disp->fp_vkBindImageMemory(device, newEntry.vkImages[i], newEntry.vkMemories[i], 0);
                    if (vkRes != VK_SUCCESS) {
                        LayerLog("Vulkan Layer: [Error] vkBindImageMemory failed for encoder KMT %d: %d", i, vkRes);
                        importOk = false;
                        break;
                    }

                    newEntry.textureHandles[i] = kmtHandles[i];
                }

                if (importOk) {
                    newEntry.valid = true;
                    std::lock_guard<std::mutex> texLock(g_InteropMutex);
                    g_TextureCache.push_back(std::move(newEntry));
                    sharedTextures = &g_TextureCache.back();

                    // Publish encoder KMT handles to ring buffer
                    LayerIPC_SetTextures(kmtHandles, 4, extent.width, extent.height, VkFormatToDXGI(format));
                    // Tell encoder to use its own textures directly
                    mem->useEncoderTextures.store(true, std::memory_order_release);
                    usingEncoderTextures = true;
                    LayerLog("Vulkan Layer: DXVK zero-copy: imported %d encoder KMT textures into Vulkan", 4);
                } else {
                    // Clean up failed import
                    for (auto& mem2 : newEntry.vkMemories) {
                        if (mem2 != VK_NULL_HANDLE)
                            disp->fp_vkFreeMemory(device, mem2, nullptr);
                    }
                    for (auto& img : newEntry.vkImages) {
                        if (img != VK_NULL_HANDLE)
                            disp->fp_vkDestroyImage(device, img, nullptr);
                    }
                    LayerLog("Vulkan Layer: [Warn] Failed to import encoder KMT textures, falling back to interop");
                }
            }
        }

        if (!usingEncoderTextures) {
            LayerLog("Vulkan Layer: [Warn] Encoder KMT textures not available, falling back to D3D11 interop");
        }
    }

    // Fallback: D3D11 interop textures (non-DXVK or when encoder textures unavailable)
    if (!sharedTextures) {
        sharedTextures =
            GetOrCreateSharedTextures(device, disp, disp->physicalDevice, luid, extent.width, extent.height, format);
    }
    if (!sharedTextures || !sharedTextures->valid) {
        LayerLog("Vulkan Layer: [Error] Failed to get shared textures");
        return;
    }

    if (!usingEncoderTextures) {
        if (!dxvkActive && mem) {
            // Non-DXVK: wait for encoder textures (NT handles) for publishing
            const int maxWaitMs = 5000;
            const int checkIntervalMs = 10;
            int waitedMs = 0;

            while (waitedMs < maxWaitMs) {
                if (mem->encoderTextures.ready.load(std::memory_order_acquire)) {
                    HANDLE encoderHandles[4];
                    bool allValid = true;
                    for (int i = 0; i < 4; i++) {
                        encoderHandles[i] = (HANDLE)mem->encoderTextures.GetTextureHandle(i);
                        if (!encoderHandles[i]) {
                            allValid = false;
                            break;
                        }
                    }

                    if (allValid) {
                        LayerIPC_SetTextures(encoderHandles, 4, extent.width, extent.height, VkFormatToDXGI(format));
                        LayerLog("Vulkan Layer: Publishing encoder texture handles");
                        usingEncoderTextures = true;
                        break;
                    }
                }
                Sleep(checkIntervalMs);
                waitedMs += checkIntervalMs;
            }

            if (!usingEncoderTextures) {
                LayerLog("Vulkan Layer: Timeout waiting for encoder textures, falling back to IPC relay");
            }
        }

        // Fallback to our own textures if encoder textures not available
        if (!usingEncoderTextures) {
            if (sharedTextures->hasIpcRelay) {
                LayerIPC_SetTextures(sharedTextures->ipcHandles.data(), (uint32_t)sharedTextures->ipcHandles.size(),
                                     extent.width, extent.height, VkFormatToDXGI(format));
                LayerLog("Vulkan Layer: Publishing IPC relay %s handles to encoder",
                         sharedTextures->ipcHandlesAreNt ? "NT" : "KMT");
            } else {
                LayerIPC_SetTextures(sharedTextures->textureHandles.data(),
                                     (uint32_t)sharedTextures->textureHandles.size(), extent.width, extent.height,
                                     VkFormatToDXGI(format));
                LayerLog("Vulkan Layer: Publishing %s handles to encoder",
                         sharedTextures->textureHandlesAreNt ? "NT" : "KMT");
            }
        }
    }

    VulkanCaptureState state = {};
    state.device = device;
    state.luidKey = luidKey;
    state.initialized = true;

    // Create command pool
    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    if (disp->fp_vkCreateCommandPool(device, &poolInfo, nullptr, &state.commandPool) != VK_SUCCESS) {
        return;
    }

    state.commandBuffers.resize(imageCount);
    VkCommandBufferAllocateInfo cbInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, state.commandPool,
                                          VK_COMMAND_BUFFER_LEVEL_PRIMARY, imageCount};
    disp->fp_vkAllocateCommandBuffers(device, &cbInfo, state.commandBuffers.data());

    state.copyFences.resize(imageCount);
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    for (uint32_t i = 0; i < imageCount; i++) {
        disp->fp_vkCreateFence(device, &fenceInfo, nullptr, &state.copyFences[i]);
    }

    state.signalSemaphores.resize(imageCount);
    VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};
    for (uint32_t i = 0; i < imageCount; i++) {
        disp->fp_vkCreateSemaphore(device, &semInfo, nullptr, &state.signalSemaphores[i]);
    }

    // Create Exportable Timeline Semaphore
    // When IPC relay is NOT active (Vulkan-native / DXVK bypass), use D3D12_FENCE handle type
    // which produces standard D3D fence NT handles that OpenSharedFence can open cross-process.
    // Always prefer D3D12_FENCE: works cross-process (encoder can open it) and cross-API (D3D11 relay can use it).
    // Falls back to OPAQUE_WIN32 below if the Vulkan driver doesn't support D3D12_FENCE semaphores.
    VkExternalSemaphoreHandleTypeFlagBits semHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;

    VkSemaphoreTypeCreateInfo timelineInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineInfo.initialValue = 0;

    // Explicit security attributes for cross-process fence access.
    // Without this, the D3D12_FENCE handle may lack sufficient access rights
    // and OpenSharedFence fails in the encoder process.
    SECURITY_ATTRIBUTES secAttr = {};
    secAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    secAttr.bInheritHandle = FALSE;
    secAttr.lpSecurityDescriptor = nullptr;  // Default security descriptor (full access)

    VkExportSemaphoreWin32HandleInfoKHR exportWin32Info = {
        VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    exportWin32Info.pAttributes = &secAttr;
    exportWin32Info.dwAccess = GENERIC_ALL;
    exportWin32Info.name = nullptr;
    exportWin32Info.pNext = &timelineInfo;

    VkExportSemaphoreCreateInfo exportInfo = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportInfo.handleTypes = semHandleType;
    exportInfo.pNext = &exportWin32Info;

    VkSemaphoreCreateInfo timelineSemInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    timelineSemInfo.pNext = &exportInfo;

    VkResult semResult = disp->fp_vkCreateSemaphore(device, &timelineSemInfo, nullptr, &state.timelineSemaphore);

    // If D3D12_FENCE failed, fall back to OPAQUE_WIN32
    if (semResult != VK_SUCCESS && semHandleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT) {
        LayerLog("Vulkan Layer: D3D12_FENCE semaphore not supported, falling back to OPAQUE_WIN32");
        semHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        exportInfo.handleTypes = semHandleType;
        semResult = disp->fp_vkCreateSemaphore(device, &timelineSemInfo, nullptr, &state.timelineSemaphore);
    }

    if (semResult == VK_SUCCESS) {
        if (disp->fp_vkGetSemaphoreWin32HandleKHR) {
            VkSemaphoreGetWin32HandleInfoKHR getHandleInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
            getHandleInfo.semaphore = state.timelineSemaphore;
            getHandleInfo.handleType = semHandleType;

            HANDLE hFence = nullptr;
            VkResult fenceRes = disp->fp_vkGetSemaphoreWin32HandleKHR(device, &getHandleInfo, &hFence);

            if (fenceRes == VK_SUCCESS && hFence) {
                state.sharedFenceHandle = hFence;
                LayerLog("Vulkan Layer: Created Shared Fence %p (type=%s)", hFence,
                         semHandleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT ? "D3D12_FENCE"
                                                                                             : "OPAQUE_WIN32");
            } else {
                LayerLog("Vulkan Layer: [Error] Failed to get fence handle (vkResult=%d)", fenceRes);
            }
        }
    } else {
        LayerLog("Vulkan Layer: [Error] Failed to create timeline semaphore (vkResult=%d)", semResult);
    }

    // Set up D3D11 relay for IPC if needed (KMT path with relay textures)
    if (sharedTextures->hasIpcRelay && state.sharedFenceHandle) {
        std::lock_guard<std::mutex> interopLock(g_InteropMutex);
        D3D11InteropDevice* interopDev = nullptr;
        for (auto& dev : g_D3D11Devices) {
            if (dev.luidKey == luidKey && dev.valid) {
                interopDev = &dev;
                break;
            }
        }
        if (interopDev) {
            // Get ID3D11Device5 for OpenSharedFence
            ComPtr<ID3D11Device5> device5;
            HRESULT hr = interopDev->device->QueryInterface(IID_PPV_ARGS(&device5));
            if (SUCCEEDED(hr) && device5) {
                hr = device5->OpenSharedFence(state.sharedFenceHandle, IID_PPV_ARGS(&state.d3d11Fence));
                if (SUCCEEDED(hr) && state.d3d11Fence) {
                    LayerLog("Vulkan Layer: Opened D3D11 fence for IPC relay");
                } else {
                    LayerLog("Vulkan Layer: [Error] OpenSharedFence failed (hr=0x%08X)", hr);
                }
            } else {
                LayerLog("Vulkan Layer: [Error] ID3D11Device5 unavailable (hr=0x%08X)", hr);
            }

            // Get ID3D11DeviceContext4 for Wait/Signal
            hr = interopDev->context->QueryInterface(IID_PPV_ARGS(&state.d3d11Context4));
            if (SUCCEEDED(hr) && state.d3d11Context4) {
                LayerLog("Vulkan Layer: IPC relay D3D11 context ready");
            } else {
                LayerLog("Vulkan Layer: [Error] ID3D11DeviceContext4 unavailable (hr=0x%08X)", hr);
                if (state.d3d11Fence) {
                    state.d3d11Fence->Release();
                    state.d3d11Fence = nullptr;
                }
            }

            // Create a D3D11-native shared IPC fence whenever relay is active and D3D11 context is available.
            // Even when Vulkan exports D3D12_FENCE, some driver/runtime combinations can fail OpenSharedFence
            // in the encoder process. A D3D11-created shared fence is generally more interoperable.
            if (state.d3d11Fence && state.d3d11Context4 && device5) {
                hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&state.d3d11IpcFence));
                if (SUCCEEDED(hr) && state.d3d11IpcFence) {
                    SECURITY_ATTRIBUTES ipcFenceSecAttr = {};
                    ipcFenceSecAttr.nLength = sizeof(ipcFenceSecAttr);
                    ipcFenceSecAttr.bInheritHandle = FALSE;
                    ipcFenceSecAttr.lpSecurityDescriptor = nullptr;

                    state.ipcFenceHandle = nullptr;
                    hr = state.d3d11IpcFence->CreateSharedHandle(&ipcFenceSecAttr, GENERIC_ALL, nullptr,
                                                                 &state.ipcFenceHandle);
                    if (SUCCEEDED(hr) && state.ipcFenceHandle) {
                        LayerLog("Vulkan Layer: Created D3D11 IPC fence, handle=%p", state.ipcFenceHandle);
                    } else {
                        LayerLog("Vulkan Layer: [Error] IPC fence CreateSharedHandle failed (hr=0x%08X, handle=%p)",
                                 hr, state.ipcFenceHandle);
                        state.d3d11IpcFence->Release();
                        state.d3d11IpcFence = nullptr;
                    }
                } else {
                    LayerLog("Vulkan Layer: [Error] CreateFence for IPC failed (hr=0x%08X)", hr);
                }
            }
        }
    }

    // Publish the appropriate fence handle for the encoder.
    // In IPC relay mode, avoid publishing Vulkan fence fallback when D3D11 IPC fence is unavailable:
    // several DXVK titles fail OpenSharedFence cross-process for that handle type.
    HANDLE fenceToPublish = nullptr;
    if (state.ipcFenceHandle) {
        fenceToPublish = state.ipcFenceHandle;
    } else if (!sharedTextures->hasIpcRelay) {
        fenceToPublish = state.sharedFenceHandle;
    } else if (state.sharedFenceHandle) {
        LayerLog("Vulkan Layer: [Warn] IPC relay active but D3D11 IPC fence unavailable; clearing shared fence handle");
    }

    mem = g_IPCClient.GetSharedMem();
    if (fenceToPublish) {
        if (mem) {
            if (mem->encoderTextures.ready.load(std::memory_order_acquire)) {
                mem->encoderTextures.SetFenceHandle((uint64_t)fenceToPublish);
                LayerLog("Vulkan Layer: Set D3D11 IPC fence handle %p to encoderTextures", fenceToPublish);
            } else {
                LayerIPC_SetFence(fenceToPublish);
                LayerLog("Vulkan Layer: Set D3D11 IPC fence handle %p", fenceToPublish);
            }
        }
    } else if (mem) {
        if (mem->encoderTextures.ready.load(std::memory_order_acquire)) {
            mem->encoderTextures.SetFenceHandle(0);
        } else {
            LayerIPC_SetFence(nullptr);
        }
    }

    LayerLog("Vulkan Layer: Zero-Copy Capture Initialized (%dx%d)", extent.width, extent.height);
    g_CaptureStates[device] = state;
}

void CleanupCapture(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    auto it = g_CaptureStates.find(device);
    if (it != g_CaptureStates.end()) {
        uint64_t luidKey = it->second.luidKey;
        DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
        if (disp) {
            for (VkFence fence : it->second.copyFences)
                disp->fp_vkDestroyFence(device, fence, nullptr);
            for (VkSemaphore sem : it->second.signalSemaphores)
                disp->fp_vkDestroySemaphore(device, sem, nullptr);
            if (it->second.timelineSemaphore)
                disp->fp_vkDestroySemaphore(device, it->second.timelineSemaphore, nullptr);
            if (it->second.sharedFenceHandle)
                CloseHandle(it->second.sharedFenceHandle);
            if (it->second.d3d11IpcFence)
                it->second.d3d11IpcFence->Release();
            if (it->second.ipcFenceHandle)
                CloseHandle(it->second.ipcFenceHandle);
            if (it->second.d3d11Fence)
                it->second.d3d11Fence->Release();
            if (it->second.d3d11Context4)
                it->second.d3d11Context4->Release();
            disp->fp_vkDestroyCommandPool(device, it->second.commandPool, nullptr);
        }
        g_CaptureStates.erase(it);

        // CRITICAL FIX: Clean up texture cache entries for this device
        // This prevents memory leaks of D3D11 textures and Vulkan images
        std::lock_guard<std::mutex> interopLock(g_InteropMutex);
        for (auto& entry : g_TextureCache) {
            if (entry.luidKey == luidKey) {
                // Release D3D11 textures
                for (auto* tex : entry.textures) {
                    if (tex)
                        tex->Release();
                }
                entry.textures.clear();

                // Close shared handles
                for (auto& handle : entry.textureHandles) {
                    if (entry.textureHandlesAreNt && handle)
                        CloseHandle(handle);
                }
                entry.textureHandles.clear();
                entry.textureHandlesAreNt = false;

                // Destroy Vulkan images and memories
                for (auto& img : entry.vkImages) {
                    if (img != VK_NULL_HANDLE) {
                        disp->fp_vkDestroyImage(device, img, nullptr);
                    }
                }
                entry.vkImages.clear();

                for (auto& mem : entry.vkMemories) {
                    if (mem != VK_NULL_HANDLE) {
                        disp->fp_vkFreeMemory(device, mem, nullptr);
                    }
                }
                entry.vkMemories.clear();

                // Clean up IPC relay textures
                for (auto& handle : entry.ipcHandles) {
                    if (entry.ipcHandlesAreNt && handle)
                        CloseHandle(handle);
                }
                entry.ipcHandles.clear();
                for (auto*& tex : entry.ipcTextures) {
                    if (tex)
                        tex->Release();
                }
                entry.ipcTextures.clear();
                entry.ipcHandlesAreNt = false;
                entry.hasIpcRelay = false;

                entry.valid = false;
                LayerLog("Vulkan Layer: Cleaned up texture cache entry for LUID %llx", luidKey);
            }
        }
    }
}

VkSemaphore GetCaptureSemaphore(VkDevice device, uint32_t imageIndex) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    auto it = g_CaptureStates.find(device);
    if (it != g_CaptureStates.end() && it->second.initialized) {
        if (imageIndex < it->second.signalSemaphores.size()) {
            return it->second.signalSemaphores[imageIndex];
        }
    }
    return VK_NULL_HANDLE;
}

void CaptureFrame(VkDevice device, VkQueue queue, VkImage srcImage, uint32_t imageIndex, VkSemaphore waitSemaphore,
                  VkSemaphore signalSemaphore) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);

    auto it = g_CaptureStates.find(device);
    if (it == g_CaptureStates.end() || !it->second.initialized)
        return;

    // Check if we should throttle capture (encoder is falling behind)
    if (g_IPCClient.GetSharedMem()) {
        if (g_IPCClient.GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
            return;
        }
    }

    VulkanCaptureState& state = it->second;
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp)
        return;

    // Get shared textures from cache
    SharedTextureEntry* sharedTextures = nullptr;
    for (auto& entry : g_TextureCache) {
        if (entry.luidKey == state.luidKey && entry.valid) {
            sharedTextures = &entry;
            break;
        }
    }
    if (!sharedTextures || !sharedTextures->valid)
        return;

    // Use monotonic counter for slot rotation to ensure we cycle through all 4
    // buffers independent of swapchain index patterns (which might be 0,1,0,1...)
    uint32_t slotIndex = (state.captureFrameCounter++) % 4;

    // Ensure we don't exceed available images if for some reason we have fewer
    // than 4 (unlikely given creation logic)
    if (slotIndex >= sharedTextures->vkImages.size())
        slotIndex = 0;

    // Use imageIndex for fences/command buffers as those are tied to the
    // swapchain images
    uint32_t fenceIndex = imageIndex % state.copyFences.size();
    VkFence fence = state.copyFences[fenceIndex];

    // Non-blocking check: if the previous copy for this slot is still in flight,
    // drop this frame to avoid stalling the present queue (mirrors DX12 behavior)
    VkResult waitResult = disp->fp_vkWaitForFences(device, 1, &fence, VK_TRUE, 0);
    if (waitResult != VK_SUCCESS) {
        if (waitResult == VK_ERROR_DEVICE_LOST)
            return;
        // VK_TIMEOUT: previous copy still in flight, drop this frame
        return;
    }
    disp->fp_vkResetFences(device, 1, &fence);

    uint32_t cmdIndex = imageIndex % state.commandBuffers.size();
    VkCommandBuffer cmd = state.commandBuffers[cmdIndex];
    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                          VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (disp->fp_vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
        return;

    // Transition and copy
    VkImageMemoryBarrier srcBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    srcBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;  // Paranoid: Wait for everything
    srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;  // Assume presentable layout from game
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.image = srcImage;
    srcBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier dstBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    dstBarrier.srcAccessMask = 0;
    dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // Discard previous content
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.image = sharedTextures->vkImages[slotIndex];
    dstBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier barriers[] = {srcBarrier, dstBarrier};
    // Use ALL_COMMANDS to ensure we catch any previous usage (compute, graphics,
    // etc.)
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                  nullptr, 0, nullptr, 2, barriers);

    VkImageCopy region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffset = {0, 0, 0};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffset = {0, 0, 0};
    region.extent = {(uint32_t)sharedTextures->width, (uint32_t)sharedTextures->height, 1};

    disp->fp_vkCmdCopyImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sharedTextures->vkImages[slotIndex],
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier srcBarrier2 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    srcBarrier2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier2.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    srcBarrier2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    srcBarrier2.image = srcImage;
    srcBarrier2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier dstBarrier2 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    dstBarrier2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    dstBarrier2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier2.image = sharedTextures->vkImages[slotIndex];
    dstBarrier2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier postBarriers[] = {srcBarrier2, dstBarrier2};
    // Transition back for Present, enabling all subsequent stages
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                  nullptr, 0, nullptr, 2, postBarriers);

    if (disp->fp_vkEndCommandBuffer(cmd) != VK_SUCCESS)
        return;

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr};

    // We signal TWO semaphores:
    // 1. The binary semaphore for Present to wait on (passed as arg)
    // 2. The timeline semaphore for the Encoder to wait on (in state struct)
    // Use stack arrays to avoid heap allocation in the capture hot path (max 2 each)
    VkSemaphore signalSems[2];
    uint64_t signalValues[2] = {0, 0};
    uint32_t signalCount = 0;

    if (signalSemaphore != VK_NULL_HANDLE) {
        signalSems[signalCount] = signalSemaphore;
        signalValues[signalCount] = 0;  // Binary semaphore ignores value
        signalCount++;
    }

    // When IPC relay is active, we use two fence values per frame:
    //   vulkanSignalValue: Vulkan signals after copy to KMT texture
    //   encoderFenceValue: D3D11 signals after relay copy to NT IPC texture
    // The encoder waits on encoderFenceValue.
    bool doRelay = sharedTextures->hasIpcRelay && state.d3d11Fence && state.d3d11Context4;
    uint64_t vulkanSignalValue, encoderFenceValue;
    if (doRelay) {
        state.currentFenceValue += 2;
        vulkanSignalValue = state.currentFenceValue - 1;
        encoderFenceValue = state.currentFenceValue;
    } else {
        vulkanSignalValue = ++state.currentFenceValue;
        encoderFenceValue = vulkanSignalValue;
    }

    if (state.timelineSemaphore != VK_NULL_HANDLE) {
        signalSems[signalCount] = state.timelineSemaphore;
        signalValues[signalCount] = vulkanSignalValue;
        signalCount++;
    }

    if (signalCount > 0) {
        submit.signalSemaphoreCount = signalCount;
        submit.pSignalSemaphores = signalSems;
    }

    // Wait Semaphores
    uint64_t waitValue = 0;
    uint32_t waitCount = 0;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

    if (waitSemaphore != VK_NULL_HANDLE) {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &waitSemaphore;
        submit.pWaitDstStageMask = &waitStage;
        waitCount = 1;
    }

    // Prepare Timeline Info
    VkTimelineSemaphoreSubmitInfo timelineSubmit = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineSubmit.waitSemaphoreValueCount = waitCount;
    timelineSubmit.pWaitSemaphoreValues = waitCount > 0 ? &waitValue : nullptr;
    timelineSubmit.signalSemaphoreValueCount = signalCount;
    timelineSubmit.pSignalSemaphoreValues = signalValues;

    submit.pNext = &timelineSubmit;

    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;

    if (disp->fp_vkQueueSubmit(queue, 1, &submit, fence) == VK_ERROR_DEVICE_LOST) {
        return;
    }

    // IPC relay: D3D11 Wait/CopyResource/Signal to copy from KMT texture to NT IPC texture
    if (doRelay) {
        // GPU waits for Vulkan copy to complete (shared fence)
        state.d3d11Context4->Wait(state.d3d11Fence, vulkanSignalValue);
        // GPU copies from KMT-imported D3D11 texture to NT-shared IPC texture
        state.d3d11Context4->CopyResource(sharedTextures->ipcTextures[slotIndex], sharedTextures->textures[slotIndex]);
        // GPU signals completion for encoder to consume (use cross-process IPC fence if available)
        ID3D11Fence* signalFence = state.d3d11IpcFence ? state.d3d11IpcFence : state.d3d11Fence;
        state.d3d11Context4->Signal(signalFence, encoderFenceValue);
        // Flush to submit the D3D11 GPU work immediately
        state.d3d11Context4->Flush();
    }

    LayerIPC_SignalFrameReady(slotIndex, encoderFenceValue);
}
