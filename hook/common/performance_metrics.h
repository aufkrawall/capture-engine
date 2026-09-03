#pragma once

#include "../../common/display_timing_shared.h"
#include "system_latency_metrics.h"

#include <atomic>
#include <cstdint>
#include <mutex>

class PerformanceMetrics {
public:
    static const int HISTORY_SIZE = 8192;       // covers 15s at >500 FPS
    static const int GRAPH_HISTORY_SIZE = 240;  // visual window
    static const int VARIANCE_WINDOW = 120;

    PerformanceMetrics();
    ~PerformanceMetrics() = default;

    // Presentation timestamp source. Kept active even when display timing is
    // selected so fallback and live config changes have warm history.
    void Update(int64_t currentQpcUs);
    // A proven application-rendered source Present. Required while FG is active
    // because Update also sees generated/final-output presents.
    void ObserveApplicationPresent(int64_t currentQpcUs);

    // Import actual screen-change timestamps published by the sensor service.
    // This is intentionally called from the overlay render path so the display
    // series retains a single writer in each injected process.
    void ConsumeDisplayTiming(const SharedDisplayTiming& timing, int64_t currentQpcUs);
    void SubmitNativeLatencyReport(const ce::system_latency::NativeReport& report);
    ce::system_latency::Snapshot GetSystemLatency(int64_t currentQpcUs) const;
    // Evidence about how the published latency was produced. Diagnostic only;
    // read on the bounded telemetry cadence, never per frame.
    ce::system_latency::Diagnostics GetSystemLatencyDiagnostics(int64_t currentQpcUs) const;
    void ResetSystemLatency();
    void SetFrameTimeSource(FrameTimeSource source);
    FrameTimeSource GetEffectiveFrameTimeSource() const {
        return m_effectiveSource.load(std::memory_order_acquire);
    }

    const float* GetHistoryArray() const;
    int GetHistoryIndex() const;
    void GetLastHistory(float* outBuffer, int count) const;
    // Total samples ever appended to the active series. Absolute indices are
    // stable across ring wrap, which is what lets the overlay hold a scroll
    // cursor that does not depend on when it last read.
    uint64_t GetSampleCount() const;
    // Reads `count` samples ending at absolute index `endIndex` (inclusive),
    // oldest first. Indices outside the retained window read back as zero, the
    // same as an unfilled ring.
    void GetHistoryEndingAt(uint64_t endIndex, float* outBuffer, int count) const;
    float GetCurrentFPS() const;

    float GetAverageFPS() const;
    float Get1PercentLowFPS() const;
    float Get01PercentLowFPS() const;

    double GetWindowStdDev() const;
    bool IsStutterDetected() const;

    void GetSmartScale(float& outMin, float& outMax, float minRangeMs = 33.0f) const;
    float GetMaxFrameTime(float windowSeconds) const;

    float GetLastPresentationFrameTimeMs() const {
        const int idx = (m_presentation.historyIdx.load(std::memory_order_acquire) - 1 + HISTORY_SIZE) % HISTORY_SIZE;
        return m_presentation.history[idx];
    }
    float GetLastDisplayFrameTimeMs() const {
        if (m_display.sampleCount.load(std::memory_order_relaxed) == 0) {
            return GetLastPresentationFrameTimeMs();
        }
        const int idx = (m_display.historyIdx.load(std::memory_order_acquire) - 1 + HISTORY_SIZE) % HISTORY_SIZE;
        const float val = m_display.history[idx];
        return val > 0.0f ? val : GetLastPresentationFrameTimeMs();
    }
    bool HasDisplayTimingSamples() const {
        return m_display.sampleCount.load(std::memory_order_relaxed) > 0;
    }

    void SetRecording(bool isRecording);

    // Frame Generation metrics (for displaying base vs output FPS)
    // fgType: 0=None, 1=DLSS_FG, 2=FSR_FG, 3=NVIDIA_SM
    void SetFGMetrics(float outputFPS, float baseFPS, int multiplier, int fgType = 0);
    float GetFGOutputFPS() const {
        return m_fgOutputFPS.load(std::memory_order_relaxed);
    }
    float GetFGBaseFPS() const {
        return m_fgBaseFPS.load(std::memory_order_relaxed);
    }
    int GetFGMultiplier() const {
        return m_fgMultiplier.load(std::memory_order_relaxed);
    }
    bool IsFGActive() const {
        return m_fgMultiplier.load(std::memory_order_relaxed) >= 2;
    }
    const char* GetFGTypeLabel() const {
        switch (m_fgType.load(std::memory_order_relaxed)) {
            case 1:
                return "DLSS FG";
            case 2:
                return "FSR FG";
            case 3:
                return "NVIDIA SM";
            default:
                return "FG";
        }
    }

private:
    struct MetricSeries {
        alignas(64) float history[HISTORY_SIZE];
        std::atomic<int> historyIdx{0};
        std::atomic<uint64_t> sampleCount{0};
        std::atomic<int64_t> lastFrameTimeUs{0};

        int64_t frameTimeWindow[VARIANCE_WINDOW];
        int windowIndex = 0;
        bool windowFilled = false;
        std::atomic<double> windowVariance{0.0};
        std::atomic<double> windowStdDev{0.0};

        bool recordingState = false;
        double baselineMean = 0;
        double baselineM2 = 0;
        int64_t baselineCount = 0;
        double recordingMean = 0;
        double recordingM2 = 0;
        int64_t recordingCount = 0;
        double lastBaselineVariance = 0;
        std::atomic<bool> stutterDetected{false};

        void Reset();
    };

    const MetricSeries& ActiveSeries() const;
    MetricSeries& ActiveSeries();
    void UpdateSeries(MetricSeries& series, int64_t currentQpcUs);
    void ApplyRecordingTransition(MetricSeries& series);
    void RefreshEffectiveSource(const SharedDisplayTiming& timing, int64_t currentQpcUs);

    MetricSeries m_presentation;
    MetricSeries m_display;
    std::atomic<bool> m_recordingRequested{false};
    std::atomic<FrameTimeSource> m_preferredSource{FrameTimeSource::Presentation};
    std::atomic<FrameTimeSource> m_effectiveSource{FrameTimeSource::Presentation};
    std::mutex m_displayConsumeMutex;
    uint64_t m_displayGeneration = UINT64_MAX;
    uint64_t m_nextDisplaySequence = 1;

    std::atomic<float> m_fgOutputFPS{0.0f};
    std::atomic<float> m_fgBaseFPS{0.0f};
    std::atomic<int> m_fgMultiplier{1};
    std::atomic<int> m_fgType{0};
    ce::system_latency::Tracker m_systemLatency;
};
