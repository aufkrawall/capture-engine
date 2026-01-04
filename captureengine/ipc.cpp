#include "ipc.h"

IPCManager::IPCManager(const AppConfig &config)
    : config(config), hMapFile(NULL), pSharedMem(NULL), hMapShmem(NULL), pShmem(NULL) {}

IPCManager::~IPCManager() {
  if (pShmem)
    UnmapViewOfFile(pShmem);
  if (hMapShmem)
    CloseHandle(hMapShmem);
    
  if (pSharedMem)
    UnmapViewOfFile(pSharedMem);
  if (hMapFile)
    CloseHandle(hMapFile);
}

#include <sddl.h>

bool IPCManager::Init() {
  // Host creates the shared memory with PID-based name
  wchar_t sharedMemName[64];
  GenerateSharedMemName(sharedMemName, 64, GetCurrentProcessId());
  
  // Create Security Descriptor for Low Integrity access (allows sandboxed games/browsers)
  // D: DACL
  // (A;;GA;;;SY) - System: Generic All
  // (A;;GA;;;BA) - Built-in Admins: Generic All
  // (A;;GA;;;IU) - Interactive User: Generic All
  // (A;;GA;;;AC) - All Application Packages (UWP): Generic All
  // S: SACL
  // (ML;;NW;;;LW) - Mandatory Label: No Write Up, Low Integrity Level
  SECURITY_ATTRIBUTES sa = {0};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = FALSE;
  
  const wchar_t* sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)(A;;GA;;;AC)S:(ML;;NW;;;LW)";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) {
      OutputDebugStringA("Failed to create security descriptor from SDDL.");
      // Fallback to default (might fail for low integrity games)
      sa.lpSecurityDescriptor = NULL; 
  }
  
  hMapFile = CreateFileMappingW(
      INVALID_HANDLE_VALUE,       // Use paging file
      sa.lpSecurityDescriptor ? &sa : NULL, // Custom security
      PAGE_READWRITE,             // Read/write access
      0,                          // Maximum object size (high-order DWORD)
      sizeof(SharedMemoryLayout), // Maximum object size (low-order DWORD)
      sharedMemName);             // Name of mapping object

  if (sa.lpSecurityDescriptor) {
      LocalFree(sa.lpSecurityDescriptor);
  }

  if (hMapFile == NULL) {
    OutputDebugStringA("Could not create file mapping object.");
    return false;
  }

  pSharedMem = (SharedMemoryLayout *)MapViewOfFile(
      hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemoryLayout));
  if (pSharedMem == NULL) {
    OutputDebugStringA("Could not map view of file.");
    CloseHandle(hMapFile);
    hMapFile = NULL;
    return false;
  }

  // Initialize main memory
  ZeroMemory(pSharedMem, sizeof(SharedMemoryLayout));
  pSharedMem->hostPID = GetCurrentProcessId();

  // Create separate Shmem mapping for large buffer
  wchar_t shmemName[64];
  GenerateShmemName(shmemName, 64, GetCurrentProcessId());
  
  hMapShmem = CreateFileMappingW(
      INVALID_HANDLE_VALUE,
      NULL,
      PAGE_READWRITE,
      0,
      sizeof(ShmemBuffer),
      shmemName);
      
  if (hMapShmem) {
      pShmem = (ShmemBuffer*)MapViewOfFile(hMapShmem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShmemBuffer));
      if (pShmem) {
          ZeroMemory(pShmem, sizeof(ShmemBuffer));
          pSharedMem->shmemMappingCreated = true;
          pSharedMem->shmemMappingSize = sizeof(ShmemBuffer);
      }
  }

  // Set initial config
  UpdateConfig(config);

  return true;
}

