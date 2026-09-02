#include "system_metrics.h"
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "../../common/thread_wait.h"
#include "hook_common.h"
#include "ipc_client.h"

// PDH Lib is linked via build.py

// Definitions for NtQuerySystemInformation
typedef LONG KPRIORITY;
typedef enum _SYSTEM_INFORMATION_CLASS_LITERAL {
    SystemProcessorPerformanceInformation = 8,
} SYSTEM_INFORMATION_CLASS_LITERAL;

typedef struct _SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
} SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION;

typedef NTSTATUS(WINAPI* NtQuerySystemInformationPtr)(ULONG SystemInformationClass, PVOID SystemInformation,
                                                      ULONG SystemInformationLength, PULONG ReturnLength);

namespace {
bool JoinThreadWithTimeout(std::thread& thread, DWORD timeoutMs, const char* context) {
    if (!thread.joinable()) {
        return true;
    }

    HANDLE threadHandle = ce::Win32ThreadHandle(thread);
    DWORD waitResult = WaitForSingleObject(threadHandle, timeoutMs);
    if (waitResult == WAIT_OBJECT_0) {
        thread.join();
        return true;
    }

    if (waitResult == WAIT_TIMEOUT) {
        EarlyLog("%s: thread join timed out after %lu ms", context, static_cast<unsigned long>(timeoutMs));
    } else {
        EarlyLog("%s: WaitForSingleObject failed (error=%lu)", context, GetLastError());
    }
    return false;
}

struct HostMetricsPublication {
    float cpuUsage = 0.0f;
    float ramUsageGB = 0.0f;
    float gpuUsage = 0.0f;
    float vramUsageMB = 0.0f;
    uint64_t vramTotal = 0;
    uint32_t maxCoreLoad = 0;
    float cpuTemperatureC = 0.0f;
    float gpuTemperatureC = 0.0f;
    float cpuPackagePowerW = 0.0f;
    float gpuPackagePowerW = 0.0f;
    float gpuFanRpm = 0.0f;
    float cpuCoreClockMhz = 0.0f;
    float gpuCoreClockMhz = 0.0f;
    float gpuMemoryClockMhz = 0.0f;
    float gpuCoreVoltageV = 0.0f;
    uint32_t validityMask = 0;
    uint32_t sourcePid = 0;
    LUID adapterLuid = {0, 0};
};

bool IsSaneHostMetricsPublication(const HostMetricsPublication& publication) {
    constexpr uint32_t kKnownValidity =
        SYSTEM_METRIC_GPU_USAGE_VALID | SYSTEM_METRIC_VRAM_USAGE_VALID | SYSTEM_METRIC_VRAM_TOTAL_VALID |
        SYSTEM_METRIC_CPU_TEMPERATURE_VALID | SYSTEM_METRIC_GPU_TEMPERATURE_VALID |
        SYSTEM_METRIC_CPU_PACKAGE_POWER_VALID | SYSTEM_METRIC_GPU_PACKAGE_POWER_VALID |
        SYSTEM_METRIC_GPU_FAN_VALID | SYSTEM_METRIC_CPU_CORE_CLOCK_VALID | SYSTEM_METRIC_GPU_CORE_CLOCK_VALID |
        SYSTEM_METRIC_GPU_MEMORY_CLOCK_VALID | SYSTEM_METRIC_GPU_VOLTAGE_VALID;
    if (!std::isfinite(publication.cpuUsage) || publication.cpuUsage < 0.0f || publication.cpuUsage > 100.0f ||
        !std::isfinite(publication.ramUsageGB) || publication.ramUsageGB < 0.0f ||
        publication.ramUsageGB > 1048576.0f || publication.maxCoreLoad > 100u ||
        (publication.validityMask & ~kKnownValidity) != 0) {
        return false;
    }
    if ((publication.validityMask & SYSTEM_METRIC_GPU_USAGE_VALID) != 0 &&
        (!std::isfinite(publication.gpuUsage) || publication.gpuUsage < 0.0f || publication.gpuUsage > 100.0f)) {
        return false;
    }
    if ((publication.validityMask & SYSTEM_METRIC_VRAM_USAGE_VALID) != 0 &&
        (!std::isfinite(publication.vramUsageMB) || publication.vramUsageMB < 0.0f ||
         publication.vramUsageMB > 1073741824.0f)) {
        return false;
    }
    if ((publication.validityMask & SYSTEM_METRIC_VRAM_TOTAL_VALID) != 0 &&
        (publication.vramTotal == 0 || publication.vramTotal > (1ull << 60))) {
        return false;
    }
    if ((publication.validityMask & SYSTEM_METRIC_CPU_TEMPERATURE_VALID) != 0 &&
        (!std::isfinite(publication.cpuTemperatureC) || publication.cpuTemperatureC <= 0.0f ||
         publication.cpuTemperatureC > 250.0f)) {
        return false;
    }
    if ((publication.validityMask & SYSTEM_METRIC_GPU_TEMPERATURE_VALID) != 0 &&
        (!std::isfinite(publication.gpuTemperatureC) || publication.gpuTemperatureC <= 0.0f ||
         publication.gpuTemperatureC > 250.0f)) {
        return false;
    }
    if ((publication.validityMask & SYSTEM_METRIC_CPU_PACKAGE_POWER_VALID) != 0 &&
        (!std::isfinite(publication.cpuPackagePowerW) || publication.cpuPackagePowerW <= 0.0f ||
         publication.cpuPackagePowerW > 5000.0f)) {
        return false;
    }
    if ((publication.validityMask & SYSTEM_METRIC_GPU_PACKAGE_POWER_VALID) != 0 &&
        (!std::isfinite(publication.gpuPackagePowerW) || publication.gpuPackagePowerW <= 0.0f ||
         publication.gpuPackagePowerW > 5000.0f)) {
        return false;
    }
    // A stopped fan is the one metric for which zero is a real reading.
    if ((publication.validityMask & SYSTEM_METRIC_GPU_FAN_VALID) != 0 &&
        (!std::isfinite(publication.gpuFanRpm) || publication.gpuFanRpm < 0.0f ||
         publication.gpuFanRpm > 100000.0f)) {
        return false;
    }
    if ((publication.validityMask & SYSTEM_METRIC_CPU_CORE_CLOCK_VALID) != 0 &&
        (!std::isfinite(publication.cpuCoreClockMhz) || publication.cpuCoreClockMhz <= 0.0f ||
         publication.cpuCoreClockMhz > 20000.0f)) {
        return false;
    }
    if ((publication.validityMask & SYSTEM_METRIC_GPU_CORE_CLOCK_VALID) != 0 &&
        (!std::isfinite(publication.gpuCoreClockMhz) || publication.gpuCoreClockMhz <= 0.0f ||
         publication.gpuCoreClockMhz > 20000.0f)) {
        return false;
    }
    if ((publication.validityMask & SYSTEM_METRIC_GPU_MEMORY_CLOCK_VALID) != 0 &&
        (!std::isfinite(publication.gpuMemoryClockMhz) || publication.gpuMemoryClockMhz <= 0.0f ||
         publication.gpuMemoryClockMhz > 20000.0f)) {
        return false;
    }
    if ((publication.validityMask & SYSTEM_METRIC_GPU_VOLTAGE_VALID) != 0 &&
        (!std::isfinite(publication.gpuCoreVoltageV) || publication.gpuCoreVoltageV <= 0.0f ||
         publication.gpuCoreVoltageV > 10.0f)) {
        return false;
    }
    if ((publication.validityMask & (SYSTEM_METRIC_VRAM_USAGE_VALID | SYSTEM_METRIC_VRAM_TOTAL_VALID)) ==
        (SYSTEM_METRIC_VRAM_USAGE_VALID | SYSTEM_METRIC_VRAM_TOTAL_VALID)) {
        const double usedBytes = static_cast<double>(publication.vramUsageMB) * 1024.0 * 1024.0;
        if (usedBytes > static_cast<double>(publication.vramTotal) * 1.25)
            return false;
    }
    return true;
}

bool ReadHostMetricsPublication(SharedMemoryLayout* sharedMem, HostMetricsPublication& publication) {
    if (!sharedMem)
        return false;
    auto& metrics = sharedMem->systemMetrics;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t sequenceBefore = metrics.publicationSequence.load(std::memory_order_acquire);
        if ((sequenceBefore & 1u) != 0)
            continue;
        publication.sourcePid = metrics.sourcePid.load(std::memory_order_relaxed);
        publication.cpuUsage = metrics.cpuUsage.load(std::memory_order_relaxed);
        publication.ramUsageGB = metrics.ramUsage.load(std::memory_order_relaxed);
        publication.gpuUsage = metrics.gpuUsage.load(std::memory_order_relaxed);
        publication.vramUsageMB = metrics.vramUsage.load(std::memory_order_relaxed);
        publication.vramTotal = metrics.vramTotal.load(std::memory_order_relaxed);
        publication.maxCoreLoad = metrics.maxCoreLoad.load(std::memory_order_relaxed);
        publication.cpuTemperatureC = metrics.cpuTemperatureC.load(std::memory_order_relaxed);
        publication.gpuTemperatureC = metrics.gpuTemperatureC.load(std::memory_order_relaxed);
        publication.cpuPackagePowerW = metrics.cpuPackagePowerW.load(std::memory_order_relaxed);
        publication.gpuPackagePowerW = metrics.gpuPackagePowerW.load(std::memory_order_relaxed);
        publication.gpuFanRpm = metrics.gpuFanRpm.load(std::memory_order_relaxed);
        publication.cpuCoreClockMhz = metrics.cpuCoreClockMhz.load(std::memory_order_relaxed);
        publication.gpuCoreClockMhz = metrics.gpuCoreClockMhz.load(std::memory_order_relaxed);
        publication.gpuMemoryClockMhz = metrics.gpuMemoryClockMhz.load(std::memory_order_relaxed);
        publication.gpuCoreVoltageV = metrics.gpuCoreVoltageV.load(std::memory_order_relaxed);
        publication.adapterLuid.LowPart = static_cast<DWORD>(metrics.adapterLuidLow.load(std::memory_order_relaxed));
        publication.adapterLuid.HighPart = static_cast<LONG>(metrics.adapterLuidHigh.load(std::memory_order_relaxed));
        publication.validityMask = metrics.validityMask.load(std::memory_order_relaxed);
        const uint32_t sequenceAfter = metrics.publicationSequence.load(std::memory_order_acquire);
        if (sequenceBefore == sequenceAfter) {
            const uint32_t activeSourcePid = sharedMem->GetSourcePid();
            if (publication.sourcePid == 0 || publication.sourcePid != activeSourcePid)
                return false;
            if (IsSaneHostMetricsPublication(publication))
                return true;

            static std::atomic<uint32_t> rejectedPublications{0};
            const uint32_t rejected = rejectedPublications.fetch_add(1, std::memory_order_relaxed) + 1;
            if (rejected <= 4 || rejected % 100u == 0u) {
                EarlyLog(
                    "SystemMetricsCollector: Rejected invalid host publication "
                    "cpu=%.3f maxCore=%u ramGB=%.3f gpu=%.3f vramMB=%.3f vramTotal=%llu validity=0x%X count=%u",
                    publication.cpuUsage, publication.maxCoreLoad, publication.ramUsageGB, publication.gpuUsage,
                    publication.vramUsageMB, static_cast<unsigned long long>(publication.vramTotal),
                    publication.validityMask, rejected);
            }
            return false;
        }
    }
    return false;
}
}  // namespace

