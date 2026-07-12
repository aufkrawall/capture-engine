/**
 * D3D10 Device Wrapper Implementation
 */

#include "d3d10_device_wrap.h"
#include <cstdlib>
#include "../apis/lod_helper.h"
#include "../common/sampler_override_utils.h"
#include "dxgi_device_wrap.h"
#include "hook_common.h"

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapD3D10Device::CWrapD3D10Device(ID3D10Device* pReal)
    : m_pReal(pReal),
      m_pReal1(nullptr),
      m_RefCount(1),
      m_IsDevice1(false) {
    if (pReal) {
        pReal->AddRef();
        if (SUCCEEDED(pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1)))) {
            m_IsDevice1 = true;
        }
    }
    WrapperLog("D3D10 Device Wrapper: Created (real=%p, isDevice1=%d)", pReal, m_IsDevice1);
}

CWrapD3D10Device::~CWrapD3D10Device() {
    WrapperLog("D3D10 Device Wrapper: Destroyed");
    if (m_pReal1)
        m_pReal1->Release();
    if (m_pReal)
        m_pReal->Release();
}

void CWrapD3D10Device::ApplySamplerOverrides(D3D10_SAMPLER_DESC* pDesc) {
    if (!pDesc)
        return;
    if (!g_GraphicsOverridesActive.load(std::memory_order_acquire))
        return;

    if (pDesc->MaxLOD == 0.0f || pDesc->MinLOD == pDesc->MaxLOD)
        return;

    const auto& gfx = GetActiveGraphicsConfig();

    const std::string& af = gfx.anisotropicFiltering;
    if (af != "default" && !af.empty()) {
        if (af == "off") {
            if (ce::sampler_override::IsD3D10AnisotropicFilter(pDesc->Filter)) {
                bool comparison = ce::sampler_override::IsD3D10ComparisonFilter(pDesc->Filter);
                pDesc->Filter =
                    comparison ? D3D10_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR : D3D10_FILTER_MIN_MAG_MIP_LINEAR;
                pDesc->MaxAnisotropy = 1;
            }
        } else {
            UINT maxAniso = ce::sampler_override::GetConfiguredMaxAnisotropy(gfx);

            if (pDesc->AddressU != D3D10_TEXTURE_ADDRESS_BORDER && pDesc->AddressV != D3D10_TEXTURE_ADDRESS_BORDER &&
                pDesc->AddressW != D3D10_TEXTURE_ADDRESS_BORDER) {
                pDesc->Filter = ce::sampler_override::GetForcedAnisotropicFilter(pDesc->Filter);
                pDesc->MaxAnisotropy = maxAniso;
            }
        }
    }

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

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_POINTER;
    *ppvObj = nullptr;

    if (riid == IID_CWrapD3D10Device) {
        m_pReal->AddRef();
        *ppvObj = m_pReal;
        return S_OK;
    }

    if (riid == IID_IUnknown || riid == IID_ID3D10Device) {
        AddRef();
        *ppvObj = static_cast<ID3D10Device*>(this);
        return S_OK;
    }

    if (riid == IID_ID3D10Device1 && m_IsDevice1) {
        AddRef();
        *ppvObj = static_cast<ID3D10Device1*>(this);
        return S_OK;
    }

    if (riid == IID_IDXGIDevice || riid == IID_IDXGIDevice1 || riid == IID_IDXGIDevice2 || riid == IID_IDXGIDevice3 ||
        riid == IID_IDXGIDevice4 || riid == IID_IDXGIObject) {
        return m_pReal->QueryInterface(riid, ppvObj);
    }

    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapD3D10Device::AddRef() {
    return InterlockedIncrement(&m_RefCount);
}

ULONG STDMETHODCALLTYPE CWrapD3D10Device::Release() {
    ULONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0)
        delete this;
    return count;
}

// ============================================================================
// ID3D10Device - Forward All Methods
// ============================================================================

