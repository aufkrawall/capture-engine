#include "test_dxgi_shared_shared.h"

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

TEST(DXGISharedTest, PostSLReactivationRestartsConfirmedStartupProgressPerEpoch) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldResetPostSLStartupProgressOnReactivation(true, 0, 0, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldResetPostSLStartupProgressOnReactivation(false, 8, 0, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldResetPostSLStartupProgressOnReactivation(false, 0, 2, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldResetPostSLStartupProgressOnReactivation(false, 0, 0, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldResetPostSLStartupProgressOnReactivation(false, 0, 0, false));
}

TEST(DXGISharedTest, PostSLStaysActiveWithoutRealECLWhenSubmitPathOrProofExists) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldKeepPostSLActiveWhenRealECLUnavailable(true, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldKeepPostSLActiveWhenRealECLUnavailable(false, true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldKeepPostSLActiveWhenRealECLUnavailable(false, false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldKeepPostSLActiveWhenRealECLUnavailable(false, false, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldKeepPostSLActiveWhenRealECLUnavailable(false, false, false, false));
}

TEST(DXGISharedTest, PostSLBootstrapsOverlayStateOnlyWhenDormantReactivationOutrunsProcessFrame) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(true, true, false, false, false, false, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(false, true, false, false, false, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(true, false, false, false, false, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(true, true, true, false, false, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(true, true, false, true, false, false, false));
}

TEST(DXGISharedTest, PostSLBootstrapsOverlayStateDuringHalfArmedSyntheticStartupEvenIfProcessFrameIsRecent) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(true, true, false, true, true, false, false));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(true, true, false, true, false, true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(true, true, false, true, false, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(true, true, false, true, true, false, true));
}

TEST(DXGISharedTest, SceneTransitionCooldownIsSuppressedDuringHalfArmedSyntheticStartup) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownDuringSyntheticPostSLStartup(
        true, true, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownDuringSyntheticPostSLStartup(
        true, false, true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownDuringSyntheticPostSLStartup(
        true, false, false, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownDuringSyntheticPostSLStartup(
        false, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownDuringSyntheticPostSLStartup(
        true, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownDuringSyntheticPostSLStartup(
        true, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownDuringSyntheticPostSLStartup(
        true, false, false, true, false));
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
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(true, true, false, true, false,
                                                                                      false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(false, true, false, true, false,
                                                                                       false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(true, false, false, true, false,
                                                                                       false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(true, true, true, true, false,
                                                                                       false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(true, true, false, false, false,
                                                                                       false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(true, true, false, true, false,
                                                                                       true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(true, true, false, true, true,
                                                                                       false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(true, true, false, true, false,
                                                                                      false, true));
}

TEST(DXGISharedTest, PostSLPrefersValidatedCommandQueueWrapperForBootstrapAfterFSR) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(true, true, false, true,
                                                                                                false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(false, true, false,
                                                                                                 true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(true, false, false,
                                                                                                 true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(true, true, true, true,
                                                                                                 false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(true, true, false,
                                                                                                 false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(true, true, false,
                                                                                                 true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(true, true, false,
                                                                                                 true, false, true));
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

TEST(DXGISharedTest, ConfirmedPostSLBackendWarmupUsesProofThreshold) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLBackendAsWarmupProtected(false, 10));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLBackendAsWarmupProtected(true, 0));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLBackendAsWarmupProtected(true, 1));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLBackendAsWarmupProtected(true, 30));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLBackendAsWarmupProtected(true, 31));
}

TEST(DXGISharedTest, PostFSRConfirmedPostSLBackendSurvivesActiveSwapchainChange) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, true, true, true, true, true, true, false, false, false, false, false));

    // 20260612_002523: the PURE-DLSS startup (no FSR history) must preserve a
    // confirmed PostSL backend too. PostSL confirmation is proof on the LIVE
    // Streamline swapchain; the ordinary reinit + 90-frame cooldown blanked
    // the overlay permanently when zero-ECL classification also starved the
    // cooldown ticks.
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, true, false, true, true, true, true, false, false, false, false, false));

    // 20260715_164211: after thousands of confirmed frames, a DLSS suspend ->
    // warm resume cleared transient swapchain-queue capture. The first wrapper
    // Present was still the exact last successful PostSL swapchain/queue and
    // must not destroy that route or arm a 90-frame cooldown.
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, false, true, false, false, true, false, false, false, true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, false, true, false, false, true, false, false, false, false, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        false, true, true, true, true, true, true, true, false, false, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, false, true, true, true, true, true, true, false, false, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, false, true, false, false, true, false, false, false, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, true, true, false, true, true, true, false, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, true, true, true, false, true, true, false, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, true, true, true, true, false, true, false, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, true, true, true, true, true, false, false, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, false, true, false, false, true, false, true, false, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, false, true, false, false, true, false, false, true, true, true, true));
}

TEST(DXGISharedTest, ArmedFGTransitionCooldownAlwaysTicksDespiteZeroECLClassification) {
    using ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent;
    using ce::fg_runtime::RuntimeMode;

    // 20260612_002523: the game retired its original render queue when
    // entering DLSS FG, so every present classified as zero-ECL and the
    // armed 90-frame cooldown never counted down - PostSL stayed disabled
    // forever. Armed cooldowns must always be allowed to tick.
    EXPECT_FALSE(ShouldSkipProcessFrameForZeroECLPresent(true, false, false, false, true, false, false,
                                                         RuntimeMode::kDLSSFG, false, true));

    // Without an armed cooldown the conservative zero-ECL skip stays.
    EXPECT_TRUE(ShouldSkipProcessFrameForZeroECLPresent(true, false, false, false, true, false, false,
                                                        RuntimeMode::kDLSSFG, false, false));
}

TEST(DXGISharedTest, FreshStreamlineStartupHandoffStaysPendingWhileSyntheticStartupIsHalfArmed) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(
        true, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(
        false, true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(
        false, false, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(
        false, false, true, false));
}

TEST(DXGISharedTest, ActivePostSLStartupAlsoStaysActiveDuringRemainingFGCooldown) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveActivePostSLDuringFGCooldown(true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveActivePostSLDuringFGCooldown(true, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveActivePostSLDuringFGCooldown(true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveActivePostSLDuringFGCooldown(false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveActivePostSLDuringFGCooldown(true, false, false));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveActivePostSLWhenPreSLDrawIsSkipped(true, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveActivePostSLWhenPreSLDrawIsSkipped(true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveActivePostSLWhenPreSLDrawIsSkipped(false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveActivePostSLWhenPreSLDrawIsSkipped(true, false, false));
}

TEST(DXGISharedTest, SyntheticPostSLStartupActivationIsOneShotUntilConfirmation) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldEnterSyntheticPostSLStartupActivation(true, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEnterSyntheticPostSLStartupActivation(false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEnterSyntheticPostSLStartupActivation(true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEnterSyntheticPostSLStartupActivation(true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEnterSyntheticPostSLStartupActivation(false, true, true));
}

TEST(DXGISharedTest, ConfirmedPostSLResumeSeedsStartupBootstrapAsConsumed) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSeedStreamlineStartupBootstrapAsConsumedForConfirmedPostSLResume(
        false, true, true, true, false, false, false));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSeedStreamlineStartupBootstrapAsConsumedForConfirmedPostSLResume(
        false, false, false, false, true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSeedStreamlineStartupBootstrapAsConsumedForConfirmedPostSLResume(
        true, true, true, true, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSeedStreamlineStartupBootstrapAsConsumedForConfirmedPostSLResume(
        false, false, true, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSeedStreamlineStartupBootstrapAsConsumedForConfirmedPostSLResume(
        false, true, false, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSeedStreamlineStartupBootstrapAsConsumedForConfirmedPostSLResume(
        false, true, true, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSeedStreamlineStartupBootstrapAsConsumedForConfirmedPostSLResume(
        false, false, false, false, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSeedStreamlineStartupBootstrapAsConsumedForConfirmedPostSLResume(
        false, false, false, false, true, true, false));
}

TEST(DXGISharedTest, FreshStreamlineHandoffAfterFSRDoesNotClearTheRecapturedSwapchainQueue) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldClearSwapchainQueueAsStaleFSROwnershipOnStreamlineOn(true, true, true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldClearSwapchainQueueAsStaleFSROwnershipOnStreamlineOn(true, true, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldClearSwapchainQueueAsStaleFSROwnershipOnStreamlineOn(false, true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldClearSwapchainQueueAsStaleFSROwnershipOnStreamlineOn(true, false, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldClearSwapchainQueueAsStaleFSROwnershipOnStreamlineOn(true, true, false, false));
}

TEST(DXGISharedTest, StaleFSRQueueClearSkippedForWarmPostSLResume) {
    using ce::dx12_overlay_policy::ShouldClearSwapchainQueueAsStaleFSROwnershipOnStreamlineOn;

    // Cold FSR->DLSS transition (no warm resume): the non-origGame queue is stale
    // FSR ownership and MUST be cleared to prevent DEVICE_REMOVED.
    EXPECT_TRUE(ShouldClearSwapchainQueueAsStaleFSROwnershipOnStreamlineOn(
        /*hadFSR=*/true, /*hasScQueue=*/true, /*scQueueDiffersFromOrig=*/true,
        /*handoffPending=*/false, /*warmPostSLResume=*/false));

    // Session 20260613_151646: a DLSS-FG suspend->resume bridged by the make-before-break
    // keep-alive (warm PostSL resume) is NOT an FSR->DLSS transition — the non-origGame queue
    // is the LIVE DLSS-G proxy PostSL has been submitting on. Clearing it strands the warm
    // resume (scQueue=null + FSR history => "refusing SL wrapper bootstrap" forever). Preserve it.
    EXPECT_FALSE(ShouldClearSwapchainQueueAsStaleFSROwnershipOnStreamlineOn(
        /*hadFSR=*/true, /*hasScQueue=*/true, /*scQueueDiffersFromOrig=*/true,
        /*handoffPending=*/false, /*warmPostSLResume=*/true));
}

TEST(DXGISharedTest, ConfirmedPostSLStartupRoutingProtectsThroughFirstEightFrames) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsStartupSettling(true, 0));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsStartupSettling(true, 1));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsStartupSettling(true, 2));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsStartupSettling(true, 3));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsStartupSettling(true, 4));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsStartupSettling(true, 5));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsStartupSettling(true, 6));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsStartupSettling(true, 7));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsStartupSettling(true, 8));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsStartupSettling(false, 0));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsStartupSettling(true, 9));
}

TEST(DXGISharedTest, ConfirmedPostSLRuntimeStateStabilizationStartsRightAfterSettlingEnds) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(true, 8));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(true, 9));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(true, 10));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(true, 11));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(true, 12));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(true, 13));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(false, 9));
}

