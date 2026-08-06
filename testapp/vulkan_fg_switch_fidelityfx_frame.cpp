#include "vulkan_fg_switch_test_internal.h"

namespace testapp::vkfg {

FfxApiResource MakeFfxResource(const ImageResource& image, uint32_t state,
                               uint32_t additionalUsage) {
    FfxApiResourceDescription description =
        ffxApiGetImageResourceDescriptionVK(image.image, image.createInfo, additionalUsage);
    return ffxApiGetResourceVK(NativeHandleToVoid(image.image), description, state);
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
