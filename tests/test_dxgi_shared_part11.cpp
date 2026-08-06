#include "test_dxgi_shared_shared.h"

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
