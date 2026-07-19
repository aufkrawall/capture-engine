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
#include <algorithm>
#include <array>
#include <vector>
#include "../../common/capture_base.h"
#include "../../common/shared_defs.h"
#include "../common/hook_common.h"
#include "../common/screenshot_hook.h"
#include "layer_main.h"
#include "vulkan_layer.h"
#include "vulkan_presentation_color.h"
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
    VkDevice vkDevice = VK_NULL_HANDLE;
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

static bool SelectImportedWin32MemoryType(DeviceDispatch* disp, VkDevice device,
                                          VkExternalMemoryHandleTypeFlagBits handleType, HANDLE handle,
                                          uint32_t imageMemoryTypeBits,
                                          const VkPhysicalDeviceMemoryProperties& memoryProperties,
                                          uint32_t* memoryTypeIndex);

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

static bool IsSpecificDxvkWrapperLoaded(const char* dllName) {
    if (!IsDllFromProject(dllName, "dxvk")) {
        return false;
    }
    return GetModuleHandleA(dllName) != nullptr;
}

enum class VulkanCaptureInteropMode {
    kNative,
    kDxvkD3D11,
    kDxvkD3D9,
};

static VulkanCaptureInteropMode DetectVulkanInteropMode() {
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

static const char* VulkanInteropModeToString(VulkanCaptureInteropMode mode) {
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
        case VK_FORMAT_B8G8R8A8_SRGB:
            // Map SRGB to UNORM: same byte layout, avoids SRGB shared-texture compatibility issues
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

// Normalize SRGB swapchain formats to their UNORM equivalents for D3D11 interop.
// D3D11 KMT textures are created as UNORM (SRGB isn't needed for byte-level copies),
// so the VkImage used to import them must also be UNORM for a valid format match.
// vkCmdCopyImage between compatible 32-bit format classes (SRGB↔UNORM) is spec-valid.
static VkFormat NormalizeVkFormat(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_B8G8R8A8_SRGB:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return VK_FORMAT_R8G8B8A8_UNORM;
        default:
            return fmt;
    }
}

static bool CreateSharedTextures(D3D11InteropDevice* interopDev, VkDevice vkDev, DeviceDispatch* disp,
                                 VkPhysicalDevice physDev, const LUID& luid, uint32_t width, uint32_t height,
                                 uint32_t vkFormat, SharedTextureEntry& entry) {
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
    if (mem) {
        mem->SetIsHDR(ce::presentation_color::IsHDR(presentationEncoding));
    }
    if (presentationEncoding == ce::presentation_color::Encoding::Unsupported) {
        LayerLog("Vulkan Layer: [Error] Unsupported capture presentation contract format=%d colorSpace=%d", format,
                 colorSpace);
        return;
    }
    LayerLog("Vulkan Layer: Capture presentation contract format=%d colorSpace=%d encoding=%s", format, colorSpace,
             ce::presentation_color::Describe(presentationEncoding));

    // Get LUID for GPU identification
    LUID luid = {};
    if (!GetLUIDFromPhysicalDevice(disp->physicalDevice, &luid)) {
        LayerLog("Vulkan Layer: [Error] Failed to get LUID from physical device");
        return;
    }
    LayerIPC_SetLUID(static_cast<int32_t>(luid.LowPart), static_cast<int32_t>(luid.HighPart));

    uint64_t luidKey = MakeLuidKey(luid);

    SharedMemoryLayout* generationSharedMem = mem;
    const bool retiredLeasesDrained =
        !generationSharedMem || generationSharedMem->frameRing.readIndex.load(std::memory_order_acquire) ==
                                    generationSharedMem->frameRing.writeIndex.load(std::memory_order_acquire);
    for (const auto& retired : g_RetiredCaptureStates) {
        if (retired.device == device && (!retiredLeasesDrained || !CaptureStateCopiesComplete(retired, disp))) {
            return;
        }
    }

    // Check for existing state
    auto it = g_CaptureStates.find(device);
    if (it != g_CaptureStates.end()) {
        const uint32_t normalizedFormat = NormalizeVkFormat(format);
        const size_t expectedSubmissionCount = std::max<size_t>(imageCount, SHARED_TEXTURE_SLOT_COUNT);
        if (it->second.initialized && it->second.luidKey == luidKey && it->second.swapchain == swapchain &&
            it->second.captureWidth == extent.width && it->second.captureHeight == extent.height &&
            it->second.captureFormat == normalizedFormat && it->second.colorSpace == colorSpace &&
            it->second.copyFences.size() == expectedSubmissionCount &&
            it->second.signalSemaphores.size() == imageCount) {
            return;
        }

        SharedMemoryLayout* sharedMem = g_IPCClient.GetSharedMem();
        const bool leasesDrained = !sharedMem || sharedMem->frameRing.readIndex.load(std::memory_order_acquire) ==
                                                     sharedMem->frameRing.writeIndex.load(std::memory_order_acquire);
        if (!leasesDrained || !CaptureStateCopiesComplete(it->second, disp)) {
            static std::atomic<int> s_generationDrainLogCount{0};
            if (s_generationDrainLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
                LayerLog(
                    "Vulkan Layer: Deferring capture generation switch %p -> %p until old leases/GPU copies "
                    "drain",
                    it->second.swapchain, swapchain);
            }
            return;
        }
        LayerLog("Vulkan Layer: Retiring capture state for replaced swapchain %p -> %p (%ux%u/%u -> %ux%u/%u)",
                 it->second.swapchain, swapchain, it->second.captureWidth, it->second.captureHeight,
                 it->second.captureFormat, extent.width, extent.height, normalizedFormat);
        g_RetiredCaptureStates.emplace_back(std::move(it->second));
        g_CaptureStates.erase(it);
    }

    // Once old leases and GPU copies are complete, no capture state can still
    // reference texture generations for obsolete dimensions/formats. Reclaim
    // them instead of accumulating VRAM across repeated resizes.
    {
        const uint32_t normalizedFormat = NormalizeVkFormat(format);
        std::lock_guard<std::mutex> textureLock(g_InteropMutex);
        for (auto textureIt = g_TextureCache.begin(); textureIt != g_TextureCache.end();) {
            if (textureIt->vkDevice == device &&
                (textureIt->width != extent.width || textureIt->height != extent.height ||
                 textureIt->vkFormat != normalizedFormat)) {
                DestroySharedTextureEntryResources(*textureIt, disp);
                textureIt = g_TextureCache.erase(textureIt);
            } else {
                ++textureIt;
            }
        }
    }

    // Get shared textures (creates if needed)
    // For DXVK, we'll try encoder-created KMT textures first (imported into Vulkan)
    SharedTextureEntry* sharedTextures = nullptr;
    bool usingEncoderTextures = false;
    const VulkanCaptureInteropMode interopMode = DetectVulkanInteropMode();
    const bool allowDxvkEncoderTextures = (interopMode == VulkanCaptureInteropMode::kDxvkD3D11);
    LayerLog("Vulkan Layer: Capture interop mode = %s", VulkanInteropModeToString(interopMode));
    if (interopMode == VulkanCaptureInteropMode::kNative) {
        LayerLog("Vulkan Layer: Native Vulkan detected - encoder KMT adoption disabled");
    }

    if (allowDxvkEncoderTextures && mem) {
        // DXVK D3D11 zero-copy path: import encoder's KMT handles into Vulkan.

        // Publish resolution FIRST so the encoder can create textures
        mem->SetWidth(extent.width);
        mem->SetHeight(extent.height);
        mem->SetFormat(VkFormatToDXGI(format));

        // If existing KMT textures have a different format, clear kmtReady to
        // force the encoder to recreate them with the correct format.
        uint32_t ourDxgiFormat = VkFormatToDXGI(format);
        if (mem->encoderTextures.kmtReady.load(std::memory_order_acquire)) {
            uint32_t existingFormat = mem->encoderTextures.GetFormat();
            if (existingFormat != 0 && existingFormat != ourDxgiFormat) {
                LayerLog("Vulkan Layer: KMT format mismatch (existing=%d, need=%d) - requesting recreation",
                         existingFormat, ourDxgiFormat);
                mem->encoderTextures.kmtReady.store(false, std::memory_order_release);
            }
        }

        LayerLog(
            "Vulkan Layer: DXVK InitCapture - VkFmt=%d NormVkFmt=%d DXGIFmt=%d - "
            "published %dx%d, checking encoder KMT textures",
            format, NormalizeVkFormat((VkFormat)format), VkFormatToDXGI(format), extent.width, extent.height);

        // Never block swapchain creation waiting for the media process. The
        // capture hot path already retries encoder KMT adoption once media has
        // published the textures, so an immediate interop fallback is lossless.
        if (mem->encoderTextures.kmtReady.load(std::memory_order_acquire)) {
            SharedTextureEntry encoderEntry;
            HANDLE kmtHandles[ENCODER_TEXTURE_SLOT_COUNT] = {};
            if (ImportEncoderKmtTextures(device, disp, luidKey, extent.width, extent.height,
                                         NormalizeVkFormat((VkFormat)format), mem, &encoderEntry, kmtHandles)) {
                std::lock_guard<std::mutex> texLock(g_InteropMutex);
                g_TextureCache.push_back(std::move(encoderEntry));
                sharedTextures = &g_TextureCache.back();

                // Publish encoder KMT handles to ring buffer
                LayerIPC_SetTextures(kmtHandles, ENCODER_TEXTURE_SLOT_COUNT, extent.width, extent.height,
                                     VkFormatToDXGI(format));
                // Tell encoder to use its own textures directly
                mem->useEncoderTextures.store(true, std::memory_order_release);
                usingEncoderTextures = true;
                LayerLog("Vulkan Layer: DXVK d3d11 zero-copy: imported %d encoder KMT textures into Vulkan",
                         ENCODER_TEXTURE_SLOT_COUNT);
            } else {
                LayerLog("Vulkan Layer: [Warn] Failed to import encoder KMT textures, falling back to interop");
            }
        }

        if (!usingEncoderTextures) {
            mem->useEncoderTextures.store(false, std::memory_order_release);
            LayerLog(
                "Vulkan Layer: Encoder KMT textures not ready at swapchain creation; using immediate D3D11 "
                "interop fallback and retrying adoption asynchronously");
        }
    }

    // Fallback: D3D11 interop textures (non-DXVK or when encoder textures unavailable)
    if (!sharedTextures) {
        sharedTextures = GetOrCreateSharedTextures(device, disp, disp->physicalDevice, luid, extent.width,
                                                   extent.height, NormalizeVkFormat((VkFormat)format));
    }
    if (!sharedTextures || !sharedTextures->valid) {
        LayerLog("Vulkan Layer: [Error] Failed to get shared textures");
        return;
    }

    if (!usingEncoderTextures) {
        if (!allowDxvkEncoderTextures && mem) {
            LayerLog("Vulkan Layer: Native Vulkan mode - keeping IPC relay path (encoder KMT adoption disabled)");
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
    state.swapchain = swapchain;
    state.luidKey = luidKey;
    state.captureWidth = extent.width;
    state.captureHeight = extent.height;
    state.captureFormat = NormalizeVkFormat((VkFormat)format);
    state.colorSpace = colorSpace;
    state.interopMode = interopMode;
    state.initialized = false;

    const uint32_t captureSubmissionCount = std::max<uint32_t>(imageCount, SHARED_TEXTURE_SLOT_COUNT);
    state.copyFences.resize(captureSubmissionCount);
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    for (uint32_t i = 0; i < captureSubmissionCount; i++) {
        const VkResult fenceResult = disp->fp_vkCreateFence(device, &fenceInfo, nullptr, &state.copyFences[i]);
        if (fenceResult != VK_SUCCESS || state.copyFences[i] == VK_NULL_HANDLE) {
            LayerLog("Vulkan Layer: [Error] Failed to create capture fence %u/%u (result=%d)", i,
                     captureSubmissionCount, fenceResult);
            DestroyCaptureStateResources(state, disp);
            return;
        }
    }

    state.signalSemaphores.resize(imageCount);
    state.presentedImages.assign(imageCount, false);
    VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};
    for (uint32_t i = 0; i < imageCount; i++) {
        const VkResult signalSemaphoreResult =
            disp->fp_vkCreateSemaphore(device, &semInfo, nullptr, &state.signalSemaphores[i]);
        if (signalSemaphoreResult != VK_SUCCESS || state.signalSemaphores[i] == VK_NULL_HANDLE) {
            LayerLog("Vulkan Layer: [Error] Failed to create capture signal semaphore %u/%u (result=%d)", i, imageCount,
                     signalSemaphoreResult);
            DestroyCaptureStateResources(state, disp);
            return;
        }
    }

    // D3D12_FENCE produces a standard fence handle that D3D11 OpenSharedFence
    // can consume in both the relay and media processes. OPAQUE_WIN32 is not a
    // D3D fence and must never be published as though it were one.
    constexpr VkExternalSemaphoreHandleTypeFlagBits semHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;

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

    VkExportSemaphoreWin32HandleInfoKHR exportWin32Info = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
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

    if (semResult == VK_SUCCESS) {
        if (disp->fp_vkGetSemaphoreWin32HandleKHR) {
            VkSemaphoreGetWin32HandleInfoKHR getHandleInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
            getHandleInfo.semaphore = state.timelineSemaphore;
            getHandleInfo.handleType = semHandleType;

            HANDLE hFence = nullptr;
            VkResult fenceRes = disp->fp_vkGetSemaphoreWin32HandleKHR(device, &getHandleInfo, &hFence);

            if (fenceRes == VK_SUCCESS && hFence) {
                state.sharedFenceHandle = hFence;
                LayerLog("Vulkan Layer: Created D3D12 shared fence %p", hFence);
            } else {
                LayerLog("Vulkan Layer: [Error] Failed to get fence handle (vkResult=%d)", fenceRes);
            }
        }
    } else {
        LayerLog("Vulkan Layer: [Error] Failed to create timeline semaphore (vkResult=%d)", semResult);
    }
    if (semResult != VK_SUCCESS || state.timelineSemaphore == VK_NULL_HANDLE || !state.sharedFenceHandle) {
        LayerLog(
            "Vulkan Layer: [Error] Capture initialization requires an exportable timeline fence; disabling "
            "capture for this swapchain");
        DestroyCaptureStateResources(state, disp);
        return;
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
                        LayerLog("Vulkan Layer: [Error] IPC fence CreateSharedHandle failed (hr=0x%08X, handle=%p)", hr,
                                 state.ipcFenceHandle);
                        if (state.ipcFenceHandle) {
                            CloseHandle(state.ipcFenceHandle);
                            state.ipcFenceHandle = nullptr;
                        }
                        state.d3d11IpcFence->Release();
                        state.d3d11IpcFence = nullptr;
                    }
                } else {
                    LayerLog("Vulkan Layer: [Error] CreateFence for IPC failed (hr=0x%08X)", hr);
                }
            }
        }
    }
    if (sharedTextures->hasIpcRelay && (!state.d3d11Fence || !state.d3d11Context4 || sharedTextures->vkImages.empty() ||
                                        sharedTextures->ipcTextures.size() < sharedTextures->vkImages.size() ||
                                        sharedTextures->textures.size() < sharedTextures->vkImages.size())) {
        LayerLog(
            "Vulkan Layer: [Error] IPC relay synchronization/resources are incomplete; capture will not "
            "publish unsynchronized relay textures");
        DestroyCaptureStateResources(state, disp);
        return;
    }

    // Publish the appropriate fence handle for the encoder.
    // When relay uses the shared Vulkan fence as its signal target, publish that
    // handle so the encoder can still wait on the post-relay fence value.
    HANDLE fenceToPublish = nullptr;
    if (state.ipcFenceHandle) {
        fenceToPublish = state.ipcFenceHandle;
    } else if (state.sharedFenceHandle) {
        fenceToPublish = state.sharedFenceHandle;
        if (sharedTextures->hasIpcRelay) {
            LayerLog("Vulkan Layer: [Warn] IPC relay fence export unavailable; falling back to shared Vulkan fence");
        }
    }

    mem = g_IPCClient.GetSharedMem();
    if (fenceToPublish) {
        if (mem) {
            if (mem->encoderTextures.ready.load(std::memory_order_acquire)) {
                mem->encoderTextures.SetFenceHandle((uint64_t)fenceToPublish);
                LayerLog("Vulkan Layer: Published capture fence handle %p to encoderTextures", fenceToPublish);
            } else {
                LayerIPC_SetFence(fenceToPublish);
                LayerLog("Vulkan Layer: Published capture fence handle %p", fenceToPublish);
            }
        }
    } else if (mem) {
        if (mem->encoderTextures.ready.load(std::memory_order_acquire)) {
            mem->encoderTextures.SetFenceHandle(0);
        } else {
            LayerIPC_SetFence(nullptr);
        }
    }

    state.initialized = true;
    LayerLog("Vulkan Layer: Zero-Copy Capture Initialized (%dx%d)", extent.width, extent.height);
    g_CaptureStates[device] = std::move(state);
}

