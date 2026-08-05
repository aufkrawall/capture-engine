#include "vulkan_layer_internal.h"

VulkanLayerState& VulkanLayerState::Get() {
    static VulkanLayerState instance;
    return instance;
}

VulkanLayerState::VulkanLayerState()
    : m_OverlayEnabled(true),
      m_CaptureEnabled(true),
      m_MaxAnisotropy(16),
      m_AnisotropyOverrideActive(false),
      m_MipLodBias(0.0f),
      m_MipBiasOverrideActive(false),
      m_ForceMipBiasClamp(false),
      m_MipBiasMode("strict"),
      m_MipMapping("default"),
      m_SamplerOverrideMode("safe"),
      m_VsyncMode("default"),
      m_BackbufferCount(0),
      m_PrerenderLimit(-1.0f) {}

void VulkanLayerState::RegisterInstance(VkInstance instance, InstanceDispatch* dispatch) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_Instances[instance] = dispatch;
}

void VulkanLayerState::UnregisterInstance(VkInstance instance) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Instances.find(instance);
    if (it != m_Instances.end()) {
        delete it->second;
        m_Instances.erase(it);
    }
}

InstanceDispatch* VulkanLayerState::GetInstanceDispatch(VkInstance instance) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Instances.find(instance);
    return (it != m_Instances.end()) ? it->second : nullptr;
}

void VulkanLayerState::RegisterDevice(VkDevice device, DeviceDispatch* dispatch) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_Devices[device] = dispatch;
}

void VulkanLayerState::UnregisterDevice(VkDevice device) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Devices.find(device);
    if (it != m_Devices.end()) {
        delete it->second;
        m_Devices.erase(it);
    }
}

DeviceDispatch* VulkanLayerState::GetDeviceDispatch(VkDevice device) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Devices.find(device);
    if (it != m_Devices.end())
        return it->second;
    return nullptr;
}

void VulkanLayerState::RegisterQueue(VkQueue queue, VkDevice device, uint32_t familyIndex) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_Queues[queue] = device;
    m_QueueFamilies[queue] = familyIndex;

    uint32_t queueFlags = 0;
    auto deviceIt = m_Devices.find(device);
    if (deviceIt != m_Devices.end() && deviceIt->second) {
        VkPhysicalDevice physicalDevice = deviceIt->second->physicalDevice;
        auto physToInstanceIt = m_PhysDevToInstance.find(physicalDevice);
        if (physToInstanceIt != m_PhysDevToInstance.end()) {
            auto instanceIt = m_Instances.find(physToInstanceIt->second);
            if (instanceIt != m_Instances.end() && instanceIt->second &&
                instanceIt->second->fp_vkGetPhysicalDeviceQueueFamilyProperties) {
                uint32_t queueFamilyCount = 0;
                instanceIt->second->fp_vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
                                                                                nullptr);
                if (familyIndex < queueFamilyCount) {
                    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
                    instanceIt->second->fp_vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
                                                                                    queueFamilies.data());
                    queueFlags = queueFamilies[familyIndex].queueFlags;
                }
            }
        }
    }

    m_QueueFlags[queue] = queueFlags;
}

DeviceDispatch* VulkanLayerState::GetDeviceFromQueue(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Queues.find(queue);
    if (it != m_Queues.end()) {
        auto itDev = m_Devices.find(it->second);
        if (itDev != m_Devices.end())
            return itDev->second;
    }
    return nullptr;
}

VkDevice VulkanLayerState::GetVkDeviceFromQueue(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Queues.find(queue);
    if (it != m_Queues.end()) {
        return it->second;
    }
    return VK_NULL_HANDLE;
}

uint32_t VulkanLayerState::GetQueueFamilyIndex(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_QueueFamilies.find(queue);
    return (it != m_QueueFamilies.end()) ? it->second : VK_QUEUE_FAMILY_IGNORED;
}

uint32_t VulkanLayerState::GetQueueFlags(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_QueueFlags.find(queue);
    return (it != m_QueueFlags.end()) ? it->second : 0;
}

bool VulkanLayerState::QueueSupportsGraphics(VkQueue queue) {
    return (GetQueueFlags(queue) & VK_QUEUE_GRAPHICS_BIT) != 0;
}

bool VulkanLayerState::QueueSupportsTransfer(VkQueue queue) {
    const uint32_t flags = GetQueueFlags(queue);
    return (flags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0;
}

void VulkanLayerState::NoteQueueSubmit(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto queueIt = m_Queues.find(queue);
    if (queueIt == m_Queues.end()) {
        return;
    }
    m_DeviceLastSubmitThreadIds[queueIt->second] = GetCurrentThreadId();
}

uint32_t VulkanLayerState::GetLastSubmitThreadId(VkDevice device) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_DeviceLastSubmitThreadIds.find(device);
    return (it != m_DeviceLastSubmitThreadIds.end()) ? it->second : 0;
}

void VulkanLayerState::RegisterSwapchain(VkSwapchainKHR swapchain, SwapchainData* data) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_Swapchains[swapchain] = data;
}

void VulkanLayerState::UnregisterSwapchain(VkSwapchainKHR swapchain) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Swapchains.find(swapchain);
    if (it != m_Swapchains.end()) {
        delete it->second;
        m_Swapchains.erase(it);
        // Increment generation to invalidate all TLS caches
        m_SwapchainGeneration.fetch_add(1, std::memory_order_release);
    }
}

