#include "vulkan_fg_switch_test_internal.h"

namespace testapp::vkfg {
namespace {
void AppendUnique(std::vector<const char*>* values, const char* name) {
    if (!name || !*name) {
        return;
    }
    for (const char* existing : *values) {
        if (std::strcmp(existing, name) == 0) {
            return;
        }
    }
    values->push_back(name);
}
}
}

namespace testapp::vkfg {
namespace {
std::vector<const char*> FeatureNamePointers(const std::vector<std::string>& names) {
    std::vector<const char*> pointers;
    pointers.reserve(names.size());
    for (const std::string& name : names) {
        pointers.push_back(name.c_str());
    }
    return pointers;
}
}
}

namespace testapp::vkfg {
namespace {
VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                   VkDebugUtilsMessageTypeFlagsEXT type,
                                                   const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const char* severityName = "info";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        severityName = "error";
        ++g_App.validationErrors;
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        severityName = "warning";
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        severityName = "verbose";
    }
    testapp::Log("[VK-VALIDATION] severity=%s type=0x%x id=%d name='%s' message=%s\n", severityName,
                 static_cast<unsigned>(type), data ? data->messageIdNumber : 0,
                 data && data->pMessageIdName ? data->pMessageIdName : "?",
                 data && data->pMessage ? data->pMessage : "");
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        testapp::LogFlush();
    }
    return VK_FALSE;
}
}
}

namespace testapp::vkfg {
namespace {
VkDebugUtilsMessengerCreateInfoEXT MakeDebugMessengerCreateInfo() {
    VkDebugUtilsMessengerCreateInfoEXT info = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = DebugUtilsCallback;
    return info;
}
}
}

namespace testapp::vkfg {
namespace {
std::vector<VkExtensionProperties> EnumerateInstanceExtensions() {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> values(count);
    if (count) {
        vkEnumerateInstanceExtensionProperties(nullptr, &count, values.data());
        values.resize(count);
    }
    return values;
}
}
}

namespace testapp::vkfg {
namespace {
std::vector<VkLayerProperties> EnumerateInstanceLayers() {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> values(count);
    if (count) {
        vkEnumerateInstanceLayerProperties(&count, values.data());
        values.resize(count);
    }
    return values;
}
}
}

namespace testapp::vkfg {
namespace {
bool ContainsLayer(const std::vector<VkLayerProperties>& layers, const char* name) {
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}
}
}

namespace testapp::vkfg {
namespace {
std::vector<VkExtensionProperties> EnumerateDeviceExtensions(VkPhysicalDevice physicalDevice) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> values(count);
    if (count) {
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, values.data());
        values.resize(count);
    }
    return values;
}
}
}

namespace testapp::vkfg {
namespace {
VulkanWsiDispatch LoaderWsiDispatch() {
    VulkanWsiDispatch wsi{};
    wsi.route = VulkanWsiRoute::Loader;
    wsi.createSwapchain = vkCreateSwapchainKHR;
    wsi.destroySwapchain = vkDestroySwapchainKHR;
    wsi.getSwapchainImages = vkGetSwapchainImagesKHR;
    wsi.acquireNextImage = vkAcquireNextImageKHR;
    wsi.queuePresent = vkQueuePresentKHR;
    wsi.deviceWaitIdle = vkDeviceWaitIdle;
    return wsi;
}
}
}

namespace testapp::vkfg {
namespace {
VkQueue QueueFromRef(const VulkanQueueRef& ref) {
    VkQueue queue = VK_NULL_HANDLE;
    if (ref.Valid()) {
        vkGetDeviceQueue(g_App.vk.device, ref.familyIndex, ref.queueIndex, &queue);
    }
    return queue;
}
}
}

