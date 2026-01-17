/**
 * DXGI Adapter Wrapper
 * 
 * Wraps IDXGIAdapter to correct GetParent() identity.
 */

#pragma once

#include "wrapper_base.h"
#include <dxgi1_6.h>

// Forward declaration of Factory Wrapper to start the parent chain
class CWrapDXGIFactory2;

/**
 * CWrapDXGIAdapter - Wraps IDXGIAdapter4
 * 
 * Ensures that GetParent() returns our DXGI Factory Wrapper
 * instead of the Real DXGI Factory.
 */
class CWrapDXGIAdapter : public IDXGIAdapter4, public ICWrapDXGIAdapter
{
public:
    CWrapDXGIAdapter(IDXGIAdapter* pReal, CWrapDXGIFactory2* pFactoryWrapper);
    CWrapDXGIAdapter(IDXGIAdapter1* pReal, CWrapDXGIFactory2* pFactoryWrapper);
    virtual ~CWrapDXGIAdapter();
    
    IDXGIAdapter* GetReal() override { return m_pReal; }
    
    // ========================================================================
    // IUnknown
    // ========================================================================
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    
    // ========================================================================
    // IDXGIObject
    // ========================================================================
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;
    
    // ========================================================================
    // IDXGIAdapter
    // ========================================================================
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output, IDXGIOutput** ppOutput) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID InterfaceName, LARGE_INTEGER* pUMDVersion) override;
    
    // ========================================================================
    // IDXGIAdapter1
    // ========================================================================
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* pDesc) override;
    
    // ========================================================================
    // IDXGIAdapter2
    // ========================================================================
    HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2* pDesc) override;
    
    // ========================================================================
    // IDXGIAdapter3
    // ========================================================================
    HRESULT STDMETHODCALLTYPE RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override;
    void STDMETHODCALLTYPE UnregisterHardwareContentProtectionTeardownStatus(DWORD dwCookie) override;
    HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup, DXGI_QUERY_VIDEO_MEMORY_INFO* pVideoMemoryInfo) override;
    HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup, UINT64 Reservation) override;
    HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(HANDLE hEvent, DWORD* pdwCookie) override;
    void STDMETHODCALLTYPE UnregisterVideoMemoryBudgetChangeNotification(DWORD dwCookie) override;
    
    // ========================================================================
    // IDXGIAdapter4
    // ========================================================================
    HRESULT STDMETHODCALLTYPE GetDesc3(DXGI_ADAPTER_DESC3* pDesc) override;

private:
    IDXGIAdapter* m_pReal;
    IDXGIAdapter1* m_pReal1;
    IDXGIAdapter2* m_pReal2;
    IDXGIAdapter3* m_pReal3;
    IDXGIAdapter4* m_pReal4;
    
    // Parent Factory Wrapper
    CWrapDXGIFactory2* m_pParentFactory;
    
    LONG m_RefCount;
    int m_Version;
    
    void PromoteInterfaces();
};
