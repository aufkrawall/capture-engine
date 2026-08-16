#include "test_dxgi_shared_shared.h"

// Regression (session 20260811_214252): late inject into a running Talos with
// DLSS FG suspended missed the Streamline FG signal and the runtime-ownership
// latch (sl.dlssg was loaded before hook installation), so the planner's DLSS
// runtime mode was the only FG evidence. The dedicated overlay queue was
// created and the first backbuffer-drawing submit on it removed the device
// with DXGI_ERROR_ACCESS_DENIED (0x887A002B); UE5 then fatal-exited. The
// dedicated queue must be disabled for NVIDIA DLSS FG in every detection
// state, not only when the Streamline latch is present.
TEST(DXGISharedTest, DedicatedOverlayQueueDisabledForNvidiaDLSSFrameGeneration) {
    using ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForNvidiaFrameGeneration;
    using ce::fg_runtime::RuntimeMode;

    // Streamline-signalled DLSS FG (the healthy startup-inject latch).
    EXPECT_TRUE(ShouldDisableDedicatedOverlayQueueForNvidiaFrameGeneration(true, RuntimeMode::kDLSSFG));
    // The SL signal dominates even when the planner has not classified yet.
    EXPECT_TRUE(ShouldDisableDedicatedOverlayQueueForNvidiaFrameGeneration(true, RuntimeMode::kStreamlineNoFG));
    // THE late-inject regression: planner-classified DLSS FG without any
    // Streamline/runtime-ownership latch. Must disable the dedicated queue.
    EXPECT_TRUE(ShouldDisableDedicatedOverlayQueueForNvidiaFrameGeneration(false, RuntimeMode::kDLSSFG));

    // Non-DLSS states keep the generic policy decision (FSR is handled by
    // ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration).
    EXPECT_FALSE(ShouldDisableDedicatedOverlayQueueForNvidiaFrameGeneration(false, RuntimeMode::kOff));
    EXPECT_FALSE(ShouldDisableDedicatedOverlayQueueForNvidiaFrameGeneration(false, RuntimeMode::kFSRFG));
    EXPECT_FALSE(ShouldDisableDedicatedOverlayQueueForNvidiaFrameGeneration(false, RuntimeMode::kNvidiaSmoothMotion));
    EXPECT_FALSE(ShouldDisableDedicatedOverlayQueueForNvidiaFrameGeneration(false, RuntimeMode::kStreamlineNoFG));
    EXPECT_FALSE(ShouldDisableDedicatedOverlayQueueForNvidiaFrameGeneration(false, RuntimeMode::kUnknown));
}

// The dedicated overlay queue may only execute pure-offscreen overlay work.
// Any recorded list that touches the swapchain backbuffer (direct RTV draw or
// offscreen-copy composite) must be submitted on the game queue; the
// documented failure mode is DXGI_ERROR_ACCESS_DENIED (0x887A002B) with device
// removal on the first submit (logs/20260606_153428 and 20260811_214252).
TEST(DXGISharedTest, DedicatedOverlayQueueSubmitRequiresOffscreenList) {
    using ce::dx12_overlay_policy::ShouldUseDedicatedQueueForOverlaySubmit;

    // Pure-offscreen work may use the dedicated queue when it exists and the
    // policy allows it.
    EXPECT_TRUE(ShouldUseDedicatedQueueForOverlaySubmit(true, true, false));
    // No dedicated queue, policy denied, or backbuffer-touching list -> game
    // queue.
    EXPECT_FALSE(ShouldUseDedicatedQueueForOverlaySubmit(false, true, false));
    EXPECT_FALSE(ShouldUseDedicatedQueueForOverlaySubmit(true, false, false));
    EXPECT_FALSE(ShouldUseDedicatedQueueForOverlaySubmit(true, true, true));
    EXPECT_FALSE(ShouldUseDedicatedQueueForOverlaySubmit(false, false, true));
    // Backbuffer-touching list stays on the game queue even when the policy
    // would otherwise allow the dedicated queue.
    EXPECT_FALSE(ShouldUseDedicatedQueueForOverlaySubmit(true, true, true));
}

