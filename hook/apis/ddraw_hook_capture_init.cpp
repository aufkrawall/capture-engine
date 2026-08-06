#include "ddraw_hook_internal.h"


bool DDrawCapture::CreateD3D11Device() {


        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL featureLevel;

        HMODULE hD3D11 = ce::security::LoadSystemLibrary(L"d3d11.dll");
        if (!hD3D11) {
            HookLog("DDraw: D3D11 DLL not found");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                          const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                                          D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice =
            (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
            HookLog("DDraw: D3D11CreateDevice not found");
            return false;
        }

        HRESULT hr = pD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, featureLevels, 2, D3D11_SDK_VERSION,
                                        &d3d11Device, &featureLevel, &d3d11Context);

        if (FAILED(hr)) {
            HookLog("DDraw: Failed to create D3D11 device (hr=0x%08x)", hr);
            return false;
        }

        // Get adapter LUID
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc;
                adapter->GetDesc(&desc);
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                luidLow = desc.AdapterLuid.LowPart;
                luidHigh = desc.AdapterLuid.HighPart;

                // Report LUID to shared memory for out-of-process polling
                ReportLUID(luidLow, luidHigh);
                adapter->Release();
            }
            dxgiDevice->Release();
        }

        // Try to get context4 for fences
        if (SUCCEEDED(d3d11Context->QueryInterface(IID_PPV_ARGS(&context4)))) {
            ID3D11Device5* device5 = nullptr;
            if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&device5)))) {
                if (SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)))) {
                    HANDLE hTemp = NULL;
                    fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hTemp);
                    sharedFenceHandle.store(hTemp, std::memory_order_release);
                    useFences = true;
                    HookLog("DDraw: D3D11.3 fence sync enabled");
                }
                device5->Release();
            }
        }

        HookLog("DDraw: D3D11 device created (LUID: %08x)", luidLow);
        return true;

}
bool DDrawCapture::CreateStagingTexture() {


        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DYNAMIC;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = d3d11Device->CreateTexture2D(&texDesc, NULL, &stagingTexture);
        if (FAILED(hr)) {
            HookLog("DDraw: Failed to create staging texture (hr=0x%08x)", hr);
            return false;
        }

        return true;

}
bool DDrawCapture::CreateSharedTextures() {


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
            if (FAILED(hr)) {
                HookLog("DDraw: Failed to create shared texture %d (hr=0x%08x)", i, hr);
                return false;
            }

            // Get shared handle
            IDXGIResource* resource = nullptr;
            sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            HANDLE hTemp = NULL;
            resource->GetSharedHandle(&hTemp);
            sharedTextureHandles[i].store(hTemp, std::memory_order_release);
            resource->Release();
        }

        HookLog("DDraw: Shared textures created");
        return true;

}
bool DDrawCapture::CreateD3D9ExWrapper(HWND hwnd) {


        HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
        if (!d3d9)
            d3d9 = ce::security::LoadSystemLibrary(L"d3d9.dll");
        if (!d3d9) {
            HookLog("DDraw: D3D9 DLL not found");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);
        PFN_Direct3DCreate9Ex pDirect3DCreate9Ex = (PFN_Direct3DCreate9Ex)GetProcAddress(d3d9, "Direct3DCreate9Ex");

        if (!pDirect3DCreate9Ex) {
            HookLog("DDraw: Direct3DCreate9Ex not found");
            return false;
        }

        HRESULT hr = pDirect3DCreate9Ex(D3D_SDK_VERSION, &d3d9Ex);
        if (FAILED(hr)) {
            HookLog("DDraw: Failed to create D3D9Ex (hr=0x%08x)", hr);
            return false;
        }

        // The DirectDraw app already controls frame pacing on its own presentation path.
        // The helper swap chain should avoid introducing a second vsync throttle.
        UINT presentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

        auto tryCreateDevice = [&](D3DSWAPEFFECT swapEffect, UINT backBufferCount) {
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
            D3DPRESENT_PARAMETERS d3dpp = {};
            d3dpp.Windowed = TRUE;
            d3dpp.SwapEffect = swapEffect;
            d3dpp.hDeviceWindow = hwnd;
            d3dpp.BackBufferFormat = D3DFMT_A8R8G8B8;
            d3dpp.BackBufferWidth = width;
            d3dpp.BackBufferHeight = height;
            d3dpp.BackBufferCount = backBufferCount;
            d3dpp.PresentationInterval = presentationInterval;
            return d3d9Ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                          D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &d3dpp, NULL,
                                          &d3d9DeviceEx);
        };

        hr = tryCreateDevice(D3DSWAPEFFECT_FLIPEX, 2);
        if (SUCCEEDED(hr)) {
            d3d9UsesFlipEx = true;
        } else {
            hr = tryCreateDevice(D3DSWAPEFFECT_DISCARD, 1);
            d3d9UsesFlipEx = false;
        }

        if (FAILED(hr)) {
            HookLog("DDraw: Failed to create D3D9Ex device (hr=0x%08x)", hr);
            return false;
        }

        LUID helperLuid = {};
        const HRESULT luidHr = d3d9Ex->GetAdapterLUID(D3DADAPTER_DEFAULT, &helperLuid);
        if (SUCCEEDED(luidHr) && (helperLuid.LowPart != 0 || helperLuid.HighPart != 0)) {
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            luidLow = helperLuid.LowPart;
            luidHigh = helperLuid.HighPart;
            ReportLUID(luidLow, luidHigh);
            HookLog("DDraw: Published D3D9Ex overlay-helper LUID %08x:%08x", luidHigh, luidLow);
        } else {
            HookLog("DDraw: D3D9Ex overlay-helper LUID unavailable (hr=0x%08x)", luidHr);
        }

        HookLog("DDraw: Created D3D9Ex helper device with %s swap effect", d3d9UsesFlipEx ? "FLIPEX" : "DISCARD");

        d3d9DeviceEx->SetMaximumFrameLatency(1);

        hr = d3d9DeviceEx->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                                       &d3d9FastUploadSurface, nullptr);
        if (FAILED(hr)) {
            HookLog("DDraw: Failed to create fast D3D9Ex upload surface (hr=0x%08x)", hr);
        }

        hr = d3d9DeviceEx->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                       &d3d9UploadSurface, nullptr);
        if (FAILED(hr)) {
            HookLog("DDraw: Failed to create D3D9Ex upload surface (hr=0x%08x)", hr);
            return false;
        }

        HookLog("DDraw: D3D9Ex wrapper created for overlay");
        return true;

}
bool DDrawCapture::EnsureOverlayDevice(HWND hwnd,  uint32_t w,  uint32_t ddraw_hook_h) {


        if (!hwnd || w == 0 || ddraw_hook_h == 0) {
            static int invalidOverlayStateLogCount = 0;
            if (invalidOverlayStateLogCount < 3) {
                HookLog("DDraw: EnsureOverlayDevice skipped (hwnd=%p, size=%ux%u)", hwnd, w, ddraw_hook_h);
                invalidOverlayStateLogCount++;
            }
            return false;
        }

        const bool hwndChanged = targetHwnd && hwnd != targetHwnd;
        const bool sizeChanged = width != w || height != ddraw_hook_h;
        if (initialized && (hwndChanged || sizeChanged)) {
            // Capture owns the generation-wide dimensions. Let
            // EnsureCaptureResources drain/rebuild it before mutating them.
            return false;
        }
        if ((hwndChanged || sizeChanged) && d3d9DeviceEx) {
            HookLog("DDraw: Recreating overlay helper (oldHwnd=%p newHwnd=%p old=%ux%u new=%ux%u)", targetHwnd, hwnd,
                    width, height, w, ddraw_hook_h);
            ReleaseOverlayResources();
        }

        targetHwnd = hwnd;
        width = w;
        height = ddraw_hook_h;
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (d3d9DeviceEx) {
            return true;
        }

        if (!CreateD3D9ExWrapper(hwnd)) {
            HookLog("DDraw: Overlay disabled (D3D9Ex wrapper failed)");
            return false;
        }

        HookLog("DDraw: Overlay helper ready (hwnd=%p, size=%ux%u)", hwnd, width, height);
        return true;

}
bool DDrawCapture::EnsureCaptureResources(IDirectDrawSurface7* surface,  HWND hwnd,  uint32_t w,  uint32_t ddraw_hook_h) {


        if (!surface || w == 0 || ddraw_hook_h == 0) {
            HookLog("DDraw: EnsureCaptureResources skipped (surface=%p, size=%ux%u)", surface, w, ddraw_hook_h);
            return false;
        }

        if (initialized && ddrawSurface == surface && width == w && height == ddraw_hook_h) {
            if (hwnd) {
                targetHwnd = hwnd;
            }
            return true;
        }

        if (initialized) {
            HookLog(
                "DDraw: Reinitializing capture resources for new surface/size (oldSurface=%p newSurface=%p old=%ux%u "
                "new=%ux%u)",
                ddrawSurface, surface, width, height, w, ddraw_hook_h);
            if (!CleanupDDraw(false))
                return false;
        }

        ddrawSurface = surface;
        targetHwnd = hwnd;
        width = w;
        height = ddraw_hook_h;
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (!CreateD3D11Device()) {
            CleanupDDraw(false);
            return false;
        }

        if (!CreateStagingTexture()) {
            CleanupDDraw(false);
            return false;
        }

        if (!CreateSharedTextures()) {
            CleanupDDraw(false);
            return false;
        }

        EnsureOverlayDevice(hwnd, w, ddraw_hook_h);

        if (g_IPC) {
            PublishToSharedMemory(g_IPC);
        }

        initialized = true;
        HookLog("DDraw Capture Initialized: %dx%d", width, height);
        return true;

}
void DDrawCapture::Init(IDirectDrawSurface7* surface,  HWND hwnd,  uint32_t w,  uint32_t ddraw_hook_h) {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        EnsureCaptureResources(surface, hwnd, w, ddraw_hook_h);

}
