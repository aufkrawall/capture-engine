#include "vulkan_fg_switch_test_internal.h"

namespace testapp::vkfg {
bool InitializeRenderer() {
    const testapp::fg::RenderSize renderSize = testapp::fg::ComputeRenderSize(
        g_App.swapchain.extent.width, g_App.swapchain.extent.height, g_App.config.upscaleQuality,
        g_App.config.upscaleScalePercent);
    g_App.renderer.renderWidth = g_App.config.upscalingEnabled ? renderSize.width : g_App.swapchain.extent.width;
    g_App.renderer.renderHeight =
        g_App.config.upscalingEnabled ? renderSize.height : g_App.swapchain.extent.height;
    if (!CheckFormat(kSceneColorFormat,
                     VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                         VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
                     "scene/hudless RGBA16F") ||
        !CheckFormat(kMotionFormat, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
                     "motion RG16F") ||
        !CheckFormat(kDepthFormat,
                     VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
                     "depth D32") ||
        !CheckFormat(kMaskFormat, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
                     "mask R8") ||
        !CheckFormat(g_App.swapchain.format,
                     VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
                     "8-bit dithered presentation/FFX HUDless")) {
        return false;
    }
    g_App.renderer.sceneRenderPass = CreateSceneRenderPass();
    g_App.renderer.hdrRenderPass = CreateSingleColorRenderPass(kSceneColorFormat);
    g_App.renderer.uiRenderPass = CreateSingleColorRenderPass(kUiFormat);
    g_App.renderer.swapchainRenderPass = CreateSingleColorRenderPass(g_App.swapchain.format);
    if (!g_App.renderer.sceneRenderPass || !g_App.renderer.hdrRenderPass ||
        !g_App.renderer.uiRenderPass || !g_App.renderer.swapchainRenderPass) {
        return false;
    }
    g_App.renderer.taaSetLayout = CreateSampledSetLayout(3);
    g_App.renderer.composeSetLayout = CreateSampledSetLayout(2);
    g_App.renderer.presentSetLayout = CreateSampledSetLayout(1);
    g_App.renderer.scenePipelineLayout = CreatePipelineLayout(VK_NULL_HANDLE, 32, VK_SHADER_STAGE_FRAGMENT_BIT);
    g_App.renderer.taaPipelineLayout =
        CreatePipelineLayout(g_App.renderer.taaSetLayout, 16, VK_SHADER_STAGE_FRAGMENT_BIT);
    g_App.renderer.uiPipelineLayout = CreatePipelineLayout(VK_NULL_HANDLE, 40, VK_SHADER_STAGE_FRAGMENT_BIT);
    g_App.renderer.composePipelineLayout =
        CreatePipelineLayout(g_App.renderer.composeSetLayout, 0, VK_SHADER_STAGE_FRAGMENT_BIT);
    g_App.renderer.presentPipelineLayout =
        CreatePipelineLayout(g_App.renderer.presentSetLayout, 4, VK_SHADER_STAGE_FRAGMENT_BIT);
    g_App.renderer.scenePipeline =
        CreateGraphicsPipeline(g_App.renderer.sceneRenderPass, g_App.renderer.scenePipelineLayout,
                               shaders::kSceneFragmentSpirv, sizeof(shaders::kSceneFragmentSpirv), 4, true);
    g_App.renderer.taaPipeline =
        CreateGraphicsPipeline(g_App.renderer.hdrRenderPass, g_App.renderer.taaPipelineLayout,
                               shaders::kTaaFragmentSpirv, sizeof(shaders::kTaaFragmentSpirv), 1, false);
    g_App.renderer.uiPipeline =
        CreateGraphicsPipeline(g_App.renderer.uiRenderPass, g_App.renderer.uiPipelineLayout,
                               shaders::kUiFragmentSpirv, sizeof(shaders::kUiFragmentSpirv), 1, false);
    g_App.renderer.composePipeline =
        CreateGraphicsPipeline(g_App.renderer.swapchainRenderPass, g_App.renderer.composePipelineLayout,
                               shaders::kComposeFragmentSpirv, sizeof(shaders::kComposeFragmentSpirv), 1, false);
    g_App.renderer.presentPipeline =
        CreateGraphicsPipeline(g_App.renderer.swapchainRenderPass, g_App.renderer.presentPipelineLayout,
                               shaders::kPresentFragmentSpirv, sizeof(shaders::kPresentFragmentSpirv), 1, false);
    if (!g_App.renderer.scenePipeline || !g_App.renderer.taaPipeline || !g_App.renderer.uiPipeline ||
        !g_App.renderer.composePipeline || !g_App.renderer.presentPipeline) {
        return false;
    }
    VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(g_App.vk.device, &samplerInfo, nullptr, &g_App.renderer.linearSampler) != VK_SUCCESS) {
        return false;
    }
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        if (!CreateFrameResources(&g_App.renderer.resources[frame], frame) ||
            !CreateFramebuffers(&g_App.renderer.resources[frame])) {
            return false;
        }
    }
    if (!AllocateAndWriteDescriptors() || !CreateFrameContexts()) {
        return false;
    }
    if (!CreateSwapchainFramebuffers(&g_App.swapchain)) {
        return false;
    }
    g_App.renderer.initialized = true;
    testapp::Log(
        "[FG-DIAG] Renderer initialized framesInFlight=%u display=%ux%u render=%ux%u resources="
        "RGBA16F/RG16F/D32/R8/RGBA8 presentationFormat=%d\n",
        kFramesInFlight, g_App.swapchain.extent.width, g_App.swapchain.extent.height,
        g_App.renderer.renderWidth, g_App.renderer.renderHeight, static_cast<int>(g_App.swapchain.format));
    const SceneCameraPolicy camera = BuildSceneCameraPolicy(
        static_cast<float>(g_App.renderer.renderWidth) /
        static_cast<float>(std::max(g_App.renderer.renderHeight, 1u)));
    testapp::Log(
        "[FG-DIAG] Scene camera eye=(%.3f,%.3f,%.3f) right=(%.3f,%.3f,%.3f) "
        "up=(%.3f,%.3f,%.3f) forward=(%.3f,%.3f,%.3f) near=%.3f far=%.1f fovY=%.6f "
        "motion=UV(prev-current) SLscale=(2,-2) FFXscale=%ux%u\n",
        camera.position[0], camera.position[1], camera.position[2], camera.right[0],
        camera.right[1], camera.right[2], camera.up[0], camera.up[1], camera.up[2],
        camera.forward[0], camera.forward[1], camera.forward[2], camera.nearPlane,
        camera.farPlane, camera.verticalFov, g_App.renderer.renderWidth,
        g_App.renderer.renderHeight);
    return true;
}
}

