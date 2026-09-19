#include "injection_internal.h"

// What CE does when a tracked, injected target disappears: name the exit-code
// class in the log, and claim the Windows Error Reporting dump for the crashes
// no in-process CE handler could ever have recorded.

// A tracked target is gone. Name the exit-code class in the log - "exit=
// 0xC0000409" on its own leaves the reader to look up why CE wrote nothing -
// and queue the WER dump claim when the exit was crash-like and CE produced no
// dump of its own.
void InjectionManager::NoteTrackedProcessExitLocked(DWORD pid, const std::string& name, DWORD exitCode) {
    const char* exitClass = ce::crash_dump_policy::DescribeProcessExitCodeClass(exitCode);
    const std::string sessionDumpDir = GetCrashDumpDirectory();
    const bool sessionDumpPresent = ce::wer_dump_adoption::SessionDirectoryHasDumpForProcess(sessionDumpDir, pid);

    LogInfo("[Inject] Tracked injected process exited: %s (PID: %lu, exit=0x%08lX, %s); CE session dump present=%d",
            name.c_str(), (unsigned long)pid, (unsigned long)exitCode, exitClass, sessionDumpPresent ? 1 : 0);

    if (!ce::crash_dump_policy::ShouldAdoptWerDumpForTrackedProcessExit(exitCode, sessionDumpPresent)) {
        return;
    }

    pendingWerDumpAdoptions.push_back({pid, name, exitCode, GetTickCount64()});
    LogInfo(
        "[CrashDump] %s (PID: %lu) ended without a CE dump; watching the Windows Error Reporting store for its dump",
        name.c_str(), (unsigned long)pid);
}

// Poll-driven, never a wait: each Update() tick re-checks the WER store for a
// dump WerFault has finished writing, and drops the entry once it is claimed or
// the adoption window expires.
void InjectionManager::ServicePendingWerDumpAdoptionsLocked(uint64_t nowMs) {
    if (pendingWerDumpAdoptions.empty()) {
        return;
    }

    const std::string sessionDumpDir = GetCrashDumpDirectory();
    pendingWerDumpAdoptions.erase(
        std::remove_if(
            pendingWerDumpAdoptions.begin(), pendingWerDumpAdoptions.end(),
            [&](const PendingWerDumpAdoption& pending) {
                std::string adoptedPath;
                std::string sourcePath;
                uint64_t sizeBytes = 0;
                if (ce::wer_dump_adoption::TryAdoptLocalDump(pending.name.c_str(), pending.pid, sessionDumpDir,
                                                             &adoptedPath, &sourcePath, &sizeBytes)) {
                    LogInfo("[CrashDump] Adopted the WER dump for %s (PID: %lu, %llu bytes) from %s into %s",
                            pending.name.c_str(), (unsigned long)pending.pid, (unsigned long long)sizeBytes,
                            sourcePath.c_str(), adoptedPath.c_str());
                    return true;
                }

                if (!ce::crash_dump_policy::HasWerDumpAdoptionWindowExpired(pending.firstAttemptMs, nowMs)) {
                    return false;
                }

                LogWarn(
                    "[CrashDump] No dump for %s (PID: %lu, exit=0x%08lX) in any WER local-dump store. %s Enable WER "
                    "local dumps once from an elevated prompt to capture this class: reg add \"HKLM\\SOFTWARE\\"
                    "Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\" /v DumpType /t REG_DWORD /d 2 /f",
                    pending.name.c_str(), (unsigned long)pending.pid, (unsigned long)pending.exitCode,
                    ce::crash_dump_policy::DescribeProcessExitCodeClass(pending.exitCode));
                return true;
            }),
        pendingWerDumpAdoptions.end());
}
