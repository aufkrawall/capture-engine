#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "../../common/sharpen_policy.h"
#include "sharpen_pass_log.h"

// D3D12 sharpen pass.
//
// Records its own command list and submits it on the queue that rendered the
// frame, the same way `SharedCaptureD3D12::CaptureFrame` does. Submitting on
// that queue is what orders the filter after the game's rendering and before
// the present without a single cross-queue fence or wait.
namespace ce::sharpen {

class D3D12Pass {
public:
    // `target` is the resource this Present will put on screen, in
    // `targetStateBefore` when the pass starts and restored to it when the pass
    // ends. Returns true when the filter was recorded and submitted.
    bool Render(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* target, DXGI_FORMAT viewFormat,
                D3D12_RESOURCE_STATES targetStateBefore, const Request& request, Route route,
                TargetEncoding encoding);

    void Shutdown();

    // Drops every reference without releasing it, for a device that is already
    // being torn down. Deliberately leaks; see D3D11Pass::Abandon.
    void Abandon();

private:
    // Command allocators cannot be reset until the GPU has finished with them,
    // so they ring. The source texture does not: the pass is submitted on one
    // queue, which executes in order, so frame N's read of it always completes
    // before frame N+1's write to it begins.
    static constexpr UINT kAllocatorSlots = 8;

    bool EnsureDeviceObjects(ID3D12Device* device);
    bool EnsurePipelineState(ID3D12Device* device, Mode mode, DXGI_FORMAT viewFormat);
    bool EnsureSourceCopy(ID3D12Device* device, ID3D12Resource* target, DXGI_FORMAT viewFormat);
    bool EnsureTargetView(ID3D12Device* device, ID3D12Resource* target, DXGI_FORMAT viewFormat);
    // Index of an allocator the GPU has finished with, or -1 when all are busy.
    int AcquireAllocatorSlot();

    // Identity only, never dereferenced.
    ID3D12Device* ownerDevice_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState_;
    Mode pipelineMode_ = Mode::Off;
    DXGI_FORMAT pipelineFormat_ = DXGI_FORMAT_UNKNOWN;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocators_[kAllocatorSlots];
    UINT64 allocatorFenceValues_[kAllocatorSlots] = {};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    UINT64 fenceValue_ = 0;

    // One shader-visible SRV for the source copy, and one CPU-only RTV for the
    // frame. Both heaps hold exactly one descriptor: nothing else is bound.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;

    Microsoft::WRL::ComPtr<ID3D12Resource> sourceCopy_;
    UINT64 copyWidth_ = 0;
    UINT copyHeight_ = 0;
    DXGI_FORMAT copyFormat_ = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT copyViewFormat_ = DXGI_FORMAT_UNKNOWN;

    // The resource the current RTV descriptor was written for.
    ID3D12Resource* viewedTarget_ = nullptr;
    DXGI_FORMAT viewedFormat_ = DXGI_FORMAT_UNKNOWN;

    DecisionLogGate logGate_;
};

}  // namespace ce::sharpen
