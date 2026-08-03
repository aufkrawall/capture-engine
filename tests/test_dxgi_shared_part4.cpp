#include "test_dxgi_shared_shared.h"

TEST(DXGISharedTest, LiveNoCallbackNativeFSRToggleAcceptsStreamlineNoFGAsNonFSRSide) {
    using ce::dx12_overlay_policy::IsLiveNoCallbackNativeFSRSuspensionToggle;
    using ce::fg_runtime::RuntimeMode;

    // Session 20260612_215439: with Streamline DLLs merely loaded (no SL FG
    // signal) the classifier labels the non-FG state STREAMLINE_NO_FG. The
    // finalized no-callback FFX takeover had already rebuilt the overlay on
    // the runtime queue, but the STREAMLINE_NO_FG->FSR_FG classification one
    // frame later re-armed the 60-frame draw cooldown (60-present blank)
    // because the exemption only matched kOff. Both directions must qualify
    // with the full proof shape.
    EXPECT_TRUE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kStreamlineNoFG, RuntimeMode::kFSRFG, false,
                                                          true, true, true, true, true));
    EXPECT_TRUE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kFSRFG, RuntimeMode::kStreamlineNoFG, false,
                                                          true, true, true, true, true));

    // The hard requirements are unchanged: no SL FG signal, enabled-configure
    // no-callback latch, runtime ownership, live queue, and the backend bound
    // to that exact queue. The early enable edge (backend still on the game
    // queue) keeps the protected cooldown path.
    EXPECT_FALSE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kStreamlineNoFG, RuntimeMode::kFSRFG, true,
                                                           true, true, true, true, true));
    EXPECT_FALSE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kStreamlineNoFG, RuntimeMode::kFSRFG, false,
                                                           false, true, true, true, true));
    EXPECT_FALSE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kStreamlineNoFG, RuntimeMode::kFSRFG, false,
                                                           true, false, true, true, true));
    EXPECT_FALSE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kStreamlineNoFG, RuntimeMode::kFSRFG, false,
                                                           true, true, true, true, false));

    // STREAMLINE_NO_FG <-> Off label changes are not FSR toggles.
    EXPECT_FALSE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kStreamlineNoFG, RuntimeMode::kOff, false, true,
                                                           true, true, true, true));
}

TEST(DXGISharedTest, HeuristicOnlyRuntimeModeFlipSkipsDrawCooldown) {
    using ce::dx12_overlay_policy::IsHeuristicOnlyRuntimeModeFlip;
    using ce::dx12_overlay_policy::ShouldStartFrameGenerationTransitionCooldown;
    using ce::fg_runtime::RuntimeMode;

    // Session 20260612_215439: stale ECL-pattern evidence latched phantom
    // FSR_FG right after FSR->OFF swapchain recovery; the double flip
    // (STREAMLINE_NO_FG->FSR_FG->STREAMLINE_NO_FG, ownership=0,
    // sl_signal=0->0) armed two 60-frame cooldowns blanking a healthy,
    // freshly initialized overlay for 61 presents. A label-only flip — no SL
    // signal, no ownership, no authoritative FSR, backend live on the current
    // queue — must not arm the draw cooldown.
    EXPECT_TRUE(IsHeuristicOnlyRuntimeModeFlip(false, false, false, false, true, true, true));

    // Any transport-relevant signal keeps the protected cooldown path: SL FG
    // signal on either side, runtime ownership, authoritative FSR API state,
    // missing live queue, uninitialized backend, or a backend on a different
    // queue.
    EXPECT_FALSE(IsHeuristicOnlyRuntimeModeFlip(true, false, false, false, true, true, true));
    EXPECT_FALSE(IsHeuristicOnlyRuntimeModeFlip(false, true, false, false, true, true, true));
    EXPECT_FALSE(IsHeuristicOnlyRuntimeModeFlip(false, false, true, false, true, true, true));
    EXPECT_FALSE(IsHeuristicOnlyRuntimeModeFlip(false, false, false, true, true, true, true));
    EXPECT_FALSE(IsHeuristicOnlyRuntimeModeFlip(false, false, false, false, false, true, true));
    EXPECT_FALSE(IsHeuristicOnlyRuntimeModeFlip(false, false, false, false, true, false, true));
    EXPECT_FALSE(IsHeuristicOnlyRuntimeModeFlip(false, false, false, false, true, true, false));

    // Plumbed through the cooldown decision: the phantom flip shape skips the
    // cooldown, while the same mode change without the exemption keeps it.
    EXPECT_FALSE(ShouldStartFrameGenerationTransitionCooldown(RuntimeMode::kStreamlineNoFG, RuntimeMode::kFSRFG, false,
                                                              true, false, false, true));
    EXPECT_TRUE(ShouldStartFrameGenerationTransitionCooldown(RuntimeMode::kStreamlineNoFG, RuntimeMode::kFSRFG, false,
                                                             true, false, false, false));
}

