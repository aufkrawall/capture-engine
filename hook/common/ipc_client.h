#pragma once
#include <Windows.h>
#include "../../common/shared_defs.h"

class IPCClient {
public:
    IPCClient();
    ~IPCClient();

    bool Connect();
    void Disconnect();

    SharedMemoryLayout* GetSharedMem() { return pSharedMem; }
    ShmemBuffer* GetShmem();  // Returns current mapping or attempts to connect if metadata exists

    // Check if host requested capture
    bool IsRecording() const { return pSharedMem && pSharedMem->runtimeState.isRecording; }

private:
    HANDLE hMapFile;
    SharedMemoryLayout* pSharedMem;

    HANDLE hMapShmem;
    ShmemBuffer* pShmem;
};
