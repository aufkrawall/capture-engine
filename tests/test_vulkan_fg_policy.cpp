#include <gtest/gtest.h>

#include "../testapp/fg_switch_config.h"
#include "../testapp/vulkan_fg_policy.h"

namespace {

using namespace testapp::vkfg;

std::array<float, 16> MultiplyMatrices(const std::array<float, 16>& left, const std::array<float, 16>& right) {
    std::array<float, 16> result{};
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            for (size_t inner = 0; inner < 4; ++inner) {
                result[row * 4 + column] += left[row * 4 + inner] * right[inner * 4 + column];
            }
        }
    }
    return result;
}

TEST(VulkanFgPolicyTest, OwnerAndDispatchTablesRemainPermanentlyPaired) {
    EXPECT_EQ(OwnerForMode(FgMode::Off), SwapchainOwner::Native);
    EXPECT_EQ(OwnerForMode(FgMode::Dlss), SwapchainOwner::Streamline);
    EXPECT_EQ(OwnerForMode(FgMode::Fsr), SwapchainOwner::FidelityFX);
    EXPECT_TRUE(IsOwnerDispatchPairValid(SwapchainOwner::Native, VulkanWsiRoute::Loader));
    EXPECT_TRUE(IsOwnerDispatchPairValid(SwapchainOwner::Streamline, VulkanWsiRoute::StreamlineProxy));
    EXPECT_TRUE(IsOwnerDispatchPairValid(SwapchainOwner::FidelityFX, VulkanWsiRoute::FidelityFXReplacement));
    EXPECT_FALSE(IsOwnerDispatchPairValid(SwapchainOwner::Streamline, VulkanWsiRoute::Loader));
    EXPECT_FALSE(IsOwnerDispatchPairValid(SwapchainOwner::FidelityFX, VulkanWsiRoute::StreamlineProxy));
}

TEST(VulkanFgPolicyTest, OldSwapchainNeverCrossesOwnerDispatchBoundaries) {
    EXPECT_TRUE(ShouldForwardOldSwapchain(SwapchainOwner::Native, SwapchainOwner::Native));
    EXPECT_TRUE(ShouldForwardOldSwapchain(SwapchainOwner::Native, SwapchainOwner::Streamline));
    EXPECT_TRUE(ShouldForwardOldSwapchain(SwapchainOwner::Streamline, SwapchainOwner::Native));
    EXPECT_TRUE(ShouldForwardOldSwapchain(SwapchainOwner::Streamline, SwapchainOwner::Streamline));
    EXPECT_FALSE(ShouldForwardOldSwapchain(SwapchainOwner::FidelityFX, SwapchainOwner::FidelityFX));
    EXPECT_FALSE(ShouldForwardOldSwapchain(SwapchainOwner::Native, SwapchainOwner::FidelityFX));
    EXPECT_FALSE(ShouldForwardOldSwapchain(SwapchainOwner::Streamline, SwapchainOwner::FidelityFX));
    EXPECT_FALSE(ShouldForwardOldSwapchain(SwapchainOwner::FidelityFX, SwapchainOwner::Native));
    EXPECT_FALSE(ShouldForwardOldSwapchain(SwapchainOwner::FidelityFX, SwapchainOwner::Streamline));
}

