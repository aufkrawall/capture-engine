#include "launch_command.h"

#include <windows.h>

#include <cctype>
#include <limits>

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

bool LooksLikeGameExecutable(std::string_view fileName) {
    std::string lowerName;
    lowerName.reserve(fileName.size());
    for (const char c : fileName) {
        lowerName += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return lowerName.find("_dx") != std::string::npos || lowerName.find("_vulkan") != std::string::npos ||
           lowerName.find("_vk") != std::string::npos || lowerName.find("game") != std::string::npos ||
           lowerName.find("_test") != std::string::npos || lowerName.find("test.exe") != std::string::npos;
}

bool Utf8ToWide(std::string_view utf8, std::wstring* outWide) {
    if (!outWide) {
        return false;
    }
    outWide->clear();
    if (utf8.empty()) {
        return true;
    }
    if (utf8.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    const int needed =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return false;
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                            needed) != needed) {
        return false;
    }
    *outWide = std::move(wide);
    return true;
}

bool WideToUtf8(std::wstring_view wide, std::string* outUtf8) {
    if (!outUtf8) {
        return false;
    }
    outUtf8->clear();
    if (wide.empty()) {
        return true;
    }
    if (wide.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return false;
    }
    std::string utf8(static_cast<size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                            needed, nullptr, nullptr) != needed) {
        return false;
    }
    *outUtf8 = std::move(utf8);
    return true;
}

}  // namespace ce::runtime
