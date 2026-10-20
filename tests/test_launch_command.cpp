#include <gtest/gtest.h>

#include <string>

#include "runtime/inject/launch_command.h"

namespace {

using ce::runtime::LaunchCommand;
using ce::runtime::ParseLaunchCommand;

LaunchCommand MustParse(const std::string& command) {
    LaunchCommand parsed;
    EXPECT_TRUE(ParseLaunchCommand(command, &parsed)) << command;
    return parsed;
}

TEST(LaunchCommandParse, UnquotedPathStopsAtFirstWhitespace) {
    const LaunchCommand parsed = MustParse("C:\\Games\\Foo\\foo_dx11.exe -windowed -fps 60");
    EXPECT_EQ(parsed.executablePath, "C:\\Games\\Foo\\foo_dx11.exe");
    EXPECT_EQ(parsed.workingDirectory, "C:\\Games\\Foo");
    EXPECT_EQ(parsed.fileName, "foo_dx11.exe");
    // The whole line, arguments included, is what the process receives as its command line.
    EXPECT_EQ(parsed.rawCommandLine, "C:\\Games\\Foo\\foo_dx11.exe -windowed -fps 60");
}

TEST(LaunchCommandParse, QuotedPathMayContainSpacesAndKeepsItsArguments) {
    const LaunchCommand parsed =
        MustParse("\"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Foo Game\\foo.exe\" -arg \"two words\"");
    EXPECT_EQ(parsed.executablePath, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Foo Game\\foo.exe");
    EXPECT_EQ(parsed.workingDirectory, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Foo Game");
    EXPECT_EQ(parsed.fileName, "foo.exe");
    EXPECT_EQ(parsed.rawCommandLine,
              "\"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Foo Game\\foo.exe\" -arg \"two words\"");
}

TEST(LaunchCommandParse, QuotedPathWithoutArguments) {
    const LaunchCommand parsed = MustParse("\"D:\\Spiele\\game.exe\"");
    EXPECT_EQ(parsed.executablePath, "D:\\Spiele\\game.exe");
    EXPECT_EQ(parsed.fileName, "game.exe");
}

TEST(LaunchCommandParse, SurroundingWhitespaceIsTrimmedFromTheCommandLine) {
    const LaunchCommand parsed = MustParse(" \t\r\n  \"C:\\g\\a.exe\" -x \r\n\t ");
    EXPECT_EQ(parsed.rawCommandLine, "\"C:\\g\\a.exe\" -x");
    EXPECT_EQ(parsed.executablePath, "C:\\g\\a.exe");
}

TEST(LaunchCommandParse, EveryAsciiWhitespaceEndsAnUnquotedPath) {
    for (const char separator : {' ', '\t', '\r', '\n'}) {
        const LaunchCommand parsed = MustParse(std::string("C:\\g\\a.exe") + separator + "-x");
        EXPECT_EQ(parsed.executablePath, "C:\\g\\a.exe") << static_cast<int>(separator);
    }
}

TEST(LaunchCommandParse, BareFileNameHasNoWorkingDirectory) {
    const LaunchCommand parsed = MustParse("game.exe /x");
    EXPECT_EQ(parsed.executablePath, "game.exe");
    EXPECT_EQ(parsed.fileName, "game.exe");
    EXPECT_TRUE(parsed.workingDirectory.empty());
}

TEST(LaunchCommandParse, ForwardSlashesAndUncPathsSplitLikeBackslashes) {
    const LaunchCommand forward = MustParse("C:/Games/a.exe");
    EXPECT_EQ(forward.fileName, "a.exe");
    EXPECT_EQ(forward.workingDirectory, "C:/Games");

    const LaunchCommand unc = MustParse("\\\\server\\share\\dir\\a.exe");
    EXPECT_EQ(unc.fileName, "a.exe");
    EXPECT_EQ(unc.workingDirectory, "\\\\server\\share\\dir");

    // A root-relative file has an empty folder part, which means "inherit", not "the root".
    const LaunchCommand rooted = MustParse("\\a.exe");
    EXPECT_EQ(rooted.fileName, "a.exe");
    EXPECT_TRUE(rooted.workingDirectory.empty());
}

TEST(LaunchCommandParse, RejectsCommandsWithoutAnExecutable) {
    LaunchCommand parsed;
    EXPECT_FALSE(ParseLaunchCommand("", &parsed));
    EXPECT_FALSE(ParseLaunchCommand(" \t\r\n ", &parsed));
    EXPECT_FALSE(ParseLaunchCommand("\"\"", &parsed));
    EXPECT_FALSE(ParseLaunchCommand("\"\" -x", &parsed));
    EXPECT_FALSE(ParseLaunchCommand("\"C:\\g\\a.exe", &parsed)) << "no closing quote";
    EXPECT_TRUE(parsed.executablePath.empty());
    EXPECT_FALSE(ParseLaunchCommand("\"", &parsed));
    EXPECT_FALSE(ParseLaunchCommand("\"C:\\g\\a.exe", nullptr));
}

TEST(LaunchCommandParse, QuotesInsideAnUnquotedPathAreNotSpecial) {
    // Only a leading quote opens a quoted path; the same rule the launcher has always applied.
    const LaunchCommand parsed = MustParse("C:\\g\\a\"b c.exe x");
    EXPECT_EQ(parsed.executablePath, "C:\\g\\a\"b");
}

// "D:\Spiele\Ü\游戏\ゲーム\game.exe" with a supplementary-plane character (U+1F3AE) in the folder name.
// Non-ASCII bytes are spelled out so the test does not depend on the source or execution charset.
const char kNonAsciiPathUtf8[] =
    "D:\\Spiele\\\xC3\x9C\\\xE6\xB8\xB8\xE6\x88\x8F\\\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0\\\xF0\x9F\x8E\xAE\\game.exe";
const wchar_t kNonAsciiPathWide[] = L"D:\\Spiele\\\u00DC\\\u6E38\u620F\\\u30B2\u30FC\u30E0\\\U0001F3AE\\game.exe";

TEST(LaunchCommandParse, NonAsciiPathsSplitOnTheirAsciiSeparatorsOnly) {
    // No byte of a UTF-8 multi-byte sequence is ASCII, so the parser can never split inside a
    // character. The old active-code-page text could: a Shift-JIS trail byte may be 0x5C ('\').
    const std::string command = std::string("\"") + kNonAsciiPathUtf8 + "\" -windowed";
    const LaunchCommand parsed = MustParse(command);
    EXPECT_EQ(parsed.executablePath, kNonAsciiPathUtf8);
    EXPECT_EQ(parsed.fileName, "game.exe");
    EXPECT_EQ(parsed.workingDirectory,
              "D:\\Spiele\\\xC3\x9C\\\xE6\xB8\xB8\xE6\x88\x8F\\\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0\\\xF0\x9F\x8E\xAE");

    const LaunchCommand unquoted = MustParse("D:\\Spiele\\\xC3\x9C\\\xE3\x82\xB2.exe /x");
    EXPECT_EQ(unquoted.fileName, "\xE3\x82\xB2.exe");
    EXPECT_EQ(unquoted.workingDirectory, "D:\\Spiele\\\xC3\x9C");
}

TEST(LaunchCommandUtf8, ConvertsNonAsciiPathsToTheExactWideText) {
    std::wstring wide;
    ASSERT_TRUE(ce::runtime::Utf8ToWide(kNonAsciiPathUtf8, &wide));
    EXPECT_EQ(wide, kNonAsciiPathWide);

    std::string utf8;
    ASSERT_TRUE(ce::runtime::WideToUtf8(kNonAsciiPathWide, &utf8));
    EXPECT_EQ(utf8, kNonAsciiPathUtf8);
}

TEST(LaunchCommandUtf8, AsciiAndEmptyTextRoundTrip) {
    std::wstring wide = L"stale";
    ASSERT_TRUE(ce::runtime::Utf8ToWide("", &wide));
    EXPECT_TRUE(wide.empty());
    ASSERT_TRUE(ce::runtime::Utf8ToWide("C:\\g\\a.exe -x \"y z\"", &wide));
    EXPECT_EQ(wide, L"C:\\g\\a.exe -x \"y z\"");

    std::string utf8 = "stale";
    ASSERT_TRUE(ce::runtime::WideToUtf8(L"", &utf8));
    EXPECT_TRUE(utf8.empty());
}

TEST(LaunchCommandUtf8, RefusesInvalidSequencesInsteadOfReplacingThem) {
    // A U+FFFD in a path names no file; the launcher must fail rather than start something else.
    const char* invalid[] = {
        "C:\\g\\\xC3\x28.exe",       // continuation byte missing
        "C:\\g\\\x80.exe",           // stray continuation byte
        "C:\\g\\\xE6\xB8.exe",       // truncated three-byte sequence
        "C:\\g\\\xC0\x80.exe",       // overlong NUL
        "C:\\g\\\xED\xA0\x80.exe",   // encoded surrogate
        "C:\\g\\\xF4\x90\x80\x80.exe",  // above U+10FFFF
    };
    for (const char* text : invalid) {
        std::wstring wide = L"stale";
        EXPECT_FALSE(ce::runtime::Utf8ToWide(text, &wide)) << text;
        EXPECT_TRUE(wide.empty()) << "a failed conversion must not leave partial text";
    }
    std::string utf8 = "stale";
    EXPECT_FALSE(ce::runtime::WideToUtf8(std::wstring(L"C:\\g\\\xD800.exe"), &utf8)) << "unpaired surrogate";
    EXPECT_TRUE(utf8.empty());
    EXPECT_FALSE(ce::runtime::Utf8ToWide("x", nullptr));
    EXPECT_FALSE(ce::runtime::WideToUtf8(L"x", nullptr));
}

TEST(LaunchCommandParse, ParsingAgainDiscardsThePreviousResult) {
    LaunchCommand parsed = MustParse("C:\\old\\old.exe -old");
    ASSERT_TRUE(ParseLaunchCommand("new.exe", &parsed));
    EXPECT_EQ(parsed.rawCommandLine, "new.exe");
    EXPECT_EQ(parsed.fileName, "new.exe");
    EXPECT_TRUE(parsed.workingDirectory.empty()) << "a stale folder would run the new exe from the old game's folder";
}

// Filename heuristic kept as is pending a product decision (llm-wiki/library/08 Q7); these rows pin it.
TEST(LaunchCommandGameHeuristic, EachMarkerMarksAGameWhetherAloneOrInsideALongerName) {
    const struct {
        const char* fileName;
        bool isGame;
    } rows[] = {
        {"_dx.exe", true},          {"foo_dx11.exe", true},     {"foo_dx12.exe", true},
        {"foo_vulkan.exe", true},   {"foo_vk.exe", true},       {"game.exe", true},
        {"MyGame.exe", true},       {"foo_test.exe", true},     {"footest.exe", true},
        {"test.exe", true},         {"_test.dll", true},
    };
    for (const auto& row : rows) {
        EXPECT_EQ(ce::runtime::LooksLikeGameExecutable(row.fileName), row.isGame) << row.fileName;
    }
}

TEST(LaunchCommandGameHeuristic, NearMissesOfEveryMarkerAreNotGames) {
    const struct {
        const char* fileName;
        bool isGame;
    } rows[] = {
        {"dx11.exe", false},        // "_dx" needs the underscore
        {"vulkan.exe", false},      // "_vulkan" needs the underscore
        {"vk.exe", false},          // "_vk" needs the underscore
        {"test_app.exe", false},    // "_test" and "test.exe" do not match "test_"
        {"test.dll", false},        // "test.exe" needs the .exe suffix
        {"foo.exe", false},
    };
    for (const auto& row : rows) {
        EXPECT_EQ(ce::runtime::LooksLikeGameExecutable(row.fileName), row.isGame) << row.fileName;
    }
}

TEST(LaunchCommandGameHeuristic, MatchingIsAsciiCaseInsensitive) {
    EXPECT_TRUE(ce::runtime::LooksLikeGameExecutable("GAME.EXE"));
    EXPECT_TRUE(ce::runtime::LooksLikeGameExecutable("Foo_DX12.exe"));
    EXPECT_TRUE(ce::runtime::LooksLikeGameExecutable("FOO_VULKAN.EXE"));
    EXPECT_TRUE(ce::runtime::LooksLikeGameExecutable("FOO_VK.Exe"));
    EXPECT_TRUE(ce::runtime::LooksLikeGameExecutable("FOO_TEST.EXE"));
    EXPECT_TRUE(ce::runtime::LooksLikeGameExecutable("FooTest.EXE"));
    // Bytes above 0x7F are not ASCII letters: they are kept as is and do not stop the ASCII markers.
    EXPECT_TRUE(ce::runtime::LooksLikeGameExecutable("\xC3\x9C_DX.exe"));
    EXPECT_FALSE(ce::runtime::LooksLikeGameExecutable("\xC3\x9Clauncher.exe"));
}

TEST(LaunchCommandGameHeuristic, LaunchersAndEmptyNamesAreNotGames) {
    EXPECT_FALSE(ce::runtime::LooksLikeGameExecutable("Launcher.exe"));
    EXPECT_FALSE(ce::runtime::LooksLikeGameExecutable("steam.exe"));
    EXPECT_FALSE(ce::runtime::LooksLikeGameExecutable(""));
}

}  // namespace
