#include "test_dxgi_shared_shared.h"

TEST(DXGISharedTest, InvisibleWindowSwapchainsNeverDriveDX12OverlayProcessing) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipDX12PresentProcessingForInvisibleWindowSwapchain(true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipDX12PresentProcessingForInvisibleWindowSwapchain(true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipDX12PresentProcessingForInvisibleWindowSwapchain(false, false));

    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldSkipDX12CreateSwapchainSideEffectsForInvisibleWindowSwapchain(true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldSkipDX12CreateSwapchainSideEffectsForInvisibleWindowSwapchain(true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldSkipDX12CreateSwapchainSideEffectsForInvisibleWindowSwapchain(false, false));
}

TEST(DXGISharedTest, CEOverlaySubmitsNeverDriveQueueTracking) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSuppressQueueTrackingForCEOverlaySubmission(true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressQueueTrackingForCEOverlaySubmission(false));
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
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlaySwapchainQueueCapture(effectiveOverlayCaller,
                                                                                             true, false));
}

TEST(DXGISharedTest, WrappedStreamlineCreateSwapchainTrafficOverridesOverlayClassification) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFrameGenerationRuntime(
        false, false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFrameGenerationRuntime(
        false, false, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFrameGenerationRuntime(
        true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFrameGenerationRuntime(
        false, false, false, false));

    const bool effectiveOverlayCaller =
        true && !ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFrameGenerationRuntime(
                    false, false, true, false);
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlaySwapchainQueueCapture(effectiveOverlayCaller,
                                                                                             true, false));
}

TEST(DXGISharedTest, OriginalQueueValidatesNormalReturnWithStreamlineStack) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatOriginalQueueCreateWithStreamlineStackAsNormalReturn(
        false, false, true, false, true, true));

    // Direct Streamline/FFX provenance and a distinct runtime queue still prove
    // real late-enable takeovers (OFF->DLSS, OFF->FSR, and FSR->DLSS).
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatOriginalQueueCreateWithStreamlineStackAsNormalReturn(
        false, true, true, false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatOriginalQueueCreateWithStreamlineStackAsNormalReturn(
        true, false, true, false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatOriginalQueueCreateWithStreamlineStackAsNormalReturn(
        false, false, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatOriginalQueueCreateWithStreamlineStackAsNormalReturn(
        false, false, true, true, true, true));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldRetirePostSLRouteForNormalSwapchainReturn(true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRetirePostSLRouteForNormalSwapchainReturn(true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRetirePostSLRouteForNormalSwapchainReturn(true, false, true));
}

TEST(DXGISharedTest, PostSLSubmitAbortsWhenSwapchainLifecycleChanges) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAbortPostSLSubmitAfterLifecycleChange(7, 8));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAbortPostSLSubmitAfterLifecycleChange(8, 8));
}

TEST(DXGISharedTest, NormalSwapchainReturnWaitsForAuthoritativeQueueBaseline) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAwaitAuthoritativeQueueChangeBaseline(true, false));
    // Observing the proven queue consumes the boundary; a later distinct queue
    // remains immediately eligible for genuine FSR activation detection.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAwaitAuthoritativeQueueChangeBaseline(true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAwaitAuthoritativeQueueChangeBaseline(false, false));
}

TEST(DXGISharedSourceTest, NormalSwapchainReturnRebaselinesBeforeFirstPresent) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    const size_t handler = text.find("HandlePostSLRouteForNormalSwapchainReturn(");
    ASSERT_NE(handler, std::string::npos);
    const size_t reset = text.find("RequestFGDetectionHeuristicReset(returnedQueue);", handler);
    const size_t retirementDecision = text.find("ShouldRetirePostSLRouteForNormalSwapchainReturn(", handler);
    ASSERT_NE(reset, std::string::npos);
    ASSERT_NE(retirementDecision, std::string::npos);
    EXPECT_LT(reset, retirementDecision)
        << "every proven normal return must rebaseline even when no stale PostSL route remains armed";

    const size_t processFrame = text.find("void ProcessFrame(");
    ASSERT_NE(processFrame, std::string::npos);
    EXPECT_NE(text.find("ShouldAwaitAuthoritativeQueueChangeBaseline(", processFrame), std::string::npos);
    EXPECT_NE(text.find("Established authoritative queue-change baseline after normal swapchain return", processFrame),
              std::string::npos);
}

TEST(DXGISharedSourceTest, AuthoritativeDLSSOffNativeReturnProofFeedsFirstMatchingPresent) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    const size_t handler = text.find("HandlePostSLRouteForNormalSwapchainReturn(");
    ASSERT_NE(handler, std::string::npos);
    EXPECT_NE(text.find("dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(returnedSwapchain", handler),
              std::string::npos);

    const size_t processFrame = text.find("void ProcessFrame(");
    ASSERT_NE(processFrame, std::string::npos);
    const size_t lifetimeDecision = text.find("ShouldProcessLogicalSwapchainReplacement(", processFrame);
    const size_t decision =
        text.find("ShouldReinitOverlayImmediatelyAfterAuthoritativeDLSSOffNormalReturn(", processFrame);
    const size_t consume =
        text.find("dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.compare_exchange_strong(", processFrame);
    const size_t preserve = text.find("ShouldKeepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn(", processFrame);
    ASSERT_NE(lifetimeDecision, std::string::npos);
    ASSERT_NE(decision, std::string::npos);
    ASSERT_NE(consume, std::string::npos);
    ASSERT_NE(preserve, std::string::npos);
    EXPECT_LT(lifetimeDecision, decision);
    EXPECT_LT(decision, consume);
    EXPECT_LT(consume, preserve);
}

