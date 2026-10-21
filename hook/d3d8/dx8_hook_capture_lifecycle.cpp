#include "dx8_hook_internal.h"


void DX8Capture::Cleanup() {


        CleanupDX8(false);

}
bool DX8Capture::CleanupDX8(bool force) {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        bool hasPublishedGeneration = sharedFenceHandle.load(std::memory_order_acquire) != NULL;
        for (const auto& handle : sharedTextureHandles)
            hasPublishedGeneration = hasPublishedGeneration || handle.load(std::memory_order_acquire) != NULL;
        SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        if (!force && hasPublishedGeneration && HasOutstandingCaptureFrameLeases(sharedMem)) {
            static std::atomic<int> s_generationLeaseLogCount{0};
            if (s_generationLeaseLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
                HookLog("DX8: Deferring capture resource cleanup while old frame leases are outstanding");
            }
            return false;
        }

        // Release D3D11 resources
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            HANDLE sharedHandle = sharedTextureHandles[i].exchange(NULL, std::memory_order_acq_rel);
            if (sharedHandle && sharedTextureHandleOwned[i].exchange(false, std::memory_order_acq_rel))
                CloseHandle(sharedHandle);
            if (sharedTextures[i]) {
                sharedTextures[i]->Release();
                sharedTextures[i] = nullptr;
            }
        }

        if (fence) {
            fence->Release();
            fence = nullptr;
        }
        if (context4) {
            context4->Release();
            context4 = nullptr;
        }
        if (sharedFenceHandle) {
            CloseHandle(sharedFenceHandle);
            sharedFenceHandle = NULL;
        }

        if (d3d11Context) {
            d3d11Context->Release();
            d3d11Context = nullptr;
        }
        if (d3d11Device) {
            d3d11Device->Release();
            d3d11Device = nullptr;
        }

        // Release D3D9Ex wrapper
        if (d3d9UploadSurface) {
            d3d9UploadSurface->Release();
            d3d9UploadSurface = nullptr;
        }
        if (d3d9SharedSurface) {
            d3d9SharedSurface->Release();
            d3d9SharedSurface = nullptr;
        }
        if (d3d9DeviceEx) {
            DX9_UnregisterInternalHelperDevice(d3d9DeviceEx);
            d3d9DeviceEx->Release();
            d3d9DeviceEx = nullptr;
        }
        if (d3d9Ex) {
            d3d9Ex->Release();
            d3d9Ex = nullptr;
        }

        for (auto& q : dx8_hook_g_PrerenderQueries) {
            if (q)
                q->Release();

        }
        dx8_hook_g_PrerenderQueries.clear();
        dx8_hook_g_PrerenderFrameIndex = 0;

        ReleaseD3D8Surface(d3d8SnapshotSurface);
        ReleaseD3D8Surface(d3d8FrontBufferSurface);
        d3d8SnapshotFormat = D3DFMT_UNKNOWN;

        d3d8Device = nullptr;
        overlayHwnd = NULL;
        initialized = false;
        generationResetPending = false;
        retiredHostGenerationPending.store(false, std::memory_order_release);
        useFences = false;
        fenceValue = 0;
        return true;

}
// The host went away; this runs on HookThread while the game keeps rendering. D3D8 devices are not free-threaded and
// the game holds no CE lock while it draws, so the game device's surfaces must not be released here (the same defect
// crashed the DX9 driver after a host stop, 2026-10-21). Capture stops; CleanupDX8 runs on the game's thread later.
void DX8Capture::RetireHostGeneration() {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        const bool held = d3d8Device != nullptr || d3d9DeviceEx != nullptr || d3d11Device != nullptr;
        initialized = false;
        retiredHostGenerationPending.store(true, std::memory_order_release);
        HookLogImportant(
            "[InjectLifecycle] DX8: host generation retired on HookThread (objects held=%d); the game's thread "
            "releases them at its next Reset or the next host's first frame",
            held ? 1 : 0);

}
void DX8Capture::ReleaseRetiredHostGeneration(IDirect3DDevice8* device, const char* site) {


        if (!retiredHostGenerationPending.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (!retiredHostGenerationPending.load(std::memory_order_acquire))
            return;
        if (device && d3d8Device && device != d3d8Device)
            return;
        CleanupDX8(true);
        HookLogImportant("[InjectLifecycle] DX8: retired host generation released on the game's thread (%s, tid=%lu)",
                         site ? site : "unspecified", static_cast<unsigned long>(GetCurrentThreadId()));

}
void DX8Capture::PrepareForDeviceReset() {


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
void DX8Capture::CreateSharedResources(uint32_t w,  uint32_t h,  uint32_t fmt) {


        // Implemented in Init

}
