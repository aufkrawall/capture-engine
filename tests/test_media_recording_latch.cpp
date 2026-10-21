#include <gtest/gtest.h>

#include <windows.h>

#include <timeapi.h>

#include "captureengine/app/mediaengine_loader.h"
#include "captureengine/media/media_main_internal.h"
#include "captureengine/media/media_session_state.h"
#include "common/config/config.h"
#include "common/ipc/media_finalization.h"

// A stop owes the spawner and the overlays one terminal result. A recording that went live gets it from its own
// finalization; CompleteAbortedRecordingStart covers a stop that arrived before any recording started and must stay
// silent for one that did. The audio-only start left the started latch unset, so every audio-only recording stopped
// through the media channel finalized a second time, as canceled, over its saved result (found 2026-10-21 by the
// outside-repo SDK run: the file existed, the client was told "canceled" with no path).

namespace {

bool StartsRecording(MediaEngineHandle) {
    return true;
}

class MediaRecordingLatchTest : public testing::Test {
protected:
    void SetUp() override {
        savedStart_ = ce::mediaengine_api::MediaEngine_StartRecording;
        ce::mediaengine_api::MediaEngine_StartRecording = StartsRecording;
    }
    void TearDown() override {
        ce::mediaengine_api::MediaEngine_StartRecording = savedStart_;
    }

    // The session the recording functions act on (DR-41): no shared memory, no engine, no result slot.
    MediaSessionState session_;
    MediaSessionBinding binding_{session_};
    AppConfig config_;
    MediaEngine_StartRecording_t savedStart_ = nullptr;
};

}  // namespace

TEST_F(MediaRecordingLatchTest, AStopBeforeAnyRecordingStartedFinalizesAsCanceled) {
    CompleteAbortedRecordingStart("test stop");
    EXPECT_EQ(session_.lastFinalization.finalizationCount, 1u);
    EXPECT_EQ(session_.lastFinalization.status, ce::media_result::Status::kCanceled);
}

TEST_F(MediaRecordingLatchTest, ALiveAudioOnlyRecordingIsNotFinalizedAsAnAbortedStart) {
    session_.audioOnly = true;
    ASSERT_TRUE(StartRecording(config_));
    timeEndPeriod(1);  // StartRecording raised the timer resolution for the recording it started
    ASSERT_TRUE(session_.recording);

    CompleteAbortedRecordingStart("test stop");
    EXPECT_EQ(session_.lastFinalization.finalizationCount, 0u)
        << "the stop of a live audio-only recording added a canceled result over its own finalization";
}
