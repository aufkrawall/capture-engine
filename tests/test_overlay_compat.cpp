// Regression tests for third-party-overlay detection (hook/common/overlay_compat.h).
//
// Root cause these guard against: the Present hot path used to re-walk the Windows loader
// (GetModuleHandleA over the overlay list) whenever ANY DLL loaded, because the cache was
// invalidated unconditionally. During the x86 Alt+Tab borderless-fullscreen mode switch the
// system reloads d3d11.dll repeatedly, so the next Present re-walked the loader for NOT-loaded
// names (socialclub.dll, ...). That loader walk stalled Present >2 s and tripped the GPU TDR,
// killing the overlay. Detection is now loader-free on the hot path: it reads an atomic that
// is maintained off-thread via DLL load/unload notifications, and only overlay-relevant
// modules affect it. These tests pin that behavior with pure, deterministic logic.

#include <gtest/gtest.h>

#include <cstring>

#include "../hook/common/overlay_compat.h"

namespace {

using namespace ce::overlay_compat;

// Mirrors the downstream Steam classifier (dxgi_shared.cpp IsSteamOverlayModule /
// fg_session_state.cpp), so we prove the canonical strings still satisfy it.
bool IsSteamName(const char* name) {
    return name && std::strstr(name, "gameoverlayrenderer") != nullptr;
}

class OverlayModuleDetectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        ResetThirdPartyOverlayModuleCacheForTesting();
    }
    void TearDown() override {
        ResetThirdPartyOverlayModuleCacheForTesting();
    }
};

// --- Pure matcher: the key regression is that ordinary system DLLs do NOT match, so a load
// of e.g. d3d11.dll can never drive a Present-thread re-walk. ---
TEST_F(OverlayModuleDetectionTest, MatcherRejectsNonOverlaySystemModules) {
    EXPECT_EQ(MatchKnownThirdPartyOverlayModuleIndex("d3d11.dll"), -1);
    EXPECT_EQ(MatchKnownThirdPartyOverlayModuleIndex("dxgi.dll"), -1);
    EXPECT_EQ(MatchKnownThirdPartyOverlayModuleIndex("kernel32.dll"), -1);
    EXPECT_EQ(MatchKnownThirdPartyOverlayModuleIndex("d3d12.dll"), -1);
    EXPECT_EQ(MatchKnownThirdPartyOverlayModuleIndex(""), -1);
    EXPECT_EQ(MatchKnownThirdPartyOverlayModuleIndex(nullptr), -1);

    EXPECT_EQ(MatchKnownThirdPartyOverlayModuleBaseName("d3d11.dll"), nullptr);
    EXPECT_EQ(MatchKnownThirdPartyOverlayModuleBaseName("dxgi.dll"), nullptr);
}

TEST_F(OverlayModuleDetectionTest, MatcherAcceptsKnownOverlayModulesCaseInsensitively) {
    EXPECT_NE(MatchKnownThirdPartyOverlayModuleBaseName("socialclub.dll"), nullptr);
    EXPECT_NE(MatchKnownThirdPartyOverlayModuleBaseName("gameoverlayrenderer64.dll"), nullptr);
    EXPECT_NE(MatchKnownThirdPartyOverlayModuleBaseName("RTSSHooks64.dll"), nullptr);
    EXPECT_NE(MatchKnownThirdPartyOverlayModuleBaseName("EOSOVH_Win64_Shipping.dll"), nullptr);
    EXPECT_NE(MatchKnownThirdPartyOverlayModuleBaseName("ReShade64.dll"), nullptr);
    EXPECT_NE(MatchKnownThirdPartyOverlayModuleBaseName("SpecialK64.dll"), nullptr);
    EXPECT_NE(MatchKnownThirdPartyOverlayModuleBaseName("OptiScaler.dll"), nullptr);

    // Case-insensitive.
    EXPECT_NE(MatchKnownThirdPartyOverlayModuleBaseName("SocialClub.DLL"), nullptr);
    EXPECT_NE(MatchKnownThirdPartyOverlayModuleBaseName("GAMEOVERLAYRENDERER64.DLL"), nullptr);
    EXPECT_NE(MatchKnownThirdPartyOverlayModuleBaseName("rtsshooks64.dll"), nullptr);
}

