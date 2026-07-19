/**
 * DXGI Swapchain Wrapper Implementation
 *
 * Core implementation for safe overlay drawing with FG runtimes.
 */

#include "dxgi_swapchain_wrap.h"
#include <d3d10.h>
#include <d3d11.h>
#include <windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include "../../common/raii_helpers.h"
#include "../apis/graphics_hook.h"
#include "../common/dx12_overlay_policy.h"
#include "../common/dxgi_shared.h"
#include "../common/overlay_compat.h"
#include "../common/perf_logger.h"
#include "../common/performance_metrics.h"
#include "hook_common.h"

// External overlay functions (implemented in dx11_hook.cpp / dx12_hook.cpp)
extern void DrawDX11Overlay(IDXGISwapChain* pSwapChain);
extern void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain);
extern void DX11_ProcessFrameExternal(IDXGISwapChain* pSwapChain);
extern void DX12_OnSwapchainResizeBegin();
extern void DX12_OnSwapchainResizeEnd();
extern bool DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();
extern "C" __declspec(dllimport) void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue);
extern "C" __declspec(dllimport) void DX12_WaitForOverlayCompletion(ID3D12CommandQueue* pQueue);
extern "C" __declspec(dllimport) bool DX12_FlushDeferredSignalWithInfo(
    ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo* outInfo);
extern "C" __declspec(dllimport) bool DX12_WaitForFocusLossOverlayFenceAfterPresent(
    const ce::dx12_overlay_policy::D3D12FocusLossOverlayFenceWaitContext* context,
    const ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo* flushInfo);
extern "C" __declspec(dllimport) void DX12_SetWrappedPresentFocusLossContext(const char* presentName, int callCount,
                                                                             UINT syncInterval, UINT presentFlags);
extern "C" __declspec(dllimport) void DX12_ClearWrappedPresentFocusLossContext();
extern "C" __declspec(dllimport) void DX12_NoteWrappedD3D12PresentResult(const char* presentName, int callCount,
                                                                         UINT syncInterval, UINT presentFlags,
                                                                         HRESULT presentHr, BOOL isFullscreen,
                                                                         BOOL isIconic, BOOL hasZeroSize,
                                                                         HWND gameWindow);

// Query-based CPU prerender limit for D3D11 (implemented in dx11_hook.cpp)
extern void ApplyPrerenderLimit(IDXGISwapChain* pSwapChain, float limit);

// FG detection for FSR FG/DLSS FG compatibility
#include "../common/fg_detection.h"
#include "../common/fps_limiter.h"
#include "../common/freeze_watchdog.h"

// Thread-local VEH guard for safely calling DXGI methods that might AV
// during shutdown (the swapchain's internal hash table may have been freed).
class ScopedAvGuard {
    PVOID handle_;
    static LONG CALLBACK Handler(PEXCEPTION_POINTERS ep) {
        if (ep->ExceptionRecord->ExceptionCode == STATUS_ACCESS_VIOLATION) {
            // Skip the crashing instruction (5 bytes for MOV with SIB+disp8)
            // and set rax/Rax=0 so the caller sees "entry not found" and handles
            // it gracefully (creates a new entry or returns error).
#ifdef _WIN64
            ep->ContextRecord->Rax = 0;
            ep->ContextRecord->Rip += 5;
#else
            ep->ContextRecord->Eax = DXGI_ERROR_DEVICE_REMOVED;
            ep->ContextRecord->Eip += 5;
#endif
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

public:
    ScopedAvGuard() {
        handle_ = AddVectoredExceptionHandler(1, Handler);
    }
    ~ScopedAvGuard() {
        RemoveVectoredExceptionHandler(handle_);
    }
};

#ifndef BUILDING_CAPTURE_HOOK
// Dynamically import from capture_hook to update shared state across DLL
// boundaries (for d3d12_wrappers.dll)
typedef void (*PFN_AdjustDepth)(int);
static PFN_AdjustDepth pAdjustDepth = nullptr;
static std::once_flag pAdjustDepthInitOnce;

void DX12_AdjustWrapperResizeDepth(int delta) {
    std::call_once(pAdjustDepthInitOnce, []() {
        HMODULE hHook = GetModuleHandleA("capture_hook_x64.dll");
        if (!hHook)
            hHook = GetModuleHandleA("capture_hook_x86.dll");
        if (hHook) {
            pAdjustDepth = (PFN_AdjustDepth)GetProcAddress(hHook, "DX12_AdjustWrapperResizeDepth_C");
        }
    });
    if (pAdjustDepth)
        pAdjustDepth(delta);
}
#else
// Internal build: Symbol provided by dx12_hook.cpp
extern void DX12_AdjustWrapperResizeDepth(int delta);
#endif

// RAII Guard for Resize Scope
struct ScopedResizeGuard {
    ScopedResizeGuard() {
        DX12_AdjustWrapperResizeDepth(1);
    }
    ~ScopedResizeGuard() {
        DX12_AdjustWrapperResizeDepth(-1);
    }
};
#include <cstdint>
extern void DX11_UpdatePerformanceMetrics(int64_t qpcUs);

// Shutdown safety flag to prevent accessing freed memory during process exit
static std::atomic<bool> g_WrapperShutdown{false};

static bool g_OverlayEnabled = true;

// Thread-local flag to track when we're inside the wrapper's Present
// This prevents vtable hooks from also processing the frame
thread_local bool g_InWrapperPresent = false;

// Function to check if we're in wrapper Present (same DLL, no export needed)
bool IsInWrapperPresent() {
    return g_InWrapperPresent;
}

static bool ShouldYieldToVulkanLayer() {
    SharedMemoryLayout* shm = nullptr;
    if (g_IPC && g_IPC->GetSharedMem()) {
        shm = g_IPC->GetSharedMem();
    } else {
        shm = g_pSharedMem;
    }
    if (!shm) {
        return false;
    }

    const uint64_t lastVulkan = shm->runtimeState.vulkanPresentTick.load(std::memory_order_acquire);
    if (lastVulkan == 0) {
        return false;
    }

    const uint64_t now = GetTickCount64();
    return shm->runtimeState.vulkanLayerActive.load(std::memory_order_acquire) && now >= lastVulkan &&
           (now - lastVulkan) < 200;
}

static const char* DetectWrappedSwapchainApi(IUnknown* pDevice, bool isD3D12) {
    if (isD3D12)
        return "DX12";
    if (!pDevice)
        return "DXGI";

    ID3D10Device* device10 = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D10Device), (void**)&device10))) {
        device10->Release();
        return "DX10";
    }

    ID3D11Device* device11 = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D11Device), (void**)&device11))) {
        device11->Release();
        return "DX11";
    }

    return "DXGI";
}

static const char* GetDX12PresentDelegationOverlayModuleName() {
    const char* overlayModule = ce::overlay_compat::GetStartupBlockingOverlayModuleName();
    if (overlayModule) {
        return overlayModule;
    }
    return ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
}

// When a third-party overlay already owns the DXGI Present chain, route the
// wrapper through our detour path instead of running a second wrapper-managed
// Present path. This avoids wrapper -> detour -> external overlay re-entry.
static bool ShouldDelegateDX12PresentToDetourHook(const char** overlayModuleOut = nullptr) {
    const char* overlayModule = GetDX12PresentDelegationOverlayModuleName();
    if (overlayModuleOut) {
        *overlayModuleOut = overlayModule;
    }
    if (!overlayModule || !DXGIShared::HasPresentDetourHooks()) {
        return false;
    }
    return !g_FGCompat.IsDLSSFGApiActive() && !g_FGCompat.IsFSRFGApiActive();
}

static bool IsD3D12PresentDeviceLostHRESULT(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG;
}

static bool ResolveCurrentProcessForeground(HWND* foregroundWindowOut, DWORD* foregroundPidOut) {
    HWND foregroundWindow = GetForegroundWindow();
    DWORD foregroundPid = 0;
    bool processHasForeground = false;
    if (foregroundWindow) {
        GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
        processHasForeground = (foregroundPid == GetCurrentProcessId());
    }
    if (foregroundWindowOut) {
        *foregroundWindowOut = foregroundWindow;
    }
    if (foregroundPidOut) {
        *foregroundPidOut = foregroundPid;
    }
    return processHasForeground;
}

static ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo FlushDeferredDX12OverlaySignalAfterWrappedPresent(
    bool isD3D12, const char* presentName, int callCount, bool focusLostForSwapchain) {
    ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo flushInfo = {};
    if (!ce::dx12_overlay_policy::ShouldFlushDeferredOverlaySignalAfterPresent(isD3D12)) {
        return flushInfo;
    }

    DX12_FlushDeferredSignalWithInfo(&flushInfo);

    if (flushInfo.hadDeferredSignal) {
        static std::atomic<int> s_flushLogCount{0};
        const int n = s_flushLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 24 || (n % 1000) == 0) {
            WrapperLog(
                "%s#%d: flushed deferred DX12 overlay fence signal after wrapped Present "
                "(signal=%d hr=0x%08X fence=%p event=%p value=%llu completed=%llu queue=%p)",
                presentName, callCount, flushInfo.signalSucceeded ? 1 : 0, (unsigned)flushInfo.signalHr,
                flushInfo.fence, flushInfo.fenceEvent, (unsigned long long)flushInfo.fenceValue,
                (unsigned long long)flushInfo.completedValue, flushInfo.queue);
        }
    } else if (focusLostForSwapchain) {
        static std::atomic<int> s_noDeferredFocusLossLogCount{0};
        const int n = s_noDeferredFocusLossLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 40 || (n % 300) == 0) {
            WrapperLog(
                "%s#%d: no deferred DX12 overlay fence signal after wrapped Present "
                "(focus-loss; expected when background backbuffer work was held or same-frame immediate-fence path "
                "already waited before Present, fence=%p event=%p completed=%llu)",
                presentName, callCount, flushInfo.fence, flushInfo.fenceEvent,
                (unsigned long long)flushInfo.completedValue);
        }
    }
    return flushInfo;
}