TEST(VulkanFgPolicyTest, SceneCameraAndProjectionMatchDx12SdkInputs) {
    const SceneCameraPolicy camera = BuildSceneCameraPolicy(16.0f / 9.0f);
    EXPECT_FLOAT_EQ(camera.position[0], 0.0f);
    EXPECT_FLOAT_EQ(camera.position[1], 2.4f);
    EXPECT_FLOAT_EQ(camera.position[2], -5.5f);
    EXPECT_FLOAT_EQ(camera.right[0], 1.0f);
    EXPECT_NEAR(camera.forward[1], -0.239223f, 0.00001f);
    EXPECT_NEAR(camera.forward[2], 0.970965f, 0.00001f);
    EXPECT_NEAR(camera.up[1], 0.970965f, 0.00001f);
    EXPECT_NEAR(camera.up[2], 0.239223f, 0.00001f);
    EXPECT_FLOAT_EQ(camera.nearPlane, 0.1f);
    EXPECT_FLOAT_EQ(camera.farPlane, 1000.0f);

    const SceneProjectionPolicy projection = BuildSceneProjectionPolicy(camera);
    EXPECT_GT(projection.viewToClip[0], 0.0f);
    EXPECT_GT(projection.viewToClip[5], projection.viewToClip[0]);
    EXPECT_FLOAT_EQ(projection.viewToClip[11], 1.0f);
    EXPECT_LT(projection.viewToClip[14], 0.0f);
    const std::array<float, 16> identity = MultiplyMatrices(projection.viewToClip, projection.clipToView);
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            EXPECT_NEAR(identity[row * 4 + column], row == column ? 1.0f : 0.0f, 0.0001f);
        }
    }
}

TEST(VulkanFgPolicyTest, VulkanFsr4FallsBackWithoutDisablingNonMlFsr) {
    const VulkanFsrVersionResolution automatic = ResolveVulkanFsrVersion(0);
    EXPECT_EQ(automatic.resolved, 3);
    EXPECT_FALSE(automatic.mlFallback);
    const VulkanFsrVersionResolution fsr3 = ResolveVulkanFsrVersion(3);
    EXPECT_EQ(fsr3.resolved, 3);
    EXPECT_FALSE(fsr3.invalidRequest);
    const VulkanFsrVersionResolution fsr4 = ResolveVulkanFsrVersion(4);
    EXPECT_EQ(fsr4.resolved, 3);
    EXPECT_TRUE(fsr4.mlFallback);
    const VulkanFsrVersionResolution invalid = ResolveVulkanFsrVersion(17);
    EXPECT_EQ(invalid.resolved, 3);
    EXPECT_TRUE(invalid.invalidRequest);
}

TEST(VulkanFgPolicyTest, PresentModeSelectionHonorsVsyncAndBestLowLatencyFallback) {
    const std::vector<PresentMode> all = {PresentMode::Fifo, PresentMode::Immediate, PresentMode::Mailbox};
    EXPECT_EQ(SelectPresentMode(true, all), PresentMode::Fifo);
    EXPECT_EQ(SelectPresentMode(false, all), PresentMode::Immediate);
    EXPECT_EQ(SelectPresentMode(false, {PresentMode::Fifo, PresentMode::Immediate}), PresentMode::Immediate);
    EXPECT_EQ(SelectPresentMode(false, {PresentMode::FifoRelaxed, PresentMode::Fifo}), PresentMode::FifoRelaxed);
    EXPECT_EQ(SelectPresentMode(false, {}), PresentMode::Fifo);
}

TEST(VulkanFgPolicyTest, ResourceMetadataMatchesSdkInputChain) {
    const FgResourceMetadata scene = DescribeFgResource(FgResourceRole::SceneColor);
    EXPECT_EQ(scene.channels, 4);
    EXPECT_EQ(scene.bitsPerChannel, 16);
    EXPECT_TRUE(scene.floatingPoint);
    EXPECT_FALSE(scene.fullResolution);

    const FgResourceMetadata motion = DescribeFgResource(FgResourceRole::MotionVectors);
    EXPECT_EQ(motion.channels, 2);
    EXPECT_EQ(motion.bitsPerChannel, 16);
    const FgResourceMetadata depth = DescribeFgResource(FgResourceRole::Depth);
    EXPECT_TRUE(depth.depth);
    EXPECT_EQ(depth.bitsPerChannel, 32);
    const FgResourceMetadata mask = DescribeFgResource(FgResourceRole::Mask);
    EXPECT_EQ(mask.channels, 1);
    EXPECT_EQ(mask.bitsPerChannel, 8);
    const FgResourceMetadata hudless = DescribeFgResource(FgResourceRole::HudlessColor);
    EXPECT_TRUE(hudless.fullResolution);
    EXPECT_TRUE(hudless.floatingPoint);
    const FgResourceMetadata ui = DescribeFgResource(FgResourceRole::UiColor);
    EXPECT_TRUE(ui.fullResolution);
    EXPECT_FALSE(ui.floatingPoint);
    EXPECT_EQ(ui.bitsPerChannel, 8);
    const FgResourceMetadata presentation = DescribeFgResource(FgResourceRole::Presentation);
    EXPECT_TRUE(presentation.fullResolution);
    EXPECT_FALSE(presentation.floatingPoint);
    EXPECT_EQ(presentation.channels, 4);
    EXPECT_EQ(presentation.bitsPerChannel, 8);
}

