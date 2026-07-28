
        uint32_t waitIdx = g_PrerenderIdx % (uint32_t)g_PrerenderSurfaces.size();
        if (g_PrerenderSurfaces[waitIdx]) {
            IDirectDrawSurface7* waitSurf = g_PrerenderSurfaces[waitIdx];
            typedef HRESULT(STDMETHODCALLTYPE * GetFlipStatus_t)(IDirectDrawSurface7*, DWORD);
            void** vtable = *(void***)waitSurf;
            GetFlipStatus_t pGetFlipStatus = (GetFlipStatus_t)vtable[13];

            while (pGetFlipStatus(waitSurf, 1) == 0x887600FA) {
                std::this_thread::yield();
            }
        }

        g_PrerenderSurfaces[waitIdx] = surface;
        g_PrerenderIdx++;
    }

    // Strict Serial + Fixed Idle Gap for fractional limits
    if (isFractional) {
        // effectiveLimit already set to 0 for Strict Serial above

        // After the wait completes, calculate and apply a fixed idle gap
        float fps = g_PerfMetrics.GetCurrentFPS();
        double targetFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;

        // Fixed Idle Gap = TargetFrameTime * (1.0 - limit) * 0.10
        int64_t idleGapUs = (int64_t)(targetFrameTimeUs * (1.0 - limit) * 0.10);
        if (idleGapUs > 0) {
            if (idleGapUs > 10000)
                idleGapUs = 10000;  // Cap at 10ms
            PrecisionSleep(idleGapUs);
        }
    }
}

static void InstallDirectDrawCreateInlineHook(DirectDrawCreate_t directDrawCreate) {
    if (!directDrawCreate || g_DirectDrawCreateInlineInstalled)
        return;

    void* trampoline = nullptr;
    if (InlineHook::Install((void*)directDrawCreate, (void*)DetourDirectDrawCreate, &trampoline)) {
        oDirectDrawCreate = reinterpret_cast<DirectDrawCreate_t>(trampoline);
        g_DirectDrawCreateInlineInstalled = true;
        HookLog("DDraw: DirectDrawCreate inline hook installed");
    } else {
        HookLog("DDraw: DirectDrawCreate inline hook failed");
    }
}

static void InstallDirectDrawCreateExInlineHook(DirectDrawCreateEx_t directDrawCreateEx) {
    if (!directDrawCreateEx || g_DirectDrawCreateExInlineInstalled)
        return;

    void* trampoline = nullptr;
    if (InlineHook::Install((void*)directDrawCreateEx, (void*)DetourDirectDrawCreateEx, &trampoline)) {
        oDirectDrawCreateEx = reinterpret_cast<DirectDrawCreateEx_t>(trampoline);
        g_DirectDrawCreateExInlineInstalled = true;
        HookLog("DDraw: DirectDrawCreateEx inline hook installed");
    } else {
        HookLog("DDraw: DirectDrawCreateEx inline hook failed");
    }
}

static bool FindDirectDrawBootstrapWindow(HWND* outWindow, DWORD* outThreadId) {
    if (!outWindow || !outThreadId)
        return false;

    *outWindow = NULL;
    *outThreadId = 0;

    HWND foregroundWindow = GetForegroundWindow();
    DWORD foregroundPid = 0;
    DWORD foregroundThreadId = 0;
    if (foregroundWindow) {
        foregroundThreadId = GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
        if (foregroundPid == GetCurrentProcessId()) {
            *outWindow = foregroundWindow;
            *outThreadId = foregroundThreadId;
            return true;
        }
    }

    ce::overlay_compat::AuxiliaryProcessWindowInfo info = {};
    if (ce::overlay_compat::FindAuxiliaryProcessWindow(GetCurrentProcessId(), nullptr, &info) && info.hwnd &&
        info.threadId != 0) {
        *outWindow = info.hwnd;
        *outThreadId = info.threadId;
        return true;
    }

    return false;
}

