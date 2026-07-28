    // hooked correctly)
    return oPresent(pSwapChain, SyncInterval, PresentFlags);
}

static HRESULT WINAPI DetourD3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType,
                                                          HMODULE Software, UINT Flags,
                                                          const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                                          UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                          IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                                          D3D_FEATURE_LEVEL* pFeatureLevel,
                                                          ID3D11DeviceContext** ppImmediateContext) {
    // Unconditional log to verify hook is being called
    HookLog("DetourD3D11CreateDeviceAndSwapChain: ENTER");

    if (pSwapChainDesc) {
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
            EarlyLog("DX11: D3D11CreateDeviceAndSwapChain called. Width=%u Height=%u", pSwapChainDesc->BufferDesc.Width,
                     pSwapChainDesc->BufferDesc.Height);
        }
    } else {
        EarlyLog("DX11: D3D11CreateDeviceAndSwapChain called (pSwapChainDesc=NULL)");
    }

    DXGI_SWAP_CHAIN_DESC desc;
    const DXGI_SWAP_CHAIN_DESC* pFinalDesc = pSwapChainDesc;

    if (pSwapChainDesc) {
        const GraphicsConfig& gfx = GetActiveGraphicsConfig();
        desc = *pSwapChainDesc;
        bool modified = false;

        // Backbuffer Count
        modified = ApplyDX11BackbufferCountOverride(desc, "CreateDeviceAndSwapChain") || modified;

        // MSAA Override
        const char* msaa = gfx.msaaSamples.c_str();
        if (msaa[0] != 'd') {
            if (strcmp(msaa, "off") == 0) {
                desc.SampleDesc.Count = 1;
                desc.SampleDesc.Quality = 0;
                modified = true;
                HookLog("DX11: CreateDeviceAndSwapChain: Forcing MSAA OFF");
            } else {
                UINT samples = 1;
                if (strcmp(msaa, "2x") == 0)
                    samples = 2;
                else if (strcmp(msaa, "4x") == 0)
                    samples = 4;
                else if (strcmp(msaa, "8x") == 0)
                    samples = 8;

                if (samples > 1) {
                    desc.SampleDesc.Count = samples;
                    desc.SampleDesc.Quality = 0;
                    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;  // MSAA requires DISCARD in D3D11
                    modified = true;
                    HookLog("DX11: CreateDeviceAndSwapChain: Forcing MSAA %dx", samples);
                }
            }
        }

        if (modified)
            pFinalDesc = &desc;
    }

    // Use the local copy of the real original.  The shared oD3D11CreateDeviceAndSwapChain
    // (from dx11_hook.h) may have been overwritten by HookExport -> PatchIATAllModules,
    // which sets it to Wrapped_D3D11CreateDeviceAndSwapChain when a prior IAT hook exists.
    // Calling that would re-enter the wrapper -> infinite recursion -> stack overflow.
    PFN_D3D11CreateDeviceAndSwapChain realOriginal =
        s_oRealD3D11CreateDeviceAndSwapChain ? s_oRealD3D11CreateDeviceAndSwapChain : oD3D11CreateDeviceAndSwapChain;
    HRESULT hr = realOriginal(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
                              pFinalDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);

    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        DX11Hook_RegisterDeviceIdentity(*ppDevice, "D3D11CreateDeviceAndSwapChain", true);
        const GraphicsConfig& gfx = GetActiveGraphicsConfig();
        if (ppSwapChain && *ppSwapChain && HasBackbufferCountOverride(gfx.backbufferCount)) {
            DXGI_SWAP_CHAIN_DESC actualDesc = {};
            if (SUCCEEDED((*ppSwapChain)->GetDesc(&actualDesc))) {
                HookLogImportant(
                    "DX11: CreateDeviceAndSwapChain actual BufferCount=%u requested=%d "
                    "size=%ux%u swapEffect=%d",
                    actualDesc.BufferCount, gfx.backbufferCount, actualDesc.BufferDesc.Width,
                    actualDesc.BufferDesc.Height, actualDesc.SwapEffect);
            }
        }

        // Wrap the swapchain returned by D3D11CreateDeviceAndSwapChain so our
        // wrapper captures Present
        if (ppSwapChain && *ppSwapChain) {
            IUnknown* pDev = (ppDevice && *ppDevice) ? *ppDevice : nullptr;
            IDXGISwapChain* pReal = *ppSwapChain;
            *ppSwapChain = (IDXGISwapChain*)new CWrapDXGISwapChain(pReal, pDev);
            pReal->Release();
            HookLogImportant("DX11: Wrapped swapchain from D3D11CreateDeviceAndSwapChain");
        }

        IDXGISwapChain* sc = (ppSwapChain && *ppSwapChain) ? *ppSwapChain : nullptr;
        ID3D11DeviceContext* ctx = (ppImmediateContext && *ppImmediateContext) ? *ppImmediateContext : nullptr;
        // If immediate context not provided, get it from device
        if (!ctx)
            (*ppDevice)->GetImmediateContext(&ctx);  // AddRef'd

        InstallVTableHooks(*ppDevice, ctx, sc);

        // Note: Factory vtable hooks removed - wrappers handle swapchain creation

        if (!ppImmediateContext && ctx)
            ctx->Release();

        // Explicitly set VRAM Total to prevent background thread crash
        if (ppDevice && *ppDevice) {
            IDXGIDevice* dxgiDevice = nullptr;
            if (SUCCEEDED((*ppDevice)->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) {
                IDXGIAdapter* adapter = nullptr;
                if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                    DXGI_ADAPTER_DESC desc;
                    if (SUCCEEDED(adapter->GetDesc(&desc))) {
                        SystemMetricsCollector::Get().SetVRAMTotal(desc.DedicatedVideoMemory);
                    }
                    adapter->Release();
                }
                dxgiDevice->Release();
            }
        }
    }

    return hr;
}