SystemMetricsCollector& SystemMetricsCollector::Get() {
    static SystemMetricsCollector instance;
    return instance;
}

SystemMetricsCollector::SystemMetricsCollector()
    : threadRunning(false),
      stopThread(false),
      pdhInitialized(false),
      gpuPdhInitialized(false),
      vramPdhInitialized(false) {}

SystemMetricsCollector::~SystemMetricsCollector() {
    stopThread = true;

    if (updateThread.joinable()) {
        if (!JoinThreadWithTimeout(updateThread, 1500, "SystemMetricsCollector::~SystemMetricsCollector")) {
            updateThread.detach();
        }
    }

    // Do NOT release DXGI, PDH, or other DLL-bound resources here.
    // This destructor runs during DLL_PROCESS_DETACH (static singleton),
    // when dxgi.dll, pdh.dll etc. may already be partially torn down.
    // Releasing them at this point can raise exceptions (e.g. DXGI 0x87A).
    // The OS reclaims all resources on process exit.
}

void SystemMetricsCollector::Shutdown() {
    stopThread = true;

    // Wait for thread to exit with a timeout
    if (updateThread.joinable()) {
        if (!JoinThreadWithTimeout(updateThread, 1500, "SystemMetricsCollector::Shutdown")) {
            updateThread.detach();
        } else {
            EarlyLog("SystemMetricsCollector: Thread exited cleanly, joining");
        }
    }
    EarlyLog("SystemMetricsCollector: Shutdown() complete");
}