namespace testapp::vkfg {
namespace {
bool IsPhysicalDeviceUsable(VkPhysicalDevice device) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    if (VK_VERSION_MAJOR(properties.apiVersion) < 1 ||
        (VK_VERSION_MAJOR(properties.apiVersion) == 1 && VK_VERSION_MINOR(properties.apiVersion) < 2)) {
        testapp::Log("[FG-DIAG] Adapter '%s' rejected: Vulkan %u.%u is below 1.2\n", properties.deviceName,
                     VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion));
        return false;
    }
    const auto extensions = EnumerateDeviceExtensions(device);
    return ContainsName(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
}
}
}

namespace testapp::vkfg {
namespace {
void LogQueuePlan(const std::vector<VulkanQueueFamilyCaps>& caps) {
    for (const VulkanQueueFamilyCaps& family : caps) {
        testapp::Log(
            "[FG-DIAG] Queue family=%u count=%u graphics=%d compute=%d transfer=%d present=%d opticalFlow=%d "
            "requested=%u\n",
            family.familyIndex, family.queueCount, family.graphics ? 1 : 0, family.compute ? 1 : 0,
            family.transfer ? 1 : 0, family.present ? 1 : 0, family.opticalFlow ? 1 : 0,
            family.familyIndex < g_App.vk.queuePlan.requestedQueueCounts.size()
                ? g_App.vk.queuePlan.requestedQueueCounts[family.familyIndex]
                : 0);
    }
    const auto logRef = [](const char* name, const VulkanQueueRef& ref) {
        testapp::Log("[FG-DIAG] Queue role=%s available=%d family=%u index=%u\n", name, ref.Valid() ? 1 : 0,
                     ref.familyIndex, ref.queueIndex);
    };
    logRef("game", g_App.vk.queuePlan.game);
    logRef("app-async-present", g_App.vk.queuePlan.asyncPresent);
    logRef("ffx-async-compute", g_App.vk.queuePlan.ffxAsyncCompute);
    logRef("ffx-present", g_App.vk.queuePlan.ffxPresent);
    logRef("ffx-image-acquire", g_App.vk.queuePlan.ffxImageAcquire);
    for (const VulkanQueueRef& ref : g_App.vk.queuePlan.streamlineGraphics) {
        logRef("streamline-graphics", ref);
    }
    for (const VulkanQueueRef& ref : g_App.vk.queuePlan.streamlineCompute) {
        logRef("streamline-compute", ref);
    }
    for (const VulkanQueueRef& ref : g_App.vk.queuePlan.streamlineOpticalFlow) {
        logRef("streamline-optical-flow", ref);
    }
}
}
}

namespace testapp::vkfg {
bool InitializeVulkanDevice() {
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&loaderVersion);
    const uint32_t instanceApiVersion = loaderVersion >= VK_API_VERSION_1_3
                                            ? VK_API_VERSION_1_3
                                            : VK_API_VERSION_1_2;
    testapp::Log("[FG-DIAG] Vulkan loader version=%u.%u.%u requested=%u.%u.%u baseline=1.2\n",
                 VK_VERSION_MAJOR(loaderVersion), VK_VERSION_MINOR(loaderVersion), VK_VERSION_PATCH(loaderVersion),
                 VK_VERSION_MAJOR(instanceApiVersion), VK_VERSION_MINOR(instanceApiVersion),
                 VK_VERSION_PATCH(instanceApiVersion));
    if (loaderVersion < VK_API_VERSION_1_2) {
        testapp::Log("[FG-DIAG] ERROR Vulkan 1.2 loader is required\n");
        return false;
    }

