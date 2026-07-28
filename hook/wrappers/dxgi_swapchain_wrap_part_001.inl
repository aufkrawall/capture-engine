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
#include "../../common/logging.h"
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
    } catch (...) {
        // A foreign swapchain (Streamline/FFX proxies in particular) can throw out
        // of QueryInterface. Keeping the version reached so far is the right
        // fallback, but swallowing it silently hid why an FG proxy was treated as
        // an older interface than it really is.
        static std::atomic<uint64_t> s_promoteFailures{0};
        const uint64_t failures = s_promoteFailures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failures <= 3 || (failures % 100ull) == 0ull) {
            HookLog("[DXGI] Swapchain interface promotion threw; retaining version %u (occurrence %llu)", m_Version,
                    static_cast<unsigned long long>(failures));
        }
    }
}

void CWrapDXGISwapChain::CleanupOverlayResources() {
    // Atomic update - no mutex needed for simple flag
