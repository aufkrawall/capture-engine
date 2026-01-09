#include "host_metrics.h"
#include "../common/logging.h"
#include <iostream>
#include <algorithm>
#include <thread>
#include <mutex>
#include <stdio.h>

// For NTQuerySystemInformation
#include <winternl.h>
#pragma comment(lib, "ntdll.lib")

namespace scan_host {

    static HostMetricsState g_HostMetrics;
    static std::mutex g_MetricsMutex;

    // Helper for NTQuerySystemInformation
    typedef NTSTATUS (WINAPI *NtQuerySystemInformationPtr)(
        SYSTEM_INFORMATION_CLASS SystemInformationClass,
        PVOID SystemInformation,
        ULONG SystemInformationLength,
        PULONG ReturnLength
    );

    void HostMetricsState::Initialize() {
        static bool loggedInit = false;
        
        if (!pdhInitialized) {
            if (PdhOpenQueryA(NULL, 0, &cpuQuery) == ERROR_SUCCESS) {
                PdhAddEnglishCounterA(cpuQuery, "\\Processor(_Total)\\% Processor Time", 0, &cpuCounter);
                PdhCollectQueryData(cpuQuery);
                pdhInitialized = true;
                if (!loggedInit) LogInfo("[HostMetrics] CPU PDH initialized");
            }
        }
        
        if (!gpuPdhInitialized) {
            if (PdhOpenQueryA(NULL, 0, &gpuQuery) == ERROR_SUCCESS) {
                PDH_STATUS status = PdhAddEnglishCounterA(gpuQuery, "\\GPU Engine(*)\\Utilization Percentage", 0, &gpuCounter);
                if (status != ERROR_SUCCESS) {
                     status = PdhAddCounterA(gpuQuery, "\\GPU Engine(*)\\Utilization Percentage", 0, &gpuCounter);
                }
                
                if (status == ERROR_SUCCESS) {
                    PdhCollectQueryData(gpuQuery);
                    gpuPdhInitialized = true;
                    if (!loggedInit) LogInfo("[HostMetrics] GPU PDH initialized");
                } else {
                    if (!loggedInit) LogError("[HostMetrics] GPU PDH failed: 0x%08X", status);
                    PdhCloseQuery(gpuQuery);
                    gpuQuery = nullptr;
                }
            }
        }
        
        if (!vramPdhInitialized) {
            if (PdhOpenQueryA(NULL, 0, &vramQuery) == ERROR_SUCCESS) {
                 PDH_STATUS status = PdhAddEnglishCounterA(vramQuery, "\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &vramCounter);
                 if (status != ERROR_SUCCESS) {
                     status = PdhAddCounterA(vramQuery, "\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &vramCounter);
                 }
                 if (status == ERROR_SUCCESS) {
                     PdhCollectQueryData(vramQuery);
                     vramPdhInitialized = true;
                     if (!loggedInit) LogInfo("[HostMetrics] VRAM PDH initialized");
                 } else {
                     if (!loggedInit) LogError("[HostMetrics] VRAM PDH failed: 0x%08X", status);
                     PdhCloseQuery(vramQuery);
                     vramQuery = nullptr;
                 }
            }
        }
        
        loggedInit = true;
    }

    void HostMetricsState::Cleanup() {
        if (cpuQuery) PdhCloseQuery(cpuQuery);
        if (gpuQuery) PdhCloseQuery(gpuQuery);
        if (vramQuery) PdhCloseQuery(vramQuery);
        if (pdhBuffer) free(pdhBuffer);
        
        cpuQuery = gpuQuery = vramQuery = nullptr;
        pdhInitialized = gpuPdhInitialized = vramPdhInitialized = false;
    }

    void UpdateSystemMetrics(SharedMemoryLayout* shm, uint32_t targetPid, int64_t luid) {
        if (!shm) return;
        
        std::lock_guard<std::mutex> lock(g_MetricsMutex);
        g_HostMetrics.Initialize(); // Init if needed

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
        char luidStr[64];
        uint32_t low = (uint32_t)(luid & 0xFFFFFFFF);
        uint32_t high = (uint32_t)((luid >> 32) & 0xFFFFFFFF);
        snprintf(luidStr, sizeof(luidStr), "luid_0x%08X_0x%08X", high, low);

        // GPU Load
        if (g_HostMetrics.gpuPdhInitialized && g_HostMetrics.gpuCounter) {
             PdhCollectQueryData(g_HostMetrics.gpuQuery);
             
             DWORD bufSize = 0, itemCount = 0;
             PdhGetFormattedCounterArrayA(g_HostMetrics.gpuCounter, PDH_FMT_DOUBLE, &bufSize, &itemCount, NULL);
             if (bufSize > 0) {
                 if (bufSize > g_HostMetrics.pdhBufferSize) {
                     void* newBuf = realloc(g_HostMetrics.pdhBuffer, bufSize);
                     if (newBuf) {
                         g_HostMetrics.pdhBuffer = newBuf;
                         g_HostMetrics.pdhBufferSize = bufSize;
                     }
                 }
                 
                 if (g_HostMetrics.pdhBuffer) {
                      PDH_FMT_COUNTERVALUE_ITEM_A* items = (PDH_FMT_COUNTERVALUE_ITEM_A*)g_HostMetrics.pdhBuffer;
                      if (PdhGetFormattedCounterArrayA(g_HostMetrics.gpuCounter, PDH_FMT_DOUBLE, &g_HostMetrics.pdhBufferSize, &itemCount, items) == ERROR_SUCCESS) {
                          double totalLoad = 0;
                          int matchCount = 0;
                          for (DWORD i = 0; i < itemCount; i++) {
                              if (strstr(items[i].szName, luidStr)) {
                                  bool isVideo = strstr(items[i].szName, "VideoDecode") || strstr(items[i].szName, "VideoEncode");
                                  if (!isVideo) {
                                      totalLoad += items[i].FmtValue.doubleValue;
                                      matchCount++;
                                  }
                              }
                          }
                          if (totalLoad > 100.0) totalLoad = 100.0;
                          shm->systemMetrics.gpuUsage.store((float)totalLoad, std::memory_order_relaxed);
                          
                          static int gpuLogCount = 0;
                          if (gpuLogCount++ < 3) {
                              LogInfo("[HostMetrics] GPU: %.1f%% (%d engines matched for %s, %d total)", 
                                      totalLoad, matchCount, luidStr, itemCount);
                          }
                      }
                 }
             }
        }
        
        // VRAM Usage
        if (g_HostMetrics.vramPdhInitialized && g_HostMetrics.vramCounter) {
             PdhCollectQueryData(g_HostMetrics.vramQuery);
             
             DWORD bufSize = 0, itemCount = 0;
             PdhGetFormattedCounterArrayA(g_HostMetrics.vramCounter, PDH_FMT_LARGE, &bufSize, &itemCount, NULL);
             if (bufSize > 0) {
                 // Use a separate buffer for VRAM to avoid conflicts
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
                     if (PdhGetFormattedCounterArrayA(g_HostMetrics.vramCounter, PDH_FMT_LARGE, &vramBufferSize, &itemCount, items) == ERROR_SUCCESS) {
                         int64_t maxVram = 0;
                         for (DWORD i = 0; i < itemCount; i++) {
                             if (strstr(items[i].szName, luidStr)) {
                                 if (items[i].FmtValue.largeValue > maxVram) {
                                     maxVram = items[i].FmtValue.largeValue;
                                 }
                             }
                         }
                         // Store as MB for shared memory
                         shm->systemMetrics.vramUsage.store((float)(maxVram / (1024.0 * 1024.0)), std::memory_order_relaxed);
                     }
                 }
             }
        }
    }
}