void STDMETHODCALLTYPE CWrapD3D10Device::VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                              ID3D10Buffer* const* ppConstantBuffers) {
    m_pReal->VSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D10Device::PSSetShaderResources(UINT StartSlot, UINT NumViews,
                                                              ID3D10ShaderResourceView* const* ppShaderResourceViews) {
    m_pReal->PSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D10Device::PSSetShader(ID3D10PixelShader* pPixelShader) {
    m_pReal->PSSetShader(pPixelShader);
}

void STDMETHODCALLTYPE CWrapD3D10Device::PSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                       ID3D10SamplerState* const* ppSamplers) {
    m_pReal->PSSetSamplers(StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D10Device::VSSetShader(ID3D10VertexShader* pVertexShader) {
    m_pReal->VSSetShader(pVertexShader);
}

void STDMETHODCALLTYPE CWrapD3D10Device::DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) {
    m_pReal->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
}

void STDMETHODCALLTYPE CWrapD3D10Device::Draw(UINT VertexCount, UINT StartVertexLocation) {
    m_pReal->Draw(VertexCount, StartVertexLocation);
}

void STDMETHODCALLTYPE CWrapD3D10Device::PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                              ID3D10Buffer* const* ppConstantBuffers) {
    m_pReal->PSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D10Device::IASetInputLayout(ID3D10InputLayout* pInputLayout) {
    m_pReal->IASetInputLayout(pInputLayout);
}

void STDMETHODCALLTYPE CWrapD3D10Device::IASetVertexBuffers(UINT StartSlot, UINT NumBuffers,
                                                            ID3D10Buffer* const* ppVertexBuffers, const UINT* pStrides,
                                                            const UINT* pOffsets) {
    m_pReal->IASetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
}

void STDMETHODCALLTYPE CWrapD3D10Device::IASetIndexBuffer(ID3D10Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset) {
    m_pReal->IASetIndexBuffer(pIndexBuffer, Format, Offset);
}

void STDMETHODCALLTYPE CWrapD3D10Device::DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount,
                                                              UINT StartIndexLocation, INT BaseVertexLocation,
                                                              UINT StartInstanceLocation) {
    m_pReal->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                                  StartInstanceLocation);
}

void STDMETHODCALLTYPE CWrapD3D10Device::DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount,
                                                       UINT StartVertexLocation, UINT StartInstanceLocation) {
    m_pReal->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE CWrapD3D10Device::GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                              ID3D10Buffer* const* ppConstantBuffers) {
    m_pReal->GSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D10Device::GSSetShader(ID3D10GeometryShader* pShader) {
    m_pReal->GSSetShader(pShader);
}

void STDMETHODCALLTYPE CWrapD3D10Device::IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY Topology) {
    m_pReal->IASetPrimitiveTopology(Topology);
}

