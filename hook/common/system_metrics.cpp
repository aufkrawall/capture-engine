#include "system_metrics.h"
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
    InitD3DKMT();
}

SystemMetricsCollector::~SystemMetricsCollector() {
    if (cpuQuery) {
        PdhCloseQuery((PDH_HQUERY)cpuQuery);
        cpuQuery = nullptr;
    }
}

void SystemMetricsCollector::InitPDH() {
    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryA(NULL, 0, &query) == ERROR_SUCCESS) {
        cpuQuery = (void*)query;
        // Add counter for total processor time
        PDH_HCOUNTER counter = nullptr;
        // Try English first
        if (PdhAddEnglishCounterA(query, "\\Processor(_Total)\\% Processor Time", 0, &counter) == ERROR_SUCCESS) {
            cpuCounter = (void*)counter;
            PdhCollectQueryData(query); // First collection to prime
            pdhInitialized = true;
        } else {
             // Fallback to local
             if (PdhAddCounterA(query, "\\Processor(_Total)\\% Processor Time", 0, &counter) == ERROR_SUCCESS) {
                cpuCounter = (void*)counter;
                PdhCollectQueryData(query);
                pdhInitialized = true;
             }
        }
    }
}

void SystemMetricsCollector::InitD3DKMT() {
    // TODO: Implement D3DKMT for GPU Usage
    d3dkmtInitialized = false;
}

void SystemMetricsCollector::Initialize(int32_t luidLow, int32_t luidHigh) {
    std::lock_guard<std::mutex> lock(mutex);
    adapterLuid.LowPart = luidLow;
    adapterLuid.HighPart = luidHigh;
    
    // Invalidate D3DKMT handle if LUID changed
    hAdapter = 0; 
    d3dkmtInitialized = false;
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
    // VRAM Usage via DXGI
    if (adapterLuid.LowPart == 0 && adapterLuid.HighPart == 0) return;

    IDXGIFactory4* pFactory = nullptr;
    if (CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&pFactory) == S_OK) {
        IDXGIAdapter3* pAdapter = nullptr;
        
        // Find adapter by LUID
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
    
    // GPU Load: Placeholder
    current.gpuUsageValid = false;
    current.gpuUsage = 0.0f;
}

SystemMetrics SystemMetricsCollector::GetMetrics() {
    std::lock_guard<std::mutex> lock(mutex);
    return current;
}
