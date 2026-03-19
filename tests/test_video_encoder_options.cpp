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
    config.qp = 23;
    config.lookahead = false;
    config.aq = false;
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
    config.lookahead = true;
    config.aq = true;
    config.bFrames = 4;
    config.bRefMode = "middle";
    config.multipass.clear();  // User didn't set multipass → auto-upgrade

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420");
    const std::optional<std::string> preset = FindOptionValue(plan.generatedOptions, "preset");
    const std::optional<std::string> tune = FindOptionValue(plan.generatedOptions, "tune");
    const std::optional<std::string> rc = FindOptionValue(plan.generatedOptions, "rc");
    const std::optional<std::string> cq = FindOptionValue(plan.generatedOptions, "cq");
    const std::optional<std::string> lookahead = FindOptionValue(plan.generatedOptions, "rc-lookahead");
    const std::optional<std::string> spatialAq = FindOptionValue(plan.generatedOptions, "spatial-aq");
    const std::optional<std::string> temporalAq = FindOptionValue(plan.generatedOptions, "temporal-aq");
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
    ASSERT_TRUE(bRefMode.has_value());
    EXPECT_EQ(*preset, "p3");
    EXPECT_EQ(*tune, "hq");
    EXPECT_EQ(*rc, "vbr");
    EXPECT_EQ(*cq, "29");
    EXPECT_EQ(*lookahead, "32");
    EXPECT_EQ(*spatialAq, "1");
    EXPECT_EQ(*temporalAq, "1");
    EXPECT_EQ(*bRefMode, "middle");
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "qp").has_value());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "profile").has_value());
    EXPECT_FALSE(plan.bitRate.has_value());
    ASSERT_TRUE(plan.maxBitRate.has_value());
    EXPECT_EQ(*plan.maxBitRate, 150000000);
    EXPECT_EQ(plan.maxBFrames, 4);
    EXPECT_TRUE(plan.isHardwareEncoder);
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "bitrate is ignored"));
    // multipass auto-upgraded since B-frames active and multipass=disabled
    ASSERT_TRUE(multipass.has_value());
    EXPECT_EQ(*multipass, "qres");
    // AV1 B-frame QP constraints applied
    ASSERT_TRUE(FindOptionValue(plan.generatedOptions, "qmin").has_value());
    ASSERT_TRUE(FindOptionValue(plan.generatedOptions, "qmax").has_value());
    EXPECT_EQ(*FindOptionValue(plan.generatedOptions, "qmin"), "1");
    EXPECT_EQ(*FindOptionValue(plan.generatedOptions, "qmax"), "200");
}

TEST(VideoEncoderOptionsTest, AutoProfileChoosesCodecAwareDefaults) {
    VideoConfig h264 = MakeBaseVideoConfig("h264_nvenc");
    h264.preset.clear();
    h264.tuning.clear();
    h264.bitrate.clear();
    h264.maxBitrate.clear();
    const EncoderOptionPlan h264Plan = BuildEncoderOptionPlan(h264, false, "420");
    const std::optional<std::string> h264Profile = FindOptionValue(h264Plan.generatedOptions, "profile");
    ASSERT_TRUE(h264Profile.has_value());
    EXPECT_EQ(*h264Profile, "high");

    VideoConfig hevc = MakeBaseVideoConfig("hevc_nvenc");
    hevc.preset.clear();
    hevc.tuning.clear();
    hevc.bitrate.clear();
    hevc.maxBitrate.clear();
    const EncoderOptionPlan hevcPlan = BuildEncoderOptionPlan(hevc, true, "420");
    const std::optional<std::string> hevcProfile = FindOptionValue(hevcPlan.generatedOptions, "profile");
    ASSERT_TRUE(hevcProfile.has_value());
    EXPECT_EQ(*hevcProfile, "main10");

    VideoConfig av1Qsv = MakeBaseVideoConfig("av1_qsv");
    av1Qsv.preset.clear();
    av1Qsv.tuning.clear();
    av1Qsv.bitrate.clear();
    av1Qsv.maxBitrate.clear();
    const EncoderOptionPlan av1QsvPlan = BuildEncoderOptionPlan(av1Qsv, false, "420");
    const std::optional<std::string> av1QsvProfile = FindOptionValue(av1QsvPlan.generatedOptions, "profile");
    ASSERT_TRUE(av1QsvProfile.has_value());
    EXPECT_EQ(*av1QsvProfile, "main");
}

