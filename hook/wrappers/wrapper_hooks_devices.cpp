/**
 * Wrapper Hook Entry Points — device creation and installation
 *
 * The wrapped D3D12/D3D11/D3D10/D3D9/DirectDraw creation exports, plus the
 * per-API wrapper installation and teardown.
 *
 * Split out of wrapper_hooks.cpp.
 */

#include <atomic>
#include <cstdint>
#include <cstdarg>
#include <cstdio>

#include "../../common/raii_helpers.h"

// Include Windows header for MinGW compatibility
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef DIRECTDRAW_VERSION
#define DIRECTDRAW_VERSION 0x0700
#endif
#include <ddraw.h>
#define CE_WRAPPER_RETURN_ADDRESS() __builtin_return_address(0)

// Forward declaration from dx12_hook.cpp
extern void EnsureDX12Hook();
struct IDXGISwapChain;
// Forward declaration from dx11_hook.cpp
extern void DX11Hook_OnSwapChainCreated(IDXGISwapChain* pSwapChain);
#include "../apis/ddraw_hook.h"
#include "../apis/dx11_hook.h"
#include "../apis/dx12_hook.h"  // Access to g_DX12Hook implementation
#include "../common/dx12_dred.h"
#include "../common/dx12_overlay_policy.h"
#include "../common/dx12_process_frame_diagnostics.h"
#include "../common/hook_common.h"
#include "../common/overlay_compat.h"
#include "../common/streamline_runtime_policy.h"
#include "d3d10_device_wrap.h"

#include "d3d11_device_wrap.h"
#include "d3d11_devicecontext_wrap.h"
#include "d3d9_device_wrap.h"
#include "d3d9_wrap.h"
#include "dxgi_factory_wrap.h"
#include "dxgi_swapchain_wrap.h"
#include "iat_hook.h"
#include "wrapper_hooks.h"
// Forward declaration from dx11_hook.cpp (after D3D11 types are available)
extern void DX11Hook_InstallDeviceAndContextHooks(ID3D11Device* pDevice, ID3D11DeviceContext* pContext,
                                                  IDXGISwapChain* pSwapChain);
#include "wrapper_hooks_internal.h"

// ============================================================================
// Wrapped D3D12 Device Creation (uses MSVC-compiled wrapper via C interface)
// ============================================================================

