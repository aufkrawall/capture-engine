                    if (shmemQueries[consumeIdx]) {
                        LARGE_INTEGER qwStart, qwEnd;
                        QueryPerformanceCounter(&qwStart);
                        HRESULT queryHr = shmemQueries[consumeIdx]->GetData(nullptr, 0, 0);
                        QueryPerformanceCounter(&qwEnd);
                        stagingQueryWaitUs =
                            static_cast<int32_t>(((qwEnd.QuadPart - qwStart.QuadPart) * 1000000) / qpcFreq);

                        if (queryHr == S_FALSE) {
                            queryReady = false;  // Not ready yet - don't block Present.
                        } else if (FAILED(queryHr)) {
                            shmemTextureReady[consumeIdx] = false;
                            stagingReadIdx = (stagingReadIdx + 1) % CAPTURE_TEXTURE_COUNT;
                            stagingPending--;
                            queryReady = false;
                        }
                    }

                    if (queryReady) {
                        if (captureThreadRunning.load(std::memory_order_acquire)) {
                            // Async path: enqueue to background thread for LockRect +
                            // UpdateSubresource. This keeps the render thread overhead to
                            // just the query check (~0us).
                            shmemTextureReady[consumeIdx] = false;
                            stagingReadIdx = (stagingReadIdx + 1) % CAPTURE_TEXTURE_COUNT;
                            stagingPending--;
                            EnqueueFrame(stagingTimestampQpc[consumeIdx], 0, consumeIdx, nullptr);
                        } else {
                            // Inline fallback: process on render thread
                            LARGE_INTEGER lockStart, lockEnd;
                            QueryPerformanceCounter(&lockStart);

                            D3DLOCKED_RECT rect;
                            DWORD lockFlags = D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK;
                            HRESULT lockHr = shmemSurfaces[consumeIdx]->LockRect(&rect, NULL, lockFlags);

                            QueryPerformanceCounter(&lockEnd);
                            stagingLockRectUs =
                                static_cast<int32_t>(((lockEnd.QuadPart - lockStart.QuadPart) * 1000000) / qpcFreq);

                            if (SUCCEEDED(lockHr)) {
                                LARGE_INTEGER uploadStart, uploadEnd;
                                QueryPerformanceCounter(&uploadStart);

                                const int idx = AcquirePublishedTextureSlot();
                                const bool canUpload = idx >= 0 && d3d11Context && sharedTextures[idx];
                                if (canUpload) {
                                    d3d11Context->UpdateSubresource(sharedTextures[idx], 0, NULL, rect.pBits,
                                                                    rect.Pitch, 0);
                                }
                                shmemSurfaces[consumeIdx]->UnlockRect();

                                QueryPerformanceCounter(&uploadEnd);
                                stagingD3D11UploadUs = static_cast<int32_t>(
                                    ((uploadEnd.QuadPart - uploadStart.QuadPart) * 1000000) / qpcFreq);

                                shmemTextureReady[consumeIdx] = false;
                                stagingReadIdx = (stagingReadIdx + 1) % CAPTURE_TEXTURE_COUNT;
                                stagingPending--;

                                if (canUpload) {
                                    SignalPublishedTextureFrame(idx, stagingTimestampQpc[consumeIdx]);
                                    AdvanceWriteIndex();
                                } else if (idx < 0) {
                                    droppedFrames.fetch_add(1, std::memory_order_relaxed);
                                }
                            } else {
                                shmemTextureReady[consumeIdx] = false;
                                stagingReadIdx = (stagingReadIdx + 1) % CAPTURE_TEXTURE_COUNT;
                                stagingPending--;
                            }
                        }
                    }
                }
            }

            // === PHASE 2: SUBMIT new readback ===
            // StretchRect + GetRenderTargetData batched in same GPU command stream.
            // Query covers both operations, so consume only needs LockRect.
            bool canSubmit = (stagingPending < CAPTURE_TEXTURE_COUNT - 1);

            // Rate-limit to capture FPS target
            if (canSubmit) {
                int64_t targetSubmitIntervalUs = 0;
                if (g_IPC) {
                    SharedMemoryLayout* shm = g_IPC->GetSharedMem();
                    if (shm) {
                        const int captureFps = shm->fpsLimiter.GetCaptureFps();
                        if (captureFps > 0) {
                            targetSubmitIntervalUs = 1000000LL / (int64_t)captureFps;
                        }
                    }
                }

                if (targetSubmitIntervalUs > 0 && stagingLastSubmitQpc != 0) {
                    const int64_t sinceLastUs = ((qpc.QuadPart - stagingLastSubmitQpc) * 1000000) / qpcFreq;
                    if (sinceLastUs < targetSubmitIntervalUs) {
                        canSubmit = false;
                    }
                }
            }

            if (canSubmit) {
                const int submitIdx = stagingWriteIdx;

                if (stagingUseGpuIntermediate && stagingRenderSurfaces[submitIdx]) {
                    // GPU blit: backBuffer -> intermediate render target (fast, no DMA)
                    LARGE_INTEGER stretchStart, stretchEnd;
                    QueryPerformanceCounter(&stretchStart);
                    HRESULT stretchHr = device->StretchRect(backBuffer, nullptr, stagingRenderSurfaces[submitIdx],
                                                            nullptr, D3DTEXF_NONE);

                    if (FAILED(stretchHr)) {
                        // Fallback: disable intermediates, do direct readback now
                        EarlyLog(
                            "DX9: StretchRect staging failed (hr=0x%08x), "
                            "falling back to direct readback",
                            stretchHr);
                        stagingUseGpuIntermediate = false;
                        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
                            if (stagingRenderSurfaces[i]) {
                                stagingRenderSurfaces[i]->Release();
                                stagingRenderSurfaces[i] = nullptr;
                            }
                            if (stagingTextures[i]) {
                                stagingTextures[i]->Release();
                                stagingTextures[i] = nullptr;
                            }
                        }
                        // Direct readback (no deferred path without intermediate)
                        LARGE_INTEGER readbackStart, submitEnd;
                        QueryPerformanceCounter(&readbackStart);
                        HRESULT readbackHr = device->GetRenderTargetData(backBuffer, shmemSurfaces[submitIdx]);
                        QueryPerformanceCounter(&submitEnd);
                        stagingStretchRectUs = 0;
                        stagingReadbackSubmitUs =
                            static_cast<int32_t>(((submitEnd.QuadPart - readbackStart.QuadPart) * 1000000) / qpcFreq);
                        if (SUCCEEDED(readbackHr)) {
                            if (shmemQueries[submitIdx])
                                shmemQueries[submitIdx]->Issue(D3DISSUE_END);
                            shmemTextureReady[submitIdx] = true;
                            stagingTimestampQpc[submitIdx] = qpc.QuadPart;
                            stagingWriteIdx = (submitIdx + 1) % CAPTURE_TEXTURE_COUNT;
                            stagingPending++;
                            stagingLastSubmitQpc = submitEnd.QuadPart;
                        }
                    } else {
                        QueryPerformanceCounter(&stretchEnd);
                        stagingStretchRectUs =
                            static_cast<int32_t>(((stretchEnd.QuadPart - stretchStart.QuadPart) * 1000000) / qpcFreq);
                        // Defer GetRenderTargetData to after Present (PostPresentReadback).
                        // This prevents the GPU->CPU DMA from blocking the Present call.
                        stagingTimestampQpc[submitIdx] = qpc.QuadPart;
                        stagingPendingBlitIdx = submitIdx;
                    }
                } else {
                    // Direct readback (no intermediate - must happen before Present)
                    LARGE_INTEGER readbackStart, submitEnd;
                    QueryPerformanceCounter(&readbackStart);
                    HRESULT readbackHr = device->GetRenderTargetData(backBuffer, shmemSurfaces[submitIdx]);
                    QueryPerformanceCounter(&submitEnd);
                    stagingStretchRectUs = 0;
                    stagingReadbackSubmitUs =
                        static_cast<int32_t>(((submitEnd.QuadPart - readbackStart.QuadPart) * 1000000) / qpcFreq);
                    if (SUCCEEDED(readbackHr)) {
                        if (shmemQueries[submitIdx])
                            shmemQueries[submitIdx]->Issue(D3DISSUE_END);
                        shmemTextureReady[submitIdx] = true;
                        stagingTimestampQpc[submitIdx] = qpc.QuadPart;
                        stagingWriteIdx = (submitIdx + 1) % CAPTURE_TEXTURE_COUNT;
                        stagingPending++;
                        stagingLastSubmitQpc = submitEnd.QuadPart;
                    }
                }
            } else if (stagingPending >= CAPTURE_TEXTURE_COUNT - 1) {
                stagingTotalDropped++;
            }

            stagingCurrentDepth = stagingPending;
        } else if (useShmem) {
            // SHMEM capture fallback path (used for Trine3 DXVK compatibility).
            SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
            int idx = -1;
            for (int attempt = 0; attempt < CAPTURE_TEXTURE_COUNT; ++attempt) {
                const int candidate = (shmemCurTex + attempt) % CAPTURE_TEXTURE_COUNT;
                if (!IsCaptureTextureSlotOutstanding(sharedMem, 100 + (candidate % 2))) {
                    idx = candidate;
                    break;
                }
            }
            if (idx < 0) {
                droppedFrames.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            HRESULT hr = device->GetRenderTargetData(backBuffer, shmemSurfaces[idx]);
            if (SUCCEEDED(hr)) {
                D3DLOCKED_RECT rect;
                hr = shmemSurfaces[idx]->LockRect(&rect, NULL, D3DLOCK_READONLY);
                if (SUCCEEDED(hr)) {
                    int slot = idx % 2;
                    ShmemBuffer* shmBuf = g_IPC ? g_IPC->GetShmem() : nullptr;
                    if (shmBuf && shmBuf->slot_size > 0) {
                        uint32_t copyW = width;
                        uint32_t copyH = height;
                        if (copyW > shmBuf->max_width)
                            copyW = shmBuf->max_width;
                        if (copyH > shmBuf->max_height)
                            copyH = shmBuf->max_height;

                        uint8_t* dst = shmBuf->GetData(slot);
                        if (dst) {
                            uint8_t* src = (uint8_t*)rect.pBits;
                            uint32_t dstPitch = copyW * 4;

                            for (uint32_t y = 0; y < copyH; y++) {
                                memcpy(dst + (y * dstPitch), src + (y * rect.Pitch), dstPitch);
                            }

                            shmBuf->validWidth = copyW;
                            shmBuf->validHeight = copyH;
                            shmBuf->pitch = dstPitch;
                            shmBuf->writeSlot.store(slot);
                            shmBuf->mark_ready(slot);
                            SignalFrameReady(g_IPC, 100 + slot, qpc.QuadPart, 0);
                        }
                    } else {
                        static bool shmemUnavailableLogged = false;
                        if (!shmemUnavailableLogged) {
                            HookLogImportant("DX9: SHMEM transport unavailable (mapping not ready), frame dropped");
                            shmemUnavailableLogged = true;
                        }
                    }
                    shmemSurfaces[idx]->UnlockRect();
                }
            }
            shmemCurTex = (idx + 1) % CAPTURE_TEXTURE_COUNT;
        } else {
            // Zero-copy path: pipelined one frame behind.
            // 1. Complete PREVIOUS frame's GPU work (query has had an entire frame to finish)
            // 2. StretchRect CURRENT frame's backbuffer to the shared surface/ring slot
            // 3. Issue a D3D9 event query for NEXT frame's synchronization
            if (useDirectD3D9SharedRing) {
                zeroCopyQueryWaitUs = 0;
                zeroCopyReadbackUs = 0;

                DrainDirectD3D9SharedRingCompletions(false);

                const int idx = AcquireDirectD3D9SharedRingSubmitIndex();
                if (idx < 0 || !directSharedSurfaces9[idx]) {
                    droppedFrames.fetch_add(1, std::memory_order_relaxed);
                    return;
                }

                HRESULT hr = device->StretchRect(backBuffer, NULL, directSharedSurfaces9[idx], NULL, D3DTEXF_NONE);
                if (FAILED(hr)) {
                    return;
                }

                IDirect3DQuery9* completionQuery = directSharedQueries9[idx];
                if (!completionQuery) {
                    SignalDirectD3D9SharedRingFrame(idx, qpc.QuadPart);
                    return;
                }

                completionQuery->Issue(D3DISSUE_END);
                directSharedPending[idx] = true;
                directSharedPendingTimestampQpc[idx] = qpc.QuadPart;
                if (directSharedPendingCount < CAPTURE_TEXTURE_COUNT) {
                    directSharedPendingCount++;
                }
                return;
            }

            CompletePendingZeroCopy();
            if (zeroCopyPendingCopy) {
                droppedFrames.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            int idx = writeIndex.load(std::memory_order_acquire);
            IDirect3DSurface9* targetSurface = useDirectD3D9SharedRing ? directSharedSurfaces9[idx] : copySurface;
            if (!targetSurface) {
                return;
            }

            HRESULT hr = device->StretchRect(backBuffer, NULL, targetSurface, NULL, D3DTEXF_NONE);
            if (FAILED(hr)) {
                return;
            }

            IDirect3DQuery9* completionQuery = useDirectD3D9SharedRing ? directSharedQueries9[idx] : zeroCopyQuery;
            if (completionQuery)
                completionQuery->Issue(D3DISSUE_END);

            zeroCopyPendingCopy = true;
            zeroCopyPendingIdx = idx;
            zeroCopyPendingTimestampQpc = qpc.QuadPart;
        }
    }

    // Completes a pending zero-copy submission. Called at the start of the next
    // frame's CaptureFrame so the D3D9 event query has had an entire frame of
    // rendering time to finish — typically completes instantly.
    void CompletePendingZeroCopy() {
        if (!zeroCopyPendingCopy)
            return;

        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }

        int idx = zeroCopyPendingIdx;
        const int64_t frameTimestampQpc = zeroCopyPendingTimestampQpc;

        LARGE_INTEGER queryStart, queryEnd;
        QueryPerformanceCounter(&queryStart);

        IDirect3DQuery9* completionQuery = useDirectD3D9SharedRing ? directSharedQueries9[idx] : zeroCopyQuery;
        if (completionQuery) {
            const HRESULT queryHr = completionQuery->GetData(NULL, 0, 0);
            if (queryHr == S_FALSE)
                return;
            if (FAILED(queryHr)) {
                zeroCopyPendingCopy = false;
                zeroCopyPendingTimestampQpc = 0;
                return;
            }
        }
        zeroCopyPendingCopy = false;
        zeroCopyPendingTimestampQpc = 0;
        QueryPerformanceCounter(&queryEnd);
        zeroCopyQueryWaitUs = static_cast<int32_t>(((queryEnd.QuadPart - queryStart.QuadPart) * 1000000) / qpcFreq);

        if (useDirectD3D9SharedRing) {
            SignalPublishedTextureFrame(idx, frameTimestampQpc);

            zeroCopyReadbackUs = 0;
            AdvanceWriteIndex();
            return;
        }

        idx = AcquirePublishedTextureSlot();
        if (idx < 0) {
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (d3d11Context && d3d11SharedTexture && sharedTextures[idx]) {
            LARGE_INTEGER copyStart;
            QueryPerformanceCounter(&copyStart);

            d3d11Context->CopySubresourceRegion(sharedTextures[idx], 0, 0, 0, 0, d3d11SharedTexture, 0, NULL);

            LARGE_INTEGER copyEnd;
            QueryPerformanceCounter(&copyEnd);

            SignalPublishedTextureFrame(idx, frameTimestampQpc);

            zeroCopyReadbackUs = static_cast<int32_t>(((copyEnd.QuadPart - copyStart.QuadPart) * 1000000) / qpcFreq);

            AdvanceWriteIndex();
        }
    }

    // Called AFTER the actual D3D9 Present to complete deferred readback.
    // This prevents GetRenderTargetData's GPU->CPU DMA from blocking Present.
    void PostPresentReadback(IDirect3DDevice9* device) {
        std::unique_lock<std::recursive_mutex> captureLock(captureMutex, std::try_to_lock);
        if (!captureLock.owns_lock())
            return;
        if (!initialized)
            return;

        // GDI interop: capture is fully handled in CaptureFrame (double-buffered)
        if (useGDIInterop)
            return;

        bool isRecordingNow = g_IPC && g_IPC->IsRecording();
        if (useDirectD3D9SharedRing) {
            DrainDirectD3D9SharedRingCompletions(!isRecordingNow);
            return;
        }

        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }

        // --- Staging path: deferred GetRenderTargetData (legacy D3D9) ---
        if (useD3D11Staging && stagingPendingBlitIdx >= 0) {
            const int idx = stagingPendingBlitIdx;
            stagingPendingBlitIdx = -1;

            LARGE_INTEGER readbackStart, readbackEnd;
            QueryPerformanceCounter(&readbackStart);
            HRESULT readbackHr = device->GetRenderTargetData(stagingRenderSurfaces[idx], shmemSurfaces[idx]);
            QueryPerformanceCounter(&readbackEnd);
            stagingReadbackSubmitUs =
                static_cast<int32_t>(((readbackEnd.QuadPart - readbackStart.QuadPart) * 1000000) / qpcFreq);

            if (SUCCEEDED(readbackHr)) {
                if (shmemQueries[idx])
                    shmemQueries[idx]->Issue(D3DISSUE_END);
                shmemTextureReady[idx] = true;
                stagingWriteIdx = (idx + 1) % CAPTURE_TEXTURE_COUNT;
                stagingPending++;
                stagingLastSubmitQpc = readbackEnd.QuadPart;
            }
        }

        // Zero-copy path: completion is pipelined to next CaptureFrame.
        // Only flush here when recording is NOT active (final frame cleanup).
        if (!useD3D11Staging && zeroCopyPendingCopy && !isRecordingNow) {
            CompletePendingZeroCopy();
        }
    }

    void WaitPrerender(IDirect3DDevice9* device, float limit) {
        if (limit < 0.0f)
            return;

        static float s_LastLoggedLimit = -9999.0f;
        if (std::fabs(limit - s_LastLoggedLimit) > 0.01f) {
            HookLogImportant("DX9: CPU prerender limit active: %.2f", limit);
            s_LastLoggedLimit = limit;
        }

        bool isFractional = (limit > 0.01f && limit < 1.0f);

        if (limit == 0.0f) {
            // Strict Serial: Wait for current frame
            if (prerenderQueries.size() != 1) {
                for (auto& q : prerenderQueries)
                    if (q.query)
                        q.query->Release();
                prerenderQueries.clear();
                prerenderQueries.resize(1);
                prerenderIdx = 0;
            }

            uint32_t currentIdx = 0;
            if (!prerenderQueries[currentIdx].query) {
                device->CreateQuery(D3DQUERYTYPE_EVENT, &prerenderQueries[currentIdx].query);
            }
            if (prerenderQueries[currentIdx].query) {
                prerenderQueries[currentIdx].query->Issue(D3DISSUE_END);
                while (prerenderQueries[currentIdx].query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
                    Sleep(0);
                }
            }
        } else {
            // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1
            // (Lookback 1) combined with an idle gap to approximate sub-frame latency.
            int effectiveLimit = isFractional ? 1 : (int)limit;
            int lookback = effectiveLimit;
            size_t needed = 16;  // Use fixed size for simplicity in DX9 ring buffer

            if (prerenderQueries.size() != needed) {
                for (auto& q : prerenderQueries)
                    if (q.query)
                        q.query->Release();
                prerenderQueries.clear();
                prerenderQueries.resize(needed);
                prerenderIdx = 0;
            }

            // Wait for lookback frame
            if (prerenderIdx >= (uint32_t)lookback) {
                uint32_t waitIdx = (prerenderIdx - lookback) % (uint32_t)prerenderQueries.size();
                if (prerenderQueries[waitIdx].query) {
                    while (prerenderQueries[waitIdx].query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
                        Sleep(0);
                    }
                }
            }

            // Push New Fence
            uint32_t currentIdx = prerenderIdx % (uint32_t)prerenderQueries.size();
            if (!prerenderQueries[currentIdx].query) {
                device->CreateQuery(D3DQUERYTYPE_EVENT, &prerenderQueries[currentIdx].query);
            }
            if (prerenderQueries[currentIdx].query) {
                prerenderQueries[currentIdx].query->Issue(D3DISSUE_END);
            }

            // Strict Serial + Fixed Idle Gap for fractional limits
            if (isFractional) {
                // effectiveLimit already set to 0 for Strict Serial above

                // After the wait completes, calculate and apply a fixed idle gap
                float fps = g_PerfMetrics.GetCurrentFPS();
                double targetFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;

                // Fixed Idle Gap = TargetFrameTime * (1.0 - limit) * 0.10
                int64_t idleGapUs = (int64_t)(targetFrameTimeUs * (1.0 - limit) * 0.10);
                if (idleGapUs > 0) {
                    if (idleGapUs > 10000)
                        idleGapUs = 10000;  // Cap at 10ms
                    PrecisionSleep(idleGapUs);
                }
            }

            prerenderIdx++;
        }
    }
};

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
static DX9Capture g_DX9Capture;
static void InstallDeviceHooks(IDirect3DDevice9* device, bool newDevice = false);
static HRESULT STDMETHODCALLTYPE DetourEndScene(IDirect3DDevice9* device);

