/**
 * VK_LAYER_CE_overlay - Zero-Copy Capture via D3D11 Interop
 *
 * OLD WORKING APPROACH:
 * Creates D3D11 textures with KMT sharing, imports them into Vulkan.
 * Vulkan copies to D3D11 textures, encoder opens KMT handles cross-process.
 * This is GPU zero-copy and proven to work.
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

// Static D3D11 Pointers
typedef HRESULT(WINAPI* PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

static bool CreateD3D11Device(IDXGIAdapter* adapter, ID3D11Device** ppDevice, ID3D11DeviceContext** ppContext) {
    HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
    if (!hD3D11) return false;

    PFN_D3D11_CREATE_DEVICE createFn = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
    if (!createFn) return false;

    UINT flags = 0; // D3D11_CREATE_DEVICE_BGRA_SUPPORT needed? Usually yes for D2D/DWrite but for capture maybe not strict. 
                    // However, DX11/12 interop often likes BGRA support.
    flags |= D3D11_CREATE_DEVICE_BGRA_SUPPORT; 

#ifdef _DEBUG
    // flags |= D3D11_CREATE_DEVICE_DEBUG; 
#endif

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;

    // Must use D3D_DRIVER_TYPE_UNKNOWN when adapter is non-NULL
    HRESULT hr = createFn(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, 
                          nullptr, flags, levels, 1, D3D11_SDK_VERSION, ppDevice, &featureLevel, ppContext);
    
    return SUCCEEDED(hr);
}


struct VulkanCaptureState {
    bool initialized = false;
    VkDevice device = VK_NULL_HANDLE;
    VkFormat vkFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0, 0};
    uint32_t imageCount = 0;
    
    // D3D11 intermediate path - creates D3D11 textures that Vulkan imports
    // KMT handles work cross-process with encoder
    ID3D11Device* d3d11Device = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    ID3D11Texture2D* d3d11Textures[4] = {};
    HANDLE d3d11TextureHandles[4] = {};  // KMT handles for cross-process sharing
    uint32_t dxgiFormat = 0;
    
    // Vulkan imported images (reference D3D11 textures)
    std::vector<VkImage> importedImages;
    std::vector<VkDeviceMemory> importedMemories;
    
    std::vector<VkFence> copyFences;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    
    uint32_t currentWriteIndex = 0;
};

static std::mutex g_CaptureMutex;
static std::unordered_map<VkDevice, VulkanCaptureState> g_CaptureStates;

// Helper to getting LUID from Vulkan Physical Device
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

static ComPtr<IDXGIAdapter> GetAdapterByLUID(const LUID& luid) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;

    ComPtr<IDXGIAdapter> adapter;
    for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);
        if (desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart) {
            return adapter;
        }
        adapter.Reset(); // CComPtr Release -> ComPtr Reset
    }
    return nullptr;
}

// Helper to find memory type
uint32_t FindMemoryType(VkDevice device, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    auto disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp) return 0;

    VkPhysicalDeviceMemoryProperties memProperties;
    InstanceDispatch* instDisp = VulkanLayerState::Get().GetInstanceDispatch(VulkanLayerState::Get().GetInstanceFromPhysicalDevice(disp->physicalDevice));
    if (!instDisp) return 0;

    instDisp->fp_vkGetPhysicalDeviceMemoryProperties(disp->physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

// Format conversion helpers
static uint32_t VkFormatToDXGI(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            return 28; // DXGI_FORMAT_R8G8B8A8_UNORM
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return 87; // DXGI_FORMAT_B8G8R8A8_UNORM
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return 10; // DXGI_FORMAT_R16G16B16A16_FLOAT
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            return 24; // DXGI_FORMAT_R10G10B10A2_UNORM
        default:
            return 87; // Fallback to BGRA
    }
}

static VkFormat DXGIToVkFormat(uint32_t dxgiFormat) {
    switch (dxgiFormat) {
        case 87: return VK_FORMAT_B8G8R8A8_UNORM;
        case 28: return VK_FORMAT_R8G8B8A8_UNORM;
        case 10: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 24: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        default: return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

/**
 * Check if timeline semaphores are supported
 * Requires Vulkan 1.2+ or VK_KHR_timeline_semaphore extension
 *
 * Note: This is a simplified check that just tries to create a timeline semaphore.
 * Full feature checking requires vkGetPhysicalDeviceFeatures2 which isn't
 * currently available in our dispatch table.
 */