TEST_F(OverlayModuleDetectionTest, IdentifiedProxyPathIsRecognizedWithoutMatchingItsGenericFilename) {
    const char* proxyPath = "C:\\Games\\Example\\dxgi.dll";
    EXPECT_FALSE(IsThirdPartyOverlayModulePath(proxyPath));
    EXPECT_TRUE(PublishIdentifiedThirdPartyOverlayModulePath(proxyPath));
    EXPECT_TRUE(IsThirdPartyOverlayModulePath(proxyPath));
    EXPECT_TRUE(IsThirdPartyOverlayModulePath(L"c:\\games\\example\\DXGI.DLL"));
    EXPECT_FALSE(IsThirdPartyOverlayModulePath("C:\\OtherGame\\dxgi.dll"));
}

TEST_F(OverlayModuleDetectionTest, IdentifiedProxySupportsWideNonAsciiInstallPaths) {
    const wchar_t* proxyPath = L"C:\\Spiele\\\u00D6ffentlich\\dxgi.dll";
    EXPECT_FALSE(IsThirdPartyOverlayModulePath(proxyPath));
    EXPECT_TRUE(PublishIdentifiedThirdPartyOverlayModulePath(proxyPath));
    EXPECT_TRUE(IsThirdPartyOverlayModulePath(proxyPath));
    EXPECT_FALSE(IsThirdPartyOverlayModulePath(L"C:\\Spiele\\Andere\\dxgi.dll"));
}

TEST_F(OverlayModuleDetectionTest, ProxyIdentityRefreshPublishesAtomically) {
    const char* oldProxy = "C:\\Games\\Old\\dxgi.dll";
    const char* newProxy = "C:\\Games\\New\\d3d11.dll";
    ASSERT_TRUE(PublishIdentifiedThirdPartyOverlayModulePath(oldProxy));

    const uint32_t refreshBank = BeginIdentifiedThirdPartyOverlayModulePathRefresh();
    ASSERT_TRUE(PublishIdentifiedThirdPartyOverlayModulePathToBank(newProxy, refreshBank));
    EXPECT_TRUE(IsThirdPartyOverlayModulePath(oldProxy));
    EXPECT_FALSE(IsThirdPartyOverlayModulePath(newProxy));

    CommitIdentifiedThirdPartyOverlayModulePathRefresh(refreshBank);
    EXPECT_FALSE(IsThirdPartyOverlayModulePath(oldProxy));
    EXPECT_TRUE(IsThirdPartyOverlayModulePath(newProxy));
}

TEST_F(OverlayModuleDetectionTest, ExportIdentifiedProxyParticipatesInLoaderFreeOverlayState) {
    EXPECT_EQ(GetLoadedThirdPartyOverlayModuleName(), nullptr);
    SetIdentifiedOverlayIdentityLoaded("CE.ReShadeProxyIdentity", true);
    ASSERT_NE(GetLoadedThirdPartyOverlayModuleName(), nullptr);
    EXPECT_NE(std::strstr(GetLoadedThirdPartyOverlayModuleName(), "ReShade"), nullptr);
    SetIdentifiedOverlayIdentityLoaded("CE.ReShadeProxyIdentity", false);
    EXPECT_EQ(GetLoadedThirdPartyOverlayModuleName(), nullptr);
}

TEST_F(OverlayModuleDetectionTest, CanonicalNamePreservesSteamClassifier) {
    const char* steam = MatchKnownThirdPartyOverlayModuleBaseName("gameoverlayrenderer64.dll");
    ASSERT_NE(steam, nullptr);
    EXPECT_TRUE(IsSteamName(steam));

    // A non-Steam overlay must NOT classify as Steam.
    const char* social = MatchKnownThirdPartyOverlayModuleBaseName("socialclub.dll");
    ASSERT_NE(social, nullptr);
    EXPECT_FALSE(IsSteamName(social));
}

