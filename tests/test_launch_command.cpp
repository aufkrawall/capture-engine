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

TEST(LaunchCommandParse, ParsingAgainDiscardsThePreviousResult) {
    LaunchCommand parsed = MustParse("C:\\old\\old.exe -old");
    ASSERT_TRUE(ParseLaunchCommand("new.exe", &parsed));
    EXPECT_EQ(parsed.rawCommandLine, "new.exe");
    EXPECT_EQ(parsed.fileName, "new.exe");
    EXPECT_TRUE(parsed.workingDirectory.empty()) << "a stale folder would run the new exe from the old game's folder";
}

}  // namespace
