/**
 * DXGI Swapchain Wrapper Implementation
 *
 * Core implementation for safe overlay drawing with FG runtimes.
 */

#include "dxgi_swapchain_wrap.h"
#include "hook_common.h"
#include "d3d12_wrapper_interface.h"
#include <windows.h>
#include "../apis/graphics_hook.h"

// External overlay functions (implemented in dx11_hook.cpp / dx12_hook.cpp)
extern void DrawDX11Overlay(IDXGISwapChain* pSwapChain);
extern void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain);
extern void DX12_OnSwapchainResizeBegin();
extern void DX12_OnSwapchainResizeEnd();
extern "C" __declspec(dllimport) void DX12_SetCommandQueue(IUnknown* pQueue);

#ifndef BUILDING_CAPTURE_HOOK
// Dynamically import from capture_hook to update shared state across DLL boundaries (for d3d12_wrappers.dll)
typedef void (*PFN_AdjustDepth)(int);
static PFN_AdjustDepth pAdjustDepth = nullptr;

void DX12_AdjustWrapperResizeDepth(int delta) {
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

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapDXGISwapChain::CWrapDXGISwapChain(IDXGISwapChain* pReal, IUnknown* pDevice)
    : m_pReal(pReal)
    , m_pReal1(nullptr)
    , m_pReal2(nullptr)
    , m_pReal3(nullptr)
    , m_pReal4(nullptr)
    , m_pDevice(pDevice)
    , m_pD3D12Queue(nullptr)
    , m_RefCount(1)
    , m_hWnd(nullptr)
    , m_Version(0)
    , m_OverlayResourcesValid(false)
    , m_IsD3D12(false)
    , m_Promoted(false)
{
    if (pReal) {
        pReal->AddRef();
        
        // FIX B: AddRef the device/queue if we store it
        if (m_pDevice) {
            m_pDevice->AddRef();
        }
        
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(pReal->GetDesc(&desc))) {
            m_hWnd = desc.OutputWindow;
        }
        
        if (pDevice) {
            ID3D12CommandQueue* pQueue = nullptr;
            if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue)))) {
                m_IsD3D12 = true;
                DX12_SetCommandQueue(pQueue);
                pQueue->Release();
            }
        }
    }
    
    WrapperStateManager::Get().RegisterSwapchain(this, pReal);
}

void CWrapDXGISwapChain::EnsurePromoted() {
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

CWrapDXGISwapChain::~CWrapDXGISwapChain() {
    WrapperStateManager::Get().UnregisterSwapchain(this);
    CleanupOverlayResources();
    if (m_pD3D12Queue) m_pD3D12Queue->Release();
    if (m_pReal4) m_pReal4->Release();
    if (m_pReal3) m_pReal3->Release();
    if (m_pReal2) m_pReal2->Release();
    if (m_pReal1) m_pReal1->Release();
    if (m_pReal) m_pReal->Release();
    if (m_pDevice) m_pDevice->Release();
}

void CWrapDXGISwapChain::PromoteInterfaces() {
    if (!m_pReal) return;
    try {
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1)))) m_Version = 1;
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2)))) m_Version = 2;
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3)))) m_Version = 3;
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4)))) m_Version = 4;
    } catch (...) {}
}

void CWrapDXGISwapChain::CleanupOverlayResources() {
    std::lock_guard<std::mutex> lock(m_ResourceLock);
    m_OverlayResourcesValid = false;
}

void CWrapDXGISwapChain::DrawOverlay() {
    if (!g_OverlayEnabled) return;
    if (m_IsD3D12) {
        DX12_ProcessFrameExternal(m_pReal);
    } else {
        DrawDX11Overlay(m_pReal);
    }
}

