/**
 * Custom Overlay - Vulkan Backend
 *
 * Renders overlay using Vulkan.
 * Note: The project uses a Vulkan layer for hooking, so this backend
 * requires dispatch table access because VK_NO_PROTOTYPES is defined.
 */

#pragma once

#include "custom_overlay.h"

// Vulkan headers - use dynamic loading with Win32 platform support
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace CustomOverlay {

class VulkanBackend : public RendererBackend {
public:
  VulkanBackend(VkDevice device, VkPhysicalDevice physDevice, VkQueue queue,
                uint32_t queueFamily);
  virtual ~VulkanBackend();

  // Set dispatch tables for Vulkan function calls (required before Initialize)
  // Uses void* to avoid circular dependency with vulkan_layer.h
  void SetDispatchTable(void* deviceDispatch, void* instanceDispatch);

  bool Initialize(int fontTextureWidth, int fontTextureHeight,
                  const uint8_t *fontTextureData) override;
  void Shutdown() override;

  // Called with render pass when it becomes available (swapchain created)
  bool CreatePipelineForRenderPass(VkRenderPass renderPass);

  void Render(const std::vector<DrawVertex> &vertices,
              const std::vector<uint16_t> &indices,
              const std::vector<DrawCommand> &commands, int viewportWidth,
              int viewportHeight) override;

  // Vulkan-specific: Set command buffer and render pass info before rendering
  void SetRenderContext(VkCommandBuffer cmdBuffer, VkRenderPass renderPass,
                        VkFramebuffer framebuffer, VkExtent2D extent);

private:
  bool CreateDescriptorSetLayout();
  bool CreatePipelineLayout();
  bool CreatePipeline(VkRenderPass renderPass);
  bool CreateFontTexture(int width, int height, const uint8_t *data);
  bool CreateBuffers();
  uint32_t FindMemoryType(uint32_t typeFilter,
                          VkMemoryPropertyFlags properties);

  VkDevice device = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queueFamilyIndex = 0;

  VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipeline pipelineSolid = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;

  VkImage fontImage = VK_NULL_HANDLE;
  VkDeviceMemory fontMemory = VK_NULL_HANDLE;
  VkImageView fontImageView = VK_NULL_HANDLE;

  VkBuffer vertexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
  VkBuffer indexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory indexMemory = VK_NULL_HANDLE;

  void *vertexBufferPtr = nullptr;
  void *indexBufferPtr = nullptr;
  size_t vertexBufferSize = 0;
  size_t indexBufferSize = 0;

  // Current render context
  VkCommandBuffer currentCmdBuffer = VK_NULL_HANDLE;
  VkRenderPass currentRenderPass = VK_NULL_HANDLE;
  VkFramebuffer currentFramebuffer = VK_NULL_HANDLE;
  VkExtent2D currentExtent = {};

  // Dispatch tables (casted from void* in .cpp to avoid circular dependency)
  void* deviceDispatch = nullptr;      // DeviceDispatch*
  void* instanceDispatch = nullptr;    // InstanceDispatch*

  bool initialized = false;
  bool pipelineCreated = false;
};

} // namespace CustomOverlay