TEST(DXGISharedTest, ExactCreationProofDefeatsSwapchainPointerABAReuse) {
    using ce::dx12_overlay_policy::ShouldProcessLogicalSwapchainReplacement;

    EXPECT_TRUE(ShouldProcessLogicalSwapchainReplacement(true, false));
    EXPECT_TRUE(ShouldProcessLogicalSwapchainReplacement(false, true));
    EXPECT_TRUE(ShouldProcessLogicalSwapchainReplacement(true, true));
    EXPECT_FALSE(ShouldProcessLogicalSwapchainReplacement(false, false));
}

TEST(DXGISharedSourceTest, CleanPresentReturnRetiresPostSLRouteBeforeNormalQueueRouting) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    const size_t processFrame = text.find("void ProcessFrame(");
    ASSERT_NE(processFrame, std::string::npos);
    const size_t explicitOffRouteProtection = text.find("const bool explicitOffKeepAlivePending =", processFrame);
    const size_t routeProtectionGuard =
        text.find("if (!postFSRRecoveryPending && !explicitOffKeepAlivePending)", explicitOffRouteProtection);
    const size_t recoveryDecision = text.find("DecideInactiveDLSSPresentRoute(", processFrame);
    ASSERT_NE(explicitOffRouteProtection, std::string::npos);
    ASSERT_NE(routeProtectionGuard, std::string::npos);
    ASSERT_NE(recoveryDecision, std::string::npos);
    EXPECT_LT(explicitOffRouteProtection, routeProtectionGuard);
    EXPECT_LT(routeProtectionGuard, recoveryDecision)
        << "a pure-DLSS explicit-OFF proxy must use the exact keep-alive route even without FSR history";
    const size_t preRoutingCoverage = text.find("DXGIShared::WasPostSLOffKeepAlivePrePresentDrawn()", recoveryDecision);
    const size_t dedupDecision = text.find("ShouldSubmitInactiveDLSSExactPostSLKeepAlive(", preRoutingCoverage);
    const size_t fallbackSubmitGuard = text.find("if (shouldSubmitKeepAlive)", dedupDecision);
    const size_t exactPostSLKeepAlive = text.find("PostSLOverlayRenderGated(pSwapChain);", fallbackSubmitGuard);
    const size_t directDrawSuccess = text.find("fallbackKeepAliveDrawSucceeded =", exactPostSLKeepAlive);
    const size_t directDrawSuccessGuard = text.find("if (fallbackKeepAliveDrawSucceeded)", directDrawSuccess);
    const size_t markPrePresentDraw =
        text.find("DXGIShared::MarkPostSLOffKeepAlivePrePresentDrawn();", directDrawSuccessGuard);
    const size_t exactPostSLReturn = text.find("return true;", markPrePresentDraw);
    ASSERT_NE(preRoutingCoverage, std::string::npos);
    ASSERT_NE(dedupDecision, std::string::npos);
    ASSERT_NE(fallbackSubmitGuard, std::string::npos);
    ASSERT_NE(exactPostSLKeepAlive, std::string::npos);
    ASSERT_NE(directDrawSuccess, std::string::npos);
    ASSERT_NE(directDrawSuccessGuard, std::string::npos);
    ASSERT_NE(markPrePresentDraw, std::string::npos);
    ASSERT_NE(exactPostSLReturn, std::string::npos);
    const size_t overlayMutex = text.find("dx12_hook_g_OverlayMutex.try_lock()", recoveryDecision);
    ASSERT_NE(overlayMutex, std::string::npos);
    EXPECT_LT(recoveryDecision, preRoutingCoverage);
    EXPECT_LT(preRoutingCoverage, dedupDecision);
    EXPECT_LT(dedupDecision, fallbackSubmitGuard);
    EXPECT_LT(fallbackSubmitGuard, exactPostSLKeepAlive);
    EXPECT_LT(exactPostSLKeepAlive, directDrawSuccess);
    EXPECT_LT(directDrawSuccess, directDrawSuccessGuard);
    EXPECT_LT(directDrawSuccessGuard, markPrePresentDraw);
    EXPECT_LT(markPrePresentDraw, exactPostSLReturn);
    EXPECT_LT(exactPostSLReturn, overlayMutex)
        << "the exact confirmed proxy must have one keep-alive draw before Present and normal backbuffer access";
    const size_t postLockRecoveryRecheck =
        text.find("if (dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire) ||",
                  overlayMutex);
    const size_t postLockExplicitOffRecheck =
        text.find("dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire))", postLockRecoveryRecheck);
    ASSERT_NE(postLockRecoveryRecheck, std::string::npos);
    ASSERT_NE(postLockExplicitOffRecheck, std::string::npos);
    const size_t postLockRouteResnapshot =
        text.find("routeInactiveDLSSPresentBeforeBackbufferAccess()", postLockExplicitOffRecheck);
    ASSERT_NE(postLockRouteResnapshot, std::string::npos);
    EXPECT_LT(overlayMutex, postLockRecoveryRecheck);
    EXPECT_LT(postLockRecoveryRecheck, postLockExplicitOffRecheck);
    EXPECT_LT(postLockRecoveryRecheck, postLockRouteResnapshot)
        << "a newly armed OFF edge must invalidate even an earlier normal-route ownership proof";

    const size_t cleanReturn = text.find("Swapchain change (no FG active) — normal reinit", overlayMutex);
    ASSERT_NE(cleanReturn, std::string::npos);
    const size_t ownershipGuard =
        text.find("if (endingPostFSRNonFGRecovery && !postFSRNormalRouteOwnershipProven)", cleanReturn);
    ASSERT_NE(ownershipGuard, std::string::npos);
    const size_t provenNormalBoundary =
        text.find("if (endingPostFSRNonFGRecovery && postFSRNormalRouteOwnershipProven)", ownershipGuard);
    ASSERT_NE(provenNormalBoundary, std::string::npos);
    const size_t publishNormalBoundary = text.find(
        "dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG.store(false, std::memory_order_release);",
        provenNormalBoundary);
    ASSERT_NE(publishNormalBoundary, std::string::npos);
    const size_t unlockOverlay = text.find("lock.unlock();", publishNormalBoundary);
    ASSERT_NE(unlockOverlay, std::string::npos);
    const size_t retirePostSL = text.find("FinishPostSLRouteRetirementForNormalSwapchainReturn(", unlockOverlay);
    ASSERT_NE(retirePostSL, std::string::npos);
    const size_t relockOverlay = text.find("lock.lock();", retirePostSL);
    ASSERT_NE(relockOverlay, std::string::npos);
    const size_t normalQueueRouting = text.find("DecideSwapchainOverlayRouting(", relockOverlay);
    ASSERT_NE(normalQueueRouting, std::string::npos);
    EXPECT_LT(ownershipGuard, provenNormalBoundary);
    EXPECT_LT(provenNormalBoundary, publishNormalBoundary);
    EXPECT_LT(publishNormalBoundary, unlockOverlay);
    EXPECT_LT(unlockOverlay, retirePostSL);
    EXPECT_LT(retirePostSL, relockOverlay);
    EXPECT_LT(relockOverlay, normalQueueRouting)
        << "a clean normal return must invalidate the retired PostSL queue before this Present chooses a queue";
}

