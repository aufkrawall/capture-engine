#include "ddraw_hook_internal.h"

#include <bit>


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
                luidLow = std::bit_cast<int32_t>(desc.AdapterLuid.LowPart);
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
                    const HRESULT shareHr = fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hTemp);
                    if (SUCCEEDED(shareHr) && hTemp) {
                        sharedFenceHandle.store(hTemp, std::memory_order_release);
                        sharedFenceHandleOwned.store(true, std::memory_order_release);
                        useFences = true;
                        HookLog("DDraw: D3D11.3 fence sync enabled");
                    } else {
                        HookLog("DDraw: Fence sharing unavailable (hr=0x%08x); using implicit synchronization",
                                shareHr);
                    }
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
            const HRESULT resourceHr = sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            if (FAILED(resourceHr) || !resource) {
                HookLog("DDraw: Shared texture %d has no IDXGIResource (hr=0x%08x)", i, resourceHr);
                return false;
            }
            HANDLE hTemp = NULL;
            const HRESULT handleHr = resource->GetSharedHandle(&hTemp);
            resource->Release();
            if (FAILED(handleHr) || !hTemp) {
                HookLog("DDraw: Failed to export shared texture %d (hr=0x%08x)", i, handleHr);
                return false;
            }
            sharedTextureHandles[i].store(hTemp, std::memory_order_release);
            sharedTextureHandleOwned[i].store(false, std::memory_order_release);
        }

        HookLog("DDraw: Shared textures created");
        return true;

}
bool DDrawCapture::EnsureOverlayDevice(HWND hwnd,  uint32_t w,  uint32_t ddraw_hook_h) {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
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
        if (hwndChanged || sizeChanged) {
            HookLog("DDraw: Overlay target changed (oldHwnd=%p newHwnd=%p old=%ux%u new=%ux%u)", targetHwnd, hwnd,
                    width, height, w, ddraw_hook_h);
            // The composite's backdrop and sprite belong to the old surface
            // geometry; keeping them across a size or target change would write
            // stale pixels into the new one.
            ReleaseOverlayResources();
        }

        targetHwnd = hwnd;
        width = w;
        height = ddraw_hook_h;
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        // The CPU composite needs no device. The LUID still has to be published
        // so the out-of-process VRAM telemetry can bind to the adapter.
        PublishOverlayAdapterLuidOnce();
        return true;

}
void DDrawCapture::PublishOverlayAdapterLuidOnce() {


        // The host's GPU/VRAM telemetry needs an adapter identity even in
        // overlay-only runs, where no capture device is ever created. DXGI
        // answers that without loading d3d9.dll: the DirectDraw route no longer
        // has a D3D9 helper device, and a process that merely contains d3d9.dll
        // must not look like a D3D9 title.
        if (luidLow != 0 || luidHigh != 0) {
            return;
        }

        static bool attempted = false;
        if (attempted) {
            return;
        }
        attempted = true;

        HMODULE dxgi = ce::security::LoadSystemLibrary(L"dxgi.dll");
        if (!dxgi) {
            return;
        }

        typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
        auto createFactory = reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
        if (!createFactory) {
            return;
        }

        IDXGIFactory1* factory = nullptr;
        if (FAILED(createFactory(IID_PPV_ARGS(&factory))) || !factory) {
            return;
        }

        IDXGIAdapter1* adapter = nullptr;
        LUID adapterLuid = {};
        const HRESULT enumHr = factory->EnumAdapters1(0, &adapter);
        if (SUCCEEDED(enumHr) && adapter) {
            DXGI_ADAPTER_DESC1 desc = {};
            if (SUCCEEDED(adapter->GetDesc1(&desc)))
                adapterLuid = desc.AdapterLuid;
            adapter->Release();
        }
        factory->Release();

        if ((adapterLuid.LowPart == 0 && adapterLuid.HighPart == 0)) {
            HookLog("DDraw: Overlay adapter LUID unavailable (hr=0x%08x)", enumHr);
            return;
        }

        luidLow = std::bit_cast<int32_t>(adapterLuid.LowPart);
        luidHigh = adapterLuid.HighPart;
        ReportLUID(luidLow, luidHigh);
        HookLog("DDraw: Published overlay adapter LUID %08x:%08x", luidHigh, luidLow);

}
bool DDrawCapture::EnsureCaptureResources(IDirectDrawSurface7* surface,  HWND hwnd,  uint32_t w,  uint32_t ddraw_hook_h) {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
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
