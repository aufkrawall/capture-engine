#include "system_metrics.h"
#include <Pdh.h>
#include <PdhMsg.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
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

SystemMetricsCollector& SystemMetricsCollector::Get()
{
    static SystemMetricsCollector instance;
    return instance;
}

SystemMetricsCollector::SystemMetricsCollector()
    : threadRunning(false), stopThread(false), pdhInitialized(false), gpuPdhInitialized(false),
      vramPdhInitialized(false)
{
}

SystemMetricsCollector::~SystemMetricsCollector()
{
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

void SystemMetricsCollector::Shutdown()
{
    // EarlyLog("SystemMetricsCollector: Shutdown() called"); // Avoid logging in shutdown to prevent deadlocks
    stopThread = true;

    // Wait for thread to exit with a timeout
    if (updateThread.joinable()) {
        // Give thread max 500ms to exit gracefully
        auto start = std::chrono::steady_clock::now();
        while (threadRunning.load() && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500)) {
            Sleep(10);
        }

        if (threadRunning.load()) {
            // EarlyLog("SystemMetricsCollector: Thread still running after 500ms, detaching");
            updateThread.detach();  // Don't deadlock - just detach
        } else {
            EarlyLog("SystemMetricsCollector: Thread exited cleanly, joining");
            updateThread.join();
        }
    }
    EarlyLog("SystemMetricsCollector: Shutdown() complete");
}

void SystemMetricsCollector::InitPDH()
{
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

void SystemMetricsCollector::Initialize(int32_t luidLow, int32_t luidHigh)
{
    {
        std::lock_guard<std::mutex> lock(mutex);

        // If LUID matches, just ensure thread is running
        if (adapterLuid.LowPart == (DWORD)luidLow && adapterLuid.HighPart == (LONG)luidHigh) {
            if (!threadRunning) {
                EarlyLog("SystemMetricsCollector: LUID matches but thread not running. Starting...");
                stopThread = false;
                updateThread = std::thread(&SystemMetricsCollector::BackgroundUpdateLoop, this);
                threadRunning = true;
            }
            return;
        }

        // New LUID
        adapterLuid.LowPart = luidLow;
        adapterLuid.HighPart = luidHigh;
        snprintf(cachedLuidPart, sizeof(cachedLuidPart), "luid_0x%08X_0x%08X", (unsigned int)luidHigh,
                 (unsigned int)luidLow);

        // Invalidate cached resources
        if (cachedAdapter) {
            ((IUnknown*)cachedAdapter)->Release();
            cachedAdapter = nullptr;
        }

        // Publish LUID to shared memory for Host (sensors.exe)
        if (g_IPC && g_IPC->GetSharedMem()) {
            g_IPC->GetSharedMem()->luidLowPart = luidLow;
            g_IPC->GetSharedMem()->luidHighPart = luidHigh;
            // EarlyLog("SystemMetricsCollector: Published LUID to Shared Memory: %08X-%08X", luidHigh, luidLow);
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
            EarlyLog("SystemMetricsCollector: Failed to start background thread (Exception)!");
        }
    }
}

void SystemMetricsCollector::BackgroundUpdateLoop()
{
    EarlyLog("SystemMetricsCollector: Background thread started");
    static bool loggedIPCMode = false;

    while (!stopThread) {
        static int loopTrace = 0;
        if (loopTrace % 100 == 0) EarlyLog("SystemMetricsCollector: Loop Iteration %d", loopTrace);
        loopTrace++;

        bool usedIPC = false;

        // Prioritize IPC (Host-provided) metrics
        if (g_IPC && g_IPC->GetSharedMem()) {
            // Ensure LUID is published (handles late IPC connection)
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (g_IPC->GetSharedMem()->luidLowPart != (int32_t)adapterLuid.LowPart) {
                    g_IPC->GetSharedMem()->luidLowPart = adapterLuid.LowPart;
                    g_IPC->GetSharedMem()->luidHighPart = adapterLuid.HighPart;
                }
            }

            auto& shm = g_IPC->GetSharedMem()->systemMetrics;
            float cpu = shm.cpuUsage.load(std::memory_order_relaxed);
            float gpu = shm.gpuUsage.load(std::memory_order_relaxed);
            float vramMB = shm.vramUsage.load(std::memory_order_relaxed);
            // Heuristic: host always provides CPU/RAM; only trust GPU/VRAM when non-zero
            if (cpu > 0.0f) {
                if (!loggedIPCMode) {
                    EarlyLog("SystemMetricsCollector: Using Host (IPC) metrics");
                    loggedIPCMode = true;
                }
                std::lock_guard<std::mutex> lock(mutex);
                current.cpuUsage = cpu;
                current.ramUsed =
                    (uint64_t)(shm.ramUsage.load(std::memory_order_relaxed) * 1024.0f * 1024.0f * 1024.0f);
                if (gpu > 0.0f || vramMB > 0.0f) {
                    current.gpuUsage = gpu;
                    current.vramUsed = (uint64_t)(vramMB * 1024.0f * 1024.0f);
                    current.gpuUsageValid = true;
                    usedIPC = true;
                }

                uint64_t vTotal = shm.vramTotal.load(std::memory_order_relaxed);
                if (vTotal > 0) current.vramTotal = vTotal;

                current.cpuMaxCoreUsage = (float)shm.maxCoreLoad.load(std::memory_order_relaxed);
            }
        }

        if (!usedIPC) {
            static bool loggedFallback = false;
            if (!loggedFallback) {
                EarlyLog("SystemMetricsCollector: IPC stats not available yet. Waiting for Host...");
                loggedFallback = true;
            }
            // Strict Optimization: DO NOT fallback to local PDH/NtQuery.
            // This ensures zero overhead in the game process if the host is dead/disconnected.
            // We simply report 0 until IPC is restored.
        } else {
            // IPC Active: We still need VRAM Total if missing (Host doesn't send it)
            if (current.vramTotal == 0) {
                UpdateVRAMTotal();
            }
        }

        // Sleep for 200ms (Faster updates for better stability)
        for (int i = 0; i < 20 && !stopThread; i++) {
            Sleep(10);
        }
    }

    threadRunning = false;
    EarlyLog("SystemMetricsCollector: Background thread exiting");
}