TEST(DXGISharedSourceTest, PostFSROwnershipProofsAreExactAndPublishedBeforeTransitionConsumers) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    const size_t queueCaptureComment = text.find("// Capture the queue that was passed to CreateSwapChain");
    ASSERT_NE(queueCaptureComment, std::string::npos);
    const size_t setSwapchainQueue = text.find("bool DX12_SetSwapchainQueue(", queueCaptureComment);
    ASSERT_NE(setSwapchainQueue, std::string::npos);
    const size_t queueLock =
        text.find("std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);", setSwapchainQueue);
    const size_t exactQueueAssociation =
        text.find("dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(associatedSwapchain", queueLock);
    const size_t staleNativeAssociationClear =
        text.find("dx12_hook_g_LastProvenOriginalQueueSwapchain.compare_exchange_strong(", exactQueueAssociation);
    const size_t swapchainQueueWrite = text.find("dx12_hook_g_SwapchainQueue = pQueue;", exactQueueAssociation);
    ASSERT_NE(queueLock, std::string::npos);
    ASSERT_NE(exactQueueAssociation, std::string::npos);
    ASSERT_NE(staleNativeAssociationClear, std::string::npos);
    ASSERT_NE(swapchainQueueWrite, std::string::npos);
    EXPECT_LT(queueLock, exactQueueAssociation);
    EXPECT_LT(exactQueueAssociation, staleNativeAssociationClear);
    EXPECT_LT(staleNativeAssociationClear, swapchainQueueWrite);
    EXPECT_LT(exactQueueAssociation, swapchainQueueWrite)
        << "queue ownership and its exact swapchain identity must share one publication boundary";

    const size_t captureSwapchainQueue = text.find("void CaptureSwapchainQueueFromCreateDevice(");
    ASSERT_NE(captureSwapchainQueue, std::string::npos);
    const size_t capturedOnOriginalQueue = text.find("bool capturedOnOriginalQueue = false;", captureSwapchainQueue);
    ASSERT_NE(capturedOnOriginalQueue, std::string::npos);
    const size_t normalIdentityLock =
        text.find("std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);", capturedOnOriginalQueue);
    const size_t rememberNormalIdentity =
        text.find("RememberOriginalQueueSwapchainIdentity(pSwapChain", normalIdentityLock);
    ASSERT_NE(normalIdentityLock, std::string::npos);
    ASSERT_NE(rememberNormalIdentity, std::string::npos);
    EXPECT_LT(normalIdentityLock, rememberNormalIdentity)
        << "normal queue verification and exact native identity publication must share one lock boundary";

    const size_t postSLRender = text.find("void PostSLOverlayRender(IDXGISwapChain* pSwapChain) {");
    ASSERT_NE(postSLRender, std::string::npos);
    const size_t postSubmitHealth = text.find("HRESULT postDevReason = dev->GetDeviceRemovedReason();", postSLRender);
    const size_t healthySuccessfulSubmit =
        text.find("if (SUCCEEDED(postDevReason) && rendered && pSwapChain && submittedQueue)", postSubmitHealth);
    const size_t successfulSubmitSequence =
        text.find("++dx12_hook_s_PostSLSuccessfulSubmitSequence;", healthySuccessfulSubmit);
    const size_t exactPostSLProof =
        text.find("dx12_hook_g_LastSuccessfulPostSLSwapchain.exchange(pSwapChain", successfulSubmitSequence);
    const size_t confirmedPostSL =
        text.find("dx12_hook_g_PostSLConfirmedRenderInCurrentReactivationEpoch.store(true", exactPostSLProof);
    ASSERT_NE(postSubmitHealth, std::string::npos);
    ASSERT_NE(healthySuccessfulSubmit, std::string::npos);
    ASSERT_NE(successfulSubmitSequence, std::string::npos);
    ASSERT_NE(exactPostSLProof, std::string::npos);
    ASSERT_NE(confirmedPostSL, std::string::npos);
    EXPECT_LT(postSubmitHealth, healthySuccessfulSubmit);
    EXPECT_LT(healthySuccessfulSubmit, successfulSubmitSequence);
    EXPECT_LT(successfulSubmitSequence, exactPostSLProof);
    EXPECT_LT(exactPostSLProof, confirmedPostSL)
        << "the OFF callback must not observe confirmation before exact proxy ownership proof";
}

