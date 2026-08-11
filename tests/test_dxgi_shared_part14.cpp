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
