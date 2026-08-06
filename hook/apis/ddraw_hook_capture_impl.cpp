#include "ddraw_hook_internal.h"


bool DDrawCapture::UploadOverlaySurfaceToBackbuffer() {


        IDirect3DSurface9* backBuffer = nullptr;
        HRESULT hr = d3d9DeviceEx->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr) || !backBuffer) {
            static int backBufferFailLogCount = 0;
            if (backBufferFailLogCount < 4) {
                HookLog("DDraw: Failed to get helper backbuffer for overlay composite (hr=0x%08x)", hr);
                backBufferFailLogCount++;
            }
            return false;
        }

        hr = d3d9DeviceEx->UpdateSurface(d3d9UploadSurface, nullptr, backBuffer, nullptr);
        backBuffer->Release();

        if (FAILED(hr)) {
            static int updateSurfaceFailLogCount = 0;
            if (updateSurfaceFailLogCount < 4) {
                HookLog("DDraw: Failed to upload DD surface into helper backbuffer (hr=0x%08x)", hr);
                updateSurfaceFailLogCount++;
            }
            return false;
        }

        return true;

}

bool DDrawCapture::StretchOverlaySurfaceToBackbuffer(IDirect3DSurface9* surface) {


        if (!surface) {
            return false;
        }

        IDirect3DSurface9* backBuffer = nullptr;
        HRESULT hr = d3d9DeviceEx->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr) || !backBuffer) {
            static int backBufferFailLogCount = 0;
            if (backBufferFailLogCount < 4) {
                HookLog("DDraw: Failed to get helper backbuffer for fast overlay composite (hr=0x%08x)", hr);
                backBufferFailLogCount++;
            }
            return false;
        }

        hr = d3d9DeviceEx->StretchRect(surface, nullptr, backBuffer, nullptr, D3DTEXF_NONE);
        backBuffer->Release();

        if (FAILED(hr)) {
            static int stretchRectFailLogCount = 0;
            if (stretchRectFailLogCount < 4) {
                HookLog("DDraw: Failed to stretch DD surface into helper backbuffer (hr=0x%08x)", hr);
                stretchRectFailLogCount++;
            }
            return false;
        }

        return true;

}

bool DDrawCapture::CopyLockedSurfaceToUploadSurface(const DDSURFACEDESC2& desc) {


        if (!desc.lpSurface || desc.dwWidth != width || desc.dwHeight != height ||

            desc.ddpfPixelFormat.dwRGBBitCount != 32) {
            return false;
        }

        if (d3d9FastUploadSurface) {
            D3DLOCKED_RECT fastLockedRect = {};
            HRESULT fastHr = d3d9FastUploadSurface->LockRect(&fastLockedRect, nullptr, 0);
            if (SUCCEEDED(fastHr)) {
                const uint8_t* src = static_cast<const uint8_t*>(desc.lpSurface);
                uint8_t* dst = static_cast<uint8_t*>(fastLockedRect.pBits);
                const size_t rowBytes = static_cast<size_t>(width) * 4u;
                for (uint32_t y = 0; y < height; ++y) {
                    memcpy(dst, src, rowBytes);
                    src += desc.lPitch;
                    dst += fastLockedRect.Pitch;
                }

                d3d9FastUploadSurface->UnlockRect();
                if (StretchOverlaySurfaceToBackbuffer(d3d9FastUploadSurface)) {
                    return true;
                }
            }
        }

        D3DLOCKED_RECT lockedRect = {};
        HRESULT hr = d3d9UploadSurface->LockRect(&lockedRect, nullptr, 0);
        if (FAILED(hr)) {
            static int uploadLockFailLogCount = 0;
            if (uploadLockFailLogCount < 4) {
                HookLog("DDraw: Failed to lock D3D9 upload surface for overlay composite (hr=0x%08x)", hr);
                uploadLockFailLogCount++;
            }
            return false;
        }

        const uint8_t* src = static_cast<const uint8_t*>(desc.lpSurface);
        uint8_t* dst = static_cast<uint8_t*>(lockedRect.pBits);
        const size_t rowBytes = static_cast<size_t>(width) * 4u;
        for (uint32_t y = 0; y < height; ++y) {
            memcpy(dst, src, rowBytes);
            src += desc.lPitch;
            dst += lockedRect.Pitch;
        }

        d3d9UploadSurface->UnlockRect();
        return UploadOverlaySurfaceToBackbuffer();

}

