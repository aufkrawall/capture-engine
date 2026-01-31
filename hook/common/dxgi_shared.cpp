#include "dxgi_shared.h"
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include "../../common/raii_helpers.h"
#include "../wrappers/vtable_hook.h"
#include "../wrappers/wrapper_base.h"
#include "config.h"
#include "fg_detection.h"
#include "hook_common.h"
#include "logging.h"
#include "performance_metrics.h"

// Vulkan is handled by VK_LAYER_CE_overlay (ICD layer approach)
// No global hook pointer needed - extern void* g_VulkanHook;

namespace DXGIShared {

SharedState g_SharedState;
std::mutex g_SharedMutex;

// Global metrics for DXGI-based APIs
static PerformanceMetrics g_DXGIPerfMetrics;

// Recursion detection globals (avoiding thread_local which requires runtime init)
static std::atomic<DWORD> g_presentThreadId{0};
static std::atomic<int> g_presentDepth{0};
static std::atomic<DWORD> g_resizeThreadId{0};
static std::atomic<int> g_resizeDepth{0};

// Helper to check if we're recursively entering from the same thread
static bool IsRecursivePresent() {
    DWORD currentId = GetCurrentThreadId();
    if (g_presentDepth.load() > 0 && g_presentThreadId.load() == currentId) {
        return true;
    }
    // Claim this thread
    g_presentThreadId.store(currentId);
    g_presentDepth.fetch_add(1);
    return false;
}

static void ReleasePresent() {
    if (g_presentDepth.fetch_sub(1) == 1) {
        g_presentThreadId.store(0);
    }
}

static bool IsRecursiveResize() {
    DWORD currentId = GetCurrentThreadId();
    if (g_resizeDepth.load() > 0 && g_resizeThreadId.load() == currentId) {
        return true;
    }
    g_resizeThreadId.store(currentId);
    g_resizeDepth.fetch_add(1);
    return false;
}

static void ReleaseResize() {
    if (g_resizeDepth.fetch_sub(1) == 1) {
        g_resizeThreadId.store(0);
    }
}

// Original function pointers
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers1)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                       const UINT*, IUnknown* const*);

static PFN_Present oPresent = nullptr;
static PFN_Present1 oPresent1 = nullptr;
static PFN_ResizeBuffers oResizeBuffers = nullptr;
static PFN_ResizeBuffers1 oResizeBuffers1 = nullptr;

// Vulkan detection via ICD layer - returns false since we use layer approach
bool IsVulkanPrimary() { 
    // VK_LAYER_CE_overlay handles Vulkan separately
    return false; 
}

PerformanceMetrics* GetPerformanceMetrics() { return &g_DXGIPerfMetrics; }

