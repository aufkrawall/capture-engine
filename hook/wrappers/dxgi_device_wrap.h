/**
 * DXGI Device Wrapper
 *
 * Ensures QueryInterface(IDXGIDevice*) keeps the wrapper chain intact so
 * adapter->GetParent() still returns our wrapped DXGI factory.
 */

#pragma once

#include <dxgi1_6.h>

class CWrapDXGIDevice : public IDXGIDevice4 {
public:
    explicit CWrapDXGIDevice(IDXGIDevice* pReal);
    virtual ~CWrapDXGIDevice();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;

    HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter** pAdapter) override;
    HRESULT STDMETHODCALLTYPE CreateSurface(const DXGI_SURFACE_DESC* pDesc, UINT NumSurfaces, DXGI_USAGE Usage,
                                            const DXGI_SHARED_RESOURCE* pSharedResource,
                                            IDXGISurface** ppSurface) override;
    HRESULT STDMETHODCALLTYPE QueryResourceResidency(IUnknown* const* ppResources, DXGI_RESIDENCY* pResidencyStatus,
                                                     UINT NumResources) override;
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT Priority) override;
    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* pPriority) override;

    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override;

    HRESULT STDMETHODCALLTYPE OfferResources(UINT NumResources, IDXGIResource* const* ppResources,
                                             DXGI_OFFER_RESOURCE_PRIORITY Priority) override;
    HRESULT STDMETHODCALLTYPE ReclaimResources(UINT NumResources, IDXGIResource* const* ppResources,
                                               BOOL* pDiscarded) override;
    HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE hEvent) override;

    void STDMETHODCALLTYPE Trim() override;

    HRESULT STDMETHODCALLTYPE OfferResources1(UINT NumResources, IDXGIResource* const* ppResources,
                                              DXGI_OFFER_RESOURCE_PRIORITY Priority, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE ReclaimResources1(UINT NumResources, IDXGIResource* const* ppResources,
                                                DXGI_RECLAIM_RESOURCE_RESULTS* pResults) override;

private:
    void PromoteInterfaces();
    HRESULT WrapAdapter(REFIID riid, void** ppvObj);

    IDXGIDevice* m_pReal = nullptr;
    IDXGIDevice1* m_pReal1 = nullptr;
    IDXGIDevice2* m_pReal2 = nullptr;
    IDXGIDevice3* m_pReal3 = nullptr;
    IDXGIDevice4* m_pReal4 = nullptr;
    LONG m_RefCount = 1;
    int m_Version = 0;
};
