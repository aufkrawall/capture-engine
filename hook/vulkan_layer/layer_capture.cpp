/**
 * VK_LAYER_CE_overlay - Zero-Copy Capture via D3D11 Interop
 *
 * Architecture:
 * - Global D3D11 interop device per GPU (keyed by LUID)
 * - Global shared texture cache per (LUID, resolution) 
 * - Vulkan devices map to D3D11 devices via LUID matching
 * - No recreation during runtime, no resource leaks
 */

#include "vulkan_layer.h"
#include "layer_main.h"
#include "../../common/shared_defs.h"
#include <vector>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <d3d11_1.h>
#include <wrl/client.h>
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
    
    std::vector<ID3D11Texture2D*> textures;     // 4 textures for double/triple buffering
    std::vector<HANDLE> textureHandles;         // KMT handles for IPC
    std::vector<VkImage> vkImages;              // Vulkan imported images
    std::vector<VkDeviceMemory> vkMemories;     // Vulkan imported memories
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
    HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
    if (!hD3D11) return false;

    PFN_D3D11_CREATE_DEVICE createFn = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
    if (!createFn) return false;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = createFn(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, 
                          nullptr, flags, levels, 1, D3D11_SDK_VERSION, ppDevice, &featureLevel, ppContext);
    
    return SUCCEEDED(hr);
}

static uint64_t MakeLuidKey(const LUID& luid) {
    return (static_cast<uint64_t>(luid.HighPart) << 32) | static_cast<uint32_t>(luid.LowPart);
}

