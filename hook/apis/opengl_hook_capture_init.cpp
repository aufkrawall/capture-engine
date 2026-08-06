#include "opengl_hook_internal.h"


void OpenGLCapture::CreateSharedResources(uint32_t w,  uint32_t h,  uint32_t fmt) {


        // Implemented in Init

}
bool OpenGLCapture::CreateD3D11Device() {


        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL featureLevel;

        HMODULE hD3D11 = ce::security::LoadSystemLibrary(L"d3d11.dll");
        if (!hD3D11) {
            HookLog("OpenGL: D3D11 DLL not found");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                          const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                                          D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice =
            (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
            HookLog("OpenGL: D3D11CreateDevice not found");
            return false;
        }

        HRESULT hr =
            pD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels,
                               2, D3D11_SDK_VERSION, &d3d11Device, &featureLevel, &d3d11Context);

        if (FAILED(hr)) {
            HookLog("OpenGL: Failed to create D3D11 device (hr=0x%08x)", hr);
            return false;
        }

        // Get LUID and prevent DXGI from stealing window focus/cursor
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc;
                adapter->GetDesc(&desc);
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                luidLow = desc.AdapterLuid.LowPart;
                luidHigh = desc.AdapterLuid.HighPart;

                // Initialize SystemMetricsCollector with adapter LUID for GPU stats
                SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);

                // Report LUID to shared memory for out-of-process polling
                ReportLUID(luidLow, luidHigh);

                // Prevent DXGI from associating with the game window; without this DXGI
                // hides the hardware cursor and intercepts Alt+Enter on D3D11 device creation.
                IDXGIFactory* factory = nullptr;
                if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
                    HWND hwnd = opengl_hook_g_CaptureHDC ? WindowFromDC(opengl_hook_g_CaptureHDC) : nullptr;
                    if (hwnd)
                        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
                    factory->Release();
                }

                adapter->Release();
            }
            dxgiDevice->Release();
        }

        HookLog("OpenGL: Created D3D11 device (LUID: %08x)", luidLow);
        return true;

}
bool OpenGLCapture::InitNVInterop() {


        if (!opengl_hook_g_NVInteropAvailable || !opengl_hook_wglDXOpenDeviceNV || !opengl_hook_pglGenTextures || !opengl_hook_pglCopyTexSubImage2D) {
            return false;
        }

        // Open NV interop device
        nvDevice = opengl_hook_wglDXOpenDeviceNV(d3d11Device);
        if (!nvDevice) {
            HookLog("OpenGL: wglDXOpenDeviceNV failed");
            return false;
        }

        // Create shared D3D11 textures and register with GL
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        // Use RGBA format: OpenGL naturally writes RGBA, so using RGBA here avoids
        // R↔B channel swap that would occur with BGRA.
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        // SHADER_RESOURCE is required for SwapRBChannels to create an SRV for
        // RGBA→BGRA conversion in the encoder; RENDER_TARGET is required for NV
        // interop write access from OpenGL.
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            HRESULT hr = d3d11Device->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
            if (FAILED(hr) || !sharedTextures[i]) {
                HookLog("OpenGL: Failed to create D3D11 texture %d (hr=0x%08X)", i, hr);
                return false;
            }

            // Get shared handle
            IDXGIResource* resource = nullptr;
            hr = sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            if (FAILED(hr) || !resource) {
                HookLog("OpenGL: IDXGIResource query failed for NV texture %d (hr=0x%08X)", i, hr);
                return false;
            }
            HANDLE hShared = NULL;
            hr = resource->GetSharedHandle(&hShared);
            resource->Release();
            if (FAILED(hr) || !hShared) {
                HookLog("OpenGL: GetSharedHandle failed for NV texture %d (hr=0x%08X handle=%p)", i, hr, hShared);
                return false;
            }
            sharedTextureHandles[i].store(hShared, std::memory_order_release);

            // Create GL texture and register with NV interop
            opengl_hook_pglGenTextures(1, &glTextures[i]);

            nvTextureHandles[i] = opengl_hook_wglDXRegisterObjectNV(nvDevice, sharedTextures[i], glTextures[i], GL_TEXTURE_2D,
                                                        1  // WGL_ACCESS_WRITE_DISCARD_NV
            );

            if (!nvTextureHandles[i]) {
                HookLog("OpenGL: wglDXRegisterObjectNV failed for texture %d", i);
                return false;
            }
        }

        format = DXGI_FORMAT_R8G8B8A8_UNORM;  // GL writes RGBA naturally; encoder handles via SwapRB if needed
        usingNVInterop = true;
        HookLog("OpenGL: NV interop initialized successfully");
        return true;

}
bool OpenGLCapture::InitPBOFence() {


        ID3D11Device5* device5 = nullptr;
        HRESULT hr = d3d11Device->QueryInterface(IID_PPV_ARGS(&device5));
        if (FAILED(hr) || !device5)
            return false;

        hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
        device5->Release();
        if (FAILED(hr) || !fence) {
            fence = nullptr;
            return false;
        }

        HANDLE fenceHandle = nullptr;
        hr = fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle);
        if (FAILED(hr) || !fenceHandle) {
            fence->Release();
            fence = nullptr;
            return false;
        }

        hr = d3d11Context->QueryInterface(IID_PPV_ARGS(&context4));
        if (FAILED(hr) || !context4) {
            CloseHandle(fenceHandle);
            fence->Release();
            fence = nullptr;
            context4 = nullptr;
            return false;
        }

        sharedFenceHandle.store(fenceHandle, std::memory_order_release);
        useFences = true;
        HookLog("OpenGL: PBO upload fence initialized (cross-process GPU completion sync)");
        return true;

}
bool OpenGLCapture::InitPBOFallback() {


        if (!opengl_hook_pglGenBuffers || !opengl_hook_pglBindBuffer || !opengl_hook_pglBufferData || !opengl_hook_pglReadPixels || !opengl_hook_pglMapBuffer || !opengl_hook_pglUnmapBuffer) {
            HookLog("OpenGL: PBO fallback unavailable because required buffer/readback functions are missing");
            return false;
        }

        // Create PBOs for async readback
        opengl_hook_pglGenBuffers(2, pbos);
        if (!pbos[0] || !pbos[1]) {
            HookLog("OpenGL: Failed to allocate PBO names (%u, %u)", pbos[0], pbos[1]);
            return false;
        }

        const uint64_t bufferSize64 = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4u;
        if (bufferSize64 == 0 || bufferSize64 > static_cast<uint64_t>((std::numeric_limits<ptrdiff_t>::max)())) {
            HookLog("OpenGL: PBO allocation size is invalid (%ux%u, bytes=%llu)", width, height,
                    static_cast<unsigned long long>(bufferSize64));
            return false;
        }
        const ptrdiff_t bufferSize = static_cast<ptrdiff_t>(bufferSize64);
        for (int i = 0; i < 2; i++) {
            opengl_hook_pglBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[i]);
            opengl_hook_pglBufferData(GL_PIXEL_PACK_BUFFER, bufferSize, NULL, GL_STREAM_READ);
        }
        opengl_hook_pglBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        // Create D3D11 textures for sharing
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            HRESULT hr = d3d11Device->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
            if (FAILED(hr) || !sharedTextures[i]) {
                HookLog("OpenGL: Failed to create D3D11 texture %d for PBO fallback (hr=0x%08X)", i, hr);
                return false;
            }

            IDXGIResource* resource = nullptr;
            hr = sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            if (FAILED(hr) || !resource) {
                HookLog("OpenGL: IDXGIResource query failed for PBO texture %d (hr=0x%08X)", i, hr);
                return false;
            }
            HANDLE hShared = NULL;
            hr = resource->GetSharedHandle(&hShared);
            resource->Release();
            if (FAILED(hr) || !hShared) {
                HookLog("OpenGL: GetSharedHandle failed for PBO texture %d (hr=0x%08X handle=%p)", i, hr, hShared);

                return false;
            }
            sharedTextureHandles[i].store(hShared, std::memory_order_release);
        }

        if (!InitPBOFence()) {
            HookLog("OpenGL: D3D11.3 PBO fence unavailable; using legacy shared-resource Flush synchronization");
        }
        pboSyncSupported = opengl_hook_pglFenceSync && opengl_hook_pglClientWaitSync && opengl_hook_pglDeleteSync;
        if (!pboSyncSupported) {
            HookLog("OpenGL: GL sync objects unavailable; PBO readback may use the legacy mapping path");
        }
        usePBO = true;
        HookLog("OpenGL: PBO fallback initialized");
        return true;

}
void OpenGLCapture::Init(HDC hDC) {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (initialized)
            return;
        HookLog("OpenGLCapture: Init(HDC=0x%p)", hDC);

        // Safety: Ensure required functions are loaded
        if (!opengl_hook_pglGenFramebuffers || !opengl_hook_pglBindFramebuffer || !opengl_hook_pglFramebufferTexture2D || !opengl_hook_pglCheckFramebufferStatus ||
            !opengl_hook_pglGenTextures || !opengl_hook_pglBindTexture || !opengl_hook_pglTexImage2D || !opengl_hook_pglBlitFramebuffer || !opengl_hook_pglGetIntegerv) {
            HookLog("OpenGLCapture: FBO extensions not available. FBO capture disabled.");
            return;
        }

        opengl_hook_g_CaptureHDC = hDC;
        opengl_hook_g_CaptureContext = wglGetCurrentContext();

        // Get window size
        HWND hwnd = WindowFromDC(hDC);
        RECT rect = {};
        width = 0;
        height = 0;
        if (hwnd && GetClientRect(hwnd, &rect)) {
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
        }
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (width == 0 || height == 0) {
            HookLog("OpenGL: Invalid window size");
            return;
        }

        // Initialization temporarily binds CE-owned GL objects. Preserve the
        // application's state even on a partial initialization failure.
        GLint previousReadFramebuffer = 0;
        GLint previousDrawFramebuffer = 0;
        GLint previousTexture2D = 0;
        GLint previousPixelPackBuffer = 0;
        opengl_hook_pglGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        opengl_hook_pglGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
        opengl_hook_pglGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture2D);
        opengl_hook_pglGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPixelPackBuffer);
        auto restoreApplicationBindings = [&]() {
            opengl_hook_pglBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPixelPackBuffer));
            opengl_hook_pglBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture2D));
            opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
            opengl_hook_pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
        };

        // Create D3D11 device for interop
        if (!CreateD3D11Device()) {
            restoreApplicationBindings();
            CleanupGL();
            return;
        }

        // Create FBO for capturing
        opengl_hook_pglGenFramebuffers(1, &fbo);
        opengl_hook_pglGenTextures(1, &captureTexture);

        opengl_hook_pglBindTexture(GL_TEXTURE_2D, captureTexture);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        opengl_hook_pglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

        opengl_hook_pglBindFramebuffer(GL_FRAMEBUFFER, fbo);
        opengl_hook_pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, captureTexture, 0);

        if (opengl_hook_pglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            HookLog("OpenGL: FBO not complete");
            CleanupGL();
            restoreApplicationBindings();
            return;
        }

        // Try NV interop first, fallback to PBO
        bool captureReady = false;
        if (opengl_hook_g_NVInteropAvailable) {
            captureReady = InitNVInterop();
        }

        if (!captureReady) {
            // NV interop can fail after creating only part of its texture ring.
            // Tear that generation down before PBO creation so COM output slots,
            // handles, and GL names are never overwritten/leaked.
            CleanupTransportResources();
            captureReady = InitPBOFallback();
        }

        if (!captureReady) {
            HookLog("OpenGL: Failed to initialize capture");
            CleanupGL();
            restoreApplicationBindings();
            return;
        }

        // NV interop transfers ownership back to D3D on unlock. Queueing a
        // D3D11 fence immediately afterwards gives the media process an exact
        // cross-process completion point, just like the PBO upload path.
        if (usingNVInterop && !InitPBOFence()) {
            HookLog("OpenGL: NV interop fence unavailable; using interop's implicit synchronization");
        }

        // Publish to shared memory
        if (g_IPC) {
            PublishToSharedMemory(g_IPC);
        }

        // Re-assert MakeWindowAssociation after NV interop init: wglDXOpenDeviceNV
        // may re-enable DXGI window/cursor management. Repeat the call to ensure it
        // stays suppressed.
        if (usingNVInterop) {
            IDXGIDevice* dxgiDev = nullptr;
            if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
                IDXGIAdapter* adapter = nullptr;
                if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
                    IDXGIFactory* factory = nullptr;
                    if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
                        HWND wnd = WindowFromDC(hDC);
                        if (wnd)
                            factory->MakeWindowAssociation(wnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
                        factory->Release();
                    }
                    adapter->Release();
                }
                dxgiDev->Release();
            }
        }

        restoreApplicationBindings();
        initialized = true;
        HookLog("OpenGL Capture Initialized: %dx%d (NV Interop: %s)", width, height,
                usingNVInterop ? "Yes" : "No (PBO Fallback)");

}