TEST(DXGISharedSourceTest, ExactExplicitOffProxyUsesLastSuccessfulQueueAheadOfAnyStaleEpochLock) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    const size_t postSLRender = text.find("void PostSLOverlayRender(IDXGISwapChain* pSwapChain) {");
    const size_t exactQueueSelection =
        text.find("ShouldUsePostSLLastWorkingQueueForExactExplicitOffKeepAlive(", postSLRender);
    const size_t staleLockedQueueFallback =
        text.find("} else if (dx12_hook_g_PostSLLockedQueue) {", exactQueueSelection);
    ASSERT_NE(postSLRender, std::string::npos);
    ASSERT_NE(exactQueueSelection, std::string::npos);
    ASSERT_NE(staleLockedQueueFallback, std::string::npos);
    EXPECT_LT(exactQueueSelection, staleLockedQueueFallback);

    const size_t lockedQueueMutation =
        text.find("ShouldUsePostSLLastWorkingQueueForExactExplicitOffKeepAlive(", staleLockedQueueFallback);
    const size_t selectedQueueMatch = text.find("const bool selectedQueueMatchesLockedQueue", lockedQueueMutation);
    ASSERT_NE(lockedQueueMutation, std::string::npos);
    ASSERT_NE(selectedQueueMatch, std::string::npos);
    EXPECT_LT(lockedQueueMutation, selectedQueueMatch)
        << "the exact retained queue must be allowed to replace a stale epoch lock before mutation is decided";
}

