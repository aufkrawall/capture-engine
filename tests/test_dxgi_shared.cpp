#include <gtest/gtest.h>

#include "../hook/common/dx12_overlay_policy.h"
#include "../hook/common/dxgi_factory_policy.h"
#include "../hook/common/dxgi_shared.h"

TEST(DXGISharedTest, ExternalPresentDetourPathRequiresBypassSupport) {
    EXPECT_TRUE(DXGIShared::CanSafelyInstallExternalPresentDetourPath(false, false));
    EXPECT_TRUE(DXGIShared::CanSafelyInstallExternalPresentDetourPath(false, true));
    EXPECT_FALSE(DXGIShared::CanSafelyInstallExternalPresentDetourPath(true, false));
    EXPECT_TRUE(DXGIShared::CanSafelyInstallExternalPresentDetourPath(true, true));
}

TEST(DXGISharedTest, SelectTranslatedGraphicsAPINamePrefersDXVKD3D11OverDXVKD3D9) {
    EXPECT_STREQ(DXGIShared::SelectTranslatedGraphicsAPIName(true, true, false, false), "DX11 (DXVK)");
    EXPECT_STREQ(DXGIShared::SelectTranslatedGraphicsAPIName(true, true, false, true), "DX10 (DXVK)");
    EXPECT_STREQ(DXGIShared::SelectTranslatedGraphicsAPIName(false, true, false, false), "DX9 (DXVK)");
    EXPECT_STREQ(DXGIShared::SelectTranslatedGraphicsAPIName(false, false, true, false), "DX12 (VKD3D-Proton)");
    EXPECT_STREQ(DXGIShared::SelectTranslatedGraphicsAPIName(false, false, false, false), "Vulkan");
}

TEST(DXGISharedTest, SelectPrimarySwapChainAPITypePrefersHighestDeviceVersion) {
    EXPECT_EQ(DXGIShared::APIType::D3D12, DXGIShared::SelectPrimarySwapChainAPIType(true, true, true));
    EXPECT_EQ(DXGIShared::APIType::D3D11, DXGIShared::SelectPrimarySwapChainAPIType(false, true, true));
    EXPECT_EQ(DXGIShared::APIType::D3D10, DXGIShared::SelectPrimarySwapChainAPIType(false, false, true));
    EXPECT_EQ(DXGIShared::APIType::Unknown, DXGIShared::SelectPrimarySwapChainAPIType(false, false, false));
}

TEST(DXGISharedTest, DXGIFactoryEnumerationLoggingTreatsNotFoundAsBenign) {
    EXPECT_FALSE(ce::dxgi_factory_policy::ShouldLogAdapterEnumerationFailure(S_OK));
    EXPECT_FALSE(ce::dxgi_factory_policy::ShouldLogAdapterEnumerationFailure(DXGI_ERROR_NOT_FOUND));
    EXPECT_TRUE(ce::dxgi_factory_policy::ShouldLogAdapterEnumerationFailure(E_FAIL));
}

TEST(DXGISharedTest, RecreatedSwapchainsStayHookableWhenExternalOverlayPathIsAlreadyActive) {
    EXPECT_TRUE(DXGIShared::ShouldInstallSwapchainHooksWithThirdPartyOverlay(false, false));
    EXPECT_TRUE(DXGIShared::ShouldInstallSwapchainHooksWithThirdPartyOverlay(true, true));

    EXPECT_FALSE(DXGIShared::ShouldInstallSwapchainHooksWithThirdPartyOverlay(true, false));
}

TEST(DXGISharedTest, SteamDX12BypassStaysEnabledUntilStreamlineFGActuallyRuns) {
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, false, false, true,
                                                               ce::fg_runtime::RuntimeMode::kOff, false, false));
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, true, false, false, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false));
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, true, false, false, true, ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion, false, false));
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, false, false, true,
                                                                ce::fg_runtime::RuntimeMode::kDLSSFG, false, false));
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, false, false, true,
                                                                ce::fg_runtime::RuntimeMode::kOff, true, false));
}

TEST(DXGISharedTest, SteamDX12BypassAlsoCoversNvPresentStartupWindow) {
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, false, false, false,
                                                               ce::fg_runtime::RuntimeMode::kOff, false, true));
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, true, false, false, false, ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion, false, true));
}

TEST(DXGISharedTest, SteamDX12BypassRequiresCleanNonWrappedEntryPath) {
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(false, true, true, false, false, true,
                                                                ce::fg_runtime::RuntimeMode::kOff, false, false));
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(true, false, true, false, false, true,
                                                                ce::fg_runtime::RuntimeMode::kOff, false, false));
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, false, false, false, true,
                                                                ce::fg_runtime::RuntimeMode::kOff, false, false));
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, true, false, true,
                                                                ce::fg_runtime::RuntimeMode::kOff, false, false));
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, false, true, true,
                                                                ce::fg_runtime::RuntimeMode::kOff, false, false));
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, false, false, false,
                                                                ce::fg_runtime::RuntimeMode::kOff, false, false));
}

TEST(DXGISharedTest, DX12StartupPresentPassStaysAvailableOnlyForInactiveNonBypassStartup) {
    EXPECT_TRUE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, false,
                                                                      ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_TRUE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        true, false, false, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false));
    EXPECT_TRUE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        true, false, false, false, ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion, false));
}

TEST(DXGISharedTest, DX12StartupPresentPassDisablesWhenRealFGOrBypassOwnsPath) {
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(false, false, false, false,
                                                                       ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, true, false, false,
                                                                       ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, true, false,
                                                                       ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, true,
                                                                       ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, false,
                                                                       ce::fg_runtime::RuntimeMode::kDLSSFG, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, false,
                                                                       ce::fg_runtime::RuntimeMode::kFSRFG, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, false,
                                                                       ce::fg_runtime::RuntimeMode::kOff, true));
}