bool IsTimelineSemaphoreSupported(VkDevice device, DeviceDispatch* disp) {
    // Just try to create a timeline semaphore - if it works, the feature is supported
    VkSemaphoreTypeCreateInfo timelineCreateInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        nullptr,
        VK_SEMAPHORE_TYPE_TIMELINE,
        0  // Initial value
    };

    VkSemaphoreCreateInfo createInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        &timelineCreateInfo
    };

    VkSemaphore testSemaphore = VK_NULL_HANDLE;
    VkResult result = disp->fp_vkCreateSemaphore(device, &createInfo, nullptr, &testSemaphore);

    if (result == VK_SUCCESS) {
        disp->fp_vkDestroySemaphore(device, testSemaphore, nullptr);
        LayerLog("Vulkan Layer: Timeline semaphores supported");
        return true;
    }

    LayerLog("Vulkan Layer: Timeline semaphores NOT supported (vk result: %d) - using binary fences", result);
    return false;
}

/**
 * Create a timeline semaphore for cross-queue synchronization
 */
bool CreateTimelineSemaphore(VkDevice device, DeviceDispatch* disp, VkSemaphore* pSemaphore) {
    VkSemaphoreTypeCreateInfo timelineCreateInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        nullptr,
        VK_SEMAPHORE_TYPE_TIMELINE,
        0  // Initial value
    };

    VkSemaphoreCreateInfo createInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        &timelineCreateInfo
    };

    VkResult result = disp->fp_vkCreateSemaphore(device, &createInfo, nullptr, pSemaphore);
    if (result == VK_SUCCESS) {
        LayerLog("Vulkan Layer: Created timeline semaphore");
        return true;
    }

    LayerLog("Vulkan Layer: Failed to create timeline semaphore (vk result: %d)", result);
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

    VulkanCaptureState state = {};
    state.device = device;
    state.vkFormat = format;
    state.extent = extent;
    state.imageCount = imageCount;

    // 1. Create D3D11 Device on matching adapter
    LUID luid = {};
    if (GetLUIDFromPhysicalDevice(disp->physicalDevice, &luid)) {
        ComPtr<IDXGIAdapter> adapter = GetAdapterByLUID(luid);
        if (adapter) {
            ID3D11Device* pD3D11Device = nullptr;
            ID3D11DeviceContext* pD3D11Context = nullptr;
            if (CreateD3D11Device(adapter.Get(), &pD3D11Device, &pD3D11Context)) {
                state.d3d11Device = pD3D11Device;
                state.d3d11Context = pD3D11Context;
                LayerLog("Vulkan Layer: Created D3D11 device on matching adapter (LUID %08X:%08X)", luid.HighPart, luid.LowPart);
            }
        }
    }

    if (!state.d3d11Device) {
        LayerLog("Vulkan Layer: [Error] Failed to create D3D11 device for interop. Fallback not implemented.");
        return;
    }

    // 2. Create D3D11 Shared Textures (KMT) and Import to Vulkan
    state.dxgiFormat = VkFormatToDXGI(format);
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = extent.width;
    texDesc.Height = extent.height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = (DXGI_FORMAT)state.dxgiFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // KMT Shared Handle

    // Create 4 textures for double/triple buffering
    const uint32_t kTextureCount = 4;
    std::vector<HANDLE> sharedHandles;

    for (uint32_t i = 0; i < kTextureCount; i++) {
        HRESULT hr = state.d3d11Device->CreateTexture2D(&texDesc, nullptr, &state.d3d11Textures[i]);
        if (FAILED(hr)) {
            LayerLog("Vulkan Layer: [Error] Failed to create D3D11 shared texture %d (hr=0x%08X)", i, hr);
            return;
        }

        ComPtr<IDXGIResource> dxgiRes;
        state.d3d11Textures[i]->QueryInterface(IID_PPV_ARGS(&dxgiRes));
        dxgiRes->GetSharedHandle(&state.d3d11TextureHandles[i]);
        sharedHandles.push_back(state.d3d11TextureHandles[i]);

        // Import to Vulkan
        VkExternalMemoryImageCreateInfo extInfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

        VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.pNext = &extInfo;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { extent.width, extent.height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkImage img = VK_NULL_HANDLE;
        if (disp->fp_vkCreateImage(device, &imageInfo, nullptr, &img) != VK_SUCCESS) {
             LayerLog("Vulkan Layer: [Error] Failed to create imported VkImage");
             return;
        }
        state.importedImages.push_back(img);

        VkMemoryRequirements memReqs;
        disp->fp_vkGetImageMemoryRequirements(device, img, &memReqs);

        VkImportMemoryWin32HandleInfoKHR importInfo = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
        importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
        importInfo.handle = state.d3d11TextureHandles[i];

        VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocInfo.pNext = &importInfo;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(device, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (disp->fp_vkAllocateMemory(device, &allocInfo, nullptr, &mem) != VK_SUCCESS) {
            LayerLog("Vulkan Layer: [Error] Failed to import D3D11 memory");
            return;
        }
        state.importedMemories.push_back(mem);
        disp->fp_vkBindImageMemory(device, img, mem, 0);
    }

    // Publish handles to Encoder via IPC
    LayerIPC_SetTextures(sharedHandles.data(), (uint32_t)sharedHandles.size(), extent.width, extent.height, state.dxgiFormat);

    // Setup Command Pool and Fences
    VkCommandPoolCreateInfo cpInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
    disp->fp_vkCreateCommandPool(device, &cpInfo, nullptr, &state.commandPool);

    state.commandBuffers.resize(imageCount); // One CB per swapchain image
    VkCommandBufferAllocateInfo cbInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, state.commandPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, imageCount };
    disp->fp_vkAllocateCommandBuffers(device, &cbInfo, state.commandBuffers.data());

    state.copyFences.resize(imageCount);
    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT };
    for (uint32_t i = 0; i < imageCount; i++) disp->fp_vkCreateFence(device, &fenceInfo, nullptr, &state.copyFences[i]);

    LayerLog("Vulkan Layer: Zero-Copy Capture Initialized (%dx%d, %d textures)", extent.width, extent.height, kTextureCount);
    
    state.initialized = true;
    g_CaptureStates[device] = state;
}

