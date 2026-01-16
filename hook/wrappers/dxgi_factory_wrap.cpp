/**
 * DXGI Factory Wrapper Implementation
 */

#include "dxgi_factory_wrap.h"
#include "hook_common.h"

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapDXGIFactory2::CWrapDXGIFactory2(IDXGIFactory2* pReal)
    : m_pReal(pReal)
    , m_pReal3(nullptr)
    , m_pReal4(nullptr)
    , m_pReal5(nullptr)
    , m_pReal6(nullptr)
    , m_pReal7(nullptr)
    , m_RefCount(1)
    , m_Version(2)
{
    if (pReal) {
        pReal->AddRef();
        PromoteInterfaces();
    }
    WrapperLog("DXGI Factory Wrapper: Created (real=%p, version=%d)", pReal, m_Version);
}

CWrapDXGIFactory2::~CWrapDXGIFactory2() {
    WrapperLog("DXGI Factory Wrapper: Destroyed");
    if (m_pReal7) m_pReal7->Release();
    if (m_pReal6) m_pReal6->Release();
    if (m_pReal5) m_pReal5->Release();
    if (m_pReal4) m_pReal4->Release();
    if (m_pReal3) m_pReal3->Release();
    if (m_pReal) m_pReal->Release();
}

void CWrapDXGIFactory2::PromoteInterfaces() {
    if (!m_pReal) return;
    
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal7)))) {
        m_Version = 7;
    } else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal6)))) {
        m_Version = 6;
    } else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal5)))) {
        m_Version = 5;
    } else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4)))) {
        m_Version = 4;
    } else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3)))) {
        m_Version = 3;
    }
}

// ============================================================================
// IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_POINTER;
    
    if (riid == IID_CWrapDXGIFactory) {
        AddRef();
        *ppvObj = this;
        return S_OK;
    }
    
    if (riid == IID_IUnknown || riid == IID_IDXGIObject || 
        riid == IID_IDXGIFactory || riid == IID_IDXGIFactory1 || riid == IID_IDXGIFactory2) {
        AddRef();
        *ppvObj = static_cast<IDXGIFactory2*>(this);
        return S_OK;
    }
    
    if (riid == IID_IDXGIFactory3 && m_Version >= 3) {
        AddRef();
        *ppvObj = static_cast<IDXGIFactory3*>(this);
        return S_OK;
    }
    
    if (riid == IID_IDXGIFactory4 && m_Version >= 4) {
        AddRef();
        *ppvObj = static_cast<IDXGIFactory4*>(this);
        return S_OK;
    }
    
    if (riid == IID_IDXGIFactory5 && m_Version >= 5) {
        AddRef();
        *ppvObj = static_cast<IDXGIFactory5*>(this);
        return S_OK;
    }
    
    if (riid == IID_IDXGIFactory6 && m_Version >= 6) {
        AddRef();
        *ppvObj = static_cast<IDXGIFactory6*>(this);
        return S_OK;
    }
    
    if (riid == IID_IDXGIFactory7 && m_Version >= 7) {
        AddRef();
        *ppvObj = static_cast<IDXGIFactory7*>(this);
        return S_OK;
    }
    
    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapDXGIFactory2::AddRef() {
    return InterlockedIncrement(&m_RefCount);
}

ULONG STDMETHODCALLTYPE CWrapDXGIFactory2::Release() {
    ULONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0) {
        delete this;
    }
    return count;
}

