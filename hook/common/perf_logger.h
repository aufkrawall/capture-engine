#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>

struct FrameMetrics {
    uint64_t frameNum = 0;
    int64_t qpcUs = 0;
    int32_t totalUs = 0;    // Total time in Present hook
    int32_t overlayUs = 0;  // CPU time for overlay work (cmd record + submit)
    int32_t captureUs = 0;
    int32_t deviceInitUs = 0;
    int32_t prerenderWaitUs = 0;
    int32_t fpsLimitWaitUs = 0;
    int32_t fenceWaitUs = 0;  // Time waiting for fence (previous frame sync)
    // Detailed DX12 breakdown (optional, set to 0 if not applicable)
    int32_t cmdListResetUs = 0;
    int32_t renderUs = 0;
    int32_t executeUs = 0;
    // DX9 staging capture breakdown (optional, set to 0 if not applicable)
    int32_t stretchRectUs = 0;     // GPU blit: backbuffer → intermediate
    int32_t readbackSubmitUs = 0;  // GetRenderTargetData submit time
    int32_t queryWaitUs = 0;       // Time polling/waiting for query completion
    int32_t lockRectUs = 0;        // LockRect on system memory surface
    int32_t d3d11UploadUs = 0;     // UpdateSubresource CPU→GPU upload
    int32_t stagingDepth = 0;      // Current staging pipeline occupancy
    int32_t stagingDropped = 0;    // Frames dropped due to full pipeline
    int32_t presentCallUs = 0;     // Actual D3D9/DXGI Present call time
    uint32_t sourceFrameIndex = 0;
    uint32_t sourceCapturePhase = 0;
    uint32_t sourceEncoderQueueDepth = 0;
    uint32_t sourceMuxQueueKb = 0;
    uint32_t sourceOverloadFlags = 0;
    int32_t source1PctLowTimes100 = 0;
    int32_t sourcePoint1PctLowTimes100 = 0;
    int32_t sourceFrameTimeStdDevUs = 0;
    int32_t sourceCurrentFpsTimes100 = 0;
    char api[8] = "";
};

enum PresentDebugSampleFlags : uint32_t {
    kPresentSampleFlagOverlayCacheHit = 1u << 0,
    kPresentSampleFlagOverlayRebuilt = 1u << 1,
    kPresentSampleFlagInterpolatedFrame = 1u << 2,
    kPresentSampleFlagMutexBusy = 1u << 3,
    kPresentSampleFlagAllocatorBusy = 1u << 4,
};

struct PresentDebugSample {
    uint64_t frameNum = 0;
    int32_t wrapperTotalUs = 0;
    int32_t swapchainAcquireUs = 0;
    int32_t metricsUpdateUs = 0;
    int32_t processFrameExternalUs = 0;
    int32_t processFrameUs = 0;
    int32_t overlayBuildUs = 0;
    int32_t overlayRenderUs = 0;
    int32_t captureUs = 0;
    int32_t fpsLimiterUs = 0;
    int32_t presentCallUs = 0;
    int32_t csvWriteUs = 0;
    uint32_t flags = 0;
    uint32_t sourceFrameIndex = 0;
    uint32_t capturePhase = 0;
    uint32_t encoderQueueDepth = 0;
    uint32_t muxQueueKb = 0;
    uint32_t overloadFlags = 0;
    char api[8] = "";
};

inline bool ShouldFlushPerfMetricsCsvAfterFrame(uint64_t frameNum) {
    // Keep early crash / hang sessions durable without forcing a flush on every
    // frame. Early frames are flushed aggressively; later runs flush periodically.
    return frameNum <= 8 || (frameNum % 32) == 0;
}

class PerfLogger {
public:
    static PerfLogger& Get();

    void Init(const char* logPath);
    void Shutdown();

    void LogFrame(const FrameMetrics& metrics);
    bool ShouldSampleDetailedFrame(uint64_t frameNum) const;
    void ActivateDebugSample(PresentDebugSample* sample);
    void DeactivateDebugSample(PresentDebugSample* sample);
    PresentDebugSample* GetActiveDebugSample();
    void CommitDebugSample(const PresentDebugSample& sample);

    bool IsEnabled() const {
        return file_ != nullptr;
    }
    uint64_t GetFrameCount() const {
        return frameCount_.load(std::memory_order_relaxed);
    }

    // Present-row dedup (session 20260702_094955/140811: perf CSV recorded TWO rows per present on paths
    // where an inner per-API ProcessFrame row nests inside the outer DetourPresent/wrapper catch-all row —
    // ~50% zero-delta qpc pairs that skew any present-rate analysis). The OUTER present hook calls
    // BeginPresentRowScope() before dispatching; every LogFrame marks the scope; the outer catch-all row is
    // then written only when NO inner row was logged for this present. Thread-local — presents dispatch
    // their inner work synchronously on the same thread.
    static void BeginPresentRowScope();
    static bool InnerRowLoggedInPresentRowScope();

    static int64_t GetQpcUs();
    static int64_t GetQpcFrequency();

private:
    struct DebugSummaryState {
        uint64_t sampleCount = 0;
        int64_t wrapperTotalUsSum = 0;
        int64_t swapchainAcquireUsSum = 0;
        int64_t metricsUpdateUsSum = 0;
        int64_t processFrameExternalUsSum = 0;
        int64_t processFrameUsSum = 0;
        int64_t overlayBuildUsSum = 0;
        int64_t overlayRenderUsSum = 0;
        int64_t captureUsSum = 0;
        int64_t fpsLimiterUsSum = 0;
        int64_t presentCallUsSum = 0;
        int64_t csvWriteUsSum = 0;
        int32_t wrapperTotalUsMax = 0;
        int32_t presentCallUsMax = 0;
        int32_t csvWriteUsMax = 0;
        uint64_t livePhaseSamples = 0;
        uint64_t warmupPhaseSamples = 0;
        uint64_t drainPhaseSamples = 0;
        uint64_t maxEncoderQueueDepth = 0;
        uint64_t maxMuxQueueKb = 0;
        uint64_t overloadSampleCount = 0;
        uint64_t overlayCacheHitCount = 0;
        uint64_t overlayRebuildCount = 0;
        uint64_t interpolatedFrameCount = 0;
        uint64_t mutexBusyCount = 0;
        uint64_t allocatorBusyCount = 0;
    };

    PerfLogger() = default;
    ~PerfLogger() {
        Shutdown();
    }

    PerfLogger(const PerfLogger&) = delete;
    PerfLogger& operator=(const PerfLogger&) = delete;

    FILE* file_ = nullptr;
    std::mutex fileMutex_;
    std::atomic<uint64_t> frameCount_{0};
    int64_t qpcFreq_ = 0;
    bool headerWritten_ = false;
    int64_t lastLoggedQpcUs_ = 0;
    std::mutex debugSummaryMutex_;
    DebugSummaryState debugSummary_;

    void ResetDebugSummaryLocked();
};

class ScopedPerfTimer {
public:
    explicit ScopedPerfTimer(int32_t* resultUs);
    ~ScopedPerfTimer();

    ScopedPerfTimer(const ScopedPerfTimer&) = delete;
    ScopedPerfTimer& operator=(const ScopedPerfTimer&) = delete;

private:
    int32_t* resultUs_;
    int64_t startQpc_;
};

#define PERF_TIMER(var) ScopedPerfTimer _perfTimer_##var(&(var))
