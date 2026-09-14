#include "ddraw_hook_internal.h"

// Overlay composite for the DirectDraw/DX6/DX7 compatibility route.
//
// The overlay is drawn by the D3D9Ex helper device and then has to end up
// inside a DirectDraw surface, which means a CPU round trip: the surface's
// pixels go up into the helper back buffer, the overlay is blended over them,
// and the result comes back down. Only the overlay's own bounding rectangle
// makes that trip. Moving the whole frame instead cost about 66 MB per present
// at 4K and held Gothic II to roughly 17 presents per second.

namespace {

using ce::ddraw_present_policy::Rect;

uint32_t Unpack565ToBgra(uint16_t value) {
    const uint32_t r = ((value >> 11) & 0x1Fu);
    const uint32_t g = ((value >> 5) & 0x3Fu);
    const uint32_t b = (value & 0x1Fu);
    // Replicate the high bits into the low ones so full-scale inputs stay full scale.
    const uint32_t r8 = (r << 3) | (r >> 2);
    const uint32_t g8 = (g << 2) | (g >> 4);
    const uint32_t b8 = (b << 3) | (b >> 2);
    return 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
}

uint32_t Unpack555ToBgra(uint16_t value) {
    const uint32_t r = ((value >> 10) & 0x1Fu);
    const uint32_t g = ((value >> 5) & 0x1Fu);
    const uint32_t b = (value & 0x1Fu);
    const uint32_t r8 = (r << 3) | (r >> 2);
    const uint32_t g8 = (g << 3) | (g >> 2);
    const uint32_t b8 = (b << 3) | (b >> 2);
    return 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
}

uint16_t Pack565FromBgra(uint32_t color) {
    return static_cast<uint16_t>(((color >> 8) & 0xF800u) | ((color >> 5) & 0x07E0u) | ((color >> 3) & 0x001Fu));
}

uint16_t Pack555FromBgra(uint32_t color) {
    return static_cast<uint16_t>(((color >> 9) & 0x7C00u) | ((color >> 6) & 0x03E0u) | ((color >> 3) & 0x001Fu));
}

bool Is565(const DDPIXELFORMAT& format) {
    return format.dwGBitMask == 0x07E0u;
}

}  // namespace

bool DDrawCapture::EnsureCompositeRegionResources(const Rect& region) {
    if (!d3d9DeviceEx)
        return false;

    const uint32_t neededWidth = static_cast<uint32_t>(region.right - region.left);
    const uint32_t neededHeight = static_cast<uint32_t>(region.bottom - region.top);
    if (neededWidth == 0 || neededHeight == 0)
        return false;

    if (d3d9RegionTarget && d3d9RegionSysMem && regionWidth == neededWidth && regionHeight == neededHeight) {
        return true;
    }

    ReleaseCompositeRegionResources();

    HRESULT hr = d3d9DeviceEx->CreateRenderTarget(neededWidth, neededHeight, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0,
                                                  FALSE, &d3d9RegionTarget, nullptr);
    if (FAILED(hr) || !d3d9RegionTarget) {
        HookLog("DDraw: Failed to create %ux%u overlay composite render target (hr=0x%08x)", neededWidth, neededHeight,
                hr);
        ReleaseCompositeRegionResources();
        return false;
    }

    hr = d3d9DeviceEx->CreateOffscreenPlainSurface(neededWidth, neededHeight, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                   &d3d9RegionSysMem, nullptr);
    if (FAILED(hr) || !d3d9RegionSysMem) {
        HookLog("DDraw: Failed to create %ux%u overlay composite staging surface (hr=0x%08x)", neededWidth,
                neededHeight, hr);
        ReleaseCompositeRegionResources();
        return false;
    }

    // Optional: a default-pool staging surface uploads through StretchRect,
    // which avoids the system-memory copy UpdateSurface performs. Its absence
    // only costs speed, so a failure here is not fatal.
    hr = d3d9DeviceEx->CreateOffscreenPlainSurface(neededWidth, neededHeight, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                                   &d3d9RegionUpload, nullptr);
    if (FAILED(hr)) {
        d3d9RegionUpload = nullptr;
    }

    regionWidth = neededWidth;
    regionHeight = neededHeight;
    HookLogImportant("DDraw: Overlay composite region resized to %ux%u at (%d,%d)", regionWidth, regionHeight,
                     region.left, region.top);
    return true;
}