SwapchainData* VulkanLayerState::GetSwapchainData(VkSwapchainKHR swapchain) {
    static thread_local VkSwapchainKHR tls_LastSwapchain = VK_NULL_HANDLE;
    static thread_local SwapchainData* tls_LastData = nullptr;
    static thread_local uint64_t tls_LastGeneration = 0;

    // Check if TLS cache is still valid (no swapchains unregistered since last lookup)
    uint64_t currentGen = m_SwapchainGeneration.load(std::memory_order_acquire);
    if (currentGen != tls_LastGeneration) {
        tls_LastSwapchain = VK_NULL_HANDLE;
        tls_LastData = nullptr;
        tls_LastGeneration = currentGen;
    }

    if (swapchain == tls_LastSwapchain && tls_LastData)
        return tls_LastData;

    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Swapchains.find(swapchain);
    if (it != m_Swapchains.end()) {
        tls_LastSwapchain = swapchain;
        tls_LastData = it->second;
        tls_LastGeneration = m_SwapchainGeneration.load(std::memory_order_acquire);
        return tls_LastData;
    }
    return nullptr;
}

void VulkanLayerState::RegisterSurface(VkSurfaceKHR surface, HWND window) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_Surfaces[surface] = window;
}

void VulkanLayerState::UnregisterSurface(VkSurfaceKHR surface) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_Surfaces.erase(surface);
}

HWND VulkanLayerState::GetSurfaceWindow(VkSurfaceKHR surface) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Surfaces.find(surface);
    return (it != m_Surfaces.end()) ? it->second : NULL;
}

void VulkanLayerState::TrackPhysicalDevice(VkPhysicalDevice pd, VkInstance inst) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_PhysDevToInstance[pd] = inst;
}

VkInstance VulkanLayerState::GetInstanceFromPhysicalDevice(VkPhysicalDevice pd) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_PhysDevToInstance.find(pd);
    return (it != m_PhysDevToInstance.end()) ? it->second : VK_NULL_HANDLE;
}

void VulkanLayerState::UpdateFromSharedMemory(IPCClient* ipc) {
    if (!ipc || !ipc->GetSharedMem())
        return;

    auto& cfg = ipc->GetSharedMem()->graphicsConfig;

    // Parse anisotropic filtering
    if (strncmp(cfg.anisotropicFiltering, "default", 7) == 0 || cfg.anisotropicFiltering[0] == '\0') {
        m_AnisotropyOverrideActive = false;
        m_MaxAnisotropy = 16;
    } else {
        m_AnisotropyOverrideActive = true;
        if (strncmp(cfg.anisotropicFiltering, "16x", 3) == 0)
            m_MaxAnisotropy = 16;
        else if (strncmp(cfg.anisotropicFiltering, "8x", 2) == 0)
            m_MaxAnisotropy = 8;
        else if (strncmp(cfg.anisotropicFiltering, "4x", 2) == 0)
            m_MaxAnisotropy = 4;
        else if (strncmp(cfg.anisotropicFiltering, "2x", 2) == 0)
            m_MaxAnisotropy = 2;
        else if (strncmp(cfg.anisotropicFiltering, "off", 3) == 0)
            m_MaxAnisotropy = 1;
        else {
            m_AnisotropyOverrideActive = false;
            m_MaxAnisotropy = 16;
        }
    }

    // Parse mip bias
    if (strncmp(cfg.mipBias, "default", 7) != 0 && cfg.mipBias[0] != '\0') {
        float parsedBias = 0.0f;
        if (ce::TryParseFiniteFloat(cfg.mipBias, parsedBias)) {
            m_MipLodBias = parsedBias;
            m_MipBiasOverrideActive = true;
        } else {
            m_MipLodBias = 0.0f;
            m_MipBiasOverrideActive = false;
            static std::atomic<bool> loggedInvalidMipBias{false};
            if (!loggedInvalidMipBias.exchange(true, std::memory_order_relaxed))
                LayerLog("VulkanLayerState: rejected malformed mip bias '%s'", cfg.mipBias);
        }
    } else {
        m_MipLodBias = 0.0f;
        m_MipBiasOverrideActive = false;
    }

    // Parse mip bias mode
    if (cfg.mipBiasMode[0] != '\0')
        m_MipBiasMode = cfg.mipBiasMode;
    else
        m_MipBiasMode = "strict";

    m_ForceMipBiasClamp = cfg.forceMipBiasClamp;
    m_MipMapping = cfg.mipMapping[0] ? cfg.mipMapping : "default";
    m_SamplerOverrideMode = cfg.samplerOverrideMode[0] ? cfg.samplerOverrideMode : "safe";

    // VSync mode
    m_VsyncMode = cfg.vsyncMode;

    // Backbuffer count
    m_BackbufferCount = cfg.backbufferCount;

    // Prerender limit
    m_PrerenderLimit = cfg.prerenderLimit;

    LayerLog(
        "VulkanLayerState: Updated from config - policy=%s AF=%d, MipBias=%.1f, "
        "MipMap=%s, Clamp=%d, VSync=%s, BBCount=%d",
        m_SamplerOverrideMode.c_str(), m_MaxAnisotropy, m_MipLodBias, m_MipMapping.c_str(), m_ForceMipBiasClamp ? 1 : 0,
        m_VsyncMode.c_str(), m_BackbufferCount);
}
