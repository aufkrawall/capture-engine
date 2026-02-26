/**
 * DXGI Output Wrapper
 *
 * Wraps IDXGIOutput to correct GetParent() identity.
 */

#pragma once

#include <dxgi1_6.h>
#include "wrapper_base.h"

// Forward declaration of Adapter Wrapper to start the parent chain
class CWrapDXGIAdapter;

/**
 * CWrapDXGIOutput - Wraps IDXGIOutput6
 *
 * Ensures that GetParent() returns our DXGI Adapter Wrapper
 * instead of the Real DXGI Adapter.
 */
class CWrapDXGIOutput : public IDXGIOutput6 {
public:
    CWrapDXGIOutput(IDXGIOutput* pReal, CWrapDXGIAdapter* pAdapterWrapper);
    virtual ~CWrapDXGIOutput();

    IDXGIOutput* GetReal() const {
        return m_pReal;
    }

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
    // IDXGIOutput
    // ========================================================================
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetDisplayModeList(DXGI_FORMAT EnumFormat, UINT Flags, UINT* pNumModes,
                                                 DXGI_MODE_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC* pModeToMatch, DXGI_MODE_DESC* pClosestMatch,
                                                      IUnknown* pConcernedDevice) override;
    HRESULT STDMETHODCALLTYPE WaitForVBlank() override;
    HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown* pDevice, BOOL Exclusive) override;
    void STDMETHODCALLTYPE ReleaseOwnership() override;
    HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES* pGammaCaps) override;
    HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL* pArray) override;
    HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL* pArray) override;
    HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface* pScanoutSurface) override;
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface* pDestination) override;
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override;

    // ========================================================================
    // IDXGIOutput1
    // ========================================================================
    HRESULT STDMETHODCALLTYPE GetDisplayModeList1(DXGI_FORMAT EnumFormat, UINT Flags, UINT* pNumModes,
                                                  DXGI_MODE_DESC1* pDesc) override;
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode1(const DXGI_MODE_DESC1* pModeToMatch,
                                                       DXGI_MODE_DESC1* pClosestMatch,
                                                       IUnknown* pConcernedDevice) override;
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData1(IDXGIResource* pDestination) override;
    HRESULT STDMETHODCALLTYPE DuplicateOutput(IUnknown* pDevice, IDXGIOutputDuplication** ppOutputDuplication) override;

    // ========================================================================
    // IDXGIOutput2
    // ========================================================================
    BOOL STDMETHODCALLTYPE SupportsOverlays() override;

    // ========================================================================
    // IDXGIOutput3
    // ========================================================================
    HRESULT STDMETHODCALLTYPE CheckOverlaySupport(DXGI_FORMAT EnumFormat, IUnknown* pConcernedDevice,
                                                  UINT* pFlags) override;

    // ========================================================================
    // IDXGIOutput4
    // ========================================================================
    HRESULT STDMETHODCALLTYPE CheckOverlayColorSpaceSupport(DXGI_FORMAT Format, DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                            IUnknown* pConcernedDevice, UINT* pFlags) override;

    // ========================================================================
    // IDXGIOutput5
    // ========================================================================
    HRESULT STDMETHODCALLTYPE DuplicateOutput1(IUnknown* pDevice, UINT Flags, UINT SupportedFormatsCount,
                                               const DXGI_FORMAT* pSupportedFormats,
                                               IDXGIOutputDuplication** ppOutputDuplication) override;

    // ========================================================================
    // IDXGIOutput6
    // ========================================================================
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_OUTPUT_DESC1* pDesc) override;
    HRESULT STDMETHODCALLTYPE CheckHardwareCompositionSupport(UINT* pFlags) override;

private:
    IDXGIOutput* m_pReal;
    IDXGIOutput1* m_pReal1;
    IDXGIOutput2* m_pReal2;
    IDXGIOutput3* m_pReal3;
    IDXGIOutput4* m_pReal4;
    IDXGIOutput5* m_pReal5;
    IDXGIOutput6* m_pReal6;

    // Parent Adapter Wrapper
    CWrapDXGIAdapter* m_pParentAdapter;

    LONG m_RefCount;
    int m_Version;

    void PromoteInterfaces();
};
