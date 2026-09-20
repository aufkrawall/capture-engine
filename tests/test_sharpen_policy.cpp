#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>

#include "../common/sharpen_policy.h"
#include "../hook/common/sharpen_constants.h"

using namespace ce::sharpen;

namespace {

uint32_t AsBits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float FromBits(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

Target MakeUsableTarget() {
    Target target;
    target.route = Route::NormalBackbuffer;
    target.encoding = TargetEncoding::Srgb;
    target.width = 1920;
    target.height = 1080;
    target.readable = true;
    target.writable = true;
    return target;
}

Request MakeRequest(Mode mode, float strength = kDefaultStrength, float intensity = kDefaultIntensity) {
    Request request;
    request.mode = mode;
    request.strength = strength;
    request.intensity = intensity;
    return request;
}

}  // namespace

TEST(SharpenPolicy, ParseModeAcceptsKnownNamesCaseInsensitively) {
    EXPECT_EQ(ParseMode("cas"), Mode::Cas);
    EXPECT_EQ(ParseMode("CAS"), Mode::Cas);
    EXPECT_EQ(ParseMode("rcas"), Mode::Rcas);
    EXPECT_EQ(ParseMode("RCas"), Mode::Rcas);
    EXPECT_EQ(ParseMode("off"), Mode::Off);
}

TEST(SharpenPolicy, ParseModeRejectsUnknownValuesAsOff) {
    // An unrecognized value must never resolve to a different effect than the
    // user asked for; refusing to sharpen is the safe reading.
    EXPECT_EQ(ParseMode("fsr"), Mode::Off);
    EXPECT_EQ(ParseMode("ca"), Mode::Off);
    EXPECT_EQ(ParseMode("casx"), Mode::Off);
    EXPECT_EQ(ParseMode(""), Mode::Off);
    EXPECT_EQ(ParseMode(nullptr), Mode::Off);
}

TEST(SharpenPolicy, ModeNameRoundTripsThroughParse) {
    for (Mode mode : {Mode::Off, Mode::Cas, Mode::Rcas}) {
        EXPECT_EQ(ParseMode(ModeName(mode)), mode);
    }
}

TEST(SharpenPolicy, ClampStrengthBoundsAndRejectsNaN) {
    EXPECT_FLOAT_EQ(ClampStrength(0.5f), 0.5f);
    EXPECT_FLOAT_EQ(ClampStrength(-1.0f), kMinStrength);
    EXPECT_FLOAT_EQ(ClampStrength(4.0f), kMaxStrength);
    EXPECT_FLOAT_EQ(ClampStrength(std::numeric_limits<float>::quiet_NaN()), kMinStrength);
}

TEST(SharpenPolicy, StrengthMapsToEachEffectsNativeConvention) {
    // CAS sharpness rises with the value; RCAS attenuation is in stops and
    // falls with it. A shared 0..1 strength must not be handed to both raw.
    EXPECT_FLOAT_EQ(CasSharpnessFromStrength(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(CasSharpnessFromStrength(1.0f), 1.0f);
    EXPECT_FLOAT_EQ(RcasAttenuationFromStrength(1.0f), 0.0f);
    EXPECT_FLOAT_EQ(RcasAttenuationFromStrength(0.0f), kRcasMaxAttenuation);
    EXPECT_GT(RcasAttenuationFromStrength(0.25f), RcasAttenuationFromStrength(0.75f));
}

TEST(SharpenPolicy, AutoFilterSpaceOnlyConvertsLinearValues) {
    EXPECT_EQ(ResolveFilterSpace(false, ConfiguredSpace::Auto), FilterSpace::Direct);
    EXPECT_EQ(ResolveFilterSpace(true, ConfiguredSpace::Auto), FilterSpace::LinearToGamma);
}

TEST(SharpenPolicy, ExplicitFilterSpaceOverridesTheResolvedValueSpace) {
    EXPECT_EQ(ResolveFilterSpace(true, ConfiguredSpace::Direct), FilterSpace::Direct);
    EXPECT_EQ(ResolveFilterSpace(false, ConfiguredSpace::Gamma), FilterSpace::LinearToGamma);
}

TEST(SharpenPolicy, StoredPerceptualEncodingsReachTheShaderNonLinear) {
    EXPECT_FALSE(ValuesReachShaderAsLinear(TargetEncoding::Srgb, false));
    EXPECT_FALSE(ValuesReachShaderAsLinear(TargetEncoding::Unorm, false));
    // PQ is itself perceptual, so a contrast-adaptive kernel runs on it directly.
    EXPECT_FALSE(ValuesReachShaderAsLinear(TargetEncoding::Pq, false));
    // scRGB is linear light: filtering it directly rings around highlights.
    EXPECT_TRUE(ValuesReachShaderAsLinear(TargetEncoding::ScrgbLinear, false));
}

TEST(SharpenPolicy, AnSrgbViewMakesEvenAnEightBitTargetLinearInTheShader) {
    // The hardware decodes on load and re-encodes on store, so the kernel would
    // otherwise run on linear light without anything in the format saying so.
    EXPECT_TRUE(ValuesReachShaderAsLinear(TargetEncoding::Srgb, true));
    EXPECT_TRUE(ValuesReachShaderAsLinear(TargetEncoding::Unorm, true));
}

TEST(SharpenPolicy, ParseConfiguredSpaceDefaultsToAuto) {
    EXPECT_EQ(ParseConfiguredSpace("auto"), ConfiguredSpace::Auto);
    EXPECT_EQ(ParseConfiguredSpace("direct"), ConfiguredSpace::Direct);
    EXPECT_EQ(ParseConfiguredSpace("gamma"), ConfiguredSpace::Gamma);
    EXPECT_EQ(ParseConfiguredSpace("nonsense"), ConfiguredSpace::Auto);
    EXPECT_EQ(ParseConfiguredSpace(nullptr), ConfiguredSpace::Auto);
}

TEST(SharpenPolicy, UiResourceRouteCarriesNoFrame) {
    // The frame-generation runtimes' UI resource is a transparent overlay
    // texture, not the frame. Sharpening it would filter CE's own pixels and
    // leave the game's image untouched.
    EXPECT_FALSE(RouteCarriesFrame(Route::RuntimeUiResource));
    EXPECT_FALSE(RouteCarriesFrame(Route::Unknown));
    EXPECT_TRUE(RouteCarriesFrame(Route::NormalBackbuffer));
    EXPECT_TRUE(RouteCarriesFrame(Route::OffscreenCopy));
    EXPECT_TRUE(RouteCarriesFrame(Route::PostStreamline));
    // Every displayed frame under a present interposer arrives on its private
    // output chain, generated ones included.
    EXPECT_TRUE(RouteCarriesFrame(Route::InterposerOutputChain));
}

TEST(SharpenPolicyDecide, RunsOnAUsableBackbuffer) {
    const Decision decision = Decide(MakeRequest(Mode::Cas, 0.75f), MakeUsableTarget());
    EXPECT_TRUE(decision.run);
    EXPECT_STREQ(decision.reason, "cas");
    EXPECT_FLOAT_EQ(decision.effectParameter, 0.75f);
    EXPECT_EQ(decision.filterSpace, FilterSpace::Direct);
}

TEST(SharpenPolicyDecide, RcasReportsItsOwnParameterAndReason) {
    const Decision decision = Decide(MakeRequest(Mode::Rcas, 1.0f), MakeUsableTarget());
    EXPECT_TRUE(decision.run);
    EXPECT_STREQ(decision.reason, "rcas");
    EXPECT_FLOAT_EQ(decision.effectParameter, 0.0f);
}

TEST(SharpenPolicyDecide, DisabledModeNeverRuns) {
    const Decision decision = Decide(MakeRequest(Mode::Off), MakeUsableTarget());
    EXPECT_FALSE(decision.run);
    EXPECT_STREQ(decision.reason, "disabled");
}

TEST(SharpenPolicyDecide, RefusesTheUiResourceRouteWithItsOwnReason) {
    Target target = MakeUsableTarget();
    target.route = Route::RuntimeUiResource;
    const Decision decision = Decide(MakeRequest(Mode::Cas), target);
    EXPECT_FALSE(decision.run);
    EXPECT_STREQ(decision.reason, "ui_resource_route_carries_no_frame");
}

TEST(SharpenPolicyDecide, RefusesAnUnclassifiedRoute) {
    Target target = MakeUsableTarget();
    target.route = Route::Unknown;
    const Decision decision = Decide(MakeRequest(Mode::Cas), target);
    EXPECT_FALSE(decision.run);
    EXPECT_STREQ(decision.reason, "route_unclassified");
}

TEST(SharpenPolicyDecide, RefusesAnUnknownPresentationEncoding) {
    Target target = MakeUsableTarget();
    target.encoding = TargetEncoding::Unknown;
    const Decision decision = Decide(MakeRequest(Mode::Cas), target);
    EXPECT_FALSE(decision.run);
    EXPECT_STREQ(decision.reason, "unknown_presentation_encoding");
}

TEST(SharpenPolicyDecide, RefusesTinyTargets) {
    Target target = MakeUsableTarget();
    target.width = 16;
    const Decision tooNarrow = Decide(MakeRequest(Mode::Cas), target);
    EXPECT_FALSE(tooNarrow.run);
    EXPECT_STREQ(tooNarrow.reason, "target_too_small");

    target = MakeUsableTarget();
    target.height = 1;
    const Decision tooShort = Decide(MakeRequest(Mode::Cas), target);
    EXPECT_FALSE(tooShort.run);
    EXPECT_STREQ(tooShort.reason, "target_too_small");
}

TEST(SharpenPolicyDecide, FailsClosedWhenTheFrameCannotBeReadOrWritten) {
    Target unreadable = MakeUsableTarget();
    unreadable.readable = false;
    const Decision readDecision = Decide(MakeRequest(Mode::Cas), unreadable);
    EXPECT_FALSE(readDecision.run);
    EXPECT_STREQ(readDecision.reason, "frame_not_readable");

    Target unwritable = MakeUsableTarget();
    unwritable.writable = false;
    const Decision writeDecision = Decide(MakeRequest(Mode::Cas), unwritable);
    EXPECT_FALSE(writeDecision.run);
    EXPECT_STREQ(writeDecision.reason, "target_not_writable");
}

TEST(SharpenPolicyDecide, RefusalStillReportsTheResolvedFilterSpace) {
    // Diagnostics have to be able to say what the pass would have done, so the
    // resolved space is filled in even on the paths that do not run.
    Target target = MakeUsableTarget();
    target.encoding = TargetEncoding::ScrgbLinear;
    const Decision decision = Decide(MakeRequest(Mode::Off), target);
    EXPECT_FALSE(decision.run);
    EXPECT_EQ(decision.filterSpace, FilterSpace::LinearToGamma);
}

TEST(SharpenPolicyDecide, AnSrgbViewSelectsTheGammaRoundTripOnAnEightBitTarget) {
    Target target = MakeUsableTarget();
    target.encoding = TargetEncoding::Srgb;
    target.viewAppliesSrgbConversion = true;
    const Decision decision = Decide(MakeRequest(Mode::Cas), target);
    ASSERT_TRUE(decision.run);
    EXPECT_EQ(decision.filterSpace, FilterSpace::LinearToGamma);
}

TEST(SharpenConstants, CasMatchesAmdsPackedSharpnessDerivation) {
    const Decision decision = Decide(MakeRequest(Mode::Cas, 1.0f), MakeUsableTarget());
    ASSERT_TRUE(decision.run);
    const ShaderConstants constants = BuildShaderConstants(Mode::Cas, decision, 1920, 1080);

    // Sharpen-only means input and output extents are equal, so the scaling
    // terms are exactly 1.0 and the sample offset exactly 0.0.
    EXPECT_EQ(constants.const0[0], AsBits(1.0f));
    EXPECT_EQ(constants.const0[1], AsBits(1.0f));
    EXPECT_EQ(constants.const0[2], AsBits(0.0f));
    EXPECT_EQ(constants.const0[3], AsBits(0.0f));

    // ffxCasSetup: sharp = -1 / lerp(8, 5, saturate(sharpness)).
    const float expectedSharp = -1.0f / (8.0f + (5.0f - 8.0f) * 1.0f);
    EXPECT_FLOAT_EQ(FromBits(constants.const1[0]), expectedSharp);
    EXPECT_EQ(constants.const1[2], AsBits(8.0f));
    EXPECT_EQ(constants.const1[3], 0u);
}

TEST(SharpenConstants, CasSharpnessZeroIsStillAValidFilterNotAnOff) {
    // 0 is CAS's mildest setting, not a disable. The constants must therefore
    // still describe a real filter; only Mode::Off suppresses the pass.
    const Decision decision = Decide(MakeRequest(Mode::Cas, 0.0f), MakeUsableTarget());
    ASSERT_TRUE(decision.run);
    const ShaderConstants constants = BuildShaderConstants(Mode::Cas, decision, 1920, 1080);
    EXPECT_FLOAT_EQ(FromBits(constants.const1[0]), -1.0f / 8.0f);
}

TEST(SharpenConstants, RcasTransformsStopsToALinearScale) {
    const Decision maxSharp = Decide(MakeRequest(Mode::Rcas, 1.0f), MakeUsableTarget());
    ASSERT_TRUE(maxSharp.run);
    const ShaderConstants sharpConstants = BuildShaderConstants(Mode::Rcas, maxSharp, 1920, 1080);
    EXPECT_FLOAT_EQ(FromBits(sharpConstants.const0[0]), 1.0f);
    EXPECT_EQ(sharpConstants.const0[2], 0u);
    EXPECT_EQ(sharpConstants.const0[3], 0u);

    const Decision mildest = Decide(MakeRequest(Mode::Rcas, 0.0f), MakeUsableTarget());
    ASSERT_TRUE(mildest.run);
    const ShaderConstants mildConstants = BuildShaderConstants(Mode::Rcas, mildest, 1920, 1080);
    EXPECT_FLOAT_EQ(FromBits(mildConstants.const0[0]), std::exp2(-kRcasMaxAttenuation));
    EXPECT_LT(FromBits(mildConstants.const0[0]), FromBits(sharpConstants.const0[0]));
}

TEST(SharpenConstants, EdgeClampCoordinatesAreTheLastValidTexel) {
    const Decision decision = Decide(MakeRequest(Mode::Cas), MakeUsableTarget());
    ASSERT_TRUE(decision.run);
    const ShaderConstants constants = BuildShaderConstants(Mode::Cas, decision, 1920, 1080);
    EXPECT_EQ(constants.maxCoord[0], 1919);
    EXPECT_EQ(constants.maxCoord[1], 1079);
}

TEST(SharpenConstants, FilterSpaceReachesTheShaderBlock) {
    Target target = MakeUsableTarget();
    target.encoding = TargetEncoding::ScrgbLinear;
    const Decision decision = Decide(MakeRequest(Mode::Rcas), target);
    ASSERT_TRUE(decision.run);
    const ShaderConstants constants = BuildShaderConstants(Mode::Rcas, decision, 3840, 2160);
    EXPECT_EQ(constants.filterSpace, static_cast<uint32_t>(FilterSpace::LinearToGamma));
}

TEST(SharpenConstants, ZeroExtentsDoNotUnderflowTheClampCoordinates) {
    Decision decision;
    decision.run = true;
    decision.effectParameter = 0.0f;
    const ShaderConstants constants = BuildShaderConstants(Mode::Cas, decision, 0, 0);
    EXPECT_EQ(constants.maxCoord[0], 0);
    EXPECT_EQ(constants.maxCoord[1], 0);
}

TEST(SharpenPolicy, ClampIntensityBoundsAndRejectsNaN) {
    EXPECT_FLOAT_EQ(ClampIntensity(0.25f), 0.25f);
    EXPECT_FLOAT_EQ(ClampIntensity(-1.0f), kMinIntensity);
    EXPECT_FLOAT_EQ(ClampIntensity(7.0f), kMaxIntensity);
    EXPECT_FLOAT_EQ(ClampIntensity(std::numeric_limits<float>::quiet_NaN()), kMinIntensity);
}

TEST(SharpenPolicy, IntensityDefaultsToTheFullEffect) {
    // An absent intensity must leave the effect at full weight, so adding the
    // control cannot quietly weaken an existing configuration.
    EXPECT_FLOAT_EQ(kDefaultIntensity, kMaxIntensity);
    EXPECT_FLOAT_EQ(Request{}.intensity, kMaxIntensity);
}

TEST(SharpenPolicyDecide, IntensityIsIndependentOfTheEffectParameter) {
    // Strength selects how the kernel reacts to contrast; intensity selects how
    // much of that result reaches the frame. Neither may move the other.
    const Decision quarter = Decide(MakeRequest(Mode::Cas, 0.75f, 0.25f), MakeUsableTarget());
    ASSERT_TRUE(quarter.run);
    EXPECT_FLOAT_EQ(quarter.effectParameter, 0.75f);
    EXPECT_FLOAT_EQ(quarter.intensity, 0.25f);

    const Decision full = Decide(MakeRequest(Mode::Cas, 0.75f, 1.0f), MakeUsableTarget());
    ASSERT_TRUE(full.run);
    EXPECT_FLOAT_EQ(full.effectParameter, quarter.effectParameter);
    EXPECT_FLOAT_EQ(full.intensity, 1.0f);
}

TEST(SharpenPolicyDecide, ZeroIntensitySkipsThePassInsteadOfWritingTheFrameBackUnchanged) {
    // At zero weight the shader would read every pixel and store it unchanged.
    // Under 4x MFG that is four full-screen passes per rendered frame for a
    // result identical to not running at all.
    const Decision decision = Decide(MakeRequest(Mode::Rcas, 0.5f, 0.0f), MakeUsableTarget());
    EXPECT_FALSE(decision.run);
    EXPECT_STREQ(decision.reason, "zero_intensity");
}

TEST(SharpenPolicyDecide, ADisabledModeOutranksAZeroIntensity) {
    // Both would refuse; the reason a session log shows has to be the one the
    // user actually set.
    const Decision decision = Decide(MakeRequest(Mode::Off, 0.5f, 0.0f), MakeUsableTarget());
    EXPECT_FALSE(decision.run);
    EXPECT_STREQ(decision.reason, "disabled");
}

TEST(SharpenConstants, IntensityReachesTheShaderBlock) {
    const Decision decision = Decide(MakeRequest(Mode::Cas, 0.5f, 0.375f), MakeUsableTarget());
    ASSERT_TRUE(decision.run);
    const ShaderConstants constants = BuildShaderConstants(Mode::Cas, decision, 1920, 1080);
    EXPECT_FLOAT_EQ(constants.intensity, 0.375f);
}