static D3D11InteropDevice* GetOrCreateD3D11Device(const LUID& luid) {
    uint64_t key = MakeLuidKey(luid);
    
    for (auto it = g_D3D11Devices.begin(); it != g_D3D11Devices.end(); ) {
        if (it->luidKey == key) {
            if (it->valid) return &(*it);
            it = g_D3D11Devices.erase(it);
            continue;
        }
        ++it;
    }
    
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    
    ComPtr<IDXGIAdapter> adapter;
    for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);
        if (desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart) {
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
        case VK_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

static bool CreateSharedTextures(D3D11InteropDevice* interopDev, VkDevice vkDev, DeviceDispatch* disp,
                                  VkPhysicalDevice physDev, const LUID& luid,
                                  uint32_t width, uint32_t height, uint32_t vkFormat, 
                                  SharedTextureEntry& entry) {
    const uint32_t kTextureCount = 4;
    
    entry.luidKey = MakeLuidKey(luid);
    entry.width = width;
    entry.height = height;
    entry.vkFormat = vkFormat;
    
    entry.textures.resize(kTextureCount);
    entry.textureHandles.resize(kTextureCount);
    entry.vkImages.resize(kTextureCount);
    entry.vkMemories.resize(kTextureCount);
    
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = (DXGI_FORMAT)VkFormatToDXGI(vkFormat);
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    
    // Create D3D11 textures
    for (uint32_t i = 0; i < kTextureCount; i++) {
        HRESULT hr = interopDev->device->CreateTexture2D(&texDesc, nullptr, &entry.textures[i]);
        if (FAILED(hr)) {
            LayerLog("Vulkan Layer: [Error] Failed to create D3D11 texture %d (hr=0x%08X)", i, hr);
            return false;
        }
        
        ComPtr<IDXGIResource> dxgiRes;
        entry.textures[i]->QueryInterface(IID_PPV_ARGS(&dxgiRes));
        dxgiRes->GetSharedHandle(&entry.textureHandles[i]);
    }
    
    // Import textures to Vulkan
    for (uint32_t i = 0; i < kTextureCount; i++) {
        VkExternalMemoryImageCreateInfo extInfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
        
        VkImageCreateInfo imgInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &extInfo };
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = (VkFormat)vkFormat;
        imgInfo.extent = { width, height, 1 };
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        
        if (disp->fp_vkCreateImage(vkDev, &imgInfo, nullptr, &entry.vkImages[i]) != VK_SUCCESS) {
            return false;
        }
        
        VkMemoryRequirements memReq;
        disp->fp_vkGetImageMemoryRequirements(vkDev, entry.vkImages[i], &memReq);
        
        VkImportMemoryWin32HandleInfoKHR importInfo = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
        importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
        importInfo.handle = entry.textureHandles[i];
        
        VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &importInfo };
        allocInfo.allocationSize = memReq.size;
        
        VkPhysicalDeviceMemoryProperties memProps;
        InstanceDispatch* instDisp = VulkanLayerState::Get().GetInstanceDispatch(VulkanLayerState::Get().GetInstanceFromPhysicalDevice(physDev));
        instDisp->fp_vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);
        uint32_t memType = 0xFFFFFFFF;
        for (uint32_t j = 0; j < memProps.memoryTypeCount; j++) {
            if ((memReq.memoryTypeBits & (1 << j)) && (memProps.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
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
        
        if (disp->fp_vkAllocateMemory(vkDev, &allocInfo, nullptr, &entry.vkMemories[i]) != VK_SUCCESS) {
            return false;
        }
        
        disp->fp_vkBindImageMemory(vkDev, entry.vkImages[i], entry.vkMemories[i], 0);
    }
    
    entry.valid = true;
    return true;
}

static SharedTextureEntry* GetOrCreateSharedTextures(VkDevice vkDev, DeviceDispatch* disp,
                                                      VkPhysicalDevice physDev, const LUID& luid,
                                                      uint32_t width, uint32_t height, uint32_t vkFormat) {
    std::lock_guard<std::mutex> lock(g_InteropMutex);
    uint64_t luidKey = MakeLuidKey(luid);
    
    // Check existing cache
    for (auto it = g_TextureCache.begin(); it != g_TextureCache.end(); ) {
        if (it->luidKey == luidKey && it->width == width && it->height == height && it->vkFormat == vkFormat) {
            if (it->valid) return &(*it);
            // Remove invalid entry
            it = g_TextureCache.erase(it);
            continue;
        }
        ++it;
    }
    
    // Get D3D11 device
    D3D11InteropDevice* interopDev = GetOrCreateD3D11Device(luid);
    if (!interopDev || !interopDev->valid) {
        return nullptr;
    }
    
    // Create new textures
    SharedTextureEntry newEntry;
    if (!CreateSharedTextures(interopDev, vkDev, disp, physDev, luid, width, height, vkFormat, newEntry)) {
        return nullptr;
    }
    
    g_TextureCache.push_back(std::move(newEntry));
    return &g_TextureCache.back();
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
};

static std::mutex g_CaptureMutex;
static std::unordered_map<VkDevice, VulkanCaptureState> g_CaptureStates;

// Helper to get LUID from Vulkan Physical Device
static bool GetLUIDFromPhysicalDevice(VkPhysicalDevice physDev, LUID* outLuid) {
    InstanceDispatch* instDisp = VulkanLayerState::Get().GetInstanceDispatch(VulkanLayerState::Get().GetInstanceFromPhysicalDevice(physDev));
    if (!instDisp || !instDisp->fp_vkGetPhysicalDeviceProperties2) return false;

    VkPhysicalDeviceIDProperties idProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    props2.pNext = &idProps;

    instDisp->fp_vkGetPhysicalDeviceProperties2(physDev, &props2);
    
    if (idProps.deviceLUIDValid) {
        memcpy(outLuid, idProps.deviceLUID, sizeof(LUID));
        return true;
    }
    return false;
}

void InitializeCapture(VkDevice device, VkSwapchainKHR swapchain, VkFormat format,
                       VkExtent2D extent, uint32_t imageCount)
{
    LayerLog("Vulkan Layer: InitializeCapture(device=%p, images=%d, size=%dx%d, vkFormat=%d)",
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
    SharedTextureEntry* sharedTextures = GetOrCreateSharedTextures(device, disp, disp->physicalDevice, luid, extent.width, extent.height, format);
    if (!sharedTextures || !sharedTextures->valid) {
        LayerLog("Vulkan Layer: [Error] Failed to get shared textures");
        return;
    }

    // Publish textures to encoder via IPC
    LayerIPC_SetTextures(sharedTextures->textureHandles.data(), (uint32_t)sharedTextures->textureHandles.size(), extent.width, extent.height, VkFormatToDXGI(format));

    VulkanCaptureState state = {};
    state.device = device;
    state.luidKey = luidKey;
    state.initialized = true;

    // Create command pool
    VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
    if (disp->fp_vkCreateCommandPool(device, &poolInfo, nullptr, &state.commandPool) != VK_SUCCESS) {
        return;
    }

    state.commandBuffers.resize(imageCount);
    VkCommandBufferAllocateInfo cbInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, state.commandPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, imageCount };
    disp->fp_vkAllocateCommandBuffers(device, &cbInfo, state.commandBuffers.data());

    state.copyFences.resize(imageCount);
    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT };
    for (uint32_t i = 0; i < imageCount; i++) {
        disp->fp_vkCreateFence(device, &fenceInfo, nullptr, &state.copyFences[i]);
    }

    LayerLog("Vulkan Layer: Zero-Copy Capture Initialized (%dx%d)", extent.width, extent.height);
    g_CaptureStates[device] = state;
}

