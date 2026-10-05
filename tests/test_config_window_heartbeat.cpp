#include "test_config_shared.h"

TEST_F(ConfigTest, WindowHeartbeatIsOptInAndIndependentOfDesktopOverlayAndInjection) {
    WriteConfig("[Profile.Game]\nprocess=game.exe\nDesktopOverlay.enabled=true\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    ASSERT_EQ(config.applicationProfiles.size(), 1u);
    EXPECT_FALSE(config.applicationProfiles[0].windowHeartbeatEnabled);

    WriteConfig("[DesktopOverlay]\nenabled=false\n[Profile.Game]\nprocess=game.exe\n"
                "dll_injection=never\nWindowHeartbeat.enabled=true\n");
    LoadConfig(tempConfigFile, config);
    ASSERT_EQ(config.applicationProfiles.size(), 1u);
    EXPECT_TRUE(config.applicationProfiles[0].windowHeartbeatEnabled);
    EXPECT_FALSE(config.pseudoOverlay.enabled);
    EXPECT_EQ(config.applicationProfiles[0].injectionMode, ApplicationInjectionMode::kNone);
    EXPECT_EQ(config.applicationProfiles[0].resolvedVideoCapture, ApplicationVideoCapture::kNone);
    EXPECT_TRUE(config.gameWhitelist.empty());
    EXPECT_TRUE(config.overlayWhitelist.empty());
}

TEST_F(ConfigTest, WindowHeartbeatGlobalDefaultQualifiedOverridesAndTitleOnlyExclusion) {
    WriteConfig("[WindowHeartbeat]\nenabled=true\n"
                "[Profile.Inherited]\nprocess=game.exe\n"
                "[Profile.Disabled]\nprocess=other.exe\nWindowHeartbeat.enabled=false\n"
                "[Profile.Title]\nwindow_title=Title only\nWindowHeartbeat.enabled=true\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    ASSERT_EQ(config.applicationProfiles.size(), 3u);
    EXPECT_TRUE(config.applicationProfiles[0].windowHeartbeatEnabled);
    EXPECT_FALSE(config.applicationProfiles[1].windowHeartbeatEnabled);
    EXPECT_FALSE(config.applicationProfiles[2].windowHeartbeatEnabled);
}

TEST_F(ConfigTest, WindowHeartbeatIgnoresBareEnabledAndReloadRemovalClearsTheToggle) {
    WriteConfig("[Profile.Game]\nprocess=game.exe\nenabled=true\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    ASSERT_EQ(config.applicationProfiles.size(), 1u);
    EXPECT_FALSE(config.applicationProfiles[0].windowHeartbeatEnabled);
    WriteConfig("[Profile.Game]\nprocess=game.exe\nWindowHeartbeat.enabled=true\n");
    LoadConfig(tempConfigFile, config);
    ASSERT_TRUE(config.applicationProfiles[0].windowHeartbeatEnabled);
    WriteConfig("[Profile.Game]\nprocess=game.exe\n");
    LoadConfig(tempConfigFile, config);
    ASSERT_EQ(config.applicationProfiles.size(), 1u);
    EXPECT_FALSE(config.applicationProfiles[0].windowHeartbeatEnabled);
}

TEST_F(ConfigTest, InvalidWindowHeartbeatValueUsesTheGlobalDefault) {
    WriteConfig("[WindowHeartbeat]\nenabled=false\n"
                "[Profile.Game]\nprocess=game.exe\nWindowHeartbeat.enabled=invalid\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    ASSERT_EQ(config.applicationProfiles.size(), 1u);
    EXPECT_FALSE(config.applicationProfiles[0].windowHeartbeatEnabled);
}
