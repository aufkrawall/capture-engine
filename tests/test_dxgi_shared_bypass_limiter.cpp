#include "test_dxgi_shared_shared.h"

#include <tuple>

namespace {

std::string ReadHookSource(const char* name) {
    return ce::test_source::ReadFile(ce::test_source::FindSource("hook", name));
}

}  // namespace

// Regression: after an in-game FSR FG -> DLSS FG switch with the Steam overlay
// loaded, every Streamline final output took the post-FSR confirmed-standalone
// bypass, which returned before the normal route's limiter stage. Capture sync
// (and the general cap) then stayed inactive until the game was restarted.
//
// The same route wrote no CSV row for all of DLSS FG and fed neither result report, and its costs read as
// `startup_routing` (Talos 20261104_210842). Flow scenarios cannot reach it, because it needs the Steam overlay.
TEST(DXGISharedSourceTest, PostFSRConfirmedStandaloneBypassEndsLikeTheNormalRoute) {
    const std::string helper = ReadHookSource("dxgi_shared_forwarded_present.h");
    ASSERT_FALSE(helper.empty());
    const size_t route = helper.find("HRESULT ExecuteStandaloneOutputBypass(");
    ASSERT_NE(route, std::string::npos);
    size_t previous = route;
    for (const char* marker : {"ForwardedPresentRecord record(true, route);",
                               "InvokePostSLCallbackForFinalOutputPresent(postSLCallback, pSwapChain);",
                               "RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, route);",
                               "record.TimeLimiter(",
                               "ApplyFpsLimiterBeforeBypassedFinalOutputPresent(pSwapChain, wrapperOwnsPresent,",
                               "CostScope limiterStage(CostStage::kLimiter);",
                               "ProcessPresentVSyncOverride(SyncInterval, Flags, pSwapChain);",
                               "record.Forward([&] { return forward(SyncInterval, Flags); });",
                               "EnterStage(CostStage::kPostPresent);",
                               "return FinishForwardedPresent(pSwapChain, SyncInterval, Flags, APIType::D3D12, hr, presentName);"}) {
        const size_t position = helper.find(marker, previous);
        ASSERT_NE(position, std::string::npos) << marker;
        previous = position;
    }

    for (const auto& [file, bypassCall, presentName] :
         {std::tuple<const char*, const char*, const char*>{"dxgi_shared_present_routing.cpp",
                                                            "ForwardPresentThrough(presentBypass, pSwapChain, sync, flags)",
                                                            "\"post-FSR confirmed standalone Present\", \"Present\""},
          std::tuple<const char*, const char*, const char*>{
              "dxgi_shared_present1.cpp", "ForwardPresent1Through(present1Bypass, pSwapChain, sync, flags, pPresentParameters)",
              "\"post-FSR confirmed standalone Present1\", \"Present1\""}}) {
        const std::string source = ReadHookSource(file);
        ASSERT_FALSE(source.empty()) << file;
        const size_t branch = source.find("ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(");
        ASSERT_NE(branch, std::string::npos) << file;
        const size_t execute = source.find("return ExecuteStandaloneOutputBypass(", branch);
        const size_t name = source.find(presentName, execute);
        const size_t forward = source.find(bypassCall, execute);
        ASSERT_NE(execute, std::string::npos) << file;
        ASSERT_NE(name, std::string::npos) << file;
        ASSERT_NE(forward, std::string::npos) << file;
        // Without a bypass trampoline the Present goes on to the normal route, whose limiter and overlay still run once.
        const size_t fallbackDraw = source.find("InvokePostSLCallbackForFinalOutputPresent(postSLCallback, pSwapChain);", forward);
        ASSERT_NE(fallbackDraw, std::string::npos) << file;
        EXPECT_EQ(source.substr(branch, fallbackDraw - branch).find("ProcessPresentVSyncOverride("), std::string::npos)
            << file << ": the route owns it";
    }
}

