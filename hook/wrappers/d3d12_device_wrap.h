/**
 * D3D12 Device Wrapper
 *
 * Wraps ID3D12Device to intercept CreateCommandQueue for capture interception.
 * Note: Limited to ID3D12Device7 due to ABI differences in MSYS2/MinGW SDK for
 * Device8+
 */

#pragma once

#include <d3d12.h>
#include "wrapper_base.h"

// Forward declare command queue wrapper
class CWrapD3D12CommandQueue;

/**
 * CWrapD3D12Device - Wraps ID3D12Device through ID3D12Device7
 * Note: ID3D12Device8-10 have ABI incompatibilities with MinGW due to
 * __ret parameter ordering for aggregate return types. We limit to Device7.
 */
class CWrapD3D12Device : public ID3D12Device7 {
public:
    CWrapD3D12Device(ID3D12Device* pReal);
    virtual ~CWrapD3D12Device();

    ID3D12Device* GetReal() const {
        return m_pReal;
    }

    // ========================================================================
    // IUnknown
    // ========================================================================
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ========================================================================
    // ID3D12Object
    // ========================================================================
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;

    // ========================================================================
    // ID3D12Device - CORE METHODS
    // ========================================================================
    UINT STDMETHODCALLTYPE GetNodeCount() override;
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid,
                                                 void** ppCommandQueue) override;
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid,
                                                     void** ppCommandAllocator) override;
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid,
                                                          void** ppPipelineState) override;
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid,
                                                         void** ppPipelineState) override;
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
                                                ID3D12CommandAllocator* pCommandAllocator,
                                                ID3D12PipelineState* pInitialState, REFIID riid,
                                                void** ppCommandList) override;
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE Feature, void* pFeatureSupportData,
                                                  UINT FeatureSupportDataSize) override;
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc, REFIID riid,
                                                   void** ppvHeap) override;
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) override;
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT nodeMask, const void* pBlobWithRootSignature,
                                                  SIZE_T blobLengthInBytes, REFIID riid,
                                                  void** ppvRootSignature) override;
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource* pResource,
                                                    const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource* pResource, ID3D12Resource* pCounterResource,
                                                     const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC* pDesc,
                                         D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CopyDescriptors(UINT NumDestDescriptorRanges,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
                                           const UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
                                           const UINT* pSrcDescriptorRangeSizes,
                                           D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override;
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT NumDescriptors,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
                                                 D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override;
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(
        UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) override;
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT nodeMask, D3D12_HEAP_TYPE heapType) override;
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES* pHeapProperties,
                                                      D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc,
                                                      D3D12_RESOURCE_STATES InitialResourceState,
                                                      const D3D12_CLEAR_VALUE* pOptimizedClearValue,
                                                      REFIID riidResource, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap* pHeap, UINT64 HeapOffset,
                                                   const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
                                                   const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid,
                                                   void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC* pDesc,
                                                     D3D12_RESOURCE_STATES InitialState,
                                                     const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid,
                                                     void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild* pObject, const SECURITY_ATTRIBUTES* pAttributes,
                                                 DWORD Access, LPCWSTR Name, HANDLE* pHandle) override;
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE NTHandle, REFIID riid, void** ppvObj) override;
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR Name, DWORD Access, HANDLE* pNTHandle) override;
    HRESULT STDMETHODCALLTYPE MakeResident(UINT NumObjects, ID3D12Pageable* const* ppObjects) override;
    HRESULT STDMETHODCALLTYPE Evict(UINT NumObjects, ID3D12Pageable* const* ppObjects) override;
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid,
                                          void** ppFence) override;
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override;
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC* pResourceDesc, UINT FirstSubresource,
                                                 UINT NumSubresources, UINT64 BaseOffset,
                                                 D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts, UINT* pNumRows,
                                                 UINT64* pRowSizeInBytes, UINT64* pTotalBytes) override;
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL Enable) override;
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC* pDesc,
                                                     ID3D12RootSignature* pRootSignature, REFIID riid,
                                                     void** ppvCommandSignature) override;
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource* pTiledResource, UINT* pNumTilesForEntireResource,
                                             D3D12_PACKED_MIP_INFO* pPackedMipDesc,
                                             D3D12_TILE_SHAPE* pStandardTileShapeForNonPackedMips,
                                             UINT* pNumSubresourceTilings, UINT FirstSubresourceTilingToGet,
                                             D3D12_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) override;
    LUID STDMETHODCALLTYPE GetAdapterLuid() override;

    // ========================================================================
    // ID3D12Device1
    // ========================================================================
    HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(const void* pLibraryBlob, SIZE_T BlobLength, REFIID riid,
                                                    void** ppPipelineLibrary) override;
    HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(ID3D12Fence* const* ppFences,
                                                                const UINT64* pFenceValues, UINT NumFences,
                                                                D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags,
                                                                HANDLE hEvent) override;
    HRESULT STDMETHODCALLTYPE SetResidencyPriority(UINT NumObjects, ID3D12Pageable* const* ppObjects,
                                                   const D3D12_RESIDENCY_PRIORITY* pPriorities) override;

    // ========================================================================
    // ID3D12Device2
    // ========================================================================
    HRESULT STDMETHODCALLTYPE CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid,
                                                  void** ppPipelineState) override;

    // ========================================================================
    // ID3D12Device3
    // ========================================================================
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress(const void* pAddress, REFIID riid, void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromFileMapping(HANDLE hFileMapping, REFIID riid,
                                                              void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE EnqueueMakeResident(D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects,
                                                  ID3D12Pageable* const* ppObjects, ID3D12Fence* pFenceToSignal,
                                                  UINT64 FenceValueToSignal) override;

    // ========================================================================
    // ID3D12Device4
    // ========================================================================
    HRESULT STDMETHODCALLTYPE CreateCommandList1(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
                                                 D3D12_COMMAND_LIST_FLAGS flags, REFIID riid,
                                                 void** ppCommandList) override;
    HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession(const D3D12_PROTECTED_RESOURCE_SESSION_DESC* pDesc,
                                                             REFIID riid, void** ppSession) override;
    HRESULT STDMETHODCALLTYPE CreateCommittedResource1(const D3D12_HEAP_PROPERTIES* pHeapProperties,
                                                       D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc,
                                                       D3D12_RESOURCE_STATES InitialResourceState,
                                                       const D3D12_CLEAR_VALUE* pOptimizedClearValue,
                                                       ID3D12ProtectedResourceSession* pProtectedSession,
                                                       REFIID riidResource, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateHeap1(const D3D12_HEAP_DESC* pDesc,
                                          ID3D12ProtectedResourceSession* pProtectedSession, REFIID riid,
                                          void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE CreateReservedResource1(const D3D12_RESOURCE_DESC* pDesc,
                                                      D3D12_RESOURCE_STATES InitialState,
                                                      const D3D12_CLEAR_VALUE* pOptimizedClearValue,
                                                      ID3D12ProtectedResourceSession* pProtectedSession, REFIID riid,
                                                      void** ppvResource) override;
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE
    GetResourceAllocationInfo1(UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs,
                               D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) override;

    // ========================================================================
    // ID3D12Device5
    // ========================================================================
    HRESULT STDMETHODCALLTYPE CreateLifetimeTracker(ID3D12LifetimeOwner* pOwner, REFIID riid,
                                                    void** ppvTracker) override;
    void STDMETHODCALLTYPE RemoveDevice() override;
    HRESULT STDMETHODCALLTYPE EnumerateMetaCommands(UINT* pNumMetaCommands, D3D12_META_COMMAND_DESC* pDescs) override;
    HRESULT STDMETHODCALLTYPE EnumerateMetaCommandParameters(
        REFGUID CommandId, D3D12_META_COMMAND_PARAMETER_STAGE Stage, UINT* pTotalStructureSizeInBytes,
        UINT* pParameterCount, D3D12_META_COMMAND_PARAMETER_DESC* pParameterDescs) override;
    HRESULT STDMETHODCALLTYPE CreateMetaCommand(REFGUID CommandId, UINT NodeMask, const void* pCreationParametersData,
                                                SIZE_T CreationParametersDataSizeInBytes, REFIID riid,
                                                void** ppMetaCommand) override;
    HRESULT STDMETHODCALLTYPE CreateStateObject(const D3D12_STATE_OBJECT_DESC* pDesc, REFIID riid,
                                                void** ppStateObject) override;
    void STDMETHODCALLTYPE GetRaytracingAccelerationStructurePrebuildInfo(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* pDesc,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo) override;
    D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE
    CheckDriverMatchingIdentifier(D3D12_SERIALIZED_DATA_TYPE SerializedDataType,
                                  const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER* pIdentifierToCheck) override;

    // ========================================================================
    // ID3D12Device6
    // ========================================================================
    HRESULT STDMETHODCALLTYPE SetBackgroundProcessingMode(D3D12_BACKGROUND_PROCESSING_MODE Mode,
                                                          D3D12_MEASUREMENTS_ACTION MeasurementsAction,
                                                          HANDLE hEventToSignalUponCompletion,
                                                          BOOL* pbFurtherMeasurementsDesired) override;

    // ========================================================================
    // ID3D12Device7
    // ========================================================================
    HRESULT STDMETHODCALLTYPE AddToStateObject(const D3D12_STATE_OBJECT_DESC* pAddition,
                                               ID3D12StateObject* pStateObjectToGrowFrom, REFIID riid,
                                               void** ppNewStateObject) override;
    HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession1(const D3D12_PROTECTED_RESOURCE_SESSION_DESC1* pDesc,
                                                              REFIID riid, void** ppSession) override;

private:
    ID3D12Device* m_pReal;
    ID3D12Device1* m_pReal1;
    ID3D12Device2* m_pReal2;
    ID3D12Device3* m_pReal3;
    ID3D12Device4* m_pReal4;
    ID3D12Device5* m_pReal5;
    ID3D12Device6* m_pReal6;
    ID3D12Device7* m_pReal7;
    LONG m_RefCount;
    int m_Version;

    void PromoteInterfaces();
};
