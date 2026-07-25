#include "test_dxgi_shared_shared.h"

TEST(DXGISharedTest, GuardedSteamOverlayCallbackStateSuppressesDisabledOrInvalidSteamRenderer) {
    EXPECT_TRUE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForCallbackState(true, false, false, false,
                                                                                           false, false));

    EXPECT_TRUE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForCallbackState(true, true, false, false,
                                                                                           false, false));

    EXPECT_TRUE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForCallbackState(true, true, true, false,
                                                                                           false, true));
    EXPECT_FALSE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForCallbackState(true, true, true, false,
                                                                                            false, false));

    EXPECT_FALSE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForCallbackState(true, true, false, true,
                                                                                            false, true));
    EXPECT_FALSE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForCallbackState(true, true, false, false,
                                                                                            true, true));
    EXPECT_FALSE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForCallbackState(false, true, false, false,
                                                                                            false, true));
}

TEST(DXGISharedTest, SteamNullCallbackRecoveryPrefersDXGIBypassOverDummy) {
    EXPECT_EQ(DXGIShared::SelectSteamNullCallbackRecoveryPatchTarget(true),
              DXGIShared::SteamNullCallbackRecoveryPatchTarget::DXGIBypassPresent);
    EXPECT_EQ(DXGIShared::SelectSteamNullCallbackRecoveryPatchTarget(false),
              DXGIShared::SteamNullCallbackRecoveryPatchTarget::DummyNoPresent);
}

TEST(DXGISharedTest, GuardedSteamOverlayFallbackUsesBypassOnFailureOrMissingBackbufferAdvance) {
    EXPECT_TRUE(DXGIShared::ShouldFallbackGuardedExternalSteamOverlayPresentForResult(true, E_FAIL, false, false));
    EXPECT_FALSE(DXGIShared::ShouldFallbackGuardedExternalSteamOverlayPresentForResult(false, E_FAIL, false, false));

    EXPECT_TRUE(DXGIShared::ShouldFallbackGuardedExternalSteamOverlayPresentForResult(true, S_OK, true, false));
    EXPECT_FALSE(DXGIShared::ShouldFallbackGuardedExternalSteamOverlayPresentForResult(true, S_OK, true, true));
    EXPECT_FALSE(DXGIShared::ShouldFallbackGuardedExternalSteamOverlayPresentForResult(true, S_OK, false, false));
}

TEST(DXGISharedTest, RecursiveExternalOverlayPresentUsesBypassOnlyWhileGuarded) {
    EXPECT_TRUE(DXGIShared::ShouldBypassRecursiveExternalOverlayPresent(true, true));
    EXPECT_FALSE(DXGIShared::ShouldBypassRecursiveExternalOverlayPresent(false, true));
    EXPECT_FALSE(DXGIShared::ShouldBypassRecursiveExternalOverlayPresent(true, false));
}

