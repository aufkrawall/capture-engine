#pragma once

#include <string>
#include <string_view>

namespace ce::runtime {

// A deferred launch request split into what CreateProcess needs. Pure text handling: nothing here
// touches the OS, so the parsing rules are tested without starting a process.
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

}  // namespace ce::runtime
