                shmemSurfaces[consumeIdx]->UnlockRect();

                if (canUpload) {
                    SignalPublishedTextureFrame(texIdx, frame.timestampQPC);
                    AdvanceWriteIndex();
                } else if (texIdx < 0) {
                    droppedFrames.fetch_add(1, std::memory_order_relaxed);
                }
            }

            pendingReadIdx.store(rIdx + 1, std::memory_order_release);
        }

        captureThreadRunning = false;
        EarlyLog("DX9: Staging capture thread stopped");
    }

    // Background capture thread for GDI interop path.
    // Dequeues frames from the pending ring and runs the expensive
    // GetDC+BitBlt transfer work off the render thread.
    void GDICaptureThreadProc() {
        captureThreadRunning = true;
        EarlyLog("DX9: GDI capture thread started");

        int64_t qpcFreq = 0;
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }

        while (!captureThreadShutdown.load(std::memory_order_acquire)) {
            uint32_t rIdx = pendingReadIdx.load(std::memory_order_acquire);
            uint32_t wIdx = pendingWriteIdx.load(std::memory_order_acquire);

            if (rIdx == wIdx) {
                WaitForSingleObject(captureEvent, 50);
                continue;
            }

            PendingCaptureFrame& frame = pendingRing[rIdx % CAPTURE_RING_SIZE];
            const int surfIdx = static_cast<int>(frame.backBufferIndex);

            // Mark buffer busy so render thread won't StretchRect to it
            gdiBufferBusy[surfIdx].store(true, std::memory_order_release);

            LARGE_INTEGER captureStart;
            QueryPerformanceCounter(&captureStart);

            CompleteGDIInteropCapture(gdiCopySurfaces[surfIdx], frame.timestampQPC);

            LARGE_INTEGER captureEnd;
            QueryPerformanceCounter(&captureEnd);
            int32_t captureUs =
                static_cast<int32_t>(((captureEnd.QuadPart - captureStart.QuadPart) * 1000000) / qpcFreq);

            // Mark buffer available again
            gdiBufferBusy[surfIdx].store(false, std::memory_order_release);

            static int gdiThreadLogCount = 0;
            if (++gdiThreadLogCount <= 5 || gdiThreadLogCount % 200 == 0)
                HookLog("DX9: GDI thread: frame #%d surf[%d] %dus", gdiThreadLogCount, surfIdx, captureUs);

            pendingReadIdx.store(rIdx + 1, std::memory_order_release);
        }

        captureThreadRunning = false;
        EarlyLog("DX9: GDI capture thread stopped");
    }

    // CPU Prerender Limit
    struct QuerySlot {
        IDirect3DQuery9* query = nullptr;
    };
    std::vector<QuerySlot> prerenderQueries;
    uint32_t prerenderIdx = 0;

    void Cleanup() override {
        CleanupDX9(false);
    }

    void ForceCleanup() {
        CleanupDX9(false, true);
    }

    void ReleaseSharedTextureRing() {
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            if (gdiSharedRingSurfaces[i]) {
                gdiSharedRingSurfaces[i]->Release();
                gdiSharedRingSurfaces[i] = nullptr;
            }
            if (sharedTextures[i]) {
                sharedTextures[i]->Release();
                sharedTextures[i] = nullptr;
            }
            sharedTextureHandles[i].store(NULL, std::memory_order_release);
        }
        gdiDirectSharedRing = false;
    }

    bool CreateSharedTextureRing(bool gdiCompatible) {
        ReleaseSharedTextureRing();

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = (DXGI_FORMAT)format;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        if (gdiCompatible) {
            texDesc.MiscFlags |= D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
        }

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            HRESULT createHr = d3d11Device->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
            if (FAILED(createHr) || !sharedTextures[i]) {
                HookLogImportant("DX9: Failed to create %sring texture %d (hr=0x%08x)",
                                 gdiCompatible ? "GDI-shared " : "", i, (unsigned)createHr);
                ReleaseSharedTextureRing();
                return false;
            }

            IDXGIResource* resource = nullptr;
            HRESULT handleHr = sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            if (FAILED(handleHr) || !resource) {
                HookLogImportant("DX9: Failed to query IDXGIResource for ring texture %d (hr=0x%08x)", i,
                                 (unsigned)handleHr);
                ReleaseSharedTextureRing();
                return false;
            }

            HANDLE handle = NULL;
            resource->GetSharedHandle(&handle);
            resource->Release();
            if (!handle) {
                HookLogImportant("DX9: Failed to get shared handle for ring texture %d", i);
                ReleaseSharedTextureRing();
                return false;
            }
            sharedTextureHandles[i].store(handle, std::memory_order_release);

            if (gdiCompatible) {
                HRESULT surfaceHr = sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&gdiSharedRingSurfaces[i]));
                if (FAILED(surfaceHr) || !gdiSharedRingSurfaces[i]) {
                    HookLogImportant("DX9: Ring texture %d is not GDI-compatible (hr=0x%08x)", i, (unsigned)surfaceHr);
                    ReleaseSharedTextureRing();
                    return false;
                }
            }
        }

        gdiDirectSharedRing = gdiCompatible;
        return true;
    }

    void ReleaseDirectD3D9RingResources() {
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            if (directSharedQueries9[i]) {
                directSharedQueries9[i]->Release();
                directSharedQueries9[i] = nullptr;
            }
            if (directSharedSurfaces9[i]) {
                directSharedSurfaces9[i]->Release();
                directSharedSurfaces9[i] = nullptr;
            }
            if (directSharedTextures9[i]) {
                directSharedTextures9[i]->Release();
                directSharedTextures9[i] = nullptr;
            }
            if (directSharedProducerTextures9[i]) {
                directSharedProducerTextures9[i]->Release();
                directSharedProducerTextures9[i] = nullptr;
            }
            sharedTextureHandles[i].store(NULL, std::memory_order_release);
        }

        useDirectD3D9SharedRing = false;
        directSharedUsesHelperProducer = false;
        ResetDirectD3D9SharedRingPendingState();
    }

    void ReleaseDirectD3D9HelperDevices() {
        if (directSharedProducerDevice) {
            DX9_UnregisterInternalHelperDevice(directSharedProducerDevice);
            directSharedProducerDevice->Release();
            directSharedProducerDevice = nullptr;
        }
        directSharedLegacyConfig = {};
        if (directSharedFactory) {
            directSharedFactory->Release();
            directSharedFactory = nullptr;
        }
        if (directSharedProducerDeviceEx) {
            DX9_UnregisterInternalHelperDevice(directSharedProducerDeviceEx);
            directSharedProducerDeviceEx->Release();
            directSharedProducerDeviceEx = nullptr;
        }
        directSharedExConfig = {};
        if (directSharedFactoryEx) {
            directSharedFactoryEx->Release();
            directSharedFactoryEx = nullptr;
        }
        if (directSharedHelperWindow) {
            DestroyWindow(directSharedHelperWindow);
            directSharedHelperWindow = nullptr;
        }
    }

    void ReleaseDirectD3D9SharedRing() {
        ReleaseDirectD3D9RingResources();
        ReleaseDirectD3D9HelperDevices();
    }

    bool EnsureDirectD3D9HelperWindow() {
        if (directSharedHelperWindow)
            return true;

        static constexpr const char* kHelperWindowClass = "CE_DX9SharedRingHelper";

        WNDCLASSEXA wc = {sizeof(wc)};
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = kHelperWindowClass;
        if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            HookLogImportant("DX9: Direct D3D9 shared ring unavailable - helper window class registration failed");
            return false;
        }

        directSharedHelperWindow = CreateWindowA(kHelperWindowClass, "CE DX9 Shared Ring", WS_OVERLAPPED, 0, 0, 64, 64,
                                                 nullptr, nullptr, wc.hInstance, nullptr);
        if (!directSharedHelperWindow) {
            HookLogImportant("DX9: Direct D3D9 shared ring unavailable - helper window creation failed");
            return false;
        }
        ShowWindow(directSharedHelperWindow, SW_HIDE);
        return true;
    }

    static DWORD BuildDirectD3D9HelperBehaviorFlags(DWORD gameBehaviorFlags) {
        DWORD helperFlags = D3DCREATE_MULTITHREADED;
        if (gameBehaviorFlags & D3DCREATE_FPU_PRESERVE) {
            helperFlags |= D3DCREATE_FPU_PRESERVE;
        }

        if (gameBehaviorFlags & D3DCREATE_HARDWARE_VERTEXPROCESSING) {
            helperFlags |= D3DCREATE_HARDWARE_VERTEXPROCESSING;
        } else if (gameBehaviorFlags & D3DCREATE_MIXED_VERTEXPROCESSING) {
            helperFlags |= D3DCREATE_MIXED_VERTEXPROCESSING;
        } else {
            helperFlags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
        }

        return helperFlags;
    }

    static DWORD BuildDirectD3D9HelperSoftwareVpFlags(DWORD helperFlags) {
        helperFlags &= ~(D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING);
        helperFlags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
        return helperFlags;
    }

    D3DFORMAT ResolveDirectD3D9HelperBackBufferFormat() const {
        if (d3d9SharedFormat != D3DFMT_UNKNOWN) {
            return d3d9SharedFormat;
        }
        if (d3d9Format != D3DFMT_UNKNOWN) {
            return d3d9Format;
        }
        return D3DFMT_A8R8G8B8;
    }

    void BuildDirectD3D9HelperPresentParameters(D3DPRESENT_PARAMETERS& pp, bool useExRuntime) const {
        pp = {};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = directSharedHelperWindow;
        if (useExRuntime) {
            // The Ex helper producer never presents. A minimal hidden swapchain
            // avoids 0x0/UNKNOWN device-creation quirks and keeps helper VRAM
            // pressure low while still allowing shared-resource creation.
            pp.BackBufferWidth = 1;
            pp.BackBufferHeight = 1;
            pp.BackBufferFormat = ResolveDirectD3D9HelperBackBufferFormat();
        } else {
            // Plain D3D9 is stricter in windowed mode: let the runtime pick a
            // desktop-compatible backbuffer so the helper device can exist only
            // as a resource factory for native shared-texture zero-copy.
            pp.BackBufferWidth = 0;
            pp.BackBufferHeight = 0;
            pp.BackBufferFormat = D3DFMT_UNKNOWN;
        }
        pp.BackBufferCount = 1;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }

    void ResetDirectD3D9SharedRingPendingState() {
        directSharedSubmitIdx = 0;
        directSharedDrainIdx = 0;
        directSharedPendingCount = 0;
        zeroCopyPendingCopy = false;
        zeroCopyPendingIdx = -1;
        zeroCopyPendingTimestampQpc = 0;
        zeroCopyQueryWaitUs = 0;
        zeroCopyReadbackUs = 0;
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            directSharedPending[i] = false;
            directSharedPendingTimestampQpc[i] = 0;
        }
    }

    int AcquirePublishedTextureSlot() {
        SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        const int slot = FindAvailableCaptureTextureSlot(sharedMem, writeIndex.load(std::memory_order_relaxed));
        if (slot >= 0)
            writeIndex.store(slot, std::memory_order_relaxed);
        return slot;
    }

    void SignalPublishedTextureFrame(int idx, int64_t frameTimestampQpc) {
        uint64_t publishedFenceValue = 0;
        if (useFences && fence && context4) {
            const uint64_t candidateFenceValue = ++fenceValue;
            const HRESULT signalHr = context4->Signal(fence, candidateFenceValue);
            if (SUCCEEDED(signalHr)) {
                publishedFenceValue = candidateFenceValue;
            } else {
                HookLog("DX9: Capture fence Signal failed value=%llu hr=0x%08X; using implicit sync later",
                        static_cast<unsigned long long>(candidateFenceValue), signalHr);
                useFences = false;
            }
        }
        if (publishedFenceValue == 0 && d3d11Context)
            d3d11Context->Flush();
        SignalFrameReady(g_IPC, idx, frameTimestampQpc, publishedFenceValue);
    }

    int AcquireDirectD3D9SharedRingSubmitIndex() {
        SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        for (int attempt = 0; attempt < CAPTURE_TEXTURE_COUNT; ++attempt) {
            const int idx = (directSharedSubmitIdx + attempt) % CAPTURE_TEXTURE_COUNT;
            if (!directSharedPending[idx] && !IsCaptureTextureSlotOutstanding(sharedMem, idx)) {
                directSharedSubmitIdx = (idx + 1) % CAPTURE_TEXTURE_COUNT;
                return idx;
            }
        }
        return -1;
    }

    void SignalDirectD3D9SharedRingFrame(int idx, int64_t frameTimestampQpc) {
        SignalPublishedTextureFrame(idx, frameTimestampQpc);
    }

    void DrainDirectD3D9SharedRingCompletions(bool flushOutstanding) {
        zeroCopyQueryWaitUs = 0;
        zeroCopyReadbackUs = 0;

        if (!useDirectD3D9SharedRing || directSharedPendingCount <= 0) {
            return;
        }

        const DWORD getDataFlags = flushOutstanding ? D3DGETDATA_FLUSH : 0;
        int completedThisPass = 0;

        while (directSharedPendingCount > 0 && completedThisPass < CAPTURE_TEXTURE_COUNT) {
            int idx = -1;
            for (int attempt = 0; attempt < CAPTURE_TEXTURE_COUNT; ++attempt) {
                const int candidate = (directSharedDrainIdx + attempt) % CAPTURE_TEXTURE_COUNT;
                if (directSharedPending[candidate]) {
                    idx = candidate;
                    break;
                }
            }

            if (idx < 0) {
                ResetDirectD3D9SharedRingPendingState();
                return;
            }

            IDirect3DQuery9* query = directSharedQueries9[idx];
            HRESULT queryHr = S_OK;
            if (query) {
                // D3DGETDATA_FLUSH requests submission, but completion remains
                // non-blocking. Never spin the Present/shutdown thread on a GPU
                // query whose device may be lost or no longer making progress.
                queryHr = query->GetData(nullptr, 0, getDataFlags);
            }

            if (queryHr == S_FALSE) {
                break;
            }

            const int64_t frameTimestampQpc = directSharedPendingTimestampQpc[idx];
            directSharedPending[idx] = false;
            directSharedPendingTimestampQpc[idx] = 0;
            if (directSharedPendingCount > 0) {
                directSharedPendingCount--;
            }
            directSharedDrainIdx = (idx + 1) % CAPTURE_TEXTURE_COUNT;

            if (SUCCEEDED(queryHr)) {
                SignalDirectD3D9SharedRingFrame(idx, frameTimestampQpc);
            } else {
                droppedFrames.fetch_add(1, std::memory_order_relaxed);
                static int queryFailLogCount = 0;
                if (queryFailLogCount < 4) {
                    HookLogImportant("DX9: Direct shared-ring query failed idx=%d hr=0x%08x", idx, (unsigned)queryHr);
                    queryFailLogCount++;
                }
            }

            completedThisPass++;
        }
    }

    void LogDirectD3D9SharingDiagnostics(IDirect3DDevice9* device, const D3DDEVICE_CREATION_PARAMETERS& params,
                                         const char* label) {
        if (!device || !label)
            return;

        IDirect3D9* direct3D = nullptr;
        HRESULT getD3DHr = device->GetDirect3D(&direct3D);
        if (FAILED(getD3DHr) || !direct3D) {
            HookLogImportant("DX9: %s diagnostics unavailable - GetDirect3D failed (hr=0x%08x)", label,
                             (unsigned)getD3DHr);
            return;
        }

        IDirect3DDevice9Ex* deviceEx = nullptr;
        const bool isEx =
            SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&deviceEx)) && deviceEx;
        if (deviceEx) {
            deviceEx->Release();
        }

        D3DCAPS9 caps = {};
        D3DADAPTER_IDENTIFIER9 identifier = {};
        D3DDISPLAYMODE displayMode = {};
        const HRESULT capsHr = direct3D->GetDeviceCaps(params.AdapterOrdinal, params.DeviceType, &caps);
        const HRESULT identHr = direct3D->GetAdapterIdentifier(params.AdapterOrdinal, 0, &identifier);
        const HRESULT modeHr = direct3D->GetAdapterDisplayMode(params.AdapterOrdinal, &displayMode);
        const D3DFORMAT adapterFormat = SUCCEEDED(modeHr) ? displayMode.Format : d3d9Format;
        const HRESULT sharedFmtHr =
            direct3D->CheckDeviceFormat(params.AdapterOrdinal, params.DeviceType, adapterFormat, D3DUSAGE_RENDERTARGET,
                                        D3DRTYPE_TEXTURE, d3d9SharedFormat);
        const HRESULT conversionHr = d3d9Format == d3d9SharedFormat
                                         ? D3D_OK
                                         : direct3D->CheckDeviceFormatConversion(
                                               params.AdapterOrdinal, params.DeviceType, d3d9Format, d3d9SharedFormat);
        bool advertisesExSharing = false;
