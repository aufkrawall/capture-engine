#include "vulkan_fg_switch_test_internal.h"

namespace testapp::vkfg {
namespace {
std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring directory = path;
    const size_t slash = directory.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : directory.substr(0, slash);
}
}
}

namespace testapp::vkfg {
namespace {
void SlLogCallback(sl::LogType type, const char* message) {
    testapp::Log("[SL-LOG] type=%u %s\n", static_cast<unsigned>(type), message ? message : "");
}
}
}

namespace testapp::vkfg {
namespace {
bool ResolveStreamlineFeatureFunctions() {
    g_App.sl.dlssSetOptions = SlFeatureFunction<PFun_slDLSSSetOptions>(sl::kFeatureDLSS, "slDLSSSetOptions");
    g_App.sl.dlssGetOptimalSettings =
        SlFeatureFunction<PFun_slDLSSGetOptimalSettings>(sl::kFeatureDLSS, "slDLSSGetOptimalSettings");
    g_App.sl.dlssgSetOptions =
        SlFeatureFunction<PFun_slDLSSGSetOptions>(sl::kFeatureDLSS_G, "slDLSSGSetOptions");
    g_App.sl.dlssgGetState =
        SlFeatureFunction<PFun_slDLSSGGetState>(sl::kFeatureDLSS_G, "slDLSSGGetState");
    g_App.sl.reflexGetState =
        SlFeatureFunction<PFun_slReflexGetState>(sl::kFeatureReflex, "slReflexGetState");
    g_App.sl.reflexSetOptions =
        SlFeatureFunction<PFun_slReflexSetOptions>(sl::kFeatureReflex, "slReflexSetOptions");
    g_App.sl.reflexSleep = SlFeatureFunction<PFun_slReflexSleep>(sl::kFeatureReflex, "slReflexSleep");
    g_App.sl.pclSetMarker = SlFeatureFunction<PFun_slPCLSetMarker>(sl::kFeaturePCL, "slPCLSetMarker");
    return g_App.sl.dlssgSetOptions && g_App.sl.dlssgGetState;
}
}
}

namespace testapp::vkfg {
namespace {
const SlFeatureRequirementCopy* FindRequirement(sl::Feature feature) {
    for (const SlFeatureRequirementCopy& requirement : g_App.sl.requirements) {
        if (requirement.feature == feature) {
            return &requirement;
        }
    }
    return nullptr;
}
}
}

namespace testapp::vkfg {
namespace {
bool FeatureRequirementsAvailable(sl::Feature feature) {
    const SlFeatureRequirementCopy* requirement = FindRequirement(feature);
    return requirement && requirement->instanceExtensionsAvailable && requirement->deviceExtensionsAvailable &&
           requirement->deviceFeaturesAvailable &&
           (requirement->flags & sl::FeatureRequirementFlags::eVulkanSupported);
}
}
}

namespace testapp::vkfg {
namespace {
sl::DLSSMode DlssQualityMode() {
    if (g_App.config.upscaleScalePercent > 0) {
        const float ratio = 100.0f / static_cast<float>(g_App.config.upscaleScalePercent);
        if (ratio < 1.05f) return sl::DLSSMode::eDLAA;
        if (ratio < 1.60f) return sl::DLSSMode::eMaxQuality;
        if (ratio < 1.85f) return sl::DLSSMode::eBalanced;
        if (ratio < 2.50f) return sl::DLSSMode::eMaxPerformance;
        return sl::DLSSMode::eUltraPerformance;
    }
    switch (g_App.config.upscaleQuality) {
        case UpscaleQuality::Quality:
            return sl::DLSSMode::eMaxQuality;
        case UpscaleQuality::Balanced:
            return sl::DLSSMode::eBalanced;
        case UpscaleQuality::Performance:
            return sl::DLSSMode::eMaxPerformance;
        case UpscaleQuality::UltraPerformance:
            return sl::DLSSMode::eUltraPerformance;
        case UpscaleQuality::NativeAA:
        default:
            return sl::DLSSMode::eDLAA;
    }
}
}
}

namespace testapp::vkfg {
namespace {
sl::DLSSPreset DlssPreset() {
    switch (g_App.config.dlssPreset) {
        case 'j':
            return sl::DLSSPreset::ePresetJ;
        case 'k':
            return sl::DLSSPreset::ePresetK;
        case 'l':
            return sl::DLSSPreset::ePresetL;
        case 'm':
            return sl::DLSSPreset::ePresetM;
        default:
            return sl::DLSSPreset::eDefault;
    }
}
}
}

namespace testapp::vkfg {
namespace {
sl::float4x4 IdentitySlMatrix() {
    sl::float4x4 matrix{};
    matrix[0] = sl::float4(1.0f, 0.0f, 0.0f, 0.0f);
    matrix[1] = sl::float4(0.0f, 1.0f, 0.0f, 0.0f);
    matrix[2] = sl::float4(0.0f, 0.0f, 1.0f, 0.0f);
    matrix[3] = sl::float4(0.0f, 0.0f, 0.0f, 1.0f);
    return matrix;
}
}
}

namespace testapp::vkfg {
namespace {
sl::float4x4 MakeSlMatrix(const std::array<float, 16>& values) {
    sl::float4x4 matrix{};
    for (uint32_t row = 0; row < 4; ++row) {
        matrix[row] = sl::float4(values[row * 4 + 0], values[row * 4 + 1],
                                 values[row * 4 + 2], values[row * 4 + 3]);
    }
    return matrix;
}
}
}

