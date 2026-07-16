// Included by vulkan_fg_switch_test.cpp; FidelityFX SDK 1.1.4 Vulkan SR/FG integration.

namespace testapp::vkfg {
namespace {

std::wstring FidelityFxRuntimePath() {
    wchar_t executable[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    std::wstring directory = executable;
    const size_t slash = directory.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        directory.resize(slash);
    }
    return directory + L"\\amd_fidelityfx_vk.dll";
}

void FidelityFxMessage(uint32_t type, const wchar_t* message) {
    testapp::Log("[FFX-LOG] type=%u %S\n", type, message ? message : L"");
}

bool MandatoryFidelityFxFunctionsAvailable(const ffxFunctions& functions) {
    return functions.CreateContext && functions.DestroyContext && functions.Configure &&
           functions.Query && functions.Dispatch;
}

FfxApiResource MakeFfxResource(const ImageResource& image, uint32_t state,
                               uint32_t additionalUsage = 0) {
    FfxApiResourceDescription description =
        ffxApiGetImageResourceDescriptionVK(image.image, image.createInfo, additionalUsage);
    return ffxApiGetResourceVK(NativeHandleToVoid(image.image), description, state);
}

VkImageLayout LayoutForFfxState(uint32_t state) {
    const FfxResourceStateBits bits{
        FFX_API_RESOURCE_STATE_PRESENT,
        FFX_API_RESOURCE_STATE_COPY_SRC,
        FFX_API_RESOURCE_STATE_COPY_DEST,
        FFX_API_RESOURCE_STATE_RENDER_TARGET,
        FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ,
    };
    switch (ResolveFfxResourceLayout(state, bits)) {
        case VulkanImageLayoutClass::Present:
            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        case VulkanImageLayoutClass::TransferSource:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case VulkanImageLayoutClass::TransferDestination:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case VulkanImageLayoutClass::ColorAttachment:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case VulkanImageLayoutClass::ShaderRead:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case VulkanImageLayoutClass::General:
        default:
            return VK_IMAGE_LAYOUT_GENERAL;
    }
}

void CallbackImageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout,
                          VkImageLayout newLayout, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) {
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

ffxReturnCode_t FidelityFxPresentCallback(ffxCallbackDescFrameGenerationPresent* params, void*) {
    if (!params || !params->commandList || !params->currentBackBuffer.resource ||
        !params->outputSwapChainBuffer.resource) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    const uint64_t callbackIndex = g_App.ffx.presentCallbackCount.fetch_add(1) + 1;
    VkCommandBuffer commandBuffer = reinterpret_cast<VkCommandBuffer>(params->commandList);
    VkImage source = reinterpret_cast<VkImage>(params->currentBackBuffer.resource);
    VkImage destination = reinterpret_cast<VkImage>(params->outputSwapChainBuffer.resource);
    const VkImageLayout sourceLayout = LayoutForFfxState(params->currentBackBuffer.state);
    const VkImageLayout destinationLayout = LayoutForFfxState(params->outputSwapChainBuffer.state);
    CallbackImageBarrier(commandBuffer, source, sourceLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT);
    CallbackImageBarrier(commandBuffer, destination, destinationLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.dstSubresource.layerCount = 1;
    copy.extent.width = std::min(params->currentBackBuffer.description.width,
                                 params->outputSwapChainBuffer.description.width);
    copy.extent.height = std::min(params->currentBackBuffer.description.height,
                                  params->outputSwapChainBuffer.description.height);
    copy.extent.depth = 1;
    vkCmdCopyImage(commandBuffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    CallbackImageBarrier(commandBuffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sourceLayout,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
    CallbackImageBarrier(commandBuffer, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         destinationLayout, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
    if (callbackIndex <= 5 || (callbackIndex % 120) == 0) {
        testapp::Log(
            "[FG-DIAG] FFX present callback count=%llu frameID=%llu generated=%d source=%p "
            "output=%p extent=%ux%u sourceState=0x%x outputState=0x%x\n",
            static_cast<unsigned long long>(callbackIndex),
            static_cast<unsigned long long>(params->frameID), params->isGeneratedFrame ? 1 : 0,
            params->currentBackBuffer.resource, params->outputSwapChainBuffer.resource, copy.extent.width,
            copy.extent.height, params->currentBackBuffer.state, params->outputSwapChainBuffer.state);
    }
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t FidelityFxFrameGenerationCallback(ffxDispatchDescFrameGeneration* params, void* userContext) {
    if (!params || !userContext || !g_App.ffx.functions.Dispatch) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    auto* context = static_cast<ffxContext*>(userContext);
    const ffxReturnCode_t result = g_App.ffx.functions.Dispatch(context, &params->header);
    const uint64_t callbackIndex = g_App.ffx.frameGenerationCallbackCount.fetch_add(1) + 1;
    if (result != FFX_API_RETURN_OK || callbackIndex <= 5 || (callbackIndex % 120) == 0) {
        testapp::Log(
            "[FG-DIAG] FFX frame-generation callback count=%llu frameID=%llu generated=%u "
            "present=%p output0=%p result=%u(%s)\n",
            static_cast<unsigned long long>(callbackIndex),
            static_cast<unsigned long long>(params->frameID), params->numGeneratedFrames,
            params->presentColor.resource, params->outputs[0].resource, result, FfxResultName(result));
    }
    return result;
}

bool ShouldUsePresentCallback(bool enabled) {
    if (!g_App.config.fsrPresentCallbackStress) {
        return true;
    }
    if (!enabled) {
        return g_App.ffx.lastPresentCallback;
    }
    const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() -
                                                       g_App.ffx.callbackStressStart).count();
    const int interval = std::max(1, g_App.config.fsrPresentCallbackToggleIntervalSeconds);
    return (static_cast<int>(elapsed) / interval) % 2 != 0;
}

void LogFfxProvider(const char* role, ffxContext* context) {
    if (!g_App.ffx.functions.Query || !context || !*context) {
        return;
    }
    ffxQueryGetProviderVersion query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const ffxReturnCode_t result = g_App.ffx.functions.Query(context, &query.header);
    testapp::Log("[FG-DIAG] FFX provider role=%s result=%u(%s) id=0x%llx name='%s'\n", role,
                 result, FfxResultName(result), static_cast<unsigned long long>(query.versionId),
                 query.versionName ? query.versionName : "unknown");
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FidelityFxGetDeviceProcAddr(VkDevice device,
                                                                     const char* name) {
    PFN_vkVoidFunction function = vkGetDeviceProcAddr(device, name);
    if (!function && name && std::strcmp(name, "vkGetBufferMemoryRequirements2KHR") == 0) {
        // SDK 1.1.4 asks only for the extension spelling after detecting the promoted
        // dedicated-allocation extension. Vulkan 1.1+ implementations are permitted to
        // expose only the core spelling when the extension itself was not enabled.
        function = vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements2");
    }
    return function;
}

bool CreateFidelityFxEffectContexts() {
    if (g_App.ffx.upscaleContext && g_App.ffx.frameGenerationContext) {
        return true;
    }
    g_App.ffx.upscaleBackend = {};
    g_App.ffx.upscaleBackend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    g_App.ffx.upscaleBackend.vkDevice = g_App.vk.device;
    g_App.ffx.upscaleBackend.vkPhysicalDevice = g_App.vk.physicalDevice;
    g_App.ffx.upscaleBackend.vkDeviceProcAddr = FidelityFxGetDeviceProcAddr;
    g_App.ffx.upscaleCreate = {};
    g_App.ffx.upscaleCreate.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    g_App.ffx.upscaleCreate.header.pNext = &g_App.ffx.upscaleBackend.header;
    g_App.ffx.upscaleCreate.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    g_App.ffx.upscaleCreate.maxRenderSize = {g_App.renderer.renderWidth, g_App.renderer.renderHeight};
    g_App.ffx.upscaleCreate.maxUpscaleSize = {g_App.swapchain.extent.width, g_App.swapchain.extent.height};
    g_App.ffx.upscaleCreate.fpMessage = FidelityFxMessage;
    const PFN_vkVoidFunction requirements2Khr =
        FidelityFxGetDeviceProcAddr(g_App.vk.device, "vkGetBufferMemoryRequirements2KHR");
    const PFN_vkVoidFunction requirements2Core =
        vkGetDeviceProcAddr(g_App.vk.device, "vkGetBufferMemoryRequirements2");
    testapp::Log(
        "[FG-DIAG] FidelityFX Vulkan proc preflight getBufferMemoryRequirements2KHR=%p "
        "core=%p resolver=%p\n",
        reinterpret_cast<void*>(requirements2Khr), reinterpret_cast<void*>(requirements2Core),
        reinterpret_cast<void*>(g_App.ffx.upscaleBackend.vkDeviceProcAddr));
    testapp::LogFlush();
    if (!requirements2Khr) {
        testapp::Log(
            "[FG-DIAG] ERROR FidelityFX Vulkan backend requires buffer-memory-requirements2 "
            "but neither the KHR nor core device entry point is callable\n");
        testapp::LogFlush();
        return false;
    }
    ffxReturnCode_t result = g_App.ffx.functions.CreateContext(
        &g_App.ffx.upscaleContext, &g_App.ffx.upscaleCreate.header, nullptr);
    testapp::Log(
        "[FG-DIAG] ffxCreateContext(upscale Vulkan) result=%u(%s) context=%p render=%ux%u "
        "display=%ux%u\n",
        result, FfxResultName(result), g_App.ffx.upscaleContext, g_App.renderer.renderWidth,
        g_App.renderer.renderHeight, g_App.swapchain.extent.width, g_App.swapchain.extent.height);
    if (result != FFX_API_RETURN_OK || !g_App.ffx.upscaleContext) {
        g_App.ffx.upscaleContext = nullptr;
        return false;
    }

    g_App.ffx.frameGenerationHudless = {};
    g_App.ffx.frameGenerationHudless.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    g_App.ffx.frameGenerationHudless.hudlessBackBufferFormat =
        ffxApiGetSurfaceFormatVK(g_App.swapchain.format);
    g_App.ffx.frameGenerationBackend = {};
    g_App.ffx.frameGenerationBackend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    g_App.ffx.frameGenerationBackend.header.pNext = &g_App.ffx.frameGenerationHudless.header;
    g_App.ffx.frameGenerationBackend.vkDevice = g_App.vk.device;
    g_App.ffx.frameGenerationBackend.vkPhysicalDevice = g_App.vk.physicalDevice;
    g_App.ffx.frameGenerationBackend.vkDeviceProcAddr = FidelityFxGetDeviceProcAddr;
    g_App.ffx.frameGenerationCreate = {};
    g_App.ffx.frameGenerationCreate.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    g_App.ffx.frameGenerationCreate.header.pNext = &g_App.ffx.frameGenerationBackend.header;
    g_App.ffx.frameGenerationCreate.flags = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
    g_App.ffx.frameGenerationCreate.displaySize = {g_App.swapchain.extent.width, g_App.swapchain.extent.height};
    g_App.ffx.frameGenerationCreate.maxRenderSize = {g_App.renderer.renderWidth, g_App.renderer.renderHeight};
    g_App.ffx.frameGenerationCreate.backBufferFormat = ffxApiGetSurfaceFormatVK(g_App.swapchain.format);
    testapp::Log(
        "[FG-DIAG] FidelityFX HUDless format preflight swapchainVk=%d backBufferFfx=%u "
        "hudlessFfx=%u match=%d source=8-bit-dithered-presentation\n",
        static_cast<int>(g_App.swapchain.format), g_App.ffx.frameGenerationCreate.backBufferFormat,
        g_App.ffx.frameGenerationHudless.hudlessBackBufferFormat,
        g_App.ffx.frameGenerationCreate.backBufferFormat ==
                g_App.ffx.frameGenerationHudless.hudlessBackBufferFormat
            ? 1
            : 0);
    if (g_App.ffx.frameGenerationCreate.backBufferFormat !=
        g_App.ffx.frameGenerationHudless.hudlessBackBufferFormat) {
        testapp::Log(
            "[FG-DIAG] ERROR FidelityFX 1.1.4 requires matching swapchain and HUDless "
            "precision groups; refusing an invalid frame-generation context\n");
        g_App.ffx.functions.DestroyContext(&g_App.ffx.upscaleContext, nullptr);
        g_App.ffx.upscaleContext = nullptr;
        testapp::LogFlush();
        return false;
    }
    result = g_App.ffx.functions.CreateContext(
        &g_App.ffx.frameGenerationContext, &g_App.ffx.frameGenerationCreate.header, nullptr);
    testapp::Log(
        "[FG-DIAG] ffxCreateContext(frame-generation Vulkan) result=%u(%s) context=%p "
        "display=%ux%u render=%ux%u format=%u\n",
        result, FfxResultName(result), g_App.ffx.frameGenerationContext,
        g_App.ffx.frameGenerationCreate.displaySize.width,
        g_App.ffx.frameGenerationCreate.displaySize.height,
        g_App.ffx.frameGenerationCreate.maxRenderSize.width,
        g_App.ffx.frameGenerationCreate.maxRenderSize.height,
        g_App.ffx.frameGenerationCreate.backBufferFormat);
    if (result != FFX_API_RETURN_OK || !g_App.ffx.frameGenerationContext) {
        g_App.ffx.functions.DestroyContext(&g_App.ffx.upscaleContext, nullptr);
        g_App.ffx.upscaleContext = nullptr;
        return false;
    }
    g_App.ffx.upscaleSupported = true;
    g_App.ffx.frameGenerationSupported = g_App.vk.queuePlan.fidelityFxAvailable;
    LogFfxProvider("upscale", &g_App.ffx.upscaleContext);
    LogFfxProvider("frame-generation", &g_App.ffx.frameGenerationContext);
    return true;
}

}  // namespace

void StartFidelityFxRuntimePreload(const char* reason) {
    if (!g_App.config.asyncRuntimePreload || g_App.ffx.runtimeLoaded ||
        g_App.ffx.preloadInProgress.load() || g_App.ffx.preloadStarted.exchange(true)) {
        return;
    }
    const std::wstring runtimePath = FidelityFxRuntimePath();
    const std::string reasonText = reason ? reason : "unknown";
    g_App.ffx.preloadInProgress = true;
    g_App.ffx.preloadSucceeded = false;
    testapp::Log("[FG-DIAG] Async FidelityFX Vulkan runtime preload scheduled reason=%s path='%S'\n",
                 reasonText.c_str(), runtimePath.c_str());
    g_App.ffx.preloadThread = std::thread([runtimePath, reasonText]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        HMODULE module = LoadLibraryW(runtimePath.c_str());
        ffxFunctions functions{};
        if (module) {
            ffxLoadFunctions(&functions, module);
            if (!MandatoryFidelityFxFunctionsAvailable(functions)) {
                FreeLibrary(module);
                module = nullptr;
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_App.ffx.preloadMutex);
            g_App.ffx.preloadedModule = module;
        }
        g_App.ffx.preloadSucceeded = module != nullptr;
        g_App.ffx.preloadInProgress = false;
        testapp::Log(
            "[FG-DIAG] Async FidelityFX Vulkan runtime preload finished reason=%s ok=%d module=%p\n",
            reasonText.c_str(), module ? 1 : 0, module);
        testapp::LogFlush();
    });
}

void JoinFidelityFxRuntimePreload(const char* reason) {
    if (!g_App.ffx.preloadThread.joinable()) {
        return;
    }
    testapp::Log(
        "[FG-DIAG] Joining FidelityFX Vulkan runtime preload reason=%s inProgress=%d succeeded=%d\n",
        reason ? reason : "unknown", g_App.ffx.preloadInProgress.load() ? 1 : 0,
        g_App.ffx.preloadSucceeded.load() ? 1 : 0);
    g_App.ffx.preloadThread.join();
}

bool LoadFidelityFxRuntime(const char* reason) {
    if (g_App.ffx.runtimeLoaded) {
        return true;
    }
    const VulkanFsrVersionResolution version = ResolveVulkanFsrVersion(g_App.config.fsrUpscaleVersion);
    if (version.mlFallback) {
        testapp::Log(
            "[FG-DIAG] WARN fsr_version=4 requests ML SR/FG, which is unsupported by Vulkan; "
            "falling back to non-ML FSR 3.1.4 from FidelityFX SDK 1.1.4\n");
    } else if (version.invalidRequest) {
        testapp::Log("[FG-DIAG] WARN invalid Vulkan fsr_version=%d; using FSR 3.1.4\n",
                     g_App.config.fsrUpscaleVersion);
    }
    const std::wstring runtimePath = FidelityFxRuntimePath();
    JoinFidelityFxRuntimePreload("adopt preloaded FidelityFX Vulkan runtime");
    {
        std::lock_guard<std::mutex> lock(g_App.ffx.preloadMutex);
        g_App.ffx.module = g_App.ffx.preloadedModule;
        g_App.ffx.preloadedModule = nullptr;
    }
    if (!g_App.ffx.module) {
        g_App.ffx.module = LoadLibraryW(runtimePath.c_str());
    }
    if (!g_App.ffx.module) {
        testapp::Log("[FG-DIAG] FidelityFX Vulkan runtime load failed path='%S' reason=%s error=%lu\n",
                     runtimePath.c_str(), reason ? reason : "unknown", GetLastError());
        return false;
    }
    ffxLoadFunctions(&g_App.ffx.functions, g_App.ffx.module);
    if (!MandatoryFidelityFxFunctionsAvailable(g_App.ffx.functions)) {
        testapp::Log("[FG-DIAG] FidelityFX Vulkan runtime is missing mandatory ffx API exports\n");
        FreeLibrary(g_App.ffx.module);
        g_App.ffx.module = nullptr;
        g_App.ffx.functions = {};
        return false;
    }
    g_App.ffx.runtimeLoaded = true;
    testapp::Log(
        "[FG-DIAG] FidelityFX Vulkan runtime loaded path='%S' module=%p sdk=1.1.4 fsr=3.1.4 "
        "reason=%s exports(create=%p configure=%p query=%p dispatch=%p destroy=%p)\n",
        runtimePath.c_str(), g_App.ffx.module, reason ? reason : "unknown",
        reinterpret_cast<void*>(g_App.ffx.functions.CreateContext),
        reinterpret_cast<void*>(g_App.ffx.functions.Configure),
        reinterpret_cast<void*>(g_App.ffx.functions.Query),
        reinterpret_cast<void*>(g_App.ffx.functions.Dispatch),
        reinterpret_cast<void*>(g_App.ffx.functions.DestroyContext));
    testapp::LogFlush();
    return true;
}

bool PrepareFidelityFxMode() {
    if (!LoadFidelityFxRuntime("prepare FSR mode") || !g_App.renderer.initialized) {
        return false;
    }
    if (!CreateFidelityFxEffectContexts()) {
        return false;
    }
    if (!g_App.vk.queuePlan.fidelityFxAvailable) {
        testapp::Log(
            "[FG-DIAG] FSR SR supported but FSR FG disabled: four distinct Vulkan queue roles "
            "could not be provisioned\n");
        g_App.ffx.frameGenerationSupported = false;
        return false;
    }
    return true;
}

bool RecreateFidelityFxEffectsForExtent() {
    if (!g_App.ffx.runtimeLoaded) {
        return false;
    }
    ReleaseFidelityFxEffectsForExtent("recreate FidelityFX effects for extent");
    return CreateFidelityFxEffectContexts();
}

void ReleaseFidelityFxEffectsForExtent(const char* reason) {
    if (g_App.ffx.frameGenerationContext) {
        if (g_App.ffx.frameGenerationConfigured) {
            SetFsrFrameGeneration(false, reason, true);
        }
        g_App.ffx.functions.DestroyContext(&g_App.ffx.frameGenerationContext, nullptr);
        g_App.ffx.frameGenerationContext = nullptr;
    }
    if (g_App.ffx.upscaleContext) {
        g_App.ffx.functions.DestroyContext(&g_App.ffx.upscaleContext, nullptr);
        g_App.ffx.upscaleContext = nullptr;
    }
    g_App.ffx.upscaleSupported = false;
    g_App.ffx.frameGenerationSupported = false;
    g_App.ffx.frameGenerationConfigured = false;
    testapp::Log(
        "[FG-DIAG] FidelityFX extent-bound effects released reason=%s; renderer images may now be retired\n",
        reason ? reason : "unknown");
}

bool CreateFidelityFxSwapchain(VkSwapchainKHR oldSwapchain,
                               const VkSwapchainCreateInfoKHR& createInfo,
                               VkSwapchainKHR* replacement, bool* oldSwapchainConsumed) {
    if (oldSwapchainConsumed) {
        *oldSwapchainConsumed = false;
    }
    if (!replacement || !oldSwapchainConsumed || !PrepareFidelityFxMode()) {
        return false;
    }
    g_App.ffx.swapchainMode = {};
    g_App.ffx.swapchainMode.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_MODE_VK;
    g_App.ffx.swapchainMode.composeOnPresentQueue = false;
    g_App.ffx.swapchainHandleStorage = oldSwapchain;
    g_App.ffx.swapchainCreate = {};
    g_App.ffx.swapchainCreate.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    g_App.ffx.swapchainCreate.header.pNext = &g_App.ffx.swapchainMode.header;
    g_App.ffx.swapchainCreate.physicalDevice = g_App.vk.physicalDevice;
    g_App.ffx.swapchainCreate.device = g_App.vk.device;
    g_App.ffx.swapchainCreate.swapchain = &g_App.ffx.swapchainHandleStorage;
    g_App.ffx.swapchainCreate.createInfo = createInfo;
    g_App.ffx.swapchainCreate.createInfo.oldSwapchain = oldSwapchain;
    auto queueInfo = [](VkQueue queue, const VulkanQueueRef& ref) {
        VkQueueInfoFFXAPI result{};
        result.queue = queue;
        result.familyIndex = ref.familyIndex;
        return result;
    };
    g_App.ffx.swapchainCreate.gameQueue =
        queueInfo(ApplicationPresentQueue(), ApplicationPresentQueueRef());
    g_App.ffx.swapchainCreate.asyncComputeQueue =
        queueInfo(g_App.vk.ffxAsyncQueue, g_App.vk.queuePlan.ffxAsyncCompute);
    g_App.ffx.swapchainCreate.presentQueue =
        queueInfo(g_App.vk.ffxPresentQueue, g_App.vk.queuePlan.ffxPresent);
    g_App.ffx.swapchainCreate.imageAcquireQueue =
        queueInfo(g_App.vk.ffxAcquireQueue, g_App.vk.queuePlan.ffxImageAcquire);
    testapp::Log(
        "[FG-DIAG] FidelityFX game/present-call queue family=%u index=%u separate=%d; "
        "provider queues async=%u:%u present=%u:%u acquire=%u:%u\n",
        g_App.ffx.swapchainCreate.gameQueue.familyIndex, ApplicationPresentQueueRef().queueIndex,
        g_App.vk.asyncPresentActive ? 1 : 0,
        g_App.ffx.swapchainCreate.asyncComputeQueue.familyIndex,
        g_App.vk.queuePlan.ffxAsyncCompute.queueIndex,
        g_App.ffx.swapchainCreate.presentQueue.familyIndex,
        g_App.vk.queuePlan.ffxPresent.queueIndex,
        g_App.ffx.swapchainCreate.imageAcquireQueue.familyIndex,
        g_App.vk.queuePlan.ffxImageAcquire.queueIndex);
    const ffxReturnCode_t result = g_App.ffx.functions.CreateContext(
        &g_App.ffx.swapchainContext, &g_App.ffx.swapchainCreate.header, nullptr);
    // The pinned Vulkan provider retires/destroys a non-null incoming swapchain even when its
    // replacement initialization fails. Report consumption as soon as CreateContext has been
    // called so rollback never double-destroys that bridge through an implicit Vulkan layer.
    *oldSwapchainConsumed = oldSwapchain != VK_NULL_HANDLE;
    *replacement = g_App.ffx.swapchainHandleStorage;
    // Per ffx_api_vk.h, a successfully created FG swapchain context destroys the incoming
    // swapchain and replaces the pointed-to handle. Surface-view rollback must account for this
    // even if a later function-table query fails.
    testapp::Log(
        "[FG-TRANSITION] ffxCreateContext(FG swapchain Vulkan) result=%u(%s) context=%p "
        "old=%p new=%p queues(game=%u:%u async=%u:%u present=%u:%u acquire=%u:%u)\n",
        result, FfxResultName(result), g_App.ffx.swapchainContext,
        reinterpret_cast<void*>(oldSwapchain), reinterpret_cast<void*>(*replacement),
        g_App.vk.queuePlan.game.familyIndex, g_App.vk.queuePlan.game.queueIndex,
        g_App.vk.queuePlan.ffxAsyncCompute.familyIndex, g_App.vk.queuePlan.ffxAsyncCompute.queueIndex,
        g_App.vk.queuePlan.ffxPresent.familyIndex, g_App.vk.queuePlan.ffxPresent.queueIndex,
        g_App.vk.queuePlan.ffxImageAcquire.familyIndex, g_App.vk.queuePlan.ffxImageAcquire.queueIndex);
    if (result != FFX_API_RETURN_OK || !g_App.ffx.swapchainContext || !*replacement) {
        g_App.ffx.swapchainContext = nullptr;
        return false;
    }
    g_App.ffx.replacement = {};
    g_App.ffx.replacement.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    const ffxReturnCode_t queryResult = g_App.ffx.functions.Query(
        &g_App.ffx.swapchainContext, &g_App.ffx.replacement.header);
    g_App.ffx.swapchainReady = queryResult == FFX_API_RETURN_OK &&
        g_App.ffx.replacement.pOutCreateSwapchainFFXAPI &&
        g_App.ffx.replacement.pOutDestroySwapchainFFXAPI &&
        g_App.ffx.replacement.pOutGetSwapchainImagesKHR &&
        g_App.ffx.replacement.pOutAcquireNextImageKHR &&
        g_App.ffx.replacement.pOutQueuePresentKHR;
    testapp::Log(
        "[FG-DIAG] ffxQuery(FG swapchain functions) result=%u(%s) ready=%d "
        "create=%p destroy=%p images=%p acquire=%p present=%p count=%p\n",
        queryResult, FfxResultName(queryResult), g_App.ffx.swapchainReady ? 1 : 0,
        reinterpret_cast<void*>(g_App.ffx.replacement.pOutCreateSwapchainFFXAPI),
        reinterpret_cast<void*>(g_App.ffx.replacement.pOutDestroySwapchainFFXAPI),
        reinterpret_cast<void*>(g_App.ffx.replacement.pOutGetSwapchainImagesKHR),
        reinterpret_cast<void*>(g_App.ffx.replacement.pOutAcquireNextImageKHR),
        reinterpret_cast<void*>(g_App.ffx.replacement.pOutQueuePresentKHR),
        reinterpret_cast<void*>(g_App.ffx.replacement.pOutGetLastPresentCountFFXAPI));
    if (!g_App.ffx.swapchainReady) {
        g_App.ffx.functions.DestroyContext(&g_App.ffx.swapchainContext, nullptr);
        g_App.ffx.swapchainContext = nullptr;
        *replacement = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool SetFsrFrameGeneration(bool enabled, const char* reason, bool forceLog) {
    if (!g_App.ffx.functions.Configure || !g_App.ffx.frameGenerationContext) {
        return !enabled;
    }
    const bool usePresentCallback = ShouldUsePresentCallback(enabled);
    const bool routeChanged = usePresentCallback != g_App.ffx.lastPresentCallback;
    FrameResources& resources = g_App.renderer.resources[g_App.frameSlot % kFramesInFlight];
    ffxConfigureDescFrameGeneration configure{};
    configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configure.swapChain = NativeHandleToVoid(g_App.swapchain.handle);
    configure.presentCallback = usePresentCallback ? FidelityFxPresentCallback : nullptr;
    configure.frameGenerationCallback = FidelityFxFrameGenerationCallback;
    configure.frameGenerationCallbackUserContext = &g_App.ffx.frameGenerationContext;
    configure.frameGenerationEnabled = enabled;
    configure.allowAsyncWorkloads = true;
    if (resources.presentationColor.image) {
        configure.HUDLessColor = MakeFfxResource(
            resources.presentationColor, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    }
    configure.onlyPresentGenerated = false;
    configure.generationRect = {0, 0, static_cast<int32_t>(g_App.swapchain.extent.width),
                                static_cast<int32_t>(g_App.swapchain.extent.height)};
    configure.frameID = g_App.frameId;
    const ffxReturnCode_t result = g_App.ffx.functions.Configure(
        &g_App.ffx.frameGenerationContext, &configure.header);
    if (result == FFX_API_RETURN_OK) {
        g_App.ffx.frameGenerationConfigured = enabled;
        g_App.ffx.lastPresentCallback = usePresentCallback;
    }
    if (forceLog || routeChanged || result != FFX_API_RETURN_OK || (g_App.frameId % 120) == 0) {
        testapp::Log(
            "[FG-DIAG] ffxConfigure(frame-generation) requested=%d effective=%d reason=%s "
            "frameID=%llu swapchain=%p owner=%s callbackRoute=%s routeChanged=%d result=%u(%s)\n",
            enabled ? 1 : 0, g_App.ffx.frameGenerationConfigured ? 1 : 0,
            reason ? reason : "unknown", static_cast<unsigned long long>(g_App.frameId),
            reinterpret_cast<void*>(g_App.swapchain.handle), OwnerName(g_App.swapchain.owner),
            usePresentCallback ? "application" : "ffx-internal", routeChanged ? 1 : 0, result,
            FfxResultName(result));
        testapp::LogFlush();
    }
    return result == FFX_API_RETURN_OK;
}

bool WaitForFsrPresents(const char* reason) {
    if (!g_App.ffx.swapchainContext || !g_App.ffx.functions.Dispatch) {
        return true;
    }
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait{};
    wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
    const ffxReturnCode_t result = g_App.ffx.functions.Dispatch(
        &g_App.ffx.swapchainContext, &wait.header);
    testapp::Log("[FG-DIAG] ffxDispatch(wait-for-presents Vulkan) reason=%s result=%u(%s) context=%p\n",
                 reason ? reason : "unknown", result, FfxResultName(result),
                 g_App.ffx.swapchainContext);
    if (result != FFX_API_RETURN_OK) {
        testapp::LogFlush();
    }
    return result == FFX_API_RETURN_OK;
}

void RegisterFsrUiResource(FrameResources& resources) {
    if (!g_App.ffx.swapchainContext || !g_App.ffx.functions.Configure) {
        return;
    }
    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceVK ui{};
    ui.header.type = FFX_API_CONFIGURE_DESC_TYPE_FGSWAPCHAIN_REGISTERUIRESOURCE_VK;
    const bool degenerate = g_App.config.fsrDegenerateUiResource &&
                            resources.degenerateUiColor.image != VK_NULL_HANDLE;
    const ImageResource& registeredUi = degenerate ? resources.degenerateUiColor : resources.uiColor;
    ui.uiResource = MakeFfxResource(registeredUi,
                                    FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    ui.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
    const ffxReturnCode_t result = g_App.ffx.functions.Configure(&g_App.ffx.swapchainContext, &ui.header);
    if (result != FFX_API_RETURN_OK || g_App.frameId < 5 || (g_App.frameId % 120) == 0) {
        testapp::Log(
            "[FG-DIAG] ffxConfigure(register UI Vulkan) frameID=%llu ui=%p size=%ux%u "
            "state=0x%x flags=0x%x degenerate=%d result=%u(%s)\n",
            static_cast<unsigned long long>(g_App.frameId), ui.uiResource.resource,
            ui.uiResource.description.width, ui.uiResource.description.height, ui.uiResource.state,
            ui.flags, degenerate ? 1 : 0, result, FfxResultName(result));
    }
}

bool RecordFsrUpscaleAndPrepare(VkCommandBuffer commandBuffer, FrameResources& resources,
                               const JitterOffset& jitter) {
    if (!g_App.ffx.frameGenerationContext || !g_App.ffx.functions.Dispatch ||
        (g_App.config.upscalingEnabled && !g_App.ffx.upscaleContext)) {
        return false;
    }
    const float aspect = static_cast<float>(g_App.renderer.renderWidth) /
                         static_cast<float>(std::max(g_App.renderer.renderHeight, 1u));
    const SceneCameraPolicy sceneCamera = BuildSceneCameraPolicy(aspect);
    if (g_App.config.upscalingEnabled) {
        ffxDispatchDescUpscale upscale{};
        upscale.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        upscale.commandList = NativeHandleToVoid(commandBuffer);
        upscale.color = MakeFfxResource(resources.sceneColor, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        upscale.depth = MakeFfxResource(resources.depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ,
                                        FFX_API_RESOURCE_USAGE_DEPTHTARGET);
        upscale.motionVectors = MakeFfxResource(resources.motionVectors,
                                                FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        upscale.reactive = MakeFfxResource(resources.reactiveMask,
                                           FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        upscale.transparencyAndComposition = MakeFfxResource(
            resources.transparencyMask, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        upscale.output = MakeFfxResource(resources.hudlessColor, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        upscale.jitterOffset = {jitter.x, jitter.y};
        upscale.motionVectorScale = {static_cast<float>(g_App.renderer.renderWidth),
                                     static_cast<float>(g_App.renderer.renderHeight)};
        upscale.renderSize = {g_App.renderer.renderWidth, g_App.renderer.renderHeight};
        upscale.upscaleSize = {g_App.swapchain.extent.width, g_App.swapchain.extent.height};
        upscale.enableSharpening = g_App.config.fsrSharpeningEnabled;
        upscale.sharpness = static_cast<float>(g_App.config.fsrSharpnessPercent) / 100.0f;
        upscale.frameTimeDelta = g_App.frameDeltaMs;
        upscale.preExposure = 1.0f;
        upscale.reset = g_App.resetTemporalHistory;
        upscale.cameraNear = sceneCamera.nearPlane;
        upscale.cameraFar = sceneCamera.farPlane;
        upscale.cameraFovAngleVertical = sceneCamera.verticalFov;
        upscale.viewSpaceToMetersFactor = 1.0f;
        const ffxReturnCode_t upscaleResult = g_App.ffx.functions.Dispatch(
            &g_App.ffx.upscaleContext, &upscale.header);
        if (upscaleResult != FFX_API_RETURN_OK || g_App.frameId < 5 || (g_App.frameId % 120) == 0) {
            testapp::Log(
                "[FG-DIAG] ffxDispatch(upscale Vulkan) frameID=%llu result=%u(%s) reset=%d "
                "render=%ux%u display=%ux%u jitter=(%.3f,%.3f) sharpening=%d/%.2f\n",
                static_cast<unsigned long long>(g_App.frameId), upscaleResult,
                FfxResultName(upscaleResult), upscale.reset ? 1 : 0, upscale.renderSize.width,
                upscale.renderSize.height, upscale.upscaleSize.width, upscale.upscaleSize.height,
                jitter.x, jitter.y, upscale.enableSharpening ? 1 : 0, upscale.sharpness);
        }
        if (upscaleResult != FFX_API_RETURN_OK) {
            return false;
        }
    }

    if (g_App.ffx.frameGenerationConfigured && !g_App.transition.suspended) {
        ffxDispatchDescFrameGenerationPrepareCameraInfo camera{};
        camera.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
        for (uint32_t axis = 0; axis < 3; ++axis) {
            camera.cameraPosition[axis] = sceneCamera.position[axis];
            camera.cameraUp[axis] = sceneCamera.up[axis];
            camera.cameraRight[axis] = sceneCamera.right[axis];
            camera.cameraForward[axis] = sceneCamera.forward[axis];
        }
        ffxDispatchDescFrameGenerationPrepare prepare{};
        prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        prepare.header.pNext = &camera.header;
        prepare.frameID = g_App.frameId;
        prepare.commandList = NativeHandleToVoid(commandBuffer);
        prepare.renderSize = {g_App.renderer.renderWidth, g_App.renderer.renderHeight};
        prepare.jitterOffset = {jitter.x, jitter.y};
        prepare.motionVectorScale = {static_cast<float>(g_App.renderer.renderWidth),
                                     static_cast<float>(g_App.renderer.renderHeight)};
        prepare.frameTimeDelta = g_App.frameDeltaMs;
        prepare.unused_reset = g_App.resetTemporalHistory;
        prepare.cameraNear = sceneCamera.nearPlane;
        prepare.cameraFar = sceneCamera.farPlane;
        prepare.cameraFovAngleVertical = sceneCamera.verticalFov;
        prepare.viewSpaceToMetersFactor = 1.0f;
        prepare.depth = MakeFfxResource(resources.depth,
                                        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ,
                                        FFX_API_RESOURCE_USAGE_DEPTHTARGET);
        prepare.motionVectors = MakeFfxResource(resources.motionVectors,
                                                FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        const ffxReturnCode_t prepareResult = g_App.ffx.functions.Dispatch(
            &g_App.ffx.frameGenerationContext, &prepare.header);
        if (prepareResult != FFX_API_RETURN_OK || g_App.frameId < 5 || (g_App.frameId % 120) == 0) {
            testapp::Log(
                "[FG-DIAG] ffxDispatch(frame-generation prepare Vulkan) frameID=%llu "
                "result=%u(%s) depth=%p motion=%p deltaMs=%.3f\n",
                static_cast<unsigned long long>(g_App.frameId), prepareResult,
                FfxResultName(prepareResult), prepare.depth.resource, prepare.motionVectors.resource,
                prepare.frameTimeDelta);
        }
        if (prepareResult != FFX_API_RETURN_OK) {
            return false;
        }
    }
    RegisterFsrUiResource(resources);
    SetFsrFrameGeneration(g_App.ffx.frameGenerationConfigured, "per-frame configuration", false);
    return true;
}

void DestroyFidelityFxContexts(bool unloadRuntime, const char* reason,
                               bool presentationAlreadyRetired) {
    JoinFidelityFxRuntimePreload(reason);
    if (!presentationAlreadyRetired) {
        if (g_App.ffx.frameGenerationContext) {
            SetFsrFrameGeneration(false, reason, true);
        }
        WaitForFsrPresents(reason);
    } else {
        // The FFX replacement destroy function has already stopped the presenter and cleared the
        // swapchain stored in its context. Configuring or waiting through that empty proxy would
        // dereference it inside the signed provider, so teardown proceeds directly to contexts.
        g_App.ffx.frameGenerationConfigured = false;
        testapp::Log(
            "[FG-DIAG] FidelityFX presentation already retired; skipping configure/wait "
            "reason=%s\n",
            reason ? reason : "unknown");
    }
    testapp::Log(
        "[FG-DIAG] FidelityFX callback totals present=%llu frameGeneration=%llu reason=%s\n",
        static_cast<unsigned long long>(g_App.ffx.presentCallbackCount.load()),
        static_cast<unsigned long long>(g_App.ffx.frameGenerationCallbackCount.load()),
        reason ? reason : "unknown");
    ReleaseFidelityFxEffectsForExtent(reason);
    if (g_App.ffx.functions.DestroyContext && g_App.ffx.swapchainContext) {
        g_App.ffx.functions.DestroyContext(&g_App.ffx.swapchainContext, nullptr);
    }
    g_App.ffx.frameGenerationContext = nullptr;
    g_App.ffx.upscaleContext = nullptr;
    g_App.ffx.swapchainContext = nullptr;
    g_App.ffx.swapchainReady = false;
    g_App.ffx.upscaleSupported = false;
    g_App.ffx.frameGenerationSupported = false;
    g_App.ffx.frameGenerationConfigured = false;
    g_App.ffx.swapchainHandleStorage = VK_NULL_HANDLE;
    g_App.ffx.replacement = {};
    HMODULE preloadedModule = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_App.ffx.preloadMutex);
        preloadedModule = g_App.ffx.preloadedModule;
        g_App.ffx.preloadedModule = nullptr;
    }
    if (preloadedModule) {
        testapp::Log("[FG-DIAG] Releasing unused preloaded FidelityFX Vulkan module=%p reason=%s\n",
                     preloadedModule, reason ? reason : "unknown");
        FreeLibrary(preloadedModule);
    }
    if (unloadRuntime && g_App.ffx.module) {
        testapp::Log("[FG-DIAG] Unloading FidelityFX Vulkan runtime module=%p reason=%s\n",
                     g_App.ffx.module, reason ? reason : "unknown");
        FreeLibrary(g_App.ffx.module);
        g_App.ffx.module = nullptr;
        g_App.ffx.functions = {};
        g_App.ffx.runtimeLoaded = false;
    }
    testapp::LogFlush();
}

}  // namespace testapp::vkfg