void NoteCaptureSwapchainImagePresented(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageIndex) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    auto current = g_CaptureStates.find(device);
    if (current == g_CaptureStates.end() || current->second.swapchain != swapchain ||
        imageIndex >= current->second.presentedImages.size()) {
        return;
    }

    const bool imageWasPresented = current->second.presentedImages[imageIndex];
    current->second.presentedImages[imageIndex] = true;
    if (!imageWasPresented)
        return;

    // Reacquiring and presenting the same image proves its preceding present
    // wait was consumed. At that point capture semaphores from older swapchains
    // can no longer be referenced by the presentation engine.
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    for (auto retired = g_RetiredCaptureStates.begin(); retired != g_RetiredCaptureStates.end();) {
        if (retired->device == device && CaptureStateCopiesComplete(*retired, disp)) {
            DestroyCaptureStateResources(*retired, disp);
            retired = g_RetiredCaptureStates.erase(retired);
        } else {
            ++retired;
        }
    }
}

void RetireCaptureSwapchain(VkDevice device, VkSwapchainKHR swapchain) {
    if (device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE)
        return;
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    auto it = g_CaptureStates.find(device);
    if (it == g_CaptureStates.end() || it->second.swapchain != swapchain)
        return;
    it->second.initialized = false;
    g_RetiredCaptureStates.emplace_back(std::move(it->second));
    g_CaptureStates.erase(it);
    LayerLog("Vulkan Layer: Retired capture state for destroyed swapchain %p", swapchain);
}

