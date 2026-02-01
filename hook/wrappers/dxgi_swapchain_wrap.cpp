/**
 * DXGI Swapchain Wrapper Implementation
 *
 * Core implementation for safe overlay drawing with FG runtimes.
 */

#include "dxgi_swapchain_wrap.h"
#include <windows.h>
#include "../apis/graphics_hook.h"
#include "d3d12_wrapper_interface.h"
#include "hook_common.h"
#include "../common/dxgi_shared.h"
#include "../common/performance_metrics.h"

// External overlay functions (implemented in dx11_hook.cpp / dx12_hook.cpp)
extern void DrawDX11Overlay(IDXGISwapChain* pSwapChain);
extern void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain);
extern void DX11_ProcessFrameExternal(IDXGISwapChain* pSwapChain);
extern void DX12_OnSwapchainResizeBegin();
extern void DX12_OnSwapchainResizeEnd();
extern "C" __declspec(dllimport) void DX12_SetCommandQueue(IUnknown* pQueue);

// FG detection for FSR FG/DLSS FG compatibility
#include "../common/fg_detection.h"

#ifndef BUILDING_CAPTURE_HOOK
// Dynamically import from capture_hook to update shared state across DLL boundaries (for d3d12_wrappers.dll)
typedef void (*PFN_AdjustDepth)(int);
static PFN_AdjustDepth pAdjustDepth = nullptr;

void DX12_AdjustWrapperResizeDepth(int delta)
{
    if (!pAdjustDepth) {
        HMODULE hHook = GetModuleHandleA("capture_hook_x64.dll");
        if (!hHook) hHook = GetModuleHandleA("capture_hook_x86.dll");
        if (hHook) {
            pAdjustDepth = (PFN_AdjustDepth)GetProcAddress(hHook, "DX12_AdjustWrapperResizeDepth_C");
        }
    }
    if (pAdjustDepth) pAdjustDepth(delta);
}
#else
// Internal build: Symbol provided by dx12_hook.cpp
extern void DX12_AdjustWrapperResizeDepth(int delta);
#endif

// RAII Guard for Resize Scope
struct ScopedResizeGuard {
    ScopedResizeGuard() { DX12_AdjustWrapperResizeDepth(1); }
    ~ScopedResizeGuard() { DX12_AdjustWrapperResizeDepth(-1); }
};
#include <cstdint>
extern void DX11_UpdatePerformanceMetrics(int64_t qpcUs);

static bool g_OverlayEnabled = true;

// Thread-local flag to track when we're inside the wrapper's Present
// This prevents vtable hooks from also processing the frame
thread_local bool g_InWrapperPresent = false;

// Function to check if we're in wrapper Present (same DLL, no export needed)
bool IsInWrapperPresent()
{
    return g_InWrapperPresent;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapDXGISwapChain::CWrapDXGISwapChain(IDXGISwapChain* pReal, IUnknown* pDevice)
    : m_pReal(pReal), m_pReal1(nullptr), m_pReal2(nullptr), m_pReal3(nullptr), m_pReal4(nullptr), m_pDevice(pDevice),
      m_pD3D12Queue(nullptr), m_RefCount(1), m_hWnd(nullptr), m_Version(0), m_OverlayResourcesValid(false),
      m_IsD3D12(false), m_Promoted(false), m_DestructionCookie(0)
{
    if (pReal) {
        pReal->AddRef();

        // FIX B: AddRef the device/queue if we store it
        if (m_pDevice) {
            m_pDevice->AddRef();
        }

        // Detect swapchain state (flip model, fullscreen, etc.)
        DetectSwapChainState();

        if (pDevice) {
            ID3D12CommandQueue* pQueue = nullptr;
            if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue)))) {
                m_IsD3D12 = true;
                DX12_SetCommandQueue(pQueue);
                pQueue->Release();
                // FIX: Mark overlay as ready - DX12 systems handle lazy initialization internally
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
    WrapperLog("SwapChain: Created wrapper (real=%p, isD3D12=%d, flipModel=%d)", 
               pReal, m_IsD3D12, m_FlipModel.active);
}

