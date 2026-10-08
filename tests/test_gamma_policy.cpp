#include <gtest/gtest.h>

#include <limits>

#include "common/graphics/gamma_policy.h"
#include "common/graphics/sharpen_policy.h"
#include "hook/sharpen/gamma_native_curve.h"
#include "hook/sharpen/sharpen_constants.h"
#include "hook/sharpen/sharpen_request.h"

TEST(GammaPolicy, VocabularyAndSourceValidation) {
    using namespace ce::gamma;
    Curve curve = Curve::Default;
    for (const char* text : {"srgb", "SRGB", "piecewise"}) {
        ASSERT_TRUE(TryParse(text, curve));
        EXPECT_EQ(curve, Curve::Srgb);
    }
    EXPECT_TRUE(TryParse("2,4", curve));
    EXPECT_EQ(curve, Curve::Power24);
    for (const char* text : {"", "2.3", "nan", "srgbjunk"})
        EXPECT_FALSE(TryParse(text, curve));
    EXPECT_FALSE(Requested({std::numeric_limits<float>::quiet_NaN(), 0.0f}));
    EXPECT_FALSE(Requested({2.2f, std::numeric_limits<float>::infinity()}));
}

TEST(GammaPolicy, KnownSrgbBreakpointsAndDirection) {
    EXPECT_NEAR(ce::gamma::Decode(0.04045, 0.0), 0.00313080495356037, 1e-14);
    EXPECT_NEAR(ce::gamma::Encode(0.0031308, 0.0), 0.040449936, 1e-12);
    EXPECT_NEAR(ce::gamma::Convert(0.1, {2.2f, 0.0f}), 0.07281616831, 1e-8);
    EXPECT_GT(ce::gamma::Convert(0.1, {2.2f, 2.4f}), 0.1);
}

TEST(GammaPolicy, AllConversionsPreserveEndpointsAndAreMonotonic) {
    for (float source : {0.0f, 2.2f, 2.4f}) {
        for (float destination : {0.0f, 2.2f, 2.4f}) {
            const ce::gamma::Request request{source, destination};
            EXPECT_DOUBLE_EQ(ce::gamma::Convert(0.0, request), 0.0);
            EXPECT_NEAR(ce::gamma::Convert(1.0, request), 1.0, 1e-14);
            double previous = -1.0;
            for (int step = 0; step <= 4096; ++step) {
                const double value = static_cast<double>(step) / 4096.0;
                const double converted = ce::gamma::Convert(value, request);
                EXPECT_GE(converted, previous);
                EXPECT_NEAR(ce::gamma::Convert(converted, {destination, source}), value, 5e-8);
                previous = converted;
            }
        }
    }
}

TEST(GammaPolicy, GammaRunsWithSharpenOffOrZeroAmountAndHdrKeepsSharpening) {
    using namespace ce::sharpen;
    Request request;
    request.gamma.destination = 0.0f;
    Target target;
    target.route = Route::NormalBackbuffer;
    target.encoding = TargetEncoding::Unorm;
    target.width = 256;
    target.height = 32;
    target.readable = target.writable = true;
    target.colorBits = 8;
    auto decision = Decide(request, target);
    ASSERT_TRUE(decision.run);
    EXPECT_EQ(decision.mode, Mode::Off);
    EXPECT_FLOAT_EQ(decision.gammaDitherScale, 1.0f / 255.0f);
    request.mode = Mode::Cas;
    request.intensity = 0.0f;
    EXPECT_EQ(Decide(request, target).mode, Mode::Off);
    target.encoding = TargetEncoding::Pq;
    EXPECT_FALSE(Decide(request, target).run);
    request.intensity = 1.0f;
    decision = Decide(request, target);
    EXPECT_TRUE(decision.run);
    EXPECT_EQ(decision.mode, Mode::Cas);
    EXPECT_FALSE(ce::gamma::Requested(decision.gamma));
    target.encoding = TargetEncoding::ScrgbLinear;
    EXPECT_FALSE(ce::gamma::Requested(Decide(request, target).gamma));
    target.encoding = TargetEncoding::SdrLinear;
    target.colorBits = 0;
    decision = Decide(request, target);
    EXPECT_TRUE(decision.gammaValuesLinear);
    EXPECT_FLOAT_EQ(decision.gammaDitherScale, 0.0f);
    const auto constants = BuildShaderConstants(decision.mode, decision, 256, 32);
    EXPECT_FLOAT_EQ(constants.gammaDestination, 0.0f);
    EXPECT_EQ(constants.gammaValuesLinear, 3u);
}

TEST(GammaPolicy, OnlyConfirmedNativeCurveReplacesTheExplicitSource) {
    GraphicsConfig graphics;
    graphics.postProcessDisplayGamma = "srgb";
    graphics.displayGamma = 0.0f;
    ce::gamma::PublishVerifiedNativeCurve(-1.0f);
    EXPECT_FLOAT_EQ(ce::sharpen::ResolveRequest(graphics).gamma.source, 2.2f);
    ce::gamma::PublishVerifiedNativeCurve(0.0f);
    EXPECT_FALSE(ce::sharpen::Requested(ce::sharpen::ResolveRequest(graphics)));
    ce::gamma::PublishVerifiedNativeCurve(2.4f);
    EXPECT_FLOAT_EQ(ce::sharpen::ResolveRequest(graphics).gamma.source, 2.4f);
    ce::gamma::PublishVerifiedNativeCurve(-1.0f);
}
