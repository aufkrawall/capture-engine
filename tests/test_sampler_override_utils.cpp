#include <gtest/gtest.h>

#include "../common/config.h"
#include "../hook/common/sampler_override_utils.h"

namespace {

using namespace ce::sampler_override;

TEST(SamplerOverrideUtilsTest, ResolvesFullMipCountForAutogenTextures) {
    EXPECT_EQ(ResolveFullMipCount2D(1024, 256, 0), 11u);
    EXPECT_EQ(ResolveFullMipCount2D(1, 1, 0), 1u);
    EXPECT_EQ(ResolveFullMipCount2D(128, 128, 5), 5u);
}

TEST(SamplerOverrideUtilsTest, ResolvesVisibleMipCountForAllRemainingViews) {
    EXPECT_EQ(ResolveVisibleMipCount(8, 2, UINT_MAX), 6u);
    EXPECT_EQ(ResolveVisibleMipCount(4, 4, UINT_MAX), 0u);
    EXPECT_EQ(ResolveVisibleMipCount(8, 1, 3), 3u);
}

TEST(SamplerOverrideUtilsTest, ClassifiesD3D11ComparisonAndReductionFilters) {
    EXPECT_TRUE(IsD3D11ComparisonFilter(D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR));
    EXPECT_FALSE(IsD3D11ComparisonFilter(D3D11_FILTER_ANISOTROPIC));
    EXPECT_TRUE(IsD3D11ReductionFilter(D3D11_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR));
    EXPECT_FALSE(IsD3D11ReductionFilter(D3D11_FILTER_COMPARISON_ANISOTROPIC));
    EXPECT_EQ(GetForcedAnisotropicFilter(D3D11_FILTER_MIN_MAG_MIP_LINEAR), D3D11_FILTER_ANISOTROPIC);
}

TEST(SamplerOverrideUtilsTest, ClassifiesD3D12ComparisonAndReductionFilters) {
    EXPECT_TRUE(IsD3D12ComparisonFilter(D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR));
    EXPECT_FALSE(IsD3D12ComparisonFilter(D3D12_FILTER_ANISOTROPIC));
    EXPECT_TRUE(IsD3D12ReductionFilter(D3D12_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR));
    EXPECT_FALSE(IsD3D12ReductionFilter(D3D12_FILTER_COMPARISON_ANISOTROPIC));
    EXPECT_EQ(GetForcedAnisotropicFilter(D3D12_FILTER_MIN_MAG_MIP_LINEAR), D3D12_FILTER_ANISOTROPIC);
}

TEST(SamplerOverrideUtilsTest, HashChangesWhenSamplerOverrideInputsChange) {
    GraphicsConfig base;
    base.anisotropicFiltering = "16x";
    base.mipMapping = "default";
    base.mipBias = "0.0";
    base.mipBiasMode = "strict";
    base.msaaSamples = "4x";

    GraphicsConfig changed = base;
    changed.anisotropicFiltering = "8x";

    EXPECT_NE(HashSamplerOverrideConfig(base), HashSamplerOverrideConfig(changed));
}

TEST(SamplerOverrideUtilsTest, AnisotropicOverrideEnabledOnlyForExplicitNonOffModes) {
    GraphicsConfig gfx;

    gfx.anisotropicFiltering = "default";
    EXPECT_FALSE(IsAnisotropicOverrideEnabled(gfx));

    gfx.anisotropicFiltering = "off";
    EXPECT_FALSE(IsAnisotropicOverrideEnabled(gfx));

    gfx.anisotropicFiltering = "8x";
    EXPECT_TRUE(IsAnisotropicOverrideEnabled(gfx));
}

TEST(SamplerOverrideUtilsTest, D3D11ForcedAFIgnoresComparisonFuncOnNonComparisonFilters) {
    GraphicsConfig gfx;
    gfx.anisotropicFiltering = "16x";

    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = D3D11_FLOAT32_MAX;

    EXPECT_EQ(ClassifyD3D11SamplerForForcedAF(desc, gfx), D3D11ForcedAFSamplerDecision::Allow);
    EXPECT_TRUE(D3D11SamplerAllowsForcedAF(desc, gfx));

    desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    EXPECT_EQ(ClassifyD3D11SamplerForForcedAF(desc, gfx), D3D11ForcedAFSamplerDecision::ComparisonFilter);
}

TEST(SamplerOverrideUtilsTest, ParsesD3D11ShaderSamplerTexturePairs) {
    const char disassembly[] =
        "    sample r0.xyzw, v0.xyxx, t0.xyzw, s0\n"
        "    sample_l r1.xyzw, v1.xyxx, t7.xyzw, s0, l(0)\n"
        "    sample_c_lz r2.x, v2.xyxx, t13.xxxx, s2, r3.x\n"
        "    ld r3.xyzw, v3.xyxx, t4.xyzw\n"
        "    sampleinfo r4.xy, t8.xyzw\n";

    const D3D11ShaderSamplerUsage usage =
        ParseD3D11ShaderSamplerUsage(disassembly, sizeof(disassembly) - 1);

    EXPECT_TRUE(usage.sawSampleInstruction);
    EXPECT_FALSE(usage.sawUnsupportedRegister);
    EXPECT_EQ(CountD3D11ShaderSamplerTextureUses(usage, 0), 2u);
    EXPECT_TRUE(D3D11ShaderSamplerUsesTexture(usage, 0, 0));
    EXPECT_TRUE(D3D11ShaderSamplerUsesTexture(usage, 0, 7));
    EXPECT_TRUE(D3D11ShaderSamplerUsesExplicitSample(usage, 0));
    EXPECT_TRUE(D3D11ShaderSamplerUsesLodSample(usage, 0));
    EXPECT_TRUE(D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, 0));
    EXPECT_FALSE(D3D11ShaderSamplerUsesAFSafeSample(usage, 0));
    EXPECT_FALSE(D3D11ShaderSamplerUsesOnlyImplicitSample(usage, 0));
    EXPECT_TRUE(D3D11ShaderSamplerUsesExplicitSample(usage, 2));
    EXPECT_TRUE(D3D11ShaderSamplerUsesComparisonSample(usage, 2));
    EXPECT_TRUE(D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, 2));
    EXPECT_FALSE(D3D11ShaderSamplerUsesAFSafeSample(usage, 2));
    EXPECT_FALSE(D3D11ShaderSamplerUsesOnlyImplicitSample(usage, 2));
    EXPECT_TRUE(D3D11ShaderSamplerUsesTexture(usage, 2, 13));
    EXPECT_FALSE(D3D11ShaderSamplerUsesTexture(usage, 1, 4));
    EXPECT_FALSE(D3D11ShaderSamplerUsesTexture(usage, 0, 8));
}

