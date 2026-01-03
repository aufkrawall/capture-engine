#include "system_metrics.h"
#include "hook_common.h"
#include <Pdh.h>
#include <PdhMsg.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>

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

typedef NTSTATUS (WINAPI *NtQuerySystemInformationPtr)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
);


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
    static bool loggedIPCMode = false;
    
    while (!stopThread) {
        bool usedIPC = false;
        
        // Prioritize IPC (Host-provided) metrics
        if (g_IPC && g_IPC->GetSharedMem()) {
            auto& shm = g_IPC->GetSharedMem()->systemMetrics;
            float cpu = shm.cpuUsage.load(std::memory_order_relaxed);
            // Heuristic: if CPU usage > 0, assume host is writing stats
            if (cpu > 0.0f || shm.gpuUsage.load(std::memory_order_relaxed) > 0.0f) {
                if (!loggedIPCMode) {
                    EarlyLog("SystemMetricsCollector: Using Host (IPC) metrics");
                    loggedIPCMode = true;
                }
                std::lock_guard<std::mutex> lock(mutex);
                current.cpuUsage = cpu;
                current.ramUsed = (uint64_t)(shm.ramUsage.load(std::memory_order_relaxed) * 1024.0f * 1024.0f * 1024.0f);
                current.gpuUsage = shm.gpuUsage.load(std::memory_order_relaxed);
                current.vramUsed = (uint64_t)(shm.vramUsage.load(std::memory_order_relaxed) * 1024.0f * 1024.0f);
                
                uint64_t vTotal = shm.vramTotal.load(std::memory_order_relaxed);
                if (vTotal > 0) current.vramTotal = vTotal;
                
                current.cpuMaxCoreUsage = (float)shm.maxCoreLoad.load(std::memory_order_relaxed);
                current.gpuUsageValid = true;
                usedIPC = true;
            }
        }

        if (!usedIPC) {
            static bool loggedFallback = false;
            if (!loggedFallback) {
                EarlyLog("SystemMetricsCollector: IPC stats not available yet, falling back to local");
                loggedFallback = true;
            }

            std::lock_guard<std::mutex> lock(mutex);
            
            // Lazy PDH init if we are falling back to local
            if (!pdhInitialized) {
                InitPDH();
            }

            // Granular logging for fallback loop
            static int loopCount = 0;
            if (loopCount % 50 == 0) EarlyLog("SystemMetricsCollector: Local update starting (CPU)...");
            UpdateCPU();
            if (loopCount % 50 == 0) EarlyLog("SystemMetricsCollector: Local update (RAM)...");
            UpdateRAM();
            if (loopCount % 50 == 0) EarlyLog("SystemMetricsCollector: Local update (GPU)...");
            UpdateGPU();
            if (loopCount % 50 == 0) EarlyLog("SystemMetricsCollector: Local update done.");
            loopCount++;
        }
        
        // Sleep for 200ms (Faster updates for better stability)
        for (int i = 0; i < 20 && !stopThread; i++) {
            Sleep(10);
        }
    }
    
    threadRunning = false;
    EarlyLog("SystemMetricsCollector: Background thread exiting");
}

