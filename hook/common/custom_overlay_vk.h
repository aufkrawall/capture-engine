/**
 * Custom Overlay - Vulkan Backend
 *
 * Renders overlay using Vulkan.
 * Note: The project uses a Vulkan layer for hooking, so this backend
 * requires dispatch table access because VK_NO_PROTOTYPES is defined.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include "custom_overlay.h"

// Vulkan headers - use dynamic loading with Win32 platform support
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

// Fallback forward declarations if vulkan.h wasn't found (LSP environments)
#ifndef VULKAN_H_
struct VkDevice_T;
struct VkPhysicalDevice_T;
struct VkQueue_T;
using VkDevice = VkDevice_T*;
using VkPhysicalDevice = VkPhysicalDevice_T*;
using VkQueue = VkQueue_T*;
using VkInstance = struct VkInstance_T*;
#endif

namespace CustomOverlay {

class VulkanBackend : public RendererBackend {
public:
    VulkanBackend(VkDevice device, VkPhysicalDevice physDevice, VkQueue queue, uint32_t queueFamily);
    virtual ~VulkanBackend();

    // Set dispatch tables for Vulkan function calls (required before Initialize)
    // Uses void* to avoid circular dependency with vulkan_layer.h
    void SetDispatchTable(void* deviceDispatch, void* instanceDispatch);

    bool Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) override;
    void Shutdown() override;

    // Called with render pass when it becomes available (swapchain created)
    bool CreatePipelineForRenderPass(VkRenderPass renderPass);

    void Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) override;

    // Vulkan-specific: Set command buffer and render pass info before rendering
    void SetRenderContext(VkCommandBuffer cmdBuffer, VkRenderPass renderPass, VkFramebuffer framebuffer,
                          VkExtent2D extent);

private:
    bool CreateDescriptorSetLayout();
    bool CreatePipelineLayout();
    bool CreatePipeline(VkRenderPass renderPass);
    bool CreateFontTexture(int width, int height, const uint8_t* data);
    bool CreateBuffers();
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

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

    // Per-frame buffer pool — prevents CPU/GPU data race on host-visible buffers.
    // Pool size matches the DX12 backend so fence guarantees that slot N is
    // GPU-idle before the CPU reuses it.
    static constexpr int kFramePoolSize = 16;
    VkBuffer vertexBuffer[kFramePoolSize] = {};
    VkDeviceMemory vertexMemory[kFramePoolSize] = {};
    VkBuffer indexBuffer[kFramePoolSize] = {};
    VkDeviceMemory indexMemory[kFramePoolSize] = {};

    void* vertexBufferPtr[kFramePoolSize] = {};
    void* indexBufferPtr[kFramePoolSize] = {};
    size_t vertexBufferSize[kFramePoolSize] = {};
    size_t indexBufferSize[kFramePoolSize] = {};
    std::atomic<int> frameIdx{0};

    // Current render context
    VkCommandBuffer currentCmdBuffer = VK_NULL_HANDLE;
    VkRenderPass currentRenderPass = VK_NULL_HANDLE;
    VkFramebuffer currentFramebuffer = VK_NULL_HANDLE;
    VkExtent2D currentExtent = {};

    // Dispatch tables (casted from void* in .cpp to avoid circular dependency)
    void* deviceDispatch = nullptr;    // DeviceDispatch*
    void* instanceDispatch = nullptr;  // InstanceDispatch*

    bool initialized = false;
    bool pipelineCreated = false;
};

}  // namespace CustomOverlay
