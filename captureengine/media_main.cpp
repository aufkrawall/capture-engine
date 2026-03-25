// clang-format off
#include <windows.h>
#include <avrt.h>
#include <d3d11.h>
#include <psapi.h>
#include <timeapi.h>
// clang-format on
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../common/capture_pipeline_policy.h"
#include "../common/config.h"
#include "../common/frame_queue.h"
#include "../common/frame_timing_utils.h"
#include "../common/logging.h"
#include "../common/process_ipc.h"
#include "../common/rate_window_utils.h"
#include "../common/shared_defs.h"
#include "mediaengine_loader.h"
#include "wgc_capture.h"

#ifdef _MSC_VER
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "winmm.lib")
#endif

static std::atomic<bool> g_Running{true};
static std::atomic<bool> g_Recording{false};
static std::atomic<bool> g_EncoderRunning{false};
static std::atomic<bool> g_IsEncoderBottlenecked{false};
static std::atomic<bool> g_RecordingUsesVfr{false};
static std::atomic<bool> g_DrainOutstandingWgcTicks{false};
static std::atomic<int64_t> g_WgcDrainStopQpc{0};

BOOL WINAPI MediaConsoleHandler(DWORD ctrlType) {
    // Handle all console events including Windows shutdown/logoff
    LogInfo("[Media] Console event %lu received, shutting down...", ctrlType);
    g_Running = false;
    return TRUE;
}

static FrameQueue g_FrameQueue(32);
static std::thread g_EncoderThread;
static QueuedFrame g_LastFrame;
static bool g_HasLastFrame = false;
static std::atomic<uint64_t> g_InjectDeferredFrames{0};

// Screengrab mode components
static std::unique_ptr<WGCCapture> g_WgcCap;
static std::atomic<bool> g_UseScreenGrab{false};     // Active capture mode for the current recording
static std::atomic<bool> g_PreferScreenGrab{false};  // Preferred mode for the next recording

// Shared memory for hook communication
static HANDLE g_hMapFile = NULL;
static SharedMemoryLayout* g_pSharedMem = nullptr;

static HANDLE g_hMapShmem = NULL;
static ShmemBuffer* g_pShmem = nullptr;

// Inject thread specific
static std::atomic<bool> g_InjectCaptureRunning{false};
static std::atomic<bool> g_InjectCaptureShutdown{false};
static std::atomic<bool> g_InjectSessionReset{true};  // Set true on StartRecording to reset inject session state
static std::thread g_InjectCaptureThread;

// WGC thread specific
static std::atomic<bool> g_WgcCaptureRunning{false};
static std::atomic<bool> g_WgcCaptureShutdown{false};
static std::thread g_WgcCaptureThread;
static std::atomic<bool> g_InjectDeliveredFirstFrame{false};
static std::atomic<bool> g_RejectInjectFrames{false};
static std::atomic<bool> g_AutoWgcFallbackArmed{false};
static std::atomic<uint32_t> g_InjectBufferedTrimmedFrames{0};
static std::atomic<uint32_t> g_InjectCadenceDroppedFrames{0};
static std::atomic<uint32_t> g_WgcAdaptiveTargetFps{0};

// Forward declaration
void InjectCaptureThreadFunc(const AppConfig& config);
void WgcCaptureThreadFunc(const AppConfig& config);
void StopRecording();
void StartRecording(const AppConfig& config);

namespace {
constexpr int kInjectTextureSlotCount = 8;

// Encoder-bottleneck EMA parameters.  The smoothing factor (alpha) controls
// how quickly the EMA reacts to encode-time changes.  Hysteresis avoids
// bang-bang oscillation: we enter bottleneck at a higher threshold and exit
// at a significantly lower one.
constexpr double kEncodeEmaAlpha = 0.10;
constexpr double kBottleneckEnterRatio = 0.95;  // smoothedEncodeMs > 95% of frame interval → enter
constexpr double kBottleneckExitRatio = 0.75;   // smoothedEncodeMs < 75% of frame interval → exit

// Update g_IsEncoderBottlenecked with hysteresis to prevent rapid toggling.
inline void UpdateEncoderBottleneckFlag(double smoothedEncodeMs, double frameIntervalMs) {
    const bool currentlyBottlenecked = g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
    if (currentlyBottlenecked) {
        // Exit bottleneck only when encode time drops well below the frame budget
        if (smoothedEncodeMs < frameIntervalMs * kBottleneckExitRatio) {
            g_IsEncoderBottlenecked.store(false, std::memory_order_relaxed);
        }
    } else {
        // Enter bottleneck when encode time approaches the frame budget
        if (smoothedEncodeMs > frameIntervalMs * kBottleneckEnterRatio) {
            g_IsEncoderBottlenecked.store(true, std::memory_order_relaxed);
        }
    }
}

bool MediaAudioConfigEquals(const AudioConfig& lhs, const AudioConfig& rhs) {
    return lhs.enabled == rhs.enabled && lhs.device == rhs.device && lhs.processName == rhs.processName &&
           lhs.processId == rhs.processId && lhs.sourceType == rhs.sourceType && lhs.tracks == rhs.tracks &&
           lhs.codec == rhs.codec && lhs.bitrate == rhs.bitrate && lhs.sampleRate == rhs.sampleRate &&
           lhs.bitDepth == rhs.bitDepth && lhs.downmix == rhs.downmix;
}

bool MediaScalingConfigEquals(const ScalingConfig& lhs, const ScalingConfig& rhs) {
    return lhs.enabled == rhs.enabled && lhs.outputResolution == rhs.outputResolution && lhs.quality == rhs.quality &&
           lhs.sharpness == rhs.sharpness && lhs.outputWidth == rhs.outputWidth && lhs.outputHeight == rhs.outputHeight;
}

bool MediaVideoConfigEquals(const VideoConfig& lhs, const VideoConfig& rhs) {
    return lhs.encoder == rhs.encoder && lhs.fps == rhs.fps && lhs.container == rhs.container &&
           lhs.outputDir == rhs.outputDir && lhs.rateControl == rhs.rateControl && lhs.bitrate == rhs.bitrate &&
           lhs.maxBitrate == rhs.maxBitrate && lhs.keyframeInterval == rhs.keyframeInterval &&
           lhs.preset == rhs.preset && lhs.tuning == rhs.tuning && lhs.multipass == rhs.multipass &&
           lhs.profile == rhs.profile && lhs.lookahead == rhs.lookahead && lhs.aq == rhs.aq &&
           lhs.bFrames == rhs.bFrames && lhs.bRefMode == rhs.bRefMode && lhs.customOptions == rhs.customOptions &&
           lhs.captureCursor == rhs.captureCursor && lhs.qp == rhs.qp && lhs.mfRateControl == rhs.mfRateControl &&
           lhs.mfQuality == rhs.mfQuality && lhs.mfScenario == rhs.mfScenario && lhs.mfHwEncoding == rhs.mfHwEncoding &&
           lhs.gpuPriority == rhs.gpuPriority && lhs.bitDepth == rhs.bitDepth && lhs.colorSpace == rhs.colorSpace &&
           lhs.colorRange == rhs.colorRange && lhs.chromaSubsampling == rhs.chromaSubsampling &&
           lhs.useVFR == rhs.useVFR && lhs.useVFR_AudioSync == rhs.useVFR_AudioSync &&
           MediaScalingConfigEquals(lhs.scaling, rhs.scaling);
}

bool MediaEngineConfigEquals(const AppConfig& lhs, const AppConfig& rhs) {
    if (lhs.debugLogging != rhs.debugLogging || lhs.captureMethod != rhs.captureMethod ||
        !MediaVideoConfigEquals(lhs.video, rhs.video) || lhs.audioSources.size() != rhs.audioSources.size()) {
        return false;
    }

    for (size_t i = 0; i < lhs.audioSources.size(); ++i) {
        if (!MediaAudioConfigEquals(lhs.audioSources[i], rhs.audioSources[i])) {
            return false;
        }
    }

    return true;
}

uint32_t SaturatingToUint32(uint64_t value) {
    return value > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(value);
}

template <typename AtomicT>
void UpdateAtomicPeak(AtomicT& peak, uint32_t value) {
    uint32_t current = peak.load(std::memory_order_relaxed);
    while (value > current &&
           !peak.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

void SetCapturePipelinePhase(CapturePipelinePhase phase) {
    if (!g_pSharedMem) {
        return;
    }
    g_pSharedMem->runtimeState.capturePhase.store(static_cast<uint32_t>(phase), std::memory_order_relaxed);
}

void ResetRuntimeDiagnostics(SharedMemoryLayout* sharedMem) {
    if (!sharedMem) {
        return;
    }

    auto& state = sharedMem->runtimeState;
    state.currentFPS.store(0.0, std::memory_order_relaxed);
    state.gameFPS.store(0.0, std::memory_order_relaxed);
    state.hostDroppedFrames.store(0, std::memory_order_relaxed);
    state.duplicateFrames.store(0, std::memory_order_relaxed);
    state.lateFrames.store(0, std::memory_order_relaxed);
    state.encoderOverloadFlags.store(0, std::memory_order_relaxed);
    state.muxQueueBytes.store(0, std::memory_order_relaxed);
    state.muxQueuePackets.store(0, std::memory_order_relaxed);
    state.muxQueuePeakBytes.store(0, std::memory_order_relaxed);
    state.muxQueuePeakPackets.store(0, std::memory_order_relaxed);
    state.muxBackpressureCount.store(0, std::memory_order_relaxed);
    state.muxBackpressureWaitUs.store(0, std::memory_order_relaxed);
    state.muxBackpressureMaxWaitUs.store(0, std::memory_order_relaxed);
    state.capturePhase.store(static_cast<uint32_t>(CapturePipelinePhase::kIdle), std::memory_order_relaxed);
    state.sourceFramesReceived.store(0, std::memory_order_relaxed);
    state.framesQueued.store(0, std::memory_order_relaxed);
    state.framesEncoded.store(0, std::memory_order_relaxed);
    state.liveFramesEncoded.store(0, std::memory_order_relaxed);
    state.drainFramesEncoded.store(0, std::memory_order_relaxed);
    state.invalidFrameMetadata.store(0, std::memory_order_relaxed);
    state.invalidSharedHandles.store(0, std::memory_order_relaxed);
    state.injectPacingDrops.store(0, std::memory_order_relaxed);
    state.injectCadenceDrops.store(0, std::memory_order_relaxed);
    state.injectTrimmedFrames.store(0, std::memory_order_relaxed);
    state.deferredFrames.store(0, std::memory_order_relaxed);
    state.repeatedDeferredFrames.store(0, std::memory_order_relaxed);
    state.consecutiveDeferredFrames.store(0, std::memory_order_relaxed);
    state.maxConsecutiveDeferredFrames.store(0, std::memory_order_relaxed);
    state.duplicateFramesNoSource.store(0, std::memory_order_relaxed);
    state.duplicateFramesDeferred.store(0, std::memory_order_relaxed);
    state.duplicateFramesTimerRebase.store(0, std::memory_order_relaxed);
    state.duplicateFramesDrain.store(0, std::memory_order_relaxed);
    state.consecutiveDuplicateFrames.store(0, std::memory_order_relaxed);
    state.maxConsecutiveDuplicateFrames.store(0, std::memory_order_relaxed);
    state.frameIndexRegressions.store(0, std::memory_order_relaxed);
    state.textureReuseAnomalies.store(0, std::memory_order_relaxed);
    state.sourceTimestampRegressions.store(0, std::memory_order_relaxed);
    state.sourceTimestampStalls.store(0, std::memory_order_relaxed);
    state.timerRebases.store(0, std::memory_order_relaxed);
    state.bufferedInjectDepthPeak.store(0, std::memory_order_relaxed);
    state.encoderQueuePeakDepth.store(0, std::memory_order_relaxed);
    state.packetDurationClamps.store(0, std::memory_order_relaxed);
    state.negativePtsCount.store(0, std::memory_order_relaxed);
    state.nonMonotonicPtsCount.store(0, std::memory_order_relaxed);
    state.frameAgeAvgUs.store(0, std::memory_order_relaxed);
    state.frameAgeMaxUs.store(0, std::memory_order_relaxed);
    state.selectionErrorAvgUs.store(0, std::memory_order_relaxed);
    state.selectionErrorMaxUs.store(0, std::memory_order_relaxed);
    state.selectionErrorSignedAvgUs.store(0, std::memory_order_relaxed);
    state.selectionEarlyMaxUs.store(0, std::memory_order_relaxed);
    state.selectionLateMaxUs.store(0, std::memory_order_relaxed);
    state.oldestBufferedFrameAgeUs.store(0, std::memory_order_relaxed);
    state.wgcSourceFrameIntervalAvgUs.store(0, std::memory_order_relaxed);
    state.wgcSourceFrameJitterAvgUs.store(0, std::memory_order_relaxed);
    state.wgcSourceFrameJitterMaxUs.store(0, std::memory_order_relaxed);
    state.wgcSourceToCopyLatencyAvgUs.store(0, std::memory_order_relaxed);
    state.wgcSourceToCopyLatencyMaxUs.store(0, std::memory_order_relaxed);
    state.wgcTargetFps.store(0, std::memory_order_relaxed);
    state.wgcDeliveredFramesPerSec.store(0, std::memory_order_relaxed);
    state.wgcDeliveredMin250Fps.store(0, std::memory_order_relaxed);
    state.wgcDeliveredMin500Fps.store(0, std::memory_order_relaxed);
    state.wgcInputMin250Fps.store(0, std::memory_order_relaxed);
    state.wgcInputMin500Fps.store(0, std::memory_order_relaxed);
    state.wgcQueueEmptyTickPermille.store(0, std::memory_order_relaxed);
    state.wgcBufferedAtTickAvgPermille.store(0, std::memory_order_relaxed);
    state.wgcBufferedAtTickMin.store(0, std::memory_order_relaxed);
    state.wgcStarvedTickCount.store(0, std::memory_order_relaxed);
    state.wgcSingleFrameTickCount.store(0, std::memory_order_relaxed);
}

bool IsActiveScreenGrab() {
    return g_UseScreenGrab.load(std::memory_order_acquire);
}

void SetActiveScreenGrab(bool enabled) {
    g_UseScreenGrab.store(enabled, std::memory_order_release);
}

bool IsPreferredScreenGrab() {
    return g_PreferScreenGrab.load(std::memory_order_acquire);
}

void SetPreferredScreenGrab(bool enabled) {
    g_PreferScreenGrab.store(enabled, std::memory_order_release);
}

void SetCaptureRequestedState(bool enabled) {
    if (!g_pSharedMem) {
        return;
    }

    g_pSharedMem->runtimeState.captureRequested.store(enabled, std::memory_order_release);
}

void SetRecordingVisibleState(bool enabled) {
    if (!g_pSharedMem) {
        return;
    }

    if (enabled) {
        const bool wasVisible = g_pSharedMem->runtimeState.isRecording.exchange(true, std::memory_order_acq_rel);
        if (!wasVisible) {
            g_pSharedMem->runtimeState.recordingStartTime.store(GetTickCount64(), std::memory_order_release);
        }
    } else {
        g_pSharedMem->runtimeState.isRecording.store(false, std::memory_order_release);
        g_pSharedMem->runtimeState.recordingStartTime.store(0, std::memory_order_release);
    }
}

bool GetWindowClientRectInScreen(HWND hwnd, RECT& rect) {
    RECT clientRect = {};
    if (!GetClientRect(hwnd, &clientRect)) {
        return false;
    }

    POINT topLeft = {clientRect.left, clientRect.top};
    POINT bottomRight = {clientRect.right, clientRect.bottom};
    if (!ClientToScreen(hwnd, &topLeft) || !ClientToScreen(hwnd, &bottomRight)) {
        return false;
    }

    rect.left = topLeft.x;
    rect.top = topLeft.y;
    rect.right = bottomRight.x;
    rect.bottom = bottomRight.y;
    return true;
}

bool RectNearlyMatches(const RECT& lhs, const RECT& rhs, LONG tolerance) {
    auto absDiff = [](LONG a, LONG b) -> LONG { return (a >= b) ? (a - b) : (b - a); };

    return absDiff(lhs.left, rhs.left) <= tolerance && absDiff(lhs.top, rhs.top) <= tolerance &&
           absDiff(lhs.right, rhs.right) <= tolerance && absDiff(lhs.bottom, rhs.bottom) <= tolerance;
}

bool IsWindowFullscreenLike(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor) {
        return false;
    }

    MONITORINFO monitorInfo = {sizeof(monitorInfo)};
    if (!GetMonitorInfo(monitor, &monitorInfo)) {
        return false;
    }

    RECT windowRect = {};
    RECT clientRect = {};
    const bool haveWindowRect = GetWindowRect(hwnd, &windowRect) != FALSE;
    const bool haveClientRect = GetWindowClientRectInScreen(hwnd, clientRect);
    constexpr LONG kFullscreenTolerancePx = 8;

    if (!haveWindowRect && !haveClientRect) {
        return false;
    }

    const bool windowMatchesMonitor =
        haveWindowRect && RectNearlyMatches(windowRect, monitorInfo.rcMonitor, kFullscreenTolerancePx);
    const bool clientMatchesMonitor =
        haveClientRect && RectNearlyMatches(clientRect, monitorInfo.rcMonitor, kFullscreenTolerancePx);
    if (!windowMatchesMonitor && !clientMatchesMonitor) {
        return false;
    }

    return true;
}

bool WindowBelongsToProcess(HWND hwnd, DWORD pid) {
    if (!hwnd || pid == 0) {
        return false;
    }

    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    return windowPid == pid;
}

bool ShouldPreferInjectCaptureForFullscreenWindow(HWND hwnd, DWORD pid) {
    return WindowBelongsToProcess(hwnd, pid) && IsWindowFullscreenLike(hwnd);
}

struct InjectFrameLineage {
    uint32_t frameIndex = 0;
    int32_t textureIndex = -1;
    uint64_t fenceValue = 0;
    uint32_t ringIndex = 0;
    int64_t timestamp = 0;
    int64_t enqueueQpc = 0;
    uint32_t deferCount = 0;

    bool IsValid() const {
        return frameIndex != 0 || fenceValue != 0 || timestamp != 0;
    }
};

struct CadenceHealthCounters {
    uint64_t frameAgeAccumUs = 0;
    uint32_t frameAgeSamples = 0;
    uint32_t frameAgeMaxUs = 0;
    uint64_t selectionErrorAccumUs = 0;
    int64_t selectionErrorSignedAccumUs = 0;
    uint32_t selectionErrorSamples = 0;
    uint32_t selectionErrorMaxUs = 0;
    uint32_t selectionEarlyMaxUs = 0;
    uint32_t selectionLateMaxUs = 0;
    uint32_t consecutiveDeferredFrames = 0;
    uint32_t maxConsecutiveDeferredFrames = 0;
    uint32_t consecutiveDuplicateFrames = 0;
    uint32_t maxConsecutiveDuplicateFrames = 0;
    uint32_t liveTickEmitCount = 0;
    uint32_t liveTickUniqueCount = 0;
    uint32_t liveTickDuplicateCount = 0;
    uint32_t liveTickMissCount = 0;
    uint64_t outputScheduleErrorAccumUs = 0;
    int64_t outputScheduleErrorSignedAccumUs = 0;
    uint32_t outputScheduleErrorSamples = 0;
    uint32_t outputScheduleErrorMaxUs = 0;
    uint32_t outputScheduleEarlyMaxUs = 0;
    uint32_t outputScheduleLateMaxUs = 0;

    // Hold-time histogram: how many output ticks each unique source frame was
    // shown for.  holdHist[0]=frames shown 1 tick, holdHist[1]=2 ticks, …
    // holdHist[kHoldHistBuckets-1]=6+ ticks.  Even distribution → smooth video.
    static constexpr uint32_t kHoldHistBuckets = 6;
    uint32_t holdHist[kHoldHistBuckets] = {};
    uint32_t holdTicksRunning = 0;  // ticks the current source frame has been shown

    void CommitHoldRun() {
        if (holdTicksRunning > 0) {
            const uint32_t bucket = std::min(holdTicksRunning, kHoldHistBuckets) - 1;
            holdHist[bucket]++;
            holdTicksRunning = 0;
        }
    }

    // Input frame rate predictor diagnostics (updated per-second)
    uint32_t srcFpsX100 = 0;         // smoothed input FPS * 100
    uint32_t srcJitterUs = 0;        // smoothed jitter in microseconds
    uint32_t encCycleAvgUs = 0;      // encoder cycle time EMA in microseconds
    uint32_t encCycleMaxUs = 0;      // max encoder cycle time in microseconds
    uint32_t dupTimestampCount = 0;  // frames with duplicate timestamps per second

    void Reset() {
        *this = {};
    }
};

enum class WgcAdaptiveThrottleMode : uint32_t {
    kOff = 0,
    kHeadroom108 = 1,
};

InjectFrameLineage MakeInjectFrameLineage(const QueuedFrame& frame) {
    InjectFrameLineage lineage;
    lineage.frameIndex = frame.frameIndex;
    lineage.textureIndex = frame.textureIndex;
    lineage.fenceValue = frame.fenceValue;
    lineage.ringIndex = frame.ringIndex;
    lineage.timestamp = frame.timestamp;
    lineage.enqueueQpc = frame.enqueueQpc;
    lineage.deferCount = frame.deferCount;
    return lineage;
}

bool MatchesInjectFrameLineage(const QueuedFrame& frame, const InjectFrameLineage& lineage) {
    return lineage.IsValid() && frame.isInjectMode && frame.frameIndex == lineage.frameIndex &&
           frame.textureIndex == lineage.textureIndex && frame.fenceValue == lineage.fenceValue &&
           frame.ringIndex == lineage.ringIndex && frame.timestamp == lineage.timestamp;
}

bool MatchesInjectFrameLineage(const InjectFrameLineage& lhs, const InjectFrameLineage& rhs) {
    return lhs.IsValid() && rhs.IsValid() && lhs.frameIndex == rhs.frameIndex && lhs.textureIndex == rhs.textureIndex &&
           lhs.fenceValue == rhs.fenceValue && lhs.ringIndex == rhs.ringIndex && lhs.timestamp == rhs.timestamp;
}

bool IsInjectTextureIndexValid(int32_t textureIndex) {
    return textureIndex >= 0 && textureIndex < kInjectTextureSlotCount;
}
}  // namespace

static bool JoinThreadWithTimeout(std::thread& thread, DWORD timeoutMs, const char* threadName) {
    if (!thread.joinable()) {
        return true;
    }

    HANDLE threadHandle = reinterpret_cast<HANDLE>(thread.native_handle());
    DWORD waitResult = WaitForSingleObject(threadHandle, timeoutMs);
    if (waitResult == WAIT_OBJECT_0) {
        thread.join();
        return true;
    }

    if (waitResult == WAIT_TIMEOUT) {
        LogWarn("[Media] Timeout waiting for %s thread (%lu ms), detaching", threadName,
                static_cast<unsigned long>(timeoutMs));
    } else {
        LogWarn("[Media] WaitForSingleObject failed for %s thread (error=%lu), detaching", threadName, GetLastError());
    }
    thread.detach();
    return false;
}

void MediaLogCallback(const char* msg) {
    LogInfo("[Media] %s", msg);
}

static void QueueWgcFrame(ID3D11Texture2D* texture, uint32_t width, uint32_t height, int64_t timestamp,
                          int64_t rawTimestamp, bool isHDR, bool duplicateSourceTimestamp, int32_t captureLeft,
                          int32_t captureTop) {
    static std::atomic<int64_t> s_lastWgcTimestamp{0};

    QueuedFrame qf;
    qf.isInjectMode = false;
    qf.texture = texture;
    qf.width = width;
    qf.height = height;
    qf.timestamp = timestamp;
    qf.rawTimestamp = rawTimestamp;
    qf.duplicateSourceTimestamp = duplicateSourceTimestamp;
    LARGE_INTEGER enqueueQpc;
    QueryPerformanceCounter(&enqueueQpc);
    qf.enqueueQpc = enqueueQpc.QuadPart;
    qf.isHDR = isHDR;
    qf.captureLeft = captureLeft;
    qf.captureTop = captureTop;

    if (g_pSharedMem) {
        const int64_t comparisonTimestamp = rawTimestamp > 0 ? rawTimestamp : timestamp;
        const int64_t previousTimestamp = s_lastWgcTimestamp.exchange(comparisonTimestamp, std::memory_order_relaxed);
        if (previousTimestamp > 0) {
            if (comparisonTimestamp < previousTimestamp) {
                g_pSharedMem->runtimeState.sourceTimestampRegressions.fetch_add(1, std::memory_order_relaxed);
            } else if (comparisonTimestamp == previousTimestamp) {
                g_pSharedMem->runtimeState.sourceTimestampStalls.fetch_add(1, std::memory_order_relaxed);
            }
        }
        g_pSharedMem->runtimeState.sourceFramesReceived.fetch_add(1, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.framesQueued.fetch_add(1, std::memory_order_relaxed);
    }

    if (!g_FrameQueue.Push(std::move(qf))) {
        texture->Release();
    }
}

static void ResetInjectFrameRingToLatest(const char* reason) {
    if (!g_pSharedMem) {
        return;
    }

    FrameRingBuffer& ring = g_pSharedMem->frameRing;
    uint32_t readIndex = ring.readIndex.load(std::memory_order_acquire);
    uint32_t writeIndex = ring.writeIndex.load(std::memory_order_acquire);
    if (readIndex == writeIndex) {
        return;
    }

    ring.readIndex.store(writeIndex, std::memory_order_release);
    LogInfo("[Media] Discarded %u stale inject frame(s) before %s", static_cast<unsigned>(writeIndex - readIndex),
            reason);
}

static void ResetLastQueuedFrameCache() {
    if (g_HasLastFrame && !g_LastFrame.isInjectMode && g_LastFrame.texture) {
        g_LastFrame.texture->Release();
    }
    g_LastFrame = QueuedFrame{};
    g_HasLastFrame = false;
}

static void StopInjectCapturePipeline() {
    g_InjectCaptureShutdown = true;
    JoinThreadWithTimeout(g_InjectCaptureThread, 5000, "inject capture");
    ResetInjectFrameRingToLatest("inject pipeline stop");
}

static void StopWgcCapturePipeline() {
    if (g_WgcCap) {
        g_WgcCap->SetDirectFrameCallback(nullptr);
        g_WgcCap->SetTargetFps(0);
        if (g_WgcCap->IsCapturing()) {
            g_WgcCap->StopCapture();
        }
    }

    g_WgcCaptureShutdown = true;
    g_WgcAdaptiveTargetFps.store(0, std::memory_order_relaxed);
    JoinThreadWithTimeout(g_WgcCaptureThread, 5000, "WGC capture");
}

static bool StartWgcRecordingCapture(const AppConfig& config) {
    if (!g_WgcCap) {
        return false;
    }

    if (g_WgcCaptureThread.joinable()) {
        LogWarn("[Media] Cleaning up stale WGC capture thread before restart");
        g_WgcCaptureShutdown = true;
        JoinThreadWithTimeout(g_WgcCaptureThread, 5000, "WGC capture");
    }

    if (g_WgcCap->IsCapturing()) {
        g_WgcCap->SetDirectFrameCallback(nullptr);
        g_WgcCap->StopCapture();
    }

    g_WgcCap->SetCaptureCursor(config.video.captureCursor);
    g_WgcCap->SetDirectFrameCallback(QueueWgcFrame);
    g_WgcCap->ResetStats();
    g_IsEncoderBottlenecked.store(false, std::memory_order_relaxed);
    g_WgcAdaptiveTargetFps.store(0, std::memory_order_relaxed);
    // Capture WGC at source cadence and let the fixed-rate encoder thread choose
    // the best frame each output tick. Pre-throttling WGC to the output FPS
    // throws away temporal information that helps smooth 140 -> 120 style cases.
    g_WgcCap->SetTargetFps(0);

    // For CFR recording, disable the encoder-bottleneck throttle at the WGC
    // callback level.  The throttle is all-or-nothing (bang-bang) and its slow
    // EMA causes boom-bust oscillation that starves the Bresenham credit
    // accumulator, producing irregular frame-hold patterns (visible judder).
    // The encoder thread's buffer cap + Bresenham skip already provide smooth
    // backpressure, so the throttle is both unnecessary and harmful for CFR.
    if (!config.video.useVFR) {
        g_WgcCap->SetThrottleFlag(nullptr);
        LogInfo("[Media] WGC CFR mode: encoder-bottleneck throttle disabled (buffer cap provides backpressure)");
    }
    if (g_pSharedMem) {
        g_pSharedMem->runtimeState.wgcTargetFps.store(0, std::memory_order_relaxed);
    }
    if (!g_WgcCap->StartCapture()) {
        g_WgcCap->SetDirectFrameCallback(nullptr);
        return false;
    }

    // Tell the encoder whether the capture source runs at >8 bpc so that
    // bit_depth=auto resolves to 10-bit even when the WGC frame pool fell
    // back to BGRA8 (e.g. R10G10B10A2 pool creation failed).
    if (MediaEngine_SetSourcePrefers10Bit) {
        const bool hiPrec = g_WgcCap->IsHighPrecisionSource();
        LogInfo("[Media] WGC source high-precision=%s, notifying encoder", hiPrec ? "YES" : "NO");
        MediaEngine_SetSourcePrefers10Bit(hiPrec);
    } else {
        LogWarn("[Media] MediaEngine_SetSourcePrefers10Bit not available (old mediaengine.dll?)");
    }

    g_WgcCaptureShutdown = false;
    g_WgcCaptureThread = std::thread(WgcCaptureThreadFunc, std::ref(config));
    SetThreadPriority(reinterpret_cast<HANDLE>(g_WgcCaptureThread.native_handle()), THREAD_PRIORITY_HIGHEST);
    return true;
}

// Defined in inject_main.cpp
extern std::string GetProcessNameFromPID(DWORD pid);

// Window finding helper
struct WindowSearch {
    DWORD pid;
    HWND hwnd;
};

static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    WindowSearch* search = (WindowSearch*)lParam;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == search->pid) {
        // Look for the main visible window
        // Checks: Visible, not child
        if (IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == 0) {
            // Check styles to avoid tool windows
            LONG_PTR styles = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            if (!(styles & WS_EX_TOOLWINDOW)) {
                search->hwnd = hwnd;
                return FALSE;  // Found, stop
            }
        }
    }
    return TRUE;
}

