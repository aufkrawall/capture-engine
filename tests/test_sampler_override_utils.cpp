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

    const D3D11ShaderSamplerUsage usage = ParseD3D11ShaderSamplerUsage(disassembly, sizeof(disassembly) - 1);

    EXPECT_TRUE(usage.sawSampleInstruction);
    EXPECT_FALSE(usage.sawUnsupportedRegister);
    EXPECT_EQ(CountD3D11ShaderSamplerTextureUses(usage, 0), 2u);
    EXPECT_TRUE(D3D11ShaderSamplerUsesTexture(usage, 0, 0));
    EXPECT_TRUE(D3D11ShaderSamplerUsesTexture(usage, 0, 7));
    EXPECT_TRUE(D3D11ShaderSamplerUsesExplicitSample(usage, 0));
    EXPECT_TRUE(D3D11ShaderSamplerUsesLodSample(usage, 0));
    EXPECT_FALSE(D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, 0));
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

    const D3D11ShaderSamplerUsage usage = ParseD3D11ShaderSamplerUsage(disassembly, sizeof(disassembly) - 1);

    EXPECT_TRUE(D3D11ShaderSamplerUsesOnlyImplicitSample(usage, 3));
    EXPECT_TRUE(D3D11ShaderSamplerUsesAFSafeSample(usage, 3));
    EXPECT_FALSE(D3D11ShaderSamplerUsesExplicitSample(usage, 3));
    EXPECT_EQ(CountD3D11ShaderSamplerTextureUses(usage, 3), 2u);
}

TEST(SamplerOverrideUtilsTest, BuildsD3D11ShaderSamplerDirtyMasksFromTextureSlots) {
    const char disassembly[] =
        "    sample r0.xyzw, v0.xyxx, t0.xyzw, s0\n"
        "    sample r1.xyzw, v1.xyxx, t0.xyzw, s2\n"
        "    sample r2.xyzw, v2.xyxx, t3.xyzw, s2\n"
        "    sample_l r3.xyzw, v3.xyxx, t0.xyzw, s4, l(0)\n"
        "    sample_d r4.xyzw, v4.xyxx, t3.xyzw, s5, r1.xyzw, r2.xyzw\n";

    const D3D11ShaderSamplerUsage usage = ParseD3D11ShaderSamplerUsage(disassembly, sizeof(disassembly) - 1);

    EXPECT_EQ(D3D11ShaderSamplerMaskForTextureSlot(usage, 0), (1u << 0) | (1u << 2) | (1u << 4));
    EXPECT_EQ(D3D11ShaderSamplerMaskForTextureSlot(usage, 3), (1u << 2) | (1u << 5));
    EXPECT_EQ(D3D11ShaderSamplerMaskForTextureSlot(usage, 8), 0u);
    EXPECT_EQ(D3D11ShaderSamplerMaskForAnyTexture(usage), (1u << 0) | (1u << 2) | (1u << 4) | (1u << 5));

    EXPECT_TRUE(D3D11ShaderSamplerIsAFCandidate(usage, 0));
    EXPECT_TRUE(D3D11ShaderSamplerIsAFCandidate(usage, 2));
    EXPECT_FALSE(D3D11ShaderSamplerIsAFCandidate(usage, 4));
    EXPECT_FALSE(D3D11ShaderSamplerIsAFCandidate(usage, 5));
    EXPECT_EQ(D3D11ShaderAFSafeSamplerMaskForTextureSlot(usage, 0), (1u << 0) | (1u << 2));
    EXPECT_EQ(D3D11ShaderAFSafeSamplerMaskForTextureSlot(usage, 3), (1u << 2));
    EXPECT_EQ(D3D11ShaderAFSafeSamplerMaskForTextureSlot(usage, 8), 0u);
    EXPECT_EQ(D3D11ShaderAFSafeSamplerMaskForAnyTexture(usage), (1u << 0) | (1u << 2));
}

