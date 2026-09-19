#pragma once

// Claiming the Windows Error Reporting dump for a process that died where no
// CE handler could run.
//
// A __fastfail termination (exit code 0xC0000409) is dispatched by the kernel
// with FirstChance = FALSE: no vectored handler, no SEH frame and no unhandled
// exception filter executes, so CE's in-process crash handler is structurally
// unable to write a dump. WER still records one, just in its own store under a
// name of its own choosing. These helpers move that file into the CE session
// directory so the session stays the single place a crash is investigated from.

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ce::wer_dump_adoption {

// Directories WER may have written a local dump into, most specific first:
// the per-image DumpFolder, the global DumpFolder, then the default store
// (%LOCALAPPDATA%\CrashDumps). Empty when none can be resolved.
std::vector<std::wstring> ResolveLocalDumpSearchDirectories(const char* imageFileName);

// Looks for `<image>.<pid>.dmp` in those directories and moves the first match
// into `sessionDumpDirectory` under a CE-style name. Returns true only when a
// non-empty dump ended up in the session directory; `outAdoptedPath` then holds
// its full path, `outSourcePath` where it came from and `outSizeBytes` its size.
//
// A dump that is still being written is reported as not-found rather than
// claimed: WerFault runs after the target is already gone and takes seconds for
// a large game, so the caller retries on its own polling schedule.
bool TryAdoptLocalDump(const char* imageFileName, DWORD processId, const std::string& sessionDumpDirectory,
                       std::string* outAdoptedPath, std::string* outSourcePath, uint64_t* outSizeBytes);

// Does the session directory already hold a CE-written dump for this pid? An
// adopted WER dump is only a fallback for the case CE could not cover itself.
bool SessionDirectoryHasDumpForProcess(const std::string& sessionDumpDirectory, DWORD processId);

// Removes the inert HKCU LocalDumps entries older CE builds wrote. Only subkeys
// whose DumpFolder names a directory under `captureEngineLogsRoot` are touched;
// anything another product registered is left alone. Returns how many were
// removed. Safe to call when the key does not exist.
size_t PurgeInertCaptureEngineLocalDumpsRegistration(const std::string& captureEngineLogsRoot);

}  // namespace ce::wer_dump_adoption