TEST(VulkanFgPolicyTest, FidelityFxResourceStatesMapToRequiredVulkanLayouts) {
    const FfxResourceStateBits bits{1u << 0, 1u << 1, 1u << 2, 1u << 3, 1u << 4, 1u << 5};
    EXPECT_EQ(ResolveFfxResourceLayout(bits.present, bits), VulkanImageLayoutClass::Present);
    EXPECT_EQ(ResolveFfxResourceLayout(bits.copySource, bits), VulkanImageLayoutClass::TransferSource);
    EXPECT_EQ(ResolveFfxResourceLayout(bits.copyDestination, bits), VulkanImageLayoutClass::TransferDestination);
    EXPECT_EQ(ResolveFfxResourceLayout(bits.renderTarget, bits), VulkanImageLayoutClass::ColorAttachment);
    EXPECT_EQ(ResolveFfxResourceLayout(bits.unorderedAccess, bits), VulkanImageLayoutClass::General);
    EXPECT_EQ(ResolveFfxResourceLayout(bits.pixelComputeRead, bits), VulkanImageLayoutClass::ShaderRead);
    EXPECT_EQ(ResolveFfxResourceLayout(0, bits), VulkanImageLayoutClass::General);
    EXPECT_EQ(ResolveFfxResourceLayout(bits.present | bits.unorderedAccess, bits), VulkanImageLayoutClass::Present);
}

TEST(VulkanFgPolicyTest, QueuePlannerKeepsAllRuntimeQueuesDistinct) {
    const std::vector<VulkanQueueFamilyCaps> caps = {
        {0, 8, true, true, true, true, false},
        {1, 2, false, true, true, false, false},
        {2, 1, false, false, false, false, true},
    };
    VulkanQueueRequirements requirements{};
    requirements.requestAsyncPresent = true;
    requirements.streamlineGraphicsQueues = 1;
    requirements.streamlineComputeQueues = 1;
    requirements.streamlineOpticalFlowQueues = 1;
    const VulkanQueuePlan plan = BuildVulkanQueuePlan(caps, requirements);
    EXPECT_TRUE(plan.baseAvailable);
    EXPECT_TRUE(plan.fidelityFxAvailable);
    EXPECT_TRUE(plan.streamlineAvailable);
    ASSERT_TRUE(plan.asyncPresentAvailable);
    std::vector<VulkanQueueRef> queues = {plan.game, plan.asyncPresent, plan.ffxAsyncCompute, plan.ffxPresent,
                                          plan.ffxImageAcquire};
    queues.insert(queues.end(), plan.streamlineGraphics.begin(), plan.streamlineGraphics.end());
    queues.insert(queues.end(), plan.streamlineCompute.begin(), plan.streamlineCompute.end());
    queues.insert(queues.end(), plan.streamlineOpticalFlow.begin(), plan.streamlineOpticalFlow.end());
    for (size_t i = 0; i < queues.size(); ++i) {
        ASSERT_TRUE(queues[i].Valid());
        for (size_t j = i + 1; j < queues.size(); ++j) {
            EXPECT_FALSE(queues[i] == queues[j]);
        }
    }
    EXPECT_GE(plan.requestedQueueCounts[0], 4u);
}