void IPCManager::UpdateConfig(const AppConfig &config) {
  if (!pSharedMem)
    return;
  pSharedMem->overlayConfig = config.overlay;
  pSharedMem->debugLogging = config.debugLogging;
  pSharedMem->logLevel = LogLevel::Info;

  // Copy log file path for hook logging
  strncpy(pSharedMem->logFilePath, config.logFilePath.c_str(),
          sizeof(pSharedMem->logFilePath) - 1);
  pSharedMem->logFilePath[sizeof(pSharedMem->logFilePath) - 1] = '\0';

  // Copy priority settings for hook/encoder
  pSharedMem->gpuPriority = config.video.gpuPriority;
  // Convert string to int: low=0, normal=1, high=2
  if (config.copyQueuePriority == "low")
    pSharedMem->copyQueuePriority = 0;
  else if (config.copyQueuePriority == "high")
    pSharedMem->copyQueuePriority = 2;
  else
    pSharedMem->copyQueuePriority = 1; // normal default

  // Copy fence wait mode for debugging
  pSharedMem->fenceWaitMode = config.fenceWaitMode;
  pSharedMem->useGameQueue = config.useGameQueue;

  // Copy FPS limiter settings
  pSharedMem->fpsLimiter.captureSyncEnabled =
      config.fpsLimiter.captureSyncEnabled;
  pSharedMem->fpsLimiter.captureSyncMultiplier =
      config.fpsLimiter.captureSyncMultiplier;
  pSharedMem->fpsLimiter.generalEnabled = config.fpsLimiter.generalEnabled;
  pSharedMem->fpsLimiter.generalFps = config.fpsLimiter.generalFps;
  pSharedMem->fpsLimiter.captureFps = config.video.fps;
  pSharedMem->fpsLimiter.useVFR = config.video.useVFR;

  // Copy Graphics Config (SharedGraphicsConfig)
  {
      auto& dst = pSharedMem->graphicsConfig;
      const auto& src = config.graphics;
      
      strncpy(dst.vsyncMode, src.vsyncMode.c_str(), sizeof(dst.vsyncMode)-1);
      dst.vsyncMode[sizeof(dst.vsyncMode)-1] = '\0';
      
      strncpy(dst.anisotropicFiltering, src.anisotropicFiltering.c_str(), sizeof(dst.anisotropicFiltering)-1);
      dst.anisotropicFiltering[sizeof(dst.anisotropicFiltering)-1] = '\0';
      
      strncpy(dst.mipMapping, src.mipMapping.c_str(), sizeof(dst.mipMapping)-1);
      dst.mipMapping[sizeof(dst.mipMapping)-1] = '\0';
      
      strncpy(dst.mipBias, src.mipBias.c_str(), sizeof(dst.mipBias)-1);
      dst.mipBias[sizeof(dst.mipBias)-1] = '\0';
      
      strncpy(dst.msaaSamples, src.msaaSamples.c_str(), sizeof(dst.msaaSamples)-1);
      dst.msaaSamples[sizeof(dst.msaaSamples)-1] = '\0';
      
      dst.prerenderLimit = src.cpuPrerenderLimit;
      dst.backbufferCount = src.backbufferCount;
      dst.sgssaa = src.sgssaa;
  }
}

