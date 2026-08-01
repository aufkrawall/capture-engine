#include "test_dxgi_shared_shared.h"

TEST(DXGISharedTest, ExternalHookBypassResumeExtendsPastPatchedFillBytes) {
    EXPECT_FALSE(ce::inline_hook_policy::IsVerifiedExternalHookResumeOffset(5, 5, false));
    EXPECT_TRUE(ce::inline_hook_policy::IsVerifiedExternalHookResumeOffset(14, 5, true));
    EXPECT_FALSE(ce::inline_hook_policy::IsVerifiedExternalHookResumeOffset(4, 5, true));

    EXPECT_TRUE(ce::inline_hook_policy::ShouldExtendExternalHookResumeOffset(5, 14, true));
    EXPECT_FALSE(ce::inline_hook_policy::ShouldExtendExternalHookResumeOffset(5, 5, true));
    EXPECT_FALSE(ce::inline_hook_policy::ShouldExtendExternalHookResumeOffset(5, 14, false));
}

TEST(DXGISharedTest, DredIsOffByDefaultAndOnlyExplicitAffirmativeEnablesIt) {
    // Default OFF: unset env, or empty/null value. DRED auto-breadcrumbs add a per-frame kernel
    // GPU allocation on the app's CommandList::Reset that stalls Present during the Alt+Tab mode
    // switch (logs/20260606_145929), so it must not be on unless explicitly requested.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEnableDredFromEnv(nullptr, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEnableDredFromEnv("", true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEnableDredFromEnv("0", true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEnableDredFromEnv("off", true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEnableDredFromEnv("garbage", true));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldEnableDredFromEnv("1", true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldEnableDredFromEnv("on", true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldEnableDredFromEnv("TRUE", true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldEnableDredFromEnv("Yes", true));

    // Page-fault-only spellings also count as "enabled" (a distinct, lower-perturbation mode).
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldEnableDredFromEnv("pf", true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldEnableDredFromEnv("2", true));
}

TEST(DXGISharedTest, DredArmModeSelectsPageFaultOnlyVsFullVsOff) {
    using ce::dx12_overlay_policy::DecideDredArmMode;
    using Mode = ce::dx12_overlay_policy::DredArmMode;

    // Off: unset / empty / explicit-negative / unrecognized.
    EXPECT_EQ(DecideDredArmMode(nullptr, false), Mode::kOff);
    EXPECT_EQ(DecideDredArmMode("", true), Mode::kOff);
    EXPECT_EQ(DecideDredArmMode("0", true), Mode::kOff);
    EXPECT_EQ(DecideDredArmMode("off", true), Mode::kOff);
    EXPECT_EQ(DecideDredArmMode("garbage", true), Mode::kOff);

    // Page-fault-only: low-perturbation diagnosis mode (no auto-breadcrumbs).
    EXPECT_EQ(DecideDredArmMode("pf", true), Mode::kPageFaultOnly);
    EXPECT_EQ(DecideDredArmMode("PageFault", true), Mode::kPageFaultOnly);
    EXPECT_EQ(DecideDredArmMode("page-fault", true), Mode::kPageFaultOnly);
    EXPECT_EQ(DecideDredArmMode("2", true), Mode::kPageFaultOnly);

    // Full: auto-breadcrumbs + page-fault + context (high perturbation).
    EXPECT_EQ(DecideDredArmMode("1", true), Mode::kFull);
    EXPECT_EQ(DecideDredArmMode("on", true), Mode::kFull);
    EXPECT_EQ(DecideDredArmMode("TRUE", true), Mode::kFull);
    EXPECT_EQ(DecideDredArmMode("full", true), Mode::kFull);
}

TEST(DXGISharedTest, AppCommandListsAreNotForwardedIntoARemovedDevice) {
    // Forwarding the app's ExecuteCommandLists into a torn-down driver after a
    // DEVICE_HUNG TDR is the nvwgf2um access violation in logs/20260608_211517. Once
    // the device is removed, the submission must be dropped; while healthy it forwards.
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldForwardAppCommandListsToDriver(/*deviceRemoved=*/false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldForwardAppCommandListsToDriver(/*deviceRemoved=*/true));
}

TEST(DXGISharedTest, StreamlineUiActivationCoverageSurvivesFrameTagsUntilPostSLConsumesIt) {
    ce::dx12_overlay_policy::StreamlineUiActivationCoverageBudget coverage;

    coverage.Arm(2);
    EXPECT_TRUE(coverage.NeedsCurrentFrameRecord());
    EXPECT_EQ(coverage.Remaining(), 2u);

    // Any number of eValidUntilPresent source-tag rollovers must keep requesting
    // a current-frame record without spending the PostSL output budget.
    EXPECT_TRUE(coverage.NeedsCurrentFrameRecord());
    EXPECT_TRUE(coverage.NeedsCurrentFrameRecord());
    EXPECT_EQ(coverage.Remaining(), 2u);

    EXPECT_TRUE(coverage.ConsumePostSLOutput());
    EXPECT_TRUE(coverage.NeedsCurrentFrameRecord());
    EXPECT_EQ(coverage.Remaining(), 1u);
    EXPECT_TRUE(coverage.ConsumePostSLOutput());
    EXPECT_FALSE(coverage.NeedsCurrentFrameRecord());
    EXPECT_FALSE(coverage.ConsumePostSLOutput());

    coverage.Reset();
    EXPECT_EQ(coverage.Remaining(), 0u);
}

