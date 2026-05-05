/**
 * D3D11 Device Wrapper Implementation
 *
 * CreateSamplerState intercepts for AF/mip LOD bias override.
 */

#include "d3d11_device_wrap.h"
#include "../apis/lod_helper.h"
#include "../common/sampler_override_utils.h"
#include "d3d11_devicecontext_wrap.h"
#include "dxgi_device_wrap.h"
#include "hook_common.h"

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapD3D11Device::CWrapD3D11Device(ID3D11Device* pReal)
    : m_pReal(pReal),
      m_pReal1(nullptr),
      m_pReal2(nullptr),
      m_pReal3(nullptr),
      m_pReal4(nullptr),
      m_pReal5(nullptr),
      m_RefCount(1),
      m_Version(0),
      m_pWrappedContext(nullptr) {
    if (pReal) {
        pReal->AddRef();
        PromoteInterfaces();
    }
    WrapperLog("D3D11 Device Wrapper: Created (real=%p, version=%d)", pReal, m_Version);
}

CWrapD3D11Device::~CWrapD3D11Device() {
    WrapperLog("D3D11 Device Wrapper: Destroyed");
    if (m_pWrappedContext) {
        m_pWrappedContext->InvalidateDeviceWrapper();
        m_pWrappedContext->Release();
        m_pWrappedContext = nullptr;
    }
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

void CWrapD3D11Device::PromoteInterfaces() {
    if (!m_pReal)
        return;

    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal5))))
        m_Version = 5;
    else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4))))
        m_Version = 4;
    else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3))))
        m_Version = 3;
    else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2))))
        m_Version = 2;
    else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1))))
        m_Version = 1;
}

void CWrapD3D11Device::ApplySamplerOverrides(D3D11_SAMPLER_DESC* pDesc) {
    if (!pDesc)
        return;
    if (!g_GraphicsOverridesActive.load(std::memory_order_acquire))
        return;

    // Skip non-mipmapped samplers (same logic as DetourCreateSamplerState)
    if (pDesc->MaxLOD == 0.0f || pDesc->MinLOD == pDesc->MaxLOD)
        return;

    const auto& gfx = GetActiveGraphicsConfig();

    // Forced AF-on needs SRV/resource context on Blackwell. Create-time only
    // handles disabling existing AF and mip-bias changes.
    const std::string& af = gfx.anisotropicFiltering;
    if (af != "default" && !af.empty()) {
        if (af == "off") {
            if (ce::sampler_override::IsD3D11AnisotropicFilter(pDesc->Filter)) {
                bool wasComparison = ce::sampler_override::IsD3D11ComparisonFilter(pDesc->Filter);
                pDesc->Filter =
                    wasComparison ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                pDesc->MaxAnisotropy = 1;
            }
        }
    }

    // Mip Bias
    const std::string& bias = gfx.mipBias;
    if ((bias != "default" && !bias.empty()) || gfx.forceMipBiasClamp) {
        float originalBias = pDesc->MipLODBias;
        pDesc->MipLODBias = ApplyConfiguredMipBias(gfx, originalBias);
        pDesc->MipLODBias = FinalizeMipBias(gfx, pDesc->MipLODBias);
    }
}

// ============================================================================
// IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_POINTER;
    *ppvObj = nullptr;

    if (riid == IID_CWrapD3D11Device) {
        m_pReal->AddRef();
        *ppvObj = m_pReal;
        return S_OK;
    }

    if (riid == IID_IUnknown || riid == IID_ID3D11Device) {
        AddRef();
        *ppvObj = static_cast<ID3D11Device*>(this);
        return S_OK;
    }

#define CHECK_DEVICE_VERSION(N, IFACE)                   \
    if (riid == IID_ID3D11Device##N && m_Version >= N) { \
        AddRef();                                        \
        *ppvObj = static_cast<IFACE*>(this);             \
        return S_OK;                                     \
    }

    CHECK_DEVICE_VERSION(1, ID3D11Device1)
    CHECK_DEVICE_VERSION(2, ID3D11Device2)
    CHECK_DEVICE_VERSION(3, ID3D11Device3)
    CHECK_DEVICE_VERSION(4, ID3D11Device4)
    CHECK_DEVICE_VERSION(5, ID3D11Device5)

