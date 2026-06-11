#include <gtest/gtest.h>
#include "../testapp/fg_upscale_policy.h"

using testapp::fg::ComputeJitter;
using testapp::fg::ComputeRenderSize;
using testapp::fg::HaltonSequence;
using testapp::fg::JitterOffset;
using testapp::fg::JitterPhaseCount;
using testapp::fg::ParseUpscaleQuality;
using testapp::fg::RenderSize;
using testapp::fg::UpscaleQuality;
using testapp::fg::UpscaleQualityName;
using testapp::fg::UpscaleRatio;

TEST(UpscalePolicyTest, QualityNamesRoundTrip) {
    const UpscaleQuality all[] = {UpscaleQuality::NativeAA, UpscaleQuality::Quality, UpscaleQuality::Balanced,
                                  UpscaleQuality::Performance, UpscaleQuality::UltraPerformance};
    for (UpscaleQuality quality : all) {
        UpscaleQuality parsed = UpscaleQuality::NativeAA;
        EXPECT_TRUE(ParseUpscaleQuality(UpscaleQualityName(quality), &parsed));
        EXPECT_EQ(parsed, quality);
    }
    UpscaleQuality parsed = UpscaleQuality::NativeAA;
    EXPECT_FALSE(ParseUpscaleQuality("ultra", &parsed));
    EXPECT_FALSE(ParseUpscaleQuality(nullptr, &parsed));
}

TEST(UpscalePolicyTest, RenderSizeMatchesQualityRatios) {
    // 4K display, the standard FSR/DLSS ratios.
    const RenderSize quality = ComputeRenderSize(3840, 2160, UpscaleQuality::Quality, 0);
    EXPECT_EQ(quality.width, 2560u);
    EXPECT_EQ(quality.height, 1440u);
    const RenderSize performance = ComputeRenderSize(3840, 2160, UpscaleQuality::Performance, 0);
    EXPECT_EQ(performance.width, 1920u);
    EXPECT_EQ(performance.height, 1080u);
    const RenderSize ultra = ComputeRenderSize(3840, 2160, UpscaleQuality::UltraPerformance, 0);
    EXPECT_EQ(ultra.width, 1280u);
    EXPECT_EQ(ultra.height, 720u);
}

TEST(UpscalePolicyTest, NativeAaIsIdentity) {
    const RenderSize native = ComputeRenderSize(3840, 2160, UpscaleQuality::NativeAA, 0);
    EXPECT_EQ(native.width, 3840u);
    EXPECT_EQ(native.height, 2160u);
    // Odd display size must stay identical at native (no even-alignment shrink).
    const RenderSize odd = ComputeRenderSize(1366, 768, UpscaleQuality::NativeAA, 0);
    EXPECT_EQ(odd.width, 1366u);
    EXPECT_EQ(odd.height, 768u);
}

TEST(UpscalePolicyTest, RenderSizeIsEvenAlignedAndClamped) {
    // Balanced 1.7x on 4K: 2258.8 -> floored to even 2258.
    const RenderSize balanced = ComputeRenderSize(3840, 2160, UpscaleQuality::Balanced, 0);
    EXPECT_EQ(balanced.width % 2, 0u);
    EXPECT_EQ(balanced.height % 2, 0u);
    EXPECT_EQ(balanced.width, 2258u);
    EXPECT_EQ(balanced.height, 1270u);
    // Tiny display never reaches zero.
    const RenderSize tiny = ComputeRenderSize(4, 4, UpscaleQuality::UltraPerformance, 0);
    EXPECT_GE(tiny.width, 2u);
    EXPECT_GE(tiny.height, 2u);
}

TEST(UpscalePolicyTest, ScalePercentOverrideWins) {
    // 66% override on 4K: 3840*0.66 = 2534.4 -> 2534, 2160*0.66 = 1425.6 -> floored even 1424.
    const RenderSize scaled = ComputeRenderSize(3840, 2160, UpscaleQuality::Performance, 66);
    EXPECT_EQ(scaled.width, 2534u);
    EXPECT_EQ(scaled.height, 1424u);
    // Override >= 100 is native.
    const RenderSize full = ComputeRenderSize(3840, 2160, UpscaleQuality::Performance, 100);
    EXPECT_EQ(full.width, 3840u);
    EXPECT_EQ(full.height, 2160u);
}

TEST(UpscalePolicyTest, HaltonFirstValuesMatchReference) {
    EXPECT_FLOAT_EQ(HaltonSequence(1, 2), 0.5f);
    EXPECT_FLOAT_EQ(HaltonSequence(2, 2), 0.25f);
    EXPECT_FLOAT_EQ(HaltonSequence(3, 2), 0.75f);
    EXPECT_FLOAT_EQ(HaltonSequence(1, 3), 1.0f / 3.0f);
    EXPECT_FLOAT_EQ(HaltonSequence(2, 3), 2.0f / 3.0f);
    EXPECT_FLOAT_EQ(HaltonSequence(3, 3), 1.0f / 9.0f);
}

TEST(UpscalePolicyTest, JitterStaysSubPixelAndCyclesThroughPhase) {
    const int phaseCount = JitterPhaseCount(2560, 3840);
    EXPECT_EQ(phaseCount, 18);  // 8 * 1.5^2
    for (uint64_t frame = 0; frame < 64; ++frame) {
        const JitterOffset jitter = ComputeJitter(frame, phaseCount);
        EXPECT_GE(jitter.x, -0.5f);
        EXPECT_LT(jitter.x, 0.5f);
        EXPECT_GE(jitter.y, -0.5f);
        EXPECT_LT(jitter.y, 0.5f);
    }
    // Sequence repeats with the phase.
    const JitterOffset a = ComputeJitter(3, phaseCount);
    const JitterOffset b = ComputeJitter(3 + phaseCount, phaseCount);
    EXPECT_FLOAT_EQ(a.x, b.x);
    EXPECT_FLOAT_EQ(a.y, b.y);
}

TEST(UpscalePolicyTest, JitterPhaseCountFollowsFsrFormula) {
    EXPECT_EQ(JitterPhaseCount(3840, 3840), 8);   // native
    EXPECT_EQ(JitterPhaseCount(1920, 3840), 32);  // 2.0x
    EXPECT_EQ(JitterPhaseCount(1280, 3840), 72);  // 3.0x
    EXPECT_EQ(JitterPhaseCount(0, 3840), 8);      // degenerate input falls back to base
}
