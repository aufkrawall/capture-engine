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

// UE5 `ensure` is continuable and can fire in a storm (once per call site, and
// some titles re-ensure every frame). Each assert dump costs the whole process
// a synchronous MiniDumpWriteDump stall - the same ~61.6 s family as the rich
// path when a foreign overlay hooks the loader/version APIs - and uncapped it
// wrote another assert_*.dmp per ensure. The first few are worth keeping; after
// that the event is logged to crash.log only.
inline constexpr uint32_t kQuickAssertDumpPerProcessLimit = 3;

inline bool ShouldWriteQuickAssertDump(uint32_t quickAssertDumpsAlreadyWritten) {
    return quickAssertDumpsAlreadyWritten < kQuickAssertDumpPerProcessLimit;
}

inline constexpr MINIDUMP_TYPE kRichFreezeDumpType = static_cast<MINIDUMP_TYPE>(
    MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
    MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo |
    MiniDumpIgnoreInaccessibleMemory);

inline constexpr MINIDUMP_TYPE kCompatibilityFreezeDumpType =
    static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                               MiniDumpWithIndirectlyReferencedMemory);

// A freeze the application explains itself - its own modal dialog parked on the
// render thread - is still worth recording, but its memory is not: the process
// is sitting in a message pump, not corrupting anything. Thread stacks, thread
// info and the module list answer "which thread, in what call" at about a
// megabyte instead of thirty. Memory ranges a dump callback contributes (the
// WoW64 32-bit stacks) are still written, so a 32-bit target stays walkable.
inline constexpr MINIDUMP_TYPE kStackOnlyDumpType =
    static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                               MiniDumpIgnoreInaccessibleMemory);

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

