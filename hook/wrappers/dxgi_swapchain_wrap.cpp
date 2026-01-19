/**
 * DXGI Swapchain Wrapper Implementation
 *
 * Core implementation for safe overlay drawing with FG runtimes.
 */

#include "dxgi_swapchain_wrap.h"
#include "hook_common.h"
#include "d3d12_wrapper_interface.h"
#include <windows.h>

// External overlay function (from dx12_hook.cpp or dx11_hook.cpp)
// These will be linked at runtime
// External overlay functions (implemented in dx11_hook.cpp / dx12_hook.cpp)
extern void DrawDX11Overlay(IDXGISwapChain* pSwapChain);
extern void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain);
extern "C" __declspec(dllimport) void DX12_SetCommandQueue(IUnknown* pQueue);
#include <cstdint>
extern void DX11_UpdatePerformanceMetrics(int64_t qpcUs);

static bool g_OverlayEnabled = true;

// WrapperLog now defined in wrapper_hooks.cpp

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
    // MINIMAL: Just store the real swapchain pointer and AddRef it
    if (pReal) {
        pReal->AddRef();
        
        // Get window handle directly (simple call)
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(pReal->GetDesc(&desc))) {
            m_hWnd = desc.OutputWindow;
        }
        
        // Detect D3D12 - check if device is an ID3D12CommandQueue
        if (pDevice) {
            ID3D12CommandQueue* pQueue = nullptr;
            if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue)))) {
                m_IsD3D12 = true;
                
                // CRITICAL: Register the command queue with the DX12 hook system
                // handles the case where CreateSwapChainForHwnd detour was bypassed or called before hook was ready
                DX12_SetCommandQueue(pQueue);
                
                pQueue->Release();
            }
        }
        
        WrapperLog("DXGI Wrapper: swapchain wrapper created (real=%p, hwnd=%p, isD3D12=%d) - lazy promotion enabled", 
                   pReal, m_hWnd, m_IsD3D12);
    } else {
        WrapperLog("DXGI Wrapper: swapchain wrapper created (pReal=null)");
    }
    
    // Skip all device handling - it can crash
    WrapperStateManager::Get().RegisterSwapchain(this, pReal);
}

// Lazy promotion - only promote when actually needed
void CWrapDXGISwapChain::EnsurePromoted() {
    if (m_Promoted || !m_pReal) return;
    
    WrapperLog("DXGI Wrapper: Lazy promoting swapchain...");
    PromoteInterfaces();
    m_Promoted = true;
    WrapperLog("DXGI Wrapper: Lazy promotion complete (version=%d)", m_Version);
}

CWrapDXGISwapChain::CWrapDXGISwapChain(IDXGISwapChain1* pReal, IUnknown* pDevice)
    : CWrapDXGISwapChain(static_cast<IDXGISwapChain*>(pReal), pDevice)
{
    // Version 1 or higher
    if (!m_pReal1 && pReal) {
        m_pReal1 = pReal;
        m_pReal1->AddRef();
        m_Version = 1;
    }
}

CWrapDXGISwapChain::~CWrapDXGISwapChain() {
    WrapperLog("DXGI Wrapper: Destroying swapchain wrapper (real=%p)", m_pReal);
    
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
    
    // CRITICAL: For Strange Brigade, wrap QueryInterface calls in try/catch
    // to prevent crashes from failing QueryInterface
    try {
        // Query ALL interfaces sequentially to ensure all supported pointers are populated.
        // m_Version will be set to the highest one found.
        
        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1)))) {
            m_Version = 1;
        }

        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2)))) {
            m_Version = 2;
        }

        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3)))) {
            m_Version = 3;
        }

        if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4)))) {
            m_Version = 4;
        }

        WrapperLog("DXGI Wrapper: Promoted to version %d", m_Version);
    } catch (...) {
        WrapperLog("DXGI Wrapper: PromoteInterfaces caught exception");
    }
    
    WrapperLog("DXGI Wrapper: Swapchain version = %d", m_Version);
}

void CWrapDXGISwapChain::CleanupOverlayResources() {
    std::lock_guard<std::mutex> lock(m_ResourceLock);
    m_OverlayResourcesValid = false;
    // Actual resource cleanup will be handled by overlay system
}

