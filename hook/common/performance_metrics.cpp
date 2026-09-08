#include "performance_metrics.h"

#include "hook_common.h"
#include "pacing_health_telemetry.h"
#include "pacing_trace.h"
#include "perf_logger.h"
#include "system_latency_frame_begin.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {
constexpr int64_t kDuplicateFrameThresholdUs = 100;
constexpr int64_t kDisplayTimingStaleThresholdUs = 2'000'000;

float ComputeWorstPercentileFPS(const std::atomic<float>* history, int historyIdx, float percentile, int minSamples) {
    static thread_local std::array<float, PerformanceMetrics::HISTORY_SIZE> frameTimes;
    int count = 0;
    float totalMs = 0.0f;

    for (int i = 0; i < PerformanceMetrics::HISTORY_SIZE; i++) {
        const int idx =
            (historyIdx - 1 - i + PerformanceMetrics::HISTORY_SIZE) % PerformanceMetrics::HISTORY_SIZE;
        const float ms = history[idx].load(std::memory_order_relaxed);
        if (ms <= 0.0001f)
            break;
        frameTimes[count++] = ms;
        totalMs += ms;
        if (totalMs >= 5000.0f)
            break;
    }

    if (count < minSamples)
        return 0.0f;

    const int worstCount = std::min(count, std::max(1, static_cast<int>(
        std::ceil(static_cast<float>(count) * percentile))));
    auto begin = frameTimes.begin();
    std::nth_element(begin, begin + worstCount, begin + count, std::greater<float>());

    float sum = 0.0f;
    for (int i = 0; i < worstCount; i++)
        sum += frameTimes[i];
    return 1000.0f / (sum / static_cast<float>(worstCount));
}
}  // namespace

