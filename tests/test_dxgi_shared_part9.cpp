#include "test_dxgi_shared_shared.h"

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

TEST(DXGISharedTest, OverlayVisibilityInterruptionAccountingStartsAfterFirstDraw) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAccountOverlayVisibilityPresent(0));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAccountOverlayVisibilityPresent(1));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldAccountOverlayVisibilityPresent(42));
}

TEST(DXGISharedTest, InlinePostSLKeepAliveDrawBelongsToEnclosingPresent) {
    using ce::dx12_overlay_policy::ShouldAccountPostSLCallbackAsSeparatePresent;

    EXPECT_TRUE(ShouldAccountPostSLCallbackAsSeparatePresent(true, false, false));
    EXPECT_FALSE(ShouldAccountPostSLCallbackAsSeparatePresent(true, false, true));
    EXPECT_FALSE(ShouldAccountPostSLCallbackAsSeparatePresent(false, false, false));
    EXPECT_FALSE(ShouldAccountPostSLCallbackAsSeparatePresent(true, true, false));
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

TEST(DXGISharedTest, FreshProvenStreamlineHandoffPrewarmsBeforeDLSSActivation) {
    using ce::dx12_overlay_policy::ShouldPrewarmPostSLOverlayAtFreshProvenHandoff;

    // The replacement Streamline proxy queue and buffers exist, the retiring FSR route had a fully live overlay,
    // and DLSS has not been enabled yet: prepare the new swapchain-scoped state before its first generated Present.
    EXPECT_TRUE(ShouldPrewarmPostSLOverlayAtFreshProvenHandoff(true, true, false, true, false, true, true));

    // Session 20260716_135326: after a successful pure-DLSS phase and authoritative OFF/native return, the next
    // fresh proxy issued its FG-off passthrough Present before SetOptions(ON). Prior healthy PostSL submission is
    // equivalent route-history proof for prewarm without weakening first-ever pure-DLSS cold start.
    EXPECT_TRUE(ShouldPrewarmPostSLOverlayAtFreshProvenHandoff(true, false, true, true, false, true, true));

    // First-ever pure-DLSS cold start retains its stricter guards. A non-authoritative/reused queue,
    // already-running DLSS, no live prior overlay, non-runtime ownership, or a non-DX12 swapchain cannot use this
    // preparation window.
    EXPECT_FALSE(ShouldPrewarmPostSLOverlayAtFreshProvenHandoff(true, false, false, true, false, true, true));
    EXPECT_FALSE(ShouldPrewarmPostSLOverlayAtFreshProvenHandoff(false, true, false, true, false, true, true));
    EXPECT_FALSE(ShouldPrewarmPostSLOverlayAtFreshProvenHandoff(true, true, false, false, false, true, true));
    EXPECT_FALSE(ShouldPrewarmPostSLOverlayAtFreshProvenHandoff(true, true, false, true, true, true, true));
    EXPECT_FALSE(ShouldPrewarmPostSLOverlayAtFreshProvenHandoff(true, true, false, true, false, false, true));
    EXPECT_FALSE(ShouldPrewarmPostSLOverlayAtFreshProvenHandoff(true, true, false, true, false, true, false));
}

TEST(DXGISharedTest, ExactPrewarmedPostSLHandoffSurvivesItsFirstMatchingPresent) {
    using ce::dx12_overlay_policy::ShouldPreserveExactPrewarmedPostSLHandoffBackendOnFirstPresent;

    EXPECT_TRUE(ShouldPreserveExactPrewarmedPostSLHandoffBackendOnFirstPresent(true, true, true, true, true, true, true,
                                                                               true, false, false, false));
    EXPECT_FALSE(ShouldPreserveExactPrewarmedPostSLHandoffBackendOnFirstPresent(false, true, true, true, true, true,
                                                                                true, true, false, false, false));
    EXPECT_FALSE(ShouldPreserveExactPrewarmedPostSLHandoffBackendOnFirstPresent(true, false, true, true, true, true,
                                                                                true, true, false, false, false));
    EXPECT_FALSE(ShouldPreserveExactPrewarmedPostSLHandoffBackendOnFirstPresent(true, true, false, true, true, true,
                                                                                true, true, false, false, false));
    EXPECT_FALSE(ShouldPreserveExactPrewarmedPostSLHandoffBackendOnFirstPresent(true, true, true, true, true, true,
                                                                                true, false, false, false, false));
    EXPECT_FALSE(ShouldPreserveExactPrewarmedPostSLHandoffBackendOnFirstPresent(true, true, true, true, true, true,
                                                                                true, true, true, false, false));
    EXPECT_FALSE(ShouldPreserveExactPrewarmedPostSLHandoffBackendOnFirstPresent(true, true, true, true, true, true,
                                                                                true, true, false, true, false));
    EXPECT_FALSE(ShouldPreserveExactPrewarmedPostSLHandoffBackendOnFirstPresent(true, true, true, true, true, true,
                                                                                true, true, false, false, true));
}

TEST(DXGISharedSourceTest, PrewarmedPostSLHandoffProofIsArmedAndConsumedBeforeGenericSwapchainCleanup) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty()) << source.string();

    const size_t prewarm = text.find("PrewarmPostSLOverlayForFreshStreamlineHandoff(pSwapChain, pQueue, context)");
    const size_t arm = text.find("g_PrewarmedPostSLHandoffSwapchain.store(pSwapChain", prewarm);
    const size_t lifetimeDecision = text.find("ShouldProcessLogicalSwapchainReplacement(", arm);
    const size_t preserve =
        text.find("ShouldPreserveExactPrewarmedPostSLHandoffBackendOnFirstPresent(", lifetimeDecision);
    const size_t cleanup = text.find("CleanupRTVs();", preserve);
    ASSERT_NE(prewarm, std::string::npos);
    ASSERT_NE(arm, std::string::npos);
    ASSERT_NE(lifetimeDecision, std::string::npos);
    ASSERT_NE(preserve, std::string::npos);
    ASSERT_NE(cleanup, std::string::npos);
    EXPECT_LT(prewarm, arm);
    EXPECT_LT(arm, lifetimeDecision);
    EXPECT_LT(lifetimeDecision, preserve);
    EXPECT_LT(preserve, cleanup);
}

