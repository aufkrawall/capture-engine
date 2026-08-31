#include "sensor_service.h"
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <map>
#include <memory>
#include <string_view>
#include <vector>
#include "../common/logging.h"
#include "../common/shared_defs.h"
#include "../common/strict_integer_parse.h"
#include "host_metrics.h"  // Reuse existing logic for native sensors
#include "display_timing_service.h"
#include "sensor_plugin.h"

struct SensorSession {
    HANDLE hMap;
    SharedMemoryLayout* shm;
    int64_t cachedLuid = 0;  // Cache valid LUID once discovered
    uint32_t lastSourcePid = 0;
    int64_t lastEffectiveLuid = 0;
    uint32_t lastLuidPublisherPid = 0;
    uint32_t lastLuidPublisherParentPid = 0;
    bool lastLuidPublisherEligible = false;
    bool lastSourceWasScreenGrab = false;
    uint32_t updatesSinceSummary = 0;
};

// `processFound` distinguishes "the process is gone" from "the snapshot did not
// answer", which a parent PID of 0 alone cannot. Callers that reap state on a
// dead process must not act on a failed snapshot; it stays true in that case.
static uint32_t QueryDirectParentProcessId(uint32_t processId, bool* processFound = nullptr) {
    if (processFound)
        *processFound = true;
    if (processId == 0)
        return 0;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    uint32_t parentProcessId = 0;
    bool found = false;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == processId) {
                parentProcessId = entry.th32ParentProcessID;
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
        if (processFound)
            *processFound = found;
    }
    CloseHandle(snapshot);
    return parentProcessId;
}

