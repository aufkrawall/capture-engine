#include "test_config_shared.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "common/config/config_diagnostics.h"
#include "common/config/config_text_encoding.h"

// Per-profile [DesktopOverlay] overrides. They used to be parsed a second time by the
// controller (re-reading the file by path with its own bool/int parsers); the loader now
// resolves them into ApplicationProfile::desktopOverlay. The reference below is that old
// parser, kept verbatim, so the fixture proves the semantics did not move.

namespace {

namespace text = ce::config_text;

bool ReferenceTryParseBool(std::string_view val, bool& out) {
    if (val == "1" || _stricmp(std::string(val).c_str(), "true") == 0 ||
        _stricmp(std::string(val).c_str(), "on") == 0 || _stricmp(std::string(val).c_str(), "yes") == 0) {
        out = true;
        return true;
    }
    if (val == "0" || _stricmp(std::string(val).c_str(), "false") == 0 ||
        _stricmp(std::string(val).c_str(), "off") == 0 || _stricmp(std::string(val).c_str(), "no") == 0) {
        out = false;
        return true;
    }
    return false;
}

bool ReferenceTryParseInt(std::string_view val, int minVal, int maxVal, int& out) {
    char* end = nullptr;
    std::string s(val);
    long v = strtol(s.c_str(), &end, 10);
    if (end != s.c_str() && *end == '\0') {
        out = std::clamp(static_cast<int>(v), minVal, maxVal);
        return true;
    }
    return false;
}

// ParseProfileDesktopOverlayOverrides as it was in captureengine/app/main_controller.cpp.
PseudoOverlayConfig ReferenceOverrides(const std::string& path, const std::string& section,
                                       const PseudoOverlayConfig& base) {
    std::vector<std::string> sectionLines;
    if (!text::ReadIniSectionLines(path, section, &sectionLines))
        return base;

    PseudoOverlayConfig cfg = base;
    for (const std::string& line : sectionLines) {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        key.erase(0, key.find_first_not_of(" \t"));
        const size_t keyEnd = key.find_last_not_of(" \t");
        if (keyEnd != std::string::npos)
            key.erase(keyEnd + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        const size_t valEnd = val.find_last_not_of(" \t");
        if (valEnd != std::string::npos)
            val.erase(valEnd + 1);

        std::string lowerKey;
        for (char c : key)
            lowerKey.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

        if (lowerKey == "desktopoverlay.enabled" || lowerKey == "pseudo-overlay.enabled") {
            ReferenceTryParseBool(val, cfg.enabled);
        } else if (lowerKey == "desktopoverlay.size" || lowerKey == "pseudo-overlay.size") {
            ReferenceTryParseInt(val, 10, 200, cfg.size);
        } else if (lowerKey == "desktopoverlay.pad" || lowerKey == "pseudo-overlay.pad") {
            ReferenceTryParseInt(val, 0, 100, cfg.pad);
        } else if (lowerKey == "desktopoverlay.pos" || lowerKey == "pseudo-overlay.pos") {
            ReferenceTryParseInt(val, 0, 3, cfg.pos);
        } else if (lowerKey == "desktopoverlay.mode" || lowerKey == "pseudo-overlay.mode") {
            ReferenceTryParseInt(val, 0, 2, cfg.mode);
        } else if (lowerKey == "desktopoverlay.always_render" || lowerKey == "pseudo-overlay.always_render" ||
                   lowerKey == "always_render") {
            ReferenceTryParseBool(val, cfg.alwaysRender);
        } else if (lowerKey == "desktopoverlay.always_render_only_when_game" ||
                   lowerKey == "pseudo-overlay.always_render_only_when_game" ||
                   lowerKey == "always_render_only_when_game") {
            ReferenceTryParseBool(val, cfg.alwaysRenderOnlyWhenGame);
        } else if (lowerKey == "desktopoverlay.show_encoder_overload_warnings" ||
                   lowerKey == "pseudo-overlay.show_encoder_overload_warnings" ||
                   lowerKey == "show_encoder_overload_warnings") {
            ReferenceTryParseBool(val, cfg.showEncoderOverloadWarn);
        } else if (lowerKey == "desktopoverlay.foreground_acquire_grace_ms" ||
                   lowerKey == "pseudo-overlay.foreground_acquire_grace_ms" ||
                   lowerKey == "foreground_acquire_grace_ms") {
            ReferenceTryParseInt(val, 0, 10000, cfg.foregroundAcquireGraceMs);
        }
    }
    return cfg;
}

void ExpectSameOverlay(const PseudoOverlayConfig& actual, const PseudoOverlayConfig& expected, const char* what) {
    SCOPED_TRACE(what);
    EXPECT_EQ(actual.enabled, expected.enabled);
    EXPECT_EQ(actual.size, expected.size);
    EXPECT_EQ(actual.pad, expected.pad);
    EXPECT_EQ(actual.pos, expected.pos);
    EXPECT_EQ(actual.mode, expected.mode);
    EXPECT_EQ(actual.alwaysRender, expected.alwaysRender);
    EXPECT_EQ(actual.alwaysRenderOnlyWhenGame, expected.alwaysRenderOnlyWhenGame);
    EXPECT_EQ(actual.showEncoderOverloadWarn, expected.showEncoderOverloadWarn);
    EXPECT_EQ(actual.foregroundAcquireGraceMs, expected.foregroundAcquireGraceMs);
    EXPECT_EQ(actual.processList, expected.processList);
}

const ApplicationProfile* FindProfile(const AppConfig& config, const char* section) {
    for (const ApplicationProfile& profile : config.applicationProfiles) {
        if (_stricmp(profile.section.c_str(), section) == 0)
            return &profile;
    }
    return nullptr;
}

const char kBaseSection[] =
    "[DesktopOverlay]\n"
    "enabled=true\nsize=40\npad=10\npos=2\nmode=1\n"
    "always_render=false\nalways_render_only_when_game=false\nshow_encoder_overload_warnings=true\n"
    "foreground_acquire_grace_ms=1500\nprocess_list=base.exe|other.exe\n\n";

}  // namespace

class ConfigProfileDesktopOverlayTest : public ConfigTest {
protected:
    // Loads the fixture from the file (so the reference parser reads the same path) and from a document.
    void ExpectProfilesMatchReference(const std::string& body, std::vector<const char*> sections) {
        const std::string source = std::string(kBaseSection) + body;
        WriteConfig(source);
        AppConfig fromFile;
        LoadConfig(tempConfigFile, fromFile);
        const text::IniDocument document = text::ParseUtf8Ini(source);
        AppConfig fromDocument;
        LoadConfigFromDocument(document, fromDocument, "");
        for (const char* section : sections) {
            SCOPED_TRACE(section);
            const PseudoOverlayConfig expected = ReferenceOverrides(tempConfigFile, section, fromFile.pseudoOverlay);
            const ApplicationProfile* viaFile = FindProfile(fromFile, section);
            const ApplicationProfile* viaDocument = FindProfile(fromDocument, section);
            ASSERT_NE(viaFile, nullptr);
            ASSERT_NE(viaDocument, nullptr);
            ExpectSameOverlay(viaFile->desktopOverlay, expected, "file");
            ExpectSameOverlay(viaDocument->desktopOverlay, expected, "document");
        }
    }
};

TEST_F(ConfigProfileDesktopOverlayTest, ProfileWithoutOverridesKeepsTheBaseValues) {
    WriteConfig(std::string(kBaseSection) + "[Profile.Plain]\nprocess=plain.exe\nvideo_capture=wgc\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    const ApplicationProfile* plain = FindProfile(config, "Profile.Plain");
    ASSERT_NE(plain, nullptr);
    ExpectSameOverlay(plain->desktopOverlay, config.pseudoOverlay, "base");
    EXPECT_EQ(plain->desktopOverlay.size, 40);
    EXPECT_EQ(plain->desktopOverlay.processList, "base.exe|other.exe");
}

TEST_F(ConfigProfileDesktopOverlayTest, AliasesClampingAndLastLineWinsMatchTheOldParser) {
    ExpectProfilesMatchReference(
        "[Profile.A]\nprocess=a.exe\nvideo_capture=wgc\n"
        "DesktopOverlay.enabled=false\n"
        "DesktopOverlay.size=500\n"                       // clamped to 200
        "pseudo-overlay.pad=-5\n"                         // clamped to 0
        "PSEUDO-OVERLAY.pos=9\n"                          // clamped to 3, key case ignored
        "DesktopOverlay.mode=abc\n"                       // unparsable: the base value stays
        "always_render=on\n"                              // bare spelling
        "desktopoverlay.always_render_only_when_game=YES\n"
        "show_encoder_overload_warnings=off\n"
        "foreground_acquire_grace_ms=99999\n"             // clamped to 10000
        "[Profile.B]\nprocess=b.exe\nvideo_capture=wgc\n"
        "pseudo-overlay.size=33\nDesktopOverlay.size=44\n"  // both spellings: the later line wins
        "DesktopOverlay.pad=7\npseudo-overlay.pad=8\n"
        "DesktopOverlay.pos=1\nDesktopOverlay.pos=oops\n"   // a later unparsable line changes nothing
        "enabled=false\nsize=99\n"                          // bare enabled/size are not overlay keys
        "[Profile.C]\nprocess=c.exe\nvideo_capture=wgc\n"
        "DesktopOverlay.foreground_acquire_grace_ms=0\n"
        "DesktopOverlay.show_encoder_overload_warnings=0\n"
        "DesktopOverlay.always_render=1\n"
        "DesktopOverlay.size= 25 \n"
        "DesktopOverlay.mode=+2\n"                          // strtol accepts a sign
        "DesktopOverlay.pad=12px\n",                        // trailing junk: rejected
        {"Profile.A", "Profile.B", "Profile.C"});

    // The values themselves, not only the equality with the reference.
    WriteConfig(std::string(kBaseSection) +
                "[Profile.A]\nprocess=a.exe\nDesktopOverlay.size=500\npseudo-overlay.pad=-5\n"
                "PSEUDO-OVERLAY.pos=9\nDesktopOverlay.mode=abc\nalways_render=on\n"
                "foreground_acquire_grace_ms=99999\nDesktopOverlay.enabled=false\n"
                "[Profile.B]\nprocess=b.exe\npseudo-overlay.size=33\nDesktopOverlay.size=44\nenabled=false\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    const ApplicationProfile* a = FindProfile(config, "Profile.A");
    ASSERT_NE(a, nullptr);
    EXPECT_FALSE(a->desktopOverlay.enabled);
    EXPECT_EQ(a->desktopOverlay.size, 200);
    EXPECT_EQ(a->desktopOverlay.pad, 0);
    EXPECT_EQ(a->desktopOverlay.pos, 3);
    EXPECT_EQ(a->desktopOverlay.mode, 1);  // base
    EXPECT_TRUE(a->desktopOverlay.alwaysRender);
    EXPECT_EQ(a->desktopOverlay.foregroundAcquireGraceMs, 10000);
    const ApplicationProfile* b = FindProfile(config, "Profile.B");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->desktopOverlay.size, 44);
    EXPECT_TRUE(b->desktopOverlay.enabled);  // bare "enabled" is not an overlay key
}

TEST_F(ConfigProfileDesktopOverlayTest, ProfileValuesDoNotChangeTheGlobalOverlayOrOtherProfiles) {
    WriteConfig(std::string(kBaseSection) +
                "[Profile.A]\nprocess=a.exe\nDesktopOverlay.size=77\n[Profile.B]\nprocess=b.exe\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config, "unrelated.exe");  // the controller's case: no profile matches
    EXPECT_EQ(config.pseudoOverlay.size, 40);
    EXPECT_EQ(FindProfile(config, "Profile.A")->desktopOverlay.size, 77);
    EXPECT_EQ(FindProfile(config, "Profile.B")->desktopOverlay.size, 40);

    // A load that runs AS a profile's process applies that profile's override to the global
    // settings too (the long-standing per-process override), so the other profiles resolve over
    // that base - exactly what the controller's old parser did with RuntimeConfiguration().
    AppConfig asProfileA;
    LoadConfig(tempConfigFile, asProfileA, "a.exe");
    EXPECT_EQ(asProfileA.pseudoOverlay.size, 77);
    EXPECT_EQ(FindProfile(asProfileA, "Profile.B")->desktopOverlay.size, 77);
}

TEST_F(ConfigProfileDesktopOverlayTest, Utf8ProfileNamesResolveLikeEveryOtherProfileKey) {
    // "Caf\xC3\xA9" is representable in the Western code pages; the loader (like the rest of
    // CaptureEngine) addresses sections through the active code page.
    bool lossy = true;
    text::WideToCodePage(L"Café", GetACP(), &lossy);
    if (lossy)
        GTEST_SKIP() << "the active code page cannot represent the test profile name";
    const std::string section = text::WideToCodePage(L"Profile.Café", GetACP());
    ExpectProfilesMatchReference(
        "[Profile.Caf\xC3\xA9]\nprocess=Caf\xC3\xA9.exe\nvideo_capture=wgc\n"
        "DesktopOverlay.size=55\nDesktopOverlay.mode=2\n",
        {section.c_str()});
    AppConfig config;
    LoadConfig(tempConfigFile, config);
    const ApplicationProfile* profile = FindProfile(config, section.c_str());
    ASSERT_NE(profile, nullptr);
    EXPECT_EQ(profile->desktopOverlay.size, 55);
    EXPECT_EQ(profile->desktopOverlay.mode, 2);
}

TEST_F(ConfigProfileDesktopOverlayTest, InvalidProfileValuesAreDiagnosedButLoadLikeTheOldParser) {
    const std::string source = std::string(kBaseSection) +
                               "[Profile.A]\nprocess=a.exe\nDesktopOverlay.size=500\nDesktopOverlay.mode=abc\n"
                               "DesktopOverlay.enabled=maybe\nDesktopOverlay.pad=5\n";
    const text::IniDocument document = text::ParseUtf8Ini(source);
    AppConfig config;
    ConfigDiagnostics diagnostics;
    LoadConfigFromDocument(document, config, "", &diagnostics);
    const ApplicationProfile* profile = FindProfile(config, "Profile.A");
    ASSERT_NE(profile, nullptr);
    EXPECT_EQ(profile->desktopOverlay.size, 200);
    EXPECT_EQ(profile->desktopOverlay.mode, 1);
    EXPECT_TRUE(profile->desktopOverlay.enabled);
    EXPECT_EQ(profile->desktopOverlay.pad, 5);

    auto warned = [&](const char* key) {
        return std::any_of(diagnostics.entries.begin(), diagnostics.entries.end(), [&](const ConfigDiagnostic& entry) {
            return entry.kind == ConfigDiagnosticKind::kInvalidValue && entry.section == "Profile.A" &&
                   entry.key == key;
        });
    };
    EXPECT_TRUE(warned("DesktopOverlay.size"));     // clamped
    EXPECT_TRUE(warned("DesktopOverlay.mode"));     // unparsable
    EXPECT_TRUE(warned("DesktopOverlay.enabled"));  // unparsable
    EXPECT_FALSE(warned("DesktopOverlay.pad"));
}
