#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mediaengine/video/video_encoder_options.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace {

using ce::video::BuildEncoderOptionPlan;
using ce::video::EncoderOption;
using ce::video::EncoderOptionPlan;

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

bool EncoderAcceptsOption(const std::string& encoderName, const EncoderOption& option) {
    const AVCodec* codec = avcodec_find_encoder_by_name(encoderName.c_str());
    if (!codec) {
        return false;
    }
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (!context) {
        return false;
    }
    const int result = av_opt_set(context, option.key.c_str(), option.value.c_str(), AV_OPT_SEARCH_CHILDREN);
    avcodec_free_context(&context);
    return result >= 0;
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
    config.splitEncode = "0";
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

TEST(VideoEncoderOptionsTest, AmfUsesNativeVbrQualityAndPreanalysisControls) {
    VideoConfig config = MakeBaseVideoConfig("av1_amf");
    config.bufferSize = "250Mbps";
    config.amfUsage = "high_quality";
    config.amfPreset = "high_quality";
    config.amfAsyncDepth = 12;
    config.amfPreanalysis = "false";  // older config files carry false; lookahead/TAQ still auto-enable PA
    config.amfLookahead = "25";
    config.amfSpatialAq = true;
    config.amfTemporalAq = true;
    config.amfAqStrength = 2;
    config.amfHighMotionQualityBoost = true;
    config.bFrames = 4;

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "rc").value_or(""), "vbr_peak");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "usage").value_or(""), "high_quality");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "preset").value_or(""), "high_quality");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "async_depth").value_or(""), "26");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "preanalysis").value_or(""), "1");
    // Encoder-native AQ and high-motion boost are disabled while PA drives them to avoid AMF conflicts.
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "aq_mode").value_or(""), "none");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "high_motion_quality_boost_enable").value_or(""), "0");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "pa_paq_mode").value_or(""), "caq");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "pa_taq_mode").value_or(""), "2");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "pa_caq_strength").value_or(""), "2");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "pa_lookahead_buffer_depth").value_or(""), "25");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "pa_high_motion_quality_boost_mode").value_or(""), "auto");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "bf").value_or(""), "3");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "max_b_frames").value_or(""), "3");
    EXPECT_EQ(plan.maxBFrames, 3);
    EXPECT_EQ(plan.bitRate.value_or(0), 75000000);
    EXPECT_EQ(plan.maxBitRate.value_or(0), 150000000);
    EXPECT_EQ(plan.bufferSize.value_or(0), 250000000);
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "preanalysis was enabled"));
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "async_depth was increased"));
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "at most 3"));
}

TEST(VideoEncoderOptionsTest, AmfCqAndCqpMapToNativeModes) {
    VideoConfig config = MakeBaseVideoConfig("hevc_amf");
    config.rateControl = "CQ";
    config.amfQp = 19;

    EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "rc").value_or(""), "qvbr");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "qvbr_quality_level").value_or(""), "19");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "preanalysis").value_or(""), "1");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "pa_lookahead_buffer_depth").value_or(""), "0");
    EXPECT_EQ(plan.bitRate.value_or(0), 75000000);
    EXPECT_EQ(plan.maxBitRate.value_or(0), 150000000);
    // preanalysis=auto enables PA silently when CQ/QVBR/HQVBR/HQCBR needs it.
    EXPECT_FALSE(HasMessageContaining(plan.warnings, "preanalysis was enabled"));

    config.amfQp = 0;  // AMF QVBR_QUALITY_LEVEL starts at 1
    plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_TRUE(HasMessageContaining(plan.errors, "between 1 and 51"));

    config.encoder = "av1_amf";
    config.rateControl = "CQP";
    config.amfQp = 180;
    plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "rc").value_or(""), "cqp");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "qp_i").value_or(""), "180");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "qp_p").value_or(""), "180");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "qp_b").value_or(""), "180");
    EXPECT_FALSE(plan.bitRate.has_value());
    EXPECT_FALSE(plan.maxBitRate.has_value());

    config.amfQp = 0;  // AMF AV1 Q_INDEX starts at 1
    plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_TRUE(HasMessageContaining(plan.errors, "out of range"));
}

