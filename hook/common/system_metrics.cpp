#include "system_metrics.h"
#include "hook_common.h"
#include <Pdh.h>
#include <PdhMsg.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <cstdio>

// PDH Lib is linked via build.py

SystemMetricsCollector& SystemMetricsCollector::Get() {
    static SystemMetricsCollector instance;
    return instance;
}

SystemMetricsCollector::SystemMetricsCollector() {
    InitPDH();
}

SystemMetricsCollector::~SystemMetricsCollector() {
    if (cpuQuery) {
        PdhCloseQuery((PDH_HQUERY)cpuQuery);
        cpuQuery = nullptr;
    }
    if (gpuQuery) {
        PdhCloseQuery((PDH_HQUERY)gpuQuery);
        gpuQuery = nullptr;
    }
}

void SystemMetricsCollector::InitPDH() {
    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryA(NULL, 0, &query) == ERROR_SUCCESS) {
        cpuQuery = (void*)query;
        PDH_HCOUNTER counter = nullptr;
        if (PdhAddEnglishCounterA(query, "\\Processor(_Total)\\% Processor Time", 0, &counter) == ERROR_SUCCESS) {
            cpuCounter = (void*)counter;
            PdhCollectQueryData(query);
            pdhInitialized = true;
        } else {
             if (PdhAddCounterA(query, "\\Processor(_Total)\\% Processor Time", 0, &counter) == ERROR_SUCCESS) {
                cpuCounter = (void*)counter;
                PdhCollectQueryData(query);
                pdhInitialized = true;
             }
        }
    }
}

void SystemMetricsCollector::Initialize(int32_t luidLow, int32_t luidHigh) {
    std::lock_guard<std::mutex> lock(mutex);
    adapterLuid.LowPart = luidLow;
    adapterLuid.HighPart = luidHigh;
}

void SystemMetricsCollector::Update() {
    std::lock_guard<std::mutex> lock(mutex);
    UpdateCPU();
    UpdateRAM();
    UpdateGPU();
}

void SystemMetricsCollector::UpdateCPU() {
    if (!pdhInitialized) return;

    if (PdhCollectQueryData((PDH_HQUERY)cpuQuery) == ERROR_SUCCESS) {
        PDH_FMT_COUNTERVALUE displayValue;
        if (PdhGetFormattedCounterValue((PDH_HCOUNTER)cpuCounter, PDH_FMT_DOUBLE, NULL, &displayValue) == ERROR_SUCCESS) {
            current.cpuUsage = (float)displayValue.doubleValue;
        }
    }
}

void SystemMetricsCollector::UpdateRAM() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        current.ramUsed = memInfo.ullTotalPhys - memInfo.ullAvailPhys;
        current.ramTotal = memInfo.ullTotalPhys;
    }
}