bool DDrawCapture::CopySurfaceToOverlayBackbufferViaLock(IDirectDrawSurface7* surface) {


        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        HRESULT hr = surface->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR, nullptr);
        if (FAILED(hr) || !desc.lpSurface) {
            return false;
        }

        const bool copied = CopyLockedSurfaceToUploadSurface(desc);
        surface->Unlock(nullptr);
        return copied;

}

bool DDrawCapture::CopyPrimarySurfaceToOverlayBackbuffer(IDirectDrawSurface7* surface) {


        if (!surface || !d3d9DeviceEx || !d3d9UploadSurface || width == 0 || height == 0) {
            return false;
        }

        if (CopySurfaceToOverlayBackbufferViaLock(surface)) {
            return true;
        }

        HDC sourceDC = nullptr;
        HRESULT hr = surface->GetDC(&sourceDC);
        if (FAILED(hr) || !sourceDC) {
            static int sourceDcFailLogCount = 0;
            if (sourceDcFailLogCount < 4) {
                HookLog("DDraw: Failed to get source surface DC for overlay composite (hr=0x%08x)", hr);
                sourceDcFailLogCount++;
            }
            return false;
        }

        HDC uploadDC = nullptr;
        hr = d3d9UploadSurface->GetDC(&uploadDC);
        if (FAILED(hr) || !uploadDC) {
            surface->ReleaseDC(sourceDC);
            static int uploadDcFailLogCount = 0;
            if (uploadDcFailLogCount < 4) {
                HookLog("DDraw: Failed to get D3D9 upload DC for overlay composite (hr=0x%08x)", hr);
                uploadDcFailLogCount++;
            }
            return false;
        }

        BOOL bitBltOk =
            BitBlt(uploadDC, 0, 0, static_cast<int>(width), static_cast<int>(height), sourceDC, 0, 0, SRCCOPY);

        d3d9UploadSurface->ReleaseDC(uploadDC);
        surface->ReleaseDC(sourceDC);

        if (!bitBltOk) {
            static int bitBltFailLogCount = 0;
            if (bitBltFailLogCount < 4) {
                HookLog("DDraw: BitBlt into overlay upload surface failed (err=%lu)", GetLastError());
                bitBltFailLogCount++;
            }
            return false;
        }

        return UploadOverlaySurfaceToBackbuffer();

}

bool DDrawCapture::PresentOverlay() {


        if (!d3d9DeviceEx) {
            return false;
        }

        HWND presentWindowOverride = d3d9UsesFlipEx ? nullptr : targetHwnd;
        HRESULT hr = d3d9DeviceEx->PresentEx(nullptr, nullptr, presentWindowOverride, nullptr, 0);
        static uint32_t overlayPresentCount = 0;
        overlayPresentCount++;
        if (overlayPresentCount <= 8) {
            HookLogImportant("DDraw: Overlay helper PresentEx hr=0x%08X hwnd=%p size=%ux%u count=%u", (unsigned)hr,
                             targetHwnd, width, height, overlayPresentCount);
        }

        if (FAILED(hr) && hr != D3DERR_WASSTILLDRAWING) {
            HookLog("DDraw: Overlay helper present failed (hr=0x%08x)", hr);
        }

        return SUCCEEDED(hr);

}

bool DDrawCapture::CaptureFrameFromSurface(IDirectDrawSurface7* surface) {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (!surface) {
            return false;
        }

        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        HRESULT hr = surface->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR, nullptr);
        if (SUCCEEDED(hr) && desc.lpSurface && desc.ddpfPixelFormat.dwRGBBitCount == 32 && desc.dwWidth == width &&
            desc.dwHeight == height) {
            CaptureFrame(desc.lpSurface, desc.lPitch);
            surface->Unlock(nullptr);
            return true;
        }

        if (SUCCEEDED(hr)) {
            surface->Unlock(nullptr);
        }

        CaptureFrameViaGDI(surface);
        return true;

}