TEST(VideoEncoderOptionsTest, AmfPreanalysisAvoidsLookaheadDeadlockAndMutuallyExclusiveFlags) {
    // Explicit PA with lookahead=off must still emit pa_lookahead_buffer_depth=0 so AMF does not
    // silently apply its usage default (11) behind a smaller async_depth.
    VideoConfig paNoLookahead = MakeBaseVideoConfig("h264_amf");
    paNoLookahead.rateControl = "HQVBR";
    paNoLookahead.amfAsyncDepth = 4;
    paNoLookahead.amfLookahead = "off";
    EncoderOptionPlan plan = BuildEncoderOptionPlan(paNoLookahead, false, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "rc").value_or(""), "hqvbr");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "preanalysis").value_or(""), "1");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "pa_lookahead_buffer_depth").value_or(""), "0");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "async_depth").value_or(""), "4");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "pa_taq_mode").value_or(""), "none");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "pa_high_motion_quality_boost_mode").value_or(""), "none");

    // Temporal AQ or high_motion_quality_boost with PA requires at least 1 frame of lookahead.
    VideoConfig taqNoLookahead = MakeBaseVideoConfig("h264_amf");
    taqNoLookahead.amfAsyncDepth = 1;
    taqNoLookahead.amfLookahead = "off";
    taqNoLookahead.amfTemporalAq = true;
    plan = BuildEncoderOptionPlan(taqNoLookahead, false, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "preanalysis").value_or(""), "1");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "pa_lookahead_buffer_depth").value_or(""), "1");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "async_depth").value_or(""), "2");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "vbaq").value_or(""), "0");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "pa_taq_mode").value_or(""), "2");
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "pa_lookahead_buffer_depth was set to 1"));
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "async_depth was increased"));

    // Without PA, hardware VBAQ and encoder-native high-motion boost are used with no pa_* options.
    VideoConfig hwOnly = MakeBaseVideoConfig("hevc_amf");
    hwOnly.amfSpatialAq = true;
    hwOnly.amfHighMotionQualityBoost = true;
    plan = BuildEncoderOptionPlan(hwOnly, false, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "preanalysis").value_or(""), "0");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "vbaq").value_or(""), "1");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "high_motion_quality_boost_enable").value_or(""), "1");
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "pa_paq_mode").has_value());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "pa_high_motion_quality_boost_mode").has_value());
}

TEST(VideoEncoderOptionsTest, AmfGuardsIncompatibleCqpLowLatencyCbrAndTenBitCombinations) {
    // CQP disables preencode, PA, lookahead, AQ, high-motion boost, HRD, and filler data.
    VideoConfig cqp = MakeBaseVideoConfig("av1_amf");
    cqp.rateControl = "CQP";
    cqp.amfQp = 100;
    cqp.amfPreencode = true;
    cqp.amfPreanalysis = "enabled";
    cqp.amfLookahead = "16";
    cqp.amfSpatialAq = true;
    cqp.amfTemporalAq = true;
    cqp.amfHighMotionQualityBoost = true;
    cqp.amfEnforceHrd = true;
    cqp.amfFillerData = true;
    EncoderOptionPlan plan = BuildEncoderOptionPlan(cqp, false, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "preencode").value_or(""), "0");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "preanalysis").value_or(""), "0");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "aq_mode").value_or(""), "none");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "high_motion_quality_boost_enable").value_or(""), "0");
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "pa_lookahead_buffer_depth").has_value());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "enforce_hrd").has_value());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "filler_data").has_value());
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "preencode is not supported with rate_control=CQP"));
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "spatial_aq is not supported with rate_control=CQP"));
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "enforce_hrd is not supported with rate_control=CQP"));
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "filler_data is only supported with CBR/HQCBR"));

    // Low-latency usage disables preencode; non-CBR suppresses filler_data.
    VideoConfig lowLatency = MakeBaseVideoConfig("h264_amf");
    lowLatency.rateControl = "VBR";
    lowLatency.amfUsage = "ultralowlatency";
    lowLatency.amfPreencode = true;
    lowLatency.amfFillerData = true;
    plan = BuildEncoderOptionPlan(lowLatency, false, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "preencode").value_or(""), "0");
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "filler_data").has_value());
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "low-latency usage"));
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "CBR/HQCBR"));

    // 10-bit/HDR: PA-dependent rate controls fail closed; optional PA assists degrade cleanly to HW AQ.
    VideoConfig tenBitQvbr = MakeBaseVideoConfig("hevc_amf");
    tenBitQvbr.rateControl = "CQ";
    tenBitQvbr.amfQp = 20;
    plan = BuildEncoderOptionPlan(tenBitQvbr, true, "420", true);
    EXPECT_TRUE(HasMessageContaining(plan.errors, "only supports 8-bit NV12"));

    VideoConfig tenBitVbr = MakeBaseVideoConfig("av1_amf");
    tenBitVbr.rateControl = "VBR";
    tenBitVbr.amfLookahead = "20";
    tenBitVbr.amfSpatialAq = true;
    tenBitVbr.amfTemporalAq = true;
    plan = BuildEncoderOptionPlan(tenBitVbr, true, "420", true);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "preanalysis").value_or(""), "0");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "aq_mode").value_or(""), "caq");
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "pa_lookahead_buffer_depth").has_value());
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "only support 8-bit NV12"));
}