TEST(DXGISharedTest, FirstProvenPostSLOutputCannotTrustOfficialUiAlone) {
    using ce::dx12_overlay_policy::ShouldRequireExactPostSLBackbufferDrawForStartup;

    EXPECT_TRUE(ShouldRequireExactPostSLBackbufferDrawForStartup(false, true, true, false, true));
    EXPECT_TRUE(ShouldRequireExactPostSLBackbufferDrawForStartup(false, false, false, true, true));
    EXPECT_TRUE(ShouldRequireExactPostSLBackbufferDrawForStartup(true, false, false, false, false));

    EXPECT_FALSE(ShouldRequireExactPostSLBackbufferDrawForStartup(false, false, true, false, true));
    EXPECT_FALSE(ShouldRequireExactPostSLBackbufferDrawForStartup(false, true, false, false, true));
    EXPECT_FALSE(ShouldRequireExactPostSLBackbufferDrawForStartup(false, true, true, false, false));
    EXPECT_FALSE(ShouldRequireExactPostSLBackbufferDrawForStartup(false, false, false, true, false));
}

TEST(DXGISharedTest, GameSwapchainCreationAfterExplicitDLSSOffProvesNativeReturn) {
    using ce::dx12_overlay_policy::ShouldTreatGameSwapchainCreateAfterExplicitDLSSOffAsNormalReturn;

    EXPECT_TRUE(ShouldTreatGameSwapchainCreateAfterExplicitDLSSOffAsNormalReturn(
        /*gameCreatedSwapchain=*/true, /*postSLExplicitOffKeepAlive=*/true,
        /*actualFrameGenerationActive=*/false, /*streamlineFGRunning=*/false));

    // Explicit OFF without a game-created replacement is only suspension: the
    // exact Streamline proxy must keep its proven PostSL rendering route.
    EXPECT_FALSE(ShouldTreatGameSwapchainCreateAfterExplicitDLSSOffAsNormalReturn(
        /*gameCreatedSwapchain=*/false, /*postSLExplicitOffKeepAlive=*/true,
        /*actualFrameGenerationActive=*/false, /*streamlineFGRunning=*/false));
    EXPECT_FALSE(ShouldTreatGameSwapchainCreateAfterExplicitDLSSOffAsNormalReturn(
        /*gameCreatedSwapchain=*/true, /*postSLExplicitOffKeepAlive=*/true,
        /*actualFrameGenerationActive=*/true, /*streamlineFGRunning=*/false));
    EXPECT_FALSE(ShouldTreatGameSwapchainCreateAfterExplicitDLSSOffAsNormalReturn(
        /*gameCreatedSwapchain=*/true, /*postSLExplicitOffKeepAlive=*/true,
        /*actualFrameGenerationActive=*/false, /*streamlineFGRunning=*/true));
    EXPECT_FALSE(ShouldTreatGameSwapchainCreateAfterExplicitDLSSOffAsNormalReturn(
        /*gameCreatedSwapchain=*/true, /*postSLExplicitOffKeepAlive=*/false,
        /*actualFrameGenerationActive=*/false, /*streamlineFGRunning=*/false));
}

TEST(DXGISharedTest, AuthoritativeDLSSOffNativeReturnBypassesOnlyResidualRecentFGCooldown) {
    using ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyAfterAuthoritativeDLSSOffNormalReturn;

    EXPECT_TRUE(ShouldReinitOverlayImmediatelyAfterAuthoritativeDLSSOffNormalReturn(true, true, false, false, false,
                                                                                    false, false, false));

    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterAuthoritativeDLSSOffNormalReturn(false, true, false, false, false,
                                                                                     false, false, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterAuthoritativeDLSSOffNormalReturn(true, false, false, false, false,
                                                                                     false, false, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterAuthoritativeDLSSOffNormalReturn(true, true, true, false, false,
                                                                                     false, false, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterAuthoritativeDLSSOffNormalReturn(true, true, false, true, false,
                                                                                     false, false, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterAuthoritativeDLSSOffNormalReturn(true, true, false, false, true,
                                                                                     false, false, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterAuthoritativeDLSSOffNormalReturn(true, true, false, false, false,
                                                                                     true, false, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterAuthoritativeDLSSOffNormalReturn(true, true, false, false, false,
                                                                                     false, true, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterAuthoritativeDLSSOffNormalReturn(true, true, false, false, false,
                                                                                     false, false, true));
}

TEST(DXGISharedTest, LateOuterStreamlineOffCannotTearDownNewlyRebuiltNativeReturn) {
    using ce::dx12_overlay_policy::ShouldKeepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn;

    EXPECT_TRUE(ShouldKeepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn(true, true, true, true, true, false));

    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn(false, true, true, true, true, false));
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn(true, false, true, true, true, false));
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn(true, true, false, true, true, false));
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn(true, true, true, false, true, false));
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn(true, true, true, true, false, false));
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn(true, true, true, true, true, true));
}

