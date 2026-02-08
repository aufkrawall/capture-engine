/**
 * D3D12 Device Wrapper Implementation
 *
 * Most methods forward directly to real device.
 * CreateCommandQueue is intercepted to return wrapped queue for capture.
 */

#include "d3d12_device_wrap.h"
#include "d3d12_commandqueue_wrap.h"
#include "d3d12_wrapper_interface.h"
#include "wrapper_base.h"

#include <vector>

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapD3D12Device::CWrapD3D12Device(ID3D12Device* pReal)
    : m_pReal(pReal), m_pReal1(nullptr), m_pReal2(nullptr), m_pReal3(nullptr), m_pReal4(nullptr), m_pReal5(nullptr),
      m_pReal6(nullptr), m_pReal7(nullptr), m_RefCount(1), m_Version(0)
{
    if (pReal) {
        pReal->AddRef();
        PromoteInterfaces();
    }
    WrapperLog("D3D12 Device Wrapper: Created (real=%p, version=%d)", pReal, m_Version);
}

CWrapD3D12Device::~CWrapD3D12Device()
{
    WrapperLog("D3D12 Device Wrapper: Destroyed");
    if (m_pReal7) m_pReal7->Release();
    if (m_pReal6) m_pReal6->Release();
    if (m_pReal5) m_pReal5->Release();
    if (m_pReal4) m_pReal4->Release();
    if (m_pReal3) m_pReal3->Release();
    if (m_pReal2) m_pReal2->Release();
    if (m_pReal1) m_pReal1->Release();
    if (m_pReal) m_pReal->Release();
}

void CWrapD3D12Device::PromoteInterfaces()
{
    if (!m_pReal) return;

    // Note: We limit to Device7 due to ABI issues with Device8+ in MinGW
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal7))))
        m_Version = 7;
    else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal6))))
        m_Version = 6;
    else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal5))))
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

// ============================================================================
// IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;

    if (riid == IID_CWrapD3D12Device) {
        AddRef();
        *ppvObj = this;
        return S_OK;
    }

    // CRITICAL FIX: IUnknown must return consistent pointer for COM identity
    // All IUnknown queries must return the same pointer value
    if (riid == IID_IUnknown) {
        AddRef();
        *ppvObj = static_cast<IUnknown*>(static_cast<ID3D12Device*>(this));
        return S_OK;
    }

    if (riid == IID_ID3D12Object || riid == IID_ID3D12Device) {
        AddRef();
        *ppvObj = static_cast<ID3D12Device*>(this);
        return S_OK;
    }

// Return wrapper for supported versions
#define CHECK_VERSION(N, IFACE)                          \
    if (riid == IID_ID3D12Device##N && m_Version >= N) { \
        AddRef();                                        \
        *ppvObj = static_cast<IFACE*>(this);             \
        return S_OK;                                     \
    }

    CHECK_VERSION(1, ID3D12Device1)
    CHECK_VERSION(2, ID3D12Device2)
    CHECK_VERSION(3, ID3D12Device3)
    CHECK_VERSION(4, ID3D12Device4)
    CHECK_VERSION(5, ID3D12Device5)
    CHECK_VERSION(6, ID3D12Device6)
    CHECK_VERSION(7, ID3D12Device7)
    // Note: Device8+ omitted due to ABI issues with MinGW

#undef CHECK_VERSION

    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapD3D12Device::AddRef() { return InterlockedIncrement(&m_RefCount); }

ULONG STDMETHODCALLTYPE CWrapD3D12Device::Release()
{
    ULONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0) delete this;
    return count;
}

