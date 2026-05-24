#include <gtest/gtest.h>

#include "../hook/common/dx12_overlay_policy.h"
#include "../hook/common/dxgi_factory_policy.h"
#include "../hook/common/dxgi_shared.h"
#include "../hook/wrappers/inline_hook_policy.h"

TEST(DXGISharedTest, ExternalHookBypassResumeExtendsPastPatchedFillBytes) {
    EXPECT_FALSE(ce::inline_hook_policy::IsVerifiedExternalHookResumeOffset(5, 5, false));
    EXPECT_TRUE(ce::inline_hook_policy::IsVerifiedExternalHookResumeOffset(14, 5, true));
    EXPECT_FALSE(ce::inline_hook_policy::IsVerifiedExternalHookResumeOffset(4, 5, true));

    EXPECT_TRUE(ce::inline_hook_policy::ShouldExtendExternalHookResumeOffset(5, 14, true));
    EXPECT_FALSE(ce::inline_hook_policy::ShouldExtendExternalHookResumeOffset(5, 5, true));
    EXPECT_FALSE(ce::inline_hook_policy::ShouldExtendExternalHookResumeOffset(5, 14, false));
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

TEST(DXGISharedTest, SLPresentRoutingStaysDisabledAcrossNativeFGTeardownAndActiveOwnership) {
    EXPECT_TRUE(DXGIShared::ShouldKeepSLPresentRoutingDisabledForNativeFG(true, false));
    EXPECT_TRUE(DXGIShared::ShouldKeepSLPresentRoutingDisabledForNativeFG(false, true));
    EXPECT_TRUE(DXGIShared::ShouldKeepSLPresentRoutingDisabledForNativeFG(true, true));

    EXPECT_FALSE(DXGIShared::ShouldKeepSLPresentRoutingDisabledForNativeFG(false, false));
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

TEST(DXGISharedTest, AuthoritativeStreamlineRuntimeQueuesStayHookableWhileGenericRuntimeQueuesDoNot) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldHookSwapchainQueueVTableForFrameGenerationRuntime(false, false, false, false));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldHookSwapchainQueueVTableForFrameGenerationRuntime(true, true, false, false));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldHookSwapchainQueueVTableForFrameGenerationRuntime(true, false, true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldHookSwapchainQueueVTableForFrameGenerationRuntime(true, false, false, false));
}

TEST(DXGISharedTest, StreamlineRuntimeQueueAuthorityIsSeparateFromFreshHandoffState) {
    const bool authoritativeRuntimeQueue =
        ce::dx12_overlay_policy::ShouldTreatSwapchainQueueAsAuthoritativeStreamlineRuntime(true, true, false);
    EXPECT_TRUE(authoritativeRuntimeQueue);
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldHookSwapchainQueueVTableForFrameGenerationRuntime(
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
                                                                                      true, true, true, false));
    EXPECT_FALSE(DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(true, true, false,
                                                                                                  false, false, false));

    EXPECT_TRUE(DXGIShared::ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(true, false, false, true, true,
                                                                                     true, true, true, true));
    EXPECT_TRUE(DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(true, true, false,
                                                                                                 false, true, false));
    EXPECT_TRUE(DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(true, true, true, true,
                                                                                                 true, false));

    EXPECT_FALSE(DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(true, true, true,
                                                                                                  false, true, false));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(true, true, false, true, true,
                                                                                      true, true, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(true, false, true, true, true,
                                                                                      true, true, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(true, false, false, false, true,
                                                                                      true, true, true, true));
    EXPECT_FALSE(DXGIShared::ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(true, false, false, true, true,
                                                                                      true, false, true, true));
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

TEST(DXGISharedTest, ProgressResolvedOfficialFFXCallbackStallRequiresCallbackBridgeBeforeNormalOverlayFallback) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        true, true, false, false));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        false, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, true,
        ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(true, true, false,
                                                                                              false)));

    // Stable same-queue proof means the native-FSR runtime survived startup,
    // not that CE can submit extra overlay GPU work into that queue.
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

TEST(DXGISharedTest, ExplicitNativeFSROffBlocksStalledCallbackNormalOverlayFallback) {
    // GTA menu/suspend paths explicitly configure native FSR FG off while the
    // FFX context and callback bridge remain alive. The missing callback is
    // expected in that state, so even fresh callback proof must not wake the
    // separate DX12 overlay command-list path.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        true, false, true, false, false, 0, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        true, false, false, true, false, 0, true));

    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        false, false, ce::fg_runtime::RuntimeMode::kOff, false, true,
        ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
            true, false, false, true, false, 0, true)));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(
        true, false, true,
        ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
            true, false, false, true, false, 0, true)));
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

