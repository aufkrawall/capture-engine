#include "dx11_hook_internal.h"

#include "../common/sharpen_d3d11.h"
#include "../common/sharpen_request.h"

// DX11 entry point for the post-processing sharpen pass.
//
// The pass resolves its own target rather than borrowing the overlay's. Two
// reasons, either sufficient: the overlay may be switched off entirely, and
// with `capture_include_overlay=false` the capture runs *before* the overlay
// draws - so a filter that lived inside the overlay path would either never run
// or would leave the recording unsharpened while the screen was sharpened.
//
// The target is the buffer `IDXGISwapChain3::GetCurrentBackBufferIndex` names,
// which is the frame this Present will put on screen and is exactly what
// `DX11Capture::CaptureFrame` reads. Screen and recording therefore always
// agree.
namespace {

ce::sharpen::D3D11Pass g_SharpenPass;

// The view is retained across presents for the same buffer, the same way the
// overlay retains its own - except on a present interposer's private chain,
// which CE hooks present-only: the interposer recreates that chain through a
// ResizeBuffers CE never observes, so a retained view would pin a back buffer
// belonging to a swapchain that no longer exists.
ID3D11RenderTargetView* g_SharpenView = nullptr;
IDXGISwapChain* g_SharpenViewSwapChain = nullptr;
UINT g_SharpenViewBufferIndex = 0xFFFFFFFFu;
// True once the pass has been asked to render, which is the only way it
// allocates anything. Switching sharpening off then has something to hand back.
bool g_SharpenEverRendered = false;

void ReleaseRetainedSharpenView(bool releaseObjects = true) {
    if (g_SharpenView && releaseObjects)
        g_SharpenView->Release();
    g_SharpenView = nullptr;
    g_SharpenViewSwapChain = nullptr;
    g_SharpenViewBufferIndex = 0xFFFFFFFFu;
}

}  // namespace

void ReleaseDX11SharpenResources(bool releaseObjects) {
    ReleaseRetainedSharpenView(releaseObjects);
    if (releaseObjects) {
        g_SharpenPass.Shutdown();
    } else {
        // The device is already going away: releasing anything it owns can
        // fault, so the references are dropped instead.
        g_SharpenPass.Abandon();
    }
}

void SharpenDX11PresentedFrame(IDXGISwapChain* pSwapChain) {
    if (HookIsShuttingDown() || !pSwapChain)
        return;

    const ce::sharpen::Request request = ce::sharpen::ResolveRequest(GetActiveGraphicsConfigCached());
    if (request.mode == ce::sharpen::Mode::Off) {
        // Nothing is allocated and nothing is queried while the feature is off,
        // but anything already built is handed back rather than kept resident.
        if (g_SharpenEverRendered || g_SharpenView) {
            ReleaseDX11SharpenResources();
            g_SharpenEverRendered = false;
        }
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(pSwapChain->GetDesc(&desc)))
        return;

    ID3D11Device* device = nullptr;
    if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&device))) || !device)
        return;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) {
        device->Release();
        return;
    }

    const bool interposerPrivateOutputChain = DXGIShared::IsPresentOnPresentInterposerPrivateOutputChain();
    const UINT bufferIndex = ResolveDX11BackBufferIndex(pSwapChain, &desc);
    const bool retainView = !interposerPrivateOutputChain;

    if (!retainView || g_SharpenViewSwapChain != pSwapChain || g_SharpenViewBufferIndex != bufferIndex) {
        ReleaseRetainedSharpenView();
    }

    ID3D11RenderTargetView* targetView = g_SharpenView;
    ID3D11RenderTargetView* transientView = nullptr;
    if (!targetView) {
        ID3D11Texture2D* backbuffer = nullptr;
        if (SUCCEEDED(pSwapChain->GetBuffer(bufferIndex, IID_PPV_ARGS(&backbuffer))) && backbuffer) {
            ID3D11RenderTargetView* createdView = nullptr;
            if (SUCCEEDED(device->CreateRenderTargetView(backbuffer, nullptr, &createdView)) && createdView) {
                if (retainView) {
                    g_SharpenView = createdView;
                    g_SharpenViewSwapChain = pSwapChain;
                    g_SharpenViewBufferIndex = bufferIndex;
                    targetView = createdView;
                } else {
                    transientView = createdView;
                    targetView = createdView;
                }
            }
            backbuffer->Release();
        }
    }

    if (targetView) {
        const auto presentationEncoding =
            DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, desc.BufferDesc.Format);
        const bool isHDR = ce::presentation_color::IsHDR(presentationEncoding);
        // A present interposer's private output chain carries every displayed
        // frame, generated ones included, so it is a frame route like any other.
        const ce::sharpen::Route route = interposerPrivateOutputChain
                                             ? ce::sharpen::Route::InterposerOutputChain
                                             : ce::sharpen::Route::NormalBackbuffer;
        g_SharpenEverRendered = true;
        g_SharpenPass.Render(device, context, targetView, request, route,
                             ce::sharpen::ResolveDxgiEncoding(desc.BufferDesc.Format, isHDR));
    }

    if (transientView)
        transientView->Release();
    context->Release();
    device->Release();
}