TEST(VulkanFgPolicyTest, AsyncPresentFallsBackWithoutDisablingFeatureQueues) {
    const std::vector<VulkanQueueFamilyCaps> caps = {
        {0, 1, true, true, true, true, false},
        {1, 3, false, true, true, true, false},
    };
    VulkanQueueRequirements requirements{};
    requirements.requestFidelityFX = false;
    requirements.requestAsyncPresent = true;
    const VulkanQueuePlan plan = BuildVulkanQueuePlan(caps, requirements);
    EXPECT_TRUE(plan.baseAvailable);
    EXPECT_FALSE(plan.asyncPresentAvailable);
    EXPECT_FALSE(plan.asyncPresent.Valid());
}

TEST(VulkanFgPolicyTest, MissingFfxQueuesDisablesOnlyFfx) {
    const std::vector<VulkanQueueFamilyCaps> caps = {{0, 2, true, true, true, true, false}};
    VulkanQueueRequirements requirements{};
    requirements.streamlineGraphicsQueues = 1;
    const VulkanQueuePlan plan = BuildVulkanQueuePlan(caps, requirements);
    EXPECT_TRUE(plan.baseAvailable);
    EXPECT_TRUE(plan.streamlineAvailable);
    EXPECT_FALSE(plan.fidelityFxAvailable);
}

TEST(VulkanFgPolicyTest, MissingStreamlineQueueDoesNotDisableNativeOrFfx) {
    const std::vector<VulkanQueueFamilyCaps> caps = {{0, 5, true, true, true, true, false}};
    VulkanQueueRequirements requirements{};
    requirements.streamlineOpticalFlowQueues = 1;
    const VulkanQueuePlan plan = BuildVulkanQueuePlan(caps, requirements);
    EXPECT_TRUE(plan.baseAvailable);
    EXPECT_TRUE(plan.fidelityFxAvailable);
    EXPECT_FALSE(plan.streamlineAvailable);
}

TEST(VulkanFgPolicyTest, PartialStreamlineReservationDoesNotStarveFidelityFx) {
    const std::vector<VulkanQueueFamilyCaps> caps = {{0, 5, true, true, true, true, false}};
    VulkanQueueRequirements requirements{};
    requirements.streamlineGraphicsQueues = 1;
    requirements.streamlineOpticalFlowQueues = 1;
    const VulkanQueuePlan plan = BuildVulkanQueuePlan(caps, requirements);
    EXPECT_TRUE(plan.baseAvailable);
    EXPECT_FALSE(plan.streamlineAvailable);
    EXPECT_TRUE(plan.streamlineGraphics.empty());
    EXPECT_TRUE(plan.fidelityFxAvailable);
}

void CompleteTransition(FgTransitionState* state, FgMode target) {
    ASSERT_TRUE(BeginModeTransition(state, target));
    MarkOldFgDisabled(state);
    ASSERT_TRUE(MarkOldPassthroughPresented(state, true));
    ASSERT_TRUE(MarkReplacementPrepared(state, true));
    ASSERT_TRUE(MarkReplacementCreated(state, true));
    ASSERT_TRUE(MarkReplacementPresented(state, true));
    ASSERT_TRUE(MarkTargetActivated(state, true));
}

TEST(VulkanFgPolicyTest, EveryModeTransitionDirectionUsesMakeBeforeBreak) {
    const FgMode modes[] = {FgMode::Off, FgMode::Dlss, FgMode::Fsr};
    for (FgMode from : modes) {
        for (FgMode to : modes) {
            if (from == to) {
                continue;
            }
            FgTransitionState state{};
            state.currentMode = from;
            state.targetMode = from;
            state.owner = OwnerForMode(from);
            CompleteTransition(&state, to);
            EXPECT_EQ(state.currentMode, to);
            EXPECT_EQ(state.owner, OwnerForMode(to));
            EXPECT_EQ(state.stage, TransitionStage::Idle);
            EXPECT_EQ(state.epoch, 1u);
        }
    }
}

