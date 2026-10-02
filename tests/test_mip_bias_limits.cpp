// mip_bias_min / mip_bias_max: bounds on the application's own sampler mip
// bias. The application keeps deciding which samplers get a bias and how much;
// only a value past a bound is pulled back onto it.
#include "test_config_shared.h"

#include "common/graphics/mip_bias_limits.h"
#include "hook/ddraw/lod_helper.h"
#include "hook/d3d12/dx12_sampler_policy.h"
#include "hook/overrides/mip_bias_range.h"

#include <limits>

namespace {

D3D12_SAMPLER_DESC BiasedMaterialSampler(float bias) {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_SAMPLER_DESC desc = {};
    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = D3D12_FLOAT32_MAX;
    desc.MipLODBias = bias;
    return desc;
}

GraphicsConfig LimitsOnly(const char* minBias, const char* maxBias) {
    GraphicsConfig gfx;
    gfx.anisotropicFiltering = "default";
    gfx.mipMapping = "default";
    gfx.mipBias = "default";
    gfx.mipBiasMin = minBias;
    gfx.mipBiasMax = maxBias;
    return gfx;
}

}  // namespace

TEST(MipBiasLimitsTest, ParsesOnlyFiniteValuesInsideTheSamplerRange) {
    float value = 0.0f;
    EXPECT_FALSE(ce::mip_bias::TryParseLimit("default", value));
    EXPECT_FALSE(ce::mip_bias::TryParseLimit("", value));
    EXPECT_FALSE(ce::mip_bias::TryParseLimit("nan", value));
    EXPECT_FALSE(ce::mip_bias::TryParseLimit("-2.0x", value));
    EXPECT_FALSE(ce::mip_bias::TryParseLimit("-16.5", value));
    EXPECT_FALSE(ce::mip_bias::TryParseLimit("16", value));
    ASSERT_TRUE(ce::mip_bias::TryParseLimit("-2.0", value));
    EXPECT_FLOAT_EQ(value, -2.0f);
    ASSERT_TRUE(ce::mip_bias::TryParseLimit("+1.5", value));
    EXPECT_FLOAT_EQ(value, 1.5f);
}

TEST(MipBiasLimitsTest, BoundsOnlyValuesPastTheLimit) {
    const ce::mip_bias::Limits limits = ce::mip_bias::ParseLimits("-2.0", "2.0");
    ASSERT_TRUE(limits.Active());
    EXPECT_FLOAT_EQ(ce::mip_bias::ApplyLimits(-3.5f, limits), -2.0f);
    EXPECT_FLOAT_EQ(ce::mip_bias::ApplyLimits(-1.0f, limits), -1.0f);
    EXPECT_FLOAT_EQ(ce::mip_bias::ApplyLimits(0.0f, limits), 0.0f);
    EXPECT_FLOAT_EQ(ce::mip_bias::ApplyLimits(1.99f, limits), 1.99f);
    EXPECT_FLOAT_EQ(ce::mip_bias::ApplyLimits(4.0f, limits), 2.0f);
}

TEST(MipBiasLimitsTest, OneSidedLimitLeavesTheOtherSideAlone) {
    const ce::mip_bias::Limits floorOnly = ce::mip_bias::ParseLimits("-2.0", "default");
    EXPECT_TRUE(floorOnly.hasMin);
    EXPECT_FALSE(floorOnly.hasMax);
    EXPECT_FLOAT_EQ(ce::mip_bias::ApplyLimits(-5.0f, floorOnly), -2.0f);
    EXPECT_FLOAT_EQ(ce::mip_bias::ApplyLimits(8.0f, floorOnly), 8.0f);

    const ce::mip_bias::Limits ceilingOnly = ce::mip_bias::ParseLimits("default", "2.0");
    EXPECT_FLOAT_EQ(ce::mip_bias::ApplyLimits(-5.0f, ceilingOnly), -5.0f);
    EXPECT_FLOAT_EQ(ce::mip_bias::ApplyLimits(8.0f, ceilingOnly), 2.0f);
}

