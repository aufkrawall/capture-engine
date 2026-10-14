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

// Reads the raw RT_VERSION (type 16) resource directly from a PE file on disk
// without calling GetFileVersionInfoSizeW / GetFileVersionInfoW.
//
// GetFileVersionInfo* maps the target via LoadLibraryExW(LOAD_LIBRARY_AS_DATAFILE),
// which re-enters the loader and any foreign LoadLibraryExW hook (e.g. Steam's
// gameoverlayrenderer64.dll). Calling it from DllMain while arming Streamline
// interposer takeover stalled DllMain for ~220 ms under the loader lock, letting
// The Witcher 3's main thread enter the resident sl.interposer.dll!slInit before
// its IAT could be gated (session 20261011_113848).
static inline std::string DllReadPeVersionResourceW(const wchar_t* dllPath) {
    if (!dllPath || !dllPath[0]) {
        return {};
    }
    HANDLE file = ::CreateFileW(dllPath, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    struct FileCloser {
        HANDLE handle;
        ~FileCloser() { ::CloseHandle(handle); }
    } closer{file};

    auto readAt = [&](uint64_t fileOffset, void* out, DWORD bytesToRead) -> bool {
        LARGE_INTEGER pos = {};
        pos.QuadPart = static_cast<LONGLONG>(fileOffset);
        if (!::SetFilePointerEx(file, pos, nullptr, FILE_BEGIN)) {
            return false;
        }
        DWORD bytesRead = 0;
        return ::ReadFile(file, out, bytesToRead, &bytesRead, nullptr) && bytesRead == bytesToRead;
    };

    BYTE headerBuf[4096] = {};
    DWORD headerBytes = 0;
    if (!::ReadFile(file, headerBuf, sizeof(headerBuf), &headerBytes, nullptr) ||
        headerBytes < sizeof(IMAGE_DOS_HEADER)) {
        return {};
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(headerBuf);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return {};
    }
    const size_t ntOffset = static_cast<size_t>(dos->e_lfanew);
    if (ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > headerBytes) {
        return {};
    }
    DWORD signature = 0;
    std::memcpy(&signature, headerBuf + ntOffset, sizeof(signature));
    if (signature != IMAGE_NT_SIGNATURE) {
        return {};
    }
    const auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(headerBuf + ntOffset + sizeof(DWORD));
    const size_t optOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    if (fileHeader->SizeOfOptionalHeader < sizeof(WORD) ||
        optOffset + fileHeader->SizeOfOptionalHeader > headerBytes) {
        return {};
    }
    WORD optMagic = 0;
    std::memcpy(&optMagic, headerBuf + optOffset, sizeof(optMagic));
    IMAGE_DATA_DIRECTORY rsrcDir = {};
    if (optMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
        fileHeader->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64)) {
        const auto* opt64 = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(headerBuf + optOffset);
        if (opt64->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_RESOURCE) {
            rsrcDir = opt64->DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        }
    } else if (optMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
               fileHeader->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32)) {
        const auto* opt32 = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(headerBuf + optOffset);
        if (opt32->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_RESOURCE) {
            rsrcDir = opt32->DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        }
    }
    if (rsrcDir.VirtualAddress == 0 || rsrcDir.Size < sizeof(IMAGE_RESOURCE_DIRECTORY)) {
        return {};
    }

    const size_t secOffset = optOffset + fileHeader->SizeOfOptionalHeader;
    const size_t numSections = fileHeader->NumberOfSections;
    if (numSections == 0 || numSections > 96 ||
        secOffset + numSections * sizeof(IMAGE_SECTION_HEADER) > headerBytes) {
        return {};
    }
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(headerBuf + secOffset);
    auto rvaToFileOffset = [&](uint32_t rva, uint32_t span, uint64_t* outFileOffset) -> bool {
        for (size_t i = 0; i < numSections; ++i) {
            const uint32_t va = sections[i].VirtualAddress;
            const uint32_t rawSize = sections[i].SizeOfRawData;
            const uint32_t virtSize = sections[i].Misc.VirtualSize;
            const uint32_t secLimit = virtSize > rawSize ? virtSize : rawSize;
            if (rva >= va && rva - va < secLimit) {
                const uint32_t delta = rva - va;
                if (delta <= rawSize && span <= rawSize - delta) {
                    *outFileOffset = static_cast<uint64_t>(sections[i].PointerToRawData) + delta;
                    return true;
                }
                return false;
            }
        }
        return false;
    };

    auto findChildOffset = [&](uint32_t dirRelOffset, bool wantTypeVersion, bool wantSubdir,
                               uint32_t* outChildRelOffset) -> bool {
        if (dirRelOffset > rsrcDir.Size ||
            sizeof(IMAGE_RESOURCE_DIRECTORY) > rsrcDir.Size - dirRelOffset) {
            return false;
        }
        uint64_t dirFileOffset = 0;
        IMAGE_RESOURCE_DIRECTORY dir = {};
        if (!rvaToFileOffset(rsrcDir.VirtualAddress + dirRelOffset, sizeof(dir), &dirFileOffset) ||
            !readAt(dirFileOffset, &dir, sizeof(dir))) {
            return false;
        }
        const uint32_t entryCount =
            static_cast<uint32_t>(dir.NumberOfNamedEntries) + static_cast<uint32_t>(dir.NumberOfIdEntries);
        if (entryCount == 0 || entryCount > 256) {
            return false;
        }
        const uint32_t entriesRelOffset = dirRelOffset + static_cast<uint32_t>(sizeof(IMAGE_RESOURCE_DIRECTORY));
        const uint32_t entriesBytes = entryCount * static_cast<uint32_t>(sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY));
        if (entriesRelOffset > rsrcDir.Size || entriesBytes > rsrcDir.Size - entriesRelOffset) {
            return false;
        }
        for (uint32_t i = 0; i < entryCount; ++i) {
            uint64_t entryFileOffset = 0;
            IMAGE_RESOURCE_DIRECTORY_ENTRY entry = {};
            const uint32_t curRel =
                entriesRelOffset + i * static_cast<uint32_t>(sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY));
            if (!rvaToFileOffset(rsrcDir.VirtualAddress + curRel, sizeof(entry), &entryFileOffset) ||
                !readAt(entryFileOffset, &entry, sizeof(entry))) {
                return false;
            }
            const bool isSubdir = (entry.OffsetToData & 0x80000000u) != 0;
            if (isSubdir != wantSubdir) {
                continue;
            }
            if (wantTypeVersion) {
                if ((entry.Name & 0x80000000u) != 0 || (entry.Name & 0xFFFFu) != 16u /* RT_VERSION */) {
                    continue;
                }
            }
            *outChildRelOffset = entry.OffsetToData & 0x7FFFFFFFu;
            return true;
        }
        return false;
    };

    uint32_t nameDirRel = 0;
    uint32_t langDirRel = 0;
    uint32_t dataEntryRel = 0;
    if (!findChildOffset(0, /*wantTypeVersion=*/true, /*wantSubdir=*/true, &nameDirRel) ||
        !findChildOffset(nameDirRel, /*wantTypeVersion=*/false, /*wantSubdir=*/true, &langDirRel) ||
        !findChildOffset(langDirRel, /*wantTypeVersion=*/false, /*wantSubdir=*/false, &dataEntryRel)) {
        return {};
    }

    if (dataEntryRel > rsrcDir.Size ||
        sizeof(IMAGE_RESOURCE_DATA_ENTRY) > rsrcDir.Size - dataEntryRel) {
        return {};
    }
    uint64_t dataEntryFileOffset = 0;
    IMAGE_RESOURCE_DATA_ENTRY dataEntry = {};
    if (!rvaToFileOffset(rsrcDir.VirtualAddress + dataEntryRel, sizeof(dataEntry), &dataEntryFileOffset) ||
        !readAt(dataEntryFileOffset, &dataEntry, sizeof(dataEntry))) {
        return {};
    }
    if (dataEntry.Size < sizeof(VS_FIXEDFILEINFO) || dataEntry.Size > 65536u) {
        return {};
    }
    uint64_t payloadFileOffset = 0;
    if (!rvaToFileOffset(dataEntry.OffsetToData, dataEntry.Size, &payloadFileOffset)) {
        return {};
    }
    std::string buf(static_cast<size_t>(dataEntry.Size) * 2, '\0');
    if (!readAt(payloadFileOffset, &buf[0], dataEntry.Size)) {
        return {};
    }
    return buf;
}