// Draw overlay using CustomOverlay
static void DrawDX9Overlay(IDirect3DDevice9* device) {
    if (ShouldSkipDX9OverlayForVulkan()) {
        return;
    }
    static int drawLogCount = 0;
    static int initFailCount = 0;

    if (drawLogCount < 5) {
        SharedMemoryLayout* dbgShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        HookLogImportant("DX9: DrawDX9Overlay #%d, IsInitialized=%d, IPC=%p, SHM=%p, showOverlay=%d", drawLogCount,
                         g_OverlayAdapter.IsInitialized() ? 1 : 0, (void*)g_IPC, (void*)dbgShm,
                         dbgShm ? dbgShm->ReadOverlayConfig().showOverlay : -1);
        drawLogCount++;
    }

    if (!g_OverlayAdapter.IsInitialized()) {
        // Get the window handle
        D3DDEVICE_CREATION_PARAMETERS params;
        HRESULT paramsHr = device->GetCreationParameters(&params);
        if (FAILED(paramsHr)) {
            if (initFailCount < 3) {
                EarlyLog("DX9: GetCreationParameters failed (hr=0x%08X)", paramsHr);
                initFailCount++;
            }
            return;
        }
        g_CachedHwnd = params.hFocusWindow;

        // Hook Input
        InputManager::Get().HookWindow(g_CachedHwnd);
        g_OverlayAdapter.SetHwnd(g_CachedHwnd);

        EarlyLog("DX9: Attempting OverlayAdapter::InitDX9 (device=%p, hwnd=%p)", (void*)device, (void*)g_CachedHwnd);
        if (g_OverlayAdapter.InitDX9(device)) {
            g_OverlayAdapter.SetHwnd(g_CachedHwnd);
            EarlyLog("DX9: OverlayAdapter initialized successfully");
        } else {
            if (initFailCount < 3) {
                EarlyLog("DX9: OverlayAdapter::InitDX9 FAILED");
                initFailCount++;
            }
            return;
        }
    }

    // Get viewport size
    D3DVIEWPORT9 vp;
    device->GetViewport(&vp);

    static int vpLogCount = 0;
    if (vpLogCount < 3) {
        HookLogImportant("DX9: DrawDX9Overlay vp=%ux%u (device=%p, IPC=%p)", vp.Width, vp.Height, (void*)device,
                         (void*)g_IPC);
        vpLogCount++;
    }

    g_OverlayAdapter.SetMetrics(&g_PerfMetrics);
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(g_DX9Capture.droppedFrames.load(std::memory_order_relaxed));
    const bool isEx = ResolveD3D9DeviceIsEx(device);
    const char* finalApi = ce::graphics_api_identity::D3D9Label(isEx, IsDXVKD3D9WrapperLoaded());
    g_OverlayAdapter.SetGraphicsAPI(finalApi, isEx ? "active IDirect3DDevice9Ex" : "active IDirect3DDevice9");

    // Render Custom Overlay
    // Note: RenderOverlay calls BeginFrame/RenderContent/EndFrame.
    // DX9 backend handles state saving/restoring internally.
    g_InOverlayRender = true;
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_OverlayAdapter.RenderOverlay(vp.Width, vp.Height);
    g_InOverlayRender = false;
}

