// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "frontend/app/command_line.h"

#include <windows.h>
#include <shellapi.h>

#include <charconv>
#include <cstring>
#include <system_error>

namespace ce::frontend {
namespace {

constexpr std::wstring_view kAutoRecordFlag = L"--auto-record=";
constexpr std::wstring_view kLaunchFlag = L"--launch";
constexpr std::wstring_view kLaunchEquals = L"--launch=";
constexpr std::wstring_view kLicenseFlag = L"--license";
constexpr std::wstring_view kListMonitorsFlag = L"--list-monitors";
constexpr std::wstring_view kRestartFlag = L"--restart-from-pid=";
constexpr std::wstring_view kConfigPrefix = L"--config=";

std::string Utf8FromWide(std::wstring_view text) {
    if (text.empty())
        return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

// An unsigned decimal exactly as ce::TryParseUInt32 takes it: digits only (no sign, blanks or prefix),
// at least one, no overflow. Anything outside ASCII fails like it does on the narrowed command line.
bool TryParseUnsigned(std::wstring_view text, uint32_t& value) {
    if (text.empty())
        return false;
    std::string narrow;
    narrow.reserve(text.size());
    for (const wchar_t character : text) {
        if (character > 0x7F)
            return false;
        narrow.push_back(static_cast<char>(character));
    }
    uint32_t parsed = 0;
    const auto result = std::from_chars(narrow.data(), narrow.data() + narrow.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != narrow.data() + narrow.size())
        return false;
    value = parsed;
    return true;
}

bool HasToken(const std::vector<std::wstring>& argv, std::wstring_view expected) {
    const std::wstring wanted(expected);
    for (size_t index = 1; index < argv.size(); ++index) {
        if (_wcsicmp(argv[index].c_str(), wanted.c_str()) == 0)
            return true;
    }
    return false;
}

// common/platform ReadConfigurationArgument, rule for rule: a --launch token ends the search (the
// game's own options follow), a bare --config or a repeated / empty / NUL-bearing --config= is invalid.
void ReadConfig(const std::vector<std::wstring>& argv, FrontendOptions& options) {
    for (size_t index = 1; index < argv.size(); ++index) {
        const std::wstring& argument = argv[index];
        if (argument == kLaunchFlag || std::wstring_view(argument).starts_with(kLaunchEquals))
            break;
        if (argument == L"--config") {
            options.config = ConfigArgumentStatus::Invalid;
            options.configPath.clear();
            return;
        }
        if (!std::wstring_view(argument).starts_with(kConfigPrefix))
            continue;
        if (options.config != ConfigArgumentStatus::Missing || argument.size() == kConfigPrefix.size() ||
            argument.find(L'\0') != std::wstring::npos) {
            options.config = ConfigArgumentStatus::Invalid;
            options.configPath.clear();
            return;
        }
        options.config = ConfigArgumentStatus::Supplied;
        options.configPath = argument.substr(kConfigPrefix.size());
    }
}

void ReadAutoRecord(std::wstring_view arguments, FrontendOptions& options) {
    const size_t position = arguments.find(kAutoRecordFlag);
    if (position == std::wstring_view::npos)
        return;
    std::wstring_view params = arguments.substr(position + kAutoRecordFlag.size());
    const size_t tokenEnd = params.find_first_of(L" \t");
    if (tokenEnd != std::wstring_view::npos)
        params = params.substr(0, tokenEnd);
    const size_t comma = params.find(L',');
    if (comma == std::wstring_view::npos)
        return;  // today: no comma means no feature and no warning
    AutoRecordOptions parsed;
    if (TryParseUnsigned(params.substr(0, comma), parsed.delayMs) &&
        TryParseUnsigned(params.substr(comma + 1), parsed.durationMs)) {
        options.autoRecordRequested = true;
        options.autoRecord = parsed;
    } else {
        options.warnings.push_back("[Controller] Ignoring malformed --auto-record value '" + Utf8FromWide(params) +
                                   "'");
    }
}

void ReadLaunch(std::wstring_view arguments, FrontendOptions& options) {
    const size_t position = arguments.find(kLaunchFlag);
    if (position == std::wstring_view::npos)
        return;
    size_t valueStart = position + kLaunchFlag.size();
    while (valueStart < arguments.size() && (arguments[valueStart] == L'=' || arguments[valueStart] == L' '))
        ++valueStart;
    if (valueStart < arguments.size())
        options.launchCommand = Utf8FromWide(arguments.substr(valueStart));
}

void ReadRestartPid(std::wstring_view fullCommandLine, FrontendOptions& options) {
    const size_t position = fullCommandLine.find(kRestartFlag);
    if (position == std::wstring_view::npos)
        return;
    const size_t begin = position + kRestartFlag.size();
    size_t end = begin;
    while (end < fullCommandLine.size() && fullCommandLine[end] >= L'0' && fullCommandLine[end] <= L'9')
        ++end;
    uint32_t pid = 0;
    if (TryParseUnsigned(fullCommandLine.substr(begin, end - begin), pid) && pid != 0)
        options.restartFromPid = pid;
}

}  // namespace

std::wstring_view ArgumentsAfterProgramName(std::wstring_view fullCommandLine) {
    size_t index = 0;
    bool quoted = false;
    while (index < fullCommandLine.size() && (fullCommandLine[index] > L' ' || quoted)) {
        if (fullCommandLine[index] == L'"')
            quoted = !quoted;
        ++index;
    }
    while (index < fullCommandLine.size() && fullCommandLine[index] <= L' ')
        ++index;
    return fullCommandLine.substr(index);
}

FrontendOptions ParseCommandLine(const std::vector<std::wstring>& argv, std::wstring_view arguments,
                                 std::wstring_view fullCommandLine) {
    FrontendOptions options;
    options.listMonitors = HasToken(argv, kListMonitorsFlag);
    options.licenseToken = HasToken(argv, kLicenseFlag);
    options.showLicense = arguments.find(kLicenseFlag) != std::wstring_view::npos;
    ReadAutoRecord(arguments, options);
    ReadLaunch(arguments, options);
    ReadRestartPid(fullCommandLine, options);
    ReadConfig(argv, options);
    return options;
}

FrontendOptions ParseCommandLine(std::wstring_view fullCommandLine) {
    std::vector<std::wstring> argv;
    if (!fullCommandLine.empty()) {
        const std::wstring terminated(fullCommandLine);
        int count = 0;
        wchar_t** parsed = CommandLineToArgvW(terminated.c_str(), &count);
        if (parsed) {
            argv.reserve(static_cast<size_t>(count));
            for (int index = 0; index < count; ++index)
                argv.emplace_back(parsed[index]);
            LocalFree(static_cast<HLOCAL>(parsed));
        }
    }
    return ParseCommandLine(argv, ArgumentsAfterProgramName(fullCommandLine), fullCommandLine);
}

}  // namespace ce::frontend