#undef CHECK_DEVICE_VERSION

    if (riid == IID_IDXGIDevice || riid == IID_IDXGIDevice1 || riid == IID_IDXGIDevice2 || riid == IID_IDXGIDevice3 ||
        riid == IID_IDXGIDevice4 || riid == IID_IDXGIObject) {
        IDXGIDevice* pRealDxgiDevice = nullptr;
        HRESULT hr = m_pReal->QueryInterface(IID_PPV_ARGS(&pRealDxgiDevice));
        if (FAILED(hr) || !pRealDxgiDevice) {
            return hr;
        }

        auto* pWrappedDxgiDevice = new CWrapDXGIDevice(pRealDxgiDevice);
        pRealDxgiDevice->Release();

        hr = pWrappedDxgiDevice->QueryInterface(riid, ppvObj);
        pWrappedDxgiDevice->Release();
        return hr;
    }

    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapD3D11Device::AddRef() {
    return InterlockedIncrement(&m_RefCount);
}

ULONG STDMETHODCALLTYPE CWrapD3D11Device::Release() {
    ULONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0)
        delete this;
    return count;
}

// ============================================================================
// ID3D11Device - Core Creation Methods
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateBuffer(const D3D11_BUFFER_DESC* pDesc,
                                                         const D3D11_SUBRESOURCE_DATA* pInitialData,
                                                         ID3D11Buffer** ppBuffer) {
    return m_pReal->CreateBuffer(pDesc, pInitialData, ppBuffer);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateTexture1D(const D3D11_TEXTURE1D_DESC* pDesc,
                                                            const D3D11_SUBRESOURCE_DATA* pInitialData,
                                                            ID3D11Texture1D** ppTexture1D) {
    return m_pReal->CreateTexture1D(pDesc, pInitialData, ppTexture1D);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateTexture2D(const D3D11_TEXTURE2D_DESC* pDesc,
                                                            const D3D11_SUBRESOURCE_DATA* pInitialData,
                                                            ID3D11Texture2D** ppTexture2D) {
    return m_pReal->CreateTexture2D(pDesc, pInitialData, ppTexture2D);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateTexture3D(const D3D11_TEXTURE3D_DESC* pDesc,
                                                            const D3D11_SUBRESOURCE_DATA* pInitialData,
                                                            ID3D11Texture3D** ppTexture3D) {
    return m_pReal->CreateTexture3D(pDesc, pInitialData, ppTexture3D);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateShaderResourceView(ID3D11Resource* pResource,
                                                                     const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                                                     ID3D11ShaderResourceView** ppSRView) {
    return m_pReal->CreateShaderResourceView(pResource, pDesc, ppSRView);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateUnorderedAccessView(ID3D11Resource* pResource,
                                                                      const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc,
                                                                      ID3D11UnorderedAccessView** ppUAView) {
    return m_pReal->CreateUnorderedAccessView(pResource, pDesc, ppUAView);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateRenderTargetView(ID3D11Resource* pResource,
                                                                   const D3D11_RENDER_TARGET_VIEW_DESC* pDesc,
                                                                   ID3D11RenderTargetView** ppRTView) {
    return m_pReal->CreateRenderTargetView(pResource, pDesc, ppRTView);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateDepthStencilView(ID3D11Resource* pResource,
                                                                   const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                                   ID3D11DepthStencilView** ppDepthStencilView) {
    return m_pReal->CreateDepthStencilView(pResource, pDesc, ppDepthStencilView);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs,
                                                              UINT NumElements,
                                                              const void* pShaderBytecodeWithInputSignature,
                                                              SIZE_T BytecodeLength,
                                                              ID3D11InputLayout** ppInputLayout) {
    return m_pReal->CreateInputLayout(pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature,
                                      BytecodeLength, ppInputLayout);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateVertexShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                                               ID3D11ClassLinkage* pClassLinkage,
                                                               ID3D11VertexShader** ppVertexShader) {
    return m_pReal->CreateVertexShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateGeometryShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                                                 ID3D11ClassLinkage* pClassLinkage,
                                                                 ID3D11GeometryShader** ppGeometryShader) {
    return m_pReal->CreateGeometryShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppGeometryShader);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateGeometryShaderWithStreamOutput(
    const void* pShaderBytecode, SIZE_T BytecodeLength, const D3D11_SO_DECLARATION_ENTRY* pSODeclaration,
    UINT NumEntries, const UINT* pBufferStrides, UINT NumStrides, UINT RasterizedStream,
    ID3D11ClassLinkage* pClassLinkage, ID3D11GeometryShader** ppGeometryShader) {
    return m_pReal->CreateGeometryShaderWithStreamOutput(pShaderBytecode, BytecodeLength, pSODeclaration, NumEntries,
                                                         pBufferStrides, NumStrides, RasterizedStream, pClassLinkage,
                                                         ppGeometryShader);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreatePixelShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                                              ID3D11ClassLinkage* pClassLinkage,
                                                              ID3D11PixelShader** ppPixelShader) {
    return m_pReal->CreatePixelShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateHullShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                                             ID3D11ClassLinkage* pClassLinkage,
                                                             ID3D11HullShader** ppHullShader) {
    return m_pReal->CreateHullShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppHullShader);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateDomainShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                                               ID3D11ClassLinkage* pClassLinkage,
                                                               ID3D11DomainShader** ppDomainShader) {
    return m_pReal->CreateDomainShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppDomainShader);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateComputeShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                                                ID3D11ClassLinkage* pClassLinkage,
                                                                ID3D11ComputeShader** ppComputeShader) {
    return m_pReal->CreateComputeShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppComputeShader);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateClassLinkage(ID3D11ClassLinkage** ppLinkage) {
    return m_pReal->CreateClassLinkage(ppLinkage);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateBlendState(const D3D11_BLEND_DESC* pBlendStateDesc,
                                                             ID3D11BlendState** ppBlendState) {
    return m_pReal->CreateBlendState(pBlendStateDesc, ppBlendState);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* pDepthStencilDesc,
                                                                    ID3D11DepthStencilState** ppDepthStencilState) {
    return m_pReal->CreateDepthStencilState(pDepthStencilDesc, ppDepthStencilState);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateRasterizerState(const D3D11_RASTERIZER_DESC* pRasterizerDesc,
                                                                  ID3D11RasterizerState** ppRasterizerState) {
    return m_pReal->CreateRasterizerState(pRasterizerDesc, ppRasterizerState);
}

// KEY METHOD: Sampler state creation with AF/mip override
HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateSamplerState(const D3D11_SAMPLER_DESC* pSamplerDesc,
                                                               ID3D11SamplerState** ppSamplerState) {
    if (pSamplerDesc) {
        D3D11_SAMPLER_DESC desc = *pSamplerDesc;
        ApplySamplerOverrides(&desc);
        return m_pReal->CreateSamplerState(&desc, ppSamplerState);
    }
    return m_pReal->CreateSamplerState(pSamplerDesc, ppSamplerState);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateQuery(const D3D11_QUERY_DESC* pQueryDesc, ID3D11Query** ppQuery) {
    return m_pReal->CreateQuery(pQueryDesc, ppQuery);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreatePredicate(const D3D11_QUERY_DESC* pPredicateDesc,
                                                            ID3D11Predicate** ppPredicate) {
    return m_pReal->CreatePredicate(pPredicateDesc, ppPredicate);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateCounter(const D3D11_COUNTER_DESC* pCounterDesc,
                                                          ID3D11Counter** ppCounter) {
    return m_pReal->CreateCounter(pCounterDesc, ppCounter);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateDeferredContext(UINT ContextFlags,
                                                                  ID3D11DeviceContext** ppDeferredContext) {
    return m_pReal->CreateDeferredContext(ContextFlags, ppDeferredContext);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::OpenSharedResource(HANDLE hResource, REFIID ReturnedInterface,
                                                               void** ppResource) {
    return m_pReal->OpenSharedResource(hResource, ReturnedInterface, ppResource);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CheckFormatSupport(DXGI_FORMAT Format, UINT* pFormatSupport) {
    return m_pReal->CheckFormatSupport(Format, pFormatSupport);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CheckMultisampleQualityLevels(DXGI_FORMAT Format, UINT SampleCount,
                                                                          UINT* pNumQualityLevels) {
    return m_pReal->CheckMultisampleQualityLevels(Format, SampleCount, pNumQualityLevels);
}

void STDMETHODCALLTYPE CWrapD3D11Device::CheckCounterInfo(D3D11_COUNTER_INFO* pCounterInfo) {
    m_pReal->CheckCounterInfo(pCounterInfo);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CheckCounter(const D3D11_COUNTER_DESC* pDesc, D3D11_COUNTER_TYPE* pType,
                                                         UINT* pActiveCounters, LPSTR szName, UINT* pNameLength,
                                                         LPSTR szUnits, UINT* pUnitsLength, LPSTR szDescription,
                                                         UINT* pDescriptionLength) {
    return m_pReal->CheckCounter(pDesc, pType, pActiveCounters, szName, pNameLength, szUnits, pUnitsLength,
                                 szDescription, pDescriptionLength);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CheckFeatureSupport(D3D11_FEATURE Feature, void* pFeatureSupportData,
                                                                UINT FeatureSupportDataSize) {
    return m_pReal->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    return m_pReal->GetPrivateData(guid, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    return m_pReal->SetPrivateData(guid, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return m_pReal->SetPrivateDataInterface(guid, pData);
}

D3D_FEATURE_LEVEL STDMETHODCALLTYPE CWrapD3D11Device::GetFeatureLevel() {
    return m_pReal->GetFeatureLevel();
}

UINT STDMETHODCALLTYPE CWrapD3D11Device::GetCreationFlags() {
    return m_pReal->GetCreationFlags();
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::GetDeviceRemovedReason() {
    return m_pReal->GetDeviceRemovedReason();
}

void STDMETHODCALLTYPE CWrapD3D11Device::GetImmediateContext(ID3D11DeviceContext** ppImmediateContext) {
    if (!ppImmediateContext)
        return;

    // Create wrapped context on demand
    if (!m_pWrappedContext) {
        ID3D11DeviceContext* pRealContext = nullptr;
        m_pReal->GetImmediateContext(&pRealContext);
        if (pRealContext) {
            m_pWrappedContext = new CWrapD3D11DeviceContext(pRealContext, this);
            pRealContext->Release();  // Wrapper took ownership
        }
    }

    if (m_pWrappedContext) {
        m_pWrappedContext->AddRef();
        *ppImmediateContext = m_pWrappedContext;
    } else {
        m_pReal->GetImmediateContext(ppImmediateContext);
    }
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::SetExceptionMode(UINT RaiseFlags) {
    return m_pReal->SetExceptionMode(RaiseFlags);
}

UINT STDMETHODCALLTYPE CWrapD3D11Device::GetExceptionMode() {
    return m_pReal->GetExceptionMode();
}

// ============================================================================
// ID3D11Device1-5 Methods (Forward all)
// ============================================================================

void STDMETHODCALLTYPE CWrapD3D11Device::GetImmediateContext1(ID3D11DeviceContext1** ppImmediateContext) {
    if (!ppImmediateContext || !m_pReal1)
        return;

    // Create wrapped context on demand
    if (!m_pWrappedContext) {
        ID3D11DeviceContext1* pRealContext = nullptr;
        m_pReal1->GetImmediateContext1(&pRealContext);
        if (pRealContext) {
            m_pWrappedContext = new CWrapD3D11DeviceContext(pRealContext, this);
            pRealContext->Release();
        }
    }

    if (m_pWrappedContext) {
        m_pWrappedContext->AddRef();
        *ppImmediateContext = static_cast<ID3D11DeviceContext1*>(m_pWrappedContext);
    } else {
        m_pReal1->GetImmediateContext1(ppImmediateContext);
    }
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateDeferredContext1(UINT ContextFlags,
                                                                   ID3D11DeviceContext1** ppDeferredContext) {
    if (!m_pReal1)
        return E_NOINTERFACE;
    return m_pReal1->CreateDeferredContext1(ContextFlags, ppDeferredContext);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateBlendState1(const D3D11_BLEND_DESC1* pBlendStateDesc,
                                                              ID3D11BlendState1** ppBlendState) {
    if (!m_pReal1)
        return E_NOINTERFACE;
    return m_pReal1->CreateBlendState1(pBlendStateDesc, ppBlendState);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateRasterizerState1(const D3D11_RASTERIZER_DESC1* pRasterizerDesc,
                                                                   ID3D11RasterizerState1** ppRasterizerState) {
    if (!m_pReal1)
        return E_NOINTERFACE;
    return m_pReal1->CreateRasterizerState1(pRasterizerDesc, ppRasterizerState);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateDeviceContextState(
    UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, REFIID EmulatedInterface,
    D3D_FEATURE_LEVEL* pChosenFeatureLevel, ID3DDeviceContextState** ppContextState) {
    if (!m_pReal1)
        return E_NOINTERFACE;
    return m_pReal1->CreateDeviceContextState(Flags, pFeatureLevels, FeatureLevels, SDKVersion, EmulatedInterface,
                                              pChosenFeatureLevel, ppContextState);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::OpenSharedResource1(HANDLE hResource, REFIID returnedInterface,
                                                                void** ppResource) {
    if (!m_pReal1)
        return E_NOINTERFACE;
    return m_pReal1->OpenSharedResource1(hResource, returnedInterface, ppResource);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::OpenSharedResourceByName(LPCWSTR lpName, DWORD dwDesiredAccess,
                                                                     REFIID returnedInterface, void** ppResource) {
    if (!m_pReal1)
        return E_NOINTERFACE;
    return m_pReal1->OpenSharedResourceByName(lpName, dwDesiredAccess, returnedInterface, ppResource);
}

// ID3D11Device2
void STDMETHODCALLTYPE CWrapD3D11Device::GetImmediateContext2(ID3D11DeviceContext2** ppImmediateContext) {
    if (!ppImmediateContext || !m_pReal2)
        return;

    if (!m_pWrappedContext) {
        ID3D11DeviceContext2* pRealContext = nullptr;
        m_pReal2->GetImmediateContext2(&pRealContext);
        if (pRealContext) {
            m_pWrappedContext = new CWrapD3D11DeviceContext(pRealContext, this);
            pRealContext->Release();
        }
    }

    if (m_pWrappedContext) {
        m_pWrappedContext->AddRef();
        *ppImmediateContext = static_cast<ID3D11DeviceContext2*>(m_pWrappedContext);
    } else {
        m_pReal2->GetImmediateContext2(ppImmediateContext);
    }
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateDeferredContext2(UINT ContextFlags,
                                                                   ID3D11DeviceContext2** ppDeferredContext) {
    if (!m_pReal2)
        return E_NOINTERFACE;
    return m_pReal2->CreateDeferredContext2(ContextFlags, ppDeferredContext);
}

void STDMETHODCALLTYPE CWrapD3D11Device::GetResourceTiling(
    ID3D11Resource* pTiledResource, UINT* pNumTilesForEntireResource, D3D11_PACKED_MIP_DESC* pPackedMipDesc,
    D3D11_TILE_SHAPE* pStandardTileShapeForNonPackedMips, UINT* pNumSubresourceTilings,
    UINT FirstSubresourceTilingToGet, D3D11_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) {
    if (m_pReal2)
        m_pReal2->GetResourceTiling(pTiledResource, pNumTilesForEntireResource, pPackedMipDesc,
                                    pStandardTileShapeForNonPackedMips, pNumSubresourceTilings,
                                    FirstSubresourceTilingToGet, pSubresourceTilingsForNonPackedMips);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CheckMultisampleQualityLevels1(DXGI_FORMAT Format, UINT SampleCount,
                                                                           UINT Flags, UINT* pNumQualityLevels) {
    if (!m_pReal2)
        return E_NOINTERFACE;
    return m_pReal2->CheckMultisampleQualityLevels1(Format, SampleCount, Flags, pNumQualityLevels);
}

// ID3D11Device3
HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateTexture2D1(const D3D11_TEXTURE2D_DESC1* pDesc1,
                                                             const D3D11_SUBRESOURCE_DATA* pInitialData,
                                                             ID3D11Texture2D1** ppTexture2D) {
    if (!m_pReal3)
        return E_NOINTERFACE;
    return m_pReal3->CreateTexture2D1(pDesc1, pInitialData, ppTexture2D);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateTexture3D1(const D3D11_TEXTURE3D_DESC1* pDesc1,
                                                             const D3D11_SUBRESOURCE_DATA* pInitialData,
                                                             ID3D11Texture3D1** ppTexture3D) {
    if (!m_pReal3)
        return E_NOINTERFACE;
    return m_pReal3->CreateTexture3D1(pDesc1, pInitialData, ppTexture3D);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateRasterizerState2(const D3D11_RASTERIZER_DESC2* pRasterizerDesc,
                                                                   ID3D11RasterizerState2** ppRasterizerState) {
    if (!m_pReal3)
        return E_NOINTERFACE;
    return m_pReal3->CreateRasterizerState2(pRasterizerDesc, ppRasterizerState);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateShaderResourceView1(ID3D11Resource* pResource,
                                                                      const D3D11_SHADER_RESOURCE_VIEW_DESC1* pDesc1,
                                                                      ID3D11ShaderResourceView1** ppSRView1) {
    if (!m_pReal3)
        return E_NOINTERFACE;
    return m_pReal3->CreateShaderResourceView1(pResource, pDesc1, ppSRView1);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateUnorderedAccessView1(ID3D11Resource* pResource,
                                                                       const D3D11_UNORDERED_ACCESS_VIEW_DESC1* pDesc1,
                                                                       ID3D11UnorderedAccessView1** ppUAView1) {
    if (!m_pReal3)
        return E_NOINTERFACE;
    return m_pReal3->CreateUnorderedAccessView1(pResource, pDesc1, ppUAView1);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateRenderTargetView1(ID3D11Resource* pResource,
                                                                    const D3D11_RENDER_TARGET_VIEW_DESC1* pDesc1,
                                                                    ID3D11RenderTargetView1** ppRTView1) {
    if (!m_pReal3)
        return E_NOINTERFACE;
    return m_pReal3->CreateRenderTargetView1(pResource, pDesc1, ppRTView1);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateQuery1(const D3D11_QUERY_DESC1* pQueryDesc1,
                                                         ID3D11Query1** ppQuery1) {
    if (!m_pReal3)
        return E_NOINTERFACE;
    return m_pReal3->CreateQuery1(pQueryDesc1, ppQuery1);
}

void STDMETHODCALLTYPE CWrapD3D11Device::GetImmediateContext3(ID3D11DeviceContext3** ppImmediateContext) {
    if (!ppImmediateContext || !m_pReal3)
        return;

    if (!m_pWrappedContext) {
        ID3D11DeviceContext3* pRealContext = nullptr;
        m_pReal3->GetImmediateContext3(&pRealContext);
        if (pRealContext) {
            m_pWrappedContext = new CWrapD3D11DeviceContext(pRealContext, this);
            pRealContext->Release();
        }
    }

    if (m_pWrappedContext) {
        m_pWrappedContext->AddRef();
        *ppImmediateContext = static_cast<ID3D11DeviceContext3*>(m_pWrappedContext);
    } else {
        m_pReal3->GetImmediateContext3(ppImmediateContext);
    }
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateDeferredContext3(UINT ContextFlags,
                                                                   ID3D11DeviceContext3** ppDeferredContext) {
    if (!m_pReal3)
        return E_NOINTERFACE;
    return m_pReal3->CreateDeferredContext3(ContextFlags, ppDeferredContext);
}

void STDMETHODCALLTYPE CWrapD3D11Device::WriteToSubresource(ID3D11Resource* pDstResource, UINT DstSubresource,
                                                            const D3D11_BOX* pDstBox, const void* pSrcData,
                                                            UINT SrcRowPitch, UINT SrcDepthPitch) {
    if (m_pReal3)
        m_pReal3->WriteToSubresource(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
}

void STDMETHODCALLTYPE CWrapD3D11Device::ReadFromSubresource(void* pDstData, UINT DstRowPitch, UINT DstDepthPitch,
                                                             ID3D11Resource* pSrcResource, UINT SrcSubresource,
                                                             const D3D11_BOX* pSrcBox) {
    if (m_pReal3)
        m_pReal3->ReadFromSubresource(pDstData, DstRowPitch, DstDepthPitch, pSrcResource, SrcSubresource, pSrcBox);
}

// ID3D11Device4
HRESULT STDMETHODCALLTYPE CWrapD3D11Device::RegisterDeviceRemovedEvent(HANDLE hEvent, DWORD* pdwCookie) {
    if (!m_pReal4)
        return E_NOINTERFACE;
    return m_pReal4->RegisterDeviceRemovedEvent(hEvent, pdwCookie);
}

void STDMETHODCALLTYPE CWrapD3D11Device::UnregisterDeviceRemoved(DWORD dwCookie) {
    if (m_pReal4)
        m_pReal4->UnregisterDeviceRemoved(dwCookie);
}

// ID3D11Device5
HRESULT STDMETHODCALLTYPE CWrapD3D11Device::OpenSharedFence(HANDLE hFence, REFIID ReturnedInterface, void** ppFence) {
    if (!m_pReal5)
        return E_NOINTERFACE;
    return m_pReal5->OpenSharedFence(hFence, ReturnedInterface, ppFence);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11Device::CreateFence(UINT64 InitialValue, D3D11_FENCE_FLAG Flags,
                                                        REFIID ReturnedInterface, void** ppFence) {
    if (!m_pReal5)
        return E_NOINTERFACE;
    return m_pReal5->CreateFence(InitialValue, Flags, ReturnedInterface, ppFence);
}
