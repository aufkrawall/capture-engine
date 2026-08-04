#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "source_fragment_reader.h"

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
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

TEST(RecordingStartFeedbackSourceTest, RecordingFinalizationEnumsDistinguishAcceptanceFromCompletion) {
    // shared_defs.h is an umbrella over common/shared_defs_detail/; the overlay
    // notification enum lives in the constants/config part.
    const std::string source = ReadSource("common/shared_defs_detail/abi_constants_and_config.h");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("RecordingFinalizing = 3"), std::string::npos);
    EXPECT_NE(source.find("RecordingSaved = 4"), std::string::npos);
    EXPECT_NE(source.find("RecordingSavedDegraded = 5"), std::string::npos);
    EXPECT_NE(source.find("RecordingFailed = 7"), std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, RecordingFinalizationTextInInjectOverlay) {
    const std::string source = ReadSource("hook/common/overlay_adapter.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("\"Finalizing recording...\""), std::string::npos);
    EXPECT_NE(source.find("\"Recording saved\""), std::string::npos);
    EXPECT_NE(source.find("\"Recording saved - video degraded\""), std::string::npos);
    EXPECT_NE(source.find("\"Recording failed\""), std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, RecordingStopNotifPublishedOnVideoStop) {
    const std::string source = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(source.empty());
    const size_t stopLine = source.find("PublishRecordingStartIntent(RecordingStartIntent::Idle, \"record stop hotkey\")");
    ASSERT_NE(stopLine, std::string::npos);
    const size_t notifCall = source.find("ShowRecordingFinalizingNotification()", stopLine);
    EXPECT_NE(notifCall, std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, RecordingStopNotifPublishedOnAudioStop) {
    const std::string source = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(source.empty());
    const size_t stopLine = source.find("PublishRecordingStartIntent(RecordingStartIntent::Idle, \"audio-only stop hotkey\")");
    ASSERT_NE(stopLine, std::string::npos);
    const size_t notifCall = source.find("ShowRecordingFinalizingNotification()", stopLine);
    EXPECT_NE(notifCall, std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, MediaPublishesSavedStateOnlyAfterMuxFinalization) {
    const std::string source = ReadSource("captureengine/media_main.cpp");
    ASSERT_FALSE(source.empty());
    const size_t stop = source.find("MediaEngine_StopRecording(cancelBeforeLive)");
    const size_t complete = source.find("CompleteRecordingFinalization(cancelBeforeLive, outputSaved)", stop);
    ASSERT_NE(stop, std::string::npos);
    ASSERT_NE(complete, std::string::npos);
    EXPECT_LT(stop, complete);
    EXPECT_NE(source.find("OverlayNotificationType::RecordingSavedDegraded"), std::string::npos);
    EXPECT_NE(source.find("OverlayNotificationType::RecordingFailed"), std::string::npos);
    EXPECT_NE(source.find("outputSaved=%d"), std::string::npos);
    EXPECT_NE(source.find("finalizationComplete=1"), std::string::npos);

    const std::string overlay = ReadSource("hook/common/overlay_adapter.cpp");
    ASSERT_FALSE(overlay.empty());
    EXPECT_NE(overlay.find("recordingFinalizationNotification"), std::string::npos);
    EXPECT_NE(overlay.find("recordingState == ce::recording_indicator::State::Idle"), std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, MediaPublishesFailureNotificationOnStartFailure) {
    const std::string source = ReadSource("captureengine/media_main.cpp");
    ASSERT_FALSE(source.empty());
    const size_t publish = source.find("void PublishRecordingStartFailure(");
    ASSERT_NE(publish, std::string::npos);
    EXPECT_NE(source.find("OverlayNotificationType::RecordingFailed", publish), std::string::npos);
    EXPECT_NE(source.find("notificationExpiry.store(GetTickCount64() + 7000ULL", publish), std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, ControllerPublishesFailureNotificationOnFailureCode) {
    const std::string source = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("void PublishRecordingFailureOverlayNotification("), std::string::npos);
    const size_t check = source.find("void CheckRecordingFailureState()");
    ASSERT_NE(check, std::string::npos);
    const size_t notif = source.find("PublishRecordingFailureOverlayNotification(\"recording failure\")", check);
    EXPECT_NE(notif, std::string::npos);
    const size_t reset = source.find("recordingFailureCode.store(", check);
    EXPECT_NE(reset, std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, ControllerPublishesFailureNotificationOnStartAborts) {
    const std::string source = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(source.empty());
    const std::vector<std::string> reasons = {
        "\"media readiness failure\"",
        "\"limiter readiness failure\"",
        "\"inject start command failure\"",
        "\"inject unavailable\"",
        "\"audio-only media readiness failure\"",
        "\"audio-only start command failure\"",
    };
    for (const std::string& reason : reasons) {
        const size_t intent = source.find("PublishRecordingStartIntent(RecordingStartIntent::Idle, " + reason + ")");
        ASSERT_NE(intent, std::string::npos) << reason;
        const size_t notif = source.find("PublishRecordingFailureOverlayNotification(" + reason + ")", intent);
        EXPECT_NE(notif, std::string::npos) << reason;
    }
}

TEST(RecordingStartFeedbackSourceTest, ControllerPublishesFailureNotificationWhenChildDiesBeforeLive) {
    const std::string source = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(source.empty());
    const size_t block = source.find("\"required child exited before recording live\"");
    ASSERT_NE(block, std::string::npos);
    EXPECT_NE(source.find("PublishRecordingFailureOverlayNotification(\"required child exited before recording live\")",
                          block),
              std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, ControllerPublishesFailureNotificationWhenMediaDiesLive) {
    const std::string source = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(source.empty());
    const size_t block = source.find("if (mediaGoneWhileLive) {");
    ASSERT_NE(block, std::string::npos);
    EXPECT_NE(source.find(
                  "PublishRecordingFailureOverlayNotification(\"media process exited while recording live\")",
                  block),
              std::string::npos);
    EXPECT_NE(source.find("runtimeState.isRecording.store(false", block), std::string::npos);
    EXPECT_NE(source.find(
                  "PublishRecordingStartIntent(RecordingStartIntent::Idle, \"media process exited while recording live\")",
                  block),
              std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, MediaStopResultRequiresPublishedOutput) {
    const std::string api = ReadSource("mediaengine/mediaengine.h");
    const std::string loader = ReadSource("captureengine/mediaengine_loader.h");
    const std::string encoder = ReadSource("mediaengine/video_encoder.h");
    ASSERT_FALSE(api.empty());
    ASSERT_FALSE(loader.empty());
    ASSERT_FALSE(encoder.empty());

    EXPECT_NE(api.find("MEDIAENGINE_API bool MediaEngine_StopRecording"), std::string::npos);
    EXPECT_NE(loader.find("typedef bool (*MediaEngine_StopRecording_t)"), std::string::npos);
    EXPECT_NE(encoder.find("WasLastOutputPublished() const"), std::string::npos);
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
