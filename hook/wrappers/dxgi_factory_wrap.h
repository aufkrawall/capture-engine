/**
 * DXGI Factory Wrapper
 *
 * Intercepts CreateSwapChain* calls to return our wrapped swapchains.
 */

#pragma once

#include <dxgi1_6.h>
#include "dxgi_adapter_wrap.h"
#include "dxgi_swapchain_wrap.h"
#include "wrapper_base.h"

/**
 * CWrapDXGIFactory2 - Wraps IDXGIFactory2 through IDXGIFactory7
 *
 * The key role is intercepting CreateSwapChain* methods to return
 * our wrapped swapchains instead of the real ones.
 */
class CWrapDXGIFactory2 : public IDXGIFactory7 {
public:
    CWrapDXGIFactory2(IDXGIFactory2* pReal);
    virtual ~CWrapDXGIFactory2();

    IDXGIFactory2* GetReal() const { return m_pReal; }

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
    // IDXGIFactory
    // ========================================================================
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) override;
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* pWindowHandle) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                              IDXGISwapChain** ppSwapChain) override;
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter** ppAdapter) override;

    // ========================================================================
    // IDXGIFactory1
    // ========================================================================
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) override;
    BOOL STDMETHODCALLTYPE IsCurrent() override;

    // ========================================================================
    // IDXGIFactory2 - CORE: CreateSwapChainFor* methods
    // ========================================================================
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                                     IDXGIOutput* pRestrictToOutput,
                                                     IDXGISwapChain1** ppSwapChain) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* pDevice, IUnknown* pWindow,
                                                           const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                           IDXGIOutput* pRestrictToOutput,
                                                           IDXGISwapChain1** ppSwapChain) override;
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID* pLuid) override;
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) override;
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override;
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) override;
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) override;
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override;
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                            IDXGIOutput* pRestrictToOutput,
                                                            IDXGISwapChain1** ppSwapChain) override;

    // ========================================================================
    // IDXGIFactory3
    // ========================================================================
    UINT STDMETHODCALLTYPE GetCreationFlags() override;

    // ========================================================================
    // IDXGIFactory4
    // ========================================================================
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void** ppvAdapter) override;
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** ppvAdapter) override;

    // ========================================================================
    // IDXGIFactory5
    // ========================================================================
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE Feature, void* pFeatureSupportData,
                                                  UINT FeatureSupportDataSize) override;

    // ========================================================================
    // IDXGIFactory6
    // ========================================================================
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference, REFIID riid,
                                                         void** ppvAdapter) override;

    // ========================================================================
    // IDXGIFactory7
    // ========================================================================
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE hEvent, DWORD* pdwCookie) override;
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD dwCookie) override;

private:
    IDXGIFactory2* m_pReal;
    IDXGIFactory3* m_pReal3;
    IDXGIFactory4* m_pReal4;
    IDXGIFactory5* m_pReal5;
    IDXGIFactory6* m_pReal6;
    IDXGIFactory7* m_pReal7;
    LONG m_RefCount;
    int m_Version;

    void PromoteInterfaces();
};
