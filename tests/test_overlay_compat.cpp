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

TEST_F(OverlayModuleDetectionTest, IsThirdPartyOverlayLoadedTracksOverlaySubset) {
    EXPECT_FALSE(IsThirdPartyOverlayLoaded());
    NoteModuleLoadedForOverlayCache("RTSSHooks64.dll");
    EXPECT_TRUE(IsThirdPartyOverlayLoaded());
    NoteModuleUnloadedForOverlayCache("RTSSHooks64.dll");
    EXPECT_FALSE(IsThirdPartyOverlayLoaded());
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
