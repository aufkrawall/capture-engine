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
    static bool UploadCpuFrameToSlot0(FaceCameraRenderer& renderer, const FaceCameraFrame& frame) {
        renderer.ReleaseHistory();
        renderer.history_.resize(1);
        auto& slot = renderer.history_[0];
        if (!renderer.UploadCpuFrame(frame, slot))
            return false;
        slot.sequence = frame.sequence > 0 ? frame.sequence : 1;
        slot.captureQpc = frame.captureQpc > 0 ? frame.captureQpc : 1000000000;
        renderer.lastCaptureQpc_ = slot.captureQpc;
        renderer.cameraIntervalQpc_ = 10000000 / 30;
        renderer.qpcFrequency_ = 10000000;
        renderer.uploadedTickMs_ = GetTickCount64();
        return true;
    }
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

namespace {

bool ReadTargetPixelBgra(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* target, uint32_t x,
                         uint32_t y, uint8_t outBgra[4]) {
    D3D11_TEXTURE2D_DESC desc = {};
    target->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
        return false;
    context->CopyResource(staging.Get(), target);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    const auto* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
    std::memcpy(outBgra, row + static_cast<size_t>(x) * 4u, 4u);
    context->Unmap(staging.Get(), 0);
    return true;
}

}  // namespace

