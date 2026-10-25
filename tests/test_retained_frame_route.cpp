#include <gtest/gtest.h>

#include <cstring>

#include "mediaengine/video/retained_frame_route.h"

// Which reservoirs may hold NV12/P010 instead of RGB (retained_frame_route.h).

namespace {

namespace vf = ce::video_format;

vf::RetainedFrameRouteInputs Eligible(DXGI_FORMAT source, bool hdr, bool tenBit) {
    vf::RetainedFrameRouteInputs in;
    in.sourceFormat = source;
    in.width = 3840;
    in.height = 2160;
    in.sourceIsHdr = hdr;
    in.hdrOutput = hdr;
    in.use10BitOutput = tenBit;
    in.formatResolved = true;
    in.encoderUsesYuvInput = true;
    in.sameDevice = true;
    in.enabled = true;
    return in;
}

}  // namespace

TEST(RetainedFrameRouteTest, SdrSourcesRetainTheEncodersYuvFormat) {
    auto r10 = vf::DecideRetainedFrameRoute(Eligible(DXGI_FORMAT_R10G10B10A2_UNORM, false, true));
    EXPECT_TRUE(r10.yuv);
    EXPECT_EQ(r10.format, DXGI_FORMAT_P010);
    EXPECT_EQ(r10.normalizeTransform, vf::RgbColorTransform::kNone);
    EXPECT_EQ(r10.rgbLevels, 1023u);

    auto fp16 = vf::DecideRetainedFrameRoute(Eligible(DXGI_FORMAT_R16G16B16A16_FLOAT, false, true));
    EXPECT_TRUE(fp16.yuv);
    EXPECT_EQ(fp16.normalizeTransform, vf::RgbColorTransform::kLinearToSrgb);  // the old FP16 -> R10 pool pass
    EXPECT_EQ(fp16.rgbLevels, 1023u);

    auto bgra = vf::DecideRetainedFrameRoute(Eligible(DXGI_FORMAT_B8G8R8A8_UNORM, false, false));
    EXPECT_TRUE(bgra.yuv);
    EXPECT_EQ(bgra.format, DXGI_FORMAT_NV12);
    EXPECT_EQ(bgra.rgbLevels, 255u);

    // bit_depth=8 with a 10-bit source still converts at RGB10 precision.
    auto fp16To8 = vf::DecideRetainedFrameRoute(Eligible(DXGI_FORMAT_R16G16B16A16_FLOAT, false, false));
    EXPECT_EQ(fp16To8.format, DXGI_FORMAT_NV12);
    EXPECT_EQ(fp16To8.rgbLevels, 1023u);
}

TEST(RetainedFrameRouteTest, HdrScRgbBecomesPqP010) {
    auto hdr = vf::DecideRetainedFrameRoute(Eligible(DXGI_FORMAT_R16G16B16A16_FLOAT, true, true));
    EXPECT_TRUE(hdr.yuv);
    EXPECT_EQ(hdr.format, DXGI_FORMAT_P010);
    EXPECT_EQ(hdr.normalizeTransform, vf::RgbColorTransform::kScRgbToHdr10);
}

