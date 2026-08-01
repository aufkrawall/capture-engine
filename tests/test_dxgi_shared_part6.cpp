#include "test_dxgi_shared_shared.h"

TEST(DXGISharedTest, CleanNonFGSwapchainChangeResetsQueueHeuristicOnlyWhenEndingPostFSRRecovery) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldResetQueueChangeHeuristicAfterCleanNonFGSwapchainChange(true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldResetQueueChangeHeuristicAfterCleanNonFGSwapchainChange(false));
}

TEST(DXGISharedTest, ExplicitSwapchainQueueProofEndsPostFSRRecoveryOnlyWhenOwnershipReturnsToOriginalQueue) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldEndPostFSRNonFGRecoveryOnExplicitSwapchainQueueProof(true, true, true,
                                                                                                    true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEndPostFSRNonFGRecoveryOnExplicitSwapchainQueueProof(false, true, true,
                                                                                                     true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEndPostFSRNonFGRecoveryOnExplicitSwapchainQueueProof(true, false, true,
                                                                                                     true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEndPostFSRNonFGRecoveryOnExplicitSwapchainQueueProof(true, true, false,
                                                                                                     true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEndPostFSRNonFGRecoveryOnExplicitSwapchainQueueProof(true, true, true,
                                                                                                     false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEndPostFSRNonFGRecoveryOnExplicitSwapchainQueueProof(true, true, true,
                                                                                                     true, false));
}

TEST(DXGISharedTest, PostFSRNormalRouteRequiresQueueOrExactRememberedSwapchainOwnershipProof) {
    EXPECT_TRUE(ce::dx12_overlay_policy::IsPostFSRNormalRouteOwnershipProven(true, true, true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::IsPostFSRNormalRouteOwnershipProven(false, true, false, false, true));

    // GTA can rotate FFX proxy -> existing Streamline proxy while every FG
    // signal is already off. Pointer change alone must not authorize the
    // original queue for that runtime proxy's backbuffer.
    EXPECT_FALSE(ce::dx12_overlay_policy::IsPostFSRNormalRouteOwnershipProven(false, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsPostFSRNormalRouteOwnershipProven(true, true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsPostFSRNormalRouteOwnershipProven(true, false, true, true, true));
}

TEST(DXGISharedTest, GTAProxyRotationKeepsExactConfirmedPostSLRouteUntilNormalOwnershipIsProven) {
    using Route = ce::dx12_overlay_policy::InactiveDLSSPresentRoute;

    EXPECT_EQ(
        ce::dx12_overlay_policy::DecideInactiveDLSSPresentRoute(true, false, false, false, true, true, true, true),
        Route::kConfirmedPostSLKeepAlive);
    EXPECT_EQ(
        ce::dx12_overlay_policy::DecideInactiveDLSSPresentRoute(true, false, false, true, true, true, true, false),
        Route::kNormal);
    EXPECT_EQ(ce::dx12_overlay_policy::DecideInactiveDLSSPresentRoute(true, true, false, false, true, true, true, true),
              Route::kNormal);
    EXPECT_EQ(ce::dx12_overlay_policy::DecideInactiveDLSSPresentRoute(true, false, true, false, true, true, true, true),
              Route::kNormal);

    EXPECT_EQ(
        ce::dx12_overlay_policy::DecideInactiveDLSSPresentRoute(true, false, false, false, true, true, true, false),
        Route::kAwaitNormalOwnershipProof);
    EXPECT_EQ(
        ce::dx12_overlay_policy::DecideInactiveDLSSPresentRoute(true, false, false, false, true, false, true, true),
        Route::kAwaitNormalOwnershipProof);
    EXPECT_EQ(
        ce::dx12_overlay_policy::DecideInactiveDLSSPresentRoute(true, false, false, false, true, true, false, true),
        Route::kAwaitNormalOwnershipProof);
}

TEST(DXGISharedTest, ExplicitOffPostSLKeepAliveRejectsEverySwapchainExceptItsLastSuccessfulIdentity) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldRejectPostSLKeepAliveRenderForUnprovenSwapchain(true, false, true, true));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldRejectPostSLKeepAliveRenderForUnprovenSwapchain(true, false, true, false));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldRejectPostSLKeepAliveRenderForUnprovenSwapchain(true, false, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldRejectPostSLKeepAliveRenderForUnprovenSwapchain(true, true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldRejectPostSLKeepAliveRenderForUnprovenSwapchain(false, false, true, false));
}

TEST(DXGISharedTest, ExactPostSLOffKeepAliveCoversEveryDlssSuspendEntryButNeverNativeFSROwnership) {
    using ce::dx12_overlay_policy::ShouldDriveExactPostSLOffKeepAliveBeforePresent;

    // OFF->DLSS->suspend, FSR->DLSS->suspend, and DLSS->all-FG-off all
    // converge on the same exact-proxy invariant. FSR history is deliberately
    // irrelevant once PostSL has proved the current proxy and queue.
    {
        SCOPED_TRACE("all FG off -> DLSS -> suspended/off");
        EXPECT_TRUE(ShouldDriveExactPostSLOffKeepAliveBeforePresent(true, false, false, false, false, true, true, true,
                                                                    true, true));
    }
    {
        SCOPED_TRACE("FSR active/off -> DLSS -> suspended/off");
        EXPECT_TRUE(ShouldDriveExactPostSLOffKeepAliveBeforePresent(true, false, false, false, false, true, true, true,
                                                                    true, true));
    }

    // Active DLSS uses its ordinary callback; either active/suspended FSR route
    // owns composition and must keep separate PostSL work GPU-quiet.
    EXPECT_FALSE(
        ShouldDriveExactPostSLOffKeepAliveBeforePresent(true, true, false, false, false, true, true, true, true, true));
    EXPECT_FALSE(
        ShouldDriveExactPostSLOffKeepAliveBeforePresent(true, false, true, false, false, true, true, true, true, true));
    EXPECT_FALSE(
        ShouldDriveExactPostSLOffKeepAliveBeforePresent(true, false, false, true, false, true, true, true, true, true));
    EXPECT_FALSE(
        ShouldDriveExactPostSLOffKeepAliveBeforePresent(true, false, false, false, true, true, true, true, true, true));

    EXPECT_FALSE(ShouldDriveExactPostSLOffKeepAliveBeforePresent(false, false, false, false, false, true, true, true,
                                                                 true, true));
    EXPECT_FALSE(ShouldDriveExactPostSLOffKeepAliveBeforePresent(true, false, false, false, false, false, true, true,
                                                                 true, true));
    EXPECT_FALSE(ShouldDriveExactPostSLOffKeepAliveBeforePresent(true, false, false, false, false, true, false, true,
                                                                 true, true));
    EXPECT_FALSE(ShouldDriveExactPostSLOffKeepAliveBeforePresent(true, false, false, false, false, true, true, false,
                                                                 true, true));
    EXPECT_FALSE(ShouldDriveExactPostSLOffKeepAliveBeforePresent(true, false, false, false, false, true, true, true,
                                                                 false, true));
    EXPECT_FALSE(ShouldDriveExactPostSLOffKeepAliveBeforePresent(true, false, false, false, false, true, true, true,
                                                                 true, false));
}

TEST(DXGISharedTest, ExactExplicitOffKeepAlivePrefersOnlyItsProvenLastWorkingQueue) {
    using ce::dx12_overlay_policy::ShouldUsePostSLLastWorkingQueueForExactExplicitOffKeepAlive;

    EXPECT_TRUE(ShouldUsePostSLLastWorkingQueueForExactExplicitOffKeepAlive(true, true, true));
    EXPECT_FALSE(ShouldUsePostSLLastWorkingQueueForExactExplicitOffKeepAlive(false, true, true));
    EXPECT_FALSE(ShouldUsePostSLLastWorkingQueueForExactExplicitOffKeepAlive(true, false, true));
    EXPECT_FALSE(ShouldUsePostSLLastWorkingQueueForExactExplicitOffKeepAlive(true, true, false));
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
                                                                                             true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, false, true, true,
                                                                                             false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, false, true, true,
                                                                                             true, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, false, false, true,
                                                                                              true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(true, false, true, true,
                                                                                              true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, true, true, true,
                                                                                              true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, false, true, true,
                                                                                              true, true, false));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, false, true, true,
                                                                                             true, false, false));
}

// ---------------------------------------------------------------------------
// Test app session 20260702_142655: suspending FSR FG (no-callback mode) blanked the overlay for the WHOLE
// suspension. The retained no-callback suspension keeps AMD's FI swapchain + runtime-owned queue latched
// while the app renders on origGame, so the "command traffic settles onto the live swapchain queue"
// condition can structurally NEVER be met — the settle-defer stranded overlay init until resume. The
// suspension exemption already approves normal overlay rendering on the runtime-owned swapchain queue
// (AMD not interpolating → backbuffer submit safe; queue routing picks scQueue), so init must proceed.
// ---------------------------------------------------------------------------
TEST(DXGISharedTest, RetainedNoCallbackFSRSuspensionInitIsNotDeferredByQueueSettle) {
    // The exact failing state: FG inactive, runtime owns, cmdQ != scQ, retained no-callback suspension.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(
        /*actualFGActive=*/false, /*streamlineFGRunning=*/false, /*runtimeOwnsSwapchain=*/true,
        /*hasSwapchainQueue=*/true, /*hasCommandQueue=*/true, /*commandQueueMatchesSwapchainQueue=*/false,
        /*retainedNoCallbackFSRSuspension=*/true));
    // Null command queue during the suspension: same exemption (the render targets scQueue anyway).
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(
        false, false, true, true, /*hasCommandQueue=*/false, false, true));
    // The exemption changes nothing outside the suspension (Talos post-FG settle crash path stays guarded).
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(
        false, false, true, true, true, false, /*retainedNoCallbackFSRSuspension=*/false));
}

