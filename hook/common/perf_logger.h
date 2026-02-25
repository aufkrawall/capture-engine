#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>

struct FrameMetrics {
  uint64_t frameNum = 0;
  int64_t qpcUs = 0;
  int32_t totalUs = 0;      // Total time in Present hook
  int32_t overlayUs = 0;    // CPU time for overlay work (cmd record + submit)
  int32_t captureUs = 0;
  int32_t deviceInitUs = 0;
  int32_t prerenderWaitUs = 0;
  int32_t fpsLimitWaitUs = 0;
  int32_t fenceWaitUs = 0;  // Time waiting for fence (previous frame sync)
  // Detailed DX12 breakdown (optional, set to 0 if not applicable)
  int32_t cmdListResetUs = 0;
  int32_t renderUs = 0;
  int32_t executeUs = 0;
  // DX9 staging capture breakdown (optional, set to 0 if not applicable)
  int32_t stretchRectUs = 0;      // GPU blit: backbuffer → intermediate
  int32_t readbackSubmitUs = 0;   // GetRenderTargetData submit time
  int32_t queryWaitUs = 0;        // Time polling/waiting for query completion
  int32_t lockRectUs = 0;         // LockRect on system memory surface
  int32_t d3d11UploadUs = 0;      // UpdateSubresource CPU→GPU upload
  int32_t stagingDepth = 0;       // Current staging pipeline occupancy
  int32_t stagingDropped = 0;     // Frames dropped due to full pipeline
  int32_t presentCallUs = 0;      // Actual D3D9/DXGI Present call time
  char api[8] = "";
};

class PerfLogger {
public:
  static PerfLogger &Get();

  void Init(const char *logPath);
  void Shutdown();

  void LogFrame(const FrameMetrics &metrics);

  bool IsEnabled() const { return file_ != nullptr; }
  uint64_t GetFrameCount() const {
    return frameCount_.load(std::memory_order_relaxed);
  }

  static int64_t GetQpcUs();
  static int64_t GetQpcFrequency();

private:
  PerfLogger() = default;
  ~PerfLogger() { Shutdown(); }

  PerfLogger(const PerfLogger &) = delete;
  PerfLogger &operator=(const PerfLogger &) = delete;

  FILE *file_ = nullptr;
  std::atomic<uint64_t> frameCount_{0};
  int64_t qpcFreq_ = 0;
  bool headerWritten_ = false;
};

class ScopedPerfTimer {
public:
  explicit ScopedPerfTimer(int32_t *resultUs);
  ~ScopedPerfTimer();

  ScopedPerfTimer(const ScopedPerfTimer &) = delete;
  ScopedPerfTimer &operator=(const ScopedPerfTimer &) = delete;

private:
  int32_t *resultUs_;
  int64_t startQpc_;
};

#define PERF_TIMER(var) ScopedPerfTimer _perfTimer_##var(&var)