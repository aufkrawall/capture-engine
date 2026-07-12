#include "performance_metrics.h"
#include <array>
#include <cstring>
namespace {
constexpr int64_t kDuplicateFrameThresholdUs = 100;

float ComputeWorstPercentileFPS(const float* history, int historyIdx, float percentile, int minSamples) {
    std::array<float, PerformanceMetrics::HISTORY_SIZE> frameTimes{};
    int count = 0;
    float totalMs = 0.0f;

    for (int i = 0; i < PerformanceMetrics::HISTORY_SIZE; i++) {
        int idx = (historyIdx - 1 - i + PerformanceMetrics::HISTORY_SIZE) % PerformanceMetrics::HISTORY_SIZE;
        float ms = history[idx];
        if (ms <= 0.0001f)
            break;
        frameTimes[count++] = ms;
        totalMs += ms;
        if (totalMs >= 5000.0f)
            break;
    }

    if (count < minSamples)
        return 0.0f;

    int percentileIdx = static_cast<int>(count * percentile);
    if (percentileIdx >= count)
        percentileIdx = count - 1;

    int worstCount = std::max(1, percentileIdx + 1);
    auto begin = frameTimes.begin();
    auto middle = begin + worstCount;
    auto end = begin + count;
    std::nth_element(begin, middle, end, std::greater<float>());

    float sum = 0.0f;
    for (int i = 0; i < worstCount; i++) {
        sum += frameTimes[i];
    }
    return 1000.0f / (sum / worstCount);
}
}  // namespace

PerformanceMetrics::PerformanceMetrics() {
    memset(m_history, 0, sizeof(m_history));
    memset(m_frameTimeWindow, 0, sizeof(m_frameTimeWindow));
}

PerformanceMetrics::~PerformanceMetrics() {}

void PerformanceMetrics::SetRecording(bool isRecording) {
    // Single-writer: only called from hook thread
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
            m_stutterDetected.store(false, std::memory_order_relaxed);
        }
        m_isRecording = isRecording;
    }
}

void PerformanceMetrics::SetFGMetrics(float outputFPS, float baseFPS, int multiplier, int fgType) {
    m_fgOutputFPS.store(outputFPS, std::memory_order_relaxed);
    m_fgBaseFPS.store(baseFPS, std::memory_order_relaxed);
    m_fgMultiplier.store(multiplier, std::memory_order_relaxed);
    m_fgType.store(fgType, std::memory_order_relaxed);
}

void PerformanceMetrics::Update(int64_t currentQpcUs) {
    // Lock-free hot path — single writer (present thread), readers use atomics.
    int64_t lastUs = m_lastFrameTimeUs.load(std::memory_order_relaxed);
    int64_t frameToFrameUs = 0;
    if (lastUs > 0) {
        frameToFrameUs = currentQpcUs - lastUs;
    }

    // Filter true duplicate hook re-entry without capping legitimate high-FPS
    // frame pacing. A 2ms debounce incorrectly flattened real >500 FPS sessions.
    if (lastUs > 0 && frameToFrameUs > 0 && frameToFrameUs < kDuplicateFrameThresholdUs) {
        return;
    }

    m_lastFrameTimeUs.store(currentQpcUs, std::memory_order_relaxed);

    // Ignore first frame or jumps
    if (frameToFrameUs <= 0)
        return;

    m_frameCounter++;

    // 1. Update Plot History — atomic index publish ensures readers see
    //    consistent boundary. Individual float writes are naturally atomic on
    //    x86/x64 for aligned 4-byte values.
    float frameTimeMs = (float)frameToFrameUs / 1000.0f;
    int idx = m_historyIdx.load(std::memory_order_relaxed);
    m_history[idx] = frameTimeMs;
    m_historyIdx.store((idx + 1) % HISTORY_SIZE, std::memory_order_release);

    // 3. Update Rolling Window (writer-only)
    m_frameTimeWindow[m_windowIndex] = frameToFrameUs;
    m_windowIndex = (m_windowIndex + 1) % VARIANCE_WINDOW;
    if (m_windowIndex == 0)
        m_windowFilled = true;

    // 4. Welford's Online Variance (writer-only)
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

    // 5. Calculate Window Variance/StdDev — publish atomically for readers
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
        if (var < 0)
            var = 0;
        m_windowVariance.store(var, std::memory_order_relaxed);
        m_windowStdDev.store(sqrt(var), std::memory_order_relaxed);
    }

    // 6. Stutter Detection Logic (writer-only, publish atomically)
    if (m_isRecording && m_recordingCount > 240 && m_lastBaselineVariance > 1.0) {
        double currentRecVar = (m_recordingCount > 1) ? (m_recordingM2 / m_recordingCount) : 0.0;
        double ratio = currentRecVar / m_lastBaselineVariance;
        if (ratio > 2.0) {
            m_stutterDetected.store(true, std::memory_order_relaxed);
        } else if (ratio < 1.5) {
            m_stutterDetected.store(false, std::memory_order_relaxed);
        }
    }
}

