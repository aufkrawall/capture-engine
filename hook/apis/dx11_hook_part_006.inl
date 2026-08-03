    InstallContextVTableHookSlot11(pContextVTable, 8, (LPVOID)&DetourPSSetShaderResources11, oPSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::psSetShaderResources, "PSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 9, (LPVOID)&DetourPSSetShader11, oPSSetShader11,
                                   &D3D11ContextVTableOriginals::psSetShader, "PSSetShader", source);
    InstallContextVTableHookSlot11(pContextVTable, 10, (LPVOID)&DetourPSSetSamplers11, oPSSetSamplers11,
                                   &D3D11ContextVTableOriginals::psSetSamplers, "PSSetSamplers", source);
    InstallContextVTableHookSlot11(pContextVTable, 12, (LPVOID)&DetourDrawIndexed11, oDrawIndexed11,
                                   &D3D11ContextVTableOriginals::drawIndexed, "DrawIndexed", source);
    InstallContextVTableHookSlot11(pContextVTable, 13, (LPVOID)&DetourDraw11, oDraw11,
                                   &D3D11ContextVTableOriginals::draw, "Draw", source);
    InstallContextVTableHookSlot11(pContextVTable, 20, (LPVOID)&DetourDrawIndexedInstanced11, oDrawIndexedInstanced11,
                                   &D3D11ContextVTableOriginals::drawIndexedInstanced, "DrawIndexedInstanced", source);
    InstallContextVTableHookSlot11(pContextVTable, 21, (LPVOID)&DetourDrawInstanced11, oDrawInstanced11,
                                   &D3D11ContextVTableOriginals::drawInstanced, "DrawInstanced", source);
    InstallContextVTableHookSlot11(pContextVTable, 25, (LPVOID)&DetourVSSetShaderResources11, oVSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::vsSetShaderResources, "VSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 26, (LPVOID)&DetourVSSetSamplers11, oVSSetSamplers11,
                                   &D3D11ContextVTableOriginals::vsSetSamplers, "VSSetSamplers", source);
    InstallContextVTableHookSlot11(pContextVTable, 31, (LPVOID)&DetourGSSetShaderResources11, oGSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::gsSetShaderResources, "GSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 32, (LPVOID)&DetourGSSetSamplers11, oGSSetSamplers11,
                                   &D3D11ContextVTableOriginals::gsSetSamplers, "GSSetSamplers", source);
    InstallContextVTableHookSlot11(pContextVTable, 38, (LPVOID)&DetourDrawAuto11, oDrawAuto11,
                                   &D3D11ContextVTableOriginals::drawAuto, "DrawAuto", source);
    InstallContextVTableHookSlot11(
        pContextVTable, 39, (LPVOID)&DetourDrawIndexedInstancedIndirect11, oDrawIndexedInstancedIndirect11,
        &D3D11ContextVTableOriginals::drawIndexedInstancedIndirect, "DrawIndexedInstancedIndirect", source);
    InstallContextVTableHookSlot11(pContextVTable, 40, (LPVOID)&DetourDrawInstancedIndirect11, oDrawInstancedIndirect11,
                                   &D3D11ContextVTableOriginals::drawInstancedIndirect, "DrawInstancedIndirect",
                                   source);
    InstallContextVTableHookSlot11(pContextVTable, 58, (LPVOID)&DetourExecuteCommandList11, oExecuteCommandList11,
                                   &D3D11ContextVTableOriginals::executeCommandList, "ExecuteCommandList", source);
    InstallContextVTableHookSlot11(pContextVTable, 59, (LPVOID)&DetourHSSetShaderResources11, oHSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::hsSetShaderResources, "HSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 61, (LPVOID)&DetourHSSetSamplers11, oHSSetSamplers11,
                                   &D3D11ContextVTableOriginals::hsSetSamplers, "HSSetSamplers", source);
    InstallContextVTableHookSlot11(pContextVTable, 63, (LPVOID)&DetourDSSetShaderResources11, oDSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::dsSetShaderResources, "DSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 65, (LPVOID)&DetourDSSetSamplers11, oDSSetSamplers11,
                                   &D3D11ContextVTableOriginals::dsSetSamplers, "DSSetSamplers", source);
    InstallContextVTableHookSlot11(pContextVTable, 67, (LPVOID)&DetourCSSetShaderResources11, oCSSetShaderResources11,
                                   &D3D11ContextVTableOriginals::csSetShaderResources, "CSSetShaderResources", source);
    InstallContextVTableHookSlot11(pContextVTable, 70, (LPVOID)&DetourCSSetSamplers11, oCSSetSamplers11,
                                   &D3D11ContextVTableOriginals::csSetSamplers, "CSSetSamplers", source);
}

