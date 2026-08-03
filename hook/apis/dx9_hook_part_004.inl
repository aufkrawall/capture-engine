        }

        if (FAILED(deviceHr) || !directSharedProducerDevice) {
            HookLogImportant("DX9: Direct D3D9 helper unavailable - CreateDevice failed (0x%08x)", (unsigned)deviceHr);
            if (directSharedProducerDevice) {
                directSharedProducerDevice->Release();
                directSharedProducerDevice = nullptr;
            }
            directSharedLegacyConfig = {};
            return false;
        }

        directSharedLegacyConfig.adapterOrdinal = params.AdapterOrdinal;
        directSharedLegacyConfig.deviceType = params.DeviceType;
        directSharedLegacyConfig.behaviorFlags = selectedFlags;
        directSharedLegacyConfig.valid = true;

        DX9_RegisterInternalHelperDevice(directSharedProducerDevice);

        ProbeDirectD3D9SharedTexture(directSharedProducerDevice, "helper legacy D3D9 producer");
        return true;
    }

    bool ValidateDirectD3D9SharedHandle(HANDLE sharedHandle) {
        if (!d3d11Device || !sharedHandle)
            return false;

        ID3D11Texture2D* openedTexture = nullptr;
        HRESULT openHr =
            d3d11Device->OpenSharedResource(sharedHandle, __uuidof(ID3D11Texture2D), (void**)&openedTexture);
        if (FAILED(openHr) || !openedTexture) {
            HookLogImportant("DX9: Direct D3D9 shared ring validation failed (OpenSharedResource hr=0x%08x)",
                             (unsigned)openHr);
            return false;
        }

        D3D11_TEXTURE2D_DESC desc = {};
        openedTexture->GetDesc(&desc);
        format = static_cast<uint32_t>(desc.Format);
        HookLogImportant("DX9: Direct D3D9 shared ring validated in D3D11 (format=%u)", (unsigned)desc.Format);
        openedTexture->Release();
        return true;
    }

    bool TrySetupDirectD3D9SharedRingWithProducer(IDirect3DDevice9* gameDevice, IDirect3DDevice9* producerDevice,
                                                  bool useHelperProducer, const char* producerLabel) {
        if (!gameDevice || !producerDevice || !producerLabel)
            return false;

        auto failSetup = [&](const char* message, HRESULT failureHr) {
            HookLogImportant("DX9: %s (producer=%s hr=0x%08x)", message, producerLabel, (unsigned)failureHr);
            CleanupSharedHandles();
            ReleaseDirectD3D9RingResources();
            return false;
        };

        ReleaseDirectD3D9RingResources();

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            HANDLE sharedHandle = NULL;
            IDirect3DTexture9* producerTexture = nullptr;
            const HRESULT producerHr =
                producerDevice->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9SharedFormat,
                                              D3DPOOL_DEFAULT, &producerTexture, &sharedHandle);
            if (FAILED(producerHr) || !producerTexture || !sharedHandle) {
                if (producerTexture) {
                    producerTexture->Release();
                }
                return failSetup("Direct D3D9 shared ring producer texture creation failed", producerHr);
            }

            IDirect3DTexture9* captureTexture = producerTexture;
            if (useHelperProducer) {
                HANDLE openHandle = sharedHandle;
                const HRESULT openHr =
                    gameDevice->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9SharedFormat,
                                              D3DPOOL_DEFAULT, &captureTexture, &openHandle);
                if (FAILED(openHr) || !captureTexture) {
                    producerTexture->Release();
                    return failSetup("Direct D3D9 shared ring open-on-game-device failed", openHr);
                }
                if (openHandle) {
                    sharedHandle = openHandle;
                }
                directSharedProducerTextures9[i] = producerTexture;
            }

            directSharedTextures9[i] = captureTexture;
            sharedTextureHandles[i].store(sharedHandle, std::memory_order_release);

            const HRESULT surfaceHr = captureTexture->GetSurfaceLevel(0, &directSharedSurfaces9[i]);
            if (FAILED(surfaceHr) || !directSharedSurfaces9[i]) {
                return failSetup("Direct D3D9 shared ring GetSurfaceLevel failed", surfaceHr);
            }

            const HRESULT queryHr = gameDevice->CreateQuery(D3DQUERYTYPE_EVENT, &directSharedQueries9[i]);
            if (FAILED(queryHr) || !directSharedQueries9[i]) {
                return failSetup("Direct D3D9 shared ring query creation failed", queryHr);
            }
        }

        if (!ValidateDirectD3D9SharedHandle(sharedTextureHandles[0].load(std::memory_order_acquire))) {
            CleanupSharedHandles();
            ReleaseDirectD3D9RingResources();
            return false;
        }

        useDirectD3D9SharedRing = true;
        directSharedUsesHelperProducer = useHelperProducer;
        HookLogImportant("DX9: Direct D3D9 shared ring zero-copy path active (%s)", producerLabel);
        return true;
    }

    bool SetupDirectD3D9SharedRing(IDirect3DDevice9* device, bool isD3D9Ex) {
        if (!device || IsDXVKD3D9WrapperLoaded())
            return false;

        CleanupSharedHandles();
        ReleaseDirectD3D9SharedRing();

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3DDEVICE_CREATION_PARAMETERS params = {};
        if (FAILED(device->GetCreationParameters(&params))) {
            HookLogImportant("DX9: Direct D3D9 shared ring unavailable - GetCreationParameters failed");
            return false;
        }

        const D3DFORMAT preferredSharedFormat = d3d9SharedFormat;
        D3DFORMAT sharedFormatCandidates[2] = {preferredSharedFormat, d3d9Format};
        const int candidateCount = (d3d9Format != D3DFMT_UNKNOWN && d3d9Format != preferredSharedFormat) ? 2 : 1;

        for (int candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
            d3d9SharedFormat = sharedFormatCandidates[candidateIndex];
            if (candidateIndex > 0) {
                HookLogImportant("DX9: Retrying direct shared ring with native backbuffer format %s/%d",
                                 D3D9FormatName(d3d9SharedFormat), (int)d3d9SharedFormat);
            }

            LogDirectD3D9SharingDiagnostics(device, params, "game device");

            // Some WDDM drivers expose the sharing caps bit through a classic
            // device but reject shared creation/opening with D3DERR_INVALIDCALL.
            // Probe the actual classic device first so an unsupported runtime
            // does not pay for helpers that the game device cannot open.
            const bool nativeProbeOk = ProbeDirectD3D9SharedTexture(device, "game device");
            if (!isD3D9Ex && !nativeProbeOk) {
                HookLogImportant(
                    "DX9: Classic device rejected shared-resource creation for %s/%d; skipping helper producers",
                    D3D9FormatName(d3d9SharedFormat), (int)d3d9SharedFormat);
                CleanupSharedHandles();
                ReleaseDirectD3D9SharedRing();
                continue;
            }

            if (EnsureDirectD3D9ExProducerDevice(params)) {
                // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
                D3DDEVICE_CREATION_PARAMETERS helperParams = {};
                if (SUCCEEDED(directSharedProducerDeviceEx->GetCreationParameters(&helperParams))) {
                    LogDirectD3D9SharingDiagnostics(directSharedProducerDeviceEx, helperParams,
                                                    "helper D3D9Ex producer");
                }
                if (TrySetupDirectD3D9SharedRingWithProducer(device, directSharedProducerDeviceEx, true,
                                                             "helper D3D9Ex producer")) {
                    return true;
                }
            }

            if (EnsureDirectD3D9LegacyProducerDevice(params)) {
                // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
                D3DDEVICE_CREATION_PARAMETERS helperParams = {};
                if (SUCCEEDED(directSharedProducerDevice->GetCreationParameters(&helperParams))) {
                    LogDirectD3D9SharingDiagnostics(directSharedProducerDevice, helperParams,
                                                    "helper legacy D3D9 producer");
                }
                if (TrySetupDirectD3D9SharedRingWithProducer(device, directSharedProducerDevice, true,
                                                             "helper legacy D3D9 producer")) {
                    return true;
                }
            }

            // Native ownership is a last resort: a helper-owned shared resource
            // can survive release of the game device's default-pool view during
            // Reset while queued media frames finish consuming the old generation.
            if (nativeProbeOk) {
                const char* nativeProducerLabel = isD3D9Ex ? "native D3D9Ex producer" : "native D3D9 producer";
                if (TrySetupDirectD3D9SharedRingWithProducer(device, device, false, nativeProducerLabel)) {
                    HookLogImportant(
                        "DX9: Native shared-ring ownership active; reset-time generation retention may "
                        "require an immediate helper handoff");
                    return true;
                }
            }

            CleanupSharedHandles();
            ReleaseDirectD3D9SharedRing();
        }

        d3d9SharedFormat = preferredSharedFormat;

        HookLogImportant("DX9: Direct D3D9 shared ring unavailable after all producer attempts");
        return false;
    }

    bool HasPublishedGeneration() const {
        if (sharedFenceHandle.load(std::memory_order_acquire) != NULL)
            return true;
        for (const auto& handle : sharedTextureHandles) {
            if (handle.load(std::memory_order_acquire) != NULL)
                return true;
        }
        return false;
    }

    bool EnsureNativeDirectRingRetirementOwner() {
        if (!useDirectD3D9SharedRing || directSharedUsesHelperProducer)
            return true;
        if (!d3d9Device)
            return false;

// NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3DDEVICE_CREATION_PARAMETERS params = {};
        if (FAILED(d3d9Device->GetCreationParameters(&params)))
            return false;

        IDirect3DDevice9* ownerDevice = nullptr;
        if (EnsureDirectD3D9ExProducerDevice(params)) {
            ownerDevice = directSharedProducerDeviceEx;
        } else if (EnsureDirectD3D9LegacyProducerDevice(params)) {
            ownerDevice = directSharedProducerDevice;
        }
        if (!ownerDevice)
            return false;

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            HANDLE openHandle = sharedTextureHandles[i].load(std::memory_order_acquire);
            if (!openHandle || !directSharedTextures9[i])
                return false;

            IDirect3DTexture9* retirementOwner = nullptr;
            const HRESULT openHr = ownerDevice->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9SharedFormat,
                                                              D3DPOOL_DEFAULT, &retirementOwner, &openHandle);
            if (FAILED(openHr) || !retirementOwner) {
                for (auto*& owner : directSharedProducerTextures9) {
                    if (owner) {
                        owner->Release();
                        owner = nullptr;
                    }
                }
                HookLogImportant(
                    "DX9: Failed to hand native shared-ring generation to helper owner "
                    "before Reset (slot=%d hr=0x%08x)",
                    i, (unsigned)openHr);
                return false;
            }
            directSharedProducerTextures9[i] = retirementOwner;
        }

        directSharedUsesHelperProducer = true;
        HookLogImportant("DX9: Native shared-ring generation handed to helper owner for nonblocking Reset");
        return true;
    }

    void ReleaseGameDeviceResourcesForReset() {
        ResetDirectD3D9SharedRingPendingState();
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
            if (stagingRenderSurfaces[i]) {
                stagingRenderSurfaces[i]->Release();
                stagingRenderSurfaces[i] = nullptr;
            }
            if (stagingTextures[i]) {
                stagingTextures[i]->Release();
                stagingTextures[i] = nullptr;
            }
            if (shmemSurfaces[i]) {
                shmemSurfaces[i]->Release();
                shmemSurfaces[i] = nullptr;
            }
            if (shmemQueries[i]) {
                shmemQueries[i]->Release();
                shmemQueries[i] = nullptr;
            }
            shmemTextureReady[i] = false;
            stagingTimestampQpc[i] = 0;
        }

        auto releaseObject = [](auto*& object) {
            if (object) {
                object->Release();
                object = nullptr;
            }
        };
        releaseObject(copySurface);
        releaseObject(zeroCopyQuery);
        releaseObject(sharedTexture9);
        releaseObject(gdiSurface);
        releaseObject(gdiTexture);
        releaseObject(gdiCopySurfaces[0]);
        releaseObject(gdiCopySurfaces[1]);
        releaseObject(d3d11SharedTexture);
        releaseObject(d3d9DeviceEx);
        releaseObject(overlayTexture9);
        releaseObject(d3d9Device);

        for (auto& q : prerenderQueries) {
            if (q.query)
                q.query->Release();
        }
        prerenderQueries.clear();
        prerenderIdx = 0;

        sharedHandle9 = NULL;
        useDirectD3D9SharedRing = false;
        useGDIInterop = false;
        gdiDirectSharedRing = false;
        gdiHasPrevFrame = false;
        gdiWriteIdx = 0;
        gdiLastCaptureQpc = 0;
        gdiBufferTimestampQpc[0] = 0;
        gdiBufferTimestampQpc[1] = 0;
        gdiBufferBusy[0].store(false, std::memory_order_relaxed);
        gdiBufferBusy[1].store(false, std::memory_order_relaxed);
        allowAsyncD3D9WorkerCapture = false;
        stagingWriteIdx = 0;
        stagingReadIdx = 0;
        stagingPending = 0;
        stagingPendingBlitIdx = -1;
        stagingUseGpuIntermediate = false;
        stagingLastSubmitQpc = 0;
        useD3D11Staging = false;
        g_DX9StagingCaptureActive.store(false, std::memory_order_release);
        d3d9Format = D3DFMT_UNKNOWN;
        d3d9SharedFormat = D3DFMT_UNKNOWN;
        initialized = false;
        firstFrame = true;
    }

    bool PrepareForDeviceReset() {
        {
            std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
            initialized = false;
            captureThreadShutdown.store(true, std::memory_order_release);
            if (captureEvent)
                SetEvent(captureEvent);
        }
        StopCaptureThread();

        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);

        SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        if (!HasPublishedGeneration() || !HasOutstandingCaptureFrameLeases(sharedMem)) {
            CleanupDX9(false, true);
            return true;
        }

        if (!EnsureNativeDirectRingRetirementOwner()) {
            initialized = false;
            generationResetPending = true;
            HookLogImportant(
                "DX9: Deferring device Reset because the leased native shared generation could not "
                "be handed to an independent owner");
            return false;
        }

        ReleaseGameDeviceResourcesForReset();
        generationResetPending = true;
        HookLogImportant("DX9: Retaining shared transport generation across Reset until frame leases drain");
        return true;
    }

    bool CleanupDX9(bool permanentFailure = false, bool force = false) {
        std::unique_lock<std::recursive_mutex> captureLock(captureMutex);
        SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        if (!force && HasPublishedGeneration() && HasOutstandingCaptureFrameLeases(sharedMem)) {
            static std::atomic<int> s_generationLeaseLogCount{0};
            if (s_generationLeaseLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
                HookLog("DX9: Deferring capture resource cleanup while old frame leases are outstanding");
            }
            return false;
        }

        initialized = false;
        captureThreadShutdown.store(true, std::memory_order_release);
        if (captureEvent)
            SetEvent(captureEvent);
        captureLock.unlock();
        StopCaptureThread();
        captureLock.lock();
        // Close shared handles first via base class
        CleanupSharedHandles();

        ReleaseSharedTextureRing();
        ReleaseDirectD3D9SharedRing();

        if (fence) {
            fence->Release();
            fence = nullptr;
        }
        if (context4) {
            context4->Release();
            context4 = nullptr;
        }

        if (copySurface) {
            copySurface->Release();
            copySurface = nullptr;
        }
        if (zeroCopyQuery) {
            zeroCopyQuery->Release();
            zeroCopyQuery = nullptr;
        }
        if (sharedTexture9) {
            sharedTexture9->Release();
            sharedTexture9 = nullptr;
        }
        sharedHandle9 = NULL;
        useDirectD3D9SharedRing = false;

        if (gdiSurface) {
            gdiSurface->Release();
            gdiSurface = nullptr;
        }
        if (gdiTexture) {
            gdiTexture->Release();
            gdiTexture = nullptr;
        }
        if (gdiCopySurfaces[0]) {
            gdiCopySurfaces[0]->Release();
            gdiCopySurfaces[0] = nullptr;
        }
        if (gdiCopySurfaces[1]) {
            gdiCopySurfaces[1]->Release();
            gdiCopySurfaces[1] = nullptr;
        }
        useGDIInterop = false;
        gdiDirectSharedRing = false;
        gdiHasPrevFrame = false;
        gdiWriteIdx = 0;
        gdiLastCaptureQpc = 0;
        gdiBufferTimestampQpc[0] = 0;
        gdiBufferTimestampQpc[1] = 0;
        gdiBufferBusy[0].store(false, std::memory_order_relaxed);
        gdiBufferBusy[1].store(false, std::memory_order_relaxed);
        allowAsyncD3D9WorkerCapture = false;

        if (d3d11SharedTexture) {
            d3d11SharedTexture->Release();
            d3d11SharedTexture = nullptr;
        }
        if (d3d11Context) {
            d3d11Context->Release();
            d3d11Context = nullptr;
        }
        if (d3d11Device) {
            d3d11Device->Release();
            d3d11Device = nullptr;
        }
        if (d3d9DeviceEx) {
            d3d9DeviceEx->Release();
            d3d9DeviceEx = nullptr;
        }
        if (overlayTexture9) {
            overlayTexture9->Release();
            overlayTexture9 = nullptr;
        }

        if (d3d9Device) {
            d3d9Device->Release();
            d3d9Device = nullptr;
        }

        if (g_OverlayAdapter.IsInitialized()) {
            g_OverlayAdapter.Shutdown();
        }

        d3d9Format = D3DFMT_UNKNOWN;
        d3d9SharedFormat = D3DFMT_UNKNOWN;
        initialized = false;
        generationResetPending = false;
        useFences = false;
        fenceValue = 0;
        firstFrame = true;

        // Cleanup shmem resources
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            if (stagingRenderSurfaces[i]) {
                stagingRenderSurfaces[i]->Release();
                stagingRenderSurfaces[i] = nullptr;
            }
            if (stagingTextures[i]) {
                stagingTextures[i]->Release();
                stagingTextures[i] = nullptr;
            }
            if (shmemSurfaces[i]) {
                shmemSurfaces[i]->Release();
                shmemSurfaces[i] = nullptr;
            }
            if (shmemQueries[i]) {
                shmemQueries[i]->Release();
                shmemQueries[i] = nullptr;
            }
            shmemTextureReady[i] = false;
            stagingTimestampQpc[i] = 0;
        }

        stagingWriteIdx = 0;
        stagingReadIdx = 0;
        stagingPending = 0;
        stagingUseGpuIntermediate = false;
        stagingLastSubmitQpc = 0;
        useD3D11Staging = false;
        g_DX9StagingCaptureActive.store(false, std::memory_order_release);

        for (auto& q : prerenderQueries) {
            if (q.query)
                q.query->Release();
        }
        prerenderQueries.clear();
        prerenderIdx = 0;

        if (permanentFailure) {
            initializationFailed = true;
        } else {
            initializationFailed = false;  // Allow retry if it wasn't a permanent fail
        }
        return true;
    }

    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
        // Implemented in Init
    }

    // Set up GDI interop: D3D9 render target + D3D11 GDI-compatible texture.
    // On WDDM 2.0+ (Win10+), BitBlt between GPU-backed DCs uses the GPU blitter.
    bool SetupGDIInterop(IDirect3DDevice9* device) {
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

    // Complete GDI interop transfer from a specific D3D9 RT to the published D3D11 ring.
    // The surface should have been written to in a PREVIOUS frame so GetDC won't stall.
    void CompleteGDIInteropCapture(IDirect3DSurface9* srcSurface, int64_t frameTimestampQpc) {
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