void SystemMetricsCollector::Update()
{
    // Check if we have valid data from Host Process (IPC)
    if (g_IPC && g_IPC->GetSharedMem()) {
        auto& shmMetrics = g_IPC->GetSharedMem()->systemMetrics;
        float cpu = shmMetrics.cpuUsage.load(std::memory_order_relaxed);
        float gpu = shmMetrics.gpuUsage.load(std::memory_order_relaxed);
        float vramMB = shmMetrics.vramUsage.load(std::memory_order_relaxed);

        if (cpu > 0.0f) {
            std::lock_guard<std::mutex> lock(mutex);
            current.cpuUsage = cpu;
            current.ramUsed =
                (uint64_t)(shmMetrics.ramUsage.load(std::memory_order_relaxed) * 1024.0 * 1024.0 * 1024.0);
            if (gpu > 0.0f || vramMB > 0.0f) {
                current.gpuUsage = gpu;
                current.vramUsed = (uint64_t)(vramMB * 1024.0 * 1024.0);
                current.gpuUsageValid = true;
            }

            uint64_t vramTotal = shmMetrics.vramTotal.load(std::memory_order_relaxed);
            if (vramTotal > 0) current.vramTotal = vramTotal;

            current.cpuMaxCoreUsage = (float)shmMetrics.maxCoreLoad.load(std::memory_order_relaxed);
            return;
        }
    }
}

void SystemMetricsCollector::SetVRAMTotal(uint64_t totalBytes)
{
    std::lock_guard<std::mutex> lock(mutex);
    current.vramTotal = totalBytes;
    EarlyLog("SystemMetricsCollector: VRAM Total set explicitly: %llu MB", totalBytes / (1024 * 1024));
}