// ============================================================================
// IDXGIObject
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) {
    return m_pReal->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) {
    return m_pReal->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) {
    return m_pReal->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::GetParent(REFIID riid, void** ppParent) {
    return m_pReal->GetParent(riid, ppParent);
}

// ============================================================================
// IDXGIFactory
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) {
    return m_pReal->EnumAdapters(Adapter, ppAdapter);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::MakeWindowAssociation(HWND WindowHandle, UINT Flags) {
    return m_pReal->MakeWindowAssociation(WindowHandle, Flags);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::GetWindowAssociation(HWND* pWindowHandle) {
    return m_pReal->GetWindowAssociation(pWindowHandle);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::CreateSwapChain(IUnknown* pDevice, 
                                                              DXGI_SWAP_CHAIN_DESC* pDesc,
                                                              IDXGISwapChain** ppSwapChain) {
    WrapperLog("DXGI Factory: CreateSwapChain called");
    
    IDXGISwapChain* pRealSwapChain = nullptr;
    HRESULT hr = m_pReal->CreateSwapChain(pDevice, pDesc, &pRealSwapChain);
    
    if (SUCCEEDED(hr) && pRealSwapChain) {
        // Wrap the swapchain
        auto* pWrapper = new CWrapDXGISwapChain(pRealSwapChain, pDevice);
        *ppSwapChain = pWrapper;
        WrapperLog("DXGI Factory: Created wrapped swapchain (wrapper=%p, real=%p)", pWrapper, pRealSwapChain);
        pRealSwapChain->Release(); // Wrapper took ownership
    } else {
        *ppSwapChain = nullptr;
    }
    
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter** ppAdapter) {
    return m_pReal->CreateSoftwareAdapter(Module, ppAdapter);
}

// ============================================================================
// IDXGIFactory1
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) {
    return m_pReal->EnumAdapters1(Adapter, ppAdapter);
}

BOOL STDMETHODCALLTYPE CWrapDXGIFactory2::IsCurrent() {
    return m_pReal->IsCurrent();
}

// ============================================================================
// IDXGIFactory2 - CORE: CreateSwapChainFor* methods
// ============================================================================

BOOL STDMETHODCALLTYPE CWrapDXGIFactory2::IsWindowedStereoEnabled() {
    return m_pReal->IsWindowedStereoEnabled();
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::CreateSwapChainForHwnd(
    IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain) {
    
    WrapperLog("DXGI Factory: CreateSwapChainForHwnd called (%ux%u)", 
               pDesc ? pDesc->Width : 0, pDesc ? pDesc->Height : 0);
    
    IDXGISwapChain1* pRealSwapChain = nullptr;
    HRESULT hr = m_pReal->CreateSwapChainForHwnd(pDevice, hWnd, pDesc, pFullscreenDesc, 
                                                   pRestrictToOutput, &pRealSwapChain);
    
    if (SUCCEEDED(hr) && pRealSwapChain) {
        // Wrap the swapchain
        auto* pWrapper = new CWrapDXGISwapChain(pRealSwapChain, pDevice);
        *ppSwapChain = pWrapper;
        WrapperLog("DXGI Factory: Created wrapped swapchain1 (wrapper=%p, real=%p)", pWrapper, pRealSwapChain);
        pRealSwapChain->Release();
    } else {
        *ppSwapChain = nullptr;
    }
    
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::CreateSwapChainForCoreWindow(
    IUnknown* pDevice, IUnknown* pWindow,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain) {
    
    WrapperLog("DXGI Factory: CreateSwapChainForCoreWindow called");
    
    IDXGISwapChain1* pRealSwapChain = nullptr;
    HRESULT hr = m_pReal->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, 
                                                        pRestrictToOutput, &pRealSwapChain);
    
    if (SUCCEEDED(hr) && pRealSwapChain) {
        auto* pWrapper = new CWrapDXGISwapChain(pRealSwapChain, pDevice);
        *ppSwapChain = pWrapper;
        pRealSwapChain->Release();
    } else {
        *ppSwapChain = nullptr;
    }
    
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::GetSharedResourceAdapterLuid(HANDLE hResource, LUID* pLuid) {
    return m_pReal->GetSharedResourceAdapterLuid(hResource, pLuid);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) {
    return m_pReal->RegisterStereoStatusWindow(WindowHandle, wMsg, pdwCookie);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::RegisterStereoStatusEvent(HANDLE hEvent, DWORD* pdwCookie) {
    return m_pReal->RegisterStereoStatusEvent(hEvent, pdwCookie);
}

void STDMETHODCALLTYPE CWrapDXGIFactory2::UnregisterStereoStatus(DWORD dwCookie) {
    m_pReal->UnregisterStereoStatus(dwCookie);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) {
    return m_pReal->RegisterOcclusionStatusWindow(WindowHandle, wMsg, pdwCookie);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD* pdwCookie) {
    return m_pReal->RegisterOcclusionStatusEvent(hEvent, pdwCookie);
}

void STDMETHODCALLTYPE CWrapDXGIFactory2::UnregisterOcclusionStatus(DWORD dwCookie) {
    m_pReal->UnregisterOcclusionStatus(dwCookie);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::CreateSwapChainForComposition(
    IUnknown* pDevice,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain) {
    
    WrapperLog("DXGI Factory: CreateSwapChainForComposition called");
    
    IDXGISwapChain1* pRealSwapChain = nullptr;
    HRESULT hr = m_pReal->CreateSwapChainForComposition(pDevice, pDesc, pRestrictToOutput, &pRealSwapChain);
    
    if (SUCCEEDED(hr) && pRealSwapChain) {
        auto* pWrapper = new CWrapDXGISwapChain(pRealSwapChain, pDevice);
        *ppSwapChain = pWrapper;
        pRealSwapChain->Release();
    } else {
        *ppSwapChain = nullptr;
    }
    
    return hr;
}

// ============================================================================
// IDXGIFactory3
// ============================================================================

UINT STDMETHODCALLTYPE CWrapDXGIFactory2::GetCreationFlags() {
    if (!m_pReal3) return 0;
    return m_pReal3->GetCreationFlags();
}

// ============================================================================
// IDXGIFactory4
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void** ppvAdapter) {
    if (!m_pReal4) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal4->EnumAdapterByLuid(AdapterLuid, riid, ppvAdapter);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::EnumWarpAdapter(REFIID riid, void** ppvAdapter) {
    if (!m_pReal4) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal4->EnumWarpAdapter(riid, ppvAdapter);
}

// ============================================================================
// IDXGIFactory5
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::CheckFeatureSupport(DXGI_FEATURE Feature, 
                                                                   void* pFeatureSupportData, 
                                                                   UINT FeatureSupportDataSize) {
    if (!m_pReal5) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal5->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);
}

// ============================================================================
// IDXGIFactory6
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::EnumAdapterByGpuPreference(UINT Adapter, 
                                                                          DXGI_GPU_PREFERENCE GpuPreference, 
                                                                          REFIID riid, void** ppvAdapter) {
    if (!m_pReal6) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal6->EnumAdapterByGpuPreference(Adapter, GpuPreference, riid, ppvAdapter);
}

// ============================================================================
// IDXGIFactory7
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::RegisterAdaptersChangedEvent(HANDLE hEvent, DWORD* pdwCookie) {
    if (!m_pReal7) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal7->RegisterAdaptersChangedEvent(hEvent, pdwCookie);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::UnregisterAdaptersChangedEvent(DWORD dwCookie) {
    if (!m_pReal7) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal7->UnregisterAdaptersChangedEvent(dwCookie);
}