void CleanupCapture(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    uint64_t luidKey = 0;

    auto it = g_CaptureStates.find(device);
    if (it != g_CaptureStates.end()) {
        luidKey = it->second.luidKey;
        DestroyCaptureStateResources(it->second, disp);
        g_CaptureStates.erase(it);
    }
    for (auto retired = g_RetiredCaptureStates.begin(); retired != g_RetiredCaptureStates.end();) {
        if (retired->device == device) {
            if (luidKey == 0)
                luidKey = retired->luidKey;
            DestroyCaptureStateResources(*retired, disp);
            retired = g_RetiredCaptureStates.erase(retired);
        } else {
            ++retired;
        }
    }

    if (luidKey != 0 && disp) {
        // CRITICAL FIX: Clean up texture cache entries for this device
        // This prevents memory leaks of D3D11 textures and Vulkan images
        std::lock_guard<std::mutex> interopLock(g_InteropMutex);
        for (auto entry = g_TextureCache.begin(); entry != g_TextureCache.end();) {
            if (entry->vkDevice == device) {
                DestroySharedTextureEntryResources(*entry, disp);
                LayerLog("Vulkan Layer: Cleaned up texture cache entry for LUID %llx", luidKey);
                entry = g_TextureCache.erase(entry);
            } else {
                ++entry;
            }
        }

        // Signal media process to release its preserved encoder textures.
        // The VkImages we exported are gone; the D3D11 textures in the media process
        // are now unused and should be freed to reclaim VRAM.
        auto* sharedMem = g_IPCClient.GetSharedMem();
        if (sharedMem && sharedMem->useEncoderTextures.load(std::memory_order_acquire)) {
            sharedMem->useEncoderTextures.store(false, std::memory_order_release);
            sharedMem->encoderTextures.kmtReady.store(false, std::memory_order_release);
            LayerLog("Vulkan Layer: Cleared useEncoderTextures on vkDestroyDevice");
        }
    }
}

