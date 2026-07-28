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

    bool CopySurfaceToOverlayBackbufferViaLock(IDirectDrawSurface7* surface) {
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

    bool CopyPrimarySurfaceToOverlayBackbuffer(IDirectDrawSurface7* surface) {
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

    bool EnsureOverlayDevice(HWND hwnd, uint32_t w, uint32_t h) {
        if (!hwnd || w == 0 || h == 0) {
            static int invalidOverlayStateLogCount = 0;
            if (invalidOverlayStateLogCount < 3) {
                HookLog("DDraw: EnsureOverlayDevice skipped (hwnd=%p, size=%ux%u)", hwnd, w, h);
                invalidOverlayStateLogCount++;
            }
            return false;
        }

        const bool hwndChanged = targetHwnd && hwnd != targetHwnd;
        const bool sizeChanged = width != w || height != h;
        if (initialized && (hwndChanged || sizeChanged)) {
            // Capture owns the generation-wide dimensions. Let
            // EnsureCaptureResources drain/rebuild it before mutating them.
            return false;
        }
        if ((hwndChanged || sizeChanged) && d3d9DeviceEx) {
            HookLog("DDraw: Recreating overlay helper (oldHwnd=%p newHwnd=%p old=%ux%u new=%ux%u)", targetHwnd, hwnd,
                    width, height, w, h);
            ReleaseOverlayResources();
        }

        targetHwnd = hwnd;
        width = w;
        height = h;
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (d3d9DeviceEx) {
            return true;
        }

        if (!CreateD3D9ExWrapper(hwnd)) {
            HookLog("DDraw: Overlay disabled (D3D9Ex wrapper failed)");
            return false;
        }

        HookLog("DDraw: Overlay helper ready (hwnd=%p, size=%ux%u)", hwnd, width, height);
        return true;
    }

    bool EnsureCaptureResources(IDirectDrawSurface7* surface, HWND hwnd, uint32_t w, uint32_t h) {
        if (!surface || w == 0 || h == 0) {
            HookLog("DDraw: EnsureCaptureResources skipped (surface=%p, size=%ux%u)", surface, w, h);
            return false;
        }

        if (initialized && ddrawSurface == surface && width == w && height == h) {
            if (hwnd) {
                targetHwnd = hwnd;
            }
            return true;
        }

        if (initialized) {
            HookLog(
                "DDraw: Reinitializing capture resources for new surface/size (oldSurface=%p newSurface=%p old=%ux%u "
                "new=%ux%u)",
                ddrawSurface, surface, width, height, w, h);
            if (!CleanupDDraw(false))
                return false;
        }

        ddrawSurface = surface;
        targetHwnd = hwnd;
        width = w;
        height = h;
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (!CreateD3D11Device()) {
            CleanupDDraw(false);
            return false;
        }

        if (!CreateStagingTexture()) {
            CleanupDDraw(false);
            return false;
        }

        if (!CreateSharedTextures()) {
            CleanupDDraw(false);
            return false;
        }

        EnsureOverlayDevice(hwnd, w, h);

        if (g_IPC) {
            PublishToSharedMemory(g_IPC);
        }

        initialized = true;
        HookLog("DDraw Capture Initialized: %dx%d", width, height);
        return true;
    }

    bool PresentOverlay() {
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

    bool CaptureFrameFromSurface(IDirectDrawSurface7* surface) {
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

    void Init(IDirectDrawSurface7* surface, HWND hwnd, uint32_t w, uint32_t h) {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        EnsureCaptureResources(surface, hwnd, w, h);
    }

    void CaptureFrame(void* bits, int pitch) {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (!initialized || !bits)
            return;

        // Check if we should throttle capture (encoder is falling behind)
        if (g_IPC && g_IPC->GetSharedMem()) {
            if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
                return;
            }
        }

        SharedMemoryLayout* captureSharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        const int idx = FindAvailableCaptureTextureSlot(captureSharedMem, writeIndex.load(std::memory_order_relaxed));
        if (idx < 0) {
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        writeIndex.store(idx, std::memory_order_relaxed);

        // Get timestamp
        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;

        // Map staging texture and copy from DDraw surface
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = d3d11Context->Map(stagingTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr) || !mapped.pData)
            return;

        // Copy row by row (handle different pitches)
        uint8_t* src = (uint8_t*)bits;
        uint8_t* dst = (uint8_t*)mapped.pData;
        int rowSize = width * 4;  // Assuming 32-bit color

        for (uint32_t y = 0; y < height; y++) {
            memcpy(dst, src, rowSize);
            src += pitch;
            dst += mapped.RowPitch;
        }

        d3d11Context->Unmap(stagingTexture, 0);

        // Copy staging to shared texture
        d3d11Context->CopyResource(sharedTextures[idx], stagingTexture);

        // Signal fence if available
        uint64_t publishedFenceValue = 0;
        if (useFences && context4 && fence) {
            const uint64_t candidateFenceValue = ++fenceValue;
            const HRESULT signalHr = context4->Signal(fence, candidateFenceValue);
            if (SUCCEEDED(signalHr)) {
                publishedFenceValue = candidateFenceValue;
            } else {
                HookLog("DDraw: Capture fence Signal failed value=%llu hr=0x%08X; using implicit sync later",
                        static_cast<unsigned long long>(candidateFenceValue), signalHr);
                useFences = false;
            }
        }
        if (publishedFenceValue == 0)
            d3d11Context->Flush();

        // PASS RAW QPC
        SignalFrameReady(g_IPC, idx, qpc.QuadPart, publishedFenceValue);
        AdvanceWriteIndex();
    }

    // Capture via GetDC for surfaces that don't support Lock
    void CaptureFrameViaGDI(IDirectDrawSurface7* surface) {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (!initialized)
            return;

        HDC hdc = NULL;
        if (FAILED(surface->GetDC(&hdc)) || !hdc)
            return;

        // Create compatible DC and bitmap
        HDC memDC = CreateCompatibleDC(hdc);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -(int)height;  // Top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP hbm = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        HGDIOBJ oldBm = SelectObject(memDC, hbm);

        // BitBlt from surface DC
        BitBlt(memDC, 0, 0, width, height, hdc, 0, 0, SRCCOPY);

        // Capture the bits
        CaptureFrame(bits, width * 4);

        // Cleanup
        SelectObject(memDC, oldBm);
        DeleteObject(hbm);
        DeleteDC(memDC);

        surface->ReleaseDC(hdc);
    }
};

static DDrawCapture g_DDrawCapture;

// Draw overlay using D3D9Ex
static void DrawDDrawOverlay(IDirectDrawSurface7* overlaySourceSurface) {
    if (!g_DDrawCapture.d3d9DeviceEx)
        return;

    if (g_DDrawCapture.targetHwnd && g_DDrawCapture.targetHwnd != g_CachedHwnd) {
        g_CachedHwnd = g_DDrawCapture.targetHwnd;
        InputManager::Get().HookWindow(g_CachedHwnd);
    }

    if (g_CachedHwnd) {
        g_OverlayAdapter.SetHwnd(g_CachedHwnd);
    }

    if (!g_OverlayAdapter.IsInitialized()) {
        g_CachedHwnd = g_DDrawCapture.targetHwnd;
        if (g_CachedHwnd) {
            InputManager::Get().HookWindow(g_CachedHwnd);
            g_OverlayAdapter.SetHwnd(g_CachedHwnd);
        }
        if (g_OverlayAdapter.InitDX9(g_DDrawCapture.d3d9DeviceEx)) {
            if (g_CachedHwnd) {
                g_OverlayAdapter.SetHwnd(g_CachedHwnd);
            }
            HookLog("DDraw: OverlayAdapter initialized");
        }
    }

    g_OverlayAdapter.SetMetrics(&g_PerfMetrics);
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(g_DDrawCapture.droppedFrames.load(std::memory_order_relaxed));
    const auto directDrawVersion = static_cast<ce::graphics_api_identity::DirectDrawVersion>(
        g_ActiveDirectDrawVersion.load(std::memory_order_acquire));
    const unsigned d3dVersion = g_ActiveLegacyD3DVersion.load(std::memory_order_acquire);
    g_OverlayAdapter.SetGraphicsAPI(ce::graphics_api_identity::LegacyDirectXLabel(directDrawVersion, d3dVersion),
                                    "active DirectDraw presentation surface");

    if (g_OverlayAdapter.IsInitialized() && g_DDrawCapture.width > 0 && g_DDrawCapture.height > 0) {
        g_DDrawCapture.CopyPrimarySurfaceToOverlayBackbuffer(overlaySourceSurface);
        g_OverlayAdapter.RenderOverlay(g_DDrawCapture.width, g_DDrawCapture.height);
        static uint32_t overlayRenderSubmitCount = 0;
        overlayRenderSubmitCount++;
        if (overlayRenderSubmitCount <= 8) {
            HookLogImportant("DDraw: Overlay render submitted (hwnd=%p, size=%ux%u count=%u)",
                             g_DDrawCapture.targetHwnd, g_DDrawCapture.width, g_DDrawCapture.height,
                             overlayRenderSubmitCount);
        }
        g_DDrawCapture.PresentOverlay();
    }
}

static void InstallAttachedBackBufferHooks(IDirectDrawSurface7* primarySurface, const char* reason) {
    if (!primarySurface) {
        return;
    }

    DDSCAPS2 backBufferCaps = {};
    backBufferCaps.dwCaps = DDSCAPS_BACKBUFFER;
    IDirectDrawSurface7* backBuffer = nullptr;
    if (SUCCEEDED(primarySurface->GetAttachedSurface(&backBufferCaps, &backBuffer)) && backBuffer) {
        InstallSurfaceHooksForSurface(backBuffer, reason);
        backBuffer->Release();
    }
}

// Get surface dimensions from DDSURFACEDESC2
static bool GetSurfaceSize(IDirectDrawSurface7* surface, uint32_t& w, uint32_t& h) {
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);

    if (surface && SUCCEEDED(surface->GetSurfaceDesc(&desc))) {
        w = desc.dwWidth;
        h = desc.dwHeight;
        return true;
    }
    return false;
}

static void InstallSurfaceHooksForLegacySurface(IDirectDrawSurface* surface, const char* reason) {
    if (!surface)
        return;
    void** vtable = *(void***)surface;
    if (!vtable)
        return;

    std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
    if (g_LegacySurfaceVTables.find(vtable) != g_LegacySurfaceVTables.end())
        return;

    LegacySurfaceVTableRecord record;
    const VTableHook::Status flipStatus =
        VTableHook::Create(&vtable[DDSURFACE7_VTABLE_FLIP], (LPVOID)&DetourDDSurfaceLegacyFlip, (LPVOID*)&record.flip);
    const VTableHook::Status bltStatus =
        VTableHook::Create(&vtable[DDSURFACE7_VTABLE_BLT], (LPVOID)&DetourDDSurfaceLegacyBlt, (LPVOID*)&record.blt);
    const VTableHook::Status lockStatus =
        VTableHook::Create(&vtable[DDSURFACE7_VTABLE_LOCK], (LPVOID)&DetourDDSurfaceLegacyLock, (LPVOID*)&record.lock);
    const VTableHook::Status unlockStatus = VTableHook::Create(
        &vtable[DDSURFACE7_VTABLE_UNLOCK], (LPVOID)&DetourDDSurfaceLegacyUnlock, (LPVOID*)&record.unlock);
    g_LegacySurfaceVTables.emplace(vtable, record);
    if (flipStatus == VTableHook::Success && bltStatus == VTableHook::Success && lockStatus == VTableHook::Success &&
        unlockStatus == VTableHook::Success && record.flip && record.blt && record.lock && record.unlock) {
        HookLog("DDraw: Legacy surface hooks installed via %s (surface=%p, vtable=%p)", reason, surface, vtable);
    } else {
        HookLogImportant("DDraw: Legacy surface hook installation incomplete via %s (flip=%s blt=%s lock=%s unlock=%s)",
                         reason, VTableHook::StatusToString(flipStatus), VTableHook::StatusToString(bltStatus),
                         VTableHook::StatusToString(lockStatus), VTableHook::StatusToString(unlockStatus));
    }
}

static void InstallLegacyDirectDrawHooksForInstance(IDirectDraw* ddraw,
                                                    ce::graphics_api_identity::DirectDrawVersion version,
                                                    const char* reason) {
    if (!ddraw)
        return;
    void** vtable = *(void***)ddraw;
    if (!vtable)
        return;

    std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
    if (g_LegacyDDrawVTables.find(vtable) != g_LegacyDDrawVTables.end())
        return;

    DDrawLegacyCreateSurface_t original = nullptr;
    const VTableHook::Status status =
        VTableHook::Create(&vtable[6], (LPVOID)&DetourDirectDrawLegacyCreateSurface, (LPVOID*)&original);
    if (status == VTableHook::Success && original) {
        g_LegacyDDrawVTables.emplace(vtable, LegacyDDrawVTableRecord{original, version});
        HookLog("DDraw: %s CreateSurface identity hook installed via %s (object=%p, vtable=%p)",
                ce::graphics_api_identity::DirectDrawLabel(version), reason, ddraw, vtable);
    } else {
        HookLogImportant("DDraw: %s CreateSurface identity hook failed via %s (%s)",
                         ce::graphics_api_identity::DirectDrawLabel(version), reason,
                         VTableHook::StatusToString(status));
    }
}

static void InstallSurfaceHooksForSurface4(IDirectDrawSurface4* surface, const char* reason, bool markPrototype) {
    if (!surface)
        return;

    void** surfaceVTable = *(void***)surface;
    if (!surfaceVTable) {
        HookLog("DDraw: InstallSurfaceHooksForSurface4 skipped for %s - null vtable (surface=%p)", reason, surface);
        return;
    }

    if (HasHookedVTable(g_HookedSurfaceVTables, surfaceVTable)) {
        HookLog("DDraw: InstallSurfaceHooksForSurface4 skipped for %s - vtable already hooked (surface=%p, vtable=%p)",
                reason, surface, surfaceVTable);
        return;
    }

    g_HookedSurfaceVTables.push_back(surfaceVTable);
    if (markPrototype && !g_HookSurfacePrototype4)
        g_HookSurfacePrototype4 = surface;

    HookLog("DDraw: Installing surface4 hooks via %s (surface=%p, vtable=%p, prototype=%d)", reason, surface,
            surfaceVTable, markPrototype ? 1 : 0);

    VTableHook::Status flipStatus =
        VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_FLIP], (LPVOID)&DetourDDSurface4Flip,
                           oDDSurface4Flip ? nullptr : (LPVOID*)&oDDSurface4Flip);
    if (flipStatus == VTableHook::Success) {
        HookLog("DDraw: Flip4 hook installed via %s", reason);
    } else {
        HookLog("DDraw: Flip4 hook install via %s returned %s", reason, VTableHook::StatusToString(flipStatus));
    }

    VTableHook::Status bltStatus =
        VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_BLT], (LPVOID)&DetourDDSurface4Blt,
                           oDDSurface4Blt ? nullptr : (LPVOID*)&oDDSurface4Blt);
    if (bltStatus == VTableHook::Success) {
        HookLog("DDraw: Blt4 hook installed via %s", reason);
    } else {
        HookLog("DDraw: Blt4 hook install via %s returned %s", reason, VTableHook::StatusToString(bltStatus));
    }

    VTableHook::Status lockStatus =
        VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_LOCK], (LPVOID)&DetourDDSurface4Lock,
                           oDDSurface4Lock ? nullptr : (LPVOID*)&oDDSurface4Lock);
    if (lockStatus == VTableHook::Success) {
        HookLog("DDraw: Lock4 hook installed via %s", reason);
    } else {
        HookLog("DDraw: Lock4 hook install via %s returned %s", reason, VTableHook::StatusToString(lockStatus));
    }

    VTableHook::Status unlockStatus =
        VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_UNLOCK], (LPVOID)&DetourDDSurface4Unlock,
                           oDDSurface4Unlock ? nullptr : (LPVOID*)&oDDSurface4Unlock);
    if (unlockStatus == VTableHook::Success) {
        HookLog("DDraw: Unlock4 hook installed via %s", reason);
    } else {
        HookLog("DDraw: Unlock4 hook install via %s returned %s", reason, VTableHook::StatusToString(unlockStatus));
    }
}

