#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

class PerformanceMetrics {
public:
  static const int HISTORY_SIZE = 240;
  static const int VARIANCE_WINDOW = 120;

  PerformanceMetrics();
  ~PerformanceMetrics();

  // Call this once per frame with the current time (e.g. QPC microseconds)
  void Update(int64_t currentQpcUs);

  // Get plotting data for ImGui
  const float *GetHistoryArray() const { return m_history; }
  int GetHistoryIndex() const { return m_historyIdx; }
  float GetCurrentFPS() const;

  // Variance / Stutter detection stats
  double GetWindowStdDev() const { return m_windowStdDev; }
  bool IsStutterDetected() const { return m_stutterDetected; }

  // Graph Scaling
  // Returns min/max for PlotLines. Anchors min at 0, ensures max is at least
  // minRangeMs.
  void GetSmartScale(float &outMin, float &outMax,
                     float minRangeMs = 33.0f) const;

  // State management
  void SetRecording(bool isRecording);

  // CSV Logging for frame times (debug mode)
  void EnableCSVLogging(const char *logPath);
  void DisableCSVLogging();
  bool IsCSVLoggingEnabled() const { return m_csvFile != nullptr; }

private:
  float m_history[HISTORY_SIZE];
  int m_historyIdx;

  int64_t m_lastFrameTimeUs;
  int64_t m_frameCounter; // Frame number for CSV

  // Variance calculation (Welford's)
  int64_t m_frameTimeWindow[VARIANCE_WINDOW];
  int m_windowIndex;
  bool m_windowFilled;

  double m_windowVariance;
  double m_windowStdDev;

  // Baseline vs Recording stats
  bool m_isRecording;
  double m_baselineMean;
  double m_baselineM2;
  int64_t m_baselineCount;

  double m_recordingMean;
  double m_recordingM2;
  int64_t m_recordingCount;

  double m_lastBaselineVariance;
  bool m_stutterDetected;

  // CSV logging
  FILE *m_csvFile;
  mutable std::mutex m_mutex;
};