TEST(DXGISharedSourceTest, ExactExplicitOffDirectDrawSuppressesOnlySameThreadNestedPresentDuplicate) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "common" / "dxgi_shared.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    const size_t present = text.find(
        "HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {");
    const size_t presentScope = text.find("BeginPostSLOffKeepAlivePresentScope();", present);
    const size_t wrappedPassThrough = text.find("if (ctx.wrappedSwapchain) {", presentScope);
    const size_t wrappedKeepAlive =
        text.find("DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(", wrappedPassThrough);
    const size_t wrappedOriginalPresent = text.find("CallOriginalPresent(pSwapChain", wrappedKeepAlive);
    const size_t recursivePresent = text.find("if (IsRecursivePresent()) {", present);
    const size_t recursivePresentDedup =
        text.find("if (postSLCallback && !WasPostSLOffKeepAlivePrePresentDrawn())", recursivePresent);
    const size_t processPresent =
        text.find("HandleDX12ProcessFrame(pSwapChain, applicationSourcePresent, frameGenerationPresentationActive);",
                  presentScope);
    ASSERT_NE(present, std::string::npos);
    ASSERT_NE(presentScope, std::string::npos);
    ASSERT_NE(wrappedPassThrough, std::string::npos);
    ASSERT_NE(wrappedKeepAlive, std::string::npos);
    ASSERT_NE(wrappedOriginalPresent, std::string::npos);
    ASSERT_NE(recursivePresent, std::string::npos);
    ASSERT_NE(recursivePresentDedup, std::string::npos);
    ASSERT_NE(processPresent, std::string::npos);
    EXPECT_LT(presentScope, wrappedPassThrough);
    EXPECT_LT(wrappedPassThrough, wrappedKeepAlive);
    EXPECT_LT(wrappedKeepAlive, wrappedOriginalPresent);
    EXPECT_LT(recursivePresent, recursivePresentDedup);
    EXPECT_LT(presentScope, recursivePresentDedup);
    EXPECT_LT(presentScope, processPresent);

    const size_t present1 =
        text.find("HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,",
                  present);
    const size_t present1Scope = text.find("BeginPostSLOffKeepAlivePresentScope();", present1);
    const size_t wrappedPresent1PassThrough =
        text.find("pSwapChain->QueryInterface(IID_CWrapDXGISwapChain", present1Scope);
    const size_t wrappedPresent1KeepAlive =
        text.find("DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(", wrappedPresent1PassThrough);
    const size_t wrappedOriginalPresent1 = text.find("CallOriginalPresent1(pSwapChain", wrappedPresent1KeepAlive);
    const size_t recursivePresent1 = text.find("if (IsRecursivePresent()) {", present1);
    const size_t recursivePresent1Dedup =
        text.find("if (postSLCallback && !WasPostSLOffKeepAlivePrePresentDrawn())", recursivePresent1);
    const size_t processPresent1 =
        text.find("HandleDX12ProcessFrame(pSwapChain, applicationSourcePresent, frameGenerationPresentationActive);",
                  present1Scope);
    ASSERT_NE(present1, std::string::npos);
    ASSERT_NE(present1Scope, std::string::npos);
    ASSERT_NE(wrappedPresent1PassThrough, std::string::npos);
    ASSERT_NE(wrappedPresent1KeepAlive, std::string::npos);
    ASSERT_NE(wrappedOriginalPresent1, std::string::npos);
    ASSERT_NE(recursivePresent1, std::string::npos);
    ASSERT_NE(recursivePresent1Dedup, std::string::npos);
    ASSERT_NE(processPresent1, std::string::npos);
    EXPECT_LT(present1Scope, wrappedPresent1PassThrough);
    EXPECT_LT(wrappedPresent1PassThrough, wrappedPresent1KeepAlive);
    EXPECT_LT(wrappedPresent1KeepAlive, wrappedOriginalPresent1);
    EXPECT_LT(recursivePresent1, recursivePresent1Dedup);
    EXPECT_LT(present1Scope, recursivePresent1Dedup);
    EXPECT_LT(present1Scope, processPresent1);

    const size_t threadLocalScope = text.find("thread_local uint32_t s_postSLOffKeepAlivePresentScopeDepth");
    const size_t markFunction = text.find("void MarkPostSLOffKeepAlivePrePresentDrawn()", threadLocalScope);
    const size_t markOnlyInsideScope = text.find("if (s_postSLOffKeepAlivePresentScopeDepth != 0)", markFunction);
    ASSERT_NE(threadLocalScope, std::string::npos);
    ASSERT_NE(markFunction, std::string::npos);
    ASSERT_NE(markOnlyInsideScope, std::string::npos);
    EXPECT_LT(threadLocalScope, markFunction);
    EXPECT_LT(markFunction, markOnlyInsideScope)
        << "a generated frame on another worker thread must retain its independent PostSL draw";
}

TEST(DXGISharedSourceTest, WrapperPresentScopeSpansProcessFrameAndRealPresentForExactOffDeduplication) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "wrappers" / "dxgi_swapchain_wrap.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    const size_t present = text.find("HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present(");
    const size_t presentScope = text.find("DXGIShared::BeginPostSLOffKeepAlivePresentScope();", present);
    const size_t processPresent = text.find("DX12_ProcessFrameExternal(pRealCached);", presentScope);
    const size_t realPresent = text.find("pRealCached->Present(SyncInterval, presentFlags)", processPresent);
    ASSERT_NE(present, std::string::npos);
    ASSERT_NE(presentScope, std::string::npos);
    ASSERT_NE(processPresent, std::string::npos);
    ASSERT_NE(realPresent, std::string::npos);
    EXPECT_LT(presentScope, processPresent);
    EXPECT_LT(processPresent, realPresent);

    const size_t present1 = text.find("HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present1(", realPresent);
    const size_t present1Scope = text.find("DXGIShared::BeginPostSLOffKeepAlivePresentScope();", present1);
    const size_t processPresent1 = text.find("DX12_ProcessFrameExternal(pReal1Cached);", present1Scope);
    const size_t realPresent1 =
        text.find("pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters)", processPresent1);
    ASSERT_NE(present1, std::string::npos);
    ASSERT_NE(present1Scope, std::string::npos);
    ASSERT_NE(processPresent1, std::string::npos);
    ASSERT_NE(realPresent1, std::string::npos);
    EXPECT_LT(present1Scope, processPresent1);
    EXPECT_LT(processPresent1, realPresent1);
}

TEST(DXGISharedSourceTest, WrappedPassThroughDrivesOnlySuccessfulExactProxyKeepAliveBeforePresent) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    const size_t helper = text.find("bool DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(");
    const size_t latch = text.find("const bool keepAliveLatched", helper);
    const size_t samePresentDedup = text.find("WasPostSLOffKeepAlivePrePresentDrawn()", latch);
    const size_t queueLock = text.find("g_CommandQueueMutex", samePresentDedup);
    const size_t exactPolicy = text.find("ShouldDriveExactPostSLOffKeepAliveBeforePresent(", queueLock);
    const size_t submit = text.find("PostSLOverlayRenderGated(pSwapChain);", exactPolicy);
    const size_t success = text.find("const bool submitted", submit);
    const size_t mark = text.find("DXGIShared::MarkPostSLOffKeepAlivePrePresentDrawn();", success);
    ASSERT_NE(helper, std::string::npos);
    ASSERT_NE(latch, std::string::npos);
    ASSERT_NE(samePresentDedup, std::string::npos);
    ASSERT_NE(queueLock, std::string::npos);
    ASSERT_NE(exactPolicy, std::string::npos);
    ASSERT_NE(submit, std::string::npos);
    ASSERT_NE(success, std::string::npos);
    ASSERT_NE(mark, std::string::npos);
    EXPECT_LT(latch, queueLock) << "steady-state Presents must fast-return before taking the queue lock";
    EXPECT_LT(samePresentDedup, queueLock);
    EXPECT_LT(queueLock, exactPolicy);
    EXPECT_LT(exactPolicy, submit);
    EXPECT_LT(submit, success);
    EXPECT_LT(success, mark) << "only a real successful submit may suppress the same-present nested callback";
}

