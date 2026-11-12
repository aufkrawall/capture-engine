#pragma once

// Who presents the no-callback FSR outputs, as seen from the presenter thread: the queue the presented swapchain's
// buffers belong to and the code that composes into them (dx12_hook_ffx_presentation.cpp). The final-batch route
// (dx12_hook_ffx_topmost_batch.cpp) anchors CE's draw only on a batch that queue runs and that code submitted.

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "dx12_overlay_policy/ffx_topmost_batch.h"

struct IDXGISwapChain;
struct ID3D12CommandQueue;

struct DX12FFXPresentation {
    IDXGISwapChain* swapChain = nullptr;
    // The native queue that owns the swapchain's buffers (its create queue, or the one a ResizeBuffers1 bound); null
    // when CE never saw the swapchain created. Identity only: the presenter owns the queue.
    ID3D12CommandQueue* ownerQueue = nullptr;
    // The presenter nested between the runtime and the native swapchain (ce::dx12_overlay_policy::PresenterImages),
    // and its call into the layer below it: its own Present site. All 0 when the runtime presents the swapchain itself.
    uintptr_t nestedBase = 0;
    uintptr_t nestedEnd = 0;
    uintptr_t nestedPresentSite = 0;
};

// The presentation for the Present this thread is in: `presentOriginatorFrame` is that Present's originator
// (PresentOrigin::originatorFrame). A nested presenter is the originator's module when it is not the runtime image
// and it created the native swapchain (ce::fg_presenter::Binding::creator). Cached per thread; a change of
// swapchain, registry, originator or runtime image refreshes it with one line in the log.
const DX12FFXPresentation& DX12_RefreshFFXPresentation(IDXGISwapChain* swapChain, const void* presentOriginatorFrame);

// This thread's presentation as its last Present left it: what its submissions until the next one are judged by.
const DX12FFXPresentation& DX12_CurrentFFXPresentation();

// The presenting images for this thread: the runtime's (DX12_GetPresentingFFXRuntimeImage) and, with
// `includeNested`, the nested presenter's. False when the runtime image is unknown.
bool DX12_CurrentFFXPresenterImages(bool includeNested, ce::dx12_overlay_policy::PresenterImages* images);

// The nearest return address inside either image on the calling thread's stack (NearestPresenterFrame). A short walk
// covers the common case; a long one reaches a runtime Present call beneath deep Present chains (GTA V Enhanced:
// Social Club, Epic's overlay, Streamline and ReShade).
ce::dx12_overlay_policy::PresenterFrame DX12_FindNearestFFXPresenterFrame(
    const ce::dx12_overlay_policy::PresenterImages& images);

// "module.dll+0x1234" (base name only), or the bare address: for the route's rate-limited diagnostics.
void DX12_DescribeCodeAddress(uintptr_t address, char* out, size_t outSize);
