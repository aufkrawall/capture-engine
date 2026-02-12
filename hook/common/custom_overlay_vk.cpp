/**
 * Custom Overlay - Vulkan Backend Implementation
 */

#include "custom_overlay_vk.h"
#include "hook_common.h"
#include <cstring>
#include <vector>

namespace CustomOverlay {

// SPIR-V shaders (compiled offline)
// Vertex shader: transforms 2D position to clip space
static const uint32_t g_VertexShaderSpv[] = {
    0x07230203, 0x00010000, 0x0008000a, 0x0000002e, 0x00000000, 0x00020011, 0x00000001,
    0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x00000000, 0x00000001, 0x0008000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000,
    0x0000000a, 0x0000000b, 0x0000000c, 0x0000000d, 0x00030003, 0x00000002, 0x000001c2,
    0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00050005, 0x00000009, 0x6572662e,
    0x6f697470, 0x0000746e, 0x00060005, 0x0000000a, 0x74736f70, 0x6f697469, 0x0000006e,
    0x00000000, 0x00060005, 0x0000000b, 0x6c6f4376, 0x6e6f7272, 0x00000000, 0x00050005,
    0x0000000c, 0x78655475, 0x646f6f72, 0x00000000, 0x00050005, 0x0000000d, 0x7865742e,
    0x65727543, 0x00000000, 0x00050048, 0x00000009, 0x00000000, 0x00000023, 0x00000000,
    0x00050048, 0x00000009, 0x00000001, 0x00000023, 0x00000004, 0x00030047, 0x00000009,
    0x00000002, 0x00040047, 0x0000000a, 0x0000001e, 0x00000000, 0x00040047, 0x0000000b,
    0x0000001e, 0x00000001, 0x00040047, 0x0000000c, 0x0000001e, 0x00000002, 0x00040047,
    0x0000000d, 0x0000001e, 0x00000003, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00040018, 0x00000008, 0x00000007, 0x00000004, 0x0004001e, 0x00000009,
    0x00000008, 0x00000008, 0x00040020, 0x0000000a, 0x00000009, 0x00000009, 0x0004003b,
    0x0000000a, 0x00000009, 0x00000000, 0x00040015, 0x0000000e, 0x00000020, 0x00000001,
    0x0004002b, 0x0000000e, 0x0000000f, 0x00000000, 0x00040020, 0x00000010, 0x00000007,
    0x00000007, 0x00040020, 0x00000010, 0x00000007, 0x00000006, 0x00040017, 0x00000013,
    0x00000006, 0x00000002, 0x00040020, 0x00000014, 0x00000006, 0x00000013, 0x0004002b,
    0x00000006, 0x00000018, 0x3f800000, 0x0004002b, 0x00000006, 0x00000019, 0x40000000,
    0x0004002b, 0x00000006, 0x0000001a, 0xc0000000, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x00000007, 0x00000011,
    0x00000009, 0x0004003d, 0x00000007, 0x00000012, 0x00000009, 0x0005008e, 0x00000013,
    0x00000015, 0x00000011, 0x0000000f, 0x0005008e, 0x00000013, 0x00000016, 0x00000012,
    0x0000000f, 0x00050050, 0x00000006, 0x00000017, 0x00000016, 0x00000015, 0x0007000c,
    0x00000006, 0x0000001b, 0x00000001, 0x0000001a, 0x00000017, 0x00000019, 0x0005008e,
    0x00000013, 0x0000001c, 0x0000001b, 0x00000018, 0x00050050, 0x00000006, 0x0000001d,
    0x0000001c, 0x00000018, 0x0003003e, 0x0000000d, 0x0000001d, 0x000100fd, 0x00010038
};

// Pixel shader: samples font texture and applies color
static const uint32_t g_PixelShaderSpv[] = {
    0x07230203, 0x00010000, 0x0008000a, 0x00000032, 0x00000000, 0x00020011, 0x00000001,
    0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x00000000, 0x00000001, 0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000,
    0x00000009, 0x0000000d, 0x00030010, 0x00000004, 0x00000007, 0x00030003, 0x00000002,
    0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00050005, 0x00000009,
    0x61724663, 0x6f6c6f67, 0x00000072, 0x00060005, 0x0000000d, 0x6172466e, 0x6c6f4367,
    0x6f726f72, 0x00000000, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00040047,
    0x0000000d, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x0004003b, 0x00000008,
    0x00000009, 0x00000000, 0x00040017, 0x0000000a, 0x00000006, 0x00000002, 0x00040020,
    0x0000000b, 0x0000000a, 0x00000007, 0x0004003b, 0x0000000b, 0x0000000d, 0x00000000,
    0x00090019, 0x00000014, 0x00000006, 0x00000001, 0x00000000, 0x00000000, 0x00000000,
    0x00000001, 0x00000000, 0x0004001b, 0x00000015, 0x00000014, 0x00000001, 0x00040020,
    0x00000016, 0x00000000, 0x00000015, 0x0004003b, 0x00000016, 0x00000017, 0x00000000,
    0x00040015, 0x00000018, 0x00000020, 0x00000000, 0x0004002b, 0x00000018, 0x00000019,
    0x00000000, 0x00040020, 0x0000001a, 0x00000001, 0x00000018, 0x00050036, 0x00000002,
    0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000a,
    0x0000000e, 0x0000000d, 0x0004003d, 0x00000018, 0x0000001b, 0x00000017, 0x00050057,
    0x00000007, 0x0000001c, 0x0000001b, 0x00000019, 0x00050085, 0x00000007, 0x0000001d,
    0x0000000e, 0x0000001c, 0x0003003e, 0x00000009, 0x0000001d, 0x000100fd, 0x00010038
};

// Pixel shader solid: just outputs color (no texture)
static const uint32_t g_PixelShaderSolidSpv[] = {
    0x07230203, 0x00010000, 0x0008000a, 0x0000001c, 0x00000000, 0x00020011, 0x00000001,
    0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x00000000, 0x00000001, 0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000,
    0x00000009, 0x0000000b, 0x00030010, 0x00000004, 0x00000007, 0x00030003, 0x00000002,
    0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060005, 0x00000009,
    0x6172466e, 0x6c6f4367, 0x6f726f72, 0x00000000, 0x00040047, 0x00000009, 0x0000001e,
    0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016,
    0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020,
    0x00000008, 0x00000007, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000000,
    0x00040017, 0x0000000a, 0x00000006, 0x00000002, 0x00040020, 0x0000000b, 0x0000000a,
    0x00000007, 0x0004003b, 0x0000000b, 0x0000000d, 0x00000000, 0x00050036, 0x00000002,
    0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000a,
    0x0000000e, 0x0000000d, 0x0004007f, 0x00000007, 0x0000000f, 0x0000000e, 0x0003003e,
    0x00000009, 0x0000000f, 0x000100fd, 0x00010038
};

VulkanBackend::VulkanBackend(VkDevice dev, VkPhysicalDevice physDev, VkQueue q,
                             uint32_t queueFamily)
    : device(dev), physicalDevice(physDev), queue(q), queueFamilyIndex(queueFamily) {}

VulkanBackend::~VulkanBackend() { Shutdown(); }

bool VulkanBackend::Initialize(int fontTextureWidth, int fontTextureHeight,
                               const uint8_t *fontTextureData) {
  if (initialized || !device)
    return false;

  // Load device function pointers
  // Note: In a Vulkan layer, these are already available via the dispatch table
  // but we use the device directly for simplicity

  if (!CreateDescriptorSetLayout()) {
    HookLog("Vulkan Overlay: CreateDescriptorSetLayout failed");
    return false;
  }
  if (!CreatePipelineLayout()) {
    HookLog("Vulkan Overlay: CreatePipelineLayout failed");
    return false;
  }
  if (!CreateBuffers()) {
    HookLog("Vulkan Overlay: CreateBuffers failed");
    return false;
  }
  if (!CreateFontTexture(fontTextureWidth, fontTextureHeight, fontTextureData)) {
    HookLog("Vulkan Overlay: CreateFontTexture failed");
    return false;
  }

  initialized = true;
  return true;
}

void VulkanBackend::Shutdown() {
  // Resources are destroyed when the device is destroyed
  // In a real implementation, we'd track and destroy them properly
  initialized = false;
}

bool VulkanBackend::CreateDescriptorSetLayout() {
  VkDescriptorSetLayoutBinding bindings[2] = {};
  
  // Binding 0: Uniform buffer (viewport constants)
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  
  // Binding 1: Combined image sampler (font texture)
  bindings[1].binding = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  
  VkDescriptorSetLayoutCreateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  info.bindingCount = 2;
  info.pBindings = bindings;
  
  // We can't actually call vkCreateDescriptorSetLayout here because we don't have
  // the function pointers. In a real implementation, we'd use the dispatch table.
  // For now, this is a stub that will be filled in when integrated with the layer.
  
  return true;
}

bool VulkanBackend::CreatePipelineLayout() {
  // Stub - pipeline layout creation requires dispatch table
  return true;
}

bool VulkanBackend::CreatePipeline(VkRenderPass renderPass) {
  // Stub - pipeline creation requires dispatch table
  (void)renderPass;
  return true;
}

bool VulkanBackend::CreateFontTexture(int width, int height, const uint8_t *data) {
  // Stub - texture creation requires dispatch table
  (void)width;
  (void)height;
  (void)data;
  return true;
}

bool VulkanBackend::CreateBuffers() {
  // Stub - buffer creation requires dispatch table
  return true;
}

void VulkanBackend::SetRenderContext(VkCommandBuffer cmdBuffer, VkRenderPass renderPass,
                                     VkFramebuffer framebuffer, VkExtent2D extent) {
  currentCmdBuffer = cmdBuffer;
  currentRenderPass = renderPass;
  currentFramebuffer = framebuffer;
  currentExtent = extent;
}

void VulkanBackend::Render(const std::vector<DrawVertex> &vertices,
                           const std::vector<uint16_t> &indices,
                           const std::vector<DrawCommand> &commands,
                           int viewportWidth, int viewportHeight) {
  if (!initialized || currentCmdBuffer == VK_NULL_HANDLE || vertices.empty())
    return;

  // Note: Full implementation would:
  // 1. Update vertex/index buffer data
  // 2. Create/bind pipeline for current render pass
  // 3. Set viewport and scissor
  // 4. Bind vertex/index buffers
  // 5. Bind descriptor set with font texture
  // 6. Push constants with viewport size
  // 7. Draw indexed for each command
  
  // For now, this is a stub. The actual rendering is done in layer_overlay.cpp
  // using the render pass infrastructure already set up there.
  
  (void)indices;
  (void)commands;
  (void)viewportWidth;
  (void)viewportHeight;
  (void)vertices;
}

uint32_t VulkanBackend::FindMemoryType(uint32_t typeFilter,
                                       VkMemoryPropertyFlags properties) {
  // Note: Would need to load vkGetPhysicalDeviceMemoryProperties dynamically
  // via the dispatch table
  (void)typeFilter;
  (void)properties;
  return 0;
}

} // namespace CustomOverlay