TEST_F(OverlayModuleDetectionTest, ExtractBaseNameHandlesWindowsAndUnixSeparators) {
    EXPECT_STREQ(detail::ExtractBaseName("C:\\Program Files\\Steam\\gameoverlayrenderer64.dll"),
                 "gameoverlayrenderer64.dll");
    EXPECT_STREQ(detail::ExtractBaseName("/some/unix/path/socialclub.dll"), "socialclub.dll");
    EXPECT_STREQ(detail::ExtractBaseName("socialclub.dll"), "socialclub.dll");
}

// --- Cache lifecycle (loader-free helpers). ---

// THE key regression: a d3d11.dll load notification must NOT set/disturb detection state.
TEST_F(OverlayModuleDetectionTest, NonOverlayLoadDoesNotAffectDetection) {
    EXPECT_EQ(GetLoadedThirdPartyOverlayModuleName(), nullptr);

    EXPECT_EQ(NoteModuleLoadedForOverlayCache("C:\\Windows\\System32\\d3d11.dll"), nullptr);
    EXPECT_EQ(NoteModuleLoadedForOverlayCache("dxgi.dll"), nullptr);
    EXPECT_EQ(NoteModuleLoadedForOverlayCache("kernel32.dll"), nullptr);

    // Still no overlay detected — nothing could have triggered a Present-thread re-walk.
    EXPECT_EQ(GetLoadedThirdPartyOverlayModuleName(), nullptr);
}

TEST_F(OverlayModuleDetectionTest, OverlayLoadThenUnloadUpdatesDetection) {
    EXPECT_EQ(GetLoadedThirdPartyOverlayModuleName(), nullptr);

    const char* loaded = NoteModuleLoadedForOverlayCache("C:\\x\\gameoverlayrenderer64.dll");
    ASSERT_NE(loaded, nullptr);
    const char* active = GetLoadedThirdPartyOverlayModuleName();
    ASSERT_NE(active, nullptr);
    EXPECT_TRUE(IsSteamName(active));

    // Idempotent: hot-path read is stable and side-effect-free.
    EXPECT_EQ(GetLoadedThirdPartyOverlayModuleName(), active);

    NoteModuleUnloadedForOverlayCache("gameoverlayrenderer64.dll");
    EXPECT_EQ(GetLoadedThirdPartyOverlayModuleName(), nullptr);
}

TEST_F(OverlayModuleDetectionTest, NonMatchingUnloadIsNoOp) {
    NoteModuleLoadedForOverlayCache("socialclub.dll");
    const char* active = GetLoadedThirdPartyOverlayModuleName();
    ASSERT_NE(active, nullptr);

    // Unloading an unrelated/system module must not clear detection.
    EXPECT_EQ(NoteModuleUnloadedForOverlayCache("d3d11.dll"), nullptr);
    EXPECT_EQ(GetLoadedThirdPartyOverlayModuleName(), active);
}

// List-order priority: when several overlays are loaded, Steam (earliest in the list) is
// reported, matching the previous in-order GetModuleHandleA walk regardless of load order.
TEST_F(OverlayModuleDetectionTest, ListOrderPriorityIsIndependentOfLoadOrder) {
    NoteModuleLoadedForOverlayCache("RTSSHooks64.dll");            // lower priority, loaded first
    NoteModuleLoadedForOverlayCache("gameoverlayrenderer64.dll");  // Steam, higher priority
    EXPECT_TRUE(IsSteamName(GetLoadedThirdPartyOverlayModuleName()));

    // Steam unloads -> the still-loaded RTSS becomes the reported overlay.
    NoteModuleUnloadedForOverlayCache("gameoverlayrenderer64.dll");
    const char* after = GetLoadedThirdPartyOverlayModuleName();
    ASSERT_NE(after, nullptr);
    EXPECT_FALSE(IsSteamName(after));
    EXPECT_NE(std::strstr(after, "RTSSHooks"), nullptr);
}

