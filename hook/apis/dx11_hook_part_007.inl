                }
            } else {
                EarlyLog("DX11: Warning - Fence creation failed (hr=0x%08x)", hr);
            }
            device5->Release();
        } else {
            EarlyLog("DX11: ID3D11Device5 not available (DX11.3 required for Fences)");
        }

        bool success = true;
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            HRESULT hr = captureDevice->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
            if (SUCCEEDED(hr)) {
                IDXGIResource1* pResource1 = NULL;
                // Use IDXGIResource1 for NT Handles
                if (SUCCEEDED(sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&pResource1)))) {
                    HANDLE hTemp = NULL;
                    hr = pResource1->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                        NULL, &hTemp);
                    sharedTextureHandles[i].store(hTemp, std::memory_order_release);
                    sharedTextureHandlesAreNt[i] = SUCCEEDED(hr) && hTemp != NULL;
                    pResource1->Release();
                } else {
                    // Fallback to legacy KMT if Resource1 not valid (should not happen
                    // with NTHANDLE flag) But if we requested NTHANDLE, GetSharedHandle
                    // (KMT) will fail on some drivers. We should log this specific
                    // failure path.
                    IDXGIResource* pResource = NULL;
                    if (SUCCEEDED(sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&pResource))) &&
                        (texDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED)) {
                        HANDLE hTemp = NULL;
                        pResource->GetSharedHandle(&hTemp);
                        sharedTextureHandles[i].store(hTemp, std::memory_order_release);
                        sharedTextureHandlesAreNt[i] = false;
                        pResource->Release();
                        EarlyLog(
                            "DX11: Warning - Fallback to KMT handle for KeyedMutex "
                            "(NT Handle QI failed)");
                    } else {
                        EarlyLog(
                            "DX11: Error - Failed to get any shared handle interface "
                            "for texture %d",
                            i);
                    }
                }

                if (sharedTextureHandles[i].load() == NULL) {
                    EarlyLog("DX11: Critical - Shared Handle is NULL for texture %d", i);
                    success = false;
                } else {
                    EarlyLog("DX11: Created Texture %d Handle %p", i, sharedTextureHandles[i].load());
                }

            } else {
                // Fallback to legacy shared if NT Handle not supported
                if (hr == E_INVALIDARG && (texDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
                    EarlyLog(
                        "DX11: NT Handle not supported, falling back to legacy "
                        "shared textures");
                    texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                    i--;  // Retry this index
                    continue;
                }

                success = false;
                HookLog("%s: Failed to create texture %d (hr=0x%08x)", isDX10Mode ? "DX10" : "DX11", i, hr);
            }
        }

        if (success) {
            // For DXVK mode: open each system D3D11 texture in DXVK's device.
            // The copy at capture time will use DXVK's context to copy the game
            // backbuffer into the DXVK-imported texture. The encoder opens the
            // original system D3D11 NT handles normally.
            if (isDXVKMode) {
                ID3D11Device1* dxvkDevice1 = nullptr;
                D3D11InternalIdentityProbeScope identityProbeScope;
                if (SUCCEEDED(cachedDevice->QueryInterface(IID_PPV_ARGS(&dxvkDevice1)))) {
                    for (int i = 0; i < CAPTURE_TEXTURE_COUNT && success; i++) {
                        HANDLE ntHandle = sharedTextureHandles[i].load();
                        if (!ntHandle) {
                            EarlyLog("DX11-DXVK: NT handle %d is NULL, cannot import", i);
                            success = false;
                            break;
                        }
                        HRESULT hr = dxvkDevice1->OpenSharedResource1(ntHandle, IID_PPV_ARGS(&dxvkImportedTextures[i]));
                        if (FAILED(hr)) {
                            EarlyLog("DX11-DXVK: Failed to open texture %d in DXVK device (hr=0x%08x)", i, hr);
                            success = false;
                        }
                    }
                    dxvkDevice1->Release();
                } else {
                    EarlyLog("DX11-DXVK: DXVK device doesn't support ID3D11Device1 - disabling DXVK mode");
                    success = false;
                }
            }
        }

        if (success) {
            // Create GPU synchronization queries for each texture
            // These queries are used to ensure CopyResource completes before reusing
            // the texture. Skip for DXVK: queries live in system D3D11 but context
            // is DXVK's - they can't be used cross-device.
            if (!isDXVKMode) {
                D3D11_QUERY_DESC queryDesc = {};
                queryDesc.Query = D3D11_QUERY_EVENT;
                queryDesc.MiscFlags = 0;

                for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
                    HRESULT queryHr = captureDevice->CreateQuery(&queryDesc, &copyQueries[i]);
                    if (FAILED(queryHr)) {
                        HookLog("%s: Failed to create copy query %d (hr=0x%08x)", isDX10Mode ? "DX10" : "DX11", i,
                                queryHr);
                        copyQueries[i] = nullptr;
                    }
                }
            }

            if (g_IPC) {
                PublishToSharedMemory(g_IPC);
            }
            initialized = true;
            HookLogImportant("%s Capture Initialized: %dx%d (Fence: %s, Queries: %s, DXVK: %s)",
                             isDX10Mode ? "DX10" : "DX11", width, height, useFences ? "ON" : "OFF",
                             (copyQueries[0] != nullptr) ? "ON" : "OFF", isDXVKMode ? "ON" : "OFF");
        } else {
            EarlyLog("%s Capture Init FAILED (success=false)", isDX10Mode ? "DX10" : "DX11");
            Cleanup();
        }
    }

    // Wait for a specific query to complete (with timeout)
    bool WaitForCopy(ID3D11DeviceContext* context, int idx, DWORD timeoutMs = 10) {
        if (!copyQueries[idx])
            return true;  // No query = assume complete
        DWORD start = GetTickCount();
        BOOL data = FALSE;
        while (context->GetData(copyQueries[idx], &data, sizeof(data), 0) == S_FALSE) {
            if (GetTickCount() - start > timeoutMs) {
                return false;  // Timeout
            }
            SwitchToThread();  // Yield CPU
        }
        return true;
    }

    // Get the context to use for DX11 capture operations
    ID3D11DeviceContext* GetCaptureContext() {
        return cachedContext;
    }

    // Capture a frame from the swapchain to shared texture
    // Returns true if frame was captured, false if skipped/dropped
    bool CaptureFrame(IDXGISwapChain* swapChain) {
        std::unique_lock<std::recursive_mutex> captureLock(captureMutex, std::try_to_lock);
        if (!captureLock.owns_lock()) {
            if (g_IPC && g_IPC->GetSharedMem()) {
                g_IPC->GetSharedMem()->runtimeState.injectProducerCaptureLockDrops.fetch_add(1,
                                                                                             std::memory_order_relaxed);
            }
            static std::atomic<int> s_contentionLogCount{0};
            if (s_contentionLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
                HookLog("DX11Capture: Skipping concurrent capture while another Present/cleanup owns resources");
            }
            return false;
        }

        static std::atomic<int> s_captureFrameCount{0};
        int frameNum = s_captureFrameCount.fetch_add(1, std::memory_order_relaxed) + 1;

        if (!swapChain) {
            HookLog("DX11Capture: [%d] swapChain is null", frameNum);
            return false;
        }

        // Check if we should throttle capture (encoder is falling behind)
        if (g_IPC && g_IPC->GetSharedMem()) {
            if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
                return false;
            }
        }

        // Initialize capture if needed (GetDevice only called during init to avoid per-frame COM overhead)
        if (!initialized) {
            HookLog("DX11Capture: [%d] Not initialized, initializing...", frameNum);
            if (generationResetPending) {
                SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
                if (HasOutstandingCaptureFrameLeases(sharedMem)) {
                    static std::atomic<int> s_generationLeaseLogCount{0};
                    if (s_generationLeaseLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
                        HookLog("DX11Capture: [%d] Waiting for old frame leases before rebuilding resources", frameNum);
                    }
                    return false;
                }
                Cleanup();
            }
            const DXGIShared::APIType swapChainApi = DetectSwapChainAPITypeForDX11Hook(swapChain);
            if (swapChainApi == DXGIShared::APIType::D3D10) {
                if (!InitDX10(swapChain)) {
                    HookLog("DX11Capture: [%d] InitDX10 failed", frameNum);
                    return false;
                }
                Init(nullptr, swapChain);
            } else if (swapChainApi == DXGIShared::APIType::D3D11) {
                ID3D11Device* device = nullptr;
                HRESULT initHr = swapChain->GetDevice(IID_PPV_ARGS(&device));
                if (FAILED(initHr) || !device) {
                    HookLog("DX11Capture: [%d] GetDevice failed hr=0x%08X", frameNum, initHr);
                    return false;
                }
                Init(device, swapChain);
                device->Release();
            } else {
                HookLog("DX11Capture: [%d] Unsupported swapchain API %s during init", frameNum,
                        GetDX11HookBaseAPIName(swapChainApi));
                return false;
            }
        }

        if (!initialized) {
            HookLog("DX11Capture: [%d] Still not initialized after Init", frameNum);
            return false;
        }

        if (isDX10Mode) {
            if (!cachedDevice10) {
                HookLog("DX10Capture: [%d] cachedDevice10 is null", frameNum);
                return false;
            }

            ID3D10Texture2D* backbuffer10 = nullptr;
            UINT bufferIndex = ResolveDX11BackBufferIndex(swapChain);
            HRESULT hr = swapChain->GetBuffer(bufferIndex, __uuidof(ID3D10Texture2D), (void**)&backbuffer10);
            if (FAILED(hr) || !backbuffer10) {
                HookLog("DX10Capture: [%d] GetBuffer(%u) failed hr=0x%08X", frameNum, bufferIndex, hr);
                return false;
            }

            int writeIdx = writeIndex % CAPTURE_TEXTURE_COUNT;
            SharedMemoryLayout* captureSharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
            uint32_t cpuBusySlots = 0;
            uint32_t gpuBusySlots = 0;
            writeIdx = FindAvailableCaptureTextureSlotIf(
                captureSharedMem, writeIdx, CAPTURE_TEXTURE_COUNT,
                [&](int32_t candidate) {
                    ID3D10Query* query = copyQueries10[candidate];
                    if (!query)
                        return true;
                    BOOL complete = FALSE;
                    return query->GetData(&complete, sizeof(complete), D3D10_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
                },
                &cpuBusySlots, &gpuBusySlots);
            if (writeIdx < 0) {
                if (captureSharedMem) {
                    if (cpuBusySlots != 0)
                        captureSharedMem->runtimeState.injectProducerCpuLeaseBusyDrops.fetch_add(
                            1, std::memory_order_relaxed);
                    if (gpuBusySlots != 0)
                        captureSharedMem->runtimeState.injectProducerGpuBusyDrops.fetch_add(1,
                                                                                            std::memory_order_relaxed);
                }
                droppedFrames.fetch_add(1, std::memory_order_relaxed);
                backbuffer10->Release();
                return false;
            }
            writeIndex.store(writeIdx, std::memory_order_relaxed);
            if (frameNum <= 20 || frameNum % 60 == 0) {
                HookLog("DX10Capture: [%d] Copying to texture %d (writeIndex=%d)", frameNum, writeIdx,
                        writeIndex.load());
            }

            LARGE_INTEGER qpc;
            QueryPerformanceCounter(&qpc);
            int64_t timestamp = qpc.QuadPart;

            cachedDevice10->CopyResource(sharedTextures10[writeIdx], backbuffer10);
            backbuffer10->Release();

            if (copyQueries10[writeIdx]) {
                copyQueries10[writeIdx]->End();
            }

            // D3D10->D3D11 shared-texture interop requires a producer-side Flush()
            // so the media process sees the latest contents of the shared surface.
            cachedDevice10->Flush();

            if (g_IPC) {
                SignalFrameReady(g_IPC, writeIdx, timestamp, 0);
                if (frameNum <= 10) {
                    HookLog("DX10Capture: [%d] Signaled frame ready via IPC (texture=%d)", frameNum, writeIdx);
                }
            } else {
                HookLog("DX10Capture: [%d] g_IPC is NULL - cannot signal frame", frameNum);
            }

            AdvanceWriteIndex();
            return true;
        }

        // Get immediate context for copy
        ID3D11DeviceContext* context = GetCaptureContext();
        if (!context) {
            HookLog("DX11Capture: [%d] GetCaptureContext returned null", frameNum);
            return false;
        }

        ID3D11Texture2D* backbuffer = nullptr;

        // When capture is intentionally ordered after overlay, prefer the RTV
        // resource that overlay rendered to on this frame.
        if (!isDX10Mode && g_CaptureUsesOverlayRTV && g_mainRenderTargetView) {
            ID3D11Resource* rtResource = nullptr;
            g_mainRenderTargetView->GetResource(&rtResource);
            if (rtResource) {
                rtResource->QueryInterface(IID_PPV_ARGS(&backbuffer));
                rtResource->Release();
            }
        }

        // Fallback: resolve backbuffer index directly from swapchain.
        UINT bufferIndex = 0;
        if (!backbuffer) {
            bufferIndex = ResolveDX11BackBufferIndex(swapChain);
            HRESULT hr = swapChain->GetBuffer(bufferIndex, IID_PPV_ARGS(&backbuffer));
            if (FAILED(hr) || !backbuffer) {
                HookLog("DX11Capture: [%d] GetBuffer(%u) failed hr=0x%08X", frameNum, bufferIndex, hr);
                return false;
            }
        }

        // Determine which texture slot to write to
        int writeIdx = writeIndex % CAPTURE_TEXTURE_COUNT;
        SharedMemoryLayout* captureSharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        const UINT64 completedFenceValue = (useFences && fence) ? fence->GetCompletedValue() : 0;
        if (completedFenceValue == UINT64_MAX) {
            HookLog("DX11Capture: Producer fence reported device removal");
            backbuffer->Release();
            return false;
        }
        uint32_t cpuBusySlots = 0;
        uint32_t gpuBusySlots = 0;
        writeIdx = FindAvailableCaptureTextureSlotIf(
            captureSharedMem, writeIdx, CAPTURE_TEXTURE_COUNT,
            [&](int32_t candidate) {
                if (useFences && fence) {
                    const UINT64 requiredValue = slotFenceValues[candidate];
                    return requiredValue == 0 || completedFenceValue >= requiredValue;
                }
                ID3D11Query* query = copyQueries[candidate];
                if (!query)
                    return true;
                BOOL complete = FALSE;
                return context->GetData(query, &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            },
            &cpuBusySlots, &gpuBusySlots);
        if (writeIdx < 0) {
            if (captureSharedMem) {
                if (cpuBusySlots != 0)
                    captureSharedMem->runtimeState.injectProducerCpuLeaseBusyDrops.fetch_add(1,
                                                                                             std::memory_order_relaxed);
                if (gpuBusySlots != 0)
                    captureSharedMem->runtimeState.injectProducerGpuBusyDrops.fetch_add(1, std::memory_order_relaxed);
            }
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            backbuffer->Release();
            return false;
        }
        writeIndex.store(writeIdx, std::memory_order_relaxed);

        if (frameNum <= 20 || frameNum % 60 == 0) {
            HookLog("DX11Capture: [%d] Copying to texture %d (writeIndex=%d)", frameNum, writeIdx, writeIndex.load());
        }

        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        int64_t timestamp = qpc.QuadPart;

        // Perform GPU copy: backbuffer -> shared texture
        // For DXVK: copy into the DXVK-imported texture (system D3D11-owned,
        // imported into DXVK's device). The encoder opens the system D3D11 NT handle.
        ID3D11Texture2D* copyTarget =
            (isDXVKMode && dxvkImportedTextures[writeIdx]) ? dxvkImportedTextures[writeIdx] : sharedTextures[writeIdx];
        context->CopyResource(copyTarget, backbuffer);
        backbuffer->Release();

        // Issue query for GPU completion tracking
        if (copyQueries[writeIdx]) {
            context->End(copyQueries[writeIdx]);
        }

        // Signal fence if using D3D11.3 fences
        uint64_t currentFenceValue = 0;
        if (useFences && fence && context4) {
            currentFenceValue = ++fenceValue;
            const HRESULT signalHr = context4->Signal(fence, currentFenceValue);
            if (FAILED(signalHr)) {
                HookLog(
                    "DX11Capture: Fence Signal failed value=%llu hr=0x%08X; falling back to implicit shared "
                    "synchronization",
                    static_cast<unsigned long long>(currentFenceValue), signalHr);
                useFences = false;
                currentFenceValue = 0;
                context->Flush();
                if (cachedDevice && FAILED(cachedDevice->GetDeviceRemovedReason())) {
                    return false;
                }
            } else {
                slotFenceValues[writeIdx] = currentFenceValue;
            }
        } else {
            // A legacy shared texture has no explicit cross-process completion
            // primitive. Submit the copy before publishing its ring entry so the
            // media device cannot indefinitely observe an older texture version.
            context->Flush();
        }

        // Signal frame ready to media process via IPC
        // Note: EnqueueFrame to internal pendingRing is skipped for inject mode
        // since SignalFrameReady writes directly to the shared memory ring buffer
        if (g_IPC) {
            SignalFrameReady(g_IPC, writeIdx, timestamp, currentFenceValue);
            if (frameNum <= 10) {
                HookLog("DX11Capture: [%d] Signaled frame ready via IPC (texture=%d)", frameNum, writeIdx);
            }
        } else {
            HookLog("DX11Capture: [%d] g_IPC is NULL - cannot signal frame", frameNum);
        }

        // Advance write index
        AdvanceWriteIndex();

        return true;
    }
};

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
static DX11Capture g_DX11Capture;

