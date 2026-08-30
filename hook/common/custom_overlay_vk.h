/**
 * Custom Overlay - Vulkan Backend
 *
 * Renders overlay using Vulkan.
 * Note: The project uses a Vulkan layer for
 * hooking, so this backend
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

// Fallback forward declarations if vulkan.h wasn't found (LSP environments
// without Vulkan SDK). VULKAN_H_ is defined by the official vulkan.h header.
#ifndef VULKAN_H_
struct VkDevice_T;
struct VkPhysicalDevice_T;
struct VkQueue_T;
struct VkInstance_T;
struct VkImage_T;
struct VkImageView_T;
struct VkDeviceMemory_T;
struct VkBuffer_T;
struct VkSampler_T;
struct VkRenderPass_T;
struct VkFramebuffer_T;
struct VkCommandBuffer_T;
struct VkPipeline_T;
struct VkPipelineLayout_T;
struct VkDescriptorSetLayout_T;
struct VkDescriptorPool_T;
struct VkDescriptorSet_T;
using VkDevice = VkDevice_T*;
using VkPhysicalDevice = VkPhysicalDevice_T*;
using VkQueue = VkQueue_T*;
using VkInstance = VkInstance_T*;
using VkImage = VkImage_T*;
using VkImageView = VkImageView_T*;
using VkDeviceMemory = VkDeviceMemory_T*;
using VkBuffer = VkBuffer_T*;
using VkSampler = VkSampler_T*;
using VkRenderPass = VkRenderPass_T*;
using VkFramebuffer = VkFramebuffer_T*;
using VkCommandBuffer = VkCommandBuffer_T*;
using VkPipeline = VkPipeline_T*;
using VkPipelineLayout = VkPipelineLayout_T*;
using VkDescriptorSetLayout = VkDescriptorSetLayout_T*;
using VkDescriptorPool = VkDescriptorPool_T*;
using VkDescriptorSet = VkDescriptorSet_T*;
using VkMemoryPropertyFlags = uint32_t;
using VkExtent2D = struct {
    uint32_t width;
    uint32_t height;
};
#define VK_NULL_HANDLE nullptr
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
                          VkExtent2D extent, bool clearTransparent = false);
    VkRect2D GetLastRenderBounds() const {
        return lastRenderBounds;
    }

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
    // The index is free-running, so the pool is only safe while fewer overlay
    // submissions are in flight than it has entries. Public because it is the
    // hard ceiling on the Vulkan layer's submission ring: the layer asserts
    // ce::overlay_submit_queue_policy::kMaxSubmissionSlots against it. Each
    // entry is a small host-visible vertex/index pair, so depth here is cheap.
public:
    static constexpr int kFramePoolSize = 32;

private:
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
    VkRect2D lastRenderBounds = {};
    bool clearTransparentTarget = false;

    // Dispatch tables (casted from void* in .cpp to avoid circular dependency)
    void* deviceDispatch = nullptr;    // DeviceDispatch*
    void* instanceDispatch = nullptr;  // InstanceDispatch*

    bool initialized = false;
    bool pipelineCreated = false;
};

}  // namespace CustomOverlay
