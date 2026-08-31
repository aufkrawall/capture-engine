#include "performance_metrics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {
constexpr int64_t kDuplicateFrameThresholdUs = 100;
constexpr int64_t kDisplayTimingStaleThresholdUs = 2'000'000;

float ComputeWorstPercentileFPS(const float* history, int historyIdx, float percentile, int minSamples) {
    static thread_local std::array<float, PerformanceMetrics::HISTORY_SIZE> frameTimes;
    int count = 0;
    float totalMs = 0.0f;

    for (int i = 0; i < PerformanceMetrics::HISTORY_SIZE; i++) {
        const int idx =
            (historyIdx - 1 - i + PerformanceMetrics::HISTORY_SIZE) % PerformanceMetrics::HISTORY_SIZE;
        const float ms = history[idx];
        if (ms <= 0.0001f)
            break;
        frameTimes[count++] = ms;
        totalMs += ms;
        if (totalMs >= 5000.0f)
            break;
    }

    if (count < minSamples)
        return 0.0f;

    const int percentileIdx = std::min(count - 1, static_cast<int>(static_cast<float>(count) * percentile));
    const int worstCount = std::max(1, percentileIdx + 1);
    auto begin = frameTimes.begin();
    std::nth_element(begin, begin + worstCount, begin + count, std::greater<float>());

    float sum = 0.0f;
    for (int i = 0; i < worstCount; i++)
        sum += frameTimes[i];
    return 1000.0f / (sum / static_cast<float>(worstCount));
}
}  // namespace

void PerformanceMetrics::MetricSeries::Reset() {
    std::memset(history, 0, sizeof(history));
    std::memset(frameTimeWindow, 0, sizeof(frameTimeWindow));
    historyIdx.store(0, std::memory_order_relaxed);
    sampleCount.store(0, std::memory_order_relaxed);
    lastFrameTimeUs.store(0, std::memory_order_relaxed);
    windowIndex = 0;
    windowFilled = false;
    windowVariance.store(0.0, std::memory_order_relaxed);
    windowStdDev.store(0.0, std::memory_order_relaxed);
    recordingState = false;
    baselineMean = 0;
    baselineM2 = 0;
    baselineCount = 0;
    recordingMean = 0;
    recordingM2 = 0;
    recordingCount = 0;
    lastBaselineVariance = 0;
    stutterDetected.store(false, std::memory_order_relaxed);
}

PerformanceMetrics::PerformanceMetrics() {
    m_presentation.Reset();
    m_display.Reset();
}

const PerformanceMetrics::MetricSeries& PerformanceMetrics::ActiveSeries() const {
    return m_effectiveSource.load(std::memory_order_acquire) == FrameTimeSource::DisplayChange ? m_display
                                                                                              : m_presentation;
}

PerformanceMetrics::MetricSeries& PerformanceMetrics::ActiveSeries() {
    return m_effectiveSource.load(std::memory_order_acquire) == FrameTimeSource::DisplayChange ? m_display
                                                                                              : m_presentation;
}

void PerformanceMetrics::SetRecording(bool isRecording) {
    m_recordingRequested.store(isRecording, std::memory_order_release);
}

void PerformanceMetrics::ApplyRecordingTransition(MetricSeries& series) {
    const bool requested = m_recordingRequested.load(std::memory_order_acquire);
    if (series.recordingState == requested)
        return;

    if (requested) {
        if (series.baselineCount > VARIANCE_WINDOW)
            series.lastBaselineVariance = series.baselineM2 / static_cast<double>(series.baselineCount);
        series.recordingMean = 0;
        series.recordingM2 = 0;
        series.recordingCount = 0;
        series.stutterDetected.store(false, std::memory_order_relaxed);
    }
    series.recordingState = requested;
}

void PerformanceMetrics::SetFGMetrics(float outputFPS, float baseFPS, int multiplier, int fgType) {
    m_fgOutputFPS.store(outputFPS, std::memory_order_relaxed);
    m_fgBaseFPS.store(baseFPS, std::memory_order_relaxed);
    m_fgMultiplier.store(multiplier, std::memory_order_relaxed);
    m_fgType.store(fgType, std::memory_order_relaxed);
    m_systemLatency.SetFrameGeneration(baseFPS, multiplier);
}

