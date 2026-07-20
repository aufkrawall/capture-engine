#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "../mediaengine/video_encoder_options.h"

namespace {

using ce::video::BuildEncoderOptionPlan;
using ce::video::EncoderOption;
using ce::video::EncoderOptionPlan;
using ce::video::ParseBitrateString;

std::optional<std::string> FindOptionValue(const std::vector<EncoderOption>& options, const char* key) {
    for (const auto& option : options) {
        if (option.key == key) {
            return option.value;
        }
    }
    return std::nullopt;
}

bool HasMessageContaining(const std::vector<std::string>& messages, const std::string& needle) {
    for (const auto& message : messages) {
        if (message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

VideoConfig MakeBaseVideoConfig(const std::string& encoder) {
    VideoConfig config;
    config.encoder = encoder;
    config.rateControl = "VBR";
    config.bitrate = "75Mbps";
    config.maxBitrate = "150Mbps";
    config.profile = "auto";
    config.preset = "p3";
    config.tuning = "hq";
    config.multipass = "disabled";
    config.splitEncode = "auto";
    config.qp = 23;
    config.lookahead = "off";
    config.spatialAq = false;
    config.temporalAq = false;
    config.aqStrength = 0;
    config.bFrames = 0;
    config.bRefMode = "disabled";
    return config;
}

}  // namespace

TEST(VideoEncoderOptionsTest, ParseBitrateStringSupportsDocumentedFormats) {
    int64_t bitrate = 0;
    std::string error;

    EXPECT_TRUE(ParseBitrateString("100Mbps", &bitrate, &error));
    EXPECT_EQ(bitrate, 100000000);

    EXPECT_TRUE(ParseBitrateString("60000Kbps", &bitrate, &error));
    EXPECT_EQ(bitrate, 60000000);

    EXPECT_TRUE(ParseBitrateString("60000000", &bitrate, &error));
    EXPECT_EQ(bitrate, 60000000);

    EXPECT_TRUE(ParseBitrateString(" 75mbps ", &bitrate, &error));
    EXPECT_EQ(bitrate, 75000000);

    EXPECT_FALSE(ParseBitrateString("abc", &bitrate, &error));
    EXPECT_NE(error.find("integer"), std::string::npos);
}

TEST(VideoEncoderOptionsTest, NvencCQUsesTrueCQAndWiresMissingSettings) {
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.rateControl = "CQ";
    config.bitrate = "100Mbps";
    config.maxBitrate = "150Mbps";
    config.profile = "auto";
    config.qp = 29;
    config.lookahead = "auto";
    config.spatialAq = true;
    config.temporalAq = true;
    config.aqStrength = 7;
    config.bFrames = 4;
    config.bRefMode = "middle";
    config.multipass = "auto";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    const std::optional<std::string> preset = FindOptionValue(plan.generatedOptions, "preset");
    const std::optional<std::string> tune = FindOptionValue(plan.generatedOptions, "tune");
    const std::optional<std::string> rc = FindOptionValue(plan.generatedOptions, "rc");
    const std::optional<std::string> cq = FindOptionValue(plan.generatedOptions, "cq");
    const std::optional<std::string> lookahead = FindOptionValue(plan.generatedOptions, "rc-lookahead");
    const std::optional<std::string> spatialAq = FindOptionValue(plan.generatedOptions, "spatial-aq");
    const std::optional<std::string> temporalAq = FindOptionValue(plan.generatedOptions, "temporal-aq");
    const std::optional<std::string> aqStrength = FindOptionValue(plan.generatedOptions, "aq-strength");
    const std::optional<std::string> bRefMode = FindOptionValue(plan.generatedOptions, "b_ref_mode");
    const std::optional<std::string> multipass = FindOptionValue(plan.generatedOptions, "multipass");

    EXPECT_TRUE(plan.errors.empty());
    ASSERT_TRUE(preset.has_value());
    ASSERT_TRUE(tune.has_value());
    ASSERT_TRUE(rc.has_value());
    ASSERT_TRUE(cq.has_value());
    ASSERT_TRUE(lookahead.has_value());
    ASSERT_TRUE(spatialAq.has_value());
    ASSERT_TRUE(temporalAq.has_value());
    ASSERT_TRUE(aqStrength.has_value());
    ASSERT_TRUE(bRefMode.has_value());
    EXPECT_EQ(preset.value_or(""), "p3");
    EXPECT_EQ(tune.value_or(""), "hq");
    EXPECT_EQ(rc.value_or(""), "vbr");
    EXPECT_EQ(cq.value_or(""), "29");
    EXPECT_EQ(lookahead.value_or(""), "20");
    EXPECT_EQ(spatialAq.value_or(""), "1");
    EXPECT_EQ(temporalAq.value_or(""), "1");
    EXPECT_EQ(aqStrength.value_or(""), "7");
    EXPECT_EQ(bRefMode.value_or(""), "middle");
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "qp").has_value());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "profile").has_value());
    EXPECT_FALSE(plan.bitRate.has_value());
    ASSERT_TRUE(plan.maxBitRate.has_value());
    EXPECT_EQ(plan.maxBitRate.value_or(0), 150000000);
    EXPECT_EQ(plan.maxBFrames, 4);
    EXPECT_TRUE(plan.isHardwareEncoder);
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "bitrate is ignored"));
    ASSERT_TRUE(multipass.has_value());
    EXPECT_EQ(multipass.value_or(""), "qres");
    const std::optional<std::string> maxQpB = FindOptionValue(plan.generatedOptions, "max_qp_b");
    ASSERT_TRUE(maxQpB.has_value());
    EXPECT_EQ(maxQpB.value_or(""), "200");
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "qmin").has_value());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "qmax").has_value());
}