TEST(DXGISharedTest, StablePostSLGapDoesNotForceSceneCooldown) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownForStablePostSLGap(true, true, true,
                                                                                                 false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownForStablePostSLGap(false, true, true,
                                                                                                  false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownForStablePostSLGap(true, false, true,
                                                                                                  false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownForStablePostSLGap(true, true, false,
                                                                                                  false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownForStablePostSLGap(true, true, true,
                                                                                                  true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownForStablePostSLGap(true, true, true,
                                                                                                  false, true));
}

TEST(DXGISharedTest, SceneTransitionCooldownSuppressedForRuntimeOwnedOverlayRoute) {
    using ce::dx12_overlay_policy::ShouldSuppressSceneTransitionCooldownForRuntimeOwnedOverlayRoute;

    // Any non-normal overlay route makes the scene-gap delta a cadence artifact, so arming
    // is suppressed (session 20260613_202646: phantom gap=1001ms during FSR → 14-present blank).
    EXPECT_TRUE(ShouldSuppressSceneTransitionCooldownForRuntimeOwnedOverlayRoute(
        /*runtimeOwnsSwapchain=*/true, /*fsrFGApiActive=*/false, /*runtimeOwnedNativeFGPresentPath=*/false,
        /*protectedOfficialFFXStartupActive=*/false));
    EXPECT_TRUE(ShouldSuppressSceneTransitionCooldownForRuntimeOwnedOverlayRoute(false, true, false, false));
    EXPECT_TRUE(ShouldSuppressSceneTransitionCooldownForRuntimeOwnedOverlayRoute(false, false, true, false));
    EXPECT_TRUE(ShouldSuppressSceneTransitionCooldownForRuntimeOwnedOverlayRoute(false, false, false, true));

    // Plain normal route (all-FG-off, CE owns the present path): the scene-gap heuristic is
    // valid here, so it is NOT suppressed.
    EXPECT_FALSE(ShouldSuppressSceneTransitionCooldownForRuntimeOwnedOverlayRoute(false, false, false, false));
}

TEST(DXGISharedTest, DLSSOffOnConfirmedPostSLRuntimeOwnedQueueReinitsImmediately) {
    using ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue;

    // Talos menu FG-switch (session 20260614_023730: 89 + 90 presents / 828 ms): slDLSSGSetOptions(OFF)
    // over a runtime-owned swapchain whose FSR-ownership latch is STALE. DLSS-PostSL was the actual
    // presenter (change queue == g_PostSLLastWorkingQueue), the keep-alive could not arm, FSR is not
    // actually presenting (api inactive, callback quiet) -> reinit the warm backend immediately on the
    // same queue instead of the 90-frame cooldown.
    EXPECT_TRUE(ShouldReinitOverlayImmediatelyAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue(
        /*streamlineFGRunning=*/false, /*fsrFGApiActive=*/false, /*nativeFSRInternalNoCallbackComposition=*/false,
        /*ffxPresentCallbackActive=*/false, /*runtimeOwnsSwapchain=*/true,
        /*swapchainQueueIsConfirmedPostSLRenderQueue=*/true, /*deviceRemoved=*/false));

    // A REAL FSR takeover keeps the strict cooldown (the documented GTA crash path).
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue(
        false, /*fsrFGApiActive=*/true, false, false, true, true, false));
    // A live FFX present callback means FSR is actively presenting -> keep the cooldown.
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue(
        false, false, false, /*ffxPresentCallbackActive=*/true, true, true, false));
    // AMD internal no-callback composition is live -> keep the cooldown.
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue(
        false, false, /*nativeFSRInternalNoCallbackComposition=*/true, false, true, true, false));
    // The change queue is NOT the confirmed PostSL render queue -> not proven the DLSS presenter -> cooldown.
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue(
        false, false, false, false, true, /*swapchainQueueIsConfirmedPostSLRenderQueue=*/false, false));
    // Device removed -> keep the cooldown.
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue(
        false, false, false, false, true, true, /*deviceRemoved=*/true));
    // Streamline FG still running (not an OFF) -> the active-FG preserve path owns this, not us.
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue(
        /*streamlineFGRunning=*/true, false, false, false, true, true, false));
    // Not runtime-owned -> the generic non-FG reinit handles it.
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue(
        false, false, false, false, /*runtimeOwnsSwapchain=*/false, true, false));
}

TEST(DXGISharedTest, EagerlyDrawsPreSLOverlayDuringDLSSToggleOnWhenSameQueueAndOptedIn) {
    using ce::dx12_overlay_policy::ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn;

    // Round 4 (Talos DLSS-FG toggle-ON, session 20260614_030417): with the opt-in kill-switch ON,
    // pure DLSS (no FSR history), the runtime not owning the swapchain, the overlay backend warm,
    // and NO separate Streamline queue (present swapchain queue == the game's original queue), keep
    // drawing the live pre-SL overlay so the DLSS-G-init frozen frame still carries the overlay.
    EXPECT_TRUE(ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn(
        /*eagerEnabled=*/true, /*hadFSRFGPhase=*/false, /*runtimeOwnsSwapchain=*/false, /*overlayInit=*/true,
        /*syncInit=*/true, /*swapchainQueueIsOriginalGameQueue=*/true));

    // Kill-switch OFF (default) -> behave exactly as before (suppress).
    EXPECT_FALSE(ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn(
        /*eagerEnabled=*/false, false, false, true, true, true));
    // FSR history present -> a separate SL/FSR queue topology is likely; keep the strict suppression.
    EXPECT_FALSE(
        ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn(true, /*hadFSRFGPhase=*/true, false, true, true, true));
    // Runtime owns the swapchain -> not the same-queue case; keep suppression.
    EXPECT_FALSE(
        ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn(true, false, /*runtimeOwnsSwapchain=*/true, true, true, true));
    // Overlay backend not initialized -> nothing to keep drawing.
    EXPECT_FALSE(
        ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn(true, false, false, /*overlayInit=*/false, true, true));
    // Sync resources not initialized -> nothing to keep drawing.
    EXPECT_FALSE(ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn(true, false, false, true, /*syncInit=*/false, true));
    // A SEPARATE Streamline queue exists (swapchain queue != original game queue) -> a pre-SL ECL
    // on origGame against SL's backbuffers risks the cross-queue DEVICE_HUNG; keep suppression.
    EXPECT_FALSE(ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn(true, false, false, true, true,
                                                                 /*swapchainQueueIsOriginalGameQueue=*/false));
}

