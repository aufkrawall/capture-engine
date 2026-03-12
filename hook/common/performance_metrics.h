#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

class PerformanceMetrics {
public:
    static const int HISTORY_SIZE = 8192;       // covers 15s at >500 FPS
    static const int GRAPH_HISTORY_SIZE = 240;  // visual window
    static const int VARIANCE_WINDOW = 120;

    PerformanceMetrics();
    ~PerformanceMetrics();

    // Call this once per frame with the current time (e.g. QPC microseconds)
    void Update(int64_t currentQpcUs);

    // Get plotting data — lock-free reads from atomic history
    const float* GetHistoryArray() const {
        return m_history;
    }
    int GetHistoryIndex() const {
        return m_historyIdx.load(std::memory_order_acquire);
    }
    void GetLastHistory(float* outBuffer, int count) const;
    float GetCurrentFPS() const;

    // FPS Statistics (based on last 5 seconds = 300 frames at 60fps, 600 at
    // 120fps)
    float GetAverageFPS() const;
    float Get1PercentLowFPS() const;
    float Get01PercentLowFPS() const;

    // Variance / Stutter detection stats
    double GetWindowStdDev() const {
        return m_windowStdDev.load(std::memory_order_relaxed);
    }
    bool IsStutterDetected() const {
        return m_stutterDetected.load(std::memory_order_relaxed);
    }

    // Graph Scaling
    // Returns min/max for PlotLines. Anchors min at 0, ensures max is at least
    // minRangeMs.
    void GetSmartScale(float& outMin, float& outMax, float minRangeMs = 33.0f) const;

    // Get maximum frame time in the last N seconds (for latency indicator)
    float GetMaxFrameTime(float windowSeconds) const;

    // State management
    void SetRecording(bool isRecording);

    // Frame Generation metrics (for displaying base vs output FPS like RTSS)
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
    // Returns short label: "DLSS FG", "FSR FG", "NVIDIA SM", or "FG"
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
    // History ring buffer — written by Update(), read lock-free by overlay.
    // Individual floats are naturally atomic on x86/x64 (aligned 4-byte writes).
    // The index is atomic to ensure readers see a consistent snapshot boundary.
    float m_history[HISTORY_SIZE];
    std::atomic<int> m_historyIdx{0};

    std::atomic<int64_t> m_lastFrameTimeUs{0};
    int64_t m_frameCounter = 0;  // Only accessed under m_csvMutex or by writer

    // Variance calculation (Welford's) — writer-only, protected by single-writer
    int64_t m_frameTimeWindow[VARIANCE_WINDOW];
    int m_windowIndex = 0;
    bool m_windowFilled = false;

    std::atomic<double> m_windowVariance{0.0};
    std::atomic<double> m_windowStdDev{0.0};

    // Baseline vs Recording stats — writer-only
    bool m_isRecording = false;
    double m_baselineMean = 0;
    double m_baselineM2 = 0;
    int64_t m_baselineCount = 0;

    double m_recordingMean = 0;
    double m_recordingM2 = 0;
    int64_t m_recordingCount = 0;

    double m_lastBaselineVariance = 0;
    std::atomic<bool> m_stutterDetected{false};

    // Frame Generation metrics — atomic for lock-free reads
    std::atomic<float> m_fgOutputFPS{0.0f};
    std::atomic<float> m_fgBaseFPS{0.0f};
    std::atomic<int> m_fgMultiplier{1};
    std::atomic<int> m_fgType{0};
};
