#include "sensor_service.h"
#include "../common/logging.h"
#include "../common/shared_defs.h"
#include "host_metrics.h" // Reuse existing logic for native sensors
#include "sensor_plugin.h"
#include <map>
#include <vector>
#include <windows.h>

struct Session {
  HANDLE hMap;
  SharedMemoryLayout *shm;
  int64_t cachedLuid = 0; // Cache valid LUID once discovered
};

int SensorProcessMain(const AppConfig &config) {
  LogInfo("[Sensors] Dedicated sensor service started");

  std::map<uint32_t, Session> sessions;
  static bool loggedDiscoveryAttempt = false;

  // In a real plugin-based system, we'd load DLLs here.
  // For now, we reuse the scan_host logic but we will encapsulate it better.

  while (true) {
    // 1. Discover new sessions
    HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (hDisc) {
      DiscoveryInfo *info = (DiscoveryInfo *)MapViewOfFile(
          hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
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
              SharedMemoryLayout *shm = (SharedMemoryLayout *)MapViewOfFile(
                  hSM, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemoryLayout));
              if (shm) {
                LogInfo("[Sensors] Discovered new session: Inject PID %u, Game "
                        "PID %u",
                        pid, shm->GetSourcePid());
                sessions[pid] = {hSM, shm};
              } else {
                LogError("[Sensors] Failed to map shared memory for PID %u",
                         pid);
                CloseHandle(hSM);
              }
            } else {
              LogError("[Sensors] Failed to open shared memory for PID %u: %d",
                       pid, GetLastError());
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
      Session &s = it->second;

      // Read LUID from shared memory
      int64_t luid = ((int64_t)s.shm->GetLuidHighPart() << 32) |
                     (uint32_t)s.shm->GetLuidLowPart();

      // Cache valid LUID once discovered (it may reset during game restart)
      if (luid != 0) {
        s.cachedLuid = luid;
      }

      // Use cached LUID if current is 0
      int64_t effectiveLuid = (luid != 0) ? luid : s.cachedLuid;

      if (s.shm->GetDebugLogging() && effectiveLuid != 0) {
        LogInfo("[Sensors] Updating PID %u (Game: %u), LUID: 0x%llX", it->first,
                s.shm->GetSourcePid(), effectiveLuid);
      }

      // Update metrics using the existing host_metrics logic
      scan_host::UpdateSystemMetrics(s.shm, it->first, effectiveLuid);


      ++it;
    }

    Sleep(1000); // Poll every 1s (standard for system sensors)
  }

  return 0;
}