static HWND GetMainWindowForProcess(DWORD pid) {
    WindowSearch search = {pid, NULL};
    EnumWindows(EnumWindowsCallback, (LPARAM)&search);
    return search.hwnd;
}

static bool MatchesProcessEntry(const WhitelistEntry& entry, const std::string& lowerProcessName) {
    if (!entry.HasProcess() || lowerProcessName.empty()) {
        return false;
    }

    std::string lowerItem = entry.pattern;
    std::transform(lowerItem.begin(), lowerItem.end(), lowerItem.begin(), ::tolower);

    if (entry.mode == MatchMode::kExact) {
        return lowerProcessName == lowerItem;
    }

    return lowerProcessName == lowerItem || lowerProcessName.find(lowerItem) != std::string::npos;
}

static bool MatchesProcessEntries(const std::vector<WhitelistEntry>& entries, const std::string& processName) {
    if (processName.empty()) {
        return false;
    }

    std::string lowerName = processName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

    for (const auto& entry : entries) {
        if (MatchesProcessEntry(entry, lowerName)) {
            return true;
        }
    }

    return false;
}

static HWND FindMatchingWgcWindow(const std::vector<WhitelistEntry>& targets) {
    struct WgcSearchContext {
        const std::vector<WhitelistEntry>* targets;
        HWND result;
        int checked;
    };

    WgcSearchContext ctx = {&targets, NULL, 0};
    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            WgcSearchContext* context = (WgcSearchContext*)lParam;
            if (!IsWindowVisible(hwnd)) {
                return TRUE;
            }
            if (GetWindow(hwnd, GW_OWNER) != 0) {
                return TRUE;
            }

            context->checked++;

            char title[256];
            GetWindowTextA(hwnd, title, sizeof(title));
            std::string titleStr = title;
            std::transform(titleStr.begin(), titleStr.end(), titleStr.begin(), ::tolower);

            char className[256];
            GetClassNameA(hwnd, className, sizeof(className));
            std::string classStr = className;
            std::transform(classStr.begin(), classStr.end(), classStr.begin(), ::tolower);

            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            std::string procName;
            if (pid != 0) {
                HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (hProcess) {
                    char exePath[MAX_PATH];
                    DWORD size = MAX_PATH;
                    if (QueryFullProcessImageNameA(hProcess, 0, exePath, &size)) {
                        procName = exePath;
                        auto pos = procName.find_last_of("\\/");
                        if (pos != std::string::npos) {
                            procName = procName.substr(pos + 1);
                        }
                        std::transform(procName.begin(), procName.end(), procName.begin(), ::tolower);
                    }
                    CloseHandle(hProcess);
                }
            }

            for (const auto& entry : *context->targets) {
                MatchMode mode = entry.mode;
                bool matched = false;

                if (entry.HasWindow()) {
                    std::string winLower = entry.windowName;
                    std::transform(winLower.begin(), winLower.end(), winLower.begin(), ::tolower);

                    if (mode == MatchMode::kExact) {
                        matched = !titleStr.empty() && titleStr == winLower;
                    } else {
                        matched = !titleStr.empty() && titleStr.find(winLower) != std::string::npos;
                        if (!matched && mode == MatchMode::kTitleType && !classStr.empty()) {
                            matched = classStr.find(winLower) != std::string::npos;
                        }
                    }
                }

                if (!matched && MatchesProcessEntry(entry, procName)) {
                    matched = true;
                }

                if (matched) {
                    context->result = hwnd;
                    return FALSE;
                }
            }
            return TRUE;
        },
        (LPARAM)&ctx);

    return ctx.result;
}

static std::string GetLocalConfigPath() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string path = exePath;
    return path.substr(0, path.find_last_of("\\/")) + "\\config.ini";
}

class ScopedMmcssTask {
public:
    ScopedMmcssTask(const wchar_t* taskName, AVRT_PRIORITY priority) {
        DWORD taskIndex = 0;
        handle_ = AvSetMmThreadCharacteristicsW(taskName, &taskIndex);
        if (handle_) {
            AvSetMmThreadPriority(handle_, priority);
        }
    }

    ~ScopedMmcssTask() {
        if (handle_) {
            AvRevertMmThreadCharacteristics(handle_);
        }
    }

    ScopedMmcssTask(const ScopedMmcssTask&) = delete;
    ScopedMmcssTask& operator=(const ScopedMmcssTask&) = delete;

private:
    HANDLE handle_ = nullptr;
};

static void DisableCurrentThreadPowerThrottling() {
    THREAD_POWER_THROTTLING_STATE throttlingState = {};
    throttlingState.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttlingState.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttlingState.StateMask = 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttlingState, sizeof(throttlingState));
}