// --- Startup-blocking subsets must also be loader-free + correct (they're on per-present
// paths too: ShouldDelegateDX12PresentToDetourHook etc.). This is the regression the
// 20260606_021018 freeze exposed: GetStartupBlockingOverlayModuleName still walked the loader. ---
TEST_F(OverlayModuleDetectionTest, StartupBlockingSubsetsAreCorrect) {
    EXPECT_EQ(GetStartupBlockingOverlayModuleName(), nullptr);
    EXPECT_EQ(GetStartupBlockingOverlayRenderModuleName(), nullptr);

    // A pure overlay (Steam) is NOT a startup-blocking module.
    NoteModuleLoadedForOverlayCache("gameoverlayrenderer64.dll");
    EXPECT_TRUE(IsSteamName(GetLoadedThirdPartyOverlayModuleName()));
    EXPECT_EQ(GetStartupBlockingOverlayModuleName(), nullptr);
    EXPECT_EQ(GetStartupBlockingOverlayRenderModuleName(), nullptr);

    // Social Club is overlay + startup-blocking (but not a render module).
    NoteModuleLoadedForOverlayCache("socialclub.dll");
    ASSERT_NE(GetStartupBlockingOverlayModuleName(), nullptr);
    EXPECT_NE(std::strstr(GetStartupBlockingOverlayModuleName(), "socialclub"), nullptr);
    EXPECT_EQ(GetStartupBlockingOverlayRenderModuleName(), nullptr);
}

// SocialClubD3D12Renderer is render-only: it must drive the render subset but must NOT be
// reported as a general third-party overlay (preserves the old GetLoadedThirdPartyOverlayModuleName
// behavior, which never listed it).
TEST_F(OverlayModuleDetectionTest, SocialClubRendererIsRenderOnlyNotOverlay) {
    NoteModuleLoadedForOverlayCache("SocialClubD3D12Renderer.dll");
    ASSERT_NE(GetStartupBlockingOverlayRenderModuleName(), nullptr);
    EXPECT_NE(std::strstr(GetStartupBlockingOverlayRenderModuleName(), "SocialClubD3D12Renderer"), nullptr);

    EXPECT_EQ(GetLoadedThirdPartyOverlayModuleName(), nullptr);
    EXPECT_FALSE(IsThirdPartyOverlayLoaded());
    EXPECT_EQ(GetStartupBlockingOverlayModuleName(), nullptr);
}

// Counting the overlay subset decides whether CE may patch the shared dxgi!Present entry.
// Render-only and non-overlay tracked modules (SocialClubD3D12Renderer, sl.interposer) must
// not inflate that count, or CE would drop its entry hook in single-overlay games.
TEST_F(OverlayModuleDetectionTest, LoadedOverlayCountIgnoresNonOverlayTrackedModules) {
    EXPECT_EQ(CountLoadedTrackedOverlayModules(TrackedOverlaySubset::kOverlay), 0u);

    NoteModuleLoadedForOverlayCache("gameoverlayrenderer64.dll");
    EXPECT_EQ(CountLoadedTrackedOverlayModules(TrackedOverlaySubset::kOverlay), 1u);

    // Neither the Streamline interposer nor the render-only Social Club module is an overlay.
    NoteModuleLoadedForOverlayCache("sl.interposer.dll");
    NoteModuleLoadedForOverlayCache("SocialClubD3D12Renderer.dll");
    EXPECT_EQ(CountLoadedTrackedOverlayModules(TrackedOverlaySubset::kOverlay), 1u);

    NoteModuleLoadedForOverlayCache("RTSSHooks64.dll");
    EXPECT_EQ(CountLoadedTrackedOverlayModules(TrackedOverlaySubset::kOverlay), 2u);

    NoteModuleUnloadedForOverlayCache("RTSSHooks64.dll");
    EXPECT_EQ(CountLoadedTrackedOverlayModules(TrackedOverlaySubset::kOverlay), 1u);
}

