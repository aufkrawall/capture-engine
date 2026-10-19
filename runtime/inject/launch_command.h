#pragma once

#include <string>
#include <string_view>

namespace ce::runtime {

// A deferred launch request split into what CreateProcess needs. Text handling only: nothing here
// starts a process, so the parsing and conversion rules are tested without one. All text is UTF-8
// (the library's string encoding); the launcher converts it to UTF-16 at the Win32 boundary.
struct LaunchCommand {
    std::string rawCommandLine;
    std::string executablePath;
    std::string workingDirectory;
    std::string fileName;
};

// Splits "path args" or "\"path with spaces\" args" (as a Steam launch option or --launch passes
// it) into its parts. The working directory is the executable's folder. Returns false for an empty
// command, an unterminated or empty quoted path, and a command without an executable.
bool ParseLaunchCommand(std::string_view command, LaunchCommand* outCommand);

// Strict conversions at the Win32 boundary. A byte sequence that is not valid UTF-8 (and a UTF-16
// string with an unpaired surrogate) is refused rather than replaced: a path with U+FFFD in it
// names no file, and launching a different one than the caller asked for is worse than failing.
// The out string is cleared on failure.
bool Utf8ToWide(std::string_view utf8, std::wstring* outWide);
bool WideToUtf8(std::wstring_view wide, std::string* outUtf8);

}  // namespace ce::runtime