TEST(DXGISharedTest, TransitionCooldownDoesNotHeavySuspendDrawableDX12Overlay) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldHeavySuspendDX12OverlayForSwapchainState(false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldHeavySuspendDX12OverlayForSwapchainState(true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldHeavySuspendDX12OverlayForSwapchainState(false, true));
}

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
    // On Windows 10+ the D3D10 runtime is implemented on D3D11 (D3D10-on-D3D11).
    // A D3D10 device QI's for both ID3D11Device (translation layer) and
    // ID3D10Device (native).  When both succeed we prefer D3D10 so the DX10
    // overlay path is used — the D3D11 overlay path produces no visible output
    // on D3D10-on-D3D11 devices.
    EXPECT_EQ(DXGIShared::APIType::D3D10, DXGIShared::SelectPrimarySwapChainAPIType(false, true, true));
    EXPECT_EQ(DXGIShared::APIType::D3D11, DXGIShared::SelectPrimarySwapChainAPIType(false, true, false));
    EXPECT_EQ(DXGIShared::APIType::D3D10, DXGIShared::SelectPrimarySwapChainAPIType(false, false, true));
    EXPECT_EQ(DXGIShared::APIType::Unknown, DXGIShared::SelectPrimarySwapChainAPIType(false, false, false));
}

// Regression: D3D10-on-D3D11 detection.  Before the fix the D3D10 QI was
// short-circuited when D3D11 succeeded, so a D3D10 device on Windows 10+
// (where D3D10 is implemented on D3D11) was wrongly classified as D3D11.
// The DX11 overlay path then ran on a D3D10-on-D3D11 device and produced
// no visible output (overlay_us=0 in perf_metrics; overlay log messages
// visible but nothing on screen).  The fix always tries all three QI's
// and SelectPrimarySwapChainAPIType prefers D3D10 when both succeed.
TEST(DXGISharedTest, D3D10OnD3D11SwapchainReturnsD3D10) {
    // Simulate a D3D10-on-D3D11 device: QI succeeds for both D3D11 and D3D10
    EXPECT_EQ(DXGIShared::APIType::D3D10, DXGIShared::SelectPrimarySwapChainAPIType(false, true, true));
    // Native D3D11 has D3D11 only
    EXPECT_EQ(DXGIShared::APIType::D3D11, DXGIShared::SelectPrimarySwapChainAPIType(false, true, false));
    // Native D3D10 has D3D10 only
    EXPECT_EQ(DXGIShared::APIType::D3D10, DXGIShared::SelectPrimarySwapChainAPIType(false, false, true));
}

TEST(DXGISharedTest, D3D10UsesSharedD3D10D3D11ProcessFramePath) {
    EXPECT_TRUE(DXGIShared::ShouldRunSharedD3D10Or11ProcessFrame(DXGIShared::APIType::D3D10));
    EXPECT_TRUE(DXGIShared::ShouldRunSharedD3D10Or11ProcessFrame(DXGIShared::APIType::D3D11));
    EXPECT_FALSE(DXGIShared::ShouldRunSharedD3D10Or11ProcessFrame(DXGIShared::APIType::D3D12));
    EXPECT_FALSE(DXGIShared::ShouldRunSharedD3D10Or11ProcessFrame(DXGIShared::APIType::Unknown));
}

TEST(DXGISharedTest, D3D12FocusLossPreservesPresentPacing) {
    EXPECT_FALSE(DXGIShared::ShouldApplyUnfocusedFlipModelDoNotWait(true, false, false, 0));
    EXPECT_TRUE(DXGIShared::ShouldApplyUnfocusedFlipModelDoNotWait(false, false, false, 0));
    EXPECT_FALSE(DXGIShared::ShouldApplyUnfocusedFlipModelDoNotWait(false, false, true, 0));
    EXPECT_FALSE(DXGIShared::ShouldApplyUnfocusedFlipModelDoNotWait(false, true, false, 0));
    EXPECT_FALSE(DXGIShared::ShouldApplyUnfocusedFlipModelDoNotWait(false, false, false, 0x00000200U));
    EXPECT_FALSE(DXGIShared::ShouldApplyUnfocusedFlipModelDoNotWait(false, false, false, 0x00000004U));
}

TEST(DXGISharedTest, D3D12FocusLossFrameLatencyWaitableProbeIsDisabledForPresentPassthrough) {
    EXPECT_FALSE(
        DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(true, false, false, false, false, true, false, false, true));

    EXPECT_FALSE(
        DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(true, false, true, false, false, true, false, false, true));
    EXPECT_FALSE(
        DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(true, true, false, false, false, true, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(false, false, false, false, false, true, false,
                                                                    false, true));
    EXPECT_FALSE(DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(true, false, false, false, false, true, false,
                                                                    false, false));
    EXPECT_FALSE(DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(true, false, false, false, false, false, false,
                                                                    false, true));
    EXPECT_FALSE(
        DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(true, false, false, false, false, true, true, false, true));
    EXPECT_FALSE(
        DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(true, false, false, false, false, true, false, true, true));
    EXPECT_FALSE(
        DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(true, false, false, true, false, true, false, false, true));
    EXPECT_FALSE(
        DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(true, false, false, false, true, true, false, false, true));
}

TEST(DXGISharedTest, D3D12FocusLossFrameLatencyDisabledPolicyIsPresentPathAgnostic) {
    const bool presentPath =
        DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(true, false, false, false, false, true, false, false, true);
    const bool present1Path =
        DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(true, false, false, false, false, true, false, false, true);

    EXPECT_FALSE(presentPath);
    EXPECT_EQ(presentPath, present1Path);
}

TEST(DXGISharedTest, D3D12FocusLossPostPresentOverlayFenceWaitsForDeferredSingleQueueWork) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, false, false, false, false, true, false, false, false, false, true, true, true, true, 42));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, false, true, false, false, true, false, false, false, false, true, true, true, true, 42));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, true, false, false, false, true, false, false, false, false, true, true, true, true, 42));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        false, false, false, false, false, true, false, false, false, false, true, true, true, true, 42));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, false, false, true, false, true, false, false, false, false, true, true, true, true, 42));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, false, false, false, true, true, false, false, false, false, true, true, true, true, 42));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, false, false, false, false, false, false, false, false, false, true, true, true, true, 42));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, false, false, false, false, true, true, false, false, false, true, true, true, true, 42));
}

