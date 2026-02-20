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
#include "../apis/dx12_hook.h" // Access to g_DX12Hook implementation
#include "../common/fg_detection.h"
#include "../common/hook_common.h"
#include "d3d10_device_wrap.h"
#include "d3d11_device_wrap.h"
#include "d3d9_device_wrap.h"
#include "d3d9_wrap.h"
#include "dxgi_factory_wrap.h"
#include "dxgi_swapchain_wrap.h"
#include "iat_hook.h"
#include "wrapper_hooks.h"

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
typedef HRESULT(WINAPI *PFN_D3D11CreateDevice)(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext);

typedef HRESULT(WINAPI *PFN_D3D11CreateDeviceAndSwapChain)(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice,
    D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext);

// Removed static to allow external access (match wrapper_hooks.h)
PFN_D3D11CreateDevice oD3D11CreateDevice = nullptr;
// PFN_D3D11CreateDeviceAndSwapChain oD3D11CreateDeviceAndSwapChain = nullptr;
// // Defined in dx11_hook.cpp

// D3D10 function pointers definitions
// Removed static
PFN_D3D10CreateDevice oD3D10CreateDevice = nullptr;
PFN_D3D10CreateDevice1 oD3D10CreateDevice1 = nullptr;
PFN_D3D10CreateDeviceAndSwapChain oD3D10CreateDeviceAndSwapChain = nullptr;

// D3D9 function pointers
typedef IDirect3D9 *(WINAPI *PFN_Direct3DCreate9)(UINT SDKVersion);
typedef HRESULT(WINAPI *PFN_Direct3DCreate9Ex)(UINT SDKVersion,
                                               IDirect3D9Ex **ppD3D);

PFN_Direct3DCreate9 oDirect3DCreate9 = nullptr;
PFN_Direct3DCreate9Ex oDirect3DCreate9Ex = nullptr;

static bool g_WrappersActive = false;

// API Detection Flags - set when actual device creation is called
// These allow distinguishing between DLL being loaded (e.g. d3d12.dll via
// D3D11On12) vs the app actually using that API.
static std::atomic<bool> g_D3D11Or10DeviceCreated{false};
static std::atomic<bool> g_D3D12DeviceCreated{false};

bool WasD3D11Or10DeviceCreated() {
  return g_D3D11Or10DeviceCreated.load(std::memory_order_acquire);
}

bool WasD3D12DeviceCreated() {
  return g_D3D12DeviceCreated.load(std::memory_order_acquire);
}