TEST(DXGISharedSourceTest, NormalCommandSubmitCannotRetireExactOffKeepAliveWithoutPresentationOwnershipProof) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    EXPECT_EQ(text.find("RetirePostSLExplicitOffKeepAliveAfterNormalRouteDraw"), std::string::npos);

    const size_t callback = text.find("void PostSLOverlayRenderGated(");
    const size_t streamlineGone = text.find("const bool streamlineGone = !IsStreamlineLoaded();", callback);
    const size_t unloadRetirement = text.find("PostSL keep-alive retired after Streamline unload", streamlineGone);
    ASSERT_NE(callback, std::string::npos);
    ASSERT_NE(streamlineGone, std::string::npos);
    ASSERT_NE(unloadRetirement, std::string::npos);

    const size_t authoritativeRetirement = text.find("HandlePostSLRouteForNormalSwapchainReturn(");
    const size_t normalReturnPolicy =
        text.find("ShouldRetirePostSLRouteForNormalSwapchainReturn(", authoritativeRetirement);
    ASSERT_NE(authoritativeRetirement, std::string::npos);
    ASSERT_NE(normalReturnPolicy, std::string::npos);

    const size_t warmResumeArm =
        text.find("dx12_hook_g_PostSLWarmResumePreservationPending.store(callbackAlreadyInstalled &&");
    const size_t successfulPostSLSubmit = text.find("if (SUCCEEDED(postDevReason) && rendered && pSwapChain");
    const size_t warmResumeCompletion =
        text.find("dx12_hook_g_PostSLWarmResumePreservationPending.exchange(false", successfulPostSLSubmit);
    ASSERT_NE(warmResumeArm, std::string::npos);
    ASSERT_NE(successfulPostSLSubmit, std::string::npos);
    ASSERT_NE(warmResumeCompletion, std::string::npos);
    // The arming site lives in DX12_OnStreamlineFGStateChanged, which now
    // resides in a different unit than PostSLOverlayRender; the physical
    // order in the logical source no longer reflects program flow, so only
    // the completion-after-submit ordering is asserted.
    EXPECT_LT(successfulPostSLSubmit, warmResumeCompletion)
        << "the warm-resume marker must be proof-completed by a real PostSL submit, not a timer or pointer event";
}

TEST(DXGISharedTest, ThirdPartyOverlayECLQueueDoesNotOverrideKnownGameTrackingQueues) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlayQueueForGameTracking(true, true, false, false, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlayQueueForGameTracking(false, true, false, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlayQueueForGameTracking(true, false, false, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlayQueueForGameTracking(true, true, true, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlayQueueForGameTracking(true, true, false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlayQueueForGameTracking(true, true, false, false, true));
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

TEST(DXGISharedTest, DX12SwapchainOverlayRoutingUsesLastWorkingQueueOnlyDuringPostFSRInactiveRecoveryEpoch) {
    using ce::dx12_overlay_policy::DecideSwapchainOverlayRouting;
    using ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision;

    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, false, true, false, false, false),
              SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue);

    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, false, true, true, true, false),
              SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue);

    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, false, true, true, true, true),
              SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue);

    // A clean non-FG swapchain return has ended the recovery epoch. The retained
    // PostSL pointer is now historical and must not receive replacement-swapchain
    // overlay work, regardless of the current ECL/primary queue relationship.
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, false, true, true, false, false),
              SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue);
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, false, true, true, false, true),
              SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue);

    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, true, true, false, false, false),
              SwapchainOverlayRoutingDecision::kUseNormalRouting);
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, false, false, true, false, false, false),
              SwapchainOverlayRoutingDecision::kUseNormalRouting);
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, false, false, false, false, false),
              SwapchainOverlayRoutingDecision::kUseNormalRouting);
}

