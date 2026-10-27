#include "captureengine/diagnostics/logger_service.h"

#include <windows.h>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>

#include "captureengine/diagnostics/logger_service_policy.h"
#include "captureengine/elevation/service_lifetime_wait.h"
#include "captureengine/roles/role_entries.h"
#include "common/ipc/role_context.h"
#include "common/ipc/shared_defs.h"
#include "common/logging/log_meter.h"
#include "common/logging/log_record.h"
#include "common/logging/log_ring.h"
#include "common/logging/logging.h"

namespace {

struct LoggerSession {
    LoggerRing ring;
    uint32_t lastReadIndex;
    uint32_t lastOverflowCount;
    std::string logsDirectory;
};

}  // namespace

namespace logger_service {

int RunLoggerWithRings(ce::role::RoleContext& context, const AppConfig& config, LoggerRingSource& rings) {
    if (!IsAnyLoggingEnabled(config.logLevel)) {
        return 0;
    }
    // A thread-hosted role writes its lines to its own sink (DR-40); a process role has none and writes the
    // process log, as it always did.
    ce::log::SinkScope sinkScope(context.logSink);
    if (!context.ownsProcess && !context.stopEvent) {
        LogError("[Logger] Thread-hosted logger has no stop event; not started");
        return 1;
    }
    LogInfo("[Logger] Dedicated logging service started");
    const HANDLE recordsReady = ce::logging::LogRingReadyEvent();
    if (recordsReady) {
        LogInfo("[Logger] Event-driven ring drain enabled; polling remains a discovery/failure fallback");
    } else {
        LogWarn("[Logger] Cannot create ring-ready event (error=%lu); using bounded polling", GetLastError());
    }

    // Process mode also watches its controller: the shutdown event keyed to the controller PID, and the
    // controller process itself. A hard controller exit never sets the event, and an orphaned logger would
    // keep draining the shared rings against the next controller's.
    HANDLE shutdownEvent = nullptr;
    HANDLE controllerProcess = nullptr;
    if (context.ownsProcess && context.controllerPid != 0) {
        wchar_t eventName[64];
        GenerateShutdownEventName(eventName, 64, context.controllerPid);
        shutdownEvent = CreateEventW(NULL, TRUE, FALSE, eventName);
        if (!shutdownEvent) {
            LogWarn("[Logger] Cannot create the controller shutdown event (error=%lu)", GetLastError());
        }
        controllerProcess = OpenProcess(SYNCHRONIZE, FALSE, context.controllerPid);
        if (!controllerProcess) {
            // The controller is already gone (or cannot be watched): never run orphaned.
            LogError("[Logger] Cannot monitor controller PID %u lifetime (error=%lu); exiting", context.controllerPid,
                     GetLastError());
            if (shutdownEvent)
                CloseHandle(shutdownEvent);
            return 1;
        }
    }

    std::map<uint32_t, LoggerSession> sessions;
    std::map<std::string, HANDLE> openFiles;
    ce::log_meter::ChangeGate fileFailureLog(5000);
    uint64_t failedFileRecords = 0;
    const std::string fallbackDirectory = context.logsRoot;

    bool finalDrain = false;
    bool loggedWaitFailure = false;
    while (true) {
        // Attach the ring of the injected process that discovery names, once per PID.
        LoggerDiscovery discovery;
        if (rings.ReadDiscovery(discovery) && sessions.find(discovery.injectPid) == sessions.end()) {
            const uint32_t pid = discovery.injectPid;
            const std::string sessionLogsDirectory = logger_service_policy::SelectSessionLogsDirectory(
                discovery.logsPath, sizeof(discovery.logsPath), fallbackDirectory);
            LoggerRing ring = rings.OpenRing(pid);
            if (ring.shm && ValidateSharedMemory(ring.shm)) {
                if (!CreateDirectoryA(sessionLogsDirectory.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                    LogError("[Logger] Failed to create session logs directory '%s': %lu",
                             sessionLogsDirectory.c_str(), GetLastError());
                }
                LogInfo("[Logger] Discovered new session: PID %u, logs=%s, ABI=0x%08X", pid,
                        sessionLogsDirectory.c_str(), SHARED_MEMORY_ABI_SIGNATURE);
                // Resume the released cursor. Skipping to write would discard
                // accepted records and leave their committed flags stale on reuse.
                const uint32_t currentReadIdx = ring.shm->logs.readIndex.load(std::memory_order_acquire);
                sessions[pid] = {ring, currentReadIdx, ring.shm->logs.overflowCount.load(std::memory_order_relaxed),
                                 sessionLogsDirectory};
                LogInfo("[Logger] Session PID %u resumed at readIndex=%u pending=%u", pid, currentReadIdx,
                        ring.shm->logs.writeIndex.load(std::memory_order_acquire) - currentReadIdx);
            } else {
                if (ring.shm) {
                    LogError("[Logger] Rejected incompatible shared memory for PID %u "
                             "(version=%u size=%u abi=0x%08X)",
                             pid, ring.shm->GetVersion(), ring.shm->structSize.load(std::memory_order_acquire),
                             ring.shm->abiSignature.load(std::memory_order_acquire));
                }
                rings.CloseRing(ring);
            }
        }

        // Poll all active sessions for new logs.
        bool hasPendingLogs = false;
        bool hasActiveSource = false;
        // A ring that was full when we looked at it means the producer had
        // already started discarding into its file fallback. Sleeping the normal
        // 100 ms after draining it just guarantees the next burst overflows too,
        // so keep draining until the producer is ahead of us again.
        bool sawSaturatedRing = false;
        for (auto it = sessions.begin(); it != sessions.end();) {
            LoggerSession& s = it->second;
            SharedMemoryLayout* const shm = s.ring.shm;
            if (shm->GetSourcePid() != 0) {
                hasActiveSource = true;
            }

            uint32_t writeIdx = shm->logs.writeIndex.load(std::memory_order_acquire);
            uint32_t readIdx = s.lastReadIndex;  // Use session's tracked index, not SHM's stale value
            if (readIdx != writeIdx) {
                hasPendingLogs = true;
            }

            const bool ringWasSaturated =
                static_cast<uint32_t>(writeIdx - readIdx) >= SharedMemoryLayout::LogBuffer::SLOT_COUNT;
            if (ringWasSaturated) {
                sawSaturatedRing = true;
            }

            char record[SharedMemoryLayout::LogBuffer::SLOT_SIZE] = {};
            while (readIdx != writeIdx && ce::logging::TryPopLogRecord(shm->logs, readIdx, record)) {
                const char* entry = record;

                // Format: [FILENAME] Message
                std::string filename = "hook_debug.log";
                const char* message = entry;

                if (entry[0] == '[') {
                    const char* endBracket = strchr(entry, ']');
                    if (endBracket) {
                        size_t fnLen = endBracket - entry - 1;
                        const std::string_view candidate(entry + 1, fnLen);
                        if (logger_service_policy::IsSafeLogFilename(candidate)) {
                            filename.assign(candidate);
                            message = endBracket + 1;
                            if (*message == ' ')
                                ++message;
                        }
                    }
                }

                if (message[0] != '\0') {
                    char fullPath[MAX_PATH];
                    const int pathLength = snprintf(fullPath, sizeof(fullPath), "%s\\%s", s.logsDirectory.c_str(),
                                                    filename.c_str());
                    if (pathLength <= 0 || pathLength >= static_cast<int>(sizeof(fullPath))) {
                        ++failedFileRecords;
                        const auto verdict = fileFailureLog.Observe(
                            ce::log_meter::FieldKey(it->first, static_cast<DWORD>(ERROR_FILENAME_EXCED_RANGE)),
                            GetTickCount64());
                        if (verdict.log) {
                            LogError("[Logger] Log path too long: pid=%u file=%s lostTotal=%llu suppressed=%llu",
                                     it->first, filename.c_str(), static_cast<unsigned long long>(failedFileRecords),
                                     static_cast<unsigned long long>(verdict.suppressed));
                        }
                        continue;
                    }

                    HANDLE hFile = INVALID_HANDLE_VALUE;
                    auto itFile = openFiles.find(fullPath);
                    if (itFile != openFiles.end()) {
                        hFile = itFile->second;
                    } else {
                        hFile = CreateFileA(fullPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                        if (hFile != INVALID_HANDLE_VALUE) {
                            openFiles[fullPath] = hFile;
                        }
                    }

                    if (hFile == INVALID_HANDLE_VALUE || !ce::logging::AppendLogRecord(hFile, message)) {
                        const DWORD error = GetLastError();
                        ++failedFileRecords;
                        const auto verdict = fileFailureLog.Observe(
                            ce::log_meter::FieldKey(it->first, error), GetTickCount64());
                        if (verdict.log) {
                            LogWarn("[Logger] Log record write failed: pid=%u file=%s error=%lu lostTotal=%llu "
                                    "suppressed=%llu; next record retries the file",
                                    it->first, filename.c_str(), error,
                                    static_cast<unsigned long long>(failedFileRecords),
                                    static_cast<unsigned long long>(verdict.suppressed));
                        }
                        if (hFile != INVALID_HANDLE_VALUE) {
                            CloseHandle(hFile);
                            openFiles.erase(fullPath);
                        }
                    }
                }
            }

            // `overflowCount` existed from the start and nothing ever read it, so
            // ring overflow was invisible even though the producer counted it.
            const uint32_t overflow = shm->logs.overflowCount.load(std::memory_order_relaxed);
            if (overflow != s.lastOverflowCount) {
                LogInfo("[Logger] PID %u: log ring overflowed %u time(s) (total %u); records used direct append "
                        "fallback; actual file failures have separate loss diagnostics",
                        it->first, overflow - s.lastOverflowCount, overflow);
                s.lastOverflowCount = overflow;
            }

            // Update session's tracked read index
            s.lastReadIndex = readIdx;
            // TryPopLogRecord already releases each slot before the file I/O.

            // Optional: Remove dead sessions (maybe if they haven't updated in X
            // seconds?) For now, keep them.
            ++it;
        }

        if (finalDrain)
            break;
        const DWORD waitMs = static_cast<DWORD>(
            logger_service_policy::SelectLogDrainWaitMs(sawSaturatedRing, hasPendingLogs, hasActiveSource));
        switch (ce::service_lifetime::WaitForServiceLifetime(context.stopEvent, shutdownEvent, controllerProcess,
                                                             waitMs, recordsReady)) {
            case ce::service_lifetime::WaitOutcome::kTimeout:
                break;
            case ce::service_lifetime::WaitOutcome::kStopRequested:
                // A process's console stop has always been silent; the runtime's stop is logged.
                if (!context.ownsProcess)
                    LogInfo("[Logger] Stop requested by the runtime; draining the final committed snapshot");
                finalDrain = true;
                break;
            case ce::service_lifetime::WaitOutcome::kShutdownSignaled:
                LogInfo("[Logger] Shutdown signal received; draining the final committed snapshot");
                finalDrain = true;
                break;
            case ce::service_lifetime::WaitOutcome::kControllerExited:
                LogInfo("[Logger] Controller exited without a shutdown signal; exiting so the next controller's "
                        "logger owns the log rings after a final committed drain");
                finalDrain = true;
                break;
            case ce::service_lifetime::WaitOutcome::kFailed:
                if (!loggedWaitFailure) {
                    LogWarn("[Logger] Lifetime wait failed (error=%lu); falling back to bounded sleeps",
                            GetLastError());
                    loggedWaitFailure = true;
                }
                Sleep(waitMs);
                break;
        }
    }

    if (shutdownEvent)
        CloseHandle(shutdownEvent);
    if (controllerProcess)
        CloseHandle(controllerProcess);

    // Close all cached file handles to prevent handle leak
    for (auto& [path, hFile] : openFiles) {
        if (hFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(hFile);
            CloseHandle(hFile);
        }
    }
    openFiles.clear();

    for (auto& [pid, session] : sessions) {
        (void)pid;
        rings.CloseRing(session.ring);
    }
    sessions.clear();

    return 0;
}

}  // namespace logger_service

namespace ce::role {
int RunLoggerRole(RoleContext& context, const AppConfig& config) {
    SharedLoggerRings rings;
    return logger_service::RunLoggerWithRings(context, config, rings);
}
}  // namespace ce::role