TEST(VideoEncoderOptionsTest, AutoProfileChoosesCodecAwareDefaults) {
    VideoConfig h264 = MakeBaseVideoConfig("h264_nvenc");
    h264.preset.clear();
    h264.tuning.clear();
    h264.bitrate.clear();
    h264.maxBitrate.clear();
    const EncoderOptionPlan h264Plan = BuildEncoderOptionPlan(h264, false, "420", false);
    const std::optional<std::string> h264Profile = FindOptionValue(h264Plan.generatedOptions, "profile");
    ASSERT_TRUE(h264Profile.has_value());
    EXPECT_EQ(h264Profile.value_or(""), "high");

    VideoConfig hevc = MakeBaseVideoConfig("hevc_nvenc");
    hevc.preset.clear();
    hevc.tuning.clear();
    hevc.bitrate.clear();
    hevc.maxBitrate.clear();
    const EncoderOptionPlan hevcPlan = BuildEncoderOptionPlan(hevc, true, "420", false);
    const std::optional<std::string> hevcProfile = FindOptionValue(hevcPlan.generatedOptions, "profile");
    ASSERT_TRUE(hevcProfile.has_value());
    EXPECT_EQ(hevcProfile.value_or(""), "main10");

    VideoConfig av1Qsv = MakeBaseVideoConfig("av1_qsv");
    av1Qsv.preset.clear();
    av1Qsv.tuning.clear();
    av1Qsv.bitrate.clear();
    av1Qsv.maxBitrate.clear();
    const EncoderOptionPlan av1QsvPlan = BuildEncoderOptionPlan(av1Qsv, false, "420", false);
    const std::optional<std::string> av1QsvProfile = FindOptionValue(av1QsvPlan.generatedOptions, "profile");
    ASSERT_TRUE(av1QsvProfile.has_value());
    EXPECT_EQ(av1QsvProfile.value_or(""), "main");
}

TEST(VideoEncoderOptionsTest, CustomOptionsParseAndValidate) {
    VideoConfig config = MakeBaseVideoConfig("hevc_nvenc");
    config.customOptions = "rc-lookahead=8:spatial-aq=0:foo=bar";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, true, "420", false);
    ASSERT_TRUE(plan.errors.empty());
    ASSERT_EQ(plan.customOptions.size(), 3u);
    EXPECT_EQ(plan.customOptions[0].key, "rc-lookahead");
    EXPECT_EQ(plan.customOptions[0].value, "8");
    EXPECT_EQ(plan.customOptions[1].key, "spatial-aq");
    EXPECT_EQ(plan.customOptions[1].value, "0");
    EXPECT_EQ(plan.customOptions[2].key, "foo");
    EXPECT_EQ(plan.customOptions[2].value, "bar");

    config.customOptions = "missing_equals";
    const EncoderOptionPlan invalidPlan = BuildEncoderOptionPlan(config, true, "420", false);
    EXPECT_FALSE(invalidPlan.errors.empty());
    EXPECT_TRUE(HasMessageContaining(invalidPlan.errors, "missing '='"));
}