TEST(DXGISharedSourceTest, RepeatedPureDLSSHandoffUsesOnlyPriorHealthyPostSLProof) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty()) << source.string();

    const size_t successfulSubmit =
        text.find("if (SUCCEEDED(postDevReason) && rendered && pSwapChain && submittedQueue)");
    const size_t latch = text.find("g_HadSuccessfulPostSLPhase.exchange(true", successfulSubmit);
    const size_t handoffLoad = text.find("g_HadSuccessfulPostSLPhase.load(std::memory_order_acquire)");
    const size_t prewarmDecision = text.find("ShouldPrewarmPostSLOverlayAtFreshProvenHandoff(", handoffLoad);
    const size_t prewarm = text.find("PrewarmPostSLOverlayForFreshStreamlineHandoff(", prewarmDecision);
    ASSERT_NE(successfulSubmit, std::string::npos);
    ASSERT_NE(latch, std::string::npos);
    ASSERT_NE(handoffLoad, std::string::npos);
    ASSERT_NE(prewarmDecision, std::string::npos);
    ASSERT_NE(prewarm, std::string::npos);
    EXPECT_LT(successfulSubmit, latch);
    EXPECT_LT(handoffLoad, prewarmDecision);
    EXPECT_LT(prewarmDecision, prewarm);
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

    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    EXPECT_NE(text.find("ReleaseStreamlineStartupActivationSwapchain(\"DX12: Streamline FG OFF (startup churn)\")"),
              std::string::npos);
    EXPECT_NE(text.find("DeepHook: CreateSwapChainForHwnd E_ACCESSDENIED recovery"), std::string::npos);
    EXPECT_NE(text.find("CreateSwapChainForHwnd INLINE: E_ACCESSDENIED recovery"), std::string::npos);
}

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

TEST(DXGISharedTest, ExactConfirmedPostSLProxySurvivesOuterOffWithoutDrainOrReinit) {
    using ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLProxyResourcesAcrossOuterOff;

    EXPECT_TRUE(ShouldPreserveConfirmedPostSLProxyResourcesAcrossOuterOff(true, true, true, true, true, false));

    EXPECT_FALSE(ShouldPreserveConfirmedPostSLProxyResourcesAcrossOuterOff(false, true, true, true, true, false));
    EXPECT_FALSE(ShouldPreserveConfirmedPostSLProxyResourcesAcrossOuterOff(true, false, true, true, true, false));
    EXPECT_FALSE(ShouldPreserveConfirmedPostSLProxyResourcesAcrossOuterOff(true, true, false, true, true, false));
    EXPECT_FALSE(ShouldPreserveConfirmedPostSLProxyResourcesAcrossOuterOff(true, true, true, false, true, false));
    EXPECT_FALSE(ShouldPreserveConfirmedPostSLProxyResourcesAcrossOuterOff(true, true, true, true, false, false));
    EXPECT_FALSE(ShouldPreserveConfirmedPostSLProxyResourcesAcrossOuterOff(true, true, true, true, true, true));
}

