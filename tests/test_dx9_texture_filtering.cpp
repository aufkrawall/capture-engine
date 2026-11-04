// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <gtest/gtest.h>

#include "hook/d3d9/dx9_sampler_config_policy.h"

namespace {
namespace sampler = ce::dx9_sampler_state;

sampler::SamplerState Material(UINT levels, DWORD firstMip) {
    sampler::SamplerState state;
    state.textureBound = true;
    state.textureMipLevels = levels;
    state.textureSupportsAnisotropy = true;
    state.textureSupportsMagAnisotropy = true;
    state.logical[0] = D3DTADDRESS_WRAP;
    state.logical[1] = D3DTADDRESS_WRAP;
    state.logical[2] = D3DTADDRESS_WRAP;
    state.logical[3] = D3DTEXF_LINEAR;
    state.logical[4] = D3DTEXF_LINEAR;
    state.logical[5] = D3DTEXF_LINEAR;
    state.logical[7] = firstMip;
    return state;
}

sampler::ParsedSamplerConfig ForcedAf() {
    GraphicsConfig config;
    config.anisotropicFiltering = "16x";
    return sampler::ParseSamplerConfig(config);
}
}  // namespace

TEST(Dx9TextureFilteringTest, BindingAcrossTheVisibleMipBoundaryRequiresReconciliationInBothDirections) {
    const auto twoVisible = Material(3, 1);
    const auto oneVisible = Material(2, 1);
    EXPECT_FALSE(sampler::ClassifyStageTexture(twoVisible) == sampler::ClassifyStageTexture(oneVisible));
    EXPECT_FALSE(sampler::ClassifyStageTexture(oneVisible) == sampler::ClassifyStageTexture(twoVisible));
    const auto config = ForcedAf();
    EXPECT_EQ(sampler::BuildDesiredState(twoVisible, 16, config, nullptr)[4], D3DTEXF_ANISOTROPIC);
    EXPECT_EQ(sampler::BuildDesiredState(oneVisible, 16, config, nullptr)[4], D3DTEXF_LINEAR);
}

TEST(Dx9TextureFilteringTest, EqualTextureClassificationsAlwaysPreserveTheFilteringDecision) {
    auto config = ForcedAf();
    config.mipMode = ce::mip_mapping::Mode::Trilinear;
    for (DWORD firstMip : {0u, 1u, 2u, 8u, UINT32_MAX}) {
        for (UINT before = 0; before <= 12; ++before) {
            for (UINT after = 0; after <= 12; ++after) {
                const auto oldTexture = Material(before, firstMip);
                const auto newTexture = Material(after, firstMip);
                if (sampler::ClassifyStageTexture(oldTexture) == sampler::ClassifyStageTexture(newTexture)) {
                    EXPECT_EQ(sampler::BuildDesiredState(oldTexture, 16, config, nullptr),
                              sampler::BuildDesiredState(newTexture, 16, config, nullptr))
                        << "first mip=" << firstMip << " levels=" << before << " -> " << after;
                }
            }
        }
    }
}

TEST(Dx9TextureFilteringTest, TexturesWithinTheSameVisibleMipClassKeepTheShortcut) {
    EXPECT_TRUE(sampler::ClassifyStageTexture(Material(4, 1)) == sampler::ClassifyStageTexture(Material(12, 1)));
    EXPECT_TRUE(sampler::ClassifyStageTexture(Material(1, 1)) == sampler::ClassifyStageTexture(Material(2, 1)));
    EXPECT_EQ(sampler::VisibleMipLevels(Material(2, UINT32_MAX)), 0u);
}
