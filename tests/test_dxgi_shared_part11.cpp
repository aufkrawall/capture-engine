#include "test_dxgi_shared_shared.h"

TEST(DXGISharedSourceTest, GuardedSteamRuntimeWorkerRejectionPrecedesEverySteamTouchAndInvoke) {
    namespace fs = std::filesystem;
    const fs::path steamSource = fs::current_path() / "hook" / "common" / "dxgi_shared_steam.cpp";
    const fs::path coreSource = fs::current_path() / "hook" / "common" / "dxgi_shared_present_core.cpp";
    ASSERT_TRUE(fs::exists(steamSource));
    ASSERT_TRUE(fs::exists(coreSource));

    const std::string steam = ce::test_source::ReadFile(steamSource);
    const std::string core = ce::test_source::ReadFile(coreSource);
    ASSERT_FALSE(steam.empty());
    ASSERT_FALSE(core.empty());

    const size_t entry = steam.find("bool TryInvokeGuardedExternalSteamOverlayPresent(");
    const size_t provenance = steam.find("ShouldInvokeSynchronousExternalOverlayPresentForThreadState(", entry);
    const size_t reject = steam.find("if (!synchronousPresentThreadAllowed)", provenance);
    const size_t callbackRead = steam.find("TryReadSteamOverlayNullCallbackSlot", reject);
    const size_t recoveryGuard = steam.find("ScopedSteamNullCallbackRecoveryGuard", callbackRead);
    const size_t externalInvoke = steam.find("const HRESULT hr = externalPresent", recoveryGuard);
    ASSERT_NE(entry, std::string::npos);
    ASSERT_NE(provenance, std::string::npos);
    ASSERT_NE(reject, std::string::npos);
    ASSERT_NE(callbackRead, std::string::npos);
    ASSERT_NE(recoveryGuard, std::string::npos);
    ASSERT_NE(externalInvoke, std::string::npos);
    EXPECT_LT(provenance, reject);
    EXPECT_LT(reject, callbackRead);
    EXPECT_LT(callbackRead, recoveryGuard);
    EXPECT_LT(recoveryGuard, externalInvoke);
    EXPECT_NE(steam.find("this is not the verified source Present thread", reject), std::string::npos);

    EXPECT_NE(core.find("SL external-overlay vtable transport"), std::string::npos);
    EXPECT_EQ(core.find("\"SL startup bypass\""), std::string::npos);

    const fs::path originalSource = fs::current_path() / "hook" / "common" / "dxgi_shared_original.cpp";
    ASSERT_TRUE(fs::exists(originalSource));
    const std::string original = ce::test_source::ReadFile(originalSource);
    ASSERT_FALSE(original.empty());
    const size_t naturalGuard = original.find("refusing Steam Present transport on runtime worker");
    const size_t inlineTrampoline = original.find("if (presentTrampoline)");
    const size_t forcedBypassRoute = original.find("if (forceSteamDX12Bypass)");
    const size_t slFastPath = original.find("if (slLoaded && presentOriginal && presentOriginal != DetourPresent)");
    ASSERT_NE(naturalGuard, std::string::npos);
    ASSERT_NE(inlineTrampoline, std::string::npos);
    ASSERT_NE(forcedBypassRoute, std::string::npos);
    ASSERT_NE(slFastPath, std::string::npos);
    EXPECT_LT(naturalGuard, inlineTrampoline);
    EXPECT_LT(naturalGuard, forcedBypassRoute);
    EXPECT_LT(naturalGuard, slFastPath);

    const size_t present1Entry = original.find("HRESULT CallOriginalPresent1(");
    const size_t present1WorkerGuard =
        original.find("refusing Steam Present1 transport on runtime worker", present1Entry);
    const size_t present1InlineTrampoline = original.find("if (present1Trampoline)", present1Entry);
    const size_t present1ForcedBypass = original.find("if (forceSteamDX12Bypass)", present1Entry);
    ASSERT_NE(present1Entry, std::string::npos);
    ASSERT_NE(present1WorkerGuard, std::string::npos);
    ASSERT_NE(present1InlineTrampoline, std::string::npos);
    ASSERT_NE(present1ForcedBypass, std::string::npos);
    EXPECT_LT(present1WorkerGuard, present1InlineTrampoline);
    EXPECT_LT(present1WorkerGuard, present1ForcedBypass);
}

