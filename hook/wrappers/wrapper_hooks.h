/**
 * Wrapper Hook Entry Points
 * 
 * Contains the initial hooks that intercept API factory/device creation
 * to inject our COM wrappers. This is the transition layer from MinHook
 * to our full wrapper system.
 */

#pragma once

#include "wrapper_base.h"
#include "dxgi_factory_wrap.h"
#include "dxgi_swapchain_wrap.h"

// D3D12 wrapper is compiled with MSVC due to ABI incompatibility
// with WIDL_EXPLICIT_AGGREGATE_RETURNS in MinGW's d3d12.h.
// We use a C interface to call into the MSVC-compiled code.
#ifdef ENABLE_D3D12_WRAPPER
#include "d3d12_wrapper_interface.h"
#endif


// ============================================================================
// DXGI Factory Hook
// ============================================================================

// Original function pointers (from MinHook or IAT patching)
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory)(REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1)(REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT Flags, REFIID riid, void** ppFactory);

extern PFN_CreateDXGIFactory  oCreateDXGIFactory;
extern PFN_CreateDXGIFactory1 oCreateDXGIFactory1;
extern PFN_CreateDXGIFactory2 oCreateDXGIFactory2;

// Our wrapped factory creation functions
HRESULT WINAPI Wrapped_CreateDXGIFactory(REFIID riid, void** ppFactory);
HRESULT WINAPI Wrapped_CreateDXGIFactory1(REFIID riid, void** ppFactory);
HRESULT WINAPI Wrapped_CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory);

// ============================================================================
// D3D12 Device Hook (disabled in MinGW builds)
// ============================================================================

#ifdef ENABLE_D3D12_WRAPPER
typedef HRESULT(WINAPI* PFN_D3D12CreateDevice)(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice);

extern PFN_D3D12CreateDevice oD3D12CreateDevice;

HRESULT WINAPI Wrapped_D3D12CreateDevice(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice);
#endif

// ============================================================================
// D3D10 Wrapper Hook
// ============================================================================

#include <d3d10_1.h>

// D3D10 function pointers
typedef HRESULT (WINAPI* PFN_D3D10CreateDevice)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT, ID3D10Device**);
typedef HRESULT (WINAPI* PFN_D3D10CreateDevice1)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, D3D10_FEATURE_LEVEL1, UINT, ID3D10Device1**);
typedef HRESULT (WINAPI* PFN_D3D10CreateDeviceAndSwapChain)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D10Device**);

extern PFN_D3D10CreateDevice oD3D10CreateDevice;
extern PFN_D3D10CreateDevice1 oD3D10CreateDevice1;
extern PFN_D3D10CreateDeviceAndSwapChain oD3D10CreateDeviceAndSwapChain;

// Wrapped functions
HRESULT WINAPI Wrapped_D3D10CreateDevice(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, UINT SDKVersion, ID3D10Device** ppDevice);
HRESULT WINAPI Wrapped_D3D10CreateDevice1(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, D3D10_FEATURE_LEVEL1 HardwareLevel, UINT SDKVersion, ID3D10Device1** ppDevice);
HRESULT WINAPI Wrapped_D3D10CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, UINT SDKVersion, DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain, ID3D10Device** ppDevice);

// ============================================================================
// D3D11 Wrapper Hook
// ============================================================================

#include <d3d11.h>

// D3D11 function pointers
typedef HRESULT(WINAPI* PFN_D3D11CreateDevice)(
    IDXGIAdapter* pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext);

typedef HRESULT(WINAPI* PFN_D3D11CreateDeviceAndSwapChain)(
    IDXGIAdapter* pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext);

extern PFN_D3D11CreateDevice oD3D11CreateDevice;
extern PFN_D3D11CreateDeviceAndSwapChain oD3D11CreateDeviceAndSwapChain;

// Wrapped functions
HRESULT WINAPI Wrapped_D3D11CreateDevice(
    IDXGIAdapter* pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext);

HRESULT WINAPI Wrapped_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext);


#include <d3d9.h>

// D3D9 function pointers
typedef IDirect3D9* (WINAPI* PFN_Direct3DCreate9)(UINT SDKVersion);
typedef HRESULT (WINAPI* PFN_Direct3DCreate9Ex)(UINT SDKVersion, IDirect3D9Ex** ppD3D);

extern PFN_Direct3DCreate9 oDirect3DCreate9;
extern PFN_Direct3DCreate9Ex oDirect3DCreate9Ex;

// Wrapped functions
IDirect3D9* WINAPI Wrapped_Direct3DCreate9(UINT SDKVersion);
HRESULT WINAPI Wrapped_Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D);

// ============================================================================
// Wrapper System Initialization
// ============================================================================

// Initialize wrapper hooks (replaces parts of current MinHook setup)
bool InitializeWrapperHooks();

// Shutdown wrapper hooks
void ShutdownWrapperHooks();

// Check if wrappers are active
bool AreWrappersActive();

// ============================================================================
// API Detection Flags
// ============================================================================
// These flags are set when the app ACTUALLY calls device creation APIs,
// not just when the DLLs are loaded (d3d12.dll can be loaded by D3D11 runtime).
// This allows proper API detection even when d3d12.dll is present in DX11 apps.

// Returns true if a D3D11 or D3D10 device creation function was called
bool WasD3D11Or10DeviceCreated();

// Returns true if D3D12CreateDevice was actually called
bool WasD3D12DeviceCreated();

// ============================================================================
// Helper Functions
// ============================================================================

// Get the real swapchain from a potentially wrapped one
IDXGISwapChain* UnwrapSwapchain(IDXGISwapChain* pSwapChain);

#ifdef ENABLE_D3D12_WRAPPER
// Get the real device from a potentially wrapped one
ID3D12Device* UnwrapDevice(ID3D12Device* pDevice);
#endif

// Check if a swapchain is wrapped
bool IsSwapchainWrapped(IDXGISwapChain* pSwapChain);