TEST(DXGISharedTest, NativeFSRNoCallbackCompositionRetainedAcrossSuspension) {
    using ce::dx12_overlay_policy::ShouldRetainNativeFSRInternalNoCallbackCompositionForDisabledConfigure;

    // Disabled (suspend) configure with no callback route while the runtime
    // still owns the live present path keeps the latch alive.
    EXPECT_TRUE(
        ShouldRetainNativeFSRInternalNoCallbackCompositionForDisabledConfigure(false, false, false, true, true));

    // Enabled configures compute the latch directly; bridge/app-callback
    // routes own suspension rendering; a fresh session has nothing to retain;
    // and without live runtime ownership the suspension is really a teardown.
    EXPECT_FALSE(
        ShouldRetainNativeFSRInternalNoCallbackCompositionForDisabledConfigure(true, false, false, true, true));
    EXPECT_FALSE(
        ShouldRetainNativeFSRInternalNoCallbackCompositionForDisabledConfigure(false, true, false, true, true));
    EXPECT_FALSE(
        ShouldRetainNativeFSRInternalNoCallbackCompositionForDisabledConfigure(false, false, true, true, true));
    EXPECT_FALSE(
        ShouldRetainNativeFSRInternalNoCallbackCompositionForDisabledConfigure(false, false, false, false, true));
    EXPECT_FALSE(
        ShouldRetainNativeFSRInternalNoCallbackCompositionForDisabledConfigure(false, false, false, true, false));
}

TEST(DXGISharedTest, GameSwapchainCreationEndsRuntimeOwnedNativeFSRTeardown) {
    using ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation;

    // Args: (gameCreated, explicitOff, contextsDestroyed, noCallbackCompositionActive, streamlineFG).
    // 20260611_191950 FSR->OFF: the game recreates its swapchain on a FRESH queue after explicit native-FSR
    // OFF/destroy; that creation is the stronger off signal and must end the runtime-owned teardown.
    EXPECT_TRUE(ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(true, true, false, false, false));
    EXPECT_TRUE(ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(true, false, true, false, false));
    EXPECT_TRUE(ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(true, true, true, false, false));

    // 20260623_053805 FSR->OFF under the no-callback route: the explicit OFF/destroy signals are MISSED
    // (ffxDestroyContext bypass + one-shot ffxConfigure VEH permanently disarmed) and the game recreates on a
    // fresh queue, so neither pending flag nor the origGame-return ever fires. The no-callback composition being
    // active is itself the runtime-owned-native-FG state, so a game-created swapchain there must end the
    // teardown (otherwise the overlay stays blanked on the bundle-only route forever).
    EXPECT_TRUE(ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(true, false, false, true, false));

    // Runtime/third-party creators, NO off evidence at all, or live Streamline FG keep the conservative path.
    EXPECT_FALSE(ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(false, true, true, true, false));
    EXPECT_FALSE(ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(true, false, false, false, false));
    EXPECT_FALSE(ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(true, true, true, true, true));
}