// Helper to install vtable hooks
static void InstallVTableHooks(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain) {
    // Hook D3D11 Device methods
    if (pDevice) {
        DX11Hook_RegisterDeviceIdentity(pDevice, "D3D11 device hook installation");
        InstallD3D11IdentityQueryHook(pDevice, "device");
        void** pDeviceVTable = *(void***)pDevice;
        EnsureVTableHookSlot11(pDeviceVTable, 15, (LPVOID)&DetourCreatePixelShader11, oCreatePixelShader11,
                               "CreatePixelShader");
        // Index 23 is CreateSamplerState for D3D11
        EnsureVTableHookSlot11(pDeviceVTable, 23, (LPVOID)&DetourCreateSamplerState, oCreateSamplerState,
                               "CreateSamplerState");
        EnsureVTableHookSlot11(pDeviceVTable, 27, (LPVOID)&DetourCreateDeferredContext11, oCreateDeferredContext11,
                               "CreateDeferredContext");
    }

    InstallD3D11IdentityQueryHook(pContext, "context");
    InstallContextVTableHooks11(pContext, "immediate");

    // Some DX11 implementations expose D3D10 compatibility interfaces too.
    // Only install the D3D10 runtime hooks when the swapchain actually belongs
    // to a D3D10 device.
    if (pSwapChain && DetectSwapChainAPITypeForDX11Hook(pSwapChain) == DXGIShared::APIType::D3D10) {
        ID3D10Device* pDevice10 = nullptr;
        HRESULT hr = pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&pDevice10);
        if (SUCCEEDED(hr) && pDevice10) {
            void** pDeviceVTable = *(void***)pDevice10;

            // CreateSamplerState (Index 9)
            if (oCreateSamplerState10 == NULL) {
                if (VTableHook::Create(reinterpret_cast<void*>(&pDeviceVTable[9]), (LPVOID)&DetourCreateSamplerState10,
                                       (LPVOID*)&oCreateSamplerState10) == VTableHook::Success) {
                    HookLog("DX10: CreateSamplerState hook installed");
                }
            }
            pDevice10->Release();
        }
    }
}

// Update metrics for wrapper calls
void DX11_UpdatePerformanceMetrics(int64_t qpcUs) {
    if (auto* m = DXGIShared::GetPerformanceMetrics()) {
        m->Update(qpcUs);
    }
}

void CleanupDX11Resources(bool releaseDeviceContext) {
    ReleaseTrackedShaderResources11();

    // When the window is being destroyed (releaseDeviceContext=false), skip ALL
    // releases because the underlying D3D device is already being torn down.
    // Calling Release() on any resource (even RTVs) can crash at this point.
    if (releaseDeviceContext) {
        if (g_mainRenderTargetView) {
            g_mainRenderTargetView->Release();
            g_mainRenderTargetView = nullptr;
        }
        if (g_mainRenderTargetView10) {
            g_mainRenderTargetView10->Release();
            g_mainRenderTargetView10 = nullptr;
        }
    } else {
        g_mainRenderTargetView = nullptr;
        g_mainRenderTargetView10 = nullptr;
    }

    // CRITICAL: Only call Shutdown() when doing normal cleanup (resize, etc.)
    // During app shutdown (releaseDeviceContext=false), skip Shutdown() - the
    // OverlayAdapter destructor will handle cleanup by leaking memory since
    // we've set skipDeviceRelease=true.
    if (releaseDeviceContext && g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    if (releaseDeviceContext) {
        if (g_pd3dDeviceContext) {
            g_pd3dDeviceContext->Release();
            g_pd3dDeviceContext = nullptr;
        }
        if (g_pd3dDevice) {
            g_pd3dDevice->Release();
            g_pd3dDevice = nullptr;
        }
        if (g_D3D11IdentityDevice) {
            g_D3D11IdentityDevice->Release();
            g_D3D11IdentityDevice = nullptr;
        }
        g_D3D11IdentitySwapChain = nullptr;
    } else {
        g_pd3dDeviceContext = nullptr;
        g_pd3dDevice = nullptr;
        g_D3D11IdentityDevice = nullptr;
        g_D3D11IdentitySwapChain = nullptr;
    }
}

extern void DrawDX11Overlay(IDXGISwapChain* pSwapChain);

void HandleDX11ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame) {
    (void)isRealFrame;
    g_FGCompat.RecordPresentForNvidiaSmoothMotion();
    ce::overlay_metrics::PublishDetectedOverlayFGMetrics(DXGIShared::GetPerformanceMetrics(),
                                                         "DX11::HandleProcessFrame");
    ProcessDX11FrameWithOverlayOrdering(pSwapChain);
}

