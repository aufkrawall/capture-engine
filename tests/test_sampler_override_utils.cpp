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

}  // namespace