TEST(DXGISharedTest, GetStateOffWarmupProtectionExtendsToPostSLProofThreshold) {
    EXPECT_EQ(30, ce::dx12_overlay_policy::GetConfirmedPostSLStaleOffWarmupProtectionLastFrame());
    EXPECT_EQ(30, ce::dx12_overlay_policy::GetConfirmedPostSLGetStateOffWarmupProtectionLastFrame());

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferStaleOffDuringConfirmedPostSLWarmup(true, 8));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferStaleOffDuringConfirmedPostSLWarmup(true, 9));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferStaleOffDuringConfirmedPostSLWarmup(true, 13));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferStaleOffDuringConfirmedPostSLWarmup(true, 30));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferStaleOffDuringConfirmedPostSLWarmup(true, 31));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferStaleOffDuringConfirmedPostSLWarmup(false, 13));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferGetStateOffDuringConfirmedPostSLWarmup(true, 8));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferGetStateOffDuringConfirmedPostSLWarmup(true, 9));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferGetStateOffDuringConfirmedPostSLWarmup(true, 13));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferGetStateOffDuringConfirmedPostSLWarmup(true, 30));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferGetStateOffDuringConfirmedPostSLWarmup(true, 31));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferGetStateOffDuringConfirmedPostSLWarmup(false, 13));
}

