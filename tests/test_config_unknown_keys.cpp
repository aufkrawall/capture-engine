#include "test_config_shared.h"

#include <cstdio>
#include <functional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "common/config/config_diagnostics.h"
#include "common/config/config_ini_reader.h"
#include "common/logging/logging.h"

// Unknown-key reporting: a key the document contains that no loader path reads is a warning
// diagnostic. The loader itself is the schema (it records what it looks up), so these tests
// pin the places where a plain lookup trace would be wrong: the shipped template, legacy
// spellings, gated groups, profile sections and aliases.

namespace {

namespace text = ce::config_text;

struct UnknownItem {
    ConfigDiagnosticKind kind;
    std::string section;
    std::string key;
};

std::vector<UnknownItem> UnknownEntries(const ConfigDiagnostics& diagnostics) {
    std::vector<UnknownItem> unknown;
    for (const ConfigDiagnostic& entry : diagnostics.entries) {
        if (entry.kind == ConfigDiagnosticKind::kUnknownKey || entry.kind == ConfigDiagnosticKind::kUnknownSection)
            unknown.push_back({entry.kind, entry.section, entry.key});
    }
    return unknown;
}

std::string Describe(const std::vector<UnknownItem>& unknown) {
    std::string description;
    for (const UnknownItem& entry : unknown)
        description += "[" + entry.section + "] " + (entry.key.empty() ? "(section)" : entry.key) + "; ";
    return description;
}

bool Contains(const std::vector<UnknownItem>& unknown, ConfigDiagnosticKind kind, const char* section, const char* key) {
    for (const UnknownItem& entry : unknown) {
        if (entry.kind == kind && entry.section == section && entry.key == key)
            return true;
    }
    return false;
}

// Loads `source` as a document under `process` and returns what the sink collected.
ConfigDiagnostics LoadDocument(const std::string& source, const char* process = "") {
    const text::IniDocument document = text::ParseUtf8Ini(source);
    AppConfig config;
    ConfigDiagnostics diagnostics;
    LoadConfigFromDocument(document, config, process, &diagnostics);
    return diagnostics;
}

std::string UncommentSettings(const std::string& templateText) {
    // The template documents optional sections and profile keys as ";[Section]" / ";key=value"
    // lines; prose comments start with "; ". Dropping the semicolon of the former turns the
    // template into a file that mentions every example key.
    static const std::regex commentedSetting(R"(^;(\[|[A-Za-z_][A-Za-z0-9_.\-]*=))");
    std::istringstream stream(templateText);
    std::string line;
    std::string result;
    while (std::getline(stream, line)) {
        std::smatch match;
        if (std::regex_search(line, match, commentedSetting))
            line.erase(0, 1);
        result += line + "\n";
    }
    return result;
}

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

}  // namespace

class ConfigUnknownKeysTest : public ConfigTest {};

TEST_F(ConfigUnknownKeysTest, ShippedTemplateHasNoUnknownKeys) {
    AppConfig config;
    ConfigDiagnostics diagnostics;
    LoadConfig(DefaultTemplatePath(), config, "", &diagnostics);
    const std::vector<UnknownItem> unknown = UnknownEntries(diagnostics);
    EXPECT_TRUE(unknown.empty()) << Describe(unknown);
}

TEST_F(ConfigUnknownKeysTest, TemplateWithEveryExampleUncommentedHasNoUnknownKeys) {
    const std::string uncommented = UncommentSettings(ReadTextFile(DefaultTemplatePath()));
    ASSERT_NE(uncommented.find("[Profile.My Game]"), std::string::npos);
    ASSERT_NE(uncommented.find("\nDesktopOverlay.mode=2"), std::string::npos);
    // Under a process none of the profiles names, and under one a profile does.
    for (const char* process : {"", "game.exe", "HeroesOfTheStorm_x64.exe"}) {
        const std::vector<UnknownItem> unknown = UnknownEntries(LoadDocument(uncommented, process));
        EXPECT_TRUE(unknown.empty()) << "process '" << process << "': " << Describe(unknown);
    }
}