TEST(DXGISharedTest, DX12SwapchainOverlayRoutingCoversEveryOffFSRAndDLSSDirection) {
    using ce::dx12_overlay_policy::DecideSwapchainOverlayRouting;
    using ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision;

    // Fresh OFF and direct OFF -> FSR.
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, false, true, true, false, false, true),
              SwapchainOverlayRoutingDecision::kUseNormalRouting);
    EXPECT_EQ(DecideSwapchainOverlayRouting(true, false, true, true, true, true, false, false, true),
              SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue);

    // FSR -> OFF recovery, OFF -> FSR re-enable, and direct FSR -> DLSS.
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, false, true, false, true, true),
              SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue);
    EXPECT_EQ(DecideSwapchainOverlayRouting(true, false, true, true, true, true, false, true, true),
              SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue);
    EXPECT_EQ(DecideSwapchainOverlayRouting(true, true, false, true, true, true, true, true, true),
              SwapchainOverlayRoutingDecision::kUsePostFSRStreamlineQueue);

    // DLSS -> OFF keeps the proven transition queue only inside recovery. The
    // clean OFF return and a later OFF -> DLSS must use original-queue proof.
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, false, true, true, true, true),
              SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue);
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, false, true, true, false, true),
              SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue);
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, true, false, true, false, true, true, false, true),
              SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue);

    // DLSS -> FSR and the pure-DLSS OFF -> ON -> OFF family.
    EXPECT_EQ(DecideSwapchainOverlayRouting(true, false, true, true, true, true, true, false, true),
              SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue);
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, true, false, false, true, true, false, false, true),
              SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue);
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, false, true, true, false, false, true),
              SwapchainOverlayRoutingDecision::kUseNormalRouting);
}

TEST(DXGISharedTest, ExplicitNativeFSROffRecoveryUsesOriginalQueueDespiteStaleSwapchainQueue) {
    using ce::dx12_overlay_policy::DecideSwapchainOverlayRouting;
    using ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision;

    EXPECT_EQ(DecideSwapchainOverlayRouting(true, false, false, true, true, true, false, false, false, true),
              SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue);

    // Without an original game queue, keep the existing conservative
    // runtime-owned routing because there is no proven normal Present queue.
    EXPECT_EQ(DecideSwapchainOverlayRouting(true, false, false, true, true, false, false, false, false, true),
              SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue);
}

TEST(DXGISharedTest, SuspendedNoCallbackNativeFSRKeepsFSRSwapchainQueueRouting) {
    using ce::dx12_overlay_policy::DecideSwapchainOverlayRouting;
    using ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision;

    // Suspended native FSR (explicit disabled configure) on AMD's internal
    // no-callback composition route: the runtime-owned swapchain is still the
    // live present path, so the overlay must keep rendering on the FSR
    // swapchain queue instead of being routed into post-FSR recovery
    // (20260611_142923: overlay disappeared forever during FSR FG suspension).
    EXPECT_EQ(DecideSwapchainOverlayRouting(true, false, false, true, true, true, false, false, false, true, true),
              SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue);

    // Without a captured swapchain queue there is no safe submit target —
    // skip instead of falling back to the original game queue cross-queue.
    EXPECT_EQ(DecideSwapchainOverlayRouting(true, false, false, true, false, true, false, false, false, true, true),
              SwapchainOverlayRoutingDecision::kSkipFSRWithoutSwapchainQueue);

    // A stale no-callback latch without live runtime ownership must keep the
    // proven post-FSR-inactive original-queue recovery.
    EXPECT_EQ(DecideSwapchainOverlayRouting(false, false, false, true, true, true, false, false, false, true, true),
              SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue);

    // Active Streamline routing keeps precedence over the suspension branch.
    EXPECT_EQ(DecideSwapchainOverlayRouting(true, true, false, true, true, true, false, false, false, true, true),
              SwapchainOverlayRoutingDecision::kUsePostFSRStreamlineQueue);
}

TEST(DXGISharedTest, LiveNoCallbackNativeFSRSuspensionToggleSkipsTransitionCooldown) {
    using ce::fg_runtime::RuntimeMode;

    // Suspend and resume edges of a live no-callback native-FSR session flip
    // only the FG flag; the draw cooldown would blank the overlay for ~60
    // frames at every menu-style toggle.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldStartFrameGenerationTransitionCooldown(
        RuntimeMode::kFSRFG, RuntimeMode::kOff, true, false, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldStartFrameGenerationTransitionCooldown(
        RuntimeMode::kOff, RuntimeMode::kFSRFG, false, true, false, false, true));

    // The same transitions without the live no-callback suspension shape keep
    // the protective cooldown.
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldStartFrameGenerationTransitionCooldown(
        RuntimeMode::kFSRFG, RuntimeMode::kOff, true, false, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldStartFrameGenerationTransitionCooldown(
        RuntimeMode::kOff, RuntimeMode::kFSRFG, false, true, false, false, false));
}

TEST(DXGISharedTest, LiveNoCallbackNativeFSRSuspensionToggleRequiresExactShape) {
    using ce::dx12_overlay_policy::IsLiveNoCallbackNativeFSRSuspensionToggle;
    using ce::fg_runtime::RuntimeMode;

    EXPECT_TRUE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kFSRFG, RuntimeMode::kOff, false, true, true,
                                                          true, true, true));
    EXPECT_TRUE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kOff, RuntimeMode::kFSRFG, false, true, true,
                                                          true, true, true));

    // Streamline running, missing latch, missing ownership, missing queue,
    // uninitialized backend, or a backend on a different queue (early enable
    // edge before the FFX swapchain goes live) must all keep the cooldown.
    EXPECT_FALSE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kFSRFG, RuntimeMode::kOff, true, true, true,
                                                           true, true, true));
    EXPECT_FALSE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kFSRFG, RuntimeMode::kOff, false, false, true,
                                                           true, true, true));
    EXPECT_FALSE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kFSRFG, RuntimeMode::kOff, false, true, false,
                                                           true, true, true));
    EXPECT_FALSE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kFSRFG, RuntimeMode::kOff, false, true, true,
                                                           false, true, true));
    EXPECT_FALSE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kFSRFG, RuntimeMode::kOff, false, true, true,
                                                           true, false, true));
    EXPECT_FALSE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kFSRFG, RuntimeMode::kOff, false, true, true,
                                                           true, true, false));

    // Only FSR_FG <-> Off toggles qualify; DLSS transitions keep their paths.
    EXPECT_FALSE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kFSRFG, RuntimeMode::kDLSSFG, false, true, true,
                                                           true, true, true));
    EXPECT_FALSE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kDLSSFG, RuntimeMode::kOff, false, true, true,
                                                           true, true, true));
}

