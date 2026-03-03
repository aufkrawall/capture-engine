/**
 * D3D9 Direct3D9 Wrapper Implementation
 */

#include "d3d9_wrap.h"
#include "d3d9_device_wrap.h"
#include "hook_common.h"

// GUID for wrapper identification
static const GUID IID_CWrapDirect3D9 = {0xaabbccdd, 0x1122, 0x3344, {0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc}};

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapDirect3D9::CWrapDirect3D9(IDirect3D9* pReal, bool isEx)
    : m_pReal(pReal),
      m_pRealEx(nullptr),
      m_RefCount(1),
      m_IsEx(isEx) {
    if (pReal) {
        pReal->AddRef();
        if (isEx) {
            pReal->QueryInterface(IID_IDirect3D9Ex, (void**)&m_pRealEx);
        }
    }
    WrapperLog("D3D9 Wrapper: Created (real=%p, ex=%d)", pReal, isEx);
}

CWrapDirect3D9::~CWrapDirect3D9() {
    WrapperLog("D3D9 Wrapper: Destroyed");
    if (m_pRealEx)
        m_pRealEx->Release();
    if (m_pReal)
        m_pReal->Release();
}

// ============================================================================
// IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDirect3D9::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_POINTER;
    *ppvObj = nullptr;

    if (riid == IID_CWrapDirect3D9) {
        AddRef();
        *ppvObj = this;
        return S_OK;
    }

    if (riid == IID_IUnknown || riid == IID_IDirect3D9) {
        AddRef();
        *ppvObj = static_cast<IDirect3D9*>(this);
        return S_OK;
    }

    if (riid == IID_IDirect3D9Ex && m_IsEx) {
        AddRef();
        *ppvObj = static_cast<IDirect3D9Ex*>(this);
        return S_OK;
    }

    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapDirect3D9::AddRef() {
    return InterlockedIncrement(&m_RefCount);
}

ULONG STDMETHODCALLTYPE CWrapDirect3D9::Release() {
    ULONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0)
        delete this;
    return count;
}

// ============================================================================
// Key Override: CreateDevice
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDirect3D9::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                                       DWORD BehaviorFlags,
                                                       D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                       IDirect3DDevice9** ppReturnedDeviceInterface) {
    // If backed by D3D9Ex, use CreateDeviceEx to get a device with native
    // shared handle support for zero-copy capture.
    if (m_pRealEx) {
        WrapperLog("D3D9: CreateDevice -> redirecting to CreateDeviceEx for zero-copy capture");

        // Build fullscreen display mode from present parameters if needed
        D3DDISPLAYMODEEX* pMode = nullptr;
        D3DDISPLAYMODEEX fullscreenMode = {};
        if (pPresentationParameters && !pPresentationParameters->Windowed) {
            fullscreenMode.Size = sizeof(D3DDISPLAYMODEEX);
            fullscreenMode.Width = pPresentationParameters->BackBufferWidth;
            fullscreenMode.Height = pPresentationParameters->BackBufferHeight;
            fullscreenMode.RefreshRate = pPresentationParameters->FullScreen_RefreshRateInHz;
            fullscreenMode.Format = pPresentationParameters->BackBufferFormat;
            fullscreenMode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
            pMode = &fullscreenMode;
        }

        IDirect3DDevice9Ex* pRealDeviceEx = nullptr;
        HRESULT hr = m_pRealEx->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                               pPresentationParameters, pMode, &pRealDeviceEx);

        if (SUCCEEDED(hr) && pRealDeviceEx) {
            CWrapD3D9Device* pWrapper = new CWrapD3D9Device(pRealDeviceEx, true);
            pRealDeviceEx->Release();
            *ppReturnedDeviceInterface = pWrapper;
            WrapperLog("D3D9: Created D3D9Ex device via CreateDeviceEx (zero-copy ready)");
            return S_OK;
        }

        WrapperLog("D3D9: CreateDeviceEx failed (hr=0x%08X), falling back to CreateDevice", hr);
    }

    WrapperLog("D3D9: CreateDevice called (non-Ex path)");

    IDirect3DDevice9* pRealDevice = nullptr;
    HRESULT hr =
        m_pReal->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, &pRealDevice);

    if (SUCCEEDED(hr) && pRealDevice) {
        CWrapD3D9Device* pWrapper = new CWrapD3D9Device(pRealDevice, false);
        pRealDevice->Release();
        *ppReturnedDeviceInterface = pWrapper;
        WrapperLog("D3D9: Created wrapped device (non-Ex)");
        return S_OK;
    }

    *ppReturnedDeviceInterface = pRealDevice;
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDirect3D9::CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                                         DWORD BehaviorFlags,
                                                         D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                         D3DDISPLAYMODEEX* pFullscreenDisplayMode,
                                                         IDirect3DDevice9Ex** ppReturnedDeviceInterface) {
    if (!m_pRealEx)
        return E_NOTIMPL;

    WrapperLog("D3D9Ex: CreateDeviceEx called");

    IDirect3DDevice9Ex* pRealDevice = nullptr;
    HRESULT hr = m_pRealEx->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
                                           pFullscreenDisplayMode, &pRealDevice);

    if (SUCCEEDED(hr) && pRealDevice) {
        CWrapD3D9Device* pWrapper = new CWrapD3D9Device(pRealDevice, true);
        pRealDevice->Release();
        *ppReturnedDeviceInterface = static_cast<IDirect3DDevice9Ex*>(pWrapper);
        WrapperLog("D3D9Ex: Created wrapped device");
        return S_OK;
    }

    *ppReturnedDeviceInterface = pRealDevice;
    return hr;
}