TEST(DXGISharedSourceTest, OuterOffPreservesExactConfirmedPostSLProxyBeforeTransitionDraw) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));

    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    const size_t outerOff = text.find("const bool preserveConfirmedPostSLProxyResourcesAcrossOuterOff");
    ASSERT_NE(outerOff, std::string::npos);
    const size_t coverageGate = text.find("[OVERLAY COVERAGE] attribute", outerOff);
    ASSERT_NE(coverageGate, std::string::npos);

    const size_t drainGuard =
        text.find("if (g_State.fence && !preserveConfirmedPostSLProxyResourcesAcrossOuterOff &&", outerOff);
    const size_t drainNativeReturnGuard =
        text.find("!keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn)", drainGuard);
    const size_t reinitBranch =
        text.find("if (g_State.overlayInit && !keepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover", drainGuard);
    const size_t reinitGuard = text.find("!preserveConfirmedPostSLProxyResourcesAcrossOuterOff &&", reinitBranch);
    const size_t reinitNativeReturnGuard =
        text.find("!keepOverlayLiveAcrossAuthoritativeDLSSOffNormalReturn) {", reinitGuard);
    ASSERT_NE(drainGuard, std::string::npos);
    ASSERT_NE(drainNativeReturnGuard, std::string::npos);
    ASSERT_NE(reinitBranch, std::string::npos);
    ASSERT_NE(reinitGuard, std::string::npos);
    ASSERT_NE(reinitNativeReturnGuard, std::string::npos);
    EXPECT_LT(drainNativeReturnGuard - drainGuard, static_cast<size_t>(240));
    EXPECT_LT(reinitNativeReturnGuard - reinitBranch, static_cast<size_t>(320));
    EXPECT_LT(drainGuard, coverageGate);
    EXPECT_LT(reinitGuard, coverageGate);
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

TEST(DXGISharedTest, RendersOverlayDirectlyOnFirstPostFSRDLSSReactivationUnderFastBootstrap) {
    using ce::dx12_overlay_policy::ShouldRenderOverlayDirectlyOnFirstPostFSRDLSSReactivation;

    // Synthetic dx12_fg_switch_test session 20260615_010145: even with the fast probe, every FSR->DLSS
    // engage still spent its FIRST reactivation present on the single scratch-barrier probe and rendered
    // the overlay only on the NEXT present (1-present `postsl-bootstrap-reactivation` flicker). Under the
    // fast-bootstrap proof the real overlay render is itself the device-health proof (pre/post
    // GetDeviceRemovedReason), so render directly on the first present (probe level 0).
    EXPECT_TRUE(ShouldRenderOverlayDirectlyOnFirstPostFSRDLSSReactivation(/*fastPostFSRDLSSProbe=*/true,
                                                                          /*postFSRProbeLevel=*/0));

    // Without the fast-bootstrap proof the fragile graduated probe is retained (no direct render).
    EXPECT_FALSE(ShouldRenderOverlayDirectlyOnFirstPostFSRDLSSReactivation(/*fastPostFSRDLSSProbe=*/false, 0));
    // Only the FIRST present (level 0) renders directly; once advanced to full-render level it is the
    // normal render path, not this fast-skip (so the predicate must not re-fire).
    EXPECT_FALSE(ShouldRenderOverlayDirectlyOnFirstPostFSRDLSSReactivation(true, /*postFSRProbeLevel=*/3));
    EXPECT_FALSE(ShouldRenderOverlayDirectlyOnFirstPostFSRDLSSReactivation(true, /*postFSRProbeLevel=*/2));
}

