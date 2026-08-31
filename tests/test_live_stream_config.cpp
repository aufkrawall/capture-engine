#include "test_config_shared.h"

#include "../common/live_stream_config.h"
#include "../mediaengine/video_encoder_options.h"

#include <limits>

extern "C" {
#include <libavformat/avio.h>
}

TEST(LiveStreamConfigPolicyTest, BundledFfmpegExposesTheMinimalPublishingProtocolClosure) {
    EXPECT_STREQ(avio_find_protocol_name("file:capture.mkv"), "file");
    EXPECT_STREQ(avio_find_protocol_name("rtmp://ingest.example/app/key"), "rtmp");
    EXPECT_STREQ(avio_find_protocol_name("rtmps://ingest.example/app/key"), "rtmps");
    EXPECT_STREQ(avio_find_protocol_name("tcp://ingest.example:1935"), "tcp");
    EXPECT_STREQ(avio_find_protocol_name("tls://ingest.example:443"), "tls");
    EXPECT_EQ(avio_find_protocol_name("http://example.invalid/"), nullptr);
    EXPECT_EQ(avio_find_protocol_name("dtls://127.0.0.1:9000"), nullptr);
    EXPECT_EQ(avio_find_protocol_name("udp://127.0.0.1:9000"), nullptr);
    EXPECT_EQ(avio_find_protocol_name("udplite://127.0.0.1:9000"), nullptr);
}

TEST(LiveStreamConfigPolicyTest, AcceptsRtmpAndRtmpsTargetsWithoutBroadUrlSchemes) {
    EXPECT_TRUE(ce::live_stream::IsValidLiveStreamTarget("rtmp://ingest.example/app/test_key"));
    EXPECT_TRUE(ce::live_stream::IsValidLiveStreamTarget("rtmps://ingest.example/live2/test_key"));
    EXPECT_FALSE(ce::live_stream::IsValidLiveStreamTarget("https://ingest.example/test_key"));
    EXPECT_FALSE(ce::live_stream::IsValidLiveStreamTarget("rtmp://ingest.example"));
    EXPECT_FALSE(ce::live_stream::IsValidLiveStreamTarget("rtmp://ingest.example/app/key with space"));
}

TEST(LiveStreamConfigPolicyTest, BuildsTargetWithoutChangingOpaqueStreamKey) {
    std::string target;
    ASSERT_TRUE(ce::live_stream::BuildLiveStreamTarget("rtmps://ingest.example/app", "key_123-abc", &target));
    EXPECT_EQ(target, "rtmps://ingest.example/app/key_123-abc");

    ASSERT_TRUE(ce::live_stream::BuildLiveStreamTarget("rtmp://ingest.example/app/", "key_123", &target));
    EXPECT_EQ(target, "rtmp://ingest.example/app/key_123");
    EXPECT_FALSE(ce::live_stream::BuildLiveStreamTarget("https://ingest.example/app", "key", &target));
    EXPECT_FALSE(ce::live_stream::BuildLiveStreamTarget("rtmp://ingest.example/app", "", &target));
}

TEST(LiveStreamConfigPolicyTest, CanonicalizesOnlyTheSchemeWithoutChangingCredentials) {
    VideoConfig video{};
    video.encoder = "av1_nvenc";
    video.fps = 60;
    std::vector<AudioConfig> audio;
    ce::live_stream::ProfileSettings settings;
    settings.service = ce::live_stream::Service::kCustom;
    settings.destination = "RTMPS://Ingest.Example/App/CaseSensitive_KEY";

    std::string error;
    ASSERT_TRUE(ce::live_stream::ApplyProfile(settings, &video, &audio, &error)) << error;
    EXPECT_EQ(video.outputDir, "rtmps://Ingest.Example/App/CaseSensitive_KEY");
}

