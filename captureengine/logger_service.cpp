#include "logger_service.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "../common/logging.h"
#include "../common/shared_defs.h"

struct Session {
    HANDLE hMap;
    SharedMemoryLayout* shm;
    uint32_t lastReadIndex;
};

int LoggerProcessMain(const AppConfig& config) {
    LogInfo("[Logger] Dedicated logging service started");

    // Parse controller PID from command line for shutdown signaling
    uint32_t controllerPid = 0;
    const char* cmdLine = GetCommandLineA();
    const char* pidArg = strstr(cmdLine, "--parent-pid=");
    if (pidArg)
        controllerPid = (uint32_t)atoi(pidArg + 13);

    // Create/open shutdown event keyed to controller PID
    HANDLE hShutdownEvent = INVALID_HANDLE_VALUE;
    if (controllerPid != 0) {
        wchar_t eventName[64];
        GenerateShutdownEventName(eventName, 64, controllerPid);
        hShutdownEvent = CreateEventW(NULL, TRUE, FALSE, eventName);
    }

    std::map<uint32_t, Session> sessions;
    std::map<std::string, HANDLE> openFiles;
    char logsDir[MAX_PATH];

    // Get logs directory
    GetModuleFileNameA(NULL, logsDir, MAX_PATH);
    char* lastSlash = strrchr(logsDir, '\\');
    if (lastSlash) {
        *lastSlash = '\0';
        char tmpDir[MAX_PATH];
        int written = snprintf(tmpDir, sizeof(tmpDir), "%s\\logs", logsDir);
        if (written > 0 && written < (int)sizeof(tmpDir)) {
            CreateDirectoryA(tmpDir, NULL);
            strncpy(logsDir, tmpDir, sizeof(logsDir) - 1);
            logsDir[sizeof(logsDir) - 1] = '\0';
        }
    }

    while (true) {
        // 1. Discover new sessions
        // We look for CE_SM_XXXXXXXX names in the Local namespace
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
                if (info->magic == DISCOVERY_MAGIC) {
                    uint32_t pid = info->injectPid;
                    if (sessions.find(pid) == sessions.end()) {
                        wchar_t smName[64];
                        GenerateSharedMemName(smName, 64, pid);
                        HANDLE hSM = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, smName);
                        if (hSM) {
                            SharedMemoryLayout* shm = (SharedMemoryLayout*)MapViewOfFile(hSM, FILE_MAP_ALL_ACCESS, 0, 0,
                                                                                         sizeof(SharedMemoryLayout));
                            if (shm) {
                                LogInfo("[Logger] Discovered new session: PID %u", pid);
                                // Initialize readIndex to current writeIndex to skip stale data
                                // from previous sessions. The hook writes early logs directly to
                                // file before IPC connects, so SHM may contain old data.
                                uint32_t currentWriteIdx = shm->logs.writeIndex.load(std::memory_order_acquire);
                                sessions[pid] = {hSM, shm, currentWriteIdx};
                                LogInfo("[Logger] Session PID %u initialized at writeIndex=%u", pid, currentWriteIdx);
                            } else {
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
        for (auto it = sessions.begin(); it != sessions.end();) {
            Session& s = it->second;

            // Check if process is still alive?
            // We can check if shm is still valid or use GetExitCodeProcess if we had
            // the handle. For now, just check if we can still read from it.

            uint32_t writeIdx = s.shm->logs.writeIndex.load(std::memory_order_acquire);
            uint32_t readIdx = s.lastReadIndex;  // Use session's tracked index, not SHM's stale value

            while (readIdx != writeIdx) {
                uint32_t slotIdx = readIdx % SharedMemoryLayout::LogBuffer::SLOT_COUNT;

                // Wait for slot to be committed (producer may still be writing)
                if (!s.shm->logs.committed[slotIdx].load(std::memory_order_acquire)) {
                    break;  // Slot not ready yet, try again next poll
                }

                const char* entry = s.shm->logs.buffer[slotIdx];

                // Format: [FILENAME] Message
                const char* filename = "hook_debug.log";
                const char* message = entry;

                if (entry[0] == '[') {
                    const char* endBracket = strchr(entry, ']');
                    if (endBracket) {
                        char fnBuffer[64];
                        size_t fnLen = endBracket - entry - 1;
                        if (fnLen < 63) {
                            strncpy(fnBuffer, entry + 1, fnLen);
                            fnBuffer[fnLen] = '\0';
                            filename = fnBuffer;
                            message = endBracket + 2;  // Skip "] "
                        }
                    }
                }

                if (message[0] != '\0') {
                    char fullPath[MAX_PATH];
                    snprintf(fullPath, sizeof(fullPath), "%s\\%s", logsDir, filename);

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

        Sleep(100);  // Poll every 100ms

        // Check for shutdown signal
        if (hShutdownEvent != INVALID_HANDLE_VALUE && WaitForSingleObject(hShutdownEvent, 0) == WAIT_OBJECT_0) {
            LogInfo("[Logger] Shutdown signal received, exiting");
            break;
        }
    }

    if (hShutdownEvent != INVALID_HANDLE_VALUE)
        CloseHandle(hShutdownEvent);

    return 0;
}
