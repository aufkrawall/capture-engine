#include "test_dxgi_shared_shared.h"

TEST(DXGISharedTest, ProcessDiscoveryUsesEventDrivenStartTraceAndNoWmiPollingQuery) {
    const std::wstring realtimeQuery = ce::injection_policy::kRealtimeProcessStartQuery;

    EXPECT_NE(realtimeQuery.find(L"Win32_ProcessStartTrace"), std::wstring::npos);
    // A `WITHIN` clause is what turns a WMI subscription into a service-side
    // poll over every Win32_Process instance. CE's only query must never carry
    // one; the unelevated path is the native poller instead.
    EXPECT_EQ(realtimeQuery.find(L"WITHIN"), std::wstring::npos);
    EXPECT_EQ(realtimeQuery.find(L"__InstanceCreationEvent"), std::wstring::npos);
}

// The poll interval is a system-wide sweep, so its bounds are policy rather than
// a tuning detail: too fast is a permanent background cost for no gain (a real
// title leaves seconds of margin before its first swapchain), and too slow would
// eventually matter.
TEST(DXGISharedTest, NativeProcessStartPollIntervalStaysWithinItsBounds) {
    using namespace ce::process_start;
    EXPECT_GE(kDefaultPollIntervalMs, kMinPollIntervalMs);
    EXPECT_LE(kDefaultPollIntervalMs, kMaxPollIntervalMs);
    EXPECT_EQ(ClampPollIntervalMs(0u), kMinPollIntervalMs);
    EXPECT_EQ(ClampPollIntervalMs(kMaxPollIntervalMs + 1u), kMaxPollIntervalMs);
    EXPECT_EQ(ClampPollIntervalMs(kDefaultPollIntervalMs), kDefaultPollIntervalMs);
    // Faster than the WMI fallback it replaces, which is free here because the
    // sweep no longer materialises a Win32_Process instance per process.
    EXPECT_LT(kDefaultPollIntervalMs, 500u);
}

// The app-callback deep draw and no-callback final-batch draw key renderer state by the presented FFX
// swapchain, while the queue bindings are keyed by the game-facing proxy. Both the explicit Streamline
// enable prep and the FFX context-destroy unregister must retire the fence and inline-marker maps, otherwise
// orphaned command lists/backbuffer references survive FFX teardown and the game's next resize/present fails
// E_ACCESSDENIED (Talos 20260813_142910).
TEST(DXGISharedSourceTest, NativeFSRTeardownRetiresEverySuspendOverlayState) {
    namespace fs = std::filesystem;
    auto readFile = [](const fs::path& p) {
        EXPECT_TRUE(fs::exists(p)) << p.string();
        const std::string text = ce::test_source::ReadLogicalSource(p);
        EXPECT_FALSE(text.empty()) << p.string();
        return text;
    };

    const std::string overlay = readFile(fs::current_path() / "hook" / "apis" / "dx12_ffx_suspend_overlay.cpp");
    ASSERT_FALSE(overlay.empty());
    EXPECT_NE(overlay.find("void RetireAllForNativeFSRTeardown("), std::string::npos)
        << "the teardown retire-all entry point must exist";
    EXPECT_NE(overlay.find("{&g_ProxyStates, &g_InlineProxyStates}"), std::string::npos)
        << "teardown must enumerate both the fenced baseline and inline-marker renderer maps";
    EXPECT_NE(overlay.find("for (const auto& entry : *states)"), std::string::npos)
        << "teardown must retire every live state, including either presented-swapchain draw key";

    const std::string fgState = readFile(fs::current_path() / "hook" / "apis" / "dx12_hook_fg_state.cpp");
    ASSERT_FALSE(fgState.empty());
    const size_t prep = fgState.find("DX12_PrepareForStreamlineEnableTransition");
    ASSERT_NE(prep, std::string::npos);
    const size_t retire = fgState.find("RetireAllForNativeFSRTeardown(", prep);
    EXPECT_NE(retire, std::string::npos) << "Streamline enable prep must retire the suspend-overlay states";
    EXPECT_NE(fgState.find("ShouldRetireNativeFSRSuspendOverlayStatesBeforeStreamlineEnable(", prep),
              std::string::npos)
        << "the retire must stay gated to the live FSR-owned swapchain";
    const size_t invalidate = fgState.find("InvalidateAllOverlayCachedFrames()", prep);
    EXPECT_NE(invalidate, std::string::npos);
    if (retire != std::string::npos && invalidate != std::string::npos) {
        EXPECT_LT(retire, invalidate) << "states must be retired before the game tears the FFX swapchain down";
    }

    const std::string ownerQueue = readFile(fs::current_path() / "hook" / "apis" / "dx12_hook_ffx_owner_queue.cpp");
    ASSERT_FALSE(ownerQueue.empty());
    const size_t unregister = ownerQueue.find("DX12_UnregisterNativeFSRSwapchainPresentationQueue");
    ASSERT_NE(unregister, std::string::npos);
    EXPECT_NE(ownerQueue.find("RetireAllForNativeFSRTeardown(reason);", unregister), std::string::npos)
        << "FFX context destruction must retire orphaned presented-swapchain renderer states";

    const std::string policy =
        readFile(fs::current_path() / "hook" / "common" / "dx12_overlay_policy" / "ffx_routing.h");
    ASSERT_FALSE(policy.empty());
    EXPECT_NE(policy.find("ShouldRetireNativeFSRSuspendOverlayStatesBeforeStreamlineEnable("), std::string::npos)
        << "the boundary gate must live in the shared overlay policy";
}

