#include "host_metrics.h"
#include <stdio.h>
#include <algorithm>
#include <iostream>
#include <mutex>
#include <thread>
#include "../common/logging.h"
#include "host_metrics_policy.h"
#include "sensor_plugin.h"

// For NTQuerySystemInformation
#include <winternl.h>
#ifdef _MSC_VER
#pragma comment(lib, "ntdll.lib")
#endif

namespace scan_host {

static HostMetricsState g_HostMetrics;
static std::mutex g_MetricsMutex;

// Helper for NTQuerySystemInformation
typedef NTSTATUS(WINAPI* NtQuerySystemInformationPtr)(SYSTEM_INFORMATION_CLASS SystemInformationClass,
                                                      PVOID SystemInformation, ULONG SystemInformationLength,
                                                      PULONG ReturnLength);

void HostMetricsState::Initialize(bool includeGpuMetrics) {
    if (!pdhInitialized) {
        if (PdhOpenQueryA(NULL, 0, &cpuQuery) == ERROR_SUCCESS) {
            // Total CPU
            PdhAddEnglishCounterA(cpuQuery, "\\Processor(_Total)\\% Processor Time", 0, &cpuCounter);
            PdhCollectQueryData(cpuQuery);
            pdhInitialized = true;
        }
    }

    if (!includeGpuMetrics) {
        return;
    }

    if (!gpuPdhInitialized) {
        if (PdhOpenQueryA(NULL, 0, &gpuQuery) == ERROR_SUCCESS) {
            // GPU Engines
            PDH_STATUS status =
                PdhAddEnglishCounterA(gpuQuery, "\\GPU Engine(*)\\Utilization Percentage", 0, &gpuCounter);
            if (status != ERROR_SUCCESS) {
                status = PdhAddCounterA(gpuQuery, "\\GPU Engine(*)\\Utilization Percentage", 0, &gpuCounter);
            }

            if (status == ERROR_SUCCESS) {
                PdhCollectQueryData(gpuQuery);
                gpuPdhInitialized = true;
            } else {
                PdhCloseQuery(gpuQuery);
                gpuQuery = nullptr;
            }
        }
    }

    if (!vramPdhInitialized) {
        if (PdhOpenQueryA(NULL, 0, &vramQuery) == ERROR_SUCCESS) {
            // VRAM Dedicated Usage (only - Total via DXGI instead)
            PDH_STATUS status =
                PdhAddEnglishCounterA(vramQuery, "\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &vramCounter);
            if (status != ERROR_SUCCESS) {
                status = PdhAddCounterA(vramQuery, "\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &vramCounter);
            }
            if (status == ERROR_SUCCESS) {
                PdhCollectQueryData(vramQuery);
                vramPdhInitialized = true;
                LogInfo("[Metrics] VRAM Usage PDH initialized successfully");
            } else {
                LogInfo("[Metrics] VRAM Usage PDH init failed with status: 0x%lX", (unsigned long)status);
                PdhCloseQuery(vramQuery);
                vramQuery = nullptr;
            }
        }
    }

    // Initialize DXGI factory for VRAM Total queries
    if (!dxgiFactory) {
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&dxgiFactory);
        if (SUCCEEDED(hr)) {
            LogInfo("[Metrics] DXGI Factory created for VRAM Total queries");
        } else {
            LogInfo("[Metrics] Failed to create DXGI Factory: 0x%lX", (unsigned long)hr);
        }
    }
}

void HostMetricsState::Cleanup() {
    if (cpuQuery)
        PdhCloseQuery(cpuQuery);
    if (gpuQuery)
        PdhCloseQuery(gpuQuery);
    if (vramQuery)
        PdhCloseQuery(vramQuery);
    if (pdhBuffer)
        free(pdhBuffer);
    if (dxgiFactory)
        dxgiFactory->Release();

    cpuQuery = gpuQuery = vramQuery = nullptr;
    dxgiFactory = nullptr;
    pdhInitialized = gpuPdhInitialized = vramPdhInitialized = false;
    cachedVRAMTotal = 0;
    cachedAdapterLuid = {0, 0};
    lastLoggedMissingGpuLuid = 0;
    lastLoggedMissingVramLuid = 0;
    lastLoggedMissingDxgiLuid = 0;
    lastLoggedVramTotalLuid = 0;
    lastLoggedVramTotal = 0;
    processResolvedPid = 0;
    processResolvedLuid = 0;
    lastPublishedPid = 0;
    lastPublishedLuid = 0;
    lastPublishedAdapterSource = 0;
}

uint64_t HostMetricsState::QueryVRAMTotalFromDXGI(int32_t luidLow, int32_t luidHigh) {
    if (!dxgiFactory)
        return 0;

    // Check if we have cached value for this LUID
    if (cachedVRAMTotal > 0 && cachedAdapterLuid.LowPart == (DWORD)luidLow &&
        cachedAdapterLuid.HighPart == (LONG)luidHigh) {
        return cachedVRAMTotal;
    }

    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            if (desc.AdapterLuid.LowPart == (DWORD)luidLow && desc.AdapterLuid.HighPart == (LONG)luidHigh) {
                cachedVRAMTotal = desc.DedicatedVideoMemory;
                cachedAdapterLuid = desc.AdapterLuid;
                LogInfo("[Metrics] DXGI: Found VRAM Total %llu MB for LUID %08lx:%08lx",
                        cachedVRAMTotal / (1024 * 1024), desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
                adapter->Release();
                return cachedVRAMTotal;
            }
        }
        adapter->Release();
    }

    int64_t luid = (static_cast<int64_t>(static_cast<uint32_t>(luidHigh)) << 32) | static_cast<uint32_t>(luidLow);
    if (lastLoggedMissingDxgiLuid != luid) {
        LogInfo("[Metrics] DXGI: No adapter found matching LUID %08x:%08x", static_cast<uint32_t>(luidHigh),
                static_cast<uint32_t>(luidLow));
        lastLoggedMissingDxgiLuid = luid;
    }
    return 0;
}

namespace {

static_assert(static_cast<uint32_t>(metrics_policy::AdapterResolutionSource::HookLuid) ==
              SYSTEM_METRICS_ADAPTER_HOOK_LUID);
static_assert(static_cast<uint32_t>(metrics_policy::AdapterResolutionSource::ProcessGpuEngine) ==
              SYSTEM_METRICS_ADAPTER_PROCESS_ENGINE);
static_assert(static_cast<uint32_t>(metrics_policy::AdapterResolutionSource::RetainedProcessGpuEngine) ==
              SYSTEM_METRICS_ADAPTER_RETAINED_PROCESS_ENGINE);
static_assert(static_cast<uint32_t>(metrics_policy::AdapterResolutionSource::CaptureDeviceLuid) ==
              SYSTEM_METRICS_ADAPTER_CAPTURE_DEVICE);

struct GpuEngineValue {
    int64_t adapterLuid = 0;
    double utilization = 0.0;
    bool videoEngine = false;
    bool valueValid = false;
};

bool IsPdhValueValid(DWORD status) {
    return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA;
}

bool ReadGpuEngineValues(HostMetricsState& state, std::vector<GpuEngineValue>& values,
                         std::vector<metrics_policy::GpuEngineSample>& processSamples) {
    if (!state.gpuPdhInitialized || !state.gpuQuery || !state.gpuCounter ||
        PdhCollectQueryData(state.gpuQuery) != ERROR_SUCCESS) {
        return false;
    }

    DWORD requiredBytes = 0;
    DWORD itemCount = 0;
    const PDH_STATUS sizingStatus =
        PdhGetFormattedCounterArrayA(state.gpuCounter, PDH_FMT_DOUBLE, &requiredBytes, &itemCount, nullptr);
    if (sizingStatus != static_cast<PDH_STATUS>(PDH_MORE_DATA) && sizingStatus != ERROR_SUCCESS)
        return false;
    if (requiredBytes == 0)
        return true;

    if (requiredBytes > state.pdhBufferSize) {
        void* replacement = realloc(state.pdhBuffer, requiredBytes);
        if (!replacement)
            return false;
        state.pdhBuffer = replacement;
        state.pdhBufferSize = requiredBytes;
    }

    DWORD availableBytes = state.pdhBufferSize;
    auto* items = static_cast<PDH_FMT_COUNTERVALUE_ITEM_A*>(state.pdhBuffer);
    if (PdhGetFormattedCounterArrayA(state.gpuCounter, PDH_FMT_DOUBLE, &availableBytes, &itemCount, items) !=
        ERROR_SUCCESS) {
        return false;
    }

    values.reserve(itemCount);
    processSamples.reserve(itemCount);
    for (DWORD i = 0; i < itemCount; ++i) {
        if (!items[i].szName)
            continue;
        int64_t itemLuid = 0;
        if (!metrics_policy::ParseLuid(items[i].szName, itemLuid))
            continue;

        const bool valueValid = IsPdhValueValid(items[i].FmtValue.CStatus);
        const double utilization = valueValid ? items[i].FmtValue.doubleValue : 0.0;
        values.push_back({itemLuid, utilization, metrics_policy::IsVideoEngine(items[i].szName), valueValid});

        metrics_policy::GpuEngineSample processSample;
        if (metrics_policy::ParseGpuEngineSample(items[i].szName, utilization, processSample))
            processSamples.push_back(processSample);
    }
    return true;
}

bool ReadVramUsage(HostMetricsState& state, int64_t adapterLuid, uint64_t& usedBytes) {
    usedBytes = 0;
    if (!state.vramPdhInitialized || !state.vramQuery || !state.vramCounter || adapterLuid == 0 ||
        PdhCollectQueryData(state.vramQuery) != ERROR_SUCCESS) {
        return false;
    }

    DWORD requiredBytes = 0;
    DWORD itemCount = 0;
    const PDH_STATUS sizingStatus =
        PdhGetFormattedCounterArrayA(state.vramCounter, PDH_FMT_LARGE, &requiredBytes, &itemCount, nullptr);
    if (sizingStatus != static_cast<PDH_STATUS>(PDH_MORE_DATA) && sizingStatus != ERROR_SUCCESS)
        return false;
    if (requiredBytes == 0)
        return false;

    std::vector<BYTE> storage(requiredBytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_A*>(storage.data());
    if (PdhGetFormattedCounterArrayA(state.vramCounter, PDH_FMT_LARGE, &requiredBytes, &itemCount, items) !=
        ERROR_SUCCESS) {
        return false;
    }

    uint64_t total = 0;
    bool foundValidValue = false;
    for (DWORD i = 0; i < itemCount; ++i) {
        int64_t itemLuid = 0;
        if (!items[i].szName || !IsPdhValueValid(items[i].FmtValue.CStatus) ||
            !metrics_policy::ParseLuid(items[i].szName, itemLuid) || itemLuid != adapterLuid) {
            continue;
        }
        foundValidValue = true;
        if (items[i].FmtValue.largeValue > 0)
            total += static_cast<uint64_t>(items[i].FmtValue.largeValue);
    }
    usedBytes = total;
    return foundValidValue;
}

const char* AdapterSourceName(metrics_policy::AdapterResolutionSource source) {
    switch (source) {
        case metrics_policy::AdapterResolutionSource::HookLuid:
            return "hook LUID";
        case metrics_policy::AdapterResolutionSource::ProcessGpuEngine:
            return "target PID GPU Engine";
        case metrics_policy::AdapterResolutionSource::RetainedProcessGpuEngine:
            return "retained target PID GPU Engine";
        case metrics_policy::AdapterResolutionSource::CaptureDeviceLuid:
            return "media capture-device LUID";
        default:
            return "unavailable";
    }
}

uint32_t AdapterSourceClass(metrics_policy::AdapterResolutionSource source) {
    if (source == metrics_policy::AdapterResolutionSource::HookLuid)
        return 1;
    if (source == metrics_policy::AdapterResolutionSource::CaptureDeviceLuid)
        return 4;
    return source == metrics_policy::AdapterResolutionSource::Unavailable ? 0 : 2;
}

}  // namespace

void UpdateSystemMetrics(SharedMemoryLayout* shm, uint32_t targetPid, int64_t knownLuid,
                         metrics_policy::AdapterResolutionSource knownSource,
                         const ce::hardware_sensors::HardwareSensorSnapshot& hardwareSensors) {
    if (!shm)
        return;

    std::lock_guard<std::mutex> lock(g_MetricsMutex);
    g_HostMetrics.Initialize(targetPid != 0 || knownLuid != 0);
    const bool debugLogging = shm->GetDebugLogging();

    float cpuUsage = shm->systemMetrics.cpuUsage.load(std::memory_order_relaxed);
    float ramUsage = shm->systemMetrics.ramUsage.load(std::memory_order_relaxed);
    uint32_t maxCoreLoad = shm->systemMetrics.maxCoreLoad.load(std::memory_order_relaxed);

    if (g_HostMetrics.pdhInitialized && PdhCollectQueryData(g_HostMetrics.cpuQuery) == ERROR_SUCCESS) {
        PDH_FMT_COUNTERVALUE value = {};
        if (PdhGetFormattedCounterValue(g_HostMetrics.cpuCounter, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS &&
            IsPdhValueValid(value.CStatus)) {
            cpuUsage = static_cast<float>(value.doubleValue);
        }
    }

    MEMORYSTATUSEX memInfo = {};
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        ramUsage = static_cast<float>(static_cast<double>(memInfo.ullTotalPhys - memInfo.ullAvailPhys) /
                                      (1024.0 * 1024.0 * 1024.0));
    }

    std::vector<GpuEngineValue> gpuValues;
    std::vector<metrics_policy::GpuEngineSample> processSamples;
    const bool gpuCounterRead = ReadGpuEngineValues(g_HostMetrics, gpuValues, processSamples);

    if (g_HostMetrics.processResolvedPid != targetPid) {
        g_HostMetrics.processResolvedPid = targetPid;
        g_HostMetrics.processResolvedLuid = 0;
    }
    const metrics_policy::AdapterResolution adapter = metrics_policy::ResolveAdapterLuid(
        knownLuid, targetPid, processSamples, g_HostMetrics.processResolvedLuid, knownSource);
    if (adapter.source == metrics_policy::AdapterResolutionSource::ProcessGpuEngine && adapter.adapterLuid != 0)
        g_HostMetrics.processResolvedLuid = adapter.adapterLuid;

    const uint32_t sourceClass = AdapterSourceClass(adapter.source);
    if (g_HostMetrics.lastPublishedPid != targetPid || g_HostMetrics.lastPublishedLuid != adapter.adapterLuid ||
        g_HostMetrics.lastPublishedAdapterSource != sourceClass) {
        if (adapter.adapterLuid != 0) {
            LogInfo("[Metrics] Adapter resolved: gamePid=%u luid=0x%llX source=%s", targetPid, adapter.adapterLuid,
                    AdapterSourceName(adapter.source));
        } else {
            LogInfo("[Metrics] Adapter unresolved: gamePid=%u; waiting for hook or process GPU-engine evidence",
                    targetPid);
        }
        g_HostMetrics.lastPublishedPid = targetPid;
        g_HostMetrics.lastPublishedLuid = adapter.adapterLuid;
        g_HostMetrics.lastPublishedAdapterSource = sourceClass;
    }

    double totalGpuLoad = 0.0;
    bool gpuUsageValid = false;
    if (gpuCounterRead && adapter.adapterLuid != 0) {
        for (const GpuEngineValue& value : gpuValues) {
            if (value.adapterLuid != adapter.adapterLuid || !value.valueValid)
                continue;
            gpuUsageValid = true;
            if (!value.videoEngine)
                totalGpuLoad += value.utilization;
        }
    }
    totalGpuLoad = (std::max)(0.0, (std::min)(100.0, totalGpuLoad));

    static NtQuerySystemInformationPtr NtQuerySystemInformation = nullptr;
    if (!NtQuerySystemInformation) {
        HMODULE hNtDll = GetModuleHandleA("ntdll.dll");
        if (hNtDll)
            NtQuerySystemInformation =
                reinterpret_cast<NtQuerySystemInformationPtr>(GetProcAddress(hNtDll, "NtQuerySystemInformation"));
    }

    static std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> prevCpuInfo;
    static int numProcs = 0;
    if (numProcs == 0) {
        SYSTEM_INFO sysInfo = {};
        GetSystemInfo(&sysInfo);
        numProcs = static_cast<int>(sysInfo.dwNumberOfProcessors);
        prevCpuInfo.resize(numProcs);
    }

    if (NtQuerySystemInformation && numProcs > 0) {
        std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> currCpuInfo(numProcs);
        ULONG len = 0;
        if (NtQuerySystemInformation(SystemProcessorPerformanceInformation, currCpuInfo.data(),
                                     sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION) * numProcs, &len) >= 0) {
            float currentMaxCoreLoad = 0.0f;
            if (!prevCpuInfo.empty() && prevCpuInfo[0].IdleTime.QuadPart != 0) {
                for (int i = 0; i < numProcs; ++i) {
                    const uint32_t usage = metrics_policy::CalculateProcessorUsagePercent(
                        static_cast<uint64_t>(prevCpuInfo[i].IdleTime.QuadPart),
                        static_cast<uint64_t>(prevCpuInfo[i].KernelTime.QuadPart),
                        static_cast<uint64_t>(prevCpuInfo[i].UserTime.QuadPart),
                        static_cast<uint64_t>(currCpuInfo[i].IdleTime.QuadPart),
                        static_cast<uint64_t>(currCpuInfo[i].KernelTime.QuadPart),
                        static_cast<uint64_t>(currCpuInfo[i].UserTime.QuadPart));
                    currentMaxCoreLoad = (std::max)(currentMaxCoreLoad, static_cast<float>(usage));
                }
            }
            prevCpuInfo = currCpuInfo;
            maxCoreLoad = static_cast<uint32_t>((std::min)(100.0f, currentMaxCoreLoad));
        }
    }

