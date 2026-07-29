#include "test_dxgi_shared_shared.h"

TEST(DXGISharedTest, DX12PrerenderLimiterRunsOnlyOnProvenSourcePresentsDuringFrameGeneration) {
    using ce::dx12_overlay_policy::ShouldApplyDX12PrerenderLimitOnPresent;

    EXPECT_TRUE(ShouldApplyDX12PrerenderLimitOnPresent(false, 0, 0x1604));
    EXPECT_TRUE(ShouldApplyDX12PrerenderLimitOnPresent(false, 0x5444, 0x1604));

    EXPECT_FALSE(ShouldApplyDX12PrerenderLimitOnPresent(true, 0, 0x1604));
    EXPECT_FALSE(ShouldApplyDX12PrerenderLimitOnPresent(true, 0x5444, 0x1604));
    EXPECT_TRUE(ShouldApplyDX12PrerenderLimitOnPresent(true, 0x5444, 0x5444));
}

TEST(DXGISharedSourceTest, DX12PrerenderLimiterPinsTheGameQueueAndRejectsRuntimeGeneratedPresents) {
    namespace fs = std::filesystem;
    const fs::path dx12Source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    const fs::path dxgiSource = fs::current_path() / "hook" / "common" / "dxgi_shared.cpp";
    ASSERT_TRUE(fs::exists(dx12Source));
    ASSERT_TRUE(fs::exists(dxgiSource));
    const std::string dx12Text = ce::test_source::ReadLogicalSource(dx12Source);
    const std::string dxgiText = ce::test_source::ReadLogicalSource(dxgiSource);
    ASSERT_FALSE(dx12Text.empty());
    ASSERT_FALSE(dxgiText.empty());

    const size_t prerenderContext = dx12Text.find("static DX12Context GetDX12PrerenderContext(");
    const size_t originalQueueSelection =
        dx12Text.find("preferOriginalGameQueue && g_OriginalGameQueue != nullptr", prerenderContext);
    const size_t selectedQueueDevice =
        dx12Text.find("selectedQueue->GetDevice(IID_PPV_ARGS(&queueDevice))", originalQueueSelection);
    const size_t limiter =
        dx12Text.find("static void ApplyPrerenderLimitDX12(float limit, bool frameGenerationPresentationActive)");
    const size_t sourcePresentGate =
        dx12Text.find("if (prerenderLimit >= 0.0f && applicationSourcePresent)", limiter);
    ASSERT_NE(prerenderContext, std::string::npos);
    ASSERT_NE(originalQueueSelection, std::string::npos);
    ASSERT_NE(selectedQueueDevice, std::string::npos);
    ASSERT_NE(limiter, std::string::npos);
    ASSERT_NE(sourcePresentGate, std::string::npos);

    const size_t minimalProcessFrame = dx12Text.find("void DX12_ProcessFrameMinimal(");
    const size_t minimalForward =
        dx12Text.find(
            "ProcessFrame(sc3, processCapture, applicationSourcePresent, frameGenerationPresentationActive);",
            minimalProcessFrame);
    const size_t externalProcessFrame = dx12Text.find("static void DX12_ProcessFrameExternal(", minimalForward);
    const size_t externalForward =
        dx12Text.find(
            "ProcessFrame(sc3, processCapture, applicationSourcePresent, frameGenerationPresentationActive, "
            "diagnostics);",
            externalProcessFrame);
    const size_t wrapperEntry =
        dx12Text.find("void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain) {", externalForward);
    const size_t wrapperClassification =
        dx12Text.find("ShouldApplyDX12PrerenderLimitOnPresent(", wrapperEntry);
    ASSERT_NE(minimalProcessFrame, std::string::npos);
    ASSERT_NE(minimalForward, std::string::npos);
    ASSERT_NE(externalProcessFrame, std::string::npos);
    ASSERT_NE(externalForward, std::string::npos);
    ASSERT_NE(wrapperEntry, std::string::npos);
    ASSERT_NE(wrapperClassification, std::string::npos);

    const size_t handler = dx12Text.find("void HandleDX12ProcessFrame(");
    const size_t handlerForward =
        dx12Text.find(
            "DX12_ProcessFrameExternalForPresent(pSwapChain, applicationSourcePresent, "
            "frameGenerationPresentationActive);",
            handler);
    ASSERT_NE(handler, std::string::npos);
    ASSERT_NE(handlerForward, std::string::npos);

    const size_t eagerStartup = dxgiText.find("MaybeEagerDrawOverlayBeforeStreamlineStartupBypass(");
    const size_t eagerReject = dxgiText.find("HandleDX12ProcessFrame(pSwapChain, false, true);", eagerStartup);
    const size_t present = dxgiText.find("HRESULT STDMETHODCALLTYPE DetourPresent(");
    const size_t presentClassification =
        dxgiText.find("ShouldApplyDX12PrerenderLimitOnPresent(", present);
    const size_t presentFFXProvenance =
        dxgiText.rfind("callerFromFFXFrameGenerationModule || HookHasRuntimeOwnedNativeFGPresentPath()",
                       presentClassification);
    const size_t minimalForwardFromPresent =
        dxgiText.find("DX12_ProcessFrameMinimal(pSwapChain, applicationSourcePresent,",
                      presentClassification);
    const size_t present1 = dxgiText.find("HRESULT STDMETHODCALLTYPE DetourPresent1(");
    const size_t present1Classification =
        dxgiText.find("ShouldApplyDX12PrerenderLimitOnPresent(", present1);
    ASSERT_NE(eagerStartup, std::string::npos);
    ASSERT_NE(eagerReject, std::string::npos);
    ASSERT_NE(present, std::string::npos);
    ASSERT_NE(presentClassification, std::string::npos);
    ASSERT_NE(presentFFXProvenance, std::string::npos);
    ASSERT_NE(minimalForwardFromPresent, std::string::npos);
    ASSERT_NE(present1, std::string::npos);
    ASSERT_NE(present1Classification, std::string::npos);
}

