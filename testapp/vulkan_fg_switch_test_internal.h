#pragma once

namespace testapp::vkfg {
namespace {
struct LayoutAccess;
}
}

namespace testapp::vkfg {
namespace {
struct CpuTimingAccumulator;
}
}

#if defined(__clang__)
// Vulkan's canonical {sType} initialization intentionally zero-initializes every remaining field.
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
// testapp_common.h owns the printf-style logger and is shared with existing test applications.
#pragma clang diagnostic ignored "-Wmissing-format-attribute"
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif

#include "vulkan_fg_switch_common.h"

#include <algorithm>

#include <cstdio>

#include <cstdlib>

#include <cstring>

using testapp::vkfg::g_App;

#include "vulkan_fg_shaders.h"

namespace testapp::vkfg {
const char* VkResultName(VkResult result);
}

namespace testapp::vkfg {
const char* SlResultName(sl::Result result);
}

namespace testapp::vkfg {
const char* FfxResultName(ffxReturnCode_t result);
}

namespace testapp::vkfg {
const char* ModeName(FgMode mode);
}

namespace testapp::vkfg {
const char* TransitionStageName(TransitionStage stage);
}

namespace testapp::vkfg {
void LogTransition(const char* event, bool flush);
}

namespace testapp::vkfg {
void LogDeviceFault(const char* reason);
}

namespace testapp::vkfg {
bool InitializeStreamlineBeforeVulkan();
}

namespace testapp::vkfg {
bool ConfigureStreamlineAfterDevice();
}

namespace testapp::vkfg {
bool SetStreamlineFeaturesLoaded(bool loaded, const char* reason);
}

namespace testapp::vkfg {
bool ConfigureDlssSuperResolution(bool enabled);
}

namespace testapp::vkfg {
bool PrepareStreamlineMode();
}

namespace testapp::vkfg {
bool RetireStreamlinePresentation(SwapchainOwner nextOwner, const char* reason);
}

namespace testapp::vkfg {
bool SetDlssFrameGeneration(bool enabled, const char* reason);
}

namespace testapp::vkfg {
sl::FrameToken* BeginStreamlineFrame();
}

namespace testapp::vkfg {
void SetStreamlineMarker(sl::FrameToken* token, sl::PCLMarker marker, const char* name);
}

namespace testapp::vkfg {
bool RecordStreamlineInputsAndUpscale(VkCommandBuffer commandBuffer, FrameResources& resources, sl::FrameToken* frameToken, const JitterOffset& jitter, uint32_t backbufferIndex);
}

namespace testapp::vkfg {
void PollStreamlineState();
}

namespace testapp::vkfg {
void ShutdownStreamline();
}

namespace testapp::vkfg {
bool SetReflexMode(bool enabled, const char* reason);
void PollReflexState(bool force);
}

namespace testapp::vkfg {
bool InitializeVulkanDevice();
}

namespace testapp::vkfg {
VkQueue ApplicationPresentQueue();
}

namespace testapp::vkfg {
const VulkanQueueRef& ApplicationPresentQueueRef();
}

namespace testapp::vkfg {
void QueryMemoryBudgetStress();
}

namespace testapp::vkfg {
void ReleaseVulkanSurfaceBeforeStreamlineShutdown();
}

namespace testapp::vkfg {
void ShutdownVulkanDevice();
}

namespace testapp::vkfg {
void StartFidelityFxRuntimePreload(const char* reason);
}

namespace testapp::vkfg {
void JoinFidelityFxRuntimePreload(const char* reason);
}

namespace testapp::vkfg {
bool LoadFidelityFxRuntime(const char* reason);
}

namespace testapp::vkfg {
bool PrepareFidelityFxMode();
}

namespace testapp::vkfg {
bool RecreateFidelityFxEffectsForExtent();
}

namespace testapp::vkfg {
void ReleaseFidelityFxEffectsForExtent(const char* reason);
}

