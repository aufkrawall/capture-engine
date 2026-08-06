#include "dx11_hook_internal.h"


void DX11Capture::Cleanup() {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        CaptureBase::StopCaptureThread();
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            HANDLE handle = sharedTextureHandles[i].exchange(NULL, std::memory_order_acq_rel);
            if (handle && sharedTextureHandlesAreNt[i]) {
                CloseHandle(handle);
            }
            sharedTextureHandlesAreNt[i] = false;
        }
        HANDLE fenceHandle = sharedFenceHandle.exchange(NULL, std::memory_order_acq_rel);
        if (fenceHandle) {
            CloseHandle(fenceHandle);
        }

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            g_DeferredRelease.Queue(sharedTextures[i]);
            sharedTextures[i] = nullptr;

            g_DeferredRelease.Queue(copyQueries[i]);
            copyQueries[i] = nullptr;

            g_DeferredRelease.Queue(sharedTextures10[i]);
            sharedTextures10[i] = nullptr;

            g_DeferredRelease.Queue(copyQueries10[i]);
            copyQueries10[i] = nullptr;

            slotFenceValues[i] = 0;

            g_DeferredRelease.Queue(dxvkImportedTextures[i]);
            dxvkImportedTextures[i] = nullptr;
        }

        g_DeferredRelease.Queue(fence);
        fence = nullptr;

        g_DeferredRelease.Queue(context4);
        context4 = nullptr;

        g_DeferredRelease.Queue(ownedContext);
        ownedContext = nullptr;

        g_DeferredRelease.Queue(ownedDevice);
        ownedDevice = nullptr;

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            g_DeferredRelease.Queue(keyedMutexes[i]);
            keyedMutexes[i] = nullptr;
        }

        g_DeferredRelease.Queue(cachedDevice10);
        cachedDevice10 = nullptr;

        // GetImmediateContext returns an owned reference. Keeping it across every
        // resize leaked a device/context generation and its driver allocations.
        g_DeferredRelease.Queue(cachedContext);
        cachedContext = nullptr;

        g_DeferredRelease.Queue(cachedSwapChainIdentity);
        cachedSwapChainIdentity = nullptr;

        cachedDevice = nullptr;
        initialized = false;
        generationResetPending = false;
        useFences = false;
        useKeyedMutex = false;
        isDX10Mode = false;
        isDXVKMode = false;
        fenceValue = 0;  // Reset fence value for next session

}
void DX11Capture::RequestGenerationReset(IDXGISwapChain* swapChain) {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (!initialized || !swapChain)
            return;

        IUnknown* identity = nullptr;
        const HRESULT identityHr = swapChain->QueryInterface(IID_PPV_ARGS(&identity));
        const bool matchesCaptureSwapChain =
            SUCCEEDED(identityHr) && identity && cachedSwapChainIdentity && identity == cachedSwapChainIdentity;
        if (identity)
            identity->Release();
        if (!matchesCaptureSwapChain)
            return;

        initialized = false;
        generationResetPending = true;
        HookLog("DX11Capture: Swapchain resized; deferring capture generation rebuild until frame leases drain");

}
void DX11Capture::CreateSharedResources(uint32_t w,  uint32_t h,  uint32_t fmt) {


        // This virtual method is called by CheckCaptureInit or manually
        // We need the device to create resources, so we'll store it in Init

}
bool DX11Capture::WaitForCopy(ID3D11DeviceContext* context,  int idx,  DWORD timeoutMs) {


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
ID3D11DeviceContext* DX11Capture::GetCaptureContext() {


        return cachedContext;

}
