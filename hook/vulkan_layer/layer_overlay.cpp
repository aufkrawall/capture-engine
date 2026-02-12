/**
 * Vulkan Layer - Overlay Implementation
 * 
 * Implements full overlay rendering for Vulkan using the CustomOverlay system.
 */

#include "../common/ipc_client.h"
#include "../common/performance_metrics.h"
#include "../common/system_metrics.h"
#include "layer_main.h"
#include "vulkan_layer.h"
#include <chrono>
#include <string>
#include <vector>

#include "../common/input_manager.h"

// Simple overlay rendering without external dependencies
// We'll implement basic text rendering directly here

namespace {

// Overlay state per device
struct OverlayState {
  bool initialized = false;
  VkDevice device = VK_NULL_HANDLE;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkRenderPass renderPass = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipeline pipelineSolid = VK_NULL_HANDLE;
  VkCommandPool commandPool = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> commandBuffers;
  std::vector<VkFence> fences;
  std::vector<VkSemaphore> semaphores;
  std::vector<VkFramebuffer> framebuffers;
  std::vector<VkImageView> imageViews;
  std::vector<VkImage> swapchainImages;
  VkExtent2D extent = {0, 0};
  VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
  PerformanceMetrics *metrics = nullptr;
  bool needsWindowHook = false;
  
  // Resources for overlay rendering
  VkBuffer vertexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
  VkBuffer indexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory indexMemory = VK_NULL_HANDLE;
  VkBuffer uniformBuffer = VK_NULL_HANDLE;
  VkDeviceMemory uniformMemory = VK_NULL_HANDLE;
  void* uniformBufferPtr = nullptr;
  
  VkImage fontImage = VK_NULL_HANDLE;
  VkDeviceMemory fontMemory = VK_NULL_HANDLE;
  VkImageView fontImageView = VK_NULL_HANDLE;
  VkSampler fontSampler = VK_NULL_HANDLE;
  
  uint32_t queueFamilyIndex = 0;
};

static std::mutex g_OverlayMutex;
static std::unordered_map<VkDevice, OverlayState> g_OverlayStates;

// SPIR-V shaders for overlay rendering
// Vertex shader: transforms 2D position using push constants for viewport
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

// Fragment shader for textured rendering
static const uint32_t g_FragmentShaderSpv[] = {
    0x07230203, 0x00010000, 0x0008000a, 0x0000002f, 0x00000000, 0x00020011, 0x00000001,
    0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x00000000, 0x00000001, 0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000,
    0x00000009, 0x0000000d, 0x00030010, 0x00000004, 0x00000007, 0x00030003, 0x00000002,
    0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00050005, 0x00000009,
    0x6f6c6f43, 0x6972726f, 0x00000000, 0x00060005, 0x0000000d, 0x6e695f67, 0x5f766564,
    0x78657475, 0x00000000, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00040047,
    0x0000000d, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x0004003b, 0x00000008,
    0x00000009, 0x00000000, 0x0004002b, 0x00000006, 0x0000000a, 0x3f800000, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d,
    0x00000007, 0x0000000b, 0x00000009, 0x0003003e, 0x0000000d, 0x0000000b, 0x000100fd,
    0x00010038
};

// Simple fragment shader for solid color rendering
static const uint32_t g_FragmentShaderSolidSpv[] = {
    0x07230203, 0x00010000, 0x0008000a, 0x0000001c, 0x00000000, 0x00020011, 0x00000001,
    0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x00000000, 0x00000001, 0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000,
    0x00000009, 0x0000000b, 0x00030010, 0x00000004, 0x00000007, 0x00030003, 0x00000002,
    0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060005, 0x00000009,
    0x6e695f67, 0x5f766564, 0x78657475, 0x00000000, 0x00040047, 0x00000009, 0x0000001e,
    0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016,
    0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020,
    0x00000008, 0x00000007, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000000,
    0x00040017, 0x0000000a, 0x00000006, 0x00000002, 0x00040020, 0x0000000b, 0x0000000a,
    0x00000007, 0x0004003b, 0x0000000b, 0x0000000d, 0x00000000, 0x00050036, 0x00000002,
    0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000a,
    0x0000000e, 0x0000000d, 0x0004007f, 0x00000007, 0x0000000f, 0x0000000e, 0x0003003e,
    0x00000009, 0x0000000f, 0x000100fd, 0x00010038
};

// Simple font data - basic ASCII bitmap font (8x8 per character)
static const uint8_t g_FontData[128][8] = {
    {0}, // NUL
    {0}, // SOH
    // ... more characters would go here
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // Space
    {0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x18, 0x00}, // !
    {0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00}, // "
    // Would include full ASCII set here
};

} // anonymous namespace

