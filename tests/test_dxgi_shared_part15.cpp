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
// swapchain on a submittable queue must initialize the overlay immediately — at BOTH init-deferral gates
// of the Phase3 block: the second gate keys its defer on command tracking the handoff has not populated
// yet, so an exemption at the first gate alone just walks into the second one.
TEST(DXGISharedTest, FreshStreamlineHandoffSwapchainInitIsNotDeferredByQueueSettle) {
    using ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit;
    using ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown;
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
                                                                    /*freshStreamlineHandoffOnSubmittableQueue=*/false));
    EXPECT_TRUE(ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(false, false, true, true, false, false, false,
                                                                    /*freshStreamlineHandoffOnSubmittableQueue=*/false));

    // Same handoff state at the SECOND gate: recent Streamline teardown grace is active (the
    // transition's own late observer re-seeds it) and command tracking is empty — this deferred
    // right past the gate-A exemption and kept the overlay gone.
    EXPECT_FALSE(ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        /*actualFGActive=*/false, /*streamlineFGRunning=*/false, /*recentStreamlineTeardown=*/true,
        /*hasSwapchainQueue=*/true, /*hasOriginalGameQueue=*/true, /*hasPostSLLastWorkingQueue=*/true,
        /*hasCommandQueue=*/false, /*commandQueueMatchesSwapchainQueue=*/false,
        /*commandQueueMatchesOriginalGameQueue=*/false, /*commandQueueMatchesPrimaryGameQueue=*/false,
        /*freshStreamlineHandoffOnSubmittableQueue=*/true));
    // Command tracking populated but still on a non-matching queue: the exemption wins there too.
    EXPECT_FALSE(ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, true, true, true, /*hasCommandQueue=*/true, false, false, false,
        /*freshStreamlineHandoffOnSubmittableQueue=*/true));
    // Without the exemption both states keep deferring (the Talos DEVICE_REMOVED settle guard).
    EXPECT_TRUE(ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, true, true, true, /*hasCommandQueue=*/false, false, false, false,
        /*freshStreamlineHandoffOnSubmittableQueue=*/false));
    EXPECT_TRUE(ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
        false, false, true, true, true, true, /*hasCommandQueue=*/true, false, false, false,
        /*freshStreamlineHandoffOnSubmittableQueue=*/false));
}

// GTA V Enhanced FSR FG -> DLSS FG (session 20260925_050613): sl.dlss_g created the new
// swapchain on its own queue while the game kept rendering on origGame. The stale no-FG
// cleanup read "command traffic on origGame" as "the runtime swapchain is not live",
// retargeted CE's backbuffer work to origGame and the device was removed one frame later.
TEST(DXGISharedTest, StaleRuntimeOwnedStreamlineNoFGNeverTracksWhileItsSwapchainIsPresented) {
    using ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun;
    using ce::fg_runtime::RuntimeMode;

    // The crash state: every other stale signal holds, but the presented
    // swapchain lives on the runtime's queue.
    EXPECT_FALSE(ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        false, true, RuntimeMode::kStreamlineNoFG, true, /*commandQueueUsesOriginalGameQueue=*/true, false,
        /*presentedSwapchainUsesOriginalGameQueue=*/false));
    // The case the cleanup exists for: the game presents its own swapchain on origGame.
    EXPECT_TRUE(ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        false, true, RuntimeMode::kStreamlineNoFG, true, /*commandQueueUsesOriginalGameQueue=*/true, false,
        /*presentedSwapchainUsesOriginalGameQueue=*/true));
}