void CWrapDXGISwapChain::DrawOverlay() {
    if (!g_OverlayEnabled) return;
    if (WrapperStateManager::Get().swapchainInvalid.load()) return;
    
    // Self-Healing: Ensure this wrapper is registered
    // This handles cases where creation hooking might have been bypassed or failed
    if (!WrapperStateManager::Get().FindWrapper(m_pReal)) {
        static bool s_LoggedMissing = false;
        if (!s_LoggedMissing) {
             WrapperLog("DXGI Wrapper: Self-Registering missing swapchain wrapper (real=%p)", m_pReal);
             s_LoggedMissing = true;
        }
        WrapperStateManager::Get().RegisterSwapchain(this, m_pReal);
    }

    static int drawCount = 0;
    if (++drawCount % 60 == 0) {
        WrapperLog("DXGI Wrapper: DrawOverlay called (SC=%p, IsD3D12=%d, Count=%d)", m_pReal, m_IsD3D12, drawCount);
    }

    // Dispatch to the correct API-specific overlay drawer
    if (m_IsD3D12) {
        DX12_ProcessFrameExternal(m_pReal);
    } else {
        // Fallback for DX11/DX10
        DrawDX11Overlay(m_pReal);
    }
}

// ============================================================================
// IUnknown Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_POINTER;

    // Ensure we have probed for all available interfaces (Lazy Promotion)
    EnsurePromoted();
    
    // Block known FG runtime unwrap attempts
    if (IsUnwrapAttemptGUID(riid)) {
        WrapperLog("DXGI Wrapper: BLOCKED unwrap attempt!");
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }
    
    // Return real for our own GUID to support safe unwrapping
    if (riid == IID_CWrapDXGISwapChain) {
        *ppvObj = m_pReal;
        return S_OK;
    }
    
    // Return ourselves for standard DXGI interfaces
    if (riid == IID_IUnknown ||
        riid == IID_IDXGIObject ||
        riid == IID_IDXGIDeviceSubObject ||
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
    
    // Unknown interface - forward to real
    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapDXGISwapChain::AddRef() {
    return InterlockedIncrement(&m_RefCount);
}

ULONG STDMETHODCALLTYPE CWrapDXGISwapChain::Release() {
    ULONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0) {
        delete this;
    }
    return count;
}

// ============================================================================
// IDXGIObject Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) {
    return m_pReal->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) {
    return m_pReal->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) {
    // Block unwrap attempts via GetPrivateData
    if (IsUnwrapAttemptGUID(Name)) {
        WrapperLog("DXGI Wrapper: BLOCKED GetPrivateData unwrap attempt!");
        return DXGI_ERROR_NOT_FOUND;
    }
    return m_pReal->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetParent(REFIID riid, void** ppParent) {
    return m_pReal->GetParent(riid, ppParent);
}

// ============================================================================
// IDXGIDeviceSubObject Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDevice(REFIID riid, void** ppDevice) {
    return m_pReal->GetDevice(riid, ppDevice);
}