#ifdef ENABLE_D3D12_WRAPPER
// Removed dllexport attribute to match header declaration and avoid warning
HRESULT WINAPI Wrapped_D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid,
                                         void** ppDevice) {
    WrapperLog("Wrapper: D3D12CreateDevice called (feature level=0x%X, pAdapter=%p)", MinimumFeatureLevel, pAdapter);

    // If adapter is provided, try to get its LUID for debugging
    if (pAdapter) {
        IDXGIAdapter* pDXGIAdapter = nullptr;
        if (SUCCEEDED(pAdapter->QueryInterface(IID_PPV_ARGS(&pDXGIAdapter)))) {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(pDXGIAdapter->GetDesc(&desc))) {
                WrapperLog(
                    "Wrapper: D3D12CreateDevice - Adapter LUID: %08X:%08X, "
                    "VRAM: %llu MB",
                    desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart, desc.DedicatedVideoMemory / (1024 * 1024));
            }
            pDXGIAdapter->Release();
        }
    } else {
        WrapperLog(
            "Wrapper: D3D12CreateDevice - pAdapter is NULL (will use "
            "default adapter)");
    }

    // Mark that D3D12 device creation was actually called
    MarkD3D12DeviceCreated();

    // Initialize DX12 hooks (global vtable hooks + swapchain recreation trigger)
    if (g_dx12HookInstance) {
        g_dx12HookInstance->Init();
        g_dx12HookInstance->EnsurePresentHooks();  // Deferred: only now is D3D12 confirmed
    } else {
        WrapperLog("Wrapper: WARNING - g_dx12HookInstance is null, creating new instance");
        EnsureDX12Hook();
        if (g_dx12HookInstance) {
            g_dx12HookInstance->Init();
            g_dx12HookInstance->EnsurePresentHooks();
        }
    }

    if (!oD3D12CreateDevice) {
        WrapperLog("Wrapper: FATAL - oD3D12CreateDevice is NULL");
        return E_FAIL;
    }

    WrapperLog("Wrapper: Call oD3D12CreateDevice at %p", oD3D12CreateDevice);

    // Arm DRED auto-breadcrumbs + page-fault BEFORE the real device is created so
    // a later DXGI_ERROR_DEVICE_HUNG/REMOVED (e.g. the x86 DX12 focus-loss
    // freeze) yields the exact hung command list and faulting GPU VA instead of a
    // bare HRESULT. Gated by env CE_DX12_DRED (default on).
    ce::dx12_dred::ArmBeforeDeviceCreation();

    // Create the real device first
    ID3D12Device* pRealDevice = nullptr;
    HRESULT hr = oD3D12CreateDevice(pAdapter, MinimumFeatureLevel, IID_PPV_ARGS(&pRealDevice));
    WrapperLog("Wrapper: oD3D12CreateDevice returned hr=0x%08X, pRealDevice=%p", hr, pRealDevice);

    // Hook CreateSampler on the game's actual device
    if (SUCCEEDED(hr) && pRealDevice) {
        DX12_HookDeviceVTable(pRealDevice);
        WrapperLog("Wrapper: Hooked CreateSampler on device %p", pRealDevice);
        hr = pRealDevice->QueryInterface(riid, ppDevice);
        pRealDevice->Release();
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
                                         ID3D11DeviceContext** ppImmediateContext) {
    WrapperLog("Wrapped_D3D11CreateDevice: CALLED");

    // Mark that D3D11 device creation was actually called
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D11CreateDevice) {
        WrapperLog("Wrapped_D3D11CreateDevice: ERROR - oD3D11CreateDevice is NULL!");
        return E_FAIL;
    }

    if (IsInD3D10CreateScope()) {
        static std::atomic<int> s_D3D10BypassLogCount{0};
        if (s_D3D10BypassLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
            WrapperLog(
                "Wrapped_D3D11CreateDevice: D3D10 create path detected, "
                "bypassing D3D11 wrappers");
        }
        return oD3D11CreateDevice(DeWrap(pAdapter), DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                                  SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
    }

    ID3D11Device* pRealDevice = nullptr;
    ID3D11DeviceContext* pRealContext = nullptr;
    HRESULT hr = oD3D11CreateDevice(DeWrap(pAdapter), DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                                    SDKVersion, ppDevice ? &pRealDevice : nullptr, pFeatureLevel,
                                    ppImmediateContext ? &pRealContext : nullptr);

    WrapperLog("Wrapped_D3D11CreateDevice: Original returned hr=0x%08X", hr);

    CWrapD3D11Device* pWrapper = nullptr;
    if (SUCCEEDED(hr) && pRealDevice && ppDevice) {
        DX11Hook_RegisterDeviceIdentity(pRealDevice, "D3D11CreateDevice", true);
        pWrapper = new CWrapD3D11Device(pRealDevice);
        *ppDevice = pWrapper;
        pRealDevice->Release();
        WrapperLog("Wrapped_D3D11CreateDevice: Created wrapped D3D11 device");
    }

    // Install vtable hooks immediately so the game cannot cache un-hooked Draw
    // function pointers from the real context before our detours are active.
    if (SUCCEEDED(hr)) {
        DX11Hook_InstallDeviceAndContextHooks(pRealDevice, pRealContext, NULL);
    }

    if (ppImmediateContext) {
        if (SUCCEEDED(hr) && pRealContext) {
            auto* wrappedContext = new CWrapD3D11DeviceContext(pRealContext, pWrapper);
            *ppImmediateContext = wrappedContext;
            pRealContext->Release();
            WrapperLog("Wrapped_D3D11CreateDevice: Returned wrapped immediate context real=%p wrapper=%p", pRealContext,
                       wrappedContext);
        } else {
            *ppImmediateContext = pRealContext;
        }
    }

    return hr;
}