void SystemMetricsCollector::Update() {
    // Check if we have valid data from Host Process (IPC)
    if (g_IPC && g_IPC->GetSharedMem()) {
        auto& shmMetrics = g_IPC->GetSharedMem()->systemMetrics;
        float cpu = shmMetrics.cpuUsage.load(std::memory_order_relaxed);
        float gpu = shmMetrics.gpuUsage.load(std::memory_order_relaxed);
        
        bool hostProvidingMetrics = (cpu > 0.0f || gpu > 0.0f); // Simple heuristic

        if (hostProvidingMetrics) {
            std::lock_guard<std::mutex> lock(mutex);
            current.cpuUsage = cpu;
            current.ramUsed = (uint64_t)(shmMetrics.ramUsage.load(std::memory_order_relaxed) * 1024.0 * 1024.0 * 1024.0);
            current.gpuUsage = gpu;
            current.vramUsed = (uint64_t)(shmMetrics.vramUsage.load(std::memory_order_relaxed) * 1024.0 * 1024.0);
            
            uint64_t vramTotal = shmMetrics.vramTotal.load(std::memory_order_relaxed);
            if (vramTotal > 0) current.vramTotal = vramTotal;
            
            current.cpuMaxCoreUsage = (float)shmMetrics.maxCoreLoad.load(std::memory_order_relaxed);
            
            // Mark as valid since we got them from host
            current.gpuUsageValid = true; 
            return; // Skip local collection if host is providing
        }
    }
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
        numProcs = sysInfo.dwNumberOfProcessors;
        prevInfo.resize(numProcs);
        
        initialized = true;
    }

    if (!NtQuerySystemInformation || numProcs == 0) return;

    std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> currInfo(numProcs);
    ULONG len = 0;
    
    // Query System Processor Performance Information (Class 8)
    NTSTATUS status = NtQuerySystemInformation(8, currInfo.data(), sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION) * numProcs, &len);

    if (status >= 0) { // success
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
                float coreLoad = (float)busy / total * 100.0f;
                if (coreLoad > 100.0f) coreLoad = 100.0f; // Clamp due to timing jitter
                if (coreLoad > maxCore) maxCore = coreLoad;

                totalBusy += busy;
                totalTime += total;
            }
        }

        if (totalTime > 0) {
            current.cpuUsage = (float)totalBusy / totalTime * 100.0f;
            current.cpuMaxCoreUsage = maxCore;
        }
        
        // Update previous state
        prevInfo = currInfo;
    }
    // EarlyLog("SystemMetricsCollector: UpdateCPU done"); // Commented out to avoid spam
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
    if (strstr(g_ProcessName, "SonsOfTheForest") != nullptr) {
        return;
    }

    // 1. GPU Load via PDH Engine counters (System-Wide, filtered by engine type)
    if (!gpuPdhInitialized && gpuQuery == nullptr) {
        EarlyLog("SystemMetricsCollector: Initializing GPU PDH");
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
        } else {
             EarlyLog("SystemMetricsCollector: GPU PDH Open failed");
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
                        double totalLoad = 0;
                        for (DWORD i = 0; i < itemCount; i++) {
                            const char* instance = items[i].szName;
                            if (instance && items[i].FmtValue.CStatus == ERROR_SUCCESS) {
                                if (strstr(instance, cachedLuidPart) || strstr(instance, "luid_") == nullptr) {
                                    // Robust Aggregation: Sum EVERYTHING for this GPU
                                    // excluding only video recording/decode noise
                                    bool isVideo = strstr(instance, "VideoDecode") || strstr(instance, "VideoEncode") || 
                                                   strstr(instance, "OFA_0") || strstr(instance, "JPEG_Decode");
                                    
                                    if (!isVideo) {
                                        totalLoad += items[i].FmtValue.doubleValue;
                                    }
                                }
                            }
                        }
                        
                        if (totalLoad > 100.0) totalLoad = 100.0;
                        
                        // --- Peak-Hold Filter (1 Second Window) ---
                        // Maintain a history of the last 5 samples (1s @ 200ms update rate).
                        // We report the MAX of these samples to eliminate temporal PDH under-reporting.
                        static float gpuLoadHistory[5] = {0.0f};
                        static int historyIdx = 0;
                        gpuLoadHistory[historyIdx] = (float)totalLoad;
                        historyIdx = (historyIdx + 1) % 5;

                        float peakLoad = 0.0f;
                        for (int i = 0; i < 5; i++) {
                            if (gpuLoadHistory[i] > peakLoad) peakLoad = gpuLoadHistory[i];
                        }
                        
                        // Use responsive smoothing on the peak value for a "liquid" feel
                        const float smoothingWeight = (peakLoad > smoothedGpuUsage) ? 0.65f : 0.4f; 
                        smoothedGpuUsage = smoothedGpuUsage * (1.0f - smoothingWeight) + peakLoad * smoothingWeight;
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
    // If VRAM Total was set explicitly (e.g. by hook on main thread), SKIP this risky background creation!
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (current.vramTotal > 0) return;
    }

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