namespace testapp::vkfg {
bool CreateFidelityFxSwapchain(VkSwapchainKHR oldSwapchain, const VkSwapchainCreateInfoKHR& createInfo, VkSwapchainKHR* replacement, bool* oldSwapchainConsumed);
}

namespace testapp::vkfg {
bool SetFsrFrameGeneration(bool enabled, const char* reason, bool forceLog);
}

namespace testapp::vkfg {
bool WaitForFsrPresents(const char* reason);
}

namespace testapp::vkfg {
FfxApiResource MakeFfxResource(const ImageResource& image, uint32_t state, uint32_t additionalUsage = 0);
}

namespace testapp::vkfg {
void RegisterFsrUiResource(FrameResources& resources);
}

namespace testapp::vkfg {
bool RecordFsrUpscaleAndPrepare(VkCommandBuffer commandBuffer, FrameResources& resources, const JitterOffset& jitter);
}

namespace testapp::vkfg {
void DestroyFidelityFxContexts(bool unloadRuntime, const char* reason, bool presentationAlreadyRetired);
}

namespace testapp::vkfg {
bool DrainSwapchainBoundWork(const char* reason);
}

namespace testapp::vkfg {
void DestroySwapchainState(bool destroyHandle);
}

namespace testapp::vkfg {
bool CreateOrReplaceSwapchain(SwapchainOwner owner, const char* reason);
}

namespace testapp::vkfg {
bool RecreateCurrentSwapchain(const char* reason);
}

namespace testapp::vkfg {
VkResult AcquireSwapchainImage(FrameContext& frame, uint32_t* imageIndex);
}

namespace testapp::vkfg {
VkResult PresentSwapchainImage(uint32_t imageIndex);
}

namespace testapp::vkfg {
bool InitializeRenderer();
}

namespace testapp::vkfg {
void ShutdownRenderer();
}

namespace testapp::vkfg {
bool RecreateRendererForExtent();
}

namespace testapp::vkfg {
bool SetModeFeatureState(FgMode mode, bool enabled, const char* reason);
}

namespace testapp::vkfg {
bool RequestMode(FgMode mode, const char* reason);
}

namespace testapp::vkfg {
void DriveTransitionBeforeFrame();
}

namespace testapp::vkfg {
void OnFramePresentedSuccessfully();
}

namespace testapp::vkfg {
bool RenderFrame();
}

int main(int argc, char* argv[]);

namespace testapp::vkfg {
namespace {
template <typename T>
T* SlExport(const char* name) {
    return reinterpret_cast<T*>(GetProcAddress(g_App.sl.module, name));
}
}
}

namespace testapp::vkfg {
namespace {
template <typename T>
T* SlFeatureFunction(sl::Feature feature, const char* name) {
    if (!g_App.sl.getFeatureFunction) {
        return nullptr;
    }
    void* function = nullptr;
    const sl::Result result = g_App.sl.getFeatureFunction(feature, name, function);
    testapp::Log("[FG-DIAG] slGetFeatureFunction feature=%u name=%s result=%d(%s) function=%p\n",
                 static_cast<unsigned>(feature), name, static_cast<int>(result), SlResultName(result), function);
    return result == sl::Result::eOk ? reinterpret_cast<T*>(function) : nullptr;
}
}
}

