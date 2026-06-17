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
#include <cstring>
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
#include "../common/thread_power_throttling_compat.h"
#include "av_sync_calibrator.h"
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
static std::atomic<bool> g_DrainOutstandingCfrTicks{false};
static std::atomic<int64_t> g_CfrDrainStopQpc{0};

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

// Audio-only recording flag (set via IPC or shared memory)
static bool g_AudioOnly = false;

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
static std::atomic<uint64_t> g_ActivePathMismatchFramesDiscarded{0};

struct WgcRetargetRequest {
    HWND window = NULL;
    HMONITOR monitor = NULL;
    bool preferMonitor = false;
    bool active = false;
};

// Forward declaration
void InjectCaptureThreadFunc(const AppConfig& config);
void WgcCaptureThreadFunc(const AppConfig& config);
void StopRecording();
void StartRecording(const AppConfig& config);

// --- Auto-detected render-endpoint audio latency (per-device latency model, Part B) -----------
// Measured once per media process via the WASAPI render->loopback probe (mediaengine export), then
// applied to the render-domain audio sources on every config (re)load. The probe is cached to disk
// per render endpoint, so a fresh process is a cache hit (no calibration sound replay).
static double g_AutoDetectedRenderLatencyMs = -1.0;  // <0 = not measured / unavailable
static bool g_RenderLatencyMeasureAttempted = false;

// Apply the auto-detected render-endpoint latency to render-domain sources (system loopback + app
// process loopback only). No-op when autodetect is off, a manual override is configured, or no
// value has been measured. Microphones (Domain 2) are never touched here. Cheap and idempotent.
static void ApplyAutoDetectedRenderLatencyToConfig(AppConfig& config) {
    if (!config.audioLatencyAutodetect) {
        return;
    }
    if (config.audioCaptureLatencyMs > 0.0f) {
        return;  // manual override; sources already carry it from config parsing
    }
    if (g_AutoDetectedRenderLatencyMs <= 0.0) {
        return;
    }
    const float ms = static_cast<float>(g_AutoDetectedRenderLatencyMs);
    config.audioCaptureLatencyMs = ms;
    int applied = 0;
    for (auto& s : config.audioSources) {
        const bool renderDomain = s.sourceType == AudioConfig::SystemAudio || s.sourceType == AudioConfig::AppAudio;
        if (renderDomain && s.captureLatencyMs == 0.0f) {  // inherited auto default, no per-source override
            s.captureLatencyMs = ms;
            ++applied;
        }
    }
    LogInfo("[Media] Applied auto-detected render-endpoint latency %.3f ms to %d render-domain source(s)", ms, applied);
}

// Perform the one-time audio-vs-video offset measurement. Call only when NOT recording and only
// after MediaEngine_Init (the A/V self-calibration needs the shared D3D11 device): on a cache miss
// it briefly shows a small flashing window + plays a faint near-inaudible marker. Safe to call
// repeatedly; it runs at most once per process and is a cheap disk cache hit otherwise.
//
// Primary: the WGC+loopback A/V self-calibration measures the TRUE offset through CE's real
// pipeline. Fallback (WGC/device unavailable or inconclusive): the audio-only render->loopback
// probe (Start-anchor), which only approximates it.
static void MeasureRenderLatencyOnce(const AppConfig& config, const std::string& cacheDir) {
    if (g_RenderLatencyMeasureAttempted) {
        return;
    }
    if (!config.audioLatencyAutodetect || config.audioCaptureLatencyMs > 0.0f) {
        g_RenderLatencyMeasureAttempted = true;  // disabled or manual override: never measure
        return;
    }
    g_RenderLatencyMeasureAttempted = true;

    // Primary (opt-in, WIP): full A/V self-calibration through the real WGC + loopback pipeline.
    // Off by default while it is hardware-iterated; the audio-only probe below is the active path.
    if (config.audioAvCalibration) {
        ID3D11Device* d3dDevice = MediaEngine_GetD3D11Device ? MediaEngine_GetD3D11Device() : nullptr;
        if (d3dDevice) {
            const ce::avcal::AvCalibrationResult av = ce::avcal::MeasureAvOffset(d3dDevice, cacheDir, false);
            if (av.ok && av.offsetMs > 0.0) {
                g_AutoDetectedRenderLatencyMs = av.offsetMs;
                LogInfo("[Media] Auto-detected A/V content offset: %.3f ms (WGC+loopback self-calibration, %s)",
                        av.offsetMs, av.fromCache ? "cached" : "measured");
                return;
            }
            LogWarn("[Media] A/V self-calibration inconclusive; falling back to audio render->loopback probe");
        } else {
            LogWarn("[Media] no D3D11 device for A/V self-calibration; falling back to audio render->loopback probe");
        }
    }

    // Audio-only render->loopback probe (Start-anchor) - the default active auto-detect.
    double ms = 0.0;
    if (MediaEngine_MeasureRenderEndpointLatency &&
        MediaEngine_MeasureRenderEndpointLatency(cacheDir.c_str(), false, &ms) && ms > 0.0) {
        g_AutoDetectedRenderLatencyMs = ms;
        LogInfo("[Media] Auto-detected render-endpoint audio latency: %.3f ms (render->loopback probe fallback)", ms);
    } else {
        LogWarn("[Media] Audio latency auto-detect unavailable; using configured audio_capture_latency_ms=%.3f",
                static_cast<double>(config.audioCaptureLatencyMs));
    }
}

