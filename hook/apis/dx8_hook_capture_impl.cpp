#include "dx8_hook_internal.h"



bool DX8Capture::EnsureSnapshotSurface(IDirect3DDevice8* device) {


        if (!device || width == 0 || height == 0) {
            return false;
        }

        if (d3d8SnapshotSurface) {
            return true;
        }

        IDirect3DSurface8* backBuffer = nullptr;
        HRESULT hr = GetD3D8GetBackBuffer(device)(device, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr) || !backBuffer) {
            static int getBackBufferFailLogCount = 0;
            if (getBackBufferFailLogCount < 4) {
                HookLog("DX8: Failed to get backbuffer for overlay composite (hr=0x%08x)", hr);
                getBackBufferFailLogCount++;
            }
            return false;
        }

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3D8_SURFACE_DESC_LOCAL dx8_hook_desc = {};
        hr = D3D8SurfaceGetDesc(backBuffer, &dx8_hook_desc);
        ReleaseD3D8Surface(backBuffer);
        if (FAILED(hr)) {
            static int getDescFailLogCount = 0;
            if (getDescFailLogCount < 4) {
                HookLog("DX8: Failed to query backbuffer desc for overlay composite (hr=0x%08x)", hr);
                getDescFailLogCount++;
            }
            return false;
        }

        if (dx8_hook_desc.Width != width || dx8_hook_desc.Height != height) {
            static int sizeMismatchLogCount = 0;
            if (sizeMismatchLogCount < 4) {
                HookLog("DX8: Backbuffer/helper size mismatch for overlay composite (%ux%u vs %ux%u)", dx8_hook_desc.Width,
                        dx8_hook_desc.Height, width, height);
                sizeMismatchLogCount++;
            }
            return false;
        }

        hr = GetD3D8CreateImageSurface(device)(device, width, height, dx8_hook_desc.Format, &d3d8SnapshotSurface);
        if (FAILED(hr) || !d3d8SnapshotSurface) {
            static int createSnapshotFailLogCount = 0;
            if (createSnapshotFailLogCount < 4) {
                HookLog("DX8: Failed to create snapshot surface for overlay composite (hr=0x%08x)", hr);
                createSnapshotFailLogCount++;
            }
            return false;
        }

        d3d8SnapshotFormat = dx8_hook_desc.Format;
        return true;

}

bool DX8Capture::EnsureFrontBufferSurface(IDirect3DDevice8* device) {


        if (!device || width == 0 || height == 0) {
            return false;
        }

        if (d3d8FrontBufferSurface) {
            return true;
        }

        HRESULT hr = GetD3D8CreateImageSurface(device)(device, width, height, D3DFMT_A8R8G8B8, &d3d8FrontBufferSurface);
        if (FAILED(hr) || !d3d8FrontBufferSurface) {
            static int createFrontBufferFailLogCount = 0;
            if (createFrontBufferFailLogCount < 4) {
                HookLog("DX8: Failed to create front-buffer surface for overlay composite (hr=0x%08x)", hr);
                createFrontBufferFailLogCount++;
            }
            return false;
        }

        return true;

}

