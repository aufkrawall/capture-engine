/**
 * Wrapper Hook Entry Points Implementation
 */

#include <atomic>
#include <cstdarg>
#include <cstdio>

// Include Windows header for MinGW compatibility
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Forward declaration from dx12_hook.cpp
extern void EnsureDX12Hook();
#include "../apis/dx12_hook.h"  // Access to g_DX12Hook implementation
#include "../common/hook_common.h"
#include "d3d10_device_wrap.h"
#include "d3d11_device_wrap.h"
#include "d3d9_device_wrap.h"
#include "d3d9_wrap.h"
#include "iat_hook.h"
#include "wrapper_hooks.h"
// MinHook shim removed

// ============================================================================
// Original Function Pointers
// ============================================================================

PFN_CreateDXGIFactory oCreateDXGIFactory = nullptr;
PFN_CreateDXGIFactory1 oCreateDXGIFactory1 = nullptr;
PFN_CreateDXGIFactory2 oCreateDXGIFactory2 = nullptr;
#ifdef ENABLE_D3D12_WRAPPER
PFN_D3D12CreateDevice oD3D12CreateDevice = nullptr;
#endif

// D3D11 function pointers
typedef HRESULT(WINAPI* PFN_D3D11CreateDevice)(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                               UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                               UINT SDKVersion, ID3D11Device** ppDevice,
                                               D3D_FEATURE_LEVEL* pFeatureLevel,
                                               ID3D11DeviceContext** ppImmediateContext);

typedef HRESULT(WINAPI* PFN_D3D11CreateDeviceAndSwapChain)(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType,
                                                           HMODULE Software, UINT Flags,
                                                           const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                                           UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                           IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                                           D3D_FEATURE_LEVEL* pFeatureLevel,
                                                           ID3D11DeviceContext** ppImmediateContext);

// Removed static to allow external access (match wrapper_hooks.h)
PFN_D3D11CreateDevice oD3D11CreateDevice = nullptr;
// PFN_D3D11CreateDeviceAndSwapChain oD3D11CreateDeviceAndSwapChain = nullptr; // Defined in dx11_hook.cpp

// D3D10 function pointers definitions
// Removed static
PFN_D3D10CreateDevice oD3D10CreateDevice = nullptr;
PFN_D3D10CreateDevice1 oD3D10CreateDevice1 = nullptr;
PFN_D3D10CreateDeviceAndSwapChain oD3D10CreateDeviceAndSwapChain = nullptr;

// D3D9 function pointers
typedef IDirect3D9*(WINAPI* PFN_Direct3DCreate9)(UINT SDKVersion);
typedef HRESULT(WINAPI* PFN_Direct3DCreate9Ex)(UINT SDKVersion, IDirect3D9Ex** ppD3D);

PFN_Direct3DCreate9 oDirect3DCreate9 = nullptr;
PFN_Direct3DCreate9Ex oDirect3DCreate9Ex = nullptr;

static bool g_WrappersActive = false;

// API Detection Flags - set when actual device creation is called
// These allow distinguishing between DLL being loaded (e.g. d3d12.dll via D3D11On12)
// vs the app actually using that API.
static std::atomic<bool> g_D3D11Or10DeviceCreated{false};
static std::atomic<bool> g_D3D12DeviceCreated{false};

bool WasD3D11Or10DeviceCreated() { return g_D3D11Or10DeviceCreated.load(std::memory_order_acquire); }

bool WasD3D12DeviceCreated() { return g_D3D12DeviceCreated.load(std::memory_order_acquire); }

void WrapperLog(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Log to EarlyLog (which goes to hook_debug.log)
    EarlyLog("%s", buf);
}

// ============================================================================
// Wrapped DXGI Factory Creation
// ============================================================================

HRESULT WINAPI Wrapped_CreateDXGIFactory(REFIID riid, void** ppFactory)
{
    WrapperLog("Wrapper: CreateDXGIFactory called");

    if (!oCreateDXGIFactory) return E_FAIL;

    // Create real factory
    IDXGIFactory* pRealFactory = nullptr;
    HRESULT hr = oCreateDXGIFactory(IID_PPV_ARGS(&pRealFactory));

    if (SUCCEEDED(hr) && pRealFactory) {
        // Try to promote to Factory2 for wrapping
        IDXGIFactory2* pFactory2 = nullptr;
        if (SUCCEEDED(pRealFactory->QueryInterface(IID_PPV_ARGS(&pFactory2)))) {
            auto* pWrapper = new CWrapDXGIFactory2(pFactory2);
            hr = pWrapper->QueryInterface(riid, ppFactory);
            pWrapper->Release();
            pFactory2->Release();
            pRealFactory->Release();
            WrapperLog("Wrapper: Created wrapped factory (promoted to Factory2)");
        } else {
            // Return real factory if can't wrap (very old systems)
            hr = pRealFactory->QueryInterface(riid, ppFactory);
            pRealFactory->Release();
            WrapperLog("Wrapper: Returning unwrapped factory (no Factory2 support)");
        }
    }

    return hr;
}