TEST(LiveStreamConfigPolicyTest, AppliesLowLatencyProfileWithoutChangingHardwareBackend) {
    VideoConfig video;
    video.encoder = "hevc_qsv";
    video.fps = 144;
    video.customOptions = "look_ahead=1";
    video.bitDepth = "10";
    video.colorSpace = "bt2020";

    AudioConfig system;
    system.enabled = true;
    system.tracks = {2, 4};
    system.codec = "flac";
    system.sampleRate = "96000";
    std::vector<AudioConfig> audio{system};

    ce::live_stream::ProfileSettings settings;
    settings.service = ce::live_stream::Service::kTwitch;
    settings.destination = "rtmps://ingest.example/app/test_key";

    std::string error;
    ASSERT_TRUE(ce::live_stream::ApplyProfile(settings, &video, &audio, &error)) << error;
    EXPECT_EQ(video.encoder, "h264_qsv");
    EXPECT_EQ(video.fps, 60);
    EXPECT_EQ(video.container, "flv");
    EXPECT_EQ(video.rateControl, "CBR");
    EXPECT_EQ(video.bitrate, "6000Kbps");
    EXPECT_EQ(video.maxBitrate, video.bitrate);
    EXPECT_EQ(video.bufferSize, video.bitrate);
    EXPECT_EQ(video.keyframeInterval, 2);
    EXPECT_EQ(video.profile, "high");
    EXPECT_EQ(video.bFrames, 0);
    EXPECT_EQ(video.qsvAsyncDepth, 2);
    EXPECT_EQ(video.qsvLookahead, "off");
    EXPECT_EQ(video.qsvLowDelayBrc, "enabled");
    EXPECT_EQ(video.qsvScenario, "livestreaming");
    EXPECT_TRUE(video.customOptions.empty());
    EXPECT_EQ(video.bitDepth, "8");
    EXPECT_EQ(video.colorSpace, "bt709");
    EXPECT_FALSE(video.useVFR);

    ASSERT_EQ(audio.size(), 1u);
    EXPECT_EQ(audio[0].tracks, (std::vector<int>{1}));
    EXPECT_EQ(audio[0].codec, "aac");
    EXPECT_EQ(audio[0].bitrate, 160);
    EXPECT_EQ(audio[0].sampleRate, "48000");
    EXPECT_TRUE(audio[0].downmix);
}

TEST(LiveStreamConfigPolicyTest, QueueBudgetAllowsTwoSecondsButStaysBounded) {
    EXPECT_EQ(ce::live_stream::ComputeQueueBudgetBytes(1), 1024u * 1024u);
    EXPECT_EQ(ce::live_stream::ComputeQueueBudgetBytes(8'000'000), 2'000'000u);
    EXPECT_EQ(ce::live_stream::ComputeQueueBudgetBytes(std::numeric_limits<int64_t>::max()), 64u * 1024u * 1024u);
    EXPECT_FALSE(ce::live_stream::ExceedsQueueBudget(900, 100, 1000));
    EXPECT_TRUE(ce::live_stream::ExceedsQueueBudget(901, 100, 1000));
    EXPECT_TRUE(ce::live_stream::ExceedsQueueBudget(std::numeric_limits<size_t>::max(), 1, 1000));
}

TEST(LiveStreamConfigPolicyTest, CompletionNotificationsDoNotClaimThatAStreamWasSavedToDisk) {
    using ce::live_stream::SelectOutputCompletionNotification;
    EXPECT_EQ(SelectOutputCompletionNotification(false, false, true, false),
              OverlayNotificationType::RecordingSaved);
    EXPECT_EQ(SelectOutputCompletionNotification(true, false, true, false),
              OverlayNotificationType::StreamingEnded);
    EXPECT_EQ(SelectOutputCompletionNotification(true, false, true, true),
              OverlayNotificationType::StreamingEndedDegraded);
    EXPECT_EQ(SelectOutputCompletionNotification(true, false, false, false),
              OverlayNotificationType::StreamingFailed);
    EXPECT_EQ(SelectOutputCompletionNotification(true, true, false, false),
              OverlayNotificationType::RecordingCanceled);
}

TEST(LiveStreamConfigPolicyTest, TerminalNetworkErrorsCannotBeReportedAsSuccessfulStreams) {
    using ce::live_stream::IsSuccessfulSession;
    EXPECT_TRUE(IsSuccessfulSession(false, false, 0, 0, 1'000'000, 60));
    EXPECT_FALSE(IsSuccessfulSession(true, false, 0, 0, 1'000'000, 60));
    EXPECT_FALSE(IsSuccessfulSession(false, true, 0, 0, 1'000'000, 60));
    EXPECT_FALSE(IsSuccessfulSession(false, false, -1, 0, 1'000'000, 60));
    EXPECT_FALSE(IsSuccessfulSession(false, false, 0, -1, 1'000'000, 60));
    EXPECT_FALSE(IsSuccessfulSession(false, false, 0, 0, 0, 60));
    EXPECT_FALSE(IsSuccessfulSession(false, false, 0, 0, 1'000'000, 0));
}

