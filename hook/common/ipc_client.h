#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include "../../common/shared_defs.h"

class IPCClient {
public:
    IPCClient();
    ~IPCClient();

    bool Connect();
    // Atomically publishes a new host mapping while intentionally retaining the
    // previous mapping. In-flight detours can therefore finish against the old
    // session without a use-after-unmap during host restart.
    bool Reconnect();
    void Disconnect();

    SharedMemoryLayout* GetSharedMem() const {
        return publishedSharedMem.load(std::memory_order_acquire);
    }
    ShmemBuffer* GetShmem();  // Returns current mapping or attempts to connect if
                              // metadata exists
    bool SignalInjectFrameReady();

    // Check whether the active video backend consumes injected frames, including
    // the short hidden startup warmup. WGC/DXGI recordings still set the session's
    // raw captureRequested bit for REC/limiter state, but do not enable hook copies.
    bool IsCaptureRequested() const {
        SharedMemoryLayout* sharedMemory = GetSharedMem();
        return sharedMemory && sharedMemory->runtimeState.IsInjectVideoCaptureRequested();
    }

    bool IsRecording() const {
        return IsCaptureRequested();
    }

private:
    bool ConnectLocked();

    std::mutex connectionMutex;
    HANDLE hMapFile;
    SharedMemoryLayout* pSharedMem;
    std::atomic<SharedMemoryLayout*> publishedSharedMem;

    HANDLE hMapShmem;
    ShmemBuffer* pShmem;
    HANDLE hInjectFrameReadyEvent;
};