// ============================================================================
// ID3D12Object
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData)
{
    return m_pReal->GetPrivateData(guid, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData)
{
    return m_pReal->SetPrivateData(guid, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData)
{
    return m_pReal->SetPrivateDataInterface(guid, pData);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::SetName(LPCWSTR Name) { return m_pReal->SetName(Name); }

// ============================================================================
// ID3D12Device - CORE METHODS
// ============================================================================

UINT STDMETHODCALLTYPE CWrapD3D12Device::GetNodeCount() { return m_pReal->GetNodeCount(); }

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid,
                                                               void** ppCommandQueue)
{
    if (!ppCommandQueue) return E_POINTER;

    WrapperLog("D3D12 Device: CreateCommandQueue (Type=%d)", pDesc ? pDesc->Type : -1);

    ID3D12CommandQueue* pRealQueue = nullptr;
    HRESULT hr = m_pReal->CreateCommandQueue(pDesc, IID_PPV_ARGS(&pRealQueue));

    if (SUCCEEDED(hr) && pRealQueue) {
        // Wrap the command queue
        auto* pWrapper = new CWrapD3D12CommandQueue(pRealQueue, this);

        // Query for the requested interface
        hr = pWrapper->QueryInterface(riid, ppCommandQueue);

        // Release our initial reference (QI added a ref if successful)
        pWrapper->Release();

        // Release the real queue reference (wrapper owns it now)
        pRealQueue->Release();

        if (SUCCEEDED(hr)) {
            WrapperLog("D3D12 Device: Created wrapped CommandQueue (wrapper=%p)", *ppCommandQueue);
        } else {
            WrapperLog("D3D12 Device: Failed to query interface for CommandQueue, hr=0x%08X", hr);
        }
    }

    return hr;
}

// Forward all creation methods
HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid,
                                                                   void** ppCmdAlloc)
{
    return m_pReal->CreateCommandAllocator(type, riid, ppCmdAlloc);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
                                                                        REFIID riid, void** ppPSO)
{
    return m_pReal->CreateGraphicsPipelineState(pDesc, riid, ppPSO);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
                                                                       REFIID riid, void** ppPSO)
{
    return m_pReal->CreateComputePipelineState(pDesc, riid, ppPSO);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateCommandList(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
                                                              ID3D12CommandAllocator* pAlloc,
                                                              ID3D12PipelineState* pInitial, REFIID riid,
                                                              void** ppCmdList)
{
    return m_pReal->CreateCommandList(nodeMask, type, pAlloc, pInitial, riid, ppCmdList);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CheckFeatureSupport(D3D12_FEATURE Feature, void* pData, UINT DataSize)
{
    // Log feature support queries for debugging
    const char* featureName = "Unknown";
    switch (Feature) {
        case D3D12_FEATURE_D3D12_OPTIONS:
            featureName = "D3D12_OPTIONS";
            break;
        case D3D12_FEATURE_ARCHITECTURE:
            featureName = "ARCHITECTURE";
            break;
        case D3D12_FEATURE_FEATURE_LEVELS:
            featureName = "FEATURE_LEVELS";
            break;
        case D3D12_FEATURE_FORMAT_SUPPORT:
            featureName = "FORMAT_SUPPORT";
            break;
        case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS:
            featureName = "MULTISAMPLE_QUALITY_LEVELS";
            break;
        case D3D12_FEATURE_FORMAT_INFO:
            featureName = "FORMAT_INFO";
            break;
        case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT:
            featureName = "GPU_VIRTUAL_ADDRESS_SUPPORT";
            break;
        case D3D12_FEATURE_SHADER_MODEL:
            featureName = "SHADER_MODEL";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS1:
            featureName = "D3D12_OPTIONS1";
            break;
        case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT:
            featureName = "PROTECTED_RESOURCE_SESSION_SUPPORT";
            break;
        case D3D12_FEATURE_ROOT_SIGNATURE:
            featureName = "ROOT_SIGNATURE";
            break;
        case D3D12_FEATURE_ARCHITECTURE1:
            featureName = "ARCHITECTURE1";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS2:
            featureName = "D3D12_OPTIONS2";
            break;
        case D3D12_FEATURE_SHADER_CACHE:
            featureName = "SHADER_CACHE";
            break;
        case D3D12_FEATURE_COMMAND_QUEUE_PRIORITY:
            featureName = "COMMAND_QUEUE_PRIORITY";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS3:
            featureName = "D3D12_OPTIONS3";
            break;
        case D3D12_FEATURE_EXISTING_HEAPS:
            featureName = "EXISTING_HEAPS";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS4:
            featureName = "D3D12_OPTIONS4";
            break;
        case D3D12_FEATURE_SERIALIZATION:
            featureName = "SERIALIZATION";
            break;
        case D3D12_FEATURE_CROSS_NODE:
            featureName = "CROSS_NODE";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS5:
            featureName = "D3D12_OPTIONS5";
            break;
        case D3D12_FEATURE_DISPLAYABLE:
            featureName = "DISPLAYABLE";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS6:
            featureName = "D3D12_OPTIONS6";
            break;
        case D3D12_FEATURE_QUERY_META_COMMAND:
            featureName = "QUERY_META_COMMAND";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS7:
            featureName = "D3D12_OPTIONS7";
            break;
        case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPE_COUNT:
            featureName = "PROTECTED_RESOURCE_SESSION_TYPE_COUNT";
            break;
        case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES:
            featureName = "PROTECTED_RESOURCE_SESSION_TYPES";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS8:
            featureName = "D3D12_OPTIONS8";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS9:
            featureName = "D3D12_OPTIONS9";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS10:
            featureName = "D3D12_OPTIONS10";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS11:
            featureName = "D3D12_OPTIONS11";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS12:
            featureName = "D3D12_OPTIONS12";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS13:
            featureName = "D3D12_OPTIONS13";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS14:
            featureName = "D3D12_OPTIONS14";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS15:
            featureName = "D3D12_OPTIONS15";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS16:
            featureName = "D3D12_OPTIONS16";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS17:
            featureName = "D3D12_OPTIONS17";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS18:
            featureName = "D3D12_OPTIONS18";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS19:
            featureName = "D3D12_OPTIONS19";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS20:
            featureName = "D3D12_OPTIONS20";
            break;
        case D3D12_FEATURE_PREDICATION:
            featureName = "PREDICATION";
            break;
        case D3D12_FEATURE_PLACED_RESOURCE_SUPPORT_INFO:
            featureName = "PLACED_RESOURCE_SUPPORT_INFO";
            break;
        case D3D12_FEATURE_HARDWARE_COPY:
            featureName = "HARDWARE_COPY";
            break;
        case D3D12_FEATURE_D3D12_OPTIONS21:
            featureName = "D3D12_OPTIONS21";
            break;
    }

    WrapperLog("D3D12 Device: CheckFeatureSupport CALLED - Feature=%s (%d), DataSize=%u", featureName, (int)Feature,
               DataSize);

    HRESULT hr = m_pReal->CheckFeatureSupport(Feature, pData, DataSize);

    // Log architecture info which might contain GPU memory info
    if (Feature == D3D12_FEATURE_ARCHITECTURE1 && SUCCEEDED(hr) && pData &&
        DataSize >= sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1)) {
        D3D12_FEATURE_DATA_ARCHITECTURE1* pArch = (D3D12_FEATURE_DATA_ARCHITECTURE1*)pData;
        WrapperLog("D3D12 Device: ARCHITECTURE1 - UMA=%d, CacheCoherentUMA=%d, IsolatedMMU=%d", pArch->UMA,
                   pArch->CacheCoherentUMA, pArch->IsolatedMMU);
    }

    // Log GPU virtual address support which contains memory info
    if (Feature == D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT && SUCCEEDED(hr) && pData &&
        DataSize >= sizeof(D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT)) {
        D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT* pGpuVA = (D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT*)pData;
        WrapperLog("D3D12 Device: GPU_VIRTUAL_ADDRESS_SUPPORT - MaxGPUVirtualAddressBitsPerResource=%u, "
                   "MaxGPUVirtualAddressBitsPerProcess=%u",
                   pGpuVA->MaxGPUVirtualAddressBitsPerResource, pGpuVA->MaxGPUVirtualAddressBitsPerProcess);
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* pDesc, REFIID riid,
                                                                 void** ppHeap)
{
    return m_pReal->CreateDescriptorHeap(pDesc, riid, ppHeap);
}

UINT STDMETHODCALLTYPE CWrapD3D12Device::GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE Type)
{
    return m_pReal->GetDescriptorHandleIncrementSize(Type);
}

// Helper to modify static samplers in root signature blob
static std::vector<BYTE> ModifyRootSignatureStaticSamplers(const void* pBlob, SIZE_T blobLen)
{
    std::vector<BYTE> result;

    if (!pBlob || blobLen < sizeof(UINT)) return result;

    // Parse root signature version
    const UINT* pHeader = static_cast<const UINT*>(pBlob);
    UINT version = pHeader[0];

    // Only handle version 1.0 (0x1) and 1.1 (0x2)
    if (version != 0x1 && version != 0x2) return result;

    // For now, just pass through - modifying root signature blobs is complex
    // and would require deserializing, modifying, and re-serializing
    // Most games use dynamic samplers via CreateSampler anyway

    // TODO: Implement full root signature deserialization if needed
    // The D3D12_ROOT_SIGNATURE_DESC structure contains pStaticSamplers
    // which we would need to modify

    return result;
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateRootSignature(UINT nodeMask, const void* pBlob, SIZE_T blobLen,
                                                                REFIID riid, void** ppRootSig)
{
    // Try to modify static samplers in the root signature
    auto modifiedBlob = ModifyRootSignatureStaticSamplers(pBlob, blobLen);

    if (!modifiedBlob.empty()) {
        return m_pReal->CreateRootSignature(nodeMask, modifiedBlob.data(), modifiedBlob.size(), riid, ppRootSig);
    }

    return m_pReal->CreateRootSignature(nodeMask, pBlob, blobLen, riid, ppRootSig);
}

void STDMETHODCALLTYPE CWrapD3D12Device::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc,
                                                                  D3D12_CPU_DESCRIPTOR_HANDLE Dest)
{
    m_pReal->CreateConstantBufferView(pDesc, Dest);
}

void STDMETHODCALLTYPE CWrapD3D12Device::CreateShaderResourceView(ID3D12Resource* pRes,
                                                                  const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                                                  D3D12_CPU_DESCRIPTOR_HANDLE Dest)
{
    m_pReal->CreateShaderResourceView(pRes, pDesc, Dest);
}

void STDMETHODCALLTYPE CWrapD3D12Device::CreateUnorderedAccessView(ID3D12Resource* pRes, ID3D12Resource* pCounter,
                                                                   const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
                                                                   D3D12_CPU_DESCRIPTOR_HANDLE Dest)
{
    m_pReal->CreateUnorderedAccessView(pRes, pCounter, pDesc, Dest);
}

void STDMETHODCALLTYPE CWrapD3D12Device::CreateRenderTargetView(ID3D12Resource* pRes,
                                                                const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
                                                                D3D12_CPU_DESCRIPTOR_HANDLE Dest)
{
    m_pReal->CreateRenderTargetView(pRes, pDesc, Dest);
}

void STDMETHODCALLTYPE CWrapD3D12Device::CreateDepthStencilView(ID3D12Resource* pRes,
                                                                const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                                D3D12_CPU_DESCRIPTOR_HANDLE Dest)
{
    m_pReal->CreateDepthStencilView(pRes, pDesc, Dest);
}

void STDMETHODCALLTYPE CWrapD3D12Device::CreateSampler(const D3D12_SAMPLER_DESC* pDesc,
                                                       D3D12_CPU_DESCRIPTOR_HANDLE Dest)
{
    if (pDesc) {
        D3D12_SAMPLER_DESC desc = *pDesc;
        ApplySamplerOverridesInternal(&desc);
        m_pReal->CreateSampler(&desc, Dest);
    } else {
        m_pReal->CreateSampler(pDesc, Dest);
    }
}

void STDMETHODCALLTYPE CWrapD3D12Device::CopyDescriptors(UINT NumDest, const D3D12_CPU_DESCRIPTOR_HANDLE* pDestStarts,
                                                         const UINT* pDestSizes, UINT NumSrc,
                                                         const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcStarts,
                                                         const UINT* pSrcSizes, D3D12_DESCRIPTOR_HEAP_TYPE Type)
{
    m_pReal->CopyDescriptors(NumDest, pDestStarts, pDestSizes, NumSrc, pSrcStarts, pSrcSizes, Type);
}

void STDMETHODCALLTYPE CWrapD3D12Device::CopyDescriptorsSimple(UINT Num, D3D12_CPU_DESCRIPTOR_HANDLE Dest,
                                                               D3D12_CPU_DESCRIPTOR_HANDLE Src,
                                                               D3D12_DESCRIPTOR_HEAP_TYPE Type)
{
    m_pReal->CopyDescriptorsSimple(Num, Dest, Src, Type);
}

D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE
CWrapD3D12Device::GetResourceAllocationInfo(UINT mask, UINT num, const D3D12_RESOURCE_DESC* pDescs)
{
    return m_pReal->GetResourceAllocationInfo(mask, num, pDescs);
}

D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE CWrapD3D12Device::GetCustomHeapProperties(UINT nodeMask, D3D12_HEAP_TYPE type)
{
    return m_pReal->GetCustomHeapProperties(nodeMask, type);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateCommittedResource(
    const D3D12_HEAP_PROPERTIES* pHeap, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* pClear, REFIID riid, void** ppRes)
{
    return m_pReal->CreateCommittedResource(pHeap, flags, pDesc, state, pClear, riid, ppRes);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateHeap(const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppHeap)
{
    return m_pReal->CreateHeap(pDesc, riid, ppHeap);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreatePlacedResource(ID3D12Heap* pHeap, UINT64 offset,
                                                                 const D3D12_RESOURCE_DESC* pDesc,
                                                                 D3D12_RESOURCE_STATES state,
                                                                 const D3D12_CLEAR_VALUE* pClear, REFIID riid,
                                                                 void** ppRes)
{
    return m_pReal->CreatePlacedResource(pHeap, offset, pDesc, state, pClear, riid, ppRes);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateReservedResource(const D3D12_RESOURCE_DESC* pDesc,
                                                                   D3D12_RESOURCE_STATES state,
                                                                   const D3D12_CLEAR_VALUE* pClear, REFIID riid,
                                                                   void** ppRes)
{
    return m_pReal->CreateReservedResource(pDesc, state, pClear, riid, ppRes);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateSharedHandle(ID3D12DeviceChild* pObj,
                                                               const SECURITY_ATTRIBUTES* pAttr, DWORD access,
                                                               LPCWSTR name, HANDLE* pHandle)
{
    return m_pReal->CreateSharedHandle(pObj, pAttr, access, name, pHandle);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::OpenSharedHandle(HANDLE handle, REFIID riid, void** ppObj)
{
    return m_pReal->OpenSharedHandle(handle, riid, ppObj);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::OpenSharedHandleByName(LPCWSTR name, DWORD access, HANDLE* pHandle)
{
    return m_pReal->OpenSharedHandleByName(name, access, pHandle);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::MakeResident(UINT num, ID3D12Pageable* const* ppObjs)
{
    return m_pReal->MakeResident(num, ppObjs);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::Evict(UINT num, ID3D12Pageable* const* ppObjs)
{
    return m_pReal->Evict(num, ppObjs);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateFence(UINT64 val, D3D12_FENCE_FLAGS flags, REFIID riid,
                                                        void** ppFence)
{
    return m_pReal->CreateFence(val, flags, riid, ppFence);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::GetDeviceRemovedReason() { return m_pReal->GetDeviceRemovedReason(); }

void STDMETHODCALLTYPE CWrapD3D12Device::GetCopyableFootprints(const D3D12_RESOURCE_DESC* pDesc, UINT first, UINT num,
                                                               UINT64 base,
                                                               D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts,
                                                               UINT* pRows, UINT64* pRowSizes, UINT64* pTotal)
{
    m_pReal->GetCopyableFootprints(pDesc, first, num, base, pLayouts, pRows, pRowSizes, pTotal);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid,
                                                            void** ppHeap)
{
    return m_pReal->CreateQueryHeap(pDesc, riid, ppHeap);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::SetStablePowerState(BOOL enable)
{
    return m_pReal->SetStablePowerState(enable);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC* pDesc,
                                                                   ID3D12RootSignature* pRoot, REFIID riid,
                                                                   void** ppSig)
{
    return m_pReal->CreateCommandSignature(pDesc, pRoot, riid, ppSig);
}

void STDMETHODCALLTYPE CWrapD3D12Device::GetResourceTiling(ID3D12Resource* pRes, UINT* pTiles,
                                                           D3D12_PACKED_MIP_INFO* pPacked, D3D12_TILE_SHAPE* pShape,
                                                           UINT* pNum, UINT first, D3D12_SUBRESOURCE_TILING* pTilings)
{
    m_pReal->GetResourceTiling(pRes, pTiles, pPacked, pShape, pNum, first, pTilings);
}

LUID STDMETHODCALLTYPE CWrapD3D12Device::GetAdapterLuid()
{
    LUID luid = m_pReal->GetAdapterLuid();
    WrapperLog("D3D12 Device: GetAdapterLuid called - LUID: %08X:%08X", luid.HighPart, luid.LowPart);
    return luid;
}

// ============================================================================
// ID3D12Device1
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreatePipelineLibrary(const void* pBlob, SIZE_T len, REFIID riid,
                                                                  void** ppLib)
{
    if (!m_pReal1) return E_NOINTERFACE;
    return m_pReal1->CreatePipelineLibrary(pBlob, len, riid, ppLib);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::SetEventOnMultipleFenceCompletion(ID3D12Fence* const* ppFences,
                                                                              const UINT64* pVals, UINT num,
                                                                              D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags,
                                                                              HANDLE hEvent)
{
    if (!m_pReal1) return E_NOINTERFACE;
    return m_pReal1->SetEventOnMultipleFenceCompletion(ppFences, pVals, num, flags, hEvent);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::SetResidencyPriority(UINT num, ID3D12Pageable* const* ppObjs,
                                                                 const D3D12_RESIDENCY_PRIORITY* pPriorities)
{
    if (!m_pReal1) return E_NOINTERFACE;
    return m_pReal1->SetResidencyPriority(num, ppObjs, pPriorities);
}

// ============================================================================
// ID3D12Device2-10 - Forward all methods
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc,
                                                                REFIID riid, void** ppPSO)
{
    if (!m_pReal2) return E_NOINTERFACE;
    return m_pReal2->CreatePipelineState(pDesc, riid, ppPSO);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::OpenExistingHeapFromAddress(const void* pAddr, REFIID riid, void** ppHeap)
{
    if (!m_pReal3) return E_NOINTERFACE;
    return m_pReal3->OpenExistingHeapFromAddress(pAddr, riid, ppHeap);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::OpenExistingHeapFromFileMapping(HANDLE hFile, REFIID riid, void** ppHeap)
{
    if (!m_pReal3) return E_NOINTERFACE;
    return m_pReal3->OpenExistingHeapFromFileMapping(hFile, riid, ppHeap);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::EnqueueMakeResident(D3D12_RESIDENCY_FLAGS flags, UINT num,
                                                                ID3D12Pageable* const* ppObjs, ID3D12Fence* pFence,
                                                                UINT64 val)
{
    if (!m_pReal3) return E_NOINTERFACE;
    return m_pReal3->EnqueueMakeResident(flags, num, ppObjs, pFence, val);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateCommandList1(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
                                                               D3D12_COMMAND_LIST_FLAGS flags, REFIID riid,
                                                               void** ppCmdList)
{
    if (!m_pReal4) return E_NOINTERFACE;
    return m_pReal4->CreateCommandList1(nodeMask, type, flags, riid, ppCmdList);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateProtectedResourceSession(
    const D3D12_PROTECTED_RESOURCE_SESSION_DESC* pDesc, REFIID riid, void** ppSession)
{
    if (!m_pReal4) return E_NOINTERFACE;
    return m_pReal4->CreateProtectedResourceSession(pDesc, riid, ppSession);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateCommittedResource1(
    const D3D12_HEAP_PROPERTIES* pHeap, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* pClear, ID3D12ProtectedResourceSession* pSession, REFIID riid,
    void** ppRes)
{
    if (!m_pReal4) return E_NOINTERFACE;
    return m_pReal4->CreateCommittedResource1(pHeap, flags, pDesc, state, pClear, pSession, riid, ppRes);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateHeap1(const D3D12_HEAP_DESC* pDesc,
                                                        ID3D12ProtectedResourceSession* pSession, REFIID riid,
                                                        void** ppHeap)
{
    if (!m_pReal4) return E_NOINTERFACE;
    return m_pReal4->CreateHeap1(pDesc, pSession, riid, ppHeap);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateReservedResource1(const D3D12_RESOURCE_DESC* pDesc,
                                                                    D3D12_RESOURCE_STATES state,
                                                                    const D3D12_CLEAR_VALUE* pClear,
                                                                    ID3D12ProtectedResourceSession* pSession,
                                                                    REFIID riid, void** ppRes)
{
    if (!m_pReal4) return E_NOINTERFACE;
    return m_pReal4->CreateReservedResource1(pDesc, state, pClear, pSession, riid, ppRes);
}

D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE CWrapD3D12Device::GetResourceAllocationInfo1(
    UINT mask, UINT num, const D3D12_RESOURCE_DESC* pDescs, D3D12_RESOURCE_ALLOCATION_INFO1* pInfo1)
{
    if (!m_pReal4) return {};
    return m_pReal4->GetResourceAllocationInfo1(mask, num, pDescs, pInfo1);
}

// ID3D12Device5
HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateLifetimeTracker(ID3D12LifetimeOwner* pOwner, REFIID riid,
                                                                  void** ppTracker)
{
    if (!m_pReal5) return E_NOINTERFACE;
    return m_pReal5->CreateLifetimeTracker(pOwner, riid, ppTracker);
}

void STDMETHODCALLTYPE CWrapD3D12Device::RemoveDevice()
{
    if (m_pReal5) m_pReal5->RemoveDevice();
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::EnumerateMetaCommands(UINT* pNum, D3D12_META_COMMAND_DESC* pDescs)
{
    if (!m_pReal5) return E_NOINTERFACE;
    return m_pReal5->EnumerateMetaCommands(pNum, pDescs);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::EnumerateMetaCommandParameters(REFGUID id,
                                                                           D3D12_META_COMMAND_PARAMETER_STAGE stage,
                                                                           UINT* pSize, UINT* pCount,
                                                                           D3D12_META_COMMAND_PARAMETER_DESC* pDescs)
{
    if (!m_pReal5) return E_NOINTERFACE;
    return m_pReal5->EnumerateMetaCommandParameters(id, stage, pSize, pCount, pDescs);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateMetaCommand(REFGUID id, UINT nodeMask, const void* pData, SIZE_T size,
                                                              REFIID riid, void** ppCmd)
{
    if (!m_pReal5) return E_NOINTERFACE;
    return m_pReal5->CreateMetaCommand(id, nodeMask, pData, size, riid, ppCmd);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateStateObject(const D3D12_STATE_OBJECT_DESC* pDesc, REFIID riid,
                                                              void** ppState)
{
    if (!m_pReal5) return E_NOINTERFACE;
    return m_pReal5->CreateStateObject(pDesc, riid, ppState);
}

void STDMETHODCALLTYPE CWrapD3D12Device::GetRaytracingAccelerationStructurePrebuildInfo(
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* pDesc,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo)
{
    if (m_pReal5) m_pReal5->GetRaytracingAccelerationStructurePrebuildInfo(pDesc, pInfo);
}

D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE CWrapD3D12Device::CheckDriverMatchingIdentifier(
    D3D12_SERIALIZED_DATA_TYPE type, const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER* pId)
{
    if (!m_pReal5) return D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED;
    return m_pReal5->CheckDriverMatchingIdentifier(type, pId);
}

// ID3D12Device6
HRESULT STDMETHODCALLTYPE CWrapD3D12Device::SetBackgroundProcessingMode(D3D12_BACKGROUND_PROCESSING_MODE mode,
                                                                        D3D12_MEASUREMENTS_ACTION action, HANDLE hEvent,
                                                                        BOOL* pFurther)
{
    if (!m_pReal6) return E_NOINTERFACE;
    return m_pReal6->SetBackgroundProcessingMode(mode, action, hEvent, pFurther);
}

// ID3D12Device7
HRESULT STDMETHODCALLTYPE CWrapD3D12Device::AddToStateObject(const D3D12_STATE_OBJECT_DESC* pAdd,
                                                             ID3D12StateObject* pState, REFIID riid, void** ppNew)
{
    if (!m_pReal7) return E_NOINTERFACE;
    return m_pReal7->AddToStateObject(pAdd, pState, riid, ppNew);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12Device::CreateProtectedResourceSession1(
    const D3D12_PROTECTED_RESOURCE_SESSION_DESC1* pDesc, REFIID riid, void** ppSession)
{
    if (!m_pReal7) return E_NOINTERFACE;
    return m_pReal7->CreateProtectedResourceSession1(pDesc, riid, ppSession);
}

// Note: ID3D12Device8-10 methods omitted due to ABI incompatibility with MinGW/MSYS2