static OverlayConfig GetActiveDX11OverlayConfig(SharedMemoryLayout* shm) {
    OverlayConfig cfg{};
    cfg.captureIncludeOverlay = true;
    cfg.screenshotIncludeOverlay = true;
    if (shm) {
        cfg = shm->ReadOverlayConfig();
    }
    return cfg;
}

static void CaptureDX11Screenshot(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm, uint64_t requestId) {
    bool queued = false;
    ID3D11Texture2D* backbuffer = nullptr;
    UINT bbIdx = ResolveDX11BackBufferIndex(pSwapChain);
    pSwapChain->GetBuffer(bbIdx, IID_PPV_ARGS(&backbuffer));
    if (backbuffer) {
        ID3D11Device* device = nullptr;
        backbuffer->GetDevice(&device);
        if (device) {
            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);
            if (context) {
                D3D11_TEXTURE2D_DESC textureDesc{};
                backbuffer->GetDesc(&textureDesc);
                const auto presentationEncoding =
                    DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, textureDesc.Format);
                queued = SaveD3D11TextureAsScreenshotRaw(device, context, backbuffer, shm, requestId,
                                                         presentationEncoding);
                context->Release();
            }
            device->Release();
        }
        backbuffer->Release();
    }
    if (!queued)
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);
}

