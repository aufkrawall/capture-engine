#include "launch_command.h"

namespace ce::runtime {

namespace {
constexpr const char* kCommandWhitespace = " \t\r\n";

std::string TrimCommandWhitespace(std::string_view value) {
    const size_t start = value.find_first_not_of(kCommandWhitespace);
    if (start == std::string_view::npos) {
        return "";
    }

    const size_t end = value.find_last_not_of(kCommandWhitespace);
    return std::string(value.substr(start, end - start + 1));
}
}  // namespace

bool ParseLaunchCommand(std::string_view command, LaunchCommand* outCommand) {
    if (!outCommand) {
        return false;
    }

    *outCommand = {};
    outCommand->rawCommandLine = TrimCommandWhitespace(command);
    if (outCommand->rawCommandLine.empty()) {
        return false;
    }

    const std::string& raw = outCommand->rawCommandLine;
    if (raw.front() == '"') {
        const size_t closingQuote = raw.find('"', 1);
        if (closingQuote == std::string::npos || closingQuote == 1) {
            return false;
        }
        outCommand->executablePath = raw.substr(1, closingQuote - 1);
    } else {
        const size_t separator = raw.find_first_of(kCommandWhitespace);
        outCommand->executablePath = raw.substr(0, separator);
    }

    if (outCommand->executablePath.empty()) {
        return false;
    }

    const size_t lastSlash = outCommand->executablePath.find_last_of("\\/");
    outCommand->fileName = (lastSlash != std::string::npos) ? outCommand->executablePath.substr(lastSlash + 1)
                                                            : outCommand->executablePath;
    if (lastSlash != std::string::npos) {
        outCommand->workingDirectory = outCommand->executablePath.substr(0, lastSlash);
    }

    return true;
}

}  // namespace ce::runtime