TEST(DXGISharedTest, ChurnedPostSLReactivationExtendsRuntimeStateStabilizationToWarmupProofThreshold) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldExtendConfirmedPostSLRuntimeStateStabilizationAfterReactivation(0));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldExtendConfirmedPostSLRuntimeStateStabilizationAfterReactivation(2));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldExtendConfirmedPostSLRuntimeStateStabilizationAfterReactivation(12));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldExtendConfirmedPostSLRuntimeStateStabilizationAfterReactivation(29));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldExtendConfirmedPostSLRuntimeStateStabilizationAfterReactivation(30));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldExtendConfirmedPostSLRuntimeStateStabilizationAfterReactivation(60));

    EXPECT_EQ(30, ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationLastFrame(true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(true, 13, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(true, 30, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(true, 31, true));
}

TEST(DXGISharedTest, ConfirmedStartupSettlingCanStillInvokePostSLWithoutSyntheticBypass) {
    EXPECT_TRUE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, false, false, false, false, false, false, false, true, true));
    EXPECT_TRUE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, true, true, false, true, true, false, false, false, true));
    EXPECT_TRUE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, true, true, false, true, false, true, false, false, true));
    // safePostFSRBootstrapPath is now sufficient; explicitSetOptionsActivation is no longer required
    EXPECT_TRUE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, true, false, false, true, true, false, false, false, true));
    EXPECT_TRUE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, true, false, false, true, false, true, false, false, true));
    EXPECT_TRUE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, false, true, false, false, true, true, true, false, true));
    EXPECT_TRUE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, false, true, false, false, true, false, false, false, true));
    EXPECT_TRUE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, false, false, true, false, true, false, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, true, false, false, false, true, false, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, true, true, true, false, true, false, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, true, true, true, false, false, true, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, false, false, false, false, true, false, false, false, true));

    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        true, false, false, false, false, false, false, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, false, false, false, false, false, false, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, false, false, false, false, false, false, false, true, false));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, false, false, false, false, true, true, true, false, false));
    EXPECT_TRUE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, false, false, false, false, false, false, false, true, true));
}

