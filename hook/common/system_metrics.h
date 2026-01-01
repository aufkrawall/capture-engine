#pragma once
#include <cstdint>
#include <mutex>
#include <Windows.h>

struct SystemMetrics {
  float cpuUsage;       // 0-100%
  uint64_t ramUsed;     // Bytes
  uint64_t ramTotal;    // Bytes
  uint64_t vramUsed;    // Bytes (local video memory)
  uint64_t vramTotal;   // Bytes
  float gpuUsage;       // 0-100%
  bool gpuUsageValid;   // True if gpuUsage is real
};

class SystemMetricsCollector {
public:
  static SystemMetricsCollector& Get();
  
  // Initialize with the LUID of the active GPU
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
  void UpdateCPU();
  void UpdateRAM();
  void UpdateGPU();

  // PDH (CPU)
  void* cpuQuery = nullptr;   // Type: PDH_HQUERY
  void* cpuCounter = nullptr; // Type: PDH_HCOUNTER
  bool pdhInitialized = false;

  // PDH (GPU) - Using Performance Counters
  void* gpuQuery = nullptr;      // Type: PDH_HQUERY
  void* gpuCounter = nullptr;    // Type: PDH_HCOUNTER
  void* gpuMemCounter = nullptr; // Type: PDH_HCOUNTER for dedicated memory
  bool gpuPdhInitialized = false;

  // GPU (DXGI for VRAM)
  LUID adapterLuid = {0, 0};
  
  SystemMetrics current{};
  std::mutex mutex;
};