// Resident-hook reactivation must rebind every session-scoped diagnostic to the
// replacement host's log directory: crash dumps, perf_metrics_*.csv, and the
// cached fps_limiter_trace.log path. Without this, a reactivated hook keeps
// writing to the previous CE session (20260811_212728: no perf_metrics CSV in
// the new session; frames landed in the old session's file).
TEST(DXGISharedSourceTest, ResidentHookReactivationRebindsSessionDiagnostics) {
    namespace fs = std::filesystem;
    auto readFile = [](const fs::path& p) {
        EXPECT_TRUE(fs::exists(p)) << p.string();
        const std::string text = ce::test_source::ReadLogicalSource(p);
        EXPECT_FALSE(text.empty()) << p.string();
        return text;
    };

    const std::string lifecycle = readFile(fs::current_path() / "hook" / "main_host_lifecycle.cpp");
    ASSERT_FALSE(lifecycle.empty());
    EXPECT_NE(lifecycle.find("SetCrashDumpDirectory(sessionLogsDir, /*archiveInstalledSymbols=*/false)"),
              std::string::npos)
        << "reactivation must re-point the crash dump directory to the new session (the controller, not the "
           "hook, archives that session's symbols)";
    EXPECT_NE(lifecycle.find("PerfLogger::Get().Init(perfLogPath, true)"), std::string::npos)
        << "reactivation must force-rebind the perf metrics CSV to the new session";
    EXPECT_NE(lifecycle.find("g_SharedFpsLimiter.ResetTraceLogPath()"), std::string::npos)
        << "reactivation must drop the cached fps_limiter_trace.log path";

    const std::string perfHeader = readFile(fs::current_path() / "hook" / "common" / "perf_logger.h");
    ASSERT_FALSE(perfHeader.empty());
    EXPECT_NE(perfHeader.find("void Init(const char* logPath, bool forceRebind = false);"), std::string::npos)
        << "PerfLogger must expose the force-rebind entry point";

    const std::string perfLogger = readFile(fs::current_path() / "hook" / "common" / "perf_logger.cpp");
    ASSERT_FALSE(perfLogger.empty());
    const size_t initPos = perfLogger.find("void PerfLogger::Init(");
    ASSERT_NE(initPos, std::string::npos);
    EXPECT_NE(perfLogger.find("fclose(file_)", initPos), std::string::npos)
        << "force rebind must finalize the previous session CSV";
    EXPECT_NE(perfLogger.find("frameCount_.store(0", initPos), std::string::npos)
        << "force rebind must restart the CSV frame sequence";

    const std::string limiterHeader = readFile(fs::current_path() / "hook" / "common" / "fps_limiter.h");
    ASSERT_FALSE(limiterHeader.empty());
    EXPECT_NE(limiterHeader.find("void ResetTraceLogPath();"), std::string::npos)
        << "FpsLimiter must expose the trace-path reset entry point";
}

// Talos 20260903_070055: DLSS FG -> FSR FG -> DLSS FG left the provisional official FFX startup latch
// armed, because the second FSR enable never issued an enabled ffxConfigure and DLSS-G came back through
// GetState instead of an explicit slDLSSGSetOptions enable. The latch quiesces every CE GPU side effect,
// so InitOverlaySync kept syncInit=0 and PostSL skipped every present until the game was closed.
TEST(DXGISharedTest, AuthoritativeStreamlineOwnershipRetiresAbandonedOfficialFFXStartup) {
    using ce::dx12_overlay_policy::ShouldRetireProtectedOfficialFFXStartupForAuthoritativeStreamlineOwnership;

    EXPECT_TRUE(ShouldRetireProtectedOfficialFFXStartupForAuthoritativeStreamlineOwnership(true, true, false));

    // Nothing to retire, no Streamline ownership proof, or FSR FG genuinely owns the runtime.
    EXPECT_FALSE(ShouldRetireProtectedOfficialFFXStartupForAuthoritativeStreamlineOwnership(false, true, false));
    EXPECT_FALSE(ShouldRetireProtectedOfficialFFXStartupForAuthoritativeStreamlineOwnership(true, false, false));
    EXPECT_FALSE(ShouldRetireProtectedOfficialFFXStartupForAuthoritativeStreamlineOwnership(true, true, true));
}