static void CaptureDX10Screenshot(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm, uint64_t requestId) {
    ID3D10Device* device = nullptr;
    if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&device))) {
        ID3D10Device1* device10_1 = nullptr;
        if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&device10_1))) {
            CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_NOT_SUPPORTED);
            return;
        }
        device = device10_1;
    }

    bool queued = false;
    ID3D10Texture2D* backbuffer = nullptr;
    UINT bbIdx = ResolveDX11BackBufferIndex(pSwapChain);
    if (SUCCEEDED(pSwapChain->GetBuffer(bbIdx, __uuidof(ID3D10Texture2D), (void**)&backbuffer))) {
        D3D10_TEXTURE2D_DESC bbDesc;
        backbuffer->GetDesc(&bbDesc);

        D3D10_TEXTURE2D_DESC stagingDesc = bbDesc;
        stagingDesc.Usage = D3D10_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;

        ID3D10Texture2D* staging = nullptr;
        if (SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
            device->CopyResource(staging, backbuffer);
            D3D10_MAPPED_TEXTURE2D mapped;
            if (SUCCEEDED(staging->Map(0, D3D10_MAP_READ, 0, &mapped))) {
                ScreenshotPixelFormat pixelFormat = ScreenshotPixelFormat::BGRA8;
                ScreenshotColorEncoding colorEncoding = ScreenshotColorEncoding::SRGB;
                const auto presentationEncoding =
                    DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, bbDesc.Format);
                if (bbDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || bbDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                    pixelFormat = ScreenshotPixelFormat::RGBA8;
                } else if (bbDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                    pixelFormat = ScreenshotPixelFormat::R10G10B10A2;
                    colorEncoding = presentationEncoding == ce::presentation_color::Encoding::Hdr10Pq
                                        ? ScreenshotColorEncoding::BT2020_PQ
                                        : ScreenshotColorEncoding::BT709_G22;
                } else if (bbDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    pixelFormat = ScreenshotPixelFormat::RGBA16F;
                    colorEncoding = ScreenshotColorEncoding::LinearScRGB;
                }
                if (presentationEncoding != ce::presentation_color::Encoding::Unsupported) {
                    queued = QueueScreenshotPixels(shm, requestId, static_cast<const uint8_t*>(mapped.pData),
                                                   bbDesc.Width, bbDesc.Height, mapped.RowPitch, pixelFormat,
                                                   colorEncoding);
                }
                staging->Unmap(0);
            }
            staging->Release();
        }
        backbuffer->Release();
    }

    device->Release();
    if (!queued)
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);
}

