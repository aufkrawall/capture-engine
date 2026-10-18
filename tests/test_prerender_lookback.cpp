#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "hook/pacing/prerender_lookback.h"
#include "hook/present/dxgi_shared_detail/types_and_state.h"
#include "source_fragment_reader.h"

// cpu_prerender_limit as a marker lookback (hook/pacing/prerender_lookback.h) and where the D3D11
// fence ring sits in the DXGI Present detours (hook/d3d11/dx11_hook_prerender.cpp).
namespace {
using ce::prerender_lookback::ClampLookback;
using ce::prerender_lookback::WaitTarget;

TEST(PrerenderLookbackTest, WaitsForThePresentLimitBack) {
    // Limit 1: the previous present's marker. Limit 3 with five out: the second present's.
    EXPECT_EQ(WaitTarget(1, 1), 1u);
    EXPECT_EQ(WaitTarget(5, 1), 5u);
    EXPECT_EQ(WaitTarget(5, 3), 3u);
    EXPECT_EQ(WaitTarget(6, 6), 1u);
}

TEST(PrerenderLookbackTest, NoWaitBeforeEnoughPresents) {
    EXPECT_EQ(WaitTarget(0, 1), 0u);
    EXPECT_EQ(WaitTarget(1, 2), 0u);
    EXPECT_EQ(WaitTarget(5, 6), 0u);
}

TEST(PrerenderLookbackTest, DepthIsClampedToTheIntegerContract) {
    EXPECT_EQ(ClampLookback(0), 1);
    EXPECT_EQ(ClampLookback(9), 6);
    EXPECT_EQ(WaitTarget(20, 9), WaitTarget(20, 6));
}

// The marker of a present is signaled after its forwarded down-call: the scope that signals it is
// declared before the detour code that applies the limit, in both DXGI Present detours, and the
// wait is charged to the limiter stage (it was startup_routing until 2026-10-18).
TEST(PrerenderLookbackSourceTest, DetoursSignalAfterTheForwardedPresent) {
    namespace fs = std::filesystem;
    const std::string present =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "present" / "dxgi_shared_present.cpp");
    const std::string present1 =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "present" / "dxgi_shared_present1.cpp");
    const std::string routing =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "present" / "dxgi_shared_present_routing.cpp");
    ASSERT_FALSE(present.empty());
    ASSERT_FALSE(present1.empty());
    ASSERT_FALSE(routing.empty());

    const size_t scope = present.find("D3D11PrerenderPresentScope prerenderScope;");
    const size_t routingCall = present.find("ExecuteStartupRouting(pSwapChain");
    ASSERT_NE(scope, std::string::npos);
    ASSERT_NE(routingCall, std::string::npos);
    EXPECT_LT(scope, routingCall);

    const size_t scope1 = present1.find("D3D11PrerenderPresentScope prerenderScope;");
    const size_t apply1 = present1.find("ApplyPrerenderLimit(pSwapChain");
    ASSERT_NE(scope1, std::string::npos);
    ASSERT_NE(apply1, std::string::npos);
    EXPECT_LT(scope1, apply1);

    for (const std::string* text : {&present1, &routing}) {
        const size_t apply = text->find("ApplyPrerenderLimit(pSwapChain");
        const size_t limiter = text->rfind("StageScope limiterStage(ce::present_stage_cost::Stage::kLimiter)", apply);
        ASSERT_NE(apply, std::string::npos);
        ASSERT_NE(limiter, std::string::npos);
        EXPECT_GT(text->find('}', limiter), apply) << "the limit is applied inside the limiter stage";
    }
}

TEST(PrerenderLookbackTest, BothD3D10And11SwapchainsGetTheLimit) {
    using DXGIShared::APIType;
    EXPECT_TRUE(DXGIShared::ShouldApplyD3D10Or11PrerenderLimit(APIType::D3D10));
    EXPECT_TRUE(DXGIShared::ShouldApplyD3D10Or11PrerenderLimit(APIType::D3D11));
    EXPECT_FALSE(DXGIShared::ShouldApplyD3D10Or11PrerenderLimit(APIType::D3D12));
    EXPECT_FALSE(DXGIShared::ShouldApplyD3D10Or11PrerenderLimit(APIType::Vulkan));
    EXPECT_FALSE(DXGIShared::ShouldApplyD3D10Or11PrerenderLimit(APIType::Unknown));
}

// Until 2026-10-18 both detours guarded the limit with `api == APIType::D3D11`, so D3D10 swapchains
// never reached it, and the entry point asked for ID3D11Device first, which a D3D10 swapchain on
// Windows 10+ also answers. The flow test FlowD3D10Prerender runs the entry point on WARP.
TEST(PrerenderLookbackSourceTest, D3D10SwapchainsReachTheD3D10Path) {
    namespace fs = std::filesystem;
    const std::string present1 =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "present" / "dxgi_shared_present1.cpp");
    const std::string routing =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "present" / "dxgi_shared_present_routing.cpp");
    const std::string prerender =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "d3d11" / "dx11_hook_prerender.cpp");
    ASSERT_FALSE(present1.empty());
    ASSERT_FALSE(routing.empty());
    ASSERT_FALSE(prerender.empty());
    EXPECT_NE(present1.find("ShouldApplyD3D10Or11PrerenderLimit(api)"), std::string::npos);
    EXPECT_NE(present1.find("ApplyPrerenderLimit(pSwapChain, prerenderLimit, api == APIType::D3D10)"),
              std::string::npos);
    EXPECT_NE(routing.find("ShouldApplyD3D10Or11PrerenderLimit(ctx.api)"), std::string::npos);
    EXPECT_NE(routing.find("ApplyPrerenderLimit(pSwapChain, prerenderLimit, ctx.api == APIType::D3D10)"),
              std::string::npos);

    const size_t entry = prerender.find("void ApplyPrerenderLimit(IDXGISwapChain* pSwapChain, float limit, bool");
    ASSERT_NE(entry, std::string::npos);
    const size_t try10 = prerender.find("d3d10Swapchain && TryBeforePresent10(", entry);
    const size_t try11 = prerender.find("TryBeforePresent11(", entry);
    ASSERT_NE(try10, std::string::npos);
    ASSERT_NE(try11, std::string::npos);
    EXPECT_LT(try10, try11) << "a classified D3D10 swapchain asks for its D3D10 device first";
}

// The fence path waits in the kernel; only the fallback ring for devices without fences polls.
TEST(PrerenderLookbackSourceTest, FencePathNeverSpins) {
    namespace fs = std::filesystem;
    const std::string source =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "d3d11" / "dx11_hook_prerender.cpp");
    ASSERT_FALSE(source.empty());
    const size_t fenceWait = source.find("bool WaitFenceLocked(");
    const size_t fenceWaitEnd = source.find("\n}\n", fenceWait);
    ASSERT_NE(fenceWait, std::string::npos);
    ASSERT_NE(fenceWaitEnd, std::string::npos);
    const std::string body = source.substr(fenceWait, fenceWaitEnd - fenceWait);
    EXPECT_NE(body.find("SetEventOnCompletion(target"), std::string::npos);
    EXPECT_NE(body.find("WaitForSingleObject(g_11.event, INFINITE)"), std::string::npos);
    EXPECT_EQ(body.find("SwitchToThread"), std::string::npos);
    EXPECT_EQ(body.find("GetData"), std::string::npos);
}

}  // namespace