TEST(DXGISharedSourceTest, PresentBootstrapPreservesE9AndDetoursFF25ForeignEntries) {
    namespace fs = std::filesystem;
    const fs::path hooksSource = fs::current_path() / "hook" / "common" / "dxgi_shared_hooks_present.cpp";
    ASSERT_TRUE(fs::exists(hooksSource));
    const std::string hooks = ce::test_source::ReadFile(hooksSource);
    ASSERT_FALSE(hooks.empty());

    const size_t e9Detection = hooks.find("const bool entryUsesE9");
    const size_t ff25Detection = hooks.find("const bool entryUsesFF25");
    const size_t ff25Resolve = hooks.find("ResolveFF25JmpTarget(presentAddr)", ff25Detection);
    const size_t bypass = hooks.find("InlineHook::CreateBypassTrampoline(presentAddr)", ff25Resolve);
    const size_t prepend = hooks.find("InlineHook::InstallPublished(presentAddr", bypass);
    ASSERT_NE(e9Detection, std::string::npos);
    ASSERT_NE(ff25Detection, std::string::npos);
    ASSERT_NE(ff25Resolve, std::string::npos);
    ASSERT_NE(bypass, std::string::npos);
    ASSERT_NE(prepend, std::string::npos);
    EXPECT_LT(e9Detection, ff25Resolve);
    EXPECT_LT(ff25Detection, ff25Resolve);
    EXPECT_LT(ff25Resolve, bypass);
    EXPECT_LT(bypass, prepend);
}

TEST(DXGISharedSourceTest, RuntimeWorkerCannotReplaceTrackedSourcePresentThread) {
    namespace fs = std::filesystem;
    const fs::path phase2Source =
        fs::current_path() / "hook" / "apis" / "dx12_hook_process_session_phase2.cpp";
    ASSERT_TRUE(fs::exists(phase2Source));
    const std::string phase2 = ce::test_source::ReadFile(phase2Source);
    ASSERT_FALSE(phase2.empty());

    const size_t tracking = phase2.find("Track the application's source Present thread");
    const size_t sourceGate = phase2.find("if (applicationSourcePresent)", tracking);
    const size_t threadStore = phase2.find("dx12_hook_g_GamePresentThreadId.store", sourceGate);
    ASSERT_NE(tracking, std::string::npos);
    ASSERT_NE(sourceGate, std::string::npos);
    ASSERT_NE(threadStore, std::string::npos);
    EXPECT_LT(tracking, sourceGate);
    EXPECT_LT(sourceGate, threadStore);
    EXPECT_EQ(phase2.find("if (!slFGNow)", tracking), std::string::npos);
}