static LRESULT CALLBACK DirectDrawBootstrapHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && g_DDrawBootstrapQueued.load(std::memory_order_acquire) &&
        !g_DDrawBootstrapRunning.exchange(true, std::memory_order_acq_rel)) {
        HHOOK hook = g_DDrawBootstrapHook;
        g_DDrawBootstrapHook = nullptr;
        g_DDrawBootstrapQueued.store(false, std::memory_order_release);
        if (hook) {
            UnhookWindowsHookEx(hook);
        }

        BootstrapDirectDrawHooksOnCurrentThread("window-thread bootstrap");
        g_DDrawBootstrapRunning.store(false, std::memory_order_release);
    }

    return CallNextHookEx(g_DDrawBootstrapHook, code, wParam, lParam);
}

static bool QueueDirectDrawBootstrapOnWindowThread() {
    if (g_HooksInitialized)
        return true;

    HWND bootstrapWindow = NULL;
    DWORD bootstrapThreadId = 0;
    if (!FindDirectDrawBootstrapWindow(&bootstrapWindow, &bootstrapThreadId) || !bootstrapWindow ||
        bootstrapThreadId == 0) {
        HookLog("DDraw: Failed to find bootstrap window thread");
        return false;
    }

    if (g_DDrawBootstrapHook) {
        HookLog("DDraw: Bootstrap window hook already queued (hwnd=%p, tid=%lu)", bootstrapWindow,
                (unsigned long)bootstrapThreadId);
        return true;
    }

    g_DDrawBootstrapWindow = bootstrapWindow;
    g_DDrawBootstrapThreadId = bootstrapThreadId;
    g_DDrawBootstrapHook = SetWindowsHookExA(WH_CALLWNDPROC, DirectDrawBootstrapHookProc, NULL, bootstrapThreadId);
    if (!g_DDrawBootstrapHook) {
        HookLog("DDraw: Failed to install bootstrap window hook (hwnd=%p, tid=%lu, err=%lu)", bootstrapWindow,
                (unsigned long)bootstrapThreadId, GetLastError());
        return false;
    }

    g_DDrawBootstrapQueued.store(true, std::memory_order_release);
    HookLog("DDraw: Queued bootstrap window hook (hwnd=%p, tid=%lu)", bootstrapWindow,
            (unsigned long)bootstrapThreadId);

    DWORD_PTR sendResult = 0;
    if (!SendMessageTimeoutA(bootstrapWindow, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &sendResult)) {
        HookLog("DDraw: Failed to send bootstrap wake message (hwnd=%p, tid=%lu, err=%lu)", bootstrapWindow,
                (unsigned long)bootstrapThreadId, GetLastError());
    }

    return true;
}