void SystemMetricsCollector::InitPDH() {
    if (strstr(g_ProcessName, "SonsOfTheForest") != nullptr) {
        EarlyLog("SystemMetricsCollector: PDH disabled for %s to prevent crash.", g_ProcessName);
        return;
    }

    EarlyLog("SystemMetricsCollector: InitPDH started");
    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryA(NULL, 0, &query) == ERROR_SUCCESS) {
        cpuQuery = (void*)query;
        PDH_HCOUNTER counter = nullptr;
        if (PdhAddEnglishCounterA(query, "\\Processor(_Total)\\% Processor Time", 0, &counter) == ERROR_SUCCESS) {
            cpuCounter = (void*)counter;
            PdhCollectQueryData(query);
            pdhInitialized = true;
            EarlyLog("SystemMetricsCollector: InitPDH success (English)");
        } else {
            if (PdhAddCounterA(query, "\\Processor(_Total)\\% Processor Time", 0, &counter) == ERROR_SUCCESS) {
                cpuCounter = (void*)counter;
                PdhCollectQueryData(query);
                pdhInitialized = true;
                EarlyLog("SystemMetricsCollector: InitPDH success (Local)");
            } else {
                EarlyLog("SystemMetricsCollector: InitPDH failed to add counter");
            }
        }
    } else {
        EarlyLog("SystemMetricsCollector: InitPDH PdhOpenQueryA failed");
    }
}