static void WaitUntilQpcTarget(HANDLE timer, int64_t targetQpc, int64_t qpcFrequency) {
    if (targetQpc <= 0 || qpcFrequency <= 0) {
        return;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int64_t diff = targetQpc - now.QuadPart;
    if (diff <= 0) {
        return;
    }

    int64_t diffUs = (diff * 1000000) / qpcFrequency;
    constexpr int64_t kTimerTrimUs = 400;
    if (timer && diffUs > kTimerTrimUs) {
        LARGE_INTEGER dueTime;
        dueTime.QuadPart = -static_cast<int64_t>(static_cast<double>(diffUs - kTimerTrimUs) * 10.0);
        if (SetWaitableTimer(timer, &dueTime, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, static_cast<DWORD>(((diffUs - kTimerTrimUs) / 1000) + 5));
        }
    }

    for (;;) {
        QueryPerformanceCounter(&now);
        diff = targetQpc - now.QuadPart;
        if (diff <= 0) {
            break;
        }

        diffUs = (diff * 1000000) / qpcFrequency;
        if (diffUs > 3000) {
            Sleep(1);
        } else if (diffUs > 1000) {
            Sleep(0);
        } else if (diffUs > 200) {
            SwitchToThread();
        } else {
            YieldProcessor();
        }
    }
}

static void ApplyMediaProcessPriority(const AppConfig& config) {
    DWORD priorityClass = NORMAL_PRIORITY_CLASS;
    if (config.processPriority == "idle")
        priorityClass = IDLE_PRIORITY_CLASS;
    else if (config.processPriority == "below_normal")
        priorityClass = BELOW_NORMAL_PRIORITY_CLASS;
    else if (config.processPriority == "above_normal")
        priorityClass = ABOVE_NORMAL_PRIORITY_CLASS;
    else if (config.processPriority == "high")
        priorityClass = HIGH_PRIORITY_CLASS;
    SetPriorityClass(GetCurrentProcess(), priorityClass);
}

// =================================================================================================
// THREAD FUNCTIONS
// =================================================================================================

void InjectCaptureThreadFunc(const AppConfig& config) {
    LogInfo(
        "[Inject Thread] Started (High Priority Polling with adaptive source-side "
        "Pacing)");
    g_InjectCaptureRunning = true;

    if (!g_pSharedMem) {
        LogError("[Inject Thread] Shared memory not available! Aborting.");
        g_InjectCaptureRunning = false;
        return;
    }

    // Local read index tracks what WE have pushed to the FrameQueue
    uint32_t localReadIndex = g_pSharedMem->frameRing.readIndex.load(std::memory_order_acquire);

    // PACING INITIALIZATION
    LARGE_INTEGER qpcFreq;
    QueryPerformanceFrequency(&qpcFreq);
    // Target interval in ticks (e.g. 1/120s)
    int64_t targetIntervalTicks =
        (config.video.fps > 0) ? (qpcFreq.QuadPart / config.video.fps) : (qpcFreq.QuadPart / 60);
    int64_t nextPushTime = 0;

    DWORD lastLog = GetTickCount();
    uint32_t pushedCount = 0;
    uint32_t droppedCount = 0;
    uint32_t pacingDroppedCount = 0;
    uint32_t emptySpinCount = 0;
    uint32_t lastDuplicateCount = 0;
    uint32_t lastLateCount = 0;
    uint32_t lastTrimmedCount = g_InjectBufferedTrimmedFrames.load(std::memory_order_relaxed);
    uint32_t lastCadenceDroppedCount = g_InjectCadenceDroppedFrames.load(std::memory_order_relaxed);
    uint32_t lastDeferredCount = g_InjectDeferredFrames.load(std::memory_order_relaxed);

    while (!g_InjectCaptureShutdown && g_Recording) {
        // Create encoder textures as soon as resolution is available (before frames arrive)
        // This is critical for DXVK where the Vulkan layer waits for encoder KMT textures
        // NOTE: non-static so it resets per thread lifetime (new recording = new thread)
        bool earlyTexturesCreated = false;
        if (!earlyTexturesCreated && g_pSharedMem->GetWidth() > 0 && g_pSharedMem->GetHeight() > 0) {
            if (!g_pSharedMem->encoderTextures.kmtReady.load(std::memory_order_acquire)) {
                if (MediaEngine_CreateSharedCaptureTextures(g_pSharedMem->GetWidth(), g_pSharedMem->GetHeight(),
                                                            g_pSharedMem->GetFormat(), g_pSharedMem)) {
                    LogInfo("[Inject Thread] Created encoder KMT textures early: %dx%d", g_pSharedMem->GetWidth(),
                            g_pSharedMem->GetHeight());
                    earlyTexturesCreated = true;
                }
            } else {
                earlyTexturesCreated = true;
            }
        }

        // 1. Check for new frames
        uint32_t writeIndex = g_pSharedMem->frameRing.writeIndex.load(std::memory_order_acquire);

        // Overflow Protection
        if (writeIndex > localReadIndex + FRAME_RING_SIZE) {
            uint32_t dropped = writeIndex - localReadIndex - 1;
            // Only log huge jumps to avoid spam
            if (dropped > 10) {
                LogInfo("[Inject Thread] Lag detected! Dropping %u frames to catch up", dropped);
            }
            localReadIndex = writeIndex - 1;
            droppedCount += dropped;
            g_pSharedMem->runtimeState.hostDroppedFrames.fetch_add(dropped, std::memory_order_relaxed);
            // Reset pacing on overflow/lag
            nextPushTime = 0;
        }

        if (writeIndex != localReadIndex) {
            emptySpinCount = 0;

            uint32_t index = localReadIndex % FRAME_RING_SIZE;
            FrameSlot& slot = g_pSharedMem->frameRing.slots[index];

            if (slot.valid.load(std::memory_order_acquire)) {
                // CRITICAL FIX: Ensure all slot fields are visible after valid flag
                // The acquire on valid provides synchronization with the producer's release,
                // but we add an explicit fence to prevent compiler reordering of reads.
                std::atomic_thread_fence(std::memory_order_acquire);

                // PACING CHECK:
                // If this frame is too early relative to the configured output grid,
                // drop it. This acts as a smart decimator for inputs running above the
                // requested capture FPS.
                bool shouldProcess = false;

                const uint32_t queueDepth = static_cast<uint32_t>(g_FrameQueue.Size());
                const uint32_t queuePressureThreshold =
                    std::max<uint32_t>(24u, static_cast<uint32_t>(g_FrameQueue.Capacity() * 3 / 4));
                const bool useSourceSidePacing = queueDepth >= queuePressureThreshold;

                if (!useSourceSidePacing) {
                    // When the queue is shallow and the encoder is healthy, apply a
                    // lightweight decimation gate at ~125% of target cadence.  This
                    // prevents queue overflow when game FPS >> recording FPS (e.g.
                    // 240fps game → 120fps recording) while still giving the encoder
                    // thread enough candidate frames for timestamp-aware selection.
                    // Without this gate, the 32-slot FrameQueue overflows between
                    // 1-second EMA updates, causing drop-oldest of frames the Bresenham
                    // would have selected.
                    int64_t overcaptureInterval = (targetIntervalTicks * 4) / 5;  // 125% rate
                    if (nextPushTime == 0) {
                        nextPushTime = slot.timestamp;
                        shouldProcess = true;
                    } else if (slot.timestamp >= nextPushTime) {
                        shouldProcess = true;
                        nextPushTime += overcaptureInterval;
                        // Resync on time jumps
                        if (slot.timestamp > nextPushTime + (targetIntervalTicks * 5)) {
                            nextPushTime = slot.timestamp + overcaptureInterval;
                        }
                    } else {
                        // Frame arrived before next cadence gate — drop it
                        shouldProcess = false;
                        pacingDroppedCount++;
                        g_pSharedMem->runtimeState.injectPacingDrops.fetch_add(1, std::memory_order_relaxed);
                    }
                } else if (nextPushTime == 0) {
                    // First frame or resync
                    nextPushTime = slot.timestamp;
                    shouldProcess = true;
                } else {
                    // Under real backlog/bottleneck pressure, re-enable source-side pacing
                    // to stop the queue from running away.
                    int64_t jitterWindow = (targetIntervalTicks * 8) / 10;  // 80% tolerance

                    if (slot.timestamp >= nextPushTime - jitterWindow) {
                        shouldProcess = true;

                        // Advance target time by actual interval, not to current timestamp
                        // This maintains steady output cadence even with jittery input
                        nextPushTime += targetIntervalTicks;

                        // Resync if game time jumped way ahead (e.g. pause/lag spike > 5
                        // frames) Increased from 3 to 5 frames to avoid unnecessary resyncs
                        if (slot.timestamp > nextPushTime + (targetIntervalTicks * 5)) {
                            nextPushTime = slot.timestamp + targetIntervalTicks;
                        }
                    } else {
                        // Frame is too early - only drop if we're not behind on processing
                        // Check if we have a backlog of frames waiting
                        uint32_t pendingFrames = (writeIndex > localReadIndex) ? (writeIndex - localReadIndex) : 0;

                        if (pendingFrames > 2) {
                            // We have a backlog, process this frame anyway to catch up
                            shouldProcess = true;
                            nextPushTime = slot.timestamp + targetIntervalTicks;
                        } else {
                            // Frame is genuinely too early and no backlog - safe to drop
                            shouldProcess = false;
                            pacingDroppedCount++;
                            g_pSharedMem->runtimeState.injectPacingDrops.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }

                if (shouldProcess) {
                    // Validate shared memory frame data before using it
                    uint32_t fw = g_pSharedMem->GetWidth();
                    uint32_t fh = g_pSharedMem->GetHeight();
                    int32_t texIdx = slot.textureIndex;

                    bool dropFrame = false;
                    if (fw == 0 || fh == 0 || fw > 15360 || fh > 8640 || texIdx < 0 || texIdx > 200) {
                        if (localReadIndex % 100 == 0) {
                            LogWarn("[Inject Thread] Invalid frame data: %ux%u texIdx=%d, dropping", fw, fh, texIdx);
                        }
                        g_pSharedMem->runtimeState.invalidFrameMetadata.fetch_add(1, std::memory_order_relaxed);
                        dropFrame = true;
                    }

                    QueuedFrame qf;
                    qf.isInjectMode = true;
                    qf.ringIndex = localReadIndex;
                    qf.frameIndex = slot.frameIndex;
                    qf.textureIndex = texIdx;
                    qf.timestamp = slot.timestamp;
                    LARGE_INTEGER enqueueQpc;
                    QueryPerformanceCounter(&enqueueQpc);
                    qf.enqueueQpc = enqueueQpc.QuadPart;

                    static int64_t s_lastInjectTimestamp = 0;
                    if (s_lastInjectTimestamp > 0) {
                        if (slot.timestamp < s_lastInjectTimestamp) {
                            g_pSharedMem->runtimeState.sourceTimestampRegressions.fetch_add(1,
                                                                                            std::memory_order_relaxed);
                        } else if (slot.timestamp == s_lastInjectTimestamp) {
                            g_pSharedMem->runtimeState.sourceTimestampStalls.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    s_lastInjectTimestamp = slot.timestamp;

                    // CRITICAL FIX: Reset valid flag after reading to prevent stale data
                    // on slot reuse
                    slot.valid.store(0, std::memory_order_release);

                    if (texIdx >= 100) {
                        qf.isShmem = true;
                        qf.shmemSlot = texIdx - 100;
                        qf.sharedHandle = nullptr;
                        qf.fenceHandle = nullptr;
                        qf.fenceValue = 0;
                    } else {
                        qf.isShmem = false;
                        qf.shmemSlot = 0;
                        if (texIdx >= 0 && texIdx < 8) {
                            qf.sharedHandle = (HANDLE)g_pSharedMem->GetSharedHandle(texIdx);
                            LogDebug("[Inject Thread] Read handle for texIdx=%d: %p", texIdx, qf.sharedHandle);
                        } else {
                            qf.sharedHandle = (HANDLE)g_pSharedMem->GetSharedHandle(0);
                            LogDebug("[Inject Thread] Invalid texIdx=%d, using handle 0: %p", texIdx, qf.sharedHandle);
                        }
                        const bool useEncoderTextureFence =
                            g_pSharedMem->useEncoderTextures.load(std::memory_order_acquire);
                        qf.fenceHandle = useEncoderTextureFence ? (HANDLE)g_pSharedMem->encoderTextures.GetFenceHandle()
                                                                : (HANDLE)g_pSharedMem->GetFenceShareHandle();
                        qf.fenceValue = slot.fenceValue;
                    }

                    qf.sourcePid = slot.sourcePid;
                    qf.width = g_pSharedMem->GetWidth();
                    qf.height = g_pSharedMem->GetHeight();
                    qf.format = g_pSharedMem->GetFormat();
                    qf.luidLow = g_pSharedMem->GetLuidLowPart();
                    qf.luidHigh = g_pSharedMem->GetLuidHighPart();
                    qf.isHDR = g_pSharedMem->GetIsHDR();

                    // Per-recording state (reset on thread creation)
                    bool sharedTexturesCreated = false;
                    if (!sharedTexturesCreated && g_pSharedMem->GetWidth() > 0 && g_pSharedMem->GetHeight() > 0) {
                        if (!g_pSharedMem->encoderTextures.ready.load(std::memory_order_acquire)) {
                            if (MediaEngine_CreateSharedCaptureTextures(g_pSharedMem->GetWidth(),
                                                                        g_pSharedMem->GetHeight(),
                                                                        g_pSharedMem->GetFormat(), g_pSharedMem)) {
                                sharedTexturesCreated = true;
                            }
                        } else {
                            sharedTexturesCreated = true;
                        }
                    }

                    // Validate handles look reasonable (not 0, not -1, not obviously stale)
                    bool validHandles = true;
                    if (!qf.isShmem) {
                        uint64_t handleVal = (uint64_t)qf.sharedHandle;
                        // Reject obviously invalid handles
                        if (handleVal == 0 || handleVal == 0xFFFFFFFFFFFFFFFF || handleVal == 0xCCCCCCCCCCCCCCCC ||
                            handleVal == 0xDDDDDDDDDDDDDDDD) {
                            LogInfo("[Inject Thread] Invalid handle detected (0x%p), skipping frame", qf.sharedHandle);
                            g_pSharedMem->runtimeState.invalidSharedHandles.fetch_add(1, std::memory_order_relaxed);
                            validHandles = false;
                        }
                    }

                    if (!dropFrame && validHandles) {
                        if (g_RejectInjectFrames.load(std::memory_order_acquire)) {
                            droppedCount++;
                            dropFrame = true;
                        } else {
                            const InjectFrameLineage lineage = MakeInjectFrameLineage(qf);
                            if (g_FrameQueue.Push(std::move(qf))) {
                                static uint64_t s_lastQueuedLineageLogTick = 0;
                                const uint64_t nowTick = GetTickCount64();
                                if (nowTick - s_lastQueuedLineageLogTick >= 1000) {
                                    LogInfo(
                                        "[Inject Thread] Queue frame=%u ring=%u tex=%d fence=%llu ts=%lld qDepth=%u",
                                        lineage.frameIndex, lineage.ringIndex, lineage.textureIndex,
                                        static_cast<unsigned long long>(lineage.fenceValue),
                                        static_cast<long long>(lineage.timestamp), queueDepth);
                                    s_lastQueuedLineageLogTick = nowTick;
                                }
                                if (!g_InjectDeliveredFirstFrame.exchange(true, std::memory_order_acq_rel)) {
                                    LogInfo("[Inject Thread] First actual inject frame queued");
                                }
                                pushedCount++;
                                g_pSharedMem->runtimeState.framesQueued.fetch_add(1, std::memory_order_relaxed);
                            } else {
                                droppedCount++;
                                dropFrame = true;
                            }
                        }
                    } else {
                        droppedCount++;
                        dropFrame = true;
                    }

                    if (dropFrame) {
                        localReadIndex++;
                        g_pSharedMem->frameRing.readIndex.store(localReadIndex, std::memory_order_release);
                        continue;
                    }
                } else {
                    // Pacing drop: release the ring slot immediately so the producer
                    // does not stall behind frames that will never be encoded.
                    slot.valid.store(0, std::memory_order_release);
                    localReadIndex++;
                    g_pSharedMem->frameRing.readIndex.store(localReadIndex, std::memory_order_release);
                    continue;
                }

                localReadIndex++;
            } else {
                localReadIndex++;
            }
        } else {
            emptySpinCount++;
            if (emptySpinCount > 1000) {
                Sleep(1);
            } else {
                std::this_thread::yield();
            }
        }

        DWORD now = GetTickCount();
        if (now - lastLog >= 1000) {
            uint32_t dupDelta = 0;
            uint32_t lateDelta = 0;
            uint32_t trimDelta = 0;
            uint32_t cadenceDropDelta = 0;
            uint32_t deferredDelta = 0;
            uint32_t overloadFlags = 0;
            uint32_t muxQueueBytes = 0;
            uint32_t encoderQueueDepth = static_cast<uint32_t>(g_FrameQueue.Size());
            if (g_pSharedMem) {
                uint32_t currentDup = g_pSharedMem->runtimeState.duplicateFrames.load(std::memory_order_relaxed);
                uint32_t currentLate = g_pSharedMem->runtimeState.lateFrames.load(std::memory_order_relaxed);
                uint32_t currentTrimmed = g_InjectBufferedTrimmedFrames.load(std::memory_order_relaxed);
                uint32_t currentCadenceDropped = g_InjectCadenceDroppedFrames.load(std::memory_order_relaxed);
                uint32_t currentDeferred = g_InjectDeferredFrames.load(std::memory_order_relaxed);
                dupDelta = currentDup - lastDuplicateCount;
                lateDelta = currentLate - lastLateCount;
                trimDelta = currentTrimmed - lastTrimmedCount;
                cadenceDropDelta = currentCadenceDropped - lastCadenceDroppedCount;
                deferredDelta = currentDeferred - lastDeferredCount;
                lastDuplicateCount = currentDup;
                lastLateCount = currentLate;
                lastTrimmedCount = currentTrimmed;
                lastCadenceDroppedCount = currentCadenceDropped;
                lastDeferredCount = currentDeferred;
                overloadFlags = g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
                muxQueueBytes = g_pSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed);
                encoderQueueDepth = g_pSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
                g_pSharedMem->runtimeState.injectTrimmedFrames.store(currentTrimmed, std::memory_order_relaxed);
                g_pSharedMem->runtimeState.injectCadenceDrops.store(currentCadenceDropped, std::memory_order_relaxed);
                g_pSharedMem->runtimeState.deferredFrames.store(currentDeferred, std::memory_order_relaxed);
            } else {
                uint32_t currentTrimmed = g_InjectBufferedTrimmedFrames.load(std::memory_order_relaxed);
                uint32_t currentCadenceDropped = g_InjectCadenceDroppedFrames.load(std::memory_order_relaxed);
                uint32_t currentDeferred = g_InjectDeferredFrames.load(std::memory_order_relaxed);
                trimDelta = currentTrimmed - lastTrimmedCount;
                cadenceDropDelta = currentCadenceDropped - lastCadenceDroppedCount;
                deferredDelta = currentDeferred - lastDeferredCount;
                lastTrimmedCount = currentTrimmed;
                lastCadenceDroppedCount = currentCadenceDropped;
                lastDeferredCount = currentDeferred;
            }

            uint32_t inputFrames = pushedCount + droppedCount + pacingDroppedCount;
            g_pSharedMem->runtimeState.sourceFramesReceived.fetch_add(inputFrames, std::memory_order_relaxed);
            LogInfo(
                "[Inject Perf] Input: %u | Queued: %u | DropFull: %u | DropPace: %u | HostQ: %u | EncQ: %u | Dup: %u "
                "| Late: %u | Trim: %u | SelDrop: %u | Def: %u | Encode: %lldus | Fence: %lldus | Mux: %uKB | "
                "Overload: 0x%X",
                inputFrames, pushedCount, droppedCount, pacingDroppedCount, static_cast<uint32_t>(g_FrameQueue.Size()),
                encoderQueueDepth, dupDelta, lateDelta, trimDelta, cadenceDropDelta, deferredDelta,
                MediaEngine_GetLastFrameEncodeTimeUs(), MediaEngine_GetLastFrameFenceWaitUs(),
                (muxQueueBytes + 1023u) / 1024u, overloadFlags);
            pushedCount = 0;
            droppedCount = 0;
            pacingDroppedCount = 0;
            lastLog = now;
        }
    }

    g_InjectCaptureRunning = false;
    LogInfo("[Inject Thread] Stopped");
}

void WgcCaptureThreadFunc(const AppConfig& config) {
    LogInfo("[WGC CaptureThread] Started (diagnostics logger)");
    g_WgcCaptureRunning = true;

    DWORD lastDiagTime = 0;
    uint32_t lastInputCount = 0;
    uint32_t lastCallbackCount = 0;
    uint64_t lastHostDroppedCount = 0;
    uint32_t lastPacingSkipCount = 0;
    uint32_t lastThrottleSkipCount = 0;
    uint32_t lastStaleSkipCount = 0;
    uint32_t lastStaleDuplicateTsCount = 0;
    uint32_t lastStaleOutOfOrderTsCount = 0;
    uint32_t lastCursorSkipCount = 0;
    uint32_t lastPoolDropCount = 0;
    uint32_t lastDuplicateCount = 0;
    uint32_t lastLateCount = 0;
    bool sessionPrimed = false;

    while (!g_WgcCaptureShutdown) {
        Sleep(1000);

        if (!g_Recording || !g_WgcCap) {
            sessionPrimed = false;
            lastInputCount = 0;
            lastCallbackCount = 0;
            lastHostDroppedCount = 0;
            lastPacingSkipCount = 0;
            lastThrottleSkipCount = 0;
            lastStaleSkipCount = 0;
            lastStaleDuplicateTsCount = 0;
            lastStaleOutOfOrderTsCount = 0;
            lastCursorSkipCount = 0;
            lastPoolDropCount = 0;
            lastDuplicateCount = 0;
            lastLateCount = 0;
            lastDiagTime = 0;
            continue;
        }

        if (!sessionPrimed) {
            lastInputCount = g_WgcCap->GetInputFrameCount();
            lastCallbackCount = g_WgcCap->GetCallbackFrameCount();
            lastHostDroppedCount = g_FrameQueue.GetDroppedCount();
            lastPacingSkipCount = g_WgcCap->GetPacingSkipCount();
            lastThrottleSkipCount = g_WgcCap->GetThrottleSkipCount();
            lastStaleSkipCount = g_WgcCap->GetStaleSkipCount();
            lastStaleDuplicateTsCount = g_WgcCap->GetStaleDuplicateTimestampCount();
            lastStaleOutOfOrderTsCount = g_WgcCap->GetStaleOutOfOrderTimestampCount();
            lastCursorSkipCount = g_WgcCap->GetCursorOnlySkipCount();
            lastPoolDropCount = g_WgcCap->GetPoolDropCount();
            if (g_pSharedMem) {
                lastDuplicateCount = g_pSharedMem->runtimeState.duplicateFrames.load(std::memory_order_relaxed);
                lastLateCount = g_pSharedMem->runtimeState.lateFrames.load(std::memory_order_relaxed);
            }
            lastDiagTime = GetTickCount();
            sessionPrimed = true;
            continue;
        }

        DWORD now = GetTickCount();
        if (now - lastDiagTime >= 1000) {
            uint32_t currentInputCount = g_WgcCap->GetInputFrameCount();
            uint32_t currentCount = g_WgcCap->GetCallbackFrameCount();
            uint64_t queueDropped = g_FrameQueue.GetDroppedCount();
            uint32_t currentPacingSkipCount = g_WgcCap->GetPacingSkipCount();
            uint32_t currentThrottleSkipCount = g_WgcCap->GetThrottleSkipCount();
            uint32_t currentStaleSkipCount = g_WgcCap->GetStaleSkipCount();
            uint32_t currentStaleDuplicateTsCount = g_WgcCap->GetStaleDuplicateTimestampCount();
            uint32_t currentStaleOutOfOrderTsCount = g_WgcCap->GetStaleOutOfOrderTimestampCount();
            uint32_t currentCursorSkipCount = g_WgcCap->GetCursorOnlySkipCount();
            uint32_t currentPoolDropCount = g_WgcCap->GetPoolDropCount();
            uint32_t inputFrames = currentInputCount - lastInputCount;
            uint32_t deliveredFrames = currentCount - lastCallbackCount;
            uint32_t hostDropDelta =
                static_cast<uint32_t>(queueDropped >= lastHostDroppedCount ? (queueDropped - lastHostDroppedCount) : 0);
            uint32_t pacingSkipDelta = currentPacingSkipCount - lastPacingSkipCount;
            uint32_t throttleSkipDelta = currentThrottleSkipCount - lastThrottleSkipCount;
            uint32_t staleSkipDelta = currentStaleSkipCount - lastStaleSkipCount;
            uint32_t staleDuplicateTsDelta = currentStaleDuplicateTsCount - lastStaleDuplicateTsCount;
            uint32_t staleOutOfOrderTsDelta = currentStaleOutOfOrderTsCount - lastStaleOutOfOrderTsCount;
            uint32_t cursorSkipDelta = currentCursorSkipCount - lastCursorSkipCount;
            uint32_t poolDropDelta = currentPoolDropCount - lastPoolDropCount;
            uint32_t queuedFrames = deliveredFrames >= hostDropDelta ? (deliveredFrames - hostDropDelta) : 0;
            int64_t copyUs = g_WgcCap->GetLastCopyTimeUs();
            int64_t srcIntervalAvgUs = g_WgcCap->GetSourceIntervalAvgUs();
            int64_t srcJitterAvgUs = g_WgcCap->GetSourceJitterAvgUs();
            int64_t srcJitterMaxUs = g_WgcCap->GetSourceJitterMaxUs();
            int64_t srcToCopyAvgUs = g_WgcCap->GetSourceToCopyLatencyAvgUs();
            int64_t srcToCopyMaxUs = g_WgcCap->GetSourceToCopyLatencyMaxUs();
            int64_t encodeUs = MediaEngine_GetLastFrameEncodeTimeUs();
            int64_t fenceUs = MediaEngine_GetLastFrameFenceWaitUs();
            uint32_t dupDelta = 0;
            uint32_t lateDelta = 0;
            uint32_t overloadFlags = 0;
            uint32_t muxQueueBytes = 0;
            uint32_t encoderQueueDepth = static_cast<uint32_t>(g_FrameQueue.Size());
            uint32_t cadenceSelAvgUs = 0;
            int32_t cadenceSelBiasUs = 0;
            uint32_t throttleTargetFps = g_WgcCap->GetTargetFps();
            const uint32_t deliveredRatePerSec = g_WgcCap->GetDeliveredRatePerSec();
            const uint32_t deliveredMin250Fps = g_WgcCap->GetDeliveredMin250Fps();
            const uint32_t deliveredMin500Fps = g_WgcCap->GetDeliveredMin500Fps();
            const uint32_t inputMin250Fps = g_WgcCap->GetInputMin250Fps();
            const uint32_t inputMin500Fps = g_WgcCap->GetInputMin500Fps();
            const uint32_t queueEmptyPermille =
                g_pSharedMem ? g_pSharedMem->runtimeState.wgcQueueEmptyTickPermille.load(std::memory_order_relaxed)
                             : 0u;
            const uint32_t bufferedAtTickAvgPermille =
                g_pSharedMem ? g_pSharedMem->runtimeState.wgcBufferedAtTickAvgPermille.load(std::memory_order_relaxed)
                             : 0u;
            const uint32_t bufferedAtTickMin =
                g_pSharedMem ? g_pSharedMem->runtimeState.wgcBufferedAtTickMin.load(std::memory_order_relaxed) : 0u;
            const uint32_t starvedTicks =
                g_pSharedMem ? g_pSharedMem->runtimeState.wgcStarvedTickCount.load(std::memory_order_relaxed) : 0u;
            const uint32_t singleFrameTicks =
                g_pSharedMem ? g_pSharedMem->runtimeState.wgcSingleFrameTickCount.load(std::memory_order_relaxed) : 0u;
            if (g_pSharedMem) {
                uint32_t currentDup = g_pSharedMem->runtimeState.duplicateFrames.load(std::memory_order_relaxed);
                uint32_t currentLate = g_pSharedMem->runtimeState.lateFrames.load(std::memory_order_relaxed);
                dupDelta = currentDup - lastDuplicateCount;
                lateDelta = currentLate - lastLateCount;
                lastDuplicateCount = currentDup;
                lastLateCount = currentLate;
                overloadFlags = g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
                muxQueueBytes = g_pSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed);
                encoderQueueDepth = g_pSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
                cadenceSelAvgUs = g_pSharedMem->runtimeState.selectionErrorAvgUs.load(std::memory_order_relaxed);
                cadenceSelBiasUs = g_pSharedMem->runtimeState.selectionErrorSignedAvgUs.load(std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcSourceFrameIntervalAvgUs.store(SaturatingToUint32(srcIntervalAvgUs),
                                                                             std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcSourceFrameJitterAvgUs.store(SaturatingToUint32(srcJitterAvgUs),
                                                                           std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcSourceFrameJitterMaxUs.store(SaturatingToUint32(srcJitterMaxUs),
                                                                           std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcSourceToCopyLatencyAvgUs.store(SaturatingToUint32(srcToCopyAvgUs),
                                                                             std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcSourceToCopyLatencyMaxUs.store(SaturatingToUint32(srcToCopyMaxUs),
                                                                             std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcTargetFps.store(throttleTargetFps, std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcDeliveredFramesPerSec.store(deliveredRatePerSec,
                                                                          std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcDeliveredMin250Fps.store(deliveredMin250Fps, std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcDeliveredMin500Fps.store(deliveredMin500Fps, std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcInputMin250Fps.store(inputMin250Fps, std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcInputMin500Fps.store(inputMin500Fps, std::memory_order_relaxed);
            }

            LogInfo(
                "[WGC Perf] Input: %u | Queued: %u | DropFull: %u | DropPace: %u | DropThrottle: %u | "
                "DropStale: %u (DupTs=%u OOO=%u) | DropCursor: %u | DropPool: %u | HostQ: %u | EncQ: %u | Dup: %u | "
                "Late: %u | "
                "SrcAvg: %lldus | JitAvg: %lldus | JitMax: %lldus | Src->Copy: %lld/%lldus | Deliv: %u | "
                "MinIn250/500: %u/%u | MinDel250/500: %u/%u | FreshMiss: %upm | BufAvg: %upm | BufMin: %u | "
                "NoFresh: %u | NoReserve: %u | SelAvg: %uus "
                "SelBias: %dus | Copy: %lldus | Encode: %lldus | Fence: %lldus | Throttle: %u | Mux: %uKB | "
                "Overload: 0x%X",
                inputFrames, queuedFrames, hostDropDelta, pacingSkipDelta, throttleSkipDelta, staleSkipDelta,
                staleDuplicateTsDelta, staleOutOfOrderTsDelta, cursorSkipDelta, poolDropDelta,
                static_cast<uint32_t>(g_FrameQueue.Size()), encoderQueueDepth, dupDelta, lateDelta, srcIntervalAvgUs,
                srcJitterAvgUs, srcJitterMaxUs, srcToCopyAvgUs, srcToCopyMaxUs, deliveredRatePerSec, inputMin250Fps,
                inputMin500Fps, deliveredMin250Fps, deliveredMin500Fps, queueEmptyPermille, bufferedAtTickAvgPermille,
                bufferedAtTickMin, starvedTicks, singleFrameTicks, cadenceSelAvgUs, cadenceSelBiasUs, copyUs, encodeUs,
                fenceUs, throttleTargetFps, (muxQueueBytes + 1023u) / 1024u, overloadFlags);

            lastInputCount = currentInputCount;
            lastCallbackCount = currentCount;
            lastHostDroppedCount = queueDropped;
            lastPacingSkipCount = currentPacingSkipCount;
            lastThrottleSkipCount = currentThrottleSkipCount;
            lastStaleSkipCount = currentStaleSkipCount;
            lastStaleDuplicateTsCount = currentStaleDuplicateTsCount;
            lastStaleOutOfOrderTsCount = currentStaleOutOfOrderTsCount;
            lastCursorSkipCount = currentCursorSkipCount;
            lastPoolDropCount = currentPoolDropCount;
            lastDiagTime = now;
        }
    }

    g_WgcCaptureRunning = false;
    LogInfo("[WGC CaptureThread] Stopped");
}

void EncoderThreadFunc(const AppConfig& config) {
    LogInfo("[EncoderThread] Started");

    DisableCurrentThreadPowerThrottling();
    ScopedMmcssTask encoderMmcssTask(L"Pro Audio", AVRT_PRIORITY_HIGH);

    g_FrameQueue.StartRecording();
    SetCapturePipelinePhase(CapturePipelinePhase::kWarmup);

    LARGE_INTEGER qpcFreq;
    QueryPerformanceFrequency(&qpcFreq);
    int64_t targetIntervalTicks = qpcFreq.QuadPart / config.video.fps;
    LARGE_INTEGER nextSampleTime;
    QueryPerformanceCounter(&nextSampleTime);

    HANDLE hTimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!hTimer) {
        hTimer = CreateWaitableTimer(NULL, TRUE, NULL);
        LogInfo("[EncoderThread] Using standard waitable timer");
    } else {
        LogInfo("[EncoderThread] Using high-resolution waitable timer");
    }

    double smoothedEncodeMs = 0.0;
    double frameIntervalMs = 1000.0 / config.video.fps;
    auto ReleaseQueuedFrameTexture = [](QueuedFrame& queuedFrame) {
        if (!queuedFrame.isInjectMode && queuedFrame.texture) {
            queuedFrame.texture->Release();
            queuedFrame.texture = nullptr;
        }
    };
    auto DiscardQueuedFrame = [&](QueuedFrame& queuedFrame) {
        if (queuedFrame.isInjectMode) {
            if (g_pSharedMem) {
                g_pSharedMem->frameRing.readIndex.store(queuedFrame.ringIndex + 1, std::memory_order_release);
            }
        } else {
            ReleaseQueuedFrameTexture(queuedFrame);
        }
        queuedFrame = QueuedFrame{};
    };
    std::vector<QueuedFrame> drainedScreenGrabFrames;
    drainedScreenGrabFrames.reserve(8);
    std::deque<QueuedFrame> bufferedWgcFrames;
    std::vector<QueuedFrame> drainedInjectFrames;
    drainedInjectFrames.reserve(8);
    std::deque<QueuedFrame> bufferedInjectFrames;
    double smoothedInjectFenceMs = 0.0;
    bool recordingOutputLive = false;
    bool pendingLiveActivation = false;
    uint64_t startupWarmupStartTick = GetTickCount64();
    uint64_t recordingLiveTick = 0;
    uint32_t hiddenStartupFrames = 0;
    ce::capture_policy::WarmupTransitionState warmupState = {
        IsActiveScreenGrab(),
        GetTickCount64(),
        0,
    };
    uint32_t pendingInjectTrimmedLogCount = 0;
    size_t maxBufferedInjectDepthSinceLog = 0;
    DWORD lastInjectTrimLog = GetTickCount();
    // Bresenham credit-based frame pacing state: distributes duplicates evenly
    // when source fps < recording target fps instead of clustering them at
    // frame-timing jitter boundaries. When source fps > target fps, selection
    // stays timestamp-aware against the encoder output grid.
    uint32_t pacingInputThisWindow = 0;
    uint32_t pacingTicksThisWindow = 0;
    uint32_t pacingEmaUpdates = 0;
    double smoothedInputPerTick = 1.0;    // EMA: avg unique frames per encoder tick
    double frameCreditAccumulator = 0.0;  // Bresenham error term
    // Output-grid tracking for timestamp-aware frame selection.
    // When multiple buffered frames are available (game fps > target fps),
    // selecting the frame closest to the ideal output grid time produces
    // the smoothest motion in the CFR output.
    int64_t encoderGridStartQpc = 0;
    int64_t encoderGridTickCount = 0;
    uint64_t selectionLogCounter = 0;
    uint32_t lastEncodedInjectFrameIndex = 0;
    std::array<uint32_t, kInjectTextureSlotCount> lastEncodedFrameByTextureIndex{};
    InjectFrameLineage lastDeferredLineage;
    CadenceHealthCounters cadenceCounters;
    InputFrameRatePredictor wgcInputPredictor;
    double smoothedEncCycleMs = 0.0;
    uint32_t encCycleMaxMs = 0;
    uint32_t encodeSpikeCountThisSecond = 0;
    uint32_t dupTimestampCount = 0;
    uint32_t lastDuplicateReasonNoSource = 0;
    uint32_t lastDuplicateReasonDeferred = 0;
    uint32_t lastDuplicateReasonTimerRebase = 0;
    uint32_t lastDuplicateReasonDrain = 0;
    uint32_t lastInvalidMetaCount = 0;
    uint32_t lastInvalidHandleCount = 0;
    uint32_t lastTimestampRegressionCount = 0;
    uint32_t lastTimestampStallCount = 0;
    uint32_t lastPacketClampCount = 0;
    uint32_t lastNegativePtsCount = 0;
    uint32_t lastNonMonotonicPtsCount = 0;
    size_t pendingLiveInjectReadyFrames = 0;
    DWORD lastHealthLog = GetTickCount();
    LARGE_INTEGER liveStartQpc = {};
    uint64_t liveTicksOutput = 0;
    uint64_t liveTicksScheduled = 0;
    uint64_t liveTicksDiscardedByTimerRebase = 0;
    uint64_t wgcSelectionErrorAccumUs = 0;
    int64_t wgcSelectionErrorSignedAccumUs = 0;
    uint32_t wgcSelectionErrorSamples = 0;
    uint32_t wgcSelectionErrorMaxUs = 0;
    uint32_t wgcSelectionEarlyMaxUs = 0;
    uint32_t wgcSelectionLateMaxUs = 0;
    uint32_t wgcHoldForNextTickCount = 0;
    uint32_t wgcAdaptiveThrottleAdjustments = 0;
    WgcAdaptiveThrottleMode wgcAdaptiveThrottleMode = WgcAdaptiveThrottleMode::kOff;
    uint32_t wgcRecentDeliveredFps = 0;
    uint32_t wgcRecentDeliveredMin250Fps = 0;
    uint32_t wgcRecentDeliveredMin500Fps = 0;
    uint32_t wgcRecentInputMin250Fps = 0;
    uint32_t wgcRecentInputMin500Fps = 0;
    uint32_t wgcNoFreshTickCount = 0;
    uint32_t wgcQueueTickSampleCount = 0;
    uint32_t wgcNoFreshTickPermille = 0;
    uint32_t wgcBufferedAtTickSum = 0;
    uint32_t wgcBufferedAtTickMin = UINT32_MAX;
    uint32_t wgcNoReserveTickCount = 0;
    uint32_t wgcAncientSelectionCount = 0;
    uint32_t wgcFreshSelectionMissCount = 0;
    uint32_t wgcHeldFreshFrameTickCount = 0;
    uint32_t cfrCatchupTicksExecuted = 0;
    uint32_t wgcReserveSpendTickCount = 0;
    uint32_t wgcStaleUniqueFallbackCount = 0;
    uint32_t wgcRepeatNoFreshCount = 0;
    uint32_t wgcRepeatPolicyHoldCount = 0;
    uint32_t wgcRepeatTimerLateCount = 0;
    uint32_t wgcRepeatCatchupCount = 0;
    uint32_t wgcFreshCatchupCount = 0;
    uint32_t wgcSelectFreshCount = 0;
    uint32_t wgcSelectDuplicateSourceCount = 0;
    uint32_t wgcDropObsoleteCount = 0;
    uint32_t wgcCoverageRepeatHoldCount = 0;
    uint32_t wgcCoverageDelayTicksCurrent = 0;
    bool wgcCoverageRepeatActiveCurrent = false;
    bool wgcLowSourceModeActive = false;
    bool wgcReservePressureActive = false;
    uint64_t wgcLowSourceStateChangeTick = 0;
    int64_t lastEmittedWgcSourceQpc = 0;
    int64_t lastWarmupWgcSourceQpc = 0;
    uint32_t wgcFreshWarmupFrameCount = 0;
    uint32_t wgcOldestBufferedFrameAgeUs = 0;
    double wgcCoverageRepeatAccumulator = 0.0;
    auto ClearBufferedInjectFrames = [&]() {
        while (!bufferedInjectFrames.empty()) {
            QueuedFrame queuedFrame = std::move(bufferedInjectFrames.front());
            bufferedInjectFrames.pop_front();
            DiscardQueuedFrame(queuedFrame);
        }
    };
    auto ClearBufferedWgcFrames = [&]() {
        while (!bufferedWgcFrames.empty()) {
            QueuedFrame queuedFrame = std::move(bufferedWgcFrames.front());
            bufferedWgcFrames.pop_front();
            ReleaseQueuedFrameTexture(queuedFrame);
        }
    };
    auto ResetWarmupWgcFreshness = [&]() {
        lastWarmupWgcSourceQpc = 0;
        wgcFreshWarmupFrameCount = 0;
    };
    auto TrackWarmupWgcFreshFrame = [&](const QueuedFrame& queuedFrame) {
        if (queuedFrame.isInjectMode || queuedFrame.timestamp <= 0) {
            return;
        }
        if (queuedFrame.timestamp > lastWarmupWgcSourceQpc) {
            lastWarmupWgcSourceQpc = queuedFrame.timestamp;
            ++wgcFreshWarmupFrameCount;
        }
    };
    auto updateLiveCfrShortfall = [&](int64_t nowQpc) {
        if (config.video.useVFR || !recordingOutputLive || liveStartQpc.QuadPart <= 0 || targetIntervalTicks <= 0 ||
            nowQpc <= liveStartQpc.QuadPart) {
            liveTicksScheduled = 0;
            return 0u;
        }
        int64_t scheduledUntilQpc = nowQpc;
        if (!g_Recording.load(std::memory_order_acquire)) {
            const int64_t drainStopQpc = g_WgcDrainStopQpc.load(std::memory_order_acquire);
            if (drainStopQpc > liveStartQpc.QuadPart) {
                scheduledUntilQpc = drainStopQpc;
            }
        }
        const uint64_t elapsedTicks =
            static_cast<uint64_t>(scheduledUntilQpc - liveStartQpc.QuadPart) / static_cast<uint64_t>(targetIntervalTicks);
        liveTicksScheduled =
            ce::capture_policy::GetAdjustedCfrScheduledTicks(elapsedTicks, liveTicksDiscardedByTimerRebase);
        return ce::capture_policy::GetCfrOutputShortfallTicks(liveTicksScheduled, liveTicksOutput);
    };
    while (g_EncoderRunning || g_DrainOutstandingWgcTicks.load(std::memory_order_acquire) || g_FrameQueue.Size() > 0 ||
           !bufferedWgcFrames.empty() || !bufferedInjectFrames.empty()) {
        LARGE_INTEGER cycleStartQpc;
        // NOTE: cycleStartQpc is set after timer sleep below to measure
        // encode processing time, not the full loop including sleep.
        if (g_pSharedMem) {
            if (!g_Recording.load(std::memory_order_acquire)) {
                g_pSharedMem->runtimeState.capturePhase.store(static_cast<uint32_t>(CapturePipelinePhase::kDrain),
                                                              std::memory_order_relaxed);
            } else if (recordingOutputLive) {
                g_pSharedMem->runtimeState.capturePhase.store(static_cast<uint32_t>(CapturePipelinePhase::kLive),
                                                              std::memory_order_relaxed);
            }
        }
        static DWORD lastThreadLog = 0;
        if (GetTickCount() - lastThreadLog > 1000) {
            LogInfo(
                "[EncoderThread] Alive. Q=%u Bot=%d Rate=%.3f Credit=%.2f IBuf=%zu WBuf=%zu Grid=%lld Live=%d "
                "EMA=%u Fence=%.2fms Encode=%.2fms",
                (unsigned int)g_FrameQueue.Size(), (int)g_IsEncoderBottlenecked, smoothedInputPerTick,
                frameCreditAccumulator, bufferedInjectFrames.size(), bufferedWgcFrames.size(),
                static_cast<long long>(encoderGridTickCount), (int)recordingOutputLive, pacingEmaUpdates,
                smoothedInjectFenceMs, smoothedEncodeMs);
            lastThreadLog = GetTickCount();
        }

        if (g_pSharedMem) {
            UpdateAtomicPeak(g_pSharedMem->runtimeState.bufferedInjectDepthPeak,
                             static_cast<uint32_t>(bufferedInjectFrames.size()));
        }

        if (g_pSharedMem) {
            uint32_t queueDepth = (uint32_t)g_FrameQueue.Size();
            queueDepth += static_cast<uint32_t>(bufferedInjectFrames.size());
            queueDepth += static_cast<uint32_t>(bufferedWgcFrames.size());
            double fenceWaitMs = (double)MediaEngine_GetLastFrameFenceWaitUs() / 1000.0;
            const uint32_t queuePressureThreshold =
                std::max<uint32_t>(8u, static_cast<uint32_t>(g_FrameQueue.Capacity() / 2));
            bool shouldThrottle = queueDepth >= queuePressureThreshold || fenceWaitMs > 16.0;

            g_pSharedMem->encoderQueueDepth.store(queueDepth, std::memory_order_relaxed);
            g_pSharedMem->throttleCapture.store(shouldThrottle, std::memory_order_release);
            g_pSharedMem->runtimeState.hostDroppedFrames.store(static_cast<uint32_t>(g_FrameQueue.GetDroppedCount()));
            UpdateAtomicPeak(g_pSharedMem->runtimeState.encoderQueuePeakDepth, queueDepth);

            int64_t oldestBufferedTimestamp = 0;
            if (!bufferedInjectFrames.empty()) {
                oldestBufferedTimestamp = bufferedInjectFrames.front().timestamp;
            } else if (!bufferedWgcFrames.empty()) {
                oldestBufferedTimestamp = bufferedWgcFrames.front().timestamp;
            }
            if (oldestBufferedTimestamp > 0) {
                LARGE_INTEGER nowQpc;
                QueryPerformanceCounter(&nowQpc);
                uint64_t oldestAgeUs = 0;
                if (nowQpc.QuadPart > oldestBufferedTimestamp) {
                    oldestAgeUs =
                        static_cast<uint64_t>((nowQpc.QuadPart - oldestBufferedTimestamp) * 1000000 / qpcFreq.QuadPart);
                }
                wgcOldestBufferedFrameAgeUs = SaturatingToUint32(oldestAgeUs);
                g_pSharedMem->runtimeState.oldestBufferedFrameAgeUs.store(wgcOldestBufferedFrameAgeUs,
                                                                          std::memory_order_relaxed);
            } else {
                wgcOldestBufferedFrameAgeUs = 0;
                g_pSharedMem->runtimeState.oldestBufferedFrameAgeUs.store(0, std::memory_order_relaxed);
            }
        }

        uint32_t outputShortfallTicks = 0;
        const bool activeScreenGrab = IsActiveScreenGrab();
        if (!config.video.useVFR && recordingOutputLive) {
            LARGE_INTEGER shortfallNow;
            QueryPerformanceCounter(&shortfallNow);
            outputShortfallTicks = updateLiveCfrShortfall(shortfallNow.QuadPart);
        }
        if (!g_Recording.load(std::memory_order_acquire) && recordingOutputLive &&
            g_DrainOutstandingWgcTicks.load(std::memory_order_acquire)) {
            const bool mediaEngineCanRepeatLastFrame =
                activeScreenGrab && MediaEngine_CanRepeatLastFrame && MediaEngine_CanRepeatLastFrame();
            const bool canDrainOutstandingTicks = ce::capture_policy::CanDrainOutstandingWgcTicks(
                g_FrameQueue.Size() > 0, !bufferedWgcFrames.empty(), g_HasLastFrame, mediaEngineCanRepeatLastFrame);
            static uint64_t s_lastStopDrainProgressLogTick = 0;
            const uint64_t nowTick = GetTickCount64();
            if (outputShortfallTicks > 0 && nowTick - s_lastStopDrainProgressLogTick >= 5000) {
                LogInfo(
                    "[EncoderThread] WGC stop drain progress: shortfall=%u/%.1fms queue=%u buffered=%zu hostLast=%d "
                    "cachedRepeat=%d",
                    outputShortfallTicks, ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs),
                    static_cast<unsigned>(g_FrameQueue.Size()), bufferedWgcFrames.size(), g_HasLastFrame ? 1 : 0,
                    mediaEngineCanRepeatLastFrame ? 1 : 0);
                s_lastStopDrainProgressLogTick = nowTick;
            }
            if (outputShortfallTicks == 0 || !canDrainOutstandingTicks) {
                if (outputShortfallTicks == 0) {
                    LogInfo("[EncoderThread] WGC stop drain complete: scheduled=%llu output=%llu",
                            static_cast<unsigned long long>(liveTicksScheduled),
                            static_cast<unsigned long long>(liveTicksOutput));
                } else {
                    LogWarn(
                        "[EncoderThread] WGC stop drain aborted: no frame available for outstanding shortfall=%u/%.1fms "
                        "(queue=%u buffered=%zu hostLast=%d cachedRepeat=%d)",
                        outputShortfallTicks, ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs),
                        static_cast<unsigned>(g_FrameQueue.Size()), bufferedWgcFrames.size(), g_HasLastFrame ? 1 : 0,
                        mediaEngineCanRepeatLastFrame ? 1 : 0);
                }
                s_lastStopDrainProgressLogTick = 0;
                g_DrainOutstandingWgcTicks.store(false, std::memory_order_release);
            }
        }

        const bool frameAvailableForCatchup =
            activeScreenGrab ? (!bufferedWgcFrames.empty()) : (!bufferedInjectFrames.empty());
        bool shouldCatchUpToWallClock = false;
        uint32_t catchupTicksThisLoop = 0;
        const auto computeWgcCoverageRepeatActive = [&]() {
            if (!activeScreenGrab || !recordingOutputLive) {
                return false;
            }
            const double shortfallDurationMs =
                ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs);
            const double oldestBufferedFrameAgeMs = static_cast<double>(wgcOldestBufferedFrameAgeUs) / 1000.0;
            return ce::capture_policy::HasWgcUnrecoverableCoverageLoss(shortfallDurationMs, oldestBufferedFrameAgeMs);
        };
        auto recomputeCatchupPolicy = [&]() {
            wgcCoverageRepeatActiveCurrent = computeWgcCoverageRepeatActive();
            shouldCatchUpToWallClock =
                !config.video.useVFR && recordingOutputLive &&
                ce::capture_policy::ShouldCfrCatchUpToWallClock(outputShortfallTicks, activeScreenGrab,
                                                                frameAvailableForCatchup, g_HasLastFrame);
            catchupTicksThisLoop =
                shouldCatchUpToWallClock
                    ? (activeScreenGrab ? ce::capture_policy::GetWgcCatchupTicksThisLoop(
                                              g_IsEncoderBottlenecked.load(std::memory_order_relaxed),
                                              bufferedWgcFrames.size(), frameCreditAccumulator, outputShortfallTicks)
                                        : ce::capture_policy::GetCfrCatchupTicksThisLoop(outputShortfallTicks))
                    : 0u;
            if (activeScreenGrab && wgcCoverageRepeatActiveCurrent &&
                g_IsEncoderBottlenecked.load(std::memory_order_relaxed)) {
                catchupTicksThisLoop = std::min<uint32_t>(catchupTicksThisLoop, 1u);
            }
        };
        recomputeCatchupPolicy();

        const int64_t selectionGridTick =
            (!config.video.useVFR && recordingOutputLive) ? (encoderGridTickCount + 1) : encoderGridTickCount;
        int64_t scheduledSampleQpc = 0;
        int64_t encoderLateQpc = 0;
        uint32_t encoderLateTickCount = 0;
        bool drainingOutstandingLiveTicks =
            !g_EncoderRunning && g_DrainOutstandingWgcTicks.load(std::memory_order_acquire) && recordingOutputLive &&
            !config.video.useVFR;
        if (g_EncoderRunning || drainingOutstandingLiveTicks) {
            scheduledSampleQpc = nextSampleTime.QuadPart;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (g_EncoderRunning) {
                int64_t waitTicks = nextSampleTime.QuadPart - now.QuadPart;
                if (waitTicks > 0) {
                    WaitUntilQpcTarget(hTimer, scheduledSampleQpc, qpcFreq.QuadPart);
                }

                QueryPerformanceCounter(&now);
                cycleStartQpc = now;  // Start measuring encode processing after timer sleep
                if (!config.video.useVFR && targetIntervalTicks > 0 && now.QuadPart > scheduledSampleQpc) {
                    encoderLateQpc = now.QuadPart - scheduledSampleQpc;
                    const uint64_t lateTicks =
                        static_cast<uint64_t>(encoderLateQpc) / static_cast<uint64_t>(targetIntervalTicks);
                    encoderLateTickCount = SaturatingToUint32(lateTicks);
                }

                nextSampleTime.QuadPart += targetIntervalTicks;

                // Periodically resync the encoder grid to wall clock time to
                // prevent systematic drift when encoder ticks are consistently
                // longer than the target interval.  Without this, the selection
                // target grows increasingly out of sync with actual frame times.
                if (recordingOutputLive && encoderGridStartQpc > 0 && targetIntervalTicks > 0 && liveTicksOutput > 0 &&
                    (liveTicksOutput % 60 == 0)) {
                    LARGE_INTEGER resyncNow;
                    QueryPerformanceCounter(&resyncNow);
                    const int64_t idealGridStart =
                        resyncNow.QuadPart - static_cast<int64_t>(liveTicksOutput) * targetIntervalTicks;
                    const int64_t driftTicks = (idealGridStart - encoderGridStartQpc) / targetIntervalTicks;
                    if (driftTicks >= 2 || driftTicks <= -2) {
                        encoderGridStartQpc = idealGridStart;
                    }
                }

                // Hidden warmup can rebase freely because those frames are discarded.
                // Once recording is live, skip ahead when significantly late to prevent
                // linear accumulation of encoder timer drift.  The buffered WGC frames
                // provide continuity — the output PTS gap is filled from the frame pool.
                if (!recordingOutputLive && now.QuadPart > nextSampleTime.QuadPart + targetIntervalTicks * 2) {
                    nextSampleTime = now;
                } else if (recordingOutputLive && encoderLateTickCount >= 2) {
                    static uint32_t s_lateTickLogCount = 0;
                    s_lateTickLogCount++;
                    if (g_pSharedMem) {
                        g_pSharedMem->runtimeState.timerRebases.fetch_add(1, std::memory_order_relaxed);
                    }
                    int64_t overshootTicks = (encoderLateQpc + targetIntervalTicks - 1) / targetIntervalTicks;
                    const int64_t overshootUs = (encoderLateQpc * 1000000) / qpcFreq.QuadPart;
                    uint32_t droppedShortfallTicks = 0;
                    const bool discardTimerDebt = ce::capture_policy::ShouldDiscardCfrTimerRebaseDebt(activeScreenGrab);
                    if (discardTimerDebt && liveStartQpc.QuadPart > 0 && now.QuadPart > liveStartQpc.QuadPart) {
                        const uint64_t elapsedTicks = static_cast<uint64_t>(now.QuadPart - liveStartQpc.QuadPart) /
                                                      static_cast<uint64_t>(targetIntervalTicks);
                        droppedShortfallTicks = ce::capture_policy::GetCfrTimerRebaseDiscardTicks(
                            elapsedTicks, liveTicksDiscardedByTimerRebase, liveTicksOutput);
                    }
                    liveTicksDiscardedByTimerRebase += droppedShortfallTicks;
                    outputShortfallTicks = updateLiveCfrShortfall(now.QuadPart);
                    recomputeCatchupPolicy();
                    if (s_lateTickLogCount <= 10 || s_lateTickLogCount % 60 == 0) {
                        LogInfo(
                            "[EncoderThread] Timer skip-ahead: late by %lld ticks (%lld us), rebasing "
                            "(count=%u, dropShortfall=%u, discardTotal=%llu, preserveShortfall=%u)",
                            (long long)overshootTicks, (long long)overshootUs, s_lateTickLogCount, droppedShortfallTicks,
                            static_cast<unsigned long long>(liveTicksDiscardedByTimerRebase), discardTimerDebt ? 0u : 1u);
                    }
                    // Reset nextSampleTime to current time + 1 tick interval
                    // so the timer wakes on time from now on.
                    nextSampleTime.QuadPart = now.QuadPart + targetIntervalTicks;
                }
            } else {
                cycleStartQpc = now;
                nextSampleTime.QuadPart += targetIntervalTicks;
            }
        }

        const auto computeWgcSelectionTargetForTick = [&](int64_t scheduledQpcForTick, int64_t selectionGridTickForTick,
                                                          bool applyLiveDelay) {
            const int64_t fallbackTargetQpc =
                ComputeIdealOutputQpc(encoderGridStartQpc, selectionGridTickForTick, targetIntervalTicks);
            int64_t selectionTargetQpc = ce::capture_policy::GetWgcSelectionTargetQpc(
                scheduledQpcForTick, fallbackTargetQpc, targetIntervalTicks, applyLiveDelay);
            if (selectionTargetQpc > 0 && targetIntervalTicks > 0 && wgcCoverageDelayTicksCurrent > 0) {
                const int64_t coverageDelayQpc =
                    static_cast<int64_t>(wgcCoverageDelayTicksCurrent) * targetIntervalTicks;
                if (coverageDelayQpc > 0 && selectionTargetQpc > coverageDelayQpc) {
                    selectionTargetQpc -= coverageDelayQpc;
                }
            }
            return selectionTargetQpc;
        };
        const auto computeWgcSelectionTargetQpc = [&](bool applyLiveDelay) {
            return computeWgcSelectionTargetForTick(scheduledSampleQpc, selectionGridTick, applyLiveDelay);
        };
        const auto computeLiveTimelineElapsedUs = [&](int64_t scheduledQpcForTick) -> int64_t {
            if (liveStartQpc.QuadPart <= 0 || qpcFreq.QuadPart <= 0) {
                return -1;
            }
            const int64_t deltaQpc = scheduledQpcForTick - liveStartQpc.QuadPart;
            if (deltaQpc < 0) {
                return -1;
            }
            return (deltaQpc * 1000000) / qpcFreq.QuadPart;
        };

        QueuedFrame frame;
        bool popped = false;
        bool wgcTelemetryTickArmed = false;
        uint32_t wgcBufferedAtTickStart = 0;
        bool wgcFreshAvailableAtTickStart = false;
        bool wgcReserveAvailableAtTickStart = false;
        const uint32_t outputFps = std::max<uint32_t>(1u, static_cast<uint32_t>(config.video.fps));
        auto tryPopBufferedWgcFrameForTarget = [&](int64_t selectionTargetQpc, bool allowPolicyHold,
                                                   bool reserveAvailableAtTickStartForHold,
                                                   uint32_t policyOutputShortfallTicks, QueuedFrame* selectedFrame,
                                                   bool* heldByPolicyOut) {
            if (heldByPolicyOut) {
                *heldByPolicyOut = false;
            }
            if (!selectedFrame || bufferedWgcFrames.empty()) {
                return false;
            }

            const size_t availableCount = bufferedWgcFrames.size();
            bool skipWgcPopThisTick = false;
            size_t bestIdx = 0;
            if (selectionTargetQpc > 0 && targetIntervalTicks > 0) {
                const double shortfallDurationMs =
                    ce::capture_policy::GetCfrShortfallDurationMs(policyOutputShortfallTicks, frameIntervalMs);
                const double oldestBufferedFrameAgeMs = static_cast<double>(wgcOldestBufferedFrameAgeUs) / 1000.0;
                const bool wgcCoverageRepeatActive =
                    recordingOutputLive &&
                    ce::capture_policy::HasWgcUnrecoverableCoverageLoss(shortfallDurationMs, oldestBufferedFrameAgeMs);
                if (!wgcCoverageRepeatActive) {
                    wgcCoverageRepeatAccumulator = 0.0;
                }
                if (wgcCoverageRepeatActive && g_HasLastFrame && !g_LastFrame.isInjectMode) {
                    double coverageRepeatRatio =
                        ce::capture_policy::ComputeWgcCoverageLossRepeatRatio(shortfallDurationMs, oldestBufferedFrameAgeMs);
                    if (g_IsEncoderBottlenecked.load(std::memory_order_relaxed)) {
                        coverageRepeatRatio = std::min(coverageRepeatRatio * 1.5, 0.5);
                    }
                    wgcCoverageRepeatAccumulator += coverageRepeatRatio;
                    if (wgcCoverageRepeatAccumulator >= 1.0) {
                        wgcCoverageRepeatAccumulator -= 1.0;
                        frameCreditAccumulator = std::min(frameCreditAccumulator, 1.0);
                        skipWgcPopThisTick = true;
                        if (heldByPolicyOut) {
                            *heldByPolicyOut = true;
                        }
                        ++wgcCoverageRepeatHoldCount;
                    }
                }
                if (skipWgcPopThisTick) {
                    return false;
                }

                const int64_t minFreshTimestampQpc = ce::capture_policy::GetWgcMinimumFreshTimestampQpc(
                    lastEmittedWgcSourceQpc, selectionTargetQpc, targetIntervalTicks, wgcLowSourceModeActive);
                const int64_t staleUniqueFallbackMinTimestampQpc =
                    ce::capture_policy::GetWgcStaleUniqueFallbackMinTimestampQpc(
                        lastEmittedWgcSourceQpc, selectionTargetQpc, targetIntervalTicks, wgcLowSourceModeActive);
                const auto isFreshWgcCandidate = [&](const QueuedFrame& candidate) {
                    return candidate.timestamp > 0 &&
                           ce::capture_policy::IsWgcTimestampFreshEnough(candidate.timestamp, minFreshTimestampQpc) &&
                           !candidate.duplicateSourceTimestamp;
                };
                bestIdx = SelectFrameClosestToTimestampIf(bufferedWgcFrames, availableCount, selectionTargetQpc,
                                                          isFreshWgcCandidate);
                if (bestIdx < availableCount) {
                    const size_t previousFreshIdx =
                        FindPreviousFrameIndexIf(bufferedWgcFrames, bestIdx, isFreshWgcCandidate);
                    if (previousFreshIdx < availableCount &&
                        ce::capture_policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(
                            bufferedWgcFrames[previousFreshIdx].timestamp, bufferedWgcFrames[bestIdx].timestamp,
                            selectionTargetQpc, targetIntervalTicks, wgcReservePressureActive,
                            wgcLowSourceModeActive)) {
                        bestIdx = previousFreshIdx;
                    }
                } else {
                    ++wgcFreshSelectionMissCount;
                    const size_t staleUniqueIdx = SelectFrameClosestToTimestampIf(
                        bufferedWgcFrames, availableCount, selectionTargetQpc, [&](const QueuedFrame& candidate) {
                            return candidate.timestamp > 0 && candidate.timestamp > lastEmittedWgcSourceQpc &&
                                   !candidate.duplicateSourceTimestamp &&
                                   candidate.timestamp >= staleUniqueFallbackMinTimestampQpc;
                        });
                    if (staleUniqueIdx < availableCount) {
                        bestIdx = staleUniqueIdx;
                        ++wgcStaleUniqueFallbackCount;
                        ++wgcAncientSelectionCount;
                    } else if (g_HasLastFrame && !g_LastFrame.isInjectMode) {
                        frameCreditAccumulator = std::min(frameCreditAccumulator, 1.0);
                        skipWgcPopThisTick = true;
                    } else {
                        bestIdx = 0;
                    }
                }

                if (!skipWgcPopThisTick && allowPolicyHold && bestIdx + 1 == availableCount && g_HasLastFrame &&
                    !g_LastFrame.isInjectMode && encoderLateTickCount == 0) {
                    const bool shouldHold = ce::capture_policy::ShouldHoldSingleFreshWgcFrame(
                        wgcReservePressureActive, wgcLowSourceModeActive, wgcRecentInputMin250Fps, outputFps,
                        smoothedInputPerTick, policyOutputShortfallTicks,
                        g_IsEncoderBottlenecked.load(std::memory_order_relaxed), reserveAvailableAtTickStartForHold);
                    const int64_t holdSlackQpc = std::max<int64_t>(targetIntervalTicks / 8, 1);
                    if (shouldHold &&
                        ShouldHoldFrameForNextTick(bufferedWgcFrames[bestIdx].timestamp, selectionTargetQpc,
                                                   targetIntervalTicks, holdSlackQpc)) {
                        frameCreditAccumulator = std::min(frameCreditAccumulator, 1.0);
                        skipWgcPopThisTick = true;
                        if (heldByPolicyOut) {
                            *heldByPolicyOut = true;
                        }
                        ++wgcHoldForNextTickCount;
                        ++wgcHeldFreshFrameTickCount;
                    }
                }
            }

            if (skipWgcPopThisTick) {
                return false;
            }

            if (bestIdx >= bufferedWgcFrames.size()) {
                bestIdx = 0;
            }
            for (size_t i = 0; i < bestIdx; ++i) {
                QueuedFrame stale = std::move(bufferedWgcFrames.front());
                bufferedWgcFrames.pop_front();
                ReleaseQueuedFrameTexture(stale);
                ++wgcDropObsoleteCount;
            }

            const bool spentBufferedReserve = frameCreditAccumulator < 1.0;
            *selectedFrame = std::move(bufferedWgcFrames.front());
            bufferedWgcFrames.pop_front();
            if (selectedFrame->duplicateSourceTimestamp) {
                ++wgcSelectDuplicateSourceCount;
            } else {
                ++wgcSelectFreshCount;
            }
            if (spentBufferedReserve) {
                frameCreditAccumulator = 0.0;
                ++wgcReserveSpendTickCount;
            } else {
                frameCreditAccumulator -= 1.0;
            }

            return true;
        };

        if (IsActiveScreenGrab()) {
            if (!bufferedInjectFrames.empty()) {
                ClearBufferedInjectFrames();
            }
            smoothedInjectFenceMs = 0.0;
            if (!config.video.useVFR) {
                // CFR WGC: buffer source-cadence frames, maintain a shallow pool,
                // then select the frame closest to the encoder output grid.
                drainedScreenGrabFrames.clear();
                QueuedFrame temp;
                while (g_FrameQueue.Pop(temp, 0)) {
                    if (g_RejectInjectFrames.load(std::memory_order_acquire) && temp.isInjectMode) {
                        DiscardQueuedFrame(temp);
                        continue;
                    }
                    drainedScreenGrabFrames.push_back(std::move(temp));
                }

                const bool sampleWgcCadenceTick = !(recordingOutputLive && encoderLateTickCount > 0);

                // Phase 1: Feed raw timestamps to predictor BEFORE moving
                // frames to the buffer (std::move invalidates source object).
                // Always feed the predictor (even when encoder is late) so it
                // can calibrate the source FPS for Bresenham pacing and logging.
                if (!drainedScreenGrabFrames.empty()) {
                    for (auto& drainedFrame : drainedScreenGrabFrames) {
                        if (!drainedFrame.isInjectMode && drainedFrame.timestamp > 0) {
                            wgcInputPredictor.Update(drainedFrame.timestamp, qpcFreq.QuadPart);
                            static int64_t s_lastWgcSrcQpc = 0;
                            if (drainedFrame.timestamp == s_lastWgcSrcQpc) {
                                ++dupTimestampCount;
                            }
                            s_lastWgcSrcQpc = drainedFrame.timestamp;
                        }
                    }
                }

                // Phase 2: Move drained frames to the main buffer.
                for (auto& drainedFrame : drainedScreenGrabFrames) {
                    bufferedWgcFrames.push_back(std::move(drainedFrame));
                }

                // Track frame arrival rate for pacing telemetry only.
                if (sampleWgcCadenceTick) {
                    pacingInputThisWindow += static_cast<uint32_t>(drainedScreenGrabFrames.size());
                    pacingTicksThisWindow++;
                    const uint32_t wgcPacingWindowSize = (pacingEmaUpdates < 6)
                                                             ? std::max((uint32_t)config.video.fps / 8, 8u)
                                                             : (uint32_t)config.video.fps / 2;
                    if (pacingTicksThisWindow >= wgcPacingWindowSize) {
                        double measuredRate = (double)pacingInputThisWindow / (double)pacingTicksThisWindow;
                        // Adaptive alpha: fast convergence during startup, burst detection, steady-state
                        double alpha = 0.5;
                        if (pacingEmaUpdates < 6) {
                            alpha = 0.7;
                        } else if (smoothedInputPerTick > 0.01) {
                            double deviation = std::abs(measuredRate - smoothedInputPerTick) / smoothedInputPerTick;
                            if (deviation > 0.20) {
                                alpha = 0.8;
                            }
                        }
                        smoothedInputPerTick = smoothedInputPerTick * (1.0 - alpha) + measuredRate * alpha;
                        pacingInputThisWindow = 0;
                        pacingTicksThisWindow = 0;
                        ++pacingEmaUpdates;
                    }
                }

                if (g_WgcCap) {
                    wgcRecentDeliveredFps = g_WgcCap->GetDeliveredRatePerSec();
                    wgcRecentDeliveredMin250Fps = g_WgcCap->GetDeliveredMin250Fps();
                    wgcRecentDeliveredMin500Fps = g_WgcCap->GetDeliveredMin500Fps();
                    wgcRecentInputMin250Fps = g_WgcCap->GetInputMin250Fps();
                    wgcRecentInputMin500Fps = g_WgcCap->GetInputMin500Fps();
                }

                const bool wgcSourceMarginal = smoothedInputPerTick < 0.995 ||
                                               wgcRecentDeliveredMin250Fps < outputFps ||
                                               wgcRecentDeliveredMin500Fps < outputFps || wgcNoFreshTickPermille >= 40;
                wgcReservePressureActive =
                    wgcSourceMarginal && ce::capture_policy::IsWgcReservePressureActive(
                                             wgcNoReserveTickCount, wgcQueueTickSampleCount, outputFps);

                ce::capture_policy::WgcAdaptiveTelemetry sourceTelemetry{};
                sourceTelemetry.outputFps = outputFps;
                sourceTelemetry.recentDeliveredFps = wgcRecentDeliveredFps;
                sourceTelemetry.recentDeliveredMin250Fps = wgcRecentDeliveredMin250Fps;
                sourceTelemetry.recentDeliveredMin500Fps = wgcRecentDeliveredMin500Fps;
                sourceTelemetry.recentInputMin250Fps = wgcRecentInputMin250Fps;
                sourceTelemetry.recentInputMin500Fps = wgcRecentInputMin500Fps;
                sourceTelemetry.averageJitterUs = SaturatingToUint32(g_WgcCap ? g_WgcCap->GetSourceJitterAvgUs() : 0);
                sourceTelemetry.emptyTickPermille = wgcNoFreshTickPermille;
                sourceTelemetry.bufferedWgcFrames = static_cast<uint32_t>(bufferedWgcFrames.size());
                sourceTelemetry.encoderQueueDepth = static_cast<uint32_t>(g_FrameQueue.Size());
                sourceTelemetry.duplicateRatio = (cadenceCounters.liveTickEmitCount > 0)
                                                     ? static_cast<double>(cadenceCounters.liveTickDuplicateCount) /
                                                           static_cast<double>(cadenceCounters.liveTickEmitCount)
                                                     : 0.0;

                const uint64_t lowSourceNowTick = GetTickCount64();
                const bool wantsLowSourceMode = ce::capture_policy::ShouldEnterWgcLowSourceMode(sourceTelemetry);
                if (!wgcLowSourceModeActive) {
                    if (wantsLowSourceMode) {
                        if (wgcLowSourceStateChangeTick == 0) {
                            wgcLowSourceStateChangeTick = lowSourceNowTick;
                        } else if ((lowSourceNowTick - wgcLowSourceStateChangeTick) >=
                                   ce::capture_policy::kWgcLowSourceEnterHoldMs) {
                            wgcLowSourceModeActive = true;
                            wgcLowSourceStateChangeTick = lowSourceNowTick;
                        }
                    } else {
                        wgcLowSourceStateChangeTick = 0;
                    }
                } else {
                    const bool shouldExitLowSourceMode =
                        ce::capture_policy::ShouldExitWgcLowSourceMode(sourceTelemetry);
                    if (shouldExitLowSourceMode) {
                        if ((lowSourceNowTick - wgcLowSourceStateChangeTick) >=
                            ce::capture_policy::kWgcLowSourceExitHoldMs) {
                            wgcLowSourceModeActive = false;
                            wgcLowSourceStateChangeTick = lowSourceNowTick;
                        }
                    } else {
                        wgcLowSourceStateChangeTick = lowSourceNowTick;
                    }
                }

                if (g_WgcCap && recordingOutputLive && g_Recording && targetIntervalTicks > 0) {
                    const uint32_t currentTargetFps = g_WgcCap->GetTargetFps();
                    uint32_t desiredTargetFps = 0;
                    const double duplicateRatio = (cadenceCounters.liveTickEmitCount > 0)
                                                      ? static_cast<double>(cadenceCounters.liveTickDuplicateCount) /
                                                            static_cast<double>(cadenceCounters.liveTickEmitCount)
                                                      : 0.0;
                    const uint32_t averageJitterUs = SaturatingToUint32(g_WgcCap->GetSourceJitterAvgUs());
                    wgcRecentDeliveredFps = g_WgcCap->GetDeliveredRatePerSec();
                    wgcRecentDeliveredMin250Fps = g_WgcCap->GetDeliveredMin250Fps();
                    wgcRecentDeliveredMin500Fps = g_WgcCap->GetDeliveredMin500Fps();
                    wgcRecentInputMin250Fps = g_WgcCap->GetInputMin250Fps();
                    wgcRecentInputMin500Fps = g_WgcCap->GetInputMin500Fps();
                    (void)averageJitterUs;
                    (void)duplicateRatio;

                    if (wgcAdaptiveThrottleMode != WgcAdaptiveThrottleMode::kOff) {
                        wgcAdaptiveThrottleMode = WgcAdaptiveThrottleMode::kOff;
                    }

                    if (desiredTargetFps != currentTargetFps) {
                        g_WgcCap->SetTargetFps(desiredTargetFps);
                        g_WgcAdaptiveTargetFps.store(desiredTargetFps, std::memory_order_relaxed);
                        ++wgcAdaptiveThrottleAdjustments;
                    }
                }

                // Trim excess (WGC textures are COM-refcounted; keep buffer shallow)
                size_t maxBufferedWgcFrames = 8;
                if (recordingOutputLive) {
                    if (wgcLowSourceModeActive || wgcReservePressureActive) {
                        maxBufferedWgcFrames = 10;
                    } else if (smoothedInputPerTick < 1.01) {
                        maxBufferedWgcFrames = 10;
                    } else if (smoothedInputPerTick < 1.05) {
                        maxBufferedWgcFrames = 9;
                    }
                    if (encoderLateTickCount > 0) {
                        maxBufferedWgcFrames =
                            std::max<size_t>(maxBufferedWgcFrames,
                                             std::min<size_t>(static_cast<size_t>(encoderLateTickCount) + 4u, 12u));
                    }
                if (outputShortfallTicks > 0) {
                        maxBufferedWgcFrames = std::max<size_t>(maxBufferedWgcFrames, 12u);
                    }
                    if (wgcCoverageDelayTicksCurrent > 0) {
                        maxBufferedWgcFrames =
                            std::max<size_t>(maxBufferedWgcFrames,
                                             std::min<size_t>(static_cast<size_t>(wgcCoverageDelayTicksCurrent) + 4u, 36u));
                    }
                }
                while (bufferedWgcFrames.size() > maxBufferedWgcFrames) {
                    QueuedFrame stale = std::move(bufferedWgcFrames.front());
                    bufferedWgcFrames.pop_front();
                    ReleaseQueuedFrameTexture(stale);
                }

                const double creditPerTick = smoothedInputPerTick;
                frameCreditAccumulator += creditPerTick;
                // Prevent unbounded credit accumulation when game FPS > encoder FPS
                frameCreditAccumulator = std::min(frameCreditAccumulator, 5.0);

                // Discard excess WGC frames only under clear overcapture pressure.
                while (outputShortfallTicks == 0 && encoderLateTickCount == 0 && !wgcLowSourceModeActive &&
                       !wgcReservePressureActive && frameCreditAccumulator >= 2.0 && bufferedWgcFrames.size() > 1) {
                    QueuedFrame excess = std::move(bufferedWgcFrames.front());
                    bufferedWgcFrames.pop_front();
                    ReleaseQueuedFrameTexture(excess);
                    frameCreditAccumulator -= 1.0;
                }

                const bool scheduledWgcTelemetryTick =
                    !config.video.useVFR && g_EncoderRunning && g_Recording && recordingOutputLive;
                wgcCoverageDelayTicksCurrent = 0;
                if (scheduledWgcTelemetryTick) {
                    wgcTelemetryTickArmed = true;
                    wgcBufferedAtTickStart = static_cast<uint32_t>(bufferedWgcFrames.size());
                    wgcReserveAvailableAtTickStart = false;

                    if (!bufferedWgcFrames.empty()) {
                        const bool applyDelayedWgcSelection = ce::capture_policy::ShouldApplyWgcSelectionDelay(
                            recordingOutputLive, outputShortfallTicks,
                            g_IsEncoderBottlenecked.load(std::memory_order_relaxed), bufferedWgcFrames.size() > 1);
                        const int64_t selectionTargetQpc = computeWgcSelectionTargetQpc(applyDelayedWgcSelection);
                        const int64_t minFreshTimestampQpc = ce::capture_policy::GetWgcMinimumFreshTimestampQpc(
                            lastEmittedWgcSourceQpc, selectionTargetQpc, targetIntervalTicks, wgcLowSourceModeActive);
                        const size_t freshCandidateIdx =
                            SelectFrameClosestToTimestampIf(bufferedWgcFrames, bufferedWgcFrames.size(),
                                                            selectionTargetQpc, [&](const QueuedFrame& candidate) {
                                                                return ce::capture_policy::IsWgcTimestampFreshEnough(
                                                                           candidate.timestamp, minFreshTimestampQpc) &&
                                                                       !candidate.duplicateSourceTimestamp;
                                                            });
                        wgcFreshAvailableAtTickStart = freshCandidateIdx < bufferedWgcFrames.size();
                        if (wgcFreshAvailableAtTickStart) {
                            const size_t previousFreshIdx = FindPreviousFrameIndexIf(
                                bufferedWgcFrames, freshCandidateIdx, [&](const QueuedFrame& candidate) {
                                    return ce::capture_policy::IsWgcTimestampFreshEnough(candidate.timestamp,
                                                                                         minFreshTimestampQpc) &&
                                           !candidate.duplicateSourceTimestamp;
                                });
                            wgcReserveAvailableAtTickStart = previousFreshIdx < bufferedWgcFrames.size();
                        }
                    }
                }

                if (!g_EncoderRunning && !bufferedWgcFrames.empty()) {
                    frame = std::move(bufferedWgcFrames.front());
                    bufferedWgcFrames.pop_front();
                    popped = true;
                    frameCreditAccumulator = std::min(frameCreditAccumulator, 1.0);
                } else if (!bufferedWgcFrames.empty()) {
                    bool heldByPolicy = false;
                    const bool applyDelayedWgcSelection = ce::capture_policy::ShouldApplyWgcSelectionDelay(
                        recordingOutputLive, outputShortfallTicks,
                        g_IsEncoderBottlenecked.load(std::memory_order_relaxed), wgcReserveAvailableAtTickStart);
                    const int64_t selectionTargetQpc = (encoderGridStartQpc > 0 && targetIntervalTicks > 0)
                                                           ? computeWgcSelectionTargetQpc(applyDelayedWgcSelection)
                                                           : 0;
                    if (tryPopBufferedWgcFrameForTarget(selectionTargetQpc, true, wgcReserveAvailableAtTickStart,
                                                        outputShortfallTicks, &frame, &heldByPolicy)) {
                        popped = true;
                    } else if (heldByPolicy) {
                        ++wgcRepeatPolicyHoldCount;
                    }
                }
            } else {
                // VFR: keep the existing lowest-latency newest-frame sampling.
                QueuedFrame temp;
                while (g_FrameQueue.Pop(temp, 0)) {
                    if (g_RejectInjectFrames.load(std::memory_order_acquire) && temp.isInjectMode) {
                        DiscardQueuedFrame(temp);
                        continue;
                    }
                    if (popped && !frame.isInjectMode && frame.texture) {
                        frame.texture->Release();
                    }
                    frame = std::move(temp);
                    popped = true;
                }
            }
        } else {
            if (!bufferedWgcFrames.empty()) {
                ClearBufferedWgcFrames();
            }
            if (g_RejectInjectFrames.load(std::memory_order_acquire) && !bufferedInjectFrames.empty()) {
                ClearBufferedInjectFrames();
            }

            if (!config.video.useVFR) {
                // Keep multiple inject frames in reserve so the encoder usually works on
                // textures whose GPU copy has already completed instead of blocking on the
                // newest frame's fence.
                drainedInjectFrames.clear();
                QueuedFrame temp;
                while (g_FrameQueue.Pop(temp, 0)) {
                    if (g_RejectInjectFrames.load(std::memory_order_acquire) && temp.isInjectMode) {
                        DiscardQueuedFrame(temp);
                        continue;
                    }
                    drainedInjectFrames.push_back(std::move(temp));
                }

                for (auto& drainedFrame : drainedInjectFrames) {
                    bufferedInjectFrames.push_back(std::move(drainedFrame));
                }

                // Track frame arrival rate for Bresenham pacing.  Use a short
                // window during warmup/startup so the EMA is already calibrated
                // when recording goes live, then widen to half-second for
                // steady-state stability.
                pacingInputThisWindow += (uint32_t)drainedInjectFrames.size();
                pacingTicksThisWindow++;
                const uint32_t pacingWindowSize = (pacingEmaUpdates < 6) ? std::max((uint32_t)config.video.fps / 8, 8u)
                                                                         : (uint32_t)config.video.fps / 2;
                if (pacingTicksThisWindow >= pacingWindowSize) {
                    double measuredRate = (double)pacingInputThisWindow / (double)pacingTicksThisWindow;
                    // Adaptive alpha: converge fast during startup (0.7), steady-state (0.5),
                    // or when FPS transitions detected (>20% deviation → 0.8) to prevent
                    // Bresenham mis-pacing during rapid FPS changes.
                    double alpha = 0.5;
                    if (pacingEmaUpdates < 6) {
                        alpha = 0.7;
                    } else if (smoothedInputPerTick > 0.01) {
                        double deviation = std::abs(measuredRate - smoothedInputPerTick) / smoothedInputPerTick;
                        if (deviation > 0.20) {
                            alpha = 0.8;
                        }
                    }
                    smoothedInputPerTick = smoothedInputPerTick * (1.0 - alpha) + measuredRate * alpha;
                    pacingInputThisWindow = 0;
                    pacingTicksThisWindow = 0;
                    ++pacingEmaUpdates;
                }

                const size_t injectReserveFrames = ce::capture_policy::GetInjectReserveFrames(
                    config.video.useVFR, smoothedInjectFenceMs, frameIntervalMs);
                const size_t maxBufferedInjectFrames = ce::capture_policy::GetMaxBufferedInjectFrames(
                    injectReserveFrames, recordingOutputLive, recordingLiveTick, GetTickCount64());
                maxBufferedInjectDepthSinceLog = std::max(maxBufferedInjectDepthSinceLog, bufferedInjectFrames.size());
                uint32_t trimmedInjectFrames = 0;
                while (bufferedInjectFrames.size() > maxBufferedInjectFrames) {
                    QueuedFrame staleFrame = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    DiscardQueuedFrame(staleFrame);
                    ++trimmedInjectFrames;
                }
                if (trimmedInjectFrames > 0) {
                    pendingInjectTrimmedLogCount += trimmedInjectFrames;
                    g_InjectBufferedTrimmedFrames.fetch_add(trimmedInjectFrames, std::memory_order_relaxed);
                    if (g_pSharedMem) {
                        g_pSharedMem->runtimeState.injectTrimmedFrames.fetch_add(trimmedInjectFrames,
                                                                                 std::memory_order_relaxed);
                    }
                    if (lastDeferredLineage.IsValid() && !bufferedInjectFrames.empty() &&
                        !std::any_of(bufferedInjectFrames.begin(), bufferedInjectFrames.end(),
                                     [&](const QueuedFrame& candidate) {
                                         return MatchesInjectFrameLineage(candidate, lastDeferredLineage);
                                     })) {
                        lastDeferredLineage = {};
                    }
                }
                DWORD now = GetTickCount();
                if (pendingInjectTrimmedLogCount > 0 && now - lastInjectTrimLog >= 1000) {
                    LogInfo(
                        "[EncoderThread] Trimmed %u stale inject frame(s) to cap backlog (peak=%zu cap=%zu "
                        "reserve=%zu)",
                        pendingInjectTrimmedLogCount, maxBufferedInjectDepthSinceLog, maxBufferedInjectFrames,
                        injectReserveFrames);
                    pendingInjectTrimmedLogCount = 0;
                    maxBufferedInjectDepthSinceLog = bufferedInjectFrames.size();
                    lastInjectTrimLog = now;
                }
                size_t minBufferedInjectFrames =
                    ce::capture_policy::GetMinBufferedInjectFrames(injectReserveFrames, recordingOutputLive);

                // Bresenham credit-based pacing: add credit proportional to the
                // measured game-frame arrival rate.  When game fps >= target,
                // credit reaches 1.0 every tick → pop a unique frame.  When
                // game fps < target, credit occasionally stays below 1.0 → a
                // scheduled duplicate distributed evenly.  When game fps >
                // target (overcapture), credit exceeds 2.0 → skip excess frames
                // with Bresenham-even distribution for smoothest decimation.
                double creditPerTick = smoothedInputPerTick;
                frameCreditAccumulator += creditPerTick;
                // Prevent unbounded credit accumulation when game FPS > encoder FPS
                frameCreditAccumulator = std::min(frameCreditAccumulator, 5.0);

                // Discard excess frames when overcapturing (Bresenham skip).
                // Each discarded frame represents one "step" in the Bresenham
                // line from input rate to output rate, distributing the skips
                // as evenly as possible across the output timeline.
                while (frameCreditAccumulator >= 2.0 && bufferedInjectFrames.size() > minBufferedInjectFrames + 1) {
                    QueuedFrame excess = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    DiscardQueuedFrame(excess);
                    frameCreditAccumulator -= 1.0;
                    g_InjectCadenceDroppedFrames.fetch_add(1, std::memory_order_relaxed);
                    if (g_pSharedMem) {
                        g_pSharedMem->runtimeState.injectCadenceDrops.fetch_add(1, std::memory_order_relaxed);
                    }
                }

                if (!g_EncoderRunning && !bufferedInjectFrames.empty()) {
                    frame = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    popped = true;
                    frameCreditAccumulator = std::min(frameCreditAccumulator, 1.0);
                    lastDeferredLineage = {};
                } else if (frameCreditAccumulator >= 1.0 && bufferedInjectFrames.size() > minBufferedInjectFrames) {
                    // Timestamp-aware selection: when multiple frames are
                    // available, pick the one closest to the ideal output grid
                    // time.  This reduces temporal error and smooths the motion
                    // cadence compared to always taking the oldest frame (FIFO).
                    size_t availableCount = bufferedInjectFrames.size() - minBufferedInjectFrames;
                    if (availableCount > 1 && encoderGridStartQpc > 0) {
                        auto isAllowedCandidate = [&](const QueuedFrame& candidate) {
                            return !MatchesInjectFrameLineage(candidate, lastDeferredLineage);
                        };
                        size_t bestIdx =
                            SelectFrameClosestToGridIf(bufferedInjectFrames, availableCount, encoderGridStartQpc,
                                                       selectionGridTick, targetIntervalTicks, isAllowedCandidate);
                        bool usedDeferredFallback = false;
                        if (bestIdx >= availableCount) {
                            bestIdx =
                                SelectFrameClosestToGrid(bufferedInjectFrames, availableCount, encoderGridStartQpc,
                                                         selectionGridTick, targetIntervalTicks);
                            usedDeferredFallback = lastDeferredLineage.IsValid();
                        }
                        if (bestIdx > 0) {
                            selectionLogCounter++;
                            if (selectionLogCounter <= 12 || (selectionLogCounter % 240) == 0) {
                                const int64_t idealQpc =
                                    ComputeIdealOutputQpc(encoderGridStartQpc, selectionGridTick, targetIntervalTicks);
                                LogInfo(
                                    "[EncoderThread] Select tick=%lld idealQpc=%lld chose idx=%zu frame=%u tex=%d "
                                    "ts=%lld oldest=%lld "
                                    "avail=%zu reserve=%zu credit=%.3f%s",
                                    static_cast<long long>(selectionGridTick), static_cast<long long>(idealQpc),
                                    bestIdx, bufferedInjectFrames[bestIdx].frameIndex,
                                    bufferedInjectFrames[bestIdx].textureIndex,
                                    static_cast<long long>(bufferedInjectFrames[bestIdx].timestamp),
                                    static_cast<long long>(bufferedInjectFrames.front().timestamp), availableCount,
                                    minBufferedInjectFrames, frameCreditAccumulator,
                                    usedDeferredFallback ? " fallback=deferred-only" : "");
                            }
                        }
                        // Discard all frames older than the selected one.
                        for (size_t i = 0; i < bestIdx; i++) {
                            QueuedFrame stale = std::move(bufferedInjectFrames.front());
                            bufferedInjectFrames.pop_front();
                            DiscardQueuedFrame(stale);
                            g_InjectCadenceDroppedFrames.fetch_add(1, std::memory_order_relaxed);
                            if (g_pSharedMem) {
                                g_pSharedMem->runtimeState.injectCadenceDrops.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    } else if (availableCount > 0 && lastDeferredLineage.IsValid() &&
                               MatchesInjectFrameLineage(bufferedInjectFrames.front(), lastDeferredLineage)) {
                        bool foundAlternate = false;
                        for (size_t i = 1; i < bufferedInjectFrames.size(); ++i) {
                            if (!MatchesInjectFrameLineage(bufferedInjectFrames[i], lastDeferredLineage)) {
                                for (size_t j = 0; j < i; ++j) {
                                    QueuedFrame stale = std::move(bufferedInjectFrames.front());
                                    bufferedInjectFrames.pop_front();
                                    bufferedInjectFrames.push_back(std::move(stale));
                                }
                                foundAlternate = true;
                                LogInfo(
                                    "[EncoderThread] Deferred-lineage bypass moved frame=%u tex=%d behind %zu "
                                    "candidate(s)",
                                    lastDeferredLineage.frameIndex, lastDeferredLineage.textureIndex, i);
                                break;
                            }
                        }
                        if (!foundAlternate) {
                            lastDeferredLineage = {};
                        }
                    }
                    frame = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    popped = true;
                    frameCreditAccumulator -= 1.0;
                    lastDeferredLineage = {};
                } else if (bufferedInjectFrames.size() > injectReserveFrames + 6) {
                    // Buffer pressure: pop to prevent unnecessary trimming
                    frame = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    popped = true;
                    frameCreditAccumulator = std::fmod(frameCreditAccumulator, 1.0);
                    lastDeferredLineage = {};
                }
            } else {
                // VFR: keep the existing newest-frame sampling for the lowest latency.
                QueuedFrame temp;
                while (g_FrameQueue.Pop(temp, 0)) {
                    if (g_RejectInjectFrames.load(std::memory_order_acquire) && temp.isInjectMode) {
                        DiscardQueuedFrame(temp);
                        continue;
                    }
                    if (popped && !frame.isInjectMode && frame.texture) {
                        frame.texture->Release();
                    }
                    frame = std::move(temp);
                    popped = true;
                }
            }
        }

        QueuedFrame* frameToProcess = nullptr;
        bool isDuplicate = false;
        bool duplicateFromDeferred = false;
        bool duplicateFromTimerRebase = false;
        bool wantsTrueRepeatLastFrame = false;

        if (popped && frame.isInjectMode && g_RejectInjectFrames.load(std::memory_order_acquire)) {
            DiscardQueuedFrame(frame);
            popped = false;
        }

        if (g_HasLastFrame && g_LastFrame.isInjectMode && g_RejectInjectFrames.load(std::memory_order_acquire)) {
            g_LastFrame = QueuedFrame{};
            g_HasLastFrame = false;
        }

        const bool useScreenGrab = IsActiveScreenGrab();
        const bool hasRepeatLastFramePath =
            !config.video.useVFR &&
            ((useScreenGrab && MediaEngine_RepeatLastFrameWithTimeline) || MediaEngine_RepeatLastFrame);
        auto repeatLastFrameForScheduledQpc = [&](int64_t scheduledQpc) {
            if (useScreenGrab && !config.video.useVFR && MediaEngine_RepeatLastFrameWithTimeline) {
                return MediaEngine_RepeatLastFrameWithTimeline(scheduledQpc, computeLiveTimelineElapsedUs(scheduledQpc));
            }
            return MediaEngine_RepeatLastFrame && MediaEngine_RepeatLastFrame(scheduledQpc);
        };
        const bool warmupCaptureModeChanged = ce::capture_policy::ResetWarmupOnCaptureModeChange(
            recordingOutputLive, useScreenGrab, GetTickCount64(), warmupState);
        if (warmupCaptureModeChanged || !useScreenGrab) {
            ResetWarmupWgcFreshness();
            wgcLowSourceModeActive = false;
            wgcLowSourceStateChangeTick = 0;
        }
        startupWarmupStartTick = warmupState.startupWarmupStartTick;
        hiddenStartupFrames = warmupState.hiddenStartupFrames;
        const size_t injectReserveFrames = (!useScreenGrab)
                                               ? ce::capture_policy::GetInjectReserveFrames(
                                                     config.video.useVFR, smoothedInjectFenceMs, frameIntervalMs)
                                               : 0;
        if (!recordingOutputLive && !pendingLiveActivation && g_Recording && g_EncoderRunning) {
            const uint64_t warmupElapsedMs64 = GetTickCount64() - startupWarmupStartTick;
            const DWORD warmupElapsedMs =
                warmupElapsedMs64 > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<DWORD>(warmupElapsedMs64);
            const bool warmupReady = useScreenGrab
                                         ? ce::capture_policy::ShouldCommitWgcWarmup(
                                               popped, bufferedWgcFrames.size(), warmupElapsedMs,
                                               smoothedInputPerTick * static_cast<double>(config.video.fps),
                                               static_cast<uint32_t>(config.video.fps))
                                         : ce::capture_policy::ShouldCommitRecordingWarmup(
                                               useScreenGrab, config.video.useVFR, popped, !bufferedWgcFrames.empty(),
                                               bufferedInjectFrames.size(), injectReserveFrames, warmupElapsedMs);
            const bool warmupFreshEnough =
                !useScreenGrab || wgcFreshWarmupFrameCount >= ce::capture_policy::kWgcWarmupFreshFrames;
            if (warmupReady && warmupFreshEnough) {
                pendingLiveActivation = true;
                // Reset Bresenham credit for a clean start; keep smoothedInputPerTick
                // so the EMA calibration from warmup carries over.
                frameCreditAccumulator = 0.0;
                selectionLogCounter = 0;
                pacingInputThisWindow = 0;
                pacingTicksThisWindow = 0;
                encoderGridStartQpc = 0;
                encoderGridTickCount = 0;
                liveTicksOutput = 0;
                liveTicksScheduled = 0;
                liveTicksDiscardedByTimerRebase = 0;
                liveStartQpc = {};
                wgcInputPredictor.Reset();
                smoothedEncCycleMs = 0.0;
                encCycleMaxMs = 0;
                dupTimestampCount = 0;
                wgcRecentDeliveredFps = 0;
                wgcRecentDeliveredMin250Fps = 0;
                wgcRecentDeliveredMin500Fps = 0;
                wgcRecentInputMin250Fps = 0;
                wgcRecentInputMin500Fps = 0;
                wgcNoFreshTickCount = 0;
                encodeSpikeCountThisSecond = 0;
                wgcQueueTickSampleCount = 0;
                wgcNoFreshTickPermille = 0;
                wgcBufferedAtTickSum = 0;
                wgcBufferedAtTickMin = UINT32_MAX;
                wgcNoReserveTickCount = 0;
                wgcAncientSelectionCount = 0;
                wgcFreshSelectionMissCount = 0;
                wgcStaleUniqueFallbackCount = 0;
                wgcRepeatNoFreshCount = 0;
                wgcRepeatPolicyHoldCount = 0;
                wgcCoverageRepeatHoldCount = 0;
                wgcCoverageDelayTicksCurrent = 0;
                wgcRepeatTimerLateCount = 0;
                wgcRepeatCatchupCount = 0;
                wgcSelectFreshCount = 0;
                wgcSelectDuplicateSourceCount = 0;
                wgcDropObsoleteCount = 0;
                wgcCoverageRepeatAccumulator = 0.0;
                lastEmittedWgcSourceQpc = 0;
                pendingLiveInjectReadyFrames = useScreenGrab ? 0
                                                             : ce::capture_policy::GetWarmupInjectKeepCount(
                                                                   smoothedInjectFenceMs, frameIntervalMs);
                // CRITICAL: Reset nextSampleTime after sleeping one full interval
                // so the first live tick fires at the correct cadence. During warmup,
                // nextSampleTime advances freely (frames are discarded), so it can be
                // far in the past when we transition to live. Without this, the first
                // several live ticks fire immediately because nextSampleTime is behind
                // now, creating a burst of frames at wrong spacing that causes visible
                // judder in the first second of recording.
                //
                // The sleep ensures the encoder thread cadence is established BEFORE
                // the first live encode, preventing the initial burst.
                //
                // WGC needs extra ticks because the encoder is lazily initialized at
                // first frame encode (~127ms for codec open + MKV header + VP setup).
                // Inject initializes the encoder at MediaEngine init, so only 1 tick
                // is needed for the first frame's shader/texture setup (~12ms).
                if (hTimer) {
                    LARGE_INTEGER afterLive;
                    QueryPerformanceCounter(&afterLive);
                    // Inject needs extra ticks for NVENC encode warmup (~21ms for
                    // rate control init, lookahead buffer fill).  WGC needs more
                    // because the codec itself initializes at first frame (~127ms).
                    int64_t sleepTicks = useScreenGrab ? (targetIntervalTicks * 24) : (targetIntervalTicks * 4);
                    int64_t sleep100ns = (sleepTicks * 10000000) / qpcFreq.QuadPart;
                    LARGE_INTEGER dueTime;
                    dueTime.QuadPart = -sleep100ns;
                    if (SetWaitableTimer(hTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                        WaitForSingleObject(hTimer, INFINITE);
                    }
                }
                QueryPerformanceCounter(&nextSampleTime);
                liveStartQpc = nextSampleTime;
                encoderGridStartQpc = liveStartQpc.QuadPart;
                // Flush stale warmup frames from ALL buffers.  During the gap
                // between capture start and encoder readiness, frames accumulate
                // in the shmem ring → g_FrameQueue → bufferedInjectFrames.
                // Without flushing, they burst into the encoder all at once,
                // causing trimming and duplicate-induced judder in the first
                // second of recording.
                {
                    QueuedFrame qf;
                    size_t queueFlushed = 0;
                    while (g_FrameQueue.Pop(qf, 0)) {
                        if (qf.isInjectMode)
                            DiscardQueuedFrame(qf);
                        else if (qf.texture)
                            ReleaseQueuedFrameTexture(qf);
                        queueFlushed++;
                    }
                    if (queueFlushed > 0) {
                        LogInfo("[EncoderThread] Flushed %zu warmup frames from queue", queueFlushed);
                    }
                }
                if (!bufferedInjectFrames.empty()) {
                    // Keep reserve+1 frames instead of just 1 so the fence EMA
                    // has enough lead time immediately after warmup flush.
                    size_t keepCount =
                        ce::capture_policy::GetWarmupInjectKeepCount(smoothedInjectFenceMs, frameIntervalMs);
                    size_t flushed = 0;
                    while (bufferedInjectFrames.size() > keepCount) {
                        QueuedFrame stale = std::move(bufferedInjectFrames.front());
                        bufferedInjectFrames.pop_front();
                        DiscardQueuedFrame(stale);
                        flushed++;
                    }
                    if (flushed > 0) {
                        LogInfo("[EncoderThread] Flushed %zu stale warmup inject frames (keep=%zu)", flushed,
                                keepCount);
                    }
                }
                if (!bufferedWgcFrames.empty()) {
                    size_t flushed = bufferedWgcFrames.size();
                    ClearBufferedWgcFrames();
                    LogInfo("[EncoderThread] Flushed %zu warmup WGC frames before live handoff", flushed);
                }
                // Reset counters so per-second logs start clean at going-live.
                g_InjectBufferedTrimmedFrames.store(0, std::memory_order_relaxed);
                g_InjectCadenceDroppedFrames.store(0, std::memory_order_relaxed);
                LogInfo(
                    "[EncoderThread] Warmup ready after %llums hidden warmup (%s, hiddenFrames=%u, inputRate=%.3f, "
                    "readyFrames=%zu freshWgc=%u)",
                    static_cast<unsigned long long>(warmupElapsedMs64), useScreenGrab ? "WGC" : "inject",
                    hiddenStartupFrames, smoothedInputPerTick, pendingLiveInjectReadyFrames, wgcFreshWarmupFrameCount);
                ResetWarmupWgcFreshness();
            }
        }

        if (pendingLiveActivation) {
            const bool liveReady = useScreenGrab || bufferedInjectFrames.size() >= pendingLiveInjectReadyFrames;
            if (!liveReady) {
                SetCapturePipelinePhase(CapturePipelinePhase::kWarmup);
                if (popped) {
                    if (useScreenGrab) {
                        TrackWarmupWgcFreshFrame(frame);
                    }
                    ++hiddenStartupFrames;
                    warmupState.hiddenStartupFrames = hiddenStartupFrames;
                    DiscardQueuedFrame(frame);
                }
                continue;
            }

            pendingLiveActivation = false;
            recordingOutputLive = true;
            SetCapturePipelinePhase(CapturePipelinePhase::kLive);
            recordingLiveTick = GetTickCount64();
            lastDeferredLineage = InjectFrameLineage{};
            ResetWarmupWgcFreshness();
            if (g_HasLastFrame && g_LastFrame.isInjectMode && !useScreenGrab) {
                g_LastFrame = QueuedFrame{};
                g_HasLastFrame = false;
            }
            SetRecordingVisibleState(true);
            LogInfo("[EncoderThread] Recording live (%s, hiddenFrames=%u, bufferedInject=%zu)",
                    useScreenGrab ? "WGC" : "inject", hiddenStartupFrames, bufferedInjectFrames.size());
        }

        if (!recordingOutputLive) {
            SetCapturePipelinePhase(CapturePipelinePhase::kWarmup);
            if (popped) {
                if (useScreenGrab) {
                    TrackWarmupWgcFreshFrame(frame);
                }
                ++hiddenStartupFrames;
                warmupState.hiddenStartupFrames = hiddenStartupFrames;
                DiscardQueuedFrame(frame);
            }
            continue;
        }

        if (popped) {
            if (!config.video.useVFR && encoderGridStartQpc == 0) {
                encoderGridStartQpc = frame.timestamp;
            }
            if (frame.isInjectMode) {
                if (g_HasLastFrame && !g_LastFrame.isInjectMode && g_LastFrame.texture) {
                    g_LastFrame.texture->Release();
                    g_LastFrame.texture = nullptr;
                }
                g_LastFrame = std::move(frame);
                g_HasLastFrame = true;
                // After move, frame fields are nullptr - use g_LastFrame for processing
                frameToProcess = &g_LastFrame;
            } else {
                if (g_HasLastFrame && !g_LastFrame.isInjectMode && g_LastFrame.texture) {
                    g_LastFrame.texture->Release();
                }
                // AddRef for cache ownership before moving
                if (frame.texture) {
                    frame.texture->AddRef();
                }
                if (frame.timestamp > 0) {
                    lastEmittedWgcSourceQpc = frame.timestamp;
                }
                g_LastFrame = std::move(frame);
                g_HasLastFrame = true;
                // After move, frame.texture is nullptr - use g_LastFrame for processing
                frameToProcess = &g_LastFrame;
            }
        } else if (g_HasLastFrame && g_EncoderRunning && g_Recording) {
            if (hasRepeatLastFramePath) {
                wantsTrueRepeatLastFrame = true;
                isDuplicate = true;
            } else {
                if (!g_LastFrame.isInjectMode && g_LastFrame.timestamp > 0) {
                    lastEmittedWgcSourceQpc = g_LastFrame.timestamp;
                }
                frameToProcess = &g_LastFrame;
                isDuplicate = true;
            }
        }

        const bool refreshedDrainOutstandingLiveTicks =
            !g_EncoderRunning && g_DrainOutstandingWgcTicks.load(std::memory_order_acquire) && recordingOutputLive &&
            !config.video.useVFR;
        if (!popped && !drainingOutstandingLiveTicks && refreshedDrainOutstandingLiveTicks) {
            LogInfo("[EncoderThread] WGC stop drain picked up mid-cycle");
            continue;
        }
        drainingOutstandingLiveTicks = refreshedDrainOutstandingLiveTicks;

        if (!g_EncoderRunning && !popped && !drainingOutstandingLiveTicks) {
            break;
        }

        const bool consumesCfrTick =
            !config.video.useVFR && ((g_EncoderRunning && g_Recording) || drainingOutstandingLiveTicks);
        const bool isDrainPhase = !g_Recording.load(std::memory_order_acquire);
        const bool isLivePhase = recordingOutputLive && (g_Recording.load(std::memory_order_acquire) ||
                                                         drainingOutstandingLiveTicks);
        const bool scheduledLiveCfrTick = consumesCfrTick && isLivePhase;
        if (scheduledLiveCfrTick) {
            encoderGridTickCount = selectionGridTick;
            outputShortfallTicks = ce::capture_policy::GetCfrOutputShortfallTicks(liveTicksScheduled, liveTicksOutput);
            ++wgcQueueTickSampleCount;
            if (useScreenGrab) {
                const uint32_t bufferedAtTick =
                    wgcTelemetryTickArmed ? wgcBufferedAtTickStart : static_cast<uint32_t>(bufferedWgcFrames.size());
                wgcBufferedAtTickSum += bufferedAtTick;
                wgcBufferedAtTickMin = std::min(wgcBufferedAtTickMin, bufferedAtTick);
                if (wgcTelemetryTickArmed && !wgcFreshAvailableAtTickStart) {
                    ++wgcNoFreshTickCount;
                }
                if (wgcTelemetryTickArmed && !wgcReserveAvailableAtTickStart) {
                    ++wgcNoReserveTickCount;
                }
            }
            wgcNoFreshTickPermille = wgcQueueTickSampleCount > 0
                                         ? SaturatingToUint32((static_cast<uint64_t>(wgcNoFreshTickCount) * 1000ull) /
                                                              static_cast<uint64_t>(wgcQueueTickSampleCount))
                                         : 0u;
        }

        auto recordDuplicate = [&](const QueuedFrame* duplicateFrame, const InjectFrameLineage* duplicateLineage,
                                   bool duplicateFromDrainReason, bool duplicateFromDeferredReason,
                                   bool duplicateFromTimerRebaseReason, bool duplicateFromCatchupReason = false) {
            cadenceCounters.consecutiveDuplicateFrames++;
            cadenceCounters.maxConsecutiveDuplicateFrames =
                std::max(cadenceCounters.maxConsecutiveDuplicateFrames, cadenceCounters.consecutiveDuplicateFrames);
            const bool duplicateFromSourceGap = !duplicateFromDrainReason && !duplicateFromDeferredReason &&
                                                !duplicateFromTimerRebaseReason && !duplicateFromCatchupReason;
            if (useScreenGrab && duplicateFromSourceGap) {
                if (encoderLateTickCount == 0) {
                    frameCreditAccumulator = std::min(frameCreditAccumulator, bufferedWgcFrames.empty() ? 0.25 : 0.50);
                }
                if (encoderLateTickCount == 0 && bufferedWgcFrames.empty() && smoothedInputPerTick > 1.0) {
                    smoothedInputPerTick = std::max(0.90, smoothedInputPerTick * 0.90);
                }
            }
            if (g_pSharedMem) {
                g_pSharedMem->runtimeState.duplicateFrames.fetch_add(1, std::memory_order_relaxed);
                if (duplicateFromDrainReason) {
                    g_pSharedMem->runtimeState.duplicateFramesDrain.fetch_add(1, std::memory_order_relaxed);
                } else if (duplicateFromDeferredReason ||
                           (duplicateFrame && MatchesInjectFrameLineage(*duplicateFrame, lastDeferredLineage)) ||
                           (duplicateLineage && MatchesInjectFrameLineage(*duplicateLineage, lastDeferredLineage))) {
                    g_pSharedMem->runtimeState.duplicateFramesDeferred.fetch_add(1, std::memory_order_relaxed);
                } else if (duplicateFromTimerRebaseReason) {
                    g_pSharedMem->runtimeState.duplicateFramesTimerRebase.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_pSharedMem->runtimeState.duplicateFramesNoSource.fetch_add(1, std::memory_order_relaxed);
                }
            }
            static uint64_t s_lastDupLogTick = 0;
            uint64_t nowTick = GetTickCount64();
            if (nowTick - s_lastDupLogTick >= 1000) {
                const uint32_t logFrameIndex =
                    duplicateFrame ? duplicateFrame->frameIndex : (duplicateLineage ? duplicateLineage->frameIndex : 0);
                const int32_t logTextureIndex = duplicateFrame
                                                    ? duplicateFrame->textureIndex
                                                    : (duplicateLineage ? duplicateLineage->textureIndex : -1);
                const uint32_t logRingIndex =
                    duplicateFrame ? duplicateFrame->ringIndex : (duplicateLineage ? duplicateLineage->ringIndex : 0);
                const uint64_t logFenceValue =
                    duplicateFrame ? duplicateFrame->fenceValue : (duplicateLineage ? duplicateLineage->fenceValue : 0);
                LogInfo(
                    "[EncoderThread] Duplicate frame=%u tex=%d ring=%u fence=%llu: credit=%.3f rate=%.3f bufferedI=%zu "
                    "bufferedW=%zu",
                    logFrameIndex, logTextureIndex, logRingIndex, static_cast<unsigned long long>(logFenceValue),
                    frameCreditAccumulator, smoothedInputPerTick, bufferedInjectFrames.size(),
                    bufferedWgcFrames.size());
                s_lastDupLogTick = nowTick;
            }
        };
        auto emitCatchupRepeats = [&](const InjectFrameLineage* duplicateLineage) {
            if (!scheduledLiveCfrTick || catchupTicksThisLoop <= 1 || !g_HasLastFrame) {
                return;
            }

            if (useScreenGrab && !ce::capture_policy::ShouldAllowWgcExtraCatchupTicks(
                                     g_IsEncoderBottlenecked.load(std::memory_order_relaxed), bufferedWgcFrames.size(),
                                     frameCreditAccumulator, outputShortfallTicks)) {
                return;
            }

            for (uint32_t extraTick = 1; extraTick < catchupTicksThisLoop; ++extraTick) {
                if (useScreenGrab && !ce::capture_policy::ShouldAllowWgcExtraCatchupTicks(
                                         g_IsEncoderBottlenecked.load(std::memory_order_relaxed),
                                         bufferedWgcFrames.size(), frameCreditAccumulator, outputShortfallTicks)) {
                    break;
                }

                // Time budget check: if the tick budget is already exhausted,
                // skip further catchup to avoid cascading latency.
                LARGE_INTEGER budgetNow;
                QueryPerformanceCounter(&budgetNow);
                const double elapsedFromTickStartMs =
                    static_cast<double>(budgetNow.QuadPart - cycleStartQpc.QuadPart) * 1000.0 / qpcFreq.QuadPart;
                const bool allowWgcCatchupBudget = useScreenGrab && catchupTicksThisLoop > 1;
                const bool allowForceCatchupBudget =
                    useScreenGrab &&
                    outputShortfallTicks >= ce::capture_policy::kCfrShortfallForceCatchupThresholdTicks;
                const double catchupBudgetMs = allowForceCatchupBudget ? (frameIntervalMs * 3.0)
                                               : allowWgcCatchupBudget ? (frameIntervalMs * 2.0)
                                                                       : frameIntervalMs;
                if (elapsedFromTickStartMs > catchupBudgetMs) {
                    LogInfo(
                        "[EncoderThread] Catchup budget exceeded at extraTick=%u (elapsed=%.2fms > budget=%.2fms), "
                        "skipping remaining catchup",
                        extraTick, elapsedFromTickStartMs, catchupBudgetMs);
                    break;
                }

                const int64_t repeatScheduledQpc =
                    scheduledSampleQpc + static_cast<int64_t>(extraTick) * targetIntervalTicks;

                if (useScreenGrab && MediaEngine_ProcessFrameD3D11 && !bufferedWgcFrames.empty()) {
                    const int64_t catchupGridTick = encoderGridTickCount + 1;
                    const int64_t catchupSelectionTargetQpc =
                        computeWgcSelectionTargetForTick(repeatScheduledQpc, catchupGridTick, false);
                    QueuedFrame catchupFrame;
                    bool heldByPolicy = false;
                    if (tryPopBufferedWgcFrameForTarget(catchupSelectionTargetQpc, false, false, 0, &catchupFrame,
                                                        &heldByPolicy)) {
                        if (g_HasLastFrame && !g_LastFrame.isInjectMode && g_LastFrame.texture) {
                            g_LastFrame.texture->Release();
                        }
                        if (catchupFrame.texture) {
                            catchupFrame.texture->AddRef();
                        }
                        if (catchupFrame.timestamp > 0) {
                            lastEmittedWgcSourceQpc = catchupFrame.timestamp;
                        }
                        g_LastFrame = std::move(catchupFrame);
                        g_HasLastFrame = true;

                        LARGE_INTEGER catchupStartEnc, catchupEndEnc;
                        QueryPerformanceCounter(&catchupStartEnc);
                        uint64_t frameAgeUs = 0;
                        if (g_LastFrame.timestamp > 0 && catchupStartEnc.QuadPart > g_LastFrame.timestamp) {
                            frameAgeUs = static_cast<uint64_t>((catchupStartEnc.QuadPart - g_LastFrame.timestamp) *
                                                               1000000 / qpcFreq.QuadPart);
                        }
                        cadenceCounters.frameAgeAccumUs += frameAgeUs;
                        cadenceCounters.frameAgeSamples++;
                        cadenceCounters.frameAgeMaxUs =
                            std::max(cadenceCounters.frameAgeMaxUs, SaturatingToUint32(frameAgeUs));
                        if (repeatScheduledQpc > 0) {
                            const int64_t signedOutputScheduleErrorUs =
                                ((catchupStartEnc.QuadPart - repeatScheduledQpc) * 1000000) / qpcFreq.QuadPart;
                            const uint64_t absoluteOutputScheduleErrorUs =
                                static_cast<uint64_t>(signedOutputScheduleErrorUs >= 0 ? signedOutputScheduleErrorUs
                                                                                       : -signedOutputScheduleErrorUs);
                            cadenceCounters.outputScheduleErrorAccumUs += absoluteOutputScheduleErrorUs;
                            cadenceCounters.outputScheduleErrorSignedAccumUs += signedOutputScheduleErrorUs;
                            cadenceCounters.outputScheduleErrorSamples++;
                            cadenceCounters.outputScheduleErrorMaxUs =
                                std::max(cadenceCounters.outputScheduleErrorMaxUs,
                                         SaturatingToUint32(absoluteOutputScheduleErrorUs));
                            if (signedOutputScheduleErrorUs < 0) {
                                cadenceCounters.outputScheduleEarlyMaxUs =
                                    std::max(cadenceCounters.outputScheduleEarlyMaxUs,
                                             SaturatingToUint32(static_cast<uint64_t>(-signedOutputScheduleErrorUs)));
                            } else {
                                cadenceCounters.outputScheduleLateMaxUs =
                                    std::max(cadenceCounters.outputScheduleLateMaxUs,
                                             SaturatingToUint32(static_cast<uint64_t>(signedOutputScheduleErrorUs)));
                            }
                        }

                        const int64_t catchupTimelineElapsedUs = computeLiveTimelineElapsedUs(repeatScheduledQpc);
                        if (!MediaEngine_ProcessFrameD3D11(g_LastFrame.texture, g_LastFrame.timestamp,
                                                           g_LastFrame.width, g_LastFrame.height, g_LastFrame.isHDR,
                                                           g_LastFrame.captureLeft, g_LastFrame.captureTop,
                                                           catchupTimelineElapsedUs)) {
                            cadenceCounters.liveTickMissCount++;
                            break;
                        }
                        QueryPerformanceCounter(&catchupEndEnc);

                        const double currentEncodeMs =
                            (double)(catchupEndEnc.QuadPart - catchupStartEnc.QuadPart) * 1000.0 / qpcFreq.QuadPart;
                        const double pureEncodeMs = (double)MediaEngine_GetLastFrameEncodeTimeUs() / 1000.0;
                        if (pureEncodeMs > 0.0) {
                            if (smoothedEncodeMs == 0.0) {
                                smoothedEncodeMs = pureEncodeMs;
                            } else {
                                smoothedEncodeMs =
                                    smoothedEncodeMs * (1.0 - kEncodeEmaAlpha) + pureEncodeMs * kEncodeEmaAlpha;
                            }
                        }
                        UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs);

                        if (g_pSharedMem) {
                            if (currentEncodeMs > frameIntervalMs * 1.10) {
                                g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
                            }
                            g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                            g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                        }

                        if (catchupSelectionTargetQpc > 0 && g_LastFrame.timestamp > 0) {
                            const int64_t signedSelectionErrorUs =
                                ((g_LastFrame.timestamp - catchupSelectionTargetQpc) * 1000000) / qpcFreq.QuadPart;
                            const int64_t absoluteSelectionErrorUs =
                                signedSelectionErrorUs >= 0 ? signedSelectionErrorUs : -signedSelectionErrorUs;
                            cadenceCounters.selectionErrorAccumUs += static_cast<uint64_t>(absoluteSelectionErrorUs);
                            cadenceCounters.selectionErrorSignedAccumUs += signedSelectionErrorUs;
                            cadenceCounters.selectionErrorSamples++;
                            cadenceCounters.selectionErrorMaxUs =
                                std::max(cadenceCounters.selectionErrorMaxUs,
                                         SaturatingToUint32(static_cast<uint64_t>(absoluteSelectionErrorUs)));
                            if (signedSelectionErrorUs < 0) {
                                cadenceCounters.selectionEarlyMaxUs =
                                    std::max(cadenceCounters.selectionEarlyMaxUs,
                                             SaturatingToUint32(static_cast<uint64_t>(-signedSelectionErrorUs)));
                            } else {
                                cadenceCounters.selectionLateMaxUs =
                                    std::max(cadenceCounters.selectionLateMaxUs,
                                             SaturatingToUint32(static_cast<uint64_t>(signedSelectionErrorUs)));
                            }
                            wgcSelectionErrorAccumUs += static_cast<uint64_t>(absoluteSelectionErrorUs);
                            wgcSelectionErrorSignedAccumUs += signedSelectionErrorUs;
                            ++wgcSelectionErrorSamples;
                            wgcSelectionErrorMaxUs =
                                std::max(wgcSelectionErrorMaxUs,
                                         SaturatingToUint32(static_cast<uint64_t>(absoluteSelectionErrorUs)));
                            if (signedSelectionErrorUs < 0) {
                                wgcSelectionEarlyMaxUs =
                                    std::max(wgcSelectionEarlyMaxUs,
                                             SaturatingToUint32(static_cast<uint64_t>(-signedSelectionErrorUs)));
                            } else {
                                wgcSelectionLateMaxUs =
                                    std::max(wgcSelectionLateMaxUs,
                                             SaturatingToUint32(static_cast<uint64_t>(signedSelectionErrorUs)));
                            }
                        }

                        cadenceCounters.consecutiveDuplicateFrames = 0;
                        cadenceCounters.liveTickEmitCount++;
                        cadenceCounters.liveTickUniqueCount++;
                        cadenceCounters.CommitHoldRun();
                        cadenceCounters.holdTicksRunning = 1;
                        ++liveTicksOutput;
                        ++encoderGridTickCount;
                        ++cfrCatchupTicksExecuted;
                        ++wgcFreshCatchupCount;
                        continue;
                    }

                    break;
                }

                if (!hasRepeatLastFramePath) {
                    cadenceCounters.liveTickMissCount++;
                    break;
                }

                LARGE_INTEGER repeatStartEnc, repeatEndEnc;
                QueryPerformanceCounter(&repeatStartEnc);
                bool repeatSucceeded = repeatLastFrameForScheduledQpc(repeatScheduledQpc);
                bool repeatDeferred = MediaEngine_WasLastFrameDeferred && MediaEngine_WasLastFrameDeferred();
                QueryPerformanceCounter(&repeatEndEnc);
                if (!repeatSucceeded || repeatDeferred) {
                    cadenceCounters.liveTickMissCount++;
                    break;
                }

                const double currentEncodeMs =
                    (double)(repeatEndEnc.QuadPart - repeatStartEnc.QuadPart) * 1000.0 / qpcFreq.QuadPart;
                const double pureEncodeMs = (double)MediaEngine_GetLastFrameEncodeTimeUs() / 1000.0;
                if (pureEncodeMs > 0.0) {
                    if (smoothedEncodeMs == 0.0) {
                        smoothedEncodeMs = pureEncodeMs;
                    } else {
                        smoothedEncodeMs = smoothedEncodeMs * (1.0 - kEncodeEmaAlpha) + pureEncodeMs * kEncodeEmaAlpha;
                    }
                }
                UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs);

                if (g_pSharedMem) {
                    if (currentEncodeMs > frameIntervalMs * 1.10) {
                        g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
                    }
                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                }

                recordDuplicate(&g_LastFrame, duplicateLineage, false, false, false, true);
                ++wgcRepeatCatchupCount;
                cadenceCounters.liveTickEmitCount++;
                cadenceCounters.liveTickDuplicateCount++;
                cadenceCounters.holdTicksRunning++;
                ++liveTicksOutput;
                ++encoderGridTickCount;
                ++cfrCatchupTicksExecuted;
            }
        };

        if ((!frameToProcess || wantsTrueRepeatLastFrame) && scheduledLiveCfrTick && hasRepeatLastFramePath) {
            LARGE_INTEGER repeatStartEnc, repeatEndEnc;
            QueryPerformanceCounter(&repeatStartEnc);
            const bool duplicateFromDrain = isDrainPhase;
            bool duplicateFromDeferred = false;
            const bool duplicateFromTimerRebase = encoderLateTickCount >= 2;
            const InjectFrameLineage duplicateLineage =
                g_HasLastFrame ? MakeInjectFrameLineage(g_LastFrame) : InjectFrameLineage{};
            bool encodeSucceeded = repeatLastFrameForScheduledQpc(scheduledSampleQpc);
            bool encodeDeferred = MediaEngine_WasLastFrameDeferred && MediaEngine_WasLastFrameDeferred();
            QueryPerformanceCounter(&repeatEndEnc);

            if (encodeSucceeded && !encodeDeferred) {
                double currentEncodeMs =
                    (double)(repeatEndEnc.QuadPart - repeatStartEnc.QuadPart) * 1000.0 / qpcFreq.QuadPart;
                double pureEncodeMs = (double)MediaEngine_GetLastFrameEncodeTimeUs() / 1000.0;
                if (smoothedEncodeMs == 0.0) {
                    smoothedEncodeMs = pureEncodeMs;
                } else {
                    smoothedEncodeMs = smoothedEncodeMs * (1.0 - kEncodeEmaAlpha) + pureEncodeMs * kEncodeEmaAlpha;
                }
                UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs);
                if (g_pSharedMem && currentEncodeMs > frameIntervalMs * 1.10) {
                    g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                } else if (g_pSharedMem) {
                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                }
                cadenceCounters.consecutiveDeferredFrames = 0;
                if (useScreenGrab) {
                    if (duplicateFromTimerRebase) {
                        ++wgcRepeatTimerLateCount;
                    } else if (!frameToProcess && !wantsTrueRepeatLastFrame) {
                        ++wgcRepeatNoFreshCount;
                    } else if (wantsTrueRepeatLastFrame) {
                        ++wgcRepeatNoFreshCount;
                    }
                }
                recordDuplicate(nullptr, duplicateLineage.IsValid() ? &duplicateLineage : nullptr, duplicateFromDrain,
                                duplicateFromDeferred, duplicateFromTimerRebase);
                cadenceCounters.liveTickEmitCount++;
                cadenceCounters.liveTickDuplicateCount++;
                cadenceCounters.holdTicksRunning++;
                ++liveTicksOutput;
                emitCatchupRepeats(duplicateLineage.IsValid() ? &duplicateLineage : nullptr);
            } else {
                cadenceCounters.liveTickMissCount++;
            }
            continue;
        }

        if (frameToProcess) {
            LARGE_INTEGER startEnc, endEnc;
            QueryPerformanceCounter(&startEnc);

            bool encodeSucceeded = true;
            bool encodeDeferred = false;
            const bool duplicateFromDrain = isDuplicate && isDrainPhase;

            const int64_t idealQpc =
                (encoderGridStartQpc > 0 && targetIntervalTicks > 0)
                    ? ComputeIdealOutputQpc(encoderGridStartQpc, selectionGridTick, targetIntervalTicks)
                    : 0;
            int64_t signedSelectionErrorUs = 0;
            int64_t absoluteSelectionErrorUs = 0;
            const int64_t selectionMetricTargetQpc =
                !frameToProcess->isInjectMode
                    ? computeWgcSelectionTargetQpc(ce::capture_policy::ShouldApplyWgcSelectionDelay(
                          recordingOutputLive, outputShortfallTicks,
                          g_IsEncoderBottlenecked.load(std::memory_order_relaxed), wgcReserveAvailableAtTickStart))
                    : idealQpc;
            if (selectionMetricTargetQpc > 0) {
                signedSelectionErrorUs =
                    ((frameToProcess->timestamp - selectionMetricTargetQpc) * 1000000) / qpcFreq.QuadPart;
                absoluteSelectionErrorUs =
                    signedSelectionErrorUs >= 0 ? signedSelectionErrorUs : -signedSelectionErrorUs;
            }

            uint64_t frameAgeUs = 0;
            if (frameToProcess->timestamp > 0 && startEnc.QuadPart > frameToProcess->timestamp) {
                frameAgeUs =
                    static_cast<uint64_t>((startEnc.QuadPart - frameToProcess->timestamp) * 1000000 / qpcFreq.QuadPart);
            }
            cadenceCounters.frameAgeAccumUs += frameAgeUs;
            cadenceCounters.frameAgeSamples++;
            cadenceCounters.frameAgeMaxUs = std::max(cadenceCounters.frameAgeMaxUs, SaturatingToUint32(frameAgeUs));
            if (scheduledLiveCfrTick && scheduledSampleQpc > 0) {
                const int64_t signedOutputScheduleErrorUs =
                    ((startEnc.QuadPart - scheduledSampleQpc) * 1000000) / qpcFreq.QuadPart;
                const uint64_t absoluteOutputScheduleErrorUs = static_cast<uint64_t>(
                    signedOutputScheduleErrorUs >= 0 ? signedOutputScheduleErrorUs : -signedOutputScheduleErrorUs);
                cadenceCounters.outputScheduleErrorAccumUs += absoluteOutputScheduleErrorUs;
                cadenceCounters.outputScheduleErrorSignedAccumUs += signedOutputScheduleErrorUs;
                cadenceCounters.outputScheduleErrorSamples++;
                cadenceCounters.outputScheduleErrorMaxUs = std::max(cadenceCounters.outputScheduleErrorMaxUs,
                                                                    SaturatingToUint32(absoluteOutputScheduleErrorUs));
                if (signedOutputScheduleErrorUs < 0) {
                    cadenceCounters.outputScheduleEarlyMaxUs =
                        std::max(cadenceCounters.outputScheduleEarlyMaxUs,
                                 SaturatingToUint32(static_cast<uint64_t>(-signedOutputScheduleErrorUs)));
                } else {
                    cadenceCounters.outputScheduleLateMaxUs =
                        std::max(cadenceCounters.outputScheduleLateMaxUs,
                                 SaturatingToUint32(static_cast<uint64_t>(signedOutputScheduleErrorUs)));
                }
            }

            auto encodeCurrentFrame = [&]() {
                if (frameToProcess->isInjectMode) {
                    encodeSucceeded = MediaEngine_ProcessFrame(
                        (uint64_t)frameToProcess->sharedHandle, (uint64_t)frameToProcess->fenceHandle,
                        frameToProcess->fenceValue, frameToProcess->timestamp, frameToProcess->luidLow,
                        frameToProcess->luidHigh, frameToProcess->sourcePid, frameToProcess->width,
                        frameToProcess->height, frameToProcess->format, frameToProcess->isHDR, frameToProcess->isShmem,
                        frameToProcess->shmemSlot);
                    encodeDeferred = MediaEngine_WasLastFrameDeferred && MediaEngine_WasLastFrameDeferred();
                } else {
                    const int64_t liveTimelineElapsedUs =
                        scheduledLiveCfrTick ? computeLiveTimelineElapsedUs(scheduledSampleQpc) : -1;
                    encodeSucceeded = MediaEngine_ProcessFrameD3D11(frameToProcess->texture, frameToProcess->timestamp,
                                                                    frameToProcess->width, frameToProcess->height,
                                                                    frameToProcess->isHDR, frameToProcess->captureLeft,
                                                                    frameToProcess->captureTop, liveTimelineElapsedUs);
                    encodeDeferred = false;
                }
            };

            encodeCurrentFrame();

            QueryPerformanceCounter(&endEnc);
            double currentEncodeMs = (double)(endEnc.QuadPart - startEnc.QuadPart) * 1000.0 / qpcFreq.QuadPart;
            double pureEncodeMs = (double)MediaEngine_GetLastFrameEncodeTimeUs() / 1000.0;
            if (pureEncodeMs > 0.0) {
                if (smoothedEncodeMs == 0.0) {
                    smoothedEncodeMs = pureEncodeMs;
                } else {
                    smoothedEncodeMs = smoothedEncodeMs * (1.0 - kEncodeEmaAlpha) + pureEncodeMs * kEncodeEmaAlpha;
                }
            }
            UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs);

            if (popped && frameToProcess->isInjectMode) {
                if (encodeDeferred) {
                    const InjectFrameLineage deferredLineage = MakeInjectFrameLineage(*frameToProcess);
                    frameCreditAccumulator = std::max(frameCreditAccumulator, 1.0);
                    g_InjectDeferredFrames.fetch_add(1, std::memory_order_relaxed);
                    if (g_pSharedMem) {
                        g_pSharedMem->runtimeState.deferredFrames.fetch_add(1, std::memory_order_relaxed);
                        if (lastDeferredLineage.IsValid() &&
                            MatchesInjectFrameLineage(*frameToProcess, lastDeferredLineage)) {
                            g_pSharedMem->runtimeState.repeatedDeferredFrames.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    cadenceCounters.consecutiveDeferredFrames++;
                    cadenceCounters.maxConsecutiveDeferredFrames = std::max(
                        cadenceCounters.maxConsecutiveDeferredFrames, cadenceCounters.consecutiveDeferredFrames);
                    lastDeferredLineage = deferredLineage;
                    if (g_HasLastFrame) {
                        g_LastFrame.deferCount++;
                        bufferedInjectFrames.push_front(std::move(g_LastFrame));
                        g_HasLastFrame = false;
                        g_LastFrame = QueuedFrame{};
                    }
                    static uint64_t s_lastDeferredLogTick = 0;
                    uint64_t nowTick = GetTickCount64();
                    if (nowTick - s_lastDeferredLogTick >= 1000) {
                        LogInfo(
                            "[EncoderThread] Deferred inject frame=%u ring=%u tex=%d fence=%llu ts=%lld buffered=%zu "
                            "credit=%.3f",
                            deferredLineage.frameIndex, deferredLineage.ringIndex, deferredLineage.textureIndex,
                            static_cast<unsigned long long>(deferredLineage.fenceValue),
                            static_cast<long long>(deferredLineage.timestamp), bufferedInjectFrames.size(),
                            frameCreditAccumulator);
                        s_lastDeferredLogTick = nowTick;
                    }

                    if (consumesCfrTick && isLivePhase && hasRepeatLastFramePath) {
                        isDuplicate = true;
                        duplicateFromDeferred = true;
                        encodeSucceeded = repeatLastFrameForScheduledQpc(scheduledSampleQpc);
                        encodeDeferred = MediaEngine_WasLastFrameDeferred && MediaEngine_WasLastFrameDeferred();
                        if (!encodeSucceeded || encodeDeferred) {
                            if (scheduledLiveCfrTick) {
                                cadenceCounters.liveTickMissCount++;
                            }
                            continue;
                        }
                    } else {
                        if (scheduledLiveCfrTick) {
                            cadenceCounters.liveTickMissCount++;
                        }
                        continue;
                    }
                }

                if (g_pSharedMem && currentEncodeMs > frameIntervalMs * 1.10) {
                    g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
                }

                if (popped && frameToProcess->isInjectMode && encodeSucceeded) {
                    const double currentFenceMs = (double)MediaEngine_GetLastFrameFenceWaitUs() / 1000.0;
                    if (smoothedInjectFenceMs == 0.0) {
                        smoothedInjectFenceMs = currentFenceMs;
                    } else {
                        smoothedInjectFenceMs = smoothedInjectFenceMs * 0.90 + currentFenceMs * 0.10;
                    }
                }
                static DWORD lastWarningTime = 0;
                if (smoothedEncodeMs > frameIntervalMs * 0.85) {
                    DWORD now = GetTickCount();
                    if (now - lastWarningTime > 5000) {
                        LogInfo("[WARN] Encoder approaching capacity: %.2fms avg vs %.2fms budget", smoothedEncodeMs,
                                frameIntervalMs);
                        lastWarningTime = now;
                    }
                }

                cadenceCounters.consecutiveDeferredFrames = 0;

                if (!isDuplicate && frameToProcess->frameIndex != 0) {
                    if (lastEncodedInjectFrameIndex != 0 && frameToProcess->frameIndex < lastEncodedInjectFrameIndex) {
                        LogWarn(
                            "[EncoderThread] Inject lineage regression: encoded frame=%u after frame=%u (ring=%u "
                            "tex=%d ts=%lld)",
                            frameToProcess->frameIndex, lastEncodedInjectFrameIndex, frameToProcess->ringIndex,
                            frameToProcess->textureIndex, static_cast<long long>(frameToProcess->timestamp));
                        if (g_pSharedMem) {
                            g_pSharedMem->runtimeState.frameIndexRegressions.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    lastEncodedInjectFrameIndex = frameToProcess->frameIndex;
                }
                if (!isDuplicate && IsInjectTextureIndexValid(frameToProcess->textureIndex)) {
                    uint32_t& lastTextureFrame =
                        lastEncodedFrameByTextureIndex[static_cast<size_t>(frameToProcess->textureIndex)];
                    if (lastTextureFrame != 0 && frameToProcess->frameIndex != 0 &&
                        frameToProcess->frameIndex <= lastTextureFrame) {
                        LogWarn(
                            "[EncoderThread] Texture slot reuse anomaly: tex=%d frame=%u previous=%u ring=%u "
                            "fence=%llu ts=%lld",
                            frameToProcess->textureIndex, frameToProcess->frameIndex, lastTextureFrame,
                            frameToProcess->ringIndex, static_cast<unsigned long long>(frameToProcess->fenceValue),
                            static_cast<long long>(frameToProcess->timestamp));
                        if (g_pSharedMem) {
                            g_pSharedMem->runtimeState.textureReuseAnomalies.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    lastTextureFrame = frameToProcess->frameIndex;
                }
                lastDeferredLineage = {};

                if (g_pSharedMem) {
                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    if (isLivePhase) {
                        g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    } else if (isDrainPhase) {
                        g_pSharedMem->runtimeState.drainFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    }
                    g_pSharedMem->frameRing.readIndex.store(frameToProcess->ringIndex + 1, std::memory_order_release);
                }
            } else {
                cadenceCounters.consecutiveDeferredFrames = 0;
                if (g_pSharedMem) {
                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    if (isLivePhase) {
                        g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    } else if (isDrainPhase) {
                        g_pSharedMem->runtimeState.drainFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }

            if (encodeSucceeded) {
                if (selectionMetricTargetQpc > 0 && !frameToProcess->isInjectMode && !isDuplicate) {
                    cadenceCounters.selectionErrorAccumUs += static_cast<uint64_t>(absoluteSelectionErrorUs);
                    cadenceCounters.selectionErrorSignedAccumUs += signedSelectionErrorUs;
                    cadenceCounters.selectionErrorSamples++;
                    cadenceCounters.selectionErrorMaxUs =
                        std::max(cadenceCounters.selectionErrorMaxUs,
                                 SaturatingToUint32(static_cast<uint64_t>(absoluteSelectionErrorUs)));
                    if (signedSelectionErrorUs < 0) {
                        cadenceCounters.selectionEarlyMaxUs =
                            std::max(cadenceCounters.selectionEarlyMaxUs,
                                     SaturatingToUint32(static_cast<uint64_t>(-signedSelectionErrorUs)));
                    } else {
                        cadenceCounters.selectionLateMaxUs =
                            std::max(cadenceCounters.selectionLateMaxUs,
                                     SaturatingToUint32(static_cast<uint64_t>(signedSelectionErrorUs)));
                    }
                    wgcSelectionErrorAccumUs += static_cast<uint64_t>(absoluteSelectionErrorUs);
                    wgcSelectionErrorSignedAccumUs += signedSelectionErrorUs;
                    ++wgcSelectionErrorSamples;
                    wgcSelectionErrorMaxUs = std::max(
                        wgcSelectionErrorMaxUs, SaturatingToUint32(static_cast<uint64_t>(absoluteSelectionErrorUs)));
                    if (signedSelectionErrorUs < 0) {
                        wgcSelectionEarlyMaxUs = std::max(
                            wgcSelectionEarlyMaxUs, SaturatingToUint32(static_cast<uint64_t>(-signedSelectionErrorUs)));
                    } else {
                        wgcSelectionLateMaxUs = std::max(
                            wgcSelectionLateMaxUs, SaturatingToUint32(static_cast<uint64_t>(signedSelectionErrorUs)));
                    }
                }

                if (isDuplicate) {
                    const InjectFrameLineage duplicateLineage =
                        frameToProcess ? MakeInjectFrameLineage(*frameToProcess) : InjectFrameLineage{};
                    recordDuplicate(frameToProcess, duplicateLineage.IsValid() ? &duplicateLineage : nullptr,
                                    duplicateFromDrain, duplicateFromDeferred, duplicateFromTimerRebase);
                } else {
                    cadenceCounters.consecutiveDuplicateFrames = 0;
                }
                cadenceCounters.liveTickEmitCount += (consumesCfrTick && isLivePhase) ? 1u : 0u;
                if (consumesCfrTick && isLivePhase) {
                    if (isDuplicate) {
                        cadenceCounters.liveTickDuplicateCount++;
                        cadenceCounters.holdTicksRunning++;
                    } else {
                        cadenceCounters.liveTickUniqueCount++;
                        cadenceCounters.CommitHoldRun();
                        cadenceCounters.holdTicksRunning = 1;
                    }
                }
                if (consumesCfrTick && isLivePhase) {
                    ++liveTicksOutput;
                }
                const InjectFrameLineage catchupLineage =
                    frameToProcess ? MakeInjectFrameLineage(*frameToProcess) : InjectFrameLineage{};
                emitCatchupRepeats(catchupLineage.IsValid() ? &catchupLineage : nullptr);
            } else if (scheduledLiveCfrTick) {
                cadenceCounters.liveTickMissCount++;
            }
        }

        if (popped && !frame.isInjectMode && frame.texture) {
            frame.texture->Release();
        }

        // Track encoder processing cycle time (timer wake through end of encode)
        if (cycleStartQpc.QuadPart > 0) {
            LARGE_INTEGER cycleEndQpc;
            QueryPerformanceCounter(&cycleEndQpc);
            const double cycleMs =
                static_cast<double>(cycleEndQpc.QuadPart - cycleStartQpc.QuadPart) * 1000.0 / qpcFreq.QuadPart;
            if (smoothedEncCycleMs < 0.001) {
                smoothedEncCycleMs = cycleMs;
            } else {
                smoothedEncCycleMs = smoothedEncCycleMs * 0.85 + cycleMs * 0.15;
            }
            encCycleMaxMs = std::max(encCycleMaxMs, static_cast<uint32_t>(cycleMs * 1000.0));
            // Log encode spikes > 10ms (pure encode, not full cycle)
            if (smoothedEncodeMs > 10.0) {
                ++encodeSpikeCountThisSecond;
                static uint32_t s_spikeLogCount = 0;
                ++s_spikeLogCount;
                if (s_spikeLogCount <= 5 || s_spikeLogCount % 120 == 0) {
                    LogInfo("[EncoderThread] Spike: encode=%.2fms cycle=%.2fms frame=%llu", smoothedEncodeMs, cycleMs,
                            static_cast<unsigned long long>(liveTicksOutput));
                }
            }
        }

        if (g_pSharedMem && GetTickCount() - lastHealthLog >= 1000) {
            auto& state = g_pSharedMem->runtimeState;
            const uint32_t avgFrameAgeUs =
                cadenceCounters.frameAgeSamples > 0
                    ? SaturatingToUint32(cadenceCounters.frameAgeAccumUs / cadenceCounters.frameAgeSamples)
                    : 0;
            const uint32_t avgSelectionErrorUs = cadenceCounters.outputScheduleErrorSamples > 0
                                                     ? SaturatingToUint32(cadenceCounters.outputScheduleErrorAccumUs /
                                                                          cadenceCounters.outputScheduleErrorSamples)
                                                     : 0;
            const int32_t avgSignedSelectionErrorUs =
                cadenceCounters.outputScheduleErrorSamples > 0
                    ? static_cast<int32_t>(cadenceCounters.outputScheduleErrorSignedAccumUs /
                                           static_cast<int64_t>(cadenceCounters.outputScheduleErrorSamples))
                    : 0;
            const uint32_t avgWgcSelectionErrorUs =
                wgcSelectionErrorSamples > 0 ? SaturatingToUint32(wgcSelectionErrorAccumUs / wgcSelectionErrorSamples)
                                             : 0;
            const int32_t avgSignedWgcSelectionErrorUs =
                wgcSelectionErrorSamples > 0 ? static_cast<int32_t>(wgcSelectionErrorSignedAccumUs /
                                                                    static_cast<int64_t>(wgcSelectionErrorSamples))
                                             : 0;
            state.frameAgeAvgUs.store(avgFrameAgeUs, std::memory_order_relaxed);
            state.frameAgeMaxUs.store(cadenceCounters.frameAgeMaxUs, std::memory_order_relaxed);
            state.selectionErrorAvgUs.store(avgSelectionErrorUs, std::memory_order_relaxed);
            state.selectionErrorMaxUs.store(cadenceCounters.outputScheduleErrorMaxUs, std::memory_order_relaxed);
            state.selectionErrorSignedAvgUs.store(avgSignedSelectionErrorUs, std::memory_order_relaxed);
            state.selectionEarlyMaxUs.store(cadenceCounters.outputScheduleEarlyMaxUs, std::memory_order_relaxed);
            state.selectionLateMaxUs.store(cadenceCounters.outputScheduleLateMaxUs, std::memory_order_relaxed);
            state.consecutiveDeferredFrames.store(cadenceCounters.consecutiveDeferredFrames, std::memory_order_relaxed);
            state.maxConsecutiveDeferredFrames.store(cadenceCounters.maxConsecutiveDeferredFrames,
                                                     std::memory_order_relaxed);
            state.consecutiveDuplicateFrames.store(cadenceCounters.consecutiveDuplicateFrames,
                                                   std::memory_order_relaxed);
            state.maxConsecutiveDuplicateFrames.store(cadenceCounters.maxConsecutiveDuplicateFrames,
                                                      std::memory_order_relaxed);

            const uint32_t dupNoSource = state.duplicateFramesNoSource.load(std::memory_order_relaxed);
            const uint32_t dupDeferred = state.duplicateFramesDeferred.load(std::memory_order_relaxed);
            const uint32_t dupTimer = state.duplicateFramesTimerRebase.load(std::memory_order_relaxed);
            const uint32_t dupDrain = state.duplicateFramesDrain.load(std::memory_order_relaxed);
            const uint32_t invalidMeta = state.invalidFrameMetadata.load(std::memory_order_relaxed);
            const uint32_t invalidHandle = state.invalidSharedHandles.load(std::memory_order_relaxed);
            const uint32_t tsRegress = state.sourceTimestampRegressions.load(std::memory_order_relaxed);
            const uint32_t tsStall = state.sourceTimestampStalls.load(std::memory_order_relaxed);
            const uint32_t timerRebases = state.timerRebases.load(std::memory_order_relaxed);
            const uint32_t packetClamps = state.packetDurationClamps.load(std::memory_order_relaxed);
            const uint32_t negativePts = state.negativePtsCount.load(std::memory_order_relaxed);
            const uint32_t nonMonotonicPts = state.nonMonotonicPtsCount.load(std::memory_order_relaxed);
            const uint32_t overloadFlags = state.encoderOverloadFlags.load(std::memory_order_relaxed);
            const uint32_t muxQueueBytes = state.muxQueueBytes.load(std::memory_order_relaxed);
            const uint32_t muxQueuePackets = state.muxQueuePackets.load(std::memory_order_relaxed);
            const uint32_t muxBackpressureCount = state.muxBackpressureCount.load(std::memory_order_relaxed);
            const uint32_t muxBackpressureWaitUs = state.muxBackpressureWaitUs.load(std::memory_order_relaxed);
            const uint32_t muxBackpressureMaxWaitUs = state.muxBackpressureMaxWaitUs.load(std::memory_order_relaxed);
            const uint32_t oldestBufferedFrameAgeUs = state.oldestBufferedFrameAgeUs.load(std::memory_order_relaxed);
            uint64_t liveWallElapsedUs = 0;
            if (recordingOutputLive && liveStartQpc.QuadPart > 0 && targetIntervalTicks > 0 && liveTicksScheduled > 0) {
                LARGE_INTEGER nowQpc;
                QueryPerformanceCounter(&nowQpc);
                if (nowQpc.QuadPart > liveStartQpc.QuadPart) {
                    liveWallElapsedUs =
                        static_cast<uint64_t>((nowQpc.QuadPart - liveStartQpc.QuadPart) * 1000000 / qpcFreq.QuadPart);
                    outputShortfallTicks = updateLiveCfrShortfall(nowQpc.QuadPart);
                }
            }
            const double shortfallDurationMs =
                ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs);
            const double sustainableOutputFps = ce::capture_policy::GetEncoderSustainableOutputFps(smoothedEncodeMs);
            const uint32_t encoderBudgetUtilizationPermille =
                ce::capture_policy::GetEncoderBudgetUtilizationPermille(smoothedEncodeMs, frameIntervalMs);
            const bool encoderTooSlowForTarget =
                ce::capture_policy::IsEncoderTooSlowForTargetFps(smoothedEncodeMs, frameIntervalMs, outputFps);
            const double oldestBufferedFrameAgeMs = static_cast<double>(oldestBufferedFrameAgeUs) / 1000.0;

            const uint32_t bufferedAtTickAvgPermille =
                wgcQueueTickSampleCount > 0
                    ? SaturatingToUint32((static_cast<uint64_t>(wgcBufferedAtTickSum) * 1000ull) /
                                         static_cast<uint64_t>(wgcQueueTickSampleCount))
                    : 0u;
            const uint32_t bufferedAtTickMinValue = (wgcBufferedAtTickMin == UINT32_MAX) ? 0u : wgcBufferedAtTickMin;
            state.wgcQueueEmptyTickPermille.store(wgcNoFreshTickPermille, std::memory_order_relaxed);
            state.wgcBufferedAtTickAvgPermille.store(bufferedAtTickAvgPermille, std::memory_order_relaxed);
            state.wgcBufferedAtTickMin.store(bufferedAtTickMinValue, std::memory_order_relaxed);
            state.wgcStarvedTickCount.store(wgcNoFreshTickCount, std::memory_order_relaxed);
            state.wgcSingleFrameTickCount.store(wgcNoReserveTickCount, std::memory_order_relaxed);

            // Flush the in-progress hold run into the histogram before logging,
            // but preserve the running count so it continues into the next interval.
            const uint32_t savedHoldTicks = cadenceCounters.holdTicksRunning;
            cadenceCounters.CommitHoldRun();

            // Compute input frame rate predictor diagnostics
            const uint32_t srcFpsX100Val =
                wgcInputPredictor.IsCalibrated()
                    ? static_cast<uint32_t>(wgcInputPredictor.GetPredictedFps(qpcFreq.QuadPart) * 100.0)
                    : 0u;
            const uint32_t srcJitterUsVal = wgcInputPredictor.IsCalibrated()
                                                ? static_cast<uint32_t>(wgcInputPredictor.GetJitterUs(qpcFreq.QuadPart))
                                                : 0u;
            const uint32_t dupTsPerSec =
                g_WgcCap ? g_WgcCap->GetNormalizedDuplicateTimestampCount() : dupTimestampCount;
            dupTimestampCount = 0;
            encCycleMaxMs = 0;

            LogInfo(
                "[Cadence Health] Phase=%s | AgeAvg=%uus AgeMax=%uus | SelAvg=%uus SelMax=%uus SelBias=%dus "
                "EarlyMax=%uus LateMax=%uus | WgcSelAvg=%uus WgcSelMax=%uus WgcSelBias=%dus WgcEarly=%uus WgcLate=%uus "
                "Hold=%u HoldFresh=%u Spend=%u CatchUp=%u CatchFresh=%u | DefStreak=%u/%u DupStreak=%u/%u | DupSrc=%u "
                "DupDef=%u "
                "DupTimer=%u DupDrain=%u | TickEmit=%u TickUnique=%u TickDup=%u TickMiss=%u | "
                "HoldHist=%u/%u/%u/%u/%u/%u | LiveWall=%lluus LiveTicks=%llu Shortfall=%u/%.1fms FreshMiss=%upm "
                "BufAvg=%upm BufMin=%u NoFresh=%u NoReserve=%u Oldest=%.1fms | WgcAct Fresh=%u DupSrc=%u DropObs=%u "
                "SelMiss=%u StaleUni=%u "
                "Ancient=%u RepFreshMiss=%u RepHold=%u RepCov=%u CovDelay=%u RepLate=%u RepCatch=%u | TsReg=%u TsStall=%u "
                "TimerRebase=%u | "
                "InvalidMeta=%u InvalidHandle=%u | PktClamp=%u NegPTS=%u NonMonoPTS=%u | WgcThr=%u Adj=%u | Over=0x%X "
                "MuxQ=%uKB/%u MuxBp=%u Wait=%uus Max=%uus | EncEma=%.2fms Budget=%upm Sust=%.1ffps TooSlow=%d "
                "Bottleneck=%d | SrcFps=%.2f SrcJitter=%uus DupTs=%u EncCycle=%.2fms EncSpike=%u",
                CapturePipelinePhaseToString(state.capturePhase.load(std::memory_order_relaxed)), avgFrameAgeUs,
                cadenceCounters.frameAgeMaxUs, avgSelectionErrorUs, cadenceCounters.outputScheduleErrorMaxUs,
                avgSignedSelectionErrorUs, cadenceCounters.outputScheduleEarlyMaxUs,
                cadenceCounters.outputScheduleLateMaxUs, avgWgcSelectionErrorUs, wgcSelectionErrorMaxUs,
                avgSignedWgcSelectionErrorUs, wgcSelectionEarlyMaxUs, wgcSelectionLateMaxUs, wgcHoldForNextTickCount,
                wgcHeldFreshFrameTickCount, wgcReserveSpendTickCount, cfrCatchupTicksExecuted, wgcFreshCatchupCount,
                cadenceCounters.consecutiveDeferredFrames, cadenceCounters.maxConsecutiveDeferredFrames,
                cadenceCounters.consecutiveDuplicateFrames, cadenceCounters.maxConsecutiveDuplicateFrames,
                dupNoSource - lastDuplicateReasonNoSource, dupDeferred - lastDuplicateReasonDeferred,
                dupTimer - lastDuplicateReasonTimerRebase, dupDrain - lastDuplicateReasonDrain,
                cadenceCounters.liveTickEmitCount, cadenceCounters.liveTickUniqueCount,
                cadenceCounters.liveTickDuplicateCount, cadenceCounters.liveTickMissCount, cadenceCounters.holdHist[0],
                cadenceCounters.holdHist[1], cadenceCounters.holdHist[2], cadenceCounters.holdHist[3],
                cadenceCounters.holdHist[4], cadenceCounters.holdHist[5],
                static_cast<unsigned long long>(liveWallElapsedUs), static_cast<unsigned long long>(liveTicksOutput),
                outputShortfallTicks, shortfallDurationMs, wgcNoFreshTickPermille, bufferedAtTickAvgPermille,
                bufferedAtTickMinValue, wgcNoFreshTickCount, wgcNoReserveTickCount, oldestBufferedFrameAgeMs,
                wgcSelectFreshCount, wgcSelectDuplicateSourceCount, wgcDropObsoleteCount, wgcFreshSelectionMissCount,
                wgcStaleUniqueFallbackCount, wgcAncientSelectionCount, wgcRepeatNoFreshCount, wgcRepeatPolicyHoldCount,
                wgcCoverageRepeatHoldCount, wgcCoverageDelayTicksCurrent, wgcRepeatTimerLateCount, wgcRepeatCatchupCount,
                tsRegress - lastTimestampRegressionCount,
                tsStall - lastTimestampStallCount, timerRebases, invalidMeta - lastInvalidMetaCount,
                invalidHandle - lastInvalidHandleCount, packetClamps - lastPacketClampCount,
                negativePts - lastNegativePtsCount, nonMonotonicPts - lastNonMonotonicPtsCount,
                g_WgcAdaptiveTargetFps.load(std::memory_order_relaxed), wgcAdaptiveThrottleAdjustments, overloadFlags,
                (muxQueueBytes + 1023u) / 1024u, muxQueuePackets, muxBackpressureCount, muxBackpressureWaitUs,
                muxBackpressureMaxWaitUs, smoothedEncodeMs, encoderBudgetUtilizationPermille, sustainableOutputFps,
                encoderTooSlowForTarget ? 1 : 0, g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ? 1 : 0,
                srcFpsX100Val / 100.0, srcJitterUsVal, dupTsPerSec, smoothedEncCycleMs, encodeSpikeCountThisSecond);

            static uint64_t s_lastWgcCapacityWarnTick = 0;
            static uint32_t s_wgcCapacityLimitedStreakSeconds = 0;
            if (useScreenGrab && recordingOutputLive) {
                const bool encoderPressure = g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ||
                                             (overloadFlags & 0x1u) != 0 || smoothedEncodeMs >= frameIntervalMs;
                const bool muxPressure = (overloadFlags & 0x2u) != 0 || muxBackpressureWaitUs > 0;
                const bool capacityLimitedThisSecond =
                    (encoderPressure || muxPressure) && (outputShortfallTicks > 0 || oldestBufferedFrameAgeUs > 0);
                s_wgcCapacityLimitedStreakSeconds =
                    capacityLimitedThisSecond ? (s_wgcCapacityLimitedStreakSeconds + 1) : 0;
                const uint64_t nowTick = GetTickCount64();
                if ((encoderPressure || muxPressure) && (nowTick - s_lastWgcCapacityWarnTick) >= 5000) {
                    const char* limiter = encoderPressure && muxPressure ? "encoder+mux"
                                          : encoderPressure              ? "encoder"
                                                                         : "mux";
                    const char* warningPrefix =
                        encoderTooSlowForTarget ? "Encoder cannot sustain target" : "Output limited";
                    LogWarn(
                        "[WGC CFR] %s (%s): target=%ufps sustain=%.1ffps encode=%.2fms budget=%.2fms util=%upm "
                        "shortfall=%u/%.1fms oldest=%.1fms streak=%us muxQ=%uKB/%u muxWait=%uus noFresh=%upm",
                        warningPrefix, limiter, outputFps, sustainableOutputFps, smoothedEncodeMs, frameIntervalMs,
                        encoderBudgetUtilizationPermille, outputShortfallTicks, shortfallDurationMs,
                        oldestBufferedFrameAgeMs, s_wgcCapacityLimitedStreakSeconds, (muxQueueBytes + 1023u) / 1024u,
                        muxQueuePackets, muxBackpressureWaitUs, wgcNoFreshTickPermille);
                    s_lastWgcCapacityWarnTick = nowTick;
                }
            } else {
                s_wgcCapacityLimitedStreakSeconds = 0;
            }

            lastDuplicateReasonNoSource = dupNoSource;
            lastDuplicateReasonDeferred = dupDeferred;
            lastDuplicateReasonTimerRebase = dupTimer;
            lastDuplicateReasonDrain = dupDrain;
            lastInvalidMetaCount = invalidMeta;
            lastInvalidHandleCount = invalidHandle;
            lastTimestampRegressionCount = tsRegress;
            lastTimestampStallCount = tsStall;
            lastPacketClampCount = packetClamps;
            lastNegativePtsCount = negativePts;
            lastNonMonotonicPtsCount = nonMonotonicPts;
            cadenceCounters.Reset();
            cadenceCounters.holdTicksRunning = savedHoldTicks;  // Preserve in-progress hold run
            wgcSelectionErrorAccumUs = 0;
            wgcSelectionErrorSignedAccumUs = 0;
            wgcSelectionErrorSamples = 0;
            wgcSelectionErrorMaxUs = 0;
            wgcSelectionEarlyMaxUs = 0;
            wgcSelectionLateMaxUs = 0;
            wgcHoldForNextTickCount = 0;
            wgcHeldFreshFrameTickCount = 0;
            cfrCatchupTicksExecuted = 0;
            wgcFreshCatchupCount = 0;
            wgcReserveSpendTickCount = 0;
            wgcAdaptiveThrottleAdjustments = 0;
            wgcNoFreshTickCount = 0;
            wgcQueueTickSampleCount = 0;
            wgcNoFreshTickPermille = 0;
            wgcBufferedAtTickSum = 0;
            wgcBufferedAtTickMin = UINT32_MAX;
            wgcNoReserveTickCount = 0;
            wgcAncientSelectionCount = 0;
            wgcFreshSelectionMissCount = 0;
            wgcStaleUniqueFallbackCount = 0;
            wgcRepeatNoFreshCount = 0;
            wgcRepeatPolicyHoldCount = 0;
            wgcCoverageRepeatHoldCount = 0;
            wgcCoverageDelayTicksCurrent = 0;
            wgcRepeatTimerLateCount = 0;
            wgcRepeatCatchupCount = 0;
            wgcSelectFreshCount = 0;
            wgcSelectDuplicateSourceCount = 0;
            wgcDropObsoleteCount = 0;
            lastHealthLog = GetTickCount();
        }
    }

    if (hTimer) {
        CloseHandle(hTimer);
    }

    if (!bufferedWgcFrames.empty()) {
        ClearBufferedWgcFrames();
    }

    SetCapturePipelinePhase(CapturePipelinePhase::kIdle);

    LogInfo("[EncoderThread] Stopped");
}

void StartRecording(const AppConfig& config) {
    if (g_Recording)
        return;

    LogInfo("[Media] Starting recording...");

    const bool useScreenGrab = IsPreferredScreenGrab();
    SetActiveScreenGrab(useScreenGrab);

    // Clear any stale shared memory commands/state from previous (possibly crashed)
    // recording sessions. If a previous media process crashed, cmdStopRecording
    // may still be true, causing the new recording to stop immediately.
    if (g_pSharedMem) {
        StoreRelease(g_pSharedMem->runtimeState.cmdStartRecording, false);
        StoreRelease(g_pSharedMem->runtimeState.cmdStopRecording, false);
        SetRecordingVisibleState(false);
    }

    // Reset inject session state so main loop re-initializes on new recording
    g_InjectSessionReset.store(true, std::memory_order_release);

    StopWgcCapturePipeline();
    StopInjectCapturePipeline();
    ResetInjectFrameRingToLatest("recording start");

    g_FrameQueue.Clear();
    ResetLastQueuedFrameCache();
    g_InjectDeliveredFirstFrame.store(false, std::memory_order_release);
    g_RejectInjectFrames.store(false, std::memory_order_release);
    g_IsEncoderBottlenecked.store(false, std::memory_order_relaxed);
    g_InjectBufferedTrimmedFrames.store(0, std::memory_order_relaxed);
    g_InjectCadenceDroppedFrames.store(0, std::memory_order_relaxed);
    g_InjectDeferredFrames.store(0, std::memory_order_relaxed);

    if (g_pSharedMem) {
        ResetRuntimeDiagnostics(g_pSharedMem);
        g_pSharedMem->encoderQueueDepth.store(0, std::memory_order_relaxed);
        g_pSharedMem->throttleCapture.store(false, std::memory_order_release);
    }

    SetCaptureRequestedState(true);

    if (!MediaEngine_StartRecording || !MediaEngine_StartRecording()) {
        LogError("[Media] Failed to start MediaEngine recording");
        SetCaptureRequestedState(false);
        SetRecordingVisibleState(false);
        return;
    }

    g_Recording = true;
    g_EncoderRunning = true;
    g_RecordingUsesVfr.store(config.video.useVFR, std::memory_order_release);
    g_DrainOutstandingWgcTicks.store(false, std::memory_order_release);
    g_WgcDrainStopQpc.store(0, std::memory_order_release);

    g_EncoderThread = std::thread(EncoderThreadFunc, std::ref(config));
    SetThreadPriority(reinterpret_cast<HANDLE>(g_EncoderThread.native_handle()), THREAD_PRIORITY_HIGHEST);

    if (useScreenGrab && g_WgcCap) {
        if (!StartWgcRecordingCapture(config)) {
            LogError("[Media] Failed to start WGC capture");
            g_EncoderRunning = false;
            JoinThreadWithTimeout(g_EncoderThread, 10000, "encoder");
            g_Recording = false;
            SetCaptureRequestedState(false);
            SetRecordingVisibleState(false);
            MediaEngine_StopRecording();
            SetActiveScreenGrab(false);
            return;
        }
        LogInfo("[Media] Active recording path: WGC direct callback (source cadence -> %d fps output)",
                config.video.fps);
    } else if (!useScreenGrab) {
        if (config.captureMethod == "auto" && g_WgcCap && g_AutoWgcFallbackArmed.load(std::memory_order_acquire)) {
            LogInfo("[Media] Active recording path: inject shared-memory capture (WGC auto-fallback armed)");
        } else {
            LogInfo("[Media] Active recording path: inject shared-memory capture");
        }
        g_InjectCaptureShutdown = false;
        g_InjectCaptureThread = std::thread(InjectCaptureThreadFunc, std::ref(config));
        SetThreadPriority(reinterpret_cast<HANDLE>(g_InjectCaptureThread.native_handle()), THREAD_PRIORITY_HIGHEST);
    }

    LogInfo("[Media] Recording warmup armed");
}

void StopRecording() {
    if (!g_Recording)
        return;

    LogInfo("[Media] Stopping recording...");

    const bool immediateWgcStop = IsActiveScreenGrab() && !g_RecordingUsesVfr.load(std::memory_order_acquire);
    const bool drainOutstandingWgcTicks = false;

    g_Recording = false;
    SetCaptureRequestedState(false);
    SetRecordingVisibleState(false);
    SetCapturePipelinePhase(CapturePipelinePhase::kStopping);
    g_WgcDrainStopQpc.store(0, std::memory_order_release);
    g_DrainOutstandingWgcTicks.store(drainOutstandingWgcTicks, std::memory_order_release);
    if (immediateWgcStop) {
        LogInfo("[Media] WGC immediate stop policy active - skipping stop drain so finalization completes immediately");
    }

    StopWgcCapturePipeline();
    StopInjectCapturePipeline();

    g_EncoderRunning = false;
    g_InjectDeliveredFirstFrame.store(false, std::memory_order_release);
    g_RejectInjectFrames.store(false, std::memory_order_release);
    g_AutoWgcFallbackArmed.store(false, std::memory_order_release);

    JoinThreadWithTimeout(g_EncoderThread, 60000, "encoder");

    g_FrameQueue.Clear();
    ResetLastQueuedFrameCache();
    ResetInjectFrameRingToLatest("recording stop");

    if (g_pSharedMem) {
        // Recording has stopped, so zero-copy encoder textures must not stay
        // live. Clear the handshake before stopping the encoder so the DLL
        // tears down all preserved D3D11/KMT resources immediately.
        g_pSharedMem->useEncoderTextures.store(false, std::memory_order_release);
        g_pSharedMem->encoderTextures.ready.store(false, std::memory_order_release);
        g_pSharedMem->encoderTextures.kmtReady.store(false, std::memory_order_release);
    }

    MediaEngine_StopRecording();
    if (MediaEngine_ReleaseEncoderTextures) {
        MediaEngine_ReleaseEncoderTextures();
    }
    // Reset WGC-specific 10-bit hint so inject-mode recordings don't inherit it.
    if (MediaEngine_SetSourcePrefers10Bit) {
        MediaEngine_SetSourcePrefers10Bit(false);
    }
    g_IsEncoderBottlenecked.store(false, std::memory_order_relaxed);

    if (g_pSharedMem) {
        ResetRuntimeDiagnostics(g_pSharedMem);
        g_pSharedMem->encoderQueueDepth.store(0, std::memory_order_relaxed);
        g_pSharedMem->throttleCapture.store(false, std::memory_order_release);
    }

    g_DrainOutstandingWgcTicks.store(false, std::memory_order_release);
    g_WgcDrainStopQpc.store(0, std::memory_order_release);
    g_RecordingUsesVfr.store(false, std::memory_order_release);
    SetActiveScreenGrab(false);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    LogInfo("[Media] Recording stopped");
}

int MediaProcessMain(const AppConfig& initialConfig) {
    AppConfig config = initialConfig;
    timeBeginPeriod(1);
    SetConsoleCtrlHandler(MediaConsoleHandler, TRUE);

    // Get exe directory for DLL loading
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string exeDir = std::string(exePath);
    exeDir = exeDir.substr(0, exeDir.find_last_of("\\/"));
    const std::string configPath = GetLocalConfigPath();
    bool mediaEngineReady = false;

    auto unloadMediaEngineIdle = [&]() {
        if (mediaEngineReady && MediaEngine_Shutdown) {
            MediaEngine_Shutdown();
            mediaEngineReady = false;
        }
        MediaEngine_Unload();
        LogInfo("[Media] MediaEngine unloaded for idle state");
    };

    auto ensureMediaEngineReady = [&]() -> bool {
        if (mediaEngineReady) {
            return true;
        }

        if (!MediaEngine_Load(exeDir.c_str())) {
            LogError("[Media] Failed to load mediaengine.dll");
            return false;
        }

        MediaEngine_SetLogCallback(config.debugLogging ? MediaLogCallback : nullptr);
        if (!MediaEngine_Init(&config)) {
            LogError("[Media] Failed to initialize MediaEngine");
            if (MediaEngine_Shutdown) {
                MediaEngine_Shutdown();
            }
            MediaEngine_Unload();
            return false;
        }

        if (g_pSharedMem || g_pShmem) {
            MediaEngine_SetSharedMem(g_pSharedMem, g_pShmem);
        }

        mediaEngineReady = true;
        LogInfo("[Media] MediaEngine initialized");
        return true;
    };

    ApplyMediaProcessPriority(config);

    ProcessIPCServer ipc(ProcessMode::Media);
    if (!ipc.Init()) {
        LogError("[Media] Failed to initialize IPC");
        timeEndPeriod(1);
        return 1;
    }

    if (!ensureMediaEngineReady()) {
        timeEndPeriod(1);
        return 1;
    }

    LogInfo("[Media] SharedMemory Layout Check:");
    LogInfo("[Media] sizeof(FrameSlot) = %zu", sizeof(FrameSlot));
    LogInfo("[Media] sizeof(CaptureState) = %zu", sizeof(CaptureState));
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    LogInfo("[Media] offsetof(frameRing) = %zu", offsetof(SharedMemoryLayout, frameRing));
    LogInfo("[Media] offsetof(runtimeState) = %zu", offsetof(SharedMemoryLayout, runtimeState));
#pragma GCC diagnostic pop

    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;

    bool explicitScreengrab = (config.captureMethod == "screengrab" || config.captureMethod == "framegrab" ||
                               config.captureMethod == "desktop_dup");
    if (explicitScreengrab) {
        SetPreferredScreenGrab(true);
        LogInfo("[Media] Using screengrab mode (explicit)");
    }

    LogInfo("[Media] Attempting to connect to shared memory...");

    for (int retry = 0; retry < 10 && !g_pSharedMem; retry++) {
        HANDLE hDiscovery = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
        if (hDiscovery) {
            DiscoveryInfo* pDiscovery =
                (DiscoveryInfo*)MapViewOfFile(hDiscovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));

            if (pDiscovery && pDiscovery->magic == DISCOVERY_MAGIC && pDiscovery->injectPid.load() != 0) {
                wchar_t sharedMemName[64];
                GenerateSharedMemName(sharedMemName, 64, pDiscovery->injectPid.load());

                g_hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sharedMemName);
                if (g_hMapFile) {
                    g_pSharedMem = (SharedMemoryLayout*)MapViewOfFile(g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0,
                                                                      sizeof(SharedMemoryLayout));

                    if (g_pSharedMem && g_pSharedMem->GetHostPID() != 0) {
                        LogInfo("[Media] Connected via discovery (inject PID: %u)", pDiscovery->injectPid.load());
                        UnmapViewOfFile(pDiscovery);
                        CloseHandle(hDiscovery);
                        break;
                    }

                    if (g_pSharedMem) {
                        UnmapViewOfFile(g_pSharedMem);
                        g_pSharedMem = nullptr;
                    }
                    CloseHandle(g_hMapFile);
                    g_hMapFile = NULL;
                }
                UnmapViewOfFile(pDiscovery);
            }
            CloseHandle(hDiscovery);
        }

        if (!g_pSharedMem) {
            Sleep(50);
        }
    }

    if (g_pSharedMem) {
        if (g_pSharedMem->GetShmemMappingCreated()) {
            wchar_t shmemName[64];
            GenerateShmemName(shmemName, 64, g_pSharedMem->GetHostPID());
            g_hMapShmem = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmemName);
            if (g_hMapShmem) {
                size_t mapSize = g_pSharedMem->GetShmemMappingSize();
                g_pShmem = (ShmemBuffer*)MapViewOfFile(g_hMapShmem, FILE_MAP_ALL_ACCESS, 0, 0, mapSize);
                if (g_pShmem) {
                    LogInfo("[Media] Connected to separate Shmem mapping '%ls' (mapped %zu bytes)", shmemName, mapSize);
                }
            }
        }

        if (mediaEngineReady) {
            MediaEngine_SetSharedMem(g_pSharedMem, g_pShmem);
        }

        if (explicitScreengrab) {
            SetPreferredScreenGrab(true);
            LogInfo("[Media] Connected to shared memory - using screengrab for capture");
        } else {
            SetPreferredScreenGrab(false);
            LogInfo("[Media] Connected to shared memory - using inject mode");
        }
    } else if (config.captureMethod == "inject") {
        LogError("[Media] Failed to connect to shared memory in inject mode!");
        unloadMediaEngineIdle();
        timeEndPeriod(1);
        return 1;
    } else {
        SetPreferredScreenGrab(true);
        LogInfo("[Media] Shared memory not available - using screengrab mode");
    }

    if (IsPreferredScreenGrab() || config.captureMethod == "auto") {
        if (!ensureMediaEngineReady()) {
            timeEndPeriod(1);
            return 1;
        }
        d3dDevice = MediaEngine_GetD3D11Device();
        if (!d3dDevice) {
            if (IsPreferredScreenGrab()) {
                LogError("[Media] Failed to get D3D11 device");
                unloadMediaEngineIdle();
                timeEndPeriod(1);
                return 1;
            }
        } else {
            d3dDevice->GetImmediateContext(&d3dContext);

            if (WGCCapture::IsSupported()) {
                g_WgcCap = std::make_unique<WGCCapture>();
                if (g_WgcCap->Init(d3dDevice)) {
                    // Connect encoder bottleneck flag to WGC for throttle
                    g_WgcCap->SetThrottleFlag(&g_IsEncoderBottlenecked);
                    LogInfo("[Media] WGC support initialized%s",
                            IsPreferredScreenGrab() ? "" : " (standby for auto fallback)");
                } else {
                    if (IsPreferredScreenGrab()) {
                        LogError("[Media] WGC capture init failed");
                        unloadMediaEngineIdle();
                        timeEndPeriod(1);
                        return 1;
                    } else {
                        LogInfo("[Media] WGC init failed - inject mode only");
                        g_WgcCap.reset();
                    }
                }
            }
        }
    }

    LogInfo("[Media] Process started (PID: %lu) Mode: %s", GetCurrentProcessId(),
            IsPreferredScreenGrab() ? "screengrab" : "inject");
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    LARGE_INTEGER qpcFreq;
    QueryPerformanceFrequency(&qpcFreq);
    int64_t recordingStartTime = 0;
    auto ensureWgcDevice = [&]() -> bool {
        if (!ensureMediaEngineReady()) {
            return false;
        }
        if (d3dDevice) {
            return true;
        }
        d3dDevice = MediaEngine_GetD3D11Device();
        if (!d3dDevice) {
            return false;
        }
        if (!d3dContext) {
            d3dDevice->GetImmediateContext(&d3dContext);
        }
        return true;
    };
    auto ensureWgcStandby = [&]() -> bool {
        if (g_WgcCap) {
            g_WgcCap->SetCaptureCursor(config.video.captureCursor);
            g_WgcCap->SetThrottleFlag(&g_IsEncoderBottlenecked);
            return true;
        }
        if (!ensureWgcDevice()) {
            return false;
        }
        g_WgcCap = std::make_unique<WGCCapture>();
        if (!g_WgcCap->Init(d3dDevice)) {
            g_WgcCap.reset();
            return false;
        }
        g_WgcCap->SetCaptureCursor(config.video.captureCursor);
        g_WgcCap->SetThrottleFlag(&g_IsEncoderBottlenecked);
        return true;
    };
    auto releaseIdleWgcResources = [&]() {
        StopWgcCapturePipeline();
        g_WgcCap.reset();
        if (d3dContext) {
            d3dContext->ClearState();
            d3dContext->Flush();
            d3dContext->Release();
            d3dContext = nullptr;
        }
        d3dDevice = nullptr;
        if (MediaEngine_ReleaseSharedD3D11Device) {
            MediaEngine_ReleaseSharedD3D11Device();
        }
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        LogInfo("[Media] Released idle WGC D3D11 resources");
    };
    auto isExplicitScreenGrabConfig = [&]() -> bool {
        return config.captureMethod == "screengrab" || config.captureMethod == "framegrab" ||
               config.captureMethod == "desktop_dup";
    };

    DWORD lastEarlyWgcScan = 0;
    DWORD lastWindowScanTime = 0;
    HWND currentCapturedWindow = NULL;
    bool currentTargetPrefersInject = false;
    uint32_t lastSourcePid = 0;
    uint32_t activeConfigSourcePid = 0;
    std::string activeConfigProcessName;

    auto clearCurrentWgcTarget = [&]() {
        currentCapturedWindow = NULL;
        currentTargetPrefersInject = false;
    };

    auto refreshActiveConfig = [&](bool forceReload) -> std::string {
        uint32_t sourcePid = 0;
        std::string processName;
        if (g_pSharedMem) {
            sourcePid = g_pSharedMem->GetSourcePid();
            if (sourcePid != 0) {
                processName = GetProcessNameFromPID(sourcePid);
                if (processName == "unknown") {
                    processName.clear();
                }
            }
        }

        if (!forceReload && sourcePid == activeConfigSourcePid && processName == activeConfigProcessName) {
            return processName;
        }

        AppConfig resolvedConfig;
        LoadConfig(configPath, resolvedConfig);
        if (!processName.empty()) {
            LoadConfig(configPath, resolvedConfig, processName);
        }

        const bool mediaConfigChanged = !MediaEngineConfigEquals(config, resolvedConfig);

        config = std::move(resolvedConfig);
        activeConfigSourcePid = sourcePid;
        activeConfigProcessName = processName;

        ApplyMediaProcessPriority(config);
        if (g_WgcCap) {
            g_WgcCap->SetCaptureCursor(config.video.captureCursor);
        }
        if (mediaEngineReady) {
            MediaEngine_SetLogCallback(config.debugLogging ? MediaLogCallback : nullptr);
            if (forceReload || mediaConfigChanged) {
                MediaEngine_ReloadConfig(&config);
            }
            if (g_pSharedMem || g_pShmem) {
                MediaEngine_SetSharedMem(g_pSharedMem, g_pShmem);
            }
        }
        return processName;
    };

    auto markInjectPreferredTarget = [&](HWND targetWindow, uint32_t sourcePid, const char* reason) -> bool {
        if (!ShouldPreferInjectCaptureForFullscreenWindow(targetWindow, sourcePid)) {
            return false;
        }

        if (currentTargetPrefersInject && currentCapturedWindow == targetWindow) {
            SetPreferredScreenGrab(false);
            return true;
        }

        currentCapturedWindow = targetWindow;
        currentTargetPrefersInject = true;
        SetPreferredScreenGrab(false);
        LogInfo("[Media] %s: hooked fullscreen-like window 0x%p will use inject capture instead of WGC", reason,
                targetWindow);
        return true;
    };

    auto primeWgcMonitorTarget = [&]() -> bool {
        if (currentCapturedWindow == NULL && !currentTargetPrefersInject && g_WgcCap) {
            g_WgcCap->SetCaptureCursor(config.video.captureCursor);
            g_WgcCap->SetThrottleFlag(&g_IsEncoderBottlenecked);
            SetPreferredScreenGrab(true);
            return true;
        }

        if (!ensureWgcDevice()) {
            return false;
        }

        g_WgcCap.reset();
        g_WgcCap = std::make_unique<WGCCapture>();
        if (!g_WgcCap->Init(d3dDevice)) {
            g_WgcCap.reset();
            return false;
        }

        g_WgcCap->SetCaptureCursor(config.video.captureCursor);
        g_WgcCap->SetThrottleFlag(&g_IsEncoderBottlenecked);
        SetPreferredScreenGrab(true);
        currentCapturedWindow = NULL;
        currentTargetPrefersInject = false;
        return true;
    };

    auto primeWgcWindowTarget = [&](HWND targetWindow, bool logPrimed) -> bool {
        if (!targetWindow) {
            return false;
        }

        if (currentCapturedWindow == targetWindow && g_WgcCap && !currentTargetPrefersInject) {
            g_WgcCap->SetCaptureCursor(config.video.captureCursor);
            g_WgcCap->SetThrottleFlag(&g_IsEncoderBottlenecked);
            SetPreferredScreenGrab(true);
            return true;
        }

        if (!ensureWgcDevice()) {
            SetPreferredScreenGrab(false);
            return false;
        }

        g_WgcCap.reset();
        g_WgcCap = std::make_unique<WGCCapture>();
        if (g_WgcCap->InitForWindow(d3dDevice, targetWindow)) {
            g_WgcCap->SetCaptureCursor(config.video.captureCursor);
            g_WgcCap->SetThrottleFlag(&g_IsEncoderBottlenecked);
            SetPreferredScreenGrab(true);
            currentCapturedWindow = targetWindow;
            currentTargetPrefersInject = false;
            if (logPrimed) {
                LogInfo("[Media] WGC target primed for window 0x%p", targetWindow);
            }
            return true;
        }

        LogError("[Media] Failed to init WGC for found window.");
        if (!primeWgcMonitorTarget()) {
            SetPreferredScreenGrab(false);
            clearCurrentWgcTarget();
            return false;
        }
        return true;
    };

    auto prepareCaptureForRecordingStart = [&]() {
        const std::string processName = refreshActiveConfig(false);
        const uint32_t sourcePid = g_pSharedMem ? g_pSharedMem->GetSourcePid() : 0;
        const bool injectWhitelisted = !processName.empty() && MatchesProcessEntries(config.gameWhitelist, processName);

        g_AutoWgcFallbackArmed.store(false, std::memory_order_release);

        if (!config.wgcWindowTitles.empty()) {
            HWND matchedWindow = FindMatchingWgcWindow(config.wgcWindowTitles);
            if (matchedWindow) {
                if (markInjectPreferredTarget(matchedWindow, sourcePid, "WGC title match")) {
                    return;
                }
                primeWgcWindowTarget(matchedWindow, false);
                return;
            }
        }

        if (sourcePid != 0 && MatchesProcessEntries(config.overlayWhitelist, processName)) {
            if (!ensureWgcDevice()) {
                LogWarn("[Media] Overlay whitelist requested WGC but D3D11 device unavailable");
                SetPreferredScreenGrab(false);
                clearCurrentWgcTarget();
                return;
            }

            SetPreferredScreenGrab(true);
            HWND hGameWindow = GetMainWindowForProcess(sourcePid);
            if (hGameWindow) {
                if (markInjectPreferredTarget(hGameWindow, sourcePid, "Overlay whitelist")) {
                    return;
                }
                primeWgcWindowTarget(hGameWindow, false);
            } else if (!primeWgcMonitorTarget()) {
                SetPreferredScreenGrab(false);
                clearCurrentWgcTarget();
            }
            return;
        }

        if (isExplicitScreenGrabConfig()) {
            if (!primeWgcMonitorTarget()) {
                SetPreferredScreenGrab(false);
                clearCurrentWgcTarget();
            }
            return;
        }

        if (currentCapturedWindow != NULL && !IsWindow(currentCapturedWindow)) {
            clearCurrentWgcTarget();
        }

        if (!isExplicitScreenGrabConfig()) {
            SetPreferredScreenGrab(false);
        }

        if (injectWhitelisted) {
            LogInfo("[Media] Injection whitelist matched %s; auto mode will stay on inject capture",
                    processName.c_str());
            return;
        }

        if (config.captureMethod == "auto" && WGCCapture::IsSupported()) {
            if (g_WgcCap || ensureWgcStandby()) {
                g_AutoWgcFallbackArmed.store(true, std::memory_order_release);
                LogInfo("[Media] WGC fallback armed for this recording");
            } else {
                LogWarn("[Media] Failed to arm WGC fallback for this recording");
            }
        }
    };

    while (g_Running) {
        // WGC window detection must run BEFORE resolution polling/texture creation.
        // CreateSharedCaptureTextures sets the encoder's LUID device, which conflicts
        // with WGC's shared device. By scanning first, the preferred capture mode is set correctly
        // and we skip the LUID-based texture creation for WGC games.
        if (mediaEngineReady && g_pSharedMem && !config.wgcWindowTitles.empty() && !IsPreferredScreenGrab() &&
            !g_Recording) {
            DWORD now = GetTickCount();
            if (now - lastEarlyWgcScan > 500) {
                lastEarlyWgcScan = now;
                HWND matchedWindow = FindMatchingWgcWindow(config.wgcWindowTitles);
                if (matchedWindow) {
                    uint32_t sourcePid = g_pSharedMem->GetSourcePid();
                    if (!markInjectPreferredTarget(matchedWindow, sourcePid, "Early WGC scan")) {
                        primeWgcWindowTarget(matchedWindow, false);
                    }
                }
            }
        }

        // Poll for resolution availability and create encoder textures early.
        // The Vulkan layer sets resolution when it creates the swapchain, then waits
        // for encoder KMT textures. We must create them ASAP to avoid timeout.
        // Skip when using screengrab (WGC) - the encoder should use the shared device.
        if (g_Recording && g_pSharedMem && !IsPreferredScreenGrab() &&
            !g_pSharedMem->encoderTextures.kmtReady.load(std::memory_order_acquire)) {
            uint32_t w = g_pSharedMem->GetWidth();
            uint32_t h = g_pSharedMem->GetHeight();
            uint32_t f = g_pSharedMem->GetFormat();
            if (w > 0 && h > 0) {
                LogInfo("[Media] Resolution available (%dx%d fmt=%d), creating encoder textures", w, h, f);
                if (MediaEngine_CreateSharedCaptureTextures(w, h, f, g_pSharedMem)) {
                    LogInfo("[Media] Encoder KMT textures created (main loop)");
                }
            }
        }

        ProcessCommand cmd;
        if (ipc.PollCommand(cmd)) {
            switch (cmd) {
                case ProcessCommand::Shutdown:
                    LogInfo("[Media] Shutdown command received");
                    g_Running = false;
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
                case ProcessCommand::StartRecording:
                    if (!ensureMediaEngineReady()) {
                        LogError("[Media] Failed to reinitialize MediaEngine for recording start");
                        ipc.SendResponse(ProcessResponse::Ack);
                        break;
                    }
                    prepareCaptureForRecordingStart();
                    StartRecording(config);
                    ipc.SendResponse(ProcessResponse::RecordingStarted);
                    break;
                case ProcessCommand::StopRecording:
                    StopRecording();
                    releaseIdleWgcResources();
                    ipc.SendResponse(ProcessResponse::RecordingStopped);
                    // Exit after recording stops to free GPU VRAM.
                    // Controller respawns on next recording via EnsureMediaProcessReady.
                    LogInfo("[Media] Recording finished, exiting to release GPU resources");
                    g_Running = false;
                    break;
                case ProcessCommand::Ping:
                    ipc.SendResponse(ProcessResponse::Pong);
                    break;
                case ProcessCommand::ReloadConfig: {
                    refreshActiveConfig(true);
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
                }
                default:
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
            }
        }

        if (g_pSharedMem) {
            if (LoadAcquire(g_pSharedMem->runtimeState.cmdStartRecording)) {
                StoreRelease(g_pSharedMem->runtimeState.cmdStartRecording, false);
                if (!g_Recording) {
                    if (!ensureMediaEngineReady()) {
                        LogError("[Media] Failed to reinitialize MediaEngine for shared-memory recording start");
                    } else {
                        prepareCaptureForRecordingStart();
                        StartRecording(config);
                        g_pSharedMem->runtimeState.ackRecordingStarted.store(true, std::memory_order_release);
                    }
                }
            }
            if (LoadAcquire(g_pSharedMem->runtimeState.cmdStopRecording)) {
                StoreRelease(g_pSharedMem->runtimeState.cmdStopRecording, false);
                if (g_Recording) {
                    StopRecording();
                    releaseIdleWgcResources();
                    g_pSharedMem->runtimeState.ackRecordingStopped.store(true, std::memory_order_release);
                }
                // Exit after recording stops to free GPU VRAM.
                LogInfo("[Media] Recording finished (shmem), exiting to release GPU resources");
                g_Running = false;
            }

            DWORD now = GetTickCount();
            if (!g_Recording && !config.wgcWindowTitles.empty() && (now - lastWindowScanTime > 1000)) {
                lastWindowScanTime = now;
                HWND foundWindow = FindMatchingWgcWindow(config.wgcWindowTitles);

                if (foundWindow) {
                    if (foundWindow != currentCapturedWindow) {
                        LogInfo(
                            "[Media] WGC Trigger: Found window (0x%p) matching config. "
                            "Switching capture...",
                            foundWindow);
                    }
                    if (markInjectPreferredTarget(foundWindow, g_pSharedMem->GetSourcePid(), "WGC trigger")) {
                        continue;
                    }
                    if (!primeWgcWindowTarget(foundWindow, foundWindow != currentCapturedWindow) &&
                        !ensureWgcDevice()) {
                        LogWarn("[Media] WGC trigger ignored: D3D11 device unavailable");
                    }
                } else if (!foundWindow && currentCapturedWindow != NULL) {
                    if (!IsWindow(currentCapturedWindow)) {
                        LogInfo(
                            "[Media] Captured window 0x%p no longer valid. Reverting "
                            "to monitor/inject.",
                            currentCapturedWindow);
                        clearCurrentWgcTarget();
                        if (!primeWgcMonitorTarget()) {
                            SetPreferredScreenGrab(false);
                        }
                    }
                }
            }

            uint32_t currentSourcePid = g_pSharedMem->GetSourcePid();

            if (currentSourcePid != 0 && currentSourcePid != lastSourcePid) {
                lastSourcePid = currentSourcePid;
                std::string procName = refreshActiveConfig(false);
                if (procName.empty()) {
                    procName = GetProcessNameFromPID(currentSourcePid);
                }
                LogInfo("[Media] Hook connected: %s (PID: %u)", procName.c_str(), currentSourcePid);

                const bool forceWGC = MatchesProcessEntries(config.overlayWhitelist, procName);
                const bool injectWhitelisted = MatchesProcessEntries(config.gameWhitelist, procName);
                if (forceWGC) {
                    LogInfo("[Media] Overlay whitelist matched %s; preferring WGC when the target allows it",
                            procName.c_str());
                    if (g_Recording && !IsActiveScreenGrab()) {
                        LogInfo(
                            "[Media] Current recording stays on inject; WGC remains armed "
                            "only as a startup fallback");
                    }
                }

                if (g_Recording && !IsActiveScreenGrab() && injectWhitelisted &&
                    g_AutoWgcFallbackArmed.exchange(false, std::memory_order_acq_rel)) {
                    LogInfo("[Media] Injection whitelist matched %s; disarming WGC startup fallback for this recording",
                            procName.c_str());
                }

                if (!g_Recording && forceWGC) {
                    HWND matchedWindow =
                        config.wgcWindowTitles.empty() ? NULL : FindMatchingWgcWindow(config.wgcWindowTitles);
                    if (matchedWindow) {
                        if (markInjectPreferredTarget(matchedWindow, currentSourcePid,
                                                      "Overlay whitelist title match")) {
                            continue;
                        }
                        primeWgcWindowTarget(matchedWindow, false);
                        continue;
                    }

                    if (!ensureWgcDevice()) {
                        LogWarn("[Media] Overlay whitelist requested WGC but D3D11 device unavailable");
                        SetPreferredScreenGrab(false);
                        clearCurrentWgcTarget();
                    } else {
                        SetPreferredScreenGrab(true);

                        HWND hGameWindow = GetMainWindowForProcess(currentSourcePid);
                        if (hGameWindow) {
                            if (markInjectPreferredTarget(hGameWindow, currentSourcePid,
                                                          "Overlay whitelist main window")) {
                                continue;
                            }
                            LogInfo(
                                "[Media] Whitelist Optimization: Found main window 0x%p. "
                                "Switching WGC to Window Mode.",
                                hGameWindow);

                            if (primeWgcWindowTarget(hGameWindow, false)) {
                                LogInfo("[Media] WGC window target primed for PID %u", currentSourcePid);
                            } else {
                                LogError("[Media] Failed to init WGC for window - falling back to Monitor");
                            }
                        } else {
                            LogInfo(
                                "[Media] Whitelist: No main window found for PID %u. Using "
                                "Monitor Capture.",
                                currentSourcePid);
                            if (!primeWgcMonitorTarget()) {
                                SetPreferredScreenGrab(false);
                            }
                        }
                    }

                } else if (!g_Recording && !isExplicitScreenGrabConfig()) {
                    SetPreferredScreenGrab(false);
                    LogInfo("[Media] Using Inject Mode (Default)");
                }
            }
        }

        // Release preserved encoder textures once the hook signals it no longer uses them.
        // The Vulkan layer clears useEncoderTextures in CleanupCapture (vkDestroyDevice).
        if (!g_Recording && g_pSharedMem && MediaEngine_ReleaseEncoderTextures) {
            static bool lastUseEncoderTextures = false;
            bool curUseEncoderTextures = g_pSharedMem->useEncoderTextures.load(std::memory_order_acquire);
            if (lastUseEncoderTextures && !curUseEncoderTextures) {
                LogInfo("[Media] Game released encoder textures - freeing preserved D3D11/VRAM resources");
                MediaEngine_ReleaseEncoderTextures();
            }
            lastUseEncoderTextures = curUseEncoderTextures;
        }

        bool hasPendingInputs = false;

        if (IsActiveScreenGrab() && g_Recording) {
            if (recordingStartTime == 0) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                recordingStartTime = now.QuadPart;
            }

            if (g_pSharedMem) {
                g_pSharedMem->runtimeState.hostDroppedFrames.store(
                    static_cast<uint32_t>(g_FrameQueue.GetDroppedCount()), std::memory_order_relaxed);
            }
        } else if (!IsActiveScreenGrab() && g_Recording && g_pSharedMem) {
            FrameRingBuffer& ring = g_pSharedMem->frameRing;
            uint32_t wIdx = ring.writeIndex.load(std::memory_order_acquire);
            static uint32_t localReadIdx = 0;
            static bool receivedFirstFrame = false;
            static DWORD injectModeStartTime = 0;
            static bool sessionInitialized = false;

            static bool sharedTexturesCreated = false;

            // Reset session state when a new recording starts
            if (g_InjectSessionReset.exchange(false, std::memory_order_acq_rel)) {
                sessionInitialized = false;
                localReadIdx = 0;
                receivedFirstFrame = false;
                injectModeStartTime = 0;
                sharedTexturesCreated = false;
            }

            if (!sessionInitialized) {
                localReadIdx = wIdx;
                receivedFirstFrame = false;
                injectModeStartTime = 0;
                sessionInitialized = true;
                LogInfo("[Media] Inject mode session initialized, localReadIdx=%u, wIdx=%u", localReadIdx, wIdx);
            }

            static DWORD lastPollLog = 0;
            if (GetTickCount() - lastPollLog > 1000) {
                LogInfo("[Media] Polling: localReadIdx=%u, wIdx=%u", localReadIdx, wIdx);
                lastPollLog = GetTickCount();
            }

            if (injectModeStartTime == 0) {
                injectModeStartTime = GetTickCount();
                receivedFirstFrame = false;
            }

            if (!receivedFirstFrame && g_InjectDeliveredFirstFrame.load(std::memory_order_acquire)) {
                receivedFirstFrame = true;
                LogInfo("[Media] Inject delivery confirmed before monitor observed ring activity");
            }

            if (!receivedFirstFrame && config.captureMethod == "auto" &&
                g_AutoWgcFallbackArmed.load(std::memory_order_acquire) && g_WgcCap) {
                DWORD elapsed = GetTickCount() - injectModeStartTime;
                const uint32_t activeSourcePid = g_pSharedMem ? g_pSharedMem->GetSourcePid() : 0;
                if (ce::capture_policy::ShouldTriggerAutoWgcFallback(
                        receivedFirstFrame, config.captureMethod == "auto",
                        g_AutoWgcFallbackArmed.load(std::memory_order_acquire), g_WgcCap != nullptr, elapsed,
                        activeSourcePid)) {
                    LogInfo(
                        "[Media] No frames from inject mode after %lums - falling "
                        "back to WGC",
                        elapsed);

                    g_RejectInjectFrames.store(true, std::memory_order_release);
                    g_AutoWgcFallbackArmed.store(false, std::memory_order_release);
                    StopInjectCapturePipeline();
                    if (StartWgcRecordingCapture(config)) {
                        SetActiveScreenGrab(true);
                        LogInfo(
                            "[Media] Active recording path switched to WGC direct callback "
                            "(auto fallback from inject)");
                    } else {
                        LogWarn("[Media] WGC fallback failed; inject monitoring remains unavailable");
                    }

                    injectModeStartTime = 0;
                }
            }

            if (!sharedTexturesCreated && g_pSharedMem->GetWidth() > 0 && g_pSharedMem->GetHeight() > 0) {
                if (g_pSharedMem->encoderTextures.ready.load(std::memory_order_acquire)) {
                    sharedTexturesCreated = true;
                }
            }

            if (wIdx > localReadIdx + FRAME_RING_SIZE) {
                uint32_t newReadIdx = wIdx - FRAME_RING_SIZE;
                localReadIdx = newReadIdx;
            }

            if (localReadIdx < wIdx) {
                if (!receivedFirstFrame) {
                    receivedFirstFrame = true;
                    LogInfo("[Media] First frame detected (monitoring)");
                }
                localReadIdx = wIdx;
            }

            hasPendingInputs = false;
        } else {
            recordingStartTime = 0;
        }

        if (g_Recording && (g_FrameQueue.Size() > 0 || hasPendingInputs)) {
            Sleep(1);
        } else {
            Sleep(5);
        }
    }

    StopRecording();

    if (g_WgcCap)
        g_WgcCap->StopCapture();
    if (d3dContext)
        d3dContext->Release();
    d3dDevice = nullptr;

    // Shutdown media engine BEFORE unmapping shared memory to avoid use-after-free
    // (VideoEncoder::CleanupResources accesses pSharedMem during destruction)
    if (mediaEngineReady && MediaEngine_Shutdown) {
        MediaEngine_Shutdown();
        mediaEngineReady = false;
    }
    MediaEngine_Unload();

    if (g_pShmem)
        UnmapViewOfFile(g_pShmem);
    if (g_hMapShmem)
        CloseHandle(g_hMapShmem);

    if (g_pSharedMem)
        UnmapViewOfFile(g_pSharedMem);
    if (g_hMapFile)
        CloseHandle(g_hMapFile);
    timeEndPeriod(1);

    LogInfo("[Media] Process exiting");
    return 0;
}