void STDMETHODCALLTYPE CWrapD3D10Device::VSSetShaderResources(UINT StartSlot, UINT NumViews,
                                                              ID3D10ShaderResourceView* const* ppShaderResourceViews) {
    m_pReal->VSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D10Device::VSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                       ID3D10SamplerState* const* ppSamplers) {
    m_pReal->VSSetSamplers(StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D10Device::SetPredication(ID3D10Predicate* pPredicate, BOOL PredicateValue) {
    m_pReal->SetPredication(pPredicate, PredicateValue);
}

void STDMETHODCALLTYPE CWrapD3D10Device::GSSetShaderResources(UINT StartSlot, UINT NumViews,
                                                              ID3D10ShaderResourceView* const* ppShaderResourceViews) {
    m_pReal->GSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D10Device::GSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                       ID3D10SamplerState* const* ppSamplers) {
    m_pReal->GSSetSamplers(StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D10Device::OMSetRenderTargets(UINT NumViews,
                                                            ID3D10RenderTargetView* const* ppRenderTargetViews,
                                                            ID3D10DepthStencilView* pDepthStencilView) {
    m_pReal->OMSetRenderTargets(NumViews, ppRenderTargetViews, pDepthStencilView);
}

void STDMETHODCALLTYPE CWrapD3D10Device::OMSetBlendState(ID3D10BlendState* pBlendState, const FLOAT BlendFactor[4],
                                                         UINT SampleMask) {
    m_pReal->OMSetBlendState(pBlendState, BlendFactor, SampleMask);
}

void STDMETHODCALLTYPE CWrapD3D10Device::OMSetDepthStencilState(ID3D10DepthStencilState* pDepthStencilState,
                                                                UINT StencilRef) {
    m_pReal->OMSetDepthStencilState(pDepthStencilState, StencilRef);
}

void STDMETHODCALLTYPE CWrapD3D10Device::SOSetTargets(UINT NumBuffers, ID3D10Buffer* const* ppSOTargets,
                                                      const UINT* pOffsets) {
    m_pReal->SOSetTargets(NumBuffers, ppSOTargets, pOffsets);
}

void STDMETHODCALLTYPE CWrapD3D10Device::DrawAuto() {
    m_pReal->DrawAuto();
}

void STDMETHODCALLTYPE CWrapD3D10Device::RSSetState(ID3D10RasterizerState* pRasterizerState) {
    m_pReal->RSSetState(pRasterizerState);
}

void STDMETHODCALLTYPE CWrapD3D10Device::RSSetViewports(UINT NumViewports, const D3D10_VIEWPORT* pViewports) {
    m_pReal->RSSetViewports(NumViewports, pViewports);
}

void STDMETHODCALLTYPE CWrapD3D10Device::RSSetScissorRects(UINT NumRects, const D3D10_RECT* pRects) {
    m_pReal->RSSetScissorRects(NumRects, pRects);
}

void STDMETHODCALLTYPE CWrapD3D10Device::CopySubresourceRegion(ID3D10Resource* pDstResource, UINT DstSubresource,
                                                               UINT DstX, UINT DstY, UINT DstZ,
                                                               ID3D10Resource* pSrcResource, UINT SrcSubresource,
                                                               const D3D10_BOX* pSrcBox) {
    m_pReal->CopySubresourceRegion(pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource,
                                   pSrcBox);
}

void STDMETHODCALLTYPE CWrapD3D10Device::CopyResource(ID3D10Resource* pDstResource, ID3D10Resource* pSrcResource) {
    m_pReal->CopyResource(pDstResource, pSrcResource);
}

void STDMETHODCALLTYPE CWrapD3D10Device::UpdateSubresource(ID3D10Resource* pDstResource, UINT DstSubresource,
                                                           const D3D10_BOX* pDstBox, const void* pSrcData,
                                                           UINT SrcRowPitch, UINT SrcDepthPitch) {
    m_pReal->UpdateSubresource(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
}

void STDMETHODCALLTYPE CWrapD3D10Device::ClearRenderTargetView(ID3D10RenderTargetView* pRenderTargetView,
                                                               const FLOAT ColorRGBA[4]) {
    m_pReal->ClearRenderTargetView(pRenderTargetView, ColorRGBA);
}

void STDMETHODCALLTYPE CWrapD3D10Device::ClearDepthStencilView(ID3D10DepthStencilView* pDepthStencilView,
                                                               UINT ClearFlags, FLOAT Depth, UINT8 Stencil) {
    m_pReal->ClearDepthStencilView(pDepthStencilView, ClearFlags, Depth, Stencil);
}

void STDMETHODCALLTYPE CWrapD3D10Device::GenerateMips(ID3D10ShaderResourceView* pShaderResourceView) {
    m_pReal->GenerateMips(pShaderResourceView);
}

void STDMETHODCALLTYPE CWrapD3D10Device::ResolveSubresource(ID3D10Resource* pDstResource, UINT DstSubresource,
                                                            ID3D10Resource* pSrcResource, UINT SrcSubresource,
                                                            DXGI_FORMAT Format) {
    m_pReal->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format);
}

// Get methods (forward all)
void STDMETHODCALLTYPE CWrapD3D10Device::VSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                              ID3D10Buffer** ppConstantBuffers) {
    m_pReal->VSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}
void STDMETHODCALLTYPE CWrapD3D10Device::PSGetShaderResources(UINT StartSlot, UINT NumViews,
                                                              ID3D10ShaderResourceView** ppShaderResourceViews) {
    m_pReal->PSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}
void STDMETHODCALLTYPE CWrapD3D10Device::PSGetShader(ID3D10PixelShader** ppPixelShader) {
    m_pReal->PSGetShader(ppPixelShader);
}
void STDMETHODCALLTYPE CWrapD3D10Device::PSGetSamplers(UINT StartSlot, UINT NumSamplers,
                                                       ID3D10SamplerState** ppSamplers) {
    m_pReal->PSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}
void STDMETHODCALLTYPE CWrapD3D10Device::VSGetShader(ID3D10VertexShader** ppVertexShader) {
    m_pReal->VSGetShader(ppVertexShader);
}
void STDMETHODCALLTYPE CWrapD3D10Device::PSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                              ID3D10Buffer** ppConstantBuffers) {
    m_pReal->PSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}
void STDMETHODCALLTYPE CWrapD3D10Device::IAGetInputLayout(ID3D10InputLayout** ppInputLayout) {
    m_pReal->IAGetInputLayout(ppInputLayout);
}
void STDMETHODCALLTYPE CWrapD3D10Device::IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers,
                                                            ID3D10Buffer** ppVertexBuffers, UINT* pStrides,
                                                            UINT* pOffsets) {
    m_pReal->IAGetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
}
void STDMETHODCALLTYPE CWrapD3D10Device::IAGetIndexBuffer(ID3D10Buffer** pIndexBuffer, DXGI_FORMAT* Format,
                                                          UINT* Offset) {
    m_pReal->IAGetIndexBuffer(pIndexBuffer, Format, Offset);
}
void STDMETHODCALLTYPE CWrapD3D10Device::GSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                              ID3D10Buffer** ppConstantBuffers) {
    m_pReal->GSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}
