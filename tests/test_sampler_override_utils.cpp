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

TEST(SamplerOverrideUtilsTest, D3D10SafeAndAggressiveEligibilityProtectSpecialSamplers) {
    GraphicsConfig gfx;
    D3D10_SAMPLER_DESC desc = {};
    desc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D10_TEXTURE_ADDRESS_WRAP;
    desc.AddressV = D3D10_TEXTURE_ADDRESS_WRAP;
    desc.AddressW = D3D10_TEXTURE_ADDRESS_WRAP;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = D3D10_FLOAT32_MAX;

    EXPECT_TRUE(IsD3D10SamplerOverrideEligible(desc, gfx));
    desc.AddressU = D3D10_TEXTURE_ADDRESS_CLAMP;
    EXPECT_TRUE(IsD3D10SamplerOverrideEligible(desc, gfx));

    desc.Filter = D3D10_FILTER_MIN_MAG_MIP_POINT;
    EXPECT_FALSE(IsD3D10SamplerOverrideEligible(desc, gfx));

    gfx.samplerOverrideMode = "aggressive";
    EXPECT_TRUE(IsD3D10SamplerOverrideEligible(desc, gfx));
    desc.AddressU = D3D10_TEXTURE_ADDRESS_BORDER;
    EXPECT_FALSE(IsD3D10SamplerOverrideEligible(desc, gfx));
    desc.AddressU = D3D10_TEXTURE_ADDRESS_WRAP;
    desc.Filter = D3D10_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    EXPECT_FALSE(IsD3D10SamplerOverrideEligible(desc, gfx));
}

TEST(SamplerOverrideUtilsTest, D3D10CreationTimeForcedAFAllowsClampButProtectsSpecialSamplers) {
    GraphicsConfig gfx;
    gfx.anisotropicFiltering = "16x";
    D3D10_SAMPLER_DESC desc = {};
    desc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D10_TEXTURE_ADDRESS_WRAP;
    desc.AddressV = D3D10_TEXTURE_ADDRESS_MIRROR;
    desc.AddressW = D3D10_TEXTURE_ADDRESS_WRAP;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = D3D10_FLOAT32_MAX;

    EXPECT_TRUE(D3D10SamplerAllowsCreationTimeForcedAF(desc, gfx));
    desc.AddressU = D3D10_TEXTURE_ADDRESS_CLAMP;
    EXPECT_TRUE(D3D10SamplerAllowsCreationTimeForcedAF(desc, gfx));
    desc.AddressU = D3D10_TEXTURE_ADDRESS_BORDER;
    EXPECT_FALSE(D3D10SamplerAllowsCreationTimeForcedAF(desc, gfx));
}

TEST(SamplerOverrideUtilsTest, D3D9ForcedAFRequiresMipmappedFilterableTexturesAndClampsToCaps) {
    GraphicsConfig gfx;
    gfx.anisotropicFiltering = "16x";
    D3D9SamplerForcedAFInfo info = {};
    info.textureBound = true;
    info.textureMipLevels = 8;
    info.minFilter = D3DTEXF_LINEAR;
    info.magFilter = D3DTEXF_LINEAR;
    info.mipFilter = D3DTEXF_LINEAR;
    info.addressU = D3DTADDRESS_CLAMP;
    info.addressV = D3DTADDRESS_WRAP;
    info.addressW = D3DTADDRESS_BORDER;
    info.deviceMaxAnisotropy = 8;

    EXPECT_EQ(ClassifyD3D9SamplerForForcedAF(info, gfx), D3D9ForcedAFDecision::Allow);
    EXPECT_EQ(ResolveD3D9ForcedAnisotropy(info, gfx), 8u);

    info.usesAddressW = true;
    EXPECT_EQ(ClassifyD3D9SamplerForForcedAF(info, gfx), D3D9ForcedAFDecision::BorderAddress);
    info.usesAddressW = false;
    info.textureMipLevels = 1;
    EXPECT_EQ(ClassifyD3D9SamplerForForcedAF(info, gfx), D3D9ForcedAFDecision::SingleMipTexture);
    info.textureMipLevels = 8;
    info.minFilter = D3DTEXF_POINT;
    EXPECT_EQ(ClassifyD3D9SamplerForForcedAF(info, gfx), D3D9ForcedAFDecision::PointMinMag);
    gfx.samplerOverrideMode = "aggressive";
    EXPECT_EQ(ClassifyD3D9SamplerForForcedAF(info, gfx), D3D9ForcedAFDecision::Allow);
}

