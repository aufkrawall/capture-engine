/**
 * DXGI Factory Wrapper Implementation
 */

#include "dxgi_factory_wrap.h"
#include "dxgi_factory_wrap.h"
#include "hook_common.h"
#include <objbase.h>

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
    
    // Query ALL interfaces sequentially to ensure all supported pointers are populated.
    // m_Version will be set to the highest one found.
    
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3)))) {
        m_Version = 3;
    }
    
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4)))) {
        m_Version = 4;
    }
    
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal5)))) {
        m_Version = 5;
    }
    
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal6)))) {
        m_Version = 6;
    }
    
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal7)))) {
        m_Version = 7;
    }
}

// ============================================================================
// IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_POINTER;

    // Log all QI calls
    {
        LPOLESTR pStr;
        StringFromIID(riid, &pStr);
        WrapperLog("DXGI Factory: QueryInterface(%ls) Entry", pStr);
        CoTaskMemFree(pStr);
    }

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
    
    // Check for IDXGIFactory4 to 7 via string comparison as fallback
    // This handles cases where IID constants might not be linked correctly
    
    // IDXGIFactory3: {25483823-CD46-4C7D-86CA-47AA95B837BD}
    if ((riid == IID_IDXGIFactory3 || IsEqualGUID(riid, {0x25483823,0xcd46,0x4c7d,{0x86,0xca,0x47,0xaa,0x95,0xb8,0x37,0xbd}})) && m_Version >= 3) {
        WrapperLog("DXGI Factory: QI Matched IDXGIFactory3");
        AddRef();
        *ppvObj = static_cast<IDXGIFactory3*>(this);
        return S_OK;
    }
    
    // IDXGIFactory4: {1BC6EA02-EF36-464F-BF0C-21CA39E5168A}
    if ((riid == IID_IDXGIFactory4 || IsEqualGUID(riid, {0x1bc6ea02,0xef36,0x464f,{0xbf,0x0c,0x21,0xca,0x39,0xe5,0x16,0x8a}})) && m_Version >= 4) {
        WrapperLog("DXGI Factory: QI Matched IDXGIFactory4");
        AddRef();
        *ppvObj = static_cast<IDXGIFactory4*>(this);
        return S_OK;
    }
    
    // IDXGIFactory5: {7632E1F5-EE65-4DCA-87FD-84CD75F8838D}
    if ((riid == IID_IDXGIFactory5 || IsEqualGUID(riid, {0x7632e1f5,0xee65,0x4dca,{0x87,0xfd,0x84,0xcd,0x75,0xf8,0x83,0x8d}})) && m_Version >= 5) {
        WrapperLog("DXGI Factory: QI Matched IDXGIFactory5");
        AddRef();
        *ppvObj = static_cast<IDXGIFactory5*>(this);
        return S_OK;
    }
    
    // IDXGIFactory6: {C1B6694F-FF09-44A9-B03C-77900A0A1D17}
    if ((riid == IID_IDXGIFactory6 || IsEqualGUID(riid, {0xc1b6694f,0xff09,0x44a9,{0xb0,0x3c,0x77,0x90,0x0a,0x0a,0x1d,0x17}})) && m_Version >= 6) {
        WrapperLog("DXGI Factory: QI Matched IDXGIFactory6");
        AddRef();
        *ppvObj = static_cast<IDXGIFactory6*>(this);
        return S_OK;
    }
    
    // IDXGIFactory7: {A4966EED-0518-4EB7-9215-32955251310C}
    if ((riid == IID_IDXGIFactory7 || IsEqualGUID(riid, {0xa4966eed,0x0518,0x4eb7,{0x92,0x15,0x32,0x95,0x52,0x51,0x31,0x0c}})) && m_Version >= 7) {
        WrapperLog("DXGI Factory: QI Matched IDXGIFactory7");
        AddRef();
        *ppvObj = static_cast<IDXGIFactory7*>(this);
        return S_OK;
    }

    // Log the unknown IID for debugging (Before forwarding)
    WrapperLog("DXGI Factory: QueryInterface Fallthrough - Forwarding to Real");
    
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
    // WrapperLog("DXGI Factory: GetPrivateData");
    return m_pReal->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::GetParent(REFIID riid, void** ppParent) {
    WrapperLog("DXGI Factory: GetParent");
    return m_pReal->GetParent(riid, ppParent);
}