namespace testapp::vkfg {
void ShutdownRenderer() {
    if (g_App.vk.device == VK_NULL_HANDLE) {
        return;
    }
    DestroyFrameContexts();
    for (FrameResources& resources : g_App.renderer.resources) {
        DestroyFrameResources(&resources);
    }
    if (g_App.renderer.descriptorPool)
        vkDestroyDescriptorPool(g_App.vk.device, g_App.renderer.descriptorPool, nullptr);
    if (g_App.renderer.linearSampler) vkDestroySampler(g_App.vk.device, g_App.renderer.linearSampler, nullptr);
    const VkPipeline pipelines[] = {g_App.renderer.scenePipeline, g_App.renderer.taaPipeline,
                                    g_App.renderer.uiPipeline, g_App.renderer.composePipeline,
                                    g_App.renderer.presentPipeline};
    for (VkPipeline pipeline : pipelines) {
        if (pipeline) vkDestroyPipeline(g_App.vk.device, pipeline, nullptr);
    }
    const VkPipelineLayout pipelineLayouts[] = {
        g_App.renderer.scenePipelineLayout, g_App.renderer.taaPipelineLayout,
        g_App.renderer.uiPipelineLayout, g_App.renderer.composePipelineLayout,
        g_App.renderer.presentPipelineLayout};
    for (VkPipelineLayout layout : pipelineLayouts) {
        if (layout) vkDestroyPipelineLayout(g_App.vk.device, layout, nullptr);
    }
    const VkDescriptorSetLayout descriptorLayouts[] = {
        g_App.renderer.taaSetLayout, g_App.renderer.composeSetLayout, g_App.renderer.presentSetLayout};
    for (VkDescriptorSetLayout layout : descriptorLayouts) {
        if (layout) vkDestroyDescriptorSetLayout(g_App.vk.device, layout, nullptr);
    }
    const VkRenderPass renderPasses[] = {g_App.renderer.sceneRenderPass, g_App.renderer.hdrRenderPass,
                                         g_App.renderer.uiRenderPass, g_App.renderer.swapchainRenderPass};
    for (VkRenderPass renderPass : renderPasses) {
        if (renderPass) vkDestroyRenderPass(g_App.vk.device, renderPass, nullptr);
    }
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    g_App.renderer = {};
}
}

