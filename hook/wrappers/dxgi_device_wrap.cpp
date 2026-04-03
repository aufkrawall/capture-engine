/**
 * DXGI Device Wrapper Implementation
 */

#include "dxgi_device_wrap.h"

#include "dxgi_adapter_wrap.h"
#include "dxgi_factory_wrap.h"
#include "hook_common.h"

CWrapDXGIDevice::CWrapDXGIDevice(IDXGIDevice* pReal) : m_pReal(pReal) {
    if (m_pReal) {
        m_pReal->AddRef();
        PromoteInterfaces();
    }
    WrapperLog("DXGI Device Wrapper: Created (real=%p, version=%d)", pReal, m_Version);
}

CWrapDXGIDevice::~CWrapDXGIDevice() {
    WrapperLog("DXGI Device Wrapper: Destroyed");
    if (m_pReal4)
        m_pReal4->Release();
    if (m_pReal3)
        m_pReal3->Release();
    if (m_pReal2)
        m_pReal2->Release();
    if (m_pReal1)
        m_pReal1->Release();
    if (m_pReal)
        m_pReal->Release();
}

void CWrapDXGIDevice::PromoteInterfaces() {
    if (!m_pReal) {
        return;
    }

    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4))))
        m_Version = 4;

    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3))) && m_Version < 3)
        m_Version = 3;
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2))) && m_Version < 2)
        m_Version = 2;
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1))) && m_Version < 1)
        m_Version = 1;
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_POINTER;
    *ppvObj = nullptr;

    if (riid == IID_IUnknown || riid == IID_IDXGIObject || riid == IID_IDXGIDevice) {
        AddRef();
        *ppvObj = static_cast<IDXGIDevice*>(this);
        return S_OK;
    }

    if (riid == IID_IDXGIDevice1 && m_Version >= 1) {
        AddRef();
        *ppvObj = static_cast<IDXGIDevice1*>(this);
        return S_OK;
    }
    if (riid == IID_IDXGIDevice2 && m_Version >= 2) {
        AddRef();
        *ppvObj = static_cast<IDXGIDevice2*>(this);
        return S_OK;
    }
    if (riid == IID_IDXGIDevice3 && m_Version >= 3) {
        AddRef();
        *ppvObj = static_cast<IDXGIDevice3*>(this);
        return S_OK;
    }
    if (riid == IID_IDXGIDevice4 && m_Version >= 4) {
        AddRef();
        *ppvObj = static_cast<IDXGIDevice4*>(this);
        return S_OK;
    }

    return m_pReal ? m_pReal->QueryInterface(riid, ppvObj) : E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE CWrapDXGIDevice::AddRef() {
    return InterlockedIncrement(&m_RefCount);
}

ULONG STDMETHODCALLTYPE CWrapDXGIDevice::Release() {
    ULONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0) {
        delete this;
    }
    return count;
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) {
    return m_pReal->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) {
    return m_pReal->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) {
    return m_pReal->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::GetParent(REFIID riid, void** ppParent) {
    return WrapAdapter(riid, ppParent);
}

HRESULT CWrapDXGIDevice::WrapAdapter(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_POINTER;
    *ppvObj = nullptr;

    IDXGIAdapter* pRealAdapter = nullptr;
    HRESULT hr = m_pReal->GetAdapter(&pRealAdapter);
    if (FAILED(hr) || !pRealAdapter) {
        return hr;
    }

    CWrapDXGIFactory2* pFactoryWrapper = nullptr;
    IDXGIFactory2* pRealFactory2 = nullptr;
    if (SUCCEEDED(pRealAdapter->GetParent(IID_PPV_ARGS(&pRealFactory2))) && pRealFactory2) {
        pFactoryWrapper = new CWrapDXGIFactory2(pRealFactory2);
        pRealFactory2->Release();
    }

    auto* pWrappedAdapter = new CWrapDXGIAdapter(pRealAdapter, pFactoryWrapper);
    pRealAdapter->Release();
    if (pFactoryWrapper) {
        pFactoryWrapper->Release();
    }

    hr = pWrappedAdapter->QueryInterface(riid, ppvObj);
    pWrappedAdapter->Release();
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::GetAdapter(IDXGIAdapter** pAdapter) {
    return WrapAdapter(IID_IDXGIAdapter, reinterpret_cast<void**>(pAdapter));
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::CreateSurface(const DXGI_SURFACE_DESC* pDesc, UINT NumSurfaces, DXGI_USAGE Usage,
                                                         const DXGI_SHARED_RESOURCE* pSharedResource,
                                                         IDXGISurface** ppSurface) {
    return m_pReal->CreateSurface(pDesc, NumSurfaces, Usage, pSharedResource, ppSurface);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::QueryResourceResidency(IUnknown* const* ppResources,
                                                                  DXGI_RESIDENCY* pResidencyStatus,
                                                                  UINT NumResources) {
    return m_pReal->QueryResourceResidency(ppResources, pResidencyStatus, NumResources);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::SetGPUThreadPriority(INT Priority) {
    return m_pReal->SetGPUThreadPriority(Priority);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::GetGPUThreadPriority(INT* pPriority) {
    return m_pReal->GetGPUThreadPriority(pPriority);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::SetMaximumFrameLatency(UINT MaxLatency) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->SetMaximumFrameLatency(MaxLatency);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::GetMaximumFrameLatency(UINT* pMaxLatency) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetMaximumFrameLatency(pMaxLatency);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::OfferResources(UINT NumResources, IDXGIResource* const* ppResources,
                                                          DXGI_OFFER_RESOURCE_PRIORITY Priority) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->OfferResources(NumResources, ppResources, Priority);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::ReclaimResources(UINT NumResources, IDXGIResource* const* ppResources,
                                                            BOOL* pDiscarded) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->ReclaimResources(NumResources, ppResources, pDiscarded);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::EnqueueSetEvent(HANDLE hEvent) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->EnqueueSetEvent(hEvent);
}

void STDMETHODCALLTYPE CWrapDXGIDevice::Trim() {
    if (m_pReal3) {
        m_pReal3->Trim();
    }
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::OfferResources1(UINT NumResources, IDXGIResource* const* ppResources,
                                                           DXGI_OFFER_RESOURCE_PRIORITY Priority, UINT Flags) {
    if (!m_pReal4)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal4->OfferResources1(NumResources, ppResources, Priority, Flags);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIDevice::ReclaimResources1(UINT NumResources, IDXGIResource* const* ppResources,
                                                             DXGI_RECLAIM_RESOURCE_RESULTS* pResults) {
    if (!m_pReal4)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal4->ReclaimResources1(NumResources, ppResources, pResults);
}
