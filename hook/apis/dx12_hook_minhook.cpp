/*
 * DX12 Hook - MinHook Edition
 * 
 * This implementation uses MinHook for stable API hooking
 * but leverages the existing overlay rendering infrastructure.
 * 
 * Key difference from dx12_hook.cpp:
 * - Uses MinHook (trampoline-based) instead of VTableHook (direct vtable patching)
 * - Same overlay rendering logic (uses g_State, g_OverlayQueue, etc.)
 * - "One fits all" - no FG-specific code paths
 */

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "../wrappers/hook_system.h"
#include "../common/hook_common.h"
#include "dx12_hook.h"

// Include the existing DX12 hook implementation but override the hook mechanism
// We'll use MinHook for hook installation instead of VTableHook

// External symbols from dx12_hook.cpp
extern ID3D12Device* g_Device;
extern ID3D12CommandQueue* g_CommandQueue;
extern std::recursive_mutex g_CommandQueueMutex;
extern ID3D12CommandQueue* g_OverlayQueue;

// Forward declarations from dx12_hook.cpp
void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue);
void DX12_OnSwapchainResizeBegin();
void DX12_OnSwapchainResizeEnd();
void DX12_InvalidateSwapchain();

// Original function pointers (filled by MinHook)
static PFN_D3D12CreateDevice s_origD3D12CreateDevice = nullptr;
static decltype(&D3D12CreateDevice) s_origCreateSwapChain = nullptr;
static decltype(&D3D12CreateDevice) s_origCreateSwapChainForHwnd = nullptr;
static void* s_origPresent = nullptr;
static void* s_origResizeBuffers = nullptr;
static void* s_origExecuteCommandLists = nullptr;

namespace dx12_minhook {

// Global state
static HookSystem::ScopedInitializer g_mhInit;
static std::atomic<bool> g_hooksInstalled{false};
static std::mutex g_hookMutex;

// Feature flag
bool IsEnabled() {
    static bool enabled = []() {
        const char* env = getenv("CE_USE_MINHOOK");
        return env && (strcmp(env, "1") == 0 || _stricmp(env, "true") == 0);
    }();
    return enabled;
}

// Hook: D3D12CreateDevice
HRESULT WINAPI Hook_D3D12CreateDevice(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice)
{
    HRESULT hr = s_origD3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
    
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        HookLog("MinHook-DX12: D3D12CreateDevice succeeded, device=%p", *ppDevice);
        
        // Install factory hooks on first device creation
        if (!g_hooksInstalled.load()) {
            std::lock_guard<std::mutex> lock(g_hookMutex);
            if (!g_hooksInstalled.load()) {
                // Factory hooks will be installed when CreateSwapChain is called
                g_hooksInstalled.store(true);
            }
        }
    }
    
    return hr;
}

// Hook: CreateSwapChain
HRESULT WINAPI Hook_CreateSwapChain(
    IDXGIFactory* pFactory,
    IUnknown* pDevice,
    DXGI_SWAP_CHAIN_DESC* pDesc,
    IDXGISwapChain** ppSwapChain)
{
    // Get the existing hook's original function
    extern HRESULT(STDMETHODCALLTYPE* oCreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    
    HRESULT hr = oCreateSwapChain(pFactory, pDevice, pDesc, ppSwapChain);
    
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        HookLog("MinHook-DX12: CreateSwapChain succeeded, swapchain=%p", *ppSwapChain);
        
        // Hook the queue if it's a D3D12 device
        if (pDevice) {
            ID3D12CommandQueue* q = nullptr;
            if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&q)))) {
                DX12_SetCommandQueue(q);
                q->Release();
            }
        }
        
        // Install swapchain hooks
        // This will be done by the existing dx12_hook.cpp via DXGIShared::InstallHooks
        // which is called from the original CreateSwapChain detour
    }
    
    return hr;
}

// Hook: Present
HRESULT WINAPI Hook_Present(
    IDXGISwapChain* pSwapChain,
    UINT SyncInterval,
    UINT Flags)
{
    // Get the existing hook's original function
    typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
    extern PresentFn oPresent;
    
    // Call the existing frame processing logic
    // This uses the existing g_State, g_OverlayQueue, etc.
    extern void DX12_ProcessFrameExternal(IDXGISwapChain*);
    DX12_ProcessFrameExternal(pSwapChain);
    
    return oPresent(pSwapChain, SyncInterval, Flags);
}

// Initialize MinHook-based hooks
bool Initialize() {
    if (!g_mhInit.IsInitialized()) {
        HookLog("MinHook-DX12: Failed to initialize MinHook");
        return false;
    }
    
    HookLog("MinHook-DX12: Initializing...");
    
    // Wait for d3d12.dll
    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (!hD3D12) {
        HookLog("MinHook-DX12: d3d12.dll not loaded, delaying initialization");
        return false;
    }
    
    // Hook D3D12CreateDevice
    void* target = reinterpret_cast<void*>(GetProcAddress(hD3D12, "D3D12CreateDevice"));
    if (!target) {
        HookLog("MinHook-DX12: D3D12CreateDevice not found");
        return false;
    }
    
    if (!HookSystem::CreateFunctionHook(target, (void*)Hook_D3D12CreateDevice, 
                                        (void**)&s_origD3D12CreateDevice)) {
        HookLog("MinHook-DX12: Failed to hook D3D12CreateDevice");
        return false;
    }
    
    HookLog("MinHook-DX12: D3D12CreateDevice hooked successfully");
    return true;
}

} // namespace dx12_minhook

// C export to be called from hook/main.cpp
extern "C" bool DX12_InitializeMinHook() {
    if (!dx12_minhook::IsEnabled()) {
        return false;
    }
    return dx12_minhook::Initialize();
}