void SystemMetricsCollector::UpdateGPU() {
    // Initialize GPU PDH counters if not done yet
    // Note: This is called from Update() which already holds the mutex
    if (!gpuPdhInitialized && gpuQuery == nullptr) {
        PDH_HQUERY query = nullptr;
        PDH_STATUS openStatus = PdhOpenQueryA(NULL, 0, &query);
        if (openStatus == ERROR_SUCCESS) {
            gpuQuery = (void*)query;
            
            PDH_HCOUNTER counter = nullptr;
            PDH_STATUS status;
            
            // Windows Task Manager uses GPU Engine utilization
            // Use wildcard to sum all GPU engines
            status = PdhAddEnglishCounterA(query, "\\GPU Engine(*)\\Utilization Percentage", 0, &counter);
            
            if (status == ERROR_SUCCESS) {
                gpuCounter = (void*)counter;
                // Need to collect data twice for rate-based counters
                PdhCollectQueryData(query);
                Sleep(100); // Small delay for first sample
                PdhCollectQueryData(query);
                gpuPdhInitialized = true;
                EarlyLog("GPU PDH: Initialized with GPU Engine wildcard");
            } else {
                EarlyLog("GPU PDH: GPU Engine failed (0x%08X), trying localized...", status);
                // Try localized counter names
                status = PdhAddCounterA(query, "\\GPU Engine(*)\\Utilization Percentage", 0, &counter);
                if (status == ERROR_SUCCESS) {
                    gpuCounter = (void*)counter;
                    PdhCollectQueryData(query);
                    Sleep(100);
                    PdhCollectQueryData(query);
                    gpuPdhInitialized = true;
                    EarlyLog("GPU PDH: Initialized with localized counter");
                } else {
                    EarlyLog("GPU PDH: All attempts failed (0x%08X)", status);
                    PdhCloseQuery(query);
                    gpuQuery = nullptr;
                }
            }
        } else {
            EarlyLog("GPU PDH: PdhOpenQuery failed (0x%08X)", openStatus);
        }
    }
    
    // Update GPU usage via PDH - wildcard counters need array retrieval
    static int logCount = 0;
    static float smoothedGpuUsage = 0.0f; // Smoothed value for stable display
    if (gpuPdhInitialized && gpuQuery && gpuCounter) {
        PDH_STATUS collectStatus = PdhCollectQueryData((PDH_HQUERY)gpuQuery);
        if (collectStatus == ERROR_SUCCESS) {
            // Wildcard counters return multiple values - need array retrieval
            DWORD bufSize = 0;
            DWORD itemCount = 0;
            PDH_STATUS arrayStatus = PdhGetFormattedCounterArrayA((PDH_HCOUNTER)gpuCounter, PDH_FMT_DOUBLE, &bufSize, &itemCount, NULL);
            
            if (arrayStatus == PDH_MORE_DATA && bufSize > 0) {
                PDH_FMT_COUNTERVALUE_ITEM_A* items = (PDH_FMT_COUNTERVALUE_ITEM_A*)malloc(bufSize);
                if (items) {
                    arrayStatus = PdhGetFormattedCounterArrayA((PDH_HCOUNTER)gpuCounter, PDH_FMT_DOUBLE, &bufSize, &itemCount, items);
                    if (arrayStatus == ERROR_SUCCESS && itemCount > 0) {
                        // Sum all engine utilizations for the active adapter
                        // Instance names look like: pid_11308_luid_0x00000000_0x0000C5C8_phys_0_eng_0_engtype_3D
                        // PDH uses 0x prefix for hex values in instance names
                        char luidPart[64];
                        snprintf(luidPart, sizeof(luidPart), "luid_0x%08X_0x%08X", (unsigned int)adapterLuid.HighPart, (unsigned int)adapterLuid.LowPart);

                        double totalUtil = 0.0;
                        int matchedEngines = 0;
                        for (DWORD i = 0; i < itemCount; i++) {
                            const char* instance = items[i].szName;
                            
                            // Log first few instances to see the format
                            if (logCount < 1 && i < 5) {
                                EarlyLog("GPU PDH Instance[%lu]: %s", i, instance ? instance : "NULL");
                            }

                            // Filter by LUID to avoid summing across multiple GPUs
                            if (instance && (strstr(instance, luidPart) || strstr(instance, "luid_") == nullptr)) {
                                totalUtil += items[i].FmtValue.doubleValue;
                                matchedEngines++;
                            }
                        }
                        
                        // Cap at 100% (GPU can't be more than 100% utilized)
                        if (totalUtil > 100.0) totalUtil = 100.0;
                        
                        // Apply exponential smoothing (Stronger weight for stability if fluctuating)
                        const float smoothingWeight = 0.2f; // 20% new, 80% old
                        smoothedGpuUsage = smoothedGpuUsage * (1.0f - smoothingWeight) + (float)totalUtil * smoothingWeight;
                        current.gpuUsage = smoothedGpuUsage;
                        current.gpuUsageValid = true;
                        
                        // Log for debugging
                        if (logCount < 5) {
                            EarlyLog("GPU PDH: items=%lu, matches=%d, total=%.1f%%, smoothed=%.1f%%, LUID=%s", 
                                     itemCount, matchedEngines, totalUtil, smoothedGpuUsage, luidPart);
                            logCount++;
                        }
                    } else if (logCount < 3) {
                        EarlyLog("GPU PDH: GetArray failed (0x%08X), items=%lu", arrayStatus, itemCount);
                        logCount++;
                    }
                    free(items);
                }
            } else if (logCount < 3) {
                // Try single value as fallback
                PDH_FMT_COUNTERVALUE displayValue;
                PDH_STATUS singleStatus = PdhGetFormattedCounterValue((PDH_HCOUNTER)gpuCounter, PDH_FMT_DOUBLE, NULL, &displayValue);
                if (singleStatus == ERROR_SUCCESS) {
                    const float smoothingWeight = 0.3f;
                    smoothedGpuUsage = smoothedGpuUsage * (1.0f - smoothingWeight) + (float)displayValue.doubleValue * smoothingWeight;
                    current.gpuUsage = smoothedGpuUsage;
                    current.gpuUsageValid = true;
                    EarlyLog("GPU PDH: Single value=%.1f%%, smoothed=%.1f%%", displayValue.doubleValue, smoothedGpuUsage);
                } else {
                    EarlyLog("GPU PDH: Both array(0x%08X) and single(0x%08X) failed", arrayStatus, singleStatus);
                }
                logCount++;
            }
        }
    }
    
    // VRAM Usage via DXGI (unchanged)
    if (adapterLuid.LowPart == 0 && adapterLuid.HighPart == 0) return;

    IDXGIFactory4* pFactory = nullptr;
    if (CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&pFactory) == S_OK) {
        IDXGIAdapter3* pAdapter = nullptr;
        LUID targetLuid = adapterLuid;
        IDXGIAdapter1* tempAdapter = nullptr;
        for (UINT i = 0; pFactory->EnumAdapters1(i, &tempAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            tempAdapter->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == targetLuid.LowPart && 
                desc.AdapterLuid.HighPart == targetLuid.HighPart) {
                tempAdapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&pAdapter);
                tempAdapter->Release();
                break;
            }
            tempAdapter->Release();
        }
        
        if (pAdapter) {
            DXGI_QUERY_VIDEO_MEMORY_INFO videoMemoryInfo;
            if (pAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &videoMemoryInfo) == S_OK) {
                current.vramUsed = videoMemoryInfo.CurrentUsage;
                current.vramTotal = videoMemoryInfo.Budget; 
            }
            pAdapter->Release();
        }
        pFactory->Release();
    }
}

SystemMetrics SystemMetricsCollector::GetMetrics() {
    std::lock_guard<std::mutex> lock(mutex);
    return current;
}
