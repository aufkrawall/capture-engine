#include "opengl_hook_internal.h"


void OpenGLCapture::Cleanup() {


        TryCleanup(false);

}
bool OpenGLCapture::TryCleanup(bool force) {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        bool hasPublishedGeneration = sharedFenceHandle.load(std::memory_order_acquire) != NULL;
        for (const auto& handle : sharedTextureHandles)
            hasPublishedGeneration = hasPublishedGeneration || handle.load(std::memory_order_acquire) != NULL;
        SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        if (!force && hasPublishedGeneration && HasOutstandingCaptureFrameLeases(sharedMem)) {
            static std::atomic<int> s_generationLeaseLogCount{0};
            if (s_generationLeaseLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
                HookLog("OpenGL: Deferring capture resource cleanup while old frame leases are outstanding");
            }
            return false;
        }
        CleanupGL();
        return true;

}
void OpenGLCapture::CleanupTransportResources() {


        // NV interop objects must be unregistered before either their GL names
        // or backing D3D11 textures are destroyed.
        if (nvDevice) {
            for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
                if (nvTextureHandles[i] && opengl_hook_wglDXUnregisterObjectNV) {
                    if (!opengl_hook_wglDXUnregisterObjectNV(nvDevice, nvTextureHandles[i])) {
                        HookLog("OpenGL: wglDXUnregisterObjectNV failed for texture %d during cleanup", i);
                    }
                    nvTextureHandles[i] = nullptr;
                }
            }
            if (opengl_hook_wglDXCloseDeviceNV && !opengl_hook_wglDXCloseDeviceNV(nvDevice)) {
                HookLog("OpenGL: wglDXCloseDeviceNV failed during cleanup");
            }
            nvDevice = nullptr;
        }

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            if (glTextures[i] && opengl_hook_pglDeleteTextures) {
                opengl_hook_pglDeleteTextures(1, &glTextures[i]);
                glTextures[i] = 0;
            }
        }
        if (opengl_hook_pglDeleteSync) {
            for (auto& sync : pboSyncs) {
                if (sync) {
                    opengl_hook_pglDeleteSync(sync);
                    sync = nullptr;
                }
            }
        } else {
            pboSyncs[0] = pboSyncs[1] = nullptr;
        }
        if ((pbos[0] || pbos[1]) && opengl_hook_pglDeleteBuffers) {
            opengl_hook_pglDeleteBuffers(2, pbos);
            pbos[0] = pbos[1] = 0;
        }

        // IDXGIResource::GetSharedHandle returns legacy KMT handles. They are
        // owned by the resource and must not be passed to CloseHandle.
        for (auto& handle : sharedTextureHandles) {
            handle.store(NULL, std::memory_order_release);
        }
        HANDLE fenceHandle = sharedFenceHandle.exchange(NULL, std::memory_order_acq_rel);
        if (fenceHandle) {
            CloseHandle(fenceHandle);  // ID3D11Fence::CreateSharedHandle is an owned NT handle.
        }
        for (auto*& texture : sharedTextures) {
            if (texture) {
                texture->Release();
                texture = nullptr;
            }
        }
        if (context4) {
            context4->Release();
            context4 = nullptr;
        }
        if (fence) {
            fence->Release();
            fence = nullptr;
        }

        useFences = false;
        usingNVInterop = false;
        usePBO = false;
        pboPopulated = false;
        pboSyncSupported = false;
        currentPBO = 0;
        pboTimestampQpc[0] = 0;
        pboTimestampQpc[1] = 0;
        fenceValue = 0;

}
void OpenGLCapture::CleanupGL() {


        CleanupTransportResources();

        // Cleanup OpenGL resources
        if (fbo && opengl_hook_pglDeleteFramebuffers) {
            opengl_hook_pglDeleteFramebuffers(1, &fbo);
            fbo = 0;
        }
        if (captureTexture && opengl_hook_pglDeleteTextures) {
            opengl_hook_pglDeleteTextures(1, &captureTexture);
            captureTexture = 0;
        }
        if (d3d11Context) {
            d3d11Context->Release();
            d3d11Context = nullptr;
        }
        if (d3d11Device) {
            d3d11Device->Release();
            d3d11Device = nullptr;
        }

        if (g_OverlayAdapter.IsInitialized()) {
            g_OverlayAdapter.Shutdown();
        }

        initialized = false;
        width = 0;
        height = 0;
        format = 0;
        opengl_hook_g_CaptureHDC = NULL;
        opengl_hook_g_CaptureContext = NULL;

}