TEST(DXGISharedTest, DX12StartupPresentPassStaysDisabledWhenSteamBypassAlreadyOwnsStartupPath) {
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, true,
                                                                       ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, true,
                                                                       ce::fg_runtime::RuntimeMode::kStreamlineNoFG,
                                                                       false));
}

TEST(DXGISharedTest, GlobalCreateSwapchainPathsCaptureQueueWhenSkippingWrapForStreamline) {
    EXPECT_TRUE(DXGIShared::ShouldCaptureQueueWhenSkippingWrapForStreamline(true));
    EXPECT_FALSE(DXGIShared::ShouldCaptureQueueWhenSkippingWrapForStreamline(false));
}

TEST(DXGISharedTest, StreamlineGeneratedFramePresentUsesSyntheticReentrantRoutingOnlyForDX12FGCallers) {
    EXPECT_TRUE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, true));

    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, false, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, false));
}

TEST(DXGISharedTest, FFXPresentBypassesNormalRoutingOnlyDuringDX12StreamlineStartup) {
    EXPECT_TRUE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(true, true, true, false));
    EXPECT_TRUE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(true, true, false, true));

    EXPECT_FALSE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(false, true, true, false));
    EXPECT_FALSE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(true, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(true, true, false, false));
}

TEST(DXGISharedTest, PostSLCallbackStaysInstalledOnlyWhileStreamlineStillOwnsPresentPath) {
    EXPECT_TRUE(DXGIShared::ShouldKeepPostSLCallbackInstalledDuringTransition(true));
    EXPECT_FALSE(DXGIShared::ShouldKeepPostSLCallbackInstalledDuringTransition(false));
}

TEST(DXGISharedTest, EarlyPresentRecursionOnlyShortCircuitsWhenSafeForwardingExists) {
    EXPECT_FALSE(DXGIShared::ShouldTreatEarlyPresentRecursionAsForwardable(false, false, false, false, false));

    EXPECT_TRUE(DXGIShared::ShouldTreatEarlyPresentRecursionAsForwardable(true, false, false, false, false));
    EXPECT_TRUE(DXGIShared::ShouldTreatEarlyPresentRecursionAsForwardable(false, true, false, false, false));
    EXPECT_TRUE(DXGIShared::ShouldTreatEarlyPresentRecursionAsForwardable(false, false, true, false, false));
    EXPECT_TRUE(DXGIShared::ShouldTreatEarlyPresentRecursionAsForwardable(false, false, false, true, false));
    EXPECT_TRUE(DXGIShared::ShouldTreatEarlyPresentRecursionAsForwardable(false, false, false, false, true));
}

TEST(DXGISharedTest, DX12OverlayWaitPolicySkipsSmoothMotionButKeepsStartupSafety) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(false, true, true, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(true, false, false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(true, false, true, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(true, false, true,
                                                                        ce::fg_runtime::RuntimeMode::kStreamlineNoFG));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(
        true, true, true, ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(true, true, false,
                                                                        ce::fg_runtime::RuntimeMode::kFSRFG));
}

TEST(DXGISharedTest, EarlyDX12TempSwapchainHookInstallDefersForStartupOverlayBeforeRealDevice) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferEarlyDX12TempSwapchainPresentHookInstall(false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferEarlyDX12TempSwapchainPresentHookInstall(true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferEarlyDX12TempSwapchainPresentHookInstall(false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferEarlyDX12TempSwapchainPresentHookInstall(true, false));
}

TEST(DXGISharedTest, StartupOverlayCompatibilityModeDependsOnObservedOverlayStateNotProcessName) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUseStartupOverlayCompatibilityMode(true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseStartupOverlayCompatibilityMode(true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseStartupOverlayCompatibilityMode(false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseStartupOverlayCompatibilityMode(true, true, false));
}

TEST(DXGISharedTest, StartupOverlayRenderingRequiresStableNonRuntimeQueue) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(true, true, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(false, false, true));
}

TEST(DXGISharedTest, StartupOverlayResumeDefersOnlyForShortRuntimeOwnedQueueHandoff) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferStartupOverlayWorkAfterResume(true, true, 50, 100));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferStartupOverlayWorkAfterResume(true, true, 100, 100));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferStartupOverlayWorkAfterResume(true, false, 50, 100));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferStartupOverlayWorkAfterResume(false, true, 50, 100));
}

TEST(DXGISharedTest, ThirdPartyOverlaySwapchainQueueCaptureNeverOverridesUnknownOrDifferentGameQueue) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlaySwapchainQueueCapture(true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlaySwapchainQueueCapture(true, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlaySwapchainQueueCapture(false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlaySwapchainQueueCapture(true, true, true));
}

TEST(DXGISharedTest, ThirdPartyOverlaySwapchainsNeverDrivePresentProcessing) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipPresentProcessingForThirdPartyOverlaySwapchain(true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipPresentProcessingForThirdPartyOverlaySwapchain(false));
}