namespace testapp::vkfg {
namespace {
sl::Resource MakeSlResource(const ImageResource& image) {
    sl::Resource resource(sl::ResourceType::eTex2d, NativeHandleToVoid(image.image),
                          NativeHandleToVoid(image.memory), NativeHandleToVoid(image.view),
                          static_cast<uint32_t>(image.layout));
    resource.width = image.createInfo.extent.width;
    resource.height = image.createInfo.extent.height;
    resource.nativeFormat = static_cast<uint32_t>(image.createInfo.format);
    resource.mipLevels = image.createInfo.mipLevels;
    resource.arrayLayers = image.createInfo.arrayLayers;
    resource.flags = image.createInfo.flags;
    resource.usage = image.createInfo.usage;
    return resource;
}
}
}

namespace testapp::vkfg {
namespace {
bool SetReflexMode(bool enabled, const char* reason) {
    if (!g_App.sl.reflexSetOptions || !g_App.sl.reflexSupported) {
        return !enabled;
    }
    if (g_App.sl.reflexOptionsConfigured && g_App.sl.reflexActive == enabled) {
        return true;
    }
    sl::ReflexOptions options{};
    options.mode = enabled ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
    const sl::Result result = g_App.sl.reflexSetOptions(options);
    testapp::Log(
        "[FG-DIAG] slReflexSetOptions requested=%s frameLimitUs=%u "
        "automaticDriverPacing=unmodified reason=%s result=%d(%s)\n",
        enabled ? "low-latency" : "off", options.frameLimitUs,
        reason ? reason : "unknown", static_cast<int>(result), SlResultName(result));
    if (result == sl::Result::eOk) {
        g_App.sl.reflexActive = enabled;
        g_App.sl.reflexOptionsConfigured = true;
    }
    testapp::LogFlush();
    return result == sl::Result::eOk;
}
}
}

namespace testapp::vkfg {
namespace {
void PollReflexState(bool force) {
    if (!g_App.sl.reflexGetState || !g_App.sl.reflexSupported ||
        (!force && (g_App.frameId % 120) != 0)) {
        return;
    }
    sl::ReflexState state{};
    const sl::Result result = g_App.sl.reflexGetState(state);
    g_App.sl.reflexStateAvailable = result == sl::Result::eOk && state.lowLatencyAvailable;
    testapp::Log(
        "[FG-DIAG] slReflexGetState result=%d(%s) lowLatencyAvailable=%d "
        "latencyReportAvailable=%d configuredMode=%s frameLimitUs=0\n",
        static_cast<int>(result), SlResultName(result), state.lowLatencyAvailable ? 1 : 0,
        state.latencyReportAvailable ? 1 : 0,
        g_App.sl.reflexActive ? "low-latency" : "off");
}
}
}

