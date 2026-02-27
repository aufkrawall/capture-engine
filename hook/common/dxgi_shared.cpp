#include "dxgi_shared.h"
#include "../../common/raii_helpers.h"
#include "../wrappers/inline_hook.h"
#include "../wrappers/vtable_hook.h"
#include "../wrappers/wrapper_base.h"
#include "config.h"
#include "fg_detection.h"
#include "fps_limiter.h"
#include "freeze_watchdog.h"
#include "hook_common.h"
#include "logging.h"
#include "performance_metrics.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

// Vulkan is handled by VK_LAYER_CE_overlay (ICD layer approach)
// No global hook pointer needed - extern void* g_VulkanHook;

// Put shutdown check outside the DXGIShared namespace
static bool IsShuttingDown() {
    extern std::atomic<bool> g_ShuttingDown;
    return g_ShuttingDown.load();
}

// Check if we're inside a CWrapDXGISwapChain Present call
extern bool IsInWrapperPresent();

namespace DXGIShared {

SharedState g_SharedState;
std::mutex g_SharedMutex;

// Global metrics for DXGI-based APIs
static PerformanceMetrics g_DXGIPerfMetrics;

// Recursion detection globals (avoiding thread_local which requires runtime
// init)
static std::atomic<DWORD> g_presentThreadId{0};
static std::atomic<int> g_presentDepth{0};
static std::atomic<DWORD> g_resizeThreadId{0};
static std::atomic<int> g_resizeDepth{0};

// Helper to check if we're recursively entering from the same thread
static bool IsRecursivePresent() {
    DWORD currentId = GetCurrentThreadId();
    DWORD expected = g_presentThreadId.load(std::memory_order_acquire);

    // Fast path: same thread re-entering (recursion)
    if (expected == currentId && g_presentDepth.load(std::memory_order_acquire) > 0) {
        return true;
    }

    // Try to claim ownership atomically — only one thread can succeed
    DWORD zero = 0;
    if (g_presentThreadId.compare_exchange_strong(zero, currentId, std::memory_order_acq_rel)) {
        g_presentDepth.store(1, std::memory_order_release);
        return false;
    }

    // Another thread owns it — if it's us (race between check and CAS), re-check
    if (g_presentThreadId.load(std::memory_order_acquire) == currentId) {
        g_presentDepth.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    // Different thread owns it — treat as non-recursive, let it proceed
    // (this matches original behavior: each thread processes Present
    // independently)
    return false;
}

static void ReleasePresent() {
    if (g_presentDepth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        g_presentThreadId.store(0, std::memory_order_release);
    }
}

static bool IsRecursiveResize() {
    DWORD currentId = GetCurrentThreadId();
    DWORD expected = g_resizeThreadId.load(std::memory_order_acquire);

    if (expected == currentId && g_resizeDepth.load(std::memory_order_acquire) > 0) {
        return true;
    }

    DWORD zero = 0;
    if (g_resizeThreadId.compare_exchange_strong(zero, currentId, std::memory_order_acq_rel)) {
        g_resizeDepth.store(1, std::memory_order_release);
        return false;
    }

    if (g_resizeThreadId.load(std::memory_order_acquire) == currentId) {
        g_resizeDepth.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    return false;
}

static void ReleaseResize() {
    if (g_resizeDepth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        g_resizeThreadId.store(0, std::memory_order_release);
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

// Inline hook trampolines - calling these bypasses the hook entirely
static PFN_Present oPresentTrampoline = nullptr;
static PFN_Present1 oPresent1Trampoline = nullptr;

// Stored vtable pointer for unhooking Present when COM wrapper takes over
static void** s_hookedVTable = nullptr;

// Vulkan detection via ICD layer - returns false since we use layer approach
bool IsVulkanPrimary() {
    // VK_LAYER_CE_overlay handles Vulkan separately
    return false;
}

PerformanceMetrics* GetPerformanceMetrics() {
    return &g_DXGIPerfMetrics;
}

APIType DetectAPIType(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return APIType::Unknown;

    // Fast path: avoid expensive GetDevice() calls every Present on the same
    // swapchain/thread.
    thread_local IDXGISwapChain* s_cachedSwapchain = nullptr;
    thread_local APIType s_cachedApi = APIType::Unknown;
    if (pSwapChain == s_cachedSwapchain && s_cachedApi != APIType::Unknown) {
        return s_cachedApi;
    }

    APIType detected = APIType::Unknown;

    ID3D12Device* d12Device = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&d12Device))) {
        d12Device->Release();
        detected = APIType::D3D12;
    } else {
        ID3D11Device* d11Device = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&d11Device))) {
            d11Device->Release();
            detected = APIType::D3D11;
        }
    }

    s_cachedSwapchain = pSwapChain;
    s_cachedApi = detected;
    return detected;
}

