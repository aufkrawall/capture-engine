/**
 * D3D11 device-context wrapper — Get surface and the versioned interfaces
 *
 * The ID3D11DeviceContext getters and the ID3D11DeviceContext1/2/3/4
 * additions, all forwarded to the real context.
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
// ID3D11DeviceContext - Get Methods (forwarded)
// ============================================================================

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer** ppConstantBuffers) {
    m_pReal->VSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSGetShaderResources(UINT StartSlot, UINT NumViews,
                                                                     ID3D11ShaderResourceView** ppShaderResourceViews) {
    m_pReal->PSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSGetShader(ID3D11PixelShader** ppPixelShader,
                                                            ID3D11ClassInstance** ppClassInstances,
                                                            UINT* pNumClassInstances) {
    m_pReal->PSGetShader(ppPixelShader, ppClassInstances, pNumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSGetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState** ppSamplers) {
    m_pReal->PSGetSamplers(StartSlot, NumSamplers, ppSamplers);
    if (!ppSamplers || StartSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return;
    }
    const UINT actualSamplers = (NumSamplers < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)
                                    ? NumSamplers
                                    : D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot;
    for (UINT i = 0; i < actualSamplers; ++i) {
        const UINT slot = StartSlot + i;
        ID3D11SamplerState* logical = m_TrackedSamplers[kWrapperStagePS][slot];
        if (logical && ppSamplers[i] == m_RealSamplers[kWrapperStagePS][slot] && ppSamplers[i] != logical) {
            if (ppSamplers[i]) {
                ppSamplers[i]->Release();
            }
            logical->AddRef();
            ppSamplers[i] = logical;
        }
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSGetShader(ID3D11VertexShader** ppVertexShader,
                                                            ID3D11ClassInstance** ppClassInstances,
                                                            UINT* pNumClassInstances) {
    m_pReal->VSGetShader(ppVertexShader, ppClassInstances, pNumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer** ppConstantBuffers) {
    m_pReal->PSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::IAGetInputLayout(ID3D11InputLayout** ppInputLayout) {
    m_pReal->IAGetInputLayout(ppInputLayout);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers,
                                                                   ID3D11Buffer** ppVertexBuffers, UINT* pStrides,
                                                                   UINT* pOffsets) {
    m_pReal->IAGetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::IAGetIndexBuffer(ID3D11Buffer** pIndexBuffer, DXGI_FORMAT* Format,
                                                                 UINT* Offset) {
    m_pReal->IAGetIndexBuffer(pIndexBuffer, Format, Offset);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer** ppConstantBuffers) {
    m_pReal->GSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSGetShader(ID3D11GeometryShader** ppGeometryShader,
                                                            ID3D11ClassInstance** ppClassInstances,
                                                            UINT* pNumClassInstances) {
    m_pReal->GSGetShader(ppGeometryShader, ppClassInstances, pNumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* pTopology) {
    m_pReal->IAGetPrimitiveTopology(pTopology);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSGetShaderResources(UINT StartSlot, UINT NumViews,
                                                                     ID3D11ShaderResourceView** ppShaderResourceViews) {
    m_pReal->VSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSGetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState** ppSamplers) {
    m_pReal->VSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GetPredication(ID3D11Predicate** ppPredicate, BOOL* pPredicateValue) {
    m_pReal->GetPredication(ppPredicate, pPredicateValue);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSGetShaderResources(UINT StartSlot, UINT NumViews,
                                                                     ID3D11ShaderResourceView** ppShaderResourceViews) {
    m_pReal->GSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSGetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState** ppSamplers) {
    m_pReal->GSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::OMGetRenderTargets(UINT NumViews,
                                                                   ID3D11RenderTargetView** ppRenderTargetViews,
                                                                   ID3D11DepthStencilView** ppDepthStencilView) {
    m_pReal->OMGetRenderTargets(NumViews, ppRenderTargetViews, ppDepthStencilView);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::OMGetRenderTargetsAndUnorderedAccessViews(
    UINT NumRTVs, ID3D11RenderTargetView** ppRenderTargetViews, ID3D11DepthStencilView** ppDepthStencilView,
    UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** ppUnorderedAccessViews) {
    m_pReal->OMGetRenderTargetsAndUnorderedAccessViews(NumRTVs, ppRenderTargetViews, ppDepthStencilView, UAVStartSlot,
                                                       NumUAVs, ppUnorderedAccessViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::OMGetBlendState(ID3D11BlendState** ppBlendState, FLOAT BlendFactor[4],
                                                                UINT* pSampleMask) {
    m_pReal->OMGetBlendState(ppBlendState, BlendFactor, pSampleMask);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::OMGetDepthStencilState(ID3D11DepthStencilState** ppDepthStencilState,
                                                                       UINT* pStencilRef) {
    m_pReal->OMGetDepthStencilState(ppDepthStencilState, pStencilRef);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::SOGetTargets(UINT NumBuffers, ID3D11Buffer** ppSOTargets) {
    m_pReal->SOGetTargets(NumBuffers, ppSOTargets);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::RSGetState(ID3D11RasterizerState** ppRasterizerState) {
    m_pReal->RSGetState(ppRasterizerState);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::RSGetViewports(UINT* pNumViewports, D3D11_VIEWPORT* pViewports) {
    m_pReal->RSGetViewports(pNumViewports, pViewports);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::RSGetScissorRects(UINT* pNumRects, D3D11_RECT* pRects) {
    m_pReal->RSGetScissorRects(pNumRects, pRects);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSGetShaderResources(UINT StartSlot, UINT NumViews,
                                                                     ID3D11ShaderResourceView** ppShaderResourceViews) {
    m_pReal->HSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSGetShader(ID3D11HullShader** ppHullShader,
                                                            ID3D11ClassInstance** ppClassInstances,
                                                            UINT* pNumClassInstances) {
    m_pReal->HSGetShader(ppHullShader, ppClassInstances, pNumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSGetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState** ppSamplers) {
    m_pReal->HSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer** ppConstantBuffers) {
    m_pReal->HSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSGetShaderResources(UINT StartSlot, UINT NumViews,
                                                                     ID3D11ShaderResourceView** ppShaderResourceViews) {
    m_pReal->DSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSGetShader(ID3D11DomainShader** ppDomainShader,
                                                            ID3D11ClassInstance** ppClassInstances,
                                                            UINT* pNumClassInstances) {
    m_pReal->DSGetShader(ppDomainShader, ppClassInstances, pNumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSGetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState** ppSamplers) {
    m_pReal->DSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer** ppConstantBuffers) {
    m_pReal->DSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSGetShaderResources(UINT StartSlot, UINT NumViews,
                                                                     ID3D11ShaderResourceView** ppShaderResourceViews) {
    m_pReal->CSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSGetUnorderedAccessViews(
    UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** ppUnorderedAccessViews) {
    m_pReal->CSGetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSGetShader(ID3D11ComputeShader** ppComputeShader,
                                                            ID3D11ClassInstance** ppClassInstances,
                                                            UINT* pNumClassInstances) {
    m_pReal->CSGetShader(ppComputeShader, ppClassInstances, pNumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSGetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState** ppSamplers) {
    m_pReal->CSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer** ppConstantBuffers) {
    m_pReal->CSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearState() {
    m_pReal->ClearState();
    ClearForcedAFTracking();
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::Flush() {
    m_pReal->Flush();
}

D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE CWrapD3D11DeviceContext::GetType() {
    return m_pReal->GetType();
}

UINT STDMETHODCALLTYPE CWrapD3D11DeviceContext::GetContextFlags() {
    return m_pReal->GetContextFlags();
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::FinishCommandList(BOOL RestoreDeferredContextState,
                                                                     ID3D11CommandList** ppCommandList) {
    const HRESULT hr = m_pReal->FinishCommandList(RestoreDeferredContextState, ppCommandList);
    if (SUCCEEDED(hr) && !RestoreDeferredContextState) {
        ClearForcedAFTracking();
    }
    return hr;
}

// ============================================================================
// ID3D11DeviceContext1
// ============================================================================

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CopySubresourceRegion1(ID3D11Resource* pDstResource,
                                                                       UINT DstSubresource, UINT DstX, UINT DstY,
                                                                       UINT DstZ, ID3D11Resource* pSrcResource,
                                                                       UINT SrcSubresource, const D3D11_BOX* pSrcBox,
                                                                       UINT CopyFlags) {
    if (m_pReal1)
        m_pReal1->CopySubresourceRegion1(pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource,
                                         pSrcBox, CopyFlags);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::UpdateSubresource1(ID3D11Resource* pDstResource, UINT DstSubresource,
                                                                   const D3D11_BOX* pDstBox, const void* pSrcData,
                                                                   UINT SrcRowPitch, UINT SrcDepthPitch,
                                                                   UINT CopyFlags) {
    if (m_pReal1)
        m_pReal1->UpdateSubresource1(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch,
                                     CopyFlags);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DiscardResource(ID3D11Resource* pResource) {
    if (m_pReal1)
        m_pReal1->DiscardResource(pResource);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DiscardView(ID3D11View* pResourceView) {
    if (m_pReal1)
        m_pReal1->DiscardView(pResourceView);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer* const* ppConstantBuffers,
                                                                      const UINT* pFirstConstant,
                                                                      const UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->VSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer* const* ppConstantBuffers,
                                                                      const UINT* pFirstConstant,
                                                                      const UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->HSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer* const* ppConstantBuffers,
                                                                      const UINT* pFirstConstant,
                                                                      const UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->DSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer* const* ppConstantBuffers,
                                                                      const UINT* pFirstConstant,
                                                                      const UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->GSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer* const* ppConstantBuffers,
                                                                      const UINT* pFirstConstant,
                                                                      const UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->PSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer* const* ppConstantBuffers,
                                                                      const UINT* pFirstConstant,
                                                                      const UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->CSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer** ppConstantBuffers,
                                                                      UINT* pFirstConstant, UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->VSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer** ppConstantBuffers,
                                                                      UINT* pFirstConstant, UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->HSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer** ppConstantBuffers,
                                                                      UINT* pFirstConstant, UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->DSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer** ppConstantBuffers,
                                                                      UINT* pFirstConstant, UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->GSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer** ppConstantBuffers,
                                                                      UINT* pFirstConstant, UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->PSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer** ppConstantBuffers,
                                                                      UINT* pFirstConstant, UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->CSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::SwapDeviceContextState(ID3DDeviceContextState* pState,
                                                                       ID3DDeviceContextState** ppPreviousState) {
    if (m_pReal1) {
        m_pReal1->SwapDeviceContextState(pState, ppPreviousState);
        ClearForcedAFTracking();
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearView(ID3D11View* pView, const FLOAT Color[4],
                                                          const D3D11_RECT* pRect, UINT NumRects) {
    static std::atomic<int> s_ClearViewMissingRealContext1Logs{0};
    if (m_pReal1) {
        m_pReal1->ClearView(pView, Color, pRect, NumRects);
    } else {
        int logIndex = s_ClearViewMissingRealContext1Logs.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 4) {
            WrapperLog("D3D11 Context Wrapper: ClearView requested without real Context1; view=%p rects=%u", pView,
                       NumRects);
        }
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DiscardView1(ID3D11View* pResourceView, const D3D11_RECT* pRects,
                                                             UINT NumRects) {
    if (m_pReal1)
        m_pReal1->DiscardView1(pResourceView, pRects, NumRects);
}

// ============================================================================
// ID3D11DeviceContext2
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::UpdateTileMappings(
    ID3D11Resource* pTiledResource, UINT NumTiledResourceRegions,
    const D3D11_TILED_RESOURCE_COORDINATE* pTiledResourceRegionStartCoordinates,
    const D3D11_TILE_REGION_SIZE* pTiledResourceRegionSizes, ID3D11Buffer* pTilePool, UINT NumRanges,
    const UINT* pRangeFlags, const UINT* pTilePoolStartOffsets, const UINT* pRangeTileCounts, UINT Flags) {
    if (m_pReal2)
        return m_pReal2->UpdateTileMappings(pTiledResource, NumTiledResourceRegions,
                                            pTiledResourceRegionStartCoordinates, pTiledResourceRegionSizes, pTilePool,
                                            NumRanges, pRangeFlags, pTilePoolStartOffsets, pRangeTileCounts, Flags);
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::CopyTileMappings(
    ID3D11Resource* pDestTiledResource, const D3D11_TILED_RESOURCE_COORDINATE* pDestRegionStartCoordinate,
    ID3D11Resource* pSourceTiledResource, const D3D11_TILED_RESOURCE_COORDINATE* pSourceRegionStartCoordinate,
    const D3D11_TILE_REGION_SIZE* pTileRegionSize, UINT Flags) {
    if (m_pReal2)
        return m_pReal2->CopyTileMappings(pDestTiledResource, pDestRegionStartCoordinate, pSourceTiledResource,
                                          pSourceRegionStartCoordinate, pTileRegionSize, Flags);
    return E_NOTIMPL;
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CopyTiles(
    ID3D11Resource* pTiledResource, const D3D11_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate,
    const D3D11_TILE_REGION_SIZE* pTileRegionSize, ID3D11Buffer* pBuffer, UINT64 BufferStartOffsetInBytes, UINT Flags) {
    if (m_pReal2)
        m_pReal2->CopyTiles(pTiledResource, pTileRegionStartCoordinate, pTileRegionSize, pBuffer,
                            BufferStartOffsetInBytes, Flags);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::UpdateTiles(
    ID3D11Resource* pDestTiledResource, const D3D11_TILED_RESOURCE_COORDINATE* pDestTileRegionStartCoordinate,
    const D3D11_TILE_REGION_SIZE* pDestTileRegionSize, const void* pSourceTileData, UINT Flags) {
    if (m_pReal2)
        m_pReal2->UpdateTiles(pDestTiledResource, pDestTileRegionStartCoordinate, pDestTileRegionSize, pSourceTileData,
                              Flags);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::ResizeTilePool(ID3D11Buffer* pTilePool, UINT64 NewSizeInBytes) {
    if (m_pReal2)
        return m_pReal2->ResizeTilePool(pTilePool, NewSizeInBytes);
    return E_NOTIMPL;
}

void STDMETHODCALLTYPE
CWrapD3D11DeviceContext::TiledResourceBarrier(ID3D11DeviceChild* pTiledResourceOrViewAccessBeforeBarrier,
                                              ID3D11DeviceChild* pTiledResourceOrViewAccessAfterBarrier) {
    if (m_pReal2)
        m_pReal2->TiledResourceBarrier(pTiledResourceOrViewAccessBeforeBarrier, pTiledResourceOrViewAccessAfterBarrier);
}

BOOL STDMETHODCALLTYPE CWrapD3D11DeviceContext::IsAnnotationEnabled() {
    if (m_pReal2)
        return m_pReal2->IsAnnotationEnabled();
    return FALSE;
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::SetMarkerInt(LPCWSTR pLabel, INT Data) {
    if (m_pReal2)
        m_pReal2->SetMarkerInt(pLabel, Data);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::BeginEventInt(LPCWSTR pLabel, INT Data) {
    if (m_pReal2)
        m_pReal2->BeginEventInt(pLabel, Data);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::EndEvent() {
    if (m_pReal2)
        m_pReal2->EndEvent();
}

// ============================================================================
// ID3D11DeviceContext3
// ============================================================================

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::Flush1(D3D11_CONTEXT_TYPE ContextType, HANDLE hEvent) {
    if (m_pReal3)
        m_pReal3->Flush1(ContextType, hEvent);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::SetHardwareProtectionState(BOOL HwProtectionEnable) {
    if (m_pReal3)
        m_pReal3->SetHardwareProtectionState(HwProtectionEnable);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GetHardwareProtectionState(BOOL* pHwProtectionEnable) {
    if (m_pReal3)
        m_pReal3->GetHardwareProtectionState(pHwProtectionEnable);
}

// ============================================================================
// ID3D11DeviceContext4
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::Signal(ID3D11Fence* pFence, UINT64 Value) {
    if (m_pReal4)
        return m_pReal4->Signal(pFence, Value);
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::Wait(ID3D11Fence* pFence, UINT64 Value) {
    if (m_pReal4)
        return m_pReal4->Wait(pFence, Value);
    return E_NOTIMPL;
}