TEST(DXGISharedTest, GameSwapchainRecoveryToggleSkipsFSROffTransitionCooldown) {
    using ce::dx12_overlay_policy::IsGameSwapchainRecoveryToggleAfterNativeFSROff;
    using ce::fg_runtime::RuntimeMode;

    EXPECT_TRUE(IsGameSwapchainRecoveryToggleAfterNativeFSROff(RuntimeMode::kFSRFG, RuntimeMode::kOff, false, true));
    // Session 20260613_035221: after a prior DLSS phase the Streamline DLLs
    // stay loaded, so the recovered non-FG state classifies as
    // STREAMLINE_NO_FG (not kOff). The second FSR->OFF ran the recovery edge
    // correctly but the FSR_FG->STREAMLINE_NO_FG classification armed the
    // 60-frame cooldown anyway; that side must qualify with the recovery-queue
    // match and no SL FG signal.
    EXPECT_TRUE(
        IsGameSwapchainRecoveryToggleAfterNativeFSROff(RuntimeMode::kFSRFG, RuntimeMode::kStreamlineNoFG, false, true));

    EXPECT_FALSE(IsGameSwapchainRecoveryToggleAfterNativeFSROff(RuntimeMode::kFSRFG, RuntimeMode::kOff, true, true));
    EXPECT_FALSE(IsGameSwapchainRecoveryToggleAfterNativeFSROff(RuntimeMode::kFSRFG, RuntimeMode::kOff, false, false));
    EXPECT_FALSE(IsGameSwapchainRecoveryToggleAfterNativeFSROff(RuntimeMode::kOff, RuntimeMode::kFSRFG, false, true));
    EXPECT_FALSE(IsGameSwapchainRecoveryToggleAfterNativeFSROff(RuntimeMode::kDLSSFG, RuntimeMode::kOff, false, true));
    // STREAMLINE_NO_FG next side still needs the recovery-queue match and no SL FG signal.
    EXPECT_FALSE(
        IsGameSwapchainRecoveryToggleAfterNativeFSROff(RuntimeMode::kFSRFG, RuntimeMode::kStreamlineNoFG, true, true));
    EXPECT_FALSE(IsGameSwapchainRecoveryToggleAfterNativeFSROff(RuntimeMode::kFSRFG, RuntimeMode::kStreamlineNoFG,
                                                                false, false));

    // The exemption feeds the same transition-cooldown gate as the suspension
    // toggle: FSR_FG -> Off with the recovery proof must not arm the cooldown.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldStartFrameGenerationTransitionCooldown(
        RuntimeMode::kFSRFG, RuntimeMode::kOff, true, false, false, false, true));
}

TEST(DXGISharedTest, GameRecoverySwapchainPresentsDriveProcessFrameDespiteZeroECLClassification) {
    using ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent;
    using ce::fg_runtime::RuntimeMode;

    // 20260612_000936: after the game-created recovery ended runtime ownership
    // and cleared the heuristic keep-alives, zero-ECL classification against
    // the game's retired original queue skipped ProcessFrame on every present
    // and the overlay never came back. Presents on the game-created recovery
    // swapchain are real game frames by construction.
    EXPECT_FALSE(ShouldSkipProcessFrameForZeroECLPresent(true, false, false, false, false, false, false,
                                                         RuntimeMode::kOff, true));

    // Without the recovery proof the conservative zero-ECL skip stays.
    EXPECT_TRUE(ShouldSkipProcessFrameForZeroECLPresent(true, false, false, false, false, false, false,
                                                        RuntimeMode::kOff, false));
}

