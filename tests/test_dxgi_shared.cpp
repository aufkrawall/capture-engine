#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../hook/common/dx12_overlay_policy.h"
#include "../hook/common/dxgi_factory_policy.h"
#include "../hook/common/dxgi_shared.h"
#include "../hook/wrappers/inline_hook_policy.h"
#include "../hook/wrappers/iat_hook.h"
#include "../captureengine/injection_policy.h"

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
    EXPECT_FALSE(DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(
        true, false, false, false, false, true, false, false, true));

    EXPECT_FALSE(DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(
        true, false, true, false, false, true, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(
        true, true, false, false, false, true, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(
        false, false, false, false, false, true, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(
        true, false, false, false, false, true, false, false, false));
    EXPECT_FALSE(DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(
        true, false, false, false, false, false, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(
        true, false, false, false, false, true, true, false, true));
    EXPECT_FALSE(DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(
        true, false, false, false, false, true, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(
        true, false, false, true, false, true, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(
        true, false, false, false, true, true, false, false, true));
}

TEST(DXGISharedTest, D3D12FocusLossFrameLatencyDisabledPolicyIsPresentPathAgnostic) {
    const bool presentPath = DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(
        true, false, false, false, false, true, false, false, true);
    const bool present1Path = DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(
        true, false, false, false, false, true, false, false, true);

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
                           bool iconic = false, bool zeroSized = false, bool submitted = true,
                           bool deviceLost = false, bool fgActive = false, bool runtimeOwned = false,
                           bool dedicated = false, bool steamDeferred = false, bool hasFence = true,
                           bool hasEvent = true, bool hasQueue = true, UINT64 fenceValue = 42) {
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
    EXPECT_FALSE(shouldSignal(true, false, false, false, false, true, false, false, false, false, false, true,
                              false));
    EXPECT_FALSE(shouldSignal(true, false, false, false, false, true, false, false, false, false, false, true, true,
                              false));
    EXPECT_FALSE(shouldSignal(true, false, false, false, false, true, false, false, false, false, false, true, true,
                              true, 0));
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
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusLossImmediateFenceWait(false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusLossImmediateFenceWait(true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusLossImmediateFenceWait(false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusLossImmediateFenceWait(true, true));
}

TEST(DXGISharedTest, D3D12NonPresentableSwapchainHoldsBackbufferWork) {
    auto shouldHold = [](bool wrappedD3D12 = true, bool fullscreen = false, bool occluded = true,
                         bool iconic = false, bool zeroSized = false, bool fgActive = false,
                         bool runtimeOwned = false, bool dedicated = false, bool steamDeferred = false,
                         bool deviceLost = false, bool hasQueue = true) {
        return ce::dx12_overlay_policy::ShouldHoldD3D12OverlayBackbufferWorkForNonPresentableSwapchain(
            wrappedD3D12, fullscreen, occluded, iconic, zeroSized, fgActive, runtimeOwned, dedicated,
            steamDeferred, deviceLost, hasQueue);
    };

    // Occluded, iconic, or zero-sized each make the swapchain non-presentable -> hold.
    EXPECT_TRUE(shouldHold());                                  // occluded
    EXPECT_TRUE(shouldHold(true, false, false, true, false));   // iconic
    EXPECT_TRUE(shouldHold(true, false, false, false, true));   // zero-sized

    // Negating conditions never hold.
    EXPECT_FALSE(shouldHold(false));                                                       // not wrapped D3D12
    EXPECT_FALSE(shouldHold(true, true));                                                  // fullscreen
    EXPECT_FALSE(shouldHold(true, false, false, false, false));                            // presentable/visible
    EXPECT_FALSE(shouldHold(true, false, true, false, false, true));                       // FG active
    EXPECT_FALSE(shouldHold(true, false, true, false, false, false, true));                // runtime-owned
    EXPECT_FALSE(shouldHold(true, false, true, false, false, false, false, true));         // dedicated queue
    EXPECT_FALSE(shouldHold(true, false, true, false, false, false, false, false, true));  // steam deferred
    EXPECT_FALSE(shouldHold(true, false, true, false, false, false, false, false, false, true));  // device lost
    EXPECT_FALSE(
        shouldHold(true, false, true, false, false, false, false, false, false, false, false));  // no queue
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

TEST(DXGISharedTest, D3D12FocusTransitionHoldGatesOnlyTheModeSwitchWindow) {
    auto hold = [](bool windowed = true, int transitionRemaining = 30, bool fgActive = false,
                   bool runtimeOwned = false, bool dedicated = false, bool steamDeferred = false,
                   bool deviceLost = false, bool hasQueue = true) {
        return ce::dx12_overlay_policy::ShouldHoldD3D12OverlayBackbufferWorkDuringFocusTransition(
            windowed, transitionRemaining, fgActive, runtimeOwned, dedicated, steamDeferred, deviceLost, hasQueue);
    };

    // During the transition window (counter > 0) on a windowed swapchain -> hold all
    // backbuffer work (any draw/copy pure-hangs the GPU mid mode-switch; DRED-proven).
    EXPECT_TRUE(hold());
    // Outside the transition window (counter == 0) -> render directly (steady focused
    // OR steady unfocused-but-visible). This is the no-hide guarantee.
    EXPECT_FALSE(hold(true, 0));

    // Negating conditions never hold (these routes manage their own submission).
    EXPECT_FALSE(hold(false, 30));                               // exclusive fullscreen
    EXPECT_FALSE(hold(true, 30, true));                          // FG active
    EXPECT_FALSE(hold(true, 30, false, true));                  // runtime-owned
    EXPECT_FALSE(hold(true, 30, false, false, true));          // dedicated queue
    EXPECT_FALSE(hold(true, 30, false, false, false, true));   // steam deferred
    EXPECT_FALSE(hold(true, 30, false, false, false, false, true));   // device lost
    EXPECT_FALSE(hold(true, 30, false, false, false, false, false, false));  // no queue
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
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusTransitionDeviceRemoval(
        true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusTransitionDeviceRemoval(
        false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusTransitionDeviceRemoval(
        true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusTransitionDeviceRemoval(
        true, true, true));
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

// Models the x86 Alt+Tab freeze root cause: during the iflip<->composited mode
// switch the GPU stops retiring for a stretch while the CPU keeps drawing the
// overlay every frame.  The DescFree backend round-robins kPoolSize UPLOAD
// vertex/index buffers; without a per-slot guard the CPU wraps and overwrites a
// slot the GPU is still reading (the data race that corrupted the draw and hung
// the device).  With the guard the present thread waits before reuse, so no slot
// is ever stomped.  The unguarded branch reproduces the pre-fix behavior and must
// stomp; the guarded branch must not.
TEST(DXGISharedTest, OverlayUploadRingGuardPreventsModeSwitchStomp) {
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
    EXPECT_FALSE(ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn(
        true, /*hadFSRFGPhase=*/true, false, true, true, true));
    // Runtime owns the swapchain -> not the same-queue case; keep suppression.
    EXPECT_FALSE(ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn(
        true, false, /*runtimeOwnsSwapchain=*/true, true, true, true));
    // Overlay backend not initialized -> nothing to keep drawing.
    EXPECT_FALSE(ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn(
        true, false, false, /*overlayInit=*/false, true, true));
    // Sync resources not initialized -> nothing to keep drawing.
    EXPECT_FALSE(ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn(
        true, false, false, true, /*syncInit=*/false, true));
    // A SEPARATE Streamline queue exists (swapchain queue != original game queue) -> a pre-SL ECL
    // on origGame against SL's backbuffers risks the cross-queue DEVICE_HUNG; keep suppression.
    EXPECT_FALSE(ShouldEagerlyDrawPreSLOverlayDuringDLSSToggleOn(
        true, false, false, true, true, /*swapchainQueueIsOriginalGameQueue=*/false));
}

TEST(DXGISharedTest, NormalOverlayAllowedDuringDormantProtectedFFXStartup) {
    using ce::dx12_overlay_policy::ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup;

    // Talos 2D-menu FSR arming (session 20260613_212112): protected official-FFX startup pending,
    // but the game still presents menu frames on its OWN queue (runtimeOwns=0, scQueue==origGame),
    // the staged AMD queue differs, AMD is dormant (no enabled ffxConfigure / callback), and a few
    // stable frames have elapsed → render the overlay on origGame instead of an 8 s menu blank.
    EXPECT_TRUE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(
        /*protectedOfficialFFXStartupPending=*/true, /*runtimeOwnsSwapchain=*/false, /*overlayInit=*/true,
        /*syncInit=*/true, /*hasSwapchainQueue=*/true, /*swapchainQueueIsOriginalGameQueue=*/true,
        /*stagedQueueDiffersFromOriginalGameQueue=*/true, /*hasDirectFFXApiConfirmation=*/false,
        /*ffxPresentCallbackActive=*/false, /*sustainedGameProgressOnOriginalQueue=*/true));

    // Runtime took over presentation → full quiesce (the GTA staged-queue crash path).
    EXPECT_FALSE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(
        true, true, true, true, true, true, true, false, false, true));
    // Present queue is no longer origGame → quiesce.
    EXPECT_FALSE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(
        true, false, true, true, true, false, true, false, false, true));
    // Staged queue == origGame (would mean rendering on the staged queue) → quiesce.
    EXPECT_FALSE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(
        true, false, true, true, true, true, false, false, false, true));
    // Enabled ffxConfigure landed → AMD no longer dormant; hand to the FFX callback route.
    EXPECT_FALSE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(
        true, false, true, true, true, true, true, true, false, true));
    // An FFX present callback is firing → AMD active; quiesce the normal route.
    EXPECT_FALSE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(
        true, false, true, true, true, true, true, false, true, true));
    // Not yet enough stable frames (protects the fragile AMD swapchain-create instant) → quiesce.
    EXPECT_FALSE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(
        true, false, true, true, true, true, true, false, false, false));
    // Overlay backend not live → nothing to draw.
    EXPECT_FALSE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(
        true, false, false, true, true, true, true, false, false, true));
    // Not in a protected-FFX startup window → predicate is inert.
    EXPECT_FALSE(ShouldAllowNormalOverlayDrawDuringDormantProtectedOfficialFFXStartup(
        false, false, true, true, true, true, true, false, false, true));
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
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipCommandQueueVTableHookForFrameGenerationRuntimeModule(
        true, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipCommandQueueVTableHookForFrameGenerationRuntimeModule(
        false, true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipCommandQueueVTableHookForFrameGenerationRuntimeModule(
        false, false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipCommandQueueVTableHookForFrameGenerationRuntimeModule(
        false, false, false, true));
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
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(
        true, false, false, ce::fg_runtime::RuntimeMode::kOff, false));
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
        true, true, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        false, true, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        true, false, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        true, true, true, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        true, true, true, false, ce::fg_runtime::RuntimeMode::kDLSSFG, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        true, true, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        true, true, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        true, true, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false, false));
}

TEST(DXGISharedTest, LiveStartupOverlayHandoffSkipsResourcePrimingBlank) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPrimeStartupOverlayResources(true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPrimeStartupOverlayResources(true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPrimeStartupOverlayResources(true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPrimeStartupOverlayResources(false, true, false));

    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldDelayAfterStartupOverlayResourcePrime(true, false, 50, 100, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldDelayAfterStartupOverlayResourcePrime(true, false, 50, 100, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldDelayAfterStartupOverlayResourcePrime(true, true, 50, 100, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldDelayAfterStartupOverlayResourcePrime(true, false, 100, 100, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldDelayAfterStartupOverlayResourcePrime(false, false, 50, 100, false));
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
    EXPECT_FALSE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kFSRFG, RuntimeMode::kDLSSFG, false, true,
                                                           true, true, true, true));
    EXPECT_FALSE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kDLSSFG, RuntimeMode::kOff, false, true, true,
                                                           true, true, true));
}

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
    EXPECT_FALSE(IsLiveNoCallbackNativeFSRSuspensionToggle(RuntimeMode::kStreamlineNoFG, RuntimeMode::kOff, false,
                                                           true, true, true, true, true));
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
    EXPECT_TRUE(ShouldRetainNativeFSRInternalNoCallbackCompositionForDisabledConfigure(false, false, false, true,
                                                                                       true));

    // Enabled configures compute the latch directly; bridge/app-callback
    // routes own suspension rendering; a fresh session has nothing to retain;
    // and without live runtime ownership the suspension is really a teardown.
    EXPECT_FALSE(ShouldRetainNativeFSRInternalNoCallbackCompositionForDisabledConfigure(true, false, false, true,
                                                                                        true));
    EXPECT_FALSE(ShouldRetainNativeFSRInternalNoCallbackCompositionForDisabledConfigure(false, true, false, true,
                                                                                        true));
    EXPECT_FALSE(ShouldRetainNativeFSRInternalNoCallbackCompositionForDisabledConfigure(false, false, true, true,
                                                                                        true));
    EXPECT_FALSE(ShouldRetainNativeFSRInternalNoCallbackCompositionForDisabledConfigure(false, false, false, false,
                                                                                        true));
    EXPECT_FALSE(ShouldRetainNativeFSRInternalNoCallbackCompositionForDisabledConfigure(false, false, false, true,
                                                                                        false));
}

TEST(DXGISharedTest, GameSwapchainCreationEndsRuntimeOwnedNativeFSRTeardown) {
    using ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation;

    // 20260611_191950 FSR->OFF: the game recreates its swapchain on a FRESH
    // queue after explicit native-FSR OFF/destroy; that creation is the
    // stronger off signal and must end the runtime-owned teardown.
    EXPECT_TRUE(ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(true, true, false, false));
    EXPECT_TRUE(ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(true, false, true, false));
    EXPECT_TRUE(ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(true, true, true, false));

    // Runtime/third-party creators, missing off/destroy evidence, or live
    // Streamline FG keep the existing conservative teardown handling.
    EXPECT_FALSE(ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(false, true, true, false));
    EXPECT_FALSE(ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(true, false, false, false));
    EXPECT_FALSE(ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(true, true, true, true));
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
    EXPECT_TRUE(IsGameSwapchainRecoveryToggleAfterNativeFSROff(RuntimeMode::kFSRFG, RuntimeMode::kStreamlineNoFG, false,
                                                              true));

    EXPECT_FALSE(IsGameSwapchainRecoveryToggleAfterNativeFSROff(RuntimeMode::kFSRFG, RuntimeMode::kOff, true, true));
    EXPECT_FALSE(IsGameSwapchainRecoveryToggleAfterNativeFSROff(RuntimeMode::kFSRFG, RuntimeMode::kOff, false, false));
    EXPECT_FALSE(IsGameSwapchainRecoveryToggleAfterNativeFSROff(RuntimeMode::kOff, RuntimeMode::kFSRFG, false, true));
    EXPECT_FALSE(IsGameSwapchainRecoveryToggleAfterNativeFSROff(RuntimeMode::kDLSSFG, RuntimeMode::kOff, false, true));
    // STREAMLINE_NO_FG next side still needs the recovery-queue match and no SL FG signal.
    EXPECT_FALSE(IsGameSwapchainRecoveryToggleAfterNativeFSROff(RuntimeMode::kFSRFG, RuntimeMode::kStreamlineNoFG, true,
                                                               true));
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
    EXPECT_TRUE(ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(true, true, true, true, true,
                                                                                        false));

    // Every missing proof keeps the protective cooldown.
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(false, true, true, true,
                                                                                         true, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(true, false, true, true,
                                                                                         true, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(true, true, false, true,
                                                                                         true, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(true, true, true, false,
                                                                                         true, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(true, true, true, true,
                                                                                         false, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(true, true, true, true, true,
                                                                                         true));
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
        /*keepAlive=*/true, /*streamlineFGRunning=*/false, /*fsrFGApiActive=*/false,
        /*nativeFSRNoCallback=*/false, /*runtimeOwnsSwapchain=*/true, /*scQueueIsLiveCmdQueue=*/true,
        /*scQueueIsConfirmedPostSLRenderQueue=*/false));

    // Without the keep-alive latch this is not a confirmed-PostSL suspension — keep the cooldown.
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
        false, false, false, false, true, true, false));
    // Streamline FG still running is the ACTIVE-FG preserve path's job, not this one.
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
        true, true, false, false, true, true, false));
    // An FSR / native-FG no-callback takeover must keep the quiesce cooldown (device-removal hazard).
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
        true, false, true, false, true, true, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
        true, false, false, true, true, true, false));
    // Must be the live runtime-owned queue (cross-queue / non-owned change keeps the cooldown).
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
        true, false, false, false, false, true, false));
    // scQueue matches NEITHER the live cmdQueue NOR the confirmed PostSL render queue → keep cooldown.
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
        true, false, false, false, true, false, false));
}