TEST(DXGISharedTest, NormalOverlayDeniedDuringDormantProtectedFFXStartup) {
    using ce::dx12_overlay_policy::ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup;

    // GTA session 20260714_142550 proves the strongest former dormant evidence is still unsafe: the staged
    // create queue is AMD's internal presenter, and normal overlay work there strands its completion fence.
    // Signature: (protectedPending, overlayInit, syncInit, hasDistinctStagedTakeoverQueue, directFFX,
    //             callbackActive, sustainedProgress).
    EXPECT_FALSE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(
        /*protectedOfficialFFXStartupPending=*/true, /*overlayInit=*/true, /*syncInit=*/true,
        /*hasDistinctStagedTakeoverQueue=*/true, /*hasDirectFFXApiConfirmation=*/false,
        /*ffxPresentCallbackActive=*/false, /*sustainedGameProgress=*/true));

    // No distinct staged takeover queue → no live-swapchain creation queue to submit on → quiesce.
    EXPECT_FALSE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(
        true, true, true, /*hasStaged=*/false, false, false, true));
    // Enabled ffxConfigure landed → AMD no longer dormant; hand to the FFX callback route.
    EXPECT_FALSE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(true, true, true, true,
                                                                                      /*directFFX=*/true, false, true));
    // An FFX present callback is firing → AMD active; quiesce the normal route.
    EXPECT_FALSE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(true, true, true, true, false,
                                                                                      /*callbackActive=*/true, true));
    // Not yet enough stable frames (protects the fragile AMD swapchain-create instant) → quiesce.
    EXPECT_FALSE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(
        true, true, true, true, false, false, /*sustainedProgress=*/false));
    // Overlay backend not live → nothing to draw (the bootstrap wrappers substitute this true).
    EXPECT_FALSE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(true, /*overlayInit=*/false, true,
                                                                                      true, false, false, true));
    EXPECT_FALSE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(true, true, /*syncInit=*/false,
                                                                                      true, false, false, true));
    // Not in a protected-FFX startup window → predicate is inert.
    EXPECT_FALSE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(
        /*protectedPending=*/false, true, true, true, false, false, true));
}

TEST(DXGISharedTest, ProtectedFFXStartupUsesProxyBackbufferOnlyUntilDirectProof) {
    using ce::dx12_overlay_policy::ShouldUseProtectedOfficialFFXStartupProxyBackbufferRoute;

    EXPECT_TRUE(ShouldUseProtectedOfficialFFXStartupProxyBackbufferRoute(
        /*protectedPending=*/true, /*startupResolved=*/false, /*proxyHookInstalled=*/true));
    EXPECT_FALSE(ShouldUseProtectedOfficialFFXStartupProxyBackbufferRoute(false, false, true));
    EXPECT_FALSE(ShouldUseProtectedOfficialFFXStartupProxyBackbufferRoute(true, true, true));
    EXPECT_FALSE(ShouldUseProtectedOfficialFFXStartupProxyBackbufferRoute(true, false, false));
}

TEST(DXGISharedTest, SLPresentRoutingStaysDisabledAcrossNativeFGTeardownAndActiveOwnership) {
    EXPECT_TRUE(DXGIShared::ShouldKeepSLPresentRoutingDisabledForNativeFG(true, false));
    EXPECT_TRUE(DXGIShared::ShouldKeepSLPresentRoutingDisabledForNativeFG(false, true));
    EXPECT_TRUE(DXGIShared::ShouldKeepSLPresentRoutingDisabledForNativeFG(true, true));

    EXPECT_FALSE(DXGIShared::ShouldKeepSLPresentRoutingDisabledForNativeFG(false, false));
}

