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

namespace testapp::vkfg {
void OnFramePresentedSuccessfully() {
    if (g_App.transition.stage == TransitionStage::OldPassthroughPending) {
        if (MarkOldPassthroughPresented(&g_App.transition, true)) {
            LogTransition("old-surface-passthrough-presented");
        }
    } else if (g_App.transition.stage == TransitionStage::ReplacementPresentPending) {
        if (MarkReplacementPresented(&g_App.transition, true)) {
            LogTransition("replacement-surface-fg-off-presented");
        }
    }
}
}

namespace testapp::vkfg {
namespace {
CpuTimingAccumulator g_CpuTimings;
}
}

namespace testapp::vkfg {
namespace {
double MillisecondsBetween(std::chrono::steady_clock::time_point begin,
                           std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}
}
}

namespace testapp::vkfg {
namespace {
LayoutAccess AccessForLayout(VkImageLayout layout, bool depth) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT};
        case VK_IMAGE_LAYOUT_GENERAL:
            return {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            return {VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_MEMORY_READ_BIT};
        case VK_IMAGE_LAYOUT_UNDEFINED:
        default:
            return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
    }
}
}
}

namespace testapp::vkfg {
namespace {
void TransitionImage(VkCommandBuffer commandBuffer, VkImage image, VkImageAspectFlags aspect,
                     VkImageLayout* trackedLayout, VkImageLayout targetLayout) {
    if (!trackedLayout || *trackedLayout == targetLayout) {
        return;
    }
    const LayoutAccess source = AccessForLayout(*trackedLayout, (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0);
    const LayoutAccess destination = AccessForLayout(targetLayout, (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0);
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = source.access;
    barrier.dstAccessMask = destination.access;
    barrier.oldLayout = *trackedLayout;
    barrier.newLayout = targetLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, source.stage, destination.stage, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
    *trackedLayout = targetLayout;
}
}
}

namespace testapp::vkfg {
namespace {
void TransitionResource(VkCommandBuffer commandBuffer, ImageResource& resource,
                        VkImageLayout targetLayout) {
    TransitionImage(commandBuffer, resource.image, resource.aspect, &resource.layout, targetLayout);
}
}
}

namespace testapp::vkfg {
namespace {
void SetViewportAndScissor(VkCommandBuffer commandBuffer, uint32_t width, uint32_t height) {
    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}
}
}

namespace testapp::vkfg {
namespace {
void BeginRenderPass(VkCommandBuffer commandBuffer, VkRenderPass renderPass, VkFramebuffer framebuffer,
                     uint32_t width, uint32_t height, const VkClearValue* clears, uint32_t clearCount) {
    VkRenderPassBeginInfo beginInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    beginInfo.renderPass = renderPass;
    beginInfo.framebuffer = framebuffer;
    beginInfo.renderArea.extent = {width, height};
    beginInfo.clearValueCount = clearCount;
    beginInfo.pClearValues = clears;
    vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
    SetViewportAndScissor(commandBuffer, width, height);
}
}
}

namespace testapp::vkfg {
namespace {
JitterOffset CurrentJitter() {
    const int phaseCount = JitterPhaseCount(g_App.renderer.renderWidth, g_App.swapchain.extent.width);
    return ComputeJitter(g_App.frameId, phaseCount);
}
}
}

namespace testapp::vkfg {
namespace {
void RecordScene(VkCommandBuffer commandBuffer, FrameResources& resources,
                 const JitterOffset& jitter, float timeSeconds) {
    TransitionResource(commandBuffer, resources.sceneColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    TransitionResource(commandBuffer, resources.motionVectors, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    TransitionResource(commandBuffer, resources.reactiveMask, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    TransitionResource(commandBuffer, resources.transparencyMask, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    TransitionResource(commandBuffer, resources.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    const VkClearValue clears[] = {
        {{{0.0f, 0.0f, 0.0f, 1.0f}}}, {{{0.0f, 0.0f, 0.0f, 0.0f}}},
        {{{0.0f, 0.0f, 0.0f, 0.0f}}}, {{{0.0f, 0.0f, 0.0f, 0.0f}}},
        {.depthStencil = {1.0f, 0}},
    };
    BeginRenderPass(commandBuffer, g_App.renderer.sceneRenderPass, resources.sceneFramebuffer,
                    g_App.renderer.renderWidth, g_App.renderer.renderHeight, clears,
                    static_cast<uint32_t>(std::size(clears)));
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_App.renderer.scenePipeline);
    struct ScenePush {
        float timeSeconds;
        float previousTimeSeconds;
        float jitterX;
        float jitterY;
        float renderWidth;
        float renderHeight;
        int loadPasses;
        int frameMode;
    } push{timeSeconds, g_App.previousTimeSeconds, jitter.x, jitter.y,
           static_cast<float>(g_App.renderer.renderWidth),
           static_cast<float>(g_App.renderer.renderHeight), g_App.config.gpuLoadPasses,
           static_cast<int>(g_App.transition.currentMode)};
    static_assert(sizeof(ScenePush) == 32);
    vkCmdPushConstants(commandBuffer, g_App.renderer.scenePipelineLayout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);
    resources.sceneColor.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    resources.motionVectors.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    resources.reactiveMask.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    resources.transparencyMask.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    resources.depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
}
}
}

namespace testapp::vkfg {
namespace {
void RecordUi(VkCommandBuffer commandBuffer, FrameResources& resources, float timeSeconds) {
    TransitionResource(commandBuffer, resources.uiColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const VkClearValue clear = {{{0.0f, 0.0f, 0.0f, 0.0f}}};
    BeginRenderPass(commandBuffer, g_App.renderer.uiRenderPass, resources.uiFramebuffer,
                    g_App.swapchain.extent.width, g_App.swapchain.extent.height, &clear, 1);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_App.renderer.uiPipeline);
    struct UiPush {
        float timeSeconds;
        float fps;
        int mode;
        int suspended;
        int upscaleMode;
        int dlssSupported;
        int fsrSupported;
        int reflexActive;
        float displayWidth;
        float displayHeight;
    } push{timeSeconds, g_App.fps, static_cast<int>(g_App.transition.currentMode),
           g_App.transition.suspended ? 1 : 0,
           g_App.config.upscalingEnabled ? static_cast<int>(g_App.config.upscaleQuality) + 1 : 0,
           g_App.sl.dlssFgSupported ? 1 : 0, g_App.ffx.frameGenerationSupported ? 1 : 0,
           g_App.sl.reflexActive ? 1 : 0, static_cast<float>(g_App.swapchain.extent.width),
           static_cast<float>(g_App.swapchain.extent.height)};
    static_assert(sizeof(UiPush) == 40);
    vkCmdPushConstants(commandBuffer, g_App.renderer.uiPipelineLayout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);
    resources.uiColor.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    TransitionResource(commandBuffer, resources.uiColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (resources.degenerateUiColor.image != VK_NULL_HANDLE) {
        if (resources.degenerateUiColor.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            TransitionResource(commandBuffer, resources.degenerateUiColor,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const VkClearColorValue transparent = {{0.0f, 0.0f, 0.0f, 0.0f}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(commandBuffer, resources.degenerateUiColor.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &transparent, 1, &range);
        }
        TransitionResource(commandBuffer, resources.degenerateUiColor,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}
}
}

namespace testapp::vkfg {
namespace {
void InitializeHistory(VkCommandBuffer commandBuffer, FrameResources& resources) {
    if (resources.historyValid && !g_App.resetTemporalHistory) {
        return;
    }
    TransitionResource(commandBuffer, resources.historyColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue clear = {{0.0f, 0.0f, 0.0f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(commandBuffer, resources.historyColor.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    TransitionResource(commandBuffer, resources.historyColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    resources.historyValid = true;
}
}
}

namespace testapp::vkfg {
namespace {
void RecordTaa(VkCommandBuffer commandBuffer, FrameResources& resources) {
    InitializeHistory(commandBuffer, resources);
    TransitionResource(commandBuffer, resources.hudlessColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const VkClearValue clear = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    BeginRenderPass(commandBuffer, g_App.renderer.hdrRenderPass, resources.hudlessFramebuffer,
                    g_App.swapchain.extent.width, g_App.swapchain.extent.height, &clear, 1);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_App.renderer.taaPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g_App.renderer.taaPipelineLayout, 0, 1, &resources.taaSet, 0, nullptr);
    struct TaaPush {
        float historyWeight;
        float resetHistory;
        float renderWidth;
        float renderHeight;
    } push{0.88f, g_App.resetTemporalHistory ? 1.0f : 0.0f,
           static_cast<float>(g_App.renderer.renderWidth),
           static_cast<float>(g_App.renderer.renderHeight)};
    vkCmdPushConstants(commandBuffer, g_App.renderer.taaPipelineLayout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);
    resources.hudlessColor.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    TransitionResource(commandBuffer, resources.hudlessColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
}
}

namespace testapp::vkfg {
namespace {
bool RecordUpscaling(VkCommandBuffer commandBuffer, FrameResources& resources,
                     sl::FrameToken* frameToken, const JitterOffset& jitter,
                     uint32_t backbufferIndex) {
    TransitionResource(commandBuffer, resources.sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionResource(commandBuffer, resources.motionVectors, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionResource(commandBuffer, resources.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionResource(commandBuffer, resources.reactiveMask, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionResource(commandBuffer, resources.transparencyMask,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    bool usedVendorUpscaler = false;
    bool vendorSucceeded = false;
    if (g_App.transition.currentMode == FgMode::Dlss && g_App.config.upscalingEnabled &&
        g_App.sl.dlssSrConfigured) {
        usedVendorUpscaler = true;
        TransitionResource(commandBuffer, resources.hudlessColor, VK_IMAGE_LAYOUT_GENERAL);
        vendorSucceeded = RecordStreamlineInputsAndUpscale(commandBuffer, resources, frameToken, jitter,
                                                           backbufferIndex);
        resources.hudlessColor.layout = VK_IMAGE_LAYOUT_GENERAL;
    } else if (g_App.transition.currentMode == FgMode::Fsr && g_App.config.upscalingEnabled &&
               g_App.ffx.upscaleContext) {
        usedVendorUpscaler = true;
        TransitionResource(commandBuffer, resources.hudlessColor, VK_IMAGE_LAYOUT_GENERAL);
        vendorSucceeded = RecordFsrUpscaleAndPrepare(commandBuffer, resources, jitter);
        resources.hudlessColor.layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    if (!usedVendorUpscaler || !vendorSucceeded) {
        if (usedVendorUpscaler) {
            testapp::Log("[FG-DIAG] WARN vendor SR dispatch failed frameID=%llu mode=%s; using TAAU\n",
                         static_cast<unsigned long long>(g_App.frameId),
                         ModeName(g_App.transition.currentMode));
        }
        RecordTaa(commandBuffer, resources);
    } else {
        TransitionResource(commandBuffer, resources.hudlessColor,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (g_App.transition.currentMode == FgMode::Dlss &&
        (!g_App.config.upscalingEnabled || !g_App.sl.dlssSrConfigured)) {
        RecordStreamlineInputsAndUpscale(commandBuffer, resources, frameToken, jitter, backbufferIndex);
    }
    if (g_App.transition.currentMode == FgMode::Fsr && !g_App.config.upscalingEnabled) {
        RecordFsrUpscaleAndPrepare(commandBuffer, resources, jitter);
        TransitionResource(commandBuffer, resources.hudlessColor,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return vendorSucceeded || !usedVendorUpscaler;
}
}
}

namespace testapp::vkfg {
namespace {
void UpdateHistory(VkCommandBuffer commandBuffer, FrameResources& resources) {
    TransitionResource(commandBuffer, resources.hudlessColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    TransitionResource(commandBuffer, resources.historyColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.dstSubresource.layerCount = 1;
    copy.extent = {g_App.swapchain.extent.width, g_App.swapchain.extent.height, 1};
    vkCmdCopyImage(commandBuffer, resources.hudlessColor.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   resources.historyColor.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    TransitionResource(commandBuffer, resources.hudlessColor,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionResource(commandBuffer, resources.historyColor,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    resources.historyValid = true;
}
}
}

namespace testapp::vkfg {
namespace {
void RecordPresentationTarget(VkCommandBuffer commandBuffer, FrameResources& resources) {
    TransitionResource(commandBuffer, resources.hudlessColor,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionResource(commandBuffer, resources.presentationColor,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const VkClearValue clear = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    BeginRenderPass(commandBuffer, g_App.renderer.swapchainRenderPass,
                    resources.presentationFramebuffer,
                    g_App.swapchain.extent.width, g_App.swapchain.extent.height, &clear, 1);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_App.renderer.presentPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g_App.renderer.presentPipelineLayout, 0, 1, &resources.presentSet, 0,
                            nullptr);
    const uint32_t frameIndex = static_cast<uint32_t>(g_App.frameId);
    vkCmdPushConstants(commandBuffer, g_App.renderer.presentPipelineLayout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(frameIndex), &frameIndex);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);
    resources.presentationColor.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    TransitionResource(commandBuffer, resources.presentationColor,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
}
}

namespace testapp::vkfg {
namespace {
void RecordPresent(VkCommandBuffer commandBuffer, FrameResources& resources, uint32_t imageIndex) {
    TransitionResource(commandBuffer, resources.presentationColor,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionResource(commandBuffer, resources.uiColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionImage(commandBuffer, g_App.swapchain.images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
                    &g_App.swapchain.layouts[imageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const VkClearValue clear = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    BeginRenderPass(commandBuffer, g_App.renderer.swapchainRenderPass,
                    g_App.swapchain.framebuffers[imageIndex], g_App.swapchain.extent.width,
                    g_App.swapchain.extent.height, &clear, 1);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_App.renderer.composePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g_App.renderer.composePipelineLayout, 0, 1, &resources.composeSet, 0,
                            nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);
    g_App.swapchain.layouts[imageIndex] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    TransitionImage(commandBuffer, g_App.swapchain.images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
                    &g_App.swapchain.layouts[imageIndex], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}
}
}

namespace testapp::vkfg {
namespace {
bool HandleVulkanFailure(VkResult result, const char* operation) {
    testapp::Log("[FG-DIAG] %s failed result=%s(%d) frameID=%llu\n", operation,
                 VkResultName(result), static_cast<int>(result),
                 static_cast<unsigned long long>(g_App.frameId));
    if (result == VK_ERROR_DEVICE_LOST) {
        g_App.vk.deviceLost = true;
        MarkDeviceLost(&g_App.transition);
        LogDeviceFault(operation);
    }
    return false;
}
}
}

namespace testapp::vkfg {
namespace {
void UpdateFrameTiming() {
    const auto now = std::chrono::steady_clock::now();
    const float deltaMs = std::chrono::duration<float, std::milli>(now - g_App.previousFrameTime).count();
    g_App.previousFrameTime = now;
    const float sampleSeconds =
        std::chrono::duration<float>(now - g_App.fpsSampleTime).count();
    if (sampleSeconds >= 0.25f) {
        const uint64_t sampleFrames = g_App.frameId - g_App.fpsSampleFrame;
        const float sampleFps = static_cast<float>(sampleFrames) / sampleSeconds;
        if (sampleFrames != 0 && sampleFps > 0.0f) {
            g_App.fps = g_App.fps <= 0.0f ? sampleFps : g_App.fps * 0.35f + sampleFps * 0.65f;
        }
        g_App.fpsSampleTime = now;
        g_App.fpsSampleFrame = g_App.frameId;
    }
    // Frame pacing is bursty with three frames in flight. Feed SDKs the measured presentation
    // cadence, not the near-zero CPU interval between submissions in the same burst.
    g_App.frameDeltaMs = std::clamp(1000.0f / std::max(g_App.fps, 1.0f), 0.05f, 250.0f);
    if (deltaMs > 50.0f) {
        ++g_App.pacingSpikes;
        if (g_App.pacingSpikes <= 5 || (g_App.pacingSpikes % 60) == 0) {
            testapp::Log(
                "[FG-PACING] spike=%llu frameID=%llu deltaMs=%.3f owner=%s mode=%s suspended=%d\n",
                static_cast<unsigned long long>(g_App.pacingSpikes),
                static_cast<unsigned long long>(g_App.frameId), deltaMs,
                OwnerName(g_App.swapchain.owner), ModeName(g_App.transition.currentMode),
                g_App.transition.suspended ? 1 : 0);
        }
    }
}
}
}

namespace testapp::vkfg {
namespace {
void LogHeartbeat() {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<float>(now - g_App.heartbeatTime).count() < 1.0f) {
        return;
    }
    const uint64_t frameDelta = g_App.frameId - g_App.heartbeatFrame;
    const float seconds = std::chrono::duration<float>(now - g_App.heartbeatTime).count();
    g_App.heartbeatTime = now;
    g_App.heartbeatFrame = g_App.frameId;
    const char* upscaler = "TAAU";
    if (g_App.transition.currentMode == FgMode::Dlss && g_App.sl.dlssSrConfigured) {
        upscaler = "DLSS SR";
    } else if (g_App.transition.currentMode == FgMode::Fsr && g_App.ffx.upscaleSupported) {
        upscaler = "FSR 3.1.4 SR";
    }
    const double timingSamples = static_cast<double>(std::max<uint64_t>(g_CpuTimings.samples, 1));
    testapp::Log(
        "[FG-HEARTBEAT] fps=%.2f intervalFps=%.2f frameID=%llu slot=%u upscaler='%s' "
        "requestedFG=%s suspended=%d effective(dlss=%d,fsr=%d) reflex=%s owner=%s route=%s "
        "presented=%llu generated=%llu validationErrors=%llu pacingSpikes=%llu "
        "cpuMs(start=%.3f,frameFence=%.3f,acquire=%.3f,imageFence=%.3f,recordSubmit=%.3f,"
        "present=%.3f,slPoll=%.3f)\n",
        g_App.fps, static_cast<float>(frameDelta) / std::max(seconds, 0.001f),
        static_cast<unsigned long long>(g_App.frameId), g_App.frameSlot, upscaler,
        ModeName(g_App.transition.currentMode), g_App.transition.suspended ? 1 : 0,
        g_App.sl.dlssFgConfigured ? 1 : 0, g_App.ffx.frameGenerationConfigured ? 1 : 0,
        g_App.sl.reflexActive ? "low-latency" : "off", OwnerName(g_App.swapchain.owner),
        WsiRouteName(g_App.swapchain.wsi.route),
        static_cast<unsigned long long>(g_App.presentedFrames),
        static_cast<unsigned long long>(g_App.generatedFrames),
        static_cast<unsigned long long>(g_App.validationErrors),
        static_cast<unsigned long long>(g_App.pacingSpikes),
        g_CpuTimings.reflexStartMs / timingSamples,
        g_CpuTimings.frameFenceMs / timingSamples,
        g_CpuTimings.acquireMs / timingSamples,
        g_CpuTimings.imageFenceMs / timingSamples,
        g_CpuTimings.recordSubmitMs / timingSamples,
        g_CpuTimings.presentMs / timingSamples,
        g_CpuTimings.streamlinePollMs / timingSamples);
    g_CpuTimings = {};
}
}
}

namespace testapp::vkfg {
bool RenderFrame() {
    if (!g_App.renderer.initialized || g_App.vk.deviceLost || g_App.swapchain.handle == VK_NULL_HANDLE) {
        return false;
    }
    UpdateFrameTiming();
    // Reflex sleep belongs at the beginning of the application frame, before CPU/GPU availability
    // waits. It is intentionally invoked even when Reflex mode is off; frameLimitUs remains zero.
    const auto reflexStart = std::chrono::steady_clock::now();
    sl::FrameToken* frameToken = BeginStreamlineFrame();
    const auto reflexEnd = std::chrono::steady_clock::now();
    FrameContext& frame = g_App.frames[g_App.frameSlot];
    const auto frameFenceStart = reflexEnd;
    VkResult result = vkWaitForFences(g_App.vk.device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
    const auto frameFenceEnd = std::chrono::steady_clock::now();
    if (result != VK_SUCCESS) {
        return HandleVulkanFailure(result, "vkWaitForFences(frame)");
    }

    uint32_t imageIndex = 0;
    const auto acquireStart = frameFenceEnd;
    result = AcquireSwapchainImage(frame, &imageIndex);
    const auto acquireEnd = std::chrono::steady_clock::now();
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return true;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return HandleVulkanFailure(result, "AcquireSwapchainImage");
    }
    if (imageIndex >= g_App.swapchain.imageFences.size()) {
        return HandleVulkanFailure(VK_ERROR_INITIALIZATION_FAILED, "acquired image index bounds");
    }
    const auto imageFenceStart = acquireEnd;
    if (g_App.swapchain.imageFences[imageIndex] != VK_NULL_HANDLE &&
        g_App.swapchain.imageFences[imageIndex] != frame.fence) {
        result = vkWaitForFences(g_App.vk.device, 1, &g_App.swapchain.imageFences[imageIndex], VK_TRUE,
                                 UINT64_MAX);
        if (result != VK_SUCCESS) {
            return HandleVulkanFailure(result, "vkWaitForFences(swapchain image)");
        }
    }
    const auto imageFenceEnd = std::chrono::steady_clock::now();
    g_App.swapchain.imageFences[imageIndex] = frame.fence;
    vkResetFences(g_App.vk.device, 1, &frame.fence);
    vkResetCommandPool(g_App.vk.device, frame.commandPool, 0);

    SetStreamlineMarker(frameToken, sl::PCLMarker::eSimulationStart, "SimulationStart");
    const auto recordSubmitStart = imageFenceEnd;
    const JitterOffset jitter = CurrentJitter();
    const float timeSeconds = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - g_App.startTime).count();
    SetStreamlineMarker(frameToken, sl::PCLMarker::eSimulationEnd, "SimulationEnd");

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        return HandleVulkanFailure(result, "vkBeginCommandBuffer");
    }
    FrameResources& resources = g_App.renderer.resources[g_App.frameSlot];
    RecordScene(frame.commandBuffer, resources, jitter, timeSeconds);
    RecordUi(frame.commandBuffer, resources, timeSeconds);
    RecordUpscaling(frame.commandBuffer, resources, frameToken, jitter, imageIndex);
    UpdateHistory(frame.commandBuffer, resources);
    RecordPresentationTarget(frame.commandBuffer, resources);
    RecordPresent(frame.commandBuffer, resources, imageIndex);
    result = vkEndCommandBuffer(frame.commandBuffer);
    if (result != VK_SUCCESS) {
        return HandleVulkanFailure(result, "vkEndCommandBuffer");
    }

    SetStreamlineMarker(frameToken, sl::PCLMarker::eRenderSubmitStart, "RenderSubmitStart");
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.commandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &g_App.swapchain.presentReadySemaphores[imageIndex];
    result = vkQueueSubmit(g_App.vk.gameQueue, 1, &submit, frame.fence);
    SetStreamlineMarker(frameToken, sl::PCLMarker::eRenderSubmitEnd, "RenderSubmitEnd");
    if (result != VK_SUCCESS) {
        return HandleVulkanFailure(result, "vkQueueSubmit");
    }
    const auto recordSubmitEnd = std::chrono::steady_clock::now();

    SetStreamlineMarker(frameToken, sl::PCLMarker::ePresentStart, "PresentStart");
    const auto presentStart = recordSubmitEnd;
    result = PresentSwapchainImage(imageIndex);
    const auto presentEnd = std::chrono::steady_clock::now();
    SetStreamlineMarker(frameToken, sl::PCLMarker::ePresentEnd, "PresentEnd");
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR && result != VK_ERROR_OUT_OF_DATE_KHR) {
        return HandleVulkanFailure(result, "PresentSwapchainImage");
    }
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        ++g_App.presentedFrames;
        OnFramePresentedSuccessfully();
        if (g_App.presentedFrames == 1) {
            StartFidelityFxRuntimePreload("first visible FG-off present");
        }
    }
    const auto streamlinePollStart = presentEnd;
    PollStreamlineState();
    const auto streamlinePollEnd = std::chrono::steady_clock::now();
    g_CpuTimings.reflexStartMs += MillisecondsBetween(reflexStart, reflexEnd);
    g_CpuTimings.frameFenceMs += MillisecondsBetween(frameFenceStart, frameFenceEnd);
    g_CpuTimings.acquireMs += MillisecondsBetween(acquireStart, acquireEnd);
    g_CpuTimings.imageFenceMs += MillisecondsBetween(imageFenceStart, imageFenceEnd);
    g_CpuTimings.recordSubmitMs += MillisecondsBetween(recordSubmitStart, recordSubmitEnd);
    g_CpuTimings.presentMs += MillisecondsBetween(presentStart, presentEnd);
    g_CpuTimings.streamlinePollMs +=
        MillisecondsBetween(streamlinePollStart, streamlinePollEnd);
    ++g_CpuTimings.samples;
    QueryMemoryBudgetStress();
    LogHeartbeat();
    g_App.previousTimeSeconds = timeSeconds;
    g_App.resetTemporalHistory = false;
    ++g_App.frameId;
    g_App.frameSlot = (g_App.frameSlot + 1) % kFramesInFlight;
    return true;
}
}
