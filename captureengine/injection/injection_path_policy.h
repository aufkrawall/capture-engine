#pragma once

// Pure path-policy helpers shared by the injection security gates. Kept free of
// injection-internal headers so unit tests can exercise them in isolation.
#include <windows.h>

#include <string>

namespace ce::injection {

// True when childPath equals or resides inside parentDir. Both inputs are
// expected to be weakly-canonical absolute paths as produced by
// std::filesystem::weakly_canonical on Windows. A shared string prefix alone is
// not sufficient: "C:\appdir2\x.dll" must not pass for "C:\appdir".
template <typename String>
inline bool IsPathInsideDirectory(const String& childPath, const String& parentDir) {
    if (parentDir.empty() || childPath.size() < parentDir.size())
        return false;
    if (childPath.compare(0, parentDir.size(), parentDir) != 0)
        return false;
    if (childPath.size() == parentDir.size())
        return true;
    return childPath[parentDir.size()] == static_cast<typename String::value_type>('\\');
}

inline bool IsPathInsideDirectory(const char* childPath, const char* parentDir) {
    return IsPathInsideDirectory(std::string(childPath ? childPath : ""), std::string(parentDir ? parentDir : ""));
}

inline bool IsPathInsideDirectory(const wchar_t* childPath, const wchar_t* parentDir) {
    return IsPathInsideDirectory(std::wstring(childPath ? childPath : L""),
                                 std::wstring(parentDir ? parentDir : L""));
}

// The hook DLL path is carried as UTF-16 from GetModuleFileNameW all the way to
// the remote LoadLibraryW. The ANSI route (GetModuleFileNameA + LoadLibraryA)
// replaced every character the system code page cannot represent with '?', so
// an installation below, for example, a Cyrillic or CJK user-profile folder on
// a Western-locale Windows could never be injected at all. UTF-8 is only the
// log representation. Returns an empty string when conversion fails.
inline std::string WidePathToUtf8(const std::wstring& text) {
    if (text.empty())
        return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), length, nullptr,
                            nullptr) != length)
        return {};
    return result;
}

// Bytes WriteProcessMemory must copy for LoadLibraryW, terminator included.
inline size_t RemoteWidePathBytes(const std::wstring& path) {
    return (path.size() + 1) * sizeof(wchar_t);
}

}  // namespace ce::injection
