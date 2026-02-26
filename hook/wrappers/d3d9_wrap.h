/**
 * D3D9 Direct3D9 Wrapper
 *
 * Wraps IDirect3D9 and IDirect3D9Ex to intercept device creation.
 */

#pragma once

#include <d3d9.h>
#include "wrapper_base.h"

/**
 * CWrapDirect3D9 - Wraps IDirect3D9 and IDirect3D9Ex
 */
class CWrapDirect3D9 : public IDirect3D9Ex {
public:
    CWrapDirect3D9(IDirect3D9* pReal, bool isEx = false);
    virtual ~CWrapDirect3D9();

    IDirect3D9* GetReal() const {
        return m_pReal;
    }
    bool IsEx() const {
        return m_IsEx;
    }

    // ========================================================================
    // IUnknown
    // ========================================================================
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ========================================================================
    // IDirect3D9
    // ========================================================================
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override;
    UINT STDMETHODCALLTYPE GetAdapterCount() override;
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags,
                                                   D3DADAPTER_IDENTIFIER9* pIdentifier) override;
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode,
                                               D3DDISPLAYMODE* pMode) override;
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat,
                                              D3DFORMAT BackBufferFormat, BOOL bWindowed) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat,
                                                DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat,
                                                         BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType,
                                                         DWORD* pQualityLevels) override;
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat,
                                                     D3DFORMAT RenderTargetFormat,
                                                     D3DFORMAT DepthStencilFormat) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat,
                                                          D3DFORMAT TargetFormat) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override;
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override;

    // KEY OVERRIDE: CreateDevice
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
                                           D3DPRESENT_PARAMETERS* pPresentationParameters,
                                           IDirect3DDevice9** ppReturnedDeviceInterface) override;

    // ========================================================================
    // IDirect3D9Ex
    // ========================================================================
    UINT STDMETHODCALLTYPE GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter, UINT Mode,
                                                 D3DDISPLAYMODEEX* pMode) override;
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX* pMode,
                                                      D3DDISPLAYROTATION* pRotation) override;
    HRESULT STDMETHODCALLTYPE CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                             DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
                                             D3DDISPLAYMODEEX* pFullscreenDisplayMode,
                                             IDirect3DDevice9Ex** ppReturnedDeviceInterface) override;
    HRESULT STDMETHODCALLTYPE GetAdapterLUID(UINT Adapter, LUID* pLUID) override;

private:
    IDirect3D9* m_pReal;
    IDirect3D9Ex* m_pRealEx;
    LONG m_RefCount;
    bool m_IsEx;
};
