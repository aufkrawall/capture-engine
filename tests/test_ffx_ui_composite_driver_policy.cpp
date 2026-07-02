#include <gtest/gtest.h>

#include "../hook/common/dx12_overlay_policy.h"

namespace {

using ce::dx12_overlay_policy::ChooseFFXUiCompositeDriver;
using ce::dx12_overlay_policy::FFXUiCompositeDriver;
using ce::dx12_overlay_policy::MayReassertSubstituteUiResource;
using ce::dx12_overlay_policy::ShouldInstallFFXProxyPresentHook;

// Regression for session 20260701_213656: GTA froze PERMANENTLY on the first FSR-FG frame because CE
// re-asserted the substitute UI resource (ffxConfigure/RegisterUiResource, which takes AMD's
// FrameInterpolationSwapchain criticalSection) from DetourPresent on AMD's PRESENTER thread, while AMD's
// Present held that criticalSection on the GAME thread spin-waiting (no timeout) on compositionFenceCPU —
// a fence only the presenter thread can advance. The re-assert must be forbidden for the real-present
// (presenter-thread) fallback driver in every case.
TEST(FFXUiCompositeDriverPolicyTest, ReassertForbiddenFromRealPresentFallbackDriver) {
    EXPECT_FALSE(MayReassertSubstituteUiResource(FFXUiCompositeDriver::kRealPresentFallback));
}

// The proxy-present prework runs on the game thread BEFORE AMD's Present enters its criticalSection — the
// same thread and lock order as the game's own per-frame RegisterUiResource — so the re-assert is legal
// there (and only there).
TEST(FFXUiCompositeDriverPolicyTest, ReassertAllowedOnlyFromProxyPresentPrework) {
    EXPECT_TRUE(MayReassertSubstituteUiResource(FFXUiCompositeDriver::kProxyPresentPrework));
}

TEST(FFXUiCompositeDriverPolicyTest, DriverFollowsProxyHookLiveness) {
    EXPECT_EQ(ChooseFFXUiCompositeDriver(true), FFXUiCompositeDriver::kProxyPresentPrework);
    EXPECT_EQ(ChooseFFXUiCompositeDriver(false), FFXUiCompositeDriver::kRealPresentFallback);
}

// The proxy hook may only patch a Present entry that resolves into the FFX runtime module. A real DXGI
// swapchain (dxgi.dll) or an sl_interposer/CE-wrapped chain is NOT the game-facing FFX proxy — hooking one
// of those would run the prework on the wrong thread (AMD's presenter), recreating the deadlock the proxy
// driver exists to prevent.
TEST(FFXProxyPresentHookInstallPolicyTest, InstallsOnlyForFFXRuntimeModulePresentEntry) {
    EXPECT_TRUE(ShouldInstallFFXProxyPresentHook(/*presentEntryInFFXRuntimeModule=*/true,
                                                 /*presentEntryIsCEDetour=*/false,
                                                 /*alreadyInstalledOnThisVtableEntry=*/false));
    EXPECT_FALSE(ShouldInstallFFXProxyPresentHook(false, false, false));
}

TEST(FFXProxyPresentHookInstallPolicyTest, NeverStacksCEDetours) {
    EXPECT_FALSE(ShouldInstallFFXProxyPresentHook(true, true, false));
    EXPECT_FALSE(ShouldInstallFFXProxyPresentHook(false, true, false));
}

TEST(FFXProxyPresentHookInstallPolicyTest, IdempotentWhenAlreadyInstalledOnThisVtableEntry) {
    EXPECT_FALSE(ShouldInstallFFXProxyPresentHook(true, false, true));
    EXPECT_FALSE(ShouldInstallFFXProxyPresentHook(false, false, true));
}

// The route selection itself is unchanged by the driver split: under active runtime-owned no-callback FSR
// FG (not suspended, live queue is AMD's separate FG queue) the composite route must still be chosen — the
// driver split only decides WHERE the composite runs, never WHETHER it runs. Guards the shared route
// decision both drivers now consult (DX12_RunFFXProxyPrePresentWork and DetourPresent must never disagree).
TEST(FFXUiCompositeDriverPolicyTest, ActiveInterpolationStillChoosesCompositeRouteForBothDrivers) {
    const auto route = ce::dx12_overlay_policy::ChooseNoCallbackFSRFGOverlayRoute(
        /*runtimeOwnsSwapchain=*/true, /*liveSwapchainQueueIsOriginalGameQueue=*/false,
        /*fsrFGDisabledSuspendPending=*/false, /*uiResourceCachedForBundle=*/true,
        /*bundleOverlayActivelyFiring=*/false);
    EXPECT_EQ(route, ce::dx12_overlay_policy::NoCallbackFSRFGOverlayRoute::kSkipBundleCovers);
}

}  // namespace
