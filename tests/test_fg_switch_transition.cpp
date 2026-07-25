#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../testapp/fg_switch_transition.h"

using testapp::fg::CanCommitFsrPresentationBreak;
using testapp::fg::FsrExitTransitionAction;
using testapp::fg::FsrExitTransitionStage;
using testapp::fg::IsDlssReplacementSurfaceStage;
using testapp::fg::ResolveFsrExitTransitionAction;
using testapp::fg::ShouldDeferDlssActivationUntilReplacementPresent;
using testapp::fg::ShouldPrepareDlssBeforeFsrPresentationBreak;

TEST(FsrExitTransitionTest, ActiveFsrStagesPassthroughBeforeEveryExitDirection) {
    EXPECT_EQ(ResolveFsrExitTransitionAction(true, false, true, FsrExitTransitionStage::None),
              FsrExitTransitionAction::DisableAndPresentPassthrough);
}

TEST(FsrExitTransitionTest, PendingPassthroughMustPresentBeforeTeardownCanContinue) {
    EXPECT_EQ(ResolveFsrExitTransitionAction(true, false, false, FsrExitTransitionStage::PresentPending),
              FsrExitTransitionAction::PresentPassthrough);
    EXPECT_EQ(ResolveFsrExitTransitionAction(true, false, false, FsrExitTransitionStage::PassthroughPresented),
              FsrExitTransitionAction::ContinueSwitch);
}

TEST(FsrExitTransitionTest, NonFsrAndCancelledExitsDoNotStagePassthrough) {
    EXPECT_EQ(ResolveFsrExitTransitionAction(false, false, true, FsrExitTransitionStage::None),
              FsrExitTransitionAction::ContinueSwitch);
    EXPECT_EQ(ResolveFsrExitTransitionAction(false, true, true, FsrExitTransitionStage::None),
              FsrExitTransitionAction::ContinueSwitch);
    EXPECT_EQ(ResolveFsrExitTransitionAction(true, true, true, FsrExitTransitionStage::PresentPending),
              FsrExitTransitionAction::ContinueSwitch);
}

TEST(FsrExitTransitionTest, DlssRuntimeMustBePreparedBeforeBreakingPresentedFsrSurface) {
    EXPECT_TRUE(
        ShouldPrepareDlssBeforeFsrPresentationBreak(true, true, false, FsrExitTransitionStage::PassthroughPresented));
    EXPECT_FALSE(CanCommitFsrPresentationBreak(true, true, false, FsrExitTransitionStage::PassthroughPresented));
    EXPECT_TRUE(CanCommitFsrPresentationBreak(true, true, true, FsrExitTransitionStage::PassthroughPresented));
}

TEST(FsrExitTransitionTest, OffTargetCanCommitWithoutPreparingDlss) {
    EXPECT_FALSE(
        ShouldPrepareDlssBeforeFsrPresentationBreak(true, false, false, FsrExitTransitionStage::PassthroughPresented));
    EXPECT_TRUE(CanCommitFsrPresentationBreak(true, false, false, FsrExitTransitionStage::PassthroughPresented));
    EXPECT_FALSE(CanCommitFsrPresentationBreak(false, true, true, FsrExitTransitionStage::PassthroughPresented));
}

TEST(FsrExitTransitionTest, DlssActivationWaitsForSuccessfulReplacementPresent) {
    EXPECT_TRUE(IsDlssReplacementSurfaceStage(FsrExitTransitionStage::ReplacementPresentPending));
    EXPECT_TRUE(IsDlssReplacementSurfaceStage(FsrExitTransitionStage::ReplacementPresented));
    EXPECT_FALSE(IsDlssReplacementSurfaceStage(FsrExitTransitionStage::PassthroughPresented));
    EXPECT_TRUE(
        ShouldDeferDlssActivationUntilReplacementPresent(true, FsrExitTransitionStage::ReplacementPresentPending));
    EXPECT_FALSE(ShouldDeferDlssActivationUntilReplacementPresent(true, FsrExitTransitionStage::ReplacementPresented));
    EXPECT_FALSE(
        ShouldDeferDlssActivationUntilReplacementPresent(false, FsrExitTransitionStage::ReplacementPresentPending));
}

