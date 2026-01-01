#pragma once
#include <cstdint>
#include <mutex>
#include <Windows.h>

// Forward decl or typedef for D3DKMT if header not available
// typedef unsigned int D3DKMT_HANDLE; 


struct SystemMetrics {
  float cpuUsage;       // 0-100%
  uint64_t ramUsed;     // Bytes
  uint64_t ramTotal;    // Bytes
  uint64_t vramUsed;    // Bytes (local video memory)
  uint64_t vramTotal;   // Bytes
  float gpuUsage;       // 0-100% (D3DKMT, if available)
  bool gpuUsageValid;   // True if gpuUsage is real
};

class SystemMetricsCollector {
public:
  static SystemMetricsCollector& Get();
  
  // Initialize with the LUID of the active GPU (from SharedMemory)
  void Initialize(int32_t luidLow, int32_t luidHigh);
  
  // Update all metrics (call once per frame or throttled)
  void Update();
  
  // Thread-safe snapshot
  SystemMetrics GetMetrics();

private:
  SystemMetricsCollector();
  ~SystemMetricsCollector();
  
  // Helpers
  void InitPDH();
  void InitD3DKMT();
  void UpdateCPU();
  void UpdateRAM();
  void UpdateGPU();

  // PDH (CPU)
  void* cpuQuery = nullptr;   // Type: PDH_HQUERY
  void* cpuCounter = nullptr; // Type: PDH_HCOUNTER
  bool pdhInitialized = false;

  // GPU (DXGI/D3DKMT)
  LUID adapterLuid = {0, 0};
  uint32_t hAdapter = 0; // for D3DKMT (D3DKMT_HANDLE is UINT)
  bool d3dkmtInitialized = false;
  
  // GPU Usage Tracking
  uint64_t lastGpuRunningTime = 0;
  uint64_t lastGpuTime = 0;    // QPC
  
  SystemMetrics current{};
  std::mutex mutex;
};