// GTA session 20260714_140617 proved the protected inner DXGI create queue is FFX's newly-created internal
// presentQueue, not the descriptor gameQueue. It proves the missed-create topology but must never become CE's
// overlay owner binding. Recovery uses the retained original game/producer queue before the proxy hook is live.
TEST(DXGISharedSourceTest, ProtectedCreateQueueRecoveryPrecedesFFXProxyPresentHookInstallation) {
    namespace fs = std::filesystem;
    const fs::path ffxSource = fs::current_path() / "hook" / "apis" / "ffx_hook.cpp";
    const fs::path dx12Source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(ffxSource));
    ASSERT_TRUE(fs::exists(dx12Source));
    const std::string ffxText = ce::test_source::ReadLogicalSource(ffxSource);
    const std::string dx12Text = ce::test_source::ReadLogicalSource(dx12Source);
    ASSERT_FALSE(ffxText.empty());
    ASSERT_FALSE(dx12Text.empty());

    const size_t createBlock = ffxText.find("if (parsedSwapChainCreate.recognized && context && *context");
    ASSERT_NE(createBlock, std::string::npos);
    const size_t createQueueRegistration =
        ffxText.find("DX12_RegisterNativeFSRSwapchainPresentationQueue(", createBlock);
    const size_t createHookInstall =
        ffxText.find("DX12_TryInstallFFXProxyPresentHook(*parsedSwapChainCreate.swapChainOutput", createBlock);
    ASSERT_NE(createQueueRegistration, std::string::npos);
    ASSERT_NE(createHookInstall, std::string::npos);
    EXPECT_LT(createQueueRegistration, createHookInstall)
        << "the exact descriptor producer queue must be registered before the first proxy Present can run";

    const size_t configureBlock = ffxText.find("if (recognizedFGConfigure && localConfig.swapChain) {");
    ASSERT_NE(configureBlock, std::string::npos);
    const size_t recovery = ffxText.find(
        "DX12_TryRecoverNativeFSRSwapchainPresentationQueue(contextHandle, localConfig.swapChain)", configureBlock);
    const size_t hookInstall = ffxText.find("DX12_TryInstallFFXProxyPresentHook(localConfig.swapChain", configureBlock);
    ASSERT_NE(recovery, std::string::npos);
    ASSERT_NE(hookInstall, std::string::npos);
    EXPECT_LT(recovery, hookInstall);

    const size_t recoveryFunction =
        dx12Text.find("bool DX12_TryRecoverNativeFSRSwapchainPresentationQueue(void* context, void* swapChain)");
    ASSERT_NE(recoveryFunction, std::string::npos);
    size_t recoveryFunctionEnd = dx12Text.find("\n}\n", recoveryFunction);
    if (recoveryFunctionEnd == std::string::npos) {
        recoveryFunctionEnd = dx12Text.find("\n}\r\n", recoveryFunction);
    }
    ASSERT_NE(recoveryFunctionEnd, std::string::npos);
    const std::string recoveryBody = dx12Text.substr(recoveryFunction, recoveryFunctionEnd - recoveryFunction);
    EXPECT_NE(recoveryBody.find("ReferenceDeferredOfficialFFXTakeoverQueue()"), std::string::npos);
    EXPECT_NE(recoveryBody.find("DX12_AcquireOriginalGameQueueForOverlay()"), std::string::npos);
    EXPECT_NE(recoveryBody.find("originalGameQueue, true, true, protectedInnerPresentQueue != nullptr"),
              std::string::npos);
    EXPECT_EQ(recoveryBody.find("context, swapChain, protectedInnerPresentQueue, true"), std::string::npos);
}

