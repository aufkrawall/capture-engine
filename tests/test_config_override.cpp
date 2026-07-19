#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include "../common/config.h"

class ConfigOverrideTest : public ::testing::Test {
protected:
    std::string tempConfigFile;

    void SetUp() override {
        tempConfigFile = (std::filesystem::current_path() / "test_config_override.ini").string();
        remove(tempConfigFile.c_str());
    }

    void TearDown() override {
        remove(tempConfigFile.c_str());
    }

    void WriteConfig(const std::string& content) {
        std::ofstream out(tempConfigFile);
        out << content;
        out.close();
    }
};

TEST_F(ConfigOverrideTest, SimpleOverride) {
    std::string iniContent =
        "[Overlay]\n"
        "enabled=true\n"
        "observer_only=false\n"
        "observer_policy_only=false\n"
        "observer_startup_present_only=false\n"
        "\n"
        "[App.1]\n"
        "Process=game.exe\n"
        "Overlay.enabled=false\n"
        "Overlay.observer_only=true\n"
        "Overlay.observer_policy_only=true\n"
        "Overlay.observer_startup_present_only=true\n";

    WriteConfig(iniContent);

    AppConfig config;

    // 1. Load without override (simulate other process)
    LoadConfig(tempConfigFile, config, "notepad.exe");
    EXPECT_TRUE(config.overlay.showOverlay);

    // 2. Load with override
    LoadConfig(tempConfigFile, config, "game.exe");
    EXPECT_FALSE(config.overlay.showOverlay);
    EXPECT_TRUE(config.overlay.observerOnly);
    EXPECT_TRUE(config.overlay.observerPolicyOnly);
    EXPECT_TRUE(config.overlay.observerStartupPresentOnly);
}

TEST_F(ConfigOverrideTest, CaseInsensitiveProcessMatch) {
    std::string iniContent =
        "[Overlay]\n"
        "enabled=true\n"
        "\n"
        "[App.1]\n"
        "Process=Game.exe\n"
        "Overlay.enabled=false\n";

    WriteConfig(iniContent);

    AppConfig config;

    // Mixed case override name
    LoadConfig(tempConfigFile, config, "GAME.EXE");
    EXPECT_FALSE(config.overlay.showOverlay);
}

TEST_F(ConfigOverrideTest, FallbackToGlobal) {
    std::string iniContent =
        "[Video]\n"
        "bitrate=50Mbps\n"
        "fps=60\n"
        "\n"
        "[App.1]\n"
        "Process=game.exe\n"
        "Video.bitrate=100Mbps\n";
    // fps NOT overridden

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config, "game.exe");

    // Overridden value
    EXPECT_EQ(config.video.bitrate, "100Mbps");
    // Global fallback value
    EXPECT_EQ(config.video.fps, 60);
}

TEST_F(ConfigOverrideTest, MultipleAppSections) {
    std::string iniContent =
        "[Overlay]\n"
        "enabled=true\n"
        "\n"
        "[App.1]\n"
        "Process=game1.exe\n"
        "Overlay.enabled=false\n"
        "\n"
        "[App.2]\n"
        "Process=game2.exe\n"
        "Overlay.enabled=true\n";

    WriteConfig(iniContent);

    AppConfig config;

    // Game 1 -> False
    LoadConfig(tempConfigFile, config, "game1.exe");
    EXPECT_FALSE(config.overlay.showOverlay);

    // Game 2 -> True (explicitly set, though same as default)
    // Let's change default to false to be sure
    std::string iniContent2 =
        "[Overlay]\n"
        "enabled=false\n"
        "\n"
        "[App.2]\n"
        "Process=game2.exe\n"
        "Overlay.enabled=true\n";
    WriteConfig(iniContent2);

    LoadConfig(tempConfigFile, config, "game2.exe");
    EXPECT_TRUE(config.overlay.showOverlay);
}