// CE never joins a foreign dxgi!Present entry chain — it intercepts below it. Two reasons:
// chain integrity with two or more overlays (Strange Brigade DX12 + Steam + RTSS, sessions
// 20260812_002958 / _005530 / _010529: whichever tool re-hooks while CE's prepend is live
// records CE as its "next" and the other overlay falls out of the chain), and draw order with
// any number of them (everyone in that chain composites before forwarding, so CE's prepend made
// CE the bottom layer and Steam's fullscreen overlay drew over it, build 0.1.5959).
TEST(OverlayPresentEntryChainPolicyTest, CEStaysOutOfForeignPresentEntryChains) {
    // Steam only -> CE goes below it so its own overlay composites last.
    EXPECT_TRUE(ShouldLeavePresentEntryToForeignOverlayChain(1));
    // Steam + RTSS -> CE stays out (chain integrity as well as draw order).
    EXPECT_TRUE(ShouldLeavePresentEntryToForeignOverlayChain(2));
    EXPECT_TRUE(ShouldLeavePresentEntryToForeignOverlayChain(3));

    // No overlay module loaded: CE owns the entry outright, nothing to stay out of or draw after.
    EXPECT_FALSE(ShouldLeavePresentEntryToForeignOverlayChain(0));
}

// Ownership follows the loaded MODULE, never a live sample of the entry bytes. Steam hooks
// Present when the game's first swapchain appears, seconds after CE's guarded temp swapchain has
// already installed CE's Present hooks; RTSS restores and re-patches those bytes around every
// call. Sampling therefore decides only who was first — and CE being first is what breaks the
// other overlay (Cyberpunk + Steam, 20260816_154722: CE prepended with foreignJumpVisibleNow=0
// and the Steam overlay never worked again in that session).
TEST(OverlayPresentEntryChainPolicyTest, EntryOwnershipDoesNotDependOnAVisiblePatchRightNow) {
    // A loaded overlay that has not patched Present yet still owns that entry.
    EXPECT_TRUE(ShouldLeavePresentEntryToForeignOverlayChain(1));
    // ...and CE may still take an entry nobody is going to contest.
    EXPECT_FALSE(ShouldLeavePresentEntryToForeignOverlayChain(0));
}

TEST(OverlayPresentEntryChainPolicyTest, DeepForeignOverlayViewPreservesDX12SwapchainIdentity) {
    EXPECT_TRUE(ShouldPreserveDX12SwapchainIdentityBelowForeignPresentChain(true, true, 1));
    EXPECT_TRUE(ShouldPreserveDX12SwapchainIdentityBelowForeignPresentChain(true, true, 2));
    EXPECT_TRUE(ShouldPreserveDX12SwapchainIdentityBelowForeignPresentChain(true, true, 3));

    EXPECT_FALSE(ShouldPreserveDX12SwapchainIdentityBelowForeignPresentChain(false, true, 2));
    EXPECT_FALSE(ShouldPreserveDX12SwapchainIdentityBelowForeignPresentChain(true, false, 2));
    EXPECT_FALSE(ShouldPreserveDX12SwapchainIdentityBelowForeignPresentChain(true, true, 0));
}

TEST(OverlayPresentEntryChainPolicyTest, InvisibleDX12CreateWithForeignOverlayPreservesIdentity) {
    EXPECT_TRUE(ShouldPreserveInvisibleDX12SwapchainIdentityWithForeignOverlay(true, true, false, 1));
    EXPECT_TRUE(ShouldPreserveInvisibleDX12SwapchainIdentityWithForeignOverlay(true, true, false, 2));

    EXPECT_FALSE(ShouldPreserveInvisibleDX12SwapchainIdentityWithForeignOverlay(false, true, false, 1));
    EXPECT_FALSE(ShouldPreserveInvisibleDX12SwapchainIdentityWithForeignOverlay(true, false, false, 1));
    EXPECT_FALSE(ShouldPreserveInvisibleDX12SwapchainIdentityWithForeignOverlay(true, true, true, 1));
    EXPECT_FALSE(ShouldPreserveInvisibleDX12SwapchainIdentityWithForeignOverlay(true, true, false, 0));
}

