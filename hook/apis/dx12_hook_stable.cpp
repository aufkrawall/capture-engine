/*
 * DX12 Hook - MinHook Edition (Stable)
 * 
 * Strategy: Use MinHook for stable API interception, but delegate
 * all overlay rendering to the existing dx12_hook.cpp infrastructure.
 * 
 * This gives us:
 * 1. Stable MinHook-based hooking (no vtable patching)
 * 2. Proven overlay rendering from existing code
 * 3. "One fits all" - same code path for FG on/off
 */

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>
#include <mutex>

#include "../wrappers/hook_system.h"
#include "../common/hook_common.h"
#include "dx12_hook.h"

// =============================================================================
// External symbols from dx12_hook.cpp
// =============================================================================

// The existing hook implementation's ProcessFrame function
extern void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain);
extern void DX12_OnSwapchainResizeBegin();
extern void DX12_OnSwapchainResizeEnd();
extern void DX12_HookQueueVTable(ID3D12CommandQueue* queue);
extern void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue);

// Original function pointers from dx12_hook.cpp
extern "C" {
    typedef HRESULT(STDMETHODCALLTYPE* CreateSwapChainFn)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    typedef HRESULT(STDMETHODCALLTYPE* CreateSwapChainForHwndFn)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, 
                                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
    typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffersFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    typedef void(STDMETHODCALLTYPE* ExecuteCommandListsFn)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    
    // These are defined in dx12_hook.cpp and filled by its hook installation
    extern CreateSwapChainFn oCreateSwapChain;
    extern CreateSwapChainForHwndFn oCreateSwapChainForHwnd;
    extern PresentFn oPresent;
    extern ResizeBuffersFn oResizeBuffers;
    extern ExecuteCommandListsFn oExecuteCommandLists;
}

namespace dx12_stable {

// =============================================================================
// MinHook State
// =============================================================================

static HookSystem::ScopedInitializer g_mhInit;
static std::atomic<bool> g_initialized{false};
static std::mutex g_initMutex;

// Our own original function pointers (from MinHook)
static HRESULT(WINAPI* s_origD3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**) = nullptr;

// =============================================================================
// Feature Flag
// =============================================================================

bool IsMinHookEnabled() {
    static bool enabled = []() {
        const char* env = getenv("CE_USE_MINHOOK");
        return env && (strcmp(env, "1") == 0 || _stricmp(env, "true") == 0);
    }();
    return enabled;
}

// =============================================================================
// Hook Implementations
// =============================================================================

HRESULT WINAPI Hook_D3D12CreateDevice(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice)
{
    HRESULT hr = s_origD3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
    
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        HookLog("DX12-MinHook: Device created, waiting for swapchain...");
        
        // The existing dx12_hook.cpp will handle swapchain creation
        // We just need to make sure it gets the command queue
    }
    
    return hr;
}

// =============================================================================
// Factory VTable Hooks (Installed once at runtime)
// =============================================================================

static decltype(&CreateDXGIFactory1) s_origCreateDXGIFactory1 = nullptr;

HRESULT WINAPI Hook_CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    HRESULT hr = s_origCreateDXGIFactory1(riid, ppFactory);
    
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        HookLog("DX12-MinHook: Factory created, installing vtable hooks");
        
        // Get the factory vtable and hook CreateSwapChain methods
        void** vtable = *(void***)*ppFactory;
        
        // Hook CreateSwapChain (vtable[10]) if not already hooked
        if (!oCreateSwapChain) {
            if (HookSystem::CreateCOMHook(&vtable[10], 
                reinterpret_cast<void*>(oCreateSwapChain),
                reinterpret_cast<void**>(&oCreateSwapChain))) {
                HookLog("DX12-MinHook: CreateSwapChain hooked");
            }
        }
        
        // Hook CreateSwapChainForHwnd (vtable[15]) if not already hooked  
        if (!oCreateSwapChainForHwnd) {
            IDXGIFactory2* factory2 = nullptr;
            if (SUCCEEDED(static_cast<IUnknown*>(*ppFactory)->QueryInterface(IID_PPV_ARGS(&factory2)))) {
                void** vtable2 = *(void***)factory2;
                if (HookSystem::CreateCOMHook(&vtable2[15],
                    reinterpret_cast<void*>(oCreateSwapChainForHwnd),
                    reinterpret_cast<void**>(&oCreateSwapChainForHwnd))) {
                    HookLog("DX12-MinHook: CreateSwapChainForHwnd hooked");
                }
                factory2->Release();
            }
        }
    }
    
    return hr;
}

// =============================================================================
// Initialization
// =============================================================================

bool InstallD3D12CreateDeviceHook() {
    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (!hD3D12) {
        // Try to load it
        hD3D12 = LoadLibraryA("d3d12.dll");
        if (!hD3D12) {
            HookLog("DX12-MinHook: d3d12.dll not available");
            return false;
        }
    }
    
    void* target = reinterpret_cast<void*>(GetProcAddress(hD3D12, "D3D12CreateDevice"));
    if (!target) {
        HookLog("DX12-MinHook: D3D12CreateDevice not found");
        return false;
    }
    
    if (!HookSystem::CreateFunctionHook(target, (void*)Hook_D3D12CreateDevice, 
                                        (void**)&s_origD3D12CreateDevice)) {
        HookLog("DX12-MinHook: Failed to hook D3D12CreateDevice");
        return false;
    }
    
    HookLog("DX12-MinHook: D3D12CreateDevice hooked");
    return true;
}

bool InstallFactoryHook() {
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (!hDXGI) {
        hDXGI = LoadLibraryA("dxgi.dll");
        if (!hDXGI) {
            HookLog("DX12-MinHook: dxgi.dll not available");
            return false;
        }
    }
    
    void* target = reinterpret_cast<void*>(GetProcAddress(hDXGI, "CreateDXGIFactory1"));
    if (!target) {
        HookLog("DX12-MinHook: CreateDXGIFactory1 not found");
        return false;
    }
    
    if (!HookSystem::CreateFunctionHook(target, (void*)Hook_CreateDXGIFactory1,
                                        (void**)&s_origCreateDXGIFactory1)) {
        HookLog("DX12-MinHook: Failed to hook CreateDXGIFactory1");
        return false;
    }
    
    HookLog("DX12-MinHook: CreateDXGIFactory1 hooked");
    return true;
}

bool Initialize() {
    std::lock_guard<std::mutex> lock(g_initMutex);
    
    if (g_initialized.load()) {
        return true;
    }
    
    if (!g_mhInit.IsInitialized()) {
        HookLog("DX12-MinHook: MinHook initialization failed");
        return false;
    }
    
    HookLog("DX12-MinHook: Initializing hooks...");
    
    // Install hooks
    bool success = true;
    
    // Hook D3D12CreateDevice to detect when D3D12 is used
    if (!InstallD3D12CreateDeviceHook()) {
        success = false;
    }
    
    // Hook CreateDXGIFactory1 to intercept swapchain creation
    if (!InstallFactoryHook()) {
        success = false;
    }
    
    if (success) {
        g_initialized.store(true);
        HookLog("DX12-MinHook: Initialization complete");
    }
    
    return success;
}

} // namespace dx12_stable

// =============================================================================
// C Exports
// =============================================================================

extern "C" {

bool DX12Stable_IsEnabled() {
    return dx12_stable::IsMinHookEnabled();
}

bool DX12Stable_Initialize() {
    if (!dx12_stable::IsMinHookEnabled()) {
        return false;
    }
    return dx12_stable::Initialize();
}

} // extern "C"