TEST(VulkanFgPolicyTest, RepeatedKeySuspendsAndResumesWithoutReplacingProxy) {
    FgTransitionState state{};
    state.currentMode = FgMode::Fsr;
    state.targetMode = FgMode::Fsr;
    state.owner = SwapchainOwner::FidelityFX;
    EXPECT_FALSE(BeginModeTransition(&state, FgMode::Fsr));
    EXPECT_TRUE(state.suspended);
    EXPECT_EQ(state.owner, SwapchainOwner::FidelityFX);
    EXPECT_FALSE(BeginModeTransition(&state, FgMode::Fsr));
    EXPECT_FALSE(state.suspended);
    EXPECT_EQ(state.epoch, 0u);
}

TEST(VulkanFgPolicyTest, FailedPreparationRollsBackToVisibleOldOwner) {
    FgTransitionState state{};
    state.currentMode = FgMode::Fsr;
    state.targetMode = FgMode::Fsr;
    state.owner = SwapchainOwner::FidelityFX;
    ASSERT_TRUE(BeginModeTransition(&state, FgMode::Dlss));
    MarkOldFgDisabled(&state);
    ASSERT_TRUE(MarkOldPassthroughPresented(&state, true));
    EXPECT_FALSE(MarkReplacementPrepared(&state, false));
    EXPECT_EQ(state.stage, TransitionStage::Rollback);
    EXPECT_TRUE(RollbackPreparedTransition(&state));
    EXPECT_EQ(state.owner, SwapchainOwner::FidelityFX);
    EXPECT_EQ(state.currentMode, FgMode::Fsr);
}

TEST(VulkanFgPolicyTest, FailedTransitionConsumesOnlyItsOwnRequest) {
    EXPECT_EQ(ResolveRequestedModeAfterTransitionFailure(FgMode::Fsr, FgMode::Fsr, FgMode::Off), FgMode::Off);
    EXPECT_EQ(ResolveRequestedModeAfterTransitionFailure(FgMode::Dlss, FgMode::Fsr, FgMode::Off), FgMode::Dlss);
}

TEST(VulkanFgPolicyTest, FailedPassthroughCannotBreakOldSurface) {
    FgTransitionState state{};
    state.currentMode = FgMode::Dlss;
    state.targetMode = FgMode::Dlss;
    state.owner = SwapchainOwner::Streamline;
    ASSERT_TRUE(BeginModeTransition(&state, FgMode::Fsr));
    MarkOldFgDisabled(&state);
    EXPECT_FALSE(MarkOldPassthroughPresented(&state, false));
    EXPECT_EQ(state.stage, TransitionStage::OldPassthroughPending);
    EXPECT_EQ(state.owner, SwapchainOwner::Streamline);
}

TEST(VulkanFgPolicyTest, FailedReplacementPresentKeepsNewSurfaceAndRetries) {
    FgTransitionState state{};
    ASSERT_TRUE(BeginModeTransition(&state, FgMode::Dlss));
    MarkOldFgDisabled(&state);
    ASSERT_TRUE(MarkOldPassthroughPresented(&state, true));
    ASSERT_TRUE(MarkReplacementPrepared(&state, true));
    ASSERT_TRUE(MarkReplacementCreated(&state, true));
    EXPECT_FALSE(MarkReplacementPresented(&state, false));
    EXPECT_EQ(state.stage, TransitionStage::ReplacementPresentPending);
    EXPECT_EQ(state.owner, SwapchainOwner::Streamline);
    EXPECT_TRUE(MarkReplacementPresented(&state, true));
}

