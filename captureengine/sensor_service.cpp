#include "sensor_service.h"
#include "sensor_plugin.h"
#include "../common/shared_defs.h"
#include "../common/logging.h"
#include "host_metrics.h" // Reuse existing logic for native sensors
#include <windows.h>
#include <vector>
#include <map>

struct Session {
    HANDLE hMap;
    SharedMemoryLayout* shm;
};

int SensorProcessMain(const AppConfig& config) {
    LogInfo("[Sensors] Dedicated sensor service started");
    
    std::map<uint32_t, Session> sessions;
    
    // In a real plugin-based system, we'd load DLLs here.
    // For now, we reuse the scan_host logic but we will encapsulate it better.
    
    while (true) {
        // 1. Discover new sessions
        HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
        if (hDisc) {
            DiscoveryInfo* info = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
            if (info) {
                if (info->magic == DISCOVERY_MAGIC) {
                    uint32_t pid = info->injectPid;
                    if (pid != 0 && sessions.find(pid) == sessions.end()) {
                        wchar_t smName[64];
                        GenerateSharedMemName(smName, 64, pid);
                        HANDLE hSM = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, smName);
                        if (hSM) {
                            SharedMemoryLayout* shm = (SharedMemoryLayout*)MapViewOfFile(hSM, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemoryLayout));
                            if (shm) {
                                LogInfo("[Sensors] Discovered new session: Inject PID %u, Game PID %u", pid, shm->sourcePid);
                                sessions[pid] = { hSM, shm };
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

        // 2. Poll metrics for all active sessions
        for (auto it = sessions.begin(); it != sessions.end(); ) {
            Session& s = it->second;
            
            // Check if process still has valid LUID for GPU metrics
            int64_t luid = ((int64_t)s.shm->luidHighPart << 32) | (uint32_t)s.shm->luidLowPart;
            
            if (s.shm->debugLogging && luid != 0) {
                LogInfo("[Sensors] Updating PID %u (Game: %u), LUID: 0x%llX", it->first, s.shm->sourcePid, luid);
            }

            // Update metrics using the existing host_metrics logic
            scan_host::UpdateSystemMetrics(s.shm, it->first, luid);
            
            // TODO: If we had plugins, we'd call ISensorPlugin::Poll here and aggregate data
            
            ++it;
        }

        Sleep(1000); // Poll every 1s (standard for system sensors)
    }

    return 0;
}