// Strange Brigade DX12 session 20260806_165849: every Present re-initialized the
// whole overlay (ImGui + RTVs + 16 allocators + fence) because the ProcessFrame
// semantic-unit refactor (3398151e) hoisted the GetBuffer-failure recovery out of
// its else-branch and ran CleanupRTVs()/overlayInit=false unconditionally after
// every successful overlay draw. That per-frame teardown flooded the GPU queue,
// produced 1s upload-ring fence timeouts (game render stalls) and made the overlay
// rebuild itself (and visibly flicker) on every frame. The recovery must stay in
// the GetBuffer-failure branch only.
TEST(DXGISharedSourceTest, GetBufferFailureForcesRtvReinitButSuccessPathKeepsOverlayState) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));

    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    // DrawSc3Front must pair the GetBuffer success path with an else-branch that
    // forces RTV reinit (CleanupRTVs + overlayInit=false) only when GetBuffer fails.
    const size_t front = text.find("ProcessFrameFlow FrameProcessSession::DrawSc3Front() {");
    ASSERT_NE(front, std::string::npos);
    const size_t getBuffer = text.find("sc3->GetBuffer(swapchainBufferIdx, IID_PPV_ARGS(&bb))) && bb)", front);
    ASSERT_NE(getBuffer, std::string::npos);
    const size_t rtvRecreate = text.find("CreateRenderTargetView(bb, nullptr, rtvRecreate)", getBuffer);
    ASSERT_NE(rtvRecreate, std::string::npos);
    const size_t failureElse = text.find("} else {", rtvRecreate);
    ASSERT_NE(failureElse, std::string::npos);
    EXPECT_LT(failureElse - rtvRecreate, static_cast<size_t>(120))
        << "the GetBuffer-failure else-branch must directly follow the success path";
    const size_t failureLog = text.find("HookLog(\"DX12: GetBuffer(%u) failed, forcing RTV reinit\"", failureElse);
    const size_t failureCleanup = text.find("CleanupRTVs();", failureElse);
    const size_t failureInvalidate = text.find("dx12_hook_g_State.overlayInit = false;", failureElse);
    ASSERT_NE(failureLog, std::string::npos);
    ASSERT_NE(failureCleanup, std::string::npos);
    ASSERT_NE(failureInvalidate, std::string::npos);
    EXPECT_LT(failureLog - failureElse, static_cast<size_t>(400));
    EXPECT_LT(failureCleanup - failureLog, static_cast<size_t>(200));
    EXPECT_LT(failureInvalidate - failureCleanup, static_cast<size_t>(80));

    // DrawSubmitCoreTail must NOT clear overlay state after a successful draw:
    // that is what forced the per-frame reinit storm. It must only release the
    // per-frame backbuffer reference.
    const size_t tail = text.find("ProcessFrameFlow FrameProcessSession::DrawSubmitCoreTail() {");
    ASSERT_NE(tail, std::string::npos);
    const size_t nextFunction = text.find("ProcessFrameFlow FrameProcessSession::DrawSc3Else() {", tail);
    ASSERT_NE(nextFunction, std::string::npos);
    const std::string tailBody = text.substr(tail, nextFunction - tail);
    EXPECT_NE(tailBody.find("if (bbNeedsRelease)"), std::string::npos)
        << "the per-frame backbuffer reference release must remain";
    EXPECT_NE(tailBody.find("bb->Release();"), std::string::npos);
    EXPECT_EQ(tailBody.find("dx12_hook_g_State.overlayInit = false;"), std::string::npos)
        << "successful draws must never invalidate the overlay state";
    EXPECT_EQ(tailBody.find("CleanupRTVs();"), std::string::npos)
        << "successful draws must never tear down RTV state";
}

// Strange Brigade DX12 session 20260806_174024: after the first successful draw
// every present re-ran the staged sync activation (InitOverlaySync with a fresh
// fence + 16 allocators) because the ProcessFrame semantic-unit refactor
// (3398151e) hoisted the failure else-branches of the draw chain into the
// success path: DrawSubmitElse (list->Reset failed) and DrawResetElse
// (alloc->Reset failed) both ran unconditionally after every successful draw and
// cleared syncInit. The overlay then drew ~once per ~200ms (flicker / ghostly
// transparency) and the repeated fence recreation made the DescFree upload-ring
// guard wait hit its 1s timeout every frame (game collapsed to ~1 fps). All four
// else-chunks (DrawSc3Else, DrawSubmitElse, DrawResetElse, DrawNullList) must be
// reachable ONLY from their original failure branches.
TEST(DXGISharedSourceTest, DrawChainFailureElseBranchesNeverRunOnTheSuccessPath) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook.cpp";
    ASSERT_TRUE(fs::exists(source));

    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    const auto assertElseOnly = [&](const char* wrapper, const char* elseCall) {
        const size_t wrapperPos = text.find(wrapper);
        ASSERT_NE(wrapperPos, std::string::npos) << wrapper;
        const size_t nextWrapper = text.find("ProcessFrameFlow FrameProcessSession::", wrapperPos + 1);
        const std::string body = text.substr(wrapperPos, nextWrapper - wrapperPos);
        const size_t elsePos = body.find("} else {");
        ASSERT_NE(elsePos, std::string::npos) << wrapper << " must keep its failure else-branch";
        const size_t firstCall = body.find(elseCall);
        ASSERT_NE(firstCall, std::string::npos) << wrapper << " must call " << elseCall;
        EXPECT_LT(elsePos, firstCall)
            << wrapper << ": " << elseCall << " must be reachable only from the failure branch";
    };
    assertElseOnly(
        "ProcessFrameFlow FrameProcessSession::DrawSubmit() {",
        "flow = DrawSubmitElse();");
    assertElseOnly(
        "ProcessFrameFlow FrameProcessSession::DrawReset() {",
        "flow = DrawResetElse();");
    assertElseOnly(
        "ProcessFrameFlow FrameProcessSession::DrawSc3() {",
        "flow = DrawSc3Else();");
    assertElseOnly(
        "ProcessFrameFlow FrameProcessSession::DrawListAndAlloc() {",
        "flow = DrawNullList();");

    // The failure branches must keep their original recovery semantics.
    const size_t sc3Else = text.find("ProcessFrameFlow FrameProcessSession::DrawSc3Else() {");
    ASSERT_NE(sc3Else, std::string::npos);
    const size_t submitElse = text.find("ProcessFrameFlow FrameProcessSession::DrawSubmitElse() {", sc3Else);
    ASSERT_NE(submitElse, std::string::npos);
    const std::string sc3ElseBody = text.substr(sc3Else, submitElse - sc3Else);
    EXPECT_NE(sc3ElseBody.find("failed to get SwapChain3 interface"), std::string::npos);

    const size_t submitElseEnd = text.find("ProcessFrameFlow FrameProcessSession::DrawResetElse() {", submitElse);
    ASSERT_NE(submitElseEnd, std::string::npos);
    const std::string submitElseBody = text.substr(submitElse, submitElseEnd - submitElse);
    EXPECT_NE(submitElseBody.find("list->Reset failed hr=0x%08X"), std::string::npos);
    EXPECT_NE(submitElseBody.find("dx12_hook_g_State.syncInit = false;"), std::string::npos);

    const size_t resetElseEnd = text.find("ProcessFrameFlow FrameProcessSession::DrawNullList() {", submitElseEnd);
    ASSERT_NE(resetElseEnd, std::string::npos);
    const std::string resetElseBody = text.substr(submitElseEnd, resetElseEnd - submitElseEnd);
    EXPECT_NE(resetElseBody.find("alloc->Reset failed hr=0x%08X"), std::string::npos);
    EXPECT_NE(resetElseBody.find("dx12_hook_g_State.syncInit = false;"), std::string::npos);

    const size_t nullListEnd = text.find("ProcessFrameFlow FrameProcessSession::pw5_c3() {", resetElseEnd);
    ASSERT_NE(nullListEnd, std::string::npos);
    const std::string nullListBody = text.substr(resetElseEnd, nullListEnd - resetElseEnd);
    EXPECT_NE(nullListBody.find("null list or alloc"), std::string::npos);
}