TEST(DXGISharedTest, StartupBlockingOverlaySwapchainBypassClearsOnceLivePresentLeavesOverlayStack) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldKeepStartupBlockingOverlaySwapchainBypass(true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldKeepStartupBlockingOverlaySwapchainBypass(false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldKeepStartupBlockingOverlaySwapchainBypass(true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldKeepStartupBlockingOverlaySwapchainBypass(true, true, false));
}

TEST(DXGISharedTest, WrappedFFXCreateSwapchainTrafficOverridesOverlayClassification) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFFX(false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFFX(true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFFX(true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFFX(false, false));

    const bool effectiveOverlayCaller =
        true && !ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFFX(false, true);
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlaySwapchainQueueCapture(effectiveOverlayCaller, true, false));
}

TEST(DXGISharedTest, ThirdPartyOverlayECLQueueDoesNotOverrideKnownGameTrackingQueues) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlayQueueForGameTracking(true, true, false, false,
                                                                                           false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlayQueueForGameTracking(false, true, false, false,
                                                                                            false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlayQueueForGameTracking(true, false, false, false,
                                                                                            false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlayQueueForGameTracking(true, true, true, false,
                                                                                            false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlayQueueForGameTracking(true, true, false, true,
                                                                                            false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlayQueueForGameTracking(true, true, false, false,
                                                                                            true));
}

TEST(DXGISharedTest, DX12SwapchainOverlayRoutingUsesFSRSwapchainQueueWhenAvailable) {
    using ce::dx12_overlay_policy::DecideSwapchainOverlayRouting;
    using ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision;

    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, true, false, true, true, false, false, false),
              SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue);
    EXPECT_EQ(DecideSwapchainOverlayRouting(true, false, true, false, true, true, false, false, false),
              SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue);
}

TEST(DXGISharedTest, DX12SwapchainOverlayRoutingUsesRuntimeOwnedQueueWithoutTreatingItAsFSR) {
    using ce::dx12_overlay_policy::DecideSwapchainOverlayRouting;
    using ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision;

    EXPECT_EQ(DecideSwapchainOverlayRouting(true, false, false, false, true, true, false, false, false),
              SwapchainOverlayRoutingDecision::kUseRuntimeOwnedSwapchainQueue);

    // GTA 5 Enhanced can briefly land here when DLSS FG suspends during loading
    // screens: the swapchain is runtime-owned, but there is no authoritative FSR
    // signal and we must not enter the post-FSR recovery path.
    EXPECT_EQ(DecideSwapchainOverlayRouting(true, false, false, false, true, true, true, false, false),
              SwapchainOverlayRoutingDecision::kUseRuntimeOwnedSwapchainQueue);
}

TEST(DXGISharedTest, DX12SwapchainOverlayRoutingPreservesRuntimeOwnedFSRAfterHistoryLatch) {
    using ce::dx12_overlay_policy::DecideSwapchainOverlayRouting;
    using ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision;

    EXPECT_EQ(DecideSwapchainOverlayRouting(true, false, false, true, true, true, false, false, false),
              SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue);
    EXPECT_EQ(DecideSwapchainOverlayRouting(true, false, false, true, false, true, false, false, false),
              SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue);
}

TEST(DXGISharedTest, DX12SwapchainOverlayRoutingSkipsOnlyWhenFSRQueueIsUnavailable) {
    using ce::dx12_overlay_policy::DecideSwapchainOverlayRouting;
    using ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision;

    EXPECT_EQ(DecideSwapchainOverlayRouting(true, false, false, false, false, true, false, false, false),
              SwapchainOverlayRoutingDecision::kSkipRuntimeOwnedSwapchainWithoutQueue);
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, true, false, false, true, false, false, false),
              SwapchainOverlayRoutingDecision::kSkipFSRWithoutSwapchainQueue);
}

TEST(DXGISharedTest, DX12SwapchainOverlayRoutingPreservesPostFSRStreamlineTransition) {
    using ce::dx12_overlay_policy::DecideSwapchainOverlayRouting;
    using ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision;

    EXPECT_EQ(DecideSwapchainOverlayRouting(false, true, false, true, true, true, false, false, false),
              SwapchainOverlayRoutingDecision::kUsePostFSRStreamlineQueue);
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, true, false, true, false, true, false, false, false),
              SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue);
}

TEST(DXGISharedTest, DX12SwapchainOverlayRoutingPrefersValidatedLastWorkingQueueDuringPostFSRInactiveRecovery) {
    using ce::dx12_overlay_policy::DecideSwapchainOverlayRouting;
    using ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision;

    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, false, true, false, false, false),
              SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue);

    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, false, true, true, false, false),
              SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue);

    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, false, true, true, false, true),
              SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue);

    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, false, true, true, true, false),
              SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue);

    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, false, true, true, true, true),
              SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue);

    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, true, true, false, false, false),
              SwapchainOverlayRoutingDecision::kUseNormalRouting);
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, false, false, true, false, false, false),
              SwapchainOverlayRoutingDecision::kUseNormalRouting);
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, false, false, false, false, false),
              SwapchainOverlayRoutingDecision::kUseNormalRouting);
}

TEST(DXGISharedTest, PostFSRInactiveRecoveryQueueSourcePrefersOriginalPresentQueue) {
    using ce::dx12_overlay_policy::DecidePostFSRInactiveRecoveryQueueSource;
    using ce::dx12_overlay_policy::PostFSRInactiveRecoveryQueueSource;

    EXPECT_EQ(DecidePostFSRInactiveRecoveryQueueSource(true),
              PostFSRInactiveRecoveryQueueSource::kOriginalPresentQueue);
    EXPECT_EQ(DecidePostFSRInactiveRecoveryQueueSource(false),
              PostFSRInactiveRecoveryQueueSource::kCurrentCommandQueueFallback);
}

TEST(DXGISharedTest, TransitionCooldownOverrideReplacesStaleLongCooldownForSettledPrimaryPostFSROff) {
    EXPECT_EQ(ce::dx12_overlay_policy::ResolveTransitionCooldownFrames(90, 15, true), 15);
    EXPECT_EQ(ce::dx12_overlay_policy::ResolveTransitionCooldownFrames(0, 15, true), 15);
    EXPECT_EQ(ce::dx12_overlay_policy::ResolveTransitionCooldownFrames(10, 60, false), 60);
    EXPECT_EQ(ce::dx12_overlay_policy::ResolveTransitionCooldownFrames(90, 15, false), 90);
}

TEST(DXGISharedTest, SettledPrimaryPostFSROffUsesShorterCooldown) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(true, true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(false, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(true, false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(true, true, false));
}

TEST(DXGISharedTest, FSRSwapchainTakeoverRequiresAuthoritativeFFXTraffic) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(true, true, false, false,
                                                                                               false));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(true, true, true, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(false, true, false,
                                                                                                false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(true, false, false,
                                                                                                false, false));
}