TEST(RetainedFrameRouteTest, LiveOrDifferentPipelinesKeepTheRgbReservoir) {
    auto expectRgb = [](vf::RetainedFrameRouteInputs in, const char* reason) {
        const auto decision = vf::DecideRetainedFrameRoute(in);
        EXPECT_FALSE(decision.yuv) << reason;
        EXPECT_STREQ(decision.reason, reason);
    };
    auto in = Eligible(DXGI_FORMAT_R10G10B10A2_UNORM, false, true);
    in.enabled = false;
    expectRgb(in, "disabled");
    // A split capture device (DXGI duplication) without planar sharing.
    in = Eligible(DXGI_FORMAT_R10G10B10A2_UNORM, false, true);
    in.sameDevice = false;
    expectRgb(in, "split capture device cannot share P010");
    in.splitShareNv12 = true;  // the other format does not help
    expectRgb(in, "split capture device cannot share P010");
    in = Eligible(DXGI_FORMAT_B8G8R8A8_UNORM, false, false);
    in.sameDevice = false;
    in.splitShareP010 = true;
    expectRgb(in, "split capture device cannot share NV12");
    in = Eligible(DXGI_FORMAT_R10G10B10A2_UNORM, false, true);
    in.scaling = true;
    expectRgb(in, "output scaling");
    in = Eligible(DXGI_FORMAT_R10G10B10A2_UNORM, false, true);
    in.width = 1919;
    expectRgb(in, "odd frame size");
    in = Eligible(DXGI_FORMAT_R10G10B10A2_UNORM, false, true);
    in.geometryMismatch = true;
    expectRgb(in, "encoder geometry locked to another size");
    in = Eligible(DXGI_FORMAT_R10G10B10A2_UNORM, false, true);
    in.formatResolved = false;
    expectRgb(in, "encoder format unresolved");
    in = Eligible(DXGI_FORMAT_R10G10B10A2_UNORM, false, true);
    in.encoderUsesYuvInput = false;
    expectRgb(in, "direct RGB encoder input");
    expectRgb(Eligible(DXGI_FORMAT_R8_UNORM, false, false), "source format");

    // HDR desktop recorded as SDR: the tone map follows the live SDR white.
    in = Eligible(DXGI_FORMAT_R16G16B16A16_FLOAT, true, true);
    in.hdrOutput = false;
    expectRgb(in, "HDR-to-SDR tone map");
    // An HDR10 RGB source blends overlays in PQ, not linear scRGB.
    expectRgb(Eligible(DXGI_FORMAT_R10G10B10A2_UNORM, true, true), "HDR source is not FP16 scRGB");
}

TEST(RetainedFrameRouteTest, RetainedYuvFormatsAreNv12P010AndPackedP010) {
    EXPECT_TRUE(vf::IsRetainedYuvFormat(DXGI_FORMAT_NV12));
    EXPECT_TRUE(vf::IsRetainedYuvFormat(DXGI_FORMAT_P010));
    EXPECT_TRUE(vf::IsRetainedYuvFormat(DXGI_FORMAT_R16_UINT));  // packed P010 slot
    EXPECT_FALSE(vf::IsRetainedYuvFormat(DXGI_FORMAT_R16_UNORM));
    EXPECT_FALSE(vf::IsRetainedYuvFormat(DXGI_FORMAT_R10G10B10A2_UNORM));
    EXPECT_FALSE(vf::IsRetainedYuvFormat(DXGI_FORMAT_P016));
    // A packed slot feeds the encoder's P010 frame; the others feed their own format.
    EXPECT_EQ(vf::RetainedYuvEncoderFormat(DXGI_FORMAT_R16_UINT), DXGI_FORMAT_P010);
    EXPECT_EQ(vf::RetainedYuvEncoderFormat(DXGI_FORMAT_P010), DXGI_FORMAT_P010);
    EXPECT_EQ(vf::RetainedYuvEncoderFormat(DXGI_FORMAT_NV12), DXGI_FORMAT_NV12);
}