// Source invariant: both ProcessFrame overlay submit sites must route through
// the backbuffer-touching-list guard, and the runtime policy gate must use the
// NVIDIA DLSS disable predicate (so a future planner state cannot resurrect
// the dedicated-queue device removal).
TEST(DXGISharedSourceTest, DedicatedOverlayQueueSubmitGuardsBackbufferLists) {
    namespace fs = std::filesystem;
    const fs::path tailSource = fs::current_path() / "hook" / "apis" / "dx12_hook_process_session_draw_tail.cpp";
    const fs::path renderSource = fs::current_path() / "hook" / "apis" / "dx12_hook_overlay_render.cpp";
    const fs::path policySource = fs::current_path() / "hook" / "apis" / "dx12_hook_overlay_dedicated_queue.cpp";
    ASSERT_TRUE(fs::exists(tailSource));
    ASSERT_TRUE(fs::exists(renderSource));
    ASSERT_TRUE(fs::exists(policySource));

    const std::string tailText = ce::test_source::ReadLogicalSource(tailSource);
    const std::string renderText = ce::test_source::ReadLogicalSource(renderSource);
    const std::string policyText = ce::test_source::ReadLogicalSource(policySource);
    ASSERT_FALSE(tailText.empty());
    ASSERT_FALSE(renderText.empty());
    ASSERT_FALSE(policyText.empty());

    // The submit-time guard must gate both ProcessFrame submit sites.
    EXPECT_NE(tailText.find("ShouldUseDedicatedQueueForOverlaySubmit"), std::string::npos);
    EXPECT_NE(renderText.find("ShouldUseDedicatedQueueForOverlaySubmit"), std::string::npos);
    // The ProcessFrame overlay list always touches the backbuffer.
    EXPECT_NE(tailText.find("recordedListTouchesBackbuffer=*/true"), std::string::npos);
    // The runtime policy gate must disable the dedicated queue for NVIDIA DLSS
    // FG in every detection state.
    EXPECT_NE(policyText.find("ShouldDisableDedicatedOverlayQueueForNvidiaFrameGeneration"), std::string::npos);
}

// The deep hook below a multi-overlay foreign Present chain is CE's only view of a swapchain
// created before injection (session 20260812_140930). A pushes-only prolog rule refuses
// dxgi!CDXGISwapChain::Present, whose first instruction is a shadow-space save, so that shape
// must be accepted with a zero stack undo.
TEST(DXGISharedTest, DeepHookPrologAcceptsTheShadowSpaceSaveThatOpensDxgiPresent) {
    // 48 89 5C 24 10 = mov [rsp+10h], rbx (the live dxgi!Present prolog, resume offset 5)
    const unsigned char dxgiPresentProlog[] = {0x48, 0x89, 0x5C, 0x24, 0x10};
    int stackDelta = -1;
    ASSERT_TRUE(ce::inline_hook_policy::TryComputeDeepHookPrologStackDelta(
        dxgiPresentProlog, static_cast<int>(sizeof(dxgiPresentProlog)), &stackDelta));
    EXPECT_EQ(stackDelta, 0);

    // mod=00 (no displacement) and mod=10 (disp32) forms of the same save.
    const unsigned char noDisp[] = {0x48, 0x89, 0x1C, 0x24};
    stackDelta = -1;
    ASSERT_TRUE(ce::inline_hook_policy::TryComputeDeepHookPrologStackDelta(noDisp, static_cast<int>(sizeof(noDisp)),
                                                                          &stackDelta));
    EXPECT_EQ(stackDelta, 0);
    const unsigned char disp32[] = {0x4C, 0x89, 0xA4, 0x24, 0x10, 0x01, 0x00, 0x00};
    stackDelta = -1;
    ASSERT_TRUE(ce::inline_hook_policy::TryComputeDeepHookPrologStackDelta(disp32, static_cast<int>(sizeof(disp32)),
                                                                          &stackDelta));
    EXPECT_EQ(stackDelta, 0);
}

TEST(DXGISharedTest, DeepHookPrologStackDeltaCountsPushesAndStackReservation) {
    // 40 55 53 56 57 = push rbp / push rbx / push rsi / push rdi — the
    // CreateSwapChainForHwnd prolog CE already deep-hooks in production.
    const unsigned char pushes[] = {0x40, 0x55, 0x53, 0x56, 0x57};
    int stackDelta = -1;
    ASSERT_TRUE(
        ce::inline_hook_policy::TryComputeDeepHookPrologStackDelta(pushes, static_cast<int>(sizeof(pushes)), &stackDelta));
    EXPECT_EQ(stackDelta, 32);

    // Mixed shadow-space save + pushes + sub rsp, imm8.
    const unsigned char mixed[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20};
    stackDelta = -1;
    ASSERT_TRUE(
        ce::inline_hook_policy::TryComputeDeepHookPrologStackDelta(mixed, static_cast<int>(sizeof(mixed)), &stackDelta));
    EXPECT_EQ(stackDelta, 8 + 8 + 8 + 0x20);

    // sub rsp, imm32
    const unsigned char subImm32[] = {0x48, 0x81, 0xEC, 0x70, 0x01, 0x00, 0x00};
    stackDelta = -1;
    ASSERT_TRUE(ce::inline_hook_policy::TryComputeDeepHookPrologStackDelta(subImm32, static_cast<int>(sizeof(subImm32)),
                                                                          &stackDelta));
    EXPECT_EQ(stackDelta, 0x170);

    // An empty prolog (patch at the entry itself) undoes nothing.
    stackDelta = -1;
    ASSERT_TRUE(ce::inline_hook_policy::TryComputeDeepHookPrologStackDelta(pushes, 0, &stackDelta));
    EXPECT_EQ(stackDelta, 0);
}