namespace testapp::vkfg {
namespace {
template <typename T>
bool ContainsName(const std::vector<T>& values, const char* name) {
    for (const T& value : values) {
        if (std::strcmp(value.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}
}
}

namespace testapp::vkfg {
namespace {
template <typename T>
bool RequiredFeatureTailSupported(const T& requested, const T& supported,
                                  const VkBool32* requestedFirst, const VkBool32* supportedFirst) {
    const auto* end = reinterpret_cast<const uint8_t*>(&requested) + sizeof(T);
    const size_t count = static_cast<size_t>(end - reinterpret_cast<const uint8_t*>(requestedFirst)) /
                         sizeof(VkBool32);
    for (size_t index = 0; index < count; ++index) {
        if (requestedFirst[index] && !supportedFirst[index]) {
            return false;
        }
    }
    return true;
}
}
}

namespace testapp::vkfg {
namespace {
inline bool CreateSwapchainFramebuffers(SwapchainState* state) {
    if (!state || g_App.renderer.swapchainRenderPass == VK_NULL_HANDLE) {
        return true;
    }
    state->framebuffers.resize(state->views.size(), VK_NULL_HANDLE);
    for (size_t index = 0; index < state->views.size(); ++index) {
        VkFramebufferCreateInfo framebufferInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = g_App.renderer.swapchainRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &state->views[index];
        framebufferInfo.width = state->extent.width;
        framebufferInfo.height = state->extent.height;
        framebufferInfo.layers = 1;
        const VkResult result =
            vkCreateFramebuffer(g_App.vk.device, &framebufferInfo, nullptr, &state->framebuffers[index]);
        if (result != VK_SUCCESS) {
            testapp::Log("[FG-DIAG] vkCreateFramebuffer(swapchain index=%zu) result=%s(%d)\n", index,
                         VkResultName(result), static_cast<int>(result));
            return false;
        }
    }
    return true;
}
}
}

namespace testapp::vkfg {
namespace {
inline uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) {
    for (uint32_t index = 0; index < g_App.vk.memoryProperties.memoryTypeCount; ++index) {
        if ((typeBits & (1u << index)) != 0 &&
            (g_App.vk.memoryProperties.memoryTypes[index].propertyFlags & required) == required) {
            return index;
        }
    }
    return UINT32_MAX;
}
}
}

namespace testapp::vkfg {
namespace {
inline void SetObjectName(VkObjectType type, uint64_t handle, const char* name) {
    if (!g_App.config.apiDebug || !name || !handle) {
        return;
    }
    auto setName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetDeviceProcAddr(g_App.vk.device, "vkSetDebugUtilsObjectNameEXT"));
    if (!setName) {
        return;
    }
    VkDebugUtilsObjectNameInfoEXT info = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    setName(g_App.vk.device, &info);
}
}
}

namespace testapp::vkfg {
namespace {
inline bool CreateImageResource(ImageResource* resource, uint32_t width, uint32_t height, VkFormat format,
                         VkImageUsageFlags usage, VkImageAspectFlags aspect, const char* name) {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    resource->createInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    resource->createInfo.imageType = VK_IMAGE_TYPE_2D;
    resource->createInfo.format = format;
    resource->createInfo.extent = {width, height, 1};
    resource->createInfo.mipLevels = 1;
    resource->createInfo.arrayLayers = 1;
    resource->createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    resource->createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    resource->createInfo.usage = usage;
    resource->createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    resource->createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    resource->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    resource->aspect = aspect;
    VkResult result = vkCreateImage(g_App.vk.device, &resource->createInfo, nullptr, &resource->image);
    if (result != VK_SUCCESS) {
        testapp::Log("[FG-DIAG] vkCreateImage name=%s format=%d size=%ux%u usage=0x%x result=%s(%d)\n", name,
                     static_cast<int>(format), width, height, static_cast<unsigned>(usage), VkResultName(result),
                     static_cast<int>(result));
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_App.vk.device, resource->image, &requirements);
    const uint32_t memoryType = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryType == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo allocationInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocationInfo.allocationSize = requirements.size;
    allocationInfo.memoryTypeIndex = memoryType;
    result = vkAllocateMemory(g_App.vk.device, &allocationInfo, nullptr, &resource->memory);
    if (result != VK_SUCCESS) {
        testapp::Log("[FG-DIAG] vkAllocateMemory image=%s bytes=%llu result=%s(%d)\n", name,
                     static_cast<unsigned long long>(requirements.size), VkResultName(result),
                     static_cast<int>(result));
        return false;
    }
    result = vkBindImageMemory(g_App.vk.device, resource->image, resource->memory, 0);
    if (result != VK_SUCCESS) {
        return false;
    }
    VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = resource->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    result = vkCreateImageView(g_App.vk.device, &viewInfo, nullptr, &resource->view);
    if (result != VK_SUCCESS) {
        return false;
    }
    SetObjectName(VK_OBJECT_TYPE_IMAGE, NativeHandleToUint64(resource->image), name);
    SetObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, NativeHandleToUint64(resource->view), name);
    return true;
}
}
}

