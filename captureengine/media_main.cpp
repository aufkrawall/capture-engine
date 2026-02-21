#include "../common/config.h"
#include "../common/frame_queue.h"
#include "../common/frame_timing.h"
#include "../common/logging.h"
#include "../common/process_ipc.h"
#include "../common/shared_defs.h"
#include "mediaengine_loader.h"
#include "dxgi_capture.h"
#include "wgc_capture.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <d3d11.h>
#include <mutex>
#include <psapi.h>
#include <string>
#include <thread>
#include <timeapi.h>
#include <windows.h>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

static std::atomic<bool> g_Running{true};
static std::atomic<bool> g_Recording{false};
static std::atomic<bool> g_EncoderRunning{false};
static std::atomic<bool> g_IsEncoderBottlenecked{false};

static FrameQueue g_FrameQueue(5);
static std::thread g_EncoderThread;
static QueuedFrame g_LastFrame;
static bool g_HasLastFrame = false;

// Screengrab mode components
static std::unique_ptr<WGCCapture> g_WgcCap;
static std::unique_ptr<DxgiCapture> g_DxgiCap;
static bool g_UseScreenGrab = false;

// Shared memory for hook communication
static HANDLE g_hMapFile = NULL;
static SharedMemoryLayout *g_pSharedMem = nullptr;

static HANDLE g_hMapShmem = NULL;
static ShmemBuffer *g_pShmem = nullptr;

// Inject thread specific
static std::atomic<bool> g_InjectCaptureRunning{false};
static std::atomic<bool> g_InjectCaptureShutdown{false};
static std::thread g_InjectCaptureThread;

// WGC thread specific
static std::atomic<bool> g_WgcCaptureRunning{false};
static std::atomic<bool> g_WgcCaptureShutdown{false};
static std::thread g_WgcCaptureThread;
static std::atomic<uint32_t> g_WgcDroppedFrames{0};

// Forward declaration
void StopRecording();
void StartRecording(const AppConfig &config);
void MediaLogCallback(const char *msg) { LogInfo("[Media] %s", msg); }

// Helper: Get process name from PID
// Window finding helper
struct WindowSearch {
  DWORD pid;
  HWND hwnd;
};

static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
  WindowSearch *search = (WindowSearch *)lParam;
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
        return FALSE; // Found, stop
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

static std::string GetProcessNameFromPID(DWORD pid) {
  char buffer[MAX_PATH] = "unknown";
  HANDLE hProcess =
      OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (hProcess) {
    if (GetModuleBaseNameA(hProcess, NULL, buffer, MAX_PATH)) {
      // Success
    }
    CloseHandle(hProcess);
  }
  // Strip path if present
  std::string name = buffer;
  size_t lastSlash = name.find_last_of("\\/");
  if (lastSlash != std::string::npos) {
    name = name.substr(lastSlash + 1);
  }
  return name;
}

// =================================================================================================
// THREAD FUNCTIONS
// =================================================================================================

