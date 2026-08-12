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