    const uint32_t low = static_cast<uint32_t>(adapter.adapterLuid & 0xFFFFFFFFull);
    const uint32_t high = static_cast<uint32_t>((static_cast<uint64_t>(adapter.adapterLuid) >> 32) & 0xFFFFFFFFull);
    uint64_t vramTotal = 0;
    if (adapter.adapterLuid != 0)
        vramTotal = g_HostMetrics.QueryVRAMTotalFromDXGI(static_cast<int32_t>(low), static_cast<int32_t>(high));

    uint64_t vramUsed = 0;
    const bool vramCounterRead = ReadVramUsage(g_HostMetrics, adapter.adapterLuid, vramUsed);

    uint32_t validity = 0;
    if (gpuUsageValid)
        validity |= SYSTEM_METRIC_GPU_USAGE_VALID;
    if (vramCounterRead && adapter.adapterLuid != 0)
        validity |= SYSTEM_METRIC_VRAM_USAGE_VALID;
    if (vramTotal > 0)
        validity |= SYSTEM_METRIC_VRAM_TOTAL_VALID;
    if (hardwareSensors.cpuTemperature.valid)
        validity |= SYSTEM_METRIC_CPU_TEMPERATURE_VALID;
    if (hardwareSensors.gpuTemperature.valid)
        validity |= SYSTEM_METRIC_GPU_TEMPERATURE_VALID;
    if (hardwareSensors.cpuPackagePower.valid)
        validity |= SYSTEM_METRIC_CPU_PACKAGE_POWER_VALID;
    if (hardwareSensors.gpuPackagePower.valid)
        validity |= SYSTEM_METRIC_GPU_PACKAGE_POWER_VALID;
    if (hardwareSensors.gpuFan.valid)
        validity |= SYSTEM_METRIC_GPU_FAN_VALID;