HRESULT WINAPI Wrapped_CreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    WrapperLog("Wrapper: CreateDXGIFactory1 called");

    // Ensure DX12 hooks are initialized (fixes race condition)
    // SAFETY: Check for null before calling Init()
    if (g_dx12HookInstance) {
        g_dx12HookInstance->Init();
    } else {
        WrapperLog("Wrapper: WARNING - g_dx12HookInstance is null, creating new instance");
        EnsureDX12Hook();
        if (g_dx12HookInstance) {
            g_dx12HookInstance->Init();
        }
    }

    if (!oCreateDXGIFactory1) return E_FAIL;

    IDXGIFactory1* pRealFactory = nullptr;
    HRESULT hr = oCreateDXGIFactory1(IID_PPV_ARGS(&pRealFactory));

    if (SUCCEEDED(hr) && pRealFactory) {
        IDXGIFactory2* pFactory2 = nullptr;
        if (SUCCEEDED(pRealFactory->QueryInterface(IID_PPV_ARGS(&pFactory2)))) {
            auto* pWrapper = new CWrapDXGIFactory2(pFactory2);
            hr = pWrapper->QueryInterface(riid, ppFactory);
            pWrapper->Release();
            pFactory2->Release();
            pRealFactory->Release();
            WrapperLog("Wrapper: Created wrapped factory1");
        } else {
            WrapperLog("Wrapper: CreateDXGIFactory1 - FAILED to QueryInterface Factory2. Returning real factory.");
            hr = pRealFactory->QueryInterface(riid, ppFactory);
            pRealFactory->Release();
        }
    } else {
        WrapperLog("Wrapper: CreateDXGIFactory1 - oCreateDXGIFactory1 FAILED or returned NULL. hr=0x%08X", hr);
    }

    return hr;
}

HRESULT WINAPI Wrapped_CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory)
{
    WrapperLog("Wrapper: CreateDXGIFactory2 called (flags=0x%X)", Flags);

    if (!oCreateDXGIFactory2) return E_FAIL;

    IDXGIFactory2* pRealFactory = nullptr;
    HRESULT hr = oCreateDXGIFactory2(Flags, IID_PPV_ARGS(&pRealFactory));

    if (SUCCEEDED(hr) && pRealFactory) {
        auto* pWrapper = new CWrapDXGIFactory2(pRealFactory);
        hr = pWrapper->QueryInterface(riid, ppFactory);
        pWrapper->Release();
        pRealFactory->Release();
        WrapperLog("Wrapper: Created wrapped factory2");
    } else {
        WrapperLog("Wrapper: CreateDXGIFactory2 - oCreateDXGIFactory2 FAILED or returned NULL. hr=0x%08X", hr);
    }

    return hr;
}

// ============================================================================
// Wrapped D3D12 Device Creation (uses MSVC-compiled wrapper via C interface)
// ============================================================================