// Same session: CE submitted its overlay on the Streamline swapchain's queue and deferred the
// fence Signal, but the post-Present flush skipped every runtime-owned swapchain. The fence
// never advanced, the upload ring reported all 16 slots in flight and the overlay stopped
// drawing. Only AMD's native FSR presentation queue may skip the flush.
TEST(DXGISharedTest, DeferredOverlaySignalFlushSkipsOnlyAMDNativeFSRPresentationQueue) {
    using ce::dx12_overlay_policy::ShouldFlushDeferredOverlaySignalAfterHookedPresent;
    using ce::fg_runtime::RuntimeMode;

    // Streamline-owned swapchain, FG off or on: flush.
    EXPECT_TRUE(ShouldFlushDeferredOverlaySignalAfterHookedPresent(true, false, false, true,
                                                                   RuntimeMode::kStreamlineNoFG));
    EXPECT_TRUE(ShouldFlushDeferredOverlaySignalAfterHookedPresent(true, false, false, true, RuntimeMode::kDLSSFG));
    // Plain game swapchain: flush.
    EXPECT_TRUE(ShouldFlushDeferredOverlaySignalAfterHookedPresent(true, false, false, false, RuntimeMode::kOff));

    // AMD's presentation queue in each of its three shapes: skip.
    EXPECT_FALSE(ShouldFlushDeferredOverlaySignalAfterHookedPresent(true, true, false, false, RuntimeMode::kOff));
    EXPECT_FALSE(ShouldFlushDeferredOverlaySignalAfterHookedPresent(true, false, true, true,
                                                                    RuntimeMode::kStreamlineNoFG));
    EXPECT_FALSE(ShouldFlushDeferredOverlaySignalAfterHookedPresent(true, false, false, true, RuntimeMode::kFSRFG));

    // Not a D3D12 swapchain: nothing to flush.
    EXPECT_FALSE(ShouldFlushDeferredOverlaySignalAfterHookedPresent(false, false, false, false, RuntimeMode::kOff));
}

// GTA V Enhanced, DLSS FG toggled in the menu after FSR history (session 20260925_052251): the
// post-FSR recovery latch survived a proven normal return because that swapchain change landed in
// the recent-FG cooldown branch. The next DLSS FG toggle created a fresh Streamline swapchain on its
// own queue while the menu kept DLSS-G OFF; the stale latch demanded original-queue proof it can
// never have, and the overlay stayed GPU-quiet until the game was closed.
TEST(DXGISharedTest, PostFSRRecoveryEndsOnProvenReturnOrExactPrewarmedStreamlineHandoff) {
    using ce::dx12_overlay_policy::ShouldEndPostFSRNonFGRecoveryOnSwapchainChange;

    EXPECT_TRUE(ShouldEndPostFSRNonFGRecoveryOnSwapchainChange(true, /*normalRouteOwnershipProven=*/true, false));
    EXPECT_TRUE(ShouldEndPostFSRNonFGRecoveryOnSwapchainChange(true, false, /*exactPrewarmedStreamlineHandoff=*/true));
    // A bare pointer change proves nothing: the latch keeps an unknown swapchain quiet.
    EXPECT_FALSE(ShouldEndPostFSRNonFGRecoveryOnSwapchainChange(true, false, false));
    // Nothing to end.
    EXPECT_FALSE(ShouldEndPostFSRNonFGRecoveryOnSwapchainChange(false, true, true));
}

TEST(DXGISharedTest, ExactPrewarmedStreamlineHandoffIsNotHeldQuietByStalePostFSRRecovery) {
    using ce::dx12_overlay_policy::DecideInactiveDLSSPresentRoute;
    using Route = ce::dx12_overlay_policy::InactiveDLSSPresentRoute;

    // The session state: recovery pending, DLSS-G OFF, no original-queue proof, no keep-alive,
    // no PostSL callback or queue, never presented through PostSL.
    EXPECT_EQ(DecideInactiveDLSSPresentRoute(true, false, false, false, false, false, false, false,
                                             /*currentSwapchainIsExactPrewarmedStreamlineHandoff=*/true),
              Route::kNormal);
    // Any other unknown swapchain in that state still waits for proof.
    EXPECT_EQ(DecideInactiveDLSSPresentRoute(true, false, false, false, false, false, false, false,
                                             /*currentSwapchainIsExactPrewarmedStreamlineHandoff=*/false),
              Route::kAwaitNormalOwnershipProof);
    // The exact confirmed PostSL proxy keeps its keep-alive route.
    EXPECT_EQ(DecideInactiveDLSSPresentRoute(true, false, false, false, true, true, true, true, false),
              Route::kConfirmedPostSLKeepAlive);
}

