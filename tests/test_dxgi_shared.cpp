#include <gtest/gtest.h>

#include "../hook/common/dx12_overlay_policy.h"
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

TEST(DXGISharedTest, SteamDX12BypassStaysEnabledUntilStreamlineFGActuallyRuns) {
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, true, false, false, true, ce::fg_runtime::RuntimeMode::kOff, false, false));
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, true, false, false, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false));
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, true, false, false, true, ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion, false, false));
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, true, false, false, true, ce::fg_runtime::RuntimeMode::kDLSSFG, false, false));
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, true, false, false, true, ce::fg_runtime::RuntimeMode::kOff, true, false));
}

TEST(DXGISharedTest, SteamDX12BypassAlsoCoversNvPresentStartupWindow) {
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, true, false, false, false, ce::fg_runtime::RuntimeMode::kOff, false, true));
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, true, false, false, false, ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion, false, true));
}

TEST(DXGISharedTest, SteamDX12BypassRequiresCleanNonWrappedEntryPath) {
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(
        false, true, true, false, false, true, ce::fg_runtime::RuntimeMode::kOff, false, false));
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, false, true, false, false, true, ce::fg_runtime::RuntimeMode::kOff, false, false));
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, false, false, false, true, ce::fg_runtime::RuntimeMode::kOff, false, false));
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, true, true, false, true, ce::fg_runtime::RuntimeMode::kOff, false, false));
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, true, false, true, true, ce::fg_runtime::RuntimeMode::kOff, false, false));
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, true, false, false, false, ce::fg_runtime::RuntimeMode::kOff, false, false));
}

TEST(DXGISharedTest, DX12StartupPresentPassStaysAvailableOnlyForInactiveNonBypassStartup) {
    EXPECT_TRUE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        true, false, false, false, ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_TRUE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        true, false, false, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false));
    EXPECT_TRUE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        true, false, false, false, ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion, false));
}

TEST(DXGISharedTest, DX12StartupPresentPassDisablesWhenRealFGOrBypassOwnsPath) {
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        false, false, false, false, ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        true, true, false, false, ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        true, false, true, false, ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        true, false, false, true, ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        true, false, false, false, ce::fg_runtime::RuntimeMode::kDLSSFG, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        true, false, false, false, ce::fg_runtime::RuntimeMode::kFSRFG, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        true, false, false, false, ce::fg_runtime::RuntimeMode::kOff, true));
}

TEST(DXGISharedTest, StreamlineGeneratedFramePresentUsesSyntheticReentrantRoutingOnlyForDX12FGCallers) {
    EXPECT_TRUE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, true));

    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, false, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, false));
}

TEST(DXGISharedTest, FFXPresentBypassesNormalRoutingOnlyDuringDX12StreamlineStartup) {
    EXPECT_TRUE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(true, true, true));

    EXPECT_FALSE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(true, false, true));
    EXPECT_FALSE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(true, true, false));
}

TEST(DXGISharedTest, PostSLCallbackStaysInstalledOnlyWhileStreamlineStillOwnsPresentPath) {
    EXPECT_TRUE(DXGIShared::ShouldKeepPostSLCallbackInstalledDuringTransition(true));
    EXPECT_FALSE(DXGIShared::ShouldKeepPostSLCallbackInstalledDuringTransition(false));
}

TEST(DXGISharedTest, DX12OverlayWaitPolicySkipsSmoothMotionButKeepsStartupSafety) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(
        false, true, true, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(
        true, false, false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(
        true, false, true, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(
        true, false, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(
        true, true, true, ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(
        true, true, false, ce::fg_runtime::RuntimeMode::kFSRFG));
}

TEST(DXGISharedTest, DX12SwapchainOverlayRoutingUsesFSRSwapchainQueueWhenAvailable) {
    using ce::dx12_overlay_policy::DecideSwapchainOverlayRouting;
    using ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision;

    EXPECT_EQ(DecideSwapchainOverlayRouting(true, false, false, false, true, true),
              SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue);
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, true, false, true, true),
              SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue);
}

TEST(DXGISharedTest, DX12SwapchainOverlayRoutingSkipsOnlyWhenFSRQueueIsUnavailable) {
    using ce::dx12_overlay_policy::DecideSwapchainOverlayRouting;
    using ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision;

    EXPECT_EQ(DecideSwapchainOverlayRouting(true, false, false, false, false, true),
              SwapchainOverlayRoutingDecision::kSkipRuntimeOwnedSwapchainWithoutQueue);
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, true, false, false, true),
              SwapchainOverlayRoutingDecision::kSkipFSRWithoutSwapchainQueue);
}

TEST(DXGISharedTest, DX12SwapchainOverlayRoutingPreservesPostFSRStreamlineTransition) {
    using ce::dx12_overlay_policy::DecideSwapchainOverlayRouting;
    using ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision;

    EXPECT_EQ(DecideSwapchainOverlayRouting(false, true, false, true, true, true),
              SwapchainOverlayRoutingDecision::kUsePostFSRStreamlineQueue);
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, true, false, true, false, true),
              SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue);
}

TEST(DXGISharedTest, DX12OverlayMetricsBindingAlwaysKeepsMetricsBound) {
    const auto realFrameBinding = ce::dx12_overlay_policy::DecideOverlayMetricsBinding(true);
    EXPECT_TRUE(realFrameBinding.bindMetrics);
    EXPECT_TRUE(realFrameBinding.refreshFrameMetadata);

    const auto interpolatedFrameBinding = ce::dx12_overlay_policy::DecideOverlayMetricsBinding(false);
    EXPECT_TRUE(interpolatedFrameBinding.bindMetrics);
    EXPECT_FALSE(interpolatedFrameBinding.refreshFrameMetadata);
}

TEST(DXGISharedTest, SyntheticPostSLAdvancesDormantStartupOnlyWhenNormalFramePathStops) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, true, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(false, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, true, false, true));
}

TEST(DXGISharedTest, PostSLWrapperBootstrapIsBlockedForPureDLSSWithoutDirectPath) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowPostSLWrapperBootstrap(false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowPostSLWrapperBootstrap(false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowPostSLWrapperBootstrap(false, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowPostSLWrapperBootstrap(true, false, false));
}

TEST(DXGISharedTest, PostSLActivationWaitsForSafeBootstrapPathAfterFSRPhase) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, true, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, false, true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, false, true, true));
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

TEST(DXGISharedTest, PostSLUsesSelectedSwapchainQueueDirectSubmitAfterFSRWhenAvailable) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(true, true, false, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(false, true, false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(true, false, false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(true, true, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(true, true, false, false));
}

TEST(DXGISharedTest, SyntheticPostSLRefreshesMetricsOnlyWhenNormalFramePathIsDormant) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSyntheticPostSLRefreshMetrics(true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLRefreshMetrics(false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLRefreshMetrics(true, true));
}

TEST(DXGISharedTest, FFXSwapchainTakeoverForcesEndOfStaleStreamlineOwnership) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(true, false));
}
