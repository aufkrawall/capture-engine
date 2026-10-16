#include <gtest/gtest.h>

#include "hook/overrides/sampler_override/shader_af_plan.h"

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
