#include <gtest/gtest.h>

#include "../common/pseudo_overlay_visibility.h"

namespace pov = ce::pseudo_overlay;

namespace {

// Baseline inputs: mode-0 indicator, nothing pending, "now" at a fixed clock.
pov::OverlayVisibilityInputs Base() {
    pov::OverlayVisibilityInputs in;
    in.mode = 0;
    in.isRecording = false;
    in.warnVisible = false;
    in.showEncoderOverloadWarn = true;
    in.ghostActive = false;
    in.nowMs = 100000;
    in.overloadWarnUntilMs = 0;
    in.screenshotNotifyUntilMs = 0;
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
    in.isRecording = true;
    in.warnVisible = true;  // stale: not yet cleared by the next timer tick
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, Mode1RecordingWithStaleWarnStillShowsIndicatorOnly) {
    // In mode 1 the indicator is shown while recording regardless, but the leak gate still
    // applies to the warning term. The overlay is visible (indicator), and that visibility
    // is not coming from the stale warning.
    auto in = Base();
    in.mode = 1;
    in.isRecording = true;
    in.warnVisible = true;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));  // indicator
    // Prove it's the indicator, not the warning: drop the indicator (mode 2) and the
    // same stale warning must not keep it visible.
    in.mode = 2;
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in));
}

// ---- Sanctioned exceptions still work during recording ----

TEST(PseudoOverlayVisibilityTest, Mode2RecordingOverloadWarningStillShows) {
    auto in = Base();
    in.mode = 2;
    in.isRecording = true;
    in.warnVisible = false;
    in.overloadWarnUntilMs = in.nowMs + 5000;  // overload active
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, Mode2RecordingOverloadSuppressedWhenWarningDisabled) {
    auto in = Base();
    in.mode = 2;
    in.isRecording = true;
    in.showEncoderOverloadWarn = false;  // user disabled the overload warning
    in.overloadWarnUntilMs = in.nowMs + 5000;
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, Mode2RecordingScreenshotNotificationStillShows) {
    auto in = Base();
    in.mode = 2;
    in.isRecording = true;
    in.screenshotNotifyUntilMs = in.nowMs + 2000;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, ExpiredOverloadAndScreenshotDoNotShow) {
    auto in = Base();
    in.mode = 2;
    in.isRecording = true;
    in.overloadWarnUntilMs = in.nowMs;          // expired (now < until is false)
    in.screenshotNotifyUntilMs = in.nowMs - 1;  // expired
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in));
}

// ---- Not-recording warning behavior preserved ----

TEST(PseudoOverlayVisibilityTest, NotRecordingWarningShowsWhenWarnVisible) {
    auto in = Base();
    in.mode = 2;
    in.isRecording = false;
    in.warnVisible = true;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, NotRecordingWarningHiddenDuringBlinkOffPhase) {
    auto in = Base();
    in.mode = 2;
    in.isRecording = false;
    in.warnVisible = false;  // blink "off" phase
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in));
}

// ---- Indicator behavior preserved ----

TEST(PseudoOverlayVisibilityTest, Mode0RecordingShowsIndicator) {
    auto in = Base();
    in.mode = 0;
    in.isRecording = true;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, Mode2RecordingNoExceptionsIsInactive) {
    auto in = Base();
    in.mode = 2;
    in.isRecording = true;
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(in));
}

TEST(PseudoOverlayVisibilityTest, IdleNothingPendingIsInactive) {
    EXPECT_FALSE(pov::ShouldPseudoOverlayBeVisible(Base()));
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
    in.isRecording = true;
    in.ghostActive = true;
    EXPECT_TRUE(pov::ShouldPseudoOverlayBeVisible(in));
}
