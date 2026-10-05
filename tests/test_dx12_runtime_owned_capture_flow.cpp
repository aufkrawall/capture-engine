#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

// Capture must not depend on the normal overlay backend. On a runtime-owned FSR
// FG chain (FFX present callback owns the overlay) CE never initializes that
// backend, and after a DLSS FG -> FSR FG switch it had been torn down: InitOverlayBackend
// then ended every Present before InitOverlaySyncAndFocusHold's capture decision and no recording
// went live (logs/20260929_142415, Talos Reawakened).

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

std::string FunctionBody(const std::string& source, const std::string& signature, const std::string& nextSignature) {
    const size_t begin = source.find(signature);
    if (begin == std::string::npos)
        return {};
    const size_t end = source.find(nextSignature, begin + signature.size());
    return source.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

}  // namespace

TEST(DX12RuntimeOwnedCaptureFlowTest, Phase3RuntimeOwnedDeferralSkipsOnlyOverlayInit) {
    const std::string phase3 = ReadSource("hook/d3d12/dx12_hook_process_session_stage3_overlay_init.cpp");
    ASSERT_FALSE(phase3.empty());
    const size_t skip = phase3.find("ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain(&initGpuSkipReason)");
    ASSERT_NE(skip, std::string::npos);
    const size_t flow = phase3.find("return ProcessFrameFlow::", skip);
    ASSERT_NE(flow, std::string::npos);
    EXPECT_EQ(phase3.find("return ProcessFrameFlow::kSkipOverlayInit;", skip), flow);
}

TEST(DX12RuntimeOwnedCaptureFlowTest, Phase4RuntimeOwnedSyncInitDeferralReachesCaptureDecision) {
    const std::string phase4 = ReadSource("hook/d3d12/dx12_hook_process_session_stage4_sync_focus_hold.cpp");
    ASSERT_FALSE(phase4.empty());
    const size_t deferral = phase4.find("if (stagedSyncInitOwnedByRuntime) {");
    const size_t staged = phase4.find("if (stagedSyncInitPending && !stagedSyncInitOwnedByRuntime) {");
    const size_t decision = phase4.find("captureBeforeOverlay =");
    ASSERT_NE(deferral, std::string::npos);
    ASSERT_NE(staged, std::string::npos);
    ASSERT_NE(decision, std::string::npos);
    EXPECT_LT(deferral, staged);
    EXPECT_LT(staged, decision);
    EXPECT_EQ(phase4.substr(deferral, staged - deferral).find("return ProcessFrameFlow::"), std::string::npos)
        << "the runtime-owned sync-init deferral must fall through to the capture decision";
}

TEST(DX12RuntimeOwnedCaptureFlowTest, OverlayFreeCaptureIsPublishedWhenTheDrawChainDidNotRun) {
    const std::string drawMain = ReadSource("hook/d3d12/dx12_hook_process_session_draw_main.cpp");
    const std::string drawTail = ReadSource("hook/d3d12/dx12_hook_process_session_draw_capture.cpp");
    ASSERT_FALSE(drawMain.empty());
    ASSERT_FALSE(drawTail.empty());

    const size_t drawGate = drawMain.find("if (captureBeforeOverlay) {");
    ASSERT_NE(drawGate, std::string::npos);
    const size_t mark = drawMain.find("captureBeforeOverlayPublished = true;", drawGate);
    const size_t publish = drawMain.find("PublishDX12CapturedFrame(", drawGate);
    ASSERT_NE(mark, std::string::npos);
    ASSERT_NE(publish, std::string::npos);
    EXPECT_LT(mark, publish);

    const std::string tail = FunctionBody(drawTail, "ProcessFrameFlow FrameProcessSession::PublishPostOverlayCapture()",
                                          "return ProcessFrameFlow::kContinue;");
    ASSERT_FALSE(tail.empty());
    EXPECT_NE(tail.find("captureBeforeOverlay && !captureBeforeOverlayPublished"), std::string::npos);
    EXPECT_NE(tail.find("if (captureAfterOverlay || overlayFreeCaptureMissedByDraw) {"), std::string::npos);
    EXPECT_NE(tail.find("PublishDX12CapturedFrame("), std::string::npos);
}