HRESULT WINAPI Wrapped_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType,
                                                     HMODULE Software, UINT Flags,
                                                     const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                                     UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                     IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                                     D3D_FEATURE_LEVEL* pFeatureLevel,
                                                     ID3D11DeviceContext** ppImmediateContext) {
    WrapperLog("Wrapped_D3D11CreateDeviceAndSwapChain: CALLED");
    WrapperLog("  Adapter=%p, DriverType=%d, Flags=0x%X", pAdapter, DriverType, Flags);
    if (pSwapChainDesc) {
        WrapperLog("  SwapChain: %dx%d, BufferCount=%u", pSwapChainDesc->BufferDesc.Width,
                   pSwapChainDesc->BufferDesc.Height, pSwapChainDesc->BufferCount);
    }

    // Mark that D3D11 device creation was actually called
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D11CreateDeviceAndSwapChain) {
        WrapperLog(
            "Wrapped_D3D11CreateDeviceAndSwapChain: ERROR - "
            "oD3D11CreateDeviceAndSwapChain is NULL!");
        return E_FAIL;
    }

    if (IsInD3D10CreateScope()) {
        static std::atomic<int> s_D3D10BypassLogCount{0};
        if (s_D3D10BypassLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
            WrapperLog(
                "Wrapped_D3D11CreateDeviceAndSwapChain: D3D10 create path "
                "detected, bypassing D3D11 wrappers");
        }
        return oD3D11CreateDeviceAndSwapChain(DeWrap(pAdapter), DriverType, Software, Flags, pFeatureLevels,
                                              FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice,
                                              pFeatureLevel, ppImmediateContext);
    }

    WrapperLog("Wrapped_D3D11CreateDeviceAndSwapChain: Calling original at %p", oD3D11CreateDeviceAndSwapChain);

    DXGI_SWAP_CHAIN_DESC modifiedDesc = {};
    const DXGI_SWAP_CHAIN_DESC* pDescToUse = pSwapChainDesc;
    if (pSwapChainDesc) {
        modifiedDesc = *pSwapChainDesc;
        ApplyD3D11CreateDeviceSwapChainBackbufferOverride(modifiedDesc);
        pDescToUse = &modifiedDesc;
    }

    HRESULT hr = oD3D11CreateDeviceAndSwapChain(DeWrap(pAdapter), DriverType, Software, Flags, pFeatureLevels,
                                                FeatureLevels, SDKVersion, pDescToUse, ppSwapChain, ppDevice,
                                                pFeatureLevel, ppImmediateContext);

    WrapperLog("Wrapped_D3D11CreateDeviceAndSwapChain: Original returned hr=0x%08X", hr);

    if (SUCCEEDED(hr)) {
        if (ppDevice && *ppDevice) {
            DX11Hook_RegisterDeviceIdentity(*ppDevice, "D3D11CreateDeviceAndSwapChain", true);
        }
        const auto& gfx = GetActiveGraphicsConfig();
        if (ppSwapChain && *ppSwapChain && HasBackbufferCountOverride(gfx.backbufferCount)) {
            DXGI_SWAP_CHAIN_DESC actualDesc = {};
            if (SUCCEEDED((*ppSwapChain)->GetDesc(&actualDesc))) {
                WrapperLog(
                    "Wrapped_D3D11CreateDeviceAndSwapChain: Actual BufferCount=%u requested=%d "
                    "size=%ux%u swapEffect=%d",
                    actualDesc.BufferCount, gfx.backbufferCount, actualDesc.BufferDesc.Width,
                    actualDesc.BufferDesc.Height, actualDesc.SwapEffect);
            }
        }

        // Install vtable hooks immediately on device and context, before any
        // swapchain-specific setup, so the game cannot cache un-hooked function
        // pointers.
        DX11Hook_InstallDeviceAndContextHooks(ppDevice ? *ppDevice : NULL,
                                              ppImmediateContext ? *ppImmediateContext : NULL,
                                              ppSwapChain ? *ppSwapChain : NULL);

        // D3D11 runtime compatibility: return raw objects and hook swapchain vtable.
        if (ppSwapChain && *ppSwapChain) {
            DX11Hook_OnSwapChainCreated(*ppSwapChain);
        }
        if (ppImmediateContext && *ppImmediateContext) {
            ID3D11DeviceContext* realContext = *ppImmediateContext;
            auto* wrappedContext = new CWrapD3D11DeviceContext(realContext, nullptr);
            *ppImmediateContext = wrappedContext;
            realContext->Release();
            WrapperLog("Wrapped_D3D11CreateDeviceAndSwapChain: Returned wrapped immediate context real=%p wrapper=%p",
                       realContext, wrappedContext);
        }
        static std::atomic<int> s_D3D11CompatLogCount{0};
        if (s_D3D11CompatLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
            WrapperLog(
                "Wrapped_D3D11CreateDeviceAndSwapChain: compatibility mode - "
                "returning raw device/swapchain with wrapped context");
        }
    }

    return hr;
}

