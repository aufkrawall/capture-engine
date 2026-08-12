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

// Session 20260812_153840 (Talos, DLSS FG -> FSR FG, Steam + RTSS active, build 0.1.5957):
// the overlay stayed visible and GPU load kept updating, but the FPS readout and the frametime
// graph froze at the switch. The log has ZERO DetourPresent lines after 15:39:29.889 while the
// game keeps submitting (ECL heartbeats every ~2.7 s): once the native FSR runtime owns
// presentation it presents from its own swapchain, so neither CE's Present detour nor its
// swapchain wrapper is entered — and those are the only two places that advance the frame-time
// history. The FFX present callback is CE's only per-frame present observation there.
TEST(DXGISharedTest, FFXPresentCallbackAdvancesFrameTimingWhileTheRuntimeOwnsPresentation) {
    namespace fs = std::filesystem;
    const fs::path ffxSource = fs::current_path() / "hook" / "apis" / "dx12_hook_ffx.cpp";
    ASSERT_TRUE(fs::exists(ffxSource));
    const std::string ffx = ce::test_source::ReadFile(ffxSource);
    ASSERT_FALSE(ffx.empty());

    const size_t callback = ffx.find("uint32_t DX12_RenderOverlayViaFFXPresentCallback(");
    ASSERT_NE(callback, std::string::npos);
    const size_t metricsTick = ffx.find("perf->Update(PerfLogger::GetQpcUs());", callback);
    ASSERT_NE(metricsTick, std::string::npos);

    // Gated on runtime ownership: PerformanceMetrics::Update is a single-writer hot path, so
    // when the runtime does NOT own presentation the Present detour must stay the only writer.
    const size_t gate = ffx.rfind("if (ffxRuntimeOwnsNativeFSRPresentation) {", metricsTick);
    ASSERT_NE(gate, std::string::npos);
    EXPECT_LT(gate, metricsTick);
    EXPECT_LT(metricsTick - gate, 200u);

    // The FG-state publication stays where it was — it is a different concern from the tick.
    EXPECT_NE(ffx.find("PublishOverlayFGMetrics(perf, plan", metricsTick), std::string::npos);
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
    // Only when a proxy is actually in play — no extra factory in the common case.
    EXPECT_NE(install.find("hSystemDXGI && hSystemDXGI != hDXGI", systemFactory), std::string::npos);
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
