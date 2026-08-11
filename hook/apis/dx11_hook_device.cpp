#include "dx11_hook_internal.h"

// Use typedef from dx11_hook.h
// typedef HRESULT(WINAPI *D3D11CreateDeviceAndSwapChain_t)(...);
// Global original function pointer - set by IAT patching
PFN_D3D11CreateDeviceAndSwapChain oD3D11CreateDeviceAndSwapChain = NULL;

static PFN_D3D10CreateDeviceAndSwapChain oD3D10CreateDeviceAndSwapChain = NULL;

static PFN_D3D10CreateDeviceAndSwapChain1 oD3D10CreateDeviceAndSwapChain1 = NULL;

static PFN_D3D10CreateDevice oD3D10CreateDevice = NULL;

static PFN_D3D10CreateDevice1 oD3D10CreateDevice1 = NULL;

static PFN_CreateDXGIFactory oCreateDXGIFactory = NULL;

static PFN_CreateDXGIFactory1 oCreateDXGIFactory1 = NULL;

static PFN_CreateDXGIFactory2 oCreateDXGIFactory2 = NULL;

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

void DX11Hook_OnSwapChainCreated(IDXGISwapChain* pSwapChain) {
    if (HookIsShuttingDown() || !pSwapChain)
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
    if (HookIsShuttingDown())
        return;
    HookLog("DX11: InstallDeviceAndContextHooks device=%p context=%p swapChain=%p", pDevice, pContext, pSwapChain);
    InstallVTableHooks(pDevice, pContext, pSwapChain);
}

static HRESULT WINAPI DetourD3D10CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType,
                                                          HMODULE Software, UINT Flags, UINT SDKVersion,
                                                          DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                          IDXGISwapChain** ppSwapChain, ID3D10Device** ppDevice) {
    if (HookIsShuttingDown()) {
        return oD3D10CreateDeviceAndSwapChain
                   ? oD3D10CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, SDKVersion,
                                                    pSwapChainDesc, ppSwapChain, ppDevice)
                   : E_FAIL;
    }
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
    if (HookIsShuttingDown()) {
        return oD3D10CreateDeviceAndSwapChain1
                   ? oD3D10CreateDeviceAndSwapChain1(pAdapter, DriverType, Software, Flags, HardwareLevel, SDKVersion,
                                                     pSwapChainDesc, ppSwapChain, ppDevice)
                   : E_FAIL;
    }
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
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ppDevice && *ppDevice)
        DX10Hook_RegisterDeviceIdentity(*ppDevice, false, "D3D10CreateDevice");
    return hr;
}

static HRESULT WINAPI DetourD3D10CreateDevice1(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
                                               UINT Flags, D3D10_FEATURE_LEVEL1 HardwareLevel, UINT SDKVersion,
                                               ID3D10Device1** ppDevice) {
    const HRESULT hr = oD3D10CreateDevice1(pAdapter, DriverType, Software, Flags, HardwareLevel, SDKVersion, ppDevice);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ppDevice && *ppDevice)
        DX10Hook_RegisterDeviceIdentity(*ppDevice, true, "D3D10CreateDevice1");
    return hr;
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
        if (dx11_hook_g_mainRenderTargetView) {
            dx11_hook_g_mainRenderTargetView->Release();
            dx11_hook_g_mainRenderTargetView = nullptr;
        }
        if (dx11_hook_g_mainRenderTargetView10) {
            dx11_hook_g_mainRenderTargetView10->Release();
            dx11_hook_g_mainRenderTargetView10 = nullptr;
        }
    } else {
        dx11_hook_g_mainRenderTargetView = nullptr;
        dx11_hook_g_mainRenderTargetView10 = nullptr;
    }

    // CRITICAL: Only call Shutdown() when doing normal cleanup (resize, etc.)
    // During app shutdown (releaseDeviceContext=false), skip Shutdown() - the
    // OverlayAdapter destructor will handle cleanup by leaking memory since
    // we've set skipDeviceRelease=true.
    if (releaseDeviceContext && g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    if (releaseDeviceContext) {
        if (dx11_hook_g_pd3dDeviceContext) {
            dx11_hook_g_pd3dDeviceContext->Release();
            dx11_hook_g_pd3dDeviceContext = nullptr;
        }
        if (dx11_hook_g_pd3dDevice) {
            dx11_hook_g_pd3dDevice->Release();
            dx11_hook_g_pd3dDevice = nullptr;
        }
        if (dx11_hook_g_D3D11IdentityDevice) {
            dx11_hook_g_D3D11IdentityDevice->Release();
            dx11_hook_g_D3D11IdentityDevice = nullptr;
        }
        dx11_hook_g_D3D11IdentitySwapChain = nullptr;
    } else {
        dx11_hook_g_pd3dDeviceContext = nullptr;
        dx11_hook_g_pd3dDevice = nullptr;
        dx11_hook_g_D3D11IdentityDevice = nullptr;
        dx11_hook_g_D3D11IdentitySwapChain = nullptr;
    }
}

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