void WrapperLog(const char *fmt, ...) {
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

HRESULT WINAPI Wrapped_CreateDXGIFactory(REFIID riid, void **ppFactory) {
  if (!oCreateDXGIFactory)
    return E_FAIL;

  IDXGIFactory *pRealFactory = nullptr;
  HRESULT hr = oCreateDXGIFactory(riid, (void **)&pRealFactory);

  if (SUCCEEDED(hr) && pRealFactory) {
    // Wrap with CWrapDXGIFactory2 (handles all factory versions)
    IDXGIFactory2 *pRealFactory2 = nullptr;
    if (SUCCEEDED(pRealFactory->QueryInterface(IID_PPV_ARGS(&pRealFactory2)))) {
      auto *pWrapper = new CWrapDXGIFactory2(pRealFactory2);
      pRealFactory2->Release();
      pRealFactory->Release();

      // Return the wrapper via QueryInterface to handle different riid
      hr = pWrapper->QueryInterface(riid, ppFactory);
      pWrapper->Release();
      WrapperLog("Wrapper: Created wrapped DXGIFactory");
    } else {
      // Fallback: no IDXGIFactory2 interface (unlikely)
      *ppFactory = pRealFactory;
      WrapperLog("Wrapper: DXGI factory does not support IDXGIFactory2, "
                 "returning unwrapped");
    }
  }
  return hr;
}

HRESULT WINAPI Wrapped_CreateDXGIFactory1(REFIID riid, void **ppFactory) {
  if (!oCreateDXGIFactory1)
    return E_FAIL;

  IDXGIFactory1 *pRealFactory = nullptr;
  HRESULT hr = oCreateDXGIFactory1(riid, (void **)&pRealFactory);

  if (SUCCEEDED(hr) && pRealFactory) {
    // Wrap with CWrapDXGIFactory2
    IDXGIFactory2 *pRealFactory2 = nullptr;
    if (SUCCEEDED(pRealFactory->QueryInterface(IID_PPV_ARGS(&pRealFactory2)))) {
      auto *pWrapper = new CWrapDXGIFactory2(pRealFactory2);
      pRealFactory2->Release();
      pRealFactory->Release();

      hr = pWrapper->QueryInterface(riid, ppFactory);
      pWrapper->Release();
      WrapperLog("Wrapper: Created wrapped DXGIFactory1");
    } else {
      *ppFactory = pRealFactory;
      WrapperLog("Wrapper: DXGI factory does not support IDXGIFactory2, "
                 "returning unwrapped");
    }
  }
  return hr;
}

HRESULT WINAPI Wrapped_CreateDXGIFactory2(UINT Flags, REFIID riid,
                                          void **ppFactory) {
  if (!oCreateDXGIFactory2)
    return E_FAIL;

  IDXGIFactory2 *pRealFactory = nullptr;
  HRESULT hr = oCreateDXGIFactory2(Flags, riid, (void **)&pRealFactory);

  if (SUCCEEDED(hr) && pRealFactory) {
    // Wrap with CWrapDXGIFactory2
    auto *pWrapper = new CWrapDXGIFactory2(pRealFactory);
    pRealFactory->Release();

    hr = pWrapper->QueryInterface(riid, ppFactory);
    pWrapper->Release();
    WrapperLog("Wrapper: Created wrapped DXGIFactory2");
  }
  return hr;
}

// ============================================================================
// Wrapped D3D12 Device Creation (uses MSVC-compiled wrapper via C interface)
// ============================================================================

#ifdef ENABLE_D3D12_WRAPPER
// Removed dllexport attribute to match header declaration and avoid warning
HRESULT WINAPI Wrapped_D3D12CreateDevice(IUnknown *pAdapter,
                                         D3D_FEATURE_LEVEL MinimumFeatureLevel,
                                         REFIID riid, void **ppDevice) {

  WrapperLog(
      "Wrapper: D3D12CreateDevice called (feature level=0x%X, pAdapter=%p)",
      MinimumFeatureLevel, pAdapter);

  // If adapter is provided, try to get its LUID for debugging
  if (pAdapter) {
    IDXGIAdapter *pDXGIAdapter = nullptr;
    if (SUCCEEDED(pAdapter->QueryInterface(IID_PPV_ARGS(&pDXGIAdapter)))) {
      DXGI_ADAPTER_DESC desc;
      if (SUCCEEDED(pDXGIAdapter->GetDesc(&desc))) {
        WrapperLog("Wrapper: D3D12CreateDevice - Adapter LUID: %08X:%08X, "
                   "VRAM: %llu MB",
                   desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart,
                   desc.DedicatedVideoMemory / (1024 * 1024));
      }
      pDXGIAdapter->Release();
    }
  } else {
    WrapperLog("Wrapper: D3D12CreateDevice - pAdapter is NULL (will use "
               "default adapter)");
  }

  // Mark that D3D12 device creation was actually called
  g_D3D12DeviceCreated.store(true, std::memory_order_release);

  // Initialize DX12 hooks (global vtable hooks + swapchain recreation trigger)
  if (g_dx12HookInstance) {
    g_dx12HookInstance->Init();
  } else {
    WrapperLog(
        "Wrapper: WARNING - g_dx12HookInstance is null, creating new instance");
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
  ID3D12Device *pRealDevice = nullptr;
  HRESULT hr = oD3D12CreateDevice(pAdapter, MinimumFeatureLevel,
                                  IID_PPV_ARGS(&pRealDevice));
  WrapperLog("Wrapper: oD3D12CreateDevice returned hr=0x%08X, pRealDevice=%p",
             hr, pRealDevice);

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

HRESULT WINAPI Wrapped_D3D11CreateDevice(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext) {

  WrapperLog("Wrapped_D3D11CreateDevice: CALLED");

  // Mark that D3D11 device creation was actually called
  g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

  if (!oD3D11CreateDevice) {
    WrapperLog(
        "Wrapped_D3D11CreateDevice: ERROR - oD3D11CreateDevice is NULL!");
    return E_FAIL;
  }

  ID3D11Device *pRealDevice = nullptr;
  ID3D11DeviceContext *pRealContext = nullptr;
  HRESULT hr = oD3D11CreateDevice(
      DeWrap(pAdapter), DriverType, Software, Flags, pFeatureLevels,
      FeatureLevels, SDKVersion, ppDevice ? &pRealDevice : nullptr,
      pFeatureLevel, ppImmediateContext ? &pRealContext : nullptr);

  WrapperLog("Wrapped_D3D11CreateDevice: Original returned hr=0x%08X", hr);

  if (SUCCEEDED(hr) && pRealDevice && ppDevice) {
    auto *pWrapper = new CWrapD3D11Device(pRealDevice);
    *ppDevice = pWrapper;
    pRealDevice->Release();
    WrapperLog("Wrapped_D3D11CreateDevice: Created wrapped D3D11 device");
  }

  if (ppImmediateContext) {
    *ppImmediateContext = pRealContext;
    // TODO: Could wrap context here too
  }

  return hr;
}

HRESULT WINAPI Wrapped_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice,
    D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext) {

  WrapperLog("Wrapped_D3D11CreateDeviceAndSwapChain: CALLED");
  WrapperLog("  Adapter=%p, DriverType=%d, Flags=0x%X", pAdapter, DriverType,
             Flags);
  if (pSwapChainDesc) {
    WrapperLog("  SwapChain: %dx%d, BufferCount=%u",
               pSwapChainDesc->BufferDesc.Width,
               pSwapChainDesc->BufferDesc.Height, pSwapChainDesc->BufferCount);
  }

  // Mark that D3D11 device creation was actually called
  g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

  if (!oD3D11CreateDeviceAndSwapChain) {
    WrapperLog("Wrapped_D3D11CreateDeviceAndSwapChain: ERROR - "
               "oD3D11CreateDeviceAndSwapChain is NULL!");
    return E_FAIL;
  }

  ID3D11Device *pRealDevice = nullptr;
  IDXGISwapChain *pRealSwapChain = nullptr;
  ID3D11DeviceContext *pRealContext = nullptr;

  WrapperLog("Wrapped_D3D11CreateDeviceAndSwapChain: Calling original at %p",
             oD3D11CreateDeviceAndSwapChain);

  HRESULT hr = oD3D11CreateDeviceAndSwapChain(
      DeWrap(pAdapter), DriverType, Software, Flags, pFeatureLevels,
      FeatureLevels, SDKVersion, pSwapChainDesc,
      ppSwapChain ? &pRealSwapChain : nullptr,
      ppDevice ? &pRealDevice : nullptr, pFeatureLevel,
      ppImmediateContext ? &pRealContext : nullptr);

  WrapperLog(
      "Wrapped_D3D11CreateDeviceAndSwapChain: Original returned hr=0x%08X", hr);

  if (SUCCEEDED(hr)) {
    // Wrap the device
    if (pRealDevice && ppDevice) {
      auto *pDevWrapper = new CWrapD3D11Device(pRealDevice);
      *ppDevice = pDevWrapper;
      pRealDevice->Release();
      WrapperLog("Wrapped_D3D11CreateDeviceAndSwapChain: Created wrapped D3D11 "
                 "device");
    }

    // Wrap the swapchain
    if (pRealSwapChain && ppSwapChain) {
      // CRITICAL: Check if this swapchain is already wrapped
      // This prevents double-wrapping which causes infinite Present recursion
      void *pExistingWrapper = nullptr;
      if (SUCCEEDED(pRealSwapChain->QueryInterface(IID_CWrapDXGISwapChain,
                                                   &pExistingWrapper))) {
        ((IUnknown *)pExistingWrapper)->Release();
        WrapperLog("Wrapped_D3D11CreateDeviceAndSwapChain: Swapchain already "
                   "wrapped, skipping double-wrap");
        *ppSwapChain = pRealSwapChain;
        pRealSwapChain->Release();
      } else {
        WrapperLog(
            "Wrapped_D3D11CreateDeviceAndSwapChain: Wrapping swapchain %p",
            pRealSwapChain);
        auto *pSwapWrapper =
            new CWrapDXGISwapChain(pRealSwapChain, pRealDevice);
        *ppSwapChain = pSwapWrapper;
        pRealSwapChain->Release();
        WrapperLog("Wrapped_D3D11CreateDeviceAndSwapChain: Swapchain wrapped, "
                   "new ptr=%p",
                   pSwapWrapper);
      }
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

HRESULT WINAPI Wrapped_D3D10CreateDevice(IDXGIAdapter *pAdapter,
                                         D3D10_DRIVER_TYPE DriverType,
                                         HMODULE Software, UINT Flags,
                                         UINT SDKVersion,
                                         ID3D10Device **ppDevice) {

  WrapperLog("Wrapper: D3D10CreateDevice called");

  // Mark that D3D10 device creation was actually called
  g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

  if (!oD3D10CreateDevice)
    return E_FAIL;

  ID3D10Device *pRealDevice = nullptr;
  HRESULT hr = oD3D10CreateDevice(DeWrap(pAdapter), DriverType, Software, Flags,
                                  SDKVersion, &pRealDevice);

  if (SUCCEEDED(hr) && pRealDevice && ppDevice) {
    auto *pWrapper = new CWrapD3D10Device(pRealDevice);
    *ppDevice = pWrapper;
    pRealDevice->Release();
    WrapperLog("Wrapper: Created wrapped D3D10 device");
  }

  return hr;
}

HRESULT WINAPI Wrapped_D3D10CreateDevice1(IDXGIAdapter *pAdapter,
                                          D3D10_DRIVER_TYPE DriverType,
                                          HMODULE Software, UINT Flags,
                                          D3D10_FEATURE_LEVEL1 HardwareLevel,
                                          UINT SDKVersion,
                                          ID3D10Device1 **ppDevice) {

  WrapperLog("Wrapper: D3D10CreateDevice1 called");

  // Mark that D3D10 device creation was actually called
  g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

  if (!oD3D10CreateDevice1)
    return E_FAIL;

  ID3D10Device1 *pRealDevice = nullptr;
  HRESULT hr =
      oD3D10CreateDevice1(DeWrap(pAdapter), DriverType, Software, Flags,
                          HardwareLevel, SDKVersion, &pRealDevice);

  if (SUCCEEDED(hr) && pRealDevice && ppDevice) {
    // Cast to base and wrap
    auto *pWrapper = new CWrapD3D10Device(pRealDevice);
    *ppDevice = static_cast<ID3D10Device1 *>(pWrapper);
    pRealDevice->Release();
    WrapperLog("Wrapper: Created wrapped D3D10Device1");
  }

  return hr;
}

HRESULT WINAPI Wrapped_D3D10CreateDeviceAndSwapChain(
    IDXGIAdapter *pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, UINT SDKVersion, DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    IDXGISwapChain **ppSwapChain, ID3D10Device **ppDevice) {

  WrapperLog("Wrapper: D3D10CreateDeviceAndSwapChain called");

  // Mark that D3D10 device creation was actually called
  g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

  if (!oD3D10CreateDeviceAndSwapChain)
    return E_FAIL;

  ID3D10Device *pRealDevice = nullptr;
  IDXGISwapChain *pRealSwapChain = nullptr;

  HRESULT hr = oD3D10CreateDeviceAndSwapChain(
      DeWrap(pAdapter), DriverType, Software, Flags, SDKVersion, pSwapChainDesc,
      ppSwapChain ? &pRealSwapChain : nullptr,
      ppDevice ? &pRealDevice : nullptr);

  if (SUCCEEDED(hr)) {
    if (pRealDevice && ppDevice) {
      auto *pDevWrapper = new CWrapD3D10Device(pRealDevice);
      *ppDevice = pDevWrapper;
      pRealDevice->Release();
      WrapperLog("Wrapper: Created wrapped D3D10 device");
    }

    // Wrap the swapchain
    if (pRealSwapChain && ppSwapChain) {
      // CRITICAL: Check if this swapchain is already wrapped
      // This prevents double-wrapping which causes infinite Present recursion
      void *pExistingWrapper = nullptr;
      if (SUCCEEDED(pRealSwapChain->QueryInterface(IID_CWrapDXGISwapChain,
                                                   &pExistingWrapper))) {
        ((IUnknown *)pExistingWrapper)->Release();
        WrapperLog("Wrapper: D3D10 swapchain already wrapped, skipping "
                   "double-wrap");
        *ppSwapChain = pRealSwapChain;
        pRealSwapChain->Release();
      } else {
        auto *pSwapWrapper =
            new CWrapDXGISwapChain(pRealSwapChain, pRealDevice);
        *ppSwapChain = pSwapWrapper;
        pRealSwapChain->Release();
        WrapperLog("Wrapper: Created wrapped DXGI swapchain (D3D10)");
      }
    }
  }

  return hr;
}

// ============================================================================
// Wrapped D3D9 Direct3DCreate9
// ============================================================================

IDirect3D9 *WINAPI Wrapped_Direct3DCreate9(UINT SDKVersion) {
  WrapperLog("Wrapper: Direct3DCreate9 called (version %u)", SDKVersion);

  if (!oDirect3DCreate9)
    return nullptr;

  IDirect3D9 *pReal = oDirect3DCreate9(SDKVersion);

  if (pReal) {
    CWrapDirect3D9 *pWrapper = new CWrapDirect3D9(pReal, false);
    pReal->Release();
    WrapperLog("Wrapper: Created wrapped IDirect3D9");
    return pWrapper;
  }

  return pReal;
}

HRESULT WINAPI Wrapped_Direct3DCreate9Ex(UINT SDKVersion,
                                         IDirect3D9Ex **ppD3D) {
  WrapperLog("Wrapper: Direct3DCreate9Ex called (version %u)", SDKVersion);

  if (!oDirect3DCreate9Ex)
    return E_FAIL;

  IDirect3D9Ex *pReal = nullptr;
  HRESULT hr = oDirect3DCreate9Ex(SDKVersion, &pReal);

  if (SUCCEEDED(hr) && pReal) {
    CWrapDirect3D9 *pWrapper = new CWrapDirect3D9(pReal, true);
    pReal->Release();
    *ppD3D = static_cast<IDirect3D9Ex *>(pWrapper);
    WrapperLog("Wrapper: Created wrapped IDirect3D9Ex");
    return S_OK;
  }

  *ppD3D = pReal;
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

bool InitializeWrapperHooks() {
  if (g_WrappersActive)
    return true;

  EarlyLog("Wrapper: Initializing wrapper hooks (IAT mode)...");

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
    if (s_D3D11Initialized)
      anySuccess = true;
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

  // Mark as active if ANY hooks were installed (allows partial initialization)
  // This enables retry for categories whose DLLs weren't loaded yet
  if (anySuccess) {
    g_WrappersActive = true;
  }

  EarlyLog("Wrapper: IAT initialization complete (DXGI=%d, D3D10=%d, D3D11=%d, "
           "D3D12=%d, D3D9=%d)",
           s_DXGIInitialized, s_D3D10Initialized, s_D3D11Initialized,
           s_D3D12Initialized, s_D3D9Initialized);
  return anySuccess;
}

void ShutdownWrapperHooks() {
  if (!g_WrappersActive)
    return;

  WrapperLog("Wrapper: Shutting down wrapper hooks...");

  IATHook::ShutdownIATHooks();

  g_WrappersActive = false;
}

bool AreWrappersActive() { return g_WrappersActive; }

// ============================================================================
// Helper Functions
// ============================================================================

IDXGISwapChain *UnwrapSwapchain(IDXGISwapChain *pSwapChain) {
  if (!pSwapChain)
    return nullptr;

  CWrapDXGISwapChain *pWrapper = nullptr;
  if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain,
                                           (void **)&pWrapper))) {
    IDXGISwapChain *pReal = pWrapper->GetReal();
    pWrapper->Release();
    return pReal;
  }

  return pSwapChain;
}

#ifdef ENABLE_D3D12_WRAPPER
ID3D12Device *UnwrapDevice(ID3D12Device *pDevice) {
  // Use the C interface to unwrap (calls into MSVC-compiled code)
  return D3D12Wrapper_UnwrapDevice(pDevice);
}
#endif

bool IsSwapchainWrapped(IDXGISwapChain *pSwapChain) {
  if (!pSwapChain)
    return false;

  CWrapDXGISwapChain *pWrapper = nullptr;
  if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain,
                                           (void **)&pWrapper))) {
    pWrapper->Release();
    return true;
  }

  return false;
}