void STDMETHODCALLTYPE CWrapD3D10Device::GSGetShader(ID3D10GeometryShader** ppGeometryShader) {
    m_pReal->GSGetShader(ppGeometryShader);
}
void STDMETHODCALLTYPE CWrapD3D10Device::IAGetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY* pTopology) {
    m_pReal->IAGetPrimitiveTopology(pTopology);
}
void STDMETHODCALLTYPE CWrapD3D10Device::VSGetShaderResources(UINT StartSlot, UINT NumViews,
                                                              ID3D10ShaderResourceView** ppShaderResourceViews) {
    m_pReal->VSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}
void STDMETHODCALLTYPE CWrapD3D10Device::VSGetSamplers(UINT StartSlot, UINT NumSamplers,
                                                       ID3D10SamplerState** ppSamplers) {
    m_pReal->VSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}
void STDMETHODCALLTYPE CWrapD3D10Device::GetPredication(ID3D10Predicate** ppPredicate, BOOL* pPredicateValue) {
    m_pReal->GetPredication(ppPredicate, pPredicateValue);
}
void STDMETHODCALLTYPE CWrapD3D10Device::GSGetShaderResources(UINT StartSlot, UINT NumViews,
                                                              ID3D10ShaderResourceView** ppShaderResourceViews) {
    m_pReal->GSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}
void STDMETHODCALLTYPE CWrapD3D10Device::GSGetSamplers(UINT StartSlot, UINT NumSamplers,
                                                       ID3D10SamplerState** ppSamplers) {
    m_pReal->GSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}