void SystemMetricsCollector::UpdateCPU()
{
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
                float coreLoad = (float)busy / total * 100.0f;
                if (coreLoad > 100.0f) coreLoad = 100.0f;  // Clamp due to timing jitter
                if (coreLoad > maxCore) maxCore = coreLoad;

                totalBusy += busy;
                totalTime += total;
            }
        }

        if (totalTime > 0) {
            float cpuUsage = (float)totalBusy / totalTime * 100.0f;
            std::lock_guard<std::mutex> lock(mutex);
            current.cpuUsage = cpuUsage;
            current.cpuMaxCoreUsage = maxCore;
        }

        // Update previous state
        prevInfo = currInfo;
    }
    // EarlyLog("SystemMetricsCollector: UpdateCPU done"); // Commented out to avoid spam
}

void SystemMetricsCollector::UpdateRAM()
{
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        uint64_t used = memInfo.ullTotalPhys - memInfo.ullAvailPhys;
        uint64_t total = memInfo.ullTotalPhys;
        std::lock_guard<std::mutex> lock(mutex);
        current.ramUsed = used;
        current.ramTotal = total;
        // EarlyLog("UpdateRAM: used=%llu total=%llu", used, total); // Uncomment for deep debug
    } else {
        EarlyLog("UpdateRAM: GlobalMemoryStatusEx failed err=%d", GetLastError());
    }
}

void SystemMetricsCollector::UpdateGPU()
{
    if (strstr(g_ProcessName, "SonsOfTheForest") != nullptr) {
        return;
    }

    float newGpuUsage = 0.0f;
    bool newGpuUsageValid = false;
    uint64_t newVramUsed = 0;
    bool haveVramUsed = false;

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
                    if (PdhGetFormattedCounterArrayA((PDH_HCOUNTER)gpuCounter, PDH_FMT_DOUBLE, &pdhBufferSize,
                                                     &itemCount, items) == ERROR_SUCCESS) {
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
                        newGpuUsage = smoothedGpuUsage;
                        newGpuUsageValid = true;
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
                    if (newBuf) {
                        vramBuffer = newBuf;
                        vramBufferSize = bufSize;
                    }
                }
                if (vramBuffer) {
                    PDH_FMT_COUNTERVALUE_ITEM_A* items = (PDH_FMT_COUNTERVALUE_ITEM_A*)vramBuffer;
                    if (PdhGetFormattedCounterArrayA((PDH_HCOUNTER)vramCounter, PDH_FMT_DOUBLE, &vramBufferSize,
                                                     &itemCount, items) == ERROR_SUCCESS) {
                        for (DWORD i = 0; i < itemCount; i++) {
                            const char* instance = items[i].szName;
                            if (instance && strstr(instance, cachedLuidPart) &&
                                items[i].FmtValue.CStatus == ERROR_SUCCESS) {
                                float rawVram = (float)items[i].FmtValue.doubleValue;
                                if (smoothedVramUsed == 0.0f)
                                    smoothedVramUsed = rawVram;
                                else
                                    smoothedVramUsed = smoothedVramUsed * 0.6f + rawVram * 0.4f;
                                newVramUsed = (uint64_t)smoothedVramUsed;
                                haveVramUsed = true;
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
    UpdateVRAMTotal();

    // Apply the computed GPU/VRAM values (short lock, no PDH/DXGI inside the lock)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (newGpuUsageValid) {
            current.gpuUsage = newGpuUsage;
            current.gpuUsageValid = true;
        }
        if (haveVramUsed) {
            current.vramUsed = newVramUsed;
        }
    }
}

void SystemMetricsCollector::UpdateVRAMTotal()
{
    static bool loggedEntry = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!loggedEntry) {
            EarlyLog("UpdateVRAMTotal: Entry. current.vramTotal=%llu, LUID=%08x:%08x", current.vramTotal,
                     adapterLuid.HighPart, adapterLuid.LowPart);
            loggedEntry = true;
        }

        if (current.vramTotal > 0) {
            return;  // Already have it.
        }
    }

    if (adapterLuid.LowPart == 0 && adapterLuid.HighPart == 0) return;
    if (!cachedFactory) CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&cachedFactory);
    if (cachedFactory && !cachedAdapter) {
        IDXGIFactory4* pFactory = (IDXGIFactory4*)cachedFactory;
        IDXGIAdapter1* tempAdapter = nullptr;
        static bool s_LoggedEnum = false;

        // Log once per detection attempt
        if (adapterLuid.LowPart != 0 || adapterLuid.HighPart != 0) {
            if (!s_LoggedEnum)
                EarlyLog("UpdateGPU: Searching for LUID %08x:%08x", adapterLuid.HighPart, adapterLuid.LowPart);
        }

        for (UINT i = 0; pFactory->EnumAdapters1(i, &tempAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            tempAdapter->GetDesc1(&desc);

            if (!s_LoggedEnum) {
                EarlyLog("UpdateGPU: Checking Adapter %d: LUID %08x:%08x", i, desc.AdapterLuid.HighPart,
                         desc.AdapterLuid.LowPart);
            }

            if (desc.AdapterLuid.LowPart == adapterLuid.LowPart && desc.AdapterLuid.HighPart == adapterLuid.HighPart) {
                tempAdapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&cachedAdapter);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    current.vramTotal = desc.DedicatedVideoMemory;
                    EarlyLog("UpdateGPU: Found match! VRAM Total: %llu bytes", desc.DedicatedVideoMemory);
                }
                s_LoggedEnum = true;
                tempAdapter->Release();
                break;
            }
            tempAdapter->Release();
        }
    }
}

