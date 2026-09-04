#include "performance_metrics.h"

#include "system_latency_frame_begin.h"

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
    windowJaggedness.store(0.0, std::memory_order_relaxed);
    windowStatisticsValid.store(false, std::memory_order_relaxed);
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
    m_systemLatency.SetFrameGeneration(baseFPS, multiplier, fgType);
}

void PerformanceMetrics::Update(int64_t currentQpcUs) {
    // The boundary the game's current CPU frame started from, published by the
    // low-latency sleep hooks. Resolving it here keeps
    // the tracker free of process-wide state so it stays unit-testable.
    ce::system_latency::FrameBeginKind frameBeginKind = ce::system_latency::FrameBeginKind::Modelled;
    const int64_t frameBeginUs = ce::system_latency::LatestFrameBegin(currentQpcUs, frameBeginKind);
    m_systemLatency.ObservePresent(currentQpcUs, frameBeginUs, frameBeginKind);
    UpdateSeries(m_presentation, currentQpcUs);
}

void PerformanceMetrics::ObserveApplicationPresent(int64_t currentQpcUs) {
    ce::system_latency::FrameBeginKind frameBeginKind = ce::system_latency::FrameBeginKind::Modelled;
    const int64_t frameBeginUs = ce::system_latency::LatestFrameBegin(currentQpcUs, frameBeginKind);
    m_systemLatency.ObserveApplicationPresent(currentQpcUs, frameBeginUs, frameBeginKind);
}

void PerformanceMetrics::SubmitNativeLatencyReport(const ce::system_latency::NativeReport& report) {
    if (m_fgType.load(std::memory_order_relaxed) == 2) {
        // FSR FG is driven by FidelityFX; foreign Streamline/NVAPI markers do not track
        // FidelityFX presentations and must not override the proven fallback estimate.
        return;
    }
    m_systemLatency.SubmitNativeReport(report);
}

ce::system_latency::Snapshot PerformanceMetrics::GetSystemLatency(int64_t currentQpcUs) const {
    return m_systemLatency.GetSnapshot(currentQpcUs);
}

ce::system_latency::Diagnostics PerformanceMetrics::GetSystemLatencyDiagnostics(int64_t currentQpcUs) const {
    return m_systemLatency.GetDiagnostics(currentQpcUs);
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
        // Walked oldest to newest, because jaggedness - the mean absolute
        // difference between neighbouring intervals - is the one statistic here
        // that depends on the order. It is what separates a series that is
        // merely spread out from one that alternates, and comparing it against
        // the same statistic on the presentation series is how the frame-time
        // source is chosen; mean and variance do not care about the order and
        // ride along in the same pass.
        const int oldest = series.windowFilled ? series.windowIndex : 0;
        double sum = 0;
        double sumSq = 0;
        double jaggednessSum = 0;
        double previous = 0;
        for (int i = 0; i < count; i++) {
            const double sample =
                static_cast<double>(series.frameTimeWindow[(oldest + i) % VARIANCE_WINDOW]);
            sum += sample;
            sumSq += sample * sample;
            if (i > 0)
                jaggednessSum += std::abs(sample - previous);
            previous = sample;
        }
        const double mean = sum / count;
        const double variance = std::max(0.0, (sumSq / count) - (mean * mean));
        series.windowVariance.store(variance, std::memory_order_relaxed);
        series.windowStdDev.store(std::sqrt(variance), std::memory_order_relaxed);
        series.windowJaggedness.store(count > 1 ? jaggednessSum / (count - 1) : 0.0,
                                      std::memory_order_relaxed);
        series.windowStatisticsValid.store(count > 1, std::memory_order_release);
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
        m_displayScreenTimeCadence.Reset();
        m_displayScreenTimePermille.store(0, std::memory_order_relaxed);
        m_displayGeneration = generationBefore;
        const uint64_t earliestAvailable =
            writeSequence >= DISPLAY_TIMING_RING_SIZE ? writeSequence - DISPLAY_TIMING_RING_SIZE + 1 : 1;
        m_nextDisplaySequence = earliestAvailable;
    }

    const uint64_t earliestAvailable =
        writeSequence >= DISPLAY_TIMING_RING_SIZE ? writeSequence - DISPLAY_TIMING_RING_SIZE + 1 : 1;
    if (m_nextDisplaySequence < earliestAvailable) {
        // Displayed transitions were published faster than they were consumed.
        // The correlator counts retirements, so silently skipping them would
        // inflate its in-flight estimate for the rest of the epoch.
        m_nextDisplaySequence = earliestAvailable;
        m_systemLatency.NoteDisplayStreamGap();
    }

    while (m_nextDisplaySequence <= writeSequence) {
        int64_t screenTimeUs = 0;
        int64_t presentStartTimeUs = 0;
        bool screenTimeResolved = false;
        if (!timing.Read(m_nextDisplaySequence, screenTimeUs, presentStartTimeUs, screenTimeResolved))
            break;
        m_systemLatency.ObserveDisplay(screenTimeUs, presentStartTimeUs);
        // The series stays warm whatever the provenance is: an unresolved
        // timestamp is still an ordered displayed transition, it is only its
        // *interval* that cannot be trusted, and a stream that starts resolving
        // again must not have to refill its history first.
        m_displayScreenTimeCadence.Observe(screenTimeResolved);
        UpdateSeries(m_display, screenTimeUs);
        ++m_nextDisplaySequence;
    }
    m_displayScreenTimePermille.store(m_displayScreenTimeCadence.permille(), std::memory_order_relaxed);

    RefreshEffectiveSource(timing, currentQpcUs);
}

