/**
 * Vulkan Layer - Core Implementation
 *
 * Consolidates all Vulkan hooks and state management.
 */

#define VK_USE_PLATFORM_WIN32_KHR
#include "vulkan_layer.h"
#include "../common/fps_limiter.h"
#include "layer_main.h" // For LayerLog and g_LayerState
#include <cstring>
#include <vector>

// Reentrancy guard shared with other hooks (defined here for the layer)
thread_local bool g_InPresentHook = false;

// VulkanLayerState Implementation
// ============================================================================

VulkanLayerState &VulkanLayerState::Get() {
  static VulkanLayerState instance;
  return instance;
}

VulkanLayerState::VulkanLayerState()
    : m_OverlayEnabled(true), m_CaptureEnabled(true), m_MaxAnisotropy(16),
      m_MipLodBias(0.0f) {}

void VulkanLayerState::RegisterInstance(VkInstance instance,
                                        InstanceDispatch *dispatch) {
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

InstanceDispatch *VulkanLayerState::GetInstanceDispatch(VkInstance instance) {
  std::lock_guard<std::recursive_mutex> lock(m_Lock);
  auto it = m_Instances.find(instance);
  return (it != m_Instances.end()) ? it->second : nullptr;
}

void VulkanLayerState::RegisterDevice(VkDevice device,
                                      DeviceDispatch *dispatch) {
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

DeviceDispatch *VulkanLayerState::GetDeviceDispatch(VkDevice device) {
  static thread_local VkDevice tls_LastDevice = VK_NULL_HANDLE;
  static thread_local DeviceDispatch *tls_LastDispatch = nullptr;
  if (device == tls_LastDevice && tls_LastDispatch)
    return tls_LastDispatch;

  std::lock_guard<std::recursive_mutex> lock(m_Lock);
  auto it = m_Devices.find(device);
  if (it != m_Devices.end()) {
    tls_LastDevice = device;
    tls_LastDispatch = it->second;
    return tls_LastDispatch;
  }
  return nullptr;
}

void VulkanLayerState::RegisterQueue(VkQueue queue, VkDevice device,
                                     uint32_t familyIndex) {
  std::lock_guard<std::recursive_mutex> lock(m_Lock);
  m_Queues[queue] = device;
  m_QueueFamilies[queue] = familyIndex;
}

DeviceDispatch *VulkanLayerState::GetDeviceFromQueue(VkQueue queue) {
  static thread_local VkQueue tls_LastQueue = VK_NULL_HANDLE;
  static thread_local DeviceDispatch *tls_LastDispatch = nullptr;
  if (queue == tls_LastQueue && tls_LastDispatch)
    return tls_LastDispatch;

  std::lock_guard<std::recursive_mutex> lock(m_Lock);
  auto it = m_Queues.find(queue);
  if (it != m_Queues.end()) {
    auto itDev = m_Devices.find(it->second);
    if (itDev != m_Devices.end()) {
      tls_LastQueue = queue;
      tls_LastDispatch = itDev->second;
      return tls_LastDispatch;
    }
  }
  return nullptr;
}

uint32_t VulkanLayerState::GetQueueFamilyIndex(VkQueue queue) {
  std::lock_guard<std::recursive_mutex> lock(m_Lock);
  auto it = m_QueueFamilies.find(queue);
  return (it != m_QueueFamilies.end()) ? it->second : VK_QUEUE_FAMILY_IGNORED;
}

void VulkanLayerState::RegisterSwapchain(VkSwapchainKHR swapchain,
                                         SwapchainData *data) {
  std::lock_guard<std::recursive_mutex> lock(m_Lock);
  m_Swapchains[swapchain] = data;
}

void VulkanLayerState::UnregisterSwapchain(VkSwapchainKHR swapchain) {
  std::lock_guard<std::recursive_mutex> lock(m_Lock);
  auto it = m_Swapchains.find(swapchain);
  if (it != m_Swapchains.end()) {
    delete it->second;
    m_Swapchains.erase(it);
  }
}

SwapchainData *VulkanLayerState::GetSwapchainData(VkSwapchainKHR swapchain) {
  static thread_local VkSwapchainKHR tls_LastSwapchain = VK_NULL_HANDLE;
  static thread_local SwapchainData *tls_LastData = nullptr;
  if (swapchain == tls_LastSwapchain && tls_LastData)
    return tls_LastData;

  std::lock_guard<std::recursive_mutex> lock(m_Lock);
  auto it = m_Swapchains.find(swapchain);
  if (it != m_Swapchains.end()) {
    tls_LastSwapchain = swapchain;
    tls_LastData = it->second;
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

void VulkanLayerState::TrackPhysicalDevice(VkPhysicalDevice pd,
                                           VkInstance inst) {
  std::lock_guard<std::recursive_mutex> lock(m_Lock);
  m_PhysDevToInstance[pd] = inst;
}

VkInstance
VulkanLayerState::GetInstanceFromPhysicalDevice(VkPhysicalDevice pd) {
  std::lock_guard<std::recursive_mutex> lock(m_Lock);
  auto it = m_PhysDevToInstance.find(pd);
  return (it != m_PhysDevToInstance.end()) ? it->second : VK_NULL_HANDLE;
}

// ============================================================================
// Helper to populate dispatch tables
// ============================================================================

void PopulateInstanceDispatch(InstanceDispatch *dispatch, VkInstance instance,
                              PFN_vkGetInstanceProcAddr gipa) {
  dispatch->instance = instance;
  dispatch->fp_vkGetInstanceProcAddr = gipa;
  dispatch->fp_vkDestroyInstance =
      (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
  dispatch->fp_vkEnumeratePhysicalDevices =
      (PFN_vkEnumeratePhysicalDevices)gipa(instance,
                                           "vkEnumeratePhysicalDevices");
  dispatch->fp_vkGetPhysicalDeviceProperties =
      (PFN_vkGetPhysicalDeviceProperties)gipa(instance,
                                              "vkGetPhysicalDeviceProperties");
  dispatch->fp_vkGetPhysicalDeviceProperties2 =
      (PFN_vkGetPhysicalDeviceProperties2)gipa(
          instance, "vkGetPhysicalDeviceProperties2");
  dispatch->fp_vkGetPhysicalDeviceFeatures =
      (PFN_vkGetPhysicalDeviceFeatures)gipa(instance,
                                            "vkGetPhysicalDeviceFeatures");
  dispatch->fp_vkGetPhysicalDeviceFeatures2 =
      (PFN_vkGetPhysicalDeviceFeatures2)gipa(instance,
                                             "vkGetPhysicalDeviceFeatures2");
  dispatch->fp_vkGetPhysicalDeviceQueueFamilyProperties =
      (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(
          instance, "vkGetPhysicalDeviceQueueFamilyProperties");
  dispatch->fp_vkGetPhysicalDeviceMemoryProperties =
      (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(
          instance, "vkGetPhysicalDeviceMemoryProperties");
  dispatch->fp_vkCreateDevice =
      (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
  dispatch->fp_vkEnumerateDeviceExtensionProperties =
      (PFN_vkEnumerateDeviceExtensionProperties)gipa(
          instance, "vkEnumerateDeviceExtensionProperties");
  dispatch->fp_vkDestroySurfaceKHR =
      (PFN_vkDestroySurfaceKHR)gipa(instance, "vkDestroySurfaceKHR");
  dispatch->fp_vkGetPhysicalDeviceSurfaceSupportKHR =
      (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)gipa(
          instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
  dispatch->fp_vkGetPhysicalDeviceSurfaceCapabilitiesKHR =
      (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(
          instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  dispatch->fp_vkGetPhysicalDeviceSurfaceFormatsKHR =
      (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)gipa(
          instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
  dispatch->fp_vkGetPhysicalDeviceSurfacePresentModesKHR =
      (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)gipa(
          instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
#ifdef VK_USE_PLATFORM_WIN32_KHR
  dispatch->fp_vkCreateWin32SurfaceKHR =
      (PFN_vkCreateWin32SurfaceKHR)gipa(instance, "vkCreateWin32SurfaceKHR");
#endif
}

void PopulateDeviceDispatch(DeviceDispatch *dispatch, VkDevice device,
                            PFN_vkGetDeviceProcAddr gdpa) {
  dispatch->device = device;
  dispatch->fp_vkGetDeviceProcAddr = gdpa;
  dispatch->fp_vkDestroyDevice =
      (PFN_vkDestroyDevice)gdpa(device, "vkDestroyDevice");
  dispatch->fp_vkGetDeviceQueue =
      (PFN_vkGetDeviceQueue)gdpa(device, "vkGetDeviceQueue");
  dispatch->fp_vkQueueSubmit = (PFN_vkQueueSubmit)gdpa(device, "vkQueueSubmit");
  dispatch->fp_vkQueueSubmit2 =
      (PFN_vkQueueSubmit2)gdpa(device, "vkQueueSubmit2");
  dispatch->fp_vkQueueSubmit2KHR =
      (PFN_vkQueueSubmit2KHR)gdpa(device, "vkQueueSubmit2KHR");
  dispatch->fp_vkQueueWaitIdle =
      (PFN_vkQueueWaitIdle)gdpa(device, "vkQueueWaitIdle");
  dispatch->fp_vkDeviceWaitIdle =
      (PFN_vkDeviceWaitIdle)gdpa(device, "vkDeviceWaitIdle");
  dispatch->fp_vkAllocateMemory =
      (PFN_vkAllocateMemory)gdpa(device, "vkAllocateMemory");
  dispatch->fp_vkFreeMemory = (PFN_vkFreeMemory)gdpa(device, "vkFreeMemory");
  dispatch->fp_vkMapMemory = (PFN_vkMapMemory)gdpa(device, "vkMapMemory");
  dispatch->fp_vkUnmapMemory = (PFN_vkUnmapMemory)gdpa(device, "vkUnmapMemory");
  dispatch->fp_vkBindBufferMemory =
      (PFN_vkBindBufferMemory)gdpa(device, "vkBindBufferMemory");
  dispatch->fp_vkBindImageMemory =
      (PFN_vkBindImageMemory)gdpa(device, "vkBindImageMemory");
  dispatch->fp_vkCreateBuffer =
      (PFN_vkCreateBuffer)gdpa(device, "vkCreateBuffer");
  dispatch->fp_vkDestroyBuffer =
      (PFN_vkDestroyBuffer)gdpa(device, "vkDestroyBuffer");
  dispatch->fp_vkCreateImage = (PFN_vkCreateImage)gdpa(device, "vkCreateImage");
  dispatch->fp_vkDestroyImage =
      (PFN_vkDestroyImage)gdpa(device, "vkDestroyImage");
  dispatch->fp_vkGetImageMemoryRequirements =
      (PFN_vkGetImageMemoryRequirements)gdpa(device,
                                             "vkGetImageMemoryRequirements");
  dispatch->fp_vkGetBufferMemoryRequirements =
      (PFN_vkGetBufferMemoryRequirements)gdpa(device,
                                              "vkGetBufferMemoryRequirements");
  dispatch->fp_vkCreateImageView =
      (PFN_vkCreateImageView)gdpa(device, "vkCreateImageView");
  dispatch->fp_vkDestroyImageView =
      (PFN_vkDestroyImageView)gdpa(device, "vkDestroyImageView");
  dispatch->fp_vkCreateSampler =
      (PFN_vkCreateSampler)gdpa(device, "vkCreateSampler");
  dispatch->fp_vkDestroySampler =
      (PFN_vkDestroySampler)gdpa(device, "vkDestroySampler");
  dispatch->fp_vkCreateFramebuffer =
      (PFN_vkCreateFramebuffer)gdpa(device, "vkCreateFramebuffer");
  dispatch->fp_vkDestroyFramebuffer =
      (PFN_vkDestroyFramebuffer)gdpa(device, "vkDestroyFramebuffer");
  dispatch->fp_vkCreateRenderPass =
      (PFN_vkCreateRenderPass)gdpa(device, "vkCreateRenderPass");
  dispatch->fp_vkDestroyRenderPass =
      (PFN_vkDestroyRenderPass)gdpa(device, "vkDestroyRenderPass");
  dispatch->fp_vkCreateCommandPool =
      (PFN_vkCreateCommandPool)gdpa(device, "vkCreateCommandPool");
  dispatch->fp_vkDestroyCommandPool =
      (PFN_vkDestroyCommandPool)gdpa(device, "vkDestroyCommandPool");
  dispatch->fp_vkAllocateCommandBuffers =
      (PFN_vkAllocateCommandBuffers)gdpa(device, "vkAllocateCommandBuffers");
  dispatch->fp_vkFreeCommandBuffers =
      (PFN_vkFreeCommandBuffers)gdpa(device, "vkFreeCommandBuffers");
  dispatch->fp_vkBeginCommandBuffer =
      (PFN_vkBeginCommandBuffer)gdpa(device, "vkBeginCommandBuffer");
  dispatch->fp_vkEndCommandBuffer =
      (PFN_vkEndCommandBuffer)gdpa(device, "vkEndCommandBuffer");
  dispatch->fp_vkResetCommandBuffer =
      (PFN_vkResetCommandBuffer)gdpa(device, "vkResetCommandBuffer");
  dispatch->fp_vkCmdBeginRenderPass =
      (PFN_vkCmdBeginRenderPass)gdpa(device, "vkCmdBeginRenderPass");
  dispatch->fp_vkCmdEndRenderPass =
      (PFN_vkCmdEndRenderPass)gdpa(device, "vkCmdEndRenderPass");
  dispatch->fp_vkCmdBindPipeline =
      (PFN_vkCmdBindPipeline)gdpa(device, "vkCmdBindPipeline");
  dispatch->fp_vkCmdDraw = (PFN_vkCmdDraw)gdpa(device, "vkCmdDraw");
  dispatch->fp_vkCmdDrawIndexed =
      (PFN_vkCmdDrawIndexed)gdpa(device, "vkCmdDrawIndexed");
  dispatch->fp_vkCmdPushConstants =
      (PFN_vkCmdPushConstants)gdpa(device, "vkCmdPushConstants");
  dispatch->fp_vkCmdSetViewport =
      (PFN_vkCmdSetViewport)gdpa(device, "vkCmdSetViewport");
  dispatch->fp_vkCmdSetScissor =
      (PFN_vkCmdSetScissor)gdpa(device, "vkCmdSetScissor");
  dispatch->fp_vkCmdBindVertexBuffers =
      (PFN_vkCmdBindVertexBuffers)gdpa(device, "vkCmdBindVertexBuffers");
  dispatch->fp_vkCmdBindIndexBuffer =
      (PFN_vkCmdBindIndexBuffer)gdpa(device, "vkCmdBindIndexBuffer");
  dispatch->fp_vkCmdCopyImage =
      (PFN_vkCmdCopyImage)gdpa(device, "vkCmdCopyImage");
  dispatch->fp_vkCmdCopyImageToBuffer =
      (PFN_vkCmdCopyImageToBuffer)gdpa(device, "vkCmdCopyImageToBuffer");
  dispatch->fp_vkCmdBlitImage =
      (PFN_vkCmdBlitImage)gdpa(device, "vkCmdBlitImage");
  dispatch->fp_vkCmdPipelineBarrier =
      (PFN_vkCmdPipelineBarrier)gdpa(device, "vkCmdPipelineBarrier");
  dispatch->fp_vkCmdClearAttachments =
      (PFN_vkCmdClearAttachments)gdpa(device, "vkCmdClearAttachments");
  dispatch->fp_vkCreateFence = (PFN_vkCreateFence)gdpa(device, "vkCreateFence");
  dispatch->fp_vkDestroyFence =
      (PFN_vkDestroyFence)gdpa(device, "vkDestroyFence");
  dispatch->fp_vkWaitForFences =
      (PFN_vkWaitForFences)gdpa(device, "vkWaitForFences");
  dispatch->fp_vkResetFences = (PFN_vkResetFences)gdpa(device, "vkResetFences");
  dispatch->fp_vkCreateSemaphore =
      (PFN_vkCreateSemaphore)gdpa(device, "vkCreateSemaphore");
  dispatch->fp_vkDestroySemaphore =
      (PFN_vkDestroySemaphore)gdpa(device, "vkDestroySemaphore");
  dispatch->fp_vkCreateSwapchainKHR =
      (PFN_vkCreateSwapchainKHR)gdpa(device, "vkCreateSwapchainKHR");
  dispatch->fp_vkDestroySwapchainKHR =
      (PFN_vkDestroySwapchainKHR)gdpa(device, "vkDestroySwapchainKHR");
  dispatch->fp_vkGetSwapchainImagesKHR =
      (PFN_vkGetSwapchainImagesKHR)gdpa(device, "vkGetSwapchainImagesKHR");
  dispatch->fp_vkAcquireNextImageKHR =
      (PFN_vkAcquireNextImageKHR)gdpa(device, "vkAcquireNextImageKHR");
  dispatch->fp_vkQueuePresentKHR =
      (PFN_vkQueuePresentKHR)gdpa(device, "vkQueuePresentKHR");
  dispatch->fp_vkCreateDescriptorSetLayout =
      (PFN_vkCreateDescriptorSetLayout)gdpa(device,
                                            "vkCreateDescriptorSetLayout");
  dispatch->fp_vkDestroyDescriptorSetLayout =
      (PFN_vkDestroyDescriptorSetLayout)gdpa(device,
                                             "vkDestroyDescriptorSetLayout");
  dispatch->fp_vkCreateDescriptorPool =
      (PFN_vkCreateDescriptorPool)gdpa(device, "vkCreateDescriptorPool");
  dispatch->fp_vkDestroyDescriptorPool =
      (PFN_vkDestroyDescriptorPool)gdpa(device, "vkDestroyDescriptorPool");
  dispatch->fp_vkAllocateDescriptorSets =
      (PFN_vkAllocateDescriptorSets)gdpa(device, "vkAllocateDescriptorSets");
  dispatch->fp_vkFreeDescriptorSets =
      (PFN_vkFreeDescriptorSets)gdpa(device, "vkFreeDescriptorSets");
  dispatch->fp_vkUpdateDescriptorSets =
      (PFN_vkUpdateDescriptorSets)gdpa(device, "vkUpdateDescriptorSets");
  dispatch->fp_vkCreatePipelineLayout =
      (PFN_vkCreatePipelineLayout)gdpa(device, "vkCreatePipelineLayout");
  dispatch->fp_vkDestroyPipelineLayout =
      (PFN_vkDestroyPipelineLayout)gdpa(device, "vkDestroyPipelineLayout");
  dispatch->fp_vkCreateGraphicsPipelines =
      (PFN_vkCreateGraphicsPipelines)gdpa(device, "vkCreateGraphicsPipelines");
  dispatch->fp_vkDestroyPipeline =
      (PFN_vkDestroyPipeline)gdpa(device, "vkDestroyPipeline");
  dispatch->fp_vkCreateShaderModule =
      (PFN_vkCreateShaderModule)gdpa(device, "vkCreateShaderModule");
  dispatch->fp_vkDestroyShaderModule =
      (PFN_vkDestroyShaderModule)gdpa(device, "vkDestroyShaderModule");
#ifdef VK_USE_PLATFORM_WIN32_KHR
  dispatch->fp_vkGetMemoryWin32HandleKHR =
      (PFN_vkGetMemoryWin32HandleKHR)gdpa(device, "vkGetMemoryWin32HandleKHR");
  dispatch->fp_vkGetSemaphoreWin32HandleKHR =
      (PFN_vkGetSemaphoreWin32HandleKHR)gdpa(device,
                                             "vkGetSemaphoreWin32HandleKHR");
#endif
}

// ============================================================================
// Hook Implementations
// ============================================================================

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateInstance(
    const VkInstanceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) {

  LayerLog("Vulkan Layer: Capture_vkCreateInstance called");

  // Mark Vulkan layer as active in shared memory
  auto *shm = g_IPCClient.GetSharedMem();
  if (shm)
    shm->runtimeState.vulkanLayerActive = true;

  PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)NULL;
  VkLayerInstanceCreateInfo *chain_info =
      (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;
  while (chain_info &&
         !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
           chain_info->function == VK_LAYER_LINK_INFO)) {
    chain_info = (VkLayerInstanceCreateInfo *)chain_info->pNext;
  }
  if (!chain_info)
    return VK_ERROR_INITIALIZATION_FAILED;
  gipa = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

  PFN_vkCreateInstance create_fn =
      (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");

  VkResult res = VK_SUCCESS;

  if (!g_LayerState.whitelisted) {
    // Passthrough: call next layer directly without modification
    res = create_fn(pCreateInfo, pAllocator, pInstance);
  } else {
    // Inject required extensions
    std::vector<const char *> extensions;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
      extensions.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
    }

    bool hasProps2 = false;
    bool hasExtMemCaps = false;
    for (const char *ext : extensions) {
      if (strcmp(ext, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) ==
          0)
        hasProps2 = true;
      if (strcmp(ext, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME) == 0)
        hasExtMemCaps = true;
    }

    if (!hasProps2)
      extensions.push_back(
          VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    if (!hasExtMemCaps)
      extensions.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);

    VkInstanceCreateInfo modifiedCreateInfo = *pCreateInfo;
    modifiedCreateInfo.enabledExtensionCount = (uint32_t)extensions.size();
    modifiedCreateInfo.ppEnabledExtensionNames = extensions.data();

    LayerLog("Vulkan Layer: Calling next vkCreateInstance...");
    res = create_fn(&modifiedCreateInfo, pAllocator, pInstance);
  }

  LayerLog("Vulkan Layer: next vkCreateInstance returned %d", res);
  if (res != VK_SUCCESS)
    return res;

  auto *dispatch = new InstanceDispatch();
  PopulateInstanceDispatch(dispatch, *pInstance, gipa);
  VulkanLayerState::Get().RegisterInstance(*pInstance, dispatch);

  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyInstance(
    VkInstance instance, const VkAllocationCallbacks *pAllocator) {

  InstanceDispatch *disp =
      VulkanLayerState::Get().GetInstanceDispatch(instance);
  if (disp && disp->fp_vkDestroyInstance)
    disp->fp_vkDestroyInstance(instance, pAllocator);
  VulkanLayerState::Get().UnregisterInstance(instance);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkEnumeratePhysicalDevices(
    VkInstance instance, uint32_t *pPhysicalDeviceCount,
    VkPhysicalDevice *pPhysicalDevices) {

  InstanceDispatch *disp =
      VulkanLayerState::Get().GetInstanceDispatch(instance);
  if (!disp || !disp->fp_vkEnumeratePhysicalDevices)
    return VK_ERROR_INITIALIZATION_FAILED;

  VkResult res = disp->fp_vkEnumeratePhysicalDevices(
      instance, pPhysicalDeviceCount, pPhysicalDevices);

  if (res >= VK_SUCCESS && pPhysicalDevices && pPhysicalDeviceCount &&
      *pPhysicalDeviceCount > 0) {
    for (uint32_t i = 0; i < *pPhysicalDeviceCount; i++) {
      VulkanLayerState::Get().TrackPhysicalDevice(pPhysicalDevices[i],
                                                  instance);
    }
  }

  return res;
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {

  LayerLog("Vulkan Layer: Capture_vkCreateDevice called");

  VkInstance instance =
      VulkanLayerState::Get().GetInstanceFromPhysicalDevice(physicalDevice);
  if (instance == VK_NULL_HANDLE) {
    LayerLog(
        "Vulkan Layer: [Error] Could not find instance for physical device %p",
        physicalDevice);
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkLayerDeviceCreateInfo *chain_info =
      (VkLayerDeviceCreateInfo *)pCreateInfo->pNext;
  while (chain_info &&
         !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
           chain_info->function == VK_LAYER_LINK_INFO)) {
    chain_info = (VkLayerDeviceCreateInfo *)chain_info->pNext;
  }
  if (!chain_info)
    return VK_ERROR_INITIALIZATION_FAILED;

  PFN_vkGetDeviceProcAddr gdpa =
      chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  PFN_vkGetInstanceProcAddr gipa =
      chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

  PFN_vkCreateDevice create_fn =
      (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
  if (!create_fn) {
    LayerLog("Vulkan Layer: [Error] Failed to get next vkCreateDevice from "
             "instance %p",
             instance);
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkResult result = VK_SUCCESS;

  if (!g_LayerState.whitelisted) {
    // Passthrough: call next layer directly without modification
    result = create_fn(physicalDevice, pCreateInfo, pAllocator, pDevice);
  } else {
    // Inject required extensions
    std::vector<const char *> extensions;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
      extensions.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
    }

    bool hasExtMem = false;
    bool hasExtMemWin32 = false;
    bool hasExtSem = false;
    bool hasExtSemWin32 = false;
    bool hasTimeline = false;
    for (const char *ext : extensions) {
      if (strcmp(ext, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) == 0)
        hasExtMem = true;
      if (strcmp(ext, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) == 0)
        hasExtMemWin32 = true;
      if (strcmp(ext, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME) == 0)
        hasExtSem = true;
      if (strcmp(ext, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME) == 0)
        hasExtSemWin32 = true;
      if (strcmp(ext, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0)
        hasTimeline = true;
    }

    if (!hasExtMem)
      extensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
    if (!hasExtMemWin32)
      extensions.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
    if (!hasExtSem)
      extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
    if (!hasExtSemWin32)
      extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
    if (!hasTimeline)
      extensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);

    VkDeviceCreateInfo modifiedCreateInfo = *pCreateInfo;
    modifiedCreateInfo.enabledExtensionCount = (uint32_t)extensions.size();
    modifiedCreateInfo.ppEnabledExtensionNames = extensions.data();

    // Enable timeline semaphore feature (required for
    // VK_KHR_timeline_semaphore)
    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    timelineFeatures.timelineSemaphore = VK_TRUE;

    // Chain the feature structure
    timelineFeatures.pNext = (void *)modifiedCreateInfo.pNext;
    modifiedCreateInfo.pNext = &timelineFeatures;

    LayerLog("Vulkan Layer: Calling next vkCreateDevice...");
    result =
        create_fn(physicalDevice, &modifiedCreateInfo, pAllocator, pDevice);
  }

  LayerLog("Vulkan Layer: next vkCreateDevice returned %d", result);
  if (result != VK_SUCCESS)
    return result;

  auto *dispatch = new DeviceDispatch();
  dispatch->physicalDevice = physicalDevice;
  PopulateDeviceDispatch(dispatch, *pDevice, gdpa);
  VulkanLayerState::Get().RegisterDevice(*pDevice, dispatch);

  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyDevice(
    VkDevice device, const VkAllocationCallbacks *pAllocator) {

  CleanupOverlay(device);
  CleanupCapture(device);
  DeviceDispatch *disp = VulkanLayerState::Get().GetDeviceDispatch(device);
  if (disp && disp->fp_vkDestroyDevice)
    disp->fp_vkDestroyDevice(device, pAllocator);
  VulkanLayerState::Get().UnregisterDevice(device);
}

VKAPI_ATTR void VKAPI_CALL Capture_vkGetDeviceQueue(VkDevice device,
                                                    uint32_t queueFamilyIndex,
                                                    uint32_t queueIndex,
                                                    VkQueue *pQueue) {

  DeviceDispatch *disp = VulkanLayerState::Get().GetDeviceDispatch(device);
  if (disp && disp->fp_vkGetDeviceQueue) {
    disp->fp_vkGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
    if (pQueue && *pQueue != VK_NULL_HANDLE) {
      VulkanLayerState::Get().RegisterQueue(*pQueue, device, queueFamilyIndex);
    }
  }
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain) {

  DeviceDispatch *disp = VulkanLayerState::Get().GetDeviceDispatch(device);
  if (!disp || !disp->fp_vkCreateSwapchainKHR)
    return VK_ERROR_INITIALIZATION_FAILED;

  VkResult res = disp->fp_vkCreateSwapchainKHR(device, pCreateInfo, pAllocator,
                                               pSwapchain);
  if (res == VK_SUCCESS && g_LayerState.whitelisted) {
    auto *sd = new SwapchainData();
    sd->swapchain = *pSwapchain;
    sd->device = device;
    sd->format = pCreateInfo->imageFormat;
    sd->extent = pCreateInfo->imageExtent;

    uint32_t count = 0;
    disp->fp_vkGetSwapchainImagesKHR(device, *pSwapchain, &count, nullptr);
    sd->images.resize(count);
    disp->fp_vkGetSwapchainImagesKHR(device, *pSwapchain, &count,
                                     sd->images.data());
    sd->imageCount = count;

    HWND window =
        VulkanLayerState::Get().GetSurfaceWindow(pCreateInfo->surface);
    InitializeOverlay(device, *pSwapchain, sd->format, sd->extent, count,
                      sd->images.data(), window);
    InitializeCapture(device, *pSwapchain, sd->format, sd->extent, count);

    VulkanLayerState::Get().RegisterSwapchain(*pSwapchain, sd);
  }
  return res;
}

VKAPI_ATTR void VKAPI_CALL
Capture_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                              const VkAllocationCallbacks *pAllocator) {

  DeviceDispatch *disp = VulkanLayerState::Get().GetDeviceDispatch(device);
  if (disp && disp->fp_vkDestroySwapchainKHR)
    disp->fp_vkDestroySwapchainKHR(device, swapchain, pAllocator);
  VulkanLayerState::Get().UnregisterSwapchain(swapchain);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkGetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t *pSwapchainImageCount,
    VkImage *pSwapchainImages) {

  DeviceDispatch *disp = VulkanLayerState::Get().GetDeviceDispatch(device);
  if (!disp || !disp->fp_vkGetSwapchainImagesKHR)
    return VK_ERROR_INITIALIZATION_FAILED;
  return disp->fp_vkGetSwapchainImagesKHR(
      device, swapchain, pSwapchainImageCount, pSwapchainImages);
}

VKAPI_ATTR VkResult VKAPI_CALL
Capture_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
  // Set flag to inform DXGI hooks that Vulkan is presenting on this thread.
  // This prevents double-drawing and incorrect API labeling when Vulkan
  // presents via DXGI.
  auto *shm = g_IPCClient.GetSharedMem();
  if (shm) {
    shm->runtimeState.vulkanPresentThreadId.store(GetCurrentThreadId(),
                                                  std::memory_order_release);
    shm->runtimeState.vulkanPresentTick.store(GetTickCount64(),
                                              std::memory_order_release);
  }

  bool isFirstHook = !g_InPresentHook;
  g_InPresentHook = true;

  if (isFirstHook) {
    g_SharedFpsLimiter.SetIPCClient(&g_IPCClient);
    g_SharedFpsLimiter.Apply();
  }

  DeviceDispatch *disp = VulkanLayerState::Get().GetDeviceFromQueue(queue);

  // Track the semaphore we should wait on (starts with the game's semaphore)
  VkSemaphore currentWait =
      (pPresentInfo && pPresentInfo->waitSemaphoreCount > 0)
          ? pPresentInfo->pWaitSemaphores[0]
          : VK_NULL_HANDLE;
  bool modified = false;

  if (g_LayerState.whitelisted && pPresentInfo &&
      pPresentInfo->swapchainCount > 0) {
    SwapchainData *sd =
        VulkanLayerState::Get().GetSwapchainData(pPresentInfo->pSwapchains[0]);
    if (sd) {
      uint32_t idx = pPresentInfo->pImageIndices[0];

      // OPTIMIZATION: Chained semaphores for perfect GPU-side async execution.
      // Game -> Overlay -> Capture -> Present
      VkSemaphore overlayDone = GetOverlaySemaphore(sd->device, idx);

      if (shm && shm->overlayConfig.showOverlay) {
        RenderOverlay(sd->device, queue, idx, currentWait, overlayDone);
        currentWait = overlayDone; // Next step waits for overlay
        modified = true;
      }

      if (shm && shm->runtimeState.isRecording) {
        VkSemaphore captureDone = GetCaptureSemaphore(sd->device, idx);
        CaptureFrame(sd->device, queue, sd->images[idx], idx, currentWait,
                     captureDone);
        if (captureDone != VK_NULL_HANDLE) {
          currentWait = captureDone;
          modified = true;
        }
      }
    }
  }

  // Create modified PresentInfo with chained semaphore
  VkPresentInfoKHR presentInfoCopy;
  if (pPresentInfo && modified) {
    presentInfoCopy = *pPresentInfo;
    if (currentWait != VK_NULL_HANDLE) {
      presentInfoCopy.waitSemaphoreCount = 1;
      presentInfoCopy.pWaitSemaphores = &currentWait;
    } else {
      // If we have no wait semaphore (e.g. game didn't provide one and we
      // didn't add one), we must ensure we don't pass garbage.
      presentInfoCopy.waitSemaphoreCount = 0;
      presentInfoCopy.pWaitSemaphores = nullptr;
    }
  }

  VkResult res = VK_SUCCESS;
  if (disp && disp->fp_vkQueuePresentKHR) {
    res = disp->fp_vkQueuePresentKHR(
        queue, (pPresentInfo && modified) ? &presentInfoCopy : pPresentInfo);
  }

  if (isFirstHook)
    g_InPresentHook = false;

  if (shm)
    shm->runtimeState.vulkanPresentThreadId.store(0, std::memory_order_release);

  return res;
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex) {

  DeviceDispatch *disp = VulkanLayerState::Get().GetDeviceDispatch(device);
  if (!disp || !disp->fp_vkAcquireNextImageKHR)
    return VK_ERROR_INITIALIZATION_FAILED;
  return disp->fp_vkAcquireNextImageKHR(device, swapchain, timeout, semaphore,
                                        fence, pImageIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSampler(
    VkDevice device, const VkSamplerCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSampler *pSampler) {

  DeviceDispatch *disp = VulkanLayerState::Get().GetDeviceDispatch(device);
  VkSamplerCreateInfo modified = *pCreateInfo;
  if (g_LayerState.whitelisted) {
    modified.maxAnisotropy = (float)VulkanLayerState::Get().GetMaxAnisotropy();
    modified.mipLodBias += VulkanLayerState::Get().GetMipLodBias();
  }
  if (disp && disp->fp_vkCreateSampler)
    return disp->fp_vkCreateSampler(device, &modified, pAllocator, pSampler);
  return VK_ERROR_INITIALIZATION_FAILED;
}

#ifdef VK_USE_PLATFORM_WIN32_KHR
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateWin32SurfaceKHR(
    VkInstance instance, const VkWin32SurfaceCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) {

  InstanceDispatch *disp =
      VulkanLayerState::Get().GetInstanceDispatch(instance);
  if (!disp || !disp->fp_vkCreateWin32SurfaceKHR)
    return VK_ERROR_INITIALIZATION_FAILED;

  VkResult res = disp->fp_vkCreateWin32SurfaceKHR(instance, pCreateInfo,
                                                  pAllocator, pSurface);
  if (res == VK_SUCCESS) {
    VulkanLayerState::Get().RegisterSurface(*pSurface, pCreateInfo->hwnd);
  }
  return res;
}
#endif