static void CaptureDX9Screenshot(IDirect3DDevice9* device, SharedMemoryLayout* shm, uint64_t requestId) {
    if (!device || !shm || requestId == 0)
        return;

    bool queued = false;
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(device->GetRenderTarget(0, &bb)) && bb) {
        D3DSURFACE_DESC bbDesc;
        bb->GetDesc(&bbDesc);

        IDirect3DSurface9* staging = nullptr;
        if (SUCCEEDED(device->CreateOffscreenPlainSurface(bbDesc.Width, bbDesc.Height, bbDesc.Format, D3DPOOL_SYSTEMMEM,
                                                          &staging, NULL))) {
            if (SUCCEEDED(device->GetRenderTargetData(bb, staging))) {
                D3DLOCKED_RECT locked;
                if (SUCCEEDED(staging->LockRect(&locked, NULL, D3DLOCK_READONLY))) {
                    if (locked.Pitch > 0) {
                        queued = QueueScreenshotPixels(shm, requestId, static_cast<const uint8_t*>(locked.pBits),
                                                       bbDesc.Width, bbDesc.Height, static_cast<uint32_t>(locked.Pitch),
                                                       ScreenshotPixelFormat::BGRA8, ScreenshotColorEncoding::SRGB);
                    }
                    staging->UnlockRect();
                }
            }
            staging->Release();
        }
        bb->Release();
    }
    if (!queued)
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);
}

// Performance measurement
struct PresentTiming {
    int64_t startTime;
    int64_t overlayTime;
    int64_t captureTime;
    int64_t prerenderTime;
    int64_t fpsLimitTime;
    int64_t presentCallTime;
};
static thread_local PresentTiming g_Timing;
// Tracks whether the overlay was already drawn before the current Present call.
static thread_local bool g_overlayDrawnBeforePresent = false;
// Tracks whether the overlay was redrawn from a nested EndScene during Present.
static thread_local bool g_overlayDrawnInPresentEndScene = false;
static thread_local bool g_captureDeferredToPresentEndScene = false;
static thread_local uint64_t g_screenshotDeferredToPresentEndScene = 0;
static thread_local bool g_sawPresentNestedEndScene = false;
static std::atomic<bool> g_PreferOverlayInPresentEndScene{false};

static bool IsD3D9On12Loaded() {
    static int s_loaded = -1;
    HMODULE d3d9on12 = GetModuleHandleA("d3d9on12.dll");
    if (d3d9on12) {
        s_loaded = 1;
    } else if (s_loaded < 0) {
        s_loaded = 0;
    }
    return s_loaded > 0;
}