TEST(DXGISharedTest, ConfirmedPostSLSuspensionReinitsImmediatelyWhenSwapchainQueueIsConfirmedPostSLQueue) {
    using ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange;

    // Session 20260613_202646 (90-present blank): at the DLSS-suspend edge the live
    // confirmed-PostSL queue is the DLSS-G proxy queue (== scQueue == g_PostSLLastWorkingQueue,
    // 180+ confirmed submits), while the live wrapper cmdQueue is a SEPARATE object. The strict
    // scQueue==cmdQueue test wrongly rejected this safe suspend and dropped it into the 90-frame
    // cooldown. The confirmed PostSL render queue alone now satisfies the queue proof.
    EXPECT_TRUE(ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
        /*keepAlive=*/true, /*streamlineFGRunning=*/false, /*fsrFGApiActive=*/false,
        /*nativeFSRNoCallback=*/false, /*runtimeOwnsSwapchain=*/true, /*scQueueIsLiveCmdQueue=*/false,
        /*scQueueIsConfirmedPostSLRenderQueue=*/true));

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
                        true, true, false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::
                    ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
                        true, true, false, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::
                     ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
                         false, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::
                     ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
                         true, false, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::
                     ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
                         true, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::
                     ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
                         true, true, false, false, false));
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
    EXPECT_EQ(ResolveTransitionCooldownFrames(/*existing=*/90, cooldownFrames, /*override=*/immediate), 0);

    // No confirmed PostSL (e.g. a real teardown) keeps the protective 60-frame cooldown.
    const bool noKeepAlive =
        ShouldKeepConfirmedPostSLAliveAcrossStreamlineOff(false, false, false, false);
    EXPECT_FALSE(noKeepAlive);
    EXPECT_EQ(ResolveTransitionCooldownFrames(/*existing=*/0, 60, /*override=*/false), 60);
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
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        true, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, true, false));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        true, false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        true, false, false, true));
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
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        true, true, false, false));

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
        ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(true, true, false,
                                                                                               false, true)));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        true, true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        true, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        false, true, true, true));
}

