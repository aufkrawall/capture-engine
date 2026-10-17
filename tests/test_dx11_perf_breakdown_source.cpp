#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

// The DX11 rows of the perf CSV (perf_metrics_*.csv). Until 2026-10-17 a game presenting through
// CE's swapchain wrapper (BioShock Infinite, 20261017_215604) got one row from the DX11 Present
// detour the real Present re-enters: total_us of the real Present alone, every CE column zero, and
// the row dedup dropped the wrapper's own row. No flow scenario presents a D3D11 swapchain, so the
// composition is pinned here; the columns themselves show in the next game run.
namespace {

std::string ReadSource(const char* relative) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relative);
}

std::string Between(const std::string& text, const char* from, const char* to) {
    const size_t begin = text.find(from);
    const size_t end = begin == std::string::npos ? std::string::npos : text.find(to, begin);
    return begin == std::string::npos || end == std::string::npos ? std::string() : text.substr(begin, end - begin);
}

}  // namespace

TEST(DX11PerfBreakdownSourceTest, TheWrappedPresentLogsOneRowWithCEsPhases) {
    const std::string detour = ReadSource("hook/d3d11/dx11_hook_present.cpp");
    const std::string inWrapper =
        Between(detour, "if (inWrapper) {", "return dx11_hook_oPresent(pSwapChain, SyncInterval, Flags);");
    ASSERT_FALSE(inWrapper.empty());
    EXPECT_NE(inWrapper.find("perfGuard.dismiss();"), std::string::npos) << "the wrapper's row is the present's";

    const std::string wrapper = ReadSource("hook/wrappers/dxgi_swapchain_wrap_present.cpp");
    const std::string dx11Branch =
        Between(wrapper, "DX11_ProcessFrameExternal(pRealCached);", "if (activeDebugSample)");
    ASSERT_FALSE(dx11Branch.empty());
    for (const char* field :
         {"TakeDX11FramePhases()", "perfMetrics.overlayUs = phases.overlayUs;",
          "perfMetrics.captureUs = phases.captureUs;", "perfMetrics.overlayGpuUs = phases.overlayGpuUs;"}) {
        EXPECT_NE(dx11Branch.find(field), std::string::npos) << field;
    }
}

TEST(DX11PerfBreakdownSourceTest, BothPresentHooksSplitTheDownCallFromCEsWork) {
    for (const char* path : {"hook/d3d11/dx11_hook_present.cpp", "hook/wrappers/dxgi_swapchain_wrap_present.cpp"}) {
        const std::string source = ReadSource(path);
        for (const char* piece :
             {"perfMetrics.prePresentUs = static_cast<int32_t>(presentCallStartUs - perfMetrics.qpcUs);",
              "perfMetrics.presentCallUs = static_cast<int32_t>(presentCallEndUs - presentCallStartUs);",
              "perfMetrics.postPresentUs = static_cast<int32_t>(endUs - presentCallEndUs);"}) {
            EXPECT_NE(source.find(piece), std::string::npos) << path << ": " << piece;
        }
    }
    // CE's phases are timed where they run, and only while the CSV is written.
    const std::string frame = ReadSource("hook/d3d11/dx11_hook_screenshot.cpp");
    EXPECT_NE(frame.find("const bool timePhases = PerfLogger::Get().IsEnabled();"), std::string::npos);
    EXPECT_NE(frame.find("t_framePhases.captureUs += elapsedUs(startUs);"), std::string::npos);
    EXPECT_NE(frame.find("t_framePhases.overlayUs = elapsedUs(startUs);"), std::string::npos);
    const std::string overlay = ReadSource("hook/d3d11/dx11_hook_overlay.cpp");
    const size_t gpuBegin = overlay.find("ce::dx11_overlay_gpu::BeginOverlay(device, context);");
    const size_t render =
        overlay.find("g_OverlayAdapter.RenderOverlay(desc.BufferDesc.Width, desc.BufferDesc.Height);");
    const size_t gpuEnd = overlay.find("ce::dx11_overlay_gpu::EndOverlay(context);");
    ASSERT_NE(gpuBegin, std::string::npos);
    ASSERT_NE(render, std::string::npos);
    ASSERT_NE(gpuEnd, std::string::npos);
    EXPECT_LT(gpuBegin, render);
    EXPECT_LT(render, gpuEnd);
}
