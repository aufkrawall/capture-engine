#pragma once

#include <d3d12.h>
#include <cstdint>
#include <vector>

using ExecuteCommandListsPtr = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using SignalPtr = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);

namespace ce::dx12_queue_dispatch {

struct Binding {
    void** vtable;
    ExecuteCommandListsPtr original;
};
enum class CaptureResult { kCaptured, kKnown, kFollower, kFailed, kRetired };
struct Capture {
    CaptureResult result;
    ExecuteCommandListsPtr original = nullptr;
};
struct SignalCapture {
    CaptureResult result;
    SignalPtr original = nullptr;
};

Capture CaptureVTable(void** vtable);
ExecuteCommandListsPtr Resolve(ID3D12CommandQueue* queue);
SignalCapture CaptureSignalVTable(void** vtable);
SignalPtr ResolveSignal(ID3D12CommandQueue* queue);
bool HasBinding(ID3D12CommandQueue* queue);
std::vector<Binding> Snapshot();
uint64_t Generation();
void Reset();

}  // namespace ce::dx12_queue_dispatch