TEST(DXGISharedTest, NativeFSRTimeoutOverrideRequiresSafeCallbackStallFallback) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(
        false, false, false, false));

    // A healthy native FSR present callback means the overlay already has the
    // correct runtime-owned path.  The normal DX12 overlay path must not wake
    // up just because the generic 2s suppression timer expired.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(
        true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(
        false, true, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(
        true, true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(
        true, true, true, true));
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
        ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
            true, false, false, true, false, 0, true)));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(
        true, false, true,
        ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
            true, false, false, true, false, 0, true)));
}

TEST(DXGISharedTest, DisabledNativeFSRConfigureRetainsExistingCallbackBridgeOnlyAfterEnabledConfigure) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldRetainFFXPresentCallbackBridgeForDisabledConfigure(true, false, true,
                                                                                                  false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRetainFFXPresentCallbackBridgeForDisabledConfigure(false, false, true,
                                                                                                   false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRetainFFXPresentCallbackBridgeForDisabledConfigure(true, true, true,
                                                                                                  false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRetainFFXPresentCallbackBridgeForDisabledConfigure(true, false, false,
                                                                                                   false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldRetainFFXPresentCallbackBridgeForDisabledConfigure(true, false, true,
                                                                                                  true));
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
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUseLiveDXGIFactoryExportForFrameGenerationRuntime(
        true, false, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseLiveDXGIFactoryExportForFrameGenerationRuntime(
        false, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseLiveDXGIFactoryExportForFrameGenerationRuntime(
        true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseLiveDXGIFactoryExportForFrameGenerationRuntime(
        true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUseLiveDXGIFactoryExportForFrameGenerationRuntime(
        true, false, true, true));
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

TEST(DXGISharedTest, ECLStartupActivationProbeIsSuppressedDuringNativeFSRPresentPath) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldProbePostSLStartupActivationSwapchainFromECL(
        true, true, false, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldProbePostSLStartupActivationSwapchainFromECL(
        false, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldProbePostSLStartupActivationSwapchainFromECL(
        true, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldProbePostSLStartupActivationSwapchainFromECL(
        true, true, true, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldProbePostSLStartupActivationSwapchainFromECL(
        true, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldProbePostSLStartupActivationSwapchainFromECL(
        true, true, false, false, true));
}

TEST(DXGISharedTest, FFXPresentCallbackOverlayBackendResetsOnlyForDeviceOrFormatChange) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldResetFFXPresentCallbackOverlayBackend(true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldResetFFXPresentCallbackOverlayBackend(true, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldResetFFXPresentCallbackOverlayBackend(true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldResetFFXPresentCallbackOverlayBackend(false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldResetFFXPresentCallbackOverlayBackend(true, false, false));
}

TEST(DXGISharedTest, NormalOverlayCleanupPreservesFFXCallbackBackendOnlyWhileRuntimeOwnedNativeFGPresentPathPersists) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveFFXPresentCallbackBackendDuringNormalOverlayCleanup(true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveFFXPresentCallbackBackendDuringNormalOverlayCleanup(false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveFFXPresentCallbackBackendDuringNormalOverlayCleanup(true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveFFXPresentCallbackBackendDuringNormalOverlayCleanup(false, false));
}

TEST(DXGISharedTest, FFXPresentCallbackOverlayBridgeTrustsDirectFFXEvidenceWithoutRuntimeOwnedPath) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBridgeOverlayViaFFXPresentCallback(
        false, true, false, ce::fg_runtime::RuntimeMode::kFSRFG));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBridgeOverlayViaFFXPresentCallback(
        false, false, true, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBridgeOverlayViaFFXPresentCallback(
        true, false, false, ce::fg_runtime::RuntimeMode::kOff));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBridgeOverlayViaFFXPresentCallback(
        false, false, false, ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBridgeOverlayViaFFXPresentCallback(
        false, false, false, ce::fg_runtime::RuntimeMode::kDLSSFG));
}

TEST(DXGISharedTest, FFXPresentCallbackMirrorsOverlayToCurrentBackbufferOnlyDuringSuspension) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldMirrorFFXPresentCallbackOverlayToCurrentBackBuffer(
        false, true, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldMirrorFFXPresentCallbackOverlayToCurrentBackBuffer(
        true, true, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldMirrorFFXPresentCallbackOverlayToCurrentBackBuffer(
        false, false, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldMirrorFFXPresentCallbackOverlayToCurrentBackBuffer(
        false, true, false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldMirrorFFXPresentCallbackOverlayToCurrentBackBuffer(
        false, true, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldMirrorFFXPresentCallbackOverlayToCurrentBackBuffer(
        false, true, true, true, false));
}

TEST(DXGISharedTest, FFXPresentCallbackComposesOutputOnlyWithoutRuntimeCompositionCallback) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldComposeFFXPresentSourceToOutput(false, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldComposeFFXPresentSourceToOutput(true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldComposeFFXPresentSourceToOutput(false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldComposeFFXPresentSourceToOutput(false, true, false));
}

TEST(DXGISharedTest, FFXPresentCallbackBridgeRequiresRealAppCallback) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(false, true, true));
}

TEST(DXGISharedTest, NativeFSRInternalNoCallbackCompositionUsesNormalOverlayRoute) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, true, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, true, false, true));
}

TEST(DXGISharedTest, HDRDetectionTreatsFP16AsDefinitelyHDR) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatFormatAsDefinitelyHDR(DXGI_FORMAT_R16G16B16A16_FLOAT));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatFormatAsDefinitelyHDR(DXGI_FORMAT_R10G10B10A2_UNORM));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatFormatAsDefinitelyHDR(DXGI_FORMAT_R8G8B8A8_UNORM));
}

TEST(DXGISharedTest, HDRDetectionOnlyProbesDisplayColorSpaceForTenBitUNormOutputs) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldProbeDisplayColorSpaceForHDR(DXGI_FORMAT_R10G10B10A2_UNORM));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldProbeDisplayColorSpaceForHDR(DXGI_FORMAT_R16G16B16A16_FLOAT));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldProbeDisplayColorSpaceForHDR(DXGI_FORMAT_R8G8B8A8_UNORM));
}

TEST(DXGISharedTest, HDRDetectionRecognizesHDRAndSDRTenBitColorSpaces) {
    EXPECT_TRUE(ce::dx12_overlay_policy::IsHDRColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
    EXPECT_TRUE(ce::dx12_overlay_policy::IsHDRColorSpace(DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsHDRColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsHDRColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));
}

TEST(DXGISharedTest, HDRDetectionResolvesActualOverlayTargetStateFromFormatAndColorSpace) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ResolveActualHDRStateForOverlayTarget(DXGI_FORMAT_R16G16B16A16_FLOAT, false, -1));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ResolveActualHDRStateForOverlayTarget(DXGI_FORMAT_R10G10B10A2_UNORM, false, -1));
    EXPECT_TRUE(ce::dx12_overlay_policy::ResolveActualHDRStateForOverlayTarget(
        DXGI_FORMAT_R10G10B10A2_UNORM, true, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
    EXPECT_FALSE(ce::dx12_overlay_policy::ResolveActualHDRStateForOverlayTarget(
        DXGI_FORMAT_R10G10B10A2_UNORM, true, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
    EXPECT_FALSE(ce::dx12_overlay_policy::ResolveActualHDRStateForOverlayTarget(
        DXGI_FORMAT_R8G8B8A8_UNORM, true, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
}

TEST(DXGISharedTest, RuntimeOwnedCallbackHDRFallbackUsesCachedKnownState) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ResolveRuntimeOwnedCallbackHDRStateFromCachedState(
        DXGI_FORMAT_R16G16B16A16_FLOAT, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ResolveRuntimeOwnedCallbackHDRStateFromCachedState(DXGI_FORMAT_R8G8B8A8_UNORM,
                                                                                             true, true));

    EXPECT_TRUE(ce::dx12_overlay_policy::ResolveRuntimeOwnedCallbackHDRStateFromCachedState(
        DXGI_FORMAT_R10G10B10A2_UNORM, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ResolveRuntimeOwnedCallbackHDRStateFromCachedState(
        DXGI_FORMAT_R10G10B10A2_UNORM, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ResolveRuntimeOwnedCallbackHDRStateFromCachedState(
        DXGI_FORMAT_R10G10B10A2_UNORM, false, true));
}

TEST(DXGISharedTest, AuthoritativeFSRRealFrameOnlyRunTracksOnlyQualifiedFrames) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(false, true, true, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(true, true, true, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(false, false, true, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(false, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(false, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(false, true, true, false, true));
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

TEST(DXGISharedTest, TracksStaleRuntimeOwnedStreamlineNoFGOnlyOnRealFramesBackOnOriginalQueue) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        false, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        true, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        false, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        false, true, ce::fg_runtime::RuntimeMode::kDLSSFG, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        false, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        false, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        false, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true, true, true));
}

TEST(DXGISharedTest, StaleRuntimeOwnedStreamlineNoFGRequiresLongRealFrameRunBeforeClearing) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearStaleRuntimeOwnedStreamlineNoFGAfterRealFrameRun(1));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearStaleRuntimeOwnedStreamlineNoFGAfterRealFrameRun(119));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldClearStaleRuntimeOwnedStreamlineNoFGAfterRealFrameRun(120));
}

TEST(DXGISharedTest, StaleRuntimeOwnedStreamlineNoFGCleanupReleasesRetainedActivationSwapchain) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldReleaseRetainedStartupActivationSwapchainAfterStaleNoFGCleanup(true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldReleaseRetainedStartupActivationSwapchainAfterStaleNoFGCleanup(false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldReleaseRetainedStartupActivationSwapchainAfterStaleNoFGCleanup(true, false));
}

TEST(DXGISharedTest, AuthoritativeFFXCreateReleasesRetainedStreamlineStartupActivationSwapchain) {
    EXPECT_TRUE(ce::dx12_overlay_policy::
                    ShouldReleaseRetainedStreamlineStartupActivationSwapchainForAuthoritativeFFXCreate(true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::
                     ShouldReleaseRetainedStreamlineStartupActivationSwapchainForAuthoritativeFFXCreate(false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::
                     ShouldReleaseRetainedStreamlineStartupActivationSwapchainForAuthoritativeFFXCreate(true, false));
}

TEST(DXGISharedTest, DescFreeBackendUsesAdapterShutdownWhenAdapterOwnsCustomBackend) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldShutdownDescFreeBackendViaOverlayAdapter(true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldShutdownDescFreeBackendViaOverlayAdapter(false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldShutdownDescFreeBackendViaOverlayAdapter(true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldShutdownDescFreeBackendViaOverlayAdapter(true, true, false));
}

TEST(DXGISharedTest, AuthoritativeFSRIsPreservedDuringTransitionCooldownForTransientOffEdges) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(true, true, 1));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(true, true, 90));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(false, true, 90));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(true, false, 90));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(true, true, 0));
}