// ============================================================================
// IDirect3D9 - Forward all other methods
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDirect3D9::RegisterSoftwareDevice(void* pInitializeFunction) {
    return m_pReal->RegisterSoftwareDevice(pInitializeFunction);
}

UINT STDMETHODCALLTYPE CWrapDirect3D9::GetAdapterCount() {
    return m_pReal->GetAdapterCount();
}

HRESULT STDMETHODCALLTYPE CWrapDirect3D9::GetAdapterIdentifier(UINT Adapter, DWORD Flags,
                                                               D3DADAPTER_IDENTIFIER9* pIdentifier) {
    return m_pReal->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
}

UINT STDMETHODCALLTYPE CWrapDirect3D9::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) {
    return m_pReal->GetAdapterModeCount(Adapter, Format);
}

HRESULT STDMETHODCALLTYPE CWrapDirect3D9::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode,
                                                           D3DDISPLAYMODE* pMode) {
    return m_pReal->EnumAdapterModes(Adapter, Format, Mode, pMode);
}

HRESULT STDMETHODCALLTYPE CWrapDirect3D9::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) {
    return m_pReal->GetAdapterDisplayMode(Adapter, pMode);
}

HRESULT STDMETHODCALLTYPE CWrapDirect3D9::CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat,
                                                          D3DFORMAT BackBufferFormat, BOOL bWindowed) {
    return m_pReal->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed);
}

HRESULT STDMETHODCALLTYPE CWrapDirect3D9::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType,
                                                            D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType,
                                                            D3DFORMAT CheckFormat) {
    return m_pReal->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
}

HRESULT STDMETHODCALLTYPE CWrapDirect3D9::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType,
                                                                     D3DFORMAT SurfaceFormat, BOOL Windowed,
                                                                     D3DMULTISAMPLE_TYPE MultiSampleType,
                                                                     DWORD* pQualityLevels) {
    return m_pReal->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType,
                                               pQualityLevels);
}

HRESULT STDMETHODCALLTYPE CWrapDirect3D9::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType,
                                                                 D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat,
                                                                 D3DFORMAT DepthStencilFormat) {
    return m_pReal->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
}

HRESULT STDMETHODCALLTYPE CWrapDirect3D9::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType,
                                                                      D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) {
    return m_pReal->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
}

HRESULT STDMETHODCALLTYPE CWrapDirect3D9::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) {
    return m_pReal->GetDeviceCaps(Adapter, DeviceType, pCaps);
}

HMONITOR STDMETHODCALLTYPE CWrapDirect3D9::GetAdapterMonitor(UINT Adapter) {
    return m_pReal->GetAdapterMonitor(Adapter);
}

// ============================================================================
// IDirect3D9Ex Methods
// ============================================================================

UINT STDMETHODCALLTYPE CWrapDirect3D9::GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter) {
    if (!m_pRealEx)
        return 0;
    return m_pRealEx->GetAdapterModeCountEx(Adapter, pFilter);
}

HRESULT STDMETHODCALLTYPE CWrapDirect3D9::EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter,
                                                             UINT Mode, D3DDISPLAYMODEEX* pMode) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    return m_pRealEx->EnumAdapterModesEx(Adapter, pFilter, Mode, pMode);
}

HRESULT STDMETHODCALLTYPE CWrapDirect3D9::GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX* pMode,
                                                                  D3DDISPLAYROTATION* pRotation) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    return m_pRealEx->GetAdapterDisplayModeEx(Adapter, pMode, pRotation);
}

HRESULT STDMETHODCALLTYPE CWrapDirect3D9::GetAdapterLUID(UINT Adapter, LUID* pLUID) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    return m_pRealEx->GetAdapterLUID(Adapter, pLUID);
}