namespace testapp::vkfg {
bool InitializeStreamlineBeforeVulkan() {
    const std::wstring modulePath = ExecutableDirectory() + L"\\sl.interposer.dll";
    g_App.sl.module = LoadLibraryW(modulePath.c_str());
    if (!g_App.sl.module) {
        testapp::Log("[FG-DIAG] Streamline unavailable: LoadLibrary('%S') failed error=%lu\n", modulePath.c_str(),
                     GetLastError());
        return false;
    }
    g_App.sl.init = SlExport<PFun_slInit>("slInit");
    g_App.sl.shutdown = SlExport<PFun_slShutdown>("slShutdown");
    g_App.sl.getFeatureRequirements = SlExport<PFun_slGetFeatureRequirements>("slGetFeatureRequirements");
    g_App.sl.isFeatureSupported = SlExport<PFun_slIsFeatureSupported>("slIsFeatureSupported");
    g_App.sl.setVulkanInfo = SlExport<PFun_slSetVulkanInfo>("slSetVulkanInfo");
    g_App.sl.setFeatureLoaded = SlExport<PFun_slSetFeatureLoaded>("slSetFeatureLoaded");
    g_App.sl.getFeatureFunction = SlExport<PFun_slGetFeatureFunction>("slGetFeatureFunction");
    g_App.sl.getNewFrameToken = SlExport<PFun_slGetNewFrameToken>("slGetNewFrameToken");
    g_App.sl.setConstants = SlExport<PFun_slSetConstants>("slSetConstants");
    g_App.sl.setTagForFrame = SlExport<PFun_slSetTagForFrame>("slSetTagForFrame");
    g_App.sl.evaluateFeature = SlExport<PFun_slEvaluateFeature>("slEvaluateFeature");
    g_App.sl.proxyGetInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(g_App.sl.module, "vkGetInstanceProcAddr"));
    g_App.sl.proxyGetDeviceProcAddr =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(g_App.sl.module, "vkGetDeviceProcAddr"));
    if (!g_App.sl.init || !g_App.sl.shutdown || !g_App.sl.getFeatureRequirements ||
        !g_App.sl.isFeatureSupported || !g_App.sl.setVulkanInfo || !g_App.sl.setFeatureLoaded ||
        !g_App.sl.getFeatureFunction || !g_App.sl.getNewFrameToken || !g_App.sl.setConstants ||
        !g_App.sl.setTagForFrame || !g_App.sl.evaluateFeature || !g_App.sl.proxyGetInstanceProcAddr ||
        !g_App.sl.proxyGetDeviceProcAddr) {
        testapp::Log("[FG-DIAG] Streamline interposer missing mandatory Vulkan/manual-hook exports\n");
        FreeLibrary(g_App.sl.module);
        g_App.sl.module = nullptr;
        return false;
    }

    std::wstring pluginPath = ExecutableDirectory();
    const wchar_t* pluginPaths[] = {pluginPath.c_str()};
    const sl::Feature features[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences preferences{};
    preferences.logLevel = sl::LogLevel::eVerbose;
    preferences.pathsToPlugins = pluginPaths;
    preferences.numPathsToPlugins = static_cast<uint32_t>(std::size(pluginPaths));
    preferences.pathToLogsAndData = pluginPath.c_str();
    preferences.logMessageCallback = SlLogCallback;
    preferences.flags = sl::PreferenceFlags::eUseManualHooking |
                        sl::PreferenceFlags::eUseFrameBasedResourceTagging |
                        sl::PreferenceFlags::eDisableCLStateTracking;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "CaptureProject Vulkan FG switch test";
    preferences.projectId = "7f1d0f20-2f9a-4f2d-9c64-5d1220e9d013";
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    const sl::Result initResult = g_App.sl.init(preferences, sl::kSDKVersion);
    testapp::Log("[FG-DIAG] slInit(before Vulkan) result=%d(%s) sdk=0x%llx module=%p pluginPath='%S'\n",
                 static_cast<int>(initResult), SlResultName(initResult),
                 static_cast<unsigned long long>(sl::kSDKVersion), g_App.sl.module, pluginPath.c_str());
    if (initResult != sl::Result::eOk) {
        g_App.sl.shutdown();
        FreeLibrary(g_App.sl.module);
        g_App.sl.module = nullptr;
        return false;
    }
    g_App.sl.initialized = true;
    g_App.sl.featuresLoaded = true;

    const sl::Feature queriedFeatures[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex};
    for (sl::Feature feature : queriedFeatures) {
        sl::FeatureRequirements requirements{};
        const sl::Result requirementResult = g_App.sl.getFeatureRequirements(feature, requirements);
        testapp::Log(
            "[FG-DIAG] slGetFeatureRequirements feature=%u result=%d(%s) flags=0x%x "
            "vulkan=%d vsyncOffRequired=%d hardwareSchedulingRequired=%d "
            "queues(g=%u,c=%u,of=%u) extensions(instance=%u,device=%u) "
            "features(1.2=%u,1.3=%u)\n",
            static_cast<unsigned>(feature), static_cast<int>(requirementResult), SlResultName(requirementResult),
            static_cast<unsigned>(requirements.flags),
            (requirements.flags & sl::FeatureRequirementFlags::eVulkanSupported) ? 1 : 0,
            (requirements.flags & sl::FeatureRequirementFlags::eVSyncOffRequired) ? 1 : 0,
            (requirements.flags & sl::FeatureRequirementFlags::eHardwareSchedulingRequired) ? 1 : 0,
            requirements.vkNumGraphicsQueuesRequired,
            requirements.vkNumComputeQueuesRequired, requirements.vkNumOpticalFlowQueuesRequired,
            requirements.vkNumInstanceExtensions, requirements.vkNumDeviceExtensions, requirements.vkNumFeatures12,
            requirements.vkNumFeatures13);
        if (requirementResult != sl::Result::eOk) {
            continue;
        }
        SlFeatureRequirementCopy copy{};
        copy.feature = feature;
        copy.flags = requirements.flags;
        copy.graphicsQueues = requirements.vkNumGraphicsQueuesRequired;
        copy.computeQueues = requirements.vkNumComputeQueuesRequired;
        copy.opticalFlowQueues = requirements.vkNumOpticalFlowQueuesRequired;
        for (uint32_t index = 0; index < requirements.vkNumInstanceExtensions; ++index) {
            copy.instanceExtensions.emplace_back(requirements.vkInstanceExtensions[index]);
        }
        for (uint32_t index = 0; index < requirements.vkNumDeviceExtensions; ++index) {
            copy.deviceExtensions.emplace_back(requirements.vkDeviceExtensions[index]);
        }
        for (uint32_t index = 0; index < requirements.vkNumFeatures12; ++index) {
            copy.features12.emplace_back(requirements.vkFeatures12[index]);
            testapp::Log("[FG-DIAG] Streamline feature=%u requires Vulkan1.2 feature '%s'\n",
                         static_cast<unsigned>(feature), requirements.vkFeatures12[index]);
        }
        for (uint32_t index = 0; index < requirements.vkNumFeatures13; ++index) {
            copy.features13.emplace_back(requirements.vkFeatures13[index]);
            testapp::Log("[FG-DIAG] Streamline feature=%u requires Vulkan1.3 feature '%s'\n",
                         static_cast<unsigned>(feature), requirements.vkFeatures13[index]);
        }
        g_App.sl.requirements.push_back(std::move(copy));
    }
    testapp::LogFlush();
    return true;
}
}

