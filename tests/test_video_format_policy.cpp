#include <gtest/gtest.h>

#include "../mediaengine/video_format_policy.h"

namespace vf = ce::video_format;

TEST(VideoFormatPolicyTest, Fp16SourcesUseTypedFp16ShaderResourceViews) {
    EXPECT_EQ(vf::GetRgbShaderResourceViewFormat(DXGI_FORMAT_R16G16B16A16_FLOAT), DXGI_FORMAT_R16G16B16A16_FLOAT);
    EXPECT_EQ(vf::GetRgbShaderResourceViewFormat(DXGI_FORMAT_R16G16B16A16_TYPELESS), DXGI_FORMAT_R16G16B16A16_FLOAT);
}

TEST(VideoFormatPolicyTest, R10SourcesUseTypedR10ShaderResourceViews) {
    EXPECT_EQ(vf::GetRgbShaderResourceViewFormat(DXGI_FORMAT_R10G10B10A2_UNORM), DXGI_FORMAT_R10G10B10A2_UNORM);
    EXPECT_EQ(vf::GetRgbShaderResourceViewFormat(DXGI_FORMAT_R10G10B10A2_TYPELESS), DXGI_FORMAT_R10G10B10A2_UNORM);
}

TEST(VideoFormatPolicyTest, CommonRgbTypelessFormatsUseTypedUnormViews) {
    EXPECT_EQ(vf::GetRgbShaderResourceViewFormat(DXGI_FORMAT_B8G8R8A8_TYPELESS), DXGI_FORMAT_B8G8R8A8_UNORM);
    EXPECT_EQ(vf::GetRgbShaderResourceViewFormat(DXGI_FORMAT_R8G8B8A8_TYPELESS), DXGI_FORMAT_R8G8B8A8_UNORM);
}

TEST(VideoFormatPolicyTest, ShaderResourceViewPolicyNeverReturnsTypelessFormats) {
    const DXGI_FORMAT formats[] = {
        DXGI_FORMAT_R16G16B16A16_FLOAT,   DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM,        DXGI_FORMAT_B8G8R8A8_TYPELESS,
        DXGI_FORMAT_R8G8B8A8_UNORM,       DXGI_FORMAT_R8G8B8A8_TYPELESS,
    };

    for (const DXGI_FORMAT format : formats) {
        const DXGI_FORMAT srvFormat = vf::GetRgbShaderResourceViewFormat(format);
        EXPECT_NE(srvFormat, DXGI_FORMAT_UNKNOWN) << static_cast<int>(format);
        EXPECT_FALSE(vf::IsTypelessDxgiFormat(srvFormat)) << static_cast<int>(format);
    }
}

TEST(VideoFormatPolicyTest, SdrFp16FallbackRequestsGammaEncoding) {
    EXPECT_TRUE(vf::ShouldApplySdrLinearToSrgbBeforeRgb10(DXGI_FORMAT_R16G16B16A16_FLOAT, false));
    EXPECT_TRUE(vf::ShouldApplySdrLinearToSrgbBeforeRgb10(DXGI_FORMAT_R16G16B16A16_TYPELESS, false));
    EXPECT_FALSE(vf::ShouldApplySdrLinearToSrgbBeforeRgb10(DXGI_FORMAT_R16G16B16A16_FLOAT, true));
    EXPECT_FALSE(vf::ShouldApplySdrLinearToSrgbBeforeRgb10(DXGI_FORMAT_R10G10B10A2_UNORM, false));
}