TEST(DXGISharedTest, DeepHookPrologRefusesShapesWithAnUnknownStackEffect) {
    // lea rbp,[rsp-70h] — reads RSP but its effect on the frame is not one CE can undo here.
    const unsigned char lea[] = {0x48, 0x8D, 0x6C, 0x24, 0x90};
    int stackDelta = 0;
    EXPECT_FALSE(
        ce::inline_hook_policy::TryComputeDeepHookPrologStackDelta(lea, static_cast<int>(sizeof(lea)), &stackDelta));

    // mov [rax+10h], rbx — a store through another base register, not a shadow-space save.
    const unsigned char nonRspStore[] = {0x48, 0x89, 0x58, 0x10};
    EXPECT_FALSE(ce::inline_hook_policy::TryComputeDeepHookPrologStackDelta(
        nonRspStore, static_cast<int>(sizeof(nonRspStore)), &stackDelta));

    // mov rsp, rsp — register-to-register form (mod=11, rm=100) must not be read as a SIB
    // store just because rm names the SIB escape.
    const unsigned char regToReg[] = {0x48, 0x89, 0xE4, 0x90};
    EXPECT_FALSE(ce::inline_hook_policy::TryComputeDeepHookPrologStackDelta(
        regToReg, static_cast<int>(sizeof(regToReg)), &stackDelta));

    // add rsp, 8 — grows the frame back; not a prolog shape CE may undo blindly.
    const unsigned char addRsp[] = {0x48, 0x83, 0xC4, 0x08};
    EXPECT_FALSE(ce::inline_hook_policy::TryComputeDeepHookPrologStackDelta(
        addRsp, static_cast<int>(sizeof(addRsp)), &stackDelta));

    // A shadow-space save truncated by the resume offset must be refused, never
    // half-consumed.
    const unsigned char truncated[] = {0x48, 0x89, 0x5C, 0x24, 0x10};
    EXPECT_FALSE(ce::inline_hook_policy::TryComputeDeepHookPrologStackDelta(truncated, 4, &stackDelta));
}

// Session 20260812_150918: the caller logged the foreign `E9 at 00007FFD5C049960` and the deep
// install, milliseconds later, read `byte=0x48` — the ORIGINAL first byte — and refused.
// RTSS restores the entry bytes, calls through, and re-patches on every present, so byte 0 is
// a coin flip on a target that is very much hooked. Present1 happened to install and Present
// did not, and Present is the entry the game uses, so the overlay never appeared.
//
// The caller therefore passes the span it observed, and the deep hook honours it whatever the
// live sample shows. A larger span than the real one is safe (the foreign trampoline resumes
// below CE's patch and still reaches it); a smaller one would land inside bytes the foreign
// tool rewrites, so an unobservable patch must use the widest recognized form.
TEST(DXGISharedTest, DeepHookHonoursACallerObservedEntryPatchSpanWhenTheSampleReadsClean) {
    namespace fs = std::filesystem;
    const fs::path deepSource = fs::current_path() / "hook" / "wrappers" / "inline_hook_deep.cpp";
    ASSERT_TRUE(fs::exists(deepSource));
    const std::string deep = ce::test_source::ReadFile(deepSource);
    ASSERT_FALSE(deep.empty());

    const size_t detection = deep.find("int existingJmpSize = 0;");
    ASSERT_NE(detection, std::string::npos);
    // A clean sample plus a caller-observed span is accepted instead of refused...
    const size_t hintBranch = deep.find("} else if (minimumExternalPatchSize > 0) {", detection);
    ASSERT_NE(hintBranch, std::string::npos);
    // ...and a visible span narrower than the observed one is widened, never narrowed.
    const size_t widen = deep.find("if (minimumExternalPatchSize > existingJmpSize) {", hintBranch);
    ASSERT_NE(widen, std::string::npos);
    EXPECT_NE(deep.find("existingJmpSize = minimumExternalPatchSize;", widen), std::string::npos);
    // With no observation at all the strict refusal stays.
    EXPECT_NE(deep.find("No external hook at byte 0 of %p", detection), std::string::npos);

    const fs::path header = fs::current_path() / "hook" / "wrappers" / "inline_hook.h";
    const std::string headerText = ce::test_source::ReadFile(header);
    ASSERT_FALSE(headerText.empty());
    EXPECT_NE(headerText.find("int minimumExternalPatchSize = 0"), std::string::npos);
}

