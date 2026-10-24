#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <mmsystem.h>

#include <cstdint>

#include "common/platform/raii_helpers.h"
#include "mediaengine/video/cursor_renderer.h"
#include "mediaengine/video/retained_yuv_overlays.h"
#include "mediaengine/video/video_format_policy.h"
#include "retained_yuv_layer_readback.h"

// The retained NV12/P010 route has the cursor renderer draw the cursor alone
// into an FP32 layer, then patches that layer into the YUV frame. A layer the
// renderer refuses leaves every retained frame without a cursor (hardware
// cursor missing from DXGI duplication recordings; software cursors survive
// because they are part of the duplicated image).

namespace {

ce::cursor::CaptureState VisibleArrowAt(int32_t x, int32_t y, int frameSize) {
    ce::cursor::CaptureState state;
    state.handle = reinterpret_cast<uint64_t>(LoadCursor(nullptr, IDC_ARROW));
    state.screenX = x;
    state.screenY = y;
    state.captureWidth = static_cast<uint32_t>(frameSize);
    state.captureHeight = static_cast<uint32_t>(frameSize);
    state.requestedWidth = 32;
    state.requestedHeight = 32;
    state.flags = ce::cursor::kStateValid | ce::cursor::kStateVisible;
    return state;
}

}  // namespace

TEST(CursorRendererRetainedLayerTest, DrawsTheCursorIntoTheRetainedYuvOverlayLayer) {
    ce::ComGuard<ID3D11Device> device;
    ce::ComGuard<ID3D11DeviceContext> context;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                            device.addressof(), nullptr, context.addressof())));
    constexpr int kFrame = 256;
    const ce::cursor::CaptureState state = VisibleArrowAt(100, 80, kFrame);
    ASSERT_TRUE(state.IsVisible());

    CursorRenderer renderer;
    ASSERT_TRUE(renderer.Init(device.get(), context.get()));
    RECT rect = {};
    ASSERT_TRUE(renderer.GetCursorFrameRect(kFrame, kFrame, state, &rect));
    ASSERT_GT(rect.right - rect.left, 0);
    ASSERT_GT(rect.bottom - rect.top, 0);

    RetainedYuvOverlayLayer layer;
    ASSERT_TRUE(layer.Prepare(device.get(), context.get(), rect));
    D3D11_TEXTURE2D_DESC desc = {};
    layer.texture->GetDesc(&desc);
    EXPECT_EQ(desc.Format, ce::video_format::kRetainedYuvOverlayLayerFormat);

    // The encoder's call: the layer's texel (0,0) is frame pixel rect.left/top.
    EXPECT_TRUE(renderer.CompositeOntoFrame(layer.texture, kFrame, kFrame, state, CursorColorMode::Sdr, 80.0f,
                                            rect.left, rect.top));
    const RetainedYuvLayerCoverage coverage = ReadRetainedYuvLayerCoverage(device.get(), context.get(), layer);
    ASSERT_TRUE(coverage.read);
    EXPECT_GT(coverage.maxAlpha, 0.99f);  // the arrow has opaque texels
    EXPECT_GT(coverage.coveredTexels, 0u);
    EXPECT_EQ(coverage.unpremultiplied, 0u);

    // The next slot reuses the layer cleared to transparent, also in scRGB.
    ASSERT_TRUE(layer.Prepare(device.get(), context.get(), rect));
    const RetainedYuvLayerCoverage cleared = ReadRetainedYuvLayerCoverage(device.get(), context.get(), layer);
    ASSERT_TRUE(cleared.read);
    EXPECT_EQ(cleared.coveredTexels, 0u);
    EXPECT_TRUE(renderer.CompositeOntoFrame(layer.texture, kFrame, kFrame, state, CursorColorMode::ScRgb, 203.0f,
                                            rect.left, rect.top));
    EXPECT_GT(ReadRetainedYuvLayerCoverage(device.get(), context.get(), layer).maxAlpha, 0.99f);
    layer.Release();
}
