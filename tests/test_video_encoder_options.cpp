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
    config.profile = "high";
    config.qp = 29;
    config.lookahead = true;
    config.aq = true;
    config.bFrames = 4;
    config.bRefMode = "middle";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420");
    const std::optional<std::string> preset = FindOptionValue(plan.generatedOptions, "preset");
    const std::optional<std::string> tune = FindOptionValue(plan.generatedOptions, "tune");
    const std::optional<std::string> rc = FindOptionValue(plan.generatedOptions, "rc");
    const std::optional<std::string> cq = FindOptionValue(plan.generatedOptions, "cq");
    const std::optional<std::string> lookahead = FindOptionValue(plan.generatedOptions, "rc-lookahead");
    const std::optional<std::string> spatialAq = FindOptionValue(plan.generatedOptions, "spatial-aq");
    const std::optional<std::string> temporalAq = FindOptionValue(plan.generatedOptions, "temporal-aq");
    const std::optional<std::string> bRefMode = FindOptionValue(plan.generatedOptions, "b_ref_mode");

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
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "bitrate is ignored"));
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "AV1"));
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