TEST(DXGISharedTest, SLPresentRoutingStaysDisabledForStreamlineNoFGAndNativeFSR) {
    using ce::fg_runtime::RuntimeMode;

    EXPECT_TRUE(DXGIShared::ShouldKeepSLPresentRoutingDisabledForRuntimeState(RuntimeMode::kStreamlineNoFG, false));
    EXPECT_TRUE(DXGIShared::ShouldKeepSLPresentRoutingDisabledForRuntimeState(RuntimeMode::kFSRFG, false));
    EXPECT_TRUE(DXGIShared::ShouldKeepSLPresentRoutingDisabledForRuntimeState(RuntimeMode::kDLSSFG, true));

    EXPECT_FALSE(DXGIShared::ShouldKeepSLPresentRoutingDisabledForRuntimeState(RuntimeMode::kDLSSFG, false));
    EXPECT_FALSE(DXGIShared::ShouldKeepSLPresentRoutingDisabledForRuntimeState(RuntimeMode::kOff, false));
}

TEST(DXGISharedTest, SteamDX12HookRiskExtendsToProtectedPostFSRStartupHandoff) {
    EXPECT_TRUE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupHandoff(true, true, true, false,
                                                                                                false, true, true));

    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupHandoff(
        false, true, true, false, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupHandoff(
        true, false, true, false, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupHandoff(
        true, true, false, false, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupHandoff(true, true, true, true,
                                                                                                 false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupHandoff(
        true, true, true, false, true, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupHandoff(
        true, true, true, false, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupHandoff(
        true, true, true, false, false, true, false));
}

TEST(DXGISharedTest, DX12StartupPresentPassStaysAvailableOnlyForInactiveNonBypassStartup) {
    EXPECT_TRUE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, false, true,
                                                                      ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_TRUE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        true, false, false, false, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false));
    EXPECT_TRUE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        true, false, false, false, true, ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion, false));
}

TEST(DXGISharedTest, DX12StartupPresentPassDisablesWhenRealFGOrBypassOwnsPath) {
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(false, false, false, false, true,
                                                                       ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, true, false, false, true,
                                                                       ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, true, false, true,
                                                                       ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, true, true,
                                                                       ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, false, true,
                                                                       ce::fg_runtime::RuntimeMode::kDLSSFG, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, false, true,
                                                                       ce::fg_runtime::RuntimeMode::kFSRFG, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, false, true,
                                                                       ce::fg_runtime::RuntimeMode::kOff, true));
}

TEST(DXGISharedTest, DX12StartupPresentPassStaysDisabledWhenSteamBypassAlreadyOwnsStartupPath) {
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, true, true,
                                                                       ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        true, false, false, true, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false));
}

TEST(DXGISharedTest, DX12StartupPresentPassDisablesWhenNoBypassAvailableForVtableHookPath) {
    // Regression: vtable-hook path (no inline trampoline) with no bypass
    // trampoline available must NOT allow the startup compat pass, otherwise
    // CallOriginalPresent would route through the external overlay's E9 JMP
    // causing a null-pointer crash (RIP=0).
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, false, false,
                                                                       ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        true, false, false, false, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false));
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(
        true, false, false, false, false, ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion, false));
    // When bypass IS available, the pass should still work (no regression)
    EXPECT_TRUE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, false, true,
                                                                      ce::fg_runtime::RuntimeMode::kOff, false));
}

TEST(DXGISharedTest, GlobalCreateSwapchainPathsCaptureQueueWhenSkippingWrapForStreamline) {
    EXPECT_TRUE(DXGIShared::ShouldCaptureQueueWhenSkippingWrapForStreamline(true));
    EXPECT_FALSE(DXGIShared::ShouldCaptureQueueWhenSkippingWrapForStreamline(false));
}

TEST(DXGISharedTest, GlobalCreateSwapchainForHwndSkipsDuplicateSideEffectsAfterInlineForward) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipGlobalCreateSwapchainForHwndSideEffectsAfterInlineForward(true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipGlobalCreateSwapchainForHwndSideEffectsAfterInlineForward(false));
}

TEST(DXGISharedTest, RuntimeOwnedSwapchainQueuesStayUnhookedUnlessTheyAreOriginalGameQueue) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldHookSwapchainQueueVTableForFrameGenerationRuntime(false, false, false, false));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldHookSwapchainQueueVTableForFrameGenerationRuntime(true, true, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldHookSwapchainQueueVTableForFrameGenerationRuntime(true, false, true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldHookSwapchainQueueVTableForFrameGenerationRuntime(true, false, false, false));
}