TEST(VideoEncoderOptionsTest, HevcAmfDisablesUnsupportedBFramesAndAcceptsHighQualityPreset) {
    VideoConfig config = MakeBaseVideoConfig("hevc_amf");
    config.bFrames = 3;
    config.amfPreset = "high_quality";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);

    EXPECT_EQ(plan.maxBFrames, 0);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "preset").value_or(""), "high_quality");
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "forcing b_frames=0"));
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "bf").has_value());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "max_b_frames").has_value());
}

TEST(VideoEncoderOptionsTest, AmfAndQuickSyncBFrameOptionMapping) {
    // AMF H.264 and AV1 map b_frames to both "bf" and "max_b_frames" because FFmpeg's
    // amfenc_h264/av1 wrapper reads "bf" into max_b_frames (which sets B_PIC_PATTERN,
    // MAX_CONSECUTIVE_BPICTURES and DTS shift delay) while "max_b_frames" sets
    // max_consecutive_b_frames.
    for (const char* amfEncoder : {"h264_amf", "av1_amf"}) {
        VideoConfig config = MakeBaseVideoConfig(amfEncoder);
        config.bFrames = 2;
        const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
        EXPECT_TRUE(plan.errors.empty()) << amfEncoder;
        EXPECT_EQ(plan.maxBFrames, 2) << amfEncoder;
        EXPECT_EQ(FindOptionValue(plan.generatedOptions, "bf").value_or(""), "2") << amfEncoder;
        EXPECT_EQ(FindOptionValue(plan.generatedOptions, "max_b_frames").value_or(""), "2") << amfEncoder;
    }

    // Quick Sync (H.264, HEVC, AV1) consumes max_b_frames directly via
    // AVCodecContext::max_b_frames in libavcodec/qsvenc.c (setting GopRefDist).
    // It does not use private dictionary options for B-frames.
    for (const char* qsvEncoder : {"h264_qsv", "hevc_qsv", "av1_qsv"}) {
        VideoConfig config = MakeBaseVideoConfig(qsvEncoder);
        config.bFrames = 2;
        const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
        EXPECT_TRUE(plan.errors.empty()) << qsvEncoder;
        EXPECT_EQ(plan.maxBFrames, 2) << qsvEncoder;
        EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "bf").has_value()) << qsvEncoder;
        EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "max_b_frames").has_value()) << qsvEncoder;
    }

    // Custom option override is preserved if user explicitly sets bf
    VideoConfig overrideConfig = MakeBaseVideoConfig("h264_amf");
    overrideConfig.bFrames = 2;
    overrideConfig.customOptions = "bf=1";
    const EncoderOptionPlan overridePlan = BuildEncoderOptionPlan(overrideConfig, false, "420", false);
    EXPECT_TRUE(overridePlan.errors.empty());
    EXPECT_EQ(FindOptionValue(overridePlan.customOptions, "bf").value_or(""), "1");
}

