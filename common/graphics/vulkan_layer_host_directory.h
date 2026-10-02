#pragma once

// Where the Vulkan layer finds the CaptureEngine hook DLL when it has to load
// it into a process itself.
//
// A split-renderer title (RTX Remix: the 32-bit game hands rendering to a
// 64-bit NvRemixBridge.exe child) has no injected hook in the process that owns
// Vulkan, DLSS and Streamline. The layer therefore loads capture_hook_x64.dll
// into that renderer, so the profile's dlss_*_dll_path / streamline_dll_path /
// dlss_*_preset / dlss_debug_overlay overrides can act where the runtimes live.
//
// The layer runs from a versioned staging copy (%LOCALAPPDATA% or %ProgramData%
// \CaptureEngine\vulkan_layers\b<build>, common/graphics/vulkan_layer_registration.cpp),
// and only the layer and gate images are staged there. The hook DLL stays in the
// install directory beside captureengine.exe: it reads config.ini, writes crash
// dumps and loads its wrapper DLLs from its own folder, so it cannot run from a
// copy. Looking "beside the layer" therefore fails with ERROR_MOD_NOT_FOUND for
// every staged layer (Portal RTX, session 20260929_033904: `failed to load
// capture_hook_x64.dll (error=126)`), and the renderer never got its overrides.
//
// The host directory is handed over as a pointer file staged BESIDE the layer,
// not through the shared-memory discovery mapping. The staging directory is the
// trust anchor the loader already gave the layer DLL: whoever can write there
// can replace the layer itself. A path published in shared memory would let any
// same-session process that creates the mapping first name the DLL an elevated
// game loads. It also needs no shared-memory version bump, and it always names
// the install directory of the build that staged this very layer.

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ce::vulkan_layer_host_directory {

inline constexpr const wchar_t* kPointerFileName = L"VK_LAYER_CE_host_directory.txt";

// A directory path never needs more; bounds what the layer reads from the file.
inline constexpr size_t kMaxPointerFileBytes = 4096;

inline bool IsSeparator(wchar_t ch) {
    return ch == L'\\' || ch == L'/';
}

inline bool IsAsciiLetter(wchar_t ch) {
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
}

// X:\..., X:/..., \\server\share\... and \\?\X:\... - the spellings the loader
// treats as fully qualified. Anything else would be resolved against the
// process's current directory, which is exactly what a DLL load must not do.
inline bool IsFullyQualifiedPath(std::wstring_view path) {
    if (path.find(L'\0') != std::wstring_view::npos)
        return false;
    if (path.size() >= 3 && IsAsciiLetter(path[0]) && path[1] == L':' && IsSeparator(path[2]))
        return true;
    if (path.size() >= 3 && IsSeparator(path[0]) && IsSeparator(path[1]) && !IsSeparator(path[2])) {
        // `\\.\` names a device, not a directory a DLL can sit in.
        return !(path[2] == L'.' && (path.size() == 3 || IsSeparator(path[3])));
    }
    return false;
}

inline bool HasParentSegment(std::wstring_view path) {
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = start;
        while (end < path.size() && !IsSeparator(path[end]))
            ++end;
        if (path.substr(start, end - start) == L"..")
            return true;
        start = end + 1;
    }
    return false;
}

inline std::wstring_view TrimTrailingSeparators(std::wstring_view path) {
    // Keep the separator that makes `C:\` a root, and the two of a UNC prefix.
    while (path.size() > 3 && IsSeparator(path.back()))
        path.remove_suffix(1);
    return path;
}

// A directory a hook DLL may be loaded from: fully qualified, no `..` segment
// (GetModuleFileName never produces one for an install directory), no control
// characters. Shape only - existence is the loader's answer.
inline bool IsUsableHostDirectory(std::wstring_view directory) {
    if (!IsFullyQualifiedPath(directory) || HasParentSegment(directory))
        return false;
    for (const wchar_t ch : directory) {
        if (ch < 0x20)
            return false;
    }
    return true;
}

// The pointer file's bytes for `directory`: UTF-8, the directory only. Empty
// when the directory cannot be used, so callers write nothing rather than a
// pointer the layer would reject.
inline std::string Serialize(std::wstring_view directory) {
    directory = TrimTrailingSeparators(directory);
    if (!IsUsableHostDirectory(directory))
        return {};
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, directory.data(),
                                           static_cast<int>(directory.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0 || static_cast<size_t>(needed) > kMaxPointerFileBytes)
        return {};
    std::string utf8(static_cast<size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, directory.data(), static_cast<int>(directory.size()),
                            utf8.data(), needed, nullptr, nullptr) != needed) {
        return {};
    }
    return utf8;
}

// The directory a pointer file names, or empty when the file is not a usable
// pointer. Tolerates what an editor leaves behind (a UTF-8 BOM, a trailing
// newline) and refuses everything a partial write or a stray edit could produce:
// a second line, a relative path, an unusable directory.
inline std::wstring Parse(std::string_view contents) {
    if (contents.size() > kMaxPointerFileBytes)
        return {};
    if (contents.size() >= 3 && contents.substr(0, 3) == "\xEF\xBB\xBF")
        contents.remove_prefix(3);
    while (!contents.empty() && (contents.back() == '\r' || contents.back() == '\n' || contents.back() == ' ' ||
                                 contents.back() == '\t' || contents.back() == '\0')) {
        contents.remove_suffix(1);
    }
    if (contents.empty() || contents.find_first_of(std::string_view("\r\n\0", 3)) != std::string_view::npos)
        return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, contents.data(),
                                           static_cast<int>(contents.size()), nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, contents.data(), static_cast<int>(contents.size()),
                            wide.data(), needed) != needed) {
        return {};
    }
    const std::wstring_view trimmed = TrimTrailingSeparators(wide);
    if (!IsUsableHostDirectory(trimmed))
        return {};
    return std::wstring(trimmed);
}

inline bool EqualsIgnoreCaseAscii(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size())
        return false;
    for (size_t i = 0; i < left.size(); ++i) {
        wchar_t a = left[i];
        wchar_t b = right[i];
        if (a >= L'A' && a <= L'Z')
            a = static_cast<wchar_t>(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z')
            b = static_cast<wchar_t>(b - L'A' + L'a');
        if (a != b)
            return false;
    }
    return true;
}

// Full paths to try for `hookName`, most authoritative first:
//  1. the host directory the pointer file names - where the installed hook is;
//  2. the layer's own directory - a development layout where nothing was staged
//     (staging directory == install directory) keeps the hook beside the layer.
// Unusable and duplicate directories are dropped, so the list is exactly the
// attempts worth logging.
inline std::vector<std::wstring> HookLoadCandidates(std::wstring_view hostDirectory,
                                                    std::wstring_view layerDirectory,
                                                    std::wstring_view hookName) {
    std::vector<std::wstring> candidates;
    if (hookName.empty() || hookName.find_first_of(L"\\/:") != std::wstring_view::npos)
        return candidates;
    for (std::wstring_view directory : {hostDirectory, layerDirectory}) {
        directory = TrimTrailingSeparators(directory);
        if (!IsUsableHostDirectory(directory))
            continue;
        std::wstring path(directory);
        if (!IsSeparator(path.back()))
            path += L'\\';
        path += hookName;
        bool duplicate = false;
        for (const std::wstring& existing : candidates)
            duplicate = duplicate || EqualsIgnoreCaseAscii(existing, path);
        if (!duplicate)
            candidates.push_back(std::move(path));
    }
    return candidates;
}

}  // namespace ce::vulkan_layer_host_directory