// ============================================================================
// Wrapped D3D10 Device Creation
// ============================================================================

HRESULT WINAPI Wrapped_D3D10CreateDevice(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
                                         UINT Flags, UINT SDKVersion, ID3D10Device** ppDevice) {
    WrapperLog("Wrapper: D3D10CreateDevice called");

    // Mark that D3D10 device creation was actually called
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D10CreateDevice)
        return E_FAIL;

    D3D10CreateScope d3d10CreateScope;

    ID3D10Device* pRealDevice = nullptr;
    HRESULT hr = oD3D10CreateDevice(DeWrap(pAdapter), DriverType, Software, Flags, SDKVersion, &pRealDevice);

    if (SUCCEEDED(hr) && pRealDevice && ppDevice) {
        DX10Hook_RegisterDeviceIdentity(pRealDevice, false, "D3D10CreateDevice");
        auto* pWrapper = new CWrapD3D10Device(pRealDevice);
        *ppDevice = pWrapper;
        pRealDevice->Release();
        WrapperLog("Wrapper: Created wrapped D3D10 device");
    }

    return hr;
}

HRESULT WINAPI Wrapped_D3D10CreateDevice1(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
                                          UINT Flags, D3D10_FEATURE_LEVEL1 HardwareLevel, UINT SDKVersion,
                                          ID3D10Device1** ppDevice) {
    WrapperLog("Wrapper: D3D10CreateDevice1 called");

    // Mark that D3D10 device creation was actually called
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D10CreateDevice1)
        return E_FAIL;

    D3D10CreateScope d3d10CreateScope;

    ID3D10Device1* pRealDevice = nullptr;
    HRESULT hr =
        oD3D10CreateDevice1(DeWrap(pAdapter), DriverType, Software, Flags, HardwareLevel, SDKVersion, &pRealDevice);

    if (SUCCEEDED(hr) && pRealDevice && ppDevice) {
        DX10Hook_RegisterDeviceIdentity(pRealDevice, true, "D3D10CreateDevice1");
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
                                                     ID3D10Device** ppDevice) {
    WrapperLog("Wrapper: D3D10CreateDeviceAndSwapChain called");

    // Mark that D3D10 device creation was actually called
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D10CreateDeviceAndSwapChain)
        return E_FAIL;

    D3D10CreateScope d3d10CreateScope;

    ID3D10Device* pRealDevice = nullptr;
    IDXGISwapChain* pRealSwapChain = nullptr;

    HRESULT hr =
        oD3D10CreateDeviceAndSwapChain(DeWrap(pAdapter), DriverType, Software, Flags, SDKVersion, pSwapChainDesc,
                                       ppSwapChain ? &pRealSwapChain : nullptr, ppDevice ? &pRealDevice : nullptr);

    if (SUCCEEDED(hr)) {
        if (pRealDevice) {
            DX10Hook_RegisterDeviceIdentity(pRealDevice, false, "D3D10CreateDeviceAndSwapChain");
        }
        if (pRealSwapChain) {
            DX10Hook_RegisterSwapChainIdentity(pRealSwapChain, false, "D3D10CreateDeviceAndSwapChain");
        }
        // D3D10 runtime compatibility: return raw objects.
        // Wrapping D3D10 swapchains/devices has caused invalid vtable pointers in
        // both x64 and x86 test coverage.
        if (pRealDevice && ppDevice) {
            *ppDevice = pRealDevice;
            pRealDevice = nullptr;
        }
        if (pRealSwapChain && ppSwapChain) {
            *ppSwapChain = pRealSwapChain;
            pRealSwapChain = nullptr;
        }
        WrapperLog("Wrapper: D3D10 compatibility mode - returning unwrapped objects");
        return hr;
    }

    return hr;
}