TEST(VideoEncoderOptionsTest, CustomOptionsParseAndValidate) {
    VideoConfig config = MakeBaseVideoConfig("hevc_nvenc");
    config.customOptions = "rc-lookahead=8:spatial-aq=0:foo=bar";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, true, "420");
    ASSERT_TRUE(plan.errors.empty());
    ASSERT_EQ(plan.customOptions.size(), 3u);
    EXPECT_EQ(plan.customOptions[0].key, "rc-lookahead");
    EXPECT_EQ(plan.customOptions[0].value, "8");
    EXPECT_EQ(plan.customOptions[1].key, "spatial-aq");
    EXPECT_EQ(plan.customOptions[1].value, "0");
    EXPECT_EQ(plan.customOptions[2].key, "foo");
    EXPECT_EQ(plan.customOptions[2].value, "bar");

    config.customOptions = "missing_equals";
    const EncoderOptionPlan invalidPlan = BuildEncoderOptionPlan(config, true, "420");
    EXPECT_FALSE(invalidPlan.errors.empty());
    EXPECT_TRUE(HasMessageContaining(invalidPlan.errors, "missing '='"));
}

TEST(VideoEncoderOptionsTest, NvencWeightedPredNotAutoEnabledForH264BFrames) {
    // OBS Studio does not set weighted_pred and works smoothly.
    // We follow the same approach — leave at NVENC defaults.
    VideoConfig config = MakeBaseVideoConfig("h264_nvenc");
    config.bFrames = 4;
    config.bRefMode.clear();

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420");
    const std::optional<std::string> weightedPred = FindOptionValue(plan.generatedOptions, "weighted_pred");

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_FALSE(weightedPred.has_value());
}

TEST(VideoEncoderOptionsTest, NvencWeightedPredSkippedForAV1) {
    // NVENC AV1 does not support weighted_pred (returns ENOSYS), but
    // B-frames themselves are allowed.  multipass should auto-upgrade
    // when user didn't set it explicitly.
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 4;
    config.bRefMode.clear();
    config.multipass.clear();  // User didn't set multipass → auto-upgrade

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420");
    const std::optional<std::string> weightedPred = FindOptionValue(plan.generatedOptions, "weighted_pred");
    const std::optional<std::string> multipass = FindOptionValue(plan.generatedOptions, "multipass");

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_FALSE(weightedPred.has_value());
    EXPECT_EQ(plan.maxBFrames, 4);
    // multipass auto-upgraded from disabled to qres
    ASSERT_TRUE(multipass.has_value());
    EXPECT_EQ(*multipass, "qres");
}

TEST(VideoEncoderOptionsTest, NvencWeightedPredNotSetWhenBFramesZero) {
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 0;
    config.bRefMode.clear();

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420");
    const std::optional<std::string> bRefMode = FindOptionValue(plan.generatedOptions, "b_ref_mode");
    const std::optional<std::string> weightedPred = FindOptionValue(plan.generatedOptions, "weighted_pred");

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_FALSE(bRefMode.has_value());
    EXPECT_FALSE(weightedPred.has_value());
}

TEST(VideoEncoderOptionsTest, NvencExplicitBRefModeDisabledIsRespected) {
    // b_ref_mode=disabled SHOULD appear in options when explicitly set.
    // This prevents FFmpeg's patched NVENC wrapper from auto-enabling
    // b_ref_mode=middle, which would override the user's explicit choice.
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 4;
    config.bRefMode = "disabled";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420");
    const std::optional<std::string> bRefMode = FindOptionValue(plan.generatedOptions, "b_ref_mode");

    EXPECT_TRUE(plan.errors.empty());
    ASSERT_TRUE(bRefMode.has_value());
    EXPECT_EQ(*bRefMode, "disabled");
    EXPECT_EQ(plan.maxBFrames, 4);
}