TEST(DXGISharedTest, FSRSwapchainTakeoverDoesNotClearStreamlineOwnershipWithoutFFXEvidence) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(true, false, true, false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(true, false, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(true, false, false,
                                                                                                false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(true, false, true,
                                                                                                false, false));
}

TEST(DXGISharedTest, DX12OverlayMetricsBindingAlwaysKeepsMetricsBound) {
    const auto realFrameBinding = ce::dx12_overlay_policy::DecideOverlayMetricsBinding(true);
    EXPECT_TRUE(realFrameBinding.bindMetrics);
    EXPECT_TRUE(realFrameBinding.refreshFrameMetadata);

    const auto interpolatedFrameBinding = ce::dx12_overlay_policy::DecideOverlayMetricsBinding(false);
    EXPECT_TRUE(interpolatedFrameBinding.bindMetrics);
    EXPECT_FALSE(interpolatedFrameBinding.refreshFrameMetadata);
}

TEST(DXGISharedTest, OverlayFGMetricTypeFollowsEffectiveRuntimeState) {
    EXPECT_EQ(1, ce::dx12_overlay_policy::ResolveOverlayFGMetricType(true, ce::fg_runtime::RuntimeMode::kDLSSFG));
    EXPECT_EQ(2, ce::dx12_overlay_policy::ResolveOverlayFGMetricType(true, ce::fg_runtime::RuntimeMode::kFSRFG));
    EXPECT_EQ(3, ce::dx12_overlay_policy::ResolveOverlayFGMetricType(true,
                                                                      ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion));

    EXPECT_EQ(0, ce::dx12_overlay_policy::ResolveOverlayFGMetricType(false, ce::fg_runtime::RuntimeMode::kDLSSFG));
    EXPECT_EQ(0, ce::dx12_overlay_policy::ResolveOverlayFGMetricType(false,
                                                                      ce::fg_runtime::RuntimeMode::kStreamlineNoFG));
    EXPECT_EQ(0, ce::dx12_overlay_policy::ResolveOverlayFGMetricType(true, ce::fg_runtime::RuntimeMode::kOff));
}

TEST(DXGISharedTest, ZeroECLPresentsStillReachProcessFrameForRuntimeOwnedNonStreamlineSwapchains) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(true, false, false, true, false, false,
                                                                         false, ce::fg_runtime::RuntimeMode::kOff));

    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(true, false, false, false, false, false,
                                                                         false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(true, false, false, true, true, false,
                                                                         false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(true, true, false, false, false, false,
                                                                         false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(true, false, true, false, false, false,
                                                                         false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(false, false, false, false, false, false,
                                                                         false, ce::fg_runtime::RuntimeMode::kOff));
}

TEST(DXGISharedTest, ZeroECLPresentsStillReachProcessFrameDuringRecentStreamlineTeardown) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(true, false, false, false, false, true,
                                                                         false, ce::fg_runtime::RuntimeMode::kOff));

    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(true, false, false, false, true, true,
                                                                         false, ce::fg_runtime::RuntimeMode::kOff));
}

TEST(DXGISharedTest, ZeroECLPresentsStillReachProcessFrameDuringPostFSRNonFGRecovery) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(true, false, false, false, false, false,
                                                                         true, ce::fg_runtime::RuntimeMode::kOff));

    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(true, false, false, false, false, false,
                                                                         false, ce::fg_runtime::RuntimeMode::kOff));
}

TEST(DXGISharedTest, ZeroECLPresentsStillReachProcessFrameDuringStreamlineNoFGStartup) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(true, false, false, false, false, false,
                                                                         false,
                                                                         ce::fg_runtime::RuntimeMode::kStreamlineNoFG));

    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(true, false, false, false, false, false,
                                                                         false, ce::fg_runtime::RuntimeMode::kOff));
}

TEST(DXGISharedTest, DuplicateTopLevelPresentSuppressionBypassesRuntimeOwnedNonStreamlineSwapchains) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressLikelyDuplicateTopLevelPresent(true, false));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSuppressLikelyDuplicateTopLevelPresent(false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSuppressLikelyDuplicateTopLevelPresent(true, true));
}

TEST(DXGISharedTest, DedicatedOverlayQueueStaysDisabledForRuntimeOwnedNativeFG) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(
        true, true, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(
        true, false, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(
        true, true, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(
        true, false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(
        false, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(
        true, false, false, false));
}

TEST(DXGISharedTest, AuthoritativeFSRRealFrameOnlyRunTracksOnlyQualifiedFrames) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(false, true, true, false, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(true, true, true, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(false, false, true, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(false, true, false, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(false, true, true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(false, true, true, false, true));
}

TEST(DXGISharedTest, StaleAuthoritativeFSRRequiresLongFreshRealFrameOnlyRunBeforeClearing) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearAuthoritativeFSRAfterRealFrameOnlyRun(1, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearAuthoritativeFSRAfterRealFrameOnlyRun(119, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldClearAuthoritativeFSRAfterRealFrameOnlyRun(120, false));
}

TEST(DXGISharedTest, StaleAuthoritativeFSRDoesNotClearAfterDirectFFXApiConfirmation) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearAuthoritativeFSRAfterRealFrameOnlyRun(120, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearAuthoritativeFSRAfterRealFrameOnlyRun(1200, true));
}

TEST(DXGISharedTest, AuthoritativeFSRIsPreservedDuringTransitionCooldownForTransientOffEdges) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(true, true, 1));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(true, true, 90));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(false, true, 90));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(true, false, 90));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(true, true, 0));
}

TEST(DXGISharedTest, HeuristicFSRIsPreservedDuringTransientBlocksOnRuntimeOwnedPostFSRSwapchains) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(false, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(true, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(false, false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(false, true, false));
}

TEST(DXGISharedTest, RuntimeOwnedPostFSRTeardownRequiresStrongerOffSignalThanTransientNoneEdge) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(true, true, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(true, true, true, true));
}