static void LogD3D12PresentDeviceLostHRESULT(bool isD3D12, const char* presentName, int callCount, HRESULT hr) {
    if (!isD3D12 || !IsD3D12PresentDeviceLostHRESULT(hr)) {
        return;
    }

    DXGIShared::g_SharedState.deviceRemovedFatal.store(true, std::memory_order_release);
    static std::atomic<int> s_deviceLostLogCount{0};
    const int n = s_deviceLostLogCount.fetch_add(1, std::memory_order_relaxed);
    if (n < 20) {
        WrapperLog("%s#%d: D3D12 Present returned device-lost hr=0x%08X", presentName, callCount, (unsigned)hr);
    }
}

static const char* WaitResultName(DWORD waitResult) {
    switch (waitResult) {
        case WAIT_OBJECT_0:
            return "signaled";
        case WAIT_TIMEOUT:
            return "timeout";
        case WAIT_ABANDONED:
            return "abandoned";
        case WAIT_FAILED:
            return "failed";
        default:
            return "unknown";
    }
}

// FSR Frame Generation detection helpers
static bool IsFSRFrameGenerationActive() {
    static HMODULE fsrFgDll = nullptr;
    static std::once_flag fsrFgCheckOnce;
    std::call_once(fsrFgCheckOnce, []() {
        fsrFgDll = GetModuleHandleW(L"amd_fidelityfx_fg.dll");
        if (!fsrFgDll)
            fsrFgDll = GetModuleHandleW(L"ffx_fsr3upscaler_x64.dll");
        if (!fsrFgDll)
            fsrFgDll = GetModuleHandleW(L"ffx_frameinterpolation_x64.dll");
    });
    return fsrFgDll != nullptr;
}

// Check if this swapchain is likely an FSR internal swapchain
bool CWrapDXGISwapChain::IsFSRInternalSwapchain() {
    // If FSR FG is not active, this can't be an FSR internal swapchain
    if (!IsFSRFrameGenerationActive())
        return false;

    // CRITICAL FIX: Enhanced FSR internal swapchain detection
    // FSR creates internal swapchains for frame generation that we should not
    // intercept

    // 1. Check for null window handle (primary indicator)
    if (!m_hWnd || m_hWnd == nullptr) {
        WrapperLog("Swapchain %p has no window handle, possible FSR internal", this);
        return true;
    }

    // 2. Check for very small dimensions (FSR internal swapchains often have
    // small intermediate buffers) FSR 3 uses 1/3 resolution buffers for upscaling
    if (m_State.width > 0 && m_State.height > 0) {
        // Check if dimensions suggest an internal buffer (not a main display
        // resolution) Common FSR internal resolutions are typically not standard
        // display sizes
        bool isStandardResolution =
            (m_State.width == 1920 && m_State.height == 1080) || (m_State.width == 2560 && m_State.height == 1440) ||
            (m_State.width == 3840 && m_State.height == 2160) || (m_State.width == 2560 && m_State.height == 1080) ||
            (m_State.width == 3440 && m_State.height == 1440) || (m_State.width == 1280 && m_State.height == 720);

        // Also check for common upscaling ratios from common base resolutions
        // FSR typically scales from 360p, 540p, 720p to 1080p/4K
        bool isCommonBaseResolution = (m_State.width == 640 && m_State.height == 360) ||
                                      (m_State.width == 960 && m_State.height == 540) ||
                                      (m_State.width == 1280 && m_State.height == 720);

        // If it's not a standard display resolution and not a common base, it might
        // be internal
        if (!isStandardResolution && !isCommonBaseResolution && (m_State.width < 800 || m_State.height < 600)) {
            WrapperLog("Swapchain %p has unusual dimensions %ux%u, possible FSR internal", this, m_State.width,
                       m_State.height);
            return true;
        }
    }

    // 3. FSR internal swapchains often have flip model but no actual presentation
    // (they're used for intermediate buffering)
    if (m_FlipModel.active && m_State.width == 0 && m_State.height == 0) {
        WrapperLog("Swapchain %p has flip model with zero dimensions, possible FSR internal", this);
        return true;
    }

    return false;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapDXGISwapChain::CWrapDXGISwapChain(IDXGISwapChain* pReal, IUnknown* pDevice)
    : m_pReal(pReal),
      m_pReal1(nullptr),
      m_pReal2(nullptr),
      m_pReal3(nullptr),
      m_pReal4(nullptr),
      m_pDevice(pDevice),
      m_pD3D12Queue(nullptr),
      m_RefCount(1),
      m_hWnd(nullptr),
      m_Version(0),
      m_OverlayResourcesValid(false),
      m_IsD3D12(false),
      m_Promoted(false),
      m_DestructionCookie(0) {
    WrapperLog("SwapChain: CWrapDXGISwapChain CONSTRUCTOR called (real=%p, device=%p)", pReal, pDevice);
    if (pReal) {
        pReal->AddRef();

        // FIX B: AddRef the device/queue if we store it
        if (m_pDevice) {
            m_pDevice->AddRef();
        }

        // Detect swapchain state (flip model, fullscreen, etc.)
        DetectSwapChainState();

        if (pDevice) {
            if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&m_pD3D12Queue)))) {
                m_IsD3D12 = true;
                DX12_SetCommandQueue(m_pD3D12Queue);
                // FIX: Mark overlay as ready - DX12 systems handle lazy initialization
                // internally
                m_OverlayResourcesValid.store(true, std::memory_order_release);
            }
        }

        // Register for destruction notification (DXGI 1.4+)
        RegisterDestructionCallback();

        // Store wrapper pointer on real swapchain for retrieval
        void* pThis = this;
        pReal->SetPrivateData(IID_CWrapDXGISwapChain, sizeof(void*), &pThis);
    }

    WrapperStateManager::Get().RegisterSwapchain(this, pReal);
    WrapperLog("SwapChain: Created wrapper (real=%p, isD3D12=%d, flipModel=%d)", pReal, m_IsD3D12, m_FlipModel.active);
}

