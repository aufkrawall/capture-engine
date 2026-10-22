#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "hook/d3d11/dx11_back_buffer_view_cache.h"
#include "source_fragment_reader.h"

// The D3D10/D3D11 overlay retains one render-target view per back buffer. With one retained view, a
// FLIP chain whose current index alternates 0/1 (DXUP's D3D11) created a view, plus two log lines,
// on every Present: 16438 times in Mirror's Edge's 37 s menu (session 20261018_143747). Since
// 2026-10-22 every D3D10/D3D11 path uses the application's buffer 0 (kDX11ApplicationBackBuffer;
// FlowD3D11FlipBufferIndex.OverlayDrawsOnBufferZeroOfATranslatorsRotatingChain).
namespace {

struct FakeView {
    int id = 0;
};

struct Releases {
    std::vector<int> ids;
    auto Fn() {
        return [this](FakeView* view) { ids.push_back(view->id); };
    }
};

TEST(BackBufferViewCacheTest, AlternatingFlipBuffersCreateEachViewOnce) {
    BackBufferViewCache<FakeView> cache;
    Releases releases;
    FakeView views[2] = {{0}, {1}};
    const void* chain = &views;
    int created = 0;
    for (int present = 0; present < 1000; ++present) {
        const UINT index = static_cast<UINT>(present & 1);
        if (!cache.Find(chain, index)) {
            ASSERT_TRUE(cache.Store(chain, index, &views[index], releases.Fn()));
            ++created;
        }
        EXPECT_EQ(cache.Find(chain, index), &views[index]);
    }
    EXPECT_EQ(created, 2);
    EXPECT_TRUE(releases.ids.empty());
    EXPECT_EQ(cache.Count(), 2u);
}

TEST(BackBufferViewCacheTest, AnotherSwapchainReleasesTheRetainedViews) {
    BackBufferViewCache<FakeView> cache;
    Releases releases;
    FakeView a0{10}, a1{11}, b0{20};
    int chainA = 0, chainB = 0;
    ASSERT_TRUE(cache.Store(&chainA, 0, &a0, releases.Fn()));
    ASSERT_TRUE(cache.Store(&chainA, 1, &a1, releases.Fn()));
    EXPECT_EQ(cache.Find(&chainB, 0), nullptr);
    ASSERT_TRUE(cache.Store(&chainB, 0, &b0, releases.Fn()));
    EXPECT_EQ(releases.ids, (std::vector<int>{10, 11}));
    EXPECT_EQ(cache.Find(&chainA, 0), nullptr);
    EXPECT_EQ(cache.Find(&chainB, 0), &b0);
    EXPECT_EQ(cache.SwapChain(), &chainB);
    EXPECT_EQ(cache.Count(), 1u);
}

TEST(BackBufferViewCacheTest, ReplacingAndClearingReleaseExactlyOnce) {
    BackBufferViewCache<FakeView> cache;
    Releases releases;
    FakeView first{1}, second{2}, other{3};
    int chain = 0;
    ASSERT_TRUE(cache.Store(&chain, 0, &first, releases.Fn()));
    ASSERT_TRUE(cache.Store(&chain, 0, &second, releases.Fn()));
    ASSERT_TRUE(cache.Store(&chain, 2, &other, releases.Fn()));
    EXPECT_EQ(releases.ids, (std::vector<int>{1}));
    cache.Clear(releases.Fn());
    EXPECT_EQ(releases.ids, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(cache.Count(), 0u);
    EXPECT_EQ(cache.SwapChain(), nullptr);
    EXPECT_EQ(cache.Find(&chain, 0), nullptr);
    cache.Clear(releases.Fn());
    EXPECT_EQ(releases.ids.size(), 3u);
}

TEST(BackBufferViewCacheTest, AnIndexPastTheCacheStaysWithTheCaller) {
    BackBufferViewCache<FakeView> cache;
    Releases releases;
    FakeView view{7};
    int chain = 0;
    EXPECT_FALSE(cache.Store(&chain, BackBufferViewCache<FakeView>::kMaxBuffers, &view, releases.Fn()));
    EXPECT_FALSE(cache.Store(nullptr, 0, &view, releases.Fn()));
    EXPECT_EQ(cache.Find(&chain, BackBufferViewCache<FakeView>::kMaxBuffers), nullptr);
    EXPECT_EQ(cache.Count(), 0u);
    EXPECT_TRUE(releases.ids.empty());
}

TEST(BackBufferViewCacheSourceTest, OverlayPathsLookUpTheViewOfThisBuffer) {
    namespace fs = std::filesystem;
    const fs::path root = fs::current_path() / "hook" / "d3d11";
    const std::string overlay = ce::test_source::ReadFile(root / "dx11_hook_overlay.cpp");
    ASSERT_FALSE(overlay.empty());
    EXPECT_NE(overlay.find("dx11_hook_g_overlayViews.Find(pSwapChain, kDX11ApplicationBackBuffer)"),
              std::string::npos);
    EXPECT_NE(overlay.find("dx11_hook_g_overlayViews10.Find(pSwapChain, bufferIndex)"), std::string::npos);
    // No D3D10/D3D11 path draws on or reads the index a translator's chain reports (DXUP refuses
    // GetBuffer of it; 3388 of 6788 Presents lost the overlay in 20261022_162106).
    for (const char* file : {"dx11_hook_overlay.cpp", "dx11_hook_sharpen.cpp", "dx11_hook_capture_frame.cpp",
                             "dx11_hook_screenshot.cpp"}) {
        SCOPED_TRACE(file);
        const std::string source = ce::test_source::ReadFile(root / file);
        EXPECT_NE(source.find("kDX11ApplicationBackBuffer"), std::string::npos);
        EXPECT_EQ(source.find("GetCurrentBackBufferIndex"), std::string::npos);
    }
    // The single-view rebuild on every index change is gone.
    EXPECT_EQ(overlay.find("resolvedBufferIndex != lastBufferIndex"), std::string::npos);
    EXPECT_EQ(overlay.find("s_LastDX10OverlayBufferIndex != bufferIndex"), std::string::npos);
    // The pre-render line must not change with the alternating view.
    EXPECT_NE(overlay.find("FieldKey(device, context, pSwapChain)"), std::string::npos);
    // The "current view" pointers own nothing: no path releases them directly.
    for (const char* file : {"dx11_hook.cpp", "dx11_hook_device.cpp", "dx11_hook_present.cpp", "dx11_hook_overlay.cpp"}) {
        SCOPED_TRACE(file);
        const std::string source = ce::test_source::ReadFile(root / file);
        ASSERT_FALSE(source.empty());
        EXPECT_EQ(source.find("dx11_hook_g_mainRenderTargetView->Release()"), std::string::npos);
        EXPECT_EQ(source.find("dx11_hook_g_mainRenderTargetView10->Release()"), std::string::npos);
        EXPECT_EQ(source.find("Queue(dx11_hook_g_mainRenderTargetView"), std::string::npos);
    }
}

}  // namespace