// The logs root is a fallback for an early crash dump, not a session artifact directory.
// Archiving the installed symbols there left a stray `installed\captureengine\logs\symbols`
// full of PDBs beside the per-session folders (reported 2026-08-12), which nothing consumes:
// the real session directory stages its own copy moments later.
TEST(DXGISharedTest, InstalledSymbolsAreOnlyArchivedIntoARealSessionDirectory) {
    namespace fs = std::filesystem;
    const fs::path entrySource = fs::current_path() / "captureengine" / "main_entry.cpp";
    const fs::path handlerSource = fs::current_path() / "common" / "crash_handler.cpp";
    ASSERT_TRUE(fs::exists(entrySource));
    ASSERT_TRUE(fs::exists(handlerSource));
    const std::string entry = ce::test_source::ReadFile(entrySource);
    const std::string handler = ce::test_source::ReadFile(handlerSource);
    ASSERT_FALSE(entry.empty());
    ASSERT_FALSE(handler.empty());

    // The early call opts out of archiving exactly when there is no session directory name.
    EXPECT_NE(entry.find("SetCrashDumpDirectory(earlyLogsDir, /*archiveInstalledSymbols=*/!g_SessionDirName.empty())"),
              std::string::npos);

    // The dump directory itself is still set in that state - an early crash must land somewhere.
    const size_t setter = handler.find("void SetCrashDumpDirectory(const std::string& dir, bool archiveInstalledSymbols)");
    ASSERT_NE(setter, std::string::npos);
    const size_t storage = handler.find("CrashDumpDirectoryStorage() = dir;", setter);
    const size_t guard = handler.find("if (archiveInstalledSymbols) {", setter);
    ASSERT_NE(storage, std::string::npos);
    ASSERT_NE(guard, std::string::npos);
    EXPECT_LT(storage, guard);
}

// Sessions 20260812_153840 and 20260814_035452 establish both sides of the ownership rule. When runtime-owned
// FSR presentation does not re-enter CE through DXGI, the callback is the only observer and must advance timing.
// Once CE has a deep-body Present observer below Steam/RTSS, callback plus Present samples the same output twice:
// the overlay reports ~288 instead of ~144 FPS and the graph alternates tiny/normal frame times.
TEST(DXGISharedTest, FFXFrameTimingHasExactlyOneDisplayedOutputObserver) {
    namespace fs = std::filesystem;
    const fs::path ffxSource = fs::current_path() / "hook" / "apis" / "dx12_hook_ffx_metrics.cpp";
    const fs::path callbackSource = fs::current_path() / "hook" / "apis" / "dx12_hook_ffx.cpp";
    ASSERT_TRUE(fs::exists(ffxSource));
    ASSERT_TRUE(fs::exists(callbackSource));
    const std::string ffx = ce::test_source::ReadFile(ffxSource);
    const std::string callbackFfx = ce::test_source::ReadFile(callbackSource);
    ASSERT_FALSE(ffx.empty());
    ASSERT_FALSE(callbackFfx.empty());

    const size_t callback = ffx.find("void DX12_UpdateFFXPresentCallbackFrameTiming(");
    ASSERT_NE(callback, std::string::npos);
    const size_t deepObserver = ffx.find("DXGIShared::IsPresentInterceptedBelowForeignChain()", callback);
    const size_t timingDecision = ffx.find("ShouldSampleFrameTimingFromFFXPresentCallback(", deepObserver);
    const size_t timingGate = ffx.find("if (callbackSamplesFrameTiming) {", timingDecision);
    const size_t metricsTick = ffx.find("metrics->Update(PerfLogger::GetQpcUs());", timingGate);
    ASSERT_NE(deepObserver, std::string::npos);
    ASSERT_NE(timingDecision, std::string::npos);
    ASSERT_NE(timingGate, std::string::npos);
    ASSERT_NE(metricsTick, std::string::npos);
    EXPECT_LT(deepObserver, timingDecision);
    EXPECT_LT(timingDecision, timingGate);
    EXPECT_LT(timingGate, metricsTick);

    // The FG-state publication stays where it was — it is a different concern from the tick.
    EXPECT_NE(callbackFfx.find("PublishOverlayFGMetrics(perf, plan"), std::string::npos);
}