// Talos 20260502_233427 proved a diagnostic COMPUTE queue can crash Streamline
// during startup. Sessions 20260809_030710/035333 correlated later downstream
// hangs with the same live-device mutation; 042528 subsequently proved that was
// not their sole cause, but it does not make invasive discovery safe. Resolution
// must stay observational: use queue methods already captured by the vtable hook
// and never mutate the live D3D12/Streamline device to inspect it.
TEST(DXGISharedSourceTest, RealECLResolutionNeverCreatesALiveRuntimeProbeQueue) {
    namespace fs = std::filesystem;
    const fs::path source =
        fs::current_path() / "hook" / "apis" / "dx12_hook_queue_method_resolution.cpp";
    ASSERT_TRUE(fs::exists(source));

    const std::string text = ce::test_source::ReadFile(source);
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(text.find("CreateCommandQueue("), std::string::npos);
    EXPECT_NE(text.find("dx12_hook_g_ExecuteCommandListsOriginalByVTable"), std::string::npos);
    EXPECT_NE(text.find("dx12_hook_g_LastExecuteCommandListsOriginal"), std::string::npos);
    EXPECT_NE(text.find("dx12_hook_g_ExecuteCommandListsCaptureGeneration"), std::string::npos);
    EXPECT_NE(text.find("refusing temporary queue creation during live runtime"), std::string::npos);
}

TEST(DXGISharedSourceTest, DeferredRealECLResolutionRemainsPendingUntilPassiveProofExists) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "dx12_hook_process.cpp";
    ASSERT_TRUE(fs::exists(source));

    const std::string text = ce::test_source::ReadFile(source);
    ASSERT_FALSE(text.empty());
    const size_t service = text.find("void DX12_ServiceDeferredECLProbe() {");
    const size_t nextFunction = text.find("DWORD WINAPI UnloadThread", service);
    ASSERT_NE(service, std::string::npos);
    ASSERT_NE(nextFunction, std::string::npos);
    const std::string body = text.substr(service, nextFunction - service);
    const size_t proofGate = body.find("if (srvProbed) {");
    const size_t clear = body.find("dx12_hook_g_ProbeRealD3D12ECLDeferred.store(false", proofGate);
    ASSERT_NE(proofGate, std::string::npos);
    ASSERT_NE(clear, std::string::npos);
    EXPECT_LT(proofGate, clear);
}