TEST(SamplerOverrideUtilsTest, LegacyD3DPolicyModelsDistinctD3D7MagFilterValues) {
    GraphicsConfig gfx;
    gfx.anisotropicFiltering = "16x";
    LegacyD3DSamplerTraits d3d7Traits = {};
    d3d7Traits.anisotropicMag = 5;
    d3d7Traits.mipNone = 1;
    d3d7Traits.mipPoint = 2;
    d3d7Traits.mipLinear = 3;
    LegacyD3DSamplerForcedAFInfo info = {};
    info.minFilter = d3d7Traits.linearMin;
    info.magFilter = d3d7Traits.linearMag;
    info.mipFilter = d3d7Traits.mipLinear;
    info.addressU = 3;
    info.addressV = 1;
    info.addressW = 1;
    info.deviceMaxAnisotropy = 16;

    EXPECT_EQ(ClassifyLegacyD3DSamplerForForcedAF(info, d3d7Traits, gfx), LegacyD3DForcedAFDecision::Allow);
    info.magFilter = d3d7Traits.anisotropicMag;
    EXPECT_EQ(ClassifyLegacyD3DSamplerForForcedAF(info, d3d7Traits, gfx), LegacyD3DForcedAFDecision::Allow);
    info.addressU = 4;
    EXPECT_EQ(ClassifyLegacyD3DSamplerForForcedAF(info, d3d7Traits, gfx),
              LegacyD3DForcedAFDecision::BorderAddress);
}

TEST(SamplerOverrideUtilsTest, OpenGLForcedAFUsesAllocatedMipAndMaterialAddressSafety) {
    GraphicsConfig gfx;
    gfx.anisotropicFiltering = "16x";
    OpenGLSamplerForcedAFInfo info = {};
    info.extensionSupported = true;
    info.deviceMaxAnisotropy = 8.0f;
    info.minFilter = 0x2703;
    info.magFilter = 0x2601;
    info.wrapS = 0x812F;
    info.wrapT = 0x2901;
    info.wrapR = 0x812D;
    info.usesWrapR = false;

    EXPECT_EQ(ClassifyOpenGLSamplerForForcedAF(info, gfx), OpenGLForcedAFDecision::Allow);
    EXPECT_FLOAT_EQ(ResolveOpenGLForcedAnisotropy(info, gfx), 8.0f);
    info.usesWrapR = true;
    EXPECT_EQ(ClassifyOpenGLSamplerForForcedAF(info, gfx), OpenGLForcedAFDecision::BorderAddress);
    info.usesWrapR = false;
    info.maxLevel = info.baseLevel;
    EXPECT_EQ(ClassifyOpenGLSamplerForForcedAF(info, gfx), OpenGLForcedAFDecision::NoMipRange);
    info.maxLevel = 8;
    info.minFilter = 0x2700;
    EXPECT_EQ(ClassifyOpenGLSamplerForForcedAF(info, gfx), OpenGLForcedAFDecision::PointMinMag);
    gfx.samplerOverrideMode = "aggressive";
    EXPECT_EQ(ClassifyOpenGLSamplerForForcedAF(info, gfx), OpenGLForcedAFDecision::Allow);
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
    EXPECT_TRUE(D3D11ShaderSamplerIsAFCandidate(usage, 5));
    EXPECT_EQ(D3D11ShaderAFSafeSamplerMaskForTextureSlot(usage, 0), (1u << 0) | (1u << 2));
    EXPECT_EQ(D3D11ShaderAFSafeSamplerMaskForTextureSlot(usage, 3), (1u << 2) | (1u << 5));
    EXPECT_EQ(D3D11ShaderAFSafeSamplerMaskForTextureSlot(usage, 8), 0u);
    EXPECT_EQ(D3D11ShaderAFSafeSamplerMaskForAnyTexture(usage), (1u << 0) | (1u << 2) | (1u << 5));
}

TEST(SamplerOverrideUtilsTest, TreatsDerivativeFootprintD3D11SamplesAsAFSafe) {
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
    EXPECT_TRUE(D3D11ShaderSamplerUsesAFSafeSample(usage, 5));
    EXPECT_EQ(CountD3D11ShaderSamplerTextureUses(usage, 5), 2u);

    EXPECT_TRUE(D3D11ShaderSamplerUsesGradientSample(usage, 6));
    EXPECT_FALSE(D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, 6));
    EXPECT_TRUE(D3D11ShaderSamplerUsesAFSafeSample(usage, 6));

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
    EXPECT_EQ(summary.afSafeSamplers, 2u);
    EXPECT_EQ(summary.unsafeExplicitSamplers, 2u);
}

TEST(SamplerOverrideUtilsTest, RejectsMixedImplicitAndLodForForcedAF) {
    const char disassembly[] =
        "    sample r0.xyzw, v0.xyxx, t0.xyzw, s0\n"
        "    sample_l r1.xyzw, v1.xyxx, t1.xyzw, s0, l(0)\n";

    const D3D11ShaderSamplerUsage usage = ParseD3D11ShaderSamplerUsage(disassembly, sizeof(disassembly) - 1);

    EXPECT_TRUE(usage.samplerUsesImplicitSample[0]);
    EXPECT_TRUE(D3D11ShaderSamplerUsesLodSample(usage, 0));
    EXPECT_TRUE(D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, 0));
    EXPECT_FALSE(D3D11ShaderSamplerUsesAFSafeSample(usage, 0));
}