TEST(DXGISharedTest, D3D12FocusLossPostPresentOverlayFenceSkipsRuntimeAndSyncInvalidPaths) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, false, false, false, false, true, false, true, false, false, true, true, true, true, 42));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, false, false, false, false, true, false, false, true, false, true, true, true, true, 42));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, false, false, false, false, true, false, false, false, true, true, true, true, true, 42));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, false, false, false, false, true, false, false, false, false, false, true, true, true, 42));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, false, false, false, false, true, false, false, false, false, true, false, true, true, 42));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, false, false, false, false, true, false, false, false, false, true, true, false, true, 42));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, false, false, false, false, true, false, false, false, false, true, true, true, false, 42));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, false, false, false, false, true, false, false, false, false, true, true, true, true, 0));
}

TEST(DXGISharedTest, D3D12FocusLossPostPresentOverlayFencePolicyIsPresentPathAgnostic) {
    const bool presentPath = ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, false, false, false, false, true, false, false, false, false, true, true, true, true, 42);
    const bool present1Path = ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        true, false, false, false, false, true, false, false, false, false, true, true, true, true, 42);

    EXPECT_TRUE(presentPath);
    EXPECT_EQ(presentPath, present1Path);
}

TEST(DXGISharedTest, D3D12FocusLossImmediateOverlayFenceSyncsSingleQueueWrappedSubmit) {
    auto shouldSignal = [](bool wrappedD3D12 = true, bool fullscreen = false, bool foreground = false,
                           bool iconic = false, bool zeroSized = false, bool submitted = true, bool deviceLost = false,
                           bool fgActive = false, bool runtimeOwned = false, bool dedicated = false,
                           bool steamDeferred = false, bool hasFence = true, bool hasEvent = true, bool hasQueue = true,
                           UINT64 fenceValue = 42) {
        return ce::dx12_overlay_policy::ShouldSignalD3D12FocusLossOverlayFenceImmediately(
            wrappedD3D12, fullscreen, foreground, iconic, zeroSized, submitted, deviceLost, fgActive, runtimeOwned,
            dedicated, steamDeferred, hasFence, hasEvent, hasQueue, fenceValue);
    };

    EXPECT_TRUE(shouldSignal());

    EXPECT_FALSE(shouldSignal(false));
    EXPECT_FALSE(shouldSignal(true, true));
    EXPECT_FALSE(shouldSignal(true, false, true));
    EXPECT_FALSE(shouldSignal(true, false, false, true));
    EXPECT_FALSE(shouldSignal(true, false, false, false, true));
    EXPECT_FALSE(shouldSignal(true, false, false, false, false, false));
    EXPECT_FALSE(shouldSignal(true, false, false, false, false, true, true));
    EXPECT_FALSE(shouldSignal(true, false, false, false, false, true, false, true));
    EXPECT_FALSE(shouldSignal(true, false, false, false, false, true, false, false, true));
    EXPECT_FALSE(shouldSignal(true, false, false, false, false, true, false, false, false, true));
    EXPECT_FALSE(shouldSignal(true, false, false, false, false, true, false, false, false, false, true));
    EXPECT_FALSE(shouldSignal(true, false, false, false, false, true, false, false, false, false, false, false));
    EXPECT_FALSE(shouldSignal(true, false, false, false, false, true, false, false, false, false, false, true, false));
    EXPECT_FALSE(
        shouldSignal(true, false, false, false, false, true, false, false, false, false, false, true, true, false));
    EXPECT_FALSE(
        shouldSignal(true, false, false, false, false, true, false, false, false, false, false, true, true, true, 0));
}

TEST(DXGISharedTest, D3D12FocusLossImmediateOverlayFencePolicyIsPresentPathAgnostic) {
    const bool presentPath = ce::dx12_overlay_policy::ShouldSignalD3D12FocusLossOverlayFenceImmediately(
        true, false, false, false, false, true, false, false, false, false, false, true, true, true, 42);
    const bool present1Path = ce::dx12_overlay_policy::ShouldSignalD3D12FocusLossOverlayFenceImmediately(
        true, false, false, false, false, true, false, false, false, false, false, true, true, true, 42);

    EXPECT_TRUE(presentPath);
    EXPECT_EQ(presentPath, present1Path);
}