TEST(SamplerOverrideUtilsTest, TreatsOnlyImplicitD3D11SamplesAsAFSafe) {
    const char disassembly[] =
        "    sample_b r0.xyzw, v0.xyxx, t4.xyzw, s5, l(0)\n"
        "    sample_b_indexable(texture2d)(float,float,float,float) r1.xyzw, v1.xyxx, t5.xyzw, s5, r0.x\n"
        "    sample_d_indexable(texture2d)(float,float,float,float) r2.xyzw, v2.xyxx, t6.xyzw, s6, r1.xyzw, r2.xyzw\n"
        "    sample_l_indexable(texture2d)(float,float,float,float) r3.xyzw, v3.xyxx, t7.xyzw, s7, l(0)\n"
        "    sample_c_lz_indexable(texture2d)(float,float,float,float) r4.x, v4.xyxx, t8.xxxx, s8, r4.x\n";

    const D3D11ShaderSamplerUsage usage = ParseD3D11ShaderSamplerUsage(disassembly, sizeof(disassembly) - 1);

    EXPECT_TRUE(D3D11ShaderSamplerUsesExplicitSample(usage, 5));
    EXPECT_TRUE(D3D11ShaderSamplerUsesBiasSample(usage, 5));
    EXPECT_FALSE(D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, 5));
    EXPECT_FALSE(D3D11ShaderSamplerUsesAFSafeSample(usage, 5));
    EXPECT_EQ(CountD3D11ShaderSamplerTextureUses(usage, 5), 2u);

    EXPECT_TRUE(D3D11ShaderSamplerUsesGradientSample(usage, 6));
    EXPECT_TRUE(D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, 6));
    EXPECT_FALSE(D3D11ShaderSamplerUsesAFSafeSample(usage, 6));

    EXPECT_TRUE(D3D11ShaderSamplerUsesLodSample(usage, 7));
    EXPECT_FALSE(D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, 7));
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
    EXPECT_EQ(summary.afSafeSamplers, 0u);
    EXPECT_EQ(summary.unsafeExplicitSamplers, 2u);
}

TEST(SamplerOverrideUtilsTest, RejectsMixedImplicitAndLodForForcedAF) {
    const char disassembly[] =
        "    sample r0.xyzw, v0.xyxx, t0.xyzw, s0\n"
        "    sample_l r1.xyzw, v1.xyxx, t1.xyzw, s0, l(0)\n";

    const D3D11ShaderSamplerUsage usage = ParseD3D11ShaderSamplerUsage(disassembly, sizeof(disassembly) - 1);

    EXPECT_TRUE(usage.samplerUsesImplicitSample[0]);
    EXPECT_TRUE(D3D11ShaderSamplerUsesLodSample(usage, 0));
    EXPECT_FALSE(D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, 0));
    EXPECT_FALSE(D3D11ShaderSamplerUsesAFSafeSample(usage, 0));
}

TEST(SamplerOverrideUtilsTest, RejectsLodOnlyForForcedAF) {
    const char disassembly[] = "    sample_l r0.xyzw, v0.xyxx, t0.xyzw, s1, l(0)\n";

    const D3D11ShaderSamplerUsage usage = ParseD3D11ShaderSamplerUsage(disassembly, sizeof(disassembly) - 1);

    EXPECT_TRUE(D3D11ShaderSamplerUsesLodSample(usage, 1));
    EXPECT_FALSE(usage.samplerUsesImplicitSample[1]);
    EXPECT_FALSE(usage.samplerUsesBiasSample[1]);
    EXPECT_FALSE(D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, 1));
    EXPECT_FALSE(D3D11ShaderSamplerUsesAFSafeSample(usage, 1));
}