SystemMetrics SystemMetricsCollector::GetMetrics()
{
    std::lock_guard<std::mutex> lock(mutex);
    return current;
}

// Helper to shorten names
static std::string CleanHardwareName(std::string raw, bool isGpu)
{
    // 1. Common cleanup
    std::string s = raw;
    // Replace (R), (TM), @ with spaces
    for (char& c : s) {
        if (c == '(' || c == ')' || c == '@' || c == ',') c = ' ';
    }

    // 2. Tokenize and Filter
    std::vector<std::string> tokens;
    std::string token;
    for (char c : s) {
        if (isspace((unsigned char)c)) {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
        } else {
            token += c;
        }
    }
    if (!token.empty()) tokens.push_back(token);

    std::string result = "";

    if (isGpu) {
        // GPU Strategy: Look for specific start anchors or valid tokens
        // Check for specific series markers first
        bool foundAnchor = false;
        const char* anchors[] = {"RTX", "GTX", "RX", "Arc", "Titan", "Quadro"};

        for (const auto& t : tokens) {
            std::string upperT = t;
            // ToUpper
            for (auto& c : upperT) c = toupper((unsigned char)c);

            // Check anchors
            for (const char* anchor : anchors) {
                if (upperT.find(anchor) == 0) foundAnchor = true;
            }

            // Filter out brands/garbage
            bool skip = false;
            const char* blocklist[] = {"NVIDIA",      "AMD", "INTEL", "GEFORCE", "RADEON", "GRAPHICS",
                                       "CORPORATION", "CO.", "LTD",   "TM",      "R",      "ADAPTER"};
            for (const char* block : blocklist) {
                if (upperT == block) skip = true;
            }

            // Heuristic usage
            if (foundAnchor) {
                // Once anchor found, keep everything (e.g. RTX 4090 Ti)
                if (!skip) {
                    if (!result.empty()) result += " ";
                    result += t;
                }
            } else {
                // While searching for anchor, verify if this token IS the anchor logic?
                // If the token IS "RTX" or similar, we start keeping.
                // Wait, logic above sets foundAnchor if token matches.
                // So if this token IS the anchor, foundAnchor becomes true NOW.
                // But we need to add THIS token too.
                // Re-check anchor logic
                for (const char* anchor : anchors) {
                    if (upperT.find(anchor) == 0) {
                        foundAnchor = true;
                    }
                }

                if (foundAnchor) {
                    if (!skip) {
                        if (!result.empty()) result += " ";
                        result += t;
                    }
                }
            }
        }
        // If no anchor found (e.g. "Intel Graphics"), just show filtered tokens
        if (result.empty()) {
            for (const auto& t : tokens) {
                std::string upperT = t;
                for (auto& c : upperT) c = toupper((unsigned char)c);

                bool skip = false;
                const char* blocklist[] = {"NVIDIA",      "AMD", "INTEL", "GEFORCE", "RADEON", "GRAPHICS",
                                           "CORPORATION", "CO.", "LTD",   "TM",      "R"};
                for (const char* block : blocklist) {
                    if (upperT == block) skip = true;
                }

                if (!skip) {
                    if (!result.empty()) result += " ";
                    result += t;
                }
            }
        }

    } else {
        // CPU Strategy: Filter noise, keep model
        for (const auto& t : tokens) {
            std::string upperT = t;
            for (auto& c : upperT) c = toupper((unsigned char)c);

            bool skip = false;

            // Blocklist
            const char* blocklist[] = {"AMD",  "INTEL", "RYZEN",   "CORE",      "PROCESSOR", "CPU",     "APU",
                                       "SOC",  "GEN",   "COMPUTE", "MICROSOFT", "DUAL",      "QUAD",    "HEXA",
                                       "OCTA", "EIGHT", "SIX",     "TEN",       "TWELVE",    "SIXTEEN", "TWENTY",
                                       "R",    "TM",    "CO.",     "LTD."};
            for (const char* block : blocklist) {
                if (upperT == block) skip = true;
            }

            // Suffix check
            if (upperT.size() > 5 && upperT.substr(upperT.size() - 5) == "-CORE") skip = true;
            if (upperT.size() > 7 && upperT.substr(upperT.size() - 7) == "-THREAD") skip = true;
            if (upperT.find("GHZ") != std::string::npos) skip = true;
            if (upperT.find("MHZ") != std::string::npos) skip = true;

            // Length check (Skip "7", "5", "i9" if split?)
            // User wants "5700X". "i9-13900K".
            // "i9" is 2 chars. "7" is 1 char.
            if (t.size() < 2) skip = true;
            // Also skip "i3", "i5", "i7", "i9" if they are standalone?
            // "i9-13900K" is kept (>=2).
            // "Core i9 13900K" -> "i9 13900K".
            // If I skip "i9", I get "13900K". Cleaner.
            if (upperT == "I3" || upperT == "I5" || upperT == "I7" || upperT == "I9") skip = true;

            if (!skip) {
                if (!result.empty()) result += " ";
                result += t;
            }
        }
    }

    // Fallback if empty (e.g. logic stripped everything)
    if (result.empty()) return raw;
    return result;
}