    const auto instanceExtensions = EnumerateInstanceExtensions();
    std::vector<const char*> enabledInstanceExtensions;
    AppendUnique(&enabledInstanceExtensions, VK_KHR_SURFACE_EXTENSION_NAME);
    AppendUnique(&enabledInstanceExtensions, VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    for (SlFeatureRequirementCopy& requirement : g_App.sl.requirements) {
        for (const std::string& extension : requirement.instanceExtensions) {
            if (ContainsName(instanceExtensions, extension.c_str())) {
                AppendUnique(&enabledInstanceExtensions, extension.c_str());
            } else {
                requirement.instanceExtensionsAvailable = false;
                testapp::Log("[FG-DIAG] Streamline feature=%u missing instance extension '%s'\n",
                             static_cast<unsigned>(requirement.feature), extension.c_str());
            }
        }
    }

    const auto layers = EnumerateInstanceLayers();
    std::vector<const char*> enabledLayers;
    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo = MakeDebugMessengerCreateInfo();
    if (g_App.config.apiDebug) {
        if (ContainsLayer(layers, "VK_LAYER_KHRONOS_validation")) {
            enabledLayers.push_back("VK_LAYER_KHRONOS_validation");
            g_App.vk.validationEnabled = true;
        } else {
            testapp::Log("[FG-DIAG] WARN --vk-debug requested but VK_LAYER_KHRONOS_validation is unavailable\n");
        }
        if (ContainsName(instanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            AppendUnique(&enabledInstanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    }

    VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "CaptureProject Vulkan FG Switch Test";
    appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    appInfo.pEngineName = "CaptureProject testapp";
    appInfo.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    appInfo.apiVersion = instanceApiVersion;
    VkInstanceCreateInfo createInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pNext = g_App.config.apiDebug ? &debugCreateInfo : nullptr;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledInstanceExtensions.size());
    createInfo.ppEnabledExtensionNames = enabledInstanceExtensions.data();
    createInfo.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
    createInfo.ppEnabledLayerNames = enabledLayers.data();
    VkResult result = vkCreateInstance(&createInfo, nullptr, &g_App.vk.instance);
    testapp::Log("[FG-DIAG] vkCreateInstance result=%s(%d) instance=%p validation=%d extensions=%u\n",
                 VkResultName(result), static_cast<int>(result), reinterpret_cast<void*>(g_App.vk.instance),
                 g_App.vk.validationEnabled ? 1 : 0, createInfo.enabledExtensionCount);
    if (result != VK_SUCCESS) {
        return false;
    }

    if (g_App.config.apiDebug && ContainsName(instanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(g_App.vk.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createMessenger) {
            result = createMessenger(g_App.vk.instance, &debugCreateInfo, nullptr, &g_App.vk.debugMessenger);
            testapp::Log("[FG-DIAG] vkCreateDebugUtilsMessengerEXT result=%s(%d) messenger=%p\n",
                         VkResultName(result), static_cast<int>(result),
                         reinterpret_cast<void*>(g_App.vk.debugMessenger));
        }
    }

    if (g_App.sl.initialized && g_App.sl.proxyGetInstanceProcAddr) {
        g_App.sl.proxyCreateWin32Surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
            g_App.sl.proxyGetInstanceProcAddr(g_App.vk.instance, "vkCreateWin32SurfaceKHR"));
        g_App.sl.proxyDestroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
            g_App.sl.proxyGetInstanceProcAddr(g_App.vk.instance, "vkDestroySurfaceKHR"));
    }
    VkWin32SurfaceCreateInfoKHR surfaceCreateInfo = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfaceCreateInfo.hinstance = g_App.instanceHandle;
    surfaceCreateInfo.hwnd = g_App.hwnd;
    PFN_vkCreateWin32SurfaceKHR createSurface = g_App.sl.proxyCreateWin32Surface
                                                    ? g_App.sl.proxyCreateWin32Surface
                                                    : vkCreateWin32SurfaceKHR;
    result = createSurface(g_App.vk.instance, &surfaceCreateInfo, nullptr, &g_App.vk.surface);
    g_App.vk.surfaceCreatedByStreamline = createSurface == g_App.sl.proxyCreateWin32Surface &&
                                         g_App.sl.proxyCreateWin32Surface != nullptr;
    testapp::Log("[FG-DIAG] vkCreateWin32SurfaceKHR route=%s result=%s(%d) surface=%p\n",
                 g_App.vk.surfaceCreatedByStreamline ? "streamline-proxy" : "loader",
                 VkResultName(result), static_cast<int>(result),
                 reinterpret_cast<void*>(g_App.vk.surface));
    if (result != VK_SUCCESS) {
        return false;
    }

    uint32_t physicalDeviceCount = 0;
    vkEnumeratePhysicalDevices(g_App.vk.instance, &physicalDeviceCount, nullptr);
    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    if (physicalDeviceCount) {
        vkEnumeratePhysicalDevices(g_App.vk.instance, &physicalDeviceCount, physicalDevices.data());
    }
    auto hasGraphicsPresentQueue = [](VkPhysicalDevice device) {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
        std::vector<VkQueueFamilyProperties> properties(count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, properties.data());
        for (uint32_t family = 0; family < count; ++family) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, family, g_App.vk.surface, &present);
            if ((properties[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && present) {
                return true;
            }
        }
        return false;
    };
    for (VkPhysicalDevice candidate : physicalDevices) {
        if (IsPhysicalDeviceUsable(candidate) && hasGraphicsPresentQueue(candidate)) {
            g_App.vk.physicalDevice = candidate;
            break;
        }
    }
    if (g_App.vk.physicalDevice == VK_NULL_HANDLE) {
        testapp::Log("[FG-DIAG] ERROR no Vulkan 1.2 physical device with VK_KHR_swapchain\n");
        return false;
    }

    vkGetPhysicalDeviceProperties(g_App.vk.physicalDevice, &g_App.vk.properties);
    vkGetPhysicalDeviceMemoryProperties(g_App.vk.physicalDevice, &g_App.vk.memoryProperties);
    testapp::Log(
        "[FG-DIAG] Adapter='%s' vendor=0x%04x device=0x%04x type=%u driver=0x%08x api=%u.%u.%u limits.maxImage2D=%u\n",
        g_App.vk.properties.deviceName, g_App.vk.properties.vendorID, g_App.vk.properties.deviceID,
        static_cast<unsigned>(g_App.vk.properties.deviceType), g_App.vk.properties.driverVersion,
        VK_VERSION_MAJOR(g_App.vk.properties.apiVersion), VK_VERSION_MINOR(g_App.vk.properties.apiVersion),
        VK_VERSION_PATCH(g_App.vk.properties.apiVersion), g_App.vk.properties.limits.maxImageDimension2D);

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_App.vk.physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> familyProperties(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(g_App.vk.physicalDevice, &familyCount, familyProperties.data());
    std::vector<VulkanQueueFamilyCaps> queueCaps;
    queueCaps.reserve(familyCount);
    for (uint32_t family = 0; family < familyCount; ++family) {
        const VkQueueFlags flags = familyProperties[family].queueFlags;
        VulkanQueueFamilyCaps caps{};
        caps.familyIndex = family;
        caps.queueCount = familyProperties[family].queueCount;
        caps.graphics = (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
        caps.compute = (flags & VK_QUEUE_COMPUTE_BIT) != 0;
        caps.transfer = (flags & VK_QUEUE_TRANSFER_BIT) != 0;
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_App.vk.physicalDevice, family, g_App.vk.surface,
                                             &present);
        caps.present = present == VK_TRUE;
        caps.opticalFlow = (flags & VK_QUEUE_OPTICAL_FLOW_BIT_NV) != 0;
        queueCaps.push_back(caps);
    }

    VulkanQueueRequirements queueRequirements{};
    queueRequirements.requestAsyncPresent = g_App.asyncPresentRequested;
    for (const SlFeatureRequirementCopy& requirement : g_App.sl.requirements) {
        if (!requirement.instanceExtensionsAvailable) {
            continue;
        }
        queueRequirements.streamlineGraphicsQueues =
            std::max(queueRequirements.streamlineGraphicsQueues, requirement.graphicsQueues);
        queueRequirements.streamlineComputeQueues =
            std::max(queueRequirements.streamlineComputeQueues, requirement.computeQueues);
        queueRequirements.streamlineOpticalFlowQueues =
            std::max(queueRequirements.streamlineOpticalFlowQueues, requirement.opticalFlowQueues);
    }
    g_App.vk.queuePlan = BuildVulkanQueuePlan(queueCaps, queueRequirements);
    if (!g_App.vk.queuePlan.baseAvailable) {
        testapp::Log("[FG-DIAG] ERROR no graphics+Win32-present game queue\n");
        return false;
    }
    LogQueuePlan(queueCaps);

    const auto deviceExtensions = EnumerateDeviceExtensions(g_App.vk.physicalDevice);
    std::vector<const char*> enabledDeviceExtensions;
    AppendUnique(&enabledDeviceExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    // FidelityFX SDK 1.1.4's Vulkan backend detects dedicated-allocation support from the
    // physical-device extension list and then resolves the KHR-suffixed memory-requirements
    // entry point. The official Cauldron Vulkan device setup enables both extensions even
    // when their functionality has been promoted to core Vulkan. Do the same so the signed
    // runtime cannot observe support without a callable device entry point.
    const bool ffxMemoryRequirements2Available =
        ContainsName(deviceExtensions, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
    const bool ffxDedicatedAllocationAvailable =
        ContainsName(deviceExtensions, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
    if (ffxMemoryRequirements2Available) {
        AppendUnique(&enabledDeviceExtensions, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
    }
    if (ffxDedicatedAllocationAvailable) {
        AppendUnique(&enabledDeviceExtensions, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
    }
    testapp::Log(
        "[FG-DIAG] FidelityFX Vulkan memory extensions getMemoryRequirements2=%d "
        "dedicatedAllocation=%d\n",
        ffxMemoryRequirements2Available ? 1 : 0, ffxDedicatedAllocationAvailable ? 1 : 0);
    for (SlFeatureRequirementCopy& requirement : g_App.sl.requirements) {
        for (const std::string& extension : requirement.deviceExtensions) {
            if (ContainsName(deviceExtensions, extension.c_str())) {
                AppendUnique(&enabledDeviceExtensions, extension.c_str());
            } else {
                requirement.deviceExtensionsAvailable = false;
                testapp::Log("[FG-DIAG] Streamline feature=%u missing device extension '%s'\n",
                             static_cast<unsigned>(requirement.feature), extension.c_str());
            }
        }
    }
    if (ContainsName(deviceExtensions, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
        AppendUnique(&enabledDeviceExtensions, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        g_App.vk.memoryBudgetEnabled = true;
    }
    if (g_App.config.apiDebug && ContainsName(deviceExtensions, VK_EXT_DEVICE_FAULT_EXTENSION_NAME)) {
        AppendUnique(&enabledDeviceExtensions, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        g_App.vk.deviceFaultEnabled = true;
    }

    const bool opticalExtensionEnabled = std::any_of(
        enabledDeviceExtensions.begin(), enabledDeviceExtensions.end(), [](const char* extension) {
            return std::strcmp(extension, VK_NV_OPTICAL_FLOW_EXTENSION_NAME) == 0;
        });
    const bool useVulkan13Features = instanceApiVersion >= VK_API_VERSION_1_3 &&
                                     g_App.vk.properties.apiVersion >= VK_API_VERSION_1_3;
    VkPhysicalDeviceVulkan12Features supported12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features supported13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceOpticalFlowFeaturesNV supportedOptical = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV};
    supported12.pNext = useVulkan13Features ? static_cast<void*>(&supported13)
                                            : (opticalExtensionEnabled
                                                   ? static_cast<void*>(&supportedOptical)
                                                   : nullptr);
    supported13.pNext = opticalExtensionEnabled ? &supportedOptical : nullptr;
    VkPhysicalDeviceFeatures2 supportedFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supportedFeatures.pNext = &supported12;
    vkGetPhysicalDeviceFeatures2(g_App.vk.physicalDevice, &supportedFeatures);

    bool dlssgRequirementsAvailable = false;
    for (SlFeatureRequirementCopy& requirement : g_App.sl.requirements) {
        std::vector<const char*> names12 = FeatureNamePointers(requirement.features12);
        std::vector<const char*> names13 = FeatureNamePointers(requirement.features13);
        const VkPhysicalDeviceVulkan12Features required12 =
            sl::getVkPhysicalDeviceVulkan12Features(static_cast<uint32_t>(names12.size()), names12.data());
        const VkPhysicalDeviceVulkan13Features required13 =
            sl::getVkPhysicalDeviceVulkan13Features(static_cast<uint32_t>(names13.size()), names13.data());
        const VkPhysicalDeviceOpticalFlowFeaturesNV requiredOptical =
            sl::getVkPhysicalDeviceOpticalFlowNVFeatures(static_cast<uint32_t>(names12.size()), names12.data());
        const bool supportedRequired12 = RequiredFeatureTailSupported(
            required12, supported12, &required12.samplerMirrorClampToEdge,
            &supported12.samplerMirrorClampToEdge);
        const bool supportedRequired13 = names13.empty() ||
            (useVulkan13Features && RequiredFeatureTailSupported(
                required13, supported13, &required13.robustImageAccess,
                &supported13.robustImageAccess));
        const bool supportedRequiredOptical = RequiredFeatureTailSupported(
            requiredOptical, supportedOptical, &requiredOptical.opticalFlow,
            &supportedOptical.opticalFlow);
        requirement.deviceFeaturesAvailable =
            supportedRequired12 && supportedRequired13 && supportedRequiredOptical;
        testapp::Log(
            "[FG-DIAG] Streamline Vulkan feature merge feature=%u required(1.2=%zu,1.3=%zu) "
            "supported(1.2=%d,1.3=%d,optical=%d) available=%d\n",
            static_cast<unsigned>(requirement.feature), requirement.features12.size(),
            requirement.features13.size(), supportedRequired12 ? 1 : 0,
            supportedRequired13 ? 1 : 0, supportedRequiredOptical ? 1 : 0,
            requirement.deviceFeaturesAvailable ? 1 : 0);
        if (requirement.feature == sl::kFeatureDLSS_G && requirement.instanceExtensionsAvailable &&
            requirement.deviceExtensionsAvailable && requirement.deviceFeaturesAvailable &&
            (requirement.flags & sl::FeatureRequirementFlags::eVulkanSupported)) {
            dlssgRequirementsAvailable = true;
        }
    }
    if (!dlssgRequirementsAvailable && g_App.vk.queuePlan.streamlineAvailable) {
        VulkanQueueRequirements ffxOnlyRequirements{};
        ffxOnlyRequirements.requestAsyncPresent = g_App.asyncPresentRequested;
        g_App.vk.queuePlan = BuildVulkanQueuePlan(queueCaps, ffxOnlyRequirements);
        testapp::Log(
            "[FG-DIAG] Streamline DLSS-G requirements unavailable; released reserved queues "
            "and replanned FidelityFX availability=%d\n",
            g_App.vk.queuePlan.fidelityFxAvailable ? 1 : 0);
        LogQueuePlan(queueCaps);
    }
    g_App.vk.asyncPresentActive =
        g_App.asyncPresentRequested && g_App.vk.queuePlan.asyncPresentAvailable;
    if (g_App.asyncPresentRequested && !g_App.vk.asyncPresentActive) {
        testapp::Log(
            "[FG-DIAG] WARN separate application present queue requested but no distinct "
            "graphics+compute+present queue exists in the game family; using game queue\n");
    }

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::vector<std::vector<float>> queuePriorities(familyCount);
    for (uint32_t family = 0; family < familyCount; ++family) {
        uint32_t requested = family < g_App.vk.queuePlan.requestedQueueCounts.size()
                                 ? g_App.vk.queuePlan.requestedQueueCounts[family]
                                 : 0;
        if (requested == 0) {
            continue;
        }
        requested = std::min(requested, familyProperties[family].queueCount);
        queuePriorities[family].assign(requested, 1.0f);
        VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = requested;
        queueInfo.pQueuePriorities = queuePriorities[family].data();
        queueCreateInfos.push_back(queueInfo);
    }

    VkPhysicalDeviceVulkan12Features enabled12 = supported12;
    enabled12.pNext = nullptr;
    VkPhysicalDeviceVulkan13Features enabled13 = supported13;
    enabled13.pNext = nullptr;
    VkPhysicalDeviceOpticalFlowFeaturesNV enabledOptical = supportedOptical;
    enabledOptical.pNext = nullptr;
    enabled12.pNext = useVulkan13Features ? static_cast<void*>(&enabled13)
                                          : (opticalExtensionEnabled
                                                 ? static_cast<void*>(&enabledOptical)
                                                 : nullptr);
    enabled13.pNext = opticalExtensionEnabled ? &enabledOptical : nullptr;
    VkPhysicalDeviceFeatures2 enabledFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enabledFeatures.features = supportedFeatures.features;
    enabledFeatures.pNext = &enabled12;

    VkDeviceCreateInfo deviceCreateInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceCreateInfo.pNext = &enabledFeatures;
    deviceCreateInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
    deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(enabledDeviceExtensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = enabledDeviceExtensions.data();
    result = vkCreateDevice(g_App.vk.physicalDevice, &deviceCreateInfo, nullptr, &g_App.vk.device);
    testapp::Log(
        "[FG-DIAG] vkCreateDevice result=%s(%d) device=%p extensions=%u ffxQueues=%d slQueues=%d "
        "asyncPresent(requested=%d,active=%d) memoryBudget=%d deviceFault=%d core13=%d "
        "opticalFlow=%d\n",
        VkResultName(result), static_cast<int>(result), reinterpret_cast<void*>(g_App.vk.device),
        deviceCreateInfo.enabledExtensionCount, g_App.vk.queuePlan.fidelityFxAvailable ? 1 : 0,
        g_App.vk.queuePlan.streamlineAvailable ? 1 : 0, g_App.asyncPresentRequested ? 1 : 0,
        g_App.vk.asyncPresentActive ? 1 : 0, g_App.vk.memoryBudgetEnabled ? 1 : 0,
        g_App.vk.deviceFaultEnabled ? 1 : 0, useVulkan13Features ? 1 : 0,
        opticalExtensionEnabled && enabledOptical.opticalFlow ? 1 : 0);
    if (result != VK_SUCCESS) {
        return false;
    }

    g_App.vk.gameQueue = QueueFromRef(g_App.vk.queuePlan.game);
    g_App.vk.asyncPresentQueue = QueueFromRef(g_App.vk.queuePlan.asyncPresent);
    g_App.vk.ffxAsyncQueue = QueueFromRef(g_App.vk.queuePlan.ffxAsyncCompute);
    g_App.vk.ffxPresentQueue = QueueFromRef(g_App.vk.queuePlan.ffxPresent);
    g_App.vk.ffxAcquireQueue = QueueFromRef(g_App.vk.queuePlan.ffxImageAcquire);
    for (const VulkanQueueRef& ref : g_App.vk.queuePlan.streamlineGraphics) {
        g_App.vk.streamlineGraphicsQueues.push_back(QueueFromRef(ref));
    }
    for (const VulkanQueueRef& ref : g_App.vk.queuePlan.streamlineCompute) {
        g_App.vk.streamlineComputeQueues.push_back(QueueFromRef(ref));
    }
    for (const VulkanQueueRef& ref : g_App.vk.queuePlan.streamlineOpticalFlow) {
        g_App.vk.streamlineOpticalFlowQueues.push_back(QueueFromRef(ref));
    }

    ConfigureStreamlineAfterDevice();

    VkBool32 presentSupported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_App.vk.physicalDevice, g_App.vk.queuePlan.game.familyIndex,
                                         g_App.vk.surface, &presentSupported);
    if (!presentSupported) {
        testapp::Log("[FG-DIAG] ERROR selected game queue cannot present to the created surface\n");
        return false;
    }

    g_App.swapchain.owner = SwapchainOwner::Native;
    g_App.swapchain.wsi = LoaderWsiDispatch();
    return true;
}
}

namespace testapp::vkfg {
VkQueue ApplicationPresentQueue() {
    if (g_App.vk.asyncPresentActive && g_App.vk.asyncPresentQueue != VK_NULL_HANDLE) {
        return g_App.vk.asyncPresentQueue;
    }
    return g_App.vk.gameQueue;
}
}

namespace testapp::vkfg {
const VulkanQueueRef& ApplicationPresentQueueRef() {
    if (g_App.vk.asyncPresentActive && g_App.vk.queuePlan.asyncPresent.Valid()) {
        return g_App.vk.queuePlan.asyncPresent;
    }
    return g_App.vk.queuePlan.game;
}
}

namespace testapp::vkfg {
void QueryMemoryBudgetStress() {
    if (!g_App.config.videoMemoryQueryStress || !g_App.vk.memoryBudgetEnabled ||
        g_App.vk.physicalDevice == VK_NULL_HANDLE) {
        return;
    }
    const int count = testapp::fg::ClampSwitchConfigInt(
        g_App.config.videoMemoryQueryCountPerFrame, 0, 512);
    VkDeviceSize totalBudget = 0;
    VkDeviceSize totalUsage = 0;
    for (int query = 0; query < count; ++query) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        properties.pNext = &budget;
        vkGetPhysicalDeviceMemoryProperties2(g_App.vk.physicalDevice, &properties);
        if (query == count - 1) {
            for (uint32_t heap = 0; heap < properties.memoryProperties.memoryHeapCount; ++heap) {
                totalBudget += budget.heapBudget[heap];
                totalUsage += budget.heapUsage[heap];
            }
        }
    }
    if ((g_App.frameId % 240) == 0) {
        testapp::Log("[FG-DIAG] VK_EXT_memory_budget queries=%d totalUsageMiB=%.1f totalBudgetMiB=%.1f\n", count,
                     static_cast<double>(totalUsage) / (1024.0 * 1024.0),
                     static_cast<double>(totalBudget) / (1024.0 * 1024.0));
    }
}
}

namespace testapp::vkfg {
void ReleaseVulkanSurfaceBeforeStreamlineShutdown() {
    if (g_App.vk.surface != VK_NULL_HANDLE) {
        if (g_App.vk.surfaceCreatedByStreamline && g_App.sl.proxyDestroySurface) {
            g_App.sl.proxyDestroySurface(g_App.vk.instance, g_App.vk.surface, nullptr);
        } else {
            vkDestroySurfaceKHR(g_App.vk.instance, g_App.vk.surface, nullptr);
        }
        testapp::Log("[FG-DIAG] Vulkan surface destroyed route=%s before Streamline shutdown\n",
                     g_App.vk.surfaceCreatedByStreamline ? "streamline-proxy" : "loader");
        g_App.vk.surface = VK_NULL_HANDLE;
        g_App.vk.surfaceCreatedByStreamline = false;
    }
}
}

namespace testapp::vkfg {
void ShutdownVulkanDevice() {
    ReleaseVulkanSurfaceBeforeStreamlineShutdown();
    if (g_App.vk.device != VK_NULL_HANDLE) {
        vkDestroyDevice(g_App.vk.device, nullptr);
        g_App.vk.device = VK_NULL_HANDLE;
    }
    g_App.vk.gameQueue = VK_NULL_HANDLE;
    g_App.vk.asyncPresentQueue = VK_NULL_HANDLE;
    g_App.vk.asyncPresentActive = false;
    if (g_App.vk.debugMessenger != VK_NULL_HANDLE) {
        auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(g_App.vk.instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger) {
            destroyMessenger(g_App.vk.instance, g_App.vk.debugMessenger, nullptr);
        }
        g_App.vk.debugMessenger = VK_NULL_HANDLE;
    }
    if (g_App.vk.instance != VK_NULL_HANDLE) {
        vkDestroyInstance(g_App.vk.instance, nullptr);
        g_App.vk.instance = VK_NULL_HANDLE;
    }
}
}