TEST(SamplerOverrideUtilsTest, ParsesD3D11ShaderSamplerUsageCaseInsensitivelyAndFlagsUnsupportedRegisters) {
    const char disassembly[] =
        "SAMPLE r0.xyzw, v0.xyxx, t1.xyzw, s3\n"
        "sample r1.xyzw, v1.xyxx, t130.xyzw, s16\n";

    const D3D11ShaderSamplerUsage usage = ParseD3D11ShaderSamplerUsage(disassembly, sizeof(disassembly) - 1);

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
    info.format = DXGI_FORMAT_BC7_UNORM_SRGB;
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

    info.format = DXGI_FORMAT_BC1_UNORM;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::Allow);
    EXPECT_TRUE(IsHighConfidenceColorD3D11AFFormat(info.format));
    EXPECT_TRUE(D3D11Texture2DMayNeedForcedAFMutationTracking(info));

    info.format = DXGI_FORMAT_BC3_UNORM;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::Allow);
    EXPECT_TRUE(IsHighConfidenceColorD3D11AFFormat(info.format));
    EXPECT_TRUE(D3D11Texture2DMayNeedForcedAFMutationTracking(info));

    info.format = DXGI_FORMAT_BC2_UNORM;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::Allow);
    EXPECT_TRUE(IsHighConfidenceColorD3D11AFFormat(info.format));
    EXPECT_TRUE(D3D11Texture2DMayNeedForcedAFMutationTracking(info));

    info.format = DXGI_FORMAT_BC7_UNORM;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::Allow);
    EXPECT_TRUE(IsHighConfidenceColorD3D11AFFormat(info.format));
    EXPECT_TRUE(D3D11Texture2DMayNeedForcedAFMutationTracking(info));

    info.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::Allow);
    EXPECT_TRUE(IsHighConfidenceColorD3D11AFFormat(info.format));
    EXPECT_TRUE(D3D11Texture2DMayNeedForcedAFMutationTracking(info));

    info.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::Allow);
    EXPECT_TRUE(IsHighConfidenceColorD3D11AFFormat(info.format));
    EXPECT_TRUE(D3D11Texture2DMayNeedForcedAFMutationTracking(info));

    info.format = DXGI_FORMAT_BC3_UNORM_SRGB;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::Allow);
    EXPECT_TRUE(IsHighConfidenceColorD3D11AFFormat(info.format));
    EXPECT_TRUE(D3D11Texture2DMayNeedForcedAFMutationTracking(info));

    info.format = DXGI_FORMAT_BC3_TYPELESS;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::ProblematicFormat);
    EXPECT_FALSE(D3D11Texture2DMayNeedForcedAFMutationTracking(info));

    info.format = DXGI_FORMAT_BC5_UNORM;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::ProblematicFormat);
    EXPECT_FALSE(D3D11Texture2DMayNeedForcedAFMutationTracking(info));

    info.format = DXGI_FORMAT_BC4_UNORM;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::ProblematicFormat);
    EXPECT_FALSE(D3D11Texture2DMayNeedForcedAFMutationTracking(info));

    info.format = DXGI_FORMAT_BC3_UNORM_SRGB;
    info.bindFlags = 0;
    EXPECT_FALSE(D3D11Texture2DMayNeedForcedAFMutationTracking(info));
    info.bindFlags = D3D11_BIND_SHADER_RESOURCE;

    EXPECT_STREQ(D3D11ForcedAFResourceDecisionName(D3D11ForcedAFResourceDecision::NonColorFormat), "non-color-format");

    info.format = DXGI_FORMAT_BC7_UNORM_SRGB;
    info.viewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::UnsupportedViewDimension);
}