// Exported version of the detour for IAT patching access
HRESULT WINAPI DX11_DetourCreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                                   UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels,
                                                   UINT FeatureLevels, UINT SDKVersion,
                                                   const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                   IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                                   D3D_FEATURE_LEVEL* pFeatureLevel,
                                                   ID3D11DeviceContext** ppImmediateContext) {
    return DetourD3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                                               SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel,
                                               ppImmediateContext);
}

void DX11Hook_OnSwapChainCreated(IDXGISwapChain* pSwapChain);

// Update metrics for wrapper calls
void DX11_UpdatePerformanceMetrics(int64_t qpcUs);

void DX11Hook_OnSwapChainCreated(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return;

    // Check if it's really a D3D11 swapchain
    ID3D11Device* pDevice = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&pDevice))) {
        ID3D11DeviceContext* pContext = nullptr;
        pDevice->GetImmediateContext(&pContext);

        HookLog("DX11: Manual Hook Activation triggered for SwapChain %p", pSwapChain);
        InstallVTableHooks(pDevice, pContext, pSwapChain);

        if (pContext)
            pContext->Release();
        pDevice->Release();
    }
}

void DX11Hook_InstallDeviceAndContextHooks(ID3D11Device* pDevice, ID3D11DeviceContext* pContext,
                                           IDXGISwapChain* pSwapChain) {
    HookLog("DX11: InstallDeviceAndContextHooks device=%p context=%p swapChain=%p", pDevice, pContext, pSwapChain);
    InstallVTableHooks(pDevice, pContext, pSwapChain);
}

static HRESULT WINAPI DetourD3D10CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType,
                                                          HMODULE Software, UINT Flags, UINT SDKVersion,
                                                          DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                          IDXGISwapChain** ppSwapChain, ID3D10Device** ppDevice) {
    EarlyLog("DX10: D3D10CreateDeviceAndSwapChain called");

    DXGI_SWAP_CHAIN_DESC desc;
    DXGI_SWAP_CHAIN_DESC* pFinalDesc = pSwapChainDesc;

    if (pSwapChainDesc) {
        const GraphicsConfig& gfx = GetActiveGraphicsConfig();
        desc = *pSwapChainDesc;
        bool modified = false;

        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            bool isFlip = (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (!(isFlip && (UINT)count < desc.BufferCount)) {
                desc.BufferCount = (UINT)count;
                modified = true;
            }
        }

        if (modified)
            pFinalDesc = &desc;
    }

    HRESULT hr = oD3D10CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, SDKVersion, pFinalDesc,
                                                ppSwapChain, ppDevice);

    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        DX10Hook_RegisterDeviceIdentity(*ppDevice, false, "D3D10CreateDeviceAndSwapChain");
    }
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        DX10Hook_RegisterSwapChainIdentity(*ppSwapChain, false, "D3D10CreateDeviceAndSwapChain");
        InstallVTableHooks(NULL, NULL, *ppSwapChain);
    }
    return hr;
}