TEST(DXGISharedTest, GameSwapchainRecoveryReinitsOverlayImmediatelyAfterNativeFSROff) {
    using ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyAfterGameSwapchainRecoveryFromNativeFSROff;

    EXPECT_TRUE(ShouldReinitOverlayImmediatelyAfterGameSwapchainRecoveryFromNativeFSROff(true, false, false));

    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterGameSwapchainRecoveryFromNativeFSROff(false, false, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterGameSwapchainRecoveryFromNativeFSROff(true, true, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterGameSwapchainRecoveryFromNativeFSROff(true, false, true));
}

TEST(DXGISharedTest, FinalizedNoCallbackFFXTakeoverReinitsOverlayImmediately) {
    using ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange;

    // Enabled-configure proof + applied staged runtime queue + internal
    // no-callback route: the first Present on AMD's swapchain may rebuild the
    // overlay immediately instead of blanking through the 90-frame cooldown
    // (20260611_142923: ~1.3s overlay blank on OFF -> FSR FG).
    EXPECT_TRUE(
        ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(true, true, true, true, true, false));

    // Every missing proof keeps the protective cooldown.
    EXPECT_FALSE(
        ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(false, true, true, true, true, false));
    EXPECT_FALSE(
        ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(true, false, true, true, true, false));
    EXPECT_FALSE(
        ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(true, true, false, true, true, false));
    EXPECT_FALSE(
        ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(true, true, true, false, true, false));
    EXPECT_FALSE(
        ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(true, true, true, true, false, false));
    EXPECT_FALSE(
        ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(true, true, true, true, true, true));
}

TEST(DXGISharedTest, ConfirmedPostSLSuspensionReinitsOverlayImmediatelyInsteadOfBlanking) {
    using ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange;

    // Session 20260613_145008: slDLSSGSetOptions(off) suspend surfaced a fresh proxy
    // swapchain on the same live runtime-owned queue. The active-FG preserve path can't
    // fire (streamlineFGRunning already false), so the change took the 90-frame cooldown
    // and blanked the live overlay ~800ms. With the make-before-break keep-alive latch set
    // (a CONFIRMED PostSL path that is merely suspended), reinit the warm backend
    // immediately on its live queue.
    EXPECT_TRUE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
        /*postSLExplicitOffKeepAlive=*/true, /*streamlineFGRunning=*/false, /*fsrFGApiActive=*/false,
        /*nativeFSRInternalNoCallbackComposition=*/false, /*runtimeOwnsSwapchain=*/true, /*swapchainQueueIsLiveCommandQueue=*/true,
        /*swapchainQueueIsConfirmedPostSLRenderQueue=*/false));

    // Without the keep-alive latch this is not a confirmed-PostSL suspension — keep the cooldown.
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(false, false, false, false,
                                                                                             true, true, false));
    // Streamline FG still running is the ACTIVE-FG preserve path's job, not this one.
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(true, true, false, false,
                                                                                             true, true, false));
    // An FSR / native-FG no-callback takeover must keep the quiesce cooldown (device-removal hazard).
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(true, false, true, false,
                                                                                             true, true, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(true, false, false, true,
                                                                                             true, true, false));
    // Must be the live runtime-owned queue (cross-queue / non-owned change keeps the cooldown).
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(true, false, false, false,
                                                                                             false, true, false));
    // scQueue matches NEITHER the live cmdQueue NOR the confirmed PostSL render queue → keep cooldown.
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(true, false, false, false,
                                                                                             true, false, false));
}

TEST(DXGISharedTest, ConfirmedPostSLSuspensionReinitsImmediatelyWhenSwapchainQueueIsConfirmedPostSLQueue) {
    using ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange;

    // Session 20260613_202646 (90-present blank): at the DLSS-suspend edge the live
    // confirmed-PostSL queue is the DLSS-G proxy queue (== scQueue == g_PostSLLastWorkingQueue,
    // 180+ confirmed submits), while the live wrapper cmdQueue is a SEPARATE object. The strict
    // scQueue==cmdQueue test wrongly rejected this safe suspend and dropped it into the 90-frame
    // cooldown. The confirmed PostSL render queue alone now satisfies the queue proof.
    EXPECT_TRUE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
        /*postSLExplicitOffKeepAlive=*/true, /*streamlineFGRunning=*/false, /*fsrFGApiActive=*/false,
        /*nativeFSRInternalNoCallbackComposition=*/false, /*runtimeOwnsSwapchain=*/true, /*swapchainQueueIsLiveCommandQueue=*/false,
        /*swapchainQueueIsConfirmedPostSLRenderQueue=*/true));

    // The relaxed queue proof does NOT loosen any of the hard suspension guards.
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
        false, false, false, false, true, false, true));  // no keep-alive
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
        true, true, false, false, true, false, true));  // SL FG running
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
        true, false, true, false, true, false, true));  // FSR API active
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
        true, false, false, false, false, false, true));  // not runtime-owned
}

TEST(DXGISharedTest, ReusesValidatedLastWorkingQueueForResumedDLSSDuringPostFSRInactiveRecovery) {
    EXPECT_TRUE(ce::dx12_overlay_policy::
                    ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
                        true, true, true, false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::
                    ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
                        true, true, true, false, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::
                     ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
                         false, true, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::
                     ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
                         true, false, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::
                     ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
                         true, true, false, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::
                     ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
                         true, true, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::
                     ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
                         true, true, true, false, false, false));
}

