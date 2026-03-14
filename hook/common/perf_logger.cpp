#include "perf_logger.h"
#include <windows.h>
#include <cstring>
#include <filesystem>
#include "hook_common.h"

namespace {
constexpr uint64_t kDetailedSampleInterval = 64;
constexpr uint64_t kDetailedSummarySampleCount = 32;
constexpr size_t kPerfLogBufferSize = 1024 * 1024;

thread_local PresentDebugSample* g_ActivePresentDebugSample = nullptr;

int64_t GetCachedQpcFrequency() {
    static const int64_t kQpcFrequency = []() {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        return freq.QuadPart;
    }();
    return kQpcFrequency;
}

double UsToMs(int64_t valueUs) {
    return static_cast<double>(valueUs) / 1000.0;
}

double AvgUsToMs(int64_t sumUs, double sampleCount) {
    return (sampleCount > 0.0) ? (static_cast<double>(sumUs) / sampleCount) / 1000.0 : 0.0;
}

void AccumulateSample(int32_t valueUs, int64_t& sumUs, int32_t& maxUs) {
    sumUs += valueUs;
    if (valueUs > maxUs) {
        maxUs = valueUs;
    }
}
}  // namespace

PerfLogger& PerfLogger::Get() {
    static PerfLogger instance;
    return instance;
}

void PerfLogger::Init(const char* logPath) {
    std::lock_guard<std::mutex> lock(fileMutex_);
    if (file_ || !HookDebugLoggingEnabled())
        return;

    qpcFreq_ = GetCachedQpcFrequency();

    std::filesystem::path path(logPath);
    std::filesystem::path dir = path.parent_path();
    if (!dir.empty() && !std::filesystem::exists(dir)) {
        std::filesystem::create_directories(dir);
    }

    file_ = fopen(logPath, "w");
    if (file_) {
        setvbuf(file_, nullptr, _IOFBF, kPerfLogBufferSize);
        fprintf(file_,
                "frame,qpc_us,total_us,overlay_us,capture_us,device_init_us,"
                "prerender_wait_us,fps_limit_wait_us,fence_wait_us,"
                "cmdlist_reset_us,render_us,execute_us,"
                "stretch_rect_us,readback_submit_us,query_wait_us,"
                "lock_rect_us,d3d11_upload_us,staging_depth,staging_dropped,"
                "present_call_us,api\n");
        fflush(file_);
        headerWritten_ = true;
        HookLog("PerfLogger: Initialized CSV logging to %s", logPath);
    } else {
        HookLog("PerfLogger: Failed to open %s for writing", logPath);
    }
}

void PerfLogger::Shutdown() {
    std::lock_guard<std::mutex> lock(fileMutex_);
    if (file_) {
        fflush(file_);
        fclose(file_);
        file_ = nullptr;
        HookLog("PerfLogger: Shutdown, logged %llu frames", (unsigned long long)frameCount_.load());
    }
}

void PerfLogger::LogFrame(const FrameMetrics& metrics) {
    std::lock_guard<std::mutex> lock(fileMutex_);
    if (!file_)
        return;

    uint64_t frameNum = frameCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    const int64_t writeStartUs = g_ActivePresentDebugSample ? GetQpcUs() : 0;

    fprintf(file_, "%llu,%lld,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s\n", (unsigned long long)frameNum,
            (long long)metrics.qpcUs, metrics.totalUs, metrics.overlayUs, metrics.captureUs, metrics.deviceInitUs,
            metrics.prerenderWaitUs, metrics.fpsLimitWaitUs, metrics.fenceWaitUs, metrics.cmdListResetUs,
            metrics.renderUs, metrics.executeUs, metrics.stretchRectUs, metrics.readbackSubmitUs, metrics.queryWaitUs,
            metrics.lockRectUs, metrics.d3d11UploadUs, metrics.stagingDepth, metrics.stagingDropped,
            metrics.presentCallUs, metrics.api);

    if (g_ActivePresentDebugSample) {
        g_ActivePresentDebugSample->csvWriteUs = static_cast<int32_t>(GetQpcUs() - writeStartUs);
    }
}

int64_t PerfLogger::GetQpcUs() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart * 1000000) / GetCachedQpcFrequency();
}

int64_t PerfLogger::GetQpcFrequency() {
    return GetCachedQpcFrequency();
}

bool PerfLogger::ShouldSampleDetailedFrame(uint64_t frameNum) const {
    return file_ != nullptr && (frameNum % kDetailedSampleInterval) == 0;
}

void PerfLogger::ActivateDebugSample(PresentDebugSample* sample) {
    g_ActivePresentDebugSample = sample;
}

void PerfLogger::DeactivateDebugSample(PresentDebugSample* sample) {
    if (g_ActivePresentDebugSample == sample) {
        g_ActivePresentDebugSample = nullptr;
    }
}

PresentDebugSample* PerfLogger::GetActiveDebugSample() {
    return g_ActivePresentDebugSample;
}

