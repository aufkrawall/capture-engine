#include <gtest/gtest.h>

#include "hook/overrides/sampler_override/shader_af_plan.h"
#include "hook/overrides/sampler_override/translation_scope.h"
#include "hook/overrides/sampler_override/creation_policy.h"

#include <thread>
#include <wrl/client.h>

using namespace ce::sampler_override;

TEST(D3D11ShaderAFPlanTest, CompactTextureTraversalCoversEverySlotExactlyOnce) {
    D3D11AFTextureMask textures;
    EXPECT_EQ(textures.First(), UINT_MAX);
    EXPECT_EQ(textures.Last(), UINT_MAX);
    EXPECT_EQ(textures.TakeFirst(), UINT_MAX);
    for (UINT slot = 0; slot < 128; ++slot) {
        textures.Add(slot);
        textures.Add(slot);
    }
    textures.Add(128);
    EXPECT_EQ(textures.Count(), 128u);
    EXPECT_EQ(textures.First(), 0u);
    EXPECT_EQ(textures.Last(), 127u);
    for (UINT slot = 0; slot < 128; ++slot) {
        EXPECT_EQ(textures.TakeFirst(), slot);
        EXPECT_EQ(textures.Count(), 127u - slot);
    }
    EXPECT_EQ(textures.TakeFirst(), UINT_MAX);
}

TEST(D3D11ShaderAFPlanTest, MatchesExistingSafetyPolicyForEverySampleKindCombination) {
    for (UINT kinds = 0; kinds < 128; ++kinds) {
        D3D11ShaderSamplerUsage usage;
        for (UINT slot = 0; slot < 16; ++slot) {
            usage.samplerUsesImplicitSample[slot] = (kinds & 1) != 0;
            usage.samplerUsesBiasSample[slot] = (kinds & 2) != 0;
            usage.samplerUsesLodSample[slot] = (kinds & 4) != 0;
            usage.samplerUsesGradientSample[slot] = (kinds & 8) != 0;
            usage.samplerUsesComparisonSample[slot] = (kinds & 16) != 0;
            usage.samplerUsesOtherExplicitSample[slot] = (kinds & 32) != 0;
            usage.samplerUsesExplicitSample[slot] = (kinds & 64) != 0;
            usage.samplerTextures[slot][slot] = true;
            usage.samplerTextures[slot][127 - slot] = true;
        }
        const auto plan = BuildD3D11ShaderAFPlan(usage, true);
        EXPECT_EQ(plan.candidateSamplerMask, D3D11ShaderAFSafeSamplerMaskForAnyTexture(usage)) << kinds;
        for (UINT texture = 0; texture < 128; ++texture) {
            EXPECT_EQ(plan.textureSamplerMasks[texture], D3D11ShaderAFSafeSamplerMaskForTextureSlot(usage, texture));
        }
        for (UINT slot = 0; slot < 16; ++slot) {
            const auto& sampler = plan.samplers[slot];
            EXPECT_EQ(sampler.SafeSample(), D3D11ShaderSamplerUsesAFSafeSample(usage, slot));
            UINT first = UINT_MAX;
            UINT last = UINT_MAX;
            EXPECT_EQ(sampler.textures.Count(), CountD3D11ShaderSamplerTextureUses(usage, slot, &first, &last));
            EXPECT_EQ(sampler.textures.First(), first);
            EXPECT_EQ(sampler.textures.Last(), last);
        }
    }
    EXPECT_LT(sizeof(D3D11AFSamplerPlan), sizeof(D3D11ShaderSamplerUsage) / 32);
    EXPECT_LT(sizeof(D3D11ShaderAFPlan), sizeof(D3D11ShaderSamplerUsage));
}

