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
    // False records only the requested clear/transitions, or only the inline completion marker when clear is
    // also false. The FSR handoffs use a marker-only submit to prove the new route before it may blend pixels.
    bool renderOverlay = true;
    const char* routeName = "suspend-backbuffer";
    SubmitCommandListCallback submitCommandList = nullptr;
    SignalFenceCallback signalFence = nullptr;
    // Record a GPU completion marker at the tail of the overlay command list instead of adding a queue Signal.
    // Used only when the submit callback appends that list to an existing foreign ECL batch: AMD then observes
    // the same queue operation it would have seen without CE, while the marker still protects allocator/upload
    // slot reuse and target lifetime. The ordinary owner-queue routes retain their explicit fence contract.
    bool inlineCompletionMarker = false;
    bool hdr = false;
};

// Renders immediately before the game calls the FFX proxy Present. The command list is submitted on the
// target-compatible owner queue (the exact FFX queue or a validated real queue beneath its Streamline wrapper),
// so queue order provides the handoff into Present without a CPU wait or foreign-queue access.
bool Render(const RenderRequest& request);

// Latches true after at least one inline-marker submission for this swapchain key reaches its command-list tail
// on the GPU. Later in-flight outputs do not erase that route proof; their allocator/upload slots are guarded
// independently. Used to hand overlay ownership away from the FFX UI texture only after real GPU completion.
bool HasCompletedInlineRender(void* proxySwapChain);

// Invalidates only the route-activation latch when the learned ECL signature changes or an expected append is
// missed. In-flight slot markers and retained resources remain intact; the next route must complete a fresh
// marker-only probe before it can retire the UI baseline again.
void ResetInlineCompletionProof(void* proxySwapChain);

// Retire resources tied to one proxy when its FFX swapchain context is destroyed/rebound. In-flight resources
// remain referenced until their queue fence or inline marker completes; this never waits on the Present thread.
// Keying by proxy avoids tearing down a new context that legitimately shares the same game queue with an older
// context.
void RetireProxy(void* proxySwapChain, const char* reason);

// Retires every live renderer state. Used at native-FSR teardown boundaries (explicit Streamline enable prep
// and FFX context destruction) so the below-foreign-chain state — keyed by the presented FFX swapchain rather
// than the registered game-facing proxy — cannot keep command lists/backbuffer references alive across the FFX
// swapchain teardown (the documented E_ACCESSDENIED boundary for the game's resize/present).
void RetireAllForNativeFSRTeardown(const char* reason);

// Called during DX12 hook teardown. Incomplete healthy-queue state is intentionally abandoned rather than
// blocking or releasing objects still referenced by the GPU.
void Shutdown(const char* reason);

}  // namespace ce::dx12_ffx_suspend_overlay