TEST(VideoEncoderOptionsTest, QuickSyncAutoAsyncDepthCoversThePipelineLatencyBudget) {
    // oneVPL's default 4 left ~3 frame intervals for CE's queued conversion
    // before every submit blocked in SyncOperation (the NVENC failure mode).
    EXPECT_EQ(ce::video::ResolveQsvAsyncDepth(120), 12);
    EXPECT_EQ(ce::video::ResolveQsvAsyncDepth(60), 6);
    EXPECT_EQ(ce::video::ResolveQsvAsyncDepth(30), ce::video::kQsvMinAsyncDepth);
    EXPECT_EQ(ce::video::ResolveQsvAsyncDepth(1000), ce::video::kQsvMaxAsyncDepth);
    EXPECT_EQ(ce::video::ResolveQsvAsyncDepth(0), ce::video::kQsvMinAsyncDepth);

    VideoConfig config = MakeBaseVideoConfig("hevc_qsv");
    config.fps = 120;
    config.qsvAsyncDepth = 0;
    EncoderOptionPlan plan = BuildEncoderOptionPlan(config, true, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "async_depth").value_or(""), "12");

    config.qsvAsyncDepth = 3;  // explicit values are honored
    plan = BuildEncoderOptionPlan(config, true, "420", false);
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "async_depth").value_or(""), "3");

    config.qsvAsyncDepth = 65;
    plan = BuildEncoderOptionPlan(config, true, "420", false);
    EXPECT_TRUE(HasMessageContaining(plan.errors, "async_depth"));
}

TEST(VideoEncoderOptionsTest, AmfAutoAsyncDepthKeepsTheDefaultAndCoversHighFrameRates) {
    // AMF blocks a submit once async_depth surfaces are queued; its fixed 16 left
    // only 67 ms at 240 fps for CE's queued conversion. auto never goes below 16.
    EXPECT_EQ(ce::video::ResolveAmfAsyncDepth(120), ce::video::kAmfDefaultAsyncDepth);
    EXPECT_EQ(ce::video::ResolveAmfAsyncDepth(160), ce::video::kAmfDefaultAsyncDepth);
    EXPECT_EQ(ce::video::ResolveAmfAsyncDepth(240), 24);
    EXPECT_EQ(ce::video::ResolveAmfAsyncDepth(1000), ce::video::kAmfMaxAsyncDepth);
    EXPECT_EQ(ce::video::ResolveAmfAsyncDepth(0), ce::video::kAmfDefaultAsyncDepth);

    VideoConfig config = MakeBaseVideoConfig("hevc_amf");
    config.fps = 240;
    config.amfAsyncDepth = 0;
    EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "async_depth").value_or(""), "24");

    config.amfLookahead = "41";  // the lookahead raise still applies on top of auto
    plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "async_depth").value_or(""), "42");
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "async_depth was increased"));

    config.amfLookahead = "off";
    config.amfAsyncDepth = 16;  // an explicit value, as older config files carry, is honored
    plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "async_depth").value_or(""), "16");

    config.amfAsyncDepth = 43;
    plan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_TRUE(HasMessageContaining(plan.errors, "async_depth"));
}

TEST(VideoEncoderOptionsTest, QuickSyncVbrCbrAndCqpUseOneVplContextSemantics) {
    VideoConfig config = MakeBaseVideoConfig("hevc_qsv");
    config.bufferSize = "240Mbps";
    config.qsvPreset = "slow";
    config.qsvLowPower = "disabled";
    config.qsvMbbRc = "enabled";
    config.qsvAdaptiveI = "enabled";
    config.qsvAdaptiveB = "disabled";

    EncoderOptionPlan plan = BuildEncoderOptionPlan(config, true, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "rc").has_value());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "preset").value_or(""), "slow");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "low_power").value_or(""), "0");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "mbbrc").value_or(""), "1");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "adaptive_i").value_or(""), "1");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "adaptive_b").value_or(""), "0");
    EXPECT_EQ(plan.bitRate.value_or(0), 75000000);
    EXPECT_EQ(plan.maxBitRate.value_or(0), 150000000);
    EXPECT_EQ(plan.bufferSize.value_or(0), 240000000);

    config.rateControl = "CBR";
    plan = BuildEncoderOptionPlan(config, true, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(plan.bitRate.value_or(0), 75000000);
    EXPECT_EQ(plan.maxBitRate.value_or(0), 75000000);

    config.encoder = "av1_qsv";
    config.rateControl = "CQP";
    config.qsvQp = 160;
    config.qsvMbbRc = "auto";
    plan = BuildEncoderOptionPlan(config, true, "420", false);
    EXPECT_TRUE(plan.errors.empty());
    EXPECT_TRUE(plan.useConstantQscale);
    EXPECT_TRUE(plan.scaleGlobalQualityByQp2Lambda);
    EXPECT_EQ(plan.globalQuality.value_or(0), 160);
    EXPECT_FALSE(plan.bitRate.has_value());
    EXPECT_FALSE(plan.maxBitRate.has_value());
}