TEST(VideoEncoderOptionsTest, NvencMultipassAutoUpgradeWithBFrames) {
    // When B-frames are active and user didn't set multipass at all,
    // auto-upgrade to qres for better B-frame bit allocation.
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 4;
    config.multipass.clear();  // User didn't set multipass → auto-upgrade
    config.bRefMode = "middle";
    config.lookahead = true;

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420");
    const std::optional<std::string> multipass = FindOptionValue(plan.generatedOptions, "multipass");

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(plan.maxBFrames, 4);
    ASSERT_TRUE(multipass.has_value());
    EXPECT_EQ(*multipass, "qres");
    EXPECT_FALSE(HasMessageContaining(plan.warnings, "multipass auto-upgraded"));
    // weighted_pred not set (OBS approach)
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "weighted_pred").has_value());
    // b_ref_mode still applied
    EXPECT_TRUE(FindOptionValue(plan.generatedOptions, "b_ref_mode").has_value());
    // AV1 B-frame QP constraints
    EXPECT_EQ(*FindOptionValue(plan.generatedOptions, "qmin"), "1");
    EXPECT_EQ(*FindOptionValue(plan.generatedOptions, "qmax"), "200");
}

TEST(VideoEncoderOptionsTest, NvencMultipassDisabledRespected) {
    // When user explicitly sets multipass=disabled, DON'T auto-upgrade.
    // Respect the user's config.ini choice.
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 4;
    config.multipass = "disabled";  // Explicit user choice

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420");
    const std::optional<std::string> multipass = FindOptionValue(plan.generatedOptions, "multipass");

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_FALSE(multipass.has_value());  // Not emitted → FFmpeg uses its default
    EXPECT_FALSE(HasMessageContaining(plan.warnings, "multipass auto-upgraded"));
}

TEST(VideoEncoderOptionsTest, NvencMultipassNotUpgradedWhenExplicitlySet) {
    // When user explicitly sets multipass=fullres, don't downgrade to qres
    VideoConfig config = MakeBaseVideoConfig("hevc_nvenc");
    config.bFrames = 2;
    config.multipass = "fullres";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420");
    const std::optional<std::string> multipass = FindOptionValue(plan.generatedOptions, "multipass");

    EXPECT_TRUE(plan.errors.empty());
    ASSERT_TRUE(multipass.has_value());
    EXPECT_EQ(*multipass, "fullres");
    EXPECT_FALSE(HasMessageContaining(plan.warnings, "multipass auto-upgraded"));
}

TEST(VideoEncoderOptionsTest, NvencMultipassNotUpgradedWithoutBFrames) {
    // multipass should NOT auto-upgrade when b_frames=0
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 0;
    config.multipass = "disabled";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420");
    const std::optional<std::string> multipass = FindOptionValue(plan.generatedOptions, "multipass");

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(plan.maxBFrames, 0);
    EXPECT_FALSE(multipass.has_value());
    EXPECT_FALSE(HasMessageContaining(plan.warnings, "multipass auto-upgraded"));
}

TEST(VideoEncoderOptionsTest, IsHardwareEncoderFlagSetCorrectly) {
    // NVENC is hardware
    VideoConfig nvenc = MakeBaseVideoConfig("h264_nvenc");
    nvenc.bitrate.clear();
    nvenc.maxBitrate.clear();
    const EncoderOptionPlan nvencPlan = BuildEncoderOptionPlan(nvenc, false, "420");
    EXPECT_TRUE(nvencPlan.isHardwareEncoder);

    // libx264 is software
    VideoConfig sw = MakeBaseVideoConfig("libx264");
    sw.preset.clear();
    sw.tuning.clear();
    sw.bitrate.clear();
    sw.maxBitrate.clear();
    const EncoderOptionPlan swPlan = BuildEncoderOptionPlan(sw, false, "420");
    EXPECT_FALSE(swPlan.isHardwareEncoder);
}