TEST(DXGISharedTest, D3D12FocusLossImmediateOverlayFenceSignalRequiresSameFrameWait) {
    auto shouldWait = [](bool policyAccepted = true, bool signalSucceeded = true, bool hasFence = true,
                         bool hasEvent = true, bool hasQueue = true, UINT64 fenceValue = 42) {
        return ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossImmediateOverlayFence(
            policyAccepted, signalSucceeded, hasFence, hasEvent, hasQueue, fenceValue);
    };

    EXPECT_TRUE(shouldWait());

    EXPECT_FALSE(shouldWait(false));
    EXPECT_FALSE(shouldWait(true, false));
    EXPECT_FALSE(shouldWait(true, true, false));
    EXPECT_FALSE(shouldWait(true, true, true, false));
    EXPECT_FALSE(shouldWait(true, true, true, true, false));
    EXPECT_FALSE(shouldWait(true, true, true, true, true, 0));
}

TEST(DXGISharedTest, D3D12FocusLossImmediateOverlayFenceWaitPolicyIsPresentPathAgnostic) {
    const bool presentPath =
        ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossImmediateOverlayFence(true, true, true, true, true, 42);
    const bool present1Path =
        ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossImmediateOverlayFence(true, true, true, true, true, 42);

    EXPECT_TRUE(presentPath);
    EXPECT_EQ(presentPath, present1Path);
}