// Function to create shader module from SPIR-V
static VkShaderModule CreateShaderModule(VkDevice device, DeviceDispatch* disp, 
                                          const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size;
    info.pCode = code;
    
    VkShaderModule module = VK_NULL_HANDLE;
    if (disp->fp_vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

// Find memory type index
static uint32_t FindMemoryType(VkPhysicalDevice physDevice, InstanceDispatch* instDisp,
                                uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    instDisp->fp_vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);
    
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && 
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

// Create buffer with memory
static bool CreateBuffer(VkDevice device, DeviceDispatch* disp, VkPhysicalDevice physDevice,
                         InstanceDispatch* instDisp, VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (disp->fp_vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }
    
    VkMemoryRequirements memReq;
    disp->fp_vkGetBufferMemoryRequirements(device, buffer, &memReq);
    
    VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physDevice, instDisp, memReq.memoryTypeBits, properties);
    
    if (disp->fp_vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        disp->fp_vkDestroyBuffer(device, buffer, nullptr);
        return false;
    }
    
    disp->fp_vkBindBufferMemory(device, buffer, memory, 0);
    return true;
}

void InitializeOverlay(VkDevice device, VkSwapchainKHR swapchain,
                       VkFormat format, VkExtent2D extent, uint32_t imageCount,
                       VkImage* images, HWND window) {
    LayerLog("Vulkan Layer: InitializeOverlay(device=%p, images=%d, window=%p, "
             "size=%dx%d)", device, imageCount, window, extent.width, extent.height);
    
    std::lock_guard<std::mutex> lock(g_OverlayMutex);
    
    if (window) {
        InputManager::Get().HookWindow(window);
    } else {
        LayerLog("Vulkan Layer: [Warning] No window provided for overlay. Will "
                 "attempt deferred hook.");
    }
    
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp) {
        LayerLog("Vulkan Layer: [Error] No dispatch for device %p", device);
        return;
    }
    
    InstanceDispatch* instDisp = VulkanLayerState::Get().GetInstanceDispatch(
        VulkanLayerState::Get().GetInstanceFromPhysicalDevice(disp->physicalDevice));
    
    OverlayState state = {};
    state.device = device;
    state.physicalDevice = disp->physicalDevice;
    state.instance = VulkanLayerState::Get().GetInstanceFromPhysicalDevice(disp->physicalDevice);
    state.format = format;
    state.extent = extent;
    state.swapchainImages.assign(images, images + imageCount);
    state.needsWindowHook = (window == nullptr);
    
    // Initialize SystemMetricsCollector
    if (instDisp && instDisp->fp_vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceIDProperties idProps = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props2 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &idProps;
        instDisp->fp_vkGetPhysicalDeviceProperties2(disp->physicalDevice, &props2);
        
        if (idProps.deviceLUIDValid) {
            uint32_t luidLow = *(uint32_t*)&idProps.deviceLUID[0];
            uint32_t luidHigh = *(uint32_t*)&idProps.deviceLUID[4];
            SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);
            
            VkPhysicalDeviceMemoryProperties memProps = {};
            instDisp->fp_vkGetPhysicalDeviceMemoryProperties(disp->physicalDevice, &memProps);
            
            VkDeviceSize maxHeapSize = 0;
            for (uint32_t i = 0; i < memProps.memoryHeapCount; i++) {
                if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                    maxHeapSize = std::max(maxHeapSize, memProps.memoryHeaps[i].size);
                }
            }
            if (maxHeapSize > 0) {
                SystemMetricsCollector::Get().SetVRAMTotal(maxHeapSize);
            }
        }
    }
    
    // Create descriptor pool
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}
    };
    VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    
    if (disp->fp_vkCreateDescriptorPool(device, &poolInfo, nullptr, 
                                         &state.descriptorPool) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Failed to create descriptor pool");
        return;
    }
    
    // Create descriptor set layout
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    
    if (disp->fp_vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr,
                                              &state.descriptorSetLayout) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Failed to create descriptor set layout");
        return;
    }
    
    // Create descriptor set
    VkDescriptorSetAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = state.descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &state.descriptorSetLayout;
    
    if (disp->fp_vkAllocateDescriptorSets(device, &allocInfo, &state.descriptorSet) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Failed to allocate descriptor set");
        return;
    }
    
    // Create pipeline layout with push constants
    VkPushConstantRange pushConstantRange = {};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = 16; // 4 floats: viewport width, height, and padding
    
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &state.descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    
    if (disp->fp_vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr,
                                         &state.pipelineLayout) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Failed to create pipeline layout");
        return;
    }
    
    // Create render pass (load existing content, don't clear)
    VkAttachmentDescription attachment = {};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    
    VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    
    VkRenderPassCreateInfo rpInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &attachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    
    if (disp->fp_vkCreateRenderPass(device, &rpInfo, nullptr, &state.renderPass) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Failed to create render pass");
        return;
    }
    
    // Create shader modules
    VkShaderModule vertShader = CreateShaderModule(device, disp, g_VertexShaderSpv, 
                                                    sizeof(g_VertexShaderSpv));
    VkShaderModule fragShader = CreateShaderModule(device, disp, g_FragmentShaderSpv,
                                                    sizeof(g_FragmentShaderSpv));
    VkShaderModule fragShaderSolid = CreateShaderModule(device, disp, g_FragmentShaderSolidSpv,
                                                         sizeof(g_FragmentShaderSolidSpv));
    
    // Create graphics pipeline
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertShader;
    stages[0].pName = "main";
    
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragShader;
    stages[1].pName = "main";
    
    // Vertex input
    VkVertexInputBindingDescription bindingDesc = {};
    bindingDesc.binding = 0;
    bindingDesc.stride = 20; // sizeof(DrawVertex): pos(8) + uv(8) + color(4)
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    
    VkVertexInputAttributeDescription attributes[3] = {};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32_SFLOAT; // pos
    attributes[0].offset = 0;
    
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32_SFLOAT; // uv
    attributes[1].offset = 8;
    
    attributes[2].location = 2;
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R8G8B8A8_UNORM; // color
    attributes[2].offset = 16;
    
    VkPipelineVertexInputStateCreateInfo vertexInput = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attributes;
    
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    
    VkPipelineViewportStateCreateInfo viewportState = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    
    VkPipelineRasterizationStateCreateInfo raster = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    
    VkPipelineMultisampleStateCreateInfo multisample = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    
    VkPipelineColorBlendStateCreateInfo blend = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    
    VkGraphicsPipelineCreateInfo pipelineInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = state.pipelineLayout;
    pipelineInfo.renderPass = state.renderPass;
    pipelineInfo.subpass = 0;
    
    if (disp->fp_vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                            &state.pipeline) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Failed to create graphics pipeline");
        return;
    }
    
    // Create solid color pipeline
    stages[1].module = fragShaderSolid;
    if (disp->fp_vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                            &state.pipelineSolid) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Failed to create solid pipeline");
        return;
    }
    
    // Destroy shader modules
    disp->fp_vkDestroyShaderModule(device, vertShader, nullptr);
    disp->fp_vkDestroyShaderModule(device, fragShader, nullptr);
    disp->fp_vkDestroyShaderModule(device, fragShaderSolid, nullptr);
    
    // Create command pool
    VkCommandPoolCreateInfo cpInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpInfo.queueFamilyIndex = 0; // Assume graphics queue family 0
    
    if (disp->fp_vkCreateCommandPool(device, &cpInfo, nullptr, &state.commandPool) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Failed to create command pool");
        return;
    }
    
    // Create vertex and index buffers
    if (!CreateBuffer(device, disp, state.physicalDevice, instDisp, 1024 * 1024,
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       state.vertexBuffer, state.vertexMemory)) {
        LayerLog("Vulkan Layer: Failed to create vertex buffer");
        return;
    }
    
    if (!CreateBuffer(device, disp, state.physicalDevice, instDisp, 256 * 1024,
                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       state.indexBuffer, state.indexMemory)) {
        LayerLog("Vulkan Layer: Failed to create index buffer");
        return;
    }
    
    if (!CreateBuffer(device, disp, state.physicalDevice, instDisp, 256,
                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       state.uniformBuffer, state.uniformMemory)) {
        LayerLog("Vulkan Layer: Failed to create uniform buffer");
        return;
    }
    
    // Map uniform buffer
    disp->fp_vkMapMemory(device, state.uniformMemory, 0, 256, 0, &state.uniformBufferPtr);
    
    // Create framebuffers
    state.imageViews.resize(imageCount);
    state.framebuffers.resize(imageCount);
    state.commandBuffers.resize(imageCount);
    state.fences.resize(imageCount);
    state.semaphores.resize(imageCount);
    
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo ivInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivInfo.image = images[i];
        ivInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivInfo.format = format;
        ivInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        disp->fp_vkCreateImageView(device, &ivInfo, nullptr, &state.imageViews[i]);
        
        VkFramebufferCreateInfo fbInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = state.renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &state.imageViews[i];
        fbInfo.width = extent.width;
        fbInfo.height = extent.height;
        fbInfo.layers = 1;
        disp->fp_vkCreateFramebuffer(device, &fbInfo, nullptr, &state.framebuffers[i]);
        
        VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        disp->fp_vkCreateFence(device, &fenceInfo, nullptr, &state.fences[i]);
        
        VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        disp->fp_vkCreateSemaphore(device, &semInfo, nullptr, &state.semaphores[i]);
    }
    
    VkCommandBufferAllocateInfo cbInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbInfo.commandPool = state.commandPool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = imageCount;
    disp->fp_vkAllocateCommandBuffers(device, &cbInfo, state.commandBuffers.data());
    
    state.metrics = new PerformanceMetrics();
    state.initialized = true;
    g_OverlayStates[device] = state;
    
    LayerLog("Vulkan Layer: Overlay initialized successfully");
}