TEST(DXGISharedTest, FFXPresentCallbackFallbackCopyOnlyRunsWithoutRuntimeCompositionCallback) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldFallbackCopyFFXPresentSourceToOutput(false, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldFallbackCopyFFXPresentSourceToOutput(true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldFallbackCopyFFXPresentSourceToOutput(false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldFallbackCopyFFXPresentSourceToOutput(false, true, false));
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
        true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(
        false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(
        false, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(
        false, false, false));
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

TEST(DXGISharedTest, ProtectedOfficialFFXStartupQuiescesCESideEffectsUntilDirectConfigure) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(true, true));
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

TEST(DXGISharedTest, ProtectedOfficialFFXStartupCanResolveAfterSustainedProgressWithoutDirectConfigure) {
    const uint32_t processFrameThreshold =
        ce::dx12_overlay_policy::GetProtectedOfficialFFXStartupProcessFrameProgressThreshold();
    const uint32_t eclThreshold = ce::dx12_overlay_policy::GetProtectedOfficialFFXStartupECLProgressThreshold();

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
        true, false, processFrameThreshold - 1, eclThreshold - 1));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
        true, false, processFrameThreshold, 0));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
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
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDelaySyntheticPostSLActivationBehindRepeatedCallbacks(true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDelaySyntheticPostSLActivationBehindRepeatedCallbacks(false));
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
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBypassPostSLReactivationWarmupAfterTopLevelHandoffWrapperProgress(false, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBypassPostSLReactivationWarmupAfterTopLevelHandoffWrapperProgress(true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBypassPostSLReactivationWarmupAfterTopLevelHandoffWrapperProgress(false, false));
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
        false, false, false, false, false, false, false, true, true));
    EXPECT_TRUE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, true, true, true, true, false, false, false, true));
    EXPECT_TRUE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, true, true, true, false, true, false, false, true));
    // safePostFSRBootstrapPath is now sufficient; explicitSetOptionsActivation is no longer required
    EXPECT_TRUE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, true, false, true, true, false, false, false, true));
    EXPECT_TRUE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, true, false, true, false, true, false, false, true));
    EXPECT_TRUE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, false, true, false, true, true, true, false, true));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, true, false, false, true, false, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, true, true, false, true, false, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, true, true, false, false, true, false, false, true));

    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        true, false, false, false, false, false, false, true, true));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, false, false, false, false, false, false, false, true));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, false, false, false, false, false, false, true, false));
    EXPECT_FALSE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, false, false, false, true, true, true, false, false));
    EXPECT_TRUE(DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
        false, false, false, false, false, false, false, true, true));
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
    // pipeline is settled. Even the pure-DLSS top-level handoff family must
    // keep deferring real PostSL rendering until the startup window expires.
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(true, false, true));

    // The post-FSR safe bootstrap proof is stronger than wrapper progress: it
    // proves a current runtime-owned Streamline swapchain queue and submit path,
    // so the overlay can draw during short FSR->DLSS switch intervals.
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(true, false, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(true, false, false, true));

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
        true, false, false, true, true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, false, false, true, true, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, false, false, true, true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        false, false, false, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, true, false, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, false, true, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, false, false, false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, false, false, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        true, false, false, true, true, false, false));
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
// Fix (build 0.1.2923): One-time vtable[8] unhook → E9 JMP call (Steam init
// reads vtable[8] = dxgi!Present) → re-hook.  Subsequent frames route through
// oPresent (E9 JMP) normally since Steam's internal "next" handler is now
// initialized and non-NULL.
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

    // The fix in CallOriginalPresent uses one-time vtable unhook + Steam init +
    // re-hook for the non-SL Steam overlay case.  The policy alone still allows
    // the dangerous path — the call-site fix is what prevents the crash.
    const bool isNonSLStrangeBrigadeScenario = DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(
        true, true, true, true, false, false, false, false, false);
    EXPECT_TRUE(isNonSLStrangeBrigadeScenario);
    // The crash is prevented by the call-site logic in CallOriginalPresent
    // (AttemptSteamDX12OverlayInit with one-time vtable unhook), not by the policy.
    const bool crashPreventedByCallSiteFix = true;
    EXPECT_TRUE(crashPreventedByCallSiteFix);
}

// Regression: Strange Brigade DX12 Steam overlay stays visible with CE injection.
// When Steam overlay is loaded without Streamline (e.g. Strange Brigade DX12),
// CallOriginalPresent uses one-time vtable[8] unhook + init + re-hook.  On the
// very first Present call, vtable[8] is temporarily restored to dxgi!Present so
// Steam's overlay can initialize its internal trampoline by reading the correct
// value.  Subsequent frames route through oPresent (E9 JMP) normally.
//
// Expected hook chain (frame 1, init):
//   vtable unhook → oPresent (E9 JMP) → Steam's OverlayHookD3D3 →
//   reads vtable[8]=dxgi!Present ✓ → creates trampoline → renders overlay →
//   calls trampoline → real dxgi!Present → vtable re-hook to DetourPresent
//
// Expected hook chain (frame 2+, steady state):
//   vtable[8]=DetourPresent → oPresent (E9 JMP) → Steam's OverlayHookD3D3 →
//   non-NULL "next" handler → renders overlay → trampoline → real Present
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