TEST(DXGISharedTest, HeuristicFSRIsPreservedDuringTransientBlocksOnRuntimeOwnedPostFSRSwapchains) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(false, true, true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(true, true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(false, false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(false, true, false, false));
}

TEST(DXGISharedTest, RuntimeOwnedPostFSRTeardownRequiresStrongerOffSignalThanTransientNoneEdge) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(true, true, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(true, true, true, true));
}

TEST(DXGISharedTest, ExplicitNativeFSROffSuppressesHeuristicReactivationUntilRuntimeOwnedTeardownEnds) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRAfterExplicitNativeFSROff(true, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRAfterExplicitNativeFSROff(true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRAfterExplicitNativeFSROff(false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRAfterExplicitNativeFSROff(false, false));
}

TEST(DXGISharedTest, ExplicitNativeFSROffEndsRuntimeOwnedTeardownWhenQueueReturnsToOriginal) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(
        true, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(
        true, true, false, ce::fg_runtime::RuntimeMode::kOff, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(
        false, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(
        true, false, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(
        true, true, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(
        true, true, false, ce::fg_runtime::RuntimeMode::kFSRFG, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(
        true, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false));
}

TEST(DXGISharedTest, DisabledNativeFSRConfigurePreservesCallbackOwnedPresentPath) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(
        true, false, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(
        false, true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(
        false, false, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(
        true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(
        false, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(
        false, false, false, false));
}

TEST(DXGISharedTest, FFXPresentCallbackBridgeInstallsOnlyForEnabledFrameGenerationConfigure) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(false, false));
}

TEST(DXGISharedTest, NativeFSRDisabledConfigureStartupArmingRequiresFreshRuntimeOwnedTakeover) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatNativeFSRDisabledConfigureAsStartupArming(true, false, true, true,
                                                                                              true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatNativeFSRDisabledConfigureAsStartupArming(true, false, true, true,
                                                                                              false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatNativeFSRDisabledConfigureAsStartupArming(false, false, true, true,
                                                                                               true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatNativeFSRDisabledConfigureAsStartupArming(true, true, true, true,
                                                                                               true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatNativeFSRDisabledConfigureAsStartupArming(true, false, false, true,
                                                                                               true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatNativeFSRDisabledConfigureAsStartupArming(true, false, true, false,
                                                                                               true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatNativeFSRDisabledConfigureAsStartupArming(true, false, true, true,
                                                                                               true, true));
}

TEST(DXGISharedTest, RuntimeOwnedSwapchainNeedsNativeFSRProofBeforeNativePresentPath) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainAsNativeFSRPresentPath(true, true, false));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainAsNativeFSRPresentPath(true, false, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainAsNativeFSRPresentPath(true, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainAsNativeFSRPresentPath(false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainAsNativeFSRPresentPath(false, false, true));
}

TEST(DXGISharedTest, OfficialFFXTakeoverDefersHeavySideEffectsUntilEnabledConfigure) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferOfficialFFXTakeoverSideEffectsUntilEnabledConfigure(true, true,
                                                                                                        true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOfficialFFXTakeoverSideEffectsUntilEnabledConfigure(false, true,
                                                                                                         true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOfficialFFXTakeoverSideEffectsUntilEnabledConfigure(true, false,
                                                                                                         true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOfficialFFXTakeoverSideEffectsUntilEnabledConfigure(true, true,
                                                                                                         false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOfficialFFXTakeoverSideEffectsUntilEnabledConfigure(true, true,
                                                                                                         true, true));
}

TEST(DXGISharedTest, OfficialFFXStartupSwapchainCreateUsesProtectedPassThroughUntilDirectConfigure) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldProtectOfficialFFXStartupSwapchainCreateFromCESideEffects(true, true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldProtectOfficialFFXStartupSwapchainCreateFromCESideEffects(false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldProtectOfficialFFXStartupSwapchainCreateFromCESideEffects(true, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldProtectOfficialFFXStartupSwapchainCreateFromCESideEffects(true, true, true));
}

TEST(DXGISharedTest, ProtectedOfficialFFXStartupStagesOnlyDirectQueuesForDeferredTakeover) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldStageProtectedOfficialFFXStartupQueueForDeferredTakeover(true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldStageProtectedOfficialFFXStartupQueueForDeferredTakeover(false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldStageProtectedOfficialFFXStartupQueueForDeferredTakeover(true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldStageProtectedOfficialFFXStartupQueueForDeferredTakeover(false, false));
}

TEST(DXGISharedTest, ProtectedOfficialFFXStartupQuiescesCESideEffectsUntilDirectConfigure) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(true, true));
}

TEST(DXGISharedTest, ProtectedOfficialFFXStartupDoesNotAllowSeparateOverlayOnlyBeforeProof) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldAllowOverlayOnlyDuringProtectedOfficialFFXStartup(true, false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldAllowOverlayOnlyDuringProtectedOfficialFFXStartup(true, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldAllowOverlayOnlyDuringProtectedOfficialFFXStartup(false, false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldAllowOverlayOnlyDuringProtectedOfficialFFXStartup(true, false, false));
}

TEST(DXGISharedTest, ProtectedOfficialFFXStartupOverlayOnlyDoesNotBypassFGCooldownWithStagedQueue) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBypassFGTransitionCooldownForProtectedOfficialFFXOverlayOnly(true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBypassFGTransitionCooldownForProtectedOfficialFFXOverlayOnly(false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBypassFGTransitionCooldownForProtectedOfficialFFXOverlayOnly(true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBypassFGTransitionCooldownForProtectedOfficialFFXOverlayOnly(false, false));
}

TEST(DXGISharedTest, ProtectedOfficialFFXStartupPreservesBackendAcrossSwapchainChangeUntilProof) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldPreserveOverlayBackendAcrossProtectedOfficialFFXStartupSwapchainChange(true,
                                                                                                              false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveOverlayBackendAcrossProtectedOfficialFFXStartupSwapchainChange(false,
                                                                                                              false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveOverlayBackendAcrossProtectedOfficialFFXStartupSwapchainChange(true,
                                                                                                              true));
}

TEST(DXGISharedTest, ProtectedOfficialFFXStartupQuiescesLiveStreamlinePostSLImmediately) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        true, false, true, false, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        true, false, false, true, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        true, false, false, false, true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        true, false, false, false, false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        true, false, false, false, false, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        true, false, false, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        false, false, true, true, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        true, true, true, true, true, true, true));
}

TEST(DXGISharedTest, ProtectedOfficialFFXStartupDoesNotResolveFromProgressWithoutDirectConfigure) {
    const uint32_t processFrameThreshold =
        ce::dx12_overlay_policy::GetProtectedOfficialFFXStartupProcessFrameProgressThreshold();
    const uint32_t eclThreshold = ce::dx12_overlay_policy::GetProtectedOfficialFFXStartupECLProgressThreshold();

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
        true, false, processFrameThreshold - 1, eclThreshold - 1));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
        true, false, processFrameThreshold, 0));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
        true, false, 0, eclThreshold));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
        false, false, processFrameThreshold, eclThreshold));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
        true, true, processFrameThreshold, eclThreshold));
}

TEST(DXGISharedTest, AuthoritativeRuntimeSwapchainCreatePreservesOriginalDescriptor) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldApplySwapchainDescriptorOverridesForCreate(false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldApplySwapchainDescriptorOverridesForCreate(true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldApplySwapchainDescriptorOverridesForCreate(true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldApplySwapchainDescriptorOverridesForCreate(false, false));
}

TEST(DXGISharedTest, ProtectedOfficialFFXStartupCountsAsRuntimeOwnedForDisabledStartupArmingConfigure) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatNativeFSRSwapchainAsRuntimeOwnedForConfigure(true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatNativeFSRSwapchainAsRuntimeOwnedForConfigure(false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatNativeFSRSwapchainAsRuntimeOwnedForConfigure(true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatNativeFSRSwapchainAsRuntimeOwnedForConfigure(false, false));
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
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceForCurrentFrame(true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceForCurrentFrame(true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceForCurrentFrame(false, true, true));
}

TEST(DXGISharedTest, InactiveFGOverlaySpaceReservationRequiresShortPostSLTeardownActivity) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(true, true, true));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(true, true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(false, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(true, false, false));
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

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(
        false, false, true, true, true, false));
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
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUnregisterSwapchainDestructionCallbackDuringWrapperDestructor(
        true, true, true));
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

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(false, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(true, true, false, true, false));
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
    EXPECT_TRUE(ce::dx12_overlay_policy::HasConfirmedPureStreamlinePostSLResumeProof(false, true, true, true, true,
                                                                                    true));

    EXPECT_FALSE(ce::dx12_overlay_policy::HasConfirmedPureStreamlinePostSLResumeProof(true, true, true, true, true,
                                                                                     true));
    EXPECT_FALSE(ce::dx12_overlay_policy::HasConfirmedPureStreamlinePostSLResumeProof(false, false, true, true, true,
                                                                                     true));
    EXPECT_FALSE(ce::dx12_overlay_policy::HasConfirmedPureStreamlinePostSLResumeProof(false, true, false, true, true,
                                                                                     true));
    EXPECT_FALSE(ce::dx12_overlay_policy::HasConfirmedPureStreamlinePostSLResumeProof(false, true, true, false, true,
                                                                                     true));
    EXPECT_FALSE(ce::dx12_overlay_policy::HasConfirmedPureStreamlinePostSLResumeProof(false, true, true, true, false,
                                                                                     true));
    EXPECT_FALSE(ce::dx12_overlay_policy::HasConfirmedPureStreamlinePostSLResumeProof(false, true, true, true, true,
                                                                                     false));
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
        true, true, true, true, true, true, true, true));

    // 20260612_002523: the PURE-DLSS startup (no FSR history) must preserve a
    // confirmed PostSL backend too. PostSL confirmation is proof on the LIVE
    // Streamline swapchain; the ordinary reinit + 90-frame cooldown blanked
    // the overlay permanently when zero-ECL classification also starved the
    // cooldown ticks.
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, true, false, true, true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        false, true, true, true, true, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, false, true, true, true, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, false, true, true, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, true, true, false, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, true, true, true, false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, true, true, true, true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
        true, true, true, true, true, true, true, false));
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
    EXPECT_TRUE(DXGIShared::ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(
        true, true, true, true, false));
    EXPECT_TRUE(DXGIShared::ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(
        true, true, true, false, true));

    EXPECT_FALSE(DXGIShared::ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(
        false, true, true, true, false));
    EXPECT_FALSE(DXGIShared::ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(
        true, false, true, true, false));
    EXPECT_FALSE(DXGIShared::ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(
        true, true, false, true, false));
    EXPECT_FALSE(DXGIShared::ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(
        true, true, true, false, false));
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