void CleanupCapture(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    // Don't cleanup - textures are in global cache and reused
    // Command pool/fences can be cleaned up if needed
    g_CaptureStates.erase(device);
}

void CaptureFrame(VkDevice device, VkQueue queue, VkImage srcImage, uint32_t imageIndex) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    
    auto it = g_CaptureStates.find(device);
    if (it == g_CaptureStates.end() || !it->second.initialized) return;
    
    VulkanCaptureState& state = it->second;
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp) return;

    // Get shared textures from cache
    SharedTextureEntry* sharedTextures = nullptr;
    for (auto& entry : g_TextureCache) {
        if (entry.luidKey == state.luidKey && entry.valid) {
            sharedTextures = &entry;
            break;
        }
    }
    if (!sharedTextures || !sharedTextures->valid) return;

    uint32_t slotIndex = imageIndex % 4;
    if (slotIndex >= sharedTextures->vkImages.size()) return;

    uint32_t fenceIndex = imageIndex % state.copyFences.size();
    VkFence fence = state.copyFences[fenceIndex];

    // Wait for previous frame with timeout
    constexpr uint64_t FENCE_TIMEOUT_NS = 50000000; // 50ms timeout
    VkResult waitResult = disp->fp_vkWaitForFences(device, 1, &fence, VK_TRUE, FENCE_TIMEOUT_NS);
    if (waitResult != VK_SUCCESS) {
        if (waitResult == VK_ERROR_DEVICE_LOST) return;
        if (waitResult == VK_TIMEOUT) return;
        return;
    }
    disp->fp_vkResetFences(device, 1, &fence);

    uint32_t cmdIndex = imageIndex % state.commandBuffers.size();
    VkCommandBuffer cmd = state.commandBuffers[cmdIndex];
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (disp->fp_vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) return;

    // Transition and copy
    VkImageMemoryBarrier srcBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    srcBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.image = srcImage;
    srcBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier dstBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    dstBarrier.srcAccessMask = 0;
    dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.image = sharedTextures->vkImages[slotIndex];
    dstBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier barriers[] = { srcBarrier, dstBarrier };
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

    VkImageCopy region = {};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffset = { 0, 0, 0 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstOffset = { 0, 0, 0 };
    region.extent = { (uint32_t)sharedTextures->width, (uint32_t)sharedTextures->height, 1 };

    disp->fp_vkCmdCopyImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sharedTextures->vkImages[slotIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier srcBarrier2 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    srcBarrier2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier2.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    srcBarrier2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    srcBarrier2.image = srcImage;
    srcBarrier2.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier dstBarrier2 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    dstBarrier2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier2.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    dstBarrier2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dstBarrier2.image = sharedTextures->vkImages[slotIndex];
    dstBarrier2.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier postBarriers[] = { srcBarrier2, dstBarrier2 };
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2, postBarriers);

    if (disp->fp_vkEndCommandBuffer(cmd) != VK_SUCCESS) return;

    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd };
    if (disp->fp_vkQueueSubmit(queue, 1, &submit, fence) == VK_ERROR_DEVICE_LOST) {
        return;
    }

    LayerIPC_SignalFrameReady(slotIndex);
}