void SystemMetricsCollector::Initialize(int32_t luidLow, int32_t luidHigh) {
    {
        std::lock_guard<std::mutex> lock(mutex);

        // If LUID matches, just ensure thread is running
        if (adapterLuid.LowPart == (DWORD)luidLow && adapterLuid.HighPart == (LONG)luidHigh) {
            if (!threadRunning) {
                EarlyLog(
                    "SystemMetricsCollector: LUID matches but thread not running. "
                    "Starting...");
                stopThread = false;
                updateThread = std::thread(&SystemMetricsCollector::BackgroundUpdateLoop, this);
                threadRunning = true;
            }
            return;
        }

        // New LUID
        adapterLuid.LowPart = luidLow;
        adapterLuid.HighPart = luidHigh;
        current.gpuUsage = 0.0f;
        current.gpuUsageValid = false;
        current.vramUsed = 0;
        current.vramUsageValid = false;
        current.vramTotal = 0;
        current.gpuTemperatureC = 0.0f;
        current.gpuTemperatureValid = false;
        current.gpuPackagePowerW = 0.0f;
        current.gpuPackagePowerValid = false;
        current.gpuFanRpm = 0.0f;
        current.gpuFanValid = false;
        current.gpuCoreClockMhz = 0.0f;
        current.gpuCoreClockValid = false;
        current.gpuMemoryClockMhz = 0.0f;
        current.gpuMemoryClockValid = false;
        current.gpuCoreVoltageV = 0.0f;
        current.gpuCoreVoltageValid = false;
        snprintf(cachedLuidPart, sizeof(cachedLuidPart), "luid_0x%08X_0x%08X", (unsigned int)luidHigh,
                 (unsigned int)luidLow);

        // Invalidate cached resources
        if (cachedAdapter) {
            ((IUnknown*)cachedAdapter)->Release();
            cachedAdapter = nullptr;
        }

        // Publish LUID to shared memory for Host (sensors.exe)
        if (g_IPC && g_IPC->GetSharedMem()) {
            g_IPC->GetSharedMem()->SetLuidLowPart(luidLow);
            g_IPC->GetSharedMem()->SetLuidHighPart(luidHigh);
            g_IPC->GetSharedMem()->SetLuidSourcePid(GetCurrentProcessId());
            // EarlyLog("SystemMetricsCollector: Published LUID to Shared Memory:
            // %08X-%08X", luidHigh, luidLow);
        }
    }

    EarlyLog("SystemMetricsCollector: Initialized with LUID %s. ThreadRunning=%d", cachedLuidPart,
             threadRunning.load());
    DetectHardwareNames();

    if (!threadRunning) {
        EarlyLog("SystemMetricsCollector: Starting Background Thread...");
        try {
            stopThread = false;
            updateThread = std::thread(&SystemMetricsCollector::BackgroundUpdateLoop, this);
            threadRunning = true;
            EarlyLog("SystemMetricsCollector: Background Thread started successfully.");
        } catch (...) {
            EarlyLog(
                "SystemMetricsCollector: Failed to start background thread "
                "(Exception)!");
        }
    }
}

