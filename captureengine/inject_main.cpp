#include "../common/config.h"
#include "../common/logging.h"
#include "../common/process_ipc.h"
#include "../common/shared_defs.h"
#include "injection.h"
#include "host_metrics.h"
#include <Windows.h>
#include <atomic>
#include <psapi.h>

static std::atomic<bool> g_Running{true};

// Console control handler for graceful cleanup
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        LogInfo("[Inject] Console interrupt received, shutting down...");
        g_Running = false;
        return TRUE;
    }
    return FALSE;
}

// Helper: Get process name from PID
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
}

// Helper: Update shared memory from config
static void UpdateSharedMemoryFromConfig(SharedMemoryLayout* pSharedMem, const AppConfig& config) {
    if (!pSharedMem) return;
    
    // Graphics
    strncpy(pSharedMem->graphicsConfig.vsyncMode, config.graphics.vsyncMode.c_str(), 31);
    strncpy(pSharedMem->graphicsConfig.anisotropicFiltering, config.graphics.anisotropicFiltering.c_str(), 31);
    strncpy(pSharedMem->graphicsConfig.mipMapping, config.graphics.mipMapping.c_str(), 31);
    strncpy(pSharedMem->graphicsConfig.mipBias, config.graphics.mipBias.c_str(), 31);
    strncpy(pSharedMem->graphicsConfig.msaaSamples, config.graphics.msaaSamples.c_str(), 31);
    pSharedMem->graphicsConfig.prerenderLimit = config.graphics.cpuPrerenderLimit;
    pSharedMem->graphicsConfig.backbufferCount = config.graphics.backbufferCount;
    pSharedMem->graphicsConfig.sgssaa = config.graphics.sgssaa;
    pSharedMem->graphicsConfig.disableAutoMipBias = config.graphics.disableAutoMipBias;
    strncpy(pSharedMem->graphicsConfig.dlssAutoExposure, config.graphics.dlssAutoExposure.c_str(), 31);
    strncpy(pSharedMem->graphicsConfig.dlssExposureNormalization, config.graphics.dlssExposureNormalization.c_str(), 31);
    
    // DLSS Presets
    pSharedMem->graphicsConfig.dlssPresetDLAA = ParseDlssPreset(config.graphics.dlssPresetDLAA);
    pSharedMem->graphicsConfig.dlssPresetQuality = ParseDlssPreset(config.graphics.dlssPresetQuality);
    pSharedMem->graphicsConfig.dlssPresetBalanced = ParseDlssPreset(config.graphics.dlssPresetBalanced);
    pSharedMem->graphicsConfig.dlssPresetPerformance = ParseDlssPreset(config.graphics.dlssPresetPerformance);
    pSharedMem->graphicsConfig.dlssPresetUltraPerformance = ParseDlssPreset(config.graphics.dlssPresetUltraPerformance);
    pSharedMem->graphicsConfig.dlssPresetUltraQuality = ParseDlssPreset(config.graphics.dlssPresetUltraQuality);

    // RR Presets
    pSharedMem->graphicsConfig.dlssRRPresetDLAA = ParseDlssRRPreset(config.graphics.dlssRRPresetDLAA);
    pSharedMem->graphicsConfig.dlssRRPresetQuality = ParseDlssRRPreset(config.graphics.dlssRRPresetQuality);
    pSharedMem->graphicsConfig.dlssRRPresetBalanced = ParseDlssRRPreset(config.graphics.dlssRRPresetBalanced);
    pSharedMem->graphicsConfig.dlssRRPresetPerformance = ParseDlssRRPreset(config.graphics.dlssRRPresetPerformance);
    pSharedMem->graphicsConfig.dlssRRPresetUltraPerformance = ParseDlssRRPreset(config.graphics.dlssRRPresetUltraPerformance);
    pSharedMem->graphicsConfig.dlssRRPresetUltraQuality = ParseDlssRRPreset(config.graphics.dlssRRPresetUltraQuality);

    pSharedMem->graphicsConfig.dlssSRPreset = ParseDlssPreset(config.graphics.dlssSRPreset);
    pSharedMem->graphicsConfig.dlssRRPreset = ParseDlssRRPreset(config.graphics.dlssRRPreset);

    pSharedMem->graphicsConfig.dlssSharpening = ParseDlssSharpening(config.graphics.dlssSharpening);

    pSharedMem->configVersion.fetch_add(1, std::memory_order_release);

    // Overlay
    pSharedMem->overlayConfig = config.overlay;

    // Performance / Priority
    pSharedMem->gpuPriority = config.video.gpuPriority;
    if (config.copyQueuePriority == "low")
        pSharedMem->copyQueuePriority = 0;
    else if (config.copyQueuePriority == "high")
        pSharedMem->copyQueuePriority = 2;
    else
        pSharedMem->copyQueuePriority = 1;

    // Synchronization
    pSharedMem->fenceWaitMode = config.fenceWaitMode;
    pSharedMem->useGameQueue = config.useGameQueue;

    // FPS Limiter
    pSharedMem->fpsLimiter.captureSyncEnabled = config.fpsLimiter.captureSyncEnabled;
    pSharedMem->fpsLimiter.captureSyncMultiplier = config.fpsLimiter.captureSyncMultiplier;
    pSharedMem->fpsLimiter.generalEnabled = config.fpsLimiter.generalEnabled;
    pSharedMem->fpsLimiter.generalFps = config.fpsLimiter.generalFps;
    // Note: captureFps is usually set dynamically during recording start, 
    // but we can update it here if it's based on config.video.fps
    pSharedMem->fpsLimiter.captureFps = config.video.fps;
    pSharedMem->fpsLimiter.useVFR = config.video.useVFR;
    
    LogInfo("[Inject] Updated SharedMem Config: VSync=%s, AF=%s, FPS Limit=%d (%s), CaptureOverlay=%d", 
            pSharedMem->graphicsConfig.vsyncMode, 
            pSharedMem->graphicsConfig.anisotropicFiltering,
            pSharedMem->fpsLimiter.generalFps,
            pSharedMem->fpsLimiter.generalEnabled ? "ON" : "OFF",
            pSharedMem->overlayConfig.captureIncludeOverlay);
}

