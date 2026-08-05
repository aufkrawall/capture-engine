#pragma once

struct WgcRuntimeLogSnapshot;

struct WgcRetargetRequest;

struct WindowSearch;

struct ForegroundWgcWindowCandidate;

class ScopedMmcssTask;

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

#include "status_overlay_sync.h"

#include "wgc_capture.h"

#include "windows_gpu_scheduling.h"

using ce::screen_grab_privacy::GetWindowClientRectInScreen;

using ce::screen_grab_privacy::IsWindowFullscreenLike;

#ifdef _MSC_VER
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "winmm.lib")
#endif

BOOL WINAPI MediaConsoleHandler(DWORD ctrlType);

void InjectCaptureThreadFunc(const AppConfig& config);

void WgcCaptureThreadFunc(const AppConfig& config);

void StopRecording();

bool StartRecording(const AppConfig& config);

void MediaLogCallback(const char* msg);

extern std::string GetProcessNameFromPID(DWORD pid);

void InjectCaptureThreadFunc(const AppConfig& config);

void WgcCaptureThreadFunc(const AppConfig& config);

void EncoderThreadFunc(const AppConfig& config);

bool StartRecording(const AppConfig& config);

void StopRecording();

int MediaProcessMain(const AppConfig& initialConfig);

inline std::atomic<bool> media_main_g_Running{true};

inline std::atomic<bool> media_main_g_Recording{false};

inline std::atomic<bool> media_main_g_EncoderRunning{false};

inline std::atomic<bool> media_main_g_IsEncoderBottlenecked{false};

inline std::atomic<bool> media_main_g_RecordingUsesVfr{false};

inline std::atomic<bool> media_main_g_DrainOutstandingCfrTicks{false};

inline std::atomic<bool> media_main_g_PrivacyFailClosedStopRequested{false};

inline std::atomic<int64_t> media_main_g_CfrDrainStopQpc{0};

inline std::atomic<uint32_t> media_main_g_RecordingHealthFlags{0};

inline std::atomic<uint32_t> media_main_g_RecordingTimelineDebtMs{0};

inline std::atomic<uint32_t> media_main_g_RecordingPeakTimelineDebtMs{0};

inline std::atomic<uint32_t> media_main_g_RecordingCapacityAttributedDebtMs{0};

inline std::string media_main_g_RecordingManifestLogPath;

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline FrameQueue media_main_g_FrameQueue(32);

inline std::mutex media_main_g_StandbyWgcFrameMutex;

inline QueuedFrame media_main_g_StandbyWgcFrame;

inline bool media_main_g_HasStandbyWgcFrame = false;

inline std::atomic<bool> media_main_g_RetainStandbyWgcFrameForHandoff{false};

inline std::thread media_main_g_EncoderThread;

inline QueuedFrame media_main_g_LastFrame;

inline bool media_main_g_HasLastFrame = false;

inline std::atomic<uint64_t> media_main_g_InjectDeferredFrames{0};

// Screengrab mode components
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline ce::AtomicSharedOwner<WGCCapture> media_main_g_WgcCap;

inline std::atomic<uint64_t> media_main_g_WgcSourceEpoch{0};

inline std::atomic<bool> media_main_g_UseScreenGrab{false};     // Active capture mode for the current recording

inline std::atomic<bool> media_main_g_PreferScreenGrab{false};  // Preferred mode for the next recording

// Shared memory for hook communication
inline HANDLE media_main_g_hMapFile = NULL;

inline SharedMemoryLayout* media_main_g_pSharedMem = nullptr;

inline HANDLE media_main_g_hMapShmem = NULL;

inline ShmemBuffer* media_main_g_pShmem = nullptr;

// Audio-only recording flag (set via IPC or shared memory)
inline bool media_main_g_AudioOnly = false;

// Inject thread specific
inline std::atomic<bool> media_main_g_InjectCaptureRunning{false};

inline std::atomic<bool> media_main_g_InjectCaptureShutdown{false};

inline std::atomic<bool> media_main_g_InjectSessionReset{true};  // Set true on StartRecording to reset inject session state

inline std::thread media_main_g_InjectCaptureThread;

inline HANDLE media_main_g_InjectFrameReadyEvent = NULL;

inline HANDLE media_main_g_InjectCaptureShutdownEvent = NULL;

// WGC thread specific
inline std::atomic<bool> media_main_g_WgcCaptureRunning{false};

inline std::atomic<bool> media_main_g_WgcCaptureShutdown{false};

inline std::thread media_main_g_WgcCaptureThread;

inline std::atomic<bool> media_main_g_InjectDeliveredFirstFrame{false};

inline std::atomic<bool> media_main_g_RejectInjectFrames{false};

inline std::atomic<bool> media_main_g_AutoWgcFallbackArmed{false};

inline std::atomic<uint32_t> media_main_g_InjectBufferedTrimmedFrames{0};

inline std::atomic<uint32_t> media_main_g_InjectCadenceDroppedFrames{0};

inline std::atomic<uint32_t> media_main_g_WgcProducerTargetFps{0};

inline std::atomic<uint64_t> media_main_g_ActivePathMismatchFramesDiscarded{0};

// Preserve several seconds even with a 1000 Hz DXGI hardware pointer so normal
// delayed screen-grab targets retain the source history they still need.
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline ce::cursor::Timeline media_main_g_WgcCursorTimeline(8192);

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline ce::cursor::Timeline media_main_g_InjectCursorTimeline(1024);

inline std::atomic<uint64_t> media_main_g_DxgiCursorTimelinePublished{0};

inline std::mutex media_main_g_WgcCursorPublicationMutex;