APIType DetectAPIType(IDXGISwapChain* pSwapChain)
{
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
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion
    if (IsRecursivePresent()) {
        // Recursion detected - call original directly through vtable to bypass Steam's hook
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_Present)(IDXGISwapChain*, UINT, UINT);
        PFN_Present originalPresent = (PFN_Present)vtable[8];  // Present is at index 8
        return originalPresent(pSwapChain, SyncInterval, Flags);
    }
    
    bool isFirstHook = !g_SharedState.inPresentHook.exchange(true);
    auto hookGuard = ::ce::make_scope_guard([&] {
        if (isFirstHook) g_SharedState.inPresentHook.store(false);
        ReleasePresent();
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
        // FSR4 cleanup resets swapchain state but is NOT a resize operation
        // Don't call ResizeBegin here as it would block frame processing indefinitely
        g_SharedState.fsr4RecreationPending.store(false);
        g_SharedState.swapchainInvalid.store(false);
    }

    APIType api = DetectAPIType(pSwapChain);
    g_SharedState.frameCount.fetch_add(1, std::memory_order_relaxed);

    // Update Metrics
    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
    if (isFirstHook) {
        g_DXGIPerfMetrics.Update(us);
        // Also update FG metrics if available
        if (g_FGCompat.IsFGActive()) {
            g_DXGIPerfMetrics.SetFGMetrics(
                g_FGCompat.GetOutputFPS(),
                g_FGCompat.GetBaseFPS(),
                g_FGCompat.GetFGMultiplier()
            );
        } else {
            g_DXGIPerfMetrics.SetFGMetrics(0.0f, 0.0f, 1);
        }
    }

    // Process Frame (Overlay/Capture)
    if (api == APIType::D3D12) {
        HandleDX12ProcessFrame(pSwapChain, true);
    } else if (api == APIType::D3D11) {
        HandleDX11ProcessFrame(pSwapChain, true);
    }

    // VSync Override
    VSyncOverride vsync = GetVSyncOverride();
    if (vsync.shouldOverride) SyncInterval = (UINT)vsync.presentInterval;
    if (SyncInterval > 0) Flags &= ~512;  // DXGI_PRESENT_ALLOW_TEARING

    return oPresent(pSwapChain, SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                         const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion
    if (IsRecursivePresent()) {
        // Recursion detected - call original directly through vtable to bypass Steam's hook
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_Present1)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
        PFN_Present1 originalPresent1 = (PFN_Present1)vtable[14];  // Present1 is at index 14
        return originalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }
    
    bool isFirstHook = !g_SharedState.inPresentHook.exchange(true);
    auto hookGuard = ::ce::make_scope_guard([&] {
        if (isFirstHook) g_SharedState.inPresentHook.store(false);
        ReleasePresent();
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

HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                              DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion through hook chain
    if (IsRecursiveResize()) {
        // Recursion detected - call original directly through vtable to bypass Steam's hook
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        PFN_ResizeBuffers originalResize = (PFN_ResizeBuffers)vtable[13];  // ResizeBuffers is at index 13
        return originalResize(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    
    if (g_SharedState.wrapperResizeDepth.fetch_add(1) > 0) {
        HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    // Check if this is our wrapper swapchain - if so, skip resize handling
    void* pWrapperTest = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapperTest))) {
        ((IUnknown*)pWrapperTest)->Release();
        HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    g_SharedState.swapchainInvalid.store(true);

    APIType api = DetectAPIType(pSwapChain);
    if (api == APIType::D3D12)
        HandleDX12ResizeBegin();
    else if (api == APIType::D3D11)
        HandleDX11ResizeBegin();

    HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    if (FAILED(hr)) {
        HookLog("DXGI: ResizeBuffers FAILED with 0x%08X", hr);
    } else {
        HookLog("DXGI: ResizeBuffers SUCCESS");
    }

    // Reset resize flags after resize completes
    if (api == APIType::D3D12)
        HandleDX12ResizeEnd();

    g_SharedState.swapchainInvalid.store(false);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    ReleaseResize();
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers1(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                               DXGI_FORMAT NewFormat, UINT SwapChainFlags,
                                               const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue)
{
    // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion through hook chain
    if (IsRecursiveResize()) {
        // Recursion detected - call original directly through vtable to bypass Steam's hook
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers1)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
        PFN_ResizeBuffers1 originalResize1 = (PFN_ResizeBuffers1)vtable[39];  // ResizeBuffers1 is at index 39
        return originalResize1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
    }
    
    if (g_SharedState.wrapperResizeDepth.fetch_add(1) > 0) {
        HRESULT hr = oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags,
                                     pCreationNodeMask, ppPresentQueue);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    // Check if this is our wrapper swapchain - if so, skip resize handling
    void* pWrapperTest = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapperTest))) {
        ((IUnknown*)pWrapperTest)->Release();
        HRESULT hr = oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags,
                                     pCreationNodeMask, ppPresentQueue);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    g_SharedState.swapchainInvalid.store(true);

    APIType api = DetectAPIType(pSwapChain);
    if (api == APIType::D3D12)
        HandleDX12ResizeBegin();
    else if (api == APIType::D3D11)
        HandleDX11ResizeBegin();

    HRESULT hr = oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask,
                                 ppPresentQueue);

    if (FAILED(hr)) {
        HookLog("DXGI: ResizeBuffers1 FAILED with 0x%08X", hr);
    } else {
        HookLog("DXGI: ResizeBuffers1 SUCCESS");
    }

    // Reset resize flags after resize completes
    if (api == APIType::D3D12)
        HandleDX12ResizeEnd();

    g_SharedState.swapchainInvalid.store(false);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    ReleaseResize();
    return hr;
}

bool InstallHooks(IDXGISwapChain* pSwapChain)
{
    if (!pSwapChain) return false;

    void** vtable = *(void***)pSwapChain;
    bool anyInstalled = false;

    // Check if this is our own Wrapper
    IUnknown* pWrapper = nullptr;
    // GUID matching wrapper_base.h: {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
    static const GUID IID_CWrapDXGISwapChainLocal = {
        0xa1b2c3d4, 0xe5f6, 0x7890, {0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90}};
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

void Init() { g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now(); }

}  // namespace DXGIShared