static HRESULT WINAPI DetourD3D10CreateDeviceAndSwapChain1(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType,
                                                           HMODULE Software, UINT Flags,
                                                           D3D10_FEATURE_LEVEL1 HardwareLevel, UINT SDKVersion,
                                                           DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                           IDXGISwapChain** ppSwapChain, ID3D10Device1** ppDevice) {
    EarlyLog("DX10.1: D3D10CreateDeviceAndSwapChain1 called");

    DXGI_SWAP_CHAIN_DESC desc;
    DXGI_SWAP_CHAIN_DESC* pFinalDesc = pSwapChainDesc;

    if (pSwapChainDesc) {
        const GraphicsConfig& gfx = GetActiveGraphicsConfig();
        desc = *pSwapChainDesc;
        bool modified = false;

        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            bool isFlip = (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (!(isFlip && (UINT)count < desc.BufferCount)) {
                desc.BufferCount = (UINT)count;
                modified = true;
            }
        }

        if (modified)
            pFinalDesc = &desc;
    }

    HRESULT hr = oD3D10CreateDeviceAndSwapChain1(pAdapter, DriverType, Software, Flags, HardwareLevel, SDKVersion,
                                                 pFinalDesc, ppSwapChain, ppDevice);

    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        DX10Hook_RegisterDeviceIdentity(*ppDevice, true, "D3D10CreateDeviceAndSwapChain1");
    }
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        DX10Hook_RegisterSwapChainIdentity(*ppSwapChain, true, "D3D10CreateDeviceAndSwapChain1");
        InstallVTableHooks(NULL, NULL, *ppSwapChain);
    }
    return hr;
}

static HRESULT WINAPI DetourD3D10CreateDevice(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
                                              UINT Flags, UINT SDKVersion, ID3D10Device** ppDevice) {
    const HRESULT hr = oD3D10CreateDevice(pAdapter, DriverType, Software, Flags, SDKVersion, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice)
        DX10Hook_RegisterDeviceIdentity(*ppDevice, false, "D3D10CreateDevice");
    return hr;
}

