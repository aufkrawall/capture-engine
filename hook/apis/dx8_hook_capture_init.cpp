#include "dx8_hook_internal.h"


bool DX8Capture::CreateD3D9ExWrapper(HWND hwnd) {


        if (d3d9DeviceEx)
            return true;
        // Create D3D9Ex calls dynamic
        HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
        if (!d3d9)
            d3d9 = ce::security::LoadSystemLibrary(L"d3d9.dll");
        if (!d3d9) {
            HookLog("DX8: D3D9 DLL not found");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);
        PFN_Direct3DCreate9Ex pDirect3DCreate9Ex = (PFN_Direct3DCreate9Ex)GetProcAddress(d3d9, "Direct3DCreate9Ex");

        if (!pDirect3DCreate9Ex) {
            HookLog("DX8: Direct3DCreate9Ex not found");
            return false;
        }

        HRESULT hr = pDirect3DCreate9Ex(D3D_SDK_VERSION, &d3d9Ex);
        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D9Ex (hr=0x%08x)", hr);
            return false;
        }

        // Create D3D9Ex device
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3DPRESENT_PARAMETERS d3dpp = {};
        d3dpp.Windowed = TRUE;
        d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        d3dpp.hDeviceWindow = hwnd;
        d3dpp.BackBufferFormat = D3DFMT_A8R8G8B8;
        d3dpp.BackBufferWidth = width;
        d3dpp.BackBufferHeight = height;
        d3dpp.BackBufferCount = 1;
        d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

        {
            DX9InternalBypassScope dx9Bypass;
            hr = d3d9Ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &d3dpp, NULL,
                                        &d3d9DeviceEx);
        }

        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D9Ex device (hr=0x%08x)", hr);
            d3d9Ex->Release();
            d3d9Ex = nullptr;
            return false;
        }

        DX9_RegisterInternalHelperDevice(d3d9DeviceEx);
        d3d9DeviceEx->SetMaximumFrameLatency(1);

        hr = d3d9DeviceEx->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                       &d3d9UploadSurface, nullptr);
        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D9 upload surface (hr=0x%08x)", hr);
            d3d9DeviceEx->Release();
            d3d9DeviceEx = nullptr;
            d3d9Ex->Release();
            d3d9Ex = nullptr;
            return false;
        }

        HookLog("DX8: D3D9Ex wrapper created");
        return true;

}


bool DX8Capture::CreateD3D11Device() {


        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL featureLevel;

        HMODULE hD3D11 = ce::security::LoadSystemLibrary(L"d3d11.dll");
        if (!hD3D11) {
            HookLog("DX8: D3D11 DLL not found");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                          const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                                          D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice =
            (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
            HookLog("DX8: D3D11CreateDevice not found");
            return false;
        }

        HRESULT hr = pD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, featureLevels, 2, D3D11_SDK_VERSION,
                                        &d3d11Device, &featureLevel, &d3d11Context);

        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D11 device (hr=0x%08x)", hr);
            return false;
        }

        // Get adapter LUID
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC dx8_hook_desc;
                adapter->GetDesc(&dx8_hook_desc);
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                luidLow = dx8_hook_desc.AdapterLuid.LowPart;
                luidHigh = dx8_hook_desc.AdapterLuid.HighPart;

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
                    HookLog("DX8: D3D11.3 fence sync enabled");
                }
                device5->Release();
            }
        }

        HookLog("DX8: D3D11 device created (LUID: %08x)", luidLow);
        return true;

}


bool DX8Capture::CreateSharedTextures() {


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
                HookLog("DX8: Failed to create shared texture %d (hr=0x%08x)", i, hr);
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

        HookLog("DX8: Shared textures created");
        return true;

}


bool DX8Capture::CreateD3D9ExSharedSurface() {


        // Create D3D9Ex offscreen surface that can share with D3D11
        HANDLE sharedHandle = nullptr;
        HRESULT hr = d3d9DeviceEx->CreateOffscreenPlainSurfaceEx(width, height, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                                                 &d3d9SharedSurface, &sharedHandle, 0);

        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D9Ex shared surface (hr=0x%08x)", hr);
            return false;
        }

        HookLog("DX8: D3D9Ex shared surface created");
        return true;

}


bool DX8Capture::EnsureOverlayDevice(IDirect3DDevice8* device,  HWND hwnd) {


        if (!hwnd) {
            return false;
        }

        RECT rect = {};
        GetClientRect(hwnd, &rect);
        uint32_t newWidth = rect.right - rect.left;
        uint32_t newHeight = rect.bottom - rect.top;
        if (newWidth == 0 || newHeight == 0) {
            return false;
        }

        const bool hwndChanged = overlayHwnd && overlayHwnd != hwnd;
        const bool sizeChanged = width != newWidth || height != newHeight;
        if ((hwndChanged || sizeChanged) && (d3d9DeviceEx || initialized)) {
            if (g_OverlayAdapter.IsInitialized()) {
                g_OverlayAdapter.Shutdown();
            }
            if (!CleanupDX8(false))
                return false;
        }

        d3d8Device = device;
        overlayHwnd = hwnd;
        width = newWidth;
        height = newHeight;
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (d3d9DeviceEx) {
            return true;
        }

        if (!CreateD3D9ExWrapper(hwnd)) {
            HookLog("DX8: Overlay helper creation failed");
            return false;
        }

        HookLog("DX8: Overlay helper ready (hwnd=%p, size=%ux%u)", hwnd, width, height);
        return true;

}


void DX8Capture::Init(IDirect3DDevice8* device,  HWND hwnd) {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (initialized)
            return;
        if (generationResetPending && !CleanupDX8(false))
            return;

        d3d8Device = device;
        overlayHwnd = hwnd;

        // Get backbuffer size from HWND
        RECT rect;
        GetClientRect(hwnd, &rect);
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (width == 0 || height == 0) {
            HookLog("DX8: Invalid window size");
            return;
        }

        // Create D3D9Ex wrapper for sharing
        if (!CreateD3D9ExWrapper(hwnd)) {
            CleanupDX8(false);
            return;
        }

        // Create D3D11 device
        if (!CreateD3D11Device()) {
            CleanupDX8(false);
            return;
        }

        // Create shared textures
        if (!CreateSharedTextures()) {
            CleanupDX8(false);
            return;
        }

        // Create D3D9Ex shared surface
        if (!CreateD3D9ExSharedSurface()) {
            CleanupDX8(false);
            return;
        }

        // Publish to shared memory
        if (g_IPC) {
            PublishToSharedMemory(g_IPC);
        }

        initialized = true;
        HookLog("DX8 Capture Initialized: %dx%d", width, height);

}