TEST_F(ConfigOverrideTest, ProfilesUseExplicitInjectionModes) {
    WriteConfig(
        "[Injection]\n"
        "whitelist=(\n"
        "manual.exe\n"
        ")\n"
        "[Profile.1]\n"
        "Process=normal.exe\n"
        "injection_mode=capture\n"
        "injection=overlay\n"
        "[Profile.2]\n"
        "Process=overlay.exe\n"
        "injection_mode=overlay\n"
        "[Profile.3]\n"
        "Process=none.exe\n"
        "injection_mode=none\n"
        "Video.fps=77\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "none.exe");

    auto HasProcess = [](const std::vector<WhitelistEntry>& entries, const char* process) {
        return std::any_of(entries.begin(), entries.end(),
                           [&](const WhitelistEntry& entry) { return entry.pattern == process; });
    };
    EXPECT_TRUE(HasProcess(config.gameWhitelist, "manual.exe"));
    EXPECT_TRUE(HasProcess(config.gameWhitelist, "normal.exe"));
    EXPECT_FALSE(HasProcess(config.gameWhitelist, "overlay.exe"));
    EXPECT_FALSE(HasProcess(config.gameWhitelist, "none.exe"));
    EXPECT_TRUE(HasProcess(config.overlayWhitelist, "overlay.exe"));
    EXPECT_FALSE(HasProcess(config.overlayWhitelist, "normal.exe"));
    EXPECT_EQ(config.video.fps, 77);
}

TEST_F(ConfigOverrideTest, NamedProfilesProvideAllApplicationRoutesWithoutANumericLimit) {
    WriteConfig(
        "[Capture]\n"
        "capture_method=auto\n"
        "[SystemAudio]\n"
        "enabled=false\n"
        "[Profile.Inject Game]\n"
        "process=inject-game.exe\n"
        "video_capture=inject\n"
        "[Profile.Window Game]\n"
        "process=window-game.exe\n"
        "window_title=The Window Game\n"
        "window_match=contains\n"
        "video_capture=wgc\n"
        "dll_injection=always\n"
        "audio_enabled=true\n"
        "audio_track=9\n"
        "[Profile.Desktop Game]\n"
        "process=desktop-game.exe\n"
        "video_capture=dxgi_dup\n"
        "[Profile.Audio Only]\n"
        "process=audio-only.exe\n"
        "video_capture=none\n"
        "audio_enabled=true\n"
        "audio_track=10\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    auto HasProcess = [](const std::vector<WhitelistEntry>& entries, const char* process) {
        return std::any_of(entries.begin(), entries.end(),
                           [&](const WhitelistEntry& entry) { return entry.pattern == process; });
    };
    ASSERT_EQ(config.applicationProfiles.size(), 4u);
    EXPECT_TRUE(HasProcess(config.gameWhitelist, "inject-game.exe"));
    EXPECT_TRUE(HasProcess(config.overlayWhitelist, "window-game.exe"));
    EXPECT_TRUE(HasProcess(config.profileWgcTargets, "window-game.exe"));
    EXPECT_TRUE(HasProcess(config.wgcWindowTitles, "window-game.exe"));
    EXPECT_TRUE(HasProcess(config.profileDxgiDupTargets, "desktop-game.exe"));
    EXPECT_FALSE(HasProcess(config.gameWhitelist, "audio-only.exe"));
    ASSERT_EQ(config.profileWgcTargets.size(), 1u);
    ASSERT_EQ(config.overlayWhitelist.size(), 1u);
    EXPECT_EQ(config.overlayWhitelist.front().mode, MatchMode::kExact);
    EXPECT_EQ(config.profileWgcTargets.front().windowName, "The Window Game");
    EXPECT_EQ(config.profileWgcTargets.front().mode, MatchMode::kTitleExecutable);
    ASSERT_EQ(config.audioSources.size(), 2u);
    EXPECT_EQ(config.audioSources[0].processName, "window-game.exe");
    EXPECT_EQ(config.audioSources[1].processName, "audio-only.exe");

    LoadConfig(tempConfigFile, config, "window-game.exe");
    EXPECT_EQ(config.captureMethod, "wgc");
    LoadConfig(tempConfigFile, config, "desktop-game.exe");
    EXPECT_EQ(config.captureMethod, "dxgi_dup");
    LoadConfig(tempConfigFile, config, "audio-only.exe");
    EXPECT_EQ(config.captureMethod, "none");
}

TEST_F(ConfigOverrideTest, DesktopOverlaySettingsCanBeOverriddenByApplicationProfile) {
    WriteConfig(
        "[DesktopOverlay]\n"
        "enabled=false\n"
        "size=20\n"
        "pad=10\n"
        "pos=0\n"
        "mode=0\n"
        "show_encoder_overload_warnings=false\n"
        "foreground_acquire_grace_ms=2000\n"
        "process_list=legacy.exe\n"
        "[Profile.Game]\n"
        "process=game.exe\n"
        "video_capture=wgc\n"
        "DesktopOverlay.enabled=true\n"
        "DesktopOverlay.size=28\n"
        "DesktopOverlay.pad=16\n"
        "DesktopOverlay.pos=3\n"
        "DesktopOverlay.mode=2\n"
        "DesktopOverlay.show_encoder_overload_warnings=true\n"
        "DesktopOverlay.foreground_acquire_grace_ms=750\n");

    AppConfig globalConfig;
    LoadConfig(tempConfigFile, globalConfig);
    EXPECT_FALSE(globalConfig.pseudoOverlay.enabled);
    EXPECT_EQ(globalConfig.pseudoOverlay.size, 20);
    EXPECT_EQ(globalConfig.pseudoOverlay.processList, "legacy.exe");

    AppConfig gameConfig;
    LoadConfig(tempConfigFile, gameConfig, "GAME.EXE");
    EXPECT_TRUE(gameConfig.pseudoOverlay.enabled);
    EXPECT_EQ(gameConfig.pseudoOverlay.size, 28);
    EXPECT_EQ(gameConfig.pseudoOverlay.pad, 16);
    EXPECT_EQ(gameConfig.pseudoOverlay.pos, 3);
    EXPECT_EQ(gameConfig.pseudoOverlay.mode, 2);
    EXPECT_TRUE(gameConfig.pseudoOverlay.showEncoderOverloadWarn);
    EXPECT_EQ(gameConfig.pseudoOverlay.foregroundAcquireGraceMs, 750);
    EXPECT_EQ(gameConfig.pseudoOverlay.processList, "legacy.exe");
}

TEST_F(ConfigOverrideTest, CanonicalProfileOverridesEveryLegacyRouteForTheSameTarget) {
    WriteConfig(
        "[Injection]\n"
        "whitelist=(\n"
        "game.exe\n"
        ")\n"
        "overlay_whitelist=(\n"
        "game.exe\n"
        ")\n"
        "wgc_window_detection=(\n"
        "game.exe\n"
        ")\n"
        "[Profile.Game]\n"
        "process=game.exe\n"
        "video_capture=none\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "game.exe");

    EXPECT_TRUE(config.gameWhitelist.empty());
    EXPECT_TRUE(config.overlayWhitelist.empty());
    EXPECT_TRUE(config.wgcWindowTitles.empty());
    EXPECT_TRUE(config.profileWgcTargets.empty());
    EXPECT_TRUE(config.profileDxgiDupTargets.empty());
    EXPECT_EQ(config.captureMethod, "none");
}

TEST_F(ConfigOverrideTest, InjectVideoImpliesFullDllInjectionWithoutASecondSetting) {
    WriteConfig(
        "[Profile.Game]\n"
        "process=game.exe\n"
        "video_capture=inject\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "game.exe");

    ASSERT_EQ(config.applicationProfiles.size(), 1u);
    EXPECT_EQ(config.applicationProfiles.front().dllInjection, ApplicationDllInjection::kWhenNeeded);
    EXPECT_EQ(config.applicationProfiles.front().resolvedVideoCapture, ApplicationVideoCapture::kInject);
    ASSERT_EQ(config.gameWhitelist.size(), 1u);
    EXPECT_EQ(config.gameWhitelist.front().pattern, "game.exe");
    EXPECT_TRUE(config.overlayWhitelist.empty());
    EXPECT_EQ(config.captureMethod, "inject");
}

TEST_F(ConfigOverrideTest, DllInjectionNeverBlocksInjectedVideo) {
    WriteConfig(
        "[Profile.Game]\n"
        "process=game.exe\n"
        "video_capture=inject\n"
        "dll_injection=never\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "game.exe");

    ASSERT_EQ(config.applicationProfiles.size(), 1u);
    EXPECT_EQ(config.applicationProfiles.front().resolvedVideoCapture, ApplicationVideoCapture::kNone);
    EXPECT_TRUE(config.gameWhitelist.empty());
    EXPECT_EQ(config.captureMethod, "none");
}

TEST_F(ConfigOverrideTest, LaterOverlappingNamedProfileWinsDeterministically) {
    WriteConfig(
        "[Profile.First]\n"
        "process=game.exe\n"
        "video_capture=wgc\n"
        "dll_injection=always\n"
        "[Profile.Second]\n"
        "process=GAME.EXE\n"
        "video_capture=dxgi_dup\n"
        "Video.fps=81\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "game.exe");

    ASSERT_EQ(config.applicationProfiles.size(), 1u);
    EXPECT_EQ(config.applicationProfiles.front().section, "Profile.Second");
    EXPECT_TRUE(config.profileWgcTargets.empty());
    ASSERT_EQ(config.profileDxgiDupTargets.size(), 1u);
    EXPECT_EQ(config.captureMethod, "dxgi_dup");
    EXPECT_EQ(config.video.fps, 81);
}

TEST_F(ConfigOverrideTest, LegacyProfileInjectionKeyRemainsSupported) {
    WriteConfig(
        "[Profile.1]\n"
        "Process=legacy-key.exe\n"
        "injection=normal\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    ASSERT_EQ(config.gameWhitelist.size(), 1u);
    EXPECT_EQ(config.gameWhitelist.front().pattern, "legacy-key.exe");
    EXPECT_TRUE(config.overlayWhitelist.empty());
}

TEST_F(ConfigOverrideTest, VideoSourceDrivesInjectionUnlessDllPolicyOverridesIt) {
    WriteConfig(
        "[Capture]\n"
        "capture_method=auto\n"
        "[Profile.Inherit]\n"
        "process=inherit.exe\n"
        "video_capture=inherit\n"
        "[Profile.Global Alias]\n"
        "process=global-alias.exe\n"
        "video_capture=global\n"
        "dll_injection=when_needed\n"
        "[Profile.Screen Default]\n"
        "process=screen-default.exe\n"
        "video_capture=wgc\n"
        "[Profile.Screen Always]\n"
        "process=screen-always.exe\n"
        "video_capture=wgc\n"
        "dll_injection=always\n"
        "[Profile.DXGI Always]\n"
        "process=dxgi-always.exe\n"
        "video_capture=dxgi_dup\n"
        "dll_injection=always\n"
        "[Profile.Overlay Only]\n"
        "process=overlay-only.exe\n"
        "dll_injection=always\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    auto HasProcess = [](const std::vector<WhitelistEntry>& entries, const char* process) {
        return std::any_of(entries.begin(), entries.end(),
                           [&](const WhitelistEntry& entry) { return entry.pattern == process; });
    };
    auto FindProfile = [&](const char* section) -> const ApplicationProfile* {
        const auto found = std::find_if(config.applicationProfiles.begin(), config.applicationProfiles.end(),
                                        [&](const ApplicationProfile& profile) { return profile.section == section; });
        return found == config.applicationProfiles.end() ? nullptr : &*found;
    };

    EXPECT_TRUE(HasProcess(config.gameWhitelist, "inherit.exe"));
    EXPECT_TRUE(HasProcess(config.gameWhitelist, "global-alias.exe"));
    EXPECT_FALSE(HasProcess(config.gameWhitelist, "screen-default.exe"));
    EXPECT_FALSE(HasProcess(config.overlayWhitelist, "screen-default.exe"));
    EXPECT_TRUE(HasProcess(config.overlayWhitelist, "screen-always.exe"));
    EXPECT_TRUE(HasProcess(config.overlayWhitelist, "dxgi-always.exe"));
    EXPECT_TRUE(HasProcess(config.overlayWhitelist, "overlay-only.exe"));

    const ApplicationProfile* overlayOnly = FindProfile("Profile.Overlay Only");
    ASSERT_NE(overlayOnly, nullptr);
    EXPECT_FALSE(overlayOnly->videoCaptureExplicit);
    EXPECT_EQ(overlayOnly->resolvedVideoCapture, ApplicationVideoCapture::kNone);

    LoadConfig(tempConfigFile, config, "dxgi-always.exe");
    EXPECT_EQ(config.captureMethod, "dxgi_dup");

    LoadConfig(tempConfigFile, config, "overlay-only.exe");
    EXPECT_EQ(config.captureMethod, "none");
}

TEST_F(ConfigOverrideTest, DllInjectionWinsOverCompatibilityInjectionKeys) {
    WriteConfig(
        "[Profile.Game]\n"
        "process=game.exe\n"
        "video_capture=inject\n"
        "dll_injection=never\n"
        "injection_mode=capture\n"
        "injection=normal\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "game.exe");

    ASSERT_EQ(config.applicationProfiles.size(), 1u);
    EXPECT_EQ(config.applicationProfiles.front().dllInjection, ApplicationDllInjection::kNever);
    EXPECT_FALSE(config.applicationProfiles.front().legacyInjectionSyntax);
    EXPECT_EQ(config.applicationProfiles.front().resolvedVideoCapture, ApplicationVideoCapture::kNone);
    EXPECT_TRUE(config.gameWhitelist.empty());
    EXPECT_TRUE(config.overlayWhitelist.empty());
    EXPECT_EQ(config.captureMethod, "none");
}

TEST_F(ConfigOverrideTest, InvalidDllInjectionFailsSafe) {
    WriteConfig(
        "[Profile.Game]\n"
        "process=game.exe\n"
        "video_capture=inject\n"
        "dll_injection=sometimes\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "game.exe");

    ASSERT_EQ(config.applicationProfiles.size(), 1u);
    EXPECT_EQ(config.applicationProfiles.front().dllInjection, ApplicationDllInjection::kNever);
    EXPECT_EQ(config.applicationProfiles.front().resolvedVideoCapture, ApplicationVideoCapture::kNone);
    EXPECT_TRUE(config.gameWhitelist.empty());
    EXPECT_TRUE(config.overlayWhitelist.empty());
    EXPECT_EQ(config.captureMethod, "none");
}

TEST_F(ConfigOverrideTest, ProfileWithoutDllInjectionDoesNotInject) {
    WriteConfig(
        "[Profile.1]\n"
        "Process=game.exe\n"
        "Video.fps=75\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "game.exe");

    EXPECT_TRUE(config.gameWhitelist.empty());
    EXPECT_TRUE(config.overlayWhitelist.empty());
    EXPECT_EQ(config.video.fps, 75);
}

TEST_F(ConfigOverrideTest, ProfilesCombineAppAudioAndQualifiedOverrides) {
    WriteConfig(
        "[Audio]\n"
        "codec=alac\n"
        "bitrate=192\n"
        "sample_rate=default\n"
        "bit_depth=default\n"
        "downmix=false\n"
        "[SystemAudio]\n"
        "enabled=false\n"
        "[Profile.1]\n"
        "Process=game1.exe\n"
        "audio_enabled=true\n"
        "audio_track=3,4\n"
        "audio_codec=opus\n"
        "audio_bitrate=256\n"
        "audio_sample_rate=48000\n"
        "audio_bit_depth=16\n"
        "audio_downmix=true\n"
        "audio_capture_latency_ms=12.5\n"
        "Graphics.vsync_mode=fifo\n"
        "[Profile.2]\n"
        "Process=game2.exe\n"
        "audio_enabled=true\n"
        "audio_track=5\n"
        "audio_codec=flac\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "game1.exe");

    EXPECT_EQ(config.graphics.vsyncMode, "fifo");
    ASSERT_EQ(config.audioSources.size(), 2u);
    EXPECT_EQ(config.audioSources[0].sourceType, AudioConfig::AppAudio);
    EXPECT_EQ(config.audioSources[0].processName, "game1.exe");
    EXPECT_EQ(config.audioSources[0].tracks, (std::vector<int>{3, 4}));
    EXPECT_EQ(config.audioSources[0].codec, "opus");
    EXPECT_EQ(config.audioSources[0].bitrate, 256);
    EXPECT_EQ(config.audioSources[0].sampleRate, "48000");
    EXPECT_EQ(config.audioSources[0].bitDepth, "16");
    EXPECT_TRUE(config.audioSources[0].downmix);
    EXPECT_FLOAT_EQ(config.audioSources[0].captureLatencyMs, 12.5f);
    EXPECT_EQ(config.audioSources[1].processName, "game2.exe");
    EXPECT_EQ(config.audioSources[1].tracks, (std::vector<int>{5}));
    EXPECT_EQ(config.audioSources[1].codec, "flac");
    EXPECT_FALSE(config.audioSources[1].downmix);
}

TEST_F(ConfigOverrideTest, ProfileAudioSettingTakesPrecedenceOverSameNumberedLegacySection) {
    WriteConfig(
        "[SystemAudio]\n"
        "enabled=false\n"
        "[Profile.1]\n"
        "Process=canonical.exe\n"
        "audio_enabled=false\n"
        "[AppAudio.1]\n"
        "enabled=true\n"
        "process=legacy.exe\n"
        "track=3\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_TRUE(config.audioSources.empty());
}

TEST_F(ConfigOverrideTest, LegacyAppProfileStillAddsNormalInjectionTarget) {
    WriteConfig(
        "[App.1]\n"
        "Process=legacy.exe\n"
        "Video.fps=75\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "legacy.exe");

    ASSERT_EQ(config.gameWhitelist.size(), 1u);
    EXPECT_EQ(config.gameWhitelist.front().pattern, "legacy.exe");
    EXPECT_TRUE(config.overlayWhitelist.empty());
    EXPECT_EQ(config.video.fps, 75);
}

TEST_F(ConfigOverrideTest, EmptyCanonicalNumberedSectionStillFallsBackToLegacyApp) {
    WriteConfig(
        "[Profile.1]\n"
        "Video.fps=90\n"
        "[App.1]\n"
        "Process=legacy.exe\n"
        "Video.fps=75\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "legacy.exe");

    ASSERT_EQ(config.applicationProfiles.size(), 1u);
    EXPECT_TRUE(config.applicationProfiles.front().legacy);
    ASSERT_EQ(config.gameWhitelist.size(), 1u);
    EXPECT_EQ(config.gameWhitelist.front().pattern, "legacy.exe");
    EXPECT_EQ(config.video.fps, 75);
}

TEST_F(ConfigOverrideTest, PerAppDLSSFGFactorOverride) {
    std::string iniContent =
        "[Graphics]\n"
        "dlss_fg_factor=2x\n"
        "dlss_sr_preset=K\n"
        "\n"
        "[App.1]\n"
        "Process=RoboCop-Win64-Shipping.exe\n"
        "dlss_fg_factor=3x\n"
        "dlss_sr_preset=L\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config, "RoboCop-Win64-Shipping.exe");

    EXPECT_EQ(config.graphics.dlssFgFactor, "3x");
    EXPECT_EQ(config.graphics.parsed.dlssFGFactor, 3);
    EXPECT_EQ(config.graphics.parsed.srPreset, 12u);
}

TEST_F(ConfigOverrideTest, StreamlineDllPathParsing) {
    std::string iniContent =
        "[Graphics]\n"
        "streamline_dll_path=C:\\custom\\sl\\dlls\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config, "test.exe");

    EXPECT_EQ(config.graphics.streamlineDllPath, "C:\\custom\\sl\\dlls");
}

// Regression: an [App.N] override section's reserved "Process" selector key must
// NOT leak into the [AppAudio.N] "process" field. Before the fix this collapsed
// every app-audio source onto the running game, summing identical captures into a
// track and producing comb-filter ("metallic") audio.
TEST_F(ConfigOverrideTest, AppAudioProcessNotRewrittenByAppSelectorKey) {
    std::string iniContent =
        "[App.1]\n"
        "Process=game.exe\n"
        "\n"
        "[AppAudio.1]\n"
        "enabled=true\n"
        "process=other.exe\n"
        "track=1\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config, "game.exe");

    int appCount = 0;
    for (const auto& s : config.audioSources) {
        if (s.sourceType == AudioConfig::AppAudio) {
            ++appCount;
            EXPECT_EQ(s.processName, "other.exe");  // not rewritten to "game.exe"
        }
    }
    EXPECT_EQ(appCount, 1);
}

// The explicit per-app override form ([App.N] AppAudio.1.process=...) must still
// work; only the bare reserved selector key is excluded from the fallback.
TEST_F(ConfigOverrideTest, AppAudioExplicitProcessOverrideStillApplies) {
    std::string iniContent =
        "[App.1]\n"
        "Process=game.exe\n"
        "AppAudio.1.process=explicit.exe\n"
        "\n"
        "[AppAudio.1]\n"
        "enabled=true\n"
        "process=other.exe\n"
        "track=1\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config, "game.exe");

    for (const auto& s : config.audioSources) {
        if (s.sourceType == AudioConfig::AppAudio) {
            EXPECT_EQ(s.processName, "explicit.exe");
        }
    }
}

// Reproduces the Strange Brigade profile: three candidate game processes routed to
// tracks 1 & 2, with an [App.N] override matching the running game. All three
// app-audio sources must keep their distinct process names (only the running one
// will actually capture at runtime), never collapsing onto the running game.
TEST_F(ConfigOverrideTest, MultipleAppAudioSourcesKeepDistinctProcessNames) {
    std::string iniContent =
        "[AppAudio.1]\n"
        "enabled=true\n"
        "Process=FortniteClient-Win64-Shipping.exe\n"
        "track=1,2\n"
        "\n"
        "[AppAudio.2]\n"
        "enabled=true\n"
        "Process=StrangeBrigade_DX12.exe\n"
        "track=1,2\n"
        "\n"
        "[AppAudio.3]\n"
        "enabled=true\n"
        "Process=StrangeBrigade_Vulkan.exe\n"
        "track=1,2\n"
        "\n"
        "[App.2]\n"
        "Process=StrangeBrigade_DX12.exe\n";

    WriteConfig(iniContent);

    AppConfig config;
    LoadConfig(tempConfigFile, config, "StrangeBrigade_DX12.exe");

    std::set<std::string> names;
    for (const auto& s : config.audioSources) {
        if (s.sourceType == AudioConfig::AppAudio) {
            names.insert(s.processName);
        }
    }
    EXPECT_EQ(names.size(), 3u);
    EXPECT_EQ(names.count("FortniteClient-Win64-Shipping.exe"), 1u);
    EXPECT_EQ(names.count("StrangeBrigade_DX12.exe"), 1u);
    EXPECT_EQ(names.count("StrangeBrigade_Vulkan.exe"), 1u);
}