namespace {
constexpr int kInjectTextureSlotCount = SHARED_TEXTURE_SLOT_COUNT;

// Encoder-bottleneck EMA parameters.  The smoothing factor (alpha) controls
// how quickly the EMA reacts to encode-time changes.  Hysteresis avoids
// bang-bang oscillation: we enter bottleneck at a higher threshold and exit
// at a significantly lower one.
constexpr double kEncodeEmaAlpha = 0.10;
constexpr double kBottleneckEnterRatio = 0.95;  // smoothedEncodeMs > 95% of frame interval → enter
constexpr double kBottleneckExitRatio = 0.75;   // smoothedEncodeMs < 75% of frame interval → exit

// Update g_IsEncoderBottlenecked with hysteresis to prevent rapid toggling.
// During startup we still learn the encode-time EMA, but we keep the
// bottleneck flag cleared so one-time encoder priming doesn't raise false
// overload warnings or skew WGC recovery logic.
inline void UpdateEncoderBottleneckFlag(double smoothedEncodeMs, double frameIntervalMs, bool startupWindowActive) {
    const bool currentlyBottlenecked = g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
    bool newState = currentlyBottlenecked;
    if (startupWindowActive) {
        newState = false;
    } else if (currentlyBottlenecked) {
        // Exit bottleneck only when encode time drops well below the frame budget
        if (smoothedEncodeMs < frameIntervalMs * kBottleneckExitRatio) {
            newState = false;
        }
    } else {
        // Enter bottleneck when encode time approaches the frame budget
        if (smoothedEncodeMs > frameIntervalMs * kBottleneckEnterRatio) {
            newState = true;
        }
    }
    if (newState != currentlyBottlenecked) {
        g_IsEncoderBottlenecked.store(newState, std::memory_order_relaxed);
        if (g_pSharedMem) {
            g_pSharedMem->runtimeState.encoderBottlenecked.store(newState ? 1u : 0u, std::memory_order_relaxed);
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
    if (lhs.logLevel != rhs.logLevel || lhs.captureMethod != rhs.captureMethod ||
        lhs.wgcSkipSplitDeviceFlush != rhs.wgcSkipSplitDeviceFlush ||
        lhs.wgcSameDeviceCapture != rhs.wgcSameDeviceCapture || !MediaVideoConfigEquals(lhs.video, rhs.video) ||
        lhs.audioSources.size() != rhs.audioSources.size()) {
        return false;
    }

    for (size_t i = 0; i < lhs.audioSources.size(); ++i) {
        if (!MediaAudioConfigEquals(lhs.audioSources[i], rhs.audioSources[i])) {
            return false;
        }
    }

    return true;
}

bool IsExplicitTenBitVideo(const VideoConfig& video) {
    return _stricmp(video.bitDepth.c_str(), "10") == 0;
}

uint32_t GetInitialWgcCfrTargetFps(const VideoConfig& video) {
    if (video.useVFR || video.fps <= 0) {
        return 0;
    }

    return ce::capture_policy::GetWgcCfrOvercaptureTargetFps(static_cast<uint32_t>(video.fps));
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
    state.encoderSustainFpsX100.store(0, std::memory_order_relaxed);
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
    state.wgcSelectionErrorAvgUs.store(0, std::memory_order_relaxed);
    state.wgcSelectionErrorMaxUs.store(0, std::memory_order_relaxed);
    state.wgcSelectionErrorSignedAvgUs.store(0, std::memory_order_relaxed);
    state.wgcSelectionEarlyMaxUs.store(0, std::memory_order_relaxed);
    state.wgcSelectionLateMaxUs.store(0, std::memory_order_relaxed);
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
    state.wgcCaptureHealthFlags.store(0, std::memory_order_relaxed);
    state.wgcCaptureHealthFps.store(0, std::memory_order_relaxed);
    state.encoderBottlenecked.store(0, std::memory_order_relaxed);
}

bool IsActiveScreenGrab() {
    return g_UseScreenGrab.load(std::memory_order_acquire);
}

void SetActiveScreenGrab(bool enabled) {
    g_UseScreenGrab.store(enabled, std::memory_order_release);
    if (MediaEngine_SetActiveScreenGrab) {
        MediaEngine_SetActiveScreenGrab(enabled);
    }
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
        // Propagate audio-only flag so overlay can show AUDIO vs REC
        g_pSharedMem->runtimeState.audioOnly.store(g_AudioOnly, std::memory_order_release);
    } else {
        g_pSharedMem->runtimeState.isRecording.store(false, std::memory_order_release);
        g_pSharedMem->runtimeState.recordingStartTime.store(0, std::memory_order_release);
        g_pSharedMem->runtimeState.audioOnly.store(false, std::memory_order_release);
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
    kHeadroom105 = 1,
    kHeadroom108 = 2,
    kHeadroom125 = 3,
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
    if (g_Recording.load(std::memory_order_acquire) && !IsActiveScreenGrab()) {
        const uint64_t discarded = g_ActivePathMismatchFramesDiscarded.fetch_add(1, std::memory_order_relaxed) + 1;
        if (discarded <= 3 || (discarded % 120ull) == 0ull) {
            LogInfo(
                "[WGC] Dropping standby WGC frame while inject capture is active (discarded=%llu, ts=%lld). This "
                "prevents mid-recording encoder mode switches.",
                static_cast<unsigned long long>(discarded), static_cast<long long>(timestamp));
        }
        if (texture) {
            texture->Release();
        }
        return;
    }

    QueuedFrame qf;
    qf.isInjectMode = false;
    qf.texture = texture;
    qf.width = width;
    qf.height = height;
    qf.timestamp = timestamp;
    qf.rawTimestamp = rawTimestamp;
    qf.selectionTimestamp = timestamp;
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

static QueuedFrame MakeQueuedWgcFrame(WGCCapturedFrame&& frame) {
    QueuedFrame qf;
    qf.isInjectMode = false;
    qf.texture = frame.texture;
    frame.texture = nullptr;
    qf.width = frame.width;
    qf.height = frame.height;
    qf.timestamp = frame.timestamp;
    qf.rawTimestamp = frame.rawTimestamp;
    qf.selectionTimestamp = frame.timestamp;
    LARGE_INTEGER enqueueQpc;
    QueryPerformanceCounter(&enqueueQpc);
    qf.enqueueQpc = enqueueQpc.QuadPart;
    qf.isHDR = frame.isHDR;
    qf.duplicateSourceTimestamp = frame.duplicateSourceTimestamp;
    qf.captureLeft = frame.captureLeft;
    qf.captureTop = frame.captureTop;
    return qf;
}

static void ReleaseWgcCapturedFrame(WGCCapturedFrame& frame) {
    if (frame.texture) {
        frame.texture->Release();
        frame.texture = nullptr;
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
    if (g_pSharedMem) {
        g_pSharedMem->runtimeState.wgcTargetFps.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcCaptureHealthFlags.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcCaptureHealthFps.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionErrorAvgUs.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionErrorMaxUs.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionErrorSignedAvgUs.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionEarlyMaxUs.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionLateMaxUs.store(0, std::memory_order_relaxed);
    }
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
    g_WgcCap->SetSkipSplitDeviceFlush(config.wgcSkipSplitDeviceFlush);
    g_WgcCap->SetSameDeviceCapture(config.wgcSameDeviceCapture);
    g_WgcCap->SetRequireHighPrecisionCapture(IsExplicitTenBitVideo(config.video));
    if (config.video.useVFR) {
        g_WgcCap->SetDirectFrameCallback(QueueWgcFrame);
    } else {
        g_WgcCap->SetDirectFrameCallback(nullptr);
    }
    g_WgcCap->ResetStats();
    g_IsEncoderBottlenecked.store(false, std::memory_order_relaxed);
    const uint32_t initialWgcTargetFps = GetInitialWgcCfrTargetFps(config.video);
    g_WgcAdaptiveTargetFps.store(initialWgcTargetFps, std::memory_order_relaxed);
    // CFR WGC keeps source capture uncapped by default. The encoder thread
    // down-samples to CFR ticks, and extra candidates are important for
    // zero-latency phase selection under compositor/callback jitter.
    g_WgcCap->SetTargetFps(initialWgcTargetFps);

    // For CFR recording, disable the encoder-bottleneck throttle at the WGC
    // callback level.  The throttle is all-or-nothing (bang-bang) and its slow
    // EMA causes boom-bust oscillation that starves the Bresenham credit
    // accumulator, producing irregular frame-hold patterns (visible judder).
    // The encoder thread's buffer cap + Bresenham skip already provide smooth
    // backpressure, so the throttle is both unnecessary and harmful for CFR.
    if (!config.video.useVFR) {
        g_WgcCap->SetThrottleFlag(nullptr);
        LogInfo("[Media] WGC CFR mode: pull-latest sampling enabled, callback queue bypassed");
        LogInfo("[Media] WGC CFR mode: encoder-bottleneck throttle disabled (buffer cap provides backpressure)");
    }
    if (g_pSharedMem) {
        g_pSharedMem->runtimeState.wgcTargetFps.store(initialWgcTargetFps, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcCaptureHealthFlags.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcCaptureHealthFps.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionErrorAvgUs.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionErrorMaxUs.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionErrorSignedAvgUs.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionEarlyMaxUs.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionLateMaxUs.store(0, std::memory_order_relaxed);
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

    g_WgcCap->SetGpuPriority(config.video.gpuPriority);

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
    const int64_t targetIntervalTicks =
        (config.video.fps > 0) ? (qpcFreq.QuadPart / config.video.fps) : (qpcFreq.QuadPart / 60);
    const uint32_t injectPublicationFps =
        ce::capture_policy::GetInjectCfrSourcePublicationFps(static_cast<uint32_t>(std::max(config.video.fps, 1)));
    const int64_t injectPublicationIntervalTicks =
        std::max<int64_t>(1, ce::capture_policy::GetInjectCfrSourcePublicationIntervalQpc(
                                 static_cast<uint32_t>(std::max(config.video.fps, 1)), qpcFreq.QuadPart));
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
                // The media thread owns CFR source selection.  In normal shallow-queue
                // operation, keep a high-rate publication stream so the selector has
                // before/after candidates for every output grid tick.  Only fall back to
                // source-side target pacing under real queue pressure.
                bool shouldProcess = false;

                const uint32_t queueDepth = static_cast<uint32_t>(g_FrameQueue.Size());
                const uint32_t queuePressureThreshold =
                    std::max<uint32_t>(24u, static_cast<uint32_t>(g_FrameQueue.Capacity() * 3 / 4));
                const bool useSourceSidePacing = queueDepth >= queuePressureThreshold;
                const int64_t pacingIntervalTicks =
                    useSourceSidePacing ? targetIntervalTicks : injectPublicationIntervalTicks;

                if (nextPushTime == 0) {
                    // First frame or resync
                    nextPushTime = slot.timestamp;
                    shouldProcess = true;
                } else {
                    const int64_t jitterWindow = useSourceSidePacing ? (pacingIntervalTicks * 8) / 10
                                                                     : std::max<int64_t>(1, pacingIntervalTicks / 8);

                    if (slot.timestamp >= nextPushTime - jitterWindow) {
                        shouldProcess = true;

                        // Advance target time by actual interval, not to current timestamp
                        // This maintains steady output cadence even with jittery input
                        nextPushTime += pacingIntervalTicks;

                        // Resync if game time jumped way ahead (e.g. pause/lag spike > 5
                        // frames) Increased from 3 to 5 frames to avoid unnecessary resyncs
                        if (slot.timestamp > nextPushTime + (pacingIntervalTicks * 5)) {
                            nextPushTime = slot.timestamp + pacingIntervalTicks;
                        }
                    } else {
                        // Frame is too early - only drop if we're not behind on processing
                        // Check if we have a backlog of frames waiting
                        uint32_t pendingFrames = (writeIndex > localReadIndex) ? (writeIndex - localReadIndex) : 0;

                        if (useSourceSidePacing && pendingFrames > 2) {
                            // We have a backlog, process this frame anyway to catch up
                            shouldProcess = true;
                            nextPushTime = slot.timestamp + pacingIntervalTicks;
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
                        if (IsValidTextureIndex(texIdx)) {
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
                "[Inject Perf] Input: %u | Queued: %u | DropFull: %u | DropPace: %u | PubFps: %u | HostQ: %u | EncQ: "
                "%u | Dup: %u | Late: %u | Trim: %u | SelDrop: %u | Def: %u | Encode: %lldus | Fence: %lldus | Mux: "
                "%uKB | Overload: 0x%X",
                inputFrames, pushedCount, droppedCount, pacingDroppedCount, injectPublicationFps,
                static_cast<uint32_t>(g_FrameQueue.Size()), encoderQueueDepth, dupDelta, lateDelta, trimDelta,
                cadenceDropDelta, deferredDelta, MediaEngine_GetLastFrameEncodeTimeUs(),
                MediaEngine_GetLastFrameFenceWaitUs(), (muxQueueBytes + 1023u) / 1024u, overloadFlags);
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
    uint32_t lastKeyedAcquireFailCount = 0;
    uint32_t lastKeyedReleaseFailCount = 0;
    uint32_t lastSplitFlushCount = 0;
    uint32_t lastSplitFlushSkippedCount = 0;
    uint32_t lastPoolSlotFastRewriteCount = 0;
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
            lastKeyedAcquireFailCount = 0;
            lastKeyedReleaseFailCount = 0;
            lastSplitFlushCount = 0;
            lastSplitFlushSkippedCount = 0;
            lastPoolSlotFastRewriteCount = 0;
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
            lastKeyedAcquireFailCount = g_WgcCap->GetKeyedMutexAcquireFailCount();
            lastKeyedReleaseFailCount = g_WgcCap->GetKeyedMutexReleaseFailCount();
            lastSplitFlushCount = g_WgcCap->GetSplitDeviceFlushCount();
            lastSplitFlushSkippedCount = g_WgcCap->GetSplitDeviceFlushSkippedCount();
            lastPoolSlotFastRewriteCount = g_WgcCap->GetPoolSlotFastRewriteCount();
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
            uint32_t currentKeyedAcquireFailCount = g_WgcCap->GetKeyedMutexAcquireFailCount();
            uint32_t currentKeyedReleaseFailCount = g_WgcCap->GetKeyedMutexReleaseFailCount();
            uint32_t currentSplitFlushCount = g_WgcCap->GetSplitDeviceFlushCount();
            uint32_t currentSplitFlushSkippedCount = g_WgcCap->GetSplitDeviceFlushSkippedCount();
            uint32_t currentPoolSlotFastRewriteCount = g_WgcCap->GetPoolSlotFastRewriteCount();
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
            uint32_t keyedAcquireFailDelta = currentKeyedAcquireFailCount - lastKeyedAcquireFailCount;
            uint32_t keyedReleaseFailDelta = currentKeyedReleaseFailCount - lastKeyedReleaseFailCount;
            uint32_t splitFlushDelta = currentSplitFlushCount - lastSplitFlushCount;
            uint32_t splitFlushSkippedDelta = currentSplitFlushSkippedCount - lastSplitFlushSkippedCount;
            uint32_t poolSlotFastRewriteDelta = currentPoolSlotFastRewriteCount - lastPoolSlotFastRewriteCount;
            uint32_t queuedFrames = deliveredFrames >= hostDropDelta ? (deliveredFrames - hostDropDelta) : 0;
            int64_t copyUs = g_WgcCap->GetLastCopyTimeUs();
            int64_t srcIntervalAvgUs = g_WgcCap->GetSourceIntervalAvgUs();
            int64_t srcJitterAvgUs = g_WgcCap->GetSourceJitterAvgUs();
            int64_t srcJitterMaxUs = g_WgcCap->GetSourceJitterMaxUs();
            int64_t srcToCopyAvgUs = g_WgcCap->GetSourceToCopyLatencyAvgUs();
            int64_t srcToCopyMaxUs = g_WgcCap->GetSourceToCopyLatencyMaxUs();
            int64_t poolSlotRewriteUs = g_WgcCap->GetLastPoolSlotRewriteUs();
            int64_t callbackGapAvgUs = g_WgcCap->GetCallbackGapAvgUs();
            int64_t callbackGapMaxUs = g_WgcCap->GetCallbackGapMaxUs();
            int64_t callbackProcessAvgUs = g_WgcCap->GetCallbackProcessAvgUs();
            int64_t callbackProcessMaxUs = g_WgcCap->GetCallbackProcessMaxUs();
            uint32_t callbackDrainMax = g_WgcCap->GetCallbackDrainMaxCount();
            int64_t encodeUs = MediaEngine_GetLastFrameEncodeTimeUs();
            int64_t fenceUs = MediaEngine_GetLastFrameFenceWaitUs();
            uint32_t dupDelta = 0;
            uint32_t lateDelta = 0;
            uint32_t overloadFlags = 0;
            uint32_t muxQueueBytes = 0;
            uint32_t encoderQueueDepth = static_cast<uint32_t>(g_FrameQueue.Size());
            uint32_t cadenceSelAvgUs = 0;
            int32_t cadenceSelBiasUs = 0;
            uint32_t wgcSelAvgUs = 0;
            int32_t wgcSelBiasUs = 0;
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
                wgcSelAvgUs = g_pSharedMem->runtimeState.wgcSelectionErrorAvgUs.load(std::memory_order_relaxed);
                wgcSelBiasUs = g_pSharedMem->runtimeState.wgcSelectionErrorSignedAvgUs.load(std::memory_order_relaxed);
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
                "NoFresh: %u | NoReserve: %u | SchedSelAvg: %uus "
                "SchedSelBias: %dus | WgcSelAvg: %uus WgcSelBias: %dus | CbGap: %lld/%lldus "
                "CbProc: %lld/%lldus CbDrainMax: %u | Copy: %lldus | "
                "SlotAge: %lldus FastSlot: %u | KMFail: %u/%u | Flush: %u/%u | "
                "Dedicated: %d | Encode: %lldus | Fence: %lldus | Throttle: %u | Mux: %uKB | Overload: 0x%X",
                inputFrames, queuedFrames, hostDropDelta, pacingSkipDelta, throttleSkipDelta, staleSkipDelta,
                staleDuplicateTsDelta, staleOutOfOrderTsDelta, cursorSkipDelta, poolDropDelta,
                static_cast<uint32_t>(g_FrameQueue.Size()), encoderQueueDepth, dupDelta, lateDelta, srcIntervalAvgUs,
                srcJitterAvgUs, srcJitterMaxUs, srcToCopyAvgUs, srcToCopyMaxUs, deliveredRatePerSec, inputMin250Fps,
                inputMin500Fps, deliveredMin250Fps, deliveredMin500Fps, queueEmptyPermille, bufferedAtTickAvgPermille,
                bufferedAtTickMin, starvedTicks, singleFrameTicks, cadenceSelAvgUs, cadenceSelBiasUs, wgcSelAvgUs,
                wgcSelBiasUs, callbackGapAvgUs, callbackGapMaxUs, callbackProcessAvgUs, callbackProcessMaxUs,
                callbackDrainMax, copyUs, poolSlotRewriteUs, poolSlotFastRewriteDelta, keyedAcquireFailDelta,
                keyedReleaseFailDelta, splitFlushDelta, splitFlushSkippedDelta,
                g_WgcCap->IsUsingDedicatedCaptureDevice() ? 1 : 0, encodeUs, fenceUs, throttleTargetFps,
                (muxQueueBytes + 1023u) / 1024u, overloadFlags);

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
            lastKeyedAcquireFailCount = currentKeyedAcquireFailCount;
            lastKeyedReleaseFailCount = currentKeyedReleaseFailCount;
            lastSplitFlushCount = currentSplitFlushCount;
            lastSplitFlushSkippedCount = currentSplitFlushSkippedCount;
            lastPoolSlotFastRewriteCount = currentPoolSlotFastRewriteCount;
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
    std::vector<WGCCapturedFrame> drainedWgcCapturedFrames;
    drainedWgcCapturedFrames.reserve(8);
    std::deque<QueuedFrame> bufferedWgcFrames;
    std::vector<size_t> wgcFreshCandidateIndices;
    wgcFreshCandidateIndices.reserve(64);
    std::vector<size_t> wgcFallbackCandidateIndices;
    wgcFallbackCandidateIndices.reserve(64);
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
    InputFrameRatePredictor injectInputPredictor;
    uint32_t injectWorstSourceFpsX100 = std::numeric_limits<uint32_t>::max();
    uint32_t injectBestSourceFpsX100 = 0;
    uint32_t injectWorstSourceJitterUs = 0;
    uint32_t injectWorstSelectionErrorUs = 0;
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
    uint32_t injectDeferredRequeuedThisWindow = 0;
    uint32_t injectDeferredDroppedThisWindow = 0;
    uint64_t injectDeferredRequeuedTotal = 0;
    uint64_t injectDeferredDroppedTotal = 0;
    uint32_t injectFreshCatchupThisWindow = 0;
    uint32_t injectRepeatCatchupThisWindow = 0;
    uint32_t injectLiveStaleTrimThisWindow = 0;
    uint32_t activePathMismatchDiscardThisWindow = 0;
    uint64_t injectFreshCatchupTotal = 0;
    uint64_t injectRepeatCatchupTotal = 0;
    uint64_t injectLiveStaleTrimTotal = 0;
    uint64_t activePathMismatchDiscardTotal = 0;
    size_t pendingLiveInjectReadyFrames = 0;
    DWORD lastHealthLog = GetTickCount();
    LARGE_INTEGER liveStartQpc = {};
    uint64_t liveTicksOutput = 0;
    uint64_t liveTicksScheduled = 0;
    uint64_t liveTicksDiscardedByTimerRebase = 0;
    uint64_t wgcVisualDebtMaxExcessTicks = 0;
    uint64_t wgcLiveSchedulerRebaseTotal = 0;
    uint32_t wgcLiveSchedulerRebaseMaxTicks = 0;
    uint32_t wgcLiveSchedulerRebaseThisWindow = 0;
    bool wgcStopDrainHeldFrameLogged = false;
    uint64_t wgcSelectionErrorAccumUs = 0;
    int64_t wgcSelectionErrorSignedAccumUs = 0;
    uint32_t wgcSelectionErrorSamples = 0;
    uint32_t wgcSelectionErrorMaxUs = 0;
    uint32_t wgcSelectionEarlyMaxUs = 0;
    uint32_t wgcSelectionLateMaxUs = 0;
    uint32_t wgcSelectionTargetClampCount = 0;
    uint32_t wgcSelectionTargetClampMaxUs = 0;
    uint32_t wgcHoldForNextTickCount = 0;
    uint32_t wgcSelectionDelayTickCount = 0;
    uint32_t wgcAdaptiveThrottleAdjustments = 0;
    WgcAdaptiveThrottleMode wgcAdaptiveThrottleMode = WgcAdaptiveThrottleMode::kOff;
    uint32_t wgcAdaptiveThrottlePendingTargetFps = 0;
    WgcAdaptiveThrottleMode wgcAdaptiveThrottlePendingMode = WgcAdaptiveThrottleMode::kOff;
    uint64_t wgcAdaptiveThrottlePendingSinceTick = 0;
    uint64_t wgcOvercaptureStableSinceTick = 0;
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
    uint64_t wgcRepeatPolicyHoldTotal = 0;
    uint32_t wgcRepeatTimerLateCount = 0;
    uint32_t wgcRepeatCatchupCount = 0;
    uint32_t wgcFreshCatchupCount = 0;
    uint32_t wgcSelectFreshCount = 0;
    uint32_t wgcSelectDuplicateSourceCount = 0;
    uint32_t wgcDropObsoleteCount = 0;
    uint32_t wgcDropStaleDebtCount = 0;
    uint64_t wgcDropStaleDebtTotal = 0;
    uint32_t wgcDropStaleDebtMaxUs = 0;
    uint32_t wgcEncoderLimitedSourceDropThisWindow = 0;
    uint64_t wgcEncoderLimitedSourceDropTotal = 0;
    uint32_t wgcEncoderLimitedSourceDropMaxTicks = 0;
    uint64_t wgcEncoderLimitedCadenceEventCount = 0;
    uint32_t wgcEncoderLimitedSuppressedByLowSourceThisWindow = 0;
    uint64_t wgcEncoderLimitedSuppressedByLowSourceTotal = 0;
    uint32_t wgcCapacityPressureModeMismatchThisWindow = 0;
    uint64_t wgcCapacityPressureModeMismatchTotal = 0;
    uint32_t wgcSelectedSourceBacktrackThisWindow = 0;
    uint64_t wgcSelectedSourceBacktrackTotal = 0;
    uint64_t wgcPostStopFrameDropTotal = 0;
    uint32_t wgcPostStopFrameDropMaxUs = 0;
    uint32_t wgcCoverageRepeatHoldCount = 0;
    uint32_t wgcCoverageDelayTicksCurrent = 0;
    double wgcAudioLeadExcessMsCurrent = 0.0;
    bool wgcCoverageRepeatActiveCurrent = false;
    bool encoderTooSlowForTargetCurrent = false;
    bool wgcLowSourceModeActive = false;
    bool wgcLiveRecoveryModeActive = false;
    bool wgcReservePressureActive = false;
    uint64_t wgcLowSourceStateChangeTick = 0;
    uint64_t wgcLiveRecoveryStateChangeTick = 0;
    bool wgcSourceStarvedCurrent = false;
    bool wgcSchedulerLimitedCurrent = false;
    bool wgcEncoderRecoveryLimitedCurrent = false;
    int64_t lastEmittedWgcSourceQpc = 0;
    int64_t lastEmittedInjectSourceQpc = 0;
    int64_t lastWarmupWgcSourceQpc = 0;
    int64_t wgcStartupBarrierQpc = 0;
    uint32_t wgcStartupBarrierDroppedFrames = 0;
    bool wgcStartupPreLiveDelayComplete = false;
    uint32_t wgcStartupPreLiveDelayDroppedFrames = 0;
    uint32_t wgcFreshWarmupFrameCount = 0;
    uint32_t wgcOldestBufferedFrameAgeUs = 0;
    double wgcCoverageRepeatAccumulator = 0.0;
    struct WgcStarvedEpisodeSummary {
        bool active = false;
        uint64_t startTickMs = 0;
        int64_t startQpc = 0;
        uint64_t startLiveTicks = 0;
        uint64_t startDuplicateTicks = 0;
        uint32_t minInputFps = std::numeric_limits<uint32_t>::max();
        uint32_t minDeliveredFps = std::numeric_limits<uint32_t>::max();
        uint32_t peakFreshMissPermille = 0;
        uint32_t minBufferedFrames = std::numeric_limits<uint32_t>::max();
        uint32_t maxCallbackGapUs = 0;
        uint32_t maxCopyUs = 0;
        uint32_t maxFenceUs = 0;
        uint32_t maxMuxBackpressureCount = 0;
        uint32_t maxMuxBackpressureWaitUs = 0;
        uint32_t maxMuxQueueKb = 0;
        uint32_t peakOverloadFlags = 0;
        double maxEncodeEmaMs = 0.0;

        void Reset() {
            *this = {};
            minInputFps = std::numeric_limits<uint32_t>::max();
            minDeliveredFps = std::numeric_limits<uint32_t>::max();
            minBufferedFrames = std::numeric_limits<uint32_t>::max();
        }
    };
    struct CaptureSessionSummary {
        uint64_t duplicateTicks = 0;
        uint64_t duplicateNoSourceTicks = 0;
        uint64_t duplicateDeferredTicks = 0;
        uint64_t duplicateTimerTicks = 0;
        uint64_t duplicateDrainTicks = 0;
        uint64_t queueTickSamples = 0;
        uint64_t noFreshTicks = 0;
        uint64_t noReserveTicks = 0;
        uint64_t starvedEpisodes = 0;
        uint64_t longestStarvedEpisodeMs = 0;
        uint64_t longestStarvedEpisodeOutputTicks = 0;
        uint64_t longestStarvedEpisodeDuplicateTicks = 0;
        uint32_t longestStarvedEpisodeMinInputFps = std::numeric_limits<uint32_t>::max();
        uint32_t longestStarvedEpisodeMinDeliveredFps = std::numeric_limits<uint32_t>::max();
        uint32_t worstFreshMissPermille = 0;
        uint32_t worstSourceFpsX100 = std::numeric_limits<uint32_t>::max();
        uint32_t bestSourceFpsX100 = 0;
        uint32_t worstInputMin250Fps = std::numeric_limits<uint32_t>::max();
        uint32_t worstDeliveredMin250Fps = std::numeric_limits<uint32_t>::max();
        uint32_t worstSourceJitterUs = 0;
        uint32_t worstSelectionErrorUs = 0;
        uint32_t worstWgcSelectionErrorUs = 0;
        uint32_t worstOldestBufferedFrameAgeUs = 0;
        uint32_t lowSourceImmediateExits = 0;
        double maxShortfallDurationMs = 0.0;
        double maxEncodeEmaMs = 0.0;
        double minEncoderSustainFps = std::numeric_limits<double>::max();
        uint32_t maxWgcContentPhaseErrorUs = 0;

        void Reset() {
            *this = {};
            longestStarvedEpisodeMinInputFps = std::numeric_limits<uint32_t>::max();
            longestStarvedEpisodeMinDeliveredFps = std::numeric_limits<uint32_t>::max();
            worstSourceFpsX100 = std::numeric_limits<uint32_t>::max();
            worstInputMin250Fps = std::numeric_limits<uint32_t>::max();
            worstDeliveredMin250Fps = std::numeric_limits<uint32_t>::max();
            minEncoderSustainFps = std::numeric_limits<double>::max();
        }
    };
    WgcStarvedEpisodeSummary wgcStarvedEpisode;
    wgcStarvedEpisode.Reset();
    CaptureSessionSummary captureSessionSummary;
    captureSessionSummary.Reset();
    const auto accumulateCaptureSummarySample =
        [&](bool useScreenGrabSession, uint32_t srcFpsX100Val, uint32_t srcJitterUsVal, uint32_t dupNoSource,
            uint32_t dupDeferred, uint32_t dupTimer, uint32_t dupDrain, uint32_t oldestBufferedFrameAgeUs,
            double shortfallDurationMs, double sustainableOutputFps) {
            captureSessionSummary.duplicateTicks += cadenceCounters.liveTickDuplicateCount;
            captureSessionSummary.duplicateNoSourceTicks += dupNoSource - lastDuplicateReasonNoSource;
            captureSessionSummary.duplicateDeferredTicks += dupDeferred - lastDuplicateReasonDeferred;
            captureSessionSummary.duplicateTimerTicks += dupTimer - lastDuplicateReasonTimerRebase;
            captureSessionSummary.duplicateDrainTicks += dupDrain - lastDuplicateReasonDrain;
            captureSessionSummary.maxEncodeEmaMs = std::max(captureSessionSummary.maxEncodeEmaMs, smoothedEncodeMs);
            captureSessionSummary.minEncoderSustainFps =
                std::min(captureSessionSummary.minEncoderSustainFps, sustainableOutputFps);

            if (useScreenGrabSession) {
                captureSessionSummary.queueTickSamples += wgcQueueTickSampleCount;
                captureSessionSummary.noFreshTicks += wgcNoFreshTickCount;
                captureSessionSummary.noReserveTicks += wgcNoReserveTickCount;
                captureSessionSummary.worstFreshMissPermille =
                    std::max(captureSessionSummary.worstFreshMissPermille, wgcNoFreshTickPermille);
                if (srcFpsX100Val > 0) {
                    captureSessionSummary.worstSourceFpsX100 =
                        std::min(captureSessionSummary.worstSourceFpsX100, srcFpsX100Val);
                    captureSessionSummary.bestSourceFpsX100 =
                        std::max(captureSessionSummary.bestSourceFpsX100, srcFpsX100Val);
                }
                if (wgcRecentInputMin250Fps > 0) {
                    captureSessionSummary.worstInputMin250Fps =
                        std::min(captureSessionSummary.worstInputMin250Fps, wgcRecentInputMin250Fps);
                }
                if (wgcRecentDeliveredMin250Fps > 0) {
                    captureSessionSummary.worstDeliveredMin250Fps =
                        std::min(captureSessionSummary.worstDeliveredMin250Fps, wgcRecentDeliveredMin250Fps);
                }
                captureSessionSummary.worstSourceJitterUs =
                    std::max(captureSessionSummary.worstSourceJitterUs, srcJitterUsVal);
                captureSessionSummary.worstSelectionErrorUs =
                    std::max(captureSessionSummary.worstSelectionErrorUs, cadenceCounters.selectionErrorMaxUs);
                captureSessionSummary.worstWgcSelectionErrorUs =
                    std::max(captureSessionSummary.worstWgcSelectionErrorUs, wgcSelectionErrorMaxUs);
                if (wgcSelectionErrorSamples > 0) {
                    const int64_t avgContentPhaseErrorUs =
                        wgcSelectionErrorSignedAccumUs / static_cast<int64_t>(wgcSelectionErrorSamples);
                    captureSessionSummary.maxWgcContentPhaseErrorUs =
                        std::max(captureSessionSummary.maxWgcContentPhaseErrorUs,
                                 SaturatingToUint32(static_cast<uint64_t>(
                                     avgContentPhaseErrorUs >= 0 ? avgContentPhaseErrorUs : -avgContentPhaseErrorUs)));
                }
                captureSessionSummary.worstOldestBufferedFrameAgeUs =
                    std::max(captureSessionSummary.worstOldestBufferedFrameAgeUs, oldestBufferedFrameAgeUs);
                captureSessionSummary.maxShortfallDurationMs =
                    std::max(captureSessionSummary.maxShortfallDurationMs, shortfallDurationMs);
            } else {
                captureSessionSummary.worstSelectionErrorUs =
                    std::max(captureSessionSummary.worstSelectionErrorUs, cadenceCounters.selectionErrorMaxUs);
                injectWorstSelectionErrorUs =
                    std::max(injectWorstSelectionErrorUs, cadenceCounters.outputScheduleErrorMaxUs);
                if (srcFpsX100Val > 0) {
                    injectWorstSourceFpsX100 = std::min(injectWorstSourceFpsX100, srcFpsX100Val);
                    injectBestSourceFpsX100 = std::max(injectBestSourceFpsX100, srcFpsX100Val);
                }
                injectWorstSourceJitterUs = std::max(injectWorstSourceJitterUs, srcJitterUsVal);
            }
        };
    const uint64_t minLoggedWgcStarvedEpisodeMs =
        std::max<uint64_t>(100ull, static_cast<uint64_t>(frameIntervalMs * 8.0 + 0.5));
    const auto shouldLogWgcStarvedEpisode = [&](uint64_t durationMs, uint64_t outputTicks, uint64_t duplicateTicks,
                                                uint32_t peakFreshMissPermille) {
        if (duplicateTicks > 0 || durationMs >= minLoggedWgcStarvedEpisodeMs) {
            return true;
        }

        // Suppress single-tick/no-duplicate blips that can occur when the rolling
        // no-fresh telemetry briefly spikes without a visible cadence miss.
        return outputTicks > 1 && peakFreshMissPermille >= ce::capture_policy::kWgcDeepUnderfeedEmptyTickPermille;
    };
    const auto finishWgcStarvedEpisode = [&](uint64_t durationMs, uint64_t outputTicks, uint64_t duplicateTicks) {
        ++captureSessionSummary.starvedEpisodes;
        const uint32_t minInputFps =
            wgcStarvedEpisode.minInputFps == std::numeric_limits<uint32_t>::max() ? 0u : wgcStarvedEpisode.minInputFps;
        const uint32_t minDeliveredFps = wgcStarvedEpisode.minDeliveredFps == std::numeric_limits<uint32_t>::max()
                                             ? 0u
                                             : wgcStarvedEpisode.minDeliveredFps;
        const uint32_t minBufferedFrames = wgcStarvedEpisode.minBufferedFrames == std::numeric_limits<uint32_t>::max()
                                               ? 0u
                                               : wgcStarvedEpisode.minBufferedFrames;
        const uint32_t targetOutputFps = std::max<uint32_t>(1u, static_cast<uint32_t>(config.video.fps));
        wgcStarvedEpisode.maxEncodeEmaMs = std::max(wgcStarvedEpisode.maxEncodeEmaMs, smoothedEncodeMs);
        wgcStarvedEpisode.maxFenceUs = std::max(
            wgcStarvedEpisode.maxFenceUs,
            SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(0, MediaEngine_GetLastFrameFenceWaitUs()))));
        if (g_WgcCap) {
            wgcStarvedEpisode.maxCallbackGapUs = std::max(
                wgcStarvedEpisode.maxCallbackGapUs,
                SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(0, g_WgcCap->GetCallbackGapMaxUs()))));
            wgcStarvedEpisode.maxCopyUs = std::max(
                wgcStarvedEpisode.maxCopyUs,
                SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(0, g_WgcCap->GetLastCopyTimeUs()))));
        }
        if (g_pSharedMem) {
            const auto& runtimeState = g_pSharedMem->runtimeState;
            const uint32_t muxQueueBytes = runtimeState.muxQueueBytes.load(std::memory_order_relaxed);
            wgcStarvedEpisode.maxMuxQueueKb =
                std::max(wgcStarvedEpisode.maxMuxQueueKb, (muxQueueBytes + 1023u) / 1024u);
            wgcStarvedEpisode.maxMuxBackpressureCount =
                std::max(wgcStarvedEpisode.maxMuxBackpressureCount,
                         runtimeState.muxBackpressureCount.load(std::memory_order_relaxed));
            wgcStarvedEpisode.maxMuxBackpressureWaitUs =
                std::max(wgcStarvedEpisode.maxMuxBackpressureWaitUs,
                         runtimeState.muxBackpressureMaxWaitUs.load(std::memory_order_relaxed));
            wgcStarvedEpisode.peakOverloadFlags |= runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
        }
        LARGE_INTEGER endQpc = {};
        QueryPerformanceCounter(&endQpc);
        const bool capacityPressure = wgcStarvedEpisode.peakOverloadFlags != 0 ||
                                      wgcStarvedEpisode.maxMuxBackpressureCount > 0 ||
                                      wgcStarvedEpisode.maxEncodeEmaMs >= frameIntervalMs;
        const char* faultHint = capacityPressure ? "ce_capacity_pressure"
                                : minInputFps > 0 && minInputFps < targetOutputFps
                                    ? "source_present_gap_or_source_underfeed"
                                    : "source_starved";
        const uint32_t frameBudgetUs = static_cast<uint32_t>(std::max(1.0, frameIntervalMs * 1000.0));
        const bool copySlow = wgcStarvedEpisode.maxCopyUs > frameBudgetUs;
        const bool fenceSlow = wgcStarvedEpisode.maxFenceUs > frameBudgetUs;
        if (durationMs >= captureSessionSummary.longestStarvedEpisodeMs) {
            captureSessionSummary.longestStarvedEpisodeMs = durationMs;
            captureSessionSummary.longestStarvedEpisodeOutputTicks = outputTicks;
            captureSessionSummary.longestStarvedEpisodeDuplicateTicks = duplicateTicks;
            captureSessionSummary.longestStarvedEpisodeMinInputFps = minInputFps;
            captureSessionSummary.longestStarvedEpisodeMinDeliveredFps = minDeliveredFps;
        }
        if (shouldLogWgcStarvedEpisode(durationMs, outputTicks, duplicateTicks,
                                       wgcStarvedEpisode.peakFreshMissPermille)) {
            LogInfo(
                "[WGC CFR] Source-starved episode: duration=%llums out=%llu dup=%llu minIn=%u minDel=%u "
                "freshMiss=%upm minBuf=%u",
                static_cast<unsigned long long>(durationMs), static_cast<unsigned long long>(outputTicks),
                static_cast<unsigned long long>(duplicateTicks), minInputFps, minDeliveredFps,
                wgcStarvedEpisode.peakFreshMissPermille, minBufferedFrames);
            LogInfo(
                "[WGC CFR ATTRIBUTION] fault_hint=%s qpc=%lld..%lld duration=%llums out=%llu dup=%llu "
                "minIn=%u minDel=%u freshMiss=%upm minBuf=%u cbGapMax=%uus encEmaMax=%.2fms "
                "muxBp=%u waitMax=%uus muxMax=%uKB overload=0x%X copyMax=%uus copyHealth=%s "
                "fenceMax=%uus fenceHealth=%s",
                faultHint, static_cast<long long>(wgcStarvedEpisode.startQpc), static_cast<long long>(endQpc.QuadPart),
                static_cast<unsigned long long>(durationMs), static_cast<unsigned long long>(outputTicks),
                static_cast<unsigned long long>(duplicateTicks), minInputFps, minDeliveredFps,
                wgcStarvedEpisode.peakFreshMissPermille, minBufferedFrames, wgcStarvedEpisode.maxCallbackGapUs,
                wgcStarvedEpisode.maxEncodeEmaMs, wgcStarvedEpisode.maxMuxBackpressureCount,
                wgcStarvedEpisode.maxMuxBackpressureWaitUs, wgcStarvedEpisode.maxMuxQueueKb,
                wgcStarvedEpisode.peakOverloadFlags, wgcStarvedEpisode.maxCopyUs, copySlow ? "slow" : "ok",
                wgcStarvedEpisode.maxFenceUs, fenceSlow ? "slow" : "ok");
        }
        wgcStarvedEpisode.Reset();
    };
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
        wgcStartupBarrierQpc = 0;
        wgcStartupBarrierDroppedFrames = 0;
        wgcStartupPreLiveDelayComplete = false;
        wgcStartupPreLiveDelayDroppedFrames = 0;
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
            const int64_t drainStopQpc = g_CfrDrainStopQpc.load(std::memory_order_acquire);
            if (drainStopQpc > liveStartQpc.QuadPart) {
                scheduledUntilQpc = drainStopQpc;
            }
        }
        const uint64_t elapsedTicks = static_cast<uint64_t>(scheduledUntilQpc - liveStartQpc.QuadPart) /
                                      static_cast<uint64_t>(targetIntervalTicks);
        liveTicksScheduled = ce::capture_policy::GetCfrScheduledTicksForEndpoint(
            elapsedTicks, liveTicksDiscardedByTimerRebase, wgcVisualDebtMaxExcessTicks);
        return ce::capture_policy::GetCfrOutputShortfallTicks(liveTicksScheduled, liveTicksOutput);
    };

    // A/V content delay: align video content with inherently-late loopback audio by biasing
    // WGC source-frame selection back by the loopback capture latency (= the slowest audio
    // source's latency, so faster sources can be equalized up to it). Audio samples and the
    // CFR PTS grid are untouched, so track length/start/end and zero-drift are preserved.
    // Video buffer self-builds/holds via the bounded "too new for slot" path. QPC ticks.
    float maxAudioCaptureLatencyMs = 0.0f;
    for (const auto& audioSrc : config.audioSources) {
        if (audioSrc.captureLatencyMs > maxAudioCaptureLatencyMs) {
            maxAudioCaptureLatencyMs = audioSrc.captureLatencyMs;
        }
    }
    const int64_t avContentDelayQpc =
        (maxAudioCaptureLatencyMs > 0.0f && qpcFreq.QuadPart > 0)
            ? static_cast<int64_t>(std::llround(static_cast<double>(maxAudioCaptureLatencyMs) / 1000.0 *
                                                static_cast<double>(qpcFreq.QuadPart)))
            : 0;
    const bool avContentDelayActive = avContentDelayQpc > 0;
    // Inject path has no selection target; it pops the oldest buffered frame above a reserve,
    // so delaying inject video content = retaining this many extra frames (the oldest popped
    // frame becomes ~L old). Rounded up to whole frames.
    const size_t injectContentDelayFrames =
        (avContentDelayActive && frameIntervalMs > 0.0)
            ? static_cast<size_t>(std::ceil(static_cast<double>(maxAudioCaptureLatencyMs) / frameIntervalMs))
            : 0;
    if (avContentDelayActive) {
        LogInfo(
            "[EncoderThread] A/V content delay armed: maxAudioCaptureLatencyMs=%.3f delayUs=%lld method=%s "
            "injectDelayFrames=%zu (delays video content to match late loopback audio; audio/PTS unchanged)",
            static_cast<double>(maxAudioCaptureLatencyMs),
            (long long)((avContentDelayQpc * 1000000) / qpcFreq.QuadPart),
            IsActiveScreenGrab() ? "wgc-selection-bias" : "inject-buffer-reserve", injectContentDelayFrames);
    }

    while (g_EncoderRunning || g_DrainOutstandingCfrTicks.load(std::memory_order_acquire) || g_FrameQueue.Size() > 0 ||
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
        const bool useScreenGrab = activeScreenGrab;
        const uint32_t outputFps = std::max<uint32_t>(1u, static_cast<uint32_t>(config.video.fps));
        auto loadEncoderOverloadFlags = [&]() -> uint32_t {
            return g_pSharedMem ? g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed) : 0u;
        };
        auto isWgcCapacityPressureActive = [&]() -> bool {
            const uint32_t overloadFlags = loadEncoderOverloadFlags();
            return g_IsEncoderBottlenecked.load(std::memory_order_relaxed) || encoderTooSlowForTargetCurrent ||
                   (overloadFlags & (ce::capture_policy::kEncoderOverloadFlagEncoder |
                                     ce::capture_policy::kEncoderOverloadFlagMux)) != 0;
        };
        auto isWgcTrueSourceStarvedForCapacityPolicy = [&]() -> bool {
            return ce::capture_policy::IsWgcTrueSourceStarvedForRecovery(
                outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull)),
                isWgcCapacityPressureActive());
        };
        auto isWgcEncoderLimitedSmoothnessMode = [&]() -> bool {
            if (!activeScreenGrab || config.video.useVFR || !recordingOutputLive) {
                return false;
            }
            if (wgcSourceStarvedCurrent || isWgcTrueSourceStarvedForCapacityPolicy()) {
                return false;
            }
            const uint32_t bufferedWgcFrameCount =
                static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull));
            if (!ce::capture_policy::IsWgcSourceHealthyEnoughForEncoderLimitedSmoothness(
                    outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                    bufferedWgcFrameCount)) {
                return false;
            }
            return ce::capture_policy::IsWgcEncoderLimitedSmoothnessMode(
                g_IsEncoderBottlenecked.load(std::memory_order_relaxed), encoderTooSlowForTargetCurrent,
                loadEncoderOverloadFlags());
        };
        if (!g_EncoderRunning && !g_DrainOutstandingCfrTicks.load(std::memory_order_acquire) && activeScreenGrab) {
            const size_t bufferedDiscarded = bufferedWgcFrames.size();
            const size_t bufferedInjectDiscarded = bufferedInjectFrames.size();
            ClearBufferedWgcFrames();
            ClearBufferedInjectFrames();
            size_t queuedDiscarded = 0;
            QueuedFrame queuedFrame;
            while (g_FrameQueue.Pop(queuedFrame, 0)) {
                DiscardQueuedFrame(queuedFrame);
                ++queuedDiscarded;
            }
            if (bufferedDiscarded > 0 || bufferedInjectDiscarded > 0 || queuedDiscarded > 0) {
                LogInfo(
                    "[EncoderThread] WGC CFR exact-stop discarded pending frames: queued=%zu bufferedWgc=%zu "
                    "bufferedInject=%zu. "
                    "No post-stop CFR drain will be encoded.",
                    queuedDiscarded, bufferedDiscarded, bufferedInjectDiscarded);
            }
            break;
        }
        auto dropWgcVisualTimelineDebtToLiveWindow = [&](const char* reason) -> uint32_t {
            if (!activeScreenGrab || config.video.useVFR || !recordingOutputLive || targetIntervalTicks <= 0 ||
                qpcFreq.QuadPart <= 0) {
                return 0;
            }

            const bool encoderLimitedSmoothnessMode = isWgcEncoderLimitedSmoothnessMode();
            const uint32_t maxDebtTicks = ce::capture_policy::GetWgcLiveVisualDebtLimitTicksForMode(
                targetIntervalTicks, qpcFreq.QuadPart, encoderLimitedSmoothnessMode);
            if (maxDebtTicks == 0 || outputShortfallTicks <= maxDebtTicks) {
                return 0;
            }

            const uint32_t excessTicks = outputShortfallTicks - maxDebtTicks;
            wgcVisualDebtMaxExcessTicks = std::max<uint64_t>(wgcVisualDebtMaxExcessTicks, excessTicks);

            static uint64_t s_lastWgcTimelineDebtDropLogTick = 0;
            const uint64_t nowTick = GetTickCount64();
            if (nowTick - s_lastWgcTimelineDebtDropLogTick >= 1000 || excessTicks >= maxDebtTicks) {
                LogWarn(
                    "[EncoderThread] WGC CFR visual timeline debt drop: reason=%s mode=%s excessTicks=%u "
                    "maxDebtTicks=%u maxExcessTicks=%llu shortfall=%u",
                    reason ? reason : "unknown", encoderLimitedSmoothnessMode ? "encoder_limited" : "bounded_live",
                    excessTicks, maxDebtTicks, static_cast<unsigned long long>(wgcVisualDebtMaxExcessTicks),
                    outputShortfallTicks);
                s_lastWgcTimelineDebtDropLogTick = nowTick;
            }
            return excessTicks;
        };
        auto rebaseWgcLiveSchedulerToNow = [&](int64_t liveNowQpc) -> uint32_t {
            if (!activeScreenGrab || config.video.useVFR || !recordingOutputLive ||
                !g_Recording.load(std::memory_order_acquire) || liveStartQpc.QuadPart <= 0 ||
                targetIntervalTicks <= 0 || qpcFreq.QuadPart <= 0 || liveTicksOutput == 0) {
                return 0;
            }

            const bool encoderLimitedSmoothnessMode = isWgcEncoderLimitedSmoothnessMode();
            const uint32_t excessTicks = ce::capture_policy::GetWgcLiveVisualDebtExcessTicksForMode(
                outputShortfallTicks, targetIntervalTicks, qpcFreq.QuadPart, encoderLimitedSmoothnessMode);
            if (excessTicks == 0 || nextSampleTime.QuadPart <= 0 || liveNowQpc <= nextSampleTime.QuadPart) {
                return 0;
            }

            const uint32_t requestedTicks =
                SaturatingToUint32(static_cast<uint64_t>(liveNowQpc - nextSampleTime.QuadPart) /
                                   static_cast<uint64_t>(targetIntervalTicks));
            const uint32_t skippedTicks = ce::capture_policy::GetWgcLiveSchedulerRebaseTicksThisLoopForMode(
                requestedTicks, outputShortfallTicks, excessTicks, encoderLimitedSmoothnessMode);
            if (skippedTicks == 0) {
                return 0;
            }

            const int64_t oldNextSampleQpc = nextSampleTime.QuadPart;
            liveTicksOutput += skippedTicks;
            const int64_t gridHeadroom = std::numeric_limits<int64_t>::max() - encoderGridTickCount;
            if (gridHeadroom > 0) {
                encoderGridTickCount +=
                    static_cast<int64_t>(std::min<uint64_t>(skippedTicks, static_cast<uint64_t>(gridHeadroom)));
            }
            nextSampleTime.QuadPart += targetIntervalTicks * static_cast<int64_t>(skippedTicks);
            wgcLiveSchedulerRebaseTotal += skippedTicks;
            wgcLiveSchedulerRebaseThisWindow =
                SaturatingToUint32(static_cast<uint64_t>(wgcLiveSchedulerRebaseThisWindow) + skippedTicks);
            wgcLiveSchedulerRebaseMaxTicks = std::max(wgcLiveSchedulerRebaseMaxTicks, SaturatingToUint32(skippedTicks));
            wgcVisualDebtMaxExcessTicks = std::max<uint64_t>(wgcVisualDebtMaxExcessTicks, excessTicks);
            if (encoderLimitedSmoothnessMode) {
                wgcEncoderLimitedSourceDropThisWindow =
                    SaturatingToUint32(static_cast<uint64_t>(wgcEncoderLimitedSourceDropThisWindow) + skippedTicks);
                wgcEncoderLimitedSourceDropTotal += skippedTicks;
                wgcEncoderLimitedSourceDropMaxTicks =
                    std::max(wgcEncoderLimitedSourceDropMaxTicks, SaturatingToUint32(skippedTicks));
            }
            if (g_pSharedMem) {
                g_pSharedMem->runtimeState.timerRebases.fetch_add(1, std::memory_order_relaxed);
            }

            static uint64_t s_lastWgcLiveSchedulerRebaseLogTick = 0;
            const uint64_t nowTick = GetTickCount64();
            if (nowTick - s_lastWgcLiveSchedulerRebaseLogTick >= 1000 || skippedTicks >= 8) {
                LogWarn(
                    "[EncoderThread] WGC CFR live scheduler rebase: mode=%s skippedTicks=%llu excessTicks=%u "
                    "requestedTicks=%u shortfallBefore=%u nextQpc=%lld nextAfterQpc=%lld liveNowQpc=%lld "
                    "timelineCovered=%llu",
                    encoderLimitedSmoothnessMode ? "encoder_limited" : "bounded_live",
                    static_cast<unsigned long long>(skippedTicks), excessTicks, requestedTicks, outputShortfallTicks,
                    static_cast<long long>(oldNextSampleQpc), static_cast<long long>(nextSampleTime.QuadPart),
                    static_cast<long long>(liveNowQpc), static_cast<unsigned long long>(liveTicksOutput));
                s_lastWgcLiveSchedulerRebaseLogTick = nowTick;
            }
            if (encoderLimitedSmoothnessMode) {
                static uint64_t s_lastWgcEncoderLimitedDropLogTick = 0;
                const uint64_t nowTickDrop = GetTickCount64();
                if (nowTickDrop - s_lastWgcEncoderLimitedDropLogTick >= 1000 || skippedTicks > 1) {
                    LogWarn(
                        "[EncoderThread] WGC CFR encoder-limited source drop: skippedTicks=%llu excessTicks=%u "
                        "maxDebtTicks=%u shortfallBefore=%u total=%llu",
                        static_cast<unsigned long long>(skippedTicks), excessTicks,
                        ce::capture_policy::GetWgcLiveVisualDebtLimitTicksForMode(targetIntervalTicks, qpcFreq.QuadPart,
                                                                                  true),
                        outputShortfallTicks, static_cast<unsigned long long>(wgcEncoderLimitedSourceDropTotal));
                    s_lastWgcEncoderLimitedDropLogTick = nowTickDrop;
                }
            }
            return SaturatingToUint32(skippedTicks);
        };
        auto pruneStaleWgcVisualDebt = [&](int64_t liveNowQpc, const char* reason, bool allowDropAll) -> size_t {
            const bool encoderLimitedSmoothnessMode = isWgcEncoderLimitedSmoothnessMode();
            const int64_t visualDebtFloorQpc = ce::capture_policy::GetWgcLiveVisualDebtFloorQpcForMode(
                liveNowQpc, targetIntervalTicks, qpcFreq.QuadPart, encoderLimitedSmoothnessMode);
            if (visualDebtFloorQpc <= 0) {
                return 0;
            }

            size_t dropped = 0;
            uint64_t maxDebtUs = 0;
            while (!bufferedWgcFrames.empty()) {
                const int64_t selectionTimestampQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.front());
                if (selectionTimestampQpc <= 0 || selectionTimestampQpc >= visualDebtFloorQpc) {
                    break;
                }
                if (bufferedWgcFrames.size() == 1 && !allowDropAll) {
                    break;
                }

                if (qpcFreq.QuadPart > 0) {
                    maxDebtUs = std::max<uint64_t>(
                        maxDebtUs, static_cast<uint64_t>((visualDebtFloorQpc - selectionTimestampQpc) * 1000000 /
                                                         qpcFreq.QuadPart));
                }
                QueuedFrame stale = std::move(bufferedWgcFrames.front());
                bufferedWgcFrames.pop_front();
                ReleaseQueuedFrameTexture(stale);
                ++dropped;
                ++wgcDropStaleDebtCount;
                ++wgcDropStaleDebtTotal;
            }

            if (dropped > 0) {
                wgcDropStaleDebtMaxUs = std::max(wgcDropStaleDebtMaxUs, SaturatingToUint32(maxDebtUs));
                static uint64_t s_lastStaleWgcDebtLogTick = 0;
                const uint64_t nowTick = GetTickCount64();
                if (nowTick - s_lastStaleWgcDebtLogTick >= 1000 || dropped >= 8) {
                    LogWarn(
                        "[EncoderThread] WGC CFR stale visual debt drop: reason=%s mode=%s dropped=%zu floorQpc=%lld "
                        "liveNowQpc=%lld maxDebt=%lluus remaining=%zu shortfall=%u",
                        reason ? reason : "unknown", encoderLimitedSmoothnessMode ? "encoder_limited" : "bounded_live",
                        dropped, static_cast<long long>(visualDebtFloorQpc), static_cast<long long>(liveNowQpc),
                        static_cast<unsigned long long>(maxDebtUs), bufferedWgcFrames.size(), outputShortfallTicks);
                    s_lastStaleWgcDebtLogTick = nowTick;
                }
            }
            return dropped;
        };
        auto noteActivePathMismatchDiscard = [&](bool frameIsInjectMode, const char* source) {
            ++activePathMismatchDiscardThisWindow;
            ++activePathMismatchDiscardTotal;
            const uint64_t discarded = g_ActivePathMismatchFramesDiscarded.fetch_add(1, std::memory_order_relaxed) + 1;
            if (activePathMismatchDiscardThisWindow <= 3 || (discarded % 120ull) == 0ull) {
                LogWarn(
                    "[EncoderThread] Discarded %s frame on active %s path from %s (window=%u total=%llu). Preventing "
                    "mid-recording encoder mode switch.",
                    frameIsInjectMode ? "inject" : "WGC/D3D11", useScreenGrab ? "WGC" : "inject", source,
                    activePathMismatchDiscardThisWindow, static_cast<unsigned long long>(discarded));
            }
        };
        auto discardActivePathMismatchFrame = [&](QueuedFrame& mismatchedFrame, const char* source, bool queuedFrame) {
            noteActivePathMismatchDiscard(mismatchedFrame.isInjectMode, source);
            if (queuedFrame) {
                DiscardQueuedFrame(mismatchedFrame);
            } else {
                if (!mismatchedFrame.isInjectMode) {
                    ReleaseQueuedFrameTexture(mismatchedFrame);
                }
                mismatchedFrame = QueuedFrame{};
            }
        };
        if (!config.video.useVFR && recordingOutputLive) {
            LARGE_INTEGER shortfallNow;
            QueryPerformanceCounter(&shortfallNow);
            outputShortfallTicks = updateLiveCfrShortfall(shortfallNow.QuadPart);
            dropWgcVisualTimelineDebtToLiveWindow(g_Recording.load(std::memory_order_acquire) ? "live" : "drain");
            if (rebaseWgcLiveSchedulerToNow(shortfallNow.QuadPart) > 0) {
                outputShortfallTicks = updateLiveCfrShortfall(shortfallNow.QuadPart);
            }
        }
        if (!g_Recording.load(std::memory_order_acquire) && recordingOutputLive &&
            g_DrainOutstandingCfrTicks.load(std::memory_order_acquire)) {
            const bool mediaEngineCanRepeatLastFrame =
                MediaEngine_CanRepeatLastFrame && MediaEngine_CanRepeatLastFrame();
            if (activeScreenGrab && !bufferedWgcFrames.empty()) {
                int64_t drainPolicyQpc = g_CfrDrainStopQpc.load(std::memory_order_acquire);
                if (drainPolicyQpc <= 0) {
                    LARGE_INTEGER drainNowQpc;
                    QueryPerformanceCounter(&drainNowQpc);
                    drainPolicyQpc = drainNowQpc.QuadPart;
                }
                pruneStaleWgcVisualDebt(drainPolicyQpc, "stop-drain", g_HasLastFrame && mediaEngineCanRepeatLastFrame);
            }
            const bool bufferedFrameAvailable =
                activeScreenGrab ? !bufferedWgcFrames.empty() : !bufferedInjectFrames.empty();
            const size_t bufferedFrameCount = activeScreenGrab ? bufferedWgcFrames.size() : bufferedInjectFrames.size();
            const bool canDrainOutstandingTicks = ce::capture_policy::CanDrainOutstandingCfrTicks(
                activeScreenGrab, g_FrameQueue.Size() > 0, bufferedFrameAvailable, g_HasLastFrame,
                mediaEngineCanRepeatLastFrame);
            static uint64_t s_lastStopDrainProgressLogTick = 0;
            const uint64_t nowTick = GetTickCount64();
            if (outputShortfallTicks > 0 && nowTick - s_lastStopDrainProgressLogTick >= 5000) {
                LogInfo(
                    "[EncoderThread] CFR stop drain progress: shortfall=%u/%.1fms queue=%u buffered=%zu hostLast=%d "
                    "cachedRepeat=%d",
                    outputShortfallTicks,
                    ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs),
                    static_cast<unsigned>(g_FrameQueue.Size()), bufferedFrameCount, g_HasLastFrame ? 1 : 0,
                    mediaEngineCanRepeatLastFrame ? 1 : 0);
                s_lastStopDrainProgressLogTick = nowTick;
            }
            if (activeScreenGrab && outputShortfallTicks > 0 && !bufferedFrameAvailable && g_FrameQueue.Size() == 0 &&
                g_HasLastFrame && mediaEngineCanRepeatLastFrame && !wgcStopDrainHeldFrameLogged) {
                LogWarn(
                    "[EncoderThread] WGC CFR stop drain using held pre-stop frame: holdTicks=%u/%.1fms "
                    "queued=0 buffered=0. Audio endpoint is preserved; this is visual hold debt, not audio recovery.",
                    outputShortfallTicks,
                    ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs));
                wgcStopDrainHeldFrameLogged = true;
            }
            if (outputShortfallTicks == 0 || !canDrainOutstandingTicks) {
                if (outputShortfallTicks == 0) {
                    LogInfo("[EncoderThread] CFR stop drain complete: scheduled=%llu output=%llu",
                            static_cast<unsigned long long>(liveTicksScheduled),
                            static_cast<unsigned long long>(liveTicksOutput));
                } else {
                    LogWarn(
                        "[EncoderThread] CFR stop drain aborted: no captured frame/repeat available for outstanding "
                        "shortfall=%u/%.1fms "
                        "(queue=%u buffered=%zu hostLast=%d cachedRepeat=%d; cached repeats close only accrued "
                        "CFR debt)",
                        outputShortfallTicks,
                        ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs),
                        static_cast<unsigned>(g_FrameQueue.Size()), bufferedFrameCount, g_HasLastFrame ? 1 : 0,
                        mediaEngineCanRepeatLastFrame ? 1 : 0);
                }
                s_lastStopDrainProgressLogTick = 0;
                g_DrainOutstandingCfrTicks.store(false, std::memory_order_release);
            }
        }

        const bool frameAvailableForCatchup =
            activeScreenGrab ? (!bufferedWgcFrames.empty()) : (!bufferedInjectFrames.empty());
        bool shouldCatchUpToWallClock = false;
        uint32_t catchupTicksThisLoop = 0;
        const auto loadWgcAudioLeadExcessMs = [&]() -> double {
            if (!g_pSharedMem) {
                return 0.0;
            }
            const uint32_t audioLeadExcessSamples =
                g_pSharedMem->runtimeState.wgcAudioLeadExcessSamples.load(std::memory_order_relaxed);
            return static_cast<double>(audioLeadExcessSamples) * 1000.0 / 48000.0;
        };
        const auto computeWgcCoverageRepeatActive = [&](double audioLeadExcessMs) {
            if (!activeScreenGrab || !recordingOutputLive) {
                return false;
            }
            const double shortfallDurationMs =
                ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs);
            const double oldestBufferedFrameAgeMs = static_cast<double>(wgcOldestBufferedFrameAgeUs) / 1000.0;
            uint32_t effectiveDeliveredFps = wgcRecentDeliveredFps;
            if (wgcRecentDeliveredMin250Fps > 0) {
                effectiveDeliveredFps = std::min(effectiveDeliveredFps, wgcRecentDeliveredMin250Fps);
            }
            if (wgcRecentDeliveredMin500Fps > 0) {
                effectiveDeliveredFps = std::min(effectiveDeliveredFps, wgcRecentDeliveredMin500Fps);
            }
            if (ce::capture_policy::ShouldSuppressWgcCoverageLossForEncoderBottleneck(
                    g_IsEncoderBottlenecked.load(std::memory_order_relaxed), effectiveDeliveredFps,
                    std::max<uint32_t>(1u, static_cast<uint32_t>(config.video.fps > 0 ? config.video.fps : 1)))) {
                return false;
            }
            return ce::capture_policy::HasWgcUnrecoverableCoverageLoss(shortfallDurationMs, oldestBufferedFrameAgeMs,
                                                                       audioLeadExcessMs);
        };
        auto recomputeCatchupPolicy = [&]() {
            const uint32_t targetOutputFpsForPolicy = std::max<uint32_t>(1u, static_cast<uint32_t>(config.video.fps));
            encoderTooSlowForTargetCurrent = ce::capture_policy::IsEncoderTooSlowForTargetFps(
                smoothedEncodeMs, frameIntervalMs, targetOutputFpsForPolicy);
            const bool encoderCatchupBottleneckedCurrent =
                encoderTooSlowForTargetCurrent || g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
            const double shortfallDurationMs =
                ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs);
            wgcAudioLeadExcessMsCurrent = loadWgcAudioLeadExcessMs();
            const bool wgcAudioLeadCatchupPressure =
                ce::capture_policy::ShouldPrioritizeWgcAudioLeadCatchup(wgcAudioLeadExcessMsCurrent);
            wgcCoverageRepeatActiveCurrent = computeWgcCoverageRepeatActive(wgcAudioLeadExcessMsCurrent);
            shouldCatchUpToWallClock =
                !config.video.useVFR && recordingOutputLive &&
                ce::capture_policy::ShouldCfrCatchUpToWallClock(outputShortfallTicks, activeScreenGrab,
                                                                frameAvailableForCatchup, g_HasLastFrame);
            if (!shouldCatchUpToWallClock) {
                catchupTicksThisLoop = 0u;
            } else if (activeScreenGrab) {
                catchupTicksThisLoop = ce::capture_policy::GetWgcCatchupTicksThisLoop(
                    encoderCatchupBottleneckedCurrent, encoderTooSlowForTargetCurrent, bufferedWgcFrames.size(),
                    frameCreditAccumulator, outputShortfallTicks, targetOutputFpsForPolicy, wgcRecentDeliveredMin250Fps,
                    wgcRecentInputMin250Fps, wgcNoFreshTickPermille, wgcLowSourceModeActive,
                    wgcAudioLeadExcessMsCurrent);
            } else {
                catchupTicksThisLoop = ce::capture_policy::GetInjectCfrCatchupTicksThisLoop(
                    outputShortfallTicks, encoderTooSlowForTargetCurrent);
            }
            if (activeScreenGrab && wgcLiveRecoveryModeActive && !wgcAudioLeadCatchupPressure) {
                catchupTicksThisLoop = std::min<uint32_t>(catchupTicksThisLoop, 1u);
            }
            if (activeScreenGrab &&
                ce::capture_policy::ShouldClampWgcCoverageCatchupToSingleTick(
                    wgcCoverageRepeatActiveCurrent, encoderCatchupBottleneckedCurrent, shortfallDurationMs)) {
                catchupTicksThisLoop = std::min<uint32_t>(catchupTicksThisLoop, 1u);
            }
        };
        recomputeCatchupPolicy();

        const int64_t selectionGridTick =
            (!config.video.useVFR && recordingOutputLive) ? (encoderGridTickCount + 1) : encoderGridTickCount;
        int64_t scheduledSampleQpc = 0;
        int64_t encoderLateQpc = 0;
        uint32_t encoderLateTickCount = 0;
        bool drainingOutstandingLiveTicks = !g_EncoderRunning &&
                                            g_DrainOutstandingCfrTicks.load(std::memory_order_acquire) &&
                                            recordingOutputLive && !config.video.useVFR;
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
            } else {
                nextSampleTime.QuadPart += targetIntervalTicks;
                cycleStartQpc = now;
            }

            if (g_EncoderRunning) {
                // Periodically resync the encoder grid to wall clock time to
                // prevent systematic drift when encoder ticks are consistently
                // longer than the target interval.  Without this, the selection
                // target grows increasingly out of sync with actual frame times.
                if (recordingOutputLive && encoderGridStartQpc > 0 && targetIntervalTicks > 0 && liveTicksOutput > 0 &&
                    (liveTicksOutput % 60 == 0) && outputShortfallTicks < 2) {
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
                const uint32_t timerRebaseThreshold = ce::capture_policy::GetCfrTimerRebaseThresholdTicks(
                    activeScreenGrab, config.video.useVFR, recordingOutputLive);
                if (!recordingOutputLive && now.QuadPart > nextSampleTime.QuadPart + targetIntervalTicks * 2) {
                    nextSampleTime = now;
                } else if (recordingOutputLive && encoderLateTickCount >= timerRebaseThreshold) {
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
                            (long long)overshootTicks, (long long)overshootUs, s_lateTickLogCount,
                            droppedShortfallTicks, static_cast<unsigned long long>(liveTicksDiscardedByTimerRebase),
                            discardTimerDebt ? 0u : 1u);
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
            return ce::capture_policy::GetWgcSelectionTargetQpc(
                scheduledQpcForTick, fallbackTargetQpc, targetIntervalTicks,
                recordingOutputLive && applyLiveDelay && !wgcLiveRecoveryModeActive, avContentDelayQpc);
        };
        const auto computeWgcSelectionTargetQpc = [&](bool applyLiveDelay) {
            return computeWgcSelectionTargetForTick(scheduledSampleQpc, selectionGridTick, applyLiveDelay);
        };
        const auto computeLiveWgcSelectionTargetQpc = [&]() { return computeWgcSelectionTargetQpc(false); };
        const auto computeDelayedWgcSelectionTargetQpc = [&]() { return computeWgcSelectionTargetQpc(true); };
        const auto clampWgcSelectionTargetQpc = [&](int64_t selectionTargetQpc, int64_t liveNowQpc) {
            const bool encoderBottlenecked = g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
            const int64_t clampedSelectionTargetQpc = ce::capture_policy::ClampWgcSelectionTargetToLiveQpc(
                selectionTargetQpc, liveNowQpc, targetIntervalTicks, qpcFreq.QuadPart, wgcLowSourceModeActive,
                wgcLiveRecoveryModeActive, outputShortfallTicks, encoderBottlenecked,
                ce::capture_policy::kCfrShortfallCatchupThresholdTicks, isWgcEncoderLimitedSmoothnessMode());
            if (clampedSelectionTargetQpc > selectionTargetQpc) {
                const uint64_t clampDeltaUs = static_cast<uint64_t>(clampedSelectionTargetQpc - selectionTargetQpc) *
                                              1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
                ++wgcSelectionTargetClampCount;
                wgcSelectionTargetClampMaxUs = std::max(wgcSelectionTargetClampMaxUs, SaturatingToUint32(clampDeltaUs));
            }
            return clampedSelectionTargetQpc;
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
        bool wgcSelectionDelayAppliedThisTick = false;
        auto tryPopBufferedWgcFrameForTarget = [&](int64_t selectionTargetQpc, int64_t liveSelectionTargetQpc,
                                                   int64_t liveNowQpc, bool selectionDelayApplied,
                                                   QueuedFrame* selectedFrame,
                                                   bool* repeatedBecauseNoFrameCoverage = nullptr) {
            if (repeatedBecauseNoFrameCoverage) {
                *repeatedBecauseNoFrameCoverage = false;
            }
            if (!selectedFrame || bufferedWgcFrames.empty()) {
                return false;
            }

            pruneStaleWgcVisualDebt(liveNowQpc, "selection", g_HasLastFrame && !g_LastFrame.isInjectMode);

            const bool lowSourceMode = wgcLowSourceModeActive;
            const bool deepUnderfeed = ce::capture_policy::IsWgcDeepUnderfeed(
                outputFps, wgcRecentDeliveredMin250Fps, wgcRecentInputMin250Fps, wgcNoFreshTickPermille);
            const int64_t effectiveSelectionTargetQpc =
                selectionTargetQpc > 0 ? selectionTargetQpc : liveSelectionTargetQpc;
            const int64_t minFreshTimestampQpc = ce::capture_policy::GetWgcMinimumFreshTimestampQpc(
                lastEmittedWgcSourceQpc, liveSelectionTargetQpc, targetIntervalTicks, lowSourceMode);
            const int64_t staleFallbackMinTimestampQpc = ce::capture_policy::GetWgcStaleUniqueFallbackMinTimestampQpc(
                lastEmittedWgcSourceQpc, effectiveSelectionTargetQpc, targetIntervalTicks, lowSourceMode,
                deepUnderfeed);

            while (bufferedWgcFrames.size() > 1) {
                const QueuedFrame& current = bufferedWgcFrames[0];
                const QueuedFrame& next = bufferedWgcFrames[1];
                const bool sameTimestamp = current.timestamp > 0 && current.timestamp == next.timestamp;
                const bool sameSelectionTimestamp =
                    current.selectionTimestamp > 0 && current.selectionTimestamp == next.selectionTimestamp;
                const bool duplicateSelectionCandidate = sameTimestamp || sameSelectionTimestamp;
                const bool currentTooOld = staleFallbackMinTimestampQpc > 0 && current.timestamp > 0 &&
                                           current.timestamp < staleFallbackMinTimestampQpc &&
                                           next.timestamp > current.timestamp;
                if (!duplicateSelectionCandidate && !currentTooOld) {
                    break;
                }

                QueuedFrame obsolete = std::move(bufferedWgcFrames.front());
                bufferedWgcFrames.pop_front();
                ReleaseQueuedFrameTexture(obsolete);
                ++wgcDropObsoleteCount;
            }

            if (bufferedWgcFrames.empty()) {
                ++wgcFreshSelectionMissCount;
                if (repeatedBecauseNoFrameCoverage) {
                    *repeatedBecauseNoFrameCoverage = true;
                }
                return false;
            }

            bool skippedTooNewForSlot = false;
            auto buildCandidateList = [&](std::vector<size_t>* outIndices, bool requireFresh) {
                if (!outIndices) {
                    return;
                }
                outIndices->clear();
                for (size_t i = 0; i < bufferedWgcFrames.size(); ++i) {
                    const QueuedFrame& candidate = bufferedWgcFrames[i];
                    if (candidate.timestamp <= 0) {
                        continue;
                    }
                    const bool sourceTimestampAdvanced = candidate.timestamp > lastEmittedWgcSourceQpc;
                    const bool freshEnough =
                        ce::capture_policy::IsWgcTimestampFreshEnough(candidate.timestamp, minFreshTimestampQpc);
                    const bool fallbackFreshEnough = ce::capture_policy::IsWgcTimestampFreshEnough(
                        candidate.timestamp, staleFallbackMinTimestampQpc);
                    const int64_t candidateSelectionTimestamp = GetFrameSelectionTimestamp(candidate);
                    const bool tooNewForSlot =
                        g_HasLastFrame && !g_LastFrame.isInjectMode &&
                        ce::capture_policy::IsWgcFrameTooNewForCfrSlot(
                            candidateSelectionTimestamp, effectiveSelectionTargetQpc, targetIntervalTicks);
                    if (tooNewForSlot) {
                        skippedTooNewForSlot = true;
                        continue;
                    }
                    if (requireFresh) {
                        if (!(sourceTimestampAdvanced && freshEnough)) {
                            continue;
                        }
                    } else {
                        if (!(sourceTimestampAdvanced && fallbackFreshEnough)) {
                            continue;
                        }
                    }
                    outIndices->push_back(i);
                }
            };

            buildCandidateList(&wgcFreshCandidateIndices, true);
            if (wgcFreshCandidateIndices.empty()) {
                buildCandidateList(&wgcFallbackCandidateIndices, false);
            }

            std::vector<size_t>* candidateIndices =
                !wgcFreshCandidateIndices.empty() ? &wgcFreshCandidateIndices : &wgcFallbackCandidateIndices;
            const bool usingFreshCandidateSet = !wgcFreshCandidateIndices.empty();

            if (candidateIndices->empty()) {
                ++wgcFreshSelectionMissCount;
                if (skippedTooNewForSlot) {
                    ++wgcRepeatPolicyHoldCount;
                    ++wgcRepeatPolicyHoldTotal;
                    static uint64_t s_lastTooNewWgcSelectionLogTick = 0;
                    const uint64_t nowTick = GetTickCount64();
                    if (nowTick - s_lastTooNewWgcSelectionLogTick >= 1000) {
                        const int64_t firstSelectionTimestamp =
                            !bufferedWgcFrames.empty() ? GetFrameSelectionTimestamp(bufferedWgcFrames.front()) : 0;
                        int64_t leadUs = 0;
                        if (qpcFreq.QuadPart > 0 && firstSelectionTimestamp > effectiveSelectionTargetQpc) {
                            leadUs =
                                ((firstSelectionTimestamp - effectiveSelectionTargetQpc) * 1000000) / qpcFreq.QuadPart;
                        }
                        LogInfo(
                            "[EncoderThread] WGC CFR slot repeat: buffered frame is too new for scheduled slot "
                            "(lead=%lldus targetQpc=%lld firstQpc=%lld buffered=%zu shortfall=%u)",
                            static_cast<long long>(leadUs), static_cast<long long>(effectiveSelectionTargetQpc),
                            static_cast<long long>(firstSelectionTimestamp), bufferedWgcFrames.size(),
                            outputShortfallTicks);
                        s_lastTooNewWgcSelectionLogTick = nowTick;
                    }
                }
                if (repeatedBecauseNoFrameCoverage) {
                    *repeatedBecauseNoFrameCoverage = true;
                }
                return false;
            }

            size_t selectedIndex = candidateIndices->front();
            if (effectiveSelectionTargetQpc > 0) {
                size_t bestCandidateOffset = 0;
                int64_t bestDistance = AbsoluteTimestampDistance(
                    GetFrameSelectionTimestamp(bufferedWgcFrames[(*candidateIndices)[0]]), effectiveSelectionTargetQpc);
                for (size_t candidateOffset = 1; candidateOffset < candidateIndices->size(); ++candidateOffset) {
                    const size_t bufferedIndex = (*candidateIndices)[candidateOffset];
                    const int64_t candidateDistance = AbsoluteTimestampDistance(
                        GetFrameSelectionTimestamp(bufferedWgcFrames[bufferedIndex]), effectiveSelectionTargetQpc);
                    if (candidateDistance < bestDistance ||
                        (candidateDistance == bestDistance &&
                         GetFrameSelectionTimestamp(bufferedWgcFrames[bufferedIndex]) >
                             GetFrameSelectionTimestamp(bufferedWgcFrames[(*candidateIndices)[bestCandidateOffset]]))) {
                        bestDistance = candidateDistance;
                        bestCandidateOffset = candidateOffset;
                    }
                }
                selectedIndex = (*candidateIndices)[bestCandidateOffset];
            }

            selectedIndex = ce::capture_policy::ClampWgcSelectionIndexForLowSource(
                selectedIndex, bufferedWgcFrames.size(), bufferedWgcFrames.size(), wgcRecentDeliveredFps,
                wgcRecentInputMin250Fps, outputFps, wgcNoFreshTickPermille, wgcLiveRecoveryModeActive);

            if (usingFreshCandidateSet && selectedIndex > 0) {
                const QueuedFrame& earlierFresh = bufferedWgcFrames[0];
                const QueuedFrame& chosenFresh = bufferedWgcFrames[selectedIndex];
                if (earlierFresh.timestamp > lastEmittedWgcSourceQpc &&
                    chosenFresh.timestamp > earlierFresh.timestamp &&
                    ce::capture_policy::ShouldPreferEarlierFreshWgcFrameToPreserveReserve(
                        earlierFresh.selectionTimestamp > 0 ? earlierFresh.selectionTimestamp : earlierFresh.timestamp,
                        chosenFresh.selectionTimestamp > 0 ? chosenFresh.selectionTimestamp : chosenFresh.timestamp,
                        effectiveSelectionTargetQpc, targetIntervalTicks, wgcReservePressureActive, lowSourceMode,
                        deepUnderfeed, wgcLiveRecoveryModeActive)) {
                    selectedIndex = 0;
                } else {
                    ++wgcReserveSpendTickCount;
                }
            }

            const QueuedFrame& candidate = bufferedWgcFrames[selectedIndex];
            const bool encoderLimitedSmoothnessForBacktrack = isWgcEncoderLimitedSmoothnessMode();
            if (candidate.timestamp > 0 && lastEmittedWgcSourceQpc > 0 &&
                (candidate.timestamp < lastEmittedWgcSourceQpc ||
                 (encoderLimitedSmoothnessForBacktrack && candidate.timestamp == lastEmittedWgcSourceQpc))) {
                ++wgcSelectedSourceBacktrackThisWindow;
                ++wgcSelectedSourceBacktrackTotal;
                static uint64_t s_lastWgcBacktrackLogTick = 0;
                const uint64_t nowTick = GetTickCount64();
                if (nowTick - s_lastWgcBacktrackLogTick >= 1000) {
                    LogWarn(
                        "[EncoderThread] WGC CFR selected source backtrack blocked: candidateQpc=%lld "
                        "lastEmittedQpc=%lld selectedIndex=%zu buffered=%zu mode=%s shortfall=%u",
                        static_cast<long long>(candidate.timestamp), static_cast<long long>(lastEmittedWgcSourceQpc),
                        selectedIndex, bufferedWgcFrames.size(),
                        encoderLimitedSmoothnessForBacktrack ? "encoder_limited" : "normal", outputShortfallTicks);
                    s_lastWgcBacktrackLogTick = nowTick;
                }
                if (repeatedBecauseNoFrameCoverage) {
                    *repeatedBecauseNoFrameCoverage = true;
                }
                return false;
            }
            const int64_t candidateSelectionTimestamp =
                candidate.selectionTimestamp > 0 ? candidate.selectionTimestamp : candidate.timestamp;
            const bool canHoldFreshFrame =
                selectionDelayApplied && selectedIndex == 0 && bufferedWgcFrames.size() == 1 &&
                ce::capture_policy::ShouldHoldSingleFreshWgcFrame(
                    wgcReservePressureActive, lowSourceMode, wgcRecentInputMin250Fps, outputFps, smoothedInputPerTick,
                    outputShortfallTicks, g_IsEncoderBottlenecked.load(std::memory_order_relaxed), false,
                    deepUnderfeed);
            if (canHoldFreshFrame && effectiveSelectionTargetQpc > 0 &&
                ShouldHoldFrameForNextTick(candidateSelectionTimestamp, effectiveSelectionTargetQpc,
                                           targetIntervalTicks, targetIntervalTicks / 10)) {
                ++wgcHoldForNextTickCount;
                ++wgcHeldFreshFrameTickCount;
                if (repeatedBecauseNoFrameCoverage) {
                    *repeatedBecauseNoFrameCoverage = true;
                }
                return false;
            }

            /*
            if (liveSelectionTargetQpc > 0 && candidateSelectionTimestamp > liveSelectionTargetQpc && g_HasLastFrame &&
                !g_LastFrame.isInjectMode) {
                ++wgcFreshSelectionMissCount;
                if (repeatedBecauseNoFrameCoverage) {
                    *repeatedBecauseNoFrameCoverage = true;
                }
                return false;
            }
            */

            if (!usingFreshCandidateSet) {
                ++wgcStaleUniqueFallbackCount;
                if (effectiveSelectionTargetQpc > 0 &&
                    candidateSelectionTimestamp + targetIntervalTicks < effectiveSelectionTargetQpc) {
                    ++wgcAncientSelectionCount;
                }
            }

            // Frame age limit: when the encoder is severely behind, reject frames that are
            // too old.  This prevents encoding ancient content (e.g., 17 seconds old) that
            // doesn't match the intended output position. Instead, we emit a duplicate frame
            // which "stutters honestly" while the encoder recovers.
            constexpr int64_t kMaxFrameAgeMs = 1000;  // 1 second maximum frame age
            if (encoderTooSlowForTargetCurrent && effectiveSelectionTargetQpc > 0 && qpcFreq.QuadPart > 0) {
                const int64_t frameAgeTicks = effectiveSelectionTargetQpc - candidateSelectionTimestamp;
                const int64_t frameAgeMs = (frameAgeTicks * 1000) / qpcFreq.QuadPart;
                if (frameAgeMs > kMaxFrameAgeMs) {
                    ++wgcFreshSelectionMissCount;
                    if (repeatedBecauseNoFrameCoverage) {
                        *repeatedBecauseNoFrameCoverage = true;
                    }
                    return false;
                }
            }

            for (size_t i = 0; i < selectedIndex; ++i) {
                QueuedFrame obsolete = std::move(bufferedWgcFrames.front());
                bufferedWgcFrames.pop_front();
                ReleaseQueuedFrameTexture(obsolete);
                ++wgcDropObsoleteCount;
            }

            *selectedFrame = std::move(bufferedWgcFrames.front());
            bufferedWgcFrames.pop_front();
            if (selectedFrame->duplicateSourceTimestamp) {
                ++wgcSelectDuplicateSourceCount;
            } else {
                ++wgcSelectFreshCount;
            }

            return true;
        };

        auto inspectBufferedWgcCoverageForTarget = [&](int64_t selectionTargetQpc, bool* hasFrameForTick,
                                                       bool* hasReserveFrame) {
            if (hasFrameForTick) {
                *hasFrameForTick = false;
            }
            if (hasReserveFrame) {
                *hasReserveFrame = false;
            }
            if (bufferedWgcFrames.empty()) {
                return;
            }

            size_t idx = 0;
            if (selectionTargetQpc > 0) {
                while ((idx + 1) < bufferedWgcFrames.size()) {
                    const QueuedFrame& current = bufferedWgcFrames[idx];
                    const QueuedFrame& next = bufferedWgcFrames[idx + 1];
                    const bool sameTimestamp = current.timestamp > 0 && current.timestamp == next.timestamp;
                    const bool nextAlreadyCoversTarget = next.timestamp > 0 && next.timestamp <= selectionTargetQpc;
                    if (!sameTimestamp && !nextAlreadyCoversTarget) {
                        break;
                    }
                    ++idx;
                }
            }

            if (idx >= bufferedWgcFrames.size()) {
                return;
            }

            const QueuedFrame& candidate = bufferedWgcFrames[idx];
            const int64_t candidateSelectionTimestamp = GetFrameSelectionTimestamp(candidate);
            const bool canUseCandidateNow = selectionTargetQpc <= 0 || candidateSelectionTimestamp <= 0 ||
                                            !ce::capture_policy::IsWgcFrameTooNewForCfrSlot(
                                                candidateSelectionTimestamp, selectionTargetQpc, targetIntervalTicks) ||
                                            !g_HasLastFrame || g_LastFrame.isInjectMode;
            if (hasFrameForTick) {
                *hasFrameForTick = canUseCandidateNow;
            }
            if (hasReserveFrame) {
                *hasReserveFrame = canUseCandidateNow && ((idx + 1) < bufferedWgcFrames.size());
            }
        };

        if (useScreenGrab) {
            if (!bufferedInjectFrames.empty()) {
                ClearBufferedInjectFrames();
            }
            smoothedInjectFenceMs = 0.0;
            if (!config.video.useVFR) {
                // CFR WGC: drain all pending captured frames and let the CFR slot
                // scheduler be the only authority for selection/repeat/drop.
                drainedScreenGrabFrames.clear();
                drainedWgcCapturedFrames.clear();
                if (g_WgcCap) {
                    g_WgcCap->DrainPendingFrames(drainedWgcCapturedFrames, 0);
                    for (auto& capturedFrame : drainedWgcCapturedFrames) {
                        if (capturedFrame.texture) {
                            drainedScreenGrabFrames.push_back(MakeQueuedWgcFrame(std::move(capturedFrame)));
                        }
                    }
                }

                QueuedFrame temp;
                while (g_FrameQueue.Pop(temp, 0)) {
                    DiscardQueuedFrame(temp);
                }

                const bool sampleWgcCadenceTick = !(recordingOutputLive && encoderLateTickCount > 0);

                // Phase 1: keep only frames that belong to the recording interval,
                // then feed raw timestamps to predictor BEFORE moving frames to the
                // buffer (std::move invalidates source object). Always feed the
                // predictor (even when encoder is late) so it can calibrate the
                // source FPS for Bresenham pacing and logging.
                if (!drainedScreenGrabFrames.empty()) {
                    const int64_t stopBoundaryQpc = !g_Recording.load(std::memory_order_acquire)
                                                        ? g_CfrDrainStopQpc.load(std::memory_order_acquire)
                                                        : 0;
                    size_t postStopDropped = 0;
                    uint32_t postStopMaxLeadUs = 0;
                    std::vector<QueuedFrame> keptFrames;
                    keptFrames.reserve(drainedScreenGrabFrames.size());
                    for (auto& drainedFrame : drainedScreenGrabFrames) {
                        const int64_t sourceFrameQpc =
                            drainedFrame.rawTimestamp > 0 ? drainedFrame.rawTimestamp : drainedFrame.timestamp;
                        if (!ce::capture_policy::ShouldKeepWgcFrameForStopDrain(sourceFrameQpc, stopBoundaryQpc)) {
                            if (qpcFreq.QuadPart > 0 && sourceFrameQpc > stopBoundaryQpc) {
                                const uint64_t leadUs = static_cast<uint64_t>(sourceFrameQpc - stopBoundaryQpc) *
                                                        1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
                                postStopMaxLeadUs = std::max(postStopMaxLeadUs, SaturatingToUint32(leadUs));
                            }
                            ReleaseQueuedFrameTexture(drainedFrame);
                            ++postStopDropped;
                            ++wgcPostStopFrameDropTotal;
                            continue;
                        }

                        if (!drainedFrame.isInjectMode && drainedFrame.timestamp > 0) {
                            wgcInputPredictor.Update(drainedFrame.timestamp, qpcFreq.QuadPart);
                            drainedFrame.selectionTimestamp =
                                wgcInputPredictor.GetIdealTimestamp(drainedFrame.timestamp);
                            static int64_t s_lastWgcSrcQpc = 0;
                            if (drainedFrame.timestamp == s_lastWgcSrcQpc) {
                                ++dupTimestampCount;
                            }
                            s_lastWgcSrcQpc = drainedFrame.timestamp;
                        }
                        keptFrames.push_back(std::move(drainedFrame));
                    }
                    if (postStopDropped > 0) {
                        wgcPostStopFrameDropMaxUs = std::max(wgcPostStopFrameDropMaxUs, postStopMaxLeadUs);
                        static uint64_t s_lastPostStopWgcDropLogTick = 0;
                        const uint64_t nowTick = GetTickCount64();
                        if (nowTick - s_lastPostStopWgcDropLogTick >= 1000 || postStopDropped >= 4) {
                            LogWarn(
                                "[EncoderThread] WGC CFR post-stop frame drop: dropped=%zu stopQpc=%lld maxLead=%uus "
                                "total=%llu",
                                postStopDropped, static_cast<long long>(stopBoundaryQpc), postStopMaxLeadUs,
                                static_cast<unsigned long long>(wgcPostStopFrameDropTotal));
                            s_lastPostStopWgcDropLogTick = nowTick;
                        }
                    }
                    drainedScreenGrabFrames.swap(keptFrames);
                }

                // Phase 2: append newly drained WGC frames and keep the host-side
                // reserve shallow. This restores timestamp-aware selection for
                // >target source cadence without letting callback bursts create a
                // deep unstable queue.
                for (auto& drainedFrame : drainedScreenGrabFrames) {
                    bufferedWgcFrames.push_back(std::move(drainedFrame));
                }

                if (recordingOutputLive && !bufferedWgcFrames.empty()) {
                    LARGE_INTEGER visualDebtNowQpc;
                    QueryPerformanceCounter(&visualDebtNowQpc);
                    int64_t visualDebtPolicyQpc = visualDebtNowQpc.QuadPart;
                    if (!g_Recording.load(std::memory_order_acquire)) {
                        const int64_t drainStopQpc = g_CfrDrainStopQpc.load(std::memory_order_acquire);
                        if (drainStopQpc > 0) {
                            visualDebtPolicyQpc = drainStopQpc;
                        }
                    }
                    pruneStaleWgcVisualDebt(visualDebtPolicyQpc, "live-buffer",
                                            g_HasLastFrame && !g_LastFrame.isInjectMode);
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

                wgcCoverageDelayTicksCurrent = 0;

                const uint64_t wgcPolicyNowTick = GetTickCount64();
                const ce::capture_policy::WgcAdaptiveTelemetry wgcAdaptiveTelemetry = {
                    outputFps,
                    wgcRecentDeliveredFps,
                    wgcRecentDeliveredMin250Fps,
                    wgcRecentDeliveredMin500Fps,
                    wgcRecentInputMin250Fps,
                    wgcRecentInputMin500Fps,
                    0u,
                    wgcNoFreshTickPermille,
                    static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull)),
                    0u,
                    0.0,
                };
                const bool allowWgcLiveRecoveryMode =
                    recordingOutputLive && g_Recording.load(std::memory_order_acquire);
                const bool wgcSourceHealthTelemetryReady =
                    allowWgcLiveRecoveryMode && liveTicksOutput >= std::max<uint64_t>(8ull, outputFps / 8u) &&
                    wgcRecentDeliveredMin250Fps > 0 && wgcRecentDeliveredMin500Fps > 0 && wgcRecentInputMin250Fps > 0 &&
                    wgcRecentInputMin500Fps > 0;
                const bool wgcCapacityPressureForRecovery = isWgcCapacityPressureActive();
                const ce::capture_policy::WgcLiveRecoveryState wgcLiveRecoveryStateCurrent =
                    wgcSourceHealthTelemetryReady
                        ? ce::capture_policy::ClassifyWgcLiveRecoveryState(wgcAdaptiveTelemetry, outputShortfallTicks,
                                                                           wgcCapacityPressureForRecovery)
                        : ce::capture_policy::WgcLiveRecoveryState::kHealthy;
                wgcSourceStarvedCurrent =
                    allowWgcLiveRecoveryMode &&
                    wgcLiveRecoveryStateCurrent == ce::capture_policy::WgcLiveRecoveryState::kSourceStarved;
                wgcSchedulerLimitedCurrent =
                    allowWgcLiveRecoveryMode &&
                    wgcLiveRecoveryStateCurrent == ce::capture_policy::WgcLiveRecoveryState::kSchedulerLimited;
                wgcEncoderRecoveryLimitedCurrent =
                    allowWgcLiveRecoveryMode &&
                    wgcLiveRecoveryStateCurrent == ce::capture_policy::WgcLiveRecoveryState::kEncoderLimited;
                wgcReservePressureActive = ce::capture_policy::IsWgcReservePressureActive(
                    wgcNoReserveTickCount, wgcQueueTickSampleCount, outputFps);
                const ce::capture_policy::WgcLowSourceState wgcLowSourceStateCurrent =
                    wgcSourceHealthTelemetryReady ? ce::capture_policy::ClassifyWgcLowSourceState(wgcAdaptiveTelemetry)
                                                  : ce::capture_policy::WgcLowSourceState::kHealthy;
                const bool shouldEnterWgcLowSourceMode =
                    wgcLowSourceStateCurrent != ce::capture_policy::WgcLowSourceState::kHealthy;
                const bool bufferedReserveRecovered = bufferedWgcFrames.size() >= 3;
                const bool shouldExitWgcLowSourceMode = ce::capture_policy::ShouldExitWgcLowSourceMode(
                    wgcAdaptiveTelemetry, encoderTooSlowForTargetCurrent, bufferedReserveRecovered);
                const auto wgcLowSourceModeUpdate = ce::capture_policy::UpdateHeldMode(
                    wgcLowSourceModeActive, wgcLowSourceStateChangeTick, wgcPolicyNowTick, shouldEnterWgcLowSourceMode,
                    shouldExitWgcLowSourceMode, !encoderTooSlowForTargetCurrent && bufferedReserveRecovered,
                    ce::capture_policy::kWgcLowSourceEnterHoldMs, ce::capture_policy::kWgcLowSourceExitHoldMs);
                wgcLowSourceModeActive = wgcLowSourceModeUpdate.active;
                wgcLowSourceStateChangeTick = wgcLowSourceModeUpdate.stateChangeTick;
                if (wgcLowSourceModeUpdate.transition == ce::capture_policy::HeldModeTransition::kEntered) {
                    LogInfo(
                        "[WGC CFR] Low-source mode entered: state=%s src=%u/%u/%u input=%u/%u empty=%upm buffered=%zu",
                        ce::capture_policy::WgcLowSourceStateToString(wgcLowSourceStateCurrent), wgcRecentDeliveredFps,
                        wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps, wgcRecentInputMin250Fps,
                        wgcRecentInputMin500Fps, wgcNoFreshTickPermille, bufferedWgcFrames.size());
                } else if (wgcLowSourceModeUpdate.transition == ce::capture_policy::HeldModeTransition::kExited) {
                    if (wgcLowSourceModeUpdate.immediate) {
                        ++captureSessionSummary.lowSourceImmediateExits;
                    } else {
                        LogInfo("[WGC CFR] Low-source mode exited: src=%u/%u/%u input=%u/%u empty=%upm buffered=%zu",
                                wgcRecentDeliveredFps, wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps,
                                wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                                bufferedWgcFrames.size());
                    }
                }

                if (wgcSourceHealthTelemetryReady) {
                    const bool shouldEnterWgcLiveRecoveryMode = ce::capture_policy::ShouldEnterWgcLiveRecoveryMode(
                        wgcAdaptiveTelemetry, outputShortfallTicks, wgcCapacityPressureForRecovery);
                    const bool shouldExitWgcLiveRecoveryMode = ce::capture_policy::ShouldExitWgcLiveRecoveryMode(
                        wgcAdaptiveTelemetry, outputShortfallTicks, wgcCapacityPressureForRecovery);
                    const auto wgcLiveRecoveryModeUpdate = ce::capture_policy::UpdateHeldMode(
                        wgcLiveRecoveryModeActive, wgcLiveRecoveryStateChangeTick, wgcPolicyNowTick,
                        shouldEnterWgcLiveRecoveryMode, shouldExitWgcLiveRecoveryMode, false,
                        ce::capture_policy::kWgcRecoveryEnterHoldMs, ce::capture_policy::kWgcRecoveryExitHoldMs);
                    wgcLiveRecoveryModeActive = wgcLiveRecoveryModeUpdate.active;
                    wgcLiveRecoveryStateChangeTick = wgcLiveRecoveryModeUpdate.stateChangeTick;
                    if (wgcLiveRecoveryModeUpdate.transition == ce::capture_policy::HeldModeTransition::kEntered) {
                        LogInfo(
                            "[WGC CFR] Live-recovery entered: state=%s srcStarved=%d schedLimited=%d encLimited=%d "
                            "shortfall=%u/%.1fms src=%u/%u/%u input=%u/%u empty=%upm buffered=%zu",
                            ce::capture_policy::WgcLiveRecoveryStateToString(wgcLiveRecoveryStateCurrent),
                            wgcSourceStarvedCurrent ? 1 : 0, wgcSchedulerLimitedCurrent ? 1 : 0,
                            wgcEncoderRecoveryLimitedCurrent ? 1 : 0, outputShortfallTicks,
                            ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs),
                            wgcRecentDeliveredFps, wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps,
                            wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                            bufferedWgcFrames.size());
                    } else if (wgcLiveRecoveryModeUpdate.transition ==
                               ce::capture_policy::HeldModeTransition::kExited) {
                        LogInfo(
                            "[WGC CFR] Live-recovery exited: shortfall=%u/%.1fms src=%u/%u/%u input=%u/%u empty=%upm "
                            "buffered=%zu",
                            outputShortfallTicks,
                            ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs),
                            wgcRecentDeliveredFps, wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps,
                            wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                            bufferedWgcFrames.size());
                    }
                } else {
                    wgcLiveRecoveryModeActive = false;
                    wgcLiveRecoveryStateChangeTick = 0;
                }

                const bool wgcCapacityPressureActiveCurrent = isWgcCapacityPressureActive();
                const uint32_t wgcBufferedFramesForPolicy =
                    static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull));
                const bool wgcTrueSourceStarvedForCapacityCurrent =
                    ce::capture_policy::IsWgcTrueSourceStarvedForRecovery(
                        outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                        wgcBufferedFramesForPolicy, wgcCapacityPressureActiveCurrent);
                const bool wgcSourceHealthyForEncoderLimitedCurrent =
                    ce::capture_policy::IsWgcSourceHealthyEnoughForEncoderLimitedSmoothness(
                        outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                        wgcBufferedFramesForPolicy);
                const bool wgcEncoderLimitedSmoothnessActiveCurrent = isWgcEncoderLimitedSmoothnessMode();
                if (wgcLowSourceModeActive && wgcCapacityPressureActiveCurrent &&
                    wgcSourceHealthyForEncoderLimitedCurrent && wgcEncoderLimitedSmoothnessActiveCurrent) {
                    ++wgcEncoderLimitedSuppressedByLowSourceThisWindow;
                    ++wgcEncoderLimitedSuppressedByLowSourceTotal;
                }
                if (wgcEncoderRecoveryLimitedCurrent && !wgcEncoderLimitedSmoothnessActiveCurrent &&
                    !wgcTrueSourceStarvedForCapacityCurrent) {
                    ++wgcCapacityPressureModeMismatchThisWindow;
                    ++wgcCapacityPressureModeMismatchTotal;
                    static uint64_t s_lastWgcModeMismatchLogTick = 0;
                    const uint64_t mismatchNowTick = GetTickCount64();
                    if (mismatchNowTick - s_lastWgcModeMismatchLogTick >= 1000) {
                        LogWarn(
                            "[WGC CFR] encoder-limited mode mismatch: recovery=encoder_limited smoothness=0 "
                            "lowSource=%d sourceHealthy=%d trueSourceStarved=%d shortfall=%u input=%u/%u "
                            "freshMiss=%upm buffered=%u overload=0x%X",
                            wgcLowSourceModeActive ? 1 : 0, wgcSourceHealthyForEncoderLimitedCurrent ? 1 : 0,
                            wgcTrueSourceStarvedForCapacityCurrent ? 1 : 0, outputShortfallTicks,
                            wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                            wgcBufferedFramesForPolicy, loadEncoderOverloadFlags());
                        s_lastWgcModeMismatchLogTick = mismatchNowTick;
                    }
                }

                const bool starvedEpisodeShouldBeActive =
                    wgcSourceHealthTelemetryReady &&
                    (ce::capture_policy::IsWgcDeepUnderfeed(outputFps, wgcRecentDeliveredMin250Fps,
                                                            wgcRecentInputMin250Fps, wgcNoFreshTickPermille) ||
                     (wgcLiveRecoveryModeActive && wgcSourceStarvedCurrent));
                if (starvedEpisodeShouldBeActive) {
                    if (!wgcStarvedEpisode.active) {
                        wgcStarvedEpisode.Reset();
                        wgcStarvedEpisode.active = true;
                        wgcStarvedEpisode.startTickMs = GetTickCount64();
                        LARGE_INTEGER episodeStartQpc = {};
                        QueryPerformanceCounter(&episodeStartQpc);
                        wgcStarvedEpisode.startQpc = episodeStartQpc.QuadPart;
                        wgcStarvedEpisode.startLiveTicks = liveTicksOutput;
                        wgcStarvedEpisode.startDuplicateTicks = captureSessionSummary.duplicateTicks;
                    }
                    wgcStarvedEpisode.minInputFps = std::min(wgcStarvedEpisode.minInputFps, wgcRecentInputMin250Fps);
                    wgcStarvedEpisode.minDeliveredFps =
                        std::min(wgcStarvedEpisode.minDeliveredFps, wgcRecentDeliveredMin250Fps);
                    wgcStarvedEpisode.peakFreshMissPermille =
                        std::max(wgcStarvedEpisode.peakFreshMissPermille, wgcNoFreshTickPermille);
                    wgcStarvedEpisode.minBufferedFrames = std::min<uint32_t>(
                        wgcStarvedEpisode.minBufferedFrames,
                        static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull)));
                } else if (wgcStarvedEpisode.active) {
                    const uint64_t nowTickMs = GetTickCount64();
                    const uint64_t durationMs = nowTickMs - wgcStarvedEpisode.startTickMs;
                    const uint64_t outputTicks = liveTicksOutput - wgcStarvedEpisode.startLiveTicks;
                    const uint64_t duplicateTicks =
                        captureSessionSummary.duplicateTicks - wgcStarvedEpisode.startDuplicateTicks;
                    finishWgcStarvedEpisode(durationMs, outputTicks, duplicateTicks);
                }

                if (g_WgcCap && recordingOutputLive && g_Recording && targetIntervalTicks > 0) {
                    const uint32_t currentTargetFps = g_WgcCap->GetTargetFps();
                    const uint32_t overcaptureTargetFps = ce::capture_policy::GetWgcCfrOvercaptureTargetFps(outputFps);
                    uint32_t desiredTargetFps = overcaptureTargetFps;
                    WgcAdaptiveThrottleMode desiredThrottleMode = overcaptureTargetFps > outputFps
                                                                      ? WgcAdaptiveThrottleMode::kHeadroom125
                                                                      : WgcAdaptiveThrottleMode::kOff;
                    const double duplicateRatio = (cadenceCounters.liveTickEmitCount > 0)
                                                      ? static_cast<double>(cadenceCounters.liveTickDuplicateCount) /
                                                            static_cast<double>(cadenceCounters.liveTickEmitCount)
                                                      : 0.0;
                    const uint32_t averageJitterUs = SaturatingToUint32(g_WgcCap->GetSourceJitterAvgUs());
                    const uint32_t poolDropCount = g_WgcCap->GetPoolDropCount();
                    const uint32_t queueDepth =
                        static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull));
                    wgcRecentDeliveredFps = g_WgcCap->GetDeliveredRatePerSec();
                    wgcRecentDeliveredMin250Fps = g_WgcCap->GetDeliveredMin250Fps();
                    wgcRecentDeliveredMin500Fps = g_WgcCap->GetDeliveredMin500Fps();
                    wgcRecentInputMin250Fps = g_WgcCap->GetInputMin250Fps();
                    wgcRecentInputMin500Fps = g_WgcCap->GetInputMin500Fps();

                    ce::capture_policy::WgcAdaptiveTelemetry adaptiveTelemetry{};
                    adaptiveTelemetry.outputFps = outputFps;
                    adaptiveTelemetry.recentDeliveredFps = wgcRecentDeliveredFps;
                    adaptiveTelemetry.recentDeliveredMin250Fps = wgcRecentDeliveredMin250Fps;
                    adaptiveTelemetry.recentDeliveredMin500Fps = wgcRecentDeliveredMin500Fps;
                    adaptiveTelemetry.recentInputMin250Fps = wgcRecentInputMin250Fps;
                    adaptiveTelemetry.recentInputMin500Fps = wgcRecentInputMin500Fps;
                    adaptiveTelemetry.averageJitterUs = averageJitterUs;
                    adaptiveTelemetry.emptyTickPermille = wgcNoFreshTickPermille;
                    adaptiveTelemetry.bufferedWgcFrames = queueDepth;
                    adaptiveTelemetry.duplicateRatio = duplicateRatio;
                    const bool maxRateRecovery = ce::capture_policy::ShouldUseWgcMaxRateForRecovery(
                        adaptiveTelemetry, wgcNoFreshTickPermille, wgcLowSourceModeActive, wgcLiveRecoveryModeActive);
                    if (maxRateRecovery) {
                        desiredTargetFps = 0;
                        desiredThrottleMode = WgcAdaptiveThrottleMode::kOff;
                        wgcOvercaptureStableSinceTick = 0;
                    } else {
                        if (wgcOvercaptureStableSinceTick == 0) {
                            wgcOvercaptureStableSinceTick = wgcPolicyNowTick;
                        }
                        const uint64_t stableMs = wgcPolicyNowTick >= wgcOvercaptureStableSinceTick
                                                      ? (wgcPolicyNowTick - wgcOvercaptureStableSinceTick)
                                                      : 0;
                        if (currentTargetFps == 0 && !ce::capture_policy::ShouldRestoreWgcOvercaptureCap(
                                                         adaptiveTelemetry, wgcNoFreshTickPermille, stableMs)) {
                            desiredTargetFps = 0;
                            desiredThrottleMode = WgcAdaptiveThrottleMode::kOff;
                        }
                    }

                    if (desiredTargetFps > 0 && wgcRecentInputMin250Fps > 0) {
                        desiredTargetFps = std::max<uint32_t>(outputFps, desiredTargetFps);
                    }

                    if (desiredTargetFps != wgcAdaptiveThrottlePendingTargetFps ||
                        desiredThrottleMode != wgcAdaptiveThrottlePendingMode) {
                        wgcAdaptiveThrottlePendingTargetFps = desiredTargetFps;
                        wgcAdaptiveThrottlePendingMode = desiredThrottleMode;
                        wgcAdaptiveThrottlePendingSinceTick = wgcPolicyNowTick;
                    }

                    if (desiredTargetFps == currentTargetFps) {
                        wgcAdaptiveThrottleMode = desiredThrottleMode;
                    } else {
                        constexpr uint64_t kWgcAdaptiveThrottleEnterHoldMs = 120;
                        constexpr uint64_t kWgcAdaptiveThrottleExitHoldMs = 120;
                        constexpr uint64_t kWgcAdaptiveThrottleRetuneHoldMs = 180;
                        const uint64_t requiredHoldMs =
                            currentTargetFps == 0 ? kWgcAdaptiveThrottleEnterHoldMs
                                                  : (desiredTargetFps == 0 ? kWgcAdaptiveThrottleExitHoldMs
                                                                           : kWgcAdaptiveThrottleRetuneHoldMs);
                        const bool holdElapsed =
                            wgcAdaptiveThrottlePendingSinceTick > 0 &&
                            (wgcPolicyNowTick - wgcAdaptiveThrottlePendingSinceTick) >= requiredHoldMs;
                        if (holdElapsed) {
                            g_WgcCap->SetTargetFps(desiredTargetFps);
                            g_WgcAdaptiveTargetFps.store(desiredTargetFps, std::memory_order_relaxed);
                            wgcAdaptiveThrottleMode = desiredThrottleMode;
                            ++wgcAdaptiveThrottleAdjustments;
                            LogInfo(
                                "[WGC CFR] Adaptive capture rate %s: target=%u output=%u src=%u/%u noFresh=%upm "
                                "lowSrc=%d recover=%d queue=%u jitter=%uus copy=%lldus poolDrop=%u",
                                desiredTargetFps == 0 ? "max-rate recovery" : "overcapture cap", desiredTargetFps,
                                outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                                wgcLowSourceModeActive ? 1 : 0, wgcLiveRecoveryModeActive ? 1 : 0, queueDepth,
                                averageJitterUs, g_WgcCap->GetLastCopyTimeUs(), poolDropCount);
                        }
                    }
                }

                const bool scheduledWgcTelemetryTick =
                    !config.video.useVFR && g_EncoderRunning && g_Recording && recordingOutputLive;
                if (scheduledWgcTelemetryTick) {
                    LARGE_INTEGER selectionNowQpc;
                    QueryPerformanceCounter(&selectionNowQpc);
                    wgcTelemetryTickArmed = true;
                    wgcBufferedAtTickStart = static_cast<uint32_t>(bufferedWgcFrames.size());
                    wgcReserveAvailableAtTickStart = false;
                    inspectBufferedWgcCoverageForTarget(
                        clampWgcSelectionTargetQpc(computeWgcSelectionTargetQpc(false), selectionNowQpc.QuadPart),
                        &wgcFreshAvailableAtTickStart, &wgcReserveAvailableAtTickStart);
                    wgcSelectionDelayAppliedThisTick =
                        !wgcLiveRecoveryModeActive && ce::capture_policy::ShouldApplyWgcSelectionDelay(
                                                          recordingOutputLive, outputShortfallTicks,
                                                          g_IsEncoderBottlenecked.load(std::memory_order_relaxed),
                                                          wgcReserveAvailableAtTickStart, avContentDelayActive);
                    if (wgcSelectionDelayAppliedThisTick) {
                        ++wgcSelectionDelayTickCount;
                    }
                }

                if (!g_EncoderRunning && !bufferedWgcFrames.empty()) {
                    frame = std::move(bufferedWgcFrames.front());
                    bufferedWgcFrames.pop_front();
                    popped = true;
                } else if (!bufferedWgcFrames.empty()) {
                    LARGE_INTEGER selectionNowQpc;
                    QueryPerformanceCounter(&selectionNowQpc);
                    const int64_t liveSelectionTargetQpc =
                        (encoderGridStartQpc > 0 && targetIntervalTicks > 0)
                            ? clampWgcSelectionTargetQpc(computeLiveWgcSelectionTargetQpc(), selectionNowQpc.QuadPart)
                            : 0;
                    const int64_t delayedSelectionTargetQpc =
                        (encoderGridStartQpc > 0 && targetIntervalTicks > 0)
                            ? clampWgcSelectionTargetQpc(computeDelayedWgcSelectionTargetQpc(),
                                                         selectionNowQpc.QuadPart)
                            : 0;
                    const int64_t effectiveSelectionTargetQpc =
                        wgcSelectionDelayAppliedThisTick ? delayedSelectionTargetQpc : liveSelectionTargetQpc;
                    if (tryPopBufferedWgcFrameForTarget(effectiveSelectionTargetQpc, liveSelectionTargetQpc,
                                                        selectionNowQpc.QuadPart, wgcSelectionDelayAppliedThisTick,
                                                        &frame)) {
                        popped = true;
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
                    if (!ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, temp.isInjectMode)) {
                        discardActivePathMismatchFrame(temp, "WGC VFR queue", true);
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
                    if (!ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, temp.isInjectMode)) {
                        discardActivePathMismatchFrame(temp, "inject CFR queue", true);
                        continue;
                    }
                    if (temp.isInjectMode && temp.timestamp > 0) {
                        injectInputPredictor.Update(temp.timestamp, qpcFreq.QuadPart);
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
                // A/V content delay (inject): retain injectContentDelayFrames extra frames so the
                // oldest popped frame is ~L old, delaying video content to match late loopback
                // audio. The cap and live-age bound are widened to admit the deeper buffer.
                size_t minBufferedInjectFrames =
                    ce::capture_policy::GetMinBufferedInjectFrames(injectReserveFrames, recordingOutputLive) +
                    injectContentDelayFrames;
                const size_t maxBufferedInjectFrames =
                    std::max(ce::capture_policy::GetMaxBufferedInjectFrames(injectReserveFrames, recordingOutputLive,
                                                                            recordingLiveTick, GetTickCount64()),
                             minBufferedInjectFrames + 2);
                maxBufferedInjectDepthSinceLog = std::max(maxBufferedInjectDepthSinceLog, bufferedInjectFrames.size());
                uint32_t trimmedInjectFrames = 0;
                uint32_t ageTrimmedInjectFrames = 0;
                while (bufferedInjectFrames.size() > maxBufferedInjectFrames) {
                    QueuedFrame staleFrame = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    DiscardQueuedFrame(staleFrame);
                    ++trimmedInjectFrames;
                }
                LARGE_INTEGER trimNowQpc;
                QueryPerformanceCounter(&trimNowQpc);
                const bool injectEncoderBottlenecked = g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
                int64_t maxInjectLiveAgeQpc = ce::capture_policy::GetInjectLiveMaxFrameAgeQpc(
                    recordingOutputLive, injectEncoderBottlenecked, encoderTooSlowForTargetCurrent,
                    targetIntervalTicks);
                if (avContentDelayActive) {
                    // Don't age-trim the intentionally retained content-delay frames.
                    maxInjectLiveAgeQpc = std::max(maxInjectLiveAgeQpc,
                                                   avContentDelayQpc + 2 * std::max<int64_t>(targetIntervalTicks, 1));
                }
                while (!bufferedInjectFrames.empty() &&
                       ce::capture_policy::ShouldTrimStaleInjectLiveFrame(
                           bufferedInjectFrames.front().timestamp, trimNowQpc.QuadPart, maxInjectLiveAgeQpc,
                           bufferedInjectFrames.size(), minBufferedInjectFrames)) {
                    QueuedFrame staleFrame = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    DiscardQueuedFrame(staleFrame);
                    ++trimmedInjectFrames;
                    ++ageTrimmedInjectFrames;
                }
                if (ageTrimmedInjectFrames > 0) {
                    injectLiveStaleTrimThisWindow += ageTrimmedInjectFrames;
                    injectLiveStaleTrimTotal += ageTrimmedInjectFrames;
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
                        "reserve=%zu ageTrim=%u maxAgeTicks=%u)",
                        pendingInjectTrimmedLogCount, maxBufferedInjectDepthSinceLog, maxBufferedInjectFrames,
                        injectReserveFrames, injectLiveStaleTrimThisWindow,
                        maxInjectLiveAgeQpc > 0 && targetIntervalTicks > 0
                            ? static_cast<unsigned>(maxInjectLiveAgeQpc / targetIntervalTicks)
                            : 0u);
                    pendingInjectTrimmedLogCount = 0;
                    maxBufferedInjectDepthSinceLog = bufferedInjectFrames.size();
                    lastInjectTrimLog = now;
                }

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
                    auto isFreshInjectCandidate = [&](const QueuedFrame& candidate) {
                        return ce::capture_policy::IsInjectFrameFreshAfterLastEmission(candidate.timestamp,
                                                                                       lastEmittedInjectSourceQpc);
                    };
                    auto discardStaleInjectFront = [&]() {
                        size_t droppedStale = 0;
                        while (bufferedInjectFrames.size() > minBufferedInjectFrames &&
                               !isFreshInjectCandidate(bufferedInjectFrames.front())) {
                            QueuedFrame stale = std::move(bufferedInjectFrames.front());
                            bufferedInjectFrames.pop_front();
                            DiscardQueuedFrame(stale);
                            g_InjectCadenceDroppedFrames.fetch_add(1, std::memory_order_relaxed);
                            if (g_pSharedMem) {
                                g_pSharedMem->runtimeState.injectCadenceDrops.fetch_add(1, std::memory_order_relaxed);
                            }
                            ++droppedStale;
                        }
                        if (droppedStale > 0) {
                            LogWarn(
                                "[EncoderThread] Dropped %zu stale inject frame(s) behind emitted source timestamp "
                                "%lld",
                                droppedStale, static_cast<long long>(lastEmittedInjectSourceQpc));
                        }
                        return droppedStale;
                    };
                    bool canPopInjectFrame = true;
                    if (availableCount > 1 && encoderGridStartQpc > 0) {
                        auto isAllowedCandidate = [&](const QueuedFrame& candidate) {
                            return isFreshInjectCandidate(candidate) &&
                                   !MatchesInjectFrameLineage(candidate, lastDeferredLineage);
                        };
                        size_t bestIdx =
                            SelectFrameClosestToGridIf(bufferedInjectFrames, availableCount, encoderGridStartQpc,
                                                       selectionGridTick, targetIntervalTicks, isAllowedCandidate);
                        bool usedDeferredFallback = false;
                        if (bestIdx >= availableCount) {
                            bestIdx = SelectFrameClosestToGridIf(bufferedInjectFrames, availableCount,
                                                                 encoderGridStartQpc, selectionGridTick,
                                                                 targetIntervalTicks, isFreshInjectCandidate);
                            usedDeferredFallback = lastDeferredLineage.IsValid();
                        }
                        if (bestIdx >= availableCount) {
                            canPopInjectFrame = false;
                            discardStaleInjectFront();
                        } else if (bestIdx > 0) {
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
                    if (canPopInjectFrame) {
                        discardStaleInjectFront();
                        if (!bufferedInjectFrames.empty() && isFreshInjectCandidate(bufferedInjectFrames.front())) {
                            frame = std::move(bufferedInjectFrames.front());
                            bufferedInjectFrames.pop_front();
                            popped = true;
                            frameCreditAccumulator -= 1.0;
                            lastDeferredLineage = {};
                        }
                    }
                } else if (bufferedInjectFrames.size() > injectReserveFrames + 6) {
                    // Buffer pressure: pop to prevent unnecessary trimming
                    while (bufferedInjectFrames.size() > minBufferedInjectFrames &&
                           !ce::capture_policy::IsInjectFrameFreshAfterLastEmission(
                               bufferedInjectFrames.front().timestamp, lastEmittedInjectSourceQpc)) {
                        QueuedFrame stale = std::move(bufferedInjectFrames.front());
                        bufferedInjectFrames.pop_front();
                        DiscardQueuedFrame(stale);
                        g_InjectCadenceDroppedFrames.fetch_add(1, std::memory_order_relaxed);
                        if (g_pSharedMem) {
                            g_pSharedMem->runtimeState.injectCadenceDrops.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    if (!bufferedInjectFrames.empty() &&
                        ce::capture_policy::IsInjectFrameFreshAfterLastEmission(bufferedInjectFrames.front().timestamp,
                                                                                lastEmittedInjectSourceQpc)) {
                        frame = std::move(bufferedInjectFrames.front());
                        bufferedInjectFrames.pop_front();
                        popped = true;
                        frameCreditAccumulator = std::fmod(frameCreditAccumulator, 1.0);
                        lastDeferredLineage = {};
                    }
                }
            } else {
                // VFR: keep the existing newest-frame sampling for the lowest latency.
                QueuedFrame temp;
                while (g_FrameQueue.Pop(temp, 0)) {
                    if (g_RejectInjectFrames.load(std::memory_order_acquire) && temp.isInjectMode) {
                        DiscardQueuedFrame(temp);
                        continue;
                    }
                    if (!ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, temp.isInjectMode)) {
                        discardActivePathMismatchFrame(temp, "inject VFR queue", true);
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

        if (popped && !ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, frame.isInjectMode)) {
            discardActivePathMismatchFrame(frame, "selected frame", true);
            popped = false;
        }

        if (g_HasLastFrame &&
            !ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, g_LastFrame.isInjectMode)) {
            discardActivePathMismatchFrame(g_LastFrame, "cached last frame", false);
            g_HasLastFrame = false;
        }

        if (popped && frame.isInjectMode && g_RejectInjectFrames.load(std::memory_order_acquire)) {
            DiscardQueuedFrame(frame);
            popped = false;
        }

        if (g_HasLastFrame && g_LastFrame.isInjectMode && g_RejectInjectFrames.load(std::memory_order_acquire)) {
            g_LastFrame = QueuedFrame{};
            g_HasLastFrame = false;
        }

        const bool hasRepeatLastFramePath =
            !config.video.useVFR &&
            ((useScreenGrab && MediaEngine_RepeatLastFrameWithTimeline) || MediaEngine_RepeatLastFrame);
        auto repeatLastFrameForScheduledQpc = [&](int64_t scheduledQpc) {
            if (useScreenGrab && !config.video.useVFR && MediaEngine_RepeatLastFrameWithTimeline) {
                return MediaEngine_RepeatLastFrameWithTimeline(scheduledQpc,
                                                               computeLiveTimelineElapsedUs(scheduledQpc));
            }
            return MediaEngine_RepeatLastFrame && MediaEngine_RepeatLastFrame(scheduledQpc);
        };
        const bool warmupCaptureModeChanged = ce::capture_policy::ResetWarmupOnCaptureModeChange(
            recordingOutputLive, useScreenGrab, GetTickCount64(), warmupState);
        if (warmupCaptureModeChanged || !useScreenGrab) {
            ResetWarmupWgcFreshness();
            wgcLowSourceModeActive = false;
            wgcLowSourceStateChangeTick = 0;
            wgcLiveRecoveryModeActive = false;
            wgcLiveRecoveryStateChangeTick = 0;
            wgcSourceStarvedCurrent = false;
            wgcSchedulerLimitedCurrent = false;
            wgcEncoderRecoveryLimitedCurrent = false;
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
                const bool wgcCfrStartupSync = ce::capture_policy::ShouldUseWgcCfrStartupSyncBarrier(
                    useScreenGrab, config.video.useVFR, targetIntervalTicks);
                if (wgcCfrStartupSync) {
                    if (!wgcStartupPreLiveDelayComplete) {
                        if (popped) {
                            TrackWarmupWgcFreshFrame(frame);
                            ++hiddenStartupFrames;
                            ++wgcStartupPreLiveDelayDroppedFrames;
                            warmupState.hiddenStartupFrames = hiddenStartupFrames;
                            DiscardQueuedFrame(frame);
                        }

                        const int64_t delayTicks =
                            ce::capture_policy::GetWgcCfrStartupPreLiveDelayTicks(targetIntervalTicks);
                        if (hTimer && delayTicks > 0) {
                            const int64_t delay100ns = (delayTicks * 10000000) / qpcFreq.QuadPart;
                            LARGE_INTEGER dueTime;
                            dueTime.QuadPart = -delay100ns;
                            if (SetWaitableTimer(hTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                                WaitForSingleObject(hTimer, INFINITE);
                            }
                        }

                        QueuedFrame qf;
                        size_t queueFlushed = 0;
                        while (g_FrameQueue.Pop(qf, 0)) {
                            if (qf.isInjectMode)
                                DiscardQueuedFrame(qf);
                            else if (qf.texture)
                                ReleaseQueuedFrameTexture(qf);
                            queueFlushed++;
                        }
                        size_t bufferedFlushed = 0;
                        if (!bufferedWgcFrames.empty()) {
                            bufferedFlushed = bufferedWgcFrames.size();
                            ClearBufferedWgcFrames();
                        }

                        LARGE_INTEGER barrierNow;
                        QueryPerformanceCounter(&barrierNow);
                        wgcStartupBarrierQpc =
                            ce::capture_policy::GetWgcStartupBarrierQpc(barrierNow.QuadPart, targetIntervalTicks);
                        wgcStartupBarrierDroppedFrames = 0;
                        wgcStartupPreLiveDelayComplete = true;
                        const uint64_t warmupElapsedWithDelayMs64 = GetTickCount64() - startupWarmupStartTick;
                        LogInfo(
                            "[EncoderThread] WGC startup pre-live delay complete: anchorQpc=%lld now=%lld "
                            "oneFrame=%lld delayTicks=%lld hiddenFrames=%u discarded=%u queueFlushed=%zu "
                            "bufferedFlushed=%zu warmupMs=%llu",
                            static_cast<long long>(wgcStartupBarrierQpc), static_cast<long long>(barrierNow.QuadPart),
                            static_cast<long long>(targetIntervalTicks), static_cast<long long>(delayTicks),
                            hiddenStartupFrames, wgcStartupPreLiveDelayDroppedFrames, queueFlushed, bufferedFlushed,
                            static_cast<unsigned long long>(warmupElapsedWithDelayMs64));
                        continue;
                    }

                    if (wgcStartupBarrierQpc <= 0) {
                        LARGE_INTEGER barrierNow;
                        QueryPerformanceCounter(&barrierNow);
                        wgcStartupBarrierQpc =
                            ce::capture_policy::GetWgcStartupBarrierQpc(barrierNow.QuadPart, targetIntervalTicks);
                        LogInfo(
                            "[EncoderThread] WGC startup sync post-delay barrier armed: anchorQpc=%lld now=%lld "
                            "oneFrame=%lld",
                            static_cast<long long>(wgcStartupBarrierQpc), static_cast<long long>(barrierNow.QuadPart),
                            static_cast<long long>(targetIntervalTicks));
                    }

                    if (!popped || frame.isInjectMode ||
                        !ce::capture_policy::IsWgcFramePastStartupBarrier(frame.timestamp, wgcStartupBarrierQpc)) {
                        if (popped) {
                            TrackWarmupWgcFreshFrame(frame);
                            ++hiddenStartupFrames;
                            ++wgcStartupBarrierDroppedFrames;
                            warmupState.hiddenStartupFrames = hiddenStartupFrames;
                            DiscardQueuedFrame(frame);
                        }
                        continue;
                    }

                    size_t startupBufferedExamined = 0;
                    size_t startupQueueExamined = 0;
                    size_t startupFreshened = 0;
                    size_t startupDiscardedOlder = 0;
                    size_t startupDiscardedBeforeBarrier = 0;
                    size_t startupDiscardedPathMismatch = 0;
                    auto considerStartupWgcCandidate = [&](QueuedFrame candidate, bool fromQueue) {
                        if (fromQueue) {
                            ++startupQueueExamined;
                        } else {
                            ++startupBufferedExamined;
                        }

                        if (candidate.isInjectMode) {
                            ++startupDiscardedPathMismatch;
                            DiscardQueuedFrame(candidate);
                            return;
                        }

                        if (!ce::capture_policy::IsWgcFramePastStartupBarrier(candidate.timestamp,
                                                                              wgcStartupBarrierQpc)) {
                            ++startupDiscardedBeforeBarrier;
                            ++wgcStartupBarrierDroppedFrames;
                            ReleaseQueuedFrameTexture(candidate);
                            return;
                        }

                        const int64_t candidateSelectionQpc = GetFrameSelectionTimestamp(candidate);
                        const int64_t selectedSelectionQpc = GetFrameSelectionTimestamp(frame);
                        const bool newerCandidate =
                            candidate.timestamp > frame.timestamp ||
                            (candidate.timestamp == frame.timestamp && candidateSelectionQpc > selectedSelectionQpc);
                        if (newerCandidate) {
                            ReleaseQueuedFrameTexture(frame);
                            frame = std::move(candidate);
                            ++startupFreshened;
                        } else {
                            ReleaseQueuedFrameTexture(candidate);
                            ++startupDiscardedOlder;
                        }
                    };

                    while (!bufferedWgcFrames.empty()) {
                        QueuedFrame candidate = std::move(bufferedWgcFrames.front());
                        bufferedWgcFrames.pop_front();
                        considerStartupWgcCandidate(std::move(candidate), false);
                    }

                    QueuedFrame queuedStartupCandidate;
                    while (g_FrameQueue.Pop(queuedStartupCandidate, 0)) {
                        considerStartupWgcCandidate(std::move(queuedStartupCandidate), true);
                        queuedStartupCandidate = QueuedFrame{};
                    }

                    LARGE_INTEGER anchorNow;
                    QueryPerformanceCounter(&anchorNow);
                    const int64_t startDeltaUs =
                        ((frame.timestamp - wgcStartupBarrierQpc) * 1000000) / qpcFreq.QuadPart;
                    const int64_t frameAgeUs =
                        anchorNow.QuadPart >= frame.timestamp
                            ? ((anchorNow.QuadPart - frame.timestamp) * 1000000) / qpcFreq.QuadPart
                            : 0;
                    LogInfo(
                        "[EncoderThread] WGC startup sync post-delay barrier satisfied: anchorQpc=%lld "
                        "firstFrameQpc=%lld delta=%lldus frameAge=%lldus droppedPostDelay=%u "
                        "discardedBeforeDelay=%u freshened=%zu bufferedExamined=%zu queueExamined=%zu "
                        "discardedOlder=%zu discardedBeforeBarrier=%zu pathMismatch=%zu",
                        static_cast<long long>(wgcStartupBarrierQpc), static_cast<long long>(frame.timestamp),
                        static_cast<long long>(startDeltaUs), static_cast<long long>(frameAgeUs),
                        wgcStartupBarrierDroppedFrames, wgcStartupPreLiveDelayDroppedFrames, startupFreshened,
                        startupBufferedExamined, startupQueueExamined, startupDiscardedOlder,
                        startupDiscardedBeforeBarrier, startupDiscardedPathMismatch);
                }

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
                wgcVisualDebtMaxExcessTicks = 0;
                wgcStopDrainHeldFrameLogged = false;
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
                wgcRepeatPolicyHoldTotal = 0;
                wgcCoverageRepeatHoldCount = 0;
                wgcCoverageDelayTicksCurrent = 0;
                wgcRepeatTimerLateCount = 0;
                wgcRepeatCatchupCount = 0;
                wgcSelectFreshCount = 0;
                wgcSelectDuplicateSourceCount = 0;
                wgcDropObsoleteCount = 0;
                wgcEncoderLimitedSourceDropThisWindow = 0;
                wgcEncoderLimitedSourceDropTotal = 0;
                wgcEncoderLimitedSourceDropMaxTicks = 0;
                wgcEncoderLimitedCadenceEventCount = 0;
                wgcCoverageRepeatAccumulator = 0.0;
                lastEmittedWgcSourceQpc = 0;
                lastEmittedInjectSourceQpc = 0;
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
                    const bool wgcCfrDelayAlreadyDone = ce::capture_policy::ShouldUseWgcCfrStartupSyncBarrier(
                                                            useScreenGrab, config.video.useVFR, targetIntervalTicks) &&
                                                        wgcStartupPreLiveDelayComplete;
                    // WGC CFR performs this delay before the final startup barrier
                    // so the shared A/V anchor is selected from a fresh post-delay frame.
                    int64_t sleepTicks =
                        wgcCfrDelayAlreadyDone
                            ? 0
                            : (useScreenGrab
                                   ? ce::capture_policy::GetWgcCfrStartupPreLiveDelayTicks(targetIntervalTicks)
                                   : (targetIntervalTicks * 4));
                    if (sleepTicks > 0) {
                        int64_t sleep100ns = (sleepTicks * 10000000) / qpcFreq.QuadPart;
                        LARGE_INTEGER dueTime;
                        dueTime.QuadPart = -sleep100ns;
                        if (SetWaitableTimer(hTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                            WaitForSingleObject(hTimer, INFINITE);
                        }
                    }
                }
                QueryPerformanceCounter(&nextSampleTime);
                liveStartQpc.QuadPart =
                    0;  // Set after first frame's encoder initialization delay to avoid initial judder
                encoderGridStartQpc = nextSampleTime.QuadPart;
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
            const size_t bufferedInjectReadyFrames =
                bufferedInjectFrames.size() + ((!useScreenGrab && popped && frame.isInjectMode) ? 1u : 0u);
            const bool liveReady = useScreenGrab || bufferedInjectReadyFrames >= pendingLiveInjectReadyFrames;
            if (!liveReady) {
                SetCapturePipelinePhase(CapturePipelinePhase::kWarmup);
                if (popped) {
                    if (useScreenGrab) {
                        TrackWarmupWgcFreshFrame(frame);
                        ++hiddenStartupFrames;
                        warmupState.hiddenStartupFrames = hiddenStartupFrames;
                        DiscardQueuedFrame(frame);
                    } else if (frame.isInjectMode) {
                        bufferedInjectFrames.push_front(std::move(frame));
                    } else {
                        ++hiddenStartupFrames;
                        warmupState.hiddenStartupFrames = hiddenStartupFrames;
                        DiscardQueuedFrame(frame);
                    }
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

        const bool refreshedDrainOutstandingLiveTicks = !g_EncoderRunning &&
                                                        g_DrainOutstandingCfrTicks.load(std::memory_order_acquire) &&
                                                        recordingOutputLive && !config.video.useVFR;
        if (!popped && !drainingOutstandingLiveTicks && refreshedDrainOutstandingLiveTicks) {
            LogInfo("[EncoderThread] CFR stop drain picked up mid-cycle");
            continue;
        }
        drainingOutstandingLiveTicks = refreshedDrainOutstandingLiveTicks;

        if (!g_EncoderRunning && !popped && !drainingOutstandingLiveTicks) {
            break;
        }

        const bool consumesCfrTick =
            !config.video.useVFR && ((g_EncoderRunning && g_Recording) || drainingOutstandingLiveTicks);
        const bool isDrainPhase = !g_Recording.load(std::memory_order_acquire);
        const bool isLivePhase =
            recordingOutputLive && (g_Recording.load(std::memory_order_acquire) || drainingOutstandingLiveTicks);
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

            uint32_t remainingFreshCatchupBudget =
                useScreenGrab && !config.video.useVFR
                    ? ce::capture_policy::GetWgcFreshCatchupBudgetThisLoop(catchupTicksThisLoop)
                    : 0u;

            for (uint32_t extraTick = 1; extraTick < catchupTicksThisLoop; ++extraTick) {
                if (extraTick > 1) {
                    break;
                }

                if (useScreenGrab && config.video.useVFR &&
                    !ce::capture_policy::ShouldAllowWgcExtraCatchupTicks(
                        encoderTooSlowForTargetCurrent, bufferedWgcFrames.size(), frameCreditAccumulator,
                        outputShortfallTicks)) {
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
                const double shortfallDurationMs =
                    ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs);
                const double catchupBudgetMs =
                    allowForceCatchupBudget
                        ? (frameIntervalMs *
                           ce::capture_policy::GetWgcForceCatchupBudgetFrameMultiplier(shortfallDurationMs))
                    : allowWgcCatchupBudget ? (frameIntervalMs * 2.0)
                                            : frameIntervalMs;

                // For CFR recording, video smoothness is paramount. We have a 32-frame deep queue
                // (~266ms at 120fps) to absorb temporary encoder spikes. We only force duplicate frames
                // if we are meaningfully behind (e.g. > 50ms delay) to prevent runaway latency.
                // Otherwise, we process the fresh frame to preserve the correct visual pacing.
                const double cfrSmoothnessToleranceMs = (!config.video.useVFR && useScreenGrab) ? 50.0 : 0.0;
                bool allowFreshCatchup = remainingFreshCatchupBudget > 0u;

                if (elapsedFromTickStartMs > catchupBudgetMs + cfrSmoothnessToleranceMs) {
                    static uint64_t s_lastBudgetLog = 0;
                    uint64_t nowTick = GetTickCount64();
                    if (nowTick - s_lastBudgetLog >= 1000) {
                        LogInfo(
                            "[EncoderThread] Catchup budget exceeded at extraTick=%u (elapsed=%.2fms > budget=%.2fms + "
                            "tol=%.2fms). %s",
                            extraTick, elapsedFromTickStartMs, catchupBudgetMs, cfrSmoothnessToleranceMs,
                            useScreenGrab ? "Switching to duplicate frames to preserve CFR timeline without stalling."
                                          : "Inject fresh catch-up remains gated by encoder health and queued credit.");
                        s_lastBudgetLog = nowTick;
                    }
                    if (config.video.useVFR || outputShortfallTicks == 0) {
                        break;
                    } else if (useScreenGrab) {
                        // CFR must not break to avoid timeline holes, but we must stop using expensive fresh frames!
                        allowFreshCatchup = false;
                    }
                }

                const int64_t repeatScheduledQpc =
                    scheduledSampleQpc + static_cast<int64_t>(extraTick) * targetIntervalTicks;

                if (!useScreenGrab && !config.video.useVFR && MediaEngine_ProcessFrame) {
                    const size_t catchupInjectReserveFrames = ce::capture_policy::GetInjectReserveFrames(
                        config.video.useVFR, smoothedInjectFenceMs, frameIntervalMs);
                    const size_t catchupMinBufferedInjectFrames =
                        ce::capture_policy::GetMinBufferedInjectFrames(catchupInjectReserveFrames, recordingOutputLive);
                    const bool encoderBottleneckedNow = g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
                    const bool allowFreshInjectCatchup = ce::capture_policy::ShouldUseFreshInjectCatchup(
                        config.video.useVFR, encoderBottleneckedNow, encoderTooSlowForTargetCurrent,
                        bufferedInjectFrames.size(), catchupMinBufferedInjectFrames, frameCreditAccumulator,
                        outputShortfallTicks);
                    if (allowFreshInjectCatchup) {
                        size_t availableCount = bufferedInjectFrames.size() - catchupMinBufferedInjectFrames;
                        const int64_t catchupGridTick = encoderGridTickCount + 1;
                        size_t bestIdx = 0;
                        auto isFreshInjectCandidate = [&](const QueuedFrame& candidate) {
                            return ce::capture_policy::IsInjectFrameFreshAfterLastEmission(candidate.timestamp,
                                                                                           lastEmittedInjectSourceQpc);
                        };
                        if (availableCount > 1 && encoderGridStartQpc > 0) {
                            auto isAllowedCandidate = [&](const QueuedFrame& candidate) {
                                return isFreshInjectCandidate(candidate) &&
                                       !MatchesInjectFrameLineage(candidate, lastDeferredLineage);
                            };
                            bestIdx =
                                SelectFrameClosestToGridIf(bufferedInjectFrames, availableCount, encoderGridStartQpc,
                                                           catchupGridTick, targetIntervalTicks, isAllowedCandidate);
                            if (bestIdx >= availableCount) {
                                bestIdx = SelectFrameClosestToGridIf(bufferedInjectFrames, availableCount,
                                                                     encoderGridStartQpc, catchupGridTick,
                                                                     targetIntervalTicks, isFreshInjectCandidate);
                            }
                        }

                        if (bestIdx < availableCount && isFreshInjectCandidate(bufferedInjectFrames[bestIdx])) {
                            for (size_t i = 0; i < bestIdx; ++i) {
                                QueuedFrame stale = std::move(bufferedInjectFrames.front());
                                bufferedInjectFrames.pop_front();
                                DiscardQueuedFrame(stale);
                                g_InjectCadenceDroppedFrames.fetch_add(1, std::memory_order_relaxed);
                                if (g_pSharedMem) {
                                    g_pSharedMem->runtimeState.injectCadenceDrops.fetch_add(1,
                                                                                            std::memory_order_relaxed);
                                }
                            }

                            QueuedFrame catchupFrame = std::move(bufferedInjectFrames.front());
                            bufferedInjectFrames.pop_front();
                            const InjectFrameLineage catchupLineage = MakeInjectFrameLineage(catchupFrame);

                            LARGE_INTEGER catchupStartEnc, catchupEndEnc;
                            QueryPerformanceCounter(&catchupStartEnc);
                            uint64_t frameAgeUs = 0;
                            if (catchupFrame.timestamp > 0 && catchupStartEnc.QuadPart > catchupFrame.timestamp) {
                                frameAgeUs = static_cast<uint64_t>((catchupStartEnc.QuadPart - catchupFrame.timestamp) *
                                                                   1000000 / qpcFreq.QuadPart);
                            }
                            cadenceCounters.frameAgeAccumUs += frameAgeUs;
                            cadenceCounters.frameAgeSamples++;
                            cadenceCounters.frameAgeMaxUs =
                                std::max(cadenceCounters.frameAgeMaxUs, SaturatingToUint32(frameAgeUs));
                            if (repeatScheduledQpc > 0) {
                                const int64_t signedOutputScheduleErrorUs =
                                    ((catchupStartEnc.QuadPart - repeatScheduledQpc) * 1000000) / qpcFreq.QuadPart;
                                const uint64_t absoluteOutputScheduleErrorUs = static_cast<uint64_t>(
                                    signedOutputScheduleErrorUs >= 0 ? signedOutputScheduleErrorUs
                                                                     : -signedOutputScheduleErrorUs);
                                cadenceCounters.outputScheduleErrorAccumUs += absoluteOutputScheduleErrorUs;
                                cadenceCounters.outputScheduleErrorSignedAccumUs += signedOutputScheduleErrorUs;
                                cadenceCounters.outputScheduleErrorSamples++;
                                cadenceCounters.outputScheduleErrorMaxUs =
                                    std::max(cadenceCounters.outputScheduleErrorMaxUs,
                                             SaturatingToUint32(absoluteOutputScheduleErrorUs));
                                if (signedOutputScheduleErrorUs < 0) {
                                    cadenceCounters.outputScheduleEarlyMaxUs = std::max(
                                        cadenceCounters.outputScheduleEarlyMaxUs,
                                        SaturatingToUint32(static_cast<uint64_t>(-signedOutputScheduleErrorUs)));
                                } else {
                                    cadenceCounters.outputScheduleLateMaxUs = std::max(
                                        cadenceCounters.outputScheduleLateMaxUs,
                                        SaturatingToUint32(static_cast<uint64_t>(signedOutputScheduleErrorUs)));
                                }
                            }

                            const bool catchupEncodeSucceeded = MediaEngine_ProcessFrame(
                                (uint64_t)catchupFrame.sharedHandle, (uint64_t)catchupFrame.fenceHandle,
                                catchupFrame.fenceValue, catchupFrame.timestamp, catchupFrame.luidLow,
                                catchupFrame.luidHigh, catchupFrame.sourcePid, catchupFrame.width, catchupFrame.height,
                                catchupFrame.format, catchupFrame.isHDR, catchupFrame.isShmem, catchupFrame.shmemSlot);
                            const bool catchupEncodeDeferred =
                                MediaEngine_WasLastFrameDeferred && MediaEngine_WasLastFrameDeferred();
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
                            UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs,
                                                        ce::capture_policy::IsEncoderStartupWindow(
                                                            recordingOutputLive, recordingLiveTick, GetTickCount64()));

                            if (catchupEncodeSucceeded && !catchupEncodeDeferred) {
                                if (g_HasLastFrame && !g_LastFrame.isInjectMode && g_LastFrame.texture) {
                                    g_LastFrame.texture->Release();
                                    g_LastFrame.texture = nullptr;
                                }

                                const double currentFenceMs = (double)MediaEngine_GetLastFrameFenceWaitUs() / 1000.0;
                                if (smoothedInjectFenceMs == 0.0) {
                                    smoothedInjectFenceMs = currentFenceMs;
                                } else {
                                    smoothedInjectFenceMs = smoothedInjectFenceMs * 0.90 + currentFenceMs * 0.10;
                                }

                                if (catchupFrame.frameIndex != 0) {
                                    if (lastEncodedInjectFrameIndex != 0 &&
                                        catchupFrame.frameIndex < lastEncodedInjectFrameIndex) {
                                        LogWarn(
                                            "[EncoderThread] Inject lineage regression during catch-up: encoded "
                                            "frame=%u after frame=%u (ring=%u tex=%d ts=%lld)",
                                            catchupFrame.frameIndex, lastEncodedInjectFrameIndex,
                                            catchupFrame.ringIndex, catchupFrame.textureIndex,
                                            static_cast<long long>(catchupFrame.timestamp));
                                        if (g_pSharedMem) {
                                            g_pSharedMem->runtimeState.frameIndexRegressions.fetch_add(
                                                1, std::memory_order_relaxed);
                                        }
                                    }
                                    lastEncodedInjectFrameIndex = catchupFrame.frameIndex;
                                }
                                if (IsInjectTextureIndexValid(catchupFrame.textureIndex)) {
                                    uint32_t& lastTextureFrame =
                                        lastEncodedFrameByTextureIndex[static_cast<size_t>(catchupFrame.textureIndex)];
                                    if (lastTextureFrame != 0 && catchupFrame.frameIndex != 0 &&
                                        catchupFrame.frameIndex <= lastTextureFrame) {
                                        LogWarn(
                                            "[EncoderThread] Texture slot reuse anomaly during catch-up: tex=%d "
                                            "frame=%u previous=%u ring=%u fence=%llu ts=%lld",
                                            catchupFrame.textureIndex, catchupFrame.frameIndex, lastTextureFrame,
                                            catchupFrame.ringIndex,
                                            static_cast<unsigned long long>(catchupFrame.fenceValue),
                                            static_cast<long long>(catchupFrame.timestamp));
                                        if (g_pSharedMem) {
                                            g_pSharedMem->runtimeState.textureReuseAnomalies.fetch_add(
                                                1, std::memory_order_relaxed);
                                        }
                                    }
                                    lastTextureFrame = catchupFrame.frameIndex;
                                }

                                if (g_pSharedMem) {
                                    if (currentEncodeMs > frameIntervalMs * 1.10) {
                                        g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
                                    }
                                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                                    g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1,
                                                                                           std::memory_order_relaxed);
                                    g_pSharedMem->frameRing.readIndex.store(catchupFrame.ringIndex + 1,
                                                                            std::memory_order_release);
                                }

                                g_LastFrame = std::move(catchupFrame);
                                g_HasLastFrame = true;
                                if (g_LastFrame.timestamp > 0) {
                                    lastEmittedInjectSourceQpc = g_LastFrame.timestamp;
                                }
                                lastDeferredLineage = {};
                                frameCreditAccumulator -= 1.0;
                                cadenceCounters.consecutiveDeferredFrames = 0;
                                cadenceCounters.consecutiveDuplicateFrames = 0;
                                cadenceCounters.liveTickEmitCount++;
                                cadenceCounters.liveTickUniqueCount++;
                                cadenceCounters.CommitHoldRun();
                                cadenceCounters.holdTicksRunning = 1;
                                ++liveTicksOutput;
                                ++encoderGridTickCount;
                                ++cfrCatchupTicksExecuted;
                                ++injectFreshCatchupThisWindow;
                                ++injectFreshCatchupTotal;
                                nextSampleTime.QuadPart += targetIntervalTicks;
                                continue;
                            }

                            if (catchupEncodeDeferred) {
                                frameCreditAccumulator = std::max(frameCreditAccumulator, 1.0);
                                g_InjectDeferredFrames.fetch_add(1, std::memory_order_relaxed);
                                if (g_pSharedMem) {
                                    g_pSharedMem->runtimeState.deferredFrames.fetch_add(1, std::memory_order_relaxed);
                                }
                                cadenceCounters.consecutiveDeferredFrames++;
                                cadenceCounters.maxConsecutiveDeferredFrames =
                                    std::max(cadenceCounters.maxConsecutiveDeferredFrames,
                                             cadenceCounters.consecutiveDeferredFrames);
                                lastDeferredLineage = catchupLineage;
                                catchupFrame.deferCount++;
                                if (!g_RejectInjectFrames.load(std::memory_order_acquire) &&
                                    catchupFrame.deferCount <= ce::capture_policy::kMaxInjectDeferredFrameRetries) {
                                    bufferedInjectFrames.push_front(std::move(catchupFrame));
                                    ++injectDeferredRequeuedThisWindow;
                                    ++injectDeferredRequeuedTotal;
                                } else {
                                    DiscardQueuedFrame(catchupFrame);
                                    ++injectDeferredDroppedThisWindow;
                                    ++injectDeferredDroppedTotal;
                                }
                            } else {
                                DiscardQueuedFrame(catchupFrame);
                            }
                        }
                    }
                }

                if (allowFreshCatchup && useScreenGrab && MediaEngine_ProcessFrameD3D11 && !bufferedWgcFrames.empty()) {
                    const int64_t catchupGridTick = encoderGridTickCount + 1;
                    LARGE_INTEGER catchupNowQpc;
                    QueryPerformanceCounter(&catchupNowQpc);
                    const int64_t catchupSelectionTargetQpc = clampWgcSelectionTargetQpc(
                        computeWgcSelectionTargetForTick(repeatScheduledQpc, catchupGridTick, false),
                        catchupNowQpc.QuadPart);
                    QueuedFrame catchupFrame;
                    if (tryPopBufferedWgcFrameForTarget(catchupSelectionTargetQpc, catchupSelectionTargetQpc,
                                                        catchupNowQpc.QuadPart, false, &catchupFrame)) {
                        if (!ce::capture_policy::ShouldUseFreshWgcCatchupFrame(
                                GetFrameSelectionTimestamp(catchupFrame), catchupNowQpc.QuadPart, targetIntervalTicks,
                                qpcFreq.QuadPart, outputShortfallTicks)) {
                            LogWarn(
                                "[EncoderThread] WGC CFR stale fresh-catchup blocked: frameQpc=%lld nowQpc=%lld "
                                "shortfall=%u buffered=%zu",
                                static_cast<long long>(GetFrameSelectionTimestamp(catchupFrame)),
                                static_cast<long long>(catchupNowQpc.QuadPart), outputShortfallTicks,
                                bufferedWgcFrames.size());
                            ReleaseQueuedFrameTexture(catchupFrame);
                            ++wgcFreshSelectionMissCount;
                            allowFreshCatchup = false;
                        } else {
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
                                const uint64_t absoluteOutputScheduleErrorUs = static_cast<uint64_t>(
                                    signedOutputScheduleErrorUs >= 0 ? signedOutputScheduleErrorUs
                                                                     : -signedOutputScheduleErrorUs);
                                cadenceCounters.outputScheduleErrorAccumUs += absoluteOutputScheduleErrorUs;
                                cadenceCounters.outputScheduleErrorSignedAccumUs += signedOutputScheduleErrorUs;
                                cadenceCounters.outputScheduleErrorSamples++;
                                cadenceCounters.outputScheduleErrorMaxUs =
                                    std::max(cadenceCounters.outputScheduleErrorMaxUs,
                                             SaturatingToUint32(absoluteOutputScheduleErrorUs));
                                if (signedOutputScheduleErrorUs < 0) {
                                    cadenceCounters.outputScheduleEarlyMaxUs = std::max(
                                        cadenceCounters.outputScheduleEarlyMaxUs,
                                        SaturatingToUint32(static_cast<uint64_t>(-signedOutputScheduleErrorUs)));
                                } else {
                                    cadenceCounters.outputScheduleLateMaxUs = std::max(
                                        cadenceCounters.outputScheduleLateMaxUs,
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
                            UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs,
                                                        ce::capture_policy::IsEncoderStartupWindow(
                                                            recordingOutputLive, recordingLiveTick, GetTickCount64()));

                            if (g_pSharedMem) {
                                if (currentEncodeMs > frameIntervalMs * 1.10) {
                                    g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
                                }
                                g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                                g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                            }

                            if (catchupSelectionTargetQpc > 0 && g_LastFrame.timestamp > 0) {
                                if (encoderTooSlowForTargetCurrent) {
                                    static uint64_t s_lastCatchupLog = 0;
                                    uint64_t nowTick = GetTickCount64();
                                    if (nowTick - s_lastCatchupLog >= 1000) {
                                        LogInfo(
                                            "[EncoderThread] CFR Catchup applied using fresh frame (encoder slow, but "
                                            "preserving smoothness)");
                                        s_lastCatchupLog = nowTick;
                                    }
                                }
                                const int64_t signedSelectionErrorUs =
                                    ((GetFrameSelectionTimestamp(g_LastFrame) - catchupSelectionTargetQpc) * 1000000) /
                                    qpcFreq.QuadPart;
                                const int64_t absoluteSelectionErrorUs =
                                    signedSelectionErrorUs >= 0 ? signedSelectionErrorUs : -signedSelectionErrorUs;
                                cadenceCounters.selectionErrorAccumUs +=
                                    static_cast<uint64_t>(absoluteSelectionErrorUs);
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
                            if (remainingFreshCatchupBudget > 0u) {
                                --remainingFreshCatchupBudget;
                            }
                            nextSampleTime.QuadPart += targetIntervalTicks;
                            continue;
                        }
                    }

                    // Coverage-loss policy may intentionally hold fresh catch-up here
                    // so the existing repeat path can absorb the mismatch instead.
                    // For pure CFR shortfall, we MUST NOT break here! We must fall through
                    // to repeatLastFrameForScheduledQpc to fill the timeline gaps!
                    if (!(!config.video.useVFR && outputShortfallTicks > 0)) {
                        break;
                    }
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
                UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs,
                                            ce::capture_policy::IsEncoderStartupWindow(
                                                recordingOutputLive, recordingLiveTick, GetTickCount64()));

                if (g_pSharedMem) {
                    if (currentEncodeMs > frameIntervalMs * 1.10) {
                        g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
                    }
                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                }

                recordDuplicate(&g_LastFrame, duplicateLineage, false, false, false, true);
                if (useScreenGrab) {
                    ++wgcRepeatCatchupCount;
                } else {
                    ++injectRepeatCatchupThisWindow;
                    ++injectRepeatCatchupTotal;
                }
                cadenceCounters.liveTickEmitCount++;
                cadenceCounters.liveTickDuplicateCount++;
                cadenceCounters.holdTicksRunning++;
                ++liveTicksOutput;
                ++encoderGridTickCount;
                ++cfrCatchupTicksExecuted;
                nextSampleTime.QuadPart += targetIntervalTicks;
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
                UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs,
                                            ce::capture_policy::IsEncoderStartupWindow(
                                                recordingOutputLive, recordingLiveTick, GetTickCount64()));
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
                    ? (wgcSelectionDelayAppliedThisTick ? computeDelayedWgcSelectionTargetQpc()
                                                        : computeLiveWgcSelectionTargetQpc())
                    : idealQpc;
            if (selectionMetricTargetQpc > 0) {
                const int64_t selectionTimestampQpc = !frameToProcess->isInjectMode
                                                          ? GetFrameSelectionTimestamp(*frameToProcess)
                                                          : frameToProcess->timestamp;
                signedSelectionErrorUs =
                    ((selectionTimestampQpc - selectionMetricTargetQpc) * 1000000) / qpcFreq.QuadPart;
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
            UpdateEncoderBottleneckFlag(
                smoothedEncodeMs, frameIntervalMs,
                ce::capture_policy::IsEncoderStartupWindow(recordingOutputLive, recordingLiveTick, GetTickCount64()));

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
                    QueuedFrame deferredFrame = std::move(frame);
                    deferredFrame.deferCount++;
                    if (!g_RejectInjectFrames.load(std::memory_order_acquire) &&
                        deferredFrame.deferCount <= ce::capture_policy::kMaxInjectDeferredFrameRetries) {
                        bufferedInjectFrames.push_front(std::move(deferredFrame));
                        ++injectDeferredRequeuedThisWindow;
                        ++injectDeferredRequeuedTotal;
                    } else {
                        DiscardQueuedFrame(deferredFrame);
                        ++injectDeferredDroppedThisWindow;
                        ++injectDeferredDroppedTotal;
                    }
                    frameToProcess = nullptr;
                    popped = false;
                    static uint64_t s_lastDeferredLogTick = 0;
                    uint64_t nowTick = GetTickCount64();
                    if (nowTick - s_lastDeferredLogTick >= 1000) {
                        LogInfo(
                            "[EncoderThread] Deferred inject frame=%u ring=%u tex=%d fence=%llu ts=%lld buffered=%zu "
                            "credit=%.3f requeued=%llu dropped=%llu",
                            deferredLineage.frameIndex, deferredLineage.ringIndex, deferredLineage.textureIndex,
                            static_cast<unsigned long long>(deferredLineage.fenceValue),
                            static_cast<long long>(deferredLineage.timestamp), bufferedInjectFrames.size(),
                            frameCreditAccumulator, static_cast<unsigned long long>(injectDeferredRequeuedTotal),
                            static_cast<unsigned long long>(injectDeferredDroppedTotal));
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

                if (popped && frameToProcess && frameToProcess->isInjectMode && encodeSucceeded) {
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

                if (!isDuplicate && frameToProcess && frameToProcess->frameIndex != 0) {
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
                if (!isDuplicate && frameToProcess && IsInjectTextureIndexValid(frameToProcess->textureIndex)) {
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

                if (encodeSucceeded && !isDuplicate && frameToProcess && frameToProcess->timestamp > 0) {
                    lastEmittedInjectSourceQpc = frameToProcess->timestamp;
                }

                if (g_pSharedMem) {
                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    if (isLivePhase) {
                        g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    } else if (isDrainPhase) {
                        g_pSharedMem->runtimeState.drainFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (frameToProcess) {
                        g_pSharedMem->frameRing.readIndex.store(frameToProcess->ringIndex + 1,
                                                                std::memory_order_release);
                    }
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
                if (selectionMetricTargetQpc > 0 && frameToProcess && !frameToProcess->isInjectMode && !isDuplicate) {
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
                    if (liveStartQpc.QuadPart == 0 && liveTicksOutput == 0) {
                        LARGE_INTEGER afterInit;
                        QueryPerformanceCounter(&afterInit);
                        liveStartQpc = afterInit;
                        // For the selection grid, we treat the first frame as tick 1.
                        // To align future idealQpc calculations perfectly with scheduledSampleQpc,
                        // we must offset the anchor back by one target interval.
                        encoderGridStartQpc = liveStartQpc.QuadPart - targetIntervalTicks;
                        // Start the CFR timeline exactly 1 tick from now, skipping the init delay entirely
                        nextSampleTime.QuadPart = liveStartQpc.QuadPart + targetIntervalTicks;
                        LogInfo("[EncoderThread] Anchored CFR live timeline after first frame (init delay skipped)");
                    }
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
            state.wgcSelectionErrorAvgUs.store(avgWgcSelectionErrorUs, std::memory_order_relaxed);
            state.wgcSelectionErrorMaxUs.store(wgcSelectionErrorMaxUs, std::memory_order_relaxed);
            state.wgcSelectionErrorSignedAvgUs.store(avgSignedWgcSelectionErrorUs, std::memory_order_relaxed);
            state.wgcSelectionEarlyMaxUs.store(wgcSelectionEarlyMaxUs, std::memory_order_relaxed);
            state.wgcSelectionLateMaxUs.store(wgcSelectionLateMaxUs, std::memory_order_relaxed);
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
            state.encoderSustainFpsX100.store(
                static_cast<uint32_t>(std::clamp(sustainableOutputFps * 100.0, 0.0, 4294967295.0)),
                std::memory_order_relaxed);
            const uint32_t encoderBudgetUtilizationPermille =
                ce::capture_policy::GetEncoderBudgetUtilizationPermille(smoothedEncodeMs, frameIntervalMs);
            const bool encoderTooSlowForTarget =
                ce::capture_policy::IsEncoderTooSlowForTargetFps(smoothedEncodeMs, frameIntervalMs, outputFps);
            const double oldestBufferedFrameAgeMs = static_cast<double>(oldestBufferedFrameAgeUs) / 1000.0;
            if (wgcStarvedEpisode.active) {
                wgcStarvedEpisode.maxEncodeEmaMs = std::max(wgcStarvedEpisode.maxEncodeEmaMs, smoothedEncodeMs);
                wgcStarvedEpisode.maxMuxBackpressureCount =
                    std::max(wgcStarvedEpisode.maxMuxBackpressureCount, muxBackpressureCount);
                wgcStarvedEpisode.maxMuxBackpressureWaitUs =
                    std::max(wgcStarvedEpisode.maxMuxBackpressureWaitUs, muxBackpressureMaxWaitUs);
                wgcStarvedEpisode.maxMuxQueueKb =
                    std::max(wgcStarvedEpisode.maxMuxQueueKb, (muxQueueBytes + 1023u) / 1024u);
                wgcStarvedEpisode.peakOverloadFlags |= overloadFlags;
                wgcStarvedEpisode.maxFenceUs =
                    std::max(wgcStarvedEpisode.maxFenceUs, SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(
                                                               0, MediaEngine_GetLastFrameFenceWaitUs()))));
                if (g_WgcCap) {
                    wgcStarvedEpisode.maxCallbackGapUs = std::max(
                        wgcStarvedEpisode.maxCallbackGapUs, SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(
                                                                0, g_WgcCap->GetCallbackGapMaxUs()))));
                    wgcStarvedEpisode.maxCopyUs = std::max(
                        wgcStarvedEpisode.maxCopyUs,
                        SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(0, g_WgcCap->GetLastCopyTimeUs()))));
                }
            }

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
            uint32_t wgcCaptureHealthFlags = 0;
            if (wgcSourceStarvedCurrent ||
                (wgcNoFreshTickPermille >= ce::capture_policy::kWgcDeepUnderfeedEmptyTickPermille &&
                 wgcRecentInputMin250Fps + ce::capture_policy::kWgcRecoverySourceMarginFps < outputFps)) {
                wgcCaptureHealthFlags |= ce::capture_policy::kWgcCaptureHealthFlagSourceStarved;
            }
            if (wgcSchedulerLimitedCurrent) {
                wgcCaptureHealthFlags |= ce::capture_policy::kWgcCaptureHealthFlagSchedulerLimited;
            }
            state.wgcCaptureHealthFlags.store(wgcCaptureHealthFlags, std::memory_order_relaxed);
            state.wgcCaptureHealthFps.store(wgcRecentInputMin250Fps, std::memory_order_relaxed);

            // Flush the in-progress hold run into the histogram before logging,
            // but preserve the running count so it continues into the next interval.
            const uint32_t savedHoldTicks = cadenceCounters.holdTicksRunning;
            cadenceCounters.CommitHoldRun();

            // Compute input frame rate predictor diagnostics
            const InputFrameRatePredictor& activeInputPredictor =
                useScreenGrab ? wgcInputPredictor : injectInputPredictor;
            const uint32_t srcFpsX100Val =
                activeInputPredictor.IsCalibrated()
                    ? static_cast<uint32_t>(activeInputPredictor.GetPredictedFps(qpcFreq.QuadPart) * 100.0)
                    : 0u;
            const uint32_t srcJitterUsVal =
                activeInputPredictor.IsCalibrated()
                    ? static_cast<uint32_t>(activeInputPredictor.GetJitterUs(qpcFreq.QuadPart))
                    : 0u;
            const uint32_t dupTsPerSec =
                g_WgcCap ? g_WgcCap->GetNormalizedDuplicateTimestampCount() : dupTimestampCount;
            dupTimestampCount = 0;
            encCycleMaxMs = 0;

            accumulateCaptureSummarySample(useScreenGrab, srcFpsX100Val, srcJitterUsVal, dupNoSource, dupDeferred,
                                           dupTimer, dupDrain, oldestBufferedFrameAgeUs, shortfallDurationMs,
                                           sustainableOutputFps);

            LogInfo(
                "[Cadence Health] Phase=%s | AgeAvg=%uus AgeMax=%uus | SelAvg=%uus SelMax=%uus SelBias=%dus "
                "EarlyMax=%uus LateMax=%uus | WgcSelAvg=%uus WgcSelMax=%uus WgcSelBias=%dus WgcEarly=%uus WgcLate=%uus "
                "Hold=%u HoldFresh=%u Delay=%u Spend=%u CatchUp=%u CatchFresh=%u InjectCatch=%u/%u "
                "InjectAgeTrim=%u PathMismatch=%u/%llu LiveClamp=%u/%uus | DefStreak=%u/%u "
                "DupStreak=%u/%u | DupSrc=%u "
                "DupDef=%u "
                "DupTimer=%u DupDrain=%u InjectDefReQ=%u InjectDefDrop=%u | TickEmit=%u TickUnique=%u TickDup=%u "
                "TickMiss=%u | "
                "HoldHist=%u/%u/%u/%u/%u/%u | LiveWall=%lluus LiveTicks=%llu Shortfall=%u/%.1fms FreshMiss=%upm "
                "BufAvg=%upm BufMin=%u BufNow=%zu NoFresh=%u NoReserve=%u Oldest=%.1fms LeadExcess=%.1fms | "
                "WgcAct Fresh=%u "
                "DupSrc=%u DropObs=%u "
                "DropDebt=%u/%llu DebtMax=%uus SelMiss=%u StaleUni=%u "
                "Ancient=%u RepFreshMiss=%u RepHold=%u RepCov=%u CovDelay=%u RepLate=%u RepCatch=%u | TsReg=%u "
                "TsStall=%u "
                "TimerRebase=%u WgcDebtMax=%llu WgcLiveRebase=%u/%llu/%u | "
                "EncLowBypass=%u/%llu ModeMis=%u/%llu SrcBack=%u/%llu | "
                "InvalidMeta=%u InvalidHandle=%u | PktClamp=%u NegPTS=%u NonMonoPTS=%u | WgcThr=%u Adj=%u | Over=0x%X "
                "MuxQ=%uKB/%u MuxBp=%u Wait=%uus Max=%uus | EncEma=%.2fms Budget=%upm Sust=%.1ffps TooSlow=%d "
                "Bottleneck=%d | LowSrc=%d Recover=%d Cause=S%d/D%d/E%d | SrcFps=%.2f SrcJitter=%uus DupTs=%u "
                "EncCycle=%.2fms EncSpike=%u",
                CapturePipelinePhaseToString(state.capturePhase.load(std::memory_order_relaxed)), avgFrameAgeUs,
                cadenceCounters.frameAgeMaxUs, avgSelectionErrorUs, cadenceCounters.outputScheduleErrorMaxUs,
                avgSignedSelectionErrorUs, cadenceCounters.outputScheduleEarlyMaxUs,
                cadenceCounters.outputScheduleLateMaxUs, avgWgcSelectionErrorUs, wgcSelectionErrorMaxUs,
                avgSignedWgcSelectionErrorUs, wgcSelectionEarlyMaxUs, wgcSelectionLateMaxUs, wgcHoldForNextTickCount,
                wgcHeldFreshFrameTickCount, wgcSelectionDelayTickCount, wgcReserveSpendTickCount,
                cfrCatchupTicksExecuted, wgcFreshCatchupCount, injectFreshCatchupThisWindow,
                injectRepeatCatchupThisWindow, injectLiveStaleTrimThisWindow, activePathMismatchDiscardThisWindow,
                static_cast<unsigned long long>(g_ActivePathMismatchFramesDiscarded.load(std::memory_order_relaxed)),
                wgcSelectionTargetClampCount, wgcSelectionTargetClampMaxUs, cadenceCounters.consecutiveDeferredFrames,
                cadenceCounters.maxConsecutiveDeferredFrames, cadenceCounters.consecutiveDuplicateFrames,
                cadenceCounters.maxConsecutiveDuplicateFrames, dupNoSource - lastDuplicateReasonNoSource,
                dupDeferred - lastDuplicateReasonDeferred, dupTimer - lastDuplicateReasonTimerRebase,
                dupDrain - lastDuplicateReasonDrain, injectDeferredRequeuedThisWindow, injectDeferredDroppedThisWindow,
                cadenceCounters.liveTickEmitCount, cadenceCounters.liveTickUniqueCount,
                cadenceCounters.liveTickDuplicateCount, cadenceCounters.liveTickMissCount, cadenceCounters.holdHist[0],
                cadenceCounters.holdHist[1], cadenceCounters.holdHist[2], cadenceCounters.holdHist[3],
                cadenceCounters.holdHist[4], cadenceCounters.holdHist[5],
                static_cast<unsigned long long>(liveWallElapsedUs), static_cast<unsigned long long>(liveTicksOutput),
                outputShortfallTicks, shortfallDurationMs, wgcNoFreshTickPermille, bufferedAtTickAvgPermille,
                bufferedAtTickMinValue, bufferedWgcFrames.size(), wgcNoFreshTickCount, wgcNoReserveTickCount,
                oldestBufferedFrameAgeMs, wgcAudioLeadExcessMsCurrent, wgcSelectFreshCount,
                wgcSelectDuplicateSourceCount, wgcDropObsoleteCount, wgcDropStaleDebtCount,
                static_cast<unsigned long long>(wgcDropStaleDebtTotal), wgcDropStaleDebtMaxUs,
                wgcFreshSelectionMissCount, wgcStaleUniqueFallbackCount, wgcAncientSelectionCount,
                wgcRepeatNoFreshCount, wgcRepeatPolicyHoldCount, wgcCoverageRepeatHoldCount,
                wgcCoverageDelayTicksCurrent, wgcRepeatTimerLateCount, wgcRepeatCatchupCount,
                tsRegress - lastTimestampRegressionCount, tsStall - lastTimestampStallCount, timerRebases,
                static_cast<unsigned long long>(wgcVisualDebtMaxExcessTicks), wgcLiveSchedulerRebaseThisWindow,
                static_cast<unsigned long long>(wgcLiveSchedulerRebaseTotal), wgcLiveSchedulerRebaseMaxTicks,
                wgcEncoderLimitedSuppressedByLowSourceThisWindow,
                static_cast<unsigned long long>(wgcEncoderLimitedSuppressedByLowSourceTotal),
                wgcCapacityPressureModeMismatchThisWindow,
                static_cast<unsigned long long>(wgcCapacityPressureModeMismatchTotal),
                wgcSelectedSourceBacktrackThisWindow, static_cast<unsigned long long>(wgcSelectedSourceBacktrackTotal),
                invalidMeta - lastInvalidMetaCount, invalidHandle - lastInvalidHandleCount,
                packetClamps - lastPacketClampCount, negativePts - lastNegativePtsCount,
                nonMonotonicPts - lastNonMonotonicPtsCount, g_WgcAdaptiveTargetFps.load(std::memory_order_relaxed),
                wgcAdaptiveThrottleAdjustments, overloadFlags, (muxQueueBytes + 1023u) / 1024u, muxQueuePackets,
                muxBackpressureCount, muxBackpressureWaitUs, muxBackpressureMaxWaitUs, smoothedEncodeMs,
                encoderBudgetUtilizationPermille, sustainableOutputFps, encoderTooSlowForTarget ? 1 : 0,
                g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ? 1 : 0, wgcLowSourceModeActive ? 1 : 0,
                wgcLiveRecoveryModeActive ? 1 : 0, wgcSourceStarvedCurrent ? 1 : 0, wgcSchedulerLimitedCurrent ? 1 : 0,
                wgcEncoderRecoveryLimitedCurrent ? 1 : 0, srcFpsX100Val / 100.0, srcJitterUsVal, dupTsPerSec,
                smoothedEncCycleMs, encodeSpikeCountThisSecond);

            const bool wgcEncoderLimitedSmoothnessActive = isWgcEncoderLimitedSmoothnessMode();
            if (useScreenGrab && recordingOutputLive &&
                (wgcEncoderLimitedSmoothnessActive || wgcSourceStarvedCurrent || wgcSchedulerLimitedCurrent ||
                 outputShortfallTicks > 0 || wgcRepeatPolicyHoldCount > 0 || wgcDropStaleDebtCount > 0)) {
                ++wgcEncoderLimitedCadenceEventCount;
                const char* cadenceMode = wgcEncoderLimitedSmoothnessActive ? "encoder_limited"
                                          : wgcSourceStarvedCurrent         ? "source_starved"
                                          : wgcSchedulerLimitedCurrent      ? "scheduler_limited"
                                                                            : "normal_pressure";
                LogInfo(
                    "[WGC CFR CADENCE EVENT] mode=%s shortfall=%u/%.1fms phaseErrorAvg=%dus "
                    "phaseErrorMax=%uus rebaseWindow=%u encoderDropWindow=%u encoderDropTotal=%llu "
                    "tooNewRepeat=%u staleDrop=%u freshMiss=%upm bufNow=%zu oldest=%.1fms enc=%.2fms "
                    "sustain=%.1ffps overload=0x%X lowSourceBypass=%u modeMismatch=%u sourceBacktrack=%u "
                    "cause=S%d/D%d/E%d",
                    cadenceMode, outputShortfallTicks, shortfallDurationMs, avgSignedWgcSelectionErrorUs,
                    wgcSelectionErrorMaxUs, wgcLiveSchedulerRebaseThisWindow, wgcEncoderLimitedSourceDropThisWindow,
                    static_cast<unsigned long long>(wgcEncoderLimitedSourceDropTotal), wgcRepeatPolicyHoldCount,
                    wgcDropStaleDebtCount, wgcNoFreshTickPermille, bufferedWgcFrames.size(), oldestBufferedFrameAgeMs,
                    smoothedEncodeMs, sustainableOutputFps, overloadFlags,
                    wgcEncoderLimitedSuppressedByLowSourceThisWindow, wgcCapacityPressureModeMismatchThisWindow,
                    wgcSelectedSourceBacktrackThisWindow, wgcSourceStarvedCurrent ? 1 : 0,
                    wgcSchedulerLimitedCurrent ? 1 : 0, wgcEncoderRecoveryLimitedCurrent ? 1 : 0);
            }

            static uint64_t s_lastWgcCapacityWarnTick = 0;
            static uint64_t s_lastWgcSourceLimitedInfoTick = 0;
            static uint32_t s_wgcCapacityLimitedStreakSeconds = 0;
            if (useScreenGrab && recordingOutputLive) {
                const bool encoderPressure = g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ||
                                             (overloadFlags & ce::capture_policy::kEncoderOverloadFlagEncoder) != 0 ||
                                             smoothedEncodeMs >= frameIntervalMs;
                const bool muxPressure =
                    (overloadFlags & ce::capture_policy::kEncoderOverloadFlagMux) != 0 || muxBackpressureWaitUs > 0;
                const bool captureLimitedForOverlay =
                    ce::capture_policy::IsWgcCaptureLimitedForOverlay(wgcCaptureHealthFlags);
                const bool hardCapacityPressure = muxPressure || (encoderPressure && !captureLimitedForOverlay);
                const bool capacityLimitedThisSecond =
                    hardCapacityPressure && (outputShortfallTicks > 0 || oldestBufferedFrameAgeUs > 0);
                s_wgcCapacityLimitedStreakSeconds =
                    capacityLimitedThisSecond ? (s_wgcCapacityLimitedStreakSeconds + 1) : 0;
                const uint64_t nowTick = GetTickCount64();
                if (hardCapacityPressure && (nowTick - s_lastWgcCapacityWarnTick) >= 5000) {
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
                const bool sourceStarvedPressure =
                    wgcSourceStarvedCurrent ||
                    (wgcNoFreshTickPermille >= ce::capture_policy::kWgcDeepUnderfeedEmptyTickPermille &&
                     wgcRecentInputMin250Fps + ce::capture_policy::kWgcRecoverySourceMarginFps < outputFps);
                if (sourceStarvedPressure && (nowTick - s_lastWgcSourceLimitedInfoTick) >= 5000) {
                    LogInfo(
                        "[WGC CFR] Source-limited CFR repeats: target=%ufps input=%u/%u delivered=%u/%u freshMiss=%upm "
                        "buffered=%u oldest=%.1fms shortfall=%u/%.1fms duplicates=%u cause=S%d/D%d/E%d "
                        "encoderPressure=%d muxPressure=%d overlayEncoderWarn=%d",
                        outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcRecentDeliveredMin250Fps,
                        wgcRecentDeliveredMin500Fps, wgcNoFreshTickPermille, bufferedAtTickMinValue,
                        oldestBufferedFrameAgeMs, outputShortfallTicks, shortfallDurationMs,
                        cadenceCounters.liveTickDuplicateCount, wgcSourceStarvedCurrent ? 1 : 0,
                        wgcSchedulerLimitedCurrent ? 1 : 0, wgcEncoderRecoveryLimitedCurrent ? 1 : 0,
                        encoderPressure ? 1 : 0, muxPressure ? 1 : 0,
                        ce::capture_policy::SelectWgcOverlayWarningKind(overloadFlags, wgcCaptureHealthFlags) ==
                                ce::capture_policy::kOverlayWarningEncoderOverload
                            ? 1
                            : 0);
                    s_lastWgcSourceLimitedInfoTick = nowTick;
                }
            } else if (!useScreenGrab && recordingOutputLive) {
                s_wgcCapacityLimitedStreakSeconds = 0;
                static uint64_t s_lastInjectRepeatPressureInfoTick = 0;
                const uint32_t duplicateTicksThisWindow = cadenceCounters.liveTickDuplicateCount;
                const uint32_t deferredRepeatsThisWindow = dupDeferred - lastDuplicateReasonDeferred;
                const uint32_t sourceRepeatsThisWindow = dupNoSource - lastDuplicateReasonNoSource;
                const bool hardEncoderPressure =
                    (overloadFlags & ce::capture_policy::kEncoderOverloadFlagEncoder) != 0 ||
                    (overloadFlags & ce::capture_policy::kEncoderOverloadFlagMux) != 0 ||
                    g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
                const uint64_t nowTick = GetTickCount64();
                if ((duplicateTicksThisWindow > 0 || injectDeferredRequeuedThisWindow > 0 ||
                     injectFreshCatchupThisWindow > 0 || injectLiveStaleTrimThisWindow > 0) &&
                    (nowTick - s_lastInjectRepeatPressureInfoTick) >= 5000) {
                    LogInfo(
                        "[Inject CFR] Repeat pressure: hardEncoderOverload=%d dup=%u srcLimited=%u fenceDeferred=%u "
                        "timer=%u freshCatchup=%u repeatCatchup=%u staleTrim=%u requeued=%u droppedDeferred=%u "
                        "tickEmit=%u unique=%u sourceFps=%.2f enc=%.2fms sustain=%.1ffps overload=0x%X",
                        hardEncoderPressure ? 1 : 0, duplicateTicksThisWindow, sourceRepeatsThisWindow,
                        deferredRepeatsThisWindow, dupTimer - lastDuplicateReasonTimerRebase,
                        injectFreshCatchupThisWindow, injectRepeatCatchupThisWindow, injectLiveStaleTrimThisWindow,
                        injectDeferredRequeuedThisWindow, injectDeferredDroppedThisWindow,
                        cadenceCounters.liveTickEmitCount, cadenceCounters.liveTickUniqueCount, srcFpsX100Val / 100.0,
                        smoothedEncodeMs, sustainableOutputFps, overloadFlags);
                    s_lastInjectRepeatPressureInfoTick = nowTick;
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
            injectDeferredRequeuedThisWindow = 0;
            injectDeferredDroppedThisWindow = 0;
            injectFreshCatchupThisWindow = 0;
            injectRepeatCatchupThisWindow = 0;
            injectLiveStaleTrimThisWindow = 0;
            activePathMismatchDiscardThisWindow = 0;
            cadenceCounters.Reset();
            cadenceCounters.holdTicksRunning = savedHoldTicks;  // Preserve in-progress hold run
            wgcSelectionErrorAccumUs = 0;
            wgcSelectionErrorSignedAccumUs = 0;
            wgcSelectionErrorSamples = 0;
            wgcSelectionErrorMaxUs = 0;
            wgcSelectionEarlyMaxUs = 0;
            wgcSelectionLateMaxUs = 0;
            wgcSelectionTargetClampCount = 0;
            wgcSelectionTargetClampMaxUs = 0;
            wgcHoldForNextTickCount = 0;
            wgcHeldFreshFrameTickCount = 0;
            wgcSelectionDelayTickCount = 0;
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
            wgcDropStaleDebtCount = 0;
            wgcDropStaleDebtMaxUs = 0;
            wgcEncoderLimitedSourceDropThisWindow = 0;
            wgcEncoderLimitedSuppressedByLowSourceThisWindow = 0;
            wgcCapacityPressureModeMismatchThisWindow = 0;
            wgcSelectedSourceBacktrackThisWindow = 0;
            wgcLiveSchedulerRebaseThisWindow = 0;
            lastHealthLog = GetTickCount();
        }
    }

    if (hTimer) {
        CloseHandle(hTimer);
    }

    if (!bufferedWgcFrames.empty()) {
        ClearBufferedWgcFrames();
    }

    if (g_pSharedMem && liveTicksOutput > 0) {
        auto& state = g_pSharedMem->runtimeState;
        const bool useScreenGrab = IsActiveScreenGrab();
        const uint32_t dupNoSource = state.duplicateFramesNoSource.load(std::memory_order_relaxed);
        const uint32_t dupDeferred = state.duplicateFramesDeferred.load(std::memory_order_relaxed);
        const uint32_t dupTimer = state.duplicateFramesTimerRebase.load(std::memory_order_relaxed);
        const uint32_t dupDrain = state.duplicateFramesDrain.load(std::memory_order_relaxed);
        const uint32_t oldestBufferedFrameAgeUs = state.oldestBufferedFrameAgeUs.load(std::memory_order_relaxed);
        uint32_t outputShortfallTicks = 0;
        if (recordingOutputLive && liveStartQpc.QuadPart > 0 && targetIntervalTicks > 0 && liveTicksScheduled > 0) {
            LARGE_INTEGER nowQpc;
            QueryPerformanceCounter(&nowQpc);
            if (nowQpc.QuadPart > liveStartQpc.QuadPart) {
                outputShortfallTicks = updateLiveCfrShortfall(nowQpc.QuadPart);
            }
        }
        const double shortfallDurationMs =
            ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs);
        const double sustainableOutputFps = ce::capture_policy::GetEncoderSustainableOutputFps(smoothedEncodeMs);
        const InputFrameRatePredictor& activeInputPredictor = useScreenGrab ? wgcInputPredictor : injectInputPredictor;
        const uint32_t srcFpsX100Val =
            activeInputPredictor.IsCalibrated()
                ? static_cast<uint32_t>(activeInputPredictor.GetPredictedFps(qpcFreq.QuadPart) * 100.0)
                : 0u;
        const uint32_t srcJitterUsVal = activeInputPredictor.IsCalibrated()
                                            ? static_cast<uint32_t>(activeInputPredictor.GetJitterUs(qpcFreq.QuadPart))
                                            : 0u;
        accumulateCaptureSummarySample(useScreenGrab, srcFpsX100Val, srcJitterUsVal, dupNoSource, dupDeferred, dupTimer,
                                       dupDrain, oldestBufferedFrameAgeUs, shortfallDurationMs, sustainableOutputFps);
    }

    if (wgcStarvedEpisode.active) {
        const uint64_t durationMs = GetTickCount64() - wgcStarvedEpisode.startTickMs;
        const uint64_t outputTicks = liveTicksOutput - wgcStarvedEpisode.startLiveTicks;
        const uint64_t duplicateTicks = captureSessionSummary.duplicateTicks - wgcStarvedEpisode.startDuplicateTicks;
        finishWgcStarvedEpisode(durationMs, outputTicks, duplicateTicks);
    }

    if (liveTicksOutput > 0) {
        const uint64_t duplicatePermille = (captureSessionSummary.duplicateTicks * 1000ull) / liveTicksOutput;
        if (IsActiveScreenGrab()) {
            const uint64_t noFreshPermille =
                captureSessionSummary.queueTickSamples > 0
                    ? (captureSessionSummary.noFreshTicks * 1000ull) / captureSessionSummary.queueTickSamples
                    : 0ull;
            const uint64_t noReservePermille =
                captureSessionSummary.queueTickSamples > 0
                    ? (captureSessionSummary.noReserveTicks * 1000ull) / captureSessionSummary.queueTickSamples
                    : 0ull;
            LogInfo(
                "[WGC CFR SUMMARY] Live=%llu Dup=%llu DupPct=%.1f%% NoFresh=%llupm NoReserve=%llupm DupReason(src=%llu "
                "def=%llu timer=%llu drain=%llu) SourceLimitedRepeats=%llu StarvedEpisodes=%llu longest=%llums "
                "longestDup=%llu worstIn=%u "
                "worstDel=%u",
                static_cast<unsigned long long>(liveTicksOutput),
                static_cast<unsigned long long>(captureSessionSummary.duplicateTicks),
                static_cast<double>(duplicatePermille) / 10.0, static_cast<unsigned long long>(noFreshPermille),
                static_cast<unsigned long long>(noReservePermille),
                static_cast<unsigned long long>(captureSessionSummary.duplicateNoSourceTicks),
                static_cast<unsigned long long>(captureSessionSummary.duplicateDeferredTicks),
                static_cast<unsigned long long>(captureSessionSummary.duplicateTimerTicks),
                static_cast<unsigned long long>(captureSessionSummary.duplicateDrainTicks),
                static_cast<unsigned long long>(captureSessionSummary.duplicateNoSourceTicks),
                static_cast<unsigned long long>(captureSessionSummary.starvedEpisodes),
                static_cast<unsigned long long>(captureSessionSummary.longestStarvedEpisodeMs),
                static_cast<unsigned long long>(captureSessionSummary.longestStarvedEpisodeDuplicateTicks),
                captureSessionSummary.longestStarvedEpisodeMinInputFps == std::numeric_limits<uint32_t>::max()
                    ? 0u
                    : captureSessionSummary.longestStarvedEpisodeMinInputFps,
                captureSessionSummary.longestStarvedEpisodeMinDeliveredFps == std::numeric_limits<uint32_t>::max()
                    ? 0u
                    : captureSessionSummary.longestStarvedEpisodeMinDeliveredFps);
            LogInfo(
                "[WGC CFR SUMMARY] SourceFps=%.2f..%.2f MinIn250=%u MinDel250=%u FreshMissMax=%upm JitterMax=%uus "
                "SelMax=%uus WgcSelMax=%uus Oldest=%.1fms ShortfallMax=%.1fms EncEmaMax=%.2fms SustainMin=%.1ffps "
                "LowSrcImmediate=%u StaleDebtDrop=%llu TimelineDebtDrop=%llu LiveRebase=%llu/%u "
                "EncoderDrop=%llu/%u PostStopDrop=%llu/%uus",
                captureSessionSummary.worstSourceFpsX100 == std::numeric_limits<uint32_t>::max()
                    ? 0.0
                    : (captureSessionSummary.worstSourceFpsX100 / 100.0),
                captureSessionSummary.bestSourceFpsX100 / 100.0,
                captureSessionSummary.worstInputMin250Fps == std::numeric_limits<uint32_t>::max()
                    ? 0u
                    : captureSessionSummary.worstInputMin250Fps,
                captureSessionSummary.worstDeliveredMin250Fps == std::numeric_limits<uint32_t>::max()
                    ? 0u
                    : captureSessionSummary.worstDeliveredMin250Fps,
                captureSessionSummary.worstFreshMissPermille, captureSessionSummary.worstSourceJitterUs,
                captureSessionSummary.worstSelectionErrorUs, captureSessionSummary.worstWgcSelectionErrorUs,
                static_cast<double>(captureSessionSummary.worstOldestBufferedFrameAgeUs) / 1000.0,
                captureSessionSummary.maxShortfallDurationMs, captureSessionSummary.maxEncodeEmaMs,
                captureSessionSummary.minEncoderSustainFps == std::numeric_limits<double>::max()
                    ? 0.0
                    : captureSessionSummary.minEncoderSustainFps,
                captureSessionSummary.lowSourceImmediateExits, static_cast<unsigned long long>(wgcDropStaleDebtTotal),
                static_cast<unsigned long long>(wgcVisualDebtMaxExcessTicks),
                static_cast<unsigned long long>(wgcLiveSchedulerRebaseTotal), wgcLiveSchedulerRebaseMaxTicks,
                static_cast<unsigned long long>(wgcEncoderLimitedSourceDropTotal), wgcEncoderLimitedSourceDropMaxTicks,
                static_cast<unsigned long long>(wgcPostStopFrameDropTotal), wgcPostStopFrameDropMaxUs);
            LogInfo(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=%llu maxDropTicks=%u cadenceEvents=%llu "
                "phaseErrorMax=%uus shortfallMax=%.1fms staleDebtDrops=%llu liveRebase=%llu/%u "
                "tooNewRepeats=%u lowSourceBypass=%llu modeMismatch=%llu sourceBacktrack=%llu",
                static_cast<unsigned long long>(wgcEncoderLimitedSourceDropTotal), wgcEncoderLimitedSourceDropMaxTicks,
                static_cast<unsigned long long>(wgcEncoderLimitedCadenceEventCount),
                captureSessionSummary.maxWgcContentPhaseErrorUs, captureSessionSummary.maxShortfallDurationMs,
                static_cast<unsigned long long>(wgcDropStaleDebtTotal),
                static_cast<unsigned long long>(wgcLiveSchedulerRebaseTotal), wgcLiveSchedulerRebaseMaxTicks,
                SaturatingToUint32(wgcRepeatPolicyHoldTotal),
                static_cast<unsigned long long>(wgcEncoderLimitedSuppressedByLowSourceTotal),
                static_cast<unsigned long long>(wgcCapacityPressureModeMismatchTotal),
                static_cast<unsigned long long>(wgcSelectedSourceBacktrackTotal));
        } else {
            LogInfo(
                "[Inject CFR SUMMARY] Live=%llu Dup=%llu DupPct=%.1f%% DupReason(src=%llu def=%llu timer=%llu "
                "drain=%llu) FreshCatchup=%llu RepeatCatchup=%llu StaleTrim=%llu PathMismatch=%llu/%llu "
                "DefRequeued=%llu DefDropped=%llu",
                static_cast<unsigned long long>(liveTicksOutput),
                static_cast<unsigned long long>(captureSessionSummary.duplicateTicks),
                static_cast<double>(duplicatePermille) / 10.0,
                static_cast<unsigned long long>(captureSessionSummary.duplicateNoSourceTicks),
                static_cast<unsigned long long>(captureSessionSummary.duplicateDeferredTicks),
                static_cast<unsigned long long>(captureSessionSummary.duplicateTimerTicks),
                static_cast<unsigned long long>(captureSessionSummary.duplicateDrainTicks),
                static_cast<unsigned long long>(injectFreshCatchupTotal),
                static_cast<unsigned long long>(injectRepeatCatchupTotal),
                static_cast<unsigned long long>(injectLiveStaleTrimTotal),
                static_cast<unsigned long long>(activePathMismatchDiscardTotal),
                static_cast<unsigned long long>(g_ActivePathMismatchFramesDiscarded.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(injectDeferredRequeuedTotal),
                static_cast<unsigned long long>(injectDeferredDroppedTotal));
            LogInfo(
                "[Inject CFR SUMMARY] SourceFps=%.2f..%.2f JitterMax=%uus SelMax=%uus EncEmaMax=%.2fms "
                "SustainMin=%.1ffps",
                injectWorstSourceFpsX100 == std::numeric_limits<uint32_t>::max() ? 0.0
                                                                                 : (injectWorstSourceFpsX100 / 100.0),
                injectBestSourceFpsX100 / 100.0, injectWorstSourceJitterUs, injectWorstSelectionErrorUs,
                captureSessionSummary.maxEncodeEmaMs,
                captureSessionSummary.minEncoderSustainFps == std::numeric_limits<double>::max()
                    ? 0.0
                    : captureSessionSummary.minEncoderSustainFps);
        }
    }

    SetCapturePipelinePhase(CapturePipelinePhase::kIdle);

    LogInfo("[EncoderThread] Stopped");
}

void StartRecording(const AppConfig& config) {
    if (g_Recording)
        return;

    LogInfo("[Media] Starting recording...");

    timeBeginPeriod(1);

    if (g_AudioOnly) {
        LogInfo("[Media] Audio-only recording mode - skipping video capture");

        // Clear any stale shared memory state
        if (g_pSharedMem) {
            StoreRelease(g_pSharedMem->runtimeState.cmdStartRecording, false);
            StoreRelease(g_pSharedMem->runtimeState.cmdStopRecording, false);
            SetRecordingVisibleState(false);
        }

        if (!MediaEngine_StartRecording || !MediaEngine_StartRecording()) {
            LogError("[Media] Failed to start MediaEngine audio-only recording");
            SetRecordingVisibleState(false);
            timeEndPeriod(1);
            g_AudioOnly = false;
            return;
        }

        g_Recording = true;
        SetRecordingVisibleState(true);
        SetCapturePipelinePhase(CapturePipelinePhase::kLive);

        LogInfo("[Media] Audio-only recording active");
        return;
    }

    bool useScreenGrab = IsPreferredScreenGrab();
    if (IsWgcCaptureMethod(config.captureMethod)) {
        useScreenGrab = true;
    } else if (IsInjectCaptureMethod(config.captureMethod)) {
        useScreenGrab = false;
    }
    SetActiveScreenGrab(useScreenGrab);

    if (useScreenGrab && !g_WgcCap) {
        LogError("[Media] WGC capture requested but no WGC target is available");
        SetActiveScreenGrab(false);
        SetCaptureRequestedState(false);
        SetRecordingVisibleState(false);
        timeEndPeriod(1);
        return;
    }

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
    g_ActivePathMismatchFramesDiscarded.store(0, std::memory_order_relaxed);

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
        timeEndPeriod(1);
        return;
    }

    g_Recording = true;
    g_EncoderRunning = true;
    g_RecordingUsesVfr.store(config.video.useVFR, std::memory_order_release);
    g_DrainOutstandingCfrTicks.store(false, std::memory_order_release);
    g_CfrDrainStopQpc.store(0, std::memory_order_release);

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
            timeEndPeriod(1);
            return;
        }
        LogInfo("[Media] Active recording path: WGC bounded pull-drain CFR (%d fps output)", config.video.fps);
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

    // Audio-only: skip all video/capture cleanup
    if (g_AudioOnly) {
        g_Recording = false;
        SetRecordingVisibleState(false);
        SetCapturePipelinePhase(CapturePipelinePhase::kStopping);

        MediaEngine_StopRecording();

        if (g_pSharedMem) {
            ResetRuntimeDiagnostics(g_pSharedMem);
            g_pSharedMem->encoderQueueDepth.store(0, std::memory_order_relaxed);
            g_pSharedMem->throttleCapture.store(false, std::memory_order_release);
        }

        SetActiveScreenGrab(false);
        g_AudioOnly = false;
        timeEndPeriod(1);
        LogInfo("[Media] Audio-only recording stopped");
        return;
    }

    const bool wasActiveScreenGrab = IsActiveScreenGrab();
    const bool recordingUsesVfr = g_RecordingUsesVfr.load(std::memory_order_acquire);
    const bool drainOutstandingCfrTicks =
        ce::capture_policy::ShouldDrainOutstandingCfrTicksAtStop(wasActiveScreenGrab, recordingUsesVfr);
    int64_t drainStopQpc = 0;
    if (drainOutstandingCfrTicks) {
        LARGE_INTEGER stopQpc;
        QueryPerformanceCounter(&stopQpc);
        drainStopQpc = stopQpc.QuadPart;
    }

    g_Recording = false;
    SetCaptureRequestedState(false);
    SetRecordingVisibleState(false);
    SetCapturePipelinePhase(CapturePipelinePhase::kStopping);
    g_CfrDrainStopQpc.store(drainStopQpc, std::memory_order_release);
    g_DrainOutstandingCfrTicks.store(drainOutstandingCfrTicks && drainStopQpc > 0, std::memory_order_release);
    if (wasActiveScreenGrab) {
        g_EncoderRunning = false;
    }
    if (drainOutstandingCfrTicks && drainStopQpc > 0) {
        LogInfo("[Media] CFR stop drain armed at QPC=%lld path=%s", drainStopQpc,
                IsActiveScreenGrab() ? "WGC" : "inject");
    } else if (wasActiveScreenGrab && !recordingUsesVfr) {
        LogInfo("[Media] WGC CFR exact-stop: generic stop drain disabled; live scheduler rebases own debt");
    }

    StopWgcCapturePipeline();
    StopInjectCapturePipeline();
    if (!wasActiveScreenGrab) {
        g_EncoderRunning = false;
    }

    g_InjectDeliveredFirstFrame.store(false, std::memory_order_release);
    g_RejectInjectFrames.store(false, std::memory_order_release);
    g_AutoWgcFallbackArmed.store(false, std::memory_order_release);

    const bool encoderJoined = JoinThreadWithTimeout(g_EncoderThread, 60000, "encoder");

    if (wasActiveScreenGrab && !encoderJoined) {
        LogWarn("[Media] WGC encoder join timed out after exact-stop shutdown");
    }

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

    g_DrainOutstandingCfrTicks.store(false, std::memory_order_release);
    g_CfrDrainStopQpc.store(0, std::memory_order_release);
    g_RecordingUsesVfr.store(false, std::memory_order_release);
    SetActiveScreenGrab(false);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    LogInfo("[Media] Recording stopped");
    timeEndPeriod(1);
}

int MediaProcessMain(const AppConfig& initialConfig) {
    AppConfig config = initialConfig;
    Log_SetLevel(config.logLevel);
    SetConsoleCtrlHandler(MediaConsoleHandler, TRUE);

    // Get exe directory for DLL loading
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string exeDir = std::string(exePath);
    exeDir = exeDir.substr(0, exeDir.find_last_of("\\/"));
    const std::string configPath = GetLocalConfigPath();
    // Directory for the per-device render-endpoint latency cache (next to the config).
    std::string mediaCacheDir = configPath;
    {
        const size_t slash = mediaCacheDir.find_last_of("\\/");
        mediaCacheDir = (slash != std::string::npos) ? mediaCacheDir.substr(0, slash) : std::string();
    }
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
        // When switching to audio-only mode with an already-initialized engine,
        // force reinit so Init() skips the VideoEncoder and creates the audio-only muxer.
        if (mediaEngineReady && g_AudioOnly) {
            LogInfo("[Media] Re-initializing MediaEngine for audio-only mode");
            if (MediaEngine_Shutdown) {
                MediaEngine_Shutdown();
            }
            MediaEngine_Unload();
            mediaEngineReady = false;
            // Fall through to full init below
        }

        if (mediaEngineReady) {
            return true;
        }

        if (!MediaEngine_Load(exeDir.c_str())) {
            LogError("[Media] Failed to load mediaengine.dll");
            return false;
        }

        MediaEngine_SetLogCallback(IsDebugLoggingEnabled(config.logLevel) ? MediaLogCallback : nullptr);
        // Propagate audio-only flag to MediaEngine before Init
        if (g_AudioOnly && MediaEngine_SetAudioOnly) {
            MediaEngine_SetAudioOnly(true);
        }
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

        // Auto-detect the audio-vs-video content offset AFTER Init (the A/V self-calibration needs
        // the shared D3D11 device for WGC frame readback) and only when not recording. Then apply
        // the resolved value to this config AND push it to the media engine, so both the encoder
        // thread (video content delay) and the media engine (per-source equalization) use it.
        if (!g_Recording.load(std::memory_order_acquire)) {
            MeasureRenderLatencyOnce(config, mediaCacheDir);
        }
        if (g_AutoDetectedRenderLatencyMs > 0.0) {
            ApplyAutoDetectedRenderLatencyToConfig(config);
            MediaEngine_ReloadConfig(&config);
        }
        return true;
    };

    ApplyMediaProcessPriority(config);

    ProcessIPCServer ipc(ProcessMode::Media);
    if (!ipc.Init()) {
        LogError("[Media] Failed to initialize IPC");
        return 1;
    }

    if (!ensureMediaEngineReady()) {
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

    auto isExplicitInjectConfig = [&]() -> bool { return IsInjectCaptureMethod(config.captureMethod); };
    auto isExplicitWgcConfig = [&]() -> bool { return IsWgcCaptureMethod(config.captureMethod); };
    auto isAutoCaptureConfig = [&]() -> bool { return IsAutoCaptureMethod(config.captureMethod); };
    auto setWgcPreferenceAfterFailure = [&]() {
        SetPreferredScreenGrab(isExplicitWgcConfig() || isAutoCaptureConfig());
    };
    auto isInjectCaptureTarget = [&](const std::string& processName) -> bool {
        const bool gameWhitelistMatched =
            !processName.empty() && MatchesProcessEntries(config.gameWhitelist, processName);
        return ce::capture_policy::ShouldUseInjectCaptureForAutoTarget(isExplicitInjectConfig(), isAutoCaptureConfig(),
                                                                       gameWhitelistMatched);
    };
    auto resolveSourceProcessName = [&](uint32_t sourcePid, const std::string& knownName = std::string{}) {
        if (!knownName.empty() && knownName != "unknown") {
            return knownName;
        }
        if (sourcePid == 0) {
            return std::string{};
        }
        std::string resolvedName = GetProcessNameFromPID(sourcePid);
        return resolvedName == "unknown" ? std::string{} : resolvedName;
    };
    auto isInjectCaptureTargetForSource = [&](uint32_t sourcePid, const std::string& knownName = std::string{}) {
        return isInjectCaptureTarget(resolveSourceProcessName(sourcePid, knownName));
    };

    if (isExplicitWgcConfig()) {
        SetPreferredScreenGrab(true);
        LogInfo("[Media] Using WGC mode (explicit)");
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

        if (isExplicitWgcConfig()) {
            SetPreferredScreenGrab(true);
            LogInfo("[Media] Connected to shared memory - using WGC for capture");
        } else {
            SetPreferredScreenGrab(false);
            LogInfo("[Media] Connected to shared memory - using inject mode");
        }
    } else if (isExplicitInjectConfig()) {
        LogError("[Media] Failed to connect to shared memory in inject mode!");
        unloadMediaEngineIdle();
        return 1;
    } else {
        SetPreferredScreenGrab(true);
        LogInfo("[Media] Shared memory not available - using WGC mode");
    }

    auto applyWgcOptions = [&]() {
        if (!g_WgcCap) {
            return;
        }
        g_WgcCap->SetSkipSplitDeviceFlush(config.wgcSkipSplitDeviceFlush);
        g_WgcCap->SetSameDeviceCapture(config.wgcSameDeviceCapture);
    };

    if (IsPreferredScreenGrab() || isAutoCaptureConfig()) {
        if (!ensureMediaEngineReady()) {
            return 1;
        }
        d3dDevice = MediaEngine_GetD3D11Device();
        if (!d3dDevice) {
            if (IsPreferredScreenGrab()) {
                LogError("[Media] Failed to get D3D11 device");
                unloadMediaEngineIdle();
                return 1;
            }
        } else {
            d3dDevice->GetImmediateContext(&d3dContext);

            if (WGCCapture::IsSupported()) {
                g_WgcCap = std::make_unique<WGCCapture>();
                applyWgcOptions();
                if (g_WgcCap->Init(d3dDevice)) {
                    // Connect encoder bottleneck flag to WGC for throttle
                    g_WgcCap->SetThrottleFlag(nullptr);
                    LogInfo("[Media] WGC support initialized%s",
                            IsPreferredScreenGrab() ? "" : " (standby for auto fallback)");
                } else {
                    if (IsPreferredScreenGrab()) {
                        LogError("[Media] WGC capture init failed");
                        unloadMediaEngineIdle();
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
            IsPreferredScreenGrab() ? "WGC" : "inject");
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
            applyWgcOptions();
            g_WgcCap->SetCaptureCursor(config.video.captureCursor);
            g_WgcCap->SetThrottleFlag(nullptr);
            return true;
        }
        if (!ensureWgcDevice()) {
            return false;
        }
        g_WgcCap = std::make_unique<WGCCapture>();
        applyWgcOptions();
        if (!g_WgcCap->Init(d3dDevice)) {
            g_WgcCap.reset();
            return false;
        }
        g_WgcCap->SetCaptureCursor(config.video.captureCursor);
        g_WgcCap->SetThrottleFlag(nullptr);
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
    DWORD lastEarlyWgcScan = 0;
    DWORD lastWindowScanTime = 0;
    HWND currentCapturedWindow = NULL;
    bool currentTargetPrefersInject = false;
    WgcRetargetRequest pendingWgcRetarget;
    uint32_t lastSourcePid = 0;
    uint32_t activeConfigSourcePid = 0;
    std::string activeConfigProcessName;

    auto clearCurrentWgcTarget = [&]() {
        currentCapturedWindow = NULL;
        currentTargetPrefersInject = false;
        pendingWgcRetarget = {};
    };

    auto queueWgcRetarget = [&](HWND targetWindow, HMONITOR targetMonitor, bool preferMonitor, const char* reason) {
        pendingWgcRetarget.window = targetWindow;
        pendingWgcRetarget.monitor = targetMonitor;
        pendingWgcRetarget.preferMonitor = preferMonitor || targetWindow == NULL;
        pendingWgcRetarget.active = true;
        LogWarn("[Media] Queued WGC retarget: %s (window=0x%p monitor=0x%p monitorOnly=%d)", reason, targetWindow,
                targetMonitor, pendingWgcRetarget.preferMonitor ? 1 : 0);
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
        // Re-apply the auto-detected render-endpoint latency: LoadConfig reset captureLatencyMs to
        // the config defaults, so the resolved value must be re-stamped before ReloadConfig. This
        // only applies a cached process-local value (no WASAPI / no sound).
        ApplyAutoDetectedRenderLatencyToConfig(config);
        Log_SetLevel(config.logLevel);
        activeConfigSourcePid = sourcePid;
        activeConfigProcessName = processName;

        ApplyMediaProcessPriority(config);
        if (g_WgcCap) {
            applyWgcOptions();
            g_WgcCap->SetCaptureCursor(config.video.captureCursor);
        }
        if (mediaEngineReady) {
            MediaEngine_SetLogCallback(IsDebugLoggingEnabled(config.logLevel) ? MediaLogCallback : nullptr);
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
        if (isExplicitWgcConfig()) {
            return false;
        }

        if (!isInjectCaptureTargetForSource(sourcePid)) {
            return false;
        }

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

    auto primeWgcMonitorTarget = [&](HMONITOR targetMonitor = NULL) -> bool {
        if (isExplicitInjectConfig()) {
            return false;
        }

        if (targetMonitor == NULL && currentCapturedWindow == NULL && !currentTargetPrefersInject && g_WgcCap) {
            applyWgcOptions();
            g_WgcCap->SetCaptureCursor(config.video.captureCursor);
            g_WgcCap->SetThrottleFlag(nullptr);
            SetPreferredScreenGrab(true);
            return true;
        }

        if (!ensureWgcDevice()) {
            return false;
        }

        g_WgcCap.reset();
        g_WgcCap = std::make_unique<WGCCapture>();
        applyWgcOptions();
        bool initOk = false;
        if (targetMonitor) {
            initOk = g_WgcCap->InitForMonitor(d3dDevice, targetMonitor);
            if (!initOk) {
                LogWarn("[Media] Failed to init WGC for monitor 0x%p, falling back to primary", targetMonitor);
            }
        }
        if (!initOk) {
            initOk = g_WgcCap->Init(d3dDevice);
        }
        if (!initOk) {
            g_WgcCap.reset();
            return false;
        }

        g_WgcCap->SetCaptureCursor(config.video.captureCursor);
        g_WgcCap->SetThrottleFlag(nullptr);
        SetPreferredScreenGrab(true);
        currentCapturedWindow = NULL;
        currentTargetPrefersInject = false;
        return true;
    };

    auto primeWgcWindowTarget = [&](HWND targetWindow, bool logPrimed) -> bool {
        if (isExplicitInjectConfig()) {
            return false;
        }

        if (!targetWindow) {
            return false;
        }

        if (currentCapturedWindow == targetWindow && g_WgcCap && !currentTargetPrefersInject) {
            applyWgcOptions();
            g_WgcCap->SetCaptureCursor(config.video.captureCursor);
            g_WgcCap->SetThrottleFlag(nullptr);
            SetPreferredScreenGrab(true);
            return true;
        }

        if (!ensureWgcDevice()) {
            setWgcPreferenceAfterFailure();
            return false;
        }

        g_WgcCap.reset();
        g_WgcCap = std::make_unique<WGCCapture>();
        applyWgcOptions();
        if (g_WgcCap->InitForWindow(d3dDevice, targetWindow)) {
            g_WgcCap->SetCaptureCursor(config.video.captureCursor);
            g_WgcCap->SetThrottleFlag(nullptr);
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
            setWgcPreferenceAfterFailure();
            clearCurrentWgcTarget();
            return false;
        }
        return true;
    };

    auto applyPendingWgcRetarget = [&]() -> bool {
        if (!pendingWgcRetarget.active) {
            return false;
        }

        WgcRetargetRequest request = pendingWgcRetarget;
        pendingWgcRetarget = {};

        const bool restartActiveCapture = g_Recording && IsActiveScreenGrab();
        if (restartActiveCapture) {
            StopWgcCapturePipeline();
        }

        if (!request.preferMonitor && request.window && !IsWindow(request.window)) {
            request.window = NULL;
            request.preferMonitor = true;
        }

        bool primed = false;
        if (!request.preferMonitor && request.window) {
            primed = primeWgcWindowTarget(request.window, true);
        }
        if (!primed) {
            primed = primeWgcMonitorTarget(request.monitor);
        }
        if (!primed) {
            LogWarn("[Media] Failed to apply queued WGC retarget");
            return false;
        }

        if (restartActiveCapture) {
            if (!StartWgcRecordingCapture(config)) {
                LogError("[Media] Failed to restart WGC capture after retarget");
                return false;
            }
            LogInfo("[Media] WGC capture restarted after retarget");
        }
        return true;
    };

    auto prepareCaptureForRecordingStart = [&]() {
        const std::string processName = refreshActiveConfig(false);
        const uint32_t sourcePid = g_pSharedMem ? g_pSharedMem->GetSourcePid() : 0;
        const bool injectWhitelisted = isInjectCaptureTarget(processName);

        g_AutoWgcFallbackArmed.store(false, std::memory_order_release);

        if (isExplicitInjectConfig()) {
            SetPreferredScreenGrab(false);
            clearCurrentWgcTarget();
            return;
        }

        if (injectWhitelisted) {
            SetPreferredScreenGrab(false);
            clearCurrentWgcTarget();
            LogInfo("[Media] Injection whitelist matched %s; auto mode will use inject capture", processName.c_str());
            return;
        }

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
                setWgcPreferenceAfterFailure();
                clearCurrentWgcTarget();
                return;
            }

            SetPreferredScreenGrab(true);
            HWND hGameWindow = GetMainWindowForProcess(sourcePid);
            if (hGameWindow) {
                LogInfo("[Media] Overlay-only hook target %s; WGC capture selected", processName.c_str());
                primeWgcWindowTarget(hGameWindow, false);
            } else if (!primeWgcMonitorTarget()) {
                setWgcPreferenceAfterFailure();
                clearCurrentWgcTarget();
            }
            return;
        }

        if (isExplicitWgcConfig()) {
            if (!primeWgcMonitorTarget()) {
                setWgcPreferenceAfterFailure();
                clearCurrentWgcTarget();
            }
            return;
        }

        if (currentCapturedWindow != NULL && !IsWindow(currentCapturedWindow)) {
            clearCurrentWgcTarget();
        }

        if (isAutoCaptureConfig()) {
            if (sourcePid != 0) {
                HWND hGameWindow = GetMainWindowForProcess(sourcePid);
                if (hGameWindow && primeWgcWindowTarget(hGameWindow, false)) {
                    LogInfo("[Media] Auto mode: %s is not on the inject whitelist; WGC window capture selected",
                            processName.empty() ? "target" : processName.c_str());
                    return;
                }
            }

            if (primeWgcMonitorTarget()) {
                LogInfo("[Media] Auto mode: no inject whitelist match; WGC monitor capture selected");
            } else {
                setWgcPreferenceAfterFailure();
                clearCurrentWgcTarget();
                LogWarn("[Media] Auto mode: WGC target unavailable and inject capture is not allowed for this target");
            }
            return;
        }

        SetPreferredScreenGrab(false);
    };

    while (g_Running) {
        // WGC window detection must run BEFORE resolution polling/texture creation.
        // CreateSharedCaptureTextures sets the encoder's LUID device, which conflicts
        // with WGC's shared device. By scanning first, the preferred capture mode is set correctly
        // and we skip the LUID-based texture creation for WGC games.
        if (mediaEngineReady && g_pSharedMem && isAutoCaptureConfig() && !config.wgcWindowTitles.empty() &&
            !IsPreferredScreenGrab() && !g_Recording) {
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
        char cmdPayload[256] = {};
        if (ipc.PollCommand(cmd, cmdPayload, sizeof(cmdPayload))) {
            switch (cmd) {
                case ProcessCommand::Shutdown:
                    LogInfo("[Media] Shutdown command received");
                    g_Running = false;
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
                case ProcessCommand::StartRecording:
                    g_AudioOnly = (strcmp(cmdPayload, "audio_only") == 0);
                    if (!ensureMediaEngineReady()) {
                        LogError("[Media] Failed to reinitialize MediaEngine for recording start");
                        ipc.SendResponse(ProcessResponse::Ack);
                        break;
                    }
                    prepareCaptureForRecordingStart();
                    StartRecording(config);
                    g_AudioOnly = false;  // Reset after StartRecording consumed it
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
            if (g_WgcCap && g_Recording && IsActiveScreenGrab() && g_WgcCap->NeedsReset()) {
                HWND currentWindow = NULL;
                HMONITOR currentMonitor = NULL;
                g_WgcCap->GetTargetIdentity(&currentWindow, &currentMonitor);
                const std::string resetReason = g_WgcCap->ConsumeResetReason();
                queueWgcRetarget(currentWindow, currentMonitor, currentWindow == NULL,
                                 resetReason.empty() ? "runtime reset requested" : resetReason.c_str());
            }

            if (g_WgcCap && g_Recording && IsActiveScreenGrab() && g_WgcCap->IsWindowTarget() &&
                !g_WgcCap->IsTargetWindowValid()) {
                HWND currentWindow = NULL;
                HMONITOR currentMonitor = NULL;
                g_WgcCap->GetTargetIdentity(&currentWindow, &currentMonitor);
                queueWgcRetarget(NULL, currentMonitor, true, "target window invalid during recording");
            }

            if (pendingWgcRetarget.active && g_Recording && IsActiveScreenGrab()) {
                applyPendingWgcRetarget();
            }

            if (LoadAcquire(g_pSharedMem->runtimeState.cmdStartRecording)) {
                StoreRelease(g_pSharedMem->runtimeState.cmdStartRecording, false);
                if (!g_Recording) {
                    // Check audioOnly flag from shared memory (set by controller for audio-only via inject)
                    g_AudioOnly = LoadAcquire(g_pSharedMem->runtimeState.audioOnly);
                    // Always clear after consuming to prevent stale carry-over
                    StoreRelease(g_pSharedMem->runtimeState.audioOnly, false);
                    if (!ensureMediaEngineReady()) {
                        LogError("[Media] Failed to reinitialize MediaEngine for shared-memory recording start");
                    } else {
                        prepareCaptureForRecordingStart();
                        StartRecording(config);
                        g_AudioOnly = false;
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
            if (!g_Recording && !isExplicitInjectConfig() && !config.wgcWindowTitles.empty() &&
                (now - lastWindowScanTime > 1000)) {
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
                            setWgcPreferenceAfterFailure();
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

                const bool injectWhitelisted = isInjectCaptureTarget(procName);
                const bool forceWGC = !isExplicitInjectConfig() && !injectWhitelisted &&
                                      MatchesProcessEntries(config.overlayWhitelist, procName);
                if (forceWGC) {
                    LogInfo("[Media] Overlay-only hook target %s connected; WGC capture remains selected",
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
                        primeWgcWindowTarget(matchedWindow, false);
                        continue;
                    }

                    if (!ensureWgcDevice()) {
                        LogWarn("[Media] Overlay whitelist requested WGC but D3D11 device unavailable");
                        setWgcPreferenceAfterFailure();
                        clearCurrentWgcTarget();
                    } else {
                        SetPreferredScreenGrab(true);

                        HWND hGameWindow = GetMainWindowForProcess(currentSourcePid);
                        if (hGameWindow) {
                            LogInfo(
                                "[Media] Overlay-only target: found main window 0x%p. "
                                "Switching WGC to window mode.",
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
                                setWgcPreferenceAfterFailure();
                            }
                        }
                    }

                } else if (!g_Recording && injectWhitelisted) {
                    SetPreferredScreenGrab(false);
                    clearCurrentWgcTarget();
                    LogInfo("[Media] Injection whitelist matched %s; using inject capture", procName.c_str());
                } else if (!g_Recording && isAutoCaptureConfig()) {
                    HWND hGameWindow = GetMainWindowForProcess(currentSourcePid);
                    if (hGameWindow && primeWgcWindowTarget(hGameWindow, false)) {
                        LogInfo("[Media] Auto mode: %s is not on the inject whitelist; WGC window capture selected",
                                procName.c_str());
                    } else if (primeWgcMonitorTarget()) {
                        LogInfo("[Media] Auto mode: %s is not on the inject whitelist; WGC monitor capture selected",
                                procName.c_str());
                    } else {
                        setWgcPreferenceAfterFailure();
                        clearCurrentWgcTarget();
                        LogWarn("[Media] Auto mode: WGC target unavailable for %s; no inject capture fallback",
                                procName.c_str());
                    }
                } else if (!g_Recording && !isExplicitWgcConfig()) {
                    SetPreferredScreenGrab(false);
                    LogInfo("[Media] Using Inject Mode (explicit/default)");
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

            if (!receivedFirstFrame && isAutoCaptureConfig() &&
                g_AutoWgcFallbackArmed.load(std::memory_order_acquire) && g_WgcCap) {
                DWORD elapsed = GetTickCount() - injectModeStartTime;
                const uint32_t activeSourcePid = g_pSharedMem ? g_pSharedMem->GetSourcePid() : 0;
                if (ce::capture_policy::ShouldTriggerAutoWgcFallback(
                        receivedFirstFrame, isAutoCaptureConfig(),
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
                            "[Media] Active recording path switched to WGC bounded pull-drain CFR "
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

    LogInfo("[Media] Process exiting");
    return 0;
}