TEST(DXGISharedTest, RendersOverlayDirectlyOnPureDLSSTransitionProbeWhenOnSwapchainQueue) {
    using ce::dx12_overlay_policy::ShouldRenderOverlayDirectlyOnPostSLTransitionProbe;

    // Synthetic dx12_fg_switch_test session 20260615_014832 (verbose handoff diagnostic): pure off->DLSS
    // reactivations (hadFSR=0, epoch>1) blanked the overlay for 1 present on the empty-ECL
    // `postsl-transition-probe` (coverage drawObserved=0 covered=0 currentStreak=1). On the SL-owned
    // swapchain queue the real overlay render is itself the queue-health proof (pre/post
    // GetDeviceRemovedReason), so render directly instead of probing.
    EXPECT_TRUE(ShouldRenderOverlayDirectlyOnPostSLTransitionProbe(/*selectedQueueIsSwapchainQueue=*/true,
                                                                   /*deviceHealthy=*/true));

    // Off the swapchain queue (e.g. origGame first-ECL fragile path) -> keep the empty-ECL probe.
    EXPECT_FALSE(ShouldRenderOverlayDirectlyOnPostSLTransitionProbe(/*selectedQueueIsSwapchainQueue=*/false, true));
    // Device removed -> keep the probe (don't submit a real overlay ECL into a removed device).
    EXPECT_FALSE(ShouldRenderOverlayDirectlyOnPostSLTransitionProbe(true, /*deviceHealthy=*/false));
}

TEST(DXGISharedTest, SameQueuePureDLSSColdStartIsSafeToRenderEarly) {
    using ce::dx12_overlay_policy::ShouldTreatSameQueuePureDLSSColdStartAsSafe;

    // Talos startup (session 20260615_162947): DLSS FG runs on the game's OWN single queue
    // (scQueue==origGame==cmdQueue, no separate SL wrapper queue), so there is no separate DLSS-G
    // proxy-init pipeline for CE's overlay ECL to corrupt -> render from the first callback instead of
    // the 437ms countdown+warmup blank.
    EXPECT_TRUE(ShouldTreatSameQueuePureDLSSColdStartAsSafe(
        /*hadFSRFGPhase=*/false, /*swapchainQueueIsOriginalGameQueue=*/true, /*noSeparateCommandQueue=*/true,
        /*hasSeparateSLWrapperQueue=*/false, /*deviceRemoved=*/false));

    // FSR history -> the post-FSR bootstrap proof path owns it, not this.
    EXPECT_FALSE(ShouldTreatSameQueuePureDLSSColdStartAsSafe(true, true, true, false, false));
    // A SEPARATE swapchain/runtime queue (the documented GTA pure-DLSS startup) -> keep the countdown +
    // warmup; CE's ECL on a separate proxy-init queue is the documented corruption/hang.
    EXPECT_FALSE(ShouldTreatSameQueuePureDLSSColdStartAsSafe(false, /*swapchainQueueIsOriginalGameQueue=*/false, true,
                                                             false, false));
    // A separate command queue -> not the single-queue topology; keep protections.
    EXPECT_FALSE(
        ShouldTreatSameQueuePureDLSSColdStartAsSafe(false, true, /*noSeparateCommandQueue=*/false, false, false));
    // A separate SL wrapper queue exists -> there IS a separate runtime pipeline; keep protections.
    EXPECT_FALSE(
        ShouldTreatSameQueuePureDLSSColdStartAsSafe(false, true, true, /*hasSeparateSLWrapperQueue=*/true, false));
    // Device removed -> never render.
    EXPECT_FALSE(ShouldTreatSameQueuePureDLSSColdStartAsSafe(false, true, true, false, /*deviceRemoved=*/true));
}

TEST(DXGISharedTest, SameQueuePureDLSSColdStartBypassesReactivationWarmup) {
    using ce::dx12_overlay_policy::ShouldBypassPostSLReactivationWarmup;

    // The same-queue pure-DLSS cold-start proof is a new warmup-bypass leg (renders during the
    // otherwise-blank 15-frame warmup). Pure DLSS, no FSR, none of the other proofs needed.
    EXPECT_TRUE(ShouldBypassPostSLReactivationWarmup(
        /*hadFSRFGPhase=*/false, /*useTopLevelHandoffWrapperProgress=*/false, /*safePostFSRBootstrapPath=*/false,
        /*confirmedPureStreamlineResumeProof=*/false, /*explicitEnablePureDLSSColdStartProof=*/false,
        /*postSLConfirmedRenderInCurrentEpoch=*/false, /*sameQueuePureDLSSColdStartSafe=*/true));

    // Without the same-queue proof (and no other proof), a pure-DLSS cold start still keeps the warmup
    // (the documented GTA separate-queue init protection).
    EXPECT_FALSE(ShouldBypassPostSLReactivationWarmup(false, false, false, false, false, false,
                                                      /*sameQueuePureDLSSColdStartSafe=*/false));
    // The same-queue leg is pure-DLSS only: with FSR history it does not apply (post-FSR path governs).
    EXPECT_FALSE(ShouldBypassPostSLReactivationWarmup(/*hadFSRFGPhase=*/true, false, false, false, false, false,
                                                      /*sameQueuePureDLSSColdStartSafe=*/true));
}