TEST(SamplerOverrideUtilsTest, HoldsD3D11ForcedAFViewWarmupPerResourceDuringStreamingBursts) {
    constexpr UINT currentDraw = 500000;
    constexpr UINT firstSeenDraw = 300000;
    constexpr UINT requiredObservations = 4;
    constexpr UINT requiredAgeDraws = 120000;
    constexpr UINT requiredStreamingAgeDraws = 300000;

    EXPECT_EQ(ResolveD3D11ForcedAFViewWarmupDecision(currentDraw, firstSeenDraw, 3, requiredObservations,
                                                     requiredAgeDraws, requiredStreamingAgeDraws),
              D3D11ForcedAFResourceDecision::PendingStableObservation);
    EXPECT_EQ(ResolveD3D11ForcedAFViewWarmupDecision(currentDraw, 450000, requiredObservations, requiredObservations,
                                                     requiredAgeDraws, requiredStreamingAgeDraws),
              D3D11ForcedAFResourceDecision::PendingStableObservation);
    EXPECT_EQ(ResolveD3D11ForcedAFViewWarmupDecision(currentDraw, firstSeenDraw, requiredObservations,
                                                     requiredObservations, requiredAgeDraws, requiredStreamingAgeDraws),
              D3D11ForcedAFResourceDecision::PendingStreamingQuiet);
    EXPECT_EQ(ResolveD3D11ForcedAFViewWarmupStartDraw(firstSeenDraw, 420000), 420000u);
    EXPECT_EQ(ResolveD3D11ForcedAFViewWarmupDecision(
                  currentDraw, ResolveD3D11ForcedAFViewWarmupStartDraw(firstSeenDraw, 420000), 32, requiredObservations,
                  requiredAgeDraws, requiredStreamingAgeDraws, 8, 180000),
              D3D11ForcedAFResourceDecision::PendingStableObservation);
    EXPECT_EQ(ResolveD3D11ForcedAFViewWarmupDecision(currentDraw, firstSeenDraw, 7, requiredObservations,
                                                     requiredAgeDraws, requiredStreamingAgeDraws, 8, 180000),
              D3D11ForcedAFResourceDecision::PendingStreamingQuiet);
    EXPECT_EQ(ResolveD3D11ForcedAFViewWarmupDecision(currentDraw, firstSeenDraw, 8, requiredObservations,
                                                     requiredAgeDraws, requiredStreamingAgeDraws, 8, 240000),
              D3D11ForcedAFResourceDecision::PendingStreamingQuiet);
    EXPECT_EQ(ResolveD3D11ForcedAFViewWarmupDecision(currentDraw, firstSeenDraw, 8, requiredObservations,
                                                     requiredAgeDraws, requiredStreamingAgeDraws, 8, 180000),
              D3D11ForcedAFResourceDecision::Allow);
    EXPECT_EQ(ResolveD3D11ForcedAFViewWarmupDecision(600000, firstSeenDraw, requiredObservations, requiredObservations,
                                                     requiredAgeDraws, requiredStreamingAgeDraws),
              D3D11ForcedAFResourceDecision::Allow);
    EXPECT_STREQ(D3D11ForcedAFResourceDecisionName(D3D11ForcedAFResourceDecision::PendingStreamingQuiet),
                 "pending-streaming-quiet");
}

TEST(SamplerOverrideUtilsTest, RequiresQuietD3D11ForcedAFStreamingWindowForFastPromotion) {
    constexpr UINT currentDraw = 500000;
    constexpr UINT requiredQuietDraws = 120000;

    EXPECT_TRUE(D3D11ForcedAFStreamingWindowIsQuiet(currentDraw, 0, 0, requiredQuietDraws));
    EXPECT_TRUE(D3D11ForcedAFStreamingWindowIsQuiet(currentDraw, 360000, 340000, requiredQuietDraws));
    EXPECT_FALSE(D3D11ForcedAFStreamingWindowIsQuiet(currentDraw, 420000, 0, requiredQuietDraws));
    EXPECT_FALSE(D3D11ForcedAFStreamingWindowIsQuiet(currentDraw, 0, 430000, requiredQuietDraws));
    EXPECT_FALSE(D3D11ForcedAFStreamingWindowIsQuiet(currentDraw, 520000, 0, requiredQuietDraws));
    EXPECT_TRUE(D3D11ForcedAFStreamingWindowIsQuiet(currentDraw, 520000, 530000, 0));
}

TEST(SamplerOverrideUtilsTest, GlobalStreamingGateSuspendsOnlyAllowedD3D11ForcedAFResources) {
    EXPECT_EQ(ApplyD3D11ForcedAFGlobalStreamingGate(D3D11ForcedAFResourceDecision::Allow, true),
              D3D11ForcedAFResourceDecision::Allow);
    EXPECT_EQ(ApplyD3D11ForcedAFGlobalStreamingGate(D3D11ForcedAFResourceDecision::Allow, false),
              D3D11ForcedAFResourceDecision::PendingStreamingQuiet);
    EXPECT_EQ(ApplyD3D11ForcedAFGlobalStreamingGate(D3D11ForcedAFResourceDecision::Allow, false, false),
              D3D11ForcedAFResourceDecision::PendingStreamingQuiet);
    EXPECT_EQ(ApplyD3D11ForcedAFGlobalStreamingGate(D3D11ForcedAFResourceDecision::Allow, false, true),
              D3D11ForcedAFResourceDecision::Allow);
    EXPECT_EQ(ApplyD3D11ForcedAFGlobalStreamingGate(D3D11ForcedAFResourceDecision::PendingStableObservation, false),
              D3D11ForcedAFResourceDecision::PendingStableObservation);
    EXPECT_EQ(ApplyD3D11ForcedAFGlobalStreamingGate(D3D11ForcedAFResourceDecision::NonColorFormat, false),
              D3D11ForcedAFResourceDecision::NonColorFormat);
}

