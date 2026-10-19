// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The command line of the CaptureEngine app (controller mode), parsed exactly as WinMain/ControllerMain
// accept it today. Pure: no process state, no logging, nothing from common/ (frontend/ includes only
// include/cengine/** and system headers). Where today's matching is sloppy it is kept on purpose, and
// the golden tests pin each quirk:
//  * --launch, --license, --auto-record= and --restart-from-pid= are found with a plain substring
//    search anywhere in the line, the first occurrence wins, and a later occurrence is never consulted;
//  * --launch takes the REST of the line (the Steam `--launch %command%` form), so a flag that appears
//    behind it is found inside the game's own command line;
//  * only --list-monitors (and the bootstrap gate --license) and --config= are argv tokens.
namespace ce::frontend {

struct AutoRecordOptions {
    uint32_t delayMs = 0;     // READY -> start recording
    uint32_t durationMs = 0;  // start -> stop, both measured from READY
};

// --config= as common/platform ReadConfigurationArgument reads it (same argv rules).
enum class ConfigArgumentStatus { Missing, Supplied, Invalid };

struct FrontendOptions {
    // Exact argv tokens, case-insensitive. Today's precedence: listMonitors, then showLicense, then the
    // rest; the elevation bootstrap is skipped for either informational token (SkipsElevationBootstrap).
    bool listMonitors = false;
    bool licenseToken = false;
    // "--license" appears anywhere in the arguments (substring, case-sensitive): opens LICENSE.txt, exits.
    bool showLicense = false;
    // --auto-record=DELAY_MS,DURATION_MS (first occurrence only, value ends at the first space or tab).
    // No comma: silently ignored. A comma with a part that is not an unsigned decimal: ignored and a
    // warning is returned.
    bool autoRecordRequested = false;
    AutoRecordOptions autoRecord;
    // Everything after "--launch" and its run of '=' / ' ' delimiters, UTF-8, unmodified (trailing
    // spaces included). Empty = no launch (no flag, or nothing behind it).
    std::string launchCommand;
    // --restart-from-pid=<digits>: the ASCII digits right behind the first occurrence in the FULL command
    // line (program name included). 0 = none, unparsable, zero or out of range.
    uint32_t restartFromPid = 0;
    ConfigArgumentStatus config = ConfigArgumentStatus::Missing;
    std::wstring configPath;
    // Complete log lines (UTF-8, today's text) for the caller to emit once a log exists.
    std::vector<std::string> warnings;

    bool SkipsElevationBootstrap() const {
        return listMonitors || licenseToken;
    }
};

// `fullCommandLine` is GetCommandLineW(): program name included. Tokenized with CommandLineToArgvW (as
// HasExactCommandLineArgument and ReadProcessConfigurationArgument do today).
FrontendOptions ParseCommandLine(std::wstring_view fullCommandLine);

// The same with the argv tokens (argv[0] = program) already split and `arguments` the raw text behind
// the program name (WinMain's lpCmdLine); `fullCommandLine` serves --restart-from-pid=.
FrontendOptions ParseCommandLine(const std::vector<std::wstring>& argv, std::wstring_view arguments,
                                 std::wstring_view fullCommandLine);

// WinMain's lpCmdLine: the text behind the program name (quotes toggle, as in the CRT startup code),
// leading blanks skipped, the rest untouched.
std::wstring_view ArgumentsAfterProgramName(std::wstring_view fullCommandLine);

}  // namespace ce::frontend
