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

void LoadConfig();
void NormalizeAutoSequenceTimings();
void ParseCommandLine(int argc, char* argv[]);
std::string TestAppConfigPath();
void ParseVulkanOptions(int argc, char* argv[]);

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
bool CreateSwapchainFramebuffers(SwapchainState* state);
}

namespace testapp::vkfg {
uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required);
}

namespace testapp::vkfg {
void SetObjectName(VkObjectType type, uint64_t handle, const char* name);
}

namespace testapp::vkfg {
bool CreateImageResource(ImageResource* resource, uint32_t width, uint32_t height, VkFormat format,
                         VkImageUsageFlags usage, VkImageAspectFlags aspect, const char* name);
}

namespace testapp::vkfg {
void DestroyImageResource(ImageResource* resource);
}

namespace testapp::vkfg {
bool CheckFormat(VkFormat format, VkFormatFeatureFlags required, const char* role);
}

namespace testapp::vkfg {
VkRenderPass CreateSingleColorRenderPass(VkFormat format);
}

namespace testapp::vkfg {
VkRenderPass CreateSceneRenderPass();
}

namespace testapp::vkfg {
VkDescriptorSetLayout CreateSampledSetLayout(uint32_t bindingCount);
}

namespace testapp::vkfg {
VkPipelineLayout CreatePipelineLayout(VkDescriptorSetLayout setLayout, uint32_t pushConstantSize,
                                      VkShaderStageFlags pushStages);
}

namespace testapp::vkfg {
VkShaderModule CreateShaderModule(const uint32_t* words, size_t byteSize);
}

namespace testapp::vkfg {
VkPipeline CreateGraphicsPipeline(VkRenderPass renderPass, VkPipelineLayout layout, const uint32_t* fragmentCode,
                                  size_t fragmentSize, uint32_t colorAttachmentCount, bool depthEnabled);
}

namespace testapp::vkfg {
bool CreateFrameResources(FrameResources* resources, uint32_t frameIndex);
}

namespace testapp::vkfg {
void DestroyFrameResources(FrameResources* resources);
}

namespace testapp::vkfg {
VkFramebuffer CreateFramebuffer(VkRenderPass renderPass, const std::vector<VkImageView>& views, uint32_t width,
                                uint32_t height);
}

namespace testapp::vkfg {
bool CreateFramebuffers(FrameResources* resources);
}

namespace testapp::vkfg {
bool AllocateAndWriteDescriptors();
}

namespace testapp::vkfg {
bool CreateFrameContexts();
}

namespace testapp::vkfg {
void DestroyFrameContexts();
}

namespace testapp::vkfg {
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
extern AppState g_App;
}