TEST(VideoEncoderOptionsTest, Av1NvencDisablesUnusedS12mTimecodeAfterCustomOptions) {
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.customOptions = "foo=bar:s12m_tc=1";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, true, "420", false);

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.customOptions, "s12m_tc").value_or(""), "1");
    EXPECT_EQ(FindOptionValue(plan.requiredOptions, "s12m_tc").value_or(""), "0");
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "s12m_tc=1 is overridden to 0"));
}

TEST(VideoEncoderOptionsTest, S12mTimecodeSafetyOverrideIsScopedToAv1Nvenc) {
    VideoConfig av1Nvenc = MakeBaseVideoConfig("av1_nvenc");
    const EncoderOptionPlan av1NvencPlan = BuildEncoderOptionPlan(av1Nvenc, false, "420", false);
    EXPECT_EQ(FindOptionValue(av1NvencPlan.requiredOptions, "s12m_tc").value_or(""), "0");

    for (const char* encoder : {"hevc_nvenc", "h264_nvenc", "av1_amf", "av1_qsv", "av1_mf"}) {
        const EncoderOptionPlan plan = BuildEncoderOptionPlan(MakeBaseVideoConfig(encoder), false, "420", false);
        EXPECT_FALSE(FindOptionValue(plan.requiredOptions, "s12m_tc").has_value()) << encoder;
    }
}

TEST(VideoEncoderOptionsTest, NvencWeightedPredNotAutoEnabledForH264BFrames) {
    // OBS Studio does not set weighted_pred and works smoothly.
    // We follow the same approach — leave at NVENC defaults.
    VideoConfig config = MakeBaseVideoConfig("h264_nvenc");
    config.bFrames = 4;
    config.bRefMode.clear();

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    const std::optional<std::string> weightedPred = FindOptionValue(plan.generatedOptions, "weighted_pred");

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_FALSE(weightedPred.has_value());
}

TEST(VideoEncoderOptionsTest, NvencWeightedPredSkippedForAV1) {
    // NVENC AV1 does not support weighted_pred (returns ENOSYS), but
    // B-frames themselves are allowed. Auto multipass should select qres.
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 4;
    config.bRefMode.clear();
    config.multipass = "auto";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    const std::optional<std::string> weightedPred = FindOptionValue(plan.generatedOptions, "weighted_pred");
    const std::optional<std::string> multipass = FindOptionValue(plan.generatedOptions, "multipass");

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_FALSE(weightedPred.has_value());
    EXPECT_EQ(plan.maxBFrames, 4);
    ASSERT_TRUE(multipass.has_value());
    EXPECT_EQ(multipass.value_or(""), "qres");
}

TEST(VideoEncoderOptionsTest, NvencWeightedPredNotSetWhenBFramesZero) {
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 0;
    config.bRefMode.clear();

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    const std::optional<std::string> bRefMode = FindOptionValue(plan.generatedOptions, "b_ref_mode");
    const std::optional<std::string> weightedPred = FindOptionValue(plan.generatedOptions, "weighted_pred");

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_FALSE(bRefMode.has_value());
    EXPECT_FALSE(weightedPred.has_value());
}

TEST(VideoEncoderOptionsTest, NvencExplicitBRefIsNotSentWithoutBFrames) {
    VideoConfig config = MakeBaseVideoConfig("hevc_nvenc");
    config.bFrames = 0;
    config.bRefMode = "middle";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "b_ref_mode").has_value());
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "ignored when b_frames=0"));
}

TEST(VideoEncoderOptionsTest, NvencExplicitBRefModeDisabledIsRespected) {
    // b_ref_mode=disabled SHOULD appear in options when explicitly set.
    // This prevents FFmpeg's patched NVENC wrapper from auto-enabling
    // b_ref_mode=middle, which would override the user's explicit choice.
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 4;
    config.bRefMode = "disabled";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    const std::optional<std::string> bRefMode = FindOptionValue(plan.generatedOptions, "b_ref_mode");

    EXPECT_TRUE(plan.errors.empty());
    ASSERT_TRUE(bRefMode.has_value());
    EXPECT_EQ(bRefMode.value_or(""), "disabled");
    EXPECT_EQ(plan.maxBFrames, 4);
}

