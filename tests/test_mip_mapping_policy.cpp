#include <gtest/gtest.h>

#include "../common/mip_mapping_policy.h"
#include "../hook/vulkan_layer/vulkan_sampler_policy.h"

TEST(MipMappingPolicyTest, ParsesOnlyNormalizedSupportedModes) {
    ce::mip_mapping::Mode mode = ce::mip_mapping::Mode::Default;
    EXPECT_TRUE(ce::mip_mapping::TryParseMode("default", mode));
    EXPECT_EQ(mode, ce::mip_mapping::Mode::Default);
    EXPECT_TRUE(ce::mip_mapping::TryParseMode("nearest", mode));
    EXPECT_EQ(mode, ce::mip_mapping::Mode::Nearest);
    EXPECT_TRUE(ce::mip_mapping::TryParseMode("bilinear", mode));
    EXPECT_EQ(mode, ce::mip_mapping::Mode::Bilinear);
    EXPECT_TRUE(ce::mip_mapping::TryParseMode("trilinear", mode));
    EXPECT_EQ(mode, ce::mip_mapping::Mode::Trilinear);
    EXPECT_FALSE(ce::mip_mapping::TryParseMode("linear", mode));
}

TEST(MipMappingPolicyTest, AppliesDiscreteD3DFilterTriples) {
    int mag = 9;
    int min = 8;
    int mip = 7;
    ce::mip_mapping::ApplyDiscreteFilters(ce::mip_mapping::Mode::Nearest, 1, 2, 3, 4, 5, 6, mag, min, mip);
    EXPECT_EQ(mag, 1);
    EXPECT_EQ(min, 3);
    EXPECT_EQ(mip, 5);

    ce::mip_mapping::ApplyDiscreteFilters(ce::mip_mapping::Mode::Bilinear, 1, 2, 3, 4, 5, 6, mag, min, mip);
    EXPECT_EQ(mag, 2);
    EXPECT_EQ(min, 4);
    EXPECT_EQ(mip, 5);

    ce::mip_mapping::ApplyDiscreteFilters(ce::mip_mapping::Mode::Trilinear, 1, 2, 3, 4, 5, 6, mag, min, mip);
    EXPECT_EQ(mag, 2);
    EXPECT_EQ(min, 4);
    EXPECT_EQ(mip, 6);
}

TEST(MipMappingPolicyTest, OverridesEveryOpenGLMipFilterWithoutEnablingMipmapping) {
    const int mipFilters[] = {ce::mip_mapping::kGLNearestMipmapNearest,
                              ce::mip_mapping::kGLLinearMipmapNearest,
                              ce::mip_mapping::kGLNearestMipmapLinear,
                              ce::mip_mapping::kGLLinearMipmapLinear};
    for (int filter : mipFilters) {
        EXPECT_EQ(ce::mip_mapping::ApplyOpenGLMinFilter(ce::mip_mapping::Mode::Nearest, filter),
                  ce::mip_mapping::kGLNearestMipmapNearest);
        EXPECT_EQ(ce::mip_mapping::ApplyOpenGLMinFilter(ce::mip_mapping::Mode::Bilinear, filter),
                  ce::mip_mapping::kGLLinearMipmapNearest);
        EXPECT_EQ(ce::mip_mapping::ApplyOpenGLMinFilter(ce::mip_mapping::Mode::Trilinear, filter),
                  ce::mip_mapping::kGLLinearMipmapLinear);
    }

    EXPECT_EQ(ce::mip_mapping::ApplyOpenGLMinFilter(ce::mip_mapping::Mode::Trilinear,
                                                    ce::mip_mapping::kGLNearest),
              ce::mip_mapping::kGLNearest);
    EXPECT_EQ(ce::mip_mapping::ApplyOpenGLMagFilter(ce::mip_mapping::Mode::Trilinear,
                                                    ce::mip_mapping::kGLNearest, false),
              ce::mip_mapping::kGLNearest);
    EXPECT_EQ(ce::mip_mapping::ApplyOpenGLMagFilter(ce::mip_mapping::Mode::Bilinear,
                                                    ce::mip_mapping::kGLNearest, true),
              ce::mip_mapping::kGLLinear);
}

TEST(MipMappingPolicyTest, VulkanMipOverrideIsIndependentOfSafeAfEligibility) {
    ce::vulkan_sampler_policy::Input input = {};
    input.mipmapped = true;
    input.standardMinMag = true;
    const auto pointClampDecision = ce::vulkan_sampler_policy::Classify(input);
    EXPECT_TRUE(pointClampDecision.allowMipMapping);
    EXPECT_FALSE(pointClampDecision.allowAnisotropy);
    EXPECT_EQ(pointClampDecision.anisotropyReason,
              ce::vulkan_sampler_policy::RejectReason::SafeModeMaterialPolicy);

    input.linearMinMag = true;
    input.materialAddress = true;
    const auto materialDecision = ce::vulkan_sampler_policy::Classify(input);
    EXPECT_TRUE(materialDecision.allowMipMapping);
    EXPECT_TRUE(materialDecision.allowAnisotropy);

    input.comparison = true;
    const auto comparisonDecision = ce::vulkan_sampler_policy::Classify(input);
    EXPECT_FALSE(comparisonDecision.allowMipMapping);
    EXPECT_EQ(comparisonDecision.mipMappingReason, ce::vulkan_sampler_policy::RejectReason::Comparison);
}
