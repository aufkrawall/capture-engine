#pragma once
// frame_timing.h - Unified frame timing and PTS calculation
// Provides consistent frame pacing across WGC and Inject capture paths

#include <Windows.h>
#include <atomic>
#include <cstdint>

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

    frameCount = 0;
    startTimeTicks = 0;
    initialized = true;
  }

  // Start timing from this point (call when recording starts)
  void Start() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    startTimeTicks = now.QuadPart;
    frameCount = 0;
  }

  // Get the next PTS in milliseconds for a new frame
  // This uses the frame's capture timestamp if provided,
  // or generates one based on frame count for CFR encoding
  int64_t GetNextPtsMs(int64_t captureTimestampUs = -1) {
    if (!initialized)
      return 0;

    int64_t pts;

    if (captureTimestampUs >= 0) {
      // VFR mode: use actual capture timestamp
      // Convert microseconds relative to start time to milliseconds
      int64_t captureTicksRelative =
          (captureTimestampUs * qpcFrequency) / 1000000;
      pts = (captureTicksRelative * 1000) / qpcFrequency;
    } else if (targetIntervalTicks > 0) {
      // CFR mode: calculate PTS based on frame count and target interval
      int64_t frameTicks = frameCount * targetIntervalTicks;
      pts = (frameTicks * 1000) / qpcFrequency;
    } else {
      // Fallback: use elapsed time from start
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      int64_t elapsed = now.QuadPart - startTimeTicks;
      pts = (elapsed * 1000) / qpcFrequency;
    }

    frameCount++;
    return pts;
  }

  // Get PTS in encoder timebase units (e.g., 1/90000)
  int64_t GetNextPtsInTimebase(int timebaseNum, int timebaseDen,
                               int64_t captureTimestampUs = -1) {
    if (!initialized || timebaseDen == 0)
      return 0;

    int64_t ptsMs = GetNextPtsMs(captureTimestampUs);
    // Convert ms to timebase: pts = (ptsMs / 1000) * (timebaseDen /
    // timebaseNum) Rearranged to avoid overflow: pts = ptsMs * timebaseDen /
    // (1000 * timebaseNum)
    return (ptsMs * timebaseDen) / (1000 * timebaseNum);
  }

  // Get current frame number
  uint64_t GetFrameCount() const { return frameCount; }

  // Get elapsed time in milliseconds since Start()
  int64_t GetElapsedMs() const {
    if (!initialized || startTimeTicks == 0)
      return 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int64_t elapsed = now.QuadPart - startTimeTicks;
    return (elapsed * 1000) / qpcFrequency;
  }

  // Check if we should capture a new frame (for rate limiting)
  // Returns true if enough time has passed since last frame
  bool ShouldCaptureFrame() const {
    if (!initialized || targetIntervalTicks == 0)
      return true;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int64_t elapsed = now.QuadPart - startTimeTicks;
    int64_t expectedFrames = elapsed / targetIntervalTicks;

    return expectedFrames >= frameCount;
  }

  // Get target FPS
  double GetTargetFps() const { return targetFps; }

  // Reset for new recording
  void Reset() {
    frameCount = 0;
    startTimeTicks = 0;
  }

private:
  double targetFps = 0;
  int64_t qpcFrequency = 0;
  int64_t targetIntervalTicks = 0;
  int64_t startTimeTicks = 0;
  uint64_t frameCount = 0;
  bool initialized = false;
};

// High-precision sleep using waitable timer
// Returns true if sleep completed, false if interrupted
inline bool PrecisionSleep(int64_t microseconds) {
  if (microseconds <= 0)
    return true;

  // Create high-resolution waitable timer if available (Windows 10 1803+)
  HANDLE timer = CreateWaitableTimerExW(
      NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

  if (!timer) {
    // Fallback to standard timer
    timer = CreateWaitableTimer(NULL, TRUE, NULL);
  }

  if (!timer) {
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

  CloseHandle(timer);
  return true;
}