TEST(MipBiasLimitsTest, ContradictoryPairAppliesNeitherBound) {
    const ce::mip_bias::Limits limits = ce::mip_bias::ParseLimits("1.0", "-1.0");
    EXPECT_FALSE(limits.Active());
    EXPECT_FLOAT_EQ(ce::mip_bias::ApplyLimits(-5.0f, limits), -5.0f);
    EXPECT_FLOAT_EQ(ce::mip_bias::ApplyLimits(5.0f, limits), 5.0f);
}

// A limit alone must arm every API's bias path. Before the shared predicate the
// gates only knew mip_bias and force_mip_bias_clamp, so a configured bound
// would have been silently skipped by DX12, D3D9, legacy D3D and OpenGL.
TEST(MipBiasLimitsTest, LimitAloneCountsAsAMipBiasOverride) {
    EXPECT_FALSE(HasMipBiasOverride(LimitsOnly("default", "default")));
    EXPECT_TRUE(HasMipBiasOverride(LimitsOnly("-2.0", "default")));
    EXPECT_TRUE(HasMipBiasOverride(LimitsOnly("default", "2.0")));
    EXPECT_FALSE(HasMipBiasOverride(LimitsOnly("1.0", "-1.0")));
    EXPECT_TRUE(ce::dx12_sampler_policy::HasSamplerOverride(LimitsOnly("-2.0", "default")));
}

TEST(MipBiasLimitsTest, FinalizeAppliesLimitsAfterTheConfiguredBias) {
    GraphicsConfig gfx = LimitsOnly("-2.0", "default");
    EXPECT_FLOAT_EQ(FinalizeMipBias(gfx, ApplyConfiguredMipBias(gfx, -3.0f)), -2.0f);
    EXPECT_FLOAT_EQ(FinalizeMipBias(gfx, ApplyConfiguredMipBias(gfx, -0.5f)), -0.5f);

    // The bound holds the final value, including an offset that crosses it.
    gfx.mipBias = "-1.5";
    gfx.mipBiasMode = "offset";
    EXPECT_FLOAT_EQ(FinalizeMipBias(gfx, ApplyConfiguredMipBias(gfx, -1.0f)), -2.0f);
    EXPECT_FLOAT_EQ(FinalizeMipBias(gfx, ApplyConfiguredMipBias(gfx, 0.0f)), -1.5f);

    // The zero clamp keeps precedence over everything else.
    gfx.forceMipBiasClamp = true;
    EXPECT_FLOAT_EQ(FinalizeMipBias(gfx, ApplyConfiguredMipBias(gfx, -3.0f)), 0.0f);
}

TEST(MipBiasLimitsTest, DX12KeepsTheApplicationBiasInsideTheLimits) {
    const GraphicsConfig gfx = LimitsOnly("-2.0", "2.0");

    D3D12_SAMPLER_DESC tooSharp = BiasedMaterialSampler(-3.25f);
    const auto tooSharpResult = ce::dx12_sampler_policy::Apply(tooSharp, gfx);
    EXPECT_EQ(tooSharpResult.decision, ce::dx12_sampler_policy::Decision::Allow);
    EXPECT_TRUE(tooSharpResult.mipBiasModified);
    EXPECT_FLOAT_EQ(tooSharp.MipLODBias, -2.0f);

    D3D12_SAMPLER_DESC inRange = BiasedMaterialSampler(-1.0f);
    const auto inRangeResult = ce::dx12_sampler_policy::Apply(inRange, gfx);
    EXPECT_FALSE(inRangeResult.Modified());
    EXPECT_EQ(inRangeResult.decision, ce::dx12_sampler_policy::Decision::AlreadyCompliant);
    EXPECT_FLOAT_EQ(inRange.MipLODBias, -1.0f);

    D3D12_SAMPLER_DESC tooBlurry = BiasedMaterialSampler(3.0f);
    ce::dx12_sampler_policy::Apply(tooBlurry, gfx);
    EXPECT_FLOAT_EQ(tooBlurry.MipLODBias, 2.0f);
}