TEST(VideoEncoderOptionsTest, NvencAutoBRefModeIsResolvedByPatchedFfmpeg) {
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 4;
    config.bRefMode = "auto";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "b_ref_mode").has_value());
}

TEST(VideoEncoderOptionsTest, NvencMultipassAutoUpgradeWithBFrames) {
    // Auto uses qres when B-frames need better bit allocation.
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 4;
    config.multipass = "auto";
    config.bRefMode = "middle";
    config.lookahead = "auto";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    const std::optional<std::string> multipass = FindOptionValue(plan.generatedOptions, "multipass");

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(plan.maxBFrames, 4);
    ASSERT_TRUE(multipass.has_value());
    EXPECT_EQ(multipass.value_or(""), "qres");
    // weighted_pred not set (OBS approach)
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "weighted_pred").has_value());
    // b_ref_mode still applied
    EXPECT_TRUE(FindOptionValue(plan.generatedOptions, "b_ref_mode").has_value());
    const std::optional<std::string> maxQpB = FindOptionValue(plan.generatedOptions, "max_qp_b");
    ASSERT_TRUE(maxQpB.has_value());
    EXPECT_EQ(maxQpB.value_or(""), "200");
}

TEST(VideoEncoderOptionsTest, NvencMultipassDisabledRespected) {
    // When user explicitly sets multipass=disabled, DON'T auto-upgrade.
    // Respect the user's config.ini choice.
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 4;
    config.multipass = "disabled";  // Explicit user choice

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    const std::optional<std::string> multipass = FindOptionValue(plan.generatedOptions, "multipass");

    EXPECT_TRUE(plan.errors.empty());
    ASSERT_TRUE(multipass.has_value());
    EXPECT_EQ(multipass.value_or(""), "disabled");
}

TEST(VideoEncoderOptionsTest, NvencMultipassNotUpgradedWhenExplicitlySet) {
    // When user explicitly sets multipass=fullres, don't downgrade to qres
    VideoConfig config = MakeBaseVideoConfig("hevc_nvenc");
    config.bFrames = 2;
    config.multipass = "fullres";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    const std::optional<std::string> multipass = FindOptionValue(plan.generatedOptions, "multipass");

    EXPECT_TRUE(plan.errors.empty());
    ASSERT_TRUE(multipass.has_value());
    EXPECT_EQ(multipass.value_or(""), "fullres");
}

TEST(VideoEncoderOptionsTest, NvencMultipassNotUpgradedWithoutBFrames) {
    // VBR without B-frames does not benefit enough to enable multipass by default.
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 0;
    config.multipass = "auto";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    const std::optional<std::string> multipass = FindOptionValue(plan.generatedOptions, "multipass");

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(plan.maxBFrames, 0);
    ASSERT_TRUE(multipass.has_value());
    EXPECT_EQ(multipass.value_or(""), "disabled");
}

TEST(VideoEncoderOptionsTest, NvencMultipassAutoUsesQresForCbrWithoutBFrames) {
    VideoConfig config = MakeBaseVideoConfig("h264_nvenc");
    config.rateControl = "CBR";
    config.bFrames = 0;
    config.multipass = "auto";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    const std::optional<std::string> multipass = FindOptionValue(plan.generatedOptions, "multipass");

    EXPECT_TRUE(plan.errors.empty());
    ASSERT_TRUE(multipass.has_value());
    EXPECT_EQ(multipass.value_or(""), "qres");
}

TEST(VideoEncoderOptionsTest, NvencSplitEncodeMapsSupportedModesForHevcAndAv1) {
    struct SplitModeCase {
        const char* configured;
        const char* expected;
    };
    const SplitModeCase modes[] = {{" auto ", "auto"}, {"disabled", "disabled"}, {"forced", "forced"},
                                   {"2", "2"},         {"3", "3"},               {"4", "4"}};

    for (const char* encoder : {"hevc_nvenc", "av1_nvenc"}) {
        for (const SplitModeCase& mode : modes) {
            VideoConfig config = MakeBaseVideoConfig(encoder);
            config.splitEncode = mode.configured;

            const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);

            EXPECT_TRUE(plan.errors.empty()) << encoder << " " << mode.configured;
            EXPECT_EQ(FindOptionValue(plan.generatedOptions, "split_encode_mode").value_or(""), mode.expected)
                << encoder << " " << mode.configured;
        }
    }
}

