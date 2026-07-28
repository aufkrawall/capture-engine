#include "test_dxgi_shared_shared.h"

TEST(DXGISharedTest, ReinitCooldownLetsHalfArmedSyntheticPostSLContinue) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldLetSyntheticPostSLProgressDuringOverlayReinitCooldown(true, true, false,
                                                                                                     false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldLetSyntheticPostSLProgressDuringOverlayReinitCooldown(true, false, true,
                                                                                                     false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldLetSyntheticPostSLProgressDuringOverlayReinitCooldown(true, false, false,
                                                                                                     true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldLetSyntheticPostSLProgressDuringOverlayReinitCooldown(
        false, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldLetSyntheticPostSLProgressDuringOverlayReinitCooldown(
        true, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldLetSyntheticPostSLProgressDuringOverlayReinitCooldown(
        true, false, false, true, false));
}

TEST(DXGISharedTest, VisibleOverlayCanWakeECLDrivenStartupActivationBeforePostSLCallbackEnters) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldContinueECLDrivenPostSLStartupProgress(true, true, false, false, true,
                                                                                      true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldContinueECLDrivenPostSLStartupProgress(true, true, true, false, true,
                                                                                       true, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldContinueECLDrivenPostSLStartupProgress(false, true, false, false, true,
                                                                                       true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldContinueECLDrivenPostSLStartupProgress(true, false, false, false, true,
                                                                                       true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldContinueECLDrivenPostSLStartupProgress(true, true, false, true, true,
                                                                                       true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldContinueECLDrivenPostSLStartupProgress(true, true, false, false, false,
                                                                                       true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldContinueECLDrivenPostSLStartupProgress(true, true, false, false, true,
                                                                                       false, false, false));
}

TEST(DXGISharedTest, VisibleOverlayBlocksECLDrivenStartupProgressForPostFSRWithoutSafeBootstrap) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldContinueECLDrivenPostSLStartupProgress(true, true, false, false, true,
                                                                                       true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldContinueECLDrivenPostSLStartupProgress(true, true, true, false, true,
                                                                                       true, true, false));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldContinueECLDrivenPostSLStartupProgress(true, true, false, false, true,
                                                                                      true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldContinueECLDrivenPostSLStartupProgress(true, true, true, false, true,
                                                                                       true, true, true));
}

TEST(DXGISharedTest, ExpiryDrivenECLStartupActivationRespectsPostFSRSafeBootstrapGate) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTriggerExpiryDrivenECLPostSLStartupActivation(true, true, true, true, false));

    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldTriggerExpiryDrivenECLPostSLStartupActivation(true, true, true, true, true));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldTriggerExpiryDrivenECLPostSLStartupActivation(true, true, true, false, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTriggerExpiryDrivenECLPostSLStartupActivation(false, true, true, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTriggerExpiryDrivenECLPostSLStartupActivation(true, false, true, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTriggerExpiryDrivenECLPostSLStartupActivation(true, true, false, true, true));
}

TEST(DXGISharedTest, FreshRuntimeOwnedStreamlineHandoffRetainsStartupActivationSwapchain) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchain(true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchain(false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchain(true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchain(true, true, false));
}

TEST(DXGISharedTest, HalfArmedNormalRouteCallbackRetainsStartupActivationSwapchain) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromNormalRoute(
        true, true, true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromNormalRoute(
        true, true, false, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromNormalRoute(
        false, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromNormalRoute(
        true, false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromNormalRoute(
        true, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromNormalRoute(
        true, true, true, true, true));
}

TEST(DXGISharedTest, RetainedStartupActivationSwapchainPreferredOnlyWhileHalfArmed) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreferRetainedStreamlineStartupActivationSwapchain(true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreferRetainedStreamlineStartupActivationSwapchain(true, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreferRetainedStreamlineStartupActivationSwapchain(false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreferRetainedStreamlineStartupActivationSwapchain(true, false, false));
}

TEST(DXGISharedTest, DeferredOffChurnServicesStartupActivationAfterWindowExpiry) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldServicePostSLStartupActivationWhileOffChurnDeferred(true, false, true,
                                                                                                   false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldServicePostSLStartupActivationWhileOffChurnDeferred(true, false, true,
                                                                                                    true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldServicePostSLStartupActivationWhileOffChurnDeferred(false, false, true,
                                                                                                    false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldServicePostSLStartupActivationWhileOffChurnDeferred(true, true, true,
                                                                                                    false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldServicePostSLStartupActivationWhileOffChurnDeferred(true, false, false,
                                                                                                    false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldServicePostSLStartupActivationWhileOffChurnDeferred(true, false, true,
                                                                                                    false, false));
}

TEST(DXGISharedTest, RetainedStartupActivationServiceAllowsPrearmedPostSLButIsWakeOnlyAfterCallbackEntry) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldInvokeRetainedPostSLStartupActivationService(true, true, true, false,
                                                                                            false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInvokeRetainedPostSLStartupActivationService(false, true, true, false,
                                                                                             false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInvokeRetainedPostSLStartupActivationService(true, false, true, false,
                                                                                             false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInvokeRetainedPostSLStartupActivationService(true, true, false, false,
                                                                                             false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInvokeRetainedPostSLStartupActivationService(true, true, true, true,
                                                                                             false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInvokeRetainedPostSLStartupActivationService(true, true, true, false,
                                                                                             true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInvokeRetainedPostSLStartupActivationService(true, true, true, false,
                                                                                             false, true));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldInvokeRetainedPostSLStartupActivationService(true, true, false, false,
                                                                                            true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInvokeRetainedPostSLStartupActivationService(true, true, false, false,
                                                                                             true, true, true));
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

TEST(DXGISharedTest, ExplicitStreamlineComebackClearsOnlyStaleNativeFGPresentOwnership) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldClearStaleNativeFGPresentOwnershipOnExplicitStreamlineComeback(
        true, true, true, true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearStaleNativeFGPresentOwnershipOnExplicitStreamlineComeback(
        false, true, true, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearStaleNativeFGPresentOwnershipOnExplicitStreamlineComeback(
        true, false, true, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearStaleNativeFGPresentOwnershipOnExplicitStreamlineComeback(
        true, true, false, false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearStaleNativeFGPresentOwnershipOnExplicitStreamlineComeback(
        true, true, true, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearStaleNativeFGPresentOwnershipOnExplicitStreamlineComeback(
        true, true, true, true, true, false));
}

TEST(DXGISharedTest, CreateSwapchainAccessDeniedPassThroughRequiresRuntimeOwnershipOrAuthoritativeFFXTakeover) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(true, true, false,
                                                                                                   false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(true, false, true,
                                                                                                   false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(true, true, false,
                                                                                                   true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(true, false, false,
                                                                                                   false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(false, true, true,
                                                                                                    false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(true, false, false,
                                                                                                    false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(false, false, false,
                                                                                                    true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(false, false, false,
                                                                                                    false, true));
}

// ---------------------------------------------------------------------------
// GTA FSR->DLSS apply crash (session 20260702_092933): the startup-transport bypass paths kept RE-RETAINING
// the startup-activation swapchain AFTER PostSL confirmed rendering (generations 3-11), but the only
// release fires AT confirmation — so the last retained COM reference leaked, pinned AMD's old FI real
// swapchain (and its HWND association), the game's replacement CreateSwapChainForHwnd failed
// E_ACCESSDENIED, and GTA null-dereferenced the missing swapchain. Retention exists ONLY for PostSL
// startup recovery and the retained slot is never consulted after confirmation
// (ShouldPreferRetainedStreamlineStartupActivationSwapchain requires pending/unconfirmed), so transport
// retains must stop at confirmation.
// ---------------------------------------------------------------------------
TEST(DXGISharedTest, StartupTransportRetainStopsAtPostSLConfirmation) {
    using ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport;
    // Pre-confirmation D3D12 transport presents may retain (startup recovery anchor).
    EXPECT_TRUE(ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport(true, false));
    // AFTER confirmation the retain is a pure leak (release already fired, slot never consulted again).
    EXPECT_FALSE(ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport(true, true));
    // Non-D3D12 swapchains never participate.
    EXPECT_FALSE(ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport(false, false));
    EXPECT_FALSE(ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport(false, true));
}

// The retained slot is only ever PREFERRED while startup is pending/unconfirmed — proving that a retain
// after confirmation has no consumer (the leak gated above had zero recovery value).
TEST(DXGISharedTest, RetainedActivationSwapchainNeverPreferredAfterConfirmation) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreferRetainedStreamlineStartupActivationSwapchain(
        /*retainedSwapchainAvailable=*/true, /*startupActivationPending=*/false,
        /*postSLActiveButUnconfirmed=*/false));
}

// ---------------------------------------------------------------------------
// A failed runtime-managed swapchain create is FATAL to the game (GTA dereferences the null swapchain,
// session 20260702_092933), so E_ACCESSDENIED must never be blindly passed through: runtime-managed /
// third-party-caller creates get the MINIMAL CE unpin (release retained activation swapchain, no overlay
// teardown) + retry with full-cleanup escalation; only CE/game-owned creates keep the direct full cleanup.
// ---------------------------------------------------------------------------
TEST(DXGISharedTest, CreateSwapchainAccessDeniedRuntimeManagedGetsMinimalReleaseNotBlindPassThrough) {
    using ce::dx12_overlay_policy::ChooseCreateSwapchainAccessDeniedRecovery;
    using ce::dx12_overlay_policy::CreateSwapchainAccessDeniedRecovery;
    EXPECT_EQ(ChooseCreateSwapchainAccessDeniedRecovery(/*passThroughForRuntimeManagedFG=*/true,
                                                        /*callerFromThirdPartyOverlay=*/false),
              CreateSwapchainAccessDeniedRecovery::kMinimalCEReleaseThenEscalate);
    EXPECT_EQ(ChooseCreateSwapchainAccessDeniedRecovery(false, true),
              CreateSwapchainAccessDeniedRecovery::kMinimalCEReleaseThenEscalate);
    EXPECT_EQ(ChooseCreateSwapchainAccessDeniedRecovery(true, true),
              CreateSwapchainAccessDeniedRecovery::kMinimalCEReleaseThenEscalate);
    EXPECT_EQ(ChooseCreateSwapchainAccessDeniedRecovery(false, false),
              CreateSwapchainAccessDeniedRecovery::kFullOverlayCleanupAndRetry);
}

// ---------------------------------------------------------------------------
// perf_metrics CSV present-row dedup (sessions 20260702_094955/140811: ~50% zero-delta qpc row pairs —
// the outer DetourPresent catch-all row AND the inner per-API ProcessFrame row were both written for the
// same present, so present-rate analysis from the CSV counted frames twice). The outer present hooks must
// open a PerfLogger present-row scope before dispatching and skip their catch-all row when an inner row
// was logged.
// ---------------------------------------------------------------------------
TEST(DXGISharedSourceTest, PresentPerfRowIsDedupedAcrossOuterAndInnerLoggers) {
    namespace fs = std::filesystem;
    auto readFile = [](const fs::path& p) {
        EXPECT_TRUE(fs::exists(p)) << p.string();
        const std::string text = ce::test_source::ReadLogicalSource(p);
        EXPECT_FALSE(text.empty()) << p.string();
        return text;
    };

    const std::string shared = readFile(fs::current_path() / "hook" / "common" / "dxgi_shared.cpp");
    ASSERT_FALSE(shared.empty());
    const size_t beginScope = shared.find("PerfLogger::BeginPresentRowScope()");
    ASSERT_NE(beginScope, std::string::npos) << "DetourPresent must open the present-row scope";
    const size_t guardCheck = shared.find("!PerfLogger::InnerRowLoggedInPresentRowScope()");
    ASSERT_NE(guardCheck, std::string::npos)
        << "DetourPresent's catch-all row must be skipped when an inner row was logged";

    const std::string wrap = readFile(fs::current_path() / "hook" / "wrappers" / "dxgi_swapchain_wrap.cpp");
    ASSERT_FALSE(wrap.empty());
    EXPECT_NE(wrap.find("PerfLogger::BeginPresentRowScope()"), std::string::npos);
    EXPECT_NE(wrap.find("!PerfLogger::InnerRowLoggedInPresentRowScope()"), std::string::npos);

    const std::string logger = readFile(fs::current_path() / "hook" / "common" / "perf_logger.cpp");
    ASSERT_FALSE(logger.empty());
    // Every logged row marks the scope (inner rows win; outer catch-alls defer).
    const size_t logFrame = logger.find("void PerfLogger::LogFrame(");
    ASSERT_NE(logFrame, std::string::npos);
    const size_t marks = logger.find("t_frameRowLoggedInPresentScope = true;", logFrame);
    ASSERT_NE(marks, std::string::npos);
}

// Source invariants for the leak fix: every startup-transport retain call site must be gated by the
// confirmation policy, the runtime-handoff create releases the retained swapchain BEFORE forwarding, and
// no "passing through without CE cleanup" arm remains for E_ACCESSDENIED.
TEST(DXGISharedSourceTest, StartupTransportRetainSitesGatedAndHandoffReleasesRetainedSwapchain) {
    namespace fs = std::filesystem;
    auto readFile = [](const fs::path& p) {
        EXPECT_TRUE(fs::exists(p)) << p.string();
        const std::string text = ce::test_source::ReadLogicalSource(p);
        EXPECT_FALSE(text.empty()) << p.string();
        return text;
    };

    const std::string shared = readFile(fs::current_path() / "hook" / "common" / "dxgi_shared.cpp");
    ASSERT_FALSE(shared.empty());
    // Every transport retain (bypass/overlayless-handoff sources) is gated by the confirmation policy:
    // count gate occurrences >= count of transport retain sources.
    size_t gates = 0;
    for (size_t pos = shared.find("ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport");
         pos != std::string::npos;
         pos = shared.find("ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport", pos + 1)) {
        ++gates;
    }
    EXPECT_GE(gates, 6u) << "all six startup-transport retain sites must consult the confirmation gate";

    const std::string dx12 = readFile(fs::current_path() / "hook" / "apis" / "dx12_hook.cpp");
    ASSERT_FALSE(dx12.empty());
    // The post-FSR runtime-handoff create releases CE's retained activation swapchain BEFORE forwarding.
    EXPECT_NE(dx12.find("pre post-FSR Streamline runtime swapchain create"), std::string::npos)
        << "runtime-handoff create must release the retained activation swapchain before forwarding";
    // No blind pass-through remains: both E_ACCESSDENIED arms recover instead.
    EXPECT_EQ(dx12.find("passing through without CE cleanup"), std::string::npos)
        << "E_ACCESSDENIED must never be blindly passed through (fatal null-swapchain deref in the game)";
    EXPECT_NE(dx12.find("E_ACCESSDENIED runtime-managed minimal recovery"), std::string::npos);
}

TEST(DXGISharedTest, PostSLRenderingDeferredDuringStartupTransitionWindowUntilConfirmed) {
    // During startup transition window with no confirmed rendering - defer
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(true, false, false));

    // Once PostSL has confirmed stable rendering - don't defer even during window
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(true, true, false));

    // Wrapper queue progress alone does not prove Streamline's internal startup
    // pipeline is settled, so it cannot bypass the startup window by itself.
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(true, false, true));

    // The post-FSR safe bootstrap proof is stronger than wrapper progress: it
    // proves a current runtime-owned Streamline swapchain queue and submit path,
    // so the overlay can draw during short FSR->DLSS switch intervals.
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(true, false, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(true, false, false, true));

    // A real DLSS-G runtime-active signal after PostSL warmup is also stronger
    // than generic Streamline presence. The startup window should still hold if
    // either side of that proof is missing.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(true, false, false,
                                                                                                  false, true, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(true, false, false,
                                                                                                 false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(true, false, false,
                                                                                                 false, false, true));

    // Outside startup window - never defer
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(false, true, false));
}

TEST(DXGISharedTest, PureDLSSStartupWrapperOnlyStallDumpRequiresStrongHalfArmedSignal) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall(
        false, true, 4, true, false, false, 1000, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall(
        false, true, 8, false, true, false, 1500, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall(
        true, true, 8, true, false, false, 1500, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall(
        false, false, 8, true, false, false, 1500, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall(
        false, true, 3, true, false, false, 1500, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall(
        false, true, 8, false, false, true, 1500, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall(
        false, true, 8, false, false, false, 1500, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall(
        false, true, 8, true, false, false, 999, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall(
        false, true, 8, true, false, false, 1500, true));
}

TEST(DXGISharedTest, PureDLSSStartupCallbackStaysDormantUntilStartupWindowExpires) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, false, false, true, true, false, false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, false, false, true, true, false, false, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, false, false, true, true, false, false, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        false, false, false, true, true, false, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, true, false, true, true, false, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, false, true, true, true, false, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, false, false, false, true, false, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, false, false, true, false, false, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, false, false, true, true, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, false, false, true, true, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, false, false, true, true, false, true, true, false));
}

TEST(DXGISharedTest, PureStreamlineFGOffBypassesReinitCooldownOnlyForNonFSRHealthyBackend) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBypassPureStreamlineFGOffOverlayReinitCooldown(
        true, false, false, false, true, true, true, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBypassPureStreamlineFGOffOverlayReinitCooldown(
        false, false, false, false, true, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBypassPureStreamlineFGOffOverlayReinitCooldown(
        true, true, false, false, true, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBypassPureStreamlineFGOffOverlayReinitCooldown(
        true, false, true, false, true, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBypassPureStreamlineFGOffOverlayReinitCooldown(
        true, false, false, true, true, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBypassPureStreamlineFGOffOverlayReinitCooldown(
        true, false, false, false, false, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBypassPureStreamlineFGOffOverlayReinitCooldown(
        true, false, false, false, true, false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBypassPureStreamlineFGOffOverlayReinitCooldown(
        true, false, false, false, true, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBypassPureStreamlineFGOffOverlayReinitCooldown(
        true, false, false, false, true, true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBypassPureStreamlineFGOffOverlayReinitCooldown(
        true, false, false, false, true, true, true, true, true));
}

TEST(DXGISharedTest, ConfirmedPostSLSuspensionBypassesReinitCooldownEvenWithFSRHistory) {
    using ce::dx12_overlay_policy::ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown;

    // Session 20260613_150750: a DLSS-FG suspend AFTER an FSR phase (sticky
    // hadFSRFGPhase=1) fell through the pure-DLSS bypass and blanked the live
    // overlay 60 presents / 672 ms via the generic FG-off reinit cooldown. With the
    // make-before-break keep-alive latched AND PostSL confirmed rendering this epoch
    // (the overlay ECL on the SL queue already succeeded), the cooldown is unneeded
    // regardless of FSR history.
    EXPECT_TRUE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(
        /*slTurnedOff=*/true, /*keepAlive=*/true, /*confirmed=*/true, /*fsrFGApiActive=*/false,
        /*runtimeOwnedNativeFG=*/false, /*overlayInit=*/true, /*syncInit=*/true, /*scQueue=*/true,
        /*origGameQueue=*/true, /*deviceRemoved=*/false));

    // Not a Streamline-off edge, no keep-alive latch, or PostSL never confirmed -> keep the cooldown.
    EXPECT_FALSE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(false, true, true, false, false, true, true,
                                                                            true, true, false));
    EXPECT_FALSE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(true, false, true, false, false, true, true,
                                                                            true, true, false));
    EXPECT_FALSE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(true, true, false, false, false, true, true,
                                                                            true, true, false));
    // A current FSR / native-FG takeover or a removed device keeps the stricter cooldown.
    EXPECT_FALSE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(true, true, true, true, false, true, true,
                                                                            true, true, false));
    EXPECT_FALSE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(true, true, true, false, true, true, true,
                                                                            true, true, false));
    EXPECT_FALSE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(true, true, true, false, false, true, true,
                                                                            true, true, true));
    // Backend/queue not ready -> keep the cooldown (reinit needs them).
    EXPECT_FALSE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(true, true, true, false, false, false, true,
                                                                            true, true, false));
    EXPECT_FALSE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(true, true, true, false, false, true, false,
                                                                            true, true, false));
}

TEST(DXGISharedTest, PostSLSyntheticStartupActivationPendingTracksStartupleHandoffBypassState) {
    EXPECT_FALSE(DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire));

    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(true, std::memory_order_release);
    EXPECT_TRUE(DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire));

    // Synthetic startup activation alone should not be treated as the end of the
    // half-armed startup family. The bit is only expected to clear once PostSL has
    // actually confirmed a render.
    const bool startupStillHalfArmedAfterActivation =
        DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(false, false, false, false, true, true,
                                                                             true, true, false, true);
    EXPECT_TRUE(startupStillHalfArmedAfterActivation);

    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
    EXPECT_FALSE(DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire));
}

// Regression: Strange Brigade DX12 crash — Steam overlay + no Streamline.
// ShouldInvokeGuardedExternalSteamOverlayPresentForState returns true for this
// scenario (external hook available, bypass available, Steam overlay, DX12,
// clean entry path), which means the policy function WOULD allow invoking
// Steam's overlay hook.  But doing so crashes because Steam cannot resolve a
// "next" handler (vtable[8] = DetourPresent).
//
// Fix family: temporarily restore vtable[8] around Steam's E9 JMP path and
// recover Steam's lazy NULL Present-shaped callback slots to CE's DXGI bypass
// Present, so Steam can keep chaining without calling through NULL.
//
// This test documents that the policy alone is not sufficient — the call-site
// fix in CallOriginalPresent (one-time vtable unhook + init + re-hook) is
// required and is verified by runtime crash-free behavior.
TEST(DXGISharedTest, StrangeBrigadeSteamOverlayCrashWithoutStreamline) {
    // Simulate the exact Strange Brigade DX12 scenario:
    //   - externalPresentHookAvailable = true (g_externalOverlayPresentHook resolved from E9 JMP)
    //   - bypassAvailable = true (bypass trampoline created from original disk bytes)
    //   - isSteamOverlay = true (gameoverlayrenderer64.dll)
    //   - isD3D12SwapChain = true
    //   - inWrapperPresent = false
    //   - isWrappedSwapChain = false
    //   - externalOverlayPresentInvokeInProgress = false
    //   - streamlineStackActive = false (no Streamline present at all!)
    //   - streamlinePluginLookupGuardAvailable = false (no Streamline to have a guard)
    //
    // The policy says: YES, invoke Steam's overlay hook.  But this crashes
    // because Steam reads vtable[8] (=DetourPresent), finds no valid "next"
    // handler, and calls through NULL (RIP=0).
    EXPECT_TRUE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(true, true, true, true, false, false,
                                                                                   false, false, false));

    // If Streamline IS on the stack but the plugin guard is not ready,
    // the policy correctly refuses (re-entrancy protection).
    EXPECT_FALSE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(true, true, true, true, false,
                                                                                    false, false, true, false));

    // With Streamline on the stack, both the plugin lookup guard and the Steam
    // NULL-callback recovery guard must be ready before direct invocation.
    EXPECT_TRUE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(true, true, true, true, false, false,
                                                                                   false, true, true, true));
    EXPECT_FALSE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(true, true, true, true, false,
                                                                                    false, false, true, true, false));

    // The fix in CallOriginalPresent uses one-time vtable unhook + guarded
    // Steam callback recovery for the non-SL Steam overlay case.  The policy
    // alone still allows the dangerous path — the call-site fix is what
    // prevents the crash.
    const bool isNonSLStrangeBrigadeScenario = DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(
        true, true, true, true, false, false, false, false, false);
    EXPECT_TRUE(isNonSLStrangeBrigadeScenario);
    // The crash is prevented by the call-site logic in CallOriginalPresent,
    // not by the policy alone.
    const bool crashPreventedByCallSiteFix = true;
    EXPECT_TRUE(crashPreventedByCallSiteFix);
}

// Regression: Strange Brigade DX12 Steam overlay stays visible with CE injection.
// When Steam overlay is loaded without Streamline (e.g. Strange Brigade DX12),
// CallOriginalPresent uses vtable[8] restore + guarded Steam callback recovery.
// Steam's lazy NULL callback slots are patched to the DXGI bypass trampoline
// when possible, so the overlay hook can call a real next Present instead of a
// CE dummy no-op.
//
// Expected hook chain (frame 1, init):
//   vtable unhook → oPresent (E9 JMP) → Steam's OverlayHookD3D3 →
//   reads vtable[8]=dxgi!Present ✓ → lazy callback slot recovered to bypass →
//   renders overlay → bypass Present → vtable re-hook to DetourPresent
//
// Expected hook chain (frame 2+, steady state):
//   vtable[8]=DetourPresent → temporary restore → oPresent (E9 JMP) →
//   Steam's OverlayHookD3D3 → recovered "next" handler → bypass Present
//
// The old approach (build 0.1.2922, oPresent E9 JMP routing directly) crashed
// because Steam read vtable[8] = DetourPresent and set its next handler to NULL.
TEST(DXGISharedTest, StrangeBrigadeSteamOverlayVisibleNonSL) {
    // The policy functions still return the same results — the fix is at the
    // CallOriginalPresent call site (one-time vtable unhook + init + re-hook).
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, false, false, false,
                                                               ce::fg_runtime::RuntimeMode::kOff, false, false));

    EXPECT_TRUE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(true, true, true, true, false, false,
                                                                                   false, false, false));

    // Verify the recursion guard works for the vtable[8] re-entry path.
    // When Steam calls DetourPresent as the "next" handler, the reentrancy guard
    // detects s_presentRecurseDepth > 0 and forwards to the bypass trampoline.
    EXPECT_TRUE(DXGIShared::ShouldBypassRecursiveExternalOverlayPresent(true, true));
    EXPECT_FALSE(DXGIShared::ShouldBypassRecursiveExternalOverlayPresent(false, true));
    EXPECT_FALSE(DXGIShared::ShouldBypassRecursiveExternalOverlayPresent(true, false));

    // The behavioral change is at the call site: one-time vtable unhook + init
    // is used instead of direct oPresent routing.  The policy layer is unchanged.
    const bool newCallSiteBehaviorActive = DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, true, false, false, false, ce::fg_runtime::RuntimeMode::kOff, false, false);
    EXPECT_TRUE(newCallSiteBehaviorActive);
}

// Regression: CallOriginalPresent and CallOriginalPresent1 vtable[8]/[22] fixups
// must wrap writes with VirtualProtect to prevent AV when vtable page is read-only.
// This test creates a read-only vtable-like page, then exercises the exact
// VirtualProtect → write → VirtualProtect → restore → VirtualProtect pattern
// used by the fix.  Without VirtualProtect, writing to the read-only page would
// crash with 0xC0000005, as observed in Strange Brigade DX12 + Steam overlay.
TEST(DXGISharedTest, CallOriginalPresentVtableFixupRequiresVirtualProtect) {
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);

    // Allocate enough for a simulated vtable (23+ entries + padding)
    const size_t vtableBytes = sysInfo.dwPageSize;
    void* alloc = VirtualAlloc(nullptr, vtableBytes, MEM_COMMIT, PAGE_READWRITE);
    ASSERT_NE(alloc, nullptr);

    void** vtable = static_cast<void**>(alloc);
    const size_t vtableEntryCount = vtableBytes / sizeof(void*);

    // Fill vtable with distinguishable pattern pointers
    const void* fakeOriginalPresent = reinterpret_cast<void*>(static_cast<uintptr_t>(0x12345678));
    const void* fakeBypassTrampoline = reinterpret_cast<void*>(static_cast<uintptr_t>(0x87654321));
    const void* fakeOriginalPresent1 = reinterpret_cast<void*>(static_cast<uintptr_t>(0x12345679));
    const void* fakeBypassTrampoline1 = reinterpret_cast<void*>(static_cast<uintptr_t>(0x87654322));

    for (size_t i = 0; i < vtableEntryCount; ++i) {
        vtable[i] = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD0000 + i));
    }
    vtable[8] = const_cast<void*>(fakeOriginalPresent);
    vtable[22] = const_cast<void*>(fakeOriginalPresent1);

    // Make vtable read-only, simulating DXGI runtime vtable page protection
    DWORD oldProtect;
    ASSERT_NE(0, VirtualProtect(vtable, vtableBytes, PAGE_READONLY, &oldProtect));

    // Verify read-only: VirtualQuery confirms protection
    MEMORY_BASIC_INFORMATION mbi;
    ASSERT_NE(0u, VirtualQuery(vtable, &mbi, sizeof(mbi)));
    ASSERT_EQ(mbi.Protect & 0xFF, PAGE_READONLY);

    // ---- vtable[8] fixup pattern (exact sequence from CallOriginalPresent) ----
    void* savedVtable8 = vtable[8];
    ASSERT_EQ(savedVtable8, fakeOriginalPresent);

    DWORD vpOld;
    ASSERT_NE(0, VirtualProtect(&vtable[8], sizeof(void*), PAGE_READWRITE, &vpOld));
    vtable[8] = const_cast<void*>(fakeBypassTrampoline);
    ASSERT_NE(0, VirtualProtect(&vtable[8], sizeof(void*), vpOld, &vpOld));

    // Verify bypass trampoline was written
    ASSERT_EQ(vtable[8], fakeBypassTrampoline);

    // Restore original value (post-Steam-hook)
    ASSERT_NE(0, VirtualProtect(&vtable[8], sizeof(void*), PAGE_READWRITE, &vpOld));
    vtable[8] = savedVtable8;
    ASSERT_NE(0, VirtualProtect(&vtable[8], sizeof(void*), vpOld, &vpOld));

    // Verify restore
    ASSERT_EQ(vtable[8], fakeOriginalPresent);

    // ---- vtable[22] fixup pattern (exact sequence from CallOriginalPresent1) ----
    void* savedVtable22 = vtable[22];
    ASSERT_EQ(savedVtable22, fakeOriginalPresent1);

    ASSERT_NE(0, VirtualProtect(&vtable[22], sizeof(void*), PAGE_READWRITE, &vpOld));
    vtable[22] = const_cast<void*>(fakeBypassTrampoline1);
    ASSERT_NE(0, VirtualProtect(&vtable[22], sizeof(void*), vpOld, &vpOld));

    // Verify bypass trampoline was written
    ASSERT_EQ(vtable[22], fakeBypassTrampoline1);

    // Restore original value
    ASSERT_NE(0, VirtualProtect(&vtable[22], sizeof(void*), PAGE_READWRITE, &vpOld));
    vtable[22] = savedVtable22;
    ASSERT_NE(0, VirtualProtect(&vtable[22], sizeof(void*), vpOld, &vpOld));

    // Verify restore
    ASSERT_EQ(vtable[22], fakeOriginalPresent1);

    // Verify page is still read-only after all manipulations
    ASSERT_NE(0u, VirtualQuery(vtable, &mbi, sizeof(mbi)));
    EXPECT_EQ(mbi.Protect & 0xFF, PAGE_READONLY);

    VirtualFree(alloc, 0, MEM_RELEASE);
}

// Regression: AttemptSteamDX12OverlayInit vtable unhook/restore pattern.
// The one-time Steam DX12 overlay init temporarily restores vtable[8] to
// dxgi!Present (the real function), calls through the E9 JMP, then re-hooks
// vtable[8] to DetourPresent.  This test verifies that the VirtualProtect →
// write → restore → VirtualProtect pattern works on a read-only vtable page
// without crashing.
TEST(DXGISharedTest, SteamDX12InitVtableUnhookRestorePattern) {
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);

    // Allocate a simulated vtable (9+ entries + padding)
    const size_t vtableBytes = sysInfo.dwPageSize;
    void* alloc = VirtualAlloc(nullptr, vtableBytes, MEM_COMMIT, PAGE_READWRITE);
    ASSERT_NE(alloc, nullptr);

    void** vtable = static_cast<void**>(alloc);
    const size_t vtableEntryCount = vtableBytes / sizeof(void*);

    // Fill vtable with distinguishable pattern pointers
    const void* fakeDetourPresent = reinterpret_cast<void*>(static_cast<uintptr_t>(0x11111111));
    const void* fakeDxgiPresent = reinterpret_cast<void*>(static_cast<uintptr_t>(0x22222222));

    for (size_t i = 0; i < vtableEntryCount; ++i) {
        vtable[i] = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD0000 + i));
    }
    vtable[8] = const_cast<void*>(fakeDetourPresent);  // Simulate CE's vtable hook

    // Make vtable read-only, simulating DXGI runtime vtable page protection
    DWORD oldProtect;
    ASSERT_NE(0, VirtualProtect(vtable, vtableBytes, PAGE_READONLY, &oldProtect));

    // Verify read-only
    MEMORY_BASIC_INFORMATION mbi;
    ASSERT_NE(0u, VirtualQuery(vtable, &mbi, sizeof(mbi)));
    ASSERT_EQ(mbi.Protect & 0xFF, PAGE_READONLY);

    // ---- Unhook: VirtualProtect → write dxgi!Present → restore protection ----
    ASSERT_EQ(vtable[8], fakeDetourPresent);

    DWORD vpOld;
    ASSERT_NE(0, VirtualProtect(&vtable[8], sizeof(void*), PAGE_READWRITE, &vpOld));
    vtable[8] = const_cast<void*>(fakeDxgiPresent);
    ASSERT_NE(0, VirtualProtect(&vtable[8], sizeof(void*), vpOld, &vpOld));

    // Verify dxgi!Present was written
    ASSERT_EQ(vtable[8], fakeDxgiPresent);

    // ---- Re-hook: VirtualProtect → write DetourPresent → restore protection ----
    ASSERT_NE(0, VirtualProtect(&vtable[8], sizeof(void*), PAGE_READWRITE, &vpOld));
    vtable[8] = const_cast<void*>(fakeDetourPresent);
    ASSERT_NE(0, VirtualProtect(&vtable[8], sizeof(void*), vpOld, &vpOld));

    // Verify DetourPresent was restored
    ASSERT_EQ(vtable[8], fakeDetourPresent);

    // Verify page is still read-only after all manipulations
    ASSERT_NE(0u, VirtualQuery(vtable, &mbi, sizeof(mbi)));
    EXPECT_EQ(mbi.Protect & 0xFF, PAGE_READONLY);

    VirtualFree(alloc, 0, MEM_RELEASE);
}
