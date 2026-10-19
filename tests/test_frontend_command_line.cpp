// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "frontend/app/command_line.h"

#include "common/platform/runtime_package_paths.h"
#include "common/platform/strict_integer_parse.h"

#include <gtest/gtest.h>
#include <windows.h>
#include <shellapi.h>

#include <cstring>
#include <string>
#include <vector>

// Golden tests of the frontend's command-line parser (llm-wiki/library/05: "keep accepted syntax
// byte-compatible"). The oracle below is today's WinMain/ControllerMain parsing transcribed verbatim
// (captureengine/app/main_entry.cpp); every line is run through it and through the new parser, and the
// quirks that are sloppy on purpose are additionally pinned with explicit expectations.
namespace {

using ce::frontend::ConfigArgumentStatus;
using ce::frontend::FrontendOptions;
using ce::frontend::ParseCommandLine;

struct Legacy {
    bool license = false;
    bool autoEnabled = false;
    uint32_t delayMs = 0;
    uint32_t durationMs = 0;
    std::string warning;
    std::string launch;
    uint32_t restartPid = 0;
    bool listMonitors = false;
    bool licenseToken = false;
};

std::wstring Widen(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

std::string Narrow(const std::wstring& ascii) {
    return std::string(ascii.begin(), ascii.end());
}

bool LegacyHasExactArgument(const std::wstring& fullWide, const wchar_t* expected) {
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(fullWide.c_str(), &count);
    if (!arguments)
        return false;
    bool found = false;
    for (int index = 1; index < count; ++index) {
        if (_wcsicmp(arguments[index], expected) == 0) {
            found = true;
            break;
        }
    }
    LocalFree(reinterpret_cast<HLOCAL>(arguments));
    return found;
}

// main_entry.cpp WinMain, verbatim apart from the globals being locals. `cmdLine` is lpCmdLine,
// `fullCommandLine` is GetCommandLineA().
Legacy LegacyParse(const std::string& cmdLine, const std::string& fullCommandLine) {
    Legacy result;
    result.license = cmdLine.find("--license") != std::string::npos;
    size_t autoRecordPos = cmdLine.find("--auto-record=");
    if (autoRecordPos != std::string::npos) {
        std::string params = cmdLine.substr(autoRecordPos + 14);
        const size_t tokenEnd = params.find_first_of(" \t");
        if (tokenEnd != std::string::npos)
            params.resize(tokenEnd);
        size_t commaPos = params.find(',');
        if (commaPos != std::string::npos) {
            uint32_t delayMs = 0;
            uint32_t durationMs = 0;
            if (ce::TryParseUInt32(std::string_view(params).substr(0, commaPos), delayMs) &&
                ce::TryParseUInt32(std::string_view(params).substr(commaPos + 1), durationMs)) {
                result.delayMs = delayMs;
                result.durationMs = durationMs;
                result.autoEnabled = true;
            } else {
                result.warning = "[Controller] Ignoring malformed --auto-record value '" + params + "'";
            }
        }
    }

    std::string searchFlag = "--launch";
    size_t launchPos = cmdLine.find(searchFlag);
    if (launchPos != std::string::npos) {
        size_t valueStart = launchPos + searchFlag.length();
        while (valueStart < cmdLine.length() && (cmdLine[valueStart] == '=' || cmdLine[valueStart] == ' ')) {
            valueStart++;
        }
        if (valueStart < cmdLine.length())
            result.launch = cmdLine.substr(valueStart);
    }

    const char* restartArg = strstr(fullCommandLine.c_str(), "--restart-from-pid=");
    if (restartArg) {
        uint32_t priorPid = 0;
        const char* val = restartArg + 19;
        const char* end = val;
        while (*end >= '0' && *end <= '9')
            ++end;
        if (ce::TryParseUInt32(std::string_view(val, static_cast<size_t>(end - val)), priorPid) && priorPid != 0)
            result.restartPid = priorPid;
    }
    return result;
}

Legacy LegacyParseFull(const std::string& program, const std::string& arguments) {
    const std::string full = program + (arguments.empty() ? "" : " " + arguments);
    Legacy result = LegacyParse(arguments, full);
    const std::wstring wide = Widen(full);
    result.listMonitors = LegacyHasExactArgument(wide, L"--list-monitors");
    result.licenseToken = LegacyHasExactArgument(wide, L"--license");
    return result;
}

void ExpectSame(const std::string& program, const std::string& arguments) {
    SCOPED_TRACE(program + " | " + arguments);
    const Legacy legacy = LegacyParseFull(program, arguments);
    const std::string full = program + (arguments.empty() ? "" : " " + arguments);
    const FrontendOptions options = ParseCommandLine(Widen(full));
    EXPECT_EQ(options.showLicense, legacy.license);
    EXPECT_EQ(options.listMonitors, legacy.listMonitors);
    EXPECT_EQ(options.licenseToken, legacy.licenseToken);
    EXPECT_EQ(options.autoRecordRequested, legacy.autoEnabled);
    if (legacy.autoEnabled) {
        EXPECT_EQ(options.autoRecord.delayMs, legacy.delayMs);
        EXPECT_EQ(options.autoRecord.durationMs, legacy.durationMs);
    }
    EXPECT_EQ(options.launchCommand, legacy.launch);
    EXPECT_EQ(options.restartFromPid, legacy.restartPid);
    if (legacy.warning.empty())
        EXPECT_TRUE(options.warnings.empty());
    else
        EXPECT_EQ(options.warnings, std::vector<std::string>{legacy.warning});
}

const char* const kPrograms[] = {"CaptureEngine.exe", "\"C:\\Program Files\\CaptureEngine\\captureengine.exe\""};

TEST(FrontendCommandLineTest, EveryLineParsesLikeTodaysWinMain) {
    const char* const lines[] = {
        "",
        "--list-monitors",
        "--LIST-Monitors",
        "\"--list-monitors\"",
        "--list-monitors=1",
        "x --list-monitors y",
        "--launch game.exe --list-monitors",
        "--license",
        "--LICENSE",
        "--license-text",
        "x--license",
        "--launch game.exe --license",
        "--auto-record=1000,2000",
        "--auto-record=1000,2000 --launch game.exe",
        "--auto-record=1000",
        "--auto-record=1000,",
        "--auto-record=,2000",
        "--auto-record=,",
        "--auto-record=",
        "--auto-record=1000, 2000",
        "--auto-record=1000,2000,3000",
        "--auto-record=abc,def",
        "--auto-record=-1,5",
        "--auto-record=+1,5",
        "--auto-record=0x10,5",
        "--auto-record=4294967295,4294967295",
        "--auto-record=4294967296,1",
        "--auto-record=0,0",
        "--auto-record=000000000000000000000001,2",
        "--auto-record=1,2\t--launch x",
        "--auto-record=bad,1 --auto-record=1,2",
        "--auto-record=1,2 --auto-record=bad,1",
        "--launch game.exe --auto-record=1,2",
        "--launch",
        "--launch=",
        "--launch  =  = ",
        "--launch game.exe",
        "--launch=game.exe",
        "--launch= game.exe -windowed",
        "--launch \"C:\\Games\\Some Game\\game.exe\" -windowed -w 1920",
        "--launch game.exe  ",
        "--launch\tgame.exe",
        "--launcher x",
        "--no--launch x",
        "-launch x",
        "--launch=--launch",
        "--launch --launch x",
        "--restart-from-pid=1234",
        "--restart-from-pid=0",
        "--restart-from-pid=",
        "--restart-from-pid=12abc",
        "--restart-from-pid=abc",
        "--restart-from-pid=4294967295",
        "--restart-from-pid=4294967296",
        "--restart-from-pid=99999999999999999999",
        "--restart-from-pid=0000000000000000000000042",
        "--restart-from-pid=abc --restart-from-pid=5",
        "--restart-from-pid=5 --restart-from-pid=9",
        "--launch game.exe --restart-from-pid=77",
        "--auto-record=3000,10000 --restart-from-pid=88 --launch \"D:\\g\\game.exe\" -novid",
    };
    for (const char* program : kPrograms) {
        for (const char* line : lines)
            ExpectSame(program, line);
    }
}

// ---- pinned quirks (explicit values, so a flaw in the oracle cannot hide) --------------------------------

TEST(FrontendCommandLineTest, DefaultsOfAnEmptyLine) {
    const FrontendOptions options = ParseCommandLine(L"CaptureEngine.exe");
    EXPECT_FALSE(options.listMonitors);
    EXPECT_FALSE(options.licenseToken);
    EXPECT_FALSE(options.showLicense);
    EXPECT_FALSE(options.autoRecordRequested);
    EXPECT_TRUE(options.launchCommand.empty());
    EXPECT_EQ(options.restartFromPid, 0u);
    EXPECT_EQ(options.config, ConfigArgumentStatus::Missing);
    EXPECT_TRUE(options.warnings.empty());
    EXPECT_FALSE(options.SkipsElevationBootstrap());
    EXPECT_FALSE(ParseCommandLine(L"").listMonitors) << "an empty string is no command line at all";
}

TEST(FrontendCommandLineTest, SteamCommandFormTakesTheRestOfTheLineUntouched) {
    const FrontendOptions options = ParseCommandLine(
        LR"("C:\CE\captureengine.exe" --launch "D:\Steam\steamapps\common\Game\game.exe" -windowed -w 1920 --license)");
    EXPECT_EQ(options.launchCommand, R"("D:\Steam\steamapps\common\Game\game.exe" -windowed -w 1920 --license)");
    EXPECT_TRUE(options.showLicense) << "the substring search also scans the game's own options";
}

TEST(FrontendCommandLineTest, LaunchDelimiterRunAndTrailingText) {
    EXPECT_EQ(ParseCommandLine(L"ce.exe --launch=game.exe").launchCommand, "game.exe");
    EXPECT_EQ(ParseCommandLine(L"ce.exe --launch game.exe").launchCommand, "game.exe");
    EXPECT_EQ(ParseCommandLine(L"ce.exe --launch = = game.exe").launchCommand, "game.exe");
    EXPECT_EQ(ParseCommandLine(L"ce.exe --launch game.exe   ").launchCommand, "game.exe   ")
        << "trailing blanks belong to the command";
    EXPECT_EQ(ParseCommandLine(L"ce.exe --launch\tgame.exe").launchCommand, "\tgame.exe") << "a tab is no delimiter";
    EXPECT_TRUE(ParseCommandLine(L"ce.exe --launch").launchCommand.empty());
    EXPECT_TRUE(ParseCommandLine(L"ce.exe --launch=").launchCommand.empty());
    EXPECT_TRUE(ParseCommandLine(L"ce.exe --launch   ").launchCommand.empty());
}

TEST(FrontendCommandLineTest, LaunchMatchesAsASubstring) {
    EXPECT_EQ(ParseCommandLine(L"ce.exe --launcher x").launchCommand, "er x")
        << "--launcher is read as --launch followed by 'er x'";
    EXPECT_EQ(ParseCommandLine(L"ce.exe --no--launch x").launchCommand, "x");
    EXPECT_EQ(ParseCommandLine(L"ce.exe --launch --launch x").launchCommand, "--launch x");
}

TEST(FrontendCommandLineTest, LaunchCommandIsUtf8FromTheWideCommandLine) {
    const FrontendOptions options = ParseCommandLine(L"ce.exe --launch C:\\Spiele\\\u00e4\u00f6\u00fc\\g.exe \u20ac");
    EXPECT_EQ(options.launchCommand, "C:\\Spiele\\\xC3\xA4\xC3\xB6\xC3\xBC\\g.exe \xE2\x82\xAC");
}

TEST(FrontendCommandLineTest, AutoRecordAcceptsOnlyTwoUnsignedDecimals) {
    FrontendOptions options = ParseCommandLine(L"ce.exe --auto-record=3000,10000");
    ASSERT_TRUE(options.autoRecordRequested);
    EXPECT_EQ(options.autoRecord.delayMs, 3000u);
    EXPECT_EQ(options.autoRecord.durationMs, 10000u);
    EXPECT_TRUE(options.warnings.empty());

    options = ParseCommandLine(L"ce.exe --auto-record=4294967295,0");
    ASSERT_TRUE(options.autoRecordRequested);
    EXPECT_EQ(options.autoRecord.delayMs, 4294967295u);
    EXPECT_EQ(options.autoRecord.durationMs, 0u);

    for (const wchar_t* malformed :
         {L"ce.exe --auto-record=1,", L"ce.exe --auto-record=,1", L"ce.exe --auto-record=a,1",
          L"ce.exe --auto-record=-1,1", L"ce.exe --auto-record=+1,1", L"ce.exe --auto-record=1.5,1",
          L"ce.exe --auto-record=1,2,3", L"ce.exe --auto-record=4294967296,1", L"ce.exe --auto-record=1, 2"}) {
        SCOPED_TRACE(Narrow(malformed));
        options = ParseCommandLine(malformed);
        EXPECT_FALSE(options.autoRecordRequested);
        ASSERT_EQ(options.warnings.size(), 1u);
        EXPECT_EQ(options.warnings[0].rfind("[Controller] Ignoring malformed --auto-record value '", 0), 0u);
    }
}

TEST(FrontendCommandLineTest, AutoRecordWithoutACommaIsIgnoredSilently) {
    for (const wchar_t* line : {L"ce.exe --auto-record=", L"ce.exe --auto-record=1000", L"ce.exe --auto-record=x"}) {
        const FrontendOptions options = ParseCommandLine(line);
        EXPECT_FALSE(options.autoRecordRequested);
        EXPECT_TRUE(options.warnings.empty()) << "today logs nothing without a comma";
    }
}

TEST(FrontendCommandLineTest, AutoRecordWarningQuotesTheValueUpToTheFirstBlank) {
    const FrontendOptions options = ParseCommandLine(L"ce.exe --auto-record=12,x7\t--launch game.exe");
    ASSERT_EQ(options.warnings.size(), 1u);
    EXPECT_EQ(options.warnings[0], "[Controller] Ignoring malformed --auto-record value '12,x7'");
    EXPECT_EQ(options.launchCommand, "game.exe");
}

TEST(FrontendCommandLineTest, OnlyTheFirstAutoRecordOccurrenceCounts) {
    FrontendOptions options = ParseCommandLine(L"ce.exe --auto-record=bad,1 --auto-record=1,2");
    EXPECT_FALSE(options.autoRecordRequested) << "a later well-formed occurrence is never consulted";
    EXPECT_EQ(options.warnings.size(), 1u);
    options = ParseCommandLine(L"ce.exe --auto-record=1,2 --auto-record=bad,1");
    EXPECT_TRUE(options.autoRecordRequested);
    EXPECT_TRUE(options.warnings.empty());
}

TEST(FrontendCommandLineTest, AutoRecordBehindLaunchIsFoundInsideTheGamesCommandLine) {
    const FrontendOptions options = ParseCommandLine(L"ce.exe --launch game.exe --auto-record=500,900");
    ASSERT_TRUE(options.autoRecordRequested);
    EXPECT_EQ(options.autoRecord.delayMs, 500u);
    EXPECT_EQ(options.launchCommand, "game.exe --auto-record=500,900");
}

TEST(FrontendCommandLineTest, LicenseIsASubstringButTheBootstrapGateAnExactToken) {
    FrontendOptions options = ParseCommandLine(L"ce.exe --license-please");
    EXPECT_TRUE(options.showLicense);
    EXPECT_FALSE(options.licenseToken);
    EXPECT_FALSE(options.SkipsElevationBootstrap());

    options = ParseCommandLine(L"ce.exe --LICENSE");
    EXPECT_FALSE(options.showLicense) << "the substring match is case-sensitive";
    EXPECT_TRUE(options.licenseToken) << "the argv comparison is not";
    EXPECT_TRUE(options.SkipsElevationBootstrap());

    options = ParseCommandLine(L"ce.exe --license");
    EXPECT_TRUE(options.showLicense);
    EXPECT_TRUE(options.licenseToken);
}

TEST(FrontendCommandLineTest, ListMonitorsIsAnExactArgvTokenAnywhereAfterTheProgram) {
    EXPECT_TRUE(ParseCommandLine(L"ce.exe --list-monitors").listMonitors);
    EXPECT_TRUE(ParseCommandLine(L"ce.exe --LIST-MONITORS").listMonitors);
    EXPECT_TRUE(ParseCommandLine(L"ce.exe \"--list-monitors\"").listMonitors) << "quotes are not part of the token";
    EXPECT_TRUE(ParseCommandLine(L"ce.exe --launch g.exe --list-monitors").listMonitors)
        << "an argv scan, unlike the launch tail";
    EXPECT_FALSE(ParseCommandLine(L"ce.exe --list-monitors=1").listMonitors);
    EXPECT_FALSE(ParseCommandLine(L"ce.exe --list-monitorsx").listMonitors);
    EXPECT_FALSE(ParseCommandLine(L"ce.exe --launch=\"g.exe --list-monitors\"").listMonitors);
    EXPECT_FALSE(ParseCommandLine(L"--list-monitors").listMonitors) << "argv[0] is the program, never a flag";
    EXPECT_TRUE(ParseCommandLine(L"ce.exe --list-monitors").SkipsElevationBootstrap());
}

TEST(FrontendCommandLineTest, RestartPidReadsTheDigitsRightBehindTheFirstOccurrence) {
    EXPECT_EQ(ParseCommandLine(L"ce.exe --restart-from-pid=1234").restartFromPid, 1234u);
    EXPECT_EQ(ParseCommandLine(L"ce.exe --restart-from-pid=12abc").restartFromPid, 12u);
    EXPECT_EQ(ParseCommandLine(L"ce.exe --restart-from-pid=0000000042 x").restartFromPid, 42u);
    EXPECT_EQ(ParseCommandLine(L"ce.exe --restart-from-pid=4294967295").restartFromPid, 4294967295u);
    for (const wchar_t* line :
         {L"ce.exe --restart-from-pid=", L"ce.exe --restart-from-pid=0", L"ce.exe --restart-from-pid=abc",
          L"ce.exe --restart-from-pid=4294967296", L"ce.exe --restart-from-pid=-5", L"ce.exe --restart-from-pid= 5"}) {
        EXPECT_EQ(ParseCommandLine(line).restartFromPid, 0u);
    }
    EXPECT_EQ(ParseCommandLine(L"ce.exe --restart-from-pid=abc --restart-from-pid=5").restartFromPid, 0u)
        << "only the first occurrence is consulted";
}

TEST(FrontendCommandLineTest, RestartPidIsSearchedInTheFullLineIncludingTheProgramName) {
    const FrontendOptions options = ParseCommandLine(LR"("C:\x\--restart-from-pid=77\ce.exe" --list-monitors)");
    EXPECT_EQ(options.restartFromPid, 77u) << "GetCommandLineA() includes the program name";
    EXPECT_TRUE(options.listMonitors);
}

TEST(FrontendCommandLineTest, ArgumentsFollowTheProgramNameLikeWinMainsCommandLine) {
    using ce::frontend::ArgumentsAfterProgramName;
    EXPECT_EQ(ArgumentsAfterProgramName(L""), L"");
    EXPECT_EQ(ArgumentsAfterProgramName(L"ce.exe"), L"");
    EXPECT_EQ(ArgumentsAfterProgramName(L"ce.exe --a"), L"--a");
    EXPECT_EQ(ArgumentsAfterProgramName(L"ce.exe  \t --a  b "), L"--a  b ") << "leading blanks go, the rest stays";
    EXPECT_EQ(ArgumentsAfterProgramName(LR"("C:\Program Files\ce.exe" --a)"), L"--a");
    EXPECT_EQ(ArgumentsAfterProgramName(LR"("C:\Program Files\ce.exe")"), L"");
    EXPECT_EQ(ArgumentsAfterProgramName(LR"("C:\x y\ce.exe --a)"), L"") << "an unterminated quote swallows the line";
    EXPECT_EQ(ArgumentsAfterProgramName(LR"(C:\ce.exe"x y" --a)"), L"--a") << "quotes toggle anywhere in argv[0]";
}

// ---- --config= parity with the role host's reader --------------------------------------------------------

TEST(FrontendCommandLineTest, ConfigArgumentFollowsReadConfigurationArgumentRuleForRule) {
    using Argv = std::vector<std::wstring>;
    const std::wstring withNul = std::wstring(L"--config=a") + L'\0' + L"b";
    const Argv cases[] = {
        {L"ce.exe"},
        {L"ce.exe", L"--config=a.ini"},
        {L"ce.exe", L"x", L"--config=C:\\dir with space\\a.ini", L"y"},
        {L"ce.exe", L"--config=a.ini", L"--config=b.ini"},
        {L"ce.exe", L"--config=a.ini", L"--config"},
        {L"ce.exe", L"--config"},
        {L"ce.exe", L"--config="},
        {L"ce.exe", L"--CONFIG=a.ini"},
        {L"ce.exe", L"--launch", L"g.exe", L"--config=late.ini"},
        {L"ce.exe", L"--launch=g.exe", L"--config=late.ini"},
        {L"ce.exe", L"--config=a.ini", L"--launch=g.exe", L"--config=b.ini", L"--config="},
        {L"ce.exe", L"--launch=game --config=wrong.ini"},
        {L"ce.exe", L"--launcher", L"--config=a.ini"},
        {L"ce.exe", withNul},
    };
    for (const Argv& argv : cases) {
        const auto expected = ce::runtime::ReadConfigurationArgument(argv);
        const FrontendOptions options = ParseCommandLine(argv, L"", L"");
        SCOPED_TRACE(argv.size() > 1 ? std::string(argv[1].begin(), argv[1].end()) : std::string("(no arguments)"));
        switch (expected.status) {
            case ce::runtime::ConfigurationArgumentStatus::Missing:
                EXPECT_EQ(options.config, ConfigArgumentStatus::Missing);
                break;
            case ce::runtime::ConfigurationArgumentStatus::Supplied:
                EXPECT_EQ(options.config, ConfigArgumentStatus::Supplied);
                EXPECT_EQ(options.configPath, expected.path);
                break;
            case ce::runtime::ConfigurationArgumentStatus::Invalid:
                EXPECT_EQ(options.config, ConfigArgumentStatus::Invalid);
                break;
        }
    }
}

TEST(FrontendCommandLineTest, ConfigPathSurvivesTheCommandLineTokenizer) {
    const FrontendOptions options =
        ParseCommandLine(LR"("C:\CE\ce.exe" "--config=D:\My Settings\ce config.ini" --auto-record=1,2)");
    EXPECT_EQ(options.config, ConfigArgumentStatus::Supplied);
    EXPECT_EQ(options.configPath, LR"(D:\My Settings\ce config.ini)");
    EXPECT_TRUE(options.autoRecordRequested);
}

}  // namespace
