#include <gtest/gtest.h>

#include <windows.h>

#include "../captureengine/pseudo_overlay.h"

TEST(PseudoOverlayThreadTest, DisabledOverlayProcessesStateAndShutsDownCleanly) {
    PseudoOverlay overlay;
    ASSERT_TRUE(overlay.Init(GetModuleHandleW(nullptr)));

    PseudoOverlayConfig config;
    config.enabled = false;
    overlay.UpdateConfig(config);
    overlay.SetRecordingStartIntent(RecordingStartIntent::AudioOnly);
    ASSERT_TRUE(overlay.WaitForUiIdleForTesting());
    EXPECT_EQ(overlay.GetRecordingIndicatorStateForTesting(), ce::recording_indicator::State::StartingAudio);

    overlay.SetRecordingStartIntent(RecordingStartIntent::Idle);
    ASSERT_TRUE(overlay.WaitForUiIdleForTesting());
    EXPECT_EQ(overlay.GetRecordingIndicatorStateForTesting(), ce::recording_indicator::State::Idle);

    overlay.Shutdown();
    EXPECT_FALSE(overlay.IsInitialized());
}

// Regression: media has to know the recording-start status is off the composited screen
// before it starts a screen-grab capture pipeline. The overlay's own 500ms poll cannot
// provide that, so media signals the sync event and waits for this acknowledgement, which
// only the UI thread that owns the windows may set.
TEST(PseudoOverlayThreadTest, CaptureDarkRequestIsAcknowledgedByTheUiThread) {
    PseudoOverlay overlay;
    ASSERT_TRUE(overlay.Init(GetModuleHandleW(nullptr)));

    // The overlay keys its events on the PID of the process that owns it, which is this
    // test process here and the controller in production.
    wchar_t syncEventName[64] = {};
    wchar_t ackEventName[64] = {};
    const uint32_t ownerPid = static_cast<uint32_t>(GetCurrentProcessId());
    GenerateStatusOverlaySyncEventName(syncEventName, 64, ownerPid);
    GenerateStatusOverlayDarkAckEventName(ackEventName, 64, ownerPid);
    HANDLE syncEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, syncEventName);
    HANDLE ackEvent = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, ackEventName);
    ASSERT_NE(syncEvent, nullptr);
    ASSERT_NE(ackEvent, nullptr);

    PseudoOverlayConfig config;
    config.enabled = false;
    overlay.UpdateConfig(config);
    overlay.SetRecordingStartIntent(RecordingStartIntent::Video);
    ASSERT_TRUE(overlay.WaitForUiIdleForTesting());

    overlay.ForceStatusDarkForCaptureForTesting(true);
    ResetEvent(ackEvent);
    ASSERT_NE(SetEvent(syncEvent), FALSE);
    EXPECT_EQ(WaitForSingleObject(ackEvent, 5000), static_cast<DWORD>(WAIT_OBJECT_0));

    overlay.ForceStatusDarkForCaptureForTesting(false);
    overlay.SetRecordingStartIntent(RecordingStartIntent::Idle);
    ASSERT_TRUE(overlay.WaitForUiIdleForTesting());
    EXPECT_EQ(overlay.GetRecordingIndicatorStateForTesting(), ce::recording_indicator::State::Idle);

    CloseHandle(syncEvent);
    CloseHandle(ackEvent);
    overlay.Shutdown();
    EXPECT_FALSE(overlay.IsInitialized());
}