HRESULT WINAPI Wrapped_D3D10CreateDeviceAndSwapChain1(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType,
                                                      HMODULE Software, UINT Flags, D3D10_FEATURE_LEVEL1 HardwareLevel,
                                                      UINT SDKVersion, DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                      IDXGISwapChain** ppSwapChain, ID3D10Device1** ppDevice) {
    WrapperLog("Wrapper: D3D10CreateDeviceAndSwapChain1 called");
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D10CreateDeviceAndSwapChain1)
        return E_FAIL;

    D3D10CreateScope d3d10CreateScope;
    ID3D10Device1* pRealDevice = nullptr;
    IDXGISwapChain* pRealSwapChain = nullptr;
    const HRESULT hr = oD3D10CreateDeviceAndSwapChain1(
        DeWrap(pAdapter), DriverType, Software, Flags, HardwareLevel, SDKVersion, pSwapChainDesc,
        ppSwapChain ? &pRealSwapChain : nullptr, ppDevice ? &pRealDevice : nullptr);

    if (SUCCEEDED(hr)) {
        if (pRealDevice) {
            DX10Hook_RegisterDeviceIdentity(pRealDevice, true, "D3D10CreateDeviceAndSwapChain1");
        }
        if (pRealSwapChain) {
            DX10Hook_RegisterSwapChainIdentity(pRealSwapChain, true, "D3D10CreateDeviceAndSwapChain1");
        }
        if (pRealDevice && ppDevice) {
            *ppDevice = pRealDevice;
            pRealDevice = nullptr;
        }
        if (pRealSwapChain && ppSwapChain) {
            *ppSwapChain = pRealSwapChain;
            pRealSwapChain = nullptr;
        }
        WrapperLog("Wrapper: D3D10.1 compatibility mode - returning unwrapped objects");
    }

    return hr;
}

// ============================================================================
// Wrapped D3D9 Direct3DCreate9
// ============================================================================

IDirect3D9* WINAPI Wrapped_Direct3DCreate9(UINT SDKVersion) {
    WrapperLog("Wrapper: Direct3DCreate9 called (version %u)", SDKVersion);

    // Always return the original IDirect3D9 object from Direct3DCreate9.
    //
    // DO NOT return IDirect3D9Ex here even though IDirect3D9Ex is a COM superset.
    // Some applications and injected overlays access non-COM runtime details;
    // IDirect3D9Ex also changes managed-resource and lost-device semantics.
    //
    // DX9Hook's vtable hook on IDirect3D9::CreateDevice (installed by
    // DetourDirect3DCreate9 when this call reaches dx9_hook.cpp) intercepts
    // device creation while preserving the requested classic device type. Shared
    // capture resources are supplied by an internal helper after creation.
    if (!oDirect3DCreate9)
        return nullptr;
    IDirect3D9* pReal = oDirect3DCreate9(SDKVersion);
    WrapperLog("Wrapper: Returning original IDirect3D9 (safe for internal-layout-aware games)");
    return pReal;
}

