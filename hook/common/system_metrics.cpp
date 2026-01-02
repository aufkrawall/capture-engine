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
    if (vramQuery) {
        PdhCloseQuery((PDH_HQUERY)vramQuery);
        vramQuery = nullptr;
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
    // 1. GPU Load via PDH Engine counters (System-Wide, filtered by engine type)
    if (!gpuPdhInitialized && gpuQuery == nullptr) {
        PDH_HQUERY query = nullptr;
        if (PdhOpenQueryA(NULL, 0, &query) == ERROR_SUCCESS) {
            gpuQuery = (void*)query;
            PDH_HCOUNTER counter = nullptr;
            PDH_STATUS status = PdhAddEnglishCounterA(query, "\\GPU Engine(*)\\Utilization Percentage", 0, &counter);
            if (status != ERROR_SUCCESS) {
                status = PdhAddCounterA(query, "\\GPU Engine(*)\\Utilization Percentage", 0, &counter);
            }
            if (status == ERROR_SUCCESS) {
                gpuCounter = (void*)counter;
                PdhCollectQueryData(query);
                gpuPdhInitialized = true;
                EarlyLog("GPU Load: Initialized");
            } else {
                PdhCloseQuery(query);
                gpuQuery = nullptr;
            }
        }
    }

    static float smoothedGpuUsage = 0.0f;
    if (gpuPdhInitialized && gpuQuery && gpuCounter) {
        if (PdhCollectQueryData((PDH_HQUERY)gpuQuery) == ERROR_SUCCESS) {
            DWORD bufSize = 0, itemCount = 0;
            PdhGetFormattedCounterArrayA((PDH_HCOUNTER)gpuCounter, PDH_FMT_DOUBLE, &bufSize, &itemCount, NULL);
            if (bufSize > 0) {
                if (bufSize > pdhBufferSize) {
                    void* newBuf = realloc(pdhBuffer, bufSize);
                    if (newBuf) {
                        pdhBuffer = newBuf;
                        pdhBufferSize = bufSize;
                    }
                }
                if (pdhBuffer) {
                    PDH_FMT_COUNTERVALUE_ITEM_A* items = (PDH_FMT_COUNTERVALUE_ITEM_A*)pdhBuffer;
                    if (PdhGetFormattedCounterArrayA((PDH_HCOUNTER)gpuCounter, PDH_FMT_DOUBLE, &pdhBufferSize, &itemCount, items) == ERROR_SUCCESS) {
                        double sum3D = 0, sumCompute = 0, sumVideo = 0, sumVR = 0;
                        for (DWORD i = 0; i < itemCount; i++) {
                            const char* instance = items[i].szName;
                            if (instance && items[i].FmtValue.CStatus == ERROR_SUCCESS) {
                                if (strstr(instance, cachedLuidPart) || strstr(instance, "luid_") == nullptr) {
                                    double val = items[i].FmtValue.doubleValue;
                                    if (strstr(instance, "engtype_3D")) sum3D += val;
                                    else if (strstr(instance, "engtype_Compute")) sumCompute += val;
                                    else if (strstr(instance, "VideoDecode") || strstr(instance, "VideoEncode")) sumVideo += val;
                                    else if (strstr(instance, "engtype_VR")) sumVR += val;
                                }
                            }
                        }
                        
                        // Final Load is the MAX of the core engine categories
                        // Each category is limited to 100% to avoid driver measurement artifacts
                        double maxLoad = std::max({std::min(100.0, sum3D), std::min(100.0, sumCompute), std::min(100.0, sumVideo), std::min(100.0, sumVR)});
                        
                        const float smoothingWeight = 0.65f; // Increased for better "latest information" feel
                        smoothedGpuUsage = smoothedGpuUsage * (1.0f - smoothingWeight) + (float)maxLoad * smoothingWeight;
                        current.gpuUsage = smoothedGpuUsage;
                        current.gpuUsageValid = true;
                    }
                }
            }
        }
    }

    // 2. VRAM Usage via PDH Adapter Memory (System-Wide)
    if (!vramPdhInitialized && vramQuery == nullptr) {
        PDH_HQUERY query = nullptr;
        if (PdhOpenQueryA(NULL, 0, &query) == ERROR_SUCCESS) {
            vramQuery = (void*)query;
            PDH_HCOUNTER counter = nullptr;
            PDH_STATUS status = PdhAddEnglishCounterA(query, "\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &counter);
            if (status != ERROR_SUCCESS) {
                status = PdhAddCounterA(query, "\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &counter);
            }
            if (status == ERROR_SUCCESS) {
                vramCounter = (void*)counter;
                PdhCollectQueryData(query);
                vramPdhInitialized = true;
                EarlyLog("VRAM: Initialized");
            } else {
                PdhCloseQuery(query);
                vramQuery = nullptr;
            }
        }
    }

    static float smoothedVramUsed = 0.0f;
    if (vramPdhInitialized && vramQuery && vramCounter) {
        if (PdhCollectQueryData((PDH_HQUERY)vramQuery) == ERROR_SUCCESS) {
            DWORD bufSize = 0, itemCount = 0;
            PdhGetFormattedCounterArrayA((PDH_HCOUNTER)vramCounter, PDH_FMT_DOUBLE, &bufSize, &itemCount, NULL);
            if (bufSize > 0) {
                static void* vramBuffer = nullptr;
                static DWORD vramBufferSize = 0;
                if (bufSize > vramBufferSize) {
                    void* newBuf = realloc(vramBuffer, bufSize);
                    if (newBuf) { vramBuffer = newBuf; vramBufferSize = bufSize; }
                }
                if (vramBuffer) {
                    PDH_FMT_COUNTERVALUE_ITEM_A* items = (PDH_FMT_COUNTERVALUE_ITEM_A*)vramBuffer;
                    if (PdhGetFormattedCounterArrayA((PDH_HCOUNTER)vramCounter, PDH_FMT_DOUBLE, &vramBufferSize, &itemCount, items) == ERROR_SUCCESS) {
                        for (DWORD i = 0; i < itemCount; i++) {
                            const char* instance = items[i].szName;
                            if (instance && strstr(instance, cachedLuidPart) && items[i].FmtValue.CStatus == ERROR_SUCCESS) {
                                float rawVram = (float)items[i].FmtValue.doubleValue;
                                if (smoothedVramUsed == 0.0f) smoothedVramUsed = rawVram;
                                else smoothedVramUsed = smoothedVramUsed * 0.6f + rawVram * 0.4f;
                                current.vramUsed = (uint64_t)smoothedVramUsed;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    // VRAM Total still via DXGI
    if (adapterLuid.LowPart == 0 && adapterLuid.HighPart == 0) return;
    if (!cachedFactory) CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&cachedFactory);
    if (cachedFactory && !cachedAdapter) {
        IDXGIFactory4* pFactory = (IDXGIFactory4*)cachedFactory;
        IDXGIAdapter1* tempAdapter = nullptr;
        for (UINT i = 0; pFactory->EnumAdapters1(i, &tempAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            tempAdapter->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == adapterLuid.LowPart && desc.AdapterLuid.HighPart == adapterLuid.HighPart) {
                tempAdapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&cachedAdapter);
                current.vramTotal = desc.DedicatedVideoMemory;
                tempAdapter->Release();
                break;
            }
            tempAdapter->Release();
        }
    }
}

SystemMetrics SystemMetricsCollector::GetMetrics() {
    std::lock_guard<std::mutex> lock(mutex);
    return current;
}