namespace testapp::vkfg {
bool ConfigureStreamlineAfterDevice() {
    if (!g_App.sl.initialized || !g_App.sl.setVulkanInfo || g_App.vk.device == VK_NULL_HANDLE) {
        return false;
    }
    sl::VulkanInfo info{};
    info.instance = g_App.vk.instance;
    info.physicalDevice = g_App.vk.physicalDevice;
    info.device = g_App.vk.device;
    if (!g_App.vk.queuePlan.streamlineCompute.empty()) {
        info.computeQueueFamily = g_App.vk.queuePlan.streamlineCompute.front().familyIndex;
        info.computeQueueIndex = g_App.vk.queuePlan.streamlineCompute.front().queueIndex;
    }
    if (!g_App.vk.queuePlan.streamlineGraphics.empty()) {
        info.graphicsQueueFamily = g_App.vk.queuePlan.streamlineGraphics.front().familyIndex;
        info.graphicsQueueIndex = g_App.vk.queuePlan.streamlineGraphics.front().queueIndex;
    }
    if (!g_App.vk.queuePlan.streamlineOpticalFlow.empty()) {
        info.opticalFlowQueueFamily = g_App.vk.queuePlan.streamlineOpticalFlow.front().familyIndex;
        info.opticalFlowQueueIndex = g_App.vk.queuePlan.streamlineOpticalFlow.front().queueIndex;
        info.useNativeOpticalFlowMode = true;
    }
    const sl::Result setInfoResult = g_App.sl.setVulkanInfo(info);
    g_App.sl.vulkanInfoSet = setInfoResult == sl::Result::eOk;
    testapp::Log(
        "[FG-DIAG] slSetVulkanInfo result=%d(%s) graphics=%u:%u compute=%u:%u optical=%u:%u nativeOF=%d\n",
        static_cast<int>(setInfoResult), SlResultName(setInfoResult), info.graphicsQueueFamily,
        info.graphicsQueueIndex, info.computeQueueFamily, info.computeQueueIndex, info.opticalFlowQueueFamily,
        info.opticalFlowQueueIndex, info.useNativeOpticalFlowMode ? 1 : 0);
    if (!g_App.sl.vulkanInfoSet) {
        return false;
    }

    g_App.sl.proxyCreateWin32Surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        g_App.sl.proxyGetInstanceProcAddr(g_App.vk.instance, "vkCreateWin32SurfaceKHR"));
    g_App.sl.proxyDestroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
        g_App.sl.proxyGetInstanceProcAddr(g_App.vk.instance, "vkDestroySurfaceKHR"));
    g_App.sl.wsi.route = VulkanWsiRoute::StreamlineProxy;
    g_App.sl.wsi.createSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
        g_App.sl.proxyGetDeviceProcAddr(g_App.vk.device, "vkCreateSwapchainKHR"));
    g_App.sl.wsi.destroySwapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
        g_App.sl.proxyGetDeviceProcAddr(g_App.vk.device, "vkDestroySwapchainKHR"));
    g_App.sl.wsi.getSwapchainImages = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
        g_App.sl.proxyGetDeviceProcAddr(g_App.vk.device, "vkGetSwapchainImagesKHR"));
    g_App.sl.wsi.acquireNextImage = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
        g_App.sl.proxyGetDeviceProcAddr(g_App.vk.device, "vkAcquireNextImageKHR"));
    g_App.sl.wsi.queuePresent = reinterpret_cast<PFN_vkQueuePresentKHR>(
        g_App.sl.proxyGetDeviceProcAddr(g_App.vk.device, "vkQueuePresentKHR"));
    g_App.sl.wsi.deviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(
        g_App.sl.proxyGetDeviceProcAddr(g_App.vk.device, "vkDeviceWaitIdle"));
    const bool wsiComplete = g_App.sl.wsi.createSwapchain && g_App.sl.wsi.destroySwapchain &&
                             g_App.sl.wsi.getSwapchainImages && g_App.sl.wsi.acquireNextImage &&
                             g_App.sl.wsi.queuePresent && g_App.sl.wsi.deviceWaitIdle &&
                             g_App.sl.proxyCreateWin32Surface && g_App.sl.proxyDestroySurface;
    testapp::Log(
        "[FG-DIAG] Streamline mandatory Vulkan hooks complete=%d surface(create=%p,destroy=%p) "
        "swapchain(create=%p,destroy=%p,images=%p,acquire=%p,present=%p,waitIdle=%p)\n",
        wsiComplete ? 1 : 0, reinterpret_cast<void*>(g_App.sl.proxyCreateWin32Surface),
        reinterpret_cast<void*>(g_App.sl.proxyDestroySurface),
        reinterpret_cast<void*>(g_App.sl.wsi.createSwapchain),
        reinterpret_cast<void*>(g_App.sl.wsi.destroySwapchain),
        reinterpret_cast<void*>(g_App.sl.wsi.getSwapchainImages),
        reinterpret_cast<void*>(g_App.sl.wsi.acquireNextImage), reinterpret_cast<void*>(g_App.sl.wsi.queuePresent),
        reinterpret_cast<void*>(g_App.sl.wsi.deviceWaitIdle));

    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = g_App.vk.physicalDevice;
    auto querySupport = [&](sl::Feature feature, bool* supported) {
        const sl::Result result = FeatureRequirementsAvailable(feature)
                                      ? g_App.sl.isFeatureSupported(feature, adapter)
                                      : sl::Result::eErrorFeatureNotSupported;
        *supported = result == sl::Result::eOk;
        testapp::Log("[FG-DIAG] slIsFeatureSupported feature=%u result=%d(%s) supported=%d\n",
                     static_cast<unsigned>(feature), static_cast<int>(result), SlResultName(result),
                     *supported ? 1 : 0);
    };
    querySupport(sl::kFeatureDLSS, &g_App.sl.dlssSrSupported);
    querySupport(sl::kFeatureDLSS_G, &g_App.sl.dlssFgSupported);
    querySupport(sl::kFeatureReflex, &g_App.sl.reflexSupported);
    ResolveStreamlineFeatureFunctions();
    g_App.sl.reflexOptionsConfigured = false;
    SetReflexMode(false, "initial Reflex configuration");
    PollReflexState(true);
    if (!wsiComplete || !g_App.vk.queuePlan.streamlineAvailable) {
        g_App.sl.dlssFgSupported = false;
        testapp::Log(
            "[FG-DIAG] DLSS-G disabled after support query: WSI complete=%d reservedQueuePlan=%d\n",
            wsiComplete ? 1 : 0, g_App.vk.queuePlan.streamlineAvailable ? 1 : 0);
    }
    testapp::LogFlush();
    return wsiComplete;
}
}

