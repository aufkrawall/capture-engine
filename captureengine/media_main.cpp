// clang-format off
#include <windows.h>
#include <d3d11.h>
#include <psapi.h>
#include <timeapi.h>
// clang-format on
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../common/config.h"
#include "../common/frame_queue.h"
#include "../common/frame_timing.h"
#include "../common/logging.h"
#include "../common/process_ipc.h"
#include "../common/shared_defs.h"
#include "mediaengine_loader.h"
#include "wgc_capture.h"

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

static std::atomic<bool> g_Running{true};
static std::atomic<bool> g_Recording{false};
static std::atomic<bool> g_EncoderRunning{false};
static std::atomic<bool> g_IsEncoderBottlenecked{false};

static FrameQueue g_FrameQueue(32);
static std::thread g_EncoderThread;
static QueuedFrame g_LastFrame;
static bool g_HasLastFrame = false;

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

// Forward declaration
void InjectCaptureThreadFunc(const AppConfig& config);
void WgcCaptureThreadFunc(const AppConfig& config);
void StopRecording();
void StartRecording(const AppConfig& config);

namespace {
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

constexpr DWORD kRecordingWarmupMinMs = 120;
constexpr DWORD kRecordingWarmupMaxMs = 350;

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

bool ShouldCommitRecordingWarmup(bool useScreenGrab, bool useVFR, bool poppedFrame, bool hasBufferedScreenGrabFrame,
                                 size_t bufferedInjectFrames, size_t injectReserveFrames, DWORD warmupElapsedMs) {
    if (!poppedFrame) {
        return false;
    }

    if (warmupElapsedMs >= kRecordingWarmupMaxMs) {
        return true;
    }

    if (warmupElapsedMs < kRecordingWarmupMinMs) {
        return false;
    }

    if (useVFR) {
        return true;
    }

    if (useScreenGrab) {
        return hasBufferedScreenGrabFrame;
    }

    return bufferedInjectFrames >= injectReserveFrames;
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

static void QueueWgcFrame(ID3D11Texture2D* texture, uint32_t width, uint32_t height, int64_t timestamp, bool isHDR) {
    QueuedFrame qf;
    qf.isInjectMode = false;
    qf.texture = texture;
    qf.width = width;
    qf.height = height;
    qf.timestamp = timestamp;
    qf.isHDR = isHDR;

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
        if (g_WgcCap->IsCapturing()) {
            g_WgcCap->StopCapture();
        }
    }

    g_WgcCaptureShutdown = true;
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
    // Capture WGC at source cadence and let the fixed-rate encoder thread choose
    // the best frame each output tick. Pre-throttling WGC to the output FPS
    // throws away temporal information that helps smooth 140 -> 120 style cases.
    g_WgcCap->SetTargetFps(0);
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
                    std::max<uint32_t>(8u, static_cast<uint32_t>(g_FrameQueue.Capacity() / 2));
                const bool useSourceSidePacing =
                    g_IsEncoderBottlenecked.load(std::memory_order_relaxed) || queueDepth >= queuePressureThreshold;

                if (!useSourceSidePacing) {
                    // When the queue is shallow and the encoder is healthy, keep all source
                    // cadence information and let the fixed-rate encoder thread choose the
                    // best source frame for each output tick.
                    shouldProcess = true;
                    nextPushTime = 0;
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
                        dropFrame = true;
                    }

                    QueuedFrame qf;
                    qf.isInjectMode = true;
                    qf.ringIndex = localReadIndex;
                    qf.timestamp = slot.timestamp;

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
                        qf.fenceHandle = (HANDLE)g_pSharedMem->GetFenceShareHandle();
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
                            validHandles = false;
                        }
                    }

                    if (!dropFrame && validHandles) {
                        if (g_RejectInjectFrames.load(std::memory_order_acquire)) {
                            droppedCount++;
                            dropFrame = true;
                        } else if (g_FrameQueue.Push(std::move(qf))) {
                            if (!g_InjectDeliveredFirstFrame.exchange(true, std::memory_order_acq_rel)) {
                                LogInfo("[Inject Thread] First actual inject frame queued");
                            }
                            pushedCount++;
                        } else {
                            droppedCount++;
                            dropFrame = true;
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
            uint32_t overloadFlags = 0;
            uint32_t muxQueueBytes = 0;
            uint32_t encoderQueueDepth = static_cast<uint32_t>(g_FrameQueue.Size());
            if (g_pSharedMem) {
                uint32_t currentDup = g_pSharedMem->runtimeState.duplicateFrames.load(std::memory_order_relaxed);
                uint32_t currentLate = g_pSharedMem->runtimeState.lateFrames.load(std::memory_order_relaxed);
                uint32_t currentTrimmed = g_InjectBufferedTrimmedFrames.load(std::memory_order_relaxed);
                uint32_t currentCadenceDropped = g_InjectCadenceDroppedFrames.load(std::memory_order_relaxed);
                dupDelta = currentDup - lastDuplicateCount;
                lateDelta = currentLate - lastLateCount;
                trimDelta = currentTrimmed - lastTrimmedCount;
                cadenceDropDelta = currentCadenceDropped - lastCadenceDroppedCount;
                lastDuplicateCount = currentDup;
                lastLateCount = currentLate;
                lastTrimmedCount = currentTrimmed;
                lastCadenceDroppedCount = currentCadenceDropped;
                overloadFlags = g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
                muxQueueBytes = g_pSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed);
                encoderQueueDepth = g_pSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
            } else {
                uint32_t currentTrimmed = g_InjectBufferedTrimmedFrames.load(std::memory_order_relaxed);
                uint32_t currentCadenceDropped = g_InjectCadenceDroppedFrames.load(std::memory_order_relaxed);
                trimDelta = currentTrimmed - lastTrimmedCount;
                cadenceDropDelta = currentCadenceDropped - lastCadenceDroppedCount;
                lastTrimmedCount = currentTrimmed;
                lastCadenceDroppedCount = currentCadenceDropped;
            }

            uint32_t inputFrames = pushedCount + droppedCount + pacingDroppedCount;
            LogInfo(
                "[Inject Perf] Input: %u | Queued: %u | DropFull: %u | DropPace: %u | HostQ: %u | EncQ: %u | Dup: %u "
                "| Late: %u | Trim: %u | SelDrop: %u | Encode: %lldus | Fence: %lldus | Mux: %uKB | Overload: 0x%X",
                inputFrames, pushedCount, droppedCount, pacingDroppedCount, static_cast<uint32_t>(g_FrameQueue.Size()),
                encoderQueueDepth, dupDelta, lateDelta, trimDelta, cadenceDropDelta,
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
            uint32_t currentCursorSkipCount = g_WgcCap->GetCursorOnlySkipCount();
            uint32_t currentPoolDropCount = g_WgcCap->GetPoolDropCount();
            uint32_t inputFrames = currentInputCount - lastInputCount;
            uint32_t deliveredFrames = currentCount - lastCallbackCount;
            uint32_t hostDropDelta =
                static_cast<uint32_t>(queueDropped >= lastHostDroppedCount ? (queueDropped - lastHostDroppedCount) : 0);
            uint32_t pacingSkipDelta = currentPacingSkipCount - lastPacingSkipCount;
            uint32_t throttleSkipDelta = currentThrottleSkipCount - lastThrottleSkipCount;
            uint32_t staleSkipDelta = currentStaleSkipCount - lastStaleSkipCount;
            uint32_t cursorSkipDelta = currentCursorSkipCount - lastCursorSkipCount;
            uint32_t poolDropDelta = currentPoolDropCount - lastPoolDropCount;
            uint32_t queuedFrames = deliveredFrames >= hostDropDelta ? (deliveredFrames - hostDropDelta) : 0;
            int64_t copyUs = g_WgcCap->GetLastCopyTimeUs();
            int64_t encodeUs = MediaEngine_GetLastFrameEncodeTimeUs();
            int64_t fenceUs = MediaEngine_GetLastFrameFenceWaitUs();
            uint32_t dupDelta = 0;
            uint32_t lateDelta = 0;
            uint32_t overloadFlags = 0;
            uint32_t muxQueueBytes = 0;
            uint32_t encoderQueueDepth = static_cast<uint32_t>(g_FrameQueue.Size());
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
            }

            LogInfo(
                "[WGC Perf] Input: %u | Queued: %u | DropFull: %u | DropPace: %u | DropThrottle: %u | "
                "DropStale: %u | DropCursor: %u | DropPool: %u | HostQ: %u | EncQ: %u | Dup: %u | Late: %u | "
                "Copy: %lldus | Encode: %lldus | Fence: %lldus | Mux: %uKB | Overload: 0x%X",
                inputFrames, queuedFrames, hostDropDelta, pacingSkipDelta, throttleSkipDelta, staleSkipDelta,
                cursorSkipDelta, poolDropDelta, static_cast<uint32_t>(g_FrameQueue.Size()), encoderQueueDepth, dupDelta,
                lateDelta, copyUs, encodeUs, fenceUs, (muxQueueBytes + 1023u) / 1024u, overloadFlags);

            lastInputCount = currentInputCount;
            lastCallbackCount = currentCount;
            lastHostDroppedCount = queueDropped;
            lastPacingSkipCount = currentPacingSkipCount;
            lastThrottleSkipCount = currentThrottleSkipCount;
            lastStaleSkipCount = currentStaleSkipCount;
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

    g_FrameQueue.StartRecording();

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
    QueuedFrame bufferedScreenGrabFrame;
    bool hasBufferedScreenGrabFrame = false;
    std::vector<QueuedFrame> drainedInjectFrames;
    drainedInjectFrames.reserve(8);
    std::deque<QueuedFrame> bufferedInjectFrames;
    double smoothedInjectFenceMs = 0.0;
    bool recordingOutputLive = false;
    uint64_t startupWarmupStartTick = GetTickCount64();
    uint32_t hiddenStartupFrames = 0;
    bool warmupWasScreenGrab = IsActiveScreenGrab();
    uint32_t pendingInjectTrimmedLogCount = 0;
    size_t maxBufferedInjectDepthSinceLog = 0;
    DWORD lastInjectTrimLog = GetTickCount();
    auto ClearBufferedInjectFrames = [&]() {
        while (!bufferedInjectFrames.empty()) {
            QueuedFrame queuedFrame = std::move(bufferedInjectFrames.front());
            bufferedInjectFrames.pop_front();
            DiscardQueuedFrame(queuedFrame);
        }
    };
    auto GetInjectReserveFrames = [&]() -> size_t {
        if (config.video.useVFR) {
            return 0;
        }

        // Start with minimal reserve (1 frame).  The GPU fence typically signals
        // well before the encoder reads the frame, so holding two frames in reserve
        // when the fence wait is near-zero only forces the encoder to duplicate
        // frames it could have consumed — the primary cause of CFR micro-stutter
        // when game FPS is close to or below the recording target.
        //
        // The reserve automatically scales up when the fence EMA shows the GPU
        // copy genuinely needs more lead time.
        const double reserveFramesNeeded = smoothedInjectFenceMs / frameIntervalMs;
        size_t reserveFrames = 1;
        if (reserveFramesNeeded > 0.5) {
            reserveFrames = 2;
        }
        if (reserveFramesNeeded > 1.25) {
            reserveFrames = 3;
        }
        if (reserveFramesNeeded > 2.25) {
            reserveFrames = 4;
        }
        return reserveFrames;
    };

    while (g_EncoderRunning || g_FrameQueue.Size() > 0 || hasBufferedScreenGrabFrame || !bufferedInjectFrames.empty()) {
        static DWORD lastThreadLog = 0;
        if (GetTickCount() - lastThreadLog > 1000) {
            LogInfo("[EncoderThread] Alive. QueueSize=%u Bottleneck=%d", (unsigned int)g_FrameQueue.Size(),
                    (int)g_IsEncoderBottlenecked);
            lastThreadLog = GetTickCount();
        }

        if (g_pSharedMem) {
            uint32_t queueDepth = (uint32_t)g_FrameQueue.Size();
            queueDepth += static_cast<uint32_t>(bufferedInjectFrames.size());
            if (hasBufferedScreenGrabFrame) {
                queueDepth += 1;
            }
            double fenceWaitMs = (double)MediaEngine_GetLastFrameFenceWaitUs() / 1000.0;
            const uint32_t queuePressureThreshold =
                std::max<uint32_t>(8u, static_cast<uint32_t>(g_FrameQueue.Capacity() / 2));
            bool shouldThrottle = queueDepth >= queuePressureThreshold || fenceWaitMs > 16.0;

            g_pSharedMem->encoderQueueDepth.store(queueDepth, std::memory_order_relaxed);
            g_pSharedMem->throttleCapture.store(shouldThrottle, std::memory_order_release);
            g_pSharedMem->runtimeState.hostDroppedFrames.store(static_cast<uint32_t>(g_FrameQueue.GetDroppedCount()));
        }

        if (g_EncoderRunning) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            int64_t waitTicks = nextSampleTime.QuadPart - now.QuadPart;

            if (waitTicks > 0 && hTimer) {
                int64_t wait100ns = (waitTicks * 10000000) / qpcFreq.QuadPart;
                LARGE_INTEGER dueTime;
                dueTime.QuadPart = -wait100ns;
                if (SetWaitableTimer(hTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                    WaitForSingleObject(hTimer, INFINITE);
                }
            } else if (waitTicks > qpcFreq.QuadPart / 1000) {
                int64_t waitMs = (waitTicks * 1000) / qpcFreq.QuadPart;
                if (waitMs > 1) {
                    Sleep((DWORD)waitMs - 1);
                }
            }

            nextSampleTime.QuadPart += targetIntervalTicks;

            // Preserve every overdue output tick once recording is live so CFR stays
            // phase-continuous and the file duration matches wall clock even if the
            // thread wakes up late for a few intervals. Hidden warmup can still
            // rebase freely because those frames are discarded before the file starts.
            QueryPerformanceCounter(&now);
            if (!recordingOutputLive && now.QuadPart > nextSampleTime.QuadPart + targetIntervalTicks * 2) {
                nextSampleTime = now;
            }
        }

        QueuedFrame frame;
        bool popped = false;

        if (IsActiveScreenGrab()) {
            if (!bufferedInjectFrames.empty()) {
                ClearBufferedInjectFrames();
            }
            smoothedInjectFenceMs = 0.0;
            if (!config.video.useVFR) {
                // Keep a tiny one-frame reserve for CFR WGC/screengrab capture. WGC
                // delivery often arrives in short bursts even when the average source
                // cadence is above the target FPS, and always taking the newest frame
                // turns those bursts into avoidable duplicate output ticks. Buffering
                // one future frame smooths that jitter without reintroducing a deep queue.
                drainedScreenGrabFrames.clear();
                QueuedFrame temp;
                while (g_FrameQueue.Pop(temp, 0)) {
                    if (g_RejectInjectFrames.load(std::memory_order_acquire) && temp.isInjectMode) {
                        DiscardQueuedFrame(temp);
                        continue;
                    }
                    drainedScreenGrabFrames.push_back(std::move(temp));
                }

                if (hasBufferedScreenGrabFrame) {
                    if (g_RejectInjectFrames.load(std::memory_order_acquire) && bufferedScreenGrabFrame.isInjectMode) {
                        DiscardQueuedFrame(bufferedScreenGrabFrame);
                        hasBufferedScreenGrabFrame = false;
                    }
                }

                if (hasBufferedScreenGrabFrame) {
                    frame = std::move(bufferedScreenGrabFrame);
                    hasBufferedScreenGrabFrame = false;
                    popped = true;
                }

                if (!drainedScreenGrabFrames.empty()) {
                    const size_t newestIndex = drainedScreenGrabFrames.size() - 1;
                    if (popped) {
                        for (size_t i = 0; i < newestIndex; ++i) {
                            ReleaseQueuedFrameTexture(drainedScreenGrabFrames[i]);
                        }
                        bufferedScreenGrabFrame = std::move(drainedScreenGrabFrames[newestIndex]);
                        hasBufferedScreenGrabFrame = true;
                    } else if (drainedScreenGrabFrames.size() >= 2) {
                        const size_t processIndex = newestIndex - 1;
                        for (size_t i = 0; i < processIndex; ++i) {
                            ReleaseQueuedFrameTexture(drainedScreenGrabFrames[i]);
                        }
                        frame = std::move(drainedScreenGrabFrames[processIndex]);
                        bufferedScreenGrabFrame = std::move(drainedScreenGrabFrames[newestIndex]);
                        hasBufferedScreenGrabFrame = true;
                        popped = true;
                    } else {
                        frame = std::move(drainedScreenGrabFrames[newestIndex]);
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
                    if (popped && !frame.isInjectMode && frame.texture) {
                        frame.texture->Release();
                    }
                    frame = std::move(temp);
                    popped = true;
                }
            }
        } else {
            if (hasBufferedScreenGrabFrame) {
                ReleaseQueuedFrameTexture(bufferedScreenGrabFrame);
                bufferedScreenGrabFrame = QueuedFrame{};
                hasBufferedScreenGrabFrame = false;
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

                const size_t injectReserveFrames = GetInjectReserveFrames();
                constexpr size_t kMaxInjectBufferedHeadroomFrames = 12;
                const size_t maxBufferedInjectFrames = injectReserveFrames + kMaxInjectBufferedHeadroomFrames;
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
                size_t minBufferedInjectFrames = injectReserveFrames;
                if (recordingOutputLive && minBufferedInjectFrames > 0) {
                    // Once recording is already live, allow the reserve to drain by one
                    // frame before duplicating so we don't visibly replay old frames
                    // while fresh inject frames are still buffered and ready.
                    minBufferedInjectFrames -= 1;
                }
                if (!g_EncoderRunning && !bufferedInjectFrames.empty()) {
                    frame = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    popped = true;
                } else if (bufferedInjectFrames.size() > minBufferedInjectFrames) {
                    frame = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    popped = true;
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

        if (popped && frame.isInjectMode && g_RejectInjectFrames.load(std::memory_order_acquire)) {
            DiscardQueuedFrame(frame);
            popped = false;
        }

        if (g_HasLastFrame && g_LastFrame.isInjectMode && g_RejectInjectFrames.load(std::memory_order_acquire)) {
            g_LastFrame = QueuedFrame{};
            g_HasLastFrame = false;
        }

        const bool useScreenGrab = IsActiveScreenGrab();
        if (!recordingOutputLive && useScreenGrab != warmupWasScreenGrab) {
            warmupWasScreenGrab = useScreenGrab;
            startupWarmupStartTick = GetTickCount64();
            hiddenStartupFrames = 0;
        }
        const size_t injectReserveFrames = (!useScreenGrab && !config.video.useVFR) ? GetInjectReserveFrames() : 0;
        if (!recordingOutputLive && g_Recording && g_EncoderRunning) {
            const uint64_t warmupElapsedMs64 = GetTickCount64() - startupWarmupStartTick;
            const DWORD warmupElapsedMs =
                warmupElapsedMs64 > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<DWORD>(warmupElapsedMs64);
            if (ShouldCommitRecordingWarmup(useScreenGrab, config.video.useVFR, popped, hasBufferedScreenGrabFrame,
                                            bufferedInjectFrames.size(), injectReserveFrames, warmupElapsedMs)) {
                recordingOutputLive = true;
                SetRecordingVisibleState(true);
                LogInfo("[EncoderThread] Recording live after %llums hidden warmup (%s, hiddenFrames=%u)",
                        static_cast<unsigned long long>(warmupElapsedMs64), useScreenGrab ? "WGC" : "inject",
                        hiddenStartupFrames);
            }
        }

        if (!recordingOutputLive) {
            if (popped) {
                ++hiddenStartupFrames;
                DiscardQueuedFrame(frame);
            }
            continue;
        }

        if (popped) {
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
                g_LastFrame = std::move(frame);
                g_HasLastFrame = true;
                // After move, frame.texture is nullptr - use g_LastFrame for processing
                frameToProcess = &g_LastFrame;
            }
        } else if (g_HasLastFrame && g_EncoderRunning && g_Recording) {
            // CFR FIX: Re-encode last frame when no new frame is available.
            // This applies to both screengrab and inject modes so output cadence
            // remains stable when source FPS dips below target.
            frameToProcess = &g_LastFrame;
            isDuplicate = true;
        }

        if (!g_EncoderRunning && !popped) {
            break;
        }

        if (frameToProcess) {
            LARGE_INTEGER startEnc, endEnc;
            QueryPerformanceCounter(&startEnc);

            if (isDuplicate && g_pSharedMem) {
                g_pSharedMem->runtimeState.duplicateFrames.fetch_add(1, std::memory_order_relaxed);
            }

            if (frameToProcess->isInjectMode) {
                MediaEngine_ProcessFrame((uint64_t)frameToProcess->sharedHandle, (uint64_t)frameToProcess->fenceHandle,
                                         frameToProcess->fenceValue, frameToProcess->timestamp, frameToProcess->luidLow,
                                         frameToProcess->luidHigh, frameToProcess->sourcePid, frameToProcess->width,
                                         frameToProcess->height, frameToProcess->format, frameToProcess->isHDR,
                                         frameToProcess->isShmem, frameToProcess->shmemSlot);
            } else {
                MediaEngine_ProcessFrameD3D11(frameToProcess->texture, frameToProcess->timestamp, frameToProcess->width,
                                              frameToProcess->height, frameToProcess->isHDR,
                                              frameToProcess->captureLeft, frameToProcess->captureTop);
            }

            QueryPerformanceCounter(&endEnc);
            double currentEncodeMs = (double)(endEnc.QuadPart - startEnc.QuadPart) * 1000.0 / qpcFreq.QuadPart;

            double pureEncodeMs = (double)MediaEngine_GetLastFrameEncodeTimeUs() / 1000.0;

            if (smoothedEncodeMs == 0.0) {
                smoothedEncodeMs = pureEncodeMs;
            } else {
                smoothedEncodeMs = smoothedEncodeMs * 0.95 + pureEncodeMs * 0.05;
            }

            if (g_pSharedMem && currentEncodeMs > frameIntervalMs * 1.10) {
                g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
            }

            if (popped && frameToProcess->isInjectMode) {
                const double currentFenceMs = (double)MediaEngine_GetLastFrameFenceWaitUs() / 1000.0;
                if (smoothedInjectFenceMs == 0.0) {
                    smoothedInjectFenceMs = currentFenceMs;
                } else {
                    smoothedInjectFenceMs = smoothedInjectFenceMs * 0.90 + currentFenceMs * 0.10;
                }
            }

            g_IsEncoderBottlenecked.store(smoothedEncodeMs > frameIntervalMs * 0.95, std::memory_order_relaxed);

            static DWORD lastWarningTime = 0;
            if (smoothedEncodeMs > frameIntervalMs * 0.85) {
                DWORD now = GetTickCount();
                if (now - lastWarningTime > 5000) {
                    LogInfo(
                        "[WARN] Encoder approaching capacity: %.2fms avg vs %.2fms "
                        "budget",
                        smoothedEncodeMs, frameIntervalMs);
                    lastWarningTime = now;
                }
            }

            if (popped && frameToProcess->isInjectMode && g_pSharedMem) {
                g_pSharedMem->frameRing.readIndex.store(frameToProcess->ringIndex + 1, std::memory_order_release);
            }
        }

        if (popped && !frame.isInjectMode && frame.texture) {
            frame.texture->Release();
        }
    }

    if (hTimer) {
        CloseHandle(hTimer);
    }

    if (hasBufferedScreenGrabFrame) {
        ReleaseQueuedFrameTexture(bufferedScreenGrabFrame);
    }

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

    if (g_pSharedMem) {
        g_pSharedMem->runtimeState.duplicateFrames = 0;
        g_pSharedMem->runtimeState.lateFrames = 0;
        g_pSharedMem->runtimeState.encoderOverloadFlags.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.muxQueueBytes.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.hostDroppedFrames.store(0, std::memory_order_relaxed);
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

    g_Recording = false;
    SetCaptureRequestedState(false);
    SetRecordingVisibleState(false);

    StopWgcCapturePipeline();
    StopInjectCapturePipeline();

    g_EncoderRunning = false;
    g_InjectDeliveredFirstFrame.store(false, std::memory_order_release);
    g_RejectInjectFrames.store(false, std::memory_order_release);
    g_AutoWgcFallbackArmed.store(false, std::memory_order_release);

    JoinThreadWithTimeout(g_EncoderThread, 10000, "encoder");

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
        g_pSharedMem->runtimeState.encoderOverloadFlags.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.muxQueueBytes.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.hostDroppedFrames.store(0, std::memory_order_relaxed);
        g_pSharedMem->encoderQueueDepth.store(0, std::memory_order_relaxed);
        g_pSharedMem->throttleCapture.store(false, std::memory_order_release);
    }

    SetActiveScreenGrab(false);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    LogInfo("[Media] Recording stopped");
}

int MediaProcessMain(const AppConfig& initialConfig) {
    AppConfig config = initialConfig;
    timeBeginPeriod(1);

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

        config = std::move(resolvedConfig);
        activeConfigSourcePid = sourcePid;
        activeConfigProcessName = processName;

        ApplyMediaProcessPriority(config);
        if (g_WgcCap) {
            g_WgcCap->SetCaptureCursor(config.video.captureCursor);
        }
        if (mediaEngineReady) {
            MediaEngine_SetLogCallback(config.debugLogging ? MediaLogCallback : nullptr);
            MediaEngine_ReloadConfig(&config);
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
                const DWORD fallbackDelayMs = (activeSourcePid == 0) ? 100 : 200;
                if (elapsed > fallbackDelayMs) {
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