namespace testapp::vkfg {
bool RecreateRendererForExtent() {
    for (VkFramebuffer framebuffer : g_App.swapchain.framebuffers) {
        if (framebuffer) vkDestroyFramebuffer(g_App.vk.device, framebuffer, nullptr);
    }
    g_App.swapchain.framebuffers.clear();
    ShutdownRenderer();
    return InitializeRenderer();
}
}

namespace testapp::vkfg {
namespace {
bool ModeCanBeRequested(FgMode mode, const char** unavailableReason) {
    if (mode == FgMode::Dlss) {
        if (!g_App.sl.initialized || !g_App.sl.vulkanInfoSet) {
            *unavailableReason = "Streamline Vulkan integration unavailable";
            return false;
        }
        if (!g_App.sl.dlssFgSupported) {
            *unavailableReason = "DLSS Frame Generation unsupported on selected adapter";
            return false;
        }
    } else if (mode == FgMode::Fsr) {
        if (!g_App.vk.queuePlan.fidelityFxAvailable) {
            *unavailableReason = "FidelityFX distinct Vulkan queue topology unavailable";
            return false;
        }
    }
    return true;
}
}
}

namespace testapp::vkfg {
namespace {
bool PrepareTarget(FgMode target) {
    switch (target) {
        case FgMode::Dlss:
            return PrepareStreamlineMode();
        case FgMode::Fsr:
            return PrepareFidelityFxMode();
        case FgMode::Off:
        default:
            return true;
    }
}
}
}

namespace testapp::vkfg {
namespace {
void RollbackTargetPreparation() {
    if (g_App.transition.targetMode == FgMode::Fsr &&
        g_App.swapchain.owner != SwapchainOwner::FidelityFX) {
        DestroyFidelityFxContexts(g_App.config.fsrReloadRuntimeOnSwitch,
                                  "rollback prepared FSR transition");
    }
    if (g_App.transition.targetMode == FgMode::Dlss &&
        g_App.swapchain.owner == SwapchainOwner::FidelityFX) {
        RetireStreamlinePresentation(SwapchainOwner::FidelityFX,
                                     "rollback prepared DLSS transition");
    }
}
}
}

namespace testapp::vkfg {
namespace {
void MaybeStartQueuedRequest() {
    if (g_App.transition.stage == TransitionStage::Idle &&
        g_App.requestedMode != g_App.transition.currentMode) {
        const FgMode queued = g_App.requestedMode;
        RequestMode(queued, "queued request after transition");
    }
}
}
}