// ============================================================================
// IDXGIFactory
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) {
    WrapperLog("DXGI Factory: EnumAdapters(%u) Enter", Adapter);
    IDXGIAdapter* pRealAdapter = nullptr;
    HRESULT hr = m_pReal->EnumAdapters(Adapter, &pRealAdapter);
    
    if (SUCCEEDED(hr) && pRealAdapter) {
        *ppAdapter = new CWrapDXGIAdapter(pRealAdapter, this);
        pRealAdapter->Release(); // Wrapper took ownership
        WrapperLog("DXGI Factory: EnumAdapters(%u) -> Wrapped=%p (Real=%p)", Adapter, *ppAdapter, pRealAdapter);
    } else {
        *ppAdapter = nullptr;
        WrapperLog("DXGI Factory: EnumAdapters(%u) failed or end of list (hr=%08X)", Adapter, hr);
    }
    return hr;
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
    WrapperLog("DXGI Factory: EnumAdapters1(%u) Enter", Adapter);
    IDXGIAdapter1* pRealAdapter = nullptr;
    HRESULT hr = m_pReal->EnumAdapters1(Adapter, &pRealAdapter);
    
    if (SUCCEEDED(hr) && pRealAdapter) {
        *ppAdapter = (IDXGIAdapter1*)new CWrapDXGIAdapter(pRealAdapter, this);
        pRealAdapter->Release();
        WrapperLog("DXGI Factory: EnumAdapters1(%u) -> Wrapped=%p (Real=%p)", Adapter, *ppAdapter, pRealAdapter);
    } else {
        *ppAdapter = nullptr;
    }
    return hr;
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
    
    // We get the interface requested by the user, then we need to wrap it.
    // However, CWrapDXGIAdapter implements IDXGIAdapter4 which inherits everything.
    // So we can get IDXGIAdapter from real factory, wrap it, then QI the wrapper for the user.
    
    // BUT EnumAdapterByLuid matches specific LUID.
    // Let's call real EnumAdapterByLuid asking for IUnknown or IDXGIAdapter
    WrapperLog("DXGI Factory: EnumAdapterByLuid Enter");
    
    if (!m_pReal4) {
        WrapperLog("DXGI Factory: EnumAdapterByLuid failed (m_pReal4 is NULL)");
        return DXGI_ERROR_UNSUPPORTED;
    }
    
    IUnknown* pRealUnk = nullptr;
    HRESULT hr = m_pReal4->EnumAdapterByLuid(AdapterLuid, IID_IDXGIAdapter, (void**)&pRealUnk);
    
    if (SUCCEEDED(hr) && pRealUnk) {
        IDXGIAdapter* pRealAdapter = static_cast<IDXGIAdapter*>(pRealUnk);
        CWrapDXGIAdapter* pWrapper = new CWrapDXGIAdapter(pRealAdapter, this);
        
        hr = pWrapper->QueryInterface(riid, ppvAdapter);
        pWrapper->Release(); // Release our ref, user has hers via QI
        pRealAdapter->Release(); // Wrapper took ownership
        
        WrapperLog("DXGI Factory: EnumAdapterByLuid -> Wrapped=%p", *ppvAdapter);
        return hr;
    }
    WrapperLog("DXGI Factory: EnumAdapterByLuid failed (hr=%08X)", hr);
    return hr;
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
    WrapperLog("DXGI Factory: CheckFeatureSupport(%d) Enter", Feature);
    if (!m_pReal5) return DXGI_ERROR_UNSUPPORTED;
    return m_pReal5->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);
}

// ============================================================================
// IDXGIFactory6
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::EnumAdapterByGpuPreference(UINT Adapter, 
                                                                          DXGI_GPU_PREFERENCE GpuPreference, 
                                                                          REFIID riid, void** ppvAdapter) {
    WrapperLog("DXGI Factory: EnumAdapterByGpuPreference(%u) Enter", Adapter);
    
    if (!m_pReal6) {
        WrapperLog("DXGI Factory: EnumAdapterByGpuPreference failed (m_pReal6 is NULL)");
        return DXGI_ERROR_UNSUPPORTED;
    }
    
    IUnknown* pRealUnk = nullptr;
    HRESULT hr = m_pReal6->EnumAdapterByGpuPreference(Adapter, GpuPreference, IID_IDXGIAdapter, (void**)&pRealUnk);
    
    if (SUCCEEDED(hr) && pRealUnk) {
        IDXGIAdapter* pRealAdapter = static_cast<IDXGIAdapter*>(pRealUnk);
        CWrapDXGIAdapter* pWrapper = new CWrapDXGIAdapter(pRealAdapter, this);
        
        hr = pWrapper->QueryInterface(riid, ppvAdapter);
        pWrapper->Release();
        pRealAdapter->Release();
        
        WrapperLog("DXGI Factory: EnumAdapterByGpuPreference -> Wrapped=%p", *ppvAdapter);
        return hr;
    }
    WrapperLog("DXGI Factory: EnumAdapterByGpuPreference failed (hr=%08X)", hr);
    return hr;
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
