#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>

struct FrameMetrics {
  uint64_t frameNum = 0;
  int64_t qpcUs = 0;
  int32_t totalUs = 0;
  int32_t overlayUs = 0;
  int32_t captureUs = 0;
  int32_t deviceInitUs = 0;
  int32_t prerenderWaitUs = 0;
  int32_t fpsLimitWaitUs = 0;
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