VkSemaphore GetCaptureSemaphore(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageIndex) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    auto it = g_CaptureStates.find(device);
    if (it != g_CaptureStates.end() && it->second.initialized && it->second.swapchain == swapchain) {
        if (imageIndex < it->second.signalSemaphores.size()) {
            return it->second.signalSemaphores[imageIndex];
        }
    }
    return VK_NULL_HANDLE;
}

bool CaptureFrame(VkDevice device, VkSwapchainKHR swapchain, VkQueue queue, VkImage srcImage,
                  const VkSemaphore* waitSemaphores, uint32_t waitSemaphoreCount, VkSemaphore signalSemaphore) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);

    auto it = g_CaptureStates.find(device);
    if (it == g_CaptureStates.end() || !it->second.initialized || it->second.swapchain != swapchain)
        return false;

    // Check if we should throttle capture (encoder is falling behind)
    if (g_IPCClient.GetSharedMem()) {
        if (g_IPCClient.GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
            return false;
        }
    }

    VulkanCaptureState& state = it->second;
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp)
        return false;
    if (signalSemaphore == VK_NULL_HANDLE || srcImage == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
        return false;
    }

    const uint32_t queueFamilyIndex = VulkanLayerState::Get().GetQueueFamilyIndex(queue);
    if (queueFamilyIndex == VK_QUEUE_FAMILY_IGNORED) {
        static std::atomic<int> s_unknownQueueLogCount{0};
        if (s_unknownQueueLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            LayerLog("Vulkan Layer: Capture skipped because present queue family is unknown");
        }
        return false;
    }

    if (!VulkanLayerState::Get().QueueSupportsTransfer(queue)) {
        static std::atomic<int> s_nonTransferQueueLogCount{0};
        if (s_nonTransferQueueLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            LayerLog("Vulkan Layer: Capture skipped on queue family %u without transfer support", queueFamilyIndex);
        }
        return false;
    }

    VulkanCaptureState::CommandResources* commandResources =
        EnsureCaptureCommandResources(state, disp, device, queueFamilyIndex);
    if (!commandResources) {
        return false;
    }

    auto* mem = g_IPCClient.GetSharedMem();
    const bool allowDxvkEncoderTextures = (state.interopMode == VulkanCaptureInteropMode::kDxvkD3D11);
    const bool encoderAdoptionRequested = allowDxvkEncoderTextures && mem &&
                                          !mem->useEncoderTextures.load(std::memory_order_acquire) &&
                                          mem->encoderTextures.kmtReady.load(std::memory_order_acquire) &&
                                          state.captureFrameCounter >= state.nextEncoderImportRetryFrame;
    if (encoderAdoptionRequested && mem->frameRing.readIndex.load(std::memory_order_acquire) !=
                                        mem->frameRing.writeIndex.load(std::memory_order_acquire)) {
        // Handles/fence are generation-global in shared memory. Stop producing
        // briefly so every old-generation lease drains before replacing them.
        static std::atomic<int> s_encoderAdoptionDrainLogCount{0};
        if (s_encoderAdoptionDrainLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
            LayerLog("Vulkan Layer: Deferring encoder-texture adoption until old capture leases drain");
        }
        return false;
    }
    if (encoderAdoptionRequested && !CaptureStateCopiesComplete(state, disp))
        return false;
    if (encoderAdoptionRequested) {
        SharedTextureEntry encoderEntry;
        HANDLE kmtHandles[ENCODER_TEXTURE_SLOT_COUNT] = {};
        if (ImportEncoderKmtTextures(device, disp, state.luidKey, state.captureWidth, state.captureHeight,
                                     state.captureFormat, mem, &encoderEntry, kmtHandles)) {
            {
                std::lock_guard<std::mutex> texLock(g_InteropMutex);
                for (auto textureIt = g_TextureCache.begin(); textureIt != g_TextureCache.end();) {
                    if (textureIt->vkDevice == device && textureIt->luidKey == state.luidKey &&
                        textureIt->width == state.captureWidth && textureIt->height == state.captureHeight &&
                        textureIt->vkFormat == state.captureFormat) {
                        DestroySharedTextureEntryResources(*textureIt, disp);
                        textureIt = g_TextureCache.erase(textureIt);
                    } else {
                        ++textureIt;
                    }
                }
                g_TextureCache.push_back(std::move(encoderEntry));
            }

            LayerIPC_SetTextures(kmtHandles, ENCODER_TEXTURE_SLOT_COUNT, state.captureWidth, state.captureHeight,
                                 VkFormatToDXGI((VkFormat)state.captureFormat));
            mem->useEncoderTextures.store(true, std::memory_order_release);
            state.sharedImageInitialized.fill(false);
            state.relayCompletionValues.fill(0);
            LayerLog("Vulkan Layer: DXVK d3d11 zero-copy: adopted encoder KMT textures after media startup");
        } else {
            state.nextEncoderImportRetryFrame = state.captureFrameCounter + 60;
        }
    }

    // Get shared textures from cache
    SharedTextureEntry* sharedTextures = nullptr;
    for (auto& entry : g_TextureCache) {
        if (entry.vkDevice == device && entry.luidKey == state.luidKey && entry.width == state.captureWidth &&
            entry.height == state.captureHeight && entry.vkFormat == state.captureFormat && entry.valid) {
            sharedTextures = &entry;
            break;
        }
    }
    if (!sharedTextures || !sharedTextures->valid)
        return false;
    if (state.copyFences.empty() || commandResources->buffers.empty() || state.signalSemaphores.empty() ||
        sharedTextures->vkImages.empty()) {
        LayerLog("Vulkan Layer: Capture skipped because synchronization/image resources are incomplete");
        return false;
    }

    // Use a monotonic counter independent of swapchain image patterns, but
    // rotate over the actual shared texture count rather than assuming four.
    const uint32_t sharedTextureCount =
        static_cast<uint32_t>(std::min<size_t>(sharedTextures->vkImages.size(), SHARED_TEXTURE_SLOT_COUNT));
    if (sharedTextureCount == 0)
        return false;
    const bool doRelay = sharedTextures->hasIpcRelay && state.d3d11Fence && state.d3d11Context4;
    ID3D11Fence* relayCompletionFence = state.d3d11IpcFence ? state.d3d11IpcFence : state.d3d11Fence;
    const uint32_t firstSlot = static_cast<uint32_t>(state.captureFrameCounter++ % sharedTextureCount);
    int32_t availableSlot = -1;
    uint32_t cpuBusySlots = 0;
    uint32_t gpuBusySlots = 0;
    for (uint32_t offset = 0; offset < sharedTextureCount; ++offset) {
        const uint32_t candidate = (firstSlot + offset) % sharedTextureCount;
        if (IsCaptureTextureSlotOutstanding(mem, static_cast<int32_t>(candidate))) {
            ++cpuBusySlots;
            continue;
        }
        if (doRelay && state.relayCompletionValues[candidate] != 0) {
            const uint64_t completedRelayValue = relayCompletionFence->GetCompletedValue();
            if (completedRelayValue == UINT64_MAX) {
                LayerLog("Vulkan Layer: IPC relay fence reported device removal; disabling capture state");
                state.initialized = false;
                return false;
            }
            if (completedRelayValue < state.relayCompletionValues[candidate]) {
                ++gpuBusySlots;
                continue;
            }
        }
        const uint32_t candidateFenceIndex = candidate % state.copyFences.size();
        const VkFence candidateFence = state.copyFences[candidateFenceIndex];
        const VkResult fenceStatus = disp->fp_vkWaitForFences(device, 1, &candidateFence, VK_TRUE, 0);
        if (fenceStatus != VK_SUCCESS) {
            if (fenceStatus == VK_ERROR_DEVICE_LOST) {
                state.initialized = false;
                return false;
            }
            ++gpuBusySlots;
            continue;
        }
        availableSlot = static_cast<int32_t>(candidate);
        break;
    }
    if (availableSlot < 0) {
        if (mem) {
            if (cpuBusySlots != 0)
                mem->runtimeState.injectProducerCpuLeaseBusyDrops.fetch_add(1, std::memory_order_relaxed);
            if (gpuBusySlots != 0)
                mem->runtimeState.injectProducerGpuBusyDrops.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }
    const uint32_t slotIndex = static_cast<uint32_t>(availableSlot);
    LARGE_INTEGER sourceQpc = {};
    QueryPerformanceCounter(&sourceQpc);

    // Fence/command-buffer ownership follows the destination texture slot.
    // Swapchains may have more images than the capture ring; indexing these by
    // image would not prove that an independently rotated destination is idle.
    uint32_t fenceIndex = slotIndex % state.copyFences.size();
    VkFence fence = state.copyFences[fenceIndex];

    // The scan above proved both the Vulkan submission and any D3D11 relay
    // operation for this exact slot complete without waiting on Present.

    uint32_t cmdIndex = slotIndex % commandResources->buffers.size();
    VkCommandBuffer cmd = commandResources->buffers[cmdIndex];
    const VkResult commandResetResult = disp->fp_vkResetCommandBuffer(cmd, 0);
    if (commandResetResult != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Capture command-buffer reset failed (index=%u family=%u result=%d)", cmdIndex,
                 queueFamilyIndex, commandResetResult);
        return false;
    }
    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                          VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    const VkResult beginResult = disp->fp_vkBeginCommandBuffer(cmd, &beginInfo);
    if (beginResult != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Capture command-buffer begin failed (index=%u family=%u result=%d)", cmdIndex,
                 queueFamilyIndex, beginResult);
        return false;
    }

    const uint32_t externalQueueFamily = VK_QUEUE_FAMILY_EXTERNAL;

    // Transition and copy
    VkImageMemoryBarrier srcBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    srcBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;  // Paranoid: Wait for everything
    srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;  // Assume presentable layout from game
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier.image = srcImage;
    srcBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier dstBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    dstBarrier.srcAccessMask = 0;
    dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier.oldLayout =
        state.sharedImageInitialized[slotIndex] ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.srcQueueFamilyIndex = externalQueueFamily;
    dstBarrier.dstQueueFamilyIndex = queueFamilyIndex;
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
    srcBarrier2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier2.image = srcImage;
    srcBarrier2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier dstBarrier2 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    dstBarrier2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier2.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    dstBarrier2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dstBarrier2.srcQueueFamilyIndex = queueFamilyIndex;
    dstBarrier2.dstQueueFamilyIndex = externalQueueFamily;
    dstBarrier2.image = sharedTextures->vkImages[slotIndex];
    dstBarrier2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier postBarriers[] = {srcBarrier2, dstBarrier2};
    // Transition back for Present, enabling all subsequent stages
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                  nullptr, 0, nullptr, 2, postBarriers);

    if (disp->fp_vkEndCommandBuffer(cmd) != VK_SUCCESS)
        return false;

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
    std::vector<uint64_t> waitValues;
    std::vector<VkPipelineStageFlags> waitStages;
    uint32_t waitCount = 0;

    if (waitSemaphores && waitSemaphoreCount > 0) {
        waitStages.assign(waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
        submit.waitSemaphoreCount = waitSemaphoreCount;
        submit.pWaitSemaphores = waitSemaphores;
        submit.pWaitDstStageMask = waitStages.data();
        waitValues.assign(waitSemaphoreCount, 0);
        waitCount = waitSemaphoreCount;
    }

    // Prepare Timeline Info
    VkTimelineSemaphoreSubmitInfo timelineSubmit = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineSubmit.waitSemaphoreValueCount = waitCount;
    timelineSubmit.pWaitSemaphoreValues = waitCount > 0 ? waitValues.data() : nullptr;
    timelineSubmit.signalSemaphoreValueCount = signalCount;
    timelineSubmit.pSignalSemaphoreValues = signalValues;

    submit.pNext = &timelineSubmit;

    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;

    const VkResult resetResult = disp->fp_vkResetFences(device, 1, &fence);
    if (resetResult != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Capture fence reset failed (index=%u result=%d)", fenceIndex, resetResult);
        return false;
    }

    const VkResult submitResult = disp->fp_vkQueueSubmit(queue, 1, &submit, fence);
    if (submitResult != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Capture queue submit failed (index=%u result=%d); Present wait chain unchanged",
                 fenceIndex, submitResult);
        if (submitResult == VK_ERROR_DEVICE_LOST) {
            state.initialized = false;
            return false;
        }

        // Queue submission did not take ownership of the command buffer or
        // signal this fence. Replace the now-unsignaled fence so a transient
        // submit failure cannot permanently suppress all later captures.
        disp->fp_vkDestroyFence(device, fence, nullptr);
        VkFenceCreateInfo recoveryFenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr,
                                               VK_FENCE_CREATE_SIGNALED_BIT};
        VkFence recoveryFence = VK_NULL_HANDLE;
        if (disp->fp_vkCreateFence(device, &recoveryFenceInfo, nullptr, &recoveryFence) == VK_SUCCESS) {
            state.copyFences[fenceIndex] = recoveryFence;
        } else {
            state.copyFences[fenceIndex] = VK_NULL_HANDLE;
            state.initialized = false;
            LayerLog("Vulkan Layer: Capture fence recovery failed; disabling this capture state");
        }
        return false;
    }
    state.sharedImageInitialized[slotIndex] = true;

    // IPC relay: D3D11 Wait/CopyResource/Signal to copy from KMT texture to NT IPC texture
    if (doRelay) {
        // GPU waits for Vulkan copy to complete (shared fence)
        const HRESULT waitHr = state.d3d11Context4->Wait(state.d3d11Fence, vulkanSignalValue);
        if (FAILED(waitHr)) {
            LayerLog("Vulkan Layer: IPC relay Wait failed (value=%llu hr=0x%08X); frame not published",
                     static_cast<unsigned long long>(vulkanSignalValue), static_cast<unsigned>(waitHr));
            return true;  // Vulkan submit consumed Present waits and will signal signalSemaphore.
        }
        // GPU copies from KMT-imported D3D11 texture to NT-shared IPC texture
        state.d3d11Context4->CopyResource(sharedTextures->ipcTextures[slotIndex], sharedTextures->textures[slotIndex]);
        // GPU signals completion for encoder to consume (use cross-process IPC fence if available)
        ID3D11Fence* signalFence = state.d3d11IpcFence ? state.d3d11IpcFence : state.d3d11Fence;
        const HRESULT signalHr = state.d3d11Context4->Signal(signalFence, encoderFenceValue);
        if (FAILED(signalHr)) {
            LayerLog("Vulkan Layer: IPC relay Signal failed (value=%llu hr=0x%08X); frame not published",
                     static_cast<unsigned long long>(encoderFenceValue), static_cast<unsigned>(signalHr));
            // CopyResource was already queued. Without a completion value this
            // slot can never be proven safe for Vulkan to overwrite again.
            state.relayCompletionUnknown = true;
            state.initialized = false;
            return true;  // Keep Present chained to the already-submitted Vulkan capture.
        }
        state.relayCompletionValues[slotIndex] = encoderFenceValue;
        // Flush to submit the D3D11 GPU work immediately
        state.d3d11Context4->Flush();
    }

    LayerIPC_SignalFrameReady(slotIndex, encoderFenceValue, sourceQpc.QuadPart);
    return true;
}

