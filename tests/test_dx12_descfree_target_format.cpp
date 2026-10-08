#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "hook/d3d12/dx12_overlay_policy.h"
#include "source_fragment_reader.h"

namespace {

namespace policy = ce::dx12_overlay_policy;

std::string ReadHookSource(const char* relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

TEST(DX12DescFreeTargetFormatTest, OverlayDrawsInTheFormatOfTheBackBufferItWritesNotTheTrackedOne) {
    const int rgba8 = static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM);
    const int rgb10a2 = static_cast<int>(DXGI_FORMAT_R10G10B10A2_UNORM);
    // Witcher 3 flips R8G8B8A8 -> R10G10B10A2 -> R8G8B8A8 while starting; CE tracked R8G8B8A8 at init.
    EXPECT_EQ(policy::ResolveOverlayTargetFormat(rgba8, rgb10a2), rgb10a2);
    EXPECT_EQ(policy::ResolveOverlayTargetFormat(rgb10a2, rgba8), rgba8);
    EXPECT_EQ(policy::ResolveOverlayTargetFormat(rgba8, rgba8), rgba8);
    // An unreadable back buffer keeps the tracked format instead of retargeting to UNKNOWN.
    EXPECT_EQ(policy::ResolveOverlayTargetFormat(rgba8, static_cast<int>(DXGI_FORMAT_UNKNOWN)), rgba8);
}

TEST(DX12DescFreeTargetFormatTest, FormatChangeRetargetsTheWarmBackendAndOnlyADeviceChangeRebuildsIt) {
    EXPECT_EQ(policy::DecideDescFreeBackendAction(true, true), policy::DescFreeBackendAction::kReuse);
    EXPECT_EQ(policy::DecideDescFreeBackendAction(true, false), policy::DescFreeBackendAction::kRetarget);
    EXPECT_EQ(policy::DecideDescFreeBackendAction(false, true), policy::DescFreeBackendAction::kRebuild);
    EXPECT_EQ(policy::DecideDescFreeBackendAction(false, false), policy::DescFreeBackendAction::kRebuild);
}

TEST(DX12DescFreeTargetFormatTest, NormalRouteFollowsTheLiveBackBufferFormatBeforeItSelectsThePipelines) {
    const std::string route = ReadHookSource("hook/d3d12/dx12_hook_process_session_draw_record.cpp");
    ASSERT_FALSE(route.empty());
    const size_t sync = route.find("ResolveOverlayTargetFormat(");
    const size_t assign = route.find("dx12_hook_g_State.format = liveFormat;");
    const size_t ensure = route.find("EnsureDescFreeBackendForDeviceAndFormat(dev, dx12_hook_g_State.format");
    ASSERT_NE(sync, std::string::npos);
    ASSERT_NE(assign, std::string::npos);
    ASSERT_NE(ensure, std::string::npos);
    EXPECT_LT(sync, assign);
    // The format is synchronized before the backend is ensured, the offscreen target is sized or the secondary
    // overlay's color contract is derived from it.
    EXPECT_LT(assign, ensure);
    EXPECT_LT(assign, route.find("EnsureOffscreenRT(dev, dx12_hook_g_State.cachedWidth"));
    EXPECT_NE(route.find("bb->GetDesc().Format"), std::string::npos);
    EXPECT_NE(route.find("Back buffer format changed without a resize CE saw"), std::string::npos);
}

TEST(DX12DescFreeTargetFormatTest, EnsureRetargetsOnFormatChangeAndFallsBackToARebuild) {
    const std::string ensure = ReadHookSource("hook/d3d12/dx12_hook_overlay_breadcrumbs.cpp");
    ASSERT_FALSE(ensure.empty());
    const size_t decide = ensure.find("DecideDescFreeBackendAction(");
    const size_t retarget = ensure.find("dx12_hook_g_DescFreeBackend->SetTargetFormat(format)");
    const size_t shutdown = ensure.find("ShutdownDescFreeBackend(context);", retarget);
    ASSERT_NE(decide, std::string::npos);
    ASSERT_NE(retarget, std::string::npos);
    ASSERT_NE(shutdown, std::string::npos);
    EXPECT_LT(decide, retarget);
    // A retarget that cannot create the pipelines degrades to the old rebuild instead of drawing with a stale pair.
    EXPECT_LT(retarget, shutdown);
    EXPECT_NE(ensure.find("rebuild = true;"), std::string::npos);
    EXPECT_NE(ensure.find("DescFree backend retargeted fmt"), std::string::npos);
}

TEST(DX12DescFreeTargetFormatTest, BackendKeepsOnePipelinePairPerFormatAliveUntilShutdown) {
    const std::string header = ReadHookSource("hook/d3d12/dx12_hook_types.h");
    const std::string impl = ReadHookSource("hook/d3d12/dx12_hook_types_impl.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(impl.empty());
    EXPECT_NE(header.find("bool SetTargetFormat(DXGI_FORMAT rtvFormat);"), std::string::npos);
    EXPECT_EQ(header.find("psoTextured_"), std::string::npos);
    EXPECT_EQ(header.find("rtvFormat_"), std::string::npos);

    // Render selects the pair of the active format, never a fixed one.
    const size_t render = impl.find("void DX12DescFreeBackend::Render(");
    ASSERT_NE(render, std::string::npos);
    const size_t select = impl.find("pipelines_[activePipelines_]", render);
    const size_t draw = impl.find("cmd.useTexture ? targetPipelines.textured : targetPipelines.solid", render);
    ASSERT_NE(select, std::string::npos);
    ASSERT_NE(draw, std::string::npos);
    EXPECT_LT(select, draw);
    EXPECT_NE(impl.find("activePipelines_ < 0)", render), std::string::npos);

    // Earlier formats' pipelines are never released while a format switch is in flight: only Shutdown does.
    const size_t setFormat = impl.find("bool DX12DescFreeBackend::SetTargetFormat(");
    const size_t initialize = impl.find("bool DX12DescFreeBackend::Initialize(");
    ASSERT_NE(setFormat, std::string::npos);
    ASSERT_NE(initialize, std::string::npos);
    const std::string setFormatBody = impl.substr(setFormat, initialize - setFormat);
    EXPECT_EQ(setFormatBody.find("->Release()"), std::string::npos);
    EXPECT_NE(setFormatBody.find("pipelineCount_ >= kMaxPipelineFormats"), std::string::npos);
}

}  // namespace