void HandleDX11ResizeBegin() {
    CleanupDX11Resources();
}
static HWND g_CachedHwnd = NULL;

// Reentrancy guard for ResizeBuffers (Recursion Breaker)
thread_local int g_ResizeBuffersDepth = 0;

// DX11 Capture Implementation extending HookCaptureBase
// Supports both DX11 and DX10 games.
// DX10 capture must copy on the real D3D10 device and publish DXGI shared
// texture handles that the media-side D3D11 device can open.
class DX11Capture : public HookCaptureBase {
public:
    std::recursive_mutex captureMutex;
    ID3D11Texture2D* sharedTextures[CAPTURE_TEXTURE_COUNT]{};
    ID3D11Query* copyQueries[CAPTURE_TEXTURE_COUNT]{};  // GPU sync queries
    ID3D11Device* cachedDevice = nullptr;
    ID3D11DeviceContext* cachedContext = nullptr;
    IUnknown* cachedSwapChainIdentity = nullptr;
    bool generationResetPending = false;

    // DX10 capture must stay on the real D3D10 device to avoid invalid
    // cross-device copies from a DX10 swapchain buffer into D3D11-owned
    // textures, which can produce corrupted output.
    ID3D10Device* cachedDevice10 = nullptr;
    ID3D10Texture2D* sharedTextures10[CAPTURE_TEXTURE_COUNT]{};
    ID3D10Query* copyQueries10[CAPTURE_TEXTURE_COUNT]{};
    bool isDX10Mode = false;

    // For DXVK games: the game's D3D11 is DXVK's (Vulkan-backed). Shared handles
    // from DXVK are Vulkan-internal IDs the real encoder D3D11 can't open.
    // Fix: create ring buffer textures in a real system D3D11 device (ownedDevice),
    // then import each into DXVK's device for CopyResource.
    ID3D11Device* ownedDevice = nullptr;
    ID3D11DeviceContext* ownedContext = nullptr;
    bool isDXVKMode = false;
    ID3D11Texture2D* dxvkImportedTextures[CAPTURE_TEXTURE_COUNT]{};  // DXVK-side imports

    // DX11.3 Fence Support
    ID3D11Fence* fence = nullptr;
    ID3D11DeviceContext4* context4 = nullptr;  // Needed for Signal
    bool useFences = false;
    UINT64 fenceValue = 0;
    UINT64 slotFenceValues[CAPTURE_TEXTURE_COUNT]{};

    // Keyed Mutex Support (Proper Fix)
    IDXGIKeyedMutex* keyedMutexes[CAPTURE_TEXTURE_COUNT]{};
    bool useKeyedMutex = false;
    bool sharedTextureHandlesAreNt[CAPTURE_TEXTURE_COUNT]{};

    // sharedTextureHandles are in base class

    void Cleanup() override {
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

    void RequestGenerationReset(IDXGISwapChain* swapChain) {
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

    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
        // This virtual method is called by CheckCaptureInit or manually
        // We need the device to create resources, so we'll store it in Init
    }

    // Initialize for DX10 games - capture stays on the real D3D10 device and
    // publishes DXGI shared handles that the media-side D3D11 device opens.
    bool InitDX10(IDXGISwapChain* swapChain) {
        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        ID3D10Device* device10 = nullptr;

        if (FAILED(swapChain->GetDevice(__uuidof(ID3D10Device), (void**)&device10))) {
            ID3D10Device1* device10_1 = nullptr;
            if (FAILED(swapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&device10_1)) || !device10_1) {
                HookLog("DX10: Failed to get device from swapchain");
                return false;
            }
            device10 = device10_1;
        }

        if (!device10) {
            HookLog("DX10: Swapchain returned null device");
            return false;
        }

        cachedDevice10 = device10;  // Keep GetDevice() reference until Cleanup()

        if (SUCCEEDED(device10->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC adapterDesc;
                if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    luidLow = adapterDesc.AdapterLuid.LowPart;
                    luidHigh = adapterDesc.AdapterLuid.HighPart;

                    SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);
                    SystemMetricsCollector::Get().SetVRAMTotal(adapterDesc.DedicatedVideoMemory);
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }

        isDX10Mode = true;
        return true;
    }

