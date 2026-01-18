#include "performance_metrics.h"
#include <cstring>

PerformanceMetrics::PerformanceMetrics() {
  memset(m_history, 0, sizeof(m_history));
  m_historyIdx = 0;
  m_lastFrameTimeUs = 0;
  m_frameCounter = 0;

  memset(m_frameTimeWindow, 0, sizeof(m_frameTimeWindow));
  m_windowIndex = 0;
  m_windowFilled = false;

  m_windowVariance = 0.0;
  m_windowStdDev = 0.0;

  m_isRecording = false;
  m_baselineMean = 0;
  m_baselineM2 = 0;
  m_baselineCount = 0;
  m_recordingMean = 0;
  m_recordingM2 = 0;
  m_recordingCount = 0;
  m_lastBaselineVariance = 0;
  m_stutterDetected = false;

  m_csvFile = nullptr;
}

PerformanceMetrics::~PerformanceMetrics() { DisableCSVLogging(); }

void PerformanceMetrics::EnableCSVLogging(const char *logPath) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_csvFile) {
    fclose(m_csvFile);
  }
  m_csvFile = fopen(logPath, "w");
  if (m_csvFile) {
    // Write CSV header
    fprintf(m_csvFile,
            "Frame,Timestamp_us,FrameTime_us,FrameTime_ms,FPS,Recording\n");
    fflush(m_csvFile);
    m_frameCounter = 0;
  }
}

void PerformanceMetrics::DisableCSVLogging() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_csvFile) {
    fclose(m_csvFile);
    m_csvFile = nullptr;
  }
}

void PerformanceMetrics::SetRecording(bool isRecording) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_isRecording != isRecording) {
    if (isRecording) {
      // STARTING: Snapshot baseline variance
      if (m_baselineCount > VARIANCE_WINDOW) {
        m_lastBaselineVariance = m_baselineM2 / m_baselineCount;
      }

      // Reset recording stats
      m_recordingMean = 0;
      m_recordingM2 = 0;
      m_recordingCount = 0;
      m_stutterDetected = false;
    }
    // STOPPING: No specific action needed for stats retention
    // (could reset baseline if we wanted, but keeping it is fine)

    m_isRecording = isRecording;
  }
}