TEST_F(ConfigUnknownKeysTest, MisspelledKeyIsReportedWithSectionAndKey) {
    const ConfigDiagnostics diagnostics = LoadDocument("[Video]\nfsp=60\nfps=60\n");
    const std::vector<UnknownItem> unknown = UnknownEntries(diagnostics);
    ASSERT_EQ(unknown.size(), 1u) << Describe(unknown);
    EXPECT_EQ(unknown[0].kind, ConfigDiagnosticKind::kUnknownKey);
    EXPECT_EQ(unknown[0].section, "Video");
    EXPECT_EQ(unknown[0].key, "fsp");
    EXPECT_EQ(diagnostics.entries.back().severity, ConfigDiagnosticSeverity::kWarning);
}

TEST_F(ConfigUnknownKeysTest, UnknownSectionIsReportedOnceNotPerKey) {
    const std::vector<UnknownItem> unknown = UnknownEntries(LoadDocument("[Vidoe]\na=1\nb=2\n[Vidoe]\nc=3\n"));
    ASSERT_EQ(unknown.size(), 1u) << Describe(unknown);
    EXPECT_EQ(unknown[0].kind, ConfigDiagnosticKind::kUnknownSection);
    EXPECT_EQ(unknown[0].section, "Vidoe");
    EXPECT_TRUE(unknown[0].key.empty());
}

TEST_F(ConfigUnknownKeysTest, NamesMatchIgnoringCaseAndDuplicatesAreNotFlagged) {
    const std::vector<UnknownItem> unknown = UnknownEntries(LoadDocument("[video]\nFPS=60\nfps=70\n"));
    EXPECT_TRUE(unknown.empty()) << Describe(unknown);
}

TEST_F(ConfigUnknownKeysTest, LegacyAndCanonicalSpellingsAreBothKnownWhenBothArePresent) {
    // The compat readers answer from the first location that has a value; the other spelling
    // is still a recognised key.
    const std::string source =
        "[Logging]\nlog_level=info\n"
        "[General]\nlog_level=debug\ndebug_logging=true\nperf_metrics_logging=false\ncapture_method=wgc\n"
        "[Capture]\ncapture_method=wgc\n"
        "[WGC]\nwgc_allow_lossy_bgra8_pool=true\nwgc_prefer_compact_10bit_pool=true\n"
        "[DLSS]\ndlss_fg_factor=3x\n[Graphics]\ndlss_fg_factor=2x\n"
        "[UE5]\nforce_ray_reconstruction=on\n[Graphics]\nforce_ray_reconstruction=on\n";
    const std::vector<UnknownItem> unknown = UnknownEntries(LoadDocument(source));
    EXPECT_TRUE(unknown.empty()) << Describe(unknown);
}

TEST_F(ConfigUnknownKeysTest, GatedStreamingKeysAreKnownWhileStreamingIsDisabled) {
    const std::string source =
        "[Streaming]\nenabled=false\nservice=twitch\nserver=rtmps://ingest.example/app\nstream_key=k\nurl=\n"
        "video_bitrate=0\naudio_bitrate=0\nmax_fps=60\nb_frames=0\nnonsense=1\n";
    const std::vector<UnknownItem> unknown = UnknownEntries(LoadDocument(source));
    ASSERT_EQ(unknown.size(), 1u) << Describe(unknown);
    EXPECT_EQ(unknown[0].key, "nonsense");
}