TEST(DXGISharedTest, FreshPostFSRStreamlineHandoffInvalidatesOnlyStaleLastWorkingQueueProof) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldInvalidatePostSLLastWorkingQueueOnFreshPostFSRStreamlineHandoff(
        true, true, true, true, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInvalidatePostSLLastWorkingQueueOnFreshPostFSRStreamlineHandoff(
        false, true, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInvalidatePostSLLastWorkingQueueOnFreshPostFSRStreamlineHandoff(
        true, false, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInvalidatePostSLLastWorkingQueueOnFreshPostFSRStreamlineHandoff(
        true, true, false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInvalidatePostSLLastWorkingQueueOnFreshPostFSRStreamlineHandoff(
        true, true, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInvalidatePostSLLastWorkingQueueOnFreshPostFSRStreamlineHandoff(
        true, true, true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInvalidatePostSLLastWorkingQueueOnFreshPostFSRStreamlineHandoff(
        true, true, true, true, true, true));
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

// Fix 2 contract (session 20260613_202646, 60-present after-FSR-history DLSS-suspend blank):
// when the make-before-break keep-alive is armed for a confirmed-PostSL suspension, the
// "after FSR history" reinit deferral must collapse to 0 frames (immediate warm reinit)
// regardless of any stale cooldown, while a non-suspension keeps the protective 60.
TEST(DXGISharedTest, ConfirmedPostSLSuspensionAfterFSRHistoryResolvesReinitCooldownToZero) {
    using ce::dx12_overlay_policy::ResolveTransitionCooldownFrames;
    using ce::dx12_overlay_policy::ShouldKeepConfirmedPostSLAliveAcrossStreamlineOff;

    // Confirmed PostSL, no FSR/native-FG takeover, not protected-FFX startup → keep-alive armed.
    const bool keepAlive = ShouldKeepConfirmedPostSLAliveAcrossStreamlineOff(
        /*postSLConfirmedRendering=*/true, /*fsrFGApiActive=*/false, /*runtimeOwnedNativeFGPresentPath=*/false,
        /*protectedOfficialFFXStartupPending=*/false);
    EXPECT_TRUE(keepAlive);
    const bool immediate = keepAlive && /*!deviceRemoved=*/true;
    const int cooldownFrames = immediate ? 0 : 60;
    // override == immediate || useShort; here useShort is false, so override == immediate.
    EXPECT_EQ(ResolveTransitionCooldownFrames(/*existingCooldownFrames=*/90, cooldownFrames, /*overrideExistingCooldown=*/immediate), 0);

    // No confirmed PostSL (e.g. a real teardown) keeps the protective 60-frame cooldown.
    const bool noKeepAlive = ShouldKeepConfirmedPostSLAliveAcrossStreamlineOff(false, false, false, false);
    EXPECT_FALSE(noKeepAlive);
    EXPECT_EQ(ResolveTransitionCooldownFrames(/*existingCooldownFrames=*/0, 60, /*overrideExistingCooldown=*/false), 60);
}

TEST(DXGISharedTest, InactiveStreamlineRuntimeStateDoesNotStartFGCooldown) {
    using ce::fg_runtime::RuntimeMode;
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldStartFrameGenerationTransitionCooldown(
        RuntimeMode::kOff, RuntimeMode::kStreamlineNoFG, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldStartFrameGenerationTransitionCooldown(
        RuntimeMode::kStreamlineNoFG, RuntimeMode::kOff, false, false, false, false));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldStartFrameGenerationTransitionCooldown(
        RuntimeMode::kStreamlineNoFG, RuntimeMode::kDLSSFG, false, true, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldStartFrameGenerationTransitionCooldown(
        RuntimeMode::kFSRFG, RuntimeMode::kStreamlineNoFG, true, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldStartFrameGenerationTransitionCooldown(
        RuntimeMode::kFSRFG, RuntimeMode::kDLSSFG, true, true, false, true));
}

TEST(DXGISharedTest, SettledPrimaryPostFSROffUsesShorterCooldown) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(true, true, false));
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
    EXPECT_EQ(
        3, ce::dx12_overlay_policy::ResolveOverlayFGMetricType(true, ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion));

    EXPECT_EQ(0, ce::dx12_overlay_policy::ResolveOverlayFGMetricType(false, ce::fg_runtime::RuntimeMode::kDLSSFG));
    EXPECT_EQ(0,
              ce::dx12_overlay_policy::ResolveOverlayFGMetricType(false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG));
    EXPECT_EQ(0, ce::dx12_overlay_policy::ResolveOverlayFGMetricType(true, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_EQ(0,
              ce::dx12_overlay_policy::ResolveOverlayFGMetricType(true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG));
}

TEST(DXGISharedTest, OverlayFGPublishedTypeDifferenceIgnoresInactiveRuntimeLabels) {
    EXPECT_FALSE(ce::dx12_overlay_policy::DoOverlayFGPublishedTypesDiffer(
        false, ce::fg_runtime::RuntimeMode::kOff, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG));
    EXPECT_FALSE(ce::dx12_overlay_policy::DoOverlayFGPublishedTypesDiffer(true, ce::fg_runtime::RuntimeMode::kOff,
                                                                          false, ce::fg_runtime::RuntimeMode::kOff));

    EXPECT_TRUE(ce::dx12_overlay_policy::DoOverlayFGPublishedTypesDiffer(false, ce::fg_runtime::RuntimeMode::kOff, true,
                                                                         ce::fg_runtime::RuntimeMode::kDLSSFG));
    EXPECT_TRUE(ce::dx12_overlay_policy::DoOverlayFGPublishedTypesDiffer(true, ce::fg_runtime::RuntimeMode::kDLSSFG,
                                                                         true, ce::fg_runtime::RuntimeMode::kFSRFG));
}

TEST(DXGISharedTest, ZeroECLPresentsStillReachProcessFrameForRuntimeOwnedNonStreamlineSwapchains) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(
        true, false, false, true, false, false, false, ce::fg_runtime::RuntimeMode::kOff));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(
        true, false, false, false, false, false, false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(
        true, false, false, true, true, false, false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(
        true, true, false, false, false, false, false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(
        true, false, true, false, false, false, false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(
        false, false, false, false, false, false, false, ce::fg_runtime::RuntimeMode::kOff));
}

TEST(DXGISharedTest, ZeroECLPresentsStillReachProcessFrameDuringRecentStreamlineTeardown) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(
        true, false, false, false, false, true, false, ce::fg_runtime::RuntimeMode::kOff));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(
        true, false, false, false, true, true, false, ce::fg_runtime::RuntimeMode::kOff));
}

TEST(DXGISharedTest, ZeroECLPresentsStillReachProcessFrameDuringPostFSRNonFGRecovery) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(
        true, false, false, false, false, false, true, ce::fg_runtime::RuntimeMode::kOff));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(
        true, false, false, false, false, false, false, ce::fg_runtime::RuntimeMode::kOff));
}

TEST(DXGISharedTest, ZeroECLPresentsStillReachProcessFrameDuringStreamlineNoFGStartup) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(
        true, false, false, false, false, false, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(
        true, false, false, false, false, false, false, ce::fg_runtime::RuntimeMode::kOff));
}