TEST(VideoEncoderOptionsTest, NvencHEVCBFramesMultipassAutoUpgrade) {
    // HEVC NVENC with B-frames and no multipass set → auto-upgrade to qres
    VideoConfig config = MakeBaseVideoConfig("hevc_nvenc");
    config.bFrames = 4;
    config.bRefMode = "middle";
    config.multipass.clear();  // User didn't set multipass → auto-upgrade
    config.lookahead = true;

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420");

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(plan.maxBFrames, 4);
    // weighted_pred NOT set (OBS approach)
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "weighted_pred").has_value());
    EXPECT_TRUE(FindOptionValue(plan.generatedOptions, "b_ref_mode").has_value());
    const std::optional<std::string> multipass = FindOptionValue(plan.generatedOptions, "multipass");
    ASSERT_TRUE(multipass.has_value());
    EXPECT_EQ(*multipass, "qres");
    // HEVC should NOT get qmin/qmax (only AV1 has the 0-255 starvation issue)
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "qmin").has_value());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "qmax").has_value());
}

TEST(VideoEncoderOptionsTest, NvencAV1BFramesGetQPConstraints) {
    // NVENC AV1 with B-frames should get qmin/qmax to prevent the rate
    // controller from pushing leaf B-frame QP to extreme values.
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 4;
    config.bRefMode = "middle";
    config.multipass = "qres";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420");

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(plan.maxBFrames, 4);
    const std::optional<std::string> qmin = FindOptionValue(plan.generatedOptions, "qmin");
    const std::optional<std::string> qmax = FindOptionValue(plan.generatedOptions, "qmax");
    ASSERT_TRUE(qmin.has_value());
    ASSERT_TRUE(qmax.has_value());
    EXPECT_EQ(*qmin, "1");
    EXPECT_EQ(*qmax, "200");
}

TEST(VideoEncoderOptionsTest, NvencAV1NoBFramesNoQPConstraints) {
    // Without B-frames, no qmin/qmax constraints needed.
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 0;

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420");

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(plan.maxBFrames, 0);
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "qmin").has_value());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "qmax").has_value());
}

TEST(VideoEncoderOptionsTest, NvencHEVCBFramesNoQPConstraints) {
    // HEVC has a natural QP range of 0-51, so qmin/qmax constraints are
    // not needed (the starvation issue is AV1-specific with its 0-255 range).
    VideoConfig config = MakeBaseVideoConfig("hevc_nvenc");
    config.bFrames = 4;
    config.bRefMode = "middle";
    config.multipass = "qres";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420");

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "qmin").has_value());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "qmax").has_value());
}

TEST(VideoEncoderOptionsTest, NvencAV1BRefModeDisabledWithBFramesGetsQP) {
    // Even with b_ref_mode=disabled (all leaf B-frames), QP constraints apply.
    // b_ref_mode=disabled IS emitted to prevent FFmpeg auto-enable from overriding.
    VideoConfig config = MakeBaseVideoConfig("av1_nvenc");
    config.bFrames = 2;
    config.bRefMode = "disabled";
    config.multipass = "qres";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420");

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(plan.maxBFrames, 2);
    // b_ref_mode=disabled IS emitted to override FFmpeg's auto-enable
    ASSERT_TRUE(FindOptionValue(plan.generatedOptions, "b_ref_mode").has_value());
    EXPECT_EQ(*FindOptionValue(plan.generatedOptions, "b_ref_mode"), "disabled");
    // QP constraints still apply since AV1 + B-frames
    ASSERT_TRUE(FindOptionValue(plan.generatedOptions, "qmin").has_value());
    ASSERT_TRUE(FindOptionValue(plan.generatedOptions, "qmax").has_value());
}
