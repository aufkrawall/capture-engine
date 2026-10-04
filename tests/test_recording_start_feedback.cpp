#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "source_fragment_reader.h"

// Controller lifecycle ordering/failure checks now exercise RecordingSession in test_recording_session.cpp.
namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

}  // namespace



TEST(RecordingStartFeedbackSourceTest, MediaOwnsLiveAndTerminalIntentTransitions) {
    const std::string source = ReadSource("captureengine/media/media_main.cpp");
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
    const std::string source = ReadSource("captureengine/media/media_main.cpp");
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
    const std::string protocol = ReadSource("captureengine/app/status_overlay_sync.cpp");
    ASSERT_FALSE(protocol.empty());
    EXPECT_NE(protocol.find("WaitForSingleObject(ackEvent, kDarkAckTimeoutMs)"), std::string::npos);
    EXPECT_NE(protocol.find("No controller status consumer"), std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, MediaReleasesTheCaptureDarkRequestOnEveryStatusPublication) {
    const std::string source = ReadSource("captureengine/media/media_main.cpp");
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("ReleaseStatusOverlayDarkForCapture(\"recording live\")"), std::string::npos);
    EXPECT_NE(source.find("ReleaseStatusOverlayDarkForCapture(\"recording not live\")"), std::string::npos);
    EXPECT_NE(source.find("ReleaseStatusOverlayDarkForCapture(\"recording start failure\")"), std::string::npos);
    // Media wakes the controller-side overlay on every status publication so the live REC
    // state does not wait for the overlay's next poll either.
    EXPECT_NE(source.find("SignalStatusOverlaySync()"), std::string::npos);

    const std::string overlay = ReadSource("hook/overlay/overlay_adapter.cpp");
    ASSERT_FALSE(overlay.empty());
    EXPECT_NE(overlay.find("kCaptureRuntimeFlagStatusOverlayDarkForCapture"), std::string::npos);
    EXPECT_NE(overlay.find("frameLayout.recordingStatusDark"), std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, WarmupStopIsAcceptedAsCancellationBeforeLiveCommit) {
    const std::string captureSource = ReadSource("captureengine/media/media_main.cpp");
    const std::string mediaSource = ReadSource("mediaengine/engine/mediaengine.cpp");
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
    const std::string source = ReadSource("mediaengine/video/video_encoder.cpp");
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
    const std::string source = ReadSource("common/ipc/shared_defs_detail/abi_constants_and_config.h");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("RecordingFinalizing = 3"), std::string::npos);
    EXPECT_NE(source.find("RecordingSaved = 4"), std::string::npos);
    EXPECT_NE(source.find("RecordingSavedDegraded = 5"), std::string::npos);
    EXPECT_NE(source.find("RecordingFailed = 7"), std::string::npos);
    EXPECT_NE(source.find("RecordingSavedAudioDegraded = 11"), std::string::npos);
    EXPECT_NE(source.find("StreamingEndedAudioVideoDegraded = 14"), std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, RecordingFinalizationTextInInjectOverlay) {
    const std::string source = ReadSource("hook/overlay/overlay_adapter.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("\"Finalizing recording...\""), std::string::npos);
    // Completion wording is shared with the pseudo overlay (see
    // OutputCompletionNotificationTest for the texts themselves).
    EXPECT_NE(source.find("ce::output_completion::DescribeOutputCompletion("), std::string::npos);
    EXPECT_NE(source.find("ce::output_completion::kOutputCompletionNotificationTypes"), std::string::npos);
    EXPECT_NE(source.find("ce::output_completion::IsRecordingFinalizationNotification("), std::string::npos);
    EXPECT_EQ(source.find("\"Recording saved - video degraded\""), std::string::npos);

    const std::string pseudo = ReadSource("captureengine/pseudo_overlay/pseudo_overlay_render.cpp");
    ASSERT_FALSE(pseudo.empty());
    EXPECT_NE(pseudo.find("ce::output_completion::DescribeOutputCompletion("), std::string::npos);
    EXPECT_EQ(pseudo.find("\"Recording saved - video degraded\""), std::string::npos);
}