TEST(DXGISharedTest, DuplicateTopLevelPresentSuppressionBypassesRuntimeOwnedNonStreamlineSwapchains) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressLikelyDuplicateTopLevelPresent(true, false));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSuppressLikelyDuplicateTopLevelPresent(false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSuppressLikelyDuplicateTopLevelPresent(true, true));
}

TEST(DXGISharedTest, DedicatedOverlayQueueStaysDisabledForRuntimeOwnedNativeFG) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(
        true, true, false, true, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(
        true, false, false, true, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(
        true, true, false, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(
        false, false, false, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(
        true, false, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(
        false, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(
        true, false, false, false, false));
}

TEST(DXGISharedTest, RuntimeOwnedNativeFSRSuppressesInjectedOverlayGpuWorkOnlyForFSRStates) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kFSRFG, false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kOff, false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        false, false, ce::fg_runtime::RuntimeMode::kOff, false, true, false));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        false, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, true, ce::fg_runtime::RuntimeMode::kFSRFG, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        false, true, ce::fg_runtime::RuntimeMode::kOff, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kOff, false, false, false));
}

TEST(DXGISharedTest, FreshRuntimeOwnedStreamlineNoFGKeepsVisibleOverlayWorkLive) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressFreshRuntimeOwnedStreamlineNoFGSeparateOverlayWork(
        true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, 1, 8));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipFreshRuntimeOwnedStreamlineNoFGPresentProcessing(
        true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, 1, 8));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressFreshRuntimeOwnedStreamlineNoFGSeparateOverlayWork(
        true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, 8, 8));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipFreshRuntimeOwnedStreamlineNoFGPresentProcessing(
        true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, 8, 8));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressFreshRuntimeOwnedStreamlineNoFGSeparateOverlayWork(
        true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, 9, 8));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipFreshRuntimeOwnedStreamlineNoFGPresentProcessing(
        true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, 9, 8));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressFreshRuntimeOwnedStreamlineNoFGSeparateOverlayWork(
        false, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, 1, 8));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipFreshRuntimeOwnedStreamlineNoFGPresentProcessing(
        false, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, 1, 8));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressFreshRuntimeOwnedStreamlineNoFGSeparateOverlayWork(
        true, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, 1, 8));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipFreshRuntimeOwnedStreamlineNoFGPresentProcessing(
        true, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, 1, 8));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressFreshRuntimeOwnedStreamlineNoFGSeparateOverlayWork(
        true, false, ce::fg_runtime::RuntimeMode::kFSRFG, 1, 8));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipFreshRuntimeOwnedStreamlineNoFGPresentProcessing(
        true, false, ce::fg_runtime::RuntimeMode::kFSRFG, 1, 8));
}