void SystemMetricsCollector::DetectHardwareNames()
{
    // 1. CPU Name via Registry
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) ==
        ERROR_SUCCESS) {
        char buf[256] = {0};
        DWORD dataSize = sizeof(buf);
        if (RegQueryValueExA(hKey, "ProcessorNameString", 0, 0, (LPBYTE)buf, &dataSize) == ERROR_SUCCESS) {
            cachedCpuName = CleanHardwareName(buf, false);
        }
        RegCloseKey(hKey);
    }

    // 2. GPU Name via DXGI
    // We reuse cachedFactory if available or create new one
    if (adapterLuid.LowPart != 0 || adapterLuid.HighPart != 0) {
        IDXGIFactory4* pFactory = nullptr;
        bool localFactory = false;

        if (cachedFactory) {
            pFactory = (IDXGIFactory4*)cachedFactory;
        } else {
            CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&pFactory);
            localFactory = true;
        }

        if (pFactory) {
            IDXGIAdapter1* tempAdapter = nullptr;
            for (UINT i = 0; pFactory->EnumAdapters1(i, &tempAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc;
                tempAdapter->GetDesc1(&desc);
                if (desc.AdapterLuid.LowPart == adapterLuid.LowPart &&
                    desc.AdapterLuid.HighPart == adapterLuid.HighPart) {
                    char mbBuf[128];
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, mbBuf, sizeof(mbBuf), NULL, NULL);
                    cachedGpuName = CleanHardwareName(mbBuf, true);
                    tempAdapter->Release();
                    break;
                }
                tempAdapter->Release();
            }
            if (localFactory) pFactory->Release();
        }
    }
    EarlyLog("Hardware Detected: CPU='%s', GPU='%s'", cachedCpuName.c_str(), cachedGpuName.c_str());
}
