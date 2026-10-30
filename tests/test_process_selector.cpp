#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "common/config/config.h"
#include "captureengine/media/profile_window_policy.h"
#include "common/platform/utf8.h"
#include "test_config_override_fixture.h"

namespace {

ApplicationProfile Profile(const char* section, const char* process) {
    ApplicationProfile profile;
    profile.section = section;
    profile.target.pattern = process;
    return profile;
}

const ApplicationProfile* FindProfile(const std::vector<ApplicationProfile>& profiles, const char* section) {
    const auto found = std::find_if(profiles.begin(), profiles.end(), [section](const ApplicationProfile& profile) {
        return profile.section == section;
    });
    return found == profiles.end() ? nullptr : &*found;
}

}  // namespace

TEST(ProcessSelectorTest, MatchesCompletePathSuffixes) {
    const ProcessTargetIdentity identity{"Game.exe", R"(C:\Games\GameFolder\Game.exe)"};
    WhitelistEntry selector;

    selector.pattern = "game.exe";
    EXPECT_TRUE(MatchesProcessTarget(selector, identity, true));

    selector.pattern = R"(GameFolder\game.exe)";
    EXPECT_TRUE(MatchesProcessTarget(selector, identity, true));

    selector.pattern = "games/gamefolder/GAME.EXE";
    EXPECT_TRUE(MatchesProcessTarget(selector, identity, true));

    selector.pattern = R"(C:\Games\GameFolder\Game.exe)";
    EXPECT_TRUE(MatchesProcessTarget(selector, identity, true));

    selector.pattern = R"(\\?\C:\Games\GameFolder\Game.exe)";
    EXPECT_TRUE(MatchesProcessTarget(selector, identity, true));

    selector.pattern = R"(C:\Games\GameFolder\Game.exe)";
    EXPECT_TRUE(
        MatchesProcessTarget(selector, ProcessTargetIdentity{"Game.exe", R"(\??\C:\Games\GameFolder\Game.exe)"}, true));

    selector.pattern = R"(\\server\share\Game.exe)";
    EXPECT_TRUE(
        MatchesProcessTarget(selector, ProcessTargetIdentity{"Game.exe", R"(\\?\UNC\server\share\Game.exe)"}, true));

    selector.pattern = R"(ameFolder\Game.exe)";
    EXPECT_FALSE(MatchesProcessTarget(selector, identity, true)) << "a suffix must begin at a path-component boundary";

    selector.pattern = R"(OtherFolder\Game.exe)";
    EXPECT_FALSE(MatchesProcessTarget(selector, identity, true));
}

TEST(ProcessSelectorTest, PathSpecificProfileBeatsNameFallback) {
    const std::vector<ApplicationProfile> profiles = {
        Profile("Profile.Generic", "Game.exe"),
        Profile("Profile.Partial", R"(GameFolder\Game.exe)"),
        Profile("Profile.Full", R"(C:\Games\GameFolder\Game.exe)"),
    };

    const ApplicationProfile* full = FindApplicationProfileForProcessIdentity(
        profiles, ProcessTargetIdentity{"Game.exe", R"(C:\Games\GameFolder\Game.exe)"});
    ASSERT_NE(full, nullptr);
    EXPECT_EQ(full->section, "Profile.Full");

    const ApplicationProfile* partial = FindApplicationProfileForProcessIdentity(
        profiles, ProcessTargetIdentity{"Game.exe", R"(D:\Other\GameFolder\Game.exe)"});
    ASSERT_NE(partial, nullptr);
    EXPECT_EQ(partial->section, "Profile.Partial");

    const ApplicationProfile* fallback = FindApplicationProfileForProcessIdentity(
        profiles, ProcessTargetIdentity{"Game.exe", R"(D:\Other\Different.exe)"});
    ASSERT_NE(fallback, nullptr);
    EXPECT_EQ(fallback->section, "Profile.Generic");
}