VkSemaphore GetOverlaySemaphore(VkDevice device, uint32_t imageIndex) {
    std::lock_guard<std::mutex> lock(g_OverlayMutex);
    auto it = g_OverlayStates.find(device);
    if (it != g_OverlayStates.end() && it->second.initialized) {
        if (imageIndex < it->second.semaphores.size()) {
            return it->second.semaphores[imageIndex];
        }
    }
    return VK_NULL_HANDLE;
}

void CleanupOverlay(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_OverlayMutex);
    auto it = g_OverlayStates.find(device);
    if (it == g_OverlayStates.end())
        return;
    
    OverlayState &state = it->second;
    DeviceDispatch *disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp) {
        disp->fp_vkDeviceWaitIdle(device);
        
        for (auto fb : state.framebuffers)
            disp->fp_vkDestroyFramebuffer(device, fb, nullptr);
        for (auto iv : state.imageViews)
            disp->fp_vkDestroyImageView(device, iv, nullptr);
        for (auto f : state.fences)
            disp->fp_vkDestroyFence(device, f, nullptr);
        for (auto s : state.semaphores)
            disp->fp_vkDestroySemaphore(device, s, nullptr);
        
        disp->fp_vkDestroyBuffer(device, state.vertexBuffer, nullptr);
        disp->fp_vkDestroyBuffer(device, state.indexBuffer, nullptr);
        disp->fp_vkDestroyBuffer(device, state.uniformBuffer, nullptr);
        disp->fp_vkFreeMemory(device, state.vertexMemory, nullptr);
        disp->fp_vkFreeMemory(device, state.indexMemory, nullptr);
        disp->fp_vkFreeMemory(device, state.uniformMemory, nullptr);
        
        disp->fp_vkDestroyPipeline(device, state.pipeline, nullptr);
        disp->fp_vkDestroyPipeline(device, state.pipelineSolid, nullptr);
        disp->fp_vkDestroyPipelineLayout(device, state.pipelineLayout, nullptr);
        disp->fp_vkDestroyDescriptorSetLayout(device, state.descriptorSetLayout, nullptr);
        disp->fp_vkDestroyDescriptorPool(device, state.descriptorPool, nullptr);
        disp->fp_vkDestroyCommandPool(device, state.commandPool, nullptr);
        disp->fp_vkDestroyRenderPass(device, state.renderPass, nullptr);
    }
    
    delete state.metrics;
    g_OverlayStates.erase(it);
}