namespace testapp::vkfg {
bool SetModeFeatureState(FgMode mode, bool enabled, const char* reason) {
    bool result = true;
    switch (mode) {
        case FgMode::Dlss:
            result = SetDlssFrameGeneration(enabled, reason);
            break;
        case FgMode::Fsr:
            result = SetFsrFrameGeneration(enabled, reason, true);
            break;
        case FgMode::Off:
        default:
            result = true;
            break;
    }
    testapp::Log(
        "[FG-DIAG] Feature state mode=%s requested=%d configured(dlss=%d,fsr=%d) "
        "reflex=%d suspended=%d reason=%s result=%d\n",
        ModeName(mode), enabled ? 1 : 0, g_App.sl.dlssFgConfigured ? 1 : 0,
        g_App.ffx.frameGenerationConfigured ? 1 : 0, g_App.sl.reflexActive ? 1 : 0,
        g_App.transition.suspended ? 1 : 0, reason ? reason : "unknown", result ? 1 : 0);
    testapp::LogFlush();
    return result;
}
}

namespace testapp::vkfg {
bool RequestMode(FgMode mode, const char* reason) {
    g_App.requestedMode = mode;
    if (g_App.transition.stage == TransitionStage::DeviceLost) {
        return false;
    }
    const char* unavailableReason = nullptr;
    if (!ModeCanBeRequested(mode, &unavailableReason)) {
        testapp::Log(
            "[FG-DIAG] Mode request rejected requested=%s current=%s reason=%s "
            "support(dlssSR=%d,dlssFG=%d,reflex=%d,fsrSR=%d,fsrFGQueues=%d)\n",
            ModeName(mode), ModeName(g_App.transition.currentMode),
            unavailableReason ? unavailableReason : "unknown", g_App.sl.dlssSrSupported ? 1 : 0,
            g_App.sl.dlssFgSupported ? 1 : 0, g_App.sl.reflexSupported ? 1 : 0,
            g_App.ffx.upscaleSupported ? 1 : 0,
            g_App.vk.queuePlan.fidelityFxAvailable ? 1 : 0);
        testapp::LogFlush();
        return false;
    }

    if (g_App.transition.stage != TransitionStage::Idle) {
        if (mode == g_App.transition.currentMode &&
            CancelModeTransitionBeforeReplacement(&g_App.transition)) {
            const bool restored = mode == FgMode::Off ||
                SetModeFeatureState(mode, true, "cancel transition and restore old owner");
            testapp::Log(
                "[FG-TRANSITION] cancellation reason=%s restored=%d current=%s owner=%s\n",
                reason ? reason : "unknown", restored ? 1 : 0, ModeName(mode),
                OwnerName(g_App.swapchain.owner));
            LogTransition("cancel-before-replacement");
            return restored;
        }
        testapp::Log(
            "[FG-TRANSITION] queued request=%s reason=%s activeEpoch=%llu stage=%s\n",
            ModeName(mode), reason ? reason : "unknown",
            static_cast<unsigned long long>(g_App.transition.epoch),
            TransitionStageName(g_App.transition.stage));
        testapp::LogFlush();
        return true;
    }

    if (mode == g_App.transition.currentMode) {
        if (mode == FgMode::Off) {
            return true;
        }
        const bool suspend = !g_App.transition.suspended;
        if (!SetModeFeatureState(mode, !suspend, suspend ? "suspend repeated-key mode"
                                                       : "resume repeated-key mode")) {
            return false;
        }
        g_App.transition.suspended = suspend;
        testapp::Log(
            "[FG-TRANSITION] %s mode=%s owner=%s proxyRetained=1 contextsRetained=1 "
            "reflex=%d frameID=%llu reason=%s\n",
            suspend ? "suspend" : "resume", ModeName(mode), OwnerName(g_App.swapchain.owner),
            g_App.sl.reflexActive ? 1 : 0, static_cast<unsigned long long>(g_App.frameId),
            reason ? reason : "unknown");
        LogTransition(suspend ? "suspended" : "resumed");
        return true;
    }

    if (!BeginModeTransition(&g_App.transition, mode)) {
        return false;
    }
    bool disabled = true;
    if (g_App.transition.currentMode != FgMode::Off) {
        disabled = SetModeFeatureState(g_App.transition.currentMode, false,
                                       "begin owner transition");
    }
    if (!disabled) {
        g_App.transition.stage = TransitionStage::Idle;
        g_App.transition.targetMode = g_App.transition.currentMode;
        g_App.transition.targetOwner = g_App.transition.owner;
        ++g_App.transitionFailures;
        LogTransition("old-feature-disable-failed");
        return false;
    }
    MarkOldFgDisabled(&g_App.transition);
    testapp::Log(
        "[FG-TRANSITION] request reason=%s epoch=%llu from=%s/%s to=%s/%s; waiting "
        "for one FG-off present on old surface\n",
        reason ? reason : "unknown", static_cast<unsigned long long>(g_App.transition.epoch),
        ModeName(g_App.transition.currentMode), OwnerName(g_App.transition.oldOwner), ModeName(mode),
        OwnerName(g_App.transition.targetOwner));
    LogTransition("requested");
    return true;
}
}