TEST(ProcessSelectorTest, ScreenCaptureUsesTheSelectedProfileForTheActualWindowOwner) {
    ApplicationProfile generic = Profile("Profile.Generic", "Game.exe");
    generic.videoCaptureExplicit = true;
    generic.resolvedVideoCapture = ApplicationVideoCapture::kWgc;
    ApplicationProfile specific = Profile("Profile.Modded", R"(Modded\Game.exe)");
    specific.target.windowName = "Shared title";
    specific.videoCaptureExplicit = true;
    specific.resolvedVideoCapture = ApplicationVideoCapture::kDxgiDup;
    const std::vector<ApplicationProfile> profiles{generic, specific};

    const ProcessTargetIdentity modded{"Game.exe", R"(D:\Games\Modded\Game.exe)"};
    const ProcessTargetIdentity other{"Game.exe", R"(D:\Games\Other\Game.exe)"};
    EXPECT_EQ(ce::profile_window::SelectedProfile(profiles, generic.target, modded), nullptr);
    EXPECT_EQ(ce::profile_window::SelectedProfile(profiles, specific.target, other), nullptr);
    const ApplicationProfile* selected = ce::profile_window::SelectedProfile(profiles, specific.target, modded);
    ASSERT_NE(selected, nullptr);
    EXPECT_EQ(selected->resolvedVideoCapture, ApplicationVideoCapture::kDxgiDup);
    EXPECT_TRUE(ce::profile_window::IsScreenCaptureTarget(selected));
    ASSERT_NE(ce::profile_window::SelectedProfile(profiles, generic.target, other), nullptr);

    specific.resolvedVideoCapture = ApplicationVideoCapture::kNone;
    const std::vector<ApplicationProfile> disabled{generic, specific};
    EXPECT_EQ(ce::profile_window::SelectedProfile(disabled, generic.target, modded), nullptr);
    EXPECT_FALSE(ce::profile_window::IsScreenCaptureTarget(
        ce::profile_window::SelectedProfile(disabled, specific.target, modded)));
}

TEST(ProcessSelectorTest, MatchesUnicodePathsAndNamesWithOrdinalCaseFolding) {
    const std::string gameName = "\xD0\x98\xD0\xB3\xD1\x80\xD0\xB0.exe";  // Игра.exe
    const std::string gameFolder = "\xD0\x98\xD0\xB3\xD1\x80\xD1\x8B";    // Игры
    const std::string cjkFolder = "\xE6\xB8\xB8\xE6\x88\x8F";             // 游戏
    const std::string path = "C:\\" + gameFolder + "\\" + cjkFolder + "\\" + gameName;
    const std::string upperName = "\xD0\x98\xD0\x93\xD0\xA0\xD0\x90.EXE";  // ИГРА.EXE

    const ProcessTargetIdentity identity{gameName, path};
    WhitelistEntry selector;
    selector.pattern = upperName;
    EXPECT_TRUE(MatchesProcessTarget(selector, identity, true));
    selector.pattern = gameFolder + "\\" + cjkFolder + "\\" + upperName;
    EXPECT_TRUE(MatchesProcessTarget(selector, identity, true));
    const std::wstring wideSelector = ce::utf8::ToWide(selector.pattern);
    EXPECT_EQ(wideSelector, ce::utf8::ToWide(ce::utf8::FromWide(wideSelector)));
}

TEST_F(ConfigOverrideTest, FullAndPartialProcessPathsSelectTheMatchingProfile) {
    WriteConfig(
        "[Video]\n"
        "fps=30\n"
        "[Profile.Generic]\n"
        "process=Game.exe\n"
        "video_capture=wgc\n"
        "Video.fps=60\n"
        "[Profile.Partial]\n"
        R"(process=GameFolder\Game.exe)"
        "\n"
        "video_capture=dxgi_dup\n"
        "Video.fps=90\n"
        R"([Profile.Full]
process=C:\Games\GameFolder\Game.exe
video_capture=inject
Video.fps=120
)");

    AppConfig full;
    LoadConfig(tempConfigFile, full, "Game.exe", nullptr, R"(C:\Games\GameFolder\Game.exe)");
    EXPECT_EQ(full.video.fps, 120);
    EXPECT_EQ(full.captureMethod, "inject");

    AppConfig partial;
    LoadConfig(tempConfigFile, partial, "Game.exe", nullptr, R"(E:\Archive\GameFolder\Game.exe)");
    EXPECT_EQ(partial.video.fps, 90);
    EXPECT_EQ(partial.captureMethod, "dxgi_dup");

    AppConfig fallback;
    LoadConfig(tempConfigFile, fallback, "Game.exe", nullptr, R"(E:\Archive\Other\Game.exe)");
    EXPECT_EQ(fallback.video.fps, 60);
    EXPECT_EQ(fallback.captureMethod, "wgc");

    AppConfig nameOnly;
    LoadConfig(tempConfigFile, nameOnly, "Game.exe");
    EXPECT_EQ(nameOnly.video.fps, 60);

    const ApplicationProfile* fullProfile = FindProfile(full.applicationProfiles, "Profile.Full");
    ASSERT_NE(fullProfile, nullptr);
    EXPECT_EQ(fullProfile->target.pattern, R"(C:\Games\GameFolder\Game.exe)");
    EXPECT_FALSE(fullProfile->target.HasWindow()) << "the drive-letter colon is not process:window syntax";
}