// ============================================================================
// IDXGISwapChain Implementation - CORE METHODS
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present(UINT SyncInterval, UINT Flags) {
    std::lock_guard<std::mutex> lock(m_ResourceLock);
    
    // Update Performance Metrics
    if (!m_IsD3D12) {
        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
        DX11_UpdatePerformanceMetrics(us);
    }
    
    // Apply VSync override
    ProcessVSyncOverride(SyncInterval, Flags);
    
    // Draw overlay BEFORE present (SpecialK approach)
    DrawOverlay();
    
    // Call real Present
    return m_pReal->Present(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) {
    return m_pReal->GetBuffer(Buffer, riid, ppSurface);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) {
    return m_pReal->SetFullscreenState(Fullscreen, pTarget);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) {
    return m_pReal->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) {
    return m_pReal->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                                              DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    WrapperLog("DXGI Wrapper: ResizeBuffers called (%ux%u)", Width, Height);
    
    // CRITICAL: Clean up overlay resources BEFORE resize
    CleanupOverlayResources();
    
    // Call real ResizeBuffers
    HRESULT hr = m_pReal->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    
    if (SUCCEEDED(hr)) {
        WrapperLog("DXGI Wrapper: ResizeBuffers succeeded - overlay will recreate on next Present");
        m_OverlayResourcesValid = true;
    } else {
        WrapperLog("DXGI Wrapper: ResizeBuffers failed (hr=0x%08X)", hr);
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
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetDesc1(pDesc);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) {
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetFullscreenDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetHwnd(HWND* pHwnd) {
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetHwnd(pHwnd);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetCoreWindow(REFIID refiid, void** ppUnk) {
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetCoreWindow(refiid, ppUnk);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present1(UINT SyncInterval, UINT PresentFlags,
                                                        const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    std::lock_guard<std::mutex> lock(m_ResourceLock);
    
    // Update Performance Metrics
    if (!m_IsD3D12) {
        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
        DX11_UpdatePerformanceMetrics(us);
    }
    
    // Apply VSync override
    ProcessVSyncOverride(SyncInterval, PresentFlags);
    
    // Draw overlay BEFORE present
    DrawOverlay();
    
    // Call real Present1
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->Present1(SyncInterval, PresentFlags, pPresentParameters);
}

BOOL STDMETHODCALLTYPE CWrapDXGISwapChain::IsTemporaryMonoSupported() {
    if (!m_pReal1) return FALSE;
    return m_pReal1->IsTemporaryMonoSupported();
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) {
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetRestrictToOutput(ppRestrictToOutput);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetBackgroundColor(const DXGI_RGBA* pColor) {
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->SetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetBackgroundColor(DXGI_RGBA* pColor) {
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetRotation(DXGI_MODE_ROTATION Rotation) {
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->SetRotation(Rotation);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetRotation(DXGI_MODE_ROTATION* pRotation) {
    if (!m_pReal1) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetRotation(pRotation);
}

// ============================================================================
// IDXGISwapChain2 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetSourceSize(UINT Width, UINT Height) {
    if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->SetSourceSize(Width, Height);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetSourceSize(UINT* pWidth, UINT* pHeight) {
    if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->GetSourceSize(pWidth, pHeight);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetMaximumFrameLatency(UINT MaxLatency) {
    if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED;
    
    // TODO: Apply prerender limit override here
    WrapperLog("DXGI Wrapper: SetMaximumFrameLatency(%u)", MaxLatency);
    
    return m_pReal2->SetMaximumFrameLatency(MaxLatency);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetMaximumFrameLatency(UINT* pMaxLatency) {
    if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->GetMaximumFrameLatency(pMaxLatency);
}

HANDLE STDMETHODCALLTYPE CWrapDXGISwapChain::GetFrameLatencyWaitableObject() {
    if (!m_pReal2) return nullptr;
    return m_pReal2->GetFrameLatencyWaitableObject();
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) {
    if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->SetMatrixTransform(pMatrix);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) {
    if (!m_pReal2) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->GetMatrixTransform(pMatrix);
}

// ============================================================================
// IDXGISwapChain3 Implementation
// ============================================================================

UINT STDMETHODCALLTYPE CWrapDXGISwapChain::GetCurrentBackBufferIndex() {
    if (!m_pReal3) return 0;
    return m_pReal3->GetCurrentBackBufferIndex();
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                       UINT* pColorSpaceSupport) {
    if (!m_pReal3) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) {
    if (!m_pReal3) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal3->SetColorSpace1(ColorSpace);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                                               DXGI_FORMAT Format, UINT SwapChainFlags,
                                                               const UINT* pCreationNodeMask,
                                                               IUnknown* const* ppPresentQueue) {
    WrapperLog("DXGI Wrapper: ResizeBuffers1 called (%ux%u)", Width, Height);
    
    // CRITICAL: Clean up overlay resources BEFORE resize
    CleanupOverlayResources();
    
    if (!m_pReal3) return DXGI_ERROR_UNSUPPORTED;
    
    HRESULT hr = m_pReal3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags,
                                           pCreationNodeMask, ppPresentQueue);
    
    if (SUCCEEDED(hr)) {
        m_OverlayResourcesValid = true;
    }
    
    return hr;
}

// ============================================================================
// IDXGISwapChain4 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) {
    if (!m_pReal4) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal4->SetHDRMetaData(Type, Size, pMetaData);
}

// ============================================================================
// WrapperStateManager Implementation
// ============================================================================

void WrapperStateManager::RegisterSwapchain(CWrapDXGISwapChain* pWrapper, IDXGISwapChain* pReal) {
    std::lock_guard<std::mutex> lock(m_Lock);
    WrapperLog("WrapperStateManager: Registering SC Real=%p -> Wrapper=%p", pReal, pWrapper);
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_Wrappers[i] == nullptr) {
            m_Wrappers[i] = pWrapper;
            m_RealSwapchains[i] = pReal;
            return;
        }
    }
    WrapperLog("DXGI Wrapper: WARNING - Max swapchains exceeded!");
}

void WrapperStateManager::UnregisterSwapchain(CWrapDXGISwapChain* pWrapper) {
    std::lock_guard<std::mutex> lock(m_Lock);
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_Wrappers[i] == pWrapper) {
            m_Wrappers[i] = nullptr;
            m_RealSwapchains[i] = nullptr;
            return;
        }
    }
}

CWrapDXGISwapChain* WrapperStateManager::FindWrapper(IDXGISwapChain* pReal) {
    std::lock_guard<std::mutex> lock(m_Lock);
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_RealSwapchains[i] == pReal || m_Wrappers[i] == (CWrapDXGISwapChain*)pReal) {
            return m_Wrappers[i];
        }
    }
    return nullptr;
}
