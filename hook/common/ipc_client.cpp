#include "ipc_client.h"

IPCClient::IPCClient() : hMapFile(NULL), pSharedMem(nullptr), hMapShmem(NULL), pShmem(nullptr) {}

IPCClient::~IPCClient() { Disconnect(); }

extern void EarlyLog(const char* fmt, ...);

bool IPCClient::Connect() {
  if (pSharedMem)
    return true;

  // Use discovery shared memory for fast lookup of inject process PID
  // This is O(1) instead of scanning thousands of PIDs
  HANDLE hDiscovery = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
  if (hDiscovery) {
    DiscoveryInfo* pDiscovery = (DiscoveryInfo*)MapViewOfFile(
        hDiscovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
    
    if (pDiscovery) {
        if (pDiscovery->magic == DISCOVERY_MAGIC && pDiscovery->injectPid != 0) {
          // Found discovery info - use inject PID to open main shared memory
          wchar_t sharedMemName[64];
          GenerateSharedMemName(sharedMemName, 64, pDiscovery->injectPid);
          
          EarlyLog("IPC: Found discovery. InjectPID=%d. Opening %ls...", pDiscovery->injectPid, sharedMemName);
          
          hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sharedMemName);
          if (hMapFile) {
            pSharedMem = (SharedMemoryLayout*)MapViewOfFile(
                hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemoryLayout));
            
            if (pSharedMem && pSharedMem->hostPID != 0) {
              UnmapViewOfFile(pDiscovery);
              CloseHandle(hDiscovery);
              EarlyLog("IPC: Connected! HostPID=%d", pSharedMem->hostPID);
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
            EarlyLog("IPC: Discovery found but invalid magic/pid. Magic=%X, PID=%d", pDiscovery->magic, pDiscovery->injectPid);
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
    if (pShmem) return pShmem;
    if (!pSharedMem || !pSharedMem->shmemMappingCreated) return nullptr;
    
    // Connect to separate shmem mapping
    wchar_t shmemName[64];
    GenerateShmemName(shmemName, 64, pSharedMem->hostPID);
    
    hMapShmem = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmemName);
    if (hMapShmem) {
        pShmem = (ShmemBuffer*)MapViewOfFile(hMapShmem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShmemBuffer));
        if (pShmem) {
            EarlyLog("IPC: Connected to Shmem buffer '%ls'", shmemName);
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
