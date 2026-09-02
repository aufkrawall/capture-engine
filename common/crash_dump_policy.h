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
// STATUS_PROCESS_IS_TERMINATING. NVIDIA's DLSS snippet worker calls
// NtTerminateProcess with this sentinel during process teardown (observed with
// the 310.7 runtime in Talos/RoboCop on 2026-08-09); the game has already
// exited cleanly, so this concurrent call is a losing teardown race, not a
// crash. The severity bits would otherwise classify it as a crash-like exit.
inline constexpr DWORD kProcessIsTerminatingExitCode = 0xC000004B;

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

// In-process MiniDumpWriteDump fallbacks run inside the game process. With a foreign overlay module
// loaded (Steam overlay / RTSS) the dump's module/version enumeration can deadlock inside the
// overlay's hooked version APIs (session 20260813_222058: the game's own fatal dump froze the render
// thread inside dbgcore -> GetFileVersionInfoW -> gameoverlayrenderer64 until the watchdog killed the
// app). The external helper process has neither overlay loaded, so the fallback is only legal when no
// foreign overlay is present; otherwise a missing dump is preferable to a hung game thread.
inline bool ShouldUseInProcessMiniDumpFallbackAfterExternalHelperFailure(bool foreignOverlayLoaded) {
    return !foreignOverlayLoaded;
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
    if (exitCode == kExternalDumpStormTerminationExitCode ||
        exitCode == kProcessIsTerminatingExitCode) {
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

// Where a termination request came from, resolved from the caller's return
// address. Only the primary image is distinguished, because that is the one
// distinction that carries meaning: code inside the process's own executable
// deciding to end the process is the application quitting, while a request
// raised from any loaded module can be a runtime failing on the way out.
// Anything that cannot be resolved stays `kUnknown` and is never suppressed.
enum class TerminationOrigin : uint8_t {
    kUnknown = 0,
    kPrimaryModule,
    kLoadedModule,
};

inline bool ShouldCapturePreTerminationDump(bool targetIsCurrentProcess, DWORD exitCode, bool alreadyAttempted,
                                            bool frameGenerationRuntimeActiveOrRecent = false,
                                            TerminationOrigin origin = TerminationOrigin::kUnknown) {
    // STATUS_PROCESS_IS_TERMINATING is the runtime's own "the process is
    // already exiting" sentinel; it never represents a genuine abnormal exit,
    // so it must also skip the active-FG fallback below.
    if (!targetIsCurrentProcess || alreadyAttempted || exitCode == kExternalDumpStormTerminationExitCode ||
        exitCode == kProcessIsTerminatingExitCode) {
        return false;
    }
    if (IsCrashLikeProcessExitCode(exitCode)) {
        return true;
    }
    if (!frameGenerationRuntimeActiveOrRecent || exitCode == 0) {
        return false;
    }

    // A live FG runtime at termination is the normal state of every game that
    // uses frame generation - quitting never turns it off first - so it cannot
    // by itself mean the exit was abnormal. What this fallback exists for is an
    // FG runtime killing the process while tearing down, and such a request
    // never originates in the application's own image. A game terminating
    // itself from its own code with a non-crash exit code is quitting, and
    // dumping several hundred megabytes for that buries the real artifacts.
    // Crash machinery is unaffected: abort/terminate/_purecall/fail-fast all
    // report a crash-like exit code and returned above, whether the CRT is
    // statically linked into the executable or not.
    return origin != TerminationOrigin::kPrimaryModule;
}

// UE5's ensure() macro raises this continuable code; the filter answers it with
// its own fast assert dump instead of the worker path.
inline constexpr DWORD kUe5EnsureExceptionCode = 0x00004000;

// Every Windows exception code carries NTSTATUS severity in its top two bits.
// Only severity 0b11 (error) codes are faults that terminate a thread when
// nobody handles them. Severity 0b00 (success), 0b01 (informational) and 0b10
// (warning) codes are raised deliberately through RaiseException by a caller
// that also handles them.
inline constexpr bool IsErrorSeverityExceptionCode(DWORD code) {
    return (code & 0xC0000000UL) == 0xC0000000UL;
}

// The non-error-severity codes CE deliberately still classifies as dump-worthy.
// Each one is either a real termination path that Windows encodes below error
// severity, or a COM/DXGI failure whose own counting/logging rule lives in the
// exception filter.
inline constexpr bool IsDumpWorthyNonErrorSeverityExceptionCode(DWORD code) {
    switch (code) {
        case static_cast<DWORD>(EXCEPTION_BREAKPOINT):  // can terminate without ExitProcess
        case kUe5EnsureExceptionCode:                   // answered by the quick assert dump
        case 0x80010108UL:                              // RPC_E_DISCONNECTED
        case 0x80004002UL:                              // E_NOINTERFACE
        case 0x80004005UL:                              // E_FAIL
        case 0x800706baUL:                              // RPC_S_SERVER_UNAVAILABLE
        case 0x8876086aUL:                              // DXGI_ERROR_DEVICE_RESET
        case 0x887a0006UL:                              // DXGI_ERROR_DEVICE_HUNG
        case 0x887a0007UL:                              // DXGI_ERROR_DEVICE_REMOVED
        case 0x887a0020UL:                              // DXGI_ERROR_ACCESS_LOST
            return true;
        default:
            return false;
    }
}

// First-chance classification for the vectored handler. The handler sees EVERY
// exception raised anywhere in the host process, most of which the raiser
// handles itself, and writing a dump for one costs the whole process a
// multi-second stall (every thread is suspended for the duration of an
// in-process MiniDumpWriteDump). Black Myth: Wukong raises RPC_S_CALL_CANCELLED
// (0x0000071A) from rpcrt4 while CEF cancels its session-notification wait on
// exit; CE classified that benign cancellation as a crash twice, froze the
// exiting game for ~62 s each time, and consumed the one-dump budget so the
// real access violation that followed got no dump at all
// (session 20260817_052857).
//
// An exception that nobody handles still reaches the top-level unhandled filter,
// which re-enters with forceDump so genuinely fatal cases are never lost.
inline bool ShouldTreatFirstChanceExceptionAsCrash(DWORD code, bool forceDump) {
    if (forceDump) {
        return true;
    }
    return IsErrorSeverityExceptionCode(code) || IsDumpWorthyNonErrorSeverityExceptionCode(code);
}

// The crash-dump worker runs inside the crashing process, and dbghelp reads the
// version resource of every loaded module while it writes the module list. A
// foreign overlay that hooks the loader/version APIs turns that walk into a
// minutes-long serialized round trip with every other thread suspended
// (session 20260817_052857: 61.6 s for a single MiniDumpNormal with the Steam
// overlay loaded, matching the 20260813_222058 in-process dump freeze). The
// external helper process has no overlay loaded and does not suspend the game,
// so prefer it whenever a foreign overlay is present.
inline bool ShouldPreferExternalCrashDumpHelper(bool foreignOverlayLoaded, bool externalHelperAvailable) {
    return foreignOverlayLoaded && externalHelperAvailable;
}

inline bool ShouldSkipBreakpointExceptionDump(bool forceDump, bool debuggerPresent) {
    // Without a debugger, an unhandled STATUS_BREAKPOINT can terminate the
    // process without reaching ExitProcess/NtTerminateProcess hooks. Capture it
    // immediately; only debugger-owned breakpoints stay benign.
    return !forceDump && debuggerPresent;
}

}  // namespace ce::crash_dump_policy