// ---------------------------------------------------------------------------
// No-callback FSR FG overlay routing — CRASH REGRESSION (session 20260621_191028,
// amd_fidelityfx_dx12!ffxQuery null-deref AV). When AMD owns the swapchain
// (runtime-owned native FSR FG) CE must NEVER submit overlay work on AMD's
// backbuffer/runtime queue: the route selector must ALWAYS skip the backbuffer
// ProcessFrame regardless of bundle-firing state, so the overlay rides AMD's
// UI-resource composition only. Iteration 1 selected kMinimalBackbuffer when the
// bundle was not firing, which submitted on AMD's queue and crashed GTA.
// ---------------------------------------------------------------------------
TEST(DXGISharedTest, NoCallbackFSRFGOverlayRouteNeverSubmitsBackbufferWhenRuntimeOwnsSwapchain) {
    using ce::dx12_overlay_policy::ChooseNoCallbackFSRFGOverlayRoute;
    using ce::dx12_overlay_policy::NoCallbackFSRFGOverlayRoute;

    // ACTIVELY INTERPOLATING: runtime-owned, NOT suspended, live present on AMD's SEPARATE FG queue (live
    // queue != origGame). ALWAYS skip the backbuffer submit, in EVERY bundle state. These four cases are the
    // exact crash boundary — the backbuffer submit must never be chosen while AMD is interpolating.
    // Args: (runtimeOwns, liveQueueIsOrigGame, fsrFGDisabledSuspendPending, cached, firing).
    for (bool cached : {false, true}) {
        for (bool firing : {false, true}) {
            EXPECT_EQ(
                static_cast<int>(ChooseNoCallbackFSRFGOverlayRoute(
                    /*runtimeOwns=*/true, /*liveQueueIsOrigGame=*/false, /*suspendPending=*/false, cached, firing)),
                static_cast<int>(NoCallbackFSRFGOverlayRoute::kSkipBundleCovers))
                << "actively interpolating on AMD's FG queue must never submit on AMD's queue (cached=" << cached
                << " firing=" << firing << ")";
        }
    }

    // NO-CALLBACK SUSPENSION (session 20260703_210021 — REVISED): runtime-owned but FG explicitly disabled
    // (AMD keeps the swapchain, not interpolating). The backbuffer submit is NOT safe here after all — AMD
    // stops flushing its runtime queue while suspended, so the overlay's GPU-completion fence never signals and
    // DetourPresent stalls ~1s EVERY present (app collapses to ~1 fps). So while AMD owns the swapchain the
    // route is ALWAYS kSkipBundleCovers (bundle composite on CE's own fenced queue), in EVERY bundle state,
    // regardless of suspend — NEVER the backbuffer. (Supersedes the earlier "suspension backbuffer is safe"
    // assumption from session 20260623_054929.)
    for (bool cached : {false, true}) {
        for (bool firing : {false, true}) {
            EXPECT_EQ(
                static_cast<int>(ChooseNoCallbackFSRFGOverlayRoute(
                    /*runtimeOwns=*/true, /*liveQueueIsOrigGame=*/false, /*suspendPending=*/true, cached, firing)),
                static_cast<int>(NoCallbackFSRFGOverlayRoute::kSkipBundleCovers))
                << "runtime-owned suspension must ride the bundle, never the (stalling) backbuffer (cached=" << cached
                << " firing=" << firing << ")";
        }
    }

    // STALE-LATCH RECOVERY (FSR->off): the no-callback latch is still set but the game recreated a native
    // swapchain and presents on its OWN queue again (live queue == origGame). AMD's FG swapchain is gone, the
    // bundle is invisible, and the backbuffer route is safe. Must draw via the backbuffer in EVERY bundle
    // state — even though runtimeOwns is still latched true and regardless of suspend-pending.
    for (bool suspend : {false, true}) {
        for (bool cached : {false, true}) {
            for (bool firing : {false, true}) {
                EXPECT_EQ(static_cast<int>(ChooseNoCallbackFSRFGOverlayRoute(
                              /*runtimeOwns=*/true, /*liveQueueIsOrigGame=*/true, suspend, cached, firing)),
                          static_cast<int>(NoCallbackFSRFGOverlayRoute::kMinimalBackbuffer))
                    << "stale latch with live present back on origGame must draw via backbuffer";
            }
        }
    }

    // Non-runtime-owned (AMD does NOT own the swapchain — safe to submit on the backbuffer):
    // bundle cached AND actively firing -> safe to skip (bundle composites the overlay).
    EXPECT_EQ(static_cast<int>(ChooseNoCallbackFSRFGOverlayRoute(/*runtimeOwns=*/false, /*liveQueueIsOrigGame=*/false,
                                                                 /*suspendPending=*/false, /*cached=*/true,
                                                                 /*firing=*/true)),
              static_cast<int>(NoCallbackFSRFGOverlayRoute::kSkipBundleCovers));
    // Cached but NOT firing, or not cached -> draw via the safe minimal backbuffer path (never blank).
    EXPECT_EQ(static_cast<int>(ChooseNoCallbackFSRFGOverlayRoute(/*runtimeOwns=*/false, /*liveQueueIsOrigGame=*/false,
                                                                 /*suspendPending=*/false, /*cached=*/true,
                                                                 /*firing=*/false)),
              static_cast<int>(NoCallbackFSRFGOverlayRoute::kMinimalBackbuffer));
    EXPECT_EQ(static_cast<int>(ChooseNoCallbackFSRFGOverlayRoute(/*runtimeOwns=*/false, /*liveQueueIsOrigGame=*/false,
                                                                 /*suspendPending=*/false, /*cached=*/false,
                                                                 /*firing=*/false)),
              static_cast<int>(NoCallbackFSRFGOverlayRoute::kMinimalBackbuffer));
}

// ---------------------------------------------------------------------------
// FFX UI-resource overlay target selection (GTA 1x1 placeholder vs usable HUD
// texture). GTA Enhanced leaves UI composition enabled but registers a 1x1
// placeholder, so CE must substitute its own backbuffer-sized texture; games /
// the test app that register a usable full-size UI texture get the overlay
// blended directly onto it.
// ---------------------------------------------------------------------------
TEST(DXGISharedTest, FFXUiOverlayTargetSubstitutesForDegenerateGameTexture) {
    using ce::dx12_overlay_policy::ChooseFFXUiOverlayTarget;
    using ce::dx12_overlay_policy::FFXUiOverlayTarget;

    // GTA's 1x1 placeholder against a 4K backbuffer -> substitute CE's full-size texture.
    EXPECT_EQ(static_cast<int>(ChooseFFXUiOverlayTarget(/*texW=*/1, /*texH=*/1, /*bbW=*/3840, /*bbH=*/2160)),
              static_cast<int>(FFXUiOverlayTarget::kSubstituteCEFullSizeTexture));

    // A usable full-size UI texture (test app) -> composite onto it directly.
    EXPECT_EQ(static_cast<int>(ChooseFFXUiOverlayTarget(/*texW=*/3840, /*texH=*/2160, /*bbW=*/3840, /*bbH=*/2160)),
              static_cast<int>(FFXUiOverlayTarget::kCompositeOntoGameTexture));

    // A UI texture much smaller than the backbuffer (under half in a dimension) is a placeholder -> substitute.
    EXPECT_EQ(static_cast<int>(ChooseFFXUiOverlayTarget(/*texW=*/640, /*texH=*/360, /*bbW=*/3840, /*bbH=*/2160)),
              static_cast<int>(FFXUiOverlayTarget::kSubstituteCEFullSizeTexture));

    // Backbuffer size unknown (0): only the trivially-degenerate (<=1px) case substitutes; otherwise composite
    // (never substitute against a possibly-usable texture we cannot size-compare).
    EXPECT_EQ(static_cast<int>(ChooseFFXUiOverlayTarget(/*texW=*/1, /*texH=*/1, /*bbW=*/0, /*bbH=*/0)),
              static_cast<int>(FFXUiOverlayTarget::kSubstituteCEFullSizeTexture));
    EXPECT_EQ(static_cast<int>(ChooseFFXUiOverlayTarget(/*texW=*/2560, /*texH=*/1440, /*bbW=*/0, /*bbH=*/0)),
              static_cast<int>(FFXUiOverlayTarget::kCompositeOntoGameTexture));
}

