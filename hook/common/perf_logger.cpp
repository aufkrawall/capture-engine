#include "perf_logger.h"
#include "hook_common.h"
#include <Windows.h>
#include <filesystem>

PerfLogger &PerfLogger::Get() {
  static PerfLogger instance;
  return instance;
}

void PerfLogger::Init(const char *logPath) {
  if (file_)
    return;

  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  qpcFreq_ = freq.QuadPart;

  std::filesystem::path path(logPath);
  std::filesystem::path dir = path.parent_path();
  if (!dir.empty() && !std::filesystem::exists(dir)) {
    std::filesystem::create_directories(dir);
  }

  file_ = fopen(logPath, "w");
  if (file_) {
    setvbuf(file_, nullptr, _IOFBF, 16384);
    fprintf(file_, "frame,qpc_us,total_us,overlay_us,capture_us,device_init_us,"
                   "prerender_wait_us,fps_limit_wait_us,api\n");
    fflush(file_);
    headerWritten_ = true;
    HookLog("PerfLogger: Initialized CSV logging to %s", logPath);
  } else {
    HookLog("PerfLogger: Failed to open %s for writing", logPath);
  }
}

void PerfLogger::Shutdown() {
  if (file_) {
    fflush(file_);
    fclose(file_);
    file_ = nullptr;
    HookLog("PerfLogger: Shutdown, logged %llu frames",
            (unsigned long long)frameCount_.load());
  }
}

void PerfLogger::LogFrame(const FrameMetrics &metrics) {
  if (!file_)
    return;

  uint64_t frameNum = frameCount_.fetch_add(1, std::memory_order_relaxed) + 1;

  fprintf(file_, "%llu,%lld,%d,%d,%d,%d,%d,%d,%s\n",
          (unsigned long long)frameNum, (long long)metrics.qpcUs,
          metrics.totalUs, metrics.overlayUs, metrics.captureUs,
          metrics.deviceInitUs, metrics.prerenderWaitUs, metrics.fpsLimitWaitUs,
          metrics.api);

  if ((frameNum % 1000) == 0) {
    fflush(file_);
  }
}

int64_t PerfLogger::GetQpcUs() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  return (now.QuadPart * 1000000) / freq.QuadPart;
}

int64_t PerfLogger::GetQpcFrequency() {
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  return freq.QuadPart;
}

ScopedPerfTimer::ScopedPerfTimer(int32_t *resultUs)
    : resultUs_(resultUs), startQpc_(0) {
  if (resultUs_) {
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    startQpc_ = start.QuadPart;
  }
}

ScopedPerfTimer::~ScopedPerfTimer() {
  if (resultUs_ && startQpc_ > 0) {
    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    int64_t elapsedUs = ((end.QuadPart - startQpc_) * 1000000) / freq.QuadPart;
    *resultUs_ = static_cast<int32_t>(elapsedUs);
  }
}