void PerformanceMetrics::RefreshEffectiveSource(const SharedDisplayTiming& timing, int64_t currentQpcUs) {
    // A live stream that is publishing flip-latch timestamps is not a screen
    // clock, however many samples it delivers at however correct a mean. The
    // frames were presented evenly and only their reported *screen* times are
    // jittering, so presentation timing - the same frames, measured where the
    // measurement is exact - is the honest series to draw and to compute lows
    // and variance from until the display clock can answer again. Judged before
    // the preference is consulted, so the diagnostic never goes stale on a
    // configuration that was not going to select the stream anyway.
    const bool alreadySelected =
        m_effectiveSource.load(std::memory_order_relaxed) == FrameTimeSource::DisplayChange;
    const bool provenSamples = m_displayScreenTimeCadence.IsScreenTime(alreadySelected);
    // Provenance is a per-sample fact, and a stream can be a screen clock while
    // a minority of its samples are not. Measured under DLSS FG, four fifths of
    // the completions are immediate flips carrying the driver's announced
    // screen time and the rest are deferred and unresolved, yet the published
    // series is flat at 450 us of jaggedness while the presents that produced
    // it - issued as a generated group in a burst - carry 18244 us. Refusing
    // that series because a fifth of it is unlabelled would throw away the one
    // measurement that shows what the screen did.
    //
    // So a series is also accepted when it is measurably not adding jitter to
    // the frames it measures: no jaggier than the presentation series over the
    // same window. That is a statement about the values rather than about their
    // labels, it needs no absolute threshold, and it fails exactly where the
    // labels already said it should - under FSR FG below the refresh cap the
    // presents are even at 351-540 us while the display series is 2043-4575 us,
    // which is the noise this gate exists to keep off the graph.
    const double displayJaggednessUs = m_display.windowJaggedness.load(std::memory_order_relaxed);
    const double presentJaggednessUs = m_presentation.windowJaggedness.load(std::memory_order_relaxed);
    // Allow up to 1.5x presentation jaggedness to select DisplayChange (admitting
    // normal VRR streams with minor DPC jitter), and 2.0x hysteresis once already
    // selected. Under FSR FG the flip-latch clock is 4x-8x jaggier than presents
    // (3000-4500 us vs ~350-540 us), so it remains firmly rejected.
    const double allowedJaggednessUs =
        alreadySelected ? presentJaggednessUs * 2.0 : presentJaggednessUs * 1.5;
    const bool bothSeriesMeasured = m_display.windowStatisticsValid.load(std::memory_order_acquire) &&
                                    m_presentation.windowStatisticsValid.load(std::memory_order_acquire);
    const bool flatterThanPresents = bothSeriesMeasured && displayJaggednessUs <= allowedJaggednessUs;
    const bool screenTime = provenSamples || flatterThanPresents;
    m_displayStreamIsScreenTime.store(screenTime, std::memory_order_release);

    if (m_preferredSource.load(std::memory_order_acquire) == FrameTimeSource::Presentation) {
        m_effectiveSource.store(FrameTimeSource::Presentation, std::memory_order_release);
        return;
    }

    const int64_t lastPublishUs = timing.lastPublishQpcUs.load(std::memory_order_acquire);
    const bool recent = lastPublishUs > 0 && currentQpcUs >= lastPublishUs &&
                        currentQpcUs - lastPublishUs <= kDisplayTimingStaleThresholdUs;
    const bool healthy = timing.GetStatus() == DisplayTimingStatus::Active && recent &&
                         m_display.sampleCount.load(std::memory_order_acquire) > 0;
    m_effectiveSource.store(healthy && screenTime ? FrameTimeSource::DisplayChange
                                                  : FrameTimeSource::Presentation,
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
