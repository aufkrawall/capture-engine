#include "sensor_service.h"
#include <windows.h>
#include <atomic>
#include <map>
#include <vector>
#include "../common/logging.h"
#include "../common/shared_defs.h"
#include "host_metrics.h"  // Reuse existing logic for native sensors
#include "sensor_plugin.h"

struct SensorSession {
    HANDLE hMap;
    SharedMemoryLayout* shm;
    int64_t cachedLuid = 0;  // Cache valid LUID once discovered
};

int SensorProcessMain(const AppConfig& config) {
    LogInfo("[Sensors] Dedicated sensor service started");

    // Handle Windows shutdown/logoff when controller may already be gone
    static std::atomic<bool> g_SensorRunning{true};
    SetConsoleCtrlHandler(
        [](DWORD ctrlType) -> BOOL {
            if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT ||
                ctrlType == CTRL_LOGOFF_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
                g_SensorRunning.store(false, std::memory_order_release);
                return TRUE;
            }
            return FALSE;
        },
        TRUE);

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

    std::map<uint32_t, SensorSession> sessions;
    static bool loggedDiscoveryAttempt = false;

    // In a real plugin-based system, we'd load DLLs here.
    // For now, we reuse the scan_host logic but we will encapsulate it better.

    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    while (g_SensorRunning.load(std::memory_order_acquire)) {
        // 1. Discover new sessions
        HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
        if (hDisc) {
            DiscoveryInfo* info = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
            if (info) {
                if (info->magic == DISCOVERY_MAGIC) {
                    uint32_t pid = info->injectPid;
                    if (!loggedDiscoveryAttempt) {
                        LogInfo("[Sensors] Discovery found inject PID %u", pid);
                        loggedDiscoveryAttempt = true;
                    }
                    if (pid != 0 && sessions.find(pid) == sessions.end()) {
                        wchar_t smName[64];
                        GenerateSharedMemName(smName, 64, pid);
                        HANDLE hSM = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, smName);
                        if (hSM) {
                            SharedMemoryLayout* shm = (SharedMemoryLayout*)MapViewOfFile(hSM, FILE_MAP_ALL_ACCESS, 0, 0,
                                                                                         sizeof(SharedMemoryLayout));
                            if (shm) {
                                LogInfo(
                                    "[Sensors] Discovered new session: Inject PID %u, Game "
                                    "PID %u",
                                    pid, shm->GetSourcePid());
                                sessions[pid] = {hSM, shm};
                            } else {
                                LogError("[Sensors] Failed to map shared memory for PID %u", pid);
                                CloseHandle(hSM);
                            }
                        } else {
                            LogError("[Sensors] Failed to open shared memory for PID %u: %d", pid, GetLastError());
                        }
                    }
                }
                UnmapViewOfFile(info);
            }
            CloseHandle(hDisc);
        } else if (!loggedDiscoveryAttempt) {
            LogInfo("[Sensors] Waiting for discovery shared memory...");
            loggedDiscoveryAttempt = true;
        }

        // 2. Poll metrics for all active sessions
        for (auto it = sessions.begin(); it != sessions.end();) {
            SensorSession& s = it->second;
            uint32_t sourcePid = s.shm->GetSourcePid();

            // No hooked source yet: skip expensive PDH/DXGI work while idle, but
            // keep checking shared memory so metrics come online quickly once a game
            // is attached.
            if (sourcePid == 0) {
                ++it;
                continue;
            }
            // Read LUID from shared memory
            int64_t luid = ((int64_t)s.shm->GetLuidHighPart() << 32) | (uint32_t)s.shm->GetLuidLowPart();

            // Cache valid LUID once discovered (it may reset during game restart)
            if (luid != 0) {
                s.cachedLuid = luid;
            }

            // Use cached LUID if current is 0
            int64_t effectiveLuid = (luid != 0) ? luid : s.cachedLuid;

            if (s.shm->GetDebugLogging() && effectiveLuid != 0) {
                LogInfo("[Sensors] Updating PID %u (Game: %u), LUID: 0x%llX", it->first, sourcePid, effectiveLuid);
            }

            // Update metrics using the existing host_metrics logic
            scan_host::UpdateSystemMetrics(s.shm, sourcePid, effectiveLuid);

            ++it;
        }

        DWORD waitMs = 1000;
        if (hShutdownEvent != INVALID_HANDLE_VALUE) {
            DWORD waitResult = WaitForSingleObject(hShutdownEvent, waitMs);
            if (waitResult == WAIT_OBJECT_0) {
                LogInfo("[Sensors] Shutdown signal received, exiting");
                break;
            }
        } else {
            Sleep(waitMs);
        }
    }

    if (hShutdownEvent != INVALID_HANDLE_VALUE)
        CloseHandle(hShutdownEvent);

    return 0;
}