// Helper to get Present function address from a swapchain's vtable
static void* GetPresentAddress(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return nullptr;
    void** vtable = *(void***)pSwapChain;
    return vtable[8];  // Present is at vtable slot 8
}

// Helper to get Present1 function address from a swapchain's vtable
static void* GetPresent1Address(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return nullptr;
    void** vtable = *(void***)pSwapChain;
    return vtable[22];  // Present1 is at vtable slot 22
}

// Global flag to disable DXGI hooks when Vulkan is active
// This is set once at startup and prevents DXGI hooks from interfering with
// Vulkan WSI
static bool s_vulkanPresent = false;
static bool s_checkedVulkan = false;

// Forward declaration for lazy hook installation
static void InstallHooksIfPending(IDXGISwapChain* pSwapChain);

static bool IsVulkanActive() {
    if (!s_checkedVulkan) {
        HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
        s_vulkanPresent = (hVulkan != nullptr);
        if (s_vulkanPresent) {
            HookLog(
                "DXGIShared: Vulkan detected (vulkan-1.dll), DXGI hooks will "
                "pass through");
        }
        s_checkedVulkan = true;
    }
    return s_vulkanPresent;
}

static bool IsSteamOverlayLoaded() {
    return GetModuleHandleA("gameoverlayrenderer64.dll") != nullptr ||
           GetModuleHandleA("gameoverlayrenderer.dll") != nullptr;
}