void PerformanceMetrics::Update(int64_t currentQpcUs) {
  std::lock_guard<std::mutex> lock(m_mutex);
  int64_t frameToFrameUs = 0;
  if (m_lastFrameTimeUs > 0) {
    frameToFrameUs = currentQpcUs - m_lastFrameTimeUs;
  }

  // Debounce: ignore calls that are too close together (< 200us)
  // This happens when both a Wrapper and a Hook call Update for the same frame.
  if (m_lastFrameTimeUs > 0 && frameToFrameUs < 200) {
      return;
  }

  m_lastFrameTimeUs = currentQpcUs;

  // Ignore first frame or jumps
  if (frameToFrameUs <= 0)
    return;



  m_frameCounter++;

  // 1. Update Plot History
  float frameTimeMs = (float)frameToFrameUs / 1000.0f;
  m_history[m_historyIdx] = frameTimeMs;
  m_historyIdx = (m_historyIdx + 1) % HISTORY_SIZE;

  // 2. Write to CSV if enabled (debug mode)
  if (m_csvFile) {
    float fps = (frameToFrameUs > 0) ? (1000000.0f / frameToFrameUs) : 0.0f;
    fprintf(m_csvFile, "%lld,%lld,%lld,%.3f,%.1f,%d\n",
            (long long)m_frameCounter, (long long)currentQpcUs,
            (long long)frameToFrameUs, frameTimeMs, fps, m_isRecording ? 1 : 0);
    // Flush periodically (every 100 frames) to avoid data loss
    if (m_frameCounter % 100 == 0) {
      fflush(m_csvFile);
    }
  }

  // 2. Update Rolling Window
  m_frameTimeWindow[m_windowIndex] = frameToFrameUs;
  m_windowIndex = (m_windowIndex + 1) % VARIANCE_WINDOW;
  if (m_windowIndex == 0)
    m_windowFilled = true;

  // 3. Welford's Online Variance (Stable)
  if (m_isRecording) {
    m_recordingCount++;
    double delta = frameToFrameUs - m_recordingMean;
    m_recordingMean += delta / m_recordingCount;
    double delta2 = frameToFrameUs - m_recordingMean;
    m_recordingM2 += delta * delta2;
  } else {
    m_baselineCount++;
    double delta = frameToFrameUs - m_baselineMean;
    m_baselineMean += delta / m_baselineCount;
    double delta2 = frameToFrameUs - m_baselineMean;
    m_baselineM2 += delta * delta2;
  }

  // 4. Calculate Window Variance/StdDev (Snapshot)
  // Re-calculate from window for immediate feedback (simplest for display)
  if (m_windowFilled || m_windowIndex > 10) {
    int count = m_windowFilled ? VARIANCE_WINDOW : m_windowIndex;
    double sum = 0;
    double sumSq = 0;
    for (int i = 0; i < count; i++) {
      sum += m_frameTimeWindow[i];
      sumSq += (double)m_frameTimeWindow[i] * m_frameTimeWindow[i];
    }
    double mean = sum / count;
    double var = (sumSq / count) - (mean * mean);
    m_windowVariance = (var > 0) ? var : 0;
    m_windowStdDev = sqrt(m_windowVariance);
  }

  // 5. Stutter Detection Logic
  if (m_isRecording && m_recordingCount > 240 && m_lastBaselineVariance > 1.0) {
    double currentRecVar =
        (m_recordingCount > 1) ? (m_recordingM2 / m_recordingCount) : 0.0;
    double ratio = currentRecVar / m_lastBaselineVariance;
    if (ratio > 2.0) {
      m_stutterDetected = true;
    } else if (ratio < 1.5) {
      m_stutterDetected = false;
    }
  }
}