bool DX8Capture::CopyLockedPixelsToSurface9(const D3DLOCKED_RECT& sourceLockedRect,  D3DFORMAT sourceFormat, 
                                    IDirect3DSurface9* destinationSurface) {


        if (!d3d9DeviceEx || !d3d9UploadSurface || !destinationSurface || width == 0 || height == 0) {
            return false;
        }

        D3DLOCKED_RECT uploadLockedRect = {};
        HRESULT hr = d3d9UploadSurface->LockRect(&uploadLockedRect, nullptr, 0);
        if (FAILED(hr)) {
            static int uploadLockFailLogCount = 0;
            if (uploadLockFailLogCount < 4) {
                HookLog("DX8: Failed to lock helper upload surface (hr=0x%08x)", hr);
                uploadLockFailLogCount++;
            }
            return false;
        }

        const uint8_t* srcBase = static_cast<const uint8_t*>(sourceLockedRect.pBits);
        uint8_t* dstBase = static_cast<uint8_t*>(uploadLockedRect.pBits);
        bool copied = true;

        for (uint32_t y = 0; y < height && copied; ++y) {
            const uint8_t* srcRow = srcBase + static_cast<size_t>(y) * static_cast<size_t>(sourceLockedRect.Pitch);
            uint32_t* dstRow = reinterpret_cast<uint32_t*>(dstBase + static_cast<size_t>(y) * uploadLockedRect.Pitch);

            switch (sourceFormat) {
                case D3DFMT_A8R8G8B8: {
                    memcpy(dstRow, srcRow, static_cast<size_t>(width) * sizeof(uint32_t));
                    break;
                }
                case D3DFMT_X8R8G8B8: {
                    const uint32_t* srcPixels = reinterpret_cast<const uint32_t*>(srcRow);
                    for (uint32_t x = 0; x < width; ++x) {
                        dstRow[x] = srcPixels[x] | 0xFF000000u;
                    }
                    break;
                }
                case D3DFMT_A8B8G8R8: {
                    const uint32_t* srcPixels = reinterpret_cast<const uint32_t*>(srcRow);
                    for (uint32_t x = 0; x < width; ++x) {
                        const uint32_t pixel = srcPixels[x];
                        const uint8_t red = static_cast<uint8_t>(pixel & 0xFFu);
                        const uint8_t green = static_cast<uint8_t>((pixel >> 8) & 0xFFu);
                        const uint8_t blue = static_cast<uint8_t>((pixel >> 16) & 0xFFu);
                        const uint8_t alpha = static_cast<uint8_t>((pixel >> 24) & 0xFFu);
                        dstRow[x] = PackBgra8(blue, green, red, alpha);
                    }
                    break;
                }
                case D3DFMT_R5G6B5: {
                    const uint16_t* srcPixels = reinterpret_cast<const uint16_t*>(srcRow);
                    for (uint32_t x = 0; x < width; ++x) {
                        const uint16_t pixel = srcPixels[x];
                        const uint8_t blue = Expand5To8(pixel & 0x1Fu);
                        const uint8_t green = Expand6To8((pixel >> 5) & 0x3Fu);
                        const uint8_t red = Expand5To8((pixel >> 11) & 0x1Fu);
                        dstRow[x] = PackBgra8(blue, green, red, 0xFF);
                    }
                    break;
                }
                case D3DFMT_X1R5G5B5:
                case D3DFMT_A1R5G5B5: {
                    const uint16_t* srcPixels = reinterpret_cast<const uint16_t*>(srcRow);
                    const bool preserveAlpha = sourceFormat == D3DFMT_A1R5G5B5;
                    for (uint32_t x = 0; x < width; ++x) {
                        const uint16_t pixel = srcPixels[x];
                        const uint8_t blue = Expand5To8(pixel & 0x1Fu);
                        const uint8_t green = Expand5To8((pixel >> 5) & 0x1Fu);
                        const uint8_t red = Expand5To8((pixel >> 10) & 0x1Fu);
                        const uint8_t alpha = preserveAlpha ? ((pixel & 0x8000u) ? 0xFF : 0x00) : 0xFF;
                        dstRow[x] = PackBgra8(blue, green, red, alpha);
                    }
                    break;
                }
                case D3DFMT_X4R4G4B4:
                case D3DFMT_A4R4G4B4: {
                    const uint16_t* srcPixels = reinterpret_cast<const uint16_t*>(srcRow);
                    const bool preserveAlpha = sourceFormat == D3DFMT_A4R4G4B4;
                    for (uint32_t x = 0; x < width; ++x) {
                        const uint16_t pixel = srcPixels[x];
                        const uint8_t blue = Expand4To8(pixel & 0xFu);
                        const uint8_t green = Expand4To8((pixel >> 4) & 0xFu);
                        const uint8_t red = Expand4To8((pixel >> 8) & 0xFu);
                        const uint8_t alpha = preserveAlpha ? Expand4To8((pixel >> 12) & 0xFu) : 0xFF;
                        dstRow[x] = PackBgra8(blue, green, red, alpha);
                    }
                    break;
                }
                default: {
                    static int unsupportedFormatLogCount = 0;
                    if (unsupportedFormatLogCount < 4) {
                        HookLog("DX8: Unsupported surface format for helper composite (fmt=%u)",
                                static_cast<unsigned>(sourceFormat));
                        unsupportedFormatLogCount++;
                    }
                    copied = false;
                    break;
                }
            }
        }

        d3d9UploadSurface->UnlockRect();
        if (!copied) {
            return false;
        }

        hr = d3d9DeviceEx->UpdateSurface(d3d9UploadSurface, nullptr, destinationSurface, nullptr);
        if (FAILED(hr)) {
            static int updateSurfaceFailLogCount = 0;
            if (updateSurfaceFailLogCount < 4) {
                HookLog("DX8: Failed to update helper surface from DX8 snapshot (hr=0x%08x)", hr);
                updateSurfaceFailLogCount++;
            }
            return false;
        }

        return true;

}

