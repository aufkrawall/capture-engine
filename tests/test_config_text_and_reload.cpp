#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../common/config.h"
#include "../common/config_reload_policy.h"
#include "../common/config_text_encoding.h"
#include "../common/path_utils.h"
#include "source_fragment_reader.h"

namespace text = ce::config_text;
namespace reload = ce::config_reload;

TEST(ConfigTextEncodingTest, Utf8DetectionIsStrict) {
    EXPECT_TRUE(text::IsValidUtf8("plain ascii"));
    EXPECT_TRUE(text::IsValidUtf8("D:\\Aufnahmen\\Spiele \xC3\x9C"));  // U+00DC
    EXPECT_TRUE(text::IsValidUtf8("\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0"));  // Japanese
    EXPECT_FALSE(text::IsValidUtf8("Spiele \xDC"));                      // ANSI 1252 U+00DC
    EXPECT_FALSE(text::IsValidUtf8("\xC0\xAF"));                          // overlong
    EXPECT_FALSE(text::IsValidUtf8("\xED\xA0\x80"));                      // surrogate
    EXPECT_FALSE(text::IsValidUtf8("\xE3\x82"));                          // truncated

    EXPECT_TRUE(text::IsUtf8ConfigText("\xEF\xBB\xBF[Capture]\n"));
    EXPECT_TRUE(text::IsUtf8ConfigText("[Output]\noutput_dir=C:\\Users\\J\xC3\xBCrgen\n"));
    EXPECT_FALSE(text::IsUtf8ConfigText("[Output]\noutput_dir=C:\\Users\\J\xFCrgen\n"));
    EXPECT_FALSE(text::IsUtf8ConfigText("[Output]\noutput_dir=C:\\Users\\Juergen\n"));
}

TEST(ConfigTextEncodingTest, Utf8ConvertsToTheCodePageAndReportsLoss) {
    bool lossy = true;
    EXPECT_EQ(text::Utf8ToCodePage("J\xC3\xBCrgen", 1252, &lossy), "J\xFCrgen");
    EXPECT_FALSE(lossy);
    const std::string japanese = text::Utf8ToCodePage("\xE3\x82\xB2", 1252, &lossy);
    EXPECT_EQ(japanese, "?");
    EXPECT_TRUE(lossy);
    EXPECT_EQ(text::Utf8ToCodePage("ascii", 1252, &lossy), "ascii");
    EXPECT_FALSE(lossy);
}

// A Notepad-saved (UTF-8) config with an umlaut in output_dir reached the rest
// of CaptureEngine as mojibake and the recordings went to a different folder.
TEST(ConfigTextEncodingTest, Utf8ConfigValuesLoadAsActiveCodePageText) {
    if (GetACP() != 1252) {
        GTEST_SKIP() << "expectation is written for Windows-1252; active code page is " << GetACP();
    }
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("ce_utf8_config_" + std::to_string(GetCurrentProcessId()) + ".ini");
    {
        std::ofstream file(path, std::ios::binary);
        file << "; UTF-8 comment \xE2\x80\x94 dash\r\n[Output]\r\noutput_dir=D:\\Aufnahmen\\Spiele \xC3\x9C\r\n";
    }
    const std::string raw = text::ReadIniValue(path.string(), "Output", "output_dir", "");
    EXPECT_EQ(raw, "D:\\Aufnahmen\\Spiele \xDC");

    AppConfig config;
    LoadConfig(path.string(), config);
    EXPECT_EQ(config.video.outputDir, "D:\\Aufnahmen\\Spiele \xDC");

    // The same value saved as ANSI is left as it is.
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "[Output]\r\noutput_dir=D:\\Aufnahmen\\Spiele \xDC\r\n";
    }
    EXPECT_EQ(text::ReadIniValue(path.string(), "Output", "output_dir", ""), "D:\\Aufnahmen\\Spiele \xDC");
    std::filesystem::remove(path);
}

