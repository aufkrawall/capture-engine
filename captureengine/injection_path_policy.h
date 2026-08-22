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
inline bool IsPathInsideDirectory(const std::string& childPath, const std::string& parentDir) {
    if (parentDir.empty() || childPath.size() < parentDir.size())
        return false;
    if (childPath.compare(0, parentDir.size(), parentDir) != 0)
        return false;
    if (childPath.size() == parentDir.size())
        return true;
    return childPath[parentDir.size()] == '\\';
}

// Convert an ANSI path (the codepage GetModuleFileNameA and LoadLibraryA
// interpret) to wide form so signature verification sees exactly the file name
// the loader will open, including non-ASCII install directories. Widening
// char-by-char would sign-extend bytes >= 0x80 and verify a different path.
// Returns an empty string when conversion fails; callers fail closed.
inline std::wstring AnsiPathToWide(const std::string& text) {
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()),
                            result.data(), length) != length)
        return {};
    return result;
}

}  // namespace ce::injection
