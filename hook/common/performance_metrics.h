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
    // Whether the display stream is currently delivering screen times rather
    // than flip-latch timestamps the vertical-blank clock could not resolve.
    // Diagnostic only: it is the reason a display_change preference can still
    // report presentation timing, so a health line can say so.
    bool IsDisplayStreamScreenTime() const {
        return m_displayStreamIsScreenTime.load(std::memory_order_acquire);
    }
    // Share of recent display publications that carried a resolved screen time,
    // in per mille. Zero until the stream has produced any sample.
    uint32_t GetDisplayScreenTimePermille() const {
        return m_displayScreenTimePermille.load(std::memory_order_relaxed);
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
    // Falls back to the presentation frame time when there is no display series
    // yet, and equally when the display stream is not publishing screen times:
    // the benchmark recorder writes this as its display column, and a flip-latch
    // interval is not one. The honest statement in that regime is that CE has no
    // separate screen-time series, which is what the two columns then say.
    float GetLastDisplayFrameTimeMs() const {
        if (m_display.sampleCount.load(std::memory_order_relaxed) == 0 ||
            !m_displayStreamIsScreenTime.load(std::memory_order_acquire)) {
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

    // How much of the display stream is actually a screen-time series.
    //
    // A published sample count that matches the display rate says nothing about
    // the timestamp values. Under variable refresh below the panel's cap the
    // vertical-blank clock has no grid to place frames on, and every deferred
    // completion then reaches the overlay carrying the moment the driver
    // latched the flip instead of the moment the screen changed. Measured in
    // Talos under FSR frame generation, two runs of the same build and settings
    // published a correct mean (85.1 against 84.9 fps at Present, 90.7 against
    // 91.1) while reporting four times and twice the frame-time standard
    // deviation the same frames had at Present - which is what "the variance is
    // sometimes bad and sometimes not" looks like from the outside.
    //
    // Judged over the most recent samples, not over a decaying total of
    // everything the stream has ever published. Which of the two it is matters,
    // because the thing being judged changes regime: a frame generator handing
    // over swaps deferred vsync/hsync completions for immediate flips, and the
    // evidence from before the handover says nothing about after it. A decaying
    // count carries the old regime in and needs about eight hundred samples to
    // shed it - measured in Talos, an FSR-FG stretch held the metric on
    // presentation timing for 7.2 s after DLSS FG had already started
    // delivering resolved screen times, which is long enough to watch the graph
    // change shape. A window of the last kWindowSamples follows a regime change
    // in kWindowSamples samples and no longer, about a second of frames.
    struct ScreenTimeCadence {
        // About one second of frames at ordinary output rates, which is also the
        // order of a frame-generation handover's own settling time; short enough
        // to follow a regime change, long enough that sampling noise near the
        // selection threshold cannot move it.
        static constexpr uint32_t kWindowSamples = 128;
        static constexpr uint32_t kWindowWords = kWindowSamples / 64;
        // Enough samples for the ratio to mean anything before it may overrule
        // the initial assumption.
        static constexpr uint32_t kMinimumSamples = 32;
        // Selecting the display stream needs it to be almost entirely screen
        // times; keeping it needs only a majority. The two thresholds are what
        // stop a stream hovering at one of them from switching the metric back
        // and forth every window.
        static constexpr uint32_t kSelectPercent = 90;
        static constexpr uint32_t kKeepPercent = 50;

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

        // Assumed to be screen times until there is enough evidence to say
        // otherwise, so a stream that is fine never spends its first frames
        // withheld from the overlay.
        bool IsScreenTime(bool currentlySelected) const {
            if (filled < kMinimumSamples)
                return true;
            const uint32_t required = currentlySelected ? kKeepPercent : kSelectPercent;
            return resolved * 100 >= filled * required;
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

    MetricSeries m_presentation;
    MetricSeries m_display;
    std::atomic<bool> m_recordingRequested{false};
    std::atomic<FrameTimeSource> m_preferredSource{FrameTimeSource::Presentation};
    std::atomic<FrameTimeSource> m_effectiveSource{FrameTimeSource::Presentation};
    std::mutex m_displayConsumeMutex;
    uint64_t m_displayGeneration = UINT64_MAX;
    uint64_t m_nextDisplaySequence = 1;
    ScreenTimeCadence m_displayScreenTimeCadence;
    std::atomic<bool> m_displayStreamIsScreenTime{true};
    std::atomic<uint32_t> m_displayScreenTimePermille{0};

    std::atomic<float> m_fgOutputFPS{0.0f};
    std::atomic<float> m_fgBaseFPS{0.0f};
    std::atomic<int> m_fgMultiplier{1};
    std::atomic<int> m_fgType{0};
    ce::system_latency::Tracker m_systemLatency;
};