TEST(DXGISharedTest, PostFSRNonFGRecoveryUsesPrimaryQueueForFrameClassificationWhenPresentAndRenderQueuesDiffer) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        true, false, false, false, true, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        false, false, false, false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        true, true, false, false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        true, false, true, false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        true, false, false, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        true, false, false, false, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        true, false, false, false, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        true, false, false, false, true, true, true));
}

TEST(DXGISharedTest, DirectPostFSROffIsTreatedAsPostFSRNonFGRecovery) {
    EXPECT_TRUE(ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(true, true, false, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(false, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(true, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(true, true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(true, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(true, true, false, false, true));
}

TEST(DXGISharedTest, PostFSRNonFGRecoveryReservesInactiveFGOverlaySpace) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpace(true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpace(false));
}

TEST(DXGISharedTest, InactiveFGOverlaySpaceReservationRequiresShortPostSLTeardownActivity) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(true, true,
                                                                                                          true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(true, true,
                                                                                                          true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(true, true,
                                                                                                           false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(false, true,
                                                                                                           true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(true, false,
                                                                                                           false));
}

TEST(DXGISharedTest, CleanNonFGSwapchainChangeResetsQueueHeuristicOnlyWhenEndingPostFSRRecovery) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldResetQueueChangeHeuristicAfterCleanNonFGSwapchainChange(true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldResetQueueChangeHeuristicAfterCleanNonFGSwapchainChange(false));
}

TEST(DXGISharedTest, ExplicitSwapchainQueueProofEndsPostFSRRecoveryOnlyWhenOwnershipReturnsToOriginalQueue) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldEndPostFSRNonFGRecoveryOnExplicitSwapchainQueueProof(true, true, true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldEndPostFSRNonFGRecoveryOnExplicitSwapchainQueueProof(false, true, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldEndPostFSRNonFGRecoveryOnExplicitSwapchainQueueProof(true, false, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldEndPostFSRNonFGRecoveryOnExplicitSwapchainQueueProof(true, true, false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldEndPostFSRNonFGRecoveryOnExplicitSwapchainQueueProof(true, true, true, false));
}

TEST(DXGISharedTest, SwapchainChangeGuardCatchesRecentStreamlineTeardownOnRuntimeOwnedQueue) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldGuardSwapchainReinitAfterChange(false, false, false, true, true, true,
                                                                               true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldGuardSwapchainReinitAfterChange(false, false, false, false, true, true,
                                                                                true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldGuardSwapchainReinitAfterChange(false, false, false, true, false, true,
                                                                                true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldGuardSwapchainReinitAfterChange(false, false, false, true, true, false,
                                                                                true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldGuardSwapchainReinitAfterChange(false, false, false, true, true, true,
                                                                                true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldGuardSwapchainReinitAfterChange(false, true, false, false, false, false,
                                                                               false, false));
}

TEST(DXGISharedTest, InactiveRuntimeOwnedSwapchainInitWaitsForCommandQueueToSettle) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, false, true, true,
                                                                                             true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, false, true, true,
                                                                                             false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, false, true, true,
                                                                                             true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, false, false, true,
                                                                                              true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(true, false, true, true,
                                                                                              true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, true, true, true,
                                                                                              true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, false, true, true,
                                                                                              true, true));
}

TEST(DXGISharedTest, RecentStreamlineTeardownInitWaitsForCommandQueueToLeaveDepartedWrapperState) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, true, true, false, false, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, true, true, false, true, false, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        true, false, true, true, true, false, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, true, true, true, true, false, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, false, true, true, false, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, true, false, false, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, true, true, false, true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, true, true, false, true, false, true, false));
}

TEST(DXGISharedTest, RecentStreamlineTeardownInitDoesNotDeferForPrimaryGameQueue) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, true, true, false, true, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, true, true, false, false, false, false, true));
}

TEST(DXGISharedTest, PostFSRStreamlineTeardownWithoutSwapchainQueueWaitsForLiveNonWrapperQueue) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, false, true, false, false, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, false, true, false, true, false, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, false, true, true, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, false, true, true, true, false, false, true));
}

TEST(DXGISharedTest, PostFSRStreamlineTeardownWithoutSwapchainQueueRequiresLastWorkingQueue) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, false, true, false, true, false, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, false, true, true, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, false, true, true, true, false, false, false));
}

TEST(DXGISharedTest, PostSLValidatedDirectQueueIsNotTreatedAsWrapper) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatPostSLSelectedQueueAsWrapper(false, false, false, true));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatPostSLSelectedQueueAsWrapper(false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatPostSLSelectedQueueAsWrapper(true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatPostSLSelectedQueueAsWrapper(false, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatPostSLSelectedQueueAsWrapper(false, false, true, false));
}

TEST(DXGISharedTest, PostFSRFGOffTransitionPreservesLastWorkingQueueOnlyForImmediateRecovery) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreservePostSLLastWorkingQueueForPostFSROffRecovery(true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreservePostSLLastWorkingQueueForPostFSROffRecovery(false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreservePostSLLastWorkingQueueForPostFSROffRecovery(true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreservePostSLLastWorkingQueueForPostFSROffRecovery(true, true, false));
}

TEST(DXGISharedTest, RecentStreamlineTeardownIgnoresOnlyDepartedWrapperQueueRegistration) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
        true, false, false, false, false, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
        false, false, false, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
        true, false, false, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
        true, false, false, false, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
        true, false, false, false, false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
        true, false, false, false, false, false, true));
}