TEST(DXGISharedTest, ConfirmedStandaloneStreamlinePresentCanStillInvokePostSLOnNormalRouteAfterStartupSettles) {
    EXPECT_TRUE(DXGIShared::ShouldInvokePostSLCallbackForConfirmedStandaloneStreamlinePresentOnNormalRoute(
        false, true, true, true, true, false, false, false));

    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackForConfirmedStandaloneStreamlinePresentOnNormalRoute(
        true, true, true, true, true, false, false, false));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackForConfirmedStandaloneStreamlinePresentOnNormalRoute(
        false, false, true, true, true, false, false, false));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackForConfirmedStandaloneStreamlinePresentOnNormalRoute(
        false, true, false, true, true, false, false, false));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackForConfirmedStandaloneStreamlinePresentOnNormalRoute(
        false, true, true, false, true, false, false, false));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackForConfirmedStandaloneStreamlinePresentOnNormalRoute(
        false, true, true, true, false, false, false, false));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackForConfirmedStandaloneStreamlinePresentOnNormalRoute(
        false, true, true, true, true, true, false, false));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackForConfirmedStandaloneStreamlinePresentOnNormalRoute(
        false, true, true, true, true, false, true, false));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackForConfirmedStandaloneStreamlinePresentOnNormalRoute(
        false, true, true, true, true, false, false, true));
}

TEST(DXGISharedTest, PostFSRConfirmedStandaloneNormalRouteUsesBypassTransport) {
    EXPECT_TRUE(
        DXGIShared::ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(true, true, true, true));

    EXPECT_FALSE(
        DXGIShared::ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(false, true, true, true));
    EXPECT_FALSE(
        DXGIShared::ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(true, false, true, true));
    EXPECT_FALSE(
        DXGIShared::ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(true, true, false, true));
    EXPECT_FALSE(
        DXGIShared::ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(true, true, true, false));
}

