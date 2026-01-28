#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <mutex>
#include <vector>
#include "graphics_hook.h"

class DX12Hook : public GraphicsHook {
    std::vector<IUnknown*> trackedResources;
    std::recursive_mutex resourceMutex;

public:
    void Init() override;
    void Shutdown() override;
    void OnHostDisconnect() override;

    void TrackResource(IUnknown* res);
    void CleanupResources();
};

extern DX12Hook* g_dx12HookInstance;

void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain);
void DX12_HookQueueVTable(ID3D12CommandQueue* queue);
void DX12_OnSwapchainResizeBegin();
void DX12_OnSwapchainResizeEnd();
void DX12_InvalidateSwapchain();
void DX12_SignalFSR4SwapchainRecreated();
void DX12_AdjustWrapperResizeDepth(int delta);

extern "C" {
__declspec(dllexport) void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue);
__declspec(dllexport) void DX12_AdjustWrapperResizeDepth_C(int delta);
}