float PerformanceMetrics::GetCurrentFPS() const {
    // Lock-free: reads from atomic history index + float array
    const int AVERAGE_FRAMES = 60;
    float totalMs = 0.0f;
    int validFrames = 0;
    int histIdx = m_historyIdx.load(std::memory_order_acquire);

    for (int i = 0; i < AVERAGE_FRAMES; i++) {
        int idx = (histIdx - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
        float ms = m_history[idx];
        if (ms > 0.0001f && ms < 100.0f) {
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
    // Lock-free read
    int histIdx = m_historyIdx.load(std::memory_order_acquire);
    float totalMs = 0.0f;
    int count = 0;
    for (int i = 0; i < HISTORY_SIZE; i++) {
        int idx = (histIdx - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
        float ms = m_history[idx];
        if (ms <= 0.0001f)
            break;
        totalMs += ms;
        count++;
        if (totalMs >= 5000.0f)
            break;
    }

    if (count > 0 && totalMs > 0.0001f) {
        return 1000.0f / (totalMs / count);
    }
    return 0.0f;
}

float PerformanceMetrics::Get1PercentLowFPS() const {
    int histIdx = m_historyIdx.load(std::memory_order_acquire);
    return ComputeWorstPercentileFPS(m_history, histIdx, 0.01f, 10);
}

float PerformanceMetrics::Get01PercentLowFPS() const {
    int histIdx = m_historyIdx.load(std::memory_order_acquire);
    return ComputeWorstPercentileFPS(m_history, histIdx, 0.001f, 100);
}

void PerformanceMetrics::GetLastHistory(float* outBuffer, int count) const {
    // Lock-free: snapshot the atomic index, then read floats
    if (count > HISTORY_SIZE)
        count = HISTORY_SIZE;

    int histIdx = m_historyIdx.load(std::memory_order_acquire);
    for (int i = 0; i < count; i++) {
        int idx = (histIdx - count + i + HISTORY_SIZE) % HISTORY_SIZE;
        outBuffer[i] = m_history[idx];
    }
}

void PerformanceMetrics::GetSmartScale(float& outMin, float& outMax, float minRangeMs) const {
    // Lock-free read
    outMin = 0.0f;
    float maxVal = 0.0f;
    float avgMs = 16.6f;
    int histIdx = m_historyIdx.load(std::memory_order_acquire);

    float totalMs = 0.0f;
    int count = 0;
    for (int i = 0; i < GRAPH_HISTORY_SIZE; i++) {
        int idx = (histIdx - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
        float ms = m_history[idx];
        if (ms > maxVal)
            maxVal = ms;
        if (ms <= 0.0001f)
            break;
        totalMs += ms;
        count++;
    }
    if (count > 0)
        avgMs = totalMs / count;

    float dynamicMin = avgMs * 3.0f;
    float lowerBound = std::max(minRangeMs, dynamicMin);

    if (maxVal < lowerBound) {
        outMax = lowerBound;
    } else {
        outMax = maxVal * 1.1f;
    }
}

float PerformanceMetrics::GetMaxFrameTime(float windowSeconds) const {
    // Lock-free read
    float maxMs = 0.0f;
    int histIdx = m_historyIdx.load(std::memory_order_acquire);
    float accumTime = 0.0f;
    for (int i = 0; i < HISTORY_SIZE; i++) {
        int idx = (histIdx - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
        float ms = m_history[idx];
        if (ms <= 0.0001f)
            break;
        if (ms > maxMs)
            maxMs = ms;
        accumTime += ms;
        if (accumTime >= windowSeconds * 1000.0f)
            break;
    }
    return maxMs;
}