static void InstallSurfaceHooksForSurface(IDirectDrawSurface7* surface, const char* reason, bool markPrototype) {
    if (!surface)
        return;

    void** surfaceVTable = *(void***)surface;
    if (!surfaceVTable) {
        HookLog("DDraw: InstallSurfaceHooksForSurface skipped for %s - null vtable (surface=%p)", reason, surface);
        return;
    }

    if (HasHookedVTable(g_HookedSurfaceVTables, surfaceVTable)) {
        HookLog("DDraw: InstallSurfaceHooksForSurface skipped for %s - vtable already hooked (surface=%p, vtable=%p)",
                reason, surface, surfaceVTable);
        return;
    }

    g_HookedSurfaceVTables.push_back(surfaceVTable);
    if (markPrototype && !g_HookSurfacePrototype)
        g_HookSurfacePrototype = surface;

    HookLog("DDraw: Installing surface hooks via %s (surface=%p, vtable=%p, prototype=%d)", reason, surface,
            surfaceVTable, markPrototype ? 1 : 0);

    VTableHook::Status flipStatus =
        VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_FLIP], (LPVOID)&DetourDDSurface7Flip,
                           oDDSurface7Flip ? nullptr : (LPVOID*)&oDDSurface7Flip);
    if (flipStatus == VTableHook::Success) {
        HookLog("DDraw: Flip hook installed via %s", reason);
    } else {
        HookLog("DDraw: Flip hook install via %s returned %s", reason, VTableHook::StatusToString(flipStatus));
    }

    VTableHook::Status bltStatus =
        VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_BLT], (LPVOID)&DetourDDSurface7Blt,
                           oDDSurface7Blt ? nullptr : (LPVOID*)&oDDSurface7Blt);
    if (bltStatus == VTableHook::Success) {
        HookLog("DDraw: Blt hook installed via %s", reason);
    } else {
        HookLog("DDraw: Blt hook install via %s returned %s", reason, VTableHook::StatusToString(bltStatus));
    }

    VTableHook::Status lockStatus =
        VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_LOCK], (LPVOID)&DetourDDSurface7Lock,
                           oDDSurface7Lock ? nullptr : (LPVOID*)&oDDSurface7Lock);
    if (lockStatus == VTableHook::Success) {
        HookLog("DDraw: Lock hook installed via %s", reason);
    } else {
        HookLog("DDraw: Lock hook install via %s returned %s", reason, VTableHook::StatusToString(lockStatus));
    }

    VTableHook::Status unlockStatus =
        VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_UNLOCK], (LPVOID)&DetourDDSurface7Unlock,
                           oDDSurface7Unlock ? nullptr : (LPVOID*)&oDDSurface7Unlock);
    if (unlockStatus == VTableHook::Success) {
        HookLog("DDraw: Unlock hook installed via %s", reason);
    } else {
        HookLog("DDraw: Unlock hook install via %s returned %s", reason, VTableHook::StatusToString(unlockStatus));
    }

    IDirectDrawSurface4* surface4 = nullptr;
    if (SUCCEEDED(surface->QueryInterface(IID_IDirectDrawSurface4, reinterpret_cast<void**>(&surface4))) && surface4) {
        InstallSurfaceHooksForSurface4(surface4, reason, markPrototype);
        surface4->Release();
    }
}

static void InstallDirectDraw4HooksForInstance(IDirectDraw4* ddraw4, const char* reason) {
    if (!ddraw4) {
        HookLog("DDraw: InstallDirectDraw4HooksForInstance skipped for %s - null object", reason);
        return;
    }

    void** ddraw4VTable = *(void***)ddraw4;
    if (!ddraw4VTable) {
        HookLog("DDraw: InstallDirectDraw4HooksForInstance skipped for %s - null vtable (object=%p)", reason, ddraw4);
        return;
    }

    if (HasHookedVTable(g_HookedDDrawVTables, ddraw4VTable)) {
        HookLog(
            "DDraw: InstallDirectDraw4HooksForInstance skipped for %s - vtable already hooked (object=%p, vtable=%p)",
            reason, ddraw4, ddraw4VTable);
        return;