// The latch is armed by one official FFX frame-generation SWAPCHAIN context create, so destroying that
// context retires it. The process-wide FG context count cannot be the bound: Talos keeps its
// FRAMEGENERATION context alive across FG toggles, so the count never returns to zero.
TEST(DXGISharedTest, DestroyedFFXSwapchainContextRetiresUnconfirmedOfficialFFXStartup) {
    using ce::dx12_overlay_policy::ShouldRetireProtectedOfficialFFXStartupForDestroyedFFXSwapchainContext;

    EXPECT_TRUE(ShouldRetireProtectedOfficialFFXStartupForDestroyedFFXSwapchainContext(true, true));

    EXPECT_FALSE(ShouldRetireProtectedOfficialFFXStartupForDestroyedFFXSwapchainContext(false, true));
    EXPECT_FALSE(ShouldRetireProtectedOfficialFFXStartupForDestroyedFFXSwapchainContext(true, false));
    EXPECT_FALSE(ShouldRetireProtectedOfficialFFXStartupForDestroyedFFXSwapchainContext(false, false));
}

// The guarded Steam transport reaches a foreign Present through a function pointer.
// Every condition that establishes it is callable is evaluated into a bool and handed
// to a policy helper, so the call site itself carries no proof - which is also why the
// static analyzer reads it as a possible null call. Keep an explicit guard immediately
// before the indirect call, so a future policy change cannot make it reachable.
TEST(DXGISharedSourceTest, GuardedSteamPresentChecksTheHookPointerAtTheCallSite) {
    namespace fs = std::filesystem;
    const fs::path steamSource = fs::current_path() / "hook" / "common" / "dxgi_shared_steam.cpp";
    ASSERT_TRUE(fs::exists(steamSource));
    const std::string steam = ce::test_source::ReadFile(steamSource);
    ASSERT_FALSE(steam.empty());

    const size_t indirectCall = steam.find("externalPresent(pSwapChain, SyncInterval, Flags)");
    ASSERT_NE(indirectCall, std::string::npos);
    const size_t guard = steam.rfind("if (!externalPresent) {", indirectCall);
    ASSERT_NE(guard, std::string::npos);
    EXPECT_LT(guard, indirectCall);
}

// Session 20260923_233317 (Talos, FSR FG -> DLSS FG in the menu): sl.dlss_g created the new swapchain on
// its own queue, the game kept submitting on its primary queue and DLSS-G stayed ON-but-not-interpolating.
// The queue-settle defer (a guard for a DEPARTING runtime's leftover queue) waited for cmdQ==scQ, which
// never happens there, so the overlay stayed gone until the game closed. The INCOMING runtime's fresh
// swapchain on a submittable queue must initialize the overlay immediately.
TEST(DXGISharedTest, FreshStreamlineHandoffSwapchainInitIsNotDeferredByQueueSettle) {
    using ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit;
    // The exact failing state: FG inactive, runtime owns, cmdQ != scQ, fresh handoff on a submittable queue.
    EXPECT_FALSE(ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(
        /*actualFGActive=*/false, /*streamlineFGRunning=*/false, /*runtimeOwnsSwapchain=*/true,
        /*hasSwapchainQueue=*/true, /*hasCommandQueue=*/true, /*commandQueueMatchesSwapchainQueue=*/false,
        /*retainedNoCallbackFSRSuspension=*/false, /*freshStreamlineHandoffOnSubmittableQueue=*/true));
    // Command tracking not populated yet: the render targets the live swapchain queue either way.
    EXPECT_FALSE(ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, false, true, true,
                                                                     /*hasCommandQueue=*/false, false, false, true));
    // Without the fresh handoff the departing-runtime guard is unchanged.
    EXPECT_TRUE(ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, false, true, true, true, false, false,
                                                                    /*freshHandoff=*/false));
    EXPECT_TRUE(ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, false, true, true, false, false, false,
                                                                    /*freshHandoff=*/false));
}