static HRESULT WINAPI DetourD3D10CreateDevice1(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
                                               UINT Flags, D3D10_FEATURE_LEVEL1 HardwareLevel, UINT SDKVersion,
                                               ID3D10Device1** ppDevice) {
    const HRESULT hr = oD3D10CreateDevice1(pAdapter, DriverType, Software, Flags, HardwareLevel, SDKVersion, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice)
        DX10Hook_RegisterDeviceIdentity(*ppDevice, true, "D3D10CreateDevice1");
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice,
                                                       DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) {
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
        if (pDesc) {
            EarlyLog(
                "DX11: CreateSwapChain called. Width=%u Height=%u Windowed=%d "
                "BufferCount=%u SwapEffect=%d",
                pDesc->BufferDesc.Width, pDesc->BufferDesc.Height, pDesc->Windowed, pDesc->BufferCount,
                pDesc->SwapEffect);
        }
    }

    DXGI_SWAP_CHAIN_DESC modifiedDesc = {};
    DXGI_SWAP_CHAIN_DESC* pDescToUse = pDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        ApplyDX11BackbufferCountOverride(modifiedDesc, "CreateSwapChain");
        pDescToUse = &modifiedDesc;
    }

    HRESULT hr = oCreateSwapChain(pFactory, DeWrap(pDevice), pDescToUse, ppSwapChain);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        DXGI_SWAP_CHAIN_DESC actualDesc = {};
        if (SUCCEEDED((*ppSwapChain)->GetDesc(&actualDesc))) {
            const GraphicsConfig& gfx = GetActiveGraphicsConfig();
            if (HasBackbufferCountOverride(gfx.backbufferCount)) {
                HookLogImportant(
                    "DX11: CreateSwapChain created sc=%p actualBufferCount=%u requested=%d "
                    "size=%ux%u swapEffect=%d",
                    *ppSwapChain, actualDesc.BufferCount, gfx.backbufferCount, actualDesc.BufferDesc.Width,
                    actualDesc.BufferDesc.Height, actualDesc.SwapEffect);
            }
        }
        // FSR4/FG swapchain recreation detection (shared with
        // CreateSwapChainForHwnd)
        bool isGameSizedSwapchain =
            pDescToUse && pDescToUse->BufferDesc.Width >= 1920 && pDescToUse->BufferDesc.Height >= 1080;
        if (isGameSizedSwapchain) {
            if (g_FirstGameSwapchainCreated) {
                // Recreation - likely FG taking over
                HookLog(
                    "DX11: CreateSwapChain: Game-sized swapchain recreated - "
                    "invalidating DX12 overlay");
                DX12_SignalFSR4SwapchainRecreated();
            } else {
                g_FirstGameSwapchainCreated = true;
                HookLog("DX11: CreateSwapChain: First game-sized swapchain created (%ux%u)",
                        pDescToUse->BufferDesc.Width, pDescToUse->BufferDesc.Height);
            }
        }

        // First try D3D12 - DX12 games create swapchains via DXGI too
        ID3D12CommandQueue* pD3D12Queue = nullptr;
        ID3D12Device* pD3D12Device = nullptr;

        // Check for CommandQueue (standard DX12)
        if (pDevice && SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&pD3D12Queue))) {
            HookLog("DX11: CreateSwapChain - Detected DX12 CommandQueue.");
            DX12_SignalFSR4SwapchainRecreated();
            // Hook the command queue vtable so ExecuteCommandLists is intercepted
            // This is needed to capture the command queue for overlay rendering
            DX12_HookQueueVTable(pD3D12Queue);
            HookLog("DX11: Hooked DX12 CommandQueue vtable from DX11 path");
            pD3D12Queue->Release();
        }
        // Check for D3D12 Device (non-standard but possible, or checking on
        // swapchain itself)
        else if (ppSwapChain && *ppSwapChain &&
                 SUCCEEDED((*ppSwapChain)->GetDevice(__uuidof(ID3D12Device), (void**)&pD3D12Device))) {
            HookLog("DX11: CreateSwapChain - Detected DX12 Device from SwapChain.");
            DX12_SignalFSR4SwapchainRecreated();
            HookLog(
                "DX11: Skipping DX12 hook installation from DX11 path (Device "
                "detection)");
            pD3D12Device->Release();
        } else {
            // DX11 path - original logic
            ID3D11Device* pD3D11Device = nullptr;
            if (SUCCEEDED((*ppSwapChain)->GetDevice(__uuidof(ID3D11Device), (void**)&pD3D11Device))) {
                ID3D11DeviceContext* ctx = nullptr;
                pD3D11Device->GetImmediateContext(&ctx);
                InstallVTableHooks(pD3D11Device, ctx, *ppSwapChain);
                if (ctx)
                    ctx->Release();
                pD3D11Device->Release();
            } else {
                // Fallback for D3D10/10.1 or other versions
                // CRITICAL FIX: Ensure we don't hook a DX12 swapchain that missed the
                // checks above
                InstallVTableHooks(NULL, NULL, *ppSwapChain);
            }
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd,
                                                              const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                                              IDXGIOutput* pRestrictToOutput,
                                                              IDXGISwapChain1** ppSwapChain) {
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
        if (pDesc) {
            EarlyLog(
                "DX11: CreateSwapChainForHwnd called. Width=%u Height=%u "
                "BufferCount=%u SwapEffect=%d",
                pDesc->Width, pDesc->Height, pDesc->BufferCount, pDesc->SwapEffect);
        }
    }

    DXGI_SWAP_CHAIN_DESC1 modifiedDesc = {};
    const DXGI_SWAP_CHAIN_DESC1* pDescToUse = pDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        ApplyDX11BackbufferCountOverride(modifiedDesc, "CreateSwapChainForHwnd");
        pDescToUse = &modifiedDesc;
    }

    // CRITICAL FG FIX: Track game-sized swapchain recreation for FG overlay
    // safety We DON'T invalidate BEFORE creation - that can cause DXGI lock
    // issues (E_ACCESSDENIED) Instead, we invalidate AFTER successful recreation
    // to clean up stale overlay resources
    bool isGameSizedSwapchain = pDescToUse && pDescToUse->Width >= 1920 && pDescToUse->Height >= 1080;
    bool wasRecreation = isGameSizedSwapchain && g_FirstGameSwapchainCreated;

    HookLog("DX11: BEFORE oCreateSwapChainForHwnd call");
    HRESULT hr = oCreateSwapChainForHwnd(pFactory, DeWrap(pDevice), hWnd, pDescToUse, pFullscreenDesc,
                                         pRestrictToOutput, ppSwapChain);
    HookLog("DX11: AFTER oCreateSwapChainForHwnd call (hr=0x%08X)", hr);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        DXGI_SWAP_CHAIN_DESC actualDesc = {};
        if (SUCCEEDED((*ppSwapChain)->GetDesc(&actualDesc))) {
            const GraphicsConfig& gfx = GetActiveGraphicsConfig();
            if (HasBackbufferCountOverride(gfx.backbufferCount)) {
                HookLogImportant(
                    "DX11: CreateSwapChainForHwnd created sc=%p actualBufferCount=%u requested=%d "
                    "size=%ux%u swapEffect=%d",
                    *ppSwapChain, actualDesc.BufferCount, gfx.backbufferCount, actualDesc.BufferDesc.Width,
                    actualDesc.BufferDesc.Height, actualDesc.SwapEffect);
            }
        }
        // Post-creation: Signal invalidation ONLY for successful recreation
        if (wasRecreation) {
            HookLog(
                "DX11: CreateSwapChainForHwnd: Game-sized swapchain RECREATED "
                "successfully - invalidating overlay");
            DX12_InvalidateSwapchain();
            DX12_SignalFSR4SwapchainRecreated();
        }
        // Post-creation tracking
        if (isGameSizedSwapchain) {
            if (!g_FirstGameSwapchainCreated) {
                g_FirstGameSwapchainCreated = true;
                HookLog(
                    "DX11: CreateSwapChainForHwnd: First game-sized swapchain "
                    "created (%ux%u)",
                    pDescToUse->Width, pDescToUse->Height);
            }
        }

        // First try D3D12 - DX12 games create swapchains via DXGI too
        ID3D12CommandQueue* pD3D12Queue = nullptr;
        ID3D12Device* pD3D12Device = nullptr;

        // Check for CommandQueue (standard DX12)
        if (pDevice && SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&pD3D12Queue))) {
            HookLog("DX11: CreateSwapChainForHwnd - Detected DX12 CommandQueue.");
            DX12_SignalFSR4SwapchainRecreated();
            // Hook the command queue vtable so ExecuteCommandLists is intercepted
            // This is needed to capture the command queue for overlay rendering
            DX12_HookQueueVTable(pD3D12Queue);
            HookLog("DX11: Hooked DX12 CommandQueue vtable from DX11 path");
            pD3D12Queue->Release();
        }
        // Check for D3D12 Device (non-standard but possible, or checking on
        // swapchain itself)
        else if (ppSwapChain && *ppSwapChain &&
                 SUCCEEDED((*ppSwapChain)->GetDevice(__uuidof(ID3D12Device), (void**)&pD3D12Device))) {
            HookLog(
                "DX11: CreateSwapChainForHwnd - Detected DX12 Device from "
                "SwapChain.");
            DX12_SignalFSR4SwapchainRecreated();
            HookLog(
                "DX11: Skipping DX12 hook installation from DX11 path (Device "
                "detection)");
            pD3D12Device->Release();
        } else {
            // DX11 path - original logic
            ID3D11Device* pD3D11Device = nullptr;
            if (SUCCEEDED((*ppSwapChain)->GetDevice(__uuidof(ID3D11Device), (void**)&pD3D11Device))) {
                ID3D11DeviceContext* ctx = nullptr;
                pD3D11Device->GetImmediateContext(&ctx);
                InstallVTableHooks(pD3D11Device, ctx, *ppSwapChain);
                if (ctx)
                    ctx->Release();
                pD3D11Device->Release();
            } else {
                // Fallback for D3D10/10.1 or other versions
                // CRITICAL FIX: Ensure we don't hook a DX12 swapchain that missed the
                // checks above This prevents infinite ResizeBuffers loops in games like
                // Strange Brigade DX12
                InstallVTableHooks(NULL, NULL, *ppSwapChain);
            }
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourCreateSamplerState(ID3D11Device* pDevice, const D3D11_SAMPLER_DESC* pSamplerDesc,
                                                          ID3D11SamplerState** ppSamplerState);
static HRESULT STDMETHODCALLTYPE DetourCreatePixelShader11(ID3D11Device* device, const void* shaderBytecode,
                                                           SIZE_T bytecodeLength, ID3D11ClassLinkage* classLinkage,
                                                           ID3D11PixelShader** pixelShader);
static HRESULT STDMETHODCALLTYPE DetourCreateDeferredContext11(ID3D11Device* device, UINT contextFlags,
                                                               ID3D11DeviceContext** deferredContext);
static void STDMETHODCALLTYPE DetourPSSetShader11(ID3D11DeviceContext* context, ID3D11PixelShader* pixelShader,
                                                  ID3D11ClassInstance* const* classInstances, UINT numClassInstances);
static void STDMETHODCALLTYPE DetourDrawIndexed11(ID3D11DeviceContext* context, UINT indexCount,
                                                  UINT startIndexLocation, INT baseVertexLocation);
static void STDMETHODCALLTYPE DetourDraw11(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertexLocation);
static void STDMETHODCALLTYPE DetourDrawIndexedInstanced11(ID3D11DeviceContext* context, UINT indexCountPerInstance,
                                                           UINT instanceCount, UINT startIndexLocation,
                                                           INT baseVertexLocation, UINT startInstanceLocation);
static void STDMETHODCALLTYPE DetourDrawInstanced11(ID3D11DeviceContext* context, UINT vertexCountPerInstance,
                                                    UINT instanceCount, UINT startVertexLocation,
                                                    UINT startInstanceLocation);
static void STDMETHODCALLTYPE DetourDrawAuto11(ID3D11DeviceContext* context);
static void STDMETHODCALLTYPE DetourDrawIndexedInstancedIndirect11(ID3D11DeviceContext* context,
                                                                   ID3D11Buffer* bufferForArgs,
                                                                   UINT alignedByteOffsetForArgs);
static void STDMETHODCALLTYPE DetourDrawInstancedIndirect11(ID3D11DeviceContext* context, ID3D11Buffer* bufferForArgs,
                                                            UINT alignedByteOffsetForArgs);
static void STDMETHODCALLTYPE DetourExecuteCommandList11(ID3D11DeviceContext* context, ID3D11CommandList* commandList,
                                                         BOOL restoreContextState);
static void STDMETHODCALLTYPE DetourPSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);
static void STDMETHODCALLTYPE DetourVSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);
static void STDMETHODCALLTYPE DetourGSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);
static void STDMETHODCALLTYPE DetourHSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);
static void STDMETHODCALLTYPE DetourDSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);
static void STDMETHODCALLTYPE DetourCSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews);
static void STDMETHODCALLTYPE DetourPSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);
static void STDMETHODCALLTYPE DetourVSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);
static void STDMETHODCALLTYPE DetourGSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);
static void STDMETHODCALLTYPE DetourHSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);
static void STDMETHODCALLTYPE DetourDSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);
static void STDMETHODCALLTYPE DetourCSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers);
static HRESULT STDMETHODCALLTYPE DetourCreateSamplerState10(ID3D10Device* pDevice,
                                                            const D3D10_SAMPLER_DESC* pSamplerDesc,
                                                            ID3D10SamplerState** ppSamplerState);