TEST(FaceCameraRendererHistoryTest, RendersNv12AndYuy2WithConfiguredMatrixAndRange) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, &device, nullptr, &context)));

    ce::face_camera::Config config;
    config.shape = ce::face_camera::Shape::kRectangle;
    config.borderWidthPercent = 0.0f;
    config.marginPercent = 0.0f;
    config.widthPercent = 100.0f;
    config.opacity = 1.0f;
    config.mirror = false;
    FaceCameraRenderer renderer(config);
    ASSERT_TRUE(renderer.InitRenderer(device.Get(), context.Get()));

    constexpr uint32_t kWidth = 16;
    constexpr uint32_t kHeight = 16;
    D3D11_TEXTURE2D_DESC targetDesc = {};
    targetDesc.Width = kWidth;
    targetDesc.Height = kHeight;
    targetDesc.MipLevels = 1;
    targetDesc.ArraySize = 1;
    targetDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    targetDesc.SampleDesc.Count = 1;
    targetDesc.Usage = D3D11_USAGE_DEFAULT;
    targetDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> target;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&targetDesc, nullptr, &target)));

    auto renderAndSampleCenter = [&](const FaceCameraFrame& frame, uint8_t bgra[4]) -> bool {
        if (!FaceCameraRendererTestAccess::UploadCpuFrameToSlot0(renderer, frame))
            return false;
        renderer.SetContentTargetQpc(frame.captureQpc);
        RECT rect = {};
        if (!renderer.GetOverlayFrameRect(kWidth, kHeight, kWidth, kHeight, &rect))
            return false;
        if (!renderer.CompositePreparedFrame(target.Get(), kWidth, kHeight, FaceCameraColorMode::Sdr, 80.0f))
            return false;
        return ReadTargetPixelBgra(device.Get(), context.Get(), target.Get(), kWidth / 2u, kHeight / 2u, bgra);
    };

    // 1. NV12 studio black (Y=16, Cb=128, Cr=128): limited range maps to ~0, full range maps to ~16.
    FaceCameraFrame nv12Frame;
    nv12Frame.sequence = 1;
    nv12Frame.captureQpc = 1000000000;
    nv12Frame.width = kWidth;
    nv12Frame.height = kHeight;
    nv12Frame.format = ce::face_camera::PixelFormat::kNv12;
    nv12Frame.matrix = ce::face_camera::YuvMatrix::kBt709;
    nv12Frame.range = ce::face_camera::YuvRange::kLimited;
    nv12Frame.bgra.assign(kWidth * kHeight, 16u);
    nv12Frame.chroma.assign(kWidth * (kHeight / 2u), 128u);

    uint8_t limitedBlack[4] = {};
    ASSERT_TRUE(renderAndSampleCenter(nv12Frame, limitedBlack));
    EXPECT_EQ(renderer.GpuBytes(), static_cast<uint64_t>(kWidth) * kHeight * 3u / 2u);
    EXPECT_LE(limitedBlack[0], 1u);
    EXPECT_LE(limitedBlack[1], 1u);
    EXPECT_LE(limitedBlack[2], 1u);

    nv12Frame.sequence = 2;
    nv12Frame.range = ce::face_camera::YuvRange::kFull;
    uint8_t fullRangeAt16[4] = {};
    ASSERT_TRUE(renderAndSampleCenter(nv12Frame, fullRangeAt16));
    EXPECT_NEAR(fullRangeAt16[0], 16, 1);
    EXPECT_NEAR(fullRangeAt16[1], 16, 1);
    EXPECT_NEAR(fullRangeAt16[2], 16, 1);

    // 2. NV12 chroma matrix difference (Y=126, Cb=128, Cr=200): BT.601 produces more green attenuation than BT.709.
    nv12Frame.sequence = 3;
    nv12Frame.range = ce::face_camera::YuvRange::kLimited;
    nv12Frame.matrix = ce::face_camera::YuvMatrix::kBt709;
    nv12Frame.bgra.assign(kWidth * kHeight, 126u);
    for (size_t i = 0; i < nv12Frame.chroma.size(); i += 2) {
        nv12Frame.chroma[i] = 128u;
        nv12Frame.chroma[i + 1] = 200u;
    }
    uint8_t bt709Pixel[4] = {};
    ASSERT_TRUE(renderAndSampleCenter(nv12Frame, bt709Pixel));

    nv12Frame.sequence = 4;
    nv12Frame.matrix = ce::face_camera::YuvMatrix::kBt601;
    uint8_t bt601Pixel[4] = {};
    ASSERT_TRUE(renderAndSampleCenter(nv12Frame, bt601Pixel));
    // Green channel (index 1 in BGRA) is lower under BT.601 (-0.714*Cr) than BT.709 (-0.468*Cr).
    EXPECT_LT(bt601Pixel[1] + 10, bt709Pixel[1]);

    // 3. YUY2 packed 4:2:2 with distinct even (Y0=16 -> 0) and odd (Y1=235 -> 255) luma columns.
    FaceCameraFrame yuy2Frame;
    yuy2Frame.sequence = 5;
    yuy2Frame.captureQpc = 1000000000;
    yuy2Frame.width = kWidth;
    yuy2Frame.height = kHeight;
    yuy2Frame.format = ce::face_camera::PixelFormat::kYuy2;
    yuy2Frame.matrix = ce::face_camera::YuvMatrix::kBt709;
    yuy2Frame.range = ce::face_camera::YuvRange::kLimited;
    yuy2Frame.bgra.resize(kWidth * kHeight * 2u);
    for (size_t i = 0; i < yuy2Frame.bgra.size(); i += 4) {
        yuy2Frame.bgra[i + 0] = 16u;   // Y0 (even column)
        yuy2Frame.bgra[i + 1] = 128u;  // Cb
        yuy2Frame.bgra[i + 2] = 235u;  // Y1 (odd column)
        yuy2Frame.bgra[i + 3] = 128u;  // Cr
    }
    ASSERT_TRUE(FaceCameraRendererTestAccess::UploadCpuFrameToSlot0(renderer, yuy2Frame));
    EXPECT_EQ(renderer.GpuBytes(), static_cast<uint64_t>(kWidth) * kHeight * 2u);
    renderer.SetContentTargetQpc(yuy2Frame.captureQpc);
    RECT rect = {};
    ASSERT_TRUE(renderer.GetOverlayFrameRect(kWidth, kHeight, kWidth, kHeight, &rect));
    ASSERT_TRUE(renderer.CompositePreparedFrame(target.Get(), kWidth, kHeight, FaceCameraColorMode::Sdr, 80.0f));

    uint8_t evenColBgra[4] = {};
    uint8_t oddColBgra[4] = {};
    ASSERT_TRUE(ReadTargetPixelBgra(device.Get(), context.Get(), target.Get(), 4u, kHeight / 2u, evenColBgra));
    ASSERT_TRUE(ReadTargetPixelBgra(device.Get(), context.Get(), target.Get(), 5u, kHeight / 2u, oddColBgra));
    EXPECT_LE(evenColBgra[1], 2u);
    EXPECT_GE(oddColBgra[1], 253u);
}