// ---------------------------------------------------------------------------
// GTA crash fix (session 20260623_060436, nvwgf2umx null-deref inside
// ExecuteCommandLists): the no-callback FSR FG overlay must be driven by the
// FENCED CE-owned-queue composite, NOT the game-ECL bundle. The bundle reused
// its allocators with no GPU fence and corrupted an in-flight command list when
// GTA's startup churn fell >3 frames behind. DetourPresent's kSkipBundleCovers
// arm must call DX12_CompositeOverlayOntoCachedFFXUiResource(), and the retired
// bundle drive (TryAppendNoCallbackBundleOverlay / RecordBundleOverlayForGameECL)
// plus the dead DX12_ShouldCompositeOverlayOntoFFXUiResource gate must be gone.
// ---------------------------------------------------------------------------
TEST(DXGISharedSourceTest, NoCallbackPresentDrivesFencedCompositeNotBundle) {
    namespace fs = std::filesystem;

    auto readFile = [](const fs::path& p) {
        const std::string text = ce::test_source::ReadLogicalSource(p);
        EXPECT_FALSE(text.empty()) << p.string();
        return text;
    };

    const fs::path present = fs::current_path() / "hook" / "common" / "dxgi_shared.cpp";
    ASSERT_TRUE(fs::exists(present));
    const std::string presentText = readFile(present);
    ASSERT_FALSE(presentText.empty());

    // The no-callback present path must DRIVE the fenced composite (the per-present overlay refresh).
    EXPECT_NE(presentText.find("DX12_CompositeOverlayOntoCachedFFXUiResource()"), std::string::npos)
        << "DetourPresent must drive the fenced CE-owned-queue composite under no-callback FSR FG";
    // The composite must be driven from the kSkipBundleCovers arm (the active-interpolation route).
    const size_t skipArm = presentText.find("NoCallbackFSRFGOverlayRoute::kSkipBundleCovers");
    ASSERT_NE(skipArm, std::string::npos);
    const size_t compositeCall = presentText.find("DX12_CompositeOverlayOntoCachedFFXUiResource()", skipArm);
    ASSERT_NE(compositeCall, std::string::npos);
    // The retired per-frame bundle reset must no longer be called from the present path.
    EXPECT_EQ(presentText.find("DX12_ResetNoCallbackBundleFrame"), std::string::npos)
        << "the per-frame bundle-append latch reset is retired with the bundle";

    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = readFile(source);
    ASSERT_FALSE(text.empty());

    // The crashing bundle drive must be fully retired — no dead code left behind.
    EXPECT_EQ(text.find("TryAppendNoCallbackBundleOverlay"), std::string::npos)
        << "the unfenced game-ECL bundle drive must be removed";
    EXPECT_EQ(text.find("RecordBundleOverlayForGameECL"), std::string::npos)
        << "the unfenced bundle record helper must be removed";
    EXPECT_EQ(text.find("DX12_ShouldCompositeOverlayOntoFFXUiResource"), std::string::npos)
        << "the dead composite gate (hard-returned false) must be removed";

    // The cached-target wrapper must exist and forward to the real composite.
    const size_t wrapper = text.find("bool DX12_CompositeOverlayOntoCachedFFXUiResource()");
    ASSERT_NE(wrapper, std::string::npos);
    const size_t wrapperBodyEnd = text.find("\n}", wrapper);
    ASSERT_NE(wrapperBodyEnd, std::string::npos);
    EXPECT_LT(text.find("DX12_CompositeOverlayOntoFFXUiResource(uiTexture, ffxState, flags)", wrapper), wrapperBodyEnd);
}

// ---------------------------------------------------------------------------
// The composite must clear CE's SUBSTITUTE target to transparent each frame (it
// is CE-owned and otherwise empty, so only the overlay should composite over the
// game frame). When the target is the game's own usable UI texture the overlay
// blends on top of the HUD already there — so the clear is gated on the
// g_BundleTargetNeedsTransparentClear flag. This is the same clear policy the
// retired bundle had; it must now live inside the composite.
// ---------------------------------------------------------------------------
TEST(DXGISharedSourceTest, FFXUiCompositeClearsSubstituteTargetTransparent) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));

    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    const size_t composite = text.find("bool DX12_CompositeOverlayOntoFFXUiResource(void* uiResourcePtr");
    ASSERT_NE(composite, std::string::npos);
    const size_t compositeBodyEnd = text.find("\n}", text.find("return true;", composite));
    ASSERT_NE(compositeBodyEnd, std::string::npos);

    // The composite reads the substitute clear flag and clears the RTV to transparent before drawing.
    const size_t clearFlag = text.find("needsTransparentClear", composite);
    ASSERT_NE(clearFlag, std::string::npos);
    EXPECT_LT(clearFlag, compositeBodyEnd);
    const size_t clearCall = text.find("ClearRenderTargetView(rtv", composite);
    ASSERT_NE(clearCall, std::string::npos);
    EXPECT_LT(clearCall, compositeBodyEnd);

    // The composite must guard against a degenerate (e.g. 1x1) cached target so it never crashes when no
    // substitute was available — mirroring the retired bundle's degenerate guard.
    const size_t degenerate = text.find("degenerate", composite);
    ASSERT_NE(degenerate, std::string::npos);
    EXPECT_LT(degenerate, compositeBodyEnd);
}