void CWrapDXGISwapChain::DetectSwapChainState() {
    if (!m_pReal)
        return;

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (SUCCEEDED(m_pReal->GetDesc(&desc))) {
        m_hWnd = desc.OutputWindow;
        m_State.isFullscreen = !desc.Windowed;
        m_State.format = desc.BufferDesc.Format;
        m_State.width = desc.BufferDesc.Width;
        m_State.height = desc.BufferDesc.Height;

        // Detect flip model for FSR FG compatibility
        m_FlipModel.active =
            (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD || desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
        m_FlipModel.native = m_FlipModel.active;

        WrapperLog(
            "SwapChain: Detected state - %dx%d, FlipModel=%d, "
            "Fullscreen=%d, Format=%d",
            m_State.width, m_State.height, m_FlipModel.active, m_State.isFullscreen, m_State.format);
    }

    // Get frame latency if available (SwapChain2+)
    if (m_pReal2) {
        m_pReal2->GetMaximumFrameLatency(&m_State.frameLatency);
    }
}

void WINAPI CWrapDXGISwapChain::DestructionCallback(void* pData) {
    auto* pSwapChain = static_cast<CWrapDXGISwapChain*>(pData);
    if (pSwapChain) {
        WrapperLog("SwapChain: DestructionCallback for wrapper %p", pSwapChain);

        // CRITICAL FIX: Check if Present() is still using the swapchain
        // Wait for refs to drop to 1 (only the callback itself)
        int attempts = 0;
        while (pSwapChain->m_RealSwapchainRefs.load() > 1 && attempts < 100) {
            Sleep(1);  // Wait 1ms
            attempts++;
        }

        // CRITICAL FIX: Lock mutex before modifying swapchain pointers
        // This prevents race conditions with Present() running on another thread
        std::lock_guard<std::mutex> lock(pSwapChain->m_ResourceLock);

        // Mark that the real swapchain is being destroyed
        // This happens when FSR FG creates a new swapchain
        pSwapChain->m_SwapchainDestroyed.store(true);  // Mark as destroyed

        // CRITICAL: Null out the real swapchain pointer to prevent use-after-free
        // The wrapper will be cleaned up when its ref count reaches 0
        pSwapChain->m_pReal = nullptr;
        pSwapChain->m_pReal1 = nullptr;
        pSwapChain->m_pReal2 = nullptr;
        pSwapChain->m_pReal3 = nullptr;
        pSwapChain->m_pReal4 = nullptr;
        pSwapChain->m_pRealCached = nullptr;
        WrapperLog(
            "SwapChain: Real swapchain pointers nulled out for wrapper %p "
            "(mutex protected, waited %d ms)",
            pSwapChain, attempts);
    }
}

HRESULT CWrapDXGISwapChain::RegisterDestructionCallback() {
    if (!m_pReal)
        return E_FAIL;

    // Try to register for destruction notification (requires DXGI 1.4+ / Windows
    // 10) ID3DDestructionNotifier interface GUID:
    // {A05C8C18-92DB-4B35-944B-E3083333C2A0}
    static const GUID IID_ID3DDestructionNotifier = {
        0xa05c8c18, 0x92db, 0x4b35, {0x94, 0x4b, 0xe3, 0x08, 0x33, 0x33, 0xc2, 0xa0}};

    struct ID3DDestructionNotifier : public IUnknown {
        virtual HRESULT RegisterDestructionCallback(void* pCallbackFn, void* pData, UINT* pCookie) = 0;
        virtual HRESULT UnregisterDestructionCallback(UINT Cookie) = 0;
    };

    ID3DDestructionNotifier* pNotifier = nullptr;
    if (SUCCEEDED(m_pReal->QueryInterface(IID_ID3DDestructionNotifier, (void**)&pNotifier))) {
        HRESULT hr = pNotifier->RegisterDestructionCallback((void*)DestructionCallback, this, &m_DestructionCookie);
        pNotifier->Release();
        if (SUCCEEDED(hr)) {
            WrapperLog("SwapChain: Registered destruction callback (cookie=%u)", m_DestructionCookie);
        }
        return hr;
    }
    return E_NOTIMPL;
}

void CWrapDXGISwapChain::EnsurePromoted() {
    if (m_Promoted || !m_pReal)
        return;
    PromoteInterfaces();
    m_Promoted = true;
}

CWrapDXGISwapChain::CWrapDXGISwapChain(IDXGISwapChain1* pReal, IUnknown* pDevice)
    : CWrapDXGISwapChain(static_cast<IDXGISwapChain*>(pReal), pDevice) {
    if (!m_pReal1 && pReal) {
        m_pReal1 = pReal;
        m_pReal1->AddRef();
        m_Version = 1;
    }
}

CWrapDXGISwapChain::~CWrapDXGISwapChain() {
    // SAFETY: Check if destructor has already run (prevents double-free during
    // shutdown)
    if (m_DestructorCalled.exchange(true, std::memory_order_acq_rel)) {
        // Destructor already ran, don't access any members
        WrapperLog(
            "SwapChain: Destructor called again on already-destroyed "
            "wrapper %p - skipping",
            this);
        return;
    }

    // SAFETY: Check global shutdown flag - during shutdown we skip cleanup to
    // avoid crashes
    if (g_WrapperShutdown.load(std::memory_order_acquire)) {
        WrapperLog(
            "SwapChain: Destructor called during shutdown on wrapper %p - "
            "skipping cleanup",
            this);
        return;
    }

    const bool wrapperReleasing = m_Releasing.load(std::memory_order_acquire);
    WrapperLog("SwapChain: Destroying wrapper %p (real=%p releasing=%d cookie=%u)", this, m_pReal,
               wrapperReleasing ? 1 : 0, m_DestructionCookie);

    // Unregister destruction callback if registered
    if (ce::dx12_overlay_policy::ShouldUnregisterSwapchainDestructionCallbackDuringWrapperDestructor(
            wrapperReleasing, m_pReal != nullptr, m_DestructionCookie != 0)) {
        WrapperLog("SwapChain: Unregistering destruction callback (wrapper=%p cookie=%u)", this, m_DestructionCookie);
        static const GUID IID_ID3DDestructionNotifier = {
            0xa05c8c18, 0x92db, 0x4b35, {0x94, 0x4b, 0xe3, 0x08, 0x33, 0x33, 0xc2, 0xa0}};
        struct ID3DDestructionNotifier : public IUnknown {
            virtual HRESULT RegisterDestructionCallback(void* pCallbackFn, void* pData, UINT* pCookie) = 0;
            virtual HRESULT UnregisterDestructionCallback(UINT Cookie) = 0;
        };
        ID3DDestructionNotifier* pNotifier = nullptr;
        if (SUCCEEDED(m_pReal->QueryInterface(IID_ID3DDestructionNotifier, (void**)&pNotifier))) {
            pNotifier->UnregisterDestructionCallback(m_DestructionCookie);
            pNotifier->Release();
        }
    } else if (m_DestructionCookie != 0 && m_pReal) {
        WrapperLog(
            "SwapChain: Skipping destruction callback unregister during releasing destruction "
            "(wrapper=%p real=%p cookie=%u)",
            this, m_pReal, m_DestructionCookie);
    }

    WrapperLog("SwapChain: Unregistering wrapper state (wrapper=%p)", this);
    WrapperStateManager::Get().UnregisterSwapchain(this);
    CleanupOverlayResources();
    if (m_pD3D12Queue) {
        WrapperLog("SwapChain: Releasing stored D3D12 queue (wrapper=%p queue=%p)", this, m_pD3D12Queue);
        m_pD3D12Queue->Release();
    }
    // CRITICAL FIX: Null out all real swapchain pointers BEFORE releasing them.
    // If another thread calls forwarding methods on this wrapper (e.g.
    // SetPrivateData) during destruction, m_pReal==nullptr is caught by guards
    // instead of accessing already-freed memory.
    IDXGISwapChain* pRealToFree = m_pReal ? m_pReal : m_pRealCached;
    m_pReal = nullptr;
    m_pRealCached = nullptr;
    // Save interface pointers for release after nulling
    IDXGISwapChain1* pReal1ToFree = m_pReal1;
    IDXGISwapChain2* pReal2ToFree = m_pReal2;
    IDXGISwapChain3* pReal3ToFree = m_pReal3;
    IDXGISwapChain4* pReal4ToFree = m_pReal4;
    m_pReal1 = nullptr;
    m_pReal2 = nullptr;
    m_pReal3 = nullptr;
    m_pReal4 = nullptr;
    // Release interface references (nulled above, so no thread can see them)
    if (pReal4ToFree) {
        WrapperLog("SwapChain: Releasing promoted IDXGISwapChain4 (wrapper=%p real4=%p)", this, pReal4ToFree);
        pReal4ToFree->Release();
    }
    if (pReal3ToFree) {
        WrapperLog("SwapChain: Releasing promoted IDXGISwapChain3 (wrapper=%p real3=%p)", this, pReal3ToFree);
        pReal3ToFree->Release();
    }
    if (pReal2ToFree) {
        WrapperLog("SwapChain: Releasing promoted IDXGISwapChain2 (wrapper=%p real2=%p)", this, pReal2ToFree);
        pReal2ToFree->Release();
    }
    if (pReal1ToFree) {
        WrapperLog("SwapChain: Releasing promoted IDXGISwapChain1 (wrapper=%p real1=%p)", this, pReal1ToFree);
        pReal1ToFree->Release();
    }
    // Remove wrapper↔real mapping and release final reference
    if (pRealToFree) {
        if (ce::dx12_overlay_policy::ShouldClearSwapchainWrapperPrivateDataDuringWrapperDestructor(wrapperReleasing,
                                                                                                   true)) {
            WrapperLog("SwapChain: Clearing wrapper private-data marker (wrapper=%p real=%p)", this, pRealToFree);
            ScopedAvGuard guard;
            pRealToFree->SetPrivateData(IID_CWrapDXGISwapChain, 0, nullptr);
        } else {
            WrapperLog(
                "SwapChain: Skipping private-data clear during releasing destruction "
                "(wrapper=%p real=%p)",
                this, pRealToFree);
        }
        WrapperLog("SwapChain: Releasing real swapchain final wrapper reference (wrapper=%p real=%p)", this,
                   pRealToFree);
        pRealToFree->Release();
    }
    // CRITICAL FIX: Always release device reference, even if swapchain was
    // destroyed
    if (m_pDevice) {
        WrapperLog("SwapChain: Releasing stored device (wrapper=%p device=%p)", this, m_pDevice);
        m_pDevice->Release();
    }
}

void CWrapDXGISwapChain::PromoteInterfaces() {
    if (!m_pReal)
        return;
    try {
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1))))
            m_Version = 1;
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2))))
            m_Version = 2;
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3))))
            m_Version = 3;
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4))))
            m_Version = 4;
    } catch (...) {}
}

void CWrapDXGISwapChain::CleanupOverlayResources() {
    // Atomic update - no mutex needed for simple flag
    m_OverlayResourcesValid.store(false, std::memory_order_release);
}

void CWrapDXGISwapChain::WaitFrameLatency() {
    const auto& gfx = GetActiveGraphicsConfig();
    if (!HasBackbufferCountOverride(gfx.backbufferCount))
        return;

    HANDLE waitable = EnsureFrameLatencyWaitable("backbuffer pacing");
    if (waitable && waitable != INVALID_HANDLE_VALUE) {
        DWORD waitResult = WaitForSingleObject(waitable, INFINITE);
        if (waitResult != WAIT_OBJECT_0) {
            static std::atomic<int> s_waitFailLogCount{0};
            if (s_waitFailLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                WrapperLog("WaitFrameLatency: wait failed result=%lu error=%lu", waitResult, GetLastError());
            }
        }
    }
}

HANDLE CWrapDXGISwapChain::EnsureFrameLatencyWaitable(const char* reason) {
    if (m_FrameLatencyWaitableQueried) {
        return m_hFrameLatencyWaitable;
    }

    EnsurePromoted();
    m_FrameLatencyWaitableQueried = true;

    if (!m_pReal2) {
        static std::atomic<int> s_noSwapchain2Log{0};
        const int n = s_noSwapchain2Log.fetch_add(1, std::memory_order_relaxed);
        if (n < 10) {
            WrapperLog("Frame latency waitable unavailable for %s: IDXGISwapChain2 not available",
                       reason ? reason : "unknown");
        }
        m_hFrameLatencyWaitable = nullptr;
        return m_hFrameLatencyWaitable;
    }

    m_hFrameLatencyWaitable = m_pReal2->GetFrameLatencyWaitableObject();
    if (m_hFrameLatencyWaitable && m_hFrameLatencyWaitable != INVALID_HANDLE_VALUE) {
        static std::atomic<int> s_waitableLog{0};
        const int n = s_waitableLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 20) {
            WrapperLog("Frame latency waitable obtained for %s (waitable=%p)", reason ? reason : "unknown",
                       m_hFrameLatencyWaitable);
        }
        return m_hFrameLatencyWaitable;
    }

    static std::atomic<int> s_invalidWaitableLog{0};
    const int n = s_invalidWaitableLog.fetch_add(1, std::memory_order_relaxed);
    if (n < 20) {
        WrapperLog("Frame latency waitable unavailable for %s: GetFrameLatencyWaitableObject returned %p",
                   reason ? reason : "unknown", m_hFrameLatencyWaitable);
    }
    m_hFrameLatencyWaitable = nullptr;
    return m_hFrameLatencyWaitable;
}

void CWrapDXGISwapChain::WaitD3D12FocusLossOverlayFenceAfterPresent(
    const char* presentName, int callCount, UINT syncInterval, UINT presentFlags, HRESULT presentHr,
    const ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo& flushInfo) {
    if (!m_IsD3D12) {
        return;
    }

    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    const bool processHasForeground = ResolveCurrentProcessForeground(&foregroundWindow, &foregroundPid);
    ce::dx12_overlay_policy::D3D12FocusLossOverlayFenceWaitContext context = {};
    context.presentName = presentName;
    context.callCount = callCount;
    context.isD3D12Swapchain = m_IsD3D12;
    context.isFullscreen = m_State.isFullscreen;
    context.processHasForeground = processHasForeground;
    context.isIconic = (m_hWnd != nullptr) && IsIconic(m_hWnd);
    context.hasZeroSize = (m_State.width == 0 || m_State.height == 0);
    context.presentSucceeded = SUCCEEDED(presentHr);
    context.presentDeviceLost = IsD3D12PresentDeviceLostHRESULT(presentHr);
    context.frameGenerationActive = g_FGCompat.IsFGActive();
    context.runtimeOwnedPresentation =
        DXGIShared::DoesFGRuntimeOwnSwapchain() || DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();
    context.usingDedicatedQueue = false;
    context.foregroundWindow = foregroundWindow;
    context.foregroundPid = foregroundPid;
    context.gameWindow = m_hWnd;
    context.processId = GetCurrentProcessId();
    context.syncInterval = syncInterval;
    context.presentFlags = presentFlags;
    context.presentHr = presentHr;
    DX12_WaitForFocusLossOverlayFenceAfterPresent(&context, &flushInfo);
}