void STDMETHODCALLTYPE CWrapD3D10Device::OMGetRenderTargets(UINT NumViews, ID3D10RenderTargetView** ppRenderTargetViews,
                                                            ID3D10DepthStencilView** ppDepthStencilView) {
    m_pReal->OMGetRenderTargets(NumViews, ppRenderTargetViews, ppDepthStencilView);
}
void STDMETHODCALLTYPE CWrapD3D10Device::OMGetBlendState(ID3D10BlendState** ppBlendState, FLOAT BlendFactor[4],
                                                         UINT* pSampleMask) {
    m_pReal->OMGetBlendState(ppBlendState, BlendFactor, pSampleMask);
}
void STDMETHODCALLTYPE CWrapD3D10Device::OMGetDepthStencilState(ID3D10DepthStencilState** ppDepthStencilState,
                                                                UINT* pStencilRef) {
    m_pReal->OMGetDepthStencilState(ppDepthStencilState, pStencilRef);
}
void STDMETHODCALLTYPE CWrapD3D10Device::SOGetTargets(UINT NumBuffers, ID3D10Buffer** ppSOTargets, UINT* pOffsets) {
    m_pReal->SOGetTargets(NumBuffers, ppSOTargets, pOffsets);
}
void STDMETHODCALLTYPE CWrapD3D10Device::RSGetState(ID3D10RasterizerState** ppRasterizerState) {
    m_pReal->RSGetState(ppRasterizerState);
}
void STDMETHODCALLTYPE CWrapD3D10Device::RSGetViewports(UINT* NumViewports, D3D10_VIEWPORT* pViewports) {
    m_pReal->RSGetViewports(NumViewports, pViewports);
}
void STDMETHODCALLTYPE CWrapD3D10Device::RSGetScissorRects(UINT* NumRects, D3D10_RECT* pRects) {
    m_pReal->RSGetScissorRects(NumRects, pRects);
}
HRESULT STDMETHODCALLTYPE CWrapD3D10Device::GetDeviceRemovedReason() {
    return m_pReal->GetDeviceRemovedReason();
}
HRESULT STDMETHODCALLTYPE CWrapD3D10Device::SetExceptionMode(UINT RaiseFlags) {
    return m_pReal->SetExceptionMode(RaiseFlags);
}
UINT STDMETHODCALLTYPE CWrapD3D10Device::GetExceptionMode() {
    return m_pReal->GetExceptionMode();
}
HRESULT STDMETHODCALLTYPE CWrapD3D10Device::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    return m_pReal->GetPrivateData(guid, pDataSize, pData);
}
HRESULT STDMETHODCALLTYPE CWrapD3D10Device::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    return m_pReal->SetPrivateData(guid, DataSize, pData);
}
HRESULT STDMETHODCALLTYPE CWrapD3D10Device::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return m_pReal->SetPrivateDataInterface(guid, pData);
}
void STDMETHODCALLTYPE CWrapD3D10Device::ClearState() {
    m_pReal->ClearState();
}
void STDMETHODCALLTYPE CWrapD3D10Device::Flush() {
    m_pReal->Flush();
}