// Draw order in a dxgi!Present entry chain is the reverse of hook order: every participant
// composites BEFORE it forwards, so the one that runs last ends up on top. CE's entry prepend
// made it the FIRST participant and therefore the bottom layer — Steam's fullscreen overlay and
// RTSS's OSD drew over CE's overlay (user report on build 0.1.5959, Talos/Strange Brigade with
// Steam + RTSS). Below the chain CE composites after all of them.
//
// The single-overlay case is the one that changed here, so it needs the escape hatch: the body
// patch can be refused (thread quiescence, an unrecognized prolog, a 32-bit target), and a
// session with no Present view at all costs the overlay entirely, which is far worse than being
// the bottom layer. The prepend is therefore taken as a fallback — but only with fewer than two
// overlays, because with two the prepend is what corrupts their saved chains.
TEST(DXGISharedSourceTest, BelowChainViewFallsBackToThePrependOnlyAgainstASingleOverlay) {
    namespace fs = std::filesystem;
    const fs::path installSource = fs::current_path() / "hook" / "common" / "dxgi_shared_hooks_present.cpp";
    ASSERT_TRUE(fs::exists(installSource));
    const std::string install = ce::test_source::ReadFile(installSource);
    ASSERT_FALSE(install.empty());

    const size_t leaveEntry = install.find("ShouldLeavePresentEntryToForeignOverlayChain(");
    ASSERT_NE(leaveEntry, std::string::npos);
    const size_t fallbackDecision =
        install.find("MayPrependPresentEntryWhenBelowChainViewUnavailable(loadedOverlayCount)", leaveEntry);
    ASSERT_NE(fallbackDecision, std::string::npos);

    // The published leave-entry state is reverted before falling through to the prepend;
    // otherwise CE would own entry bytes while every forward still ran the live entry.
    const size_t revert =
        install.find("dxgi_shared_s_presentEntryLeftToForeignChain.store(false", fallbackDecision);
    ASSERT_NE(revert, std::string::npos);
    const size_t prependInstall = install.find("InlineHook::InstallPublished(presentAddr", revert);
    ASSERT_NE(prependInstall, std::string::npos);
    EXPECT_NE(install.find("dxgi_shared_oPresent = previousPresent;", revert), std::string::npos);
    EXPECT_LT(install.find("dxgi_shared_oPresent = previousPresent;", revert), prependInstall);

    // Present1 must NOT be deep-hooked once Present failed and the fallback is available: a lone
    // Present1 deep trampoline makes IsPresentInterceptedBelowForeignChain() true while CE owns
    // the Present entry bytes, and those two modes contradict each other (below the chain CE
    // must never invoke a foreign handler; prepended it must).
    const size_t helper = install.find("bool InstallPresentBodyHooksBelowForeignChain(");
    ASSERT_NE(helper, std::string::npos);
    EXPECT_NE(install.find("if (!present1Addr || (!haveBodyView && prependFallbackAvailable)) {", helper),
              std::string::npos);
}

// Below the chain, `oPresent` IS the live foreign entry. The Streamline present route forwards
// through it directly instead of through CallOriginalPresent (the only place that prefers the
// deep trampoline), so activating SL routing there would send the call back through Steam/RTSS
// and straight into CE's own body hook again — unbounded recursion. It was previously
// unreachable only by accident (the mode leaves oPresentTrampoline null); with FG interposers
// now allowed into this mode it must be refused outright.
TEST(DXGISharedSourceTest, StreamlinePresentRoutingIsRefusedBelowAForeignChain) {
    namespace fs = std::filesystem;
    const fs::path routingSource = fs::current_path() / "hook" / "common" / "dxgi_shared_steam_routing.cpp";
    ASSERT_TRUE(fs::exists(routingSource));
    const std::string routing = ce::test_source::ReadFile(routingSource);
    ASSERT_FALSE(routing.empty());

    const size_t detect = routing.find("void DetectSLPresentHook() {");
    ASSERT_NE(detect, std::string::npos);
    const size_t guard =
        routing.find("if (IsPresentEntryLeftToForeignChain() || IsPresentInterceptedBelowForeignChain()) {", detect);
    ASSERT_NE(guard, std::string::npos);
    // Before any byte inspection of the entry, and before the activation itself.
    const size_t activation = routing.find("dxgi_shared_s_slRoutingActive.store(true", detect);
    ASSERT_NE(activation, std::string::npos);
    EXPECT_LT(guard, activation);
    const size_t byteProbe = routing.find("auto* funcBytes = (const uint8_t*)dxgi_shared_oPresent;", detect);
    ASSERT_NE(byteProbe, std::string::npos);
    EXPECT_LT(guard, byteProbe);
}