TEST(D3D11ShaderAFPlanTest, ResourceChangesOnlyInvalidateSafeDependentTrackedSamplers) {
    D3D11ShaderSamplerUsage usage;
    usage.samplerUsesImplicitSample[2] = true;
    usage.samplerTextures[2][5] = true;
    usage.samplerTextures[2][127] = true;
    usage.samplerUsesGradientSample[7] = true;
    usage.samplerTextures[7][5] = true;
    usage.samplerUsesImplicitSample[9] = true;
    usage.samplerUsesLodSample[9] = true;
    usage.samplerTextures[9][5] = true;
    const auto plan = BuildD3D11ShaderAFPlan(usage, true);
    EXPECT_EQ(plan.ResourceDirtyMask(5, 0xFFFF), (1u << 2) | (1u << 7));
    EXPECT_EQ(plan.ResourceDirtyMask(5, 1u << 2), 1u << 2);
    EXPECT_EQ(plan.ResourceDirtyMask(127, 0xFFFF), 1u << 2);
    EXPECT_EQ(plan.ResourceDirtyMask(6, 0xFFFF), 0u);
    EXPECT_EQ(plan.ResourceDirtyMask(128, 0xFFFF), 0u);
}

TEST(D3D11ShaderAFPlanTest, MissingMetadataPreservesConservativeResourceInvalidation) {
    D3D11ShaderSamplerUsage usage;
    usage.samplerUsesImplicitSample[0] = true;
    usage.samplerTextures[0][0] = true;
    const auto plan = BuildD3D11ShaderAFPlan(usage, false, true);
    EXPECT_FALSE(plan.available);
    EXPECT_TRUE(plan.disassembleFailed);
    EXPECT_EQ(plan.candidateSamplerMask, 0u);
    EXPECT_EQ(plan.ResourceDirtyMask(127, 0x1234), 0x1234u);
    EXPECT_EQ(plan.samplers[0].textures.Count(), 0u);
}

TEST(D3D11ShaderAFPlanTest, ShaderTransitionsRestorePreviouslyOverriddenSlots) {
    D3D11ShaderSamplerUsage usage;
    usage.samplerUsesImplicitSample[2] = true;
    usage.samplerTextures[2][5] = true;
    auto plan = BuildD3D11ShaderAFPlan(usage, true);
    EXPECT_EQ(plan.ShaderDirtyMask(0xFFFF, 1u << 7), (1u << 2) | (1u << 7));
    EXPECT_EQ(plan.ShaderDirtyMask(1u << 7, 1u << 7), 1u << 7);
    usage.samplerUsesComparisonSample[2] = true;
    plan = BuildD3D11ShaderAFPlan(usage, true);
    EXPECT_EQ(plan.ShaderDirtyMask(0xFFFF, 1u << 2), 1u << 2);
    plan = BuildD3D11ShaderAFPlan(usage, false, true);
    EXPECT_EQ(plan.ShaderDirtyMask(0xFFFF, 1u << 2), 1u << 2);
    EXPECT_EQ(plan.ShaderDirtyMask(0xFFFF, 0), 0u);
}

TEST(D3D11ShaderAFPlanTest, UnrelatedGraphicsOverridesDoNotEnableSamplerReplacement) {
    GraphicsConfig gfx;
    gfx.vsyncMode = "fifo";
    gfx.cpuPrerenderLimit = 1;
    gfx.backbufferCount = 3;
    gfx.frameLatency = 1;
    gfx.msaaSamples = "4x";
    gfx.sharpenMode = "rcas";
    gfx.postProcessDisplayGamma = "2.2";
    gfx.parsed.srPreset = 5;
    gfx.parsed.fgMode = 1;
    gfx.nvLodSpreadFix = true;
    gfx.displayGamma = 0;
    gfx.internalAnisotropicFiltering = 16;
    EXPECT_FALSE(HasD3D11SamplerOverride(gfx));
    for (const char* af : {"off", "2x", "16x"}) {
        gfx.anisotropicFiltering = af;
        EXPECT_TRUE(HasD3D11SamplerOverride(gfx));
    }
    gfx.anisotropicFiltering = "default";
    gfx.mipMapping = "trilinear";
    EXPECT_TRUE(HasD3D11SamplerOverride(gfx));
    gfx.mipMapping = "default";
    gfx.mipBias = "-0.5";
    EXPECT_TRUE(HasD3D11SamplerOverride(gfx));
    gfx.mipBias = "default";
    gfx.mipBiasMin = "-2";
    EXPECT_TRUE(HasD3D11SamplerOverride(gfx));
    gfx.mipBiasMin = "default";
    gfx.mipBiasMax = "0";
    EXPECT_TRUE(HasD3D11SamplerOverride(gfx));
    gfx.mipBiasMax = "default";
    gfx.forceMipBiasClamp = true;
    EXPECT_TRUE(HasD3D11SamplerOverride(gfx));
    gfx.forceMipBiasClamp = false;
    gfx.sgssaa = true;
    EXPECT_TRUE(HasD3D11SamplerOverride(gfx));
    gfx.disableAutoMipBias = true;
    EXPECT_FALSE(HasD3D11SamplerOverride(gfx));
}