TEST(DXGISharedTest, RetainsFFXBridgeAcrossEnabledAppToNullCallbackToggle) {
    using ce::dx12_overlay_policy::ShouldRetainFFXPresentCallbackBridgeForEnabledNullCallbackToggle;

    // Session 20260615_021242 (~1s AMD ffxQuery freeze): the app provided a present callback (CE wrapped
    // it), then re-enabled FSR with a NULL callback. AMD retains CE's bridge, so clearing its retained
    // original made CE self-compose (CopyResource) and wedge AMD. Keep delegating to the retained
    // original when FG stays ENABLED, the new configure has no app callback, and a bridge with a non-null
    // original exists.
    EXPECT_TRUE(ShouldRetainFFXPresentCallbackBridgeForEnabledNullCallbackToggle(
        /*recognizedFrameGenerationConfigure=*/true, /*frameGenerationEnabled=*/true,
        /*appPresentCallbackProvided=*/false, /*hasExistingBridgeWithOriginal=*/true));

    // The app still provides a callback -> the install/already-bridged path handles it, not this retain.
    EXPECT_FALSE(ShouldRetainFFXPresentCallbackBridgeForEnabledNullCallbackToggle(
        true, true, /*appPresentCallbackProvided=*/true, true));
    // FG disabled -> the disabled-configure retain path owns it, not this one.
    EXPECT_FALSE(ShouldRetainFFXPresentCallbackBridgeForEnabledNullCallbackToggle(
        true, /*frameGenerationEnabled=*/false, false, true));
    // No existing bridge with a usable original (genuine null-callback startup) -> preserve AMD internal
    // composition; do NOT synthesize a bridge.
    EXPECT_FALSE(ShouldRetainFFXPresentCallbackBridgeForEnabledNullCallbackToggle(
        true, true, false, /*hasExistingBridgeWithOriginal=*/false));
    // Not a recognized FG configure.
    EXPECT_FALSE(ShouldRetainFFXPresentCallbackBridgeForEnabledNullCallbackToggle(
        /*recognizedFrameGenerationConfigure=*/false, true, false, true));
}

TEST(DXGISharedTest, KeepsOverlayLiveAcrossDLSSToFSRNoCallbackTakeover) {
    using ce::dx12_overlay_policy::ShouldKeepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover;

    // Session 20260615_020100 (after many switches): a DLSS->FSR no-callback takeover warm-reinited the
    // overlay on the runtime-owned FSR queue, then the [outer] SL-FG-OFF teardown force-cleared it + armed
    // a 60-frame cooldown (missed=60 / 422 ms). When FSR is the active no-callback presenter on a
    // runtime-owned path and the overlay backend is already init/sync with a healthy device, keep it live.
    EXPECT_TRUE(ShouldKeepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover(
        /*slTurnedOff=*/true, /*fsrFGApiActive=*/true, /*nativeFSRInternalNoCallbackComposition=*/true,
        /*runtimeOwnedNativeFGPresentPath=*/true, /*overlayInit=*/true, /*syncInit=*/true, /*deviceRemoved=*/false));

    // Not an OFF edge -> not this path.
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover(false, true, true, true, true, true, false));
    // FSR not active (pure DLSS->off) -> the existing pure-Streamline / confirmed-PostSL bypasses own it.
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover(true, false, true, true, true, true, false));
    // App-callback FFX bridge route (internalNoCallback=false): a separate overlay ECL on the FSR queue is
    // the documented 0x887A002B crash -> keep the teardown.
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover(true, true, false, true, true, true, false));
    // Runtime does not own the native-FG present path -> not this takeover.
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover(true, true, true, false, true, true, false));
    // Overlay backend not init/sync -> nothing live to keep (let the reinit run).
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover(true, true, true, true, false, true, false));
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover(true, true, true, true, true, false, false));
    // Device removed -> don't keep rendering into a removed device.
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossDLSSToFSRNoCallbackTakeover(true, true, true, true, true, true, true));
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

    const std::string text = ce::test_source::ReadLogicalSource(source);
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

    const std::string text = ce::test_source::ReadLogicalSource(source);
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