TEST(DXGISharedTest, RecentStreamlineTeardownQueueChangeHeuristicIgnoresOnlyDepartedRuntimeQueues) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldIgnoreQueueChangeHeuristicDuringRecentStreamlineTeardown(
        true, false, false, false, false, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreQueueChangeHeuristicDuringRecentStreamlineTeardown(
        false, false, false, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreQueueChangeHeuristicDuringRecentStreamlineTeardown(
        true, false, false, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreQueueChangeHeuristicDuringRecentStreamlineTeardown(
        true, false, false, false, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreQueueChangeHeuristicDuringRecentStreamlineTeardown(
        true, false, false, false, false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldIgnoreQueueChangeHeuristicDuringRecentStreamlineTeardown(
        true, false, false, false, false, false, true));
}

TEST(DXGISharedTest, PostFSRNonFGRecoverySuppressesHeuristicFSRActivationWhileTeardownTrafficPersists) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringPostFSRNonFGRecovery(
        true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringPostFSRNonFGRecovery(
        true, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringPostFSRNonFGRecovery(
        false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringPostFSRNonFGRecovery(
        true, false, false));
}

TEST(DXGISharedTest, RecentPostSLTeardownActivityStillIgnoresPreservedLastWorkingQueueAfterGraceExpires) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
        false, false, true, false, false, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldIgnoreQueueChangeHeuristicDuringRecentStreamlineTeardown(
        false, false, true, false, false, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
        false, false, true, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreQueueChangeHeuristicDuringRecentStreamlineTeardown(
        false, false, true, false, false, false, false));
}

TEST(DXGISharedTest, PostFSRInactiveRecoveryKeepsIgnoringPreservedLastWorkingQueueUntilRecoveryEnds) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
        false, true, false, false, false, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldIgnoreQueueChangeHeuristicDuringRecentStreamlineTeardown(
        false, true, false, false, false, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
        false, true, false, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreQueueChangeHeuristicDuringRecentStreamlineTeardown(
        false, true, false, false, false, false, false));
}

TEST(DXGISharedTest, InactiveCommandQueueRealignsOnlyForDepartedWrapperState) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldRealignInactiveCommandQueueToSwapchainQueue(false, false, true, true,
                                                                                           true, false, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRealignInactiveCommandQueueToSwapchainQueue(true, false, true, true,
                                                                                            true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRealignInactiveCommandQueueToSwapchainQueue(false, true, true, true,
                                                                                            true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRealignInactiveCommandQueueToSwapchainQueue(false, false, false, true,
                                                                                            true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRealignInactiveCommandQueueToSwapchainQueue(false, false, true, false,
                                                                                            true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRealignInactiveCommandQueueToSwapchainQueue(
        false, false, true, true, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRealignInactiveCommandQueueToSwapchainQueue(false, false, true, true,
                                                                                            true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRealignInactiveCommandQueueToSwapchainQueue(false, false, true, true,
                                                                                            true, false, true, false));
}

TEST(DXGISharedTest, InactiveCommandQueueDoesNotRealignForPrimaryGameQueue) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRealignInactiveCommandQueueToSwapchainQueue(false, false, true, true,
                                                                                            true, false, false, true));
}

TEST(DXGISharedTest, DirectPostFSRStreamlineTeardownDefersImmediateOverlayReinitWhenStateWasInvalidated) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldDeferOverlayReinitAfterDirectPostFSRStreamlineTeardown(true, false, true));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldDeferOverlayReinitAfterDirectPostFSRStreamlineTeardown(true, true, false));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldDeferOverlayReinitAfterDirectPostFSRStreamlineTeardown(true, false, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldDeferOverlayReinitAfterDirectPostFSRStreamlineTeardown(false, false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldDeferOverlayReinitAfterDirectPostFSRStreamlineTeardown(true, true, true));
}

TEST(DXGISharedTest, PostSLLockedQueueMutationOnlyHappensForInitialLockOrExplicitPromotion) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldMutatePostSLLockedQueue(false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldMutatePostSLLockedQueue(true, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldMutatePostSLLockedQueue(true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldMutatePostSLLockedQueue(true, false, false));
}

TEST(DXGISharedTest, PostSLLastWorkingQueueIgnoresTransientWrapperQueues) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldRememberPostSLLastWorkingQueue(false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRememberPostSLLastWorkingQueue(true));
}

TEST(DXGISharedTest, SyntheticPostSLAdvancesDormantStartupOnlyWhenNormalFramePathStops) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, true, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(false, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, true, false, true));
}

TEST(DXGISharedTest, PostSLWrapperBootstrapRequiresDirectPathAndStaysBlockedAfterFSRPhase) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowPostSLWrapperBootstrap(false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowPostSLWrapperBootstrap(false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowPostSLWrapperBootstrap(false, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowPostSLWrapperBootstrap(true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowPostSLWrapperBootstrap(true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowPostSLWrapperBootstrap(true, false, true));
}

TEST(DXGISharedTest, PostSLNoWrapperVirtualBootstrapBlockedDuringActiveStreamlineFG) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(true, false, false, false,
                                                                                       false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(true, false, true, false,
                                                                                       false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(true, false, false, true,
                                                                                       false, false));

    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(true, false, false, true, true,
                                                                                       true));

    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(false, false, false, false,
                                                                                       false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(false, true, false, false,
                                                                                       false, false));
}

TEST(DXGISharedTest, RecentPostSLTeardownActivityRefreshRequiresLiveStreamlineOrPostSLState) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldRefreshRecentPostSLTeardownActivity(true, true, true, false));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldRefreshRecentPostSLTeardownActivity(true, true, false, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldRefreshRecentPostSLTeardownActivity(false, true, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldRefreshRecentPostSLTeardownActivity(true, false, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldRefreshRecentPostSLTeardownActivity(true, true, false, false));
}

TEST(DXGISharedTest, DelayedPostFSRNonFGRecoveryPreservesRealECLOnlyAfterPrimarySettles) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveRealECLForDelayedPostFSRNonFGRecovery(true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRealECLForDelayedPostFSRNonFGRecovery(false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRealECLForDelayedPostFSRNonFGRecovery(true, false));
}

TEST(DXGISharedTest, PostSLActivationWaitsForSafeBootstrapPathAfterFSRPhase) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, true, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, false, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, false, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(false, false, false, false));
}

TEST(DXGISharedTest, PostSLScQueueVirtualSubmitIsDisabledAfterFSRPhase) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUsePostSLScQueueVirtualSubmit(false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLScQueueVirtualSubmit(true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLScQueueVirtualSubmit(false, false));
}

TEST(DXGISharedTest, FSRHistoryLatchesOnlyFromApiOrAuthoritativeRuntimeTraffic) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldLatchFSRFGHistory(true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldLatchFSRFGHistory(false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldLatchFSRFGHistory(false, false));
}

TEST(DXGISharedTest, PostSLReactivatesAfterLifecycleResetEvenIfCallbackStateWasStale) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatPostSLAsReactivated(true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatPostSLAsReactivated(true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatPostSLAsReactivated(true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatPostSLAsReactivated(false, false, true));
}

TEST(DXGISharedTest, PostSLBootstrapsOverlayStateOnlyWhenDormantReactivationOutrunsProcessFrame) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(true, true, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(false, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(true, true, false, true));
}

TEST(DXGISharedTest, PostSLUsesSelectedSwapchainQueueDirectSubmitAfterFSRWhenAvailable) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(true, true, false, true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(false, true, false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(true, false, false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(true, true, true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(true, true, false, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(true, true, false, true, true));
}

TEST(DXGISharedTest, PostSLUsesSelectedNonSwapchainQueueDirectSubmitAfterFSRWhenItAlreadyMatchesRealECL) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedQueueDirectSubmitAfterFSR(true, false, true, true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedQueueDirectSubmitAfterFSR(false, false, true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedQueueDirectSubmitAfterFSR(true, true, true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedQueueDirectSubmitAfterFSR(true, false, false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedQueueDirectSubmitAfterFSR(true, false, true, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedQueueDirectSubmitAfterFSR(true, false, true, true, true));
}

TEST(DXGISharedTest, PostSLPrefersRealQueueBehindWrapperAfterFSRWhenAvailable) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUsePostSLRealQueueBehindWrapperAfterFSR(true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLRealQueueBehindWrapperAfterFSR(false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLRealQueueBehindWrapperAfterFSR(true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLRealQueueBehindWrapperAfterFSR(true, true, false));
}

TEST(DXGISharedTest, PostSLPrefersValidatedDirectQueueForLockAfterFSRWhenAvailable) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreferValidatedDirectQueueForPostFSRLock(true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreferValidatedDirectQueueForPostFSRLock(false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreferValidatedDirectQueueForPostFSRLock(true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreferValidatedDirectQueueForPostFSRLock(true, true, false));
}

TEST(DXGISharedTest, PostSLValidatedDirectQueueCandidateRejectsKnownWrapperShapedQueues) {
    EXPECT_TRUE(ce::dx12_overlay_policy::IsUsableValidatedPostSLDirectQueueCandidate(true, false, false, false, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::IsUsableValidatedPostSLDirectQueueCandidate(false, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsUsableValidatedPostSLDirectQueueCandidate(true, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsUsableValidatedPostSLDirectQueueCandidate(true, false, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsUsableValidatedPostSLDirectQueueCandidate(true, false, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsUsableValidatedPostSLDirectQueueCandidate(true, false, false, false, true));
}

TEST(DXGISharedTest, PostSLUsesWrapperBootstrapQueueAfterFSROnlyWhenDirectPathIsUnavailable) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(true, true, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(false, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(true, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(true, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(true, true, false, false));
}

TEST(DXGISharedTest, PostSLPrefersValidatedCommandQueueWrapperForBootstrapAfterFSR) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(true, true, false, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(false, true, false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(true, false, false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(true, true, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(true, true, false, false));
}

TEST(DXGISharedTest, PostSLPromotesFromWrapperBootstrapToRealQueueAfterFSR) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueBehindWrapperAfterFSR(true, true, true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueBehindWrapperAfterFSR(false, true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueBehindWrapperAfterFSR(true, false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueBehindWrapperAfterFSR(true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueBehindWrapperAfterFSR(true, true, true, true));
}

TEST(DXGISharedTest, PostSLSelectsRealQueueInsteadOfLockedWrapperAfterFSRBootstrapCapture) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSelectPostSLRealQueueBehindWrapperInsteadOfLockedQueueAfterFSR(
        true, true, true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSelectPostSLRealQueueBehindWrapperInsteadOfLockedQueueAfterFSR(
        false, true, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSelectPostSLRealQueueBehindWrapperInsteadOfLockedQueueAfterFSR(
        true, false, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSelectPostSLRealQueueBehindWrapperInsteadOfLockedQueueAfterFSR(
        true, true, false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSelectPostSLRealQueueBehindWrapperInsteadOfLockedQueueAfterFSR(
        true, true, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSelectPostSLRealQueueBehindWrapperInsteadOfLockedQueueAfterFSR(
        true, true, true, true, false));
}

TEST(DXGISharedTest, PostSLSelectsSwapchainQueueInsteadOfLockedWrapperAfterFSRWhenDirectPathExists) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
        true, true, true, true, true, true, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
        false, true, true, true, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
        true, false, true, true, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
        true, true, false, true, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
        true, true, true, false, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
        true, true, true, true, false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
        true, true, true, true, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
        true, true, true, true, true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
        true, true, true, true, true, true, true, true));
}

TEST(DXGISharedTest, PostSLUsesOnlyLevelZeroWrapperProbeToCaptureRealQueueAfterFSR) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueCaptureViaWrapperProbeAfterFSR(
        true, true, 0, false, true, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueCaptureViaWrapperProbeAfterFSR(
        false, true, 0, false, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueCaptureViaWrapperProbeAfterFSR(
        true, false, 0, false, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueCaptureViaWrapperProbeAfterFSR(
        true, true, 1, false, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueCaptureViaWrapperProbeAfterFSR(
        true, true, 0, true, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueCaptureViaWrapperProbeAfterFSR(
        true, true, 0, false, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueCaptureViaWrapperProbeAfterFSR(
        true, true, 0, false, true, true, false));
}

TEST(DXGISharedTest, PostSLPostFSRProbeFallbackDoesNotReuseWrapperWithoutValidatedDirectQueue) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseWrapperQueueForPostFSRProbeFallback(true, 1, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseWrapperQueueForPostFSRProbeFallback(true, 2, true, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseWrapperQueueForPostFSRProbeFallback(true, 1, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseWrapperQueueForPostFSRProbeFallback(true, 1, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseWrapperQueueForPostFSRProbeFallback(true, 0, true, false, true));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUseWrapperQueueForPostFSRProbeFallback(false, 1, true, false, true));
}

TEST(DXGISharedTest, PostSLDoesNotPreferWrapperSubmitAfterFSRWhenScQueuePathIsAvailable) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperSubmitAfterFSR(true, true, true, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperSubmitAfterFSR(false, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperSubmitAfterFSR(true, false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperSubmitAfterFSR(true, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperSubmitAfterFSR(true, true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperSubmitAfterFSR(true, true, true, true, true));
}

TEST(DXGISharedTest, PostSLDoesNotPinWrapperQueueAfterFSR) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPinPostSLWrapperQueueAfterFSR(true, true, true, false, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPinPostSLWrapperQueueAfterFSR(false, true, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPinPostSLWrapperQueueAfterFSR(true, false, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPinPostSLWrapperQueueAfterFSR(true, true, false, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPinPostSLWrapperQueueAfterFSR(true, true, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPinPostSLWrapperQueueAfterFSR(true, true, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPinPostSLWrapperQueueAfterFSR(true, true, true, false, true, true));
}

TEST(DXGISharedTest, PostFSRSwapchainQueuePathUsesExplicitBackbufferTransitions) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUseExplicitBackbufferTransitionsForPostFSRSwapchainQueuePath(
        true, true, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseExplicitBackbufferTransitionsForPostFSRSwapchainQueuePath(
        false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseExplicitBackbufferTransitionsForPostFSRSwapchainQueuePath(
        true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseExplicitBackbufferTransitionsForPostFSRSwapchainQueuePath(
        true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseExplicitBackbufferTransitionsForPostFSRSwapchainQueuePath(
        true, true, true, true));
}

TEST(DXGISharedTest, PostSLBackbufferBarrierModeUsesPresentTransitionsForPostFSRSwapchainQueuePath) {
    using ce::dx12_overlay_policy::DecidePostSLBackbufferBarrierMode;
    using ce::dx12_overlay_policy::PostSLBackbufferBarrierMode;

    EXPECT_EQ(DecidePostSLBackbufferBarrierMode(false, false), PostSLBackbufferBarrierMode::kCommonToRenderTarget);
    EXPECT_EQ(DecidePostSLBackbufferBarrierMode(true, false), PostSLBackbufferBarrierMode::kUavBarrierOnly);
    EXPECT_EQ(DecidePostSLBackbufferBarrierMode(true, true), PostSLBackbufferBarrierMode::kPresentToRenderTarget);
    EXPECT_EQ(DecidePostSLBackbufferBarrierMode(false, true), PostSLBackbufferBarrierMode::kPresentToRenderTarget);
}

TEST(DXGISharedTest, PostFSRSwapchainQueuePathDoesNotUseOffscreenCompositeInPostSL) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCompositeAfterFSR(true, true, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCompositeAfterFSR(false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCompositeAfterFSR(true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCompositeAfterFSR(true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCompositeAfterFSR(true, true, true, true));
}

TEST(DXGISharedTest, PostFSROffscreenCopyOnlyProbeRunsOnlyAtStagedProbeLevel) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCopyOnlyProbeAfterFSR(true, 2, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCopyOnlyProbeAfterFSR(false, 2, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCopyOnlyProbeAfterFSR(true, 1, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCopyOnlyProbeAfterFSR(true, 3, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCopyOnlyProbeAfterFSR(true, 2, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCopyOnlyProbeAfterFSR(true, 2, true, true));
}

TEST(DXGISharedTest, PostFSROffscreenCompositeUsesExplicitBackbufferCopyTransitionsOnSwapchainQueuePath) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldUseExplicitBackbufferCopyTransitionsForPostFSROffscreenComposite(true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseExplicitBackbufferCopyTransitionsForPostFSROffscreenComposite(false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseExplicitBackbufferCopyTransitionsForPostFSROffscreenComposite(true, false));
}

TEST(DXGISharedTest, SyntheticPostSLRefreshesMetricsOnlyWhenNormalFramePathIsDormant) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSyntheticPostSLRefreshMetrics(true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLRefreshMetrics(false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLRefreshMetrics(true, true));
}

TEST(DXGISharedTest, ConfirmedPostSLStaysActiveDuringRemainingFGCooldown) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLDuringFGCooldown(true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLDuringFGCooldown(false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLDuringFGCooldown(true, false));
}

TEST(DXGISharedTest, PostSLOnlyLatchesSuspensionForFullyInactiveSignalDrop) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldLatchPostSLSuspensionOnStreamlineSignalDrop(false, false, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldLatchPostSLSuspensionOnStreamlineSignalDrop(true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldLatchPostSLSuspensionOnStreamlineSignalDrop(false, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldLatchPostSLSuspensionOnStreamlineSignalDrop(false, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldLatchPostSLSuspensionOnStreamlineSignalDrop(false, false, false, true));
}

TEST(DXGISharedTest, FFXSwapchainTakeoverStillClearsStaleStreamlineOwnershipWhenFfxIsAuthoritative) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(true, true, true, false, true));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(true, true, true, true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(false, true, true, false, true));
}

TEST(DXGISharedTest, CreateSwapchainAccessDeniedPassThroughRequiresActiveStreamlineOwnership) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(true, true, false,
                                                                                                   false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(true, false, true,
                                                                                                   false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(false, true, true,
                                                                                                    false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(true, false, false,
                                                                                                    false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(true, true, false,
                                                                                                    true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(true, true, false,
                                                                                                    false, true));
}
