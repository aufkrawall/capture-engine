#pragma once
#include <windows.h>
#include <cstdint>
#include "../common/config.h"
#include "../common/shared_defs.h"

// Manages shared memory IPC between captureengine and the injected hook DLL.
class IPCManager {
public:
    explicit IPCManager(const AppConfig& config);
    ~IPCManager();

    bool Init();
    void UpdateConfig(const AppConfig& config);

    bool GetLatestFrame(SharedMemoryLayout& outState);
    void SendCaptureState(const CaptureState& state);
    void UpdateHostStats(uint32_t droppedFrames);
    void UpdateReadIndex(uint32_t readIndex);
    uint32_t GetReadIndex() const;

    SharedMemoryLayout* GetSharedMem() const;
    ShmemBuffer* GetShmem() const;

    void PollLogs();
    void UpdateThrottleState(uint32_t queueDepth, bool throttle);
    void SignalExit();

private:
    const AppConfig& config;
    HANDLE hMapFile;
    SharedMemoryLayout* pSharedMem;
    HANDLE hMapShmem;
    ShmemBuffer* pShmem;
    uint32_t localReadIndex{0};
    uint32_t lastReadLogIndex{0};
};