TEST(VideoEncoderOptionsTest, QuickSyncCqAndLookaheadMapToQvbrAndExtBrc) {
    VideoConfig config = MakeBaseVideoConfig("hevc_qsv");
    config.rateControl = "CQ";
    config.qsvQp = 21;
    config.qsvLookahead = "32";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, true, "420", false);

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(plan.globalQuality.value_or(0), 21);
    EXPECT_FALSE(plan.scaleGlobalQualityByQp2Lambda);
    EXPECT_EQ(plan.bitRate.value_or(0), 75000000);
    EXPECT_EQ(plan.maxBitRate.value_or(0), 150000000);
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "extbrc").value_or(""), "1");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "look_ahead_depth").value_or(""), "32");

    config.qsvLookahead = "off";
    config.maxBitrate = config.bitrate;
    const EncoderOptionPlan ambiguousPlan = BuildEncoderOptionPlan(config, true, "420", false);
    EXPECT_TRUE(HasMessageContaining(ambiguousPlan.errors, "infers CBR"));

    config.maxBitrate.clear();
    const EncoderOptionPlan missingMaximumPlan = BuildEncoderOptionPlan(config, true, "420", false);
    EXPECT_TRUE(HasMessageContaining(missingMaximumPlan.errors, "QVBR requires max_bitrate"));
}

TEST(VideoEncoderOptionsTest, QuickSyncIcqIgnoresBitratesAndRejectsHevcLookahead) {
    VideoConfig config = MakeBaseVideoConfig("hevc_qsv");
    config.rateControl = "ICQ";
    config.qsvQp = 20;
    config.qsvLookahead = "12";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, true, "420", false);

    EXPECT_FALSE(plan.bitRate.has_value());
    EXPECT_FALSE(plan.maxBitRate.has_value());
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "ignored when Quick Sync rate_control=ICQ"));
    EXPECT_TRUE(HasMessageContaining(plan.errors, "lookahead is not available with ICQ"));
}

TEST(VideoEncoderOptionsTest, H264QuickSyncLookaheadSelectsLaIcqAndDropsBitrateControls) {
    VideoConfig config = MakeBaseVideoConfig("h264_qsv");
    config.rateControl = "CQ";
    config.qsvLookahead = "auto";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "look_ahead").value_or(""), "1");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "look_ahead_depth").value_or(""), "20");
    EXPECT_FALSE(plan.bitRate.has_value());
    EXPECT_FALSE(plan.maxBitRate.has_value());
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "LA_ICQ"));

    config.maxBitrate.clear();
    const EncoderOptionPlan noMaximumPlan = BuildEncoderOptionPlan(config, false, "420", false);
    EXPECT_TRUE(noMaximumPlan.errors.empty());
    EXPECT_FALSE(noMaximumPlan.bitRate.has_value());
    EXPECT_FALSE(noMaximumPlan.maxBitRate.has_value());
}

TEST(VideoEncoderOptionsTest, QuickSyncRejectsUnsupportedTenBitH264AndLookaheadModes) {
    VideoConfig config = MakeBaseVideoConfig("h264_qsv");
    config.qsvLookahead = "12";
    config.rateControl = "CBR";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, true, "420", false);

    EXPECT_TRUE(HasMessageContaining(plan.errors, "10-bit H.264"));
    EXPECT_TRUE(HasMessageContaining(plan.errors, "lookahead is incompatible"));
}