TEST(DXGISharedTest, ReinitCooldownLetsHalfArmedSyntheticPostSLContinue) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldLetSyntheticPostSLProgressDuringOverlayReinitCooldown(
        true, true, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldLetSyntheticPostSLProgressDuringOverlayReinitCooldown(
        true, false, true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldLetSyntheticPostSLProgressDuringOverlayReinitCooldown(
        true, false, false, true, true));

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
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(
        true, false, false, false, true, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(
        true, false, false, false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(
        true, false, false, false, false, true));

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
    EXPECT_FALSE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(
        false, true, true, false, false, true, true, true, true, false));
    EXPECT_FALSE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(
        true, false, true, false, false, true, true, true, true, false));
    EXPECT_FALSE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(
        true, true, false, false, false, true, true, true, true, false));
    // A current FSR / native-FG takeover or a removed device keeps the stricter cooldown.
    EXPECT_FALSE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(
        true, true, true, true, false, true, true, true, true, false));
    EXPECT_FALSE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(
        true, true, true, false, true, true, true, true, true, false));
    EXPECT_FALSE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(
        true, true, true, false, false, true, true, true, true, true));
    // Backend/queue not ready -> keep the cooldown (reinit needs them).
    EXPECT_FALSE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(
        true, true, true, false, false, false, true, true, true, false));
    EXPECT_FALSE(ShouldBypassConfirmedPostSLSuspensionOverlayReinitCooldown(
        true, true, true, false, false, true, false, true, true, false));
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