TEST(DXGISharedTest, D3D12InjectionProbeDoesNotAddFixedStartupDelay) {
    EXPECT_TRUE(ce::injection_policy::ShouldInjectAfterGraphicsProbe(false));
    EXPECT_TRUE(ce::injection_policy::ShouldInjectAfterGraphicsProbe(true));
}

TEST(DXGISharedTest, PendingInjectionLaunchRequiresLiveWhitelistedNonFailedTarget) {
    EXPECT_TRUE(ce::injection_policy::ShouldLaunchPendingInjection(true, false, false));

    EXPECT_FALSE(ce::injection_policy::ShouldLaunchPendingInjection(false, false, false));
    EXPECT_FALSE(ce::injection_policy::ShouldLaunchPendingInjection(true, true, false));
    EXPECT_FALSE(ce::injection_policy::ShouldLaunchPendingInjection(true, false, true));
}

TEST(DXGISharedTest, ReleasingSwapchainWrapperSkipsOptionalDXGIDestructorMutation) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUnregisterSwapchainDestructionCallbackDuringWrapperDestructor(true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearSwapchainWrapperPrivateDataDuringWrapperDestructor(true, true));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUnregisterSwapchainDestructionCallbackDuringWrapperDestructor(
        false, true, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldClearSwapchainWrapperPrivateDataDuringWrapperDestructor(false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUnregisterSwapchainDestructionCallbackDuringWrapperDestructor(
        false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUnregisterSwapchainDestructionCallbackDuringWrapperDestructor(
        false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearSwapchainWrapperPrivateDataDuringWrapperDestructor(false, false));
}

TEST(DXGISharedTest, DX12FocusLossDoesNotStartRenderBlankingCooldown) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldStartDX12FocusLossOverlayCooldown(true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldStartDX12FocusLossOverlayCooldown(false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldStartDX12FocusLossOverlayCooldown(true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldStartDX12FocusLossOverlayCooldown(false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldKeepDX12FocusLossOverlayCooldown(true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldKeepDX12FocusLossOverlayCooldown(true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldKeepDX12FocusLossOverlayCooldown(false, false));
}

TEST(DXGISharedTest, FreshStreamlineActivationClearsStaleTeardownGraceOnlyWhenGraceExists) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldClearRecentStreamlineTeardownGraceOnFreshActivation(true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldClearRecentStreamlineTeardownGraceOnFreshActivation(true, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldClearRecentStreamlineTeardownGraceOnFreshActivation(true, true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldClearRecentStreamlineTeardownGraceOnFreshActivation(true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearRecentStreamlineTeardownGraceOnFreshActivation(false, true, true));
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

    // When the command queue has settled to the primary game queue, overlay init
    // should proceed even without a swapchain queue or preserved lastWorkingQueue.
    // This prevents indefinite deferral after lastWorkingQueue was cleared during
    // a prior failed FSR->DLSS comeback.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, false, true, false, true, false, false, true));
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
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringPostFSRNonFGRecovery(true, true, false));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringPostFSRNonFGRecovery(true, false, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringPostFSRNonFGRecovery(false, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringPostFSRNonFGRecovery(true, false, false));
}

TEST(DXGISharedTest, FreshAuthoritativeStreamlineStartupHandoffKeepsHeuristicFSRInactive) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringAuthoritativeStreamlineStartupHandoff(
            true, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringAuthoritativeStreamlineStartupHandoff(
            true, true, ce::fg_runtime::RuntimeMode::kOff));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringAuthoritativeStreamlineStartupHandoff(
            false, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringAuthoritativeStreamlineStartupHandoff(
            true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringAuthoritativeStreamlineStartupHandoff(
            true, true, ce::fg_runtime::RuntimeMode::kDLSSFG));
}

TEST(DXGISharedTest, FreshStreamlineStartupHandoffDoesNotPreserveStaleHeuristicFSR) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(false, true, true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(false, true, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(true, true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(false, false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(false, true, false, false));
}

TEST(DXGISharedTest, BlockedFSRHeuristicWindowResetsStaleECLPatternEvidence) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldResetBlockedECLPatternHeuristicEvidence(false, true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldResetBlockedECLPatternHeuristicEvidence(false, false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldResetBlockedECLPatternHeuristicEvidence(false, false, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldResetBlockedECLPatternHeuristicEvidence(false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldResetBlockedECLPatternHeuristicEvidence(true, true, true, true));
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

TEST(DXGISharedTest, SyntheticPostSLAdvancesDormantStartupOnlyWhenFramePathStopsUnlessWrapperProgressProvesHandoff) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, true, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, true, false, true, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(
        true, true, false, true, false, /*sameQueuePureDLSSColdStartSafe=*/true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(false, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(
        true, false, false, true, false, /*sameQueuePureDLSSColdStartSafe=*/true));
}

TEST(DXGISharedTest, SyntheticPostSLStartupOnlyUsesRepeatedCallbackCountdownAfterFSRPhase) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDelaySyntheticPostSLActivationBehindRepeatedCallbacks(true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDelaySyntheticPostSLActivationBehindRepeatedCallbacks(true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDelaySyntheticPostSLActivationBehindRepeatedCallbacks(false, false));
}

TEST(DXGISharedTest, SyntheticPostSLStartupCanUseWrapperProgressAfterTopLevelHandoffForPureDLSS) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUseTopLevelHandoffWrapperProgressForSyntheticPostSLActivation(
        false, true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseTopLevelHandoffWrapperProgressForSyntheticPostSLActivation(true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseTopLevelHandoffWrapperProgressForSyntheticPostSLActivation(
        false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseTopLevelHandoffWrapperProgressForSyntheticPostSLActivation(
        false, true, false));
}

TEST(DXGISharedTest, PostSLReactivationWarmupIsNotBypassedEvenAfterWrapperBackedTopLevelHandoffForPureDLSS) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBypassPostSLReactivationWarmup(false, true, false));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBypassPostSLReactivationWarmup(true, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBypassPostSLReactivationWarmup(false, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBypassPostSLReactivationWarmup(true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBypassPostSLReactivationWarmup(false, false, true));
}

TEST(DXGISharedTest, PureStreamlineResumeProofRequiresActiveDLSSAndSamePreviouslyWorkingQueue) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::HasConfirmedPureStreamlinePostSLResumeProof(false, true, true, true, true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::HasConfirmedPureStreamlinePostSLResumeProof(true, true, true, true, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::HasConfirmedPureStreamlinePostSLResumeProof(false, false, true, true, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::HasConfirmedPureStreamlinePostSLResumeProof(false, true, false, true, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::HasConfirmedPureStreamlinePostSLResumeProof(false, true, true, false, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::HasConfirmedPureStreamlinePostSLResumeProof(false, true, true, true, false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::HasConfirmedPureStreamlinePostSLResumeProof(false, true, true, true, true, false));
}

TEST(DXGISharedTest, StreamlineStartupHandoffPresentUsesTopLevelPathAfterLargeGapWithoutPresentOwner) {
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, true, false, false, true,
                                                                              false, true, true, false));

    EXPECT_TRUE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, true, false, false, true, true,
                                                                             true, true, false));
    EXPECT_TRUE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, true, false, false, false,
                                                                             false, true, true, false));
    EXPECT_TRUE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, true, false, false, true,
                                                                             false, false, true, false));
    EXPECT_TRUE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, true, false, false, true,
                                                                             false, true, false, false));
    EXPECT_TRUE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, true, false, false, true,
                                                                             false, true, true, true));

    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(false, true, true, false, false, true,
                                                                              false, true, true, false));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, false, true, false, false, true,
                                                                              false, true, true, false));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, false, false, false, true,
                                                                              false, true, true, false));
}

TEST(DXGISharedTest, ConfirmedPostSLCannotReenterLargeGapStartupHandoffBypass) {
    EXPECT_TRUE(DXGIShared::ShouldUseStreamlineStartupTopLevelCandidate(false, false, true, true, true, true, true,
                                                                        true, false));

    EXPECT_FALSE(DXGIShared::ShouldUseStreamlineStartupTopLevelCandidate(false, false, true, true, true, true, true,
                                                                         true, true));
    EXPECT_FALSE(DXGIShared::ShouldUseStreamlineStartupTopLevelCandidate(true, false, true, true, true, true, true,
                                                                         true, false));
    EXPECT_FALSE(DXGIShared::ShouldUseStreamlineStartupTopLevelCandidate(false, true, true, true, true, true, true,
                                                                         true, false));
    EXPECT_FALSE(DXGIShared::ShouldUseStreamlineStartupTopLevelCandidate(false, false, false, true, true, true, true,
                                                                         true, false));
    EXPECT_FALSE(DXGIShared::ShouldUseStreamlineStartupTopLevelCandidate(false, false, true, true, true, true, false,
                                                                         true, false));
}

TEST(DXGISharedTest, ProvenPostFSRHandoffDrawsPostSLBeforeItsFirstTransportPresent) {
    using DXGIShared::ShouldRenderExactPostSLBeforeStartupHandoffTransport;

    EXPECT_TRUE(ShouldRenderExactPostSLBeforeStartupHandoffTransport(true, true, true, true, true,
                                                                     /*postSLConfirmedRendering=*/false));
    EXPECT_FALSE(ShouldRenderExactPostSLBeforeStartupHandoffTransport(false, true, true, true, true, false));
    EXPECT_FALSE(ShouldRenderExactPostSLBeforeStartupHandoffTransport(true, false, true, true, true, false));
    EXPECT_FALSE(ShouldRenderExactPostSLBeforeStartupHandoffTransport(true, true, false, true, true, false));
    EXPECT_FALSE(ShouldRenderExactPostSLBeforeStartupHandoffTransport(true, true, true, false, true, false));
    EXPECT_FALSE(ShouldRenderExactPostSLBeforeStartupHandoffTransport(true, true, true, true, false, false));
    EXPECT_FALSE(ShouldRenderExactPostSLBeforeStartupHandoffTransport(true, true, true, true, true, true));
}

TEST(DXGISharedTest, ConfirmedPostSLStandaloneStreamlinePresentUsesNormalRouteWithoutPresentOwnerAfterStartupSettles) {
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, true, true, false, false,
                                                                              false, false, true, true));

    EXPECT_TRUE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, true, true, false, false, true,
                                                                             false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(false, true, true, true, false, false,
                                                                              false, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, false, true, true, false, false,
                                                                              false, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, false, true, false, false,
                                                                              false, false, true, true));
}

