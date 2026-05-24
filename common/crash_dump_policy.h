#pragma once

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

#include <cstdint>
#include <cstdio>
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
inline constexpr ULONGLONG kExternalDumpStormWindowMs = 30'000;
inline constexpr uint32_t kExternalDumpStormTerminateHitThreshold = 3;
inline constexpr DWORD kExternalDumpStormTerminationExitCode = 0xE000D00D;
inline constexpr DWORD kFailFastExceptionExitCode = 0xC0000409;
inline constexpr DWORD kBreakpointExceptionExitCode = EXCEPTION_BREAKPOINT;

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

inline uint64_t StablePathHashAsciiInsensitive(const char* value) {
    uint64_t hash = 1469598103934665603ULL;
    if (!value) {
        return hash;
    }

    for (const char* current = value; *current != '\0'; ++current) {
        char c = *current;
        if (c == '\\' || c == '/') {
            c = '\\';
        } else {
            c = ToLowerAscii(c);
        }
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::string ShortHashSuffix(const char* value) {
    char buffer[16] = {};
    snprintf(buffer, sizeof(buffer), "%08llx",
             static_cast<unsigned long long>(StablePathHashAsciiInsensitive(value) & 0xffffffffULL));
    return buffer;
}

inline std::string AppendShortHashBeforeDumpExtension(std::string fileName, const char* hashSource) {
    if (fileName.empty()) {
        fileName = "dump.dmp";
    }

    const std::string suffix = "_" + ShortHashSuffix(hashSource);
    if (EndsWithAsciiInsensitive(fileName.c_str(), ".dmp")) {
        fileName.insert(fileName.size() - 4, suffix);
    } else {
        fileName += suffix;
        fileName += ".dmp";
    }
    return fileName;
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
    return AppendShortHashBeforeDumpExtension(mirroredName, sourcePathOrFileName);
}

inline std::string BuildSupplementalCrashDumpFileNameFromExternalSource(const char* sourcePathOrFileName) {
    const char* fileName = GetPathFileName(sourcePathOrFileName);
    if (!fileName || fileName[0] == '\0') {
        return kSupplementalExternalCrashDumpFallbackFileName;
    }

    std::string crashName = kSupplementalExternalCrashDumpPrefix;
    crashName += fileName;
    return AppendShortHashBeforeDumpExtension(crashName, sourcePathOrFileName);
}

inline std::string BuildInProgressDumpFileName(const char* finalDumpFileName) {
    if (!finalDumpFileName || finalDumpFileName[0] == '\0') {
        return "dump.dmp.inprogress";
    }

    std::string inProgressName = finalDumpFileName;
    inProgressName += ".inprogress";
    return inProgressName;
}

inline bool IsStaleEmptyInProgressDumpArtifact(const char* fileName, uint64_t fileSizeBytes) {
    return fileSizeBytes == 0 && EndsWithAsciiInsensitive(fileName, ".dmp.inprogress");
}

struct ExternalDumpSignature {
    DWORD processId = 0;
    std::string dumpBaseName;
    DWORD exceptionCode = 0;
    uintptr_t exceptionAddress = 0;
    DWORD exceptionThreadId = 0;
    bool hasExceptionInfo = false;
};

inline bool IsStrongExternalDumpSignature(const ExternalDumpSignature& signature) {
    return signature.processId != 0 && !signature.dumpBaseName.empty() && signature.hasExceptionInfo &&
           signature.exceptionCode != 0 && signature.exceptionAddress != 0;
}

inline std::string BuildExternalDumpSignatureKey(const ExternalDumpSignature& signature) {
    char buffer[256] = {};
    snprintf(buffer, sizeof(buffer), "pid=%lu;file=%s;code=%08lx;addr=%p;tid=%lu;strong=%d", signature.processId,
             signature.dumpBaseName.c_str(), signature.exceptionCode,
             reinterpret_cast<void*>(signature.exceptionAddress), signature.exceptionThreadId,
             IsStrongExternalDumpSignature(signature) ? 1 : 0);
    return buffer;
}

inline bool ShouldSuppressDuplicateExternalDumpArtifacts(uint32_t signatureHitCount, bool artifactAlreadyCaptured) {
    return artifactAlreadyCaptured && signatureHitCount > 1;
}

inline bool ShouldTerminateAfterExternalDumpStorm(bool strongSignature, uint32_t signatureHitCount,
                                                  ULONGLONG firstHitMs, ULONGLONG currentHitMs,
                                                  bool supplementalDumpCaptured, bool terminationAlreadyRequested) {
    if (!strongSignature || !supplementalDumpCaptured || terminationAlreadyRequested) {
        return false;
    }
    if (signatureHitCount < kExternalDumpStormTerminateHitThreshold) {
        return false;
    }
    return currentHitMs >= firstHitMs && (currentHitMs - firstHitMs) <= kExternalDumpStormWindowMs;
}

inline bool IsCrashLikeProcessExitCode(DWORD exitCode) {
    if (exitCode == kExternalDumpStormTerminationExitCode) {
        return false;
    }
    if (exitCode == kFailFastExceptionExitCode || exitCode == kBreakpointExceptionExitCode ||
        exitCode == EXCEPTION_ACCESS_VIOLATION || exitCode == EXCEPTION_ILLEGAL_INSTRUCTION ||
        exitCode == EXCEPTION_STACK_OVERFLOW) {
        return true;
    }

    // NTSTATUS severity bits 11xx identify error/status-failure exits. Normal
    // app exits such as 0, 1, or HRESULT-style success/warning codes stay out.
    return (exitCode & 0xC0000000UL) == 0xC0000000UL;
}

inline bool ShouldCapturePreTerminationDump(bool targetIsCurrentProcess, DWORD exitCode, bool alreadyAttempted,
                                            bool frameGenerationRuntimeActiveOrRecent = false) {
    if (!targetIsCurrentProcess || alreadyAttempted || exitCode == kExternalDumpStormTerminationExitCode) {
        return false;
    }
    if (IsCrashLikeProcessExitCode(exitCode)) {
        return true;
    }

    // Active/recent FG is useful context for abnormal process termination, but
    // clean game exits should not create confusing crash artifacts.
    return frameGenerationRuntimeActiveOrRecent && exitCode != 0;
}

inline bool ShouldSkipBreakpointExceptionDump(bool forceDump, bool debuggerPresent) {
    // Without a debugger, an unhandled STATUS_BREAKPOINT can terminate the
    // process without reaching ExitProcess/NtTerminateProcess hooks. Capture it
    // immediately; only debugger-owned breakpoints stay benign.
    return !forceDump && debuggerPresent;
}

}  // namespace ce::crash_dump_policy
