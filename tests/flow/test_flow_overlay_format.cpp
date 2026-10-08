// The inject overlay follows the format of the back buffer it writes. A game can move its swapchain between
// formats at runtime (Witcher 3 flips R8G8B8A8 -> R10G10B10A2 + HDR10 -> R8G8B8A8 while starting, with a foreign
// overlay owning the ResizeBuffers entry so CE never sees the resize); the overlay's pipelines must follow, or it
// is drawn through pipelines of another format than the render target - garbled colors for the frames in between.

#include "tests/flow/flow_test_support.h"

namespace {

using ce::flow::CurrentTestName;

// Opaque red written by the overlay backend's solid pipeline in SDR mode, as each format stores its first texel.
constexpr uint32_t kRedRgba8 = 0xFF0000FF;          // R in the low byte
constexpr uint32_t kRedBgra8 = 0xFFFF0000;          // R in the third byte
constexpr uint32_t kRedRgb10A2 = 0xC00003FF;        // R bits 0-9, alpha bits 30-31
constexpr uint32_t kRedRgba16FFirstHalves = 0x3C00;  // R = 1.0 (half), G = 0

TEST(FlowOverlayFormat, BackendDrawsCorrectlyIntoEveryRenderTargetFormatItFollows) {
    ce::flow::FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();

    // Built for R8G8B8A8, then flipped away and back the way a game flips its swapchain; B8G8R8A8 and
    // R16G16B16A16_FLOAT are the other formats a game presents in.
    const auto probe = game.ProbeOverlayBackendFormats({DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
                                                        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                        DXGI_FORMAT_R16G16B16A16_FLOAT});
    ASSERT_TRUE(probe.ok) << "logs: " << game.LogDirectory();
    ASSERT_EQ(probe.firstPixels.size(), 5u);
    EXPECT_EQ(probe.firstPixels[0], kRedRgba8) << "R8G8B8A8";
    EXPECT_EQ(probe.firstPixels[1], kRedRgb10A2) << "R10G10B10A2: drawn through the R8G8B8A8 pipelines it is garbled";
    EXPECT_EQ(probe.firstPixels[2], kRedRgba8) << "R8G8B8A8 again, after the flip";
    EXPECT_EQ(probe.firstPixels[3], kRedBgra8) << "B8G8R8A8";
    EXPECT_EQ(probe.firstPixels[4], kRedRgba16FFirstHalves) << "R16G16B16A16_FLOAT";
    // One pipeline pair per format, created once: the return to R8G8B8A8 reuses the first pair.
    EXPECT_EQ(probe.pipelineFormats, 4u);
    ce::flow::ExpectNoDebugLayerErrors();
}

TEST(FlowOverlayFormat, OverlayCoversEveryPresentAcrossSdrHdr10SdrBackBufferFlips) {
    ce::flow::FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrames(30)) << game.Error();

    ASSERT_TRUE(game.SetBackBufferFormat(DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020))
        << game.Error();
    ASSERT_TRUE(game.RenderFrames(30)) << game.Error();

    ASSERT_TRUE(game.SetBackBufferFormat(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709))
        << game.Error();
    ASSERT_TRUE(game.RenderFrames(30)) << game.Error();

    EXPECT_EQ(game.PhysicalPresents(), 90u);
    // The harness runs display gamma on every frame: the HDR10 frames are a deliberate passthrough, so the
    // post-process ledger must not count them as failures (ExpectPostProcessAccounted: failed == 0).
    ce::flow::ExpectEveryPresentCoveredOnce(game);
    ce::flow::ExpectNoDebugLayerErrors();
}

}  // namespace
