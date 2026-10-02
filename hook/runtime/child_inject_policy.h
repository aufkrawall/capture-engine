#pragma once

// Hook-module path and child-injection command-line policy.
//
// The hook DLL used to derive its own directory with GetModuleFileNameA, which
// turns every install-path character outside the system code page into '?'. The
// derivation is UTF-16 here so one helper serves every consumer (config.ini,
// the d3d12 wrapper DLL, the crash-dump folder, the remote LoadLibraryW path,
// the pristine-image read), with each legacy narrow API getting its explicit
// conversion at the boundary instead of inheriting a lossy derivation.
//
// The command-line half answers "which executable does this CreateProcess call
// actually launch", the question the child-injection whitelist has to answer
// before the process exists.

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace ce::child_inject_policy {

// --- UTF-16 hook module paths ------------------------------------------------

// `path` minus its final segment ("C:\a\b\c.dll" -> "C:\a\b"), with no trailing
// separator. False when the path contains no separator at all.
inline bool StripLastPathSegmentW(wchar_t* path) {
    if (!path || path[0] == L'\0')
        return false;
    size_t length = 0;
    while (path[length] != L'\0')
        ++length;
    while (length > 0 && (path[length - 1] == L'\\' || path[length - 1] == L'/'))
        --length;
    while (length > 0 && path[length - 1] != L'\\' && path[length - 1] != L'/')
        --length;
    if (length == 0)
        return false;
    path[length - 1] = L'\0';
    return true;
}

// `module`'s image path in UTF-16. False on failure or truncation; `out` stays
// NUL-terminated whenever count > 0.
inline bool GetHookModulePathW(HMODULE module, wchar_t* out, size_t count) {
    if (!out || count == 0)
        return false;
    out[0] = L'\0';
    const DWORD length = GetModuleFileNameW(module, out, static_cast<DWORD>(count));
    if (length == 0 || length >= count) {
        out[0] = L'\0';
        return false;
    }
    return true;
}

// Directory containing `module`'s image in UTF-16, without a trailing separator.
inline bool GetHookModuleDirectoryW(HMODULE module, wchar_t* out, size_t count) {
    return GetHookModulePathW(module, out, count) && StripLastPathSegmentW(out);
}

// UTF-16 -> narrow conversions for the legacy boundaries CE does not own the
// file opening of. CP_ACP for ANSI-only readers (the CreateFileA in
// common/config's INI reader, fopen-based consumers); CP_UTF8 for the crash
// handler, whose WER registration converts its directory from UTF-8.
inline std::string NarrowFromWideAcp(const wchar_t* text) {
    if (!text || text[0] == L'\0')
        return {};
    const int needed = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
        return {};
    std::string result(static_cast<size_t>(needed), '\0');
    const int written = WideCharToMultiByte(CP_ACP, 0, text, -1, result.data(), needed, nullptr, nullptr);
    if (written <= 0)
        return {};
    result.resize(static_cast<size_t>(written - 1));
    return result;
}

inline std::string NarrowFromWideUtf8(const wchar_t* text) {
    if (!text || text[0] == L'\0')
        return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
        return {};
    std::string result(static_cast<size_t>(needed), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), needed, nullptr, nullptr);
    if (written <= 0)
        return {};
    result.resize(static_cast<size_t>(written - 1));
    return result;
}

// --- CreateProcess program resolution ----------------------------------------

// First token of a CreateProcess command line: the text between the first pair
// of double quotes, or - unquoted - everything up to the first whitespace.
inline std::string_view FirstCommandLineToken(std::string_view commandLine) {
    size_t begin = 0;
    while (begin < commandLine.size() && (commandLine[begin] == ' ' || commandLine[begin] == '\t'))
        ++begin;
    if (begin >= commandLine.size())
        return {};
    if (commandLine[begin] == '"') {
        const size_t close = commandLine.find('"', begin + 1);
        const size_t length = (close == std::string_view::npos) ? std::string_view::npos : close - begin - 1;
        return commandLine.substr(begin + 1, length);
    }
    size_t end = begin;
    while (end < commandLine.size() && commandLine[end] != ' ' && commandLine[end] != '\t')
        ++end;
    return commandLine.substr(begin, end - begin);
}

// `path` minus everything up to and including its last separator.
inline std::string_view FileNameOfPath(std::string_view path) {
    const size_t lastSlash = path.find_last_of("\\/");
    return (lastSlash == std::string_view::npos) ? path : path.substr(lastSlash + 1);
}

// The program path (no arguments) a CreateProcess(A/W) call launches.
// `applicationName` is lpApplicationName and is used verbatim: it is a path,
// not a command line, and its spaces are legal and unquoted. Only when it is
// absent is the command line's first token the program.
inline std::string_view ProgramPath(const char* applicationName, const char* commandLine) {
    if (applicationName && applicationName[0] != '\0')
        return std::string_view(applicationName);
    return FirstCommandLineToken(commandLine ? std::string_view(commandLine) : std::string_view{});
}

// The file name of the program a CreateProcess(A/W) call launches - the value
// the child-injection whitelist compares against.
inline std::string_view ProgramFileName(const char* applicationName, const char* commandLine) {
    return FileNameOfPath(ProgramPath(applicationName, commandLine));
}

}  // namespace ce::child_inject_policy