// ---------------------------------------------------------------------------
// GTA registers a 1x1 placeholder UI resource EVERY frame; after the one-shot ffxConfigure VEH disarms, those
// calls reach AMD directly and override CE's substitute, so AMD composites the empty 1x1 and the overlay is
// invisible (session 20260624_004915). CE must RE-ASSERT its substitute each GAME present — but ONLY from
// the FFX proxy-present prework on the GAME thread. Session 20260701_213656 froze GTA permanently on the
// first FSR-FG frame because the re-assert ran from DetourPresent on AMD's PRESENTER thread: the forwarded
// ffxConfigure(RegisterUiResource) takes AMD's swapchain criticalSection, which AMD's Present holds on the
// game thread while fence-spinning without timeout — a permanent lock cycle. Verify the wiring across the
// two hook TUs: composite wrapper does NOT re-assert; the proxy prework composites THEN re-asserts; the
// re-assert hard-refuses outside the prework; teardown still clears the stored desc.
// ---------------------------------------------------------------------------
TEST(DXGISharedSourceTest, NoCallbackSubstituteUiResourceReassertOnlyFromProxyPrework) {
    namespace fs = std::filesystem;
    auto readFile = [](const fs::path& p) {
        EXPECT_TRUE(fs::exists(p)) << p.string();
        const std::string text = ce::test_source::ReadLogicalSource(p);
        EXPECT_FALSE(text.empty()) << p.string();
        return text;
    };

    const std::string dx12 = readFile(fs::current_path() / "hook" / "apis" / "dx12_hook.cpp");
    ASSERT_FALSE(dx12.empty());
    // DEADLOCK BOUNDARY: the composite wrapper is reachable from AMD's presenter thread (DetourPresent
    // fallback driver) and must NOT call the re-assert.
    const size_t wrapper = dx12.find("bool DX12_CompositeOverlayOntoCachedFFXUiResource()");
    ASSERT_NE(wrapper, std::string::npos);
    const size_t wrapperEnd = dx12.find("\n}", wrapper);
    ASSERT_NE(wrapperEnd, std::string::npos);
    const size_t reRegInWrapper = dx12.find("FFXHook_ReRegisterSubstituteUiResource()", wrapper);
    EXPECT_TRUE(reRegInWrapper == std::string::npos || reRegInWrapper > wrapperEnd)
        << "the composite wrapper must never re-assert (presenter-thread deadlock, session 20260701_213656)";
    // The proxy-present prework (game thread) composites FIRST, then re-asserts, inside the prework guard.
    const size_t prework = dx12.find("DX12_RunFFXProxyPrePresentWork(");
    ASSERT_NE(prework, std::string::npos);
    const size_t preworkComposite =
        dx12.find("DX12_CompositeOverlayOntoCachedFFXUiResourceOnOwnerQueue(proxy)", prework);
    ASSERT_NE(preworkComposite, std::string::npos);
    const size_t preworkReReg = dx12.find("FFXHook_ReRegisterSubstituteUiResource()", prework);
    ASSERT_NE(preworkReReg, std::string::npos);
    EXPECT_LT(preworkComposite, preworkReReg)
        << "prework must draw the overlay onto the substitute BEFORE re-asserting its registration";
    // The re-registration is cleared when the substitute is released (dangling-desc safety).
    EXPECT_NE(dx12.find("FFXHook_ClearSubstituteUiReRegistration()"), std::string::npos);

    const std::string ffx = readFile(fs::current_path() / "hook" / "apis" / "ffx_hook.cpp");
    ASSERT_FALSE(ffx.empty());
    // The substitute register is stored ONLY on the degenerate-substitute path (inside the substitution block).
    const size_t prepare = ffx.find("DX12_PrepareFFXUiOverlayTarget(");
    ASSERT_NE(prepare, std::string::npos);
    const size_t store = ffx.find("StoreSubstituteUiReRegistration(context, originalConfigure", prepare);
    ASSERT_NE(store, std::string::npos);
    // The re-register call forwards to the REAL ffxConfigure (g_SubstReRegConfigure), not CE's hook.
    // The re-assert consults the driver policy and refuses outside the proxy-present prework.
    const size_t reRegFn = ffx.find("FFXSubstituteUiReRegistrationResult FFXHook_ReRegisterSubstituteUiResource()");
    ASSERT_NE(reRegFn, std::string::npos);
    const size_t guard = ffx.find("MayReassertSubstituteUiResource", reRegFn);
    const size_t reRegResult = ffx.find("const ffxReturnCode_t result =", reRegFn);
    const size_t forward = ffx.find("g_SubstReRegConfigure(", reRegResult);
    ASSERT_NE(reRegResult, std::string::npos);
    ASSERT_NE(guard, std::string::npos);
    ASSERT_NE(forward, std::string::npos);
    EXPECT_LT(guard, forward) << "the prework-context guard must run BEFORE the ffxConfigure forward";
    // Context creation installs the hook before a first passthrough Present;
    // configure remains the idempotent fallback for integrations such as GTA.
    EXPECT_NE(ffx.find("DX12_TryInstallFFXProxyPresentHook(*parsedSwapChainCreate.swapChainOutput"), std::string::npos);
    EXPECT_NE(ffx.find("DX12_TryInstallFFXProxyPresentHook(localConfig.swapChain"), std::string::npos);
}

