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
#include "../common/monitor_selection.h"
#include "../common/process_ipc.h"
#include "../common/rate_window_utils.h"
#include "../common/recording_lifecycle.h"
#include "../common/screen_grab_privacy.h"
#include "../common/secure_dll_loading.h"
#include "../common/shared_defs.h"
#include "../common/thread_power_throttling_compat.h"
#include "../common/thread_wait.h"
#include "capture_cadence_diagnostics.h"
#include "mediaengine_loader.h"
#include "recording_manifest.h"
#include "screen_grab_privacy_runtime.h"
#include "wgc_capture.h"
#include "windows_gpu_scheduling.h"

using ce::screen_grab_privacy::GetWindowClientRectInScreen;
using ce::screen_grab_privacy::IsWindowFullscreenLike;

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
static std::atomic<bool> g_PrivacyFailClosedStopRequested{false};
static std::atomic<int64_t> g_CfrDrainStopQpc{0};
static std::atomic<uint32_t> g_RecordingHealthFlags{0};
static std::atomic<uint32_t> g_RecordingTimelineDebtMs{0};
static std::atomic<uint32_t> g_RecordingPeakTimelineDebtMs{0};
static std::atomic<uint32_t> g_RecordingCapacityAttributedDebtMs{0};
static std::string g_RecordingManifestLogPath;

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
// Preserve several seconds even with a 1000 Hz DXGI hardware pointer so normal
// delayed screen-grab targets retain the source history they still need.
static ce::cursor::Timeline g_WgcCursorTimeline(8192);
static ce::cursor::Timeline g_InjectCursorTimeline(1024);
static std::atomic<uint64_t> g_DxgiCursorTimelinePublished{0};
static std::mutex g_WgcCursorPublicationMutex;

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
                                                      uint32_t captureWidth, uint32_t captureHeight,
                                                      bool sourceEmbedded) {
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
    if ((cursorInfo.flags & CURSOR_SUPPRESSED) != 0) {
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
    state.SetSourceEmbedded(sourceEmbedded);

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
    // Cursor history is source-owned just like retained textures. Do not let a
    // newly published duplication source select exact-QPC samples from the
    // retired monitor/window epoch before its first pointer update arrives.
    {
        std::lock_guard<std::mutex> lock(g_WgcCursorPublicationMutex);
        g_WgcCursorTimeline.Clear();
        g_DxgiCursorTimelinePublished.store(0, std::memory_order_release);
    }
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
bool StartRecording(const AppConfig& config);

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
           lhs.maxBitrate == rhs.maxBitrate && lhs.bufferSize == rhs.bufferSize &&
           lhs.keyframeInterval == rhs.keyframeInterval &&
           lhs.preset == rhs.preset && lhs.tuning == rhs.tuning && lhs.multipass == rhs.multipass &&
           lhs.splitEncode == rhs.splitEncode && lhs.profile == rhs.profile && lhs.lookahead == rhs.lookahead &&
           lhs.spatialAq == rhs.spatialAq && lhs.temporalAq == rhs.temporalAq &&
           lhs.aqStrength == rhs.aqStrength && lhs.bFrames == rhs.bFrames && lhs.bRefMode == rhs.bRefMode &&
           lhs.customOptions == rhs.customOptions &&
           lhs.captureCursor == rhs.captureCursor && lhs.qp == rhs.qp && lhs.amfUsage == rhs.amfUsage &&
           lhs.amfPreset == rhs.amfPreset && lhs.amfQp == rhs.amfQp && lhs.amfAsyncDepth == rhs.amfAsyncDepth &&
           lhs.amfPreencode == rhs.amfPreencode && lhs.amfPreanalysis == rhs.amfPreanalysis &&
           lhs.amfLookahead == rhs.amfLookahead && lhs.amfSpatialAq == rhs.amfSpatialAq &&
           lhs.amfTemporalAq == rhs.amfTemporalAq && lhs.amfAqStrength == rhs.amfAqStrength &&
           lhs.amfHighMotionQualityBoost == rhs.amfHighMotionQualityBoost &&
           lhs.amfBRefMode == rhs.amfBRefMode && lhs.amfEnforceHrd == rhs.amfEnforceHrd &&
           lhs.amfFillerData == rhs.amfFillerData && lhs.qsvPreset == rhs.qsvPreset && lhs.qsvQp == rhs.qsvQp &&
           lhs.qsvAsyncDepth == rhs.qsvAsyncDepth && lhs.qsvLowPower == rhs.qsvLowPower &&
           lhs.qsvLookahead == rhs.qsvLookahead && lhs.qsvMbbRc == rhs.qsvMbbRc && lhs.qsvExtBrc == rhs.qsvExtBrc &&
           lhs.qsvAdaptiveI == rhs.qsvAdaptiveI && lhs.qsvAdaptiveB == rhs.qsvAdaptiveB &&
           lhs.qsvLowDelayBrc == rhs.qsvLowDelayBrc && lhs.qsvScenario == rhs.qsvScenario &&
           lhs.mfRateControl == rhs.mfRateControl && lhs.mfQuality == rhs.mfQuality &&
           lhs.mfScenario == rhs.mfScenario && lhs.mfHwEncoding == rhs.mfHwEncoding &&
           lhs.mfQualityVsSpeed == rhs.mfQualityVsSpeed && lhs.mfLowLatency == rhs.mfLowLatency &&
           lhs.gpuPriority == rhs.gpuPriority && lhs.bitDepth == rhs.bitDepth && lhs.colorSpace == rhs.colorSpace &&
           lhs.colorRange == rhs.colorRange && lhs.chromaSubsampling == rhs.chromaSubsampling &&
           lhs.hdrNominalPeakNits == rhs.hdrNominalPeakNits &&
           lhs.useVFR == rhs.useVFR && lhs.useVFR_AudioSync == rhs.useVFR_AudioSync &&
           MediaScalingConfigEquals(lhs.scaling, rhs.scaling);