void CleanupCapture(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    auto it = g_CaptureStates.find(device);
    if (it == g_CaptureStates.end()) return;

    VulkanCaptureState& state = it->second;
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp) {
        disp->fp_vkDeviceWaitIdle(device);
        // Clean up Vulkan imported resources
        for (uint32_t i = 0; i < state.importedImages.size(); i++) {
            if (state.importedImages[i]) disp->fp_vkDestroyImage(device, state.importedImages[i], nullptr);
            if (state.importedMemories[i]) disp->fp_vkFreeMemory(device, state.importedMemories[i], nullptr);
        }
        for (auto fence : state.copyFences) disp->fp_vkDestroyFence(device, fence, nullptr);

        if (state.commandPool) {
            disp->fp_vkDestroyCommandPool(device, state.commandPool, nullptr);
        }
    }
    
    // Cleanup D3D11 resources
    for (int i = 0; i < 4; i++) { // kTextureCount is 4
        if (state.d3d11Textures[i]) {
            state.d3d11Textures[i]->Release();
            state.d3d11Textures[i] = nullptr;
        }
    }
    if (state.d3d11Context) {
        state.d3d11Context->Release();
        state.d3d11Context = nullptr;
    }
    if (state.d3d11Device) {
        state.d3d11Device->Release();
        state.d3d11Device = nullptr;
    }

    g_CaptureStates.erase(it);
}