TEST(SamplerOverrideUtilsTest, RejectsLodOnlyForForcedAF) {
    const char disassembly[] = "    sample_l r0.xyzw, v0.xyxx, t0.xyzw, s1, l(0)\n";

    const D3D11ShaderSamplerUsage usage = ParseD3D11ShaderSamplerUsage(disassembly, sizeof(disassembly) - 1);

    EXPECT_TRUE(D3D11ShaderSamplerUsesLodSample(usage, 1));
    EXPECT_FALSE(usage.samplerUsesImplicitSample[1]);
    EXPECT_FALSE(usage.samplerUsesBiasSample[1]);
    EXPECT_TRUE(D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, 1));
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
    desc.MinLOD = D3D11_FLOAT32_MAX;
    EXPECT_EQ(ClassifyD3D11SamplerForForcedAF(desc, gfx), D3D11ForcedAFSamplerDecision::FixedLOD);

    desc.MinLOD = 0.0f;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    EXPECT_EQ(ClassifyD3D11SamplerForForcedAF(desc, gfx), D3D11ForcedAFSamplerDecision::BorderAddress);

    desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.Filter = D3D11_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR;
    EXPECT_EQ(ClassifyD3D11SamplerForForcedAF(desc, gfx), D3D11ForcedAFSamplerDecision::ReductionFilter);

    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    EXPECT_EQ(ClassifyD3D11SamplerForForcedAF(desc, gfx), D3D11ForcedAFSamplerDecision::PointMinMag);

    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    EXPECT_EQ(ClassifyD3D11SamplerForForcedAF(desc, gfx), D3D11ForcedAFSamplerDecision::Allow);

    gfx.samplerOverrideMode = "aggressive";
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    EXPECT_EQ(ClassifyD3D11SamplerForForcedAF(desc, gfx), D3D11ForcedAFSamplerDecision::Allow);
    desc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    EXPECT_EQ(ClassifyD3D11SamplerForForcedAF(desc, gfx), D3D11ForcedAFSamplerDecision::BorderAddress);
}

TEST(SamplerOverrideUtilsTest, D3D11ForcedAFTexturePolicyAllowsFilterableMipmappedMaterialViews) {
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
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::Allow);

    const DXGI_FORMAT materialFormats[] = {
        DXGI_FORMAT_BC1_UNORM,  DXGI_FORMAT_BC3_UNORM_SRGB, DXGI_FORMAT_BC4_UNORM,
        DXGI_FORMAT_BC5_SNORM, DXGI_FORMAT_BC6H_UF16,      DXGI_FORMAT_BC7_UNORM,
        DXGI_FORMAT_R8_UNORM,  DXGI_FORMAT_R8G8_SNORM,     DXGI_FORMAT_R16G16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };
    for (DXGI_FORMAT format : materialFormats) {
        info.format = format;
        EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::Allow)
            << static_cast<int>(format);
    }

    info.format = DXGI_FORMAT_BC5_UNORM;
    info.viewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    info.arraySize = 32;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::Allow);

    info.viewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    info.arraySize = 6;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::Allow);

    info.viewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    info.arraySize = 12;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::Allow);
}

TEST(SamplerOverrideUtilsTest, D3D11ForcedAFTexturePolicyRejectsUnsafeOrUnfilterableViews) {
    D3D11Texture2DForcedAFInfo info = {};
    info.format = DXGI_FORMAT_BC3_UNORM;
    info.viewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    info.width = 1024;
    info.height = 1024;
    info.mipLevels = 11;
    info.viewMipLevels = UINT_MAX;
    info.sampleCount = 1;
    info.bindFlags = D3D11_BIND_SHADER_RESOURCE;
    info.formatSupported = true;

    info.viewMipLevels = 1;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::SingleVisibleMip);

    info.viewMipLevels = UINT_MAX;
    info.format = DXGI_FORMAT_R8G8B8A8_UINT;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::ProblematicFormat);

    info.format = DXGI_FORMAT_BC3_TYPELESS;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::ProblematicFormat);

    info.format = DXGI_FORMAT_BC3_UNORM;
    info.bindFlags |= D3D11_BIND_DEPTH_STENCIL;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::DepthStencilResource);

    info.bindFlags = D3D11_BIND_SHADER_RESOURCE;
    info.viewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::Multisampled);

    info.viewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::UnsupportedViewDimension);

    info.viewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    info.formatSupported = false;
    EXPECT_EQ(ClassifyD3D11Texture2DForForcedAF(info), D3D11ForcedAFResourceDecision::UnsupportedFormat);
}

}  // namespace