// The module a Present implementation belongs to must be resolved FROM ITS ADDRESS, never by
// name: ReShade, SpecialK and OptiScaler all ship their proxy as `dxgi.dll`, so
// GetModuleHandleA("dxgi.dll") returns whichever image the loader lists first. Session
// 20260812_155205 logged `E9 at 00007FFD5C049960 targets 00007FFD1C040000 (outside dxgi.dll
// 00007FFCB8460000-00007FFCB9CF0000)` — presentAddr is not inside the printed range at all, so
// the foreign-jump verdict was accidental in both directions.
TEST(DXGISharedSourceTest, ForeignPresentJumpIsClassifiedAgainstThePresentOwningModule) {
    namespace fs = std::filesystem;
    const fs::path installSource = fs::current_path() / "hook" / "common" / "dxgi_shared_hooks_present.cpp";
    ASSERT_TRUE(fs::exists(installSource));
    const std::string install = ce::test_source::ReadFile(installSource);
    ASSERT_FALSE(install.empty());

    const size_t classification = install.find("bool externalJmpDetected = false;");
    ASSERT_NE(classification, std::string::npos);
    const size_t resolve = install.find("reinterpret_cast<LPCSTR>(presentAddr), &hPresentModule);", classification);
    ASSERT_NE(resolve, std::string::npos);
    const size_t moduleInfo = install.find("GetModuleInformation(GetCurrentProcess(), hPresentModule", resolve);
    ASSERT_NE(moduleInfo, std::string::npos);
    // No by-name resolution may remain in the classification (the string still appears in the
    // comment that explains why, so this looks for the assignment, not the mention).
    EXPECT_EQ(install.find("hDXGI = GetModuleHandleA", classification), std::string::npos);
    EXPECT_EQ(install.find("(uintptr_t)hDXGI", classification), std::string::npos);
}