TEST(SamplerOverrideUtilsTest, DefersQuietReopenDirtyUntilCandidateSamplerAppears) {
    bool pendingQuietReopenDirty = false;
    EXPECT_EQ(ResolveD3D11ForcedAFQuietTransitionDirtyMask(true, true, 0, 0, pendingQuietReopenDirty), 0u);
    EXPECT_TRUE(pendingQuietReopenDirty);

    EXPECT_EQ(ResolveD3D11ForcedAFQuietTransitionDirtyMask(true, false, 0, 0, pendingQuietReopenDirty), 0u);
    EXPECT_TRUE(pendingQuietReopenDirty);

    EXPECT_EQ(ResolveD3D11ForcedAFQuietTransitionDirtyMask(true, false, 0x4, 0, pendingQuietReopenDirty), 0x4u);
    EXPECT_FALSE(pendingQuietReopenDirty);

    pendingQuietReopenDirty = true;
    EXPECT_EQ(ResolveD3D11ForcedAFQuietTransitionDirtyMask(false, true, 0x8, 0x2, pendingQuietReopenDirty), 0xAu);
    EXPECT_FALSE(pendingQuietReopenDirty);
}

TEST(SamplerOverrideUtilsTest, RecoversD3D11ForcedAFSamplerRoleAfterMixedResourceUse) {
    D3D11ForcedAFSamplerRoleState pendingStreaming;
    EXPECT_FALSE(
        ObserveD3D11ForcedAFSamplerRole(pendingStreaming, D3D11ForcedAFResourceDecision::PendingStreamingQuiet));
    EXPECT_FALSE(pendingStreaming.sawAllowedResource);
    EXPECT_FALSE(pendingStreaming.sawUnsafeResource);

    D3D11ForcedAFSamplerRoleState allowThenUnsafe;
    EXPECT_TRUE(ObserveD3D11ForcedAFSamplerRole(allowThenUnsafe, D3D11ForcedAFResourceDecision::Allow));
    EXPECT_FALSE(allowThenUnsafe.blockedMixedRole);
    EXPECT_FALSE(ObserveD3D11ForcedAFSamplerRole(allowThenUnsafe, D3D11ForcedAFResourceDecision::ProblematicFormat));
    EXPECT_TRUE(allowThenUnsafe.blockedMixedRole);
    for (UINT i = 1; i < kD3D11ForcedAFSamplerRoleRecoveryObservations; ++i) {
        EXPECT_FALSE(ObserveD3D11ForcedAFSamplerRole(allowThenUnsafe, D3D11ForcedAFResourceDecision::Allow));
        EXPECT_TRUE(allowThenUnsafe.blockedMixedRole);
    }
    EXPECT_TRUE(ObserveD3D11ForcedAFSamplerRole(allowThenUnsafe, D3D11ForcedAFResourceDecision::Allow));
    EXPECT_FALSE(allowThenUnsafe.blockedMixedRole);
    EXPECT_EQ(allowThenUnsafe.recoveryCount, 1u);

    D3D11ForcedAFSamplerRoleState unsafeThenAllow;
    EXPECT_FALSE(ObserveD3D11ForcedAFSamplerRole(unsafeThenAllow, D3D11ForcedAFResourceDecision::NonColorFormat));
    EXPECT_FALSE(unsafeThenAllow.blockedMixedRole);
    for (UINT i = 1; i < kD3D11ForcedAFSamplerRoleRecoveryObservations; ++i) {
        EXPECT_FALSE(ObserveD3D11ForcedAFSamplerRole(unsafeThenAllow, D3D11ForcedAFResourceDecision::Allow));
        EXPECT_TRUE(unsafeThenAllow.blockedMixedRole);
    }
    EXPECT_TRUE(ObserveD3D11ForcedAFSamplerRole(unsafeThenAllow, D3D11ForcedAFResourceDecision::Allow));
    EXPECT_FALSE(unsafeThenAllow.blockedMixedRole);

    D3D11ForcedAFSamplerRoleState independentColorSlot;
    EXPECT_TRUE(ObserveD3D11ForcedAFSamplerRole(independentColorSlot, D3D11ForcedAFResourceDecision::Allow));
    EXPECT_FALSE(independentColorSlot.blockedMixedRole);
}

}  // namespace