TEST(VideoEncoderOptionsTest, MediaFoundationUsesValidatedRecordingControls) {
    VideoConfig config = MakeBaseVideoConfig("h264_mf");
    config.bufferSize = "180Mbps";
    config.mfRateControl = "quality";
    config.mfQuality = 84;
    config.mfScenario = "camera_record";
    config.mfHwEncoding = true;
    config.mfQualityVsSpeed = 70;
    config.mfLowLatency = true;
    config.bFrames = 2;

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, false, "420", false);

    EXPECT_TRUE(plan.errors.empty());
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "rate_control").value_or(""), "quality");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "quality").value_or(""), "84");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "scenario").value_or(""), "camera_record");
    EXPECT_EQ(FindOptionValue(plan.generatedOptions, "hw_encoding").value_or(""), "1");
    EXPECT_FALSE(FindOptionValue(plan.generatedOptions, "profile").has_value());
    EXPECT_EQ(plan.codecProfile.value_or(AV_PROFILE_UNKNOWN), AV_PROFILE_H264_HIGH);
    EXPECT_EQ(plan.bufferSize.value_or(0), 180000000);
    EXPECT_EQ(plan.compressionLevel.value_or(-1), 70);
    EXPECT_TRUE(plan.useLowDelay);
    EXPECT_EQ(plan.maxBFrames, 0);
    EXPECT_TRUE(HasMessageContaining(plan.warnings, "forcing b_frames=0"));
}

TEST(VideoEncoderOptionsTest, MediaFoundationRejectsTenBitHdrAndInvalidEnums) {
    VideoConfig config = MakeBaseVideoConfig("hevc_mf");
    config.mfRateControl = "maybe";
    config.mfScenario = "gaming";

    const EncoderOptionPlan plan = BuildEncoderOptionPlan(config, true, "420", true);

    EXPECT_TRUE(HasMessageContaining(plan.errors, "accepts only NV12"));
    EXPECT_TRUE(HasMessageContaining(plan.errors, "rate_control"));
    EXPECT_TRUE(HasMessageContaining(plan.errors, "scenario"));
}

