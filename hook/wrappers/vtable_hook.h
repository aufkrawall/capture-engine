/**
 * VTable Patching Utility
 * 
 * Direct vtable patching to replace MinHook for COM interface hooks.
 * More lightweight than function detouring, no trampoline needed.
 */

#pragma once

#include <windows.h>
#include <mutex>
#include <vector>

namespace VTableHook {

/**
 * Patch a single vtable entry
 * 
 * @param vtable      - Pointer to the vtable (from COM object)
 * @param index       - Index of the function in the vtable
 * @param hookFunc    - Our replacement function
 * @param outOriginal - Receives pointer to original function
 * @return true on success
 */
inline bool PatchVTable(void** vtable, int index, void* hookFunc, void** outOriginal) {
    if (!vtable || !hookFunc) return false;
    
    // Save original
    if (outOriginal) {
        *outOriginal = vtable[index];
    }
    
    // Unprotect, patch, re-protect
    DWORD oldProtect;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        return false;
    }
    
    vtable[index] = hookFunc;
    
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
    
    return true;
}

/**
 * Get vtable from COM object
 */
inline void** GetVTable(void* pObject) {
    return *reinterpret_cast<void***>(pObject);
}

/**
 * Restore a vtable entry
 */
inline bool RestoreVTable(void** vtable, int index, void* originalFunc) {
    if (!vtable || !originalFunc) return false;
    
    DWORD oldProtect;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        return false;
    }
    
    vtable[index] = originalFunc;
    
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
    
    return true;
}

// ============================================================================
// DXGI Swapchain VTable Indices
// ============================================================================

// IDXGISwapChain vtable indices
constexpr int DXGI_SWAPCHAIN_Present = 8;
constexpr int DXGI_SWAPCHAIN_GetBuffer = 9;
constexpr int DXGI_SWAPCHAIN_SetFullscreenState = 10;
constexpr int DXGI_SWAPCHAIN_GetFullscreenState = 11;
constexpr int DXGI_SWAPCHAIN_GetDesc = 12;
constexpr int DXGI_SWAPCHAIN_ResizeBuffers = 13;
constexpr int DXGI_SWAPCHAIN_ResizeTarget = 14;
constexpr int DXGI_SWAPCHAIN_GetContainingOutput = 15;
constexpr int DXGI_SWAPCHAIN_GetFrameStatistics = 16;
constexpr int DXGI_SWAPCHAIN_GetLastPresentCount = 17;

// IDXGISwapChain1 additions
constexpr int DXGI_SWAPCHAIN1_Present1 = 22;

// IDXGIFactory vtable indices
constexpr int DXGI_FACTORY_CreateSwapChain = 10;

// IDXGIFactory2 additions
constexpr int DXGI_FACTORY2_CreateSwapChainForHwnd = 15;
constexpr int DXGI_FACTORY2_CreateSwapChainForCoreWindow = 16;
constexpr int DXGI_FACTORY2_CreateSwapChainForComposition = 24;

// ============================================================================
// D3D11 VTable Indices
// ============================================================================

// ID3D11Device vtable indices
constexpr int D3D11_DEVICE_CreateSamplerState = 23;

// ============================================================================
// D3D10 VTable Indices
// ============================================================================

// ID3D10Device vtable indices
constexpr int D3D10_DEVICE_CreateSamplerState = 36;

// ============================================================================
// D3D12 VTable Indices
// ============================================================================

// ID3D12CommandQueue vtable indices
constexpr int D3D12_COMMANDQUEUE_ExecuteCommandLists = 10;

// ID3D12Device vtable indices
constexpr int D3D12_DEVICE_CreateSampler = 22;

// ============================================================================
// D3D9 VTable Indices
// ============================================================================

constexpr int D3D9_DEVICE_Present = 17;
constexpr int D3D9_DEVICE_Reset = 16;
constexpr int D3D9_DEVICE_SetSamplerState = 69;
constexpr int D3D9_DEVICE_SetTextureStageState = 67;
constexpr int D3D9EX_DEVICE_ResetEx = 129;
constexpr int D3D9EX_DEVICE_PresentEx = 132;

// ============================================================================
// D3D8 VTable Indices  
// ============================================================================

constexpr int D3D8_DEVICE_Present = 15;
constexpr int D3D8_DEVICE_Reset = 14;
constexpr int D3D8_DEVICE_SetTextureStageState = 61;

// ============================================================================
// DirectDraw VTable Indices
// ============================================================================

constexpr int DDRAW_SURFACE7_Blt = 5;
constexpr int DDRAW_SURFACE7_Flip = 11;

} // namespace VTableHook
