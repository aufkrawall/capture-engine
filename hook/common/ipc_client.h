#pragma once
#include "../../common/shared_defs.h"
#include <Windows.h>

class IPCClient {
public:
  IPCClient();
  ~IPCClient();

  bool Connect();
  void Disconnect();

  SharedMemoryLayout *GetSharedMem() { return pSharedMem; }

  // Check if host requested capture
  bool IsRecording() const {
    return pSharedMem && pSharedMem->runtimeState.isRecording;
  }

private:
  HANDLE hMapFile;
  SharedMemoryLayout *pSharedMem;
};
