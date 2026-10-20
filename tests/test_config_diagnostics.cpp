#include "test_config_shared.h"

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "common/config/config_diagnostics.h"
#include "common/config/config_ini_reader.h"
#include "common/logging/logging.h"

// LoadConfigFromDocument / the diagnostics sink: a document loads exactly like the file it
// was parsed from, and every loader warning reaches the sink with its section and key.

namespace {

namespace text = ce::config_text;

// The warning sources of the loader in one ASCII file (ASCII, so the profile API and the
// UTF-8 parse agree on any code page): value boundaries, hotkey/limiter tokens, retired
// and deprecated keys, a sample rate, and a profile without a target.
const char kBaseWarnings[] =
    "[Capture]\n"
    "capture_method=wgc2\n"
    "[Video]\n"
    "fps=bogus\n"
    "[Hotkeys]\n"
    "toggle_overlay=Ctrl+F99\n"
    "[FpsLimiter]\n"
    "general_limiter_mode=bogus\n"
    "capture_sync_limiter_mode=fg_fallback\n"
    "[WGC]\n"
    "wgc_prefer_compact_10bit_pool=true\n"
    "[SystemAudio]\n"
    "sample_rate=48kHz\n"
    "[Profile.NoTarget]\n"
    "dll_injection=never\n";

// Two profiles for one process. The overlap warning has its own per-process log budget, which
// the other tests in this binary draw on, so only the sink tests (not the log comparison) use it.
const char kOverlappingProfiles[] =
    "[Profile.One]\n"
    "process=game.exe\n"
    "video_capture=wgc\n"
    "[Profile.Two]\n"
    "process=GAME.exe\n"
    "video_capture=wgc\n";

// Built on first use: a namespace-scope std::string would allocate during static initialization.
const std::string& WarningFixture() {
    static const std::string fixture = std::string(kBaseWarnings) + kOverlappingProfiles;
    return fixture;
}

const ConfigDiagnostic* Find(const ConfigDiagnostics& diagnostics, const char* section, const char* key) {
    for (const ConfigDiagnostic& entry : diagnostics.entries) {
        if (entry.section == section && entry.key == key)
            return &entry;
    }
    return nullptr;
}

// "[WARN] message" from every log line, timestamps dropped (they are the only difference
// between two loads of the same text).
std::vector<std::string> WarningLines(const std::string& log) {
    std::vector<std::string> lines;
    std::istringstream stream(log);
    std::string line;
    while (std::getline(stream, line)) {
        const size_t stamp = line.find("] ");
        if (stamp != std::string::npos && line.find("[WARN]", stamp) == stamp + 2)
            lines.push_back(line.substr(stamp + 2));
    }
    return lines;
}

class ConfigDiagnosticsTest : public ConfigTest {
protected:
    // LoadConfig with logging on; returns the warning lines it wrote.
    std::vector<std::string> LoadLogged(const std::function<void()>& load) {
        const std::string logFile = MakeTestPath("test_config_diagnostics.log");
        remove(logFile.c_str());
        Log_Init(logFile, LogLevel::Debug);
        load();
        Log_Shutdown();
        const std::string log = ReadTextFile(logFile);
        remove(logFile.c_str());
        return WarningLines(log);
    }
};

}  // namespace

TEST_F(ConfigDiagnosticsTest, SinkReceivesEveryLoaderWarningWithSectionAndKey) {
    WriteConfig(WarningFixture());
    AppConfig config;
    ConfigDiagnostics diagnostics;
    LoadConfig(tempConfigFile, config, "", &diagnostics);

    const ConfigDiagnostic* capture = Find(diagnostics, "Capture", "capture_method");
    ASSERT_NE(capture, nullptr);
    EXPECT_EQ(capture->kind, ConfigDiagnosticKind::kInvalidValue);
    EXPECT_EQ(capture->severity, ConfigDiagnosticSeverity::kWarning);
    EXPECT_NE(capture->message.find("'wgc2'"), std::string::npos) << capture->message;

    const ConfigDiagnostic* fps = Find(diagnostics, "Video", "fps");
    ASSERT_NE(fps, nullptr);
    EXPECT_NE(fps->message.find("'bogus'"), std::string::npos) << fps->message;
    EXPECT_NE(Find(diagnostics, "Hotkeys", "toggle_overlay"), nullptr);
    EXPECT_NE(Find(diagnostics, "FpsLimiter", "general_limiter_mode"), nullptr);
    EXPECT_NE(Find(diagnostics, "FpsLimiter", "capture_sync_limiter_mode"), nullptr);  // retired token
    EXPECT_NE(Find(diagnostics, "WGC", "wgc_prefer_compact_10bit_pool"), nullptr);     // deprecated key
    EXPECT_NE(Find(diagnostics, "SystemAudio", "sample_rate"), nullptr);

    const ConfigDiagnostic* noTarget = Find(diagnostics, "Profile.NoTarget", "process/window_title");
    ASSERT_NE(noTarget, nullptr);
    EXPECT_EQ(noTarget->kind, ConfigDiagnosticKind::kInvalidValue);
    bool overlap = false;
    for (const ConfigDiagnostic& entry : diagnostics.entries) {
        overlap = overlap || (entry.kind == ConfigDiagnosticKind::kProfile && entry.section == "Profile.Two" &&
                              entry.message.find("overlaps [Profile.One]") != std::string::npos);
    }
    EXPECT_TRUE(overlap);
    EXPECT_EQ(diagnostics.dropped, 0u);
}

