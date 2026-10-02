#pragma once

// WER LocalDumps adoption and registration policy: how CE claims the dump WerFault wrote for a
// tracked process that died without producing one. Split out of crash_dump_policy.h (which includes
// it, so existing includers see the same names) to keep that header under the file-size ceiling.

#include "crash_dump_policy.h"

namespace ce::crash_dump_policy {

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