float PerformanceMetrics::GetCurrentFPS() const {
  // Average over last 60 frames for stable FPS display
  // This avoids jumpy values from double-present patterns or occasional spikes
  const int AVERAGE_FRAMES = 60;
  float totalMs = 0.0f;
  int validFrames = 0;

  for (int i = 0; i < AVERAGE_FRAMES; i++) {
    int idx = (m_historyIdx - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
    float ms = m_history[idx];
    if (ms > 0.0001f && ms < 100.0f) { // Valid frame time (0.01ms to 100ms)
      totalMs += ms;
      validFrames++;
    }
  }

  if (validFrames > 0 && totalMs > 0.0001f) {
    float avgMs = totalMs / validFrames;
    return 1000.0f / avgMs;
  }
  return 0.0f;
}

float PerformanceMetrics::GetAverageFPS() const {
  // Average FPS over last 15 seconds
  std::lock_guard<std::mutex> lock(m_mutex);
  float totalMs = 0.0f;
  int count = 0;

  for (int i = 0; i < HISTORY_SIZE; i++) {
    int idx = (m_historyIdx - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
    float ms = m_history[idx];
    if (ms <= 0.0001f) break; // End of history
    
    totalMs += ms;
    count++;
    if (totalMs >= 5000.0f) break; // 5s window reached
  }

  if (count > 0 && totalMs > 0.0001f) {
    return 1000.0f / (totalMs / count);
  }
  return 0.0f;
}

float PerformanceMetrics::Get1PercentLowFPS() const {
  // 1% low = 99th percentile worst frame times over 15s
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<float> frameTimes;
  frameTimes.reserve(HISTORY_SIZE);

  float totalMs = 0.0f;
  for (int i = 0; i < HISTORY_SIZE; i++) {
    int idx = (m_historyIdx - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
    float ms = m_history[idx];
    if (ms <= 0.0001f) break;
    
    frameTimes.push_back(ms);
    totalMs += ms;
    if (totalMs >= 5000.0f) break;
  }

  if (frameTimes.size() < 10) return 0.0f;

  std::sort(frameTimes.begin(), frameTimes.end(), std::greater<float>());
  
  // Percentile index: e.g. for 1000 samples, it's index 10
  int percentileIdx = (int)(frameTimes.size() * 0.01f);
  if (percentileIdx >= (int)frameTimes.size()) percentileIdx = (int)frameTimes.size() - 1;

  // Average the worst 1% for stability
  float sum = 0.0f;
  int count = std::max(1, percentileIdx + 1);
  for (int i = 0; i < count; i++) {
    sum += frameTimes[i];
  }
  return 1000.0f / (sum / count);
}

float PerformanceMetrics::Get01PercentLowFPS() const {
  // 0.1% low = 99.9th percentile worst frame times over 15s
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<float> frameTimes;
  frameTimes.reserve(HISTORY_SIZE);

  float totalMs = 0.0f;
  for (int i = 0; i < HISTORY_SIZE; i++) {
    int idx = (m_historyIdx - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
    float ms = m_history[idx];
    if (ms <= 0.0001f) break;
    
    frameTimes.push_back(ms);
    totalMs += ms;
    if (totalMs >= 5000.0f) break;
  }

  if (frameTimes.size() < 100) return 0.0f;

  std::sort(frameTimes.begin(), frameTimes.end(), std::greater<float>());
  
  // Percentile index: e.g. for 1000 samples, it's index 1
  int percentileIdx = (int)(frameTimes.size() * 0.001f);
  if (percentileIdx >= (int)frameTimes.size()) percentileIdx = (int)frameTimes.size() - 1;

  // Average the worst 0.1% for stability
  float sum = 0.0f;
  int count = std::max(1, percentileIdx + 1);
  for (int i = 0; i < count; i++) {
    sum += frameTimes[i];
  }
  return 1000.0f / (sum / count);
}

void PerformanceMetrics::GetLastHistory(float *outBuffer, int count) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (count > HISTORY_SIZE) count = HISTORY_SIZE;
  
  for (int i = 0; i < count; i++) {
    int idx = (m_historyIdx - count + i + HISTORY_SIZE) % HISTORY_SIZE;
    outBuffer[i] = m_history[idx];
  }
}

void PerformanceMetrics::GetSmartScale(float &outMin, float &outMax,
                                       float minRangeMs) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  outMin = 0.0f;
  float maxVal = 0.0f;
  
  // Calculate average frame time for heuristic
  float totalMs = 0.0f;
  int count = 0;

  // Only scale based on what's visible in the graph
  for (int i = 0; i < GRAPH_HISTORY_SIZE; i++) {
    int idx = (m_historyIdx - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
    float ms = m_history[idx];
    if (ms > maxVal) maxVal = ms;
    if (ms <= 0.0001f) break; 
    
    totalMs += ms;
    count++;
  }
  
  float avgMs = (count > 0) ? (totalMs / count) : 16.6f;

  // Ensure we show at least the minimum range (e.g. 33ms)
  // AND ensure we show at least 3x the average frame time to prevent zoom-in on jitter
  float dynamicMin = avgMs * 3.0f;
  float lowerBound = std::max(minRangeMs, dynamicMin);

  if (maxVal < lowerBound) {
    outMax = lowerBound;
  } else {
    outMax = maxVal * 1.1f;
  }
}

float PerformanceMetrics::GetMaxFrameTime(float windowSeconds) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  float maxMs = 0.0f;
  float accumTime = 0.0f;
  
  // Iterate backwards through history
  for (int i = 0; i < HISTORY_SIZE; i++) {
    int idx = (m_historyIdx - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
    float ms = m_history[idx];
    
    if (ms <= 0.0001f) break;
    
    if (ms > maxMs) maxMs = ms;
    
    accumTime += ms;
    // Safety break if we simply have too many samples, but 
    // ms is in milliseconds, windowSeconds is in seconds.
    if (accumTime >= windowSeconds * 1000.0f) {
        break;
    }
  }
  
  return maxMs;
}
