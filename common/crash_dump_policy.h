#pragma once

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

#include <cstring>
#include <string>

namespace ce::crash_dump_policy {

inline constexpr const char* kSymbolArchiveDirName = "symbols";
inline constexpr const char* kCaptureEngineArchiveDirName = "captureengine";
inline constexpr const char* kSymbolArchiveManifestFileName = "manifest.txt";
inline constexpr const char* kMirroredExternalDumpPrefix = "external_";
inline constexpr const char* kMirroredExternalDumpFallbackFileName = "external_dump.dmp";
inline constexpr const char* kSupplementalExternalCrashDumpPrefix = "crash_external_";
inline constexpr const char* kSupplementalExternalCrashDumpFallbackFileName = "crash_external_dump.dmp";

inline constexpr MINIDUMP_TYPE kRichCrashDumpType = static_cast<MINIDUMP_TYPE>(
    MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
    MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo |
    MiniDumpScanMemory | MiniDumpIgnoreInaccessibleMemory);

inline constexpr MINIDUMP_TYPE kCompatibilityCrashDumpType =
    static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory);

inline constexpr MINIDUMP_TYPE kQuickAssertDumpType = static_cast<MINIDUMP_TYPE>(
    MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData |
    MiniDumpWithFullMemoryInfo | MiniDumpIgnoreInaccessibleMemory);

inline constexpr MINIDUMP_TYPE kRichFreezeDumpType = static_cast<MINIDUMP_TYPE>(
    MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
    MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo |
    MiniDumpIgnoreInaccessibleMemory);

inline constexpr MINIDUMP_TYPE kCompatibilityFreezeDumpType =
    static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                               MiniDumpWithIndirectlyReferencedMemory);

inline constexpr MINIDUMP_TYPE kMinimalDumpType = MiniDumpNormal;

inline constexpr char ToLowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool EndsWithAsciiInsensitive(const char* value, const char* suffix) {
    if (!value || !suffix) {
        return false;
    }
    const size_t valueLength = std::strlen(value);
    const size_t suffixLength = std::strlen(suffix);
    if (valueLength < suffixLength) {
        return false;
    }
    const size_t start = valueLength - suffixLength;
    for (size_t i = 0; i < suffixLength; ++i) {
        if (ToLowerAscii(value[start + i]) != ToLowerAscii(suffix[i])) {
            return false;
        }
    }
    return true;
}

inline bool ContainsAsciiInsensitive(const char* value, const char* needle) {
    if (!value || !needle) {
        return false;
    }
    const size_t valueLength = std::strlen(value);
    const size_t needleLength = std::strlen(needle);
    if (needleLength == 0) {
        return true;
    }
    if (valueLength < needleLength) {
        return false;
    }
    for (size_t offset = 0; offset + needleLength <= valueLength; ++offset) {
        bool match = true;
        for (size_t i = 0; i < needleLength; ++i) {
            if (ToLowerAscii(value[offset + i]) != ToLowerAscii(needle[i])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

inline bool ShouldArchiveInstalledCrashArtifactFileName(const char* fileName) {
    return !ContainsAsciiInsensitive(fileName, ".old.") &&
           (EndsWithAsciiInsensitive(fileName, ".exe") || EndsWithAsciiInsensitive(fileName, ".dll") ||
            EndsWithAsciiInsensitive(fileName, ".pdb"));
}

inline bool IsPathSeparator(char c) {
    return c == '\\' || c == '/';
}

inline size_t TrimTrailingPathSeparators(const char* value, size_t length) {
    while (length > 0 && IsPathSeparator(value[length - 1])) {
        --length;
    }
    return length;
}

inline bool PathEqualsOrHasDirectoryPrefixAsciiInsensitive(const char* path, const char* directory) {
    if (!path || !directory) {
        return false;
    }

    const size_t pathLength = std::strlen(path);
    const size_t directoryLength = TrimTrailingPathSeparators(directory, std::strlen(directory));
    if (directoryLength == 0 || pathLength < directoryLength) {
        return false;
    }

    for (size_t i = 0; i < directoryLength; ++i) {
        char pathChar = path[i];
        char directoryChar = directory[i];
        if (IsPathSeparator(pathChar)) {
            pathChar = '\\';
        } else {
            pathChar = ToLowerAscii(pathChar);
        }
        if (IsPathSeparator(directoryChar)) {
            directoryChar = '\\';
        } else {
            directoryChar = ToLowerAscii(directoryChar);
        }
        if (pathChar != directoryChar) {
            return false;
        }
    }

    return pathLength == directoryLength || IsPathSeparator(path[directoryLength]);
}

inline bool ShouldMirrorExternalDumpToSessionDirectory(const char* sourcePath, const char* sessionDirectory) {
    if (!sessionDirectory || sessionDirectory[0] == '\0') {
        return false;
    }
    if (!sourcePath || sourcePath[0] == '\0') {
        return true;
    }
    return !PathEqualsOrHasDirectoryPrefixAsciiInsensitive(sourcePath, sessionDirectory);
}

inline const char* GetPathFileName(const char* path) {
    if (!path || path[0] == '\0') {
        return nullptr;
    }

    const char* fileName = path;
    for (const char* current = path; *current != '\0'; ++current) {
        if (IsPathSeparator(*current)) {
            fileName = current + 1;
        }
    }
    return fileName;
}

inline std::string BuildMirroredExternalDumpFileName(const char* sourcePathOrFileName) {
    const char* fileName = GetPathFileName(sourcePathOrFileName);
    if (!fileName || fileName[0] == '\0') {
        return kMirroredExternalDumpFallbackFileName;
    }

    std::string mirroredName = kMirroredExternalDumpPrefix;
    mirroredName += fileName;
    if (!EndsWithAsciiInsensitive(mirroredName.c_str(), ".dmp")) {
        mirroredName += ".dmp";
    }
    return mirroredName;
}

inline std::string BuildSupplementalCrashDumpFileNameFromExternalSource(const char* sourcePathOrFileName) {
    const char* fileName = GetPathFileName(sourcePathOrFileName);
    if (!fileName || fileName[0] == '\0') {
        return kSupplementalExternalCrashDumpFallbackFileName;
    }

    std::string crashName = kSupplementalExternalCrashDumpPrefix;
    crashName += fileName;
    if (!EndsWithAsciiInsensitive(crashName.c_str(), ".dmp")) {
        crashName += ".dmp";
    }
    return crashName;
}

}  // namespace ce::crash_dump_policy