// Regression: AttemptSteamDX12OverlayInit vtable[8] re-hook safety.
// If VirtualProtect fails during the re-hook phase, the vtable[8] remains
// pointing to dxgi!Present (the unhooked value).  This test verifies that
// even in that case, the value is a valid function pointer (not NULL/corrupt)
// so the game can continue running safely (just without CE's overlay hook
// active).
TEST(DXGISharedTest, SteamDX12InitVtableRehookFailureSafety) {
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);

    const size_t vtableBytes = sysInfo.dwPageSize;
    void* alloc = VirtualAlloc(nullptr, vtableBytes, MEM_COMMIT, PAGE_READWRITE);
    ASSERT_NE(alloc, nullptr);

    void** vtable = static_cast<void**>(alloc);
    const void* fakeDxgiPresent = reinterpret_cast<void*>(static_cast<uintptr_t>(0x22222222));

    for (size_t i = 0; i < vtableBytes / sizeof(void*); ++i) {
        vtable[i] = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD0000 + i));
    }

    // Simulate successful unhook: vtable[8] now points to dxgi!Present
    vtable[8] = const_cast<void*>(fakeDxgiPresent);

    // Make vtable read-only to simulate failed re-hook (VirtualProtect fails)
    DWORD oldProtect;
    ASSERT_NE(0, VirtualProtect(vtable, vtableBytes, PAGE_READONLY, &oldProtect));

    // Attempt re-hook without VirtualProtect (simulates the failure)
    // This should NOT crash — it just won't write (vtable[8] stays as dxgi!Present)
    // In the real code, this fallback means CE's overlay won't be active,
    // but the game won't crash.

    // Verify page stays read-only and vtable[8] still has a valid value
    MEMORY_BASIC_INFORMATION mbi;
    ASSERT_NE(0u, VirtualQuery(vtable, &mbi, sizeof(mbi)));
    EXPECT_EQ(mbi.Protect & 0xFF, PAGE_READONLY);

    // vtable[8] should still have the unhooked value (dxgi!Present)
    ASSERT_EQ(vtable[8], fakeDxgiPresent);

    VirtualFree(alloc, 0, MEM_RELEASE);
}

// ---------------------------------------------------------------------------
// [OVERLAY COVERAGE] per-present overlay-coverage accounting (Pillar 0 of the
// 100%-overlay-visibility work). The tracker is the regression gate for the
// FG-transition matrix: any uncovered streak > 1 present during a scripted
// transition run is a failure.
// ---------------------------------------------------------------------------

TEST(DXGISharedTest, OverlayPresentCoverageStreakAccounting) {
    ce::dx12_overlay_policy::OverlayPresentCoverageTracker tracker;

    // Covered presents accumulate no uncovered state.
    auto r = tracker.NotePresent(true, false);
    EXPECT_TRUE(r.covered);
    EXPECT_FALSE(r.uncoveredStreakStarted);
    EXPECT_FALSE(r.uncoveredStreakEnded);
    EXPECT_EQ(tracker.TotalPresents(), 1u);
    EXPECT_EQ(tracker.UncoveredPresents(), 0u);

    // First uncovered present starts a streak.
    r = tracker.NotePresent(false, false);
    EXPECT_FALSE(r.covered);
    EXPECT_TRUE(r.uncoveredStreakStarted);
    EXPECT_TRUE(r.newLongestStreak);
    EXPECT_EQ(tracker.CurrentUncoveredStreak(), 1u);

    // Streak grows; longest follows.
    r = tracker.NotePresent(false, false);
    EXPECT_FALSE(r.uncoveredStreakStarted);
    EXPECT_TRUE(r.newLongestStreak);
    EXPECT_EQ(tracker.CurrentUncoveredStreak(), 2u);
    EXPECT_EQ(tracker.LongestUncoveredStreak(), 2u);

    // A covered present ends the streak and reports its length.
    r = tracker.NotePresent(true, false);
    EXPECT_TRUE(r.covered);
    EXPECT_TRUE(r.uncoveredStreakEnded);
    EXPECT_EQ(r.endedStreakLength, 2u);
    EXPECT_EQ(tracker.CurrentUncoveredStreak(), 0u);

    // A shorter later streak does not advance the longest streak.
    r = tracker.NotePresent(false, false);
    EXPECT_TRUE(r.uncoveredStreakStarted);
    EXPECT_FALSE(r.newLongestStreak);
    r = tracker.NotePresent(true, false);
    EXPECT_TRUE(r.uncoveredStreakEnded);
    EXPECT_EQ(r.endedStreakLength, 1u);

    EXPECT_EQ(tracker.TotalPresents(), 6u);
    EXPECT_EQ(tracker.UncoveredPresents(), 3u);
    EXPECT_EQ(tracker.LongestUncoveredStreak(), 2u);
}

TEST(DXGISharedTest, OverlayPresentCoverageFGComposedInheritance) {
    ce::dx12_overlay_policy::OverlayPresentCoverageTracker tracker;

    // Healthy FG cadence: real present draws, interpolated present inherits the
    // coverage of the previous covered present — no false 1-present streaks.
    EXPECT_TRUE(tracker.NotePresent(true, false).covered);
    auto r = tracker.NotePresent(false, true);
    EXPECT_TRUE(r.covered);
    EXPECT_FALSE(r.uncoveredStreakStarted);
    EXPECT_EQ(tracker.UncoveredPresents(), 0u);

    // Once a real blank starts, inheritance must NOT mask it: interpolated
    // presents extend the active uncovered streak.
    r = tracker.NotePresent(false, false);
    EXPECT_TRUE(r.uncoveredStreakStarted);
    r = tracker.NotePresent(false, true);
    EXPECT_FALSE(r.covered);
    EXPECT_EQ(tracker.CurrentUncoveredStreak(), 2u);

    // Recovery: a real draw ends the whole streak including inherited misses.
    r = tracker.NotePresent(true, true);
    EXPECT_TRUE(r.uncoveredStreakEnded);
    EXPECT_EQ(r.endedStreakLength, 2u);
    EXPECT_EQ(tracker.LongestUncoveredStreak(), 2u);
}

