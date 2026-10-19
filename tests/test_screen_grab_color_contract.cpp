#include <gtest/gtest.h>

#include <d3d11.h>
#include <wrl/client.h>

#include "mediaengine/video/video_format_policy.h"
#include "mediaengine/video/video_encoder.h"

struct VideoEncoderScreenGrabTestAccess {
    static bool Arm(VideoEncoder& encoder, ID3D11Texture2D* texture, const char* bitDepth, bool input10,
                    bool display10 = false) {
        if (!encoder.AdoptTextureDevice(texture))
            return false;
        encoder.savedConfig.bitDepth = bitDepth;
        encoder.sourcePrefers10Bit_.store(display10, std::memory_order_relaxed);
        encoder.currentUse10BitInput = input10;
        encoder.currentIsHDR = false;
        encoder.width = 64;
        encoder.lockedGeometryWidth = 64;
        encoder.height = 32;
        encoder.lockedGeometryHeight = 32;
        // Exercise the actual admission path with a committed output contract;
        // EnsureDevice/OpenOutput already succeeded, so no codec or file is needed.
        encoder.recordingRequested = encoder.fileOpened = encoder.initDone = true;
        return true;
    }
    static bool ContractChanged(const VideoEncoder& encoder) { return encoder.sourceContractChanged.load(); }
    static bool Input10(const VideoEncoder& encoder) { return encoder.currentUse10BitInput; }
    static void Disarm(VideoEncoder& encoder) {
        encoder.recordingRequested = encoder.fileOpened = encoder.initDone = false;
    }
};

namespace {
using Microsoft::WRL::ComPtr;

struct EncoderProbe {
    VideoEncoder encoder;
    ~EncoderProbe() { VideoEncoderScreenGrabTestAccess::Disarm(encoder); }
};

ComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, DXGI_FORMAT format, UINT width, UINT height) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = format;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> texture;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture))) << format;
    return texture;
}

ComPtr<ID3D11Device> Device() {
    ComPtr<ID3D11Device> device;
    EXPECT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, &device, nullptr, nullptr)));
    return device;
}
}  // namespace

TEST(ScreenGrabColorContractTest, RetainedToRgbResizeAndReturnKeepTheCommittedBitDepth) {
    auto device = Device();
    ASSERT_TRUE(device);
    struct Case {
        DXGI_FORMAT rgb;
        DXGI_FORMAT retained;
        const char* depth;
    };
    const Case cases[] = {{DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_P010, "10"},
                          {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_NV12, "8"}};
    for (const auto& c : cases) {
        SCOPED_TRACE(c.depth);
        auto retained = Texture(device.Get(), c.retained, 64, 32);
        auto resized = Texture(device.Get(), c.rgb, 32, 16);
        ASSERT_TRUE(retained);
        ASSERT_TRUE(resized);
        EncoderProbe probe;
        ASSERT_TRUE(VideoEncoderScreenGrabTestAccess::Arm(probe.encoder, retained.Get(), c.depth,
                                                          c.retained == DXGI_FORMAT_P010));
        ASSERT_TRUE(probe.encoder.PrepareFrameD3D11(resized.Get(), 32, 16, false));
        EXPECT_FALSE(VideoEncoderScreenGrabTestAccess::ContractChanged(probe.encoder));
        EXPECT_EQ(VideoEncoderScreenGrabTestAccess::Input10(probe.encoder),
                  ce::video_format::IsHighPrecisionRgbInputFormat(c.rgb));
        ASSERT_TRUE(probe.encoder.PrepareFrameD3D11(retained.Get(), 64, 32, false));
        EXPECT_FALSE(VideoEncoderScreenGrabTestAccess::ContractChanged(probe.encoder));
    }
}

TEST(ScreenGrabColorContractTest, AutoDepthWithDisplayHintSurvivesP010ToBgraResize) {
    auto device = Device();
    ASSERT_TRUE(device);
    auto retained = Texture(device.Get(), DXGI_FORMAT_P010, 64, 32);
    auto rgb = Texture(device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 32, 16);
    ASSERT_TRUE(retained);
    ASSERT_TRUE(rgb);
    EncoderProbe probe;
    ASSERT_TRUE(VideoEncoderScreenGrabTestAccess::Arm(probe.encoder, retained.Get(), "auto", true, true));
    EXPECT_TRUE(probe.encoder.PrepareFrameD3D11(rgb.Get(), 32, 16, false));
    EXPECT_FALSE(VideoEncoderScreenGrabTestAccess::ContractChanged(probe.encoder));
}

TEST(ScreenGrabColorContractTest, ActualAutoDepthAndHdrChangesStillRequestStop) {
    auto device = Device();
    ASSERT_TRUE(device);
    auto rgb = Texture(device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 64, 32);
    auto rgb10 = Texture(device.Get(), DXGI_FORMAT_R10G10B10A2_UNORM, 32, 16);
    ASSERT_TRUE(rgb);
    ASSERT_TRUE(rgb10);
    for (const bool hdr : {false, true}) {
        EncoderProbe probe;
        ASSERT_TRUE(VideoEncoderScreenGrabTestAccess::Arm(probe.encoder, rgb.Get(), "auto", false));
        EXPECT_FALSE(probe.encoder.PrepareFrameD3D11(rgb10.Get(), 32, 16, hdr));
        EXPECT_TRUE(VideoEncoderScreenGrabTestAccess::ContractChanged(probe.encoder));
    }
}