// Unified Detours
// For DX12 wrapped swapchains: CWrapDXGISwapChain handles Present, so when
// wrapper calls m_pReal->Present() and it re-enters here, we just passthrough.
// For DX12 pre-existing swapchains (not wrapped): full processing here.
// For DX11: full processing here.
static bool IsReadableMemory(const void* ptr, size_t size) {
    if (!ptr)
        return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    return (mbi.Protect &
            (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) != 0;
}

namespace {
thread_local bool s_bypassVtableHook = false;
}

HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    static int s_entryCount = 0;
    int entryNum = ++s_entryCount;
    if (entryNum <= 10) {
        HookLog(
            "DetourPresent: ENTRY #%d (pSwapChain=%p, IsInWrapper=%d, "
            "trampoline=%p)",
            entryNum, pSwapChain, IsInWrapperPresent() ? 1 : 0, oPresentTrampoline);
    }

    if (!pSwapChain) {
        return DXGI_ERROR_INVALID_CALL;
    }

    if (IsShuttingDown()) {
        if (IsReadableMemory(pSwapChain, sizeof(void*))) {
            return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
        }
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!IsReadableMemory(pSwapChain, sizeof(void*))) {
        g_ShuttingDown.store(true);
        return DXGI_ERROR_INVALID_CALL;
    }
    void** vtable = *(void***)pSwapChain;
    if (!vtable || !IsReadableMemory(vtable, 9 * sizeof(void*)) || !vtable[8]) {
        g_ShuttingDown.store(true);
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!oPresentTrampoline && !oPresent) {
        HookLog("DetourPresent: No original Present function available");
        return DXGI_ERROR_INVALID_CALL;
    }

    void* pWrapper = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapper))) {
        ((IUnknown*)pWrapper)->Release();
        static int s_wrappedPassCount = 0;
        if (s_wrappedPassCount < 5) {
            s_wrappedPassCount++;
            HookLog(
                "DetourPresent: Wrapped swapchain detected, passing through via "
                "trampoline #%d",
                s_wrappedPassCount);
        }
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    if (IsInWrapperPresent()) {
        static int s_inWrapperPassCount = 0;
        if (s_inWrapperPassCount < 5) {
            s_inWrapperPassCount++;
            HookLog(
                "DetourPresent: IsInWrapperPresent=true, passing through via "
                "trampoline #%d",
                s_inWrapperPassCount);
        }
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    // Break infinite re-entrancy loop caused by external overlays (e.g. Steam's
    // gameoverlayrenderer64.dll) that read vtable[8] dynamically inside their hook
    // and call back into DetourPresent instead of using a saved trampoline.
    if (IsRecursivePresent()) {
        static int s_loopBreakCount = 0;
        if (s_loopBreakCount++ < 3) {
            HookLogImportant("DetourPresent: Re-entrancy loop broken (external overlay compat)");
        }
        return S_OK;
    }
    auto presentDepthGuard = ce::make_scope_guard([]() { ReleasePresent(); });

    static int s_processCount = 0;
    if (s_processCount < 5) {
        s_processCount++;
        HookLog("DetourPresent: Processing frame #%d (not wrapped, not in wrapper)", s_processCount);
    }

    g_RenderWatchdog.Heartbeat();

    if (IsVulkanActive()) {
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    // Lazy check for NvPresent64.dll (may load after our hooks)
    g_FGCompat.CheckForNvPresent();

    // NVIDIA Smooth Motion compatibility: skip overlay/processing for invisible
    // windows. NvPresent64 creates invisible-window swapchains for DX11 frame
    // interpolation — processing them corrupts NvPresent64's internal state.
    if (g_FGCompat.IsNvPresentLoaded()) {
        DXGI_SWAP_CHAIN_DESC smDesc = {};
        if (SUCCEEDED(pSwapChain->GetDesc(&smDesc))) {
            if (!smDesc.OutputWindow || !IsWindowVisible(smDesc.OutputWindow)) {
                static int s_smSkipCount = 0;
                if (s_smSkipCount < 5) {
                    s_smSkipCount++;
                    HookLog(
                        "DetourPresent: Skipping invisible window (SM compat, "
                        "hwnd=%p) #%d",
                        smDesc.OutputWindow, s_smSkipCount);
                }
                return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
            }
        }
    }

    bool isFirstHook = !g_SharedState.inPresentHook.exchange(true);
    auto hookGuard = ::ce::make_scope_guard([&] {
        if (isFirstHook)
            g_SharedState.inPresentHook.store(false);
    });

    g_SharedState.presentCallCount.fetch_add(1, std::memory_order_relaxed);

    if (g_SharedState.deviceRemovedFatal.load()) {
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    if (g_SharedState.swapchainInvalid.load(std::memory_order_acquire)) {
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    APIType api = DetectAPIType(pSwapChain);
    g_SharedState.frameCount.fetch_add(1, std::memory_order_relaxed);

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
        if (g_FGCompat.IsFGActive()) {
            g_DXGIPerfMetrics.SetFGMetrics(g_FGCompat.GetOutputFPS(), g_FGCompat.GetBaseFPS(),
                                           g_FGCompat.GetFGMultiplier());
        } else {
            g_DXGIPerfMetrics.SetFGMetrics(0.0f, 0.0f, 1);
        }
    }

    if (!IsShuttingDown() && (oPresentTrampoline || oPresent)) {
        if (api == APIType::D3D12) {
            HandleDX12ProcessFrame(pSwapChain, true);
        } else if (api == APIType::D3D11) {
            HandleDX11ProcessFrame(pSwapChain, true);
        }
    }

    // FPS Limiter - apply frame pacing before present
    if (g_IPC) {
        g_SharedFpsLimiter.SetIPCClient(g_IPC);
        g_SharedFpsLimiter.Apply();
    }

    ProcessVSyncOverride(SyncInterval, Flags);

    HRESULT hr = CallOriginalPresent(pSwapChain, SyncInterval, Flags);

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        if (!g_SharedState.deviceRemovedFatal.exchange(true)) {
            HookLog("DXGI: Device removed (hr=0x%08X), disabling hooks", hr);
        }
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                         const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    if (!pSwapChain) {
        return DXGI_ERROR_INVALID_CALL;
    }

    if (IsShuttingDown()) {
        if (IsReadableMemory(pSwapChain, sizeof(void*))) {
            return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!IsReadableMemory(pSwapChain, sizeof(void*))) {
        g_ShuttingDown.store(true);
        return DXGI_ERROR_INVALID_CALL;
    }
    void** vtable = *(void***)pSwapChain;
    if (!vtable || !IsReadableMemory(vtable, 23 * sizeof(void*)) || !vtable[22]) {
        g_ShuttingDown.store(true);
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!oPresent1Trampoline && !oPresent1) {
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    void* pWrapper = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapper))) {
        ((IUnknown*)pWrapper)->Release();
        return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }

    if (IsInWrapperPresent()) {
        return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }

    // Break re-entrancy loop (same external overlay pattern as DetourPresent).
    if (IsRecursivePresent()) {
        static int s_loopBreakCount1 = 0;
        if (s_loopBreakCount1++ < 3) {
            HookLogImportant("DetourPresent1: Re-entrancy loop broken (external overlay compat)");
        }
        return S_OK;
    }
    auto presentDepthGuard = ce::make_scope_guard([]() { ReleasePresent(); });

    g_RenderWatchdog.Heartbeat();

    if (IsVulkanActive()) {
        return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }

    // NVIDIA Smooth Motion compatibility: skip for invisible windows
    if (g_FGCompat.IsNvPresentLoaded()) {
        DXGI_SWAP_CHAIN_DESC smDesc = {};
        if (SUCCEEDED(pSwapChain->GetDesc(&smDesc))) {
            if (!smDesc.OutputWindow || !IsWindowVisible(smDesc.OutputWindow)) {
                return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
            }
        }
    }

    bool isFirstHook = !g_SharedState.inPresentHook.exchange(true);
    auto hookGuard = ::ce::make_scope_guard([&] {
        if (isFirstHook)
            g_SharedState.inPresentHook.store(false);
    });

    if (g_SharedState.deviceRemovedFatal.load() || g_SharedState.swapchainInvalid.load(std::memory_order_acquire)) {
        return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }

    APIType api = DetectAPIType(pSwapChain);
    g_SharedState.frameCount.fetch_add(1, std::memory_order_relaxed);

    if (api == APIType::D3D12) {
        HandleDX12ProcessFrame(pSwapChain, true);
    } else if (api == APIType::D3D11) {
        HandleDX11ProcessFrame(pSwapChain, true);
    }

    // FPS Limiter - apply frame pacing before present
    if (g_IPC) {
        g_SharedFpsLimiter.SetIPCClient(g_IPC);
        g_SharedFpsLimiter.Apply();
    }

    ProcessVSyncOverride(SyncInterval, Flags);

    HRESULT hr = CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        if (!g_SharedState.deviceRemovedFatal.exchange(true)) {
            HookLog("DXGI: Device removed (hr=0x%08X), disabling hooks", hr);
        }
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                              DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    // CRITICAL: Check for global shutdown - if app is closing, don't touch
    // anything
    if (IsShuttingDown()) {
        if (oResizeBuffers) {
            return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        }
        return S_OK;
    }

    // CRITICAL FIX: When Vulkan is active, pass through DXGI ResizeBuffers calls
    if (IsVulkanActive()) {
        return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion through
    // hook chain
    if (IsRecursiveResize()) {
        // Recursion detected - call original directly through vtable to bypass
        // Steam's hook
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

    // CRITICAL FIX: Skip DX12 resize handling during initial swapchain creation
    // Some games call ResizeBuffers immediately after CreateSwapChain
    static std::atomic<int> s_initialResizeCount{0};
    if (api == APIType::D3D12 && s_initialResizeCount.fetch_add(1) == 0) {
        HookLog("DXGI: ResizeBuffers - FIRST D3D12 resize, direct vtable call");
        // Call directly through vtable to bypass any hook chain issues
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        PFN_ResizeBuffers originalResize = (PFN_ResizeBuffers)vtable[13];
        HRESULT hr = originalResize(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        HookLog("DXGI: ResizeBuffers - first D3D12 resize returned hr=0x%08X", hr);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    if (api == APIType::D3D12)
        HandleDX12ResizeBegin();
    else if (api == APIType::D3D11)
        HandleDX11ResizeBegin();

    HookLog("DXGI: ResizeBuffers - calling oResizeBuffers...");
    HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    HookLog("DXGI: ResizeBuffers - oResizeBuffers returned hr=0x%08X", hr);

    if (FAILED(hr)) {
        HookLog("DXGI: ResizeBuffers FAILED with 0x%08X", hr);
    } else {
        HookLog("DXGI: ResizeBuffers SUCCESS");
    }

    // Reset resize flags after resize completes
    if (api == APIType::D3D12) {
        HookLog("DXGI: ResizeBuffers - calling HandleDX12ResizeEnd...");
        HandleDX12ResizeEnd();
        HookLog("DXGI: ResizeBuffers - HandleDX12ResizeEnd returned");
    }

    g_SharedState.swapchainInvalid.store(false);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    ReleaseResize();
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers1(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                               DXGI_FORMAT NewFormat, UINT SwapChainFlags,
                                               const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue) {
    // Vulkan passthrough
    if (IsVulkanActive()) {
        return oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask,
                               ppPresentQueue);
    }

    // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion through
    // hook chain
    if (IsRecursiveResize()) {
        // Recursion detected - call original directly through vtable to bypass
        // Steam's hook
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers1)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                                const UINT*, IUnknown* const*);
        PFN_ResizeBuffers1 originalResize1 = (PFN_ResizeBuffers1)vtable[39];  // ResizeBuffers1 is at index 39
        return originalResize1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask,
                               ppPresentQueue);
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

    // CRITICAL FIX: Skip DX12 resize handling during initial swapchain creation
    // Some games call ResizeBuffers immediately after CreateSwapChain
    static std::atomic<int> s_initialResizeCount{0};
    if (api == APIType::D3D12 && s_initialResizeCount.fetch_add(1) == 0) {
        HookLog("DXGI: ResizeBuffers - FIRST D3D12 resize, direct vtable call");
        // Call directly through vtable to bypass any hook chain issues
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        PFN_ResizeBuffers originalResize = (PFN_ResizeBuffers)vtable[13];
        HRESULT hr = originalResize(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        HookLog("DXGI: ResizeBuffers - first D3D12 resize returned hr=0x%08X", hr);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

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

bool InstallHooks(IDXGISwapChain* pSwapChain, bool presentOnly) {
    // NOTE: This function should only be called for DX11/DX10 games.
    // DX12 games use wrapper-based Present interception (CWrapDXGISwapChain).
    // Calling this for DX12 can cause conflicts and stack overflow crashes
    // due to two competing Present interception mechanisms.
    // See: dx12_hook.cpp for the wrapper-based approach.

    if (!pSwapChain)
        return false;

    static std::atomic<int> s_installCount{0};
    int count = s_installCount.fetch_add(1);
    HookLog("DXGIShared::InstallHooks CALLED #%d (swapchain=%p, presentOnly=%d)", count, pSwapChain,
            presentOnly ? 1 : 0);

    // Steam overlay + DXGI vtable hooks can create recursive Present chains.
    // In this case wrapper-based interception remains active and avoids hook wars.
    if (IsSteamOverlayLoaded()) {
        HookLog("DXGIShared::InstallHooks: Steam overlay detected, skipping DXGI swapchain hooks");
        return true;
    }

    if (s_hookedVTable) {
        HookLog("DXGIShared::InstallHooks: Hooks already installed on vtable %p", s_hookedVTable);
        return true;
    }

    void** vtable = *(void***)pSwapChain;
    if (!vtable) {
        HookLog("DXGIShared::InstallHooks: Invalid vtable");
        return false;
    }

    DWORD oldProtect;
    if (!VirtualProtect(vtable, 40 * sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        HookLog("DXGIShared::InstallHooks: VirtualProtect failed");
        return false;
    }

    s_hookedVTable = vtable;

    oPresent = (PFN_Present)vtable[8];
    vtable[8] = (void*)DetourPresent;
    HookLog("DXGIShared: Hooked Present at vtable[8] (original=%p, detour=%p)", oPresent, DetourPresent);

    oPresent1 = (PFN_Present1)vtable[22];
    vtable[22] = (void*)DetourPresent1;
    HookLog("DXGIShared: Hooked Present1 at vtable[22] (original=%p, detour=%p)", oPresent1, DetourPresent1);

    if (!presentOnly) {
        oResizeBuffers = (PFN_ResizeBuffers)vtable[13];
        vtable[13] = (void*)DetourResizeBuffers;
        HookLog(
            "DXGIShared: Hooked ResizeBuffers at vtable[13] (original=%p, "
            "detour=%p)",
            oResizeBuffers, DetourResizeBuffers);

        oResizeBuffers1 = (PFN_ResizeBuffers1)vtable[39];
        vtable[39] = (void*)DetourResizeBuffers1;
        HookLog(
            "DXGIShared: Hooked ResizeBuffers1 at vtable[39] (original=%p, "
            "detour=%p)",
            oResizeBuffers1, DetourResizeBuffers1);
    }

    VirtualProtect(vtable, 40 * sizeof(void*), oldProtect, &oldProtect);
    HookLog("DXGIShared::InstallHooks: All vtable hooks installed successfully");
    return true;
}

// Lazy hook installation - installs hooks on first Present if they were
// deferred during swapchain creation
static IDXGISwapChain* s_PendingSwapChainForLazyHook = nullptr;
static std::atomic<bool> s_LazyHooksInstalled{false};

void SetPendingSwapChainForLazyHook(IDXGISwapChain* pSwapChain) {
    if (pSwapChain) {
        pSwapChain->AddRef();
    }
    if (s_PendingSwapChainForLazyHook) {
        s_PendingSwapChainForLazyHook->Release();
    }
    s_PendingSwapChainForLazyHook = pSwapChain;
    HookLog("DXGIShared: SetPendingSwapChainForLazyHook called");
}

static void InstallHooksIfPending(IDXGISwapChain* pSwapChain) {
    if (s_LazyHooksInstalled.load(std::memory_order_acquire))
        return;

    // Check if this is the pending swapchain
    if (pSwapChain == s_PendingSwapChainForLazyHook) {
        HookLog("DXGIShared: Installing hooks lazily on first Present");
        // CRITICAL: Use presentOnly=true to only hook Present/Present1
        // ResizeBuffers hooks can cause stack overflow crashes with some overlays
        InstallHooks(pSwapChain, true);
        s_LazyHooksInstalled.store(true, std::memory_order_release);
        if (s_PendingSwapChainForLazyHook) {
            s_PendingSwapChainForLazyHook->Release();
            s_PendingSwapChainForLazyHook = nullptr;
        }
    }
}

void Init() {
    g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();
    // Early detection of NVIDIA Smooth Motion module
    g_FGCompat.CheckForNvPresent();
}

bool InstallPresentInlineHooks(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return false;

    void* presentAddr = GetPresentAddress(pSwapChain);
    void* present1Addr = GetPresent1Address(pSwapChain);

    if (!presentAddr) {
        HookLog("InstallPresentInlineHooks: Failed to get Present address");
        return false;
    }

    static bool s_inlineHooksInstalled = false;
    if (s_inlineHooksInstalled) {
        HookLog("InstallPresentInlineHooks: Inline hooks already installed");
        return true;
    }

    // CRITICAL: Check if an external overlay has already hooked Present
    // External overlays (NVIDIA, Steam, Discord, etc.) actively re-hook Present
    // Fighting them causes a hook war that corrupts the call chain
    const uint8_t* code = (const uint8_t*)presentAddr;
    bool externalJmpDetected = false;

    if (code[0] == 0xE9) {
        // JMP rel32 detected - check where it points
        int32_t disp;
        memcpy(&disp, code + 1, 4);
        uintptr_t jumpTarget = (uintptr_t)(code + 5) + disp;

        // Check if the jump target is outside dxgi.dll
        HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
        if (hDXGI) {
            MODULEINFO dxgiInfo;
            if (GetModuleInformation(GetCurrentProcess(), hDXGI, &dxgiInfo, sizeof(dxgiInfo))) {
                uintptr_t dxgiStart = (uintptr_t)hDXGI;
                uintptr_t dxgiEnd = dxgiStart + dxgiInfo.SizeOfImage;

                if (jumpTarget < dxgiStart || jumpTarget >= dxgiEnd) {
                    externalJmpDetected = true;
                    // JMP points outside dxgi.dll - external overlay detected
                    HookLog("InstallPresentInlineHooks: External overlay detected!");
                    HookLog("InstallPresentInlineHooks: JMP at %p targets %p (outside dxgi.dll %p-%p)", presentAddr,
                            (void*)jumpTarget, (void*)dxgiStart, (void*)dxgiEnd);

                    // Check if the target module is a known overlay
                    HMODULE hTargetModule = nullptr;
                    GetModuleHandleExA(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)jumpTarget, &hTargetModule);

                    if (hTargetModule) {
                        char moduleName[MAX_PATH] = {0};
                        GetModuleFileNameA(hTargetModule, moduleName, MAX_PATH);
                        HookLog("InstallPresentInlineHooks: External overlay module: %s", moduleName);

                        // Known overlay modules that we should cooperate with
                        std::string moduleLower = moduleName;
                        std::transform(moduleLower.begin(), moduleLower.end(), moduleLower.begin(), ::tolower);

                        if (moduleLower.find("nvidia") != std::string::npos ||
                            moduleLower.find("nvngx") != std::string::npos ||
                            moduleLower.find("steam") != std::string::npos ||
                            moduleLower.find("gameoverlay") != std::string::npos ||
                            moduleLower.find("discord") != std::string::npos ||
                            moduleLower.find("overlay") != std::string::npos) {
                            HookLog("InstallPresentInlineHooks: External hook detected - cooperating (known overlay)");
                        }
                    } else {
                        // Skip inline hooks to prevent prologue corruption (black screen fix)
                        HookLog("InstallPresentInlineHooks: Unknown external JMP - possibly stale hook");
                    }
                }
            }
        }
    }

    if (externalJmpDetected) {
        // External overlay is actively hooking Present
        // Skip inline hook installation and rely on swapchain wrapper
        HookLog("InstallPresentInlineHooks: Deferring to external overlay");
        HookLog("InstallPresentInlineHooks: Swapchain wrapper will handle frame processing");
        s_inlineHooksInstalled = true;  // Mark as "installed" to prevent repeated attempts
        return true;                    // Return success to allow initialization to continue
    }

    void* presentTrampoline = nullptr;
    if (!InlineHook::Install(presentAddr, (void*)DetourPresent, &presentTrampoline)) {
        HookLog("InstallPresentInlineHooks: Failed to install Present inline hook");
        return false;
    }
    oPresentTrampoline = (PFN_Present)presentTrampoline;
    oPresent = oPresentTrampoline;
    HookLog(
        "InstallPresentInlineHooks: Present inline hook installed (addr=%p, "
        "trampoline=%p)",
        presentAddr, presentTrampoline);

    if (present1Addr) {
        void* present1Trampoline = nullptr;
        if (InlineHook::Install(present1Addr, (void*)DetourPresent1, &present1Trampoline)) {
            oPresent1Trampoline = (PFN_Present1)present1Trampoline;
            oPresent1 = oPresent1Trampoline;
            HookLog(
                "InstallPresentInlineHooks: Present1 inline hook installed "
                "(addr=%p, trampoline=%p)",
                present1Addr, present1Trampoline);
        }
    }

    s_inlineHooksInstalled = true;
    return true;
}

void RemovePresentHooks() {
    InlineHook::RemoveAll();

    if (!s_hookedVTable || !oPresent)
        return;

    DWORD oldProtect;
    if (s_hookedVTable[8] == (void*)DetourPresent) {
        VirtualProtect(&s_hookedVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[8] = (void*)oPresent;
        VirtualProtect(&s_hookedVTable[8], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present vtable hook");
    }

    if (oPresent1 && s_hookedVTable[22] == (void*)DetourPresent1) {
        VirtualProtect(&s_hookedVTable[22], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[22] = (void*)oPresent1;
        VirtualProtect(&s_hookedVTable[22], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present1 vtable hook");
    }
}

void RemoveSwapchainVTableHooks() {
    InlineHook::RemoveAll();

    if (!s_hookedVTable || !oPresent)
        return;

    DWORD oldProtect;

    if (s_hookedVTable[8] == (void*)DetourPresent) {
        VirtualProtect(&s_hookedVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[8] = (void*)oPresent;
        VirtualProtect(&s_hookedVTable[8], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present vtable hook");
    }

    if (oPresent1 && s_hookedVTable[22] == (void*)DetourPresent1) {
        VirtualProtect(&s_hookedVTable[22], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[22] = (void*)oPresent1;
        VirtualProtect(&s_hookedVTable[22], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present1 vtable hook");
    }

    if (oResizeBuffers && s_hookedVTable[13] == (void*)DetourResizeBuffers) {
        VirtualProtect(&s_hookedVTable[13], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[13] = (void*)oResizeBuffers;
        VirtualProtect(&s_hookedVTable[13], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed ResizeBuffers vtable hook");
    }

    if (oResizeBuffers1 && s_hookedVTable[39] == (void*)DetourResizeBuffers1) {
        VirtualProtect(&s_hookedVTable[39], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[39] = (void*)oResizeBuffers1;
        VirtualProtect(&s_hookedVTable[39], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed ResizeBuffers1 vtable hook");
    }

    s_hookedVTable = nullptr;
    HookLog("DXGIShared: All swapchain vtable hooks removed");
}

HRESULT CallOriginalPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!pSwapChain) {
        return DXGI_ERROR_INVALID_CALL;
    }

    // Inline-hook path: trampoline always bypasses the detour safely.
    if (oPresentTrampoline) {
        return oPresentTrampoline(pSwapChain, SyncInterval, Flags);
    }

    // Prefer the current object's vtable entry when it is not detoured.
    // This avoids mixing wrapper and real swapchain original function pointers.
    if (IsReadableMemory(pSwapChain, sizeof(void*))) {
        void** vtable = *(void***)pSwapChain;
        if (vtable && IsReadableMemory(vtable, 9 * sizeof(void*)) && vtable[8]) {
            auto currentPresent = reinterpret_cast<PFN_Present>(vtable[8]);
            if (currentPresent != DetourPresent) {
                return currentPresent(pSwapChain, SyncInterval, Flags);
            }
        }
    }

    // Vtable-hook path fallback: use saved original only if it is not detoured.
    if (oPresent && oPresent != DetourPresent) {
        return oPresent(pSwapChain, SyncInterval, Flags);
    }

    return DXGI_ERROR_INVALID_CALL;
}

HRESULT CallOriginalPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                             const DXGI_PRESENT_PARAMETERS* pParams) {
    if (!pSwapChain) {
        return DXGI_ERROR_INVALID_CALL;
    }

    // Inline-hook path: trampoline always bypasses the detour safely.
    if (oPresent1Trampoline) {
        return oPresent1Trampoline(pSwapChain, SyncInterval, Flags, pParams);
    }

    // Prefer the current object's Present1 slot when it is not detoured.
    if (IsReadableMemory(pSwapChain, sizeof(void*))) {
        void** vtable = *(void***)pSwapChain;
        if (vtable && IsReadableMemory(vtable, 23 * sizeof(void*)) && vtable[22]) {
            auto currentPresent1 = reinterpret_cast<PFN_Present1>(vtable[22]);
            if (currentPresent1 != DetourPresent1) {
                return currentPresent1(pSwapChain, SyncInterval, Flags, pParams);
            }
        }
    }

    // Vtable-hook path fallback: use saved original only if it is not detoured.
    if (oPresent1 && oPresent1 != DetourPresent1) {
        return oPresent1(pSwapChain, SyncInterval, Flags, pParams);
    }

    // Last resort: fall back to Present.
    return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
}

}  // namespace DXGIShared
