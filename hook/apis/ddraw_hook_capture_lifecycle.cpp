#include "ddraw_hook_internal.h"


DirectDrawBootstrapScope::~DirectDrawBootstrapScope() {


        --ddraw_hook_g_DDrawBootstrapDepth;

}
void DDrawCapture::ReleaseOverlayResources() {


        if (d3d9FastUploadSurface) {
            d3d9FastUploadSurface->Release();
            d3d9FastUploadSurface = nullptr;
        }
        if (d3d9UploadSurface) {
            d3d9UploadSurface->Release();
            d3d9UploadSurface = nullptr;
        }
        if (d3d9DeviceEx) {
            d3d9DeviceEx->Release();
            d3d9DeviceEx = nullptr;
        }
        if (d3d9Ex) {
            d3d9Ex->Release();
            d3d9Ex = nullptr;
        }
        d3d9UsesFlipEx = false;

}
void DDrawCapture::Cleanup() {


        CleanupDDraw(false);

}
bool DDrawCapture::CleanupDDraw(bool force) {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        bool hasPublishedGeneration = sharedFenceHandle.load(std::memory_order_acquire) != NULL;
        for (const auto& handle : sharedTextureHandles)
            hasPublishedGeneration = hasPublishedGeneration || handle.load(std::memory_order_acquire) != NULL;
        SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        if (!force && hasPublishedGeneration && HasOutstandingCaptureFrameLeases(sharedMem)) {
            static std::atomic<int> s_generationLeaseLogCount{0};
            if (s_generationLeaseLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
                HookLog("DDraw: Deferring capture resource cleanup while old frame leases are outstanding");
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

        if (stagingTexture) {
            stagingTexture->Release();
            stagingTexture = nullptr;
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

        ReleaseOverlayResources();

        ddrawSurface = nullptr;
        targetHwnd = NULL;
        initialized = false;
        useFences = false;
        fenceValue = 0;
        return true;

}
void DDrawCapture::CreateSharedResources(uint32_t w,  uint32_t ddraw_hook_h,  uint32_t fmt) {


        // Implemented in Init

}
