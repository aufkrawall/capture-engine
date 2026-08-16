#pragma once

#include <cstring>

// Classification of the configurable graphics runtime module family.
//
// CE can redirect these DLLs through the per-profile override paths
// (dlss_sr_dll_path, dlss_fg_dll_path, dlss_rr_dll_path, streamline_dll_path).
// The base-name classification drives (a) which loader loads get their full
// resolved path recorded for diagnostics and (b) which late-loaded modules get
// their LoadLibrary IAT patched so Streamline-internal loads reach the
// redirect.
namespace ce::graphics_runtime {

inline bool EqualsIgnoreCase(const char* value, const char* expected) {
    if (!value || !expected) {
        return false;
    }
    while (*value != '\0' && *expected != '\0') {
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
        ++value;
        ++expected;
    }
    return *value == '\0' && *expected == '\0';
}

inline bool HasPrefixIgnoreCase(const char* value, const char* prefix) {
    if (!value || !prefix) {
        return false;
    }
    while (*prefix != '\0') {
        char left = *value;
        char right = *prefix;
        if (left >= 'A' && left <= 'Z') {
            left = static_cast<char>(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z') {
            right = static_cast<char>(right - 'A' + 'a');
        }
        if (left != right) {
            return false;
        }
        ++value;
        ++prefix;
    }
    return true;
}

// Compares two module paths the way the Windows loader keys module identity:
// case-insensitive, with '/' and '\' treated as the same separator.
inline bool EqualsModulePathIgnoreCase(const char* left, const char* right) {
    if (!left || !right) {
        return false;
    }
    while (*left != '\0' && *right != '\0') {
        char a = *left;
        char b = *right;
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a == '/') {
            a = '\\';
        }
        if (b == '/') {
            b = '\\';
        }
        if (a != b) {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

// A runtime-override redirect must never introduce a SECOND instance of a
// module base name that is already loaded from a different file.
//
// Windows keys module identity on the resolved path, so a name-based load of an
// already-loaded runtime returns that instance, while the same load rewritten to
// the override directory maps a duplicate image. The Streamline/NGX runtimes are
// process-global singletons: a duplicate gets its own uninitialised plugin
// registry, and anything that reaches it (including CE's own export hooks, which
// forward each symbol through ONE process-global pointer) drives the game's
// Streamline calls into the wrong image. Session 20260816_045933 is exactly that
// — CE pinned the live sl.interposer by re-loading its full path, this redirect
// rewrote the pin to the override copy, CE hooked the duplicate and then freed
// it, and the game's slSetTag/slEvaluateFeature became silent no-ops until
// sl.dlss_g dereferenced null.
//
// The same rule already governs `PreloadConfiguredGraphicsRuntimeDlls`: a name
// that is already loaded keeps the loaded copy, because a second instance cannot
// take effect anyway.
inline bool WouldRedirectDuplicateLoadedModule(const char* redirectTargetPath, bool baseNameAlreadyLoaded,
                                               const char* loadedModulePath) {
    if (!baseNameAlreadyLoaded) {
        return false;
    }
    if (!redirectTargetPath || !redirectTargetPath[0]) {
        return false;
    }
    if (!loadedModulePath || !loadedModulePath[0]) {
        // The base name is loaded but its path is unresolvable. Fail closed:
        // honouring the redirect could map a duplicate we cannot rule out.
        return true;
    }
    return !EqualsModulePathIgnoreCase(redirectTargetPath, loadedModulePath);
}

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

// Maps an NVIDIA NGX model-repository segment (the folder name under
// ...\NVIDIA\NGX\models\) to the real Streamline DLL base name. The driver
// stores the Streamline plugins in the model cache under hashed file names
// (e.g. 1B0_E658703.dll) inside model folders such as "sl_dlss_g_0"; the
// loader-visible base name therefore carries no sl.* token and the ordinary
// base-name redirect cannot match it.
inline bool ModelSegmentToDllName(const char* segment, char* out, size_t outSize) {
    if (!segment || !segment[0] || !out || outSize == 0) {
        return false;
    }
    if (!HasPrefixIgnoreCase(segment, "sl_")) {
        return false;
    }

    // Expect "sl_<name>_<digits>", e.g. "sl_dlss_g_0" -> "sl.dlss_g.dll".
    // Locate the trailing "_<digits>" and the first '_' after the "sl" prefix.
    size_t segmentLen = 0;
    while (segment[segmentLen] != '\0') {
        ++segmentLen;
    }
    size_t trailingUnderscore = segmentLen;
    while (trailingUnderscore > 0 && segment[trailingUnderscore - 1] >= '0' &&
           segment[trailingUnderscore - 1] <= '9') {
        --trailingUnderscore;
    }
    if (trailingUnderscore == 0 || trailingUnderscore == segmentLen ||
        segment[trailingUnderscore - 1] != '_' || trailingUnderscore < 4) {
        return false;
    }

    const size_t firstUnderscore = 2;  // after "sl"
    if (firstUnderscore + 1 >= trailingUnderscore - 1) {
        return false;
    }

    // "sl" + "." + name + ".dll" (name length = trailingUnderscore - 3).
    const size_t nameLength = trailingUnderscore - 3;
    if (3 + nameLength + 4 >= outSize) {  // "sl." + name + ".dll" + NUL
        return false;
    }
    size_t pos = 0;
    out[pos++] = 's';
    out[pos++] = 'l';
    out[pos++] = '.';
    for (size_t i = firstUnderscore + 1; i < trailingUnderscore - 1; ++i) {
        const char c = segment[i];
        out[pos++] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    out[pos++] = '.';
    out[pos++] = 'd';
    out[pos++] = 'l';
    out[pos++] = 'l';
    out[pos] = '\0';
    return true;
}

// True when the path points into the NVIDIA NGX model repository where the
// driver stores the Streamline plugins under hashed file names.
inline bool IsNgxModelRepositoryPath(const char* path) {
    if (!path || !path[0]) {
        return false;
    }
    // "...\NVIDIA\NGX\models\" - case-insensitive, both separator styles.
    const char* cursor = path;
    while (*cursor) {
        if ((*cursor == 'n' || *cursor == 'N') &&
            (cursor[1] == 'v' || cursor[1] == 'V') &&
            (cursor[2] == 'i' || cursor[2] == 'I') &&
            (cursor[3] == 'd' || cursor[3] == 'D') &&
            (cursor[4] == 'i' || cursor[4] == 'I') &&
            (cursor[5] == 'a' || cursor[5] == 'A') &&
            (cursor[6] == '\\' || cursor[6] == '/') &&
            (cursor[7] == 'n' || cursor[7] == 'N') &&
            (cursor[8] == 'g' || cursor[8] == 'G') &&
            (cursor[9] == 'x' || cursor[9] == 'X') &&
            (cursor[10] == '\\' || cursor[10] == '/') &&
            (cursor[11] == 'm' || cursor[11] == 'M') &&
            (cursor[12] == 'o' || cursor[12] == 'O') &&
            (cursor[13] == 'd' || cursor[13] == 'D') &&
            (cursor[14] == 'e' || cursor[14] == 'E') &&
            (cursor[15] == 'l' || cursor[15] == 'L') &&
            (cursor[16] == 's' || cursor[16] == 'S') &&
            (cursor[17] == '\\' || cursor[17] == '/')) {
            return true;
        }
        ++cursor;
    }
    return false;
}

inline bool IsRuntimeModuleBaseName(const char* baseName) {
    if (!baseName || !baseName[0]) {
        return false;
    }
    baseName = ModuleFileName(baseName);
    if (!baseName[0]) {
        return false;
    }
    // The Streamline family uses the sl.* prefix (interposer, common, dlss,
    // dlss_g, dlss_d, directsr, nis, nvperf, reflex, pcl, deepdvc, ...). The
    // prefix rule intentionally mirrors GetRedirectedPath's match semantics so
    // the diagnostic family is exactly the set the redirect could rewrite.
    if (HasPrefixIgnoreCase(baseName, "sl.")) {
        return true;
    }
    return EqualsIgnoreCase(baseName, "nvngx_dlss.dll") ||
           EqualsIgnoreCase(baseName, "nvngx_dlssg.dll") ||
           EqualsIgnoreCase(baseName, "nvngx_dlssd.dll") ||
           EqualsIgnoreCase(baseName, "nvngx_deepdvc.dll") ||
           EqualsIgnoreCase(baseName, "nvlowlatencyvk.dll") ||
           EqualsIgnoreCase(baseName, "nvngx.dll") ||
           EqualsIgnoreCase(baseName, "_nvngx.dll") ||
           EqualsIgnoreCase(baseName, "nvapi64.dll") ||
           EqualsIgnoreCase(baseName, "nvapi.dll");
}

}  // namespace ce::graphics_runtime