TEST(DXGISharedSourceTest, FFXUiRegistrationPublishesOnlyAfterProviderSuccess) {
    namespace fs = std::filesystem;
    auto readFile = [](const fs::path& p) {
        const std::string text = ce::test_source::ReadLogicalSource(p);
        EXPECT_FALSE(text.empty()) << p.string();
        return text;
    };

    const std::string ffx = readFile(fs::current_path() / "hook" / "apis" / "ffx_hook.cpp");
    const size_t forward = ffx.find("const ffxReturnCode_t result = CallFfxConfigureOriginalGuarded");
    const size_t commit = ffx.find("DX12_CommitFFXUiOverlayTarget(&uiTargetPreparation)", forward);
    const size_t store = ffx.find("StoreSubstituteUiReRegistration(context, originalConfigure", forward);
    const size_t discard = ffx.find("DX12_DiscardFFXUiOverlayTarget(&uiTargetPreparation)", forward);
    ASSERT_NE(forward, std::string::npos);
    ASSERT_NE(commit, std::string::npos);
    ASSERT_NE(store, std::string::npos);
    ASSERT_NE(discard, std::string::npos);
    EXPECT_LT(forward, commit);
    EXPECT_LT(commit, store);
    EXPECT_LT(store, discard);

    const std::string dx12 = readFile(fs::current_path() / "hook" / "apis" / "dx12_hook.cpp");
    EXPECT_NE(dx12.find("COMMON/PRESENT is legitimately numeric zero"), std::string::npos);
    EXPECT_NE(dx12.find("g_CEUiSubstituteInitialState == initialState"), std::string::npos);
    EXPECT_NE(dx12.find("IsResourceOwnedByDevice(g_CEUiSubstituteTexture, device)"), std::string::npos);
    EXPECT_NE(dx12.find("preparation->sequence < g_FFXUiCommittedPreparationSequence"), std::string::npos);
}

TEST(DXGISharedSourceTest, FFXOwnerQueueRendererRetainsTargetsAndNeverCpuWaits) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_ffx_suspend_overlay.cpp";
    const std::string text = ce::test_source::ReadLogicalSource(source);

    EXPECT_NE(text.find("ComPtr<ID3D12Resource> inFlightTarget"), std::string::npos);
    EXPECT_NE(text.find("slot.inFlightTarget = targetResource"), std::string::npos);
    EXPECT_NE(text.find("slots[slotIndex].fenceValue > completed"), std::string::npos);
    EXPECT_NE(text.find("overlay.SetDX12NextUploadSlot(static_cast<int>(slotIndex))"), std::string::npos);
    EXPECT_EQ(text.find("WaitForSingleObject"), std::string::npos);
    EXPECT_EQ(text.find("CreateCommandQueue"), std::string::npos);
}

TEST(DXGISharedSourceTest, DurableCachedFFXConfigureRouteRetiresContendedVehAndLogsFirstTransitionPresent) {
    namespace fs = std::filesystem;
    auto readFile = [](const fs::path& path) {
        const std::string text = ce::test_source::ReadLogicalSource(path);
        EXPECT_FALSE(text.empty()) << path.string();
        return text;
    };
    const std::string ffx = readFile(fs::current_path() / "hook" / "apis" / "ffx_hook.cpp");
    const std::string dx12 = readFile(fs::current_path() / "hook" / "apis" / "dx12_hook.cpp");

    EXPECT_NE(ffx.find("cachedRouteResult.routedRouteMask & kConfigureRouteBit"), std::string::npos);
    EXPECT_NE(ffx.find("g_ffxConfigureVehPermanentlyDisarmed.store(true"), std::string::npos);
    EXPECT_NE(ffx.find("g_DurableCachedConfigureRouteActive.exchange(true"), std::string::npos);
    EXPECT_NE(ffx.find("durable cached ffxConfigure pointer route installed"), std::string::npos);
    EXPECT_NE(ffx.find("Kept protected ffxConfigure VEH retired across FG context destruction"), std::string::npos);
    EXPECT_NE(ffx.find("Frame Generation configure unchanged"), std::string::npos);
    EXPECT_NE(ffx.find("if (enabledStateChanged)"), std::string::npos);
    EXPECT_NE(dx12.find("FFX proxy overlay route transition %s -> %s"), std::string::npos);
    EXPECT_NE(dx12.find("the first present after the configure transition selected the new target"), std::string::npos);
}

TEST(DXGISharedSourceTest, FFXProxyPresentRemovalQuiescesAndDrainsEnteredDetours) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    const std::string text = ce::test_source::ReadLogicalSource(source);

    EXPECT_NE(text.find("g_FFXProxyPresentDetoursInFlight.fetch_add"), std::string::npos);
    EXPECT_NE(text.find("g_FFXProxyPresentQuiescing.store(true"), std::string::npos);
    EXPECT_NE(text.find("g_FFXProxyPresentDrainCV.wait"), std::string::npos);
    EXPECT_NE(text.find("DX12_RemoveFFXProxyPresentHook(\"DX12 shutdown\")"), std::string::npos);
    EXPECT_NE(text.find("if (!lastPrework"), std::string::npos)
        << "a patched but never-entered proxy must keep the real-present fallback alive";
}

