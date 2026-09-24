#pragma once
// DLL Detection Utilities
//
// Provides functions for detecting DXVK, VKD3D-Proton, and other
// replacement DLLs by checking version resources.
// No Vulkan or graphics API dependencies — safe to include from any context.

#include <windows.h>
#include <winver.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

// Version resources are read through the wide API. A path from
// GetModuleFileNameA names no file once the folder holds a character outside
// the code page (a Japanese game folder on a Western system), so DXVK,
// ReShade/Special K/OptiScaler and the Streamline generation all came up
// "unknown" for such a game. Callers holding a module use the HMODULE forms;
// the narrow-path forms remain for paths that are code-page text to begin with
// (configured override paths).

// The file's version resource, or an empty buffer when it has none.
static inline std::string DllReadVersionResourceW(const wchar_t* dllPath) {
    if (!dllPath || !dllPath[0]) {
        return {};
    }
    DWORD dummy = 0;
    const DWORD verSize = GetFileVersionInfoSizeW(dllPath, &dummy);
    if (verSize == 0) {
        return {};
    }
    std::string buf(verSize, '\0');
    if (!GetFileVersionInfoW(dllPath, 0, verSize, &buf[0])) {
        return {};
    }
    return buf;
}

static inline std::wstring DllPathToWide(const char* dllPath) {
    if (!dllPath || !dllPath[0]) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_ACP, 0, dllPath, -1, nullptr, 0);
    if (needed <= 1) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_ACP, 0, dllPath, -1, wide.data(), needed);
    wide.resize(static_cast<size_t>(needed - 1));
    return wide;
}

// `module`'s image path in UTF-16; empty on failure.
static inline std::wstring DllModulePathW(HMODULE module) {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = module ? GetModuleFileNameW(module, path, MAX_PATH) : 0;
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::wstring(path, length);
}

// Full VS_FIXEDFILEINFO file version, as major/minor/build. False when the file
// carries no version resource.
//
// Streamline 2.x populates all three (2.11.1 reports 2/11/1, 2.12.0 reports
// 2/12/0), which is what lets a caller reconstruct that distribution's own
// `sl::kSDKVersion` instead of hard-coding one version's value and mis-declaring
// itself to every other. Note that 1.x does NOT populate minor/build - The
// Witcher 3's sl.interposer reports 1/0/0 against a StringFileInfo of 1.5.6.0 -
// so this is sound for generation and for 2.x versions, but never for pinning a
// specific 1.x minor.
static inline bool DllFileVersionPartsW(const wchar_t* dllPath, uint32_t* outMajor, uint32_t* outMinor,
                                        uint32_t* outBuild) {
    const std::string buf = DllReadVersionResourceW(dllPath);
    if (buf.empty()) {
        return false;
    }
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedLen = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixedLen) || !fixed ||
        fixedLen < sizeof(VS_FIXEDFILEINFO)) {
        return false;
    }
    if (outMajor) {
        *outMajor = static_cast<uint32_t>(HIWORD(fixed->dwFileVersionMS));
    }
    if (outMinor) {
        *outMinor = static_cast<uint32_t>(LOWORD(fixed->dwFileVersionMS));
    }
    if (outBuild) {
        *outBuild = static_cast<uint32_t>(HIWORD(fixed->dwFileVersionLS));
    }
    return true;
}

static inline bool DllFileVersionParts(const char* dllPath, uint32_t* outMajor, uint32_t* outMinor,
                                       uint32_t* outBuild) {
    return DllFileVersionPartsW(DllPathToWide(dllPath).c_str(), outMajor, outMinor, outBuild);
}

static inline bool ModuleFileVersionParts(HMODULE module, uint32_t* outMajor, uint32_t* outMinor,
                                          uint32_t* outBuild) {
    return DllFileVersionPartsW(DllModulePathW(module).c_str(), outMajor, outMinor, outBuild);
}

// Major field of the file's VS_FIXEDFILEINFO version, or 0 when the file has no
// version resource. Streamline's own DLLs carry their API generation there -
// sl.interposer 1.5.6 reports 1, a 2.x distribution reports 2 - which is the one
// property every module in the set shares, plugins included.
static inline uint32_t DllFileMajorVersion(const char* dllPath) {
    uint32_t major = 0;
    return DllFileVersionParts(dllPath, &major, nullptr, nullptr) ? major : 0;
}

