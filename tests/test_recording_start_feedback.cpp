#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    std::ifstream stream(std::filesystem::current_path() / relativePath, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(RecordingStartFeedbackSourceTest, ControllerPublishesIntentBeforeReadinessWaits) {
    const std::string source = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(source.empty());

    const size_t videoToggle = source.find("void ToggleRecording()");
    const size_t videoIntent = source.find(
        "PublishRecordingStartIntent(RecordingStartIntent::Video, \"record hotkey\")", videoToggle);
    const size_t videoReady = source.find("EnsureMediaProcessReady(10000)", videoToggle);
    ASSERT_NE(videoToggle, std::string::npos);
    ASSERT_NE(videoIntent, std::string::npos);
    ASSERT_NE(videoReady, std::string::npos);
    EXPECT_LT(videoIntent, videoReady);

    const size_t audioToggle = source.find("void ToggleAudioOnlyRecording()");
    const size_t audioIntent = source.find(
        "PublishRecordingStartIntent(RecordingStartIntent::AudioOnly, \"audio-only hotkey\")", audioToggle);
    const size_t audioReady = source.find("EnsureMediaProcessReady(10000)", audioToggle);
    ASSERT_NE(audioToggle, std::string::npos);
    ASSERT_NE(audioIntent, std::string::npos);
    ASSERT_NE(audioReady, std::string::npos);
    EXPECT_LT(audioIntent, audioReady);
}

TEST(RecordingStartFeedbackSourceTest, ControllerClearsIntentOnEveryOwnedTerminalClass) {
    const std::string source = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("\"media readiness failure\""), std::string::npos);
    EXPECT_NE(source.find("\"limiter readiness failure\""), std::string::npos);
    EXPECT_NE(source.find("\"inject start command failure\""), std::string::npos);
    EXPECT_NE(source.find("\"inject unavailable\""), std::string::npos);
    EXPECT_NE(source.find("\"audio-only media readiness failure\""), std::string::npos);
    EXPECT_NE(source.find("\"audio-only start command failure\""), std::string::npos);
    EXPECT_NE(source.find("\"record stop hotkey\""), std::string::npos);
    EXPECT_NE(source.find("\"audio-only stop hotkey\""), std::string::npos);
    EXPECT_NE(source.find("\"required child exited before recording live\""), std::string::npos);
    EXPECT_NE(source.find("\"controller shutdown\""), std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, MediaOwnsLiveAndTerminalIntentTransitions) {
    const std::string source = ReadSource("captureengine/media_main.cpp");
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("bool StartRecording(const AppConfig& config)"), std::string::npos);
    EXPECT_NE(source.find("runtimeState.SetRecordingStartIntent(RecordingStartIntent::Idle)"), std::string::npos);
    EXPECT_NE(source.find("PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed"),
              std::string::npos);
    EXPECT_NE(source.find("\"WGC deferred encoder/mux initialization\""), std::string::npos);
    EXPECT_NE(source.find("const bool started = StartRecording(config)"), std::string::npos);
    EXPECT_NE(source.find("ackRecordingStarted.store(true"), std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, WarmupStopIsAcceptedAsCancellationBeforeLiveCommit) {
    const std::string captureSource = ReadSource("captureengine/media_main.cpp");
    const std::string mediaSource = ReadSource("mediaengine/mediaengine.cpp");
    ASSERT_FALSE(captureSource.empty());
    ASSERT_FALSE(mediaSource.empty());

    EXPECT_NE(captureSource.find("TryArmCapturePipelineWarmup()"), std::string::npos);
    EXPECT_NE(captureSource.find("TryCommitCapturePipelineLive()"), std::string::npos);
    EXPECT_NE(captureSource.find("BeginCapturePipelineStop()"), std::string::npos);
    EXPECT_NE(captureSource.find("MediaEngine_StopRecording(cancelBeforeLive)"), std::string::npos);
    EXPECT_NE(mediaSource.find("CancelUncommittedVideoRecording()"), std::string::npos);
    EXPECT_NE(mediaSource.find("videoEnc->Cancel()"), std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, VideoOutputStaysStagedUntilSuccessfulContentGatedPublication) {
    const std::string source = ReadSource("mediaengine/video_encoder.cpp");
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("ReserveOutputStagingFile"), std::string::npos);
    EXPECT_NE(source.find("SelectVideoOutputDisposition"), std::string::npos);
    EXPECT_NE(source.find("PublishToNewPath"), std::string::npos);
    EXPECT_NE(source.find("output_discarded"), std::string::npos);
    EXPECT_NE(source.find("output_published"), std::string::npos);

    const size_t init = source.find("bool VideoEncoder::Init(");
    const size_t start = source.find("bool VideoEncoder::Start()");
    const size_t reserveForRecording = source.find("outputReservation = ReserveOutputStagingFile(savedConfig)");
    ASSERT_NE(init, std::string::npos);
    ASSERT_NE(start, std::string::npos);
    ASSERT_NE(reserveForRecording, std::string::npos);
    EXPECT_LT(init, start);
    EXPECT_LT(start, reserveForRecording);
}

TEST(RecordingStartFeedbackSourceTest, InjectOverlayContainsExactPendingLabelsAndPseudoOwnsUiThread) {
    const std::string overlay = ReadSource("hook/common/overlay_adapter.cpp");
    const std::string pseudo = ReadSource("captureengine/pseudo_overlay.cpp");
    ASSERT_FALSE(overlay.empty());
    ASSERT_FALSE(pseudo.empty());

    EXPECT_NE(overlay.find("STARTING RECORDING..."), std::string::npos);
    EXPECT_NE(overlay.find("STARTING AUDIO..."), std::string::npos);
    EXPECT_NE(overlay.find("Colors::LabelYellow"), std::string::npos);
    EXPECT_NE(overlay.find("frameLayout.recordingState != lastFrameLayout.recordingState"), std::string::npos);
    EXPECT_NE(overlay.find("MeasureTextWidth(recBuf) + kShadowPad"), std::string::npos);
    EXPECT_NE(pseudo.find("uiThread_ = std::thread([this]() { ThreadMain(); })"), std::string::npos);
    EXPECT_NE(pseudo.find("bool PseudoOverlay::InitializeOnUiThread()"), std::string::npos);
    EXPECT_NE(pseudo.find("PostThreadMessageW(threadId, kMsgRefresh"), std::string::npos);
    EXPECT_NE(pseudo.find("kColStarting"), std::string::npos);
}