TEST(DXGISharedTest, D3D12FocusLossImmediateFenceDumpRequestsOnlyForFirstIncompleteWait) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusLossImmediateFenceWait(false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusLossImmediateFenceWait(true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusLossImmediateFenceWait(false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusLossImmediateFenceWait(true, true));
}

TEST(DXGISharedTest, D3D12NonPresentableSwapchainHoldsBackbufferWork) {
    auto shouldHold = [](bool wrappedD3D12 = true, bool fullscreen = false, bool occluded = true, bool iconic = false,
                         bool zeroSized = false, bool fgActive = false, bool runtimeOwned = false,
                         bool dedicated = false, bool steamDeferred = false, bool deviceLost = false,
                         bool hasQueue = true) {
        return ce::dx12_overlay_policy::ShouldHoldD3D12OverlayBackbufferWorkForNonPresentableSwapchain(
            wrappedD3D12, fullscreen, occluded, iconic, zeroSized, fgActive, runtimeOwned, dedicated, steamDeferred,
            deviceLost, hasQueue);
    };

    // Occluded, iconic, or zero-sized each make the swapchain non-presentable -> hold.
    EXPECT_TRUE(shouldHold());                                 // occluded
    EXPECT_TRUE(shouldHold(true, false, false, true, false));  // iconic
    EXPECT_TRUE(shouldHold(true, false, false, false, true));  // zero-sized

    // Negating conditions never hold.
    EXPECT_FALSE(shouldHold(false));                                                              // not wrapped D3D12
    EXPECT_FALSE(shouldHold(true, true));                                                         // fullscreen
    EXPECT_FALSE(shouldHold(true, false, false, false, false));                                   // presentable/visible
    EXPECT_FALSE(shouldHold(true, false, true, false, false, true));                              // FG active
    EXPECT_FALSE(shouldHold(true, false, true, false, false, false, true));                       // runtime-owned
    EXPECT_FALSE(shouldHold(true, false, true, false, false, false, false, true));                // dedicated queue
    EXPECT_FALSE(shouldHold(true, false, true, false, false, false, false, false, true));         // steam deferred
    EXPECT_FALSE(shouldHold(true, false, true, false, false, false, false, false, false, true));  // device lost
    EXPECT_FALSE(shouldHold(true, false, true, false, false, false, false, false, false, false, false));  // no queue
}

// Regression for the v7 focus-based hide: an unfocused-but-VISIBLE window (Present
// returns S_OK, not occluded; not minimized; non-zero size) must NOT hold
// backbuffer work, so the overlay keeps rendering exactly like a proper inject overlay. Under the old
// focus-gated policy this returned true (hold == overlay hidden).
TEST(DXGISharedTest, D3D12UnfocusedButVisibleSwapchainKeepsOverlayVisible) {
    const bool hold = ce::dx12_overlay_policy::ShouldHoldD3D12OverlayBackbufferWorkForNonPresentableSwapchain(
        /*wrappedD3D12=*/true, /*fullscreen=*/false, /*occluded=*/false, /*iconic=*/false,
        /*zeroSized=*/false, /*fgActive=*/false, /*runtimeOwned=*/false, /*dedicated=*/false,
        /*steamDeferred=*/false, /*deviceLost=*/false, /*hasQueue=*/true);
    EXPECT_FALSE(hold);
}

TEST(DXGISharedTest, D3D12FocusTransitionTelemetryTracksOnlyTheModeSwitchWindow) {
    auto active = [](bool windowed = true, int transitionRemaining = 30, bool fgActive = false,
                     bool runtimeOwned = false, bool dedicated = false, bool steamDeferred = false,
                     bool deviceLost = false, bool hasQueue = true) {
        return ce::dx12_overlay_policy::IsD3D12FocusTransitionTelemetryActive(
            windowed, transitionRemaining, fgActive, runtimeOwned, dedicated, steamDeferred, deviceLost, hasQueue);
    };

    // The legacy edge counter remains useful for bounded logging and DRED capture,
    // but this result is telemetry only and must not gate overlay rendering.
    EXPECT_TRUE(active());
    EXPECT_FALSE(active(true, 0));

    // Negating conditions belong to routes with separate diagnostics.
    EXPECT_FALSE(active(false, 30));                                           // exclusive fullscreen
    EXPECT_FALSE(active(true, 30, true));                                      // FG active
    EXPECT_FALSE(active(true, 30, false, true));                               // runtime-owned
    EXPECT_FALSE(active(true, 30, false, false, true));                        // dedicated queue
    EXPECT_FALSE(active(true, 30, false, false, false, true));                 // steam deferred
    EXPECT_FALSE(active(true, 30, false, false, false, false, true));          // device lost
    EXPECT_FALSE(active(true, 30, false, false, false, false, false, false));  // no queue
}

TEST(DXGISharedTest, D3D12NonPresentableHoldPolicyIsPresentPathAgnostic) {
    const bool presentPath = ce::dx12_overlay_policy::ShouldHoldD3D12OverlayBackbufferWorkForNonPresentableSwapchain(
        true, false, true, false, false, false, false, false, false, false, true);
    const bool present1Path = ce::dx12_overlay_policy::ShouldHoldD3D12OverlayBackbufferWorkForNonPresentableSwapchain(
        true, false, true, false, false, false, false, false, false, false, true);

    EXPECT_TRUE(presentPath);
    EXPECT_EQ(presentPath, present1Path);
}

TEST(DXGISharedTest, D3D12FocusTransitionDeviceRemovalDumpRequestsOnlyOnceForRecentTransition) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusTransitionDeviceRemoval(true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusTransitionDeviceRemoval(false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusTransitionDeviceRemoval(true, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusTransitionDeviceRemoval(true, true, true));
}

TEST(DXGISharedTest, IncompleteFocusLossFenceOnlyHoldsBackbufferWork) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldHoldD3D12FocusLossOverlayDrawForPendingFence(false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldHoldD3D12FocusLossBackbufferWorkForPendingFence(false, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldHoldD3D12FocusLossOverlayDrawForPendingFence(true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldHoldD3D12FocusLossOverlayDrawForPendingFence(false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldHoldD3D12FocusLossOverlayDrawForPendingFence(false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldHoldD3D12FocusLossBackbufferWorkForPendingFence(true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldHoldD3D12FocusLossBackbufferWorkForPendingFence(false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldHoldD3D12FocusLossBackbufferWorkForPendingFence(false, true, true));
}

TEST(DXGISharedTest, OverlayUploadSlotGuardDisabledForFGAndMissingFence) {
    namespace pol = ce::dx12_overlay_policy;
    // Non-FG with a live overlay fence: the slot is guarded by the value this
    // frame's overlay work will signal (currentFenceValue + 1).
    EXPECT_EQ(pol::DecideOverlayUploadSlotGuardValue(false, true, 0u), 1u);
    EXPECT_EQ(pol::DecideOverlayUploadSlotGuardValue(false, true, 41u), 42u);
    // FG active uses a separate completion fence, so the overlay-fence-keyed guard
    // would never be reached -> disabled (0).
    EXPECT_EQ(pol::DecideOverlayUploadSlotGuardValue(true, true, 41u), 0u);
    // No overlay fence -> nothing to wait on.
    EXPECT_EQ(pol::DecideOverlayUploadSlotGuardValue(false, false, 41u), 0u);
}

TEST(DXGISharedTest, OverlayUploadSlotWaitsOnlyWhenGpuBehindActiveGuard) {
    namespace pol = ce::dx12_overlay_policy;
    // Guard 0 (disabled) -> never wait, regardless of completed value.
    EXPECT_FALSE(pol::ShouldWaitForOverlayUploadSlot(0u, 0u));
    EXPECT_FALSE(pol::ShouldWaitForOverlayUploadSlot(0u, 100u));
    // GPU has reached or passed the guard -> no wait.
    EXPECT_FALSE(pol::ShouldWaitForOverlayUploadSlot(10u, 10u));
    EXPECT_FALSE(pol::ShouldWaitForOverlayUploadSlot(10u, 11u));
    // GPU still behind an active guard -> must wait (prevents the upload-ring stomp).
    EXPECT_TRUE(pol::ShouldWaitForOverlayUploadSlot(10u, 9u));
    EXPECT_TRUE(pol::ShouldWaitForOverlayUploadSlot(1u, 0u));
}

TEST(DXGISharedTest, DescFreeFontUploadRecordsOnlyWhenPendingAndResourcesExist) {
    namespace pol = ce::dx12_overlay_policy;
    EXPECT_TRUE(pol::ShouldRecordDescFreeFontUpload(true, true, true));
    EXPECT_FALSE(pol::ShouldRecordDescFreeFontUpload(false, true, true));
    EXPECT_FALSE(pol::ShouldRecordDescFreeFontUpload(true, false, true));
    EXPECT_FALSE(pol::ShouldRecordDescFreeFontUpload(true, true, false));
}

TEST(DXGISharedTest, X86Dx12OverlayUsesStandardNativeBackendRoute) {
    namespace pol = ce::dx12_overlay_policy;
    EXPECT_TRUE(pol::ShouldUseTextureDx12OverlayBackendForProcess(true));
    EXPECT_FALSE(pol::ShouldUseTextureDx12OverlayBackendForProcess(false));
}

TEST(DXGISharedTest, X86Dx12TextUsesSolidGeometry) {
    namespace pol = ce::dx12_overlay_policy;
    EXPECT_TRUE(pol::ShouldUseSolidDx12TextGeometryForProcess(true));
    EXPECT_FALSE(pol::ShouldUseSolidDx12TextGeometryForProcess(false));
}

// Models the independent upload-ring reuse hazard that an extended GPU pause can
// expose. The guard remains required, but later DRED work disproved ring reuse as
// the cause of the deterministic x86 font-resource hang: that failure reproduced
// with correct fencing. The unguarded model must stomp; the guarded model must not.
TEST(DXGISharedTest, OverlayUploadRingGuardPreventsStompDuringGpuPause) {
    namespace pol = ce::dx12_overlay_policy;
    constexpr int kPoolSize = 4;

    auto simulateStomps = [](bool useGuard) {
        uint64_t slotGuard[kPoolSize] = {0, 0, 0, 0};
        uint64_t slotSubmit[kPoolSize] = {0, 0, 0, 0};  // fence value of work that last used the slot
        uint64_t currentFenceValue = 0;
        uint64_t gpuCompleted = 0;
        int stomps = 0;

        for (int frame = 0; frame < 40; ++frame) {
            // The GPU stops retiring mid-run (the mode-switch pause).
            const bool gpuPaused = (frame >= 6 && frame < 22);
            const int slot = frame % kPoolSize;

            // Backend honors the per-slot guard: block until the GPU reaches it.
            if (useGuard && pol::ShouldWaitForOverlayUploadSlot(slotGuard[slot], gpuCompleted)) {
                gpuCompleted = slotGuard[slot];
            }

            // Overwrite the slot now.  A stomp = the GPU had not finished the work
            // that last used this slot.
            if (slotSubmit[slot] != 0 && gpuCompleted < slotSubmit[slot]) {
                stomps++;
            }

            const uint64_t guard =
                useGuard ? pol::DecideOverlayUploadSlotGuardValue(false, true, currentFenceValue) : 0u;
            currentFenceValue += 1;  // one overlay submit per frame
            slotSubmit[slot] = currentFenceValue;
            slotGuard[slot] = guard;

            if (!gpuPaused && gpuCompleted < currentFenceValue) {
                gpuCompleted++;
            }
        }
        return stomps;
    };

    EXPECT_GT(simulateStomps(false), 0);  // pre-fix unguarded ring stomps during the pause
    EXPECT_EQ(simulateStomps(true), 0);   // per-slot guard prevents every stomp
}

TEST(DXGISharedTest, DXGIFactoryEnumerationLoggingTreatsNotFoundAsBenign) {
    EXPECT_FALSE(ce::dxgi_factory_policy::ShouldLogAdapterEnumerationFailure(S_OK));
    EXPECT_FALSE(ce::dxgi_factory_policy::ShouldLogAdapterEnumerationFailure(DXGI_ERROR_NOT_FOUND));
    EXPECT_TRUE(ce::dxgi_factory_policy::ShouldLogAdapterEnumerationFailure(E_FAIL));
}

TEST(DXGISharedTest, RecreatedSwapchainsStayHookableWhenExternalOverlayPathIsAlreadyActive) {
    EXPECT_TRUE(DXGIShared::ShouldInstallSwapchainHooksWithThirdPartyOverlay(false, false));
    EXPECT_TRUE(DXGIShared::ShouldInstallSwapchainHooksWithThirdPartyOverlay(true, true));
    // Vtable hooks bypass third-party overlay inline hooks, so always install.
    EXPECT_TRUE(DXGIShared::ShouldInstallSwapchainHooksWithThirdPartyOverlay(true, false));
}

TEST(DXGISharedTest, SteamDX12BypassStaysEnabledUntilStreamlineFGActuallyRuns) {
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, false, false, true,
                                                               ce::fg_runtime::RuntimeMode::kOff, false, false));
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, true, false, false, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false));
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(
        true, true, true, false, false, true, ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion, false, false));
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, false, false, true,
                                                               ce::fg_runtime::RuntimeMode::kDLSSFG, false, false));
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, false, false, true,
                                                                ce::fg_runtime::RuntimeMode::kOff, true, false));
}