// The PostSL semantic-unit split originally left function-local copies of the
// lifecycle/probe counters in Chunk0 while Chunk1-3 consumed file-scope copies.
// Talos consequently logged every submit as epoch=0/call#=0 and later stages
// never saw reactivation resets. These variables must have one shared definition.
TEST(DXGISharedSourceTest, PostSLSemanticUnitsShareLifecycleAndAccountingState) {
    namespace fs = std::filesystem;
    const fs::path stateSource =
        fs::current_path() / "hook" / "apis" / "dx12_hook_postsl_render.cpp";
    const fs::path entrySource =
        fs::current_path() / "hook" / "apis" / "dx12_hook_postsl_render_entry.cpp";
    ASSERT_TRUE(fs::exists(stateSource));
    ASSERT_TRUE(fs::exists(entrySource));

    const std::string state = ce::test_source::ReadFile(stateSource);
    const std::string entry = ce::test_source::ReadFile(entrySource);
    ASSERT_FALSE(state.empty());
    ASSERT_FALSE(entry.empty());

    EXPECT_NE(state.find("std::atomic<int> s_postSLRenders{0};"), std::string::npos);
    EXPECT_NE(state.find("std::atomic<int> s_postSLSkipFence{0};"), std::string::npos);
    EXPECT_NE(state.find("int s_reactivationEpoch{0};"), std::string::npos);
    EXPECT_NE(state.find("int s_callsSinceReactivation{0};"), std::string::npos);
    EXPECT_NE(state.find("int s_postSLProbeFrames{0};"), std::string::npos);

    EXPECT_EQ(entry.find("static std::atomic<int> s_postSLRenders"), std::string::npos);
    EXPECT_EQ(entry.find("static std::atomic<int> s_postSLSkipFence"), std::string::npos);
    EXPECT_EQ(entry.find("static int s_reactivationEpoch"), std::string::npos);
    EXPECT_EQ(entry.find("static int s_callsSinceReactivation"), std::string::npos);
    EXPECT_EQ(entry.find("static int s_postSLProbeFrames"), std::string::npos);
}

// Talos 20260809_042528 proved the pre-proof selectedQueueOrigECL fallback
// executed and then fell through into queue->ExecuteCommandLists, submitting the
// same overlay command list twice. Passive real-ECL publication removed the
// second submit mid-session, exactly at the recurring freeze boundary.
TEST(DXGISharedSourceTest, PureDLSSSelectedQueueFallbackCannotFallThroughToSecondECL) {
    namespace fs = std::filesystem;
    const fs::path source =
        fs::current_path() / "hook" / "apis" / "dx12_hook_postsl_render_submit.cpp";
    ASSERT_TRUE(fs::exists(source));

    const std::string text = ce::test_source::ReadFile(source);
    ASSERT_FALSE(text.empty());
    const size_t select = text.find("SelectPostSLBootstrapSubmitPath(");
    const size_t directBranch = text.find("PostSLBootstrapSubmitPath::kSelectedQueueOriginal", select);
    const size_t rejectBranch = text.find("} else if (bootstrapPath ==", directBranch);
    const size_t bootstrapBranch = text.find("} else {", rejectBranch);
    ASSERT_NE(select, std::string::npos);
    ASSERT_NE(directBranch, std::string::npos);
    ASSERT_NE(rejectBranch, std::string::npos);
    ASSERT_NE(bootstrapBranch, std::string::npos);
    EXPECT_LT(select, directBranch);
    EXPECT_LT(directBranch, rejectBranch);
    EXPECT_LT(rejectBranch, bootstrapBranch);

    const std::string directBody = text.substr(directBranch, rejectBranch - directBranch);
    EXPECT_NE(directBody.find("selectedQueueOrigECL(queue, 1, lists);"), std::string::npos);
    EXPECT_EQ(directBody.find("->ExecuteCommandLists(1, lists)"), std::string::npos);
    EXPECT_NE(directBody.find("execute it exactly once"), std::string::npos);
    EXPECT_NE(text.find("PostSL submit invariant violated"), std::string::npos);
}