    // Detect if the loaded d3d11.dll is DXVK's (not the system one).
    // DXVK games place their d3d11.dll in the game directory, shadowing System32.
    static bool IsCurrentD3D11DXVK() {
        HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
        if (!hD3D11)
            return false;
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA(hD3D11, path, MAX_PATH))
            return false;
        char sysDir[MAX_PATH] = {};
        if (!GetSystemDirectoryA(sysDir, MAX_PATH))
            return false;
        uint32_t sysLen = (uint32_t)strlen(sysDir);
        return !(_strnicmp(path, sysDir, sysLen) == 0 && path[sysLen] == '\\');
    }

    // Create a real system D3D11 device on the GPU identified by a LUID.
    // Used for DXVK games so ring buffer textures carry valid Windows NT handles.
    bool CreateSystemD3D11DeviceForLUID(int32_t luidLowPart, int32_t luidHighPart) {
        char systemDir[MAX_PATH] = {};
        if (GetSystemDirectoryA(systemDir, MAX_PATH) == 0)
            return false;
        std::string dxgiPath = std::string(systemDir) + "\\dxgi.dll";
        std::string d3d11Path = std::string(systemDir) + "\\d3d11.dll";

        HMODULE hDXGI = LoadLibraryA(dxgiPath.c_str());
        if (!hDXGI) {
            EarlyLog("DX11-DXVK: System DXGI not found");
            return false;
        }
        typedef HRESULT(WINAPI * PFN_CREATE_DXGI_FACTORY1)(REFIID, void**);
        PFN_CREATE_DXGI_FACTORY1 pCreateDXGIFactory1 =
            (PFN_CREATE_DXGI_FACTORY1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
        if (!pCreateDXGIFactory1)
            return false;

        IDXGIFactory1* factory = nullptr;
        if (FAILED(pCreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;

        // Find the real adapter by LUID
        IDXGIAdapter1* adapter = nullptr;
        IDXGIAdapter1* matchedAdapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == (DWORD)luidLowPart && desc.AdapterLuid.HighPart == luidHighPart) {
                matchedAdapter = adapter;
                break;
            }
            adapter->Release();
        }
        factory->Release();

        if (!matchedAdapter) {
            EarlyLog("DX11-DXVK: No system DXGI adapter found for LUID");
            return false;
        }

        HMODULE hD3D11 = LoadLibraryA(d3d11Path.c_str());
        if (!hD3D11) {
            matchedAdapter->Release();
            EarlyLog("DX11-DXVK: System D3D11 not found");
            return false;
        }
        // Redirect system d3d11.dll's dxgi.dll IAT to system dxgi.dll
        RedirectModuleImports(hD3D11, "dxgi.dll", hDXGI);

        typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                          const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                                          D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice =
            (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
            matchedAdapter->Release();
            return false;
        }

        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL featureLevel;
        HRESULT hr = pD3D11CreateDevice(matchedAdapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, featureLevels, 2,
                                        D3D11_SDK_VERSION, &ownedDevice, &featureLevel, &ownedContext);
        matchedAdapter->Release();
        if (FAILED(hr)) {
            EarlyLog("DX11-DXVK: Failed to create system D3D11 device (hr=0x%08x)", hr);
            return false;
        }
        EarlyLog("DX11-DXVK: System D3D11 device created for cross-device capture (fl=%d)", featureLevel);
        return true;
    }

    void Init(ID3D11Device* device, IDXGISwapChain* swapChain) {
        if (initialized)
            return;

        if (!cachedSwapChainIdentity) {
            const HRESULT identityHr =
                swapChain ? swapChain->QueryInterface(IID_PPV_ARGS(&cachedSwapChainIdentity)) : E_POINTER;
            if (FAILED(identityHr) || !cachedSwapChainIdentity) {
                EarlyLog("%s Capture Init: failed to retain swapchain identity hr=0x%08X", isDX10Mode ? "DX10" : "DX11",
                         identityHr);
                Cleanup();
                return;
            }
        }

        DXGI_SWAP_CHAIN_DESC desc = {};
        const HRESULT descHr = swapChain ? swapChain->GetDesc(&desc) : E_POINTER;
        if (SUCCEEDED(descHr) && (desc.BufferDesc.Width == 0 || desc.BufferDesc.Height == 0 ||
                                  desc.BufferDesc.Format == DXGI_FORMAT_UNKNOWN)) {
            // Width/height may have been HWND-derived at creation. Resolve the
            // concrete values from the actual buffer before rejecting capture.
            if (isDX10Mode) {
                ID3D10Texture2D* buffer = nullptr;
                if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))) && buffer) {
                    D3D10_TEXTURE2D_DESC bufferDesc = {};
                    buffer->GetDesc(&bufferDesc);
                    desc.BufferDesc.Width = bufferDesc.Width;
                    desc.BufferDesc.Height = bufferDesc.Height;
                    desc.BufferDesc.Format = bufferDesc.Format;
                    buffer->Release();
                }
            } else {
                ID3D11Texture2D* buffer = nullptr;
                if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))) && buffer) {
                    D3D11_TEXTURE2D_DESC bufferDesc = {};
                    buffer->GetDesc(&bufferDesc);
                    desc.BufferDesc.Width = bufferDesc.Width;
                    desc.BufferDesc.Height = bufferDesc.Height;
                    desc.BufferDesc.Format = bufferDesc.Format;
                    buffer->Release();
                }
            }
        }
        if (FAILED(descHr) || desc.BufferDesc.Width == 0 || desc.BufferDesc.Height == 0 ||
            desc.BufferDesc.Format == DXGI_FORMAT_UNKNOWN) {
            EarlyLog("%s Capture Init: invalid swapchain description hr=0x%08X size=%ux%u format=%d",
                     isDX10Mode ? "DX10" : "DX11", descHr, desc.BufferDesc.Width, desc.BufferDesc.Height,
                     static_cast<int>(desc.BufferDesc.Format));
            Cleanup();
            return;
        }

        width = desc.BufferDesc.Width;
        height = desc.BufferDesc.Height;
        format = desc.BufferDesc.Format;

        // Determine which device to use for creating textures
        ID3D11Device* captureDevice = device;

        if (isDX10Mode) {
            if (!cachedDevice10) {
                EarlyLog("DX10: Missing cached D3D10 device during capture init");
                return;
            }

            D3D10_TEXTURE2D_DESC texDesc10 = {};
            texDesc10.Width = width;
            texDesc10.Height = height;
            texDesc10.MipLevels = 1;
            texDesc10.ArraySize = 1;
            texDesc10.Format = (DXGI_FORMAT)format;
            texDesc10.SampleDesc.Count = 1;
            texDesc10.Usage = D3D10_USAGE_DEFAULT;
            texDesc10.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
            texDesc10.CPUAccessFlags = 0;
            texDesc10.MiscFlags = D3D10_RESOURCE_MISC_SHARED;

            useFences = false;
            sharedFenceHandle.store(NULL, std::memory_order_release);

            bool success = true;
            for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
                HRESULT hr = cachedDevice10->CreateTexture2D(&texDesc10, NULL, &sharedTextures10[i]);
                if (SUCCEEDED(hr) && sharedTextures10[i]) {
                    IDXGIResource* resource = nullptr;
                    if (SUCCEEDED(sharedTextures10[i]->QueryInterface(IID_PPV_ARGS(&resource))) && resource) {
                        HANDLE hTemp = NULL;
                        hr = resource->GetSharedHandle(&hTemp);
                        sharedTextureHandles[i].store(hTemp, std::memory_order_release);
                        sharedTextureHandlesAreNt[i] = false;
                        resource->Release();
                    } else {
                        EarlyLog("DX10: Error - Failed to query IDXGIResource for texture %d", i);
                        success = false;
                    }

                    if (sharedTextureHandles[i].load(std::memory_order_acquire) == NULL) {
                        EarlyLog("DX10: Critical - Shared handle is NULL for texture %d", i);
                        success = false;
                    }
                } else {
                    success = false;
                    HookLog("DX10: Failed to create shared texture %d (hr=0x%08x)", i, hr);
                }
            }

            if (success) {
                D3D10_QUERY_DESC queryDesc = {};
                queryDesc.Query = D3D10_QUERY_EVENT;
                queryDesc.MiscFlags = 0;

                for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
                    HRESULT queryHr = cachedDevice10->CreateQuery(&queryDesc, &copyQueries10[i]);
                    if (FAILED(queryHr)) {
                        HookLog("DX10: Failed to create copy query %d (hr=0x%08x)", i, queryHr);
                        copyQueries10[i] = nullptr;
                    }
                }
            }

            if (success) {
                if (g_IPC) {
                    PublishToSharedMemory(g_IPC);
                }
                initialized = true;
                HookLogImportant("DX10 Capture Initialized: %dx%d (Fence: OFF, Queries: %s, DXVK: OFF)", width, height,
                                 (copyQueries10[0] != nullptr) ? "ON" : "OFF");
            } else {
                EarlyLog("DX10 Capture Init FAILED (success=false)");
                Cleanup();
            }
            return;
        } else {
            // Get LUID from DX11 device
            IDXGIDevice* dxgiDevice = nullptr;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
                IDXGIAdapter* adapter = nullptr;
                if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                    DXGI_ADAPTER_DESC adapterDesc;
                    adapter->GetDesc(&adapterDesc);
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    luidLow = adapterDesc.AdapterLuid.LowPart;
                    luidHigh = adapterDesc.AdapterLuid.HighPart;

                    // Initialize SystemMetricsCollector with adapter LUID for GPU stats
                    SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);

                    adapter->Release();
                }
                dxgiDevice->Release();
            }
            cachedDevice = device;
            device->GetImmediateContext(&cachedContext);

            // Detect DXVK: if d3d11.dll is not from System32, this is DXVK.
            // DXVK's GetSharedHandle returns Vulkan-internal IDs, not valid Windows
            // NT handles. Fix: create a real system D3D11 device on the same GPU
            // for ring buffer textures, then import them into DXVK for CopyResource.
            if (IsCurrentD3D11DXVK() && !isDXVKMode) {
                EarlyLog("DX11: DXVK d3d11 detected - creating system D3D11 for ring buffer");
                if (CreateSystemD3D11DeviceForLUID(luidLow, luidHigh)) {
                    isDXVKMode = true;
                    captureDevice = ownedDevice;  // Ring buffer textures come from real D3D11
                    // cachedDevice/cachedContext remain as DXVK's for CopyResource
                } else {
                    EarlyLog("DX11: DXVK detected but system D3D11 creation failed, capture may produce bad handles");
                }
            }
        }

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = (DXGI_FORMAT)format;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.CPUAccessFlags = 0;

        // Use plain shared textures with NT Handles for cross-process sharing
        // Synchronization is handled via D3D11 Fence (DX11.3+) or no sync fallback
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        useKeyedMutex = false;  // Disabled - using Fence instead

        // Try to create D3D11 Fence for async GPU synchronization (DX11.3+)
        // Skip for DXVK: fence lives in system D3D11 device but copy happens via
        // DXVK context - fence can't be signaled cross-device from DXVK.
        ID3D11Device5* device5 = nullptr;
        D3D11InternalIdentityProbeScope identityProbeScope;
        if (!isDXVKMode && SUCCEEDED(captureDevice->QueryInterface(IID_PPV_ARGS(&device5)))) {
            HRESULT hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
            if (SUCCEEDED(hr)) {
                // Get shared fence handle for cross-process
                HANDLE hTemp = NULL;
                hr = fence->CreateSharedHandle(NULL, GENERIC_ALL, NULL, &hTemp);
                sharedFenceHandle.store(hTemp, std::memory_order_release);
                if (SUCCEEDED(hr)) {
                    // Get Context4 for Signal()
                    ID3D11DeviceContext* immCtx = nullptr;
                    captureDevice->GetImmediateContext(&immCtx);
                    if (immCtx && SUCCEEDED(immCtx->QueryInterface(IID_PPV_ARGS(&context4)))) {
                        useFences = true;
                        EarlyLog("DX11: D3D11 Fence created (async GPU sync enabled)");
                    } else {
                        EarlyLog("DX11: Warning - ID3D11DeviceContext4 not available; using implicit shared sync");
                        HANDLE unusableFenceHandle = sharedFenceHandle.exchange(NULL, std::memory_order_acq_rel);
                        if (unusableFenceHandle)
                            CloseHandle(unusableFenceHandle);
                        fence->Release();
                        fence = nullptr;
                    }
                    if (immCtx)
                        immCtx->Release();
                } else {
                    EarlyLog("DX11: Warning - Fence shared handle creation failed");
                    fence->Release();
                    fence = nullptr;