static void BootstrapDirectDrawHooksOnCurrentThread(const char* reason) {
    if (g_HooksInitialized)
        return;
    DirectDrawBootstrapScope bootstrapScope;

    HookLog("DDraw: BootstrapDirectDrawHooksOnCurrentThread starting via %s", reason);

    HMODULE ddrawModule = GetModuleHandleA("ddraw.dll");
    if (!ddrawModule) {
        HookLog("DDraw: ddraw.dll not loaded during bootstrap");
        return;
    }

    DirectDrawCreateEx_t pDirectDrawCreateEx = (DirectDrawCreateEx_t)GetProcAddress(ddrawModule, "DirectDrawCreateEx");
    if (!pDirectDrawCreateEx) {
        HookLog("DDraw: DirectDrawCreateEx not found during bootstrap");
        return;
    }
    DirectDrawCreate_t pDirectDrawCreate = (DirectDrawCreate_t)GetProcAddress(ddrawModule, "DirectDrawCreate");
    InstallDirectDrawCreateInlineHook(pDirectDrawCreate);

    DirectDrawCreateEx_t createFunction = oDirectDrawCreateEx ? oDirectDrawCreateEx : pDirectDrawCreateEx;
    HookLog("DDraw: Bootstrap create function=%p (export=%p, trampoline=%p)", createFunction, pDirectDrawCreateEx,
            oDirectDrawCreateEx);

    IDirectDraw7* ddraw7 = nullptr;
    HRESULT hr = createFunction(NULL, (LPVOID*)&ddraw7, IID_IDirectDraw7, NULL);
    if (FAILED(hr) || !ddraw7) {
        HookLog("DDraw: Failed to create DirectDraw7 (hr=0x%08x)", hr);
        InstallDirectDrawCreateExInlineHook(pDirectDrawCreateEx);
        return;
    }
    HookLog("DDraw: Bootstrap DirectDraw7 created (object=%p)", ddraw7);

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "DDrawDummyClass";
    RegisterClassExA(&wc);

    HWND dummyHwnd = CreateWindowExA(0, wc.lpszClassName, "DDrawDummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL,
                                     wc.hInstance, NULL);

    hr = ddraw7->SetCooperativeLevel(dummyHwnd, DDSCL_NORMAL);
    if (FAILED(hr)) {
        HookLog("DDraw: SetCooperativeLevel failed during bootstrap (hr=0x%08x)", hr);
    }

    InstallDirectDrawHooksForInstance(ddraw7, reason);

    DDSURFACEDESC2 ddsd = {};
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    IDirectDrawSurface7* dummySurface = nullptr;
    hr = ddraw7->CreateSurface(&ddsd, &dummySurface, NULL);
    HookLog("DDraw: Bootstrap CreateSurface returned hr=0x%08x, surface=%p", hr, dummySurface);

    if (SUCCEEDED(hr) && dummySurface) {
        InstallSurfaceHooksForSurface(dummySurface, reason, true);

        IDirect3D7* d3d7 = nullptr;
        if (SUCCEEDED(ddraw7->QueryInterface(kIID_IDirect3D7, (void**)&d3d7))) {
            IDirect3DDevice7* d3d7Device = nullptr;
            if (SUCCEEDED(d3d7->CreateDevice(kIID_IDirect3DHALDevice, dummySurface, &d3d7Device))) {
                void** d3d7DeviceVTable = *(void***)d3d7Device;
                InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api::D3D7, d3d7Device, false,
                                            "bootstrap");

                if (VTableHook::Create(&d3d7DeviceVTable[D3D7_VTABLE_SETRENDERSTATE], (LPVOID)&DetourSetRenderState7,
                                       (LPVOID*)&oSetRenderState7) == VTableHook::Success) {
                    HookLog("DDraw: SetRenderState hook installed");
                }

                d3d7Device->Release();
            } else {
                HookLog("DDraw: Failed to create D3D7 device for hooking");
            }
            d3d7->Release();
        }

        IDirectDrawSurface4* dummySurface4 = nullptr;
        IUnknown* d3d3 = nullptr;
        if (SUCCEEDED(dummySurface->QueryInterface(IID_IDirectDrawSurface4, (void**)&dummySurface4)) &&
            SUCCEEDED(ddraw7->QueryInterface(kIID_IDirect3D3, (void**)&d3d3))) {
            using CreateDevice3_t =
                HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFCLSID, IDirectDrawSurface4*, IUnknown**, IUnknown*);
            void** d3d3VTable = *(void***)d3d3;
            auto createDevice3 = reinterpret_cast<CreateDevice3_t>(d3d3VTable[8]);
            IUnknown* d3d6Device = nullptr;
            if (createDevice3 &&
                SUCCEEDED(createDevice3(d3d3, kIID_IDirect3DHALDevice, dummySurface4, &d3d6Device, nullptr))) {
                InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api::D3D6, d3d6Device, false,
                                            "bootstrap");
                d3d6Device->Release();
            } else {
                HookLog("DDraw: Failed to create D3D6 device for sampler hooking");
            }
        }
        if (d3d3)
            d3d3->Release();
        if (dummySurface4)
            dummySurface4->Release();
    } else {
        HookLog("DDraw: Failed to create primary surface (hr=0x%08x)", hr);
    }

    ddraw7->Release();
    InstallDirectDrawCreateInlineHook(pDirectDrawCreate);
    InstallDirectDrawCreateExInlineHook(pDirectDrawCreateEx);

    DestroyWindow(dummyHwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);

    g_HooksInitialized = true;
    HookLog("DDrawHook: Hooks installed");
}