bool DX8Capture::CopyLockedPixelsToOverlayBackbuffer(const D3DLOCKED_RECT& sourceLockedRect,  D3DFORMAT sourceFormat) {


        IDirect3DSurface9* backBuffer = nullptr;
        HRESULT hr = d3d9DeviceEx->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr) || !backBuffer) {
            static int getBackBufferFailLogCount = 0;
            if (getBackBufferFailLogCount < 4) {
                HookLog("DX8: Failed to get helper backbuffer for overlay composite (hr=0x%08x)", hr);
                getBackBufferFailLogCount++;
            }
            return false;
        }

        const bool copied = CopyLockedPixelsToSurface9(sourceLockedRect, sourceFormat, backBuffer);
        backBuffer->Release();
        return copied;

}

bool DX8Capture::CopySurfaceToSurface9(IDirect3DSurface8* sourceSurface,  D3DFORMAT sourceFormat, 
                               IDirect3DSurface9* destinationSurface) {


        if (!sourceSurface || !destinationSurface) {
            return false;
        }

        D3DLOCKED_RECT lockedRect = {};
        HRESULT hr = D3D8SurfaceLockRect(sourceSurface, &lockedRect, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr)) {
            static int lockFailLogCount = 0;
            if (lockFailLogCount < 4) {
                HookLog("DX8: Failed to lock DX8 snapshot surface for overlay composite (hr=0x%08x)", hr);
                lockFailLogCount++;
            }
            return false;
        }

        const bool copied = CopyLockedPixelsToSurface9(lockedRect, sourceFormat, destinationSurface);
        D3D8SurfaceUnlockRect(sourceSurface);
        return copied;

}

bool DX8Capture::CopyBackBufferToSurface9(IDirect3DDevice8* device,  IDirect3DSurface9* destinationSurface) {


        if (!device || !destinationSurface || !EnsureSnapshotSurface(device)) {
            return false;
        }

        IDirect3DSurface8* backBuffer = nullptr;
        HRESULT hr = GetD3D8GetBackBuffer(device)(device, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr) || !backBuffer) {
            static int getBackBufferFailLogCount = 0;
            if (getBackBufferFailLogCount < 4) {
                HookLog("DX8: Failed to reacquire backbuffer for overlay composite (hr=0x%08x)", hr);
                getBackBufferFailLogCount++;
            }
            return false;
        }

        hr = GetD3D8CopyRects(device)(device, backBuffer, nullptr, 0, d3d8SnapshotSurface, nullptr);
        ReleaseD3D8Surface(backBuffer);
        if (FAILED(hr)) {
            static int copyRectsFailLogCount = 0;
            if (copyRectsFailLogCount < 4) {
                HookLog("DX8: CopyRects snapshot for overlay composite failed (hr=0x%08x)", hr);
                copyRectsFailLogCount++;
            }
            return false;
        }

        return CopySurfaceToSurface9(d3d8SnapshotSurface, d3d8SnapshotFormat, destinationSurface);

}

bool DX8Capture::CopyFrontBufferToSurface9(IDirect3DDevice8* device,  IDirect3DSurface9* destinationSurface) {


        if (!device || !destinationSurface || !EnsureFrontBufferSurface(device)) {
            return false;
        }

        HRESULT hr = GetD3D8GetFrontBuffer(device)(device, d3d8FrontBufferSurface);
        if (FAILED(hr)) {
            static int getFrontBufferFailLogCount = 0;
            if (getFrontBufferFailLogCount < 4) {
                HookLog("DX8: GetFrontBuffer fallback for helper composite failed (hr=0x%08x)", hr);
                getFrontBufferFailLogCount++;
            }
            return false;
        }

        D3DLOCKED_RECT lockedRect = {};
        hr = D3D8SurfaceLockRect(d3d8FrontBufferSurface, &lockedRect, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr)) {
            static int frontLockFailLogCount = 0;
            if (frontLockFailLogCount < 4) {
                HookLog("DX8: Failed to lock front-buffer fallback surface (hr=0x%08x)", hr);
                frontLockFailLogCount++;
            }
            return false;
        }

        const bool copied = CopyLockedPixelsToSurface9(lockedRect, D3DFMT_A8R8G8B8, destinationSurface);
        D3D8SurfaceUnlockRect(d3d8FrontBufferSurface);
        return copied;

}

bool DX8Capture::CopyFrontBufferToOverlayBackbuffer(IDirect3DDevice8* device) {


        IDirect3DSurface9* backBuffer = nullptr;
        HRESULT hr = d3d9DeviceEx->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr) || !backBuffer) {
            static int getBackBufferFailLogCount = 0;
            if (getBackBufferFailLogCount < 4) {
                HookLog("DX8: Failed to get helper backbuffer for front-buffer fallback (hr=0x%08x)", hr);
                getBackBufferFailLogCount++;
            }
            return false;
        }

        const bool copied = CopyFrontBufferToSurface9(device, backBuffer);
        backBuffer->Release();
        return copied;

}