void CaptureFrame(VkDevice device, VkQueue queue, VkImage srcImage, uint32_t imageIndex) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    auto it = g_CaptureStates.find(device);
    if (it == g_CaptureStates.end() || !it->second.initialized) return;

    VulkanCaptureState& state = it->second;
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp) {
        LayerLog("Vulkan Layer: [Error] No dispatch for device %p", device);
        return;
    }

    // Use frame counter or similar for slot index
    static std::atomic<uint32_t> s_FrameCounter{0};
    uint32_t slotIndex = s_FrameCounter.fetch_add(1) % 4; // Using 4 textures now
    
    // Validate resources
    if (slotIndex >= state.importedImages.size() || !state.importedImages[slotIndex]) {
        return;
    }

    // Check for device lost state
    static std::atomic<bool> s_deviceLostReported{false};

    // Use fence from imageIndex (we have fences per swapchain image)
    uint32_t fenceIndex = imageIndex % state.copyFences.size();
    VkFence fence = state.copyFences[fenceIndex];

    // Use 100ms timeout instead of UINT64_MAX to prevent potential hangs
    constexpr uint64_t FENCE_TIMEOUT_NS = 100000000;
    VkResult waitResult = disp->fp_vkWaitForFences(device, 1, &fence, VK_TRUE, FENCE_TIMEOUT_NS);

    if (waitResult != VK_SUCCESS) {
        if (waitResult == VK_ERROR_DEVICE_LOST) {
            LayerLog("Vulkan Layer: [Critical] DEVICE_LOST detected during fence wait");
            s_deviceLostReported.store(true);
            CleanupCapture(device);
            return;
        }
        if (waitResult == VK_TIMEOUT) {
            return; // Skip this frame
        }
        return;
    }

    VkResult resetResult = disp->fp_vkResetFences(device, 1, &fence);
    if (resetResult == VK_ERROR_DEVICE_LOST) {
        s_deviceLostReported.store(true);
        CleanupCapture(device);
        return;
    }

    uint32_t cmdIndex = imageIndex % state.commandBuffers.size();
    VkCommandBuffer cmd = state.commandBuffers[cmdIndex];
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkResult beginResult = disp->fp_vkBeginCommandBuffer(cmd, &beginInfo);
    if (beginResult != VK_SUCCESS) {
        if (beginResult == VK_ERROR_DEVICE_LOST) {
            s_deviceLostReported.store(true);
            CleanupCapture(device);
        }
        return;
    }

    // Transition source image to transfer source
    VkImageMemoryBarrier srcBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    srcBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.image = srcImage;
    srcBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // Transition dest image (imported D3D11) to transfer dest (assuming undefined initially or previous layout)
    // For simplicity, we can use UNDEFINED -> DST_OPTIMAL every frame if we don't care about content preserveration
    // KMT shared images should be careful about layout transitions if D3D11 is also accessing, but here we are producer.
    VkImageMemoryBarrier dstBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    dstBarrier.srcAccessMask = 0;
    dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; 
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.image = state.importedImages[slotIndex];
    dstBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier barriers[] = { srcBarrier, dstBarrier };
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 
                                   0, 0, nullptr, 0, nullptr, 2, barriers);

    // Copy image to imported image (GPU -> GPU)
    VkImageCopy region = {};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffset = { 0, 0, 0 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstOffset = { 0, 0, 0 };
    region.extent = { state.extent.width, state.extent.height, 1 };
    
    disp->fp_vkCmdCopyImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 
                             state.importedImages[slotIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 
                             1, &region);

    // Transition source image back to present
    srcBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // Transition dest image to General/ShaderReadOnly for D3D11 to consume?
    // D3D11 doesn't understand Vulkan layouts directly, but transitioning to "General" or similar is good practice.
    // However, the import is implementation dependent. GENERAL is safest for external access.
    dstBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT; // For D3D11 read
    dstBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkImageMemoryBarrier postBarriers[] = { srcBarrier, dstBarrier };
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 
                                   0, 0, nullptr, 0, nullptr, 2, postBarriers);

    VkResult endResult = disp->fp_vkEndCommandBuffer(cmd);
    if (endResult != VK_SUCCESS) {
        if (endResult == VK_ERROR_DEVICE_LOST) {
            s_deviceLostReported.store(true);
            CleanupCapture(device);
        }
        return;
    }

    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd };
    VkResult submitResult = disp->fp_vkQueueSubmit(queue, 1, &submit, fence);
    if (submitResult == VK_ERROR_DEVICE_LOST) {
        LayerLog("Vulkan Layer: [Critical] DEVICE_LOST during QueueSubmit");
        s_deviceLostReported.store(true);
        CleanupCapture(device);
        return;
    }
    if (submitResult != VK_SUCCESS) {
        return;
    }

    // Wait for fence to ensure GPU copy is done before signaling encoder
    // This is simple synchronization; for max perf we could export fences, but this is safe.
    waitResult = disp->fp_vkWaitForFences(device, 1, &fence, VK_TRUE, FENCE_TIMEOUT_NS);
    if (waitResult != VK_SUCCESS) {
        return;
    }

    // D3D11 side "Flush" might be needed if we were writing from D3D11, but we wrote from Vulkan.
    // Vulkan fence wait implies GPU work is done. D3D11 should see it.

    // Signal frame ready with texture index (0-3)
    LayerIPC_SignalFrameReady(slotIndex);
    
    s_deviceLostReported.store(false);

}
