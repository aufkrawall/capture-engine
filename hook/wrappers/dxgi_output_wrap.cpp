/**
 * DXGI Output Wrapper Implementation
 */

#include "dxgi_output_wrap.h"
#include "dxgi_adapter_wrap.h"
#include "hook_common.h"

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapDXGIOutput::CWrapDXGIOutput(IDXGIOutput* pReal, CWrapDXGIAdapter* pAdapterWrapper)
    : m_pReal(pReal),
      m_pReal1(nullptr),
      m_pReal2(nullptr),
      m_pReal3(nullptr),
      m_pReal4(nullptr),
      m_pReal5(nullptr),
      m_pReal6(nullptr),
      m_pParentAdapter(pAdapterWrapper),
      m_RefCount(1),
      m_Version(0) {
    if (pReal) {
        pReal->AddRef();
        PromoteInterfaces();
    }

    // Hold reference to parent adapter wrapper
    if (m_pParentAdapter) {
        m_pParentAdapter->AddRef();
    }

    // WrapperLog("DXGI Output Wrapper: Created (real=%p)", pReal);
}

CWrapDXGIOutput::~CWrapDXGIOutput() {
    // WrapperLog("DXGI Output Wrapper: Destroyed");

    if (m_pParentAdapter)
        m_pParentAdapter->Release();

    if (m_pReal6)
        m_pReal6->Release();
    if (m_pReal5)
        m_pReal5->Release();
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

void CWrapDXGIOutput::PromoteInterfaces() {
    if (!m_pReal)
        return;

    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal6)))) {
        m_Version = 6;
    } else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal5)))) {
        m_Version = 5;
    } else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4)))) {
        m_Version = 4;
    } else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3)))) {
        m_Version = 3;
    } else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2)))) {
        m_Version = 2;
    } else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1)))) {
        m_Version = 1;
    }
}

// ============================================================================
// IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_POINTER;

    // Return ourselves for DXGI Output interfaces
    if (riid == IID_IUnknown || riid == IID_IDXGIObject || riid == IID_IDXGIOutput) {
        AddRef();
        *ppvObj = static_cast<IDXGIOutput*>(this);
        return S_OK;
    }

    if (riid == IID_IDXGIOutput1 && m_Version >= 1) {
        AddRef();
        *ppvObj = static_cast<IDXGIOutput1*>(this);
        return S_OK;
    }

    if (riid == IID_IDXGIOutput2 && m_Version >= 2) {
        AddRef();
        *ppvObj = static_cast<IDXGIOutput2*>(this);
        return S_OK;
    }

    if (riid == IID_IDXGIOutput3 && m_Version >= 3) {
        AddRef();
        *ppvObj = static_cast<IDXGIOutput3*>(this);
        return S_OK;
    }

    if (riid == IID_IDXGIOutput4 && m_Version >= 4) {
        AddRef();
        *ppvObj = static_cast<IDXGIOutput4*>(this);
        return S_OK;
    }

    if (riid == IID_IDXGIOutput5 && m_Version >= 5) {
        AddRef();
        *ppvObj = static_cast<IDXGIOutput5*>(this);
        return S_OK;
    }

    if (riid == IID_IDXGIOutput6 && m_Version >= 6) {
        AddRef();
        *ppvObj = static_cast<IDXGIOutput6*>(this);
        return S_OK;
    }

    // WrapperLog("DXGI Output Wrapper: QueryInterface for unknown IID (First
    // DWORD: %08X)", riid.Data1);

    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapDXGIOutput::AddRef() {
    return InterlockedIncrement(&m_RefCount);
}

ULONG STDMETHODCALLTYPE CWrapDXGIOutput::Release() {
    ULONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0) {
        delete this;
    }
    return count;
}