#ifdef ENABLE_D3D12_WRAPPER
// Removed dllexport attribute to match header declaration and avoid warning
HRESULT WINAPI Wrapped_D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid,
                                         void** ppDevice)
{

    WrapperLog("Wrapper: D3D12CreateDevice called (feature level=0x%X)", MinimumFeatureLevel);

    // Mark that D3D12 device creation was actually called
    g_D3D12DeviceCreated.store(true, std::memory_order_release);

    // Ensure DX12 hooks are initialized (fixes race condition)
    // SAFETY: Check for null before calling Init()
    if (g_dx12HookInstance) {
        g_dx12HookInstance->Init();
    } else {
        WrapperLog("Wrapper: WARNING - g_dx12HookInstance is null, creating new instance");
        EnsureDX12Hook();
        if (g_dx12HookInstance) {
            g_dx12HookInstance->Init();
        }
    }

    if (!oD3D12CreateDevice) {
        WrapperLog("Wrapper: FATAL - oD3D12CreateDevice is NULL");
        return E_FAIL;
    }

    WrapperLog("Wrapper: Call oD3D12CreateDevice at %p", oD3D12CreateDevice);

    // Create the real device first
    ID3D12Device* pRealDevice = nullptr;
    HRESULT hr = oD3D12CreateDevice(pAdapter, MinimumFeatureLevel, IID_PPV_ARGS(&pRealDevice));
    WrapperLog("Wrapper: oD3D12CreateDevice returned hr=0x%08X, pRealDevice=%p", hr, pRealDevice);

    if (SUCCEEDED(hr) && pRealDevice) {
        // Use the C interface to wrap it (calls into MSVC-compiled code)
        // CRITICAL: Use explicit LoadLibrary/GetProcAddress instead of delay-load
        // to avoid race conditions during first initialization.
        
        static HMODULE s_hWrappers = nullptr;
        static decltype(&D3D12Wrapper_WrapDevice) s_pWrapDevice = nullptr;
        
        if (!s_hWrappers) {
            s_hWrappers = LoadLibraryA("d3d12_wrappers.dll");
            if (!s_hWrappers) {
                s_hWrappers = LoadLibraryA("d3d12_wrappers_x86.dll");
            }
            
            if (s_hWrappers) {
                WrapperLog("Wrapper: Loaded d3d12_wrappers.dll at %p", s_hWrappers);
                s_pWrapDevice = (decltype(&D3D12Wrapper_WrapDevice))GetProcAddress(s_hWrappers, "D3D12Wrapper_WrapDevice");
                if (!s_pWrapDevice) {
                    // Try stdcall decoration
                    s_pWrapDevice = (decltype(&D3D12Wrapper_WrapDevice))GetProcAddress(s_hWrappers, "_D3D12Wrapper_WrapDevice@4");
                }
                if (s_pWrapDevice) {
                    WrapperLog("Wrapper: Found D3D12Wrapper_WrapDevice at %p", s_pWrapDevice);
                } else {
                    WrapperLog("Wrapper: FATAL - Could not find D3D12Wrapper_WrapDevice export! Err=%d", GetLastError());
                }
            } else {
                WrapperLog("Wrapper: FATAL - Could not load d3d12_wrappers.dll! Err=%d", GetLastError());
            }
        }
        
        if (s_pWrapDevice) {
            ID3D12Device* pWrapped = s_pWrapDevice(pRealDevice);
            pRealDevice->Release();  // Wrapper took ownership

            if (pWrapped) {
                hr = pWrapped->QueryInterface(riid, ppDevice);
                pWrapped->Release();
                WrapperLog("Wrapper: Created wrapped D3D12 device");
            } else {
                WrapperLog("Wrapper: D3D12Wrapper_WrapDevice returned nullptr");
                hr = E_FAIL;
            }
        } else {
            // Fallback: return the real device without wrapping (better than crashing)
            WrapperLog("Wrapper: WARNING - Returning unwrapped device due to wrapper load failure");
            hr = pRealDevice->QueryInterface(riid, ppDevice);
            pRealDevice->Release();
        }
    }

    return hr;
}
#endif

// ============================================================================
// Wrapped D3D11 Device Creation
// ============================================================================

HRESULT WINAPI Wrapped_D3D11CreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                         UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                         UINT SDKVersion, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel,
                                         ID3D11DeviceContext** ppImmediateContext)
{

    WrapperLog("Wrapper: D3D11CreateDevice called");

    // Mark that D3D11 device creation was actually called
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D11CreateDevice) return E_FAIL;

    ID3D11Device* pRealDevice = nullptr;
    ID3D11DeviceContext* pRealContext = nullptr;
    HRESULT hr = oD3D11CreateDevice(DeWrap(pAdapter), DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                                    SDKVersion, ppDevice ? &pRealDevice : nullptr, pFeatureLevel,
                                    ppImmediateContext ? &pRealContext : nullptr);

    if (SUCCEEDED(hr) && pRealDevice && ppDevice) {
        auto* pWrapper = new CWrapD3D11Device(pRealDevice);
        *ppDevice = pWrapper;
        pRealDevice->Release();
        WrapperLog("Wrapper: Created wrapped D3D11 device");
    }

    if (ppImmediateContext) {
        *ppImmediateContext = pRealContext;
        // TODO: Could wrap context here too
    }

    return hr;
}