void CWrapDXGISwapChain::ProbeD3D12FocusLossFrameLatencyAfterPresent(const char* presentName, int callCount,
                                                                     UINT syncInterval, UINT presentFlags,
                                                                     HRESULT presentHr) {
    if (!m_IsD3D12) {
        return;
    }

    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    const bool processHasForeground = ResolveCurrentProcessForeground(&foregroundWindow, &foregroundPid);
    const bool isIconic = (m_hWnd != nullptr) && IsIconic(m_hWnd);
    const bool hasZeroSize = (m_State.width == 0 || m_State.height == 0);
    const bool presentSucceeded = SUCCEEDED(presentHr);
    const bool frameGenerationActive = g_FGCompat.IsFGActive();
    const bool runtimeOwnedPresentation =
        DXGIShared::DoesFGRuntimeOwnSwapchain() || DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();

    const bool focusLossTelemetryCandidate = m_IsD3D12 && !m_State.isFullscreen && !processHasForeground && !isIconic &&
                                             !hasZeroSize && presentSucceeded && !frameGenerationActive &&
                                             !runtimeOwnedPresentation;
    HANDLE waitable = INVALID_HANDLE_VALUE;
    const bool hasFrameLatencyWaitable = waitable && waitable != INVALID_HANDLE_VALUE;
    const bool shouldWait = DXGIShared::ShouldWaitOnD3D12FocusLossFrameLatency(
        m_IsD3D12, m_State.isFullscreen, processHasForeground, isIconic, hasZeroSize, presentSucceeded,
        frameGenerationActive, runtimeOwnedPresentation, hasFrameLatencyWaitable);

    if (!shouldWait) {
        if (!processHasForeground) {
            static std::atomic<int> s_focusSkipLog{0};
            const int n = s_focusSkipLog.fetch_add(1, std::memory_order_relaxed);
            if (n < 20 || (n % 1000) == 0) {
                WrapperLog(
                    "%s#%d: D3D12 focus-loss frame-latency waitable telemetry probe skipped "
                    "(fg=%p/%lu ours=%p/%lu sync=%u flags=0x%08X presentHr=0x%08X fullscreen=%d iconic=%d "
                    "zeroSize=%d fgActive=%d runtimeOwned=%d waitable=%p available=%d candidate=%d "
                    "reason=present-passthrough-v7)",
                    presentName, callCount, foregroundWindow, foregroundPid, m_hWnd, GetCurrentProcessId(),
                    syncInterval, presentFlags, (unsigned)presentHr, m_State.isFullscreen ? 1 : 0, isIconic ? 1 : 0,
                    hasZeroSize ? 1 : 0, frameGenerationActive ? 1 : 0, runtimeOwnedPresentation ? 1 : 0,
                    hasFrameLatencyWaitable ? waitable : nullptr, hasFrameLatencyWaitable ? 1 : 0,
                    focusLossTelemetryCandidate ? 1 : 0);
            }
        }
        return;
    }

    constexpr DWORD kFocusLossFrameLatencyProbeMs = 0;
    DWORD waitResult = WaitForSingleObject(waitable, kFocusLossFrameLatencyProbeMs);
    DWORD waitLastError = (waitResult == WAIT_FAILED) ? GetLastError() : 0;

    static std::atomic<int> s_focusWaitLog{0};
    const int n = s_focusWaitLog.fetch_add(1, std::memory_order_relaxed);
    if (n < 50 || waitResult != WAIT_OBJECT_0 || (n % 300) == 0) {
        WrapperLog(
            "%s#%d: D3D12 focus-loss frame-latency waitable telemetry probe result=%s(0x%08lX) "
            "fg=%p/%lu ours=%p/%lu sync=%u flags=0x%08X waitable=%p available=1 timeoutMs=%lu gle=%lu",
            presentName, callCount, WaitResultName(waitResult), waitResult, foregroundWindow, foregroundPid, m_hWnd,
            GetCurrentProcessId(), syncInterval, presentFlags, waitable, kFocusLossFrameLatencyProbeMs, waitLastError);
    }
}

void CWrapDXGISwapChain::DrawOverlay() {
    static int s_DrawCount = 0;
    bool shouldLog = (++s_DrawCount <= 10);

    if (!g_OverlayEnabled) {
        if (shouldLog)
            WrapperLog("DrawOverlay: skipped (overlay disabled)");
        return;
    }
    if (shouldLog)
        WrapperLog("DrawOverlay: m_IsD3D12=%d", m_IsD3D12);
    if (m_IsD3D12) {
        // DX12: ProcessFrameExternal is now called directly from Present/Present1
        // to ensure capture works even when overlay is disabled
    } else {
        DrawDX11Overlay(m_pReal);
    }
}

// ============================================================================
// IUnknown Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_POINTER;
    *ppvObj = nullptr;
    EnsurePromoted();

    // CRITICAL FIX: Block Streamline base interface to prevent FSR FG / DLSS FG
    // from unwrapping
    if (IsEqualGUID(riid, IID_IStreamlineBaseInterface)) {
        WrapperLog(
            "SwapChain: BLOCKED Streamline interface query (FSR FG/DLSS FG "
            "unwrap attempt)");
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }

    // Allow retrieval of wrapper from real swapchain (internal use only)
    if (IsEqualGUID(riid, IID_CWrapDXGISwapChain)) {
        AddRef();
        *ppvObj = this;  // Return wrapper, NOT real swapchain
        WrapperLog(
            "SwapChain: QueryInterface for IID_CWrapDXGISwapChain - "
            "returning wrapper %p",
            this);
        return S_OK;
    }

    if (riid == IID_IUnknown || riid == IID_IDXGIObject || riid == IID_IDXGIDeviceSubObject ||
        riid == IID_IDXGISwapChain) {
        AddRef();
        *ppvObj = static_cast<IDXGISwapChain*>(this);
        return S_OK;
    }

    if (riid == IID_IDXGISwapChain1 && m_Version >= 1) {
        AddRef();
        *ppvObj = static_cast<IDXGISwapChain1*>(this);
        return S_OK;
    }
    if (riid == IID_IDXGISwapChain2 && m_Version >= 2) {
        AddRef();
        *ppvObj = static_cast<IDXGISwapChain2*>(this);
        return S_OK;
    }
    if (riid == IID_IDXGISwapChain3 && m_Version >= 3) {
        AddRef();
        *ppvObj = static_cast<IDXGISwapChain3*>(this);
        return S_OK;
    }
    if (riid == IID_IDXGISwapChain4 && m_Version >= 4) {
        AddRef();
        *ppvObj = static_cast<IDXGISwapChain4*>(this);
        return S_OK;
    }

    // Log unknown interface queries for debugging
    static std::atomic<int> s_LogCount{0};
    if (s_LogCount < 20) {
        s_LogCount++;
        LPOLESTR strIID = nullptr;
        if (SUCCEEDED(StringFromIID(riid, &strIID))) {
            WrapperLog("SwapChain: QueryInterface for unknown IID: %S", strIID);
            CoTaskMemFree(strIID);
        }
    }

    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapDXGISwapChain::AddRef() {
    // SAFETY: Check if we're in shutdown - if so, return fake reference count
    if (g_WrapperShutdown.load(std::memory_order_acquire)) {
        return 1;
    }

    // Also check if this wrapper has already been destroyed
    if (m_RefCount == 0) {
        return 1;  // Already destroyed or never initialized
    }

    ULONG refs = InterlockedIncrement(&m_RefCount);
    // Also AddRef the real swapchain to track total references
    if (m_pReal) {
        m_pReal->AddRef();
        m_RealSwapchainRefs.fetch_add(1);
    }
    return refs;
}

ULONG STDMETHODCALLTYPE CWrapDXGISwapChain::Release() {
    // SAFETY: Check if we're in shutdown - if so, just return without touching
    // anything
    if (g_WrapperShutdown.load(std::memory_order_acquire)) {
        return 0;
    }

    // Also check if this wrapper has already been destroyed
    if (m_RefCount == 0) {
        // Already destroyed or never initialized
        return 0;
    }

    ULONG xrefs = InterlockedDecrement(&m_RefCount);  // External references

    // When external refs reach 0, game expects SwapChain destruction
    if (xrefs == 0) {
        WrapperLog(
            "SwapChain: External refs reached 0, preparing for destruction "
            "(wrapper=%p)",
            this);
        // CRITICAL: Mark releasing BEFORE calling CleanupOverlayResources and
        // m_pReal->Release(). During these calls, D3D12/DXGI cleanup can trigger
        // re-entrant calls back through the wrapper (e.g. SetPrivateData via
        // Streamline interposer callbacks). IsWrapperZombie() checks this flag
        // and rejects forwarding to the already-destroyed swapchain.
        m_Releasing.store(true, std::memory_order_release);
        CleanupOverlayResources();
    }

    // Release the real swapchain (if not already nulled by DestructionCallback)
    ULONG refs = 0;
    if (m_pReal) {
        refs = m_pReal->Release();
        m_RealSwapchainRefs.fetch_sub(1);
    }

    // CRITICAL FIX: Only delete when external refs are 0
    // The real swapchain's refcount is independent - it will be released when its
    // own refcount reaches 0. We must NOT wait for refs == 0 because:
    // 1. DestructionCallback may have nulled m_pReal before we could Release()
    // 2. Waiting for refs == 0 would cause the wrapper to leak if real swapchain
    //    has external refs (e.g., from GPU, other COM clients, etc.)
    // 3. The wrapper's lifetime is controlled by external AddRef/Release on the
    // wrapper
    if (xrefs == 0) {
        WrapperLog("SwapChain: Deleting wrapper %p (real refs=%u, wrapper refs=%u)", this, refs, xrefs);
        delete this;
    }

    return xrefs;  // Return external ref count
}

