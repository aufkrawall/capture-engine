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
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../common/atomic_shared_owner.h"
#include "../common/capture_handoff_state.h"
#include "../common/capture_pipeline_policy.h"
#include "../common/config.h"
#include "../common/cursor_capture_state.h"
#include "../common/frame_queue.h"
#include "../common/frame_timing_utils.h"
#include "../common/logging.h"
#include "../common/process_ipc.h"
#include "../common/rate_window_utils.h"
#include "../common/secure_dll_loading.h"
#include "../common/shared_defs.h"
#include "../common/thread_power_throttling_compat.h"
#include "mediaengine_loader.h"
#include "wgc_capture.h"
#include "windows_gpu_scheduling.h"

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
static std::mutex g_StandbyWgcFrameMutex;
static QueuedFrame g_StandbyWgcFrame;
static bool g_HasStandbyWgcFrame = false;
static std::atomic<bool> g_RetainStandbyWgcFrameForHandoff{false};
static std::thread g_EncoderThread;
static QueuedFrame g_LastFrame;
static bool g_HasLastFrame = false;
static std::atomic<uint64_t> g_InjectDeferredFrames{0};

// Screengrab mode components
static ce::AtomicSharedOwner<WGCCapture> g_WgcCap;
static std::atomic<uint64_t> g_WgcSourceEpoch{0};
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
static HANDLE g_InjectFrameReadyEvent = NULL;
static HANDLE g_InjectCaptureShutdownEvent = NULL;

// WGC thread specific
static std::atomic<bool> g_WgcCaptureRunning{false};
static std::atomic<bool> g_WgcCaptureShutdown{false};
static std::thread g_WgcCaptureThread;
static std::atomic<bool> g_InjectDeliveredFirstFrame{false};
static std::atomic<bool> g_RejectInjectFrames{false};
static std::atomic<bool> g_AutoWgcFallbackArmed{false};
static std::atomic<uint32_t> g_InjectBufferedTrimmedFrames{0};
static std::atomic<uint32_t> g_InjectCadenceDroppedFrames{0};
static std::atomic<uint32_t> g_WgcProducerTargetFps{0};
static std::atomic<uint64_t> g_ActivePathMismatchFramesDiscarded{0};
static ce::cursor::Timeline g_WgcCursorTimeline(1024);
static ce::cursor::Timeline g_InjectCursorTimeline(1024);

static UINT GetCursorDpiAtPoint(POINT point) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    using GetDpiForSystemFn = UINT(WINAPI*)();
    static const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    static const auto getDpiForWindow =
        reinterpret_cast<GetDpiForWindowFn>(user32 ? GetProcAddress(user32, "GetDpiForWindow") : nullptr);
    static const auto getDpiForSystem =
        reinterpret_cast<GetDpiForSystemFn>(user32 ? GetProcAddress(user32, "GetDpiForSystem") : nullptr);

    static thread_local HMONITOR cachedMonitor = nullptr;
    static thread_local UINT cachedDpi = 0;
    const HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    if (monitor && monitor == cachedMonitor && cachedDpi != 0) {
        return cachedDpi;
    }

    if (getDpiForWindow) {
        const HWND pointWindow = WindowFromPoint(point);
        if (pointWindow) {
            const UINT dpi = getDpiForWindow(pointWindow);
            if (dpi != 0) {
                cachedMonitor = monitor;
                cachedDpi = dpi;
                return cachedDpi;
            }
        }
    }
    if (getDpiForSystem) {
        const UINT dpi = getDpiForSystem();
        if (dpi != 0) {
            cachedMonitor = monitor;
            cachedDpi = dpi;
            return cachedDpi;
        }
    }
    return 96;
}

static int GetCursorMetricForDpi(int metric, UINT dpi) {
    using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
    static const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    static const auto getSystemMetricsForDpi =
        reinterpret_cast<GetSystemMetricsForDpiFn>(user32 ? GetProcAddress(user32, "GetSystemMetricsForDpi") : nullptr);
    return getSystemMetricsForDpi ? getSystemMetricsForDpi(metric, dpi) : GetSystemMetrics(metric);
}

static ce::cursor::CaptureState CaptureCursorSnapshot(int64_t associationQpc, int32_t captureLeft, int32_t captureTop,
                                                      uint32_t captureWidth, uint32_t captureHeight, bool suppressed) {
    ce::cursor::CaptureState state;
    state.associationQpc = associationQpc;
    state.captureLeft = captureLeft;
    state.captureTop = captureTop;
    state.captureWidth = captureWidth;
    state.captureHeight = captureHeight;

    LARGE_INTEGER observedQpc;
    QueryPerformanceCounter(&observedQpc);
    state.observedQpc = observedQpc.QuadPart;

    CURSORINFO cursorInfo = {sizeof(CURSORINFO)};
    if (!GetCursorInfo(&cursorInfo)) {
        return state;
    }

    state.flags = ce::cursor::kStateValid;
    state.handle = reinterpret_cast<uint64_t>(cursorInfo.hCursor);
    state.screenX = cursorInfo.ptScreenPos.x;
    state.screenY = cursorInfo.ptScreenPos.y;
    if (suppressed || (cursorInfo.flags & CURSOR_SUPPRESSED) != 0) {
        state.flags |= ce::cursor::kStateSuppressed;
    } else if ((cursorInfo.flags & CURSOR_SHOWING) != 0) {
        state.flags |= ce::cursor::kStateVisible;
    } else if (cursorInfo.hCursor) {
        // DirectFlip / independent-flip cursor planes can retain a valid
        // hardware cursor handle without CURSOR_SHOWING being observable in
        // this process. Preserve the existing compatibility fallback, but
        // record it so diagnostics can distinguish it from normal visibility.
        state.flags |= ce::cursor::kStateVisible | ce::cursor::kStateHandleVisibilityFallback;
    }

    state.dpi = GetCursorDpiAtPoint(cursorInfo.ptScreenPos);
    static thread_local UINT cachedMetricDpi = 0;
    static thread_local uint32_t cachedCursorWidth = 0;
    static thread_local uint32_t cachedCursorHeight = 0;
    if (state.dpi != cachedMetricDpi || cachedCursorWidth == 0 || cachedCursorHeight == 0) {
        cachedMetricDpi = state.dpi;
        cachedCursorWidth = static_cast<uint32_t>(std::max(1, GetCursorMetricForDpi(SM_CXCURSOR, state.dpi)));
        cachedCursorHeight = static_cast<uint32_t>(std::max(1, GetCursorMetricForDpi(SM_CYCURSOR, state.dpi)));
    }
    state.requestedWidth = cachedCursorWidth;
    state.requestedHeight = cachedCursorHeight;
    return state;
}

static uint64_t AdvanceWgcSourceEpoch(const char* reason) {
    const uint64_t epoch = g_WgcSourceEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    LogInfo("[Media] Advanced WGC source epoch to %llu (%s)", static_cast<unsigned long long>(epoch),
            reason ? reason : "unspecified");
    return epoch;
}

static void PublishWgcCapture(std::shared_ptr<WGCCapture> replacement, const char* reason) {
    const uint64_t epoch = AdvanceWgcSourceEpoch(reason);
    if (replacement) {
        // Bind the epoch before publication/start. A callback from the retired
        // source keeps its old identity even if it finishes after this global
        // coordinator epoch changes.
        replacement->SetSourceEpoch(epoch);
    }
    auto retired = g_WgcCap.Exchange(std::move(replacement));
    if (retired) {
        // Exchange holds the lifecycle writer gate until all reader expressions
        // finish. Releasing here keeps WinRT/COM teardown on the control thread
        // without retaining potentially large stopped texture pools all session.
        retired.reset();
    }
    LogInfo("[Media] Published WGC source epoch %llu", static_cast<unsigned long long>(epoch));
}

struct WgcRuntimeLogSnapshot {
    std::atomic<bool> hasPoolEvidence{false};
    std::atomic<uint32_t> sourceFramePoolBuffers{0};
    std::atomic<uint32_t> copyPoolSlots{0};
    std::atomic<uint32_t> budgetSurfaces{0};
    std::atomic<uint32_t> syncFrames{0};
    std::atomic<uint32_t> extraFrames{0};
    std::atomic<uint32_t> retainedCap{0};
    std::atomic<uint32_t> reservedFreeSlots{0};
    std::atomic<uint32_t> safetySlots{0};
    std::atomic<uint32_t> sourceFormat{0};
    std::atomic<uint32_t> retainedFormat{0};
    std::atomic<uint32_t> compactRetained{0};
    std::atomic<uint64_t> estimatedVramBytes{0};
    std::atomic<uint64_t> sourceBudgetBytes{0};
    std::atomic<uint64_t> copyBudgetBytes{0};
    std::atomic<uint64_t> sourceSurfaceBytes{0};
    std::atomic<uint64_t> copySurfaceBytes{0};
    std::atomic<int64_t> lastConvertUs{0};
    std::atomic<uint32_t> poolLeasedMax{0};
    std::atomic<uint32_t> poolFreeMin{UINT32_MAX};
    std::atomic<uint32_t> poolSaturatedDrops{0};
    std::atomic<uint32_t> poolOverwritePrevented{0};
    std::atomic<uint32_t> poolLeaseMismatches{0};
    std::atomic<uint32_t> duplicateTimestampsSeen{0};
    std::atomic<uint32_t> duplicateTimestampsSkipped{0};

    void Reset() {
        hasPoolEvidence.store(false, std::memory_order_relaxed);
        sourceFramePoolBuffers.store(0, std::memory_order_relaxed);
        copyPoolSlots.store(0, std::memory_order_relaxed);
        budgetSurfaces.store(0, std::memory_order_relaxed);
        syncFrames.store(0, std::memory_order_relaxed);
        extraFrames.store(0, std::memory_order_relaxed);
        retainedCap.store(0, std::memory_order_relaxed);
        reservedFreeSlots.store(0, std::memory_order_relaxed);
        safetySlots.store(0, std::memory_order_relaxed);
        sourceFormat.store(0, std::memory_order_relaxed);
        retainedFormat.store(0, std::memory_order_relaxed);
        compactRetained.store(0, std::memory_order_relaxed);
        estimatedVramBytes.store(0, std::memory_order_relaxed);
        sourceBudgetBytes.store(0, std::memory_order_relaxed);
        copyBudgetBytes.store(0, std::memory_order_relaxed);
        sourceSurfaceBytes.store(0, std::memory_order_relaxed);
        copySurfaceBytes.store(0, std::memory_order_relaxed);
        lastConvertUs.store(0, std::memory_order_relaxed);
        poolLeasedMax.store(0, std::memory_order_relaxed);
        poolFreeMin.store(UINT32_MAX, std::memory_order_relaxed);
        poolSaturatedDrops.store(0, std::memory_order_relaxed);
        poolOverwritePrevented.store(0, std::memory_order_relaxed);
        poolLeaseMismatches.store(0, std::memory_order_relaxed);
        duplicateTimestampsSeen.store(0, std::memory_order_relaxed);
        duplicateTimestampsSkipped.store(0, std::memory_order_relaxed);
    }
};

static WgcRuntimeLogSnapshot g_WgcRuntimeLogSnapshot;

static void AtomicMax(std::atomic<uint32_t>& target, uint32_t value) {
    uint32_t current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

static void AtomicMin(std::atomic<uint32_t>& target, uint32_t value) {
    uint32_t current = target.load(std::memory_order_relaxed);
    while (value < current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

static void SnapshotWgcRuntimeLogState(const WGCCapture* cap) {
    if (!cap) {
        return;
    }

    AtomicMax(g_WgcRuntimeLogSnapshot.duplicateTimestampsSeen, cap->GetNormalizedDuplicateTimestampCount());
    AtomicMax(g_WgcRuntimeLogSnapshot.duplicateTimestampsSkipped, cap->GetDuplicateTimestampSkipCount());

    const uint32_t leasedMax = cap->GetPoolSlotLeasedMaxCount();
    const uint32_t freeMin = cap->GetPoolSlotFreeMinCount();
    if (leasedMax == 0 && freeMin == 0) {
        return;
    }

    g_WgcRuntimeLogSnapshot.sourceFramePoolBuffers.store(cap->GetSourceFramePoolBufferCount(),
                                                         std::memory_order_relaxed);
    g_WgcRuntimeLogSnapshot.copyPoolSlots.store(cap->GetTexturePoolSlotCount(), std::memory_order_relaxed);
    g_WgcRuntimeLogSnapshot.budgetSurfaces.store(cap->GetSmoothnessBudgetSurfaceCount(), std::memory_order_relaxed);
    g_WgcRuntimeLogSnapshot.syncFrames.store(cap->GetSmoothnessSyncFrameCount(), std::memory_order_relaxed);
    g_WgcRuntimeLogSnapshot.extraFrames.store(cap->GetSmoothnessRetainedFrameCount(), std::memory_order_relaxed);
    g_WgcRuntimeLogSnapshot.retainedCap.store(cap->GetSmoothnessRetainedFrameCap(), std::memory_order_relaxed);
    g_WgcRuntimeLogSnapshot.reservedFreeSlots.store(cap->GetSmoothnessReservedFreeSlotCount(),
                                                    std::memory_order_relaxed);
    g_WgcRuntimeLogSnapshot.safetySlots.store(cap->GetSmoothnessSafetySlotCount(), std::memory_order_relaxed);
    g_WgcRuntimeLogSnapshot.sourceFormat.store(cap->GetSmoothnessSourceDxgiFormat(), std::memory_order_relaxed);
    g_WgcRuntimeLogSnapshot.retainedFormat.store(cap->GetSmoothnessCopyDxgiFormat(), std::memory_order_relaxed);
    const bool compact = cap->IsCompactRetainedCopyActive();
    g_WgcRuntimeLogSnapshot.compactRetained.store(compact ? 1u : 0u, std::memory_order_relaxed);
    g_WgcRuntimeLogSnapshot.estimatedVramBytes.store(cap->GetSmoothnessEstimatedVramBytes(), std::memory_order_relaxed);
    g_WgcRuntimeLogSnapshot.sourceBudgetBytes.store(cap->GetSmoothnessSourceEstimatedVramBytes(),
                                                    std::memory_order_relaxed);
    g_WgcRuntimeLogSnapshot.copyBudgetBytes.store(cap->GetSmoothnessCopyEstimatedVramBytes(),
                                                  std::memory_order_relaxed);
    g_WgcRuntimeLogSnapshot.sourceSurfaceBytes.store(cap->GetSmoothnessSourceBytesPerSurface(),
                                                     std::memory_order_relaxed);
    g_WgcRuntimeLogSnapshot.copySurfaceBytes.store(cap->GetSmoothnessCopyBytesPerSurface(), std::memory_order_relaxed);
    const int64_t convertUs = cap->GetLastPoolConvertTimeUs();
    if (compact || cap->IsUsingDesktopDuplication()) {
        if (convertUs > 0) {
            g_WgcRuntimeLogSnapshot.lastConvertUs.store(convertUs, std::memory_order_relaxed);
        }
    } else {
        g_WgcRuntimeLogSnapshot.lastConvertUs.store(0, std::memory_order_relaxed);
    }
    AtomicMax(g_WgcRuntimeLogSnapshot.poolLeasedMax, leasedMax);
    AtomicMin(g_WgcRuntimeLogSnapshot.poolFreeMin, freeMin);
    AtomicMax(g_WgcRuntimeLogSnapshot.poolSaturatedDrops, cap->GetPoolSaturatedDropCount());
    AtomicMax(g_WgcRuntimeLogSnapshot.poolOverwritePrevented, cap->GetPoolSlotOverwritePreventedCount());
    AtomicMax(g_WgcRuntimeLogSnapshot.poolLeaseMismatches, cap->GetPoolLeaseMismatchCount());
    g_WgcRuntimeLogSnapshot.hasPoolEvidence.store(true, std::memory_order_release);
}

static void SnapshotPublishedWgcRuntimeLogState() {
    const auto cap = g_WgcCap.Read();
    SnapshotWgcRuntimeLogState(cap.get());
}

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
// applied to the render-domain audio sources on every config (re)load. The probe cache is
// process-memory only; a fresh process may re-probe, but no endpoint latency file is written.
static double g_AutoDetectedRenderLatencyMs = -1.0;  // <0 = not measured / unavailable
static bool g_RenderLatencyMeasureAttempted = false;
static bool g_LegacyAudioLatencyCacheCleanupAttempted = false;
static std::mutex g_LegacyAudioLatencyCacheCleanupMutex;
static std::string g_AvSyncConfidence = "low";
static std::string g_AvSyncReason = "not_measured";
static bool g_AvSyncUsedAudioProbe = false;

static void StampAvSyncStatus(AppConfig& config, const char* confidence, const char* reason, float resolvedMs,
                              bool usedAudioProbe) {
    config.avSyncConfidence = confidence ? confidence : "low";
    config.avSyncReason = reason ? reason : "unknown";
    config.avSyncResolvedRenderLatencyMs = resolvedMs;
    config.avSyncUsedAudioProbe = usedAudioProbe;
}

// Apply the auto-detected render-endpoint latency to render-domain sources (system loopback + app
// process loopback only). No-op when autodetect is off, a manual override is configured, or no
// value has been measured. Microphones (Domain 2) are never touched here. Cheap and idempotent.
static void ApplyAutoDetectedRenderLatencyToConfig(AppConfig& config) {
    int renderDomainSources = 0;
    int micDomainSources = 0;
    for (const auto& s : config.audioSources) {
        const bool renderDomain = s.sourceType == AudioConfig::SystemAudio || s.sourceType == AudioConfig::AppAudio;
        if (renderDomain) {
            ++renderDomainSources;
        } else if (s.sourceType == AudioConfig::Microphone) {
            ++micDomainSources;
        }
    }

    if (config.audioCaptureLatencyMs > 0.0f) {
        StampAvSyncStatus(config, "medium", "manual_config_override", config.audioCaptureLatencyMs, false);
        LogInfo(
            "[AVSyncAuto] passiveInputs renderSources=%d micSources=%d generalRenderLatencyMs=%.3f "
            "autodetect=%d probe=skipped chosenDelayMs=%.3f confidence=medium reason=manual_config_override",
            renderDomainSources, micDomainSources, static_cast<double>(config.audioCaptureLatencyMs),
            config.audioLatencyAutodetect ? 1 : 0, static_cast<double>(config.audioCaptureLatencyMs));
        return;
    }

    if (!config.audioLatencyAutodetect) {
        StampAvSyncStatus(config, "low", "autodetect_disabled", 0.0f, false);
        LogWarn(
            "[AVSyncAuto] passiveInputs renderSources=%d micSources=%d generalRenderLatencyMs=0.000 "
            "autodetect=0 probe=disabled chosenDelayMs=0.000 confidence=low reason=autodetect_disabled",
            renderDomainSources, micDomainSources);
        return;
    }

    if (g_AutoDetectedRenderLatencyMs <= 0.0) {
        StampAvSyncStatus(config, g_AvSyncConfidence.c_str(), g_AvSyncReason.c_str(), 0.0f, g_AvSyncUsedAudioProbe);
        LogWarn(
            "[AVSyncAuto] passiveInputs renderSources=%d micSources=%d generalRenderLatencyMs=0.000 "
            "autodetect=1 probe=%s chosenDelayMs=0.000 confidence=%s reason=%s",
            renderDomainSources, micDomainSources, g_RenderLatencyMeasureAttempted ? "unavailable" : "not_attempted",
            config.avSyncConfidence.c_str(), config.avSyncReason.c_str());
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
    StampAvSyncStatus(config, g_AvSyncConfidence.c_str(), g_AvSyncReason.c_str(), ms, g_AvSyncUsedAudioProbe);
    LogInfo(
        "[AVSyncAuto] passiveInputs renderSources=%d micSources=%d generalRenderLatencyMs=0.000 autodetect=1 "
        "probe=%s chosenDelayMs=%.3f appliedSources=%d confidence=%s reason=%s domain=render_endpoint",
        renderDomainSources, micDomainSources,
        config.avSyncUsedAudioProbe ? "audio_render_loopback" : "memory_cache_or_manual", static_cast<double>(ms),
        applied, config.avSyncConfidence.c_str(), config.avSyncReason.c_str());
}

static void DeleteLegacyAudioLatencyCacheFileOnce(const std::string& cacheDir) {
    if (cacheDir.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_LegacyAudioLatencyCacheCleanupMutex);
        if (g_LegacyAudioLatencyCacheCleanupAttempted) {
            return;
        }
        g_LegacyAudioLatencyCacheCleanupAttempted = true;
    }

    std::string path = cacheDir;
    if (!path.empty() && path.back() != '\\' && path.back() != '/') {
        path += '\\';
    }
    path += "audio_latency_cache.ini";

    const DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return;
    }

    if (DeleteFileA(path.c_str())) {
        LogInfo("[AVSyncProbe] legacyDiskCache=deleted cacheMode=memory file=audio_latency_cache.ini");
    } else {
        LogInfo("[AVSyncProbe] legacyDiskCache=delete_failed cacheMode=memory file=audio_latency_cache.ini error=%lu",
                static_cast<unsigned long>(GetLastError()));
    }
}

// Perform the one-time product-safe audio-only render->loopback measurement. Call only when NOT
// recording. On a cache miss it may render a near-inaudible marker; it never opens a calibration
// window or emits a video stimulus. Safe to call repeatedly; it runs at most once per process and
// is a cheap memory cache hit otherwise.
static void MeasureRenderLatencyOnce(const AppConfig& config, const std::string& cacheDir) {
    DeleteLegacyAudioLatencyCacheFileOnce(cacheDir);
    if (g_RenderLatencyMeasureAttempted) {
        return;
    }
    if (config.audioCaptureLatencyMs > 0.0f) {
        g_RenderLatencyMeasureAttempted = true;  // disabled or manual override: never measure
        g_AutoDetectedRenderLatencyMs = -1.0;
        g_AvSyncConfidence = "medium";
        g_AvSyncReason = "manual_config_override";
        g_AvSyncUsedAudioProbe = false;
        LogInfo("[AVSyncAuto] probe=skipped confidence=medium reason=manual_config_override configuredDelayMs=%.3f",
                static_cast<double>(config.audioCaptureLatencyMs));
        return;
    }
    if (!config.audioLatencyAutodetect) {
        g_RenderLatencyMeasureAttempted = true;
        g_AutoDetectedRenderLatencyMs = -1.0;
        g_AvSyncConfidence = "low";
        g_AvSyncReason = "autodetect_disabled";
        g_AvSyncUsedAudioProbe = false;
        LogWarn("[AVSyncAuto] probe=disabled confidence=low reason=autodetect_disabled chosenDelayMs=0.000");
        return;
    }
    g_RenderLatencyMeasureAttempted = true;

    // Audio-only render->loopback probe (Start-anchor) - the default active auto-detect.
    double ms = 0.0;
    if (MediaEngine_MeasureRenderEndpointLatency &&
        MediaEngine_MeasureRenderEndpointLatency(cacheDir.c_str(), false, &ms) && ms > 0.0) {
        g_AutoDetectedRenderLatencyMs = ms;
        g_AvSyncConfidence = "high";
        g_AvSyncReason = "audio_probe_render_loopback";
        g_AvSyncUsedAudioProbe = true;
        LogInfo("[AVSyncAuto] probe=audio_render_loopback chosenDelayMs=%.3f confidence=high domain=render_endpoint",
                ms);
    } else {
        g_AutoDetectedRenderLatencyMs = -1.0;
        g_AvSyncConfidence = "low";
        g_AvSyncReason = "probe_unavailable_passive_insufficient";
        g_AvSyncUsedAudioProbe = false;
        LogWarn(
            "[AVSyncAuto] probe=unavailable chosenDelayMs=0.000 confidence=low "
            "reason=probe_unavailable_passive_insufficient");
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

const char* WgcIngressAdmissionReasonName(uint32_t code) {
    switch (code) {
        case 1:
            return "low_water";
        case 2:
            return "recovery";
        case 3:
            return "source_below_cfr_target";
        case 4:
            return "credit";
        case 5:
            return "healthy";
        case 6:
            return "wgc_ingress_decimated_soft_reserve";
        case 7:
            return "wgc_ingress_decimated_hard_reserve";
        case 8:
            return "wgc_ingress_decimated_credit";
        case 9:
            return "uniform_playout_soft_reserve";
        case 10:
            return "uniform_playout_credit";
        case 0:
        default:
            return "uncapped";
    }
}

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
           lhs.bitDepth == rhs.bitDepth && lhs.downmix == rhs.downmix && lhs.captureLatencyMs == rhs.captureLatencyMs;
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
           lhs.profile == rhs.profile && lhs.lookahead == rhs.lookahead && lhs.spatialAq == rhs.spatialAq &&
           lhs.temporalAq == rhs.temporalAq && lhs.aqStrength == rhs.aqStrength && lhs.bFrames == rhs.bFrames &&
           lhs.bRefMode == rhs.bRefMode && lhs.customOptions == rhs.customOptions &&
           lhs.captureCursor == rhs.captureCursor && lhs.qp == rhs.qp && lhs.mfRateControl == rhs.mfRateControl &&
           lhs.mfQuality == rhs.mfQuality && lhs.mfScenario == rhs.mfScenario && lhs.mfHwEncoding == rhs.mfHwEncoding &&
           lhs.gpuPriority == rhs.gpuPriority && lhs.bitDepth == rhs.bitDepth && lhs.colorSpace == rhs.colorSpace &&
           lhs.colorRange == rhs.colorRange && lhs.chromaSubsampling == rhs.chromaSubsampling &&
           lhs.useVFR == rhs.useVFR && lhs.useVFR_AudioSync == rhs.useVFR_AudioSync &&
           MediaScalingConfigEquals(lhs.scaling, rhs.scaling);
}

bool MediaEngineConfigEquals(const AppConfig& lhs, const AppConfig& rhs) {
    if (lhs.logLevel != rhs.logLevel || lhs.captureMethod != rhs.captureMethod ||
        lhs.autoFullscreenPrefersDxgiDup != rhs.autoFullscreenPrefersDxgiDup ||
        lhs.wgcSkipSplitDeviceFlush != rhs.wgcSkipSplitDeviceFlush ||
        lhs.wgcSameDeviceCapture != rhs.wgcSameDeviceCapture ||
        lhs.wgcSmoothnessBufferEnabled != rhs.wgcSmoothnessBufferEnabled ||
        lhs.wgcSmoothnessBufferMaxMs != rhs.wgcSmoothnessBufferMaxMs ||
        lhs.wgcSmoothnessBufferVramBudgetMb != rhs.wgcSmoothnessBufferVramBudgetMb ||
        lhs.wgcVideoMemoryReservation != rhs.wgcVideoMemoryReservation ||
        lhs.wgcAllowLossyBgra8Pool != rhs.wgcAllowLossyBgra8Pool || !MediaVideoConfigEquals(lhs.video, rhs.video) ||
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

    return ce::capture_policy::GetWgcCfrProducerTargetFps(static_cast<uint32_t>(video.fps));
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
    state.injectProducerCaptureLockDrops.store(0, std::memory_order_relaxed);
    state.injectProducerCpuLeaseBusyDrops.store(0, std::memory_order_relaxed);
    state.injectProducerGpuBusyDrops.store(0, std::memory_order_relaxed);
    state.injectProducerMetadataFullDrops.store(0, std::memory_order_relaxed);
    state.injectFrameReadySignals.store(0, std::memory_order_relaxed);
    state.injectPublicationToIngestAvgUs.store(0, std::memory_order_relaxed);
    state.injectPublicationToIngestMaxUs.store(0, std::memory_order_relaxed);
    state.encoderTimerWakeLateAvgUs.store(0, std::memory_order_relaxed);
    state.encoderTimerWakeLateMaxUs.store(0, std::memory_order_relaxed);
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

void SetInjectVideoCaptureRequestedState(bool enabled, const char* reason) {
    if (!g_pSharedMem) {
        return;
    }

    const bool previous = g_pSharedMem->runtimeState.HasRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested);
    g_pSharedMem->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, enabled);
    if (previous != enabled) {
        LogInfo("[Media] Inject video publication %s (%s)", enabled ? "enabled" : "disabled",
                reason ? reason : "unspecified");
    }
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

    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
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
        LogWarn(
            "[Media] Timeout waiting for %s thread (%lu ms); preserving ownership and continuing to wait because "
            "cleanup while the worker is live would race released capture/encoder resources",
            threadName, static_cast<unsigned long>(timeoutMs));
    } else {
        LogWarn(
            "[Media] WaitForSingleObject failed for %s thread (error=%lu); preserving ownership and joining "
            "synchronously",
            threadName, GetLastError());
    }

    thread.join();
    LogInfo("[Media] %s thread eventually joined after the bounded wait", threadName);
    return true;
}

void MediaLogCallback(const char* msg) {
    LogInfo("[Media] %s", msg);
}

static void ReleaseStandaloneWgcQueuedFrame(QueuedFrame& frame) {
    if (!frame.isInjectMode && frame.texture) {
        frame.texture->Release();
        frame.texture = nullptr;
    }
    frame.wgcPoolLease.Reset();
    frame = QueuedFrame{};
}

static void ClearStandbyWgcHandoffFrame() {
    QueuedFrame stale;
    {
        std::lock_guard<std::mutex> lock(g_StandbyWgcFrameMutex);
        if (!g_HasStandbyWgcFrame) {
            return;
        }
        stale = std::move(g_StandbyWgcFrame);
        g_HasStandbyWgcFrame = false;
    }
    ReleaseStandaloneWgcQueuedFrame(stale);
}

static bool HasStandbyWgcHandoffFrame() {
    std::lock_guard<std::mutex> lock(g_StandbyWgcFrameMutex);
    return g_HasStandbyWgcFrame;
}

static bool StoreStandbyWgcHandoffFrame(QueuedFrame&& frame) {
    QueuedFrame stale;
    {
        std::lock_guard<std::mutex> lock(g_StandbyWgcFrameMutex);
        // Recheck while holding the slot lock. A callback can observe the
        // retention flag immediately before the handoff thread disarms it; in
        // that race it must not repopulate the slot after the handoff has taken
        // the proven frame.
        if (!g_RetainStandbyWgcFrameForHandoff.load(std::memory_order_acquire)) {
            return false;
        }
        if (g_HasStandbyWgcFrame) {
            stale = std::move(g_StandbyWgcFrame);
        }
        g_StandbyWgcFrame = std::move(frame);
        g_HasStandbyWgcFrame = true;
    }
    ReleaseStandaloneWgcQueuedFrame(stale);
    return true;
}

static bool TakeStandbyWgcHandoffFrame(QueuedFrame& frame) {
    std::lock_guard<std::mutex> lock(g_StandbyWgcFrameMutex);
    if (!g_HasStandbyWgcFrame) {
        return false;
    }
    frame = std::move(g_StandbyWgcFrame);
    g_HasStandbyWgcFrame = false;
    return true;
}

static void SubmitWgcQueuedFrame(QueuedFrame&& frame) {
    static std::atomic<int64_t> s_lastWgcTimestamp{0};
    if (g_pSharedMem) {
        const int64_t comparisonTimestamp = frame.rawTimestamp > 0 ? frame.rawTimestamp : frame.timestamp;
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

    ID3D11Texture2D* texture = frame.texture;
    if (!g_FrameQueue.Push(std::move(frame)) && texture) {
        texture->Release();
    }
}

static void QueueWgcFrame(ID3D11Texture2D* texture, uint32_t width, uint32_t height, int64_t timestamp,
                          int64_t rawTimestamp, bool isHDR, bool cursorEmbedded, bool duplicateSourceTimestamp,
                          const ce::cursor::SourcePointerObservation& cursorObservation, int32_t captureLeft,
                          int32_t captureTop, uint64_t sourceEpoch, WgcPoolSlotLease&& poolLease) {
    const uint64_t activeEpoch = g_WgcSourceEpoch.load(std::memory_order_acquire);
    if (sourceEpoch != activeEpoch) {
        static std::atomic<uint64_t> s_staleEpochDrops{0};
        const uint64_t discarded = s_staleEpochDrops.fetch_add(1, std::memory_order_relaxed) + 1;
        if (discarded <= 3 || (discarded % 120ull) == 0ull) {
            LogInfo("[WGC] Dropping retired-source callback frame: frameEpoch=%llu activeEpoch=%llu discarded=%llu",
                    static_cast<unsigned long long>(sourceEpoch), static_cast<unsigned long long>(activeEpoch),
                    static_cast<unsigned long long>(discarded));
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
    qf.wgcPoolSlot = poolLease.Slot();
    qf.wgcPoolGeneration = poolLease.Generation();
    qf.wgcSourceEpoch = sourceEpoch;
    qf.wgcPoolLease = std::move(poolLease);
    LARGE_INTEGER enqueueQpc;
    QueryPerformanceCounter(&enqueueQpc);
    qf.enqueueQpc = enqueueQpc.QuadPart;
    qf.isHDR = isHDR;
    qf.wgcCursorEmbedded = cursorEmbedded;
    qf.captureLeft = captureLeft;
    qf.captureTop = captureTop;
    qf.cursorState = CaptureCursorSnapshot(timestamp, captureLeft, captureTop, width, height, cursorEmbedded);
    ce::cursor::ApplySourcePointerObservation(&qf.cursorState, cursorObservation);
    g_WgcCursorTimeline.Publish(qf.cursorState);

    if (g_Recording.load(std::memory_order_acquire) && !IsActiveScreenGrab()) {
        if (g_RetainStandbyWgcFrameForHandoff.load(std::memory_order_acquire) &&
            StoreStandbyWgcHandoffFrame(std::move(qf))) {
            return;
        }
        const uint64_t discarded = g_ActivePathMismatchFramesDiscarded.fetch_add(1, std::memory_order_relaxed) + 1;
        if (discarded <= 3 || (discarded % 120ull) == 0ull) {
            LogInfo(
                "[WGC] Dropping standby WGC frame while inject capture is active (discarded=%llu, ts=%lld). This "
                "prevents mid-recording encoder mode switches.",
                static_cast<unsigned long long>(discarded), static_cast<long long>(timestamp));
        }
        ReleaseStandaloneWgcQueuedFrame(qf);
        return;
    }

    SubmitWgcQueuedFrame(std::move(qf));
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
    qf.wgcPoolSlot = frame.poolSlot;
    qf.wgcPoolGeneration = frame.poolGeneration;
    qf.wgcSourceEpoch = frame.sourceEpoch;
    qf.wgcPoolLease = std::move(frame.poolLease);
    LARGE_INTEGER enqueueQpc;
    QueryPerformanceCounter(&enqueueQpc);
    qf.enqueueQpc = enqueueQpc.QuadPart;
    qf.isHDR = frame.isHDR;
    qf.wgcCursorEmbedded = frame.cursorEmbedded;
    qf.duplicateSourceTimestamp = frame.duplicateSourceTimestamp;
    qf.captureLeft = frame.captureLeft;
    qf.captureTop = frame.captureTop;
    qf.cursorState = CaptureCursorSnapshot(frame.timestamp, frame.captureLeft, frame.captureTop, frame.width,
                                           frame.height, frame.cursorEmbedded);
    ce::cursor::ApplySourcePointerObservation(&qf.cursorState, frame.cursorObservation);
    g_WgcCursorTimeline.Publish(qf.cursorState);
    return qf;
}

static void ReleaseWgcCapturedFrame(WGCCapturedFrame& frame) {
    if (frame.texture) {
        frame.texture->Release();
        frame.texture = nullptr;
    }
    frame.poolLease.Reset();
    frame.poolSlot = std::numeric_limits<uint32_t>::max();
    frame.poolGeneration = 0;
}

static void ResetInjectFrameRingToLatest(const char* reason) {
    if (!g_pSharedMem) {
        return;
    }

    FrameRingBuffer& ring = g_pSharedMem->frameRing;
    uint32_t readIndex = ring.readIndex.load(std::memory_order_acquire);
    uint32_t writeIndex = ring.writeIndex.load(std::memory_order_acquire);
    if (!IsFrameRingWindowValid(writeIndex, readIndex)) {
        LogError("[Media] Refusing corrupt inject frame ring reset before %s (write=%u read=%u distance=%u)",
                 reason ? reason : "unknown transition", writeIndex, readIndex,
                 static_cast<uint32_t>(writeIndex - readIndex));
        return;
    }
    if (readIndex == writeIndex) {
        return;
    }

    ring.readIndex.store(writeIndex, std::memory_order_release);
    ring.ingestIndex.store(writeIndex, std::memory_order_release);
    LogInfo("[Media] Discarded %u stale inject frame(s) before %s", static_cast<unsigned>(writeIndex - readIndex),
            reason);
}

static void ResetLastQueuedFrameCache() {
    if (g_HasLastFrame && !g_LastFrame.isInjectMode && g_LastFrame.texture) {
        g_LastFrame.texture->Release();
    }
    if (g_HasLastFrame && !g_LastFrame.isInjectMode) {
        g_LastFrame.wgcPoolLease.Reset();
    }
    g_LastFrame = QueuedFrame{};
    g_HasLastFrame = false;
}

static bool EnsureInjectCaptureEvents() {
    if (!g_InjectCaptureShutdownEvent) {
        g_InjectCaptureShutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_InjectCaptureShutdownEvent) {
            LogWarn("[Inject Thread] Failed to create shutdown event (err=%lu)", GetLastError());
        }
    }
    if (!g_InjectFrameReadyEvent && g_pSharedMem && g_pSharedMem->GetHostPID() != 0) {
        wchar_t eventName[64]{};
        GenerateInjectFrameReadyEventName(eventName, _countof(eventName), g_pSharedMem->GetHostPID());
        g_InjectFrameReadyEvent = CreateEventW(nullptr, FALSE, FALSE, eventName);
        if (!g_InjectFrameReadyEvent) {
            LogWarn("[Inject Thread] Failed to create frame-ready event '%ls' (err=%lu)", eventName, GetLastError());
        } else {
            LogInfo("[Inject Thread] Frame-ready event initialized: %ls", eventName);
        }
    }
    return g_InjectFrameReadyEvent && g_InjectCaptureShutdownEvent;
}

static void StopInjectCapturePipeline() {
    g_InjectCaptureShutdown = true;
    if (g_InjectCaptureShutdownEvent) {
        SetEvent(g_InjectCaptureShutdownEvent);
    }
    JoinThreadWithTimeout(g_InjectCaptureThread, 5000, "inject capture");
    ResetInjectFrameRingToLatest("inject pipeline stop");
}

// Duplication embedded-cursor suppression: while duplicated frames already
// CONTAIN the cursor (software/composed cursor reported by the dup pointer
// metadata), encoder-side cursor composition must be suppressed to avoid a
// double cursor. Polled cheaply on the encoder thread per submitted frame;
// the state only changes on hardware/software cursor-plane transitions.
static std::atomic<bool> g_DupCursorSuppressionActive{false};

static void SyncDuplicationCursorSuppression(bool suppress) {
    if (suppress == g_DupCursorSuppressionActive.load(std::memory_order_relaxed)) {
        return;
    }
    g_DupCursorSuppressionActive.store(suppress, std::memory_order_relaxed);
    if (MediaEngine_SetCursorCompositionSuppressed) {
        MediaEngine_SetCursorCompositionSuppressed(suppress);
    }
    LogInfo("[Media] Encoder cursor composition %s (duplication frames %s the cursor)",
            suppress ? "suppressed" : "active", suppress ? "already contain" : "do not contain");
}

static void ResetDuplicationCursorSuppression(const char* reason) {
    const bool wasSuppressed = g_DupCursorSuppressionActive.exchange(false, std::memory_order_acq_rel);
    if (MediaEngine_SetCursorCompositionSuppressed) {
        // Always publish the reset. Merely clearing the local cache can leave
        // the encoder latched in suppression across a reset/retarget.
        MediaEngine_SetCursorCompositionSuppressed(false);
    }
    if (wasSuppressed) {
        LogInfo("[Media] Encoder cursor composition restored (%s)", reason ? reason : "capture transition");
    }
}

static void StopWgcCapturePipeline() {
    g_RetainStandbyWgcFrameForHandoff.store(false, std::memory_order_release);
    ClearStandbyWgcHandoffFrame();
    ResetDuplicationCursorSuppression("WGC pipeline stop");
    g_WgcCaptureShutdown = true;
    g_WgcProducerTargetFps.store(0, std::memory_order_relaxed);
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

    // Block encoder-side readers while the capture session and its WinRT/DXGI
    // resources are torn down. Atomic shared ownership alone protects object
    // lifetime; this access gate also protects mutable session internals.
    auto capture = g_WgcCap.LockExclusive();
    if (capture) {
        capture->SetDirectFrameCallback(nullptr);
        capture->SetTargetFps(0);
        if (capture->IsCapturing()) {
            capture->StopCapture();
        }
    }
}

static bool StartWgcRecordingCapture(const AppConfig& config) {
    g_WgcRuntimeLogSnapshot.Reset();

    if (g_WgcCaptureThread.joinable()) {
        LogWarn("[Media] Cleaning up stale WGC capture thread before restart");
        g_WgcCaptureShutdown = true;
        JoinThreadWithTimeout(g_WgcCaptureThread, 5000, "WGC capture");
    }

    auto captureAccess = g_WgcCap.LockExclusive();
    WGCCapture* capture = captureAccess.get();
    if (!capture) {
        return false;
    }

    if (capture->IsCapturing()) {
        capture->SetDirectFrameCallback(nullptr);
        capture->StopCapture();
    }

    capture->SetCaptureCursor(ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
    if (config.video.captureCursor) {
        LogInfo("[Media] WGC cursor capture: native WGC cursor disabled; encoder-side cursor composition enabled");
    }
    HWND activeWgcWindow = NULL;
    HMONITOR activeWgcMonitor = NULL;
    capture->GetTargetIdentity(&activeWgcWindow, &activeWgcMonitor);
    int32_t activeCaptureLeft = 0;
    int32_t activeCaptureTop = 0;
    const bool haveCaptureOrigin = capture->GetCaptureOrigin(activeCaptureLeft, activeCaptureTop);
    LogInfo(
        "[Media] WGC recording target: target=%s backend=%s hwnd=0x%p hmon=0x%p originOk=%d origin=(%d,%d) "
        "captureCursor=%d nativeWgcCursor=%d encoderCursor=%d",
        activeWgcWindow ? "window" : "monitor", capture->IsUsingDesktopDuplication() ? "DxgiDuplication" : "WGC",
        activeWgcWindow, activeWgcMonitor, haveCaptureOrigin ? 1 : 0, activeCaptureLeft, activeCaptureTop,
        config.video.captureCursor ? 1 : 0,
        ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor) ? 1 : 0,
        config.video.captureCursor ? 1 : 0);
    capture->SetSkipSplitDeviceFlush(config.wgcSkipSplitDeviceFlush);
    capture->SetSameDeviceCapture(config.wgcSameDeviceCapture);
    capture->SetAllowLossyBgra8Pool(config.wgcAllowLossyBgra8Pool);
    const bool explicitTenBit = IsExplicitTenBitVideo(config.video);
    capture->SetRequireHighPrecisionCapture(explicitTenBit);
    capture->SetAllowDuplicationFallback(ce::capture_policy::ShouldAllowWgcFallbackAfterDxgiFailure(
        IsDxgiDupCaptureMethod(config.captureMethod), explicitTenBit));
    const uint32_t initialWgcTargetFps = GetInitialWgcCfrTargetFps(config.video);
    float maxAudioCaptureLatencyMs = 0.0f;
    for (const auto& audioSrc : config.audioSources) {
        if (audioSrc.captureLatencyMs > maxAudioCaptureLatencyMs) {
            maxAudioCaptureLatencyMs = audioSrc.captureLatencyMs;
        }
    }
    const uint32_t outputFps = static_cast<uint32_t>(std::max(0, config.video.fps));
    const bool hasWgcContentDelayBudget = maxAudioCaptureLatencyMs > 0.0f;
    // Smoothness FLOOR: when configured (auto or explicit > 0) the reservoir/copy-pool budget must
    // be allocated even with no audio-latency content delay, otherwise a video-only / low-confidence
    // capture would have no buffer to engage the active-delay jitter-absorbing playout. The floor
    // delay itself is realized within the retained-extra reservoir (not the sync-delay frames), so
    // syncDelayFramesForBudget stays audio-latency-driven (0 here when there is no audio latency).
    const bool wgcSmoothnessFloorBudgetDesired = config.wgcSmoothnessBufferEnabled && !config.video.useVFR &&
                                                 (config.wgcSmoothnessFloorAuto || config.wgcSmoothnessFloorMs > 0);
    const uint32_t syncDelayFramesForBudget =
        hasWgcContentDelayBudget ? ce::capture_policy::GetWgcEstimatedSyncDelayFramesForBudget(
                                       outputFps, static_cast<uint32_t>(std::ceil(maxAudioCaptureLatencyMs)))
                                 : 0u;
    capture->SetSmoothnessBufferBudget(config.wgcSmoothnessBufferEnabled && !config.video.useVFR &&
                                           (hasWgcContentDelayBudget || wgcSmoothnessFloorBudgetDesired),
                                       outputFps, config.wgcSmoothnessBufferMaxMs,
                                       config.wgcSmoothnessBufferVramBudgetMb, syncDelayFramesForBudget);
    capture->SetVideoMemoryReservationMode(config.wgcVideoMemoryReservation);
    if (config.video.useVFR) {
        capture->SetDirectFrameCallback(QueueWgcFrame);
    } else {
        capture->SetDirectFrameCallback(nullptr);
    }
    capture->ResetStats();
    // Explicitly reset both the cache and the encoder-side state. A prior
    // duplication session may have ended while its software cursor was embedded.
    ResetDuplicationCursorSuppression("WGC recording start");
    g_IsEncoderBottlenecked.store(false, std::memory_order_relaxed);
    g_WgcProducerTargetFps.store(initialWgcTargetFps, std::memory_order_relaxed);
    // A finite WGC MinUpdateInterval aliases variable-rate sources and can turn
    // 138 fps into about 69 fps. CFR therefore receives every compositor update
    // and leaves surplus-frame selection to the timestamp scheduler. DXGI
    // duplication has no producer interval; it shares the same zero target so
    // the screen-grab contract is backend-independent.
    capture->SetTargetFps(0);
    capture->SetProducerTargetFps(initialWgcTargetFps);
    LogInfo(
        "[WGC CFR] Producer contract: backend=%s outputFps=%u producerTargetFps=%u minUpdateInterval100ns=0 "
        "policy=max-rate-variable-input localThrottleFps=0",
        capture->IsUsingDesktopDuplication() ? "dxgi_dup" : "wgc", outputFps, initialWgcTargetFps);
    // Persist before StartCapture so any device rebuild and the first WGC/DXGI
    // submissions inherit the configured relative GPU priority.
    capture->SetGpuPriority(config.video.gpuPriority);

    // For CFR recording, disable the encoder-bottleneck throttle at the WGC
    // callback level.  The throttle is all-or-nothing (bang-bang) and its slow
    // EMA causes boom-bust oscillation that starves the Bresenham credit
    // accumulator, producing irregular frame-hold patterns (visible judder).
    // The encoder thread's buffer cap + Bresenham skip already provide smooth
    // backpressure, so the throttle is both unnecessary and harmful for CFR.
    if (!config.video.useVFR) {
        capture->SetThrottleFlag(nullptr);
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
    if (!capture->StartCapture()) {
        capture->SetDirectFrameCallback(nullptr);
        return false;
    }
    SnapshotWgcRuntimeLogState(capture);

    // Tell the encoder whether the capture source runs at >8 bpc so that
    // bit_depth=auto resolves to 10-bit even when the WGC frame pool fell
    // back to BGRA8 (e.g. R10G10B10A2 pool creation failed).
    if (MediaEngine_SetSourcePrefers10Bit) {
        const bool hiPrec = capture->IsHighPrecisionSource();
        LogInfo("[Media] WGC source high-precision=%s, notifying encoder", hiPrec ? "YES" : "NO");
        MediaEngine_SetSourcePrefers10Bit(hiPrec);
    } else {
        LogWarn("[Media] MediaEngine_SetSourcePrefers10Bit not available (old mediaengine.dll?)");
    }

    g_WgcCaptureShutdown = false;
    // Recording-lifetime config snapshot: the main thread reassigns `config`
    // on late hook connects and IPC config reloads (refreshActiveConfig),
    // which would be a use-after-free race against a by-reference reader on
    // this thread. Recording settings must not change live mid-session anyway.
    {
        auto configSnapshot = std::make_shared<const AppConfig>(config);
        g_WgcCaptureThread = std::thread([configSnapshot]() { WgcCaptureThreadFunc(*configSnapshot); });
    }
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

struct ForegroundWgcWindowCandidate {
    HWND hwnd = NULL;
    DWORD pid = 0;
    std::string processName;
    bool usable = false;
    bool fullscreenLike = false;
};

static std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

static bool IsIgnoredForegroundWgcClass(HWND hwnd) {
    char className[128] = {};
    if (GetClassNameA(hwnd, className, static_cast<int>(sizeof(className))) <= 0) {
        return false;
    }

    const std::string lowerClass = ToLowerAscii(className);
    return lowerClass == "progman" || lowerClass == "workerw" || lowerClass == "shell_traywnd";
}

static bool IsIgnoredForegroundWgcProcess(const std::string& processName) {
    const std::string lowerName = ToLowerAscii(processName);
    return lowerName.empty() || lowerName == "unknown" || lowerName == "explorer.exe" ||
           lowerName == "applicationframehost.exe" || lowerName == "shellexperiencehost.exe" ||
           lowerName == "searchhost.exe" || lowerName == "startmenuexperiencehost.exe" ||
           lowerName == "textinputhost.exe" || lowerName == "captureengine.exe";
}

static ForegroundWgcWindowCandidate GetForegroundWgcWindowCandidate() {
    ForegroundWgcWindowCandidate candidate;
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) {
        return candidate;
    }

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root) {
        hwnd = root;
    }

    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd) || hwnd == GetDesktopWindow() ||
        GetWindow(hwnd, GW_OWNER) != 0) {
        return candidate;
    }

    const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if ((style & WS_CHILD) || (exStyle & WS_EX_TOOLWINDOW) || IsIgnoredForegroundWgcClass(hwnd)) {
        return candidate;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) {
        return candidate;
    }

    std::string processName = GetProcessNameFromPID(pid);
    if (IsIgnoredForegroundWgcProcess(processName)) {
        return candidate;
    }

    candidate.hwnd = hwnd;
    candidate.pid = pid;
    candidate.processName = processName;
    candidate.usable = true;
    candidate.fullscreenLike = IsWindowFullscreenLike(hwnd);
    return candidate;
}

static bool MatchesProcessEntry(const WhitelistEntry& entry, const std::string& lowerProcessName) {
    if (!entry.HasProcess() || lowerProcessName.empty()) {
        return false;
    }

    std::string lowerItem = entry.pattern;
    std::transform(lowerItem.begin(), lowerItem.end(), lowerItem.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

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
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    for (const auto& entry : entries) {
        if (MatchesProcessEntry(entry, lowerName)) {
            return true;
        }
    }

    return false;
}

static int64_t RectArea(const RECT& rect) {
    const int64_t width = std::max<LONG>(0, rect.right - rect.left);
    const int64_t height = std::max<LONG>(0, rect.bottom - rect.top);
    return width * height;
}

static HWND FindMatchingWgcWindow(const std::vector<WhitelistEntry>& targets) {
    struct WgcSearchContext {
        const std::vector<WhitelistEntry>* targets;
        HWND result;
        HWND foregroundRoot;
        int checked;
        int matched;
        int bestScore;
    };

    HWND foregroundRoot = GetForegroundWindow();
    if (foregroundRoot) {
        HWND root = GetAncestor(foregroundRoot, GA_ROOT);
        if (root) {
            foregroundRoot = root;
        }
    }

    WgcSearchContext ctx = {&targets, NULL, foregroundRoot, 0, 0, std::numeric_limits<int>::min()};
    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            WgcSearchContext* context = (WgcSearchContext*)lParam;
            if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
                return TRUE;
            }
            if (GetWindow(hwnd, GW_OWNER) != 0) {
                return TRUE;
            }
            const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
            const LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            if ((style & WS_CHILD) || (exStyle & WS_EX_TOOLWINDOW)) {
                return TRUE;
            }

            context->checked++;

            char title[256];
            GetWindowTextA(hwnd, title, sizeof(title));
            std::string titleStr = title;
            std::transform(titleStr.begin(), titleStr.end(), titleStr.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

            char className[256];
            GetClassNameA(hwnd, className, sizeof(className));
            std::string classStr = className;
            std::transform(classStr.begin(), classStr.end(), classStr.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

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
                        std::transform(procName.begin(), procName.end(), procName.begin(),
                                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    }
                    CloseHandle(hProcess);
                }
            }

            for (const auto& entry : *context->targets) {
                MatchMode mode = entry.mode;
                bool matched = false;
                bool matchedByTitleOrClass = false;
                bool matchedByProcess = false;

                if (entry.HasWindow()) {
                    std::string winLower = entry.windowName;
                    std::transform(winLower.begin(), winLower.end(), winLower.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

                    if (mode == MatchMode::kExact) {
                        matched = !titleStr.empty() && titleStr == winLower;
                    } else {
                        matched = !titleStr.empty() && titleStr.find(winLower) != std::string::npos;
                        if (!matched && mode == MatchMode::kTitleType && !classStr.empty()) {
                            matched = classStr.find(winLower) != std::string::npos;
                        }
                    }
                    matchedByTitleOrClass = matched;
                }

                if (!matched && MatchesProcessEntry(entry, procName)) {
                    matched = true;
                    matchedByProcess = true;
                }

                if (matched) {
                    RECT windowRect = {};
                    RECT clientRect = {};
                    const bool haveWindowRect = GetWindowRect(hwnd, &windowRect) != FALSE;
                    const bool haveClientRect = GetWindowClientRectInScreen(hwnd, clientRect);
                    const int64_t area =
                        std::max(haveWindowRect ? RectArea(windowRect) : 0, haveClientRect ? RectArea(clientRect) : 0);
                    int score = 1000;
                    if (context->foregroundRoot && hwnd == context->foregroundRoot) {
                        score += 100000;
                    }
                    if (IsWindowFullscreenLike(hwnd)) {
                        score += 50000;
                    }
                    if (matchedByTitleOrClass) {
                        score += 5000;
                    }
                    if (matchedByProcess) {
                        score += 2000;
                    }
                    score += static_cast<int>(std::min<int64_t>(area / 1000, 40000));

                    ++context->matched;
                    if (!context->result || score > context->bestScore) {
                        context->result = hwnd;
                        context->bestScore = score;
                    }
                    break;
                }
            }
            return TRUE;
        },
        (LPARAM)&ctx);

    if (ctx.result) {
        DWORD pid = 0;
        GetWindowThreadProcessId(ctx.result, &pid);
        LogDebug(
            "[Media] WGC window detection selected hwnd=0x%p pid=%lu fullscreenLike=%d score=%d "
            "(matched=%d checked=%d foreground=%d)",
            ctx.result, static_cast<unsigned long>(pid), IsWindowFullscreenLike(ctx.result) ? 1 : 0, ctx.bestScore,
            ctx.matched, ctx.checked, (ctx.foregroundRoot && ctx.result == ctx.foregroundRoot) ? 1 : 0);
    }

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
    ScopedMmcssTask(const wchar_t* taskName, AVRT_PRIORITY priority, const char* role) : role_(role) {
        DWORD taskIndex = 0;
        handle_ = AvSetMmThreadCharacteristicsW(taskName, &taskIndex);
        if (handle_) {
            if (!AvSetMmThreadPriority(handle_, priority)) {
                LogWarn("[%s] AvSetMmThreadPriority failed (tid=%lu err=%lu)", role_, GetCurrentThreadId(),
                        GetLastError());
            } else {
                LogInfo("[%s] Thread QoS enabled (tid=%lu task=%ls priority=%d)", role_, GetCurrentThreadId(), taskName,
                        static_cast<int>(priority));
            }
        } else {
            const DWORD mmcssError = GetLastError();
            if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
                LogWarn("[%s] MMCSS and THREAD_PRIORITY_HIGHEST setup failed (tid=%lu mmcssErr=%lu priorityErr=%lu)",
                        role_, GetCurrentThreadId(), mmcssError, GetLastError());
            } else {
                LogWarn("[%s] MMCSS setup failed; using THREAD_PRIORITY_HIGHEST (tid=%lu err=%lu)", role_,
                        GetCurrentThreadId(), mmcssError);
            }
        }
    }

    ~ScopedMmcssTask() {
        if (handle_) {
            if (!AvRevertMmThreadCharacteristics(handle_)) {
                LogWarn("[%s] AvRevertMmThreadCharacteristics failed (tid=%lu err=%lu)", role_, GetCurrentThreadId(),
                        GetLastError());
            }
        }
    }

    ScopedMmcssTask(const ScopedMmcssTask&) = delete;
    ScopedMmcssTask& operator=(const ScopedMmcssTask&) = delete;

private:
    HANDLE handle_ = nullptr;
    const char* role_ = "Thread";
};

static void DisableCurrentThreadPowerThrottling(const char* role) {
    THREAD_POWER_THROTTLING_STATE throttlingState = {};
    throttlingState.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttlingState.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttlingState.StateMask = 0;
    if (!SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttlingState, sizeof(throttlingState))) {
        LogWarn("[%s] Failed to disable execution-speed power throttling (tid=%lu err=%lu)", role, GetCurrentThreadId(),
                GetLastError());
    }
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

static const char* Win32PriorityClassName(DWORD priorityClass) {
    switch (priorityClass) {
        case IDLE_PRIORITY_CLASS:
            return "idle";
        case BELOW_NORMAL_PRIORITY_CLASS:
            return "below_normal";
        case NORMAL_PRIORITY_CLASS:
            return "normal";
        case ABOVE_NORMAL_PRIORITY_CLASS:
            return "above_normal";
        case HIGH_PRIORITY_CLASS:
            return "high";
        case REALTIME_PRIORITY_CLASS:
            return "realtime";
        default:
            return "unknown";
    }
}

static bool IsCurrentProcessElevatedForPriorityLog() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation = {};
    DWORD returned = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
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
    else if (config.processPriority == "realtime")
        priorityClass = REALTIME_PRIORITY_CLASS;

    const DWORD currentClass = GetPriorityClass(GetCurrentProcess());
    if (currentClass == priorityClass) {
        return;
    }

    if (SetPriorityClass(GetCurrentProcess(), priorityClass)) {
        LogInfo("[Media] CPU process priority set to %s", Win32PriorityClassName(priorityClass));
    } else {
        LogWarn("[Media] Failed to set CPU process priority to %s: gle=%lu", Win32PriorityClassName(priorityClass),
                GetLastError());
    }
}

static const char* D3dkmtSchedulingPriorityClassName(int priorityClass) {
    switch (priorityClass) {
        case 0:
            return "idle";
        case 1:
            return "below_normal";
        case 2:
            return "normal";
        case 3:
            return "above_normal";
        case 4:
            return "high";
        case 5:
            return "realtime";
        default:
            return "unknown";
    }
}

static bool ResolveD3dkmtSchedulingPriorityClass(const std::string& value, int& priorityClass) {
    if (value == "idle") {
        priorityClass = 0;
    } else if (value == "below_normal") {
        priorityClass = 1;
    } else if (value == "normal") {
        priorityClass = 2;
    } else if (value == "above_normal") {
        priorityClass = 3;
    } else if (value == "high") {
        priorityClass = 4;
    } else if (value == "realtime") {
        priorityClass = 5;
    } else {
        return false;
    }
    return true;
}

static void ApplyMediaGpuSchedulingPriority(const AppConfig& config, const LUID* adapterLuid = nullptr) {
    using D3dkmtSetProcessSchedulingPriorityClassFn = LONG(WINAPI*)(HANDLE, int);
    using D3dkmtGetProcessSchedulingPriorityClassFn = LONG(WINAPI*)(HANDLE, int*);

    static std::mutex s_priorityMutex;
    std::lock_guard<std::mutex> priorityLock(s_priorityMutex);
    static bool s_loggedDisabled = false;
    static bool s_loggedAutoDeferred = false;
    static bool s_appliedNonDefault = false;
    static std::string s_lastRequest;
    static LUID s_lastEnvironmentLuid{};
    static bool s_haveLastEnvironmentLuid = false;

    const bool disabled = config.gpuSchedulingPriority == "off";
    const bool automatic = config.gpuSchedulingPriority == "auto";
    int requestedClass = 2;
    if (disabled) {
        if (!s_appliedNonDefault) {
            if (!s_loggedDisabled) {
                LogInfo("[Media] GPU scheduling priority class disabled (gpu_scheduling_priority=off)");
                s_loggedDisabled = true;
            }
            return;
        }
    } else if (automatic) {
        if (!adapterLuid) {
            if (!s_loggedAutoDeferred) {
                LogInfo("[Media] GPU scheduling priority auto deferred until the capture adapter LUID is known");
                s_loggedAutoDeferred = true;
            }
            return;
        }

        ce::windows_gpu_scheduling::AdapterSchedulingEnvironment environment{};
        const bool queried = ce::windows_gpu_scheduling::QueryAdapterSchedulingEnvironment(*adapterLuid, environment);
        requestedClass = ce::gpu_scheduling::ResolveAutomaticProcessSchedulingPriority(environment.hags);
        const bool newEnvironment =
            !s_haveLastEnvironmentLuid || !ce::windows_gpu_scheduling::SameLuid(s_lastEnvironmentLuid, *adapterLuid);
        if (newEnvironment) {
            LogInfo(
                "[Media] GPU scheduling environment: adapter=%ls luid=%s vendor=0x%04X device=0x%04X "
                "driver=0x%016llX windowsBuild=%u hagsQuery=%d hagsEnabled=%d hagsDefault=%d hagsSupported=%d "
                "hagsSupport=%s open=0x%08lX caps27=0x%08lX caps29=0x%08lX close=0x%08lX autoClass=%s",
                environment.description.empty() ? L"unknown" : environment.description.c_str(),
                ce::windows_gpu_scheduling::FormatLuid(*adapterLuid).c_str(), environment.vendorId,
                environment.deviceId, static_cast<unsigned long long>(environment.driverVersion),
                environment.windowsBuild, queried ? 1 : 0, environment.hags.enabled ? 1 : 0,
                environment.hags.enabledByDefault ? 1 : 0, environment.hags.supported ? 1 : 0,
                ce::gpu_scheduling::HagsSupportStateName(environment.hags.supportState),
                static_cast<unsigned long>(environment.openStatus),
                static_cast<unsigned long>(environment.caps27Status),
                static_cast<unsigned long>(environment.caps29Status),
                static_cast<unsigned long>(environment.closeStatus), D3dkmtSchedulingPriorityClassName(requestedClass));
            s_lastEnvironmentLuid = *adapterLuid;
            s_haveLastEnvironmentLuid = true;
        }
        s_loggedAutoDeferred = false;
    } else if (!ResolveD3dkmtSchedulingPriorityClass(config.gpuSchedulingPriority, requestedClass)) {
        LogWarn("[Media] Ignoring invalid GPU scheduling priority class '%s'", config.gpuSchedulingPriority.c_str());
        return;
    }

    HMODULE gdi32 = GetModuleHandleA("gdi32.dll");
    if (!gdi32) {
        gdi32 = ce::security::LoadSystemLibrary(L"gdi32.dll");
    }
    if (!gdi32) {
        LogWarn("[Media] GPU scheduling priority class unavailable: failed to load gdi32.dll");
        return;
    }

    auto setPriority = reinterpret_cast<D3dkmtSetProcessSchedulingPriorityClassFn>(
        GetProcAddress(gdi32, "D3DKMTSetProcessSchedulingPriorityClass"));
    auto getPriority = reinterpret_cast<D3dkmtGetProcessSchedulingPriorityClassFn>(
        GetProcAddress(gdi32, "D3DKMTGetProcessSchedulingPriorityClass"));
    if (!setPriority) {
        LogWarn("[Media] GPU scheduling priority class unavailable: D3DKMTSetProcessSchedulingPriorityClass missing");
        return;
    }

    int currentClass = -1;
    LONG getStatus = getPriority ? 0 : static_cast<LONG>(ERROR_PROC_NOT_FOUND);
    const bool haveCurrent = getPriority && ((getStatus = getPriority(GetCurrentProcess(), &currentClass)) >= 0);
    std::string automaticRequest;
    const char* requestText = nullptr;
    if (disabled) {
        requestText = "off(reset_to_normal)";
    } else if (automatic) {
        automaticRequest = std::string("auto(") + D3dkmtSchedulingPriorityClassName(requestedClass) + ")@" +
                           ce::windows_gpu_scheduling::FormatLuid(*adapterLuid);
        requestText = automaticRequest.c_str();
    } else {
        requestText = config.gpuSchedulingPriority.c_str();
    }
    if (haveCurrent && currentClass == requestedClass && s_lastRequest == requestText) {
        return;
    }

    const LONG status = setPriority(GetCurrentProcess(), requestedClass);
    const bool elevated = IsCurrentProcessElevatedForPriorityLog();
    if (status < 0) {
        if (haveCurrent) {
            LogWarn(
                "[Media] Failed to set GPU scheduling priority class to %s (config=%s current=%s elevated=%d): "
                "ntstatus=0x%08lX",
                D3dkmtSchedulingPriorityClassName(requestedClass), requestText,
                D3dkmtSchedulingPriorityClassName(currentClass), elevated ? 1 : 0, (unsigned long)status);
        } else {
            LogWarn(
                "[Media] Failed to set GPU scheduling priority class to %s (config=%s current=unknown "
                "getStatus=0x%08lX elevated=%d): ntstatus=0x%08lX",
                D3dkmtSchedulingPriorityClassName(requestedClass), requestText, (unsigned long)getStatus,
                elevated ? 1 : 0, (unsigned long)status);
        }
        return;
    }

    int verifiedClass = -1;
    LONG verifyStatus = getPriority ? 0 : static_cast<LONG>(ERROR_PROC_NOT_FOUND);
    const bool haveVerified = getPriority && ((verifyStatus = getPriority(GetCurrentProcess(), &verifiedClass)) >= 0);
    const bool verified = haveVerified && verifiedClass == requestedClass;
    s_lastRequest = requestText;
    s_loggedDisabled = disabled;
    s_appliedNonDefault = requestedClass != 2;
    if (verified) {
        LogInfo(
            "[Media] GPU scheduling priority class set to %s (config=%s previous=%s current=%s verified=1 "
            "elevated=%d ntstatus=0x%08lX)",
            D3dkmtSchedulingPriorityClassName(requestedClass), requestText,
            haveCurrent ? D3dkmtSchedulingPriorityClassName(currentClass) : "unknown",
            D3dkmtSchedulingPriorityClassName(verifiedClass), elevated ? 1 : 0, (unsigned long)status);
    } else if (haveVerified) {
        LogWarn(
            "[Media] GPU scheduling priority class set call returned success but readback mismatch (config=%s "
            "requested=%s previous=%s current=%s verified=0 elevated=%d ntstatus=0x%08lX)",
            requestText, D3dkmtSchedulingPriorityClassName(requestedClass),
            haveCurrent ? D3dkmtSchedulingPriorityClassName(currentClass) : "unknown",
            D3dkmtSchedulingPriorityClassName(verifiedClass), elevated ? 1 : 0, (unsigned long)status);
    } else {
        LogInfo(
            "[Media] GPU scheduling priority class set call returned success but readback unavailable (config=%s "
            "requested=%s previous=%s getStatus=0x%08lX verifyStatus=0x%08lX verified=unknown elevated=%d "
            "ntstatus=0x%08lX)",
            requestText, D3dkmtSchedulingPriorityClassName(requestedClass),
            haveCurrent ? D3dkmtSchedulingPriorityClassName(currentClass) : "unknown", (unsigned long)getStatus,
            (unsigned long)verifyStatus, elevated ? 1 : 0, (unsigned long)status);
    }
}

static void ApplyMediaPrioritySettings(const AppConfig& config) {
    ApplyMediaProcessPriority(config);
    ApplyMediaGpuSchedulingPriority(config);
}

static bool ApplyMediaGpuSchedulingPriorityForDevice(const AppConfig& config, ID3D11Device* device) {
    LUID luid{};
    if (!ce::windows_gpu_scheduling::GetAdapterLuid(device, luid)) {
        LogWarn("[Media] Could not resolve D3D11 adapter LUID for GPU scheduling priority");
        return false;
    }
    ApplyMediaGpuSchedulingPriority(config, &luid);
    return true;
}

static bool ApplyMediaGpuSchedulingPriorityForSharedAdapter(const AppConfig& config) {
    if (!g_pSharedMem) {
        return false;
    }
    LUID luid{};
    luid.LowPart = g_pSharedMem->GetLuidLowPart();
    luid.HighPart = static_cast<LONG>(g_pSharedMem->GetLuidHighPart());
    if (luid.LowPart == 0 && luid.HighPart == 0) {
        return false;
    }
    ApplyMediaGpuSchedulingPriority(config, &luid);
    return true;
}

// =================================================================================================
// THREAD FUNCTIONS
// =================================================================================================

void InjectCaptureThreadFunc(const AppConfig& config) {
    LogInfo("[Inject Thread] Started (event-driven ingest with adaptive source-side pacing)");
    g_InjectCaptureRunning = true;
    DisableCurrentThreadPowerThrottling("Inject Thread");
    ScopedMmcssTask injectMmcssTask(L"Capture", AVRT_PRIORITY_HIGH, "Inject Thread");

    if (!g_pSharedMem) {
        LogError("[Inject Thread] Shared memory not available! Aborting.");
        g_InjectCaptureRunning = false;
        return;
    }

    LUID lastSchedulingLuid{};
    bool haveSchedulingLuid = false;
    const auto applySchedulingForCurrentAdapter = [&]() {
        LUID current{};
        current.LowPart = g_pSharedMem->GetLuidLowPart();
        current.HighPart = static_cast<LONG>(g_pSharedMem->GetLuidHighPart());
        if (current.LowPart == 0 && current.HighPart == 0) {
            return;
        }
        if (!haveSchedulingLuid || !ce::windows_gpu_scheduling::SameLuid(lastSchedulingLuid, current)) {
            ApplyMediaGpuSchedulingPriority(config, &current);
            lastSchedulingLuid = current;
            haveSchedulingLuid = true;
        }
    };
    applySchedulingForCurrentAdapter();

    // Local read index tracks what WE have pushed to the FrameQueue
    uint32_t localReadIndex = g_pSharedMem->frameRing.readIndex.load(std::memory_order_acquire);
    g_pSharedMem->frameRing.ingestIndex.store(localReadIndex, std::memory_order_release);
    auto advanceIngestIndex = [&]() {
        ++localReadIndex;
        g_pSharedMem->frameRing.ingestIndex.store(localReadIndex, std::memory_order_release);
    };
    std::shared_ptr<ce::InjectFrameRingLeaseState> injectRingLeaseState;
    try {
        injectRingLeaseState = std::make_shared<ce::InjectFrameRingLeaseState>(&g_pSharedMem->frameRing);
    } catch (const std::exception& error) {
        LogError("[Inject Thread] Failed to allocate frame-ring ownership state: %s", error.what());
        g_InjectCaptureRunning = false;
        return;
    }

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
    uint32_t lastDuplicateCount = 0;
    uint32_t lastLateCount = 0;
    uint32_t lastTrimmedCount = g_InjectBufferedTrimmedFrames.load(std::memory_order_relaxed);
    uint32_t lastCadenceDroppedCount = g_InjectCadenceDroppedFrames.load(std::memory_order_relaxed);
    uint32_t lastDeferredCount = g_InjectDeferredFrames.load(std::memory_order_relaxed);
    bool earlyTexturesCreated = false;
    bool sharedTexturesCreated = false;
    uint64_t publicationToIngestAccumUs = 0;
    uint32_t publicationToIngestSamples = 0;
    uint32_t publicationToIngestMaxUs = 0;

    while (!g_InjectCaptureShutdown && g_Recording) {
        applySchedulingForCurrentAdapter();
        // Create encoder textures as soon as resolution is available (before frames arrive)
        // This is critical for DXVK where the Vulkan layer waits for encoder KMT textures
        // NOTE: non-static so it resets per thread lifetime (new recording = new thread)
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
        const uint32_t unreadRingEntries = writeIndex - localReadIndex;
        if (unreadRingEntries > static_cast<uint32_t>(FRAME_RING_SIZE)) {
            uint32_t dropped = unreadRingEntries - 1;
            // Only log huge jumps to avoid spam
            if (dropped > 10) {
                LogInfo("[Inject Thread] Lag detected! Dropping %u frames to catch up", dropped);
            }
            const uint32_t catchupReadIndex = writeIndex - 1;
            while (localReadIndex != catchupReadIndex) {
                injectRingLeaseState->Complete(localReadIndex);
                advanceIngestIndex();
            }
            droppedCount += dropped;
            g_pSharedMem->runtimeState.hostDroppedFrames.fetch_add(dropped, std::memory_order_relaxed);
            // Reset pacing on overflow/lag
            nextPushTime = 0;
        }

        if (writeIndex != localReadIndex) {
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
                    qf.injectRingLease = injectRingLeaseState->Acquire(localReadIndex);
                    qf.timestamp = slot.timestamp;
                    LARGE_INTEGER enqueueQpc;
                    QueryPerformanceCounter(&enqueueQpc);
                    qf.enqueueQpc = enqueueQpc.QuadPart;
                    if (slot.timestamp > 0 && enqueueQpc.QuadPart >= slot.timestamp && qpcFreq.QuadPart > 0) {
                        const uint64_t ingestDelayUs = static_cast<uint64_t>(enqueueQpc.QuadPart - slot.timestamp) *
                                                       1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
                        publicationToIngestAccumUs += ingestDelayUs;
                        ++publicationToIngestSamples;
                        publicationToIngestMaxUs =
                            std::max(publicationToIngestMaxUs, SaturatingToUint32(ingestDelayUs));
                    }

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

                    // Inject textures are swap-chain-local, while Windows cursor
                    // coordinates are desktop-global. Resolve the game client
                    // area once per PID and map through its current physical
                    // bounds so windowed, borderless, DPI, and render-scale
                    // configurations all place the cursor correctly.
                    static DWORD s_cursorWindowPid = 0;
                    static HWND s_cursorWindow = NULL;
                    if (qf.sourcePid != s_cursorWindowPid || !s_cursorWindow || !IsWindow(s_cursorWindow) ||
                        !WindowBelongsToProcess(s_cursorWindow, qf.sourcePid)) {
                        s_cursorWindowPid = qf.sourcePid;
                        s_cursorWindow = qf.sourcePid != 0 ? GetMainWindowForProcess(qf.sourcePid) : NULL;
                    }
                    RECT captureBounds = {0, 0, static_cast<LONG>(qf.width), static_cast<LONG>(qf.height)};
                    RECT clientBounds = {};
                    if (s_cursorWindow && GetWindowClientRectInScreen(s_cursorWindow, clientBounds) &&
                        clientBounds.right > clientBounds.left && clientBounds.bottom > clientBounds.top) {
                        captureBounds = clientBounds;
                    }
                    qf.captureLeft = captureBounds.left;
                    qf.captureTop = captureBounds.top;
                    qf.cursorState =
                        CaptureCursorSnapshot(qf.timestamp, captureBounds.left, captureBounds.top,
                                              static_cast<uint32_t>(captureBounds.right - captureBounds.left),
                                              static_cast<uint32_t>(captureBounds.bottom - captureBounds.top), false);
                    g_InjectCursorTimeline.Publish(qf.cursorState);

                    // Per-recording state (reset on thread creation)
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
                                    const uint32_t ringWrite =
                                        g_pSharedMem->frameRing.writeIndex.load(std::memory_order_acquire);
                                    const uint32_t ringAckRead =
                                        g_pSharedMem->frameRing.readIndex.load(std::memory_order_acquire);
                                    const uint32_t nextIngest = localReadIndex + 1;
                                    LogInfo(
                                        "[Inject Thread] Queue frame=%u ring=%u tex=%d fence=%llu ts=%lld qDepth=%u "
                                        "ringIngestNext=%u ringAckRead=%u ringWrite=%u ownedDepth=%u",
                                        lineage.frameIndex, lineage.ringIndex, lineage.textureIndex,
                                        static_cast<unsigned long long>(lineage.fenceValue),
                                        static_cast<long long>(lineage.timestamp), queueDepth, nextIngest, ringAckRead,
                                        ringWrite, static_cast<uint32_t>(ringWrite - ringAckRead));
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
                        advanceIngestIndex();
                        qf.injectRingLease.Reset();
                        continue;
                    }
                } else {
                    // Pacing drop: release the ring slot immediately so the producer
                    // does not stall behind frames that will never be encoded.
                    injectRingLeaseState->Complete(localReadIndex);
                    advanceIngestIndex();
                    continue;
                }

                advanceIngestIndex();
            } else {
                injectRingLeaseState->Complete(localReadIndex);
                advanceIngestIndex();
            }
        } else {
            if (g_InjectFrameReadyEvent && g_InjectCaptureShutdownEvent) {
                HANDLE waitHandles[] = {g_InjectCaptureShutdownEvent, g_InjectFrameReadyEvent};
                const DWORD waitResult = WaitForMultipleObjects(_countof(waitHandles), waitHandles, FALSE, INFINITE);
                if (waitResult == WAIT_OBJECT_0) {
                    break;
                }
                if (waitResult == WAIT_FAILED) {
                    LogWarn("[Inject Thread] Frame-event wait failed (err=%lu); using bounded shutdown wait",
                            GetLastError());
                    WaitForSingleObject(g_InjectCaptureShutdownEvent, 1);
                }
            } else if (g_InjectCaptureShutdownEvent) {
                WaitForSingleObject(g_InjectCaptureShutdownEvent, 1);
            } else {
                SwitchToThread();
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
            uint32_t ringReadIndex = 0;
            uint32_t ringWriteIndex = 0;
            if (g_pSharedMem) {
                ringReadIndex = g_pSharedMem->frameRing.readIndex.load(std::memory_order_acquire);
                ringWriteIndex = g_pSharedMem->frameRing.writeIndex.load(std::memory_order_acquire);
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
                "%u | RingR: %u | RingW: %u | RingDepth: %u | Dup: %u | Late: %u | Trim: %u | SelDrop: %u | Def: %u "
                "| Encode: %lldus | Fence: %lldus | Mux: %uKB | Overload: 0x%X",
                inputFrames, pushedCount, droppedCount, pacingDroppedCount, injectPublicationFps,
                static_cast<uint32_t>(g_FrameQueue.Size()), encoderQueueDepth, ringReadIndex, ringWriteIndex,
                static_cast<uint32_t>(ringWriteIndex - ringReadIndex), dupDelta, lateDelta, trimDelta, cadenceDropDelta,
                deferredDelta, MediaEngine_GetLastFrameEncodeTimeUs(), MediaEngine_GetLastFrameFenceWaitUs(),
                (muxQueueBytes + 1023u) / 1024u, overloadFlags);
            auto& contention = g_pSharedMem->runtimeState;
            const uint32_t ingestAvgUs =
                publicationToIngestSamples > 0
                    ? SaturatingToUint32(publicationToIngestAccumUs / publicationToIngestSamples)
                    : 0;
            contention.injectPublicationToIngestAvgUs.store(ingestAvgUs, std::memory_order_relaxed);
            contention.injectPublicationToIngestMaxUs.store(publicationToIngestMaxUs, std::memory_order_relaxed);
            LogInfo(
                "[Inject Contention] CaptureLock=%u CpuLease=%u GpuBusy=%u RingFull=%u EventSignals=%u "
                "PubToIngest=%u/%uus",
                contention.injectProducerCaptureLockDrops.load(std::memory_order_relaxed),
                contention.injectProducerCpuLeaseBusyDrops.load(std::memory_order_relaxed),
                contention.injectProducerGpuBusyDrops.load(std::memory_order_relaxed),
                contention.injectProducerMetadataFullDrops.load(std::memory_order_relaxed),
                contention.injectFrameReadySignals.load(std::memory_order_relaxed), ingestAvgUs,
                publicationToIngestMaxUs);
            publicationToIngestAccumUs = 0;
            publicationToIngestSamples = 0;
            publicationToIngestMaxUs = 0;
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
    DisableCurrentThreadPowerThrottling("WGC CaptureThread");
    ScopedMmcssTask wgcMmcssTask(L"Capture", AVRT_PRIORITY_HIGH, "WGC CaptureThread");

    DWORD lastDiagTime = 0;
    uint32_t lastInputCount = 0;
    uint32_t lastCallbackCount = 0;
    uint64_t lastHostDroppedCount = 0;
    uint32_t lastPacingSkipCount = 0;
    uint32_t lastThrottleSkipCount = 0;
    uint32_t lastStaleSkipCount = 0;
    uint32_t lastStaleDuplicateTsCount = 0;
    uint32_t lastStaleOutOfOrderTsCount = 0;
    uint32_t lastNormalizedDuplicateTsCount = 0;
    uint32_t lastDuplicateTsSkipCount = 0;
    uint32_t lastCursorSkipCount = 0;
    uint32_t lastPoolDropCount = 0;
    uint32_t lastKeyedAcquireFailCount = 0;
    uint32_t lastKeyedReleaseFailCount = 0;
    uint32_t lastKeyedAbandonedReclaimCount = 0;
    uint32_t lastSplitFlushCount = 0;
    uint32_t lastSplitFlushSkippedCount = 0;
    uint32_t lastPoolSlotFastRewriteCount = 0;
    uint32_t lastPoolSaturatedDropCount = 0;
    uint32_t lastPoolOverwritePreventedCount = 0;
    uint32_t lastIngressAcceptedCount = 0;
    uint32_t lastIngressDecimatedCount = 0;
    uint32_t lastIngressAcceptedLowWaterCount = 0;
    uint32_t lastIngressAcceptedRecoveryCount = 0;
    uint32_t lastIngressAcceptedSourceBelowCount = 0;
    uint32_t lastIngressAcceptedHealthyCount = 0;
    uint32_t lastIngressAcceptedUniformPlayoutSoftReserveCount = 0;
    uint32_t lastIngressAcceptedUniformPlayoutCreditCount = 0;
    uint32_t lastIngressDecimatedSoftReserveCount = 0;
    uint32_t lastIngressDecimatedHardReserveCount = 0;
    uint32_t lastIngressDecimatedCreditCount = 0;
    uint32_t lastIngressSoftReservePressureCount = 0;
    uint32_t lastIngressHardReservePressureCount = 0;
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
            lastNormalizedDuplicateTsCount = 0;
            lastDuplicateTsSkipCount = 0;
            lastCursorSkipCount = 0;
            lastPoolDropCount = 0;
            lastKeyedAcquireFailCount = 0;
            lastKeyedReleaseFailCount = 0;
            lastKeyedAbandonedReclaimCount = 0;
            lastSplitFlushCount = 0;
            lastSplitFlushSkippedCount = 0;
            lastPoolSlotFastRewriteCount = 0;
            lastPoolSaturatedDropCount = 0;
            lastPoolOverwritePreventedCount = 0;
            lastIngressAcceptedCount = 0;
            lastIngressDecimatedCount = 0;
            lastIngressAcceptedLowWaterCount = 0;
            lastIngressAcceptedRecoveryCount = 0;
            lastIngressAcceptedSourceBelowCount = 0;
            lastIngressAcceptedHealthyCount = 0;
            lastIngressAcceptedUniformPlayoutSoftReserveCount = 0;
            lastIngressAcceptedUniformPlayoutCreditCount = 0;
            lastIngressDecimatedSoftReserveCount = 0;
            lastIngressDecimatedHardReserveCount = 0;
            lastIngressDecimatedCreditCount = 0;
            lastIngressSoftReservePressureCount = 0;
            lastIngressHardReservePressureCount = 0;
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
            lastNormalizedDuplicateTsCount = g_WgcCap->GetNormalizedDuplicateTimestampCount();
            lastDuplicateTsSkipCount = g_WgcCap->GetDuplicateTimestampSkipCount();
            lastCursorSkipCount = g_WgcCap->GetCursorOnlySkipCount();
            lastPoolDropCount = g_WgcCap->GetPoolDropCount();
            lastKeyedAcquireFailCount = g_WgcCap->GetKeyedMutexAcquireFailCount();
            lastKeyedReleaseFailCount = g_WgcCap->GetKeyedMutexReleaseFailCount();
            lastKeyedAbandonedReclaimCount = g_WgcCap->GetKeyedMutexAbandonedReclaimCount();
            lastSplitFlushCount = g_WgcCap->GetSplitDeviceFlushCount();
            lastSplitFlushSkippedCount = g_WgcCap->GetSplitDeviceFlushSkippedCount();
            lastPoolSlotFastRewriteCount = g_WgcCap->GetPoolSlotFastRewriteCount();
            lastPoolSaturatedDropCount = g_WgcCap->GetPoolSaturatedDropCount();
            lastPoolOverwritePreventedCount = g_WgcCap->GetPoolSlotOverwritePreventedCount();
            lastIngressAcceptedCount = g_WgcCap->GetIngressAcceptedCount();
            lastIngressDecimatedCount = g_WgcCap->GetIngressDecimatedCount();
            lastIngressAcceptedLowWaterCount = g_WgcCap->GetIngressAcceptedLowWaterCount();
            lastIngressAcceptedRecoveryCount = g_WgcCap->GetIngressAcceptedRecoveryCount();
            lastIngressAcceptedSourceBelowCount = g_WgcCap->GetIngressAcceptedSourceBelowCount();
            lastIngressAcceptedHealthyCount = g_WgcCap->GetIngressAcceptedHealthyCount();
            lastIngressAcceptedUniformPlayoutSoftReserveCount =
                g_WgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount();
            lastIngressAcceptedUniformPlayoutCreditCount = g_WgcCap->GetIngressAcceptedUniformPlayoutCreditCount();
            lastIngressDecimatedSoftReserveCount = g_WgcCap->GetIngressDecimatedSoftReserveCount();
            lastIngressDecimatedHardReserveCount = g_WgcCap->GetIngressDecimatedHardReserveCount();
            lastIngressDecimatedCreditCount = g_WgcCap->GetIngressDecimatedCreditCount();
            lastIngressSoftReservePressureCount = g_WgcCap->GetIngressSoftReservePressureCount();
            lastIngressHardReservePressureCount = g_WgcCap->GetIngressHardReservePressureCount();
            SnapshotPublishedWgcRuntimeLogState();
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
            uint32_t currentNormalizedDuplicateTsCount = g_WgcCap->GetNormalizedDuplicateTimestampCount();
            uint32_t currentDuplicateTsSkipCount = g_WgcCap->GetDuplicateTimestampSkipCount();
            uint32_t currentCursorSkipCount = g_WgcCap->GetCursorOnlySkipCount();
            uint32_t currentPoolDropCount = g_WgcCap->GetPoolDropCount();
            uint32_t currentKeyedAcquireFailCount = g_WgcCap->GetKeyedMutexAcquireFailCount();
            uint32_t currentKeyedReleaseFailCount = g_WgcCap->GetKeyedMutexReleaseFailCount();
            uint32_t currentKeyedAbandonedReclaimCount = g_WgcCap->GetKeyedMutexAbandonedReclaimCount();
            uint32_t currentSplitFlushCount = g_WgcCap->GetSplitDeviceFlushCount();
            uint32_t currentSplitFlushSkippedCount = g_WgcCap->GetSplitDeviceFlushSkippedCount();
            uint32_t currentPoolSlotFastRewriteCount = g_WgcCap->GetPoolSlotFastRewriteCount();
            uint32_t currentPoolSaturatedDropCount = g_WgcCap->GetPoolSaturatedDropCount();
            uint32_t currentPoolOverwritePreventedCount = g_WgcCap->GetPoolSlotOverwritePreventedCount();
            uint32_t currentIngressAcceptedCount = g_WgcCap->GetIngressAcceptedCount();
            uint32_t currentIngressDecimatedCount = g_WgcCap->GetIngressDecimatedCount();
            uint32_t currentIngressAcceptedLowWaterCount = g_WgcCap->GetIngressAcceptedLowWaterCount();
            uint32_t currentIngressAcceptedRecoveryCount = g_WgcCap->GetIngressAcceptedRecoveryCount();
            uint32_t currentIngressAcceptedSourceBelowCount = g_WgcCap->GetIngressAcceptedSourceBelowCount();
            uint32_t currentIngressAcceptedHealthyCount = g_WgcCap->GetIngressAcceptedHealthyCount();
            uint32_t currentIngressAcceptedUniformPlayoutSoftReserveCount =
                g_WgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount();
            uint32_t currentIngressAcceptedUniformPlayoutCreditCount =
                g_WgcCap->GetIngressAcceptedUniformPlayoutCreditCount();
            uint32_t currentIngressDecimatedSoftReserveCount = g_WgcCap->GetIngressDecimatedSoftReserveCount();
            uint32_t currentIngressDecimatedHardReserveCount = g_WgcCap->GetIngressDecimatedHardReserveCount();
            uint32_t currentIngressDecimatedCreditCount = g_WgcCap->GetIngressDecimatedCreditCount();
            uint32_t currentIngressSoftReservePressureCount = g_WgcCap->GetIngressSoftReservePressureCount();
            uint32_t currentIngressHardReservePressureCount = g_WgcCap->GetIngressHardReservePressureCount();
            uint32_t inputFrames = currentInputCount - lastInputCount;
            uint32_t deliveredFrames = currentCount - lastCallbackCount;
            uint32_t hostDropDelta =
                static_cast<uint32_t>(queueDropped >= lastHostDroppedCount ? (queueDropped - lastHostDroppedCount) : 0);
            uint32_t pacingSkipDelta = currentPacingSkipCount - lastPacingSkipCount;
            uint32_t throttleSkipDelta = currentThrottleSkipCount - lastThrottleSkipCount;
            uint32_t staleSkipDelta = currentStaleSkipCount - lastStaleSkipCount;
            uint32_t staleDuplicateTsDelta = currentStaleDuplicateTsCount - lastStaleDuplicateTsCount;
            uint32_t staleOutOfOrderTsDelta = currentStaleOutOfOrderTsCount - lastStaleOutOfOrderTsCount;
            uint32_t normalizedDuplicateTsDelta = currentNormalizedDuplicateTsCount - lastNormalizedDuplicateTsCount;
            uint32_t duplicateTsSkipDelta = currentDuplicateTsSkipCount - lastDuplicateTsSkipCount;
            uint32_t cursorSkipDelta = currentCursorSkipCount - lastCursorSkipCount;
            uint32_t poolDropDelta = currentPoolDropCount - lastPoolDropCount;
            uint32_t keyedAcquireFailDelta = currentKeyedAcquireFailCount - lastKeyedAcquireFailCount;
            uint32_t keyedReleaseFailDelta = currentKeyedReleaseFailCount - lastKeyedReleaseFailCount;
            uint32_t keyedAbandonedReclaimDelta = currentKeyedAbandonedReclaimCount - lastKeyedAbandonedReclaimCount;
            uint32_t splitFlushDelta = currentSplitFlushCount - lastSplitFlushCount;
            uint32_t splitFlushSkippedDelta = currentSplitFlushSkippedCount - lastSplitFlushSkippedCount;
            uint32_t poolSlotFastRewriteDelta = currentPoolSlotFastRewriteCount - lastPoolSlotFastRewriteCount;
            uint32_t poolSaturatedDropDelta = currentPoolSaturatedDropCount - lastPoolSaturatedDropCount;
            uint32_t poolOverwritePreventedDelta = currentPoolOverwritePreventedCount - lastPoolOverwritePreventedCount;
            uint32_t ingressAcceptedDelta = currentIngressAcceptedCount - lastIngressAcceptedCount;
            uint32_t ingressDecimatedDelta = currentIngressDecimatedCount - lastIngressDecimatedCount;
            uint32_t ingressAcceptedLowWaterDelta =
                currentIngressAcceptedLowWaterCount - lastIngressAcceptedLowWaterCount;
            uint32_t ingressAcceptedRecoveryDelta =
                currentIngressAcceptedRecoveryCount - lastIngressAcceptedRecoveryCount;
            uint32_t ingressAcceptedSourceBelowDelta =
                currentIngressAcceptedSourceBelowCount - lastIngressAcceptedSourceBelowCount;
            uint32_t ingressAcceptedHealthyDelta = currentIngressAcceptedHealthyCount - lastIngressAcceptedHealthyCount;
            uint32_t ingressAcceptedUniformPlayoutSoftReserveDelta =
                currentIngressAcceptedUniformPlayoutSoftReserveCount -
                lastIngressAcceptedUniformPlayoutSoftReserveCount;
            uint32_t ingressAcceptedUniformPlayoutCreditDelta =
                currentIngressAcceptedUniformPlayoutCreditCount - lastIngressAcceptedUniformPlayoutCreditCount;
            uint32_t ingressDecimatedSoftReserveDelta =
                currentIngressDecimatedSoftReserveCount - lastIngressDecimatedSoftReserveCount;
            uint32_t ingressDecimatedHardReserveDelta =
                currentIngressDecimatedHardReserveCount - lastIngressDecimatedHardReserveCount;
            uint32_t ingressDecimatedCreditDelta = currentIngressDecimatedCreditCount - lastIngressDecimatedCreditCount;
            uint32_t ingressSoftReservePressureDelta =
                currentIngressSoftReservePressureCount - lastIngressSoftReservePressureCount;
            uint32_t ingressHardReservePressureDelta =
                currentIngressHardReservePressureCount - lastIngressHardReservePressureCount;
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
            uint32_t throttleTargetFps = g_WgcCap->GetProducerTargetFps();
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

            SnapshotPublishedWgcRuntimeLogState();
            LogInfo(
                "[WGC Perf] Input: %u | Queued: %u | DropFull: %u | DropPace: %u | DropThrottle: %u | "
                "DropStale: %u (DupTs=%u OOO=%u) | SrcDupTs: seen=%u skip=%u | DropCursor: %u | "
                "DropPool: %u | DropIngress: %u | "
                "HostQ: %u | EncQ: %u | Dup: %u | Late: %u | "
                "SrcAvg: %lldus | JitAvg: %lldus | JitMax: %lldus | Src->Copy: %lld/%lldus | Deliv: %u | "
                "MinIn250/500: %u/%u | MinDel250/500: %u/%u | FreshMiss: %upm | BufAvg: %upm | BufMin: %u | "
                "NoFresh: %u | NoReserve: %u | SchedSelAvg: %uus "
                "SchedSelBias: %dus | WgcSelAvg: %uus WgcSelBias: %dus | CbGap: %lld/%lldus "
                "CbProc: %lld/%lldus CbDrainMax: %u | Copy: %lldus | "
                "SlotAge: %lldus FastSlot: %u | PoolLease: max=%u freeMin=%u satDrop=%u overwritePrevented=%u "
                "mismatch=%u sourceFramePoolBuffers=%u copyPoolSlots=%u budgetSurfaces=%u syncFrames=%u "
                "extraFrames=%u retainedCap=%u reservedFree=%u safetySlots=%u "
                "sourceFmt=%u copyFmt=%u compactRetained=%d sourceBudgetMB=%.1f copyBudgetMB=%.1f "
                "sourceSurfaceMB=%.1f copySurfaceMB=%.1f convertUs=%lld | "
                "Ingress: accepted=%u decimated=%u retained=%u/%u lowWater=%u reason=%s "
                "accLow=%u accRec=%u accSrcBelow=%u accHealthy=%u accPlaySoft=%u accPlayCredit=%u "
                "decSoft=%u decHard=%u decCredit=%u softPress=%u hardPress=%u | "
                "KMFail: %u/%u KMReclaim: %u | Flush: %u/%u | "
                "Dedicated: %d | Encode: %lldus | Fence: %lldus | Throttle: %u | Mux: %uKB | Overload: 0x%X | "
                "Backend: %s DupIdleTimeouts: %llu DupMissed: %llu DupHwCursor: %d DupCursorEmbedded: %d "
                "DupPtrTransitions: %llu | TimingBasis: Copy/Convert/Encode/Fence=CPU-wall-or-submit",
                inputFrames, queuedFrames, hostDropDelta, pacingSkipDelta, throttleSkipDelta, staleSkipDelta,
                staleDuplicateTsDelta, staleOutOfOrderTsDelta, normalizedDuplicateTsDelta, duplicateTsSkipDelta,
                cursorSkipDelta, poolDropDelta, ingressDecimatedDelta, static_cast<uint32_t>(g_FrameQueue.Size()),
                encoderQueueDepth, dupDelta, lateDelta, srcIntervalAvgUs, srcJitterAvgUs, srcJitterMaxUs,
                srcToCopyAvgUs, srcToCopyMaxUs, deliveredRatePerSec, inputMin250Fps, inputMin500Fps, deliveredMin250Fps,
                deliveredMin500Fps, queueEmptyPermille, bufferedAtTickAvgPermille, bufferedAtTickMin, starvedTicks,
                singleFrameTicks, cadenceSelAvgUs, cadenceSelBiasUs, wgcSelAvgUs, wgcSelBiasUs, callbackGapAvgUs,
                callbackGapMaxUs, callbackProcessAvgUs, callbackProcessMaxUs, callbackDrainMax, copyUs,
                poolSlotRewriteUs, poolSlotFastRewriteDelta, g_WgcCap->GetPoolSlotLeasedMaxCount(),
                g_WgcCap->GetPoolSlotFreeMinCount(), poolSaturatedDropDelta, poolOverwritePreventedDelta,
                g_WgcCap->GetPoolLeaseMismatchCount(), g_WgcCap->GetSourceFramePoolBufferCount(),
                g_WgcCap->GetTexturePoolSlotCount(), g_WgcCap->GetSmoothnessBudgetSurfaceCount(),
                g_WgcCap->GetSmoothnessSyncFrameCount(), g_WgcCap->GetSmoothnessRetainedFrameCount(),
                g_WgcCap->GetSmoothnessRetainedFrameCap(), g_WgcCap->GetSmoothnessReservedFreeSlotCount(),
                g_WgcCap->GetSmoothnessSafetySlotCount(), g_WgcCap->GetSmoothnessSourceDxgiFormat(),
                g_WgcCap->GetSmoothnessCopyDxgiFormat(), g_WgcCap->IsCompactRetainedCopyActive() ? 1 : 0,
                static_cast<double>(g_WgcCap->GetSmoothnessSourceEstimatedVramBytes()) / (1024.0 * 1024.0),
                static_cast<double>(g_WgcCap->GetSmoothnessCopyEstimatedVramBytes()) / (1024.0 * 1024.0),
                static_cast<double>(g_WgcCap->GetSmoothnessSourceBytesPerSurface()) / (1024.0 * 1024.0),
                static_cast<double>(g_WgcCap->GetSmoothnessCopyBytesPerSurface()) / (1024.0 * 1024.0),
                static_cast<long long>(g_WgcCap->GetLastPoolConvertTimeUs()), ingressAcceptedDelta,
                ingressDecimatedDelta, g_WgcCap->GetIngressRetainedFrameCount(), g_WgcCap->GetIngressRetainedFrameCap(),
                g_WgcCap->GetIngressLowWaterFrameCount(),
                WgcIngressAdmissionReasonName(g_WgcCap->GetIngressAdmissionReasonCode()), ingressAcceptedLowWaterDelta,
                ingressAcceptedRecoveryDelta, ingressAcceptedSourceBelowDelta, ingressAcceptedHealthyDelta,
                ingressAcceptedUniformPlayoutSoftReserveDelta, ingressAcceptedUniformPlayoutCreditDelta,
                ingressDecimatedSoftReserveDelta, ingressDecimatedHardReserveDelta, ingressDecimatedCreditDelta,
                ingressSoftReservePressureDelta, ingressHardReservePressureDelta, keyedAcquireFailDelta,
                keyedReleaseFailDelta, keyedAbandonedReclaimDelta, splitFlushDelta, splitFlushSkippedDelta,
                g_WgcCap->IsUsingDedicatedCaptureDevice() ? 1 : 0, encodeUs, fenceUs, throttleTargetFps,
                (muxQueueBytes + 1023u) / 1024u, overloadFlags,
                g_WgcCap->IsUsingDesktopDuplication() ? "DxgiDuplication" : "WGC",
                static_cast<unsigned long long>(g_WgcCap->GetDuplicationAcquireTimeoutCount()),
                static_cast<unsigned long long>(g_WgcCap->GetDuplicationAccumulatedMissedFrameCount()),
                g_WgcCap->IsDuplicationSeparatePointerVisible() ? 1 : 0,
                g_WgcCap->IsDuplicationCursorEmbedded() ? 1 : 0,
                static_cast<unsigned long long>(g_WgcCap->GetDuplicationPointerStateTransitionCount()));

            lastInputCount = currentInputCount;
            lastCallbackCount = currentCount;
            lastHostDroppedCount = queueDropped;
            lastPacingSkipCount = currentPacingSkipCount;
            lastThrottleSkipCount = currentThrottleSkipCount;
            lastStaleSkipCount = currentStaleSkipCount;
            lastStaleDuplicateTsCount = currentStaleDuplicateTsCount;
            lastStaleOutOfOrderTsCount = currentStaleOutOfOrderTsCount;
            lastNormalizedDuplicateTsCount = currentNormalizedDuplicateTsCount;
            lastDuplicateTsSkipCount = currentDuplicateTsSkipCount;
            lastCursorSkipCount = currentCursorSkipCount;
            lastPoolDropCount = currentPoolDropCount;
            lastKeyedAcquireFailCount = currentKeyedAcquireFailCount;
            lastKeyedReleaseFailCount = currentKeyedReleaseFailCount;
            lastKeyedAbandonedReclaimCount = currentKeyedAbandonedReclaimCount;
            lastSplitFlushCount = currentSplitFlushCount;
            lastSplitFlushSkippedCount = currentSplitFlushSkippedCount;
            lastPoolSlotFastRewriteCount = currentPoolSlotFastRewriteCount;
            lastPoolSaturatedDropCount = currentPoolSaturatedDropCount;
            lastPoolOverwritePreventedCount = currentPoolOverwritePreventedCount;
            lastIngressAcceptedCount = currentIngressAcceptedCount;
            lastIngressDecimatedCount = currentIngressDecimatedCount;
            lastIngressAcceptedLowWaterCount = currentIngressAcceptedLowWaterCount;
            lastIngressAcceptedRecoveryCount = currentIngressAcceptedRecoveryCount;
            lastIngressAcceptedSourceBelowCount = currentIngressAcceptedSourceBelowCount;
            lastIngressAcceptedHealthyCount = currentIngressAcceptedHealthyCount;
            lastIngressAcceptedUniformPlayoutSoftReserveCount = currentIngressAcceptedUniformPlayoutSoftReserveCount;
            lastIngressAcceptedUniformPlayoutCreditCount = currentIngressAcceptedUniformPlayoutCreditCount;
            lastIngressDecimatedSoftReserveCount = currentIngressDecimatedSoftReserveCount;
            lastIngressDecimatedHardReserveCount = currentIngressDecimatedHardReserveCount;
            lastIngressDecimatedCreditCount = currentIngressDecimatedCreditCount;
            lastIngressSoftReservePressureCount = currentIngressSoftReservePressureCount;
            lastIngressHardReservePressureCount = currentIngressHardReservePressureCount;
            lastDiagTime = now;
        }
    }

    g_WgcCaptureRunning = false;
    LogInfo("[WGC CaptureThread] Stopped");
}

void EncoderThreadFunc(const AppConfig& config) {
    LogInfo("[EncoderThread] Started");

    g_WgcCursorTimeline.Clear();
    g_InjectCursorTimeline.Clear();

    DisableCurrentThreadPowerThrottling("EncoderThread");
    ScopedMmcssTask encoderMmcssTask(L"Pro Audio", AVRT_PRIORITY_HIGH, "EncoderThread");

    g_FrameQueue.StartRecording();
    SetCapturePipelinePhase(CapturePipelinePhase::kWarmup);

    LARGE_INTEGER qpcFreq;
    QueryPerformanceFrequency(&qpcFreq);
    int64_t targetIntervalTicks = qpcFreq.QuadPart / config.video.fps;
    const uint32_t captureSyncMultiplier =
        static_cast<uint32_t>(std::clamp(config.fpsLimiter.captureSyncMultiplier, 1, 8));
    const bool captureSyncPhaseLockEnabled =
        config.fpsLimiter.captureSyncEnabled && !config.video.useVFR && targetIntervalTicks > 0;
    const int64_t captureSyncSourceIntervalTicks = ce::capture_policy::GetCfrCaptureSyncSourceIntervalQpc(
        targetIntervalTicks, captureSyncMultiplier);
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
    uint64_t encoderWakeLateAccumUs = 0;
    uint64_t encoderWakeLateSamples = 0;
    uint32_t encoderWakeLateMaxUs = 0;
    auto ReleaseQueuedFrameTexture = [](QueuedFrame& queuedFrame) {
        if (!queuedFrame.isInjectMode && queuedFrame.texture) {
            queuedFrame.texture->Release();
            queuedFrame.texture = nullptr;
        }
        if (!queuedFrame.isInjectMode) {
            queuedFrame.wgcPoolLease.Reset();
            queuedFrame.wgcPoolSlot = std::numeric_limits<uint32_t>::max();
            queuedFrame.wgcPoolGeneration = 0;
        }
    };
    auto DiscardQueuedFrame = [&](QueuedFrame& queuedFrame) {
        if (!queuedFrame.isInjectMode) {
            ReleaseQueuedFrameTexture(queuedFrame);
        }
        queuedFrame = QueuedFrame{};
    };
    std::vector<QueuedFrame> drainedScreenGrabFrames;
    drainedScreenGrabFrames.reserve(8);
    std::vector<WGCCapturedFrame> drainedWgcCapturedFrames;
    drainedWgcCapturedFrames.reserve(8);
    std::deque<QueuedFrame> bufferedWgcFrames;
    uint64_t observedWgcSourceEpoch = g_WgcSourceEpoch.load(std::memory_order_acquire);
    bool lastSuccessfulWgcCursorEmbedded = false;
    bool hasSuccessfulWgcCursorMetadata = false;
    std::vector<size_t> wgcFreshCandidateIndices;
    wgcFreshCandidateIndices.reserve(64);
    std::vector<size_t> wgcFallbackCandidateIndices;
    wgcFallbackCandidateIndices.reserve(64);
    std::vector<size_t> wgcRelaxedFreshCandidateIndices;
    wgcRelaxedFreshCandidateIndices.reserve(64);
    std::vector<size_t> wgcRelaxedFallbackCandidateIndices;
    wgcRelaxedFallbackCandidateIndices.reserve(64);
    std::vector<size_t> wgcRepeatRescueCandidateIndices;
    wgcRepeatRescueCandidateIndices.reserve(64);
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
    // Source-rate EMA is telemetry/recovery context only. Live CFR source choice is timestamp-driven.
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
    InjectFrameLineage lastSuccessfullyEncodedInjectLineage;
    CadenceHealthCounters cadenceCounters;
    InputFrameRatePredictor wgcInputPredictor;
    InputFrameRatePredictor injectInputPredictor;
    ce::capture_policy::CfrCadencePhaseLockState injectCfrPhaseLock;
    ce::capture_policy::CfrCadencePhaseLockState wgcCfrPhaseLock;
    uint32_t injectWorstSourceFpsX100 = std::numeric_limits<uint32_t>::max();
    uint32_t injectBestSourceFpsX100 = 0;
    uint32_t injectWorstSourceJitterUs = 0;
    uint32_t injectWorstSelectionErrorUs = 0;
    double smoothedEncCycleMs = 0.0;
    double smoothedInjectServiceMs = 0.0;
    uint32_t injectServiceMaxUs = 0;
    uint32_t encCycleMaxMs = 0;
    uint32_t encodeSpikeCountThisSecond = 0;
    uint32_t dupTimestampCount = 0;
    uint32_t lastWgcDuplicateTimestampSkipCountForCadence = 0;
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
    uint32_t injectTargetSelectThisWindow = 0;
    uint32_t injectTargetSupersededThisWindow = 0;
    uint32_t injectTargetHoldThisWindow = 0;
    uint32_t injectTargetHoldWithCandidateThisWindow = 0;
    uint32_t activePathMismatchDiscardThisWindow = 0;
    uint64_t injectFreshCatchupTotal = 0;
    uint64_t injectRepeatCatchupTotal = 0;
    uint64_t injectLiveStaleTrimTotal = 0;
    uint64_t injectTargetSelectTotal = 0;
    uint64_t injectTargetSupersededTotal = 0;
    uint64_t injectTargetHoldTotal = 0;
    uint64_t injectTargetHoldWithCandidateTotal = 0;
    uint64_t injectBufferCapTrimTotal = 0;
    uint32_t injectTargetResidualMaxUs = 0;
    bool injectCfrRecoveryActive = false;
    bool injectEncoderServiceTooSlowCurrent = false;
    uint32_t injectCfrRecoveryEpisodesThisWindow = 0;
    uint64_t injectCfrRecoveryEpisodesTotal = 0;
    uint64_t injectCfrRecoveryStartTick = 0;
    uint32_t injectCfrRecoveryStartDebt = 0;
    uint32_t injectCfrRecoveryBestDebt = 0;
    uint64_t injectCfrRecoveryStartFreshCatchup = 0;
    uint64_t injectCfrRecoveryStartRepeatCatchup = 0;
    uint64_t injectCfrRecoveryLastProgressLogTick = 0;
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
    uint32_t wgcSyncDelayHoldCount = 0;
    uint64_t wgcSyncDelayHoldTotal = 0;
    uint32_t wgcSyncDelaySourceLimitedHoldCount = 0;
    uint64_t wgcSyncDelaySourceLimitedHoldTotal = 0;
    uint32_t wgcSyncDelayPolicyHoldCount = 0;
    uint64_t wgcSyncDelayPolicyHoldTotal = 0;
    uint32_t wgcTooNewLeadMaxUs = 0;
    uint32_t wgcTooNewLeadSessionMaxUs = 0;
    uint32_t wgcStartupReserveFrames = 0;
    int64_t wgcStartupReserveSpanUs = 0;
    int64_t wgcStartupDelayTargetUs = 0;
    bool wgcStartupSelectedByDelayReserve = false;
    std::string wgcStartupReserveReason = "not-run";
    int64_t wgcSmoothnessActiveDelayQpc = 0;
    ce::capture_policy::CfrTimelineStartContract pendingWgcStartContract{};
    uint64_t pendingWgcStartContractGeneration = 0;
    uint64_t committedWgcStartContractGeneration = 0;
    bool wgcEncoderPrewarmAttempted = false;
    bool wgcEncoderPrewarmSucceeded = false;
    int64_t wgcEncoderPrewarmElapsedUs = 0;
    // Smoothness FLOOR diagnostics/state (resolved once at startup, then fixed for the session).
    int64_t wgcSmoothnessFloorDelayQpc = 0;            // resolved floor delay target (QPC); 0 = floor inactive
    int64_t wgcSmoothnessFloorRequestedQpc = 0;        // pre-clamp requested floor (QPC), for logging
    const char* wgcSmoothnessFloorSource = "off";      // off | auto | config
    const char* wgcSmoothnessFloorClampedBy = "none";  // none | min | max_ms | reservoir
    bool wgcSmoothnessFloorLogged = false;  // latch: the one-time floor decision log fires once per recording
    ce::capture_policy::WgcSmoothnessFloorJitter wgcSmoothnessFloorJitter{};  // measured jitter used for auto
    uint32_t wgcSmoothnessDesiredFrames = 0;
    uint32_t wgcSmoothnessRetainedFrames = 0;
    uint32_t wgcSmoothnessActualFrames = 0;
    uint32_t wgcSmoothnessPoolSlots = 0;
    uint32_t wgcSmoothnessRetainedFrameCap = 0;
    uint32_t wgcSmoothnessReservedFreeSlots = 0;
    uint64_t wgcSmoothnessEstimatedVramBytes = 0;
    bool wgcSmoothnessCapLimited = false;
    std::string wgcSmoothnessBufferReason = "not-run";
    uint32_t wgcDelayReservoirLowWaterTickCount = 0;
    uint64_t wgcDelayReservoirLowWaterTickTotal = 0;
    // WGC selection-timestamp smoothing telemetry (monotonic bounded-deviation
    // smoother over raw compositor timestamps; see InputFrameRatePredictor::
    // SmoothMonotonicTimestamp). Deviation = |selection - raw normalized|.
    uint64_t wgcTsSmoothSamplesWindow = 0;
    uint64_t wgcTsSmoothDevAccumUsWindow = 0;
    uint32_t wgcTsSmoothDevMaxUsWindow = 0;
    uint32_t wgcTsSmoothDevMaxUsTotal = 0;
    uint32_t wgcTsSmoothSnapCountWindow = 0;
    uint64_t wgcTsSmoothSnapCountTotal = 0;
    uint64_t wgcDelayResidualSamples = 0;
    uint64_t wgcDelayResidualAbsAccumUs = 0;
    int64_t wgcDelayResidualSignedAccumUs = 0;
    uint32_t wgcDelayResidualAbsMaxUs = 0;
    uint32_t wgcDelayResidualLateMaxUs = 0;
    uint32_t wgcDelayResidualEarlyMaxUs = 0;
    uint64_t wgcDelayRealizedAccumUs = 0;
    uint32_t wgcDelayRealizedMinUs = UINT32_MAX;
    uint32_t wgcDelayRealizedMaxUs = 0;
    std::array<uint32_t, 256> wgcDelayResidualAbsHistogram{};
    uint64_t wgcDelayResidualWindowSamples = 0;
    uint64_t wgcDelayResidualWindowAbsAccumUs = 0;
    int64_t wgcDelayResidualWindowSignedAccumUs = 0;
    uint32_t wgcDelayResidualWindowAbsMaxUs = 0;
    uint32_t wgcDelayResidualWindowLateMaxUs = 0;
    std::array<uint32_t, 256> wgcDelayResidualWindowAbsHistogram{};
    uint64_t wgcDelayRawResidualSamples = 0;
    uint64_t wgcDelayRawResidualAbsAccumUs = 0;
    int64_t wgcDelayRawResidualSignedAccumUs = 0;
    uint32_t wgcDelayRawResidualAbsMaxUs = 0;
    uint32_t wgcDelayRawResidualLateMaxUs = 0;
    uint32_t wgcDelayRawResidualEarlyMaxUs = 0;
    std::array<uint32_t, 256> wgcDelayRawResidualAbsHistogram{};
    uint64_t wgcDelayRawResidualWindowSamples = 0;
    uint64_t wgcDelayRawResidualWindowAbsAccumUs = 0;
    int64_t wgcDelayRawResidualWindowSignedAccumUs = 0;
    uint32_t wgcDelayRawResidualWindowAbsMaxUs = 0;
    uint32_t wgcDelayRawResidualWindowLateMaxUs = 0;
    std::array<uint32_t, 256> wgcDelayRawResidualWindowAbsHistogram{};
    uint64_t wgcDelayRawMinusPredictedSamples = 0;
    int64_t wgcDelayRawMinusPredictedSignedAccumUs = 0;
    uint32_t wgcDelayRawMinusPredictedAbsMaxUs = 0;
    uint64_t wgcDelayRawMinusPredictedWindowSamples = 0;
    int64_t wgcDelayRawMinusPredictedWindowSignedAccumUs = 0;
    uint32_t wgcDelayRawMinusPredictedWindowAbsMaxUs = 0;
    uint64_t wgcDelayRelaxedSelectionCount = 0;
    uint32_t wgcDelayRelaxedSelectionWindowCount = 0;
    uint32_t wgcDelayRelaxedSelectionMaxUs = 0;
    uint64_t wgcDelayRelaxedBetterTargetTotal = 0;
    uint32_t wgcDelayRelaxedBetterTargetWindow = 0;
    uint64_t wgcDelayRelaxedRepeatClusterTotal = 0;
    uint32_t wgcDelayRelaxedRepeatClusterWindow = 0;
    uint64_t wgcDelayRelaxedRejectedSyncRiskTotal = 0;
    uint32_t wgcDelayRelaxedRejectedSyncRiskWindow = 0;
    uint64_t wgcDelayRelaxedRejectedResidualHeadroomTotal = 0;
    uint32_t wgcDelayRelaxedRejectedResidualHeadroomWindow = 0;
    uint64_t wgcDelayRelaxedRejectedRepeatCostTotal = 0;
    uint32_t wgcDelayRelaxedRejectedRepeatCostWindow = 0;
    uint64_t wgcDelaySoftLateRejectedTotal = 0;
    uint32_t wgcDelaySoftLateRejectedWindow = 0;
    uint64_t wgcDelaySoftLateAcceptedTotal = 0;
    uint32_t wgcDelaySoftLateAcceptedWindow = 0;
    uint64_t wgcDelayNearCapAcceptedTotal = 0;
    uint32_t wgcDelayNearCapAcceptedWindow = 0;
    // Frames selected under uniform-cadence active-delay mode (reserve-defense perturbations
    // skipped, closest-to-target with monotonic + hard-cap guards). Lets the GPU-bound judder
    // fix be confirmed from logs and distinguishes it from per-tick reserve defense.
    uint64_t wgcDelayUniformCadenceTotal = 0;
    uint32_t wgcDelayUniformCadenceWindow = 0;
    uint64_t wgcDelayUniformHoldTotal = 0;
    uint32_t wgcDelayUniformHoldWindow = 0;
    // Reservoir depth-cap trims: surplus oldest frames the uniform-cadence pacer dropped to keep the
    // realized content delay from inflating when a VRR source transiently delivered above output.
    uint64_t wgcDelayPaceCapTrimTotal = 0;
    uint32_t wgcDelayPaceCapTrimWindow = 0;
    DWORD wgcDelayPaceCapTrimLastLogTick = 0;
    // Uniform-playout anti-freeze floor engagements: the encoder grid drifted so far behind wall-clock
    // that even the oldest buffered frame was too-new for the slot, and the oldest frame was old enough
    // to preserve the active content delay, so the target was raised to resume playout.
    uint64_t wgcUniformAntiFreezeFloorTotal = 0;
    uint64_t wgcUniformAntiFreezeFloorSkippedSyncTotal = 0;
    uint64_t wgcRetainedCapTrimTotal = 0;
    uint32_t wgcRetainedCapTrimWindow = 0;
    DWORD wgcRetainedCapTrimLastLogTick = 0;
    uint64_t wgcPoolPressureTrimTotal = 0;
    uint32_t wgcPoolPressureTrimWindow = 0;
    DWORD wgcPoolPressureTrimLastLogTick = 0;
    uint64_t wgcDelayOlderFrameAvoidedRepeatTotal = 0;
    uint32_t wgcDelayOlderFrameAvoidedRepeatWindow = 0;
    uint64_t wgcDelaySourceLimitedRepeatTotal = 0;
    uint32_t wgcDelaySourceLimitedRepeatWindow = 0;
    uint64_t wgcDelayRepeatRescueAttemptTotal = 0;
    uint32_t wgcDelayRepeatRescueAttemptWindow = 0;
    uint64_t wgcDelayRepeatRescueSuccessTotal = 0;
    uint32_t wgcDelayRepeatRescueSuccessWindow = 0;
    uint64_t wgcDelayRepeatRescueRejectedSyncTotal = 0;
    uint32_t wgcDelayRepeatRescueRejectedSyncWindow = 0;
    uint64_t wgcDelayRepeatRescueRejectedHeadroomTotal = 0;
    uint32_t wgcDelayRepeatRescueRejectedHeadroomWindow = 0;
    uint64_t wgcDelayRepeatRescueRejectedCostTotal = 0;
    uint32_t wgcDelayRepeatRescueRejectedCostWindow = 0;
    uint64_t wgcDelayRepeatPromotedBeforeRepeatTotal = 0;
    uint32_t wgcDelayRepeatPromotedBeforeRepeatWindow = 0;
    uint64_t wgcDelayRepeatPromotionAttemptTotal = 0;
    uint32_t wgcDelayRepeatPromotionAttemptWindow = 0;
    uint64_t wgcDelayRepeatPromotionRejectedSoftTotal = 0;
    uint32_t wgcDelayRepeatPromotionRejectedSoftWindow = 0;
    uint64_t wgcDelayRepeatSafeAfterPromotionTotal = 0;
    uint32_t wgcDelayRepeatSafeAfterPromotionWindow = 0;
    uint64_t wgcDelayRepeatWithSafeCandidateTotal = 0;
    uint32_t wgcDelayRepeatWithSafeCandidateWindow = 0;
    uint64_t wgcDelayRepeatWithoutSafeCandidateTotal = 0;
    uint32_t wgcDelayRepeatWithoutSafeCandidateWindow = 0;
    uint64_t wgcDelayRepeatWithSoftSafeCandidateTotal = 0;
    uint32_t wgcDelayRepeatWithSoftSafeCandidateWindow = 0;
    uint64_t wgcDelayRepeatWithoutSoftSafeCandidateTotal = 0;
    uint32_t wgcDelayRepeatWithoutSoftSafeCandidateWindow = 0;
    uint64_t wgcDelayRepeatHardOnlyCandidateTotal = 0;
    uint32_t wgcDelayRepeatHardOnlyCandidateWindow = 0;
    uint64_t wgcDelaySyncProtectedRepeatTotal = 0;
    uint32_t wgcDelaySyncProtectedRepeatWindow = 0;
    uint64_t wgcDelayWindowHealthyRepeatTotal = 0;
    uint32_t wgcDelayWindowHealthyRepeatWindow = 0;
    uint64_t wgcDelayWindowRecoverableRepeatTotal = 0;
    uint32_t wgcDelayWindowRecoverableRepeatWindow = 0;
    uint64_t wgcDelayWindowSourceLimitedRepeatTotal = 0;
    uint32_t wgcDelayWindowSourceLimitedRepeatWindow = 0;
    uint64_t wgcDelayWindowHardStallRepeatTotal = 0;
    uint32_t wgcDelayWindowHardStallRepeatWindow = 0;
    uint64_t wgcDelayWindowPostStallRepeatTotal = 0;
    uint32_t wgcDelayWindowPostStallRepeatWindow = 0;
    uint64_t wgcDelayPostStallSafeFrameTotal = 0;
    uint32_t wgcDelayPostStallSafeFrameWindow = 0;
    uint32_t wgcDelayRepeatReserveDepthMax = 0;
    uint32_t wgcDelayRepeatReserveDepthWindowMax = 0;
    uint32_t wgcDelayRepeatReserveSpanMaxUs = 0;
    uint32_t wgcDelayRepeatReserveSpanWindowMaxUs = 0;
    uint32_t wgcDelayOldestSoftSafeAgeMaxUs = 0;
    uint32_t wgcDelayOldestSoftSafeAgeWindowMaxUs = 0;
    uint64_t wgcDelayPostSelectionRejectedSyncRiskTotal = 0;
    uint32_t wgcDelayPostSelectionRejectedSyncRiskWindow = 0;
    uint64_t wgcDelayPostSelectionRescuedSyncRiskTotal = 0;
    uint32_t wgcDelayPostSelectionRescuedSyncRiskWindow = 0;
    uint64_t wgcDelayRepeatClusterPressureTotal = 0;
    uint32_t wgcDelayRepeatClusterPressureWindow = 0;
    uint32_t wgcDelayRepeatClusterPressureWindowMaxTicks = 0;
    uint32_t wgcDelayRepeatClusterPressureMaxTicks = 0;
    uint64_t wgcSourceRepeatLowerBoundTotal = 0;
    uint32_t wgcSourceRepeatLowerBoundWindow = 0;
    constexpr size_t kWgcRollingSourceWindowSlots = 5;
    std::array<uint32_t, kWgcRollingSourceWindowSlots> wgcRollingSourceAcceptedSlots{};
    std::array<uint32_t, kWgcRollingSourceWindowSlots> wgcRollingSourceCfrTickSlots{};
    size_t wgcRollingSourceSlotIndex = 0;
    size_t wgcRollingSourceSlotCount = 0;
    uint32_t wgcRollingSourceAcceptedSum = 0;
    uint32_t wgcRollingSourceCfrTickSum = 0;
    uint32_t wgcRollingSourceAcceptedWindow = 0;
    uint32_t wgcRollingSourceCfrTicksWindow = 0;
    uint32_t wgcRollingSourceDeficitFrames = 0;
    uint32_t wgcRollingSourceSurplusFrames = 0;
    uint64_t wgcRollingSourceAcceptedTotal = 0;
    uint64_t wgcRollingSourceCfrTickTotal = 0;
    uint32_t wgcRollingSourceLastIngressAccepted = 0;
    bool wgcRollingSourceWindowPrimed = false;
    uint64_t wgcExcessRepeatTotal = 0;
    uint32_t wgcExcessRepeatWindow = 0;
    uint64_t wgcPolicyAddedRepeatTotal = 0;
    uint32_t wgcPolicyAddedRepeatWindow = 0;
    uint64_t wgcExcessRepeatClusterTotal = 0;
    uint32_t wgcExcessRepeatClusterWindow = 0;
    uint32_t wgcExcessRepeatClusterMaxTicks = 0;
    uint32_t wgcExcessRepeatClusterWindowMaxTicks = 0;
    uint64_t wgcSyncDelaySourceRecoveryHoldTotal = 0;
    uint32_t wgcSyncDelaySourceRecoveryHoldCount = 0;
    uint64_t wgcActiveDelaySourceRecoveryUntilTick = 0;
    uint64_t wgcActiveDelaySourceRecoveryTicks = 0;
    uint32_t wgcProducerRateRetuneCount = 0;
    uint64_t wgcProducerRateRetuneTotal = 0;
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
    int64_t wgcWarmupUntilQpc = 0;
    int64_t wgcBiasAccumQpc = 0;
    uint32_t wgcBiasClampCount = 0;
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
    bool wgcActiveDelayRepeatClassKnown = false;
    ce::capture_policy::WgcActiveDelayWindowClass wgcActiveDelayLastRepeatClass =
        ce::capture_policy::WgcActiveDelayWindowClass::kHealthy;
    uint64_t wgcActiveDelayLastRepeatClassLogTick = 0;
    int64_t lastEmittedWgcSourceQpc = 0;
    // Selection-domain (smoothed) twin of lastEmittedWgcSourceQpc. The uniform
    // active-delay playout makes its emit/hold/drop decisions in the smoothed
    // selection-timestamp domain (strictly monotonic by construction), so its
    // monotonicity guard must compare in the same domain; raw stays in
    // lastEmittedWgcSourceQpc for the legacy path and sync diagnostics.
    int64_t lastEmittedWgcSelectionQpc = 0;
    int64_t lastEmittedInjectSourceQpc = 0;
    int64_t lastWarmupWgcSourceQpc = 0;
    int64_t wgcStartupBarrierQpc = 0;
    uint32_t wgcStartupBarrierDroppedFrames = 0;
    bool wgcStartupPreLiveDelayComplete = false;
    uint32_t wgcStartupPreLiveDelayDroppedFrames = 0;
    int64_t wgcAvSyncScheduleOffsetQpc = 0;
    int64_t wgcAvSyncStartupAudioAnchorQpc = 0;
    int64_t wgcAvSyncStartupVideoQpc = 0;
    int64_t wgcAvSyncStartupEffectiveDelayQpc = 0;
    int64_t wgcStartupReserveWaitStartQpc = 0;
    uint32_t wgcStartupReserveWaitCount = 0;
    int64_t wgcStartupReserveWaitInitialSpanUs = 0;
    uint32_t wgcStartupReserveWaitFreshenedMax = 0;
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
        uint32_t startPoolSaturatedDrops = 0;
        uint32_t startPoolOverwritePrevented = 0;
        uint32_t startIngressDecimated = 0;
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
        // Longest CONTIGUOUS duplicate (held-frame) run for the whole session -- the true visible
        // freeze duration. Unlike the per-window cadence DupStreak, this survives the per-second
        // cadence reset, so a freeze that crosses a window boundary is not split/undercounted.
        // (longestStarvedEpisode* above measure a below-target *episode* and dups *within* it, which
        // overstate a freeze because the source still delivers new frames during the episode.)
        uint64_t longestContiguousDupTicks = 0;
        uint32_t currentContiguousDupTicks = 0;  // running counter (per-tick), reset on any fresh frame
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
        uint32_t worstOneSecondEmitCount = 0;
        uint32_t worstOneSecondUniqueCount = 0;
        uint32_t worstOneSecondRepeatCount = 0;
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
            if (cadenceCounters.liveTickEmitCount > 0 &&
                (cadenceCounters.liveTickDuplicateCount > captureSessionSummary.worstOneSecondRepeatCount ||
                 (cadenceCounters.liveTickDuplicateCount == captureSessionSummary.worstOneSecondRepeatCount &&
                  (captureSessionSummary.worstOneSecondEmitCount == 0 ||
                   cadenceCounters.liveTickUniqueCount < captureSessionSummary.worstOneSecondUniqueCount)))) {
                captureSessionSummary.worstOneSecondEmitCount = cadenceCounters.liveTickEmitCount;
                captureSessionSummary.worstOneSecondUniqueCount = cadenceCounters.liveTickUniqueCount;
                captureSessionSummary.worstOneSecondRepeatCount = cadenceCounters.liveTickDuplicateCount;
            }
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
        const uint32_t frameBudgetUs = static_cast<uint32_t>(std::max(1.0, frameIntervalMs * 1000.0));
        const bool muxPressure =
            wgcStarvedEpisode.maxMuxBackpressureCount > 0 || wgcStarvedEpisode.maxMuxBackpressureWaitUs > 0;
        const bool capacityPressure = wgcStarvedEpisode.peakOverloadFlags != 0 || muxPressure;
        const bool sourceBelowCfrTarget = minInputFps > 0 && minInputFps < targetOutputFps;
        const bool deliveredBelowCfrTarget = minDeliveredFps > 0 && minDeliveredFps < targetOutputFps;
        const bool callbackDeliveryGap = wgcStarvedEpisode.maxCallbackGapUs > frameBudgetUs * 2u;
        uint32_t poolSaturatedDrops = 0;
        uint32_t poolOverwritePrevented = 0;
        uint32_t ingressDecimated = 0;
        if (g_WgcCap) {
            const uint32_t currentPoolSaturatedDrops = g_WgcCap->GetPoolSaturatedDropCount();
            const uint32_t currentPoolOverwritePrevented = g_WgcCap->GetPoolSlotOverwritePreventedCount();
            const uint32_t currentIngressDecimated = g_WgcCap->GetIngressDecimatedCount();
            poolSaturatedDrops = currentPoolSaturatedDrops - wgcStarvedEpisode.startPoolSaturatedDrops;
            poolOverwritePrevented = currentPoolOverwritePrevented - wgcStarvedEpisode.startPoolOverwritePrevented;
            ingressDecimated = currentIngressDecimated - wgcStarvedEpisode.startIngressDecimated;
        }
        const bool wgcFramepoolPressure = poolSaturatedDrops > 0 || poolOverwritePrevented > 0 || ingressDecimated > 0;
        const bool wgcDeliveryGap = deliveredBelowCfrTarget || callbackDeliveryGap;
        const char* faultHint = capacityPressure       ? "ce_capacity_pressure"
                                : wgcFramepoolPressure ? "wgc_framepool_pressure"
                                : sourceBelowCfrTarget ? "source_below_cfr_target"
                                : wgcDeliveryGap       ? "wgc_delivery_gap"
                                                       : "source_starved";
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
                "fenceMax=%uus fenceHealth=%s poolSat=%u overwritePrevented=%u ingressDecimated=%u",
                faultHint, static_cast<long long>(wgcStarvedEpisode.startQpc), static_cast<long long>(endQpc.QuadPart),
                static_cast<unsigned long long>(durationMs), static_cast<unsigned long long>(outputTicks),
                static_cast<unsigned long long>(duplicateTicks), minInputFps, minDeliveredFps,
                wgcStarvedEpisode.peakFreshMissPermille, minBufferedFrames, wgcStarvedEpisode.maxCallbackGapUs,
                wgcStarvedEpisode.maxEncodeEmaMs, wgcStarvedEpisode.maxMuxBackpressureCount,
                wgcStarvedEpisode.maxMuxBackpressureWaitUs, wgcStarvedEpisode.maxMuxQueueKb,
                wgcStarvedEpisode.peakOverloadFlags, wgcStarvedEpisode.maxCopyUs, copySlow ? "slow" : "ok",
                wgcStarvedEpisode.maxFenceUs, fenceSlow ? "slow" : "ok", poolSaturatedDrops, poolOverwritePrevented,
                ingressDecimated);
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
    auto ResetWarmupWgcFreshness = [&](bool resetStartupDiagnostics = true) {
        lastWarmupWgcSourceQpc = 0;
        wgcStartupBarrierQpc = 0;
        wgcStartupBarrierDroppedFrames = 0;
        wgcStartupPreLiveDelayComplete = false;
        wgcStartupPreLiveDelayDroppedFrames = 0;
        wgcStartupReserveWaitStartQpc = 0;
        wgcStartupReserveWaitCount = 0;
        wgcStartupReserveWaitInitialSpanUs = 0;
        wgcStartupReserveWaitFreshenedMax = 0;
        wgcFreshWarmupFrameCount = 0;
        if (resetStartupDiagnostics) {
            wgcAvSyncScheduleOffsetQpc = 0;
            wgcAvSyncStartupAudioAnchorQpc = 0;
            wgcAvSyncStartupVideoQpc = 0;
            wgcAvSyncStartupEffectiveDelayQpc = 0;
            wgcSmoothnessActiveDelayQpc = 0;
            pendingWgcStartContract = {};
            pendingWgcStartContractGeneration = 0;
            committedWgcStartContractGeneration = 0;
            wgcEncoderPrewarmAttempted = false;
            wgcEncoderPrewarmSucceeded = false;
            wgcEncoderPrewarmElapsedUs = 0;
            wgcSmoothnessFloorDelayQpc = 0;
            wgcSmoothnessFloorRequestedQpc = 0;
            wgcSmoothnessFloorSource = "off";
            wgcSmoothnessFloorClampedBy = "none";
            wgcSmoothnessFloorLogged = false;
            wgcSmoothnessFloorJitter = ce::capture_policy::WgcSmoothnessFloorJitter{};
            wgcSmoothnessDesiredFrames = 0;
            wgcSmoothnessRetainedFrames = 0;
            wgcSmoothnessActualFrames = 0;
            wgcSmoothnessPoolSlots = 0;
            wgcSmoothnessRetainedFrameCap = 0;
            wgcSmoothnessReservedFreeSlots = 0;
            wgcSmoothnessEstimatedVramBytes = 0;
            wgcSmoothnessCapLimited = false;
            wgcSmoothnessBufferReason = "not-run";
        }
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
    // Smoothness FLOOR (WGC only): engage the active-delay jitter-absorbing playout even when there
    // is no audio-latency content delay. Configured = auto or explicit > 0; only meaningful for the
    // WGC (screen-grab) path. wgcSmoothnessDelayDesired drives the smoothness arming gates below in
    // place of avContentDelayActive, so the buffer arms for video-only / low-confidence captures.
    // The resolved floor magnitude (wgcSmoothnessFloorDelayQpc) is derived once at the startup
    // barrier from measured delivery jitter; here we only know whether it is configured.
    const bool wgcSmoothnessFloorConfigured = IsActiveScreenGrab() && config.wgcSmoothnessBufferEnabled &&
                                              !config.video.useVFR &&
                                              (config.wgcSmoothnessFloorAuto || config.wgcSmoothnessFloorMs > 0);
    const bool wgcSmoothnessDelayDesired =
        ce::capture_policy::WgcSmoothnessDelayDesired(avContentDelayActive, wgcSmoothnessFloorConfigured);
    // Inject path has no selection target; it pops the oldest buffered frame above a reserve,
    // so delaying inject video content = retaining this many extra frames (the oldest popped
    // frame becomes ~L old). Rounded up to whole frames.
    const size_t injectContentDelayFrames =
        (avContentDelayActive && frameIntervalMs > 0.0)
            ? static_cast<size_t>(std::ceil(static_cast<double>(maxAudioCaptureLatencyMs) / frameIntervalMs))
            : 0;
    const double injectResidualEstimateMs = (injectContentDelayFrames > 0 && frameIntervalMs > 0.0)
                                                ? static_cast<double>(injectContentDelayFrames) * frameIntervalMs -
                                                      static_cast<double>(maxAudioCaptureLatencyMs)
                                                : 0.0;
    const double avContentDelayFrames =
        (avContentDelayActive && frameIntervalMs > 0.0) ? maxAudioCaptureLatencyMs / frameIntervalMs : 0.0;
    if (avContentDelayActive) {
        LogInfo(
            "[AVSyncApply] armed: maxAudioCaptureLatencyMs=%.3f delayUs=%lld method=%s injectDelayFrames=%zu "
            "videoDelayFrames=%.2f residualEstimateMs=%.3f confidence=%s reason=%s "
            "(delays video content to match late loopback audio; audio/PTS unchanged)",
            static_cast<double>(maxAudioCaptureLatencyMs),
            (long long)((avContentDelayQpc * 1000000) / qpcFreq.QuadPart),
            IsActiveScreenGrab() ? "wgc-selection-bias" : "inject-buffer-reserve", injectContentDelayFrames,
            avContentDelayFrames, IsActiveScreenGrab() ? 0.0 : injectResidualEstimateMs,
            config.avSyncConfidence.c_str(), config.avSyncReason.c_str());
        if (IsActiveScreenGrab()) {
            LogInfo(
                "[AVSyncApply] wgc_cadence_policy: uniformCadence=%d (when active, WGC paces the content "
                "delay like the inject path: a delay-deep frame floor with unique frames advanced at the "
                "source input rate, so a VRR/under-delivering source keeps the realized delay stable and "
                "the source-limited repeats uniform instead of clustering into delay-slot holds)",
                config.wgcActiveDelayUniformCadence ? 1 : 0);
        }
    } else {
        LogWarn(
            "[AVSyncApply] inactive: maxAudioCaptureLatencyMs=%.3f delayUs=0 method=%s injectDelayFrames=0 "
            "residualEstimateMs=0.000 confidence=%s reason=%s",
            static_cast<double>(maxAudioCaptureLatencyMs),
            IsActiveScreenGrab() ? "wgc-selection-bias" : "inject-buffer-reserve", config.avSyncConfidence.c_str(),
            config.avSyncReason.c_str());
    }

    const auto qpcToUs = [&](int64_t qpcDelta) -> int64_t {
        return qpcFreq.QuadPart > 0 ? (qpcDelta * 1000000) / qpcFreq.QuadPart : 0;
    };
    const auto observeCaptureSyncPhaseSource = [&](const char* backend,
                                                    ce::capture_policy::CfrCadencePhaseLockState& state,
                                                    int64_t sourceTimestampQpc) {
        if (!captureSyncPhaseLockEnabled) {
            return;
        }
        const uint64_t releasesBefore = state.releases;
        ce::capture_policy::ObserveCfrCaptureSyncSourceTimestamp(state, sourceTimestampQpc,
                                                                 captureSyncSourceIntervalTicks);
        if (state.releases != releasesBefore) {
            LogInfo(
                "[CFR PhaseLock] backend=%s state=released reason=variable_source stable=%u unstable=%u "
                "multiplier=%u releases=%llu",
                backend, state.stableSourceIntervals, state.unstableSourceIntervals, captureSyncMultiplier,
                static_cast<unsigned long long>(state.releases));
        }
    };
    const auto applyCaptureSyncPhaseTarget = [&](const char* backend,
                                                 ce::capture_policy::CfrCadencePhaseLockState& state,
                                                 int64_t baseTargetQpc, int64_t sourceReferenceQpc) -> int64_t {
        const uint64_t acquisitionsBefore = state.acquisitions;
        const uint64_t releasesBefore = state.releases;
        const uint64_t rephasesBefore = state.rephases;
        const int64_t adjustedTargetQpc = ce::capture_policy::ApplyCfrCaptureSyncPhaseLock(
            state, baseTargetQpc, sourceReferenceQpc, captureSyncSourceIntervalTicks,
            captureSyncPhaseLockEnabled);
        if (state.acquisitions != acquisitionsBefore || state.releases != releasesBefore ||
            state.rephases != rephasesBefore) {
            const char* transition = state.acquisitions != acquisitionsBefore
                                         ? "acquired"
                                         : (state.rephases != rephasesBefore ? "rephased" : "released");
            LogInfo(
                "[CFR PhaseLock] backend=%s state=%s offset=%lldus stable=%u unstable=%u multiplier=%u "
                "transitions=%llu/%llu/%llu",
                backend, transition, static_cast<long long>(qpcToUs(state.lockedPhaseQpc)),
                state.stableSourceIntervals, state.unstableSourceIntervals, captureSyncMultiplier,
                static_cast<unsigned long long>(state.acquisitions),
                static_cast<unsigned long long>(state.rephases), static_cast<unsigned long long>(state.releases));
        }
        return adjustedTargetQpc;
    };
    const auto getWgcRawSelectionTimestamp = [](const QueuedFrame& frame) -> int64_t {
        return frame.rawTimestamp > 0 ? frame.rawTimestamp : frame.timestamp;
    };
    const auto getWgcEffectiveContentDelayQpc = [&]() -> int64_t {
        return avContentDelayQpc + std::max<int64_t>(0, wgcSmoothnessActiveDelayQpc);
    };
    const auto isWgcEffectiveContentDelayActive = [&]() -> bool { return getWgcEffectiveContentDelayQpc() > 0; };
    const auto getWgcSmoothnessOutputFps = [&]() -> uint32_t {
        return config.video.fps > 0 ? static_cast<uint32_t>(config.video.fps) : 0u;
    };
    const auto shouldUseWgcSmoothnessBaseConfig = [&]() -> bool {
        // Pass wgcSmoothnessDelayDesired (audio-latency delay OR configured floor) so the buffer
        // arms for video-only / low-confidence captures too, not only when audio latency is present.
        return ce::capture_policy::ShouldUseWgcSmoothnessBuffer(config.wgcSmoothnessBufferEnabled, config.video.useVFR,
                                                                wgcSmoothnessDelayDesired, targetIntervalTicks);
    };
    const auto getWgcSmoothnessDesiredFramesForConfig = [&]() -> uint32_t {
        if (!shouldUseWgcSmoothnessBaseConfig()) {
            return 0u;
        }
        return ce::capture_policy::GetWgcSmoothnessDesiredFrames(getWgcSmoothnessOutputFps(),
                                                                 config.wgcSmoothnessBufferMaxMs);
    };
    const auto getWgcSmoothnessRetainedFramesBudget = [&]() -> uint32_t {
        const uint32_t desiredFrames = getWgcSmoothnessDesiredFramesForConfig();
        return (g_WgcCap && desiredFrames > 0) ? g_WgcCap->GetSmoothnessRetainedFrameCount() : 0u;
    };
    const auto isWgcSmoothnessSourceRateEligibleNow = [&]() -> bool {
        if (!ce::capture_policy::ShouldUseWgcSmoothnessBuffer(config.wgcSmoothnessBufferEnabled, config.video.useVFR,
                                                              wgcSmoothnessDelayDesired, targetIntervalTicks)) {
            return false;
        }
        const uint32_t inputMin250Fps = g_WgcCap ? g_WgcCap->GetInputMin250Fps() : wgcRecentInputMin250Fps;
        const uint32_t inputMin500Fps = g_WgcCap ? g_WgcCap->GetInputMin500Fps() : wgcRecentInputMin500Fps;
        return ce::capture_policy::ShouldArmWgcSmoothnessBufferForSourceRate(getWgcSmoothnessOutputFps(),
                                                                             inputMin250Fps, inputMin500Fps);
    };
    const auto shouldAttemptWgcStartupSmoothnessBufferNow = [&]() -> bool {
        return ce::capture_policy::ShouldAttemptWgcStartupSmoothnessBuffer(
            config.wgcSmoothnessBufferEnabled, config.video.useVFR, wgcSmoothnessDelayDesired, targetIntervalTicks,
            getWgcSmoothnessRetainedFramesBudget());
    };
    const auto getWgcSmoothnessBufferReason = [&]() -> const char* {
        if (!config.wgcSmoothnessBufferEnabled) {
            return "disabled";
        }
        if (config.video.useVFR) {
            return "vfr";
        }
        if (!wgcSmoothnessDelayDesired) {
            return "sync_delay_inactive";
        }
        if (targetIntervalTicks <= 0) {
            return "invalid_target";
        }
        const uint32_t desiredFrames = getWgcSmoothnessDesiredFramesForConfig();
        if (desiredFrames == 0) {
            return "target_zero";
        }
        const uint32_t retainedFrames = getWgcSmoothnessRetainedFramesBudget();
        if (retainedFrames == 0) {
            return "vram_budget_exhausted";
        }
        if (!isWgcSmoothnessSourceRateEligibleNow()) {
            return "startup_attempt_source_rate_low";
        }
        return "startup_attempt";
    };
    const auto getWgcDelayReservoirLowWaterFramesForDelay = [&](int64_t delayQpc) -> uint32_t {
        return ce::capture_policy::GetWgcDelayReservoirLowWaterFrames(delayQpc, targetIntervalTicks);
    };
    const auto getWgcDelayReservoirTargetFramesForDelay = [&](int64_t delayQpc) -> uint32_t {
        return ce::capture_policy::GetWgcDelayReservoirTargetFrames(delayQpc, targetIntervalTicks);
    };
    const auto getWgcDelayReservoirLowWaterFrames = [&]() -> uint32_t {
        return getWgcDelayReservoirLowWaterFramesForDelay(getWgcEffectiveContentDelayQpc());
    };
    const auto getWgcDelayReservoirTargetFrames = [&]() -> uint32_t {
        return getWgcDelayReservoirTargetFramesForDelay(getWgcEffectiveContentDelayQpc());
    };
    const auto getWgcRetainedFrameCap = [&]() -> uint32_t {
        if (!g_WgcCap) {
            return 0u;
        }
        return g_WgcCap->GetSmoothnessRetainedFrameCap();
    };
    const auto updateWgcIngressPressure = [&](const char*) {
        if (!g_WgcCap) {
            return;
        }
        const uint32_t retainedCap = getWgcRetainedFrameCap();
        const uint32_t retainedFrames = SaturatingToUint32(
            static_cast<uint64_t>(bufferedWgcFrames.size()) +
            static_cast<uint64_t>(std::min<size_t>(g_FrameQueue.Size(), static_cast<size_t>(UINT32_MAX))));
        const uint32_t lowWaterFrames = getWgcDelayReservoirLowWaterFrames();
        const bool delayReservoirActive = lowWaterFrames > 0;
        const bool recovering = delayReservoirActive && (wgcLowSourceModeActive || wgcLiveRecoveryModeActive ||
                                                         (wgcActiveDelaySourceRecoveryUntilTick > GetTickCount64()) ||
                                                         retainedFrames <= lowWaterFrames);
        const bool uniformPlayoutOwnsSurplus =
            isWgcEffectiveContentDelayActive() && config.wgcActiveDelayUniformCadence;
        g_WgcCap->SetRetainedFramePressure(retainedFrames, retainedCap, lowWaterFrames, recovering,
                                           uniformPlayoutOwnsSurplus);
    };
    const auto trimBufferedWgcToRetainedCap = [&](const char* reason) {
        const uint32_t retainedCap = getWgcRetainedFrameCap();
        if (retainedCap == 0) {
            updateWgcIngressPressure(reason);
            return 0u;
        }
        uint32_t trimmed = 0;
        while (bufferedWgcFrames.size() > retainedCap) {
            QueuedFrame surplus = std::move(bufferedWgcFrames.back());
            bufferedWgcFrames.pop_back();
            ReleaseQueuedFrameTexture(surplus);
            ++trimmed;
        }
        if (trimmed > 0) {
            wgcRetainedCapTrimTotal += trimmed;
            wgcRetainedCapTrimWindow += trimmed;
            const DWORD nowTick = GetTickCount();
            if (trimmed >= 3 || nowTick - wgcRetainedCapTrimLastLogTick >= 1000) {
                LogInfo(
                    "[WGC CFR] retained reservoir capped: trimmedNewest=%u reason=%s retained=%zu cap=%u "
                    "reservedFree=%u poolSlots=%u (pool safety protected; surplus source frames become planned CFR "
                    "decimation/repeats, audio/PTS unchanged)",
                    trimmed, reason ? reason : "unknown", bufferedWgcFrames.size(), retainedCap,
                    g_WgcCap ? g_WgcCap->GetSmoothnessReservedFreeSlotCount() : 0u,
                    g_WgcCap ? g_WgcCap->GetTexturePoolSlotCount() : 0u);
                wgcRetainedCapTrimLastLogTick = nowTick;
            }
        }
        updateWgcIngressPressure(reason);
        return trimmed;
    };
    const auto trimBufferedWgcStartupWaitToRetainedCap = [&](const char* reason) {
        const uint32_t retainedCap = getWgcRetainedFrameCap();
        if (retainedCap == 0) {
            updateWgcIngressPressure(reason);
            return 0u;
        }
        uint32_t trimmed = 0;
        while (bufferedWgcFrames.size() > retainedCap) {
            QueuedFrame surplus = std::move(bufferedWgcFrames.front());
            bufferedWgcFrames.pop_front();
            ReleaseQueuedFrameTexture(surplus);
            ++trimmed;
        }
        if (trimmed > 0) {
            wgcRetainedCapTrimTotal += trimmed;
            wgcRetainedCapTrimWindow += trimmed;
            LogInfo(
                "[WGC CFR] startup wait retained reservoir capped: trimmedOldest=%u reason=%s retained=%zu cap=%u "
                "reservedFree=%u poolSlots=%u",
                trimmed, reason ? reason : "startup-wait", bufferedWgcFrames.size(), retainedCap,
                g_WgcCap ? g_WgcCap->GetSmoothnessReservedFreeSlotCount() : 0u,
                g_WgcCap ? g_WgcCap->GetTexturePoolSlotCount() : 0u);
        }
        updateWgcIngressPressure(reason);
        return trimmed;
    };
    const auto trimBufferedWgcForPoolPressure = [&](const char* reason) {
        if (!g_WgcCap) {
            updateWgcIngressPressure(reason);
            return 0u;
        }
        const uint32_t retainedCap = getWgcRetainedFrameCap();
        const uint32_t reservedFreeSlots = g_WgcCap->GetSmoothnessReservedFreeSlotCount();
        const uint32_t currentFreeSlots = g_WgcCap->GetPoolSlotFreeCurrentCount();
        const uint32_t trimTarget = ce::capture_policy::GetWgcPoolPressureRetainedTrimTarget(
            currentFreeSlots, reservedFreeSlots, getWgcDelayReservoirTargetFrames(), retainedCap);
        if (trimTarget == 0 || trimTarget >= retainedCap || bufferedWgcFrames.size() <= trimTarget) {
            updateWgcIngressPressure(reason);
            return 0u;
        }

        uint32_t trimmed = 0;
        while (bufferedWgcFrames.size() > trimTarget) {
            QueuedFrame surplus = std::move(bufferedWgcFrames.back());
            bufferedWgcFrames.pop_back();
            ReleaseQueuedFrameTexture(surplus);
            ++trimmed;
        }
        if (trimmed > 0) {
            wgcRetainedCapTrimTotal += trimmed;
            wgcRetainedCapTrimWindow += trimmed;
            wgcPoolPressureTrimTotal += trimmed;
            wgcPoolPressureTrimWindow += trimmed;
            const DWORD nowTick = GetTickCount();
            if (trimmed >= 2 || nowTick - wgcPoolPressureTrimLastLogTick >= 1000) {
                LogInfo(
                    "[WGC CFR] retained reservoir pressure trim: trimmedNewest=%u reason=%s retained=%zu "
                    "target=%u cap=%u free=%u reservedFree=%u poolSlots=%u delayTarget=%u "
                    "(preserved active-delay target; released surplus copy-pool leases, audio/PTS unchanged)",
                    trimmed, reason ? reason : "pool-pressure", bufferedWgcFrames.size(), trimTarget, retainedCap,
                    currentFreeSlots, reservedFreeSlots, g_WgcCap->GetTexturePoolSlotCount(),
                    getWgcDelayReservoirTargetFrames());
                wgcPoolPressureTrimLastLogTick = nowTick;
            }
        }
        updateWgcIngressPressure(reason);
        return trimmed;
    };
    const auto recordWgcDelayResidualSample =
        [&](int64_t signedResidualUs, uint64_t& samples, uint64_t& absAccumUs, int64_t& signedAccumUs,
            uint32_t& absMaxUs, uint32_t& lateMaxUs, uint32_t& earlyMaxUs, std::array<uint32_t, 256>& histogram,
            uint64_t& windowSamples, uint64_t& windowAbsAccumUs, int64_t& windowSignedAccumUs, uint32_t& windowAbsMaxUs,
            uint32_t& windowLateMaxUs, std::array<uint32_t, 256>& windowHistogram) {
            const uint32_t absResidualUs =
                SaturatingToUint32(static_cast<uint64_t>(signedResidualUs >= 0 ? signedResidualUs : -signedResidualUs));
            ++samples;
            absAccumUs += absResidualUs;
            signedAccumUs += signedResidualUs;
            absMaxUs = std::max(absMaxUs, absResidualUs);
            if (signedResidualUs >= 0) {
                lateMaxUs = std::max(lateMaxUs, SaturatingToUint32(static_cast<uint64_t>(signedResidualUs)));
            } else {
                earlyMaxUs = std::max(earlyMaxUs, SaturatingToUint32(static_cast<uint64_t>(-signedResidualUs)));
            }
            const size_t histogramBin = std::min<size_t>(histogram.size() - 1, absResidualUs / 1000u);
            ++histogram[histogramBin];
            ++windowSamples;
            windowAbsAccumUs += absResidualUs;
            windowSignedAccumUs += signedResidualUs;
            windowAbsMaxUs = std::max(windowAbsMaxUs, absResidualUs);
            if (signedResidualUs >= 0) {
                windowLateMaxUs =
                    std::max(windowLateMaxUs, SaturatingToUint32(static_cast<uint64_t>(signedResidualUs)));
            }
            ++windowHistogram[histogramBin];
        };
    const auto recordWgcDelayRealization = [&](int64_t predictedSignedResidualUs, int64_t rawSignedResidualUs) -> bool {
        if (!isWgcEffectiveContentDelayActive() || qpcFreq.QuadPart <= 0) {
            return false;
        }
        const int64_t requestedDelayUs = qpcToUs(getWgcEffectiveContentDelayQpc());
        const int64_t realizedDelaySignedUs = requestedDelayUs - predictedSignedResidualUs;
        const uint32_t realizedDelayUs =
            SaturatingToUint32(static_cast<uint64_t>(realizedDelaySignedUs > 0 ? realizedDelaySignedUs : 0));

        wgcDelayRealizedAccumUs += realizedDelayUs;
        wgcDelayRealizedMinUs = std::min(wgcDelayRealizedMinUs, realizedDelayUs);
        wgcDelayRealizedMaxUs = std::max(wgcDelayRealizedMaxUs, realizedDelayUs);
        recordWgcDelayResidualSample(predictedSignedResidualUs, wgcDelayResidualSamples, wgcDelayResidualAbsAccumUs,
                                     wgcDelayResidualSignedAccumUs, wgcDelayResidualAbsMaxUs, wgcDelayResidualLateMaxUs,
                                     wgcDelayResidualEarlyMaxUs, wgcDelayResidualAbsHistogram,
                                     wgcDelayResidualWindowSamples, wgcDelayResidualWindowAbsAccumUs,
                                     wgcDelayResidualWindowSignedAccumUs, wgcDelayResidualWindowAbsMaxUs,
                                     wgcDelayResidualWindowLateMaxUs, wgcDelayResidualWindowAbsHistogram);
        recordWgcDelayResidualSample(rawSignedResidualUs, wgcDelayRawResidualSamples, wgcDelayRawResidualAbsAccumUs,
                                     wgcDelayRawResidualSignedAccumUs, wgcDelayRawResidualAbsMaxUs,
                                     wgcDelayRawResidualLateMaxUs, wgcDelayRawResidualEarlyMaxUs,
                                     wgcDelayRawResidualAbsHistogram, wgcDelayRawResidualWindowSamples,
                                     wgcDelayRawResidualWindowAbsAccumUs, wgcDelayRawResidualWindowSignedAccumUs,
                                     wgcDelayRawResidualWindowAbsMaxUs, wgcDelayRawResidualWindowLateMaxUs,
                                     wgcDelayRawResidualWindowAbsHistogram);
        const int64_t rawMinusPredictedUs = rawSignedResidualUs - predictedSignedResidualUs;
        const uint32_t rawMinusPredictedAbsUs = SaturatingToUint32(
            static_cast<uint64_t>(rawMinusPredictedUs >= 0 ? rawMinusPredictedUs : -rawMinusPredictedUs));
        ++wgcDelayRawMinusPredictedSamples;
        wgcDelayRawMinusPredictedSignedAccumUs += rawMinusPredictedUs;
        wgcDelayRawMinusPredictedAbsMaxUs = std::max(wgcDelayRawMinusPredictedAbsMaxUs, rawMinusPredictedAbsUs);
        ++wgcDelayRawMinusPredictedWindowSamples;
        wgcDelayRawMinusPredictedWindowSignedAccumUs += rawMinusPredictedUs;
        wgcDelayRawMinusPredictedWindowAbsMaxUs =
            std::max(wgcDelayRawMinusPredictedWindowAbsMaxUs, rawMinusPredictedAbsUs);
        return true;
    };
    const auto wgcDelayResidualHistogramP95Us = [](const std::array<uint32_t, 256>& histogram,
                                                   uint64_t samples) -> uint32_t {
        if (samples == 0) {
            return 0;
        }
        const uint64_t targetRank = (samples * 95ull + 99ull) / 100ull;
        uint64_t cumulative = 0;
        for (size_t i = 0; i < histogram.size(); ++i) {
            cumulative += histogram[i];
            if (cumulative >= targetRank) {
                return SaturatingToUint32(static_cast<uint64_t>(i) * 1000ull);
            }
        }
        return SaturatingToUint32(static_cast<uint64_t>(histogram.size() - 1) * 1000ull);
    };
    const auto wgcDelayResidualP95Us = [&]() -> uint32_t {
        return wgcDelayResidualHistogramP95Us(wgcDelayResidualAbsHistogram, wgcDelayResidualSamples);
    };
    const auto wgcDelayResidualWindowP95Us = [&]() -> uint32_t {
        return wgcDelayResidualHistogramP95Us(wgcDelayResidualWindowAbsHistogram, wgcDelayResidualWindowSamples);
    };
    const auto wgcDelayRawResidualP95Us = [&]() -> uint32_t {
        return wgcDelayResidualHistogramP95Us(wgcDelayRawResidualAbsHistogram, wgcDelayRawResidualSamples);
    };
    const auto wgcDelayRawResidualWindowP95Us = [&]() -> uint32_t {
        return wgcDelayResidualHistogramP95Us(wgcDelayRawResidualWindowAbsHistogram, wgcDelayRawResidualWindowSamples);
    };

    while (g_EncoderRunning || g_DrainOutstandingCfrTicks.load(std::memory_order_acquire) || g_FrameQueue.Size() > 0 ||
           !bufferedWgcFrames.empty() || !bufferedInjectFrames.empty()) {
        LARGE_INTEGER cycleStartQpc;
        const uint64_t cycleLiveTicksOutputStart = liveTicksOutput;
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
        const uint64_t currentWgcSourceEpoch = g_WgcSourceEpoch.load(std::memory_order_acquire);
        if (activeScreenGrab && currentWgcSourceEpoch != observedWgcSourceEpoch) {
            size_t bufferedDiscarded = 0;
            for (auto it = bufferedWgcFrames.begin(); it != bufferedWgcFrames.end();) {
                if (it->wgcSourceEpoch != currentWgcSourceEpoch) {
                    ReleaseQueuedFrameTexture(*it);
                    it = bufferedWgcFrames.erase(it);
                    ++bufferedDiscarded;
                } else {
                    ++it;
                }
            }
            const size_t queuedDiscarded = g_FrameQueue.DiscardWgcEpochNotEqual(currentWgcSourceEpoch);
            if (g_HasLastFrame && !g_LastFrame.isInjectMode && g_LastFrame.wgcSourceEpoch != currentWgcSourceEpoch) {
                ResetLastQueuedFrameCache();
            }
            observedWgcSourceEpoch = currentWgcSourceEpoch;
            lastEmittedWgcSourceQpc = 0;
            lastEmittedWgcSelectionQpc = 0;
            lastWarmupWgcSourceQpc = 0;
            wgcInputPredictor.Reset();
            wgcCfrPhaseLock.Reset();
            wgcRecentDeliveredFps = 0;
            wgcRecentDeliveredMin250Fps = 0;
            wgcRecentDeliveredMin500Fps = 0;
            wgcRecentInputMin250Fps = 0;
            wgcRecentInputMin500Fps = 0;
            wgcLowSourceModeActive = false;
            wgcLiveRecoveryModeActive = false;
            wgcSourceStarvedCurrent = false;
            lastSuccessfulWgcCursorEmbedded = false;
            hasSuccessfulWgcCursorMetadata = false;
            if (MediaEngine_ResetRepeatFrameCache) {
                MediaEngine_ResetRepeatFrameCache();
            }
            ResetDuplicationCursorSuppression("WGC source epoch change");
            LogInfo(
                "[EncoderThread] WGC source epoch changed: epoch=%llu bufferedDiscarded=%zu queuedDiscarded=%zu; "
                "selection/cursor lineage rebased without changing the audio or CFR timeline",
                static_cast<unsigned long long>(currentWgcSourceEpoch), bufferedDiscarded, queuedDiscarded);
        } else if (!activeScreenGrab && currentWgcSourceEpoch != observedWgcSourceEpoch) {
            // Standby WGC retargets are unrelated to the authoritative inject
            // pixels. Observe their publication epoch now so activating the
            // already-proven standby source does not later invalidate the
            // inject repeat fallback at the handoff boundary.
            observedWgcSourceEpoch = currentWgcSourceEpoch;
            LogInfo("[EncoderThread] Observed standby WGC source epoch %llu while inject remained active",
                    static_cast<unsigned long long>(currentWgcSourceEpoch));
        }
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
            if (wgcWarmupUntilQpc > 0 && liveNowQpc < wgcWarmupUntilQpc) {
                return 0;
            }
            const bool encoderLimitedSmoothnessMode = isWgcEncoderLimitedSmoothnessMode();
            const int64_t intentionalContentDelayQpc = getWgcEffectiveContentDelayQpc();
            const int64_t visualDebtFloorQpc = ce::capture_policy::GetWgcLiveVisualDebtFloorQpcForMode(
                liveNowQpc, targetIntervalTicks, qpcFreq.QuadPart, encoderLimitedSmoothnessMode,
                intentionalContentDelayQpc);
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
                        "liveNowQpc=%lld contentDelay=%lldus maxDebt=%lluus remaining=%zu shortfall=%u",
                        reason ? reason : "unknown", encoderLimitedSmoothnessMode ? "encoder_limited" : "bounded_live",
                        dropped, static_cast<long long>(visualDebtFloorQpc), static_cast<long long>(liveNowQpc),
                        static_cast<long long>(qpcToUs(intentionalContentDelayQpc)),
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
        const bool recordingActive = g_Recording.load(std::memory_order_acquire);
        const bool drainOutstandingCfrTicks = g_DrainOutstandingCfrTicks.load(std::memory_order_acquire);
        if (ce::capture_policy::ShouldAbortCfrStopDrainBeforeOutputIsLive(recordingActive, recordingOutputLive,
                                                                          drainOutstandingCfrTicks)) {
            LogWarn(
                "[EncoderThread] CFR stop drain skipped before first live video frame; no output timeline or "
                "captured frame exists to drain");
            g_DrainOutstandingCfrTicks.store(false, std::memory_order_release);
        }
        if (!recordingActive && recordingOutputLive && drainOutstandingCfrTicks) {
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
            injectEncoderServiceTooSlowCurrent = ce::capture_policy::IsEncoderTooSlowForTargetFps(
                std::max(smoothedEncodeMs, smoothedInjectServiceMs), frameIntervalMs, targetOutputFpsForPolicy);
            const bool encoderCatchupBottleneckedCurrent =
                encoderTooSlowForTargetCurrent || g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
            const bool nextInjectCfrRecoveryActive = ce::capture_policy::GetInjectCfrRecoveryActive(
                injectCfrRecoveryActive, recordingOutputLive && !activeScreenGrab, config.video.useVFR,
                outputShortfallTicks);
            if (nextInjectCfrRecoveryActive != injectCfrRecoveryActive) {
                const uint64_t transitionTick = GetTickCount64();
                const bool recoveryEntering = nextInjectCfrRecoveryActive;
                const uint64_t recoveryDurationMs =
                    !recoveryEntering && injectCfrRecoveryStartTick > 0 ? transitionTick - injectCfrRecoveryStartTick
                                                                        : 0;
                const uint64_t recoveryFreshCatchup =
                    !recoveryEntering ? injectFreshCatchupTotal - injectCfrRecoveryStartFreshCatchup : 0;
                const uint64_t recoveryRepeatCatchup =
                    !recoveryEntering ? injectRepeatCatchupTotal - injectCfrRecoveryStartRepeatCatchup : 0;
                injectCfrRecoveryActive = nextInjectCfrRecoveryActive;
                if (injectCfrRecoveryActive) {
                    ++injectCfrRecoveryEpisodesThisWindow;
                    ++injectCfrRecoveryEpisodesTotal;
                    injectCfrRecoveryStartTick = transitionTick;
                    injectCfrRecoveryStartDebt = outputShortfallTicks;
                    injectCfrRecoveryBestDebt = outputShortfallTicks;
                    injectCfrRecoveryStartFreshCatchup = injectFreshCatchupTotal;
                    injectCfrRecoveryStartRepeatCatchup = injectRepeatCatchupTotal;
                    injectCfrRecoveryLastProgressLogTick = transitionTick;
                }
                LogInfo(
                    "[Inject CFR] Recovery %s: shortfall=%u/%.1fms startDebt=%u bestDebt=%u duration=%llums "
                    "fresh=%llu repeat=%llu enc=%.2fms service=%.2fms cycle=%.2fms bottleneck=%d. "
                    "exitDebt=%u tick(s)",
                    injectCfrRecoveryActive
                        ? "entered"
                        : (outputShortfallTicks <= ce::capture_policy::kInjectCfrRecoveryExitShortfallTicks
                               ? "completed"
                               : "disarmed"),
                    outputShortfallTicks,
                    ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs),
                    recoveryEntering ? outputShortfallTicks : injectCfrRecoveryStartDebt,
                    recoveryEntering ? outputShortfallTicks : injectCfrRecoveryBestDebt,
                    static_cast<unsigned long long>(recoveryDurationMs),
                    static_cast<unsigned long long>(recoveryFreshCatchup),
                    static_cast<unsigned long long>(recoveryRepeatCatchup), smoothedEncodeMs,
                    smoothedInjectServiceMs, smoothedEncCycleMs,
                    g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ? 1 : 0,
                    ce::capture_policy::kInjectCfrRecoveryExitShortfallTicks);
            }
            if (injectCfrRecoveryActive) {
                injectCfrRecoveryBestDebt = std::min(injectCfrRecoveryBestDebt, outputShortfallTicks);
                const uint64_t recoveryNowTick = GetTickCount64();
                if (injectCfrRecoveryStartTick > 0 && recoveryNowTick - injectCfrRecoveryStartTick >= 5000 &&
                    recoveryNowTick - injectCfrRecoveryLastProgressLogTick >= 5000) {
                    LogWarn(
                        "[Inject CFR] Recovery still active: duration=%llums debt=%u start=%u best=%u "
                        "fresh=%llu repeat=%llu enc=%.2fms service=%.2fms cycle=%.2fms buffered=%zu credit=%.2f "
                        "bottleneck=%d serviceSlow=%d",
                        static_cast<unsigned long long>(recoveryNowTick - injectCfrRecoveryStartTick),
                        outputShortfallTicks, injectCfrRecoveryStartDebt, injectCfrRecoveryBestDebt,
                        static_cast<unsigned long long>(injectFreshCatchupTotal - injectCfrRecoveryStartFreshCatchup),
                        static_cast<unsigned long long>(injectRepeatCatchupTotal - injectCfrRecoveryStartRepeatCatchup),
                        smoothedEncodeMs, smoothedInjectServiceMs, smoothedEncCycleMs, bufferedInjectFrames.size(),
                        frameCreditAccumulator,
                        g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ? 1 : 0,
                        injectEncoderServiceTooSlowCurrent ? 1 : 0);
                    injectCfrRecoveryLastProgressLogTick = recoveryNowTick;
                }
            }
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
                    outputShortfallTicks, injectCfrRecoveryActive,
                    encoderCatchupBottleneckedCurrent || injectEncoderServiceTooSlowCurrent);
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
                    const uint32_t wakeLateUs = SaturatingToUint32(static_cast<uint64_t>(encoderLateQpc) * 1000000ull /
                                                                   static_cast<uint64_t>(qpcFreq.QuadPart));
                    encoderWakeLateAccumUs += wakeLateUs;
                    ++encoderWakeLateSamples;
                    encoderWakeLateMaxUs = std::max(encoderWakeLateMaxUs, wakeLateUs);
                    if (g_pSharedMem) {
                        g_pSharedMem->runtimeState.encoderTimerWakeLateAvgUs.store(
                            SaturatingToUint32(encoderWakeLateAccumUs / encoderWakeLateSamples),
                            std::memory_order_relaxed);
                        g_pSharedMem->runtimeState.encoderTimerWakeLateMaxUs.store(encoderWakeLateMaxUs,
                                                                                   std::memory_order_relaxed);
                    }
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

        int64_t scheduledOutputQpc = scheduledSampleQpc;
        const auto computeWgcSelectionTargetForTick = [&](int64_t scheduledQpcForTick, int64_t selectionGridTickForTick,
                                                          bool applyLiveDelay) {
            const int64_t fallbackTargetQpc =
                ComputeIdealOutputQpc(encoderGridStartQpc, selectionGridTickForTick, targetIntervalTicks);
            // The selection target must keep subtracting the content delay through WGC live-recovery on
            // the uniform-cadence path. A VRR / GPU-bound source running below the output target keeps
            // live-recovery LATCHED indefinitely (it only exits once the source outruns output, which a
            // perpetually-below-target source never does), so a raw `!wgcLiveRecoveryModeActive` gate
            // here collapses the realized content delay to ~0 and latches it there for the rest of the
            // recording -- the collapse half of the realized-delay rubber-band (real session
            // 20260626_050554: live-recovery engaged at ~31 s and the realized delay sat at ~0/late
            // residual ~31.5 ms until stop, i.e. video ran ~31.5 ms ahead of the loopback audio it is
            // meant to align with, and the collapse transition is a visible content fast-forward).
            // Mirror ShouldLiveRecoverySuppressWgcSelectionDelay so the legacy reservoir path still
            // yields to live-recovery while the uniform path HOLDS the delay (live-recovery keeps
            // driving max-rate capture refill regardless; only the SELECTION delay is preserved). This
            // matches the flag that ShouldApplyWgcSelectionDelay already keeps set
            // (wgcSelectionDelayAppliedThisTick) -- previously the flag said "apply delay" while this
            // target computation silently dropped it. GetWgcActiveDelaySelectionTargetQpc is the single
            // source of truth that keeps the two decisions from diverging again.
            const int64_t effectiveContentDelayQpc = getWgcEffectiveContentDelayQpc();
            const bool uniformCadenceActiveDelay = effectiveContentDelayQpc > 0 && config.wgcActiveDelayUniformCadence;
            return ce::capture_policy::GetWgcActiveDelaySelectionTargetQpc(
                scheduledQpcForTick, fallbackTargetQpc, targetIntervalTicks, recordingOutputLive, applyLiveDelay,
                wgcLiveRecoveryModeActive, uniformCadenceActiveDelay, effectiveContentDelayQpc);
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
                ce::capture_policy::kCfrShortfallCatchupThresholdTicks, isWgcEncoderLimitedSmoothnessMode(),
                getWgcEffectiveContentDelayQpc());
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
        bool wgcDelayRealizationRecordedThisTick = false;
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
            // When an A/V content delay is active, a GPU-bound source that under-delivers cannot
            // sustain the delay reservoir. Defending it per-tick by selecting older-than-target
            // frames (reserve-preservation index-0 bias + soft-late older search) perturbs the
            // otherwise-uniform CFR cadence into abnormal judder. In uniform-cadence mode we take
            // the closest-to-target frame (monotonic + hard-cap guards stay intact) and let the
            // realized content delay float gracefully; anti-freeze rescue/relaxed paths are kept.
            const bool preferUniformActiveDelayCadence = ce::capture_policy::IsWgcActiveDelayUniformCadenceMode(
                selectionDelayApplied, config.wgcActiveDelayUniformCadence);
            ce::capture_policy::WgcAdaptiveTelemetry activeDelayTelemetry{};
            activeDelayTelemetry.outputFps = outputFps;
            activeDelayTelemetry.recentDeliveredFps = wgcRecentDeliveredFps;
            activeDelayTelemetry.recentDeliveredMin250Fps = wgcRecentDeliveredMin250Fps;
            activeDelayTelemetry.recentDeliveredMin500Fps = wgcRecentDeliveredMin500Fps;
            activeDelayTelemetry.recentInputMin250Fps = wgcRecentInputMin250Fps;
            activeDelayTelemetry.recentInputMin500Fps = wgcRecentInputMin500Fps;
            const uint32_t wgcSourceJitterAvgUs = g_WgcCap ? SaturatingToUint32(g_WgcCap->GetSourceJitterAvgUs()) : 0u;
            const uint32_t wgcPredictorJitterUs =
                wgcInputPredictor.IsCalibrated()
                    ? SaturatingToUint32(static_cast<uint64_t>(wgcInputPredictor.GetJitterUs(qpcFreq.QuadPart)))
                    : 0u;
            activeDelayTelemetry.averageJitterUs = std::max(wgcSourceJitterAvgUs, wgcPredictorJitterUs);
            activeDelayTelemetry.emptyTickPermille = wgcNoFreshTickPermille;
            activeDelayTelemetry.bufferedWgcFrames =
                static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), static_cast<size_t>(UINT32_MAX)));
            const int64_t effectiveSelectionTargetQpc =
                selectionTargetQpc > 0 ? selectionTargetQpc : liveSelectionTargetQpc;
            const uint32_t activeDelaySoftLateTargetUs =
                ce::capture_policy::GetWgcActiveDelaySoftLateTargetUs(targetIntervalTicks, qpcFreq.QuadPart);
            const int64_t minFreshTimestampQpc = ce::capture_policy::GetWgcMinimumFreshTimestampQpc(
                lastEmittedWgcSourceQpc, liveSelectionTargetQpc, targetIntervalTicks, lowSourceMode);
            const int64_t baseStaleFallbackMinTimestampQpc =
                ce::capture_policy::GetWgcStaleUniqueFallbackMinTimestampQpc(
                    lastEmittedWgcSourceQpc, effectiveSelectionTargetQpc, targetIntervalTicks, lowSourceMode,
                    deepUnderfeed);
            int64_t staleFallbackMinTimestampQpc = baseStaleFallbackMinTimestampQpc;
            if (selectionDelayApplied && staleFallbackMinTimestampQpc > 0 && activeDelayTelemetry.averageJitterUs > 0 &&
                targetIntervalTicks > 0 && qpcFreq.QuadPart > 0) {
                const uint32_t reserveCapFrames = getWgcDelayReservoirTargetFrames() + 2u;
                if (bufferedWgcFrames.size() <= reserveCapFrames) {
                    const int64_t jitterMarginQpc = std::min<int64_t>(
                        targetIntervalTicks * 2,
                        (static_cast<int64_t>(activeDelayTelemetry.averageJitterUs) * qpcFreq.QuadPart) / 1000000);
                    staleFallbackMinTimestampQpc = std::max<int64_t>(0, staleFallbackMinTimestampQpc - jitterMarginQpc);
                }
            }

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
            bool olderFrameAvoidedRepeatThisTick = false;
            const auto activeDelayRepeatClusterTicks = [&]() -> uint32_t {
                return std::max<uint32_t>(
                    cadenceCounters.consecutiveDuplicateFrames,
                    cadenceCounters.holdTicksRunning > 1 ? (cadenceCounters.holdTicksRunning - 1) : 0);
            };
            const auto currentDelayResidualAvgAbsUs = [&]() -> uint32_t {
                if (wgcDelayResidualWindowSamples > 0) {
                    return SaturatingToUint32(wgcDelayResidualWindowAbsAccumUs / wgcDelayResidualWindowSamples);
                }
                return wgcDelayResidualSamples > 0
                           ? SaturatingToUint32(wgcDelayResidualAbsAccumUs / wgcDelayResidualSamples)
                           : 0u;
            };
            const auto currentDelayResidualP95Us = [&]() -> uint32_t {
                const uint32_t windowP95 = wgcDelayResidualWindowP95Us();
                return windowP95 > 0 ? windowP95 : wgcDelayResidualP95Us();
            };
            const auto currentDelayResidualLateMaxUs = [&]() -> uint32_t {
                return wgcDelayResidualWindowLateMaxUs > 0 ? wgcDelayResidualWindowLateMaxUs
                                                           : wgcDelayResidualLateMaxUs;
            };
            const auto currentRawDelayResidualAvgAbsUs = [&]() -> uint32_t {
                if (wgcDelayRawResidualWindowSamples > 0) {
                    return SaturatingToUint32(wgcDelayRawResidualWindowAbsAccumUs / wgcDelayRawResidualWindowSamples);
                }
                return wgcDelayRawResidualSamples > 0
                           ? SaturatingToUint32(wgcDelayRawResidualAbsAccumUs / wgcDelayRawResidualSamples)
                           : 0u;
            };
            const auto currentRawDelayResidualP95Us = [&]() -> uint32_t {
                const uint32_t windowP95 = wgcDelayRawResidualWindowP95Us();
                return windowP95 > 0 ? windowP95 : wgcDelayRawResidualP95Us();
            };
            const auto currentRawDelayResidualLateMaxUs = [&]() -> uint32_t {
                return wgcDelayRawResidualWindowLateMaxUs > 0 ? wgcDelayRawResidualWindowLateMaxUs
                                                              : wgcDelayRawResidualLateMaxUs;
            };
            const auto currentCombinedDelayResidualAvgAbsUs = [&]() -> uint32_t {
                return std::max(currentDelayResidualAvgAbsUs(), currentRawDelayResidualAvgAbsUs());
            };
            const auto currentCombinedDelayResidualP95Us = [&]() -> uint32_t {
                return std::max(currentDelayResidualP95Us(), currentRawDelayResidualP95Us());
            };
            const auto currentCombinedDelayResidualLateMaxUs = [&]() -> uint32_t {
                return std::max(currentDelayResidualLateMaxUs(), currentRawDelayResidualLateMaxUs());
            };
            const auto activeDelayWindowClassFor = [&](bool hardSafeCandidateAvailable) {
                const bool activeDelaySourceRecovery = wgcActiveDelaySourceRecoveryUntilTick > GetTickCount64();
                return ce::capture_policy::ClassifyWgcActiveDelayWindow(
                    activeDelayTelemetry, lowSourceMode, wgcLiveRecoveryModeActive, wgcSourceStarvedCurrent,
                    deepUnderfeed, activeDelaySourceRecovery, hardSafeCandidateAvailable);
            };
            const auto rawActiveDelayCandidateSafe = [&](int64_t rawSelectionTimestamp) -> bool {
                if (rawSelectionTimestamp <= 0) {
                    return true;
                }
                if (ce::capture_policy::IsWgcFrameTooNewForActiveDelayHardLimit(
                        rawSelectionTimestamp, effectiveSelectionTargetQpc, targetIntervalTicks, qpcFreq.QuadPart)) {
                    return false;
                }
                uint32_t rawLateResidualUs = 0;
                if (rawSelectionTimestamp > effectiveSelectionTargetQpc && qpcFreq.QuadPart > 0) {
                    rawLateResidualUs = SaturatingToUint32(static_cast<uint64_t>(
                        (rawSelectionTimestamp - effectiveSelectionTargetQpc) * 1000000 / qpcFreq.QuadPart));
                }
                return ce::capture_policy::HasWgcActiveDelayResidualHeadroom(
                    rawLateResidualUs, currentRawDelayResidualAvgAbsUs(), currentRawDelayResidualP95Us(),
                    currentRawDelayResidualLateMaxUs(), activeDelayWindowClassFor(true), activeDelaySoftLateTargetUs);
            };
            const auto activeDelayCandidateLateResidualUs = [&](const QueuedFrame& candidate) -> uint32_t {
                return ce::capture_policy::GetWgcActiveDelayFinalSelectionLateResidualUs(
                    GetFrameSelectionTimestamp(candidate), getWgcRawSelectionTimestamp(candidate),
                    effectiveSelectionTargetQpc, qpcFreq.QuadPart);
            };
            const auto isActiveDelayCandidateHardSafe = [&](const QueuedFrame& candidate) -> bool {
                if (!selectionDelayApplied || effectiveSelectionTargetQpc <= 0 || candidate.timestamp <= 0) {
                    return false;
                }
                const bool sourceTimestampAdvanced = candidate.timestamp > lastEmittedWgcSourceQpc;
                const bool fallbackFreshEnough =
                    ce::capture_policy::IsWgcTimestampFreshEnough(candidate.timestamp, staleFallbackMinTimestampQpc);
                if (!sourceTimestampAdvanced || !fallbackFreshEnough) {
                    return false;
                }
                return ce::capture_policy::IsWgcActiveDelayFinalSelectionWithinHardLimit(
                    GetFrameSelectionTimestamp(candidate), getWgcRawSelectionTimestamp(candidate),
                    effectiveSelectionTargetQpc, targetIntervalTicks, qpcFreq.QuadPart);
            };
            const auto isActiveDelayCandidateSoftSafe = [&](const QueuedFrame& candidate) -> bool {
                if (!isActiveDelayCandidateHardSafe(candidate)) {
                    return false;
                }
                return ce::capture_policy::IsWgcActiveDelayFinalSelectionWithinSoftLateTarget(
                    GetFrameSelectionTimestamp(candidate), getWgcRawSelectionTimestamp(candidate),
                    effectiveSelectionTargetQpc, targetIntervalTicks, qpcFreq.QuadPart, activeDelaySoftLateTargetUs);
            };
            const auto hasActiveDelayHardSafeCandidate = [&]() -> bool {
                for (const QueuedFrame& candidate : bufferedWgcFrames) {
                    if (isActiveDelayCandidateHardSafe(candidate)) {
                        return true;
                    }
                }
                return false;
            };
            const auto hasActiveDelaySoftSafeCandidate = [&]() -> bool {
                for (const QueuedFrame& candidate : bufferedWgcFrames) {
                    if (isActiveDelayCandidateSoftSafe(candidate)) {
                        return true;
                    }
                }
                return false;
            };
            const auto currentOldestSoftSafeAgeUs = [&]() -> uint32_t {
                if (liveNowQpc <= 0 || qpcFreq.QuadPart <= 0) {
                    return 0u;
                }
                uint32_t oldestAgeUs = 0;
                for (const QueuedFrame& candidate : bufferedWgcFrames) {
                    if (!isActiveDelayCandidateSoftSafe(candidate)) {
                        continue;
                    }
                    const int64_t selectionTimestamp = GetFrameSelectionTimestamp(candidate);
                    if (selectionTimestamp <= 0 || liveNowQpc <= selectionTimestamp) {
                        continue;
                    }
                    const uint32_t ageUs = SaturatingToUint32(
                        static_cast<uint64_t>((liveNowQpc - selectionTimestamp) * 1000000 / qpcFreq.QuadPart));
                    oldestAgeUs = std::max(oldestAgeUs, ageUs);
                }
                return oldestAgeUs;
            };
            const auto currentRepeatReserveSpanUs = [&]() -> uint32_t {
                if (bufferedWgcFrames.size() < 2 || qpcFreq.QuadPart <= 0) {
                    return 0u;
                }
                const int64_t firstQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.front());
                const int64_t lastQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.back());
                if (firstQpc <= 0 || lastQpc <= firstQpc) {
                    return 0u;
                }
                return SaturatingToUint32(static_cast<uint64_t>((lastQpc - firstQpc) * 1000000 / qpcFreq.QuadPart));
            };
            const auto recordActiveDelayRepeatClass =
                [&](ce::capture_policy::WgcActiveDelayWindowClass repeatWindowClass, bool hardSafeCandidateAvailable,
                    bool softSafeCandidateAvailable) {
                    if (!selectionDelayApplied) {
                        return;
                    }
                    switch (repeatWindowClass) {
                        case ce::capture_policy::WgcActiveDelayWindowClass::kHealthy:
                            ++wgcDelayWindowHealthyRepeatWindow;
                            ++wgcDelayWindowHealthyRepeatTotal;
                            break;
                        case ce::capture_policy::WgcActiveDelayWindowClass::kRecoverableUnderfill:
                            ++wgcDelayWindowRecoverableRepeatWindow;
                            ++wgcDelayWindowRecoverableRepeatTotal;
                            break;
                        case ce::capture_policy::WgcActiveDelayWindowClass::kSourceLimited:
                            ++wgcDelayWindowSourceLimitedRepeatWindow;
                            ++wgcDelayWindowSourceLimitedRepeatTotal;
                            break;
                        case ce::capture_policy::WgcActiveDelayWindowClass::kHardSourceStall:
                            ++wgcDelayWindowSourceLimitedRepeatWindow;
                            ++wgcDelayWindowSourceLimitedRepeatTotal;
                            ++wgcDelayWindowHardStallRepeatWindow;
                            ++wgcDelayWindowHardStallRepeatTotal;
                            break;
                        case ce::capture_policy::WgcActiveDelayWindowClass::kPostStallRecovery:
                            ++wgcDelayWindowRecoverableRepeatWindow;
                            ++wgcDelayWindowRecoverableRepeatTotal;
                            ++wgcDelayWindowPostStallRepeatWindow;
                            ++wgcDelayWindowPostStallRepeatTotal;
                            break;
                    }
                    if (hardSafeCandidateAvailable) {
                        ++wgcDelayRepeatWithSafeCandidateWindow;
                        ++wgcDelayRepeatWithSafeCandidateTotal;
                    } else {
                        ++wgcDelayRepeatWithoutSafeCandidateWindow;
                        ++wgcDelayRepeatWithoutSafeCandidateTotal;
                    }
                    if (softSafeCandidateAvailable) {
                        ++wgcDelayRepeatWithSoftSafeCandidateWindow;
                        ++wgcDelayRepeatWithSoftSafeCandidateTotal;
                        const uint32_t oldestSoftSafeAgeUs = currentOldestSoftSafeAgeUs();
                        wgcDelayOldestSoftSafeAgeWindowMaxUs =
                            std::max(wgcDelayOldestSoftSafeAgeWindowMaxUs, oldestSoftSafeAgeUs);
                        wgcDelayOldestSoftSafeAgeMaxUs = std::max(wgcDelayOldestSoftSafeAgeMaxUs, oldestSoftSafeAgeUs);
                    } else {
                        ++wgcDelayRepeatWithoutSoftSafeCandidateWindow;
                        ++wgcDelayRepeatWithoutSoftSafeCandidateTotal;
                        ++wgcDelaySyncProtectedRepeatWindow;
                        ++wgcDelaySyncProtectedRepeatTotal;
                        if (hardSafeCandidateAvailable) {
                            ++wgcDelayRepeatHardOnlyCandidateWindow;
                            ++wgcDelayRepeatHardOnlyCandidateTotal;
                        }
                    }
                    if (!wgcActiveDelayRepeatClassKnown || repeatWindowClass != wgcActiveDelayLastRepeatClass) {
                        const uint64_t nowTick = GetTickCount64();
                        if (!wgcActiveDelayRepeatClassKnown || nowTick - wgcActiveDelayLastRepeatClassLogTick >= 500) {
                            LogInfo(
                                "[WGC CFR] Active-delay repeat state=%s hardSafe=%d softSafe=%d srcStarved=%d "
                                "lowSource=%d deepUnderfeed=%d recoveryActive=%d buffered=%zu span=%uus "
                                "residualP95=%uus rawP95=%uus softTarget=%uus",
                                ce::capture_policy::WgcActiveDelayWindowClassToString(repeatWindowClass),
                                hardSafeCandidateAvailable ? 1 : 0, softSafeCandidateAvailable ? 1 : 0,
                                wgcSourceStarvedCurrent ? 1 : 0, lowSourceMode ? 1 : 0, deepUnderfeed ? 1 : 0,
                                wgcActiveDelaySourceRecoveryUntilTick > nowTick ? 1 : 0, bufferedWgcFrames.size(),
                                currentRepeatReserveSpanUs(), currentCombinedDelayResidualP95Us(),
                                currentRawDelayResidualP95Us(), activeDelaySoftLateTargetUs);
                            wgcActiveDelayLastRepeatClassLogTick = nowTick;
                        }
                        wgcActiveDelayRepeatClassKnown = true;
                        wgcActiveDelayLastRepeatClass = repeatWindowClass;
                    }
                    const uint32_t reserveDepth = SaturatingToUint32(bufferedWgcFrames.size());
                    wgcDelayRepeatReserveDepthWindowMax = std::max(wgcDelayRepeatReserveDepthWindowMax, reserveDepth);
                    wgcDelayRepeatReserveDepthMax = std::max(wgcDelayRepeatReserveDepthMax, reserveDepth);
                    const uint32_t reserveSpanUs = currentRepeatReserveSpanUs();
                    wgcDelayRepeatReserveSpanWindowMaxUs =
                        std::max(wgcDelayRepeatReserveSpanWindowMaxUs, reserveSpanUs);
                    wgcDelayRepeatReserveSpanMaxUs = std::max(wgcDelayRepeatReserveSpanMaxUs, reserveSpanUs);
                };
            const auto isCurrentSyncDelayHoldSourceLimited = [&](bool softSafeCandidateAvailable) -> bool {
                if (!selectionDelayApplied) {
                    return false;
                }
                if (!softSafeCandidateAvailable) {
                    return true;
                }
                const bool activeDelaySourceRecovery = wgcActiveDelaySourceRecoveryUntilTick > GetTickCount64();
                const bool sourceRecoveryWithoutSafeFrame = activeDelaySourceRecovery && !softSafeCandidateAvailable;
                if (ce::capture_policy::IsWgcSyncDelayHoldSourceLimited(
                        outputFps, wgcRecentDeliveredMin250Fps, wgcRecentInputMin250Fps, wgcNoFreshTickPermille,
                        wgcSourceStarvedCurrent, lowSourceMode, deepUnderfeed, sourceRecoveryWithoutSafeFrame)) {
                    return true;
                }
                return false;
            };
            const auto recordActiveDelayRepeatLowerBound = [&](bool softSafeCandidateAvailable,
                                                               bool syncDelayHoldSourceLimited) {
                if (!selectionDelayApplied) {
                    return;
                }
                if (syncDelayHoldSourceLimited || !softSafeCandidateAvailable) {
                    ++wgcSourceRepeatLowerBoundWindow;
                    ++wgcSourceRepeatLowerBoundTotal;
                    ++wgcDelaySourceLimitedRepeatWindow;
                    ++wgcDelaySourceLimitedRepeatTotal;
                    return;
                }

                ++wgcExcessRepeatWindow;
                ++wgcExcessRepeatTotal;
                ++wgcPolicyAddedRepeatWindow;
                ++wgcPolicyAddedRepeatTotal;
                const uint32_t repeatClusterTicks = activeDelayRepeatClusterTicks();
                if (repeatClusterTicks > 0) {
                    ++wgcExcessRepeatClusterWindow;
                    ++wgcExcessRepeatClusterTotal;
                    wgcExcessRepeatClusterWindowMaxTicks =
                        std::max(wgcExcessRepeatClusterWindowMaxTicks, repeatClusterTicks);
                    wgcExcessRepeatClusterMaxTicks = std::max(wgcExcessRepeatClusterMaxTicks, repeatClusterTicks);
                }
            };
            const auto recordSyncDelayRepeatHold = [&](bool countRepeatClusterPressure, bool hardSafeCandidateAvailable,
                                                       bool softSafeCandidateAvailable) {
                if (selectionDelayApplied && countRepeatClusterPressure) {
                    const uint32_t repeatClusterTicks = activeDelayRepeatClusterTicks();
                    if (repeatClusterTicks > 0) {
                        ++wgcDelayRepeatClusterPressureWindow;
                        ++wgcDelayRepeatClusterPressureTotal;
                        wgcDelayRepeatClusterPressureWindowMaxTicks =
                            std::max(wgcDelayRepeatClusterPressureWindowMaxTicks, repeatClusterTicks);
                        wgcDelayRepeatClusterPressureMaxTicks =
                            std::max(wgcDelayRepeatClusterPressureMaxTicks, repeatClusterTicks);
                    }
                }
                ++wgcRepeatPolicyHoldCount;
                ++wgcRepeatPolicyHoldTotal;
                if (selectionDelayApplied) {
                    ++wgcSyncDelayHoldCount;
                    ++wgcSyncDelayHoldTotal;
                    const uint64_t selectionNowTick = GetTickCount64();
                    const bool activeDelaySourceRecovery = wgcActiveDelaySourceRecoveryUntilTick > selectionNowTick;
                    const auto repeatWindowClass = activeDelayWindowClassFor(hardSafeCandidateAvailable);
                    recordActiveDelayRepeatClass(repeatWindowClass, hardSafeCandidateAvailable,
                                                 softSafeCandidateAvailable);
                    const bool syncDelayHoldSourceLimited =
                        isCurrentSyncDelayHoldSourceLimited(softSafeCandidateAvailable);
                    if (syncDelayHoldSourceLimited) {
                        ++wgcSyncDelaySourceLimitedHoldCount;
                        ++wgcSyncDelaySourceLimitedHoldTotal;
                        if (activeDelaySourceRecovery && !wgcSourceStarvedCurrent && !lowSourceMode && !deepUnderfeed) {
                            ++wgcSyncDelaySourceRecoveryHoldCount;
                            ++wgcSyncDelaySourceRecoveryHoldTotal;
                        }
                    } else {
                        ++wgcSyncDelayPolicyHoldCount;
                        ++wgcSyncDelayPolicyHoldTotal;
                    }
                }
            };
            auto buildCandidateList = [&](std::vector<size_t>* outIndices, bool requireFresh,
                                          bool allowRelaxedActiveDelayResidual) {
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
                    const int64_t candidateRawSelectionTimestamp = getWgcRawSelectionTimestamp(candidate);
                    if (selectionDelayApplied &&
                        !ce::capture_policy::IsWgcActiveDelayFinalSelectionWithinHardLimit(
                            candidateSelectionTimestamp, candidateRawSelectionTimestamp, effectiveSelectionTargetQpc,
                            targetIntervalTicks, qpcFreq.QuadPart)) {
                        skippedTooNewForSlot = true;
                        ++wgcDelayRelaxedRejectedSyncRiskWindow;
                        ++wgcDelayRelaxedRejectedSyncRiskTotal;
                        continue;
                    }
                    const bool tooNewForSlot =
                        g_HasLastFrame && !g_LastFrame.isInjectMode &&
                        (selectionDelayApplied
                             ? ce::capture_policy::IsWgcFrameTooNewForActiveDelaySlot(
                                   candidateSelectionTimestamp, effectiveSelectionTargetQpc, targetIntervalTicks)
                             : ce::capture_policy::IsWgcFrameTooNewForCfrSlot(
                                   candidateSelectionTimestamp, effectiveSelectionTargetQpc, targetIntervalTicks));
                    if (tooNewForSlot) {
                        bool useRelaxedActiveDelayCandidate = false;
                        if (selectionDelayApplied && allowRelaxedActiveDelayResidual && g_HasLastFrame &&
                            !g_LastFrame.isInjectMode) {
                            const int64_t repeatSelectionTimestamp = GetFrameSelectionTimestamp(g_LastFrame);
                            const auto delayWindowClass = activeDelayWindowClassFor(true);
                            const auto relaxedScore = ce::capture_policy::ScoreWgcActiveDelayRelaxedCandidate(
                                candidateSelectionTimestamp, repeatSelectionTimestamp, effectiveSelectionTargetQpc,
                                targetIntervalTicks, qpcFreq.QuadPart, activeDelayRepeatClusterTicks(),
                                currentCombinedDelayResidualAvgAbsUs(), currentCombinedDelayResidualP95Us(),
                                currentCombinedDelayResidualLateMaxUs(), delayWindowClass, activeDelaySoftLateTargetUs);
                            useRelaxedActiveDelayCandidate = relaxedScore.Accepted();
                            if (useRelaxedActiveDelayCandidate &&
                                !rawActiveDelayCandidateSafe(candidateRawSelectionTimestamp)) {
                                useRelaxedActiveDelayCandidate = false;
                                ++wgcDelayRelaxedRejectedSyncRiskWindow;
                                ++wgcDelayRelaxedRejectedSyncRiskTotal;
                            }
                            switch (relaxedScore.decision) {
                                case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectSyncRisk:
                                    ++wgcDelayRelaxedRejectedSyncRiskWindow;
                                    ++wgcDelayRelaxedRejectedSyncRiskTotal;
                                    break;
                                case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom:
                                    ++wgcDelayRelaxedRejectedResidualHeadroomWindow;
                                    ++wgcDelayRelaxedRejectedResidualHeadroomTotal;
                                    if (!ce::capture_policy::IsWgcActiveDelaySourceLimitedClass(delayWindowClass) &&
                                        relaxedScore.candidateLateResidualUs > activeDelaySoftLateTargetUs) {
                                        ++wgcDelaySoftLateRejectedWindow;
                                        ++wgcDelaySoftLateRejectedTotal;
                                    }
                                    break;
                                case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectRepeatCost:
                                    ++wgcDelayRelaxedRejectedRepeatCostWindow;
                                    ++wgcDelayRelaxedRejectedRepeatCostTotal;
                                    break;
                                default:
                                    break;
                            }
                            if (useRelaxedActiveDelayCandidate &&
                                relaxedScore.candidateLateResidualUs > activeDelaySoftLateTargetUs) {
                                ++wgcDelayNearCapAcceptedWindow;
                                ++wgcDelayNearCapAcceptedTotal;
                            }
                            if (useRelaxedActiveDelayCandidate &&
                                !ce::capture_policy::IsWgcActiveDelaySourceLimitedClass(delayWindowClass) &&
                                relaxedScore.candidateLateResidualUs > activeDelaySoftLateTargetUs) {
                                ++wgcDelaySoftLateAcceptedWindow;
                                ++wgcDelaySoftLateAcceptedTotal;
                            }
                        }
                        if (!useRelaxedActiveDelayCandidate) {
                            skippedTooNewForSlot = true;
                            continue;
                        }
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

            buildCandidateList(&wgcFreshCandidateIndices, true, false);
            if (wgcFreshCandidateIndices.empty()) {
                buildCandidateList(&wgcFallbackCandidateIndices, false, false);
            }

            std::vector<size_t>* candidateIndices =
                !wgcFreshCandidateIndices.empty() ? &wgcFreshCandidateIndices : &wgcFallbackCandidateIndices;
            bool usingFreshCandidateSet = !wgcFreshCandidateIndices.empty();
            const auto bestCandidateDistance = [&](const std::vector<size_t>& indices) -> int64_t {
                if (indices.empty() || effectiveSelectionTargetQpc <= 0) {
                    return INT64_MAX;
                }
                int64_t bestDistance = AbsoluteTimestampDistance(
                    GetFrameSelectionTimestamp(bufferedWgcFrames[indices[0]]), effectiveSelectionTargetQpc);
                for (size_t candidateOffset = 1; candidateOffset < indices.size(); ++candidateOffset) {
                    const int64_t candidateDistance = AbsoluteTimestampDistance(
                        GetFrameSelectionTimestamp(bufferedWgcFrames[indices[candidateOffset]]),
                        effectiveSelectionTargetQpc);
                    bestDistance = std::min(bestDistance, candidateDistance);
                }
                return bestDistance;
            };
            if (selectionDelayApplied && skippedTooNewForSlot) {
                buildCandidateList(&wgcRelaxedFreshCandidateIndices, true, true);
                if (wgcRelaxedFreshCandidateIndices.empty()) {
                    buildCandidateList(&wgcRelaxedFallbackCandidateIndices, false, true);
                }
                std::vector<size_t>* relaxedCandidateIndices = !wgcRelaxedFreshCandidateIndices.empty()
                                                                   ? &wgcRelaxedFreshCandidateIndices
                                                                   : &wgcRelaxedFallbackCandidateIndices;
                if (!relaxedCandidateIndices->empty()) {
                    const int64_t strictDistance = bestCandidateDistance(*candidateIndices);
                    const int64_t relaxedDistance = bestCandidateDistance(*relaxedCandidateIndices);
                    if (candidateIndices->empty() || relaxedDistance < strictDistance) {
                        candidateIndices = relaxedCandidateIndices;
                        usingFreshCandidateSet = !wgcRelaxedFreshCandidateIndices.empty();
                    }
                }
            }

            if (selectionDelayApplied && skippedTooNewForSlot && candidateIndices->empty() && g_HasLastFrame &&
                !g_LastFrame.isInjectMode) {
                wgcRepeatRescueCandidateIndices.clear();
                ++wgcDelayRepeatRescueAttemptWindow;
                ++wgcDelayRepeatRescueAttemptTotal;
                ++wgcDelayRepeatPromotionAttemptWindow;
                ++wgcDelayRepeatPromotionAttemptTotal;
                const int64_t repeatSelectionTimestamp = GetFrameSelectionTimestamp(g_LastFrame);
                const auto rescueWindowClass = activeDelayWindowClassFor(true);
                for (size_t i = 0; i < bufferedWgcFrames.size(); ++i) {
                    const QueuedFrame& candidate = bufferedWgcFrames[i];
                    if (candidate.timestamp <= 0 || candidate.timestamp <= lastEmittedWgcSourceQpc ||
                        !ce::capture_policy::IsWgcTimestampFreshEnough(candidate.timestamp,
                                                                       staleFallbackMinTimestampQpc)) {
                        continue;
                    }
                    if (isActiveDelayCandidateSoftSafe(candidate)) {
                        wgcRepeatRescueCandidateIndices.push_back(i);
                        continue;
                    }

                    const auto rescueScore = ce::capture_policy::ScoreWgcActiveDelayRepeatRescueCandidate(
                        GetFrameSelectionTimestamp(candidate), getWgcRawSelectionTimestamp(candidate),
                        repeatSelectionTimestamp, effectiveSelectionTargetQpc, targetIntervalTicks, qpcFreq.QuadPart,
                        activeDelayRepeatClusterTicks(), currentCombinedDelayResidualAvgAbsUs(),
                        currentCombinedDelayResidualP95Us(), currentCombinedDelayResidualLateMaxUs(), rescueWindowClass,
                        activeDelaySoftLateTargetUs);
                    if (rescueScore.Accepted()) {
                        wgcRepeatRescueCandidateIndices.push_back(i);
                        continue;
                    }

                    switch (rescueScore.decision) {
                        case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectSyncRisk:
                            ++wgcDelayRepeatRescueRejectedSyncWindow;
                            ++wgcDelayRepeatRescueRejectedSyncTotal;
                            break;
                        case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom:
                            ++wgcDelayRepeatRescueRejectedHeadroomWindow;
                            ++wgcDelayRepeatRescueRejectedHeadroomTotal;
                            if (!ce::capture_policy::IsWgcActiveDelaySourceLimitedClass(rescueWindowClass) &&
                                rescueScore.candidateLateResidualUs > activeDelaySoftLateTargetUs) {
                                ++wgcDelayRepeatPromotionRejectedSoftWindow;
                                ++wgcDelayRepeatPromotionRejectedSoftTotal;
                            }
                            break;
                        case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectRepeatCost:
                            ++wgcDelayRepeatRescueRejectedCostWindow;
                            ++wgcDelayRepeatRescueRejectedCostTotal;
                            break;
                        default:
                            break;
                    }
                }
                if (!wgcRepeatRescueCandidateIndices.empty()) {
                    candidateIndices = &wgcRepeatRescueCandidateIndices;
                    usingFreshCandidateSet = false;
                    olderFrameAvoidedRepeatThisTick = true;
                    ++wgcDelayRepeatRescueSuccessWindow;
                    ++wgcDelayRepeatRescueSuccessTotal;
                    ++wgcDelayRepeatPromotedBeforeRepeatWindow;
                    ++wgcDelayRepeatPromotedBeforeRepeatTotal;
                    ++wgcDelayOlderFrameAvoidedRepeatWindow;
                    ++wgcDelayOlderFrameAvoidedRepeatTotal;
                }
            }

            if (candidateIndices->empty()) {
                ++wgcFreshSelectionMissCount;
                if (skippedTooNewForSlot) {
                    const bool hardSafeCandidateAvailable = hasActiveDelayHardSafeCandidate();
                    const bool softSafeCandidateAvailable = hasActiveDelaySoftSafeCandidate();
                    if (hardSafeCandidateAvailable) {
                        ++wgcDelayRepeatSafeAfterPromotionWindow;
                        ++wgcDelayRepeatSafeAfterPromotionTotal;
                    }
                    const bool syncDelayHoldSourceLimited =
                        isCurrentSyncDelayHoldSourceLimited(softSafeCandidateAvailable);
                    const auto repeatWindowClass = activeDelayWindowClassFor(hardSafeCandidateAvailable);
                    recordActiveDelayRepeatLowerBound(softSafeCandidateAvailable, syncDelayHoldSourceLimited);
                    recordSyncDelayRepeatHold(true, hardSafeCandidateAvailable, softSafeCandidateAvailable);
                    const int64_t firstSelectionTimestamp =
                        !bufferedWgcFrames.empty() ? GetFrameSelectionTimestamp(bufferedWgcFrames.front()) : 0;
                    int64_t leadUs = 0;
                    if (qpcFreq.QuadPart > 0 && firstSelectionTimestamp > effectiveSelectionTargetQpc) {
                        leadUs = ((firstSelectionTimestamp - effectiveSelectionTargetQpc) * 1000000) / qpcFreq.QuadPart;
                        const uint32_t leadUsClamped = SaturatingToUint32(static_cast<uint64_t>(leadUs));
                        wgcTooNewLeadMaxUs = std::max(wgcTooNewLeadMaxUs, leadUsClamped);
                        wgcTooNewLeadSessionMaxUs = std::max(wgcTooNewLeadSessionMaxUs, leadUsClamped);
                    }
                    static uint64_t s_lastTooNewWgcSelectionLogTick = 0;
                    const uint64_t nowTick = GetTickCount64();
                    if (nowTick - s_lastTooNewWgcSelectionLogTick >= 1000) {
                        const int64_t allowedLeadUs =
                            (qpcFreq.QuadPart > 0 && targetIntervalTicks > 0)
                                ? ((selectionDelayApplied
                                        ? ce::capture_policy::GetWgcActiveDelayResidualToleranceQpc(targetIntervalTicks)
                                        : (targetIntervalTicks *
                                           static_cast<int64_t>(ce::capture_policy::kWgcCfrSelectionMaxLeadTicks))) *
                                   1000000) /
                                      qpcFreq.QuadPart
                                : 0;
                        const int64_t avDelayUs = (qpcFreq.QuadPart > 0 && getWgcEffectiveContentDelayQpc() > 0)
                                                      ? (getWgcEffectiveContentDelayQpc() * 1000000) / qpcFreq.QuadPart
                                                      : 0;
                        LogInfo(
                            "[EncoderThread] WGC CFR slot repeat: buffered frame is too new for scheduled slot "
                            "(lead=%lldus allowedLead=%lldus avDelay=%lldus syncDelay=%d syncSourceLimited=%d "
                            "delayClass=%s softLateTarget=%uus hardSafeCandidate=%d softSafeCandidate=%d "
                            "lowSource=%d sourceStarved=%d encoderLimited=%d targetQpc=%lld firstQpc=%lld "
                            "buffered=%zu shortfall=%u minIn=%u minDel=%u noFresh=%upm)",
                            static_cast<long long>(leadUs), static_cast<long long>(allowedLeadUs),
                            static_cast<long long>(avDelayUs), selectionDelayApplied ? 1 : 0,
                            syncDelayHoldSourceLimited ? 1 : 0,
                            ce::capture_policy::WgcActiveDelayWindowClassToString(repeatWindowClass),
                            activeDelaySoftLateTargetUs, hardSafeCandidateAvailable ? 1 : 0,
                            softSafeCandidateAvailable ? 1 : 0, lowSourceMode ? 1 : 0, wgcSourceStarvedCurrent ? 1 : 0,
                            isWgcEncoderLimitedSmoothnessMode() ? 1 : 0,
                            static_cast<long long>(effectiveSelectionTargetQpc),
                            static_cast<long long>(firstSelectionTimestamp), bufferedWgcFrames.size(),
                            outputShortfallTicks, wgcRecentInputMin250Fps, wgcRecentDeliveredMin250Fps,
                            wgcNoFreshTickPermille);
                        s_lastTooNewWgcSelectionLogTick = nowTick;
                    }
                } else {
                    recordActiveDelayRepeatLowerBound(false, true);
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
                    ce::capture_policy::ShouldPreferEarlierFreshWgcFrameForReserveDefense(
                        earlierFresh.selectionTimestamp > 0 ? earlierFresh.selectionTimestamp : earlierFresh.timestamp,
                        chosenFresh.selectionTimestamp > 0 ? chosenFresh.selectionTimestamp : chosenFresh.timestamp,
                        effectiveSelectionTargetQpc, targetIntervalTicks, wgcReservePressureActive, lowSourceMode,
                        deepUnderfeed, wgcLiveRecoveryModeActive, preferUniformActiveDelayCadence)) {
                    selectedIndex = 0;
                } else {
                    ++wgcReserveSpendTickCount;
                }
            }

            const auto finalSelectionWithinActiveDelayHardLimit = [&](size_t candidateIndex) -> bool {
                if (!selectionDelayApplied || effectiveSelectionTargetQpc <= 0 ||
                    candidateIndex >= bufferedWgcFrames.size()) {
                    return true;
                }
                const QueuedFrame& finalCandidate = bufferedWgcFrames[candidateIndex];
                return ce::capture_policy::IsWgcActiveDelayFinalSelectionWithinHardLimit(
                    GetFrameSelectionTimestamp(finalCandidate), getWgcRawSelectionTimestamp(finalCandidate),
                    effectiveSelectionTargetQpc, targetIntervalTicks, qpcFreq.QuadPart);
            };
            const auto activeDelayLateResidualUsForIndex = [&](size_t candidateIndex) -> uint32_t {
                if (candidateIndex >= bufferedWgcFrames.size() || effectiveSelectionTargetQpc <= 0 ||
                    qpcFreq.QuadPart <= 0) {
                    return 0u;
                }
                return activeDelayCandidateLateResidualUs(bufferedWgcFrames[candidateIndex]);
            };
            if (!preferUniformActiveDelayCadence && selectionDelayApplied && candidateIndices &&
                !candidateIndices->empty() &&
                !ce::capture_policy::IsWgcActiveDelaySourceLimitedClass(activeDelayWindowClassFor(true)) &&
                activeDelayLateResidualUsForIndex(selectedIndex) > activeDelaySoftLateTargetUs) {
                bool foundSoftCandidate = false;
                size_t softIndex = selectedIndex;
                int64_t bestSoftDistance = INT64_MAX;
                uint32_t bestSoftLateResidualUs = UINT32_MAX;
                for (size_t bufferedIndex = 0; bufferedIndex < bufferedWgcFrames.size(); ++bufferedIndex) {
                    const QueuedFrame& softCandidate = bufferedWgcFrames[bufferedIndex];
                    if (bufferedIndex >= bufferedWgcFrames.size() ||
                        softCandidate.timestamp <= lastEmittedWgcSourceQpc ||
                        !ce::capture_policy::IsWgcTimestampFreshEnough(softCandidate.timestamp,
                                                                       staleFallbackMinTimestampQpc) ||
                        !finalSelectionWithinActiveDelayHardLimit(bufferedIndex) ||
                        activeDelayLateResidualUsForIndex(bufferedIndex) > activeDelaySoftLateTargetUs) {
                        continue;
                    }
                    const int64_t candidateDistance = AbsoluteTimestampDistance(
                        GetFrameSelectionTimestamp(bufferedWgcFrames[bufferedIndex]), effectiveSelectionTargetQpc);
                    const uint32_t candidateLateResidualUs = activeDelayLateResidualUsForIndex(bufferedIndex);
                    if (!foundSoftCandidate || candidateDistance < bestSoftDistance ||
                        (candidateDistance == bestSoftDistance &&
                         (candidateLateResidualUs < bestSoftLateResidualUs ||
                          (candidateLateResidualUs == bestSoftLateResidualUs &&
                           GetFrameSelectionTimestamp(bufferedWgcFrames[bufferedIndex]) <
                               GetFrameSelectionTimestamp(bufferedWgcFrames[softIndex]))))) {
                        foundSoftCandidate = true;
                        softIndex = bufferedIndex;
                        bestSoftDistance = candidateDistance;
                        bestSoftLateResidualUs = candidateLateResidualUs;
                    }
                }
                if (foundSoftCandidate) {
                    selectedIndex = softIndex;
                    olderFrameAvoidedRepeatThisTick = true;
                    ++wgcDelayOlderFrameAvoidedRepeatWindow;
                    ++wgcDelayOlderFrameAvoidedRepeatTotal;
                }
            }
            if (!finalSelectionWithinActiveDelayHardLimit(selectedIndex) && candidateIndices &&
                !candidateIndices->empty()) {
                bool foundSafeCandidate = false;
                size_t rescueIndex = selectedIndex;
                int64_t bestDistance = INT64_MAX;
                for (const size_t bufferedIndex : *candidateIndices) {
                    if (bufferedIndex >= bufferedWgcFrames.size() ||
                        !finalSelectionWithinActiveDelayHardLimit(bufferedIndex)) {
                        continue;
                    }
                    const int64_t candidateDistance = AbsoluteTimestampDistance(
                        GetFrameSelectionTimestamp(bufferedWgcFrames[bufferedIndex]), effectiveSelectionTargetQpc);
                    if (!foundSafeCandidate || candidateDistance < bestDistance ||
                        (candidateDistance == bestDistance &&
                         GetFrameSelectionTimestamp(bufferedWgcFrames[bufferedIndex]) >
                             GetFrameSelectionTimestamp(bufferedWgcFrames[rescueIndex]))) {
                        foundSafeCandidate = true;
                        rescueIndex = bufferedIndex;
                        bestDistance = candidateDistance;
                    }
                }
                if (foundSafeCandidate) {
                    selectedIndex = rescueIndex;
                    ++wgcDelayPostSelectionRescuedSyncRiskWindow;
                    ++wgcDelayPostSelectionRescuedSyncRiskTotal;
                }
            }

            const QueuedFrame& candidate = bufferedWgcFrames[selectedIndex];
            const int64_t candidateSelectionTimestamp = GetFrameSelectionTimestamp(candidate);
            const int64_t candidateRawSelectionTimestamp = getWgcRawSelectionTimestamp(candidate);
            if (selectionDelayApplied && effectiveSelectionTargetQpc > 0 &&
                !ce::capture_policy::IsWgcActiveDelayFinalSelectionWithinHardLimit(
                    candidateSelectionTimestamp, candidateRawSelectionTimestamp, effectiveSelectionTargetQpc,
                    targetIntervalTicks, qpcFreq.QuadPart)) {
                ++wgcDelayPostSelectionRejectedSyncRiskWindow;
                ++wgcDelayPostSelectionRejectedSyncRiskTotal;
                const bool hardSafeCandidateAvailable = hasActiveDelayHardSafeCandidate();
                const bool softSafeCandidateAvailable = hasActiveDelaySoftSafeCandidate();
                recordActiveDelayRepeatLowerBound(softSafeCandidateAvailable,
                                                  isCurrentSyncDelayHoldSourceLimited(softSafeCandidateAvailable));
                recordSyncDelayRepeatHold(true, hardSafeCandidateAvailable, softSafeCandidateAvailable);
                static uint64_t s_lastWgcFinalSelectionRejectLogTick = 0;
                const uint64_t nowTick = GetTickCount64();
                if (nowTick - s_lastWgcFinalSelectionRejectLogTick >= 1000 ||
                    wgcDelayPostSelectionRejectedSyncRiskWindow <= 3) {
                    const int64_t predictedLeadUs =
                        qpcFreq.QuadPart > 0
                            ? ((candidateSelectionTimestamp - effectiveSelectionTargetQpc) * 1000000) / qpcFreq.QuadPart
                            : 0;
                    const int64_t rawLeadUs =
                        qpcFreq.QuadPart > 0 && candidateRawSelectionTimestamp > 0
                            ? ((candidateRawSelectionTimestamp - effectiveSelectionTargetQpc) * 1000000) /
                                  qpcFreq.QuadPart
                            : 0;
                    LogWarn(
                        "[EncoderThread] WGC active-delay final selection rejected: predictedLead=%lldus "
                        "rawLead=%lldus selectedIndex=%zu buffered=%zu targetQpc=%lld predictedQpc=%lld "
                        "rawQpc=%lld",
                        static_cast<long long>(predictedLeadUs), static_cast<long long>(rawLeadUs), selectedIndex,
                        bufferedWgcFrames.size(), static_cast<long long>(effectiveSelectionTargetQpc),
                        static_cast<long long>(candidateSelectionTimestamp),
                        static_cast<long long>(candidateRawSelectionTimestamp));
                    s_lastWgcFinalSelectionRejectLogTick = nowTick;
                }
                if (repeatedBecauseNoFrameCoverage) {
                    *repeatedBecauseNoFrameCoverage = true;
                }
                return false;
            }
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
            const bool selectedActiveDelayCandidateRelaxed =
                selectionDelayApplied && effectiveSelectionTargetQpc > 0 &&
                ce::capture_policy::IsWgcFrameTooNewForActiveDelaySlot(
                    candidateSelectionTimestamp, effectiveSelectionTargetQpc, targetIntervalTicks);
            if (selectionDelayApplied && skippedTooNewForSlot && !selectedActiveDelayCandidateRelaxed &&
                !olderFrameAvoidedRepeatThisTick) {
                ++wgcDelayOlderFrameAvoidedRepeatWindow;
                ++wgcDelayOlderFrameAvoidedRepeatTotal;
            }
            if (selectedActiveDelayCandidateRelaxed && qpcFreq.QuadPart > 0) {
                const uint32_t residualUs = SaturatingToUint32(static_cast<uint64_t>(
                    (candidateSelectionTimestamp - effectiveSelectionTargetQpc) * 1000000 / qpcFreq.QuadPart));
                ++wgcDelayRelaxedSelectionCount;
                ++wgcDelayRelaxedSelectionWindowCount;
                wgcDelayRelaxedSelectionMaxUs = std::max(wgcDelayRelaxedSelectionMaxUs, residualUs);
                const int64_t repeatSelectionTimestamp = g_HasLastFrame ? GetFrameSelectionTimestamp(g_LastFrame) : 0;
                const auto relaxedScore = ce::capture_policy::ScoreWgcActiveDelayRelaxedCandidate(
                    candidateSelectionTimestamp, repeatSelectionTimestamp, effectiveSelectionTargetQpc,
                    targetIntervalTicks, qpcFreq.QuadPart, activeDelayRepeatClusterTicks(),
                    currentDelayResidualAvgAbsUs(), currentDelayResidualP95Us(), currentDelayResidualLateMaxUs(),
                    activeDelayWindowClassFor(true), activeDelaySoftLateTargetUs);
                if (relaxedScore.decision == ce::capture_policy::WgcActiveDelayRelaxedDecision::kAcceptBetterTarget) {
                    ++wgcDelayRelaxedBetterTargetWindow;
                    ++wgcDelayRelaxedBetterTargetTotal;
                } else if (relaxedScore.decision ==
                           ce::capture_policy::WgcActiveDelayRelaxedDecision::kAcceptRepeatCluster) {
                    ++wgcDelayRelaxedRepeatClusterWindow;
                    ++wgcDelayRelaxedRepeatClusterTotal;
                }
            }
            const auto selectedActiveDelayWindowClass =
                selectionDelayApplied && isActiveDelayCandidateHardSafe(candidate) ? activeDelayWindowClassFor(true)
                                                                                   : activeDelayWindowClassFor(false);
            const bool selectedPostStallSafeFrame =
                selectionDelayApplied &&
                selectedActiveDelayWindowClass == ce::capture_policy::WgcActiveDelayWindowClass::kPostStallRecovery;
            const bool canHoldFreshFrame =
                selectionDelayApplied && !selectedPostStallSafeFrame && selectedIndex == 0 &&
                bufferedWgcFrames.size() == 1 &&
                ce::capture_policy::ShouldHoldSingleFreshWgcFrame(
                    wgcReservePressureActive, lowSourceMode, wgcRecentInputMin250Fps, outputFps, smoothedInputPerTick,
                    outputShortfallTicks, g_IsEncoderBottlenecked.load(std::memory_order_relaxed), false,
                    deepUnderfeed);
            if (canHoldFreshFrame && effectiveSelectionTargetQpc > 0 &&
                ShouldHoldFrameForNextTick(candidateSelectionTimestamp, effectiveSelectionTargetQpc,
                                           targetIntervalTicks, targetIntervalTicks / 10)) {
                const bool softSafeCandidateAvailable = isActiveDelayCandidateSoftSafe(candidate);
                recordActiveDelayRepeatLowerBound(softSafeCandidateAvailable,
                                                  isCurrentSyncDelayHoldSourceLimited(softSafeCandidateAvailable));
                ++wgcHoldForNextTickCount;
                ++wgcHeldFreshFrameTickCount;
                if (repeatedBecauseNoFrameCoverage) {
                    *repeatedBecauseNoFrameCoverage = true;
                }
                return false;
            }
            if (selectedPostStallSafeFrame) {
                ++wgcDelayPostStallSafeFrameWindow;
                ++wgcDelayPostStallSafeFrameTotal;
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

            if (preferUniformActiveDelayCadence) {
                ++wgcDelayUniformCadenceWindow;
                ++wgcDelayUniformCadenceTotal;
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

        auto inspectBufferedWgcCoverageForTarget = [&](int64_t selectionTargetQpc, bool activeDelaySelection,
                                                       uint32_t requiredReserveFrames, bool* hasFrameForTick,
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
            const bool canUseCandidateNow =
                selectionTargetQpc <= 0 || candidateSelectionTimestamp <= 0 ||
                !(activeDelaySelection ? ce::capture_policy::IsWgcFrameTooNewForActiveDelaySlot(
                                             candidateSelectionTimestamp, selectionTargetQpc, targetIntervalTicks)
                                       : ce::capture_policy::IsWgcFrameTooNewForCfrSlot(
                                             candidateSelectionTimestamp, selectionTargetQpc, targetIntervalTicks)) ||
                !g_HasLastFrame || g_LastFrame.isInjectMode;
            if (hasFrameForTick) {
                *hasFrameForTick = canUseCandidateNow;
            }
            if (hasReserveFrame) {
                const uint32_t reserveFrames = static_cast<uint32_t>(
                    std::min<size_t>(bufferedWgcFrames.size() - idx, static_cast<size_t>(UINT32_MAX)));
                *hasReserveFrame = canUseCandidateNow && reserveFrames >= std::max<uint32_t>(1u, requiredReserveFrames);
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
                if (auto capture = g_WgcCap.Read()) {
                    const uint64_t drainSourceEpoch = g_WgcSourceEpoch.load(std::memory_order_acquire);
                    capture->DrainPendingFrames(drainedWgcCapturedFrames, 0);
                    for (auto& capturedFrame : drainedWgcCapturedFrames) {
                        if (!capturedFrame.texture) {
                            continue;
                        }
                        if (capturedFrame.sourceEpoch != drainSourceEpoch) {
                            static uint64_t s_retiredPullFrameDrops = 0;
                            ++s_retiredPullFrameDrops;
                            if (s_retiredPullFrameDrops <= 3 || (s_retiredPullFrameDrops % 120ull) == 0ull) {
                                LogInfo(
                                    "[WGC] Dropping retired-source pull frame: frameEpoch=%llu activeEpoch=%llu "
                                    "discarded=%llu",
                                    static_cast<unsigned long long>(capturedFrame.sourceEpoch),
                                    static_cast<unsigned long long>(drainSourceEpoch),
                                    static_cast<unsigned long long>(s_retiredPullFrameDrops));
                            }
                            ReleaseWgcCapturedFrame(capturedFrame);
                            continue;
                        }
                        drainedScreenGrabFrames.push_back(MakeQueuedWgcFrame(std::move(capturedFrame)));
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
                            // Monotonic bounded-deviation smoothing of the raw compositor timestamp.
                            // WGC/DXGI timestamps are DWM composition times and arrive quantized
                            // under VRR/composed presentation even when the game presents perfectly
                            // smoothly; a CFR playout slaved to the raw stamps converts a surplus
                            // source into constant single-tick repeats (fortistutter root cause).
                            // The raw timestamp stays untouched for sync validation/diagnostics.
                            drainedFrame.selectionTimestamp =
                                wgcInputPredictor.SmoothMonotonicTimestamp(drainedFrame.timestamp, targetIntervalTicks);
                            observeCaptureSyncPhaseSource(
                                "wgc", wgcCfrPhaseLock,
                                GetFrameSelectionTimestamp(drainedFrame));
                            if (drainedFrame.selectionTimestamp > 0 && qpcFreq.QuadPart > 0) {
                                const int64_t devQpc =
                                    AbsoluteTimestampDistance(drainedFrame.selectionTimestamp, drainedFrame.timestamp);
                                const uint32_t devUs = SaturatingToUint32(static_cast<uint64_t>(devQpc) * 1000000ull /
                                                                          static_cast<uint64_t>(qpcFreq.QuadPart));
                                ++wgcTsSmoothSamplesWindow;
                                wgcTsSmoothDevAccumUsWindow += devUs;
                                wgcTsSmoothDevMaxUsWindow = std::max(wgcTsSmoothDevMaxUsWindow, devUs);
                                wgcTsSmoothDevMaxUsTotal = std::max(wgcTsSmoothDevMaxUsTotal, devUs);
                                const uint64_t snapTotal = wgcInputPredictor.SmoothingSnapCount();
                                if (snapTotal > wgcTsSmoothSnapCountTotal) {
                                    wgcTsSmoothSnapCountWindow +=
                                        static_cast<uint32_t>(snapTotal - wgcTsSmoothSnapCountTotal);
                                    wgcTsSmoothSnapCountTotal = snapTotal;
                                }
                            }
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
                if (recordingOutputLive && !bufferedWgcFrames.empty()) {
                    trimBufferedWgcForPoolPressure("live-pool-pressure");
                }
                trimBufferedWgcToRetainedCap(recordingOutputLive ? "live-buffer" : "warmup-buffer");

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
                const uint32_t wgcPolicySourceJitterUs =
                    g_WgcCap ? SaturatingToUint32(g_WgcCap->GetSourceJitterAvgUs()) : 0u;
                const uint32_t wgcPolicyPredictorJitterUs =
                    wgcInputPredictor.IsCalibrated()
                        ? SaturatingToUint32(static_cast<uint64_t>(wgcInputPredictor.GetJitterUs(qpcFreq.QuadPart)))
                        : 0u;
                const uint32_t wgcPolicyAverageJitterUs = std::max(wgcPolicySourceJitterUs, wgcPolicyPredictorJitterUs);

                wgcCoverageDelayTicksCurrent = 0;

                const uint64_t wgcPolicyNowTick = GetTickCount64();
                const ce::capture_policy::WgcAdaptiveTelemetry wgcAdaptiveTelemetry = {
                    outputFps,
                    wgcRecentDeliveredFps,
                    wgcRecentDeliveredMin250Fps,
                    wgcRecentDeliveredMin500Fps,
                    wgcRecentInputMin250Fps,
                    wgcRecentInputMin500Fps,
                    wgcPolicyAverageJitterUs,
                    wgcNoFreshTickPermille,
                    static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull)),
                    0u,
                    0.0,
                };
                const bool allowWgcLiveRecoveryMode =
                    recordingOutputLive && g_Recording.load(std::memory_order_acquire);
                static uint64_t s_lastWgcWarmupLogTick = 0;
                const bool inWgcWarmup =
                    wgcWarmupUntilQpc > 0 && liveTicksOutput < static_cast<uint64_t>(std::max(24u, outputFps / 6u));
                if (inWgcWarmup && s_lastWgcWarmupLogTick == 0) {
                    s_lastWgcWarmupLogTick = GetTickCount64();
                    LogInfo("[WGC CFR] Warmup active: %llu ticks to stabilize capture pipeline",
                            static_cast<unsigned long long>(std::max(24u, outputFps / 6u)));
                } else if (!inWgcWarmup && s_lastWgcWarmupLogTick > 0 &&
                           (wgcPolicyNowTick - s_lastWgcWarmupLogTick) >= 1000) {
                    LogInfo("[WGC CFR] Warmup ended: liveTicksOutput=%llu buffered=%zu",
                            static_cast<unsigned long long>(liveTicksOutput), bufferedWgcFrames.size());
                    s_lastWgcWarmupLogTick = 0;
                }
                const bool wgcSourceHealthTelemetryReady =
                    !inWgcWarmup && allowWgcLiveRecoveryMode &&
                    liveTicksOutput >= std::max<uint64_t>(8ull, outputFps / 8u) && wgcRecentDeliveredMin250Fps > 0 &&
                    wgcRecentDeliveredMin500Fps > 0 && wgcRecentInputMin250Fps > 0 && wgcRecentInputMin500Fps > 0;
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
                const bool bufferedReserveRecovered =
                    isWgcEffectiveContentDelayActive()
                        ? ce::capture_policy::IsWgcDelayReservoirRecovered(
                              bufferedWgcFrames.size(), getWgcEffectiveContentDelayQpc(), targetIntervalTicks)
                        : bufferedWgcFrames.size() >= 3;
                const uint64_t wgcLowSourceDurationMs =
                    wgcLowSourceModeActive && wgcPolicyNowTick >= wgcLowSourceStateChangeTick
                        ? (wgcPolicyNowTick - wgcLowSourceStateChangeTick)
                        : 0;
                const bool stableWgcUnderfeed =
                    wgcSourceHealthTelemetryReady &&
                    wgcLowSourceDurationMs >= ce::capture_policy::kWgcStableUnderfeedClassificationMs &&
                    wgcRecentDeliveredMin250Fps > 0 && wgcRecentInputMin250Fps > 0;
                const bool shouldExitWgcLowSourceMode =
                    ce::capture_policy::ShouldExitWgcLowSourceMode(wgcAdaptiveTelemetry, encoderTooSlowForTargetCurrent,
                                                                   bufferedReserveRecovered, wgcLowSourceDurationMs);
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
                        wgcAdaptiveTelemetry, outputShortfallTicks, wgcCapacityPressureForRecovery, stableWgcUnderfeed);
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
                        if (g_WgcCap) {
                            wgcStarvedEpisode.startPoolSaturatedDrops = g_WgcCap->GetPoolSaturatedDropCount();
                            wgcStarvedEpisode.startPoolOverwritePrevented =
                                g_WgcCap->GetPoolSlotOverwritePreventedCount();
                            wgcStarvedEpisode.startIngressDecimated = g_WgcCap->GetIngressDecimatedCount();
                        }
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

                const bool activeDelaySevereSourceStall =
                    isWgcEffectiveContentDelayActive() && wgcSourceHealthTelemetryReady &&
                    ce::capture_policy::IsWgcSevereSourceStallForActiveDelay(
                        outputFps, wgcRecentDeliveredMin250Fps, wgcRecentInputMin250Fps, wgcNoFreshTickPermille,
                        wgcBufferedFramesForPolicy);
                if (activeDelaySevereSourceStall) {
                    const uint64_t recoveryUntil =
                        wgcPolicyNowTick + ce::capture_policy::kWgcActiveDelaySourceRecoveryHoldMs;
                    const bool enteringRecovery = wgcActiveDelaySourceRecoveryUntilTick <= wgcPolicyNowTick;
                    wgcActiveDelaySourceRecoveryUntilTick =
                        std::max<uint64_t>(wgcActiveDelaySourceRecoveryUntilTick, recoveryUntil);
                    if (enteringRecovery) {
                        LogInfo(
                            "[WGC CFR] Active-delay source recovery entered: src=%u/%u input=%u/%u empty=%upm "
                            "buffered=%u holdMs=%u",
                            wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps, wgcRecentInputMin250Fps,
                            wgcRecentInputMin500Fps, wgcNoFreshTickPermille, wgcBufferedFramesForPolicy,
                            ce::capture_policy::kWgcActiveDelaySourceRecoveryHoldMs);
                    }
                }
                if (isWgcEffectiveContentDelayActive() && wgcActiveDelaySourceRecoveryUntilTick > wgcPolicyNowTick) {
                    ++wgcActiveDelaySourceRecoveryTicks;
                }

                if (g_WgcCap && recordingOutputLive && g_Recording) {
                    const uint32_t currentTargetFps = g_WgcCap->GetProducerTargetFps();
                    if (currentTargetFps != 0) {
                        LogError(
                            "[WGC CFR] ERROR: producer contract violation: backend=%s outputFps=%u "
                            "producerTargetFps=%u; forcing MinUpdateInterval=0 because finite producer intervals "
                            "alias variable-rate input",
                            g_WgcCap->IsUsingDesktopDuplication() ? "dxgi_dup" : "wgc", outputFps, currentTargetFps);
                        g_WgcCap->SetProducerTargetFps(0);
                        g_WgcProducerTargetFps.store(0, std::memory_order_relaxed);
                        ++wgcProducerRateRetuneCount;
                        ++wgcProducerRateRetuneTotal;
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
                    // On the uniform-cadence active-delay path the content delay must be maintained
                    // continuously. A VRR / GPU-bound source running below the output target is the
                    // STEADY STATE (absorbed by even holds at the delay floor + setpoint cap), not a
                    // reason to abandon the delay. Live-recovery only exits once the source outruns
                    // the output target, so letting it disable the selection delay collapses the
                    // realized content delay to ~0 and latches there for tens of seconds (the collapse
                    // half of the realized-delay rubber-band). Keep the delay applied; live-recovery
                    // still drives capture-rate refill. The legacy (non-uniform) reservoir path keeps
                    // its original behavior of yielding to live-recovery.
                    const int64_t effectiveContentDelayQpc = getWgcEffectiveContentDelayQpc();
                    const bool uniformCadenceActiveDelay =
                        effectiveContentDelayQpc > 0 && config.wgcActiveDelayUniformCadence;
                    const bool liveRecoverySuppressesDelay =
                        ce::capture_policy::ShouldLiveRecoverySuppressWgcSelectionDelay(wgcLiveRecoveryModeActive,
                                                                                        uniformCadenceActiveDelay);
                    const bool activeDelayInspection =
                        effectiveContentDelayQpc > 0 && recordingOutputLive && !liveRecoverySuppressesDelay;
                    const uint32_t requiredReservoirFrames =
                        activeDelayInspection ? std::max<uint32_t>(1u, getWgcDelayReservoirLowWaterFrames()) : 1u;
                    const int64_t reservoirInspectionTargetQpc =
                        activeDelayInspection
                            ? clampWgcSelectionTargetQpc(computeDelayedWgcSelectionTargetQpc(),
                                                         selectionNowQpc.QuadPart)
                            : clampWgcSelectionTargetQpc(computeWgcSelectionTargetQpc(false), selectionNowQpc.QuadPart);
                    inspectBufferedWgcCoverageForTarget(reservoirInspectionTargetQpc, activeDelayInspection,
                                                        requiredReservoirFrames, &wgcFreshAvailableAtTickStart,
                                                        &wgcReserveAvailableAtTickStart);
                    wgcSelectionDelayAppliedThisTick =
                        !liveRecoverySuppressesDelay &&
                        ce::capture_policy::ShouldApplyWgcSelectionDelay(
                            recordingOutputLive, outputShortfallTicks,
                            g_IsEncoderBottlenecked.load(std::memory_order_relaxed), wgcReserveAvailableAtTickStart,
                            effectiveContentDelayQpc > 0);
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
                    int64_t effectiveSelectionTargetQpc =
                        wgcSelectionDelayAppliedThisTick ? delayedSelectionTargetQpc : liveSelectionTargetQpc;
                    if (wgcLowSourceModeActive && !inWgcWarmup && targetIntervalTicks > 0 &&
                        !bufferedWgcFrames.empty()) {
                        const int64_t newestQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.back());
                        if (newestQpc > 0 && effectiveSelectionTargetQpc > newestQpc + targetIntervalTicks) {
                            const int64_t drift = effectiveSelectionTargetQpc - (newestQpc + targetIntervalTicks);
                            wgcBiasAccumQpc += drift;
                            effectiveSelectionTargetQpc = newestQpc + targetIntervalTicks;
                            ++wgcBiasClampCount;
                        } else if (wgcBiasAccumQpc > 0) {
                            wgcBiasAccumQpc = std::max<int64_t>(0, wgcBiasAccumQpc - targetIntervalTicks * 2);
                        }
                    }
                    const bool useInjectParityDelayPacing = wgcSelectionDelayAppliedThisTick &&
                                                            isWgcEffectiveContentDelayActive() &&
                                                            config.wgcActiveDelayUniformCadence;
                    if (!useInjectParityDelayPacing) {
                        const int64_t phaseReferenceQpc = bufferedWgcFrames.empty()
                                                                  ? 0
                                                                  : GetFrameSelectionTimestamp(bufferedWgcFrames.back());
                        effectiveSelectionTargetQpc = applyCaptureSyncPhaseTarget(
                            "wgc", wgcCfrPhaseLock, effectiveSelectionTargetQpc, phaseReferenceQpc);
                    }
                    if (useInjectParityDelayPacing) {
                        // Fixed-latency jitter-buffer playout for the active A/V content delay. WGC
                        // delivery is bursty/gappy under a GPU-bound VRR borderless source: DWM hands
                        // frames to the capture pool in late batches even when the game itself presents
                        // perfectly smoothly (real signature: game present max-interval ~10 ms while WGC
                        // delivery dips to 24-110 fps with 170-200 ms callback gaps). The previous
                        // oldest-first + count-based depth cap turned that delivery jitter into harsh
                        // stutter -- a ~115 fps source manufactured ~20 dups AND ~14 drops in the SAME
                        // second (simultaneous drop+dup churn), and oldest-first emission rubber-banded
                        // the realized content delay 0..243 ms against a 30 ms target.
                        //
                        // Instead select the buffered frame nearest the grid playout target
                        // (gridTick - contentDelay): drop frames the audio timeline has already passed,
                        // emit the slot frame once it has aged in, otherwise hold (an evenly distributed
                        // source-limited / delivery-gap repeat) leaving newer frames as reserve. This
                        // consumes unique frames at the SOURCE rate by construction (no over-drain, no
                        // clustered "too new" holds like the old grid-rate reservoir) and pins the
                        // realized delay near the target regardless of delivery burstiness. After a true
                        // gap it resumes at the correct delay by dropping the audio-passed backlog
                        // (replaying it would put video behind audio) so the in-gap freeze stays clean.
                        // All uniform-playout decisions run in the SMOOTHED selection-timestamp
                        // domain (strictly monotonic, quantization noise removed); raw timestamps
                        // stay available for sync validation, stop boundaries, and diagnostics.
                        while (!bufferedWgcFrames.empty() && lastEmittedWgcSelectionQpc > 0 &&
                               GetFrameSelectionTimestamp(bufferedWgcFrames.front()) > 0 &&
                               GetFrameSelectionTimestamp(bufferedWgcFrames.front()) <= lastEmittedWgcSelectionQpc) {
                            QueuedFrame stale = std::move(bufferedWgcFrames.front());
                            bufferedWgcFrames.pop_front();
                            ReleaseQueuedFrameTexture(stale);
                            ++wgcDropObsoleteCount;
                        }
                        // Grid-anchored content-delay target (UNCLAMPED toward live: the uniform-cadence
                        // path maintains the delay through low-source/recovery rather than clamping
                        // toward live, which would re-collapse the realized delay -- the other half of
                        // the rubber-band).
                        const int64_t effectiveContentDelayQpc = getWgcEffectiveContentDelayQpc();
                        int64_t playoutTargetQpc = (encoderGridStartQpc > 0 && targetIntervalTicks > 0)
                                                       ? computeDelayedWgcSelectionTargetQpc()
                                                       : 0;
                        const int64_t phaseReferenceQpc = bufferedWgcFrames.empty()
                                                                  ? 0
                                                                  : GetFrameSelectionTimestamp(bufferedWgcFrames.back());
                        playoutTargetQpc = applyCaptureSyncPhaseTarget(
                            "wgc", wgcCfrPhaseLock, playoutTargetQpc, phaseReferenceQpc);
                        const int64_t playoutLeadToleranceQpc =
                            ce::capture_policy::GetWgcActiveDelayResidualToleranceQpc(targetIntervalTicks);
                        // Anti-freeze floor: if the encoder grid has drifted so far behind wall-clock
                        // that even the OLDEST buffered frame is "too new" for this slot, the grid target
                        // would hold/repeat every tick while fresh frames pile up and drop stale -- a
                        // multi-second hard freeze. Only raise the slot target when the oldest frame is
                        // still old enough to preserve the active content delay. Otherwise this is an
                        // underfilled WGC reserve, not encoder-grid drift, and advancing toward near-live
                        // would trade the freeze for a visible A/V-delay collapse.
                        if (!bufferedWgcFrames.empty()) {
                            const int64_t oldestBufferedSlotQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.front());
                            const int64_t oldestBufferedAgeQpc =
                                selectionNowQpc.QuadPart > oldestBufferedSlotQpc
                                    ? (selectionNowQpc.QuadPart - oldestBufferedSlotQpc)
                                    : 0;
                            const int64_t antiFreezeTargetQpc =
                                ce::capture_policy::ApplyWgcUniformPlayoutAntiFreezeFloor(
                                    playoutTargetQpc, oldestBufferedSlotQpc, targetIntervalTicks);
                            const bool antiFreezeSyncSafe =
                                ce::capture_policy::IsWgcUniformPlayoutAntiFreezeFloorSyncSafe(
                                    oldestBufferedAgeQpc, effectiveContentDelayQpc, targetIntervalTicks);
                            if (antiFreezeTargetQpc > playoutTargetQpc && antiFreezeSyncSafe) {
                                ++wgcUniformAntiFreezeFloorTotal;
                                static uint64_t s_lastAntiFreezeLogTick = 0;
                                const uint64_t nowAntiFreezeTick = GetTickCount64();
                                if (nowAntiFreezeTick - s_lastAntiFreezeLogTick >= 1000) {
                                    s_lastAntiFreezeLogTick = nowAntiFreezeTick;
                                    const int64_t driftUs = qpcToUs(antiFreezeTargetQpc - playoutTargetQpc);
                                    const int64_t oldestAgeUs = qpcToUs(oldestBufferedAgeQpc);
                                    const int64_t effectiveDelayUs = qpcToUs(effectiveContentDelayQpc);
                                    LogWarn(
                                        "[WGC CFR] Uniform playout anti-freeze floor engaged: grid target drifted "
                                        "%lldus behind the reserve (even the oldest buffered frame was too-new for "
                                        "the slot); advancing to the oldest sync-safe frame to resume playout. "
                                        "buffered=%zu oldestAge=%lldus effectiveDelay=%lldus total=%llu "
                                        "(A/V PTS unchanged)",
                                        static_cast<long long>(driftUs), bufferedWgcFrames.size(),
                                        static_cast<long long>(oldestAgeUs), static_cast<long long>(effectiveDelayUs),
                                        static_cast<unsigned long long>(wgcUniformAntiFreezeFloorTotal));
                                }
                                playoutTargetQpc = antiFreezeTargetQpc;
                            } else if (antiFreezeTargetQpc > playoutTargetQpc) {
                                ++wgcUniformAntiFreezeFloorSkippedSyncTotal;
                                static uint64_t s_lastAntiFreezeSkippedLogTick = 0;
                                const uint64_t nowAntiFreezeSkippedTick = GetTickCount64();
                                if (nowAntiFreezeSkippedTick - s_lastAntiFreezeSkippedLogTick >= 1000) {
                                    s_lastAntiFreezeSkippedLogTick = nowAntiFreezeSkippedTick;
                                    const int64_t driftUs = qpcToUs(antiFreezeTargetQpc - playoutTargetQpc);
                                    const int64_t oldestAgeUs = qpcToUs(oldestBufferedAgeQpc);
                                    const int64_t effectiveDelayUs = qpcToUs(effectiveContentDelayQpc);
                                    LogInfo(
                                        "[WGC CFR] Uniform playout anti-freeze floor skipped to preserve content "
                                        "delay: drift=%lldus buffered=%zu oldestAge=%lldus effectiveDelay=%lldus "
                                        "skipped=%llu (reserve underfilled; waiting for sync-safe WGC content)",
                                        static_cast<long long>(driftUs), bufferedWgcFrames.size(),
                                        static_cast<long long>(oldestAgeUs), static_cast<long long>(effectiveDelayUs),
                                        static_cast<unsigned long long>(wgcUniformAntiFreezeFloorSkippedSyncTotal));
                                }
                            }
                        }
                        const uint32_t uniformActiveDelaySoftLateTargetUs =
                            ce::capture_policy::GetWgcActiveDelaySoftLateTargetUs(targetIntervalTicks,
                                                                                  qpcFreq.QuadPart);
                        const bool uniformDeepUnderfeed = ce::capture_policy::IsWgcDeepUnderfeed(
                            outputFps, wgcRecentDeliveredMin250Fps, wgcRecentInputMin250Fps, wgcNoFreshTickPermille);
                        ce::capture_policy::WgcAdaptiveTelemetry uniformActiveDelayTelemetry{};
                        uniformActiveDelayTelemetry.outputFps = outputFps;
                        uniformActiveDelayTelemetry.recentDeliveredFps = wgcRecentDeliveredFps;
                        uniformActiveDelayTelemetry.recentDeliveredMin250Fps = wgcRecentDeliveredMin250Fps;
                        uniformActiveDelayTelemetry.recentDeliveredMin500Fps = wgcRecentDeliveredMin500Fps;
                        uniformActiveDelayTelemetry.recentInputMin250Fps = wgcRecentInputMin250Fps;
                        uniformActiveDelayTelemetry.recentInputMin500Fps = wgcRecentInputMin500Fps;
                        const uint32_t uniformWgcSourceJitterAvgUs =
                            g_WgcCap ? SaturatingToUint32(g_WgcCap->GetSourceJitterAvgUs()) : 0u;
                        const uint32_t uniformWgcPredictorJitterUs =
                            wgcInputPredictor.IsCalibrated() ? SaturatingToUint32(static_cast<uint64_t>(
                                                                   wgcInputPredictor.GetJitterUs(qpcFreq.QuadPart)))
                                                             : 0u;
                        uniformActiveDelayTelemetry.averageJitterUs =
                            std::max(uniformWgcSourceJitterAvgUs, uniformWgcPredictorJitterUs);
                        uniformActiveDelayTelemetry.emptyTickPermille = wgcNoFreshTickPermille;
                        uniformActiveDelayTelemetry.bufferedWgcFrames = static_cast<uint32_t>(
                            std::min<size_t>(bufferedWgcFrames.size(), static_cast<size_t>(UINT32_MAX)));
                        const auto uniformRepeatClusterTicks = [&]() -> uint32_t {
                            return std::max<uint32_t>(
                                cadenceCounters.consecutiveDuplicateFrames,
                                cadenceCounters.holdTicksRunning > 1 ? (cadenceCounters.holdTicksRunning - 1) : 0);
                        };
                        const auto uniformDelayResidualAvgAbsUs = [&]() -> uint32_t {
                            if (wgcDelayResidualWindowSamples > 0) {
                                return SaturatingToUint32(wgcDelayResidualWindowAbsAccumUs /
                                                          wgcDelayResidualWindowSamples);
                            }
                            return wgcDelayResidualSamples > 0
                                       ? SaturatingToUint32(wgcDelayResidualAbsAccumUs / wgcDelayResidualSamples)
                                       : 0u;
                        };
                        const auto uniformRawDelayResidualAvgAbsUs = [&]() -> uint32_t {
                            if (wgcDelayRawResidualWindowSamples > 0) {
                                return SaturatingToUint32(wgcDelayRawResidualWindowAbsAccumUs /
                                                          wgcDelayRawResidualWindowSamples);
                            }
                            return wgcDelayRawResidualSamples > 0
                                       ? SaturatingToUint32(wgcDelayRawResidualAbsAccumUs / wgcDelayRawResidualSamples)
                                       : 0u;
                        };
                        const auto uniformDelayResidualP95Us = [&]() -> uint32_t {
                            const uint32_t windowP95 = wgcDelayResidualWindowP95Us();
                            return windowP95 > 0 ? windowP95 : wgcDelayResidualP95Us();
                        };
                        const auto uniformRawDelayResidualP95Us = [&]() -> uint32_t {
                            const uint32_t windowP95 = wgcDelayRawResidualWindowP95Us();
                            return windowP95 > 0 ? windowP95 : wgcDelayRawResidualP95Us();
                        };
                        const auto uniformDelayResidualLateMaxUs = [&]() -> uint32_t {
                            return wgcDelayResidualWindowLateMaxUs > 0 ? wgcDelayResidualWindowLateMaxUs
                                                                       : wgcDelayResidualLateMaxUs;
                        };
                        const auto uniformRawDelayResidualLateMaxUs = [&]() -> uint32_t {
                            return wgcDelayRawResidualWindowLateMaxUs > 0 ? wgcDelayRawResidualWindowLateMaxUs
                                                                          : wgcDelayRawResidualLateMaxUs;
                        };
                        const auto uniformCombinedDelayResidualAvgAbsUs = [&]() -> uint32_t {
                            return std::max(uniformDelayResidualAvgAbsUs(), uniformRawDelayResidualAvgAbsUs());
                        };
                        const auto uniformCombinedDelayResidualP95Us = [&]() -> uint32_t {
                            return std::max(uniformDelayResidualP95Us(), uniformRawDelayResidualP95Us());
                        };
                        const auto uniformCombinedDelayResidualLateMaxUs = [&]() -> uint32_t {
                            return std::max(uniformDelayResidualLateMaxUs(), uniformRawDelayResidualLateMaxUs());
                        };
                        const auto uniformCandidateHardSafeForTarget = [&](const QueuedFrame& candidate,
                                                                           int64_t targetQpc) -> bool {
                            if (targetQpc <= 0 || targetIntervalTicks <= 0 || candidate.timestamp <= 0 ||
                                GetFrameSelectionTimestamp(candidate) <= lastEmittedWgcSelectionQpc) {
                                return false;
                            }
                            return ce::capture_policy::IsWgcActiveDelayFinalSelectionWithinHardLimit(
                                GetFrameSelectionTimestamp(candidate), getWgcRawSelectionTimestamp(candidate),
                                targetQpc, targetIntervalTicks, qpcFreq.QuadPart);
                        };
                        const auto uniformCandidateSoftSafeForTarget = [&](const QueuedFrame& candidate,
                                                                           int64_t targetQpc) -> bool {
                            if (!uniformCandidateHardSafeForTarget(candidate, targetQpc)) {
                                return false;
                            }
                            return ce::capture_policy::IsWgcActiveDelayFinalSelectionWithinSoftLateTarget(
                                GetFrameSelectionTimestamp(candidate), getWgcRawSelectionTimestamp(candidate),
                                targetQpc, targetIntervalTicks, qpcFreq.QuadPart, uniformActiveDelaySoftLateTargetUs);
                        };
                        const auto uniformHasHardSafeCandidateForTarget = [&](int64_t targetQpc) -> bool {
                            for (const QueuedFrame& candidate : bufferedWgcFrames) {
                                if (uniformCandidateHardSafeForTarget(candidate, targetQpc)) {
                                    return true;
                                }
                            }
                            return false;
                        };
                        const auto uniformHasSoftSafeCandidateForTarget = [&](int64_t targetQpc) -> bool {
                            for (const QueuedFrame& candidate : bufferedWgcFrames) {
                                if (uniformCandidateSoftSafeForTarget(candidate, targetQpc)) {
                                    return true;
                                }
                            }
                            return false;
                        };
                        const auto uniformRepeatReserveSpanUs = [&]() -> uint32_t {
                            if (bufferedWgcFrames.size() < 2 || qpcFreq.QuadPart <= 0) {
                                return 0u;
                            }
                            const int64_t firstQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.front());
                            const int64_t lastQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.back());
                            if (firstQpc <= 0 || lastQpc <= firstQpc) {
                                return 0u;
                            }
                            return SaturatingToUint32(
                                static_cast<uint64_t>((lastQpc - firstQpc) * 1000000 / qpcFreq.QuadPart));
                        };
                        const auto uniformOldestSoftSafeAgeUs = [&](int64_t targetQpc) -> uint32_t {
                            if (selectionNowQpc.QuadPart <= 0 || qpcFreq.QuadPart <= 0) {
                                return 0u;
                            }
                            uint32_t oldestAgeUs = 0;
                            for (const QueuedFrame& candidate : bufferedWgcFrames) {
                                if (!uniformCandidateSoftSafeForTarget(candidate, targetQpc)) {
                                    continue;
                                }
                                const int64_t selectionTimestamp = GetFrameSelectionTimestamp(candidate);
                                if (selectionTimestamp <= 0 || selectionNowQpc.QuadPart <= selectionTimestamp) {
                                    continue;
                                }
                                oldestAgeUs = std::max(
                                    oldestAgeUs,
                                    SaturatingToUint32(static_cast<uint64_t>(
                                        (selectionNowQpc.QuadPart - selectionTimestamp) * 1000000 / qpcFreq.QuadPart)));
                            }
                            return oldestAgeUs;
                        };
                        const auto uniformActiveDelayWindowClassFor = [&](bool hardSafeCandidateAvailable) {
                            const bool activeDelaySourceRecovery =
                                wgcActiveDelaySourceRecoveryUntilTick > GetTickCount64();
                            return ce::capture_policy::ClassifyWgcActiveDelayWindow(
                                uniformActiveDelayTelemetry, wgcLowSourceModeActive, wgcLiveRecoveryModeActive,
                                wgcSourceStarvedCurrent, uniformDeepUnderfeed, activeDelaySourceRecovery,
                                hardSafeCandidateAvailable);
                        };
                        const auto uniformSyncDelayHoldSourceLimited = [&](bool softSafeCandidateAvailable) -> bool {
                            if (!softSafeCandidateAvailable) {
                                return true;
                            }
                            const bool activeDelaySourceRecovery =
                                wgcActiveDelaySourceRecoveryUntilTick > GetTickCount64();
                            const bool sourceRecoveryWithoutSafeFrame =
                                activeDelaySourceRecovery && !softSafeCandidateAvailable;
                            return ce::capture_policy::IsWgcSyncDelayHoldSourceLimited(
                                outputFps, wgcRecentDeliveredMin250Fps, wgcRecentInputMin250Fps, wgcNoFreshTickPermille,
                                wgcSourceStarvedCurrent, wgcLowSourceModeActive, uniformDeepUnderfeed,
                                sourceRecoveryWithoutSafeFrame);
                        };
                        const auto recordUniformRepeatDiagnostics = [&](bool hardSafeCandidateAvailable,
                                                                        bool softSafeCandidateAvailable) {
                            ++wgcDelayUniformHoldWindow;
                            ++wgcDelayUniformHoldTotal;
                            const uint32_t repeatClusterTicks = uniformRepeatClusterTicks();
                            if (repeatClusterTicks > 0) {
                                ++wgcDelayRepeatClusterPressureWindow;
                                ++wgcDelayRepeatClusterPressureTotal;
                                wgcDelayRepeatClusterPressureWindowMaxTicks =
                                    std::max(wgcDelayRepeatClusterPressureWindowMaxTicks, repeatClusterTicks);
                                wgcDelayRepeatClusterPressureMaxTicks =
                                    std::max(wgcDelayRepeatClusterPressureMaxTicks, repeatClusterTicks);
                            }
                            ++wgcRepeatPolicyHoldCount;
                            ++wgcRepeatPolicyHoldTotal;
                            ++wgcSyncDelayHoldCount;
                            ++wgcSyncDelayHoldTotal;

                            const auto repeatWindowClass = uniformActiveDelayWindowClassFor(hardSafeCandidateAvailable);
                            switch (repeatWindowClass) {
                                case ce::capture_policy::WgcActiveDelayWindowClass::kHealthy:
                                    ++wgcDelayWindowHealthyRepeatWindow;
                                    ++wgcDelayWindowHealthyRepeatTotal;
                                    break;
                                case ce::capture_policy::WgcActiveDelayWindowClass::kRecoverableUnderfill:
                                    ++wgcDelayWindowRecoverableRepeatWindow;
                                    ++wgcDelayWindowRecoverableRepeatTotal;
                                    break;
                                case ce::capture_policy::WgcActiveDelayWindowClass::kSourceLimited:
                                    ++wgcDelayWindowSourceLimitedRepeatWindow;
                                    ++wgcDelayWindowSourceLimitedRepeatTotal;
                                    break;
                                case ce::capture_policy::WgcActiveDelayWindowClass::kHardSourceStall:
                                    ++wgcDelayWindowSourceLimitedRepeatWindow;
                                    ++wgcDelayWindowSourceLimitedRepeatTotal;
                                    ++wgcDelayWindowHardStallRepeatWindow;
                                    ++wgcDelayWindowHardStallRepeatTotal;
                                    break;
                                case ce::capture_policy::WgcActiveDelayWindowClass::kPostStallRecovery:
                                    ++wgcDelayWindowRecoverableRepeatWindow;
                                    ++wgcDelayWindowRecoverableRepeatTotal;
                                    ++wgcDelayWindowPostStallRepeatWindow;
                                    ++wgcDelayWindowPostStallRepeatTotal;
                                    break;
                            }
                            if (hardSafeCandidateAvailable) {
                                ++wgcDelayRepeatWithSafeCandidateWindow;
                                ++wgcDelayRepeatWithSafeCandidateTotal;
                            } else {
                                ++wgcDelayRepeatWithoutSafeCandidateWindow;
                                ++wgcDelayRepeatWithoutSafeCandidateTotal;
                            }
                            if (softSafeCandidateAvailable) {
                                ++wgcDelayRepeatWithSoftSafeCandidateWindow;
                                ++wgcDelayRepeatWithSoftSafeCandidateTotal;
                                const uint32_t oldestSoftSafeAgeUs = uniformOldestSoftSafeAgeUs(playoutTargetQpc);
                                wgcDelayOldestSoftSafeAgeWindowMaxUs =
                                    std::max(wgcDelayOldestSoftSafeAgeWindowMaxUs, oldestSoftSafeAgeUs);
                                wgcDelayOldestSoftSafeAgeMaxUs =
                                    std::max(wgcDelayOldestSoftSafeAgeMaxUs, oldestSoftSafeAgeUs);
                            } else {
                                ++wgcDelayRepeatWithoutSoftSafeCandidateWindow;
                                ++wgcDelayRepeatWithoutSoftSafeCandidateTotal;
                                ++wgcDelaySyncProtectedRepeatWindow;
                                ++wgcDelaySyncProtectedRepeatTotal;
                                if (hardSafeCandidateAvailable) {
                                    ++wgcDelayRepeatHardOnlyCandidateWindow;
                                    ++wgcDelayRepeatHardOnlyCandidateTotal;
                                }
                            }

                            const uint64_t nowTick = GetTickCount64();
                            if (!wgcActiveDelayRepeatClassKnown || repeatWindowClass != wgcActiveDelayLastRepeatClass) {
                                if (!wgcActiveDelayRepeatClassKnown ||
                                    nowTick - wgcActiveDelayLastRepeatClassLogTick >= 500) {
                                    LogInfo(
                                        "[WGC CFR] Uniform active-delay repeat state=%s hardSafe=%d softSafe=%d "
                                        "srcStarved=%d lowSource=%d deepUnderfeed=%d recoveryActive=%d buffered=%zu "
                                        "span=%uus residualP95=%uus rawP95=%uus softTarget=%uus",
                                        ce::capture_policy::WgcActiveDelayWindowClassToString(repeatWindowClass),
                                        hardSafeCandidateAvailable ? 1 : 0, softSafeCandidateAvailable ? 1 : 0,
                                        wgcSourceStarvedCurrent ? 1 : 0, wgcLowSourceModeActive ? 1 : 0,
                                        uniformDeepUnderfeed ? 1 : 0,
                                        wgcActiveDelaySourceRecoveryUntilTick > nowTick ? 1 : 0,
                                        bufferedWgcFrames.size(), uniformRepeatReserveSpanUs(),
                                        uniformCombinedDelayResidualP95Us(), uniformRawDelayResidualP95Us(),
                                        uniformActiveDelaySoftLateTargetUs);
                                    wgcActiveDelayLastRepeatClassLogTick = nowTick;
                                }
                                wgcActiveDelayRepeatClassKnown = true;
                                wgcActiveDelayLastRepeatClass = repeatWindowClass;
                            }

                            const bool syncDelayHoldSourceLimited =
                                uniformSyncDelayHoldSourceLimited(softSafeCandidateAvailable);
                            if (syncDelayHoldSourceLimited) {
                                ++wgcSyncDelaySourceLimitedHoldCount;
                                ++wgcSyncDelaySourceLimitedHoldTotal;
                                ++wgcSourceRepeatLowerBoundWindow;
                                ++wgcSourceRepeatLowerBoundTotal;
                                ++wgcDelaySourceLimitedRepeatWindow;
                                ++wgcDelaySourceLimitedRepeatTotal;
                                const bool activeDelaySourceRecovery = wgcActiveDelaySourceRecoveryUntilTick > nowTick;
                                if (activeDelaySourceRecovery && !wgcSourceStarvedCurrent && !wgcLowSourceModeActive &&
                                    !uniformDeepUnderfeed) {
                                    ++wgcSyncDelaySourceRecoveryHoldCount;
                                    ++wgcSyncDelaySourceRecoveryHoldTotal;
                                }
                            } else {
                                ++wgcSyncDelayPolicyHoldCount;
                                ++wgcSyncDelayPolicyHoldTotal;
                                ++wgcExcessRepeatWindow;
                                ++wgcExcessRepeatTotal;
                                ++wgcPolicyAddedRepeatWindow;
                                ++wgcPolicyAddedRepeatTotal;
                                if (repeatClusterTicks > 0) {
                                    ++wgcExcessRepeatClusterWindow;
                                    ++wgcExcessRepeatClusterTotal;
                                    wgcExcessRepeatClusterWindowMaxTicks =
                                        std::max(wgcExcessRepeatClusterWindowMaxTicks, repeatClusterTicks);
                                    wgcExcessRepeatClusterMaxTicks =
                                        std::max(wgcExcessRepeatClusterMaxTicks, repeatClusterTicks);
                                }
                            }

                            const uint32_t reserveDepth = SaturatingToUint32(bufferedWgcFrames.size());
                            wgcDelayRepeatReserveDepthWindowMax =
                                std::max(wgcDelayRepeatReserveDepthWindowMax, reserveDepth);
                            wgcDelayRepeatReserveDepthMax = std::max(wgcDelayRepeatReserveDepthMax, reserveDepth);
                            const uint32_t reserveSpanUs = uniformRepeatReserveSpanUs();
                            wgcDelayRepeatReserveSpanWindowMaxUs =
                                std::max(wgcDelayRepeatReserveSpanWindowMaxUs, reserveSpanUs);
                            wgcDelayRepeatReserveSpanMaxUs = std::max(wgcDelayRepeatReserveSpanMaxUs, reserveSpanUs);
                        };
                        const auto recordUniformWgcDelayRealizationForFrame = [&](const QueuedFrame& selectedFrame) {
                            const int64_t gridReferenceQpc =
                                scheduledSampleQpc > 0 ? scheduledSampleQpc : selectionNowQpc.QuadPart;
                            if (qpcFreq.QuadPart <= 0 || selectedFrame.timestamp <= 0 || gridReferenceQpc <= 0) {
                                return false;
                            }
                            const int64_t requestedDelayUs = qpcToUs(getWgcEffectiveContentDelayQpc());
                            const int64_t predictedRealizedDelayUs =
                                ((gridReferenceQpc - GetFrameSelectionTimestamp(selectedFrame)) * 1000000) /
                                qpcFreq.QuadPart;
                            const int64_t predictedResidualUs = requestedDelayUs - predictedRealizedDelayUs;
                            const int64_t rawSelectionQpc = getWgcRawSelectionTimestamp(selectedFrame);
                            int64_t rawResidualUs = predictedResidualUs;
                            if (rawSelectionQpc > 0) {
                                const int64_t rawRealizedDelayUs =
                                    ((gridReferenceQpc - rawSelectionQpc) * 1000000) / qpcFreq.QuadPart;
                                rawResidualUs = requestedDelayUs - rawRealizedDelayUs;
                            }
                            return recordWgcDelayRealization(predictedResidualUs, rawResidualUs);
                        };
                        const auto recordUniformRepeatRescueRejection =
                            [&](const ce::capture_policy::WgcActiveDelayRelaxedCandidateScore& rescueScore,
                                ce::capture_policy::WgcActiveDelayWindowClass rescueWindowClass) {
                                switch (rescueScore.decision) {
                                    case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectSyncRisk:
                                        ++wgcDelayRepeatRescueRejectedSyncWindow;
                                        ++wgcDelayRepeatRescueRejectedSyncTotal;
                                        break;
                                    case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom:
                                        ++wgcDelayRepeatRescueRejectedHeadroomWindow;
                                        ++wgcDelayRepeatRescueRejectedHeadroomTotal;
                                        if (!ce::capture_policy::IsWgcActiveDelaySourceLimitedClass(
                                                rescueWindowClass) &&
                                            rescueScore.candidateLateResidualUs > uniformActiveDelaySoftLateTargetUs) {
                                            ++wgcDelayRepeatPromotionRejectedSoftWindow;
                                            ++wgcDelayRepeatPromotionRejectedSoftTotal;
                                        }
                                        break;
                                    case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectRepeatCost:
                                        ++wgcDelayRepeatRescueRejectedCostWindow;
                                        ++wgcDelayRepeatRescueRejectedCostTotal;
                                        break;
                                    default:
                                        break;
                                }
                            };
                        if (!bufferedWgcFrames.empty()) {
                            uint32_t playoutStaleDrops = 0;
                            while (bufferedWgcFrames.size() > 1 && playoutTargetQpc > 0 &&
                                   ce::capture_policy::ShouldDropCfrFrontForNearerPlayout(
                                       GetFrameSelectionTimestamp(bufferedWgcFrames[0]),
                                       GetFrameSelectionTimestamp(bufferedWgcFrames[1]), playoutTargetQpc,
                                       playoutLeadToleranceQpc)) {
                                QueuedFrame staleFront = std::move(bufferedWgcFrames.front());
                                bufferedWgcFrames.pop_front();
                                ReleaseQueuedFrameTexture(staleFront);
                                ++wgcDropObsoleteCount;
                                ++playoutStaleDrops;
                            }
                            if (playoutStaleDrops > 0) {
                                // Age-based catch-up after a WGC delivery gap/burst: the audio-passed
                                // backlog is dropped (not replayed), so the realized delay stays pinned
                                // instead of rubber-banding. Expected/healthy under a bursty source;
                                // throttle the log so a busy window stays readable.
                                wgcDelayPaceCapTrimTotal += playoutStaleDrops;
                                wgcDelayPaceCapTrimWindow += playoutStaleDrops;
                                const DWORD capTrimNowTick = GetTickCount();
                                if (playoutStaleDrops >= 3 && capTrimNowTick - wgcDelayPaceCapTrimLastLogTick >= 1000) {
                                    LogWarn(
                                        "[WGC CFR] active-delay playout catch-up: dropped %u audio-passed frame(s) "
                                        "after a WGC delivery gap depthAfter=%zu target=%lldus (bursty source "
                                        "delivery, NOT a game render hitch; realized content delay pinned, A/V sync "
                                        "preserved)",
                                        playoutStaleDrops, bufferedWgcFrames.size(),
                                        static_cast<long long>(qpcToUs(getWgcEffectiveContentDelayQpc())));
                                    wgcDelayPaceCapTrimLastLogTick = capTrimNowTick;
                                }
                            }
                            const auto playout =
                                playoutTargetQpc > 0
                                    ? ce::capture_policy::DecideCfrNearestPlayout(
                                          GetFrameSelectionTimestamp(bufferedWgcFrames.front()), playoutTargetQpc,
                                          playoutLeadToleranceQpc, lastEmittedWgcSelectionQpc)
                                    : ce::capture_policy::WgcNearestPlayoutDecision{/*emit=*/true, /*hold=*/false};
                            bool uniformRepeatRescueAccepted = false;
                            ce::capture_policy::WgcActiveDelayRelaxedCandidateScore uniformRepeatRescueScore{};
                            ce::capture_policy::WgcActiveDelayWindowClass uniformRepeatRescueClass =
                                ce::capture_policy::WgcActiveDelayWindowClass::kSourceLimited;
                            if (playout.hold && playoutTargetQpc > 0 && !bufferedWgcFrames.empty() && g_HasLastFrame &&
                                !g_LastFrame.isInjectMode) {
                                ++wgcDelayRepeatRescueAttemptWindow;
                                ++wgcDelayRepeatRescueAttemptTotal;
                                ++wgcDelayRepeatPromotionAttemptWindow;
                                ++wgcDelayRepeatPromotionAttemptTotal;
                                const QueuedFrame& rescueCandidate = bufferedWgcFrames.front();
                                const int64_t repeatSelectionTimestamp = GetFrameSelectionTimestamp(g_LastFrame);
                                uniformRepeatRescueClass = uniformActiveDelayWindowClassFor(true);
                                uniformRepeatRescueScore = ce::capture_policy::ScoreWgcActiveDelayRepeatRescueCandidate(
                                    GetFrameSelectionTimestamp(rescueCandidate),
                                    getWgcRawSelectionTimestamp(rescueCandidate), repeatSelectionTimestamp,
                                    playoutTargetQpc, targetIntervalTicks, qpcFreq.QuadPart,
                                    uniformRepeatClusterTicks(), uniformCombinedDelayResidualAvgAbsUs(),
                                    uniformCombinedDelayResidualP95Us(), uniformCombinedDelayResidualLateMaxUs(),
                                    uniformRepeatRescueClass, uniformActiveDelaySoftLateTargetUs);
                                uniformRepeatRescueAccepted = uniformRepeatRescueScore.Accepted();
                                if (uniformRepeatRescueAccepted) {
                                    ++wgcDelayRepeatRescueSuccessWindow;
                                    ++wgcDelayRepeatRescueSuccessTotal;
                                    ++wgcDelayRepeatPromotedBeforeRepeatWindow;
                                    ++wgcDelayRepeatPromotedBeforeRepeatTotal;
                                    ++wgcDelayRepeatSafeAfterPromotionWindow;
                                    ++wgcDelayRepeatSafeAfterPromotionTotal;
                                    ++wgcDelayOlderFrameAvoidedRepeatWindow;
                                    ++wgcDelayOlderFrameAvoidedRepeatTotal;
                                    if (uniformRepeatRescueScore.candidateLateResidualUs >
                                        uniformActiveDelaySoftLateTargetUs) {
                                        ++wgcDelayNearCapAcceptedWindow;
                                        ++wgcDelayNearCapAcceptedTotal;
                                    }
                                    if (!ce::capture_policy::IsWgcActiveDelaySourceLimitedClass(
                                            uniformRepeatRescueClass) &&
                                        uniformRepeatRescueScore.candidateLateResidualUs >
                                            uniformActiveDelaySoftLateTargetUs) {
                                        ++wgcDelaySoftLateAcceptedWindow;
                                        ++wgcDelaySoftLateAcceptedTotal;
                                    }
                                } else {
                                    recordUniformRepeatRescueRejection(uniformRepeatRescueScore,
                                                                       uniformRepeatRescueClass);
                                }
                            }
                            if ((playout.emit || uniformRepeatRescueAccepted) && !bufferedWgcFrames.empty()) {
                                frame = std::move(bufferedWgcFrames.front());
                                bufferedWgcFrames.pop_front();
                                popped = true;
                                if (frame.duplicateSourceTimestamp) {
                                    ++wgcSelectDuplicateSourceCount;
                                } else {
                                    ++wgcSelectFreshCount;
                                }
                                ++wgcDelayUniformCadenceWindow;
                                ++wgcDelayUniformCadenceTotal;
                                // Measure the realized content delay against the GRID playout reference
                                // (scheduledSampleQpc == the slot's ideal wall time), NOT wall-clock
                                // `selectionNowQpc`. The emitted frame lands at a fixed PTS slot and the
                                // co-timed audio is anchored to the same grid, so the file's true A/V
                                // content offset is `gridSlotTime - frame.timestamp`, independent of how
                                // late the encoder thread happened to wake. Using `selectionNowQpc` here
                                // folded encoder-thread scheduling jitter (30-88 ms late wakes under
                                // 100% GPU / network-drive mux I/O in 20260626_050554) into the metric,
                                // inflating realizedDelay to ~108 ms and tripping
                                // wgc_active_delay_realized_delay_unstable even though the content placed
                                // in each slot was grid-correct. Thread-wake jitter is reported
                                // separately as SchedSel/SelMax; this counter must stay content-honest.
                                wgcDelayRealizationRecordedThisTick = recordUniformWgcDelayRealizationForFrame(frame);
                                if (uniformRepeatRescueAccepted) {
                                    static uint32_t s_uniformRepeatRescueLogCount = 0;
                                    if (s_uniformRepeatRescueLogCount < 5) {
                                        ++s_uniformRepeatRescueLogCount;
                                        const int64_t candidateLeadUs =
                                            qpcFreq.QuadPart > 0
                                                ? ((GetFrameSelectionTimestamp(frame) - playoutTargetQpc) * 1000000) /
                                                      qpcFreq.QuadPart
                                                : 0;
                                        const int64_t candidateDamageUs =
                                            qpcFreq.QuadPart > 0
                                                ? (uniformRepeatRescueScore.candidateDamageQpc * 1000000) /
                                                      qpcFreq.QuadPart
                                                : 0;
                                        const int64_t repeatDamageUs =
                                            qpcFreq.QuadPart > 0
                                                ? (uniformRepeatRescueScore.repeatDamageQpc * 1000000) /
                                                      qpcFreq.QuadPart
                                                : 0;
                                        LogInfo(
                                            "[WGC CFR] Uniform playout rescued repeat with sync-safe frame: "
                                            "decision=%s lead=%lldus lateResidual=%uus candidateDamage=%lldus "
                                            "repeatDamage=%lldus buffered=%zu class=%s softTarget=%uus",
                                            ce::capture_policy::WgcActiveDelayRelaxedDecisionToString(
                                                uniformRepeatRescueScore.decision),
                                            static_cast<long long>(candidateLeadUs),
                                            uniformRepeatRescueScore.candidateLateResidualUs,
                                            static_cast<long long>(candidateDamageUs),
                                            static_cast<long long>(repeatDamageUs), bufferedWgcFrames.size(),
                                            ce::capture_policy::WgcActiveDelayWindowClassToString(
                                                uniformRepeatRescueClass),
                                            uniformActiveDelaySoftLateTargetUs);
                                    }
                                }
                            } else if (playout.hold) {
                                const bool uniformHardSafeCandidate =
                                    uniformHasHardSafeCandidateForTarget(playoutTargetQpc);
                                const bool uniformSoftSafeCandidate =
                                    uniformHasSoftSafeCandidateForTarget(playoutTargetQpc);
                                recordUniformRepeatDiagnostics(uniformHardSafeCandidate, uniformSoftSafeCandidate);
                                const bool uniformHoldSourceLimited =
                                    uniformSyncDelayHoldSourceLimited(uniformSoftSafeCandidate);
                                const bool uniformHoldSourceAtOrAboveCfr =
                                    g_WgcCap && ce::capture_policy::IsWgcIngressSourceAtOrAboveCfrTarget(
                                                    outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps);
                                if (!uniformHoldSourceLimited) {
                                    static uint32_t s_uniformPolicyHoldLogCount = 0;
                                    if (s_uniformPolicyHoldLogCount < 5) {
                                        ++s_uniformPolicyHoldLogCount;
                                        LogWarn(
                                            "[WGC CFR] Uniform playout held while source was at/above CFR target: "
                                            "inputMin=%u/%u outputFps=%u buffered=%zu hardSafe=%d softSafe=%d "
                                            "retained=%u/%u dropIngress=%u (policy repeat; not source-limited)",
                                            g_WgcCap ? g_WgcCap->GetInputMin250Fps() : 0u,
                                            g_WgcCap ? g_WgcCap->GetInputMin500Fps() : 0u, outputFps,
                                            bufferedWgcFrames.size(), uniformHardSafeCandidate ? 1 : 0,
                                            uniformSoftSafeCandidate ? 1 : 0,
                                            g_WgcCap ? g_WgcCap->GetIngressRetainedFrameCount() : 0u,
                                            g_WgcCap ? g_WgcCap->GetIngressRetainedFrameCap() : 0u,
                                            g_WgcCap ? g_WgcCap->GetIngressDecimatedCount() : 0u);
                                    }
                                } else if (uniformHoldSourceAtOrAboveCfr && !uniformSoftSafeCandidate) {
                                    static uint32_t s_uniformSyncProtectedHighSourceLogCount = 0;
                                    if (s_uniformSyncProtectedHighSourceLogCount < 5) {
                                        ++s_uniformSyncProtectedHighSourceLogCount;
                                        int64_t leadUs = 0;
                                        if (!bufferedWgcFrames.empty() && qpcFreq.QuadPart > 0 &&
                                            GetFrameSelectionTimestamp(bufferedWgcFrames.front()) > playoutTargetQpc) {
                                            leadUs = ((GetFrameSelectionTimestamp(bufferedWgcFrames.front()) -
                                                       playoutTargetQpc) *
                                                      1000000) /
                                                     qpcFreq.QuadPart;
                                        }
                                        LogInfo(
                                            "[WGC CFR] Uniform playout sync-protected repeat while source was "
                                            "at/above CFR target: lead=%lldus inputMin=%u/%u outputFps=%u "
                                            "buffered=%zu hardSafe=%d softSafe=%d span=%uus retained=%u/%u "
                                            "dropIngress=%u (no sync-safe frame for this slot; CFR/audio held)",
                                            static_cast<long long>(leadUs),
                                            g_WgcCap ? g_WgcCap->GetInputMin250Fps() : 0u,
                                            g_WgcCap ? g_WgcCap->GetInputMin500Fps() : 0u, outputFps,
                                            bufferedWgcFrames.size(), uniformHardSafeCandidate ? 1 : 0,
                                            uniformSoftSafeCandidate ? 1 : 0, uniformRepeatReserveSpanUs(),
                                            g_WgcCap ? g_WgcCap->GetIngressRetainedFrameCount() : 0u,
                                            g_WgcCap ? g_WgcCap->GetIngressRetainedFrameCap() : 0u,
                                            g_WgcCap ? g_WgcCap->GetIngressDecimatedCount() : 0u);
                                    }
                                }
                            }
                            // playout.hold -> leave the buffer intact; the encoder repeats the last
                            // emitted frame (an even source-limited / delivery-gap repeat).
                        }
                    } else if (tryPopBufferedWgcFrameForTarget(effectiveSelectionTargetQpc, liveSelectionTargetQpc,
                                                               selectionNowQpc.QuadPart,
                                                               wgcSelectionDelayAppliedThisTick, &frame)) {
                        popped = true;
                    }
                }
                updateWgcIngressPressure(popped ? "post-select" : "post-hold");
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
                        observeCaptureSyncPhaseSource("inject", injectCfrPhaseLock, temp.timestamp);
                    }
                    drainedInjectFrames.push_back(std::move(temp));
                }

                for (auto& drainedFrame : drainedInjectFrames) {
                    bufferedInjectFrames.push_back(std::move(drainedFrame));
                }

                // Track frame arrival rate for source-health telemetry. Use a short
                // window during warmup/startup so the EMA is already calibrated
                // when recording goes live, then widen to half-second for
                // steady-state stability. Timestamp-target playout, not this EMA,
                // decides which source frame represents each CFR output slot.
                pacingInputThisWindow += (uint32_t)drainedInjectFrames.size();
                pacingTicksThisWindow++;
                const uint32_t pacingWindowSize = (pacingEmaUpdates < 6) ? std::max((uint32_t)config.video.fps / 8, 8u)
                                                                         : (uint32_t)config.video.fps / 2;
                if (pacingTicksThisWindow >= pacingWindowSize) {
                    double measuredRate = (double)pacingInputThisWindow / (double)pacingTicksThisWindow;
                    // Adaptive alpha: converge fast during startup (0.7), steady-state (0.5),
                    // or when FPS transitions are detected (>20% deviation -> 0.8) so
                    // diagnostics follow rapid source-rate changes promptly.
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
                // Only the physical GPU/fence safety tail is protected from selection. The A/V
                // content delay is a timestamp target below; treating it as additional protected
                // frames hides every useful candidate at normal queue depth and creates trim/repeat
                // churn even when the game supplies one fresh frame per CFR tick.
                const size_t protectedInjectTailFrames =
                    ce::capture_policy::GetMinBufferedInjectFrames(injectReserveFrames, recordingOutputLive);
                const size_t maxBufferedInjectFrames =
                    std::max(ce::capture_policy::GetMaxBufferedInjectFrames(injectReserveFrames, recordingOutputLive,
                                                                            recordingLiveTick, GetTickCount64()),
                             injectReserveFrames + injectContentDelayFrames + 2);
                maxBufferedInjectDepthSinceLog = std::max(maxBufferedInjectDepthSinceLog, bufferedInjectFrames.size());
                uint32_t trimmedInjectFrames = 0;
                while (bufferedInjectFrames.size() > maxBufferedInjectFrames) {
                    QueuedFrame staleFrame = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    DiscardQueuedFrame(staleFrame);
                    ++trimmedInjectFrames;
                    ++injectBufferCapTrimTotal;
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
                        "[EncoderThread] Trimmed %u inject frame(s) at the hard buffer cap "
                        "(peak=%zu cap=%zu fenceTail=%zu delayFrames=%zu total=%llu)",
                        pendingInjectTrimmedLogCount, maxBufferedInjectDepthSinceLog, maxBufferedInjectFrames,
                        protectedInjectTailFrames, injectContentDelayFrames,
                        static_cast<unsigned long long>(injectBufferCapTrimTotal));
                    pendingInjectTrimmedLogCount = 0;
                    maxBufferedInjectDepthSinceLog = bufferedInjectFrames.size();
                    lastInjectTrimLog = now;
                }

                auto recordInjectTargetDrop = [&](QueuedFrame& stale) {
                    DiscardQueuedFrame(stale);
                    g_InjectCadenceDroppedFrames.fetch_add(1, std::memory_order_relaxed);
                    if (g_pSharedMem) {
                        g_pSharedMem->runtimeState.injectCadenceDrops.fetch_add(1, std::memory_order_relaxed);
                    }
                    ++injectTargetSupersededThisWindow;
                    ++injectTargetSupersededTotal;
                };
                auto eligibleInjectFrameCount = [&]() -> size_t {
                    return bufferedInjectFrames.size() > protectedInjectTailFrames
                               ? bufferedInjectFrames.size() - protectedInjectTailFrames
                               : 0;
                };
                auto isFreshInjectCandidate = [&](const QueuedFrame& candidate) {
                    return ce::capture_policy::IsInjectFrameFreshAfterLastEmission(candidate.timestamp,
                                                                                   lastEmittedInjectSourceQpc);
                };

                // Remove only frames that can never be emitted again. Unlike the old wall-age trim,
                // this is relative to committed source lineage and cannot delete an intentional
                // delayed frame merely because the encoder thread is currently later than it.
                while (eligibleInjectFrameCount() > 0 && !isFreshInjectCandidate(bufferedInjectFrames.front())) {
                    QueuedFrame obsolete = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    recordInjectTargetDrop(obsolete);
                }

                if (!g_EncoderRunning && !bufferedInjectFrames.empty()) {
                    frame = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    popped = true;
                    lastDeferredLineage = {};
                } else if (!recordingOutputLive || encoderGridStartQpc <= 0 || targetIntervalTicks <= 0) {
                    // Warmup/startup: the readiness gate below builds the content-delay history. Pop
                    // the oldest eligible source so the eventual first live frame is causal.
                    if (eligibleInjectFrameCount() > 0) {
                        frame = std::move(bufferedInjectFrames.front());
                        bufferedInjectFrames.pop_front();
                        popped = true;
                        lastDeferredLineage = {};
                    }
                } else {
                    const size_t availableCount = eligibleInjectFrameCount();
                    const int64_t liveTargetQpc =
                        scheduledOutputQpc > 0
                            ? scheduledOutputQpc
                            : ComputeIdealOutputQpc(encoderGridStartQpc, selectionGridTick, targetIntervalTicks);
                    const int64_t basePlayoutTargetQpc =
                        ComputeDelayedContentGridStartQpc(liveTargetQpc, avContentDelayQpc);
                    const int64_t phaseReferenceQpc =
                        bufferedInjectFrames.empty() ? 0 : bufferedInjectFrames.back().timestamp;
                    const int64_t playoutTargetQpc = applyCaptureSyncPhaseTarget(
                        "inject", injectCfrPhaseLock, basePlayoutTargetQpc, phaseReferenceQpc);
                    const int64_t leadToleranceQpc =
                        ce::capture_policy::GetInjectCfrSelectionLeadToleranceQpc(targetIntervalTicks);
                    auto isAllowedCandidate = [&](const QueuedFrame& candidate) {
                        return isFreshInjectCandidate(candidate) &&
                               !MatchesInjectFrameLineage(candidate, lastDeferredLineage);
                    };
                    size_t bestIdx = SelectFrameClosestToTimestampIf(bufferedInjectFrames, availableCount,
                                                                      playoutTargetQpc, isAllowedCandidate);
                    bool usedDeferredFallback = false;
                    if (bestIdx >= availableCount) {
                        bestIdx = SelectFrameClosestToTimestampIf(bufferedInjectFrames, availableCount,
                                                                  playoutTargetQpc, isFreshInjectCandidate);
                        usedDeferredFallback = lastDeferredLineage.IsValid() && bestIdx < availableCount;
                    }

                    if (bestIdx < availableCount) {
                        const int64_t selectedTimestamp = bufferedInjectFrames[bestIdx].timestamp;
                        const auto decision = ce::capture_policy::DecideCfrNearestPlayout(
                            selectedTimestamp, playoutTargetQpc, leadToleranceQpc, lastEmittedInjectSourceQpc);
                        if (decision.emit) {
                            if (bestIdx > 0) {
                                ++selectionLogCounter;
                                if (selectionLogCounter <= 12 || (selectionLogCounter % 240) == 0) {
                                    LogInfo(
                                        "[EncoderThread] Inject target select tick=%lld targetQpc=%lld chose idx=%zu "
                                        "frame=%u tex=%d ts=%lld oldest=%lld avail=%zu fenceTail=%zu "
                                        "delayFrames=%zu delayUs=%lld%s",
                                        static_cast<long long>(selectionGridTick),
                                        static_cast<long long>(playoutTargetQpc), bestIdx,
                                        bufferedInjectFrames[bestIdx].frameIndex,
                                        bufferedInjectFrames[bestIdx].textureIndex,
                                        static_cast<long long>(selectedTimestamp),
                                        static_cast<long long>(bufferedInjectFrames.front().timestamp), availableCount,
                                        protectedInjectTailFrames, injectContentDelayFrames,
                                        static_cast<long long>(qpcToUs(avContentDelayQpc)),
                                        usedDeferredFallback ? " fallback=deferred-only" : "");
                                }
                            }
                            for (size_t i = 0; i < bestIdx; ++i) {
                                QueuedFrame superseded = std::move(bufferedInjectFrames.front());
                                bufferedInjectFrames.pop_front();
                                recordInjectTargetDrop(superseded);
                            }
                            frame = std::move(bufferedInjectFrames.front());
                            bufferedInjectFrames.pop_front();
                            popped = true;
                            lastDeferredLineage = {};
                            ++injectTargetSelectThisWindow;
                            ++injectTargetSelectTotal;
                            if (qpcFreq.QuadPart > 0) {
                                const uint64_t residualUs =
                                    ce::capture_policy::GetCfrTimestampDistanceQpc(selectedTimestamp,
                                                                                  playoutTargetQpc) *
                                    1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
                                injectTargetResidualMaxUs =
                                    std::max(injectTargetResidualMaxUs, SaturatingToUint32(residualUs));
                            }
                        } else if (decision.hold) {
                            ++injectTargetHoldThisWindow;
                            ++injectTargetHoldTotal;
                            ++injectTargetHoldWithCandidateThisWindow;
                            ++injectTargetHoldWithCandidateTotal;
                        }
                    } else {
                        ++injectTargetHoldThisWindow;
                        ++injectTargetHoldTotal;
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

        const bool canPreserveLastFrameAcrossPathHandoff =
            !config.video.useVFR &&
            ((useScreenGrab && MediaEngine_RepeatLastFrameWithTimeline) || MediaEngine_RepeatLastFrame) &&
            MediaEngine_CanRepeatLastFrame && MediaEngine_CanRepeatLastFrame();
        if (g_HasLastFrame &&
            !ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, g_LastFrame.isInjectMode) &&
            !canPreserveLastFrameAcrossPathHandoff) {
            discardActivePathMismatchFrame(g_LastFrame, "cached last frame", false);
            g_HasLastFrame = false;
        }

        if (popped && frame.isInjectMode && g_RejectInjectFrames.load(std::memory_order_acquire)) {
            DiscardQueuedFrame(frame);
            popped = false;
        }

        if (g_HasLastFrame && g_LastFrame.isInjectMode && g_RejectInjectFrames.load(std::memory_order_acquire) &&
            !canPreserveLastFrameAcrossPathHandoff) {
            g_LastFrame = QueuedFrame{};
            g_HasLastFrame = false;
        }

        const bool hasRepeatLastFramePath =
            !config.video.useVFR &&
            ((useScreenGrab && MediaEngine_RepeatLastFrameWithTimeline) || MediaEngine_RepeatLastFrame);
        auto selectCursorStateForScheduledQpc = [&](int64_t scheduledQpc, const QueuedFrame& referenceFrame,
                                                    const char* outputKind) {
            ce::cursor::CaptureState cursorState = referenceFrame.cursorState;
            if (config.video.captureCursor && scheduledQpc > 0) {
                const uint32_t captureWidth = referenceFrame.cursorState.captureWidth != 0
                                                  ? referenceFrame.cursorState.captureWidth
                                                  : referenceFrame.width;
                const uint32_t captureHeight = referenceFrame.cursorState.captureHeight != 0
                                                   ? referenceFrame.cursorState.captureHeight
                                                   : referenceFrame.height;
                const bool cursorEmbedded = useScreenGrab && referenceFrame.wgcCursorEmbedded;
                const ce::cursor::CaptureState liveState =
                    CaptureCursorSnapshot(scheduledQpc, referenceFrame.captureLeft, referenceFrame.captureTop,
                                          captureWidth, captureHeight, cursorEmbedded);
                ce::cursor::Timeline& timeline = useScreenGrab ? g_WgcCursorTimeline : g_InjectCursorTimeline;
                timeline.Publish(liveState);
                const int64_t cursorTargetQpc =
                    useScreenGrab ? std::max<int64_t>(1, scheduledQpc - getWgcEffectiveContentDelayQpc())
                                  : scheduledQpc;
                if (!timeline.SelectAtOrBefore(cursorTargetQpc, &cursorState)) {
                    cursorState = liveState;
                }
                if (cursorEmbedded) {
                    // Pixel ownership is authoritative: an embedded cursor in
                    // the selected source texture must never be drawn again,
                    // even if the delayed timeline selected an older state.
                    cursorState.flags |= ce::cursor::kStateValid | ce::cursor::kStateSuppressed;
                    cursorState.flags &= ~ce::cursor::kStateVisible;
                }

                static uint64_t s_cursorTimelineLogCount = 0;
                ++s_cursorTimelineLogCount;
                if (s_cursorTimelineLogCount <= 5 || (s_cursorTimelineLogCount % 600ull) == 0ull) {
                    LogInfo(
                        "[Cursor] CFR timeline backend=%s output=%s scheduled=%lld target=%lld source=%lld "
                        "selected=%lld observed=%lld deltaUs=%lld dpi=%u size=%ux%u bounds=(%d,%d %ux%u) "
                        "visible=%d embedded=%d fallback=%d coord=%s",
                        useScreenGrab ? "screen-grab" : "inject", outputKind ? outputKind : "unknown",
                        static_cast<long long>(scheduledQpc), static_cast<long long>(cursorTargetQpc),
                        static_cast<long long>(referenceFrame.cursorState.associationQpc),
                        static_cast<long long>(cursorState.associationQpc),
                        static_cast<long long>(cursorState.observedQpc),
                        static_cast<long long>(qpcFreq.QuadPart > 0
                                                   ? ((cursorTargetQpc - cursorState.associationQpc) * 1000000) /
                                                         qpcFreq.QuadPart
                                                   : 0),
                        cursorState.dpi, cursorState.requestedWidth, cursorState.requestedHeight,
                        cursorState.captureLeft, cursorState.captureTop, cursorState.captureWidth,
                        cursorState.captureHeight, cursorState.IsVisible() ? 1 : 0, cursorEmbedded ? 1 : 0,
                        (cursorState.flags & ce::cursor::kStateHandleVisibilityFallback) != 0 ? 1 : 0,
                        cursorState.PositionIsShapeTopLeft() ? "shape-top-left" : "hotspot");
                }
            }
            return cursorState;
        };
        auto repeatLastFrameForScheduledQpc = [&](int64_t scheduledQpc) {
            ce::cursor::CaptureState cursorState;
            if (config.video.captureCursor && g_HasLastFrame) {
                cursorState = selectCursorStateForScheduledQpc(scheduledQpc, g_LastFrame, "repeat");
            }
            if (useScreenGrab && !config.video.useVFR && MediaEngine_RepeatLastFrameWithTimeline) {
                return MediaEngine_RepeatLastFrameWithTimeline(scheduledQpc, computeLiveTimelineElapsedUs(scheduledQpc),
                                                               &cursorState);
            }
            return MediaEngine_RepeatLastFrame && MediaEngine_RepeatLastFrame(scheduledQpc, &cursorState);
        };
        auto recoverScheduledFreshEncodeFailure = [&](bool scheduledCfrTick, bool freshEncodeSucceeded,
                                                      bool freshEncodeDeferred, int64_t scheduledQpc,
                                                      const QueuedFrame* failedFrame, const char* context) {
            const bool repeatCacheAvailable = MediaEngine_CanRepeatLastFrame && MediaEngine_CanRepeatLastFrame();
            if (!ce::capture_policy::ShouldRepeatAfterScheduledFreshEncodeFailure(
                    scheduledCfrTick, freshEncodeSucceeded, freshEncodeDeferred, hasRepeatLastFramePath,
                    repeatCacheAvailable)) {
                return false;
            }

            // The failed WGC attempt may have changed cursor suppression to
            // match pixels that were never emitted. Restore the metadata that
            // belongs to the cached successful source frame before repeating.
            if (failedFrame && !failedFrame->isInjectMode && hasSuccessfulWgcCursorMetadata) {
                SyncDuplicationCursorSuppression(lastSuccessfulWgcCursorEmbedded);
            }

            const bool repeatSucceeded = repeatLastFrameForScheduledQpc(scheduledQpc);
            const bool repeatDeferred = MediaEngine_WasLastFrameDeferred && MediaEngine_WasLastFrameDeferred();
            if (!repeatSucceeded || repeatDeferred) {
                LogWarn(
                    "[EncoderThread] CFR fresh encode failed and cached-repeat recovery also failed: context=%s "
                    "scheduledQpc=%lld repeatSucceeded=%d repeatDeferred=%d",
                    context ? context : "unknown", static_cast<long long>(scheduledQpc), repeatSucceeded ? 1 : 0,
                    repeatDeferred ? 1 : 0);
                return false;
            }

            static uint64_t s_freshEncodeRecoveryCount = 0;
            ++s_freshEncodeRecoveryCount;
            if (s_freshEncodeRecoveryCount <= 5 || (s_freshEncodeRecoveryCount % 120ull) == 0ull) {
                LogWarn(
                    "[EncoderThread] CFR fresh encode failure recovered with cached duplicate: context=%s "
                    "scheduledQpc=%lld recoveryCount=%llu",
                    context ? context : "unknown", static_cast<long long>(scheduledQpc),
                    static_cast<unsigned long long>(s_freshEncodeRecoveryCount));
            }
            return true;
        };
        auto releaseWgcLeaseAfterMediaEngineCopy = [&](QueuedFrame& encodedFrame, const char* context) {
            if (encodedFrame.isInjectMode || !encodedFrame.wgcPoolLease.IsValid()) {
                return;
            }
            if (!hasRepeatLastFramePath) {
                static bool s_loggedHeldForFallback = false;
                if (!s_loggedHeldForFallback) {
                    LogWarn(
                        "[WGC] Holding encoded pool slot lease because media-engine repeat cache is unavailable "
                        "(slot=%u generation=%llu context=%s). Pool pressure can rise on fallback duplicate paths.",
                        encodedFrame.wgcPoolSlot, static_cast<unsigned long long>(encodedFrame.wgcPoolGeneration),
                        context);
                    s_loggedHeldForFallback = true;
                }
                return;
            }

            const uint32_t slot = encodedFrame.wgcPoolSlot;
            const uint64_t generation = encodedFrame.wgcPoolGeneration;
            encodedFrame.wgcPoolLease.Reset();
            encodedFrame.wgcPoolSlot = std::numeric_limits<uint32_t>::max();
            encodedFrame.wgcPoolGeneration = 0;

            static uint64_t s_releaseLogCount = 0;
            ++s_releaseLogCount;
            if (s_releaseLogCount <= 5 || (s_releaseLogCount % 1000ull) == 0ull) {
                LogInfo(
                    "[WGC] Pool slot lease released after media-engine copy: slot=%u generation=%llu context=%s "
                    "releaseCount=%llu",
                    slot, static_cast<unsigned long long>(generation), context,
                    static_cast<unsigned long long>(s_releaseLogCount));
            }
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

                        const uint32_t smoothnessStartupDesiredFrames = getWgcSmoothnessDesiredFramesForConfig();
                        const uint32_t smoothnessStartupRetainedFrames = getWgcSmoothnessRetainedFramesBudget();
                        const bool smoothnessStartupAttempt = shouldAttemptWgcStartupSmoothnessBufferNow();
                        const int64_t delayTicks =
                            ce::capture_policy::GetWgcCfrStartupPreLiveDelayTicks(targetIntervalTicks);
                        updateWgcIngressPressure("startup-pre-live-delay");
                        if (hTimer && delayTicks > 0 && qpcFreq.QuadPart > 0) {
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
                        updateWgcIngressPressure("startup-post-delay-flush");

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
                            "bufferedFlushed=%zu smoothAttempt=%d smoothDesiredFrames=%u "
                            "smoothnessRetainedFrames=%u smoothPreLiveDelayTicks=0 smoothReason=%s warmupMs=%llu",
                            static_cast<long long>(wgcStartupBarrierQpc), static_cast<long long>(barrierNow.QuadPart),
                            static_cast<long long>(targetIntervalTicks), static_cast<long long>(delayTicks),
                            hiddenStartupFrames, wgcStartupPreLiveDelayDroppedFrames, queueFlushed, bufferedFlushed,
                            smoothnessStartupAttempt ? 1 : 0, smoothnessStartupDesiredFrames,
                            smoothnessStartupRetainedFrames, getWgcSmoothnessBufferReason(),
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

                    // WGC/DXGI dimensions and texture format are known only after a frame arrives.
                    // Finish the deferred codec/device/mux initialization now, while frame zero is
                    // still transactional. The producer continues filling the reservoir during this
                    // call, and no CFR slot or A/V anchor is committed by the prepare API.
                    if (!wgcEncoderPrewarmAttempted) {
                        wgcEncoderPrewarmAttempted = true;
                        LARGE_INTEGER prewarmStartQpc = {};
                        LARGE_INTEGER prewarmEndQpc = {};
                        QueryPerformanceCounter(&prewarmStartQpc);
                        wgcEncoderPrewarmSucceeded =
                            MediaEngine_PrepareFrameD3D11 &&
                            MediaEngine_PrepareFrameD3D11(frame.texture, frame.width, frame.height, frame.isHDR);
                        QueryPerformanceCounter(&prewarmEndQpc);
                        wgcEncoderPrewarmElapsedUs = qpcToUs(prewarmEndQpc.QuadPart - prewarmStartQpc.QuadPart);
                        LogInfo(
                            "[EncoderThread] WGC transactional video prewarm %s: elapsed=%lldus frameQpc=%lld "
                            "dimensions=%ux%u hdr=%d queuedAfter=%zu bufferedAfter=%zu; frame zero remains pending",
                            wgcEncoderPrewarmSucceeded ? "complete" : "FAILED",
                            static_cast<long long>(wgcEncoderPrewarmElapsedUs), static_cast<long long>(frame.timestamp),
                            frame.width, frame.height, frame.isHDR ? 1 : 0, g_FrameQueue.Size(),
                            bufferedWgcFrames.size());
                    }

                    size_t startupBufferedExamined = 0;
                    size_t startupQueueExamined = 0;
                    size_t startupFreshened = 0;
                    size_t startupDiscardedOlder = 0;
                    size_t startupDiscardedBeforeBarrier = 0;
                    size_t startupDiscardedPathMismatch = 0;
                    struct StartupWgcCandidate {
                        QueuedFrame frame;
                        size_t sequence = 0;
                    };
                    std::vector<StartupWgcCandidate> startupCandidates;
                    startupCandidates.reserve(1 + bufferedWgcFrames.size() + 8);
                    size_t startupCandidateSequence = 0;
                    const int64_t initialStartupSelectionQpc = GetFrameSelectionTimestamp(frame);
                    auto considerStartupWgcCandidate = [&](QueuedFrame candidate, bool fromQueue,
                                                           bool initialCandidate = false) {
                        if (fromQueue) {
                            ++startupQueueExamined;
                        } else if (!initialCandidate) {
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

                        StartupWgcCandidate startupCandidate;
                        startupCandidate.frame = std::move(candidate);
                        startupCandidate.sequence = startupCandidateSequence++;
                        startupCandidates.push_back(std::move(startupCandidate));
                    };

                    considerStartupWgcCandidate(std::move(frame), false, true);

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

                    std::stable_sort(startupCandidates.begin(), startupCandidates.end(),
                                     [&](const StartupWgcCandidate& lhs, const StartupWgcCandidate& rhs) {
                                         const int64_t lhsSelectionQpc = GetFrameSelectionTimestamp(lhs.frame);
                                         const int64_t rhsSelectionQpc = GetFrameSelectionTimestamp(rhs.frame);
                                         if (lhsSelectionQpc != rhsSelectionQpc) {
                                             return lhsSelectionQpc < rhsSelectionQpc;
                                         }
                                         if (lhs.frame.timestamp != rhs.frame.timestamp) {
                                             return lhs.frame.timestamp < rhs.frame.timestamp;
                                         }
                                         return lhs.sequence < rhs.sequence;
                                     });

                    std::vector<int64_t> startupSelectionQpcs;
                    startupSelectionQpcs.reserve(startupCandidates.size());
                    for (const auto& candidate : startupCandidates) {
                        startupSelectionQpcs.push_back(GetFrameSelectionTimestamp(candidate.frame));
                        if (GetFrameSelectionTimestamp(candidate.frame) > initialStartupSelectionQpc) {
                            ++startupFreshened;
                        }
                    }

                    const uint32_t smoothnessDesiredFrames = getWgcSmoothnessDesiredFramesForConfig();
                    const uint32_t smoothnessRetainedFrames =
                        (g_WgcCap && smoothnessDesiredFrames > 0) ? g_WgcCap->GetSmoothnessRetainedFrameCount() : 0u;
                    const bool smoothnessStartupAttempted = ce::capture_policy::ShouldAttemptWgcStartupSmoothnessBuffer(
                        config.wgcSmoothnessBufferEnabled, config.video.useVFR, wgcSmoothnessDelayDesired,
                        targetIntervalTicks, smoothnessRetainedFrames);
                    const uint32_t smoothnessPoolSlots =
                        g_WgcCap ? g_WgcCap->GetTexturePoolSlotCount()
                                 : ce::capture_policy::GetWgcSmoothnessPoolFrameCount(smoothnessRetainedFrames);
                    const uint32_t smoothnessRetainedFrameCap =
                        g_WgcCap ? g_WgcCap->GetSmoothnessRetainedFrameCap() : smoothnessPoolSlots;
                    const uint32_t smoothnessReservedFreeSlots =
                        g_WgcCap ? g_WgcCap->GetSmoothnessReservedFreeSlotCount()
                                 : ce::capture_policy::kWgcSmoothnessBufferPoolSafetyFrames;
                    const uint64_t smoothnessEstimatedVramBytes =
                        g_WgcCap ? g_WgcCap->GetSmoothnessEstimatedVramBytes() : 0ull;
                    const bool smoothnessCapLimited =
                        smoothnessDesiredFrames > 0 && smoothnessRetainedFrames < smoothnessDesiredFrames;
                    // Full buildable reservoir target (audio-latency path uses this unchanged).
                    const int64_t smoothnessReservoirTargetDelayQpc =
                        smoothnessStartupAttempted ? ce::capture_policy::GetWgcStartupSmoothnessTargetDelayQpc(
                                                         smoothnessRetainedFrames, targetIntervalTicks,
                                                         getWgcSmoothnessOutputFps(), config.wgcSmoothnessBufferMaxMs)
                                                   : 0;
                    // Resolve the smoothness FLOOR once, here at the startup barrier, from measured pre-live
                    // WGC delivery jitter (auto) or the explicit config value, clamped to the buildable
                    // reservoir. It is then HELD FIXED for the session. For the audio-latency path
                    // (avContentDelayActive) the floor is a deliberate no-op: the reservoir target already
                    // dominates, so the validated with-audio behavior is unchanged.
                    if (wgcSmoothnessFloorConfigured && smoothnessStartupAttempted &&
                        smoothnessReservoirTargetDelayQpc > 0) {
                        if (g_WgcCap) {
                            wgcSmoothnessFloorJitter.deliveryGapAvgUs =
                                SaturatingToUint32(g_WgcCap->GetCallbackGapAvgUs());
                            wgcSmoothnessFloorJitter.deliveryGapMaxUs =
                                SaturatingToUint32(g_WgcCap->GetCallbackGapMaxUs());
                            wgcSmoothnessFloorJitter.sourceJitterAvgUs =
                                SaturatingToUint32(g_WgcCap->GetSourceJitterAvgUs());
                            wgcSmoothnessFloorJitter.sourceJitterMaxUs =
                                SaturatingToUint32(g_WgcCap->GetSourceJitterMaxUs());
                        }
                        if (config.wgcSmoothnessFloorAuto) {
                            wgcSmoothnessFloorSource = "auto";
                            wgcSmoothnessFloorRequestedQpc = ce::capture_policy::DeriveWgcSmoothnessFloorDelayQpc(
                                wgcSmoothnessFloorJitter, targetIntervalTicks, qpcFreq.QuadPart,
                                config.wgcSmoothnessBufferMaxMs, smoothnessRetainedFrames);
                            wgcSmoothnessFloorDelayQpc = wgcSmoothnessFloorRequestedQpc;
                        } else {
                            wgcSmoothnessFloorSource = "config";
                            wgcSmoothnessFloorRequestedQpc =
                                qpcFreq.QuadPart > 0
                                    ? (qpcFreq.QuadPart * static_cast<int64_t>(config.wgcSmoothnessFloorMs)) / 1000
                                    : 0;
                            wgcSmoothnessFloorDelayQpc = ce::capture_policy::ClampWgcSmoothnessFloorDelayQpc(
                                wgcSmoothnessFloorRequestedQpc, targetIntervalTicks, qpcFreq.QuadPart,
                                config.wgcSmoothnessBufferMaxMs, smoothnessRetainedFrames);
                        }
                        const int64_t floorCapQpc = ce::capture_policy::GetWgcSmoothnessFloorCapQpc(
                            targetIntervalTicks, qpcFreq.QuadPart, config.wgcSmoothnessBufferMaxMs,
                            smoothnessRetainedFrames);
                        const int64_t floorMinQpc =
                            targetIntervalTicks *
                            static_cast<int64_t>(ce::capture_policy::kWgcSmoothnessFloorMinFrames);
                        if (wgcSmoothnessFloorRequestedQpc >= floorCapQpc && floorCapQpc > 0) {
                            const int64_t maxMsQpc =
                                config.wgcSmoothnessBufferMaxMs > 0 && qpcFreq.QuadPart > 0
                                    ? (qpcFreq.QuadPart * static_cast<int64_t>(config.wgcSmoothnessBufferMaxMs)) / 1000
                                    : INT64_MAX;
                            wgcSmoothnessFloorClampedBy = (maxMsQpc <= floorCapQpc) ? "max_ms" : "reservoir";
                        } else if (wgcSmoothnessFloorRequestedQpc < floorMinQpc) {
                            wgcSmoothnessFloorClampedBy = "min";
                        } else {
                            wgcSmoothnessFloorClampedBy = "none";
                        }
                    }
                    // L>0: keep the full reservoir target (unchanged). L==0 floor: target ONLY the floor depth
                    // (a smaller, jitter-sized buffer) rather than the full reservoir, trading less latency for
                    // adequate jitter absorption. The floor is <= the reservoir by construction (clamped above).
                    const int64_t smoothnessTargetDelayQpc =
                        avContentDelayActive ? smoothnessReservoirTargetDelayQpc
                                             : std::min(wgcSmoothnessFloorDelayQpc, smoothnessReservoirTargetDelayQpc);
                    const int64_t startupContentDelayTargetQpc =
                        avContentDelayQpc + std::max<int64_t>(0, smoothnessTargetDelayQpc);
                    wgcSmoothnessDesiredFrames = smoothnessDesiredFrames;
                    wgcSmoothnessRetainedFrames = smoothnessRetainedFrames;
                    wgcSmoothnessPoolSlots = smoothnessPoolSlots;
                    wgcSmoothnessRetainedFrameCap = smoothnessRetainedFrameCap;
                    wgcSmoothnessReservedFreeSlots = smoothnessReservedFreeSlots;
                    wgcSmoothnessEstimatedVramBytes = smoothnessEstimatedVramBytes;
                    wgcSmoothnessCapLimited = smoothnessCapLimited;
                    wgcSmoothnessBufferReason = getWgcSmoothnessBufferReason();
                    if (smoothnessDesiredFrames > 0 && smoothnessRetainedFrames == 0) {
                        wgcSmoothnessBufferReason = "vram_budget_exhausted";
                    } else if (smoothnessCapLimited) {
                        wgcSmoothnessBufferReason = "vram_cap_limited";
                    }

                    // One-time smoothness-FLOOR decision log. Makes the auto-derivation auditable: what
                    // delivery/source jitter was measured, what floor it produced, how it was clamped, and the
                    // resulting effective target. A no-op note is logged for the with-audio path so it is clear
                    // the validated behavior is unchanged there.
                    if (wgcSmoothnessFloorConfigured && !wgcSmoothnessFloorLogged) {
                        // Log once per recording. This block re-runs every startup-barrier re-evaluation
                        // (~once per delivered frame during the reserve wait); the derived floor is stable
                        // across those iterations, so a single line is sufficient and avoids log spam.
                        wgcSmoothnessFloorLogged = true;
                        const char* floorNote = avContentDelayActive
                                                    ? "no-op: audio-latency reservoir target dominates"
                                                    : (smoothnessReservoirTargetDelayQpc > 0
                                                           ? "active: video-only/low-confidence jitter buffer"
                                                           : "inactive: no reservoir capacity");
                        LogInfo(
                            "[AVSyncApply] wgc_smoothness_floor: source=%s auto=%d configuredMs=%u "
                            "deliveryGapUs(avg/max)=%u/%u sourceJitterUs(avg/max)=%u/%u requestedUs=%lld "
                            "derivedUs=%lld clampedBy=%s reservoirTargetUs=%lld effectiveTargetUs=%lld "
                            "avContentDelayUs=%lld note=\"%s\"",
                            wgcSmoothnessFloorSource, config.wgcSmoothnessFloorAuto ? 1 : 0,
                            config.wgcSmoothnessFloorMs, wgcSmoothnessFloorJitter.deliveryGapAvgUs,
                            wgcSmoothnessFloorJitter.deliveryGapMaxUs, wgcSmoothnessFloorJitter.sourceJitterAvgUs,
                            wgcSmoothnessFloorJitter.sourceJitterMaxUs,
                            static_cast<long long>(qpcToUs(wgcSmoothnessFloorRequestedQpc)),
                            static_cast<long long>(qpcToUs(wgcSmoothnessFloorDelayQpc)), wgcSmoothnessFloorClampedBy,
                            static_cast<long long>(qpcToUs(smoothnessReservoirTargetDelayQpc)),
                            static_cast<long long>(qpcToUs(smoothnessTargetDelayQpc)),
                            static_cast<long long>(qpcToUs(avContentDelayQpc)), floorNote);
                    }

                    const int64_t startupReserveToleranceQpc =
                        qpcFreq.QuadPart > 0
                            ? std::min<int64_t>(targetIntervalTicks > 0 ? (targetIntervalTicks / 2) : 0,
                                                qpcFreq.QuadPart / 200)
                            : 0;
                    const auto startupReserveSelection = ce::capture_policy::SelectWgcStartupReserveCandidate(
                        startupSelectionQpcs.empty() ? nullptr : startupSelectionQpcs.data(),
                        startupSelectionQpcs.size(),
                        startupContentDelayTargetQpc > 0 ? startupContentDelayTargetQpc : 0,
                        startupReserveToleranceQpc);
                    size_t selectedStartupIndex = startupReserveSelection.selectedIndex;
                    if (selectedStartupIndex >= startupCandidates.size()) {
                        selectedStartupIndex = startupCandidates.empty() ? 0 : (startupCandidates.size() - 1);
                    }

                    const auto qpcDeltaToUs = [&](int64_t qpcDelta) -> int64_t {
                        return qpcFreq.QuadPart > 0 ? (qpcDelta * 1000000) / qpcFreq.QuadPart : 0;
                    };
                    wgcStartupReserveFrames = SaturatingToUint32(startupCandidates.size());
                    wgcStartupReserveSpanUs = qpcDeltaToUs(startupReserveSelection.reserveSpanQpc);
                    wgcStartupDelayTargetUs =
                        qpcDeltaToUs(startupContentDelayTargetQpc > 0 ? startupContentDelayTargetQpc : 0);
                    wgcStartupSelectedByDelayReserve = startupReserveSelection.usedDelayReserve;
                    if (startupContentDelayTargetQpc <= 0) {
                        wgcStartupReserveReason = "inactive";
                    } else if (startupCandidates.size() < 2) {
                        wgcStartupReserveReason = "insufficient_frames";
                    } else if (startupReserveSelection.usedDelayReserve) {
                        wgcStartupReserveReason = "selected";
                    } else {
                        wgcStartupReserveReason = "insufficient_span";
                    }

                    const size_t newerStartupReserveFrames = selectedStartupIndex < startupCandidates.size()
                                                                 ? (startupCandidates.size() - selectedStartupIndex - 1)
                                                                 : 0;
                    const bool startupReserveBelowLowWater =
                        startupContentDelayTargetQpc > 0 && startupReserveSelection.usedDelayReserve &&
                        newerStartupReserveFrames <
                            getWgcDelayReservoirLowWaterFramesForDelay(startupContentDelayTargetQpc);
                    const bool startupReserveMissing =
                        startupContentDelayTargetQpc > 0 &&
                        (!startupReserveSelection.usedDelayReserve || startupReserveBelowLowWater);
                    if (startupReserveMissing && targetIntervalTicks > 0 && qpcFreq.QuadPart > 0) {
                        LARGE_INTEGER waitNow;
                        QueryPerformanceCounter(&waitNow);
                        if (wgcStartupReserveWaitStartQpc <= 0) {
                            wgcStartupReserveWaitStartQpc = waitNow.QuadPart;
                            wgcStartupReserveWaitInitialSpanUs = wgcStartupReserveSpanUs;
                        }
                        wgcStartupReserveWaitFreshenedMax =
                            std::max<uint32_t>(wgcStartupReserveWaitFreshenedMax, SaturatingToUint32(startupFreshened));
                        const int64_t waitBudgetQpc = ce::capture_policy::GetWgcStartupReserveWaitBudgetQpc(
                            startupContentDelayTargetQpc, targetIntervalTicks, smoothnessTargetDelayQpc,
                            smoothnessStartupAttempted, qpcFreq.QuadPart);
                        const bool waitBudgetRemaining =
                            waitNow.QuadPart - wgcStartupReserveWaitStartQpc < waitBudgetQpc;
                        if (waitBudgetRemaining) {
                            ++wgcStartupReserveWaitCount;
                            for (auto& candidate : startupCandidates) {
                                if (candidate.frame.texture || candidate.frame.sharedHandle ||
                                    candidate.frame.timestamp > 0) {
                                    bufferedWgcFrames.push_back(std::move(candidate.frame));
                                }
                            }
                            trimBufferedWgcStartupWaitToRetainedCap("startup-wait");
                            wgcStartupReserveReason =
                                startupReserveBelowLowWater ? "waiting_low_water" : "waiting_span";
                            if (wgcStartupReserveWaitCount <= 3 || (wgcStartupReserveWaitCount % 30u) == 0u) {
                                LogInfo(
                                    "[EncoderThread] WGC startup delay-reserve wait: reason=%s candidates=%zu "
                                    "newer=%zu lowWater=%u target=%u span=%lldus initialSpan=%lldus "
                                    "freshened=%u waited=%lldus budget=%lldus smoothAttempt=%d "
                                    "smoothFrames=%u/%u capLimited=%d",
                                    wgcStartupReserveReason.c_str(), startupCandidates.size(),
                                    newerStartupReserveFrames,
                                    getWgcDelayReservoirLowWaterFramesForDelay(startupContentDelayTargetQpc),
                                    getWgcDelayReservoirTargetFramesForDelay(startupContentDelayTargetQpc),
                                    static_cast<long long>(wgcStartupReserveSpanUs),
                                    static_cast<long long>(wgcStartupReserveWaitInitialSpanUs),
                                    wgcStartupReserveWaitFreshenedMax,
                                    static_cast<long long>(qpcToUs(waitNow.QuadPart - wgcStartupReserveWaitStartQpc)),
                                    static_cast<long long>(qpcToUs(waitBudgetQpc)), smoothnessStartupAttempted ? 1 : 0,
                                    smoothnessRetainedFrames, smoothnessDesiredFrames, smoothnessCapLimited ? 1 : 0);
                            }
                            continue;
                        }
                        const bool noStartupSpanGrowth = wgcStartupReserveSpanUs <= 0 &&
                                                         wgcStartupReserveSpanUs <= wgcStartupReserveWaitInitialSpanUs;
                        wgcStartupReserveReason =
                            noStartupSpanGrowth
                                ? "source_startup_underfeed"
                                : (startupReserveBelowLowWater ? "low_water_timeout" : "reserve_timeout");
                    }

                    bool startupPartialReserveFallback = false;
                    if (ce::capture_policy::ShouldPreserveWgcStartupPartialReserve(
                            startupCandidates.size(), startupReserveSelection.reserveSpanQpc,
                            startupContentDelayTargetQpc > 0, startupReserveMissing)) {
                        selectedStartupIndex = 0;
                        startupPartialReserveFallback = true;
                        if (wgcStartupReserveReason != "source_startup_underfeed") {
                            wgcStartupReserveReason = "partial_span_timeout";
                        }
                    }

                    const int64_t latestStartupSelectionQpc =
                        startupSelectionQpcs.empty() ? 0 : startupSelectionQpcs.back();
                    int64_t selectedStartupSelectionQpc =
                        selectedStartupIndex < startupCandidates.size()
                            ? GetFrameSelectionTimestamp(startupCandidates[selectedStartupIndex].frame)
                            : 0;
                    int64_t actualStartupDelayQpc = latestStartupSelectionQpc > selectedStartupSelectionQpc
                                                        ? latestStartupSelectionQpc - selectedStartupSelectionQpc
                                                        : 0;
                    const int64_t pileupSmoothnessActiveDelayQpc =
                        ce::capture_policy::SelectWgcStartupSmoothnessExtraDelayQpc(
                            actualStartupDelayQpc, avContentDelayQpc, smoothnessTargetDelayQpc);
                    // When the reserve fill is UNDERFED (buildable reservoir target never reached) AND the
                    // source is delivering at/above the CFR target, do not let the non-deterministic startup
                    // buffer pile-up set the permanent read delay: a deep accidental lock starves fresh-frame
                    // headroom and clusters "too-new" repeat holds for the whole session (startup-timing-
                    // dependent judder). Pin to the measured jitter floor when that is shallower. Sync-neutral:
                    // the extra delay is absorbed by the live-start schedule offset; audio stays anchored to
                    // avContentDelay. Sources BELOW the CFR target keep the deep reservoir (lull absorption).
                    const bool startupMinWindowSourceAtOrAboveCfr =
                        g_WgcCap && ce::capture_policy::IsWgcIngressSourceAtOrAboveCfrTarget(
                                        std::max<uint32_t>(1u, static_cast<uint32_t>(config.video.fps)),
                                        g_WgcCap->GetInputMin250Fps(), g_WgcCap->GetInputMin500Fps());
                    const bool startupCandidateCadenceAtOrAboveCfr =
                        ce::capture_policy::IsWgcStartupCandidateCadenceAtOrAboveCfrTarget(
                            startupCandidates.size(), startupReserveSelection.reserveSpanQpc, targetIntervalTicks);
                    const bool startupSourceAtOrAboveCfr =
                        startupMinWindowSourceAtOrAboveCfr || startupCandidateCadenceAtOrAboveCfr;
                    wgcSmoothnessActiveDelayQpc = ce::capture_policy::ResolveWgcStartupSmoothnessActiveDelayQpc(
                        pileupSmoothnessActiveDelayQpc, wgcSmoothnessFloorDelayQpc, startupPartialReserveFallback,
                        startupSourceAtOrAboveCfr);
                    if (startupPartialReserveFallback && !startupSelectionQpcs.empty() &&
                        latestStartupSelectionQpc > 0) {
                        const size_t fallbackIndexBeforeContractRecalculation = selectedStartupIndex;
                        const int64_t recalculatedContentDelayQpc =
                            avContentDelayQpc + std::max<int64_t>(0, wgcSmoothnessActiveDelayQpc);
                        const int64_t recalculatedTargetQpc =
                            latestStartupSelectionQpc > recalculatedContentDelayQpc
                                ? latestStartupSelectionQpc - recalculatedContentDelayQpc
                                : latestStartupSelectionQpc;
                        selectedStartupIndex = ce::capture_policy::SelectNearestMonotonicTimestampIndex(
                            startupSelectionQpcs.data(), startupSelectionQpcs.size(), recalculatedTargetQpc);
                        selectedStartupSelectionQpc = startupSelectionQpcs[selectedStartupIndex];
                        actualStartupDelayQpc = latestStartupSelectionQpc > selectedStartupSelectionQpc
                                                    ? latestStartupSelectionQpc - selectedStartupSelectionQpc
                                                    : 0;
                        wgcSmoothnessActiveDelayQpc = ce::capture_policy::SelectWgcStartupSmoothnessExtraDelayQpc(
                            actualStartupDelayQpc, avContentDelayQpc, smoothnessTargetDelayQpc);
                        LogInfo(
                            "[EncoderThread] WGC partial reservoir contract recalculated: oldIndex=%zu newIndex=%zu "
                            "latestQpc=%lld targetQpc=%lld selectedQpc=%lld realizedContentDelayUs=%lld "
                            "renderDelayUs=%lld smoothReserveUs=%lld (frame selection and delay changed together)",
                            fallbackIndexBeforeContractRecalculation, selectedStartupIndex,
                            static_cast<long long>(latestStartupSelectionQpc),
                            static_cast<long long>(recalculatedTargetQpc),
                            static_cast<long long>(selectedStartupSelectionQpc),
                            static_cast<long long>(qpcDeltaToUs(actualStartupDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(avContentDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(wgcSmoothnessActiveDelayQpc)));
                    }
                    if (wgcSmoothnessActiveDelayQpc < pileupSmoothnessActiveDelayQpc) {
                        LogInfo(
                            "[EncoderThread] WGC startup underfed active-delay capped to measured jitter floor: "
                            "pileupUs=%lld cappedUs=%lld floorUs=%lld minWindowProof=%d candidateProof=%d "
                            "candidates=%zu candidateSpanUs=%lld reason=%s (avoids startup-timing-dependent deep-lock "
                            "repeat clustering; sync-neutral)",
                            static_cast<long long>(qpcDeltaToUs(pileupSmoothnessActiveDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(wgcSmoothnessActiveDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(wgcSmoothnessFloorDelayQpc)),
                            startupMinWindowSourceAtOrAboveCfr ? 1 : 0, startupCandidateCadenceAtOrAboveCfr ? 1 : 0,
                            startupCandidates.size(),
                            static_cast<long long>(qpcDeltaToUs(startupReserveSelection.reserveSpanQpc)),
                            wgcStartupReserveReason.c_str());
                    } else if (startupPartialReserveFallback) {
                        // The fortistutter session showed this decision silently NOT engaging because the
                        // barrier-time min-window input rate was polluted by pre-live settling gaps
                        // (MinIn250=104 for a healthy 140 fps source). Log the gate inputs so a dormant
                        // cap is diagnosable instead of invisible.
                        LogInfo(
                            "[EncoderThread] WGC startup underfed active-delay cap NOT engaged: pileupUs=%lld "
                            "floorUs=%lld sourceAtOrAboveCfr=%d minWindowProof=%d candidateProof=%d "
                            "candidates=%zu candidateSpanUs=%lld inputMin250=%u inputMin500=%u outputFps=%u "
                            "reason=%s (deep pile-up lock retained for lull absorption)",
                            static_cast<long long>(qpcDeltaToUs(pileupSmoothnessActiveDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(wgcSmoothnessFloorDelayQpc)),
                            startupSourceAtOrAboveCfr ? 1 : 0, startupMinWindowSourceAtOrAboveCfr ? 1 : 0,
                            startupCandidateCadenceAtOrAboveCfr ? 1 : 0, startupCandidates.size(),
                            static_cast<long long>(qpcDeltaToUs(startupReserveSelection.reserveSpanQpc)),
                            g_WgcCap ? g_WgcCap->GetInputMin250Fps() : 0u,
                            g_WgcCap ? g_WgcCap->GetInputMin500Fps() : 0u, static_cast<uint32_t>(config.video.fps),
                            wgcStartupReserveReason.c_str());
                    }
                    wgcSmoothnessActualFrames =
                        targetIntervalTicks > 0
                            ? SaturatingToUint32(static_cast<uint64_t>(
                                  (wgcSmoothnessActiveDelayQpc + targetIntervalTicks / 2) / targetIntervalTicks))
                            : 0u;

                    uint32_t startupRetainedCapTrimmed = 0;
                    size_t startupKeptReserveFrames = 0;
                    for (size_t i = 0; i < startupCandidates.size(); ++i) {
                        if (i < selectedStartupIndex) {
                            ReleaseQueuedFrameTexture(startupCandidates[i].frame);
                            ++startupDiscardedOlder;
                        } else if (i == selectedStartupIndex) {
                            frame = std::move(startupCandidates[i].frame);
                        } else if (startupReserveSelection.usedDelayReserve || startupPartialReserveFallback) {
                            if (smoothnessRetainedFrameCap == 0 ||
                                startupKeptReserveFrames < smoothnessRetainedFrameCap) {
                                bufferedWgcFrames.push_back(std::move(startupCandidates[i].frame));
                                ++startupKeptReserveFrames;
                            } else {
                                ReleaseQueuedFrameTexture(startupCandidates[i].frame);
                                ++startupRetainedCapTrimmed;
                            }
                        } else {
                            ReleaseQueuedFrameTexture(startupCandidates[i].frame);
                            ++startupDiscardedOlder;
                        }
                    }
                    if (startupRetainedCapTrimmed > 0) {
                        wgcRetainedCapTrimTotal += startupRetainedCapTrimmed;
                        wgcRetainedCapTrimWindow += startupRetainedCapTrimmed;
                    }

                    ++pendingWgcStartContractGeneration;
                    const int64_t selectedContentDelayQpc = getWgcEffectiveContentDelayQpc();
                    if (frame.timestamp > 0 && selectedContentDelayQpc >= 0 &&
                        frame.timestamp <= INT64_MAX - selectedContentDelayQpc) {
                        pendingWgcStartContract = ce::capture_policy::BuildCfrTimelineStartContract(
                            frame.timestamp, frame.timestamp + selectedContentDelayQpc, avContentDelayQpc);
                    } else {
                        pendingWgcStartContract = {};
                    }
                    if (pendingWgcStartContract.valid) {
                        LogInfo(
                            "[EncoderThread] WGC CFR start contract selected: generation=%llu videoQpc=%lld "
                            "selectionQpc=%lld liveQpc=%lld contentDelayUs=%lld renderDelayUs=%lld "
                            "smoothReserveUs=%lld retainedNewer=%zu prewarm=%s/%lldus",
                            static_cast<unsigned long long>(pendingWgcStartContractGeneration),
                            static_cast<long long>(pendingWgcStartContract.videoOriginQpc),
                            static_cast<long long>(GetFrameSelectionTimestamp(frame)),
                            static_cast<long long>(pendingWgcStartContract.liveQpc),
                            static_cast<long long>(qpcDeltaToUs(pendingWgcStartContract.contentDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(pendingWgcStartContract.renderLoopbackLatencyQpc)),
                            static_cast<long long>(qpcDeltaToUs(pendingWgcStartContract.smoothnessReserveQpc)),
                            bufferedWgcFrames.size(), wgcEncoderPrewarmSucceeded ? "ok" : "failed",
                            static_cast<long long>(wgcEncoderPrewarmElapsedUs));
                    } else {
                        LogWarn(
                            "[EncoderThread] ERROR: WGC CFR start contract selection failed: generation=%llu "
                            "videoQpc=%lld contentDelayUs=%lld renderDelayUs=%lld prewarm=%s/%lldus",
                            static_cast<unsigned long long>(pendingWgcStartContractGeneration),
                            static_cast<long long>(frame.timestamp),
                            static_cast<long long>(qpcDeltaToUs(selectedContentDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(avContentDelayQpc)),
                            wgcEncoderPrewarmSucceeded ? "ok" : "failed",
                            static_cast<long long>(wgcEncoderPrewarmElapsedUs));
                    }
                    updateWgcIngressPressure("startup-selected");

                    LARGE_INTEGER anchorNow;
                    QueryPerformanceCounter(&anchorNow);
                    const int64_t startupReserveWaitedUs =
                        wgcStartupReserveWaitStartQpc > 0 ? qpcToUs(anchorNow.QuadPart - wgcStartupReserveWaitStartQpc)
                                                          : 0;
                    const int64_t startupReserveSpanGrowthUs =
                        wgcStartupReserveWaitStartQpc > 0
                            ? std::max<int64_t>(0, wgcStartupReserveSpanUs - wgcStartupReserveWaitInitialSpanUs)
                            : 0;
                    const int64_t startupSelectedDelayUs = qpcDeltaToUs(actualStartupDelayQpc);
                    const bool startupReserveFallback =
                        startupContentDelayTargetQpc > 0 && !startupReserveSelection.usedDelayReserve;
                    const int64_t startDeltaUs =
                        ((frame.timestamp - wgcStartupBarrierQpc) * 1000000) / qpcFreq.QuadPart;
                    const int64_t frameAgeUs =
                        anchorNow.QuadPart >= frame.timestamp
                            ? ((anchorNow.QuadPart - frame.timestamp) * 1000000) / qpcFreq.QuadPart
                            : 0;
                    const bool startupSmoothnessUnderfed =
                        smoothnessStartupAttempted && wgcSmoothnessActiveDelayQpc < smoothnessTargetDelayQpc;
                    LogInfo(
                        "[EncoderThread] WGC startup sync post-delay barrier satisfied: anchorQpc=%lld "
                        "firstFrameQpc=%lld delta=%lldus frameAge=%lldus droppedPostDelay=%u "
                        "discardedBeforeDelay=%u freshened=%zu bufferedExamined=%zu queueExamined=%zu "
                        "discardedOlder=%zu discardedBeforeBarrier=%zu pathMismatch=%zu startupReserveFrames=%u "
                        "startupReserveSpanUs=%lld startupDelayTargetUs=%lld startupSelectedByDelayReserve=%d "
                        "startupReserveReason=%s keptReserveFrames=%zu startupReserveWaits=%u "
                        "startupReserveWaitedUs=%lld startupReserveInitialSpanUs=%lld "
                        "startupReserveSpanGrowthUs=%lld startupReserveWaitFreshened=%u "
                        "startupSelectedIndex=%zu startupSelectedDelayUs=%lld startupFallback=%d "
                        "startupPartialReserveFallback=%d smoothAttempt=%d smoothDesiredFrames=%u "
                        "smoothRetainedFrames=%u smoothActualFrames=%u smoothTargetDelayUs=%lld "
                        "smoothDelayUs=%lld smoothStartupUnderfed=%d smoothPoolSlots=%u retainedCap=%u "
                        "reservedFreeSlots=%u retainedCapTrimmed=%u smoothCapLimited=%d "
                        "startupEffectiveDelayUs=%lld smoothReason=%s",
                        static_cast<long long>(wgcStartupBarrierQpc), static_cast<long long>(frame.timestamp),
                        static_cast<long long>(startDeltaUs), static_cast<long long>(frameAgeUs),
                        wgcStartupBarrierDroppedFrames, wgcStartupPreLiveDelayDroppedFrames, startupFreshened,
                        startupBufferedExamined, startupQueueExamined, startupDiscardedOlder,
                        startupDiscardedBeforeBarrier, startupDiscardedPathMismatch, wgcStartupReserveFrames,
                        static_cast<long long>(wgcStartupReserveSpanUs),
                        static_cast<long long>(wgcStartupDelayTargetUs), wgcStartupSelectedByDelayReserve ? 1 : 0,
                        wgcStartupReserveReason.c_str(), bufferedWgcFrames.size(), wgcStartupReserveWaitCount,
                        static_cast<long long>(startupReserveWaitedUs),
                        static_cast<long long>(wgcStartupReserveWaitInitialSpanUs),
                        static_cast<long long>(startupReserveSpanGrowthUs), wgcStartupReserveWaitFreshenedMax,
                        selectedStartupIndex, static_cast<long long>(startupSelectedDelayUs),
                        startupReserveFallback ? 1 : 0, startupPartialReserveFallback ? 1 : 0,
                        smoothnessStartupAttempted ? 1 : 0, smoothnessDesiredFrames, smoothnessRetainedFrames,
                        wgcSmoothnessActualFrames, static_cast<long long>(qpcDeltaToUs(smoothnessTargetDelayQpc)),
                        static_cast<long long>(qpcDeltaToUs(wgcSmoothnessActiveDelayQpc)),
                        startupSmoothnessUnderfed ? 1 : 0, smoothnessPoolSlots, smoothnessRetainedFrameCap,
                        smoothnessReservedFreeSlots, startupRetainedCapTrimmed, smoothnessCapLimited ? 1 : 0,
                        static_cast<long long>(qpcDeltaToUs(getWgcEffectiveContentDelayQpc())),
                        wgcSmoothnessBufferReason.c_str());
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
                wgcCfrPhaseLock.Reset();
                smoothedEncCycleMs = 0.0;
                smoothedInjectServiceMs = 0.0;
                injectServiceMaxUs = 0;
                injectCfrRecoveryActive = false;
                injectEncoderServiceTooSlowCurrent = false;
                injectCfrRecoveryStartTick = 0;
                injectCfrRecoveryStartDebt = 0;
                injectCfrRecoveryBestDebt = 0;
                injectCfrRecoveryStartFreshCatchup = injectFreshCatchupTotal;
                injectCfrRecoveryStartRepeatCatchup = injectRepeatCatchupTotal;
                injectCfrRecoveryLastProgressLogTick = 0;
                encCycleMaxMs = 0;
                dupTimestampCount = 0;
                lastWgcDuplicateTimestampSkipCountForCadence =
                    g_WgcCap ? g_WgcCap->GetDuplicateTimestampSkipCount() : 0u;
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
                wgcSyncDelayHoldCount = 0;
                wgcSyncDelayHoldTotal = 0;
                wgcSyncDelaySourceLimitedHoldCount = 0;
                wgcSyncDelaySourceLimitedHoldTotal = 0;
                wgcSyncDelayPolicyHoldCount = 0;
                wgcSyncDelayPolicyHoldTotal = 0;
                wgcTooNewLeadMaxUs = 0;
                wgcTooNewLeadSessionMaxUs = 0;
                wgcDelaySoftLateRejectedTotal = 0;
                wgcDelaySoftLateRejectedWindow = 0;
                wgcDelaySoftLateAcceptedTotal = 0;
                wgcDelaySoftLateAcceptedWindow = 0;
                wgcDelayNearCapAcceptedTotal = 0;
                wgcDelayNearCapAcceptedWindow = 0;
                wgcDelayUniformCadenceTotal = 0;
                wgcDelayUniformCadenceWindow = 0;
                wgcDelayUniformHoldTotal = 0;
                wgcDelayUniformHoldWindow = 0;
                wgcDelayPaceCapTrimTotal = 0;
                wgcDelayPaceCapTrimWindow = 0;
                wgcRetainedCapTrimTotal = 0;
                wgcRetainedCapTrimWindow = 0;
                wgcPoolPressureTrimTotal = 0;
                wgcPoolPressureTrimWindow = 0;
                wgcDelayOlderFrameAvoidedRepeatTotal = 0;
                wgcDelayOlderFrameAvoidedRepeatWindow = 0;
                wgcDelaySourceLimitedRepeatTotal = 0;
                wgcDelaySourceLimitedRepeatWindow = 0;
                wgcDelayRepeatRescueAttemptTotal = 0;
                wgcDelayRepeatRescueAttemptWindow = 0;
                wgcDelayRepeatRescueSuccessTotal = 0;
                wgcDelayRepeatRescueSuccessWindow = 0;
                wgcDelayRepeatRescueRejectedSyncTotal = 0;
                wgcDelayRepeatRescueRejectedSyncWindow = 0;
                wgcDelayRepeatRescueRejectedHeadroomTotal = 0;
                wgcDelayRepeatRescueRejectedHeadroomWindow = 0;
                wgcDelayRepeatRescueRejectedCostTotal = 0;
                wgcDelayRepeatRescueRejectedCostWindow = 0;
                wgcDelayRepeatPromotedBeforeRepeatTotal = 0;
                wgcDelayRepeatPromotedBeforeRepeatWindow = 0;
                wgcDelayRepeatPromotionAttemptTotal = 0;
                wgcDelayRepeatPromotionAttemptWindow = 0;
                wgcDelayRepeatPromotionRejectedSoftTotal = 0;
                wgcDelayRepeatPromotionRejectedSoftWindow = 0;
                wgcDelayRepeatSafeAfterPromotionTotal = 0;
                wgcDelayRepeatSafeAfterPromotionWindow = 0;
                wgcDelayRepeatWithSafeCandidateTotal = 0;
                wgcDelayRepeatWithSafeCandidateWindow = 0;
                wgcDelayRepeatWithoutSafeCandidateTotal = 0;
                wgcDelayRepeatWithoutSafeCandidateWindow = 0;
                wgcDelayRepeatWithSoftSafeCandidateTotal = 0;
                wgcDelayRepeatWithSoftSafeCandidateWindow = 0;
                wgcDelayRepeatWithoutSoftSafeCandidateTotal = 0;
                wgcDelayRepeatWithoutSoftSafeCandidateWindow = 0;
                wgcDelayRepeatHardOnlyCandidateTotal = 0;
                wgcDelayRepeatHardOnlyCandidateWindow = 0;
                wgcDelaySyncProtectedRepeatTotal = 0;
                wgcDelaySyncProtectedRepeatWindow = 0;
                wgcDelayWindowHealthyRepeatTotal = 0;
                wgcDelayWindowHealthyRepeatWindow = 0;
                wgcDelayWindowRecoverableRepeatTotal = 0;
                wgcDelayWindowRecoverableRepeatWindow = 0;
                wgcDelayWindowSourceLimitedRepeatTotal = 0;
                wgcDelayWindowSourceLimitedRepeatWindow = 0;
                wgcDelayWindowHardStallRepeatTotal = 0;
                wgcDelayWindowHardStallRepeatWindow = 0;
                wgcDelayWindowPostStallRepeatTotal = 0;
                wgcDelayWindowPostStallRepeatWindow = 0;
                wgcDelayPostStallSafeFrameTotal = 0;
                wgcDelayPostStallSafeFrameWindow = 0;
                wgcDelayRepeatReserveDepthMax = 0;
                wgcDelayRepeatReserveDepthWindowMax = 0;
                wgcDelayRepeatReserveSpanMaxUs = 0;
                wgcDelayRepeatReserveSpanWindowMaxUs = 0;
                wgcDelayOldestSoftSafeAgeMaxUs = 0;
                wgcDelayOldestSoftSafeAgeWindowMaxUs = 0;
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
                lastEmittedWgcSelectionQpc = 0;
                lastEmittedInjectSourceQpc = 0;
                const size_t liveInjectReserveFrames = ce::capture_policy::GetInjectReserveFrames(
                    config.video.useVFR, smoothedInjectFenceMs, frameIntervalMs);
                pendingLiveInjectReadyFrames =
                    useScreenGrab
                        ? 0
                        : (config.video.useVFR
                               ? ce::capture_policy::GetWarmupInjectKeepCount(smoothedInjectFenceMs, frameIntervalMs)
                               : ce::capture_policy::GetInjectCfrStartupReadyFrames(liveInjectReserveFrames,
                                                                                   injectContentDelayFrames));
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
                liveStartQpc.QuadPart = 0;  // Commit the pending start contract after the first successful encode.
                encoderGridStartQpc = nextSampleTime.QuadPart;
                // Inject warmup is causal and must discard stale queued work. WGC/DXGI,
                // however, just selected an intentional look-ahead reservoir; those
                // newer frames are part of the immutable start contract and must survive
                // the live handoff.
                if (!useScreenGrab) {
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
                    // Preserve enough history to cover the configured A/V content-delay
                    // target plus the physical fence tail on the first live CFR slot.
                    const size_t keepCount =
                        config.video.useVFR
                            ? ce::capture_policy::GetWarmupInjectKeepCount(smoothedInjectFenceMs, frameIntervalMs)
                            : ce::capture_policy::GetInjectCfrStartupReadyFrames(liveInjectReserveFrames,
                                                                                injectContentDelayFrames);
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
                if (useScreenGrab) {
                    LogInfo(
                        "[EncoderThread] Preserved transactional WGC startup reserve at live handoff: "
                        "generation=%llu buffered=%zu queued=%zu contractValid=%d contentDelayUs=%lld",
                        static_cast<unsigned long long>(pendingWgcStartContractGeneration), bufferedWgcFrames.size(),
                        g_FrameQueue.Size(), pendingWgcStartContract.valid ? 1 : 0,
                        static_cast<long long>(qpcToUs(pendingWgcStartContract.contentDelayQpc)));
                }
                // Reset counters so per-second logs start clean at going-live.
                g_InjectBufferedTrimmedFrames.store(0, std::memory_order_relaxed);
                g_InjectCadenceDroppedFrames.store(0, std::memory_order_relaxed);
                LogInfo(
                    "[EncoderThread] Warmup ready after %llums hidden warmup (%s, hiddenFrames=%u, inputRate=%.3f, "
                    "readyFrames=%zu freshWgc=%u)",
                    static_cast<unsigned long long>(warmupElapsedMs64), useScreenGrab ? "WGC" : "inject",
                    hiddenStartupFrames, smoothedInputPerTick, pendingLiveInjectReadyFrames, wgcFreshWarmupFrameCount);
                ResetWarmupWgcFreshness(false);
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
            if (MediaEngine_SetWgcStartupExtraDelayQpc) {
                const int64_t startupSmoothExtraDelayQpc = useScreenGrab ? wgcSmoothnessActiveDelayQpc : 0;
                MediaEngine_SetWgcStartupExtraDelayQpc(startupSmoothExtraDelayQpc);
                if (useScreenGrab) {
                    LogInfo(
                        "[EncoderThread] WGC startup smoothness delay applied to media engine: smoothDelayUs=%lld "
                        "smoothFrames=%u/%u/%u smoothReason=%s",
                        static_cast<long long>(qpcToUs(startupSmoothExtraDelayQpc)), wgcSmoothnessActualFrames,
                        wgcSmoothnessRetainedFrames, wgcSmoothnessDesiredFrames, wgcSmoothnessBufferReason.c_str());
                }
            }
            lastDeferredLineage = InjectFrameLineage{};
            ResetWarmupWgcFreshness(false);
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
            // Keep the last successfully emitted frame authoritative until the
            // fresh candidate has actually encoded. This also keeps inject ring
            // leases attached to deferred candidates instead of accidentally
            // moving them into g_LastFrame before the fence result is known.
            frameToProcess = &frame;
        } else if (g_HasLastFrame && g_EncoderRunning && g_Recording) {
            if (hasRepeatLastFramePath) {
                wantsTrueRepeatLastFrame = true;
                isDuplicate = true;
            } else {
                if (!g_LastFrame.isInjectMode && g_LastFrame.timestamp > 0) {
                    lastEmittedWgcSourceQpc = g_LastFrame.timestamp;
                }
                if (!g_LastFrame.isInjectMode && GetFrameSelectionTimestamp(g_LastFrame) > 0) {
                    lastEmittedWgcSelectionQpc = GetFrameSelectionTimestamp(g_LastFrame);
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
                    if (isWgcEffectiveContentDelayActive()) {
                        ++wgcDelayReservoirLowWaterTickCount;
                        ++wgcDelayReservoirLowWaterTickTotal;
                    }
                }
            }
            wgcNoFreshTickPermille = wgcQueueTickSampleCount > 0
                                         ? SaturatingToUint32((static_cast<uint64_t>(wgcNoFreshTickCount) * 1000ull) /
                                                              static_cast<uint64_t>(wgcQueueTickSampleCount))
                                         : 0u;
        }
        if (scheduledLiveCfrTick && !useScreenGrab) {
            // Timer rebases keep the worker wake cadence near wall time, while liveTicksOutput owns the
            // immutable CFR media grid. Keeping those clocks separate lets inject recovery submit an
            // overdue extra slot without postponing the next normal 120 Hz wake by another tick.
            scheduledOutputQpc = ce::capture_policy::GetNextInjectCfrOutputQpc(
                liveStartQpc.QuadPart, liveTicksOutput, targetIntervalTicks, scheduledSampleQpc);
        }

        auto recordDuplicate = [&](const QueuedFrame* duplicateFrame, const InjectFrameLineage* duplicateLineage,
                                   bool duplicateFromDrainReason, bool duplicateFromDeferredReason,
                                   bool duplicateFromTimerRebaseReason, bool duplicateFromCatchupReason = false) {
            cadenceCounters.consecutiveDuplicateFrames++;
            cadenceCounters.maxConsecutiveDuplicateFrames =
                std::max(cadenceCounters.maxConsecutiveDuplicateFrames, cadenceCounters.consecutiveDuplicateFrames);
            // Session-wide contiguous run: survives the per-window cadence reset so a >1s freeze is
            // measured as one run (the real visible-freeze metric), not split per logging window.
            ++captureSessionSummary.currentContiguousDupTicks;
            captureSessionSummary.longestContiguousDupTicks =
                std::max(captureSessionSummary.longestContiguousDupTicks,
                         static_cast<uint64_t>(captureSessionSummary.currentContiguousDupTicks));
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
        auto advanceWakeDeadlineForCatchupTick = [&]() {
            if (ce::capture_policy::ShouldAdvanceWakeDeadlineForCfrCatchupTick(useScreenGrab,
                                                                               injectCfrRecoveryActive)) {
                nextSampleTime.QuadPart += targetIntervalTicks;
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
                                          : "Inject fresh catch-up remains gated by encoder health and target coverage.");
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
                    scheduledOutputQpc + static_cast<int64_t>(extraTick) * targetIntervalTicks;

                if (!useScreenGrab && !config.video.useVFR && MediaEngine_ProcessFrame) {
                    const size_t catchupInjectReserveFrames = ce::capture_policy::GetInjectReserveFrames(
                        config.video.useVFR, smoothedInjectFenceMs, frameIntervalMs);
                    const size_t catchupMinBufferedInjectFrames = ce::capture_policy::GetMinBufferedInjectFrames(
                        catchupInjectReserveFrames, recordingOutputLive);
                    const bool encoderBottleneckedNow = g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
                    const bool allowFreshInjectCatchup = ce::capture_policy::ShouldUseFreshInjectCatchup(
                        config.video.useVFR, encoderBottleneckedNow, injectEncoderServiceTooSlowCurrent,
                        bufferedInjectFrames.size(), catchupMinBufferedInjectFrames, outputShortfallTicks,
                        injectCfrRecoveryActive);
                    if (allowFreshInjectCatchup) {
                        size_t availableCount = bufferedInjectFrames.size() - catchupMinBufferedInjectFrames;
                        const int64_t baseCatchupPlayoutTargetQpc =
                            ComputeDelayedContentGridStartQpc(repeatScheduledQpc, avContentDelayQpc);
                        const int64_t catchupPhaseReferenceQpc =
                            bufferedInjectFrames.empty() ? 0 : bufferedInjectFrames.back().timestamp;
                        const int64_t catchupPlayoutTargetQpc = applyCaptureSyncPhaseTarget(
                            "inject", injectCfrPhaseLock, baseCatchupPlayoutTargetQpc,
                            catchupPhaseReferenceQpc);
                        const int64_t catchupLeadToleranceQpc =
                            ce::capture_policy::GetInjectCfrSelectionLeadToleranceQpc(targetIntervalTicks);
                        auto isFreshInjectCandidate = [&](const QueuedFrame& candidate) {
                            return ce::capture_policy::IsInjectFrameFreshAfterLastEmission(candidate.timestamp,
                                                                                           lastEmittedInjectSourceQpc);
                        };
                        auto isAllowedCandidate = [&](const QueuedFrame& candidate) {
                            return isFreshInjectCandidate(candidate) &&
                                   !MatchesInjectFrameLineage(candidate, lastDeferredLineage);
                        };
                        size_t bestIdx = SelectFrameClosestToTimestampIf(
                            bufferedInjectFrames, availableCount, catchupPlayoutTargetQpc, isAllowedCandidate);
                        if (bestIdx >= availableCount) {
                            bestIdx = SelectFrameClosestToTimestampIf(bufferedInjectFrames, availableCount,
                                                                      catchupPlayoutTargetQpc,
                                                                      isFreshInjectCandidate);
                        }

                        const bool catchupTargetCovered =
                            bestIdx < availableCount && isFreshInjectCandidate(bufferedInjectFrames[bestIdx]) &&
                            ce::capture_policy::DecideCfrNearestPlayout(
                                bufferedInjectFrames[bestIdx].timestamp, catchupPlayoutTargetQpc,
                                catchupLeadToleranceQpc, lastEmittedInjectSourceQpc)
                                .emit;
                        if (catchupTargetCovered) {
                            for (size_t i = 0; i < bestIdx; ++i) {
                                QueuedFrame stale = std::move(bufferedInjectFrames.front());
                                bufferedInjectFrames.pop_front();
                                DiscardQueuedFrame(stale);
                                g_InjectCadenceDroppedFrames.fetch_add(1, std::memory_order_relaxed);
                                if (g_pSharedMem) {
                                    g_pSharedMem->runtimeState.injectCadenceDrops.fetch_add(1,
                                                                                            std::memory_order_relaxed);
                                }
                                ++injectTargetSupersededThisWindow;
                                ++injectTargetSupersededTotal;
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
                                catchupFrame.format, catchupFrame.isHDR, catchupFrame.isShmem, catchupFrame.shmemSlot,
                                &catchupFrame.cursorState);
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
                                if (g_HasLastFrame && !g_LastFrame.isInjectMode) {
                                    ReleaseQueuedFrameTexture(g_LastFrame);
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
                                }

                                catchupFrame.injectRingLease.Reset();
                                g_LastFrame = std::move(catchupFrame);
                                g_HasLastFrame = true;
                                lastSuccessfullyEncodedInjectLineage = catchupLineage;
                                if (g_LastFrame.timestamp > 0) {
                                    lastEmittedInjectSourceQpc = g_LastFrame.timestamp;
                                }
                                lastDeferredLineage = {};
                                ++injectTargetSelectThisWindow;
                                ++injectTargetSelectTotal;
                                if (qpcFreq.QuadPart > 0) {
                                    const uint64_t residualUs =
                                        ce::capture_policy::GetCfrTimestampDistanceQpc(
                                            g_LastFrame.timestamp, catchupPlayoutTargetQpc) *
                                        1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
                                    injectTargetResidualMaxUs =
                                        std::max(injectTargetResidualMaxUs, SaturatingToUint32(residualUs));
                                }
                                cadenceCounters.consecutiveDeferredFrames = 0;
                                cadenceCounters.consecutiveDuplicateFrames = 0;
                                captureSessionSummary.currentContiguousDupTicks = 0;
                                cadenceCounters.liveTickEmitCount++;
                                cadenceCounters.liveTickUniqueCount++;
                                cadenceCounters.CommitHoldRun();
                                cadenceCounters.holdTicksRunning = 1;
                                ++liveTicksOutput;
                                ++encoderGridTickCount;
                                ++cfrCatchupTicksExecuted;
                                ++injectFreshCatchupThisWindow;
                                ++injectFreshCatchupTotal;
                                advanceWakeDeadlineForCatchupTick();
                                continue;
                            }

                            if (catchupEncodeDeferred) {
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
                    const int64_t baseCatchupSelectionTargetQpc = clampWgcSelectionTargetQpc(
                        computeWgcSelectionTargetForTick(repeatScheduledQpc, catchupGridTick, false),
                        catchupNowQpc.QuadPart);
                    const int64_t catchupPhaseReferenceQpc =
                        GetFrameSelectionTimestamp(bufferedWgcFrames.back());
                    const int64_t catchupSelectionTargetQpc = applyCaptureSyncPhaseTarget(
                        "wgc", wgcCfrPhaseLock, baseCatchupSelectionTargetQpc, catchupPhaseReferenceQpc);
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
                            LARGE_INTEGER catchupStartEnc, catchupEndEnc;
                            QueryPerformanceCounter(&catchupStartEnc);
                            uint64_t frameAgeUs = 0;
                            if (catchupFrame.timestamp > 0 && catchupStartEnc.QuadPart > catchupFrame.timestamp) {
                                frameAgeUs = static_cast<uint64_t>((catchupStartEnc.QuadPart - catchupFrame.timestamp) *
                                                                   1000000 / qpcFreq.QuadPart);
                            }
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
                            SyncDuplicationCursorSuppression(catchupFrame.wgcCursorEmbedded);
                            const ce::cursor::CaptureState catchupCursorState =
                                selectCursorStateForScheduledQpc(repeatScheduledQpc, catchupFrame, "fresh-catchup");
                            const bool freshCatchupEncodeSucceeded = MediaEngine_ProcessFrameD3D11(
                                catchupFrame.texture, catchupFrame.timestamp, catchupFrame.width, catchupFrame.height,
                                catchupFrame.isHDR, catchupFrame.captureLeft, catchupFrame.captureTop,
                                catchupTimelineElapsedUs, &catchupCursorState);
                            const bool recoveredCatchupEncodeFailure =
                                !freshCatchupEncodeSucceeded &&
                                recoverScheduledFreshEncodeFailure(true, false, false, repeatScheduledQpc,
                                                                   &catchupFrame, "WGC fresh-catchup");
                            if (!freshCatchupEncodeSucceeded && !recoveredCatchupEncodeFailure) {
                                ReleaseQueuedFrameTexture(catchupFrame);
                                cadenceCounters.liveTickMissCount++;
                                break;
                            }
                            releaseWgcLeaseAfterMediaEngineCopy(
                                catchupFrame, recoveredCatchupEncodeFailure ? "fresh-catchup encode-failure repeat"
                                                                            : "fresh-catchup");
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

                            if (recoveredCatchupEncodeFailure) {
                                ReleaseQueuedFrameTexture(catchupFrame);
                                recordDuplicate(nullptr, nullptr, false, false, false, true);
                                ++wgcRepeatCatchupCount;
                                cadenceCounters.liveTickEmitCount++;
                                cadenceCounters.liveTickDuplicateCount++;
                                cadenceCounters.holdTicksRunning++;
                                ++liveTicksOutput;
                                ++encoderGridTickCount;
                                ++cfrCatchupTicksExecuted;
                                advanceWakeDeadlineForCatchupTick();
                                continue;
                            }

                            if (g_HasLastFrame && !g_LastFrame.isInjectMode) {
                                ReleaseQueuedFrameTexture(g_LastFrame);
                            }
                            g_LastFrame = std::move(catchupFrame);
                            g_HasLastFrame = true;
                            cadenceCounters.frameAgeAccumUs += frameAgeUs;
                            cadenceCounters.frameAgeSamples++;
                            cadenceCounters.frameAgeMaxUs =
                                std::max(cadenceCounters.frameAgeMaxUs, SaturatingToUint32(frameAgeUs));

                            if (g_LastFrame.timestamp > 0) {
                                lastEmittedWgcSourceQpc = g_LastFrame.timestamp;
                            }
                            if (GetFrameSelectionTimestamp(g_LastFrame) > 0) {
                                lastEmittedWgcSelectionQpc = GetFrameSelectionTimestamp(g_LastFrame);
                            }
                            lastSuccessfulWgcCursorEmbedded = g_LastFrame.wgcCursorEmbedded;
                            hasSuccessfulWgcCursorMetadata = true;

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
                            captureSessionSummary.currentContiguousDupTicks = 0;
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
                            advanceWakeDeadlineForCatchupTick();
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
                advanceWakeDeadlineForCatchupTick();
            }
        };

        if ((!frameToProcess || wantsTrueRepeatLastFrame) && scheduledLiveCfrTick && hasRepeatLastFramePath) {
            LARGE_INTEGER repeatStartEnc, repeatEndEnc;
            QueryPerformanceCounter(&repeatStartEnc);
            const bool duplicateFromDrain = isDrainPhase;
            bool repeatDuplicateFromDeferred = false;
            const bool repeatDuplicateFromTimerRebase = encoderLateTickCount >= 2;
            const InjectFrameLineage duplicateLineage =
                !useScreenGrab && lastSuccessfullyEncodedInjectLineage.IsValid()
                    ? lastSuccessfullyEncodedInjectLineage
                    : (g_HasLastFrame ? MakeInjectFrameLineage(g_LastFrame) : InjectFrameLineage{});
            bool encodeSucceeded = repeatLastFrameForScheduledQpc(scheduledOutputQpc);
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
                    if (repeatDuplicateFromTimerRebase) {
                        ++wgcRepeatTimerLateCount;
                    } else if (!frameToProcess && !wantsTrueRepeatLastFrame) {
                        ++wgcRepeatNoFreshCount;
                    } else if (wantsTrueRepeatLastFrame) {
                        ++wgcRepeatNoFreshCount;
                    }
                }
                recordDuplicate(nullptr, duplicateLineage.IsValid() ? &duplicateLineage : nullptr, duplicateFromDrain,
                                repeatDuplicateFromDeferred, repeatDuplicateFromTimerRebase);
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
            int64_t signedRawSelectionErrorUs = 0;
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
                if (!frameToProcess->isInjectMode) {
                    const int64_t rawSelectionTimestampQpc = getWgcRawSelectionTimestamp(*frameToProcess);
                    if (rawSelectionTimestampQpc > 0) {
                        signedRawSelectionErrorUs =
                            ((rawSelectionTimestampQpc - selectionMetricTargetQpc) * 1000000) / qpcFreq.QuadPart;
                    } else {
                        signedRawSelectionErrorUs = signedSelectionErrorUs;
                    }
                }
            }

            uint64_t frameAgeUs = 0;
            if (frameToProcess->timestamp > 0 && startEnc.QuadPart > frameToProcess->timestamp) {
                frameAgeUs =
                    static_cast<uint64_t>((startEnc.QuadPart - frameToProcess->timestamp) * 1000000 / qpcFreq.QuadPart);
            }
            if (scheduledLiveCfrTick && scheduledOutputQpc > 0) {
                const int64_t signedOutputScheduleErrorUs =
                    ((startEnc.QuadPart - scheduledOutputQpc) * 1000000) / qpcFreq.QuadPart;
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
                ce::cursor::CaptureState scheduledCursorState;
                const ce::cursor::CaptureState* cursorState = &frameToProcess->cursorState;
                if (scheduledLiveCfrTick && scheduledOutputQpc > 0) {
                    scheduledCursorState =
                        selectCursorStateForScheduledQpc(scheduledOutputQpc, *frameToProcess, "fresh");
                    cursorState = &scheduledCursorState;
                }
                if (frameToProcess->isInjectMode) {
                    encodeSucceeded = MediaEngine_ProcessFrame(
                        (uint64_t)frameToProcess->sharedHandle, (uint64_t)frameToProcess->fenceHandle,
                        frameToProcess->fenceValue, frameToProcess->timestamp, frameToProcess->luidLow,
                        frameToProcess->luidHigh, frameToProcess->sourcePid, frameToProcess->width,
                        frameToProcess->height, frameToProcess->format, frameToProcess->isHDR, frameToProcess->isShmem,
                        frameToProcess->shmemSlot, cursorState);
                    encodeDeferred = MediaEngine_WasLastFrameDeferred && MediaEngine_WasLastFrameDeferred();
                } else {
                    const int64_t liveTimelineElapsedUs =
                        scheduledLiveCfrTick ? computeLiveTimelineElapsedUs(scheduledOutputQpc) : -1;
                    SyncDuplicationCursorSuppression(frameToProcess->wgcCursorEmbedded);
                    encodeSucceeded = MediaEngine_ProcessFrameD3D11(
                        frameToProcess->texture, frameToProcess->timestamp, frameToProcess->width,
                        frameToProcess->height, frameToProcess->isHDR, frameToProcess->captureLeft,
                        frameToProcess->captureTop, liveTimelineElapsedUs, cursorState);
                    encodeDeferred = false;
                }
            };

            const bool attemptedFreshCandidate = popped && frameToProcess == &frame && !isDuplicate;
            encodeCurrentFrame();
            const bool recoveredFreshEncodeFailure =
                !encodeSucceeded &&
                recoverScheduledFreshEncodeFailure(scheduledLiveCfrTick, encodeSucceeded, encodeDeferred,
                                                   scheduledOutputQpc, frameToProcess, "main fresh frame");
            if (recoveredFreshEncodeFailure) {
                encodeSucceeded = true;
                encodeDeferred = false;
                isDuplicate = true;
            }

            if (attemptedFreshCandidate && !encodeDeferred) {
                if (recoveredFreshEncodeFailure) {
                    // The scheduled output contains the previous cached frame,
                    // not this candidate. Consume its ownership without
                    // changing last-successful source metadata.
                    if (frame.isInjectMode) {
                        frame.injectRingLease.Reset();
                    } else {
                        releaseWgcLeaseAfterMediaEngineCopy(frame, "main encode-failure repeat");
                        ReleaseQueuedFrameTexture(frame);
                    }
                    frame = QueuedFrame{};
                    frameToProcess = g_HasLastFrame ? &g_LastFrame : nullptr;
                    popped = false;
                } else if (encodeSucceeded) {
                    if (frame.isInjectMode) {
                        // The synchronous call has finished using the shared
                        // slot. Deferred candidates never enter this branch and
                        // retain their lease while queued for retry.
                        frame.injectRingLease.Reset();
                    } else {
                        releaseWgcLeaseAfterMediaEngineCopy(frame, "main");
                    }
                    if (g_HasLastFrame && !g_LastFrame.isInjectMode) {
                        ReleaseQueuedFrameTexture(g_LastFrame);
                    }
                    g_LastFrame = std::move(frame);
                    g_HasLastFrame = true;
                    frameToProcess = &g_LastFrame;
                } else {
                    // A hard fresh-frame failure consumed the synchronous call
                    // but emitted nothing. Release the candidate (including its
                    // inject ring lease) and preserve g_LastFrame unchanged.
                    if (frame.isInjectMode) {
                        frame.injectRingLease.Reset();
                    } else {
                        ReleaseQueuedFrameTexture(frame);
                    }
                    frame = QueuedFrame{};
                    frameToProcess = nullptr;
                    popped = false;
                }
            } else if (encodeSucceeded && frameToProcess && !frameToProcess->isInjectMode) {
                releaseWgcLeaseAfterMediaEngineCopy(*frameToProcess, "main duplicate fallback");
            }

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
                        encodeSucceeded = repeatLastFrameForScheduledQpc(scheduledOutputQpc);
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

                if (encodeSucceeded && !isDuplicate && frameToProcess) {
                    if (frameToProcess->timestamp > 0) {
                        lastEmittedInjectSourceQpc = frameToProcess->timestamp;
                    }
                    lastSuccessfullyEncodedInjectLineage = MakeInjectFrameLineage(*frameToProcess);
                }

                if (g_pSharedMem) {
                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    if (isLivePhase) {
                        g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    } else if (isDrainPhase) {
                        g_pSharedMem->runtimeState.drainFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                if (encodeSucceeded && frameToProcess) {
                    frameToProcess->injectRingLease.Reset();
                }
            } else {
                cadenceCounters.consecutiveDeferredFrames = 0;
                if (encodeSucceeded && g_pSharedMem) {
                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    if (isLivePhase) {
                        g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    } else if (isDrainPhase) {
                        g_pSharedMem->runtimeState.drainFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }

            if (encodeSucceeded && !isDuplicate && frameToProcess && !frameToProcess->isInjectMode) {
                if (frameToProcess->timestamp > 0) {
                    lastEmittedWgcSourceQpc = frameToProcess->timestamp;
                }
                if (GetFrameSelectionTimestamp(*frameToProcess) > 0) {
                    lastEmittedWgcSelectionQpc = GetFrameSelectionTimestamp(*frameToProcess);
                }
                lastSuccessfulWgcCursorEmbedded = frameToProcess->wgcCursorEmbedded;
                hasSuccessfulWgcCursorMetadata = true;
            }

            if (encodeSucceeded && !isDuplicate && frameToProcess) {
                cadenceCounters.frameAgeAccumUs += frameAgeUs;
                cadenceCounters.frameAgeSamples++;
                cadenceCounters.frameAgeMaxUs = std::max(cadenceCounters.frameAgeMaxUs, SaturatingToUint32(frameAgeUs));
            }

            if (encodeSucceeded) {
                if (selectionMetricTargetQpc > 0 && frameToProcess && !frameToProcess->isInjectMode && !isDuplicate &&
                    wgcSelectionDelayAppliedThisTick && scheduledLiveCfrTick && !wgcDelayRealizationRecordedThisTick) {
                    recordWgcDelayRealization(signedSelectionErrorUs, signedRawSelectionErrorUs);
                }

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
                        recoveredFreshEncodeFailure && frameToProcess && frameToProcess->isInjectMode
                            ? lastSuccessfullyEncodedInjectLineage
                            : (frameToProcess ? MakeInjectFrameLineage(*frameToProcess) : InjectFrameLineage{});
                    recordDuplicate(recoveredFreshEncodeFailure ? nullptr : frameToProcess,
                                    duplicateLineage.IsValid() ? &duplicateLineage : nullptr, duplicateFromDrain,
                                    duplicateFromDeferred, duplicateFromTimerRebase);
                } else {
                    cadenceCounters.consecutiveDuplicateFrames = 0;
                    captureSessionSummary.currentContiguousDupTicks = 0;
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
                        ce::capture_policy::CfrTimelineStartContract committedStartContract{};
                        const bool canCommitTransactionalWgcStart = useScreenGrab && !recoveredFreshEncodeFailure &&
                                                                    frameToProcess && !frameToProcess->isInjectMode &&
                                                                    pendingWgcStartContract.valid;
                        if (canCommitTransactionalWgcStart) {
                            committedStartContract = ce::capture_policy::RebaseCfrTimelineStartContract(
                                pendingWgcStartContract, frameToProcess->timestamp);
                        }
                        if (committedStartContract.valid) {
                            liveStartQpc.QuadPart = committedStartContract.liveQpc;
                            committedWgcStartContractGeneration = pendingWgcStartContractGeneration;
                            const int64_t selectionOriginQpc = GetFrameSelectionTimestamp(*frameToProcess);
                            const int64_t selectionOffsetUs =
                                qpcToUs(selectionOriginQpc - committedStartContract.videoOriginQpc);
                            const int64_t commitLatenessUs = qpcToUs(afterInit.QuadPart - liveStartQpc.QuadPart);
                            LogInfo(
                                "[EncoderThread] WGC CFR start contract committed after first successful encode: "
                                "generation=%llu videoQpc=%lld selectionQpc=%lld selectionOffsetUs=%lld "
                                "liveQpc=%lld contentDelayUs=%lld commitLatenessUs=%lld prewarm=%s/%lldus",
                                static_cast<unsigned long long>(committedWgcStartContractGeneration),
                                static_cast<long long>(committedStartContract.videoOriginQpc),
                                static_cast<long long>(selectionOriginQpc), static_cast<long long>(selectionOffsetUs),
                                static_cast<long long>(liveStartQpc.QuadPart),
                                static_cast<long long>(qpcToUs(committedStartContract.contentDelayQpc)),
                                static_cast<long long>(commitLatenessUs), wgcEncoderPrewarmSucceeded ? "ok" : "failed",
                                static_cast<long long>(wgcEncoderPrewarmElapsedUs));
                        } else {
                            liveStartQpc = afterInit;
                            if (useScreenGrab) {
                                LogWarn(
                                    "[EncoderThread] ERROR: WGC first frame encoded without a valid transactional "
                                    "start contract: pendingGeneration=%llu pendingValid=%d recoveredFailure=%d "
                                    "frame=%d; using encode-completion wall anchor",
                                    static_cast<unsigned long long>(pendingWgcStartContractGeneration),
                                    pendingWgcStartContract.valid ? 1 : 0, recoveredFreshEncodeFailure ? 1 : 0,
                                    frameToProcess && !frameToProcess->isInjectMode ? 1 : 0);
                            }
                        }
                        // Set warmup window: give the capture system 200ms to accumulate a small buffer
                        // before making policy decisions. Prevents early startup starvation (slow WGC
                        // callback delivery) from permanently poisoning the entire session.
                        wgcWarmupUntilQpc = afterInit.QuadPart + targetIntervalTicks * 24;
                        // Publish the shared startup anchor whenever an effective video delay exists -- the
                        // audio-latency delay OR a realized smoothness floor (video-only / low-confidence
                        // path). The audio anchor delay stays = avContentDelayQpc (true latency, 0 for the
                        // floor case): the extra smoothness/floor delay S is absorbed purely by the later
                        // live-start (scheduleOffset), so audio stays byte-exact and the floor is
                        // sync-neutral by construction (no ghost-image judder).
                        if (useScreenGrab && isWgcEffectiveContentDelayActive()) {
                            if (committedStartContract.valid) {
                                const int64_t startupVideoQpc = committedStartContract.videoOriginQpc;
                                const int64_t startupEffectiveDelayQpc = committedStartContract.contentDelayQpc;
                                const int64_t startupAudioAnchorQpc = committedStartContract.audioAnchorQpc;
                                const int64_t startupAudioAnchorDelayQpc =
                                    committedStartContract.renderLoopbackLatencyQpc;
                                wgcSmoothnessActiveDelayQpc = committedStartContract.smoothnessReserveQpc;
                                wgcAvSyncStartupVideoQpc = startupVideoQpc;
                                wgcAvSyncStartupAudioAnchorQpc = startupAudioAnchorQpc;
                                wgcAvSyncStartupEffectiveDelayQpc = startupEffectiveDelayQpc;
                                wgcAvSyncScheduleOffsetQpc = liveStartQpc.QuadPart - startupAudioAnchorQpc;
                                const int64_t requestedDelayUs =
                                    qpcFreq.QuadPart > 0 ? (startupEffectiveDelayQpc * 1000000) / qpcFreq.QuadPart : 0;
                                const int64_t audioAnchorDelayUs =
                                    qpcFreq.QuadPart > 0 ? (startupAudioAnchorDelayQpc * 1000000) / qpcFreq.QuadPart
                                                         : 0;
                                const int64_t renderDelayUs =
                                    qpcFreq.QuadPart > 0 ? (avContentDelayQpc * 1000000) / qpcFreq.QuadPart : 0;
                                const int64_t smoothExtraDelayUs =
                                    qpcFreq.QuadPart > 0 ? (wgcSmoothnessActiveDelayQpc * 1000000) / qpcFreq.QuadPart
                                                         : 0;
                                const int64_t startupDelayUs = requestedDelayUs;
                                const int64_t scheduleOffsetUs =
                                    qpcFreq.QuadPart > 0 ? (wgcAvSyncScheduleOffsetQpc * 1000000) / qpcFreq.QuadPart
                                                         : 0;
                                LogInfo(
                                    "[AVSyncApply] wgc_cfr_start_contract: generation=%llu videoQpc=%lld "
                                    "audioAnchorQpc=%lld "
                                    "liveStartQpc=%lld requestedDelayUs=%lld startupDelayUs=%lld "
                                    "scheduleOffsetUs=%lld selectionOffsetUs=%lld audioAnchorDelayUs=%lld "
                                    "renderDelayUs=%lld "
                                    "smoothExtraDelayUs=%lld confidence=%s reason=%s",
                                    static_cast<unsigned long long>(committedWgcStartContractGeneration),
                                    static_cast<long long>(startupVideoQpc),
                                    static_cast<long long>(startupAudioAnchorQpc),
                                    static_cast<long long>(liveStartQpc.QuadPart),
                                    static_cast<long long>(requestedDelayUs), static_cast<long long>(startupDelayUs),
                                    static_cast<long long>(scheduleOffsetUs),
                                    static_cast<long long>(
                                        qpcToUs(GetFrameSelectionTimestamp(*frameToProcess) - startupVideoQpc)),
                                    static_cast<long long>(audioAnchorDelayUs), static_cast<long long>(renderDelayUs),
                                    static_cast<long long>(smoothExtraDelayUs), config.avSyncConfidence.c_str(),
                                    config.avSyncReason.c_str());
                            } else {
                                LogInfo(
                                    "[AVSyncApply] ERROR: invalid WGC CFR start contract: videoQpc=%lld "
                                    "liveStartQpc=%lld renderDelayUs=%lld observedContentDelayUs=%lld; "
                                    "startup audio anchor not published",
                                    static_cast<long long>(frameToProcess ? frameToProcess->timestamp : 0),
                                    static_cast<long long>(liveStartQpc.QuadPart),
                                    static_cast<long long>(qpcToUs(avContentDelayQpc)),
                                    static_cast<long long>(qpcToUs(liveStartQpc.QuadPart -
                                                                   (frameToProcess ? frameToProcess->timestamp : 0))));
                            }
                        }
                        pendingWgcStartContract = {};
                        // For the selection grid, we treat the first frame as tick 1.
                        // To align future idealQpc calculations perfectly with scheduledSampleQpc,
                        // we must offset the anchor back by one target interval.
                        encoderGridStartQpc = liveStartQpc.QuadPart - targetIntervalTicks;
                        // Continue from the immutable contract grid. Deferred initialization time
                        // is commit-lateness telemetry and never changes the selected content delay.
                        nextSampleTime.QuadPart = liveStartQpc.QuadPart + targetIntervalTicks;
                        LogInfo("[EncoderThread] Anchored CFR live timeline after first frame (contract grid kept)");
                    }
                    ++liveTicksOutput;
                }
                const InjectFrameLineage catchupLineage =
                    recoveredFreshEncodeFailure && frameToProcess && frameToProcess->isInjectMode
                        ? lastSuccessfullyEncodedInjectLineage
                        : (frameToProcess ? MakeInjectFrameLineage(*frameToProcess) : InjectFrameLineage{});
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
            if (!activeScreenGrab && liveTicksOutput >= cycleLiveTicksOutputStart) {
                const uint64_t outputTicksThisCycle64 = liveTicksOutput - cycleLiveTicksOutputStart;
                const uint32_t outputTicksThisCycle = SaturatingToUint32(outputTicksThisCycle64);
                const double injectServiceMs =
                    ce::capture_policy::GetInjectCfrServiceMsPerOutputTick(cycleMs, outputTicksThisCycle);
                if (injectServiceMs > 0.0) {
                    if (smoothedInjectServiceMs < 0.001) {
                        smoothedInjectServiceMs = injectServiceMs;
                    } else {
                        smoothedInjectServiceMs = smoothedInjectServiceMs * 0.85 + injectServiceMs * 0.15;
                    }
                    injectServiceMaxUs = std::max(
                        injectServiceMaxUs, SaturatingToUint32(static_cast<uint64_t>(injectServiceMs * 1000.0)));
                }
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
            const uint32_t delayReservoirLowWaterFrames = getWgcDelayReservoirLowWaterFrames();
            const uint32_t delayReservoirTargetFrames = getWgcDelayReservoirTargetFrames();
            const uint32_t delayResidualAvgUs =
                wgcDelayResidualSamples > 0 ? SaturatingToUint32(wgcDelayResidualAbsAccumUs / wgcDelayResidualSamples)
                                            : 0u;
            const uint32_t delayResidualP95Us = wgcDelayResidualP95Us();
            const int32_t delayResidualSignedAvgUs =
                wgcDelayResidualSamples > 0 ? static_cast<int32_t>(wgcDelayResidualSignedAccumUs /
                                                                   static_cast<int64_t>(wgcDelayResidualSamples))
                                            : 0;
            const uint32_t delayResidualWindowAvgUs =
                wgcDelayResidualWindowSamples > 0
                    ? SaturatingToUint32(wgcDelayResidualWindowAbsAccumUs / wgcDelayResidualWindowSamples)
                    : 0u;
            const uint32_t delayResidualWindowP95Us = wgcDelayResidualWindowP95Us();
            const int32_t delayResidualWindowSignedAvgUs =
                wgcDelayResidualWindowSamples > 0
                    ? static_cast<int32_t>(wgcDelayResidualWindowSignedAccumUs /
                                           static_cast<int64_t>(wgcDelayResidualWindowSamples))
                    : 0;
            const uint32_t rawResidualAvgUs =
                wgcDelayRawResidualSamples > 0
                    ? SaturatingToUint32(wgcDelayRawResidualAbsAccumUs / wgcDelayRawResidualSamples)
                    : 0u;
            const int32_t rawResidualSignedAvgUs =
                wgcDelayRawResidualSamples > 0 ? static_cast<int32_t>(wgcDelayRawResidualSignedAccumUs /
                                                                      static_cast<int64_t>(wgcDelayRawResidualSamples))
                                               : 0;
            const uint32_t rawResidualWindowAvgUs =
                wgcDelayRawResidualWindowSamples > 0
                    ? SaturatingToUint32(wgcDelayRawResidualWindowAbsAccumUs / wgcDelayRawResidualWindowSamples)
                    : 0u;
            const int32_t rawResidualWindowSignedAvgUs =
                wgcDelayRawResidualWindowSamples > 0
                    ? static_cast<int32_t>(wgcDelayRawResidualWindowSignedAccumUs /
                                           static_cast<int64_t>(wgcDelayRawResidualWindowSamples))
                    : 0;
            const uint32_t rawResidualP95Us = wgcDelayRawResidualP95Us();
            const uint32_t rawResidualWindowP95Us = wgcDelayRawResidualWindowP95Us();
            const int32_t rawMinusPredictedAvgUs =
                wgcDelayRawMinusPredictedSamples > 0
                    ? static_cast<int32_t>(wgcDelayRawMinusPredictedSignedAccumUs /
                                           static_cast<int64_t>(wgcDelayRawMinusPredictedSamples))
                    : 0;
            const int32_t rawMinusPredictedWindowAvgUs =
                wgcDelayRawMinusPredictedWindowSamples > 0
                    ? static_cast<int32_t>(wgcDelayRawMinusPredictedWindowSignedAccumUs /
                                           static_cast<int64_t>(wgcDelayRawMinusPredictedWindowSamples))
                    : 0;
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
            const uint32_t dupTsPerSec = dupTimestampCount;
            const uint32_t currentWgcDuplicateTimestampSkipCount =
                g_WgcCap ? g_WgcCap->GetDuplicateTimestampSkipCount() : lastWgcDuplicateTimestampSkipCountForCadence;
            const uint32_t dupTsSkippedPerSec =
                currentWgcDuplicateTimestampSkipCount >= lastWgcDuplicateTimestampSkipCountForCadence
                    ? currentWgcDuplicateTimestampSkipCount - lastWgcDuplicateTimestampSkipCountForCadence
                    : currentWgcDuplicateTimestampSkipCount;
            lastWgcDuplicateTimestampSkipCountForCadence = currentWgcDuplicateTimestampSkipCount;
            dupTimestampCount = 0;
            encCycleMaxMs = 0;

            accumulateCaptureSummarySample(useScreenGrab, srcFpsX100Val, srcJitterUsVal, dupNoSource, dupDeferred,
                                           dupTimer, dupDrain, oldestBufferedFrameAgeUs, shortfallDurationMs,
                                           sustainableOutputFps);

            if (useScreenGrab && recordingOutputLive && g_WgcCap) {
                const uint32_t currentIngressAccepted = g_WgcCap->GetIngressAcceptedCount();
                if (!wgcRollingSourceWindowPrimed || currentIngressAccepted < wgcRollingSourceLastIngressAccepted) {
                    wgcRollingSourceLastIngressAccepted = currentIngressAccepted;
                    wgcRollingSourceAcceptedSlots.fill(0);
                    wgcRollingSourceCfrTickSlots.fill(0);
                    wgcRollingSourceSlotIndex = 0;
                    wgcRollingSourceSlotCount = 0;
                    wgcRollingSourceAcceptedSum = 0;
                    wgcRollingSourceCfrTickSum = 0;
                    wgcRollingSourceWindowPrimed = true;
                }
                wgcRollingSourceAcceptedWindow = currentIngressAccepted - wgcRollingSourceLastIngressAccepted;
                wgcRollingSourceLastIngressAccepted = currentIngressAccepted;
                wgcRollingSourceCfrTicksWindow = cadenceCounters.liveTickEmitCount;

                wgcRollingSourceAcceptedSum -= wgcRollingSourceAcceptedSlots[wgcRollingSourceSlotIndex];
                wgcRollingSourceCfrTickSum -= wgcRollingSourceCfrTickSlots[wgcRollingSourceSlotIndex];
                wgcRollingSourceAcceptedSlots[wgcRollingSourceSlotIndex] = wgcRollingSourceAcceptedWindow;
                wgcRollingSourceCfrTickSlots[wgcRollingSourceSlotIndex] = wgcRollingSourceCfrTicksWindow;
                wgcRollingSourceAcceptedSum += wgcRollingSourceAcceptedWindow;
                wgcRollingSourceCfrTickSum += wgcRollingSourceCfrTicksWindow;
                wgcRollingSourceSlotIndex = (wgcRollingSourceSlotIndex + 1u) % kWgcRollingSourceWindowSlots;
                wgcRollingSourceSlotCount = std::min(wgcRollingSourceSlotCount + 1u, kWgcRollingSourceWindowSlots);
                wgcRollingSourceAcceptedTotal += wgcRollingSourceAcceptedWindow;
                wgcRollingSourceCfrTickTotal += wgcRollingSourceCfrTicksWindow;
                wgcRollingSourceDeficitFrames = wgcRollingSourceCfrTickSum > wgcRollingSourceAcceptedSum
                                                    ? (wgcRollingSourceCfrTickSum - wgcRollingSourceAcceptedSum)
                                                    : 0u;
                wgcRollingSourceSurplusFrames = wgcRollingSourceAcceptedSum > wgcRollingSourceCfrTickSum
                                                    ? (wgcRollingSourceAcceptedSum - wgcRollingSourceCfrTickSum)
                                                    : 0u;
            } else {
                wgcRollingSourceWindowPrimed = false;
                wgcRollingSourceAcceptedWindow = 0;
                wgcRollingSourceCfrTicksWindow = 0;
                wgcRollingSourceDeficitFrames = 0;
                wgcRollingSourceSurplusFrames = 0;
                wgcRollingSourceAcceptedSum = 0;
                wgcRollingSourceCfrTickSum = 0;
                wgcRollingSourceSlotIndex = 0;
                wgcRollingSourceSlotCount = 0;
                wgcRollingSourceAcceptedSlots.fill(0);
                wgcRollingSourceCfrTickSlots.fill(0);
            }

            LogInfo(
                "[Cadence Health] Phase=%s | AgeAvg=%uus AgeMax=%uus | SelAvg=%uus SelMax=%uus SelBias=%dus "
                "EarlyMax=%uus LateMax=%uus | WgcSelAvg=%uus WgcSelMax=%uus WgcSelBias=%dus WgcEarly=%uus WgcLate=%uus "
                "Hold=%u HoldFresh=%u Delay=%u Spend=%u CatchUp=%u CatchFresh=%u InjectCatch=%u/%u "
                "InjectAgeTrim=%u PathMismatch=%u/%llu LiveClamp=%u/%uus | DefStreak=%u/%u "
                "DupStreak=%u/%u | DupSrc=%u "
                "DupDef=%u "
                "DupTimer=%u DupDrain=%u InjectDefReQ=%u InjectDefDrop=%u | TickEmit=%u TickUnique=%u TickDup=%u "
                "TickMiss=%u SourceWin=%u/%u SourceRoll=%u/%u SourceDef=%u SourceSur=%u | "
                "HoldHist=%u/%u/%u/%u/%u/%u | LiveWall=%lluus LiveTicks=%llu Shortfall=%u/%.1fms FreshMiss=%upm "
                "BufAvg=%upm BufMin=%u BufNow=%zu NoFresh=%u NoReserve=%u DelayRes=%u/%u LowTicks=%u "
                "DelayResidualAvg=%d/%uus DelayResidualMax=%uus DelayResidualP95=%uus DelayResidualLateMax=%uus "
                "DelayResidualWin=%d/%uus/%uus/%uus "
                "RawResidualAvg=%d/%uus RawResidualMax=%uus RawResidualP95=%uus RawResidualLateMax=%uus "
                "RawResidualWin=%d/%uus/%uus/%uus RawMinusPred=%dus/%uus RawMinusPredWin=%dus/%uus "
                "Oldest=%.1fms LeadExcess=%.1fms | "
                "WgcAct Fresh=%u "
                "DupSrc=%u DropObs=%u "
                "DropDebt=%u/%llu DebtMax=%uus SelMiss=%u StaleUni=%u "
                "Ancient=%u RepFreshMiss=%u RepHold=%u SyncHold=%u SyncHoldSrc=%u SyncHoldPolicy=%u "
                "TooNewLead=%uus RepCov=%u CovDelay=%u "
                "RepLate=%u RepCatch=%u | TsReg=%u "
                "TsStall=%u "
                "TimerRebase=%u WgcDebtMax=%llu WgcLiveRebase=%u/%llu/%u | "
                "EncLowBypass=%u/%llu ModeMis=%u/%llu SrcBack=%u/%llu | "
                "InvalidMeta=%u InvalidHandle=%u | PktClamp=%u NegPTS=%u NonMonoPTS=%u | WgcThr=%u Adj=%u | Over=0x%X "
                "MuxQ=%uKB/%u MuxBp=%u Wait=%uus Max=%uus | EncEma=%.2fms Budget=%upm Sust=%.1ffps TooSlow=%d "
                "Bottleneck=%d | LowSrc=%d Recover=%d Cause=S%d/D%d/E%d | SrcFps=%.2f SrcJitter=%uus "
                "DupTs=%u DupTsSkip=%u TsSmoothDev=%u/%u/%uus TsSmoothSnap=%u EncCycle=%.2fms EncSpike=%u",
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
                cadenceCounters.liveTickDuplicateCount, cadenceCounters.liveTickMissCount,
                wgcRollingSourceAcceptedWindow, wgcRollingSourceCfrTicksWindow, wgcRollingSourceAcceptedSum,
                wgcRollingSourceCfrTickSum, wgcRollingSourceDeficitFrames, wgcRollingSourceSurplusFrames,
                cadenceCounters.holdHist[0], cadenceCounters.holdHist[1], cadenceCounters.holdHist[2],
                cadenceCounters.holdHist[3], cadenceCounters.holdHist[4], cadenceCounters.holdHist[5],
                static_cast<unsigned long long>(liveWallElapsedUs), static_cast<unsigned long long>(liveTicksOutput),
                outputShortfallTicks, shortfallDurationMs, wgcNoFreshTickPermille, bufferedAtTickAvgPermille,
                bufferedAtTickMinValue, bufferedWgcFrames.size(), wgcNoFreshTickCount, wgcNoReserveTickCount,
                delayReservoirLowWaterFrames, delayReservoirTargetFrames, wgcDelayReservoirLowWaterTickCount,
                delayResidualSignedAvgUs, delayResidualAvgUs, wgcDelayResidualAbsMaxUs, delayResidualP95Us,
                wgcDelayResidualLateMaxUs, delayResidualWindowSignedAvgUs, delayResidualWindowAvgUs,
                delayResidualWindowP95Us, wgcDelayResidualWindowLateMaxUs, rawResidualSignedAvgUs, rawResidualAvgUs,
                wgcDelayRawResidualAbsMaxUs, rawResidualP95Us, wgcDelayRawResidualLateMaxUs,
                rawResidualWindowSignedAvgUs, rawResidualWindowAvgUs, rawResidualWindowP95Us,
                wgcDelayRawResidualWindowLateMaxUs, rawMinusPredictedAvgUs, wgcDelayRawMinusPredictedAbsMaxUs,
                rawMinusPredictedWindowAvgUs, wgcDelayRawMinusPredictedWindowAbsMaxUs, oldestBufferedFrameAgeMs,
                wgcAudioLeadExcessMsCurrent, wgcSelectFreshCount, wgcSelectDuplicateSourceCount, wgcDropObsoleteCount,
                wgcDropStaleDebtCount, static_cast<unsigned long long>(wgcDropStaleDebtTotal), wgcDropStaleDebtMaxUs,
                wgcFreshSelectionMissCount, wgcStaleUniqueFallbackCount, wgcAncientSelectionCount,
                wgcRepeatNoFreshCount, wgcRepeatPolicyHoldCount, wgcSyncDelayHoldCount,
                wgcSyncDelaySourceLimitedHoldCount, wgcSyncDelayPolicyHoldCount, wgcTooNewLeadMaxUs,
                wgcCoverageRepeatHoldCount, wgcCoverageDelayTicksCurrent, wgcRepeatTimerLateCount,
                wgcRepeatCatchupCount, tsRegress - lastTimestampRegressionCount, tsStall - lastTimestampStallCount,
                timerRebases, static_cast<unsigned long long>(wgcVisualDebtMaxExcessTicks),
                wgcLiveSchedulerRebaseThisWindow, static_cast<unsigned long long>(wgcLiveSchedulerRebaseTotal),
                wgcLiveSchedulerRebaseMaxTicks, wgcEncoderLimitedSuppressedByLowSourceThisWindow,
                static_cast<unsigned long long>(wgcEncoderLimitedSuppressedByLowSourceTotal),
                wgcCapacityPressureModeMismatchThisWindow,
                static_cast<unsigned long long>(wgcCapacityPressureModeMismatchTotal),
                wgcSelectedSourceBacktrackThisWindow, static_cast<unsigned long long>(wgcSelectedSourceBacktrackTotal),
                invalidMeta - lastInvalidMetaCount, invalidHandle - lastInvalidHandleCount,
                packetClamps - lastPacketClampCount, negativePts - lastNegativePtsCount,
                nonMonotonicPts - lastNonMonotonicPtsCount, g_WgcProducerTargetFps.load(std::memory_order_relaxed),
                wgcProducerRateRetuneCount, overloadFlags, (muxQueueBytes + 1023u) / 1024u, muxQueuePackets,
                muxBackpressureCount, muxBackpressureWaitUs, muxBackpressureMaxWaitUs, smoothedEncodeMs,
                encoderBudgetUtilizationPermille, sustainableOutputFps, encoderTooSlowForTarget ? 1 : 0,
                g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ? 1 : 0, wgcLowSourceModeActive ? 1 : 0,
                wgcLiveRecoveryModeActive ? 1 : 0, wgcSourceStarvedCurrent ? 1 : 0, wgcSchedulerLimitedCurrent ? 1 : 0,
                wgcEncoderRecoveryLimitedCurrent ? 1 : 0, srcFpsX100Val / 100.0, srcJitterUsVal, dupTsPerSec,
                dupTsSkippedPerSec,
                wgcTsSmoothSamplesWindow > 0
                    ? SaturatingToUint32(wgcTsSmoothDevAccumUsWindow / wgcTsSmoothSamplesWindow)
                    : 0u,
                wgcTsSmoothDevMaxUsWindow, wgcTsSmoothDevMaxUsTotal, wgcTsSmoothSnapCountWindow, smoothedEncCycleMs,
                encodeSpikeCountThisSecond);
            wgcTsSmoothSamplesWindow = 0;
            wgcTsSmoothDevAccumUsWindow = 0;
            wgcTsSmoothDevMaxUsWindow = 0;
            wgcTsSmoothSnapCountWindow = 0;

            const bool wgcEncoderLimitedSmoothnessActive = isWgcEncoderLimitedSmoothnessMode();
            if (useScreenGrab && recordingOutputLive &&
                (wgcEncoderLimitedSmoothnessActive || wgcSourceStarvedCurrent || wgcSchedulerLimitedCurrent ||
                 outputShortfallTicks > 0 || wgcRepeatPolicyHoldCount > 0 || wgcDropStaleDebtCount > 0)) {
                ++wgcEncoderLimitedCadenceEventCount;
                const char* cadenceMode = wgcEncoderLimitedSmoothnessActive ? "encoder_limited"
                                          : wgcSourceStarvedCurrent         ? "source_starved"
                                          : wgcSchedulerLimitedCurrent      ? "scheduler_limited"
                                                                            : "normal_pressure";
                const uint32_t wgcSmoothnessRepeatsAvoidedWindow =
                    SaturatingToUint32(static_cast<uint64_t>(wgcDelayOlderFrameAvoidedRepeatWindow) +
                                       static_cast<uint64_t>(wgcDelayRepeatRescueSuccessWindow) +
                                       static_cast<uint64_t>(wgcDelayRepeatPromotedBeforeRepeatWindow));
                const double wgcSmoothnessActiveDelayMs =
                    static_cast<double>(qpcToUs(wgcSmoothnessActiveDelayQpc)) / 1000.0;
                const double wgcSmoothnessEstimatedVramMb =
                    static_cast<double>(wgcSmoothnessEstimatedVramBytes) / (1024.0 * 1024.0);
                const uint32_t wgcPoolFreeNow = g_WgcCap ? g_WgcCap->GetPoolSlotFreeCurrentCount() : 0u;
                const uint32_t wgcPoolFreeMin = g_WgcCap ? g_WgcCap->GetPoolSlotFreeMinCount() : 0u;
                const int64_t wgcWindowSmoothTargetUs =
                    qpcToUs(ce::capture_policy::GetWgcStartupSmoothnessTargetDelayQpc(
                        wgcSmoothnessRetainedFrames, targetIntervalTicks, getWgcSmoothnessOutputFps(),
                        config.wgcSmoothnessBufferMaxMs));
                const int64_t wgcWindowSmoothActualUs = qpcToUs(wgcSmoothnessActiveDelayQpc);
                const int64_t wgcWindowSmoothDeficitUs =
                    std::max<int64_t>(0, wgcWindowSmoothTargetUs - wgcWindowSmoothActualUs);
                const int64_t wgcWindowEffectiveDelayUs = qpcToUs(getWgcEffectiveContentDelayQpc());
                const int64_t wgcWindowStartupDeficitUs =
                    std::max<int64_t>(0, wgcStartupDelayTargetUs - wgcWindowEffectiveDelayUs);
                LogInfo(
                    "[WGC CFR CADENCE EVENT] mode=%s shortfall=%u/%.1fms phaseErrorAvg=%dus "
                    "phaseErrorMax=%uus rebaseWindow=%u encoderDropWindow=%u encoderDropTotal=%llu "
                    "tooNewRepeat=%u syncDelayHold=%u syncDelaySourceHold=%u syncDelayPolicyHold=%u "
                    "tooNewLeadMax=%uus staleDrop=%u freshMiss=%upm bufNow=%zu oldest=%.1fms enc=%.2fms "
                    "sustain=%.1ffps overload=0x%X lowSourceBypass=%u modeMismatch=%u sourceBacktrack=%u "
                    "avDelay=%.1fms delayResidualAvg=%d/%uus delayResidualMax=%uus delayResidualP95=%uus "
                    "delayResidualLateMax=%uus delayResidualWin=%d/%uus delayResidualWinP95=%uus "
                    "rawResidualAvg=%d/%uus rawResidualMax=%uus rawResidualP95=%uus rawResidualLateMax=%uus "
                    "rawResidualWin=%d/%uus rawResidualWinP95=%uus rawMinusPredicted=%dus/%uus "
                    "postRejectSync=%u postRejectRescue=%u lowerBoundRepeat=%u excessRepeat=%u "
                    "policyAddedRepeat=%u excessRepeatCluster=%u/%u "
                    "sourceWindow=%u/%u sourceRolling=%u/%u sourceDeficit=%u sourceSurplus=%u "
                    "smoothBufMs=%.1f smoothFrames=%u/%u/%u "
                    "smoothSlots=%u retainedCap=%u reservedFreeSlots=%u retainedCapTrim=%u "
                    "poolFreeNow=%u poolFreeMin=%u poolPressureTrim=%u "
                    "smoothDeficit=%lldus startupDeficit=%lldus "
                    "smoothVramMB=%.1f smoothCap=%d smoothReason=%s "
                    "repeatsAvoided=%u repeatsUnavoidable=%u "
                    "reservoir=%u/%u lowTicks=%u "
                    "delayRelaxed=%u delayRelaxedRejectSync=%u repeatClusterPressure=%u/%u "
                    "delayRelaxedBetter=%u delayRelaxedCluster=%u delayRelaxedRejectHeadroom=%u "
                    "delayRelaxedRejectCost=%u softLateReject=%u softLateAccept=%u olderFrame=%u "
                    "sourceLimitRepeat=%u repeatRescue=%u/%u repeatPromote=%u/%u repeatPromoteSoft=%u "
                    "repeatSafeAfter=%u repeatSafe=%u/%u repeatSoftSafe=%u/%u repeatClass=%u/%u/%u "
                    "repeatReserve=%u/%uus hardOnly=%u syncProtected=%u nearCap=%u oldestSoftSafe=%uus "
                    "uniformCadence=%u uniformHold=%u delayPaceCapTrim=%u sourceRecovery=%u/%llu "
                    "cause=S%d/D%d/E%d",
                    cadenceMode, outputShortfallTicks, shortfallDurationMs, avgSignedWgcSelectionErrorUs,
                    wgcSelectionErrorMaxUs, wgcLiveSchedulerRebaseThisWindow, wgcEncoderLimitedSourceDropThisWindow,
                    static_cast<unsigned long long>(wgcEncoderLimitedSourceDropTotal), wgcRepeatPolicyHoldCount,
                    wgcSyncDelayHoldCount, wgcSyncDelaySourceLimitedHoldCount, wgcSyncDelayPolicyHoldCount,
                    wgcTooNewLeadMaxUs, wgcDropStaleDebtCount, wgcNoFreshTickPermille, bufferedWgcFrames.size(),
                    oldestBufferedFrameAgeMs, smoothedEncodeMs, sustainableOutputFps, overloadFlags,
                    wgcEncoderLimitedSuppressedByLowSourceThisWindow, wgcCapacityPressureModeMismatchThisWindow,
                    wgcSelectedSourceBacktrackThisWindow,
                    static_cast<double>(qpcToUs(getWgcEffectiveContentDelayQpc())) / 1000.0, delayResidualSignedAvgUs,
                    delayResidualAvgUs, wgcDelayResidualAbsMaxUs, delayResidualP95Us, wgcDelayResidualLateMaxUs,
                    delayResidualWindowSignedAvgUs, delayResidualWindowAvgUs, delayResidualWindowP95Us,
                    rawResidualSignedAvgUs, rawResidualAvgUs, wgcDelayRawResidualAbsMaxUs, rawResidualP95Us,
                    wgcDelayRawResidualLateMaxUs, rawResidualWindowSignedAvgUs, rawResidualWindowAvgUs,
                    rawResidualWindowP95Us, rawMinusPredictedAvgUs, wgcDelayRawMinusPredictedAbsMaxUs,
                    wgcDelayPostSelectionRejectedSyncRiskWindow, wgcDelayPostSelectionRescuedSyncRiskWindow,
                    wgcSourceRepeatLowerBoundWindow, wgcExcessRepeatWindow, wgcPolicyAddedRepeatWindow,
                    wgcExcessRepeatClusterWindow, wgcExcessRepeatClusterWindowMaxTicks, wgcRollingSourceAcceptedWindow,
                    wgcRollingSourceCfrTicksWindow, wgcRollingSourceAcceptedSum, wgcRollingSourceCfrTickSum,
                    wgcRollingSourceDeficitFrames, wgcRollingSourceSurplusFrames, wgcSmoothnessActiveDelayMs,
                    wgcSmoothnessActualFrames, wgcSmoothnessRetainedFrames, wgcSmoothnessDesiredFrames,
                    wgcSmoothnessPoolSlots, wgcSmoothnessRetainedFrameCap, wgcSmoothnessReservedFreeSlots,
                    wgcRetainedCapTrimWindow, wgcPoolFreeNow, wgcPoolFreeMin, wgcPoolPressureTrimWindow,
                    static_cast<long long>(wgcWindowSmoothDeficitUs), static_cast<long long>(wgcWindowStartupDeficitUs),
                    wgcSmoothnessEstimatedVramMb, wgcSmoothnessCapLimited ? 1 : 0, getWgcSmoothnessBufferReason(),
                    wgcSmoothnessRepeatsAvoidedWindow, wgcSourceRepeatLowerBoundWindow, delayReservoirLowWaterFrames,
                    delayReservoirTargetFrames, wgcDelayReservoirLowWaterTickCount, wgcDelayRelaxedSelectionWindowCount,
                    wgcDelayRelaxedRejectedSyncRiskWindow, wgcDelayRepeatClusterPressureWindow,
                    wgcDelayRepeatClusterPressureWindowMaxTicks, wgcDelayRelaxedBetterTargetWindow,
                    wgcDelayRelaxedRepeatClusterWindow, wgcDelayRelaxedRejectedResidualHeadroomWindow,
                    wgcDelayRelaxedRejectedRepeatCostWindow, wgcDelaySoftLateRejectedWindow,
                    wgcDelaySoftLateAcceptedWindow, wgcDelayOlderFrameAvoidedRepeatWindow,
                    wgcDelaySourceLimitedRepeatWindow, wgcDelayRepeatRescueSuccessWindow,
                    wgcDelayRepeatRescueAttemptWindow, wgcDelayRepeatPromotedBeforeRepeatWindow,
                    wgcDelayRepeatPromotionAttemptWindow, wgcDelayRepeatPromotionRejectedSoftWindow,
                    wgcDelayRepeatSafeAfterPromotionWindow, wgcDelayRepeatWithSafeCandidateWindow,
                    wgcDelayRepeatWithoutSafeCandidateWindow, wgcDelayRepeatWithSoftSafeCandidateWindow,
                    wgcDelayRepeatWithoutSoftSafeCandidateWindow, wgcDelayWindowHealthyRepeatWindow,
                    wgcDelayWindowRecoverableRepeatWindow, wgcDelayWindowSourceLimitedRepeatWindow,
                    wgcDelayRepeatReserveDepthWindowMax, wgcDelayRepeatReserveSpanWindowMaxUs,
                    wgcDelayRepeatHardOnlyCandidateWindow, wgcDelaySyncProtectedRepeatWindow,
                    wgcDelayNearCapAcceptedWindow, wgcDelayOldestSoftSafeAgeWindowMaxUs, wgcDelayUniformCadenceWindow,
                    wgcDelayUniformHoldWindow, wgcDelayPaceCapTrimWindow, wgcSyncDelaySourceRecoveryHoldCount,
                    static_cast<unsigned long long>(wgcSyncDelaySourceRecoveryHoldTotal),
                    wgcSourceStarvedCurrent ? 1 : 0, wgcSchedulerLimitedCurrent ? 1 : 0,
                    wgcEncoderRecoveryLimitedCurrent ? 1 : 0);
                // Compact per-window jitter-budget view: is the (audio-latency OR floor) buffer absorbing
                // bursty WGC delivery, or is jitter overflowing it into even repeats? bufNow staying at/above
                // the reservoir low-water with bounded windowResidualLate means absorbed; bufNow draining to
                // 0 with uniformHold repeats means the delivery burst exceeded the buffer depth (overflow ->
                // even repeats, NOT a sync fault). Source-limited repeats are attributed separately.
                if (isWgcEffectiveContentDelayActive()) {
                    const int64_t jbDeliveryGapAvgUs = g_WgcCap ? g_WgcCap->GetCallbackGapAvgUs() : 0;
                    const int64_t jbDeliveryGapMaxUs = g_WgcCap ? g_WgcCap->GetCallbackGapMaxUs() : 0;
                    const int64_t jbSourceJitterAvgUs = g_WgcCap ? g_WgcCap->GetSourceJitterAvgUs() : 0;
                    const int64_t jbSourceJitterMaxUs = g_WgcCap ? g_WgcCap->GetSourceJitterMaxUs() : 0;
                    const bool jbAbsorbing = bufferedWgcFrames.size() >= delayReservoirLowWaterFrames;
                    LogInfo(
                        "[WGC CFR JITTER BUDGET] floorSource=%s effectiveDelayUs=%lld floorTargetUs=%lld "
                        "bufNow=%zu reservoir=%u/%u deliveryGapUs(avg/max)=%lld/%lld "
                        "sourceJitterUs(avg/max)=%lld/%lld windowResidualLateMaxUs=%u windowResidualP95Us=%u "
                        "uniformHoldRepeats=%u sourceLimitedRepeats=%u paceCapTrim=%u absorbing=%d",
                        avContentDelayActive ? "audio" : wgcSmoothnessFloorSource,
                        static_cast<long long>(wgcWindowEffectiveDelayUs),
                        static_cast<long long>(qpcToUs(wgcSmoothnessFloorDelayQpc)), bufferedWgcFrames.size(),
                        delayReservoirLowWaterFrames, delayReservoirTargetFrames,
                        static_cast<long long>(jbDeliveryGapAvgUs), static_cast<long long>(jbDeliveryGapMaxUs),
                        static_cast<long long>(jbSourceJitterAvgUs), static_cast<long long>(jbSourceJitterMaxUs),
                        wgcDelayResidualLateMaxUs, delayResidualWindowP95Us, wgcDelayUniformHoldWindow,
                        wgcDelaySourceLimitedRepeatWindow, wgcDelayPaceCapTrimWindow, jbAbsorbing ? 1 : 0);
                }
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
                const bool transientStartupEncoderPressure =
                    !muxPressure &&
                    ce::capture_policy::IsEncoderStartupWindow(recordingOutputLive, recordingLiveTick, nowTick) &&
                    s_wgcCapacityLimitedStreakSeconds < 2;
                if (hardCapacityPressure && !transientStartupEncoderPressure &&
                    (nowTick - s_lastWgcCapacityWarnTick) >= 5000) {
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
                    const int64_t wgcInfoCallbackGapAvgUs = g_WgcCap ? g_WgcCap->GetCallbackGapAvgUs() : 0;
                    const int64_t wgcInfoCallbackGapMaxUs = g_WgcCap ? g_WgcCap->GetCallbackGapMaxUs() : 0;
                    const int64_t wgcInfoSourceJitterAvgUs = g_WgcCap ? g_WgcCap->GetSourceJitterAvgUs() : 0;
                    const int64_t wgcInfoSourceJitterMaxUs = g_WgcCap ? g_WgcCap->GetSourceJitterMaxUs() : 0;
                    const uint32_t wgcInfoPoolFreeMin = g_WgcCap ? g_WgcCap->GetPoolSlotFreeMinCount() : 0u;
                    const uint32_t wgcInfoPoolSaturatedDrops = g_WgcCap ? g_WgcCap->GetPoolSaturatedDropCount() : 0u;
                    const uint32_t wgcInfoIngressHard = g_WgcCap ? g_WgcCap->GetIngressHardReservePressureCount() : 0u;
                    const uint32_t wgcInfoIngressSoft = g_WgcCap ? g_WgcCap->GetIngressSoftReservePressureCount() : 0u;
                    const uint32_t wgcInfoIngressDecimated = g_WgcCap ? g_WgcCap->GetIngressDecimatedCount() : 0u;
                    const bool wgcInfoPoolPressure = wgcInfoPoolSaturatedDrops > 0 || wgcInfoIngressHard > 0 ||
                                                     wgcInfoIngressDecimated > 0 || wgcInfoPoolFreeMin == 0;
                    const bool wgcInfoCleanCe = !encoderPressure && !muxPressure && !wgcInfoPoolPressure &&
                                                wgcExcessRepeatWindow == 0 && wgcPolicyAddedRepeatWindow == 0 &&
                                                wgcDelayPostSelectionRejectedSyncRiskWindow == 0;
                    const char* wgcInfoCoverageReason =
                        wgcSchedulerLimitedCurrent ? "wgc_delivery_gap"
                        : wgcSourceStarvedCurrent  ? "source_or_delivery_underfeed"
                        : wgcRecentDeliveredMin250Fps + ce::capture_policy::kWgcRecoverySourceMarginFps < outputFps
                            ? "delivered_below_cfr_target"
                            : "no_fresh_source_for_cfr_slots";
                    LogInfo(
                        "[WGC CFR] CFR source-coverage repeats: reason=%s target=%ufps input=%u/%u "
                        "delivered=%u/%u freshMiss=%upm buffered=%u oldest=%.1fms shortfall=%u/%.1fms "
                        "duplicates=%u lowerBound=%u excess=%u policyAdded=%u cleanCE=%d cause=S%d/D%d/E%d "
                        "encoderPressure=%d muxPressure=%d poolPressure=%d poolFreeMin=%u poolSat=%u "
                        "ingressHard=%u ingressSoft=%u ingressDec=%u callbackGapUs(avg/max)=%lld/%lld "
                        "sourceJitterUs(avg/max)=%lld/%lld overlayEncoderWarn=%d "
                        "note=cfr_repeats_mean_no_sync_safe_fresh_source_for_some_slots",
                        wgcInfoCoverageReason, outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps,
                        wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps, wgcNoFreshTickPermille,
                        bufferedAtTickMinValue, oldestBufferedFrameAgeMs, outputShortfallTicks, shortfallDurationMs,
                        cadenceCounters.liveTickDuplicateCount, wgcSourceRepeatLowerBoundWindow, wgcExcessRepeatWindow,
                        wgcPolicyAddedRepeatWindow, wgcInfoCleanCe ? 1 : 0, wgcSourceStarvedCurrent ? 1 : 0,
                        wgcSchedulerLimitedCurrent ? 1 : 0, wgcEncoderRecoveryLimitedCurrent ? 1 : 0,
                        encoderPressure ? 1 : 0, muxPressure ? 1 : 0, wgcInfoPoolPressure ? 1 : 0, wgcInfoPoolFreeMin,
                        wgcInfoPoolSaturatedDrops, wgcInfoIngressHard, wgcInfoIngressSoft, wgcInfoIngressDecimated,
                        static_cast<long long>(wgcInfoCallbackGapAvgUs),
                        static_cast<long long>(wgcInfoCallbackGapMaxUs),
                        static_cast<long long>(wgcInfoSourceJitterAvgUs),
                        static_cast<long long>(wgcInfoSourceJitterMaxUs),
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
                     injectFreshCatchupThisWindow > 0 || injectLiveStaleTrimThisWindow > 0 ||
                     injectTargetSupersededThisWindow > 0 || injectTargetHoldWithCandidateThisWindow > 0) &&
                    (nowTick - s_lastInjectRepeatPressureInfoTick) >= 5000) {
                    LogInfo(
                        "[Inject CFR] Repeat pressure: hardEncoderOverload=%d dup=%u srcLimited=%u fenceDeferred=%u "
                        "timer=%u freshCatchup=%u repeatCatchup=%u staleTrim=%u recovery=%d/%u requeued=%u "
                        "droppedDeferred=%u targetSelect=%u targetSuperseded=%u targetHold=%u "
                        "holdWithCandidate=%u targetResidualMax=%uus "
                        "tickEmit=%u unique=%u sourceFps=%.2f enc=%.2fms service=%.2fms cycle=%.2fms "
                        "sustain=%.1ffps overload=0x%X",
                        hardEncoderPressure ? 1 : 0, duplicateTicksThisWindow, sourceRepeatsThisWindow,
                        deferredRepeatsThisWindow, dupTimer - lastDuplicateReasonTimerRebase,
                        injectFreshCatchupThisWindow, injectRepeatCatchupThisWindow, injectLiveStaleTrimThisWindow,
                        injectCfrRecoveryActive ? 1 : 0, injectCfrRecoveryEpisodesThisWindow,
                        injectDeferredRequeuedThisWindow, injectDeferredDroppedThisWindow,
                        injectTargetSelectThisWindow, injectTargetSupersededThisWindow,
                        injectTargetHoldThisWindow, injectTargetHoldWithCandidateThisWindow,
                        injectTargetResidualMaxUs,
                        cadenceCounters.liveTickEmitCount, cadenceCounters.liveTickUniqueCount, srcFpsX100Val / 100.0,
                        smoothedEncodeMs, smoothedInjectServiceMs, smoothedEncCycleMs, sustainableOutputFps,
                        overloadFlags);
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
            injectTargetSelectThisWindow = 0;
            injectTargetSupersededThisWindow = 0;
            injectTargetHoldThisWindow = 0;
            injectTargetHoldWithCandidateThisWindow = 0;
            injectCfrRecoveryEpisodesThisWindow = 0;
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
            wgcSyncDelayHoldCount = 0;
            wgcSyncDelaySourceLimitedHoldCount = 0;
            wgcSyncDelayPolicyHoldCount = 0;
            wgcTooNewLeadMaxUs = 0;
            wgcDelayResidualWindowSamples = 0;
            wgcDelayResidualWindowAbsAccumUs = 0;
            wgcDelayResidualWindowSignedAccumUs = 0;
            wgcDelayResidualWindowAbsMaxUs = 0;
            wgcDelayResidualWindowLateMaxUs = 0;
            wgcDelayResidualWindowAbsHistogram.fill(0);
            wgcDelayRawResidualWindowSamples = 0;
            wgcDelayRawResidualWindowAbsAccumUs = 0;
            wgcDelayRawResidualWindowSignedAccumUs = 0;
            wgcDelayRawResidualWindowAbsMaxUs = 0;
            wgcDelayRawResidualWindowLateMaxUs = 0;
            wgcDelayRawResidualWindowAbsHistogram.fill(0);
            wgcDelayRawMinusPredictedWindowSamples = 0;
            wgcDelayRawMinusPredictedWindowSignedAccumUs = 0;
            wgcDelayRawMinusPredictedWindowAbsMaxUs = 0;
            wgcDelayRelaxedSelectionWindowCount = 0;
            wgcDelayRelaxedBetterTargetWindow = 0;
            wgcDelayRelaxedRepeatClusterWindow = 0;
            wgcDelayRelaxedRejectedSyncRiskWindow = 0;
            wgcDelayRelaxedRejectedResidualHeadroomWindow = 0;
            wgcDelayRelaxedRejectedRepeatCostWindow = 0;
            wgcDelaySoftLateRejectedWindow = 0;
            wgcDelaySoftLateAcceptedWindow = 0;
            wgcDelayNearCapAcceptedWindow = 0;
            wgcDelayUniformCadenceWindow = 0;
            wgcDelayUniformHoldWindow = 0;
            wgcDelayPaceCapTrimWindow = 0;
            wgcRetainedCapTrimWindow = 0;
            wgcPoolPressureTrimWindow = 0;
            wgcDelayOlderFrameAvoidedRepeatWindow = 0;
            wgcDelaySourceLimitedRepeatWindow = 0;
            wgcDelayRepeatRescueAttemptWindow = 0;
            wgcDelayRepeatRescueSuccessWindow = 0;
            wgcDelayRepeatRescueRejectedSyncWindow = 0;
            wgcDelayRepeatRescueRejectedHeadroomWindow = 0;
            wgcDelayRepeatRescueRejectedCostWindow = 0;
            wgcDelayRepeatPromotedBeforeRepeatWindow = 0;
            wgcDelayRepeatPromotionAttemptWindow = 0;
            wgcDelayRepeatPromotionRejectedSoftWindow = 0;
            wgcDelayRepeatSafeAfterPromotionWindow = 0;
            wgcDelayRepeatWithSafeCandidateWindow = 0;
            wgcDelayRepeatWithoutSafeCandidateWindow = 0;
            wgcDelayRepeatWithSoftSafeCandidateWindow = 0;
            wgcDelayRepeatWithoutSoftSafeCandidateWindow = 0;
            wgcDelayRepeatHardOnlyCandidateWindow = 0;
            wgcDelaySyncProtectedRepeatWindow = 0;
            wgcDelayWindowHealthyRepeatWindow = 0;
            wgcDelayWindowRecoverableRepeatWindow = 0;
            wgcDelayWindowSourceLimitedRepeatWindow = 0;
            wgcDelayWindowHardStallRepeatWindow = 0;
            wgcDelayWindowPostStallRepeatWindow = 0;
            wgcDelayPostStallSafeFrameWindow = 0;
            wgcDelayRepeatReserveDepthWindowMax = 0;
            wgcDelayRepeatReserveSpanWindowMaxUs = 0;
            wgcDelayOldestSoftSafeAgeWindowMaxUs = 0;
            wgcDelayPostSelectionRejectedSyncRiskWindow = 0;
            wgcDelayPostSelectionRescuedSyncRiskWindow = 0;
            wgcDelayRepeatClusterPressureWindow = 0;
            wgcDelayRepeatClusterPressureWindowMaxTicks = 0;
            wgcSourceRepeatLowerBoundWindow = 0;
            wgcExcessRepeatWindow = 0;
            wgcPolicyAddedRepeatWindow = 0;
            wgcExcessRepeatClusterWindow = 0;
            wgcExcessRepeatClusterWindowMaxTicks = 0;
            wgcSyncDelaySourceRecoveryHoldCount = 0;
            cfrCatchupTicksExecuted = 0;
            wgcFreshCatchupCount = 0;
            wgcReserveSpendTickCount = 0;
            wgcProducerRateRetuneCount = 0;
            wgcNoFreshTickCount = 0;
            wgcDelayReservoirLowWaterTickCount = 0;
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
        const bool summaryUsesScreenGrab = IsActiveScreenGrab();
        if (summaryUsesScreenGrab) {
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
                "def=%llu timer=%llu drain=%llu) SourceLimitedRepeats=%llu StarvedEpisodes=%llu AntiFreezeFloor=%llu "
                "AntiFreezeFloorSkippedSync=%llu BiasClampCount=%llu longest=%llums longestDup=%llu "
                "longestContiguousDup=%llu (%llums) "
                "worstIn=%u "
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
                static_cast<unsigned long long>(wgcUniformAntiFreezeFloorTotal),
                static_cast<unsigned long long>(wgcUniformAntiFreezeFloorSkippedSyncTotal),
                static_cast<unsigned long long>(wgcBiasClampCount),
                static_cast<unsigned long long>(captureSessionSummary.longestStarvedEpisodeMs),
                static_cast<unsigned long long>(captureSessionSummary.longestStarvedEpisodeDuplicateTicks),
                static_cast<unsigned long long>(captureSessionSummary.longestContiguousDupTicks),
                static_cast<unsigned long long>(static_cast<double>(captureSessionSummary.longestContiguousDupTicks) *
                                                frameIntervalMs),
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
            const int64_t wgcAvSyncStartupDelayUs =
                qpcFreq.QuadPart > 0 && wgcAvSyncStartupEffectiveDelayQpc > 0
                    ? (wgcAvSyncStartupEffectiveDelayQpc * 1000000) / qpcFreq.QuadPart
                    : 0;
            const int64_t wgcAvSyncScheduleOffsetUs =
                qpcFreq.QuadPart > 0 ? (wgcAvSyncScheduleOffsetQpc * 1000000) / qpcFreq.QuadPart : 0;
            const uint32_t wgcDelayResidualAvgAbsUs =
                wgcDelayResidualSamples > 0 ? SaturatingToUint32(wgcDelayResidualAbsAccumUs / wgcDelayResidualSamples)
                                            : 0u;
            const int32_t wgcDelayResidualAvgSignedUs =
                wgcDelayResidualSamples > 0 ? static_cast<int32_t>(wgcDelayResidualSignedAccumUs /
                                                                   static_cast<int64_t>(wgcDelayResidualSamples))
                                            : 0;
            const uint32_t wgcDelayRealizedAvgUs =
                wgcDelayResidualSamples > 0 ? SaturatingToUint32(wgcDelayRealizedAccumUs / wgcDelayResidualSamples)
                                            : 0u;
            const uint32_t wgcDelayRealizedMinFinalUs =
                wgcDelayResidualSamples > 0 && wgcDelayRealizedMinUs != UINT32_MAX ? wgcDelayRealizedMinUs : 0u;
            const uint32_t wgcDelayRawResidualAvgAbsUs =
                wgcDelayRawResidualSamples > 0
                    ? SaturatingToUint32(wgcDelayRawResidualAbsAccumUs / wgcDelayRawResidualSamples)
                    : 0u;
            const int32_t wgcDelayRawResidualAvgSignedUs =
                wgcDelayRawResidualSamples > 0 ? static_cast<int32_t>(wgcDelayRawResidualSignedAccumUs /
                                                                      static_cast<int64_t>(wgcDelayRawResidualSamples))
                                               : 0;
            const int32_t wgcDelayRawMinusPredictedAvgSignedUs =
                wgcDelayRawMinusPredictedSamples > 0
                    ? static_cast<int32_t>(wgcDelayRawMinusPredictedSignedAccumUs /
                                           static_cast<int64_t>(wgcDelayRawMinusPredictedSamples))
                    : 0;
            const bool wgcActiveDelayMixedPolicyFault = ce::capture_policy::IsWgcActiveDelayMixedPolicyPressureFault(
                SaturatingToUint32(wgcSyncDelaySourceLimitedHoldTotal), SaturatingToUint32(wgcSyncDelayPolicyHoldTotal),
                SaturatingToUint32(wgcSyncDelayHoldTotal));
            const uint64_t wgcPolicyNoSourceRepeats =
                std::min<uint64_t>(captureSessionSummary.duplicateNoSourceTicks, wgcSyncDelayPolicyHoldTotal);
            const uint64_t wgcDeliveryRepeatLowerBoundTotal =
                captureSessionSummary.duplicateNoSourceTicks - wgcPolicyNoSourceRepeats;
            const uint64_t wgcCombinedSourceRepeatLowerBoundTotal =
                std::max(wgcSourceRepeatLowerBoundTotal, wgcDeliveryRepeatLowerBoundTotal);
            const uint64_t wgcDuplicateRepeatExcessTotal =
                captureSessionSummary.duplicateTicks > wgcCombinedSourceRepeatLowerBoundTotal
                    ? (captureSessionSummary.duplicateTicks - wgcCombinedSourceRepeatLowerBoundTotal)
                    : 0ull;
            const uint64_t wgcCombinedExcessRepeatTotal = std::max(wgcExcessRepeatTotal, wgcDuplicateRepeatExcessTotal);
            const bool wgcCfrSmoothnessNotMaximal = ce::capture_policy::IsWgcCfrSmoothnessNotMaximal(
                SaturatingToUint32(liveTicksOutput), SaturatingToUint32(wgcCombinedExcessRepeatTotal),
                SaturatingToUint32(wgcPolicyAddedRepeatTotal), wgcExcessRepeatClusterMaxTicks,
                SaturatingToUint32(wgcDelayPostSelectionRejectedSyncRiskTotal));
            SnapshotPublishedWgcRuntimeLogState();
            const bool wgcLogSnapshotHasPool = g_WgcRuntimeLogSnapshot.hasPoolEvidence.load(std::memory_order_acquire);
            const uint32_t wgcSummarySourceFramePoolBuffers =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.sourceFramePoolBuffers.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSourceFramePoolBufferCount() : 0u);
            const uint32_t wgcSummaryBudgetSurfaces =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.budgetSurfaces.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessBudgetSurfaceCount() : 0u);
            const uint32_t wgcSummarySyncFrames =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.syncFrames.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessSyncFrameCount() : 0u);
            const uint32_t wgcSummaryExtraFrames =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.extraFrames.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessRetainedFrameCount() : 0u);
            const uint32_t wgcSummaryRetainedCap =
                wgcLogSnapshotHasPool
                    ? g_WgcRuntimeLogSnapshot.retainedCap.load(std::memory_order_relaxed)
                    : (g_WgcCap ? g_WgcCap->GetSmoothnessRetainedFrameCap() : wgcSmoothnessRetainedFrameCap);
            const uint32_t wgcSummaryReservedFreeSlots =
                wgcLogSnapshotHasPool
                    ? g_WgcRuntimeLogSnapshot.reservedFreeSlots.load(std::memory_order_relaxed)
                    : (g_WgcCap ? g_WgcCap->GetSmoothnessReservedFreeSlotCount() : wgcSmoothnessReservedFreeSlots);
            const uint32_t wgcSummarySafetySlots =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.safetySlots.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessSafetySlotCount() : 0u);
            const uint32_t wgcSummaryPoolLeasedMax =
                wgcLogSnapshotHasPool ? std::max(g_WgcRuntimeLogSnapshot.poolLeasedMax.load(std::memory_order_relaxed),
                                                 g_WgcCap ? g_WgcCap->GetPoolSlotLeasedMaxCount() : 0u)
                                      : (g_WgcCap ? g_WgcCap->GetPoolSlotLeasedMaxCount() : 0u);
            const uint32_t wgcSnapshotFreeMin = g_WgcRuntimeLogSnapshot.poolFreeMin.load(std::memory_order_relaxed);
            const uint32_t wgcCurrentFreeMin = g_WgcCap ? g_WgcCap->GetPoolSlotFreeMinCount() : 0u;
            const uint32_t wgcSummaryPoolFreeMin =
                (wgcLogSnapshotHasPool && wgcSnapshotFreeMin != UINT32_MAX)
                    ? (wgcCurrentFreeMin > 0 ? std::min(wgcSnapshotFreeMin, wgcCurrentFreeMin) : wgcSnapshotFreeMin)
                    : wgcCurrentFreeMin;
            const uint32_t wgcSummaryPoolFreeNow = g_WgcCap ? g_WgcCap->GetPoolSlotFreeCurrentCount() : 0u;
            const uint32_t wgcSummaryPoolSaturatedDrops =
                wgcLogSnapshotHasPool
                    ? std::max(g_WgcRuntimeLogSnapshot.poolSaturatedDrops.load(std::memory_order_relaxed),
                               g_WgcCap ? g_WgcCap->GetPoolSaturatedDropCount() : 0u)
                    : (g_WgcCap ? g_WgcCap->GetPoolSaturatedDropCount() : 0u);
            const uint32_t wgcSummaryOverwritePrevented =
                wgcLogSnapshotHasPool
                    ? std::max(g_WgcRuntimeLogSnapshot.poolOverwritePrevented.load(std::memory_order_relaxed),
                               g_WgcCap ? g_WgcCap->GetPoolSlotOverwritePreventedCount() : 0u)
                    : (g_WgcCap ? g_WgcCap->GetPoolSlotOverwritePreventedCount() : 0u);
            const uint32_t wgcSummaryLeaseMismatches =
                wgcLogSnapshotHasPool
                    ? std::max(g_WgcRuntimeLogSnapshot.poolLeaseMismatches.load(std::memory_order_relaxed),
                               g_WgcCap ? g_WgcCap->GetPoolLeaseMismatchCount() : 0u)
                    : (g_WgcCap ? g_WgcCap->GetPoolLeaseMismatchCount() : 0u);
            const uint64_t wgcSummarySmoothVramBytes =
                wgcLogSnapshotHasPool && g_WgcRuntimeLogSnapshot.estimatedVramBytes.load(std::memory_order_relaxed) > 0
                    ? g_WgcRuntimeLogSnapshot.estimatedVramBytes.load(std::memory_order_relaxed)
                    : wgcSmoothnessEstimatedVramBytes;
            const uint32_t wgcSummarySourceFormat =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.sourceFormat.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessSourceDxgiFormat() : 0u);
            const uint32_t wgcSummaryRetainedFormat =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.retainedFormat.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessCopyDxgiFormat() : 0u);
            const uint32_t wgcSummaryCompactRetained =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.compactRetained.load(std::memory_order_relaxed)
                                      : (g_WgcCap && g_WgcCap->IsCompactRetainedCopyActive() ? 1u : 0u);
            const uint64_t wgcSummarySourceBudgetBytes =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.sourceBudgetBytes.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessSourceEstimatedVramBytes() : 0ull);
            const uint64_t wgcSummaryCopyBudgetBytes =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.copyBudgetBytes.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessCopyEstimatedVramBytes() : 0ull);
            const uint64_t wgcSummarySourceSurfaceBytes =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.sourceSurfaceBytes.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessSourceBytesPerSurface() : 0ull);
            const uint64_t wgcSummaryCopySurfaceBytes =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.copySurfaceBytes.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessCopyBytesPerSurface() : 0ull);
            const int64_t wgcSummaryConvertUs =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.lastConvertUs.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetLastPoolConvertTimeUs() : 0);
            const uint32_t wgcSummaryOutputFps = getWgcSmoothnessOutputFps();
            const uint32_t wgcSummaryDuplicateTimestampsSeen =
                std::max(g_WgcRuntimeLogSnapshot.duplicateTimestampsSeen.load(std::memory_order_relaxed),
                         g_WgcCap ? g_WgcCap->GetNormalizedDuplicateTimestampCount() : 0u);
            const uint32_t wgcSummaryDuplicateTimestampsSkipped =
                std::max(g_WgcRuntimeLogSnapshot.duplicateTimestampsSkipped.load(std::memory_order_relaxed),
                         g_WgcCap ? g_WgcCap->GetDuplicateTimestampSkipCount() : 0u);
            const int64_t wgcSummarySmoothTargetDelayUs =
                qpcToUs(ce::capture_policy::GetWgcStartupSmoothnessTargetDelayQpc(
                    wgcSmoothnessRetainedFrames, targetIntervalTicks, wgcSummaryOutputFps,
                    config.wgcSmoothnessBufferMaxMs));
            const int64_t wgcSummarySmoothActualDelayUs = qpcToUs(wgcSmoothnessActiveDelayQpc);
            const int64_t wgcSummarySmoothDelayDeficitUs =
                std::max<int64_t>(0, wgcSummarySmoothTargetDelayUs - wgcSummarySmoothActualDelayUs);
            const int64_t wgcSummaryEffectiveDelayUs = qpcToUs(getWgcEffectiveContentDelayQpc());
            const int64_t wgcSummaryStartupDelayDeficitUs =
                std::max<int64_t>(0, wgcStartupDelayTargetUs - wgcSummaryEffectiveDelayUs);
            LogInfo(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=%llu maxDropTicks=%u cadenceEvents=%llu "
                "phaseErrorMax=%uus shortfallMax=%.1fms staleDebtDrops=%llu liveRebase=%llu/%u "
                "tooNewRepeats=%u syncDelayHolds=%llu tooNewLeadMax=%uus avDelay=%.1fms startupDelay=%.1fms "
                "scheduleOffset=%lldus effectiveDelay=%.1fms lowSourceBypass=%llu modeMismatch=%llu "
                "sourceBacktrack=%llu syncDelaySourceLimitedHolds=%llu syncDelayPolicyHolds=%llu "
                "startupReserveFrames=%u startupReserveSpan=%lldus startupDelayTarget=%lldus "
                "startupReserveSelected=%d startupReserveReason=%s producerTargetFps=%u producerContractRetunes=%llu "
                "smoothBuf=%d smoothTargetMs=%u "
                "smoothFrames=%u/%u/%u smoothDelay=%.1fms smoothPoolSlots=%u sourceFramePoolBuffers=%u "
                "budgetSurfaces=%u syncFrames=%u extraFrames=%u retainedCap=%u reservedFreeSlots=%u safetySlots=%u "
                "retainedCapTrim=%llu ingressAccepted=%u ingressDecimated=%u ingressPlaySoft=%u "
                "ingressPlayCredit=%u ingressRetained=%u/%u "
                "ingressLowWater=%u leasedMax=%u freeNow=%u freeMin=%u poolPressureTrim=%llu "
                "poolSaturatedDrops=%u overwritePrevented=%u "
                "leaseMismatches=%u smoothVramMB=%.1f smoothCapLimited=%d smoothReason=%s "
                "sourceFmt=%u retainedFmt=%u compactRetained=%d sourceBudgetMB=%.1f copyBudgetMB=%.1f "
                "sourceSurfaceMB=%.1f copySurfaceMB=%.1f convertUs=%lld",
                static_cast<unsigned long long>(wgcEncoderLimitedSourceDropTotal), wgcEncoderLimitedSourceDropMaxTicks,
                static_cast<unsigned long long>(wgcEncoderLimitedCadenceEventCount),
                captureSessionSummary.maxWgcContentPhaseErrorUs, captureSessionSummary.maxShortfallDurationMs,
                static_cast<unsigned long long>(wgcDropStaleDebtTotal),
                static_cast<unsigned long long>(wgcLiveSchedulerRebaseTotal), wgcLiveSchedulerRebaseMaxTicks,
                SaturatingToUint32(wgcRepeatPolicyHoldTotal), static_cast<unsigned long long>(wgcSyncDelayHoldTotal),
                wgcTooNewLeadSessionMaxUs, avContentDelayActive ? maxAudioCaptureLatencyMs : 0.0f,
                static_cast<double>(wgcAvSyncStartupDelayUs) / 1000.0,
                static_cast<long long>(wgcAvSyncScheduleOffsetUs),
                static_cast<double>(qpcToUs(getWgcEffectiveContentDelayQpc())) / 1000.0,
                static_cast<unsigned long long>(wgcEncoderLimitedSuppressedByLowSourceTotal),
                static_cast<unsigned long long>(wgcCapacityPressureModeMismatchTotal),
                static_cast<unsigned long long>(wgcSelectedSourceBacktrackTotal),
                static_cast<unsigned long long>(wgcSyncDelaySourceLimitedHoldTotal),
                static_cast<unsigned long long>(wgcSyncDelayPolicyHoldTotal), wgcStartupReserveFrames,
                static_cast<long long>(wgcStartupReserveSpanUs), static_cast<long long>(wgcStartupDelayTargetUs),
                wgcStartupSelectedByDelayReserve ? 1 : 0, wgcStartupReserveReason.c_str(),
                g_WgcCap ? g_WgcCap->GetProducerTargetFps() : 0u,
                static_cast<unsigned long long>(wgcProducerRateRetuneTotal), config.wgcSmoothnessBufferEnabled ? 1 : 0,
                config.wgcSmoothnessBufferMaxMs, wgcSmoothnessActualFrames, wgcSmoothnessRetainedFrames,
                wgcSmoothnessDesiredFrames, static_cast<double>(wgcSummarySmoothActualDelayUs) / 1000.0,
                wgcSmoothnessPoolSlots, wgcSummarySourceFramePoolBuffers, wgcSummaryBudgetSurfaces,
                wgcSummarySyncFrames, wgcSummaryExtraFrames, wgcSummaryRetainedCap, wgcSummaryReservedFreeSlots,
                wgcSummarySafetySlots, static_cast<unsigned long long>(wgcRetainedCapTrimTotal),
                g_WgcCap ? g_WgcCap->GetIngressAcceptedCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressDecimatedCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedUniformPlayoutCreditCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressRetainedFrameCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressRetainedFrameCap() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressLowWaterFrameCount() : 0u, wgcSummaryPoolLeasedMax,
                wgcSummaryPoolFreeNow, wgcSummaryPoolFreeMin, static_cast<unsigned long long>(wgcPoolPressureTrimTotal),
                wgcSummaryPoolSaturatedDrops, wgcSummaryOverwritePrevented, wgcSummaryLeaseMismatches,
                static_cast<double>(wgcSummarySmoothVramBytes) / (1024.0 * 1024.0), wgcSmoothnessCapLimited ? 1 : 0,
                wgcSmoothnessBufferReason.c_str(), wgcSummarySourceFormat, wgcSummaryRetainedFormat,
                wgcSummaryCompactRetained, static_cast<double>(wgcSummarySourceBudgetBytes) / (1024.0 * 1024.0),
                static_cast<double>(wgcSummaryCopyBudgetBytes) / (1024.0 * 1024.0),
                static_cast<double>(wgcSummarySourceSurfaceBytes) / (1024.0 * 1024.0),
                static_cast<double>(wgcSummaryCopySurfaceBytes) / (1024.0 * 1024.0),
                static_cast<long long>(wgcSummaryConvertUs));
            LogInfo(
                "[WGC CFR SMOOTHNESS BUFFER] smoothTargetDelay=%lldus smoothActualDelay=%lldus "
                "smoothDelayDeficit=%lldus startupDelayTarget=%lldus effectiveDelay=%lldus "
                "startupDelayDeficit=%lldus finalAvSync=exported_tracks_authoritative",
                static_cast<long long>(wgcSummarySmoothTargetDelayUs),
                static_cast<long long>(wgcSummarySmoothActualDelayUs),
                static_cast<long long>(wgcSummarySmoothDelayDeficitUs), static_cast<long long>(wgcStartupDelayTargetUs),
                static_cast<long long>(wgcSummaryEffectiveDelayUs),
                static_cast<long long>(wgcSummaryStartupDelayDeficitUs));
            const uint32_t wgcSummaryIngressHard = g_WgcCap ? g_WgcCap->GetIngressHardReservePressureCount() : 0u;
            const uint32_t wgcSummaryIngressSoft = g_WgcCap ? g_WgcCap->GetIngressSoftReservePressureCount() : 0u;
            const uint32_t wgcSummaryIngressDecimated = g_WgcCap ? g_WgcCap->GetIngressDecimatedCount() : 0u;
            const uint32_t wgcSummaryOverloadFlags =
                g_pSharedMem ? g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed) : 0u;
            const uint32_t wgcSummaryMuxBackpressure =
                g_pSharedMem ? g_pSharedMem->runtimeState.muxBackpressureCount.load(std::memory_order_relaxed) : 0u;
            const bool wgcSummaryPoolPressure = wgcSummaryPoolSaturatedDrops > 0 || wgcSummaryIngressHard > 0 ||
                                                wgcSummaryIngressDecimated > 0 || wgcSummaryPoolFreeMin == 0;
            const bool wgcSummaryEncoderMuxPressure = ce::capture_policy::HasRecordingEncoderOrMuxPressure(
                wgcSummaryOverloadFlags, wgcSummaryMuxBackpressure, wgcEncoderLimitedSourceDropTotal);
            const char* wgcSummaryLimiter = wgcSummaryEncoderMuxPressure                       ? "encoder_or_mux"
                                            : wgcSummaryPoolPressure                           ? "wgc_pool_pressure"
                                            : captureSessionSummary.duplicateNoSourceTicks > 0 ? "source_limited"
                                            : captureSessionSummary.duplicateTicks > 0         ? "source_cadence_or_vrr"
                                                                                               : "none";
            const bool wgcSummaryCleanEncoderMux =
                !wgcSummaryEncoderMuxPressure && wgcSummaryOverloadFlags == 0 && wgcSummaryMuxBackpressure == 0;
            const bool wgcSummaryCleanPool = !wgcSummaryPoolPressure;
            const bool wgcSummaryCleanSelection = wgcCombinedExcessRepeatTotal == 0 && wgcPolicyAddedRepeatTotal == 0 &&
                                                  wgcDelayPostSelectionRejectedSyncRiskTotal == 0 &&
                                                  !wgcCfrSmoothnessNotMaximal && !wgcActiveDelayMixedPolicyFault &&
                                                  wgcCapacityPressureModeMismatchTotal == 0 &&
                                                  wgcSelectedSourceBacktrackTotal == 0;
            const bool wgcSummaryCoverageHoles =
                captureSessionSummary.duplicateNoSourceTicks > 0 || wgcCombinedSourceRepeatLowerBoundTotal > 0 ||
                wgcSourceRepeatLowerBoundTotal > 0 || wgcDeliveryRepeatLowerBoundTotal > 0;
            const bool wgcSummarySourceCoverageBestEffort =
                wgcSummaryCoverageHoles && wgcSummaryCleanEncoderMux && wgcSummaryCleanPool && wgcSummaryCleanSelection;
            const char* wgcSummaryCoverage =
                wgcSummaryCoverageHoles ? "limited"
                                        : (captureSessionSummary.duplicateTicks > 0 ? "cadence_or_vrr" : "full");
            const char* wgcSummaryCoverageReason =
                (wgcSourceRepeatLowerBoundTotal > 0 && wgcDeliveryRepeatLowerBoundTotal > 0)
                    ? "source_and_delivery_holes"
                : (wgcDeliveryRepeatLowerBoundTotal > 0) ? "delivery_holes"
                : (wgcSourceRepeatLowerBoundTotal > 0)   ? "source_holes"
                : (captureSessionSummary.duplicateNoSourceTicks > 0)
                    ? "no_fresh_source_for_cfr_slots"
                    : (captureSessionSummary.duplicateTicks > 0 ? "cadence_or_vrr" : "none");
            LogInfo(
                "[WGC CFR QUALITY] duplicatePct=%.1f duplicates=%llu/%llu worst1sUnique=%u worst1sRepeats=%u "
                "worst1sEmit=%u limiter=%s sourceLimitedRepeats=%llu poolPressure=%d freeMin=%u "
                "poolSaturatedDrops=%u ingressHard=%u ingressSoft=%u ingressDecimated=%u "
                "poolPressureTrim=%llu "
                "ingressPlaySoft=%u ingressPlayCredit=%u overwritePrevented=%u "
                "syncProtectedRepeats=%llu policyAddedRepeats=%llu excessRepeats=%llu "
                "smoothDelayDeficitUs=%lld startupDelayDeficitUs=%lld "
                "dupTsSeen=%u dupTsSkipped=%u encoderOverload=0x%X muxBackpressure=%u "
                "compactRetained=%d sourceFmt=%u retainedFmt=%u convertUs=%lld backend=%s timingBasis=cpu_wall "
                "finalAvSync=exported_tracks_authoritative",
                static_cast<double>(duplicatePermille) / 10.0,
                static_cast<unsigned long long>(captureSessionSummary.duplicateTicks),
                static_cast<unsigned long long>(liveTicksOutput), captureSessionSummary.worstOneSecondUniqueCount,
                captureSessionSummary.worstOneSecondRepeatCount, captureSessionSummary.worstOneSecondEmitCount,
                wgcSummaryLimiter, static_cast<unsigned long long>(captureSessionSummary.duplicateNoSourceTicks),
                wgcSummaryPoolPressure ? 1 : 0, wgcSummaryPoolFreeMin, wgcSummaryPoolSaturatedDrops,
                wgcSummaryIngressHard, wgcSummaryIngressSoft, wgcSummaryIngressDecimated,
                static_cast<unsigned long long>(wgcPoolPressureTrimTotal),
                g_WgcCap ? g_WgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedUniformPlayoutCreditCount() : 0u, wgcSummaryOverwritePrevented,
                static_cast<unsigned long long>(wgcDelaySyncProtectedRepeatTotal),
                static_cast<unsigned long long>(wgcPolicyAddedRepeatTotal),
                static_cast<unsigned long long>(wgcCombinedExcessRepeatTotal),
                static_cast<long long>(wgcSummarySmoothDelayDeficitUs),
                static_cast<long long>(wgcSummaryStartupDelayDeficitUs), wgcSummaryDuplicateTimestampsSeen,
                wgcSummaryDuplicateTimestampsSkipped, wgcSummaryOverloadFlags, wgcSummaryMuxBackpressure,
                wgcSummaryCompactRetained, wgcSummarySourceFormat, wgcSummaryRetainedFormat,
                static_cast<long long>(wgcSummaryConvertUs),
                g_WgcCap && g_WgcCap->IsUsingDesktopDuplication() ? "dxgi_dup" : "wgc");
            LogInfo(
                "[WGC CFR SOURCE COVERAGE] coverage=%s reason=%s bestEffort=%d outputFps=%u "
                "duplicates=%llu/%llu sourceLimitedRepeats=%llu sourceRepeatLowerBound=%llu "
                "syncSourceRepeatLowerBound=%llu deliveryRepeatLowerBound=%llu excessRepeats=%llu "
                "policyAddedRepeats=%llu policyNoSourceRepeats=%llu cleanEncoderMux=%d cleanPool=%d "
                "cleanSelection=%d encoderOverload=0x%X muxBackpressure=%u poolPressure=%d "
                "poolFreeMin=%u finalAvSync=exported_tracks_authoritative "
                "note=surplus_source_frames_are_dropped_when_available_repeats_mean_cfr_coverage_holes",
                wgcSummaryCoverage, wgcSummaryCoverageReason, wgcSummarySourceCoverageBestEffort ? 1 : 0,
                wgcSummaryOutputFps, static_cast<unsigned long long>(captureSessionSummary.duplicateTicks),
                static_cast<unsigned long long>(liveTicksOutput),
                static_cast<unsigned long long>(captureSessionSummary.duplicateNoSourceTicks),
                static_cast<unsigned long long>(wgcCombinedSourceRepeatLowerBoundTotal),
                static_cast<unsigned long long>(wgcSourceRepeatLowerBoundTotal),
                static_cast<unsigned long long>(wgcDeliveryRepeatLowerBoundTotal),
                static_cast<unsigned long long>(wgcCombinedExcessRepeatTotal),
                static_cast<unsigned long long>(wgcPolicyAddedRepeatTotal),
                static_cast<unsigned long long>(wgcPolicyNoSourceRepeats), wgcSummaryCleanEncoderMux ? 1 : 0,
                wgcSummaryCleanPool ? 1 : 0, wgcSummaryCleanSelection ? 1 : 0, wgcSummaryOverloadFlags,
                wgcSummaryMuxBackpressure, wgcSummaryPoolPressure ? 1 : 0, wgcSummaryPoolFreeMin);
            LogInfo(
                "[WGC CFR SMOOTHNESS INGRESS] accepted=%u decimated=%u retained=%u/%u lowWater=%u "
                "accLowWater=%u accRecovery=%u accSourceBelow=%u accHealthy=%u "
                "accPlaySoft=%u accPlayCredit=%u "
                "decSoftReserve=%u decHardReserve=%u decCredit=%u "
                "softReservePressure=%u hardReservePressure=%u dupTsSeen=%u dupTsSkipped=%u lastReason=%s",
                g_WgcCap ? g_WgcCap->GetIngressAcceptedCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressDecimatedCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressRetainedFrameCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressRetainedFrameCap() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressLowWaterFrameCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedLowWaterCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedRecoveryCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedSourceBelowCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedHealthyCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedUniformPlayoutCreditCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressDecimatedSoftReserveCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressDecimatedHardReserveCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressDecimatedCreditCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressSoftReservePressureCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressHardReservePressureCount() : 0u, wgcSummaryDuplicateTimestampsSeen,
                wgcSummaryDuplicateTimestampsSkipped,
                g_WgcCap ? WgcIngressAdmissionReasonName(g_WgcCap->GetIngressAdmissionReasonCode()) : "none");
            LogInfo(
                "[WGC CFR SMOOTHNESS SOURCE] acceptedTotal=%llu cfrTicksTotal=%llu "
                "rollingAccepted=%u rollingCfrTicks=%u rollingDeficit=%u rollingSurplus=%u "
                "lastWindowAccepted=%u lastWindowCfrTicks=%u windowSlots=%zu",
                static_cast<unsigned long long>(wgcRollingSourceAcceptedTotal),
                static_cast<unsigned long long>(wgcRollingSourceCfrTickTotal), wgcRollingSourceAcceptedSum,
                wgcRollingSourceCfrTickSum, wgcRollingSourceDeficitFrames, wgcRollingSourceSurplusFrames,
                wgcRollingSourceAcceptedWindow, wgcRollingSourceCfrTicksWindow, wgcRollingSourceSlotCount);
            LogInfo(
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=%u delayReservoirTargetFrames=%u "
                "delayReservoirLowWaterTicks=%llu realizedDelayAvg=%uus realizedDelayMin=%uus "
                "realizedDelayMax=%uus delayResidualAvg=%d/%uus delayResidualMax=%uus "
                "delayResidualP95=%uus delayResidualLateMax=%uus delayResidualEarlyMax=%uus "
                "rawResidualAvg=%d/%uus rawResidualMax=%uus rawResidualP95=%uus rawResidualLateMax=%uus "
                "rawResidualEarlyMax=%uus predictedResidualAvg=%d/%uus predictedResidualP95=%uus "
                "predictedResidualLateMax=%uus rawMinusPredictedAvg=%d/%uus rawMinusPredictedMax=%uus",
                getWgcDelayReservoirLowWaterFrames(), getWgcDelayReservoirTargetFrames(),
                static_cast<unsigned long long>(wgcDelayReservoirLowWaterTickTotal), wgcDelayRealizedAvgUs,
                wgcDelayRealizedMinFinalUs, wgcDelayRealizedMaxUs, wgcDelayResidualAvgSignedUs,
                wgcDelayResidualAvgAbsUs, wgcDelayResidualAbsMaxUs, wgcDelayResidualP95Us(), wgcDelayResidualLateMaxUs,
                wgcDelayResidualEarlyMaxUs, wgcDelayRawResidualAvgSignedUs, wgcDelayRawResidualAvgAbsUs,
                wgcDelayRawResidualAbsMaxUs, wgcDelayRawResidualP95Us(), wgcDelayRawResidualLateMaxUs,
                wgcDelayRawResidualEarlyMaxUs, wgcDelayResidualAvgSignedUs, wgcDelayResidualAvgAbsUs,
                wgcDelayResidualP95Us(), wgcDelayResidualLateMaxUs, wgcDelayRawMinusPredictedAvgSignedUs,
                SaturatingToUint32(static_cast<uint64_t>(wgcDelayRawMinusPredictedAvgSignedUs >= 0
                                                             ? wgcDelayRawMinusPredictedAvgSignedUs
                                                             : -wgcDelayRawMinusPredictedAvgSignedUs)),
                wgcDelayRawMinusPredictedAbsMaxUs);
            // Smoothness FLOOR rollup: ties the resolved floor to its realized result so a soak run is
            // conclusive. With the floor active and working, realizedDelay(min/avg/max) should sit near
            // smoothFloorRealizedTargetUs with a bounded residualLateMax (jitter absorbed); a collapsed
            // realizedDelayMin with a large residualLateMax means jitter exceeded the floor budget
            // (overflow -> even repeats), NOT a sync/ghost-judder fault (audio anchor never moved).
            LogInfo(
                "[WGC CFR SMOOTHNESS FLOOR] smoothFloorSource=%s smoothFloorConfigured=%d smoothFloorMs=%u "
                "smoothFloorRequestedUs=%lld smoothFloorDelayUs=%lld smoothFloorClampedBy=%s "
                "smoothFloorRealizedTargetUs=%lld measuredDeliveryGapUs(avg/max)=%u/%u "
                "measuredSourceJitterUs(avg/max)=%u/%u realizedDelay(min/avg/max)Us=%u/%u/%u "
                "residualLateMaxUs=%u avContentDelayActive=%d",
                wgcSmoothnessFloorSource, wgcSmoothnessFloorConfigured ? 1 : 0, config.wgcSmoothnessFloorMs,
                static_cast<long long>(qpcToUs(wgcSmoothnessFloorRequestedQpc)),
                static_cast<long long>(qpcToUs(wgcSmoothnessFloorDelayQpc)), wgcSmoothnessFloorClampedBy,
                static_cast<long long>(qpcToUs(avContentDelayActive ? 0 : wgcSmoothnessFloorDelayQpc)),
                wgcSmoothnessFloorJitter.deliveryGapAvgUs, wgcSmoothnessFloorJitter.deliveryGapMaxUs,
                wgcSmoothnessFloorJitter.sourceJitterAvgUs, wgcSmoothnessFloorJitter.sourceJitterMaxUs,
                wgcDelayRealizedMinFinalUs, wgcDelayRealizedAvgUs, wgcDelayRealizedMaxUs, wgcDelayResidualLateMaxUs,
                avContentDelayActive ? 1 : 0);
            LogInfo(
                "[WGC CFR SMOOTHNESS REPEAT] delayResidualRelaxedSelections=%llu delayResidualRelaxedMax=%uus "
                "delayResidualRelaxedRejectedSync=%llu delayRepeatClusterPressure=%llu "
                "delayRepeatClusterMax=%u delayResidualRelaxedBetter=%llu delayResidualRelaxedCluster=%llu "
                "delayResidualRelaxedRejectedHeadroom=%llu delayResidualRelaxedRejectedCost=%llu "
                "delaySoftLateRejected=%llu delaySoftLateAccepted=%llu delayOlderFrameAvoidedRepeat=%llu "
                "delaySourceLimitedRepeats=%llu delayRepeatRescue=%llu/%llu "
                "delayRepeatRescueRejected=%llu/%llu/%llu delayRepeatPromoted=%llu/%llu "
                "delayRepeatPromoteRejectedSoft=%llu delayRepeatSafeAfterPromote=%llu "
                "delayRepeatSafeCandidate=%llu delayRepeatNoSafeCandidate=%llu "
                "delayRepeatSoftSafeCandidate=%llu delayRepeatNoSoftSafeCandidate=%llu "
                "delayRepeatWindowClass=%llu/%llu/%llu delayRepeatWindowState=%llu/%llu/%llu/%llu/%llu "
                "delayPostStallSafeFrames=%llu delayRepeatReserveMax=%u/%uus "
                "delaySourceRecoveryHolds=%llu delaySourceRecoveryTicks=%llu "
                "delayNearCapAccepted=%llu delayHardOnlyCandidates=%llu "
                "delaySyncProtectedRepeats=%llu delayOldestSoftSafeAgeMax=%uus delayUniformCadence=%llu "
                "delayUniformHold=%llu delayPaceCapTrim=%llu",
                static_cast<unsigned long long>(wgcDelayRelaxedSelectionCount), wgcDelayRelaxedSelectionMaxUs,
                static_cast<unsigned long long>(wgcDelayRelaxedRejectedSyncRiskTotal),
                static_cast<unsigned long long>(wgcDelayRepeatClusterPressureTotal),
                wgcDelayRepeatClusterPressureMaxTicks,
                static_cast<unsigned long long>(wgcDelayRelaxedBetterTargetTotal),
                static_cast<unsigned long long>(wgcDelayRelaxedRepeatClusterTotal),
                static_cast<unsigned long long>(wgcDelayRelaxedRejectedResidualHeadroomTotal),
                static_cast<unsigned long long>(wgcDelayRelaxedRejectedRepeatCostTotal),
                static_cast<unsigned long long>(wgcDelaySoftLateRejectedTotal),
                static_cast<unsigned long long>(wgcDelaySoftLateAcceptedTotal),
                static_cast<unsigned long long>(wgcDelayOlderFrameAvoidedRepeatTotal),
                static_cast<unsigned long long>(wgcDelaySourceLimitedRepeatTotal),
                static_cast<unsigned long long>(wgcDelayRepeatRescueSuccessTotal),
                static_cast<unsigned long long>(wgcDelayRepeatRescueAttemptTotal),
                static_cast<unsigned long long>(wgcDelayRepeatRescueRejectedSyncTotal),
                static_cast<unsigned long long>(wgcDelayRepeatRescueRejectedHeadroomTotal),
                static_cast<unsigned long long>(wgcDelayRepeatRescueRejectedCostTotal),
                static_cast<unsigned long long>(wgcDelayRepeatPromotedBeforeRepeatTotal),
                static_cast<unsigned long long>(wgcDelayRepeatPromotionAttemptTotal),
                static_cast<unsigned long long>(wgcDelayRepeatPromotionRejectedSoftTotal),
                static_cast<unsigned long long>(wgcDelayRepeatSafeAfterPromotionTotal),
                static_cast<unsigned long long>(wgcDelayRepeatWithSafeCandidateTotal),
                static_cast<unsigned long long>(wgcDelayRepeatWithoutSafeCandidateTotal),
                static_cast<unsigned long long>(wgcDelayRepeatWithSoftSafeCandidateTotal),
                static_cast<unsigned long long>(wgcDelayRepeatWithoutSoftSafeCandidateTotal),
                static_cast<unsigned long long>(wgcDelayWindowHealthyRepeatTotal),
                static_cast<unsigned long long>(wgcDelayWindowRecoverableRepeatTotal),
                static_cast<unsigned long long>(wgcDelayWindowSourceLimitedRepeatTotal),
                static_cast<unsigned long long>(wgcDelayWindowHealthyRepeatTotal),
                static_cast<unsigned long long>(wgcDelayWindowRecoverableRepeatTotal),
                static_cast<unsigned long long>(wgcDelayWindowSourceLimitedRepeatTotal),
                static_cast<unsigned long long>(wgcDelayWindowHardStallRepeatTotal),
                static_cast<unsigned long long>(wgcDelayWindowPostStallRepeatTotal),
                static_cast<unsigned long long>(wgcDelayPostStallSafeFrameTotal), wgcDelayRepeatReserveDepthMax,
                wgcDelayRepeatReserveSpanMaxUs, static_cast<unsigned long long>(wgcSyncDelaySourceRecoveryHoldTotal),
                static_cast<unsigned long long>(wgcActiveDelaySourceRecoveryTicks),
                static_cast<unsigned long long>(wgcDelayNearCapAcceptedTotal),
                static_cast<unsigned long long>(wgcDelayRepeatHardOnlyCandidateTotal),
                static_cast<unsigned long long>(wgcDelaySyncProtectedRepeatTotal), wgcDelayOldestSoftSafeAgeMaxUs,
                static_cast<unsigned long long>(wgcDelayUniformCadenceTotal),
                static_cast<unsigned long long>(wgcDelayUniformHoldTotal),
                static_cast<unsigned long long>(wgcDelayPaceCapTrimTotal));
            LogInfo(
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=%llu "
                "delayPostSelectionRescuedSync=%llu sourceRepeatLowerBound=%llu excessRepeats=%llu "
                "policyAddedRepeats=%llu excessRepeatClusters=%llu excessRepeatClusterMax=%u "
                "smoothnessNotMaximal=%d mixedPolicyFault=%d syncSourceRepeatLowerBound=%llu "
                "deliveryRepeatLowerBound=%llu policyNoSourceRepeats=%llu",
                static_cast<unsigned long long>(wgcDelayPostSelectionRejectedSyncRiskTotal),
                static_cast<unsigned long long>(wgcDelayPostSelectionRescuedSyncRiskTotal),
                static_cast<unsigned long long>(wgcCombinedSourceRepeatLowerBoundTotal),
                static_cast<unsigned long long>(wgcCombinedExcessRepeatTotal),
                static_cast<unsigned long long>(wgcPolicyAddedRepeatTotal),
                static_cast<unsigned long long>(wgcExcessRepeatClusterTotal), wgcExcessRepeatClusterMaxTicks,
                wgcCfrSmoothnessNotMaximal ? 1 : 0, wgcActiveDelayMixedPolicyFault ? 1 : 0,
                static_cast<unsigned long long>(wgcSourceRepeatLowerBoundTotal),
                static_cast<unsigned long long>(wgcDeliveryRepeatLowerBoundTotal),
                static_cast<unsigned long long>(wgcPolicyNoSourceRepeats));
        } else {
            LogInfo(
                "[Inject CFR SUMMARY] Live=%llu Dup=%llu DupPct=%.1f%% DupReason(src=%llu def=%llu timer=%llu "
                "drain=%llu) FreshCatchup=%llu RepeatCatchup=%llu StaleTrim=%llu Recovery=%d/%llu "
                "PathMismatch=%llu/%llu "
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
                static_cast<unsigned long long>(injectLiveStaleTrimTotal), injectCfrRecoveryActive ? 1 : 0,
                static_cast<unsigned long long>(injectCfrRecoveryEpisodesTotal),
                static_cast<unsigned long long>(activePathMismatchDiscardTotal),
                static_cast<unsigned long long>(g_ActivePathMismatchFramesDiscarded.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(injectDeferredRequeuedTotal),
                static_cast<unsigned long long>(injectDeferredDroppedTotal));
            LogInfo(
                "[Inject CFR SUMMARY] SourceFps=%.2f..%.2f JitterMax=%uus SelMax=%uus EncEmaMax=%.2fms "
                "ServiceEma=%.2fms ServiceMax=%uus SustainMin=%.1ffps",
                injectWorstSourceFpsX100 == std::numeric_limits<uint32_t>::max() ? 0.0
                                                                                 : (injectWorstSourceFpsX100 / 100.0),
                injectBestSourceFpsX100 / 100.0, injectWorstSourceJitterUs, injectWorstSelectionErrorUs,
                captureSessionSummary.maxEncodeEmaMs, smoothedInjectServiceMs, injectServiceMaxUs,
                captureSessionSummary.minEncoderSustainFps == std::numeric_limits<double>::max()
                    ? 0.0
                    : captureSessionSummary.minEncoderSustainFps);
            LogInfo(
                "[Inject CFR QUALITY SUMMARY] TargetSelect=%llu Superseded=%llu TargetHold=%llu "
                "HoldWithCandidate=%llu BufferCapTrim=%llu TargetResidualMax=%uus",
                static_cast<unsigned long long>(injectTargetSelectTotal),
                static_cast<unsigned long long>(injectTargetSupersededTotal),
                static_cast<unsigned long long>(injectTargetHoldTotal),
                static_cast<unsigned long long>(injectTargetHoldWithCandidateTotal),
                static_cast<unsigned long long>(injectBufferCapTrimTotal), injectTargetResidualMaxUs);
            if (g_pSharedMem) {
                const auto& contention = g_pSharedMem->runtimeState;
                LogInfo(
                    "[Inject Contention SUMMARY] CaptureLock=%u CpuLease=%u GpuBusy=%u RingFull=%u "
                    "EventSignals=%u",
                    contention.injectProducerCaptureLockDrops.load(std::memory_order_relaxed),
                    contention.injectProducerCpuLeaseBusyDrops.load(std::memory_order_relaxed),
                    contention.injectProducerGpuBusyDrops.load(std::memory_order_relaxed),
                    contention.injectProducerMetadataFullDrops.load(std::memory_order_relaxed),
                    contention.injectFrameReadySignals.load(std::memory_order_relaxed));
            }
        }
        const auto& phaseLockSummary = summaryUsesScreenGrab ? wgcCfrPhaseLock : injectCfrPhaseLock;
        LogInfo(
            "[CFR PHASE LOCK SUMMARY] Backend=%s Enabled=%d Locked=%d Offset=%lldus Stable=%u Unstable=%u "
            "Acquire=%llu Rephase=%llu Release=%llu Multiplier=%u",
            summaryUsesScreenGrab ? "wgc" : "inject", captureSyncPhaseLockEnabled ? 1 : 0,
            phaseLockSummary.locked ? 1 : 0,
            static_cast<long long>(qpcToUs(phaseLockSummary.lockedPhaseQpc)),
            phaseLockSummary.stableSourceIntervals, phaseLockSummary.unstableSourceIntervals,
            static_cast<unsigned long long>(phaseLockSummary.acquisitions),
            static_cast<unsigned long long>(phaseLockSummary.rephases),
            static_cast<unsigned long long>(phaseLockSummary.releases), captureSyncMultiplier);
    }

    SetCapturePipelinePhase(CapturePipelinePhase::kIdle);

    LogInfo("[EncoderThread] Stopped");
}

void StartRecording(const AppConfig& config) {
    if (g_Recording)
        return;

    LogInfo("[Media] Starting recording...");

    timeBeginPeriod(1);

    // A prior interrupted session must never leave hook-side video publication
    // armed. The selected live path below explicitly enables it only for inject.
    SetInjectVideoCaptureRequestedState(false, "recording start reset");

    if (g_AudioOnly) {
        LogInfo("[Media] Audio-only recording mode - skipping video capture");

        // Clear any stale shared memory state
        if (g_pSharedMem) {
            StoreRelease(g_pSharedMem->runtimeState.cmdStartRecording, false);
            StoreRelease(g_pSharedMem->runtimeState.cmdStopRecording, false);
            StoreRelease(g_pSharedMem->runtimeState.recordingFailureCode,
                         static_cast<uint32_t>(RecordingFailureCode::None));
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
    if (IsScreenGrabCaptureMethod(config.captureMethod)) {
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

    if (!useScreenGrab && g_pSharedMem) {
        const uint32_t writeIndex = g_pSharedMem->frameRing.writeIndex.load(std::memory_order_acquire);
        const uint32_t readIndex = g_pSharedMem->frameRing.readIndex.load(std::memory_order_acquire);
        if (!IsFrameRingWindowValid(writeIndex, readIndex)) {
            LogError(
                "[Media] Inject recording rejected corrupt shared frame ring "
                "(write=%u read=%u distance=%u version=%u ABI=0x%08X)",
                writeIndex, readIndex, static_cast<uint32_t>(writeIndex - readIndex),
                g_pSharedMem->GetVersion(), g_pSharedMem->abiSignature.load(std::memory_order_acquire));
            StoreRelease(g_pSharedMem->runtimeState.recordingFailureCode,
                         static_cast<uint32_t>(RecordingFailureCode::SharedMemoryProtocolIntegrity));
            SetCaptureRequestedState(false);
            SetRecordingVisibleState(false);
            timeEndPeriod(1);
            return;
        }
    }

    // Clear any stale shared memory commands/state from previous (possibly crashed)
    // recording sessions. If a previous media process crashed, cmdStopRecording
    // may still be true, causing the new recording to stop immediately.
    if (g_pSharedMem) {
        StoreRelease(g_pSharedMem->runtimeState.cmdStartRecording, false);
        StoreRelease(g_pSharedMem->runtimeState.cmdStopRecording, false);
        StoreRelease(g_pSharedMem->runtimeState.recordingFailureCode,
                     static_cast<uint32_t>(RecordingFailureCode::None));
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

    EnsureInjectCaptureEvents();
    if (g_InjectCaptureShutdownEvent) {
        ResetEvent(g_InjectCaptureShutdownEvent);
    }
    if (g_InjectFrameReadyEvent) {
        ResetEvent(g_InjectFrameReadyEvent);
    }

    SetInjectVideoCaptureRequestedState(!useScreenGrab,
                                        useScreenGrab ? "screen-grab recording path" : "inject recording path");
    SetCaptureRequestedState(true);

    if (!MediaEngine_StartRecording || !MediaEngine_StartRecording()) {
        LogError("[Media] Failed to start MediaEngine recording");
        SetInjectVideoCaptureRequestedState(false, "recording start failure");
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

    // Recording-lifetime config snapshot (see StartWgcRecordingCapture): the
    // main thread may reassign `config` mid-recording; encoder-thread settings
    // are fixed per session by design, so it reads an owned copy.
    {
        auto configSnapshot = std::make_shared<const AppConfig>(config);
        g_EncoderThread = std::thread([configSnapshot]() { EncoderThreadFunc(*configSnapshot); });
    }

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
        LogInfo("[Media] Active recording path: %s bounded pull-drain CFR (%d fps output)",
                g_WgcCap->IsUsingDesktopDuplication() ? "DXGI-duplication" : "WGC", config.video.fps);
    } else if (!useScreenGrab) {
        if (config.captureMethod == "auto" && g_WgcCap && g_AutoWgcFallbackArmed.load(std::memory_order_acquire)) {
            LogInfo("[Media] Active recording path: inject shared-memory capture (WGC auto-fallback armed)");
        } else {
            LogInfo("[Media] Active recording path: inject shared-memory capture");
        }
        ApplyMediaGpuSchedulingPriorityForSharedAdapter(config);
        g_InjectCaptureShutdown = false;
        EnsureInjectCaptureEvents();
        // Recording-lifetime config snapshot (see StartWgcRecordingCapture).
        {
            auto configSnapshot = std::make_shared<const AppConfig>(config);
            g_InjectCaptureThread = std::thread([configSnapshot]() { InjectCaptureThreadFunc(*configSnapshot); });
        }
    }

    LogInfo("[Media] Recording warmup armed");
}

void StopRecording() {
    if (!g_Recording)
        return;

    LogInfo("[Media] Stopping recording...");

    SetInjectVideoCaptureRequestedState(false, "recording stop");

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
    } else if (wasActiveScreenGrab && recordingUsesVfr) {
        LogInfo("[Media] WGC VFR exact-stop: no CFR debt to drain");
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
        // Auto-detect the render-domain delay before Init. The product-safe path is audio-only:
        // no calibration window, no WGC/DX stimulus, and no config.ini delay writeback. Apply the
        // result (or explicit low-confidence reason) before the media engine snapshots config.
        if (!g_Recording.load(std::memory_order_acquire)) {
            MeasureRenderLatencyOnce(config, mediaCacheDir);
        }
        ApplyAutoDetectedRenderLatencyToConfig(config);

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

    ApplyMediaPrioritySettings(config);

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
    auto isExplicitDxgiDupConfig = [&]() -> bool { return IsDxgiDupCaptureMethod(config.captureMethod); };
    // Any explicit non-inject screen-grab family method (wgc or dxgi_dup).
    auto isExplicitScreenGrabConfig = [&]() -> bool { return IsScreenGrabCaptureMethod(config.captureMethod); };
    auto isAutoCaptureConfig = [&]() -> bool { return IsAutoCaptureMethod(config.captureMethod); };
    auto setWgcPreferenceAfterFailure = [&]() {
        SetPreferredScreenGrab(isExplicitScreenGrabConfig() || isAutoCaptureConfig());
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

    if (isExplicitScreenGrabConfig()) {
        SetPreferredScreenGrab(true);
        LogInfo("[Media] Using %s mode (explicit)", isExplicitDxgiDupConfig() ? "DXGI duplication" : "WGC");
    }

    LogInfo("[Media] Attempting to connect to shared memory...");

    for (int retry = 0; retry < 10 && !g_pSharedMem; retry++) {
        HANDLE hDiscovery = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
        if (hDiscovery) {
            DiscoveryInfo* pDiscovery =
                (DiscoveryInfo*)MapViewOfFile(hDiscovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));

            if (ValidateDiscoveryInfo(pDiscovery) && pDiscovery->GetInjectPid() != 0) {
                wchar_t sharedMemName[64];
                GenerateSharedMemName(sharedMemName, 64, pDiscovery->GetInjectPid());

                g_hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sharedMemName);
                if (g_hMapFile) {
                    g_pSharedMem = (SharedMemoryLayout*)MapViewOfFile(g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0,
                                                                      sizeof(SharedMemoryLayout));

                    if (g_pSharedMem && ValidateSharedMemory(g_pSharedMem) && g_pSharedMem->GetHostPID() != 0) {
                        LogInfo("[Media] Connected via discovery (inject PID: %u, ABI: 0x%08X)",
                                pDiscovery->injectPid.load(), SHARED_MEMORY_ABI_SIGNATURE);
                        UnmapViewOfFile(pDiscovery);
                        CloseHandle(hDiscovery);
                        break;
                    }

                    if (g_pSharedMem) {
                        LogError(
                            "[Media] Rejected shared memory header: magic=0x%08X version=%u size=%u abi=0x%08X "
                            "expected=(0x%08X,%u,%zu,0x%08X)",
                            g_pSharedMem->GetMagic(), g_pSharedMem->GetVersion(),
                            g_pSharedMem->structSize.load(std::memory_order_acquire),
                            g_pSharedMem->abiSignature.load(std::memory_order_acquire), SHARED_MEMORY_MAGIC,
                            SHARED_MEMORY_VERSION, sizeof(SharedMemoryLayout), SHARED_MEMORY_ABI_SIGNATURE);
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

        if (isExplicitScreenGrabConfig()) {
            SetPreferredScreenGrab(true);
            LogInfo("[Media] Connected to shared memory - using %s for capture",
                    isExplicitDxgiDupConfig() ? "DXGI duplication" : "WGC");
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

    auto applyWgcOptions = [&](WGCCapture* capture) {
        if (!capture) {
            return;
        }
        capture->SetSkipSplitDeviceFlush(config.wgcSkipSplitDeviceFlush);
        capture->SetSameDeviceCapture(config.wgcSameDeviceCapture);
        capture->SetAllowLossyBgra8Pool(config.wgcAllowLossyBgra8Pool);
        capture->SetVideoMemoryReservationMode(config.wgcVideoMemoryReservation);
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
            ApplyMediaGpuSchedulingPriorityForDevice(config, d3dDevice);
            d3dDevice->GetImmediateContext(&d3dContext);

            if (WGCCapture::IsSupported()) {
                auto capture = std::make_shared<WGCCapture>();
                applyWgcOptions(capture.get());
                if (capture->Init(d3dDevice)) {
                    // Connect encoder bottleneck flag to WGC for throttle
                    capture->SetThrottleFlag(nullptr);
                    PublishWgcCapture(std::move(capture), "initial WGC setup");
                    LogInfo("[Media] WGC support initialized%s",
                            IsPreferredScreenGrab() ? "" : " (standby for auto fallback)");
                } else {
                    if (IsPreferredScreenGrab()) {
                        LogError("[Media] WGC capture init failed");
                        unloadMediaEngineIdle();
                        return 1;
                    } else {
                        LogInfo("[Media] WGC init failed - inject mode only");
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
        ApplyMediaGpuSchedulingPriorityForDevice(config, d3dDevice);
        if (!d3dContext) {
            d3dDevice->GetImmediateContext(&d3dContext);
        }
        return true;
    };
    auto releaseIdleWgcResources = [&]() {
        StopWgcCapturePipeline();
        PublishWgcCapture(nullptr, "idle WGC resource release");
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
    ce::capture_handoff::InjectToWgcHandoff autoWgcHandoff;
    uint32_t autoWgcHandoffBaselineFrames = 0;
    uint64_t autoWgcHandoffDeadlineTick = 0;
    constexpr uint64_t kAutoWgcHandoffReadyTimeoutMs = 2000;

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
        // Normalize runtime-only sync state before comparing with the active config. Otherwise a
        // config.ini reload with no real media changes would compare file defaults (usually 0 ms)
        // against the active auto-detected render latency and reload unnecessarily.
        ApplyAutoDetectedRenderLatencyToConfig(resolvedConfig);

        const bool mediaConfigChanged = !MediaEngineConfigEquals(config, resolvedConfig);

        config = std::move(resolvedConfig);
        Log_SetLevel(config.logLevel);
        activeConfigSourcePid = sourcePid;
        activeConfigProcessName = processName;

        ApplyMediaPrioritySettings(config);
        if (d3dDevice) {
            ApplyMediaGpuSchedulingPriorityForDevice(config, d3dDevice);
        } else {
            ApplyMediaGpuSchedulingPriorityForSharedAdapter(config);
        }
        if (auto capture = g_WgcCap.Read()) {
            applyWgcOptions(capture.get());
            capture->SetCaptureCursor(ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
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
        if (isExplicitScreenGrabConfig()) {
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

        // Monitor-scope backend priority: DXGI Desktop Duplication is the
        // preferred pure desktop/monitor capture path (explicit dxgi_dup, or
        // auto mode where no inject/window target exists). Explicit wgc keeps
        // the WGC monitor item; duplication failures always fall back to WGC.
        const bool preferDuplication = ce::capture_policy::ShouldPreferDxgiDuplicationForMonitorCapture(
            isExplicitDxgiDupConfig(), isExplicitWgcConfig(), isAutoCaptureConfig());

        {
            const auto existingCapture = g_WgcCap.Read();
            if (targetMonitor == NULL && currentCapturedWindow == NULL && !currentTargetPrefersInject &&
                existingCapture && existingCapture->IsUsingDesktopDuplication() == preferDuplication) {
                applyWgcOptions(existingCapture.get());
                existingCapture->SetCaptureCursor(
                    ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
                existingCapture->SetThrottleFlag(nullptr);
                SetPreferredScreenGrab(true);
                return true;
            }
        }

        if (!ensureWgcDevice()) {
            return false;
        }

        auto capture = std::make_shared<WGCCapture>();
        applyWgcOptions(capture.get());
        bool initOk = false;
        if (preferDuplication) {
            initOk = capture->InitForMonitorDuplication(d3dDevice, targetMonitor);
            if (initOk) {
                LogInfo("[Media] Monitor capture backend selected: DXGI duplication (%s)",
                        isExplicitDxgiDupConfig() ? "explicit capture_method=dxgi_dup" : "auto desktop fallback");
            } else {
                LogWarn(
                    "[Media] DXGI duplication monitor target unavailable (monitor=0x%p); "
                    "falling back to WGC monitor capture",
                    targetMonitor);
            }
        }
        if (!initOk && targetMonitor) {
            initOk = capture->InitForMonitor(d3dDevice, targetMonitor);
            if (!initOk) {
                LogWarn("[Media] Failed to init WGC for monitor 0x%p, falling back to primary", targetMonitor);
            }
        }
        if (!initOk) {
            initOk = capture->Init(d3dDevice);
        }
        if (!initOk) {
            return false;
        }

        capture->SetCaptureCursor(ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
        capture->SetThrottleFlag(nullptr);
        PublishWgcCapture(std::move(capture), "monitor retarget");
        SetPreferredScreenGrab(true);
        currentCapturedWindow = NULL;
        currentTargetPrefersInject = false;
        return true;
    };

    // Fullscreen-game duplication priming: capture the MONITOR that hosts the
    // game window through DXGI duplication so the live hardware cursor plane
    // is preserved (WGC sessions demote the cursor to DWM-composed rendering;
    // see wgc-capture.md). Caller falls back to WGC window capture on failure
    // (cross-adapter or rotated output).
    auto primeDxgiDupForWindowMonitor = [&](HWND targetWindow, const char* reason) -> bool {
        if (isExplicitInjectConfig() || !targetWindow) {
            return false;
        }

        HMONITOR targetMonitor = MonitorFromWindow(targetWindow, MONITOR_DEFAULTTONEAREST);
        if (!targetMonitor) {
            return false;
        }

        if (!ensureWgcDevice()) {
            return false;
        }

        auto capture = std::make_shared<WGCCapture>();
        applyWgcOptions(capture.get());
        if (!capture->InitForMonitorDuplication(d3dDevice, targetMonitor)) {
            LogWarn(
                "[Media] DXGI duplication unavailable for fullscreen target's monitor "
                "(%s hwnd=0x%p hmon=0x%p); using WGC window capture instead",
                reason, targetWindow, targetMonitor);
            return false;
        }

        capture->SetCaptureCursor(ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
        capture->SetThrottleFlag(nullptr);
        PublishWgcCapture(std::move(capture), "fullscreen duplication retarget");
        SetPreferredScreenGrab(true);
        currentCapturedWindow = NULL;
        currentTargetPrefersInject = false;
        LogInfo(
            "[Media] Auto mode: fullscreen game target captured via DXGI duplication of its monitor "
            "(hardware cursor preserved) (%s hwnd=0x%p hmon=0x%p)",
            reason, targetWindow, targetMonitor);
        return true;
    };

    auto primeWgcWindowTarget = [&](HWND targetWindow, bool logPrimed, bool allowMonitorFallback = true) -> bool {
        if (isExplicitInjectConfig()) {
            return false;
        }

        if (!targetWindow) {
            return false;
        }

        {
            const auto existingCapture = g_WgcCap.Read();
            if (currentCapturedWindow == targetWindow && existingCapture && !currentTargetPrefersInject) {
                applyWgcOptions(existingCapture.get());
                existingCapture->SetCaptureCursor(
                    ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
                existingCapture->SetThrottleFlag(nullptr);
                SetPreferredScreenGrab(true);
                return true;
            }
        }

        if (!ensureWgcDevice()) {
            setWgcPreferenceAfterFailure();
            return false;
        }

        auto capture = std::make_shared<WGCCapture>();
        applyWgcOptions(capture.get());
        if (capture->InitForWindow(d3dDevice, targetWindow)) {
            capture->SetCaptureCursor(ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
            capture->SetThrottleFlag(nullptr);
            PublishWgcCapture(std::move(capture), "window retarget");
            SetPreferredScreenGrab(true);
            currentCapturedWindow = targetWindow;
            currentTargetPrefersInject = false;
            if (logPrimed) {
                LogInfo("[Media] WGC target primed for window 0x%p", targetWindow);
            }
            return true;
        }

        LogError("[Media] Failed to init WGC for found window 0x%p.", targetWindow);
        currentCapturedWindow = NULL;
        currentTargetPrefersInject = false;
        if (!allowMonitorFallback) {
            return false;
        }

        LogWarn("[Media] Falling back to WGC monitor capture after window init failure");
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
        const auto previousCapture = g_WgcCap.Load();
        const HWND previousCapturedWindow = currentCapturedWindow;
        const bool previousTargetPrefersInject = currentTargetPrefersInject;
        const bool previousPreferredScreenGrab = IsPreferredScreenGrab();
        if (restartActiveCapture) {
            StopWgcCapturePipeline();
        }

        auto restorePreviousCapture = [&](const char* failureReason) -> bool {
            if (!previousCapture) {
                LogError("[Media] WGC retarget rollback unavailable (%s): no previous capture", failureReason);
                return false;
            }
            const auto publishedCapture = g_WgcCap.Load();
            if (publishedCapture.get() != previousCapture.get()) {
                PublishWgcCapture(previousCapture, "retarget rollback");
            }
            currentCapturedWindow = previousCapturedWindow;
            currentTargetPrefersInject = previousTargetPrefersInject;
            SetPreferredScreenGrab(previousPreferredScreenGrab);
            if (restartActiveCapture && !StartWgcRecordingCapture(config)) {
                LogError("[Media] WGC retarget rollback failed to restart previous source (%s)", failureReason);
                return false;
            }
            LogWarn("[Media] WGC retarget rolled back to previous source (%s)", failureReason);
            return true;
        };

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
            LogWarn("[Media] Failed to initialize queued WGC retarget; restoring previous source");
            restorePreviousCapture("replacement initialization failed");
            return false;
        }

        if (restartActiveCapture) {
            if (!StartWgcRecordingCapture(config)) {
                LogError("[Media] Failed to start replacement WGC capture; restoring previous source");
                restorePreviousCapture("replacement start failed");
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

        autoWgcHandoff.Reset();
        autoWgcHandoffBaselineFrames = 0;
        autoWgcHandoffDeadlineTick = 0;
        g_AutoWgcFallbackArmed.store(false, std::memory_order_release);

        if (isExplicitInjectConfig()) {
            SetPreferredScreenGrab(false);
            clearCurrentWgcTarget();
            return;
        }

        if (isAutoCaptureConfig() && injectWhitelisted) {
            // Auto mode prefers inject for known-compatible games, but a hook
            // connection is not proof that frames will arrive. Keep a fully
            // initialized screen-grab target for the SAME game/window/monitor
            // as a startup fallback. The generic primary-monitor standby is not
            // sufficient on multi-monitor systems.
            bool fallbackReady = false;
            HWND fallbackWindow = sourcePid != 0 ? GetMainWindowForProcess(sourcePid) : NULL;
            if (!fallbackWindow) {
                const ForegroundWgcWindowCandidate candidate = GetForegroundWgcWindowCandidate();
                // A foreground window is a valid fallback only when it belongs
                // to the requested source process (or no source PID is known).
                // Otherwise a transiently missing game window could silently
                // redirect an auto recording to an unrelated foreground app.
                if (candidate.usable && (sourcePid == 0 || candidate.pid == sourcePid)) {
                    fallbackWindow = candidate.hwnd;
                }
            }
            HMONITOR fallbackMonitor =
                fallbackWindow ? MonitorFromWindow(fallbackWindow, MONITOR_DEFAULTTONEAREST) : NULL;
            if (fallbackWindow && IsWindowFullscreenLike(fallbackWindow) && config.autoFullscreenPrefersDxgiDup) {
                fallbackReady = primeDxgiDupForWindowMonitor(fallbackWindow, "inject startup fallback");
            }
            if (!fallbackReady && fallbackWindow) {
                fallbackReady = primeWgcWindowTarget(fallbackWindow, false, false);
            }
            if (!fallbackReady && fallbackMonitor && WGCCapture::IsSupported()) {
                fallbackReady = primeWgcMonitorTarget(fallbackMonitor);
            }
            if (!fallbackReady && !fallbackWindow) {
                LogWarn(
                    "[Media] Auto inject fallback could not resolve a window/monitor for source PID %lu; "
                    "leaving fallback unarmed instead of capturing an unrelated primary monitor",
                    static_cast<unsigned long>(sourcePid));
            }
            g_AutoWgcFallbackArmed.store(fallbackReady, std::memory_order_release);
            SetPreferredScreenGrab(false);
            LogInfo(
                "[Media] Injection whitelist matched %s; auto mode will use inject capture "
                "(WGC fallback=%s hwnd=0x%p hmon=0x%p)",
                processName.c_str(), fallbackReady ? "armed" : "unavailable", fallbackWindow, fallbackMonitor);
            return;
        }

        if (!config.wgcWindowTitles.empty()) {
            if (isExplicitDxgiDupConfig()) {
                LogInfo("[Media] wgc_window_detection ignored: capture_method=dxgi_dup is monitor-scope only");
            } else {
                HWND matchedWindow = FindMatchingWgcWindow(config.wgcWindowTitles);
                if (matchedWindow) {
                    if (markInjectPreferredTarget(matchedWindow, sourcePid, "WGC title match")) {
                        return;
                    }
                    if (primeWgcWindowTarget(matchedWindow, false, false)) {
                        LogInfo(
                            "[Media] WGC window detection matched configured target; WGC window capture selected "
                            "(hwnd=0x%p)",
                            matchedWindow);
                        return;
                    }
                    LogWarn(
                        "[Media] WGC configured window target 0x%p failed to initialize; continuing fallback selection",
                        matchedWindow);
                }
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
            HWND hGameWindow = isExplicitDxgiDupConfig() ? NULL : GetMainWindowForProcess(sourcePid);
            if (hGameWindow) {
                LogInfo("[Media] Overlay-only hook target %s; WGC capture selected", processName.c_str());
                if (!primeWgcWindowTarget(hGameWindow, false, false)) {
                    LogWarn("[Media] Overlay-only WGC window target 0x%p failed to initialize; falling back to monitor",
                            hGameWindow);
                    if (!primeWgcMonitorTarget()) {
                        setWgcPreferenceAfterFailure();
                        clearCurrentWgcTarget();
                    }
                }
            } else if (!primeWgcMonitorTarget()) {
                setWgcPreferenceAfterFailure();
                clearCurrentWgcTarget();
            }
            return;
        }

        if (isExplicitScreenGrabConfig()) {
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
                if (hGameWindow &&
                    ce::capture_policy::ShouldPreferDxgiDuplicationForFullscreenAutoTarget(
                        isAutoCaptureConfig(), isExplicitInjectConfig(), injectWhitelisted,
                        IsWindowFullscreenLike(hGameWindow), config.autoFullscreenPrefersDxgiDup) &&
                    primeDxgiDupForWindowMonitor(hGameWindow, "unhooked source window")) {
                    return;
                }
                if (hGameWindow && primeWgcWindowTarget(hGameWindow, false, false)) {
                    LogInfo("[Media] Auto mode: %s is not on the inject whitelist; WGC window capture selected",
                            processName.empty() ? "target" : processName.c_str());
                    return;
                }
            }

            ForegroundWgcWindowCandidate foregroundCandidate = GetForegroundWgcWindowCandidate();
            const bool matchedConfiguredWgcWindow = false;
            if (ce::capture_policy::ShouldPreferForegroundFullscreenWindowForAutoWgc(
                    isAutoCaptureConfig(), isExplicitInjectConfig(), injectWhitelisted, sourcePid != 0,
                    matchedConfiguredWgcWindow, foregroundCandidate.usable, foregroundCandidate.fullscreenLike)) {
                if (ce::capture_policy::ShouldPreferDxgiDuplicationForFullscreenAutoTarget(
                        isAutoCaptureConfig(), isExplicitInjectConfig(), injectWhitelisted,
                        foregroundCandidate.fullscreenLike, config.autoFullscreenPrefersDxgiDup) &&
                    primeDxgiDupForWindowMonitor(foregroundCandidate.hwnd, "foreground fullscreen window")) {
                    return;
                }
                if (primeWgcWindowTarget(foregroundCandidate.hwnd, false, false)) {
                    LogInfo(
                        "[Media] Auto mode: no source PID; foreground fullscreen WGC window capture selected "
                        "(pid=%lu process=%s hwnd=0x%p)",
                        static_cast<unsigned long>(foregroundCandidate.pid), foregroundCandidate.processName.c_str(),
                        foregroundCandidate.hwnd);
                    return;
                }
                LogWarn(
                    "[Media] Auto mode: foreground fullscreen WGC window init failed "
                    "(pid=%lu process=%s hwnd=0x%p); falling back to monitor capture",
                    static_cast<unsigned long>(foregroundCandidate.pid), foregroundCandidate.processName.c_str(),
                    foregroundCandidate.hwnd);
            } else if (sourcePid == 0) {
                LogInfo(
                    "[Media] Auto mode: foreground WGC window candidate not used "
                    "(usable=%d fullscreenLike=%d matchedConfiguredWindow=%d configuredEntries=%zu)",
                    foregroundCandidate.usable ? 1 : 0, foregroundCandidate.fullscreenLike ? 1 : 0,
                    matchedConfiguredWgcWindow ? 1 : 0, config.wgcWindowTitles.size());
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
                        if (!primeWgcWindowTarget(matchedWindow, false, false)) {
                            LogWarn("[Media] Early WGC window scan matched 0x%p but window init failed", matchedWindow);
                        }
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
        if (ipc.HasFatalDisconnect()) {
            LogWarn("[Media] Controller IPC disconnected; stopping for a clean respawn");
            if (g_Recording)
                StopRecording();
            g_Running = false;
            break;
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
            if (!g_Recording && !isExplicitInjectConfig() && !isExplicitDxgiDupConfig() &&
                !config.wgcWindowTitles.empty() && (now - lastWindowScanTime > 1000)) {
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
                    if (!primeWgcWindowTarget(foundWindow, foundWindow != currentCapturedWindow, false)) {
                        LogWarn("[Media] WGC trigger window 0x%p failed to initialize; falling back to monitor target",
                                foundWindow);
                        if (!primeWgcMonitorTarget()) {
                            LogWarn("[Media] WGC trigger ignored: D3D11 device unavailable");
                        }
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
                    g_InjectDeliveredFirstFrame.load(std::memory_order_acquire) &&
                    g_AutoWgcFallbackArmed.exchange(false, std::memory_order_acq_rel)) {
                    LogInfo("[Media] Inject delivery confirmed for %s; disarming WGC startup fallback",
                            procName.c_str());
                }

                if (!g_Recording && forceWGC) {
                    HWND matchedWindow = (config.wgcWindowTitles.empty() || isExplicitDxgiDupConfig())
                                             ? NULL
                                             : FindMatchingWgcWindow(config.wgcWindowTitles);
                    if (matchedWindow) {
                        if (primeWgcWindowTarget(matchedWindow, false, false)) {
                            continue;
                        }
                        LogWarn(
                            "[Media] WGC configured window target 0x%p failed during pre-record scan; "
                            "continuing fallback selection",
                            matchedWindow);
                    }

                    if (!ensureWgcDevice()) {
                        LogWarn("[Media] Overlay whitelist requested WGC but D3D11 device unavailable");
                        setWgcPreferenceAfterFailure();
                        clearCurrentWgcTarget();
                    } else {
                        SetPreferredScreenGrab(true);

                        HWND hGameWindow = isExplicitDxgiDupConfig() ? NULL : GetMainWindowForProcess(currentSourcePid);
                        if (hGameWindow) {
                            LogInfo(
                                "[Media] Overlay-only target: found main window 0x%p. "
                                "Switching WGC to window mode.",
                                hGameWindow);

                            if (primeWgcWindowTarget(hGameWindow, false, false)) {
                                LogInfo("[Media] WGC window target primed for PID %u", currentSourcePid);
                            } else if (primeWgcMonitorTarget()) {
                                LogWarn("[Media] Failed to init WGC for window - falling back to monitor capture");
                            } else {
                                LogError("[Media] Failed to init WGC for window or monitor");
                                setWgcPreferenceAfterFailure();
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
                    if (hGameWindow &&
                        ce::capture_policy::ShouldPreferDxgiDuplicationForFullscreenAutoTarget(
                            isAutoCaptureConfig(), isExplicitInjectConfig(), injectWhitelisted,
                            IsWindowFullscreenLike(hGameWindow), config.autoFullscreenPrefersDxgiDup) &&
                        primeDxgiDupForWindowMonitor(hGameWindow, "unhooked connected source window")) {
                        // Selected duplication of the game's monitor.
                    } else if (hGameWindow && primeWgcWindowTarget(hGameWindow, false, false)) {
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
                } else if (!g_Recording && !isExplicitScreenGrabConfig()) {
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

            if (autoWgcHandoff.GetPhase() == ce::capture_handoff::Phase::kStarting) {
                if (receivedFirstFrame) {
                    const auto transition = autoWgcHandoff.OnInjectFrame();
                    if (transition.action == ce::capture_handoff::Action::kStopWgcKeepInject) {
                        StopWgcCapturePipeline();
                        g_RejectInjectFrames.store(false, std::memory_order_release);
                        LogInfo(
                            "[Media] Inject delivered while WGC fallback was warming; WGC standby stopped and "
                            "inject remains authoritative");
                    }
                } else {
                    bool wgcReady = false;
                    bool wgcFailed = false;
                    uint32_t observedFrames = 0;
                    if (auto capture = g_WgcCap.Read()) {
                        observedFrames = capture->GetCallbackFrameCount();
                        wgcFailed = !capture->IsCapturing() || capture->NeedsReset();
                        wgcReady = !wgcFailed && (config.video.useVFR ? HasStandbyWgcHandoffFrame()
                                                                      : observedFrames > autoWgcHandoffBaselineFrames);
                    } else {
                        wgcFailed = true;
                    }

                    if (wgcReady) {
                        const auto transition = autoWgcHandoff.OnWgcFirstFrame();
                        if (transition.action == ce::capture_handoff::Action::kCommitWgcStopInject) {
                            // Invalidate every cached/queued frame from the old
                            // inject/WGC lineage before the encoder observes the
                            // new active path. The CFR/audio clock itself is not
                            // restarted.
                            // Keep the standby capture's publication epoch. Its
                            // proven first frame already carries that identity;
                            // advancing here would discard the proof, clear the
                            // inject repeat cache, and create a handoff hole
                            // before any replacement-epoch frame exists.
                            g_RejectInjectFrames.store(true, std::memory_order_release);
                            g_RetainStandbyWgcFrameForHandoff.store(false, std::memory_order_release);
                            QueuedFrame retainedVfrFrame;
                            const bool hasRetainedVfrFrame =
                                config.video.useVFR && TakeStandbyWgcHandoffFrame(retainedVfrFrame);
                            SetActiveScreenGrab(true);
                            SetInjectVideoCaptureRequestedState(false, "auto inject-to-WGC handoff committed");
                            if (hasRetainedVfrFrame) {
                                SubmitWgcQueuedFrame(std::move(retainedVfrFrame));
                            }
                            StopInjectCapturePipeline();
                            LogInfo(
                                "[Media] Active recording path switched to WGC after first-frame proof "
                                "(inputFrames=%u); inject stopped only after the replacement was delivering",
                                observedFrames);
                        }
                    } else if (wgcFailed) {
                        const auto transition = autoWgcHandoff.OnWgcFailure();
                        if (transition.action == ce::capture_handoff::Action::kStopWgcKeepInject) {
                            StopWgcCapturePipeline();
                            g_RejectInjectFrames.store(false, std::memory_order_release);
                            LogWarn(
                                "[Media] WGC fallback stopped before first-frame proof; inject capture remains "
                                "active");
                        }
                    } else if (GetTickCount64() >= autoWgcHandoffDeadlineTick) {
                        const auto transition = autoWgcHandoff.OnWgcReadinessTimeout();
                        if (transition.action == ce::capture_handoff::Action::kStopWgcKeepInject) {
                            StopWgcCapturePipeline();
                            g_RejectInjectFrames.store(false, std::memory_order_release);
                            LogWarn(
                                "[Media] WGC fallback produced no frame within %llums; inject capture remains "
                                "active",
                                static_cast<unsigned long long>(kAutoWgcHandoffReadyTimeoutMs));
                        }
                    }
                }
            }

            if (!receivedFirstFrame && autoWgcHandoff.GetPhase() == ce::capture_handoff::Phase::kIdle &&
                isAutoCaptureConfig() && g_AutoWgcFallbackArmed.load(std::memory_order_acquire) && g_WgcCap) {
                DWORD elapsed = GetTickCount() - injectModeStartTime;
                const uint32_t activeSourcePid = g_pSharedMem ? g_pSharedMem->GetSourcePid() : 0;
                if (ce::capture_policy::ShouldTriggerAutoWgcFallback(
                        receivedFirstFrame, isAutoCaptureConfig(),
                        g_AutoWgcFallbackArmed.load(std::memory_order_acquire), g_WgcCap != nullptr, elapsed,
                        activeSourcePid)) {
                    LogInfo("[Media] No frames from inject mode after %lums - starting WGC standby handoff", elapsed);

                    const auto transition = autoWgcHandoff.Begin();
                    g_AutoWgcFallbackArmed.store(false, std::memory_order_release);
                    ClearStandbyWgcHandoffFrame();
                    g_RetainStandbyWgcFrameForHandoff.store(config.video.useVFR, std::memory_order_release);
                    if (transition.action == ce::capture_handoff::Action::kStartWgcKeepInject &&
                        StartWgcRecordingCapture(config)) {
                        autoWgcHandoffBaselineFrames = 0;
                        autoWgcHandoffDeadlineTick = GetTickCount64() + kAutoWgcHandoffReadyTimeoutMs;
                        LogInfo(
                            "[Media] WGC fallback session started; inject remains active pending first-frame "
                            "proof (timeout=%llums)",
                            static_cast<unsigned long long>(kAutoWgcHandoffReadyTimeoutMs));
                    } else {
                        g_RetainStandbyWgcFrameForHandoff.store(false, std::memory_order_release);
                        ClearStandbyWgcHandoffFrame();
                        autoWgcHandoff.OnWgcFailure();
                        g_RejectInjectFrames.store(false, std::memory_order_release);
                        LogWarn("[Media] WGC fallback failed to start; inject capture remains active");
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

    if (auto capture = g_WgcCap.LockExclusive()) {
        capture->StopCapture();
    }
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

    // Release every remaining metadata/resource lease while the cross-process
    // mapping is still valid. Normal recording stop already does this, but the
    // process-exit path also covers partial startup failures and shutdowns that
    // occur before a recording becomes active.
    g_FrameQueue.Clear();
    ClearStandbyWgcHandoffFrame();
    ResetLastQueuedFrameCache();

    if (g_InjectFrameReadyEvent) {
        CloseHandle(g_InjectFrameReadyEvent);
        g_InjectFrameReadyEvent = NULL;
    }
    if (g_InjectCaptureShutdownEvent) {
        CloseHandle(g_InjectCaptureShutdownEvent);
        g_InjectCaptureShutdownEvent = NULL;
    }

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
