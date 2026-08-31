#pragma once
#include <dxgi1_6.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <windows.h>
#include <string>
#include <vector>
#include "../common/shared_defs.h"
#include "host_metrics_policy.h"

namespace ce::hardware_sensors {
struct HardwareSensorSnapshot;
}

#ifdef _MSC_VER
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")
#endif

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

    // VRAM PDH (Usage only - Total counter doesn't exist on all systems)
    bool vramPdhInitialized = false;
    PDH_HQUERY vramQuery = nullptr;
    PDH_HCOUNTER vramCounter = nullptr;  // Dedicated Usage

    // DXGI for VRAM Total (64-bit host only)
    IDXGIFactory1* dxgiFactory = nullptr;
    uint64_t cachedVRAMTotal = 0;
    LUID cachedAdapterLuid = {0, 0};
    int64_t lastLoggedMissingGpuLuid = 0;
    int64_t lastLoggedMissingVramLuid = 0;
    int64_t lastLoggedMissingDxgiLuid = 0;
    int64_t lastLoggedVramTotalLuid = 0;
    uint64_t lastLoggedVramTotal = 0;
    uint32_t processResolvedPid = 0;
    int64_t processResolvedLuid = 0;
    uint32_t lastPublishedPid = 0;
    int64_t lastPublishedLuid = 0;
    uint32_t lastPublishedAdapterSource = 0;

    // Helper
    void Initialize(bool includeGpuMetrics);
    void Cleanup();

    // Query VRAM total from DXGI (64-bit safe)
    uint64_t QueryVRAMTotalFromDXGI(int32_t luidLow, int32_t luidHigh);
};

// Main update function called by CaptureEngine loop
// targetPid: The process ID we are capturing (to filter GPU usage if possible)
// knownLuid: A directly observed capture/hook adapter, or zero for process-engine inference.
void UpdateSystemMetrics(
    SharedMemoryLayout* shm, uint32_t targetPid, int64_t knownLuid,
    metrics_policy::AdapterResolutionSource knownSource,
    const ce::hardware_sensors::HardwareSensorSnapshot& hardwareSensors);

}  // namespace scan_host
