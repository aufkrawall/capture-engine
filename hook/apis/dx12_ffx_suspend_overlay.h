#pragma once

#include <d3d12.h>
#include <dxgi.h>

namespace ce::dx12_ffx_suspend_overlay {

using SubmitCommandListCallback = bool (*)(ID3D12CommandQueue* queue, ID3D12CommandList* commandList);
using SignalFenceCallback = HRESULT (*)(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value);

struct RenderRequest {
    IDXGISwapChain* proxySwapChain = nullptr;
    ID3D12CommandQueue* presentationQueue = nullptr;
    // Null selects the proxy's current replacement backbuffer (suspension route). A non-null target selects
    // an already-retained replacement backbuffer or the registered FFX UI resource while keeping the same
    // owner-queue ordering.
    ID3D12Resource* targetResource = nullptr;
    D3D12_RESOURCE_STATES targetState = D3D12_RESOURCE_STATE_PRESENT;
    bool clearTransparent = false;
    const char* routeName = "suspend-backbuffer";
    SubmitCommandListCallback submitCommandList = nullptr;
    SignalFenceCallback signalFence = nullptr;
    bool hdr = false;
};

// Renders immediately before the game calls the FFX proxy Present. The command list is submitted on the
// target-compatible owner queue (the exact FFX queue or a validated real queue beneath its Streamline wrapper),
// so queue order provides the handoff into Present without a CPU wait or foreign-queue access.
bool Render(const RenderRequest& request);

// Retire resources tied to one proxy when its FFX swapchain context is destroyed/rebound. In-flight resources
// remain referenced until their real GPU fence completes; this never waits on the Present thread. Keying by
// proxy avoids tearing down a new context that legitimately shares the same game queue with an older context.
void RetireProxy(void* proxySwapChain, const char* reason);

// Called during DX12 hook teardown. Incomplete healthy-queue state is intentionally abandoned rather than
// blocking or releasing objects still referenced by the GPU.
void Shutdown(const char* reason);

}  // namespace ce::dx12_ffx_suspend_overlay