TEST(RecordingStartFeedbackSourceTest, MediaPublishesSavedStateOnlyAfterMuxFinalization) {
    const std::string source = ReadSource("captureengine/media/media_main.cpp");
    ASSERT_FALSE(source.empty());
    const size_t stop = source.find("MediaEngine_StopRecording(cancelBeforeLive)");
    const size_t complete = source.find("CompleteRecordingFinalization(cancelBeforeLive, outputSaved)", stop);
    ASSERT_NE(stop, std::string::npos);
    ASSERT_NE(complete, std::string::npos);
    EXPECT_LT(stop, complete);
    EXPECT_NE(source.find("SelectOutputCompletionNotification"), std::string::npos);
    EXPECT_NE(source.find("outputSaved=%d"), std::string::npos);
    EXPECT_NE(source.find("finalizationComplete=1"), std::string::npos);

    const std::string completionPolicy = ReadSource("common/config/live_stream_config.cpp");
    ASSERT_FALSE(completionPolicy.empty());
    EXPECT_NE(completionPolicy.find("OverlayNotificationType::RecordingSavedDegraded"), std::string::npos);
    EXPECT_NE(completionPolicy.find("OverlayNotificationType::RecordingFailed"), std::string::npos);
    EXPECT_NE(completionPolicy.find("OverlayNotificationType::StreamingEndedDegraded"), std::string::npos);
    EXPECT_NE(completionPolicy.find("OverlayNotificationType::StreamingFailed"), std::string::npos);
    EXPECT_NE(completionPolicy.find("OverlayNotificationType::RecordingSavedAudioDegraded"), std::string::npos);
    EXPECT_NE(completionPolicy.find("OverlayNotificationType::StreamingEndedAudioDegraded"), std::string::npos);

    // The finalization folds each track's output loss into its own degraded bit and the
    // manifest records which track(s) lost content.
    const std::string finalization = ReadSource("captureengine/media/media_main_recording.cpp");
    ASSERT_FALSE(finalization.empty());
    EXPECT_NE(finalization.find("MediaEngine_GetLastOutputDegradedFlags() & ce::capture_policy::kRecordingHealthDegradedMask"),
              std::string::npos);
    EXPECT_NE(finalization.find("kRecordingHealthFlagAudioDegraded"), std::string::npos);
    const std::string manifest = ReadSource("captureengine/media/recording_manifest.h");
    ASSERT_FALSE(manifest.empty());
    EXPECT_NE(manifest.find("\"recording_degraded=\""), std::string::npos);

    const std::string overlay = ReadSource("hook/overlay/overlay_adapter.cpp");
    ASSERT_FALSE(overlay.empty());
    EXPECT_NE(overlay.find("recordingFinalizationNotification"), std::string::npos);
    EXPECT_NE(overlay.find("recordingState == ce::recording_indicator::State::Idle"), std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, MediaPublishesFailureNotificationOnStartFailure) {
    const std::string source = ReadSource("captureengine/media/media_main.cpp");
    ASSERT_FALSE(source.empty());
    const size_t publish = source.find("void PublishRecordingStartFailure(");
    ASSERT_NE(publish, std::string::npos);
    EXPECT_NE(source.find("OverlayNotificationType::RecordingFailed", publish), std::string::npos);
    EXPECT_NE(source.find("OverlayNotificationType::StreamingFailed", publish), std::string::npos);
    EXPECT_NE(source.find("notificationExpiry.store(GetTickCount64() + 7000ULL", publish), std::string::npos);
}





TEST(RecordingStartFeedbackSourceTest, MediaStopResultRequiresPublishedOutput) {
    const std::string api = ReadSource("mediaengine/engine/mediaengine.h");
    const std::string loader = ReadSource("captureengine/app/mediaengine_loader.h");
    const std::string encoder = ReadSource("mediaengine/video/video_encoder.h");
    ASSERT_FALSE(api.empty());
    ASSERT_FALSE(loader.empty());
    ASSERT_FALSE(encoder.empty());

    EXPECT_NE(api.find("MEDIAENGINE_API bool MediaEngine_StopRecording"), std::string::npos);
    EXPECT_NE(loader.find("typedef bool (*MediaEngine_StopRecording_t)"), std::string::npos);
    EXPECT_NE(encoder.find("WasLastOutputPublished() const"), std::string::npos);
}

TEST(RecordingStartFeedbackSourceTest, InjectOverlayContainsExactPendingLabelsAndPseudoOwnsUiThread) {
    const std::string overlay = ReadSource("hook/overlay/overlay_adapter.cpp");
    const std::string pseudo = ReadSource("captureengine/pseudo_overlay/pseudo_overlay.cpp");
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
    const std::string source = ReadSource("captureengine/media/media_main.cpp");
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
    const std::string source = ReadSource("captureengine/media/media_main.cpp");
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
    const std::string manifest = ReadSource("captureengine/media/recording_manifest.h");
    ASSERT_FALSE(manifest.empty());
    EXPECT_NE(manifest.find("recording_canceled"), std::string::npos);
}

// The inject ack only proves inject set cmdStartRecording; the media process may still be
// seconds away from a live recording. Logging "Recording started" there reported a recording
// that did not exist and, in the aborted case, never would.


TEST(RecordingStartFeedbackSourceTest, RenderLatencyProbeIsSharedAcrossDisposableMediaProcesses) {
    const std::string spawn = ReadSource("common/ipc/process_ipc_client.cpp");
    const std::string mediaMain = ReadSource("captureengine/media/media_main.cpp");
    const std::string probe = ReadSource("mediaengine/audio/audio_latency_probe.cpp");
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

// Each probe shot stops once its marker burst is fully captured instead of always recording the
// full ~0.62 s window (20260927_195021: 5 shots = 3.17 s of recording-start delay). The full window
// must stay as the upper bound for deep render paths, and the stop reason must stay in the log.
TEST(RecordingStartFeedbackSourceTest, RenderLatencyProbeShotsStopOnceTheMarkerIsCaptured) {
    const std::string probe = ReadSource("mediaengine/audio/audio_latency_probe.cpp");
    ASSERT_FALSE(probe.empty());

    const size_t shot = probe.find("bool MeasureOnceMs(");
    ASSERT_NE(shot, std::string::npos);
    const size_t early =
        probe.find("DetectCompletedMarkerCenterFrame(capturedMono.data(), capturedMono.size(), spec)", shot);
    const size_t full = probe.find("capturedMono.size() >= fullWindowFrames", shot);
    const size_t detect = probe.find("DetectMarkerCenterFrame(capturedMono.data(), capturedMono.size()", shot);
    ASSERT_NE(early, std::string::npos);
    ASSERT_NE(full, std::string::npos);
    ASSERT_NE(detect, std::string::npos);
    // The early stop only ends the capture; the measurement still comes from the same detector
    // over everything captured.
    EXPECT_LT(early, detect);
    EXPECT_LT(full, detect);
    EXPECT_NE(probe.find("stopReason = \"marker_complete\"", shot), std::string::npos);
    EXPECT_NE(probe.find("stopReason = \"full_window\"", shot), std::string::npos);
    EXPECT_NE(probe.find("confidence=high probeMs=%.1f"), std::string::npos);
}