TEST(VideoEncoderOptionsTest, GeneratedHardwareOptionsExistInBundledFfmpeg) {
    struct HardwareOptionCase {
        const char* label;
        VideoConfig config;
        bool use10Bit = false;
    };

    std::vector<HardwareOptionCase> cases;

    VideoConfig h264Amf = MakeBaseVideoConfig("h264_amf");
    h264Amf.rateControl = "VBR";
    h264Amf.amfUsage = "high_quality";
    h264Amf.amfPreset = "quality";
    h264Amf.amfPreanalysis = "enabled";
    h264Amf.amfLookahead = "16";
    h264Amf.amfSpatialAq = true;
    h264Amf.amfTemporalAq = true;
    h264Amf.amfAqStrength = 2;
    h264Amf.amfHighMotionQualityBoost = true;
    h264Amf.amfBRefMode = "enabled";
    h264Amf.amfEnforceHrd = true;
    h264Amf.bFrames = 2;
    cases.push_back({"H.264 AMF (PA)", std::move(h264Amf), false});

    VideoConfig h264AmfCbr = MakeBaseVideoConfig("h264_amf");
    h264AmfCbr.rateControl = "CBR";
    h264AmfCbr.amfUsage = "transcoding";
    h264AmfCbr.amfPreencode = true;
    h264AmfCbr.amfSpatialAq = true;
    h264AmfCbr.amfHighMotionQualityBoost = true;
    h264AmfCbr.amfEnforceHrd = true;
    h264AmfCbr.amfFillerData = true;
    cases.push_back({"H.264 AMF (CBR/HW)", std::move(h264AmfCbr), false});

    VideoConfig hevcAmf = MakeBaseVideoConfig("hevc_amf");
    hevcAmf.rateControl = "CQ";
    hevcAmf.amfPreset = "high_quality";
    hevcAmf.amfQp = 19;
    cases.push_back({"HEVC AMF (8-bit CQ)", std::move(hevcAmf), false});

    VideoConfig hevcAmfHdr = MakeBaseVideoConfig("hevc_amf");
    hevcAmfHdr.rateControl = "VBR";
    hevcAmfHdr.amfPreset = "quality";
    hevcAmfHdr.amfSpatialAq = true;
    hevcAmfHdr.amfHighMotionQualityBoost = true;
    cases.push_back({"HEVC AMF (10-bit VBR)", std::move(hevcAmfHdr), true});

    VideoConfig av1Amf = MakeBaseVideoConfig("av1_amf");
    av1Amf.rateControl = "VBR";
    av1Amf.amfUsage = "high_quality";
    av1Amf.amfPreset = "high_quality";
    av1Amf.amfPreanalysis = "enabled";
    av1Amf.amfSpatialAq = true;
    av1Amf.amfTemporalAq = true;
    av1Amf.amfHighMotionQualityBoost = true;
    av1Amf.bFrames = 3;
    cases.push_back({"AV1 AMF (8-bit PA)", std::move(av1Amf), false});

    VideoConfig av1AmfCqp = MakeBaseVideoConfig("av1_amf");
    av1AmfCqp.rateControl = "CQP";
    av1AmfCqp.amfQp = 180;
    av1AmfCqp.bFrames = 3;
    cases.push_back({"AV1 AMF (10-bit CQP)", std::move(av1AmfCqp), true});

    VideoConfig h264Qsv = MakeBaseVideoConfig("h264_qsv");
    h264Qsv.rateControl = "CQ";
    h264Qsv.qsvLookahead = "20";
    h264Qsv.qsvExtBrc = "enabled";
    h264Qsv.qsvLowPower = "disabled";
    h264Qsv.qsvMbbRc = "enabled";
    h264Qsv.qsvAdaptiveI = "enabled";
    h264Qsv.qsvAdaptiveB = "enabled";
    h264Qsv.qsvLowDelayBrc = "enabled";
    h264Qsv.qsvScenario = "gamestreaming";
    cases.push_back({"H.264 Quick Sync", std::move(h264Qsv), false});

    VideoConfig hevcQsv = MakeBaseVideoConfig("hevc_qsv");
    hevcQsv.rateControl = "CQ";
    hevcQsv.qsvLookahead = "24";
    hevcQsv.qsvMbbRc = "enabled";
    hevcQsv.qsvScenario = "gamestreaming";
    cases.push_back({"HEVC Quick Sync", std::move(hevcQsv), true});

    VideoConfig av1Qsv = MakeBaseVideoConfig("av1_qsv");
    av1Qsv.rateControl = "CQP";
    av1Qsv.qsvQp = 160;
    av1Qsv.qsvLowPower = "enabled";
    av1Qsv.qsvExtBrc = "enabled";
    av1Qsv.qsvAdaptiveI = "enabled";
    av1Qsv.qsvAdaptiveB = "enabled";
    av1Qsv.qsvLowDelayBrc = "enabled";
    cases.push_back({"AV1 Quick Sync", std::move(av1Qsv), true});

    VideoConfig h264Mf = MakeBaseVideoConfig("h264_mf");
    h264Mf.mfRateControl = "quality";
    h264Mf.mfQuality = 82;
    h264Mf.mfScenario = "camera_record";
    cases.push_back({"H.264 Media Foundation", std::move(h264Mf), false});

    VideoConfig hevcMf = MakeBaseVideoConfig("hevc_mf");
    hevcMf.mfRateControl = "quality";
    hevcMf.mfQuality = 82;
    hevcMf.mfScenario = "camera_record";
    cases.push_back({"HEVC Media Foundation", std::move(hevcMf), false});

    VideoConfig av1Mf = MakeBaseVideoConfig("av1_mf");
    av1Mf.mfRateControl = "quality";
    av1Mf.mfQuality = 82;
    av1Mf.mfScenario = "camera_record";
    cases.push_back({"AV1 Media Foundation", std::move(av1Mf), false});

    for (const HardwareOptionCase& optionCase : cases) {
        ASSERT_NE(avcodec_find_encoder_by_name(optionCase.config.encoder.c_str()), nullptr) << optionCase.label;
        const EncoderOptionPlan plan =
            BuildEncoderOptionPlan(optionCase.config, optionCase.use10Bit, "420", optionCase.use10Bit);
        ASSERT_TRUE(plan.errors.empty()) << optionCase.label;
        for (const EncoderOption& option : plan.generatedOptions) {
            EXPECT_TRUE(EncoderAcceptsOption(optionCase.config.encoder, option))
                << optionCase.label << " generated unsupported option '" << option.key << "=" << option.value
                << "'";
        }
    }
}