void InjectCaptureThreadFunc(const AppConfig &config) {
  LogInfo("[Inject Thread] Started (High Priority Polling with Source-Side "
          "Pacing)");
  g_InjectCaptureRunning = true;

  if (!g_pSharedMem) {
    LogError("[Inject Thread] Shared memory not available! Aborting.");
    g_InjectCaptureRunning = false;
    return;
  }

  // Local read index tracks what WE have pushed to the FrameQueue
  uint32_t localReadIndex =
      g_pSharedMem->frameRing.readIndex.load(std::memory_order_acquire);

  // PACING INITIALIZATION
  LARGE_INTEGER qpcFreq;
  QueryPerformanceFrequency(&qpcFreq);
  // Target interval in ticks (e.g. 1/120s)
  int64_t targetIntervalTicks = (config.video.fps > 0)
                                    ? (qpcFreq.QuadPart / config.video.fps)
                                    : (qpcFreq.QuadPart / 60);
  int64_t nextPushTime = 0;

  DWORD lastLog = GetTickCount();
  uint32_t pushedCount = 0;
  uint32_t droppedCount = 0;
  uint32_t pacingDroppedCount = 0;
  uint32_t emptySpinCount = 0;

  while (!g_InjectCaptureShutdown && g_Recording) {
    // 1. Check for new frames
    uint32_t writeIndex =
        g_pSharedMem->frameRing.writeIndex.load(std::memory_order_acquire);

    // Overflow Protection
    if (writeIndex > localReadIndex + FRAME_RING_SIZE) {
      uint32_t dropped = writeIndex - localReadIndex - 1;
      // Only log huge jumps to avoid spam
      if (dropped > 10) {
        LogInfo("[Inject Thread] Lag detected! Dropping %u frames to catch up",
                dropped);
      }
      localReadIndex = writeIndex - 1;
      droppedCount += dropped;
      // Reset pacing on overflow/lag
      nextPushTime = 0;
    }

    if (writeIndex != localReadIndex) {
      emptySpinCount = 0;

      uint32_t index = localReadIndex % FRAME_RING_SIZE;
      FrameSlot &slot = g_pSharedMem->frameRing.slots[index];

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
          int64_t jitterWindow =
              (targetIntervalTicks * 8) / 10; // 80% tolerance

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
            uint32_t pendingFrames = (writeIndex > localReadIndex)
                                         ? (writeIndex - localReadIndex)
                                         : 0;

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
          QueuedFrame qf;
          qf.isInjectMode = true;
          qf.ringIndex = localReadIndex;
          qf.timestamp = slot.timestamp;

          // CRITICAL FIX: Reset valid flag after reading to prevent stale data
          // on slot reuse
          slot.valid.store(0, std::memory_order_release);

          int32_t texIdx = slot.textureIndex;

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
            } else {
              qf.sharedHandle = (HANDLE)g_pSharedMem->GetSharedHandle(0);
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

          static bool sharedTexturesCreated = false;
          if (!sharedTexturesCreated && g_pSharedMem->GetWidth() > 0 &&
              g_pSharedMem->GetHeight() > 0) {
            if (!g_pSharedMem->encoderTextures.ready.load(
                    std::memory_order_acquire)) {
              if (MediaEngine_CreateSharedCaptureTextures(
                      g_pSharedMem->GetWidth(), g_pSharedMem->GetHeight(),
                      g_pSharedMem->GetFormat(), g_pSharedMem)) {
                sharedTexturesCreated = true;
              }
            } else {
              sharedTexturesCreated = true;
            }
          }

          if (g_FrameQueue.Push(qf, g_IsEncoderBottlenecked)) {
            pushedCount++;
          } else {
            droppedCount++;
          }
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
        LogInfo("[Inject Thread] Pushed: %u, Dropped(Full): %u, "
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

void WgcCaptureThreadFunc(const AppConfig &config) {
  LogInfo("[WGC CaptureThread] Started (OBS-style direct callback mode)");
  g_WgcCaptureRunning = true;

  DWORD lastDiagTime = GetTickCount();
  uint32_t lastCallbackCount = 0;

  while (!g_WgcCaptureShutdown) {
    Sleep(1000); // Check once per second

    if (!g_Recording || !g_WgcCap) {
      lastCallbackCount = 0;
      lastDiagTime = GetTickCount();
      continue;
    }

    DWORD now = GetTickCount();
    if (now - lastDiagTime >= 1000) {
      uint32_t currentCount = g_WgcCap->GetCallbackFrameCount();
      uint32_t framesThisSecond = currentCount - lastCallbackCount;

      LogInfo("[WGC Diag] Callback FPS: %u, Dropped: %u", framesThisSecond,
              g_WgcDroppedFrames.load(std::memory_order_relaxed));

      lastCallbackCount = currentCount;
      lastDiagTime = now;
    }
  }

  g_WgcCaptureRunning = false;
  LogInfo("[WGC CaptureThread] Stopped");
}

void EncoderThreadFunc(const AppConfig &config) {
  LogInfo("[EncoderThread] Started");

  g_FrameQueue.StartRecording();

  LARGE_INTEGER qpcFreq;
  QueryPerformanceFrequency(&qpcFreq);
  int64_t targetIntervalTicks = qpcFreq.QuadPart / config.video.fps;
  LARGE_INTEGER nextSampleTime;
  QueryPerformanceCounter(&nextSampleTime);

  HANDLE hTimer = CreateWaitableTimerExW(
      NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
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
      LogInfo("[EncoderThread] Alive. QueueSize=%u Bottleneck=%d",
              (unsigned int)g_FrameQueue.Size(), (int)g_IsEncoderBottlenecked);
      lastThreadLog = GetTickCount();
    }

    if (g_pSharedMem) {
      uint32_t queueDepth = (uint32_t)g_FrameQueue.Size();
      double fenceWaitMs =
          (double)MediaEngine_GetLastFrameFenceWaitUs() / 1000.0;
      bool shouldThrottle = queueDepth > 3 || fenceWaitMs > 16.0;

      g_pSharedMem->encoderQueueDepth.store(queueDepth,
                                            std::memory_order_relaxed);
      g_pSharedMem->throttleCapture.store(shouldThrottle,
                                          std::memory_order_release);
      g_pSharedMem->runtimeState.hostDroppedFrames.store(
          static_cast<uint32_t>(g_FrameQueue.GetDroppedCount()));

      static DWORD lastMemLog = 0;
      if (GetTickCount() - lastMemLog > 1000) {
        PROCESS_MEMORY_COUNTERS_EX pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                                 (PROCESS_MEMORY_COUNTERS *)&pmc,
                                 sizeof(pmc))) {
          SIZE_T privateBytes = pmc.PrivateUsage;
          SIZE_T workingSet = pmc.WorkingSetSize;
          LogInfo("[System] Memory: Private=%zu MB, WorkingSet=%zu MB",
                  privateBytes / (1024 * 1024), workingSet / (1024 * 1024));
        }
        lastMemLog = GetTickCount();
      }
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
    bool popped = g_FrameQueue.Pop(frame, 0);

    QueuedFrame *frameToProcess = nullptr;
    bool isDuplicate = false;

    if (popped) {
      frameToProcess = &frame;
      if (frame.isInjectMode) {
        g_LastFrame = frame;
        g_HasLastFrame = true;
      } else {
        if (g_HasLastFrame && !g_LastFrame.isInjectMode &&
            g_LastFrame.texture) {
          g_LastFrame.texture->Release();
        }
        g_LastFrame = frame;
        if (g_LastFrame.texture) {
          g_LastFrame.texture->AddRef();
        }
        g_HasLastFrame = true;
      }
    } else if (g_HasLastFrame && g_EncoderRunning) {
      if (!g_LastFrame.isInjectMode) {
        frameToProcess = &g_LastFrame;
        isDuplicate = true;
      }
    }

    if (!g_EncoderRunning && !popped) {
      break;
    }

    if (frameToProcess) {
      LARGE_INTEGER startEnc, endEnc;
      QueryPerformanceCounter(&startEnc);

      if (frameToProcess->isInjectMode) {
        MediaEngine_ProcessFrame(
            (uint64_t)frameToProcess->sharedHandle,
            (uint64_t)frameToProcess->fenceHandle, frameToProcess->fenceValue,
            frameToProcess->timestamp, frameToProcess->luidLow,
            frameToProcess->luidHigh, frameToProcess->sourcePid,
            frameToProcess->width, frameToProcess->height,
            frameToProcess->format, frameToProcess->isHDR,
            frameToProcess->isShmem, frameToProcess->shmemSlot);
      } else {
        MediaEngine_ProcessFrameD3D11(
            frameToProcess->texture, frameToProcess->timestamp,
            frameToProcess->width, frameToProcess->height);
      }

      QueryPerformanceCounter(&endEnc);
      double currentEncodeMs = (double)(endEnc.QuadPart - startEnc.QuadPart) *
                               1000.0 / qpcFreq.QuadPart;

      double pureEncodeMs =
          (double)MediaEngine_GetLastFrameEncodeTimeUs() / 1000.0;

      if (smoothedEncodeMs == 0.0) {
        smoothedEncodeMs = pureEncodeMs;
      } else {
        smoothedEncodeMs = smoothedEncodeMs * 0.95 + pureEncodeMs * 0.05;
      }

      g_IsEncoderBottlenecked = (smoothedEncodeMs > frameIntervalMs * 0.95);

      static DWORD lastWarningTime = 0;
      if (smoothedEncodeMs > frameIntervalMs * 0.85) {
        DWORD now = GetTickCount();
        if (now - lastWarningTime > 5000) {
          LogInfo("[WARN] Encoder approaching capacity: %.2fms avg vs %.2fms "
                  "budget",
                  smoothedEncodeMs, frameIntervalMs);
          lastWarningTime = now;
        }
      }

      if (popped && frameToProcess->isInjectMode && g_pSharedMem) {
        g_pSharedMem->frameRing.readIndex.store(frameToProcess->ringIndex + 1,
                                                std::memory_order_release);
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

void StartRecording(const AppConfig &config) {
  if (g_Recording)
    return;

  LogInfo("[Media] Starting recording...");

  g_FrameQueue.Clear();
  if (g_HasLastFrame && !g_LastFrame.isInjectMode && g_LastFrame.texture) {
    g_LastFrame.texture->Release();
    g_LastFrame.texture = nullptr;
  }
  g_HasLastFrame = false;

  if (g_pSharedMem) {
    g_pSharedMem->runtimeState.duplicateFrames = 0;
    g_pSharedMem->runtimeState.lateFrames = 0;
    g_pSharedMem->runtimeState.encoderOverloadFlags.store(
        0, std::memory_order_relaxed);
    g_pSharedMem->runtimeState.muxQueueBytes.store(0,
                                                   std::memory_order_relaxed);
  }

  if (!MediaEngine_StartRecording()) {
    LogError("[Media] Failed to start MediaEngine recording");
    return;
  }

  g_Recording = true;
  g_EncoderRunning = true;

  if (g_pSharedMem) {
    g_pSharedMem->runtimeState.isRecording.store(true,
                                                 std::memory_order_release);
    g_pSharedMem->runtimeState.recordingStartTime.store(
        GetTickCount64(), std::memory_order_release);
  }

  g_EncoderThread = std::thread(EncoderThreadFunc, std::ref(config));
  SetThreadPriority(reinterpret_cast<HANDLE>(
                        g_EncoderThread.native_handle()),
                    THREAD_PRIORITY_NORMAL);

  if (g_UseScreenGrab && g_WgcCap) {
    g_WgcCap->SetCaptureCursor(config.video.captureCursor);

    g_WgcCap->SetDirectFrameCallback([](ID3D11Texture2D *texture,
                                        uint32_t width, uint32_t height,
                                        int64_t timestamp) {
      QueuedFrame qf;
      qf.isInjectMode = false;
      qf.texture = texture;
      qf.width = width;
      qf.height = height;
      qf.timestamp = timestamp;

      if (!g_FrameQueue.Push(qf, g_IsEncoderBottlenecked)) {
        g_WgcDroppedFrames.fetch_add(1, std::memory_order_relaxed);
        texture->Release();
      }
    });

    g_WgcCap->StartCapture();
    g_WgcCap->ResetStats();
    g_WgcDroppedFrames.store(0, std::memory_order_relaxed);

    g_WgcCaptureShutdown = false;
    g_WgcCaptureThread = std::thread(WgcCaptureThreadFunc, std::ref(config));
    SetThreadPriority(reinterpret_cast<HANDLE>(
                          g_WgcCaptureThread.native_handle()),
                      THREAD_PRIORITY_NORMAL);
    LogInfo("[Media] WGC capture with direct callback started");
  } else if (g_UseScreenGrab && g_DxgiCap) {
    if (g_DxgiCap->StartCapture()) {
      LogInfo("[Media] DXGI Desktop Duplication capture started");

      g_WgcCaptureShutdown = false;
      g_WgcCaptureThread = std::thread(
          [](const AppConfig &cfg) {
            LogInfo("[DXGI Thread] Started");
            g_WgcCaptureRunning = true;

            auto lastLog = GetTickCount64();
            int framesLogged = 0;

            while (!g_WgcCaptureShutdown && g_DxgiCap) {
              WGCCapturedFrame frame;
              bool gotFrame =
                  g_DxgiCap->GetNextFrame(frame, g_IsEncoderBottlenecked);

              if (gotFrame) {
                QueuedFrame qf;
                qf.isInjectMode = false;
                qf.texture = frame.texture;
                qf.width = frame.width;
                qf.height = frame.height;
                qf.timestamp = frame.timestamp;

                if (!g_FrameQueue.Push(qf, g_IsEncoderBottlenecked)) {
                  frame.texture->Release();
                } else {
                  framesLogged++;
                }
              } else {
                std::this_thread::yield();
              }

              auto now = GetTickCount64();
              if (now - lastLog > 1000) {
                LogInfo("[DXGI Diag] FPS: %d, Queue: %d", framesLogged,
                        g_FrameQueue.Size());
                framesLogged = 0;
                lastLog = now;
              }
            }
            g_WgcCaptureRunning = false;
            LogInfo("[DXGI Thread] Stopped");
          },
          std::ref(config));

      SetThreadPriority(reinterpret_cast<HANDLE>(
                            g_WgcCaptureThread.native_handle()),
                        THREAD_PRIORITY_ABOVE_NORMAL);
    } else {
      LogError("[Media] Failed to start DXGI capture");
      StopRecording();
      return;
    }
  } else if (!g_UseScreenGrab) {
    LogInfo("[Media] Starting InjectCaptureThread for Shared Memory Capture");
    g_InjectCaptureShutdown = false;
    g_InjectCaptureThread =
        std::thread(InjectCaptureThreadFunc, std::ref(config));
    SetThreadPriority(reinterpret_cast<HANDLE>(
                          g_InjectCaptureThread.native_handle()),
                      THREAD_PRIORITY_ABOVE_NORMAL);
  }

  LogInfo("[Media] Recording started");
}

void StopRecording() {
  if (!g_Recording)
    return;

  LogInfo("[Media] Stopping recording...");

  g_Recording = false;

  if (g_UseScreenGrab && g_WgcCap) {
    if (g_WgcCaptureRunning) {
      g_WgcCaptureShutdown = true;
      if (g_WgcCaptureThread.joinable()) {
        g_WgcCaptureThread.join();
      }
      g_DxgiCap->StopCapture();
    }
  } else {
    g_InjectCaptureShutdown = true;
    if (g_InjectCaptureThread.joinable()) {
      g_InjectCaptureThread.join();
    }
  }

  g_EncoderRunning = false;

  if (g_EncoderThread.joinable()) {
    g_EncoderThread.join();
  }

  g_FrameQueue.Clear();
  MediaEngine_StopRecording();

  if (g_pSharedMem) {
    g_pSharedMem->runtimeState.isRecording.store(false,
                                                 std::memory_order_release);
    g_pSharedMem->runtimeState.recordingStartTime.store(
        0, std::memory_order_release);
    g_pSharedMem->runtimeState.encoderOverloadFlags.store(
        0, std::memory_order_relaxed);
    g_pSharedMem->runtimeState.muxQueueBytes.store(0,
                                                   std::memory_order_relaxed);
  }

  LogInfo("[Media] Recording stopped");
}

int MediaProcessMain(const AppConfig &config) {
  timeBeginPeriod(1);

  // Get exe directory for DLL loading
  char exePath[MAX_PATH];
  GetModuleFileNameA(NULL, exePath, MAX_PATH);
  std::string exeDir = std::string(exePath);
  exeDir = exeDir.substr(0, exeDir.find_last_of("\\/"));

  // Load mediaengine.dll dynamically (with FFmpeg DLLs in ffmpeg/ subfolder)
  if (!MediaEngine_Load(exeDir.c_str())) {
    LogError("[Media] Failed to load mediaengine.dll");
    return 1;
  }

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
  LogInfo("[Media] offsetof(frameRing) = %zu",
          offsetof(SharedMemoryLayout, frameRing));
  LogInfo("[Media] offsetof(runtimeState) = %zu",
          offsetof(SharedMemoryLayout, runtimeState));

  ID3D11Device *d3dDevice = nullptr;
  ID3D11DeviceContext *d3dContext = nullptr;

  bool explicitScreengrab = (config.captureMethod == "screengrab" ||
                             config.captureMethod == "framegrab" ||
                             config.captureMethod == "desktop_dup");
  if (explicitScreengrab) {
    g_UseScreenGrab = true;
    LogInfo("[Media] Using screengrab mode (explicit)");
  }

  LogInfo("[Media] Attempting to connect to shared memory...");

  for (int retry = 0; retry < 10 && !g_pSharedMem; retry++) {
    HANDLE hDiscovery =
        OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (hDiscovery) {
      DiscoveryInfo *pDiscovery = (DiscoveryInfo *)MapViewOfFile(
          hDiscovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));

      if (pDiscovery && pDiscovery->magic == DISCOVERY_MAGIC &&
          pDiscovery->injectPid.load() != 0) {
        wchar_t sharedMemName[64];
        GenerateSharedMemName(sharedMemName, 64, pDiscovery->injectPid.load());

        g_hMapFile =
            OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sharedMemName);
        if (g_hMapFile) {
          g_pSharedMem = (SharedMemoryLayout *)MapViewOfFile(
              g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0,
              sizeof(SharedMemoryLayout));

          if (g_pSharedMem && g_pSharedMem->GetHostPID() != 0) {
            LogInfo("[Media] Connected via discovery (inject PID: %u)",
                    pDiscovery->injectPid.load());
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
      Sleep(200);
    }
  }

  if (g_pSharedMem) {
    if (g_pSharedMem->GetShmemMappingCreated()) {
      wchar_t shmemName[64];
      GenerateShmemName(shmemName, 64, g_pSharedMem->GetHostPID());
      g_hMapShmem = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmemName);
      if (g_hMapShmem) {
        g_pShmem = (ShmemBuffer *)MapViewOfFile(
            g_hMapShmem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShmemBuffer));
        if (g_pShmem) {
          LogInfo("[Media] Connected to separate Shmem mapping '%ls'",
                  shmemName);
        }
      }
    }

    MediaEngine_SetSharedMem(g_pSharedMem, g_pShmem);

    if (explicitScreengrab) {
      g_UseScreenGrab = true;
      LogInfo(
          "[Media] Connected to shared memory - using screengrab for capture");
    } else {
      g_UseScreenGrab = false;
      LogInfo("[Media] Connected to shared memory - using inject mode");
    }
  } else if (config.captureMethod == "inject") {
    LogError("[Media] Failed to connect to shared memory in inject mode!");
    MediaEngine_Shutdown();
    timeEndPeriod(1);
    return 1;
  } else {
    g_UseScreenGrab = true;
    LogInfo("[Media] Shared memory not available - using screengrab mode");
  }

  if (g_UseScreenGrab || config.captureMethod == "auto") {
    d3dDevice = MediaEngine_GetD3D11Device();
    if (!d3dDevice) {
      if (g_UseScreenGrab) {
        LogError("[Media] Failed to get D3D11 device");
        MediaEngine_Shutdown();
        timeEndPeriod(1);
        return 1;
      }
    } else {
      d3dDevice->GetImmediateContext(&d3dContext);

      if (config.captureMethod == "desktop_dup") {
        LogInfo("[Media] Initializing Desktop Duplication...");
        g_DxgiCap = std::make_unique<DxgiCapture>();
        if (g_DxgiCap->Init(d3dDevice, 0)) {
          LogInfo("[Media] Desktop Duplication initialized");
        } else {
          LogError("[Media] Desktop Duplication init failed");
          MediaEngine_Shutdown();
          timeEndPeriod(1);
          return 1;
        }
      } else if (WGCCapture::IsSupported()) {
        g_WgcCap = std::make_unique<WGCCapture>();
        if (g_WgcCap->Init(d3dDevice)) {
          LogInfo("[Media] WGC capture initialized%s",
                  g_UseScreenGrab ? "" : " (standby for auto fallback)");
        } else {
          if (g_UseScreenGrab) {
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

  LogInfo("[Media] Process started (PID: %d) Mode: %s", GetCurrentProcessId(),
          g_UseScreenGrab ? "screengrab" : "inject");

  LARGE_INTEGER qpcFreq;
  QueryPerformanceFrequency(&qpcFreq);
  int64_t recordingStartTime = 0;

  while (g_Running) {
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
      case ProcessCommand::ReloadConfig:
        MediaEngine_ReloadConfig(&config);
        ipc.SendResponse(ProcessResponse::Ack);
        break;
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
          g_pSharedMem->runtimeState.ackRecordingStarted.store(
              true, std::memory_order_release);
        }
      }
      if (LoadAcquire(g_pSharedMem->runtimeState.cmdStopRecording)) {
        StoreRelease(g_pSharedMem->runtimeState.cmdStopRecording, false);
        if (g_Recording) {
          StopRecording();
          g_pSharedMem->runtimeState.ackRecordingStopped.store(
              true, std::memory_order_release);
        }
      }

      static DWORD lastWindowScanTime = 0;
      static HWND currentCapturedWindow = NULL;

      DWORD now = GetTickCount();
      if (!config.wgcWindowTitles.empty() &&
          (now - lastWindowScanTime > 1000)) {
        lastWindowScanTime = now;

        struct WgcSearchContext {
          const std::vector<std::string> *targets;
          HWND result;
        };

        WgcSearchContext ctx = {&config.wgcWindowTitles, NULL};

        EnumWindows(
            [](HWND hwnd, LPARAM lParam) -> BOOL {
              WgcSearchContext *context = (WgcSearchContext *)lParam;
              if (!IsWindowVisible(hwnd))
                return TRUE;
              if (GetWindow(hwnd, GW_OWNER) != 0)
                return TRUE;

              char title[256];
              GetWindowTextA(hwnd, title, sizeof(title));
              std::string titleStr = title;
              std::transform(titleStr.begin(), titleStr.end(), titleStr.begin(),
                             ::tolower);

              for (const auto &target : *context->targets) {
                std::string targetLower = target;
                std::transform(targetLower.begin(), targetLower.end(),
                               targetLower.begin(), ::tolower);

                if (!titleStr.empty() &&
                    titleStr.find(targetLower) != std::string::npos) {
                  context->result = hwnd;
                  return FALSE;
                }

                if (targetLower.length() > 4 &&
                    targetLower.substr(targetLower.length() - 4) == ".exe") {
                  DWORD pid = 0;
                  GetWindowThreadProcessId(hwnd, &pid);
                  std::string procName = GetProcessNameFromPID(pid);
                  std::transform(procName.begin(), procName.end(),
                                 procName.begin(), ::tolower);

                  if (procName == targetLower) {
                    context->result = hwnd;
                    return FALSE;
                  }
                }
              }
              return TRUE;
            },
            (LPARAM)&ctx);

        HWND foundWindow = ctx.result;

        if (foundWindow && foundWindow != currentCapturedWindow) {
          LogInfo("[Media] WGC Trigger: Found window (0x%p) matching config. "
                  "Switching capture...",
                  foundWindow);

          g_UseScreenGrab = true;
          g_WgcCap.reset();
          g_WgcCap = std::make_unique<WGCCapture>();

          if (g_WgcCap->InitForWindow(d3dDevice, foundWindow)) {
            g_WgcCap->SetCaptureCursor(config.video.captureCursor);
            LogInfo("[Media] WGC re-initialized for Window 0x%p", foundWindow);
            currentCapturedWindow = foundWindow;
          } else {
            LogError("[Media] Failed to init WGC for found window.");
            g_WgcCap.reset();
            g_WgcCap = std::make_unique<WGCCapture>();
            g_WgcCap->Init(d3dDevice);
            currentCapturedWindow = NULL;
          }
        } else if (!foundWindow && currentCapturedWindow != NULL) {
          if (!IsWindow(currentCapturedWindow)) {
            LogInfo("[Media] Captured window 0x%p no longer valid. Reverting "
                    "to monitor/inject.",
                    currentCapturedWindow);
            currentCapturedWindow = NULL;
            g_WgcCap.reset();
            g_WgcCap = std::make_unique<WGCCapture>();
            g_WgcCap->Init(d3dDevice);
          }
        }
      }

      static uint32_t lastSourcePid = 0;
      uint32_t currentSourcePid = g_pSharedMem->GetSourcePid();

      if (currentSourcePid != 0 && currentSourcePid != lastSourcePid) {
        lastSourcePid = currentSourcePid;
        std::string procName = GetProcessNameFromPID(currentSourcePid);
        LogInfo("[Media] Hook connected: %s (PID: %u)", procName.c_str(),
                currentSourcePid);

        bool forceWGC = false;
        if (!config.overlayWhitelist.empty()) {
          std::string lowerName = procName;
          std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                         ::tolower);

          for (const auto &item : config.overlayWhitelist) {
            std::string lowerItem = item;
            std::transform(lowerItem.begin(), lowerItem.end(),
                           lowerItem.begin(), ::tolower);
            if (lowerName == lowerItem ||
                lowerName.find(lowerItem) != std::string::npos) {
              forceWGC = true;
              LogInfo("[Media] Overlay Whitelist Match! Forcing WGC for %s",
                      procName.c_str());
              break;
            }
          }
        }

        if (forceWGC) {
          g_UseScreenGrab = true;

          HWND hGameWindow = GetMainWindowForProcess(currentSourcePid);
          if (hGameWindow) {
            LogInfo("[Media] Whitelist Optimization: Found main window 0x%p. "
                    "Switching WGC to Window Mode.",
                    hGameWindow);

            g_WgcCap.reset();
            g_WgcCap = std::make_unique<WGCCapture>();
            if (g_WgcCap->InitForWindow(d3dDevice, hGameWindow)) {
              g_WgcCap->SetCaptureCursor(config.video.captureCursor);
              LogInfo("[Media] WGC re-initialized for Window Capture");
            } else {
              LogError("[Media] Failed to init WGC for window - falling back "
                       "to Monitor");
              g_WgcCap.reset();
              g_WgcCap = std::make_unique<WGCCapture>();
              g_WgcCap->Init(d3dDevice);
            }
          } else {
            LogInfo("[Media] Whitelist: No main window found for PID %u. Using "
                    "Monitor Capture.",
                    currentSourcePid);
          }

        } else if (config.captureMethod != "screengrab" &&
                   config.captureMethod != "framegrab") {
          g_UseScreenGrab = false;
          LogInfo("[Media] Using Inject Mode (Default)");
        }
      }
    }

    bool hasPendingInputs = false;

    if (g_UseScreenGrab && g_Recording) {
      if (recordingStartTime == 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        recordingStartTime = now.QuadPart;
      }

      if (g_pSharedMem) {
        uint32_t totalDropped =
            g_WgcDroppedFrames.load(std::memory_order_relaxed) +
            (uint32_t)g_FrameQueue.GetDroppedCount();
        g_pSharedMem->runtimeState.hostDroppedFrames.store(totalDropped);
      }
    } else if (!g_UseScreenGrab && g_Recording && g_pSharedMem) {
      FrameRingBuffer &ring = g_pSharedMem->frameRing;
      uint32_t wIdx = ring.writeIndex.load(std::memory_order_acquire);
      static uint32_t localReadIdx = 0;
      static bool receivedFirstFrame = false;
      static DWORD injectModeStartTime = 0;
      static bool sessionInitialized = false;

      if (!sessionInitialized) {
        localReadIdx = wIdx;
        receivedFirstFrame = false;
        injectModeStartTime = 0;
        sessionInitialized = true;
        LogInfo(
            "[Media] Inject mode session initialized, localReadIdx=%u, wIdx=%u",
            localReadIdx, wIdx);
      }

      static DWORD lastPollLog = 0;
      if (GetTickCount() - lastPollLog > 1000) {
        LogInfo("[Media] Polling: localReadIdx=%u, wIdx=%u", localReadIdx,
                wIdx);
        lastPollLog = GetTickCount();
      }

      if (injectModeStartTime == 0) {
        injectModeStartTime = GetTickCount();
        receivedFirstFrame = false;
      }

      if (!receivedFirstFrame && config.captureMethod == "auto" && g_WgcCap) {
        DWORD elapsed = GetTickCount() - injectModeStartTime;
        if (elapsed > 200) {
          LogInfo("[Media] No frames from inject mode after %dms - falling "
                  "back to WGC",
                  elapsed);

          g_WgcCap->SetCaptureCursor(config.video.captureCursor);

          g_WgcCap->SetDirectFrameCallback([](ID3D11Texture2D *texture,
                                              uint32_t width, uint32_t height,
                                              int64_t timestamp) {
            QueuedFrame qf;
            qf.isInjectMode = false;
            qf.texture = texture;
            qf.width = width;
            qf.height = height;
            qf.timestamp = timestamp;

            if (!g_FrameQueue.Push(qf, g_IsEncoderBottlenecked)) {
              g_WgcDroppedFrames.fetch_add(1, std::memory_order_relaxed);
              texture->Release();
            }
          });

          g_WgcCap->StartCapture();
          g_WgcCap->ResetStats();
          g_WgcDroppedFrames.store(0, std::memory_order_relaxed);

          g_WgcCaptureShutdown = false;
          g_WgcCaptureThread =
              std::thread(WgcCaptureThreadFunc, std::ref(config));
          SetThreadPriority(reinterpret_cast<HANDLE>(
                                g_WgcCaptureThread.native_handle()),
                            THREAD_PRIORITY_NORMAL);

          g_UseScreenGrab = true;
          LogInfo("[Media] Switched to WGC capture mode with direct callback");

          injectModeStartTime = 0;
        }
      }

      static bool sharedTexturesCreated = false;
      if (!sharedTexturesCreated && g_pSharedMem->GetWidth() > 0 &&
          g_pSharedMem->GetHeight() > 0) {
        if (g_pSharedMem->encoderTextures.ready.load(
                std::memory_order_acquire)) {
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

  if (g_UseScreenGrab) {
    if (g_WgcCap)
      g_WgcCap->StopCapture();
    if (d3dContext)
      d3dContext->Release();
    if (d3dDevice)
      d3dDevice->Release();
  }

  if (g_pShmem)
    UnmapViewOfFile(g_pShmem);
  if (g_hMapShmem)
    CloseHandle(g_hMapShmem);

  if (g_pSharedMem)
    UnmapViewOfFile(g_pSharedMem);
  if (g_hMapFile)
    CloseHandle(g_hMapFile);

  MediaEngine_Shutdown();
  MediaEngine_Unload();
  timeEndPeriod(1);

  LogInfo("[Media] Process exiting");
  return 0;
}