namespace testapp::vkfg {
bool SetStreamlineFeaturesLoaded(bool loaded, const char* reason) {
    if (!g_App.sl.initialized || !g_App.sl.setFeatureLoaded) {
        return false;
    }
    if (g_App.sl.featuresLoaded == loaded) {
        return true;
    }
    const sl::Feature unloadOrder[] = {sl::kFeatureDLSS_G, sl::kFeatureDLSS, sl::kFeatureReflex};
    const sl::Feature loadOrder[] = {sl::kFeatureReflex, sl::kFeatureDLSS, sl::kFeatureDLSS_G};
    const sl::Feature* order = loaded ? loadOrder : unloadOrder;
    bool success = true;
    for (size_t index = 0; index < 3; ++index) {
        const sl::Result result = g_App.sl.setFeatureLoaded(order[index], loaded);
        testapp::Log("[FG-DIAG] slSetFeatureLoaded feature=%u loaded=%d reason=%s result=%d(%s)\n",
                     static_cast<unsigned>(order[index]), loaded ? 1 : 0, reason ? reason : "unknown",
                     static_cast<int>(result), SlResultName(result));
        success &= result == sl::Result::eOk;
    }
    if (success) {
        g_App.sl.featuresLoaded = loaded;
        if (loaded) {
            ResolveStreamlineFeatureFunctions();
            g_App.sl.reflexOptionsConfigured = false;
            SetReflexMode(false, "feature plugin reload baseline");
            PollReflexState(true);
        } else {
            g_App.sl.dlssSetOptions = nullptr;
            g_App.sl.dlssGetOptimalSettings = nullptr;
            g_App.sl.dlssgSetOptions = nullptr;
            g_App.sl.dlssgGetState = nullptr;
            g_App.sl.reflexGetState = nullptr;
            g_App.sl.reflexSetOptions = nullptr;
            g_App.sl.reflexSleep = nullptr;
            g_App.sl.reflexActive = false;
            g_App.sl.reflexOptionsConfigured = false;
            g_App.sl.reflexStateAvailable = false;
            g_App.sl.dlssgVsyncSupportKnown = false;
            g_App.sl.dlssgVsyncSupported = false;
            g_App.sl.dlssSrConfigured = false;
            g_App.sl.dlssFgConfigured = false;
        }
    }
    testapp::LogFlush();
    return success;
}
}

namespace testapp::vkfg {
bool ConfigureDlssSuperResolution(bool enabled) {
    g_App.sl.dlssSrConfigured = false;
    if (!g_App.sl.dlssSetOptions || !g_App.sl.dlssSrSupported) {
        if (enabled) {
            testapp::Log("[FG-DIAG] DLSS SR requested but unsupported; TAAU fallback remains active\n");
        }
        return false;
    }
    sl::DLSSOptions options{};
    options.mode = enabled && g_App.config.upscalingEnabled ? DlssQualityMode() : sl::DLSSMode::eOff;
    options.outputWidth = g_App.swapchain.extent.width;
    options.outputHeight = g_App.swapchain.extent.height;
    options.colorBuffersHDR = g_App.config.dlssHdrInput ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    options.useAutoExposure = sl::Boolean::eTrue;
    const sl::DLSSPreset preset = DlssPreset();
    options.dlaaPreset = preset;
    options.qualityPreset = preset;
    options.balancedPreset = preset;
    options.performancePreset = preset;
    options.ultraPerformancePreset = preset;
    options.ultraQualityPreset = preset;
    if (enabled && g_App.sl.dlssGetOptimalSettings) {
        sl::DLSSOptimalSettings optimal{};
        const sl::Result optimalResult = g_App.sl.dlssGetOptimalSettings(options, optimal);
        testapp::Log(
            "[FG-DIAG] slDLSSGetOptimalSettings result=%d(%s) optimal=%ux%u range=%ux%u..%ux%u app=%ux%u\n",
            static_cast<int>(optimalResult), SlResultName(optimalResult), optimal.optimalRenderWidth,
            optimal.optimalRenderHeight, optimal.renderWidthMin, optimal.renderHeightMin, optimal.renderWidthMax,
            optimal.renderHeightMax, g_App.renderer.renderWidth, g_App.renderer.renderHeight);
    }
    const sl::Result result = g_App.sl.dlssSetOptions(g_App.sl.viewport, options);
    g_App.sl.dlssSrConfigured = result == sl::Result::eOk && options.mode != sl::DLSSMode::eOff;
    testapp::Log(
        "[FG-DIAG] slDLSSSetOptions requested=%d configured=%d mode=%u preset=%u output=%ux%u result=%d(%s)\n",
        enabled ? 1 : 0, g_App.sl.dlssSrConfigured ? 1 : 0, static_cast<unsigned>(options.mode),
        static_cast<unsigned>(preset), options.outputWidth, options.outputHeight, static_cast<int>(result),
        SlResultName(result));
    return result == sl::Result::eOk;
}
}

namespace testapp::vkfg {
bool PrepareStreamlineMode() {
    if (!g_App.sl.initialized || !g_App.sl.vulkanInfoSet || !g_App.sl.dlssFgSupported) {
        testapp::Log("[FG-DIAG] DLSS preparation unavailable initialized=%d vkInfo=%d fgSupported=%d\n",
                     g_App.sl.initialized ? 1 : 0, g_App.sl.vulkanInfoSet ? 1 : 0,
                     g_App.sl.dlssFgSupported ? 1 : 0);
        return false;
    }
    if (!SetStreamlineFeaturesLoaded(true, "prepare DLSS owner")) {
        return false;
    }
    ConfigureDlssSuperResolution(true);
    return g_App.sl.wsi.createSwapchain && g_App.sl.wsi.queuePresent && g_App.sl.dlssgSetOptions;
}
}

