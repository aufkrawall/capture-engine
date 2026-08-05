#include <gtest/gtest.h>

#include "../common/pseudo_overlay_profile_policy.h"
#include "../common/pseudo_overlay_visibility.h"

namespace pov = ce::pseudo_overlay;

namespace {

// Baseline inputs: mode-0 indicator, nothing pending, "now" at a fixed clock.
pov::OverlayVisibilityInputs Base() {
    pov::OverlayVisibilityInputs in;
    in.mode = 0;
    in.recordingState = ce::recording_indicator::State::Idle;
    in.warnVisible = false;
    in.showEncoderOverloadWarn = true;
    in.ghostActive = false;
    in.nowMs = 100000;
    in.overloadWarnUntilMs = 0;
    in.screenshotNotifyUntilMs = 0;
    in.recordingNotifyUntilMs = 0;
    in.recordingNotification = pov::RecordingNotificationKind::None;
    return in;
}

}  // namespace

// ---- Core mode-2 leak regression ----

TEST(PseudoOverlayVisibilityTest, Mode2RecordingWithStaleWarnIsInactive) {
    // The bug this guards: starting a recording in mode 2 while the NOT-RECORDING warning
    // was in its visible phase must NOT keep an overlay window alive. Without the
    // !isRecording gate the stale warnVisible flag kept the warning window up for up to
    // ~500 ms into the recording.
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::RecordingVideo;
    in.warnVisible = true;  // stale: not yet cleared by the next timer tick
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, Mode1RecordingWithStaleWarnStillShowsIndicatorOnly) {
    // In mode 1 the indicator is shown while recording regardless, but the leak gate still
    // applies to the warning term. The overlay is visible (indicator), and that visibility
    // is not coming from the stale warning.
    auto in = Base();
    in.mode = 1;
    in.recordingState = ce::recording_indicator::State::RecordingVideo;
    in.warnVisible = true;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));  // indicator
    // Prove it's the indicator, not the warning: drop the indicator (mode 2) and the
    // same stale warning must not keep it visible.
    in.mode = 2;
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in));
}

// ---- Capture-dark request (screen-grab recordings capture the composited screen) ----

TEST(PseudoOverlayVisibilityTest, CaptureDarkRemovesEveryPendingStartupElement) {
    // The bug this guards: the amber startup status stayed on screen while the screen-grab
    // capture pipeline was already recording the desktop, so the recorded file began with
    // CE's own "STARTING RECORDING..." text burned into it.
    for (const int mode : {0, 1, 2}) {
        for (const auto state :
             {ce::recording_indicator::State::StartingVideo, ce::recording_indicator::State::StartingAudio}) {
            auto in = Base();
            in.mode = mode;
            in.recordingState = state;
            in.statusDarkForCapture = true;
            EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::None) << mode;
            EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in)) << mode;
        }
    }
}

TEST(PseudoOverlayVisibilityTest, CaptureDarkKeepsStartupStatusWhenNotRequested) {
    auto in = Base();
    in.mode = 1;
    in.recordingState = ce::recording_indicator::State::StartingVideo;
    in.statusDarkForCapture = false;
    EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::Starting);
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, CaptureDarkNeverSuppressesTheLiveRecordingIndicator) {
    // Media releases the request in the same publication that turns the state live, but a
    // stale request bit must never be able to hide an established recording indicator.
    auto in = Base();
    in.mode = 1;
    in.recordingState = ce::recording_indicator::State::RecordingVideo;
    in.statusDarkForCapture = true;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, CaptureDarkDoesNotRevivePendingSuppressedIdleText) {
    // Going dark is not the same as going idle: the NOT-RECORDING warning and idle
    // finalization feedback stay suppressed while a recording start is pending.
    auto in = Base();
    in.mode = 1;
    in.recordingState = ce::recording_indicator::State::StartingVideo;
    in.statusDarkForCapture = true;
    in.warnVisible = true;
    in.recordingNotifyUntilMs = in.nowMs + 1000;
    in.recordingNotification = pov::RecordingNotificationKind::Saved;
    EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::None);
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in));
}

// ---- Sanctioned exceptions still work during recording ----

