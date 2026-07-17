#include "logger_service.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include "../common/logging.h"
#include "../common/shared_defs.h"
#include "../common/strict_integer_parse.h"
#include "logger_service_policy.h"

struct LoggerSession {
    HANDLE hMap;
    SharedMemoryLayout* shm;
    uint32_t lastReadIndex;
    std::string logsDirectory;
};

int LoggerProcessMain(const AppConfig& config) {
    if (!IsAnyLoggingEnabled(config.logLevel)) {
        return 0;
    }
    LogInfo("[Logger] Dedicated logging service started");

    // Handle Windows shutdown/logoff when controller may already be gone
    static std::atomic<bool> g_LoggerRunning{true};
    SetConsoleCtrlHandler(
        [](DWORD ctrlType) -> BOOL {
            if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT ||
                ctrlType == CTRL_LOGOFF_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
                g_LoggerRunning.store(false, std::memory_order_release);
                return TRUE;
            }
            return FALSE;
        },
        TRUE);

    // Parse controller PID from command line for shutdown signaling
    uint32_t controllerPid = 0;
    const char* cmdLine = GetCommandLineA();
    const char* pidArg = strstr(cmdLine, "--parent-pid=");
    if (pidArg) {
        const char* value = pidArg + 13;
        const char* end = value;
        while (*end >= '0' && *end <= '9')
            ++end;
        if ((*end == '\0' || *end == ' ' || *end == '\t') &&
            !ce::TryParseUInt32(std::string_view(value, static_cast<size_t>(end - value)), controllerPid)) {
            controllerPid = 0;
        }
    }

    // Create/open shutdown event keyed to controller PID
    HANDLE hShutdownEvent = INVALID_HANDLE_VALUE;
    if (controllerPid != 0) {
        wchar_t eventName[64];
        GenerateShutdownEventName(eventName, 64, controllerPid);
        hShutdownEvent = CreateEventW(NULL, TRUE, FALSE, eventName);
    }

    std::map<uint32_t, LoggerSession> sessions;
    std::map<std::string, HANDLE> openFiles;
    char fallbackLogsDir[MAX_PATH]{};

    // Get logs directory
    GetModuleFileNameA(NULL, fallbackLogsDir, MAX_PATH);
    char* lastSlash = strrchr(fallbackLogsDir, '\\');
    if (lastSlash) {
        *lastSlash = '\0';
        char tmpDir[MAX_PATH];
        int written = snprintf(tmpDir, sizeof(tmpDir), "%s\\logs", fallbackLogsDir);
        if (written > 0 && written < (int)sizeof(tmpDir)) {
            CreateDirectoryA(tmpDir, NULL);
            strncpy(fallbackLogsDir, tmpDir, sizeof(fallbackLogsDir) - 1);
            fallbackLogsDir[sizeof(fallbackLogsDir) - 1] = '\0';
        }
    }
    const std::string fallbackDirectory = fallbackLogsDir;

    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    while (g_LoggerRunning.load(std::memory_order_acquire)) {
        // Since we don't have a global list of PIDs, we might need a better way.
        // DiscoveryInfo helps, but it only points to one PID.

        // Actually, the CaptureEngine (Controller) knows all PIDs it injects into?
        // No, the Logger should be autonomous.
        // We can use the DiscoveryInfo to find the "Main" PID, but there might be
        // multiple game processes.

        // Let's use a simpler approach for now: The Controller could pass the PIDs,
        // OR we can scan all active processes and try to open their CE_SM shared
        // memory.

        // For now, let's just try to open the Discovery shared memory to get the
        // Inject PID, and also look for other potential game PIDs if we can.

        // Better: Scan all handles or just assume PIDs are within a range?
        // No, let's use the DiscoveryInfo magic.

        HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
        if (hDisc) {
            DiscoveryInfo* info = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
            if (info) {
                if (ValidateDiscoveryInfo(info)) {
                    uint32_t pid = info->GetInjectPid();
                    if (sessions.find(pid) == sessions.end()) {
                        const std::string sessionLogsDirectory = logger_service_policy::SelectSessionLogsDirectory(
                            info->logsPath, sizeof(info->logsPath), fallbackDirectory);
                        wchar_t smName[64];
                        GenerateSharedMemName(smName, 64, pid);
                        HANDLE hSM = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, smName);
                        if (hSM) {
                            SharedMemoryLayout* shm = (SharedMemoryLayout*)MapViewOfFile(hSM, FILE_MAP_ALL_ACCESS, 0, 0,
                                                                                         sizeof(SharedMemoryLayout));
                            if (shm && ValidateSharedMemory(shm)) {
                                if (!CreateDirectoryA(sessionLogsDirectory.c_str(), NULL) &&
                                    GetLastError() != ERROR_ALREADY_EXISTS) {
                                    LogError("[Logger] Failed to create session logs directory '%s': %lu",
                                             sessionLogsDirectory.c_str(), GetLastError());
                                }
                                LogInfo("[Logger] Discovered new session: PID %u, logs=%s, ABI=0x%08X", pid,
                                        sessionLogsDirectory.c_str(), SHARED_MEMORY_ABI_SIGNATURE);
                                // Initialize readIndex to current writeIndex to skip stale data
                                // from previous sessions. The hook writes early logs directly to
                                // file before IPC connects, so SHM may contain old data.
                                uint32_t currentWriteIdx = shm->logs.writeIndex.load(std::memory_order_acquire);
                                sessions[pid] = {hSM, shm, currentWriteIdx, sessionLogsDirectory};
                                LogInfo("[Logger] Session PID %u initialized at writeIndex=%u", pid, currentWriteIdx);
                            } else {
                                if (shm) {
                                    LogError(
                                        "[Logger] Rejected incompatible shared memory for PID %u "
                                        "(version=%u size=%u abi=0x%08X)",
                                        pid, shm->GetVersion(), shm->structSize.load(std::memory_order_acquire),
                                        shm->abiSignature.load(std::memory_order_acquire));
                                    UnmapViewOfFile(shm);
                                }
                                CloseHandle(hSM);
                            }
                        }
                    }
                }
                UnmapViewOfFile(info);
            }
            CloseHandle(hDisc);
        }

        // 2. Poll all active sessions for new logs
        bool hasPendingLogs = false;
        bool hasActiveSource = false;
        for (auto it = sessions.begin(); it != sessions.end();) {
            LoggerSession& s = it->second;
            if (s.shm->GetSourcePid() != 0) {
                hasActiveSource = true;
            }

            // Check if process is still alive?
            // We can check if shm is still valid or use GetExitCodeProcess if we had
            // the handle. For now, just check if we can still read from it.

            uint32_t writeIdx = s.shm->logs.writeIndex.load(std::memory_order_acquire);
            uint32_t readIdx = s.lastReadIndex;  // Use session's tracked index, not SHM's stale value
            if (readIdx != writeIdx) {
                hasPendingLogs = true;
            }

            while (readIdx != writeIdx) {
                uint32_t slotIdx = readIdx % SharedMemoryLayout::LogBuffer::SLOT_COUNT;

                // Wait for slot to be committed (producer may still be writing)
                if (!s.shm->logs.committed[slotIdx].load(std::memory_order_acquire)) {
                    break;  // Slot not ready yet, try again next poll
                }

                const char* entry = s.shm->logs.buffer[slotIdx];

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
                        LogError("[Logger] Log path too long for PID %u: directory=%s file=%s", it->first,
                                 s.logsDirectory.c_str(), filename.c_str());
                        s.shm->logs.committed[slotIdx].store(0, std::memory_order_release);
                        ++readIdx;
                        continue;
                    }

                    HANDLE hFile = INVALID_HANDLE_VALUE;
                    auto itFile = openFiles.find(fullPath);
                    if (itFile != openFiles.end()) {
                        hFile = itFile->second;
                    } else {
                        hFile = CreateFileA(fullPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                        openFiles[fullPath] = hFile;
                    }

                    if (hFile != INVALID_HANDLE_VALUE) {
                        DWORD written;
                        WriteFile(hFile, message, (DWORD)strlen(message), &written, NULL);
                        WriteFile(hFile, "\r\n", 2, &written, NULL);
                    }
                }

                // Clear committed flag so producer can reuse this slot
                s.shm->logs.committed[slotIdx].store(0, std::memory_order_release);
                readIdx++;
            }

            // Update session's tracked read index
            s.lastReadIndex = readIdx;
            // Also update SHM for cross-process visibility (optional, for debugging)
            s.shm->logs.readIndex.store(readIdx, std::memory_order_release);

            // Optional: Remove dead sessions (maybe if they haven't updated in X
            // seconds?) For now, keep them.
            ++it;
        }

        DWORD waitMs = hasPendingLogs ? 100 : (hasActiveSource ? 250 : 1000);
        if (hShutdownEvent != INVALID_HANDLE_VALUE) {
            DWORD waitResult = WaitForSingleObject(hShutdownEvent, waitMs);
            if (waitResult == WAIT_OBJECT_0) {
                LogInfo("[Logger] Shutdown signal received, exiting");
                break;
            }
        } else {
            Sleep(waitMs);
        }
    }

    if (hShutdownEvent != INVALID_HANDLE_VALUE)
        CloseHandle(hShutdownEvent);

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
        if (session.shm)
            UnmapViewOfFile(session.shm);
        if (session.hMap)
            CloseHandle(session.hMap);
    }
    sessions.clear();

    return 0;
}