// ============================================================================
// Creation Methods
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateBuffer(const D3D10_BUFFER_DESC* pDesc,
                                                         const D3D10_SUBRESOURCE_DATA* pInitialData,
                                                         ID3D10Buffer** ppBuffer) {
    return m_pReal->CreateBuffer(pDesc, pInitialData, ppBuffer);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateTexture1D(const D3D10_TEXTURE1D_DESC* pDesc,
                                                            const D3D10_SUBRESOURCE_DATA* pInitialData,
                                                            ID3D10Texture1D** ppTexture1D) {
    return m_pReal->CreateTexture1D(pDesc, pInitialData, ppTexture1D);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateTexture2D(const D3D10_TEXTURE2D_DESC* pDesc,
                                                            const D3D10_SUBRESOURCE_DATA* pInitialData,
                                                            ID3D10Texture2D** ppTexture2D) {
    return m_pReal->CreateTexture2D(pDesc, pInitialData, ppTexture2D);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateTexture3D(const D3D10_TEXTURE3D_DESC* pDesc,
                                                            const D3D10_SUBRESOURCE_DATA* pInitialData,
                                                            ID3D10Texture3D** ppTexture3D) {
    return m_pReal->CreateTexture3D(pDesc, pInitialData, ppTexture3D);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateShaderResourceView(ID3D10Resource* pResource,
                                                                     const D3D10_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                                                     ID3D10ShaderResourceView** ppSRView) {
    return m_pReal->CreateShaderResourceView(pResource, pDesc, ppSRView);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateRenderTargetView(ID3D10Resource* pResource,
                                                                   const D3D10_RENDER_TARGET_VIEW_DESC* pDesc,
                                                                   ID3D10RenderTargetView** ppRTView) {
    return m_pReal->CreateRenderTargetView(pResource, pDesc, ppRTView);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateDepthStencilView(ID3D10Resource* pResource,
                                                                   const D3D10_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                                   ID3D10DepthStencilView** ppDepthStencilView) {
    return m_pReal->CreateDepthStencilView(pResource, pDesc, ppDepthStencilView);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateInputLayout(const D3D10_INPUT_ELEMENT_DESC* pInputElementDescs,
                                                              UINT NumElements,
                                                              const void* pShaderBytecodeWithInputSignature,
                                                              SIZE_T BytecodeLength,
                                                              ID3D10InputLayout** ppInputLayout) {
    return m_pReal->CreateInputLayout(pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature,
                                      BytecodeLength, ppInputLayout);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateVertexShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                                               ID3D10VertexShader** ppVertexShader) {
    return m_pReal->CreateVertexShader(pShaderBytecode, BytecodeLength, ppVertexShader);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateGeometryShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                                                 ID3D10GeometryShader** ppGeometryShader) {
    return m_pReal->CreateGeometryShader(pShaderBytecode, BytecodeLength, ppGeometryShader);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateGeometryShaderWithStreamOutput(
    const void* pShaderBytecode, SIZE_T BytecodeLength, const D3D10_SO_DECLARATION_ENTRY* pSODeclaration,
    UINT NumEntries, UINT OutputStreamStride, ID3D10GeometryShader** ppGeometryShader) {
    return m_pReal->CreateGeometryShaderWithStreamOutput(pShaderBytecode, BytecodeLength, pSODeclaration, NumEntries,
                                                         OutputStreamStride, ppGeometryShader);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreatePixelShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                                              ID3D10PixelShader** ppPixelShader) {
    return m_pReal->CreatePixelShader(pShaderBytecode, BytecodeLength, ppPixelShader);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateBlendState(const D3D10_BLEND_DESC* pBlendStateDesc,
                                                             ID3D10BlendState** ppBlendState) {
    return m_pReal->CreateBlendState(pBlendStateDesc, ppBlendState);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateDepthStencilState(const D3D10_DEPTH_STENCIL_DESC* pDepthStencilDesc,
                                                                    ID3D10DepthStencilState** ppDepthStencilState) {
    return m_pReal->CreateDepthStencilState(pDepthStencilDesc, ppDepthStencilState);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateRasterizerState(const D3D10_RASTERIZER_DESC* pRasterizerDesc,
                                                                  ID3D10RasterizerState** ppRasterizerState) {
    return m_pReal->CreateRasterizerState(pRasterizerDesc, ppRasterizerState);
}

// KEY METHOD: Sampler creation with AF/mip override
HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateSamplerState(const D3D10_SAMPLER_DESC* pSamplerDesc,
                                                               ID3D10SamplerState** ppSamplerState) {
    if (pSamplerDesc) {
        D3D10_SAMPLER_DESC desc = *pSamplerDesc;
        ApplySamplerOverrides(&desc);
        return m_pReal->CreateSamplerState(&desc, ppSamplerState);
    }
    return m_pReal->CreateSamplerState(pSamplerDesc, ppSamplerState);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateQuery(const D3D10_QUERY_DESC* pQueryDesc, ID3D10Query** ppQuery) {
    return m_pReal->CreateQuery(pQueryDesc, ppQuery);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreatePredicate(const D3D10_QUERY_DESC* pPredicateDesc,
                                                            ID3D10Predicate** ppPredicate) {
    return m_pReal->CreatePredicate(pPredicateDesc, ppPredicate);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateCounter(const D3D10_COUNTER_DESC* pCounterDesc,
                                                          ID3D10Counter** ppCounter) {
    return m_pReal->CreateCounter(pCounterDesc, ppCounter);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CheckFormatSupport(DXGI_FORMAT Format, UINT* pFormatSupport) {
    return m_pReal->CheckFormatSupport(Format, pFormatSupport);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CheckMultisampleQualityLevels(DXGI_FORMAT Format, UINT SampleCount,
                                                                          UINT* pNumQualityLevels) {
    return m_pReal->CheckMultisampleQualityLevels(Format, SampleCount, pNumQualityLevels);
}

void STDMETHODCALLTYPE CWrapD3D10Device::CheckCounterInfo(D3D10_COUNTER_INFO* pCounterInfo) {
    m_pReal->CheckCounterInfo(pCounterInfo);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CheckCounter(const D3D10_COUNTER_DESC* pDesc, D3D10_COUNTER_TYPE* pType,
                                                         UINT* pActiveCounters, LPSTR szName, UINT* pNameLength,
                                                         LPSTR szUnits, UINT* pUnitsLength, LPSTR szDescription,
                                                         UINT* pDescriptionLength) {
    return m_pReal->CheckCounter(pDesc, pType, pActiveCounters, szName, pNameLength, szUnits, pUnitsLength,
                                 szDescription, pDescriptionLength);
}

UINT STDMETHODCALLTYPE CWrapD3D10Device::GetCreationFlags() {
    return m_pReal->GetCreationFlags();
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::OpenSharedResource(HANDLE hResource, REFIID ReturnedInterface,
                                                               void** ppResource) {
    return m_pReal->OpenSharedResource(hResource, ReturnedInterface, ppResource);
}

void STDMETHODCALLTYPE CWrapD3D10Device::SetTextFilterSize(UINT Width, UINT Height) {
    m_pReal->SetTextFilterSize(Width, Height);
}

void STDMETHODCALLTYPE CWrapD3D10Device::GetTextFilterSize(UINT* pWidth, UINT* pHeight) {
    m_pReal->GetTextFilterSize(pWidth, pHeight);
}

// ============================================================================
// ID3D10Device1
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateShaderResourceView1(ID3D10Resource* pResource,
                                                                      const D3D10_SHADER_RESOURCE_VIEW_DESC1* pDesc,
                                                                      ID3D10ShaderResourceView1** ppSRView) {
    if (!m_pReal1)
        return E_NOINTERFACE;
    return m_pReal1->CreateShaderResourceView1(pResource, pDesc, ppSRView);
}

HRESULT STDMETHODCALLTYPE CWrapD3D10Device::CreateBlendState1(const D3D10_BLEND_DESC1* pBlendStateDesc,
                                                              ID3D10BlendState1** ppBlendState) {
    if (!m_pReal1)
        return E_NOINTERFACE;
    return m_pReal1->CreateBlendState1(pBlendStateDesc, ppBlendState);
}

D3D10_FEATURE_LEVEL1 STDMETHODCALLTYPE CWrapD3D10Device::GetFeatureLevel() {
    if (!m_pReal1)
        return D3D10_FEATURE_LEVEL_10_0;
    return m_pReal1->GetFeatureLevel();
}