TEST(D3D11ShaderAFPlanTest, TranslationOwnershipIsNestedAndThreadLocal) {
    EXPECT_FALSE(D3D11OwnsTranslatedSampler());
    {
        D3D11SamplerCreationScope frontend;
        EXPECT_TRUE(D3D11OwnsTranslatedSampler());
        {
            D3D11SamplerCreationScope nestedWrapper;
            EXPECT_TRUE(D3D11OwnsTranslatedSampler());
        }
        EXPECT_TRUE(D3D11OwnsTranslatedSampler());
        bool otherThreadOwned = true;
        std::thread other([&] {
            otherThreadOwned = D3D11OwnsTranslatedSampler();
            D3D11SamplerCreationScope ownFrontend;
            EXPECT_TRUE(D3D11OwnsTranslatedSampler());
        });
        other.join();
        EXPECT_FALSE(otherThreadOwned);
        EXPECT_TRUE(D3D11OwnsTranslatedSampler());
    }
    EXPECT_FALSE(D3D11OwnsTranslatedSampler());
}

TEST(D3D11ShaderAFPlanTest, CreationPolicyTagKeepsBindTimeAFIndependentFromBiasAndMipOverrides) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                          D3D11_SDK_VERSION, &device, nullptr, nullptr)));
    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MaxAnisotropy = 1;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    desc.MipLODBias = -1.25f;  // creation already added -0.5 to the application's -0.75
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    ASSERT_TRUE(SUCCEEDED(device->CreateSamplerState(&desc, &sampler)));
    GraphicsConfig gfx;
    gfx.anisotropicFiltering = "16x";
    gfx.mipBias = "-0.5";
    gfx.mipBiasMode = "offset";
    gfx.mipMapping = "bilinear";
    EXPECT_FALSE(HasCurrentD3D11SamplerCreationPolicy(sampler.Get(), gfx));
    ASSERT_TRUE(SUCCEEDED(RecordD3D11SamplerCreationPolicy(sampler.Get(), gfx)));
    EXPECT_TRUE(HasCurrentD3D11SamplerCreationPolicy(sampler.Get(), gfx));
    EXPECT_FALSE(ApplyD3D11BindTimeAF(desc, gfx, false));
    EXPECT_EQ(desc.Filter, D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT);
    EXPECT_TRUE(ApplyD3D11BindTimeAF(desc, gfx, true));
    EXPECT_EQ(desc.Filter, D3D11_FILTER_ANISOTROPIC);
    EXPECT_EQ(desc.MaxAnisotropy, 16u);
    EXPECT_FLOAT_EQ(desc.MipLODBias, -1.25f);
    EXPECT_FALSE(ApplyD3D11BindTimeAF(desc, gfx, true));
    gfx.mipBias = "-1.0";
    EXPECT_FALSE(HasCurrentD3D11SamplerCreationPolicy(sampler.Get(), gfx));
    EXPECT_EQ(RecordD3D11SamplerCreationPolicy(nullptr, gfx), E_POINTER);
}