TEST(DXGISharedTest, GuardedSteamForcedBypassWaitsForActualStreamlineFG) {
    EXPECT_FALSE(DXGIShared::ShouldInvokeGuardedSteamPresentDuringForcedBypass(true, false));
    EXPECT_TRUE(DXGIShared::ShouldInvokeGuardedSteamPresentDuringForcedBypass(true, true));
    EXPECT_TRUE(DXGIShared::ShouldInvokeGuardedSteamPresentDuringForcedBypass(false, false));
    EXPECT_TRUE(DXGIShared::ShouldInvokeGuardedSteamPresentDuringForcedBypass(true, false, true));
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
    // Regression: Strange Brigade DX12 crash.  When Steam overlay hooks dxgi!Present
    // with an E9 JMP and no Streamline or NvPresent is loaded, calling oPresent
    // (dxgi!Present with Steam's E9) re-enters Steam's overlay handler which
    // crashes because vtable[8] = DetourPresent.  The bypass trampoline must be
    // used instead.
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, false, false, false,
                                                               ce::fg_runtime::RuntimeMode::kOff, false, false));
}

// Regression: Strange Brigade DX12 crash.  When Steam overlay hooks dxgi!Present
// with an E9 JMP and CE uses vtable hooks (oPresentTrampoline==NULL), the startup
// compat pass must use the bypass trampoline instead of routing through Steam's
// broken hook chain.  Steam reads vtable[8] (=DetourPresent), can't resolve a
// "next" handler, and calls through NULL (RIP=0).
TEST(DXGISharedTest, StartupCompatPassRequiresBypassForSteamOverlayVtableHookPath) {
    // Steam overlay + vtable hook (no trampoline) + bypass available: allow pass
    EXPECT_TRUE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, false, true,
                                                                      ce::fg_runtime::RuntimeMode::kOff, false));

    // Without bypass trampoline: no safe forwarding path, block the pass
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, false, false,
                                                                       ce::fg_runtime::RuntimeMode::kOff, false));

    // With inline trampoline present: normal path, allow pass
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, true, false, false, true,
                                                                       ce::fg_runtime::RuntimeMode::kOff, false));

    // No third-party overlay: no startup compat pass needed
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(false, false, false, false, true,
                                                                       ce::fg_runtime::RuntimeMode::kOff, false));

    // With active frame generation: block the pass (let FG routing handle it)
    EXPECT_FALSE(DXGIShared::ShouldAllowDX12StartupPresentPassForState(true, false, false, false, true,
                                                                       ce::fg_runtime::RuntimeMode::kDLSSFG, false));
}

