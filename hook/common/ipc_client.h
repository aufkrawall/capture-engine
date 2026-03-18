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

    // Check if host requested capture, including the short hidden startup warmup.
    bool IsCaptureRequested() const {
        return pSharedMem && pSharedMem->runtimeState.captureRequested.load(std::memory_order_acquire);
    }

    bool IsRecording() const {
        return IsCaptureRequested();
    }

private:
    HANDLE hMapFile;
    SharedMemoryLayout* pSharedMem;

    HANDLE hMapShmem;
    ShmemBuffer* pShmem;
};