// ============================================================================
// IUnknown Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_POINTER;
    EnsurePromoted();
    
    if (IsUnwrapAttemptGUID(riid)) {
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }
    
    if (riid == IID_CWrapDXGISwapChain) {
        *ppvObj = m_pReal;
        return S_OK;
    }
    
    if (riid == IID_IUnknown || riid == IID_IDXGIObject || riid == IID_IDXGIDeviceSubObject || riid == IID_IDXGISwapChain) {
        AddRef();
        *ppvObj = static_cast<IDXGISwapChain*>(this);
        return S_OK;
    }
    
    if (riid == IID_IDXGISwapChain1 && m_Version >= 1) { AddRef(); *ppvObj = static_cast<IDXGISwapChain1*>(this); return S_OK; }
    if (riid == IID_IDXGISwapChain2 && m_Version >= 2) { AddRef(); *ppvObj = static_cast<IDXGISwapChain2*>(this); return S_OK; }
    if (riid == IID_IDXGISwapChain3 && m_Version >= 3) { AddRef(); *ppvObj = static_cast<IDXGISwapChain3*>(this); return S_OK; }
    if (riid == IID_IDXGISwapChain4 && m_Version >= 4) { AddRef(); *ppvObj = static_cast<IDXGISwapChain4*>(this); return S_OK; }
    
    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapDXGISwapChain::AddRef() { return InterlockedIncrement(&m_RefCount); }
ULONG STDMETHODCALLTYPE CWrapDXGISwapChain::Release() { ULONG count = InterlockedDecrement(&m_RefCount); if (count == 0) delete this; return count; }

// ============================================================================
// IDXGIObject Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) { return m_pReal->SetPrivateData(Name, DataSize, pData); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) { return m_pReal->SetPrivateDataInterface(Name, pUnknown); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) {
    if (IsUnwrapAttemptGUID(Name)) return DXGI_ERROR_NOT_FOUND;
    return m_pReal->GetPrivateData(Name, pDataSize, pData);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetParent(REFIID riid, void** ppParent) { return m_pReal->GetParent(riid, ppParent); }

// ============================================================================
// IDXGIDeviceSubObject Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDevice(REFIID riid, void** ppDevice) { return m_pReal->GetDevice(riid, ppDevice); }

// ============================================================================
// IDXGISwapChain Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present(UINT SyncInterval, UINT Flags) {
    std::lock_guard<std::mutex> lock(m_ResourceLock);
    DrawOverlay();
    return m_pReal->Present(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) { return m_pReal->GetBuffer(Buffer, riid, ppSurface); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) { return m_pReal->SetFullscreenState(Fullscreen, pTarget); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) { return m_pReal->GetFullscreenState(pFullscreen, ppTarget); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) { return m_pReal->GetDesc(pDesc); }

