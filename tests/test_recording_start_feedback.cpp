#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
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

    const size_t videoToggle = source.find("void ToggleRecording() {");
    const size_t videoIntent = source.find(
        "PublishRecordingStartIntent(RecordingStartIntent::Video, \"record hotkey\")", videoToggle);
    const size_t videoReady = source.find("EnsureMediaProcessReady(10000)", videoToggle);
    ASSERT_NE(videoToggle, std::string::npos);
    ASSERT_NE(videoIntent, std::string::npos);
    ASSERT_NE(videoReady, std::string::npos);
    EXPECT_LT(videoIntent, videoReady);

    const size_t audioToggle = source.find("void ToggleAudioOnlyRecording() {");
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

// Regression: a screen-grab recording captures the composited desktop, so CE's own
// recording-start status has to be off screen before the capture pipeline starts. Doing it
// when file output goes live is a full look-ahead reservoir too late, which is exactly how
// "STARTING RECORDING..." ended up burned into the first frames of recorded files.
TEST(RecordingStartFeedbackSourceTest, MediaTakesTheStatusOverlayDarkBeforeScreenGrabCaptureStarts) {
    const std::string source = ReadSource("captureengine/media_main.cpp");
    ASSERT_FALSE(source.empty());

    const size_t startRecording = source.find("bool StartRecording(const AppConfig& config)");
    ASSERT_NE(startRecording, std::string::npos);
    const size_t darkRequest =
        source.find("RequestStatusOverlayDarkForCapture(\"screen-grab capture start\")", startRecording);
    const size_t encoderThread = source.find("EncoderThreadFunc(*configSnapshot)", startRecording);
    const size_t wgcCaptureStart = source.find("StartWgcRecordingCapture(config)", startRecording);
    ASSERT_NE(darkRequest, std::string::npos);
    ASSERT_NE(encoderThread, std::string::npos);
    ASSERT_NE(wgcCaptureStart, std::string::npos);
    EXPECT_LT(darkRequest, encoderThread);
    EXPECT_LT(darkRequest, wgcCaptureStart);

    // The request is bounded and fail-open: a missing or unresponsive consumer must never
    // block a recording start.
    const std::string protocol = ReadSource("captureengine/status_overlay_sync.cpp");
    ASSERT_FALSE(protocol.empty());
    EXPECT_NE(protocol.find("WaitForSingleObject(ackEvent, kDarkAckTimeoutMs)"), std::string::npos);
    EXPECT_NE(protocol.find("No controller status consumer"), std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, MediaReleasesTheCaptureDarkRequestOnEveryStatusPublication) {
    const std::string source = ReadSource("captureengine/media_main.cpp");
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("ReleaseStatusOverlayDarkForCapture(\"recording live\")"), std::string::npos);
    EXPECT_NE(source.find("ReleaseStatusOverlayDarkForCapture(\"recording not live\")"), std::string::npos);
    EXPECT_NE(source.find("ReleaseStatusOverlayDarkForCapture(\"recording start failure\")"), std::string::npos);
    // Media wakes the controller-side overlay on every status publication so the live REC
    // state does not wait for the overlay's next poll either.
    EXPECT_NE(source.find("SignalStatusOverlaySync()"), std::string::npos);

    const std::string overlay = ReadSource("hook/common/overlay_adapter.cpp");
    ASSERT_FALSE(overlay.empty());
    EXPECT_NE(overlay.find("kCaptureRuntimeFlagStatusOverlayDarkForCapture"), std::string::npos);
    EXPECT_NE(overlay.find("frameLayout.recordingStatusDark"), std::string::npos);
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
    EXPECT_NE(source.find("\"Stream ended\""), std::string::npos);
    EXPECT_NE(source.find("\"Stream ended - video degraded\""), std::string::npos);
    EXPECT_NE(source.find("\"Stream failed\""), std::string::npos);
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
    EXPECT_NE(source.find("SelectOutputCompletionNotification"), std::string::npos);
    EXPECT_NE(source.find("outputSaved=%d"), std::string::npos);
    EXPECT_NE(source.find("finalizationComplete=1"), std::string::npos);

    const std::string completionPolicy = ReadSource("common/live_stream_config.cpp");
    ASSERT_FALSE(completionPolicy.empty());
    EXPECT_NE(completionPolicy.find("OverlayNotificationType::RecordingSavedDegraded"), std::string::npos);
    EXPECT_NE(completionPolicy.find("OverlayNotificationType::RecordingFailed"), std::string::npos);
    EXPECT_NE(completionPolicy.find("OverlayNotificationType::StreamingEndedDegraded"), std::string::npos);
    EXPECT_NE(completionPolicy.find("OverlayNotificationType::StreamingFailed"), std::string::npos);

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
    EXPECT_NE(source.find("OverlayNotificationType::StreamingFailed", publish), std::string::npos);
    EXPECT_NE(source.find("notificationExpiry.store(GetTickCount64() + 7000ULL", publish), std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, ControllerPublishesFailureNotificationOnFailureCode) {
    const std::string source = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("void PublishRecordingFailureOverlayNotification("), std::string::npos);
    const size_t check = source.find("void CheckRecordingFailureState()");
    ASSERT_NE(check, std::string::npos);
    const size_t notif = source.find(
        "PublishRecordingFailureOverlayNotification(\"recording failure\", IsControllerLiveStreamOutput())",
        check);
    EXPECT_NE(notif, std::string::npos);
    const size_t reset = source.find("recordingFailureCode.store(", check);
    EXPECT_NE(reset, std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, ControllerPublishesFailureNotificationOnStartAborts) {
    const std::string source = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(source.empty());
    const std::vector<std::pair<std::string, bool>> cases = {
        {"\"media readiness failure\"", true},
        {"\"limiter readiness failure\"", true},
        {"\"inject start command failure\"", true},
        {"\"inject unavailable\"", true},
        {"\"audio-only media readiness failure\"", false},
        {"\"audio-only start command failure\"", false},
    };
    for (const auto& [reason, streamingAware] : cases) {
        const size_t intent = source.find("PublishRecordingStartIntent(RecordingStartIntent::Idle, " + reason + ")");
        ASSERT_NE(intent, std::string::npos) << reason;
        const size_t notif = source.find("PublishRecordingFailureOverlayNotification(", intent);
        ASSERT_NE(notif, std::string::npos) << reason;
        const size_t callEnd = source.find(");", notif);
        ASSERT_NE(callEnd, std::string::npos) << reason;
        const std::string call = source.substr(notif, callEnd + 2 - notif);
        EXPECT_NE(call.find(reason), std::string::npos) << reason;
        if (streamingAware) {
            EXPECT_NE(call.find("IsControllerLiveStreamOutput()"), std::string::npos) << reason;
        } else {
            EXPECT_EQ(call.find("IsControllerLiveStreamOutput()"), std::string::npos) << reason;
        }
    }
}

TEST(RecordingStartFeedbackSourceTest, ControllerPublishesFailureNotificationWhenChildDiesBeforeLive) {
    const std::string source = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(source.empty());
    const size_t block = source.find("\"required child exited before recording live\"");
    ASSERT_NE(block, std::string::npos);
    const size_t notification = source.find("PublishRecordingFailureOverlayNotification(", block);
    ASSERT_NE(notification, std::string::npos);
    EXPECT_NE(source.find("\"required child exited before recording live\"", notification), std::string::npos);
    EXPECT_NE(source.find("recordingStartIntent == RecordingStartIntent::Video && IsControllerLiveStreamOutput()",
                          notification),
              std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, ControllerPublishesFailureNotificationWhenMediaDiesLive) {
    const std::string source = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(source.empty());
    const size_t block = source.find("if (mediaGoneWhileLive) {");
    ASSERT_NE(block, std::string::npos);
    const size_t notification = source.find("PublishRecordingFailureOverlayNotification(", block);
    ASSERT_NE(notification, std::string::npos);
    EXPECT_NE(source.find("\"media process exited while recording live\"", notification), std::string::npos);
    EXPECT_NE(source.find("!recordingLiveAudioOnly && IsControllerLiveStreamOutput()", notification),
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

// Regression (session 20260918_235601, r0003/r0004): the recording hotkey spawns the media
// process, which then needs seconds to become live (render->loopback probe, engine init, capture
// routing). A stop inside that window discarded the queued start and exited silently:
// CompleteRecordingFinalization - the only publisher of a terminal overlay notification - was
// never reached, so the controller's "Finalizing recording..." (60 s expiry) stayed on screen and
// the recording manifest kept no finalization record, for a recording that produced no file.
TEST(RecordingStartFeedbackSourceTest, MediaFinalizesAStopThatArrivedBeforeTheRecordingStarted) {
    const std::string source = ReadSource("captureengine/media_main.cpp");
    ASSERT_FALSE(source.empty());

    // The latch is what distinguishes an aborted start from an ordinary finalization: it is set
    // exactly where a recording becomes live.
    const size_t startRecording = source.find("bool StartRecording(const AppConfig& config)");
    ASSERT_NE(startRecording, std::string::npos);
    const size_t latch = source.find("media_main_g_RecordingEverStarted.store(true", startRecording);
    ASSERT_NE(latch, std::string::npos);

    const size_t helper = source.find("void CompleteAbortedRecordingStart(const char* reason)");
    ASSERT_NE(helper, std::string::npos);
    const size_t guard = source.find("media_main_g_RecordingEverStarted.load", helper);
    ASSERT_NE(guard, std::string::npos) << "a live recording must finalize normally, not as canceled";
    const size_t canceled =
        source.find("CompleteRecordingFinalization(true /*canceled*/, false /*outputSaved*/)", guard);
    EXPECT_NE(canceled, std::string::npos);

    // Both stop routes reach it: the authenticated media channel and the shared-memory command.
    EXPECT_NE(source.find("CompleteAbortedRecordingStart(\"authenticated stop request\")"), std::string::npos);
    EXPECT_NE(source.find("CompleteAbortedRecordingStart(\"shared-memory stop request\")"), std::string::npos);
}

// CompleteRecordingFinalization suppresses its notification when a newer recording is already
// active, so the aborted-start path must clear the hook-facing state it never owned first -
// otherwise the cancellation would be swallowed exactly like the silent exit it replaces.
TEST(RecordingStartFeedbackSourceTest, AbortedStartClearsHookFacingStateBeforeFinalizing) {
    const std::string source = ReadSource("captureengine/media_main.cpp");
    ASSERT_FALSE(source.empty());

    const size_t sharedMemoryAbort = source.find("CompleteAbortedRecordingStart(\"shared-memory stop request\")");
    ASSERT_NE(sharedMemoryAbort, std::string::npos);
    const size_t intentCleared =
        source.rfind("SetRecordingStartIntent(RecordingStartIntent::Idle)", sharedMemoryAbort);
    const size_t captureCleared = source.rfind("SetCaptureRequestedState(false)", sharedMemoryAbort);
    const size_t visibleCleared = source.rfind("SetRecordingVisibleState(false)", sharedMemoryAbort);
    ASSERT_NE(intentCleared, std::string::npos);
    ASSERT_NE(captureCleared, std::string::npos);
    ASSERT_NE(visibleCleared, std::string::npos);
    EXPECT_LT(intentCleared, sharedMemoryAbort);
    EXPECT_LT(captureCleared, sharedMemoryAbort);
    EXPECT_LT(visibleCleared, sharedMemoryAbort);

    // A canceled finalization is recorded in the manifest as such, so the evidence for a
    // recording that produced nothing is no longer just a missing line.
    const std::string manifest = ReadSource("captureengine/recording_manifest.h");
    ASSERT_FALSE(manifest.empty());
    EXPECT_NE(manifest.find("recording_canceled"), std::string::npos);
}

// The inject ack only proves inject set cmdStartRecording; the media process may still be
// seconds away from a live recording. Logging "Recording started" there reported a recording
// that did not exist and, in the aborted case, never would.
TEST(RecordingStartFeedbackSourceTest, ControllerReportsRecordingLiveOnlyWhenMediaPublishesIt) {
    const std::string source = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(source.empty());

    EXPECT_EQ(source.find("LogInfo(\"[Controller] Recording started\")"), std::string::npos)
        << "the inject command ack is not evidence that a recording started";
    EXPECT_NE(source.find("Recording start request delivered to inject"), std::string::npos);

    // The truthful transition is owned by the health check, which observes the media process's
    // published isRecording state.
    const size_t health = source.find("void CheckChildProcessHealth()");
    ASSERT_NE(health, std::string::npos);
    const size_t isRecording = source.find("runtimeState.isRecording.load(std::memory_order_acquire)", health);
    ASSERT_NE(isRecording, std::string::npos);
    const size_t live = source.find("Recording is live", isRecording);
    EXPECT_NE(live, std::string::npos);
}

// The controller's own evidence for the aborted case: how long the start had been pending when
// the stop arrived. The tick is armed on every start and cleared by every idle transition so it
// cannot leak into a later recording.
TEST(RecordingStartFeedbackSourceTest, ControllerReportsAStopInsideTheMediaStartupWindow) {
    const std::string source = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("main_g_RecordingStartRequestTick.store(GetTickCount64()"), std::string::npos);
    EXPECT_NE(source.find("before the recording went live"), std::string::npos);
    EXPECT_NE(source.find("Audio-only stop requested"), std::string::npos);

    const size_t publish = source.find("inline bool PublishRecordingStartIntent(");
    ASSERT_NE(publish, std::string::npos);
    const size_t idleClear = source.find("main_g_RecordingStartRequestTick.store(0", publish);
    EXPECT_NE(idleClear, std::string::npos) << "every idle transition must disarm the pending-start tick";

    // Reported before the intent is cleared, otherwise the tick is already gone.
    const size_t stopReport = source.find("before the recording went live");
    const size_t stopIntent =
        source.find("PublishRecordingStartIntent(RecordingStartIntent::Idle, \"record stop hotkey\")");
    ASSERT_NE(stopReport, std::string::npos);
    ASSERT_NE(stopIntent, std::string::npos);
    EXPECT_LT(stopReport, stopIntent);
}

// The ~3.2 s render->loopback probe is what makes the startup window long enough to swallow a
// short recording. Because the media process is disposable, a process-memory cache can never hit
// across recordings; the controller-owned session channel is what makes the probe cost once per
// CE session. The deliberately removed disk cache must stay removed.
TEST(RecordingStartFeedbackSourceTest, RenderLatencyProbeIsSharedAcrossDisposableMediaProcesses) {
    const std::string spawn = ReadSource("common/process_ipc_client.cpp");
    const std::string mediaMain = ReadSource("captureengine/media_main.cpp");
    const std::string probe = ReadSource("mediaengine/audio_latency_probe.cpp");
    ASSERT_FALSE(spawn.empty());
    ASSERT_FALSE(mediaMain.empty());
    ASSERT_FALSE(probe.empty());

    // Controller -> media child: an inherited handle, never a command-line value (the key holds
    // the audio endpoint id) and never a file.
    EXPECT_NE(spawn.find("ce::av_sync::GetSessionLatencyChannelChildHandle()"), std::string::npos);
    EXPECT_NE(spawn.find("--avsync-latency-handle=0x%llX"), std::string::npos);

    // The child must attach the channel BEFORE the probe can run, which is inside
    // ensureMediaEngineReady() right after the engine loads.
    const size_t attach = mediaMain.find("MediaEngine_SetRenderLatencyChannel(latencyChannel)");
    const size_t measure = mediaMain.find("MeasureRenderLatencyOnce(config, mediaCacheDir)");
    ASSERT_NE(attach, std::string::npos);
    ASSERT_NE(measure, std::string::npos);
    EXPECT_LT(attach, measure);
    EXPECT_NE(mediaMain.find("ce::av_sync::MapInheritedLatencyChannel(ParseInheritedLatencyChannelHandle())"),
              std::string::npos);

    // A fresh measurement is published back so the NEXT media process starts warm.
    const size_t store = probe.find("void StoreMemoryCache(");
    ASSERT_NE(store, std::string::npos);
    EXPECT_NE(probe.find("ce::av_sync::UpsertLatencyChannel(*channel, key, latencyMs)", store), std::string::npos);
    EXPECT_NE(probe.find("ce::av_sync::LookupLatencyChannel"), std::string::npos);

    // The persistent endpoint-latency file stays banned; the channel is memory only.
    EXPECT_NE(probe.find("cacheDir=deprecated"), std::string::npos);
    EXPECT_NE(probe.find("legacyDiskCache=deleted"), std::string::npos);
}