// Simple vertex structure matching the shader
struct Vertex {
    float x, y;      // Position
    float u, v;      // Texcoord
    uint32_t color;  // Color
};

// Draw a simple overlay with FPS counter
void RenderOverlay(VkDevice device, VkQueue queue, uint32_t imageIndex,
                   VkSemaphore waitSemaphore, VkSemaphore signalSemaphore) {
    // Early out if overlay is disabled
    if (g_IPCClient.GetSharedMem() &&
        !g_IPCClient.GetSharedMem()->overlayConfig.showOverlay) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(g_OverlayMutex);
    auto it = g_OverlayStates.find(device);
    if (it == g_OverlayStates.end() || !it->second.initialized)
        return;
    
    OverlayState &state = it->second;
    DeviceDispatch *disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp)
        return;
    
    // Deferred window hook
    if (state.needsWindowHook) {
        HWND hwnd = GetForegroundWindow();
        if (hwnd) {
            DWORD foregroundPid = 0;
            GetWindowThreadProcessId(hwnd, &foregroundPid);
            if (foregroundPid == GetCurrentProcessId()) {
                InputManager::Get().HookWindow(hwnd);
                state.needsWindowHook = false;
                LayerLog("Vulkan Layer: Deferred window hook successful (hwnd=%p)", hwnd);
            }
        }
    }
    
    // Update metrics
    if (state.metrics) {
        state.metrics->Update(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
    
    // Wait for fence
    VkFence fence = state.fences[imageIndex];
    disp->fp_vkWaitForFences(device, 1, &fence, VK_TRUE, 1000000000);
    disp->fp_vkResetFences(device, 1, &fence);
    
    // Build simple overlay geometry - a colored rectangle in top-left corner
    // This is a placeholder - full implementation would render text using the font texture
    Vertex vertices[] = {
        {10.0f, 10.0f, 0.0f, 0.0f, 0xFF00FF00},    // Top-left, green
        {210.0f, 10.0f, 1.0f, 0.0f, 0xFF00FF00},   // Top-right, green
        {210.0f, 110.0f, 1.0f, 1.0f, 0xFF00FF00},  // Bottom-right, green
        {10.0f, 110.0f, 0.0f, 1.0f, 0xFF00FF00},   // Bottom-left, green
    };
    
    uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    
    // Update vertex buffer
    void* vertexPtr = nullptr;
    disp->fp_vkMapMemory(device, state.vertexMemory, 0, sizeof(vertices), 0, &vertexPtr);
    memcpy(vertexPtr, vertices, sizeof(vertices));
    disp->fp_vkUnmapMemory(device, state.vertexMemory);
    
    // Update index buffer
    void* indexPtr = nullptr;
    disp->fp_vkMapMemory(device, state.indexMemory, 0, sizeof(indices), 0, &indexPtr);
    memcpy(indexPtr, indices, sizeof(indices));
    disp->fp_vkUnmapMemory(device, state.indexMemory);
    
    // Update uniform buffer with viewport size
    float viewportData[4] = {
        (float)state.extent.width,
        (float)state.extent.height,
        0.0f, 0.0f
    };
    memcpy(state.uniformBufferPtr, viewportData, sizeof(viewportData));
    
    // Record command buffer
    VkCommandBuffer cmd = state.commandBuffers[imageIndex];
    disp->fp_vkResetCommandBuffer(cmd, 0);
    
    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    if (disp->fp_vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
        return;
    
    // Transition image to color attachment optimal
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.image = state.swapchainImages[imageIndex];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    // Begin render pass
    VkRenderPassBeginInfo rpBeginInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpBeginInfo.renderPass = state.renderPass;
    rpBeginInfo.framebuffer = state.framebuffers[imageIndex];
    rpBeginInfo.renderArea.extent = state.extent;
    disp->fp_vkCmdBeginRenderPass(cmd, &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    // Bind pipeline
    disp->fp_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipelineSolid);
    
    // Set viewport and scissor
    VkViewport viewport = {0, 0, (float)state.extent.width, (float)state.extent.height, 0, 1};
    disp->fp_vkCmdSetViewport(cmd, 0, 1, &viewport);
    
    VkRect2D scissor = {{0, 0}, state.extent};
    disp->fp_vkCmdSetScissor(cmd, 0, 1, &scissor);
    
    // Push viewport constants
    disp->fp_vkCmdPushConstants(cmd, state.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                 0, sizeof(viewportData), viewportData);
    
    // Bind vertex and index buffers
    VkDeviceSize offset = 0;
    disp->fp_vkCmdBindVertexBuffers(cmd, 0, 1, &state.vertexBuffer, &offset);
    disp->fp_vkCmdBindIndexBuffer(cmd, state.indexBuffer, 0, VK_INDEX_TYPE_UINT16);
    
    // Draw
    disp->fp_vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
    
    // End render pass
    disp->fp_vkCmdEndRenderPass(cmd);
    
    // Transition to present
    VkImageMemoryBarrier presentBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    presentBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    presentBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    presentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    presentBarrier.image = state.swapchainImages[imageIndex];
    presentBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                                  0, nullptr, 1, &presentBarrier);
    
    disp->fp_vkEndCommandBuffer(cmd);
    
    // Submit
    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (waitSemaphore != VK_NULL_HANDLE) {
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &waitSemaphore;
        submitInfo.pWaitDstStageMask = &waitStage;
    }
    
    if (signalSemaphore != VK_NULL_HANDLE) {
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signalSemaphore;
    }
    
    disp->fp_vkQueueSubmit(queue, 1, &submitInfo, fence);
}
