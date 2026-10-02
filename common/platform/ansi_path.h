#pragma once

// Paths for the ANSI (A-suffixed) Windows APIs, derived from UTF-16.
//
// GetModuleFileNameA turns every path character outside the active code page
// into '?', and a '?'-mangled path names no file: the hook's log folder, the
// d3d12 debug flag files, a game folder probed for sl.interposer.dll, the
// version resource of a DXVK/ReShade/Streamline module - each silently came up
// empty for an installation or a game below such a folder (a Japanese game
// folder on a Western system, a Cyrillic user name). These helpers ask Windows
// in UTF-16 and hand the ANSI consumers either the exact code-page text or, when
// the code page cannot express it, the existing path's 8.3 short name (ASCII).
//
// Header-only so the Vulkan layer, which links no common/ object, can use it.
// ce::path::AnsiCompatiblePath (path_utils) forwards here.

#include <windows.h>

#include <string>

namespace ce::ansi_path {

// `wide` in the active code page, true only when every character survived.
// CP_UTF8 (the Windows "UTF-8 for worldwide language support" setting) takes no
// best-fit flag and represents every character.
inline bool TryNarrowAcpExactly(const std::wstring& wide, std::string* narrow) {
    narrow->clear();
    if (wide.empty()) {
        return true;
    }
    const bool utf8 = GetACP() == CP_UTF8;
    const DWORD flags = utf8 ? 0 : WC_NO_BEST_FIT_CHARS;
    BOOL usedDefault = FALSE;
    const int needed = WideCharToMultiByte(CP_ACP, flags, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0,
                                           nullptr, utf8 ? nullptr : &usedDefault);
    if (needed <= 0) {
        return false;
    }
    narrow->assign(static_cast<size_t>(needed), '\0');
    usedDefault = FALSE;
    WideCharToMultiByte(CP_ACP, flags, wide.c_str(), static_cast<int>(wide.size()), narrow->data(), needed, nullptr,
                        utf8 ? nullptr : &usedDefault);
    return usedDefault == FALSE;
}

// The code-page text of `widePath` when exact, else the existing path's 8.3
// short name. *exact is false only when neither works; the '?'-substituted
// text is returned then.
inline std::string CompatiblePath(const std::wstring& widePath, bool* exact = nullptr) {
    std::string narrow;
    if (TryNarrowAcpExactly(widePath, &narrow)) {
        if (exact) {
            *exact = true;
        }
        return narrow;
    }
    const DWORD shortLength = GetShortPathNameW(widePath.c_str(), nullptr, 0);
    if (shortLength > 0) {
        std::wstring shortPath(static_cast<size_t>(shortLength), L'\0');
        const DWORD written = GetShortPathNameW(widePath.c_str(), shortPath.data(), shortLength);
        if (written > 0 && written < shortLength) {
            shortPath.resize(written);
            std::string shortNarrow;
            if (TryNarrowAcpExactly(shortPath, &shortNarrow)) {
                if (exact) {
                    *exact = true;
                }
                return shortNarrow;
            }
        }
    }
    if (exact) {
        *exact = false;
    }
    // The lossy form, still useful in a log line.
    const int needed =
        WideCharToMultiByte(CP_ACP, 0, widePath.c_str(), static_cast<int>(widePath.size()), nullptr, 0, nullptr, nullptr);
    if (needed > 0) {
        narrow.assign(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_ACP, 0, widePath.c_str(), static_cast<int>(widePath.size()), narrow.data(), needed,
                            nullptr, nullptr);
    }
    return narrow;
}

// `module`'s image path in UTF-16 (nullptr: the process executable). Empty on
// failure; paths longer than MAX_PATH are supported.
inline std::wstring ModulePathW(HMODULE module) {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        if (path.size() >= 32768) {
            return {};
        }
        path.resize(path.size() * 2);
    }
}

// The directory part of a path, without a trailing separator ("" when none).
inline std::wstring ParentDirectoryW(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// Drop-in replacement for GetModuleFileNameA when the result is used to open,
// probe or derive a path: CompatiblePath of the UTF-16 image path. Returns the
// length written (0 on failure or when `size` is too small), like the API.
// Not for base-name matching: the 8.3 fallback changes the file name.
inline DWORD ModuleFileNameAnsi(HMODULE module, char* out, DWORD size, bool* exact = nullptr) {
    if (!out || size == 0) {
        return 0;
    }
    out[0] = '\0';
    const std::wstring wide = ModulePathW(module);
    if (wide.empty()) {
        return 0;
    }
    const std::string narrow = CompatiblePath(wide, exact);
    if (narrow.empty() || narrow.size() >= size) {
        return 0;
    }
    narrow.copy(out, narrow.size());
    out[narrow.size()] = '\0';
    return static_cast<DWORD>(narrow.size());
}

// The folder holding `module`'s image, as an ANSI-API path (see CompatiblePath).
inline std::string ModuleDirectoryAnsi(HMODULE module, bool* exact = nullptr) {
    const std::wstring directory = ParentDirectoryW(ModulePathW(module));
    if (directory.empty()) {
        if (exact) {
            *exact = false;
        }
        return {};
    }
    return CompatiblePath(directory, exact);
}

// An ANSI path (code-page text) in UTF-16, for handing it to a W API.
inline std::wstring AnsiToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), needed);
    return wide;
}

}  // namespace ce::ansi_path
