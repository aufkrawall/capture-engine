#pragma once

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

#include <cstring>

namespace ce::crash_dump_policy {

inline constexpr const char* kSymbolArchiveDirName = "symbols";
inline constexpr const char* kCaptureEngineArchiveDirName = "captureengine";
inline constexpr const char* kSymbolArchiveManifestFileName = "manifest.txt";

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

inline constexpr MINIDUMP_TYPE kCompatibilityFreezeDumpType = static_cast<MINIDUMP_TYPE>(
    MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
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

}  // namespace ce::crash_dump_policy
