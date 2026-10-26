#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "mediaengine/video/scale_kernel.h"
#include "mediaengine/video/shader_conversion_route.h"

// The shader scaler's policy (scale_kernel.h) and the encoder's shader
// conversion route (shader_conversion_route.h). GPU behaviour is covered by
// FrameScalerGpuTest.

namespace {

using ce::video_scale::AxisTable;
using ce::video_scale::BuildAxisTable;
using ce::video_scale::IntermediatePrecision;
using ce::video_scale::KernelSpec;
using ce::video_scale::PassOrder;

KernelSpec Spec(uint32_t lobes, float gain) {
    KernelSpec spec;
    spec.lobes = lobes;
    spec.sideLobeGain = gain;
    return spec;
}

double WeightSum(const AxisTable& table, uint32_t output) {
    double sum = 0.0;
    for (uint32_t k = 0; k < table.taps; ++k)
        sum += table.weights[static_cast<size_t>(output) * table.taps + k];
    return sum;
}

TEST(ScaleKernelTest, ConfigMapsQualityToLobesAndSharpnessToSideLobes) {
    EXPECT_EQ(ce::video_scale::KernelSpecFromConfig("normal", 100).lobes, 2u);
    EXPECT_FLOAT_EQ(ce::video_scale::KernelSpecFromConfig("normal", 100).sideLobeGain, 1.0f);
    EXPECT_EQ(ce::video_scale::KernelSpecFromConfig("BEST", 50).lobes, 3u);
    EXPECT_FLOAT_EQ(ce::video_scale::KernelSpecFromConfig("best", 50).sideLobeGain, 0.5f);
    EXPECT_FLOAT_EQ(ce::video_scale::KernelSpecFromConfig("best", -5).sideLobeGain, 0.0f);
    EXPECT_FLOAT_EQ(ce::video_scale::KernelSpecFromConfig("unknown", 250).sideLobeGain, 1.0f);
    EXPECT_EQ(ce::video_scale::KernelSpecFromConfig("", 0).lobes, 2u);
    EXPECT_STREQ(ce::video_scale::DescribeKernel(Spec(3, 1.0f)), "lanczos3");
    EXPECT_STREQ(ce::video_scale::DescribeKernel(Spec(2, 1.0f)), "lanczos2");
}

TEST(ScaleKernelTest, KernelIsAnInterpolatingLanczosWithScalableSideLobes) {
    const KernelSpec sharp = Spec(3, 1.0f);
    EXPECT_DOUBLE_EQ(ce::video_scale::KernelWeight(0.0, sharp), 1.0);
    for (double x : {1.0, 2.0, -1.0, -2.0})
        EXPECT_NEAR(ce::video_scale::KernelWeight(x, sharp), 0.0, 1e-12) << x;
    EXPECT_EQ(ce::video_scale::KernelWeight(3.0, sharp), 0.0);
    EXPECT_EQ(ce::video_scale::KernelWeight(-3.5, sharp), 0.0);
    EXPECT_GT(ce::video_scale::KernelWeight(0.5, sharp), 0.0);
    EXPECT_LT(ce::video_scale::KernelWeight(1.5, sharp), 0.0);  // the side lobe that sharpens
    EXPECT_GT(ce::video_scale::KernelWeight(2.5, sharp), 0.0);
    const KernelSpec soft = Spec(3, 0.0f);
    EXPECT_EQ(ce::video_scale::KernelWeight(1.5, soft), 0.0);
    EXPECT_EQ(ce::video_scale::KernelWeight(2.5, soft), 0.0);
    EXPECT_DOUBLE_EQ(ce::video_scale::KernelWeight(0.5, soft), ce::video_scale::KernelWeight(0.5, sharp));
    EXPECT_NEAR(ce::video_scale::KernelWeight(1.5, Spec(3, 0.5f)), 0.5 * ce::video_scale::KernelWeight(1.5, sharp),
                1e-12);
}

TEST(ScaleKernelTest, EqualSizesAreTheIdentity) {
    const AxisTable table = BuildAxisTable(17, 17, Spec(2, 1.0f));
    ASSERT_EQ(table.taps, 4u);
    for (uint32_t i = 0; i < 17; ++i) {
        EXPECT_EQ(table.first[i], static_cast<int32_t>(i));
        EXPECT_FLOAT_EQ(table.weights[i * table.taps], 1.0f);
        EXPECT_EQ(table.lobeBegin[i], 0u);
        EXPECT_EQ(table.lobeEnd[i], 1u);
    }
}

TEST(ScaleKernelTest, EveryOutputsWeightsSumToOne) {
    const uint32_t sizes[][2] = {{3840, 1920}, {1920, 1280}, {1919, 1280}, {1280, 3840}, {7, 3}, {3, 7}, {1, 5}, {5, 1}};
    for (uint32_t lobes : {2u, 3u}) {
        for (const auto& size : sizes) {
            const AxisTable table = BuildAxisTable(size[0], size[1], Spec(lobes, 1.0f));
            ASSERT_EQ(table.outSize, size[1]);
            ASSERT_EQ(table.taps % 4u, 0u);
            for (uint32_t i = 0; i < table.outSize; ++i)
                ASSERT_NEAR(WeightSum(table, i), 1.0, 1e-5) << size[0] << "->" << size[1] << " at " << i;
        }
    }
}

TEST(ScaleKernelTest, DownscalingStretchesTheKernelOverTheRatio) {
    // 3:1 with two lobes reaches six source samples to each side: the
    // passband ends at the output's Nyquist frequency, so detail cannot alias.
    const AxisTable table = BuildAxisTable(3840, 1280, Spec(2, 1.0f));
    EXPECT_DOUBLE_EQ(table.stretch, 3.0);
    EXPECT_EQ(table.taps, 12u);
    EXPECT_LT(table.first[0], 0);  // edge taps clamp to the border in the shader
    const uint32_t mid = 640;
    // Main lobe: |offset| < 3 source samples around centre 3 * 640 + 1.
    EXPECT_EQ(table.first[mid] + static_cast<int32_t>(table.lobeBegin[mid]), 1919);
    EXPECT_EQ(table.first[mid] + static_cast<int32_t>(table.lobeEnd[mid]), 1924);
    for (uint32_t k = 0; k < table.taps; ++k) {
        const float w = table.weights[mid * table.taps + k];
        if (k >= table.lobeBegin[mid] && k < table.lobeEnd[mid])
            EXPECT_GT(w, 0.0f) << k;
        else
            EXPECT_LE(w, 1e-7f) << k;  // Lanczos2's only side lobe is negative
    }
}

TEST(ScaleKernelTest, UpscalingRunsAtSourcePitchAndIsSymmetric) {
    const AxisTable table = BuildAxisTable(1280, 3840, Spec(2, 1.0f));
    EXPECT_DOUBLE_EQ(table.stretch, 1.0);
    EXPECT_EQ(table.taps, 4u);
    const AxisTable down = BuildAxisTable(64, 32, Spec(3, 1.0f));
    for (uint32_t i = 0; i < 32; ++i) {
        const uint32_t mirror = 31 - i;
        for (uint32_t k = 0; k < down.taps; ++k) {
            const int32_t sample = down.first[i] + static_cast<int32_t>(k);
            const int32_t mirrored = 63 - sample;
            const int32_t mirrorTap = mirrored - down.first[mirror];
            if (mirrorTap < 0 || mirrorTap >= static_cast<int32_t>(down.taps))
                continue;
            EXPECT_NEAR(down.weights[i * down.taps + k], down.weights[mirror * down.taps + mirrorTap], 1e-6);
        }
    }
}

TEST(ScaleKernelTest, ZeroSideLobeGainHasNoNegativeWeight) {
    for (const auto& size : {std::pair<uint32_t, uint32_t>{1920, 1280}, {1280, 1920}, {3840, 1280}}) {
        const AxisTable table = BuildAxisTable(size.first, size.second, Spec(3, 0.0f));
        for (float w : table.weights)
            ASSERT_GE(w, 0.0f);
    }
}

TEST(ScaleKernelTest, PackedTableIsHeaderThenWeights) {
    const AxisTable table = BuildAxisTable(20, 8, Spec(2, 1.0f));
    const std::vector<float> packed = ce::video_scale::PackAxisTable(table);
    const uint32_t stride = ce::video_scale::PackedAxisStride(table);
    ASSERT_EQ(stride, 1u + table.taps / 4u);
    ASSERT_EQ(packed.size(), static_cast<size_t>(8) * stride * 4u);
    for (uint32_t i = 0; i < 8; ++i) {
        const float* header = &packed[static_cast<size_t>(i) * stride * 4u];
        EXPECT_EQ(header[0], static_cast<float>(table.first[i]));
        EXPECT_EQ(header[1], static_cast<float>(table.lobeBegin[i]));
        EXPECT_EQ(header[2], static_cast<float>(table.lobeEnd[i]));
        for (uint32_t k = 0; k < table.taps; ++k)
            EXPECT_EQ(header[4 + k], table.weights[i * table.taps + k]);
    }
    EXPECT_TRUE(ce::video_scale::PackAxisTable(BuildAxisTable(0, 8, Spec(2, 1.0f))).empty());
}

TEST(ScaleKernelTest, PlanRunsOnlyScaledAxesAndTheCheaperOrderFirst) {
    const KernelSpec spec = Spec(2, 1.0f);
    const auto same = ce::video_scale::PlanScale(1920, 1080, 1920, 1080, spec, 8, 8);
    EXPECT_FALSE(same.scaleX);
    EXPECT_FALSE(same.scaleY);
    const auto widthOnly = ce::video_scale::PlanScale(1920, 1080, 1280, 1080, spec, 8, 8);
    EXPECT_TRUE(widthOnly.scaleX);
    EXPECT_FALSE(widthOnly.scaleY);
    EXPECT_EQ(widthOnly.vertical.taps, 0u);
    // 2:1 across, 4:1 down: filtering down first reads fewer source samples.
    const auto tall = ce::video_scale::PlanScale(1000, 4000, 500, 1000, spec, 8, 8);
    ASSERT_TRUE(tall.scaleX && tall.scaleY);
    EXPECT_EQ(tall.horizontal.taps, 8u);
    EXPECT_EQ(tall.vertical.taps, 16u);
    EXPECT_EQ(tall.order, PassOrder::kVerticalFirst);
    EXPECT_EQ(tall.intermediateWidth, 1000u);
    EXPECT_EQ(tall.intermediateHeight, 1000u);
    EXPECT_EQ(tall.estimatedTaps, 1000ull * 1000 * 16 + 500ull * 1000 * 8);
    const auto uhd = ce::video_scale::PlanScale(3840, 2160, 1920, 1080, spec, 8, 8);
    EXPECT_EQ(uhd.order, PassOrder::kHorizontalFirst);
    EXPECT_EQ(uhd.intermediateWidth, 1920u);
    EXPECT_EQ(uhd.intermediateHeight, 2160u);
}

TEST(ScaleKernelTest, IntermediatePrecisionFollowsSourceAndOutputBits) {
    const KernelSpec spec = Spec(2, 1.0f);
    EXPECT_EQ(ce::video_scale::PlanScale(64, 64, 32, 32, spec, 8, 8).precision, IntermediatePrecision::kUnorm10);
    EXPECT_EQ(ce::video_scale::PlanScale(64, 64, 32, 32, spec, 8, 10).precision, IntermediatePrecision::kUnorm10);
    EXPECT_EQ(ce::video_scale::PlanScale(64, 64, 32, 32, spec, 10, 8).precision, IntermediatePrecision::kUnorm10);
    EXPECT_EQ(ce::video_scale::PlanScale(64, 64, 32, 32, spec, 10, 10).precision, IntermediatePrecision::kUnorm16);
}

ce::video_format::ShaderConversionRouteInputs RouteInputs(DXGI_FORMAT source, DXGI_FORMAT output) {
    ce::video_format::ShaderConversionRouteInputs in;
    in.sourceFormat = source;
    in.sourceWidth = in.outputWidth = 1920;
    in.sourceHeight = in.outputHeight = 1080;
    in.sourceHasShaderResourceBind = true;
    in.outputFormat = output;
    return in;
}

TEST(ShaderConversionRouteTest, EverySupportedSourceHasAShaderRoute) {
    using ce::video_format::DecideShaderConversionRoute;
    using ce::video_format::RgbColorTransform;
    auto plain = DecideShaderConversionRoute(RouteInputs(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_NV12));
    ASSERT_TRUE(plain.convertible);
    EXPECT_FALSE(plain.sampleableCopy);
    EXPECT_EQ(plain.normalize, RgbColorTransform::kNone);
    EXPECT_FALSE(plain.scale);
    EXPECT_EQ(plain.scaleSourceBits, 8u);
    EXPECT_EQ(plain.outputBits, 8u);

    // Linear FP16 SDR (scRGB inject swapchains) went to the VideoProcessor.
    auto fp16 = DecideShaderConversionRoute(RouteInputs(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_P010));
    ASSERT_TRUE(fp16.convertible);
    EXPECT_EQ(fp16.normalize, RgbColorTransform::kLinearToSrgb);
    EXPECT_EQ(fp16.scaleSourceBits, 10u);
    EXPECT_EQ(fp16.outputBits, 10u);

    auto hdrIn = RouteInputs(DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_P010);
    hdrIn.sourceIsHdr = hdrIn.outputIsHdr = true;
    EXPECT_EQ(DecideShaderConversionRoute(hdrIn).normalize, RgbColorTransform::kScRgbToHdr10);
    auto toneMap = RouteInputs(DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_NV12);
    toneMap.sourceIsHdr = true;
    EXPECT_EQ(DecideShaderConversionRoute(toneMap).normalize, RgbColorTransform::kHdr10ToSdr);

    auto unsampleable = RouteInputs(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_NV12);
    unsampleable.sourceHasShaderResourceBind = false;
    EXPECT_TRUE(DecideShaderConversionRoute(unsampleable).sampleableCopy);
    EXPECT_FALSE(DecideShaderConversionRoute(unsampleable).resolveMultisample);
    auto msaa = RouteInputs(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_NV12);
    msaa.sourceSampleCount = 4;
    EXPECT_TRUE(DecideShaderConversionRoute(msaa).sampleableCopy);
    EXPECT_TRUE(DecideShaderConversionRoute(msaa).resolveMultisample);

    auto scaled = RouteInputs(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_NV12);
    scaled.outputWidth = 1280;
    scaled.outputHeight = 720;
    EXPECT_TRUE(DecideShaderConversionRoute(scaled).scale);
}

TEST(ShaderConversionRouteTest, RefusalsNameTheirReason) {
    using ce::video_format::DecideShaderConversionRoute;
    auto outputFormat = DecideShaderConversionRoute(RouteInputs(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM));
    EXPECT_FALSE(outputFormat.convertible);
    EXPECT_STREQ(outputFormat.refusal, "output format");
    auto hdrNv12 = RouteInputs(DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_NV12);
    hdrNv12.sourceIsHdr = hdrNv12.outputIsHdr = true;
    EXPECT_FALSE(DecideShaderConversionRoute(hdrNv12).convertible);
    EXPECT_STREQ(DecideShaderConversionRoute(RouteInputs(DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_NV12)).refusal,
                 "source format");
    auto hdr8 = RouteInputs(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_P010);
    hdr8.sourceIsHdr = true;
    EXPECT_STREQ(DecideShaderConversionRoute(hdr8).refusal, "HDR source format");
    auto odd = RouteInputs(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_NV12);
    odd.outputWidth = 1279;
    EXPECT_STREQ(DecideShaderConversionRoute(odd).refusal, "frame size");
}

}  // namespace