void DDrawCapture::ReleaseCompositeRegionResources() {
    if (d3d9RegionUpload) {
        d3d9RegionUpload->Release();
        d3d9RegionUpload = nullptr;
    }
    if (d3d9RegionSysMem) {
        d3d9RegionSysMem->Release();
        d3d9RegionSysMem = nullptr;
    }
    if (d3d9RegionTarget) {
        d3d9RegionTarget->Release();
        d3d9RegionTarget = nullptr;
    }
    regionWidth = 0;
    regionHeight = 0;
}

bool DDrawCapture::CopySurfaceRegionToOverlayBackbuffer(IDirectDrawSurface7* surface, const Rect& region) {
    if (!surface || !d3d9DeviceEx || !d3d9RegionSysMem)
        return false;

    const uint32_t regionW = static_cast<uint32_t>(region.right - region.left);
    const uint32_t regionH = static_cast<uint32_t>(region.bottom - region.top);

    IDirect3DSurface9* staging = d3d9RegionUpload ? d3d9RegionUpload : d3d9RegionSysMem;
    D3DLOCKED_RECT stagingLock = {};
    if (FAILED(staging->LockRect(&stagingLock, nullptr, 0)) || !stagingLock.pBits) {
        return false;
    }

    RECT sourceRect = {region.left, region.top, region.right, region.bottom};
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    bool copied = false;
    const HRESULT lockHr = surface->Lock(&sourceRect, &desc, DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR, nullptr);
    if (SUCCEEDED(lockHr) && desc.lpSurface) {
        const uint8_t* src = static_cast<const uint8_t*>(desc.lpSurface);
        uint8_t* dst = static_cast<uint8_t*>(stagingLock.pBits);
        const uint32_t bits = desc.ddpfPixelFormat.dwRGBBitCount;
        if (bits == 32) {
            const size_t rowBytes = static_cast<size_t>(regionW) * 4u;
            for (uint32_t y = 0; y < regionH; ++y) {
                memcpy(dst, src, rowBytes);
                src += desc.lPitch;
                dst += stagingLock.Pitch;
            }
            copied = true;
        } else if (bits == 16) {
            const bool is565 = Is565(desc.ddpfPixelFormat);
            for (uint32_t y = 0; y < regionH; ++y) {
                const uint16_t* src16 = reinterpret_cast<const uint16_t*>(src);
                uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst);
                for (uint32_t x = 0; x < regionW; ++x) {
                    dst32[x] = is565 ? Unpack565ToBgra(src16[x]) : Unpack555ToBgra(src16[x]);
                }
                src += desc.lPitch;
                dst += stagingLock.Pitch;
            }
            copied = true;
        }
        surface->Unlock(&sourceRect);
    }
    staging->UnlockRect();

    if (!copied) {
        // Palettized and packed-24 surfaces have no direct row form the helper
        // can consume; GDI converts them for the same region.
        HDC sourceDC = nullptr;
        if (FAILED(surface->GetDC(&sourceDC)) || !sourceDC) {
            static int sourceDcFailLogCount = 0;
            if (sourceDcFailLogCount < 4) {
                HookLog("DDraw: Overlay composite could not read the target surface (lock hr=0x%08x, no DC)", lockHr);
                sourceDcFailLogCount++;
            }
            return false;
        }
        HDC stagingDC = nullptr;
        if (FAILED(staging->GetDC(&stagingDC)) || !stagingDC) {
            surface->ReleaseDC(sourceDC);
            return false;
        }
        const BOOL blitOk = BitBlt(stagingDC, 0, 0, static_cast<int>(regionW), static_cast<int>(regionH), sourceDC,
                                   region.left, region.top, SRCCOPY);
        staging->ReleaseDC(stagingDC);
        surface->ReleaseDC(sourceDC);
        if (!blitOk) {
            static int bitBltFailLogCount = 0;
            if (bitBltFailLogCount < 4) {
                HookLog("DDraw: Overlay composite region BitBlt failed (err=%lu)", GetLastError());
                bitBltFailLogCount++;
            }
            return false;
        }
    }

    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(d3d9DeviceEx->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer) {
        static int backBufferFailLogCount = 0;
        if (backBufferFailLogCount < 4) {
            HookLog("DDraw: Overlay composite could not reach the helper backbuffer");
            backBufferFailLogCount++;
        }
        return false;
    }

    HRESULT uploadHr = E_FAIL;
    RECT destRect = {region.left, region.top, region.right, region.bottom};
    if (staging == d3d9RegionUpload) {
        uploadHr = d3d9DeviceEx->StretchRect(staging, nullptr, backBuffer, &destRect, D3DTEXF_NONE);
    }
    if (FAILED(uploadHr)) {
        POINT destPoint = {region.left, region.top};
        uploadHr = d3d9DeviceEx->UpdateSurface(d3d9RegionSysMem, nullptr, backBuffer, &destPoint);
    }
    backBuffer->Release();

    if (FAILED(uploadHr)) {
        static int uploadFailLogCount = 0;
        if (uploadFailLogCount < 4) {
            HookLog("DDraw: Overlay composite upload failed (hr=0x%08x)", uploadHr);
            uploadFailLogCount++;
        }
        return false;
    }
    return true;
}

