#include "dx12_hook_internal.h"

#include <mutex>

#include "../common/sharpen_d3d11.h"
#include "../common/sharpen_d3d12.h"
#include "../common/sharpen_request.h"

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
namespace {

ce::sharpen::D3D12Pass g_SharpenPass;
// True once the pass has been asked to render, which is the only way it
// allocates anything. Switching sharpening off then has something to hand back.
bool g_SharpenEverRendered = false;
std::mutex g_SharpenMutex;

// The teardown body, for callers that already own g_SharpenMutex.
//
// g_SharpenMutex is a plain std::mutex, so the locking entry point below must
// never be reached from a path that already holds it. In 0.1.6741 the
// sharpen-off branch of SharpenDX12PresentedFrame did exactly that: it took the
// lock, resolved `sharpen=off`, and then called the locking release. libc++ maps
// std::mutex onto an SRWLOCK, so the second acquire parked the thread forever.
// The thread it parked was the RHI/present thread inside DetourPresent, which
// UE5 reported as "GameThread timed out waiting for RenderThread after 120.00
// secs" before terminating the game (session 20260920_192913).
void ReleaseSharpenResourcesLocked(bool releaseObjects) {
    if (releaseObjects) {
        g_SharpenPass.Shutdown();
    } else {
        // The device is already going away: releasing anything it owns can
        // fault, so the references are dropped instead.
        g_SharpenPass.Abandon();
    }
    g_SharpenEverRendered = false;
}

}  // namespace

void ReleaseDX12SharpenResources(bool releaseObjects) {
    std::lock_guard<std::mutex> lock(g_SharpenMutex);
    ReleaseSharpenResourcesLocked(releaseObjects);
}

void SharpenDX12PresentedFrame(IDXGISwapChain* pSwapChain, ID3D12CommandQueue* queue, bool hasBackBufferIndex,
                               UINT backBufferIndex) {
    if (HookIsShuttingDown() || !pSwapChain || !queue)
        return;

    std::unique_lock<std::mutex> lock(g_SharpenMutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;

    const ce::sharpen::Request request = ce::sharpen::ResolveRequest(GetActiveGraphicsConfigCached());
    if (request.mode == ce::sharpen::Mode::Off) {
        // A full-frame copy plus its heaps and allocators is not something to
        // keep resident after the feature is switched off. The lock is already
        // held here, so this must be the unlocked body.
        if (g_SharpenEverRendered) {
            ReleaseSharpenResourcesLocked(true);
        }
        return;
    }

    ID3D12Device* device = g_Device.load(std::memory_order_acquire);
    if (!device)
        return;

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
        return;

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
    g_SharpenEverRendered = true;
    g_SharpenPass.Render(device, queue, backBuffer.Get(), resourceDesc.Format, D3D12_RESOURCE_STATE_PRESENT, request,
                         route, ce::sharpen::ResolveDxgiEncoding(resourceDesc.Format, isHDR));
}