// The presents before that bypass takes over, and the routes around it, returned with no CSV row, no result report and no
// [PRESENT STALL] accounting: 9 outputs per switch in 20261104_222523, drawn and forwarded and otherwise invisible. The
// routes need Steam or the fake FFX startup, so these are source contracts; ForwardedPresentRecordTest covers the record.
TEST(DXGISharedSourceTest, EveryRouteThatForwardsAPresentRecordsAndReportsIt) {
    struct Route {
        const char* file;
        const char* open;
        const char* forward;
        const char* report;
    };
    const Route routes[] = {
        {"dxgi_shared_present_routing.cpp",
         "ForwardedPresentRecord record(presentBypass != nullptr, \"Streamline startup-handoff Present\");",
         "record.Forward([&] { return ForwardPresentThrough(presentBypass, pSwapChain, SyncInterval, Flags); });",
         "return ReportForwardedPresentResult(pSwapChain, SyncInterval, Flags, ctx.api, hr, \"Present\");"},
        {"dxgi_shared_present_routing.cpp",
         "ForwardedPresentRecord record(presentBypass != nullptr, \"Streamline startup normal-route Present\");",
         "record.Forward([&] { return ForwardPresentThrough(presentBypass, pSwapChain, SyncInterval, Flags); });",
         "return ReportForwardedPresentResult(pSwapChain, SyncInterval, Flags, ctx.api, hr, \"Present\");"},
        {"dxgi_shared_present_core.cpp", "ForwardedPresentRecord record(true, \"app-thread post-FSR Streamline startup handoff\");",
         "record.Forward([&] { return ForwardPresentThrough(dxgi_shared_oPresent, pSwapChain, SyncInterval, Flags); });",
         "return FinishForwardedPresent(pSwapChain, SyncInterval, Flags, ctx.api, handoffHr, \"Present\");"},
        {"dxgi_shared_present.cpp", "ForwardedPresentRecord record(true, \"FFX startup-handoff Present\");",
         "record.Forward([&] { return ForwardPresentThrough(presentBypass, pSwapChain, SyncInterval, Flags); });",
         "return ReportForwardedPresentResult(pSwapChain, SyncInterval, Flags, api, hr, \"Present\");"},
        {"dxgi_shared_present1.cpp",
         "ForwardedPresentRecord record(present1Bypass != nullptr, \"Streamline startup-handoff Present1\");",
         "ForwardPresent1Through(present1Bypass, pSwapChain, SyncInterval, Flags, pPresentParameters); });",
         "return ReportForwardedPresentResult(pSwapChain, SyncInterval, Flags, api, hr, \"Present1\");"},
        {"dxgi_shared_present1.cpp",
         "ForwardedPresentRecord record(present1Bypass != nullptr, \"Streamline startup normal-route Present1\");",
         "ForwardPresent1Through(present1Bypass, pSwapChain, SyncInterval, Flags, pPresentParameters); });",
         "return ReportForwardedPresentResult(pSwapChain, SyncInterval, Flags, api, hr, \"Present1\");"},
        {"dxgi_shared_present1.cpp", "ForwardedPresentRecord record(true, \"FFX startup-handoff Present1\");",
         "ForwardPresent1Through(present1Bypass, pSwapChain, SyncInterval, Flags, pPresentParameters); });",
         "return ReportForwardedPresentResult(pSwapChain, SyncInterval, Flags, api, hr, \"Present1\");"},
        {"dxgi_shared_present1.cpp",
         "ForwardedPresentRecord record(true, \"app-thread post-FSR Streamline startup handoff Present1\");",
         "ForwardPresent1Through(dxgi_shared_oPresent1, pSwapChain, SyncInterval, Flags, pPresentParameters); });",
         "return FinishForwardedPresent(pSwapChain, SyncInterval, Flags, api, handoffHr, \"Present1\");"},
    };
    for (const Route& route : routes) {
        const std::string source = ReadHookSource(route.file);
        ASSERT_FALSE(source.empty()) << route.file;
        const size_t open = source.find(route.open);
        ASSERT_NE(open, std::string::npos) << route.open;
        const size_t forward = source.find(route.forward, open);
        ASSERT_NE(forward, std::string::npos) << route.open;
        EXPECT_NE(source.find(route.report, forward), std::string::npos) << route.open;
    }
    // A guarded Steam invoke presents too, and its result is reported the same way.
    const std::string routing = ReadHookSource("dxgi_shared_present_routing.cpp");
    size_t guarded = routing.find("ForwardedPresentRecord record(presentBypass != nullptr");
    for (int route = 0; route < 2; ++route) {
        ASSERT_NE(guarded, std::string::npos);
        guarded = routing.find("return ReportForwardedPresentResult(pSwapChain, SyncInterval, Flags, ctx.api, guardedSteamHr, \"Present\");",
                               guarded + 1);
    }
    EXPECT_NE(guarded, std::string::npos);
}