void CWrapDXGISwapChain::DetectSwapChainState()
{
    if (!m_pReal) return;
    
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (SUCCEEDED(m_pReal->GetDesc(&desc))) {
        m_hWnd = desc.OutputWindow;
        m_State.isFullscreen = !desc.Windowed;
        m_State.format = desc.BufferDesc.Format;
        m_State.width = desc.BufferDesc.Width;
        m_State.height = desc.BufferDesc.Height;
        
        // Detect flip model for FSR FG compatibility
        m_FlipModel.active = (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                              desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
        m_FlipModel.native = m_FlipModel.active;
        
        WrapperLog("SwapChain: Detected state - %dx%d, FlipModel=%d, Fullscreen=%d, Format=%d",
                   m_State.width, m_State.height, m_FlipModel.active, m_State.isFullscreen, m_State.format);
    }
    
    // Get frame latency if available (SwapChain2+)
    if (m_pReal2) {
        m_pReal2->GetMaximumFrameLatency(&m_State.frameLatency);
    }
}

void WINAPI CWrapDXGISwapChain::DestructionCallback(void* pData)
{
    auto* pSwapChain = static_cast<CWrapDXGISwapChain*>(pData);
    if (pSwapChain) {
        WrapperLog("SwapChain: DestructionCallback for wrapper %p", pSwapChain);
        // Mark that the real swapchain is being destroyed
        // Actual cleanup happens in Release() when ref count reaches 0
        pSwapChain->m_DestructionCookie = 0;  // Mark as destroyed
    }
}

HRESULT CWrapDXGISwapChain::RegisterDestructionCallback()
{
    if (!m_pReal) return E_FAIL;
    
    // Try to register for destruction notification (requires DXGI 1.4+ / Windows 10)
    // ID3DDestructionNotifier interface GUID: {A05C8C18-92DB-4B35-944B-E3083333C2A0}
    static const GUID IID_ID3DDestructionNotifier = 
    { 0xa05c8c18, 0x92db, 0x4b35, { 0x94, 0x4b, 0xe3, 0x08, 0x33, 0x33, 0xc2, 0xa0 } };
    
    struct ID3DDestructionNotifier : public IUnknown {
        virtual HRESULT RegisterDestructionCallback(void* pCallbackFn, void* pData, UINT* pCookie) = 0;
        virtual HRESULT UnregisterDestructionCallback(UINT Cookie) = 0;
    };
    
    ID3DDestructionNotifier* pNotifier = nullptr;
    if (SUCCEEDED(m_pReal->QueryInterface(IID_ID3DDestructionNotifier, (void**)&pNotifier))) {
        HRESULT hr = pNotifier->RegisterDestructionCallback(
            (void*)DestructionCallback, this, &m_DestructionCookie);
        pNotifier->Release();
        if (SUCCEEDED(hr)) {
            WrapperLog("SwapChain: Registered destruction callback (cookie=%u)", m_DestructionCookie);
        }
        return hr;
    }
    return E_NOTIMPL;
}

void CWrapDXGISwapChain::EnsurePromoted()
{
    if (m_Promoted || !m_pReal) return;
    PromoteInterfaces();
    m_Promoted = true;
}

CWrapDXGISwapChain::CWrapDXGISwapChain(IDXGISwapChain1* pReal, IUnknown* pDevice)
    : CWrapDXGISwapChain(static_cast<IDXGISwapChain*>(pReal), pDevice)
{
    if (!m_pReal1 && pReal) {
        m_pReal1 = pReal;
        m_pReal1->AddRef();
        m_Version = 1;
    }
}

CWrapDXGISwapChain::~CWrapDXGISwapChain()
{
    WrapperLog("SwapChain: Destroying wrapper %p (real=%p)", this, m_pReal);
    
    // Unregister destruction callback if registered
    if (m_DestructionCookie != 0 && m_pReal) {
        static const GUID IID_ID3DDestructionNotifier = 
        { 0xa05c8c18, 0x92db, 0x4b35, { 0x94, 0x4b, 0xe3, 0x08, 0x33, 0x33, 0xc2, 0xa0 } };
        struct ID3DDestructionNotifier : public IUnknown {
            virtual HRESULT RegisterDestructionCallback(void* pCallbackFn, void* pData, UINT* pCookie) = 0;
            virtual HRESULT UnregisterDestructionCallback(UINT Cookie) = 0;
        };
        ID3DDestructionNotifier* pNotifier = nullptr;
        if (SUCCEEDED(m_pReal->QueryInterface(IID_ID3DDestructionNotifier, (void**)&pNotifier))) {
            pNotifier->UnregisterDestructionCallback(m_DestructionCookie);
            pNotifier->Release();
        }
    }
    
    WrapperStateManager::Get().UnregisterSwapchain(this);
    CleanupOverlayResources();
    if (m_pD3D12Queue) m_pD3D12Queue->Release();
    if (m_pReal4) m_pReal4->Release();
    if (m_pReal3) m_pReal3->Release();
    if (m_pReal2) m_pReal2->Release();
    if (m_pReal1) m_pReal1->Release();
    if (m_pReal) {
        // Remove private data before releasing
        m_pReal->SetPrivateData(IID_CWrapDXGISwapChain, 0, nullptr);
        m_pReal->Release();
    }
    if (m_pDevice) m_pDevice->Release();
}

void CWrapDXGISwapChain::PromoteInterfaces()
{
    if (!m_pReal) return;
    try {
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1)))) m_Version = 1;
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2)))) m_Version = 2;
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3)))) m_Version = 3;
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4)))) m_Version = 4;
    } catch (...) {
    }
}