TEST(FsrExitTransitionSourceTest, ReplacementIsPreparedBeforeFsrPresentationBreak) {
    // This asserts an ordering within the dx12_fg_switch_test translation unit:
    // SwitchMode prepares DLSS before the FSR presentation break, and Render()
    // only presents afterwards. Both halves moved into .inl files that
    // dx12_fg_switch_test.cpp includes in this order, so read them in that
    // order to reconstruct the span the assertions are about.
    const std::filesystem::path testappDir = std::filesystem::current_path() / "testapp";
    std::string text;
    for (const char* part : {"dx12_fg_switch_runtime.inl", "dx12_fg_switch_render.inl"}) {
        const std::filesystem::path source = testappDir / part;
        ASSERT_TRUE(std::filesystem::exists(source)) << part;
        std::ifstream stream(source, std::ios::binary);
        ASSERT_TRUE(stream.good()) << part;
        text.append((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    }

    const size_t switchMode = text.find("static bool SwitchMode(");
    const size_t disable = text.find("ConfigureFSR(false, nullptr, \"leave FSR mode\"", switchMode);
    const size_t pending = text.find("FsrExitTransitionStage::PresentPending", disable);
    const size_t deferredReturn = text.find("return true;", pending);
    const size_t preparation = text.find("prepare DLSS before FSR presentation break", deferredReturn);
    const size_t completedWait = text.find("leave FSR mode after passthrough Present", preparation);
    const size_t dlssBranch = text.find("} else if (target == FGMode::DLSS)", completedWait);
    const size_t destroy = text.find("DestroyFSRContexts();", dlssBranch);
    const size_t recreate = text.find("enter DLSS mode after prepared FSR exit", destroy);
    const size_t replacementPending = text.find("FsrExitTransitionStage::ReplacementPresentPending", recreate);
    const size_t replacementDeferredReturn = text.find("return true;", replacementPending);
    const size_t enableDlss = text.find("SetDLSSFGMode(true)", replacementDeferredReturn);
    const size_t dlssBranchEnd = text.find("} else {", recreate);
    ASSERT_NE(disable, std::string::npos);
    ASSERT_NE(pending, std::string::npos);
    ASSERT_NE(deferredReturn, std::string::npos);
    ASSERT_NE(preparation, std::string::npos);
    ASSERT_NE(completedWait, std::string::npos);
    ASSERT_NE(destroy, std::string::npos);
    ASSERT_NE(recreate, std::string::npos);
    ASSERT_NE(replacementPending, std::string::npos);
    ASSERT_NE(replacementDeferredReturn, std::string::npos);
    ASSERT_NE(enableDlss, std::string::npos);
    ASSERT_NE(dlssBranchEnd, std::string::npos);
    EXPECT_LT(disable, pending);
    EXPECT_LT(pending, deferredReturn);
    EXPECT_LT(deferredReturn, preparation);
    EXPECT_LT(deferredReturn, completedWait);
    EXPECT_LT(completedWait, destroy);
    EXPECT_LT(destroy, recreate);
    EXPECT_LT(recreate, replacementPending);
    EXPECT_LT(replacementPending, replacementDeferredReturn);
    EXPECT_LT(replacementDeferredReturn, enableDlss);
    const size_t fullRendererRelease = text.find("ReleaseDX12RendererResourcesForSwitch", dlssBranch);
    EXPECT_TRUE(fullRendererRelease == std::string::npos || fullRendererRelease > dlssBranchEnd)
        << "FSR->DLSS must preserve the current device/queue and replace only swapchain resources";

    const size_t rollback = text.find("rolling back to active FSR without destroying its proxy", preparation);
    const size_t rollbackConfigure = text.find("rollback failed DLSS preparation\", true", rollback);
    ASSERT_NE(rollback, std::string::npos);
    ASSERT_NE(rollbackConfigure, std::string::npos);
    EXPECT_LT(preparation, rollback);
    EXPECT_LT(rollback, rollbackConfigure);
    EXPECT_LT(rollbackConfigure, destroy);

    const size_t transitionPresent = text.find("const bool fsrExitPassthroughPresent", destroy);
    const size_t present = text.find("g_SwapChain->Present(", transitionPresent);
    const size_t presented = text.find("FsrExitTransitionStage::PassthroughPresented", present);
    ASSERT_NE(transitionPresent, std::string::npos);
    ASSERT_NE(present, std::string::npos);
    ASSERT_NE(presented, std::string::npos);
    EXPECT_LT(transitionPresent, present);
    EXPECT_LT(present, presented);

    const size_t replacementTransitionPresent =
        text.find("const bool dlssReplacementPassthroughPresent", transitionPresent);
    const size_t replacementPresent = text.find("g_SwapChain->Present(", replacementTransitionPresent);
    const size_t replacementPresented = text.find("FsrExitTransitionStage::ReplacementPresented", replacementPresent);
    const size_t fsrRetirement =
        text.find("MaybeUnloadFSRRuntimeAfterSwitch(\"after first active DLSS Present\"", replacementPresented);
    ASSERT_NE(replacementTransitionPresent, std::string::npos);
    ASSERT_NE(replacementPresent, std::string::npos);
    ASSERT_NE(replacementPresented, std::string::npos);
    ASSERT_NE(fsrRetirement, std::string::npos);
    EXPECT_LT(replacementTransitionPresent, replacementPresent);
    EXPECT_LT(replacementPresent, replacementPresented);
    EXPECT_LT(enableDlss, fsrRetirement);
    EXPECT_LT(replacementPresented, fsrRetirement);
}