// Shadow-comparison samplers carry their own bias (Witcher 3 creates one at
// +2.0); the safe policy must leave special-purpose samplers untouched.
TEST(MipBiasLimitsTest, DX12LeavesComparisonSamplersAlone) {
    const GraphicsConfig gfx = LimitsOnly("-1.0", "1.0");
    D3D12_SAMPLER_DESC shadow = BiasedMaterialSampler(2.0f);
    shadow.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    const auto result = ce::dx12_sampler_policy::Apply(shadow, gfx);
    EXPECT_FALSE(result.Modified());
    EXPECT_FLOAT_EQ(shadow.MipLODBias, 2.0f);
}

TEST_F(ConfigTest, MipBiasLimitsLoadAndDefault) {
    WriteConfig(
        "[Graphics]\n"
        "mip_bias_min=-2.0\n"
        "mip_bias_max=2.5\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    EXPECT_EQ(config.graphics.mipBiasMin, "-2.0");
    EXPECT_EQ(config.graphics.mipBiasMax, "2.5");

    WriteConfig("[Graphics]\nmip_bias=default\n");
    AppConfig defaults;
    LoadConfig(tempConfigFile, defaults);
    EXPECT_EQ(defaults.graphics.mipBiasMin, "default");
    EXPECT_EQ(defaults.graphics.mipBiasMax, "default");
}

TEST_F(ConfigTest, MipBiasLimitsRejectMalformedOutOfRangeAndContradictoryValues) {
    WriteConfig(
        "[Graphics]\n"
        "mip_bias_min=-2.0oops\n"
        "mip_bias_max=99\n");
    AppConfig malformed;
    LoadConfig(tempConfigFile, malformed);
    EXPECT_EQ(malformed.graphics.mipBiasMin, "default");
    EXPECT_EQ(malformed.graphics.mipBiasMax, "default");

    WriteConfig(
        "[Graphics]\n"
        "mip_bias_min=1.0\n"
        "mip_bias_max=-1.0\n");
    AppConfig contradictory;
    LoadConfig(tempConfigFile, contradictory);
    EXPECT_EQ(contradictory.graphics.mipBiasMin, "default");
    EXPECT_EQ(contradictory.graphics.mipBiasMax, "default");
}

TEST_F(ConfigTest, MipBiasLimitsFollowAProcessProfile) {
    WriteConfig(
        "[Graphics]\n"
        "mip_bias_min=default\n"
        "[Profile.Witcher]\n"
        "process=witcher3.exe\n"
        "Graphics.mip_bias_min=-2.0\n");
    AppConfig profiled;
    LoadConfig(tempConfigFile, profiled, "witcher3.exe");
    EXPECT_EQ(profiled.graphics.mipBiasMin, "-2.0");

    AppConfig other;
    LoadConfig(tempConfigFile, other, "other.exe");
    EXPECT_EQ(other.graphics.mipBiasMin, "default");
}

// The DX12 sampler hook logs the bias range only when it widens, so Observe()
// must report exactly the widening calls and ignore values with no range.
TEST(MipBiasRangeTest, ReportsOnlyWideningValues) {
    ce::mip_bias::BiasRange range;
    EXPECT_FALSE(range.HasValue());

    EXPECT_TRUE(range.Observe(0.0f));
    EXPECT_TRUE(range.HasValue());
    EXPECT_FLOAT_EQ(range.Min(), 0.0f);
    EXPECT_FLOAT_EQ(range.Max(), 0.0f);

    EXPECT_TRUE(range.Observe(-4.0f));
    EXPECT_TRUE(range.Observe(2.0f));
    EXPECT_FALSE(range.Observe(-0.75f)) << "inside the range";
    EXPECT_FALSE(range.Observe(-4.0f)) << "equal to a bound";
    EXPECT_FALSE(range.Observe(std::numeric_limits<float>::quiet_NaN()));
    EXPECT_FALSE(range.Observe(-std::numeric_limits<float>::infinity()));
    EXPECT_FLOAT_EQ(range.Min(), -4.0f);
    EXPECT_FLOAT_EQ(range.Max(), 2.0f);
}