// The below-the-chain body patch can be refused (thread quiescence, unrecognized prolog). The
// fallback is allowed only against a single foreign overlay, which composes with a prepend;
// with two or more the prepend is exactly what corrupts their saved chains, so that case must
// wait for a later retry rather than fall back.
TEST(OverlayPresentEntryChainPolicyTest, PrependFallbackOnlyAgainstASingleForeignOverlay) {
    EXPECT_TRUE(MayPrependPresentEntryWhenBelowChainViewUnavailable(0));
    EXPECT_TRUE(MayPrependPresentEntryWhenBelowChainViewUnavailable(1));
    EXPECT_FALSE(MayPrependPresentEntryWhenBelowChainViewUnavailable(2));
    EXPECT_FALSE(MayPrependPresentEntryWhenBelowChainViewUnavailable(3));
}

// After CE wraps the FG runtime's swapchain (non-entry view established), it may remove its
// Present-entry prepend ONLY while it still owns the entry bytes. A foreign re-hook that took
// the entry recorded CE's relay inside its own saved chain; un-prepending then cannot repair
// that state and CE must not touch bytes it no longer owns.
TEST(OverlayPresentEntryChainPolicyTest, LeaveEntryAfterRuntimeWrapRequiresOwnedEntryBytes) {
    // Foreign overlay loaded + entry still CE's -> leave.
    EXPECT_TRUE(ShouldLeavePresentEntryAfterRuntimeSwapchainWrap(
        /*entryPatchStillIntact=*/true, /*loadedOverlayModuleCount=*/2, /*alreadyLeftToForeignChain=*/false));
    EXPECT_TRUE(ShouldLeavePresentEntryAfterRuntimeSwapchainWrap(
        /*entryPatchStillIntact=*/true, /*loadedOverlayModuleCount=*/1, /*alreadyLeftToForeignChain=*/false));
    // Foreign re-hook already displaced CE -> never un-prepend.
    EXPECT_FALSE(ShouldLeavePresentEntryAfterRuntimeSwapchainWrap(
        /*entryPatchStillIntact=*/false, /*loadedOverlayModuleCount=*/2, /*alreadyLeftToForeignChain=*/false));
    // Already in leave-entry mode -> nothing to transition.
    EXPECT_FALSE(ShouldLeavePresentEntryAfterRuntimeSwapchainWrap(
        /*entryPatchStillIntact=*/true, /*loadedOverlayModuleCount=*/2, /*alreadyLeftToForeignChain=*/true));
    // No overlay module loaded -> CE owns the entry outright.
    EXPECT_FALSE(ShouldLeavePresentEntryAfterRuntimeSwapchainWrap(
        /*entryPatchStillIntact=*/true, /*loadedOverlayModuleCount=*/0, /*alreadyLeftToForeignChain=*/false));
}

TEST_F(OverlayModuleDetectionTest, IsThirdPartyOverlayLoadedTracksOverlaySubset) {
    EXPECT_FALSE(IsThirdPartyOverlayLoaded());
    NoteModuleLoadedForOverlayCache("RTSSHooks64.dll");
    EXPECT_TRUE(IsThirdPartyOverlayLoaded());
    NoteModuleUnloadedForOverlayCache("RTSSHooks64.dll");
    EXPECT_FALSE(IsThirdPartyOverlayLoaded());
}

