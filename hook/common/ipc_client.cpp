#include "ipc_client.h"

IPCClient::IPCClient() : hMapFile(NULL), pSharedMem(nullptr), hMapShmem(NULL), pShmem(nullptr) {}

IPCClient::~IPCClient() {
    Disconnect();
}

extern void EarlyLog(const char* fmt, ...);

bool IPCClient::Connect() {
    if (pSharedMem)
        return true;

    // Use discovery shared memory for fast lookup of inject process PID
    // This is O(1) instead of scanning thousands of PIDs
    HANDLE hDiscovery = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (hDiscovery) {
        DiscoveryInfo* pDiscovery =
            (DiscoveryInfo*)MapViewOfFile(hDiscovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));

        if (pDiscovery) {
            // CRITICAL FIX: Use atomic accessors for thread-safe reads
            uint32_t magic = pDiscovery->GetMagic();
            uint32_t pid = pDiscovery->GetInjectPid();

            if (magic == DISCOVERY_MAGIC && pid != 0) {
                // CRITICAL FIX: Use 'pid' variable (atomically loaded) instead of
                // pDiscovery->injectPid (raw field) to avoid TOCTOU race condition.
                // The raw field could change between validation and use.
                wchar_t sharedMemName[64];
                GenerateSharedMemName(sharedMemName, 64, pid);

                EarlyLog("IPC: Found discovery. InjectPID=%d. Opening %ls...", pid, sharedMemName);

                hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sharedMemName);
                if (hMapFile) {
                    // Use 0 to map the entire section — the creator (64-bit inject process)
                    // determined the size. Using sizeof(SharedMemoryLayout) would fail if the
                    // local sizeof differs (e.g., 32-bit layer reading 64-bit shared memory).
                    pSharedMem = (SharedMemoryLayout*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, 0);

                    if (pSharedMem && pSharedMem->GetHostPID() != 0) {
                        UnmapViewOfFile(pDiscovery);
                        CloseHandle(hDiscovery);
                        EarlyLog("IPC: Connected! HostPID=%d", pSharedMem->GetHostPID());
                        return true;
                    }

                    // Failed to map - cleanup
                    if (pSharedMem) {
                        UnmapViewOfFile(pSharedMem);
                        pSharedMem = nullptr;
                    }
                    CloseHandle(hMapFile);
                    hMapFile = NULL;
                    EarlyLog("IPC: Failed to map view of main shared memory. Error=%d", GetLastError());
                } else {
                    EarlyLog("IPC: Failed to open main shared memory '%ls'. Error=%d", sharedMemName, GetLastError());
                }

                UnmapViewOfFile(pDiscovery);
            } else {
                EarlyLog("IPC: Discovery found but invalid magic/pid. Magic=%X, PID=%d", magic, pid);
                UnmapViewOfFile(pDiscovery);
            }
        } else {
            EarlyLog("IPC: Failed to map discovery view. Error=%d", GetLastError());
        }
        CloseHandle(hDiscovery);
    } else {
        EarlyLog("IPC: Failed to open discovery mapping '%ls'. Error=%d", SHARED_MEM_DISCOVERY, GetLastError());
    }

    return false;
}

ShmemBuffer* IPCClient::GetShmem() {
    if (pShmem)
        return pShmem;
    if (!pSharedMem || !pSharedMem->GetShmemMappingCreated())
        return nullptr;

    // Connect to separate shmem mapping
    wchar_t shmemName[64];
    GenerateShmemName(shmemName, 64, pSharedMem->GetHostPID());

    hMapShmem = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmemName);
    if (hMapShmem) {
        // Map the full size created by the host to ensure slot offsets are valid
        size_t mapSize = pSharedMem->GetShmemMappingSize();

        pShmem = (ShmemBuffer*)MapViewOfFile(hMapShmem, FILE_MAP_ALL_ACCESS, 0, 0, mapSize);
        if (pShmem) {
            EarlyLog("IPC: Connected to Shmem buffer '%ls' (mapped %zu bytes)", shmemName, mapSize);
            return pShmem;
        }
        CloseHandle(hMapShmem);
        hMapShmem = NULL;
    }
    return nullptr;
}

void IPCClient::Disconnect() {
    if (pShmem) {
        UnmapViewOfFile(pShmem);
        pShmem = nullptr;
    }
    if (hMapShmem) {
        CloseHandle(hMapShmem);
        hMapShmem = NULL;
    }
    if (pSharedMem) {
        UnmapViewOfFile(pSharedMem);
        pSharedMem = nullptr;
    }
    if (hMapFile) {
        CloseHandle(hMapFile);
        hMapFile = NULL;
    }
}