IDXGISwapChain* CWrapDXGISwapChain::GetRealSafe() {
    if (m_SwapchainDestroyed.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return m_pReal;
}

// Shutdown safety function - sets the global shutdown flag
void SetSwapchainWrapperShutdown() {
    g_WrapperShutdown.store(true, std::memory_order_release);
    WrapperLog("SwapChain: Wrapper shutdown flag set");
}

// ============================================================================
// IDXGIObject Implementation
// ============================================================================

// ============================================================================
// IDXGIObject Implementation
// ============================================================================

inline bool CWrapDXGISwapChain::IsWrapperZombie() const {
    return m_Releasing.load(std::memory_order_acquire) || m_RefCount == 0 ||
           m_DestructorCalled.load(std::memory_order_acquire) || g_WrapperShutdown.load(std::memory_order_acquire);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) {
    if (!m_pReal || IsWrapperZombie()) [[unlikely]]
        return DXGI_ERROR_DEVICE_REMOVED;
    ScopedAvGuard guard;
    return m_pReal->SetPrivateData(Name, DataSize, pData);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) {
    if (!m_pReal || IsWrapperZombie()) [[unlikely]]
        return DXGI_ERROR_DEVICE_REMOVED;
    ScopedAvGuard guard;
    return m_pReal->SetPrivateDataInterface(Name, pUnknown);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) {
    if (!m_pReal || IsWrapperZombie()) [[unlikely]]
        return DXGI_ERROR_DEVICE_REMOVED;
    if (IsUnwrapAttemptGUID(Name))
        return DXGI_ERROR_NOT_FOUND;
    ScopedAvGuard guard;
    return m_pReal->GetPrivateData(Name, pDataSize, pData);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetParent(REFIID riid, void** ppParent) {
    if (!m_pReal || IsWrapperZombie()) [[unlikely]]
        return DXGI_ERROR_DEVICE_REMOVED;
    ScopedAvGuard guard;
    return m_pReal->GetParent(riid, ppParent);
}

// ============================================================================
// IDXGIDeviceSubObject Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDevice(REFIID riid, void** ppDevice) {
    return m_pReal->GetDevice(riid, ppDevice);
}

// ============================================================================
// IDXGISwapChain Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present(UINT SyncInterval, UINT Flags) {
    PresentDebugSample debugSample = {};
    PresentDebugSample* activeDebugSample = nullptr;
    int64_t presentStartUs = 0;
    FrameMetrics perfMetrics = {};
    const bool perfLoggingEnabled = !m_IsD3D12 && PerfLogger::Get().IsEnabled();
    const bool phaseTimingEnabled = perfLoggingEnabled;
    if (perfLoggingEnabled) {
        perfMetrics.qpcUs = PerfLogger::GetQpcUs();
        strncpy(perfMetrics.api, DetectWrappedSwapchainApi(m_pDevice, m_IsD3D12), sizeof(perfMetrics.api) - 1);
        perfMetrics.api[sizeof(perfMetrics.api) - 1] = '\0';
    }
    // Outermost present row: skip when the forwarded present logged an inner row (DetourPresent catch-all
    // or a per-API ProcessFrame row) so a present never writes two CSV rows (present-rate dedup).
    if (perfLoggingEnabled) {
        PerfLogger::BeginPresentRowScope();
    }
    auto perfGuard = ::ce::make_scope_guard([&] {
        if (perfLoggingEnabled && !PerfLogger::InnerRowLoggedInPresentRowScope()) {
            perfMetrics.totalUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - perfMetrics.qpcUs);
            PerfLogger::Get().LogFrame(perfMetrics);
        }
    });
    if (m_IsD3D12 && PerfLogger::Get().IsEnabled()) {
        static std::atomic<uint64_t> s_presentDebugFrame{0};
        uint64_t debugFrameNum = s_presentDebugFrame.fetch_add(1, std::memory_order_relaxed) + 1;
        if (PerfLogger::Get().ShouldSampleDetailedFrame(debugFrameNum)) {
            activeDebugSample = &debugSample;
            activeDebugSample->frameNum = debugFrameNum;
            strncpy(activeDebugSample->api, "DX12", sizeof(activeDebugSample->api) - 1);
            activeDebugSample->api[sizeof(activeDebugSample->api) - 1] = '\0';
            presentStartUs = PerfLogger::GetQpcUs();
            PerfLogger::Get().ActivateDebugSample(activeDebugSample);
        }
    }
    auto debugSampleGuard = ::ce::make_scope_guard([&] {
        if (activeDebugSample) {
            activeDebugSample->wrapperTotalUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - presentStartUs);
            PerfLogger::Get().DeactivateDebugSample(activeDebugSample);
            PerfLogger::Get().CommitDebugSample(*activeDebugSample);
        }
    });

    DXGIShared::g_SharedState.presentInFlightDepth.fetch_add(1, std::memory_order_acq_rel);
    auto presentInFlightGuard = ::ce::make_scope_guard(
        []() { DXGIShared::g_SharedState.presentInFlightDepth.fetch_sub(1, std::memory_order_acq_rel); });

    // EXTREME DEBUG: Log entry with full state
    DWORD threadId = GetCurrentThreadId();
    static std::atomic<int> s_presentCallCount{0};
    int callCount = s_presentCallCount.fetch_add(1);

    SharedMemoryLayout* debugSharedMem = (g_IPC && g_IPC->GetSharedMem()) ? g_IPC->GetSharedMem() : g_pSharedMem;
    if (perfLoggingEnabled && debugSharedMem) {
        perfMetrics.sourceFrameIndex = DXGIShared::GetLatestSourceFrameIndex();
        perfMetrics.sourceCapturePhase = debugSharedMem->runtimeState.capturePhase.load(std::memory_order_relaxed);
        perfMetrics.sourceEncoderQueueDepth = debugSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
        perfMetrics.sourceMuxQueueKb =
            (debugSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed) + 1023u) / 1024u;
        perfMetrics.sourceOverloadFlags =
            debugSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
    }
    if (perfLoggingEnabled) {
        if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
            perfMetrics.sourceCurrentFpsTimes100 = static_cast<int32_t>(perf->GetCurrentFPS() * 100.0f + 0.5f);
            perfMetrics.source1PctLowTimes100 = static_cast<int32_t>(perf->Get1PercentLowFPS() * 100.0f + 0.5f);
            perfMetrics.sourcePoint1PctLowTimes100 = static_cast<int32_t>(perf->Get01PercentLowFPS() * 100.0f + 0.5f);
            perfMetrics.sourceFrameTimeStdDevUs = static_cast<int32_t>(perf->GetWindowStdDev() + 0.5);
        }
    }
    if (activeDebugSample && debugSharedMem) {
        activeDebugSample->sourceFrameIndex = DXGIShared::GetLatestSourceFrameIndex();
        activeDebugSample->capturePhase = debugSharedMem->runtimeState.capturePhase.load(std::memory_order_relaxed);
        activeDebugSample->encoderQueueDepth = debugSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
        activeDebugSample->muxQueueKb =
            (debugSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed) + 1023u) / 1024u;
        activeDebugSample->overloadFlags =
            debugSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
    }

    // CRITICAL FIX: Lock mutex to protect swapchain pointer access
    // This prevents race conditions with DestructionCallback running on another
    // thread
    IDXGISwapChain* pRealCached = nullptr;
    const int64_t swapchainAcquireStartUs = phaseTimingEnabled ? PerfLogger::GetQpcUs() : 0;
    {
        std::lock_guard<std::mutex> lock(m_ResourceLock);

        // CRITICAL FIX: Cache the pointer while holding the mutex
        // This ensures we have a valid pointer even if DestructionCallback nulls
        // m_pReal
        pRealCached = m_pReal;
        m_pRealCached = pRealCached;  // Store for potential future use

        // CRITICAL FIX: AddRef to keep the swapchain alive while we're using it
        // This prevents use-after-free if DestructionCallback runs after we release
        // the mutex
        if (pRealCached) {
            pRealCached->AddRef();
        }
    }
    if (activeDebugSample) {
        activeDebugSample->swapchainAcquireUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - swapchainAcquireStartUs);
    }
    // CRITICAL FIX: RAII guard to ensure Release is always called
    auto realSwapchainGuard = ::ce::make_scope_guard([&] {
        if (pRealCached) {
            pRealCached->Release();
        }
    });

    if (callCount < 10) {
        WrapperLog("Present ENTRY #%d - Thread=%lu, m_pReal=%p, m_IsD3D12=%d", callCount, threadId, pRealCached,
                   m_IsD3D12);
        WrapperLog("Present state - fgActive=%d, flipModel=%d", g_FGCompat.IsFGActive(), m_FlipModel.active);
    }

    // CRITICAL: Check for global shutdown - if app is closing, don't touch
    // anything
    if (HookIsShuttingDown()) {
        if (pRealCached) {
            return pRealCached->Present(SyncInterval, Flags);
        }
        return DXGI_ERROR_INVALID_CALL;
    }

    // CRITICAL FIX: Check if real swapchain has been destroyed (e.g., by FSR FG
    // recreation) If so, just pass through to avoid crashes with stale pointer
    // NOTE: We check m_SwapchainDestroyed but we still have a valid ref on
    // pRealCached thanks to AddRef, so it's safe to use
    if (!pRealCached || m_SwapchainDestroyed.load()) {
        // Real swapchain was destroyed, wrapper is now invalid
        // This happens when FSR FG creates a new swapchain
        WrapperLog("Present: Real swapchain destroyed (FSR FG swapchain recreation)");
        if (pRealCached) {
            // Safe to use because we AddRef'd it
            return pRealCached->Present(SyncInterval, Flags);
        }
        WrapperLog("Present: pRealCached is null, returning error");
        return DXGI_ERROR_INVALID_CALL;
    }

    // CRITICAL: Heartbeat FIRST - before ANY checks that might early-return
    // This ensures the freeze watchdog gets heartbeats even with FSR/DLSS FG
    // active.  BUT skip heartbeat after device removal so the watchdog can fire.
    if (!DXGIShared::g_SharedState.deviceRemovedFatal.load(std::memory_order_relaxed))
        g_RenderWatchdog.HeartbeatFromHelperThread();

    // FSR FG FIX: Skip overlay processing on FSR internal swapchains
    // FSR creates internal swapchains for frame generation that we should not
    // interfere with
    if (IsFSRInternalSwapchain()) {
        if (callCount < 10) {
            WrapperLog("Present: Skipping FSR internal swapchain processing");
        }
        HRESULT hr = pRealCached->Present(SyncInterval, Flags);
        return hr;
    }

    // NVIDIA Smooth Motion compatibility: skip overlay/processing for invisible
    // windows (NvPresent64 may create them for DX12 frame interpolation too)
    if (g_FGCompat.IsNvPresentLoaded() && m_hWnd && !IsWindowVisible(m_hWnd)) {
        return pRealCached->Present(SyncInterval, Flags);
    }

    if (ShouldYieldToVulkanLayer()) {
        static std::atomic<int> s_vulkanYieldLog{0};
        if (s_vulkanYieldLog.fetch_add(1, std::memory_order_relaxed) < 20) {
            WrapperLog("Present: Vulkan layer is presenting, bypassing DXGI wrapper path");
        }
        return pRealCached->Present(SyncInterval, Flags);
    }

    const char* delegationOverlayModule = nullptr;
    if (m_IsD3D12 && ShouldDelegateDX12PresentToDetourHook(&delegationOverlayModule)) {
        static std::atomic<int> s_inlineRouteLogCount{0};
        if (s_inlineRouteLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
            WrapperLog("Present: Delegating DX12 Present to detour hook for external overlay %s",
                       delegationOverlayModule ? delegationOverlayModule : "module");
        }
        const bool previousInWrapperPresent = g_InWrapperPresent;
        g_InWrapperPresent = false;
        auto delegateGuard =
            ::ce::make_scope_guard([previousInWrapperPresent]() { g_InWrapperPresent = previousInWrapperPresent; });
        return pRealCached->Present(SyncInterval, Flags);
    }

    // Only advertise wrapper-managed Present after ruling out the delegated
    // external-overlay path. Otherwise DetourPresent sees a wrapper call and
    // bounces back into the original chain immediately.
    g_InWrapperPresent = true;
    auto wrapperPresentGuard = ::ce::make_scope_guard([&] { g_InWrapperPresent = false; });

    // DEBUG: Log first few Present calls to verify wrapper is being invoked
    if (callCount < 10) {
        WrapperLog("Present: call#%d (m_IsD3D12=%d, flipModel=%d, FG=%d)", callCount, m_IsD3D12, m_FlipModel.active,
                   g_FGCompat.IsFGActive());
    }

    // FSR FG/DLSS FG compatibility: don't override sync interval when FG is
    // active — it can break frame pacing. Still process capture though.
    bool fgActive = g_FGCompat.IsFGActive();

    // RECURSION GUARD: Prevent infinite recursion with Steam/other overlays
    // Using atomic+threadId instead of thread_local to avoid static destructor
    // issues
    static std::atomic<DWORD> s_presentThreadId{0};
    static std::atomic<int> s_presentDepth{0};
    DWORD currentId = GetCurrentThreadId();
    if (s_presentDepth.load() > 0 && s_presentThreadId.load() == currentId) {
        WrapperLog("Present: Recursion detected, passing through to real swapchain");
        return pRealCached->Present(SyncInterval, Flags);
    }
    s_presentThreadId.store(currentId);
    s_presentDepth.fetch_add(1);
    auto depthGuard = ::ce::make_scope_guard([&] {
        if (s_presentDepth.fetch_sub(1) == 1)
            s_presentThreadId.store(0);
    });
    DXGIShared::BeginPostSLOffKeepAlivePresentScope();
    // Span both ProcessFrame and the real Present re-entry: DLSS can report its
    // suspend edge between them, and either side may be the first safe draw.
    auto postSLOffKeepAlivePresentScopeGuard =
        ::ce::make_scope_guard([]() { DXGIShared::EndPostSLOffKeepAlivePresentScope(); });

    if (callCount < 20) {
        WrapperLog("Present: Processing call#%d", callCount);
    }

    // Update performance metrics for FPS calculation
    static int64_t qpcFreq = 0;
    const int64_t metricsUpdateStartUs = phaseTimingEnabled ? PerfLogger::GetQpcUs() : 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
    DXGIShared::GetPerformanceMetrics()->Update(us);
    if (activeDebugSample) {
        activeDebugSample->metricsUpdateUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - metricsUpdateStartUs);
    }

    // Apply VSync override from config (skip if FG is active - can break frame
    // pacing)
    if (!fgActive) {
        ProcessVSyncOverride(SyncInterval, Flags);
    } else if (callCount < 20) {
        WrapperLog("Present: Skipping VSync override because FG is active");
    }

    // Process frame for capture BEFORE calling real Present
    const int64_t processFrameStartUs = phaseTimingEnabled ? PerfLogger::GetQpcUs() : 0;
    bool dx12PresentContextArmed = false;
    auto dx12PresentContextGuard = ::ce::make_scope_guard([&] {
        if (dx12PresentContextArmed) {
            DX12_ClearWrappedPresentFocusLossContext();
        }
    });
    if (m_IsD3D12) {
        DX12_SetWrappedPresentFocusLossContext("Present", callCount, SyncInterval, Flags);
        dx12PresentContextArmed = true;
    }
    if (m_IsD3D12) {
        DX12_ProcessFrameExternal(pRealCached);
    } else {
        // DX11/DX10: DX11_ProcessFrameExternal handles both capture AND overlay
        DX11_ProcessFrameExternal(pRealCached);
        if (perfLoggingEnabled) {
            perfMetrics.captureUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - processFrameStartUs);
        }
    }
    if (activeDebugSample) {
        activeDebugSample->processFrameExternalUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - processFrameStartUs);
    }

    // FPS Limiter - arm frame pacing before present. Explicit CE-owned Reflex
    // cadence is finished after Present returns so the wait happens before the
    // game starts building the next frame.
    // This applies to both DX11 and DX12.
    const int64_t limiterStartUs = phaseTimingEnabled ? PerfLogger::GetQpcUs() : 0;
    if (g_IPC) {
        g_SharedFpsLimiter.SetIPCClient(g_IPC);
        g_SharedFpsLimiter.Apply(true);
        DXGIShared::ApplyPresentFrameLatencyOverrides(pRealCached);
    }

    if (activeDebugSample) {
        activeDebugSample->fpsLimiterUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - limiterStartUs);
    }
    if (perfLoggingEnabled) {
        perfMetrics.fpsLimitWaitUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - limiterStartUs);
    }

    // When the FPS limiter is active, override SyncInterval to 0 for precise frame pacing.
    // SyncInterval=0 on FLIP model means "present at next vblank, non-blocking if queue not full."
    // This is tear-free (frames still sync to vblank) but avoids the vsync blocking that
    // would absorb the limiter's delay. The present queue drains at display refresh rate;
    // since our limiter targets <= display rate, the queue never saturates and Present
    // returns immediately, letting the limiter control the actual frame cadence.
    // We do NOT use DXGI_PRESENT_ALLOW_TEARING — it bypasses vblank sync entirely and
    // causes visible tearing with DirectFlip (even in windowed/borderless mode).
    if (g_SharedFpsLimiter.IsActivelyLimiting() && !m_State.isFullscreen) {
        static int s_syncLog = 0;
        if (s_syncLog++ < 30) {
            WrapperLog("Present: Limiter active, SyncInterval %u->0 (vblank-synced, tear-free)", SyncInterval);
        }
        SyncInterval = 0;
    }

    // When non-DX12 flip-model apps are not in the foreground, the GPU can be
    // throttled by the driver. Present() with SyncInterval>0 may block waiting
    // for the flip queue to drain, so DX11/DX10 can use DO_NOT_WAIT. D3D12 is
    // different: the app often keeps building command lists while unfocused, and
    // forcing DO_NOT_WAIT can create an unbounded ECL/Present loop that hangs
    // the device. Preserve D3D12 Present pacing and let the overlay visually
    // stall instead of disappearing or destabilizing the queue.
    UINT presentFlags = Flags;
    if (m_hWnd && !m_State.isFullscreen) {
        HWND foreground = GetForegroundWindow();
        if (foreground != m_hWnd) {
            static std::atomic<int> s_focusLossLog{0};
            int n = s_focusLossLog.fetch_add(1, std::memory_order_relaxed);
            const bool applyDoNotWait = DXGIShared::ShouldApplyUnfocusedFlipModelDoNotWait(
                m_IsD3D12, m_State.isFullscreen, false, presentFlags);
            if (n == 0 || n % 300 == 0) {
                WrapperLog("Present#%d: Not foreground (fg=%p vs ours=%p), %s", callCount, foreground, m_hWnd,
                           applyDoNotWait ? "SyncInterval ->0 + DO_NOT_WAIT (non-DX12 GPU throttle protection)"
                                          : "preserving Present pacing (D3D12 focus-loss safety)");
            }
            if (applyDoNotWait) {
                SyncInterval = 0;
                presentFlags |= 0x00000008U;  // DO_NOT_WAIT
            }
        }
    }

    if (m_IsD3D12) {
        DX12_SetWrappedPresentFocusLossContext("Present", callCount, SyncInterval, presentFlags);
        DX12_WaitForOverlayCompletion(nullptr);
    }

    const int64_t presentCallStartUs = phaseTimingEnabled ? PerfLogger::GetQpcUs() : 0;
    HRESULT hr = pRealCached->Present(SyncInterval, presentFlags);
    if (m_IsD3D12) {
        const BOOL isIconic = (m_hWnd != nullptr) ? IsIconic(m_hWnd) : FALSE;
        const BOOL hasZeroSize = (m_State.width == 0 || m_State.height == 0) ? TRUE : FALSE;
        DX12_NoteWrappedD3D12PresentResult("Present", callCount, SyncInterval, presentFlags, hr,
                                           m_State.isFullscreen ? TRUE : FALSE, isIconic, hasZeroSize, m_hWnd);
    }
    const bool flushProcessHasForeground = m_IsD3D12 ? ResolveCurrentProcessForeground(nullptr, nullptr) : true;
    const bool focusLostForFlush = m_IsD3D12 && !flushProcessHasForeground && !m_State.isFullscreen;
    const auto flushInfo =
        FlushDeferredDX12OverlaySignalAfterWrappedPresent(m_IsD3D12, "Present", callCount, focusLostForFlush);
    LogD3D12PresentDeviceLostHRESULT(m_IsD3D12, "Present", callCount, hr);
    WaitD3D12FocusLossOverlayFenceAfterPresent("Present", callCount, SyncInterval, presentFlags, hr, flushInfo);
    if (SUCCEEDED(hr)) {
        // A Present hook is the earliest boundary before the game starts the
        // next frame. Waiting here prevents simulation/render work from being
        // queued behind a full vsync present queue.
        WaitFrameLatency();
        g_SharedFpsLimiter.ApplyPostPresent();
    }
    ProbeD3D12FocusLossFrameLatencyAfterPresent("Present", callCount, SyncInterval, presentFlags, hr);
    if (activeDebugSample) {
        activeDebugSample->presentCallUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - presentCallStartUs);
    }
    if (perfLoggingEnabled) {
        perfMetrics.presentCallUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - presentCallStartUs);
    }

    // DXGI_ERROR_WAS_STILL_DRAWING: the previous flip is still pending (GPU throttled).
    // Drop this frame silently — the overlay remains visible from the last rendered frame.
    if (hr == (HRESULT)0x887A000A) {  // DXGI_ERROR_WAS_STILL_DRAWING
        static std::atomic<int> s_dropLog{0};
        if (s_dropLog.fetch_add(1, std::memory_order_relaxed) < 10)
            WrapperLog("Present#%d: WAS_STILL_DRAWING — frame dropped (GPU throttled)", callCount);
        hr = S_OK;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) {
    // CRITICAL FIX: Use safe accessor to prevent races with DestructionCallback
    IDXGISwapChain* pReal = GetRealSafe();
    if (!pReal)
        return DXGI_ERROR_INVALID_CALL;
    return pReal->GetBuffer(Buffer, riid, ppSurface);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) {
    IDXGISwapChain* pReal = GetRealSafe();
    if (!pReal)
        return DXGI_ERROR_INVALID_CALL;
    HRESULT hr = pReal->SetFullscreenState(Fullscreen, pTarget);
    if (SUCCEEDED(hr)) {
        // Update cached fullscreen state immediately so ALLOW_TEARING gate in
        // Present/Present1 uses accurate state even before ResizeBuffers is called.
        m_State.isFullscreen = (Fullscreen != FALSE);
        WrapperLog("SetFullscreenState: Fullscreen=%d hr=0x%08X", Fullscreen ? 1 : 0, hr);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) {
    IDXGISwapChain* pReal = GetRealSafe();
    if (!pReal)
        return DXGI_ERROR_INVALID_CALL;
    return pReal->GetFullscreenState(pFullscreen, ppTarget);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) {
    IDXGISwapChain* pReal = GetRealSafe();
    if (!pReal)
        return DXGI_ERROR_INVALID_CALL;
    return pReal->GetDesc(pDesc);
}

static std::atomic<bool> s_ResizeInProgress{false};

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                                            DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    WrapperLog("CWrapDXGISwapChain::ResizeBuffers called - Width=%u, Height=%u", Width, Height);
    if (HasBackbufferCountOverride(GetActiveGraphicsConfig().backbufferCount))
        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    // RECURSION GUARD: Prevent infinite recursion with Steam/other overlays
    static std::atomic<DWORD> s_resizeThreadId{0};
    static std::atomic<int> s_resizeDepth{0};
    DWORD currentId = GetCurrentThreadId();
    if (s_resizeDepth.load() > 0 && s_resizeThreadId.load() == currentId) {
        return m_pReal->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    s_resizeThreadId.store(currentId);
    s_resizeDepth.fetch_add(1);

    bool expected = false;
    if (!s_ResizeInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        WrapperLog("ResizeBuffers: already in progress, forwarding to real swapchain");
        HRESULT concurrentHr = S_OK;
        {
            ScopedResizeGuard guard;
            concurrentHr = m_pReal->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
        }
        if (s_resizeDepth.fetch_sub(1) == 1) {
            s_resizeThreadId.store(0);
        }
        return concurrentHr;
    }

    // Apply backbuffer count override from config
    if (g_IPC) {
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            // Check swap effect from current swapchain desc
            DXGI_SWAP_CHAIN_DESC scDesc = {};
            bool isFlip = false;
            if (SUCCEEDED(m_pReal->GetDesc(&scDesc))) {
                isFlip = (scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                          scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            }
            UINT gameCount = BufferCount > 0 ? BufferCount : scDesc.BufferCount;
            if (isFlip && requested < gameCount) {
                WrapperLog(
                    "ResizeBuffers: Skipping BufferCount override %u < game's %u "
                    "(flip model)",
                    requested, gameCount);
            } else {
                BufferCount = requested;
                WrapperLog("ResizeBuffers: Overriding BufferCount to %u", BufferCount);
            }
        }
    }

    WrapperLog("ResizeBuffers: calling DX12_OnSwapchainResizeBegin");
    DX12_OnSwapchainResizeBegin();
    WrapperLog("ResizeBuffers: DX12_OnSwapchainResizeBegin returned");

    WrapperLog("ResizeBuffers: calling CleanupOverlayResources");
    CleanupOverlayResources();
    WrapperLog("ResizeBuffers: CleanupOverlayResources returned");

    // CRITICAL FIX: Release DX11 backbuffer RTV before ResizeBuffers.
    // CleanupOverlayResources() only sets a flag; the actual RTV holding a COM
    // reference to the backbuffer must be released or DXGI returns
    // DXGI_ERROR_INVALID_CALL (e.g. Trine 4 vsync toggle).
    if (!m_IsD3D12)
        DXGIShared::HandleDX11ResizeBegin();

    HRESULT hr = S_OK;
    {
        ScopedResizeGuard guard;
        hr = m_pReal->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    WrapperLog("ResizeBuffers: real ResizeBuffers returned hr=0x%08X", hr);

    WrapperLog("ResizeBuffers: calling DX12_OnSwapchainResizeEnd");
    DX12_OnSwapchainResizeEnd();
    if (SUCCEEDED(hr)) {
        m_OverlayResourcesValid = true;
        m_hFrameLatencyWaitable = INVALID_HANDLE_VALUE;
        m_FrameLatencyWaitableQueried = false;
        // Refresh cached state (resolution, format, fullscreen, ALLOW_TEARING)
        DetectSwapChainState();
    }
    s_ResizeInProgress.store(false, std::memory_order_release);
    if (s_resizeDepth.fetch_sub(1) == 1) {
        s_resizeThreadId.store(0);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) {
    return m_pReal->ResizeTarget(pNewTargetParameters);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetContainingOutput(IDXGIOutput** ppOutput) {
    return m_pReal->GetContainingOutput(ppOutput);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) {
    return m_pReal->GetFrameStatistics(pStats);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetLastPresentCount(UINT* pLastPresentCount) {
    return m_pReal->GetLastPresentCount(pLastPresentCount);
}

// ============================================================================
// IDXGISwapChain1 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetDesc1(pDesc);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetFullscreenDesc(pDesc);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetHwnd(HWND* pHwnd) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetHwnd(pHwnd);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetCoreWindow(REFIID refiid, void** ppUnk) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetCoreWindow(refiid, ppUnk);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present1(UINT SyncInterval, UINT PresentFlags,
                                                       const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    // CRITICAL: Check for global shutdown - if app is closing, don't touch
    // anything
    if (HookIsShuttingDown()) {
        if (m_pReal1) {
            return m_pReal1->Present1(SyncInterval, PresentFlags, pPresentParameters);
        }
        return DXGI_ERROR_INVALID_CALL;
    }

    // CRITICAL: Heartbeat FIRST - before ANY checks that might early-return
    // This ensures the freeze watchdog gets heartbeats even with FSR/DLSS FG
    // active.  BUT skip heartbeat after device removal so the watchdog can fire.
    if (!DXGIShared::g_SharedState.deviceRemovedFatal.load(std::memory_order_relaxed))
        g_RenderWatchdog.Heartbeat();

    // CRITICAL FIX: Lock mutex to protect swapchain pointer access
    std::lock_guard<std::mutex> lock(m_ResourceLock);

    // CRITICAL FIX: Cache the pointer while holding the mutex
    IDXGISwapChain1* pReal1Cached = m_pReal1;
    if (!pReal1Cached) {
        return DXGI_ERROR_INVALID_CALL;
    }
    static std::atomic<int> s_present1CallCount{0};
    int callCount = s_present1CallCount.fetch_add(1, std::memory_order_relaxed);

    // NVIDIA Smooth Motion compatibility: skip overlay for invisible windows
    if (g_FGCompat.IsNvPresentLoaded() && m_hWnd && !IsWindowVisible(m_hWnd)) {
        return pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);
    }

    if (ShouldYieldToVulkanLayer()) {
        static std::atomic<int> s_vulkanYieldLog1{0};
        if (s_vulkanYieldLog1.fetch_add(1, std::memory_order_relaxed) < 20) {
            WrapperLog("Present1: Vulkan layer is presenting, bypassing DXGI wrapper path");
        }
        return pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);
    }

    const char* delegationOverlayModule = nullptr;
    if (m_IsD3D12 && ShouldDelegateDX12PresentToDetourHook(&delegationOverlayModule)) {
        static std::atomic<int> s_inlineRouteLogCount1{0};
        if (s_inlineRouteLogCount1.fetch_add(1, std::memory_order_relaxed) < 20) {
            WrapperLog("Present1: Delegating DX12 Present1 to detour hook for external overlay %s",
                       delegationOverlayModule ? delegationOverlayModule : "module");
        }
        const bool previousInWrapperPresent = g_InWrapperPresent;
        g_InWrapperPresent = false;
        auto delegateGuard =
            ::ce::make_scope_guard([previousInWrapperPresent]() { g_InWrapperPresent = previousInWrapperPresent; });
        return pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);
    }

    g_InWrapperPresent = true;
    auto wrapperPresentGuard = ::ce::make_scope_guard([&] { g_InWrapperPresent = false; });

    // RECURSION GUARD: Prevent infinite recursion with Steam/other overlays
    static std::atomic<DWORD> s_present1ThreadId{0};
    static std::atomic<int> s_present1Depth{0};
    DWORD currentId = GetCurrentThreadId();
    if (s_present1Depth.load() > 0 && s_present1ThreadId.load() == currentId) {
        return pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);
    }
    s_present1ThreadId.store(currentId);
    s_present1Depth.fetch_add(1);
    auto depthGuard = ::ce::make_scope_guard([&] {
        if (s_present1Depth.fetch_sub(1) == 1)
            s_present1ThreadId.store(0);
    });
    DXGIShared::BeginPostSLOffKeepAlivePresentScope();
    // Present1 needs the same outer lifetime as Present for exact-proxy dedup.
    auto postSLOffKeepAlivePresentScopeGuard =
        ::ce::make_scope_guard([]() { DXGIShared::EndPostSLOffKeepAlivePresentScope(); });

    // Update performance metrics for FPS calculation
    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
    DXGIShared::GetPerformanceMetrics()->Update(us);

    // Apply VSync override from config (skip if FG is active - can break frame
    // pacing)
    if (!g_FGCompat.IsFGActive()) {
        ProcessVSyncOverride(SyncInterval, PresentFlags);
    }

    // CRITICAL: Process frame for capture BEFORE calling real Present
    // This must happen regardless of overlay state - capture works independently
    bool dx12PresentContextArmed = false;
    auto dx12PresentContextGuard = ::ce::make_scope_guard([&] {
        if (dx12PresentContextArmed) {
            DX12_ClearWrappedPresentFocusLossContext();
        }
    });
    if (m_IsD3D12) {
        DX12_SetWrappedPresentFocusLossContext("Present1", callCount, SyncInterval, PresentFlags);
        dx12PresentContextArmed = true;
    }
    if (m_IsD3D12) {
        // Use base interface for ProcessFrameExternal (it takes IDXGISwapChain*)
        DX12_ProcessFrameExternal(pReal1Cached);
        // DX12: Overlay rendering is handled by DX12_ProcessFrameExternal above
        // No additional overlay drawing needed here
    } else {
        // DX11/DX10: Call DX11_ProcessFrameExternal for capture AND overlay
        // NOTE: DX11_ProcessFrameExternal already calls DrawDX11Overlay internally,
        // so we do NOT call DrawOverlay() here to avoid double-counting frames
        DX11_ProcessFrameExternal(pReal1Cached);
    }

    // FPS Limiter - arm frame pacing before present. Explicit CE-owned Reflex
    // cadence is finished after Present returns so the wait happens before the
    // game starts building the next frame.
    // This applies to both DX11 and DX12.
    if (g_IPC) {
        g_SharedFpsLimiter.SetIPCClient(g_IPC);
        g_SharedFpsLimiter.Apply(true);
        DXGIShared::ApplyPresentFrameLatencyOverrides(pReal1Cached);
    }

    // Same SyncInterval=0 override as Present() — tear-free via vblank sync.
    if (g_SharedFpsLimiter.IsActivelyLimiting() && !m_State.isFullscreen) {
        static int s_syncLog1 = 0;
        if (s_syncLog1++ < 30) {
            WrapperLog("Present1: Limiter active, SyncInterval %u->0 (vblank-synced, tear-free)", SyncInterval);
        }
        SyncInterval = 0;
    }

    if (m_IsD3D12) {
        DX12_SetWrappedPresentFocusLossContext("Present1", callCount, SyncInterval, PresentFlags);
        DX12_WaitForOverlayCompletion(nullptr);
    }

    HRESULT hr = pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);
    if (m_IsD3D12) {
        const BOOL isIconic = (m_hWnd != nullptr) ? IsIconic(m_hWnd) : FALSE;
        const BOOL hasZeroSize = (m_State.width == 0 || m_State.height == 0) ? TRUE : FALSE;
        DX12_NoteWrappedD3D12PresentResult("Present1", callCount, SyncInterval, PresentFlags, hr,
                                           m_State.isFullscreen ? TRUE : FALSE, isIconic, hasZeroSize, m_hWnd);
    }
    const bool flushProcessHasForeground = m_IsD3D12 ? ResolveCurrentProcessForeground(nullptr, nullptr) : true;
    const bool focusLostForFlush = m_IsD3D12 && !flushProcessHasForeground && !m_State.isFullscreen;
    const auto flushInfo =
        FlushDeferredDX12OverlaySignalAfterWrappedPresent(m_IsD3D12, "Present1", callCount, focusLostForFlush);
    LogD3D12PresentDeviceLostHRESULT(m_IsD3D12, "Present1", callCount, hr);
    WaitD3D12FocusLossOverlayFenceAfterPresent("Present1", callCount, SyncInterval, PresentFlags, hr, flushInfo);
    if (SUCCEEDED(hr)) {
        WaitFrameLatency();
        g_SharedFpsLimiter.ApplyPostPresent();
    }
    ProbeD3D12FocusLossFrameLatencyAfterPresent("Present1", callCount, SyncInterval, PresentFlags, hr);
    return hr;
}

