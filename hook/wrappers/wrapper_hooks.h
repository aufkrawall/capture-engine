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
// Wrapper System Initialization
// ============================================================================

// Initialize wrapper hooks (replaces parts of current MinHook setup)
bool InitializeWrapperHooks();

// Shutdown wrapper hooks
void ShutdownWrapperHooks();

// Check if wrappers are active
bool AreWrappersActive();

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

