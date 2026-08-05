#pragma once

struct D3D11InteropDevice;

struct SharedTextureEntry;

struct VulkanCaptureState;

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

#include "../../common/secure_dll_loading.h"

#include "../../common/shared_defs.h"

#include "../common/hook_common.h"

#include "../common/screenshot_hook.h"

#include "layer_main.h"

#include "vulkan_layer.h"

#include "vulkan_presentation_color.h"

using Microsoft::WRL::ComPtr;

enum class VulkanCaptureInteropMode {
    kNative,
    kDxvkD3D11,
    kDxvkD3D9,
};

void InitializeCapture(VkDevice device, VkSwapchainKHR swapchain, VkFormat format, VkColorSpaceKHR colorSpace, VkExtent2D extent, uint32_t imageCount);

void NoteCaptureSwapchainImagePresented(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageIndex);

void RetireCaptureSwapchain(VkDevice device, VkSwapchainKHR swapchain);

void CleanupCapture(VkDevice device);

VkSemaphore GetCaptureSemaphore(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageIndex);

bool CaptureFrame(VkDevice device, VkSwapchainKHR swapchain, VkQueue queue, VkImage srcImage, const VkSemaphore* waitSemaphores, uint32_t waitSemaphoreCount, VkSemaphore signalSemaphore);

bool TakeVulkanScreenshot(DeviceDispatch* disp, VkDevice device, VkQueue queue, VkImage srcImage, uint32_t width, uint32_t height, VkFormat format, VkColorSpaceKHR colorSpace, const VkSemaphore* waitSemaphores, uint32_t waitSemaphoreCount, SharedMemoryLayout* sharedMemory, uint64_t requestId);

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
inline std::mutex layer_capture_g_InteropMutex;

inline std::vector<D3D11InteropDevice> layer_capture_g_D3D11Devices;

inline std::vector<SharedTextureEntry> layer_capture_g_TextureCache;

inline bool SelectImportedWin32MemoryType(DeviceDispatch* disp, VkDevice device,
                                          VkExternalMemoryHandleTypeFlagBits handleType, HANDLE handle,
                                          uint32_t imageMemoryTypeBits,
                                          const VkPhysicalDeviceMemoryProperties& memoryProperties,
                                          uint32_t* layer_capture_memoryTypeIndex);

inline bool CreateD3D11InteropDevice(IDXGIAdapter* adapter, ID3D11Device** ppDevice, ID3D11DeviceContext** ppContext) {
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

inline uint64_t MakeLuidKey(const LUID& luid) {
    return (static_cast<uint64_t>(luid.HighPart) << 32) | static_cast<uint32_t>(luid.LowPart);
}

inline bool IsSpecificDxvkWrapperLoaded(const char* dllName) {
    if (!IsDllFromProject(dllName, "dxvk")) {
        return false;
    }
    return GetModuleHandleA(dllName) != nullptr;
}

inline VulkanCaptureInteropMode DetectVulkanInteropMode() {
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

inline const char* VulkanInteropModeToString(VulkanCaptureInteropMode mode) {
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

inline D3D11InteropDevice* GetOrCreateD3D11Device(const LUID& luid) {
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

inline uint32_t VkFormatToDXGI(VkFormat vkFormat) {
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

// Normalize SRGB swapchain formats to their UNORM equivalents for D3D11 interop.
// D3D11 KMT textures are created as UNORM (SRGB isn't needed for byte-level copies),
// so the VkImage used to import them must also be UNORM for a valid format match.
// vkCmdCopyImage between compatible 32-bit format classes (SRGB↔UNORM) is spec-valid.
inline VkFormat NormalizeVkFormat(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_B8G8R8A8_SRGB:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return VK_FORMAT_R8G8B8A8_UNORM;
        default:
            return fmt;
    }
}

inline bool CreateSharedTextures(D3D11InteropDevice* interopDev, VkDevice vkDev, DeviceDispatch* disp,
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

// Create Vulkan-native images with D3D11_TEXTURE NT export for cross-process sharing.
// Used when DXVK is active: bypasses D3D11 entirely since DXVK's D3D11 produces
// internal handles that native D3D11 in the encoder can't open.
// Uses NT handles (not KMT) because KMT handles from vkGetMemoryWin32HandleKHR are raw
// WDDM allocation handles without D3D11 resource metadata - D3D11's OpenSharedResource
// returns E_INVALIDARG for them. NT handles via D3D11_TEXTURE_BIT carry proper resource
// metadata and are openable by D3D11's OpenSharedResource1 after DuplicateHandle.
inline bool CreateVulkanNativeSharedTextures(VkDevice vkDev, DeviceDispatch* disp, VkPhysicalDevice physDev,
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

inline SharedTextureEntry* GetOrCreateSharedTextures(VkDevice vkDev, DeviceDispatch* disp, VkPhysicalDevice physDev,
                                                     const LUID& luid, uint32_t width, uint32_t height,
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

inline void DestroySharedTextureEntryResources(SharedTextureEntry& entry, DeviceDispatch* disp) {
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

inline std::mutex layer_capture_g_CaptureMutex;

inline std::unordered_map<VkDevice, VulkanCaptureState> layer_capture_g_CaptureStates;

inline std::vector<VulkanCaptureState> layer_capture_g_RetiredCaptureStates;

inline bool CaptureStateCopiesComplete(const VulkanCaptureState& state, DeviceDispatch* disp) {
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

inline void DestroyCaptureStateResources(VulkanCaptureState& state, DeviceDispatch* disp) {
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

inline bool SelectImportedWin32MemoryType(DeviceDispatch* disp, VkDevice device,
                                          VkExternalMemoryHandleTypeFlagBits handleType, HANDLE handle,
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

inline VulkanCaptureState::CommandResources* EnsureCaptureCommandResources(VulkanCaptureState& state,
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
inline bool GetLUIDFromPhysicalDevice(VkPhysicalDevice physDev, LUID* outLuid) {
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

inline bool ImportEncoderKmtTextures(VkDevice device, DeviceDispatch* disp, uint64_t luidKey, uint32_t width,
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