#ifdef D3DCAPS2_CANSHARERESOURCE
        advertisesExSharing = SUCCEEDED(capsHr) && ((caps.Caps2 & D3DCAPS2_CANSHARERESOURCE) != 0);
#endif

        HookLogImportant(
            "DX9: %s diagnostics: adapter=%u type=%u flags=0x%08x ex=%d adapterFmt=%s/%d backBufferFmt=%s/%d "
            "sharedFmt=%s/%d capsHr=0x%08x caps2=0x%08x exShareCap=%d fmtCheck=0x%08x conversion=0x%08x "
            "vendor=%04x device=%04x driver=%s",
            label, params.AdapterOrdinal, (unsigned)params.DeviceType, (unsigned)params.BehaviorFlags, isEx ? 1 : 0,
            D3D9FormatName(adapterFormat), (int)adapterFormat, D3D9FormatName(d3d9Format), (int)d3d9Format,
            D3D9FormatName(d3d9SharedFormat), (int)d3d9SharedFormat, (unsigned)capsHr,
            SUCCEEDED(capsHr) ? (unsigned)caps.Caps2 : 0u, advertisesExSharing ? 1 : 0, (unsigned)sharedFmtHr,
            (unsigned)conversionHr, SUCCEEDED(identHr) ? identifier.VendorId : 0u,
            SUCCEEDED(identHr) ? identifier.DeviceId : 0u, SUCCEEDED(identHr) ? identifier.Driver : "?");

        if (!isEx && !advertisesExSharing) {
            HookLogImportant(
                "DX9: %s classic-device share capability is probe-only; the Ex-only caps bit is not "
                "used as a gate",
                label);
        }

        direct3D->Release();
    }

    bool ProbeDirectD3D9SharedTexture(IDirect3DDevice9* device, const char* label) {
        if (!device || !label)
            return false;

        HANDLE sharedHandle = NULL;
        IDirect3DTexture9* texture = nullptr;
        const HRESULT probeHr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9SharedFormat,
                                                      D3DPOOL_DEFAULT, &texture, &sharedHandle);
        const bool success = SUCCEEDED(probeHr) && texture && sharedHandle;
        HookLogImportant("DX9: %s shared-texture probe fmt=%s/%d hr=0x%08x tex=%p handle=%p", label,
                         D3D9FormatName(d3d9SharedFormat), (int)d3d9SharedFormat, (unsigned)probeHr, texture,
                         sharedHandle);
        if (texture) {
            texture->Release();
        }
        // D3D9 shared-resource values are legacy resource-owned identifiers,
        // not NT handles returned by CreateSharedHandle.
        return success;
    }

    bool EnsureDirectD3D9ExProducerDevice(const D3DDEVICE_CREATION_PARAMETERS& params) {
        const DWORD helperFlags = BuildDirectD3D9HelperBehaviorFlags(params.BehaviorFlags);
        if (directSharedProducerDeviceEx) {
            const bool sameConfig = directSharedExConfig.valid &&
                                    directSharedExConfig.adapterOrdinal == params.AdapterOrdinal &&
                                    directSharedExConfig.deviceType == params.DeviceType &&
                                    directSharedExConfig.behaviorFlags == helperFlags;
            if (sameConfig)
                return true;

            HookLogImportant(
                "DX9: Recreating helper D3D9Ex producer for adapter/type change (oldAdapter=%u oldType=%u "
                "oldFlags=0x%08x newAdapter=%u newType=%u newFlags=0x%08x)",
                directSharedExConfig.adapterOrdinal, (unsigned)directSharedExConfig.deviceType,
                (unsigned)directSharedExConfig.behaviorFlags, params.AdapterOrdinal, (unsigned)params.DeviceType,
                (unsigned)helperFlags);
            directSharedProducerDeviceEx->Release();
            directSharedProducerDeviceEx = nullptr;
            directSharedExConfig = {};
        }

        HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
        if (!d3d9Module) {
            HookLogImportant("DX9: Direct D3D9 shared ring unavailable - d3d9.dll missing");
            return false;
        }
        if (!EnsureDirectD3D9HelperWindow()) {
            return false;
        }

        if (!directSharedFactoryEx) {
            Direct3DCreate9Ex_t create9Ex =
                reinterpret_cast<Direct3DCreate9Ex_t>(GetProcAddress(d3d9Module, "Direct3DCreate9Ex"));
            if (!create9Ex) {
                HookLogImportant("DX9: Direct D3D9Ex helper unavailable - Direct3DCreate9Ex missing");
                return false;
            }

            const HRESULT factoryHr = create9Ex(D3D_SDK_VERSION, &directSharedFactoryEx);
            if (FAILED(factoryHr) || !directSharedFactoryEx) {
                HookLogImportant("DX9: Direct D3D9Ex helper unavailable - Direct3DCreate9Ex failed (0x%08x)",
                                 (unsigned)factoryHr);
                directSharedFactoryEx = nullptr;
                return false;
            }
        }

        D3DPRESENT_PARAMETERS pp = {};
        BuildDirectD3D9HelperPresentParameters(pp, true);
        HookLogImportant("DX9: Trying helper D3D9Ex producer (adapter=%u type=%u flags=0x%08x)", params.AdapterOrdinal,
                         (unsigned)params.DeviceType, (unsigned)helperFlags);

        const DX9InternalBypassScope helperBypass;
        const HRESULT deviceHr =
            directSharedFactoryEx->CreateDeviceEx(params.AdapterOrdinal, params.DeviceType, directSharedHelperWindow,
                                                  helperFlags, &pp, nullptr, &directSharedProducerDeviceEx);
        if (FAILED(deviceHr) || !directSharedProducerDeviceEx) {
            HookLogImportant("DX9: Direct D3D9Ex helper unavailable - CreateDeviceEx failed (0x%08x)",
                             (unsigned)deviceHr);
            if (directSharedProducerDeviceEx) {
                directSharedProducerDeviceEx->Release();
                directSharedProducerDeviceEx = nullptr;
            }
            directSharedExConfig = {};
            return false;
        }

        directSharedExConfig.adapterOrdinal = params.AdapterOrdinal;
        directSharedExConfig.deviceType = params.DeviceType;
        directSharedExConfig.behaviorFlags = helperFlags;
        directSharedExConfig.valid = true;

        DX9_RegisterInternalHelperDevice(directSharedProducerDeviceEx);

        ProbeDirectD3D9SharedTexture(directSharedProducerDeviceEx, "helper D3D9Ex producer");
        return true;
    }

    bool EnsureDirectD3D9LegacyProducerDevice(const D3DDEVICE_CREATION_PARAMETERS& params) {
        const DWORD helperFlags = BuildDirectD3D9HelperBehaviorFlags(params.BehaviorFlags);
        if (directSharedProducerDevice) {
            const bool sameConfig = directSharedLegacyConfig.valid &&
                                    directSharedLegacyConfig.adapterOrdinal == params.AdapterOrdinal &&
                                    directSharedLegacyConfig.deviceType == params.DeviceType &&
                                    directSharedLegacyConfig.behaviorFlags == helperFlags;
            if (sameConfig)
                return true;

            HookLogImportant(
                "DX9: Recreating helper legacy D3D9 producer for adapter/type change (oldAdapter=%u oldType=%u "
                "oldFlags=0x%08x newAdapter=%u newType=%u newFlags=0x%08x)",
                directSharedLegacyConfig.adapterOrdinal, (unsigned)directSharedLegacyConfig.deviceType,
                (unsigned)directSharedLegacyConfig.behaviorFlags, params.AdapterOrdinal, (unsigned)params.DeviceType,
                (unsigned)helperFlags);
            directSharedProducerDevice->Release();
            directSharedProducerDevice = nullptr;
            directSharedLegacyConfig = {};
        }

        HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
        if (!d3d9Module) {
            HookLogImportant("DX9: Direct D3D9 shared ring unavailable - d3d9.dll missing");
            return false;
        }
        if (!EnsureDirectD3D9HelperWindow()) {
            return false;
        }

        if (!directSharedFactory) {
            Direct3DCreate9Helper_t create9 =
                reinterpret_cast<Direct3DCreate9Helper_t>(GetProcAddress(d3d9Module, "Direct3DCreate9"));
            if (!create9) {
                HookLogImportant("DX9: Direct D3D9 helper unavailable - Direct3DCreate9 missing");
                return false;
            }

            directSharedFactory = create9(D3D_SDK_VERSION);
            if (!directSharedFactory) {
                HookLogImportant("DX9: Direct D3D9 helper unavailable - Direct3DCreate9 failed");
                return false;
            }
        }

        auto tryCreateLegacyHelper = [&](DWORD attemptFlags, const char* attemptLabel) {
            D3DPRESENT_PARAMETERS pp = {};
            BuildDirectD3D9HelperPresentParameters(pp, false);
            HookLogImportant(
                "DX9: Trying helper legacy D3D9 producer (%s adapter=%u type=%u flags=0x%08x bbFmt=%s/%d bb=%ux%u)",
                attemptLabel, params.AdapterOrdinal, (unsigned)params.DeviceType, (unsigned)attemptFlags,
                D3D9FormatName(pp.BackBufferFormat), (int)pp.BackBufferFormat, pp.BackBufferWidth, pp.BackBufferHeight);

            const DX9InternalBypassScope helperBypass;
            return directSharedFactory->CreateDevice(params.AdapterOrdinal, params.DeviceType, directSharedHelperWindow,
                                                     attemptFlags, &pp, &directSharedProducerDevice);
        };

        DWORD selectedFlags = helperFlags;
        HRESULT deviceHr = tryCreateLegacyHelper(helperFlags, "game-flags");
        if ((FAILED(deviceHr) || !directSharedProducerDevice)) {
            const DWORD softwareVpFlags = BuildDirectD3D9HelperSoftwareVpFlags(helperFlags);
            if (softwareVpFlags != helperFlags) {
                if (directSharedProducerDevice) {
                    directSharedProducerDevice->Release();
                    directSharedProducerDevice = nullptr;
                }
                HookLogImportant(
                    "DX9: Helper legacy D3D9 producer retrying with software vertex processing after hr=0x%08x",
                    (unsigned)deviceHr);
                deviceHr = tryCreateLegacyHelper(softwareVpFlags, "software-vp fallback");
                selectedFlags = softwareVpFlags;
            }