TEST(LiveStreamConfigPolicyTest, GeneratedProfileIsAcceptedByEveryHardwareBackendPolicy) {
    static constexpr const char* kConfiguredEncoders[] = {
        "av1_nvenc",
        "hevc_amf",
        "av1_qsv",
        "hevc_mf",
    };

    for (const char* configuredEncoder : kConfiguredEncoders) {
        VideoConfig video{};
        video.encoder = configuredEncoder;
        video.fps = 60;
        AudioConfig system{};
        system.enabled = true;
        std::vector<AudioConfig> audio{system};

        ce::live_stream::ProfileSettings settings;
        settings.service = ce::live_stream::Service::kTwitch;
        settings.destination = "rtmps://ingest.example/app/test_key";
        std::string error;
        ASSERT_TRUE(ce::live_stream::ApplyProfile(settings, &video, &audio, &error))
            << configuredEncoder << ": " << error;

        const ce::video::EncoderOptionPlan plan =
            ce::video::BuildEncoderOptionPlan(video, false, "420", false);
        EXPECT_TRUE(plan.errors.empty()) << configuredEncoder << ": "
                                         << (plan.errors.empty() ? std::string() : plan.errors.front());
        EXPECT_TRUE(plan.isHardwareEncoder) << configuredEncoder;
        EXPECT_EQ(plan.maxBFrames, 0) << configuredEncoder;
        ASSERT_TRUE(plan.bitRate.has_value()) << configuredEncoder;
        EXPECT_EQ(plan.bitRate.value_or(-1), 6'000'000) << configuredEncoder;
    }
}

TEST_F(ConfigTest, TwitchStreamingOverridesIncompatibleRecordingSettings) {
    WriteConfig(R"ini(
[Video]
encoder=av1_nvenc
fps=120
rate_control=VBR
bitrate=125Mbps
bit_depth=10
color_space=bt2020
custom_options=rc-lookahead=12

[Audio]
enabled=true
track=3
codec=flac
sample_rate=96000
downmix=false

[Microphone]
enabled=true
track=4
codec=opus

[Streaming]
enabled=true
service=twitch
server=rtmps://ingest.example/app
stream_key=test_secret_key
)ini");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.video.encoder, "h264_nvenc");
    EXPECT_EQ(config.video.fps, 60);
    EXPECT_EQ(config.video.container, "flv");
    EXPECT_EQ(config.video.outputDir, "rtmps://ingest.example/app/test_secret_key");
    EXPECT_EQ(config.video.tuning, "ull");
    EXPECT_EQ(config.video.multipass, "disabled");
    EXPECT_EQ(config.video.bitDepth, "8");
    ASSERT_EQ(config.audioSources.size(), 2u);
    for (const AudioConfig& audio : config.audioSources) {
        EXPECT_EQ(audio.tracks, (std::vector<int>{1}));
        EXPECT_EQ(audio.codec, "aac");
        EXPECT_EQ(audio.bitrate, 160);
        EXPECT_EQ(audio.sampleRate, "48000");
        EXPECT_TRUE(audio.downmix);
    }
}

TEST_F(ConfigTest, YouTubeStreamingUsesServiceDefaultsAndFullUrlPrecedence) {
    WriteConfig(R"ini(
[Video]
encoder=hevc_amf
fps=30

[Streaming]
enabled=true
service=youtube
url=rtmps://youtube.example/live2/url_key
server=rtmp://ignored.example/app
stream_key=ignored_key
)ini");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.video.encoder, "h264_amf");
    EXPECT_EQ(config.video.fps, 30);
    EXPECT_EQ(config.video.bitrate, "12000Kbps");
    EXPECT_EQ(config.video.outputDir, "rtmps://youtube.example/live2/url_key");
    ASSERT_FALSE(config.audioSources.empty());
    EXPECT_EQ(config.audioSources.front().bitrate, 128);
    EXPECT_EQ(config.video.amfUsage, "ultralowlatency");
    EXPECT_TRUE(config.video.amfEnforceHrd);
    EXPECT_TRUE(config.video.amfFillerData);
}

TEST_F(ConfigTest, InvalidEnabledStreamingNeverFallsBackToLocalRecording) {
    WriteConfig(R"ini(
[Video]
encoder=av1_nvenc

[Streaming]
enabled=true
service=twitch
server=https://not-an-rtmp-endpoint.example/app
stream_key=secret_that_must_not_become_a_path
)ini");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.video.container, "flv");
    EXPECT_EQ(config.video.outputDir, std::string(ce::live_stream::kInvalidTarget));
    EXPECT_TRUE(ce::live_stream::IsLiveStreamTarget(config.video.outputDir));
    EXPECT_FALSE(ce::live_stream::IsValidLiveStreamTarget(config.video.outputDir));
}