// With the Present entry left to a foreign chain, the refresh cannot do anything and used to ask the kernel about the
// swapchain and its table first, on every Present of the route: `startup_routing` 131 us here, 9 us on an earlier run
// of the same code (20261104_210842, 20261104_154909; which part cost it is inferred, these stages will say).
TEST(DXGISharedSourceTest, TheHookRefreshAsksNothingItCannotUseAndNamesItsStage) {
    const std::string source = ReadHookSource("dxgi_shared_steam_routing.cpp");
    const size_t begin = source.find("void RefreshLivePresentHooksForSwapchainIfNeeded(");
    ASSERT_NE(begin, std::string::npos);
    const size_t end = source.find("HookLogImportant(", begin);
    ASSERT_NE(end, std::string::npos);
    const std::string refresh = source.substr(begin, end - begin);
    const size_t stage = refresh.find("Stage::kHookRefresh");
    const size_t foreignChain = refresh.find("if (presentEntryLeftToForeignChain) {");
    const size_t probe = refresh.find("ProbeSwapchainTable(pSwapChain, 23 * sizeof(void*), t_probed, IsReadableMemory)");
    ASSERT_NE(stage, std::string::npos);
    ASSERT_NE(foreignChain, std::string::npos);
    ASSERT_NE(probe, std::string::npos);
    EXPECT_LT(stage, foreignChain);
    EXPECT_LT(foreignChain, probe);
    EXPECT_EQ(refresh.find("IsReadableMemory("), std::string::npos) << "only through the memo";

    const std::string steam = ReadHookSource("dxgi_shared_steam.cpp");
    const size_t invoke = steam.find("bool TryInvokeGuardedExternalSteamOverlayPresent(");
    ASSERT_NE(invoke, std::string::npos);
    const size_t invokeStage = steam.find("Stage::kForeignInvoke", invoke);
    const size_t belowChain = steam.find("if (IsPresentInterceptedBelowForeignChain()) {", invoke);
    ASSERT_NE(invokeStage, std::string::npos);
    ASSERT_NE(belowChain, std::string::npos);
    EXPECT_LT(invokeStage, belowChain);
}