HRESULT WINAPI Wrapped_Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D) {
    WrapperLog("Wrapper: Direct3DCreate9Ex called (version %u)", SDKVersion);

    if (!oDirect3DCreate9Ex)
        return E_FAIL;

    IDirect3D9Ex* pReal = nullptr;
    HRESULT hr = oDirect3DCreate9Ex(SDKVersion, &pReal);

    if (SUCCEEDED(hr) && pReal) {
        // Return raw IDirect3D9Ex — same rationale as Wrapped_Direct3DCreate9.
        *ppD3D = pReal;
        WrapperLog("Wrapper: Returning raw IDirect3D9Ex from Direct3DCreate9Ex");
        return S_OK;
    }

    *ppD3D = pReal;
    return hr;
}

HRESULT WINAPI Wrapped_DirectDrawCreateEx(GUID* lpGuid, LPVOID* lplpDD, REFIID iid, IUnknown* pUnkOuter) {
    WrapperLog("Wrapper: DirectDrawCreateEx called (out=%p)", lplpDD);

    if (!oDirectDrawCreateEx)
        return E_FAIL;

    HRESULT hr = oDirectDrawCreateEx(lpGuid, lplpDD, iid, pUnkOuter);
    WrapperLog("Wrapper: DirectDrawCreateEx returned hr=0x%08X, object=%p", hr,
               (lplpDD && SUCCEEDED(hr)) ? *lplpDD : nullptr);
    if (SUCCEEDED(hr) && lplpDD && *lplpDD) {
        bool hooked = HookDirectDrawObject(*lplpDD, iid);
        WrapperLog("Wrapper: HookDirectDrawObject returned %d for object=%p", hooked ? 1 : 0, *lplpDD);
    }

    return hr;
}

HRESULT WINAPI Wrapped_DirectDrawCreate(GUID* lpGuid, LPVOID* lplpDD, IUnknown* pUnkOuter) {
    WrapperLog("Wrapper: DirectDrawCreate called (out=%p)", lplpDD);
    if (!oDirectDrawCreate)
        return E_FAIL;

    const HRESULT hr = oDirectDrawCreate(lpGuid, lplpDD, pUnkOuter);
    if (SUCCEEDED(hr) && lplpDD && *lplpDD)
        HookDirectDrawObject(*lplpDD, IID_IDirectDraw);
    return hr;
}

// ============================================================================
// Wrapper System Initialization
// ============================================================================

static bool s_DXGIInitialized = false;
static bool s_D3D10Initialized = false;
static bool s_D3D11Initialized = false;
static bool s_D3D12Initialized = false;
static bool s_D3D9Initialized = false;
static bool s_DDrawInitialized = false;
static std::atomic<uint64_t> s_WrapperHookActivityGeneration{0};
static std::atomic<uint32_t> s_WrapperHookActiveCalls{0};

ce::dx12_process_frame_diagnostics::ConcurrentActivitySnapshot GetWrapperHookActivitySnapshot() {
    ce::dx12_process_frame_diagnostics::ConcurrentActivitySnapshot snapshot;
    snapshot.generation = s_WrapperHookActivityGeneration.load(std::memory_order_relaxed);
    snapshot.activeCalls = s_WrapperHookActiveCalls.load(std::memory_order_relaxed);
    return snapshot;
}