static void CaptureRequestedDX11Screenshot(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm, uint64_t requestId) {
    if (!shm || requestId == 0)
        return;

    const DXGIShared::APIType swapChainApi = DetectSwapChainAPITypeForDX11Hook(pSwapChain);
    if (swapChainApi == DXGIShared::APIType::D3D10) {
        CaptureDX10Screenshot(pSwapChain, shm, requestId);
        return;
    }

    if (swapChainApi != DXGIShared::APIType::D3D11) {
        HookLog("DX11 Screenshot: Unsupported swapchain API %s", GetDX11HookBaseAPIName(swapChainApi));
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_NOT_SUPPORTED);
        return;
    }

    CaptureDX11Screenshot(pSwapChain, shm, requestId);
}

static void ProcessDX11FrameWithOverlayOrdering(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return;

    UINT frameBufferIndex = ResolveDX11BackBufferIndex(pSwapChain);
    g_ForcedCaptureBackBufferIndex = static_cast<int>(frameBufferIndex);
    auto indexGuard = ce::make_scope_guard([]() { g_ForcedCaptureBackBufferIndex = -1; });

    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    DXGI_SWAP_CHAIN_DESC swapChainDesc{};
    if (shm) {
        auto presentationEncoding = ce::presentation_color::Encoding::Unsupported;
        if (SUCCEEDED(pSwapChain->GetDesc(&swapChainDesc))) {
            presentationEncoding =
                DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, swapChainDesc.BufferDesc.Format);
        }
        shm->SetIsHDR(ce::presentation_color::IsHDR(presentationEncoding));
    }
    OverlayConfig overlayCfg = GetActiveDX11OverlayConfig(shm);
    const bool shouldDrawOverlay = shm && overlayCfg.showOverlay;
    const bool captureAfterOverlay = shouldDrawOverlay && overlayCfg.captureIncludeOverlay;
    const uint64_t screenshotRequestId = GetPendingScreenshotRequestId(shm);
    const bool screenshotRequested = screenshotRequestId != 0;
    const bool screenshotAfterOverlay = shouldDrawOverlay && overlayCfg.screenshotIncludeOverlay;

    auto doCapture = [&](bool afterOverlay) {
        if (g_IPC && g_IPC->IsRecording() && !ShouldSkipCaptureForTargetCadence(shm, "DX11")) {
            g_CaptureUsesOverlayRTV = afterOverlay;
            auto captureGuard = ce::make_scope_guard([]() { g_CaptureUsesOverlayRTV = false; });
            g_DX11Capture.CaptureFrame(pSwapChain);
        }
    };

    auto doScreenshot = [&]() {
        if (screenshotRequested) {
            CaptureRequestedDX11Screenshot(pSwapChain, shm, screenshotRequestId);
        }
    };

    if (!captureAfterOverlay) {
        doCapture(false);
    }
    if (screenshotRequested && !screenshotAfterOverlay) {
        doScreenshot();
    }
    if (shouldDrawOverlay) {
        DrawDX11Overlay(pSwapChain);
    }
    if (captureAfterOverlay) {
        doCapture(true);
    }
    if (screenshotRequested && screenshotAfterOverlay) {
        doScreenshot();
    }
}