TEST(DXGISharedSourceTest, SwapchainChangeEndsPostFSRRecoveryBeforeConsumingProofOrCoolingDown) {
    namespace fs = std::filesystem;
    const fs::path phase1 = fs::current_path() / "hook" / "apis" / "dx12_hook_process_session_phase1.cpp";
    const fs::path phase2 = fs::current_path() / "hook" / "apis" / "dx12_hook_process_session_phase2.cpp";
    ASSERT_TRUE(fs::exists(phase1));
    ASSERT_TRUE(fs::exists(phase2));
    const std::string gate = ce::test_source::ReadLogicalSource(phase1);
    const std::string text = ce::test_source::ReadLogicalSource(phase2);
    ASSERT_FALSE(gate.empty());
    ASSERT_FALSE(text.empty());

    // The GPU-quiet gate must see the prewarmed identity before it decides.
    const size_t prewarmedLoad = gate.find("dx12_hook_g_PrewarmedPostSLHandoffSwapchain.load(");
    const size_t gateDecision = gate.find("DecideInactiveDLSSPresentRoute(");
    ASSERT_NE(prewarmedLoad, std::string::npos);
    ASSERT_NE(gateDecision, std::string::npos);
    EXPECT_LT(prewarmedLoad, gateDecision);
    EXPECT_NE(gate.find("exactPrewarmedStreamlineHandoff);", gateDecision), std::string::npos);

    // The prewarmed handoff ends the recovery before its one-shot identity is consumed.
    const size_t handoffEnd =
        text.find("EndPostFSRNonFGRecoveryOnProvenSwapchainChange(false, exactPrewarmedPostSLHandoffSwapchainProof);");
    const size_t proofConsumed = text.find("dx12_hook_g_PrewarmedPostSLHandoffSwapchain.compare_exchange_strong(");
    ASSERT_NE(handoffEnd, std::string::npos);
    ASSERT_NE(proofConsumed, std::string::npos);
    EXPECT_LT(handoffEnd, proofConsumed);

    // A guarded change ends a proven recovery before either cooldown/reinit branch runs.
    const size_t guardedEnd =
        text.find("EndPostFSRNonFGRecoveryOnProvenSwapchainChange(postFSRNormalRouteOwnershipProven, false);");
    ASSERT_NE(guardedEnd, std::string::npos);
    const size_t immediateBranch = text.find("if (guardSwapchainReinit &&", guardedEnd);
    const size_t cooldownBranch = text.find("} else if (guardSwapchainReinit) {", guardedEnd);
    EXPECT_NE(immediateBranch, std::string::npos);
    EXPECT_NE(cooldownBranch, std::string::npos);
}

// GTA V Enhanced, FSR FG -> all FG off in the menu (session 20260925_054901): FSR FG never got an
// enabled ffxConfigure in the menu, so its protected startup latch stayed armed. CE had missed GTA's
// in-flight ffxCreateContext (the FFX module reloads per toggle), so the context destroys were
// classified non-FG and the destroy exit never ran. The game then created its own swapchain on the
// original queue and every Present of it stayed tracking-only until exit.
TEST(DXGISharedTest, GameSwapchainReturnOnOriginalQueueRetiresProtectedFFXStartup) {
    using ce::dx12_overlay_policy::ShouldRetireProtectedOfficialFFXStartupForGameSwapchainReturn;

    // The session state: same window, game-created, original queue.
    EXPECT_TRUE(ShouldRetireProtectedOfficialFFXStartupForGameSwapchainReturn(true, true, false, true, true, true));
    // Window unknown (latch armed from a queue capture): the original-queue game create still decides.
    EXPECT_TRUE(ShouldRetireProtectedOfficialFFXStartupForGameSwapchainReturn(true, true, false, true, false, false));

    // Nothing pending.
    EXPECT_FALSE(ShouldRetireProtectedOfficialFFXStartupForGameSwapchainReturn(false, true, false, true, true, true));
    // AMD's own nested swapchain create or a runtime/overlay create is not a game return.
    EXPECT_FALSE(ShouldRetireProtectedOfficialFFXStartupForGameSwapchainReturn(true, false, false, true, true, true));
    EXPECT_FALSE(ShouldRetireProtectedOfficialFFXStartupForGameSwapchainReturn(true, true, true, true, true, true));
    // A fresh queue proves nothing about the protected swapchain.
    EXPECT_FALSE(ShouldRetireProtectedOfficialFFXStartupForGameSwapchainReturn(true, true, false, false, true, true));
    // Another window can coexist with the protected FFX swapchain.
    EXPECT_FALSE(ShouldRetireProtectedOfficialFFXStartupForGameSwapchainReturn(true, true, false, true, true, false));
}