namespace testapp::vkfg {
bool RetireStreamlinePresentation(SwapchainOwner nextOwner, const char* reason) {
    if (!g_App.sl.initialized) {
        return true;
    }
    ConfigureDlssSuperResolution(false);
    const bool reflexDisabled = SetReflexMode(false, reason);
    bool pluginsReady = true;
    if (nextOwner == SwapchainOwner::FidelityFX) {
        pluginsReady = SetStreamlineFeaturesLoaded(false, reason);
    }
    testapp::Log(
        "[FG-TRANSITION] Streamline presentation retired nextOwner=%s reason=%s reflexOff=%d "
        "pluginsLoaded=%d result=%d\n",
        OwnerName(nextOwner), reason ? reason : "unknown", reflexDisabled ? 1 : 0,
        g_App.sl.featuresLoaded ? 1 : 0, reflexDisabled && pluginsReady ? 1 : 0);
    testapp::LogFlush();
    return reflexDisabled && pluginsReady;
}
}

namespace testapp::vkfg {
bool SetDlssFrameGeneration(bool enabled, const char* reason) {
    if (!g_App.sl.dlssgSetOptions || !g_App.sl.dlssFgSupported) {
        return !enabled;
    }
    if (enabled && !SetReflexMode(true, "DLSS proxy activation")) {
        testapp::Log("[FG-DIAG] WARN Reflex could not enter low-latency mode before DLSS FG\n");
    }
    sl::DLSSGOptions options{};
    options.mode = enabled ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    // Repeated-key suspension promises to retain this proxy and its contexts. Tell the plugin to
    // retain its internal FG resources too; otherwise eOff synchronously flushes and destroys the
    // worker state, causing a multi-second hitch before the passthrough frame can be presented.
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    options.numFramesToGenerate = 1;
    options.numBackBuffers = static_cast<uint32_t>(g_App.swapchain.images.size());
    options.colorWidth = g_App.swapchain.extent.width;
    options.colorHeight = g_App.swapchain.extent.height;
    options.mvecDepthWidth = g_App.renderer.renderWidth;
    options.mvecDepthHeight = g_App.renderer.renderHeight;
    options.colorBufferFormat = static_cast<uint32_t>(g_App.swapchain.format);
    options.mvecBufferFormat = static_cast<uint32_t>(kMotionFormat);
    options.depthBufferFormat = static_cast<uint32_t>(kDepthFormat);
    options.hudLessBufferFormat = static_cast<uint32_t>(kSceneColorFormat);
    options.uiBufferFormat = static_cast<uint32_t>(kUiFormat);
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    options.enableUserInterfaceRecomposition = sl::Boolean::eTrue;
    const auto setOptionsStart = std::chrono::steady_clock::now();
    const sl::Result result = g_App.sl.dlssgSetOptions(g_App.sl.viewport, options);
    const double setOptionsMs = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - setOptionsStart)
                                    .count();
    if (result == sl::Result::eOk) {
        g_App.sl.dlssFgConfigured = enabled;
    }
    testapp::Log(
        "[FG-DIAG] slDLSSGSetOptions requested=%d configured=%d reason=%s result=%d(%s) size=%ux%u "
        "mvecDepth=%ux%u buffers=%u flags=0x%x retainWhenOff=1 durationMs=%.3f reflex=%d\n",
        enabled ? 1 : 0, g_App.sl.dlssFgConfigured ? 1 : 0, reason ? reason : "unknown",
        static_cast<int>(result), SlResultName(result), options.colorWidth, options.colorHeight,
        options.mvecDepthWidth, options.mvecDepthHeight, options.numBackBuffers,
        static_cast<unsigned>(options.flags), setOptionsMs,
        g_App.sl.reflexActive ? 1 : 0);
    if (setOptionsMs > 50.0) {
        testapp::Log(
            "[FG-PACING] slow slDLSSGSetOptions requested=%d durationMs=%.3f "
            "retainWhenOff=1 reason=%s\n",
            enabled ? 1 : 0, setOptionsMs, reason ? reason : "unknown");
    }
    testapp::LogFlush();
    return result == sl::Result::eOk;
}
}

namespace testapp::vkfg {
sl::FrameToken* BeginStreamlineFrame() {
    if (!g_App.sl.initialized || !g_App.sl.featuresLoaded || !g_App.sl.getNewFrameToken) {
        return nullptr;
    }
    uint32_t frameIndex = g_App.sl.frameTokenIndex++;
    sl::FrameToken* token = nullptr;
    const sl::Result result = g_App.sl.getNewFrameToken(token, &frameIndex);
    if (result != sl::Result::eOk || !token) {
        testapp::Log("[FG-DIAG] slGetNewFrameToken frame=%u result=%d(%s) token=%p\n", frameIndex,
                     static_cast<int>(result), SlResultName(result), token);
        return nullptr;
    }
    if (g_App.sl.reflexSleep) {
        const sl::Result sleepResult = g_App.sl.reflexSleep(*token);
        ++g_App.sl.reflexSleepCalls;
        if (sleepResult != sl::Result::eOk) {
            ++g_App.sl.reflexSleepFailures;
        }
        if (sleepResult != sl::Result::eOk || frameIndex < 5 || (frameIndex % 240) == 0) {
            testapp::Log(
                "[FG-DIAG] slReflexSleep frame=%u mode=%s frameLimitUs=0 result=%d(%s) "
                "calls=%llu failures=%llu\n",
                frameIndex, g_App.sl.reflexActive ? "low-latency" : "off",
                static_cast<int>(sleepResult), SlResultName(sleepResult),
                static_cast<unsigned long long>(g_App.sl.reflexSleepCalls),
                static_cast<unsigned long long>(g_App.sl.reflexSleepFailures));
        }
    }
    return token;
}
}