void PerformanceMetrics::MetricSeries::Reset() {
    for (auto& sample : history)
        sample.store(0.0f, std::memory_order_relaxed);
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
    {
        std::lock_guard<std::mutex> lock(m_presentationUpdateMutex);
        UpdateSeries(m_presentation, currentQpcUs);
    }
    MaybeLogPacingHealth(currentQpcUs);
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

    // Only the presentation observer needs a nested-hook duplicate guard.
    // Distinct display-ring sequences are distinct transitions, even when a
    // tearing display or driver reports them less than 100 us apart.
    if (&series == &m_presentation && lastUs > 0 && frameToFrameUs > 0 &&
        frameToFrameUs < kDuplicateFrameThresholdUs)
        return;
    if (lastUs > 0 && frameToFrameUs <= 0)
        return;

    series.lastFrameTimeUs.store(currentQpcUs, std::memory_order_relaxed);
    if (frameToFrameUs <= 0)
        return;

    const bool displaySeries = &series == &m_display;
    const uint64_t fsrTag = m_fsrPacingTag.load(std::memory_order_acquire);
    auto& needsFsrAnchor = displaySeries ? m_fsrDisplayNeedsAnchor : m_fsrPresentationNeedsAnchor;
    const uint64_t sampleTag = fsrTag != 0 && !needsFsrAnchor.exchange(false, std::memory_order_acq_rel)
                                   ? fsrTag
                                   : 0;
    ce::pacing_health::Observe(displaySeries ? ce::pacing_health::Channel::kDisplay
                                             : ce::pacing_health::Channel::kPresentation,
                               frameToFrameUs, currentQpcUs, sampleTag);

    ApplyRecordingTransition(series);

    const float frameTimeMs = static_cast<float>(frameToFrameUs) / 1000.0f;
    const double frameToFrame = static_cast<double>(frameToFrameUs);
    const int idx = series.historyIdx.load(std::memory_order_relaxed);
    series.history[idx].store(frameTimeMs, std::memory_order_relaxed);
    series.historyIdx.store((idx + 1) % HISTORY_SIZE, std::memory_order_release);
    series.sampleCount.store(series.sampleCount.load(std::memory_order_relaxed) + 1, std::memory_order_release);

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
        // that depends on the order. It distinguishes a spread-out series from
        // an alternating one for diagnostics; it never selects or smooths the
        // frame-time source. Mean and variance ride along in the same pass.
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
        ce::pacing_trace::Invalidate(currentQpcUs);
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
        // Missing telemetry is not one long displayed frame. Retain measured
        // history, but start a fresh interval at the first available timestamp.
        m_display.lastFrameTimeUs.store(0, std::memory_order_relaxed);
        m_displayScreenTimeCadence.Reset();
        m_systemLatency.NoteDisplayStreamGap();
    }

    while (m_nextDisplaySequence <= writeSequence) {
        int64_t screenTimeUs = 0;
        int64_t presentStartTimeUs = 0;
        bool screenTimeResolved = false;
        if (!timing.Read(m_nextDisplaySequence, screenTimeUs, presentStartTimeUs, screenTimeResolved))
            break;
        // A reset restarts sequence numbers. Even a coherent sample read must
        // belong to the generation whose cursor/history we are consuming.
        if (timing.publicationGeneration.load(std::memory_order_acquire) != generationBefore)
            return;
        m_systemLatency.ObserveDisplay(screenTimeUs, presentStartTimeUs);
        ce::pacing_trace::Record(ce::pacing_trace::Kind::DisplayPair, m_nextDisplaySequence, nullptr,
            presentStartTimeUs, generationBefore, 0, screenTimeResolved ? 1u : 0u, screenTimeUs);
        const uint64_t fsrTag = m_fsrPacingTag.load(std::memory_order_acquire);
        if (presentStartTimeUs > 0 && screenTimeUs >= presentStartTimeUs) {
            ce::pacing_health::Observe(ce::pacing_health::Channel::kPresentToDisplay,
                                       screenTimeUs - presentStartTimeUs, screenTimeUs, fsrTag);
        }
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
    // Provenance is a producer claim, not something inferred from flatness.
    // A jagged display can be correct and a flat unlabelled stream can still
    // be wrong. This diagnostic never participates in source selection.
    m_displayStreamIsScreenTime.store(m_displayScreenTimeCadence.IsScreenTime(),
                                      std::memory_order_release);

    if (m_preferredSource.load(std::memory_order_acquire) == FrameTimeSource::Presentation) {
        m_effectiveSource.store(FrameTimeSource::Presentation, std::memory_order_release);
        return;
    }

    const int64_t lastPublishUs = timing.lastPublishQpcUs.load(std::memory_order_acquire);
    // currentQpcUs was captured before taking the consumer lock. The sensor
    // can publish while we acquire it or drain the ring, so a newer publication
    // is fresh, not a clock failure. Reject only genuinely old publications.
    const bool recent = lastPublishUs > 0 && currentQpcUs > 0 &&
                        (lastPublishUs >= currentQpcUs ||
                         currentQpcUs - lastPublishUs <= kDisplayTimingStaleThresholdUs);
    const bool healthy = timing.GetStatus() == DisplayTimingStatus::Active && recent &&
                         m_display.sampleCount.load(std::memory_order_acquire) > 0;
    // When DisplayChange is preferred, display the active screen timing stream directly so
    // the frame-time graph faithfully reflects real on-screen frame pacing (msBetweenDisplayChange)
    // with VRR, GPU maxed out, VSync capping, and uncapped FPS, without sugarcoating.
    m_effectiveSource.store(healthy ? FrameTimeSource::DisplayChange : FrameTimeSource::Presentation,
                            std::memory_order_release);
}

const std::atomic<float>* PerformanceMetrics::GetHistoryArray() const {
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
        const float ms = series.history[idx].load(std::memory_order_relaxed);
        if (ms > 0.0001f) {
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
        const float ms = series.history[idx].load(std::memory_order_relaxed);
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
                           : series.history[static_cast<std::size_t>(index % static_cast<uint64_t>(HISTORY_SIZE))].load(std::memory_order_relaxed);
    }
}

