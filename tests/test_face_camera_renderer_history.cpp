#include <gtest/gtest.h>

#include <d3d11.h>
#include <wrl/client.h>

#include "mediaengine/video/face_camera_renderer.h"
#include "mediaengine/video/retained_yuv_overlays.h"
#include "mediaengine/video/video_format_policy.h"
#include "retained_yuv_layer_readback.h"

struct FaceCameraRendererTestAccess {
    static bool SeedHistory(FaceCameraRenderer& renderer, int64_t newest, int64_t interval) {
        renderer.history_.resize(ce::face_camera::kHistoryInitialFrames);
        for (size_t i = 0; i < renderer.history_.size(); ++i) {
            auto& slot = renderer.history_[i];
            if (!renderer.EnsureSlotTexture(slot, 16, 16, DXGI_FORMAT_B8G8R8A8_UNORM))
                return false;
            slot.captureQpc = newest - static_cast<int64_t>(renderer.history_.size() - 1 - i) * interval;
        }
        renderer.lastCaptureQpc_ = newest;
        renderer.cameraIntervalQpc_ = interval;
        renderer.qpcFrequency_ = 10000000;
        return true;
    }
    static bool Update(FaceCameraRenderer& renderer) { return renderer.UpdateCameraTexture(); }
    static size_t Size(const FaceCameraRenderer& renderer) { return renderer.history_.size(); }
    static size_t Next(const FaceCameraRenderer& renderer) { return renderer.historyNext_; }
    static int64_t SelectedQpc(const FaceCameraRenderer& renderer) {
        for (const auto& slot : renderer.history_)
            if (slot.texture == renderer.cameraTexture_)
                return slot.captureQpc;
        return 0;
    }
    static ce::face_camera::LagSummary Lag(const FaceCameraRenderer& renderer) { return renderer.lagSummary_; }
};

TEST(FaceCameraRendererHistoryTest, GrowthKeepsTheSelectedFrameAndItsTimestampAlive) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, &device, nullptr, &context)));
    FaceCameraRenderer renderer(ce::face_camera::Config{});
    ASSERT_TRUE(renderer.InitRenderer(device.Get(), context.Get()));
    constexpr int64_t kNewest = 1000000000;
    constexpr int64_t kInterval = 10000000 / 30;
    constexpr int64_t kTarget = kNewest - 3000000;
    constexpr int64_t kOldest = kNewest - 3 * kInterval;
    ASSERT_TRUE(FaceCameraRendererTestAccess::SeedHistory(renderer, kNewest, kInterval));
    renderer.SetContentTargetQpc(kTarget);

    // Four camera frames cannot cover 300 ms. This reallocates the actual renderer's
    // history; sanitizer builds also check that its lag accounting retains no old entry.
    ASSERT_TRUE(FaceCameraRendererTestAccess::Update(renderer));
    EXPECT_EQ(FaceCameraRendererTestAccess::Size(renderer), 11u);
    EXPECT_EQ(FaceCameraRendererTestAccess::Next(renderer), 4u);
    EXPECT_EQ(FaceCameraRendererTestAccess::SelectedQpc(renderer), kOldest);
    auto lag = FaceCameraRendererTestAccess::Lag(renderer);
    EXPECT_EQ(lag.count, 1u);
    EXPECT_EQ(lag.olderThanHistory, 1u);
    EXPECT_EQ(lag.sumUs, (kTarget - kOldest) / 10);

    // Empty new slots do not replace the selected frame while the longer history fills.
    ASSERT_TRUE(FaceCameraRendererTestAccess::Update(renderer));
    EXPECT_EQ(FaceCameraRendererTestAccess::Size(renderer), 11u);
    EXPECT_EQ(FaceCameraRendererTestAccess::SelectedQpc(renderer), kOldest);
    lag = FaceCameraRendererTestAccess::Lag(renderer);
    EXPECT_EQ(lag.count, 2u);
    EXPECT_EQ(lag.sumUs, 2 * ((kTarget - kOldest) / 10));
}

// The retained NV12/P010 route draws the camera alone into an FP32 layer and
// patches it into the YUV frame; a layer the renderer refuses loses the camera
// on every retained frame without an error.
TEST(FaceCameraRendererHistoryTest, DrawsTheCameraIntoTheRetainedYuvOverlayLayer) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, &device, nullptr, &context)));
    FaceCameraRenderer renderer(ce::face_camera::Config{});
    ASSERT_TRUE(renderer.InitRenderer(device.Get(), context.Get()));
    constexpr int64_t kNewest = 1000000000;
    ASSERT_TRUE(FaceCameraRendererTestAccess::SeedHistory(renderer, kNewest, 10000000 / 30));
    renderer.SetContentTargetQpc(kNewest);

    constexpr uint32_t kFrame = 256;
    RECT rect = {};
    ASSERT_TRUE(renderer.GetOverlayFrameRect(kFrame, kFrame, kFrame, kFrame, &rect));
    RetainedYuvOverlayLayer layer;
    ASSERT_TRUE(layer.Prepare(device.Get(), context.Get(), rect));
    D3D11_TEXTURE2D_DESC desc = {};
    layer.texture->GetDesc(&desc);
    EXPECT_EQ(desc.Format, ce::video_format::kRetainedYuvOverlayLayerFormat);

    EXPECT_TRUE(renderer.CompositePreparedFrame(layer.texture, kFrame, kFrame, FaceCameraColorMode::Sdr, 80.0f,
                                                rect.left, rect.top));
    const RetainedYuvLayerCoverage coverage = ReadRetainedYuvLayerCoverage(device.Get(), context.Get(), layer);
    ASSERT_TRUE(coverage.read);
    EXPECT_GT(coverage.maxAlpha, 0.99f);  // opacity 1 inside the camera shape
    EXPECT_GT(coverage.coveredTexels, 0u);
    EXPECT_EQ(coverage.unpremultiplied, 0u);
    layer.Release();
}