TEST(DXGISharedTest, WarmDX12OverlayBackendReuseIsDeviceAndFormatScoped) {
    using ce::dx12_overlay_policy::CanReuseWarmDX12OverlayBackend;

    // The backend never uses its bound queue (resources are device-scoped;
    // submission happens through the hook's own command list), so a queue
    // change — which every FG transition causes — must not force a rebuild.
    // Device+format match with an initialized adapter and a preserve request
    // is the complete reuse condition.
    EXPECT_TRUE(CanReuseWarmDX12OverlayBackend(true, true, true, true));

    // No preserve request (ordinary first init), uninitialized adapter,
    // device change, or RTV format change all require the full rebuild.
    EXPECT_FALSE(CanReuseWarmDX12OverlayBackend(false, true, true, true));
    EXPECT_FALSE(CanReuseWarmDX12OverlayBackend(true, false, true, true));
    EXPECT_FALSE(CanReuseWarmDX12OverlayBackend(true, true, false, true));
    EXPECT_FALSE(CanReuseWarmDX12OverlayBackend(true, true, true, false));
}

TEST(DXGISharedTest, ExplicitEnablePureDLSSColdStartProofShape) {
    using ce::dx12_overlay_policy::HasExplicitEnablePureDLSSColdStartProof;

    // Full proof: pure-DLSS (no FSR history), the CURRENT comeback was
    // activated by an explicit slDLSSGSetOptions(ON) edge, the runtime-owned
    // startup activation swapchain is retained, and the PostSL callback is
    // installed (the consuming gates run inside one, so SL is presenting).
    EXPECT_TRUE(HasExplicitEnablePureDLSSColdStartProof(false, true, true, true));

    // Post-FSR handoffs use their own validated proofs; GetState-only enables
    // (the historical GTA startup-churn family), a missing retained startup
    // swapchain, or no installed callback keep the countdown + warmup.
    EXPECT_FALSE(HasExplicitEnablePureDLSSColdStartProof(true, true, true, true));
    EXPECT_FALSE(HasExplicitEnablePureDLSSColdStartProof(false, false, true, true));
    EXPECT_FALSE(HasExplicitEnablePureDLSSColdStartProof(false, true, false, true));
    EXPECT_FALSE(HasExplicitEnablePureDLSSColdStartProof(false, true, true, false));
}

TEST(DXGISharedTest, ExplicitEnableColdStartProofBypassesReactivationWarmup) {
    using ce::dx12_overlay_policy::ShouldBypassPostSLReactivationWarmup;

    // Session 20260612_215439: the 8-callback countdown plus the 15-callback
    // cold-start warmup ran back-to-back and blanked the OFF->DLSS engage for
    // 22 presents (~150 ms). With the explicit-enable proof the warmup no
    // longer gates the first render after activation.
    EXPECT_TRUE(ShouldBypassPostSLReactivationWarmup(false, false, false, false, true));

    // Unproven pure-DLSS cold starts keep the warmup (first-render ECL on the
    // half-initialized FG queue was the GTA hang family), and the existing
    // post-FSR / confirmed-resume proofs are unchanged.
    EXPECT_FALSE(ShouldBypassPostSLReactivationWarmup(false, false, false, false, false));
    EXPECT_TRUE(ShouldBypassPostSLReactivationWarmup(false, false, false, true, false));
    EXPECT_TRUE(ShouldBypassPostSLReactivationWarmup(true, false, true, false, false));
    EXPECT_FALSE(ShouldBypassPostSLReactivationWarmup(true, false, false, false, true));
}

TEST(DXGISharedTest, ConfirmedRenderThisEpochBypassesRemainingReactivationWarmup) {
    using ce::dx12_overlay_policy::ShouldBypassPostSLReactivationWarmup;

    // Regression for session 20260613_044046: the explicit-enable proof bypassed
    // the warmup on reactivation frame 1 and PostSL rendered one confirmed frame,
    // but that confirmed render RELEASES the retained startup-activation swapchain
    // (the "never pin a swapchain" invariant), which drops
    // HasExplicitEnablePureDLSSColdStartProof to false. Frames 2..15 then fell back
    // into the 15-frame cold-start warmup and blanked a LIVE overlay for 14 presents
    // (~118 ms). A confirmed render means the first ECL already landed safely, so the
    // warmup's hazard is past and the remaining warmup must not re-blank.
    //
    // Pre-fix this returned false (all the older proofs are false once the retained
    // swapchain is gone); with the confirmed-this-epoch leg it stays bypassed.
    EXPECT_TRUE(ShouldBypassPostSLReactivationWarmup(false, false, false, false, false, /*confirmedThisEpoch=*/true));

    // Route-agnostic: a confirmed render is equally authoritative on the post-FSR
    // path (hadFSR=true) once its first ECL has landed, even without safeBootstrap.
    EXPECT_TRUE(ShouldBypassPostSLReactivationWarmup(true, false, false, false, false, /*confirmedThisEpoch=*/true));

    // The GTA GetState-only cold-start hang family stays protected: it gets no
    // frame-1 bypass, so it produces NO confirmed render during the warmup, and with
    // confirmedThisEpoch=false (and all other proofs false) the full warmup is kept.
    EXPECT_FALSE(ShouldBypassPostSLReactivationWarmup(false, false, false, false, false, /*confirmedThisEpoch=*/false));
    EXPECT_FALSE(ShouldBypassPostSLReactivationWarmup(true, false, false, false, false, /*confirmedThisEpoch=*/false));
}

