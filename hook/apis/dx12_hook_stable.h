#pragma once

#include "graphics_hook.h"
#include "../wrappers/hook_system.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// D3D12 function pointer typedefs
typedef HRESULT (WINAPI* PFN_D3D12CreateDevice)(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice
);
#include <unordered_map>
#include <memory>
#include <mutex>

// Forward declarations
struct IDXGISwapChain;
struct ID3D12Device;
struct ID3D12CommandQueue;

namespace dx12_stable {

// Feature flag: Use MinHook-based implementation
// This can be controlled via config or compile-time flag
bool IsMinHookEnabled();

// Main hook class using MinHook
class DX12HookStable : public GraphicsHook {
public:
    DX12HookStable();
    ~DX12HookStable();

    void Init() override;
    void Shutdown() override;
    void OnHostDisconnect() override;

    // Called when D3D12CreateDevice is hooked
    static HRESULT WINAPI Hook_D3D12CreateDevice(
        IUnknown* pAdapter,
        D3D_FEATURE_LEVEL MinimumFeatureLevel,
        REFIID riid,
        void** ppDevice);

    // Called when CreateSwapChain is hooked
    static HRESULT WINAPI Hook_CreateSwapChain(
        IDXGIFactory* pFactory,
        IUnknown* pDevice,
        DXGI_SWAP_CHAIN_DESC* pDesc,
        IDXGISwapChain** ppSwapChain);

    // Called when CreateSwapChainForHwnd is hooked
    static HRESULT WINAPI Hook_CreateSwapChainForHwnd(
        IDXGIFactory2* pFactory,
        IUnknown* pDevice,
        HWND hWnd,
        const DXGI_SWAP_CHAIN_DESC1* pDesc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc,
        IDXGIOutput* pOutput,
        IDXGISwapChain1** ppSwapChain);

    // Called on every Present - same for all frames (FG or not)
    static HRESULT WINAPI Hook_Present(
        IDXGISwapChain* pSwapChain,
        UINT SyncInterval,
        UINT Flags);

    // Called on Present1 (DXGI 1.2+)
    static HRESULT WINAPI Hook_Present1(
        IDXGISwapChain1* pSwapChain,
        UINT SyncInterval,
        UINT Flags,
        const DXGI_PRESENT_PARAMETERS* pPresentParameters);

    // Called on ResizeBuffers
    static HRESULT WINAPI Hook_ResizeBuffers(
        IDXGISwapChain* pSwapChain,
        UINT BufferCount,
        UINT Width,
        UINT Height,
        DXGI_FORMAT NewFormat,
        UINT SwapChainFlags);

    // ExecuteCommandLists - for frame detection if needed
    static void WINAPI Hook_ExecuteCommandLists(
        ID3D12CommandQueue* pCommandQueue,
        UINT NumCommandLists,
        ID3D12CommandList* const* ppCommandLists);

private:
    // MinHook initialization
    HookSystem::ScopedInitializer m_mhInit;

    // Hook handles for global hooks (exports and factory vtable)
    struct GlobalHooks {
        HookSystem::TypedHook<PFN_D3D12CreateDevice> D3D12CreateDevice;
        HookSystem::TypedHook<decltype(&CreateDXGIFactory1)> CreateDXGIFactory1;
    } m_globalHooks;

    // Factory vtable hooks (installed once on first device creation)
    bool m_factoryHooksInstalled = false;
    std::mutex m_factoryHookMutex;

    // Per-swapchain tracking
    struct SwapchainData {
        IDXGISwapChain* swapchain = nullptr;
        UINT width = 0;
        UINT height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        bool imguiInitialized = false;
        bool needsReinit = false;
        int frameCounter = 0;
        HWND hwnd = nullptr;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
    };
    std::unordered_map<IDXGISwapChain*, std::unique_ptr<SwapchainData>> m_swapchainData;
    std::mutex m_swapchainMutex;

    // Original function pointers (filled by MinHook)
    static PFN_D3D12CreateDevice s_origD3D12CreateDevice;
    static decltype(&CreateDXGIFactory1) s_origCreateDXGIFactory1;
    static decltype(&Hook_CreateSwapChain) s_origCreateSwapChain;
    static decltype(&Hook_CreateSwapChainForHwnd) s_origCreateSwapChainForHwnd;
    static decltype(&Hook_Present) s_origPresent;
    static decltype(&Hook_Present1) s_origPresent1;
    static decltype(&Hook_ResizeBuffers) s_origResizeBuffers;
    static decltype(&Hook_ExecuteCommandLists) s_origExecuteCommandLists;

    // Internal methods
    bool InstallD3D12CreateDeviceHook();
    bool InstallFactoryHooks();
    bool InstallSwapchainHooks(IDXGISwapChain* swapchain);
    
    SwapchainData* GetSwapchainData(IDXGISwapChain* swapchain);
    void RemoveSwapchainData(IDXGISwapChain* swapchain);
    
public:
    // Present processing - same for every frame, no FG detection
    // Made public so C export functions can access it
    void ProcessPresent(IDXGISwapChain* swapchain, bool isPresent1 = false);
    
private:
    
    // ImGui overlay
    bool InitImGui(SwapchainData* data, ID3D12Device* device, IDXGISwapChain* swapchain,
                   const DXGI_SWAP_CHAIN_DESC& desc);
    void ShutdownImGui(SwapchainData* data);
    void RenderOverlay(SwapchainData* data, ID3D12Device* device, IDXGISwapChain* swapchain);
};

// Global instance (similar to current architecture)
extern DX12HookStable* g_dx12HookStable;

// C-compatible exports for cross-module calls
extern "C" {
    void DX12Stable_SetCommandQueue(ID3D12CommandQueue* pQueue);
    void DX12Stable_ProcessFrame(IDXGISwapChain* pSwapChain);
}

} // namespace dx12_stable

// Factory function to create the appropriate hook implementation
std::unique_ptr<GraphicsHook> CreateDX12Hook();