// Session 20260812_195840 (Talos + Steam + RTSS + a game-directory `dxgi.dll` proxy, build
// 0.1.5961): CE's deep body hook installed and reported `[OVERLAY LAYER] CE composites BELOW the
// foreign Present chain`, and Steam STILL drew on top. The log names the reason two lines apart:
// `presentAddr=00007FF95309C140 is in module: …\Talos1\Binaries\Win64\dxgi.dll` and `no visible
// jump at 00007FF95309C140` — CE was hooking the PROXY's Present method, which has no foreign
// patch on it because Steam and RTSS patched the system function the proxy forwards to. A hook
// in the proxy's prolog is above its post-processing pass AND above everything below it.
//
// The temp swapchain therefore has to come from the SYSTEM dxgi factory, resolved by full path
// (the proxy carries the same base name). The result is only accepted when its Present really
// lands inside the system image, so a proxy that also hooks real factory vtables falls back to
// the historical view instead of silently regressing.
TEST(DXGISharedSourceTest, PresentHooksTargetTheTerminalSystemDXGIPresentBelowAProxy) {
    namespace fs = std::filesystem;
    const fs::path installSource = fs::current_path() / "hook" / "apis" / "dx12_hook_hook_install.cpp";
    ASSERT_TRUE(fs::exists(installSource));
    const std::string install = ce::test_source::ReadFile(installSource);
    ASSERT_FALSE(install.empty());

    const size_t systemFactory = install.find("DXGIShared::GetSystemDXGIModuleHandle();");
    ASSERT_NE(systemFactory, std::string::npos);
    // A second factory is created only when a proxy is actually in play.
    EXPECT_NE(install.find("hSystemDXGI != nullptr && hSystemDXGI != hDXGI", systemFactory), std::string::npos);
    EXPECT_NE(install.find("if (proxyDXGILoaded) {", systemFactory), std::string::npos);
    const size_t validation = install.find("DXGIShared::IsAddressInsideSystemDXGI(terminalPresent)", systemFactory);
    ASSERT_NE(validation, std::string::npos);
    // Rejection releases the swapchain and falls through to the historical path.
    const size_t fallback = install.find("if (!pSwapChain) {", validation);
    ASSERT_NE(fallback, std::string::npos);
    EXPECT_NE(install.find("dx12_hook_oCreateSwapChainForHwndGlobal(pFactory", fallback), std::string::npos);
    EXPECT_NE(install.find("pTerminalFactory->Release();"), std::string::npos);

    // The saved predecessor belongs to the vtable CE hooked. Substituting it for an arbitrary
    // factory's slot would call a proxy's method with a real factory `this` — type confusion.
    const size_t slotHelper = install.find("HRESULT CreateTempSwapChainViaFactorySlot(");
    ASSERT_NE(slotHelper, std::string::npos);
    const size_t guardedSubstitution =
        install.find("reinterpret_cast<void*>(slot) == reinterpret_cast<void*>(DetourCreateSwapChainForHwndGlobal)",
                     slotHelper);
    ASSERT_NE(guardedSubstitution, std::string::npos);
    EXPECT_LT(guardedSubstitution, install.find("slot = dx12_hook_oCreateSwapChainForHwndGlobal;", slotHelper));

    // The system module is resolved by FULL PATH, never by base name: the proxy shares it.
    const fs::path hooksSource = fs::current_path() / "hook" / "common" / "dxgi_shared_hooks.cpp";
    ASSERT_TRUE(fs::exists(hooksSource));
    const std::string hooks = ce::test_source::ReadFile(hooksSource);
    ASSERT_FALSE(hooks.empty());
    const size_t resolver = hooks.find("HMODULE GetSystemDXGIModuleHandle() {");
    ASSERT_NE(resolver, std::string::npos);
    EXPECT_NE(hooks.find("GetSystemDirectoryA(path, MAX_PATH)", resolver), std::string::npos);
    EXPECT_NE(hooks.find("GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, path, &systemModule", resolver),
              std::string::npos);
    EXPECT_EQ(hooks.find("GetModuleHandleA(\"dxgi.dll\")", resolver), std::string::npos);
}

// Session 20260812_201336, both crashes of the first launch: CE called the system factory's
// CreateSwapChainForHwnd slot as it found it, which entered RTSS and then Steam.
//   * PID 9792: `capture_hook!CreateTempSwapChainViaFactorySlot -> RTSSHooks64 ->
//     gameoverlayrenderer64!OverlayHookD3D3 -> 0x0` — DEP execute at address 0, RAX=0. Steam's
//     overlay dispatches through callback slots that stay NULL until it has rendered on a real
//     game swapchain.
//   * PID 19828: 0xC00000FD, a stack of `gameoverlayrenderer64!OverlayHookD3D3+0x14bc4` calling
//     itself until the stack was gone — the same Steam dispatch, same entry.
// The second launch of that same build worked only because Steam had initialized by then, which
// is a race, not a fix. CE must enter NO foreign code from the install path: a slot owned by a
// foreign module is refused, and a foreign entry patch on the real function is bypassed.
TEST(DXGISharedSourceTest, TempSwapChainCreationNeverEntersAForeignOverlayHandler) {
    namespace fs = std::filesystem;
    const fs::path installSource = fs::current_path() / "hook" / "apis" / "dx12_hook_hook_install.cpp";
    ASSERT_TRUE(fs::exists(installSource));
    const std::string install = ce::test_source::ReadFile(installSource);
    ASSERT_FALSE(install.empty());

    const size_t helper = install.find("HRESULT CreateTempSwapChainViaFactorySlot(");
    ASSERT_NE(helper, std::string::npos);
    const size_t call = install.find("return slot(factory, queue, hwnd, desc, nullptr, nullptr, out);", helper);
    ASSERT_NE(call, std::string::npos);

    // A slot that does not resolve into the system DXGI image belongs to a foreign overlay and
    // is refused before the call.
    const size_t ownershipGuard =
        install.find("!DXGIShared::IsAddressInsideSystemDXGI(reinterpret_cast<const void*>(slot))", helper);
    ASSERT_NE(ownershipGuard, std::string::npos);
    EXPECT_LT(ownershipGuard, call);

    // A foreign ENTRY patch on the real function is skipped, never executed. The entry-shape
    // probe moved into the shared factory-slot policy header so the CreateDXGIFactory1 export
    // bypass and this slot use the same predicate.
    const size_t patchProbe =
        install.find("ce::dx12_factory_slot::HasForeignEntryJump(reinterpret_cast<const void*>(slot))", helper);
    ASSERT_NE(patchProbe, std::string::npos);
    EXPECT_LT(patchProbe, call);
    const size_t bypass = install.find("InlineHook::CreateBypassTrampoline(reinterpret_cast<void*>(slot))", patchProbe);
    ASSERT_NE(bypass, std::string::npos);
    EXPECT_LT(bypass, call);
    // An unbypassable patch refuses rather than running the foreign handler.
    EXPECT_NE(install.find("if (!bypass) {", bypass), std::string::npos);
    EXPECT_LT(install.find("if (!bypass) {", bypass), call);
}