int InjectProcessMain(const AppConfig &config) {
  // Register console control handler
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

  // Setup IPC server
  ProcessIPCServer ipc(ProcessMode::Inject);
  if (!ipc.Init()) {
    LogError("[Inject] Failed to initialize IPC");
    return 1;
  }

  // Deduce config path (same logic as main.cpp)
  char buffer[MAX_PATH];
  GetModuleFileNameA(NULL, buffer, MAX_PATH);
  std::string exePath = buffer;
  std::string baseDir = exePath.substr(0, exePath.find_last_of("\\/"));
  std::string configPath = baseDir + "\\config.ini";

  // Create shared memory with unique name based on our PID
  wchar_t sharedMemName[64];
  GenerateSharedMemName(sharedMemName, 64, GetCurrentProcessId());
  
  HANDLE hMapFile =
      CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                         sizeof(SharedMemoryLayout), sharedMemName);

  if (hMapFile == NULL) {
    LogError("[Inject] Failed to create shared memory: %d", GetLastError());
    return 1;
  }
  
  LogInfo("[Inject] Created shared memory: %ls", sharedMemName);

  SharedMemoryLayout *pSharedMem = (SharedMemoryLayout *)MapViewOfFile(
      hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemoryLayout));

  if (pSharedMem == NULL) {
    LogError("[Inject] Failed to map shared memory");
    CloseHandle(hMapFile);
    return 1;
  }

  // Create discovery shared memory (fixed name for hook to find us quickly)
  HANDLE hDiscoveryFile = CreateFileMappingW(
      INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
      sizeof(DiscoveryInfo), SHARED_MEM_DISCOVERY);
  DiscoveryInfo* pDiscovery = nullptr;
  if (hDiscoveryFile) {
    pDiscovery = (DiscoveryInfo*)MapViewOfFile(
        hDiscoveryFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DiscoveryInfo));
    if (pDiscovery) {
      pDiscovery->injectPid = GetCurrentProcessId();
      pDiscovery->magic = DISCOVERY_MAGIC;
      LogInfo("[Inject] Created discovery memory");
    }
  }

  // Initialize shared memory
  ZeroMemory(pSharedMem, sizeof(SharedMemoryLayout));
  pSharedMem->hostPID = GetCurrentProcessId();
  pSharedMem->debugLogging = config.debugLogging;
  pSharedMem->logLevel = LogLevel::Info;

  // Copy log path
  std::string logPath = config.logFilePath;
  strncpy(pSharedMem->logFilePath, logPath.c_str(),
          sizeof(pSharedMem->logFilePath) - 1);

  // Copy priority settings
  pSharedMem->gpuPriority = config.video.gpuPriority;
  if (config.copyQueuePriority == "low")
    pSharedMem->copyQueuePriority = 0;
  else if (config.copyQueuePriority == "high")
    pSharedMem->copyQueuePriority = 2;
  else
    pSharedMem->copyQueuePriority = 1;

  pSharedMem->fenceWaitMode = config.fenceWaitMode;
  pSharedMem->useGameQueue = config.useGameQueue;

  // Create separate Shmem mapping for large buffer
  wchar_t shmemName[64];
  GenerateShmemName(shmemName, 64, GetCurrentProcessId());
  
  HANDLE hMapShmem = CreateFileMappingW(
      INVALID_HANDLE_VALUE,
      NULL,
      PAGE_READWRITE,
      0,
      sizeof(ShmemBuffer),
      shmemName);
      
  ShmemBuffer* pShmem = nullptr;
  if (hMapShmem) {
      pShmem = (ShmemBuffer*)MapViewOfFile(hMapShmem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShmemBuffer));
      if (pShmem) {
          ZeroMemory(pShmem, sizeof(ShmemBuffer));
          pSharedMem->shmemMappingCreated = true;
          pSharedMem->shmemMappingSize = sizeof(ShmemBuffer);
          LogInfo("[Inject] Created separate Shmem mapping: %ls", shmemName);
      }
  }

  // Copy FPS limiter settings
  pSharedMem->fpsLimiter.captureSyncEnabled =
      config.fpsLimiter.captureSyncEnabled;
  pSharedMem->fpsLimiter.captureSyncMultiplier =
      config.fpsLimiter.captureSyncMultiplier;
  pSharedMem->fpsLimiter.generalEnabled = config.fpsLimiter.generalEnabled;
  pSharedMem->fpsLimiter.generalFps = config.fpsLimiter.generalFps;
  pSharedMem->fpsLimiter.captureFps = config.video.fps;

  // Copy overlay config
  pSharedMem->overlayConfig = config.overlay;

  // Copy graphics overrides
  UpdateSharedMemoryFromConfig(pSharedMem, config);

  // Create FPS limiter events (named for cross-process access)
  wchar_t releaseEventName[64];
  wchar_t requestEventName[64];
  swprintf(releaseEventName, 64, L"Local\\CE_LR_%08X", GetCurrentProcessId());
  swprintf(requestEventName, 64, L"Local\\CE_LQ_%08X", GetCurrentProcessId());
  wcscpy(pSharedMem->fpsLimiter.releaseEventName, releaseEventName);
  wcscpy(pSharedMem->fpsLimiter.requestEventName, requestEventName);

  HANDLE hLimiterReleaseEvent =
      CreateEventW(NULL, FALSE, FALSE, releaseEventName); // Auto-reset
  HANDLE hLimiterRequestEvent =
      CreateEventW(NULL, FALSE, FALSE, requestEventName); // Auto-reset
  
  if (!hLimiterReleaseEvent || !hLimiterRequestEvent) {
    LogError("[Inject] Failed to create limiter events: %d", GetLastError());
  } else {
    LogInfo("[Inject] Created limiter events");
  }

  LogInfo("[Inject] Shared memory created and initialized");

  // --- CBT Hook Global Injection (SpecialK-style) ---
  // This installs a system-wide hook that causes Windows to load our DLL
  // into EVERY process that creates a window. Much earlier than WMI.
  HHOOK hCBTHookX64 = NULL;
  HHOOK hCBTHookX86 = NULL;
  
  // Get paths to our hook DLLs
  char exePathBuf[MAX_PATH];
  GetModuleFileNameA(NULL, exePathBuf, MAX_PATH);
  std::string exeDir = std::string(exePathBuf).substr(0, std::string(exePathBuf).find_last_of("\\/"));
  std::string hookDllX64 = exeDir + "\\capture_hook_x64.dll";
  std::string hookDllX86 = exeDir + "\\capture_hook_x86.dll";
  
  // Load our hook DLLs and get the CBTHookProc export
  HMODULE hDllX64 = LoadLibraryA(hookDllX64.c_str());
  if (hDllX64) {
      HOOKPROC hookProc = (HOOKPROC)GetProcAddress(hDllX64, "CBTHookProc");
      if (hookProc) {
          hCBTHookX64 = SetWindowsHookExA(WH_CBT, hookProc, hDllX64, 0);
          if (hCBTHookX64) {
              LogInfo("[Inject] CBT Global Hook installed (x64)");
          } else {
              LogError("[Inject] Failed to install CBT hook x64: %d", GetLastError());
          }
      } else {
          LogError("[Inject] CBTHookProc not found in x64 DLL");
      }
  } else {
      LogError("[Inject] Failed to load hook DLL x64: %d", GetLastError());
  }
  
  // Also install 32-bit hook for 32-bit games (requires 32-bit helper process on 64-bit Windows)
  // For now, we skip x86 as it requires a separate 32-bit process to call SetWindowsHookEx
  // TODO: Implement 32-bit global hook via helper process
  
  // Initialize injector (WMI fallback for processes that don't create windows)
  InjectionManager injector(config);
  
  // Register callback to reload config on injection
  injector.SetOnInjectCallback([&](const std::string& processName) {
      LogInfo("[Inject] Reloading config for target: %s", processName.c_str());
      
      // Load fresh config with process-specific overrides
      AppConfig targetConfig;
      LoadConfig(configPath, targetConfig, processName);
      
      // Update Shared Memory with new values
      UpdateSharedMemoryFromConfig(pSharedMem, targetConfig);
  });
  // WMI Initialization handled in constructor or explicitly here? 
  // InjectionManager constructor calls InitializeWMI() now (added in previous step via constructor mod? No, wait, checks:
  // In injection.cpp modification, I added InitializeWMI call to Constructor. Let me double check that.
  // Looking at previous diff for injection.cpp:
  // @@ -23,9 +23,16 @@
  // ...
  //    LogError("Capture Hook X86 DLL not found: %s", hookDllPathX86.c_str());
  //    
  //  InitializeWMI();
  // }
  // Yes, I added it to the constructor. So explicit call here is not needed, but good to know.
  
  LogInfo("[Inject] Injection manager initialized");

  LogInfo("[Inject] Process started (PID: %d)", GetCurrentProcessId());

  // Track log polling
  uint32_t lastReadLogIndex = 0;

  // Main loop
  while (g_Running) {
    // Check for IPC commands
    ProcessCommand cmd;
    if (ipc.PollCommand(cmd)) {
      switch (cmd) {
      case ProcessCommand::Shutdown:
        LogInfo("[Inject] Shutdown command received");
        g_Running = false;
        ipc.SendResponse(ProcessResponse::Ack);
        break;
      case ProcessCommand::StartRecording:
        // Update shared memory recording state
        pSharedMem->runtimeState.isRecording.store(true);
        pSharedMem->runtimeState.recordingStartTime.store(GetTickCount64());
        // Set command flag for media process to poll (use atomic store)
        pSharedMem->runtimeState.cmdStartRecording.store(true, std::memory_order_release);
        ipc.SendResponse(ProcessResponse::Ack);
        break;
      case ProcessCommand::StopRecording:
        pSharedMem->runtimeState.isRecording.store(false);
        pSharedMem->runtimeState.recordingStartTime.store(0);
        // Set command flag for media process to poll (use atomic store)
        pSharedMem->runtimeState.cmdStopRecording.store(true, std::memory_order_release);
        ipc.SendResponse(ProcessResponse::Ack);
        break;
      case ProcessCommand::Ping:
        ipc.SendResponse(ProcessResponse::Pong);
        break;
      case ProcessCommand::ReloadConfig:
        // Reload and update shared memory
        // (In a full implementation, we'd reload the config here)
        ipc.SendResponse(ProcessResponse::Ack);
        break;
      default:
        ipc.SendResponse(ProcessResponse::Ack);
        break;
      }
    }

    // Monitor sourcePid for config reloads (CBT hook support)
    static uint32_t lastSourcePid = 0;
    uint32_t currentSourcePid = pSharedMem->sourcePid;
    if (currentSourcePid != 0 && currentSourcePid != lastSourcePid) {
        lastSourcePid = currentSourcePid;
        std::string procName = GetProcessNameFromPID(currentSourcePid);
        LogInfo("[Inject] Hook detected in process: %s (PID: %d). Applying overrides...", 
                procName.c_str(), currentSourcePid);
        
        // Reload config for this process
        AppConfig targetConfig;
        LoadConfig(configPath, targetConfig, procName);
        
        // Update Shared Memory
        UpdateSharedMemoryFromConfig(pSharedMem, targetConfig);
    }

    // Update injector (scan for games, inject)
    // Skip injection when capture_method=screengrab - user explicitly wants WGC only
    // Update injector (process pending WMI injections)
    // Run this every tick (10ms) for responsiveness
    // Skip injection when capture_method=screengrab (unless overlay force-enabled via whitelist)
    bool allowInjection = (config.captureMethod != "screengrab" && config.captureMethod != "framegrab");
    if (allowInjection || !config.overlayWhitelist.empty()) {
      injector.Update();
    }

    Sleep(10); // Main loop sleep (low latency for IPC)
  }

  // Signal hook to exit
  LogInfo("[Inject] Signaling hook to exit...");
  pSharedMem->requestExit = true;
  Sleep(200); // Give hook time to unload

  // Cleanup injector (ejects all hooks)
  // Destructor handles this

  // Cleanup shared memory and handles
  LogInfo("[Inject] Cleaning up...");
  if (pShmem) UnmapViewOfFile(pShmem);
  if (hMapShmem) CloseHandle(hMapShmem);

  UnmapViewOfFile(pSharedMem);
  CloseHandle(hMapFile);
  
  // Cleanup discovery shared memory
  if (pDiscovery) {
    UnmapViewOfFile(pDiscovery);
  }
  if (hDiscoveryFile) {
    CloseHandle(hDiscoveryFile);
  }
  
  // Unhook CBT global hooks
  if (hCBTHookX64) {
      UnhookWindowsHookEx(hCBTHookX64);
      LogInfo("[Inject] CBT Hook x64 removed");
  }
  if (hCBTHookX86) {
      UnhookWindowsHookEx(hCBTHookX86);
      LogInfo("[Inject] CBT Hook x86 removed");
  }
  if (hDllX64) {
      FreeLibrary(hDllX64);
  }
  
  // Close limiter event handles
  if (hLimiterReleaseEvent) CloseHandle(hLimiterReleaseEvent);
  if (hLimiterRequestEvent) CloseHandle(hLimiterRequestEvent);

  LogInfo("[Inject] Process exiting");
  return 0;
}