TEST(VideoEncoderOptionsTest, NvencSplitEncodeRejectsForcedModesForH264) {
    VideoConfig config = MakeBaseVideoConfig("h264_nvenc");

    config.splitEncode = "auto";
    EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "split_encode_mode").has_value());

    config.splitEncode = "disabled";
    plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "split_encode_mode").has_value());

    config.splitEncode = "forced";
    plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_TRUE(HasMessageContaining(plan.errors, "supported only for HEVC and AV1"));
}

TEST(VideoEncoderOptionsTest, NvencSplitEncodeCustomOverrideRemainsCompatible) {
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.splitEncode = "auto";
    config.customOptions = "split_encode_mode=3";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "split_encode_mode").value_or(""), "auto");
    EXPECT_EQ(FindOptionValue(plan.customOptions, "split_encode_mode").value_or(""), "3");
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "overrides [NVENC] split_encode=auto"));
}

TEST(VideoEncoderOptionsTest, NvencSplitEncodeRejectsForcedHevcWeightedPredictionAndWarnsForAuto) {
    VideoConfig config = MakeBaseVideoConfig("hevc_nvenc");
    config.splitEncode = "forced";
    config.customOptions = "weighted_pred=1";

    EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_TRUE(HasMessageContaining(plan.errors, "cannot be combined with forced split-frame encoding"));

    config.splitEncode = "auto";
    plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "prevents automatic split-frame encoding"));

    config.splitEncode = "disabled";
    plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_FALSE(HasMessageContaining(plan.warnings, "split-frame encoding"));
}

TEST(VideoEncoderOptionsTest, NvencLookaheadOffAutoAndExplicitDepthAreDeterministic) {
    VideoConfig config = MakeBaseVideoConfig("hevc_nvenc");
    config.bFrames = 4;

    config.lookahead = "off";
    EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "rc-lookahead").value_or(""), "0");

    config.lookahead = "auto";
    plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "rc-lookahead").value_or(""), "20");

    config.lookahead = "12";
    plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "rc-lookahead").value_or(""), "12");
}

TEST(VideoEncoderOptionsTest, NvencLookaheadClampsToBFrameDependentLimit) {
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 4;
    config.lookahead = "31";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    const std::optional<std::string> lookahead = FindOptionValue(plan.generatedOptions, "rc-lookahead");

    EXPECT_TRUE(plan.errors.empty());
    ASSERT_TRUE(lookahead.has_value());
    EXPECT_EQ(lookahead.value_or(""), "27");
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "clamping to 27"));
}

TEST(VideoEncoderOptionsTest, NvencSpatialAndTemporalAqCanBeControlledIndependently) {
    VideoConfig config = MakeBaseVideoConfig("h264_nvenc");
    config.spatialAq = true;
    config.temporalAq = false;
    config.aqStrength = 11;

    EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "spatial-aq").value_or(""), "1");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "temporal-aq").value_or(""), "0");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "aq-strength").value_or(""), "11");

    config.spatialAq = false;
    config.temporalAq = true;
    plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "spatial-aq").value_or(""), "0");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "temporal-aq").value_or(""), "1");
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "aq-strength").has_value());
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "aq_strength is ignored"));
}

TEST(VideoEncoderOptionsTest, InvalidNvencPolicyValuesAreRejected) {
    VideoConfig config = MakeBaseVideoConfig("h264_nvenc");
    config.lookahead = "many";
    config.multipass = "sometimes";
    config.splitEncode = "wide";
    config.bRefMode = "all";
    config.aqStrength = 16;

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);

    EXPECT_TRUE(HasMessageContaining(plan.errors, "lookahead"));
    EXPECT_TRUE(HasMessageContaining(plan.errors, "multipass"));
    EXPECT_TRUE(HasMessageContaining(plan.errors, "split_encode"));
    EXPECT_TRUE(HasMessageContaining(plan.errors, "b_ref_mode"));
    EXPECT_TRUE(HasMessageContaining(plan.errors, "aq_strength"));
}