bool SystemMetricsCollector::UpdateFromHost() {
    if (!g_IPC || !g_IPC->GetSharedMem())
        return false;

    HostMetricsPublication publication;
    if (!ReadHostMetricsPublication(g_IPC->GetSharedMem(), publication))
        return false;

    std::lock_guard<std::mutex> lock(mutex);
    const bool sourceChanged = hostMetricsSourcePid != publication.sourcePid;
    const bool adapterChanged = hostMetricsAdapterLuid.LowPart != publication.adapterLuid.LowPart ||
                                hostMetricsAdapterLuid.HighPart != publication.adapterLuid.HighPart;
    if (sourceChanged || adapterChanged) {
        current.gpuUsage = 0.0f;
        current.gpuUsageValid = false;
        current.vramUsed = 0;
        current.vramUsageValid = false;
        if (adapterChanged)
            current.vramTotal = 0;
        hostMetricsSourcePid = publication.sourcePid;
        hostMetricsAdapterLuid = publication.adapterLuid;
    }

    current.cpuUsage = publication.cpuUsage;
    current.cpuMaxCoreUsage = static_cast<float>(publication.maxCoreLoad);
    current.ramUsed = static_cast<uint64_t>((std::max)(0.0f, publication.ramUsageGB) * 1024.0 * 1024.0 * 1024.0);

    current.gpuUsageValid = (publication.validityMask & SYSTEM_METRIC_GPU_USAGE_VALID) != 0;
    current.gpuUsage = current.gpuUsageValid ? publication.gpuUsage : 0.0f;
    current.vramUsageValid = (publication.validityMask & SYSTEM_METRIC_VRAM_USAGE_VALID) != 0;
    current.vramUsed =
        current.vramUsageValid ? static_cast<uint64_t>((std::max)(0.0f, publication.vramUsageMB) * 1024.0 * 1024.0) : 0;
    if ((publication.validityMask & SYSTEM_METRIC_VRAM_TOTAL_VALID) != 0)
        current.vramTotal = publication.vramTotal;
    current.cpuTemperatureValid = (publication.validityMask & SYSTEM_METRIC_CPU_TEMPERATURE_VALID) != 0;
    current.cpuTemperatureC = current.cpuTemperatureValid ? publication.cpuTemperatureC : 0.0f;
    current.gpuTemperatureValid = (publication.validityMask & SYSTEM_METRIC_GPU_TEMPERATURE_VALID) != 0;
    current.gpuTemperatureC = current.gpuTemperatureValid ? publication.gpuTemperatureC : 0.0f;
    current.cpuPackagePowerValid = (publication.validityMask & SYSTEM_METRIC_CPU_PACKAGE_POWER_VALID) != 0;
    current.cpuPackagePowerW = current.cpuPackagePowerValid ? publication.cpuPackagePowerW : 0.0f;
    current.gpuPackagePowerValid = (publication.validityMask & SYSTEM_METRIC_GPU_PACKAGE_POWER_VALID) != 0;
    current.gpuPackagePowerW = current.gpuPackagePowerValid ? publication.gpuPackagePowerW : 0.0f;
    current.gpuFanValid = (publication.validityMask & SYSTEM_METRIC_GPU_FAN_VALID) != 0;
    current.gpuFanRpm = current.gpuFanValid ? publication.gpuFanRpm : 0.0f;
    current.cpuCoreClockValid = (publication.validityMask & SYSTEM_METRIC_CPU_CORE_CLOCK_VALID) != 0;
    current.cpuCoreClockMhz = current.cpuCoreClockValid ? publication.cpuCoreClockMhz : 0.0f;
    current.gpuCoreClockValid = (publication.validityMask & SYSTEM_METRIC_GPU_CORE_CLOCK_VALID) != 0;
    current.gpuCoreClockMhz = current.gpuCoreClockValid ? publication.gpuCoreClockMhz : 0.0f;
    current.gpuMemoryClockValid = (publication.validityMask & SYSTEM_METRIC_GPU_MEMORY_CLOCK_VALID) != 0;
    current.gpuMemoryClockMhz = current.gpuMemoryClockValid ? publication.gpuMemoryClockMhz : 0.0f;
    current.gpuCoreVoltageValid = (publication.validityMask & SYSTEM_METRIC_GPU_VOLTAGE_VALID) != 0;
    current.gpuCoreVoltageV = current.gpuCoreVoltageValid ? publication.gpuCoreVoltageV : 0.0f;
    return true;
}