TEST(RetainedFrameRouteTest, TenBitFramesArePackedWhenAllowedPreparedAndShareable) {
    auto packedInputs = [](DXGI_FORMAT source, bool hdr) {
        auto in = Eligible(source, hdr, true);
        in.allowPackedP010 = true;
        in.packedP010Ready = true;
        return in;
    };
    // Same device (WGC): nothing to share.
    auto sdr = vf::DecideRetainedFrameRoute(packedInputs(DXGI_FORMAT_R10G10B10A2_UNORM, false));
    EXPECT_TRUE(sdr.yuv);
    EXPECT_EQ(sdr.format, DXGI_FORMAT_R16_UINT);
    EXPECT_STREQ(sdr.reason, "yuv (packed P010)");
    EXPECT_STREQ(sdr.packing, "packed");
    auto hdr = vf::DecideRetainedFrameRoute(packedInputs(DXGI_FORMAT_R16G16B16A16_FLOAT, true));
    EXPECT_EQ(hdr.format, DXGI_FORMAT_R16_UINT);
    EXPECT_EQ(hdr.normalizeTransform, vf::RgbColorTransform::kScRgbToHdr10);
    EXPECT_STREQ(hdr.reason, "yuv (scRGB to PQ, packed P010)");

    // Split capture device (dxgi_dup): packed needs R16_UINT sharing, and it
    // does not need P010 sharing at all.
    auto in = packedInputs(DXGI_FORMAT_R10G10B10A2_UNORM, false);
    in.sameDevice = false;
    in.splitSharePackedP010 = true;
    EXPECT_EQ(vf::DecideRetainedFrameRoute(in).format, DXGI_FORMAT_R16_UINT);
    in.splitSharePackedP010 = false;
    in.splitShareP010 = true;
    auto fallback = vf::DecideRetainedFrameRoute(in);
    EXPECT_EQ(fallback.format, DXGI_FORMAT_P010);
    EXPECT_STREQ(fallback.packing, "split capture device cannot share R16_UINT");
    in.splitShareP010 = false;
    EXPECT_FALSE(vf::DecideRetainedFrameRoute(in).yuv);

    // The A/B switch and missing unpack shaders keep P010, never RGB.
    in = packedInputs(DXGI_FORMAT_R10G10B10A2_UNORM, false);
    in.allowPackedP010 = false;
    auto disabled = vf::DecideRetainedFrameRoute(in);
    EXPECT_EQ(disabled.format, DXGI_FORMAT_P010);
    EXPECT_STREQ(disabled.packing, "disabled by wgc_retained_packed_p010=false");
    in = packedInputs(DXGI_FORMAT_R10G10B10A2_UNORM, false);
    in.packedP010Ready = false;
    auto unprepared = vf::DecideRetainedFrameRoute(in);
    EXPECT_EQ(unprepared.format, DXGI_FORMAT_P010);
    EXPECT_STREQ(unprepared.packing, "unpack shaders unavailable");

    // 8-bit output stays NV12: it has no padding to remove.
    auto eightBit = packedInputs(DXGI_FORMAT_B8G8R8A8_UNORM, false);
    eightBit.use10BitOutput = false;
    auto nv12 = vf::DecideRetainedFrameRoute(eightBit);
    EXPECT_EQ(nv12.format, DXGI_FORMAT_NV12);
    EXPECT_STREQ(nv12.packing, "8-bit");
}

TEST(RetainedFrameRouteTest, SplitCaptureDeviceRetainsYuvWhenTheFormatIsShared) {
    // DXGI duplication always captures on its own device; the 2025 MB R10
    // reservoir of the 4K120 10-bit dxgi_dup run (20261016_183018) becomes P010.
    auto in = Eligible(DXGI_FORMAT_R10G10B10A2_UNORM, false, true);
    in.sameDevice = false;
    in.splitShareP010 = true;
    const auto p010 = vf::DecideRetainedFrameRoute(in);
    EXPECT_TRUE(p010.yuv);
    EXPECT_EQ(p010.format, DXGI_FORMAT_P010);
    EXPECT_STREQ(p010.reason, "yuv");

    in = Eligible(DXGI_FORMAT_B8G8R8A8_UNORM, false, false);
    in.sameDevice = false;
    in.splitShareNv12 = true;
    const auto nv12 = vf::DecideRetainedFrameRoute(in);
    EXPECT_TRUE(nv12.yuv);
    EXPECT_EQ(nv12.format, DXGI_FORMAT_NV12);

    // Every other exclusion still applies to a split device.
    in = Eligible(DXGI_FORMAT_R10G10B10A2_UNORM, false, true);
    in.sameDevice = false;
    in.splitShareP010 = true;
    in.scaling = true;
    EXPECT_FALSE(vf::DecideRetainedFrameRoute(in).yuv);
}
