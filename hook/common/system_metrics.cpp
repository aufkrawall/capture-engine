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

SystemMetricsCollector::SystemMetricsCollector() {}

SystemMetricsCollector::~SystemMetricsCollector() {
    stopThread = true;
    if (updateThread.joinable()) {
        updateThread.join();
    }

    if (cpuQuery) {
        PdhCloseQuery((PDH_HQUERY)cpuQuery);
        cpuQuery = nullptr;
    }
    if (gpuQuery) {
        PdhCloseQuery((PDH_HQUERY)gpuQuery);
        gpuQuery = nullptr;
    }
    if (cachedAdapter) {
        ((IUnknown*)cachedAdapter)->Release();
        cachedAdapter = nullptr;
    }
    if (cachedFactory) {
        ((IUnknown*)cachedFactory)->Release();
        cachedFactory = nullptr;
    }
    if (pdhBuffer) {
        free(pdhBuffer);
        pdhBuffer = nullptr;
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
    if (adapterLuid.LowPart == (DWORD)luidLow && adapterLuid.HighPart == (LONG)luidHigh) {
        // Even if LUID matches, ensure the thread is running
        if (!threadRunning) {
            stopThread = false;
            updateThread = std::thread(&SystemMetricsCollector::BackgroundUpdateLoop, this);
            threadRunning = true;
        }
        return; 
    }

    adapterLuid.LowPart = luidLow;
    adapterLuid.HighPart = luidHigh;
    
    // Pre-compute LUID search string for PDH
    snprintf(cachedLuidPart, sizeof(cachedLuidPart), "luid_0x%08X_0x%08X", (unsigned int)luidHigh, (unsigned int)luidLow);
    
    // Invalidate cached DXGI resources
    if (cachedAdapter) {
        ((IUnknown*)cachedAdapter)->Release();
        cachedAdapter = nullptr;
    }
    
    EarlyLog("SystemMetricsCollector: Initialized with LUID %s", cachedLuidPart);

    if (!threadRunning) {
        stopThread = false;
        updateThread = std::thread(&SystemMetricsCollector::BackgroundUpdateLoop, this);
        threadRunning = true;
    }
}

void SystemMetricsCollector::BackgroundUpdateLoop() {
    EarlyLog("SystemMetricsCollector: Background thread started");
    
    // Initial PDH setup if not done
    if (!pdhInitialized) {
        InitPDH();
    }

    while (!stopThread) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            UpdateCPU();
            UpdateRAM();
            UpdateGPU();
        }
        
        // Sleep for 500ms (standard overlay update interval)
        // We can use a smaller interval if we want smoother updates
        for (int i = 0; i < 50 && !stopThread; i++) {
            Sleep(10);
        }
    }
    
    threadRunning = false;
    EarlyLog("SystemMetricsCollector: Background thread exiting");
}

void SystemMetricsCollector::Update() {
    // No-op - background thread handles updates
    // Keeping this to maintain API compatibility with existing hooks
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
                // Resize cached buffer if needed
                if (bufSize > pdhBufferSize) {
                    void* newBuf = realloc(pdhBuffer, bufSize);
                    if (newBuf) {
                        pdhBuffer = newBuf;
                        pdhBufferSize = bufSize;
                    }
                }

                if (pdhBuffer) {
                    PDH_FMT_COUNTERVALUE_ITEM_A* items = (PDH_FMT_COUNTERVALUE_ITEM_A*)pdhBuffer;
                    arrayStatus = PdhGetFormattedCounterArrayA((PDH_HCOUNTER)gpuCounter, PDH_FMT_DOUBLE, &pdhBufferSize, &itemCount, items);
                    if (arrayStatus == ERROR_SUCCESS && itemCount > 0) {
                        double totalUtil = 0.0;
                        int matchedEngines = 0;
                        for (DWORD i = 0; i < itemCount; i++) {
                            const char* instance = items[i].szName;
                            
                            // Log first few instances only once ever
                            static bool instancesLogged = false;
                            if (!instancesLogged && i < 10) {
                                EarlyLog("GPU PDH Instance[%lu]: %s", i, instance ? instance : "NULL");
                                if (i == 9 || i == itemCount - 1) instancesLogged = true;
                            }

                            // Filter by LUID to avoid summing across multiple GPUs
                            if (instance && (strstr(instance, cachedLuidPart) || strstr(instance, "luid_") == nullptr)) {
                                totalUtil += items[i].FmtValue.doubleValue;
                                matchedEngines++;
                            }
                        }
                        
                        if (totalUtil > 100.0) totalUtil = 100.0;
                        
                        const float smoothingWeight = 0.2f;
                        smoothedGpuUsage = smoothedGpuUsage * (1.0f - smoothingWeight) + (float)totalUtil * smoothingWeight;
                        current.gpuUsage = smoothedGpuUsage;
                        current.gpuUsageValid = true;
                        
                        if (logCount < 5) {
                            EarlyLog("GPU PDH: items=%lu, matches=%d, total=%.1f%%, smoothed=%.1f%%, LUID=%s", 
                                     itemCount, matchedEngines, totalUtil, smoothedGpuUsage, cachedLuidPart);
                            logCount++;
                        }
                    } else if (logCount < 3) {
                        EarlyLog("GPU PDH: GetArray failed (0x%08X), items=%lu", arrayStatus, itemCount);
                        logCount++;
                    }
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
                }
                logCount++;
            }
        }
    }
    
    // VRAM Usage via DXGI
    if (adapterLuid.LowPart == 0 && adapterLuid.HighPart == 0) return;

    // Use cached resources if available
    if (!cachedFactory) {
        CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&cachedFactory);
    }

    if (cachedFactory && !cachedAdapter) {
        IDXGIFactory4* pFactory = (IDXGIFactory4*)cachedFactory;
        LUID targetLuid = adapterLuid;
        IDXGIAdapter1* tempAdapter = nullptr;
        for (UINT i = 0; pFactory->EnumAdapters1(i, &tempAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            tempAdapter->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == targetLuid.LowPart && 
                desc.AdapterLuid.HighPart == targetLuid.HighPart) {
                tempAdapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&cachedAdapter);
                tempAdapter->Release();
                break;
            }
            tempAdapter->Release();
        }
    }
    
    if (cachedAdapter) {
        IDXGIAdapter3* pAdapter = (IDXGIAdapter3*)cachedAdapter;
        DXGI_QUERY_VIDEO_MEMORY_INFO videoMemoryInfo;
        if (pAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &videoMemoryInfo) == S_OK) {
            current.vramUsed = videoMemoryInfo.CurrentUsage;
            current.vramTotal = videoMemoryInfo.Budget; 
        } else {
            // If query fails, maybe adapter is stale? Reset for next time.
            pAdapter->Release();
            cachedAdapter = nullptr;
        }
    }
}

SystemMetrics SystemMetricsCollector::GetMetrics() {
    std::lock_guard<std::mutex> lock(mutex);
    return current;
}