BOOL STDMETHODCALLTYPE CWrapDXGISwapChain::IsTemporaryMonoSupported() {
    if (!m_pReal1)
        return FALSE;
    return m_pReal1->IsTemporaryMonoSupported();
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetRestrictToOutput(ppRestrictToOutput);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetBackgroundColor(const DXGI_RGBA* pColor) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->SetBackgroundColor(pColor);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetBackgroundColor(DXGI_RGBA* pColor) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetBackgroundColor(pColor);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetRotation(DXGI_MODE_ROTATION Rotation) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->SetRotation(Rotation);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetRotation(DXGI_MODE_ROTATION* pRotation) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetRotation(pRotation);
}

// ============================================================================
// IDXGISwapChain2 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetSourceSize(UINT Width, UINT Height) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->SetSourceSize(Width, Height);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetSourceSize(UINT* pWidth, UINT* pHeight) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->GetSourceSize(pWidth, pHeight);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetMaximumFrameLatency(UINT MaxLatency) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;

    // Apply frame latency override from config
    if (g_IPC) {
        const auto& gfx = GetActiveGraphicsConfig();
        if (gfx.frameLatency > 0) {
            MaxLatency = (UINT)gfx.frameLatency;
            WrapperLog("SetMaximumFrameLatency: Overriding to %u", MaxLatency);
        }
    }

    return m_pReal2->SetMaximumFrameLatency(MaxLatency);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetMaximumFrameLatency(UINT* pMaxLatency) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->GetMaximumFrameLatency(pMaxLatency);
}
HANDLE STDMETHODCALLTYPE CWrapDXGISwapChain::GetFrameLatencyWaitableObject() {
    if (!m_pReal2)
        return nullptr;
    return m_pReal2->GetFrameLatencyWaitableObject();
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->SetMatrixTransform(pMatrix);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->GetMatrixTransform(pMatrix);
}