TEST(DXGISharedTest, SteamDX12HookRiskExtendsToPostFSRConfirmedStandaloneNormalRoute) {
    EXPECT_TRUE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRConfirmedStandaloneNormalRoute(
        true, true, true, false, false, true, true));

    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRConfirmedStandaloneNormalRoute(
        false, true, true, false, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRConfirmedStandaloneNormalRoute(
        true, false, true, false, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRConfirmedStandaloneNormalRoute(
        true, true, false, false, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRConfirmedStandaloneNormalRoute(
        true, true, true, true, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRConfirmedStandaloneNormalRoute(
        true, true, true, false, true, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRConfirmedStandaloneNormalRoute(
        true, true, true, false, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRConfirmedStandaloneNormalRoute(
        true, true, true, false, false, true, false));
}

TEST(DXGISharedTest, StreamlineStartupHandoffNormalRouteBypassesOnlyForTransportOrThirdPartyRisk) {
    EXPECT_TRUE(
        DXGIShared::ShouldBypassPresentForStreamlineStartupHandoffPresentOnNormalRoute(true, true, true, false));
    EXPECT_TRUE(
        DXGIShared::ShouldBypassPresentForStreamlineStartupHandoffPresentOnNormalRoute(true, true, false, true));

    EXPECT_FALSE(
        DXGIShared::ShouldBypassPresentForStreamlineStartupHandoffPresentOnNormalRoute(false, true, true, false));
    EXPECT_FALSE(
        DXGIShared::ShouldBypassPresentForStreamlineStartupHandoffPresentOnNormalRoute(true, false, true, false));
    EXPECT_FALSE(
        DXGIShared::ShouldBypassPresentForStreamlineStartupHandoffPresentOnNormalRoute(true, true, false, false));
}

TEST(DXGISharedTest, PostFSRStartupHandoffBypassesWhenRecoveredRuntimeQueueIsOnlySafeProof) {
    EXPECT_TRUE(
        DXGIShared::ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(true, true, true, true, false));
    EXPECT_TRUE(
        DXGIShared::ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(true, true, true, false, true));

    EXPECT_FALSE(
        DXGIShared::ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(false, true, true, true, false));
    EXPECT_FALSE(
        DXGIShared::ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(true, false, true, true, false));
    EXPECT_FALSE(
        DXGIShared::ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(true, true, false, true, false));
    EXPECT_FALSE(
        DXGIShared::ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(true, true, true, false, false));
}

TEST(DXGISharedTest, AppThreadPostFSRStreamlineStartupHandoffUsesOverlaylessSLRoute) {
    EXPECT_TRUE(DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
        false, true, false, false, true, true, true, false, true, true, true, true, false, false));

    EXPECT_FALSE(DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
        true, true, false, false, true, true, true, false, true, true, true, true, false, false));
    EXPECT_FALSE(DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
        false, false, false, false, true, true, true, false, true, true, true, true, false, false));
    EXPECT_FALSE(DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
        false, true, true, false, true, true, true, false, true, true, true, true, false, false));
    EXPECT_FALSE(DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
        false, true, false, true, true, true, true, false, true, true, true, true, false, false));
    EXPECT_FALSE(DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
        false, true, false, false, false, true, true, false, true, true, true, true, false, false));
    EXPECT_FALSE(DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
        false, true, false, false, true, false, true, false, true, true, true, true, false, false));
    EXPECT_FALSE(DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
        false, true, false, false, true, true, false, false, true, true, true, true, false, false));
    EXPECT_FALSE(DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
        false, true, false, false, true, true, true, true, true, true, true, true, false, false));
    EXPECT_FALSE(DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
        false, true, false, false, true, true, true, false, false, true, true, true, false, false));
    EXPECT_FALSE(DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
        false, true, false, false, true, true, true, false, true, false, true, true, false, false));
    EXPECT_FALSE(DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
        false, true, false, false, true, true, true, false, true, true, false, true, false, false));
    EXPECT_FALSE(DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
        false, true, false, false, true, true, true, false, true, true, true, false, false, false));
    EXPECT_FALSE(DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
        false, true, false, false, true, true, true, false, true, true, true, true, true, false));
    EXPECT_FALSE(DXGIShared::ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
        false, true, false, false, true, true, true, false, true, true, true, true, false, true));
}

TEST(DXGISharedTest, SyntheticStartupStateStaysHalfArmedUntilConfirmedRender) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(true, false, false, false));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(false, true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(true, true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(false, false, true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(false, false, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(true, false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(false, true, true, false));
}

TEST(DXGISharedTest, ReinitCooldownAlsoPreservesHalfArmedSyntheticStartupState) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(true, false, false, false));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(false, true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(false, false, true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(false, false, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldKeepSyntheticStartupStateUntilConfirmedRender(false, false, true, false));
}