void PerformanceMetrics::Update(int64_t currentQpcUs) {
    m_systemLatency.ObservePresent(currentQpcUs);
    UpdateSeries(m_presentation, currentQpcUs);
}

void PerformanceMetrics::SubmitNativeLatencyReport(const ce::system_latency::NativeReport& report) {
    m_systemLatency.SubmitNativeReport(report);
}

ce::system_latency::Snapshot PerformanceMetrics::GetSystemLatency(int64_t currentQpcUs) const {
    return m_systemLatency.GetSnapshot(currentQpcUs);
}

void PerformanceMetrics::ResetSystemLatency() {
    m_systemLatency.ResetDisplayHistory();
}

void PerformanceMetrics::UpdateSeries(MetricSeries& series, int64_t currentQpcUs) {
    const int64_t lastUs = series.lastFrameTimeUs.load(std::memory_order_relaxed);
    const int64_t frameToFrameUs = lastUs > 0 ? currentQpcUs - lastUs : 0;

    if (lastUs > 0 && frameToFrameUs > 0 && frameToFrameUs < kDuplicateFrameThresholdUs)
        return;
    if (lastUs > 0 && frameToFrameUs <= 0)
        return;

    series.lastFrameTimeUs.store(currentQpcUs, std::memory_order_relaxed);
    if (frameToFrameUs <= 0)
        return;

    ApplyRecordingTransition(series);

    const float frameTimeMs = static_cast<float>(frameToFrameUs) / 1000.0f;
    const double frameToFrame = static_cast<double>(frameToFrameUs);
    const int idx = series.historyIdx.load(std::memory_order_relaxed);
    series.history[idx] = frameTimeMs;
    series.historyIdx.store((idx + 1) % HISTORY_SIZE, std::memory_order_release);
    series.sampleCount.fetch_add(1, std::memory_order_relaxed);

    series.frameTimeWindow[series.windowIndex] = frameToFrameUs;
    series.windowIndex = (series.windowIndex + 1) % VARIANCE_WINDOW;
    if (series.windowIndex == 0)
        series.windowFilled = true;

    if (series.recordingState) {
        series.recordingCount++;
        const double delta = frameToFrame - series.recordingMean;
        series.recordingMean += delta / static_cast<double>(series.recordingCount);
        const double delta2 = frameToFrame - series.recordingMean;
        series.recordingM2 += delta * delta2;
    } else {
        series.baselineCount++;
        const double delta = frameToFrame - series.baselineMean;
        series.baselineMean += delta / static_cast<double>(series.baselineCount);
        const double delta2 = frameToFrame - series.baselineMean;
        series.baselineM2 += delta * delta2;
    }

    if (series.windowFilled || series.windowIndex > 10) {
        const int count = series.windowFilled ? VARIANCE_WINDOW : series.windowIndex;
        double sum = 0;
        double sumSq = 0;
        for (int i = 0; i < count; i++) {
            const double sample = static_cast<double>(series.frameTimeWindow[i]);
            sum += sample;
            sumSq += sample * sample;
        }
        const double mean = sum / count;
        const double variance = std::max(0.0, (sumSq / count) - (mean * mean));
        series.windowVariance.store(variance, std::memory_order_relaxed);
        series.windowStdDev.store(std::sqrt(variance), std::memory_order_relaxed);
    }

    if (series.recordingState && series.recordingCount > 240 && series.lastBaselineVariance > 1.0) {
        const double currentVariance = series.recordingM2 / static_cast<double>(series.recordingCount);
        const double ratio = currentVariance / series.lastBaselineVariance;
        if (ratio > 2.0)
            series.stutterDetected.store(true, std::memory_order_relaxed);
        else if (ratio < 1.5)
            series.stutterDetected.store(false, std::memory_order_relaxed);
    }
}

void PerformanceMetrics::SetFrameTimeSource(FrameTimeSource source) {
    m_preferredSource.store(source, std::memory_order_release);
    if (source == FrameTimeSource::Presentation)
        m_effectiveSource.store(FrameTimeSource::Presentation, std::memory_order_release);
}

