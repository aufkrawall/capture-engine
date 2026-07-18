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
