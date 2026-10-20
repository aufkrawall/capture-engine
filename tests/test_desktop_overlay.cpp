#include <gtest/gtest.h>

#include <windows.h>

#include "common/config/config.h"
#include "common/overlay/recording_indicator_policy.h"
#include "runtime/desktop/desktop_overlay.h"

namespace {

using ce::recording_indicator::State;
using ce::runtime::DesktopOverlay;
using ce::runtime::DesktopOverlayRequired;
using ce::runtime::ResolveDesktopOverlayProfiles;

ApplicationProfile ProcessProfile(const char* section, const char* process, bool overlayEnabled) {
    ApplicationProfile profile;
    profile.section = section;
    profile.target.pattern = process;
    profile.desktopOverlay.enabled = overlayEnabled;
    return profile;
}

ApplicationProfile WindowOnlyProfile(const char* section, const char* windowName) {
    ApplicationProfile profile;
    profile.section = section;
    profile.target.windowName = windowName;
    profile.desktopOverlay.enabled = true;
    return profile;
}

// Re-reads the shared state on the overlay's UI thread with recording pinned inactive, then returns
// the indicator state that thread published. The start intent alone decides the state.
State IndicatorStateAfterRefresh(PseudoOverlay& overlay) {
    overlay.ForceRecordingActiveForTesting(0);
    overlay.RequestRefresh();
    EXPECT_TRUE(overlay.WaitForUiIdleForTesting());
    return overlay.GetRecordingIndicatorStateForTesting();
}

}  // namespace

TEST(DesktopOverlayPolicyTest, ResolvesOnlyProcessBackedProfilesWithTheirSettings) {
    ApplicationProfile game = ProcessProfile("game", "game.exe", true);
    game.desktopOverlay.size = 77;
    game.resolvedVideoCapture = ApplicationVideoCapture::kInject;
    ApplicationProfile capture = ProcessProfile("capture", "capture.exe", false);
    capture.resolvedVideoCapture = ApplicationVideoCapture::kWgc;
    ApplicationProfile idle = ProcessProfile("idle", "idle.exe", false);
    idle.resolvedVideoCapture = ApplicationVideoCapture::kNone;

    AppConfig config;
    config.applicationProfiles = {game, WindowOnlyProfile("window", "Title"), capture, idle};

    const auto profiles = ResolveDesktopOverlayProfiles(config);
    ASSERT_EQ(profiles.size(), 3u);

    EXPECT_EQ(profiles[0].section, "game");
    EXPECT_EQ(profiles[0].processName, "game.exe");
    EXPECT_TRUE(profiles[0].settings.enabled);
    EXPECT_EQ(profiles[0].settings.size, 77);
    EXPECT_TRUE(profiles[0].warningTarget);
    EXPECT_TRUE(profiles[0].captureUsesInjection);

    EXPECT_EQ(profiles[1].section, "capture");
    EXPECT_FALSE(profiles[1].settings.enabled);
    EXPECT_TRUE(profiles[1].warningTarget);
    EXPECT_FALSE(profiles[1].captureUsesInjection);

    EXPECT_EQ(profiles[2].section, "idle");
    EXPECT_FALSE(profiles[2].warningTarget);
    EXPECT_FALSE(profiles[2].captureUsesInjection);
}

TEST(DesktopOverlayPolicyTest, OverlayIsRequiredOnlyByGlobalOrResolvedProfileSetting) {
    AppConfig config;
    EXPECT_FALSE(DesktopOverlayRequired(config, ResolveDesktopOverlayProfiles(config)));

    // Window-only profiles never reach the overlay's profile list, so they cannot require it.
    config.applicationProfiles = {WindowOnlyProfile("window", "Title")};
    EXPECT_FALSE(DesktopOverlayRequired(config, ResolveDesktopOverlayProfiles(config)));

    config.applicationProfiles = {ProcessProfile("game", "game.exe", true)};
    EXPECT_TRUE(DesktopOverlayRequired(config, ResolveDesktopOverlayProfiles(config)));

    config.applicationProfiles.clear();
    config.pseudoOverlay.enabled = true;
    EXPECT_TRUE(DesktopOverlayRequired(config, ResolveDesktopOverlayProfiles(config)));
}

TEST(DesktopOverlayTest, DisabledEverywhereCreatesNothing) {
    DesktopOverlay overlay(GetModuleHandleW(nullptr));
    AppConfig config;
    config.applicationProfiles = {ProcessProfile("tool", "tool.exe", false), WindowOnlyProfile("window", "Title")};

    EXPECT_TRUE(overlay.Apply(config, "test"));
    EXPECT_FALSE(overlay.Active());
    EXPECT_EQ(overlay.OverlayForTesting(), nullptr);
}