namespace testapp::vkfg {
void SetStreamlineMarker(sl::FrameToken* token, sl::PCLMarker marker, const char* name) {
    if (!token || !g_App.sl.pclSetMarker) {
        return;
    }
    const sl::Result result = g_App.sl.pclSetMarker(marker, *token);
    if (result != sl::Result::eOk && g_App.frameId < 8) {
        testapp::Log("[FG-DIAG] slPCLSetMarker name=%s result=%d(%s)\n", name ? name : "?",
                     static_cast<int>(result), SlResultName(result));
    }
}
}

namespace testapp::vkfg {
bool RecordStreamlineInputsAndUpscale(VkCommandBuffer commandBuffer, FrameResources& resources,
                                     sl::FrameToken* frameToken, const JitterOffset& jitter,
                                     uint32_t backbufferIndex) {
    if (!frameToken || !g_App.sl.setConstants || !g_App.sl.setTagForFrame) {
        return false;
    }
    if (backbufferIndex >= g_App.swapchain.images.size() ||
        backbufferIndex >= g_App.swapchain.views.size()) {
        testapp::Log("[FG-DIAG] ERROR Streamline backbuffer tag index=%u images=%zu views=%zu\n",
                     backbufferIndex, g_App.swapchain.images.size(), g_App.swapchain.views.size());
        return false;
    }
    const float aspect = static_cast<float>(g_App.renderer.renderWidth) /
                         static_cast<float>(std::max(g_App.renderer.renderHeight, 1u));
    const SceneCameraPolicy camera = BuildSceneCameraPolicy(aspect);
    const SceneProjectionPolicy projection = BuildSceneProjectionPolicy(camera);
    sl::Constants constants{};
    constants.cameraViewToClip = MakeSlMatrix(projection.viewToClip);
    constants.clipToCameraView = MakeSlMatrix(projection.clipToView);
    constants.clipToLensClip = IdentitySlMatrix();
    constants.clipToPrevClip = IdentitySlMatrix();
    constants.prevClipToClip = IdentitySlMatrix();
    constants.jitterOffset = sl::float2(jitter.x, jitter.y);
    // The shader emits UV-space prev-current motion. Streamline consumes normalized NDC motion,
    // while FidelityFX separately receives the render-resolution scale in its own descriptor.
    constants.mvecScale = sl::float2(2.0f, -2.0f);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(camera.position[0], camera.position[1], camera.position[2]);
    constants.cameraUp = sl::float3(camera.up[0], camera.up[1], camera.up[2]);
    constants.cameraRight = sl::float3(camera.right[0], camera.right[1], camera.right[2]);
    constants.cameraFwd = sl::float3(camera.forward[0], camera.forward[1], camera.forward[2]);
    constants.cameraNear = camera.nearPlane;
    constants.cameraFar = camera.farPlane;
    constants.cameraFOV = camera.verticalFov;
    constants.cameraAspectRatio = camera.aspect;
    constants.motionVectorsInvalidValue = 65504.0f;
    constants.depthInverted = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = g_App.resetTemporalHistory ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    const sl::Result constantsResult = g_App.sl.setConstants(constants, *frameToken, g_App.sl.viewport);

    sl::Resource scene = MakeSlResource(resources.sceneColor);
    sl::Resource output = MakeSlResource(resources.hudlessColor);
    sl::Resource depth = MakeSlResource(resources.depth);
    sl::Resource motion = MakeSlResource(resources.motionVectors);
    sl::Resource hudless = MakeSlResource(resources.hudlessColor);
    sl::Resource ui = MakeSlResource(resources.uiColor);
    sl::Resource reactive = MakeSlResource(resources.reactiveMask);
    sl::Resource transparency = MakeSlResource(resources.transparencyMask);
    sl::Resource backbuffer(sl::ResourceType::eTex2d,
                            NativeHandleToVoid(g_App.swapchain.images[backbufferIndex]), nullptr,
                            NativeHandleToVoid(g_App.swapchain.views[backbufferIndex]),
                            static_cast<uint32_t>(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR));
    backbuffer.width = g_App.swapchain.extent.width;
    backbuffer.height = g_App.swapchain.extent.height;
    backbuffer.nativeFormat = static_cast<uint32_t>(g_App.swapchain.format);
    backbuffer.mipLevels = 1;
    backbuffer.arrayLayers = 1;
    backbuffer.usage = g_App.swapchain.createInfo.imageUsage;
    sl::Extent renderExtent{};
    renderExtent.width = g_App.renderer.renderWidth;
    renderExtent.height = g_App.renderer.renderHeight;
    sl::Extent displayExtent{};
    displayExtent.width = g_App.swapchain.extent.width;
    displayExtent.height = g_App.swapchain.extent.height;
    const sl::ResourceTag tags[] = {
        sl::ResourceTag(&scene, sl::kBufferTypeScalingInputColor,
                        sl::ResourceLifecycle::eValidUntilPresent, &renderExtent),
        sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor,
                        sl::ResourceLifecycle::eValidUntilPresent, &displayExtent),
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent,
                        &renderExtent),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors,
                        sl::ResourceLifecycle::eValidUntilPresent, &renderExtent),
        sl::ResourceTag(&hudless, sl::kBufferTypeHUDLessColor,
                        sl::ResourceLifecycle::eValidUntilPresent, &displayExtent),
        sl::ResourceTag(&ui, sl::kBufferTypeUIColorAndAlpha,
                        sl::ResourceLifecycle::eValidUntilPresent, &displayExtent),
        sl::ResourceTag(&backbuffer, sl::kBufferTypeBackbuffer,
                        sl::ResourceLifecycle::eValidUntilPresent, &displayExtent),
        sl::ResourceTag(&reactive, sl::kBufferTypeReactiveMaskHint,
                        sl::ResourceLifecycle::eValidUntilPresent, &renderExtent),
        sl::ResourceTag(&transparency, sl::kBufferTypeTransparencyAndCompositionMaskHint,
                        sl::ResourceLifecycle::eValidUntilPresent, &renderExtent),
    };
    const sl::Result tagResult = g_App.sl.setTagForFrame(
        *frameToken, g_App.sl.viewport, tags, static_cast<uint32_t>(std::size(tags)),
        reinterpret_cast<sl::CommandBuffer*>(commandBuffer));
    sl::Result evaluateResult = sl::Result::eOk;
    if (g_App.sl.dlssSrConfigured && g_App.config.upscalingEnabled) {
        const sl::BaseStructure* inputs[] = {&g_App.sl.viewport};
        evaluateResult = g_App.sl.evaluateFeature(
            sl::kFeatureDLSS, *frameToken, inputs, static_cast<uint32_t>(std::size(inputs)),
            reinterpret_cast<sl::CommandBuffer*>(commandBuffer));
    }
    if (g_App.frameId < 5 || constantsResult != sl::Result::eOk || tagResult != sl::Result::eOk ||
        evaluateResult != sl::Result::eOk || (g_App.frameId % 240) == 0) {
        testapp::Log(
            "[FG-DIAG] Streamline frame inputs frameID=%llu constants=%d(%s) tags=%d(%s) "
            "dlssEvaluate=%d(%s) srRequested=%d srConfigured=%d fgRequested=%d fgConfigured=%d reflex=%d\n",
            static_cast<unsigned long long>(g_App.frameId), static_cast<int>(constantsResult),
            SlResultName(constantsResult), static_cast<int>(tagResult), SlResultName(tagResult),
            static_cast<int>(evaluateResult), SlResultName(evaluateResult),
            g_App.config.upscalingEnabled ? 1 : 0, g_App.sl.dlssSrConfigured ? 1 : 0,
            g_App.transition.currentMode == FgMode::Dlss && !g_App.transition.suspended ? 1 : 0,
            g_App.sl.dlssFgConfigured ? 1 : 0, g_App.sl.reflexActive ? 1 : 0);
    }
    return constantsResult == sl::Result::eOk && tagResult == sl::Result::eOk &&
           evaluateResult == sl::Result::eOk;
}
}