void SystemMetricsCollector::BackgroundUpdateLoop() {
    EarlyLog("SystemMetricsCollector: Background thread started");
    static bool loggedIPCMode = false;

    while (!stopThread.load(std::memory_order_acquire) && !HookIsShuttingDown()) {
        static int loopTrace = 0;
        if (loopTrace % 100 == 0)
            EarlyLog("SystemMetricsCollector: Loop Iteration %d", loopTrace);
        loopTrace++;

        bool usedIPC = false;

        // Prioritize IPC (Host-provided) metrics
        if (g_IPC && g_IPC->GetSharedMem()) {
            // Ensure LUID is published (handles late IPC connection)
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (adapterLuid.LowPart != 0 || adapterLuid.HighPart != 0) {
                    if (g_IPC->GetSharedMem()->GetLuidLowPart() != (int32_t)adapterLuid.LowPart ||
                        g_IPC->GetSharedMem()->GetLuidHighPart() != (int32_t)adapterLuid.HighPart) {
                        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                        g_IPC->GetSharedMem()->SetLuidLowPart(adapterLuid.LowPart);
                        g_IPC->GetSharedMem()->SetLuidHighPart(adapterLuid.HighPart);
                    }
                    g_IPC->GetSharedMem()->SetLuidSourcePid(GetCurrentProcessId());
                }
            }

            usedIPC = UpdateFromHost();
            if (usedIPC && !loggedIPCMode) {
                EarlyLog("SystemMetricsCollector: Using Host (IPC) metrics");
                loggedIPCMode = true;
            }
        }

        if (!usedIPC) {
            static bool loggedFallback = false;
            if (!loggedFallback) {
                EarlyLog(
                    "SystemMetricsCollector: IPC stats not available yet. Waiting "
                    "for Host...");
                loggedFallback = true;
            }
            // Strict Optimization: DO NOT fallback to local PDH/NtQuery.
            // This ensures zero overhead in the game process if the host is
            // dead/disconnected. We simply report 0 until IPC is restored.
        } else {
            // An exact hook LUID can still supply total capacity if the host's
            // transient DXGI query has not published it yet.
            bool needLocalVramTotal = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                needLocalVramTotal = current.vramTotal == 0 && (adapterLuid.LowPart != 0 || adapterLuid.HighPart != 0);
            }
            if (needLocalVramTotal)
                UpdateVRAMTotal();
        }

        // Sleep for 200ms (Faster updates for better stability)
        for (int i = 0; i < 20 && !stopThread.load(std::memory_order_acquire) && !HookIsShuttingDown(); i++) {
            Sleep(10);
        }
    }

    threadRunning = false;
    EarlyLog("SystemMetricsCollector: Background thread exiting");
}