// ============================================================================
// IDXGISwapChain3 Implementation
// ============================================================================

UINT STDMETHODCALLTYPE CWrapDXGISwapChain::GetCurrentBackBufferIndex() {
    if (!m_pReal3)
        return 0;
    return m_pReal3->GetCurrentBackBufferIndex();
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                     UINT* pColorSpaceSupport) {
    if (!m_pReal3)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) {
    if (!m_pReal3)
        return DXGI_ERROR_UNSUPPORTED;
    const HRESULT result = m_pReal3->SetColorSpace1(ColorSpace);
    if (SUCCEEDED(result)) {
        DXGIShared::RecordSwapChainColorSpace(m_pReal, ColorSpace);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                                             DXGI_FORMAT Format, UINT SwapChainFlags,
                                                             const UINT* pCreationNodeMask,
                                                             IUnknown* const* ppPresentQueue) {
    if (HasBackbufferCountOverride(GetActiveGraphicsConfig().backbufferCount))
        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    // RECURSION GUARD: Prevent infinite recursion with Steam/other overlays
    static std::atomic<DWORD> s_resize1ThreadId{0};
    static std::atomic<int> s_resize1Depth{0};
    DWORD currentId = GetCurrentThreadId();
    if (s_resize1Depth.load() > 0 && s_resize1ThreadId.load() == currentId) {
        return m_pReal3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                        ppPresentQueue);
    }
    s_resize1ThreadId.store(currentId);
    s_resize1Depth.fetch_add(1);

    bool expected = false;
    if (!s_ResizeInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        HRESULT concurrentHr = S_OK;
        {
            ScopedResizeGuard guard;
            concurrentHr = m_pReal3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags,
                                                    pCreationNodeMask, ppPresentQueue);
        }
        if (s_resize1Depth.fetch_sub(1) == 1) {
            s_resize1ThreadId.store(0);
        }
        return concurrentHr;
    }

    DX12_OnSwapchainResizeBegin();
    CleanupOverlayResources();
    // CRITICAL FIX: Release DX11 backbuffer RTV before ResizeBuffers (same as above).
    if (!m_IsD3D12)
        DXGIShared::HandleDX11ResizeBegin();

    HRESULT hr = S_OK;
    {
        ScopedResizeGuard guard;
        hr = m_pReal3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                      ppPresentQueue);
    }

    DX12_OnSwapchainResizeEnd();
    if (SUCCEEDED(hr)) {
        m_OverlayResourcesValid = true;
        m_hFrameLatencyWaitable = INVALID_HANDLE_VALUE;
        m_FrameLatencyWaitableQueried = false;
        DetectSwapChainState();
    }
    s_ResizeInProgress.store(false, std::memory_order_release);
    if (s_resize1Depth.fetch_sub(1) == 1) {
        s_resize1ThreadId.store(0);
    }
    return hr;
}