// ---- Vulkan Screenshot ----
// Reads pixels from the swapchain image using a staging buffer.
// Uses the Vulkan dispatch table and creates a one-shot command buffer.
bool TakeVulkanScreenshot(DeviceDispatch* disp, VkDevice device, VkQueue queue, VkImage srcImage, uint32_t width,
                          uint32_t height, VkFormat format, VkColorSpaceKHR colorSpace,
                          const VkSemaphore* waitSemaphores,
                          uint32_t waitSemaphoreCount, SharedMemoryLayout* sharedMemory, uint64_t requestId) {
    if (!disp || !device || !srcImage || !sharedMemory || requestId == 0 || width == 0 || height == 0 ||
        width > 16384 || height > 16384) {
        return false;
    }

    uint32_t bytesPerPixel = 0;
    ScreenshotPixelFormat pixelFormat{};
    ScreenshotColorEncoding colorEncoding{};
    bool swapPackedRedBlue = false;
    const auto presentationEncoding = ce::presentation_color::ResolveVulkan(format, colorSpace);
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            bytesPerPixel = 4;
            pixelFormat = ScreenshotPixelFormat::BGRA8;
            colorEncoding = ScreenshotColorEncoding::SRGB;
            break;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            bytesPerPixel = 4;
            pixelFormat = ScreenshotPixelFormat::RGBA8;
            colorEncoding = ScreenshotColorEncoding::SRGB;
            break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            bytesPerPixel = 4;
            pixelFormat = ScreenshotPixelFormat::R10G10B10A2;
            colorEncoding = presentationEncoding == ce::presentation_color::Encoding::Hdr10Pq
                                ? ScreenshotColorEncoding::BT2020_PQ
                                : ScreenshotColorEncoding::SRGB;
            break;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            bytesPerPixel = 4;
            pixelFormat = ScreenshotPixelFormat::R10G10B10A2;
            colorEncoding = presentationEncoding == ce::presentation_color::Encoding::Hdr10Pq
                                ? ScreenshotColorEncoding::BT2020_PQ
                                : ScreenshotColorEncoding::SRGB;
            swapPackedRedBlue = true;
            break;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            bytesPerPixel = 8;
            pixelFormat = ScreenshotPixelFormat::RGBA16F;
            colorEncoding = ScreenshotColorEncoding::LinearScRGB;
            break;
        default:
            HookLog("[Screenshot] Vulkan: Unsupported format %u", static_cast<unsigned>(format));
            return false;
    }
    if (presentationEncoding == ce::presentation_color::Encoding::Unsupported) {
        HookLog("[Screenshot] Vulkan: Unsupported presentation contract format=%u colorSpace=%u",
                static_cast<unsigned>(format), static_cast<unsigned>(colorSpace));
        return false;
    }

    const uint32_t queueFamilyIndex = VulkanLayerState::Get().GetQueueFamilyIndex(queue);
    if (queueFamilyIndex == VK_QUEUE_FAMILY_IGNORED) {
        HookLog("[Screenshot] Vulkan: Queue family unknown, skipping screenshot");
        return false;
    }
    if (!VulkanLayerState::Get().QueueSupportsTransfer(queue)) {
        HookLog("[Screenshot] Vulkan: Queue family %u lacks transfer support, skipping screenshot", queueFamilyIndex);
        return false;
    }

    // Calculate row pitch (4 bytes per pixel for BGRA/RGBA)
    uint32_t rowPitch = width * bytesPerPixel;
    VkDeviceSize bufferSize = static_cast<VkDeviceSize>(rowPitch) * height;

    // Find HOST_VISIBLE | HOST_COHERENT memory type
    VkPhysicalDeviceMemoryProperties memProps;
    VkPhysicalDevice physDev = disp->physicalDevice;
    if (physDev == VK_NULL_HANDLE)
        return false;

    auto* instDisp =
        VulkanLayerState::Get().GetInstanceDispatch(VulkanLayerState::Get().GetInstanceFromPhysicalDevice(physDev));
    if (!instDisp || !instDisp->fp_vkGetPhysicalDeviceMemoryProperties)
        return false;
    instDisp->fp_vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);

    // Create staging buffer
    VkBufferCreateInfo bufInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = bufferSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    if (disp->fp_vkCreateBuffer(device, &bufInfo, nullptr, &stagingBuffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReqs;
    disp->fp_vkGetBufferMemoryRequirements(device, stagingBuffer, &memReqs);

    uint32_t memTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1u << i)) != 0 &&
            (memProps.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memTypeIndex = i;
            break;
        }
    }
    if (memTypeIndex == UINT32_MAX) {
        HookLog("[Screenshot] Vulkan: No compatible HOST_VISIBLE memory type found");
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = memTypeIndex;

    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (disp->fp_vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory) != VK_SUCCESS) {
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }
    if (disp->fp_vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0) != VK_SUCCESS) {
        disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    // Create command pool
    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    if (disp->fp_vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool) != VK_SUCCESS) {
        disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    // Allocate command buffer
    VkCommandBufferAllocateInfo cmdAllocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdAllocInfo.commandPool = cmdPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    if (disp->fp_vkAllocateCommandBuffers(device, &cmdAllocInfo, &cmdBuf) != VK_SUCCESS) {
        disp->fp_vkDestroyCommandPool(device, cmdPool, nullptr);
        disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    // Record commands
    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (disp->fp_vkBeginCommandBuffer(cmdBuf, &beginInfo) != VK_SUCCESS) {
        disp->fp_vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
        disp->fp_vkDestroyCommandPool(device, cmdPool, nullptr);
        disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    // Transition image: PRESENT_SRC_KHR -> TRANSFER_SRC_OPTIMAL
    VkImageMemoryBarrier imgBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    imgBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    imgBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    imgBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imgBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imgBarrier.image = srcImage;
    imgBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    disp->fp_vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                  nullptr, 0, nullptr, 1, &imgBarrier);

    // Copy image to buffer
    VkBufferImageCopy copyRegion = {};
    copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.imageExtent = {width, height, 1};
    disp->fp_vkCmdCopyImageToBuffer(cmdBuf, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1,
                                    &copyRegion);

    // Transition image back: TRANSFER_SRC_OPTIMAL -> PRESENT_SRC_KHR
    imgBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    imgBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    imgBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imgBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    disp->fp_vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                  nullptr, 0, nullptr, 1, &imgBarrier);

    if (disp->fp_vkEndCommandBuffer(cmdBuf) != VK_SUCCESS) {
        disp->fp_vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
        disp->fp_vkDestroyCommandPool(device, cmdPool, nullptr);
        disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    // Create fence and submit
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (disp->fp_vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        disp->fp_vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
        disp->fp_vkDestroyCommandPool(device, cmdPool, nullptr);
        disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuf;
    std::vector<VkPipelineStageFlags> waitStages;
    if (waitSemaphores && waitSemaphoreCount > 0) {
        waitStages.assign(waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
        submitInfo.waitSemaphoreCount = waitSemaphoreCount;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages.data();
    }
    const VkResult submitResult = disp->fp_vkQueueSubmit(queue, 1, &submitInfo, fence);
    if (submitResult != VK_SUCCESS) {
        disp->fp_vkDestroyFence(device, fence, nullptr);
        disp->fp_vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
        disp->fp_vkDestroyCommandPool(device, cmdPool, nullptr);
        disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
        disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    // This is a one-shot readback. Waiting for its fence both makes mapped data
    // valid and proves that any Present wait semaphores consumed above are done.
    const VkResult waitResult = disp->fp_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    bool queued = false;
    if (waitResult == VK_SUCCESS) {
        void* mappedData = nullptr;
        if (disp->fp_vkMapMemory(device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mappedData) == VK_SUCCESS && mappedData) {
            if (swapPackedRedBlue) {
                std::vector<uint32_t> converted(static_cast<size_t>(width) * height);
                const auto* sourcePixels = static_cast<const uint32_t*>(mappedData);
                for (size_t i = 0; i < converted.size(); ++i) {
                    const uint32_t value = sourcePixels[i];
                    converted[i] = (value & 0xC00FFC00u) | ((value >> 20) & 0x3FFu) | ((value & 0x3FFu) << 20);
                }
                queued =
                    QueueScreenshotPixels(sharedMemory, requestId, reinterpret_cast<const uint8_t*>(converted.data()),
                                          width, height, rowPitch, pixelFormat, colorEncoding);
            } else {
                queued = QueueScreenshotPixels(sharedMemory, requestId, static_cast<const uint8_t*>(mappedData), width,
                                               height, rowPitch, pixelFormat, colorEncoding);
            }
            disp->fp_vkUnmapMemory(device, stagingMemory);
        }
    } else {
        HookLog("[Screenshot] Vulkan fence wait failed: %d", waitResult);
    }
    if (!queued) {
        CompleteScreenshotRequest(sharedMemory, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);
    }

    // Cleanup
    disp->fp_vkDestroyFence(device, fence, nullptr);
    disp->fp_vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
    disp->fp_vkDestroyCommandPool(device, cmdPool, nullptr);
    disp->fp_vkFreeMemory(device, stagingMemory, nullptr);
    disp->fp_vkDestroyBuffer(device, stagingBuffer, nullptr);
    return true;
}
