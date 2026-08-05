#include "media_main_internal.h"

std::atomic<bool> media_main_g_Running{true};

std::atomic<bool> media_main_g_Recording{false};

std::atomic<bool> media_main_g_EncoderRunning{false};

std::atomic<bool> media_main_g_IsEncoderBottlenecked{false};

std::atomic<bool> media_main_g_RecordingUsesVfr{false};

std::atomic<bool> media_main_g_DrainOutstandingCfrTicks{false};

std::atomic<bool> media_main_g_PrivacyFailClosedStopRequested{false};

std::atomic<int64_t> media_main_g_CfrDrainStopQpc{0};

std::atomic<uint32_t> media_main_g_RecordingHealthFlags{0};

std::atomic<uint32_t> media_main_g_RecordingTimelineDebtMs{0};

std::atomic<uint32_t> media_main_g_RecordingPeakTimelineDebtMs{0};

std::atomic<uint32_t> media_main_g_RecordingCapacityAttributedDebtMs{0};

std::string media_main_g_RecordingManifestLogPath;

BOOL WINAPI MediaConsoleHandler(DWORD ctrlType) {
    // Handle all console events including Windows shutdown/logoff
    LogInfo("[Media] Console event %lu received, shutting down...", ctrlType);
    media_main_g_Running = false;
    return TRUE;
}

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
FrameQueue media_main_g_FrameQueue(32);

std::mutex media_main_g_StandbyWgcFrameMutex;

QueuedFrame media_main_g_StandbyWgcFrame;

bool media_main_g_HasStandbyWgcFrame = false;

std::atomic<bool> media_main_g_RetainStandbyWgcFrameForHandoff{false};

std::thread media_main_g_EncoderThread;

QueuedFrame media_main_g_LastFrame;

bool media_main_g_HasLastFrame = false;

std::atomic<uint64_t> media_main_g_InjectDeferredFrames{0};

// Screengrab mode components
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
ce::AtomicSharedOwner<WGCCapture> media_main_g_WgcCap;

std::atomic<uint64_t> media_main_g_WgcSourceEpoch{0};

std::atomic<bool> media_main_g_UseScreenGrab{false};     // Active capture mode for the current recording

std::atomic<bool> media_main_g_PreferScreenGrab{false};  // Preferred mode for the next recording

// Shared memory for hook communication
HANDLE media_main_g_hMapFile = NULL;

SharedMemoryLayout* media_main_g_pSharedMem = nullptr;

HANDLE media_main_g_hMapShmem = NULL;

ShmemBuffer* media_main_g_pShmem = nullptr;

// Audio-only recording flag (set via IPC or shared memory)
bool media_main_g_AudioOnly = false;

// Inject thread specific
std::atomic<bool> media_main_g_InjectCaptureRunning{false};

std::atomic<bool> media_main_g_InjectCaptureShutdown{false};

std::atomic<bool> media_main_g_InjectSessionReset{true};  // Set true on StartRecording to reset inject session state

std::thread media_main_g_InjectCaptureThread;

HANDLE media_main_g_InjectFrameReadyEvent = NULL;

HANDLE media_main_g_InjectCaptureShutdownEvent = NULL;

// WGC thread specific
std::atomic<bool> media_main_g_WgcCaptureRunning{false};

std::atomic<bool> media_main_g_WgcCaptureShutdown{false};

std::thread media_main_g_WgcCaptureThread;

std::atomic<bool> media_main_g_InjectDeliveredFirstFrame{false};

std::atomic<bool> media_main_g_RejectInjectFrames{false};

std::atomic<bool> media_main_g_AutoWgcFallbackArmed{false};

std::atomic<uint32_t> media_main_g_InjectBufferedTrimmedFrames{0};

std::atomic<uint32_t> media_main_g_InjectCadenceDroppedFrames{0};

std::atomic<uint32_t> media_main_g_WgcProducerTargetFps{0};

std::atomic<uint64_t> media_main_g_ActivePathMismatchFramesDiscarded{0};

// Preserve several seconds even with a 1000 Hz DXGI hardware pointer so normal
// delayed screen-grab targets retain the source history they still need.
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
ce::cursor::Timeline media_main_g_WgcCursorTimeline(8192);

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
ce::cursor::Timeline media_main_g_InjectCursorTimeline(1024);

std::atomic<uint64_t> media_main_g_DxgiCursorTimelinePublished{0};

std::mutex media_main_g_WgcCursorPublicationMutex;

UINT GetCursorDpiAtPoint(POINT point) {
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

int GetCursorMetricForDpi(int metric, UINT dpi) {
    using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
    static const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    static const auto getSystemMetricsForDpi =
        reinterpret_cast<GetSystemMetricsForDpiFn>(user32 ? GetProcAddress(user32, "GetSystemMetricsForDpi") : nullptr);
    return getSystemMetricsForDpi ? getSystemMetricsForDpi(metric, dpi) : GetSystemMetrics(metric);
}

WgcRuntimeLogSnapshot media_main_g_WgcRuntimeLogSnapshot;

void AtomicMax(std::atomic<uint32_t>& target, uint32_t value) {
    uint32_t current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

void AtomicMin(std::atomic<uint32_t>& target, uint32_t value) {
    uint32_t current = target.load(std::memory_order_relaxed);
    while (value < current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

// --- Auto-detected render-endpoint audio latency (per-device latency model, Part B) -----------
// Measured once per media process via the WASAPI render->loopback probe (mediaengine export), then
// applied to the render-domain audio sources on every config (re)load. The probe cache is
// process-memory only; a fresh process may re-probe, but no endpoint latency file is written.
double media_main_g_AutoDetectedRenderLatencyMs = -1.0;  // <0 = not measured / unavailable

bool media_main_g_RenderLatencyMeasureAttempted = false;

bool media_main_g_LegacyAudioLatencyCacheCleanupAttempted = false;

std::mutex media_main_g_LegacyAudioLatencyCacheCleanupMutex;

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - fixed short values stay in SSO; default construction is non-allocating
std::string media_main_g_AvSyncConfidence = "low";

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - fixed short values stay in SSO; default construction is non-allocating
std::string media_main_g_AvSyncReason = "not_measured";

bool media_main_g_AvSyncUsedAudioProbe = false;

// Duplication embedded-cursor suppression: while duplicated frames already
// CONTAIN the cursor (software/composed cursor reported by the dup pointer
// metadata), encoder-side cursor composition must be suppressed to avoid a
// double cursor. Polled cheaply on the encoder thread per submitted frame;
// the state only changes on hardware/software cursor-plane transitions.
std::atomic<bool> media_main_g_DupCursorSuppressionActive{false};