template <typename Fn>
static bool EnsureVTableHookSlot11(void** vtable, UINT index, LPVOID detour, Fn& original, const char* name) {
    if (!vtable || !detour) {
        return false;
    }

    void** slot = &vtable[index];
    void* current = *slot;
    if (current == detour) {
        return true;
    }

    void* knownOriginal = original ? reinterpret_cast<void*>(original) : nullptr;
    if (knownOriginal && current != knownOriginal) {
        static std::atomic<int> s_mismatchLogs{0};
        int idx = s_mismatchLogs.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: %s hook skipped on alternate vtable (slot=%u current=%p knownOriginal=%p)", name,
                             index, current, knownOriginal);
        }
        return false;
    }

    LPVOID capturedOriginal = nullptr;
    LPVOID* originalOut = original ? &capturedOriginal : reinterpret_cast<LPVOID*>(&original);
    VTableHook::Status status = VTableHook::Create(slot, detour, originalOut);
    if (status == VTableHook::Success) {
        int idx = g_DiagSamplerRuntimeHookInstalled.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24) {
            HookLog("DX11: %s hook installed%s (slot=%u)", name, knownOriginal ? " on additional vtable" : "", index);
        }
        return true;
    }

    static std::atomic<int> s_failureLogs{0};
    int idx = s_failureLogs.fetch_add(1, std::memory_order_relaxed);
    if (idx < 12) {
        HookLogImportant("DX11: %s hook install failed status=%d slot=%u current=%p", name, status, index, current);
    }
    return false;
}

