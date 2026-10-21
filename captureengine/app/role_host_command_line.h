// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// How ce_role_host_main() tells a helper launch from a client's own start (libcengine 11, part 6). Header-only so
// the tests check the rule without linking the role hosts.

#include <cstring>
#include <string_view>

namespace ce::role_host {

// The arguments after the program name, as WinMain's lpCmdLine receives them: a quoted program name ends at its
// closing quote, an unquoted one at the first blank; the blanks behind it are skipped.
inline const char* CommandLineTail(const char* commandLine) {
    if (!commandLine)
        return "";
    const char* cursor = commandLine;
    if (*cursor == '"') {
        ++cursor;
        while (*cursor && *cursor != '"')
            ++cursor;
        if (*cursor == '"')
            ++cursor;
    } else {
        while (*cursor && *cursor != ' ' && *cursor != '\t')
            ++cursor;
    }
    while (*cursor == ' ' || *cursor == '\t')
        ++cursor;
    return cursor;
}

// Only a launch the runtime makes counts: its command names the helper in the first argument, exactly as every
// launch writes it. The crash-dump helper is not among them; only the hook DLL starts it, from the package.
inline bool IsClientHelperLaunch(const char* tail) {
    if (!tail)
        return false;
    const std::string_view text(tail);
    const std::string_view first = text.substr(0, text.find_first_of(" \t"));
    constexpr std::string_view kCommands[] = {"--mode=inject",   "--mode=media",      "--mode=logger",
                                              "--mode=sensors",  "--ce-setup",        "--process-loopback-worker",
                                              "--sensor-bridge", "--install-pawnio", "--uninstall-pawnio"};
    for (const std::string_view command : kCommands) {
        if (first == command)
            return true;
    }
    return false;
}

}  // namespace ce::role_host
