#include "test_config_shared.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "common/config/config_diagnostics.h"
#include "common/config/config_ini_reader.h"
#include "common/logging/logging.h"

// A profile override may name a setting as Section.key or, as old configs did, by its bare key.
// A bare key overrides every section that reads that name; that stays so for compatibility, and
// the loader reports each bare key that changed more than one section, naming the qualified form.

namespace {

namespace text = ce::config_text;

AppConfig Load(const std::string& source, const char* process, ConfigDiagnostics* diagnostics) {
    const text::IniDocument document = text::ParseUtf8Ini(source);
    AppConfig config;
    LoadConfigFromDocument(document, config, process, diagnostics);
    return config;
}

std::vector<ConfigDiagnostic> BareKeyNotices(const ConfigDiagnostics& diagnostics) {
    std::vector<ConfigDiagnostic> notices;
    for (const ConfigDiagnostic& entry : diagnostics.entries) {
        if (entry.kind == ConfigDiagnosticKind::kNotice && entry.message.find("bare key") != std::string::npos)
            notices.push_back(entry);
    }
    return notices;
}

std::string Describe(const std::vector<ConfigDiagnostic>& notices) {
    std::string description;
    for (const ConfigDiagnostic& entry : notices)
        description += "[" + entry.section + "] " + entry.key + ": " + entry.message + "; ";
    return description;
}

}  // namespace

TEST(ConfigProfileBareKeysTest, SharedBareKeyStillChangesEverySectionAndIsReported) {
    const std::string source =
        "[Video]\nfps=120\n[FaceCamera]\nfps=30\n"
        "[Profile.Game]\nprocess=game.exe\nfps=60\n";
    ConfigDiagnostics diagnostics;
    const AppConfig config = Load(source, "game.exe", &diagnostics);

    // Old configs relied on this: the bare key reaches both sections.
    EXPECT_EQ(config.video.fps, 60);
    EXPECT_EQ(config.video.faceCamera.requestedFps, 60u);

    const std::vector<ConfigDiagnostic> notices = BareKeyNotices(diagnostics);
    ASSERT_EQ(notices.size(), 1u) << Describe(notices);
    EXPECT_EQ(notices[0].section, "Profile.Game");
    EXPECT_EQ(notices[0].key, "fps");
    EXPECT_EQ(notices[0].severity, ConfigDiagnosticSeverity::kWarning);
    EXPECT_NE(notices[0].message.find("2 sections ([FaceCamera], [Video])"), std::string::npos) << notices[0].message;
    EXPECT_NE(notices[0].message.find("Section.key"), std::string::npos) << notices[0].message;
}

TEST(ConfigProfileBareKeysTest, UniqueQualifiedAndPartlyQualifiedKeysAreNotReported) {
    // vsync_mode exists only in [Graphics]; Video.fps takes [Video], so the bare fps reaches only
    // [FaceCamera]. Each key changes one section, as written.
    const std::string source =
        "[Video]\nfps=120\n[FaceCamera]\nfps=30\n[Graphics]\nvsync_mode=default\n"
        "[Profile.Game]\nprocess=game.exe\nmonitor=primary\naudio_enabled=true\naudio_track=3\n"
        "vsync_mode=off\nVideo.fps=60\nfps=15\n";
    ConfigDiagnostics diagnostics;
    const AppConfig config = Load(source, "game.exe", &diagnostics);

    EXPECT_EQ(config.graphics.vsyncMode, "off");
    EXPECT_EQ(config.video.fps, 60);
    EXPECT_EQ(config.video.faceCamera.requestedFps, 15u);
    EXPECT_TRUE(BareKeyNotices(diagnostics).empty()) << Describe(BareKeyNotices(diagnostics));
}

TEST(ConfigProfileBareKeysTest, UnmatchedProcessNeitherAppliesNorReportsTheProfile) {
    const std::string source = "[Video]\nfps=120\n[Profile.Game]\nprocess=game.exe\nfps=60\nenabled=true\n";
    ConfigDiagnostics diagnostics;
    const AppConfig config = Load(source, "other.exe", &diagnostics);

    EXPECT_EQ(config.video.fps, 120);
    EXPECT_TRUE(BareKeyNotices(diagnostics).empty()) << Describe(BareKeyNotices(diagnostics));
}

TEST(ConfigProfileBareKeysTest, BareEnabledReachesTheNumberedSourcesAndTheListIsCapped) {
    // The hazard the warning exists for: one bare enabled=true turns on streaming, the camera, the
    // microphone and every numbered audio source as well.
    ConfigDiagnostics diagnostics;
    const AppConfig config = Load("[Profile.Game]\nprocess=game.exe\nenabled=true\n", "game.exe", &diagnostics);

    EXPECT_TRUE(config.video.faceCamera.enabled);
    const std::vector<ConfigDiagnostic> notices = BareKeyNotices(diagnostics);
    ASSERT_EQ(notices.size(), 1u) << Describe(notices);
    EXPECT_EQ(notices[0].key, "enabled");
    EXPECT_NE(notices[0].message.find(" more)"), std::string::npos) << notices[0].message;
    EXPECT_NE(notices[0].message.find("[FaceCamera]"), std::string::npos) << notices[0].message;
}

TEST(ConfigProfileBareKeysTest, LogGetsOneLinePerChangedFanOut) {
    const std::string source =
        "[Video]\nfps=120\n[FaceCamera]\nfps=30\n"
        "[Profile.BareKeyLogProbe]\nprocess=probe.exe\nfps=60\nbitrate=90Mbps\n";
    const std::string logFile = MakeTestPath("test_config_bare_keys.log");
    remove(logFile.c_str());
    Log_Init(logFile, LogLevel::Debug);
    ConfigDiagnostics first;
    Load(source, "probe.exe", &first);
    ConfigDiagnostics second;
    Load(source, "probe.exe", &second);
    Log_Shutdown();
    const std::string log = ReadTextFile(logFile);
    remove(logFile.c_str());

    EXPECT_EQ(BareKeyNotices(first).size(), 2u) << Describe(BareKeyNotices(first));
    EXPECT_EQ(BareKeyNotices(second).size(), 2u);  // the sink always gets every key
    size_t lines = 0;
    std::istringstream stream(log);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("[Profile.BareKeyLogProbe] 2 bare key(s) change several sections") == std::string::npos)
            continue;
        ++lines;
        EXPECT_NE(line.find("[WARN]"), std::string::npos) << line;
        EXPECT_NE(line.find("bitrate -> [Microphone], [SystemAudio], [Video]"), std::string::npos) << line;
        EXPECT_NE(line.find("fps -> [FaceCamera], [Video]"), std::string::npos) << line;
    }
    EXPECT_EQ(lines, 1u) << log;  // an unchanged profile on reload is not logged again
}