TEST(DXGISharedTest, ConfirmedPostSLStandaloneStreamlinePresentStaysSyntheticDuringStartupSettling) {
    EXPECT_TRUE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, true, true, true, false, false,
                                                                             false, true, true));

    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, true, true, false, false,
                                                                              false, false, true, true));
    EXPECT_TRUE(DXGIShared::ShouldTreatStreamlinePresentAsSyntheticReentrant(true, true, true, true, true, false, true,
                                                                             false, true, true));
}

TEST(DXGISharedTest, StartupTransitionWindowClearsOnlyAfterConfirmedStablePostSLRendering) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldClearStreamlineStartupTransitionWindowAfterConfirmedPostSLRendering(false, 2));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldClearStreamlineStartupTransitionWindowAfterConfirmedPostSLRendering(true, 0));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldClearStreamlineStartupTransitionWindowAfterConfirmedPostSLRendering(true, 1));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldClearStreamlineStartupTransitionWindowAfterConfirmedPostSLRendering(true, 2));
}

TEST(DXGISharedTest, PureDLSSPrefersDirectSubmitOnSelectedSwapchainQueueWhenOrigECLMatchesRealECL) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUseSelectedSwapchainQueueDirectSubmitForPureDLSS(true == false, true,
                                                                                                true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseSelectedSwapchainQueueDirectSubmitForPureDLSS(true, true, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseSelectedSwapchainQueueDirectSubmitForPureDLSS(false, false, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseSelectedSwapchainQueueDirectSubmitForPureDLSS(false, true, false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseSelectedSwapchainQueueDirectSubmitForPureDLSS(false, true, true, false));
}

TEST(DXGISharedTest, LivePresentHookPathRefreshesWhenCurrentSwapchainPathDiffersOrLostHooks) {
    EXPECT_FALSE(DXGIShared::ShouldRefreshLivePresentHooksForSwapchainPath(false, true, true, true));

    EXPECT_TRUE(DXGIShared::ShouldRefreshLivePresentHooksForSwapchainPath(true, false, true, true));
    EXPECT_TRUE(DXGIShared::ShouldRefreshLivePresentHooksForSwapchainPath(true, true, false, true));
    EXPECT_TRUE(DXGIShared::ShouldRefreshLivePresentHooksForSwapchainPath(true, true, true, false));

    EXPECT_FALSE(DXGIShared::ShouldRefreshLivePresentHooksForSwapchainPath(true, true, true, true));
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
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(true, false, false,
                                                                                                false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(true, false, true,
                                                                                                false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(true, false, false,
                                                                                                true, false, false));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(true, false, false, true,
                                                                                               true, true));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(false, false, false,
                                                                                               false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(false, true, false,
                                                                                                false, false, false));
}

TEST(DXGISharedTest, PostSLNoWrapperVirtualBootstrapAllowedOnOriginalGameQueueDuringStreamlineFG) {
    // After stale runtime-owned swapchain cleanup, the original game queue becomes
    // the effective swapchain queue. PostSL must be able to bootstrap on it even
    // when no SL wrapper queue or realECL is available.
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(
        true, false, false, false, false, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(true, false, false, true,
                                                                                               false, false, true));

    // Must still be blocked if a wrapper or real queue exists (those paths should be used instead).
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(
        true, true, false, false, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(
        true, false, true, false, false, false, true));
}

TEST(DXGISharedTest, RecentPostSLTeardownActivityRefreshRequiresLiveStreamlineOrPostSLState) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldRefreshRecentPostSLTeardownActivity(true, true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldRefreshRecentPostSLTeardownActivity(true, true, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRefreshRecentPostSLTeardownActivity(false, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRefreshRecentPostSLTeardownActivity(true, false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRefreshRecentPostSLTeardownActivity(true, true, false, false));
}

TEST(DXGISharedTest, DelayedPostFSRNonFGRecoveryPreservesRealECLOnlyAfterPrimarySettles) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveRealECLForDelayedPostFSRNonFGRecovery(true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRealECLForDelayedPostFSRNonFGRecovery(false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRealECLForDelayedPostFSRNonFGRecovery(true, false));
}

TEST(DXGISharedTest, FreshAuthoritativeStreamlineHandoffAfterFSRReprobesMissingRealECL) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldReprobeRealD3D12ECLOnFreshAuthoritativeStreamlineHandoff(true, true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldReprobeRealD3D12ECLOnFreshAuthoritativeStreamlineHandoff(false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldReprobeRealD3D12ECLOnFreshAuthoritativeStreamlineHandoff(true, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldReprobeRealD3D12ECLOnFreshAuthoritativeStreamlineHandoff(true, true, true));
}

TEST(DXGISharedTest, PostSLActivationWaitsForSafeBootstrapPathAfterFSRPhase) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, false, false, false));
    // SL wrapper alone is enough to proceed even without realECL (post-FSR wrapper bootstrap)
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, false, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, false, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, false, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(false, false, false, false));
}

TEST(DXGISharedTest, PostSLActivationAcceptsRuntimeOwnedSwapchainBootstrapAfterFSR) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, false, true, false, true));

    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(true, false, true, false, false));
}

TEST(DXGISharedTest, RuntimeOwnedSwapchainQueueCanBootstrapPostFSRStreamlineMenuHandoff) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainQueueAsSafePostFSRBootstrap(true, true, true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainQueueAsSafePostFSRBootstrap(false, true, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainQueueAsSafePostFSRBootstrap(true, false, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainQueueAsSafePostFSRBootstrap(true, true, false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainQueueAsSafePostFSRBootstrap(true, true, true, false));
}

TEST(DXGISharedTest, RuntimeOwnedStreamlineBootstrapDoesNotRequireCommandQueueMatch) {
    // The third parameter is Streamline handoff/active proof, not "command queue
    // equals swapchain queue"; multi-queue games can keep render and runtime
    // swapchain queues distinct during FSR->DLSS handoff.
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainQueueAsSafePostFSRBootstrap(true, true, true, true));
}

TEST(DXGISharedTest, PostSLScQueueVirtualSubmitIsDisabledAfterFSRPhase) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUsePostSLScQueueVirtualSubmit(false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLScQueueVirtualSubmit(true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePostSLScQueueVirtualSubmit(false, false));
}