inline UINT GetCursorDpiAtPoint(POINT point) {
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

inline int GetCursorMetricForDpi(int metric, UINT dpi) {
    using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
    static const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    static const auto getSystemMetricsForDpi =
        reinterpret_cast<GetSystemMetricsForDpiFn>(user32 ? GetProcAddress(user32, "GetSystemMetricsForDpi") : nullptr);
    return getSystemMetricsForDpi ? getSystemMetricsForDpi(metric, dpi) : GetSystemMetrics(metric);
}

inline ce::cursor::CaptureState CaptureCursorSnapshot(int64_t associationQpc, int32_t captureLeft, int32_t captureTop,
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

inline uint64_t AdvanceWgcSourceEpoch(const char* reason) {
    const uint64_t epoch = media_main_g_WgcSourceEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    // Cursor history is source-owned just like retained textures. Do not let a
    // newly published duplication source select exact-QPC samples from the
    // retired monitor/window epoch before its first pointer update arrives.
    {
        std::lock_guard<std::mutex> lock(media_main_g_WgcCursorPublicationMutex);
        media_main_g_WgcCursorTimeline.Clear();
        media_main_g_DxgiCursorTimelinePublished.store(0, std::memory_order_release);
    }
    LogInfo("[Media] Advanced WGC source epoch to %llu (%s)", static_cast<unsigned long long>(epoch),
            reason ? reason : "unspecified");
    return epoch;
}

inline void PublishWgcCapture(std::shared_ptr<WGCCapture> replacement, const char* reason) {
    const uint64_t epoch = AdvanceWgcSourceEpoch(reason);
    if (replacement) {
        // Bind the epoch before publication/start. A callback from the retired
        // source keeps its old identity even if it finishes after this global
        // coordinator epoch changes.
        replacement->SetSourceEpoch(epoch);
    }
    auto retired = media_main_g_WgcCap.Exchange(std::move(replacement));
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

inline WgcRuntimeLogSnapshot media_main_g_WgcRuntimeLogSnapshot;

inline void AtomicMax(std::atomic<uint32_t>& target, uint32_t value) {
    uint32_t current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

inline void AtomicMin(std::atomic<uint32_t>& target, uint32_t value) {
    uint32_t current = target.load(std::memory_order_relaxed);
    while (value < current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

inline void SnapshotWgcRuntimeLogState(const WGCCapture* cap) {
    if (!cap) {
        return;
    }

    AtomicMax(media_main_g_WgcRuntimeLogSnapshot.duplicateTimestampsSeen, cap->GetNormalizedDuplicateTimestampCount());
    AtomicMax(media_main_g_WgcRuntimeLogSnapshot.duplicateTimestampsSkipped, cap->GetDuplicateTimestampSkipCount());

    const uint32_t leasedMax = cap->GetPoolSlotLeasedMaxCount();
    const uint32_t freeMin = cap->GetPoolSlotFreeMinCount();
    if (leasedMax == 0 && freeMin == 0) {
        return;
    }

    media_main_g_WgcRuntimeLogSnapshot.sourceFramePoolBuffers.store(cap->GetSourceFramePoolBufferCount(),
                                                         std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.copyPoolSlots.store(cap->GetTexturePoolSlotCount(), std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.budgetSurfaces.store(cap->GetSmoothnessBudgetSurfaceCount(), std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.syncFrames.store(cap->GetSmoothnessSyncFrameCount(), std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.extraFrames.store(cap->GetSmoothnessRetainedFrameCount(), std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.retainedCap.store(cap->GetSmoothnessRetainedFrameCap(), std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.reservedFreeSlots.store(cap->GetSmoothnessReservedFreeSlotCount(),
                                                    std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.safetySlots.store(cap->GetSmoothnessSafetySlotCount(), std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.sourceFormat.store(cap->GetSmoothnessSourceDxgiFormat(), std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.retainedFormat.store(cap->GetSmoothnessCopyDxgiFormat(), std::memory_order_relaxed);
    const bool compact = cap->IsCompactRetainedCopyActive();
    media_main_g_WgcRuntimeLogSnapshot.compactRetained.store(compact ? 1u : 0u, std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.estimatedVramBytes.store(cap->GetSmoothnessEstimatedVramBytes(), std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.sourceBudgetBytes.store(cap->GetSmoothnessSourceEstimatedVramBytes(),
                                                    std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.copyBudgetBytes.store(cap->GetSmoothnessCopyEstimatedVramBytes(),
                                                  std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.sourceSurfaceBytes.store(cap->GetSmoothnessSourceBytesPerSurface(),
                                                     std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.copySurfaceBytes.store(cap->GetSmoothnessCopyBytesPerSurface(), std::memory_order_relaxed);
    const int64_t convertUs = cap->GetLastPoolConvertTimeUs();
    if (compact || cap->IsUsingDesktopDuplication()) {
        if (convertUs > 0) {
            media_main_g_WgcRuntimeLogSnapshot.lastConvertUs.store(convertUs, std::memory_order_relaxed);
        }
    } else {
        media_main_g_WgcRuntimeLogSnapshot.lastConvertUs.store(0, std::memory_order_relaxed);
    }
    AtomicMax(media_main_g_WgcRuntimeLogSnapshot.poolLeasedMax, leasedMax);
    AtomicMin(media_main_g_WgcRuntimeLogSnapshot.poolFreeMin, freeMin);
    AtomicMax(media_main_g_WgcRuntimeLogSnapshot.poolSaturatedDrops, cap->GetPoolSaturatedDropCount());
    AtomicMax(media_main_g_WgcRuntimeLogSnapshot.poolOverwritePrevented, cap->GetPoolSlotOverwritePreventedCount());
    AtomicMax(media_main_g_WgcRuntimeLogSnapshot.poolLeaseMismatches, cap->GetPoolLeaseMismatchCount());
    media_main_g_WgcRuntimeLogSnapshot.hasPoolEvidence.store(true, std::memory_order_release);
}

inline void SnapshotPublishedWgcRuntimeLogState() {
    const auto cap = media_main_g_WgcCap.Read();
    SnapshotWgcRuntimeLogState(cap.get());
}

struct WgcRetargetRequest {
    HWND window = NULL;
    HMONITOR monitor = NULL;
    bool preferMonitor = false;
    bool active = false;
};

// --- Auto-detected render-endpoint audio latency (per-device latency model, Part B) -----------
// Measured once per media process via the WASAPI render->loopback probe (mediaengine export), then
// applied to the render-domain audio sources on every config (re)load. The probe cache is
// process-memory only; a fresh process may re-probe, but no endpoint latency file is written.
inline double media_main_g_AutoDetectedRenderLatencyMs = -1.0;  // <0 = not measured / unavailable

inline bool media_main_g_RenderLatencyMeasureAttempted = false;

inline bool media_main_g_LegacyAudioLatencyCacheCleanupAttempted = false;

inline std::mutex media_main_g_LegacyAudioLatencyCacheCleanupMutex;

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - fixed short values stay in SSO; default construction is non-allocating
inline std::string media_main_g_AvSyncConfidence = "low";

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - fixed short values stay in SSO; default construction is non-allocating
inline std::string media_main_g_AvSyncReason = "not_measured";

inline bool media_main_g_AvSyncUsedAudioProbe = false;

inline void StampAvSyncStatus(AppConfig& config, const char* confidence, const char* reason, float resolvedMs,
                              bool usedAudioProbe) {
    config.avSyncConfidence = confidence ? confidence : "low";
    config.avSyncReason = reason ? reason : "unknown";
    config.avSyncResolvedRenderLatencyMs = resolvedMs;
    config.avSyncUsedAudioProbe = usedAudioProbe;
}

// Apply the auto-detected render-endpoint latency to render-domain sources (system loopback + app
// process loopback only). No-op when autodetect is off, a manual override is configured, or no
// value has been measured. Microphones (Domain 2) are never touched here. Cheap and idempotent.
inline void ApplyAutoDetectedRenderLatencyToConfig(AppConfig& config) {
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

    if (media_main_g_AutoDetectedRenderLatencyMs <= 0.0) {
        StampAvSyncStatus(config, media_main_g_AvSyncConfidence.c_str(), media_main_g_AvSyncReason.c_str(), 0.0f, media_main_g_AvSyncUsedAudioProbe);
        LogWarn(
            "[AVSyncAuto] passiveInputs renderSources=%d micSources=%d generalRenderLatencyMs=0.000 "
            "autodetect=1 probe=%s chosenDelayMs=0.000 confidence=%s reason=%s",
            renderDomainSources, micDomainSources, media_main_g_RenderLatencyMeasureAttempted ? "unavailable" : "not_attempted",
            config.avSyncConfidence.c_str(), config.avSyncReason.c_str());
        return;
    }
    const float ms = static_cast<float>(media_main_g_AutoDetectedRenderLatencyMs);
    config.audioCaptureLatencyMs = ms;
    int applied = 0;
    for (auto& s : config.audioSources) {
        const bool renderDomain = s.sourceType == AudioConfig::SystemAudio || s.sourceType == AudioConfig::AppAudio;
        if (renderDomain && s.captureLatencyMs == 0.0f) {  // inherited auto default, no per-source override
            s.captureLatencyMs = ms;
            ++applied;
        }
    }
    StampAvSyncStatus(config, media_main_g_AvSyncConfidence.c_str(), media_main_g_AvSyncReason.c_str(), ms, media_main_g_AvSyncUsedAudioProbe);
    LogInfo(
        "[AVSyncAuto] passiveInputs renderSources=%d micSources=%d generalRenderLatencyMs=0.000 autodetect=1 "
        "probe=%s chosenDelayMs=%.3f appliedSources=%d confidence=%s reason=%s domain=render_endpoint",
        renderDomainSources, micDomainSources,
        config.avSyncUsedAudioProbe ? "audio_render_loopback" : "memory_cache_or_manual", static_cast<double>(ms),
        applied, config.avSyncConfidence.c_str(), config.avSyncReason.c_str());
}

inline void DeleteLegacyAudioLatencyCacheFileOnce(const std::string& cacheDir) {
    if (cacheDir.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(media_main_g_LegacyAudioLatencyCacheCleanupMutex);
        if (media_main_g_LegacyAudioLatencyCacheCleanupAttempted) {
            return;
        }
        media_main_g_LegacyAudioLatencyCacheCleanupAttempted = true;
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
inline void MeasureRenderLatencyOnce(const AppConfig& config, const std::string& cacheDir) {
    DeleteLegacyAudioLatencyCacheFileOnce(cacheDir);
    if (media_main_g_RenderLatencyMeasureAttempted) {
        return;
    }
    if (config.audioCaptureLatencyMs > 0.0f) {
        media_main_g_RenderLatencyMeasureAttempted = true;  // disabled or manual override: never measure
        media_main_g_AutoDetectedRenderLatencyMs = -1.0;
        media_main_g_AvSyncConfidence = "medium";
        media_main_g_AvSyncReason = "manual_config_override";
        media_main_g_AvSyncUsedAudioProbe = false;
        LogInfo("[AVSyncAuto] probe=skipped confidence=medium reason=manual_config_override configuredDelayMs=%.3f",
                static_cast<double>(config.audioCaptureLatencyMs));
        return;
    }
    if (!config.audioLatencyAutodetect) {
        media_main_g_RenderLatencyMeasureAttempted = true;
        media_main_g_AutoDetectedRenderLatencyMs = -1.0;
        media_main_g_AvSyncConfidence = "low";
        media_main_g_AvSyncReason = "autodetect_disabled";
        media_main_g_AvSyncUsedAudioProbe = false;
        LogWarn("[AVSyncAuto] probe=disabled confidence=low reason=autodetect_disabled chosenDelayMs=0.000");
        return;
    }
    media_main_g_RenderLatencyMeasureAttempted = true;

    // Audio-only render->loopback probe (Start-anchor) - the default active auto-detect.
    double ms = 0.0;
    if (MediaEngine_MeasureRenderEndpointLatency &&
        MediaEngine_MeasureRenderEndpointLatency(cacheDir.c_str(), false, &ms) && ms > 0.0) {
        media_main_g_AutoDetectedRenderLatencyMs = ms;
        media_main_g_AvSyncConfidence = "high";
        media_main_g_AvSyncReason = "audio_probe_render_loopback";
        media_main_g_AvSyncUsedAudioProbe = true;
        LogInfo("[AVSyncAuto] probe=audio_render_loopback chosenDelayMs=%.3f confidence=high domain=render_endpoint",
                ms);
    } else {
        media_main_g_AutoDetectedRenderLatencyMs = -1.0;
        media_main_g_AvSyncConfidence = "low";
        media_main_g_AvSyncReason = "probe_unavailable_passive_insufficient";
        media_main_g_AvSyncUsedAudioProbe = false;
        LogWarn(
            "[AVSyncAuto] probe=unavailable chosenDelayMs=0.000 confidence=low "
            "reason=probe_unavailable_passive_insufficient");
    }
}

inline constexpr int media_main_kInjectTextureSlotCount = SHARED_TEXTURE_SLOT_COUNT;

// Encoder-bottleneck EMA parameters.  The smoothing factor (alpha) controls
// how quickly the EMA reacts to encode-time changes.  Hysteresis avoids
// bang-bang oscillation: we enter bottleneck at a higher threshold and exit
// at a significantly lower one.
inline constexpr double media_main_kEncodeEmaAlpha = 0.10;

inline constexpr double media_main_kBottleneckEnterRatio = 0.95;  // smoothedEncodeMs > 95% of frame interval → enter

inline constexpr double media_main_kBottleneckExitRatio = 0.75;   // smoothedEncodeMs < 75% of frame interval → exit

inline const char* WgcIngressAdmissionReasonName(uint32_t code) {
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
    const bool currentlyBottlenecked = media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
    bool newState = currentlyBottlenecked;
    if (startupWindowActive) {
        newState = false;
    } else if (currentlyBottlenecked) {
        // Exit bottleneck only when encode time drops well below the frame budget
        if (smoothedEncodeMs < frameIntervalMs * media_main_kBottleneckExitRatio) {
            newState = false;
        }
    } else {
        // Enter bottleneck when encode time approaches the frame budget
        if (smoothedEncodeMs > frameIntervalMs * media_main_kBottleneckEnterRatio) {
            newState = true;
        }
    }
    if (newState != currentlyBottlenecked) {
        media_main_g_IsEncoderBottlenecked.store(newState, std::memory_order_relaxed);
        if (media_main_g_pSharedMem) {
            media_main_g_pSharedMem->runtimeState.encoderBottlenecked.store(newState ? 1u : 0u, std::memory_order_relaxed);
        }
    }
}

inline bool MediaAudioConfigEquals(const AudioConfig& lhs, const AudioConfig& rhs) {
    return lhs.enabled == rhs.enabled && lhs.device == rhs.device && lhs.processName == rhs.processName &&
           lhs.processId == rhs.processId && lhs.sourceType == rhs.sourceType && lhs.tracks == rhs.tracks &&
           lhs.codec == rhs.codec && lhs.bitrate == rhs.bitrate && lhs.sampleRate == rhs.sampleRate &&
           lhs.bitDepth == rhs.bitDepth && lhs.downmix == rhs.downmix && lhs.captureLatencyMs == rhs.captureLatencyMs;
}

inline bool MediaScalingConfigEquals(const ScalingConfig& lhs, const ScalingConfig& rhs) {
    return lhs.enabled == rhs.enabled && lhs.outputResolution == rhs.outputResolution && lhs.quality == rhs.quality &&
           lhs.sharpness == rhs.sharpness && lhs.outputWidth == rhs.outputWidth && lhs.outputHeight == rhs.outputHeight;
}

inline bool MediaVideoConfigEquals(const VideoConfig& lhs, const VideoConfig& rhs) {
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

}

inline bool MediaEngineConfigEquals(const AppConfig& lhs, const AppConfig& rhs) {
    if (lhs.logLevel != rhs.logLevel || lhs.captureMethod != rhs.captureMethod ||
        lhs.captureMonitor != rhs.captureMonitor ||
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

inline bool IsExplicitTenBitVideo(const VideoConfig& video) {
    return _stricmp(video.bitDepth.c_str(), "10") == 0;
}

inline uint32_t GetInitialWgcCfrTargetFps(const VideoConfig& video) {
    if (video.useVFR || video.fps <= 0) {
        return 0;
    }

    return ce::capture_policy::GetWgcCfrProducerTargetFps(static_cast<uint32_t>(video.fps));
}

inline uint32_t SaturatingToUint32(uint64_t value) {
    return value > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(value);
}

template <typename AtomicT>
void UpdateAtomicPeak(AtomicT& peak, uint32_t value) {
    uint32_t current = peak.load(std::memory_order_relaxed);
    while (value > current &&
           !peak.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

inline void SetCapturePipelinePhase(CapturePipelinePhase phase) {
    if (!media_main_g_pSharedMem) {
        return;
    }
    media_main_g_pSharedMem->runtimeState.capturePhase.store(static_cast<uint32_t>(phase), std::memory_order_release);
}

inline bool TryArmCapturePipelineWarmup() {
    return !media_main_g_pSharedMem ||
           ce::recording_lifecycle::TryArmWarmup(media_main_g_pSharedMem->runtimeState.capturePhase, media_main_g_Recording);
}

inline bool TryCommitCapturePipelineLive() {
    return !media_main_g_pSharedMem || ce::recording_lifecycle::TryCommitLive(media_main_g_pSharedMem->runtimeState.capturePhase, media_main_g_Recording);
}

inline CapturePipelinePhase BeginCapturePipelineStop() {
    if (!media_main_g_pSharedMem) {
        return CapturePipelinePhase::kCancelling;
    }
    const uint32_t liveFrames = media_main_g_pSharedMem->runtimeState.liveFramesEncoded.load(std::memory_order_acquire);
    return ce::recording_lifecycle::BeginStop(media_main_g_pSharedMem->runtimeState.capturePhase, liveFrames);
}

inline void ResetRuntimeDiagnostics(SharedMemoryLayout* sharedMem) {
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
    state.capturePhase.store(static_cast<uint32_t>(CapturePipelinePhase::kIdle), std::memory_order_release);
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
    state.recordingTimelineDebtMs.store(0, std::memory_order_relaxed);
    state.recordingPeakTimelineDebtMs.store(0, std::memory_order_relaxed);
    state.recordingHealthFlags.store(0, std::memory_order_release);
}

inline void ResetRecordingHealthPublication() {
    media_main_g_RecordingTimelineDebtMs.store(0, std::memory_order_relaxed);
    media_main_g_RecordingPeakTimelineDebtMs.store(0, std::memory_order_relaxed);
    media_main_g_RecordingCapacityAttributedDebtMs.store(0, std::memory_order_relaxed);
    media_main_g_RecordingHealthFlags.store(0, std::memory_order_release);
    if (media_main_g_pSharedMem) {
        media_main_g_pSharedMem->runtimeState.recordingTimelineDebtMs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.recordingPeakTimelineDebtMs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.recordingHealthFlags.store(0, std::memory_order_release);
    }
}

inline void PublishRecordingHealth(const ce::capture_policy::RecordingHealthState& health) {
    media_main_g_RecordingTimelineDebtMs.store(health.currentDebtMs, std::memory_order_relaxed);
    media_main_g_RecordingPeakTimelineDebtMs.store(health.peakDebtMs, std::memory_order_relaxed);
    media_main_g_RecordingCapacityAttributedDebtMs.store(health.capacityAttributedDebtMs, std::memory_order_relaxed);
    media_main_g_RecordingHealthFlags.store(health.flags, std::memory_order_release);
    if (media_main_g_pSharedMem) {
        media_main_g_pSharedMem->runtimeState.recordingTimelineDebtMs.store(health.currentDebtMs, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.recordingPeakTimelineDebtMs.store(health.peakDebtMs, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.recordingHealthFlags.store(health.flags, std::memory_order_release);
    }
}

inline void CompleteRecordingFinalization(bool canceled, bool outputSaved) {
    const uint32_t healthFlags = media_main_g_RecordingHealthFlags.load(std::memory_order_acquire);
    const uint32_t currentDebtMs = media_main_g_RecordingTimelineDebtMs.load(std::memory_order_relaxed);
    const uint32_t peakDebtMs = media_main_g_RecordingPeakTimelineDebtMs.load(std::memory_order_relaxed);
    const uint32_t capacityAttributedDebtMs =
        media_main_g_RecordingCapacityAttributedDebtMs.load(std::memory_order_relaxed);
    const char* healthStatus = ce::capture_policy::GetRecordingHealthStatus(healthFlags);
    const char* healthCause = ce::capture_policy::GetRecordingHealthCause(healthFlags);
    LogInfo(
        "[RECORDING FINALIZATION] status=%s health=%s cause=%s flags=0x%X currentDebtMs=%u peakDebtMs=%u "
        "capacityDebtMs=%u outputSaved=%d finalizationComplete=1 settingsChanged=0",
        canceled ? "canceled" : (outputSaved ? "media_finalized" : "failed"), healthStatus, healthCause,
        healthFlags, currentDebtMs, peakDebtMs, capacityAttributedDebtMs, outputSaved ? 1 : 0);
    FinalizeRecordingManifest(media_main_g_RecordingManifestLogPath, canceled, outputSaved, healthStatus, healthCause,
                              healthFlags, currentDebtMs, peakDebtMs, capacityAttributedDebtMs);

    if (!media_main_g_pSharedMem) {
        return;
    }
    auto& state = media_main_g_pSharedMem->runtimeState;
    const bool newerRecordingActive = state.captureRequested.load(std::memory_order_acquire) ||
                                      state.isRecording.load(std::memory_order_acquire) ||
                                      state.GetRecordingStartIntent() != RecordingStartIntent::Idle;
    if (newerRecordingActive) {
        LogInfo("[RECORDING FINALIZATION] Completion notification suppressed because a newer recording is active");
        return;
    }

    const bool degraded = ce::capture_policy::HasRecordingHealthFlag(
        healthFlags, ce::capture_policy::kRecordingHealthFlagVideoDegraded);
    const OverlayNotificationType notification =
        canceled    ? OverlayNotificationType::RecordingCanceled
        : !outputSaved ? OverlayNotificationType::RecordingFailed
        : degraded ? OverlayNotificationType::RecordingSavedDegraded
                   : OverlayNotificationType::RecordingSaved;
    state.notificationType.store(static_cast<uint32_t>(notification), std::memory_order_release);
    state.notificationExpiry.store(GetTickCount64() + ((degraded || !outputSaved) ? 7000ULL : 3000ULL),
                                   std::memory_order_release);
}

inline bool IsActiveScreenGrab() {
    return media_main_g_UseScreenGrab.load(std::memory_order_acquire);
}

inline void SetActiveScreenGrab(bool enabled) {
    media_main_g_UseScreenGrab.store(enabled, std::memory_order_release);
    if (MediaEngine_SetActiveScreenGrab) {
        MediaEngine_SetActiveScreenGrab(enabled);
    }
}

inline bool IsPreferredScreenGrab() {
    return media_main_g_PreferScreenGrab.load(std::memory_order_acquire);
}

inline void SetPreferredScreenGrab(bool enabled) {
    media_main_g_PreferScreenGrab.store(enabled, std::memory_order_release);
}

inline void SetCaptureRequestedState(bool enabled) {
    if (!media_main_g_pSharedMem) {
        return;
    }

    media_main_g_pSharedMem->runtimeState.captureRequested.store(enabled, std::memory_order_release);
}

inline void SetInjectVideoCaptureRequestedState(bool enabled, const char* reason) {
    if (!media_main_g_pSharedMem) {
        return;
    }

    const bool previous = media_main_g_pSharedMem->runtimeState.HasRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested);
    media_main_g_pSharedMem->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, enabled);
    if (previous != enabled) {
        LogInfo("[Media] Inject video publication %s (%s)", enabled ? "enabled" : "disabled",
                reason ? reason : "unspecified");
    }
}

// The media half of the recording-status overlay protocol lives in
// captureengine/status_overlay_sync.h; these thin wrappers keep the shared-memory
// null-check with the rest of the publication helpers.
inline void SignalStatusOverlaySync() {
    ce::status_overlay::SignalSync();
}

inline void RequestStatusOverlayDarkForCapture(const char* reason) {
    if (!media_main_g_pSharedMem) {
        return;
    }
    ce::status_overlay::RequestDarkForCapture(media_main_g_pSharedMem->runtimeState, reason);
}

inline void ReleaseStatusOverlayDarkForCapture(const char* reason) {
    if (!media_main_g_pSharedMem) {
        return;
    }
    ce::status_overlay::ReleaseDarkForCapture(media_main_g_pSharedMem->runtimeState, reason);
}

inline void SetRecordingVisibleState(bool enabled) {
    if (!media_main_g_pSharedMem) {
        return;
    }

    if (enabled) {
        const bool wasVisible = media_main_g_pSharedMem->runtimeState.isRecording.exchange(true, std::memory_order_acq_rel);
        if (!wasVisible) {
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            media_main_g_pSharedMem->runtimeState.recordingStartTime.store(GetTickCount64(), std::memory_order_release);
        }
        // Propagate audio-only flag so overlay can show AUDIO vs REC
        media_main_g_pSharedMem->runtimeState.audioOnly.store(media_main_g_AudioOnly, std::memory_order_release);
        media_main_g_pSharedMem->runtimeState.SetRecordingStartIntent(RecordingStartIntent::Idle);
        ReleaseStatusOverlayDarkForCapture("recording live");
    } else {
        media_main_g_pSharedMem->runtimeState.isRecording.store(false, std::memory_order_release);
        media_main_g_pSharedMem->runtimeState.recordingStartTime.store(0, std::memory_order_release);
        media_main_g_pSharedMem->runtimeState.audioOnly.store(false, std::memory_order_release);
        ReleaseStatusOverlayDarkForCapture("recording not live");
    }
    // Publish the resolved status before waking the overlay so it renders the final state.
    SignalStatusOverlaySync();
}

inline void PublishRecordingStartFailure(RecordingFailureCode failureCode, const char* reason) {
    if (media_main_g_pSharedMem) {
        ReleaseStatusOverlayDarkForCapture("recording start failure");
        media_main_g_pSharedMem->runtimeState.SetRecordingStartIntent(RecordingStartIntent::Idle);
        media_main_g_pSharedMem->runtimeState.isRecording.store(false, std::memory_order_release);
        media_main_g_pSharedMem->runtimeState.recordingStartTime.store(0, std::memory_order_release);
        StoreRelease(media_main_g_pSharedMem->runtimeState.recordingFailureCode, static_cast<uint32_t>(failureCode));
        // Surface the failed start in the inject and pseudo overlays through the
        // same transient notification channel finalization uses. Both overlays
        // show the notification only in the idle state, which the intent and
        // isRecording resets above have already established.
        media_main_g_pSharedMem->runtimeState.notificationType.store(
            static_cast<uint32_t>(OverlayNotificationType::RecordingFailed), std::memory_order_release);
        media_main_g_pSharedMem->runtimeState.notificationExpiry.store(GetTickCount64() + 7000ULL, std::memory_order_release);
        SignalStatusOverlaySync();
    }
    LogError("[Media] Recording start failed: %s (code=%u)", reason ? reason : "unspecified",
             static_cast<uint32_t>(failureCode));
}

inline bool WindowBelongsToProcess(HWND hwnd, DWORD pid) {
    if (!hwnd || pid == 0) {
        return false;
    }

    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    return windowPid == pid;
}

inline bool ShouldPreferInjectCaptureForFullscreenWindow(HWND hwnd, DWORD pid) {
    return WindowBelongsToProcess(hwnd, pid) && IsWindowFullscreenLike(hwnd);
}

inline InjectFrameLineage MakeInjectFrameLineage(const QueuedFrame& frame) {
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

inline bool MatchesInjectFrameLineage(const QueuedFrame& frame, const InjectFrameLineage& lineage) {
    return lineage.IsValid() && frame.isInjectMode && frame.frameIndex == lineage.frameIndex &&
           frame.textureIndex == lineage.textureIndex && frame.fenceValue == lineage.fenceValue &&
           frame.ringIndex == lineage.ringIndex && frame.timestamp == lineage.timestamp;
}

inline bool MatchesInjectFrameLineage(const InjectFrameLineage& lhs, const InjectFrameLineage& rhs) {
    return lhs.IsValid() && rhs.IsValid() && lhs.frameIndex == rhs.frameIndex && lhs.textureIndex == rhs.textureIndex &&
           lhs.fenceValue == rhs.fenceValue && lhs.ringIndex == rhs.ringIndex && lhs.timestamp == rhs.timestamp;
}

inline bool IsInjectTextureIndexValid(int32_t textureIndex) {
    return textureIndex >= 0 && textureIndex < media_main_kInjectTextureSlotCount;
}

inline bool JoinThreadWithTimeout(std::thread& thread, DWORD timeoutMs, const char* threadName) {
    if (!thread.joinable()) {
        return true;
    }

    HANDLE threadHandle = ce::Win32ThreadHandle(thread);
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

inline void ReleaseStandaloneWgcQueuedFrame(QueuedFrame& frame) {
    if (!frame.isInjectMode && frame.texture) {
        frame.texture->Release();
        frame.texture = nullptr;
    }
    frame.wgcPoolLease.Reset();
    frame = QueuedFrame{};
}

inline void ClearStandbyWgcHandoffFrame() {
    QueuedFrame stale;
    {
        std::lock_guard<std::mutex> lock(media_main_g_StandbyWgcFrameMutex);
        if (!media_main_g_HasStandbyWgcFrame) {
            return;
        }
        stale = std::move(media_main_g_StandbyWgcFrame);
        media_main_g_HasStandbyWgcFrame = false;
    }
    ReleaseStandaloneWgcQueuedFrame(stale);
}

inline bool HasStandbyWgcHandoffFrame() {
    std::lock_guard<std::mutex> lock(media_main_g_StandbyWgcFrameMutex);
    return media_main_g_HasStandbyWgcFrame;
}

inline bool StoreStandbyWgcHandoffFrame(QueuedFrame&& frame) {
    QueuedFrame stale;
    {
        std::lock_guard<std::mutex> lock(media_main_g_StandbyWgcFrameMutex);
        // Recheck while holding the slot lock. A callback can observe the
        // retention flag immediately before the handoff thread disarms it; in
        // that race it must not repopulate the slot after the handoff has taken
        // the proven frame.
        if (!media_main_g_RetainStandbyWgcFrameForHandoff.load(std::memory_order_acquire)) {
            return false;
        }
        if (media_main_g_HasStandbyWgcFrame) {
            stale = std::move(media_main_g_StandbyWgcFrame);
        }
        media_main_g_StandbyWgcFrame = std::move(frame);
        media_main_g_HasStandbyWgcFrame = true;
    }
    ReleaseStandaloneWgcQueuedFrame(stale);
    return true;
}

inline bool TakeStandbyWgcHandoffFrame(QueuedFrame& frame) {
    std::lock_guard<std::mutex> lock(media_main_g_StandbyWgcFrameMutex);
    if (!media_main_g_HasStandbyWgcFrame) {
        return false;
    }
    frame = std::move(media_main_g_StandbyWgcFrame);
    media_main_g_HasStandbyWgcFrame = false;
    return true;
}

inline void SubmitWgcQueuedFrame(QueuedFrame&& frame) {
    static std::atomic<int64_t> s_lastWgcTimestamp{0};
    if (media_main_g_pSharedMem) {
        const int64_t comparisonTimestamp = frame.rawTimestamp > 0 ? frame.rawTimestamp : frame.timestamp;
        const int64_t previousTimestamp = s_lastWgcTimestamp.exchange(comparisonTimestamp, std::memory_order_relaxed);
        if (previousTimestamp > 0) {
            if (comparisonTimestamp < previousTimestamp) {
                media_main_g_pSharedMem->runtimeState.sourceTimestampRegressions.fetch_add(1, std::memory_order_relaxed);
            } else if (comparisonTimestamp == previousTimestamp) {
                media_main_g_pSharedMem->runtimeState.sourceTimestampStalls.fetch_add(1, std::memory_order_relaxed);
            }
        }
        media_main_g_pSharedMem->runtimeState.sourceFramesReceived.fetch_add(1, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.framesQueued.fetch_add(1, std::memory_order_relaxed);
    }

    // The queue unconditionally takes ownership of frame.texture, including on the
    // drop-oldest overflow path. Releasing a cached raw pointer here would double-release.
    media_main_g_FrameQueue.Push(std::move(frame));
}

inline void QueueWgcCursorObservation(const ce::cursor::SourcePointerObservation& observation, int32_t captureLeft,
                                      int32_t captureTop, uint32_t captureWidth, uint32_t captureHeight,
                                      uint64_t sourceEpoch) {
    if (!observation.valid || observation.updateQpc <= 0 || captureWidth == 0 || captureHeight == 0 ||
        sourceEpoch != media_main_g_WgcSourceEpoch.load(std::memory_order_acquire)) {
        return;
    }

    ce::cursor::CaptureState cursorState =
        CaptureCursorSnapshot(observation.updateQpc, captureLeft, captureTop, captureWidth, captureHeight, false);
    ce::cursor::ApplySourcePointerObservation(&cursorState, observation);
    std::lock_guard<std::mutex> lock(media_main_g_WgcCursorPublicationMutex);
    if (sourceEpoch != media_main_g_WgcSourceEpoch.load(std::memory_order_acquire)) {
        return;
    }
    media_main_g_WgcCursorTimeline.Publish(cursorState);
    const uint64_t published = media_main_g_DxgiCursorTimelinePublished.fetch_add(1, std::memory_order_release) + 1;
    if (published == 1) {
        LogInfo(
            "[Cursor] DXGI QPC pointer timeline active: updateQpc=%lld position=(%d,%d) visible=%d embedded=%d "
            "coord=%s bounds=(%d,%d %ux%u)",
            static_cast<long long>(observation.updateQpc), observation.screenX, observation.screenY,
            observation.visible ? 1 : 0, observation.embedded ? 1 : 0,
            observation.positionIsShapeTopLeft ? "shape-top-left" : "hotspot", captureLeft, captureTop, captureWidth,
            captureHeight);
    }
}

inline void QueueWgcFrame(ID3D11Texture2D* texture, uint32_t width, uint32_t height, int64_t timestamp,
                          int64_t rawTimestamp, bool isHDR, bool cursorEmbedded, bool duplicateSourceTimestamp,
                          const ce::cursor::SourcePointerObservation& cursorObservation, int32_t captureLeft,
                          int32_t captureTop, uint64_t sourceEpoch, WgcPoolSlotLease&& poolLease) {
    const uint64_t activeEpoch = media_main_g_WgcSourceEpoch.load(std::memory_order_acquire);
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
    media_main_g_WgcCursorTimeline.Publish(qf.cursorState);

    if (media_main_g_Recording.load(std::memory_order_acquire) && !IsActiveScreenGrab()) {
        if (media_main_g_RetainStandbyWgcFrameForHandoff.load(std::memory_order_acquire) &&
            StoreStandbyWgcHandoffFrame(std::move(qf))) {
            return;
        }
        const uint64_t discarded = media_main_g_ActivePathMismatchFramesDiscarded.fetch_add(1, std::memory_order_relaxed) + 1;
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

inline QueuedFrame MakeQueuedWgcFrame(WGCCapturedFrame&& frame) {
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
    media_main_g_WgcCursorTimeline.Publish(qf.cursorState);
    return qf;
}

inline void ReleaseWgcCapturedFrame(WGCCapturedFrame& frame) {
    if (frame.texture) {
        frame.texture->Release();
        frame.texture = nullptr;
    }
    frame.poolLease.Reset();
    frame.poolSlot = std::numeric_limits<uint32_t>::max();
    frame.poolGeneration = 0;
}

inline void ResetInjectFrameRingToLatest(const char* reason) {
    if (!media_main_g_pSharedMem) {
        return;
    }

    FrameRingBuffer& ring = media_main_g_pSharedMem->frameRing;
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

inline void ResetLastQueuedFrameCache() {
    if (media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode && media_main_g_LastFrame.texture) {
        media_main_g_LastFrame.texture->Release();
    }
    if (media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode) {
        media_main_g_LastFrame.wgcPoolLease.Reset();
    }
    media_main_g_LastFrame = QueuedFrame{};
    media_main_g_HasLastFrame = false;
}

inline bool EnsureInjectCaptureEvents() {
    if (!media_main_g_InjectCaptureShutdownEvent) {
        media_main_g_InjectCaptureShutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!media_main_g_InjectCaptureShutdownEvent) {
            LogWarn("[Inject Thread] Failed to create shutdown event (err=%lu)", GetLastError());
        }
    }
    if (!media_main_g_InjectFrameReadyEvent && media_main_g_pSharedMem && media_main_g_pSharedMem->GetHostPID() != 0) {
        wchar_t eventName[64]{};
        GenerateInjectFrameReadyEventName(eventName, _countof(eventName), media_main_g_pSharedMem->GetHostPID());
        media_main_g_InjectFrameReadyEvent = CreateEventW(nullptr, FALSE, FALSE, eventName);
        if (!media_main_g_InjectFrameReadyEvent) {
            LogWarn("[Inject Thread] Failed to create frame-ready event '%ls' (err=%lu)", eventName, GetLastError());
        } else {
            LogInfo("[Inject Thread] Frame-ready event initialized: %ls", eventName);
        }
    }
    return media_main_g_InjectFrameReadyEvent && media_main_g_InjectCaptureShutdownEvent;
}

inline void StopInjectCapturePipeline() {
    media_main_g_InjectCaptureShutdown = true;
    if (media_main_g_InjectCaptureShutdownEvent) {
        SetEvent(media_main_g_InjectCaptureShutdownEvent);
    }
    JoinThreadWithTimeout(media_main_g_InjectCaptureThread, 5000, "inject capture");
    ResetInjectFrameRingToLatest("inject pipeline stop");
}

// Duplication embedded-cursor suppression: while duplicated frames already
// CONTAIN the cursor (software/composed cursor reported by the dup pointer
// metadata), encoder-side cursor composition must be suppressed to avoid a
// double cursor. Polled cheaply on the encoder thread per submitted frame;
// the state only changes on hardware/software cursor-plane transitions.
inline std::atomic<bool> media_main_g_DupCursorSuppressionActive{false};

inline void SyncDuplicationCursorSuppression(bool suppress) {
    if (suppress == media_main_g_DupCursorSuppressionActive.load(std::memory_order_relaxed)) {
        return;
    }
    media_main_g_DupCursorSuppressionActive.store(suppress, std::memory_order_relaxed);
    if (MediaEngine_SetCursorCompositionSuppressed) {
        MediaEngine_SetCursorCompositionSuppressed(suppress);
    }
    LogInfo("[Media] Encoder cursor composition %s (duplication frames %s the cursor)",
            suppress ? "suppressed" : "active", suppress ? "already contain" : "do not contain");
}

inline void ResetDuplicationCursorSuppression(const char* reason) {
    const bool wasSuppressed = media_main_g_DupCursorSuppressionActive.exchange(false, std::memory_order_acq_rel);
    if (MediaEngine_SetCursorCompositionSuppressed) {
        // Always publish the reset. Merely clearing the local cache can leave
        // the encoder latched in suppression across a reset/retarget.
        MediaEngine_SetCursorCompositionSuppressed(false);
    }
    if (wasSuppressed) {
        LogInfo("[Media] Encoder cursor composition restored (%s)", reason ? reason : "capture transition");
    }
}

inline void StopWgcCapturePipeline() {
    media_main_g_RetainStandbyWgcFrameForHandoff.store(false, std::memory_order_release);
    ClearStandbyWgcHandoffFrame();
    ResetDuplicationCursorSuppression("WGC pipeline stop");
    media_main_g_WgcCaptureShutdown = true;
    media_main_g_WgcProducerTargetFps.store(0, std::memory_order_relaxed);
    if (media_main_g_pSharedMem) {
        media_main_g_pSharedMem->runtimeState.wgcTargetFps.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcCaptureHealthFlags.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcCaptureHealthFps.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionErrorAvgUs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionErrorMaxUs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionErrorSignedAvgUs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionEarlyMaxUs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionLateMaxUs.store(0, std::memory_order_relaxed);
    }
    JoinThreadWithTimeout(media_main_g_WgcCaptureThread, 5000, "WGC capture");

    // Block encoder-side readers while the capture session and its WinRT/DXGI
    // resources are torn down. Atomic shared ownership alone protects object
    // lifetime; this access gate also protects mutable session internals.
    auto capture = media_main_g_WgcCap.LockExclusive();
    if (capture) {
        capture->SetDirectFrameCallback(nullptr);
        capture->SetDirectCursorCallback(nullptr);
        capture->SetTargetFps(0);
        if (capture->IsCapturing()) {
            capture->StopCapture();
        }
    }
}

inline bool StartWgcRecordingCapture(const AppConfig& config) {
    media_main_g_WgcRuntimeLogSnapshot.Reset();

    if (media_main_g_WgcCaptureThread.joinable()) {
        LogWarn("[Media] Cleaning up stale WGC capture thread before restart");
        media_main_g_WgcCaptureShutdown = true;
        JoinThreadWithTimeout(media_main_g_WgcCaptureThread, 5000, "WGC capture");
    }

    auto captureAccess = media_main_g_WgcCap.LockExclusive();
    WGCCapture* capture = captureAccess.get();
    if (!capture) {
        return false;
    }

    if (capture->IsCapturing()) {
        capture->SetDirectFrameCallback(nullptr);
        capture->SetDirectCursorCallback(nullptr);
        capture->StopCapture();
    }

    capture->SetCaptureCursor(ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
    if (config.video.captureCursor) {
        LogInfo("[Media] WGC cursor capture: native WGC cursor disabled; encoder-side cursor composition enabled");
    }
    HWND activeWgcWindow = NULL;
    HMONITOR activeWgcMonitor = NULL;
    capture->GetTargetIdentity(&activeWgcWindow, &activeWgcMonitor);
    LogInfo(
        "[PrivacyBlackout] session enabled=%d target=%s policy=matching-foreground-fullscreen failMode=black "
        "processInspection=0 hooks=0",
        config.blackWhenNoFullscreenFocus ? 1 : 0,
        activeWgcWindow ? "window" : (activeWgcMonitor ? "monitor" : "unresolved"));

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
    capture->SetDirectCursorCallback(config.video.captureCursor ? QueueWgcCursorObservation : nullptr);
    capture->ResetStats();
    // Explicitly reset both the cache and the encoder-side state. A prior
    // duplication session may have ended while its software cursor was embedded.
    ResetDuplicationCursorSuppression("WGC recording start");
    media_main_g_IsEncoderBottlenecked.store(false, std::memory_order_relaxed);
    media_main_g_WgcProducerTargetFps.store(initialWgcTargetFps, std::memory_order_relaxed);
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
    if (media_main_g_pSharedMem) {
        media_main_g_pSharedMem->runtimeState.wgcTargetFps.store(initialWgcTargetFps, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcCaptureHealthFlags.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcCaptureHealthFps.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionErrorAvgUs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionErrorMaxUs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionErrorSignedAvgUs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionEarlyMaxUs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionLateMaxUs.store(0, std::memory_order_relaxed);
    }
    if (!capture->StartCapture()) {
        capture->SetDirectFrameCallback(nullptr);
        capture->SetDirectCursorCallback(nullptr);
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

    media_main_g_WgcCaptureShutdown = false;
    // Recording-lifetime config snapshot: the main thread reassigns `config`
    // on late hook connects and IPC config reloads (refreshActiveConfig),
    // which would be a use-after-free race against a by-reference reader on
    // this thread. Recording settings must not change live mid-session anyway.
    {
        auto configSnapshot = std::make_shared<const AppConfig>(config);
        media_main_g_WgcCaptureThread = std::thread([configSnapshot]() { WgcCaptureThreadFunc(*configSnapshot); });
    }
    return true;
}

// Window finding helper
struct WindowSearch {
    DWORD pid;
    HWND hwnd;
};

inline BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
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

inline HWND GetMainWindowForProcess(DWORD pid) {
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

inline std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

inline bool IsIgnoredForegroundWgcClass(HWND hwnd) {
    char className[128] = {};
    if (GetClassNameA(hwnd, className, static_cast<int>(sizeof(className))) <= 0) {
        return false;
    }

    const std::string lowerClass = ToLowerAscii(className);
    return lowerClass == "progman" || lowerClass == "workerw" || lowerClass == "shell_traywnd";
}

inline bool IsIgnoredForegroundWgcProcess(const std::string& processName) {
    const std::string lowerName = ToLowerAscii(processName);
    return lowerName.empty() || lowerName == "unknown" || lowerName == "explorer.exe" ||
           lowerName == "applicationframehost.exe" || lowerName == "shellexperiencehost.exe" ||
           lowerName == "searchhost.exe" || lowerName == "startmenuexperiencehost.exe" ||
           lowerName == "textinputhost.exe" || lowerName == "captureengine.exe";
}

inline ForegroundWgcWindowCandidate GetForegroundWgcWindowCandidate() {
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

inline bool MatchesProcessEntry(const WhitelistEntry& entry, const std::string& lowerProcessName) {
    return MatchesProcessName(entry, lowerProcessName);
}

inline bool MatchesProcessEntries(const std::vector<WhitelistEntry>& entries, const std::string& processName) {
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

inline const ApplicationProfile* FindApplicationProfileForProcess(const AppConfig& config,
                                                                  const std::string& processName) {
    if (processName.empty())
        return nullptr;

    std::string lowerName = processName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    for (const ApplicationProfile& profile : config.applicationProfiles) {
        if (!profile.target.HasProcess())
            continue;
        std::string lowerTarget = profile.target.pattern;
        std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if ((!profile.legacy && lowerName == lowerTarget) ||
            (profile.legacy && MatchesProcessEntry(profile.target, lowerName)))
            return &profile;
    }
    return nullptr;
}

inline const ApplicationProfile* FindApplicationProfileForTarget(const AppConfig& config,
                                                                 const WhitelistEntry& target) {
    auto found = std::find_if(config.applicationProfiles.begin(), config.applicationProfiles.end(),
                              [&](const ApplicationProfile& profile) { return profile.target == target; });
    return found == config.applicationProfiles.end() ? nullptr : &*found;
}

inline int64_t RectArea(const RECT& rect) {
    const int64_t width = std::max<LONG>(0, rect.right - rect.left);
    const int64_t height = std::max<LONG>(0, rect.bottom - rect.top);
    return width * height;
}

inline HWND FindMatchingWgcWindow(const std::vector<WhitelistEntry>& targets, int* selectedScore = nullptr,
                                  bool requireExactProcessNames = false, uint32_t* selectedPid = nullptr,
                                  std::string* selectedProcessName = nullptr,
                                  WhitelistEntry* selectedTarget = nullptr) {
    struct WgcSearchContext {
        const std::vector<WhitelistEntry>* targets;
        HWND result;
        HWND foregroundRoot;
        int checked;
        int matched;
        int bestScore;
        bool requireExactProcessNames;
        uint32_t bestPid;
        std::string bestProcessName;
        WhitelistEntry bestTarget;
        bool hasBestTarget;
    };

    HWND foregroundRoot = GetForegroundWindow();
    if (foregroundRoot) {
        HWND root = GetAncestor(foregroundRoot, GA_ROOT);
        if (root) {
            foregroundRoot = root;
        }
    }

    WgcSearchContext ctx = {&targets, NULL, foregroundRoot, 0, 0, std::numeric_limits<int>::min(),
                            requireExactProcessNames, 0, {}, {}, false};
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
                procName = GetProcessNameFromPID(pid);
                std::transform(procName.begin(), procName.end(), procName.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
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

                if (!matched && MatchesProcessName(entry, procName, context->requireExactProcessNames)) {
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
                        context->bestPid = pid;
                        context->bestProcessName = procName;
                        context->bestTarget = entry;
                        context->hasBestTarget = true;
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

    if (selectedScore)
        *selectedScore = ctx.result ? ctx.bestScore : std::numeric_limits<int>::min();
    if (selectedPid)
        *selectedPid = ctx.result ? ctx.bestPid : 0;
    if (selectedProcessName)
        *selectedProcessName = ctx.result ? ctx.bestProcessName : std::string{};
    if (selectedTarget)
        *selectedTarget = ctx.hasBestTarget ? ctx.bestTarget : WhitelistEntry{};

    return ctx.result;
}

inline std::string GetLocalConfigPath() {
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

inline void DisableCurrentThreadPowerThrottling(const char* role) {
    THREAD_POWER_THROTTLING_STATE throttlingState = {};
    throttlingState.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttlingState.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttlingState.StateMask = 0;
    if (!SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttlingState, sizeof(throttlingState))) {
        LogWarn("[%s] Failed to disable execution-speed power throttling (tid=%lu err=%lu)", role, GetCurrentThreadId(),
                GetLastError());
    }
}

inline void WaitUntilQpcTarget(HANDLE timer, int64_t targetQpc, int64_t qpcFrequency) {
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

inline const char* Win32PriorityClassName(DWORD priorityClass) {
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

inline bool IsCurrentProcessElevatedForPriorityLog() {
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

inline void ApplyMediaProcessPriority(const AppConfig& config) {
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

inline const char* D3dkmtSchedulingPriorityClassName(int priorityClass) {
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

inline bool ResolveD3dkmtSchedulingPriorityClass(const std::string& value, int& priorityClass) {
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

inline void ApplyMediaGpuSchedulingPriority(const AppConfig& config, const LUID* adapterLuid = nullptr) {
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

inline void ApplyMediaPrioritySettings(const AppConfig& config) {
    ApplyMediaProcessPriority(config);
    ApplyMediaGpuSchedulingPriority(config);
}

inline bool ApplyMediaGpuSchedulingPriorityForDevice(const AppConfig& config, ID3D11Device* device) {
    LUID luid{};
    if (!ce::windows_gpu_scheduling::GetAdapterLuid(device, luid)) {
        LogWarn("[Media] Could not resolve D3D11 adapter LUID for GPU scheduling priority");
        return false;
    }
    ApplyMediaGpuSchedulingPriority(config, &luid);
    return true;
}

inline bool ApplyMediaGpuSchedulingPriorityForSharedAdapter(const AppConfig& config) {
    if (!media_main_g_pSharedMem) {
        return false;
    }
    LUID luid{};
    luid.LowPart = media_main_g_pSharedMem->GetLuidLowPart();
    luid.HighPart = static_cast<LONG>(media_main_g_pSharedMem->GetLuidHighPart());
    if (luid.LowPart == 0 && luid.HighPart == 0) {
        return false;
    }
    ApplyMediaGpuSchedulingPriority(config, &luid);
    return true;
}

inline void PublishMediaScreenGrabTarget(uint32_t processId, ID3D11Device* device, bool active,
                                         const char* reason) {
    if (!media_main_g_pSharedMem)
        return;

    LUID luid{};
    const bool haveLuid = active && device && ce::windows_gpu_scheduling::GetAdapterLuid(device, luid);
    media_main_g_pSharedMem->runtimeState.PublishScreenGrabTarget(
        processId, haveLuid ? static_cast<int32_t>(luid.LowPart) : 0, haveLuid ? luid.HighPart : 0, active);
    LogInfo("[Media] Screen-grab sensor target %s (pid=%lu adapter=%08lX:%08lX reason=%s)",
            active ? "published" : "cleared", static_cast<unsigned long>(active ? processId : 0),
            static_cast<unsigned long>(active && haveLuid ? static_cast<uint32_t>(luid.HighPart) : 0),
            static_cast<unsigned long>(active && haveLuid ? luid.LowPart : 0), reason ? reason : "unspecified");
}