TEST(DXGISharedTest, FFXPresentCallbackStallAllowsNormalOverlayRendering) {
    // When the FFX present callback is reported as stalled, normal overlay
    // fallback is allowed only after fresh direct FFX/callback proof.  A stale
    // callback from an earlier runtime-owned swapchain is not enough.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(true, false,
                                                                                                       false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, true, false));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(true, false, true,
                                                                                                      false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(true, false,
                                                                                                      false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kFSRFG, false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kOff, false, true, true));

    // Streamline FG running still overrides everything — no skip regardless of stall.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, true, ce::fg_runtime::RuntimeMode::kFSRFG, true, true, true));

    // Runtime-ownership latching is not required once native FSR is authoritative; the
    // callback-owned path must remain the only overlay path until a safe fallback is proven.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        false, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        false, false, ce::fg_runtime::RuntimeMode::kOff, false, false, true));
}

TEST(DXGISharedTest, ProgressResolvedOfficialFFXCallbackStallRequiresDirectProofBeforeNormalOverlayFallback) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(true, true,
                                                                                                       false, false));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        false, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, true,
        ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(true, true, false,
                                                                                              false)));

    // Stable same-queue proof is not enough. The GTA freeze family showed that
    // the AMD presenter can still be in its private query path even while the
    // game appears to make normal frame progress.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        true, true, false, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        false, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, true,
        ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(true, true, false, false,
                                                                                              true)));

    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(true, true, true, false));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(true, true, false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(false, true, true, true));
}

TEST(DXGISharedTest, NativeFSRTimeoutOverrideRequiresSafeCallbackStallFallback) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(false, false, false, false));

    // A healthy native FSR present callback means the overlay already has the
    // correct runtime-owned path.  The normal DX12 overlay path must not wake
    // up just because the generic 2s suppression timer expired.
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(true, true, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(false, true, false, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(true, true, true, false));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(true, true, true, true));
}

TEST(DXGISharedTest, ExplicitNativeFSROffKeepsSeparateOverlaySuppressedDuringRetainedRuntimePath) {
    // GTA menu/suspend paths explicitly configure native FSR FG off while the
    // FFX context and callback bridge remain alive. That state should keep the
    // already-retained callback path authoritative instead of using callback
    // proof as permission to wake normal DX12 overlay GPU work.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEvaluateFFXPresentCallbackFallback(false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldEvaluateFFXPresentCallbackFallback(true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEvaluateFFXPresentCallbackFallback(false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        true, false, true, false, false, 0, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        true, false, false, true, false, 0, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        true, true, false, false, true, 0, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        true, false, false, false, false, 0, true));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        false, false, ce::fg_runtime::RuntimeMode::kOff, false, true,
        ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(true, false, false, true,
                                                                                              false, 0, true)));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(
        true, false, true,
        ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(true, false, false, true,
                                                                                              false, 0, true)));
}