// ============================================================================
// IDXGISwapChain4 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) {
    if (!m_pReal4)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal4->SetHDRMetaData(Type, Size, pMetaData);
}

// ============================================================================
// WrapperStateManager Implementation
// ============================================================================

void WrapperStateManager::RegisterSwapchain(CWrapDXGISwapChain* pWrapper, IDXGISwapChain* pReal) {
    ScopedExclusiveLock lock(m_Lock);  // Exclusive lock for writing
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_Wrappers[i] == nullptr) {
            m_Wrappers[i] = pWrapper;
            m_RealSwapchains[i] = pReal;
            return;
        }
    }
}

void WrapperStateManager::UnregisterSwapchain(CWrapDXGISwapChain* pWrapper) {
    ScopedExclusiveLock lock(m_Lock);  // Exclusive lock for writing
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_Wrappers[i] == pWrapper) {
            m_Wrappers[i] = nullptr;
            m_RealSwapchains[i] = nullptr;
            return;
        }
    }
}

CWrapDXGISwapChain* WrapperStateManager::FindWrapper(IDXGISwapChain* pReal) {
    ScopedSharedLock lock(m_Lock);  // Shared lock for reading (Concurrent Access)
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_RealSwapchains[i] == pReal || m_Wrappers[i] == (CWrapDXGISwapChain*)pReal) {
            return m_Wrappers[i];
        }
    }
    return nullptr;
}