TEST(SamplerOverrideUtilsTest, TracksD3D11ShaderSamplersThatUseOnlyImplicitSampling) {
    const char disassembly[] =
        "    sample r0.xyzw, v0.xyxx, t1.xyzw, s3\n"
        "    sample_indexable(texture2d)(float,float,float,float) r1.xyzw, v1.xyxx, t2.xyzw, s3\n";

    const D3D11ShaderSamplerUsage usage =
        ParseD3D11ShaderSamplerUsage(disassembly, sizeof(disassembly) - 1);

    EXPECT_TRUE(D3D11ShaderSamplerUsesOnlyImplicitSample(usage, 3));
    EXPECT_TRUE(D3D11ShaderSamplerUsesAFSafeSample(usage, 3));
    EXPECT_FALSE(D3D11ShaderSamplerUsesExplicitSample(usage, 3));
    EXPECT_EQ(CountD3D11ShaderSamplerTextureUses(usage, 3), 2u);
}

TEST(SamplerOverrideUtilsTest, TreatsD3D11SampleBiasAsAFSafeButOtherExplicitSamplesUnsafe) {
    const char disassembly[] =
        "    sample_b r0.xyzw, v0.xyxx, t4.xyzw, s5, l(0)\n"
        "    sample_b_indexable(texture2d)(float,float,float,float) r1.xyzw, v1.xyxx, t5.xyzw, s5, r0.x\n"
        "    sample_d_indexable(texture2d)(float,float,float,float) r2.xyzw, v2.xyxx, t6.xyzw, s6, r1.xyzw, r2.xyzw\n"
        "    sample_l_indexable(texture2d)(float,float,float,float) r3.xyzw, v3.xyxx, t7.xyzw, s7, l(0)\n"
        "    sample_c_lz_indexable(texture2d)(float,float,float,float) r4.x, v4.xyxx, t8.xxxx, s8, r4.x\n";

    const D3D11ShaderSamplerUsage usage =
        ParseD3D11ShaderSamplerUsage(disassembly, sizeof(disassembly) - 1);

    EXPECT_TRUE(D3D11ShaderSamplerUsesExplicitSample(usage, 5));
    EXPECT_TRUE(D3D11ShaderSamplerUsesBiasSample(usage, 5));
    EXPECT_FALSE(D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, 5));
    EXPECT_TRUE(D3D11ShaderSamplerUsesAFSafeSample(usage, 5));
    EXPECT_EQ(CountD3D11ShaderSamplerTextureUses(usage, 5), 2u);

    EXPECT_TRUE(D3D11ShaderSamplerUsesGradientSample(usage, 6));
    EXPECT_TRUE(D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, 6));
    EXPECT_FALSE(D3D11ShaderSamplerUsesAFSafeSample(usage, 6));

    EXPECT_TRUE(D3D11ShaderSamplerUsesLodSample(usage, 7));
    EXPECT_TRUE(D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, 7));
    EXPECT_FALSE(D3D11ShaderSamplerUsesAFSafeSample(usage, 7));

    EXPECT_TRUE(D3D11ShaderSamplerUsesComparisonSample(usage, 8));
    EXPECT_TRUE(D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, 8));
    EXPECT_FALSE(D3D11ShaderSamplerUsesAFSafeSample(usage, 8));

    const D3D11ShaderSamplerUsageSummary summary = SummarizeD3D11ShaderSamplerUsage(usage);
    EXPECT_EQ(summary.samplerCount, 4u);
    EXPECT_EQ(summary.texturePairCount, 5u);
    EXPECT_EQ(summary.biasSamplers, 1u);
    EXPECT_EQ(summary.lodSamplers, 1u);
    EXPECT_EQ(summary.gradientSamplers, 1u);
    EXPECT_EQ(summary.comparisonSamplers, 1u);
    EXPECT_EQ(summary.afSafeSamplers, 1u);
    EXPECT_EQ(summary.unsafeExplicitSamplers, 3u);
}