static std::atomic<bool> s_ResizeInProgress{false};

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    bool expected = false;
    if (!s_ResizeInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return S_OK;
    
    DX12_OnSwapchainResizeBegin();
    CleanupOverlayResources();
    
    HRESULT hr = S_OK;
    {
        ScopedResizeGuard guard;
        hr = m_pReal->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    
    DX12_OnSwapchainResizeEnd();
    if (SUCCEEDED(hr)) m_OverlayResourcesValid = true;
    s_ResizeInProgress.store(false, std::memory_order_release);
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) { return m_pReal->ResizeTarget(pNewTargetParameters); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetContainingOutput(IDXGIOutput** ppOutput) { return m_pReal->GetContainingOutput(ppOutput); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) { return m_pReal->GetFrameStatistics(pStats); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetLastPresentCount(UINT* pLastPresentCount) { return m_pReal->GetLastPresentCount(pLastPresentCount); }

// ============================================================================
// IDXGISwapChain1 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) { if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED; return m_pReal1->GetDesc1(pDesc); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) { if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED; return m_pReal1->GetFullscreenDesc(pDesc); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetHwnd(HWND* pHwnd) { if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED; return m_pReal1->GetHwnd(pHwnd); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetCoreWindow(REFIID refiid, void** ppUnk) { if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED; return m_pReal1->GetCoreWindow(refiid, ppUnk); }

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    std::lock_guard<std::mutex> lock(m_ResourceLock);
    DrawOverlay();
    return m_pReal1->Present1(SyncInterval, PresentFlags, pPresentParameters);
}

BOOL STDMETHODCALLTYPE CWrapDXGISwapChain::IsTemporaryMonoSupported() { if (!m_pReal1) return FALSE; return m_pReal1->IsTemporaryMonoSupported(); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) { if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED; return m_pReal1->GetRestrictToOutput(ppRestrictToOutput); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetBackgroundColor(const DXGI_RGBA* pColor) { if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED; return m_pReal1->SetBackgroundColor(pColor); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetBackgroundColor(DXGI_RGBA* pColor) { if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED; return m_pReal1->GetBackgroundColor(pColor); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetRotation(DXGI_MODE_ROTATION Rotation) { if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED; return m_pReal1->SetRotation(Rotation); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetRotation(DXGI_MODE_ROTATION* pRotation) { if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED; return m_pReal1->GetRotation(pRotation); }

// ============================================================================
// IDXGISwapChain2 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetSourceSize(UINT Width, UINT Height) { if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED; return m_pReal2->SetSourceSize(Width, Height); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetSourceSize(UINT* pWidth, UINT* pHeight) { if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED; return m_pReal2->GetSourceSize(pWidth, pHeight); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetMaximumFrameLatency(UINT MaxLatency) { if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED; return m_pReal2->SetMaximumFrameLatency(MaxLatency); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetMaximumFrameLatency(UINT* pMaxLatency) { if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED; return m_pReal2->GetMaximumFrameLatency(pMaxLatency); }
HANDLE STDMETHODCALLTYPE CWrapDXGISwapChain::GetFrameLatencyWaitableObject() { if (!m_pReal2) return nullptr; return m_pReal2->GetFrameLatencyWaitableObject(); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) { if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED; return m_pReal2->SetMatrixTransform(pMatrix); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) { if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED; return m_pReal2->GetMatrixTransform(pMatrix); }

// ============================================================================
// IDXGISwapChain3 Implementation
// ============================================================================

UINT STDMETHODCALLTYPE CWrapDXGISwapChain::GetCurrentBackBufferIndex() { if (!m_pReal3) return 0; return m_pReal3->GetCurrentBackBufferIndex(); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT* pColorSpaceSupport) { if (!m_pReal3) return DXGI_ERROR_UNSUPPORTED; return m_pReal3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport); }
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) { if (!m_pReal3) return DXGI_ERROR_UNSUPPORTED; return m_pReal3->SetColorSpace1(ColorSpace); }

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue) {
    bool expected = false;
    if (!s_ResizeInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return S_OK;
    
    DX12_OnSwapchainResizeBegin();
    CleanupOverlayResources();
    
    HRESULT hr = S_OK;
    {
        ScopedResizeGuard guard;
        hr = m_pReal3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
    }
    
    DX12_OnSwapchainResizeEnd();
    if (SUCCEEDED(hr)) m_OverlayResourcesValid = true;
    s_ResizeInProgress.store(false, std::memory_order_release);
    return hr;
}

// ============================================================================
// IDXGISwapChain4 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) { if (!m_pReal4) return DXGI_ERROR_UNSUPPORTED; return m_pReal4->SetHDRMetaData(Type, Size, pMetaData); }

// ============================================================================
// WrapperStateManager Implementation
// ============================================================================

void WrapperStateManager::RegisterSwapchain(CWrapDXGISwapChain* pWrapper, IDXGISwapChain* pReal) {
    ScopedExclusiveLock lock(m_Lock); // Exclusive lock for writing
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_Wrappers[i] == nullptr) {
            m_Wrappers[i] = pWrapper;
            m_RealSwapchains[i] = pReal;
            return;
        }
    }
}

void WrapperStateManager::UnregisterSwapchain(CWrapDXGISwapChain* pWrapper) {
    ScopedExclusiveLock lock(m_Lock); // Exclusive lock for writing
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_Wrappers[i] == pWrapper) {
            m_Wrappers[i] = nullptr;
            m_RealSwapchains[i] = nullptr;
            return;
        }
    }
}

CWrapDXGISwapChain* WrapperStateManager::FindWrapper(IDXGISwapChain* pReal) {
    ScopedSharedLock lock(m_Lock); // Shared lock for reading (Concurrent Access)
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_RealSwapchains[i] == pReal || m_Wrappers[i] == (CWrapDXGISwapChain*)pReal) {
            return m_Wrappers[i];
        }
    }
    return nullptr;
}
