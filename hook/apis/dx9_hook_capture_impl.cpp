#include "dx9_hook_internal.h"


DX9Capture::DX9Capture() {


        CaptureBase::initialized = false;
        initializationFailed = false;
        firstFrame = true;

}

void DX9Capture::StagingCaptureThreadProc() {


        captureThreadRunning = true;
        EarlyLog("DX9: Staging capture thread started");

        while (!captureThreadShutdown.load(std::memory_order_acquire)) {
            uint32_t rIdx = pendingReadIdx.load(std::memory_order_acquire);
            uint32_t wIdx = pendingWriteIdx.load(std::memory_order_acquire);

            if (rIdx == wIdx) {
                WaitForSingleObject(captureEvent, 50);
                continue;
            }

            PendingCaptureFrame& frame = pendingRing[rIdx % CAPTURE_RING_SIZE];
            const int consumeIdx = static_cast<int>(frame.backBufferIndex);

            // LockRect on SYSTEMMEM surface - instant after query confirmed DMA done
            D3DLOCKED_RECT rect;
            DWORD lockFlags = D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK;
            HRESULT lockHr = shmemSurfaces[consumeIdx]->LockRect(&rect, NULL, lockFlags);

            if (SUCCEEDED(lockHr)) {
                const int texIdx = AcquirePublishedTextureSlot();
                const bool canUpload = texIdx >= 0 && d3d11Context && sharedTextures[texIdx];
                if (canUpload) {
                    d3d11Context->UpdateSubresource(sharedTextures[texIdx], 0, NULL, rect.pBits, rect.Pitch, 0);
                }

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

void DX9Capture::GDICaptureThreadProc() {


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
            ++gdiThreadLogCount;
            if (gdiThreadLogCount <= 5 || gdiThreadLogCount % 200 == 0)
                HookLog("DX9: GDI thread: frame #%d surf[%d] %dus", gdiThreadLogCount, surfIdx, captureUs);

            pendingReadIdx.store(rIdx + 1, std::memory_order_release);
        }

        captureThreadRunning = false;
        EarlyLog("DX9: GDI capture thread stopped");

}

void DX9Capture::Cleanup() {


        CleanupDX9(false);

}

void DX9Capture::ForceCleanup() {


        CleanupDX9(false, true);

}

void DX9Capture::ReleaseSharedTextureRing() {


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

bool DX9Capture::CreateSharedTextureRing(bool gdiCompatible) {


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

void DX9Capture::ReleaseDirectD3D9RingResources() {


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

void DX9Capture::ReleaseDirectD3D9HelperDevices() {


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

void DX9Capture::ReleaseDirectD3D9SharedRing() {


        ReleaseDirectD3D9RingResources();
        ReleaseDirectD3D9HelperDevices();

}

int DX9Capture::AcquirePublishedTextureSlot() {


        SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        const int slot = FindAvailableCaptureTextureSlot(sharedMem, writeIndex.load(std::memory_order_relaxed));
        if (slot >= 0)
            writeIndex.store(slot, std::memory_order_relaxed);
        return slot;

}

void DX9Capture::SignalPublishedTextureFrame(int idx,  int64_t frameTimestampQpc) {


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