    auto& published = shm->systemMetrics;
    published.publicationSequence.fetch_add(1, std::memory_order_acq_rel);
    if (published.sourcePid.load(std::memory_order_acquire) != targetPid)
        published.validityMask.store(0, std::memory_order_release);
    published.cpuUsage.store(cpuUsage, std::memory_order_relaxed);
    published.ramUsage.store(ramUsage, std::memory_order_relaxed);
    published.maxCoreLoad.store(maxCoreLoad, std::memory_order_relaxed);
    published.gpuUsage.store(static_cast<float>(totalGpuLoad), std::memory_order_relaxed);
    published.vramUsage.store(static_cast<float>(static_cast<double>(vramUsed) / (1024.0 * 1024.0)),
                              std::memory_order_relaxed);
    published.vramTotal.store(vramTotal, std::memory_order_relaxed);
    published.cpuTemperatureC.store(hardwareSensors.cpuTemperature.value, std::memory_order_relaxed);
    published.gpuTemperatureC.store(hardwareSensors.gpuTemperature.value, std::memory_order_relaxed);
    published.cpuPackagePowerW.store(hardwareSensors.cpuPackagePower.value, std::memory_order_relaxed);
    published.gpuPackagePowerW.store(hardwareSensors.gpuPackagePower.value, std::memory_order_relaxed);
    published.gpuFanRpm.store(hardwareSensors.gpuFan.value, std::memory_order_relaxed);
    published.adapterLuidLow.store(static_cast<int32_t>(low), std::memory_order_relaxed);
    published.adapterLuidHigh.store(static_cast<int32_t>(high), std::memory_order_relaxed);
    published.adapterSource.store(static_cast<uint32_t>(adapter.source), std::memory_order_relaxed);
    published.sourcePid.store(targetPid, std::memory_order_relaxed);
    published.validityMask.store(validity, std::memory_order_release);
    published.publicationSequence.fetch_add(1, std::memory_order_release);

    if (debugLogging && vramTotal > 0 &&
        (g_HostMetrics.lastLoggedVramTotalLuid != adapter.adapterLuid ||
         g_HostMetrics.lastLoggedVramTotal != vramTotal)) {
        LogInfo("[Metrics] VRAM Total: %llu MB written to shared memory", vramTotal / (1024 * 1024));
        g_HostMetrics.lastLoggedVramTotalLuid = adapter.adapterLuid;
        g_HostMetrics.lastLoggedVramTotal = vramTotal;
    }
}
}  // namespace scan_host
