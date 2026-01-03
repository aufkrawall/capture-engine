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
        if (!pdhInitialized) {
            if (PdhOpenQueryA(NULL, 0, &cpuQuery) == ERROR_SUCCESS) {
                // Total CPU
                PdhAddEnglishCounterA(cpuQuery, "\\Processor(_Total)\\% Processor Time", 0, &cpuCounter);
                PdhCollectQueryData(cpuQuery);
                pdhInitialized = true;
            }
        }
        
        if (!gpuPdhInitialized) {
            if (PdhOpenQueryA(NULL, 0, &gpuQuery) == ERROR_SUCCESS) {
                // GPU Engines
                PDH_STATUS status = PdhAddEnglishCounterA(gpuQuery, "\\GPU Engine(*)\\Utilization Percentage", 0, &gpuCounter);
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
                // VRAM Dedicated
                 PDH_STATUS status = PdhAddEnglishCounterA(vramQuery, "\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &vramCounter);
                 if (status != ERROR_SUCCESS) {
                     status = PdhAddCounterA(vramQuery, "\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &vramCounter);
                 }
                 if (status == ERROR_SUCCESS) {
                     PdhCollectQueryData(vramQuery);
                     vramPdhInitialized = true;
                 } else {
                     PdhCloseQuery(vramQuery);
                     vramQuery = nullptr;
                 }
            }
        }
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

        bool debugLogging = shm->debugLogging;

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
                          if (debugLogging) {
                              LogInfo("[Metrics] GPU Poll: Found %d engine items. Searching for %s or %s", (int)itemCount, luidStrUpper, luidStrLower);
                          }
                          double totalLoad = 0;
                          bool foundAny = false;
                          for (DWORD i = 0; i < itemCount; i++) {
                              if (strstr(items[i].szName, luidStrUpper) || strstr(items[i].szName, luidStrLower)) {
                                  foundAny = true;
                                  // Simplified logic: Just capture basic 3D/Compute loads if possible, or SUM ALL?
                                  // For now, mirroring hook logic: Sum all non-video
                                  bool isVideo = strstr(items[i].szName, "VideoDecode") || strstr(items[i].szName, "VideoEncode");
                                  if (!isVideo) {
                                      totalLoad += items[i].FmtValue.doubleValue;
                                  }
                              }
                          }
                          if (debugLogging && !foundAny) {
                              LogInfo("[Metrics] Warning: No GPU engine found matching LUID %s", luidStrUpper);
                          }
                          if (totalLoad > 100.0) totalLoad = 100.0;
                          shm->systemMetrics.gpuUsage.store((float)totalLoad, std::memory_order_relaxed);
                      }
                 }
             }
        }
        
        // --- Max Core Load (CPU) ---
        // Using NtQuerySystemInformation Class 8 (SystemProcessorPerformanceInformation)
        static NtQuerySystemInformationPtr NtQuerySystemInformation = nullptr;
        if (!NtQuerySystemInformation) {
            HMODULE hNtDll = GetModuleHandleA("ntdll.dll");
            if (hNtDll) NtQuerySystemInformation = (NtQuerySystemInformationPtr)GetProcAddress(hNtDll, "NtQuerySystemInformation");
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
            if (NtQuerySystemInformation(SystemProcessorPerformanceInformation, currCpuInfo.data(), sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION) * numProcs, &len) >= 0) {
                float maxCore = 0.0f;
                if (!prevCpuInfo.empty() && prevCpuInfo[0].IdleTime.QuadPart != 0) {
                    for (int i = 0; i < numProcs; i++) {
                        uint64_t idle = currCpuInfo[i].IdleTime.QuadPart - prevCpuInfo[i].IdleTime.QuadPart;
                        uint64_t kernel = currCpuInfo[i].KernelTime.QuadPart - prevCpuInfo[i].KernelTime.QuadPart;
                        uint64_t user = currCpuInfo[i].UserTime.QuadPart - prevCpuInfo[i].UserTime.QuadPart;
                        uint64_t total = kernel + user;
                        if (total > 0) {
                            float usage = (float)(total - idle) / total * 100.0f;
                            if (usage > maxCore) maxCore = usage;
                        }
                    }
                }
                prevCpuInfo = currCpuInfo;
                shm->systemMetrics.maxCoreLoad.store((uint32_t)maxCore, std::memory_order_relaxed);
            }
        }

        // VRAM Usage
        if (g_HostMetrics.vramPdhInitialized && g_HostMetrics.vramCounter) {
             PdhCollectQueryData(g_HostMetrics.vramQuery);
             DWORD bufSize = 0, itemCount = 0;
             PdhGetFormattedCounterArrayA(g_HostMetrics.vramCounter, PDH_FMT_LARGE, &bufSize, &itemCount, NULL);
             if (bufSize > 0) {
                  std::vector<BYTE> tempBuf(bufSize);
                  PDH_FMT_COUNTERVALUE_ITEM_A* items = (PDH_FMT_COUNTERVALUE_ITEM_A*)tempBuf.data();
                  if (PdhGetFormattedCounterArrayA(g_HostMetrics.vramCounter, PDH_FMT_LARGE, &bufSize, &itemCount, items) == ERROR_SUCCESS) {
                      if (debugLogging) {
                          LogInfo("[Metrics] VRAM Poll: Found %d adapter items.", (int)itemCount);
                      }
                      int64_t totalVRAM = 0;
                      bool foundAny = false;
                      for (DWORD i=0; i<itemCount; i++) {
                          if (strstr(items[i].szName, luidStrUpper) || strstr(items[i].szName, luidStrLower)) {
                              foundAny = true;
                              totalVRAM += items[i].FmtValue.largeValue;
                          }
                      }
                      if (debugLogging && !foundAny) {
                          LogInfo("[Metrics] Warning: No VRAM adapter found matching LUID %s", luidStrUpper);
                      }
                      double vramMB = (double)totalVRAM / (1024.0 * 1024.0);
                      shm->systemMetrics.vramUsage.store((float)vramMB, std::memory_order_relaxed);
                  }
             }
        }
    }
}
