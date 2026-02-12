#pragma once

#include "../common/config.h"
#include "../common/shared_defs.h"
#include <Windows.h>

class IPCManager {
public:
  IPCManager(const AppConfig &config);
  ~IPCManager();

  bool Init();
  void UpdateConfig(const AppConfig &config);
  bool GetLatestFrame(SharedMemoryLayout &outState);
  void SendCaptureState(const CaptureState &state);
  void UpdateHostStats(uint32_t droppedFrames);
  void UpdateReadIndex(uint32_t readIndex); // Update ring buffer read position
  uint32_t GetReadIndex() const;            // Get current read position
  SharedMemoryLayout *
  GetSharedMem() const;          // Get direct pointer (for atomic access)
  ShmemBuffer *GetShmem() const; // Get direct pointer to fallback buffer
  void PollLogs();
  void
  UpdateThrottleState(uint32_t queueDepth,
                      bool throttle); // Throttle capture when encoder behind
  void SignalExit();                  // Tell hook to gracefully unload

private:
  const AppConfig &config;
  HANDLE hMapFile;
  SharedMemoryLayout *pSharedMem;

  HANDLE hMapShmem;
  ShmemBuffer *pShmem;

  // For synchronization if we implement sophisticated rings
  uint32_t lastReadLogIndex = 0;
  uint32_t localReadIndex = 0; // Ring buffer read position (consumer side)
};