static void ResetGpuTelemetryForSource(SharedMemoryLayout* shm, uint32_t sourcePid) {
    if (!shm)
        return;
    auto& metrics = shm->systemMetrics;
    metrics.publicationSequence.fetch_add(1, std::memory_order_acq_rel);
    metrics.validityMask.store(0, std::memory_order_release);
    metrics.gpuUsage.store(0.0f, std::memory_order_relaxed);
    metrics.vramUsage.store(0.0f, std::memory_order_relaxed);
    metrics.vramTotal.store(0, std::memory_order_relaxed);
    metrics.cpuTemperatureC.store(0.0f, std::memory_order_relaxed);
    metrics.gpuTemperatureC.store(0.0f, std::memory_order_relaxed);
    metrics.cpuPackagePowerW.store(0.0f, std::memory_order_relaxed);
    metrics.gpuPackagePowerW.store(0.0f, std::memory_order_relaxed);
    metrics.gpuFanRpm.store(0.0f, std::memory_order_relaxed);
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

    // Create/open shutdown event keyed to controller PID and monitor the
    // controller handle so a hard parent exit cannot orphan this service.
    HANDLE hShutdownEvent = nullptr;
    HANDLE hControllerProcess = nullptr;
    if (controllerPid != 0) {
        wchar_t eventName[64];
        GenerateShutdownEventName(eventName, 64, controllerPid);
        hShutdownEvent = CreateEventW(NULL, TRUE, FALSE, eventName);
        if (!hShutdownEvent) {
            LogError("[Sensors] Cannot create the controller shutdown event (error=%lu)", GetLastError());
            return 1;
        }
        hControllerProcess = OpenProcess(SYNCHRONIZE, FALSE, controllerPid);
        if (!hControllerProcess) {
            LogError("[Sensors] Cannot monitor controller PID %u lifetime (error=%lu)", controllerPid,
                     GetLastError());
            CloseHandle(hShutdownEvent);
            return 1;
        }
    }

    // Cache DiscoveryInfo handle/mapping once, avoid kernel calls per iteration
    HANDLE hDisc = INVALID_HANDLE_VALUE;
    DiscoveryInfo* pDisc = nullptr;
    bool loggedDiscMissing = false;

    std::map<uint32_t, SensorSession> sessions;
    std::unique_ptr<DisplayTimingService> displayTimingService;
    static bool loggedDiscoveryAttempt = false;
    HardwareSensorsConfig effectiveHardwareSensors = config.hardwareSensors;
    if (!config.overlay.showCPU) {
        effectiveHardwareSensors.cpuTemperature = "off";
        effectiveHardwareSensors.cpuPackagePower = "off";
    }
    if (!config.overlay.showGPU) {
        effectiveHardwareSensors.gpuTemperature = "off";
        effectiveHardwareSensors.gpuPackagePower = "off";
        effectiveHardwareSensors.gpuFan = "off";
    }
    ce::hardware_sensors::LibreHardwareMonitorPlugin hardwareSensorPlugin(effectiveHardwareSensors);
    const bool hardwareSensorPluginActive = hardwareSensorPlugin.Start();
    const DWORD serviceWaitMs =
        hardwareSensorPluginActive && effectiveHardwareSensors.pollIntervalMs < 1000
            ? effectiveHardwareSensors.pollIntervalMs
            : 1000;
    bool loggedWaitFailure = false;

    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    while (g_SensorRunning.load(std::memory_order_acquire)) {
        hardwareSensorPlugin.Poll();
        const ce::hardware_sensors::HardwareSensorSnapshot hardwareSensors = hardwareSensorPlugin.GetSnapshot();

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
        std::vector<DisplayTimingTarget> displayTimingTargets;
        for (auto it = sessions.begin(); it != sessions.end();) {
            SensorSession& s = it->second;
            const uint32_t hookSourcePid = s.shm->GetSourcePid();
            ScreenGrabTargetSnapshot screenGrabTarget;
            const bool haveScreenGrabSnapshot = s.shm->runtimeState.ReadScreenGrabTarget(screenGrabTarget);
            const bool useScreenGrabTarget =
                hookSourcePid == 0 && haveScreenGrabSnapshot && screenGrabTarget.active;
            const uint32_t sourcePid = useScreenGrabTarget ? screenGrabTarget.processId : hookSourcePid;
            const bool haveTarget = hookSourcePid != 0 || useScreenGrabTarget;

            // No hooked source yet: skip expensive PDH/DXGI work while idle, but
            // keep checking shared memory so metrics come online quickly once a game
            // is attached.
            if (!haveTarget) {
                if (s.lastSourcePid != 0 || s.lastSourceWasScreenGrab) {
                    s.cachedLuid = 0;
                    s.lastEffectiveLuid = 0;
                    s.lastSourcePid = 0;
                    s.lastSourceWasScreenGrab = false;
                    ResetGpuTelemetryForSource(s.shm, 0);
                    LogInfo("[Sensors] Session target cleared: injectPid=%u", it->first);
                }
                ++it;
                continue;
            }

            if (sourcePid != s.lastSourcePid || useScreenGrabTarget != s.lastSourceWasScreenGrab) {
                s.cachedLuid = 0;
                s.lastEffectiveLuid = 0;
                ResetGpuTelemetryForSource(s.shm, sourcePid);
            }

            // A LUID belongs to this source only when the publishing hook stamped
            // the same process ID or a live direct child renderer. The latter is
            // the split-renderer case: the configured/injected parent remains the
            // profile source while its child owns final Vulkan presentation.
            const uint32_t luidSourcePid = s.shm->GetLuidSourcePid();
            const uint32_t luidSourceParentPid =
                luidSourcePid != 0 && luidSourcePid != sourcePid ? QueryDirectParentProcessId(luidSourcePid) : 0;
            const bool luidPublisherEligible = scan_host::metrics_policy::IsGpuTelemetryPublisherEligible(
                sourcePid, luidSourcePid, luidSourceParentPid);
            const uint64_t inheritedRendererClaim =
                s.shm->runtimeState.inheritedRendererClaim.load(std::memory_order_acquire);
            const uint32_t inheritedRendererPid = ce::inherited_renderer::RendererPid(inheritedRendererClaim);
            bool inheritedRendererAlive = true;
            const uint32_t inheritedRendererParentPid =
                inheritedRendererPid != 0 && inheritedRendererPid != sourcePid
                    ? QueryDirectParentProcessId(inheritedRendererPid, &inheritedRendererAlive)
                    : 0;
            // Only the publishing renderer clears its own claim, so a renderer
            // terminated without a layer shutdown would otherwise leave it set
            // for the rest of the session. Reap it here rather than leaving a
            // dead PID for later readers to interpret. Ownership of the
            // process-local DLSS/Streamline overrides is already scoped to the
            // publishing client, so this is hygiene and never the guard.
            if (inheritedRendererPid != 0 && !inheritedRendererAlive) {
                uint64_t staleClaim = inheritedRendererClaim;
                if (s.shm->runtimeState.inheritedRendererClaim.compare_exchange_strong(
                        staleClaim, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    LogInfo("[Sensors] Cleared stale inherited-renderer claim: rendererPid=%u clientPid=%u",
                            inheritedRendererPid,
                            ce::inherited_renderer::ClientPid(inheritedRendererClaim));
                }
            }
            uint32_t rendererPid = 0;
            if (inheritedRendererPid != sourcePid && inheritedRendererParentPid == sourcePid) {
                rendererPid = inheritedRendererPid;
            } else if (luidSourcePid != sourcePid && luidPublisherEligible) {
                rendererPid = luidSourcePid;
            }
            if (!useScreenGrabTarget && s.shm->ReadOverlayConfig().frameTimeSource == FrameTimeSource::DisplayChange) {
                displayTimingTargets.push_back({sourcePid, rendererPid, &s.shm->displayTiming});
            }
            int64_t luid = 0;
            if (!useScreenGrabTarget && luidPublisherEligible) {
                const uint64_t high = static_cast<uint32_t>(s.shm->GetLuidHighPart());
                const uint64_t low = static_cast<uint32_t>(s.shm->GetLuidLowPart());
                luid = static_cast<int64_t>((high << 32) | low);
            }
            if (useScreenGrabTarget) {
                const uint64_t high = static_cast<uint32_t>(screenGrabTarget.adapterLuidHigh);
                const uint64_t low = static_cast<uint32_t>(screenGrabTarget.adapterLuidLow);
                luid = static_cast<int64_t>((high << 32) | low);
            }

            // Cache valid LUID once discovered (it may reset during game restart)
            if (luid != 0) {
                s.cachedLuid = luid;
            }

            // Use cached LUID if current is 0
            int64_t effectiveLuid = (luid != 0) ? luid : s.cachedLuid;

            if (sourcePid != s.lastSourcePid || effectiveLuid != s.lastEffectiveLuid ||
                luidSourcePid != s.lastLuidPublisherPid ||
                luidSourceParentPid != s.lastLuidPublisherParentPid ||
                luidPublisherEligible != s.lastLuidPublisherEligible ||
                useScreenGrabTarget != s.lastSourceWasScreenGrab) {
                LogInfo(
                    "[Sensors] Session update: injectPid=%u targetPid=%u targetSource=%s adapterLuid=0x%llX "
                    "graphicsLuidPublisherPid=%u publisherParentPid=%u publisherEligible=%d",
                    it->first, sourcePid, useScreenGrabTarget ? "screen-grab-media" : "inject-hook", effectiveLuid,
                    luidSourcePid, luidSourceParentPid, luidPublisherEligible ? 1 : 0);
                s.lastSourcePid = sourcePid;
                s.lastEffectiveLuid = effectiveLuid;
                s.lastLuidPublisherPid = luidSourcePid;
                s.lastLuidPublisherParentPid = luidSourceParentPid;
                s.lastLuidPublisherEligible = luidPublisherEligible;
                s.lastSourceWasScreenGrab = useScreenGrabTarget;
            }

            s.updatesSinceSummary++;
            if (IsDebugLoggingEnabled(config.logLevel) && s.updatesSinceSummary >= 30) {
                const auto& metrics = s.shm->systemMetrics;
                LogInfo(
                    "[Sensors] Summary: injectPid=%u gamePid=%u luid=0x%llX updates=%u cpu=%.1f maxCore=%u "
                    "gpu=%.1f vramMB=%.1f vramTotalMB=%llu cpuTemp=%.1f gpuTemp=%.1f cpuW=%.1f gpuW=%.1f "
                    "gpuFan=%.0f validity=0x%X publisherPid=%u publisherParentPid=%u publisherEligible=%d",
                    it->first, sourcePid, effectiveLuid, s.updatesSinceSummary,
                    metrics.cpuUsage.load(std::memory_order_relaxed),
                    metrics.maxCoreLoad.load(std::memory_order_relaxed),
                    metrics.gpuUsage.load(std::memory_order_relaxed),
                    metrics.vramUsage.load(std::memory_order_relaxed),
                    metrics.vramTotal.load(std::memory_order_relaxed) / (1024 * 1024),
                    metrics.cpuTemperatureC.load(std::memory_order_relaxed),
                    metrics.gpuTemperatureC.load(std::memory_order_relaxed),
                    metrics.cpuPackagePowerW.load(std::memory_order_relaxed),
                    metrics.gpuPackagePowerW.load(std::memory_order_relaxed),
                    metrics.gpuFanRpm.load(std::memory_order_relaxed),
                    metrics.validityMask.load(std::memory_order_relaxed), luidSourcePid, luidSourceParentPid,
                    luidPublisherEligible ? 1 : 0);
                s.updatesSinceSummary = 0;
            }

            // Update metrics using the existing host_metrics logic
            scan_host::UpdateSystemMetrics(
                s.shm, sourcePid, effectiveLuid,
                useScreenGrabTarget ? scan_host::metrics_policy::AdapterResolutionSource::CaptureDeviceLuid
                                    : scan_host::metrics_policy::AdapterResolutionSource::HookLuid,
                hardwareSensors);

            ++it;
        }

        if (!displayTimingTargets.empty()) {
            if (!displayTimingService) {
                displayTimingService = std::make_unique<DisplayTimingService>();
                displayTimingService->Start();
            }
            displayTimingService->UpdateTargets(displayTimingTargets);
        } else if (displayTimingService) {
            displayTimingService->UpdateTargets({});
            displayTimingService.reset();
        }

        HANDLE waitHandles[2] = {};
        DWORD waitCount = 0;
        DWORD shutdownIndex = MAXDWORD;
        DWORD controllerIndex = MAXDWORD;
        if (hShutdownEvent) {
            shutdownIndex = waitCount;
            waitHandles[waitCount++] = hShutdownEvent;
        }
        if (hControllerProcess) {
            controllerIndex = waitCount;
            waitHandles[waitCount++] = hControllerProcess;
        }
        if (waitCount == 0) {
            Sleep(serviceWaitMs);
            continue;
        }
        const DWORD waitResult = WaitForMultipleObjects(waitCount, waitHandles, FALSE, serviceWaitMs);
        if (waitResult < WAIT_OBJECT_0 + waitCount) {
            const DWORD signaledIndex = waitResult - WAIT_OBJECT_0;
            if (signaledIndex == shutdownIndex)
                LogInfo("[Sensors] Shutdown signal received, exiting");
            else if (signaledIndex == controllerIndex)
                LogInfo("[Sensors] Controller exited; stopping the sensor service and optional bridge");
            break;
        }
        if (waitResult == WAIT_FAILED) {
            if (!loggedWaitFailure) {
                LogWarn("[Sensors] Lifetime wait failed (error=%lu); retaining bounded polling", GetLastError());
                loggedWaitFailure = true;
            }
            Sleep(serviceWaitMs);
        }
    }

    if (hControllerProcess)
        CloseHandle(hControllerProcess);
    if (hShutdownEvent)
        CloseHandle(hShutdownEvent);

    // Cleanup cached DiscoveryInfo handles
    if (pDisc)
        UnmapViewOfFile(pDisc);
    if (hDisc != INVALID_HANDLE_VALUE)
        CloseHandle(hDisc);

    return 0;
}