TEST(DXGISharedTest, FrameGenerationRuntimeModuleQueuesStayUnpatched) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipCommandQueueVTableHookForFrameGenerationRuntimeModule(true, false,
                                                                                                         false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipCommandQueueVTableHookForFrameGenerationRuntimeModule(false, true,
                                                                                                         false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipCommandQueueVTableHookForFrameGenerationRuntimeModule(false, false,
                                                                                                         true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipCommandQueueVTableHookForFrameGenerationRuntimeModule(false, false,
                                                                                                         false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipCommandQueueVTableHookForFrameGenerationRuntimeModule(
        false, false, false, false));
}

TEST(DXGISharedTest, PostFSRStreamlineRuntimeHandoffDefersPresentVTableRefresh) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(
        true, false, true, true, false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(
        true, false, true, false, true, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(
        true, false, true, false, false, ce::fg_runtime::RuntimeMode::kFSRFG));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(
        false, false, true, true, false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(
        true, true, true, true, false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(
        true, false, false, true, false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(
        true, false, true, false, false, ce::fg_runtime::RuntimeMode::kDLSSFG));
}

TEST(DXGISharedTest, StreamlineRuntimeQueueAuthorityIsSeparateFromFreshHandoffState) {
    const bool authoritativeRuntimeQueue =
        ce::dx12_overlay_policy::ShouldTreatSwapchainQueueAsAuthoritativeStreamlineRuntime(true, true, false);
    EXPECT_TRUE(authoritativeRuntimeQueue);
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldHookSwapchainQueueVTableForFrameGenerationRuntime(
        true, false, authoritativeRuntimeQueue, false));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldArmStreamlineStartupTransitionWindowForFreshAuthoritativeRuntimeQueue(
        authoritativeRuntimeQueue, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldArmStreamlineStartupTransitionWindowForFreshAuthoritativeRuntimeQueue(
        authoritativeRuntimeQueue, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTreatSwapchainQueueAsAuthoritativeStreamlineRuntime(false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTreatSwapchainQueueAsAuthoritativeStreamlineRuntime(true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatSwapchainQueueAsAuthoritativeStreamlineRuntime(true, true, true));
}

TEST(DXGISharedTest, FreshStreamlineHandoffInvalidatesPostSLProofFromPreviousQueueEpoch) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldInvalidateConfirmedPostSLForFreshAuthoritativeStreamlineHandoff(
        true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInvalidateConfirmedPostSLForFreshAuthoritativeStreamlineHandoff(
        false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInvalidateConfirmedPostSLForFreshAuthoritativeStreamlineHandoff(
        true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInvalidateConfirmedPostSLForFreshAuthoritativeStreamlineHandoff(
        true, true, true));

    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldClearPostSLQueueProofForFreshAuthoritativeStreamlineHandoff(true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldClearPostSLQueueProofForFreshAuthoritativeStreamlineHandoff(true, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldClearPostSLQueueProofForFreshAuthoritativeStreamlineHandoff(true, false, false));
}

TEST(DXGISharedTest, StreamlinePresentRoutingRequiresResolvedStreamlineJumpTarget) {
    EXPECT_TRUE(DXGIShared::ShouldActivateStreamlinePresentRoutingForHookTarget(true, true, true, false));

    EXPECT_FALSE(DXGIShared::ShouldActivateStreamlinePresentRoutingForHookTarget(false, true, true, false));
    EXPECT_FALSE(DXGIShared::ShouldActivateStreamlinePresentRoutingForHookTarget(true, false, true, false));
    EXPECT_FALSE(DXGIShared::ShouldActivateStreamlinePresentRoutingForHookTarget(true, true, false, false));
    EXPECT_FALSE(DXGIShared::ShouldActivateStreamlinePresentRoutingForHookTarget(true, true, true, true));
}

TEST(DXGISharedTest, VTableRepairDefersDuringFreshStreamlineStartupEvenWithOlderConfirmation) {
    EXPECT_TRUE(DXGIShared::ShouldDeferVTableRepairDuringStreamlineStartup(true, true, false, true));
    EXPECT_TRUE(DXGIShared::ShouldDeferVTableRepairDuringStreamlineStartup(true, false, true, true));
    EXPECT_TRUE(DXGIShared::ShouldDeferVTableRepairDuringStreamlineStartup(true, false, false, false));

    EXPECT_FALSE(DXGIShared::ShouldDeferVTableRepairDuringStreamlineStartup(false, true, true, true));
    EXPECT_FALSE(DXGIShared::ShouldDeferVTableRepairDuringStreamlineStartup(true, false, false, true));
}

TEST(DXGISharedTest, AuthoritativeFFXQueueStaysSeparateFromStreamlineRuntimeAuthority) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatSwapchainQueueAsAuthoritativeFFXRuntime(true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatSwapchainQueueAsAuthoritativeFFXRuntime(false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatSwapchainQueueAsAuthoritativeFFXRuntime(true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatSwapchainQueueAsAuthoritativeFFXRuntime(true, true, true));
}

TEST(DXGISharedTest, AuthoritativeFFXQueueOverridesStaleStreamlineQueueHookability) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldHookSwapchainQueueVTableForFrameGenerationRuntime(true, false, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldHookSwapchainQueueVTableForFrameGenerationRuntime(true, false, false, true));
}

TEST(DXGISharedTest, AuthoritativeNativeFSRSkipsSeparateOverlayWorkEvenBeforeOwnershipLatch) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        false, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        false, false, ce::fg_runtime::RuntimeMode::kOff, true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        false, false, ce::fg_runtime::RuntimeMode::kOff, false, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        false, true, ce::fg_runtime::RuntimeMode::kFSRFG, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false, false));
}

TEST(DXGISharedTest, ExtendingStartupTransitionWindowDoesNotResetConsumedTopLevelBootstrap) {
    DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.streamlineStartupTransitionUntilMs.store(0, std::memory_order_release);

    DXGIShared::ArmStreamlineStartupTransitionWindow(10);
    EXPECT_FALSE(DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire));

    DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.store(true, std::memory_order_release);
    const ULONGLONG beforeExtend =
        DXGIShared::g_SharedState.streamlineStartupTransitionUntilMs.load(std::memory_order_acquire);

    DXGIShared::ExtendStreamlineStartupTransitionWindow();

    EXPECT_TRUE(DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire));
    EXPECT_GE(DXGIShared::g_SharedState.streamlineStartupTransitionUntilMs.load(std::memory_order_acquire),
              beforeExtend);

    DXGIShared::ClearStreamlineStartupTransitionWindow();
    EXPECT_TRUE(DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire));

    DXGIShared::ResetStreamlineStartupTransitionState();
    EXPECT_FALSE(DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire));
}

TEST(DXGISharedTest, StreamlineGeneratedFramePresentUsesSyntheticReentrantRoutingOnlyForDX12FGCallers) {
    EXPECT_TRUE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, true, false, false, false,
                                                                             false, false, true, false));

    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(false, true, true, false, false, false,
                                                                              false, false, true, false));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, false, true, false, false, false,
                                                                              false, false, true, false));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, false, false, false, false,
                                                                              false, false, true, false));
}