// ============================================================================
// IDXGIObject
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) {
    return m_pReal->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) {
    return m_pReal->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) {
    return m_pReal->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::GetParent(REFIID riid, void** ppParent) {
    // CRITICAL FIX: Return our Adapter Wrapper instead of the Real Adapter
    if (m_pParentAdapter) {
        return m_pParentAdapter->QueryInterface(riid, ppParent);
    }
    return m_pReal->GetParent(riid, ppParent);
}

// ============================================================================
// IDXGIOutput
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::GetDesc(DXGI_OUTPUT_DESC* pDesc) {
    return m_pReal->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::GetDisplayModeList(DXGI_FORMAT EnumFormat, UINT Flags, UINT* pNumModes,
                                                              DXGI_MODE_DESC* pDesc) {
    return m_pReal->GetDisplayModeList(EnumFormat, Flags, pNumModes, pDesc);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::FindClosestMatchingMode(const DXGI_MODE_DESC* pModeToMatch,
                                                                   DXGI_MODE_DESC* pClosestMatch,
                                                                   IUnknown* pConcernedDevice) {
    // NOTE: pConcernedDevice might need wrapping/unwrapping if it's our device
    // wrapper, but usually null or real. If the game passes our wrapped device,
    // m_pReal->FindClosestMatchingMode might fail if it expects a real device.
    // Ideally we should unwrap pConcernedDevice. But for now pass through.
    return m_pReal->FindClosestMatchingMode(pModeToMatch, pClosestMatch, pConcernedDevice);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::WaitForVBlank() {
    return m_pReal->WaitForVBlank();
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::TakeOwnership(IUnknown* pDevice, BOOL Exclusive) {
    return m_pReal->TakeOwnership(pDevice, Exclusive);
}

void STDMETHODCALLTYPE CWrapDXGIOutput::ReleaseOwnership() {
    m_pReal->ReleaseOwnership();
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES* pGammaCaps) {
    return m_pReal->GetGammaControlCapabilities(pGammaCaps);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::SetGammaControl(const DXGI_GAMMA_CONTROL* pArray) {
    return m_pReal->SetGammaControl(pArray);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::GetGammaControl(DXGI_GAMMA_CONTROL* pArray) {
    return m_pReal->GetGammaControl(pArray);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::SetDisplaySurface(IDXGISurface* pScanoutSurface) {
    return m_pReal->SetDisplaySurface(pScanoutSurface);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::GetDisplaySurfaceData(IDXGISurface* pDestination) {
    return m_pReal->GetDisplaySurfaceData(pDestination);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) {
    return m_pReal->GetFrameStatistics(pStats);
}

// ============================================================================
// IDXGIOutput1
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::GetDisplayModeList1(DXGI_FORMAT EnumFormat, UINT Flags, UINT* pNumModes,
                                                               DXGI_MODE_DESC1* pDesc) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetDisplayModeList1(EnumFormat, Flags, pNumModes, pDesc);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::FindClosestMatchingMode1(const DXGI_MODE_DESC1* pModeToMatch,
                                                                    DXGI_MODE_DESC1* pClosestMatch,
                                                                    IUnknown* pConcernedDevice) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->FindClosestMatchingMode1(pModeToMatch, pClosestMatch, pConcernedDevice);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::GetDisplaySurfaceData1(IDXGIResource* pDestination) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetDisplaySurfaceData1(pDestination);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::DuplicateOutput(IUnknown* pDevice,
                                                           IDXGIOutputDuplication** ppOutputDuplication) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->DuplicateOutput(pDevice, ppOutputDuplication);
}

// ============================================================================
// IDXGIOutput2
// ============================================================================

BOOL STDMETHODCALLTYPE CWrapDXGIOutput::SupportsOverlays() {
    if (!m_pReal2)
        return FALSE;
    return m_pReal2->SupportsOverlays();
}

// ============================================================================
// IDXGIOutput3
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::CheckOverlaySupport(DXGI_FORMAT EnumFormat, IUnknown* pConcernedDevice,
                                                               UINT* pFlags) {
    if (!m_pReal3)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal3->CheckOverlaySupport(EnumFormat, pConcernedDevice, pFlags);
}

// ============================================================================
// IDXGIOutput4
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::CheckOverlayColorSpaceSupport(DXGI_FORMAT Format,
                                                                         DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                         IUnknown* pConcernedDevice, UINT* pFlags) {
    if (!m_pReal4)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal4->CheckOverlayColorSpaceSupport(Format, ColorSpace, pConcernedDevice, pFlags);
}

// ============================================================================
// IDXGIOutput5
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::DuplicateOutput1(IUnknown* pDevice, UINT Flags, UINT SupportedFormatsCount,
                                                            const DXGI_FORMAT* pSupportedFormats,
                                                            IDXGIOutputDuplication** ppOutputDuplication) {
    if (!m_pReal5)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal5->DuplicateOutput1(pDevice, Flags, SupportedFormatsCount, pSupportedFormats, ppOutputDuplication);
}

// ============================================================================
// IDXGIOutput6
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::GetDesc1(DXGI_OUTPUT_DESC1* pDesc) {
    if (!m_pReal6)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal6->GetDesc1(pDesc);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIOutput::CheckHardwareCompositionSupport(UINT* pFlags) {
    if (!m_pReal6)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal6->CheckHardwareCompositionSupport(pFlags);
}
