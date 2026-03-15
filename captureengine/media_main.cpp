// clang-format off
#include <windows.h>
#include <d3d11.h>
#include <psapi.h>
#include <timeapi.h>
// clang-format on
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
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

static FrameQueue g_FrameQueue(8);
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
static std::atomic<uint32_t> g_WgcDroppedFrames{0};

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

static void QueueWgcFrame(ID3D11Texture2D* texture, uint32_t width, uint32_t height, int64_t timestamp) {
    QueuedFrame qf;
    qf.isInjectMode = false;
    qf.texture = texture;
    qf.width = width;
    qf.height = height;
    qf.timestamp = timestamp;

    if (!g_FrameQueue.Push(std::move(qf), g_IsEncoderBottlenecked)) {
        g_WgcDroppedFrames.fetch_add(1, std::memory_order_relaxed);
        texture->Release();
    }
}

static void StopInjectCapturePipeline() {
    g_InjectCaptureShutdown = true;
    JoinThreadWithTimeout(g_InjectCaptureThread, 5000, "inject capture");
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
    if (!g_WgcCap->StartCapture()) {
        g_WgcCap->SetDirectFrameCallback(nullptr);
        return false;
    }

    g_WgcCap->ResetStats();
    g_WgcCap->SetTargetFps(config.video.fps * 2);
    g_WgcDroppedFrames.store(0, std::memory_order_relaxed);

    g_WgcCaptureShutdown = false;
    g_WgcCaptureThread = std::thread(WgcCaptureThreadFunc, std::ref(config));
    SetThreadPriority(reinterpret_cast<HANDLE>(g_WgcCaptureThread.native_handle()), THREAD_PRIORITY_ABOVE_NORMAL);
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
        "[Inject Thread] Started (High Priority Polling with Source-Side "
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
                // If this frame is too early relative to our target 120Hz grid, drop
                // it. This acts as a smart decimator for 144Hz/200Hz inputs.
                bool shouldProcess = false;

                if (nextPushTime == 0) {
                    // First frame or resync
                    nextPushTime = slot.timestamp;
                    shouldProcess = true;
                } else {
                    // IMPROVED PACING: Use a more lenient jitter window to reduce drops
                    // Old: half-interval was too aggressive for high FPS games (144Hz+,
                    // 240Hz+) New: allow up to 80% of interval before dropping, with
                    // adaptive resync
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
                        if (g_FrameQueue.Push(std::move(qf), g_IsEncoderBottlenecked)) {
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
        if (now - lastLog >= 2000) {
            if (pushedCount > 0 || droppedCount > 0 || pacingDroppedCount > 0) {
                LogInfo(
                    "[Inject Thread] Pushed: %u, Dropped(Full): %u, "
                    "Dropped(Pacing): %u",
                    pushedCount, droppedCount, pacingDroppedCount);
                pushedCount = 0;
                droppedCount = 0;
                pacingDroppedCount = 0;
            }
            lastLog = now;
        }
    }

    g_InjectCaptureRunning = false;
    LogInfo("[Inject Thread] Stopped");
}

void WgcCaptureThreadFunc(const AppConfig& config) {
    LogInfo("[WGC CaptureThread] Started (OBS-style direct callback mode)");
    g_WgcCaptureRunning = true;

    DWORD lastDiagTime = GetTickCount();
    uint32_t lastCallbackCount = 0;
    uint32_t totalDroppedAtStart = g_WgcDroppedFrames.load(std::memory_order_relaxed);

    while (!g_WgcCaptureShutdown) {
        Sleep(1000);

        if (!g_Recording || !g_WgcCap) {
            lastCallbackCount = 0;
            lastDiagTime = GetTickCount();
            continue;
        }

        DWORD now = GetTickCount();
        if (now - lastDiagTime >= 1000) {
            uint32_t currentCount = g_WgcCap->GetCallbackFrameCount();
            uint32_t framesThisSecond = currentCount - lastCallbackCount;
            uint32_t totalDropped = g_WgcDroppedFrames.load(std::memory_order_relaxed) - totalDroppedAtStart;
            int64_t copyUs = g_WgcCap->GetLastCopyTimeUs();
            int64_t encodeUs = MediaEngine_GetLastFrameEncodeTimeUs();
            uint32_t skipped = g_WgcCap->GetSkippedFrameCount();

            LogInfo("[WGC Perf] FPS: %u | Queue: %u | Dropped: %u | Skipped: %u | Copy: %lldus | Encode: %lldus",
                    framesThisSecond, (uint32_t)g_FrameQueue.Size(), totalDropped, skipped, copyUs, encodeUs);

            lastCallbackCount = currentCount;
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

    while (g_EncoderRunning || g_FrameQueue.Size() > 0) {
        static DWORD lastThreadLog = 0;
        if (GetTickCount() - lastThreadLog > 1000) {
            LogInfo("[EncoderThread] Alive. QueueSize=%u Bottleneck=%d", (unsigned int)g_FrameQueue.Size(),
                    (int)g_IsEncoderBottlenecked);
            lastThreadLog = GetTickCount();
        }

        if (g_pSharedMem) {
            uint32_t queueDepth = (uint32_t)g_FrameQueue.Size();
            double fenceWaitMs = (double)MediaEngine_GetLastFrameFenceWaitUs() / 1000.0;
            bool shouldThrottle = queueDepth > 3 || fenceWaitMs > 16.0;

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

            QueryPerformanceCounter(&now);
            if (now.QuadPart > nextSampleTime.QuadPart + targetIntervalTicks * 2) {
                nextSampleTime = now;
            }
        }

        QueuedFrame frame;
        bool popped = false;

        if (IsActiveScreenGrab()) {
            // WGC/screengrab: drain queue and take newest frame.
            // Reduces latency from ~56ms (queue depth 8) to 0-7ms
            // while producing identical content selection (same Bresenham pattern).
            QueuedFrame temp;
            while (g_FrameQueue.Pop(temp, 0)) {
                if (popped && !frame.isInjectMode && frame.texture) {
                    frame.texture->Release();
                }
                frame = std::move(temp);
                popped = true;
            }
        } else {
            // Inject: drain the queue and use the newest frame for this output tick.
            // DX9/DXVK sources can run slightly ahead of the encoder cadence, leaving
            // a small steady backlog. Sampling the newest queued frame reduces stale
            // frame selection and visible judder without changing the fixed output rate.
            QueuedFrame temp;
            while (g_FrameQueue.Pop(temp, 0)) {
                frame = std::move(temp);
                popped = true;
            }
        }

        QueuedFrame* frameToProcess = nullptr;
        bool isDuplicate = false;

        if (popped) {
            if (frame.isInjectMode) {
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
        } else if (g_HasLastFrame && g_EncoderRunning) {
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
                                              frameToProcess->height);
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

            g_IsEncoderBottlenecked = (smoothedEncodeMs > frameIntervalMs * 0.95);

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
        // Also clear stale recording state from crashed session
        g_pSharedMem->runtimeState.isRecording.store(false, std::memory_order_release);
        g_pSharedMem->runtimeState.recordingStartTime.store(0, std::memory_order_release);
    }

    // Reset inject session state so main loop re-initializes on new recording
    g_InjectSessionReset.store(true, std::memory_order_release);

    StopWgcCapturePipeline();
    StopInjectCapturePipeline();

    g_FrameQueue.Clear();
    if (g_HasLastFrame && !g_LastFrame.isInjectMode && g_LastFrame.texture) {
        g_LastFrame.texture->Release();
        g_LastFrame.texture = nullptr;
    }
    g_HasLastFrame = false;

    if (g_pSharedMem) {
        g_pSharedMem->runtimeState.duplicateFrames = 0;
        g_pSharedMem->runtimeState.lateFrames = 0;
        g_pSharedMem->runtimeState.encoderOverloadFlags.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.muxQueueBytes.store(0, std::memory_order_relaxed);
    }

    if (!MediaEngine_StartRecording()) {
        LogError("[Media] Failed to start MediaEngine recording");
        return;
    }

    g_Recording = true;
    g_EncoderRunning = true;

    if (g_pSharedMem) {
        g_pSharedMem->runtimeState.isRecording.store(true, std::memory_order_release);
        g_pSharedMem->runtimeState.recordingStartTime.store(GetTickCount64(), std::memory_order_release);
    }

    g_EncoderThread = std::thread(EncoderThreadFunc, std::ref(config));
    SetThreadPriority(reinterpret_cast<HANDLE>(g_EncoderThread.native_handle()), THREAD_PRIORITY_ABOVE_NORMAL);

    if (useScreenGrab && g_WgcCap) {
        if (!StartWgcRecordingCapture(config)) {
            LogError("[Media] Failed to start WGC capture");
            g_EncoderRunning = false;
            JoinThreadWithTimeout(g_EncoderThread, 10000, "encoder");
            g_Recording = false;
            if (g_pSharedMem) {
                g_pSharedMem->runtimeState.isRecording.store(false, std::memory_order_release);
                g_pSharedMem->runtimeState.recordingStartTime.store(0, std::memory_order_release);
            }
            MediaEngine_StopRecording();
            SetActiveScreenGrab(false);
            return;
        }
        LogInfo("[Media] WGC capture with direct callback started");
    } else if (!useScreenGrab) {
        LogInfo("[Media] Starting InjectCaptureThread for Shared Memory Capture");
        g_InjectCaptureShutdown = false;
        g_InjectCaptureThread = std::thread(InjectCaptureThreadFunc, std::ref(config));
        SetThreadPriority(reinterpret_cast<HANDLE>(g_InjectCaptureThread.native_handle()),
                          THREAD_PRIORITY_ABOVE_NORMAL);
    }

    LogInfo("[Media] Recording started");
}

void StopRecording() {
    if (!g_Recording)
        return;

    LogInfo("[Media] Stopping recording...");

    g_Recording = false;

    StopWgcCapturePipeline();
    StopInjectCapturePipeline();

    g_EncoderRunning = false;

    JoinThreadWithTimeout(g_EncoderThread, 10000, "encoder");

    g_FrameQueue.Clear();
    MediaEngine_StopRecording();

    if (g_pSharedMem) {
        g_pSharedMem->runtimeState.isRecording.store(false, std::memory_order_release);
        g_pSharedMem->runtimeState.recordingStartTime.store(0, std::memory_order_release);
        g_pSharedMem->runtimeState.encoderOverloadFlags.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.muxQueueBytes.store(0, std::memory_order_relaxed);
    }

    SetActiveScreenGrab(false);

    LogInfo("[Media] Recording stopped");
}

int MediaProcessMain(const AppConfig& config) {
    timeBeginPeriod(1);

    // Get exe directory for DLL loading
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string exeDir = std::string(exePath);
    exeDir = exeDir.substr(0, exeDir.find_last_of("\\/"));
    const std::string configPath = GetLocalConfigPath();

    // Load mediaengine.dll dynamically (with FFmpeg DLLs in ffmpeg/ subfolder)
    if (!MediaEngine_Load(exeDir.c_str())) {
        LogError("[Media] Failed to load mediaengine.dll");
        return 1;
    }

    ApplyMediaProcessPriority(config);

    ProcessIPCServer ipc(ProcessMode::Media);
    if (!ipc.Init()) {
        LogError("[Media] Failed to initialize IPC");
        timeEndPeriod(1);
        return 1;
    }

    MediaEngine_SetLogCallback(config.debugLogging ? MediaLogCallback : nullptr);
    if (!MediaEngine_Init(&config)) {
        LogError("[Media] Failed to initialize MediaEngine");
        timeEndPeriod(1);
        return 1;
    }
    LogInfo("[Media] MediaEngine initialized");

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

        MediaEngine_SetSharedMem(g_pSharedMem, g_pShmem);

        // Create shared capture textures immediately so Vulkan layer doesn't timeout
        // waiting for them. The textures will be created with current dimensions
        // and resized if needed when SetDimensions is called later.
        // Skip if screengrab mode - encoder will use WGC's shared device instead.
        if (!IsPreferredScreenGrab()) {
            uint32_t width = g_pSharedMem->GetWidth();
            uint32_t height = g_pSharedMem->GetHeight();
            uint32_t format = g_pSharedMem->GetFormat();
            if (width > 0 && height > 0 && !g_pSharedMem->encoderTextures.ready.load(std::memory_order_acquire)) {
                LogInfo("[Media] Creating shared capture textures early: %dx%d format=%d", width, height, format);
                if (MediaEngine_CreateSharedCaptureTextures(width, height, format, g_pSharedMem)) {
                    LogInfo("[Media] Shared capture textures created successfully");
                } else {
                    LogWarn("[Media] Failed to create shared capture textures early - will retry on first frame");
                }
            }
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
        MediaEngine_Shutdown();
        timeEndPeriod(1);
        return 1;
    } else {
        SetPreferredScreenGrab(true);
        LogInfo("[Media] Shared memory not available - using screengrab mode");
    }

    if (IsPreferredScreenGrab() || config.captureMethod == "auto") {
        d3dDevice = MediaEngine_GetD3D11Device();
        if (!d3dDevice) {
            if (IsPreferredScreenGrab()) {
                LogError("[Media] Failed to get D3D11 device");
                MediaEngine_Shutdown();
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
                    LogInfo("[Media] WGC capture initialized%s",
                            IsPreferredScreenGrab() ? "" : " (standby for auto fallback)");
                } else {
                    if (IsPreferredScreenGrab()) {
                        LogError("[Media] WGC capture init failed");
                        MediaEngine_Shutdown();
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

    while (g_Running) {
        // WGC window detection must run BEFORE resolution polling/texture creation.
        // CreateSharedCaptureTextures sets the encoder's LUID device, which conflicts
        // with WGC's shared device. By scanning first, the preferred capture mode is set correctly
        // and we skip the LUID-based texture creation for WGC games.
        if (g_pSharedMem && !config.wgcWindowTitles.empty() && !IsPreferredScreenGrab() && !g_Recording) {
            static DWORD lastEarlyWgcScan = 0;
            DWORD now = GetTickCount();
            if (now - lastEarlyWgcScan > 500) {
                lastEarlyWgcScan = now;

                struct WgcSearchContext {
                    const std::vector<WhitelistEntry>* targets;
                    HWND result;
                    int checked;
                };
                WgcSearchContext ctx = {&config.wgcWindowTitles, NULL, 0};
                EnumWindows(
                    [](HWND hwnd, LPARAM lParam) -> BOOL {
                        WgcSearchContext* context = (WgcSearchContext*)lParam;
                        if (!IsWindowVisible(hwnd))
                            return TRUE;
                        if (GetWindow(hwnd, GW_OWNER) != 0)
                            return TRUE;

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
                                    if (pos != std::string::npos)
                                        procName = procName.substr(pos + 1);
                                    std::transform(procName.begin(), procName.end(), procName.begin(), ::tolower);
                                }
                                CloseHandle(hProcess);
                            }
                        }

                        for (const auto& entry : *context->targets) {
                            MatchMode mode = entry.mode;
                            bool matched = false;

                            // Window title matching (if entry has windowName)
                            if (entry.HasWindow()) {
                                std::string winLower = entry.windowName;
                                std::transform(winLower.begin(), winLower.end(), winLower.begin(), ::tolower);

                                if (mode == MatchMode::kExact) {
                                    if (!titleStr.empty() && titleStr == winLower)
                                        matched = true;
                                } else {
                                    // title_executable or title_type: substring match on title
                                    if (!titleStr.empty() && titleStr.find(winLower) != std::string::npos)
                                        matched = true;
                                    // title_type: also try window class
                                    if (!matched && mode == MatchMode::kTitleType && !classStr.empty() &&
                                        classStr.find(winLower) != std::string::npos)
                                        matched = true;
                                }
                            }

                            // Process/exe name matching (if entry has pattern)
                            if (!matched && entry.HasProcess() && !procName.empty()) {
                                std::string procLower = entry.pattern;
                                std::transform(procLower.begin(), procLower.end(), procLower.begin(), ::tolower);

                                if (mode == MatchMode::kExact) {
                                    if (procName == procLower)
                                        matched = true;
                                } else {
                                    if (procName == procLower || procName.find(procLower) != std::string::npos)
                                        matched = true;
                                }
                            }

                            if (matched) {
                                context->result = hwnd;
                                return FALSE;
                            }
                        }
                        return TRUE;
                    },
                    (LPARAM)&ctx);
                if (ctx.result) {
                    SetPreferredScreenGrab(true);
                }
            }
        }

        // Poll for resolution availability and create encoder textures early.
        // The Vulkan layer sets resolution when it creates the swapchain, then waits
        // for encoder KMT textures. We must create them ASAP to avoid timeout.
        // Skip when using screengrab (WGC) - the encoder should use the shared device.
        if (g_pSharedMem && !IsPreferredScreenGrab() &&
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
                    StartRecording(config);
                    ipc.SendResponse(ProcessResponse::RecordingStarted);
                    break;
                case ProcessCommand::StopRecording:
                    StopRecording();
                    ipc.SendResponse(ProcessResponse::RecordingStopped);
                    break;
                case ProcessCommand::Ping:
                    ipc.SendResponse(ProcessResponse::Pong);
                    break;
                case ProcessCommand::ReloadConfig: {
                    AppConfig reloadedConfig;
                    LoadConfig(configPath, reloadedConfig);
                    if (g_pSharedMem) {
                        const uint32_t sourcePid = g_pSharedMem->GetSourcePid();
                        if (sourcePid != 0) {
                            const std::string processName = GetProcessNameFromPID(sourcePid);
                            if (!processName.empty() && processName != "unknown") {
                                LoadConfig(configPath, reloadedConfig, processName);
                            }
                        }
                    }

                    ApplyMediaProcessPriority(reloadedConfig);
                    MediaEngine_SetLogCallback(reloadedConfig.debugLogging ? MediaLogCallback : nullptr);
                    if (g_WgcCap) {
                        g_WgcCap->SetCaptureCursor(reloadedConfig.video.captureCursor);
                    }
                    MediaEngine_ReloadConfig(&reloadedConfig);
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
                    StartRecording(config);
                    g_pSharedMem->runtimeState.ackRecordingStarted.store(true, std::memory_order_release);
                }
            }
            if (LoadAcquire(g_pSharedMem->runtimeState.cmdStopRecording)) {
                StoreRelease(g_pSharedMem->runtimeState.cmdStopRecording, false);
                if (g_Recording) {
                    StopRecording();
                    g_pSharedMem->runtimeState.ackRecordingStopped.store(true, std::memory_order_release);
                }
            }

            static DWORD lastWindowScanTime = 0;
            static HWND currentCapturedWindow = NULL;

            DWORD now = GetTickCount();
            if (!g_Recording && !config.wgcWindowTitles.empty() && (now - lastWindowScanTime > 1000)) {
                lastWindowScanTime = now;

                struct WgcSearchContext {
                    const std::vector<WhitelistEntry>* targets;
                    HWND result;
                    int checked;
                };

                WgcSearchContext ctx = {&config.wgcWindowTitles, NULL, 0};

                EnumWindows(
                    [](HWND hwnd, LPARAM lParam) -> BOOL {
                        WgcSearchContext* context = (WgcSearchContext*)lParam;
                        if (!IsWindowVisible(hwnd))
                            return TRUE;
                        if (GetWindow(hwnd, GW_OWNER) != 0)
                            return TRUE;

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
                                    if (pos != std::string::npos)
                                        procName = procName.substr(pos + 1);
                                    std::transform(procName.begin(), procName.end(), procName.begin(), ::tolower);
                                }
                                CloseHandle(hProcess);
                            }
                        }

                        for (const auto& entry : *context->targets) {
                            MatchMode mode = entry.mode;
                            bool matched = false;

                            // Window title matching (if entry has windowName)
                            if (entry.HasWindow()) {
                                std::string winLower = entry.windowName;
                                std::transform(winLower.begin(), winLower.end(), winLower.begin(), ::tolower);

                                if (mode == MatchMode::kExact) {
                                    if (!titleStr.empty() && titleStr == winLower)
                                        matched = true;
                                } else {
                                    if (!titleStr.empty() && titleStr.find(winLower) != std::string::npos)
                                        matched = true;
                                    if (!matched && mode == MatchMode::kTitleType && !classStr.empty() &&
                                        classStr.find(winLower) != std::string::npos)
                                        matched = true;
                                }
                            }

                            // Process/exe name matching (if entry has pattern)
                            if (!matched && entry.HasProcess() && !procName.empty()) {
                                std::string procLower = entry.pattern;
                                std::transform(procLower.begin(), procLower.end(), procLower.begin(), ::tolower);

                                if (mode == MatchMode::kExact) {
                                    if (procName == procLower)
                                        matched = true;
                                } else {
                                    if (procName == procLower || procName.find(procLower) != std::string::npos)
                                        matched = true;
                                }
                            }

                            if (matched) {
                                context->result = hwnd;
                                return FALSE;
                            }
                        }
                        return TRUE;
                    },
                    (LPARAM)&ctx);

                HWND foundWindow = ctx.result;

                if (foundWindow && foundWindow != currentCapturedWindow) {
                    LogInfo(
                        "[Media] WGC Trigger: Found window (0x%p) matching config. "
                        "Switching capture...",
                        foundWindow);

                    if (!ensureWgcDevice()) {
                        LogWarn("[Media] WGC trigger ignored: D3D11 device unavailable");
                    } else {
                        g_WgcCap.reset();
                        g_WgcCap = std::make_unique<WGCCapture>();

                        if (g_WgcCap->InitForWindow(d3dDevice, foundWindow)) {
                            g_WgcCap->SetCaptureCursor(config.video.captureCursor);
                            g_WgcCap->SetThrottleFlag(&g_IsEncoderBottlenecked);
                            SetPreferredScreenGrab(true);
                            LogInfo("[Media] WGC target primed for window 0x%p", foundWindow);
                            currentCapturedWindow = foundWindow;
                        } else {
                            LogError("[Media] Failed to init WGC for found window.");
                            g_WgcCap.reset();
                            g_WgcCap = std::make_unique<WGCCapture>();
                            if (g_WgcCap->Init(d3dDevice)) {
                                g_WgcCap->SetThrottleFlag(&g_IsEncoderBottlenecked);
                                SetPreferredScreenGrab(true);
                            } else {
                                g_WgcCap.reset();
                                SetPreferredScreenGrab(false);
                            }
                            currentCapturedWindow = NULL;
                        }
                    }
                } else if (!foundWindow && currentCapturedWindow != NULL) {
                    if (!IsWindow(currentCapturedWindow)) {
                        LogInfo(
                            "[Media] Captured window 0x%p no longer valid. Reverting "
                            "to monitor/inject.",
                            currentCapturedWindow);
                        currentCapturedWindow = NULL;
                        if (ensureWgcDevice()) {
                            g_WgcCap.reset();
                            g_WgcCap = std::make_unique<WGCCapture>();
                            if (!g_WgcCap->Init(d3dDevice)) {
                                g_WgcCap.reset();
                                SetPreferredScreenGrab(false);
                            } else {
                                g_WgcCap->SetThrottleFlag(&g_IsEncoderBottlenecked);
                                SetPreferredScreenGrab(true);
                            }
                        } else {
                            SetPreferredScreenGrab(false);
                        }
                    }
                }
            }

            static uint32_t lastSourcePid = 0;
            uint32_t currentSourcePid = g_pSharedMem->GetSourcePid();

            if (currentSourcePid != 0 && currentSourcePid != lastSourcePid) {
                lastSourcePid = currentSourcePid;
                std::string procName = GetProcessNameFromPID(currentSourcePid);
                LogInfo("[Media] Hook connected: %s (PID: %u)", procName.c_str(), currentSourcePid);

                bool forceWGC = false;
                if (!config.overlayWhitelist.empty()) {
                    std::string lowerName = procName;
                    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

                    for (const auto& entry : config.overlayWhitelist) {
                        if (!entry.HasProcess())
                            continue;

                        std::string lowerItem = entry.pattern;
                        std::transform(lowerItem.begin(), lowerItem.end(), lowerItem.begin(), ::tolower);

                        if (entry.mode == MatchMode::kExact) {
                            if (lowerName == lowerItem) {
                                forceWGC = true;
                                LogInfo("[Media] Overlay Whitelist Match! Forcing WGC for %s", procName.c_str());
                                break;
                            }
                        } else {
                            if (lowerName == lowerItem || lowerName.find(lowerItem) != std::string::npos) {
                                forceWGC = true;
                                LogInfo("[Media] Overlay Whitelist Match! Forcing WGC for %s", procName.c_str());
                                break;
                            }
                        }
                    }
                }

                if (!g_Recording && forceWGC) {
                    if (!ensureWgcDevice()) {
                        LogWarn("[Media] Overlay whitelist requested WGC but D3D11 device unavailable");
                        SetPreferredScreenGrab(false);
                    } else {
                        SetPreferredScreenGrab(true);

                        HWND hGameWindow = GetMainWindowForProcess(currentSourcePid);
                        if (hGameWindow) {
                            LogInfo(
                                "[Media] Whitelist Optimization: Found main window 0x%p. "
                                "Switching WGC to Window Mode.",
                                hGameWindow);

                            g_WgcCap.reset();
                            g_WgcCap = std::make_unique<WGCCapture>();
                            if (g_WgcCap->InitForWindow(d3dDevice, hGameWindow)) {
                                g_WgcCap->SetCaptureCursor(config.video.captureCursor);
                                g_WgcCap->SetThrottleFlag(&g_IsEncoderBottlenecked);
                                LogInfo("[Media] WGC window target primed for PID %u", currentSourcePid);
                                currentCapturedWindow = hGameWindow;
                            } else {
                                LogError(
                                    "[Media] Failed to init WGC for window - falling back "
                                    "to Monitor");
                                g_WgcCap.reset();
                                g_WgcCap = std::make_unique<WGCCapture>();
                                if (!g_WgcCap->Init(d3dDevice)) {
                                    g_WgcCap.reset();
                                    SetPreferredScreenGrab(false);
                                } else {
                                    g_WgcCap->SetThrottleFlag(&g_IsEncoderBottlenecked);
                                    currentCapturedWindow = NULL;
                                }
                            }
                        } else {
                            LogInfo(
                                "[Media] Whitelist: No main window found for PID %u. Using "
                                "Monitor Capture.",
                                currentSourcePid);
                            g_WgcCap.reset();
                            g_WgcCap = std::make_unique<WGCCapture>();
                            if (!g_WgcCap->Init(d3dDevice)) {
                                g_WgcCap.reset();
                                SetPreferredScreenGrab(false);
                            } else {
                                g_WgcCap->SetThrottleFlag(&g_IsEncoderBottlenecked);
                                currentCapturedWindow = NULL;
                            }
                        }
                    }

                } else if (!g_Recording && config.captureMethod != "screengrab" &&
                           config.captureMethod != "framegrab") {
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
                uint32_t totalDropped =
                    g_WgcDroppedFrames.load(std::memory_order_relaxed) + (uint32_t)g_FrameQueue.GetDroppedCount();
                g_pSharedMem->runtimeState.hostDroppedFrames.store(totalDropped);
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

            if (!receivedFirstFrame && config.captureMethod == "auto" && g_WgcCap) {
                DWORD elapsed = GetTickCount() - injectModeStartTime;
                if (elapsed > 200) {
                    LogInfo(
                        "[Media] No frames from inject mode after %lums - falling "
                        "back to WGC",
                        elapsed);

                    StopInjectCapturePipeline();
                    if (StartWgcRecordingCapture(config)) {
                        SetActiveScreenGrab(true);
                        LogInfo("[Media] Switched to WGC capture mode with direct callback");
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
    if (d3dDevice)
        d3dDevice->Release();

    // Shutdown media engine BEFORE unmapping shared memory to avoid use-after-free
    // (VideoEncoder::CleanupResources accesses pSharedMem during destruction)
    MediaEngine_Shutdown();
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