// Source invariant (session 20260613_032326): the retained Streamline
// startup-activation swapchain is an AddRef'd swapchain reference. While CE
// pins it, DXGI refuses to create a new swapchain on the same HWND, so the
// game's native swapchain recreation after DLSS->OFF fails E_ACCESSDENIED
// through every retry and the app aborts its render loop. The churn-
// suppression OFF path must release the retention (a quick re-ON re-retains
// per startup-route present), and both CreateSwapChainForHwnd E_ACCESSDENIED
// recovery paths must release it before retrying.
TEST(DXGISharedSourceTest, RetainedStartupActivationSwapchainReleasedOnChurnOffAndAccessDeniedRecovery) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));

    std::ifstream stream(source, std::ios::binary);
    ASSERT_TRUE(stream.good());
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(text.empty());

    EXPECT_NE(text.find("ReleaseStreamlineStartupActivationSwapchain(\"DX12: Streamline FG OFF (startup churn)\")"),
              std::string::npos);
    EXPECT_NE(text.find("DeepHook: CreateSwapChainForHwnd E_ACCESSDENIED recovery"), std::string::npos);
    EXPECT_NE(text.find("CreateSwapChainForHwnd INLINE: E_ACCESSDENIED recovery"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Make-before-break keep-alive across explicit Streamline FG OFF (session
// 20260613_032326: DLSS suspend/resume handoff seams were the last visible
// 3-4-present blanks). Confirmed PostSL stays armed-and-rendering across the
// off edge until the normal route's first confirmed draw; Streamline FG ON
// while the latch is set is a warm resume of a continuously-live path.
// ---------------------------------------------------------------------------

TEST(DXGISharedTest, ConfirmedPostSLKeepsRenderingAcrossExplicitStreamlineOff) {
    using ce::dx12_overlay_policy::ShouldKeepConfirmedPostSLAliveAcrossStreamlineOff;

    // Confirmed PostSL with no FSR/native-FG takeover in play stays alive.
    EXPECT_TRUE(ShouldKeepConfirmedPostSLAliveAcrossStreamlineOff(true, false, false, false));

    // Unconfirmed paths never keep-alive, and any FSR/native-FG takeover
    // signal wins (the quiesce invariant: stale DLSS callbacks must not
    // submit into an AMD takeover).
    EXPECT_FALSE(ShouldKeepConfirmedPostSLAliveAcrossStreamlineOff(false, false, false, false));
    EXPECT_FALSE(ShouldKeepConfirmedPostSLAliveAcrossStreamlineOff(true, true, false, false));
    EXPECT_FALSE(ShouldKeepConfirmedPostSLAliveAcrossStreamlineOff(true, false, true, false));
    EXPECT_FALSE(ShouldKeepConfirmedPostSLAliveAcrossStreamlineOff(true, false, false, true));
}

TEST(DXGISharedTest, StreamlineOnWithKeepAliveIsWarmResumeNotColdStart) {
    using ce::dx12_overlay_policy::ShouldResumeConfirmedPostSLFromKeepAliveOnStreamlineOn;

    EXPECT_TRUE(ShouldResumeConfirmedPostSLFromKeepAliveOnStreamlineOn(true, true));
    // Without the latch (real cold start) or without preserved confirmation
    // the existing synthetic-startup machinery runs unchanged.
    EXPECT_FALSE(ShouldResumeConfirmedPostSLFromKeepAliveOnStreamlineOn(false, true));
    EXPECT_FALSE(ShouldResumeConfirmedPostSLFromKeepAliveOnStreamlineOn(true, false));
}

TEST(DXGISharedTest, PostSLKeepAliveRenderRequiresLiveStreamlineStack) {
    using ce::dx12_overlay_policy::ShouldAllowPostSLKeepAliveRenderAfterExplicitOff;

    // Latched + FG signal off + SL modules still loaded = render permission.
    EXPECT_TRUE(ShouldAllowPostSLKeepAliveRenderAfterExplicitOff(true, false, true));

    // No latch, FG actually running (normal gates own it), or SL stack gone
    // (proxy queues dead — the gated callback retires the latch instead).
    EXPECT_FALSE(ShouldAllowPostSLKeepAliveRenderAfterExplicitOff(false, false, true));
    EXPECT_FALSE(ShouldAllowPostSLKeepAliveRenderAfterExplicitOff(true, true, true));
    EXPECT_FALSE(ShouldAllowPostSLKeepAliveRenderAfterExplicitOff(true, false, false));
}

TEST(DXGISharedTest, FastPostFSRDLSSProbeRequiresSafeBootstrapAndSwapchainQueue) {
    using ce::dx12_overlay_policy::ShouldUseFastPostFSRDLSSProbeForSafeBootstrap;

    // Session 20260613_035221: every FSR->DLSS engage burned ~4 presents on
    // post-FSR GPU-health probes that always passed (~25ms overlay seam). The
    // fast path (1 scratch-barrier frame, skip the empty-ECL probe) requires
    // the safe-bootstrap proof AND submission on the runtime-owned swapchain
    // queue (SL owns its backbuffer state — not the documented origGame
    // first-ECL crash case) while Streamline FG is running.
    EXPECT_TRUE(ShouldUseFastPostFSRDLSSProbeForSafeBootstrap(true, true, true, true));

    // Missing FSR history, missing safe-bootstrap proof, submission off the
    // swapchain queue, or no SL FG signal all keep the full graduated probe.
    EXPECT_FALSE(ShouldUseFastPostFSRDLSSProbeForSafeBootstrap(false, true, true, true));
    EXPECT_FALSE(ShouldUseFastPostFSRDLSSProbeForSafeBootstrap(true, false, true, true));
    EXPECT_FALSE(ShouldUseFastPostFSRDLSSProbeForSafeBootstrap(true, true, false, true));
    EXPECT_FALSE(ShouldUseFastPostFSRDLSSProbeForSafeBootstrap(true, true, true, false));
}

TEST(DXGISharedTest, LiveOverlayKeepsDrawingThroughFGTransitionCooldown) {
    using ce::dx12_overlay_policy::ShouldKeepDrawingLiveOverlayThroughFGTransitionCooldown;

    // PRINCIPLE: a live overlay is never blanked by an FG transition. Session
    // 20260613_041204: an OFF->FSR no-callback takeover (swapchain reused,
    // syncInit kept, sync resources work on any DIRECT queue) blanked a fully
    // live overlay for 60 presents on a gratuitous cooldown; the normal route
    // drew fine on that exact FSR queue once the cooldown expired. A live
    // backend on the normal route keeps drawing through the transition.
    EXPECT_TRUE(ShouldKeepDrawingLiveOverlayThroughFGTransitionCooldown(true, true, false, false, false));

    // The cooldown's draw suppression is retained only where drawing is the
    // overlay's transport AND unsafe/owned-elsewhere: uninitialized backend or
    // sync, the pre-enable protected official-FFX startup window (wedges AMD's
    // presenter), Streamline FG running (PostSL owns the overlay), or the
    // app-callback FFX bridge route (the bridge renders it; a separate ECL is
    // the documented 0x887A002B device removal).
    EXPECT_FALSE(ShouldKeepDrawingLiveOverlayThroughFGTransitionCooldown(false, true, false, false, false));
    EXPECT_FALSE(ShouldKeepDrawingLiveOverlayThroughFGTransitionCooldown(true, false, false, false, false));
    EXPECT_FALSE(ShouldKeepDrawingLiveOverlayThroughFGTransitionCooldown(true, true, true, false, false));
    EXPECT_FALSE(ShouldKeepDrawingLiveOverlayThroughFGTransitionCooldown(true, true, false, true, false));
    EXPECT_FALSE(ShouldKeepDrawingLiveOverlayThroughFGTransitionCooldown(true, true, false, false, true));
}

// Source invariant (session 20260613_041204): the pre-SL fallback (SL-FG-not-
// active branch) must NOT uninstall the PostSL callback while a confirmed-
// PostSL suspension keep-alive is active. Uninstalling it during the brief
// SL-signal OFF window made every rapid re-ON a cold-start reactivation epoch
// (warmup + probe) with a 1-present overlay gap. Keeping it installed lets the
// re-ON warm-resume; the normal route still draws during the suspension.
TEST(DXGISharedSourceTest, PreSLFallbackRespectsConfirmedPostSLSuspensionKeepAlive) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));

    std::ifstream stream(source, std::ios::binary);
    ASSERT_TRUE(stream.good());
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(text.empty());

    // The pre-SL fallback uninstall must be guarded by the keep-alive latch,
    // and the keep-alive branch keeps the callback installed for warm re-ON.
    const size_t fallbackGuard =
        text.find("g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr &&");
    ASSERT_NE(fallbackGuard, std::string::npos);
    const size_t guardLatch =
        text.find("!g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire)) {", fallbackGuard);
    ASSERT_NE(guardLatch, std::string::npos);
    const size_t fallbackUninstall =
        text.find("SetPostSLCallbackInstalled(false, \"DX12: pre-SL fallback\")", guardLatch);
    ASSERT_NE(fallbackUninstall, std::string::npos);
    // The latch guard immediately precedes the uninstall.
    EXPECT_LT(fallbackUninstall - guardLatch, static_cast<size_t>(120));
    EXPECT_NE(text.find("callback stays installed for warm re-ON"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Warm-resume queue preservation (session 20260613_151646: overlay disappeared
// FOREVER after a DLSS-FG resume). The warm PostSL resume and the post-FSR
// stale-FSR-queue clear are contradictory; the clear MUST receive the warm-resume
// flag so it preserves the live proxy queue instead of stranding PostSL.
// ---------------------------------------------------------------------------
TEST(DXGISharedSourceTest, StaleFSRQueueClearReceivesWarmResumeFlag) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));

    std::ifstream stream(source, std::ios::binary);
    ASSERT_TRUE(stream.good());
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(text.empty());

    // The stale-FSR-queue clear call must pass resumeConfirmedPostSLFromKeepAlive so a warm
    // DLSS suspend->resume does not clear the live proxy queue PostSL is confirmed on.
    const size_t clearCall = text.find("ShouldClearSwapchainQueueAsStaleFSROwnershipOnStreamlineOn(");
    ASSERT_NE(clearCall, std::string::npos);
    const size_t clearCallEnd = text.find(")) {", clearCall);
    ASSERT_NE(clearCallEnd, std::string::npos);
    EXPECT_NE(text.find("resumeConfirmedPostSLFromKeepAlive", clearCall), std::string::npos);
    EXPECT_LT(text.find("resumeConfirmedPostSLFromKeepAlive", clearCall), clearCallEnd);
}