// The last-loaded overlay (recorded from real load notifications only) decides
// which overlay owns a foreign Present entry jump: the later hooker displaces the
// earlier one's entry bytes. Steam wins the list-priority name cache even when
// RTSS loaded after it and owns the chain (Strange Brigade + Steam + RTSS,
// session 20260811_233748), so the load-order evidence must be tracked separately
// from the loaded-set and must not be polluted by the refresh walk.
TEST_F(OverlayModuleDetectionTest, LastLoadedOverlayIsTrackedOnlyFromNotifications) {
    EXPECT_EQ(GetLastLoadedTrackedOverlayModuleName(), nullptr);

    // The plain cache update (seed walk / identity-refresh enumeration) must NOT
    // record load order.
    NoteModuleLoadedForOverlayCache("gameoverlayrenderer64.dll");
    EXPECT_EQ(GetLastLoadedTrackedOverlayModuleName(), nullptr);

    // Real load notification records the most recent tracked overlay.
    NoteModuleLoadedForOverlayCacheFromNotification("RTSSHooks64.dll");
    ASSERT_NE(GetLastLoadedTrackedOverlayModuleName(), nullptr);
    EXPECT_NE(std::strstr(GetLastLoadedTrackedOverlayModuleName(), "RTSSHooks"), nullptr);

    // A later Steam load replaces RTSS as the newest overlay.
    NoteModuleLoadedForOverlayCacheFromNotification("gameoverlayrenderer64.dll");
    EXPECT_TRUE(IsSteamName(GetLastLoadedTrackedOverlayModuleName()));

    // Non-overlay loads must not disturb the last-loaded record.
    NoteModuleLoadedForOverlayCacheFromNotification("d3d11.dll");
    EXPECT_TRUE(IsSteamName(GetLastLoadedTrackedOverlayModuleName()));
}

// Pure decision for unresolvable foreign Present chains: RTSS as the most recently
// loaded overlay owns the chain and must NOT be serviced as a Steam chain even
// though Steam is loaded and wins the list-priority name cache.
TEST_F(OverlayModuleDetectionTest, UnresolvableChainOwnerFollowsLoadOrderEvidence) {
    // RTSS loaded last (Strange Brigade + Steam + RTSS): the chain is RTSS's.
    EXPECT_FALSE(IsSteamExternalChainOwnerByLoadOrderEvidence("RTSSHooks64.dll", "gameoverlayrenderer64.dll"));
    EXPECT_FALSE(IsSteamExternalChainOwnerByLoadOrderEvidence("RTSSHooks64.dll", "RTSSHooks64.dll"));

    // Steam loaded last (or no load-order evidence at all): Steam owns the chain.
    EXPECT_TRUE(IsSteamExternalChainOwnerByLoadOrderEvidence("gameoverlayrenderer64.dll", "gameoverlayrenderer64.dll"));
    EXPECT_TRUE(IsSteamExternalChainOwnerByLoadOrderEvidence(nullptr, "gameoverlayrenderer64.dll"));

    // No Steam anywhere: never a Steam chain.
    EXPECT_FALSE(IsSteamExternalChainOwnerByLoadOrderEvidence(nullptr, "RTSSHooks64.dll"));
    EXPECT_FALSE(IsSteamExternalChainOwnerByLoadOrderEvidence(nullptr, nullptr));
    EXPECT_FALSE(IsSteamExternalChainOwnerByLoadOrderEvidence("RTSSHooks64.dll", nullptr));
}

// sl.interposer is tracked for a loader-free SL-loaded check (replaces a per-Present
// GetModuleHandleA), but it is NOT an overlay/startup-blocking module.
TEST_F(OverlayModuleDetectionTest, StreamlineInterposerTrackedButNotOverlay) {
    EXPECT_FALSE(IsStreamlineInterposerModuleLoaded());

    NoteModuleLoadedForOverlayCache("sl.interposer.dll");
    EXPECT_TRUE(IsStreamlineInterposerModuleLoaded());
    EXPECT_EQ(GetLoadedThirdPartyOverlayModuleName(), nullptr);
    EXPECT_FALSE(IsThirdPartyOverlayLoaded());
    EXPECT_EQ(GetStartupBlockingOverlayModuleName(), nullptr);

    NoteModuleUnloadedForOverlayCache("sl.interposer.dll");
    EXPECT_FALSE(IsStreamlineInterposerModuleLoaded());
}

}  // namespace