TEST(SamplerOverrideUtilsTest, ParsesD3D11ShaderSamplerUsageCaseInsensitivelyAndFlagsUnsupportedRegisters) {
    const char disassembly[] =
        "SAMPLE r0.xyzw, v0.xyxx, t1.xyzw, s3\n"
        "sample r1.xyzw, v1.xyxx, t130.xyzw, s16\n";

    const D3D11ShaderSamplerUsage usage =
        ParseD3D11ShaderSamplerUsage(disassembly, sizeof(disassembly) - 1);

    EXPECT_TRUE(usage.sawSampleInstruction);
    EXPECT_TRUE(usage.sawUnsupportedRegister);
    EXPECT_TRUE(D3D11ShaderSamplerUsesTexture(usage, 3, 1));
    EXPECT_TRUE(D3D11ShaderSamplerUsesAnyTexture(usage, 3));
    EXPECT_FALSE(D3D11ShaderSamplerUsesAnyTexture(usage, 4));
}

TEST(SamplerOverrideUtilsTest, D3D11ForcedAFRejectsUnsafeSamplerDescriptors) {
    GraphicsConfig gfx;
    gfx.anisotropicFiltering = "16x";

    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = D3D11_FLOAT32_MAX;

    desc.MaxLOD = 0.0f;
    EXPECT_EQ(ClassifyD3D11SamplerForForcedAF(desc, gfx), D3D11ForcedAFSamplerDecision::FixedLOD);

    desc.MaxLOD = D3D11_FLOAT32_MAX;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    EXPECT_EQ(ClassifyD3D11SamplerForForcedAF(desc, gfx), D3D11ForcedAFSamplerDecision::BorderAddress);

    desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.Filter = D3D11_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR;
    EXPECT_EQ(ClassifyD3D11SamplerForForcedAF(desc, gfx), D3D11ForcedAFSamplerDecision::ReductionFilter);
}

TEST(SamplerOverrideUtilsTest, D3D11ForcedAFTexturePolicyAllowsOnlyMaterialLikeMipmappedTexture2D) {
    D3D11Texture2DForcedAFInfo info = {};
    info.format = DXGI_FORMAT_BC7_UNORM;
    info.viewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    info.width = 1024;
    info.height = 1024;
    info.mipLevels = 0;
    info.mostDetailedMip = 0;
    info.viewMipLevels = UINT_MAX;
    info.arraySize = 1;
    info.sampleCount = 1;
    info.bindFlags = D3D11_BIND_SHADER_RESOURCE;
    info.formatSupported = true;

    EXPECT_TRUE(D3D11Texture2DAllowsForcedAF(info));

    info.bindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::RenderTargetResource);

    info.bindFlags = D3D11_BIND_SHADER_RESOURCE;
    info.viewMipLevels = 1;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::SingleVisibleMip);

    info.viewMipLevels = UINT_MAX;
    info.format = DXGI_FORMAT_R8G8B8A8_UINT;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::ProblematicFormat);

    info.format = DXGI_FORMAT_BC7_UNORM;
    info.viewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::UnsupportedViewDimension);
}

}  // namespace