// DirectDraw Capture class
class DDrawCapture : public HookCaptureBase {
public:
    std::recursive_mutex captureMutex;

    // D3D9Ex wrapper for GPU sharing
    IDirect3D9Ex* d3d9Ex = nullptr;
    IDirect3DDevice9Ex* d3d9DeviceEx = nullptr;
    IDirect3DSurface9* d3d9FastUploadSurface = nullptr;
    IDirect3DSurface9* d3d9UploadSurface = nullptr;
    bool d3d9UsesFlipEx = false;

    // D3D11 for shared texture
    ID3D11Device* d3d11Device = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    ID3D11Texture2D* stagingTexture = nullptr;
    ID3D11Texture2D* sharedTextures[CAPTURE_TEXTURE_COUNT]{};

    // D3D11.3 Fence support
    ID3D11Fence* fence = nullptr;
    ID3D11DeviceContext4* context4 = nullptr;
    bool useFences = false;
    UINT64 fenceValue = 0;

    // Surface info
    IDirectDrawSurface7* ddrawSurface = nullptr;
    HWND targetHwnd = NULL;

    void ReleaseOverlayResources() {
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

    void Cleanup() override {
        CleanupDDraw(false);
    }

    bool CleanupDDraw(bool force = false) {
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

    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
        // Implemented in Init
    }

    bool CreateD3D11Device() {
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

    bool CreateStagingTexture() {
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

    bool CreateSharedTextures() {
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

    bool CreateD3D9ExWrapper(HWND hwnd) {
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

    bool UploadOverlaySurfaceToBackbuffer() {
        IDirect3DSurface9* backBuffer = nullptr;
        HRESULT hr = d3d9DeviceEx->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr) || !backBuffer) {
            static int backBufferFailLogCount = 0;
            if (backBufferFailLogCount < 4) {
                HookLog("DDraw: Failed to get helper backbuffer for overlay composite (hr=0x%08x)", hr);
                backBufferFailLogCount++;
            }
            return false;
        }

        hr = d3d9DeviceEx->UpdateSurface(d3d9UploadSurface, nullptr, backBuffer, nullptr);
        backBuffer->Release();

        if (FAILED(hr)) {
            static int updateSurfaceFailLogCount = 0;
            if (updateSurfaceFailLogCount < 4) {
                HookLog("DDraw: Failed to upload DD surface into helper backbuffer (hr=0x%08x)", hr);
                updateSurfaceFailLogCount++;
            }
            return false;
        }

        return true;
    }

    bool StretchOverlaySurfaceToBackbuffer(IDirect3DSurface9* surface) {
        if (!surface) {
            return false;
        }

        IDirect3DSurface9* backBuffer = nullptr;
        HRESULT hr = d3d9DeviceEx->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr) || !backBuffer) {
            static int backBufferFailLogCount = 0;
            if (backBufferFailLogCount < 4) {
                HookLog("DDraw: Failed to get helper backbuffer for fast overlay composite (hr=0x%08x)", hr);
                backBufferFailLogCount++;
            }
            return false;
        }

        hr = d3d9DeviceEx->StretchRect(surface, nullptr, backBuffer, nullptr, D3DTEXF_NONE);
        backBuffer->Release();

        if (FAILED(hr)) {
            static int stretchRectFailLogCount = 0;
            if (stretchRectFailLogCount < 4) {
                HookLog("DDraw: Failed to stretch DD surface into helper backbuffer (hr=0x%08x)", hr);
                stretchRectFailLogCount++;
            }
            return false;
        }

        return true;
    }

    bool CopyLockedSurfaceToUploadSurface(const DDSURFACEDESC2& desc) {
        if (!desc.lpSurface || desc.dwWidth != width || desc.dwHeight != height ||