namespace testapp::vkfg {
void PollStreamlineState() {
    PollReflexState(false);
    if (!g_App.sl.dlssgGetState || g_App.swapchain.owner != SwapchainOwner::Streamline) {
        return;
    }
    sl::DLSSGState state{};
    const sl::Result result = g_App.sl.dlssgGetState(g_App.sl.viewport, state, nullptr);
    if (result == sl::Result::eOk) {
        const bool vsyncSupported = state.bIsVsyncSupportAvailable == sl::Boolean::eTrue;
        if (!g_App.sl.dlssgVsyncSupportKnown ||
            g_App.sl.dlssgVsyncSupported != vsyncSupported) {
            g_App.sl.dlssgVsyncSupportKnown = true;
            g_App.sl.dlssgVsyncSupported = vsyncSupported;
            testapp::Log(
                "[FG-DIAG] DLSS-G Vulkan VSync support available=%d; frameLimitUs=0 "
                "(no application-side limiter or emulation)\n",
                vsyncSupported ? 1 : 0);
            if (!vsyncSupported) {
                testapp::Log(
                    "[FG-DIAG] WARN automatic VSync/VRR below-refresh pacing is unavailable "
                    "while Streamline Vulkan DLSS-G owns presentation\n");
            }
            testapp::LogFlush();
        }
        const uint32_t presented = std::max(state.numFramesActuallyPresented, 1u);
        if (presented > 1) {
            g_App.generatedFrames += presented - 1;
            g_App.sl.generatedPresentCount += presented - 1;
        }
    }
    if (g_App.frameId < 5 || result != sl::Result::eOk || (g_App.frameId % 120) == 0) {
        testapp::Log(
            "[FG-DIAG] slDLSSGGetState result=%d(%s) status=0x%x presented=%u maxGenerated=%u "
            "dynamicMFG=%d vsyncSupport=%d requested=%d configured=%d effective=%d\n",
            static_cast<int>(result), SlResultName(result), static_cast<unsigned>(state.status),
            state.numFramesActuallyPresented, state.numFramesToGenerateMax,
            state.bIsDynamicMFGSupported == sl::Boolean::eTrue ? 1 : 0,
            state.bIsVsyncSupportAvailable == sl::Boolean::eTrue ? 1 : 0,
            g_App.transition.currentMode == FgMode::Dlss && !g_App.transition.suspended ? 1 : 0,
            g_App.sl.dlssFgConfigured ? 1 : 0,
            result == sl::Result::eOk && state.status == sl::DLSSGStatus::eOk &&
                    g_App.sl.dlssFgConfigured
                ? 1
                : 0);
    }
}
}

namespace testapp::vkfg {
void ShutdownStreamline() {
    if (!g_App.sl.initialized) {
        if (g_App.sl.module) {
            FreeLibrary(g_App.sl.module);
            g_App.sl.module = nullptr;
        }
        return;
    }
    SetReflexMode(false, "final Streamline shutdown");
    const sl::Result result = g_App.sl.shutdown ? g_App.sl.shutdown() : sl::Result::eErrorNotInitialized;
    testapp::Log("[FG-DIAG] slShutdown(before Vulkan destruction) result=%d(%s)\n", static_cast<int>(result),
                 SlResultName(result));
    g_App.sl.initialized = false;
    if (g_App.sl.module) {
        FreeLibrary(g_App.sl.module);
        g_App.sl.module = nullptr;
    }
    testapp::LogFlush();
}
}