// RoboCop: Rogue City session 20260809_140551 crashed the RHI thread with
// RIP=0: once DLSS FG turned on, CallOriginalPresent's SL fast-path called
// dxgi!Present through Steam's E9 JMP (gameoverlayrenderer64!OverlayHookD3D3)
// whose internal rendering callback was still NULL. Every other Steam
// transport runs under the NULL-callback VEH recovery and the source-thread
// provenance rule; the SL fast-path must get the same protections.
TEST(DXGISharedSourceTest, SlFastPathSteamTransportIsGuardedLikeEveryOtherSteamTransport) {
    namespace fs = std::filesystem;
    const fs::path originalSource = fs::current_path() / "hook" / "common" / "dxgi_shared_original.cpp";
    ASSERT_TRUE(fs::exists(originalSource));
    const std::string original = ce::test_source::ReadFile(originalSource);
    ASSERT_FALSE(original.empty());

    const size_t fastPath = original.find("if (slLoaded && presentOriginal && presentOriginal != DetourPresent)");
    // The Steam classification must be owner-based (thunk-resolved / load-order),
    // not the priority-ordered loaded-module name (RTSS+Steam coexistence).
    const size_t steamOverlayCheck = original.find("IsCurrentExternalPresentHookSteamChain()", fastPath);
    const size_t workerCheck = original.find("CanRuntimePresentFromWorkerForExternalOverlay(", fastPath);
    const size_t threadCheck = original.find("ShouldInvokeSynchronousExternalOverlayPresentForThreadState(", fastPath);
    const size_t nullGuard = original.find("ScopedSteamNullCallbackRecoveryGuard steamNullCallbackGuard(", fastPath);
    const size_t fastPathReturn = original.find("return presentOriginal(pSwapChain, SyncInterval, Flags);", fastPath);
    ASSERT_NE(fastPath, std::string::npos);
    ASSERT_NE(steamOverlayCheck, std::string::npos);
    ASSERT_NE(workerCheck, std::string::npos);
    ASSERT_NE(threadCheck, std::string::npos);
    ASSERT_NE(nullGuard, std::string::npos);
    ASSERT_NE(fastPathReturn, std::string::npos);
    EXPECT_LT(fastPath, steamOverlayCheck);
    EXPECT_LT(steamOverlayCheck, workerCheck);
    EXPECT_LT(workerCheck, threadCheck);
    EXPECT_LT(threadCheck, nullGuard);
    EXPECT_LT(nullGuard, fastPathReturn);

    EXPECT_NE(original.find("SL fast-path E9 transport", nullGuard), std::string::npos);
    EXPECT_NE(original.find("SL fast-path refusing Steam Present transport on runtime worker", workerCheck),
              std::string::npos);
}

