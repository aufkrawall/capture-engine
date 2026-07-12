#pragma once
#include <windows.h>
#include "../../common/shared_defs.h"

class IPCClient {
public:
    IPCClient();
    ~IPCClient();

    bool Connect();
    void Disconnect();

    SharedMemoryLayout* GetSharedMem() {
        return pSharedMem;
    }
    ShmemBuffer* GetShmem();  // Returns current mapping or attempts to connect if
                              // metadata exists
    bool SignalInjectFrameReady();

    // Check whether the active video backend consumes injected frames, including
    // the short hidden startup warmup. WGC/DXGI recordings still set the session's
    // raw captureRequested bit for REC/limiter state, but do not enable hook copies.
    bool IsCaptureRequested() const {
        return pSharedMem && pSharedMem->runtimeState.IsInjectVideoCaptureRequested();
    }

    bool IsRecording() const {
        return IsCaptureRequested();
    }

private:
    HANDLE hMapFile;
    SharedMemoryLayout* pSharedMem;

    HANDLE hMapShmem;
    ShmemBuffer* pShmem;
    HANDLE hInjectFrameReadyEvent;
};