TEST(DXGISharedTest, FFXPresentBypassesNormalRoutingOnlyDuringDX12StreamlineStartup) {
    EXPECT_TRUE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(true, true, true, false, false, false));
    EXPECT_TRUE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(true, true, false, true, false, false));

    EXPECT_FALSE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(false, true, true, false, false, false));
    EXPECT_FALSE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(true, false, true, true, false, false));
    EXPECT_FALSE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(true, true, false, false, false, false));
    EXPECT_FALSE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(true, true, true, true, true, false));
}

TEST(DXGISharedTest, ObserverStartupPresentOnlyReenablesFfxStartupBypassWithoutFullPostSLPath) {
    EXPECT_TRUE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(true, true, true, false, true, true));
    EXPECT_TRUE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(true, true, false, true, true, true));

    EXPECT_FALSE(DXGIShared::ShouldBypassFFXPresentDuringStreamlineStartup(true, true, true, false, true, false));
}

TEST(DXGISharedTest, ObserverModesKeepSpecialStreamlinePresentRoutingPassive) {
    EXPECT_TRUE(DXGIShared::ShouldAllowSpecialStreamlinePresentRouting(false));

    EXPECT_FALSE(DXGIShared::ShouldAllowSpecialStreamlinePresentRouting(true));
}

TEST(DXGISharedTest, WrapperBackedSyntheticStartupPresentCanStayOnNormalRouteInActiveMode) {
    EXPECT_TRUE(DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(false, false, false, false, true,
                                                                                     true, true, false, false, true));
    EXPECT_TRUE(DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(false, false, false, false, true,
                                                                                     true, false, true, false, true));
    EXPECT_TRUE(DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(false, false, false, false, true,
                                                                                     true, false, false, true, true));
    EXPECT_TRUE(DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(false, true, true, false, false,
                                                                                     true, true, false, false, true));
    EXPECT_TRUE(DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(false, true, true, false, false,
                                                                                     true, false, true, false, true));
    EXPECT_TRUE(DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(false, true, false, true, false,
                                                                                     true, true, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(false, true, false, false, false,
                                                                                      true, true, false, false, true));

    EXPECT_FALSE(DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(true, false, false, false, true,
                                                                                      true, true, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(false, false, false, false, false,
                                                                                      false, true, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(false, false, false, false, true,
                                                                                      true, false, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(false, false, false, false, true,
                                                                                      true, false, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(
        false, false, false, false, true, true, false, false, false, false));
    EXPECT_FALSE(DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(false, false, false, false, true,
                                                                                      true, false, true, false, false));
    EXPECT_FALSE(DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(false, true, false, false, false,
                                                                                      true, false, false, false, true));
}

TEST(DXGISharedTest, PostFSRStartupNormalRouteUsesBypassUntilPostSLSettles) {
    EXPECT_TRUE(DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(true, true, false,
                                                                                                 false, false, true));
    EXPECT_TRUE(DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(true, true, true, true,
                                                                                                 false, true));

    EXPECT_FALSE(DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(false, true, false,
                                                                                                  false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(true, false, false,
                                                                                                  false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(true, true, true,
                                                                                                  false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(true, true, false,
                                                                                                  false, false, false));
    EXPECT_TRUE(DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(true, true, false,
                                                                                                 false, true, false));
}

TEST(DXGISharedTest, RuntimeOwnedPureDLSSStartupNormalRouteAllowsActivationWithoutThirdPartyRisk) {
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(true, false, false, true, true,
                                                                                      true, true, true, false, false));
    EXPECT_FALSE(DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(true, true, false,
                                                                                                  false, false, false));

    EXPECT_TRUE(DXGIShared::ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(true, false, false, true, true,
                                                                                     true, true, true, false, true));
    EXPECT_TRUE(DXGIShared::ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(true, false, false, true, true,
                                                                                     true, true, true, true, false));
    EXPECT_TRUE(DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(true, true, false,
                                                                                                 false, true, false));
    EXPECT_TRUE(DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(true, true, true, true,
                                                                                                 true, false));

    EXPECT_FALSE(DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(true, true, true,
                                                                                                  false, true, false));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(true, true, false, true, true,
                                                                                      true, true, true, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(true, false, true, true, true,
                                                                                      true, true, true, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(true, false, false, false, true,
                                                                                      true, true, true, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(true, false, false, true, true,
                                                                                      true, false, true, true, true));
}

TEST(DXGISharedTest, SteamDX12HookRiskExtendsToProtectedPostFSRStartupNormalRoute) {
    EXPECT_TRUE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupNormalRoute(
        true, true, true, false, false, true, true));

    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupNormalRoute(
        false, true, true, false, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupNormalRoute(
        true, false, true, false, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupNormalRoute(
        true, true, false, false, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupNormalRoute(
        true, true, true, true, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupNormalRoute(
        true, true, true, false, true, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupNormalRoute(
        true, true, true, false, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupNormalRoute(
        true, true, true, false, false, true, false));
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

TEST(DXGISharedTest, DX12OverlayWaitPolicyPacesSingleQueueFocusLoss) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(true, false, false,
                                                                        ce::fg_runtime::RuntimeMode::kOff, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(
        true, false, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(
        true, false, false, ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion, false));
}

TEST(DXGISharedTest, D3D12PresentPathsFlushDeferredOverlaySignalAfterPresent) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldFlushDeferredOverlaySignalAfterPresent(true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldFlushDeferredOverlaySignalAfterPresent(false));
}

TEST(DXGISharedTest, EarlyDX12TempSwapchainHookInstallDefersForStartupOverlayBeforeRealDevice) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferEarlyDX12TempSwapchainPresentHookInstall(false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferEarlyDX12TempSwapchainPresentHookInstall(true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferEarlyDX12TempSwapchainPresentHookInstall(false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferEarlyDX12TempSwapchainPresentHookInstall(true, false));
}

// Regression: 64-bit DX12 test app overlay missing when injection wait window
// allowed the game to create its swapchain before hooks installed, AND a
// third-party overlay (e.g. nvspcap64.dll) deferred the eager temp-swapchain
// Present hook install.  The deferred Present hooks were never reinstalled
// because FindAndWrapPreExistingSwapchains() was a no-op and the
// CreateSwapChainForHwnd detours never fired for the pre-existing swapchain.
// After the fix, FindAndWrapPreExistingSwapchains() detects missing Present
// hooks and retries the temp-swapchain installation.  Validate that the
// detection API returns correct initial state.
TEST(DXGISharedTest, FindAndWrapPreExistingSwapchainsCanDetectMissingPresentHooks) {
    // HasPresentInlineHooks/DetourHooks uses global state that is only
    // populated by InstallPresentInlineHooks (requires real D3D12 device).
    // Verify the global state safely returns false before any hook install.
    EXPECT_FALSE(DXGIShared::HasPresentInlineHooks());
    EXPECT_FALSE(DXGIShared::HasPresentDetourHooks());
}

TEST(DXGISharedTest, StartupOverlayCompatibilityModeDependsOnObservedOverlayStateNotProcessName) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUseStartupOverlayCompatibilityMode(true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseStartupOverlayCompatibilityMode(true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseStartupOverlayCompatibilityMode(true, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseStartupOverlayCompatibilityMode(false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseStartupOverlayCompatibilityMode(true, true, false, false));
}

TEST(DXGISharedTest, StartupOverlayCompatibilityStaysActiveThroughLatePreFGRuntimeOwnedHandoff) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUseStartupOverlayCompatibilityMode(true, false, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseStartupOverlayCompatibilityMode(true, false, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseStartupOverlayCompatibilityMode(true, false, true, true, true));
}

TEST(DXGISharedTest, StartupOverlayCompatibilityCanRearmForLateRuntimeOwnedStartupHandoffBeforeAnyFG) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldRearmStartupOverlayCompatibilityForLateRuntimeOwnedSwapchain(
        true, false, true, true, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRearmStartupOverlayCompatibilityForLateRuntimeOwnedSwapchain(
        true, false, true, true, false, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRearmStartupOverlayCompatibilityForLateRuntimeOwnedSwapchain(
        true, false, true, true, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRearmStartupOverlayCompatibilityForLateRuntimeOwnedSwapchain(
        false, false, true, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRearmStartupOverlayCompatibilityForLateRuntimeOwnedSwapchain(
        true, true, true, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRearmStartupOverlayCompatibilityForLateRuntimeOwnedSwapchain(
        true, false, false, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRearmStartupOverlayCompatibilityForLateRuntimeOwnedSwapchain(
        true, false, true, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRearmStartupOverlayCompatibilityForLateRuntimeOwnedSwapchain(
        true, false, true, true, true, true));
}

TEST(DXGISharedTest, StartupOverlayRenderingRequiresStableNonRuntimeQueue) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(true, true, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(false, false, true));
}

TEST(DXGISharedTest, StartupOverlayRenderingAllowsSettledRuntimeOwnedQueueAfterLatePreFGHandoff) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(true, true, true, 99, 100));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(true, true, true, 100, 100));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(true, true, true, 250, 100));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(true, true, true, 250, 0));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(true, true, true, 1, 100, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(true, true, true, 1, 100, false, true));
}

TEST(DXGISharedTest, StartupOverlayResumeDefersOnlyForShortRuntimeOwnedQueueHandoff) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferStartupOverlayWorkAfterResume(true, true, 50, 100));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferStartupOverlayWorkAfterResume(true, true, 100, 100));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferStartupOverlayWorkAfterResume(true, false, 50, 100));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferStartupOverlayWorkAfterResume(false, true, 50, 100));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferStartupOverlayWorkAfterResume(true, true, 50, 100, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferStartupOverlayWorkAfterResume(true, true, 50, 100, false, true));
}

TEST(DXGISharedTest, SettledStartupOverlayStaysVisibleAcrossRuntimeInactiveStreamlineHandoff) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        true, true, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        false, true, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        true, false, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        true, true, true, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        true, true, true, false, ce::fg_runtime::RuntimeMode::kDLSSFG, false, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        true, true, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        true, true, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        true, true, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        true, true, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false, false, false));
}

TEST(DXGISharedTest, LiveStartupOverlayHandoffSkipsResourcePrimingBlank) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPrimeStartupOverlayResources(true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPrimeStartupOverlayResources(true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPrimeStartupOverlayResources(true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPrimeStartupOverlayResources(false, true, false));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDelayAfterStartupOverlayResourcePrime(true, false, 50, 100, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDelayAfterStartupOverlayResourcePrime(true, false, 50, 100, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDelayAfterStartupOverlayResourcePrime(true, true, 50, 100, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDelayAfterStartupOverlayResourcePrime(true, false, 100, 100, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDelayAfterStartupOverlayResourcePrime(false, false, 50, 100, false));
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