bool DDrawCapture::CopyOverlayBackbufferRegionToSurface(IDirectDrawSurface7* surface, const Rect& region) {
    if (!surface || !d3d9DeviceEx || !d3d9RegionTarget || !d3d9RegionSysMem)
        return false;

    const uint32_t regionW = static_cast<uint32_t>(region.right - region.left);
    const uint32_t regionH = static_cast<uint32_t>(region.bottom - region.top);

    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(d3d9DeviceEx->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer) {
        return false;
    }

    RECT sourceRect = {region.left, region.top, region.right, region.bottom};
    HRESULT hr = d3d9DeviceEx->StretchRect(backBuffer, &sourceRect, d3d9RegionTarget, nullptr, D3DTEXF_NONE);
    backBuffer->Release();
    if (FAILED(hr)) {
        static int resolveFailLogCount = 0;
        if (resolveFailLogCount < 4) {
            HookLog("DDraw: Overlay composite readback resolve failed (hr=0x%08x)", hr);
            resolveFailLogCount++;
        }
        return false;
    }

    // GetRenderTargetData reads the whole render target, which is why the
    // render target is the region rather than the frame.
    hr = d3d9DeviceEx->GetRenderTargetData(d3d9RegionTarget, d3d9RegionSysMem);
    if (FAILED(hr)) {
        static int readbackFailLogCount = 0;
        if (readbackFailLogCount < 4) {
            HookLog("DDraw: Overlay composite readback failed (hr=0x%08x)", hr);
            readbackFailLogCount++;
        }
        return false;
    }

    D3DLOCKED_RECT readbackLock = {};
    if (FAILED(d3d9RegionSysMem->LockRect(&readbackLock, nullptr, D3DLOCK_READONLY)) || !readbackLock.pBits) {
        return false;
    }

    RECT destRect = {region.left, region.top, region.right, region.bottom};
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    bool written = false;
    const HRESULT lockHr = surface->Lock(&destRect, &desc, DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR, nullptr);
    if (SUCCEEDED(lockHr) && desc.lpSurface) {
        const uint8_t* src = static_cast<const uint8_t*>(readbackLock.pBits);
        uint8_t* dst = static_cast<uint8_t*>(desc.lpSurface);
        const uint32_t bits = desc.ddpfPixelFormat.dwRGBBitCount;
        if (bits == 32) {
            const size_t rowBytes = static_cast<size_t>(regionW) * 4u;
            for (uint32_t y = 0; y < regionH; ++y) {
                memcpy(dst, src, rowBytes);
                src += readbackLock.Pitch;
                dst += desc.lPitch;
            }
            written = true;
        } else if (bits == 16) {
            const bool is565 = Is565(desc.ddpfPixelFormat);
            for (uint32_t y = 0; y < regionH; ++y) {
                const uint32_t* src32 = reinterpret_cast<const uint32_t*>(src);
                uint16_t* dst16 = reinterpret_cast<uint16_t*>(dst);
                for (uint32_t x = 0; x < regionW; ++x) {
                    dst16[x] = is565 ? Pack565FromBgra(src32[x]) : Pack555FromBgra(src32[x]);
                }
                src += readbackLock.Pitch;
                dst += desc.lPitch;
            }
            written = true;
        }
        surface->Unlock(&destRect);
    }
    d3d9RegionSysMem->UnlockRect();

    if (!written) {
        HDC readbackDC = nullptr;
        if (FAILED(d3d9RegionSysMem->GetDC(&readbackDC)) || !readbackDC) {
            return false;
        }
        HDC targetDC = nullptr;
        if (FAILED(surface->GetDC(&targetDC)) || !targetDC) {
            d3d9RegionSysMem->ReleaseDC(readbackDC);
            return false;
        }
        const BOOL blitOk = BitBlt(targetDC, region.left, region.top, static_cast<int>(regionW),
                                   static_cast<int>(regionH), readbackDC, 0, 0, SRCCOPY);
        surface->ReleaseDC(targetDC);
        d3d9RegionSysMem->ReleaseDC(readbackDC);
        if (!blitOk) {
            static int writebackFailLogCount = 0;
            if (writebackFailLogCount < 4) {
                HookLog("DDraw: Overlay composite writeback failed (lock hr=0x%08x, err=%lu)", lockHr, GetLastError());
                writebackFailLogCount++;
            }
            return false;
        }
    }

    static uint32_t compositeCount = 0;
    compositeCount++;
    if (compositeCount <= 4 || (compositeCount % 600 == 0)) {
        HookLogImportant("DDraw: Overlay composited into the presented surface (hwnd=%p region=%ux%u at %d,%d "
                         "frame=%ux%u count=%u)",
                         targetHwnd, regionW, regionH, region.left, region.top, width, height, compositeCount);
    }
    return true;
}

bool DDrawCapture::PresentOverlay() {
    if (!d3d9DeviceEx) {
        return false;
    }

    HWND presentWindowOverride = d3d9UsesFlipEx ? nullptr : targetHwnd;
    HRESULT hr = S_OK;
    {
        DX9InternalBypassScope dx9Bypass;
        hr = d3d9DeviceEx->PresentEx(nullptr, nullptr, presentWindowOverride, nullptr, 0);
    }
    static uint32_t overlayPresentCount = 0;
    overlayPresentCount++;
    if (overlayPresentCount <= 4 || (overlayPresentCount % 600 == 0)) {
        HookLogImportant("DDraw: Overlay helper PresentEx fallback hr=0x%08X hwnd=%p size=%ux%u count=%u",
                         static_cast<unsigned>(hr), targetHwnd, width, height, overlayPresentCount);
    }

    if (FAILED(hr) && hr != D3DERR_WASSTILLDRAWING) {
        HookLog("DDraw: Overlay helper present failed (hr=0x%08x)", hr);
    }

    return SUCCEEDED(hr);
}