TEST(VideoEncoderOptionsTest, IsHardwareEncoderFlagSetCorrectly) {
    // NVENC is hardware
    VideoConfig nvenc = MakeBaseVideoConfig("h264_nvenc");
    nvenc.bitrate.clear();
    nvenc.maxBitrate.clear();
    const EncoderOptionPlan nvencPlan = BuildEncoderOptionPlan(nvenc, false, "420", false);
    EXPECT_TRUE(nvencPlan.isHardwareEncoder);

    // libx264 is software
    VideoConfig sw = MakeBaseVideoConfig("libx264");
    sw.preset.clear();
    sw.tuning.clear();
    sw.bitrate.clear();
    sw.maxBitrate.clear();
    const EncoderOptionPlan swPlan = BuildEncoderOptionPlan(sw, false, "420", false);
    EXPECT_FALSE(swPlan.isHardwareEncoder);
}

TEST(VideoEncoderOptionsTest, NvencHEVCBFramesMultipassAutoUpgrade) {
    // HEVC NVENC with B-frames selects qres in auto mode.
    VideoConfig config = MakeBaseVideoConfig("hevc_nvenc");
    config.bFrames = 4;
    config.bRefMode = "middle";
    config.multipass = "auto";
    config.lookahead = "auto";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(plan.maxBFrames, 4);
    // weighted_pred NOT set (OBS approach)
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "weighted_pred").has_value());
    EXPECT_TRUE(FindOptionValue(plan.generatedOptions, "b_ref_mode").has_value());
    const std::optional<std::string> multipass = FindOptionValue(plan.generatedOptions, "multipass");
    ASSERT_TRUE(multipass.has_value());
    EXPECT_EQ(multipass.value_or(""), "qres");
    // HEVC should NOT get qmin/qmax (only AV1 has the 0-255 starvation issue)
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "qmin").has_value());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "qmax").has_value());
}

TEST(VideoEncoderOptionsTest, NvencAV1BFramesGetQPConstraints) {
    // Bound only B-frame QP; global qmin/qmax would also alter I/P policy.
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 4;
    config.bRefMode = "middle";
    config.multipass = "qres";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(plan.maxBFrames, 4);
    const std::optional<std::string> maxQpB = FindOptionValue(plan.generatedOptions, "max_qp_b");
    ASSERT_TRUE(maxQpB.has_value());
    EXPECT_EQ(maxQpB.value_or(""), "200");
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "qmin").has_value());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "qmax").has_value());
}

TEST(VideoEncoderOptionsTest, NvencAV1NoBFramesNoQPConstraints) {
    // Without B-frames, no B-frame-only constraint is needed.
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 0;

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(plan.maxBFrames, 0);
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "max_qp_b").has_value());
}

TEST(VideoEncoderOptionsTest, NvencAV1CqpDoesNotApplyRateControlQpBound) {
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.rateControl = "CQP";
    config.qp = 180;
    config.bFrames = 4;

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "qp").value_or(""), "180");
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "max_qp_b").has_value());
}

TEST(VideoEncoderOptionsTest, NvencHEVCBFramesNoQPConstraints) {
    // HEVC has a natural QP range of 0-51, so qmin/qmax constraints are
    // not needed (the starvation issue is AV1-specific with its 0-255 range).
    VideoConfig config = MakeBaseVideoConfig("hevc_nvenc");
    config.bFrames = 4;
    config.bRefMode = "middle";
    config.multipass = "qres";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "max_qp_b").has_value());
}

TEST(VideoEncoderOptionsTest, NvencAV1BRefModeDisabledWithBFramesGetsQP) {
    // Even with b_ref_mode=disabled (all leaf B-frames), QP constraints apply.
    // b_ref_mode=disabled IS emitted to prevent FFmpeg auto-enable from overriding.
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 2;
    config.bRefMode = "disabled";
    config.multipass = "qres";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(plan.maxBFrames, 2);
    // b_ref_mode=disabled IS emitted to override FFmpeg's auto-enable
    ASSERT_TRUE(FindOptionValue(plan.generatedOptions, "b_ref_mode").has_value());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "b_ref_mode").value_or(""), "disabled");
    // B-frame QP bound still applies since this is AV1 with B-frames.
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "max_qp_b").value_or(""), "200");
}
