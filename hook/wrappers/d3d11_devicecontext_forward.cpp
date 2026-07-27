/**
 * D3D11 device-context wrapper — IUnknown and the Set/Draw surface
 *
 * The intercepted entry points (sampler binds, pixel-shader and SRV binds,
 * draws) plus the plain pass-through setters.
 *
 * Split out of d3d11_devicecontext_wrap.cpp.
 */

#include "d3d11_devicecontext_wrap.h"
#include "d3d11_devicecontext_wrap_internal.h"

#include <d3dcompiler.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include "../apis/dx11_hook.h"
#include "../common/sampler_override_utils.h"
#include "d3d11_device_wrap.h"
#include "hook_common.h"

// ============================================================================
// IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_POINTER;
    *ppvObj = nullptr;

    if (riid == IID_CWrapD3D11DeviceContext) {
        AddRef();
        *ppvObj = this;
        return S_OK;
    }

    if (riid == IID_IUnknown || riid == IID_ID3D11DeviceChild || riid == IID_ID3D11DeviceContext) {
        AddRef();
        *ppvObj = static_cast<ID3D11DeviceContext*>(this);
        return S_OK;
    }

    if (riid == IID_ID3D11DeviceContext1 && m_pReal1) {
        DX11Hook_ReportApiUse(m_pAFRealDevice, 1, "ID3D11DeviceContext1 QI");
        AddRef();
        *ppvObj = static_cast<ID3D11DeviceContext1*>(this);
        return S_OK;
    }

    if (riid == IID_ID3D11DeviceContext2 && m_pReal2) {
        DX11Hook_ReportApiUse(m_pAFRealDevice, 2, "ID3D11DeviceContext2 QI");
        AddRef();
        *ppvObj = static_cast<ID3D11DeviceContext2*>(this);
        return S_OK;
    }

    if (riid == IID_ID3D11DeviceContext3 && m_pReal3) {
        DX11Hook_ReportApiUse(m_pAFRealDevice, 3, "ID3D11DeviceContext3 QI");
        AddRef();
        *ppvObj = static_cast<ID3D11DeviceContext3*>(this);
        return S_OK;
    }

    if (riid == IID_ID3D11DeviceContext4 && m_pReal4) {
        DX11Hook_ReportApiUse(m_pAFRealDevice, 4, "ID3D11DeviceContext4 QI");
        AddRef();
        *ppvObj = static_cast<ID3D11DeviceContext4*>(this);
        return S_OK;
    }

    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapD3D11DeviceContext::AddRef() {
    return InterlockedIncrement(&m_RefCount);
}

ULONG STDMETHODCALLTYPE CWrapD3D11DeviceContext::Release() {
    ULONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0) {
        delete this;
    }
    return count;
}