TEST_F(ConfigUnknownKeysTest, ListAndLineOrientedKeysAreKnown) {
    const std::string source =
        "[Injection]\nwhitelist=(\n  a.exe\n)\noverlay_whitelist=b.exe\nstartup_creator_hosts=c.exe\n"
        "[DesktopOverlay]\nprocess_list=a.exe|b.exe\n"
        "[pseudo-overlay]\nprocess_list=a.exe\n"
        "[Capture]\nwgc_window_detection=My Window\n";
    const std::vector<UnknownItem> unknown = UnknownEntries(LoadDocument(source));
    EXPECT_TRUE(unknown.empty()) << Describe(unknown);
}

TEST_F(ConfigUnknownKeysTest, ProfileSectionsAcceptOverridesAndDesktopOverlayAliases) {
    const std::string source =
        "[Profile.Game]\n"
        "process=game.exe\nvideo_capture=wgc\nmonitor=primary\nwindow_title=Game\nwindow_match=contains\n"
        "dll_injection=never\naudio_enabled=true\naudio_track=3\naudio_codec=aac\n"
        "Video.fps=60\ngraphics.vsync_mode=off\nfps=30\n"
        "DesktopOverlay.enabled=true\npseudo-overlay.size=40\nDESKTOPOVERLAY.pad=10\n"
        "always_render=true\nshow_encoder_overload_warnings=false\nforeground_acquire_grace_ms=100\n"
        "WindowHeartbeat.enabled=true\n"
        "Video.fsp=60\nnonsense_key=1\n";
    for (const char* process : {"", "game.exe"}) {
        const std::vector<UnknownItem> unknown = UnknownEntries(LoadDocument(source, process));
        ASSERT_EQ(unknown.size(), 2u) << process << ": " << Describe(unknown);
        EXPECT_TRUE(Contains(unknown, ConfigDiagnosticKind::kUnknownKey, "Profile.Game", "Video.fsp"));
        EXPECT_TRUE(Contains(unknown, ConfigDiagnosticKind::kUnknownKey, "Profile.Game", "nonsense_key"));
    }
}

TEST_F(ConfigUnknownKeysTest, GlobalKeyShadowedByTheMatchedProfileIsStillKnown) {
    // Under game.exe the profile's Video.fps answers and the global [Video] fps is never looked up;
    // it is the same key, so it must not be reported (and neither must the profile's spelling).
    const std::string source =
        "[Video]\nfps=30\n[DesktopOverlay]\nenabled=false\n"
        "[Profile.Game]\nprocess=game.exe\nVideo.fps=60\nDesktopOverlay.enabled=true\n";
    for (const char* process : {"", "game.exe"}) {
        const std::vector<UnknownItem> unknown = UnknownEntries(LoadDocument(source, process));
        EXPECT_TRUE(unknown.empty()) << process << ": " << Describe(unknown);
    }
}

TEST_F(ConfigUnknownKeysTest, LegacyAppProfileKeysFollowTheSameRules) {
    const std::vector<UnknownItem> unknown =
        UnknownEntries(LoadDocument("[App.1]\nProcess=game.exe\nVideo.fps=60\nvideo_capture=wgc\n"));
    // Legacy profiles do not read video_capture (they only know the injection form).
    ASSERT_EQ(unknown.size(), 1u) << Describe(unknown);
    EXPECT_EQ(unknown[0].key, "video_capture");
}

TEST_F(ConfigUnknownKeysTest, KeysOfARejectedProfileAreLeftToTheProfileWarning) {
    const ConfigDiagnostics diagnostics = LoadDocument("[Profile.NoTarget]\ndll_injection=never\nfoo=1\n");
    EXPECT_TRUE(UnknownEntries(diagnostics).empty()) << Describe(UnknownEntries(diagnostics));
    EXPECT_EQ(diagnostics.Count(ConfigDiagnosticKind::kInvalidValue), 1u);
}

