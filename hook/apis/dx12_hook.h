#pragma once
#include "graphics_hook.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <mutex>
#include <vector>

class DX12Hook : public GraphicsHook {
  std::vector<IUnknown*> trackedResources;
  std::mutex resourceMutex;
public:
  void Init() override;
  void Shutdown() override;
  void OnHostDisconnect() override;

  void TrackResource(IUnknown* res);
  void CleanupResources();
};

extern DX12Hook g_dx12HookInstance;


 void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain);
 void DX12_OnSwapchainResizeBegin();