namespace testapp::vkfg {
void DriveTransitionBeforeFrame() {
    if (g_App.transition.stage == TransitionStage::DeviceLost) {
        return;
    }
    if (g_App.transition.stage == TransitionStage::PreparingReplacement) {
        const bool prepared = PrepareTarget(g_App.transition.targetMode);
        MarkReplacementPrepared(&g_App.transition, prepared);
        LogTransition(prepared ? "replacement-prepared" : "replacement-prepare-failed");
        if (!prepared) {
            ++g_App.transitionFailures;
        }
    }
    if (g_App.transition.stage == TransitionStage::ReplacingSwapchain) {
        bool replaced = DrainSwapchainBoundWork("transactional owner replacement");
        if (replaced) {
            replaced = CreateOrReplaceSwapchain(g_App.transition.targetOwner,
                                                "transactional owner replacement");
        }
        MarkReplacementCreated(&g_App.transition, replaced);
        LogTransition(replaced ? "replacement-committed-fg-off"
                               : "replacement-create-failed");
        if (!replaced) {
            ++g_App.transitionFailures;
        }
    }
    if (g_App.transition.stage == TransitionStage::Rollback) {
        const FgMode failedTarget = g_App.transition.targetMode;
        RollbackTargetPreparation();
        const FgMode oldMode = g_App.transition.currentMode;
        const bool rolledBack = RollbackPreparedTransition(&g_App.transition);
        bool restored = rolledBack;
        if (rolledBack && oldMode != FgMode::Off) {
            restored = SetModeFeatureState(oldMode, true, "rollback restore old FG owner");
        }
        testapp::Log("[FG-TRANSITION] rollback completed=%d restoredOldFeature=%d owner=%s\n",
                     rolledBack ? 1 : 0, restored ? 1 : 0, OwnerName(g_App.swapchain.owner));
        g_App.requestedMode = ResolveRequestedModeAfterTransitionFailure(
            g_App.requestedMode, failedTarget, g_App.transition.currentMode);
        LogTransition("rollback-complete");
        MaybeStartQueuedRequest();
    }
    if (g_App.transition.stage == TransitionStage::Activating) {
        const FgMode target = g_App.transition.targetMode;
        const bool activated = target == FgMode::Off ||
            SetModeFeatureState(target, true, "activate after replacement FG-off present");
        MarkTargetActivated(&g_App.transition, activated);
        if (!activated) {
            ++g_App.transitionFailures;
            g_App.requestedMode = ResolveRequestedModeAfterTransitionFailure(
                g_App.requestedMode, target, g_App.transition.currentMode);
        }
        LogTransition(activated ? "target-activated" : "target-activation-failed-kept-fg-off");
        MaybeStartQueuedRequest();
    }
}
}

