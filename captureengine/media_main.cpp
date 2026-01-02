#include "../common/config.h"
#include "../common/frame_queue.h"
#include "../common/frame_timing.h"
#include "../common/logging.h"
#include "../common/process_ipc.h"
#include "../common/shared_defs.h"
#include "../mediaengine/mediaengine.h"
#include "../mediaengine/mediaengine.h"
#include "wgc_capture.h"
#include "dxgi_capture.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <d3d11.h>
#include <thread>
#include <mutex>
#include <timeapi.h>
#include <psapi.h>
#include <algorithm>
#include <string>

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
// Screengrab mode components
static std::unique_ptr<WGCCapture> g_WgcCap;
static std::unique_ptr<DxgiCapture> g_DxgiCap;
static bool g_UseScreenGrab = false;

// Shared memory for hook communication
static HANDLE g_hMapFile = NULL;
static SharedMemoryLayout *g_pSharedMem = nullptr;

static HANDLE g_hMapShmem = NULL;
static ShmemBuffer *g_pShmem = nullptr;

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
    WindowSearch* search = (WindowSearch*)lParam;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == search->pid) {
        // Look for the main visible window
        // Checks: Visible, not child
        // Note: Removed title check as some games have empty titles during init
        if (IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == 0) {
            // Priority: Logic to prefer "cleaner" windows if multiple exist?
            // For now, take first visible top-level window.
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
    WindowSearch search = { pid, NULL };
    EnumWindows(EnumWindowsCallback, (LPARAM)&search);
    return search.hwnd;
}

static std::string GetProcessNameFromPID(DWORD pid) {
    char buffer[MAX_PATH] = "unknown";
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
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
} // End helper


// ============================================================================
// Async WGC Capture Thread (like DX12's AsyncCaptureThreadProc)
// Runs independently to pull frames from WGC without blocking encoder
// ============================================================================
static std::thread g_WgcCaptureThread;
static std::atomic<bool> g_WgcCaptureRunning{false};
static std::atomic<bool> g_WgcCaptureShutdown{false};
static HANDLE g_WgcCaptureEvent = NULL;
static std::atomic<uint32_t> g_WgcDroppedFrames{0};

void WgcCaptureThreadFunc(const AppConfig &config) {
  // OBS-STYLE: Frames are pushed directly from WinRT callback
  // This thread only monitors and logs diagnostics
  LogInfo("[WGC CaptureThread] Started (OBS-style direct callback mode)");
  g_WgcCaptureRunning = true;
  
  // Diagnostics: track frame delivery rate from callback
  DWORD lastDiagTime = GetTickCount();
  uint32_t lastCallbackCount = 0;
  
  while (!g_WgcCaptureShutdown) {
    Sleep(1000); // Check once per second
    
    if (!g_Recording || !g_WgcCap) {
      lastCallbackCount = 0;
      lastDiagTime = GetTickCount();
      continue;
    }
    
    // Diagnostic logging every second
    DWORD now = GetTickCount();
    if (now - lastDiagTime >= 1000) {
      uint32_t currentCount = g_WgcCap->GetCallbackFrameCount();
      uint32_t framesThisSecond = currentCount - lastCallbackCount;
      
      LogInfo("[WGC Diag] Callback FPS: %u, Dropped: %u",
              framesThisSecond,
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
  
  // Start grace period for frame drop counting (first 2 seconds won't count as drops)
  g_FrameQueue.StartRecording();

  LARGE_INTEGER qpcFreq;
  QueryPerformanceFrequency(&qpcFreq);
  int64_t targetIntervalTicks = qpcFreq.QuadPart / config.video.fps;
  LARGE_INTEGER nextSampleTime;
  QueryPerformanceCounter(&nextSampleTime);
  
  // Create high-resolution waitable timer for precision pacing (Windows 10 1803+)
  HANDLE hTimer = CreateWaitableTimerExW(
      NULL, NULL,
      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
      TIMER_ALL_ACCESS);
  if (!hTimer) {
    // Fallback to standard timer if high-resolution not available
    hTimer = CreateWaitableTimer(NULL, TRUE, NULL);
    LogInfo("[EncoderThread] Using standard waitable timer");
  } else {
    LogInfo("[EncoderThread] Using high-resolution waitable timer");
  }

  double smoothedEncodeMs = 0.0;
  double frameIntervalMs = 1000.0 / config.video.fps;

  while (g_EncoderRunning) {
    // Debug log once per second
    static DWORD lastThreadLog = 0;
    if (GetTickCount() - lastThreadLog > 1000) {
      LogInfo("[EncoderThread] Alive. QueueSize=%u Bottleneck=%d",
              (unsigned int)g_FrameQueue.Size(), (int)g_IsEncoderBottlenecked);
      lastThreadLog = GetTickCount();
    }

    // Update throttle state and drop counters in shared memory
    if (g_pSharedMem) {
      uint32_t queueDepth = (uint32_t)g_FrameQueue.Size();
      
      // Throttle if queue is deep OR fence wait is long (GPU overloaded)
      // Get fence wait in ms (conversion from us)
      double fenceWaitMs = (double)MediaEngine_GetLastFrameFenceWaitUs() / 1000.0;
      bool shouldThrottle = queueDepth > 3 || fenceWaitMs > 16.0;
      
      g_pSharedMem->encoderQueueDepth.store(queueDepth,
                                            std::memory_order_relaxed);
      g_pSharedMem->throttleCapture.store(shouldThrottle,
                                          std::memory_order_release);
      
      // Sync dropped frame counter to shared memory for overlay display
      g_pSharedMem->runtimeState.hostDroppedFrames.store(
          static_cast<uint32_t>(g_FrameQueue.GetDroppedCount()));
          
      // DEBUG LEAK: Log Process Memory Usage every 1s
      static DWORD lastMemLog = 0;
      if (GetTickCount() - lastMemLog > 1000) {
          PROCESS_MEMORY_COUNTERS_EX pmc;
          if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
              SIZE_T privateBytes = pmc.PrivateUsage;
              SIZE_T workingSet = pmc.WorkingSetSize;
              LogInfo("[System] Memory: Private=%zu MB, WorkingSet=%zu MB", 
                      privateBytes / (1024*1024), workingSet / (1024*1024));
          }
          lastMemLog = GetTickCount();
      }
    }

    // Wait for next frame interval (CFR Pacing) using high-resolution timer
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int64_t waitTicks = nextSampleTime.QuadPart - now.QuadPart;

    if (waitTicks > 0 && hTimer) {
      // Convert ticks to 100ns units for waitable timer (negative = relative)
      int64_t wait100ns = (waitTicks * 10000000) / qpcFreq.QuadPart;
      LARGE_INTEGER dueTime;
      dueTime.QuadPart = -wait100ns;  // Negative = relative time
      if (SetWaitableTimer(hTimer, &dueTime, 0, NULL, NULL, FALSE)) {
        WaitForSingleObject(hTimer, INFINITE);
      }
    } else if (waitTicks > qpcFreq.QuadPart / 1000) {
      // Fallback: use Sleep for waits > 1ms if no timer
      int64_t waitMs = (waitTicks * 1000) / qpcFreq.QuadPart;
      if (waitMs > 1) {
        Sleep((DWORD)waitMs - 1);
      }
    }

    nextSampleTime.QuadPart += targetIntervalTicks;

    // Handle lag
    QueryPerformanceCounter(&now);
    if (now.QuadPart > nextSampleTime.QuadPart + targetIntervalTicks * 2) {
      nextSampleTime = now;
    }

    // Try to get a frame
    QueuedFrame frame;
    bool popped = g_FrameQueue.Pop(frame, 0);

    QueuedFrame *frameToProcess = nullptr;
    bool isDuplicate = false;

    if (popped) {
      frameToProcess = &frame;
      // Update cached last frame
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
    } else if (g_HasLastFrame) {
      // Re-encode last frame (normal CFR behavior when game FPS < video FPS)
      // Note: Not counted as indicator - low game FPS is expected scenario
      frameToProcess = &g_LastFrame;
      isDuplicate = true;
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
        // D3D11 lock is handled internally by MediaEngine_ProcessFrameD3D11
        MediaEngine_ProcessFrameD3D11(
            frameToProcess->texture, frameToProcess->timestamp,
            frameToProcess->width, frameToProcess->height);
      }

      QueryPerformanceCounter(&endEnc);
      double currentEncodeMs = (double)(endEnc.QuadPart - startEnc.QuadPart) *
                               1000.0 / qpcFreq.QuadPart;
      
      // Use pure encode time (excluding fence waits) for bottleneck detection
      // Now robustly calculated as (End - AfterFence), so safe to trust even if small.
      double pureEncodeMs = (double)MediaEngine_GetLastFrameEncodeTimeUs() / 1000.0;

      if (smoothedEncodeMs == 0.0) {
        smoothedEncodeMs = pureEncodeMs;
      } else {
        // Use slower smoothing (0.05) for stability in health tracking
        smoothedEncodeMs = smoothedEncodeMs * 0.95 + pureEncodeMs * 0.05;
      }

      g_IsEncoderBottlenecked = (smoothedEncodeMs > frameIntervalMs * 0.95);
      
      // Health Monitoring: Log warning if approaching capacity (>85%)
      static DWORD lastWarningTime = 0;
      if (smoothedEncodeMs > frameIntervalMs * 0.85) {
          DWORD now = GetTickCount();
          if (now - lastWarningTime > 5000) { // Throttle warnings to 5s
              LogInfo("[WARN] Encoder approaching capacity: %.2fms avg vs %.2fms budget", 
                      smoothedEncodeMs, frameIntervalMs);
              lastWarningTime = now;
          }
      }
      
      // NOTE: Late frame counter disabled. The async GPU wait makes it impossible
      // to reliably distinguish "waiting for game" from "encoder bottleneck".
      // Use droppedFrames and duplicateFrames as smoothness indicators instead.

      // Update read index for inject mode
      if (popped && frameToProcess->isInjectMode && g_pSharedMem) {
        g_pSharedMem->frameRing.readIndex.store(frameToProcess->ringIndex + 1,
                                                std::memory_order_release);
      }
    }

    // Release texture for non-inject mode
    if (popped && !frame.isInjectMode && frame.texture) {
      frame.texture->Release();
    }
  }

  // Cleanup waitable timer
  if (hTimer) {
    CloseHandle(hTimer);
  }

  LogInfo("[EncoderThread] Stopped");
}

void StartRecording(const AppConfig &config) {
  if (g_Recording)
    return;

  LogInfo("[Media] Starting recording...");

  // Reset queue
  g_FrameQueue.Clear();
  if (g_HasLastFrame && !g_LastFrame.isInjectMode && g_LastFrame.texture) {
    g_LastFrame.texture->Release();
    g_LastFrame.texture = nullptr;
  }
  g_HasLastFrame = false;
  
  // Reset smoothness counters for new recording
  if (g_pSharedMem) {
    g_pSharedMem->runtimeState.duplicateFrames = 0;
    g_pSharedMem->runtimeState.lateFrames = 0;
  }

  if (!MediaEngine_StartRecording()) {
    LogError("[Media] Failed to start MediaEngine recording");
    return;
  }

  g_Recording = true;
  g_EncoderRunning = true;
  
  // Update Shared Memory State for Overlay
  if (g_pSharedMem) {
      g_pSharedMem->runtimeState.isRecording.store(true, std::memory_order_release);
      g_pSharedMem->runtimeState.recordingStartTime.store(GetTickCount64(), std::memory_order_release);
  }

  g_EncoderThread = std::thread(EncoderThreadFunc, std::ref(config));
  SetThreadPriority(g_EncoderThread.native_handle(), THREAD_PRIORITY_NORMAL);

  if (g_UseScreenGrab && g_WgcCap) {
    g_WgcCap->SetCaptureCursor(config.video.captureCursor);
    
    // OBS-STYLE: Set callback BEFORE StartCapture so frames are pushed directly
    // from WinRT callback to frame queue (zero latency)
    g_WgcCap->SetDirectFrameCallback(
        [](ID3D11Texture2D* texture, uint32_t width, uint32_t height, int64_t timestamp) {
          // This runs on WinRT thread pool - push to queue immediately
          QueuedFrame qf;
          qf.isInjectMode = false;
          qf.texture = texture;  // Already AddRef'd by callback
          qf.width = width;
          qf.height = height;
          qf.timestamp = timestamp;
          
          if (!g_FrameQueue.Push(qf, g_IsEncoderBottlenecked)) {
            // Queue full - frame dropped
            g_WgcDroppedFrames.fetch_add(1, std::memory_order_relaxed);
            texture->Release();
          }
        });
    
    g_WgcCap->StartCapture();
    g_WgcCap->ResetStats();
    g_WgcDroppedFrames.store(0, std::memory_order_relaxed);
    
    // Start async WGC capture thread (now just for monitoring/diagnostics)
    g_WgcCaptureShutdown = false;
    g_WgcCaptureThread = std::thread(WgcCaptureThreadFunc, std::ref(config));
    SetThreadPriority(g_WgcCaptureThread.native_handle(), THREAD_PRIORITY_NORMAL);
    LogInfo("[Media] WGC capture with direct callback started");
    LogInfo("[Media] WGC capture with direct callback started");
  } else if (g_UseScreenGrab && g_DxgiCap) {
      if (g_DxgiCap->StartCapture()) {
          LogInfo("[Media] DXGI Desktop Duplication capture started");
          
          // Start thread to poll frames from DDAPI
          g_WgcCaptureShutdown = false;
          // Reuse WgcCaptureThreadFunc logic but for polling DXGI?
          // Actually, we can reuse the same thread or spawn a new one. 
          // Let's spawn a specific thread for DXGI since WGC thread expects callbacks.
          
          g_WgcCaptureThread = std::thread([](const AppConfig& cfg) {
               LogInfo("[DXGI Thread] Started");
               g_WgcCaptureRunning = true;
               
               auto lastLog = GetTickCount64();
               int framesLogged = 0;
               
               while (!g_WgcCaptureShutdown && g_DxgiCap) {
                   WGCCapturedFrame frame;
                   bool gotFrame = g_DxgiCap->GetNextFrame(frame, g_IsEncoderBottlenecked);
                   
                   if (gotFrame) {
                       // Push to queue
                       QueuedFrame qf;
                       qf.isInjectMode = false;
                       qf.texture = frame.texture; // Already AddRef'd
                       qf.width = frame.width;
                       qf.height = frame.height;
                       qf.timestamp = frame.timestamp;
                       
                       if (!g_FrameQueue.Push(qf, g_IsEncoderBottlenecked)) {
                           frame.texture->Release();
                           // dropped
                       } else {
                           framesLogged++;
                       }
                   } else {
                       // No frame - yield
                       std::this_thread::yield();
                   }
                   
                   auto now = GetTickCount64();
                   if (now - lastLog > 1000) {
                        LogInfo("[DXGI Diag] FPS: %d, Queue: %d", framesLogged, g_FrameQueue.Size());
                        framesLogged = 0;
                        lastLog = now;
                   }
               }
               g_WgcCaptureRunning = false;
               LogInfo("[DXGI Thread] Stopped");
          }, std::ref(config));
          
          SetThreadPriority(g_WgcCaptureThread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
      } else {
          LogError("[Media] Failed to start DXGI capture");
          StopRecording();
          return;
      }
  }

  LogInfo("[Media] Recording started");
}

void StopRecording() {
  if (!g_Recording)
    return;

  LogInfo("[Media] Stopping recording...");

  // Signal encoder thread to stop, but let it drain remaining frames first
  // The encoder thread will process all remaining queue items before exiting
  g_EncoderRunning = false;
  if (g_EncoderThread.joinable()) {
    g_EncoderThread.join();
  }
  
  // Now drain any remaining frames that were added after thread stop signal
  // This ensures no frames are lost at the end of recording
  QueuedFrame frame;
  int drainedCount = 0;
  while (g_FrameQueue.Pop(frame, 0)) {
    if (frame.isInjectMode) {
      MediaEngine_ProcessFrame(
          (uint64_t)frame.sharedHandle, (uint64_t)frame.fenceHandle,
          frame.fenceValue, frame.timestamp, frame.luidLow, frame.luidHigh,
          frame.sourcePid, frame.width, frame.height, frame.format,
          frame.isHDR);
    } else {
      MediaEngine_ProcessFrameD3D11(frame.texture, frame.timestamp,
                                    frame.width, frame.height);
      if (frame.texture) {
        frame.texture->Release();
      }
    }
    drainedCount++;
  }
  if (drainedCount > 0) {
    LogInfo("[Media] Drained %d remaining frames before stopping", drainedCount);
  }
  
  g_FrameQueue.Clear();

  MediaEngine_StopRecording();

  if (g_UseScreenGrab && g_WgcCap) {
    // Stop WGC capture thread first
    if (g_WgcCaptureRunning) {
      g_WgcCaptureShutdown = true;
      if (g_WgcCaptureThread.joinable()) {
        g_WgcCaptureThread.join();
      }
      LogInfo("[Media] WGC capture thread stopped (dropped frames: %u)", 
              g_WgcDroppedFrames.load(std::memory_order_relaxed));
    }
    g_WgcCap->StopCapture();
  } else if (g_UseScreenGrab && g_DxgiCap) {
      g_WgcCaptureShutdown = true;
      if (g_WgcCaptureThread.joinable()) {
          g_WgcCaptureThread.join();
      }
      g_DxgiCap->StopCapture();
  }

  if (g_EncoderThread.joinable()) {
    LogInfo("[Media] Waiting for encoder finalization...");
    g_EncoderThread.join();
  }

  g_Recording = false;
  
  // Update Shared Memory State for Overlay
  if (g_pSharedMem) {
      g_pSharedMem->runtimeState.isRecording.store(false, std::memory_order_release);
      g_pSharedMem->runtimeState.recordingStartTime.store(0, std::memory_order_release);
  }

  LogInfo("[Media] Recording stopped");
}

int MediaProcessMain(const AppConfig &config) {
  // Enable 1ms timer resolution
  timeBeginPeriod(1);

  // Apply process priority
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

  // Setup IPC
  ProcessIPCServer ipc(ProcessMode::Media);
  if (!ipc.Init()) {
    LogError("[Media] Failed to initialize IPC");
    timeEndPeriod(1);
    return 1;
  }

  // Initialize MediaEngine
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
  LogInfo("[Media] offsetof(frameRing) = %zu", offsetof(SharedMemoryLayout, frameRing));
  LogInfo("[Media] offsetof(runtimeState) = %zu", offsetof(SharedMemoryLayout, runtimeState));

  // Determine capture mode based on config and shared memory availability
  // Priority:
  // - "screengrab" = always use WGC
  // - "inject" = always use shared memory (fail if not available)
  // - "auto" = prefer inject if shared memory exists, else screengrab
  ID3D11Device *d3dDevice = nullptr;
  ID3D11DeviceContext *d3dContext = nullptr;
  
  bool explicitScreengrab = (config.captureMethod == "screengrab" || config.captureMethod == "framegrab" || config.captureMethod == "desktop_dup");
  if (explicitScreengrab) {
    g_UseScreenGrab = true;
    LogInfo("[Media] Using screengrab mode (explicit)");
  }

  // Always connect to shared memory for command polling
  // This is needed even in screengrab mode to receive recording start/stop commands
  LogInfo("[Media] Attempting to connect to shared memory...");

  // Retry a few times since inject process might still be starting
  // Use discovery shared memory for O(1) lookup instead of scanning
  for (int retry = 0; retry < 10 && !g_pSharedMem; retry++) {
    HANDLE hDiscovery = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (hDiscovery) {
      DiscoveryInfo* pDiscovery = (DiscoveryInfo*)MapViewOfFile(
          hDiscovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
      
      if (pDiscovery && pDiscovery->magic == DISCOVERY_MAGIC && pDiscovery->injectPid != 0) {
        wchar_t sharedMemName[64];
        GenerateSharedMemName(sharedMemName, 64, pDiscovery->injectPid);
        
        g_hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sharedMemName);
        if (g_hMapFile) {
          g_pSharedMem = (SharedMemoryLayout *)MapViewOfFile(
              g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemoryLayout));
          
          if (g_pSharedMem && g_pSharedMem->hostPID != 0) {
            LogInfo("[Media] Connected via discovery (inject PID: %u)", pDiscovery->injectPid);
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
      Sleep(200); // Wait for inject process
    }
  }

  if (g_pSharedMem) {
    // Shared memory connected - determine capture mode based on config
    
    // Connect to separate Shmem mapping if available
    if (g_pSharedMem->shmemMappingCreated) {
        wchar_t shmemName[64];
        GenerateShmemName(shmemName, 64, g_pSharedMem->hostPID);
        g_hMapShmem = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmemName);
        if (g_hMapShmem) {
            g_pShmem = (ShmemBuffer*)MapViewOfFile(g_hMapShmem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShmemBuffer));
            if (g_pShmem) {
                LogInfo("[Media] Connected to separate Shmem mapping '%ls'", shmemName);
            }
        }
    }
    
    // Set pointers in MediaEngine
    MediaEngine_SetSharedMem(g_pSharedMem, g_pShmem);

    // Initial mode setup
    if (explicitScreengrab) {
      g_UseScreenGrab = true;
      LogInfo("[Media] Connected to shared memory - using screengrab for capture");
    } else {
      // Default to inject mode (will switch if dynamic whitelist matches later)
      g_UseScreenGrab = false;
      LogInfo("[Media] Connected to shared memory - using inject mode");
    }
  } else if (config.captureMethod == "inject") {
    LogError("[Media] Failed to connect to shared memory in inject mode!");
    MediaEngine_Shutdown();
    timeEndPeriod(1);
    return 1;
  } else {
    // Auto mode or screengrab mode fallback
    g_UseScreenGrab = true;
    LogInfo("[Media] Shared memory not available - using screengrab mode");
  }

  // Initialize screengrab components if needed
  // Also pre-initialize in auto mode so WGC is ready for immediate fallback
  if (g_UseScreenGrab || config.captureMethod == "auto") {
    d3dDevice = MediaEngine_GetD3D11Device();
    if (!d3dDevice) {
      if (g_UseScreenGrab) {
        LogError("[Media] Failed to get D3D11 device");
        MediaEngine_Shutdown();
        timeEndPeriod(1);
        return 1;
      }
      // In auto mode, continue without WGC - inject mode may still work
    } else {
      d3dDevice->GetImmediateContext(&d3dContext);

      if (config.captureMethod == "desktop_dup") {
           LogInfo("[Media] Initializing Desktop Duplication...");
           g_DxgiCap = std::make_unique<DxgiCapture>();
           if (g_DxgiCap->Init(d3dDevice, 0)) { // Monitor 0, mutex handled internally
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

  // Main loop
  LARGE_INTEGER qpcFreq;
  QueryPerformanceFrequency(&qpcFreq);
  int64_t recordingStartTime = 0;

  while (g_Running) {
    // Check for IPC commands (backup method)
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

    // Poll shared memory flags for recording control (primary method)
    // This is more reliable than pipe IPC when under load
    if (g_pSharedMem) {
      // Check for start recording command (atomic read-then-clear pattern)
      if (g_pSharedMem->runtimeState.cmdStartRecording.load(std::memory_order_acquire)) {
        g_pSharedMem->runtimeState.cmdStartRecording.store(false, std::memory_order_release);
        if (!g_Recording) {
          StartRecording(config);
          g_pSharedMem->runtimeState.ackRecordingStarted.store(true, std::memory_order_release);
        }
      }
    // Check for stop recording command
      if (g_pSharedMem->runtimeState.cmdStopRecording.load(std::memory_order_acquire)) {
        g_pSharedMem->runtimeState.cmdStopRecording.store(false, std::memory_order_release);
        if (g_Recording) {
            StopRecording();
            g_pSharedMem->runtimeState.ackRecordingStopped.store(true, std::memory_order_release);
        }
      }
      
      // =================================================================================
      // WGC Window Detection (Periodic Scan - Injection Independent)
      // Allows manual target definition in config (anti-cheat compatible)
      // =================================================================================
      static DWORD lastWindowScanTime = 0;
      static HWND currentCapturedWindow = NULL;
      
      DWORD now = GetTickCount();
      if (!config.wgcWindowTitles.empty() && (now - lastWindowScanTime > 1000)) {
          lastWindowScanTime = now;
          
          struct WgcSearchContext {
             const std::vector<std::string>* targets;
             HWND result;
          };
          
          WgcSearchContext ctx = { &config.wgcWindowTitles, NULL };
          
          EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
              WgcSearchContext* context = (WgcSearchContext*)lParam;
              if (!IsWindowVisible(hwnd)) return TRUE; // Skip invisible
              if (GetWindow(hwnd, GW_OWNER) != 0) return TRUE; // Skip owned/child windows
              
              char title[256];
              GetWindowTextA(hwnd, title, sizeof(title));
              // Don't skip empty titles yet - might match by executable name
              
              std::string titleStr = title;
              // Case-insensitive check
              std::transform(titleStr.begin(), titleStr.end(), titleStr.begin(), ::tolower);
              
              for (const auto& target : *context->targets) {
                  std::string targetLower = target;
                  std::transform(targetLower.begin(), targetLower.end(), targetLower.begin(), ::tolower);
                  
                  // 1. Try Title Match
                  if (!titleStr.empty() && titleStr.find(targetLower) != std::string::npos) {
                      context->result = hwnd;
                      return FALSE; // Found
                  }
                  
                  // 2. Try Executable Name Match (if target ends in .exe)
                  if (targetLower.length() > 4 && targetLower.substr(targetLower.length() - 4) == ".exe") {
                      DWORD pid = 0;
                      GetWindowThreadProcessId(hwnd, &pid);
                      std::string procName = GetProcessNameFromPID(pid);
                      std::transform(procName.begin(), procName.end(), procName.begin(), ::tolower);
                      
                      if (procName == targetLower) {
                           context->result = hwnd;
                           return FALSE; // Found
                      }
                  }
              }
              return TRUE;
          }, (LPARAM)&ctx);
          
          HWND foundWindow = ctx.result;
          
          if (foundWindow && foundWindow != currentCapturedWindow) {
              LogInfo("[Media] WGC Trigger: Found window (0x%p) matching config. Switching capture...", foundWindow); 
              
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
                  g_WgcCap->Init(d3dDevice); // Fallback to monitor
                  currentCapturedWindow = NULL; // Retry next scan
              }
          } else if (!foundWindow && currentCapturedWindow != NULL) {
               // Window lost? (Closed?)
               if (!IsWindow(currentCapturedWindow)) {
                   LogInfo("[Media] Captured window 0x%p no longer valid. Reverting to monitor/inject.", currentCapturedWindow);
                   currentCapturedWindow = NULL;
                   // Revert to Monitor or standard behavior
                   // If we rely on whitelist injection, we might just wait.
                   // For now, keep WGC active but init fallback
                   g_WgcCap.reset();
                   g_WgcCap = std::make_unique<WGCCapture>();
                   g_WgcCap->Init(d3dDevice);
               }
          }
      }

      // Monitor sourcePid to detect when a game connects, and force WGC if it's on the overlay whitelist
      static uint32_t lastSourcePid = 0;
      uint32_t currentSourcePid = g_pSharedMem->sourcePid;
      
      if (currentSourcePid != 0 && currentSourcePid != lastSourcePid) {
          lastSourcePid = currentSourcePid;
          std::string procName = GetProcessNameFromPID(currentSourcePid);
          LogInfo("[Media] Hook connected: %s (PID: %u)", procName.c_str(), currentSourcePid);
          
          bool forceWGC = false;
          if (!config.overlayWhitelist.empty()) {
              std::string lowerName = procName;
              std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
              
              for (const auto& item : config.overlayWhitelist) {
                  std::string lowerItem = item;
                  std::transform(lowerItem.begin(), lowerItem.end(), lowerItem.begin(), ::tolower);
                  if (lowerName == lowerItem || lowerName.find(lowerItem) != std::string::npos) {
                      forceWGC = true;
                      LogInfo("[Media] Overlay Whitelist Match! Forcing WGC for %s", procName.c_str());
                      break;
                  }
              }
          }
          
          if (forceWGC) {
              g_UseScreenGrab = true;
              
              // PERFORMANCE OPTIMIZATION: Try to target specific game window instead of Monitor
              // This is critical for DX9/Legacy games where Monitor Capture causes high DWM overhead
              HWND hGameWindow = GetMainWindowForProcess(currentSourcePid);
              if (hGameWindow) {
                  LogInfo("[Media] Whitelist Optimization: Found main window 0x%p. Switching WGC to Window Mode.", hGameWindow);
                  
                  // Re-initialize WGC for Window
                  g_WgcCap.reset();
                  g_WgcCap = std::make_unique<WGCCapture>();
                  if (g_WgcCap->InitForWindow(d3dDevice, hGameWindow)) {
                      g_WgcCap->SetCaptureCursor(config.video.captureCursor);
                      LogInfo("[Media] WGC re-initialized for Window Capture");
                  } else {
                      LogError("[Media] Failed to init WGC for window - falling back to Monitor");
                      g_WgcCap.reset(); // Reset failed state
                      g_WgcCap = std::make_unique<WGCCapture>();
                      g_WgcCap->Init(d3dDevice); // Fallback
                  }
              } else {
                 LogInfo("[Media] Whitelist: No main window found for PID %u. Using Monitor Capture.", currentSourcePid);
              }

          } else if (config.captureMethod != "screengrab" && config.captureMethod != "framegrab") {
              // Reset to inject mode if not explicit screengrab and not whitelisted
              // (Only reset if we are in auto/inject mode)
              g_UseScreenGrab = false; 
              LogInfo("[Media] Using Inject Mode (Default)");
          }
      }
    }
    
    bool hasPendingInputs = false;

    // Screengrab mode: WGC capture is handled by async WgcCaptureThreadFunc
    // The thread runs at 120Hz and pushes frames to g_FrameQueue
    // We just need to sync dropped frames to shared memory for overlay display
    if (g_UseScreenGrab && g_Recording) {
      if (recordingStartTime == 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        recordingStartTime = now.QuadPart;
      }
      
      // Update shared memory with WGC dropped frame count
      if (g_pSharedMem) {
        uint32_t totalDropped = g_WgcDroppedFrames.load(std::memory_order_relaxed) +
                               (uint32_t)g_FrameQueue.GetDroppedCount();
        g_pSharedMem->runtimeState.hostDroppedFrames.store(totalDropped);
      }
    }
    // Inject mode: poll frames from shared memory
    else if (!g_UseScreenGrab && g_Recording && g_pSharedMem) {
      FrameRingBuffer &ring = g_pSharedMem->frameRing;
      uint32_t wIdx = ring.writeIndex.load(std::memory_order_acquire);
      static uint32_t localReadIdx = 0;
      static bool receivedFirstFrame = false;
      static DWORD injectModeStartTime = 0;
      static bool sessionInitialized = false;

      // Reset state when starting a new recording session
      if (!sessionInitialized) {
        localReadIdx = wIdx;  // Start from current write position
        receivedFirstFrame = false;
        injectModeStartTime = 0;
        sessionInitialized = true;
        LogInfo("[Media] Inject mode session initialized, localReadIdx=%u, wIdx=%u", localReadIdx, wIdx);
      }
      
      // Debug polling
      static DWORD lastPollLog = 0;
      if (GetTickCount() - lastPollLog > 1000) {
          LogInfo("[Media] Polling: localReadIdx=%u, wIdx=%u", localReadIdx, wIdx);
          lastPollLog = GetTickCount();
      }

      // Track when inject mode recording started
      if (injectModeStartTime == 0) {
        injectModeStartTime = GetTickCount();
        receivedFirstFrame = false;
      }

      // Auto-mode fallback: if no frames received within 200ms, switch to WGC
      // WGC is pre-initialized so this switch is fast with minimal A/V desync
      if (!receivedFirstFrame && config.captureMethod == "auto" && g_WgcCap) {
        DWORD elapsed = GetTickCount() - injectModeStartTime;
        if (elapsed > 200) {
          LogInfo("[Media] No frames from inject mode after %dms - falling back to WGC", elapsed);
          
          // WGC is already initialized, set up frame callback and start capture
          g_WgcCap->SetCaptureCursor(config.video.captureCursor);
          
          // CRITICAL: Set callback BEFORE StartCapture (same as StartRecording does)
          g_WgcCap->SetDirectFrameCallback(
              [](ID3D11Texture2D* texture, uint32_t width, uint32_t height, int64_t timestamp) {
                // Push frame to queue immediately
                QueuedFrame qf;
                qf.isInjectMode = false;
                qf.texture = texture;  // Already AddRef'd by callback
                qf.width = width;
                qf.height = height;
                qf.timestamp = timestamp;
                
                if (!g_FrameQueue.Push(qf, g_IsEncoderBottlenecked)) {
                  // Queue full - frame dropped
                  g_WgcDroppedFrames.fetch_add(1, std::memory_order_relaxed);
                  texture->Release();
                }
              });
          
          g_WgcCap->StartCapture();
          g_WgcCap->ResetStats();
          g_WgcDroppedFrames.store(0, std::memory_order_relaxed);
          
          // Start async WGC diagnostic thread
          g_WgcCaptureShutdown = false;
          g_WgcCaptureThread = std::thread(WgcCaptureThreadFunc, std::ref(config));
          SetThreadPriority(g_WgcCaptureThread.native_handle(), THREAD_PRIORITY_NORMAL);
          
          g_UseScreenGrab = true;
          LogInfo("[Media] Switched to WGC capture mode with direct callback");
          
          // Reset tracking
          injectModeStartTime = 0;
        }
      }

      // Create shared textures for Vulkan games to import (once, when
      // dimensions known)
      static bool sharedTexturesCreated = false;
      if (!sharedTexturesCreated && g_pSharedMem->width > 0 &&
          g_pSharedMem->height > 0) {
        // Check if encoder textures are NOT already ready (hook hasn't imported
        // yet)
        if (!g_pSharedMem->encoderTextures.ready.load(
                std::memory_order_acquire)) {
          LogInfo("[Media] Creating shared capture textures for Vulkan interop "
                  "(%dx%d format=%d)",
                  g_pSharedMem->width, g_pSharedMem->height,
                  g_pSharedMem->format);
          if (MediaEngine_CreateSharedCaptureTextures(
                  g_pSharedMem->width, g_pSharedMem->height,
                  g_pSharedMem->format, g_pSharedMem)) {
            LogInfo("[Media] Shared capture textures created successfully");
            sharedTexturesCreated = true;
          } else {
            LogError("[Media] Failed to create shared capture textures");
          }
        } else {
          // Textures already created (from previous recording or by hook), skip
          sharedTexturesCreated = true;
        }
      }

      // Ring Buffer Overflow Protection
      if (wIdx > localReadIdx + FRAME_RING_SIZE) {
          uint32_t newReadIdx = wIdx - FRAME_RING_SIZE;
          LogInfo("[Media] Ring buffer overflow! Jumped from %u to %u (lost %u frames)",
                  localReadIdx, newReadIdx, newReadIdx - localReadIdx);
          localReadIdx = newReadIdx;
          // Sync read index back to shared memory immediately to let hook know we caught up?
          // Actually hook only cares about not overwriting unread data if it blocks. 
          // But our hook is non-blocking (overwriting), so this is just for our side.
      }

      int processedCount = 0;
      while (localReadIdx < wIdx) {
        if (processedCount >= 16) {
            // Process max one full buffer per tick to stay responsive to other events
            break;
        }
        processedCount++;
        uint32_t slotIdx = localReadIdx % FRAME_RING_SIZE;
        const FrameSlot &slot = ring.slots[slotIdx];

        // Use atomic load for valid flag with bounds checking
        if (slot.valid.load(std::memory_order_acquire) == 0) {
          localReadIdx++;
          continue;
        }

        // Mark that we received frames (disable fallback)
        if (!receivedFirstFrame) {
          receivedFirstFrame = true;
          LogInfo("[Media] First frame received from inject mode");
        }

        QueuedFrame qf;
        qf.isInjectMode = true;
        int texIdx = slot.textureIndex;
        
        // Check for SHMEM frame (100+)
        if (texIdx >= 100) {
            qf.isShmem = true;
            qf.shmemSlot = texIdx - 100;
            // Shmem data source has its own width/height
            qf.width = (g_pShmem && g_pShmem->validWidth > 0) ? g_pShmem->validWidth : g_pSharedMem->width;
            qf.height = (g_pShmem && g_pShmem->validHeight > 0) ? g_pShmem->validHeight : g_pSharedMem->height;
            // No shared handle for shmem frames
            qf.sharedHandle = NULL; 
        } else {
            // Normal Shared Texture Frame
            if (!IsValidTextureIndex(texIdx))
              texIdx = 0;
            qf.sharedHandle = (HANDLE)g_pSharedMem->sharedHandles[texIdx];
            qf.width = g_pSharedMem->width;
            qf.height = g_pSharedMem->height;
        }
        
        qf.fenceHandle = (HANDLE)g_pSharedMem->fenceShareHandle;
        qf.fenceValue = slot.fenceValue;
        qf.luidLow = g_pSharedMem->luidLowPart;
        qf.luidHigh = g_pSharedMem->luidHighPart;
        qf.sourcePid = g_pSharedMem->sourcePid;
        qf.timestamp = slot.timestamp;
        qf.isHDR = g_pSharedMem->isHDR;
        qf.format = g_pSharedMem->format;
        qf.ringIndex = localReadIdx;

        if (g_FrameQueue.Push(qf, g_IsEncoderBottlenecked)) {
          localReadIdx++;
        } else {
          break;
        }

      }
      
      hasPendingInputs = (localReadIdx < wIdx);
    } else {
      recordingStartTime = 0;
    }

    // Adaptive Polling: Sleep less when active to reduce latency
    if (g_Recording && (g_FrameQueue.Size() > 0 || hasPendingInputs)) {
      Sleep(1);
    } else {
      Sleep(5); // Idle or waiting for frames
    }
  }

  // Cleanup
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
  timeEndPeriod(1);

  LogInfo("[Media] Process exiting");
  return 0;
}