void PerformanceMetrics::ConsumeDisplayTiming(const SharedDisplayTiming& timing, int64_t currentQpcUs) {
    std::lock_guard<std::mutex> lock(m_displayConsumeMutex);
    const uint64_t generationBefore = timing.publicationGeneration.load(std::memory_order_acquire);
    if ((generationBefore & 1u) != 0)
        return;

    const uint64_t writeSequence = timing.writeSequence.load(std::memory_order_acquire);
    const uint64_t generationAfter = timing.publicationGeneration.load(std::memory_order_acquire);
    if (generationBefore != generationAfter)
        return;

    if (m_displayGeneration != generationBefore) {
        m_display.Reset();
        m_systemLatency.ResetDisplayHistory();
        m_displayGeneration = generationBefore;
        const uint64_t earliestAvailable =
            writeSequence >= DISPLAY_TIMING_RING_SIZE ? writeSequence - DISPLAY_TIMING_RING_SIZE + 1 : 1;
        m_nextDisplaySequence = earliestAvailable;
    }

    const uint64_t earliestAvailable =
        writeSequence >= DISPLAY_TIMING_RING_SIZE ? writeSequence - DISPLAY_TIMING_RING_SIZE + 1 : 1;
    if (m_nextDisplaySequence < earliestAvailable)
        m_nextDisplaySequence = earliestAvailable;

    while (m_nextDisplaySequence <= writeSequence) {
        int64_t screenTimeUs = 0;
        if (!timing.Read(m_nextDisplaySequence, screenTimeUs))
            break;
        m_systemLatency.ObserveDisplay(screenTimeUs);
        UpdateSeries(m_display, screenTimeUs);
        ++m_nextDisplaySequence;
    }

    RefreshEffectiveSource(timing, currentQpcUs);
}

void PerformanceMetrics::RefreshEffectiveSource(const SharedDisplayTiming& timing, int64_t currentQpcUs) {
    if (m_preferredSource.load(std::memory_order_acquire) == FrameTimeSource::Presentation) {
        m_effectiveSource.store(FrameTimeSource::Presentation, std::memory_order_release);
        return;
    }

    const int64_t lastPublishUs = timing.lastPublishQpcUs.load(std::memory_order_acquire);
    const bool recent = lastPublishUs > 0 && currentQpcUs >= lastPublishUs &&
                        currentQpcUs - lastPublishUs <= kDisplayTimingStaleThresholdUs;
    const bool healthy = timing.GetStatus() == DisplayTimingStatus::Active && recent &&
                         m_display.sampleCount.load(std::memory_order_acquire) > 0;
    m_effectiveSource.store(healthy ? FrameTimeSource::DisplayChange : FrameTimeSource::Presentation,
                            std::memory_order_release);
}

const float* PerformanceMetrics::GetHistoryArray() const {
    return ActiveSeries().history;
}

int PerformanceMetrics::GetHistoryIndex() const {
    return ActiveSeries().historyIdx.load(std::memory_order_acquire);
}