// Regression 20260811_195131 (Talos DLSS FG -> FSR FG switch): the inline-hook
// trampoline re-enters Steam's hook chain when CE prepended over Steam's
// entry jump. The bare trampoline call crashed through Steam's lazy NULL
// rendering callback on the fresh FSR swapchain (RIP=0). Every Steam
// transport must run under the NULL-callback VEH recovery (or the clean
// bypass) before any bare trampoline call is reachable.
TEST(DXGISharedSourceTest, SteamExternalChainTrampolineNeverCalledBareBeforeGuardedTransport) {
    namespace fs = std::filesystem;
    const fs::path originalSource = fs::current_path() / "hook" / "common" / "dxgi_shared_original.cpp";
    ASSERT_TRUE(fs::exists(originalSource));
    const std::string original = ce::test_source::ReadFile(originalSource);
    ASSERT_FALSE(original.empty());

    // Present fast path: the Steam external-chain check and the guarded Steam
    // invoke must precede the bare trampoline call.
    const size_t trampolinePath = original.find("if (presentTrampoline) {");
    ASSERT_NE(trampolinePath, std::string::npos);
    const size_t steamChainCheck = original.find("IsSteamExternalChainTrampoline((void*)presentTrampoline", trampolinePath);
    const size_t guardedSteamInvoke = original.find("TryInvokeGuardedExternalSteamOverlayPresent(", trampolinePath);
    const size_t bareTrampolineCall = original.find("return presentTrampoline(pSwapChain, SyncInterval, Flags);", trampolinePath);
    ASSERT_NE(steamChainCheck, std::string::npos);
    ASSERT_NE(guardedSteamInvoke, std::string::npos);
    ASSERT_NE(bareTrampolineCall, std::string::npos);
    EXPECT_LT(steamChainCheck, guardedSteamInvoke);
    EXPECT_LT(guardedSteamInvoke, bareTrampolineCall);

    // Present1 fast path: same hazard; the clean Present1 bypass (or the
    // guarded Present transport) must precede the bare Present1 trampoline call.
    const size_t present1Entry = original.find("HRESULT CallOriginalPresent1(");
    ASSERT_NE(present1Entry, std::string::npos);
    const size_t present1TrampolinePath = original.find("if (present1Trampoline) {", present1Entry);
    ASSERT_NE(present1TrampolinePath, std::string::npos);
    const size_t present1ChainCheck = original.find("IsSteamExternalChainTrampoline((void*)present1Trampoline", present1TrampolinePath);
    const size_t present1Bypass = original.find("return present1Bypass(pSwapChain, SyncInterval, Flags, pParams);", present1TrampolinePath);
    const size_t barePresent1Call = original.find("return present1Trampoline(pSwapChain, SyncInterval, Flags, pParams);", present1TrampolinePath);
    ASSERT_NE(present1ChainCheck, std::string::npos);
    ASSERT_NE(present1Bypass, std::string::npos);
    ASSERT_NE(barePresent1Call, std::string::npos);
    EXPECT_LT(present1ChainCheck, present1Bypass);
    EXPECT_LT(present1Bypass, barePresent1Call);

    // The helper must resolve both chainable entry-jump forms and gate on the
    // chain owner (not the priority-ordered loaded-module name) and the D3D12
    // API. With Steam AND RTSS both loaded the name cache reports Steam even
    // though RTSS (loaded later) owns the entry jump; classifying by the name
    // alone made CE service RTSS's chain as Steam (session 20260811_233748).
    const fs::path steamSource = fs::current_path() / "hook" / "common" / "dxgi_shared_steam.cpp";
    ASSERT_TRUE(fs::exists(steamSource));
    const std::string steam = ce::test_source::ReadFile(steamSource);
    ASSERT_FALSE(steam.empty());
    const size_t chainHelper = steam.find("bool TrampolineChainsToExternalOverlay(");
    ASSERT_NE(chainHelper, std::string::npos);
    EXPECT_NE(steam.find("code[0] == 0xE9", chainHelper), std::string::npos);
    EXPECT_NE(steam.find("code[1] == 0x25", chainHelper), std::string::npos);
    const size_t steamGate = steam.find("bool IsSteamExternalChainTrampoline(", chainHelper);
    ASSERT_NE(steamGate, std::string::npos);
    EXPECT_NE(steam.find("IsExternalPresentHookSteamChain(externalHook)", steamGate), std::string::npos);
    EXPECT_NE(steam.find("!isD3D12SwapChain", steamGate), std::string::npos);
}