template <typename Fn>
static bool InstallContextVTableHookSlot11(void** vtable, UINT index, LPVOID detour, Fn& globalOriginal,
                                           Fn D3D11ContextVTableOriginals::* member, const char* name,
                                           const char* source) {
    if (!vtable || !detour) {
        return false;
    }

    void** slot = &vtable[index];
    void* current = *slot;
    if (current == detour) {
        return true;
    }

    LPVOID capturedOriginal = nullptr;
    VTableHook::Status status = VTableHook::Create(slot, detour, &capturedOriginal);
    if (status == VTableHook::Success && *slot == detour && capturedOriginal) {
        void* vtableKey = reinterpret_cast<void*>(vtable);
        bool newVTable = false;
        {
            std::unique_lock<std::shared_mutex> lock(g_D3D11ContextVTableOriginalsMutex);
            auto [it, inserted] = g_D3D11ContextVTableOriginals.try_emplace(vtableKey);
            newVTable = inserted;
            it->second.*member = reinterpret_cast<Fn>(capturedOriginal);
        }
        g_D3D11ContextVTableOriginalsGeneration.fetch_add(1, std::memory_order_acq_rel);

        if (newVTable) {
            void* expected = nullptr;
            if (g_PrimaryD3D11ContextVTable.compare_exchange_strong(expected, vtableKey, std::memory_order_acq_rel)) {
                int idx = g_DiagD3D11ContextVTablesHooked.fetch_add(1, std::memory_order_relaxed);
                if (idx < 12) {
                    HookLogImportant("DX11: Context vtable registered primary vtable=%p via %s (#%d)", vtableKey,
                                     source ? source : "unknown", idx + 1);
                }
            } else if (expected != vtableKey) {
                int idx = g_DiagD3D11ContextVTablesHooked.fetch_add(1, std::memory_order_relaxed);
                if (idx < 12) {
                    HookLogImportant("DX11: Context vtable registered alternate vtable=%p primary=%p via %s (#%d)",
                                     vtableKey, expected, source ? source : "unknown", idx + 1);
                }
            }
        }

        if (!globalOriginal || g_PrimaryD3D11ContextVTable.load(std::memory_order_acquire) == vtableKey) {
            globalOriginal = reinterpret_cast<Fn>(capturedOriginal);
        }

        int idx = g_DiagSamplerRuntimeHookInstalled.fetch_add(1, std::memory_order_relaxed);
        if (idx < 32) {
            HookLog("DX11: %s hook installed on context vtable=%p via %s (slot=%u original=%p)", name,
                    reinterpret_cast<void*>(vtable), source ? source : "unknown", index, capturedOriginal);
        }
        return true;
    }

    int idx = g_DiagD3D11ContextHookSkips.fetch_add(1, std::memory_order_relaxed);
    if (idx < 16) {
        HookLogImportant("DX11: %s context hook skipped status=%d slot=%u source=%s current=%p captured=%p now=%p",
                         name, status, index, source ? source : "unknown", current, capturedOriginal, *slot);
    }
    return false;
}

static void InstallContextVTableHooks11(ID3D11DeviceContext* context, const char* source) {
    if (!context) {
        return;
    }

    void** pContextVTable = *(void***)context;