namespace testapp::vkfg {
namespace {
inline void DestroyImageResource(ImageResource* resource) {
    if (resource->view != VK_NULL_HANDLE) {
        vkDestroyImageView(g_App.vk.device, resource->view, nullptr);
    }
    if (resource->image != VK_NULL_HANDLE) {
        vkDestroyImage(g_App.vk.device, resource->image, nullptr);
    }
    if (resource->memory != VK_NULL_HANDLE) {
        vkFreeMemory(g_App.vk.device, resource->memory, nullptr);
    }
    *resource = {};
}
}
}

namespace testapp::vkfg {
namespace {
inline bool CheckFormat(VkFormat format, VkFormatFeatureFlags required, const char* role) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(g_App.vk.physicalDevice, format, &properties);
    const bool supported = (properties.optimalTilingFeatures & required) == required;
    testapp::Log("[FG-DIAG] Resource format role=%s format=%d required=0x%x optimal=0x%x supported=%d\n", role,
                 static_cast<int>(format), static_cast<unsigned>(required),
                 static_cast<unsigned>(properties.optimalTilingFeatures), supported ? 1 : 0);
    return supported;
}
}
}

namespace testapp::vkfg {
namespace {
inline VkRenderPass CreateSingleColorRenderPass(VkFormat format) {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;
    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    VkRenderPassCreateInfo createInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    createInfo.attachmentCount = 1;
    createInfo.pAttachments = &attachment;
    createInfo.subpassCount = 1;
    createInfo.pSubpasses = &subpass;
    createInfo.dependencyCount = 2;
    createInfo.pDependencies = dependencies;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    const VkResult result = vkCreateRenderPass(g_App.vk.device, &createInfo, nullptr, &renderPass);
    if (result != VK_SUCCESS) {
        testapp::Log("[FG-DIAG] vkCreateRenderPass format=%d result=%s(%d)\n", static_cast<int>(format),
                     VkResultName(result), static_cast<int>(result));
    }
    return renderPass;
}
}
}