TEST(DXGISharedTest, DisabledNativeFSRConfigureRetainsExistingCallbackBridgeOnlyAfterEnabledConfigure) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldRetainFFXPresentCallbackBridgeForDisabledConfigure(true, false, true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldRetainFFXPresentCallbackBridgeForDisabledConfigure(false, false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldRetainFFXPresentCallbackBridgeForDisabledConfigure(true, true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldRetainFFXPresentCallbackBridgeForDisabledConfigure(true, false, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldRetainFFXPresentCallbackBridgeForDisabledConfigure(true, false, true, true));
}

TEST(DXGISharedTest, D3D12FactoryWrapperBypassesFrameGenerationRuntimeSwapchains) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBypassDXGISwapchainWrapperForFrameGenerationRuntime(
        false, true, true, true, true, ce::fg_runtime::RuntimeMode::kFSRFG));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBypassDXGISwapchainWrapperForFrameGenerationRuntime(
        true, false, false, false, false, ce::fg_runtime::RuntimeMode::kOff));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBypassDXGISwapchainWrapperForFrameGenerationRuntime(
        true, true, false, false, false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBypassDXGISwapchainWrapperForFrameGenerationRuntime(
        true, false, true, false, false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBypassDXGISwapchainWrapperForFrameGenerationRuntime(
        true, false, false, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBypassDXGISwapchainWrapperForFrameGenerationRuntime(
        true, false, false, false, true, ce::fg_runtime::RuntimeMode::kFSRFG));
}

TEST(DXGISharedTest, DXGIFactoryWrapperBypassesFrameGenerationRuntimeFactories) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBypassDXGIFactoryWrapperForFrameGenerationRuntime(
        false, false, false, ce::fg_runtime::RuntimeMode::kOff));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBypassDXGIFactoryWrapperForFrameGenerationRuntime(
        true, false, false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBypassDXGIFactoryWrapperForFrameGenerationRuntime(
        false, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBypassDXGIFactoryWrapperForFrameGenerationRuntime(
        false, false, true, ce::fg_runtime::RuntimeMode::kFSRFG));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBypassDXGIFactoryWrapperForFrameGenerationRuntime(
        false, false, false, ce::fg_runtime::RuntimeMode::kDLSSFG));
}

TEST(DXGISharedTest, DXGIFactoryLiveExportIsLimitedToAppThreadFrameGenerationHandoffs) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldUseLiveDXGIFactoryExportForFrameGenerationRuntime(true, false, true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseLiveDXGIFactoryExportForFrameGenerationRuntime(false, false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseLiveDXGIFactoryExportForFrameGenerationRuntime(true, true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseLiveDXGIFactoryExportForFrameGenerationRuntime(true, false, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldUseLiveDXGIFactoryExportForFrameGenerationRuntime(true, false, true, true));
}

TEST(DXGISharedTest, DynamicHookLeavesStreamlineDXGIFactoryProxyExportsVisible) {
    EXPECT_TRUE(IATHook::ShouldAllowStreamlineProxyExportToBypassDynamicHook(true, "CreateDXGIFactory1"));
    EXPECT_TRUE(IATHook::ShouldBypassDynamicHookForCaller(false, false, false, false, false, false, true, false,
                                                          "CreateDXGIFactory1"));

    EXPECT_FALSE(IATHook::ShouldAllowStreamlineProxyExportToBypassDynamicHook(true, "slGetFeatureFunction"));
    EXPECT_FALSE(IATHook::ShouldBypassDynamicHookForCaller(false, false, false, false, false, false, true, false,
                                                           "slGetFeatureFunction"));
    EXPECT_FALSE(IATHook::ShouldAllowStreamlineProxyExportToBypassDynamicHook(false, "CreateDXGIFactory1"));
}

TEST(DXGISharedTest, FFXPresentCallbackProofIsScopedToCurrentRuntimeTakeover) {
    EXPECT_FALSE(ce::dx12_overlay_policy::IsFFXPresentCallbackProofCurrent(0, 100, 0));
    EXPECT_TRUE(ce::dx12_overlay_policy::IsFFXPresentCallbackProofCurrent(150, 100, 0));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsFFXPresentCallbackProofCurrent(90, 100, 0));

    // A callback from an earlier FSR topology must not prove that a later
    // progress-resolved FSR takeover can accept the normal injected overlay
    // path.
    EXPECT_FALSE(ce::dx12_overlay_policy::IsFFXPresentCallbackProofCurrent(150, 100, 200));
    EXPECT_TRUE(ce::dx12_overlay_policy::IsFFXPresentCallbackProofCurrent(250, 100, 200));
}
