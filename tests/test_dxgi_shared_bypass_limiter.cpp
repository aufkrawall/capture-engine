#include "test_dxgi_shared_shared.h"

namespace {

std::string ReadHookSource(const char* name) {
    return ce::test_source::ReadFile(std::filesystem::current_path() / "hook" / "common" / name);
}

// Asserts that, inside the branch starting at `finalBranch` and ending at
// `nextBranch`, the limiter stage runs before the vsync override and the bypass
// trampoline, and the post-present half runs after the forwarded Present.
void ExpectLimiterAroundBypassedPresent(const std::string& source, const char* finalBranch, const char* nextBranch,
                                        const char* bypassCall) {
    const size_t branch = source.find(finalBranch);
    ASSERT_NE(branch, std::string::npos) << finalBranch;
    const size_t end = source.find(nextBranch, branch);
    ASSERT_NE(end, std::string::npos) << nextBranch;

    const size_t limiter = source.find("ApplyFpsLimiterBeforeBypassedFinalOutputPresent(pSwapChain", branch);
    const size_t vsync = source.find("ProcessPresentVSyncOverride(SyncInterval, Flags, pSwapChain);", branch);
    const size_t forward = source.find(bypassCall, branch);
    ASSERT_NE(limiter, std::string::npos);
    ASSERT_NE(vsync, std::string::npos);
    ASSERT_NE(forward, std::string::npos);
    EXPECT_LT(limiter, end);
    EXPECT_LT(limiter, vsync);
    EXPECT_LT(vsync, forward);
    EXPECT_LT(forward, end);

    const size_t postPresent = source.find("g_SharedFpsLimiter.ApplyPostPresent();", forward);
    ASSERT_NE(postPresent, std::string::npos);
    EXPECT_LT(postPresent, end);
}

}  // namespace

// Regression: after an in-game FSR FG -> DLSS FG switch with the Steam overlay
// loaded, every Streamline final output took the post-FSR confirmed-standalone
// bypass, which returned before the normal route's limiter stage. Capture sync
// (and the general cap) then stayed inactive until the game was restarted.
TEST(DXGISharedSourceTest, PostFSRConfirmedStandaloneBypassRunsTheFpsLimiter) {
    const std::string present = ReadHookSource("dxgi_shared_present_routing.cpp");
    const std::string present1 = ReadHookSource("dxgi_shared_present1.cpp");
    ASSERT_FALSE(present.empty());
    ASSERT_FALSE(present1.empty());

    EXPECT_NE(present.find("ApplyFpsLimiterBeforeBypassedFinalOutputPresent(pSwapChain, ctx.inWrapperPresent || "
                           "ctx.wrappedSwapchain,"),
              std::string::npos);
    EXPECT_NE(present1.find("ApplyFpsLimiterBeforeBypassedFinalOutputPresent(pSwapChain, inWrapperPresent || "
                            "wrappedSwapchain,"),
              std::string::npos);
    ExpectLimiterAroundBypassedPresent(present,
                                       "ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(",
                                       "if (ctx.streamlineSyntheticReentrant)",
                                       "ForwardPresentThrough(presentBypass, pSwapChain, SyncInterval, Flags)");
    ExpectLimiterAroundBypassedPresent(
        present1, "ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(",
        "if (streamlineSyntheticReentrant)", "present1Bypass(pSwapChain, SyncInterval, Flags, pPresentParameters)");
}

TEST(DXGISharedSourceTest, PostFSRConfirmedStandaloneGuardedSteamPresentCompletesTheLimiter) {
    const std::string present = ReadHookSource("dxgi_shared_present_routing.cpp");
    const size_t branch = present.find("ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(");
    ASSERT_NE(branch, std::string::npos);
    const size_t guarded = present.find("\"post-FSR confirmed standalone Present\", &guardedSteamHr", branch);
    const size_t guardedReturn = present.find("return guardedSteamHr;", guarded);
    const size_t postPresent = present.find("g_SharedFpsLimiter.ApplyPostPresent();", guarded);
    ASSERT_NE(guarded, std::string::npos);
    ASSERT_NE(guardedReturn, std::string::npos);
    ASSERT_NE(postPresent, std::string::npos);
    EXPECT_LT(postPresent, guardedReturn);
}

// The bypass helper must arm the limiter exactly as the normal Present route
// does, so a recovered route paces identically to a fresh game start.
TEST(DXGISharedSourceTest, BypassLimiterStageMatchesTheNormalPresentRoute) {
    const std::string routing = ReadHookSource("dxgi_shared_present_routing.cpp");
    const std::string core = ReadHookSource("dxgi_shared_present_core.cpp");
    const char* kNormalRouteApply =
        "g_SharedFpsLimiter.Apply(true, ce::fps_limiter_policy::PresentSite::kUniqueApplicationPresent);";
    ASSERT_NE(core.find(kNormalRouteApply), std::string::npos);

    const size_t helper = routing.find("void ApplyFpsLimiterBeforeBypassedFinalOutputPresent(");
    ASSERT_NE(helper, std::string::npos);
    const size_t helperEnd = routing.find("HRESULT ExecuteStartupRouting(", helper);
    ASSERT_NE(helperEnd, std::string::npos);
    // A wrapper-owned Present was already paced by the wrapper; a second Apply
    // would break the one-Apply-per-present contract of this site.
    const size_t wrapperSkip = routing.find("if (!g_IPC || wrapperOwnsPresent) {", helper);
    const size_t setIpc = routing.find("g_SharedFpsLimiter.SetIPCClient(g_IPC);", helper);
    const size_t apply = routing.find(kNormalRouteApply, helper);
    const size_t latency = routing.find("ApplyPresentFrameLatencyOverrides(pSwapChain);", helper);
    ASSERT_NE(wrapperSkip, std::string::npos);
    ASSERT_NE(setIpc, std::string::npos);
    ASSERT_NE(apply, std::string::npos);
    EXPECT_LT(wrapperSkip, setIpc);
    ASSERT_NE(latency, std::string::npos);
    EXPECT_LT(setIpc, apply);
    EXPECT_LT(apply, latency);
    EXPECT_LT(latency, helperEnd);
}