float PerformanceMetrics::GetCurrentFPS() const {
    const auto& series = ActiveSeries();
    constexpr int kAverageFrames = 60;
    float totalMs = 0.0f;
    int validFrames = 0;
    const int historyIdx = series.historyIdx.load(std::memory_order_acquire);
    for (int i = 0; i < kAverageFrames; i++) {
        const int idx = (historyIdx - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
        const float ms = series.history[idx];
        if (ms > 0.0001f && ms < 100.0f) {
            totalMs += ms;
            validFrames++;
        }
    }
    return validFrames > 0 && totalMs > 0.0001f
               ? 1000.0f / (totalMs / static_cast<float>(validFrames))
               : 0.0f;
}

float PerformanceMetrics::GetAverageFPS() const {
    const auto& series = ActiveSeries();
    const int historyIdx = series.historyIdx.load(std::memory_order_acquire);
    float totalMs = 0.0f;
    int count = 0;
    for (int i = 0; i < HISTORY_SIZE; i++) {
        const int idx = (historyIdx - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
        const float ms = series.history[idx];
        if (ms <= 0.0001f)
            break;
        totalMs += ms;
        count++;
        if (totalMs >= 5000.0f)
            break;
    }
    return count > 0 && totalMs > 0.0001f ? 1000.0f / (totalMs / static_cast<float>(count)) : 0.0f;
}

float PerformanceMetrics::Get1PercentLowFPS() const {
    const auto& series = ActiveSeries();
    return ComputeWorstPercentileFPS(series.history, series.historyIdx.load(std::memory_order_acquire), 0.01f, 10);
}

float PerformanceMetrics::Get01PercentLowFPS() const {
    const auto& series = ActiveSeries();
    return ComputeWorstPercentileFPS(series.history, series.historyIdx.load(std::memory_order_acquire), 0.001f, 100);
}

uint64_t PerformanceMetrics::GetSampleCount() const {
    return ActiveSeries().sampleCount.load(std::memory_order_acquire);
}

void PerformanceMetrics::GetHistoryEndingAt(uint64_t endIndex, float* outBuffer, int count) const {
    if (!outBuffer || count <= 0)
        return;
    count = std::min(count, HISTORY_SIZE);
    const auto& series = ActiveSeries();
    // Sample n occupies slot n % HISTORY_SIZE, so an absolute index maps to a
    // slot directly and the retained window is the last HISTORY_SIZE indices.
    const uint64_t written = series.sampleCount.load(std::memory_order_acquire);
    const uint64_t oldest =
        written > static_cast<uint64_t>(HISTORY_SIZE) ? written - static_cast<uint64_t>(HISTORY_SIZE) : 0;
    for (int i = 0; i < count; i++) {
        const uint64_t age = static_cast<uint64_t>(count - 1 - i);
        if (endIndex < age) {
            outBuffer[i] = 0.0f;
            continue;
        }
        const uint64_t index = endIndex - age;
        outBuffer[i] = (index < oldest || index >= written)
                           ? 0.0f
                           : series.history[static_cast<std::size_t>(index % static_cast<uint64_t>(HISTORY_SIZE))];
    }
}

void PerformanceMetrics::GetLastHistory(float* outBuffer, int count) const {
    count = std::min(count, HISTORY_SIZE);
    const auto& series = ActiveSeries();
    const int historyIdx = series.historyIdx.load(std::memory_order_acquire);
    for (int i = 0; i < count; i++) {
        const int idx = (historyIdx - count + i + HISTORY_SIZE) % HISTORY_SIZE;
        outBuffer[i] = series.history[idx];
    }
}

double PerformanceMetrics::GetWindowStdDev() const {
    return ActiveSeries().windowStdDev.load(std::memory_order_relaxed);
}

bool PerformanceMetrics::IsStutterDetected() const {
    return ActiveSeries().stutterDetected.load(std::memory_order_relaxed);
}

void PerformanceMetrics::GetSmartScale(float& outMin, float& outMax, float minRangeMs) const {
    const auto& series = ActiveSeries();
    outMin = 0.0f;
    float maxValue = 0.0f;
    float averageMs = 16.6f;
    const int historyIdx = series.historyIdx.load(std::memory_order_acquire);
    float totalMs = 0.0f;
    int count = 0;
    for (int i = 0; i < GRAPH_HISTORY_SIZE; i++) {
        const int idx = (historyIdx - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
        const float ms = series.history[idx];
        maxValue = std::max(maxValue, ms);
        if (ms <= 0.0001f)
            break;
        totalMs += ms;
        count++;
    }
    if (count > 0)
        averageMs = totalMs / static_cast<float>(count);
    const float lowerBound = std::max(minRangeMs, averageMs * 3.0f);
    outMax = maxValue < lowerBound ? lowerBound : maxValue * 1.1f;
}

float PerformanceMetrics::GetMaxFrameTime(float windowSeconds) const {
    const auto& series = ActiveSeries();
    float maxMs = 0.0f;
    float accumulatedMs = 0.0f;
    const int historyIdx = series.historyIdx.load(std::memory_order_acquire);
    for (int i = 0; i < HISTORY_SIZE; i++) {
        const int idx = (historyIdx - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
        const float ms = series.history[idx];
        if (ms <= 0.0001f)
            break;
        maxMs = std::max(maxMs, ms);
        accumulatedMs += ms;
        if (accumulatedMs >= windowSeconds * 1000.0f)
            break;
    }
    return maxMs;
}
