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
    EXPECT_NE(dx11Branch.find("perfRow.TakeFramePhases();"), std::string::npos);
}

TEST(DX11PerfBreakdownSourceTest, BothPresentHooksSplitTheDownCallFromCEsWork) {
    const std::string detour = ReadSource("hook/d3d11/dx11_hook_present.cpp");
    for (const char* piece :
         {"perfMetrics.prePresentUs = static_cast<int32_t>(presentCallStartUs - perfMetrics.qpcUs);",
          "perfMetrics.presentCallUs = static_cast<int32_t>(presentCallEndUs - presentCallStartUs);",
          "perfMetrics.postPresentUs = static_cast<int32_t>(endUs - presentCallEndUs);"}) {
        EXPECT_NE(detour.find(piece), std::string::npos) << piece;
    }
    // The wrapper's row is the shared DX11 present row (behavior: test_dx11_present_perf_row.cpp).
    const std::string wrapper = ReadSource("hook/wrappers/dxgi_swapchain_wrap_present.cpp");
    EXPECT_NE(wrapper.find("    perfRow.BeginPresentCall();\n"
                           "    HRESULT hr = pRealCached->Present(SyncInterval, presentFlags);\n"
                           "    perfRow.EndPresentCall();\n"),
              std::string::npos);
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

// Present1 (2026-10-17): until then neither CE's swapchain wrapper nor the DXGI Present1 detour wrote a
// row, so a game presenting with IDXGISwapChain1::Present1 had no DX11 rows at all. DetourDX11Present1 in
// dx11_hook_present.cpp is installed by no hook path; DXGIShared::DetourPresent1 is the detour that runs.
TEST(DX11PerfBreakdownSourceTest, TheWrappedPresent1LogsTheSameRowAsPresent) {
    constexpr size_t npos = std::string::npos;
    const std::string body = Between(ReadSource("hook/wrappers/dxgi_swapchain_wrap_present.cpp"),
                                     "HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present1(", "\n}\n");
    ASSERT_FALSE(body.empty());
    const size_t probeReturn = body.find("if ((PresentFlags & DXGI_PRESENT_TEST)");
    const size_t enabled = body.find("const bool perfLoggingEnabled = !m_IsD3D12 && PerfLogger::Get().IsEnabled();");
    const size_t row = body.find("DXGIShared::DX11PresentPerfRow perfRow(perfLoggingEnabled,");
    ASSERT_NE(probeReturn, npos);
    ASSERT_NE(enabled, npos);
    ASSERT_NE(row, npos);
    EXPECT_LT(probeReturn, row) << "a status probe presents nothing and writes no row";
    EXPECT_LT(enabled, row) << "a D3D12 frame's row is ProcessFrame's";
    EXPECT_NE(body.find("DX11_ProcessFrameExternal(pReal1Cached);\n"
                        "        if (DX11Hook_ShouldPassThroughCurrentPresent())\n"
                        "            return pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);\n"
                        "        perfRow.TakeFramePhases();\n"),
              npos);
    const size_t limiterStart = body.find("const int64_t limiterStartUs = perfRow.NowUs();");
    const size_t limiter =
        body.find("g_SharedFpsLimiter.Apply(true, ce::fps_limiter_policy::PresentSite::kUniqueApplicationPresent);");
    const size_t limiterEnd = body.find("perfRow.NoteFpsLimitWaitSince(limiterStartUs);");
    ASSERT_NE(limiterStart, npos);
    ASSERT_NE(limiter, npos);
    ASSERT_NE(limiterEnd, npos);
    EXPECT_LT(limiterStart, limiter);
    EXPECT_LT(limiter, limiterEnd);
    EXPECT_NE(body.find("    perfRow.BeginPresentCall();\n"
                        "    HRESULT hr = pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);\n"
                        "    perfRow.EndPresentCall();\n"),
              npos)
        << "present_call_us is the down-call alone";
}

TEST(DX11PerfBreakdownSourceTest, TheDXGIPresent1DetourLogsARowOnlyForAPresentItOwns) {
    constexpr size_t npos = std::string::npos;
    const std::string body = Between(ReadSource("hook/present/dxgi_shared_present1.cpp"),
                                     "HRESULT STDMETHODCALLTYPE DetourPresent1(", "\n}\n");
    ASSERT_FALSE(body.empty());
    const size_t row = body.find(
        "DX11PresentPerfRow perfRow(ShouldRunSharedD3D10Or11ProcessFrame(api) && PerfLogger::Get().IsEnabled(),");
    ASSERT_NE(row, npos);
    // The wrapper's Present1 re-enters here through the real Present1, and a nested Present1 belongs to the
    // outer one: those presents' rows are not this detour's.
    for (const char* earlyReturn : {"QueryInterface(IID_CWrapDXGISwapChain, &pWrapper)", "if (IsInWrapperPresent()) {",
                                    "if (IsRecursivePresent()) {"}) {
        const size_t at = body.find(earlyReturn);
        ASSERT_NE(at, npos) << earlyReturn;
        EXPECT_LT(at, row) << earlyReturn;
    }
    EXPECT_NE(body.find("HandleDX11ProcessFrame(pSwapChain, true);\n        perfRow.TakeFramePhases();\n"), npos);
    const size_t limiterStart = body.find("const int64_t limiterStartUs = perfRow.NowUs();");
    const size_t limiter = body.find(
        "g_SharedFpsLimiter.Apply(true, ce::fps_limiter_policy::ResolveDxgiPresentSite(callbackProvenRuntimeOutput));");
    const size_t limiterEnd = body.find("perfRow.NoteFpsLimitWaitSince(limiterStartUs);");
    ASSERT_NE(limiterStart, npos);
    ASSERT_NE(limiter, npos);
    ASSERT_NE(limiterEnd, npos);
    EXPECT_LT(row, limiterStart);
    EXPECT_LT(limiterStart, limiter);
    EXPECT_LT(limiter, limiterEnd);
    // The frame-latency wait before Streamline's present stays CE's pre-present work.
    EXPECT_NE(body.find("WaitBackbufferFrameLatency(pSwapChain);\n"
                        "            perfRow.BeginPresentCall();\n"
                        "            hr = dxgi_shared_oPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);"),
              npos);
    const size_t begin = body.find("    HRESULT hr;\n    perfRow.BeginPresentCall();\n");
    const size_t end = body.find("    }\n    perfRow.EndPresentCall();\n");
    const size_t flush = body.find("FlushDX12DeferredOverlaySignalAfterHookedPresent(");
    ASSERT_NE(begin, npos);
    ASSERT_NE(end, npos);
    ASSERT_NE(flush, npos);
    EXPECT_LT(limiterEnd, begin);
    EXPECT_LT(begin, end);
    EXPECT_LT(end, flush) << "post_present_us starts at the down-call's return";
}

// DetourPresent on the real swapchain (2026-10-17): its catch-all row had total_us alone. A D3D11/D3D10
// present now gets the shared row; every other API keeps the catch-all.
TEST(DX11PerfBreakdownSourceTest, TheDXGIPresentDetourLogsTheSharedRowForDX11) {
    constexpr size_t npos = std::string::npos;
    const std::string body = Between(ReadSource("hook/present/dxgi_shared_present_core.cpp"),
                                     "HRESULT ExecutePresentCore(", "    return hr;\n}");
    ASSERT_FALSE(body.empty());
    const size_t row = body.find("dx11Row.emplace(dx11Present && PerfLogger::Get().IsEnabled(), "
                                 "ctx.api == APIType::D3D10 ? \"DX10\" : \"DX11\",");
    ASSERT_NE(row, npos);
    EXPECT_NE(body.find("const bool dx11Present = ShouldRunSharedD3D10Or11ProcessFrame(ctx.api);"), npos);
    // One row per present: the catch-all's dedup scope and row are only the other APIs'.
    EXPECT_NE(body.find("    if (!dx11Present) {\n        PerfLogger::BeginPresentRowScope();\n    }\n"), npos);
    EXPECT_NE(body.find("if (!dx11Present && PerfLogger::Get().IsEnabled() && "
                        "!PerfLogger::InnerRowLoggedInPresentRowScope()) {"),
              npos);
    // perfGuard ends the row in the post-present cost stage, on every exit path.
    const size_t guard = body.find("auto perfGuard = ce::make_scope_guard([&]() {\n"
                                   "        CostScope postPresentStage(CostStage::kPostPresent);\n"
                                   "        dx11Row.reset();\n");
    ASSERT_NE(guard, npos);
    EXPECT_LT(row, guard);
    EXPECT_NE(body.find("HandleDX11ProcessFrame(pSwapChain, true);\n            perfRow.TakeFramePhases();\n"), npos);
    const size_t limiterStart = body.find("const int64_t limiterStartUs = perfRow.NowUs();");
    const size_t limiter = body.find("g_SharedFpsLimiter.Apply(\n                true, "
                                     "ce::fps_limiter_policy::ResolveDxgiPresentSite(");
    const size_t limiterEnd = body.find("perfRow.NoteFpsLimitWaitSince(limiterStartUs);");
    ASSERT_NE(limiterStart, npos);
    ASSERT_NE(limiter, npos);
    ASSERT_NE(limiterEnd, npos);
    EXPECT_LT(guard, limiterStart);
    EXPECT_LT(limiterStart, limiter);
    EXPECT_LT(limiter, limiterEnd);
    // present_call_us is the down-call alone at every forward site the row reaches.
    for (const char* site :
         {"        perfRow.BeginPresentCall();\n"
          "        if (TryInvokeGuardedExternalSteamOverlayPresent(pSwapChain, SyncInterval, Flags,\n",
          "&guardedSteamHr)) {\n            perfRow.EndPresentCall();\n",
          "            perfRow.BeginPresentCall();\n"
          "            HRESULT bypassHr = ForwardPresentThrough(bypass, pSwapChain, SyncInterval, Flags);\n"
          "            perfRow.EndPresentCall();\n",
          // The frame-latency wait before Streamline's present stays CE's pre-present work.
          "                WaitBackbufferFrameLatency(pSwapChain);\n            }\n"
          "            perfRow.BeginPresentCall();\n"
          "            hr = ForwardPresentThrough(dxgi_shared_oPresent, pSwapChain, SyncInterval, Flags);\n"}) {
        EXPECT_NE(body.find(site), npos) << site;
    }
    const size_t begin =
        body.find("    HRESULT hr;\n    perfRow.BeginPresentCall();\n    if (startupTransportBypass) {");
    const size_t end = body.find("        hr = CallOriginalPresent(pSwapChain, SyncInterval, Flags);\n    }\n"
                                 "    perfRow.EndPresentCall();\n");
    const size_t flush = body.find("FlushDX12DeferredOverlaySignalAfterHookedPresent(ctx.api == APIType::D3D12");
    ASSERT_NE(begin, npos);
    ASSERT_NE(end, npos);
    ASSERT_NE(flush, npos);
    EXPECT_LT(limiterEnd, begin);
    EXPECT_LT(begin, end);
    EXPECT_LT(end, flush) << "post_present_us starts at the down-call's return";
}