// Regression: Strange Brigade DX12 non-SL Steam overlay bypass.  When Steam
// overlay is loaded without Streamline or NvPresent, ShouldForceSteamDX12Bypass
// must return true so CallOriginalPresent routes through the bypass trampoline
// instead of calling oPresent (dxgi!Present with Steam's E9 JMP) which would
// crash because Steam can't resolve vtable[8] (=DetourPresent) as a next handler.
TEST(DXGISharedTest, SteamDX12BypassForNonSLSteamOverlay) {
    // Steam overlay + bypass available + no SL + no NV: must force bypass
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, false, false, false,
                                                               ce::fg_runtime::RuntimeMode::kOff, false, false));

    // Still requires bypassAvailable
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(false, true, true, false, false, false,
                                                                ce::fg_runtime::RuntimeMode::kOff, false, false));

    // Still requires isSteamOverlay
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(true, false, true, false, false, false,
                                                                ce::fg_runtime::RuntimeMode::kOff, false, false));

    // Still requires isD3D12SwapChain
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, false, false, false, false,
                                                                ce::fg_runtime::RuntimeMode::kOff, false, false));

    // Still blocked by inWrapperPresent
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, true, false, false,
                                                                ce::fg_runtime::RuntimeMode::kOff, false, false));

    // Still blocked by isWrappedSwapChain
    EXPECT_FALSE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, false, true, false,
                                                                ce::fg_runtime::RuntimeMode::kOff, false, false));

    // SL loaded without FG still forces bypass (existing behavior preserved)
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, false, false, true,
                                                               ce::fg_runtime::RuntimeMode::kOff, false, false));

    // NvPresent loaded still forces bypass (existing behavior preserved)
    EXPECT_TRUE(DXGIShared::ShouldForceSteamDX12BypassForState(true, true, true, false, false, false,
                                                               ce::fg_runtime::RuntimeMode::kOff, false, true));
}

TEST(DXGISharedTest, GuardedSteamOverlayInvokeRequiresBypassAndCleanDX12Path) {
    EXPECT_TRUE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(true, true, true, true, false, false,
                                                                                   false, false, false));

    EXPECT_FALSE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(false, true, true, true, false,
                                                                                    false, false, false, false));
    EXPECT_FALSE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(true, false, true, true, false,
                                                                                    false, false, false, false));
    EXPECT_FALSE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(true, true, false, true, false,
                                                                                    false, false, false, false));
    EXPECT_FALSE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(true, true, true, false, false,
                                                                                    false, false, false, false));
    EXPECT_FALSE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(true, true, true, true, true, false,
                                                                                    false, false, false));
    EXPECT_FALSE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(true, true, true, true, false, true,
                                                                                    false, false, false));
    EXPECT_FALSE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(true, true, true, true, false,
                                                                                    false, true, false, false));
}

TEST(DXGISharedTest, GuardedSteamOverlayInvokeOnStreamlineStackRequiresPluginLookupGuard) {
    EXPECT_FALSE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(true, true, true, true, false,
                                                                                    false, false, true, false, true));
    EXPECT_FALSE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(true, true, true, true, false,
                                                                                    false, false, true, true, false));
    EXPECT_TRUE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(true, true, true, true, false, false,
                                                                                   false, true, true, true));
}

TEST(DXGISharedTest, GuardedSteamOverlayInvokeWithoutStreamlineStackDoesNotRequireSteamNullGuard) {
    EXPECT_TRUE(DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(true, true, true, true, false, false,
                                                                                   false, false, false, false));
}
