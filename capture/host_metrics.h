#pragma once
#include "../common/shared_defs.h"
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <vector>
#include <string>

#pragma comment(lib, "pdh.lib")

namespace scan_host {
    
    // Internal state for host-side metrics
    struct HostMetricsState {
        bool pdhInitialized = false;
        PDH_HQUERY cpuQuery = nullptr;
        PDH_HCOUNTER cpuCounter = nullptr;
        
        // GPU PDH
        bool gpuPdhInitialized = false;
        PDH_HQUERY gpuQuery = nullptr;
        PDH_HCOUNTER gpuCounter = nullptr;
        void* pdhBuffer = nullptr;
        DWORD pdhBufferSize = 0;
        
        // VRAM
        bool vramPdhInitialized = false;
        PDH_HQUERY vramQuery = nullptr;
        PDH_HCOUNTER vramCounter = nullptr;
        
        // Helper
        void Initialize();
        void Cleanup();
    };

    // Main update function called by CaptureEngine loop
    // targetPid: The process ID we are capturing (to filter GPU usage if possible)
    // luid: The LUID of the GPU we are capturing
    void UpdateSystemMetrics(SharedMemoryLayout* shm, uint32_t targetPid, int64_t luid);

}