void PerformanceMetrics::GetLastHistory(float* outBuffer, int count) const {
    count = std::min(count, HISTORY_SIZE);
    const auto& series = ActiveSeries();
    const int historyIdx = series.historyIdx.load(std::memory_order_acquire);
    for (int i = 0; i < count; i++) {
        const int idx = (historyIdx - count + i + HISTORY_SIZE) % HISTORY_SIZE;
        outBuffer[i] = series.history[idx].load(std::memory_order_relaxed);
    }
}

double PerformanceMetrics::GetWindowStdDev() const {
    return ActiveSeries().windowStdDev.load(std::memory_order_relaxed);
}

bool PerformanceMetrics::IsStutterDetected() const {
    return ActiveSeries().stutterDetected.load(std::memory_order_relaxed);
}

void PerformanceMetrics::NotifyOverlayCallbackDraw(bool generatedFrame, bool drewOverlay) {
    if (m_fsrPacingTag.load(std::memory_order_relaxed) == 0)
        return;
    if (generatedFrame) {
        if (drewOverlay) {
            m_callbackGeneratedDraws.fetch_add(1, std::memory_order_relaxed);
        } else {
            m_callbackGeneratedSkips.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (drewOverlay) {
        m_callbackAppDraws.fetch_add(1, std::memory_order_relaxed);
    }
}

void PerformanceMetrics::LogActivationCadenceContext(const char* site) {
    const int64_t nowUs = PerfLogger::GetQpcUs();
    const auto display = ce::pacing_health::RecentCadence(ce::pacing_health::Channel::kDisplay, 120);
    const auto presentation = ce::pacing_health::RecentCadence(ce::pacing_health::Channel::kPresentation, 120);
    const auto latency = GetSystemLatencyDiagnostics(nowUs);
    HookLogImportant(
        "[FSRActivationCadence] site=%s dispMedUs=%u dispN=%u presMedUs=%u presN=%u applicationIntervalUs=%lld "
        "displayIntervalUs=%lld",
        site && site[0] ? site : "unknown", display.medianUs, display.samples, presentation.medianUs,
        presentation.samples, static_cast<long long>(latency.applicationIntervalUs),
        static_cast<long long>(latency.displayIntervalUs));
}

void PerformanceMetrics::NotifyFSRFrameGenerationTransition(bool enabled, const char* site) {
    const int64_t nowUs = PerfLogger::GetQpcUs();
    uint64_t tag = 0;
    {
        std::lock_guard<std::mutex> lock(m_pacingHealthMutex);
        const bool alreadyEnabled = m_fsrPacingTag.load(std::memory_order_acquire) != 0;
        if (enabled == alreadyEnabled)
            return;
        if (enabled) {
            tag = m_nextFsrPacingTag.fetch_add(1, std::memory_order_relaxed) + 1;
            m_fsrPacingTag.store(tag, std::memory_order_release);
            if (m_pacingHealthWindowStartUs == 0) {
                m_pacingHealthWindowStartUs = nowUs;
                m_nextPacingHealthLogUs.store(nowUs + 10'000'000, std::memory_order_release);
            }
        } else {
            m_fsrPacingTag.store(0, std::memory_order_release);
        }
        m_fsrPresentationNeedsAnchor.store(true, std::memory_order_release);
        m_fsrDisplayNeedsAnchor.store(true, std::memory_order_release);
        ce::pacing_trace::Epoch(tag, nowUs);
    }
    HookLogImportant("[FSRPacingEpoch] state=%s tag=%llu site=%s",
                     enabled ? "enabled" : "disabled", static_cast<unsigned long long>(tag),
                     site && site[0] ? site : "unknown");
}

void PerformanceMetrics::MaybeLogPacingHealth(int64_t currentQpcUs) {
    // The FSR FG degradation is a wide physical-completion distribution while
    // PresentStart stays smooth, previously recoverable only from offline CSV
    // analysis. This line carries both distributions and their classification.
    // the classification directly. Nothing here runs on the hot path beyond one
    // timestamp comparison per present.
    static constexpr int64_t kLogIntervalUs = 10'000'000;
    if (m_fsrPacingTag.load(std::memory_order_acquire) == 0) {
        return;
    }
    int64_t nextLogUs = m_nextPacingHealthLogUs.load(std::memory_order_acquire);
    if (nextLogUs == 0 || currentQpcUs < nextLogUs) {
        return;
    }
    // Serialized tick: two racing present threads must not double-log, and the
    // loser simply waits for the next window.
    if (!m_nextPacingHealthLogUs.compare_exchange_strong(nextLogUs, currentQpcUs + kLogIntervalUs,
                                                         std::memory_order_acq_rel)) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_pacingHealthMutex);
    if (m_fsrPacingTag.load(std::memory_order_acquire) == 0)
        return;
    const int64_t windowStartUs = m_pacingHealthWindowStartUs;
    m_pacingHealthWindowStartUs = currentQpcUs;
    const auto display = ce::pacing_health::SnapshotTaggedWindow(
        ce::pacing_health::Channel::kDisplay, windowStartUs, currentQpcUs);
    const auto presentation = ce::pacing_health::SnapshotTaggedWindow(
        ce::pacing_health::Channel::kPresentation, windowStartUs, currentQpcUs);
    const auto presentToDisplay = ce::pacing_health::SnapshotTaggedWindow(
        ce::pacing_health::Channel::kPresentToDisplay, windowStartUs, currentQpcUs);
    const auto signature = ce::pacing_health::Classify(display, presentation);
    const uint64_t appDraws = m_callbackAppDraws.exchange(0, std::memory_order_relaxed);
    const uint64_t generatedDraws = m_callbackGeneratedDraws.exchange(0, std::memory_order_relaxed);
    const uint64_t generatedSkips = m_callbackGeneratedSkips.exchange(0, std::memory_order_relaxed);
    HookLogImportant(
        "[FSRPacingHealth] signature=%s windowMs=%lld segments=%u mult=%d baseFps=%.1f outFps=%.1f "
        "disp med=%uu p95=%uu sd=%uu late=%upermille max=%uu n=%u | "
        "pres med=%uu p95=%uu sd=%uu late=%upermille max=%uu n=%u | "
        "presentToDisplay mean=%uu p95=%uu sd=%uu min=%uu max=%uu n=%u | "
        "cbDraws app=%llu gen=%llu genSkip=%llu",
        ce::pacing_health::SignatureName(signature), static_cast<long long>((currentQpcUs - windowStartUs) / 1000),
        std::max(display.segments, presentation.segments), GetFGMultiplier(), GetFGBaseFPS(), GetFGOutputFPS(),
        display.medianUs, display.p95Us, display.stddevUs, display.latePermille, display.maxUs, display.samples,
        presentation.medianUs, presentation.p95Us, presentation.stddevUs, presentation.latePermille,
        presentation.maxUs, presentation.samples, presentToDisplay.meanUs, presentToDisplay.p95Us,
        presentToDisplay.stddevUs, presentToDisplay.minUs, presentToDisplay.maxUs, presentToDisplay.samples,
        static_cast<unsigned long long>(appDraws), static_cast<unsigned long long>(generatedDraws),
        static_cast<unsigned long long>(generatedSkips));
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
        const float ms = series.history[idx].load(std::memory_order_relaxed);
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
        const float ms = series.history[idx].load(std::memory_order_relaxed);
        if (ms <= 0.0001f)
            break;
        maxMs = std::max(maxMs, ms);
        accumulatedMs += ms;
        if (accumulatedMs >= windowSeconds * 1000.0f)
            break;
    }
    return maxMs;
}