// RTSS + Steam coexistence (session 20260811_233748): when Steam AND RTSS are
// both loaded, the external Present chain belongs to RTSS (loaded later, it
// displaced Steam's entry jump). The Steam guarded machinery must not run on
// RTSS's chain, and the preserved-owner diagnostics must be emitted at install.
// RTSS's restore/rehook cycle still re-enters Steam's handler inside the
// forward, so Steam's NULL-callback patches must stay armed around the bare
// trampoline call whenever Steam is loaded.
TEST(DXGISharedSourceTest, RTSSCoexistenceClassifiesChainByOwnerNotNamePriority) {
    namespace fs = std::filesystem;
    const fs::path steamSource = fs::current_path() / "hook" / "common" / "dxgi_shared_steam.cpp";
    ASSERT_TRUE(fs::exists(steamSource));
    const std::string steam = ce::test_source::ReadFile(steamSource);
    ASSERT_FALSE(steam.empty());

    // The authoritative classifier resolves the thunk target into a module and
    // falls back to load-order evidence for unresolvable thunks.
    const size_t classifier = steam.find("bool IsExternalPresentHookSteamChain(");
    ASSERT_NE(classifier, std::string::npos);
    EXPECT_NE(steam.find("ResolveExternalPresentHookOwnerPath(", classifier), std::string::npos);
    EXPECT_NE(steam.find("IsSteamExternalChainOwnerByLoadOrderEvidence(", classifier), std::string::npos);
    const size_t thunkResolver = steam.find("const void* ResolveExternalPresentHookThunkTarget(");
    ASSERT_NE(thunkResolver, std::string::npos);
    EXPECT_NE(steam.find("code[0] != 0xFF || code[1] != 0x25", thunkResolver), std::string::npos);

    // All Steam-specific routing decisions must go through the owner-based
    // classifier instead of the loaded-module name.
    const size_t forcedBypass = steam.find("bool ShouldForceSteamDX12Bypass(");
    ASSERT_NE(forcedBypass, std::string::npos);
    EXPECT_NE(steam.find("IsCurrentExternalPresentHookSteamChain()", forcedBypass), std::string::npos);
    const size_t guardedInvoke = steam.find("bool TryInvokeGuardedExternalSteamOverlayPresent(");
    ASSERT_NE(guardedInvoke, std::string::npos);
    EXPECT_NE(steam.find("IsCurrentExternalPresentHookSteamChain()", guardedInvoke), std::string::npos);

    // The guarded Steam invoke must not fire for RTSS chains: its Steam callback
    // checks stay gated on the owner classification.
    EXPECT_NE(steam.find("const bool isSteamOverlay = IsCurrentExternalPresentHookSteamChain();", guardedInvoke),
              std::string::npos);

    // Install-time diagnostics resolve and log the foreign hook owner.
    const fs::path hooksSource = fs::current_path() / "hook" / "common" / "dxgi_shared_hooks_present.cpp";
    ASSERT_TRUE(fs::exists(hooksSource));
    const std::string hooks = ce::test_source::ReadFile(hooksSource);
    ASSERT_FALSE(hooks.empty());
    const size_t saveHook = hooks.find("saved for guarded overlay routing");
    ASSERT_NE(saveHook, std::string::npos);
    EXPECT_NE(hooks.find("ResolveExternalPresentHookOwnerPath(hookTarget", saveHook), std::string::npos);
    EXPECT_NE(hooks.find("External hook owner:", saveHook), std::string::npos);

    // The trampoline branch keeps only the passive VEH backstop armed around the
    // bare forward when Steam is loaded alongside a non-Steam chain. It must NOT
    // pre-patch Steam's callback slots there: frame-1 trace 20260812_002958
    // showed RTSS's OSD draw (nested inside Steam's handler) stopping permanently
    // once Steam's overlay drew every frame, while the same Steam+RTSS chain
    // without CE keeps RTSS's OSD alive — altering Steam's memory is the prime
    // suspect for dropping RTSS from the chain.
    const fs::path originalSource = fs::current_path() / "hook" / "common" / "dxgi_shared_original.cpp";
    ASSERT_TRUE(fs::exists(originalSource));
    const std::string original = ce::test_source::ReadFile(originalSource);
    ASSERT_FALSE(original.empty());
    const size_t trampolinePath = original.find("if (presentTrampoline) {");
    ASSERT_NE(trampolinePath, std::string::npos);
    const size_t nestedSteamGuard = original.find("non-Steam external chain with Steam loaded", trampolinePath);
    ASSERT_NE(nestedSteamGuard, std::string::npos);
    EXPECT_LT(trampolinePath, nestedSteamGuard);
    const size_t bareTrampolineForward =
        original.find("return presentTrampoline(pSwapChain, SyncInterval, Flags);", nestedSteamGuard);
    ASSERT_NE(bareTrampolineForward, std::string::npos);
    const std::string trampolineBranchBody =
        original.substr(nestedSteamGuard, bareTrampolineForward - nestedSteamGuard);
    EXPECT_EQ(trampolineBranchBody.find("EnsureSteamNullCallbacksPatched"), std::string::npos);
    EXPECT_NE(original.find("ScopedSteamNullCallbackRecoveryGuard steamNullCallbackGuard(", nestedSteamGuard),
              std::string::npos);

    // Load-order evidence is recorded only from real load notifications.
    const fs::path detectSource = fs::current_path() / "hook" / "main_overlay_detect.cpp";
    ASSERT_TRUE(fs::exists(detectSource));
    const std::string detect = ce::test_source::ReadFile(detectSource);
    ASSERT_FALSE(detect.empty());
    EXPECT_NE(detect.find("NoteModuleLoadedForOverlayCacheFromNotification(base);"), std::string::npos);
    EXPECT_NE(detect.find("NoteModuleLoadedForOverlayCacheFromNotification(moduleNameOrPath);"), std::string::npos);
}