bool InitializeWrapperHooks() {
    s_WrapperHookActivityGeneration.fetch_add(1, std::memory_order_relaxed);
    s_WrapperHookActiveCalls.fetch_add(1, std::memory_order_relaxed);
    auto activityGuard = ce::make_scope_guard([]() {
        s_WrapperHookActiveCalls.fetch_sub(1, std::memory_order_relaxed);
        s_WrapperHookActivityGeneration.fetch_add(1, std::memory_order_relaxed);
    });

    // Do NOT return early when g_WrappersActive is true from a previous
    // partial initialization (e.g. DllMain ran before D3D11.dll was loaded).
    // The per-category !s_*Initialized guards below let us retry categories
    // whose DLLs weren't available on the first call.  g_WrappersActive only
    // prevents double-running the category-independent setup below.

    if (!g_WrappersActive) {
        EarlyLog("Wrapper: Initializing wrapper hooks (IAT mode)...");
    }

    bool anySuccess = false;

    // CRITICAL FIX: Each hook category can be retried independently if the DLL
    // wasn't loaded yet We must NOT set g_WrappersActive = true until ALL
    // categories that will ever load are done

    if (!s_DXGIInitialized) {
        EarlyLog("Wrapper: Initializing DXGI hooks...");
        s_DXGIInitialized = IATHook::InitializeDXGIHooks();
        if (s_DXGIInitialized)
            anySuccess = true;
    }

    if (!s_D3D10Initialized) {
        EarlyLog("Wrapper: Initializing D3D10 hooks...");
        s_D3D10Initialized = IATHook::InitializeD3D10Hooks();
        if (s_D3D10Initialized)
            anySuccess = true;
    }

    if (!s_D3D11Initialized) {
        EarlyLog("Wrapper: Initializing D3D11 hooks...");
        s_D3D11Initialized = IATHook::InitializeD3D11Hooks();
        if (s_D3D11Initialized) {
            anySuccess = true;
            HookLogImportant("Wrapper: D3D11 IAT hooks installed (retry)");
        }
    }

    if (!s_D3D12Initialized) {
        EarlyLog("Wrapper: Initializing D3D12 hooks...");
        s_D3D12Initialized = IATHook::InitializeD3D12Hooks();
        if (s_D3D12Initialized)
            anySuccess = true;
    }

    if (!s_D3D9Initialized) {
        EarlyLog("Wrapper: Initializing D3D9 hooks...");
        s_D3D9Initialized = IATHook::InitializeD3D9Hooks();
        if (s_D3D9Initialized)
            anySuccess = true;
    }

    if (!s_DDrawInitialized) {
        EarlyLog("Wrapper: Initializing DirectDraw hooks...");
        s_DDrawInitialized = IATHook::InitializeDDrawHooks();
        if (s_DDrawInitialized)
            anySuccess = true;
    }

    // Mark as active if ANY hooks were installed (allows partial initialization)
    // This enables retry for categories whose DLLs weren't loaded yet
    if (anySuccess) {
        g_WrappersActive = true;
    }

    EarlyLog(
        "Wrapper: IAT initialization complete (DXGI=%d, D3D10=%d, D3D11=%d, "
        "D3D12=%d, D3D9=%d, DDraw=%d)",
        s_DXGIInitialized, s_D3D10Initialized, s_D3D11Initialized, s_D3D12Initialized, s_D3D9Initialized,
        s_DDrawInitialized);
    return anySuccess;
}

void ShutdownWrapperHooks() {
    if (!g_WrappersActive)
        return;

    WrapperLog("Wrapper: Shutting down wrapper hooks...");

    IATHook::ShutdownIATHooks();

    g_WrappersActive = false;
}

bool AreWrappersActive() {
    return g_WrappersActive;
}

// ============================================================================
// Helper Functions
// ============================================================================

IDXGISwapChain* UnwrapSwapchain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return nullptr;

    CWrapDXGISwapChain* pWrapper = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, (void**)&pWrapper))) {
        IDXGISwapChain* pReal = pWrapper->GetReal();
        pWrapper->Release();
        return pReal;
    }

    return pSwapChain;
}

#ifdef ENABLE_D3D12_WRAPPER
ID3D12Device* UnwrapDevice(ID3D12Device* pDevice) {
    // Use the C interface to unwrap (calls into MSVC-compiled code)
    return D3D12Wrapper_UnwrapDevice(pDevice);
}
#endif

bool IsSwapchainWrapped(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return false;

    CWrapDXGISwapChain* pWrapper = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, (void**)&pWrapper))) {
        pWrapper->Release();
        return true;
    }

    return false;
}