TEST(DXGISharedSourceTest, StreamlineFirstActivationUsesOfficialUiTagWithoutExtraGpuWork) {
    namespace fs = std::filesystem;
    auto readFile = [](const fs::path& path) {
        const std::string text = ce::test_source::ReadLogicalSource(path);
        EXPECT_FALSE(text.empty()) << path.string();
        return text;
    };

    const std::string streamline = readFile(fs::current_path() / "hook" / "apis" / "streamline_hook.cpp");
    const std::string renderer = readFile(fs::current_path() / "hook" / "apis" / "dx12_streamline_ui_overlay.cpp");
    const std::string dx12 = readFile(fs::current_path() / "hook" / "apis" / "dx12_hook.cpp");

    EXPECT_NE(streamline.find("RegisterDynamicHookFiltered(\"slSetTag\""), std::string::npos)
        << "deprecated/global tagging remains used by real integrations such as Talos";
    EXPECT_NE(streamline.find("RegisterDynamicHookFiltered(\"slSetTagForFrame\""), std::string::npos);
    EXPECT_NE(streamline.find("RegisterDynamicHookFiltered(\"slEvaluateFeature\""), std::string::npos)
        << "frame-local tags bypass both global tag APIs in integrations such as Talos";
    EXPECT_NE(streamline.find("g_StreamlineUsesD3D12.load"), std::string::npos)
        << "D3D11/Vulkan tags must never be interpreted as ID3D12Resource";
    EXPECT_NE(streamline.find("if (wantsUiBootstrapRecord && tags)"), std::string::npos)
        << "tag packets without a pending standby/activation record must not inspect resource arrays";
    EXPECT_NE(renderer.find("g_FrameTagTrackingActive.load"), std::string::npos)
        << "inactive and active steady state must leave only one atomic hot-path branch";
    const size_t setTagHook = streamline.find("slResult Hooked_slSetTagForFrame");
    ASSERT_NE(setTagHook, std::string::npos);
    const size_t record = streamline.find("TryRecordBootstrap(request)", setTagHook);
    const size_t forward = streamline.find("return originalSetTagForFrame", setTagHook);
    ASSERT_NE(record, std::string::npos);
    ASSERT_NE(forward, std::string::npos);
    EXPECT_LT(record, forward) << "Streamline's volatile-tag copy must include CE's UI draw";

    const size_t legacySetTagHook = streamline.find("slResult Hooked_slSetTag(");
    ASSERT_NE(legacySetTagHook, std::string::npos);
    const size_t legacyRecord = streamline.find("TryRecordOfficialUiTag(\"slSetTag\"", legacySetTagHook);
    const size_t legacyForward = streamline.find("return originalSetTag(viewport", legacySetTagHook);
    ASSERT_NE(legacyRecord, std::string::npos);
    ASSERT_NE(legacyForward, std::string::npos);
    EXPECT_LT(legacyRecord, legacyForward)
        << "global tags must receive the same official-UI record before Streamline observes them";
    EXPECT_NE(streamline.find("g_SLSetTagTarget"), std::string::npos);
    EXPECT_NE(streamline.find("{\"slSetTag\", &g_SLSetTagTarget"), std::string::npos)
        << "legacy tag hook state must invalidate safely across Streamline unload/reload";

    const size_t evaluateDeclaration = streamline.find("slResult Hooked_slEvaluateFeature");
    ASSERT_NE(evaluateDeclaration, std::string::npos);
    const size_t evaluateHook = streamline.find("slResult Hooked_slEvaluateFeature", evaluateDeclaration + 1);
    ASSERT_NE(evaluateHook, std::string::npos);
    const size_t evaluateGate = streamline.find("dx12_streamline_ui_overlay::OnFrameTag(frameToken)", evaluateHook);
    const size_t evaluateLocalTag =
        streamline.find("StructTypesEqual(input->structType, kResourceTagStructType)", evaluateGate);
    const size_t evaluateRecord =
        streamline.find("TryRecordOfficialUiResourceTag(frameToken, tag, commandBuffer)", evaluateLocalTag);
    const size_t evaluateForward = streamline.find("return originalEvaluateFeature", evaluateRecord);
    ASSERT_NE(evaluateGate, std::string::npos);
    ASSERT_NE(evaluateLocalTag, std::string::npos);
    ASSERT_NE(evaluateRecord, std::string::npos);
    ASSERT_NE(evaluateForward, std::string::npos);
    EXPECT_LT(evaluateGate, evaluateLocalTag)
        << "steady-state evaluate calls must remain one atomic bootstrap gate before input scanning";
    EXPECT_LT(evaluateRecord, evaluateForward) << "Streamline's local volatile-tag use/copy must include CE's UI draw";
    EXPECT_NE(streamline.find("{\"slEvaluateFeature\", &g_SLEvaluateFeatureTarget"), std::string::npos)
        << "evaluate hook state must invalidate safely across Streamline unload/reload";

    EXPECT_NE(renderer.find("slot.target = request.uiResource"), std::string::npos);
    EXPECT_NE(renderer.find("queue->Signal(fence.Get(), slot.fenceValue)"), std::string::npos);
    EXPECT_EQ(renderer.find("WaitForSingleObject"), std::string::npos);
    EXPECT_EQ(renderer.find("CopyResource"), std::string::npos);
    EXPECT_EQ(renderer.find("CreateCommandQueue"), std::string::npos);
    EXPECT_EQ(renderer.find("queue->ExecuteCommandLists"), std::string::npos);

    const size_t eclBefore = dx12.find("dx12_streamline_ui_overlay::BeforeExecuteCommandLists");
    const size_t realEcl = dx12.find("original(pThis, NumCommandLists, ppCommandLists)", eclBefore);
    const size_t eclAfter = dx12.find("dx12_streamline_ui_overlay::AfterExecuteCommandLists", realEcl);
    ASSERT_NE(eclBefore, std::string::npos);
    ASSERT_NE(realEcl, std::string::npos);
    ASSERT_NE(eclAfter, std::string::npos);
    EXPECT_LT(eclBefore, realEcl);
    EXPECT_LT(realEcl, eclAfter);
}