TEST(DXGISharedSourceTest, GetStateFirstPostFSRComebackClearsStaleNativeOwnershipOnExplicitUpgrade) {
    namespace fs = std::filesystem;
    const std::string streamline =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "apis" / "streamline_hook.cpp");
    const std::string dx12 =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "apis" / "dx12_hook.cpp");
    ASSERT_FALSE(streamline.empty());
    ASSERT_FALSE(dx12.empty());

    const size_t upgrade = streamline.find(
        "if (!previousExplicitSetOptionsActivation && updatedExplicitSetOptionsActivation && "
        "signalUpdate.effectiveActive");
    const size_t ownershipRefresh =
        streamline.find("DX12_OnStreamlineExplicitSetOptionsActivationConfirmed();", upgrade);
    const size_t ordinaryEdge = streamline.find("if (previousSignalObserved != signalUpdate.effectiveActive)", upgrade);
    ASSERT_NE(upgrade, std::string::npos);
    ASSERT_NE(ownershipRefresh, std::string::npos);
    ASSERT_NE(ordinaryEdge, std::string::npos);
    EXPECT_LT(ownershipRefresh, ordinaryEdge)
        << "an in-place GetState-to-SetOptions provenance upgrade has no second ON edge";

    const size_t staleOwnershipHelper = dx12.find(
        "bool explicitSetOptionsActivation, bool authoritativeStreamlineHandoff, const char* source) {");
    const size_t staleOwnershipPolicy = dx12.find(
        "ShouldClearStaleNativeFGPresentOwnershipOnStreamlineComeback(", staleOwnershipHelper);
    const size_t noCallbackOwnershipProof =
        dx12.find("DX12_IsNativeFSRInternalNoCallbackCompositionActive()", staleOwnershipHelper);
    const size_t clearNoCallback = dx12.find("ForceClearNativeFSRInternalNoCallbackComposition(", staleOwnershipPolicy);
    const size_t implementation =
        dx12.find("void DX12_OnStreamlineExplicitSetOptionsActivationConfirmed()", clearNoCallback);
    const size_t helperCall =
        dx12.find("ClearStaleNativeFGPresentOwnershipForStreamlineComebackLocked(", implementation);
    ASSERT_NE(staleOwnershipHelper, std::string::npos);
    ASSERT_NE(staleOwnershipPolicy, std::string::npos);
    ASSERT_NE(noCallbackOwnershipProof, std::string::npos);
    ASSERT_NE(clearNoCallback, std::string::npos);
    EXPECT_LT(noCallbackOwnershipProof, staleOwnershipPolicy);
    ASSERT_NE(implementation, std::string::npos);
    ASSERT_NE(helperCall, std::string::npos);
}

TEST(DXGISharedSourceTest, ExactPostSLOffKeepAliveRunsBeforeEveryTopLevelDX12PresentRoute) {
    namespace fs = std::filesystem;
    const std::string text =
        ce::test_source::ReadLogicalSource(fs::current_path() / "hook" / "common" / "dxgi_shared.cpp");
    ASSERT_FALSE(text.empty());

    const size_t present = text.find(
        "HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {");
    const size_t presentScope = text.find("BeginPostSLOffKeepAlivePresentScope();", present);
    const size_t presentKeepAlive = text.find("\"DXGIShared::DetourPresent pre-routing\"", presentScope);
    const size_t presentRouting = text.find("const void* detourCallerAddress", presentScope);
    ASSERT_NE(present, std::string::npos);
    ASSERT_NE(presentScope, std::string::npos);
    ASSERT_NE(presentKeepAlive, std::string::npos);
    ASSERT_NE(presentRouting, std::string::npos);
    EXPECT_LT(presentScope, presentKeepAlive);
    EXPECT_LT(presentKeepAlive, presentRouting);

    const size_t present1 = text.find(
        "HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,",
        presentRouting);
    const size_t present1Scope = text.find("BeginPostSLOffKeepAlivePresentScope();", present1);
    const size_t present1KeepAlive = text.find("\"DXGIShared::DetourPresent1 pre-routing\"", present1Scope);
    const size_t present1Routing = text.find("const void* detourCallerAddress", present1Scope);
    ASSERT_NE(present1, std::string::npos);
    ASSERT_NE(present1Scope, std::string::npos);
    ASSERT_NE(present1KeepAlive, std::string::npos);
    ASSERT_NE(present1Routing, std::string::npos);
    EXPECT_LT(present1Scope, present1KeepAlive);
    EXPECT_LT(present1KeepAlive, present1Routing);
}
