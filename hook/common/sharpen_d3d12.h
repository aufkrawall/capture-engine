#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <vector>

#include "../../common/sharpen_policy.h"
#include "sharpen_pass_log.h"

// D3D12 sharpen pass.
//
// Records its own command list and submits it on the queue that rendered the
// frame, the same way `SharedCaptureD3D12::CaptureFrame` does. Submitting on
// that queue is what orders the filter after the game's rendering and before
// the present: the per-frame path costs no fence and no wait.
//
// "The queue that rendered the frame" is not one queue for the life of a
// swapchain. The main route (dx12_hook_process_session_draw_main.cpp) submits on
// the game's queue; the post-Streamline route (dx12_hook_postsl_render_submit.cpp)
// submits on whichever queue that frame's overlay work went to, which can be
// Streamline's own swapchain queue. A DLSS-G activation switches CE between them
// mid-swapchain. Everything below that reasons about "the GPU has finished with
// X" therefore has to keep holding when the submitting queue changes, which is
// what RetireOnQueueChange and the deferred-release list exist for.
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
    // so they ring. The source texture does not: one fence orders every
    // submission the pass makes (see RetireOnQueueChange for why that holds
    // across a queue switch), so frame N's read of it always completes before
    // frame N+1's write to it begins.
    static constexpr UINT kAllocatorSlots = 8;

    bool EnsureDeviceObjects(ID3D12Device* device);
    bool EnsurePipelineState(ID3D12Device* device, Mode mode, DXGI_FORMAT viewFormat);
    bool EnsureSourceCopy(ID3D12Device* device, ID3D12Resource* target, DXGI_FORMAT viewFormat);
    bool EnsureTargetView(ID3D12Device* device, ID3D12Resource* target, DXGI_FORMAT viewFormat);
    // Index of an allocator the GPU has finished with, or -1 when all are busy.
    int AcquireAllocatorSlot();

    // Chains a new submitting queue behind everything already submitted on the
    // previous one, so `fence_` stays a single globally ordered timeline.
    //
    // Without this, two queues signalling one fence give completion values that
    // are not ordered against each other: queue B can reach 6 while queue A's 5
    // is still running, GetCompletedValue() reports 6, and the allocator recorded
    // for 5 gets Reset() out from under the GPU - and the one source texture is
    // written by one queue while the other still reads it. A GPU-side Wait costs
    // nothing on the CPU and is issued once per switch, not per frame.
    // False when the switch could not be ordered and the pass reset itself; the
    // caller must skip this frame rather than submit into an untrusted timeline.
    bool RetireOnQueueChange(ID3D12CommandQueue* queue);

    // Hands an object to the GPU timeline instead of releasing it now. D3D12
    // keeps no reference for a submitted command list, so releasing a pipeline
    // state or a texture that frame N-1 still references is a use-after-free.
    void RetireObject(Microsoft::WRL::ComPtr<IUnknown> object);
    // Releases everything the GPU has passed. `completed` is GetCompletedValue().
    void CollectRetired(UINT64 completed);
    // True once the GPU has passed every submission this pass has made, which is
    // the only point at which its single SRV descriptor may be rewritten.
    bool GpuIsIdle() const;

    // Identity only, never dereferenced.
    ID3D12Device* ownerDevice_ = nullptr;
    // Identity only, never dereferenced. The queue the previous submission used.
    ID3D12CommandQueue* lastQueue_ = nullptr;

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

    // Objects the pass has replaced but the GPU may still be reading, each held
    // until `fence_` passes the last value that could reference it.
    struct RetiredObject {
        Microsoft::WRL::ComPtr<IUnknown> object;
        UINT64 fenceValue = 0;
    };
    std::vector<RetiredObject> retired_;

    DecisionLogGate logGate_;
};

}  // namespace ce::sharpen