HRESULT WINAPI Wrapped_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType,
                                                     HMODULE Software, UINT Flags,
                                                     const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                                     UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                     IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                                     D3D_FEATURE_LEVEL* pFeatureLevel,
                                                     ID3D11DeviceContext** ppImmediateContext)
{

    WrapperLog("Wrapper: D3D11CreateDeviceAndSwapChain called");

    // Mark that D3D11 device creation was actually called
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D11CreateDeviceAndSwapChain) return E_FAIL;

    ID3D11Device* pRealDevice = nullptr;
    IDXGISwapChain* pRealSwapChain = nullptr;
    ID3D11DeviceContext* pRealContext = nullptr;

    HRESULT hr = oD3D11CreateDeviceAndSwapChain(
        DeWrap(pAdapter), DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc,
        ppSwapChain ? &pRealSwapChain : nullptr, ppDevice ? &pRealDevice : nullptr, pFeatureLevel,
        ppImmediateContext ? &pRealContext : nullptr);

    if (SUCCEEDED(hr)) {
        // Wrap the device
        if (pRealDevice && ppDevice) {
            auto* pDevWrapper = new CWrapD3D11Device(pRealDevice);
            *ppDevice = pDevWrapper;
            pRealDevice->Release();
            WrapperLog("Wrapper: Created wrapped D3D11 device");
        }

        // Wrap the swapchain
        if (pRealSwapChain && ppSwapChain) {
            auto* pSwapWrapper = new CWrapDXGISwapChain(pRealSwapChain, pRealDevice);
            *ppSwapChain = pSwapWrapper;
            pRealSwapChain->Release();
            WrapperLog("Wrapper: Created wrapped swapchain");
        }

        if (ppImmediateContext) {
            *ppImmediateContext = pRealContext;
        }
    }

    return hr;
}

// ============================================================================
// Wrapped D3D10 Device Creation
// ============================================================================

HRESULT WINAPI Wrapped_D3D10CreateDevice(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
                                         UINT Flags, UINT SDKVersion, ID3D10Device** ppDevice)
{

    WrapperLog("Wrapper: D3D10CreateDevice called");

    // Mark that D3D10 device creation was actually called
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D10CreateDevice) return E_FAIL;

    ID3D10Device* pRealDevice = nullptr;
    HRESULT hr = oD3D10CreateDevice(DeWrap(pAdapter), DriverType, Software, Flags, SDKVersion, &pRealDevice);

    if (SUCCEEDED(hr) && pRealDevice && ppDevice) {
        auto* pWrapper = new CWrapD3D10Device(pRealDevice);
        *ppDevice = pWrapper;
        pRealDevice->Release();
        WrapperLog("Wrapper: Created wrapped D3D10 device");
    }

    return hr;
}

HRESULT WINAPI Wrapped_D3D10CreateDevice1(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
                                          UINT Flags, D3D10_FEATURE_LEVEL1 HardwareLevel, UINT SDKVersion,
                                          ID3D10Device1** ppDevice)
{

    WrapperLog("Wrapper: D3D10CreateDevice1 called");

    // Mark that D3D10 device creation was actually called
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D10CreateDevice1) return E_FAIL;

    ID3D10Device1* pRealDevice = nullptr;
    HRESULT hr =
        oD3D10CreateDevice1(DeWrap(pAdapter), DriverType, Software, Flags, HardwareLevel, SDKVersion, &pRealDevice);

    if (SUCCEEDED(hr) && pRealDevice && ppDevice) {
        // Cast to base and wrap
        auto* pWrapper = new CWrapD3D10Device(pRealDevice);
        *ppDevice = static_cast<ID3D10Device1*>(pWrapper);
        pRealDevice->Release();
        WrapperLog("Wrapper: Created wrapped D3D10Device1");
    }

    return hr;
}

HRESULT WINAPI Wrapped_D3D10CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType,
                                                     HMODULE Software, UINT Flags, UINT SDKVersion,
                                                     DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain,
                                                     ID3D10Device** ppDevice)
{

    WrapperLog("Wrapper: D3D10CreateDeviceAndSwapChain called");

    // Mark that D3D10 device creation was actually called
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D10CreateDeviceAndSwapChain) return E_FAIL;

    ID3D10Device* pRealDevice = nullptr;
    IDXGISwapChain* pRealSwapChain = nullptr;

    HRESULT hr =
        oD3D10CreateDeviceAndSwapChain(DeWrap(pAdapter), DriverType, Software, Flags, SDKVersion, pSwapChainDesc,
                                       ppSwapChain ? &pRealSwapChain : nullptr, ppDevice ? &pRealDevice : nullptr);

    if (SUCCEEDED(hr)) {
        if (pRealDevice && ppDevice) {
            auto* pDevWrapper = new CWrapD3D10Device(pRealDevice);
            *ppDevice = pDevWrapper;
            pRealDevice->Release();
            WrapperLog("Wrapper: Created wrapped D3D10 device");
        }

        if (pRealSwapChain && ppSwapChain) {
            auto* pSwapWrapper = new CWrapDXGISwapChain(pRealSwapChain, nullptr);
            *ppSwapChain = pSwapWrapper;
            pRealSwapChain->Release();
            WrapperLog("Wrapper: Created wrapped swapchain");
        }
    }

    return hr;
}