namespace testapp::vkfg {
namespace {
inline VkRenderPass CreateSceneRenderPass() {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    std::array<VkAttachmentDescription, 5> attachments{};
    const VkFormat formats[] = {kSceneColorFormat, kMotionFormat, kMaskFormat, kMaskFormat, kDepthFormat};
    for (size_t index = 0; index < attachments.size(); ++index) {
        attachments[index].format = formats[index];
        attachments[index].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[index].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[index].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[index].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[index].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[index].initialLayout = index == 4 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                       : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[index].finalLayout = attachments[index].initialLayout;
    }
    const VkAttachmentReference colorReferences[] = {
        {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
    };
    const VkAttachmentReference depthReference{4, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<uint32_t>(std::size(colorReferences));
    subpass.pColorAttachments = colorReferences;
    subpass.pDepthStencilAttachment = &depthReference;
    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    VkRenderPassCreateInfo createInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    createInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    createInfo.pAttachments = attachments.data();
    createInfo.subpassCount = 1;
    createInfo.pSubpasses = &subpass;
    createInfo.dependencyCount = 2;
    createInfo.pDependencies = dependencies;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    const VkResult result = vkCreateRenderPass(g_App.vk.device, &createInfo, nullptr, &renderPass);
    if (result != VK_SUCCESS) {
        testapp::Log("[FG-DIAG] vkCreateRenderPass(scene) result=%s(%d)\n", VkResultName(result),
                     static_cast<int>(result));
    }
    return renderPass;
}
}
}

namespace testapp::vkfg {
namespace {
inline VkDescriptorSetLayout CreateSampledSetLayout(uint32_t bindingCount) {
    std::vector<VkDescriptorSetLayoutBinding> bindings(bindingCount);
    for (uint32_t binding = 0; binding < bindingCount; ++binding) {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo createInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    createInfo.bindingCount = bindingCount;
    createInfo.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    const VkResult result = vkCreateDescriptorSetLayout(g_App.vk.device, &createInfo, nullptr, &layout);
    return result == VK_SUCCESS ? layout : VK_NULL_HANDLE;
}
}
}

namespace testapp::vkfg {
namespace {
inline VkPipelineLayout CreatePipelineLayout(VkDescriptorSetLayout setLayout, uint32_t pushConstantSize,
                                      VkShaderStageFlags pushStages) {
    VkPushConstantRange range{};
    range.stageFlags = pushStages;
    range.size = pushConstantSize;
    VkPipelineLayoutCreateInfo createInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    createInfo.setLayoutCount = setLayout ? 1u : 0u;
    createInfo.pSetLayouts = setLayout ? &setLayout : nullptr;
    createInfo.pushConstantRangeCount = pushConstantSize ? 1u : 0u;
    createInfo.pPushConstantRanges = pushConstantSize ? &range : nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    return vkCreatePipelineLayout(g_App.vk.device, &createInfo, nullptr, &layout) == VK_SUCCESS ? layout
                                                                                                : VK_NULL_HANDLE;
}
}
}

namespace testapp::vkfg {
namespace {
inline VkShaderModule CreateShaderModule(const uint32_t* words, size_t byteSize) {
    VkShaderModuleCreateInfo createInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = byteSize;
    createInfo.pCode = words;
    VkShaderModule module = VK_NULL_HANDLE;
    return vkCreateShaderModule(g_App.vk.device, &createInfo, nullptr, &module) == VK_SUCCESS ? module
                                                                                              : VK_NULL_HANDLE;
}
}
}

namespace testapp::vkfg {
namespace {
inline VkPipeline CreateGraphicsPipeline(VkRenderPass renderPass, VkPipelineLayout layout, const uint32_t* fragmentCode,
                                  size_t fragmentSize, uint32_t colorAttachmentCount, bool depthEnabled) {
    VkShaderModule vertexModule =
        CreateShaderModule(shaders::kFullscreenVertexSpirv, sizeof(shaders::kFullscreenVertexSpirv));
    VkShaderModule fragmentModule = CreateShaderModule(fragmentCode, fragmentSize);
    if (!vertexModule || !fragmentModule) {
        return VK_NULL_HANDLE;
    }
    const VkPipelineShaderStageCreateInfo stages[] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
         vertexModule, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
         fragmentModule, "main", nullptr},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    VkPipelineMultisampleStateCreateInfo multisample = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = depthEnabled ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = depthEnabled ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(colorAttachmentCount);
    for (VkPipelineColorBlendAttachmentState& attachment : blendAttachments) {
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = colorAttachmentCount;
    blend.pAttachments = blendAttachments.data();
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<uint32_t>(std::size(dynamicStates));
    dynamic.pDynamicStates = dynamicStates;
    VkGraphicsPipelineCreateInfo createInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    createInfo.stageCount = static_cast<uint32_t>(std::size(stages));
    createInfo.pStages = stages;
    createInfo.pVertexInputState = &vertexInput;
    createInfo.pInputAssemblyState = &inputAssembly;
    createInfo.pViewportState = &viewportState;
    createInfo.pRasterizationState = &rasterization;
    createInfo.pMultisampleState = &multisample;
    createInfo.pDepthStencilState = &depthStencil;
    createInfo.pColorBlendState = &blend;
    createInfo.pDynamicState = &dynamic;
    createInfo.layout = layout;
    createInfo.renderPass = renderPass;
    createInfo.subpass = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result =
        vkCreateGraphicsPipelines(g_App.vk.device, VK_NULL_HANDLE, 1, &createInfo, nullptr, &pipeline);
    vkDestroyShaderModule(g_App.vk.device, fragmentModule, nullptr);
    vkDestroyShaderModule(g_App.vk.device, vertexModule, nullptr);
    if (result != VK_SUCCESS) {
        testapp::Log("[FG-DIAG] vkCreateGraphicsPipelines result=%s(%d) colors=%u depth=%d\n",
                     VkResultName(result), static_cast<int>(result), colorAttachmentCount, depthEnabled ? 1 : 0);
        return VK_NULL_HANDLE;
    }
    return pipeline;
}
}
}

namespace testapp::vkfg {
namespace {
inline bool CreateFrameResources(FrameResources* resources, uint32_t frameIndex) {
    const VkImageUsageFlags sampledColor = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    char names[10][64] = {};
    std::snprintf(names[0], sizeof(names[0]), "VKFG SceneColor[%u]", frameIndex);
    std::snprintf(names[1], sizeof(names[1]), "VKFG MotionVectors[%u]", frameIndex);
    std::snprintf(names[2], sizeof(names[2]), "VKFG Depth[%u]", frameIndex);
    std::snprintf(names[3], sizeof(names[3]), "VKFG ReactiveMask[%u]", frameIndex);
    std::snprintf(names[4], sizeof(names[4]), "VKFG TransparencyMask[%u]", frameIndex);
    std::snprintf(names[5], sizeof(names[5]), "VKFG HudlessColor[%u]", frameIndex);
    std::snprintf(names[6], sizeof(names[6]), "VKFG HistoryColor[%u]", frameIndex);
    std::snprintf(names[7], sizeof(names[7]), "VKFG UIColor[%u]", frameIndex);
    std::snprintf(names[8], sizeof(names[8]), "VKFG DegenerateUIColor1x1[%u]", frameIndex);
    std::snprintf(names[9], sizeof(names[9]), "VKFG PresentationColor[%u]", frameIndex);
    // Names are used only during creation/debug naming; do not retain pointers to this stack storage.
    bool created =
        CreateImageResource(&resources->sceneColor, g_App.renderer.renderWidth,
                            g_App.renderer.renderHeight, kSceneColorFormat, sampledColor,
                            VK_IMAGE_ASPECT_COLOR_BIT, names[0]) &&
        CreateImageResource(&resources->motionVectors, g_App.renderer.renderWidth,
                            g_App.renderer.renderHeight, kMotionFormat, sampledColor,
                            VK_IMAGE_ASPECT_COLOR_BIT, names[1]) &&
        CreateImageResource(&resources->depth, g_App.renderer.renderWidth, g_App.renderer.renderHeight,
                            kDepthFormat,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT, names[2]) &&
        CreateImageResource(&resources->reactiveMask, g_App.renderer.renderWidth,
                            g_App.renderer.renderHeight, kMaskFormat, sampledColor,
                            VK_IMAGE_ASPECT_COLOR_BIT, names[3]) &&
        CreateImageResource(&resources->transparencyMask, g_App.renderer.renderWidth,
                            g_App.renderer.renderHeight, kMaskFormat, sampledColor,
                            VK_IMAGE_ASPECT_COLOR_BIT, names[4]) &&
        CreateImageResource(&resources->hudlessColor, g_App.swapchain.extent.width,
                            g_App.swapchain.extent.height, kSceneColorFormat, sampledColor,
                            VK_IMAGE_ASPECT_COLOR_BIT, names[5]) &&
        CreateImageResource(&resources->historyColor, g_App.swapchain.extent.width,
                            g_App.swapchain.extent.height, kSceneColorFormat, sampledColor,
                            VK_IMAGE_ASPECT_COLOR_BIT, names[6]) &&
        CreateImageResource(&resources->uiColor, g_App.swapchain.extent.width,
                            g_App.swapchain.extent.height, kUiFormat, sampledColor,
                            VK_IMAGE_ASPECT_COLOR_BIT, names[7]) &&
        CreateImageResource(&resources->presentationColor, g_App.swapchain.extent.width,
                            g_App.swapchain.extent.height, g_App.swapchain.format, sampledColor,
                            VK_IMAGE_ASPECT_COLOR_BIT, names[9]);
    if (created && g_App.config.fsrDegenerateUiResource) {
        created = CreateImageResource(&resources->degenerateUiColor, 1, 1, kUiFormat,
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                      VK_IMAGE_ASPECT_COLOR_BIT, names[8]);
    }
    return created;
}
}
}

namespace testapp::vkfg {
namespace {
inline void DestroyFrameResources(FrameResources* resources) {
    if (resources->sceneFramebuffer) vkDestroyFramebuffer(g_App.vk.device, resources->sceneFramebuffer, nullptr);
    if (resources->hudlessFramebuffer)
        vkDestroyFramebuffer(g_App.vk.device, resources->hudlessFramebuffer, nullptr);
    if (resources->uiFramebuffer) vkDestroyFramebuffer(g_App.vk.device, resources->uiFramebuffer, nullptr);
    if (resources->presentationFramebuffer)
        vkDestroyFramebuffer(g_App.vk.device, resources->presentationFramebuffer, nullptr);
    DestroyImageResource(&resources->sceneColor);
    DestroyImageResource(&resources->motionVectors);
    DestroyImageResource(&resources->depth);
    DestroyImageResource(&resources->reactiveMask);
    DestroyImageResource(&resources->transparencyMask);
    DestroyImageResource(&resources->hudlessColor);
    DestroyImageResource(&resources->historyColor);
    DestroyImageResource(&resources->uiColor);
    DestroyImageResource(&resources->degenerateUiColor);
    DestroyImageResource(&resources->presentationColor);
    *resources = {};
}
}
}

namespace testapp::vkfg {
namespace {
inline VkFramebuffer CreateFramebuffer(VkRenderPass renderPass, const std::vector<VkImageView>& views, uint32_t width,
                                uint32_t height) {
    VkFramebufferCreateInfo createInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    createInfo.renderPass = renderPass;
    createInfo.attachmentCount = static_cast<uint32_t>(views.size());
    createInfo.pAttachments = views.data();
    createInfo.width = width;
    createInfo.height = height;
    createInfo.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    return vkCreateFramebuffer(g_App.vk.device, &createInfo, nullptr, &framebuffer) == VK_SUCCESS ? framebuffer
                                                                                                 : VK_NULL_HANDLE;
}
}
}

namespace testapp::vkfg {
namespace {
inline bool CreateFramebuffers(FrameResources* resources) {
    resources->sceneFramebuffer = CreateFramebuffer(
        g_App.renderer.sceneRenderPass,
        {resources->sceneColor.view, resources->motionVectors.view, resources->reactiveMask.view,
         resources->transparencyMask.view, resources->depth.view},
        g_App.renderer.renderWidth, g_App.renderer.renderHeight);
    resources->hudlessFramebuffer =
        CreateFramebuffer(g_App.renderer.hdrRenderPass, {resources->hudlessColor.view},
                          g_App.swapchain.extent.width, g_App.swapchain.extent.height);
    resources->uiFramebuffer = CreateFramebuffer(g_App.renderer.uiRenderPass, {resources->uiColor.view},
                                                 g_App.swapchain.extent.width, g_App.swapchain.extent.height);
    resources->presentationFramebuffer =
        CreateFramebuffer(g_App.renderer.swapchainRenderPass, {resources->presentationColor.view},
                          g_App.swapchain.extent.width, g_App.swapchain.extent.height);
    return resources->sceneFramebuffer && resources->hudlessFramebuffer && resources->uiFramebuffer &&
           resources->presentationFramebuffer;
}
}
}

namespace testapp::vkfg {
namespace {
inline bool AllocateAndWriteDescriptors() {
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight * 6};
    VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = kFramesInFlight * 3;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(g_App.vk.device, &poolInfo, nullptr, &g_App.renderer.descriptorPool) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkDescriptorSetLayout> layouts;
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        layouts.push_back(g_App.renderer.taaSetLayout);
        layouts.push_back(g_App.renderer.composeSetLayout);
        layouts.push_back(g_App.renderer.presentSetLayout);
    }
    std::vector<VkDescriptorSet> sets(layouts.size());
    VkDescriptorSetAllocateInfo allocateInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = g_App.renderer.descriptorPool;
    allocateInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocateInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(g_App.vk.device, &allocateInfo, sets.data()) != VK_SUCCESS) {
        return false;
    }
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        FrameResources& resources = g_App.renderer.resources[frame];
        resources.taaSet = sets[frame * 3 + 0];
        resources.composeSet = sets[frame * 3 + 1];
        resources.presentSet = sets[frame * 3 + 2];
        const VkDescriptorImageInfo taaInfos[] = {
            {g_App.renderer.linearSampler, resources.sceneColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {g_App.renderer.linearSampler, resources.motionVectors.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {g_App.renderer.linearSampler, resources.historyColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        const VkDescriptorImageInfo composeInfos[] = {
            {g_App.renderer.linearSampler, resources.presentationColor.view,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {g_App.renderer.linearSampler, resources.uiColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        const VkDescriptorImageInfo presentInfo = {
            g_App.renderer.linearSampler, resources.hudlessColor.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        std::vector<VkWriteDescriptorSet> writes;
        for (uint32_t binding = 0; binding < 3; ++binding) {
            VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = resources.taaSet;
            write.dstBinding = binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &taaInfos[binding];
            writes.push_back(write);
        }
        for (uint32_t binding = 0; binding < 2; ++binding) {
            VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = resources.composeSet;
            write.dstBinding = binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &composeInfos[binding];
            writes.push_back(write);
        }
        VkWriteDescriptorSet presentWrite = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        presentWrite.dstSet = resources.presentSet;
        presentWrite.dstBinding = 0;
        presentWrite.descriptorCount = 1;
        presentWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        presentWrite.pImageInfo = &presentInfo;
        writes.push_back(presentWrite);
        vkUpdateDescriptorSets(g_App.vk.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    return true;
}
}
}

namespace testapp::vkfg {
namespace {
inline bool CreateFrameContexts() {
    for (uint32_t index = 0; index < kFramesInFlight; ++index) {
        FrameContext& frame = g_App.frames[index];
        VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                         VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = g_App.vk.queuePlan.game.familyIndex;
        if (vkCreateCommandPool(g_App.vk.device, &poolInfo, nullptr, &frame.commandPool) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferAllocateInfo allocateInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocateInfo.commandPool = frame.commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(g_App.vk.device, &allocateInfo, &frame.commandBuffer) != VK_SUCCESS) {
            return false;
        }
        VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(g_App.vk.device, &semaphoreInfo, nullptr, &frame.imageAvailable) != VK_SUCCESS) {
            return false;
        }
        VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(g_App.vk.device, &fenceInfo, nullptr, &frame.fence) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}
}
}

namespace testapp::vkfg {
namespace {
inline void DestroyFrameContexts() {
    for (FrameContext& frame : g_App.frames) {
        if (frame.fence) vkDestroyFence(g_App.vk.device, frame.fence, nullptr);
        if (frame.imageAvailable) vkDestroySemaphore(g_App.vk.device, frame.imageAvailable, nullptr);
        if (frame.commandPool) vkDestroyCommandPool(g_App.vk.device, frame.commandPool, nullptr);
        frame = {};
    }
}
}
}

namespace testapp::vkfg {
namespace {
struct LayoutAccess {
    VkPipelineStageFlags stage;
    VkAccessFlags access;
};
}
}

namespace testapp::vkfg {
namespace {
struct CpuTimingAccumulator {
    double reflexStartMs = 0.0;
    double frameFenceMs = 0.0;
    double acquireMs = 0.0;
    double imageFenceMs = 0.0;
    double recordSubmitMs = 0.0;
    double presentMs = 0.0;
    double streamlinePollMs = 0.0;
    uint64_t samples = 0;
};
}
}

namespace testapp::vkfg {
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
extern AppState g_App;
}