static inline uint32_t ModuleFileMajorVersion(HMODULE module) {
    uint32_t major = 0;
    return ModuleFileVersionParts(module, &major, nullptr, nullptr) ? major : 0;
}

// Returns true if any version-resource string field of the DLL at dllPath
// contains needle (ASCII, case-insensitive). Used to fingerprint DXVK ("dxvk"),
// VKD3D-Proton ("vkd3d") and third-party proxies beyond a mere path check.
static inline bool DllVersionStringContainsW(const wchar_t* dllPath, const char* needle) {
    const std::string buf = DllReadVersionResourceW(dllPath);
    if (buf.empty() || !needle || !needle[0]) {
        return false;
    }

    // Walk all language/codepage translations
    struct LangCP {
        WORD lang, cp;
    }* trans = nullptr;
    UINT transLen = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&trans), &transLen) ||
        !trans || transLen == 0)
        return false;

    const wchar_t* fields[] = {L"ProductName", L"FileDescription", L"InternalName", L"OriginalFilename"};
    const UINT count = transLen / sizeof(LangCP);
    const size_t needleLen = strlen(needle);
    for (UINT i = 0; i < count; i++) {
        for (const wchar_t* field : fields) {
            wchar_t subkey[128];
            swprintf(subkey, sizeof(subkey) / sizeof(subkey[0]), L"\\StringFileInfo\\%04x%04x\\%ls", trans[i].lang,
                     trans[i].cp, field);
            wchar_t* val = nullptr;
            UINT len = 0;
            if (!VerQueryValueW(buf.data(), subkey, reinterpret_cast<void**>(&val), &len) || !val || len <= 1)
                continue;
            // Manual ASCII case-insensitive substring search
            for (size_t j = 0; val[j] && j + needleLen <= len; j++) {
                size_t k = 0;
                for (; k < needleLen; ++k) {
                    const wchar_t lhs = val[j + k];
                    const wchar_t rhs = static_cast<unsigned char>(needle[k]);
                    const wchar_t lhsLower = (lhs >= L'A' && lhs <= L'Z') ? lhs + (L'a' - L'A') : lhs;
                    const wchar_t rhsLower = (rhs >= L'A' && rhs <= L'Z') ? rhs + (L'a' - L'A') : rhs;
                    if (lhsLower != rhsLower)
                        break;
                }
                if (k == needleLen)
                    return true;
            }
        }
    }
    return false;
}

static inline bool DllVersionStringContains(const char* dllPath, const char* needle) {
    return DllVersionStringContainsW(DllPathToWide(dllPath).c_str(), needle);
}

static inline bool ModuleVersionStringContains(HMODULE module, const char* needle) {
    return DllVersionStringContainsW(DllModulePathW(module).c_str(), needle);
}

// Whether `module` was loaded from the System32 directory.
static inline bool IsModuleInSystem32(HMODULE module) {
    const std::wstring loadedPath = DllModulePathW(module);
    wchar_t systemDir[MAX_PATH] = {};
    const UINT sysLen = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (loadedPath.empty() || sysLen == 0 || sysLen >= MAX_PATH || loadedPath.size() <= sysLen) {
        return false;
    }
    return _wcsnicmp(loadedPath.c_str(), systemDir, sysLen) == 0 &&
           (loadedPath[sysLen] == L'\\' || loadedPath[sysLen] == L'/');
}

// Returns true if dllName is currently loaded AND its path is outside System32
// (i.e. a non-system replacement DLL in the game directory).
static inline bool IsDllOutsideSystem32(const char* dllName) {
    HMODULE hMod = GetModuleHandleA(dllName);
    return hMod && !IsModuleInSystem32(hMod);
}

// Returns true if dllName is loaded from outside System32 AND its version
// resource identifies it as originating from project identified by needle
// (e.g. "dxvk" for DXVK, "vkd3d" for VKD3D-Proton).
static inline bool IsDllFromProject(const char* dllName, const char* versionNeedle) {
    HMODULE hMod = GetModuleHandleA(dllName);
    if (!hMod || IsModuleInSystem32(hMod))
        return false;  // Not loaded, or from System32 - not a replacement
    return ModuleVersionStringContains(hMod, versionNeedle);
}