TEST(DXGISharedSourceTest, StreamlineGetStateOnlyActivationAdoptsPreTaggedOfficialUi) {
    namespace fs = std::filesystem;
    auto readFile = [](const fs::path& path) {
        const std::string text = ce::test_source::ReadLogicalSource(path);
        EXPECT_FALSE(text.empty()) << path.string();
        return text;
    };

    const std::string streamline = readFile(fs::current_path() / "hook" / "apis" / "streamline_hook.cpp");
    const std::string renderer = readFile(fs::current_path() / "hook" / "apis" / "dx12_streamline_ui_overlay.cpp");
    const std::string dx12 = readFile(fs::current_path() / "hook" / "apis" / "dx12_hook.cpp");

    const size_t getStateLookup = streamline.find("strcmp(functionName, \"slDLSSGGetState\")");
    ASSERT_NE(getStateLookup, std::string::npos);
    const size_t lookupStandby = streamline.find("BeginPreactivationStandby(2)", getStateLookup);
    const size_t lookupReturn = streamline.find("return result;", getStateLookup);
    ASSERT_NE(lookupStandby, std::string::npos);
    ASSERT_NE(lookupReturn, std::string::npos);
    EXPECT_LT(lookupStandby, lookupReturn)
        << "standby must arm while the GetState pointer is delivered, before the caller tags activation inputs";

    const size_t setDeviceDeclaration = streamline.find("slResult Hooked_slSetD3DDevice");
    ASSERT_NE(setDeviceDeclaration, std::string::npos);
    const size_t setDeviceHook = streamline.find("slResult Hooked_slSetD3DDevice", setDeviceDeclaration + 1);
    ASSERT_NE(setDeviceHook, std::string::npos);
    const size_t deviceStandby = streamline.find("BeginPreactivationStandby(2)", setDeviceHook);
    const size_t deviceFeatureResolve = streamline.find("TryResolveDLSSGFeatureHooks()", setDeviceHook);
    ASSERT_NE(deviceStandby, std::string::npos);
    ASSERT_NE(deviceFeatureResolve, std::string::npos);
    EXPECT_LT(deviceStandby, deviceFeatureResolve)
        << "standby must exist as soon as D3D12 is accepted; reusable UI tags may precede feature lookup";
    EXPECT_NE(streamline.find("Official UI tag record opportunity", setDeviceHook), std::string::npos)
        << "early tag shape/call ordering needs bounded diagnostics when no usable UI record is produced";

    const size_t getStateDeclaration = streamline.find("slResult Hooked_slDLSSGGetState");
    ASSERT_NE(getStateDeclaration, std::string::npos);
    const size_t getStateHook = streamline.find("slResult Hooked_slDLSSGGetState", getStateDeclaration + 1);
    ASSERT_NE(getStateHook, std::string::npos);
    const size_t callStandby = streamline.find("BeginPreactivationStandby(requestedOutputs)", getStateHook);
    const size_t callOriginal = streamline.find("originalGetState(viewport, state, options)", getStateHook);
    ASSERT_NE(callStandby, std::string::npos);
    ASSERT_NE(callOriginal, std::string::npos);
    EXPECT_LT(callStandby, callOriginal)
        << "GetState(options) must not activate DLSS-G before CE has armed the official UI standby";

    EXPECT_NE(renderer.find("BootstrapPhase::kStandbySubmitted"), std::string::npos);
    EXPECT_NE(renderer.find("BootstrapPhase::kActivationSubmitted"), std::string::npos);
    EXPECT_NE(renderer.find("adoptedSubmittedStandby"), std::string::npos)
        << "a GetState OFF-to-ON edge after tagging must adopt the already-submitted UI record";
    const size_t adoptedStandbyRollover = renderer.find("if (g_AdoptedStandbyNeedsActivationFrameRecord &&");
    ASSERT_NE(adoptedStandbyRollover, std::string::npos);
    const size_t requestActivationRecord = renderer.find("return true;", adoptedStandbyRollover);
    const size_t ordinaryActivationRetirement = renderer.find("g_FrameToken = frameToken;", requestActivationRecord);
    ASSERT_NE(requestActivationRecord, std::string::npos);
    ASSERT_NE(ordinaryActivationRetirement, std::string::npos);
    EXPECT_LT(requestActivationRecord, ordinaryActivationRetirement)
        << "a different first activation token must replace an expired adopted standby tag before ordinary "
           "one-shot retirement";
    EXPECT_NE(renderer.find("prior eValidUntilPresent lifetime", adoptedStandbyRollover), std::string::npos)
        << "runtime diagnostics must distinguish exact tag-lifetime rollover from missing PostSL coverage";
    EXPECT_NE(renderer.find("if (g_Phase == BootstrapPhase::kStandbyIdle)"), std::string::npos)
        << "a frame without a usable UI tag must not prevent standby from trying the next frame";
    EXPECT_NE(renderer.find("g_CoverageBudget.Arm(g_MaximumOutputPresents)"), std::string::npos)
        << "standby draws must become visible-output coverage only after an activation edge";
    EXPECT_NE(renderer.find("Streamline UI bootstrap continuing on next activation frame tag"), std::string::npos)
        << "eValidUntilPresent source-frame rollover must re-record until PostSL consumes the handoff";
    EXPECT_NE(renderer.find("g_CoverageBudget.ConsumePostSLOutput()"), std::string::npos)
        << "only real PostSL output consumption may spend the official-UI coverage budget";
    EXPECT_NE(dx12.find("officialUiCoverage = ce::dx12_streamline_ui_overlay::HasActiveCoverage()"), std::string::npos)
        << "generated presents before PostSL can render must inherit the submitted official-UI overlay";
    EXPECT_NE(dx12.find("normalRouteDrawPendingAtEntry"), std::string::npos)
        << "same-queue startup must preserve an already-drawn normal-route present instead of double drawing";
    EXPECT_NE(dx12.find("postsl-same-queue-make-before-break"), std::string::npos)
        << "a proven same-queue callback must hand off without a timed uncovered window";
    EXPECT_NE(dx12.find("PostSL synthetic startup immediate same-queue takeover"), std::string::npos)
        << "logs must distinguish event-driven immediate takeover from the separate-queue dormant guard";
    const size_t consumeCoverage = dx12.find("ConsumePostSLCoverage()");
    ASSERT_NE(consumeCoverage, std::string::npos);
    EXPECT_NE(dx12.find("NoteDX12OverlayRendered(DX12OverlayRenderRoute::kStreamlineUI)", consumeCoverage),
              std::string::npos)
        << "standby submissions must be accounted only once an active output consumes their UI coverage";
    EXPECT_EQ(renderer.find("CopyResource"), std::string::npos);
    EXPECT_EQ(renderer.find("CreateCommandQueue"), std::string::npos);
    EXPECT_EQ(renderer.find("queue->ExecuteCommandLists"), std::string::npos);
    EXPECT_EQ(renderer.find("WaitForSingleObject"), std::string::npos);
}