TEST_F(ConfigDiagnosticsTest, DocumentLoadReportsTheSameWarningsAsPathLoad) {
    WriteConfig(WarningFixture());
    AppConfig fromFile;
    ConfigDiagnostics fileDiagnostics;
    LoadConfig(tempConfigFile, fromFile, "", &fileDiagnostics);

    const text::IniDocument document = text::ParseUtf8Ini(WarningFixture());
    AppConfig fromDocument;
    ConfigDiagnostics documentDiagnostics;
    LoadConfigFromDocument(document, fromDocument, "", &documentDiagnostics);

    ASSERT_FALSE(fileDiagnostics.entries.empty());
    ASSERT_EQ(documentDiagnostics.entries.size(), fileDiagnostics.entries.size());
    for (size_t i = 0; i < fileDiagnostics.entries.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(documentDiagnostics.entries[i].kind, fileDiagnostics.entries[i].kind);
        EXPECT_EQ(documentDiagnostics.entries[i].section, fileDiagnostics.entries[i].section);
        EXPECT_EQ(documentDiagnostics.entries[i].key, fileDiagnostics.entries[i].key);
        EXPECT_EQ(documentDiagnostics.entries[i].message, fileDiagnostics.entries[i].message);
    }

    // The loaded values agree as well, including the profile routing the warnings describe.
    EXPECT_EQ(fromDocument.captureMethod, fromFile.captureMethod);
    EXPECT_EQ(fromDocument.video.fps, fromFile.video.fps);
    EXPECT_EQ(fromDocument.hotkeyToggleOverlay.vkey, fromFile.hotkeyToggleOverlay.vkey);
    EXPECT_EQ(fromDocument.fpsLimiter.generalLimiterMode, fromFile.fpsLimiter.generalLimiterMode);
    ASSERT_EQ(fromDocument.applicationProfiles.size(), fromFile.applicationProfiles.size());
    EXPECT_EQ(fromDocument.applicationProfiles.front().section, fromFile.applicationProfiles.front().section);
}

TEST_F(ConfigDiagnosticsTest, SinkDoesNotChangeTheLogOutputOrTheLoadedConfig) {
    WriteConfig(WarningFixture());
    AppConfig plain;
    const std::vector<std::string> plainLog = LoadLogged([&] { LoadConfig(tempConfigFile, plain); });
    AppConfig sinked;
    ConfigDiagnostics diagnostics;
    const std::vector<std::string> sinkedLog =
        LoadLogged([&] { LoadConfig(tempConfigFile, sinked, "", &diagnostics); });
    const text::IniDocument document = text::ParseUtf8Ini(WarningFixture());
    AppConfig fromDocument;
    const std::vector<std::string> documentLog =
        LoadLogged([&] { LoadConfigFromDocument(document, fromDocument, ""); });

    ASSERT_FALSE(plainLog.empty());
    EXPECT_EQ(sinkedLog, plainLog);
    EXPECT_EQ(documentLog, plainLog);
    EXPECT_EQ(sinked.captureMethod, plain.captureMethod);
    EXPECT_EQ(sinked.video.fps, plain.video.fps);
    EXPECT_EQ(sinked.applicationProfiles.size(), plain.applicationProfiles.size());
}