// Cyberpunk 20260816_045933: Steam's overlay was loaded before the game's first D3D12 device, so
// DX12Hook::Init deferred the eager temp swapchain and the service pass fell back to the guarded
// system-DXGI route. No dxgi proxy exists in that game, so the guarded route — gated on
// `hSystemDXGI != hDXGI` — never ran at all: every attempt logged `Failed to create temp swapchain
// (hr=0x80004005)` from the untouched initial E_FAIL, rebuilt the CreateDXGIFactory1 bypass
// trampoline into a fresh executable pool (65 pools in one second), and the process never got
// Present hooks or an overlay.
//
// What makes the route safe next to a foreign overlay is the guarded creation itself, not the
// presence of a proxy: with no proxy, the factory built from the bypassed genuine export already
// IS the system factory. The unguarded historical fallback stays deferred, and the retry is
// bounded so a structurally refused slot cannot cost a throwaway D3D12 device forever.
TEST(DXGISharedSourceTest, GuardedTempSwapchainRouteRunsWithoutADXGIProxy) {
    namespace fs = std::filesystem;
    const std::string install =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "apis" / "dx12_hook_hook_install.cpp");
    const std::string main =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "apis" / "dx12_hook_main.cpp");
    ASSERT_FALSE(install.empty());
    ASSERT_FALSE(main.empty());

    // No proxy + the guarded-only caller: create through the factory already built from the
    // bypassed genuine export, with the same terminal-Present validation as the proxy branch.
    const size_t noProxyRoute = install.find("} else if (hSystemDXGI && guardedSystemRouteOnly) {");
    ASSERT_NE(noProxyRoute, std::string::npos);
    const size_t guardedCreate =
        install.find("CreateTempSwapChainViaFactorySlot(pFactory, pQueue, hwnd, &scd, &pSwapChain)", noProxyRoute);
    ASSERT_NE(guardedCreate, std::string::npos);
    const size_t guardedValidation =
        install.find("DXGIShared::IsAddressInsideSystemDXGI(terminalPresent)", guardedCreate);
    ASSERT_NE(guardedValidation, std::string::npos);

    // The unguarded historical fallback still refuses to run for the guarded-only caller.
    EXPECT_NE(install.find("if (!pSwapChain && guardedSystemRouteOnly) {"), std::string::npos);

    // The retry is bounded: every attempt builds a throwaway device/queue/window.
    const size_t retry = main.find("void TryInstallPresentHooksViaGuardedTempSwapchain(const char* reason) {");
    ASSERT_NE(retry, std::string::npos);
    EXPECT_NE(main.find("kMaxGuardedTempSwapchainAttempts", retry), std::string::npos);

    // A bypass trampoline is a pure function of (target, disk bytes, resume offset), so retries
    // reuse it instead of reserving a new executable pool per attempt.
    const std::string deep =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "wrappers" / "inline_hook_deep.cpp");
    ASSERT_FALSE(deep.empty());
    const size_t bypassFn = deep.find("void* CreateBypassTrampoline(void* target) {");
    ASSERT_NE(bypassFn, std::string::npos);
    const size_t cacheLookup = deep.find("s_bypassTrampolines.find(target)", bypassFn);
    ASSERT_NE(cacheLookup, std::string::npos);
    const size_t slotAllocation = deep.find("GetTrampolineSlot(target)", bypassFn);
    ASSERT_NE(slotAllocation, std::string::npos);
    EXPECT_LT(cacheLookup, slotAllocation);
    // Keyed on the resume offset too, so a later longer foreign patch never reuses a trampoline
    // that would resume inside it.
    EXPECT_NE(deep.find("cached->second.resumeOffset == resumeOffset", bypassFn), std::string::npos);
    EXPECT_NE(deep.find("s_bypassTrampolines[target] = BypassTrampolineEntry{trampoline, resumeOffset};", bypassFn),
              std::string::npos);
}