// ============================================================================
// ID3D11DeviceChild
// ============================================================================

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GetDevice(ID3D11Device** ppDevice) {
    if (m_pDevice) {
        *ppDevice = m_pDevice;
        m_pDevice->AddRef();
    } else {
        m_pReal->GetDevice(ppDevice);
    }
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    return m_pReal->GetPrivateData(guid, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    return m_pReal->SetPrivateData(guid, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return m_pReal->SetPrivateDataInterface(guid, pData);
}

// ============================================================================
// ID3D11DeviceContext - Sampler State (INTERCEPTED for AF/mip enforcement)
// ============================================================================

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState* const* ppSamplers) {
    BindTrackedSamplers(kWrapperStagePS, StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState* const* ppSamplers) {
    BindTrackedSamplers(kWrapperStageVS, StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState* const* ppSamplers) {
    BindTrackedSamplers(kWrapperStageGS, StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState* const* ppSamplers) {
    BindTrackedSamplers(kWrapperStageHS, StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState* const* ppSamplers) {
    BindTrackedSamplers(kWrapperStageDS, StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState* const* ppSamplers) {
    BindTrackedSamplers(kWrapperStageCS, StartSlot, NumSamplers, ppSamplers);
}

// ============================================================================
// ID3D11DeviceContext - Core Methods (forwarded)
// ============================================================================

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer* const* ppConstantBuffers) {
    m_pReal->VSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSSetShaderResources(
    UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->PSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    }
    TrackShaderResources(kWrapperStagePS, StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSSetShader(ID3D11PixelShader* pPixelShader,
                                                            ID3D11ClassInstance* const* ppClassInstances,
                                                            UINT NumClassInstances) {
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->PSSetShader(pPixelShader, ppClassInstances, NumClassInstances);
    }
    TrackPixelShader(pPixelShader);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSSetShader(ID3D11VertexShader* pVertexShader,
                                                            ID3D11ClassInstance* const* ppClassInstances,
                                                            UINT NumClassInstances) {
    m_pReal->VSSetShader(pVertexShader, ppClassInstances, NumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawIndexed(UINT IndexCount, UINT StartIndexLocation,
                                                            INT BaseVertexLocation) {
    PreparePixelSamplersForDraw();
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::Draw(UINT VertexCount, UINT StartVertexLocation) {
    PreparePixelSamplersForDraw();
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->Draw(VertexCount, StartVertexLocation);
    }
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::Map(ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType,
                                                       UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource) {
    return m_pReal->Map(pResource, Subresource, MapType, MapFlags, pMappedResource);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::Unmap(ID3D11Resource* pResource, UINT Subresource) {
    m_pReal->Unmap(pResource, Subresource);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer* const* ppConstantBuffers) {
    m_pReal->PSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::IASetInputLayout(ID3D11InputLayout* pInputLayout) {
    m_pReal->IASetInputLayout(pInputLayout);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::IASetVertexBuffers(UINT StartSlot, UINT NumBuffers,
                                                                   ID3D11Buffer* const* ppVertexBuffers,
                                                                   const UINT* pStrides, const UINT* pOffsets) {
    m_pReal->IASetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::IASetIndexBuffer(ID3D11Buffer* pIndexBuffer, DXGI_FORMAT Format,
                                                                 UINT Offset) {
    m_pReal->IASetIndexBuffer(pIndexBuffer, Format, Offset);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount,
                                                                     UINT StartIndexLocation, INT BaseVertexLocation,
                                                                     UINT StartInstanceLocation) {
    PreparePixelSamplersForDraw();
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                                      StartInstanceLocation);
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount,
                                                              UINT StartVertexLocation, UINT StartInstanceLocation) {
    PreparePixelSamplersForDraw();
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer* const* ppConstantBuffers) {
    m_pReal->GSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSSetShader(ID3D11GeometryShader* pShader,
                                                            ID3D11ClassInstance* const* ppClassInstances,
                                                            UINT NumClassInstances) {
    m_pReal->GSSetShader(pShader, ppClassInstances, NumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) {
    m_pReal->IASetPrimitiveTopology(Topology);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSSetShaderResources(
    UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->VSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::Begin(ID3D11Asynchronous* pAsync) {
    m_pReal->Begin(pAsync);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::End(ID3D11Asynchronous* pAsync) {
    m_pReal->End(pAsync);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::GetData(ID3D11Asynchronous* pAsync, void* pData, UINT DataSize,
                                                           UINT GetDataFlags) {
    return m_pReal->GetData(pAsync, pData, DataSize, GetDataFlags);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::SetPredication(ID3D11Predicate* pPredicate, BOOL PredicateValue) {
    m_pReal->SetPredication(pPredicate, PredicateValue);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSSetShaderResources(
    UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->GSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::OMSetRenderTargets(UINT NumViews,
                                                                   ID3D11RenderTargetView* const* ppRenderTargetViews,
                                                                   ID3D11DepthStencilView* pDepthStencilView) {
    m_pReal->OMSetRenderTargets(NumViews, ppRenderTargetViews, pDepthStencilView);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::OMSetRenderTargetsAndUnorderedAccessViews(
    UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView,
    UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts) {
    m_pReal->OMSetRenderTargetsAndUnorderedAccessViews(NumRTVs, ppRenderTargetViews, pDepthStencilView, UAVStartSlot,
                                                       NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::OMSetBlendState(ID3D11BlendState* pBlendState,
                                                                const FLOAT BlendFactor[4], UINT SampleMask) {
    m_pReal->OMSetBlendState(pBlendState, BlendFactor, SampleMask);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::OMSetDepthStencilState(ID3D11DepthStencilState* pDepthStencilState,
                                                                       UINT StencilRef) {
    m_pReal->OMSetDepthStencilState(pDepthStencilState, StencilRef);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::SOSetTargets(UINT NumBuffers, ID3D11Buffer* const* ppSOTargets,
                                                             const UINT* pOffsets) {
    m_pReal->SOSetTargets(NumBuffers, ppSOTargets, pOffsets);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawAuto() {
    PreparePixelSamplersForDraw();
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->DrawAuto();
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawIndexedInstancedIndirect(ID3D11Buffer* pBufferForArgs,
                                                                             UINT AlignedByteOffsetForArgs) {
    PreparePixelSamplersForDraw();
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->DrawIndexedInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawInstancedIndirect(ID3D11Buffer* pBufferForArgs,
                                                                      UINT AlignedByteOffsetForArgs) {
    PreparePixelSamplersForDraw();
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->DrawInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY,
                                                         UINT ThreadGroupCountZ) {
    m_pReal->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DispatchIndirect(ID3D11Buffer* pBufferForArgs,
                                                                 UINT AlignedByteOffsetForArgs) {
    m_pReal->DispatchIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::RSSetState(ID3D11RasterizerState* pRasterizerState) {
    m_pReal->RSSetState(pRasterizerState);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports) {
    m_pReal->RSSetViewports(NumViewports, pViewports);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::RSSetScissorRects(UINT NumRects, const D3D11_RECT* pRects) {
    m_pReal->RSSetScissorRects(NumRects, pRects);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CopySubresourceRegion(ID3D11Resource* pDstResource, UINT DstSubresource,
                                                                      UINT DstX, UINT DstY, UINT DstZ,
                                                                      ID3D11Resource* pSrcResource, UINT SrcSubresource,
                                                                      const D3D11_BOX* pSrcBox) {
    m_pReal->CopySubresourceRegion(pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource,
                                   pSrcBox);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CopyResource(ID3D11Resource* pDstResource,
                                                             ID3D11Resource* pSrcResource) {
    m_pReal->CopyResource(pDstResource, pSrcResource);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::UpdateSubresource(ID3D11Resource* pDstResource, UINT DstSubresource,
                                                                  const D3D11_BOX* pDstBox, const void* pSrcData,
                                                                  UINT SrcRowPitch, UINT SrcDepthPitch) {
    m_pReal->UpdateSubresource(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CopyStructureCount(ID3D11Buffer* pDstBuffer, UINT DstAlignedByteOffset,
                                                                   ID3D11UnorderedAccessView* pSrcView) {
    m_pReal->CopyStructureCount(pDstBuffer, DstAlignedByteOffset, pSrcView);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearRenderTargetView(ID3D11RenderTargetView* pRenderTargetView,
                                                                      const FLOAT ColorRGBA[4]) {
    m_pReal->ClearRenderTargetView(pRenderTargetView, ColorRGBA);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearUnorderedAccessViewUint(
    ID3D11UnorderedAccessView* pUnorderedAccessView, const UINT Values[4]) {
    m_pReal->ClearUnorderedAccessViewUint(pUnorderedAccessView, Values);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearUnorderedAccessViewFloat(
    ID3D11UnorderedAccessView* pUnorderedAccessView, const FLOAT Values[4]) {
    m_pReal->ClearUnorderedAccessViewFloat(pUnorderedAccessView, Values);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearDepthStencilView(ID3D11DepthStencilView* pDepthStencilView,
                                                                      UINT ClearFlags, FLOAT Depth, UINT8 Stencil) {
    m_pReal->ClearDepthStencilView(pDepthStencilView, ClearFlags, Depth, Stencil);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GenerateMips(ID3D11ShaderResourceView* pShaderResourceView) {
    m_pReal->GenerateMips(pShaderResourceView);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::SetResourceMinLOD(ID3D11Resource* pResource, FLOAT MinLOD) {
    m_pReal->SetResourceMinLOD(pResource, MinLOD);
}

FLOAT STDMETHODCALLTYPE CWrapD3D11DeviceContext::GetResourceMinLOD(ID3D11Resource* pResource) {
    return m_pReal->GetResourceMinLOD(pResource);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ResolveSubresource(ID3D11Resource* pDstResource, UINT DstSubresource,
                                                                   ID3D11Resource* pSrcResource, UINT SrcSubresource,
                                                                   DXGI_FORMAT Format) {
    m_pReal->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ExecuteCommandList(ID3D11CommandList* pCommandList,
                                                                   BOOL RestoreContextState) {
    m_pReal->ExecuteCommandList(pCommandList, RestoreContextState);
    if (!RestoreContextState) {
        ClearForcedAFTracking();
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSSetShaderResources(
    UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->HSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSSetShader(ID3D11HullShader* pHullShader,
                                                            ID3D11ClassInstance* const* ppClassInstances,
                                                            UINT NumClassInstances) {
    m_pReal->HSSetShader(pHullShader, ppClassInstances, NumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer* const* ppConstantBuffers) {
    m_pReal->HSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSSetShaderResources(
    UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->DSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSSetShader(ID3D11DomainShader* pDomainShader,
                                                            ID3D11ClassInstance* const* ppClassInstances,
                                                            UINT NumClassInstances) {
    m_pReal->DSSetShader(pDomainShader, ppClassInstances, NumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer* const* ppConstantBuffers) {
    m_pReal->DSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSSetShaderResources(
    UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->CSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSSetUnorderedAccessViews(
    UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts) {
    m_pReal->CSSetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSSetShader(ID3D11ComputeShader* pComputeShader,
                                                            ID3D11ClassInstance* const* ppClassInstances,
                                                            UINT NumClassInstances) {
    m_pReal->CSSetShader(pComputeShader, ppClassInstances, NumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer* const* ppConstantBuffers) {
    m_pReal->CSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}