// ============================================================================
// Wrapped D3D9 Direct3DCreate9
// ============================================================================

IDirect3D9* WINAPI Wrapped_Direct3DCreate9(UINT SDKVersion)
{
    WrapperLog("Wrapper: Direct3DCreate9 called (version %u)", SDKVersion);

    if (!oDirect3DCreate9) return nullptr;

    IDirect3D9* pReal = oDirect3DCreate9(SDKVersion);

    if (pReal) {
        CWrapDirect3D9* pWrapper = new CWrapDirect3D9(pReal, false);
        pReal->Release();
        WrapperLog("Wrapper: Created wrapped IDirect3D9");
        return pWrapper;
    }

    return pReal;
}

HRESULT WINAPI Wrapped_Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D)
{
    WrapperLog("Wrapper: Direct3DCreate9Ex called (version %u)", SDKVersion);

    if (!oDirect3DCreate9Ex) return E_FAIL;

    IDirect3D9Ex* pReal = nullptr;
    HRESULT hr = oDirect3DCreate9Ex(SDKVersion, &pReal);

    if (SUCCEEDED(hr) && pReal) {
        CWrapDirect3D9* pWrapper = new CWrapDirect3D9(pReal, true);
        pReal->Release();
        *ppD3D = static_cast<IDirect3D9Ex*>(pWrapper);
        WrapperLog("Wrapper: Created wrapped IDirect3D9Ex");
        return S_OK;
    }

    *ppD3D = pReal;
    return hr;
}

// ============================================================================
// Wrapper System Initialization
// ============================================================================

bool InitializeWrapperHooks()
{
    if (g_WrappersActive) return true;

    EarlyLog("Wrapper: Initializing wrapper hooks (IAT mode)...");

    bool success = true;
    EarlyLog("Wrapper: Initializing DXGI hooks...");
    success &= IATHook::InitializeDXGIHooks();
    EarlyLog("Wrapper: Initializing D3D10 hooks...");
    success &= IATHook::InitializeD3D10Hooks();
    EarlyLog("Wrapper: Initializing D3D11 hooks...");
    success &= IATHook::InitializeD3D11Hooks();
    EarlyLog("Wrapper: Initializing D3D12 hooks...");
    success &= IATHook::InitializeD3D12Hooks();
    EarlyLog("Wrapper: Initializing D3D9 hooks...");
    success &= IATHook::InitializeD3D9Hooks();

    g_WrappersActive = true;
    EarlyLog("Wrapper: IAT initialization complete");
    return success;
}

void ShutdownWrapperHooks()
{
    if (!g_WrappersActive) return;

    WrapperLog("Wrapper: Shutting down wrapper hooks...");

    IATHook::ShutdownIATHooks();

    g_WrappersActive = false;
}

bool AreWrappersActive() { return g_WrappersActive; }

// ============================================================================
// Helper Functions
// ============================================================================

IDXGISwapChain* UnwrapSwapchain(IDXGISwapChain* pSwapChain)
{
    if (!pSwapChain) return nullptr;

    CWrapDXGISwapChain* pWrapper = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, (void**)&pWrapper))) {
        IDXGISwapChain* pReal = pWrapper->GetReal();
        pWrapper->Release();
        return pReal;
    }

    return pSwapChain;
}

#ifdef ENABLE_D3D12_WRAPPER
ID3D12Device* UnwrapDevice(ID3D12Device* pDevice)
{
    // Use the C interface to unwrap (calls into MSVC-compiled code)
    return D3D12Wrapper_UnwrapDevice(pDevice);
}
#endif

bool IsSwapchainWrapped(IDXGISwapChain* pSwapChain)
{
    if (!pSwapChain) return false;

    CWrapDXGISwapChain* pWrapper = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, (void**)&pWrapper))) {
        pWrapper->Release();
        return true;
    }

    return false;
}