TEST(ConfigReloadPolicyTest, AChangeIsAppliedOnlyOnceStable) {
    reload::State state;
    reload::FileIdentity original{true, 100, 5000};
    EXPECT_EQ(reload::Observe(state, original), reload::Decision::kNone);
    EXPECT_EQ(reload::Observe(state, original), reload::Decision::kNone);
    EXPECT_EQ(reload::CheckIntervalMs(state), reload::kIdleCheckIntervalMs);

    // Truncate-then-write: an empty file is never applied.
    EXPECT_EQ(reload::Observe(state, {true, 101, 0}), reload::Decision::kWait);
    // Partially written: seen once, waits.
    EXPECT_EQ(reload::Observe(state, {true, 102, 2048}), reload::Decision::kWait);
    EXPECT_EQ(reload::CheckIntervalMs(state), reload::kPendingCheckIntervalMs);
    // Still being written: identity moved again, still waits.
    EXPECT_EQ(reload::Observe(state, {true, 103, 5100}), reload::Decision::kWait);
    // Stable across two checks: reload once.
    EXPECT_EQ(reload::Observe(state, {true, 103, 5100}), reload::Decision::kReload);
    EXPECT_EQ(reload::Observe(state, {true, 103, 5100}), reload::Decision::kNone);
    EXPECT_EQ(reload::CheckIntervalMs(state), reload::kIdleCheckIntervalMs);
}

TEST(ConfigReloadPolicyTest, MissingFileIsNeverAppliedAndAnOlderTimestampStillCounts) {
    reload::State state;
    EXPECT_EQ(reload::Observe(state, {true, 500, 4000}), reload::Decision::kNone);
    // Editor's delete-then-rename: missing for a moment.
    EXPECT_EQ(reload::Observe(state, {false, 0, 0}), reload::Decision::kWait);
    // Restored from a backup with an OLDER timestamp: still a change.
    EXPECT_EQ(reload::Observe(state, {true, 400, 4000}), reload::Decision::kWait);
    EXPECT_EQ(reload::Observe(state, {true, 400, 4000}), reload::Decision::kReload);
    // Reverting to the applied identity cancels a pending change.
    EXPECT_EQ(reload::Observe(state, {true, 401, 4001}), reload::Decision::kWait);
    EXPECT_EQ(reload::Observe(state, {true, 400, 4000}), reload::Decision::kNone);
    EXPECT_FALSE(state.pendingValid);
}

TEST(ConfigReloadPolicyTest, ControllerReloadsThroughTheDebounce) {
    const std::string source =
        ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "captureengine/main_entry.cpp");
    ASSERT_FALSE(source.empty());
    const size_t observe = source.find("ce::config_reload::Observe(g_ConfigReloadState, identity)");
    const size_t load = source.find("LoadConfig(main_g_ConfigPath, main_g_Config);", observe);
    ASSERT_NE(observe, std::string::npos);
    ASSERT_NE(load, std::string::npos);
    EXPECT_NE(source.find("if (reloadDecision == ce::config_reload::Decision::kReload)", observe), std::string::npos);
}

// An installation below a folder the code page cannot express (Cyrillic on a
// Western system) reached the ANSI config/log APIs '?'-mangled: config.ini was
// never read and the game ran on defaults.
TEST(AnsiCompatiblePathTest, RepresentablePathsAreExactAndOthersUseTheShortName) {
    bool exact = false;
    EXPECT_EQ(ce::path::AnsiCompatiblePath(L"C:\\Program Files\\CaptureEngine", &exact),
              "C:\\Program Files\\CaptureEngine");
    EXPECT_TRUE(exact);

    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       (L"ce_path_\u041F\u0440\u043E\u0432\u0435\u0440\u043A\u0430_" +
                                        std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    const std::string ansi = ce::path::AnsiCompatiblePath(root.wstring(), &exact);
    if (GetACP() == 1251 || GetACP() == CP_UTF8) {
        EXPECT_TRUE(exact);  // representable there
    } else if (exact) {
        // The 8.3 name is ASCII and names the very same folder.
        EXPECT_FALSE(text::ContainsNonAscii(ansi));
        EXPECT_NE(GetFileAttributesA(ansi.c_str()), INVALID_FILE_ATTRIBUTES);
    } else {
        // No 8.3 name on this volume: reported, never silently wrong.
        EXPECT_EQ(GetFileAttributesA(ansi.c_str()), INVALID_FILE_ATTRIBUTES);
    }
    std::filesystem::remove_all(root);
}