void SystemMetricsCollector::Update() {
    // The host publishes used RAM but the shared-memory format intentionally has
    // no total-RAM field. Query the stable capacity locally once so the overlay
    // can show an accurate "used of total" value without a fabricated fallback.
    bool needRAMCapacity = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        needRAMCapacity = current.ramTotal == 0 && !ramCapacityQueryAttempted;
        ramCapacityQueryAttempted = true;
    }
    if (needRAMCapacity) {
        UpdateRAM();
    }

    UpdateFromHost();
}

void SystemMetricsCollector::SetVRAMTotal(uint64_t totalBytes) {
    std::lock_guard<std::mutex> lock(mutex);
    current.vramTotal = totalBytes;
    EarlyLog("SystemMetricsCollector: VRAM Total set explicitly: %llu MB", totalBytes / (1024 * 1024));
}

void SystemMetricsCollector::UpdateCPU() {
    static NtQuerySystemInformationPtr NtQuerySystemInformation = nullptr;
    static std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> prevInfo;
    static int numProcs = 0;
    static bool initialized = false;

    // One-time initialization
    if (!initialized) {
        HMODULE hNtDll = GetModuleHandleA("ntdll.dll");
        if (hNtDll) {
            NtQuerySystemInformation = (NtQuerySystemInformationPtr)GetProcAddress(hNtDll, "NtQuerySystemInformation");
        }

        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        numProcs = sysInfo.dwNumberOfProcessors;
        prevInfo.resize(numProcs);

        initialized = true;
    }

    if (!NtQuerySystemInformation || numProcs == 0)
        return;

    std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> currInfo(numProcs);
    ULONG len = 0;

    // Query System Processor Performance Information (Class 8)
    NTSTATUS status =
        NtQuerySystemInformation(8, currInfo.data(), sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION) * numProcs, &len);

    if (status >= 0) {  // success
        uint64_t totalBusy = 0;
        uint64_t totalTime = 0;
        float maxCore = 0.0f;

        for (int i = 0; i < numProcs; i++) {
            uint64_t idle = currInfo[i].IdleTime.QuadPart - prevInfo[i].IdleTime.QuadPart;
            uint64_t kernel = currInfo[i].KernelTime.QuadPart - prevInfo[i].KernelTime.QuadPart;
            uint64_t user = currInfo[i].UserTime.QuadPart - prevInfo[i].UserTime.QuadPart;

            // In SystemProcessorPerformanceInformation, KernelTime INCLUDES IdleTime.
            // So:
            // Total Time = KernelTime + UserTime (which is RealKernel + Idle + User)
            // Busy Time = Total Time - IdleTime

            uint64_t total = kernel + user;
            uint64_t busy = (total > idle) ? (total - idle) : 0;

            if (total > 0) {
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                float coreLoad = (float)busy / total * 100.0f;
                if (coreLoad > 100.0f)
                    coreLoad = 100.0f;  // Clamp due to timing jitter
                if (coreLoad > maxCore)
                    maxCore = coreLoad;

                totalBusy += busy;
                totalTime += total;
            }
        }

        if (totalTime > 0) {
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            float cpuUsage = (float)totalBusy / totalTime * 100.0f;
            std::lock_guard<std::mutex> lock(mutex);
            current.cpuUsage = cpuUsage;
            current.cpuMaxCoreUsage = maxCore;
        }

        // Update previous state
        prevInfo = currInfo;
    }
    // EarlyLog("SystemMetricsCollector: UpdateCPU done"); // Commented out to
    // avoid spam
}

void SystemMetricsCollector::UpdateRAM() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        uint64_t used = memInfo.ullTotalPhys - memInfo.ullAvailPhys;
        uint64_t total = memInfo.ullTotalPhys;
        std::lock_guard<std::mutex> lock(mutex);
        current.ramUsed = used;
        current.ramTotal = total;
        // EarlyLog("UpdateRAM: used=%llu total=%llu", used, total); // Uncomment
        // for deep debug
    } else {
        EarlyLog("UpdateRAM: GlobalMemoryStatusEx failed err=%d", GetLastError());
    }
}


SystemMetrics SystemMetricsCollector::GetMetrics() {
    std::lock_guard<std::mutex> lock(mutex);
    return current;
}