TEST(VulkanFgPolicyTest, FailedReplacementCreationRollsBackBeforeOwnerCommit) {
    FgTransitionState state{};
    state.currentMode = FgMode::Fsr;
    state.targetMode = FgMode::Fsr;
    state.owner = SwapchainOwner::FidelityFX;
    ASSERT_TRUE(BeginModeTransition(&state, FgMode::Dlss));
    MarkOldFgDisabled(&state);
    ASSERT_TRUE(MarkOldPassthroughPresented(&state, true));
    ASSERT_TRUE(MarkReplacementPrepared(&state, true));
    EXPECT_FALSE(MarkReplacementCreated(&state, false));
    EXPECT_EQ(state.stage, TransitionStage::Rollback);
    EXPECT_EQ(state.owner, SwapchainOwner::FidelityFX);
    EXPECT_FALSE(state.replacementCommitted);
}

TEST(VulkanFgPolicyTest, ResizeRecreationKeepsOwnerAndRequiresNewFgOffPresent) {
    FgTransitionState state{};
    state.currentMode = FgMode::Fsr;
    state.targetMode = FgMode::Fsr;
    state.owner = SwapchainOwner::FidelityFX;
    EXPECT_TRUE(MarkSameOwnerReplacementCommitted(&state));
    EXPECT_EQ(state.owner, SwapchainOwner::FidelityFX);
    EXPECT_EQ(state.targetOwner, SwapchainOwner::FidelityFX);
    EXPECT_EQ(state.stage, TransitionStage::ReplacementPresentPending);
    EXPECT_TRUE(state.replacementCommitted);
    EXPECT_TRUE(MarkReplacementPresented(&state, true));
    EXPECT_EQ(state.stage, TransitionStage::Activating);
}

TEST(VulkanFgPolicyTest, DeviceLossIsTerminalForTransitions) {
    FgTransitionState state{};
    MarkDeviceLost(&state);
    EXPECT_EQ(state.stage, TransitionStage::DeviceLost);
    EXPECT_FALSE(BeginModeTransition(&state, FgMode::Fsr));
}

TEST(VulkanFgPolicyTest, CancellationBeforeReplacementRestoresOldOwner) {
    FgTransitionState state{};
    state.currentMode = FgMode::Dlss;
    state.targetMode = FgMode::Dlss;
    state.owner = SwapchainOwner::Streamline;
    ASSERT_TRUE(BeginModeTransition(&state, FgMode::Fsr));
    MarkOldFgDisabled(&state);
    EXPECT_TRUE(CancelModeTransitionBeforeReplacement(&state));
    EXPECT_EQ(state.stage, TransitionStage::Idle);
    EXPECT_EQ(state.currentMode, FgMode::Dlss);
    EXPECT_EQ(state.targetMode, FgMode::Dlss);
    EXPECT_EQ(state.owner, SwapchainOwner::Streamline);
    EXPECT_FALSE(state.replacementCommitted);
}

TEST(FgSwitchConfigTest, DefaultsAndAutomaticSequenceMatchDx12Policy) {
    testapp::fg::FgSwitchConfig config{};
    EXPECT_EQ(config.windowWidth, 1920);
    EXPECT_EQ(config.windowHeight, 1080);
    EXPECT_EQ(config.autoFsrStartSeconds, 3);
    EXPECT_EQ(config.autoDlssStartSeconds, 12);
    EXPECT_EQ(config.autoReturnFsrSeconds, 30);
    EXPECT_TRUE(config.upscalingEnabled);
    EXPECT_EQ(config.upscaleQuality, testapp::fg::UpscaleQuality::Quality);
    EXPECT_TRUE(config.fsrSuspendResumeStress);
    EXPECT_TRUE(config.fsrPresentCallbackStress);
}

TEST(FgSwitchConfigTest, AutomaticSequenceIsStrictlyOrdered) {
    testapp::fg::FgSwitchConfig config{};
    config.autoFsrStartSeconds = 50;
    config.autoDlssStartSeconds = 2;
    config.autoReturnFsrSeconds = 1;
    testapp::fg::NormalizeAutoSequenceTimings(&config);
    EXPECT_EQ(config.autoFsrStartSeconds, 50);
    EXPECT_EQ(config.autoDlssStartSeconds, 51);
    EXPECT_EQ(config.autoReturnFsrSeconds, 52);
}

}  // namespace
