#pragma once

#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define VK_USE_PLATFORM_WIN32_KHR

#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_helpers_vk.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "fg_switch_config.h"
#include "testapp_common.h"
#include "vulkan_fg_policy.h"

namespace testapp::vkfg {

using testapp::fg::JitterOffset;
using testapp::fg::UpscaleQuality;
using testapp::fg::ComputeJitter;
using testapp::fg::JitterPhaseCount;

template <typename T>
inline void* NativeHandleToVoid(T handle) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<void*>(handle);
    } else {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
    }
}

template <typename T>
inline uint64_t NativeHandleToUint64(T handle) {
    if constexpr (std::is_pointer_v<T>) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    } else {
        return static_cast<uint64_t>(handle);
    }
}

constexpr uint32_t kFramesInFlight = 3;
constexpr uint32_t kRequestedSwapchainImages = 3;
constexpr VkFormat kSceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kMaskFormat = VK_FORMAT_R8_UNORM;
constexpr VkFormat kUiFormat = VK_FORMAT_R8G8B8A8_UNORM;

struct ImageResource {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageCreateInfo createInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

struct FrameResources {
    ImageResource sceneColor;
    ImageResource motionVectors;
    ImageResource depth;
    ImageResource reactiveMask;
    ImageResource transparencyMask;
    ImageResource hudlessColor;
    ImageResource historyColor;
    ImageResource uiColor;
    ImageResource degenerateUiColor;
    ImageResource presentationColor;
    VkFramebuffer sceneFramebuffer = VK_NULL_HANDLE;
    VkFramebuffer hudlessFramebuffer = VK_NULL_HANDLE;
    VkFramebuffer uiFramebuffer = VK_NULL_HANDLE;
    VkFramebuffer presentationFramebuffer = VK_NULL_HANDLE;
    VkDescriptorSet taaSet = VK_NULL_HANDLE;
    VkDescriptorSet composeSet = VK_NULL_HANDLE;
    VkDescriptorSet presentSet = VK_NULL_HANDLE;
    bool historyValid = false;
};

struct FrameContext {
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};

// Every live swapchain owns an immutable copy of the WSI table which created it. The custom FFX
// create/destroy signatures are kept separate so no proxy handle can accidentally cross routes.
struct VulkanWsiDispatch {
    VulkanWsiRoute route = VulkanWsiRoute::Loader;
    PFN_vkCreateSwapchainKHR createSwapchain = nullptr;
    PFN_vkDestroySwapchainKHR destroySwapchain = nullptr;
    PFN_vkCreateSwapchainFFXAPI createSwapchainFfx = nullptr;
    PFN_vkDestroySwapchainFFXAPI destroySwapchainFfx = nullptr;
    PFN_vkGetSwapchainImagesKHR getSwapchainImages = nullptr;
    PFN_vkAcquireNextImageKHR acquireNextImage = nullptr;
    PFN_vkQueuePresentKHR queuePresent = nullptr;
    PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;
    PFN_getLastPresentCountFFXAPI getLastPresentCountFfx = nullptr;
    void* context = nullptr;
};

struct SwapchainState {
    SwapchainOwner owner = SwapchainOwner::Native;
    VulkanWsiDispatch wsi;
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkSwapchainCreateInfoKHR createInfo = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent = {};
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkImageLayout> layouts;
    std::vector<VkFence> imageFences;
    std::vector<VkSemaphore> presentReadySemaphores;
};

struct RendererState {
    VkRenderPass sceneRenderPass = VK_NULL_HANDLE;
    VkRenderPass hdrRenderPass = VK_NULL_HANDLE;
    VkRenderPass uiRenderPass = VK_NULL_HANDLE;
    VkRenderPass swapchainRenderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout taaSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout composeSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout presentSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout scenePipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout taaPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout uiPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout composePipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout presentPipelineLayout = VK_NULL_HANDLE;
    VkPipeline scenePipeline = VK_NULL_HANDLE;
    VkPipeline taaPipeline = VK_NULL_HANDLE;
    VkPipeline uiPipeline = VK_NULL_HANDLE;
    VkPipeline composePipeline = VK_NULL_HANDLE;
    VkPipeline presentPipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkSampler linearSampler = VK_NULL_HANDLE;
    std::array<FrameResources, kFramesInFlight> resources;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    bool initialized = false;
};

struct SlFeatureRequirementCopy {
    sl::Feature feature = {};
    sl::FeatureRequirementFlags flags = {};
    uint32_t graphicsQueues = 0;
    uint32_t computeQueues = 0;
    uint32_t opticalFlowQueues = 0;
    std::vector<std::string> instanceExtensions;
    std::vector<std::string> deviceExtensions;
    std::vector<std::string> features12;
    std::vector<std::string> features13;
    bool instanceExtensionsAvailable = true;
    bool deviceExtensionsAvailable = true;
    bool deviceFeaturesAvailable = true;
};

struct StreamlineState {
    HMODULE module = nullptr;
    PFun_slInit* init = nullptr;
    PFun_slShutdown* shutdown = nullptr;
    PFun_slGetFeatureRequirements* getFeatureRequirements = nullptr;
    PFun_slIsFeatureSupported* isFeatureSupported = nullptr;
    PFun_slSetVulkanInfo* setVulkanInfo = nullptr;
    PFun_slSetFeatureLoaded* setFeatureLoaded = nullptr;
    PFun_slGetFeatureFunction* getFeatureFunction = nullptr;
    PFun_slGetNewFrameToken* getNewFrameToken = nullptr;
    PFun_slSetConstants* setConstants = nullptr;
    PFun_slSetTagForFrame* setTagForFrame = nullptr;
    PFun_slEvaluateFeature* evaluateFeature = nullptr;
    PFun_slDLSSSetOptions* dlssSetOptions = nullptr;
    PFun_slDLSSGetOptimalSettings* dlssGetOptimalSettings = nullptr;
    PFun_slDLSSGSetOptions* dlssgSetOptions = nullptr;
    PFun_slDLSSGGetState* dlssgGetState = nullptr;
    PFun_slReflexGetState* reflexGetState = nullptr;
    PFun_slReflexSetOptions* reflexSetOptions = nullptr;
    PFun_slReflexSleep* reflexSleep = nullptr;
    PFun_slPCLSetMarker* pclSetMarker = nullptr;
    PFN_vkGetInstanceProcAddr proxyGetInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr proxyGetDeviceProcAddr = nullptr;
    PFN_vkCreateWin32SurfaceKHR proxyCreateWin32Surface = nullptr;
    PFN_vkDestroySurfaceKHR proxyDestroySurface = nullptr;
    VulkanWsiDispatch wsi;
    std::vector<SlFeatureRequirementCopy> requirements;
    sl::ViewportHandle viewport = sl::ViewportHandle(1);
    bool initialized = false;
    bool vulkanInfoSet = false;
    bool featuresLoaded = false;
    bool dlssSrSupported = false;
    bool dlssFgSupported = false;
    bool reflexSupported = false;
    bool dlssSrConfigured = false;
    bool dlssFgConfigured = false;
    bool reflexActive = false;
    bool reflexOptionsConfigured = false;
    bool reflexStateAvailable = false;
    bool dlssgVsyncSupportKnown = false;
    bool dlssgVsyncSupported = false;
    uint32_t frameTokenIndex = 0;
    uint64_t reflexSleepCalls = 0;
    uint64_t reflexSleepFailures = 0;
    uint64_t generatedPresentCount = 0;
};

struct FidelityFxState {
    HMODULE module = nullptr;
    HMODULE preloadedModule = nullptr;
    ffxFunctions functions = {};
    ffxContext swapchainContext = nullptr;
    ffxContext upscaleContext = nullptr;
    ffxContext frameGenerationContext = nullptr;
    ffxQueryDescSwapchainReplacementFunctionsVK replacement = {};
    ffxCreateBackendVKDesc upscaleBackend = {};
    ffxCreateContextDescUpscale upscaleCreate = {};
    ffxCreateBackendVKDesc frameGenerationBackend = {};
    ffxCreateContextDescFrameGenerationHudless frameGenerationHudless = {};
    ffxCreateContextDescFrameGeneration frameGenerationCreate = {};
    ffxCreateContextDescFrameGenerationSwapChainModeVK swapchainMode = {};
    ffxCreateContextDescFrameGenerationSwapChainVK swapchainCreate = {};
    VkSwapchainKHR swapchainHandleStorage = VK_NULL_HANDLE;
    bool runtimeLoaded = false;
    bool swapchainReady = false;
    bool upscaleSupported = false;
    bool frameGenerationSupported = false;
    bool frameGenerationConfigured = false;
    bool lastPresentCallback = true;
    std::atomic<uint64_t> presentCallbackCount{0};
    std::atomic<uint64_t> frameGenerationCallbackCount{0};
    std::atomic<bool> preloadStarted{false};
    std::atomic<bool> preloadInProgress{false};
    std::atomic<bool> preloadSucceeded{false};
    std::thread preloadThread;
    std::mutex preloadMutex;
    uint64_t generatedPresentCount = 0;
    uint64_t lastSwapchainPresentCount = 0;
    std::chrono::steady_clock::time_point callbackStressStart = std::chrono::steady_clock::now();
};

struct VulkanDeviceState {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkQueue gameQueue = VK_NULL_HANDLE;
    VkQueue asyncPresentQueue = VK_NULL_HANDLE;
    VkQueue ffxAsyncQueue = VK_NULL_HANDLE;
    VkQueue ffxPresentQueue = VK_NULL_HANDLE;
    VkQueue ffxAcquireQueue = VK_NULL_HANDLE;
    VulkanQueuePlan queuePlan;
    std::vector<VkQueue> streamlineGraphicsQueues;
    std::vector<VkQueue> streamlineComputeQueues;
    std::vector<VkQueue> streamlineOpticalFlowQueues;
    VkPhysicalDeviceProperties properties = {};
    VkPhysicalDeviceMemoryProperties memoryProperties = {};
    bool validationEnabled = false;
    bool memoryBudgetEnabled = false;
    bool deviceFaultEnabled = false;
    bool surfaceCreatedByStreamline = false;
    bool asyncPresentActive = false;
    bool deviceLost = false;
};

struct AppState {
    HWND hwnd = nullptr;
    HINSTANCE instanceHandle = nullptr;
    testapp::fg::FgSwitchConfig config;
    VulkanDeviceState vk;
    StreamlineState sl;
    FidelityFxState ffx;
    SwapchainState swapchain;
    RendererState renderer;
    std::array<FrameContext, kFramesInFlight> frames;
    FgTransitionState transition;
    FgMode requestedMode = FgMode::Off;
    bool running = true;
    bool asyncPresentRequested = false;
    bool resizePending = false;
    bool fullscreenPending = false;
    bool swapchainRecreatePending = false;
    uint32_t pendingWidth = 0;
    uint32_t pendingHeight = 0;
    uint32_t frameSlot = 0;
    uint64_t frameId = 0;
    uint64_t presentedFrames = 0;
    uint64_t generatedFrames = 0;
    uint64_t transitionFailures = 0;
    uint64_t validationErrors = 0;
    uint64_t pacingSpikes = 0;
    bool resetTemporalHistory = true;
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point previousFrameTime = startTime;
    std::chrono::steady_clock::time_point fpsSampleTime = startTime;
    std::chrono::steady_clock::time_point heartbeatTime = startTime;
    uint64_t fpsSampleFrame = 0;
    uint64_t heartbeatFrame = 0;
    float previousTimeSeconds = 0.0f;
    float frameDeltaMs = 16.7f;
    float fps = 60.0f;
};

extern AppState g_App;

const char* VkResultName(VkResult result);
const char* SlResultName(sl::Result result);
const char* FfxResultName(ffxReturnCode_t result);
const char* ModeName(FgMode mode);
const char* TransitionStageName(TransitionStage stage);
void LogTransition(const char* event, bool flush = true);
void LogDeviceFault(const char* reason);

bool InitializeStreamlineBeforeVulkan();
bool SetStreamlineFeaturesLoaded(bool loaded, const char* reason);
bool ConfigureStreamlineAfterDevice();
bool PrepareStreamlineMode();
bool RetireStreamlinePresentation(SwapchainOwner nextOwner, const char* reason);
void ShutdownStreamline();
sl::FrameToken* BeginStreamlineFrame();
void SetStreamlineMarker(sl::FrameToken* token, sl::PCLMarker marker, const char* name);
bool SetDlssFrameGeneration(bool enabled, const char* reason);
bool ConfigureDlssSuperResolution(bool enabled);
bool RecordStreamlineInputsAndUpscale(VkCommandBuffer commandBuffer, FrameResources& resources,
                                     sl::FrameToken* frameToken, const JitterOffset& jitter,
                                     uint32_t backbufferIndex);
void PollStreamlineState();

bool InitializeVulkanDevice();
VkQueue ApplicationPresentQueue();
const VulkanQueueRef& ApplicationPresentQueueRef();
void ReleaseVulkanSurfaceBeforeStreamlineShutdown();
void ShutdownVulkanDevice();
bool CreateOrReplaceSwapchain(SwapchainOwner owner, const char* reason);
bool RecreateCurrentSwapchain(const char* reason);
void DestroySwapchainState(bool destroyHandle);
bool DrainSwapchainBoundWork(const char* reason);
VkResult AcquireSwapchainImage(FrameContext& frame, uint32_t* imageIndex);
VkResult PresentSwapchainImage(uint32_t imageIndex);
void QueryMemoryBudgetStress();

bool InitializeRenderer();
void ShutdownRenderer();
bool RecreateRendererForExtent();
bool RenderFrame();

bool LoadFidelityFxRuntime(const char* reason);
void StartFidelityFxRuntimePreload(const char* reason);
void JoinFidelityFxRuntimePreload(const char* reason);
bool PrepareFidelityFxMode();
bool CreateFidelityFxSwapchain(VkSwapchainKHR oldSwapchain, const VkSwapchainCreateInfoKHR& createInfo,
                               VkSwapchainKHR* replacement, bool* oldSwapchainConsumed);
void DestroyFidelityFxContexts(bool unloadRuntime, const char* reason,
                               bool presentationAlreadyRetired = false);
bool SetFsrFrameGeneration(bool enabled, const char* reason, bool forceLog = true);
bool RecordFsrUpscaleAndPrepare(VkCommandBuffer commandBuffer, FrameResources& resources,
                              const JitterOffset& jitter);
void ReleaseFidelityFxEffectsForExtent(const char* reason);
bool RecreateFidelityFxEffectsForExtent();
void RegisterFsrUiResource(FrameResources& resources);
bool WaitForFsrPresents(const char* reason);

bool RequestMode(FgMode mode, const char* reason);
void DriveTransitionBeforeFrame();
void OnFramePresentedSuccessfully();
bool SetModeFeatureState(FgMode mode, bool enabled, const char* reason);

}  // namespace testapp::vkfg