void CWrapDXGISwapChain::CleanupOverlayResources()
{
    // Atomic update - no mutex needed for simple flag
    m_OverlayResourcesValid.store(false, std::memory_order_release);
}

void CWrapDXGISwapChain::DrawOverlay()
{
    static int s_DrawCount = 0;
    bool shouldLog = (++s_DrawCount <= 10);
    
    if (!g_OverlayEnabled) {
        if (shouldLog) WrapperLog("DrawOverlay: skipped (overlay disabled)");
        return;
    }
    if (shouldLog) WrapperLog("DrawOverlay: m_IsD3D12=%d", m_IsD3D12);
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

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    EnsurePromoted();

    // CRITICAL FIX: Block Streamline base interface to prevent FSR FG / DLSS FG from unwrapping
    if (IsEqualGUID(riid, IID_IStreamlineBaseInterface)) {
        WrapperLog("SwapChain: BLOCKED Streamline interface query (FSR FG/DLSS FG unwrap attempt)");
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }

    // REMOVED: Don't allow unwrapping via IID_CWrapDXGISwapChain - this let FG runtimes bypass us
    // if (riid == IID_CWrapDXGISwapChain) {
    //     *ppvObj = m_pReal;
    //     return S_OK;
    // }
    
    // Allow retrieval of wrapper from real swapchain (internal use only)
    if (IsEqualGUID(riid, IID_CWrapDXGISwapChain)) {
        AddRef();
        *ppvObj = this;  // Return wrapper, NOT real swapchain
        WrapperLog("SwapChain: QueryInterface for IID_CWrapDXGISwapChain - returning wrapper %p", this);
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
    static int s_LogCount = 0;
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

ULONG STDMETHODCALLTYPE CWrapDXGISwapChain::AddRef() 
{ 
    ULONG refs = InterlockedIncrement(&m_RefCount);
    // Also AddRef the real swapchain to track total references
    if (m_pReal) {
        m_pReal->AddRef();
    }
    return refs;
}

ULONG STDMETHODCALLTYPE CWrapDXGISwapChain::Release()
{
    ULONG xrefs = InterlockedDecrement(&m_RefCount);  // External references
    
    // When external refs reach 0, game expects SwapChain destruction
    if (xrefs == 0) {
        WrapperLog("SwapChain: External refs reached 0, preparing for destruction (wrapper=%p)", this);
        CleanupOverlayResources();
    }
    
    // Release the real swapchain
    ULONG refs = 0;
    if (m_pReal) {
        refs = m_pReal->Release();
    }
    
    // Only delete when both external refs AND real swapchain refs are 0
    if (xrefs == 0 && refs == 0) {
        WrapperLog("SwapChain: Deleting wrapper %p", this);
        delete this;
    }
    
    return xrefs;  // Return external ref count
}

// ============================================================================
// IDXGIObject Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return m_pReal->SetPrivateData(Name, DataSize, pData);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return m_pReal->SetPrivateDataInterface(Name, pUnknown);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    if (IsUnwrapAttemptGUID(Name)) return DXGI_ERROR_NOT_FOUND;
    return m_pReal->GetPrivateData(Name, pDataSize, pData);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetParent(REFIID riid, void** ppParent)
{
    return m_pReal->GetParent(riid, ppParent);
}

// ============================================================================
// IDXGIDeviceSubObject Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDevice(REFIID riid, void** ppDevice)
{
    return m_pReal->GetDevice(riid, ppDevice);
}

// ============================================================================
// IDXGISwapChain Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present(UINT SyncInterval, UINT Flags)
{
    // DEBUG: Log every Present call to verify wrapper is being invoked
    static std::atomic<int> s_presentCallCount{0};
    int callCount = s_presentCallCount.fetch_add(1);
    if (callCount < 20 || callCount % 300 == 0) {
        WrapperLog("Present: CALLED call#%d (m_IsD3D12=%d, flipModel=%d, FG=%d)", 
                   callCount, m_IsD3D12, m_FlipModel.active, g_FGCompat.IsFGActive());
    }
    
    // FSR FG/DLSS FG compatibility: If FG is active and using flip model,
    // be more careful about modifying Present parameters
    bool fgActive = g_FGCompat.IsFGActive();
    if (fgActive && m_FlipModel.active) {
        // Log FG interaction for debugging
        if (callCount < 50) {
            WrapperLog("Present: FG is active with flip model, using conservative approach");
        }
        // Don't override sync interval when FG is active - it can break frame pacing
        // Still process capture though
    }
    
    // RECURSION GUARD: Prevent infinite recursion with Steam/other overlays
    // Using atomic+threadId instead of thread_local to avoid static destructor issues
    static std::atomic<DWORD> s_presentThreadId{0};
    static std::atomic<int> s_presentDepth{0};
    DWORD currentId = GetCurrentThreadId();
    if (s_presentDepth.load() > 0 && s_presentThreadId.load() == currentId) {
        WrapperLog("Present: Recursion detected, passing through to real swapchain");
        return m_pReal->Present(SyncInterval, Flags);
    }
    s_presentThreadId.store(currentId);
    s_presentDepth.fetch_add(1);
    
    // Set flag to indicate we're inside wrapper's Present
    // This prevents vtable hooks from double-processing
    g_InWrapperPresent = true;
    if (callCount < 20) {
        WrapperLog("Present: Processing call#%d", callCount);
    }
    
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
    
    // Apply VSync override from config (skip if FG is active - can break frame pacing)
    if (!fgActive) {
        ProcessVSyncOverride(SyncInterval, Flags);
    } else if (callCount < 20) {
        WrapperLog("Present: Skipping VSync override because FG is active");
    }
    
    // CRITICAL: Process frame for capture BEFORE calling real Present
    // This must happen regardless of overlay state - capture works independently
    static int s_CaptureCallCount = 0;
    if (m_IsD3D12) {
        if (++s_CaptureCallCount <= 5) {
            WrapperLog("Present #%d: Calling DX12_ProcessFrameExternal", s_CaptureCallCount);
        }
        DX12_ProcessFrameExternal(m_pReal);
        
        // DX12: Draw overlay separately (ProcessFrameExternal doesn't draw overlay for DX12)
        static int s_PresentCount = 0;
        if (++s_PresentCount <= 5) {
            WrapperLog("Present #%d: m_IsD3D12=%d", s_PresentCount, m_IsD3D12);
        }
        DrawOverlay();
    } else {
        // DX11/DX10: Call DX11_ProcessFrameExternal for capture AND overlay
        // NOTE: DX11_ProcessFrameExternal already calls DrawDX11Overlay internally,
        // so we do NOT call DrawOverlay() here to avoid double-counting frames
        if (++s_CaptureCallCount <= 5) {
            WrapperLog("Present #%d: Calling DX11_ProcessFrameExternal", s_CaptureCallCount);
        }
        DX11_ProcessFrameExternal(m_pReal);
    }
    
    HRESULT hr = m_pReal->Present(SyncInterval, Flags);
    
    // Clear wrapper Present flag
    g_InWrapperPresent = false;
    
    if (s_presentDepth.fetch_sub(1) == 1) {
        s_presentThreadId.store(0);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    return m_pReal->GetBuffer(Buffer, riid, ppSurface);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    return m_pReal->SetFullscreenState(Fullscreen, pTarget);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    return m_pReal->GetFullscreenState(pFullscreen, ppTarget);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) { return m_pReal->GetDesc(pDesc); }

static std::atomic<bool> s_ResizeInProgress{false};

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                                            DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    WrapperLog("CWrapDXGISwapChain::ResizeBuffers called - Width=%u, Height=%u", Width, Height);
    
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
        WrapperLog("ResizeBuffers: already in progress, skipping");
        if (s_resizeDepth.fetch_sub(1) == 1) {
            s_resizeThreadId.store(0);
        }
        return S_OK;
    }

    // Apply backbuffer count override from config
    if (g_IPC) {
        const auto& gfx = GetActiveGraphicsConfig();
        if (gfx.backbufferCount > 0) {
            BufferCount = (UINT)gfx.backbufferCount;
            WrapperLog("ResizeBuffers: Overriding BufferCount to %u", BufferCount);
        }
    }

    WrapperLog("ResizeBuffers: calling DX12_OnSwapchainResizeBegin");
    DX12_OnSwapchainResizeBegin();
    WrapperLog("ResizeBuffers: DX12_OnSwapchainResizeBegin returned");
    
    WrapperLog("ResizeBuffers: calling CleanupOverlayResources");
    CleanupOverlayResources();
    WrapperLog("ResizeBuffers: CleanupOverlayResources returned");

    WrapperLog("ResizeBuffers: calling real ResizeBuffers...");
    HRESULT hr = S_OK;
    {
        ScopedResizeGuard guard;
        hr = m_pReal->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    WrapperLog("ResizeBuffers: real ResizeBuffers returned hr=0x%08X", hr);

    WrapperLog("ResizeBuffers: calling DX12_OnSwapchainResizeEnd");
    DX12_OnSwapchainResizeEnd();
    if (SUCCEEDED(hr)) m_OverlayResourcesValid = true;
    s_ResizeInProgress.store(false, std::memory_order_release);
    if (s_resizeDepth.fetch_sub(1) == 1) {
        s_resizeThreadId.store(0);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    return m_pReal->ResizeTarget(pNewTargetParameters);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetContainingOutput(IDXGIOutput** ppOutput)
{
    return m_pReal->GetContainingOutput(ppOutput);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return m_pReal->GetFrameStatistics(pStats);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetLastPresentCount(UINT* pLastPresentCount)
{
    return m_pReal->GetLastPresentCount(pLastPresentCount);
}

// ============================================================================
// IDXGISwapChain1 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetDesc1(pDesc);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetFullscreenDesc(pDesc);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetHwnd(HWND* pHwnd)
{
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetHwnd(pHwnd);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetCoreWindow(refiid, ppUnk);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present1(UINT SyncInterval, UINT PresentFlags,
                                                       const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    // RECURSION GUARD: Prevent infinite recursion with Steam/other overlays
    static std::atomic<DWORD> s_present1ThreadId{0};
    static std::atomic<int> s_present1Depth{0};
    DWORD currentId = GetCurrentThreadId();
    if (s_present1Depth.load() > 0 && s_present1ThreadId.load() == currentId) {
        return m_pReal1->Present1(SyncInterval, PresentFlags, pPresentParameters);
    }
    s_present1ThreadId.store(currentId);
    s_present1Depth.fetch_add(1);
    
    // Set flag to indicate we're inside wrapper's Present
    g_InWrapperPresent = true;
    
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
    
    // Apply VSync override from config
    ProcessVSyncOverride(SyncInterval, PresentFlags);
    
    // CRITICAL: Process frame for capture BEFORE calling real Present
    // This must happen regardless of overlay state - capture works independently
    if (m_IsD3D12) {
        DX12_ProcessFrameExternal(m_pReal);
        // DX12: Draw overlay separately (ProcessFrameExternal doesn't draw overlay for DX12)
        DrawOverlay();
    } else {
        // DX11/DX10: Call DX11_ProcessFrameExternal for capture AND overlay
        // NOTE: DX11_ProcessFrameExternal already calls DrawDX11Overlay internally,
        // so we do NOT call DrawOverlay() here to avoid double-counting frames
        DX11_ProcessFrameExternal(m_pReal);
    }
    
    HRESULT hr = m_pReal1->Present1(SyncInterval, PresentFlags, pPresentParameters);
    
    // Clear wrapper Present flag
    g_InWrapperPresent = false;
    
    if (s_present1Depth.fetch_sub(1) == 1) {
        s_present1ThreadId.store(0);
    }
    return hr;
}

BOOL STDMETHODCALLTYPE CWrapDXGISwapChain::IsTemporaryMonoSupported()
{
    if (!m_pReal1) return FALSE;
    return m_pReal1->IsTemporaryMonoSupported();
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetRestrictToOutput(ppRestrictToOutput);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->SetBackgroundColor(pColor);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetBackgroundColor(DXGI_RGBA* pColor)
{
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetBackgroundColor(pColor);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->SetRotation(Rotation);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetRotation(pRotation);
}

// ============================================================================
// IDXGISwapChain2 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetSourceSize(UINT Width, UINT Height)
{
    if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->SetSourceSize(Width, Height);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->GetSourceSize(pWidth, pHeight);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetMaximumFrameLatency(UINT MaxLatency)
{
    if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED;
    
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
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->GetMaximumFrameLatency(pMaxLatency);
}
HANDLE STDMETHODCALLTYPE CWrapDXGISwapChain::GetFrameLatencyWaitableObject()
{
    if (!m_pReal2) return nullptr;
    return m_pReal2->GetFrameLatencyWaitableObject();
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->SetMatrixTransform(pMatrix);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->GetMatrixTransform(pMatrix);
}

// ============================================================================
// IDXGISwapChain3 Implementation
// ============================================================================

UINT STDMETHODCALLTYPE CWrapDXGISwapChain::GetCurrentBackBufferIndex()
{
    if (!m_pReal3) return 0;
    return m_pReal3->GetCurrentBackBufferIndex();
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                     UINT* pColorSpaceSupport)
{
    if (!m_pReal3) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    if (!m_pReal3) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal3->SetColorSpace1(ColorSpace);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                                             DXGI_FORMAT Format, UINT SwapChainFlags,
                                                             const UINT* pCreationNodeMask,
                                                             IUnknown* const* ppPresentQueue)
{
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
        if (s_resize1Depth.fetch_sub(1) == 1) {
            s_resize1ThreadId.store(0);
        }
        return S_OK;
    }

    DX12_OnSwapchainResizeBegin();
    CleanupOverlayResources();

    HRESULT hr = S_OK;
    {
        ScopedResizeGuard guard;
        hr = m_pReal3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                      ppPresentQueue);
    }

    DX12_OnSwapchainResizeEnd();
    if (SUCCEEDED(hr)) m_OverlayResourcesValid = true;
    s_ResizeInProgress.store(false, std::memory_order_release);
    if (s_resize1Depth.fetch_sub(1) == 1) {
        s_resize1ThreadId.store(0);
    }
    return hr;
}

// ============================================================================
// IDXGISwapChain4 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData)
{
    if (!m_pReal4) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal4->SetHDRMetaData(Type, Size, pMetaData);
}

// ============================================================================
// WrapperStateManager Implementation
// ============================================================================

void WrapperStateManager::RegisterSwapchain(CWrapDXGISwapChain* pWrapper, IDXGISwapChain* pReal)
{
    ScopedExclusiveLock lock(m_Lock);  // Exclusive lock for writing
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_Wrappers[i] == nullptr) {
            m_Wrappers[i] = pWrapper;
            m_RealSwapchains[i] = pReal;
            return;
        }
    }
}

void WrapperStateManager::UnregisterSwapchain(CWrapDXGISwapChain* pWrapper)
{
    ScopedExclusiveLock lock(m_Lock);  // Exclusive lock for writing
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_Wrappers[i] == pWrapper) {
            m_Wrappers[i] = nullptr;
            m_RealSwapchains[i] = nullptr;
            return;
        }
    }
}

CWrapDXGISwapChain* WrapperStateManager::FindWrapper(IDXGISwapChain* pReal)
{
    ScopedSharedLock lock(m_Lock);  // Shared lock for reading (Concurrent Access)
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_RealSwapchains[i] == pReal || m_Wrappers[i] == (CWrapDXGISwapChain*)pReal) {
            return m_Wrappers[i];
        }
    }
    return nullptr;
}