TEST(PseudoOverlayVisibilityTest, Mode2RecordingOverloadWarningStillShows) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::RecordingVideo;
    in.warnVisible = false;
    in.overloadWarnUntilMs = in.nowMs + 5000;  // overload active
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, Mode2RecordingOverloadSuppressedWhenWarningDisabled) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::RecordingVideo;
    in.showEncoderOverloadWarn = false;  // user disabled the overload warning
    in.overloadWarnUntilMs = in.nowMs + 5000;
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, Mode2RecordingScreenshotNotificationStillShows) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::RecordingVideo;
    in.screenshotNotifyUntilMs = in.nowMs + 2000;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, ExpiredOverloadAndScreenshotDoNotShow) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::RecordingVideo;
    in.overloadWarnUntilMs = in.nowMs;          // expired (now < until is false)
    in.screenshotNotifyUntilMs = in.nowMs - 1;  // expired
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in));
}

// ---- Not-recording warning behavior preserved ----

TEST(PseudoOverlayVisibilityTest, NotRecordingWarningShowsWhenWarnVisible) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::Idle;
    in.warnVisible = true;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, NotRecordingWarningHiddenDuringBlinkOffPhase) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::Idle;
    in.warnVisible = false;  // blink "off" phase
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in));
}

// ---- Indicator behavior preserved ----

TEST(PseudoOverlayVisibilityTest, Mode0RecordingShowsIndicator) {
    auto in = Base();
    in.mode = 0;
    in.recordingState = ce::recording_indicator::State::RecordingVideo;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, Mode2RecordingNoExceptionsIsInactive) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::RecordingVideo;
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, StartingVideoUsesIndicatorInMode0) {
    auto in = Base();
    in.mode = 0;
    in.recordingState = ce::recording_indicator::State::StartingVideo;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, StartingAudioUsesIndicatorAndTextInMode1) {
    auto in = Base();
    in.mode = 1;
    in.recordingState = ce::recording_indicator::State::StartingAudio;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, StartingVideoUsesTextInWarningOnlyMode) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::StartingVideo;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, StartingStateSuppressesStaleNotRecordingWarning) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::StartingVideo;
    in.warnVisible = true;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
    in.recordingState = ce::recording_indicator::State::RecordingVideo;
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, StartingStateOwnsTextPriorityAcrossModes) {
    auto in = Base();
    in.recordingState = ce::recording_indicator::State::StartingAudio;
    in.warnVisible = true;
    in.overloadWarnUntilMs = in.nowMs + 5000;
    in.screenshotNotifyUntilMs = in.nowMs + 2000;

    in.mode = 0;
    EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::None);
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));  // amber circle only

    in.mode = 1;
    EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::Starting);
    in.mode = 2;
    EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::Starting);
}

TEST(PseudoOverlayVisibilityTest, IdleAndLiveNotificationsRetainExistingPriority) {
    auto in = Base();
    in.mode = 2;
    in.warnVisible = true;
    in.overloadWarnUntilMs = in.nowMs + 5000;
    in.screenshotNotifyUntilMs = in.nowMs + 2000;
    EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::Screenshot);

    in.screenshotNotifyUntilMs = 0;
    EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::EncoderOverload);
    in.overloadWarnUntilMs = 0;
    EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::NotRecording);

    in.recordingState = ce::recording_indicator::State::RecordingVideo;
    EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::None);
}

TEST(PseudoOverlayVisibilityTest, IdleNothingPendingIsInactive) {
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(Base()));
}

// ---- Recording finalization notifications ----

TEST(PseudoOverlayVisibilityTest, RecordingFinalizingNotificationShows) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::Idle;
    in.recordingNotifyUntilMs = in.nowMs + 2000;
    in.recordingNotification = pov::RecordingNotificationKind::Finalizing;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
    EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::RecordingFinalizing);
}

TEST(PseudoOverlayVisibilityTest, RecordingNotificationExpiredDoesNotShow) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::Idle;
    in.recordingNotifyUntilMs = in.nowMs;  // not strictly < nowMs
    in.recordingNotification = pov::RecordingNotificationKind::Saved;
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in));

    in.recordingNotifyUntilMs = in.nowMs - 1;  // expired
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, RecordingNotificationPriorityBelowScreenshot) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::Idle;
    in.recordingNotifyUntilMs = in.nowMs + 2000;
    in.recordingNotification = pov::RecordingNotificationKind::Saved;
    in.screenshotNotifyUntilMs = in.nowMs + 2000;
    EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::Screenshot);
}