TEST(DXGISharedSourceTest, GameSwapchainReturnRetiresProtectedFFXStartupBeforeItsFirstPresent) {
    namespace fs = std::filesystem;
    const fs::path tracking = fs::current_path() / "hook" / "apis" / "dx12_hook_swapchain_tracking.cpp";
    const fs::path startup = fs::current_path() / "hook" / "apis" / "dx12_hook_fg_startup.cpp";
    const fs::path ffxStartup = fs::current_path() / "hook" / "apis" / "dx12_hook_ffx_startup.cpp";
    ASSERT_TRUE(fs::exists(tracking));
    ASSERT_TRUE(fs::exists(startup));
    ASSERT_TRUE(fs::exists(ffxStartup));
    const std::string capture = ce::test_source::ReadLogicalSource(tracking);
    const std::string retire = ce::test_source::ReadLogicalSource(startup);
    const std::string arm = ce::test_source::ReadLogicalSource(ffxStartup);

    // Swapchain creation retires the latch after the queue is published and before the Streamline
    // handoff branch; the first Present of the replacement then takes the normal route.
    const size_t capturedDecl = capture.find("void CaptureSwapchainQueueFromCreateDevice(");
    const size_t setQueue = capture.find("DX12_SetSwapchainQueue(pQueue,", capturedDecl);
    const size_t retireCall = capture.find("DX12_RetireProtectedOfficialFFXStartupForGameSwapchainReturn(", setQueue);
    const size_t handoffBranch = capture.find("if (freshAuthoritativeStreamlineHandoff) {", setQueue);
    ASSERT_NE(capturedDecl, std::string::npos);
    ASSERT_NE(setQueue, std::string::npos);
    ASSERT_NE(retireCall, std::string::npos);
    ASSERT_NE(handoffBranch, std::string::npos);
    EXPECT_LT(retireCall, handoffBranch);

    const size_t helper = retire.find("void DX12_RetireProtectedOfficialFFXStartupForGameSwapchainReturn(");
    ASSERT_NE(helper, std::string::npos);
    const size_t decision = retire.find("ShouldRetireProtectedOfficialFFXStartupForGameSwapchainReturn(", helper);
    const size_t clear = retire.find("DX12_ClearNativeFSRStartupConfigureArming(", decision);
    ASSERT_NE(decision, std::string::npos);
    ASSERT_NE(clear, std::string::npos);

    // The protected window is recorded when the latch is armed from the FFX swapchain create, and
    // forgotten whenever the latch is cleared.
    const size_t armCreate = arm.find("bool HandleProtectedOfficialFFXStartupSwapchainCreate(");
    const size_t hwndStore = arm.find("dx12_hook_g_ProtectedOfficialFFXStartupHwnd.store(protectedHwnd", armCreate);
    const size_t pendingStore = arm.find("dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.store(true", armCreate);
    ASSERT_NE(armCreate, std::string::npos);
    ASSERT_NE(hwndStore, std::string::npos);
    ASSERT_NE(pendingStore, std::string::npos);
    EXPECT_LT(hwndStore, pendingStore);
    const size_t clearFn = arm.find("void ClearProtectedOfficialFFXStartupSwapchainPending(");
    ASSERT_NE(clearFn, std::string::npos);
    EXPECT_NE(arm.find("dx12_hook_g_ProtectedOfficialFFXStartupHwnd.store(nullptr", clearFn), std::string::npos);
}
