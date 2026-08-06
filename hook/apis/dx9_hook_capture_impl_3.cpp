#include "dx9_hook_internal.h"


bool DX9Capture::SetupGDIInterop(IDirect3DDevice9* device) {


        if (!d3d11Device || !d3d11Context) {
            HookLogImportant("DX9: GDI interop: D3D11 device not available (dev=%p ctx=%p)", d3d11Device, d3d11Context);
            return false;
        }

        // Create TWO lockable D3D9 render targets for double-buffered capture.
        // Double-buffering eliminates GPU pipeline stalls: we StretchRect to one RT
        // while GetDC reads from the other (written last frame, already complete).
        for (int i = 0; i < 2; i++) {
            HRESULT hr = device->CreateRenderTarget(width, height, d3d9Format, D3DMULTISAMPLE_NONE, 0, TRUE,
                                                    &gdiCopySurfaces[i], nullptr);
            if (FAILED(hr)) {
                HookLogImportant("DX9: GDI interop: CreateRenderTarget[%d] failed (0x%08x)", i, (unsigned)hr);
                for (int j = 0; j < i; j++) {
                    gdiCopySurfaces[j]->Release();
                    gdiCopySurfaces[j] = nullptr;
                }
                return false;
            }
            HDC testDC = nullptr;
            hr = gdiCopySurfaces[i]->GetDC(&testDC);
            if (FAILED(hr) || !testDC) {
                HookLogImportant("DX9: GDI interop: GetDC on RT[%d] failed (0x%08x)", i, (unsigned)hr);
                for (int j = 0; j <= i; j++) {
                    gdiCopySurfaces[j]->Release();
                    gdiCopySurfaces[j] = nullptr;
                }
                return false;
            }
            gdiCopySurfaces[i]->ReleaseDC(testDC);
        }

        // Create D3D11 GDI-compatible texture
        D3D11_TEXTURE2D_DESC gdiDesc = {};
        gdiDesc.Width = width;
        gdiDesc.Height = height;
        gdiDesc.MipLevels = 1;
        gdiDesc.ArraySize = 1;
        gdiDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        gdiDesc.SampleDesc.Count = 1;
        gdiDesc.Usage = D3D11_USAGE_DEFAULT;
        gdiDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        gdiDesc.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;

        HRESULT hr = d3d11Device->CreateTexture2D(&gdiDesc, nullptr, &gdiTexture);
        if (FAILED(hr)) {
            HookLogImportant("DX9: GDI interop: D3D11 CreateTexture2D failed (0x%08x)", (unsigned)hr);
            for (int i = 0; i < 2; i++) {
                gdiCopySurfaces[i]->Release();
                gdiCopySurfaces[i] = nullptr;
            }
            return false;
        }

        hr = gdiTexture->QueryInterface(__uuidof(IDXGISurface1), (void**)&gdiSurface);
        if (FAILED(hr)) {
            HookLogImportant("DX9: GDI interop: IDXGISurface1 QI failed (0x%08x)", (unsigned)hr);
            gdiTexture->Release();
            gdiTexture = nullptr;
            for (int i = 0; i < 2; i++) {
                gdiCopySurfaces[i]->Release();
                gdiCopySurfaces[i] = nullptr;
            }
            return false;
        }

        gdiWriteIdx = 0;
        gdiHasPrevFrame = false;
        HookLogImportant("DX9: GDI interop ready: %ux%u (double-buffered, stall-free)", width, height);
        return true;

}

void DX9Capture::CompleteGDIInteropCapture(IDirect3DSurface9* srcSurface,  int64_t frameTimestampQpc) {


        if (!srcSurface || !d3d11Context)
            return;

        // Get GDI DC from D3D9 render target (source - written in a previous frame)
        HDC srcDC = nullptr;
        HRESULT hr = srcSurface->GetDC(&srcDC);
        if (FAILED(hr) || !srcDC) {
            static bool logged = false;
            if (!logged) {
                HookLogImportant("DX9: GDI: GetDC(D3D9) failed 0x%08x", (unsigned)hr);
                logged = true;
            }
            return;
        }

        const int idx = AcquirePublishedTextureSlot();
        if (idx < 0) {
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            srcSurface->ReleaseDC(srcDC);
            return;
        }
        IDXGISurface1* dstSurface = gdiDirectSharedRing ? gdiSharedRingSurfaces[idx] : gdiSurface;
        if (!dstSurface) {
            srcSurface->ReleaseDC(srcDC);
            return;
        }

        // Get GDI DC from D3D11 texture (destination, discard previous)
        HDC dstDC = nullptr;
        hr = dstSurface->GetDC(TRUE, &dstDC);
        if (FAILED(hr) || !dstDC) {
            srcSurface->ReleaseDC(srcDC);  // Release on correct surface
            static bool logged = false;

            if (!logged) {
                HookLogImportant("DX9: GDI: GetDC(D3D11%s) failed 0x%08x", gdiDirectSharedRing ? " shared-ring" : "",
                                 (unsigned)hr);
                logged = true;
            }
            return;
        }

        // GPU-accelerated blit on WDDM 2.0+ (both surfaces are GPU-resident)
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        BitBlt(dstDC, 0, 0, width, height, srcDC, 0, 0, SRCCOPY);

        dstSurface->ReleaseDC(nullptr);
        srcSurface->ReleaseDC(srcDC);

        if (!gdiDirectSharedRing)
            d3d11Context->CopyResource(sharedTextures[idx], gdiTexture);

        SignalPublishedTextureFrame(idx, frameTimestampQpc);

        AdvanceWriteIndex();

}
