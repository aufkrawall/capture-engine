#include <gtest/gtest.h>

#include <string>

#include "common/config/config.h"
#include "common/overlay/inject_overlay_policy.h"

// Inject overlay policy: which processes get full injection, which only the overlay, and when a config change
// needs a rescan. Split from test_shared_runtime_state.cpp (file size ceiling).
TEST(InjectOverlayPolicyTest, WgcKeepsFullInjectionWhitelistIndependentOfVideoMethod) {
    AppConfig config;
    config.captureMethod = "wgc";
    config.gameWhitelist.push_back({.pattern = "game.exe", .windowName = {}});

    const InjectorConfigState state = BuildInjectorConfigState(config);

    EXPECT_TRUE(state.allowInjection);
    ASSERT_EQ(state.config.gameWhitelist.size(), 1u);
    EXPECT_EQ(state.config.gameWhitelist[0].pattern, "game.exe");
    EXPECT_TRUE(state.config.overlayWhitelist.empty());
}

TEST(InjectOverlayPolicyTest, WgcKeepsFullAndOverlayOnlyTargetsSeparate) {
    AppConfig config;
    config.captureMethod = "wgc";
    config.gameWhitelist.push_back({.pattern = "game.exe", .windowName = {}});
    config.overlayWhitelist.push_back({.pattern = "overlay-only.exe", .windowName = {}});

    const InjectorConfigState state = BuildInjectorConfigState(config);

    EXPECT_TRUE(state.allowInjection);
    ASSERT_EQ(state.config.gameWhitelist.size(), 1u);
    EXPECT_EQ(state.config.gameWhitelist[0].pattern, "game.exe");
    ASSERT_EQ(state.config.overlayWhitelist.size(), 1u);
    EXPECT_EQ(state.config.overlayWhitelist[0].pattern, "overlay-only.exe");
}

TEST(InjectOverlayPolicyTest, DxgiDupKeepsFullInjectionWhitelistIndependentOfVideoMethod) {
    AppConfig config;
    config.captureMethod = "dxgi_dup";
    config.gameWhitelist.push_back({.pattern = "game.exe", .windowName = {}});

    const InjectorConfigState state = BuildInjectorConfigState(config);

    EXPECT_TRUE(state.allowInjection);
    ASSERT_EQ(state.config.gameWhitelist.size(), 1u);
    EXPECT_EQ(state.config.gameWhitelist[0].pattern, "game.exe");
    EXPECT_TRUE(state.config.overlayWhitelist.empty());
}

TEST(InjectOverlayPolicyTest, AutoOverlayOnlyInjectionKeepsOverlayTargetForHooking) {
    AppConfig config;
    config.captureMethod = "auto";
    config.overlayWhitelist.push_back({.pattern = "overlay-only.exe", .windowName = {}});

    const InjectorConfigState state = BuildInjectorConfigState(config);

    EXPECT_TRUE(state.allowInjection);
    EXPECT_TRUE(state.config.gameWhitelist.empty());
    ASSERT_EQ(state.config.overlayWhitelist.size(), 1u);
    EXPECT_EQ(state.config.overlayWhitelist[0].pattern, "overlay-only.exe");
}

TEST(InjectOverlayPolicyTest, AutoInjectionKeepsGameAndOverlayTargetsSeparate) {
    AppConfig config;
    config.captureMethod = "auto";
    config.gameWhitelist.push_back({.pattern = "capture-game.exe", .windowName = {}});
    config.overlayWhitelist.push_back({.pattern = "overlay-only.exe", .windowName = {}});

    const InjectorConfigState state = BuildInjectorConfigState(config);

    EXPECT_TRUE(state.allowInjection);
    ASSERT_EQ(state.config.gameWhitelist.size(), 1u);
    ASSERT_EQ(state.config.overlayWhitelist.size(), 1u);
    EXPECT_EQ(state.config.gameWhitelist[0].pattern, "capture-game.exe");
    EXPECT_EQ(state.config.overlayWhitelist[0].pattern, "overlay-only.exe");
}

TEST(InjectOverlayPolicyTest, CaptureMethodChangesDoNotChangeInjectionPolicyOrAutoTargets) {
    AppConfig autoConfig;
    autoConfig.captureMethod = "auto";
    autoConfig.gameWhitelist.push_back({.pattern = "capture-game.exe", .windowName = {}});
    autoConfig.overlayWhitelist.push_back({.pattern = "overlay-only.exe", .windowName = {}});

    AppConfig screenGrabConfig = autoConfig;
    screenGrabConfig.captureMethod = "wgc";

    const InjectorConfigState autoState = BuildInjectorConfigState(autoConfig);
    const InjectorConfigState screenGrabState = BuildInjectorConfigState(screenGrabConfig);

    EXPECT_TRUE(autoState.allowInjection);
    EXPECT_TRUE(screenGrabState.allowInjection);
    EXPECT_EQ(autoState.config.gameWhitelist, screenGrabState.config.gameWhitelist);
    EXPECT_EQ(autoState.config.overlayWhitelist, screenGrabState.config.overlayWhitelist);
    EXPECT_FALSE(ShouldRescanForConfigChange(autoConfig, autoState, screenGrabConfig, screenGrabState));
}

TEST(InjectOverlayPolicyTest, StandardInjectionKeepsGameWhitelist) {
    AppConfig config;
    config.captureMethod = "inject";
    config.gameWhitelist.push_back({.pattern = "game.exe", .windowName = {}});

    const InjectorConfigState state = BuildInjectorConfigState(config);

    EXPECT_TRUE(state.allowInjection);
    ASSERT_EQ(state.config.gameWhitelist.size(), 1u);
    EXPECT_EQ(state.config.gameWhitelist[0].pattern, "game.exe");
}

TEST(InjectOverlayPolicyTest, RescanTracksWhitelistAndLoggingChanges) {
    AppConfig oldConfig;
    oldConfig.captureMethod = "inject";
    oldConfig.logLevel = LogLevel::Off;
    oldConfig.debugLogging = false;
    oldConfig.gameWhitelist.push_back({.pattern = "game.exe", .windowName = {}});

    AppConfig sameConfig = oldConfig;
    EXPECT_FALSE(ShouldRescanForConfigChange(oldConfig, BuildInjectorConfigState(oldConfig), sameConfig,
                                             BuildInjectorConfigState(sameConfig)));

    AppConfig newConfig = oldConfig;
    newConfig.logLevel = LogLevel::Debug;
    newConfig.debugLogging = true;
    EXPECT_TRUE(ShouldRescanForConfigChange(oldConfig, BuildInjectorConfigState(oldConfig), newConfig,
                                            BuildInjectorConfigState(newConfig)));

    newConfig = oldConfig;
    newConfig.overlayWhitelist.push_back({.pattern = "overlay.dll", .windowName = {}});
    EXPECT_TRUE(ShouldRescanForConfigChange(oldConfig, BuildInjectorConfigState(oldConfig), newConfig,
                                            BuildInjectorConfigState(newConfig)));
}

TEST(InjectOverlayPolicyTest, PseudoOverlaySuppressionMatchesPendingAndActiveFlags) {
    EXPECT_FALSE(ShouldSuppressPseudoOverlayForInjectOverlayHandoff(false, false));
    EXPECT_TRUE(ShouldSuppressPseudoOverlayForInjectOverlayHandoff(true, false));
    EXPECT_TRUE(ShouldSuppressPseudoOverlayForInjectOverlayHandoff(false, true));
    EXPECT_TRUE(ShouldSuppressPseudoOverlayForInjectOverlayHandoff(true, true));
}
