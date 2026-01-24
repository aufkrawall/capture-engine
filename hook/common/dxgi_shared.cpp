#include "dxgi_shared.h"
#include "../wrappers/vtable_hook.h"
#include "hook_common.h"
#include "../../common/raii_helpers.h"
#include "performance_metrics.h"
#include "config.h"
#include "logging.h"
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <chrono>
#include <cmath>
#include <atomic>
#include <mutex>
#include <cstdint>

// Helper to check for Vulkan
extern void* g_VulkanHook;

namespace DXGIShared {

SharedState g_SharedState;
std::mutex g_SharedMutex;

// Global metrics for DXGI-based APIs
static PerformanceMetrics g_DXGIPerfMetrics;

// Original function pointers
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers1)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);

static PFN_Present oPresent = nullptr;
static PFN_Present1 oPresent1 = nullptr;
static PFN_ResizeBuffers oResizeBuffers = nullptr;
static PFN_ResizeBuffers1 oResizeBuffers1 = nullptr;

bool IsVulkanPrimary() {
    return (g_VulkanHook != nullptr);
}

PerformanceMetrics* GetPerformanceMetrics() {
    return &g_DXGIPerfMetrics;
}

APIType DetectAPIType(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) return APIType::Unknown;

    ID3D12Device* d12Device = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&d12Device))) {
        d12Device->Release();
        return APIType::D3D12;
    }

    ID3D11Device* d11Device = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&d11Device))) {
        d11Device->Release();
        return APIType::D3D11;
    }

    return APIType::Unknown;
}

// Unified Detours
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    bool isFirstHook = !g_SharedState.inPresentHook.exchange(true);
    auto hookGuard = ::ce::make_scope_guard([&]{ 
        if (isFirstHook) g_SharedState.inPresentHook.store(false);
    });

    g_SharedState.presentCallCount.fetch_add(1, std::memory_order_relaxed);

    if (g_SharedState.deviceRemovedFatal.load()) {
        return oPresent(pSwapChain, SyncInterval, Flags);
    }

    if (g_SharedState.swapchainInvalid.load(std::memory_order_acquire)) {
        return oPresent(pSwapChain, SyncInterval, Flags);
    }

    // FSR4 Cleanup Check
    if (g_SharedState.fsr4RecreationPending.load(std::memory_order_acquire)) {
        APIType api = DetectAPIType(pSwapChain);
        if (api == APIType::D3D12) HandleDX12ResizeBegin();
        else if (api == APIType::D3D11) HandleDX11ResizeBegin();
        g_SharedState.fsr4RecreationPending.store(false);
        g_SharedState.swapchainInvalid.store(false);
    }

    APIType api = DetectAPIType(pSwapChain);
    g_SharedState.frameCount.fetch_add(1, std::memory_order_relaxed);

    // Update Metrics
    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); qpcFreq = f.QuadPart; }
    LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
    int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
    if (isFirstHook) g_DXGIPerfMetrics.Update(us);

    // Process Frame (Overlay/Capture)
    if (api == APIType::D3D12) {
        HandleDX12ProcessFrame(pSwapChain, true);
    } else if (api == APIType::D3D11) {
        HandleDX11ProcessFrame(pSwapChain, true);
    }

    // VSync Override
    VSyncOverride vsync = GetVSyncOverride();
    if (vsync.shouldOverride) SyncInterval = (UINT)vsync.presentInterval;
    if (SyncInterval > 0) Flags &= ~512; // DXGI_PRESENT_ALLOW_TEARING

    return oPresent(pSwapChain, SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    bool isFirstHook = !g_SharedState.inPresentHook.exchange(true);
    auto hookGuard = ::ce::make_scope_guard([&]{ 
        if (isFirstHook) g_SharedState.inPresentHook.store(false);
    });

    if (g_SharedState.deviceRemovedFatal.load() || g_SharedState.swapchainInvalid.load(std::memory_order_acquire)) {
        return oPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }

    APIType api = DetectAPIType(pSwapChain);
    g_SharedState.frameCount.fetch_add(1, std::memory_order_relaxed);

    if (api == APIType::D3D12) {
        HandleDX12ProcessFrame(pSwapChain, true);
    } else if (api == APIType::D3D11) {
        HandleDX11ProcessFrame(pSwapChain, true);
    }

    VSyncOverride vsync = GetVSyncOverride();
    if (vsync.shouldOverride) SyncInterval = (UINT)vsync.presentInterval;
    if (SyncInterval > 0) Flags &= ~512;

    return oPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    if (g_SharedState.wrapperResizeDepth.fetch_add(1) > 0) {
        HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        return hr;
    }

    g_SharedState.swapchainInvalid.store(true);
    
    APIType api = DetectAPIType(pSwapChain);
    if (api == APIType::D3D12) HandleDX12ResizeBegin();
    else if (api == APIType::D3D11) HandleDX11ResizeBegin();
    
    HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    
    if (FAILED(hr)) {
        HookLog("DXGI: ResizeBuffers FAILED with 0x%08X", hr);
    } else {
        HookLog("DXGI: ResizeBuffers SUCCESS");
    }
    
    g_SharedState.swapchainInvalid.store(false);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers1(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue) {
    if (g_SharedState.wrapperResizeDepth.fetch_add(1) > 0) {
        HRESULT hr = oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        return hr;
    }

    g_SharedState.swapchainInvalid.store(true);
    
    APIType api = DetectAPIType(pSwapChain);
    if (api == APIType::D3D12) HandleDX12ResizeBegin();
    else if (api == APIType::D3D11) HandleDX11ResizeBegin();

    HRESULT hr = oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
    
    if (FAILED(hr)) {
        HookLog("DXGI: ResizeBuffers1 FAILED with 0x%08X", hr);
    } else {
        HookLog("DXGI: ResizeBuffers1 SUCCESS");
    }

    g_SharedState.swapchainInvalid.store(false);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    return hr;
}

bool InstallHooks(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) return false;

    void** vtable = *(void***)pSwapChain;
    bool anyInstalled = false;

    // Check if this is our own Wrapper
    IUnknown* pWrapper = nullptr;
    // GUID matching wrapper_base.h: {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
    static const GUID IID_CWrapDXGISwapChainLocal = { 0xa1b2c3d4, 0xe5f6, 0x7890, { 0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90 } };
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChainLocal, (void**)&pWrapper))) {
        pWrapper->Release();
        return true; 
    }

    // Present (8)
    if (vtable[8] != (void*)DetourPresent) {
        VTableHook::Create(&vtable[8], (LPVOID)DetourPresent, (LPVOID*)&oPresent);
        anyInstalled = true;
    }

    // ResizeBuffers (13)
    if (vtable[13] != (void*)DetourResizeBuffers) {
        VTableHook::Create(&vtable[13], (LPVOID)DetourResizeBuffers, (LPVOID*)&oResizeBuffers);
        anyInstalled = true;
    }

    // Present1 (22)
    if (vtable[22] != (void*)DetourPresent1) {
        VTableHook::Create(&vtable[22], (LPVOID)DetourPresent1, (LPVOID*)&oPresent1);
        anyInstalled = true;
    }

    // ResizeBuffers1 (39)
    if (vtable[39] != (void*)DetourResizeBuffers1) {
        VTableHook::Create(&vtable[39], (LPVOID)DetourResizeBuffers1, (LPVOID*)&oResizeBuffers1);
        anyInstalled = true;
    }

    return anyInstalled;
}

void Init() {
    g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();
}

} // namespace DXGIShared
