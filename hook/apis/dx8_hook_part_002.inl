        }
        g_PrerenderQueries.clear();
        g_PrerenderFrameIndex = 0;

        ReleaseD3D8Surface(d3d8SnapshotSurface);
        ReleaseD3D8Surface(d3d8FrontBufferSurface);
        d3d8SnapshotFormat = D3DFMT_UNKNOWN;

        d3d8Device = nullptr;
        overlayHwnd = NULL;
        initialized = false;
        generationResetPending = false;
        useFences = false;
        fenceValue = 0;
        return true;
    }

    void PrepareForDeviceReset() {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (CleanupDX8(false))
            return;

        // D3D8 default-pool surfaces must be released before Reset even when
        // the independently-owned D3D11 transport generation is still leased.
        ReleaseD3D8Surface(d3d8SnapshotSurface);
        ReleaseD3D8Surface(d3d8FrontBufferSurface);
        d3d8SnapshotFormat = D3DFMT_UNKNOWN;
        d3d8Device = nullptr;
        initialized = false;
        generationResetPending = true;
        HookLog("DX8: Retaining shared transport generation across device Reset until frame leases drain");
    }

    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
        // Implemented in Init
    }

    bool CreateD3D9ExWrapper(HWND hwnd) {
        if (d3d9DeviceEx)
            return true;
        // Create D3D9Ex calls dynamic
        HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
        if (!d3d9)
            d3d9 = ce::security::LoadSystemLibrary(L"d3d9.dll");
        if (!d3d9) {
            HookLog("DX8: D3D9 DLL not found");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);
        PFN_Direct3DCreate9Ex pDirect3DCreate9Ex = (PFN_Direct3DCreate9Ex)GetProcAddress(d3d9, "Direct3DCreate9Ex");

        if (!pDirect3DCreate9Ex) {
            HookLog("DX8: Direct3DCreate9Ex not found");
            return false;
        }

        HRESULT hr = pDirect3DCreate9Ex(D3D_SDK_VERSION, &d3d9Ex);
        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D9Ex (hr=0x%08x)", hr);
            return false;
        }

        // Create D3D9Ex device
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3DPRESENT_PARAMETERS d3dpp = {};
        d3dpp.Windowed = TRUE;
        d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        d3dpp.hDeviceWindow = hwnd;
        d3dpp.BackBufferFormat = D3DFMT_A8R8G8B8;
        d3dpp.BackBufferWidth = width;
        d3dpp.BackBufferHeight = height;
        d3dpp.BackBufferCount = 1;
        d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

        {
            DX9InternalBypassScope dx9Bypass;
            hr = d3d9Ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &d3dpp, NULL,
                                        &d3d9DeviceEx);
        }

        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D9Ex device (hr=0x%08x)", hr);
            d3d9Ex->Release();
            d3d9Ex = nullptr;
            return false;
        }

        DX9_RegisterInternalHelperDevice(d3d9DeviceEx);
        d3d9DeviceEx->SetMaximumFrameLatency(1);

        hr = d3d9DeviceEx->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                       &d3d9UploadSurface, nullptr);
        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D9 upload surface (hr=0x%08x)", hr);
            d3d9DeviceEx->Release();
            d3d9DeviceEx = nullptr;
            d3d9Ex->Release();
            d3d9Ex = nullptr;
            return false;
        }

        HookLog("DX8: D3D9Ex wrapper created");
        return true;
    }

    bool EnsureSnapshotSurface(IDirect3DDevice8* device) {
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
        D3D8_SURFACE_DESC_LOCAL desc = {};
        hr = D3D8SurfaceGetDesc(backBuffer, &desc);
        ReleaseD3D8Surface(backBuffer);
        if (FAILED(hr)) {
            static int getDescFailLogCount = 0;
            if (getDescFailLogCount < 4) {
                HookLog("DX8: Failed to query backbuffer desc for overlay composite (hr=0x%08x)", hr);
                getDescFailLogCount++;
            }
            return false;
        }

        if (desc.Width != width || desc.Height != height) {
            static int sizeMismatchLogCount = 0;
            if (sizeMismatchLogCount < 4) {
                HookLog("DX8: Backbuffer/helper size mismatch for overlay composite (%ux%u vs %ux%u)", desc.Width,
                        desc.Height, width, height);
                sizeMismatchLogCount++;
            }
            return false;
        }

        hr = GetD3D8CreateImageSurface(device)(device, width, height, desc.Format, &d3d8SnapshotSurface);
        if (FAILED(hr) || !d3d8SnapshotSurface) {
            static int createSnapshotFailLogCount = 0;
            if (createSnapshotFailLogCount < 4) {
                HookLog("DX8: Failed to create snapshot surface for overlay composite (hr=0x%08x)", hr);
                createSnapshotFailLogCount++;
            }
            return false;
        }

        d3d8SnapshotFormat = desc.Format;
        return true;
    }

    bool EnsureFrontBufferSurface(IDirect3DDevice8* device) {
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

    bool CopyLockedPixelsToSurface9(const D3DLOCKED_RECT& sourceLockedRect, D3DFORMAT sourceFormat,
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

    bool CopyLockedPixelsToOverlayBackbuffer(const D3DLOCKED_RECT& sourceLockedRect, D3DFORMAT sourceFormat) {
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

    bool CopySurfaceToSurface9(IDirect3DSurface8* sourceSurface, D3DFORMAT sourceFormat,
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

    bool CopyBackBufferToSurface9(IDirect3DDevice8* device, IDirect3DSurface9* destinationSurface) {
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

    bool CopyFrontBufferToSurface9(IDirect3DDevice8* device, IDirect3DSurface9* destinationSurface) {
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

    bool CopyFrontBufferToOverlayBackbuffer(IDirect3DDevice8* device) {
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

    bool PresentOverlay() {
        if (!d3d9DeviceEx) {
            return false;
        }

        HRESULT hr = E_FAIL;
        {
            DX9InternalBypassScope dx9Bypass;
            hr = d3d9DeviceEx->PresentEx(nullptr, nullptr, overlayHwnd, nullptr, 0);
        }
        static uint32_t overlayPresentCount = 0;
        overlayPresentCount++;
        if (overlayPresentCount <= 8) {
            HookLogImportant("DX8: Overlay helper PresentEx hr=0x%08X hwnd=%p size=%ux%u count=%u", (unsigned)hr,
                             overlayHwnd, width, height, overlayPresentCount);
        }

        if (FAILED(hr) && hr != D3DERR_WASSTILLDRAWING) {
            HookLog("DX8: Overlay helper present failed (hr=0x%08x)", hr);
        }

        return SUCCEEDED(hr);
    }

    bool CreateD3D11Device() {
        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL featureLevel;

        HMODULE hD3D11 = ce::security::LoadSystemLibrary(L"d3d11.dll");
        if (!hD3D11) {
            HookLog("DX8: D3D11 DLL not found");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                          const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                                          D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice =
            (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
            HookLog("DX8: D3D11CreateDevice not found");
            return false;
        }

        HRESULT hr = pD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, featureLevels, 2, D3D11_SDK_VERSION,
                                        &d3d11Device, &featureLevel, &d3d11Context);

        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D11 device (hr=0x%08x)", hr);
            return false;
        }

        // Get adapter LUID
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc;
                adapter->GetDesc(&desc);
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                luidLow = desc.AdapterLuid.LowPart;
                luidHigh = desc.AdapterLuid.HighPart;

                // Report LUID to shared memory for out-of-process polling
                ReportLUID(luidLow, luidHigh);
                adapter->Release();
            }
            dxgiDevice->Release();
        }

        // Try to get context4 for fences
        if (SUCCEEDED(d3d11Context->QueryInterface(IID_PPV_ARGS(&context4)))) {
            ID3D11Device5* device5 = nullptr;
            if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&device5)))) {
                if (SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)))) {
                    HANDLE hTemp = NULL;
                    fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hTemp);
                    sharedFenceHandle.store(hTemp, std::memory_order_release);
                    useFences = true;
                    HookLog("DX8: D3D11.3 fence sync enabled");
                }
                device5->Release();
            }
        }

        HookLog("DX8: D3D11 device created (LUID: %08x)", luidLow);
        return true;
    }

    bool CreateSharedTextures() {
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            HRESULT hr = d3d11Device->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
            if (FAILED(hr)) {
                HookLog("DX8: Failed to create shared texture %d (hr=0x%08x)", i, hr);
                return false;
            }

            // Get shared handle
            IDXGIResource* resource = nullptr;
            sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            HANDLE hTemp = NULL;
            resource->GetSharedHandle(&hTemp);
            sharedTextureHandles[i].store(hTemp, std::memory_order_release);
            resource->Release();
        }

        HookLog("DX8: Shared textures created");
        return true;
    }

    bool CreateD3D9ExSharedSurface() {
        // Create D3D9Ex offscreen surface that can share with D3D11
        HANDLE sharedHandle = nullptr;
        HRESULT hr = d3d9DeviceEx->CreateOffscreenPlainSurfaceEx(width, height, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                                                 &d3d9SharedSurface, &sharedHandle, 0);

        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D9Ex shared surface (hr=0x%08x)", hr);
            return false;
        }

        HookLog("DX8: D3D9Ex shared surface created");
        return true;
    }

    bool EnsureOverlayDevice(IDirect3DDevice8* device, HWND hwnd) {
        if (!hwnd) {
            return false;
        }

        RECT rect = {};
        GetClientRect(hwnd, &rect);
        uint32_t newWidth = rect.right - rect.left;
        uint32_t newHeight = rect.bottom - rect.top;
        if (newWidth == 0 || newHeight == 0) {
            return false;
        }

        const bool hwndChanged = overlayHwnd && overlayHwnd != hwnd;
        const bool sizeChanged = width != newWidth || height != newHeight;
        if ((hwndChanged || sizeChanged) && (d3d9DeviceEx || initialized)) {
            if (g_OverlayAdapter.IsInitialized()) {
                g_OverlayAdapter.Shutdown();
            }
            if (!CleanupDX8(false))
                return false;
        }

        d3d8Device = device;
        overlayHwnd = hwnd;
        width = newWidth;
        height = newHeight;
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (d3d9DeviceEx) {
            return true;
        }

        if (!CreateD3D9ExWrapper(hwnd)) {
            HookLog("DX8: Overlay helper creation failed");
            return false;
        }

        HookLog("DX8: Overlay helper ready (hwnd=%p, size=%ux%u)", hwnd, width, height);
        return true;
    }

    void Init(IDirect3DDevice8* device, HWND hwnd) {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (initialized)
            return;
        if (generationResetPending && !CleanupDX8(false))
            return;

        d3d8Device = device;
        overlayHwnd = hwnd;

        // Get backbuffer size from HWND
        RECT rect;
        GetClientRect(hwnd, &rect);
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (width == 0 || height == 0) {
            HookLog("DX8: Invalid window size");
            return;
        }

        // Create D3D9Ex wrapper for sharing
        if (!CreateD3D9ExWrapper(hwnd)) {
            CleanupDX8(false);
            return;
        }

        // Create D3D11 device
        if (!CreateD3D11Device()) {
            CleanupDX8(false);
            return;
        }

        // Create shared textures
        if (!CreateSharedTextures()) {
            CleanupDX8(false);
            return;
        }

        // Create D3D9Ex shared surface
        if (!CreateD3D9ExSharedSurface()) {
            CleanupDX8(false);
            return;
        }

        // Publish to shared memory
        if (g_IPC) {
            PublishToSharedMemory(g_IPC);
        }

        initialized = true;
        HookLog("DX8 Capture Initialized: %dx%d", width, height);
