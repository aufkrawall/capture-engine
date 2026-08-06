#include "dx9_hook_internal.h"


void DX9Capture::ReleaseGameDeviceResourcesForReset() {


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
        dx9_hook_g_DX9StagingCaptureActive.store(false, std::memory_order_release);
        d3d9Format = D3DFMT_UNKNOWN;
        d3d9SharedFormat = D3DFMT_UNKNOWN;
        initialized = false;
        firstFrame = true;

}

bool DX9Capture::PrepareForDeviceReset() {


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

bool DX9Capture::CleanupDX9(bool permanentFailure,  bool force) {


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
        dx9_hook_g_DX9StagingCaptureActive.store(false, std::memory_order_release);

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
