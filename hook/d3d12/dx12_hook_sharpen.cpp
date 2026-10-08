#include "dx12_hook_internal.h"

#include <mutex>

#include "hook/sharpen/sharpen_d3d11.h"
#include "hook/sharpen/sharpen_d3d12.h"
#include "hook/sharpen/sharpen_request.h"
#include "hook/sharpen/gamma_external_submission.h"

// DX12 entry point for the post-processing sharpen pass.
//
// Like the DX11 one this resolves its own target instead of borrowing the
// overlay's, because the overlay can be switched off and because the capture
// runs before the overlay draws whenever `capture_include_overlay=false`. The
// target is the buffer `GetCurrentBackBufferIndex` names, which is both the
// frame this Present puts on screen and what `PublishDX12CapturedFrame` copies.
//
// The work is submitted on the queue that rendered the frame. A single queue
// executes in order, so that submission alone orders the filter after the
// game's rendering and before the present, with no cross-queue wait and nothing
// added to CE's own overlay queue.
namespace ce::sharpen {
void ExecutePostProcessCommandList(ID3D12CommandQueue* queue, ID3D12CommandList* list) {
    ID3D12CommandList* lists[] = {list};
    ScopedCEOverlayECLSubmission submission("post-process");
    queue->ExecuteCommandLists(1, lists);
}
}

void ReleaseDX12SharpenResources(bool releaseObjects) {
    // In-flight entries remain owned until their exact completion proof retires.
    // A device already being torn down is left untouched.
    if (releaseObjects)
        ce::sharpen::CollectRuntimePostProcess(true);
}

ce::post_process_route::PassResult SharpenDX12PresentedFrame(IDXGISwapChain* pSwapChain, ID3D12CommandQueue* queue,
                                                             bool hasBackBufferIndex, UINT backBufferIndex) {
    using ce::post_process_route::PassResult;
    if (HookIsShuttingDown() || !pSwapChain || !queue)
        return PassResult::Unavailable;
    if (DX12_PostProcessAlreadyRendered(pSwapChain))
        return PassResult::AlreadyApplied;

    const ce::sharpen::Request request = ce::sharpen::ResolveRequest(GetActiveGraphicsConfigCached());
    if (!ce::sharpen::Requested(request)) {
        ce::sharpen::CollectRuntimePostProcess(true);
        return PassResult::NotRequested;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&device))))
        return PassResult::Failed;

    UINT bufferIndex = backBufferIndex;
    if (!hasBackBufferIndex) {
        Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain3;
        if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) && swapChain3) {
            bufferIndex = swapChain3->GetCurrentBackBufferIndex();
        } else {
            bufferIndex = 0;
        }
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> backBuffer;
    if (FAILED(pSwapChain->GetBuffer(bufferIndex, IID_PPV_ARGS(&backBuffer))) || !backBuffer)
        return PassResult::Failed;

    DXGI_SWAP_CHAIN_DESC desc = {};
    const bool haveDesc = SUCCEEDED(pSwapChain->GetDesc(&desc));
    const auto presentationEncoding =
        haveDesc ? DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, desc.BufferDesc.Format)
                 : ce::presentation_color::Encoding::Unsupported;
    const bool isHDR = ce::presentation_color::IsHDR(presentationEncoding);

    // A present interposer's private output chain carries every displayed frame,
    // generated ones included, so it is a frame route like any other.
    const ce::sharpen::Route route = DXGIShared::IsPresentOnPresentInterposerPrivateOutputChain()
                                         ? ce::sharpen::Route::InterposerOutputChain
                                         : ce::sharpen::Route::NormalBackbuffer;

    // The view keeps the resource's own format. DX12 flip-model swapchains are
    // created UNORM and an application opts into sRGB per view, so reading and
    // writing through the storage format keeps the filter in the stored
    // perceptual space instead of paying for a decode/encode round trip.
    const D3D12_RESOURCE_DESC resourceDesc = backBuffer->GetDesc();
    if (!ce::sharpen::RenderPresentedPostProcess(device.Get(), queue, backBuffer.Get(), resourceDesc.Format,
                                                 request, route,
                                                 ce::sharpen::ResolveDxgiEncoding(resourceDesc.Format, isHDR)))
        return PassResult::Failed;
    DX12_MarkPostProcessRendered(pSwapChain);
    return PassResult::Applied;
}