TEST(PseudoOverlayVisibilityTest, DegradedRecordingSavedPriorityAboveOverload) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::Idle;
    in.recordingNotifyUntilMs = in.nowMs + 2000;
    in.recordingNotification = pov::RecordingNotificationKind::SavedDegraded;
    in.overloadWarnUntilMs = in.nowMs + 5000;
    EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::RecordingSavedDegraded);
}

TEST(PseudoOverlayVisibilityTest, RecordingCanceledPriorityAboveNotRecording) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::Idle;
    in.recordingNotifyUntilMs = in.nowMs + 2000;
    in.recordingNotification = pov::RecordingNotificationKind::Canceled;
    in.warnVisible = true;
    EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::RecordingCanceled);
}

TEST(PseudoOverlayVisibilityTest, RecordingFailureIsDistinctFromSaved) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::Idle;
    in.recordingNotifyUntilMs = in.nowMs + 7000;
    in.recordingNotification = pov::RecordingNotificationKind::Failed;
    EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::RecordingFailed);
}

TEST(PseudoOverlayVisibilityTest, RecordingSavedNotificationShowsOnlyAfterCompletionKindArrives) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::Idle;
    in.recordingNotifyUntilMs = in.nowMs + 2000;
    in.recordingNotification = pov::RecordingNotificationKind::Saved;
    EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::RecordingSaved);
}

TEST(PseudoOverlayVisibilityTest, PriorFinalizationNotificationCannotCoverAnActiveRecording) {
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::RecordingVideo;
    in.recordingNotifyUntilMs = in.nowMs + 60000;
    in.recordingNotification = pov::RecordingNotificationKind::Finalizing;
    EXPECT_EQ(pov::SelectPseudoOverlayText(in), pov::OverlayTextKind::None);
}

// ---- Ghost keepalive ----

TEST(PseudoOverlayVisibilityTest, GhostKeepaliveKeepsOverlayAliveWhenIdle) {
    auto in = Base();
    in.ghostActive = true;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, GhostKeepaliveDuringMode2RecordingKeepsAlive) {
    // alwaysRender is an explicit opt-in; if enabled it intentionally keeps a window alive
    // even in mode-2 recording. This documents that the ghost path is not gated by the
    // leak fix (the leak fix only targets the NOT-RECORDING warning term).
    auto in = Base();
    in.mode = 2;
    in.recordingState = ce::recording_indicator::State::RecordingVideo;
    in.ghostActive = true;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayProfilePolicyTest, ProfileMatchingIsExactAndCaseInsensitive) {
    PseudoOverlayApplicationConfig first;
    first.section = "Profile.First";
    first.processName = "Game.exe";
    PseudoOverlayApplicationConfig second;
    second.section = "Profile.Second";
    second.processName = "OtherGame.exe";
    const std::vector<PseudoOverlayApplicationConfig> profiles = {first, second};

    const auto* matched = pov::FindApplicationConfig(profiles, "  GAME.EXE  ");
    ASSERT_NE(matched, nullptr);
    EXPECT_EQ(matched->section, "Profile.First");
    EXPECT_EQ(pov::FindApplicationConfig(profiles, "mygame.exe"), nullptr);
}

TEST(PseudoOverlayProfilePolicyTest, VideoProfileReplacesProcessListForWarnings) {
    PseudoOverlayApplicationConfig videoProfile;
    videoProfile.processName = "profiled.exe";
    videoProfile.warningTarget = true;
    EXPECT_TRUE(pov::IsForegroundWarningTarget(&videoProfile, "", "profiled.exe"));

    videoProfile.warningTarget = false;
    EXPECT_FALSE(pov::IsForegroundWarningTarget(&videoProfile, "", "profiled.exe"));
    EXPECT_TRUE(pov::IsForegroundWarningTarget(&videoProfile, "legacy.exe|profiled.exe", "profiled.exe"));
}

TEST(PseudoOverlayProfilePolicyTest, GlobalProcessListRemainsCaseInsensitiveFallback) {
    EXPECT_TRUE(pov::ProcessListContains(" first.exe | GAME.EXE ", "game.exe"));
    EXPECT_FALSE(pov::ProcessListContains("first.exe|game.exe", "other.exe"));
    EXPECT_FALSE(pov::ProcessListContains("", "game.exe"));
}
