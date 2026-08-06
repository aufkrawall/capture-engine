#include "dx9_hook_internal.h"


void DX9Capture::CaptureFrame(IDirect3DDevice9* device,  IDirect3DSurface9* backBuffer) {


        std::unique_lock<std::recursive_mutex> captureLock(captureMutex, std::try_to_lock);
        if (!captureLock.owns_lock()) {
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!initialized || !backBuffer)
            return;

        // GDI interop: double-buffered with background capture thread.
        // Render thread only does StretchRect (async GPU, ~0us overhead).
        // Heavy GetDC+BitBlt runs on dedicated capture thread off render path.
        if (useGDIInterop) {
            zeroCopyQueryWaitUs = 0;
            zeroCopyReadbackUs = 0;
            const bool asyncGdiCapture = captureThreadRunning.load(std::memory_order_acquire);

            // Cadence gating: skip frames when game runs faster than capture target
            if (g_IPC && g_IPC->GetSharedMem()) {
                if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire))
                    return;
                if (ShouldSkipCaptureForTargetCadence(g_IPC->GetSharedMem(), "DX9"))
                    return;
            }

            // Check that write buffer isn't still being read by capture thread
            if (asyncGdiCapture && gdiBufferBusy[gdiWriteIdx].load(std::memory_order_acquire)) {
                return;  // Capture thread still busy with this buffer, skip frame
            }

            // StretchRect backbuffer → current write buffer (async GPU, ~0us)
            if (gdiCopySurfaces[gdiWriteIdx]) {
                HRESULT hr =
                    device->StretchRect(backBuffer, nullptr, gdiCopySurfaces[gdiWriteIdx], nullptr, D3DTEXF_NONE);
                if (SUCCEEDED(hr)) {
                    // Enqueue PREVIOUS frame's RT to capture thread
                    if (gdiHasPrevFrame) {
                        int readIdx = 1 - gdiWriteIdx;
                        const int64_t readTimestampQpc = gdiBufferTimestampQpc[readIdx];
                        if (asyncGdiCapture) {
                            EnqueueFrame(readTimestampQpc, 0, readIdx, nullptr);
                        } else {
                            static int64_t gdiQpcFreq = 0;
                            if (gdiQpcFreq == 0) {
                                LARGE_INTEGER f;
                                QueryPerformanceFrequency(&f);
                                gdiQpcFreq = f.QuadPart;
                            }

                            LARGE_INTEGER captureStart, captureEnd;
                            QueryPerformanceCounter(&captureStart);
                            CompleteGDIInteropCapture(gdiCopySurfaces[readIdx], readTimestampQpc);
                            QueryPerformanceCounter(&captureEnd);
                            zeroCopyReadbackUs = static_cast<int32_t>(
                                ((captureEnd.QuadPart - captureStart.QuadPart) * 1000000) / gdiQpcFreq);
                        }
                        gdiLastCaptureQpc = readTimestampQpc;
                    }
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    gdiBufferTimestampQpc[gdiWriteIdx] = now.QuadPart;
                    gdiHasPrevFrame = true;
                    gdiWriteIdx = 1 - gdiWriteIdx;
                }
            }
            return;
        }

        // Check if we should throttle capture (encoder is falling behind)
        if (g_IPC && g_IPC->GetSharedMem()) {
            if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
                return;
            }
        }

        // Get timestamp
        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);

        if (useD3D11Staging) {
            // Restructured D3D11 staging pipeline:
            // SUBMIT: StretchRect + GetRenderTargetData batched together + Query
            //         (both GPU commands in same command stream, query covers both)
            // CONSUME: Query complete -> LockRect (instant, data in shmem) ->
            //          UpdateSubresource (D3D11 upload) -> Signal
            //
            // This eliminates the 12ms+ stall from the old approach where
            // GetRenderTargetData was called separately in the consume phase,
            // forcing a GPU pipeline flush on every consumed frame.

            // Reset per-frame metrics
            stagingStretchRectUs = 0;
            stagingReadbackSubmitUs = 0;
            stagingQueryWaitUs = 0;
            stagingLockRectUs = 0;
            stagingD3D11UploadUs = 0;

            // === PHASE 1: CONSUME completed readbacks ===
            // Data is already in system memory (GetRenderTargetData was batched
            // with StretchRect in submit). Consume is cheap: LockRect + D3D11 upload.
            if (stagingPending > 0) {
                const int consumeIdx = stagingReadIdx;
                if (shmemTextureReady[consumeIdx]) {
                    // Non-blocking query check
                    bool queryReady = true;

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


void DX9Capture::CompletePendingZeroCopy() {


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


void DX9Capture::PostPresentReadback(IDirect3DDevice9* device) {


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


void DX9Capture::WaitPrerender(IDirect3DDevice9* device,  float limit) {


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
                float fps = dx9_hook_g_PerfMetrics.GetCurrentFPS();
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