TEST_F(ConfigUnknownKeysTest, NumberedAudioGroupWithoutItsEnablingKeyIsReportedAsUnused) {
    // [Microphone.2] is only read when enabled= is present: its other keys are inert, and say so.
    const std::vector<UnknownItem> inactive = UnknownEntries(LoadDocument("[Microphone.2]\ndevice=Mic\n"));
    ASSERT_EQ(inactive.size(), 1u) << Describe(inactive);
    EXPECT_EQ(inactive[0].key, "device");
    const std::vector<UnknownItem> active =
        UnknownEntries(LoadDocument("[Microphone.2]\nenabled=true\ndevice=Mic\ntrack=3\ncodec=aac\n"));
    EXPECT_TRUE(active.empty()) << Describe(active);
}

TEST_F(ConfigUnknownKeysTest, PathLoadOfUtf8AndAnsiFilesReportsTheSameKeys) {
    const std::string utf8 = "\xEF\xBB\xBF[Video]\r\nfsp=60\r\n[Output]\r\noutput_dir=J\xC3\xBCrgen\r\n[Typo]\r\nx=1\r\n";
    WriteConfig(utf8);
    AppConfig config;
    ConfigDiagnostics fromFile;
    LoadConfig(tempConfigFile, config, "", &fromFile);
    const ConfigDiagnostics fromDocument = LoadDocument(utf8);
    EXPECT_EQ(Describe(UnknownEntries(fromFile)), Describe(UnknownEntries(fromDocument)));
    EXPECT_EQ(UnknownEntries(fromFile).size(), 2u);

    // An ANSI-saved file goes through the profile API; its keys are enumerated through it too.
    WriteConfig("[Video]\r\nfsp=60\r\n[Output]\r\noutput_dir=J\xFCrgen\r\n[Typo]\r\nx=1\r\n");
    ConfigDiagnostics ansi;
    LoadConfig(tempConfigFile, config, "", &ansi);
    EXPECT_EQ(Describe(UnknownEntries(ansi)), Describe(UnknownEntries(fromDocument)));
}

TEST_F(ConfigUnknownKeysTest, TrackingIsOptInAndSwitchable) {
    const std::string source = "[Video]\nfsp=60\n";
    WriteConfig(source);
    AppConfig config;
    LoadConfig(tempConfigFile, config);  // no sink: no pass, no cost
    ConfigDiagnostics quiet;
    quiet.trackUnknownKeys = false;
    LoadConfig(tempConfigFile, config, "", &quiet);
    EXPECT_TRUE(quiet.entries.empty());
}

TEST_F(ConfigUnknownKeysTest, LogGetsOneCappedSummaryLinePerChangedList) {
    std::string many = "[Video]\n";
    for (int i = 0; i < 30; ++i)
        many += "summary_typo_" + std::to_string(i) + "=1\n";
    const text::IniDocument document = text::ParseUtf8Ini(many);
    const std::string logFile = MakeTestPath("test_config_unknown_summary.log");
    remove(logFile.c_str());
    Log_Init(logFile, LogLevel::Debug);
    AppConfig config;
    ConfigDiagnostics first;
    LoadConfigFromDocument(document, config, "", &first);
    ConfigDiagnostics second;
    LoadConfigFromDocument(document, config, "", &second);
    Log_Shutdown();
    const std::string log = ReadTextFile(logFile);
    remove(logFile.c_str());

    EXPECT_EQ(UnknownEntries(first).size(), 30u);
    EXPECT_EQ(UnknownEntries(second).size(), 30u);  // the sink always gets everything
    size_t summaries = 0;
    for (const std::string& line : WarningLines(log)) {
        if (line.find("no setting reads") == std::string::npos)
            continue;
        ++summaries;
        EXPECT_NE(line.find("30 entries"), std::string::npos) << line;
        EXPECT_NE(line.find("(+22 more)"), std::string::npos) << line;
        EXPECT_NE(line.find("[Video] summary_typo_0"), std::string::npos) << line;
        EXPECT_EQ(line.find("summary_typo_20"), std::string::npos) << line;
    }
    EXPECT_EQ(summaries, 1u) << log;  // an unchanged list on reload is not logged again
}