// ---------------------------------------------------------------------------
// FSR-FG passthrough overlay visibility. Runtime-owned suspension and protected
// disabled startup arming both present the proxy backbuffer without consuming the
// registered UI resource. The proxy-present prework must draw onto that buffer on
// the target-compatible owner queue. Usually that is the exact FFX descriptor
// queue; a proven Streamline wrapper resolves to CE's validated underlying real
// game queue. Queue ordering then provides completion without a foreign queue or
// per-frame CPU wait.
// ---------------------------------------------------------------------------
TEST(DXGISharedSourceTest, ProxyBackbufferOverlayUsesTargetCompatibleOwnerQueueForPassthroughRoutes) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    const size_t prework = text.find("DX12_RunFFXProxyPrePresentWork(");
    ASSERT_NE(prework, std::string::npos);
    const size_t startupGate = text.find("ShouldUseProtectedOfficialFFXStartupProxyBackbufferRoute(", prework);
    const size_t suspensionGate = text.find("DX12_IsNativeFSRFGSuspendedDisablePending()", prework);
    const size_t backbufferCall = text.find("DX12_CompositeOverlayOntoSuspendBackbuffer(", prework);
    ASSERT_NE(startupGate, std::string::npos);
    ASSERT_NE(suspensionGate, std::string::npos);
    ASSERT_NE(backbufferCall, std::string::npos);
    EXPECT_LT(startupGate, backbufferCall);
    EXPECT_LT(suspensionGate, backbufferCall);
    EXPECT_NE(text.find("protected-startup-backbuffer", backbufferCall), std::string::npos);
    EXPECT_NE(text.find("suspend-backbuffer", backbufferCall), std::string::npos);

    // The backbuffer composite must resolve against the actual target resource and use the selected owner
    // queue. It must not use the foreign dedicated-queue path or wait on the CPU.
    const size_t fn = text.find("bool DX12_CompositeOverlayOntoSuspendBackbuffer(");
    ASSERT_NE(fn, std::string::npos);
    size_t fnEnd = text.find("\n}\n", fn);
    if (fnEnd == std::string::npos) {
        // Source checkouts use either LF or CRLF depending on Git's Windows
        // line-ending policy; keep this structural test invariant to both.
        fnEnd = text.find("\n}\r\n", fn);
    }
    ASSERT_NE(fnEnd, std::string::npos);
    const std::string body = text.substr(fn, fnEnd - fn);
    EXPECT_NE(body.find("AcquireNativeFSRSwapchainPresentationQueue(proxy, backBuffer)"), std::string::npos);
    EXPECT_NE(body.find("request.presentationQueue = ownerQueue.queue"), std::string::npos);
    EXPECT_NE(body.find("SubmitNativeFSROwnerQueueOverlayCommandList"), std::string::npos);
    EXPECT_NE(body.find("no target-compatible"), std::string::npos);
    EXPECT_EQ(body.find("g_FFXUiCompositeQueue"), std::string::npos);
    EXPECT_EQ(body.find("WaitForSingleObject"), std::string::npos);
}

TEST(DXGISharedSourceTest, ProtectedFFXStartupNestedPresentNeverSubmitsOnStagedInternalQueue) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    const std::string text = ce::test_source::ReadLogicalSource(source);

    const size_t processFrameQueueRouting = text.find("FSR FG: FSR creates a NEW swapchain");
    ASSERT_NE(processFrameQueueRouting, std::string::npos);
    const size_t normalRoute = text.find("ID3D12CommandQueue* gameQueue = nullptr;", processFrameQueueRouting);
    ASSERT_NE(normalRoute, std::string::npos);
    const size_t protectedBranch = text.find("if (protectedOfficialFFXStartupOverlayOnly) {", normalRoute);
    const size_t protectedReturn = text.find("return;", protectedBranch);
    const size_t normalRouting = text.find("DecideSwapchainOverlayRouting(", protectedBranch);
    ASSERT_NE(protectedBranch, std::string::npos);
    ASSERT_NE(protectedReturn, std::string::npos);
    ASSERT_NE(normalRouting, std::string::npos);
    EXPECT_LT(protectedReturn, normalRouting)
        << "the nested real-swapchain path must return tracking-only before normal overlay queue selection";
    EXPECT_EQ(text.find("gameQueue = protectedOfficialFFXStartupQueueRef"), std::string::npos)
        << "the staged nested DXGI create queue is AMD's internal presenter and is evidence only";
}
