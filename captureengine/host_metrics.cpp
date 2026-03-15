#include "host_metrics.h"
#include <stdio.h>
#include <algorithm>
#include <iostream>
#include <mutex>
#include <thread>
#include "../common/logging.h"

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
        LogInfo("[Metrics] DXGI: No adapter found matching LUID %08lx:%08lx", luidHigh, luidLow);
        lastLoggedMissingDxgiLuid = luid;
    }
    return 0;
}

void UpdateSystemMetrics(SharedMemoryLayout* shm, uint32_t targetPid, int64_t luid) {
    if (!shm)
        return;

    (void)targetPid;

    std::lock_guard<std::mutex> lock(g_MetricsMutex);
    const bool hasValidLuid = (luid != 0);
    g_HostMetrics.Initialize(hasValidLuid);  // Init if needed

    bool debugLogging = shm->GetDebugLogging();

    // --- CPU Load ---
    if (g_HostMetrics.pdhInitialized) {
        if (PdhCollectQueryData(g_HostMetrics.cpuQuery) == ERROR_SUCCESS) {
            PDH_FMT_COUNTERVALUE value;
            if (PdhGetFormattedCounterValue(g_HostMetrics.cpuCounter, PDH_FMT_DOUBLE, NULL, &value) == ERROR_SUCCESS) {
                shm->systemMetrics.cpuUsage.store((float)value.doubleValue, std::memory_order_relaxed);
            }
        }
    }

    // --- RAM Usage ---
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        double usedGB = (double)(memInfo.ullTotalPhys - memInfo.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
        shm->systemMetrics.ramUsage.store((float)usedGB, std::memory_order_relaxed);
    }

    // --- GPU Load & VRAM ---
    // Need to match LUID to PDH instance name
    char luidStrUpper[64];
    char luidStrLower[64];
    uint32_t low = (uint32_t)(luid & 0xFFFFFFFF);
    uint32_t high = (uint32_t)((luid >> 32) & 0xFFFFFFFF);
    snprintf(luidStrUpper, sizeof(luidStrUpper), "luid_0x%08X_0x%08X", high, low);
    snprintf(luidStrLower, sizeof(luidStrLower), "luid_0x%08x_0x%08x", high, low);

    // GPU Load
    if (hasValidLuid && g_HostMetrics.gpuPdhInitialized && g_HostMetrics.gpuCounter) {
        PdhCollectQueryData(g_HostMetrics.gpuQuery);

        DWORD bufSize = 0, itemCount = 0;
        PdhGetFormattedCounterArrayA(g_HostMetrics.gpuCounter, PDH_FMT_DOUBLE, &bufSize, &itemCount, NULL);
        if (bufSize > 0) {
            if (bufSize > g_HostMetrics.pdhBufferSize) {
                void* newBuf = realloc(g_HostMetrics.pdhBuffer, bufSize);
                if (newBuf) {
                    g_HostMetrics.pdhBuffer = newBuf;
                    g_HostMetrics.pdhBufferSize = bufSize;
                } else {
                    // realloc failed - keep using old buffer with old size
                    bufSize = g_HostMetrics.pdhBufferSize;
                }
            }

            if (g_HostMetrics.pdhBuffer) {
                PDH_FMT_COUNTERVALUE_ITEM_A* items = (PDH_FMT_COUNTERVALUE_ITEM_A*)g_HostMetrics.pdhBuffer;
                DWORD actualBufSize = bufSize;
                if (PdhGetFormattedCounterArrayA(g_HostMetrics.gpuCounter, PDH_FMT_DOUBLE, &actualBufSize, &itemCount,
                                                 items) == ERROR_SUCCESS) {
                    double totalLoad = 0;
                    bool foundAny = false;
                    for (DWORD i = 0; i < itemCount; i++) {
                        if (strstr(items[i].szName, luidStrUpper) || strstr(items[i].szName, luidStrLower)) {
                            foundAny = true;
                            // Simplified logic: Just capture basic 3D/Compute loads if
                            // possible, or SUM ALL? For now, mirroring hook logic: Sum all
                            // non-video
                            bool isVideo =
                                strstr(items[i].szName, "VideoDecode") || strstr(items[i].szName, "VideoEncode");
                            if (!isVideo) {
                                totalLoad += items[i].FmtValue.doubleValue;
                            }
                        }
                    }
                    if (debugLogging && !foundAny && g_HostMetrics.lastLoggedMissingGpuLuid != luid) {
                        LogInfo("[Metrics] Warning: No GPU engine found matching LUID %s", luidStrUpper);
                        g_HostMetrics.lastLoggedMissingGpuLuid = luid;
                    }
                    if (totalLoad > 100.0)
                        totalLoad = 100.0;
                    shm->systemMetrics.gpuUsage.store((float)totalLoad, std::memory_order_relaxed);
                }
            }
        }
    }

    // --- Max Core Load (CPU) ---
    // Using NtQuerySystemInformation Class 8
    // (SystemProcessorPerformanceInformation)
    static NtQuerySystemInformationPtr NtQuerySystemInformation = nullptr;
    if (!NtQuerySystemInformation) {
        HMODULE hNtDll = GetModuleHandleA("ntdll.dll");
        if (hNtDll)
            NtQuerySystemInformation = (NtQuerySystemInformationPtr)GetProcAddress(hNtDll, "NtQuerySystemInformation");
    }

    static std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> prevCpuInfo;
    static int numProcs = 0;
    if (numProcs == 0) {
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        numProcs = sysInfo.dwNumberOfProcessors;
        prevCpuInfo.resize(numProcs);
    }

    if (NtQuerySystemInformation && numProcs > 0) {
        std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> currCpuInfo(numProcs);
        ULONG len = 0;
        if (NtQuerySystemInformation(SystemProcessorPerformanceInformation, currCpuInfo.data(),
                                     sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION) * numProcs, &len) >= 0) {
            float maxCore = 0.0f;
            if (!prevCpuInfo.empty() && prevCpuInfo[0].IdleTime.QuadPart != 0) {
                for (int i = 0; i < numProcs; i++) {
                    uint64_t idle = currCpuInfo[i].IdleTime.QuadPart - prevCpuInfo[i].IdleTime.QuadPart;
                    uint64_t kernel = currCpuInfo[i].KernelTime.QuadPart - prevCpuInfo[i].KernelTime.QuadPart;
                    uint64_t user = currCpuInfo[i].UserTime.QuadPart - prevCpuInfo[i].UserTime.QuadPart;
                    uint64_t total = kernel + user;
                    if (total > 0) {
                        float usage = (float)(total - idle) / total * 100.0f;
                        if (usage > maxCore)
                            maxCore = usage;
                    }
                }
            }
            prevCpuInfo = currCpuInfo;
            shm->systemMetrics.maxCoreLoad.store((uint32_t)maxCore, std::memory_order_relaxed);
        }
    }

    // VRAM Usage
    if (hasValidLuid && g_HostMetrics.vramPdhInitialized && g_HostMetrics.vramCounter) {
        PdhCollectQueryData(g_HostMetrics.vramQuery);
        DWORD bufSize = 0, itemCount = 0;
        PdhGetFormattedCounterArrayA(g_HostMetrics.vramCounter, PDH_FMT_LARGE, &bufSize, &itemCount, NULL);
        if (bufSize > 0) {
            std::vector<BYTE> tempBuf(bufSize);
            PDH_FMT_COUNTERVALUE_ITEM_A* items = (PDH_FMT_COUNTERVALUE_ITEM_A*)tempBuf.data();
            if (PdhGetFormattedCounterArrayA(g_HostMetrics.vramCounter, PDH_FMT_LARGE, &bufSize, &itemCount, items) ==
                ERROR_SUCCESS) {
                int64_t totalVRAM = 0;
                bool foundAny = false;
                for (DWORD i = 0; i < itemCount; i++) {
                    if (strstr(items[i].szName, luidStrUpper) || strstr(items[i].szName, luidStrLower)) {
                        foundAny = true;
                        totalVRAM += items[i].FmtValue.largeValue;
                    }
                }
                if (debugLogging && !foundAny && g_HostMetrics.lastLoggedMissingVramLuid != luid) {
                    LogInfo("[Metrics] Warning: No VRAM adapter found matching LUID %s", luidStrUpper);
                    g_HostMetrics.lastLoggedMissingVramLuid = luid;
                }
                double vramMB = (double)totalVRAM / (1024.0 * 1024.0);
                shm->systemMetrics.vramUsage.store((float)vramMB, std::memory_order_relaxed);
            }
        }
    }

    // VRAM Total via DXGI (64-bit host queries for 32-bit processes)
    if (hasValidLuid) {
        uint64_t vramTotal = g_HostMetrics.QueryVRAMTotalFromDXGI((int32_t)low, (int32_t)high);
        if (vramTotal > 0) {
            shm->systemMetrics.vramTotal.store(vramTotal, std::memory_order_relaxed);
            if (debugLogging &&
                (g_HostMetrics.lastLoggedVramTotalLuid != luid || g_HostMetrics.lastLoggedVramTotal != vramTotal)) {
                LogInfo("[Metrics] VRAM Total: %llu MB written to shared memory", vramTotal / (1024 * 1024));
                g_HostMetrics.lastLoggedVramTotalLuid = luid;
                g_HostMetrics.lastLoggedVramTotal = vramTotal;
            }
        }
    }
}
}  // namespace scan_host