// The bypass helper arms the limiter through the same stage as the normal Present route, but as the
// final-output boundary it is: the bypass carries every Streamline output, while the normal route's site is
// the game's own Present. Matching the normal route's site gave 4x MFG two evaluations per game frame, so the
// Reflex warmup never finished (Talos 20261019_195220, see test_fps_limiter_bypass_site.cpp).
TEST(DXGISharedSourceTest, BypassLimiterStagePacesTheFinalOutputBoundary) {
    const std::string routing = ReadHookSource("dxgi_shared_present_routing.cpp");
    const std::string core = ReadHookSource("dxgi_shared_present_core.cpp");
    // Both select the site from the verdict read at Present entry; the normal
    // route's call wraps its argument onto the next line.
    const char* kNormalRouteApply = "ctx.callbackProvenRuntimeOutput || DX12_IsNoCallbackFSRFinalOutputCaptureAvailable()";
    ASSERT_NE(core.find(kNormalRouteApply), std::string::npos);

    const size_t helper = routing.find("void ApplyFpsLimiterBeforeBypassedFinalOutputPresent(");
    ASSERT_NE(helper, std::string::npos);
    const size_t helperEnd = routing.find("HRESULT ExecuteStartupRouting(", helper);
    ASSERT_NE(helperEnd, std::string::npos);
    EXPECT_EQ(routing.substr(helper, helperEnd - helper).find("ResolveDxgiPresentSite("), std::string::npos)
        << "the bypass helper must not use the application-present site";
    // A wrapper-owned Present was already paced by the wrapper; a second Apply
    // would break the one-Apply-per-present contract of this site.
    const size_t wrapperSkip = routing.find("if (!g_IPC || wrapperOwnsPresent) {", helper);
    const size_t stage = routing.find("ApplyFpsLimiterStage(pSwapChain, true,\n", helper);
    const size_t site = routing.find(
        "ce::fps_limiter_policy::ResolveBypassedFinalOutputSite(callbackProvenRuntimeOutput));", helper);
    ASSERT_NE(wrapperSkip, std::string::npos);
    ASSERT_NE(stage, std::string::npos);
    ASSERT_NE(site, std::string::npos);
    EXPECT_LT(wrapperSkip, stage);
    EXPECT_LT(stage, site);
    EXPECT_LT(site, helperEnd);
}

// Every DXGI limiter call goes through one stage (2026-10-18), so the D3D10/11 frame reaches the GPU
// before the limiter blocks on every route: armed for the Apply() call, then the frame-latency overrides.
TEST(DXGISharedSourceTest, TheLimiterStageArmsThePreWaitSubmissionAroundApply) {
    const std::string stage = ReadHookSource("dxgi_limiter_stage.cpp");
    const size_t helper = stage.find("void ApplyFpsLimiterStage(");
    ASSERT_NE(helper, std::string::npos);
    const size_t noIpc = stage.find("if (!g_IPC) {", helper);
    const size_t armed = stage.find(
        "const FpsLimiter::ScopedPreWaitSubmission submission({pSwapChain, &SubmitD3D1xRecordedWork});", helper);
    const size_t setIpc = stage.find("g_SharedFpsLimiter.SetIPCClient(g_IPC);", helper);
    const size_t apply = stage.find("g_SharedFpsLimiter.Apply(allowPostPresentReflexCadence, site);", helper);
    const size_t latency = stage.find("ApplyPresentFrameLatencyOverrides(pSwapChain);", helper);
    for (size_t at : {noIpc, armed, setIpc, apply, latency}) {
        ASSERT_NE(at, std::string::npos);
    }
    EXPECT_LT(noIpc, armed);
    EXPECT_LT(armed, setIpc);
    EXPECT_LT(setIpc, apply);
    EXPECT_LT(apply, latency);
    // The submission flushes the D3D11 immediate context, or the D3D10 device.
    EXPECT_NE(stage.find("immediate->Flush();"), std::string::npos);
    EXPECT_NE(stage.find("device10->Flush();"), std::string::npos);

    // No DXGI route calls the limiter around the stage.
    for (const char* name : {"dxgi_shared_present_core.cpp", "dxgi_shared_present1.cpp",
                             "dxgi_shared_present_routing.cpp", "dxgi_swapchain_wrap_present.cpp"}) {
        const std::string source = ReadHookSource(name);
        ASSERT_FALSE(source.empty()) << name;
        EXPECT_EQ(source.find("g_SharedFpsLimiter.Apply("), std::string::npos) << name;
        EXPECT_NE(source.find("ApplyFpsLimiterStage(p"), std::string::npos) << name;
    }
}
