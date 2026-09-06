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
    // Diagnostic producer-provenance share. It never selects the metric source.
    bool IsDisplayStreamScreenTime() const {
        return m_displayStreamIsScreenTime.load(std::memory_order_acquire);
    }
    // Share of recent display publications that carried a resolved screen time,
    // in per mille. Zero until the stream has produced any sample.
    uint32_t GetDisplayScreenTimePermille() const {
        return m_displayScreenTimePermille.load(std::memory_order_relaxed);
    }
    // Mean absolute difference between neighbouring frame-time samples, per
    // series. Diagnostic only: variance never selects the frame-time source.
    double GetDisplayJaggednessUs() const {
        return m_display.windowJaggedness.load(std::memory_order_relaxed);
    }
    double GetPresentationJaggednessUs() const {
        return m_presentation.windowJaggedness.load(std::memory_order_relaxed);
    }

    const std::atomic<float>* GetHistoryArray() const;
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
        return m_presentation.history[idx].load(std::memory_order_relaxed);
    }
    // Falls back to the presentation frame time when there is no display series yet.
    float GetLastDisplayFrameTimeMs() const {
        if (m_display.sampleCount.load(std::memory_order_relaxed) == 0) {
            return GetLastPresentationFrameTimeMs();
        }
        const int idx = (m_display.historyIdx.load(std::memory_order_acquire) - 1 + HISTORY_SIZE) % HISTORY_SIZE;
        const float val = m_display.history[idx].load(std::memory_order_relaxed);
        return val > 0.0f ? val : GetLastPresentationFrameTimeMs();
    }
    bool HasDisplayTimingSamples() const {
        return m_display.sampleCount.load(std::memory_order_relaxed) > 0;
    }

    void SetRecording(bool isRecording);

    // Counts FFX present-callback overlay draws for the pacing-health line. The
    // generated-frame share is the diagnostic: it is the half of the overlay's
    // GPU cost that lands inside the generator's own production path.
    void NotifyOverlayCallbackDraw(bool generatedFrame, bool drewOverlay);

    // One-shot activation context for FG pacing: the display/presentation
    // cadence the runtime is about to pace against plus the current input
    // admission interval. Called from the enabled-ffxConfigure and takeover
    // sites, never per frame.
    void LogActivationCadenceContext(const char* site);

    // Tags exact FSR-active portions of pacing-health windows. The first
    // interval after either edge is excluded because it straddles two states.
    // Brief game-driven off/on configures form separate tagged segments without
    // preventing the periodic aggregate from ever filling.
    void NotifyFSRFrameGenerationTransition(bool enabled, const char* site);

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
        // Presentation and callback draws can run on different threads.
        // Readers never lock the presenter; each history value is atomic.
        alignas(64) std::atomic<float> history[HISTORY_SIZE];
        std::atomic<int> historyIdx{0};
        std::atomic<uint64_t> sampleCount{0};
        std::atomic<int64_t> lastFrameTimeUs{0};

        int64_t frameTimeWindow[VARIANCE_WINDOW];
        int windowIndex = 0;
        bool windowFilled = false;
        std::atomic<double> windowVariance{0.0};
        std::atomic<double> windowStdDev{0.0};
        // Mean absolute difference between neighbouring intervals. A spread-out
        // series and an alternating one can share a mean and a variance; only
        // this tells them apart. Zero is a legitimate value - a perfectly even
        // series has no jaggedness at all - so readiness is carried separately
        // rather than inferred from the number being non-zero.
        std::atomic<double> windowJaggedness{0.0};
        std::atomic<bool> windowStatisticsValid{false};

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

    // Producer-provenance diagnostics over the last 128 publications. This
    // summary never selects, smooths, or filters the displayed metric.
    struct ScreenTimeCadence {
        static constexpr uint32_t kWindowSamples = 128;
        static constexpr uint32_t kWindowWords = kWindowSamples / 64;
        static constexpr uint32_t kRequiredPercent = 90;

        void Observe(bool screenTimeResolved) {
            const uint64_t bit = uint64_t{1} << (next % 64u);
            uint64_t& word = window[next / 64u];
            // The slot only holds an older sample once the window is full;
            // before that it has never been written.
            if (filled == kWindowSamples && (word & bit) != 0)
                --resolved;
            if (screenTimeResolved) {
                word |= bit;
                ++resolved;
            } else {
                word &= ~bit;
            }
            next = (next + 1u) % kWindowSamples;
            if (filled < kWindowSamples)
                ++filled;
        }

        void Reset() {
            for (uint64_t& word : window)
                word = 0;
            next = 0;
            filled = 0;
            resolved = 0;
        }

        bool IsScreenTime() const {
            return filled != 0 && resolved * 100 >= filled * kRequiredPercent;
        }

        uint32_t permille() const { return filled != 0 ? (resolved * 1000) / filled : 0; }

        uint64_t window[kWindowWords] = {};
        uint32_t next = 0;
        uint32_t filled = 0;
        uint32_t resolved = 0;
    };

    const MetricSeries& ActiveSeries() const;
    MetricSeries& ActiveSeries();
    void UpdateSeries(MetricSeries& series, int64_t currentQpcUs);
    void ApplyRecordingTransition(MetricSeries& series);
    void RefreshEffectiveSource(const SharedDisplayTiming& timing, int64_t currentQpcUs);
    // Rate-limited cadence-health aggregation line; early-outs to one timestamp
    // comparison on the per-present path.
    void MaybeLogPacingHealth(int64_t currentQpcUs);

    MetricSeries m_presentation;
    MetricSeries m_display;
    std::atomic<bool> m_recordingRequested{false};
    std::atomic<FrameTimeSource> m_preferredSource{FrameTimeSource::Presentation};
    std::atomic<FrameTimeSource> m_effectiveSource{FrameTimeSource::Presentation};
    std::mutex m_displayConsumeMutex;
    std::mutex m_presentationUpdateMutex;
    uint64_t m_displayGeneration = UINT64_MAX;
    uint64_t m_nextDisplaySequence = 1;
    ScreenTimeCadence m_displayScreenTimeCadence;
    std::atomic<bool> m_displayStreamIsScreenTime{true};
    std::atomic<uint32_t> m_displayScreenTimePermille{0};

    std::atomic<float> m_fgOutputFPS{0.0f};
    std::atomic<float> m_fgBaseFPS{0.0f};
    std::atomic<int> m_fgMultiplier{1};
    std::atomic<int> m_fgType{0};
    std::atomic<uint64_t> m_callbackAppDraws{0};
    std::atomic<uint64_t> m_callbackGeneratedDraws{0};
    std::atomic<uint64_t> m_callbackGeneratedSkips{0};
    std::atomic<uint64_t> m_fsrPacingTag{0};
    std::atomic<uint64_t> m_nextFsrPacingTag{0};
    std::atomic<bool> m_fsrPresentationNeedsAnchor{true};
    std::atomic<bool> m_fsrDisplayNeedsAnchor{true};
    std::atomic<int64_t> m_nextPacingHealthLogUs{0};
    std::mutex m_pacingHealthMutex;
    int64_t m_pacingHealthWindowStartUs = 0;
    ce::system_latency::Tracker m_systemLatency;
};
