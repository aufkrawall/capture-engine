#pragma once
// DLL Detection Utilities
//
// Provides functions for detecting DXVK, VKD3D-Proton, and other
// replacement DLLs by checking version resources.
// No Vulkan or graphics API dependencies — safe to include from any context.

#include <windows.h>
#include <string>

// Returns true if any version-resource string field of the DLL at dllPath
// contains needle (case-insensitive). Used to fingerprint DXVK ("dxvk") and
// VKD3D-Proton ("vkd3d") beyond a mere path check.
static inline bool DllVersionStringContains(const char* dllPath, const char* needle) {
    DWORD dummy = 0;
    DWORD verSize = GetFileVersionInfoSizeA(dllPath, &dummy);
    if (verSize == 0)
        return false;

    std::string buf(verSize, '\0');
    if (!GetFileVersionInfoA(dllPath, 0, verSize, &buf[0]))
        return false;

    // Walk all language/codepage translations
    struct LangCP {
        WORD lang, cp;
    }* trans = nullptr;
    UINT transLen = 0;
    if (!VerQueryValueA(buf.data(), "\\VarFileInfo\\Translation", reinterpret_cast<void**>(&trans), &transLen) ||
        !trans || transLen == 0)
        return false;

    const char* fields[] = {"ProductName", "FileDescription", "InternalName", "OriginalFilename"};
    UINT count = transLen / sizeof(LangCP);
    size_t needleLen = strlen(needle);
    for (UINT i = 0; i < count; i++) {
        for (const char* field : fields) {
            char subkey[128];
            snprintf(subkey, sizeof(subkey), "\\StringFileInfo\\%04x%04x\\%s", trans[i].lang, trans[i].cp, field);
            char* val = nullptr;
            UINT len = 0;
            if (!VerQueryValueA(buf.data(), subkey, reinterpret_cast<void**>(&val), &len) || !val || len <= 1)
                continue;
            // Manual case-insensitive substring search
            for (size_t j = 0; val[j] && j + needleLen <= len; j++) {
                if (_strnicmp(val + j, needle, needleLen) == 0)
                    return true;
            }
        }
    }
    return false;
}

// Returns true if dllName is currently loaded AND its path is outside System32
// (i.e. a non-system replacement DLL in the game directory).
static inline bool IsDllOutsideSystem32(const char* dllName) {
    HMODULE hMod = GetModuleHandleA(dllName);
    if (!hMod)
        return false;
    char loadedPath[MAX_PATH] = {};
    char systemDir[MAX_PATH] = {};
    GetModuleFileNameA(hMod, loadedPath, MAX_PATH);
    GetSystemDirectoryA(systemDir, MAX_PATH);
    size_t sysLen = strlen(systemDir);
    return !(_strnicmp(loadedPath, systemDir, sysLen) == 0 &&
             (loadedPath[sysLen] == '\\' || loadedPath[sysLen] == '/'));
}

// Returns true if dllName is loaded from outside System32 AND its version
// resource identifies it as originating from project identified by needle
// (e.g. "dxvk" for DXVK, "vkd3d" for VKD3D-Proton).
static inline bool IsDllFromProject(const char* dllName, const char* versionNeedle) {
    HMODULE hMod = GetModuleHandleA(dllName);
    if (!hMod)
        return false;
    char loadedPath[MAX_PATH] = {};
    char systemDir[MAX_PATH] = {};
    GetModuleFileNameA(hMod, loadedPath, MAX_PATH);
    GetSystemDirectoryA(systemDir, MAX_PATH);
    size_t sysLen = strlen(systemDir);
    if (_strnicmp(loadedPath, systemDir, sysLen) == 0 && (loadedPath[sysLen] == '\\' || loadedPath[sysLen] == '/'))
        return false;  // From System32 — not a replacement
    return DllVersionStringContains(loadedPath, versionNeedle);
}
