#include "sensor_service.h"
#include <windows.h>
#include <atomic>
#include <map>
#include <string_view>
#include <vector>
#include "../common/logging.h"
#include "../common/shared_defs.h"
#include "../common/strict_integer_parse.h"
#include "host_metrics.h"  // Reuse existing logic for native sensors
#include "sensor_plugin.h"

struct SensorSession {
    HANDLE hMap;
    SharedMemoryLayout* shm;
    int64_t cachedLuid = 0;  // Cache valid LUID once discovered
    uint32_t lastSourcePid = 0;
    int64_t lastEffectiveLuid = 0;
    uint32_t updatesSinceSummary = 0;
};

static void ResetGpuTelemetryForSource(SharedMemoryLayout* shm, uint32_t sourcePid) {
    if (!shm)
        return;
    auto& metrics = shm->systemMetrics;
    metrics.publicationSequence.fetch_add(1, std::memory_order_acq_rel);
    metrics.validityMask.store(0, std::memory_order_release);
    metrics.gpuUsage.store(0.0f, std::memory_order_relaxed);
    metrics.vramUsage.store(0.0f, std::memory_order_relaxed);
    metrics.vramTotal.store(0, std::memory_order_relaxed);
    metrics.adapterLuidLow.store(0, std::memory_order_relaxed);
    metrics.adapterLuidHigh.store(0, std::memory_order_relaxed);
    metrics.adapterSource.store(SYSTEM_METRICS_ADAPTER_UNAVAILABLE, std::memory_order_relaxed);
    metrics.sourcePid.store(sourcePid, std::memory_order_release);
    metrics.publicationSequence.fetch_add(1, std::memory_order_release);
}

int SensorProcessMain(const AppConfig& config) {
    Log_SetLevel(config.logLevel);
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

    // Cache DiscoveryInfo handle/mapping once, avoid kernel calls per iteration
    HANDLE hDisc = INVALID_HANDLE_VALUE;
    DiscoveryInfo* pDisc = nullptr;
    bool loggedDiscMissing = false;

    std::map<uint32_t, SensorSession> sessions;
    static bool loggedDiscoveryAttempt = false;

    // In a real plugin-based system, we'd load DLLs here.
    // For now, we reuse the scan_host logic but we will encapsulate it better.

    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    while (g_SensorRunning.load(std::memory_order_acquire)) {
        // 1. Discover new sessions (cached handle, open/map once)
        if (hDisc == INVALID_HANDLE_VALUE) {
            hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
            if (hDisc) {
                pDisc = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
                if (!pDisc) {
                    CloseHandle(hDisc);
                    hDisc = INVALID_HANDLE_VALUE;
                }
            }
        }
        if (hDisc != INVALID_HANDLE_VALUE && pDisc) {
            if (ValidateDiscoveryInfo(pDisc)) {
                uint32_t pid = pDisc->GetInjectPid();
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
                        if (shm && ValidateSharedMemory(shm)) {
                            LogInfo(
                                "[Sensors] Discovered new session: Inject PID %u, Game "
                                "PID %u, ABI 0x%08X",
                                pid, shm->GetSourcePid(), SHARED_MEMORY_ABI_SIGNATURE);
                            sessions[pid] = {hSM, shm};
                        } else {
                            if (shm) {
                                LogError(
                                    "[Sensors] Rejected incompatible shared memory for PID %u "
                                    "(version=%u size=%u abi=0x%08X)",
                                    pid, shm->GetVersion(), shm->structSize.load(std::memory_order_acquire),
                                    shm->abiSignature.load(std::memory_order_acquire));
                                UnmapViewOfFile(shm);
                            } else {
                                LogError("[Sensors] Failed to map shared memory for PID %u", pid);
                            }
                            CloseHandle(hSM);
                        }
                    } else {
                        LogError("[Sensors] Failed to open shared memory for PID %u: %lu", pid, GetLastError());
                    }
                }
            }
        } else if (!loggedDiscMissing) {
            LogInfo("[Sensors] Waiting for discovery shared memory...");
            loggedDiscMissing = true;
        }

        // 2. Poll metrics for all active sessions
        for (auto it = sessions.begin(); it != sessions.end();) {
            SensorSession& s = it->second;
            uint32_t sourcePid = s.shm->GetSourcePid();

            // No hooked source yet: skip expensive PDH/DXGI work while idle, but
            // keep checking shared memory so metrics come online quickly once a game
            // is attached.
            if (sourcePid == 0) {
                if (s.lastSourcePid != 0) {
                    s.cachedLuid = 0;
                    s.lastEffectiveLuid = 0;
                    s.lastSourcePid = 0;
                    ResetGpuTelemetryForSource(s.shm, 0);
                    LogInfo("[Sensors] Session source cleared: injectPid=%u", it->first);
                }
                ++it;
                continue;
            }

            if (sourcePid != s.lastSourcePid) {
                s.cachedLuid = 0;
                s.lastEffectiveLuid = 0;
                ResetGpuTelemetryForSource(s.shm, sourcePid);
            }

            // A LUID belongs to this source only when the publishing hook stamped
            // the same process ID. Otherwise it may be stale from an earlier game
            // in the controller session, and host-side PID inference must resolve it.
            const uint32_t luidSourcePid = s.shm->GetLuidSourcePid();
            int64_t luid = 0;
            if (luidSourcePid == sourcePid) {
                const uint64_t high = static_cast<uint32_t>(s.shm->GetLuidHighPart());
                const uint64_t low = static_cast<uint32_t>(s.shm->GetLuidLowPart());
                luid = static_cast<int64_t>((high << 32) | low);
            }

            // Cache valid LUID once discovered (it may reset during game restart)
            if (luid != 0) {
                s.cachedLuid = luid;
            }

            // Use cached LUID if current is 0
            int64_t effectiveLuid = (luid != 0) ? luid : s.cachedLuid;

            if (sourcePid != s.lastSourcePid || effectiveLuid != s.lastEffectiveLuid) {
                LogInfo("[Sensors] Session update: injectPid=%u gamePid=%u hookLuid=0x%llX luidPublisherPid=%u",
                        it->first, sourcePid, effectiveLuid, luidSourcePid);
                s.lastSourcePid = sourcePid;
                s.lastEffectiveLuid = effectiveLuid;
            }

            s.updatesSinceSummary++;
            if (IsDebugLoggingEnabled(config.logLevel) && s.updatesSinceSummary >= 30) {
                const auto& metrics = s.shm->systemMetrics;
                LogInfo(
                    "[Sensors] Summary: injectPid=%u gamePid=%u luid=0x%llX updates=%u cpu=%.1f maxCore=%u "
                    "gpu=%.1f vramMB=%.1f vramTotalMB=%llu validity=0x%X",
                    it->first, sourcePid, effectiveLuid, s.updatesSinceSummary,
                    metrics.cpuUsage.load(std::memory_order_relaxed),
                    metrics.maxCoreLoad.load(std::memory_order_relaxed),
                    metrics.gpuUsage.load(std::memory_order_relaxed),
                    metrics.vramUsage.load(std::memory_order_relaxed),
                    metrics.vramTotal.load(std::memory_order_relaxed) / (1024 * 1024),
                    metrics.validityMask.load(std::memory_order_relaxed));
                s.updatesSinceSummary = 0;
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

    // Cleanup cached DiscoveryInfo handles
    if (pDisc)
        UnmapViewOfFile(pDisc);
    if (hDisc != INVALID_HANDLE_VALUE)
        CloseHandle(hDisc);

    return 0;
}