bool IPCManager::GetLatestFrame(SharedMemoryLayout &outState) {
  if (!pSharedMem)
    return false;

  // Copy non-atomic fields manually to avoid copying atomics
  // The caller should use GetSharedMem() for direct atomic access
  outState.overlayConfig = pSharedMem->overlayConfig;
  outState.hostPID = pSharedMem->hostPID;
  outState.requestExit = pSharedMem->requestExit;
  outState.debugLogging = pSharedMem->debugLogging;
  memcpy(outState.logFilePath, pSharedMem->logFilePath,
         sizeof(outState.logFilePath));
  outState.sharedHandles[0] = pSharedMem->sharedHandles[0];
  outState.sharedHandles[1] = pSharedMem->sharedHandles[1];
  outState.sharedHandles[2] = pSharedMem->sharedHandles[2];
  outState.sharedHandles[3] = pSharedMem->sharedHandles[3];
  outState.currentReadIndex = pSharedMem->currentReadIndex;
  outState.fenceShareHandle = pSharedMem->fenceShareHandle;
  outState.fenceValue = pSharedMem->fenceValue;
  outState.timestamp = pSharedMem->timestamp;
  outState.width = pSharedMem->width;
  outState.height = pSharedMem->height;
  outState.format = pSharedMem->format;
  outState.isHDR = pSharedMem->isHDR;
  outState.luidLowPart = pSharedMem->luidLowPart;
  outState.luidHighPart = pSharedMem->luidHighPart;
  outState.sourcePid = pSharedMem->sourcePid;
  // Copy CaptureState fields individually (can't copy struct with atomics)
  outState.runtimeState.isRecording = pSharedMem->runtimeState.isRecording.load();
  outState.runtimeState.recordingStartTime = pSharedMem->runtimeState.recordingStartTime.load();
  outState.runtimeState.currentFPS = pSharedMem->runtimeState.currentFPS.load();
  outState.runtimeState.gameFPS = pSharedMem->runtimeState.gameFPS.load();
  outState.runtimeState.hostDroppedFrames = pSharedMem->runtimeState.hostDroppedFrames.load();
  // Atomic flags accessed directly, not copied
  // Note: atomics (throttleCapture, encoderQueueDepth, frameRing, logs) must be
  // accessed via GetSharedMem()
  return true;
}

void IPCManager::SendCaptureState(const CaptureState &state) {
  if (!pSharedMem)
    return;
  // Copy fields individually (can't copy struct with atomics)
  pSharedMem->runtimeState.isRecording.store(state.isRecording);
  pSharedMem->runtimeState.recordingStartTime.store(state.recordingStartTime);
  pSharedMem->runtimeState.currentFPS.store(state.currentFPS);
  pSharedMem->runtimeState.gameFPS.store(state.gameFPS);
  pSharedMem->runtimeState.hostDroppedFrames.store(state.hostDroppedFrames);
}

void IPCManager::UpdateHostStats(uint32_t droppedFrames) {
  if (!pSharedMem)
    return;
  pSharedMem->runtimeState.hostDroppedFrames.store(droppedFrames);
}

void IPCManager::UpdateReadIndex(uint32_t readIndex) {
  if (!pSharedMem)
    return;
  localReadIndex = readIndex;
  pSharedMem->frameRing.readIndex.store(readIndex, std::memory_order_release);
}

uint32_t IPCManager::GetReadIndex() const { return localReadIndex; }

SharedMemoryLayout *IPCManager::GetSharedMem() const { return pSharedMem; }

ShmemBuffer *IPCManager::GetShmem() const { return pShmem; }

void IPCManager::PollLogs() {
  if (!pSharedMem)
    return;

  // Simple ring buffer reader
  uint32_t writeIdx =
      pSharedMem->logs.writeIndex.load(std::memory_order_acquire);
  while (lastReadLogIndex < writeIdx) {
    // Don't read more than 16 lines back to avoid reading stale overwritten
    // data if we fell way behind (though in this design we just read
    // sequential) If we are way behind, jump to start?
    if (writeIdx - lastReadLogIndex > 16) {
      lastReadLogIndex = writeIdx - 16;
    }

    const char *msg = pSharedMem->logs.buffer[lastReadLogIndex % 16];
    // Log it
    printf("[Hook] %s\n", msg);
    // Also to file via LogInfo if available, but I don't have LogInfo here?
    // Use std::cout or similar, or include logging.h
    // Assuming logging.h is available
    extern void LogInfo(const char *fmt, ...);
    LogInfo("[Hook] %s", msg);

    lastReadLogIndex++;
  }
}

void IPCManager::UpdateThrottleState(uint32_t queueDepth, bool throttle) {
  if (!pSharedMem)
    return;
  pSharedMem->encoderQueueDepth.store(queueDepth, std::memory_order_relaxed);
  pSharedMem->throttleCapture.store(throttle, std::memory_order_release);
}

void IPCManager::SignalExit() {
  if (!pSharedMem)
    return;
  pSharedMem->requestExit = true;
}