inline bool EqualsAsciiInsensitive(const char* value, const char* other) {
    if (!value || !other) {
        return false;
    }
    size_t i = 0;
    for (; value[i] != '\0' && other[i] != '\0'; ++i) {
        if (ToLowerAscii(value[i]) != ToLowerAscii(other[i])) {
            return false;
        }
    }
    return value[i] == '\0' && other[i] == '\0';
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

// A C/C++ program that returns or exits with a small negative number (exit(-1),
// `return -1;` from main, a Steam shutdown callback ending in exit(-1)) reaches
// the kernel as 0xFFFFxxxx. Those values carry the error-severity bits but are
// not NTSTATUS codes: facility 0xFFF does not exist, so no exception, fail-fast
// or runtime abort ever produces one. Session 20260924_073945 dumped 49 MB and
// held Strange Brigade's exit for 1.7 s for exactly this.
inline bool IsSmallNegativeApplicationExitCode(DWORD exitCode) {
    const int32_t signedCode = static_cast<int32_t>(exitCode);
    return signedCode < 0 && signedCode >= -0xFFFF;
}

inline bool IsCrashLikeProcessExitCode(DWORD exitCode) {
    if (exitCode == kExternalDumpStormTerminationExitCode ||
        exitCode == kProcessIsTerminatingExitCode || IsSmallNegativeApplicationExitCode(exitCode)) {
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

// Windows reaches a single termination request through several layers that
// forward into one another, and CE hooks each of them separately: the
// application calls TerminateProcess and KERNELBASE calls NtTerminateProcess;
// exit() calls ExitProcess, which calls RtlExitUserProcess, which calls
// NtTerminateProcess. Only the outermost layer still sees the requester as its
// immediate caller - every layer below it is called by a Windows module, so
// classifying by that caller alone reports the plumbing instead of the
// requester. These are the modules that merely carry a request. The list stays
// deliberately narrow rather than covering the Windows directory wholesale,
// because plenty of modules that live there - the DriverStore copies of the
// NVIDIA FG runtimes among them - can legitimately be what ends the process.
inline constexpr const char* kTerminationPlumbingModuleNames[] = {
    "ntdll.dll",     "kernel32.dll", "kernelbase.dll",   "ucrtbase.dll",
    "ucrtbased.dll", "msvcrt.dll",   "vcruntime140.dll", "vcruntime140d.dll",
};

inline constexpr size_t kTerminationPlumbingModuleCount =
    sizeof(kTerminationPlumbingModuleNames) / sizeof(kTerminationPlumbingModuleNames[0]);

inline bool IsTerminationPlumbingModuleName(const char* moduleBaseName) {
    if (!moduleBaseName || moduleBaseName[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < kTerminationPlumbingModuleCount; ++i) {
        if (EqualsAsciiInsensitive(moduleBaseName, kTerminationPlumbingModuleNames[i])) {
            return true;
        }
    }
    return false;
}

// What a single stack frame on a termination path is, for attributing the
// request to the module that actually made it.
enum class TerminationFrameKind : uint8_t {
    kUnresolved = 0,       // no known module covers the frame
    kCaptureEngine,        // CE's own hook wrapper
    kTerminationPlumbing,  // a Windows layer carrying the request
    kPrimaryModule,        // the process's own executable
    kOtherModule,          // any other loaded module
};

// Frames are innermost-first, the order RtlCaptureStackBackTrace returns them
// in. The first frame that is neither CE's own hook nor a carrying layer is the
// module that asked for the termination. An unresolvable frame stops the walk:
// the request cannot be attributed past it, and kUnknown never suppresses.
inline TerminationOrigin ResolveTerminationOriginFromFrames(const TerminationFrameKind* frames, size_t frameCount,
                                                            size_t* requesterFrameIndex = nullptr) {
    if (requesterFrameIndex) {
        *requesterFrameIndex = frameCount;
    }
    if (!frames) {
        return TerminationOrigin::kUnknown;
    }

    for (size_t i = 0; i < frameCount; ++i) {
        if (frames[i] == TerminationFrameKind::kCaptureEngine ||
            frames[i] == TerminationFrameKind::kTerminationPlumbing) {
            continue;
        }
        if (frames[i] == TerminationFrameKind::kUnresolved) {
            return TerminationOrigin::kUnknown;
        }
        if (requesterFrameIndex) {
            *requesterFrameIndex = i;
        }
        return frames[i] == TerminationFrameKind::kPrimaryModule ? TerminationOrigin::kPrimaryModule
                                                                 : TerminationOrigin::kLoadedModule;
    }
    return TerminationOrigin::kUnknown;
}

// A termination that follows a fault the vectored handler only recorded (see
// ClassifyFirstChanceException): the terminating thread is still inside that
// exception's dispatch - an unhandled-exception filter or a crash reporter's
// __except filter calling TerminateProcess - or it recorded a hardware fault
// that no handler resolved by continuing. A zero exit code is a clean quit
// whatever preceded it (games leave catch blocks with exit(0)), so it never
// qualifies.
inline bool IsTerminationFollowingUnresolvedFault(DWORD exitCode, bool insideExceptionDispatch,
                                                  bool unresolvedFaultPending) {
    return exitCode != 0 && (insideExceptionDispatch || unresolvedFaultPending);
}

inline bool ShouldCapturePreTerminationDump(bool targetIsCurrentProcess, DWORD exitCode, bool alreadyAttempted,
                                            bool frameGenerationRuntimeActiveOrRecent = false,
                                            TerminationOrigin origin = TerminationOrigin::kUnknown,
                                            bool terminationFollowsUnresolvedFault = false) {
    // STATUS_PROCESS_IS_TERMINATING is the runtime's own "the process is
    // already exiting" sentinel; it never represents a genuine abnormal exit,
    // so it must also skip the active-FG fallback below.
    if (!targetIsCurrentProcess || alreadyAttempted || exitCode == kExternalDumpStormTerminationExitCode ||
        exitCode == kProcessIsTerminatingExitCode) {
        return false;
    }
    if (IsCrashLikeProcessExitCode(exitCode) || terminationFollowsUnresolvedFault) {
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

// NTSTATUS "customer" bit: set on every code an application defines for its
// own RaiseException (C++ runtimes, .NET 0xE0434352, LuaJIT 0xE24C4Axx, ...),
// clear on faults the system raises.
inline constexpr bool IsApplicationDefinedExceptionCode(DWORD code) {
    return (code & 0x20000000UL) != 0;
}

// Codes that end the thread whatever handlers exist - dumping them first is the
// only chance, and no runtime raises them to steer its own control flow.
inline constexpr bool IsInherentlyFatalExceptionCode(DWORD code) {
    switch (code) {
        case static_cast<DWORD>(EXCEPTION_STACK_OVERFLOW):
        case kFailFastExceptionExitCode:  // STATUS_STACK_BUFFER_OVERRUN
        case 0xC0000374UL:                // STATUS_HEAP_CORRUPTION
        case 0xC000041DUL:                // STATUS_FATAL_USER_CALLBACK_EXCEPTION
        case 0xC0000602UL:                // STATUS_FAIL_FAST_EXCEPTION
        case 0xC0000420UL:                // STATUS_ASSERTION_FAILURE
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
//
// Error severity alone is not enough either. Managed and JIT runtimes handle
// hardware faults as ordinary control flow - Mono (Unity) and the JVM turn
// access violations into null-reference exceptions and safepoint polls,
// emulators map guest memory through them, LuaJIT and .NET raise error-severity
// codes for every script/managed exception. Each of those was dumped at first
// chance: a multi-second in-process stall, the process's single dump spent on
// a non-crash, and three crash.log writes per exception after that. So a
// first-chance fault is only RECORDED (no I/O, no allocation). It becomes a
// dump when the process actually dies of it: the unhandled filter re-enters
// with forceDump, and the pre-termination hooks dump a termination that
// follows an unresolved fault (IsTerminationFollowingUnresolvedFault) with the
// recorded context.
enum class FirstChanceAction : uint8_t {
    kIgnore,
    kRecordFault,      // remember the context; dump only if the process dies of it
    kDumpNow,          // inherently fatal, or the unhandled filter asked for it
    kQuickAssertDump,  // UE5 ensure(): the small synchronous assert dump
};

inline FirstChanceAction ClassifyFirstChanceException(DWORD code, bool forceDump, bool debuggerPresent) {
    if (forceDump) {
        return FirstChanceAction::kDumpNow;
    }
    switch (code) {
        case 0x406D1388UL:  // thread naming (VS debugger)
        case 0x40010006UL:  // OutputDebugString
        case 0x4001000AUL:  // OutputDebugStringW
        case 0x4000001FUL:  // WoW64 breakpoint
        case 0xE06D7363UL:  // MSVC C++ EH - an unhandled one reaches the top-level filter
        case 0x20474343UL:  // GCC/clang C++ EH (" GCC")
            return FirstChanceAction::kIgnore;
        case kUe5EnsureExceptionCode:
            return FirstChanceAction::kQuickAssertDump;
        case static_cast<DWORD>(EXCEPTION_BREAKPOINT):
            // A first-chance STATUS_BREAKPOINT is recorded first, exactly like a
            // hardware fault. Most are NOT escaped asserts: anti-cheat integrity
            // int3s and another hooking engine's patch races are handled by their
            // raiser. Dumping one immediately cost a full dump stall AND latched
            // the process's one crash dump (g_DumpSuccessfullyWritten), so the
            // real crash that followed a handled int3 got no dump at all - and a
            // "first breakpoint only" budget lands on exactly those handled int3s.
            // An escaped breakpoint still produces its dump: the unhandled filter
            // re-enters with forceDump, a termination inside its dispatch dumps
            // with the recorded context (IsTerminationFollowingUnresolvedFault,
            // kBreakpointExceptionExitCode is crash-like), and a route that
            // bypasses every in-process hook is the Windows Error Reporting
            // LocalDumps capture CE adopts (wer_dump_adoption.h).
            if (debuggerPresent) {
                return FirstChanceAction::kIgnore;
            }
            return FirstChanceAction::kRecordFault;
        default:
            break;
    }
    if (IsInherentlyFatalExceptionCode(code)) {
        return FirstChanceAction::kDumpNow;
    }
    if (IsErrorSeverityExceptionCode(code) && !IsApplicationDefinedExceptionCode(code)) {
        return FirstChanceAction::kRecordFault;
    }
    return FirstChanceAction::kIgnore;
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

// ---------------------------------------------------------------------------
// Terminations no in-process handler can ever see, and the Windows Error
// Reporting dump that is the only remaining record of them.
// ---------------------------------------------------------------------------
//
// `__fastfail(code)` compiles to `int 29h`. The kernel builds the exception
// record with FirstChance = FALSE, so user-mode dispatch is skipped entirely:
// no vectored handler, no SEH frame and no unhandled-exception filter runs. CE
// installs all three and none of them can fire. The process is simply gone with
// STATUS_STACK_BUFFER_OVERRUN (0xC0000409) as its exit code, whatever subcode
// raised it - a /GS cookie check, a CFG indirect-call check, or the UCRT's
// abort() on an unhandled C++ exception (`FAST_FAIL_FATAL_APP_EXIT`).
//
// Witcher 3 + NVIDIA Smooth Motion, session 20260919_154534: NvPresent64's
// std::terminate -> abort() ended the game and CE's session directory held no
// dump at all, while WER had written a complete one elsewhere.
inline bool IsInProcessHandlerBypassingExitCode(DWORD exitCode) {
    return exitCode == kFailFastExceptionExitCode;
}

// One short phrase naming the exit-code class, so the injector's exit line says
// why no CE dump exists instead of leaving the reader to look up the code.
inline const char* DescribeProcessExitCodeClass(DWORD exitCode) {
    if (IsInProcessHandlerBypassingExitCode(exitCode)) {
        return "__fastfail/STATUS_STACK_BUFFER_OVERRUN - bypasses VEH, SEH and the unhandled filter, so no "
               "in-process CE handler can run";
    }
    if (exitCode == kBreakpointExceptionExitCode) {
        return "STATUS_BREAKPOINT";
    }
    if (exitCode == EXCEPTION_ACCESS_VIOLATION) {
        return "STATUS_ACCESS_VIOLATION";
    }
    if (exitCode == EXCEPTION_STACK_OVERFLOW) {
        return "STATUS_STACK_OVERFLOW";
    }
    if (exitCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
        return "STATUS_ILLEGAL_INSTRUCTION";
    }
    if (IsCrashLikeProcessExitCode(exitCode)) {
        return "NTSTATUS error-severity exit";
    }
    return "normal exit";
}

// Default WER local-dump store, relative to %LOCALAPPDATA%. WER names the file
// after the image and the pid, and that pair is what lets CE claim exactly the
// dump belonging to the process it was tracking.
inline constexpr const char* kWerLocalDumpsDefaultRelativeDir = "CrashDumps";
inline constexpr const char* kAdoptedWerCrashDumpPrefix = "crash_wer_";
// WerFault is started after the target is already gone and writes a full dump
// of a multi-gigabyte game, which takes seconds. The injector polls, so the
// window is a deadline rather than a wait: CE re-checks on later poll ticks and
// gives up once it expires.
inline constexpr uint64_t kWerDumpAdoptionWindowMs = 60'000;

inline std::string BuildWerLocalDumpFileName(const char* imageFileName, DWORD processId) {
    const char* baseName = GetPathFileName(imageFileName);
    if (!baseName || baseName[0] == '\0') {
        return {};
    }
    char buffer[MAX_PATH] = {};
    snprintf(buffer, sizeof(buffer), "%s.%lu.dmp", baseName, processId);
    return buffer;
}

inline std::string BuildAdoptedWerCrashDumpFileName(const char* imageFileName, DWORD processId) {
    const char* baseName = GetPathFileName(imageFileName);
    if (!baseName || baseName[0] == '\0') {
        baseName = "process";
    }
    char buffer[MAX_PATH] = {};
    snprintf(buffer, sizeof(buffer), "%s%s_pid%lu.dmp", kAdoptedWerCrashDumpPrefix, baseName, processId);
    return buffer;
}

// CE only claims a WER dump for an exit it would have wanted a dump for and did
// not produce one itself. A clean exit leaves WER's store alone, and a session
// that already holds CE's own dump keeps that one as the authoritative record.
inline bool ShouldAdoptWerDumpForTrackedProcessExit(DWORD exitCode, bool sessionDumpAlreadyPresent) {
    return IsCrashLikeProcessExitCode(exitCode) && !sessionDumpAlreadyPresent;
}

inline bool HasWerDumpAdoptionWindowExpired(uint64_t firstAttemptMs, uint64_t nowMs) {
    return nowMs >= firstAttemptMs && (nowMs - firstAttemptMs) > kWerDumpAdoptionWindowMs;
}

// ---------------------------------------------------------------------------
// WER LocalDumps registration
// ---------------------------------------------------------------------------
//
// LocalDumps is read from HKEY_LOCAL_MACHINE only. CE used to write the same
// values under HKEY_CURRENT_USER as a "last resort" for exactly the fail-fast
// case above; WER never read them. Session 20260919_154534 proves it directly:
// the HKCU key for witcher3.exe named the session directory and WER still wrote
// to %LOCALAPPDATA%\CrashDumps. The writes were inert and left one stale subkey
// per game, each embedding the user's own paths.
inline constexpr const wchar_t* kWerLocalDumpsKeyPath =
    L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps";

// A subkey CE wrote is recognizable by its DumpFolder naming a CE session
// directory. Anything else under LocalDumps belongs to another product and must
// be left untouched.
inline bool IsCaptureEngineWrittenLocalDumpsSubkey(const char* dumpFolderValue, const char* captureEngineLogsRoot) {
    if (!dumpFolderValue || dumpFolderValue[0] == '\0' || !captureEngineLogsRoot || captureEngineLogsRoot[0] == '\0') {
        return false;
    }
    return ContainsAsciiInsensitive(dumpFolderValue, captureEngineLogsRoot);
}

}  // namespace ce::crash_dump_policy