TEST(DesktopOverlayTest, ProcessBackedProfileAloneCreatesOverlay) {
    DesktopOverlay overlay(GetModuleHandleW(nullptr));
    AppConfig config;
    config.applicationProfiles = {ProcessProfile("game", "game.exe", true)};

    EXPECT_TRUE(overlay.Apply(config, "test"));
    ASSERT_TRUE(overlay.Active());
    ASSERT_NE(overlay.OverlayForTesting(), nullptr);
    EXPECT_TRUE(overlay.OverlayForTesting()->IsInitialized());
}

TEST(DesktopOverlayTest, ApplyAfterCreationReconfiguresWithoutSecondCreation) {
    DesktopOverlay overlay(GetModuleHandleW(nullptr));
    AppConfig config;
    config.pseudoOverlay.enabled = true;
    ASSERT_TRUE(overlay.Apply(config, "test"));
    PseudoOverlay* created = overlay.OverlayForTesting();
    ASSERT_NE(created, nullptr);

    // Disabling everything keeps the existing overlay alive; Apply only pushes the new config.
    config.pseudoOverlay.enabled = false;
    config.pseudoOverlay.size = 45;
    EXPECT_TRUE(overlay.Apply(config, "reload"));
    EXPECT_TRUE(overlay.Active());
    EXPECT_EQ(overlay.OverlayForTesting(), created);

    config.pseudoOverlay.enabled = true;
    EXPECT_TRUE(overlay.Apply(config, "reload"));
    EXPECT_EQ(overlay.OverlayForTesting(), created);
}

TEST(DesktopOverlayTest, StartIntentRecordedBeforeCreationIsAppliedAtCreation) {
    DesktopOverlay overlay(GetModuleHandleW(nullptr));
    overlay.OnStartIntent(RecordingStartIntent::AudioOnly);
    EXPECT_FALSE(overlay.Active());

    AppConfig config;
    config.pseudoOverlay.enabled = true;
    ASSERT_TRUE(overlay.Apply(config, "test"));
    ASSERT_NE(overlay.OverlayForTesting(), nullptr);
    EXPECT_EQ(IndicatorStateAfterRefresh(*overlay.OverlayForTesting()), State::StartingAudio);
}

TEST(DesktopOverlayTest, StartIntentAfterCreationIsForwardedToTheOverlay) {
    DesktopOverlay overlay(GetModuleHandleW(nullptr));
    AppConfig config;
    config.pseudoOverlay.enabled = true;
    ASSERT_TRUE(overlay.Apply(config, "test"));
    PseudoOverlay* created = overlay.OverlayForTesting();
    ASSERT_NE(created, nullptr);

    overlay.OnStartIntent(RecordingStartIntent::Video);
    EXPECT_EQ(IndicatorStateAfterRefresh(*created), State::StartingVideo);

    overlay.OnStartIntent(RecordingStartIntent::Idle);
    EXPECT_EQ(IndicatorStateAfterRefresh(*created), State::Idle);
}

TEST(DesktopOverlayTest, StopIsIdempotentAndReleasesTheOverlay) {
    DesktopOverlay overlay(GetModuleHandleW(nullptr));
    overlay.Stop();
    EXPECT_FALSE(overlay.Active());

    AppConfig config;
    config.pseudoOverlay.enabled = true;
    ASSERT_TRUE(overlay.Apply(config, "test"));
    ASSERT_TRUE(overlay.Active());

    overlay.Stop();
    EXPECT_FALSE(overlay.Active());
    EXPECT_EQ(overlay.OverlayForTesting(), nullptr);

    overlay.Stop();
    EXPECT_FALSE(overlay.Active());
}

// The start intent belongs to the recording owner, not to the overlay: a recreated overlay must show
// the intent that is still active instead of starting from Idle.
TEST(DesktopOverlayTest, ApplyAfterStopRecreatesWithTheRememberedStartIntent) {
    DesktopOverlay overlay(GetModuleHandleW(nullptr));
    AppConfig config;
    config.pseudoOverlay.enabled = true;
    overlay.OnStartIntent(RecordingStartIntent::AudioOnly);
    ASSERT_TRUE(overlay.Apply(config, "test"));
    overlay.Stop();

    ASSERT_TRUE(overlay.Apply(config, "test"));
    ASSERT_NE(overlay.OverlayForTesting(), nullptr);
    EXPECT_EQ(IndicatorStateAfterRefresh(*overlay.OverlayForTesting()), State::StartingAudio);
}

TEST(DesktopOverlayTest, CaptureAndNotificationCallsWithoutOverlayAreNoOps) {
    DesktopOverlay overlay(GetModuleHandleW(nullptr));
    overlay.OnStartIntent(RecordingStartIntent::Video);
    overlay.OnFinalizing();
    overlay.BeginCapture();
    overlay.EndCapture(true);
    overlay.EndCapture(false);
    EXPECT_FALSE(overlay.Active());
    EXPECT_EQ(overlay.OverlayForTesting(), nullptr);
}
