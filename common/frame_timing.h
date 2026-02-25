#pragma once
// frame_timing.h - Unified frame timing and PTS calculation
// Provides consistent frame pacing across WGC and Inject capture paths

#include <atomic>
#include <cstdint>
#include <windows.h>

// FramePacer: Manages presentation timestamp generation and frame timing
// for video encoding. This class ensures consistent PTS values regardless
// of capture source (WGC or Inject).
class FramePacer {
public:
  // Initialize with target FPS and optional QPC frequency
  // If qpcFreq is 0, it will be queried automatically
  void Init(double targetFps, int64_t qpcFreq = 0) {
    this->targetFps = targetFps;
    if (qpcFreq == 0) {
      LARGE_INTEGER freq;
      QueryPerformanceFrequency(&freq);
      this->qpcFrequency = freq.QuadPart;
    } else {
      this->qpcFrequency = qpcFreq;
    }

    // Calculate interval in ticks
    if (targetFps > 0) {
      targetIntervalTicks = qpcFrequency / targetFps;
    } else {
      targetIntervalTicks = 0; // Variable framerate
    }

    frameCount.store(0, std::memory_order_relaxed);
    startTimeTicks.store(0, std::memory_order_relaxed);
    initialized.store(true, std::memory_order_release);
  }

  // Start timing from this point (call when recording starts)
  void Start() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    startTimeTicks.store(now.QuadPart, std::memory_order_release);
    frameCount.store(0, std::memory_order_relaxed);
  }

  // Get the next PTS in milliseconds for a new frame
  // This uses the frame's capture timestamp if provided,
  // or generates one based on frame count for CFR encoding
  int64_t GetNextPtsMs(int64_t captureTimestampUs = -1) {
    if (!initialized.load(std::memory_order_acquire))
      return 0;

    int64_t pts;

    if (captureTimestampUs >= 0) {
      // VFR mode: use actual capture timestamp
      // Convert microseconds relative to start time to milliseconds
      int64_t captureTicksRelative =
          (captureTimestampUs * qpcFrequency) / 1000000;
      pts = (captureTicksRelative * 1000) / qpcFrequency;
    } else if (targetFps > 0) {
      // CFR mode: compute PTS directly from frame count and target FPS using
      // floating-point to avoid accumulated integer-division error. At 60fps,
      // integer division (qpcFreq/60) loses 0.67 ticks/frame ≈ 0.24ms/minute.
      uint64_t count = frameCount.load(std::memory_order_relaxed);
      pts = static_cast<int64_t>(static_cast<double>(count) * 1000.0 / targetFps);
    } else {
      // Fallback: use elapsed time from start
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      int64_t start = startTimeTicks.load(std::memory_order_acquire);
      int64_t elapsed = now.QuadPart - start;
      pts = (elapsed * 1000) / qpcFrequency;
    }

    frameCount.fetch_add(1, std::memory_order_relaxed);
    return pts;
  }

  // Get PTS in encoder timebase units (e.g., 1/90000)
  int64_t GetNextPtsInTimebase(int timebaseNum, int timebaseDen,
                               int64_t captureTimestampUs = -1) {
    if (!initialized.load(std::memory_order_acquire) || timebaseDen == 0)
      return 0;

    int64_t ptsMs = GetNextPtsMs(captureTimestampUs);
    // Convert ms to timebase: pts = (ptsMs / 1000) * (timebaseDen /
    // timebaseNum) Rearranged to avoid overflow: pts = ptsMs * timebaseDen /
    // (1000 * timebaseNum)
    return (ptsMs * timebaseDen) / (1000 * timebaseNum);
  }

  // Get current frame number
  uint64_t GetFrameCount() const {
    return frameCount.load(std::memory_order_relaxed);
  }

  // Get elapsed time in milliseconds since Start()
  int64_t GetElapsedMs() const {
    int64_t start = startTimeTicks.load(std::memory_order_acquire);
    if (!initialized.load(std::memory_order_acquire) || start == 0)
      return 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int64_t elapsed = now.QuadPart - start;
    return (elapsed * 1000) / qpcFrequency;
  }

  // Check if we should capture a new frame (for rate limiting)
  // Returns true if enough time has passed since last frame
  bool ShouldCaptureFrame() const {
    if (!initialized.load(std::memory_order_acquire) || targetIntervalTicks == 0)
      return true;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int64_t start = startTimeTicks.load(std::memory_order_acquire);
    int64_t elapsed = now.QuadPart - start;
    int64_t expectedFrames = elapsed / targetIntervalTicks;

    return expectedFrames >= static_cast<int64_t>(frameCount.load(std::memory_order_relaxed));
  }

  // Get target FPS
  double GetTargetFps() const { return targetFps; }

  // Reset for new recording
  void Reset() {
    frameCount.store(0, std::memory_order_relaxed);
    startTimeTicks.store(0, std::memory_order_release);
  }

private:
  double targetFps = 0;
  int64_t qpcFrequency = 0;
  int64_t targetIntervalTicks = 0;
  std::atomic<int64_t> startTimeTicks{0};
  std::atomic<uint64_t> frameCount{0};
  std::atomic<bool> initialized{false};
};

// High-precision sleep using waitable timer
// Returns true if sleep completed, false if interrupted
inline bool PrecisionSleep(int64_t microseconds) {
  if (microseconds <= 0)
    return true;

  // Cache the timer handle per-thread to avoid create/destroy overhead on
  // every call (e.g. at 144fps this would be 144 kernel object ops/sec).
  struct ThreadTimer {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ThreadTimer() {
      handle = CreateWaitableTimerExW(NULL, NULL,
                                      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
      if (handle == INVALID_HANDLE_VALUE)
        handle = CreateWaitableTimerW(NULL, TRUE, NULL);
    }
    ~ThreadTimer() {
      if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    }
  };
  thread_local ThreadTimer s_timer;
  HANDLE timer = s_timer.handle;

  if (timer == INVALID_HANDLE_VALUE) {
    // Ultimate fallback
    Sleep((DWORD)(microseconds / 1000));
    return true;
  }

  LARGE_INTEGER dueTime;
  // Negative value = relative time in 100ns units
  dueTime.QuadPart = -(microseconds * 10);

  if (SetWaitableTimer(timer, &dueTime, 0, NULL, NULL, FALSE)) {
    WaitForSingleObject(timer, INFINITE);
  }

  return true;
}