TEST_F(ConfigOverrideTest, QuotedAbsoluteProcessPathStillSupportsWindowSyntax) {
    WriteConfig(
        "[Video]\n"
        "fps=30\n"
        R"([Profile.Quoted]
process="C:\Games\GameFolder\Game.exe":"Game Window":exact
Video.fps=75
)");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "Game.exe", nullptr, R"(c:/games/gamefolder/game.exe)");
    EXPECT_EQ(config.video.fps, 75);

    const ApplicationProfile* profile = FindProfile(config.applicationProfiles, "Profile.Quoted");
    ASSERT_NE(profile, nullptr);
    EXPECT_EQ(profile->target.pattern, R"(C:\Games\GameFolder\Game.exe)");
    EXPECT_EQ(profile->target.windowName, "Game Window");
    EXPECT_EQ(profile->target.mode, MatchMode::kExact);
}

TEST_F(ConfigOverrideTest, UnicodeProcessPathSurvivesAUtf8ConfigLoad) {
    const std::string gameName = "\xD0\x98\xD0\xB3\xD1\x80\xD0\xB0.exe";  // Игра.exe
    const std::string gameFolder = "\xD0\x98\xD0\xB3\xD1\x80\xD1\x8B";    // Игры
    const std::string cjkFolder = "\xE6\xB8\xB8\xE6\x88\x8F";             // 游戏
    const std::string path = "C:\\" + gameFolder + "\\" + cjkFolder + "\\" + gameName;

    WriteConfig("[Video]\nfps=30\n[Profile.Unicode]\nprocess=" + path + "\nvideo_capture=wgc\nVideo.fps=123\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config, gameName, nullptr, path);
    EXPECT_EQ(config.video.fps, 123);
    EXPECT_EQ(config.captureMethod, "wgc");
    const ApplicationProfile* profile = FindProfile(config.applicationProfiles, "Profile.Unicode");
    ASSERT_NE(profile, nullptr);
    EXPECT_EQ(profile->target.pattern, path);
}

TEST_F(ConfigOverrideTest, PathProfileCanShadowTheInjectionRouteOfANameProfile) {
    WriteConfig(
        "[Profile.Generic]\n"
        "process=Game.exe\n"
        "video_capture=inject\n"
        "[Profile.Modded]\n"
        R"(process=Modded\Game.exe)"
        "\n"
        "video_capture=wgc\n"
        "dll_injection=never\n");

    AppConfig config;
    LoadConfig(tempConfigFile, config);

    const ApplicationProfile* generic = FindApplicationProfileForProcessIdentity(
        config.applicationProfiles, ProcessTargetIdentity{"Game.exe", R"(D:\Games\Default\Game.exe)"});
    ASSERT_NE(generic, nullptr);
    EXPECT_EQ(generic->section, "Profile.Generic");
    EXPECT_EQ(generic->injectionMode, ApplicationInjectionMode::kCapture);

    const ApplicationProfile* modded = FindApplicationProfileForProcessIdentity(
        config.applicationProfiles, ProcessTargetIdentity{"Game.exe", R"(E:\Games\Modded\Game.exe)"});
    ASSERT_NE(modded, nullptr);
    EXPECT_EQ(modded->section, "Profile.Modded");
    EXPECT_EQ(modded->injectionMode, ApplicationInjectionMode::kNone);
}

TEST_F(ConfigOverrideTest, ProfileAudioCarriesMoreSpecificAndLaterExclusions) {
    WriteConfig(
        "[Profile.Generic]\nprocess=Game.exe\naudio_enabled=true\n"
        "[Profile.Modded]\nprocess=Modded\\Game.exe\naudio_enabled=true\n"
        "[Profile.Other]\nprocess=Different.exe\naudio_enabled=true\n"
        "[Profile.NoAudio]\nprocess=Private\\Game.exe\naudio_enabled=false\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    const auto generic = std::find_if(config.audioSources.begin(), config.audioSources.end(), [](const AudioConfig& source) {
        return source.sourceType == AudioConfig::AppAudio && source.processName == "Game.exe";
    });
    ASSERT_NE(generic, config.audioSources.end());
    ASSERT_EQ(generic->processExclusions.size(), 2u);
    EXPECT_EQ(generic->processExclusions[0].pattern, R"(Modded\Game.exe)");
    EXPECT_EQ(generic->processExclusions[1].pattern, R"(Private\Game.exe)");
}