enum class DllVersionReadPolicy { AllowLoaderFallback, DirectPeOnly };

// DLL callbacks must fail closed on a missing/unreadable PE resource, never map a file through the loader.
static inline std::string DllReadVersionResourceW(
    const wchar_t* dllPath, DllVersionReadPolicy policy = DllVersionReadPolicy::AllowLoaderFallback) {
    if (!dllPath || !dllPath[0]) {
        return {};
    }
    if (std::string peBuf = DllReadPeVersionResourceW(dllPath); !peBuf.empty()) {
        return peBuf;
    }
    if (policy == DllVersionReadPolicy::DirectPeOnly) {
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

// The version resource of an image that is already mapped, read out of the
// mapping itself. GetFileVersionInfo re-opens the file through
// LoadLibraryEx(AS_DATAFILE), which is exactly the call a foreign overlay hooks
// (Steam's gameoverlayrenderer64 serializes it behind its own lock). CE's hook
// thread was inside that lock when Talos Reawakened's crash reporter began an
// in-process MiniDumpWriteDump, which suspends every other thread and then asks
// the same hook for each module's version: the game stood still for 50 s
// (logs/20260926_094906). A loaded module never needs the file: the resource is
// in its mapping, and FindResource/LoadResource do not go through the loader
// exports an overlay hooks.
//
// The copy is twice the resource size, the room GetFileVersionInfo leaves after
// the block for VerQueryValue's own bookkeeping.
static inline std::string ModuleReadVersionResource(HMODULE module) {
    if (!module) {
        return {};
    }
    HRSRC info = FindResourceW(module, MAKEINTRESOURCEW(VS_VERSION_INFO), MAKEINTRESOURCEW(16) /* RT_VERSION */);
    if (!info) {
        return {};
    }
    const DWORD size = SizeofResource(module, info);
    HGLOBAL loaded = size != 0 ? LoadResource(module, info) : nullptr;
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data) {
        return {};
    }
    std::string buf(static_cast<size_t>(size) * 2, '\0');
    memcpy(&buf[0], data, size);
    return buf;
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
static inline bool VersionResourceFileVersionParts(const std::string& buf, uint32_t* outMajor, uint32_t* outMinor,
                                                   uint32_t* outBuild) {
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

static inline bool DllFileVersionPartsW(
    const wchar_t* dllPath, uint32_t* outMajor, uint32_t* outMinor, uint32_t* outBuild,
    DllVersionReadPolicy policy = DllVersionReadPolicy::AllowLoaderFallback) {
    return VersionResourceFileVersionParts(DllReadVersionResourceW(dllPath, policy), outMajor, outMinor, outBuild);
}

static inline bool DllFileVersionParts(
    const char* dllPath, uint32_t* outMajor, uint32_t* outMinor, uint32_t* outBuild,
    DllVersionReadPolicy policy = DllVersionReadPolicy::AllowLoaderFallback) {
    return DllFileVersionPartsW(DllPathToWide(dllPath).c_str(), outMajor, outMinor, outBuild, policy);
}

static inline bool ModuleFileVersionParts(HMODULE module, uint32_t* outMajor, uint32_t* outMinor,
                                          uint32_t* outBuild) {
    return VersionResourceFileVersionParts(ModuleReadVersionResource(module), outMajor, outMinor, outBuild);
}

// Major field of the file's VS_FIXEDFILEINFO version, or 0 when the file has no
// version resource. Streamline's own DLLs carry their API generation there -
// sl.interposer 1.5.6 reports 1, a 2.x distribution reports 2 - which is the one
// property every module in the set shares, plugins included.
static inline uint32_t DllFileMajorVersion(
    const char* dllPath, DllVersionReadPolicy policy = DllVersionReadPolicy::AllowLoaderFallback) {
    uint32_t major = 0;
    return DllFileVersionParts(dllPath, &major, nullptr, nullptr, policy) ? major : 0;
}

static inline uint32_t DllFileMajorVersionW(
    const wchar_t* dllPath, DllVersionReadPolicy policy = DllVersionReadPolicy::AllowLoaderFallback) {
    uint32_t major = 0;
    return dllPath && dllPath[0] && DllFileVersionPartsW(dllPath, &major, nullptr, nullptr, policy) ? major : 0;
}

static inline uint32_t ModuleFileMajorVersion(HMODULE module) {
    uint32_t major = 0;
    return ModuleFileVersionParts(module, &major, nullptr, nullptr) ? major : 0;
}

// Returns true if any version-resource string field in `buf` contains needle
// (ASCII, case-insensitive). Used to fingerprint DXVK ("dxvk"), VKD3D-Proton
// ("vkd3d") and third-party proxies beyond a mere path check.
static inline bool VersionResourceStringContains(const std::string& buf, const char* needle) {
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

static inline bool DllVersionStringContainsW(const wchar_t* dllPath, const char* needle) {
    return VersionResourceStringContains(DllReadVersionResourceW(dllPath), needle);
}

static inline bool DllVersionStringContains(const char* dllPath, const char* needle) {
    return DllVersionStringContainsW(DllPathToWide(dllPath).c_str(), needle);
}

// A loaded module's own mapping answers; see ModuleReadVersionResource.
static inline bool ModuleVersionStringContains(HMODULE module, const char* needle) {
    return VersionResourceStringContains(ModuleReadVersionResource(module), needle);
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