// Called from DXGI SwapChain wrapper for frame capture (wrapper-only
// architecture)
void DX11_ProcessFrameExternal(IDXGISwapChain* pSwapChain) {
    HandleDX11ProcessFrame(pSwapChain, true);
}

static void DrawDX10Overlay(IDXGISwapChain* pSwapChain, HWND currentHwnd, int frameCount) {
    ID3D10Device* device = nullptr;
    if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&device))) {
        if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&device))) {
            static std::atomic<int> s_getDeviceFailureLogCount{0};
            const int logCount = s_getDeviceFailureLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 5) {
                HookLogImportant("DX10: DrawOverlay failed to get ID3D10Device/Device1 from swapchain %p", pSwapChain);
            }
            return;
        }
    }

    // Capture/Hook on the real device seen in Present
    static ID3D10Device* s_HookedDevice = nullptr;
    static IDXGISwapChain* s_LastDX10OverlaySwapChain = nullptr;
    static UINT s_LastDX10OverlayBufferIndex = 0xFFFFFFFFu;
    static HWND s_LastDX10OverlayHwnd = nullptr;
    if (s_HookedDevice != device) {
        if (g_mainRenderTargetView10) {
            g_mainRenderTargetView10->Release();
            g_mainRenderTargetView10 = nullptr;
        }
        s_LastDX10OverlaySwapChain = nullptr;
        s_LastDX10OverlayBufferIndex = 0xFFFFFFFFu;
        s_LastDX10OverlayHwnd = currentHwnd;
        if (g_OverlayAdapter.IsInitialized()) {
            HookLogImportant("DX10: Device changed, reinitializing overlay backend");
            g_OverlayAdapter.Shutdown();
        }
        // Initialize System Metrics
        IDXGIDevice* dxgiDevice = nullptr;