TEST_F(ConfigDiagnosticsTest, SinkReceivesWarningsWithLoggingDisabledAndQuietScope) {
    // The sink is independent of the log: no Log_Init here (warnings are dropped by the
    // logger), and ScopedQuietConfigLog only silences the informational narrative.
    WriteConfig(WarningFixture());
    AppConfig config;
    ConfigDiagnostics diagnostics;
    {
        ScopedQuietConfigLog quiet;
        LoadConfig(tempConfigFile, config, "", &diagnostics);
    }
    EXPECT_NE(Find(diagnostics, "Video", "fps"), nullptr);
    EXPECT_NE(Find(diagnostics, "FpsLimiter", "capture_sync_limiter_mode"), nullptr);
}

TEST_F(ConfigDiagnosticsTest, ValueTheCodePageCannotRepresentIsReportedWithItsKey) {
    // U+2603 is in no ANSI code page: the loader hands out '?' for it (as it always did) and the
    // sink now says which setting it was.
    bool lossy = false;
    text::WideToCodePage(L"\u2603", GetACP(), &lossy);
    if (!lossy)
        GTEST_SKIP() << "the active code page can represent the sample character";
    const text::IniDocument document = text::ParseUtf8Ini("[Output]\noutput_dir=D:\\\xE2\x98\x83\n");
    AppConfig config;
    ConfigDiagnostics diagnostics;
    LoadConfigFromDocument(document, config, "", &diagnostics);
    const ConfigDiagnostic* entry = Find(diagnostics, "Output", "output_dir");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->kind, ConfigDiagnosticKind::kNotice);
    EXPECT_NE(entry->message.find("cannot represent"), std::string::npos) << entry->message;
}

TEST_F(ConfigDiagnosticsTest, SinkIsCappedAndCountsTheRest) {
    // One key cannot warn twice, so spread the warnings over distinct invalid profile sections.
    std::string profiles;
    for (size_t i = 0; i < ConfigDiagnostics::kMaxEntries + 40; ++i)
        profiles += "[Profile.NoTarget" + std::to_string(i) + "]\ndll_injection=never\n";
    const text::IniDocument document = text::ParseUtf8Ini(profiles);
    AppConfig config;
    ConfigDiagnostics diagnostics;
    LoadConfigFromDocument(document, config, "", &diagnostics);
    EXPECT_EQ(diagnostics.entries.size(), ConfigDiagnostics::kMaxEntries);
    EXPECT_EQ(diagnostics.dropped, 40u);
}

TEST_F(ConfigDiagnosticsTest, DocumentLoadAppliesProfileOverridesForTheNamedProcess) {
    const std::string source =
        "[Video]\nfps=60\n"
        "[Profile.One]\nprocess=game.exe\nVideo.fps=144\n";
    const text::IniDocument document = text::ParseUtf8Ini(source);
    AppConfig other;
    LoadConfigFromDocument(document, other, "other.exe");
    EXPECT_EQ(other.video.fps, 60);
    AppConfig game;
    LoadConfigFromDocument(document, game, "game.exe");
    EXPECT_EQ(game.video.fps, 144);
}

TEST_F(ConfigDiagnosticsTest, DocumentLoadKeepsMultiLineWhitelistAndBomText) {
    // The whitelist pass is line-oriented text (parenthesized multi-line lists, a BOM on the
    // first line); a document must hand it the same bytes the file would.
    // (The BOM sits on a harmless first section: the line-oriented pass does not recognise a
    // BOM-prefixed header, for a file and a document alike.)
    const std::string source =
        "\xEF\xBB\xBF[General]\r\n"
        "[Injection]\r\n"
        "whitelist=(\r\n"
        "  first.exe\r\n"
        "  second.exe\r\n"
        ")\r\n"
        "overlay_whitelist=third.exe\r\n";
    WriteConfig(source);
    AppConfig fromFile;
    LoadConfig(tempConfigFile, fromFile);
    const text::IniDocument document = text::ParseUtf8Ini(source);
    AppConfig fromDocument;
    LoadConfigFromDocument(document, fromDocument, "");

    ASSERT_EQ(fromFile.gameWhitelist.size(), 2u);
    ASSERT_EQ(fromDocument.gameWhitelist.size(), fromFile.gameWhitelist.size());
    ASSERT_EQ(fromFile.overlayWhitelist.size(), 1u);
    ASSERT_EQ(fromDocument.overlayWhitelist.size(), fromFile.overlayWhitelist.size());
    for (size_t i = 0; i < fromFile.gameWhitelist.size(); ++i)
        EXPECT_EQ(fromDocument.gameWhitelist[i].pattern, fromFile.gameWhitelist[i].pattern);
    EXPECT_EQ(text::SerializeIni(document), source);
}