void PerfLogger::CommitDebugSample(const PresentDebugSample& sample) {
    if (!file_) {
        return;
    }

    std::lock_guard<std::mutex> lock(debugSummaryMutex_);
    DebugSummaryState& summary = debugSummary_;
    summary.sampleCount++;
    AccumulateSample(sample.wrapperTotalUs, summary.wrapperTotalUsSum, summary.wrapperTotalUsMax);
    summary.swapchainAcquireUsSum += sample.swapchainAcquireUs;
    summary.metricsUpdateUsSum += sample.metricsUpdateUs;
    summary.processFrameExternalUsSum += sample.processFrameExternalUs;
    summary.processFrameUsSum += sample.processFrameUs;
    summary.overlayBuildUsSum += sample.overlayBuildUs;
    summary.overlayRenderUsSum += sample.overlayRenderUs;
    summary.captureUsSum += sample.captureUs;
    summary.fpsLimiterUsSum += sample.fpsLimiterUs;
    AccumulateSample(sample.presentCallUs, summary.presentCallUsSum, summary.presentCallUsMax);
    AccumulateSample(sample.csvWriteUs, summary.csvWriteUsSum, summary.csvWriteUsMax);

    if ((sample.flags & kPresentSampleFlagOverlayCacheHit) != 0) {
        summary.overlayCacheHitCount++;
    }
    if ((sample.flags & kPresentSampleFlagOverlayRebuilt) != 0) {
        summary.overlayRebuildCount++;
    }
    if ((sample.flags & kPresentSampleFlagInterpolatedFrame) != 0) {
        summary.interpolatedFrameCount++;
    }
    if ((sample.flags & kPresentSampleFlagMutexBusy) != 0) {
        summary.mutexBusyCount++;
    }
    if ((sample.flags & kPresentSampleFlagAllocatorBusy) != 0) {
        summary.allocatorBusyCount++;
    }

    if (summary.sampleCount < kDetailedSummarySampleCount) {
        return;
    }

    const double sampleCount = static_cast<double>(summary.sampleCount);
    HookLog(
        "[PERF] %s Present summary (%llu samples, every %llu frames): wrapper_total avg=%.3f ms max=%.3f ms, "
        "swapchain_acquire avg=%.3f ms, metrics_update avg=%.3f ms, process_frame_external avg=%.3f ms, "
        "process_frame avg=%.3f ms, overlay_build avg=%.3f ms, overlay_render avg=%.3f ms, capture avg=%.3f ms, "
        "fps_limiter avg=%.3f ms, present_call avg=%.3f ms max=%.3f ms, csv_write avg=%.3f ms max=%.3f ms, "
        "cache_hits=%llu, rebuilds=%llu, interpolated=%llu, mutex_busy=%llu, allocator_busy=%llu",
        sample.api[0] ? sample.api : "DXGI", (unsigned long long)summary.sampleCount,
        (unsigned long long)kDetailedSampleInterval, AvgUsToMs(summary.wrapperTotalUsSum, sampleCount),
        UsToMs(summary.wrapperTotalUsMax), AvgUsToMs(summary.swapchainAcquireUsSum, sampleCount),
        AvgUsToMs(summary.metricsUpdateUsSum, sampleCount), AvgUsToMs(summary.processFrameExternalUsSum, sampleCount),
        AvgUsToMs(summary.processFrameUsSum, sampleCount), AvgUsToMs(summary.overlayBuildUsSum, sampleCount),
        AvgUsToMs(summary.overlayRenderUsSum, sampleCount), AvgUsToMs(summary.captureUsSum, sampleCount),
        AvgUsToMs(summary.fpsLimiterUsSum, sampleCount), AvgUsToMs(summary.presentCallUsSum, sampleCount),
        UsToMs(summary.presentCallUsMax), AvgUsToMs(summary.csvWriteUsSum, sampleCount), UsToMs(summary.csvWriteUsMax),
        (unsigned long long)summary.overlayCacheHitCount, (unsigned long long)summary.overlayRebuildCount,
        (unsigned long long)summary.interpolatedFrameCount, (unsigned long long)summary.mutexBusyCount,
        (unsigned long long)summary.allocatorBusyCount);
    ResetDebugSummaryLocked();
}

void PerfLogger::ResetDebugSummaryLocked() {
    debugSummary_ = {};
}

ScopedPerfTimer::ScopedPerfTimer(int32_t* resultUs) : resultUs_(resultUs), startQpc_(0) {
    if (resultUs_) {
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        startQpc_ = start.QuadPart;
    }
}

ScopedPerfTimer::~ScopedPerfTimer() {
    if (resultUs_ && startQpc_ > 0) {
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        int64_t elapsedUs = ((end.QuadPart - startQpc_) * 1000000) / freq.QuadPart;
        *resultUs_ = static_cast<int32_t>(elapsedUs);
    }
}
