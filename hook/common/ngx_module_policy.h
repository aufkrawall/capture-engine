/**
 * Which module actually provides the NGX entry points.
 *
 * Only `nvngx.dll` (loaded from the driver store via the NGXCore `FullPath`
 * registry value) and the `_nvngx.dll` System32 stub export the
 * `NVSDK_NGX_*` functions. Streamline's `sl.dlss.dll` and `sl.common.dll` are
 * *callers*: `sl.common.dll` loads nvngx and resolves those exports with
 * GetProcAddress at DLSS-initialization time, which is far too late for any
 * IAT snapshot CE took earlier. Treating a Streamline plugin as the NGX
 * provider silently ends the search on a module that exports nothing, which is
 * exactly how the SR preset override stopped taking effect.
 */

#pragma once

#include <cstring>

namespace ce::ngx {

inline const char* ModuleFileName(const char* moduleNameOrPath) {
    if (!moduleNameOrPath) {
        return "";
    }
    const char* fileName = moduleNameOrPath;
    for (const char* cursor = moduleNameOrPath; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            fileName = cursor + 1;
        }
    }
    return fileName;
}

inline bool EqualsAsciiInsensitive(const char* value, const char* expected) {
    if (!value || !expected) {
        return false;
    }
    for (;; ++value, ++expected) {
        char left = *value;
        char right = *expected;
        if (left >= 'A' && left <= 'Z') {
            left = static_cast<char>(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z') {
            right = static_cast<char>(right - 'A' + 'a');
        }
        if (left != right) {
            return false;
        }
        if (left == '\0') {
            return true;
        }
    }
}

// True only for the modules that really export NVSDK_NGX_*. Their export
// bodies are the authoritative interception point, because a caller that
// resolved the pointer through GetProcAddress cannot be reached any other way.
inline bool IsNgxCoreModulePath(const char* moduleNameOrPath) {
    const char* fileName = ModuleFileName(moduleNameOrPath);
    if (!fileName || !*fileName) {
        return false;
    }
    return EqualsAsciiInsensitive(fileName, "nvngx.dll") || EqualsAsciiInsensitive(fileName, "_nvngx.dll");
}

// Streamline plugins reach NGX through sl.common.dll; they export no NGX entry
// points of their own and must never terminate the search for the provider.
inline bool IsStreamlineNgxClientPath(const char* moduleNameOrPath) {
    const char* fileName = ModuleFileName(moduleNameOrPath);
    if (!fileName || !*fileName) {
        return false;
    }
    return EqualsAsciiInsensitive(fileName, "sl.dlss.dll") || EqualsAsciiInsensitive(fileName, "sl.dlss_d.dll") ||
           EqualsAsciiInsensitive(fileName, "sl.dlss_g.dll") || EqualsAsciiInsensitive(fileName, "sl.common.dll") ||
           EqualsAsciiInsensitive(fileName, "sl.interposer.dll");
}

}  // namespace ce::ngx
