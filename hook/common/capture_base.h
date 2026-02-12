#pragma once

// Hook-specific capture base - includes shared base and adds IPCClient
// integration
#include "../../common/capture_base.h"
#include "hook_common.h"
#include "ipc_client.h"

// Extended CaptureBase for hook usage with IPCClient convenience methods
class HookCaptureBase : public CaptureBase {
public:
  // Publish shared handles - accepts IPCClient for backward compatibility
  void PublishToSharedMemory(IPCClient *ipc) {
    if (ipc && ipc->GetSharedMem()) {
      CaptureBase::PublishToSharedMemory(ipc->GetSharedMem());
      HookLog("CaptureBase: Published to shared memory (%ux%u, format %u)",
              width, height, format);
    }
  }

  // Signal frame ready - accepts IPCClient for backward compatibility
  void SignalFrameReady(IPCClient *ipc, int textureIndex, int64_t timestamp,
                        uint64_t gpuFenceValue) {
    if (ipc && ipc->GetSharedMem()) {
      CaptureBase::SignalFrameReady(ipc->GetSharedMem(), textureIndex,
                                    timestamp, gpuFenceValue);
    }
  }
};
